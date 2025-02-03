/*
riscv-aplic.c - RISC-V Advanced Platform-Level Interrupt Controller
Copyright (C) 2024  LekKit <github.com/LekKit>

This Source Code Form is subject to the terms of the Mozilla Public
License, v. 2.0. If a copy of the MPL was not distributed with this
file, You can obtain one at https://mozilla.org/MPL/2.0/.
*/

#include "riscv-aplic.h"
#include "riscv_hart.h"
#include "bit_ops.h"
#include "mem_ops.h"
#include "atomics.h"

#define APLIC_REGION_SIZE 0x4000

// APLIC registers & register groups
#define APLIC_REG_DOMAINCFG      0x0000 // Domain configuration
#define APLIC_REG_SOURCECFG_1    0x0004 // Source configurations (1 - 1023)
#define APLIC_REG_SOURCECFG_1023 0x0FFC // Source configurations (1 - 1023)
#define APLIC_REG_MMSIADDRCFG    0x1BC0 // Machine MSI address configuration
#define APLIC_REG_MMSIADDRCFGH   0x1BC4 // Machine MSI address configuration (high)
#define APLIC_REG_SMSIADDRCFG    0x1BC8 // Supervisor MSI address configuration
#define APLIC_REG_SMSIADDRCFGH   0x1BCC // Supervisor MSI address configuration (high)
#define APLIC_REG_SETIP_0        0x1C00 // Set interrupt-pending bits (0 - 31)
#define APLIC_REG_SETIP_31       0x1C7C // Set interrupt-pending bits (0 - 31)
#define APLIC_REG_SETIPNUM       0x1CDC // Set interrupt-pending bit by number
#define APLIC_REG_IN_CLRIP_0     0x1D00 // Rectified inputs, clear interrupt-pending bits (0 - 31)
#define APLIC_REG_IN_CLRIP_31    0x1D7C // Rectified inputs, clear interrupt-pending bits (0 - 31)
#define APLIC_REG_CLRIPNUM       0x1DDC // Clear interrupt-pending bit by number
#define APLIC_REG_SETIE_0        0x1E00 // Set interrupt-enabled bits (0 - 31)
#define APLIC_REG_SETIE_31       0x1E7C // Set interrupt-enabled bits (0 - 31)
#define APLIC_REG_SETIENUM       0x1EDC // Set interrupt-enabled bit by number
#define APLIC_REG_CLRIE_0        0x1F00 // Clear interrupt-enabled bits (0 - 31)
#define APLIC_REG_CLRIE_31       0x1F7C // Clear interrupt-enabled bits (0 - 31)
#define APLIC_REG_CLRIENUM       0x1FDC // Clear interrupt-enabled bit by number
#define APLIC_REG_SETIPNUM_LE    0x2000 // Set interrupt-pending bit by number (Little-endian)
#define APLIC_REG_SETIPNUM_BE    0x2004 // Set interrupt-pending bit by number (Big-endian)
#define APLIC_REG_GENMSI         0x3000 // Generate MSI
#define APLIC_REG_TARGET_1       0x3004 // Interrupt targets (1 - 1023)
#define APLIC_REG_TARGET_1023    0x3FFC // Interrupt targets (1 - 1023)

// APLIC register values
#define APLIC_DOMAINCFG_BE 0x1        // Big-endian mode
#define APLIC_DOMAINCFG_DM 0x4        // MSI delivery mode
#define APLIC_DOMAINCFG_IE 0x100      // Interrupts enabled
#define APLIC_DOMAINCFG    0x80000004 // Default hardwired domaincfg

#define APLIC_SOURCECFG_DELEGATE  0x400 // Delegate to child domain
#define APLIC_SOURCECFG_INACTIVE  0x0   // Inactive source
#define APLIC_SOURCECFG_DETACHED  0x1   // Detached source
#define APLIC_SOURCECFG_EDGE_RISE 0x4   // Active, edge-triggered on rise
#define APLIC_SOURCECFG_EDGE_FALL 0x5   // Active, edge-triggered on fall
#define APLIC_SOURCECFG_LVL_HIGH  0x6   // Active, level-triggered when high
#define APLIC_SOURCECFG_LVL_LOW   0x7   // Active, level-triggered when low

#define APLIC_MSIADDRCFGH_L 0x80000000  // Locked

// Limit on APLIC interrupt identities, maximum 1024
#define APLIC_SRC_LIMIT 64

// Number of APLIC source bitset registers
#define APLIC_SRC_REGS (APLIC_SRC_LIMIT >> 5)

void riscv_send_msi_irq(rvvm_hart_t* vm, bool smode, uint32_t identity);

typedef struct {
    rvvm_machine_t* machine;
    rvvm_intc_t intc;
    uint32_t phandle;

    uint32_t domaincfg;
    uint32_t msicfg;

    uint32_t source[APLIC_SRC_LIMIT];
    uint32_t target[APLIC_SRC_LIMIT];
} aplic_ctx_t;

static bool aplic_mmio_read(rvvm_mmio_dev_t* dev, void* data, uint64_t offset, uint8_t size)
{
    aplic_ctx_t* aplic = dev->data;
    uint32_t val = -1;
    UNUSED(size);

    switch (offset) {
        case APLIC_REG_DOMAINCFG:
            val = atomic_load_uint32_relax(&aplic->domaincfg) | APLIC_DOMAINCFG;
            break;
        default:
            break;
    }

    //rvvm_warn("aplic read  %08x from %02x", val, (uint32_t)offset);
    write_uint32_le(data, val);
    return true;
}

static bool aplic_mmio_write(rvvm_mmio_dev_t* dev, void* data, uint64_t offset, uint8_t size)
{
    aplic_ctx_t* aplic = dev->data;
    uint32_t val = read_uint32_le(data);
    UNUSED(size);

    //rvvm_warn("aplic write %08x to   %02x", val, (uint32_t)offset);
    if (offset >= APLIC_REG_TARGET_1 && offset <= APLIC_REG_TARGET_1023) {
        uint64_t reg = ((offset - APLIC_REG_TARGET_1) >> 2) + 1;
        if (reg < APLIC_SRC_LIMIT) {
            atomic_store_uint32_relax(&aplic->target[reg], val);
        }
    } else if (offset >= APLIC_REG_SOURCECFG_1 && offset <= APLIC_REG_SOURCECFG_1023) {
        uint64_t reg = ((offset - APLIC_REG_SOURCECFG_1) >> 2) + 1;
        if (reg < APLIC_SRC_LIMIT) {
            atomic_store_uint32_relax(&aplic->source[reg], val);
        }
    }

    switch (offset) {
        case APLIC_REG_DOMAINCFG:
            atomic_store_uint32_relax(&aplic->domaincfg, val & APLIC_DOMAINCFG_IE);
            break;
    }

    return true;
}

static rvvm_mmio_type_t aplic_dev_type = {
    .name = "riscv_aplic",
};

static bool aplic_send_irq(rvvm_intc_t* intc, rvvm_irq_t irq)
{
    aplic_ctx_t* aplic = intc->data;
    if (irq > 0 && irq < APLIC_SRC_LIMIT) {
        uint32_t source =  atomic_load_uint32_relax(&aplic->source[irq]);
        if (source) {
            uint32_t target =  atomic_load_uint32_relax(&aplic->target[irq]);
            uint64_t hartid = target >> 18;
            if (hartid < vector_size(aplic->machine->harts)) {
                rvvm_hart_t* hart = vector_at(aplic->machine->harts, hartid);
                riscv_send_aia_irq(hart, true, bit_cut(target, 0, 10));
            }
            return true;
        }
    }
    return false;
}

static uint32_t aplic_fdt_phandle(rvvm_intc_t* intc)
{
    aplic_ctx_t* aplic = intc->data;
    return aplic->phandle;
}

static uint64_t aplic_fdt_irq_cells(rvvm_intc_t* intc, rvvm_irq_t irq, uint32_t* cells, uint64_t size)
{
    UNUSED(intc);
    if (cells && size >= 2) {
        cells[0] = irq;
        cells[1] = 0x4; // Level-triggered
        return 2;
    }
    return 0;
}

PUBLIC rvvm_intc_t* riscv_aplic_init(rvvm_machine_t* machine, rvvm_addr_t addr, bool smode)
{
    aplic_ctx_t* aplic = safe_new_obj(aplic_ctx_t);
    aplic->machine = machine;

    rvvm_intc_t aplic_intc = {
        .data = aplic,
        .send_irq = aplic_send_irq,
        .fdt_phandle = aplic_fdt_phandle,
        .fdt_irq_cells = aplic_fdt_irq_cells,
    };

    rvvm_mmio_dev_t aplic_mmio = {
        .addr = addr,
        .size = APLIC_REGION_SIZE,
        .data = aplic,
        .min_op_size = 4,
        .max_op_size = 4,
        .read = aplic_mmio_read,
        .write = aplic_mmio_write,
        .type = &aplic_dev_type,
    };

    aplic->intc = aplic_intc;

    if (!rvvm_attach_mmio(machine, &aplic_mmio)) {
        rvvm_error("Failed to attach RISC-V APLIC!");
        return NULL;
    }

    if (smode) {
        rvvm_set_intc(machine, &aplic->intc);
    }

#ifdef USE_FDT
    struct fdt_node* imsic_fdt = fdt_node_find_reg_any(rvvm_get_fdt_soc(machine), smode ? "imsics_s" : "imsics_m");
    struct fdt_node* aplic_s = fdt_node_find_reg_any(rvvm_get_fdt_soc(machine), "aplic_s");
    if (!imsic_fdt) {
        rvvm_warn("Missing /soc/imsic node in FDT!");
        return NULL;
    }
    if (!smode && !aplic_s) {
        rvvm_warn("Missing /soc/aplic_s node in FDT!");
        return NULL;
    }

    struct fdt_node* aplic_fdt = fdt_node_create_reg(smode ? "aplic_s" : "aplic_m", aplic_mmio.addr);
    fdt_node_add_prop_reg(aplic_fdt, "reg", aplic_mmio.addr, aplic_mmio.size);
    fdt_node_add_prop_str(aplic_fdt, "compatible", "riscv,aplic");
    fdt_node_add_prop_u32(aplic_fdt, "msi-parent", fdt_node_get_phandle(imsic_fdt));
    fdt_node_add_prop(aplic_fdt, "interrupt-controller", NULL, 0);
    fdt_node_add_prop_u32(aplic_fdt, "#interrupt-cells", 2);
    fdt_node_add_prop_u32(aplic_fdt, "#address-cells", 0);
    fdt_node_add_prop_u32(aplic_fdt, "riscv,num-sources", APLIC_SRC_LIMIT - 1);

    if (!smode) {
        uint32_t children = fdt_node_get_phandle(aplic_s);
        uint32_t delegate[] = { children, 1, APLIC_SRC_LIMIT - 1, };
        fdt_node_add_prop_u32(aplic_fdt, "riscv,children", children);
        fdt_node_add_prop_cells(aplic_fdt, "riscv,delegate", delegate, STATIC_ARRAY_SIZE(delegate));
        fdt_node_add_prop_cells(aplic_fdt, "riscv,delegation", delegate, STATIC_ARRAY_SIZE(delegate));
    }

    fdt_node_add_child(rvvm_get_fdt_soc(machine), aplic_fdt);

    aplic->phandle = fdt_node_get_phandle(aplic_fdt);
#endif

    return &aplic->intc;
}

PUBLIC rvvm_intc_t* riscv_aplic_init_auto(rvvm_machine_t* machine)
{
    rvvm_addr_t m_addr = rvvm_mmio_zone_auto(machine, APLIC_M_ADDR_DEFAULT, APLIC_REGION_SIZE);
    rvvm_addr_t s_addr = rvvm_mmio_zone_auto(machine, APLIC_S_ADDR_DEFAULT, APLIC_REGION_SIZE);
    rvvm_intc_t* ret = riscv_aplic_init(machine, s_addr, true);
    riscv_aplic_init(machine, m_addr, false);
    return ret;
}
