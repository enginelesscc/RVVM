/*
ns16550a.c - NS16550A UART
Copyright (C) 2021  LekKit <github.com/LekKit>

This Source Code Form is subject to the terms of the Mozilla Public
License, v. 2.0. If a copy of the MPL was not distributed with this
file, You can obtain one at https://mozilla.org/MPL/2.0/.
*/

#include <rvvm/rvvm_board.h>
#include <rvvm/rvvm_fdt.h>
#include <rvvm/rvvm_irq.h>
#include <rvvm/rvvm_region.h>
#include <rvvm/rvvm_snapshot.h>

#include "chardev.h"
#include "mem_ops.h"
#include "ns16550a.h"
#include "utils.h"

PUSH_OPTIMIZATION_SIZE

typedef struct {
    chardev_t* chardev;

    rvvm_irq_dev_t* irq_dev;
    rvvm_irq_t      irq;

    uint32_t variant; // 0: generic, 1: K1/XScale, 2: JH7110/DesignWare
    uint32_t flags;
    uint32_t ier;
    uint32_t lcr;
    uint32_t mcr;
    uint32_t scr;
    uint32_t dll;
    uint32_t dlm;
} ns16550a_dev_t;

// Read
#define NS16550A_REG_RBR_DLL 0x0
#define NS16550A_REG_IIR     0x2
// Write
#define NS16550A_REG_THR_DLL 0x0
#define NS16550A_REG_FCR     0x2
// RW
#define NS16550A_REG_IER_DLM 0x1
#define NS16550A_REG_LCR     0x3
#define NS16550A_REG_MCR     0x4
#define NS16550A_REG_LSR     0x5
#define NS16550A_REG_MSR     0x6
#define NS16550A_REG_SCR     0x7

#define NS16550A_IER_RECV    0x1
#define NS16550A_IER_THR     0x2
#define NS16550A_IER_LSR     0x4
#define NS16550A_IER_MSR     0x8

#define NS16550A_IIR_FIFO    0xC0
#define NS16550A_IIR_NONE    0x1
#define NS16550A_IIR_MSR     0x0
#define NS16550A_IIR_THR     0x2
#define NS16550A_IIR_RECV    0x4
#define NS16550A_IIR_LSR     0x6

#define NS16550A_LSR_RECV    0x1
#define NS16550A_LSR_THR     0x60

#define NS16550A_LCR_DLAB    0x80

// Handle IIR/IER update
static void ns16550a_update_irq(ns16550a_dev_t* uart)
{
    uint32_t flags = atomic_load_uint32_relax(&uart->flags);
    uint32_t ier   = atomic_load_uint32_relax(&uart->ier);
    if (((flags & CHARDEV_RX) && (ier & NS16550A_IER_RECV)) || ((flags & CHARDEV_TX) && (ier & NS16550A_IER_THR))) {
        rvvm_irq_raise(uart->irq_dev, uart->irq);
    } else {
        rvvm_irq_lower(uart->irq_dev, uart->irq);
    }
}

// Handle RX/TX chardev flags update
static void ns16550a_notify(void* io_dev, uint32_t flags)
{
    ns16550a_dev_t* uart = io_dev;
    if (atomic_swap_uint32(&uart->flags, flags) != flags) {
        ns16550a_update_irq(uart);
    }
}

// Poll for possible RX/TX chardev flags update
static void ns16550a_poll_rxtx(ns16550a_dev_t* uart)
{
    uint32_t flags = chardev_poll(uart->chardev);
    if (flags != atomic_load_uint32_relax(&uart->flags)) {
        ns16550a_notify(uart, flags);
    }
}

static void ns16550a_read(rvvm_reg_dev_t* dev, void* data, size_t size, size_t off)
{
    ns16550a_dev_t* uart = rvvm_region_data(dev);
    memset(data, 0, size);
    off >>= uart->variant ? 2 : 0;

    switch (off) {
        case NS16550A_REG_RBR_DLL:
            if (atomic_load_uint32_relax(&uart->lcr) & NS16550A_LCR_DLAB) {
                write_uint8(data, atomic_load_uint32_relax(&uart->dll));
            } else if (chardev_poll(uart->chardev) & CHARDEV_RX) {
                chardev_read(uart->chardev, data, 1);
                ns16550a_poll_rxtx(uart);
            }
            break;
        case NS16550A_REG_IER_DLM:
            if (atomic_load_uint32_relax(&uart->lcr) & NS16550A_LCR_DLAB) {
                write_uint8(data, atomic_load_uint32_relax(&uart->dlm));
            } else {
                write_uint8(data, atomic_load_uint32_relax(&uart->ier));
            }
            break;
        case NS16550A_REG_IIR: {
            uint32_t flags = chardev_poll(uart->chardev);
            uint32_t ier   = atomic_load_uint32_relax(&uart->ier);
            if ((flags & CHARDEV_RX) && (ier & NS16550A_IER_RECV)) {
                write_uint8(data, NS16550A_IIR_RECV | NS16550A_IIR_FIFO);
            } else if ((flags & CHARDEV_TX) && (ier & NS16550A_IER_THR)) {
                write_uint8(data, NS16550A_IIR_THR | NS16550A_IIR_FIFO);
            } else {
                write_uint8(data, NS16550A_IIR_NONE | NS16550A_IIR_FIFO);
            }
            break;
        }
        case NS16550A_REG_LCR:
            write_uint8(data, atomic_load_uint32_relax(&uart->lcr));
            break;
        case NS16550A_REG_MCR:
            write_uint8(data, atomic_load_uint32_relax(&uart->mcr));
            break;
        case NS16550A_REG_LSR: {
            uint32_t flags = chardev_poll(uart->chardev);
            write_uint8(data,
                        ((flags & CHARDEV_RX) ? NS16550A_LSR_RECV : 0) | ((flags & CHARDEV_TX) ? NS16550A_LSR_THR : 0));
            break;
        }
        case NS16550A_REG_MSR:
            write_uint8(data, 0xB0);
            break;
        case NS16550A_REG_SCR:
            write_uint8(data, atomic_load_uint32_relax(&uart->scr));
            break;
        case 0x1F: // DesignWare USR at 0x7C: idle transmitter, receive state.
            if (uart->variant == 2) {
                write_uint8(data, 6 | ((chardev_poll(uart->chardev) & CHARDEV_RX) ? 8 : 0));
            }
            break;
    }
}

static void ns16550a_write(rvvm_reg_dev_t* dev, const void* data, size_t size, size_t off)
{
    ns16550a_dev_t* uart = rvvm_region_data(dev);
    UNUSED(size);
    off >>= uart->variant ? 2 : 0;

    switch (off) {
        case NS16550A_REG_THR_DLL:
            if (atomic_load_uint32_relax(&uart->lcr) & NS16550A_LCR_DLAB) {
                atomic_store_uint32_relax(&uart->dll, read_uint8(data));
            } else {
                chardev_write(uart->chardev, data, 1);
                ns16550a_poll_rxtx(uart);
            }
            break;
        case NS16550A_REG_IER_DLM:
            if (atomic_load_uint32_relax(&uart->lcr) & NS16550A_LCR_DLAB) {
                atomic_store_uint32_relax(&uart->dlm, read_uint8(data));
            } else {
                atomic_store_uint32_relax(&uart->ier, read_uint8(data));
                ns16550a_update_irq(uart);
            }
            break;
        case NS16550A_REG_LCR:
            atomic_store_uint32_relax(&uart->lcr, read_uint8(data));
            break;
        case NS16550A_REG_MCR:
            atomic_store_uint32_relax(&uart->mcr, read_uint8(data));
            break;
        case NS16550A_REG_SCR:
            atomic_store_uint32_relax(&uart->scr, read_uint8(data));
            break;
    }
}

static void ns16550a_poll(rvvm_reg_dev_t* dev)
{
    ns16550a_dev_t* uart = rvvm_region_data(dev);
    chardev_update(uart->chardev);
}

static void ns16550a_suspend(rvvm_reg_dev_t* dev, rvvm_snapshot_t* snap, bool resume)
{
    if (snap) {
        ns16550a_dev_t* uart = rvvm_region_data(dev);
        rvvm_snapshot_section(snap, "serial-ns16550a");
        rvvm_snapshot_field(snap, uart->flags);
        rvvm_snapshot_field(snap, uart->ier);
        rvvm_snapshot_field(snap, uart->lcr);
        rvvm_snapshot_field(snap, uart->mcr);
        rvvm_snapshot_field(snap, uart->scr);
        rvvm_snapshot_field(snap, uart->dll);
        rvvm_snapshot_field(snap, uart->dlm);
    }
    UNUSED(resume);
}

static void ns16550a_cleanup(rvvm_reg_dev_t* dev)
{
    ns16550a_dev_t* uart = rvvm_region_data(dev);
    rvvm_irq_dealloc(uart->irq_dev, uart->irq);
    chardev_free(uart->chardev);
    free(uart);
}

static const rvvm_reg_type_t ns16550a_type = {
    .name     = "serial-ns16550a",
    .read     = ns16550a_read,
    .write    = ns16550a_write,
    .poll     = ns16550a_poll,
    .suspend  = ns16550a_suspend,
    .cleanup  = ns16550a_cleanup,
    .min_size = 1,
    .max_size = 1,
};

/* K1/XScale and JH7110/DesignWare registers are one byte of state in each 32-bit MMIO word. */
static const rvvm_reg_type_t wide_uart_type = {
    .name     = "serial-ns16550a-mmio32",
    .read     = ns16550a_read,
    .write    = ns16550a_write,
    .poll     = ns16550a_poll,
    .suspend  = ns16550a_suspend,
    .cleanup  = ns16550a_cleanup,
    .min_size = 4,
    .max_size = 4,
};

static rvvm_reg_dev_t* ns16550a_init_impl(rvvm_machine_t* machine, //
                                          chardev_t*      chardev, //
                                          rvvm_addr_t     addr,    //
                                          uint32_t        attr,    //
                                          rvvm_irq_dev_t* irq_dev, //
                                          rvvm_irq_t irq, uint32_t variant)
{
    ns16550a_dev_t* uart = safe_new_obj(ns16550a_dev_t);
    uart->variant        = variant;
    rvvm_reg_desc_t desc = {
        .addr = addr,
        .size = variant == 2 ? 0x10000 : (variant ? 0x100 : ((attr & RVVM_REG_ATTR_PIO) ? 0x08 : 0x1000)),
        .data = uart,
        .type = variant ? &wide_uart_type : &ns16550a_type,
        .attr = attr,
    };

    if (!irq_dev) {
        irq_dev = rvvm_get_intc(machine);
    }

    uart->chardev = chardev;
    uart->irq_dev = irq_dev;
    uart->irq     = rvvm_irq_alloc(irq_dev, irq);

    if (chardev) {
        chardev->io_dev = uart;
        chardev->notify = ns16550a_notify;
    }

    rvvm_reg_dev_t*  dev = rvvm_region_init_auto(machine, &desc);
    rvvm_fdt_node_t* soc = rvvm_get_fdt_soc(machine);

    if (dev && soc) {
        rvvm_fdt_node_t* fdt = rvvm_fdt_init_reg("uart", desc.addr);
        rvvm_fdt_prop_set_reg(fdt, "reg", desc.addr, desc.size);
        if (variant) {
            if (variant == 2) {
                rvvm_fdt_prop_set_str_list(fdt, "compatible", "starfive,jh7110-uart\0snps,dw-apb-uart\0ns16550a");
            } else {
                rvvm_fdt_prop_set_str_list(fdt, "compatible", "spacemit,k1-uart\0intel,xscale-uart\0ns16550a");
            }
            rvvm_fdt_prop_set_u32(fdt, "reg-shift", 2);
            rvvm_fdt_prop_set_u32(fdt, "reg-io-width", 4);
        } else {
            rvvm_fdt_prop_set_str(fdt, "compatible", "ns16550a");
        }
        rvvm_fdt_prop_set_u32(fdt, "clock-frequency", variant == 2 ? 24000000 : (variant ? 14745600 : 20000000));
        rvvm_fdt_prop_set_u32(fdt, "fifo-size", 16);
        rvvm_fdt_prop_set_str(fdt, "status", "okay");
        rvvm_irq_fdt_describe(fdt, uart->irq_dev, uart->irq);
        rvvm_fdt_prop_set_flag(fdt, "wakeup-source");
        rvvm_fdt_attach(soc, fdt);

        if (variant || desc.addr == 0x10000000UL) {
            rvvm_fdt_node_t* chosen = rvvm_fdt_find(rvvm_get_fdt_root(machine), "chosen");
            if (!rvvm_fdt_prop_get(chosen, "stdout-path", NULL)) {
                rvvm_fdt_prop_set_str(chosen, "stdout-path",
                                      variant == 1 ? "/soc/uart@d4017000" : "/soc/uart@10000000");
                rvvm_append_cmdline(machine, "console=ttyS");
            }
        }
    }

    return dev;
}

RVVM_PUBLIC rvvm_reg_dev_t* rvvm_ns16550a_init(rvvm_machine_t* machine, chardev_t* chardev, rvvm_addr_t addr,
                                               uint32_t attr, rvvm_irq_dev_t* irq_dev, rvvm_irq_t irq)
{
    return ns16550a_init_impl(machine, chardev, addr, attr, irq_dev, irq, 0);
}

rvvm_reg_dev_t* ns16550a_init_k1(rvvm_machine_t* machine, chardev_t* chardev)
{
    return ns16550a_init_impl(machine, chardev, 0xD4017000UL, RVVM_REG_ATTR_FIX, NULL, 42, 1);
}

rvvm_reg_dev_t* ns16550a_init_jh7110(rvvm_machine_t* machine, chardev_t* chardev)
{
    return ns16550a_init_impl(machine, chardev, 0x10000000UL, RVVM_REG_ATTR_FIX, NULL, 32, 2);
}

POP_OPTIMIZATION_SIZE
