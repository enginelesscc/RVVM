/*
<rvvm/rvvm_irq.h> - RVVM Wired Interrupts API
Copyright (C) 2020-2026 LekKit <github.com/LekKit>

This Source Code Form is subject to the terms of the Mozilla Public
License, v. 2.0. If a copy of the MPL was not distributed with this
file, You can obtain one at https://mozilla.org/MPL/2.0/.
*/

#include <rvvm/rvvm_fdt.h>
#include <rvvm/rvvm_irq.h>

#include "atomics.h"
#include "bit_ops.h"
#include "utils.h"

PUSH_OPTIMIZATION_SIZE

struct rvvm_irq_dev {
    void* data;

    const rvvm_irq_dev_cb_t* cb;

    uint32_t irqs[5];
    uint32_t irq_base;
    uint32_t phandle;
};

RVVM_PUBLIC rvvm_irq_dev_t* rvvm_irq_dev_init(const rvvm_irq_dev_cb_t* cb, void* data)
{
    rvvm_irq_dev_t* irq_dev = NULL;
    if (cb && cb->set_irq) {
        irq_dev       = safe_new_obj(rvvm_irq_dev_t);
        irq_dev->cb   = cb;
        irq_dev->data = data;
    }
    return irq_dev;
}

RVVM_PUBLIC void rvvm_irq_dev_free(rvvm_irq_dev_t* irq_dev)
{
    free(irq_dev);
}

RVVM_PUBLIC void* rvvm_irq_dev_data(rvvm_irq_dev_t* irq_dev)
{
    if (likely(irq_dev)) {
        return irq_dev->data;
    }
    return NULL;
}

RVVM_PUBLIC void rvvm_irq_dev_set_base(rvvm_irq_dev_t* irq_dev, rvvm_irq_t irq_base)
{
    if (likely(irq_dev)) {
        atomic_cas_uint32(&irq_dev->irq_base, 0, irq_base);
    }
}

RVVM_PUBLIC void rvvm_irq_dev_set_phandle(rvvm_irq_dev_t* irq_dev, uint32_t phandle)
{
    if (likely(irq_dev)) {
        atomic_store_uint32(&irq_dev->phandle, phandle);
    }
}

RVVM_PUBLIC rvvm_irq_t rvvm_irq_alloc(rvvm_irq_dev_t* irq_dev, rvvm_irq_t irq)
{
    if (likely(irq_dev)) {
        uint32_t base = atomic_load_uint32_relax(&irq_dev->irq_base);
        for (size_t cell = 0; cell < STATIC_ARRAY_SIZE(irq_dev->irqs); ++cell) {
            uint32_t bits = atomic_load_uint32_relax(&irq_dev->irqs[cell]);
            while (~bits) {
                uint32_t nbit = bit_ctz32(~bits);
                uint32_t curr = (cell << 5) + nbit + base;
                if (curr >= irq) {
                    uint32_t mask = bit_set32(nbit);
                    uint32_t swap = atomic_or_uint32(&irq_dev->irqs[cell], mask);
                    bits          = swap | mask;
                    if (!(swap & mask)) {
                        return curr;
                    }
                } else {
                    // Skip only this candidate; the requested IRQ may be in this cell.
                    bits |= bit_set32(nbit);
                }
            }
        }
    }
    return RVVM_IRQ_INVALID;
}

RVVM_PUBLIC void rvvm_irq_dealloc(rvvm_irq_dev_t* irq_dev, rvvm_irq_t irq)
{
    if (likely(irq_dev)) {
        uint32_t base = atomic_load_uint32_relax(&irq_dev->irq_base);
        uint32_t cell = (irq - base) >> 5;
        uint32_t nbit = (irq - base) & 0x1F;
        if (cell < STATIC_ARRAY_SIZE(irq_dev->irqs)) {
            atomic_and_uint32(&irq_dev->irqs[cell], ~(1UL << nbit));
        }
    }
}

RVVM_PUBLIC void rvvm_irq_set(rvvm_irq_dev_t* irq_dev, rvvm_irq_t irq, bool lvl)
{
    if (likely(irq_dev)) {
        irq_dev->cb->set_irq(irq_dev, irq, lvl);
    }
}

RVVM_PUBLIC void rvvm_irq_fdt_describe(rvvm_fdt_node_t* node, rvvm_irq_dev_t* irq_dev, rvvm_irq_t irq)
{
    if (node && irq_dev && irq != RVVM_IRQ_INVALID) {
        uint32_t cells[8] = {0};
        size_t   count    = rvvm_irq_fdt_cells(irq_dev, irq, cells, STATIC_ARRAY_SIZE(cells));
        rvvm_fdt_prop_set_u32(node, "interrupt-parent", rvvm_irq_fdt_phandle(irq_dev));
        rvvm_fdt_prop_set_cells(node, "interrupts", cells, count);
    }
}

RVVM_PUBLIC uint32_t rvvm_irq_fdt_phandle(rvvm_irq_dev_t* irq_dev)
{
    if (likely(irq_dev)) {
        return atomic_load_uint32_relax(&irq_dev->phandle);
    }
    return 0;
}

RVVM_PUBLIC size_t rvvm_irq_fdt_cells(rvvm_irq_dev_t* irq_dev, rvvm_irq_t irq, uint32_t* cells, size_t size)
{
    if (cells && size && irq != RVVM_IRQ_INVALID) {
        if (irq_dev && irq_dev->cb->fdt_irq_cells) {
            return irq_dev->cb->fdt_irq_cells(irq_dev, irq, cells, size);
        } else {
            cells[0] = irq;
            return 1;
        }
    }
    return 0;
}

POP_OPTIMIZATION_SIZE
