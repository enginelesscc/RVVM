/*
 * Intel 82574L compatible Ethernet controller.
 * SPDX-License-Identifier: MPL-2.0
 *
 * A single queue, with legacy/extended descriptors and IPv4 TCP segmentation.
 * MMIO only changes registers and wakes a worker. No socket calls or descriptor
 * walks run on a vCPU. The lock protects device state, never host network I/O.
 */
#ifdef USE_NET
#include "e1000e.h"
#include "mem_ops.h"
#include "spinlock.h"
#include "threading.h"
#include "utils.h"
#include <rvvm/rvvm_snapshot.h>

#define E1K_BAR_SIZE 0x20000
#define E1K_TX_SIZE  65536
#define CTRL         0x0000
#define STATUS       0x0008
#define EECD         0x0010
#define EERD         0x0014
#define CTRL_EXT     0x0018
#define MDIC         0x0020
#define ICR          0x00C0
#define ICS          0x00C8
#define IMS          0x00D0
#define IMC          0x00D8
#define IAM          0x00E0
#define RCTL         0x0100
#define TCTL         0x0400
#define RDBAL        0x2800
#define RDBAH        0x2804
#define RDLEN        0x2808
#define RDH          0x2810
#define RDT          0x2818
#define TDBAL        0x3800
#define TDBAH        0x3804
#define TDLEN        0x3808
#define TDH          0x3810
#define TDT          0x3818
#define RFCTL        0x5008
#define RAL          0x5400
#define RAH          0x5404
#define IRQ_TXDW     0x0001
#define IRQ_TXQE     0x0002
#define IRQ_LSC      0x0004
#define IRQ_RXO      0x0040
#define IRQ_RXT0     0x0080
#define REG(n, r)    ((n)->regs[(r) / 4])

typedef struct {
    rvvm_pci_func_t* pci;
    tap_dev_t*       tap;
    spinlock_t       lock;
    rvvm_thread_t*   worker;
    rvvm_event_t     wake;
    uint32_t         stop;
    uint32_t         regs[E1K_BAR_SIZE / 4];
    uint16_t         nvm[64];
    uint16_t         phy[32];
    uint8_t          context[16];
    uint8_t          packet[E1K_TX_SIZE];
    uint32_t         packet_size;
    uint32_t         ioaddr;
    unsigned         cleanup;
    bool             discard;
    bool             suspended;
} e1000e_t;

static void e1k_irq(e1000e_t* n, uint32_t cause)
{
    REG(n, ICR) |= cause & 0x7FFFFFFF;
    rvvm_pci_set_irq(n->pci, 0, (REG(n, ICR) & REG(n, IMS)) != 0);
}

static uint16_t e1k_checksum(const uint8_t* p, size_t len)
{
    uint32_t sum = 0;
    for (; len > 1; len -= 2, p += 2) {
        sum += read_uint16_be_m(p);
    }
    if (len) {
        sum += (uint32_t)*p << 8;
    }
    while (sum >> 16) {
        sum = (sum & 0xFFFF) + (sum >> 16);
    }
    return ~sum;
}

static void e1k_insert_checksum(uint8_t* p, size_t len, size_t start, size_t off, size_t end)
{
    if (!end) {
        end = len - 1;
    }
    if (start <= end && end < len && off + 1 < len) {
        write_uint16_be_m(p + off, e1k_checksum(p + start, end + 1 - start));
    }
}

static void e1k_transmit(e1000e_t* n, const uint8_t* desc)
{
    uint8_t  cmd      = desc[11];
    size_t   len      = n->packet_size;
    uint8_t* p        = n->packet;
    bool     extended = (cmd & 0x20) != 0;
    if (!len || n->discard) {
        return;
    }

    if (extended && (cmd & 4)) {
        /* TCP segmentation context. All offsets and sizes originate in guest memory. */
        size_t ip = n->context[0], tcp = n->context[4];
        size_t hdr = n->context[13], mss = read_uint16_le_m(n->context + 14);
        if (hdr > len || hdr < tcp + 20 || tcp < ip + 20 || mss < 64 || hdr + mss > TAP_FRAME_SIZE || (p[ip] >> 4) != 4
            || p[ip + 9] != 6) {
            return;
        }
        uint8_t  frame[TAP_FRAME_SIZE];
        uint32_t seq = read_uint32_be_m(p + tcp + 4);
        uint16_t id  = read_uint16_be_m(p + ip + 4);
        for (size_t pos = hdr; pos < len; pos += mss) {
            size_t count = EVAL_MIN(mss, len - pos), total = hdr + count;
            memcpy(frame, p, hdr);
            memcpy(frame + hdr, p + pos, count);
            write_uint16_be_m(frame + ip + 2, total - ip);
            write_uint16_be_m(frame + ip + 4, id++);
            write_uint16_be_m(frame + ip + 10, 0);
            e1k_insert_checksum(frame, total, ip, ip + 10, tcp - 1);
            write_uint32_be_m(frame + tcp + 4, seq + pos - hdr);
            if (pos + count < len) {
                frame[tcp + 13] &= ~9U; /* FIN/PSH only on last segment */
            }
            if (pos != hdr) {
                frame[tcp + 13] &= ~0x80U; /* CWR only on first segment */
            }
            uint8_t pseudo[12];
            memcpy(pseudo, frame + ip + 12, 8);
            pseudo[8] = 0;
            pseudo[9] = 6;
            write_uint16_be_m(pseudo + 10, total - tcp);
            write_uint16_be_m(frame + tcp + 16, (uint16_t)~e1k_checksum(pseudo, sizeof(pseudo)));
            e1k_insert_checksum(frame, total, tcp, tcp + 16, 0);
            tap_send(n->tap, frame, total);
        }
    } else if (len <= TAP_FRAME_SIZE) {
        if (extended) {
            if (desc[13] & 1) {
                e1k_insert_checksum(p, len, n->context[0], n->context[1], read_uint16_le_m(n->context + 2));
            }
            if (desc[13] & 2) {
                e1k_insert_checksum(p, len, n->context[4], n->context[5], read_uint16_le_m(n->context + 6));
            }
        } else if (cmd & 4) {
            e1k_insert_checksum(p, len, desc[13], desc[10], 0);
        }
        tap_send(n->tap, p, len);
    }
}

/* One descriptor per lock acquisition keeps MMIO latency bounded. */
static bool e1k_tx_step(e1000e_t* n)
{
    uint32_t count = REG(n, TDLEN) / 16, head = REG(n, TDH), tail = REG(n, TDT);
    if (n->suspended || !(REG(n, TCTL) & 2) || !count || head >= count || tail >= count || head == tail) {
        return false;
    }
    uint64_t base = ((uint64_t)REG(n, TDBAH) << 32) | REG(n, TDBAL);
    uint64_t addr = base + head * 16;
    uint8_t  d[16];
    if (addr < base || !rvvm_pci_read_dma(n->pci, addr, d, sizeof(d))) {
        return false;
    }
    bool context = (d[11] & 0x20) && !(d[10] & 0x10);
    if (context) {
        memcpy(n->context, d, 16);
    } else {
        size_t len = read_uint16_le_m(d + 8);
        if (len > sizeof(n->packet) - n->packet_size) {
            n->discard = true;
        }
        if (!n->discard && len) {
            if (rvvm_pci_read_dma(n->pci, read_uint64_le_m(d), n->packet + n->packet_size, len)) {
                n->packet_size += len;
            } else {
                n->discard = true;
            }
        }
        if (d[11] & 1) {
            e1k_transmit(n, d);
            n->packet_size = 0;
            n->discard     = false;
            REG(n, 0x4080)++; /* GPTC */
        }
    }
    if (d[11] & 8) {
        d[12] |= 1; /* DD, published after all payload DMA */
        rvvm_pci_write_dma(n->pci, addr + 12, d + 12, 1);
        e1k_irq(n, IRQ_TXDW);
    }
    REG(n, TDH) = (head + 1) % count;
    if (REG(n, TDH) == tail) {
        e1k_irq(n, IRQ_TXQE);
    }
    return REG(n, TDH) != tail;
}

static void* e1k_worker(void* arg)
{
    e1000e_t* n = arg;
    while (!atomic_load_uint32(&n->stop)) {
        rvvm_event_wait(&n->wake, RVVM_EVENT_INFINITE);
        bool more = true;
        for (unsigned budget = 0; more && budget < 256 && !atomic_load_uint32(&n->stop); ++budget) {
            spin_lock(&n->lock);
            more = e1k_tx_step(n);
            spin_unlock(&n->lock);
        }
        if (more) {
            rvvm_event_wake(&n->wake);
        }
    }
    return NULL;
}

static bool e1k_accept(e1000e_t* n, const uint8_t* p)
{
    bool broadcast = true;
    for (unsigned i = 0; i < 6; ++i) {
        broadcast &= p[i] == 255;
    }
    if (broadcast) {
        return (REG(n, RCTL) & 0x8000) != 0;
    }
    if (REG(n, RCTL) & ((p[0] & 1) ? 0x10 : 8)) {
        return true;
    }
    for (unsigned i = 0; i < 16; ++i) {
        uint32_t hi = REG(n, RAH + i * 8);
        if ((hi & 0x80000000) && read_uint32_le_m(p) == REG(n, RAL + i * 8)
            && read_uint16_le_m(p + 4) == (hi & 0xFFFF)) {
            return true;
        }
    }
    if (p[0] & 1) {
        unsigned shift = 4 - ((REG(n, RCTL) >> 12) & 3);
        if (shift == 1) {
            shift = 0;
        }
        unsigned hash = (read_uint16_le_m(p + 4) >> shift) & 0xFFF;
        return (REG(n, 0x5200 + (hash >> 5) * 4) & (1U << (hash & 31))) != 0;
    }
    return false;
}

static bool e1k_receive(void* opaque, const void* data, size_t size)
{
    e1000e_t* n = opaque;
    if (size < 14 || size > TAP_FRAME_SIZE) {
        return false;
    }
    spin_lock(&n->lock);
    bool ok = false;
    if (n->suspended || !(REG(n, RCTL) & 2) || !e1k_accept(n, data)) {
        goto done;
    }
    /* Packet-split descriptors are not supported; never interpret them as legacy. */
    if (REG(n, RCTL) & 0xC00) {
        goto done;
    }
    uint8_t frame[TAP_FRAME_SIZE + 64];
    memcpy(frame, data, size);
    if (size < 60) {
        memset(frame + size, 0, 60 - size);
        size = 60;
    }
    if (!(REG(n, RCTL) & 0x04000000)) {
        memset(frame + size, 0, 4);
        size += 4;
    }
    unsigned bsz = 2048 >> ((REG(n, RCTL) >> 16) & 3);
    if (REG(n, RCTL) & 0x02000000) {
        bsz *= 16;
    }
    uint32_t count = REG(n, RDLEN) / 16, head = REG(n, RDH), tail = REG(n, RDT);
    if (!count || head >= count || tail >= count || (tail + count - head) % count < (size + bsz - 1) / bsz) {
        e1k_irq(n, IRQ_RXO);
        goto done;
    }
    uint64_t base = ((uint64_t)REG(n, RDBAH) << 32) | REG(n, RDBAL);
    for (size_t pos = 0; pos < size;) {
        uint8_t  d[16];
        uint64_t addr = base + head * 16;
        size_t   len  = EVAL_MIN(size - pos, bsz);
        if (addr < base || !rvvm_pci_read_dma(n->pci, addr, d, 16)
            || !rvvm_pci_write_dma(n->pci, read_uint64_le_m(d), frame + pos, len)) {
            e1k_irq(n, IRQ_RXO);
            goto done;
        }
        pos            += len;
        uint8_t status  = 5 | ((pos == size) ? 2 : 0); /* DD, IXSM, EOP */
        if (REG(n, RFCTL) & 0x8000) {
            memset(d, 0, 16);
            write_uint16_le_m(d + 12, len);
            if (!rvvm_pci_write_dma(n->pci, addr, d, 16)) {
                goto done;
            }
            atomic_fence();
            d[8] = status;
            if (!rvvm_pci_write_dma(n->pci, addr + 8, d + 8, 1)) {
                goto done;
            }
        } else {
            memset(d + 8, 0, 8);
            write_uint16_le_m(d + 8, len);
            if (!rvvm_pci_write_dma(n->pci, addr + 8, d + 8, 8)) {
                goto done;
            }
            atomic_fence();
            d[12] = status;
            if (!rvvm_pci_write_dma(n->pci, addr + 12, d + 12, 1)) {
                goto done;
            }
        }
        head        = (head + 1) % count;
        REG(n, RDH) = head;
    }
    REG(n, 0x4074)++; /* GPRC */
    e1k_irq(n, IRQ_RXT0);
    ok = true;
done:
    spin_unlock(&n->lock);
    return ok;
}

static void e1k_reset_locked(e1000e_t* n)
{
    memset(n->regs, 0, sizeof(n->regs));
    memset(n->context, 0, sizeof(n->context));
    memset(n->phy, 0, sizeof(n->phy));
    n->packet_size  = 0;
    n->discard      = false;
    REG(n, CTRL)    = 0x241;
    REG(n, STATUS)  = 0x83;       /* 1000 Mb/s, full duplex, link up */
    REG(n, EECD)    = 0x300;      /* EEPROM present, auto-read complete */
    REG(n, 0x10010) = 0x40;       /* EEMNGCTL: configuration done */
    REG(n, 0x5B50)  = 0x40;       /* SWSM: driver semaphore available */
    REG(n, 0x1000)  = 0x00100030; /* PBA */
    REG(n, RAL)     = n->nvm[0] | (uint32_t)n->nvm[1] << 16;
    REG(n, RAH)     = n->nvm[2] | 0x80000000U;
    uint8_t mac[6];
    for (unsigned i = 0; i < 3; ++i) {
        write_uint16_le_m(mac + i * 2, n->nvm[i]);
    }
    tap_set_mac(n->tap, mac);
    n->phy[0]  = 0x1140;
    n->phy[1]  = 0x796D;
    n->phy[2]  = 0x0141;
    n->phy[3]  = 0x0CB1; /* BM PHY */
    n->phy[4]  = 0x0DE1;
    n->phy[5]  = 0x0DE1;
    n->phy[9]  = 0x0300;
    n->phy[10] = 0x3800;
    n->phy[17] = 0xAC00; /* resolved 1G full duplex link */
    e1k_irq(n, 0);
}

static uint32_t e1k_read_locked(e1000e_t* n, size_t off)
{
    if (off >= E1K_BAR_SIZE || (off & 3)) {
        return 0;
    }
    uint32_t value = REG(n, off);
    if (off == ICR) {
        if (value & REG(n, IMS)) {
            value |= 0x80000000U;
        }
        REG(n, ICR) = 0;
        if (REG(n, CTRL_EXT) & 0x08000000) {
            REG(n, IMS) &= ~REG(n, IAM);
        }
        e1k_irq(n, 0);
    } else if (off == ICS) {
        value = REG(n, ICR);
    } else if (off == IMC) {
        value = REG(n, IMS);
    } else if (off >= 0x4000 && off < 0x4100) {
        REG(n, off) = 0; /* statistics */
    }
    return value;
}

static void e1k_write_locked(e1000e_t* n, size_t off, uint32_t value)
{
    if (off >= E1K_BAR_SIZE || (off & 3)) {
        return;
    }
    switch (off) {
        case STATUS:
            return;
        case CTRL:
            if (value & 0x04000000) {
                e1k_reset_locked(n);
                return;
            }
            value &= ~0x80000000U; /* PHY reset self-clears */
            break;
        case EECD:
            value = (value & ~0xC0U) | 0x300 | ((value & 0x40) ? 0x80 : 0);
            break;
        case EERD: {
            unsigned word = (value >> 2) & 0x3FFF;
            value         = (value & 0xFFFF) | 2 | ((uint32_t)(word < 64 ? n->nvm[word] : 0xFFFF) << 16);
            break;
        }
        case MDIC: {
            unsigned phy = (value >> 21) & 31, reg = (value >> 16) & 31;
            if (phy != 1) {
                value |= 0x40000000;
            } else if (value & 0x08000000) {
                value = (value & 0xFFFF0000) | n->phy[reg];
            } else if (value & 0x04000000) {
                if (reg != 1 && reg != 2 && reg != 3) {
                    n->phy[reg] = value & (reg == 0 ? 0x7FFF : 0xFFFF);
                }
            }
            value |= 0x10000000;
            if (value & 0x20000000) {
                e1k_irq(n, 0x200);
            }
            break;
        }
        case ICR:
            REG(n, ICR) &= ~value;
            e1k_irq(n, 0);
            return;
        case ICS:
            e1k_irq(n, value);
            return;
        case IMS:
            REG(n, IMS) |= value & 0x7FFFFFFF;
            e1k_irq(n, 0);
            return;
        case IMC:
            REG(n, IMS) &= ~value;
            e1k_irq(n, 0);
            return;
        case RDBAL:
        case TDBAL:
            value &= ~0xFU;
            break;
        case RDLEN:
        case TDLEN:
            value &= 0x000FFF80;
            break;
        case RDH:
        case RDT:
        case TDH:
        case TDT:
            value &= 0xFFFF;
            break;
    }
    REG(n, off) = value;
    if (off == RAL || off == RAH) {
        uint8_t mac[6];
        write_uint32_le_m(mac, REG(n, RAL));
        write_uint16_le_m(mac + 4, REG(n, RAH));
        tap_set_mac(n->tap, mac);
    }
    if (off == TDT || off == TCTL) {
        rvvm_event_wake(&n->wake);
    }
}

static void e1k_read(rvvm_reg_dev_t* dev, void* data, size_t size, size_t off)
{
    e1000e_t* n = rvvm_region_data(dev);
    UNUSED(size);
    spin_lock(&n->lock);
    write_uint32_le(data, e1k_read_locked(n, off));
    spin_unlock(&n->lock);
}

static void e1k_write(rvvm_reg_dev_t* dev, const void* data, size_t size, size_t off)
{
    e1000e_t* n = rvvm_region_data(dev);
    UNUSED(size);
    spin_lock(&n->lock);
    e1k_write_locked(n, off, read_uint32_le(data));
    spin_unlock(&n->lock);
}

static void e1k_io_read(rvvm_reg_dev_t* dev, void* data, size_t size, size_t off)
{
    e1000e_t* n = rvvm_region_data(dev);
    UNUSED(size);
    spin_lock(&n->lock);
    write_uint32_le(data, off == 0 ? n->ioaddr : off == 4 ? e1k_read_locked(n, n->ioaddr) : 0);
    spin_unlock(&n->lock);
}

static void e1k_io_write(rvvm_reg_dev_t* dev, const void* data, size_t size, size_t off)
{
    e1000e_t* n = rvvm_region_data(dev);
    UNUSED(size);
    spin_lock(&n->lock);
    if (off == 0) {
        n->ioaddr = read_uint32_le(data);
    } else if (off == 4) {
        e1k_write_locked(n, n->ioaddr, read_uint32_le(data));
    }
    spin_unlock(&n->lock);
}

static void e1k_reset(rvvm_reg_dev_t* dev)
{
    e1000e_t* n = rvvm_region_data(dev);
    spin_lock(&n->lock);
    e1k_reset_locked(n);
    spin_unlock(&n->lock);
}

static void e1k_suspend(rvvm_reg_dev_t* dev, rvvm_snapshot_t* snap, bool resume)
{
    e1000e_t* n = rvvm_region_data(dev);
    spin_lock(&n->lock);
    n->suspended = !resume;
    if (snap) {
        rvvm_snapshot_field(snap, n->regs);
        rvvm_snapshot_field(snap, n->phy);
        rvvm_snapshot_field(snap, n->nvm);
        rvvm_snapshot_field(snap, n->context);
        rvvm_snapshot_field(snap, n->packet);
        rvvm_snapshot_field(snap, n->packet_size);
        rvvm_snapshot_field(snap, n->discard);
        rvvm_snapshot_field(snap, n->ioaddr);
        if (n->packet_size > sizeof(n->packet)) {
            n->packet_size = 0;
            n->discard     = true;
        }
    }
    if (resume) {
        uint8_t mac[6];
        write_uint32_le_m(mac, REG(n, RAL));
        write_uint16_le_m(mac + 4, REG(n, RAH));
        tap_set_mac(n->tap, mac);
    }
    e1k_irq(n, 0);
    spin_unlock(&n->lock);
    if (resume) {
        rvvm_event_wake(&n->wake);
    }
}

static void e1k_cleanup(rvvm_reg_dev_t* dev)
{
    e1000e_t* n = rvvm_region_data(dev);
    if (++n->cleanup != 3) {
        return;
    }
    atomic_store_uint32(&n->stop, 1);
    rvvm_event_wake(&n->wake);
    rvvm_thread_join(n->worker);
    tap_close(n->tap);
    free(n);
}

static const rvvm_reg_type_t e1k_type = {
    .name     = "e1000e",
    .read     = e1k_read,
    .write    = e1k_write,
    .reset    = e1k_reset,
    .suspend  = e1k_suspend,
    .cleanup  = e1k_cleanup,
    .min_size = 4,
    .max_size = 4,
};
static const rvvm_reg_type_t e1k_io_type = {
    .name     = "e1000e_io",
    .read     = e1k_io_read,
    .write    = e1k_io_write,
    .cleanup  = e1k_cleanup,
    .min_size = 4,
    .max_size = 4,
};
static const rvvm_reg_type_t e1k_flash_type = {.name = "e1000e_flash", .cleanup = e1k_cleanup};

RVVM_PUBLIC rvvm_pci_func_t* rvvm_e1000e_init(rvvm_machine_t* machine, tap_dev_t* tap, rvvm_pci_addr_t addr)
{
    if (!tap) {
        return NULL;
    }
    e1000e_t* n = safe_new_obj(e1000e_t);
    n->tap      = tap;
    uint8_t mac[6];
    tap_get_mac(tap, mac);
    for (unsigned i = 0; i < 3; ++i) {
        n->nvm[i] = read_uint16_le_m(mac + i * 2);
    }
    n->nvm[0x0B] = 0x10D3;
    n->nvm[0x0C] = 0x8086;
    n->nvm[0x0F] = 0x2000;
    uint16_t sum = 0;
    for (unsigned i = 0; i < 63; ++i) {
        sum += n->nvm[i];
    }
    n->nvm[63] = 0xBABA - sum;
    e1k_reset_locked(n);
    rvvm_reg_desc_t      mmio  = {.size = E1K_BAR_SIZE, .data = n, .type = &e1k_type};
    rvvm_reg_desc_t      flash = {.size = E1K_BAR_SIZE, .data = n, .type = &e1k_flash_type, .attr = RVVM_REG_ATTR_ROM};
    rvvm_reg_desc_t      io    = {.size = 32, .data = n, .type = &e1k_io_type, .attr = RVVM_REG_ATTR_PIO};
    rvvm_pci_func_desc_t desc  = {
        .vendor_id  = 0x8086,
        .device_id  = 0x10D3,
        .subsys_ven = 0x8086,
        .subsys_dev = 0,
        .class_code = 0x0200,
        .revision   = 0,
        .irq_pin    = RVVM_PCI_PIN_INTA,
        .bar[0]     = &mmio,
        .bar[1]     = &flash,
        .bar[2]     = &io,
    };
    rvvm_pci_func_t* pci = rvvm_pci_func_init(machine, &desc, addr);
    if (!pci) {
        return NULL; /* PCI core owns and cleans up all three BARs. */
    }
    n->pci    = pci;
    n->worker = rvvm_thread_create(e1k_worker, n);
    if (!n->worker) {
        rvvm_pci_func_remove(pci);
        return NULL;
    }
    tap_net_dev_t net = {.net_dev = n, .feed_rx = e1k_receive};
    tap_attach(tap, &net);
    return pci;
}
#endif
