/*
 * Experimental StarFive JH7110 application-processor boot profile.
 * SPDX-License-Identifier: MPL-2.0
 *
 * Four virtual RV64 application harts; the S7 management core is not modeled.
 * Hart IDs and interrupt contexts remain RVVM's contiguous 0-based layout,
 * described in the FDT. PCI, display, RTC, I2C and reset are virtual devices.
 */
#include <devices/i2c-oc.h>
#include <rvvm/rvvm.h>
#include <rvvm/rvvm_board.h>
#include <rvvm/rvvm_fdt.h>
#include <util/utils.h>

RVVM_PUBLIC bool rvvm_board_starfive_jh7110_init(rvvm_machine_t* machine, bool highmem)
{
    if (!machine || rvvm_get_opt(machine, RVVM_OPT_MEM_SIZE) > (highmem ? 0x40000000ULL : 0x80000000ULL)
        || rvvm_get_opt(machine, RVVM_OPT_CPU_COUNT) > 4) {
        rvvm_error("JH7110 profile supports 4 application harts and up to 2 GiB RAM (1 GiB with -jh7110-highmem)");
        return false;
    }
    if (!rvvm_set_opt(machine, RVVM_OPT_MEM_BASE, highmem ? 0x80000000UL : 0x40000000UL)) {
        return false;
    }
    rvvm_set_opt(machine, RVVM_OPT_RESET_PC, highmem ? 0x80000000UL : 0x40000000UL);
    rvvm_set_opt(machine, RVVM_OPT_TIME_FREQ, 4000000);
    rvvm_irq_dev_t* plic = rvvm_riscv_plic_init_sized(machine, 0x0C000000UL, 136);
    if (!plic || !rvvm_riscv_clint_init(machine, 0x02000000UL)
        || !rvvm_pci_ecam_init(machine, 0, 0x30000000UL, plic, NULL, 0xF0000000UL, 0xC0000000UL, 0x20000000UL)
        || !rvvm_syscon_init(machine, 0xF1000000UL) || !rvvm_rtc_goldfish_init(machine, 0xF1001000UL, plic, 0)
        || !i2c_oc_init(machine, 0xF1002000UL, plic, rvvm_irq_alloc(plic, 0))) {
        return false;
    }
    rvvm_fdt_node_t* root = rvvm_get_fdt_root(machine);
    rvvm_fdt_node_t* soc  = rvvm_get_fdt_soc(machine);
    rvvm_fdt_node_t* cpus = rvvm_fdt_find(root, "cpus");
    rvvm_fdt_prop_set_str(root, "model", "RVVM experimental StarFive JH7110 compatibility");
    rvvm_fdt_prop_set_str_list(root, "compatible", "lekkit,rvvm-jh7110\0starfive,jh7110\0riscv-virtio");
    rvvm_fdt_prop_set_u32(cpus, "timebase-frequency", 4000000);
    rvvm_fdt_prop_set_str_list(rvvm_fdt_find_reg(soc, "plic", 0x0C000000UL), "compatible",
                               "starfive,jh7110-plic\0sifive,plic-1.0.0");
    rvvm_fdt_prop_set_str_list(rvvm_fdt_find_reg(soc, "clint", 0x02000000UL), "compatible",
                               "starfive,jh7110-clint\0sifive,clint0\0riscv,clint0");
    return true;
}
