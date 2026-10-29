/*
riscv_csr.c - RISC-V Control and Status Registers
Copyright (C) 2021  LekKit <github.com/LekKit>
                    cerg2010cerg2010 <github.com/cerg2010cerg2010>

This Source Code Form is subject to the terms of the Mozilla Public
License, v. 2.0. If a copy of the MPL was not distributed with this
file, You can obtain one at https://mozilla.org/MPL/2.0/.
*/

#include "riscv_csr.h"
#include "riscv_hart.h"
#include "riscv_mmu.h"
#include "riscv_pmu.h"

PUSH_OPTIMIZATION_SIZE

#if defined(USE_FPU)
#include "fpu_lib.h"
#endif

// Get RVVM commit in a mimpid hex form
static uint32_t rvvm_mimpid(void)
{
    const char* ver  = RVVM_VERSION;
    const char* dash = rvvm_strfind(ver, "-");
    if (dash) {
        uint32_t commit_hex = str_to_uint_base(dash + 1, NULL, 16) << 4;
        if (rvvm_strfind(ver, "dirty")) {
            commit_hex |= 0xD;
        }
        return commit_hex;
    }
    return 0;
}

// Make a misa CSR value from ISA string
static uint64_t riscv_mkmisa(const char* str)
{
    uint64_t ret = 0;
    if (rvvm_strfind(str, "rv64")) {
        ret |= CSR_MISA_RV64;
        str += 4;
    } else if (rvvm_strfind(str, "rv32")) {
        ret |= CSR_MISA_RV32;
        str += 4;
    }
    while (*str && *str != '_') {
        if (*str >= 'a' && *str <= 'z') {
            ret |= (1 << (*str - 'a'));
        }
        str++;
    }
    return ret;
}

static inline bool riscv_csr_helper_masked(rvvm_hart_t* vm, rvvm_uxlen_t* csr, //
                                           rvvm_uxlen_t* dest, rvvm_uxlen_t mask, uint8_t op)
{
    rvvm_uxlen_t tmp = *csr;
    if (!vm->rv64) {
        mask &= (uint32_t)-1;
    }
    switch (op) {
        case CSR_SWAP:
            *csr &= (~mask);
            *csr |= (*dest & mask);
            break;
        case CSR_SETBITS:
            *csr |= (*dest & mask);
            break;
        case CSR_CLEARBITS:
            *csr &= (~(*dest & mask));
            break;
    }
    *dest = tmp & mask;
    return true;
}

static inline bool riscv_csr_helper(rvvm_hart_t* vm, rvvm_uxlen_t* csr, rvvm_uxlen_t* dest, uint8_t op)
{
    if (vm->rv64) {
        rvvm_uxlen_t tmp = *csr;
        switch (op) {
            case CSR_SWAP:
                *csr = *dest;
                break;
            case CSR_SETBITS:
                *csr |= *dest;
                break;
            case CSR_CLEARBITS:
                *csr &= ~(*dest);
                break;
        }
        *dest = tmp;
        return true;
    } else {
        return riscv_csr_helper_masked(vm, csr, dest, -1, op);
    }
}

static inline bool riscv_csr_tvec(rvvm_hart_t* vm, rvvm_uxlen_t* csr, rvvm_uxlen_t* dest, uint8_t op)
{
    // Apply normal CSR op first (preserves read-old-value behavior)
    riscv_csr_helper(vm, csr, dest, op);

    // WARL legalization for tvec.MODE:
    // legal values are 0 (Direct) and 1 (Vectored).
    // Any write producing mode >= 2 is mapped to a legal value.
    *csr = bit_replace(*csr, 0, 2, bit_cut(*csr, 0, 2) & 0x1);

    return true;
}

static inline bool riscv_csr_epc(rvvm_hart_t* vm, rvvm_uxlen_t* csr, rvvm_uxlen_t* dest, uint8_t op)
{
    riscv_csr_helper(vm, csr, dest, op);

    // epc[0] is always zero
    *csr &= ~((rvvm_uxlen_t)1);

    return true;
}

static inline bool riscv_csr_helper_l(rvvm_hart_t* vm, uint64_t* csr, rvvm_uxlen_t* dest, uint64_t mask, uint8_t op)
{
    rvvm_uxlen_t tmp = *csr;
    riscv_csr_helper_masked(vm, &tmp, dest, mask, op);
    if (vm->rv64) {
        *csr = tmp;
    } else {
        *csr = bit_replace(*csr, 0, 32, tmp);
    }
    return true;
}

static inline bool riscv_csr_helper_h(rvvm_hart_t* vm, uint64_t* csr, rvvm_uxlen_t* dest, uint64_t mask, uint8_t op)
{
    if (!vm->rv64) {
        rvvm_uxlen_t tmp = (*csr >> 32);
        riscv_csr_helper_masked(vm, &tmp, dest, mask >> 32, op);
        *csr = bit_replace(*csr, 32, 32, tmp);
        return true;
    }
    return false;
}

static inline bool riscv_csr_const(rvvm_uxlen_t* dest, rvvm_uxlen_t val)
{
    *dest = val;
    return true;
}

static inline bool riscv_csr_zero(rvvm_uxlen_t* dest)
{
    return riscv_csr_const(dest, 0);
}

static inline bool riscv_csr_zero_h(rvvm_hart_t* vm, rvvm_uxlen_t* dest)
{
    if (!vm->rv64) {
        return riscv_csr_const(dest, 0);
    }
    return false;
}

static inline bool riscv_csr_seed(rvvm_hart_t* vm, rvvm_uxlen_t* dest, uint8_t op)
{
    // Zkr: SEED must be accessed via CSR read-write instruction form
    if (op != CSR_SWAP) {
        return false;
    }

    if (riscv_csr_seed_enabled(vm)) {
        uint16_t seed = 0;
        rvvm_randombytes(&seed, sizeof(seed));
        return riscv_csr_const(dest, seed | CSR_SEED_OPST_ES16);
    }
    return false;
}

static bool riscv_csr_misa(rvvm_hart_t* vm, rvvm_uxlen_t* dest, uint8_t op)
{
    rvvm_uxlen_t misa = vm->csr.isa;
#ifdef USE_FPU
    misa |= riscv_mkmisa("imafdcbsu");
#else
    misa |= riscv_mkmisa("imacbsu");
#endif
    riscv_csr_helper(vm, &misa, dest, op);

    if ((vm->csr.isa & CSR_MISA_RV64) && (misa & CSR_MISA_RV32)) {
        vm->csr.isa = CSR_MISA_RV32;
        riscv_update_xlen(vm);
    } else if ((vm->csr.isa & CSR_MISA_RV32) && (misa & (CSR_MISA_RV64 >> 32))) {
        // Switch to RV64 if machine allows
        if (vm->machine->rv64) {
            vm->csr.isa = (rvvm_uxlen_t)CSR_MISA_RV64;
            riscv_update_xlen(vm);
        }
    }
    return true;
}

static inline rvvm_uxlen_t riscv_csr_sd_bit(rvvm_hart_t* vm)
{
    if (vm->rv64) {
        return (rvvm_uxlen_t)0x8000000000000000ULL;
    } else {
        return 0x80000000U;
    }
}

#define CSR_STATUS_XL_RV32 0x1
#define CSR_STATUS_XL_RV64 0x2

static inline bool riscv_csr_status_xl_valid(uint8_t xl)
{
#ifdef USE_RV32
    if (xl == CSR_STATUS_XL_RV32) {
        return true;
    }
#endif
    if (xl == CSR_STATUS_XL_RV64) {
        return true;
    }
    return false;
}

static bool riscv_csr_status(rvvm_hart_t* vm, rvvm_uxlen_t* dest, uint64_t mask, uint8_t op)
{
    rvvm_uxlen_t status     = vm->csr.status;
    rvvm_uxlen_t old_status = status;

    // Fixup XS and SD fields in status before reading
    uint8_t fs = bit_cut(status, 13, 2);
    uint8_t vs = bit_cut(status, 9, 2);
    uint8_t xs = EVAL_MAX(fs, vs);
    status     = bit_replace(status, 15, 2, xs);

    riscv_csr_helper_masked(vm, &status, dest, mask, op);

    if (unlikely(xs == FS_DIRTY)) {
        // XS is dirty, set SD bit
        *dest |= riscv_csr_sd_bit(vm);
    }

    if (unlikely(status != old_status)) {
        // Validate WARL fields
        if (vm->machine->rv64) {
            // Validate UXL, SXL if machine is 64-bit
            if (!riscv_csr_status_xl_valid(bit_cut(status, 32, 2))) {
                status = bit_replace(status, 32, 2, CSR_STATUS_XL_RV64);
            }
            if (!riscv_csr_status_xl_valid(bit_cut(status, 34, 2))) {
                status = bit_replace(status, 34, 2, CSR_STATUS_XL_RV64);
            }
        }
        if (bit_cut(status, 11, 2) == RISCV_PRIV_HYPERVISOR) {
            // Validate MPP
            status = bit_replace(status, 11, 2, RISCV_PRIV_USER);
        }

#ifndef USE_FPU
        status = bit_replace(status, 13, 2, FS_OFF);
#endif

#ifndef USE_RVV
        // TODO: Vector extension state handling
        status = bit_replace(status, 9, 2, FS_OFF);
#endif

        if ((status & 0xA) & ~(old_status & 0xA)) {
            // MIE/SIE were enabled, check interrupts
            riscv_hart_check_interrupts(vm);
        }

        vm->csr.status = status;
    }
    return true;
}

static inline bool riscv_csr_ie(rvvm_hart_t* vm, rvvm_uxlen_t* dest, rvvm_uxlen_t mask, uint8_t op)
{
    riscv_csr_helper_masked(vm, &vm->csr.ie, dest, mask, op);
    riscv_hart_check_interrupts(vm);
    return true;
}

static inline bool riscv_csr_ip(rvvm_hart_t* vm, rvvm_uxlen_t* dest, rvvm_uxlen_t mask, uint8_t op)
{
    riscv_csr_helper_masked(vm, &vm->csr.ip, dest, mask, op);
    *dest |= (riscv_interrupts_raised(vm) & mask);
    riscv_hart_check_interrupts(vm);
    return true;
}

static inline bool riscv_csr_topi(rvvm_hart_t* vm, rvvm_uxlen_t* dest, rvvm_uxlen_t mask, uint8_t op)
{
    if (vm->aia) {
        uint32_t pending = (riscv_interrupts_pending(vm) & mask);
        uint32_t priority = pending & ~(1U << RISCV_INTERRUPT_COUNTER_OVF);
        uint32_t irq = pending ? (bit_clz32(priority ? priority : pending) ^ 31) : 0;
        // Report IRQ as both pending and highest prio
        *dest = (!!irq) | (irq << 16);
        UNUSED(op);
        return true;
    }
    return false;
}

static inline bool riscv_csr_topei(rvvm_hart_t* vm, uint8_t priv_mode, rvvm_uxlen_t* dest, bool is_write)
{
    if (vm->aia) {
        bool     smode = priv_mode == RISCV_PRIV_SUPERVISOR;
        uint32_t irq   = riscv_get_aia_irq(vm, smode, is_write);
        // Report IRQ as both pending and highest prio
        *dest = irq | (irq << 16);
        return true;
    }
    return false;
}

static void riscv_csr_stimecmp_set(rvvm_hart_t* vm, uint64_t stimecmp)
{
    rvtimecmp_set(&vm->stimecmp, stimecmp);
    if (rvtimecmp_pending(&vm->stimecmp)) {
        riscv_interrupt(vm, RISCV_INTERRUPT_STIMER);
    } else {
        riscv_interrupt_clear(vm, RISCV_INTERRUPT_STIMER);
    }
}

static inline bool riscv_csr_stimecmp(rvvm_hart_t* vm, rvvm_uxlen_t* dest, uint8_t op)
{
    if (likely(riscv_csr_sstc_enabled(vm))) {
        uint64_t stimecmp = rvtimecmp_get(&vm->stimecmp);
        riscv_csr_helper_l(vm, &stimecmp, dest, -1ULL, op);
        riscv_csr_stimecmp_set(vm, stimecmp);
        return true;
    }
    return false;
}

static inline bool riscv_csr_stimecmph(rvvm_hart_t* vm, rvvm_uxlen_t* dest, uint8_t op)
{
    if (!vm->rv64 && riscv_csr_sstc_enabled(vm)) {
        uint64_t stimecmp = rvtimecmp_get(&vm->stimecmp);
        riscv_csr_helper_h(vm, &stimecmp, dest, -1ULL, op);
        riscv_csr_stimecmp_set(vm, stimecmp);
        return true;
    }
    return false;
}

static bool riscv_csr_satp(rvvm_hart_t* vm, rvvm_uxlen_t* dest, uint8_t op)
{
    uint8_t new_mmu_mode = 0;
    if ((vm->csr.status & CSR_STATUS_TVM) && vm->priv_mode < RISCV_PRIV_MACHINE) {
        // Accessing satp CSR should trap in S-mode when mstatus.TVM is enabled
        return false;
    }
    if (vm->rv64) {
        rvvm_uxlen_t satp = (((uint64_t)vm->mmu_mode) << 60) | (vm->root_page_table >> RISCV_PAGE_SHIFT);
        riscv_csr_helper(vm, &satp, dest, op);
        new_mmu_mode = bit_cut(satp, 60, 4);
        if (new_mmu_mode != CSR_SATP_MODE_BARE
            && (new_mmu_mode < CSR_SATP_MODE_SV39 || new_mmu_mode > CSR_SATP_MODE_SV57)) {
            // If satp is written with unsupported MODE, the entire write has no effect
            return true;
        }
        if (new_mmu_mode == CSR_SATP_MODE_SV57 && !rvvm_has_arg("sv57")) {
            // Disallow SV57 mode unless -sv57 is passed
            return true;
        }
        vm->root_page_table = (satp & bit_mask(44)) << RISCV_PAGE_SHIFT;
    } else {
        rvvm_uxlen_t satp = (((rvvm_uxlen_t)vm->mmu_mode) << 31) | (vm->root_page_table >> RISCV_PAGE_SHIFT);
        riscv_csr_helper(vm, &satp, dest, op);
        new_mmu_mode        = bit_cut(satp, 31, 1);
        vm->root_page_table = (satp & bit_mask(22)) << RISCV_PAGE_SHIFT;
    }

    if (!!vm->mmu_mode != !!new_mmu_mode) {
        // Switchin satp.MODE to Bare and vice versa should take effect immediately,
        // so we need to perform an implicit software TLB flush
        riscv_tlb_flush(vm);
        vm->mmu_mode = new_mmu_mode;
    }
    return true;
}

#ifdef USE_FPU

static inline void riscv_update_fflags(rvvm_hart_t* vm)
{
    vm->csr.fcsr |= fpu_get_exceptions();
}

static void riscv_update_fcsr(rvvm_hart_t* vm, uint32_t old_fcsr, uint32_t new_fcsr)
{
    if (unlikely(new_fcsr != old_fcsr)) {
        uint32_t old_frm    = bit_cut(old_fcsr, 5, 3);
        uint32_t new_frm    = bit_cut(new_fcsr, 5, 3);
        uint32_t old_fflags = bit_cut(old_fcsr, 0, 5);
        uint32_t new_fflags = bit_cut(new_fcsr, 0, 5);
        if (unlikely(new_frm != old_frm)) {
            if (new_frm > RM_RMM) {
                // Ignore invalid frm, apply other fcsr fields
                new_frm = old_frm;
            } else {
                // Set host rounding mode
                fpu_set_rounding_mode(new_frm);
            }
        }
        if (unlikely(~new_fflags & old_fflags)) {
            if (~new_fflags & fpu_get_exceptions()) {
                // Clear host-set FPU exceptions, anything needed is left in fcsr
                fpu_set_exceptions(0);
            }
        }
        vm->csr.fcsr = new_fflags | (new_frm << 5);
    }
}

static void riscv_set_fcsr(rvvm_hart_t* vm, uint32_t fcsr)
{
    riscv_update_fcsr(vm, vm->csr.fcsr, fcsr);
}

static bool riscv_csr_fflags(rvvm_hart_t* vm, rvvm_uxlen_t* dest, uint8_t op)
{
    if (likely(riscv_fpu_is_enabled(vm))) {
        riscv_update_fflags(vm);
        rvvm_uxlen_t fcsr = vm->csr.fcsr;
        riscv_csr_helper_masked(vm, &fcsr, dest, CSR_FFLAGS_MASK, op);
        riscv_set_fcsr(vm, fcsr);
        return true;
    }
    return false;
}

static bool riscv_csr_frm(rvvm_hart_t* vm, rvvm_uxlen_t* dest, uint8_t op)
{
    if (likely(riscv_fpu_is_enabled(vm))) {
        rvvm_uxlen_t fcsr = vm->csr.fcsr;
        rvvm_uxlen_t frm  = fcsr >> 5;
        riscv_csr_helper_masked(vm, &frm, dest, CSR_FRM_MASK, op);
        fcsr = bit_replace(fcsr, 5, 3, frm);
        riscv_set_fcsr(vm, fcsr);
        return true;
    }
    return false;
}

static bool riscv_csr_fcsr(rvvm_hart_t* vm, rvvm_uxlen_t* dest, uint8_t op)
{
    if (likely(riscv_fpu_is_enabled(vm))) {
        riscv_update_fflags(vm);
        rvvm_uxlen_t fcsr = vm->csr.fcsr;
        riscv_csr_helper_masked(vm, &fcsr, dest, CSR_FCSR_MASK, op);
        riscv_set_fcsr(vm, fcsr);
        return true;
    }
    return false;
}

#endif

static inline bool riscv_csr_atomic_helper(uint32_t* val, rvvm_uxlen_t* dest, uint8_t op)
{
    switch (op) {
        case CSR_SWAP:
            *dest = atomic_swap_uint32(val, *dest);
            return true;
        case CSR_SETBITS:
            if (*dest) {
                *dest = atomic_or_uint32(val, *dest);
                return true;
            }
            *dest = atomic_load_uint32(val);
            return false;
        case CSR_CLEARBITS:
            if (*dest) {
                *dest = atomic_and_uint32(val, ~(*dest));
                return true;
            }
            *dest = atomic_load_uint32(val);
            return false;
    }
    return false;
}

static bool riscv_csr_aia_helper(rvvm_hart_t* vm, bool smode, uint32_t* val, rvvm_uxlen_t* dest, uint8_t op)
{
    if (riscv_csr_atomic_helper(val, dest, op)) {
        riscv_update_aia_state(vm, smode);
    }
    return true;
}

static bool riscv_csr_aia_pair_helper(rvvm_hart_t* vm, bool smode, uint32_t reg, uint32_t* arr, rvvm_uxlen_t* dest,
                                      uint8_t op)
{
    if (vm->rv64 && (reg & 1)) {
        // Only even eip/eie are accessible in 64 bit mode
        return false;
    }
    if (reg < RVVM_AIA_ARR_LEN) {
        rvvm_uxlen_t top = ((uint64_t)*dest) >> 32;
        riscv_csr_aia_helper(vm, smode, &arr[reg], dest, op);
        if (vm->rv64 && reg + 1 < RVVM_AIA_ARR_LEN) {
            riscv_csr_aia_helper(vm, smode, &arr[reg + 1], &top, op);
            *dest |= ((uint64_t)top) << 32;
        }
        return true;
    }
    return false;
}

static bool riscv_csr_indirect(rvvm_hart_t* vm, uint8_t priv_mode, uint32_t csri, rvvm_uxlen_t* dest, uint8_t op)
{
    bool smode = priv_mode == RISCV_PRIV_SUPERVISOR;
    switch (csri) {
        case CSRI_EIDELIVERY:
            if (vm->aia) {
                *dest &= 1;
                return riscv_csr_aia_helper(vm, smode, &vm->aia[smode].eidelivery, dest, op);
            }
            break;
        case CSRI_EITHRESHOLD:
            if (vm->aia) {
                return riscv_csr_aia_helper(vm, smode, &vm->aia[smode].eithreshold, dest, op);
            }
            break;
        default:
            if (vm->aia) {
                if (csri >= CSRI_MIPRIO_0 && csri <= CSRI_MIPRIO_15) {
                    // Ignore, all major priorities hardwired to zero
                    return riscv_csr_zero(dest);
                } else if (csri >= CSRI_EIP0 && csri <= CSRI_EIP63) {
                    uint32_t reg = csri - CSRI_EIP0;
                    return riscv_csr_aia_pair_helper(vm, smode, reg, vm->aia[smode].eip, dest, op);
                } else if (csri >= CSRI_EIE0 && csri <= CSRI_EIE63) {
                    uint32_t reg = csri - CSRI_EIE0;
                    return riscv_csr_aia_pair_helper(vm, smode, reg, vm->aia[smode].eie, dest, op);
                }
            }
            break;
    }

    return false;
}

static forceinline bool riscv_csr_op_internal(rvvm_hart_t* vm, uint32_t csr_id, rvvm_uxlen_t* dest, uint8_t op, bool is_write)
{
    switch (csr_id) {
#ifdef USE_FPU
        // User Floating-Point CSRs
        case CSR_FFLAGS:
            return riscv_csr_fflags(vm, dest, op);
        case CSR_FRM:
            return riscv_csr_frm(vm, dest, op);
        case CSR_FCSR:
            return riscv_csr_fcsr(vm, dest, op);
#endif

        // Unprivileged Entropy Source CSR
        case CSR_SEED:
            return riscv_csr_seed(vm, dest, op);

        case CSR_TIME:
        case CSR_TIMEH:
            if ((csr_id == CSR_TIMEH && vm->rv64) || !riscv_csr_counter_enabled(vm, 1))
                return false;
            return riscv_csr_const(dest, rvtimer_get(&vm->machine->timer) >> (csr_id == CSR_TIMEH ? 32 : 0));

        // Supervisor Trap Setup
        case CSR_SSTATUS:
            return riscv_csr_status(vm, dest, CSR_SSTATUS_MASK, op);
        case CSR_SIE:
            return riscv_csr_ie(vm, dest, CSR_SEIP_MASK, op);
        case CSR_SIEH:
            return riscv_csr_zero_h(vm, dest);
        case CSR_STVEC:
            return riscv_csr_tvec(vm, &vm->csr.tvec[RISCV_PRIV_SUPERVISOR], dest, op);
        case CSR_SCOUNTEREN:
            return riscv_csr_helper_masked(vm, &vm->csr.counteren[RISCV_PRIV_SUPERVISOR], dest, CSR_COUNTEREN_MASK, op);

        // Supervisor Configuration
        case CSR_SENVCFG:
            return riscv_csr_helper_l(vm, &vm->csr.envcfg[RISCV_PRIV_SUPERVISOR], dest, CSR_SENVCFG_MASK, op);

        // Supervisor Counter Setup
        case CSR_SCOUNTINHIBIT:
            return riscv_csr_zero(dest);

        // Supervisor Indirect CSR Access (Sscsrind)
        case CSR_SISELECT:
            return riscv_csr_helper(vm, &vm->csr.iselect[RISCV_PRIV_SUPERVISOR], dest, op);
        case CSR_SIREG:
            return riscv_csr_indirect(vm, RISCV_PRIV_SUPERVISOR, vm->csr.iselect[RISCV_PRIV_SUPERVISOR], dest, op);

        // Supervisor Trap Handling
        case CSR_SSCRATCH:
            return riscv_csr_helper(vm, &vm->csr.scratch[RISCV_PRIV_SUPERVISOR], dest, op);
        case CSR_SEPC:
            return riscv_csr_epc(vm, &vm->csr.epc[RISCV_PRIV_SUPERVISOR], dest, op);
        case CSR_SCAUSE:
            return riscv_csr_helper(vm, &vm->csr.cause[RISCV_PRIV_SUPERVISOR], dest, op);
        case CSR_STVAL:
            return riscv_csr_helper(vm, &vm->csr.tval[RISCV_PRIV_SUPERVISOR], dest, op);
        case CSR_SIP:
            return riscv_csr_ip(vm, dest, CSR_SEIP_MASK, op);
        case CSR_SIPH:
            return riscv_csr_zero_h(vm, dest);
        case CSR_STOPEI:
            return riscv_csr_topei(vm, RISCV_PRIV_SUPERVISOR, dest, is_write);
        case CSR_STOPI:
            return riscv_csr_topi(vm, dest, CSR_SEIP_MASK, op);
        case CSR_STIMECMP:
            return riscv_csr_stimecmp(vm, dest, op);
        case CSR_STIMECMPH:
            return riscv_csr_stimecmph(vm, dest, op);

        // Supervisor Protection and Translation
        case CSR_SATP:
            return riscv_csr_satp(vm, dest, op);

        // Machine Information Registers
        case CSR_MVENDORID:
            return riscv_csr_zero(dest); // Not a commercial implementation
        case CSR_MARCHID:
            return riscv_csr_const(dest, 0x5256564D); // 'RVVM' in hex
        case CSR_MIMPID:
            return riscv_csr_const(dest, rvvm_mimpid());
        case CSR_MHARTID:
            return riscv_csr_const(dest, vm->csr.hartid);

        // Machine Trap Setup
        case CSR_MSTATUS:
            return riscv_csr_status(vm, dest, CSR_MSTATUS_MASK, op);
        case CSR_MSTATUSH:
            return riscv_csr_zero(dest); // We don't need upper half on rv32 for now
        case CSR_MISA:
            return riscv_csr_misa(vm, dest, op);
        case CSR_MEDELEG:
            return riscv_csr_helper_masked(vm, &vm->csr.edeleg[RISCV_PRIV_MACHINE], dest, CSR_MEDELEG_MASK, op);
        case CSR_MIDELEG:
            return riscv_csr_helper_masked(vm, &vm->csr.ideleg[RISCV_PRIV_MACHINE], dest, CSR_MIDELEG_MASK, op);
        case CSR_MIDELEGH:
            return riscv_csr_zero_h(vm, dest);
        case CSR_MIE:
            return riscv_csr_ie(vm, dest, CSR_MEIP_MASK, op);
        case CSR_MIEH:
            return riscv_csr_zero_h(vm, dest);
        case CSR_MTVEC:
            return riscv_csr_tvec(vm, &vm->csr.tvec[RISCV_PRIV_MACHINE], dest, op);
        case CSR_MCOUNTEREN:
            return riscv_csr_helper_masked(vm, &vm->csr.counteren[RISCV_PRIV_MACHINE], dest, CSR_COUNTEREN_MASK, op);
        // TODO: mvien, mvip

        // Machine Trap Handling
        case CSR_MSCRATCH:
            return riscv_csr_helper(vm, &vm->csr.scratch[RISCV_PRIV_MACHINE], dest, op);
        case CSR_MEPC:
            return riscv_csr_epc(vm, &vm->csr.epc[RISCV_PRIV_MACHINE], dest, op);
        case CSR_MCAUSE:
            return riscv_csr_helper(vm, &vm->csr.cause[RISCV_PRIV_MACHINE], dest, op);
        case CSR_MTVAL:
            return riscv_csr_helper(vm, &vm->csr.tval[RISCV_PRIV_MACHINE], dest, op);
        case CSR_MIP:
            return riscv_csr_ip(vm, dest, CSR_MEIP_MASK, op);
        case CSR_MIPH:
            return riscv_csr_zero_h(vm, dest);
        case CSR_MTOPEI:
            return riscv_csr_topei(vm, RISCV_PRIV_MACHINE, dest, is_write);
        case CSR_MTOPI:
            return riscv_csr_topi(vm, dest, CSR_MEIP_MASK, op);

        // Machine Configuration
        case CSR_MENVCFG:
            return riscv_csr_helper_l(vm, &vm->csr.envcfg[RISCV_PRIV_MACHINE], dest, CSR_MENVCFG_MASK, op);
        case CSR_MENVCFGH:
            return riscv_csr_helper_h(vm, &vm->csr.envcfg[RISCV_PRIV_MACHINE], dest, CSR_MENVCFG_MASK, op);
        case CSR_MSECCFG:
            return riscv_csr_helper_l(vm, &vm->csr.mseccfg, dest, CSR_MSECCFG_MASK, op);
        case CSR_MSECCFGH:
            return riscv_csr_helper_h(vm, &vm->csr.mseccfg, dest, CSR_MSECCFG_MASK, op);

        // TODO: Machine Memory Protection
        case 0x3A0: // pmpcfg0
        case 0x3A1: // pmpcfg1
        case 0x3A2: // pmpcfg2
        case 0x3A3: // pmpcfg3
            return riscv_csr_zero(dest);
        case 0x3B0: // pmpaddr0
        case 0x3B1: // pmpaddr1
        case 0x3B2: // pmpaddr2
        case 0x3B3: // pmpaddr3
        case 0x3B4: // pmpaddr4
        case 0x3B5: // pmpaddr5
        case 0x3B6: // pmpaddr6
        case 0x3B7: // pmpaddr7
        case 0x3B8: // pmpaddr8
        case 0x3B9: // pmpaddr9
        case 0x3BA: // pmpaddr10
        case 0x3BB: // pmpaddr11
        case 0x3BC: // pmpaddr12
        case 0x3BD: // pmpaddr13
        case 0x3BE: // pmpaddr14
        case 0x3BF: // pmpaddr15
            return riscv_csr_zero(dest);

        // Machine Indirect CSR Access (Sscsrind)
        case CSR_MISELECT:
            return riscv_csr_helper(vm, &vm->csr.iselect[RISCV_PRIV_MACHINE], dest, op);
        case CSR_MIREG:
            return riscv_csr_indirect(vm, RISCV_PRIV_MACHINE, vm->csr.iselect[RISCV_PRIV_MACHINE], dest, op);
    }

    return false;
}

bool riscv_csr_op(rvvm_hart_t* vm, uint32_t csr_id, rvvm_uxlen_t* dest, uint8_t op, bool is_write)
{
    if (riscv_csr_readonly(csr_id)) {
        // CSRRS/CSRRC with rs1/zimm = x0/0 are reads, not writes
        if (unlikely(is_write)) {
            return false;
        }
    }

    if (unlikely(riscv_csr_privilege(csr_id) > vm->priv_mode)) {
        // Not privileged enough to access this CSR
        return false;
    }

    bool pmu = (csr_id >= 0xb00 && csr_id <= 0xb1f && csr_id != 0xb01)
            || (csr_id >= 0xb80 && csr_id <= 0xb9f && csr_id != 0xb81)
            || (csr_id >= 0xc00 && csr_id <= 0xc1f && csr_id != CSR_TIME)
            || (csr_id >= 0xc80 && csr_id <= 0xc9f && csr_id != CSR_TIMEH)
            || (csr_id >= 0x323 && csr_id <= 0x33f)
            || (csr_id >= 0x723 && csr_id <= 0x73f)
            || csr_id == CSR_MCOUNTINHIBIT || csr_id == CSR_SCOUNTOVF;
    bool ret = pmu ? riscv_pmu_csr(vm, csr_id, dest, op, is_write)
                   : riscv_csr_op_internal(vm, csr_id, dest, op, is_write);
    if (!vm->rv64) {
        // Sign-extend the result into the register
        *dest = (int32_t)*dest;
    }
    return ret;
}

void riscv_csr_init(rvvm_hart_t* vm)
{
    // Delegate exceptions from M to S
    vm->csr.edeleg[RISCV_PRIV_HYPERVISOR] = 0xFFFFFFFF;
    vm->csr.ideleg[RISCV_PRIV_HYPERVISOR] = 0xFFFFFFFF;

    if (vm->rv64) {
#ifdef USE_RV64
        vm->csr.status = 0xA00000000;
        vm->csr.isa    = CSR_MISA_RV64;
#else
        rvvm_warn("Requested RV64 in RV32-only build");
#endif
    } else {
        vm->csr.isa = CSR_MISA_RV32;
    }
}

void riscv_csr_sync_fpu(rvvm_hart_t* vm)
{
#ifdef USE_FPU
    riscv_update_fflags(vm);
    riscv_update_fcsr(vm, 0, vm->csr.fcsr);
#else
    UNUSED(vm);
#endif
}

POP_OPTIMIZATION_SIZE
