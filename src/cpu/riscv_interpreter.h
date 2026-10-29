/*
riscv_interpreter.h - RISC-V Template Interpreter
Copyright (C) 2024  LekKit <github.com/LekKit>

This Source Code Form is subject to the terms of the Mozilla Public
License, v. 2.0. If a copy of the MPL was not distributed with this
file, You can obtain one at https://mozilla.org/MPL/2.0/.
*/

#ifndef RVVM_RISCV_INTERPRETER_H
#define RVVM_RISCV_INTERPRETER_H

// Provides entry point to riscv_emulate_insn()
#include "riscv_compressed.h"
#include "riscv_pmu.h"

#if defined(RISCV64)
#define riscv_run_interpreter func_opt_hot riscv64_run_interpreter
#elif defined(RISCV32)
#define riscv_run_interpreter riscv32_run_interpreter
#endif

/*
 * Optimized CPU interpreter dispatch loop.
 * Executes instructions until some event occurs (interrupt, trap).
 *
 * Calling this with vm->running == 0 allows single-stepping guest instructions.
 *
 * NOTE: Call riscv_restart_dispatch() after flushing TLB around PC,
 * otherwise the CPU loop will continue executing current page.
 */

TSAN_SUPPRESS void riscv_run_interpreter(rvvm_hart_t* vm)
{
    uint32_t insn = 0;

    // This is similar to TLB mechanism, but persists in local variables across instructions
    size_t insn_ptr  = 0;
    xlen_t insn_page = vm->registers[RISCV_REG_PC] + RISCV_PAGE_SIZE;

    do {
        const xlen_t insn_addr = vm->registers[RISCV_REG_PC];
        if (likely(insn_addr - insn_page <= 0xFFC)) {
            // Direct instruction fetch by pointer
            insn = read_uint32_le_m((const void*)(size_t)(insn_ptr + insn_addr));
        } else {
            uint32_t tmp = 0;
            if (likely(riscv_fetch_insn(vm, insn_addr, &tmp))) {
                // Update pointer to the current page in real memory
                // If we are executing code from MMIO, direct memory fetch fails
                const xlen_t            vpn   = vm->registers[RISCV_REG_PC] >> RISCV_PAGE_SHIFT;
                const rvvm_tlb_entry_t* entry = &vm->tlb[vpn & RVVM_TLB_MASK];
                insn                          = tmp;
                insn_ptr                      = entry->ptr;
                insn_page                     = entry->e << RISCV_PAGE_SHIFT;
            } else {
                // Instruction fetch fault happened
                return;
            }
        }

#if defined(USE_JIT) && (defined(RVJIT_NATIVE_64BIT) || !defined(RISCV64))
        if (likely(vm->jit_compiling)) {
            // If we hit non-compilable instruction or cross page boundaries, current JIT block is finalized.
            if (vm->jit_block_ends
                || (vm->jit.virt_pc >> RISCV_PAGE_SHIFT) != (vm->registers[RISCV_REG_PC] >> RISCV_PAGE_SHIFT)) {
                riscv_jit_finalize(vm);
            }
            vm->jit_block_ends = true;
        }
#endif

        // Implicitly zero x0 register each time
        vm->registers[RISCV_REG_ZERO] = 0;

        uint8_t priv = vm->priv_mode;
        uint32_t inhibit = vm->pmu.inhibit;
        vm->pmu.jit_executed = false;
        vm->pmu.wrote_instret = false;
        riscv_emulate_insn(vm, insn);
        if (!vm->trap && !vm->pmu.jit_executed) {
            riscv_pmu_retire(vm, priv, inhibit, insn);
        }
    } while (likely(vm->running));
}

#endif
