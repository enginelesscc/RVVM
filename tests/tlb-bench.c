/* SPDX-License-Identifier: MPL-2.0 */
/* Diskless RV64/Sv39 pointer-chasing benchmark. No host image mounting required. */
#include <core/rvvm.h>
#include <cpu/riscv_cpu.h>
#include <cpu/riscv_csr.h>
#include <cpu/riscv_hart.h>
#include <cpu/riscv_mmu.h>
#include <cpu/riscv_pmu.h>
#include <util/mem_ops.h>
#include <util/utils.h>
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifdef _WIN32
#include <windows.h>
#endif

#define CHECK(x) do { if (!(x)) { fprintf(stderr, "FAIL line %u: %s\n", __LINE__, #x); exit(1); } } while (0)

#ifdef _WIN32
static DWORD lock_privilege(bool remove)
{
    HANDLE token = NULL;
    LUID luid;
    CHECK(OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY | TOKEN_ADJUST_PRIVILEGES, &token));
    CHECK(LookupPrivilegeValueW(NULL, L"SeLockMemoryPrivilege", &luid));
    if (remove) {
        // Isolated test process only: simulate an account without the privilege.
        TOKEN_PRIVILEGES change = {0};
        change.PrivilegeCount = 1;
        change.Privileges[0].Luid = luid;
        change.Privileges[0].Attributes = SE_PRIVILEGE_REMOVED;
        CHECK(AdjustTokenPrivileges(token, FALSE, &change, 0, NULL, NULL));
    }
    DWORD size = 0, attributes = 0xffffffffUL;
    GetTokenInformation(token, TokenPrivileges, NULL, 0, &size);
    TOKEN_PRIVILEGES* privileges = malloc(size);
    CHECK(privileges && GetTokenInformation(token, TokenPrivileges, privileges, size, &size));
    for (DWORD i = 0; i < privileges->PrivilegeCount; ++i) {
        LUID candidate = privileges->Privileges[i].Luid;
        if (candidate.LowPart == luid.LowPart && candidate.HighPart == luid.HighPart) {
            attributes = privileges->Privileges[i].Attributes;
        }
    }
    free(privileges);
    CloseHandle(token);
    // Windows may set USED_FOR_ACCESS during allocation; compare the enabled state only.
    return attributes == 0xffffffffUL ? attributes : attributes & SE_PRIVILEGE_ENABLED;
}
#endif

static void run(rvvm_hart_t* vm, uint64_t address, uint64_t iterations)
{
    vm->registers[RISCV_REG_PC] = vm->mem.addr;
    vm->registers[10] = address;
    vm->registers[11] = iterations;
    vm->trap = false;
    do {
        vm->running = true;
        riscv_run_till_event(vm);
    } while (!vm->trap);
    riscv_pmu_update(vm);
    vm->running = false;
    CHECK(vm->csr.cause[RISCV_PRIV_USER] == RISCV_TRAP_BREAKPOINT);
    CHECK(vm->registers[11] == 0);
    vm->trap = false;
}

static void counters_test(rvvm_hart_t* vm)
{
    uint64_t address = vm->mem.addr + (64U << 20);
    uint64_t value = 0;
    riscv_tlb_flush(vm);
    memset(&vm->tlb_stats, 0, sizeof(vm->tlb_stats));
    vm->tlb_stats.enabled = true;
    riscv_load_u64(vm, address, 12);
    riscv_load_u64(vm, address, 12);
    CHECK(vm->tlb_stats.misses[0] == 1 && vm->tlb_stats.ram[0] == 1);
    // A misaligned access with a valid tag is a slow-path hit, not a miss.
    riscv_load_u64(vm, address + 1, 12);
    CHECK(vm->tlb_stats.misses[0] == 1 && vm->tlb_stats.slow_hits == 1);
    CHECK(riscv_mmu_op(vm, address, &value, 8, RISCV_MMU_WRITE));
    CHECK(vm->tlb_stats.ram[1] == 1);
    CHECK(riscv_mmu_op(vm, address, &value, 8, RISCV_MMU_WRITE));
    CHECK(vm->tlb_stats.ram[1] == 1);
    rvvm_addr_t physical = 0;
    CHECK(riscv_mmu_virt_translate(vm, address, &physical, RISCV_MMU_READ));
    CHECK(vm->tlb_stats.probes == 1 && vm->tlb_stats.misses[0] == 1);
    riscv_tlb_flush_page(vm, address);
    riscv_load_u64(vm, address, 12);
    CHECK(vm->tlb_stats.ram[0] == 2 && vm->tlb_stats.flush_page == 1);
    // Unmapped Sv39 VA generates a page fault rather than a RAM refill.
    riscv_load_u64(vm, 0x40000000, 12);
    CHECK(vm->trap && vm->tlb_stats.faults == 1 && vm->tlb_stats.ram[0] == 2);
    vm->trap = false;
    puts("PASS TLB counters: refill, hit, misalignment, write, probe, flush, page fault");
}

int main(int argc, char** argv)
{
    rvvm_set_args(argc, argv);
    size_t ram_size = rvvm_getarg_size("m");
    if (!ram_size) ram_size = 1U << 30;
#ifdef _WIN32
    DWORD privilege_before = lock_privilege(rvvm_has_arg("drop_lock_privilege"));
#endif
    rvvm_machine_t* machine = rvvm_create_machine(ram_size, 1, "rv64");
    CHECK(machine);
#ifdef _WIN32
    CHECK(lock_privilege(false) == privilege_before);
    if (rvvm_has_arg("drop_lock_privilege")) CHECK(machine->mem.large_page_size == 0);
#endif
    if (rvvm_has_arg("expect_ordinary")) CHECK(machine->mem.large_page_size == 0);
    rvvm_hart_t* vm = vector_at(machine->harts, 0);
    printf("BACKING %s page_size=%" PRIu64 " ram_bytes=%" PRIu64 "\n", vm->mem.large_page_size ? "large" : "ordinary",
           (uint64_t)vm->mem.large_page_size, (uint64_t)vm->mem.size);
    if (rvvm_has_arg("probe_only")) {
        // Check read/write/zero-fill and release even in the allocation-only probe.
        uint8_t* mem = vm->mem.data;
        CHECK(mem[0] == 0 && mem[ram_size - 1] == 0);
        mem[0] = 0x5a;
        mem[ram_size - 1] = 0xa5;
        CHECK(mem[0] == 0x5a && mem[ram_size - 1] == 0xa5);
        rvvm_free_machine(machine);
        return 0;
    }
    CHECK(ram_size == (1U << 30));
    // Prefault all RAM outside timing. Both allocation modes start fully resident.
    memset(vm->mem.data, 0, vm->mem.size);
    uint8_t* mem = vm->mem.data;
    uint64_t root = vm->mem.addr + 0x200000;
    uint64_t level1 = root + 0x1000;
    write_uint64_le(mem + 0x200000 + 2 * 8, ((level1 >> 12) << 10) | 1);
    for (unsigned i = 0; i < 512; ++i) {
        uint64_t leaf = root + 0x2000 + i * 4096;
        write_uint64_le(mem + 0x201000 + i * 8, ((leaf >> 12) << 10) | 1);
        for (unsigned j = 0; j < 512; ++j) {
            uint64_t pa = vm->mem.addr + ((uint64_t)i * 512 + j) * 4096;
            write_uint64_le(mem + 0x202000 + i * 4096 + j * 8, ((pa >> 12) << 10) | 0xcf);
        }
    }
    vm->root_page_table = root;
    vm->mmu_mode = CSR_SATP_MODE_SV39;
    vm->priv_mode = RISCV_PRIV_SUPERVISOR;
    vm->userland = true; // Return traps to this harness instead of installing a guest trap handler.
    counters_test(vm);
    // ld a0,0(a0); addi a1,a1,-1; bnez a1,-8; ebreak
    const uint32_t program[] = {0x00053503, 0xfff58593, 0xfe059ce3, 0x00100073};
    for (unsigned i = 0; i < 4; ++i) write_uint32_le(mem + 4 * i, program[i]);
    riscv_hart_prepare(vm);
    uint64_t iterations = rvvm_getarg_size("iterations");
    if (!iterations) iterations = 4000000;
    const char* names[] = {"tlb-fit-spread", "page-random", "cacheline-stream"};
    for (unsigned scenario = 0; scenario < 3; ++scenario) {
        size_t count = scenario == 0 ? 128 : scenario == 1 ? 65536 : (256U << 20) / 64;
        uint32_t* offsets = malloc(count * sizeof(*offsets));
        CHECK(offsets);
        for (size_t i = 0; i < count; ++i) {
            offsets[i] = (64U << 20) + (scenario == 0 ? i * (2U << 20) + (i + 64) * 4096
                                                 : scenario == 1 ? i * 4096 : i * 64);
        }
        if (scenario == 1) {
            uint32_t rng = 0x71b12345;
            for (size_t i = count - 1; i; --i) {
                rng ^= rng << 13; rng ^= rng >> 17; rng ^= rng << 5;
                size_t j = rng % (i + 1);
                uint32_t tmp = offsets[i]; offsets[i] = offsets[j]; offsets[j] = tmp;
            }
        }
        for (size_t i = 0; i < count; ++i) {
            write_uint64_le(mem + offsets[i], vm->mem.addr + offsets[(i + 1) % count]);
        }
        riscv_tlb_flush(vm);
        vm->tlb_stats.enabled = false;
        run(vm, vm->mem.addr + offsets[0], count); // Warm JIT, backing memory and caches.
        riscv_tlb_flush(vm);
        memset(&vm->tlb_stats, 0, sizeof(vm->tlb_stats));
        vm->tlb_stats.enabled = rvvm_has_arg("tlb_stats");
        uint64_t begin = rvtimer_clocksource(1000000000);
        run(vm, vm->mem.addr + offsets[0], iterations);
        uint64_t elapsed = rvtimer_clocksource(1000000000) - begin;
        CHECK(vm->registers[10] == vm->mem.addr + offsets[iterations % count]);
        if (vm->tlb_stats.enabled) {
            CHECK(vm->tlb_stats.faults == 0 && vm->tlb_stats.mmio == 0);
            if (scenario == 0) CHECK(vm->tlb_stats.ram[0] == (iterations < count ? iterations : count));
            if (scenario == 1) CHECK(vm->tlb_stats.ram[0] == iterations);
            if (scenario == 2) CHECK(vm->tlb_stats.ram[0] == (iterations + 63) / 64);
        }
        printf("RESULT {\"case\":\"%s\",\"backing\":\"%s\",\"profile\":%s,\"iterations\":%" PRIu64 ","
               "\"ns\":%" PRIu64 ",\"ram_read_misses\":%" PRIu64 ",\"exec_misses\":%" PRIu64 ",\"walks\":%" PRIu64 "}\n",
               names[scenario], vm->mem.large_page_size ? "large" : "ordinary",
               vm->tlb_stats.enabled ? "true" : "false", (uint64_t)iterations,
               (uint64_t)elapsed, (uint64_t)vm->tlb_stats.ram[0],
               (uint64_t)vm->tlb_stats.ram[2], (uint64_t)vm->tlb_stats.walks);
        fflush(stdout);
        free(offsets);
    }
    rvvm_free_machine(machine);
    return 0;
}
