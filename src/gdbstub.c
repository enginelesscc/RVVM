/*
gdbstub.c - GDB Debugging Stub
Copyright (C) 2024  LekKit <github.com/LekKit>

This Source Code Form is subject to the terms of the Mozilla Public
License, v. 2.0. If a copy of the MPL was not distributed with this
file, You can obtain one at https://mozilla.org/MPL/2.0/.
*/

#include "gdbstub.h"
#include "utils.h"
#include "vector.h"
#include "threading.h"
#include "spinlock.h"
#include "networking.h"
#include "riscv_mmu.h"
#include "riscv_hart.h"

#ifdef USE_GDBSTUB

#define GDB_MAX_PKTSIZE 1024

typedef struct {
    gdb_server_t* server;
    net_sock_t* sock;

    // Chosen thread via 'H' packet
    rvvm_hart_t* hart;

    uint64_t recv_size;
    uint64_t send_size;
    char   recv_buffer[GDB_MAX_PKTSIZE];
    char   send_buffer[GDB_MAX_PKTSIZE];
} gdb_client_t;

struct gdb_server {
    thread_ctx_t* thread;
    net_sock_t* shut[2];
    net_poll_t* poll;
    net_sock_t* listener;
    vector_t(gdb_client_t*) clients;
    rvvm_machine_t* machine;
    spinlock_t lock;
    uint8_t swbreak_state;
};

/*
 * Raw GDB protocol handling and serialization
 */

static inline char gdb_hexify(uint8_t nibble)
{
    if (nibble < 10) return '0' + nibble;
    if (nibble < 16) return 'a' + nibble - 10;
    return '?';
}

static void gdb_byte_to_hex(char* hex, uint8_t byte)
{
    hex[0] = gdb_hexify((byte >> 4) & 0xF);
    hex[1] = gdb_hexify(byte & 0xF);
}

static uint64_t gdb_bytes_to_hex_le(char* hex, const void* data, uint64_t bytes)
{
    for (uint64_t i = 0; i < bytes; ++i) {
        gdb_byte_to_hex(hex + (i * 2), ((uint8_t*)data)[i]);
    }
    return bytes * 2;
}

static uint64_t gdb_value_to_hex_le(char* hex, uint64_t val, uint64_t bytes)
{
    for (uint64_t i = 0; i < bytes; ++i) {
        gdb_byte_to_hex(hex + (i * 2), val >> (i * 8));
    }
    return bytes * 2;
}

static inline uint8_t gdb_hex_nibble(char digit)
{
    if (digit >= '0' && digit <= '9') return digit - '0';
    if (digit >= 'A' && digit <= 'F') return digit - 'A' + 10;
    if (digit >= 'a' && digit <= 'f') return digit - 'a' + 10;
    return -1;
}

static uint8_t gdb_hex_to_byte(const char* hex)
{
    return (gdb_hex_nibble(hex[0]) << 4) | gdb_hex_nibble(hex[1]);
}

static void gdb_hex_to_bytes_le(void* data, const char* hex, uint64_t bytes)
{
    for (uint64_t i = 0; i < bytes; ++i) {
        ((uint8_t*)data)[i] = gdb_hex_to_byte(hex + (i * 2));
    }
}

static uint64_t gdb_hex_to_value_le(const char* hex, uint64_t bytes)
{
    uint64_t val = 0;
    for (uint64_t i = bytes; i--;) {
        val = (val << 8) | gdb_hex_to_byte(hex + (i * 2));
    }
    return val;
}

static void gdb_reply_ack(gdb_client_t* gdb)
{
    net_tcp_send(gdb->sock, "+", 1);
}

static void gdb_reply_nak(gdb_client_t* gdb)
{
    net_tcp_send(gdb->sock, "-", 1);
}

static void gdb_resend_reply(gdb_client_t* gdb)
{
    net_tcp_send(gdb->sock, gdb->send_buffer, gdb->send_size);
}

static void gdb_send_buffer_append(gdb_client_t* gdb, const char* str)
{
    uint64_t size = sizeof(gdb->send_buffer) - gdb->send_size;
    gdb->send_size += rvvm_strlcpy(gdb->send_buffer + gdb->send_size, str, size);
}

static void gdb_reply_str(gdb_client_t* gdb, const char* str)
{
    uint8_t csum = 0;
    char csum_buf[3] = {0};
    gdb->send_size = 0;

    gdb_send_buffer_append(gdb, "$");
    gdb_send_buffer_append(gdb, str);
    gdb_send_buffer_append(gdb, "#");

    // Calculate checksum
    for (uint64_t i = 0; str[i]; ++i) {
        csum += (uint8_t)str[i];
    }

    gdb_value_to_hex_le(csum_buf, csum, sizeof(csum));
    gdb_send_buffer_append(gdb, csum_buf);

    gdb_resend_reply(gdb);

    rvvm_debug("Reply: %s", str);
}

static void gdb_consume_bytes(gdb_client_t* gdb, uint64_t bytes)
{
    if (bytes > gdb->recv_size) {
        bytes = gdb->recv_size;
    }
    gdb->recv_size -= bytes;
    memmove(gdb->recv_buffer, gdb->recv_buffer + bytes, gdb->recv_size);
}

/*
 * GDB command handling
 */

static void gdb_halt_reason(gdb_client_t* gdb)
{
    // Report SIGTRAP, otherwise GDB might be confused
    gdb_reply_str(gdb, "S05");
}

static void gdbstub_interrupt(gdb_server_t* server)
{
    vector_foreach(server->machine->harts, i) {
        rvvm_hart_t* vm = vector_at(server->machine->harts, i);
        riscv_hart_queue_pause(vm);
    }
    vector_foreach(server->clients, i) {
        gdb_client_t* gdb = vector_at(server->clients, i);
        gdb_halt_reason(gdb);
    }
}

bool gdbstub_halt(gdb_server_t* server)
{
    spin_lock(&server->lock);
    bool halt = vector_size(server->clients) && server->swbreak_state < 2;
    if (halt) {
        gdbstub_interrupt(server);
        server->swbreak_state = 1;
    }
    spin_unlock(&server->lock);
    return halt;
}

static void gdbstub_pause(gdb_server_t* server)
{
    spin_unlock(&server->lock);
    vector_foreach(server->machine->harts, i) {
        rvvm_hart_t* vm = vector_at(server->machine->harts, i);
        riscv_hart_pause(vm);
    }
    if (server->swbreak_state == 1) {
        server->swbreak_state = 2;
    }
    spin_lock(&server->lock);
}

static void gdb_continue(gdb_server_t* server)
{
    vector_foreach(server->machine->harts, i) {
        rvvm_hart_t* vm = vector_at(server->machine->harts, i);
        riscv_hart_spawn(vm);
    }
}

static void gdb_report_regs(gdb_client_t* gdb)
{
    if (gdb->hart) {
        char buffer[1024] = {0};
        uint64_t size = 0;
        for (uint64_t x = 0; x < 33; ++x) {
            if (gdb->server->machine->rv64) {
                size += gdb_value_to_hex_le(buffer + size, gdb->hart->registers[x], 8);
            } else {
                size += gdb_value_to_hex_le(buffer + size, gdb->hart->registers[x], 4);
            }
        }

        gdb_reply_str(gdb, buffer);
        return;
    }
    gdb_reply_str(gdb, "E.Invalid CPU");
}

static void gdb_write_regs(gdb_client_t* gdb, const char* packet, uint64_t size)
{
    if (gdb->hart) {
        uint64_t cur = 1;
        for (uint64_t x = 0; x < 33; ++x) {
            if (gdb->server->machine->rv64) {
                gdb->hart->registers[x] = gdb_hex_to_value_le(packet + cur, 8);
                cur += 16;
            } else {
                gdb->hart->registers[x] = (int32_t)gdb_hex_to_value_le(packet + cur, 4);
                cur += 8;
            }
            if (cur >= size) {
                break;
            }
        }

        gdb_reply_str(gdb, "OK");
        return;
    }
    gdb_reply_str(gdb, "E.Invalid CPU");

}

static void gdb_read_memory(gdb_client_t* gdb, uint64_t addr, uint64_t bytes)
{
    if (gdb->hart) {
        char reply[256] = {0};
        uint8_t buffer[64] = {0};
        bytes = EVAL_MIN(bytes, sizeof(buffer));

        if (riscv_mmu_op_helper(gdb->hart, addr, buffer, RISCV_MMU_ATTR_DEBUG, bytes, RISCV_MMU_READ)) {
            gdb_bytes_to_hex_le(reply, buffer, bytes);
            gdb_reply_str(gdb, reply);
            return;
        }
    }
    gdb_reply_str(gdb, "E00");
}

static void gdb_write_memory(gdb_client_t* gdb, uint64_t addr, const char* hex, uint64_t bytes)
{
    if (gdb->hart) {
        uint8_t buffer[64] = {0};
        bytes = EVAL_MIN(bytes, sizeof(buffer));
        gdb_hex_to_bytes_le(buffer, hex, bytes);

        if (riscv_mmu_op_helper(gdb->hart, addr, buffer, RISCV_MMU_ATTR_DEBUG, bytes, RISCV_MMU_WRITE)) {
            gdb_reply_str(gdb, "OK");
            gdb->server->swbreak_state = 0;
            return;
        }
    }
    gdb_reply_str(gdb, "E00");
}

static void gdb_handle_m(gdb_client_t* gdb, const char* packet, uint64_t size)
{
    uint64_t cur = 1;
    uint64_t num_size = size - cur;
    uint64_t mem_addr = str_to_uint_base(packet + cur, &num_size, 16);
    if (num_size == 0) {
        gdb_reply_nak(gdb);
        return;
    }
    cur += num_size;
    if (packet[cur] != ',') {
        gdb_reply_nak(gdb);
        return;
    }
    cur += 1;
    num_size = size - cur;
    uint32_t mem_size = str_to_uint_base(packet + cur, &num_size, 16);
    if (num_size == 0) {
        gdb_reply_nak(gdb);
        return;
    }
    cur += num_size;

    if (packet[0] == 'm') {
        gdb_read_memory(gdb, mem_addr, mem_size);
    }  else if (packet[0] == 'M') {
        if (packet[cur] != ':') {
            gdb_reply_nak(gdb);
            return;
        }
        cur += 1;
        mem_size = EVAL_MIN(mem_size, (size - cur) / 2);
        gdb_write_memory(gdb, mem_addr, packet + cur, mem_size);
    } else {
        gdb_reply_str(gdb, "");
    }
}

static void gdb_select_thread(gdb_client_t* gdb, uint64_t thread_id)
{
    rvvm_machine_t* machine = gdb->server->machine;
    rvvm_hart_t* hart = NULL;
    if (thread_id < vector_size(machine->harts)) {
        hart = vector_at(machine->harts, thread_id);
    }
    gdb->hart = hart;
}

static void gdb_handle_h(gdb_client_t* gdb, const char* packet, uint64_t size)
{
    if (packet[1] == 'g') {
        uint64_t strsize = size - 2;
        uint64_t thread_id = str_to_int_base(packet + 2, &strsize, 16);
        gdb_select_thread(gdb, thread_id);
    }
    gdb_reply_str(gdb, "OK");
}

static void gdb_handle_q(gdb_client_t* gdb, const char* packet, uint64_t size)
{
    UNUSED(size);
    if (rvvm_strfind(packet, "qfThreadInfo")) {
        rvvm_machine_t* machine = gdb->server->machine;
        char str[256] = "m";
        uint64_t cur = rvvm_strlen(str);
        vector_foreach(machine->harts, i) {
            char id[16] = {0};
            int_to_str_dec(id, sizeof(id), i);
            cur += rvvm_strlcpy(str + cur, id, sizeof(str) - cur);
            if (i < vector_size(machine->harts) - 1) {
                cur += rvvm_strlcpy(str + cur, ",", sizeof(str) - cur);
            }
        }
        gdb_reply_str(gdb, str);
    } else if (rvvm_strfind(packet, "qsThreadInfo")) {
        gdb_reply_str(gdb, "l");
    } else {
        gdb_reply_str(gdb, "");
    }
}

static void gdb_handle_packet(gdb_client_t* gdb, const char* packet, uint64_t size)
{
    rvvm_debug("Packet: %.*s", (uint32_t)size, packet);

    gdbstub_pause(gdb->server);

    switch (packet[0]) {
        case 's': // Step
        case 'c': // Continue
            gdb_continue(gdb->server);
            return;
        case 'm': // Read memory
        case 'M': // Write memory
            gdb_handle_m(gdb, packet, size);
            return;
        case 'g': // Read general-purpose registers
            gdb_report_regs(gdb);
            return;
        case 'G': // Write general-purpose registers
            gdb_write_regs(gdb, packet, size);
            return;
        case 'T': // Thread status
            gdb_reply_str(gdb, "OK");
            return;
        case 'H': // Thread selection
            gdb_handle_h(gdb, packet, size);
            return;
        case '?': // Halt reason
            gdb_halt_reason(gdb);
            return;
        case 'q': // Query information
            gdb_handle_q(gdb, packet, size);
            return;
        case 'R': // Reset
            rvvm_reset_machine(gdb->server->machine, true);
            gdb_reply_str(gdb, "OK");
            return;
        default:
            gdb_reply_str(gdb, "");
            return;
    }
}

static bool gdb_parse_packet(gdb_client_t* gdb)
{
    for (uint64_t i = 0; i < gdb->recv_size; ++i) {
        if (gdb->recv_buffer[i] == '#' && i + 3 <= gdb->recv_size) {
            // Ignore checksum
            gdb_reply_ack(gdb);
            gdb_handle_packet(gdb, gdb->recv_buffer + 1, i - 1);
            gdb_consume_bytes(gdb, i + 3);
            return true;
        }
    }

    if (gdb->recv_size >= GDB_MAX_PKTSIZE) {
        DO_ONCE(rvvm_warn("gdbstub buffer overrun!"));
        gdb_reply_nak(gdb);
        gdb_consume_bytes(gdb, gdb->recv_size);
    }
    return false;
}

static void gdb_close(gdb_client_t* gdb)
{
    net_sock_close(gdb->sock);
    free(gdb);
}

static void gdbstrub_recv(gdb_server_t* server, gdb_client_t* gdb)
{
    uint64_t size = sizeof(gdb->recv_buffer) - gdb->recv_size;
    int32_t ret = net_tcp_recv(gdb->sock, gdb->recv_buffer + gdb->recv_size, size);

    if (ret < 0) {
        // Error, consider connection closed
        vector_foreach(server->clients, j) {
            if (vector_at(server->clients, j) == gdb) {
                vector_erase(server->clients, j);
            }
        }
        gdb_close(gdb);
        rvvm_info("gdb client disconnect: %d", ret);
    } else {
        gdb->recv_size += ret;
        while (gdb->recv_size) {
            switch (gdb->recv_buffer[0]) {
                case '$': // Packet received
                    if (!gdb_parse_packet(gdb)) {
                        // Incomplete packet sequence, wait for more data
                        return;
                    }
                    break;
                case '+': // ACK
                    gdb_consume_bytes(gdb, 1);
                    break;
                case '-': // NAK
                    gdb_resend_reply(gdb);
                    gdb_consume_bytes(gdb, 1);
                    break;
                case 0x03: // Ctrl+C
                    gdbstub_interrupt(gdb->server);
                    gdb_consume_bytes(gdb, 1);
                    break;
                default:
                    DO_ONCE(rvvm_warn("gdbstub desync: %02x", gdb->recv_buffer[0]));
                    gdb_reply_nak(gdb);
                    gdb_consume_bytes(gdb, 1);
                    break;
            }
        }
    }
}

static void gdbstub_accept(gdb_server_t* server)
{
    net_sock_t* sock = net_tcp_accept(server->listener);
    if (sock) {
        gdb_client_t* gdb = safe_new_obj(gdb_client_t);
        net_event_t ev = {
            .flags = NET_POLL_RECV,
            .data = gdb,
        };
        gdb->server = server;
        gdb->sock = sock;

        if (!net_poll_add(server->poll, gdb->sock, &ev)) {
            rvvm_warn("net_poll_add() failed!");
            gdb_close(gdb);
        } else {
            rvvm_info("gdb client connected");
            vector_push_back(server->clients, gdb);
        }

        gdb_select_thread(gdb, 0);
    }
}

static bool gdbstub_tick(gdb_server_t* server, uint32_t timeout)
{
    net_event_t events[16] = {0};
    uint64_t nevents = net_poll_wait(server->poll, events, STATIC_ARRAY_SIZE(events), timeout);
    spin_lock(&server->lock);
    for (uint64_t i = 0; i < nevents; ++i) {
        if (events[i].data == server->listener) {
            // Accept new GDB client
            gdbstub_accept(server);
        } else if (events[i].data) {
            // GDB client event
            gdb_client_t* gdb = events[i].data;
            gdbstrub_recv(server, gdb);
        } else {
            spin_unlock(&server->lock);
            return false;
        }
    }
    spin_unlock(&server->lock);
    return true;
}

static void* gdbstub_thread(void* arg)
{
    gdb_server_t* server = arg;
    while (gdbstub_tick(server, NET_POLL_INF)) {}
    return NULL;
}

static void gdbstub_free(gdb_server_t* server)
{
    // Shutdown the gdb server thread
    net_sock_close(server->shut[1]);
    thread_join(server->thread);
    net_sock_close(server->shut[0]);

    // Free clients
    vector_foreach(server->clients, i) {
        gdb_close(vector_at(server->clients, i));
    }
    vector_free(server->clients);

    // Close server
    net_sock_close(server->listener);
    net_poll_close(server->poll);
    free(server);
}

static gdb_server_t* gdbstub_create(const char* bind)
{
    net_addr_t addr = {
        .port = 1234,
        .ip = {127, 0, 0, 1},
    };
    gdb_server_t* server = safe_new_obj(gdb_server_t);

    if (bind) {
        net_parse_addr(&addr, bind);
    }

    server->listener = net_tcp_listen(&addr);
    server->poll = net_poll_create();

    net_event_t ev_listener = {
        .flags = NET_POLL_RECV,
        .data = server->listener,
    };

    if (!net_poll_add(server->poll, server->listener, &ev_listener)) {
        gdbstub_free(server);
        return NULL;
    }

    net_tcp_sockpair(server->shut);

    net_event_t ev_shut = {
        .flags = NET_POLL_RECV,
        .data = NULL,
    };

    if (!net_poll_add(server->poll, server->shut[0], &ev_shut)) {
        gdbstub_free(server);
        return NULL;
    }

    server->thread = thread_create(gdbstub_thread, server);

    return server;
}

static void gdbstub_remove(rvvm_mmio_dev_t* dev)
{
    gdb_server_t* server = dev->data;
    gdbstub_free(server);
}

static const rvvm_mmio_type_t gdbstub_dev_type = {
    .name = "gdbstub",
    .remove = gdbstub_remove,
};

bool gdbstub_init(rvvm_machine_t* machine, const char* bind)
{
    gdb_server_t* server = gdbstub_create(bind);
    if (!server) {
        rvvm_error("Failed to bind GDB stub at %s", bind ? bind : ":1234");
        return false;
    }

    server->machine = machine;

    rvvm_mmio_dev_t gdbstub_dev = {
        .data = server,
        .type = &gdbstub_dev_type,
    };

    if (!rvvm_attach_mmio(machine, &gdbstub_dev)) {
        return false;
    }

    machine->gdbstub = server;
    return true;
}

#else

bool gdbstub_init(rvvm_machine_t* machine, const char* bind)
{
    UNUSED(machine); UNUSED(bind);
    return false;
}

bool gdbstub_halt(gdb_server_t* server)
{
    UNUSED(server);
    return false;
}

#endif
