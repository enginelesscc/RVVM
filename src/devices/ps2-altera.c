/*
ps2-altera.c - Altera PS2 Controller
Copyright (C) 2021  LekKit <github.com/LekKit>
                    cerg2010cerg2010 <github.com/cerg2010cerg2010>

This Source Code Form is subject to the terms of the Mozilla Public
License, v. 2.0. If a copy of the MPL was not distributed with this
file, You can obtain one at https://mozilla.org/MPL/2.0/.
*/

#include "ps2-altera.h"
#include "mem_ops.h"
#include "atomics.h"
#include "utils.h"
#include "fdtlib.h"

#define PS2_ALTERA_REG_DATA 0x0
#define PS2_ALTERA_REG_CTRL 0x4

#define PS2_ALTERA_CTRL_RE 0x1   // IRQ Enabled
#define PS2_ALTERA_CTRL_RI 0x100 // IRQ Pending
#define PS2_ALTERA_CTRL_CE 0x400 // Controller Error

#define PS2_ALTERA_DATA_RVALID 0x8000

#define PS2_ALTERA_REG_SIZE 0x1000

typedef struct {
    chardev_t* chardev;

    // IRQ data
    rvvm_intc_t* intc;
    rvvm_irq_t irq;

    // Controller registers
    uint32_t ctrl;
} ps2_altera_dev_t;

static void ps2_altera_notify(void* io_dev, uint32_t flags)
{
    ps2_altera_dev_t* ps2 = io_dev;
    if (flags & CHARDEV_RX) {
        if (atomic_or_uint32(&ps2->ctrl, PS2_ALTERA_CTRL_RI) & PS2_ALTERA_CTRL_RE) {
            rvvm_send_irq(ps2->intc, ps2->irq);
        }
    }
}

static bool ps2_altera_read(rvvm_mmio_dev_t* dev, void* data, uint64_t offset, uint8_t size)
{
    ps2_altera_dev_t* ps2 = dev->data;
    uint32_t val = 0;
    UNUSED(size);

    switch (offset) {
        case PS2_ALTERA_REG_DATA: {
            uint8_t byte = 0;
            uint32_t avail = chardev_read(ps2->chardev, &byte, sizeof(byte));
            val = byte | (avail ? PS2_ALTERA_DATA_RVALID : 0) | (avail << 16);
            break;
        }
        case PS2_ALTERA_REG_CTRL:
            val = atomic_load_uint32(&ps2->ctrl);
            break;
    }

    write_uint32_le(data, val);
    return true;
}

static bool ps2_altera_write(rvvm_mmio_dev_t* dev, void* data, uint64_t offset, uint8_t size)
{
    ps2_altera_dev_t* ps2 = dev->data;
    uint32_t val = read_uint32_le(data);
    UNUSED(size);

    switch (offset) {
        case PS2_ALTERA_REG_DATA: {
                uint8_t byte = val;
                if (!chardev_write(ps2->chardev, &byte, sizeof(byte))) {
                    atomic_or_uint32(&ps2->ctrl, PS2_ALTERA_CTRL_CE);
                }
            }
            break;
        case PS2_ALTERA_REG_CTRL:
            atomic_or_uint32(&ps2->ctrl, val & PS2_ALTERA_CTRL_RE);
            atomic_and_uint32(&ps2->ctrl, PS2_ALTERA_CTRL_RI | (val & (PS2_ALTERA_CTRL_RE | PS2_ALTERA_CTRL_CE)));
            break;
    }

    return true;
}

static void ps2_altera_update(rvvm_mmio_dev_t* dev)
{
    ps2_altera_dev_t* ps2 = dev->data;
    chardev_update(ps2->chardev);
}

static void ps2_altera_remove(rvvm_mmio_dev_t* dev)
{
    ps2_altera_dev_t* ps2 = dev->data;
    chardev_free(ps2->chardev);
    free(ps2);
}

static rvvm_mmio_type_t ps2_altera_dev_type = {
    .name = "ps2_altera",
    .update = ps2_altera_update,
    .remove = ps2_altera_remove,
};

void ps2_altera_init(rvvm_machine_t* machine, chardev_t* chardev, rvvm_addr_t addr, rvvm_intc_t* intc, rvvm_irq_t irq)
{
    ps2_altera_dev_t* ps2 = safe_new_obj(ps2_altera_dev_t);
    ps2->chardev = chardev;
    ps2->intc = intc;
    ps2->irq = irq;

    if (chardev) {
        chardev->io_dev = ps2;
        chardev->notify = ps2_altera_notify;
    }

    rvvm_mmio_dev_t ps2_mmio = {
        .addr = addr,
        .size = PS2_ALTERA_REG_SIZE,
        .data = ps2,
        .type = &ps2_altera_dev_type,
        .read = ps2_altera_read,
        .write = ps2_altera_write,
        .min_op_size = 4,
        .max_op_size = 4,
    };
    if (!rvvm_attach_mmio(machine, &ps2_mmio)) {
        rvvm_error("Failed to attach Altera PS/2 controller!");
        return;
    }

#ifdef USE_FDT
    struct fdt_node* ps2_fdt = fdt_node_create_reg("ps2", ps2_mmio.addr);
    fdt_node_add_prop_reg(ps2_fdt, "reg", ps2_mmio.addr, ps2_mmio.size);
    fdt_node_add_prop_str(ps2_fdt, "compatible", "altr,ps2-1.0");
    rvvm_fdt_describe_irq(ps2_fdt, intc, irq);
    fdt_node_add_child(rvvm_get_fdt_soc(machine), ps2_fdt);
#endif
}

void ps2_altera_init_auto(rvvm_machine_t* machine, chardev_t* chardev)
{
    rvvm_intc_t* intc = rvvm_get_intc(machine);
    rvvm_addr_t addr = rvvm_mmio_zone_auto(machine, PS2_ALTERA_ADDR_DEFAULT, PS2_ALTERA_REG_SIZE);
    ps2_altera_init(machine, chardev, addr, intc, rvvm_alloc_irq(intc));
}
