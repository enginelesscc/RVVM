/*
rtc-goldfish.c - Goldfish Real-time Clock
Copyright (C) 2021  LekKit <github.com/LekKit>

This Source Code Form is subject to the terms of the Mozilla Public
License, v. 2.0. If a copy of the MPL was not distributed with this
file, You can obtain one at https://mozilla.org/MPL/2.0/.
*/

#include "rtc-goldfish.h"
#include "fdtlib.h"
#include "mem_ops.h"
#include "utils.h"

#include <time.h>

#define RTC_TIME_LOW     0x0
#define RTC_TIME_HIGH    0x4
#define RTC_ALARM_LOW    0x8
#define RTC_ALARM_HIGH   0xC
#define RTC_IRQ_ENABLED  0x10
#define RTC_ALARM_CLEAR  0x14
#define RTC_ALARM_STATUS 0x18
#define RTC_IRQ_CLEAR    0x1C

#define RTC_GOLDFISH_REG_SIZE 0x1000

typedef struct {
    rvvm_intc_t* intc;
    rvvm_irq_t irq;

    uint32_t time_low;
    uint32_t time_high;
    uint32_t alarm_low;
    uint32_t alarm_high;
    uint32_t alarm_enabled;
    uint32_t irq_enabled;
} rtc_goldfish_dev_t;

static void rtc_goldfish_update(rtc_goldfish_dev_t* rtc)
{
    uint64_t timer64 = time(NULL) * 1000000000ULL;
    atomic_store_uint32_relax(&rtc->time_low, timer64);
    atomic_store_uint32_relax(&rtc->time_high, timer64 >> 32);

    if (atomic_load_uint32_relax(&rtc->alarm_enabled) && atomic_load_uint32_relax(&rtc->irq_enabled)) {
        uint64_t alarm64 = atomic_load_uint32_relax(&rtc->alarm_low);
        alarm64 |= (((uint64_t)atomic_load_uint32_relax(&rtc->alarm_high)) << 32);
        if (timer64 >= alarm64) {
            rvvm_raise_irq(rtc->intc, rtc->irq);
        }
    } else {
        rvvm_lower_irq(rtc->intc, rtc->irq);
    }
}

static bool rtc_goldfish_mmio_read(rvvm_mmio_dev_t* dev, void* data, uint64_t offset, uint8_t size)
{
    rtc_goldfish_dev_t* rtc = dev->data;
    uint32_t val = 0;
    UNUSED(size);

    switch (offset) {
        case RTC_TIME_LOW:
            rtc_goldfish_update(rtc);
            val = atomic_load_uint32_relax(&rtc->time_low);
            break;
        case RTC_TIME_HIGH:
            val = atomic_load_uint32_relax(&rtc->time_high);
            break;
        case RTC_ALARM_LOW:
            val = atomic_load_uint32_relax(&rtc->alarm_low);
            break;
        case RTC_ALARM_HIGH:
            val = atomic_load_uint32_relax(&rtc->alarm_high);
            break;
        case RTC_IRQ_ENABLED:
            val = atomic_load_uint32_relax(&rtc->irq_enabled);
            break;
        case RTC_ALARM_STATUS:
            val = atomic_load_uint32_relax(&rtc->alarm_enabled);
            break;
    }

    write_uint32_le(data, val);
    return true;
}

static bool rtc_goldfish_mmio_write(rvvm_mmio_dev_t* dev, void* data, uint64_t offset, uint8_t size)
{
    rtc_goldfish_dev_t* rtc = dev->data;
    uint32_t val = read_uint32_le(data);
    UNUSED(size);

    switch (offset) {
        case RTC_ALARM_LOW:
            atomic_store_uint32_relax(&rtc->alarm_enabled, true);
            atomic_store_uint32_relax(&rtc->alarm_low, val);
            rtc_goldfish_update(rtc);
            break;
        case RTC_ALARM_HIGH:
            atomic_store_uint32_relax(&rtc->alarm_high, val);
            break;
        case RTC_IRQ_ENABLED:
            atomic_store_uint32_relax(&rtc->irq_enabled, val & 1);
            rtc_goldfish_update(rtc);
            break;
        case RTC_ALARM_CLEAR:
            atomic_store_uint32_relax(&rtc->alarm_enabled, false);
            rtc_goldfish_update(rtc);
            break;
        case RTC_IRQ_CLEAR:
            rtc_goldfish_update(rtc);
            break;
    }

    return true;
}

static rvvm_mmio_type_t rtc_goldfish_dev_type = {
    .name = "rtc_goldfish",
};

PUBLIC rvvm_mmio_dev_t* rtc_goldfish_init(rvvm_machine_t* machine, rvvm_addr_t addr,
                                          rvvm_intc_t* intc, rvvm_irq_t irq)
{
    rtc_goldfish_dev_t* rtc = safe_new_obj(rtc_goldfish_dev_t);
    rtc->intc = intc;
    rtc->irq = irq;

    rvvm_mmio_dev_t rtc_mmio = {
        .addr = addr,
        .size = RTC_GOLDFISH_REG_SIZE,
        .data = rtc,
        .type = &rtc_goldfish_dev_type,
        .read = rtc_goldfish_mmio_read,
        .write = rtc_goldfish_mmio_write,
        .min_op_size = 4,
        .max_op_size = 4,
    };
    rvvm_mmio_dev_t* mmio = rvvm_attach_mmio(machine, &rtc_mmio);
    if (mmio == NULL) return mmio;
#ifdef USE_FDT
    struct fdt_node* rtc_fdt = fdt_node_create_reg("rtc", rtc_mmio.addr);
    fdt_node_add_prop_reg(rtc_fdt, "reg", rtc_mmio.addr, rtc_mmio.size);
    fdt_node_add_prop_str(rtc_fdt, "compatible", "google,goldfish-rtc");
    rvvm_fdt_describe_irq(rtc_fdt, intc, irq);
    fdt_node_add_child(rvvm_get_fdt_soc(machine), rtc_fdt);
#endif
    return mmio;
}

PUBLIC rvvm_mmio_dev_t* rtc_goldfish_init_auto(rvvm_machine_t* machine)
{
    rvvm_intc_t* intc = rvvm_get_intc(machine);
    rvvm_addr_t addr = rvvm_mmio_zone_auto(machine, RTC_GOLDFISH_ADDR_DEFAULT, RTC_GOLDFISH_REG_SIZE);
    return rtc_goldfish_init(machine, addr, intc, rvvm_alloc_irq(intc));
}
