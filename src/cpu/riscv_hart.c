/*
riscv_hart.c - RISC-V Hardware Thread
Copyright (C) 2021  LekKit <github.com/LekKit>

This Source Code Form is subject to the terms of the Mozilla Public
License, v. 2.0. If a copy of the MPL was not distributed with this
file, You can obtain one at https://mozilla.org/MPL/2.0/.
*/

#include <rvvm/rvvm.h>

#include <util/atomics.h>
#include <util/bit_ops.h>
#include <util/threading.h>
#include <util/utils.h>
#include <util/vma_ops.h>

#include <core/gdbstub.h>
#include <core/rvvm_isolation.h>

#include <cpu/riscv_cpu.h>
#include <cpu/riscv_csr.h>
#include <cpu/riscv_hart.h>
#include <cpu/riscv_mmu.h>
#include <cpu/riscv_pmu.h>

// Valid vm->pending_events bits deliverable to the hart
#define HART_EVENT_PAUSE   0x1 // Pause the hart in a consistent state
#define HART_EVENT_PREEMPT 0x2 // Preempt the hart for vm->preempt_ms

rvvm_hart_t* riscv_hart_init(rvvm_machine_t* machine)
{
    rvvm_hart_t* vm = vma_alloc(NULL, sizeof(rvvm_hart_t), VMA_RDWR);
    vm->wfi_cond    = condvar_create();
    vm->machine     = machine;
    vm->mem         = machine->mem;
    vm->rv64        = machine->rv64;
    vm->priv_mode   = RISCV_PRIV_MACHINE;
    vm->tlb_stats.enabled = rvvm_has_arg("tlb_stats");

    riscv_csr_init(vm);
    riscv_pmu_init(vm);

    rvtimecmp_init(&vm->mtimecmp, &vm->machine->timer);
    rvtimecmp_init(&vm->stimecmp, &vm->machine->timer);

    riscv_tlb_flush(vm);
    return vm;
}

void riscv_hart_prepare(rvvm_hart_t* vm)
{
#ifdef USE_JIT
    if (!vm->jit_enabled && rvvm_get_opt(vm->machine, RVVM_OPT_JIT)) {
        vm->jit_enabled = rvjit_ctx_init(&vm->jit, rvvm_get_opt(vm->machine, RVVM_OPT_JIT_CACHE));

        if (vm->jit_enabled) {
            rvjit_set_rv64(&vm->jit, vm->rv64);
            if (!rvvm_get_opt(vm->machine, RVVM_OPT_JIT_HARVARD)) {
                rvjit_init_memtracking(&vm->jit, vm->mem.size);
            }
        } else {
            rvvm_set_opt(vm->machine, RVVM_OPT_JIT, false);
            rvvm_warn("RVJIT failed to initialize, falling back to interpreter");
        }
    }
#else
    UNUSED(vm);
#endif
}

void riscv_hart_aia_init(rvvm_hart_t* vm)
{
    if (!vm->aia) {
        vm->aia = safe_new_arr(rvvm_aia_regfile_t, 2);
    }
}

void riscv_hart_free(rvvm_hart_t* vm)
{
    if (vm->tlb_stats.enabled) {
        rvvm_info("TLB hart=%u miss-r=%" PRIu64 " miss-w=%" PRIu64 " miss-x=%" PRIu64 " ram-r=%" PRIu64 " ram-w=%" PRIu64 " ram-x=%" PRIu64 " "
                  "mmio=%" PRIu64 " unmapped=%" PRIu64 " faults=%" PRIu64 " slow-hit=%" PRIu64 " probes=%" PRIu64 " walks=%" PRIu64 " flush-all=%" PRIu64 " flush-page=%" PRIu64 "",
                  vm->csr.hartid,
                  (uint64_t)vm->tlb_stats.misses[0], (uint64_t)vm->tlb_stats.misses[1],
                  (uint64_t)vm->tlb_stats.misses[2], (uint64_t)vm->tlb_stats.ram[0],
                  (uint64_t)vm->tlb_stats.ram[1], (uint64_t)vm->tlb_stats.ram[2],
                  (uint64_t)vm->tlb_stats.mmio, (uint64_t)vm->tlb_stats.unmapped,
                  (uint64_t)vm->tlb_stats.faults,
                  (uint64_t)vm->tlb_stats.slow_hits, (uint64_t)vm->tlb_stats.probes,
                  (uint64_t)vm->tlb_stats.walks, (uint64_t)vm->tlb_stats.flush_all,
                  (uint64_t)vm->tlb_stats.flush_page);
    }
#ifdef USE_JIT
    if (vm->jit_enabled) {
        rvjit_ctx_free(&vm->jit);
    }
#endif
    condvar_free(vm->wfi_cond);
    free(vm->aia);
    vma_free(vm, sizeof(rvvm_hart_t));
}

rvvm_addr_t riscv_hart_run_userland(rvvm_hart_t* vm)
{
    vm->userland = true;
    atomic_store_uint32_ex(&vm->running, true, ATOMIC_RELAXED);
    riscv_run_till_event(vm);
    if (vm->trap) {
        vm->registers[RISCV_REG_PC] = vm->trap_pc;
        vm->trap                    = false;
    }
    return vm->csr.cause[RISCV_PRIV_USER];
}

void riscv_switch_priv(rvvm_hart_t* vm, uint8_t priv_mode)
{
    if (vm->priv_mode != priv_mode) {
        riscv_pmu_update(vm);
        vm->priv_mode = priv_mode;
        riscv_update_xlen(vm);
        riscv_tlb_flush(vm);
    }
}

void riscv_update_xlen(rvvm_hart_t* vm)
{
#if defined(USE_RV64) && defined(USE_RV32)
    bool rv64 = false;
    switch (vm->priv_mode) {
        case RISCV_PRIV_MACHINE:
            rv64 = !!(vm->csr.isa & CSR_MISA_RV64);
            break;
        case RISCV_PRIV_HYPERVISOR: // TODO
        case RISCV_PRIV_SUPERVISOR:
            rv64 = bit_check(vm->csr.status, 35);
            break;
        case RISCV_PRIV_USER:
            rv64 = bit_check(vm->csr.status, 33);
            break;
    }
    if (vm->rv64 != rv64) {
        vm->rv64 = rv64;
        riscv_restart_dispatch(vm);
    }
#else
    UNUSED(vm);
#endif
}

// Save current priv to xPP, xIE to xPIE, disable interrupts for target priv
static void riscv_trap_priv_helper(rvvm_hart_t* vm, uint8_t target_priv)
{
    switch (target_priv) {
        case RISCV_PRIV_MACHINE:
            vm->csr.status = bit_replace(vm->csr.status, 11, 2, vm->priv_mode);
            vm->csr.status = bit_replace(vm->csr.status, 7, 1, bit_cut(vm->csr.status, 3, 1));
            vm->csr.status = bit_replace(vm->csr.status, 3, 1, 0);
            break;
        case RISCV_PRIV_HYPERVISOR: // TODO
        case RISCV_PRIV_SUPERVISOR:
            vm->csr.status = bit_replace(vm->csr.status, 8, 1, vm->priv_mode);
            vm->csr.status = bit_replace(vm->csr.status, 5, 1, bit_cut(vm->csr.status, 1, 1));
            vm->csr.status = bit_replace(vm->csr.status, 1, 1, 0);
            break;
        case RISCV_PRIV_USER:
            vm->csr.status = bit_replace(vm->csr.status, 4, 1, bit_cut(vm->csr.status, 0, 1));
            vm->csr.status = bit_replace(vm->csr.status, 0, 1, 0);
            break;
    }
}

static inline void riscv_restart_at_pc(rvvm_hart_t* vm, rvvm_uxlen_t pc)
{
    vm->trap_pc = pc;
    vm->trap    = true;
    riscv_restart_dispatch(vm);
}

void riscv_trap(rvvm_hart_t* vm, bitcnt_t cause, rvvm_uxlen_t tval)
{
    if (unlikely(vm->trap)) {
        // Only one trap may be raised per CPU dispatch cycle
        return;
    }
    if (cause >= RISCV_TRAP_INSN_PAGEFAULT && cause <= RISCV_TRAP_STORE_PAGEFAULT) {
        // Discard compiled JIT block torn by (likely recoverable) pagefault
        riscv_jit_discard(vm);
    } else {
        // End the compiled JIT block right before the trapping instruction
        riscv_jit_compile(vm);
    }
    if (vm->userland) {
        // Defer userland trap
        vm->csr.cause[RISCV_PRIV_USER] = cause;
        vm->csr.tval[RISCV_PRIV_USER]  = tval;
        vm->trap_pc                    = vm->registers[RISCV_REG_PC];
        riscv_restart_at_pc(vm, vm->registers[RISCV_REG_PC]);
    } else {
        // Target privilege mode
        uint8_t priv = RISCV_PRIV_MACHINE;
        // Delegate to lower privilege mode if needed
        while ((priv > vm->priv_mode) && (vm->csr.edeleg[priv] & (1 << cause))) {
            priv--;
        }
        // Write exception info
        vm->csr.epc[priv]   = vm->registers[RISCV_REG_PC];
        vm->csr.cause[priv] = cause;
        vm->csr.tval[priv]  = tval;
        // Modify exception stack in csr.status
        riscv_trap_priv_helper(vm, priv);
        // Jump to trap vector, switch to target priv
        riscv_switch_priv(vm, priv);
        riscv_restart_at_pc(vm, vm->csr.tvec[priv] & (~3ULL));
    }
}

void riscv_breakpoint(rvvm_hart_t* vm)
{
    if (vm->machine->gdbstub) {
        // Report software breakpoints to GDB stub
        if (gdbstub_halt(vm->machine->gdbstub)) {
            // GDB ate the breakpoint
            riscv_restart_at_pc(vm, vm->registers[RISCV_REG_PC]);
            return;
        }
    }
    riscv_trap(vm, RISCV_TRAP_BREAKPOINT, 0);
}

void riscv_restart_dispatch(rvvm_hart_t* vm)
{
    atomic_store_uint32_ex(&vm->running, false, ATOMIC_RELAXED);
}

static void riscv_hart_notify(rvvm_hart_t* vm)
{
    riscv_restart_dispatch(vm);
    // Wake from WFI sleep
    condvar_wake(vm->wfi_cond);
}

// Set IRQ bit, return true if it wasn't already set
static inline bool riscv_interrupt_set(rvvm_hart_t* vm, bitcnt_t irq)
{
    uint32_t mask = (1U << (irq & 0x1F));
    return !!(~atomic_or_uint32_ex(&vm->pending_irqs, mask, ATOMIC_RELAXED) & mask);
}

void riscv_interrupt(rvvm_hart_t* vm, bitcnt_t irq)
{
    if (riscv_interrupt_set(vm, irq)) {
        riscv_hart_notify(vm);
    }
}

void riscv_interrupt_clear(rvvm_hart_t* vm, bitcnt_t irq)
{
    // Discard pending irq
    atomic_and_uint32_ex(&vm->pending_irqs, ~(1U << irq), ATOMIC_RELAXED);
}

void riscv_send_aia_irq(rvvm_hart_t* vm, bool smode, uint32_t irq)
{
    if (likely(vm->aia && irq && irq < RVVM_AIA_IRQ_LIMIT)) {
        rvvm_aia_regfile_t* aia = &vm->aia[smode];
        if (likely(atomic_load_uint32_relax(&aia->eidelivery))) {
            uint32_t thresh = atomic_load_uint32_relax(&aia->eithreshold) - 1;
            uint32_t msip   = smode ? RISCV_INTERRUPT_SEXTERNAL : RISCV_INTERRUPT_MEXTERNAL;
            uint32_t reg    = irq >> 5;
            uint32_t val    = (1U << (irq & 0x1F));
            uint32_t eie    = atomic_load_uint32_relax(&aia->eie[reg]);
            uint32_t prev   = atomic_or_uint32(&aia->eip[reg], val);
            if (likely((irq < thresh) && (val & eie) && !(val & prev))) {
                // Newly arrived & enabled AIA IRQ below threshold
                riscv_interrupt(vm, msip);
            }
        }
    }
}

static uint32_t riscv_update_aia_internal(rvvm_hart_t* vm, bool smode, bool update, bool claim)
{
    uint32_t ret = 0;
    if (likely(vm->aia)) {
        rvvm_aia_regfile_t* aia    = &vm->aia[smode];
        uint32_t            thresh = atomic_load_uint32_relax(&aia->eithreshold) - 1;
        uint32_t            msip   = smode ? RISCV_INTERRUPT_SEXTERNAL : RISCV_INTERRUPT_MEXTERNAL;
        if (update) {
            riscv_interrupt_clear(vm, msip);
        }
        if (likely(atomic_load_uint32_relax(&aia->eidelivery))) {
            for (size_t i = 0; i < RVVM_AIA_ARR_LEN; ++i) {
                uint32_t eip  = atomic_load_uint32_relax(&aia->eip[i]);
                uint32_t eie  = atomic_load_uint32_relax(&aia->eie[i]);
                uint32_t bits = eip & eie;
                if (bits) {
                    if (ret) {
                        // Additional AIA interrupts are pending
                        riscv_interrupt(vm, msip);
                        return ret;
                    } else {
                        uint32_t bit  = bit_clz32(bits) ^ 31;
                        uint32_t irq  = (i << 5) | bit;
                        uint32_t mask = ~(1U << bit);
                        if (likely(irq < thresh)) {
                            // Enabled AIA interrupt below threshold
                            ret = irq;
                            if (claim) {
                                // Claim the interrupt
                                bits &= mask;
                                atomic_and_uint32(&aia->eip[i], mask);
                            }
                            if (update) {
                                if (bits) {
                                    // Additional AIA interrupts are pending
                                    riscv_interrupt(vm, msip);
                                    return ret;
                                }
                            } else {
                                // No need to scan more
                                return ret;
                            }
                        }
                    }
                }
            }
        }
    }
    return ret;
}

void riscv_update_aia_state(rvvm_hart_t* vm, bool smode)
{
    riscv_update_aia_internal(vm, smode, true, false);
}

uint32_t riscv_get_aia_irq(rvvm_hart_t* vm, bool smode, bool claim)
{
    return riscv_update_aia_internal(vm, smode, true, claim);
}

void riscv_hart_check_interrupts(rvvm_hart_t* vm)
{
    if (riscv_interrupts_pending(vm)) {
        riscv_restart_dispatch(vm);
    }
}

void riscv_hart_check_timer(rvvm_hart_t* vm)
{
    // Raise IRQ and kick hart if it's not sleeping in WFI
    // WFI precisely checks timers by itself
    if (!condvar_waiters(vm->wfi_cond)) {
        uint64_t timer = rvtimer_get(&vm->machine->timer);
        if (timer >= rvtimecmp_get(&vm->mtimecmp) && riscv_interrupt_set(vm, RISCV_INTERRUPT_MTIMER)) {
            riscv_restart_dispatch(vm);
        }
        if (timer >= rvtimecmp_get(&vm->stimecmp) && riscv_interrupt_set(vm, RISCV_INTERRUPT_STIMER)) {
            riscv_restart_dispatch(vm);
        }
    }
}

void riscv_hart_preempt(rvvm_hart_t* vm, uint32_t preempt_ms)
{
    if (preempt_ms) {
        atomic_store_uint32(&vm->preempt_ms, preempt_ms);
        atomic_or_uint32(&vm->pending_events, HART_EVENT_PREEMPT);
        riscv_restart_dispatch(vm);
    }
}

static inline rvvm_uxlen_t riscv_cause_irq_mask(rvvm_hart_t* vm)
{
    if (vm->rv64) {
        return (rvvm_uxlen_t)0x8000000000000000ULL;
    } else {
        return 0x80000000U;
    }
}

static void riscv_handle_irqs(rvvm_hart_t* vm)
{
    // IRQs that are pending & enabled by mie
    uint32_t pending_irqs = riscv_interrupts_pending(vm);
    if (unlikely(pending_irqs)) {
        // Target privilege mode
        uint8_t  priv = RISCV_PRIV_MACHINE;
        uint32_t irqs = 0;

        // Delegate pending IRQs from M to the highest possible mode
        do {
            irqs          = pending_irqs & ~vm->csr.ideleg[priv];
            pending_irqs &= vm->csr.ideleg[priv];
            if (irqs) {
                break;
            }
        } while (--priv);

        if (vm->priv_mode > priv) {
            // Target privilege mode is less privileged than current
            return;
        }

        if (vm->priv_mode == priv && !((1 << vm->priv_mode) & vm->csr.status)) {
            // Target privilege mode is equal, but IRQs are disabled in status CSR
            return;
        }

        // Discard compiled JIT block torn by interrupt
        riscv_jit_discard(vm);

        uint32_t priority = irqs & ~(1U << RISCV_INTERRUPT_COUNTER_OVF);
        uint32_t irq = bit_clz32(priority ? priority : irqs) ^ 31;
        // Modify exception stack in csr.status
        riscv_trap_priv_helper(vm, priv);
        // Switch privilege
        riscv_switch_priv(vm, priv);
        // Write exception info
        vm->csr.epc[priv]   = vm->registers[RISCV_REG_PC];
        vm->csr.cause[priv] = irq | riscv_cause_irq_mask(vm);
        vm->csr.tval[priv]  = 0;
        // Jump to trap vector
        if (vm->csr.tvec[priv] & 1) {
            vm->registers[RISCV_REG_PC] = (vm->csr.tvec[priv] & (~3ULL)) + (irq << 2);
        } else {
            vm->registers[RISCV_REG_PC] = vm->csr.tvec[priv] & (~3ULL);
        }
    }
    return;
}

void riscv_hart_run(rvvm_hart_t* vm)
{
#if defined(USE_THREAD_EMU)
    rvtimer_t timer = ZERO_INIT;
    rvtimer_init(&timer, 1000000000ULL);
#endif

    riscv_csr_sync_fpu(vm);
    do {
        // Allow hart to run
        atomic_store_uint32_ex(&vm->running, true, ATOMIC_RELAXED);

        riscv_pmu_update(vm);

        // Handle events
        uint32_t events = atomic_swap_uint32(&vm->pending_events, 0);
        if (unlikely(events)) {
            if (events & HART_EVENT_PAUSE) {
                break;
            }
            if (events & HART_EVENT_PREEMPT) {
                sleep_ms(atomic_swap_uint32(&vm->preempt_ms, 0));
            }
        }

        riscv_handle_irqs(vm);

        // Run the hart
        riscv_run_till_event(vm);
        if (vm->trap) {
            vm->registers[RISCV_REG_PC] = vm->trap_pc;
            vm->trap                    = false;
        }
#if defined(USE_THREAD_EMU)
    } while (rvtimer_get(&timer) < 16666666ULL);
#else
    } while (true);
#endif
    riscv_csr_sync_fpu(vm);
}

static void* riscv_hart_run_thread(void* ptr)
{
    rvvm_hart_t* vm = ptr;
    if (!rvvm_has_arg("noisolation")) {
        rvvm_restrict_this_thread();
    }
    riscv_hart_run(vm);
    return NULL;
}

void riscv_hart_spawn(rvvm_hart_t* vm)
{
    if (!vm->thread) {
        atomic_store_uint32(&vm->pending_events, 0);
        vm->thread = thread_create(riscv_hart_run_thread, vm);
    }
}

void riscv_hart_queue_pause(rvvm_hart_t* vm)
{
    atomic_or_uint32(&vm->pending_events, HART_EVENT_PAUSE);
    riscv_hart_notify(vm);
}

void riscv_hart_pause(rvvm_hart_t* vm)
{
    riscv_hart_queue_pause(vm);
    thread_join(vm->thread);
    vm->thread = NULL;
}
