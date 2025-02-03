/*
ata.c - IDE/ATA disk controller
Copyright (C) 2021  cerg2010cerg2010 <github.com/cerg2010cerg2010>
                    LekKit <github.com/LekKit>

This Source Code Form is subject to the terms of the Mozilla Public
License, v. 2.0. If a copy of the MPL was not distributed with this
file, You can obtain one at https://mozilla.org/MPL/2.0/.
*/

#include "ata.h"
#include "blk_io.h"
#include "bit_ops.h"
#include "mem_ops.h"
#include "spinlock.h"
#include "utils.h"
#include "threading.h"
#include "fdtlib.h"

/*
 * Useful resources:
 * - https://wiki.osdev.org/ATA_PIO_Mode
 * - https://wiki.osdev.org/ATA/ATAPI_using_DMA
 * - https://wiki.osdev.org/ATA_Command_Matrix
 * - https://www.manualslib.com/manual/574153/Hitachi-Hts548040m9at00.html?page=164#manual
 */

// Data registers
#define ATA_REG_DATA    0x00 // PIO Data Register
#define ATA_REG_ERROR   0x01 // Error Register (RO)
#define ATA_REG_NSECT   0x02 // Number of sectors to read/write (0 means 256)
#define ATA_REG_LBAL    0x03 // LBAlow
#define ATA_REG_LBAM    0x04 // LBAmid
#define ATA_REG_LBAH    0x05 // LBAhigh
#define ATA_REG_DEVICE  0x06 // Device control register
#define ATA_REG_STATUS  0x07 // Status (RO)
#define ATA_REG_COMMAND 0x07 // Command (WO)

// Control registers
#define ATA_REG_ALT_STATUS 0x00 // Alternate Status (RO)
#define ATA_REG_CONTROL    0x00 // Control (WO)

// Slave drive selection bit (Slave drives not supported!)
#define ATA_DEVICE_SLAVE 0x10

// Error flags for ERR register
#define ATA_ERROR_AMNF  0x01 // Address mark not found (Actually used to detect drive after soft reset)
#define ATA_ERROR_ABRT  0x04 // Aborted command
#define ATA_ERROR_UNC   0x40 // Uncorrectable data error

// Flags for STATUS register
#define ATA_STATUS_ERR  0x01 // Error occured
#define ATA_STATUS_DRQ  0x08 // Data ready
#define ATA_STATUS_SRV  0x10 // Overlapped Service Request
#define ATA_STATUS_RDY  0x40 // Always set, except after an error

// Commands
#define ATA_CMD_NOP               0x00 // No operation
#define ATA_CMD_READ_SECTORS      0x20 // Read Sectors (PIO)
#define ATA_CMD_WRITE_SECTORS     0x30 // Write Sectors (PIO)
#define ATA_CMD_INIT_DEV_PARAMS   0x91 // Set CHS addressing options, CHS not supported!
#define ATA_CMD_READ_DMA          0xC8 // Prepare to read DMA (Actually just seek)
#define ATA_CMD_WRITE_DMA         0xCA // Prepare to write DMA (Actually just seek)
#define ATA_CMD_CHECK_POWER_MODE  0xE4 // Check power mode
#define ATA_CMD_IDENTIFY          0xEC // Identify drive

// ATA BMDMA registers
#define ATA_BMDMA_COMMAND 0x0
#define ATA_BMDMA_STATUS  0x2
#define ATA_BMDMA_PRDT    0x4

// ATA BMDMA flags
#define ATA_BMDMA_COMMAND_DMA  0x01 // Enable DMA mode
#define ATA_BMDMA_COMMAND_READ 0x08 // Perform DMA read

#define ATA_BMDMA_STATUS_IRQ 0x04 // DMA mode exited
#define ATA_BMDMA_STATUS_ERR 0x02 // DMA operation failed
#define ATA_BMDMA_STATUS_DMA 0x01 // DMA mode enabled

#define ATA_SECTOR_SHIFT 9
#define ATA_SECTOR_SIZE  512

typedef struct {
    pci_func_t* pci_func;
    blkdev_t*   blk;
    spinlock_t  lock;
    spinlock_t  dma_lock;

    // Current LBA
    uint64_t lba;

    // ATA BMDMA
    uint32_t prdt_addr;
    uint32_t bmdma_command;
    uint32_t bmdma_status;

    // ATA generic
    uint16_t bytes_to_rw;
    uint16_t sectcount;
    uint8_t drive;
    uint8_t error;
    uint8_t status;

    uint8_t buf[ATA_SECTOR_SIZE];
    char serial[16];
} ata_dev_t;

static inline bool ata_drive_valid(ata_dev_t* ata)
{
    return !(ata->drive & ATA_DEVICE_SLAVE) && ata->blk;
}

static inline void ata_send_interrupt(ata_dev_t* ata) {
    pci_send_irq(ata->pci_func, 0);
}

static inline void ata_report_error(ata_dev_t* ata, uint8_t error)
{
    // No interrupt on error
    ata->error |= error;
    ata->status = ATA_STATUS_ERR;
}

static inline uint64_t ata_get_seek(ata_dev_t* ata)
{
    return ata->lba << ATA_SECTOR_SHIFT;
}

static void ata_copy_id_string(uint8_t* buf, const char* str, uint64_t size)
{
    // Reverse each byte pair since they are little-endian words, pad with spaces
    uint64_t len = rvvm_strnlen(str, size);
    memset(buf, ' ', size);
    for (uint64_t i = 0; i < len; ++i) {
        buf[i ^ 1ULL] = str[i];
    }
}

static void ata_cmd_identify(ata_dev_t* ata)
{
    uint8_t* id_buf = ata->buf;
    memset(id_buf, 0, ATA_SECTOR_SIZE);

    write_uint16_le(id_buf,       0x40);   // Non-removable, ATA device
    write_uint16_le(id_buf + 2,   0xFFFF); // Logical cylinders
    write_uint16_le(id_buf + 6,   0x10);   // Sectors per track
    write_uint16_le(id_buf + 12,  0x3F);   // Logical heads
    write_uint16_le(id_buf + 44,  0x4);    // Number of bytes available in READ/WRITE LONG cmds
    write_uint16_le(id_buf + 98,  0x300);  // Capabilities - LBA supported, DMA supported
    write_uint16_le(id_buf + 100, 0x4000); // Capabilities - bit 14 needs to be set as required by ATA/ATAPI-5 spec
    write_uint16_le(id_buf + 102, 0x400);  // PIO data transfer cycle timing mode
    write_uint16_le(id_buf + 106, 0x7);    // Fields 54-58, 64-70 and 88 are valid
    write_uint16_le(id_buf + 108, 0xFFFF); // Logical cylinders
    write_uint16_le(id_buf + 110, 0x10);   // Logical heads
    write_uint16_le(id_buf + 112, 0x3F);   // Sectors per track

    // Capacity in sectors
    write_uint16_le(id_buf + 114, blk_getsize(ata->blk) >> 9);
    write_uint16_le(id_buf + 116, blk_getsize(ata->blk) >> 25);
    write_uint16_le(id_buf + 120, blk_getsize(ata->blk) >> 9);
    write_uint16_le(id_buf + 122, blk_getsize(ata->blk) >> 25);

    write_uint16_le(id_buf + 128, 0x3);    // Advanced PIO modes supported
    write_uint16_le(id_buf + 134, 0x1);    // PIO transfer cycle time without flow control
    write_uint16_le(id_buf + 136, 0x1);    // PIO transfer cycle time with IORDY flow control
    write_uint16_le(id_buf + 160, 0x100);  // ATA major version
    write_uint16_le(id_buf + 176, 0x80FF); // UDMA mode 7 active, All UDMA modes supported

    // Serial Number
    ata_copy_id_string(id_buf + 20, ata->serial, 20);
    // Firmware Revision
    ata_copy_id_string(id_buf + 46, "R1847", 8);
    // Model Number
    ata_copy_id_string(id_buf + 54, "SATA HDD", 40);

    ata->bytes_to_rw = ATA_SECTOR_SIZE;
    ata->status = ATA_STATUS_RDY | ATA_STATUS_SRV | ATA_STATUS_DRQ;
    ata->sectcount = 1;
    ata_send_interrupt(ata);
}

static void ata_read_sector(ata_dev_t* ata)
{
    if (blk_read(ata->blk, ata->buf, ATA_SECTOR_SIZE, BLKDEV_CUR)) {
        ata->bytes_to_rw = ATA_SECTOR_SIZE;
        ata->status = ATA_STATUS_RDY | ATA_STATUS_DRQ;
        ata_send_interrupt(ata);
    } else {
        // IO failed
        ata_report_error(ata, ATA_ERROR_UNC);
    }
}

static void ata_write_sector(ata_dev_t* ata)
{
    if (blk_write(ata->blk, ata->buf, ATA_SECTOR_SIZE, BLKDEV_CUR)) {
        ata_send_interrupt(ata);
    } else {
        // IO failed
        ata_report_error(ata, ATA_ERROR_UNC);
    }
}

// Perform PIO read/write sectors
static void ata_perform_pio_rw(ata_dev_t* ata, bool write)
{
    if (blk_seek(ata->blk, ata_get_seek(ata), BLKDEV_SEEK_SET)) {
        if (write) {
            ata_write_sector(ata);
        } else {
            ata_read_sector(ata);
        }
    } else {
        // Seek failed
        ata_report_error(ata, ATA_ERROR_UNC);
    }
}

static void ata_handle_cmd(ata_dev_t* ata, uint8_t cmd)
{
    switch (cmd) {
        case ATA_CMD_READ_SECTORS:
        case ATA_CMD_WRITE_SECTORS:
            // Perform PIO IO
            ata_perform_pio_rw(ata, cmd == ATA_CMD_WRITE_SECTORS);
            return;
        case ATA_CMD_READ_DMA:
        case ATA_CMD_WRITE_DMA:
            // Perform a seek without doing any IO yet
            if (blk_seek(ata->blk, ata_get_seek(ata), BLKDEV_SEEK_SET)) {
                ata_send_interrupt(ata);
            } else {
                // Seek failed
                ata_report_error(ata, ATA_ERROR_UNC);
            }
            return;
        case ATA_CMD_CHECK_POWER_MODE:
            // Always active
            ata->sectcount = 0xFF;
            ata_send_interrupt(ata);
            return;
        case ATA_CMD_IDENTIFY:
            // Report drive information
            ata_cmd_identify(ata);
            return;
        default:
            // Ignore
            rvvm_debug("Unknown ATA command 0x%02x", cmd);
            ata_send_interrupt(ata);
            return;
    }
}

static bool ata_data_read(rvvm_mmio_dev_t* dev, void* data, uint64_t offset, uint8_t size)
{
    ata_dev_t* ata = dev->data;
    memset(data, 0, size);

    spin_lock(&ata->lock);
    switch (offset) {
        case ATA_REG_DATA:
            if (ata_drive_valid(ata)) {
                if (ata->bytes_to_rw >= size && ata->bytes_to_rw <= ATA_SECTOR_SIZE) {
                    uint8_t* addr = ata->buf + ATA_SECTOR_SIZE - ata->bytes_to_rw;
                    memcpy(data, addr, size);

                    ata->bytes_to_rw -= size;
                    if (ata->bytes_to_rw == 0) {
                        // Finished reading a sector
                        if (--ata->sectcount) {
                            // Read next sector
                            ata_read_sector(ata);
                        } else {
                            ata->status = ATA_STATUS_RDY;
                        }
                    }
                }
            }
            break;
        case ATA_REG_ERROR:
            if (ata_drive_valid(ata)) {
                write_uint8(data, ata->error);
            }
            break;
        case ATA_REG_NSECT:
            write_uint8(data, ata->sectcount);
            break;
        case ATA_REG_LBAL:
            write_uint8(data, ata->lba);
            break;
        case ATA_REG_LBAM:
            write_uint8(data, ata->lba >> 8);
            break;
        case ATA_REG_LBAH:
            write_uint8(data, ata->lba >> 16);
            break;
        case ATA_REG_DEVICE:
            write_uint8(data, ata->drive | 0xE0 | bit_cut(ata->lba, 24, 4));
            break;
        case ATA_REG_STATUS:
            if (ata_drive_valid(ata)) {
                write_uint8(data, ata->status);
            }
            break;
    }
    spin_unlock(&ata->lock);

    return true;
}

static bool ata_data_write(rvvm_mmio_dev_t* dev, void* data, uint64_t offset, uint8_t size)
{
    ata_dev_t* ata = dev->data;

    spin_lock(&ata->lock);
    switch (offset) {
        case ATA_REG_DATA:
            if (ata_drive_valid(ata)) {
                if (ata->bytes_to_rw >= size && ata->bytes_to_rw <= ATA_SECTOR_SIZE) {
                    uint8_t* addr = ata->buf + ATA_SECTOR_SIZE - ata->bytes_to_rw;
                    memcpy(addr, data, size);

                    ata->bytes_to_rw -= size;
                    if (ata->bytes_to_rw == 0) {
                        // Finished writing a sector
                        if (--ata->sectcount) {
                            // Prepare for writing next sector
                            ata->status = ATA_STATUS_RDY | ATA_STATUS_DRQ;
                            ata->bytes_to_rw = ATA_SECTOR_SIZE;
                        }

                        // Store buffered sector
                        ata_write_sector(ata);
                    }
                }
            }
            break;
        case ATA_REG_NSECT:
            ata->sectcount = read_uint8(data);
            if (!ata->sectcount) {
                // Byte 0x00 means 256 sectors
                ata->sectcount = 256;
            }
            break;
        case ATA_REG_LBAL:
            ata->lba = bit_replace(ata->lba, 0,  8, read_uint8(data));
            break;
        case ATA_REG_LBAM:
            ata->lba = bit_replace(ata->lba, 8,  8, read_uint8(data));
            break;
        case ATA_REG_LBAH:
            ata->lba = bit_replace(ata->lba, 16, 8, read_uint8(data));
            break;
        case ATA_REG_DEVICE:
            ata->drive = read_uint8(data) & ATA_DEVICE_SLAVE;
            ata->lba = bit_replace(ata->lba, 24, 4, read_uint8(data) & 0xF);
            break;
        case ATA_REG_COMMAND:
            if (ata_drive_valid(ata)) {
                ata->error = 0;
                ata->status = ATA_STATUS_RDY;
                ata_handle_cmd(ata, read_uint8(data));
            }
            break;
    }
    spin_unlock(&ata->lock);

    return true;
}

static bool ata_ctl_read(rvvm_mmio_dev_t* dev, void* data, uint64_t offset, uint8_t size)
{
    ata_dev_t* ata = dev->data;
    memset(data, 0, size);

    spin_lock(&ata->lock);
    switch (offset) {
        case ATA_REG_ALT_STATUS:
            if (ata_drive_valid(ata)) {
                write_uint8(data, ata->status);
            }
            break;
    }
    spin_unlock(&ata->lock);

    return true;
}

static bool ata_ctl_write(rvvm_mmio_dev_t* dev, void* data, uint64_t offset, uint8_t size)
{
    ata_dev_t* ata = dev->data;
    UNUSED(size);

    spin_lock(&ata->lock);
    switch (offset) {
        case ATA_REG_CONTROL:
            if (ata_drive_valid(ata)) {
                if (bit_check(read_uint8(data), 2)) {
                    // Soft reset
                    ata->bytes_to_rw = 0;
                    ata->lba = 1; // Sectors start from 1
                    ata->sectcount = 1;
                    ata->drive = 0;
                    ata->error = ATA_ERROR_AMNF; // AMNF means OK here...
                    ata->status = ATA_STATUS_RDY | ATA_STATUS_SRV;
                }
            }
            break;
    }
    spin_unlock(&ata->lock);

    return true;
}

static void ata_data_remove(rvvm_mmio_dev_t* dev)
{
    ata_dev_t* ata = dev->data;
    if (ata) {
        spin_lock(&ata->dma_lock);
        blk_close(ata->blk);
        spin_unlock(&ata->dma_lock);
        free(ata);
    }
}

static rvvm_mmio_type_t ata_data_dev_type = {
    .name = "ata_data",
    .remove = ata_data_remove,
};

static void ata_remove_dummy(rvvm_mmio_dev_t* device)
{
    // Dummy remove, cleanup happends in ata_data_remove()
    UNUSED(device);
}

static rvvm_mmio_type_t ata_ctl_dev_type = {
    .name = "ata_ctl",
    .remove = ata_remove_dummy,
};

static ata_dev_t* ata_create(const char* image, bool rw)
{
    blkdev_t* blk = NULL;
    if (image) {
        blk = blk_open(image, rw ? BLKDEV_RW : 0);
        if (!blk) {
            // Failed to open image
            return NULL;
        }
    }
    ata_dev_t* ata = safe_new_obj(ata_dev_t);
    rvvm_randomserial(ata->serial, 12);
    ata->blk = blk;
    return ata;
}

PUBLIC bool ata_pio_init(rvvm_machine_t* machine, rvvm_addr_t ata_data_addr, rvvm_addr_t ata_ctl_addr, const char* image, bool rw)
{
    ata_dev_t* ata = ata_create(image, rw);
    if (!ata) {
        return false;
    }

    rvvm_mmio_dev_t ata_data = {
        .addr = ata_data_addr,
        .size = 0x1000,
        .data = ata,
        .read = ata_data_read,
        .write = ata_data_write,
        .type = &ata_data_dev_type,
        .min_op_size = 1,
        .max_op_size = 4,
    };
    if (!rvvm_attach_mmio(machine, &ata_data)) {
        return false;
    }

    rvvm_mmio_dev_t ata_ctl = {
        .addr = ata_ctl_addr,
        .size = 0x1000,
        .data = ata,
        .read = ata_ctl_read,
        .write = ata_ctl_write,
        .type = &ata_ctl_dev_type,
        .min_op_size = 1,
        .max_op_size = 1,
    };
    if (!rvvm_attach_mmio(machine, &ata_ctl)) {
        return false;
    }

#ifdef USE_FDT
    uint32_t reg_cells[8] = {
        ata_data_addr >> 32,
        ata_data_addr,
        0x0,
        0x1000,
        ata_ctl_addr >> 32,
        ata_ctl_addr,
        0x0,
        0x1000,
    };

    struct fdt_node* ata_fdt = fdt_node_create_reg("ata", ata_data_addr);
    fdt_node_add_prop_cells(ata_fdt, "reg", reg_cells, STATIC_ARRAY_SIZE(reg_cells));
    fdt_node_add_prop_str(ata_fdt, "compatible", "ata-generic");
    fdt_node_add_prop_u32(ata_fdt, "reg-shift", 0);
    fdt_node_add_prop_u32(ata_fdt, "pio-mode", 4);
    fdt_node_add_child(rvvm_get_fdt_soc(machine), ata_fdt);
#endif

    return true;
}

PUBLIC bool ata_pio_init_auto(rvvm_machine_t* machine, const char* image, bool rw)
{
    rvvm_addr_t addr = rvvm_mmio_zone_auto(machine, ATA_DATA_ADDR_DEFAULT, 0x2000);
    return ata_pio_init(machine, addr, addr + 0x1000, image, rw);
}

static rvvm_mmio_type_t ata_bmdma_dev_type = {
    .name = "ata_bmdma",
    .remove = ata_remove_dummy,
};

static void ata_process_prdt(ata_dev_t* ata)
{
    rvvm_addr_t prdt_addr = atomic_load_uint32(&ata->prdt_addr);
    bool is_read = !!(atomic_load_uint32(&ata->bmdma_command) & ATA_BMDMA_COMMAND_READ);
    uint64_t to_process = ata->sectcount << ATA_SECTOR_SHIFT;
    uint64_t processed = 0;

    // According to spec, maximum amount of PRDT entries is 64k
    // This should prevent malicious guests from hanging up the thread
    for (uint64_t i = 0; i < 0x10000; ++i) {
        // Read PRD
        const uint8_t* prd = pci_get_dma_ptr(ata->pci_func, prdt_addr, 8);
        if (!prd) {
            // DMA error
            break;
        }
        uint32_t prd_physaddr = read_uint32_le_m(prd);
        uint32_t prd_sectcount = read_uint32_le_m(prd + 4);

        uint32_t buf_size = prd_sectcount & 0xFFFF;
        if (buf_size == 0) {
            // Value 0 means 64K
            buf_size = 0x10000;
        }

        void* buffer = pci_get_dma_ptr(ata->pci_func, prd_physaddr, buf_size);
        if (!buffer) {
            // DMA error
            break;
        }

        // Read/write data to/from RAM
        if (is_read) {
            if (blk_read(ata->blk, buffer, buf_size, BLKDEV_CUR) != buf_size) {
                // IO error
                break;
            }
        } else {
            if (blk_write(ata->blk, buffer, buf_size, BLKDEV_CUR) != buf_size) {
                // IO error
                break;
            }
        }

        processed += buf_size;

        if (bit_check(prd_sectcount, 31)) {
            // This is the last PRD
            break;
        }

        // All good, advance the pointer
        prdt_addr += 8;
    }

    atomic_store_uint32(&ata->bmdma_command, 0);

    if (processed == to_process) {
        // Everything OK
        atomic_store_uint32(&ata->bmdma_status, ATA_BMDMA_STATUS_IRQ);
    } else {
        // Error
        atomic_store_uint32(&ata->bmdma_status, ATA_BMDMA_STATUS_IRQ | ATA_BMDMA_STATUS_ERR);
    }

    ata_send_interrupt(ata);
}

static void* ata_prdt_io_worker(void* arg)
{
    ata_dev_t* ata = arg;
    spin_lock(&ata->dma_lock);
    ata_process_prdt(ata);
    spin_unlock(&ata->dma_lock);
    return NULL;
}

static bool ata_pci_bmdma_read(rvvm_mmio_dev_t* dev, void* data, uint64_t offset, uint8_t size)
{
    ata_dev_t* ata = dev->data;

    switch (offset) {
        case ATA_BMDMA_COMMAND:
            write_uint8(data, atomic_load_uint32_relax(&ata->bmdma_command));
            break;
        case ATA_BMDMA_STATUS:
            write_uint8(data, atomic_load_uint32_relax(&ata->bmdma_status) | ((ata->blk != NULL) << 5));
            break;
        case ATA_BMDMA_PRDT: {
            uint8_t buffer[4] = {0};
            write_uint32_le(buffer, atomic_load_uint32_relax(&ata->prdt_addr));
            memcpy(data, buffer, size);
            break;
        }
    }

    return true;
}

static bool ata_pci_bmdma_write(rvvm_mmio_dev_t* dev, void* data, uint64_t offset, uint8_t size)
{
    ata_dev_t* ata = dev->data;

    switch (offset) {
        case ATA_BMDMA_COMMAND: {
            uint8_t cmd = read_uint8(data);
            atomic_and_uint32(&ata->bmdma_command, ATA_BMDMA_COMMAND_DMA);
            uint8_t prev_cmd = atomic_or_uint32(&ata->bmdma_command, cmd);
            if (!!(cmd & ATA_BMDMA_COMMAND_DMA) && !(prev_cmd & ATA_BMDMA_COMMAND_DMA)) {
                thread_create_task(ata_prdt_io_worker, ata);
            }
            break;
        }
        case ATA_BMDMA_STATUS:
            atomic_and_uint32(&ata->bmdma_status, ~(read_uint8(data) & (ATA_BMDMA_STATUS_DMA | ATA_BMDMA_STATUS_IRQ)));
            break;
        case ATA_BMDMA_PRDT: {
            uint8_t buffer[4] = {0};
            memcpy(buffer, data, size);
            atomic_store_uint32_relax(&ata->prdt_addr, read_uint32_le(buffer));
            break;
        }
    }

    return true;
}

static bool ata_pci_ctl_read(rvvm_mmio_dev_t* dev, void* data, uint64_t offset, uint8_t size)
{
    return ata_ctl_read(dev, data, offset - 2, size);
}

static bool ata_pci_ctl_write(rvvm_mmio_dev_t* dev, void* data, uint64_t offset, uint8_t size)
{
    return ata_ctl_write(dev, data, offset - 2, size);
}

PUBLIC pci_dev_t* ata_pci_init(pci_bus_t* pci_bus, const char* image, bool rw)
{
    ata_dev_t* ata = ata_create(image, rw);
    if (ata == NULL) {
        return NULL;
    }

    ata_dev_t* ata_secondary = ata_create(NULL, false);

    pci_func_desc_t ata_desc = {
        .vendor_id = 0x1179,  // Toshiba
        .device_id = 0x0102,  // Extended IDE Controller
        .class_code = 0x0101, // Mass Storage, IDE
        .prog_if = 0x85,      // PCI native mode-only controller, supports bus mastering
        .irq_pin = PCI_IRQ_PIN_INTA,

        // Primary channel data/ctl BARs
        .bar[0] = {
            .size = 0x1000,
            .min_op_size = 1,
            .max_op_size = 4,
            .read = ata_data_read,
            .write = ata_data_write,
            .data = ata,
            .type = &ata_data_dev_type,
        },
        .bar[1] = {
            .size = 0x1000,
            .min_op_size = 1,
            .max_op_size = 1,
            .read = ata_pci_ctl_read,
            .write = ata_pci_ctl_write,
            .data = ata,
            .type = &ata_ctl_dev_type,
        },

        // Dummy secondary channel data/ctl BARs
        .bar[2] = {
            .size = 0x1000,
            .min_op_size = 1,
            .max_op_size = 4,
            .read = ata_data_read,
            .write = ata_data_write,
            .data = ata_secondary,
            .type = &ata_data_dev_type,
        },
        .bar[3] = {
            .size = 0x1000,
            .min_op_size = 1,
            .max_op_size = 1,
            .read = ata_pci_ctl_read,
            .write = ata_pci_ctl_write,
            .data = ata_secondary,
            .type = &ata_ctl_dev_type,
        },

        // ATA BMDMA
        .bar[4] = {
            .size = 0x1000,
            .min_op_size = 1,
            .max_op_size = 4,
            .read = ata_pci_bmdma_read,
            .write = ata_pci_bmdma_write,
            .data = ata,
            .type = &ata_bmdma_dev_type,
        },
    };

    pci_dev_t* pci_dev = pci_attach_func(pci_bus, &ata_desc);
    if (pci_dev) {
        // Successfully plugged in
        ata->pci_func = pci_get_device_func(pci_dev, 0);
    }
    return pci_dev;
}

PUBLIC pci_dev_t* ata_pci_init_auto(rvvm_machine_t* machine, const char* image, bool rw)
{
    pci_bus_t* pci_bus = rvvm_get_pci_bus(machine);
    if (pci_bus) {
        return ata_pci_init(pci_bus, image, rw);
    }
    return NULL;
}

PUBLIC bool ata_init_auto(rvvm_machine_t* machine, const char* image, bool rw)
{
    return ata_pci_init_auto(machine, image, rw) || ata_pio_init_auto(machine, image, rw);
}
