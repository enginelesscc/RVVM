/* SPDX-License-Identifier: MPL-2.0 */

#include <cpu/riscv_fpu.h>
#include <stdio.h>
#include <stdlib.h>

#define CHECK(x)                                                                                                       \
    do {                                                                                                               \
        if (!(x)) {                                                                                                    \
            fprintf(stderr, "FAIL line %u: %s\n", __LINE__, #x);                                                       \
            exit(1);                                                                                                   \
        }                                                                                                              \
    } while (0)

int main(void)
{
    const uint32_t modes[] = {FPU_LIB_ROUND_NE, FPU_LIB_ROUND_TZ, FPU_LIB_ROUND_DN, FPU_LIB_ROUND_UP};
    for (size_t i = 0; i < sizeof(modes) / sizeof(modes[0]); ++i) {
        for (size_t j = 0; j < sizeof(modes) / sizeof(modes[0]); ++j) {
            fpu_set_rounding_mode(modes[i]);
            fpu_set_exceptions(FPU_LIB_FLAG_NV | FPU_LIB_FLAG_NX);
            uint32_t previous = riscv_fpu_static_rm_enter(modes[j]);
            CHECK(previous == modes[i]);
            CHECK(fpu_get_rounding_mode() == modes[j]);
            CHECK((fpu_get_exceptions() & (FPU_LIB_FLAG_NV | FPU_LIB_FLAG_NX)) ==
                  (FPU_LIB_FLAG_NV | FPU_LIB_FLAG_NX));
            /* An operation may itself change rounding, as RMM preparation does. */
            fpu_set_rounding_mode(FPU_LIB_ROUND_DN);
            riscv_fpu_static_rm_leave(previous);
            CHECK(fpu_get_rounding_mode() == modes[i]);
            CHECK((fpu_get_exceptions() & (FPU_LIB_FLAG_NV | FPU_LIB_FLAG_NX)) ==
                  (FPU_LIB_FLAG_NV | FPU_LIB_FLAG_NX));
        }
        fpu_set_rounding_mode(modes[i]);
        CHECK(riscv_fpu_static_rm_enter(7) == 7);
        riscv_fpu_static_rm_leave(7);
        CHECK(fpu_get_rounding_mode() == modes[i]);
        uint32_t previous = riscv_fpu_static_rm_enter(FPU_LIB_ROUND_MM);
        CHECK(previous == modes[i]);
        CHECK(fpu_get_rounding_mode() == FPU_LIB_ROUND_NE);
        riscv_fpu_static_rm_leave(previous);
        CHECK(fpu_get_rounding_mode() == modes[i]);
    }
    fpu_set_exceptions(0);
    fpu_set_rounding_mode(FPU_LIB_ROUND_NE);
    puts("PASS floating-point rounding environment");
    return 0;
}
