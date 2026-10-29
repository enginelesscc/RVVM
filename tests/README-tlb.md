# Software TLB profiling and Windows large-page RAM

The prototype is opt-in. Normal launches retain the existing allocator.

```powershell
X:\ros\run-libernt.cmd -largepages -tlb_stats -v
```

`-largepages` tries `VirtualAlloc(MEM_RESERVE | MEM_COMMIT | MEM_LARGE_PAGES)`
for guest RAM only. It queries `GetLargePageMinimum`, requires the RAM size to
be an exact multiple, temporarily enables an already-assigned
`SeLockMemoryPrivilege`, and restores its prior enabled state. It does not
grant account rights or modify Windows security policy. Concurrent allocations
through this helper serialize their temporary process-token changes.

Unsupported APIs, missing privileges, incompatible sizes, and allocation failure
fall back to the existing allocator. `-v` prints the **actual** backing and
explicit page size; a requested large-page run is not necessarily a successful
large-page run. Explicit Windows large pages are resident/nonpageable. Guest
page sizes and permissions do not change. JIT heaps and framebuffer allocations
retain their existing behavior. This helper must only be used for RAM released
as a whole, not buffers using the generic decommit/recommit cleaning operation.

`-tlb_stats -v` prints per-hart cumulative counters on normal machine teardown:

- `miss-r/w/x`: tag misses serviced in the common MMU slow path, after splitting
  accesses crossing page boundaries. This covers interpreter accesses and JIT
  misses that return to the interpreter to refill the TLB. It does not count
  repeated native-JIT attempts before the miss is serviced.
- `ram-r/w/x`: those misses that successfully resolve to guest RAM.
- `mmio`, `unmapped`, `faults`: other outcomes for the counted misses.
- `slow-hit`: slow operations whose permission-specific tag was already present,
  such as unaligned accesses; these are not counted as TLB misses.
- `probes`: explicit physical/debug/no-trap translations excluded from miss totals.
- `walks`: paged slow-path translation attempts, including probes and slow hits.
- `flush-all/page`: software TLB flush calls, including privilege switches.

These are host diagnostics, not guest HPM counters or physical CPU TLB events.
Instruction fetches can bypass the software TLB through interpreter page-local
pointers or already-compiled JIT blocks. No generic miss percentage is printed:
hit counters would instrument the hot path. The benchmark has a known number of
guest loads and can therefore calculate an exact serviced read-miss percentage.
Diagnostics are not serialized in snapshots. Killing a process bypasses its
teardown report; close normally, including through the control server.

## Reproduce

Use the same compiler/settings as `tools/rebuild.ps1`, with these extra targets:

```powershell
$env:PATH = 'C:\Strawberry\c\bin;' + $env:PATH
cmake -S X:\ros\rvvm -B X:\ros\rvvm\build -G Ninja -DCMAKE_BUILD_TYPE=Release -DCMAKE_C_COMPILER=C:/Strawberry/c/bin/gcc.exe -DCMAKE_CXX_COMPILER=C:/Strawberry/c/bin/g++.exe -DBUILD_SHARED_LIBS=OFF -DUSE_LTO=ON -DRVVM_PMU_TESTS=ON -DRVVM_TLB_BENCH=ON
cmake --build X:\ros\rvvm\build --target rvvm_cli rvvm_pmu_test rvvm_tlb_bench -j 12
ctest --test-dir X:\ros\rvvm\build --output-on-failure
python X:\ros\rvvm\tests\run-tlb-bench.py --exe X:\ros\rvvm\build\rvvm_tlb_bench.exe --output X:\ros\rvvm\build\tlb-results-new --require-large
```

The benchmark executes RV64 load/decrement/branch loops through RVVM's normal
interpreter/JIT with Sv39 translation and 4 KiB guest leaf PTEs. Each run allocates
1 GiB, prefaults it outside timing, builds deterministic pointer chains, warms
the JIT and memory, and resets software TLB state before measurement. Each case
performs 20,000,000 dependent guest loads:

| Case | Working set/access pattern | Expected serviced read misses |
| --- | --- | --- |
| `tlb-fit-spread` | 128 pages spread across about 255 MiB, distinct software TLB slots | 128 compulsory misses |
| `page-random` | Fixed shuffled ring of 65,536 pages across 256 MiB | 20,000,000 |
| `cacheline-stream` | 64-byte steps through a 256 MiB ring | 312,500 |

The runner pins itself and its children to one allowed logical CPU (CPU 2 on
this machine). It runs one separate instrumented pass per allocation mode,
then seven alternating A/B timing pairs with counters disabled. Timing excludes
allocation, prefaulting, setup, warmup, and release. It validates pointer-chain
results, expected miss counts, actual allocation backing, and process exit.
Raw logs, executable hash, affinity, observations, medians, ranges, and paired
differences are retained. `--require-large` rejects fallback as a comparison.

Tests cover counter hits/refills, unaligned slow hits, writes, probes, page
flushes and faults, JIT and interpreter execution, unsuitable-size fallback,
and missing-privilege fallback. The missing-privilege test removes the right
only from its own short-lived test-process token. Every Windows test also
checks that allocation leaves the process privilege's enabled state unchanged.

## Measurement scope

These are synthetic guest-memory benchmarks, not a LiberNT application or boot
speed measurement. Background host activity, frequency changes, cache placement,
and the virtualization environment can affect timings. A lower host translation
cost need not reduce software TLB misses and need not improve every workload.
Keep large pages opt-in until the target application has been measured.

Windows API references: [large-page support](https://learn.microsoft.com/en-us/windows/win32/memory/large-page-support),
[VirtualAlloc](https://learn.microsoft.com/en-us/windows/win32/api/memoryapi/nf-memoryapi-virtualalloc),
[AdjustTokenPrivileges](https://learn.microsoft.com/en-us/windows/win32/api/securitybaseapi/nf-securitybaseapi-adjusttokenprivileges).

## Results on 29 October 2026

Host reports AMD EPYC-Milan-v4, Windows build 29683, x64. Build: GCC 8.3.0,
CMake Release (`-O2`), LTO, normal RVJIT enabled, existing PMU implementation.
Windows large-page size: 2,097,152 bytes. Every requested large-page benchmark
successfully allocated the full 1 GiB with `MEM_LARGE_PAGES`. The earlier
privilege failure was specific to the restricted execution token; the normal
host token can allocate large pages. No system policy change was required.

Final build, seven timing repetitions per mode, 20 million loads per case,
logical CPU 2, counters disabled during timing:

| Case | RAM read misses (both modes) | Read miss rate | Ordinary median | Large-page median | Change in median elapsed time |
| --- | ---: | ---: | ---: | ---: | ---: |
| TLB-fit spread | 128 | 0.00064% | 118.465 ms | 121.977 ms | 2.97% longer |
| Random pages | 20,000,000 | 100% | 3,010.981 ms | 2,875.496 ms | 4.50% shorter |
| Cache-line stream | 312,500 | 1.5625% | 100.852 ms | 95.793 ms | 5.02% shorter |

Software misses and page-walk counts matched across allocation modes. The
random-page samples ranged from 2,921.945–3,255.244 ms with ordinary pages and
2,549.109–3,478.911 ms with large pages: there is substantial variability and
overlap, so the median difference is not a guaranteed speedup. Adjacent-pair
median elapsed-time reductions were -1.88%, +8.59%, and +5.37%, respectively;
these differ from ratios of the group medians because host conditions varied.
An initial seven-pair run produced -3.92%, +4.49%, and +5.21% group-median
reductions. Both raw runs are retained rather than selecting only favorable
samples. These results support retaining the opt-in default.

Artifacts in this workspace:

- [Final raw observations](../build/tlb-results-final/raw.json) and
  [summary including all samples](../build/tlb-results-final/summary.json).
- [Initial summary](../build/tlb-results/summary.json), with its measured
  executable retained in the same directory.
- [Native regression log](../build/Testing/Temporary/LastTest.log): five tests
  passed, including both interpreter and JIT, both fallback conditions, and
  the existing PMU/snapshot regression.
- [Large-page firmware run](../build/tlb-firmware/pmu-sbi-largepages.log): the
  diskless OpenSBI PMU suite passed with 128 MiB of actual large-page RAM. The
  same suite also passed with ordinary pages in JIT and interpreter modes.
  Its counter report separated 104,328 MMIO misses from 913 RAM misses; this
  illustrates why a raw slow-path total alone overstates RAM TLB pressure.

The tested CLI is installed at `bin/rvvm.exe`. The previous executable is
retained at `build/rvvm-before-largepages.exe`. The normal launcher requires
`-largepages` to request the new allocation mode; use `-tlb_stats -v` only when
collecting diagnostic counts, and disable profiling when timing a workload.
