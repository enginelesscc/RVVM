/* SPDX-License-Identifier: MPL-2.0 */

typedef unsigned long long U64;
typedef unsigned long      ULONG;
typedef long               STATUS;
typedef void*              HANDLE;

typedef struct {
    U64 accumulated, timestamp;
} CYCLES;

__declspec(dllimport) STATUS NtCreateProfile(HANDLE*, HANDLE, void*, __SIZE_TYPE__, ULONG, ULONG*, ULONG, ULONG,
                                             __SIZE_TYPE__);
__declspec(dllimport) STATUS NtStartProfile(HANDLE);
__declspec(dllimport) STATUS NtStopProfile(HANDLE);
__declspec(dllimport) STATUS NtSetIntervalProfile(ULONG, ULONG);
__declspec(dllimport) STATUS NtQueryInformationThread(HANDLE, ULONG, void*, ULONG, ULONG*);
__declspec(dllimport) STATUS NtQueryInformationProcess(HANDLE, ULONG, void*, ULONG, ULONG*);
__declspec(dllimport) STATUS NtClose(HANDLE);
__declspec(dllimport) ULONG  DbgPrint(const char*, ...);
__declspec(dllimport) void   Sleep(ULONG);
__declspec(dllimport) void   ExitProcess(ULONG);
__declspec(dllimport) HANDLE GetStdHandle(ULONG);
__declspec(dllimport) int    WriteFile(HANDLE, const void*, ULONG, ULONG*, void*);
static volatile U64          result;

static void log_line(const char* text)
{
    ULONG size = 0, written;
    while (text[size]) {
        size++;
    }
    DbgPrint("PMUTEST: %s", text);
    WriteFile(GetStdHandle((ULONG)-11), text, size, &written, 0);
}

static void check(int ok, const char* stage)
{
    if (!ok) {
        log_line("FAIL ");
        log_line(stage);
        log_line("\r\n");
        ExitProcess(1);
    }
}

static U64 cycle(void)
{
    U64 v;
    __asm__ volatile("rdcycle %0" : "=r"(v));
    return v;
}

static U64 timer(void)
{
    U64 v;
    __asm__ volatile("rdtime %0" : "=r"(v));
    return v;
}

__attribute__((noinline, aligned(4096))) static void workload(void)
{
    for (unsigned i = 0; i < 200000; ++i) {
        result += i;
    }
}

void pmu_entry(void)
{
    log_line("START LiberNT cycle/PMU integration\r\n");
    U64 c0 = cycle(), t0 = timer();
    Sleep(20);
    U64 td = timer() - t0, cd = cycle() - c0;
    check(td && cd / td >= 270 && cd / td <= 330, "user cycle/time frequency");
    log_line("PASS user rdcycle at 3 GHz, rdtime at 10 MHz\r\n");
    CYCLES before, after, process;
    check(NtQueryInformationThread((HANDLE)-2, 23, &before, sizeof(before), 0) == 0, "thread cycle query");
    workload();
    Sleep(1);
    check(NtQueryInformationThread((HANDLE)-2, 23, &after, sizeof(after), 0) == 0, "thread cycle query after work");
    check(NtQueryInformationProcess((HANDLE)-1, 38, &process, sizeof(process), 0) == 0, "process cycle query");
    check(after.accumulated > before.accumulated, "cycle accounting increases");
    U64 now = cycle();
    check(now >= after.timestamp && now - after.timestamp < 3000000000ULL, "thread timestamp in cycle units");
    check(now >= process.timestamp && now - process.timestamp < 3000000000ULL, "process timestamp in cycle units");
    log_line("PASS thread/process cycle accounting and timestamps\r\n");
    const ULONG sources[] = {2, 6, 19};
    for (unsigned i = 0; i < 3; ++i) {
        HANDLE         profile;
        volatile ULONG samples = 0;
        check(NtSetIntervalProfile(i == 2 ? 30000000 : 65536, sources[i]) == 0, "set profile interval");
        check(
            NtCreateProfile(&profile, (HANDLE)-1, workload, 4096, 12, (ULONG*)&samples, sizeof(samples), sources[i], 1)
                == 0,
            "create profile");
        check(NtStartProfile(profile) == 0, "start profile");
        for (unsigned j = 0; j < 20 && samples < 3; ++j) {
            workload();
        }
        check(samples >= 3, "samples delivered and rearmed");
        ULONG first = samples;
        for (unsigned j = 0; j < 20 && samples == first; ++j) {
            workload();
        }
        check(samples > first, "live samples increase");
        check(NtStopProfile(profile) == 0, "stop profile");
        ULONG stopped = samples;
        workload();
        check(samples == stopped, "samples stop");
        check(NtClose(profile) == 0, "close profile");
        log_line(i == 0   ? "PASS instruction profile\r\n"
                 : i == 1 ? "PASS branch profile\r\n"
                          : "PASS cycle profile\r\n");
    }
    log_line("PASS LiberNT cycle/PMU integration\r\n");
    ExitProcess(0);
}
