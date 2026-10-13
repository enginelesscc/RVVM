/* End-to-end descriptor DMA -> async user NAT -> real host sockets -> RX DMA.
 * SPDX-License-Identifier: MPL-2.0
 * Run with a local non-loopback IPv4 address to include host UDP/TCP/ICMP tests.
 */
#undef NDEBUG
#define rvvm_e1000e_init   test_e1000e_init
#define rvvm_pci_read_dma  test_read_dma
#define rvvm_pci_write_dma test_write_dma
#define rvvm_pci_set_irq   test_set_irq
#define tap_send           test_tap_send
#include "../src/devices/e1000e.c"
#undef tap_send
extern bool tap_send(tap_dev_t* tap, const void* data, size_t size);
#include "networking.h"
#include "rvtimer.h"
#include <assert.h>
#include <rvvm/rvvm.h>
#include <rvvm/rvvm_board.h>
#include <stdio.h>

#undef rvvm_e1000e_init
extern rvvm_pci_func_t* rvvm_e1000e_init(rvvm_machine_t* machine, tap_dev_t* tap, rvvm_pci_addr_t addr);

static uint8_t       memory[262144];
static bool          irq;
static e1000e_t*     nic;
static unsigned      rx_cons;
static const uint8_t client[4]      = {192, 168, 0, 100};
static const uint8_t gateway[4]     = {192, 168, 0, 1};
static const uint8_t gateway_mac[6] = {0, 8, 0x97, 0xDE, 0xC0, 0xDE};
static uint8_t       mac[6];
static bool          capture_tx;
static unsigned      captured;
static size_t        captured_len[4];
static uint8_t       captured_frame[4][1514];

bool test_tap_send(tap_dev_t* tap, const void* data, size_t size)
{
    if (!capture_tx) {
        return tap_send(tap, data, size);
    }
    assert(captured < 4 && size <= 1514);
    captured_len[captured] = size;
    memcpy(captured_frame[captured++], data, size);
    return true;
}

bool test_read_dma(rvvm_pci_func_t* pci, rvvm_addr_t addr, void* out, size_t len)
{
    UNUSED(pci);
    if (addr > sizeof(memory) || len > sizeof(memory) - addr) {
        return false;
    }
    memcpy(out, memory + addr, len);
    return true;
}

bool test_write_dma(rvvm_pci_func_t* pci, rvvm_addr_t addr, const void* in, size_t len)
{
    UNUSED(pci);
    if (addr > sizeof(memory) || len > sizeof(memory) - addr) {
        return false;
    }
    memcpy(memory + addr, in, len);
    return true;
}

void test_set_irq(rvvm_pci_func_t* pci, uint32_t vec, bool level)
{
    UNUSED(pci);
    UNUSED(vec);
    irq = level;
}

static void pause_ms(unsigned ms)
{
    rvvm_event_t event = {0};
    rvvm_event_wait(&event, (uint64_t)ms * 1000000);
}

static void submit(const uint8_t* frame, size_t len)
{
    assert(len <= TAP_FRAME_SIZE);
    spin_lock(&nic->lock);
    unsigned tail = REG(nic, TDT), next = (tail + 1) % 8;
    assert(next != REG(nic, TDH));
    uint8_t* d = memory + 0x1000 + tail * 16;
    memset(d, 0, 16);
    write_uint64_le_m(d, 0x4000 + tail * 2048);
    write_uint16_le_m(d + 8, len);
    d[11] = 0x0B; /* EOP, IFCS, RS */
    memcpy(memory + 0x4000 + tail * 2048, frame, len);
    e1k_write_locked(nic, TDT, next);
    spin_unlock(&nic->lock);
    for (unsigned i = 0; i < 2000; ++i) {
        spin_lock(&nic->lock);
        bool done = d[12] & 1;
        spin_unlock(&nic->lock);
        if (done) {
            return;
        }
        pause_ms(1);
    }
    assert(!"TX descriptor timeout");
}

static size_t receive(uint8_t* frame, unsigned timeout)
{
    for (unsigned i = 0; i < timeout; ++i) {
        spin_lock(&nic->lock);
        uint8_t* d = memory + 0x2000 + rx_cons * 16;
        if (d[12] & 1) {
            size_t size = read_uint16_le_m(d + 8);
            assert((d[12] & 3) == 3 && size <= 2048);
            memcpy(frame, memory + 0x10000 + rx_cons * 2048, size);
            memset(d + 8, 0, 8);
            REG(nic, RDT) = (REG(nic, RDT) + 1) % 8;
            rx_cons       = (rx_cons + 1) % 8;
            e1k_read_locked(nic, ICR);
            spin_unlock(&nic->lock);
            if (read_uint16_be_m(frame + 12) == 0x0800) {
                assert(e1k_checksum(frame + 14, 20) == 0);
            }
            return size;
        }
        spin_unlock(&nic->lock);
        pause_ms(1);
    }
    return 0;
}

static void ether(uint8_t* frame, unsigned type)
{
    memset(frame, 0, 1514);
    memcpy(frame, gateway_mac, 6);
    memcpy(frame + 6, mac, 6);
    write_uint16_be_m(frame + 12, type);
}

static void ipv4(uint8_t* frame, unsigned proto, size_t payload, const uint8_t* dst)
{
    ether(frame, 0x0800);
    uint8_t* ip = frame + 14;
    ip[0]       = 0x45;
    ip[8]       = 64;
    ip[9]       = proto;
    memcpy(ip + 12, client, 4);
    memcpy(ip + 16, dst, 4);
    write_uint16_be_m(ip + 2, 20 + payload);
    write_uint16_be_m(ip + 10, e1k_checksum(ip, 20));
}

static void udp(uint8_t* frame, uint16_t dst_port, size_t len, const uint8_t* dst)
{
    ipv4(frame, 17, 8 + len, dst);
    write_uint16_be_m(frame + 34, 40000);
    write_uint16_be_m(frame + 36, dst_port);
    write_uint16_be_m(frame + 38, 8 + len);
}

static void tcp(uint8_t* frame, uint16_t port, uint32_t seq, uint32_t ack, uint8_t flags, const void* data, size_t len,
                const uint8_t* dst)
{
    ipv4(frame, 6, 20 + len, dst);
    uint8_t* t = frame + 34;
    write_uint16_be_m(t, 40001);
    write_uint16_be_m(t + 2, port);
    write_uint32_be_m(t + 4, seq);
    write_uint32_be_m(t + 8, ack);
    t[12] = 0x50;
    t[13] = flags;
    write_uint16_be_m(t + 14, 65535);
    if (len) {
        memcpy(t + 20, data, len);
    }
    uint8_t pseudo[12];
    memcpy(pseudo, frame + 26, 8);
    pseudo[8] = 0;
    pseudo[9] = 6;
    write_uint16_be_m(pseudo + 10, 20 + len);
    write_uint16_be_m(t + 16, (uint16_t)~e1k_checksum(pseudo, 12));
    write_uint16_be_m(t + 16, e1k_checksum(t, 20 + len));
    submit(frame, 54 + len);
}

static void test_dhcp_arp(void)
{
    uint8_t f[1514], reply[2048];
    ether(f, 0x0806);
    uint8_t* a = f + 14;
    write_uint16_be_m(a, 1);
    write_uint16_be_m(a + 2, 0x0800);
    a[4] = 6;
    a[5] = 4;
    write_uint16_be_m(a + 6, 1);
    memcpy(a + 8, mac, 6);
    memcpy(a + 14, client, 4);
    memcpy(a + 24, gateway, 4);
    submit(f, 42);
    assert(receive(reply, 1000));
    assert(read_uint16_be_m(reply + 20) == 2 && !memcmp(reply + 28, gateway, 4));
    assert(!memcmp(reply, mac, 6) && !memcmp(reply + 32, mac, 6));
    assert(!memcmp(reply + 38, client, 4));
    /* Duplicate-address probes must stay unanswered. */
    memset(a + 14, 0, 4);
    memcpy(a + 24, client, 4);
    submit(f, 42);
    assert(!receive(reply, 30));
    for (unsigned request = 1; request <= 3; request += 2) {
        udp(f, 67, 245, gateway);
        write_uint16_be_m(f + 34, 68);
        uint8_t* d = f + 42;
        d[0]       = 1;
        d[1]       = 1;
        d[2]       = 6;
        write_uint32_be_m(d + 4, 0x12345678);
        memcpy(d + 28, mac, 6);
        write_uint32_be_m(d + 236, 0x63825363);
        d[240] = 0;
        d[241] = 53;
        d[242] = 1;
        d[243] = request;
        d[244] = 255;
        submit(f, 287);
        assert(receive(reply, 1000));
        assert(read_uint32_be_m(reply + 46) == 0x12345678);
        assert(reply[284] == (request == 1 ? 2 : 5));
        assert(!memcmp(reply + 58, client, 4));
        const uint8_t* server_name = reply + 42 + 44;
        const uint8_t* name_end = memchr(server_name, 0, 64);
        assert(name_end);
        for (const uint8_t* c = server_name; c < name_end; ++c) {
            assert((*c >= 'a' && *c <= 'z') || (*c >= 'A' && *c <= 'Z')
                   || (*c >= '0' && *c <= '9') || *c == '-' || *c == '.');
        }
    }
    /* Undersized UDP/TCP/IP packets and bogus DHCP TLVs must not crash/respond. */
    udp(f, 9, 0, gateway);
    write_uint16_be_m(f + 38, 1);
    submit(f, 42);
    ipv4(f, 6, 1, gateway);
    submit(f, 35);
    ipv4(f, 1, 4, gateway);
    f[34] = 8;
    submit(f, 38);
    f[14] = 0x40;
    submit(f, 38);
    assert(!receive(reply, 30));
    puts("ARP, DHCP offer/ACK/renewal, malformed frames: PASS");
}

static void test_host(const uint8_t* host)
{
    uint8_t     f[1514], reply[2048], buf[128];
    net_addr_t  remote;
    net_sock_t* u = net_udp_bind(NET_IPV4_ANY);
    assert(u);
    net_sock_set_blocking(u, false);
    udp(f, net_sock_port(u), 5, host);
    memcpy(f + 42, "hello", 5);
    submit(f, 47);
    int32_t got = -1;
    for (unsigned i = 0; i < 2000 && got < 0; ++i) {
        got = net_udp_recv(u, buf, sizeof(buf), &remote);
        if (got < 0) {
            pause_ms(1);
        }
    }
    assert(got == 5 && !memcmp(buf, "hello", 5));
    assert(net_udp_send(u, "world", 5, &remote) == 5);
    assert(receive(reply, 1000) && !memcmp(reply + 42, "world", 5));
    net_sock_close(u);

    net_sock_t* listener = net_tcp_listen(NET_IPV4_ANY);
    assert(listener);
    net_sock_set_blocking(listener, false);
    uint16_t port = net_sock_port(listener);
    tcp(f, port, 100, 0, 2, NULL, 0, host);
    assert(receive(reply, 2000));
    assert((reply[47] & 0x12) == 0x12);
    uint32_t seq = read_uint32_be_m(reply + 38) + 1;
    assert(read_uint32_be_m(reply + 42) == 101);
    net_sock_t* peer = net_tcp_accept(listener);
    assert(peer);
    net_sock_set_blocking(peer, false);
    tcp(f, port, 101, seq, 0x10, NULL, 0, host);
    tcp(f, port, 101, seq, 0x18, "hello", 5, host);
    got = -1;
    for (unsigned i = 0; i < 2000 && got < 0; ++i) {
        got = net_tcp_recv(peer, buf, sizeof(buf));
        if (got < 0) {
            pause_ms(1);
        }
    }
    assert(got == 5 && !memcmp(buf, "hello", 5));
    assert(receive(reply, 1000) && read_uint32_be_m(reply + 42) == 106);
    /* Retransmitted guest payload must not be delivered twice. */
    tcp(f, port, 101, seq, 0x18, "hello", 5, host);
    assert(receive(reply, 1000));
    assert(net_tcp_recv(peer, buf, sizeof(buf)) == NET_ERR_BLOCK);
    assert(net_tcp_send(peer, "world", 5) == 5);
    assert(receive(reply, 1000) && !memcmp(reply + 54, "world", 5));
    /* Unacknowledged host data is retransmitted. */
    assert(receive(reply, 2000) && !memcmp(reply + 54, "world", 5));
    tcp(f, port, 106, seq + 5, 0x11, NULL, 0, host);
    assert(receive(reply, 1000));
    assert(net_tcp_shutdown(peer));
    assert(receive(reply, 1000) && (reply[47] & 1));
    tcp(f, port, 107, seq + 6, 0x10, NULL, 0, host);
    net_sock_close(peer);
    net_sock_close(listener);
    puts("Host UDP and TCP connect/data/retransmit/half-close: PASS");

    ipv4(f, 1, 13, host);
    f[34] = 8;
    write_uint16_be_m(f + 38, 0x1234);
    write_uint16_be_m(f + 40, 7);
    memcpy(f + 42, "hello", 5);
    write_uint16_be_m(f + 36, e1k_checksum(f + 34, 13));
    submit(f, 47);
    assert(receive(reply, 2000));
    assert(reply[34] == 0 && read_uint16_be_m(reply + 38) == 0x1234 && read_uint16_be_m(reply + 40) == 7);
    assert(e1k_checksum(reply + 34, 13) == 0 && !memcmp(reply + 42, "hello", 5));
    puts("Real host ICMP echo, ID, sequence, payload and checksum: PASS");
}

static void test_registers(void)
{
    spin_lock(&nic->lock);
    e1k_write_locked(nic, IMS, IRQ_LSC);
    e1k_write_locked(nic, ICS, IRQ_LSC);
    assert(irq);
    assert(e1k_read_locked(nic, ICR) == (0x80000000U | IRQ_LSC));
    assert(!irq);
    e1k_write_locked(nic, MDIC, 0x08220000);
    assert((REG(nic, MDIC) & 0xFFFF) == 0x0141);
    e1k_write_locked(nic, EERD, 1);
    assert((REG(nic, EERD) >> 16) == nic->nvm[0]);
    e1k_write_locked(nic, TDLEN, ~0U);
    assert(REG(nic, TDLEN) == 0xFFF80);
    e1k_write_locked(nic, TDLEN, 128);
    REG(nic, TDH) = 8;
    assert(!e1k_tx_step(nic));
    REG(nic, TDH) = 0;
    e1k_write_locked(nic, IMS, IRQ_TXDW | IRQ_RXT0);
    spin_unlock(&nic->lock);
    puts("EEPROM, PHY, interrupt mask/ack, malformed ring bounds: PASS");
}

static void test_offloads(void)
{
    capture_tx = true;
    uint8_t* p = nic->packet;
    ipv4(p, 6, 20 + 1500, gateway);
    write_uint32_be_m(p + 38, 123);
    p[46] = 0x50;
    p[47] = 0x99;
    memset(p + 54, 0xAB, 1500);
    nic->packet_size = 1554;
    nic->context[0]  = 14;
    nic->context[1]  = 24;
    write_uint16_le_m(nic->context + 2, 33);
    nic->context[4]  = 34;
    nic->context[5]  = 50;
    nic->context[13] = 54;
    write_uint16_le_m(nic->context + 14, 700);
    uint8_t d[16] = {0};
    d[11]         = 0x25;
    d[13]         = 3;
    e1k_transmit(nic, d);
    assert(captured == 3);
    for (unsigned i = 0; i < 3; ++i) {
        const uint8_t* f   = captured_frame[i];
        size_t         len = captured_len[i];
        assert(len == (i < 2 ? 754 : 154));
        assert(read_uint16_be_m(f + 16) == len - 14 && e1k_checksum(f + 14, 20) == 0);
        assert(read_uint32_be_m(f + 38) == 123 + i * 700);
        assert((f[47] & 9) == (i == 2 ? 9 : 0));
        assert((f[47] & 0x80) == (i == 0 ? 0x80 : 0));
        uint8_t pseudo[12];
        memcpy(pseudo, f + 26, 8);
        pseudo[8] = 0;
        pseudo[9] = 6;
        write_uint16_be_m(pseudo + 10, len - 34);
        uint32_t sum = (uint16_t)~e1k_checksum(pseudo, 12) + (uint16_t)~e1k_checksum(f + 34, len - 34);
        while (sum >> 16) {
            sum = (sum & 65535) + (sum >> 16);
        }
        assert(sum == 65535);
    }
    /* Invalid TSO context is bounded and drops instead of looping/reading past a buffer. */
    write_uint16_le_m(nic->context + 14, 0);
    e1k_transmit(nic, d);
    assert(captured == 3);
    /* Extended descriptor checksum insertion includes the pseudo-header seed. */
    captured = 0;
    udp(p, 9, 5, gateway);
    memcpy(p + 42, "hello", 5);
    write_uint16_be_m(p + 24, 0);
    uint8_t pseudo[12];
    memcpy(pseudo, p + 26, 8);
    pseudo[8] = 0;
    pseudo[9] = 17;
    write_uint16_be_m(pseudo + 10, 13);
    write_uint16_be_m(p + 40, (uint16_t)~e1k_checksum(pseudo, 12));
    nic->context[5]  = 40;
    nic->packet_size = 47;
    d[11]            = 0x21;
    e1k_transmit(nic, d);
    assert(captured == 1);
    assert(e1k_checksum(captured_frame[0] + 14, 20) == 0);
    uint32_t sum = (uint16_t)~e1k_checksum(pseudo, 12) + (uint16_t)~e1k_checksum(captured_frame[0] + 34, 13);
    while (sum >> 16) {
        sum = (sum & 65535) + (sum >> 16);
    }
    assert(sum == 65535);
    capture_tx       = false;
    nic->packet_size = 0;
    uint8_t f[1514];
    ether(f, 0x0806);
    memcpy(f, mac, 6);
    REG(nic, RFCTL) = 0x8000;
    assert(e1k_receive(nic, f, 42));
    assert(memory[0x2008] == 7 && read_uint16_le_m(memory + 0x200C) == 60);
    REG(nic, RFCTL)  = 0;
    REG(nic, RDH)    = 0;
    REG(nic, RDT)    = 7;
    REG(nic, RCTL)  |= 0x30000; /* 256-byte buffers */
    memset(memory + 0x2000, 0, 128);
    for (unsigned i = 0; i < 8; ++i) {
        write_uint64_le_m(memory + 0x2000 + i * 16, 0x10000 + i * 2048);
    }
    assert(e1k_receive(nic, f, 600));
    assert(memory[0x200C] == 5 && memory[0x201C] == 5 && memory[0x202C] == 7);
    assert(read_uint16_le_m(memory + 0x2028) == 88);
    REG(nic, RDT) = REG(nic, RDH);
    assert(!e1k_receive(nic, f, 60));
    REG(nic, RDH)   = 0;
    REG(nic, RDT)   = 7;
    REG(nic, RCTL) &= ~0x30000U;
    memset(memory + 0x2000, 0, 128);
    for (unsigned i = 0; i < 8; ++i) {
        write_uint64_le_m(memory + 0x2000 + i * 16, 0x10000 + i * 2048);
    }
    e1k_read_locked(nic, ICR);
    puts("TSO lengths/sequences/checksums/flags, extended RX, split buffers, overrun: PASS");
}

static void test_queue(void)
{
    tap_dev_t* tap = tap_open();
    assert(tap);
    uint8_t frame[1514];
    ether(frame, 0x0806);
    rvtimer_t timer;
    rvtimer_init(&timer, 1000);
    for (unsigned i = 0; i < 256; ++i) {
        assert(tap_send(tap, frame, 1514));
    }
    assert(!tap_send(tap, frame, 1514));
    assert(!tap_send(tap, frame, 1515));
    assert(rvtimer_get(&timer) < 1000);
    tap_close(tap);
    assert(!test_e1000e_init(NULL, tap_open(), RVVM_PCI_ADDR_ANY));
    puts("Bounded nonblocking TX queue, oversized frame rejection, failed-attach cleanup: PASS");
}

static void test_lifecycle(void)
{
    rvvm_machine_t* vm = rvvm_create_machine(16 * 1024 * 1024, 1, "rv64");
    assert(vm && rvvm_board_riscv_virt_init(vm, false));
    rvvm_pci_func_t* pci = rvvm_e1000e_init(vm, tap_open(), RVVM_PCI_ADDR_ANY);
    assert(pci);
    rvvm_pci_func_remove(pci);
    assert(rvvm_e1000e_init(vm, tap_open(), RVVM_PCI_ADDR_ANY));
    rvvm_free_machine(vm);
    puts("Actual PCI attach, hot-remove, reattach and VM destruction: PASS");
}

static void test_timeout(void)
{
    const uint8_t unreachable[4] = {192, 0, 2, 1};
    uint8_t       f[1514], reply[2048];
    ipv4(f, 1, 8, unreachable);
    f[34] = 8;
    write_uint16_be_m(f + 36, e1k_checksum(f + 34, 8));
    submit(f, 42);
    rvtimer_t timer;
    rvtimer_init(&timer, 1000);
    /* DHCP/ARP must complete while an external echo is pending. */
    test_dhcp_arp();
    assert(rvtimer_get(&timer) < 500);
    assert(!receive(reply, 1200));
    puts("Pending ICMP does not block DHCP/ARP and does not fake a success: PASS");
}

static void test_internet(void)
{
    const uint8_t        dns[4] = {1, 1, 1, 1};
    uint8_t              f[1514], reply[2048];
    static const uint8_t query[] = {0x12, 0x34, 1,   0,   0,   1, 0,   0,   0,   0, 0, 0, 7, 'e', 'x',
                                    'a',  'm',  'p', 'l', 'e', 3, 'c', 'o', 'm', 0, 0, 1, 0, 1};
    udp(f, 53, sizeof(query), dns);
    memcpy(f + 42, query, sizeof(query));
    submit(f, 42 + sizeof(query));
    assert(receive(reply, 5000));
    assert(read_uint16_be_m(reply + 42) == 0x1234 && (reply[44] & 0x80) && (reply[45] & 15) == 0);
    tcp(f, 80, 100, 0, 2, NULL, 0, dns);
    assert(receive(reply, 5000) && (reply[47] & 0x12) == 0x12);
    uint32_t seq = read_uint32_be_m(reply + 38) + 1;
    tcp(f, 80, 101, seq, 0x10, NULL, 0, dns);
    const char* req = "GET / HTTP/1.0\r\nHost: 1.1.1.1\r\n\r\n";
    tcp(f, 80, 101, seq, 0x18, req, strlen(req), dns);
    bool http = false;
    for (unsigned i = 0; i < 8 && !http; ++i) {
        assert(receive(reply, 5000));
        if (read_uint16_be_m(reply + 16) > 40) {
            http = !memcmp(reply + 54, "HTTP/", 5);
        }
    }
    assert(http);
    tcp(f, 80, 101 + strlen(req), seq, 4, NULL, 0, dns);
    while (receive(reply, 30)) {
    }
    ipv4(f, 1, 12, dns);
    f[34] = 8;
    write_uint16_be_m(f + 38, 0x3456);
    memcpy(f + 42, "ping", 4);
    write_uint16_be_m(f + 36, e1k_checksum(f + 34, 12));
    submit(f, 46);
    assert(receive(reply, 3000) && reply[34] == 0 && !memcmp(reply + 42, "ping", 4));
    puts("Internet UDP DNS, TCP HTTP, and real ICMP echo through guest descriptors: PASS");
}

int main(int argc, char** argv)
{
    test_queue();
    test_lifecycle();
    nic      = safe_new_obj(e1000e_t);
    nic->tap = tap_open();
    assert(nic->tap);
    nic->pci = (void*)1;
    tap_get_mac(nic->tap, mac);
    for (unsigned i = 0; i < 3; ++i) {
        nic->nvm[i] = read_uint16_le_m(mac + 2 * i);
    }
    e1k_reset_locked(nic);
    REG(nic, TDBAL) = 0x1000;
    REG(nic, TDLEN) = 128;
    REG(nic, TCTL)  = 2;
    REG(nic, RDBAL) = 0x2000;
    REG(nic, RDLEN) = 128;
    REG(nic, RDT)   = 7;
    REG(nic, RCTL)  = 0x04008002;
    for (unsigned i = 0; i < 8; ++i) {
        write_uint64_le_m(memory + 0x2000 + i * 16, 0x10000 + i * 2048);
    }
    test_registers();
    test_offloads();
    nic->worker = rvvm_thread_create(e1k_worker, nic);
    assert(nic->worker);
    tap_net_dev_t net = {.net_dev = nic, .feed_rx = e1k_receive};
    tap_attach(nic->tap, &net);
    test_dhcp_arp();
    if (argc > 1) {
        net_addr_t addr = {0};
        assert(net_parse_ipv4(&addr, argv[1]));
        test_host(addr.ip);
    }
    if (argc > 2) {
        test_internet();
    }
    if (argc > 1) {
        test_timeout();
    }
    atomic_store_uint32(&nic->stop, 1);
    rvvm_event_wake(&nic->wake);
    rvvm_thread_join(nic->worker);
    tap_close(nic->tap);
    free(nic);
    puts("Async e1000e DMA + user NAT: PASS");
    return 0;
}
