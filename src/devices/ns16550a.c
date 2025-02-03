/*
ns16550a.c - NS16550A UART
Copyright (C) 2021  LekKit <github.com/LekKit>

This Source Code Form is subject to the terms of the Mozilla Public
License, v. 2.0. If a copy of the MPL was not distributed with this
file, You can obtain one at https://mozilla.org/MPL/2.0/.
*/

#include "ns16550a.h"
#include "fdtlib.h"
#include "chardev.h"
#include "spinlock.h"
#include "utils.h"
#include "mem_ops.h"

#define NS16550A_MMIO_SIZE 0x1000

typedef struct {
    chardev_t* chardev;
    rvvm_intc_t* intc;
    rvvm_irq_t irq;

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

static void ns16550a_notify(void* io_dev, uint32_t flags)
{
    ns16550a_dev_t* uart = io_dev;
    uint32_t ier = atomic_load_uint32_relax(&uart->ier);
    if (((flags & CHARDEV_RX) && (ier & NS16550A_IER_RECV))
     || ((flags & CHARDEV_TX) && (ier & NS16550A_IER_THR))) {
        rvvm_send_irq(uart->intc, uart->irq);
    }
}

static bool ns16550a_mmio_read(rvvm_mmio_dev_t* dev, void* data, uint64_t offset, uint8_t size)
{
    ns16550a_dev_t* uart = dev->data;
    memset(data, 0, size);

    switch (offset) {
        case NS16550A_REG_RBR_DLL:
            if (atomic_load_uint32_relax(&uart->lcr) & NS16550A_LCR_DLAB) {
                write_uint8(data, atomic_load_uint32_relax(&uart->dll));
            } else if (chardev_poll(uart->chardev) & CHARDEV_RX) {
                chardev_read(uart->chardev, data, 1);
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
            uint32_t ier = atomic_load_uint32_relax(&uart->ier);
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
            write_uint8(data, ((flags & CHARDEV_RX) ? NS16550A_LSR_RECV : 0)
                            | ((flags & CHARDEV_TX) ? NS16550A_LSR_THR : 0));
            break;
        }
        case NS16550A_REG_MSR:
            write_uint8(data, 0xF0);
            break;
        case NS16550A_REG_SCR:
            write_uint8(data, atomic_load_uint32_relax(&uart->scr));
            break;
        default:
            write_uint8(data, 0);
            break;
    }
    return true;
}

static bool ns16550a_mmio_write(rvvm_mmio_dev_t* dev, void* data, uint64_t offset, uint8_t size)
{
    ns16550a_dev_t* uart = dev->data;
    UNUSED(size);

    switch (offset) {
        case NS16550A_REG_THR_DLL:
            if (atomic_load_uint32_relax(&uart->lcr) & NS16550A_LCR_DLAB) {
                atomic_store_uint32_relax(&uart->dll, read_uint8(data));
            } else {
                chardev_write(uart->chardev, data, 1);
            }
            break;
        case NS16550A_REG_IER_DLM:
            if (atomic_load_uint32_relax(&uart->lcr) & NS16550A_LCR_DLAB) {
                atomic_store_uint32_relax(&uart->dlm, read_uint8(data));
            } else {
                atomic_store_uint32_relax(&uart->ier, read_uint8(data));
                // Trigger re-enabled interrupts, if any
                ns16550a_notify(uart, chardev_poll(uart->chardev));
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
        default:
            break;
    }
    return true;
}

static void ns16550a_update(rvvm_mmio_dev_t* dev)
{
    ns16550a_dev_t* uart = dev->data;
    chardev_update(uart->chardev);
}

static void ns16550a_remove(rvvm_mmio_dev_t* dev)
{
    ns16550a_dev_t* uart = dev->data;
    chardev_free(uart->chardev);
    free(uart);
}

static rvvm_mmio_type_t ns16550a_dev_type = {
    .name = "ns16550a",
    .update = ns16550a_update,
    .remove = ns16550a_remove,
};

PUBLIC rvvm_mmio_dev_t* ns16550a_init(rvvm_machine_t* machine, chardev_t* chardev,
                                      rvvm_addr_t addr, rvvm_intc_t* intc, rvvm_irq_t irq)
{
    ns16550a_dev_t* uart = safe_new_obj(ns16550a_dev_t);
    uart->chardev = chardev;
    uart->intc = intc;
    uart->irq = irq;

    if (chardev) {
        chardev->io_dev = uart;
        chardev->notify = ns16550a_notify;
    }

    rvvm_mmio_dev_t ns16550a = {
        .addr = addr,
        .size = NS16550A_MMIO_SIZE,
        .data = uart,
        .type = &ns16550a_dev_type,
        .read = ns16550a_mmio_read,
        .write = ns16550a_mmio_write,
        .min_op_size = 1,
        .max_op_size = 1,
    };

    rvvm_mmio_dev_t* mmio = rvvm_attach_mmio(machine, &ns16550a);
    if (mmio == NULL) return mmio;

#ifdef USE_FDT
    struct fdt_node* uart_fdt = fdt_node_create_reg("uart", ns16550a.addr);
    fdt_node_add_prop_reg(uart_fdt, "reg", ns16550a.addr, ns16550a.size);
    fdt_node_add_prop_str(uart_fdt, "compatible", "ns16550a");
    fdt_node_add_prop_u32(uart_fdt, "clock-frequency", 4000000);
    fdt_node_add_prop_u32(uart_fdt, "fifo-size", 16);
    fdt_node_add_prop_str(uart_fdt, "status", "okay");
    rvvm_fdt_describe_irq(uart_fdt, intc, irq);
    fdt_node_add_child(rvvm_get_fdt_soc(machine), uart_fdt);
#endif
    return mmio;
}

PUBLIC rvvm_mmio_dev_t* ns16550a_init_auto(rvvm_machine_t* machine, chardev_t* chardev)
{
    rvvm_intc_t* intc = rvvm_get_intc(machine);
    rvvm_addr_t addr = rvvm_mmio_zone_auto(machine, NS16550A_ADDR_DEFAULT, NS16550A_MMIO_SIZE);
    rvvm_mmio_dev_t* mmio = ns16550a_init(machine, chardev, addr, intc, rvvm_alloc_irq(intc));
    if (addr == NS16550A_ADDR_DEFAULT && mmio) {
        rvvm_append_cmdline(machine, "console=ttyS");
#ifdef USE_FDT
        struct fdt_node* chosen = fdt_node_find(rvvm_get_fdt_root(machine), "chosen");
        fdt_node_add_prop_str(chosen, "stdout-path", "/soc/uart@10000000");
#endif
    }
    return mmio;
}

PUBLIC rvvm_mmio_dev_t* ns16550a_init_term_auto(rvvm_machine_t* machine)
{
    return ns16550a_init_auto(machine, chardev_term_create());
}
