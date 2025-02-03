/*
syscon.c - Poweroff/reset syscon device
Copyright (C) 2021  LekKit <github.com/LekKit>

This Source Code Form is subject to the terms of the Mozilla Public
License, v. 2.0. If a copy of the MPL was not distributed with this
file, You can obtain one at https://mozilla.org/MPL/2.0/.
*/

#include "syscon.h"
#include "mem_ops.h"
#include "fdtlib.h"

#define SYSCON_POWEROFF 0x5555
#define SYSCON_RESET    0x7777

static bool syscon_mmio_write(rvvm_mmio_dev_t* dev, void* data, uint64_t offset, uint8_t size)
{
    UNUSED(size);
    if (offset == 0) {
        switch(read_uint16_le_m(data)) {
            case SYSCON_POWEROFF:
            case SYSCON_RESET:
                rvvm_reset_machine(dev->machine, read_uint16_le_m(data) == SYSCON_RESET);
                break;
            default:
                break;
        }
    }
    return true;
}

static rvvm_mmio_type_t syscon_dev_type = {
    .name = "syscon",
};

PUBLIC rvvm_mmio_dev_t* syscon_init(rvvm_machine_t* machine, rvvm_addr_t base_addr)
{
    rvvm_mmio_dev_t syscon = {
        .addr = base_addr,
        .size = 0x1000,
        .read = rvvm_mmio_none,
        .write = syscon_mmio_write,
        .min_op_size = 2,
        .max_op_size = 2,
        .type = &syscon_dev_type,
    };
    rvvm_mmio_dev_t* mmio = rvvm_attach_mmio(machine, &syscon);
    if (mmio == NULL) return mmio;
#ifdef USE_FDT
    struct fdt_node* test = fdt_node_create_reg("test", base_addr);
    fdt_node_add_prop_reg(test, "reg", base_addr, 0x1000);
    fdt_node_add_prop(test, "compatible", "sifive,test1\0sifive,test0\0syscon\0", 33);
    fdt_node_add_child(rvvm_get_fdt_soc(machine), test);

    struct fdt_node* poweroff = fdt_node_create("poweroff");
    fdt_node_add_prop_str(poweroff, "compatible", "syscon-poweroff");
    fdt_node_add_prop_u32(poweroff, "value", SYSCON_POWEROFF);
    fdt_node_add_prop_u32(poweroff, "offset", 0);
    fdt_node_add_prop_u32(poweroff, "regmap", fdt_node_get_phandle(test));
    fdt_node_add_child(rvvm_get_fdt_root(machine), poweroff);

    struct fdt_node* reboot = fdt_node_create("reboot");
    fdt_node_add_prop_str(reboot, "compatible", "syscon-reboot");
    fdt_node_add_prop_u32(reboot, "value", SYSCON_RESET);
    fdt_node_add_prop_u32(reboot, "offset", 0);
    fdt_node_add_prop_u32(reboot, "regmap", fdt_node_get_phandle(test));
    fdt_node_add_child(rvvm_get_fdt_root(machine), reboot);
#endif
    return mmio;
}

PUBLIC rvvm_mmio_dev_t* syscon_init_auto(rvvm_machine_t* machine)
{
    rvvm_addr_t addr = rvvm_mmio_zone_auto(machine, SYSCON_DEFAULT_MMIO, 0x1000);
    return syscon_init(machine, addr);
}
