/* SPDX-License-Identifier: MPL-2.0 */

#include <core/rvvm.h>
#include <cpu/riscv_cpu.h>
#include <cpu/riscv_csr.h>
#include <cpu/riscv_hart.h>
#include <cpu/riscv_pmu.h>
#include <rvvm/rvvm_blk.h>
#include <stdio.h>
#include <stdlib.h>
#include <util/mem_ops.h>

#define CHECK(x)                                                                                                       \
    do {                                                                                                               \
        if (!(x)) {                                                                                                    \
            fprintf(stderr, "FAIL line %u: %s\n", __LINE__, #x);                                                       \
            exit(1);                                                                                                   \
        }                                                                                                              \
    } while (0)

static uint64_t get(rvvm_hart_t* vm, unsigned csr)
{
    rvvm_uxlen_t value = 0;
    CHECK(riscv_csr_op(vm, csr, &value, CSR_SETBITS, false));
    return value;
}

static void put(rvvm_hart_t* vm, unsigned csr, uint64_t value)
{
    rvvm_uxlen_t x = value;
    CHECK(riscv_csr_op(vm, csr, &x, CSR_SWAP, true));
}

static void execute(rvvm_hart_t* vm, const uint32_t* program, size_t words)
{
    riscv_jit_flush_cache(vm);
    for (size_t i = 0; i < words; ++i) {
        write_uint32_le((uint8_t*)vm->mem.data + i * 4, program[i]);
    }
    vm->registers[RISCV_REG_PC] = vm->mem.addr;
    vm->trap                    = false;
    vm->userland                = true;
    vm->running                 = true;
    do {
        riscv_run_till_event(vm);
        if (!vm->trap) {
            vm->running = true;
        }
    } while (!vm->trap);
    riscv_pmu_update(vm);
    vm->trap    = false;
    vm->running = false;
}

static void test_execution(rvvm_hart_t* vm, bool jit)
{
    rvvm_set_opt(vm->machine, RVVM_OPT_JIT, jit);
    if (jit) {
        riscv_hart_prepare(vm);
    }
#ifdef USE_JIT
    vm->jit_enabled = jit;
#endif
    riscv_pmu_init(vm);
    put(vm, CSR_MCOUNTINHIBIT, 1);
    put(vm, CSR_MINSTRET, 0);
    put(vm, 0x323, 2);
    put(vm, 0x324, 5);
    const uint32_t loop[] = {0x3e800293, 0x00000313, 0x00130313, 0xfff28293, 0xfe029ce3, 0x00100073};
    execute(vm, loop, sizeof(loop) / 4);
    CHECK(vm->registers[6] == 1000);
    CHECK(get(vm, CSR_MINSTRET) == 3002);
    CHECK(get(vm, 0xb03) == 3002);
    CHECK(get(vm, 0xb04) == 1000);
    CHECK(get(vm, CSR_INSTRET) == get(vm, CSR_MINSTRET));
    put(vm, 0x323, 0);
    put(vm, 0x324, 0);
    put(vm, CSR_MINSTRET, 0);
    execute(vm, loop, sizeof(loop) / 4);
    CHECK(get(vm, CSR_MINSTRET) == 3002);
    /* A CSR read must include JIT work still below the accounting budget. */
    const uint32_t read_pending[] = {0x06400293, 0xfff28293, 0xfe029ee3, 0xb0202373, 0x00100073};
    put(vm, CSR_MINSTRET, 0);
    execute(vm, read_pending, sizeof(read_pending) / 4);
    CHECK(vm->registers[6] == 201);
    CHECK(get(vm, CSR_MINSTRET) == 202);
    const uint32_t loads[] = {0x00800293, 0x000013b7, 0x00053583, 0x00750533, 0xfff28293, 0xfe029ae3, 0x00100073};
    vm->registers[10]      = vm->mem.addr;
    put(vm, CSR_MINSTRET, 0);
    execute(vm, loads, sizeof(loads) / 4);
    CHECK(get(vm, CSR_MINSTRET) == 34);
    uint32_t pressure[64];
    size_t   n    = 0;
    pressure[n++] = 0x3e800293;
    pressure[n++] = 0x00000393;
    for (unsigned r = 8; r < 32; ++r) {
        pressure[n++] = (r << 7) | 0x13;
    }
    size_t loop_start = n;
    for (unsigned r = 8; r < 32; ++r) {
        pressure[n++] = 0x00100013 | (r << 15) | (r << 7);
    }
    pressure[n++]   = 0x0012f313;
    pressure[n++]   = 0x00030463;
    pressure[n++]   = 0x00138393;
    pressure[n++]   = 0xfff28293;
    unsigned offset = (unsigned)((loop_start - n) * 4);
    pressure[n++]   = 0x00029063 | ((offset & 0x1000) << 19) | ((offset & 0x7e0) << 20) | ((offset & 0x1e) << 7)
                    | ((offset & 0x800) >> 4);
    pressure[n++]   = 0x00100073;
    put(vm, CSR_MINSTRET, 0);
    execute(vm, pressure, n);
    CHECK(get(vm, CSR_MINSTRET) == 28526);
    CHECK(vm->registers[7] == 500 && vm->registers[31] == 1000);
    const uint32_t compressed[] = {0x00010001, 0x00019002};
    put(vm, CSR_MINSTRET, 0);
    execute(vm, compressed, sizeof(compressed) / 4);
    CHECK(get(vm, CSR_MINSTRET) == 2);
    const uint32_t fault[] = {0x00100313, 0xffffffff, 0x00100073};
    put(vm, CSR_MINSTRET, 0);
    execute(vm, fault, sizeof(fault) / 4);
    CHECK(get(vm, CSR_MINSTRET) == 1);
    const uint32_t selfwrite[] = {0x00900293, 0xb0229073, 0x00100073};
    execute(vm, selfwrite, sizeof(selfwrite) / 4);
    CHECK(get(vm, CSR_MINSTRET) == 9);
    printf("PASS %s exact retirement, branches, linked blocks, faults, counter writes\n", jit ? "JIT" : "interpreter");
}

int main(void)
{
    rvvm_machine_t* machine = rvvm_create_machine(16U << 20, 2, "rv64");
    CHECK(machine != NULL);
    rvvm_hart_t* vm    = vector_at(machine->harts, 0);
    rvvm_hart_t* other = vector_at(machine->harts, 1);
    put(vm, CSR_MCOUNTEREN, ~0ULL);
    put(vm, CSR_SCOUNTEREN, ~0ULL);
    CHECK(get(vm, CSR_MCOUNTEREN) == 0x7f);
    put(vm, CSR_MCOUNTINHIBIT, ~0ULL);
    CHECK(get(vm, CSR_MCOUNTINHIBIT) == RISCV_PMU_MASK);
    put(vm, CSR_MCYCLE, 123);
    CHECK(get(vm, CSR_MCYCLE) == 123 && get(vm, CSR_CYCLE) == 123);
    put(vm, CSR_MINSTRET, 456);
    CHECK(get(vm, CSR_INSTRET) == 456);
    CHECK(get(other, CSR_MINSTRET) == 0);
    put(vm, CSR_MCOUNTINHIBIT, 0);
    uint64_t start = get(vm, CSR_CYCLE);
    sleep_ms(10);
    uint64_t delta = get(vm, CSR_CYCLE) - start;
    CHECK(delta >= 15000000 && delta < 3000000000ULL);
    put(vm, CSR_MCOUNTINHIBIT, 1);
    start = get(vm, CSR_CYCLE);
    sleep_ms(2);
    CHECK(get(vm, CSR_CYCLE) == start);
    put(vm, 0x323, 2);
    put(vm, 0xb03, ~0ULL);
    riscv_pmu_retire(vm, RISCV_PRIV_MACHINE, 1, 0x13);
    CHECK(get(vm, 0xb03) == 0);
    CHECK(get(vm, CSR_SCOUNTOVF) == 8);
    CHECK(vm->csr.ip & (1U << 13));
    put(vm, CSR_MIP, 0);
    put(vm, 0xb03, ~0ULL);
    riscv_pmu_retire(vm, RISCV_PRIV_MACHINE, 1, 0x13);
    CHECK(!(vm->csr.ip & (1U << 13)));
    put(vm, 0x323, 2);
    put(vm, 0xb03, ~0ULL);
    riscv_pmu_retire(vm, RISCV_PRIV_MACHINE, 1, 0x13);
    CHECK(vm->csr.ip & (1U << 13));
    put(vm, 0x323, 2 | (1ULL << 62));
    put(vm, 0xb03, 0);
    riscv_pmu_retire(vm, RISCV_PRIV_MACHINE, 1, 0x13);
    CHECK(get(vm, 0xb03) == 0);
    riscv_pmu_retire(vm, RISCV_PRIV_SUPERVISOR, 1, 0x13);
    CHECK(get(vm, 0xb03) == 1);
    put(vm, CSR_MCOUNTEREN, 0);
    vm->priv_mode    = RISCV_PRIV_SUPERVISOR;
    rvvm_uxlen_t tmp = 0;
    CHECK(!riscv_csr_op(vm, CSR_CYCLE, &tmp, CSR_SETBITS, false));
    CHECK(get(vm, CSR_SCOUNTOVF) == 0);
    vm->priv_mode = RISCV_PRIV_MACHINE;
    put(vm, CSR_MCOUNTEREN, 0x7f);
    put(vm, CSR_MIDELEG, 1U << 13);
    CHECK(get(vm, CSR_MIDELEG) == (1U << 13));
    vm->rv64 = false;
    put(vm, CSR_MCOUNTINHIBIT, RISCV_PMU_MASK);
    put(vm, CSR_MCYCLEH, 0x12345678);
    put(vm, CSR_MCYCLE, 0x87654321);
    CHECK((uint32_t)get(vm, CSR_CYCLEH) == 0x12345678);
    CHECK((uint32_t)get(vm, CSR_CYCLE) == 0x87654321);
    put(vm, 0x723, 0x80000000);
    CHECK((uint32_t)get(vm, 0x723) == 0x80000000);
    vm->rv64 = true;
    CHECK(!riscv_csr_op(vm, CSR_CYCLEH, &tmp, CSR_SETBITS, false));
    printf("PASS CSR aliases, virtual clock, inhibit, per-hart state, overflow/rearm, privilege filters, access, RV32 "
           "halves\n");
    test_execution(vm, false);
#ifdef USE_JIT
    test_execution(vm, true);
#endif
    const char*     snapshot_path = "rvvm-pmu-test.snapshot";
    rvvm_blk_dev_t* snapshot
        = rvvm_blk_open(snapshot_path, "raw", RVVM_BLK_RW | RVVM_BLK_CREAT | RVVM_BLK_TRUNC | RVVM_BLK_GROW);
    CHECK(snapshot != NULL);
    machine->power_state = 1;
    put(vm, CSR_MCOUNTINHIBIT, RISCV_PMU_MASK);
    put(vm, CSR_MCYCLE, 123456);
    put(vm, 0xb03, 654321);
    put(vm, 0x323, RISCV_PMU_OF | 2);
    put(vm, 0x326, 1);
    put(vm, 0xb06, ~0ULL);
    put(vm, CSR_MCOUNTINHIBIT, RISCV_PMU_MASK & ~(1U << 6));
    vm->csr.ip = 0;
    CHECK(rvvm_machine_snapshot(machine, snapshot, true));
    vm->csr.ip = 0;
    put(vm, CSR_MCYCLE, 0);
    put(vm, 0xb03, 0);
    put(vm, 0x323, 0);
    CHECK(rvvm_machine_snapshot(machine, snapshot, false));
    CHECK(get(vm, CSR_MCYCLE) == 123456);
    CHECK(get(vm, 0xb03) == 654321);
    CHECK(get(vm, 0x323) == (RISCV_PMU_OF | 2));
    CHECK(get(vm, CSR_MCOUNTINHIBIT) == (RISCV_PMU_MASK & ~(1U << 6)));
    CHECK(vm->csr.ip & (1U << RISCV_INTERRUPT_COUNTER_OVF));
    CHECK(get(vm, 0x326) & RISCV_PMU_OF);
    rvvm_blk_close(snapshot);
    CHECK(remove(snapshot_path) == 0);
    puts("PASS PMU snapshot round trip");
    rvvm_free_machine(machine);
    puts("PASS all PMU tests");
    return 0;
}
