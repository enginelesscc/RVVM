/* SPDX-License-Identifier: MPL-2.0 */

#include "riscv_pmu.h"
#include "riscv_csr.h"
#include "riscv_hart.h"

static bool riscv_pmu_filtered(uint64_t event, uint8_t priv)
{
    unsigned bit = priv == RISCV_PRIV_MACHINE ? 62 : (priv == RISCV_PRIV_SUPERVISOR ? 61 : 60);
    return !!(event & (1ULL << bit));
}

static void riscv_pmu_add(rvvm_hart_t* vm, unsigned counter, uint64_t amount)
{
    uint64_t old              = vm->pmu.counter[counter];
    vm->pmu.counter[counter] += amount;
    if (counter >= 3 && vm->pmu.counter[counter] < old && !(vm->pmu.event[counter] & RISCV_PMU_OF)) {
        vm->pmu.event[counter] |= RISCV_PMU_OF;
        vm->csr.ip             |= 1U << RISCV_INTERRUPT_COUNTER_OVF;
        riscv_hart_check_interrupts(vm);
    }
}

static void riscv_pmu_events(rvvm_hart_t* vm, uint8_t priv, uint32_t inhibit, uint64_t cycles, uint32_t retired,
                             uint32_t branches)
{
    for (unsigned i = 3; i < RISCV_PMU_COUNTERS; ++i) {
        uint64_t event = vm->pmu.event[i];
        if (!(inhibit & (1U << i)) && !riscv_pmu_filtered(event, priv)) {
            switch (event & 0xffff) {
                case 1:
                    riscv_pmu_add(vm, i, cycles);
                    break;
                case 2:
                    riscv_pmu_add(vm, i, retired);
                    break;
                case 5:
                    riscv_pmu_add(vm, i, branches);
                    break;
            }
        }
    }
}

void riscv_pmu_update(rvvm_hart_t* vm)
{
    uint64_t now      = rvtimer_clocksource(RISCV_PMU_CYCLE_HZ);
    uint64_t cycles   = now - vm->pmu.clock;
    uint32_t retired  = vm->pmu_retired;
    uint32_t branches = vm->pmu_branches;
    vm->pmu.clock     = now;
    vm->pmu_retired = vm->pmu_branches = 0;
    if (!(vm->pmu.inhibit & 1)) {
        vm->pmu.counter[0] += cycles;
    }
    if (!(vm->pmu.inhibit & 4)) {
        vm->pmu.counter[2] += retired;
    }
    if (vm->pmu_budget == 1) {
        riscv_pmu_events(vm, vm->priv_mode, vm->pmu.inhibit, cycles, retired, branches);
    }
}

uint64_t riscv_pmu_delay_ns(rvvm_hart_t* vm)
{
    uint64_t delay = CONDVAR_INFINITE;
    riscv_pmu_update(vm);
    for (unsigned i = 3; i < RISCV_PMU_COUNTERS; ++i) {
        uint64_t event = vm->pmu.event[i];
        if (!(vm->pmu.inhibit & (1U << i)) && (event & 0xffff) == 1 && !(event & RISCV_PMU_OF)
            && !riscv_pmu_filtered(event, vm->priv_mode)) {
            uint64_t remaining = 0 - vm->pmu.counter[i];
            if (remaining) {
                delay = EVAL_MIN(delay, remaining / 3 + !!(remaining % 3));
            }
        }
    }
    return delay;
}

void riscv_pmu_retire(rvvm_hart_t* vm, uint8_t priv, uint32_t inhibit, uint32_t insn)
{
    if (!(inhibit & 4) && !vm->pmu.wrote_instret) {
        vm->pmu.counter[2]++;
    }
    if (vm->pmu_budget == 1) {
        bool branch = (insn & 0x7f) == 0x63 || ((insn & 3) == 1 && (insn & 0xc000) == 0xc000);
        riscv_pmu_events(vm, priv, inhibit, 0, 1, branch);
        riscv_pmu_update(vm);
    }
}

void riscv_pmu_reconfigure(rvvm_hart_t* vm)
{
    vm->pmu_budget = 4096;
    for (unsigned i = 3; i < RISCV_PMU_COUNTERS; ++i) {
        if (!(vm->pmu.inhibit & (1U << i)) && (vm->pmu.event[i] & 0xffff)) {
            vm->pmu_budget = 1;
        }
    }
}

void riscv_pmu_init(rvvm_hart_t* vm)
{
    memset(&vm->pmu, 0, sizeof(vm->pmu));
    vm->pmu_retired = vm->pmu_branches = 0;
    vm->pmu.clock                      = rvtimer_clocksource(RISCV_PMU_CYCLE_HZ);
    vm->pmu.counter[0]                 = vm->pmu.clock;
    riscv_pmu_reconfigure(vm);
}

static uint64_t riscv_pmu_modify(uint64_t old, rvvm_uxlen_t value, uint8_t op)
{
    switch (op) {
        case CSR_SETBITS:
            return old | value;
        case CSR_CLEARBITS:
            return old & ~value;
        default:
            return value;
    }
}

bool riscv_pmu_csr(rvvm_hart_t* vm, uint32_t csr, rvvm_uxlen_t* dest, uint8_t op, bool write)
{
    bool      high  = (csr >= 0xb80 && csr <= 0xb9f) || (csr >= 0xc80 && csr <= 0xc9f);
    unsigned  index = csr & 31;
    uint64_t* reg   = NULL;
    bool      event = (csr >= 0x323 && csr <= 0x33f) || (csr >= 0x723 && csr <= 0x73f);
    bool      user  = (csr & 0xf00) == 0xc00;
    if (event) {
        high = csr >= 0x700;
    } else if (user) {
        if (!riscv_csr_counter_enabled(vm, index)) {
            return false;
        }
    }
    if (high && vm->rv64) {
        return false;
    }
    riscv_pmu_update(vm);
    if (csr == CSR_SCOUNTOVF) {
        uint32_t overflow = 0;
        for (unsigned i = 3; i < RISCV_PMU_COUNTERS; ++i) {
            if ((vm->pmu.event[i] & RISCV_PMU_OF) && riscv_csr_counter_enabled(vm, i)) {
                overflow |= 1U << i;
            }
        }
        *dest = overflow;
        return true;
    }
    if (csr == CSR_MCOUNTINHIBIT) {
        uint32_t old = vm->pmu.inhibit;
        if (write) {
            vm->pmu.inhibit = riscv_pmu_modify(old, *dest, op) & RISCV_PMU_MASK;
        }
        *dest = old;
        riscv_pmu_reconfigure(vm);
        return true;
    }
    if (index < RISCV_PMU_COUNTERS && index != 1) {
        reg = event ? &vm->pmu.event[index] : &vm->pmu.counter[index];
    }
    if (reg == NULL) {
        *dest = 0;
        return true;
    }
    uint64_t old  = *reg;
    uint64_t part = high ? old >> 32 : (vm->rv64 ? old : (uint32_t)old);
    if (write && !user) {
        uint64_t value = riscv_pmu_modify(part, *dest, op);
        if (!vm->rv64) {
            value = high ? ((old & 0xffffffff) | (value << 32)) : ((old & 0xffffffff00000000ULL) | (uint32_t)value);
        }
        if (event) {
            unsigned selector  = value & 0xffff;
            value             &= RISCV_PMU_OF | RISCV_PMU_FILTER;
            if (selector == 1 || selector == 2 || selector == 5) {
                value |= selector;
            }
        } else if (index == 2) {
            vm->pmu.wrote_instret = true;
        }
        *reg = value;
        riscv_pmu_reconfigure(vm);
    }
    *dest = part;
    return true;
}
