/*
gpio-sifive.c - SiFive GPIO Controller
Copyright (C) 2024  LekKit <github.com/LekKit>

This Source Code Form is subject to the terms of the Mozilla Public
License, v. 2.0. If a copy of the MPL was not distributed with this
file, You can obtain one at https://mozilla.org/MPL/2.0/.
*/

#include "gpio-sifive.h"
#include "fdtlib.h"
#include "vector.h"
#include "utils.h"
#include "mem_ops.h"

// See https://static.dev.sifive.com/FU540-C000-v1.0.pdf

#define GPIO_SIFIVE_REG_INPUT     0x00 // Pin input value
#define GPIO_SIFIVE_REG_INPUT_EN  0x04 // Pin input enable
#define GPIO_SIFIVE_REG_OUTPUT_EN 0x08 // Pin output enable
#define GPIO_SIFIVE_REG_OUTPUT    0x0C // Pin output value
#define GPIO_SIFIVE_REG_PUE       0x10 // Pull-up enable
#define GPIO_SIFIVE_REG_DS        0x14 // Drive strength
#define GPIO_SIFIVE_REG_RISE_IE   0x18 // Rise interrupt enable
#define GPIO_SIFIVE_REG_RISE_IP   0x1C // Rise interrupt pending
#define GPIO_SIFIVE_REG_FALL_IE   0x20 // Rise interrupt enable
#define GPIO_SIFIVE_REG_FALL_IP   0x24 // Fall interrupt pending
#define GPIO_SIFIVE_REG_HIGH_IE   0x28 // High interrupt enable
#define GPIO_SIFIVE_REG_HIGH_IP   0x2C // High interrupt pending
#define GPIO_SIFIVE_REG_LOW_IE    0x30 // Low interrupt enable
#define GPIO_SIFIVE_REG_LOW_IP    0x34 // Low interrupt pending
#define GPIO_SIFIVE_REG_OUT_XOR   0x40 // Output XOR (Invert)

#define GPIO_SIFIVE_MMIO_SIZE 0x1000

typedef struct {
    rvvm_gpio_dev_t* gpio;
    rvvm_intc_t* intc;
    rvvm_irq_t irq_pins[GPIO_SIFIVE_PINS];

    // Cache IRQ lines state
    uint32_t irqs;

    // Input pins
    uint32_t pins;

    // Controller registers
    uint32_t input_en;
    uint32_t output_en;
    uint32_t output;
    uint32_t pue;
    uint32_t ds;
    uint32_t rise_ie;
    uint32_t rise_ip;
    uint32_t fall_ie;
    uint32_t fall_ip;
    uint32_t high_ie;
    uint32_t high_ip;
    uint32_t low_ie;
    uint32_t low_ip;
    uint32_t out_xor;
} gpio_sifive_dev_t;

static void gpio_sifive_update_irqs(gpio_sifive_dev_t* bus)
{
    // Combine pin IRQs
    uint32_t ip = (atomic_load_uint32_relax(&bus->rise_ip) & atomic_load_uint32_relax(&bus->rise_ie))
                | (atomic_load_uint32_relax(&bus->fall_ip) & atomic_load_uint32_relax(&bus->fall_ie))
                | (atomic_load_uint32_relax(&bus->high_ip) & atomic_load_uint32_relax(&bus->high_ie))
                | (atomic_load_uint32_relax(&bus->low_ip)  & atomic_load_uint32_relax(&bus->low_ie));

    // Update IRQ pins
    if (atomic_swap_uint32(&bus->irqs, ip) != ip) {
        for (uint64_t i = 0; i < GPIO_SIFIVE_PINS; ++i) {
            if (ip & (1U << i)) {
                rvvm_raise_irq(bus->intc, bus->irq_pins[i]);
            } else {
                rvvm_lower_irq(bus->intc, bus->irq_pins[i]);
            }
        }
    }
}

static void gpio_sifive_update_pins(gpio_sifive_dev_t* bus, uint32_t pins)
{
    uint32_t enable = atomic_load_uint32_relax(&bus->input_en);
    uint32_t old_pins = atomic_swap_uint32(&bus->pins, pins);
    if ((pins ^ old_pins) & enable) {
        uint32_t pins_rise = (pins & ~old_pins) & enable;
        uint32_t pins_fall = (~pins & old_pins) & enable;
        if (pins_rise) {
            atomic_or_uint32(&bus->rise_ip, pins_rise);
            atomic_or_uint32(&bus->high_ip, pins_rise);
        }
        if (pins_fall) {
            atomic_or_uint32(&bus->fall_ip, pins_fall);
            atomic_or_uint32(&bus->low_ip,  pins_fall);
        }
        gpio_sifive_update_irqs(bus);
    }
}

static void gpio_sifive_update_out(gpio_sifive_dev_t* bus)
{
    uint32_t out = atomic_load_uint32_relax(&bus->output);
    out &= atomic_load_uint32_relax(&bus->output_en);
    out ^= atomic_load_uint32_relax(&bus->out_xor);
    gpio_pins_out(bus->gpio, 0, out);
}

static bool gpio_sifive_pins_in(rvvm_gpio_dev_t* gpio, uint64_t off, uint32_t pins)
{
    if (off == 0) {
        gpio_sifive_dev_t* bus = gpio->io_dev;
        gpio_sifive_update_pins(bus, pins);
        return true;
    }
    return false;
}

static uint32_t gpio_sifive_pins_read(rvvm_gpio_dev_t* gpio, uint64_t off)
{
    if (off == 0) {
        gpio_sifive_dev_t* bus = gpio->io_dev;
        uint32_t out = atomic_load_uint32_relax(&bus->output);
        out &= atomic_load_uint32_relax(&bus->output_en);
        out ^= atomic_load_uint32_relax(&bus->out_xor);
        return out;
    }
    return 0;
}

static bool gpio_sifive_mmio_read(rvvm_mmio_dev_t* dev, void* data, uint64_t offset, uint8_t size)
{
    gpio_sifive_dev_t* bus = dev->data;
    uint32_t val = 0;
    UNUSED(size);

    switch (offset) {
        case GPIO_SIFIVE_REG_INPUT:
            val = atomic_load_uint32_relax(&bus->pins) & atomic_load_uint32_relax(&bus->input_en);
            break;
        case GPIO_SIFIVE_REG_INPUT_EN:
            val = atomic_load_uint32_relax(&bus->input_en);
            break;
        case GPIO_SIFIVE_REG_OUTPUT_EN:
            val = atomic_load_uint32_relax(&bus->output_en);
            break;
        case GPIO_SIFIVE_REG_OUTPUT:
            val = atomic_load_uint32_relax(&bus->output);
            break;
        case GPIO_SIFIVE_REG_PUE:
            val = atomic_load_uint32_relax(&bus->pue);
            break;
        case GPIO_SIFIVE_REG_DS:
            val = atomic_load_uint32_relax(&bus->ds);
            break;
        case GPIO_SIFIVE_REG_RISE_IE:
            val = atomic_load_uint32_relax(&bus->rise_ie);
            break;
        case GPIO_SIFIVE_REG_RISE_IP:
            val = atomic_load_uint32_relax(&bus->rise_ip);
            break;
        case GPIO_SIFIVE_REG_FALL_IE:
            val = atomic_load_uint32_relax(&bus->fall_ie);
            break;
        case GPIO_SIFIVE_REG_FALL_IP:
            val = atomic_load_uint32_relax(&bus->fall_ip);
            break;
        case GPIO_SIFIVE_REG_HIGH_IE:
            val = atomic_load_uint32_relax(&bus->high_ie);
            break;
        case GPIO_SIFIVE_REG_HIGH_IP:
            val = atomic_load_uint32_relax(&bus->high_ip);
            break;
        case GPIO_SIFIVE_REG_LOW_IE:
            val = atomic_load_uint32_relax(&bus->low_ie);
            break;
        case GPIO_SIFIVE_REG_LOW_IP:
            val = atomic_load_uint32_relax(&bus->low_ip);
            break;
        case GPIO_SIFIVE_REG_OUT_XOR:
            val = atomic_load_uint32_relax(&bus->out_xor);
            break;
    }

    write_uint32_le(data, val);
    return true;
}

static bool gpio_sifive_mmio_write(rvvm_mmio_dev_t* dev, void* data, uint64_t offset, uint8_t size)
{
    gpio_sifive_dev_t* bus = dev->data;
    uint32_t val = read_uint32_le(data);
    UNUSED(size);

    switch (offset) {
        case GPIO_SIFIVE_REG_INPUT_EN:
            atomic_store_uint32_relax(&bus->input_en, val);
            gpio_sifive_update_pins(bus, atomic_load_uint32_relax(&bus->pins));
            break;
        case GPIO_SIFIVE_REG_OUTPUT_EN:
            atomic_store_uint32_relax(&bus->output_en, val);
            gpio_sifive_update_out(bus);
            break;
        case GPIO_SIFIVE_REG_OUTPUT:
            atomic_store_uint32_relax(&bus->output, val);
            gpio_sifive_update_out(bus);
            break;
        case GPIO_SIFIVE_REG_PUE:
            atomic_store_uint32_relax(&bus->pue, val);
            break;
        case GPIO_SIFIVE_REG_DS:
            atomic_store_uint32_relax(&bus->ds, val);
            break;
        case GPIO_SIFIVE_REG_RISE_IE:
            atomic_store_uint32_relax(&bus->rise_ie, val);
            gpio_sifive_update_irqs(bus);
            break;
        case GPIO_SIFIVE_REG_RISE_IP:
            atomic_and_uint32(&bus->rise_ip, ~val);
            gpio_sifive_update_irqs(bus);
            break;
        case GPIO_SIFIVE_REG_FALL_IE:
            atomic_store_uint32_relax(&bus->fall_ie, val);
            gpio_sifive_update_irqs(bus);
            break;
        case GPIO_SIFIVE_REG_FALL_IP:
            atomic_and_uint32(&bus->fall_ip, ~val);
            gpio_sifive_update_irqs(bus);
            break;
        case GPIO_SIFIVE_REG_HIGH_IE:
            atomic_store_uint32_relax(&bus->high_ie, val);
            gpio_sifive_update_irqs(bus);
            break;
        case GPIO_SIFIVE_REG_HIGH_IP:
            atomic_and_uint32(&bus->high_ip, ~val);
            gpio_sifive_update_irqs(bus);
            break;
        case GPIO_SIFIVE_REG_LOW_IE:
            atomic_store_uint32_relax(&bus->low_ie, val);
            gpio_sifive_update_irqs(bus);
            break;
        case GPIO_SIFIVE_REG_LOW_IP:
            atomic_and_uint32(&bus->low_ip, ~val);
            gpio_sifive_update_irqs(bus);
            break;
        case GPIO_SIFIVE_REG_OUT_XOR:
            atomic_store_uint32_relax(&bus->out_xor, val);
            gpio_sifive_update_out(bus);
            break;
    }

    return true;
}

static void gpio_sifive_remove(rvvm_mmio_dev_t* dev)
{
    gpio_sifive_dev_t* bus = dev->data;
    gpio_free(bus->gpio);
    free(bus);
}

static void gpio_sifive_update(rvvm_mmio_dev_t* dev)
{
    gpio_sifive_dev_t* bus = dev->data;
    gpio_update(bus->gpio);
}

static rvvm_mmio_type_t gpio_sifive_dev_type = {
    .name = "gpio_sifive",
    .remove = gpio_sifive_remove,
    .update = gpio_sifive_update,
};

PUBLIC rvvm_mmio_dev_t* gpio_sifive_init(rvvm_machine_t* machine, rvvm_gpio_dev_t* gpio,
                                         rvvm_addr_t base_addr, rvvm_intc_t* intc, const rvvm_irq_t* irqs)
{
    gpio_sifive_dev_t* bus = safe_new_obj(gpio_sifive_dev_t);
    bus->gpio = gpio;
    bus->intc = intc;

    // Amount of IRQs controlls amount of GPIO pins
    // Each GPIO pin should have a unique IRQ!
    for (uint64_t i = 0; i < GPIO_SIFIVE_PINS; ++i) {
        bus->irq_pins[i] = irqs[i];
    }

    if (gpio) {
        gpio->io_dev = bus;
        gpio->pins_in = gpio_sifive_pins_in;
        gpio->pins_read = gpio_sifive_pins_read;
    }

    rvvm_mmio_dev_t gpio_sifive = {
        .addr = base_addr,
        .size = GPIO_SIFIVE_MMIO_SIZE,
        .data = bus,
        .type = &gpio_sifive_dev_type,
        .read = gpio_sifive_mmio_read,
        .write = gpio_sifive_mmio_write,
        .min_op_size = 4,
        .max_op_size = 4,
    };

    rvvm_mmio_dev_t* mmio = rvvm_attach_mmio(machine, &gpio_sifive);
    if (mmio == NULL) return mmio;

#ifdef USE_FDT
    vector_t(uint32_t) irq_cells = {0};
    for (uint64_t i = 0; i < GPIO_SIFIVE_PINS; ++i) {
        uint32_t cells[8] = {0};
        uint64_t count = rvvm_fdt_irq_cells(intc, irqs[i], cells, STATIC_ARRAY_SIZE(cells));
        for (uint64_t cell = 0; cell < count; ++cell) {
            vector_push_back(irq_cells, cells[cell]);
        }
    }

    struct fdt_node* gpio_fdt = fdt_node_create_reg("gpio", gpio_sifive.addr);
    fdt_node_add_prop_reg(gpio_fdt, "reg", gpio_sifive.addr, gpio_sifive.size);
    fdt_node_add_prop_str(gpio_fdt, "compatible", "sifive,gpio0");
    fdt_node_add_prop_u32(gpio_fdt, "interrupt-parent", rvvm_fdt_intc_phandle(intc));
    fdt_node_add_prop_cells(gpio_fdt, "interrupts", vector_buffer(irq_cells), vector_size(irq_cells));
    fdt_node_add_prop(gpio_fdt, "gpio-controller", NULL, 0);
    fdt_node_add_prop_u32(gpio_fdt, "#gpio-cells", 2);
    fdt_node_add_prop(gpio_fdt, "interrupt-controller", NULL, 0);
    fdt_node_add_prop_u32(gpio_fdt, "#interrupt-cells", 2);
    fdt_node_add_prop_u32(gpio_fdt, "ngpios", GPIO_SIFIVE_PINS);
    fdt_node_add_prop_str(gpio_fdt, "status", "okay");
    fdt_node_add_child(rvvm_get_fdt_soc(machine), gpio_fdt);

    vector_free(irq_cells);
#endif
    return mmio;
}

PUBLIC rvvm_mmio_dev_t* gpio_sifive_init_auto(rvvm_machine_t* machine, rvvm_gpio_dev_t* gpio)
{
    rvvm_intc_t* intc = rvvm_get_intc(machine);
    rvvm_addr_t addr = rvvm_mmio_zone_auto(machine, GPIO_SIFIVE_ADDR_DEFAULT, GPIO_SIFIVE_MMIO_SIZE);
    uint32_t irqs[GPIO_SIFIVE_PINS] = {0};
    for (uint64_t i = 0; i < GPIO_SIFIVE_PINS; ++i) {
        irqs[i] = rvvm_alloc_irq(intc);
    }
    return gpio_sifive_init(machine, gpio, addr, intc, irqs);
}
