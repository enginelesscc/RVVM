/*
pci-vfio.c - VFIO PCI Passthrough
Copyright (C) 2022  LekKit <github.com/LekKit>

This Source Code Form is subject to the terms of the Mozilla Public
License, v. 2.0. If a copy of the MPL was not distributed with this
file, You can obtain one at https://mozilla.org/MPL/2.0/.
*/

// Needed for pread()/pwrite()/readlink(), O_CLOEXEC when not passing -std=gnu..
#define _GNU_SOURCE
#define _BSD_SOURCE
#define _DEFAULT_SOURCE

#include "pci-vfio.h"
#include "compiler.h"
#include "threading.h"
#include "mem_ops.h"
#include "utils.h"

// Check that <linux/vfio.h> include is available
#if defined(__linux__) && defined(USE_VFIO) && !CHECK_INCLUDE(linux/vfio.h, 0)
#warning Disabling USE_VFIO as <linux/vfio.h> is unavailable
#undef USE_VFIO
#endif

#if defined(__linux__) && defined(USE_VFIO)

#include <string.h>
#include <errno.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/types.h>
#include <sys/eventfd.h>

#include <linux/vfio.h>

static uint64_t vfio_pci_sysfs_path(char* buffer, uint64_t size, const char* pci_id, const char* suffix)
{
    uint64_t len = rvvm_strlcpy(buffer, "/sys/bus/pci/devices/", size);
    len += rvvm_strlcpy(buffer + len, pci_id, size - len);
    return len + rvvm_strlcpy(buffer + len, suffix, size - len);
}

static uint64_t vfio_rw_file(const char* path, void* rdbuf, const void* wrbuf, uint64_t size)
{
    int fd = open(path, (rdbuf ? O_RDONLY : 0) | (wrbuf ? O_WRONLY : 0) | O_CLOEXEC);
    uint64_t ret = fd >= 0;
    if (wrbuf && ret) ret = (uint64_t)write(fd, wrbuf, size) == size;
    if (rdbuf && ret) ret = read(fd, rdbuf, size);
    if (fd >= 0) close(fd);
    return ret;
}

static bool vfio_unbind_driver(const char* pci_id)
{
    char path[256] = {0};
    vfio_pci_sysfs_path(path, sizeof(path), pci_id, "/driver/unbind");
    return vfio_rw_file(path, NULL, pci_id, rvvm_strlen(pci_id));
}

static bool vfio_bind_vfio(const char* pci_id)
{
    char path[256] = {0};
    char ven_dev[256] = {0};
    vfio_pci_sysfs_path(path, sizeof(path), pci_id, "/vendor");
    uint64_t len = vfio_rw_file(path, ven_dev, NULL, sizeof(ven_dev));
    len += rvvm_strlcpy(ven_dev + len, " ", sizeof(ven_dev) - len);
    vfio_pci_sysfs_path(path, sizeof(path), pci_id, "/device");
    len += vfio_rw_file(path, ven_dev + len, NULL, sizeof(ven_dev) - len);
    vfio_rw_file("/sys/bus/pci/drivers/vfio-pci/new_id", NULL, ven_dev, len);
    return vfio_rw_file("/sys/bus/pci/drivers/vfio-pci/bind", NULL, pci_id, rvvm_strlen(pci_id));
}

static bool vfio_needs_rebind(const char* pci_id)
{
    char path[256] = {0};
    char driver_path[256] = {0};
    vfio_pci_sysfs_path(path, sizeof(path), pci_id, "/driver");
    if (readlink(path, driver_path, sizeof(driver_path)) < 0) return true;
    return !rvvm_strfind(driver_path, "vfio-pci");
}

static bool vfio_bind(const char* pci_id)
{
    if (vfio_needs_rebind(pci_id)) {
        rvvm_info("Unbinding the device from it's original driver");
        vfio_unbind_driver(pci_id);
        vfio_bind_vfio(pci_id);
    }
    rvvm_info("Host PCI device %s should now be bound to vfio-pci", pci_id);
    return !vfio_needs_rebind(pci_id);
}

static uint32_t vfio_get_iommu_group(const char* pci_id)
{
    char path[256] = {0};
    char group_path[256] = {0};
    vfio_pci_sysfs_path(path, sizeof(path), pci_id, "/iommu_group");
    if (readlink(path, group_path, sizeof(group_path)) < 0) return -1;
    const char* iommu_path = rvvm_strfind(group_path, "/kernel/iommu_groups/");
    if (iommu_path) {
        return str_to_int_dec(iommu_path + rvvm_strlen("/kernel/iommu_groups/"));
    }
    rvvm_error("Invalid VFIO IOMMU group path!");
    return -1;
}

static int vfio_open_group(const char* pci_id)
{
    uint32_t group = vfio_get_iommu_group(pci_id);
    char path[256] = "/dev/vfio/";
    uint64_t len = rvvm_strlen(path);
    int_to_str_dec(path + len, sizeof(path) - len, group);
    int fd = open(path, O_RDWR | O_CLOEXEC);
    return fd;
}

typedef struct {
    pci_func_desc_t func_desc;
    pci_func_t*     pci_func;
    thread_ctx_t*  thread;
    int container;
    int group;
    int device;
    int eventfd;
    int irqtype;
    bool running;
} vfio_dev_t;

static void vfio_unmask_irq(vfio_dev_t* vfio)
{
    struct vfio_irq_set irq_set = {
        .argsz = sizeof(irq_set),
        .flags = VFIO_IRQ_SET_DATA_NONE | VFIO_IRQ_SET_ACTION_UNMASK,
        .index = vfio->irqtype,
        .start = 0,
        .count = 1,
    };
    ioctl(vfio->device, VFIO_DEVICE_SET_IRQS, &irq_set);
}

static void vfio_trigger_irq(vfio_dev_t* vfio)
{
    struct vfio_irq_set irq_set = {
        .argsz = sizeof(irq_set),
        .flags = VFIO_IRQ_SET_DATA_NONE | VFIO_IRQ_SET_ACTION_TRIGGER,
        .index = vfio->irqtype,
        .start = 0,
        .count = 1,
    };
    if (vfio->device > 0) ioctl(vfio->device, VFIO_DEVICE_SET_IRQS, &irq_set);
}

static void* vfio_irq_thread(void* data)
{
    vfio_dev_t* vfio = data;
    uint8_t buffer[8] = {0};
    vfio_unmask_irq(vfio);
    while (vfio->running) {
        UNUSED(!read(vfio->eventfd, buffer, sizeof(buffer)));
        pci_send_irq(vfio->pci_func, 0);
    }
    return NULL;
}

static void vfio_bar_remove(rvvm_mmio_dev_t* dev)
{
    UNUSED(dev);
}

static rvvm_mmio_type_t vfio_bar_type = {
    .name = "vfio_bar",
    .remove = vfio_bar_remove,
};

static void vfio_dev_free(vfio_dev_t* vfio)
{
    for (uint64_t i = 0; i < PCI_FUNC_BARS; ++i) {
        void*  bar_ptr  = vfio->func_desc.bar[i].mapping;
        uint64_t bar_size = vfio->func_desc.bar[i].size;
        if (bar_ptr && bar_size) {
            munmap(bar_ptr, bar_size);
        }
    }
    if (vfio->func_desc.expansion_rom.mapping && vfio->func_desc.expansion_rom.size) {
        munmap(vfio->func_desc.expansion_rom.mapping, vfio->func_desc.expansion_rom.size);
    }
    vfio->running = false;
    vfio_trigger_irq(vfio);
    thread_join(vfio->thread);
    if (vfio->eventfd > 0) {
        close(vfio->eventfd);
    }
    if (vfio->device > 0) {
        close(vfio->device);
    }
    if (vfio->group > 0) {
        close(vfio->group);
    }
    if (vfio->container > 0) {
        close(vfio->container);
    }
    free(vfio);
}

static void vfio_dev_remove(rvvm_mmio_dev_t* dev)
{
    vfio_dev_t* vfio = dev->data;
    vfio_dev_free(vfio);
}

static rvvm_mmio_type_t vfio_dev_type = {
    .name = "vfio_pci_dev",
    .remove = vfio_dev_remove,
};

static bool vfio_map_dma(vfio_dev_t* vfio, rvvm_machine_t* machine, rvvm_addr_t mem_base, uint64_t mem_size)
{
    struct vfio_iommu_type1_dma_map dma_map = {
        .argsz = sizeof(struct vfio_iommu_type1_dma_map),
        .flags = VFIO_DMA_MAP_FLAG_READ | VFIO_DMA_MAP_FLAG_WRITE,
        .iova = mem_base,
        .size = mem_size,
        .vaddr = (uint64_t)rvvm_get_dma_ptr(machine, mem_base, mem_size),
    };
    return ioctl(vfio->container, VFIO_IOMMU_MAP_DMA, &dma_map) == 0;
}

static bool vfio_setup_device_irqs(vfio_dev_t* vfio, uint32_t irqtype)
{
    struct vfio_irq_info irq_info = { .argsz = sizeof(struct vfio_irq_info), .index = irqtype, };
    if (ioctl(vfio->device, VFIO_DEVICE_GET_IRQ_INFO, &irq_info)) {
        return false;
    }

    if (!(irq_info.flags & VFIO_IRQ_INFO_EVENTFD)) {
        return false;
    }

    if (vfio->eventfd <= 0) {
        // Set up IRQ eventfd
        vfio->eventfd = eventfd(0, 0);
        if (vfio->eventfd < 0) {
            rvvm_error("Failed to create VFIO IRQ eventfd: %s", strerror(errno));
            return false;
        }
    }

    uint64_t irq_size = sizeof(struct vfio_irq_set) + sizeof(int);
    struct vfio_irq_set* irq_set = safe_calloc(irq_size, 1);
    irq_set->argsz = irq_size;
    irq_set->flags = VFIO_IRQ_SET_DATA_EVENTFD | VFIO_IRQ_SET_ACTION_TRIGGER;
    irq_set->index = irqtype;
    irq_set->start = 0;
    irq_set->count = 1;

    // For MSI IRQ vectors, this is an array of eventfds
    *((int*)irq_set->data) = vfio->eventfd;

    if (ioctl(vfio->device, VFIO_DEVICE_SET_IRQS, irq_set)) {
        free(irq_set);
        return false;
    }

    vfio->irqtype = irqtype;
    return true;
}

static bool vfio_try_attach(vfio_dev_t* vfio, rvvm_machine_t* machine, const char* pci_id)
{
    vfio->container = open("/dev/vfio/vfio", O_RDWR | O_CLOEXEC);
    if (vfio->container == -1) {
        rvvm_error("Could not open /dev/vfio/vfio: %s", strerror(errno));
        return false;
    }
    vfio->group = vfio_open_group(pci_id);
    if (vfio->group == -1) {
        rvvm_error("Failed to open VFIO group: %s", strerror(errno));
        return false;
    }
    struct vfio_group_status group_status = { .argsz = sizeof(struct vfio_group_status), };
    if (ioctl(vfio->group, VFIO_GROUP_GET_STATUS, &group_status) || !(group_status.flags & VFIO_GROUP_FLAGS_VIABLE)) {
        rvvm_error("VFIO group not viable, are all group devices attached to vfio_pci module?");
        return false;
    }
    if (ioctl(vfio->group, VFIO_GROUP_SET_CONTAINER, &vfio->container)) {
        rvvm_error("Failed to set VFIO container group: %s", strerror(errno));
        return false;
    }
    if (ioctl(vfio->container, VFIO_SET_IOMMU, VFIO_TYPE1_IOMMU)) {
        rvvm_error("Failed to set up VFIO IOMMU: %s", strerror(errno));
        return false;
    }

    // Set up DMA to guest RAM
    rvvm_addr_t mem_base = rvvm_get_opt(machine, RVVM_OPT_MEM_BASE);
    rvvm_addr_t mem_size = rvvm_get_opt(machine, RVVM_OPT_MEM_SIZE);
    if (!vfio_map_dma(vfio, machine, mem_base, mem_size)) {
        // This *kinda* works around a DMA conflict with x86 MSI IRQ vector reserved region
        // More info: https://lore.kernel.org/linux-iommu/20191211082304.2d4fab45@x1.home/
        // cat /sys/kernel/iommu_groups/[iommu group]/reserved_regions

        // LAPIC MSI registers are usually placed on address 0xFEE00000, I/O APIC on address 0xFEС00000
        const rvvm_addr_t msi_x86_low = 0xFEC00000;
        const rvvm_addr_t msi_x86_end = 0xFEF00000;
        rvvm_info("Workaround reserved x86 MSI IRQ vector by splitting DMA region");
        if (mem_base < msi_x86_low) {
            uint64_t low_size = EVAL_MIN(mem_size, msi_x86_low - mem_base);
            if (!vfio_map_dma(vfio, machine, mem_base, low_size)) {
                rvvm_error("Failed to set up VFIO DMA: %s", strerror(errno));
                rvvm_error("This is likely caused by reserved mappings on your host overlapping guest RAM");
                return false;
            }
        }
        if (mem_base + mem_size > msi_x86_end) {
            uint64_t high_size = (mem_base + mem_size) - msi_x86_end;
            if (!vfio_map_dma(vfio, machine, msi_x86_end, high_size)) {
                rvvm_error("Failed to set up VFIO DMA: %s", strerror(errno));
                rvvm_error("This is likely caused by reserved mappings on your host overlapping guest RAM");
                return false;
            }
        }
    }

    vfio->device = ioctl(vfio->group, VFIO_GROUP_GET_DEVICE_FD, pci_id);
    if (vfio->device < 0) {
        rvvm_error("Failed to get VFIO device fd: %s", strerror(errno));
        return false;
    }
    struct vfio_device_info device_info = { .argsz = sizeof(struct vfio_device_info), };
    if (ioctl(vfio->device, VFIO_DEVICE_GET_INFO, &device_info)) {
        rvvm_error("Failed to get VFIO device info: %s", strerror(errno));
        return false;
    }

    // Read PCI config space of the device
    struct vfio_region_info pci_cfg_info = { .argsz = sizeof(struct vfio_region_info), .index = VFIO_PCI_CONFIG_REGION_INDEX, };
    if (ioctl(vfio->device, VFIO_DEVICE_GET_REGION_INFO, &pci_cfg_info)) {
        rvvm_error("Failed to get VFIO PCI config space info: %s", strerror(errno));
        return false;
    }
    uint8_t pci_config[64];
    if (pread(vfio->device, pci_config, 64, pci_cfg_info.offset) != 64) {
        rvvm_error("Failed to read PCI config space: %s", strerror(errno));
        return false;
    }

    vfio->func_desc.vendor_id = read_uint16_le(pci_config);
    vfio->func_desc.device_id = read_uint16_le(pci_config + 0x2);
    vfio->func_desc.class_code = read_uint16_le(pci_config + 0xA);
    vfio->func_desc.prog_if = pci_config[0x9];
    vfio->func_desc.irq_pin = pci_config[0x3D];

    // Enable interrupts, Bus-mastering, MMIO access, Write-inval
    write_uint32_le(pci_config + 4, 0x16);
    if (pwrite(vfio->device, pci_config + 4, 4, pci_cfg_info.offset + 4) != 4) {
        rvvm_error("Failed to write PCI config space: %s", strerror(errno));
        return false;
    }

    // Set up device BAR mappings
    for (uint32_t i = 0; i < device_info.num_regions && i <= VFIO_PCI_ROM_REGION_INDEX; ++i) {
        bool expansion_rom = i == VFIO_PCI_ROM_REGION_INDEX;
        struct vfio_region_info region_info = { .argsz = sizeof(struct vfio_region_info), .index = i, };
        if (ioctl(vfio->device, VFIO_DEVICE_GET_REGION_INFO, &region_info)) {
            rvvm_error("Failed to get VFIO BAR info: %s", strerror(errno));
            return false;
        }
        if (region_info.size && (region_info.flags & VFIO_REGION_INFO_FLAG_MMAP)) {
            if (expansion_rom) {
                void* rom = mmap(NULL, region_info.size, PROT_READ | PROT_WRITE, MAP_PRIVATE, vfio->device, region_info.offset);
                rvvm_info("VFIO Expansion ROM: size 0x%lx, offset 0x%lx, flags 0x%x",
                    (unsigned long)region_info.size,
                    (unsigned long)region_info.offset,
                    (uint32_t)region_info.flags);
                if (rom == MAP_FAILED) {
                    rvvm_error("VFIO Expansion ROM mmap() failed: %s", strerror(errno));
                    return false;
                }
                vfio->func_desc.expansion_rom.mapping = rom;
                vfio->func_desc.expansion_rom.size = region_info.size;
                vfio->func_desc.expansion_rom.min_op_size = 1;
                vfio->func_desc.expansion_rom.max_op_size = 16;
                vfio->func_desc.expansion_rom.type = &vfio_bar_type;
            } else {
                void* bar = mmap(NULL, region_info.size, PROT_READ | PROT_WRITE, MAP_SHARED, vfio->device, region_info.offset);
                rvvm_info("VFIO PCI BAR %d: size 0x%lx, offset 0x%lx, flags 0x%x", i,
                    (unsigned long)region_info.size,
                    (unsigned long)region_info.offset,
                    (uint32_t)region_info.flags);
                if (bar == MAP_FAILED) {
                    rvvm_error("VFIO BAR mmap() failed: %s", strerror(errno));
                    return false;
                }
                vfio->func_desc.bar[i].mapping = bar;
                vfio->func_desc.bar[i].size = region_info.size;
                vfio->func_desc.bar[i].min_op_size = 1;
                vfio->func_desc.bar[i].max_op_size = 16;
                vfio->func_desc.bar[i].type = &vfio_bar_type;
            }
        }
    }

    // Check IRQ capabilities
    if (device_info.num_irqs < VFIO_PCI_MSI_IRQ_INDEX) {
        rvvm_error("No support for VFIO MSI IRQs");
        return false;
    }

    if (!vfio_setup_device_irqs(vfio, VFIO_PCI_MSI_IRQ_INDEX) && !vfio_setup_device_irqs(vfio, VFIO_PCI_MSIX_IRQ_INDEX)) {
        rvvm_error("Failed to setup VFIO IRQs");
        return false;
    }

    // Graceful device reset, all OK
    ioctl(vfio->device, VFIO_DEVICE_RESET);
    return true;
}

PUBLIC bool pci_vfio_init_auto(rvvm_machine_t* machine, const char* pci_id)
{
    pci_bus_t* pci_bus = rvvm_get_pci_bus(machine);
    char long_pci_id[256] = "0000:";
    if (rvvm_strlen(pci_id) < 12) {
        // This is a shorthand pci id, like in lspci (00:01.0) - extend it to sysfs format
        rvvm_strlcpy(long_pci_id + 5, pci_id, sizeof(long_pci_id) - 5);
        pci_id = long_pci_id;
    }

    UNUSED(!system("modprobe vfio_pci")); // Just in case

    if (!vfio_bind(pci_id)) {
        rvvm_error("Failed to bind PCI device to vfio_pci kernel module");
        return false;
    }

    vfio_dev_t* vfio = safe_new_obj(vfio_dev_t);
    if (!vfio_try_attach(vfio, machine, pci_id)) {
        // Failed to attach to the host VFIO device
        vfio_dev_free(vfio);
        return false;
    }

    rvvm_mmio_dev_t vfio_dev_placeholder = {
        .size = 0,
        .type = &vfio_dev_type,
        .data = vfio,
    };

    rvvm_mmio_dev_t* placeholder = rvvm_attach_mmio(machine, &vfio_dev_placeholder);
    if (placeholder) {
        pci_dev_t* pci_dev = pci_attach_func(pci_bus, &vfio->func_desc);
        vfio->pci_func = pci_get_device_func(pci_dev, 0);
        if (vfio->pci_func) {
            vfio->running = true;
            vfio->thread = thread_create(vfio_irq_thread, vfio);
            return true;
        } else {
            // Failed to attach to guest PCI bus
            rvvm_remove_mmio(placeholder);
        }
    }

    return false;
}

#else

PUBLIC bool pci_vfio_init_auto(rvvm_machine_t* machine, const char* pci_id)
{
    rvvm_error("VFIO isn't available");
    UNUSED(machine); UNUSED(pci_id);
    return false;
}

#endif
