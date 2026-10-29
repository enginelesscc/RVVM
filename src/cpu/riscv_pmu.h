/* SPDX-License-Identifier: MPL-2.0 */

#ifndef RISCV_PMU_H
#define RISCV_PMU_H

#include <core/rvvm.h>

#define RISCV_PMU_CYCLE_HZ 3000000000ULL
#define RISCV_PMU_COUNTERS 7
#define RISCV_PMU_MASK     0x7dU
#define RISCV_PMU_OF       (1ULL << 63)
#define RISCV_PMU_FILTER   ((1ULL << 62) | (1ULL << 61) | (1ULL << 60))

void     riscv_pmu_init(rvvm_hart_t* vm);
void     riscv_pmu_update(rvvm_hart_t* vm);
uint64_t riscv_pmu_delay_ns(rvvm_hart_t* vm);
void     riscv_pmu_retire(rvvm_hart_t* vm, uint8_t priv, uint32_t inhibit, uint32_t insn);
void     riscv_pmu_reconfigure(rvvm_hart_t* vm);
bool     riscv_pmu_csr(rvvm_hart_t* vm, uint32_t csr, rvvm_uxlen_t* dest, uint8_t op, bool write);

#endif
