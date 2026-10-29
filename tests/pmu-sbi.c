/* SPDX-License-Identifier: MPL-2.0 */

typedef unsigned long u64;

struct result {
    long error;
    u64  value;
};

static volatile u64 interrupts;
static volatile u64 counter;
static volatile u64 interval = 65536;

static struct result sbi(u64 ext, u64 fn, u64 x0, u64 x1, u64 x2, u64 x3)
{
    register u64 a0 __asm__("a0") = x0;
    register u64 a1 __asm__("a1") = x1;
    register u64 a2 __asm__("a2") = x2;
    register u64 a3 __asm__("a3") = x3;
    register u64 a4 __asm__("a4") = 0;
    register u64 a5 __asm__("a5") = 0;
    register u64 a6 __asm__("a6") = fn;
    register u64 a7 __asm__("a7") = ext;
    __asm__ volatile("ecall" : "+r"(a0), "+r"(a1) : "r"(a2), "r"(a3), "r"(a4), "r"(a5), "r"(a6), "r"(a7) : "memory");
    return (struct result) {a0, a1};
}

static void print(const char* s)
{
    volatile unsigned char* uart = (void*)0x10000000;
    while (*s) {
        while (!(uart[5] & 32)) {
        }
        uart[0] = *s++;
    }
}

static void print_hex(u64 value)
{
    char text[19] = "0x0000000000000000";
    for (unsigned i = 0; i < 16; ++i) {
        text[17 - i] = "0123456789abcdef"[(value >> (i * 4)) & 15];
    }
    print(text);
    print("\n");
}

static void finish(int success)
{
    print(success ? "PASS SBI PMU integration\n" : "FAIL SBI PMU integration\n");
    sbi(0x53525354, 0, 0, success ? 0 : 1, 0, 0);
    for (;;) {
        __asm__ volatile("wfi");
    }
}

static void check(int ok, const char* stage)
{
    if (!ok) {
        print(stage);
        print("\n");
        finish(0);
    }
}

void handle_trap(void)
{
    u64 cause, overflow;
    __asm__ volatile("csrr %0, scause" : "=r"(cause));
    check(cause == ((1UL << 63) | 13), "unexpected trap");
    __asm__ volatile("csrr %0, 0xda0" : "=r"(overflow));
    check(overflow & (1UL << counter), "missing scountovf");
    __asm__ volatile("csrc sip, %0" ::"r"(1UL << 13) : "memory");
    check(sbi(0x504d55, 4, counter, 1, 0, 0).error == 0, "stop in ISR");
    check(sbi(0x504d55, 3, counter, 1, 1, -interval).error == 0, "rearm in ISR");
    interrupts++;
}

extern void trap_entry(void);

void test_main(void)
{
    print("START SBI PMU integration\n");
    __asm__ volatile("csrw stvec, %0" ::"r"(trap_entry));
    check(sbi(0x10, 3, 0x504d55, 0, 0, 0).value != 0, "PMU extension");
    struct result r = sbi(0x504d55, 0, 0, 0, 0, 0);
    check(!r.error && r.value >= 7, "counter discovery");
    u64 t0, t1, c0, c1;
    __asm__ volatile("rdtime %0; rdcycle %1" : "=r"(t0), "=r"(c0));
    do {
        __asm__ volatile("rdtime %0" : "=r"(t1));
    } while (t1 - t0 < 10000);
    __asm__ volatile("rdcycle %0" : "=r"(c1));
    u64 ratio = (c1 - c0) / (t1 - t0);
    check(ratio >= 270 && ratio <= 330, "3 GHz virtual clock / 10 MHz timebase");
    print("PASS cycle/time frequency\n");
    const unsigned events[] = {2, 5, 1};
    for (unsigned i = 0; i < 3; ++i) {
        r = sbi(0x504d55, 2, 3, 15, 2 | 128, events[i]);
        check(!r.error && r.value >= 3 && r.value <= 6, "configure hardware event");
        counter = r.value;
        r       = sbi(0x504d55, 1, counter, 0, 0, 0);
        check(!r.error && (r.value & 0xfff) == 0xc00 + counter && ((r.value >> 12) & 63) == 63,
              "counter info CSR/width");
        interrupts = 0;
        check(!sbi(0x504d55, 3, counter, 1, 1, -65536UL).error, "start hardware event");
        __asm__ volatile("csrs sie, %0; csrsi sstatus, 2" ::"r"(1UL << 13) : "memory");
        __asm__ volatile("rdtime %0" : "=r"(t0));
        do {
            __asm__ volatile("li t0, 2000; 1: addi t0, t0, -1; bnez t0, 1b" ::: "t0", "memory");
            __asm__ volatile("rdtime %0" : "=r"(t1));
        } while (interrupts < 3 && t1 - t0 < 10000000);
        __asm__ volatile("csrci sstatus, 2; csrc sie, %0" ::"r"(1UL << 13) : "memory");
        if (interrupts < 3) {
            print("overflow interrupt count: ");
            print_hex(interrupts);
            u64 value;
            __asm__ volatile("csrr %0, 0xc03" : "=r"(value));
            print("hpmcounter3: ");
            print_hex(value);
        }
        check(interrupts >= 3, "overflow interrupts/rearm");
        check(!sbi(0x504d55, 4, counter, 1, 1, 0).error, "stop/reset hardware event");
        print(i == 0   ? "PASS instruction overflow interrupts\n"
              : i == 1 ? "PASS branch overflow interrupts\n"
                       : "PASS cycle overflow interrupts\n");
    }
    r = sbi(0x504d55, 2, 3, 15, 2 | 128, 1);
    check(!r.error, "configure WFI cycle event");
    counter    = r.value;
    interval   = 30000000;
    interrupts = 0;
    check(!sbi(0x504d55, 3, counter, 1, 1, -interval).error, "start WFI cycle event");
    __asm__ volatile("csrs sie, %0" ::"r"(1UL << 13) : "memory");
    while (interrupts < 3) {
        __asm__ volatile("wfi; csrsi sstatus, 2; nop; csrci sstatus, 2" ::: "memory");
    }
    __asm__ volatile("csrc sie, %0" ::"r"(1UL << 13) : "memory");
    check(!sbi(0x504d55, 4, counter, 1, 1, 0).error, "stop WFI cycle event");
    print("PASS cycle overflow wakes WFI\n");
    check(sbi(0x504d55, 2, 3, 15, 2, 4).error != 0, "unmodeled cache events rejected");
    finish(1);
}
