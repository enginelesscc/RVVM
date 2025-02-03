/*
riscv-imsic.c - RISC-V Incoming Message-Signaled Interrupt Controller
Copyright (C) 2024  LekKit <github.com/LekKit>

This Source Code Form is subject to the terms of the Mozilla Public
License, v. 2.0. If a copy of the MPL was not distributed with this
file, You can obtain one at https://mozilla.org/MPL/2.0/.
*/

#include "riscv-imsic.h"
#include "riscv_hart.h"
#include "mem_ops.h"
#include "bit_ops.h"

#define IMSIC_REG_SETEIPNUM_LE 0x00
#define IMSIC_REG_SETEIPNUM_BE 0x04

typedef struct {
    bool smode;
} imsic_ctx_t;

static bool imsic_mmio_write(rvvm_mmio_dev_t* dev, void* data, uint64_t offset, uint8_t size)
{
    imsic_ctx_t* imsic = dev->data;
    uint64_t hartid = offset >> 12;
    UNUSED(size);

    if (hartid < vector_size(dev->machine->harts)) {
        rvvm_hart_t* hart = vector_at(dev->machine->harts, hartid);
        switch (offset & 0xFFC) {
            case IMSIC_REG_SETEIPNUM_LE: {
                riscv_send_aia_irq(hart, imsic->smode, read_uint32_le(data));
                break;
            }
            case IMSIC_REG_SETEIPNUM_BE: {
                riscv_send_aia_irq(hart, imsic->smode, read_uint32_be_m(data));
                break;
            }
        }
    }

    return true;
}

static rvvm_mmio_type_t imsic_dev_type = {
    .name = "riscv_imsic",
};

PUBLIC void riscv_imsic_init(rvvm_machine_t* machine, rvvm_addr_t addr, bool smode)
{
    if (rvvm_machine_running(machine)) {
        rvvm_error("Can't enable AIA on already running machine!");
        return;
    }

    vector_foreach(machine->harts, i) {
        riscv_hart_aia_init(vector_at(machine->harts, i));
    }

    rvvm_append_isa_string(machine, "_smaia_ssaia");

    imsic_ctx_t* imsic = safe_new_obj(imsic_ctx_t);

    rvvm_mmio_dev_t imsic_mmio = {
        .addr = addr,
        .size = vector_size(machine->harts) << 12,
        .data = imsic,
        .min_op_size = 4,
        .max_op_size = 4,
        .read = rvvm_mmio_none,
        .write = imsic_mmio_write,
        .type = &imsic_dev_type,
    };

    imsic->smode = smode;

    if (!rvvm_attach_msi_target(machine, &imsic_mmio)) {
        rvvm_error("Failed to attach RISC-V IMSIC!");
        return;
    }

#ifdef USE_FDT
    struct fdt_node* imsic_fdt = fdt_node_create_reg(smode ? "imsics_s" : "imsics_m", imsic_mmio.addr);
    struct fdt_node* cpus = fdt_node_find(rvvm_get_fdt_root(machine), "cpus");
    vector_t(uint32_t) irq_ext = {0};

    fdt_node_add_prop_reg(imsic_fdt, "reg", imsic_mmio.addr, imsic_mmio.size);
    fdt_node_add_prop_str(imsic_fdt, "compatible", "riscv,imsics");
    fdt_node_add_prop(imsic_fdt, "interrupt-controller", NULL, 0);
    fdt_node_add_prop_u32(imsic_fdt, "#interrupt-cells", 0);
    fdt_node_add_prop(imsic_fdt, "msi-controller", NULL, 0);
    fdt_node_add_prop_u32(imsic_fdt, "#msi-cells", 0);
    fdt_node_add_prop_u32(imsic_fdt, "riscv,num-ids", RVVM_AIA_IRQ_LIMIT - 1);

    vector_foreach(machine->harts, i) {
        struct fdt_node* cpu = fdt_node_find_reg(cpus, "cpu", i);
        struct fdt_node* cpu_irq = fdt_node_find(cpu, "interrupt-controller");

        if (cpu_irq) {
            vector_push_back(irq_ext, fdt_node_get_phandle(cpu_irq));
            vector_push_back(irq_ext, smode ? RISCV_INTERRUPT_SEXTERNAL : RISCV_INTERRUPT_MEXTERNAL);
        } else {
            rvvm_warn("Missing CPU IRQ nodes in FDT!");
        }
    }

    fdt_node_add_prop_cells(imsic_fdt, "interrupts-extended", vector_buffer(irq_ext), vector_size(irq_ext));
    fdt_node_add_child(rvvm_get_fdt_soc(machine), imsic_fdt);
    vector_free(irq_ext);
#endif
}

PUBLIC void riscv_imsic_init_auto(rvvm_machine_t* machine)
{
    uint64_t imsic_size = vector_size(machine->harts) << 12;
    rvvm_addr_t m_addr = rvvm_mmio_zone_auto(machine, IMSIC_M_ADDR_DEFAULT, imsic_size);
    rvvm_addr_t s_addr = rvvm_mmio_zone_auto(machine, IMSIC_S_ADDR_DEFAULT, imsic_size);
    riscv_imsic_init(machine, m_addr, false);
    riscv_imsic_init(machine, s_addr, true);
}
