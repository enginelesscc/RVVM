/* USB OHCI 1.0 host with three fixed HID ports (keyboard, tablet, mouse).
 * MPL-2.0. Implements control/interrupt transfers; no hubs or isochronous USB.
 * Register and DMA layouts follow the OpenHCI 1.0a specification.
 */
#include "usb-ohci.h"
#include <rvvm/rvvm_pci.h>
#include <util/mem_ops.h>
#include <util/rvtimer.h>
#include <util/spinlock.h>
#include <util/utils.h>
#define PORTS 3
#define WDH   2
#define RHSC  0x40
#define UE    0x10

typedef struct {
    hid_dev_t* hid;
    uint32_t   status;
    uint8_t    address, configured, protocol, idle;
    uint8_t    response[512];
    size_t     length, offset;
    bool       stalled;
    uint8_t    last_report[8];
    size_t     last_length;
} usb_hid_port_t;

struct usb_ohci {
    rvvm_pci_func_t* pci;
    spinlock_t       lock;
    uint32_t         control, command, status, enable, hcca;
    uint32_t         control_head, bulk_head, done, interval, periodic, threshold;
    uint16_t         frame;
    uint64_t         last_ms;
    unsigned         default_port, count;
    usb_hid_port_t   port[PORTS];
};

static void ohci_irq(usb_ohci_t* hc)
{
    rvvm_pci_set_irq(hc->pci, 0, (hc->enable & 0x80000000U) && (hc->status & hc->enable & 0x7fffffff));
}

static bool dma(usb_ohci_t* hc, uint32_t addr, void* buf, size_t len, bool write)
{
    void* ptr = rvvm_pci_get_dma(hc->pci, addr, len);
    if (!ptr) {
        hc->status |= UE;
        return false;
    }
    if (write) {
        memcpy(ptr, buf, len);
    } else {
        memcpy(buf, ptr, len);
    }
    rvvm_pci_end_dma(hc->pci, ptr);
    return true;
}

static uint32_t load32(usb_ohci_t* hc, uint32_t addr)
{
    uint8_t buf[4] = {0};
    dma(hc, addr, buf, 4, false);
    return read_uint32_le(buf);
}

static void store32(usb_ohci_t* hc, uint32_t addr, uint32_t val)
{
    uint8_t buf[4];
    write_uint32_le(buf, val);
    dma(hc, addr, buf, 4, true);
}

static size_t report(usb_hid_port_t* p, uint8_t* out)
{
    uint8_t ignored;
    /* The common HID API includes a two-byte I2C length prefix. */
    p->hid->read_report(p->hid->dev, REPORT_TYPE_INPUT, 0, 0, &ignored);
    size_t n = p->hid->max_input_size - 2;
    for (size_t i = 0; i < n; i++) {
        p->hid->read_report(p->hid->dev, REPORT_TYPE_INPUT, 0, i + 2, out + i);
    }
    return n;
}

static void setup(usb_hid_port_t* p, const uint8_t* s)
{
    uint16_t value = read_uint16_le(s + 2), length = read_uint16_le(s + 6);
    uint8_t  request = s[1];
    p->length = p->offset = 0;
    p->stalled            = false;
    memset(p->response, 0, sizeof(p->response));
    if ((s[0] & 0x60) == 0) {
        switch (request) {
            case 0:
                p->length = 2;
                break; /* GET_STATUS */
            case 1:
            case 3:
                break; /* endpoint halt/remote wake */
            case 5:
                p->address = value & 127;
                break;
            case 6: {
                uint8_t dev[] = {18, 1, 0x10, 1, 0, 0, 0, 8, 0x6b, 0x1d, 0, 0, 0, 1, 0, 0, 0, 1};
                uint8_t cfg[] = {9, 2, 34,   0,    1, 1, 0, 0xa0, 25, 9, 4, 0, 0,    1, 3, 0, 0,
                                 0, 9, 0x21, 0x11, 1, 0, 1, 0x22, 0,  0, 7, 5, 0x81, 3, 8, 0, 10};
                cfg[15]       = p->protocol ? 1 : 0;
                cfg[16]       = p->protocol;
                write_uint16_le(cfg + 25, p->hid->report_desc_size);
                dev[10] = 0x10 + p->protocol;
                switch (value >> 8) {
                    case 1:
                        memcpy(p->response, dev, sizeof(dev));
                        p->length = sizeof(dev);
                        break;
                    case 2:
                        memcpy(p->response, cfg, sizeof(cfg));
                        p->length = sizeof(cfg);
                        break;
                    case 0x21:
                        memcpy(p->response, cfg + 18, 9);
                        p->length = 9;
                        break;
                    case 0x22:
                        p->length = p->hid->report_desc_size;
                        if (p->length > sizeof(p->response)) {
                            p->length = sizeof(p->response);
                        }
                        memcpy(p->response, p->hid->report_desc, p->length);
                        break;
                    case 3:
                        p->response[0] = 4;
                        p->response[1] = 3;
                        p->response[2] = 9;
                        p->response[3] = 4;
                        p->length      = 4;
                        break;
                    default:
                        p->stalled = true;
                }
                break;
            }
            case 8:
                p->response[0] = p->configured;
                p->length      = 1;
                break;
            case 9:
                p->configured = value;
                rvvm_info("USB HID configured (address %u, protocol %u)", p->address, p->protocol);
                break;
            case 10:
                p->length = 1;
                break;
            case 11:
                break;
            default:
                p->stalled = true;
        }
    } else if ((s[0] & 0x60) == 0x20) {
        switch (request) {
            case 1:
                p->length = report(p, p->response);
                break;
            case 2:
                p->response[0] = p->idle;
                p->length      = 1;
                break;
            case 3:
                p->response[0] = 1;
                p->length      = 1;
                break;
            case 9:
            case 11:
                break;
            case 10:
                p->idle = value >> 8;
                break;
            default:
                p->stalled = true;
        }
    } else {
        p->stalled = true;
    }
    if (p->length > length) {
        p->length = length;
    }
}

/* A general TD buffer can cross into a non-contiguous second 4K page. */
static bool td_buffer(usb_ohci_t* hc, uint32_t cbp, uint32_t end, uint8_t* buf, size_t n, bool write)
{
    size_t first = 4096 - (cbp & 4095);
    if (first > n) {
        first = n;
    }
    if (first && !dma(hc, cbp, buf, first, write)) {
        return false;
    }
    return n == first || dma(hc, end & ~4095U, buf + first, n - first, write);
}

static usb_hid_port_t* device(usb_ohci_t* hc, unsigned address)
{
    if (!address) {
        return &hc->port[hc->default_port];
    }
    for (unsigned i = 0; i < hc->count; i++) {
        if (hc->port[i].address == address && (hc->port[i].status & 2)) {
            return &hc->port[i];
        }
    }
    return NULL;
}

static void process_list(usb_ohci_t* hc, uint32_t edaddr)
{
    /* Bound both guest-controlled lists, including cycles. */
    unsigned budget = 256;
    while ((edaddr &= ~15U) && budget--) {
        uint32_t ed[4];
        uint8_t  raw[16];
        if (!dma(hc, edaddr, raw, 16, false)) {
            return;
        }
        for (unsigned i = 0; i < 4; i++) {
            ed[i] = read_uint32_le(raw + 4 * i);
        }
        if (!(ed[0] & 0xc000) && !(ed[2] & 1)) {
            unsigned tds = 32;
            while ((ed[2] & ~15U) != (ed[1] & ~15U) && tds--) {
                uint32_t tdaddr = ed[2] & ~15U, td[4];
                if (!tdaddr || !dma(hc, tdaddr, raw, 16, false)) {
                    return;
                }
                for (unsigned i = 0; i < 4; i++) {
                    td[i] = read_uint32_le(raw + 4 * i);
                }
                unsigned direction = (ed[0] >> 11) & 3;
                if (!direction) {
                    direction = (td[0] >> 19) & 3;
                }
                size_t   n = 0, actual = 0;
                unsigned cc = 0;
                if (td[1]) {
                    n = ((td[1] & ~4095U) == (td[3] & ~4095U)) ? (td[3] - td[1] + 1U)
                                                               : (4096 - (td[1] & 4095)) + (td[3] & 4095) + 1;
                }
                uint8_t         buf[8192];
                usb_hid_port_t* p = device(hc, ed[0] & 127);
                if (!p || !p->hid) {
                    cc = 5;
                } else if (n > sizeof(buf)) {
                    cc = 8;
                } else if (direction == 0) {
                    if (n != 8 || !td_buffer(hc, td[1], td[3], buf, n, false)) {
                        cc = 4;
                    } else {
                        setup(p, buf);
                        actual = 8;
                    }
                } else if (p->stalled) {
                    cc = 4;
                } else if (direction == 2 && n) {
                    if ((ed[0] >> 7) & 15) {
                        if (!p->configured) {
                            break; /* NAK */
                        }
                        actual      = report(p, buf);
                        bool motion = p->protocol == 2 && (buf[1] || buf[2] || buf[3]);
                        if (!motion && actual == p->last_length && !memcmp(buf, p->last_report, actual)) {
                            break;
                        }
                        memcpy(p->last_report, buf, actual);
                        p->last_length = actual;
                        if (actual > n) {
                            actual = n;
                        }
                    } else {
                        actual = p->length - p->offset;
                        if (actual > n) {
                            actual = n;
                        }
                        memcpy(buf, p->response + p->offset, actual);
                        p->offset += actual;
                    }
                    if (!td_buffer(hc, td[1], td[3], buf, actual, true)) {
                        cc = 5;
                    }
                    if (actual < n && !(td[0] & (1U << 18))) {
                        cc = 9;
                    }
                } else {
                    actual = n;
                } /* status or HID LED output */
                uint32_t next = td[2] & ~15U;
                td[0]         = (td[0] & 0x03ffffffU) | (cc << 28);
                if (actual == n) {
                    td[1] = 0;
                } else if (actual) {
                    size_t first = 4096 - (td[1] & 4095);
                    td[1]        = actual < first ? td[1] + actual : (td[3] & ~4095U) + (actual - first);
                }
                td[2]    = hc->done;
                hc->done = tdaddr;
                for (unsigned i = 0; i < 4; i++) {
                    write_uint32_le(raw + 4 * i, td[i]);
                }
                if (!dma(hc, tdaddr, raw, 16, true)) {
                    return;
                }
                ed[2] = next | ((ed[2] ^ 2) & 2) | (cc ? 1 : 0);
                store32(hc, edaddr + 8, ed[2]);
                if (cc) {
                    break;
                }
            }
        }
        edaddr = ed[3];
    }
}

static void frame(usb_ohci_t* hc)
{
    hc->frame++;
    if ((hc->control & 0xc0) != 0x80 || !hc->hcca) {
        return;
    }
    if (hc->control & 0x10) {
        process_list(hc, hc->control_head);
    }
    if (hc->control & 0x20) {
        process_list(hc, hc->bulk_head);
    }
    if (hc->control & 4) {
        process_list(hc, load32(hc, hc->hcca + 4 * (hc->frame & 31)));
    }
    uint8_t number[2];
    write_uint16_le(number, hc->frame);
    dma(hc, hc->hcca + 0x80, number, 2, true);
    if (hc->done && !(hc->status & WDH)) {
        store32(hc, hc->hcca + 0x84, hc->done | ((hc->status & hc->enable & ~WDH) ? 1 : 0));
        hc->done    = 0;
        hc->status |= WDH;
    }
    hc->status |= 4; /* SOF */
    if (!hc->frame) {
        hc->status |= 0x20;
    }
    ohci_irq(hc);
}

static void poll(rvvm_reg_dev_t* dev)
{
    usb_ohci_t* hc = rvvm_region_data(dev);
    scoped_spin_lock (&hc->lock) {
        uint64_t now = rvtimer_clocksource(1000), elapsed = now - hc->last_ms;
        if (elapsed > 32) {
            elapsed = 32;
        }
        while (elapsed--) {
            frame(hc);
        }
        hc->last_ms = now;
    }
}

static void reset_state(usb_ohci_t* hc)
{
    hc->control = hc->command = hc->status = hc->enable = hc->hcca = 0;
    hc->control_head = hc->bulk_head = hc->done = hc->frame = 0;
    hc->interval                                            = 0x2edf;
    hc->periodic                                            = 0;
    hc->threshold                                           = 0x628;
    hc->last_ms                                             = rvtimer_clocksource(1000);
    for (unsigned i = 0; i < hc->count; i++) {
        hc->port[i].status  = 0x101;
        hc->port[i].address = hc->port[i].configured = 0;
    }
    ohci_irq(hc);
}

static void reset(rvvm_reg_dev_t* dev)
{
    reset_state(rvvm_region_data(dev));
}

static void read_reg(rvvm_reg_dev_t* dev, void* data, size_t size, size_t off)
{
    usb_ohci_t* hc = rvvm_region_data(dev);
    uint32_t    v  = 0;
    UNUSED(size);
    scoped_spin_lock (&hc->lock) {
        switch (off) {
            case 0:
                v = 0x10;
                break;
            case 4:
                v = hc->control;
                break;
            case 8:
                v = hc->command;
                break;
            case 0xc:
                v = hc->status;
                break;
            case 0x10:
            case 0x14:
                v = hc->enable;
                break;
            case 0x18:
                v = hc->hcca;
                break;
            case 0x20:
                v = hc->control_head;
                break;
            case 0x28:
                v = hc->bulk_head;
                break;
            case 0x30:
                v = hc->done;
                break;
            case 0x34:
                v = hc->interval;
                break;
            case 0x38:
                v = hc->interval & 0x3fff;
                break;
            case 0x3c:
                v = hc->frame;
                break;
            case 0x40:
                v = hc->periodic;
                break;
            case 0x44:
                v = hc->threshold;
                break;
            case 0x48:
                v = 0x02000200 | hc->count;
                break; /* no power switching, 4 ms power delay */
            case 0x4c:
                v = 0;
                break;
            case 0x50:
                v = 0;
                break;
            default:
                if (off >= 0x54 && off < 0x54 + 4 * hc->count) {
                    v = hc->port[(off - 0x54) / 4].status;
                }
        }
    }
    write_uint32_le(data, v);
}

static void write_reg(rvvm_reg_dev_t* dev, const void* data, size_t size, size_t off)
{
    usb_ohci_t* hc = rvvm_region_data(dev);
    uint32_t    v  = read_uint32_le(data);
    UNUSED(size);
    scoped_spin_lock (&hc->lock) {
        switch (off) {
            case 4:
                hc->control = v & 0x7ff;
                break;
            case 8:
                if (v & 1) {
                    reset_state(hc);
                } else {
                    hc->command |= v & 6;
                }
                break;
            case 0xc:
                hc->status &= ~v;
                break;
            case 0x10:
                hc->enable |= v;
                break;
            case 0x14:
                hc->enable &= ~v;
                break;
            case 0x18:
                hc->hcca = v & ~255U;
                break;
            case 0x20:
                hc->control_head = v & ~15U;
                break;
            case 0x28:
                hc->bulk_head = v & ~15U;
                break;
            case 0x34:
                hc->interval = v;
                break;
            case 0x40:
                hc->periodic = v;
                break;
            case 0x44:
                hc->threshold = v;
                break;
            default:
                if (off >= 0x54 && off < 0x54 + 4 * hc->count) {
                    unsigned        i  = (off - 0x54) / 4;
                    usb_hid_port_t* p  = &hc->port[i];
                    p->status         &= ~(v & 0x1f0000);
                    if (v & 1) {
                        p->status &= ~2U;
                    }
                    if (v & 2) {
                        p->status |= 2;
                    }
                    if (v & 4) {
                        p->status |= 4;
                    }
                    if (v & 8) {
                        p->status &= ~4U;
                        p->status |= 0x40000;
                    }
                    if (v & 0x100) {
                        p->status |= 0x100;
                    }
                    if (v & 0x10) {
                        hc->default_port = i;
                        p->address = p->configured = 0;
                        p->status                  = (p->status & ~0x14U) | 0x100103;
                        if (p->hid->reset) {
                            p->hid->reset(p->hid->dev);
                        }
                    }
                    if (p->status & 0x1f0000) {
                        hc->status |= RHSC;
                    }
                }
        }
        ohci_irq(hc);
    }
}

static void cleanup(rvvm_reg_dev_t* dev)
{
    usb_ohci_t* hc = rvvm_region_data(dev);
    /* Tablet shares ownership with mouse; visit it before its owning port. */
    for (unsigned i = 0; i < hc->count; i++) {
        if (hc->port[i].hid->remove) {
            hc->port[i].hid->remove(hc->port[i].hid->dev);
        }
    }
    free(hc);
}

static const rvvm_reg_type_t type = {.name     = "usb-ohci-hid",
                                     .read     = read_reg,
                                     .write    = write_reg,
                                     .poll     = poll,
                                     .reset    = reset,
                                     .cleanup  = cleanup,
                                     .min_size = 4,
                                     .max_size = 4};

usb_ohci_t* usb_ohci_init(rvvm_machine_t* machine)
{
    usb_ohci_t*          hc   = safe_new_obj(usb_ohci_t);
    rvvm_reg_desc_t      bar  = {.size = 4096, .data = hc, .type = &type};
    rvvm_pci_func_desc_t desc = {.vendor_id  = 0x106b,
                                 .device_id  = 0x003f,
                                 .class_code = 0x0c03,
                                 .prog_iface = 0x10,
                                 .irq_pin    = RVVM_PCI_PIN_INTA,
                                 .attr       = RVVM_PCI_ATTR_LEGACY,
                                 .bar[0]     = &bar};
    rvvm_pci_func_t*     pci  = rvvm_pci_func_init(machine, &desc, RVVM_PCI_ADDR_ANY);
    if (!pci) {
        return NULL;
    }
    hc->pci = pci;
    reset_state(hc);
    return hc;
}

static void input_available(void* host, uint8_t id)
{
    UNUSED(host);
    UNUSED(id);
}

void usb_ohci_attach_hid(usb_ohci_t* hc, hid_dev_t* hid, uint8_t protocol)
{
    if (hc->count >= PORTS) {
        rvvm_fatal("USB HID ports exhausted");
    }
    usb_hid_port_t* p    = &hc->port[hc->count++];
    p->hid               = hid;
    p->protocol          = protocol;
    p->status            = 0x101;
    hid->host            = p;
    hid->input_available = input_available;
}
