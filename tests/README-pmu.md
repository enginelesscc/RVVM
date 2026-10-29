# RISC-V counters and PMU

The emulated CPU now exposes independent 64-bit `cycle`/`mcycle` and
`instret`/`minstret` counters, plus four programmable HPM counters (3–6).
`time` remains the machine's 10 MHz timer. `cycle` is a fixed 3 GHz virtual
clock, matching the advertised CPU frequency; it measures host monotonic elapsed
time, including WFI and scheduling delays. It is not a measurement of physical
host CPU cycles or a cycle-accurate RISC-V pipeline simulation.

Retired instruction and conditional branch counts come from guest execution in
both the interpreter and JIT. Faulting instructions are excluded. JIT exits
account only their executed prefix, including exits caused by a TLB miss.
Overflow interrupts are observed at JIT block boundaries, so sampling can skid
past the exact overflowing instruction. Active HPM events force a dispatcher
return at every block exit; otherwise linked blocks return after a bounded
instruction budget.

Implemented interfaces:

- Zicntr/Zihpm, writable machine counters, read-only lower-privilege aliases,
  mcounteren/scounteren, mcountinhibit, RV32 high halves, and per-hart state.
- Sscofpmf event privilege filters, sticky overflow flags, scountovf, delegated
  local counter overflow interrupt 13, and cycle-overflow wakeup from WFI.
- A `riscv,pmu` device-tree node mapping SBI hardware events 1 (cycles),
  2 (instructions), and 5 (conditional branches). OpenSBI supplies the SBI PMU
  calls; no firmware modification is needed. Unmodelled cache/prediction events
  remain unavailable.
- Snapshot persistence. The hart snapshot section is versioned as
  `riscv-hart-pmu1`; pre-PMU hart snapshots are rejected rather than misread.

The x86 JIT uses fixed-size forward branches so a large conditional exit cannot
relocate already-recorded native block links. PMU epilogues exposed this hazard
under register pressure during UEFI boot.

## Native regression test

Configure the normal build with `-DRVVM_PMU_TESTS=ON`, build, then run:

```
ctest --test-dir <build> -R riscv_pmu --output-on-failure
```

This checks counter aliases and writes, clock scaling/inhibition, per-hart
independence, privilege access/filtering, sticky overflow/rearm, RV32 CSR halves,
exact interpreter/JIT retirement and branch counts, register-pressure branches,
compressed instructions, traps, TLB misses, and a PMU snapshot round trip.
The test requires RV64 support. RV32 high-half CSR behavior is exercised with
RV32 XLEN; full RV32 firmware boot is not covered.

## OpenSBI integration test

Requires Python, Clang with the RISC-V ELF target, llvm-objcopy, and OpenSBI
firmware accepting a payload at 0x80200000. The supplied U-Boot/OpenSBI 1.6
`fw_payload.bin` works with RVVM's kernel override:

```
python tests/run-pmu-sbi.py --rvvm <build>/rvvm --firmware <fw_payload.bin> --output <build>
```

On Windows use the `.exe` path and pass `--clang` / `--objcopy` if needed.
The runner builds the freestanding payload and runs it with JIT enabled and
disabled, enforcing a 45-second timeout per run and retaining both logs.
It verifies SBI discovery/configuration, counter widths, cycle/time scaling,
repeated interrupts for all three events, WFI wakeup, and rejection of an
unsupported event. Firmware shutdown alone is not success: the runner also
requires the final PASS marker.

## LiberNT integration test

`libernt-pmu.c` is a standalone RV64 Windows console test. Build it using the
LiberNT RosBE compiler from the RV64 output directory:

```
clang --target=riscv64-w64-mingw32 -march=rv64gc -O2 -ffreestanding -nostdlib -fuse-ld=lld -Wl,--subsystem,console:6.02 -Wl,-entry,pmu_entry -Wl,--major-os-version,10 -Wl,--minor-os-version,0 -o pmutest.exe <path>/libernt-pmu.c dll/ntdll/CMakeFiles/libntdll.dir/libntdll.a dll/win32/kernel32/CMakeFiles/libkernel32.dir/libkernel32.a
```

Run `start /b /wait /affinity 1 pmutest.exe` inside LiberNT. The test selects
CPU 0 in its profile affinity mask, so the workload must run on CPU 0 as well.
It checks user-mode rdcycle/rdtime, increasing
thread cycle accounting, thread/process timestamp units, and live start/rearm/
stop sampling through NtCreateProfile for instructions, branches, and cycles.
It prints results to both stdout and DbgPrint and exits nonzero on failure.

LiberNT's accompanying change uses rdcycle in ReadTimeStampCounter, enables
user CY access alongside TM, and returns cycle timestamps from the process and
thread cycle query routines. KeQueryPerformanceCounter continues using time.
