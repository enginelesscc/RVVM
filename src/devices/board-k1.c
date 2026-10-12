/*
 * Experimental SpacemiT K1 boot compatibility board.
 * SPDX-License-Identifier: MPL-2.0
 *
 * Implements the K1 UART, CLINT, PLIC, timer and optional low RAM layout. PCIe ECAM,
 * framebuffer, I2C, RTC and reset are explicitly virtual RVVM devices.
 * This is not an implementation of the K1 boot ROM or vendor firmware.
 */
#include <devices/i2c-oc.h>
#include <rvvm/rvvm.h>
#include <rvvm/rvvm_board.h>
#include <rvvm/rvvm_fdt.h>
#include <util/utils.h>

RVVM_PUBLIC bool rvvm_board_spacemit_k1_init(rvvm_machine_t* machine, bool highmem)
{
    if (!machine || rvvm_get_opt(machine, RVVM_OPT_MEM_SIZE) > (highmem ? 0x40000000ULL : 0x80000000ULL)
        || rvvm_get_opt(machine, RVVM_OPT_CPU_COUNT) > 8) {
        rvvm_error("K1 profile supports 8 harts and up to 2 GiB RAM (1 GiB with -k1-highmem)");
        return false;
    }
    if (!rvvm_set_opt(machine, RVVM_OPT_MEM_BASE, highmem ? 0x80000000UL : 0)) {
        return false;
    }
    rvvm_set_opt(machine, RVVM_OPT_RESET_PC, highmem ? 0x80000000UL : 0);
    rvvm_set_opt(machine, RVVM_OPT_TIME_FREQ, 24000000);

    rvvm_irq_dev_t* plic = rvvm_riscv_plic_init_sized(machine, 0xE0000000UL, 159);
    if (!plic || !rvvm_riscv_clint_init(machine, 0xE4000000UL)) {
        return false;
    }
    /* Virtual expansion devices live above low RAM, outside K1 core MMIO. */
    if (!rvvm_pci_ecam_init(machine, 0, highmem ? 0x30000000UL : 0x80000000UL, plic, NULL, 0xF0000000UL,
                            highmem ? 0x40000000UL : 0x90000000UL, 0x20000000UL)
        || !rvvm_syscon_init(machine, 0xF1000000UL) || !rvvm_rtc_goldfish_init(machine, 0xF1001000UL, plic, 0)
        || !i2c_oc_init(machine, 0xF1002000UL, plic, rvvm_irq_alloc(plic, 0))) {
        return false;
    }

    rvvm_fdt_node_t* root = rvvm_get_fdt_root(machine);
    rvvm_fdt_node_t* soc  = rvvm_get_fdt_soc(machine);
    rvvm_fdt_node_t* cpus = rvvm_fdt_find(root, "cpus");
    rvvm_fdt_prop_set_str(root, "model", "RVVM experimental SpacemiT K1 compatibility");
    rvvm_fdt_prop_set_str_list(root, "compatible", "lekkit,rvvm-k1\0spacemit,k1\0riscv-virtio");
    rvvm_fdt_prop_set_u32(cpus, "timebase-frequency", 24000000);
    rvvm_fdt_prop_set_str_list(rvvm_fdt_find_reg(soc, "plic", 0xE0000000UL), "compatible",
                               "spacemit,k1-plic\0sifive,plic-1.0.0");
    rvvm_fdt_prop_set_str_list(rvvm_fdt_find_reg(soc, "clint", 0xE4000000UL), "compatible",
                               "spacemit,k1-clint\0sifive,clint0\0riscv,clint0");

    /* K1 topology: two clusters of four. Keep actual emulated ISA features. */
    rvvm_fdt_free(rvvm_fdt_find(cpus, "cpu-map"));
    rvvm_fdt_node_t* map = rvvm_fdt_init("cpu-map");
    rvvm_fdt_attach(cpus, map);
    rvvm_fdt_node_t* cluster = NULL;
    for (uint32_t i = 0; i < rvvm_get_opt(machine, RVVM_OPT_CPU_COUNT); ++i) {
        if (!(i % 4)) {
            cluster = rvvm_fdt_init(i ? "cluster1" : "cluster0");
            rvvm_fdt_attach(map, cluster);
        }
        const char*      names[] = {"core0", "core1", "core2", "core3"};
        rvvm_fdt_node_t* core    = rvvm_fdt_init(names[i % 4]);
        rvvm_fdt_node_t* cpu     = rvvm_fdt_find_reg(cpus, "cpu", i);
        rvvm_fdt_prop_set_u32(core, "cpu", rvvm_fdt_phandle(cpu));
        rvvm_fdt_attach(cluster, core);
    }
    return true;
}
