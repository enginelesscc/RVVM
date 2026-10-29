# Guest framebuffer and input control

Start RVVM with `-gui none -control 1337` to keep the emulated display and HID devices without opening a host window. Other GUI backends can also use `-control`. Use `-usb-input` when the guest needs USB keyboard and mouse devices. `-nogui` removes the default display and is not a substitute for `-gui none`.

The optional control server binds only to `127.0.0.1`. It permits one connection at a time and disconnects after ten idle seconds. Treat it as a local debugging interface: any local process can connect. Disconnecting or issuing `release` releases injected keys and mouse buttons.

The Python client needs Python 3 and Pillow for PNG capture:

```powershell
python tools/guest_control.py frame frame.png
python tools/guest_control.py click 500 199
python tools/guest_control.py chord 0xe0 0xe1 0x29
python tools/guest_control.py text 'taskmgr'
```

The chord example sends left Control + left Shift + Escape. Keys use USB HID keyboard usage codes. Text entry assumes a US keyboard layout. Mouse coordinates refer to the guest framebuffer, independent of host window position or scaling. Add `--port PORT` before the command to select another port.

Scripts can import `Guest` from `guest_control`, then use it as a context manager. Methods include `frame(path)`, `key(code, down)`, `chord(*codes)`, `mouse(x, y, buttons=0)`, `click(x, y, button=1)`, `text(value)`, and `request(command)`.

The socket accepts newline-terminated ASCII commands:

| Command | Result |
| --- | --- |
| `frame` | `FRAME width height stride format byte_count\n`, followed by exactly `byte_count` raw framebuffer bytes |
| `key usage down` | Press (`1`) or release (`0`) a HID key |
| `mouse x y buttons` | Set absolute position and left/right/middle button bits (`1/2/4`) |
| `scroll amount` | Relative wheel motion, from -127 to 127 |
| `release` | Release all injected input |
| `close` | Acknowledge, then close the VM like the host window close button; permits profile counters to flush on normal process exit. With `-poweroff_key`, sends the guest power key instead. |

Input commands reply `OK\n` or `ERROR reason\n`. Requests are limited to 255 bytes and frames to 64 MiB. Input and socket work run in the existing display poll loop. Frame capture copies the current scanout; it does not pause guest drawing, so an actively changing frame can tear. Move the cursor away and allow the UI to settle when comparing screenshots.

## Current build

Run `rebuild.ps1` to build Release with LTO and update `../bin/rvvm.exe`.
The current checkout contains the PMU and localhost framebuffer/input control changes.

## LiberNT boot benchmark

From `X:\ros`, run the following outside the restricted execution sandbox, with
Pillow installed and two RVVM executables built with the same settings:

```powershell
python rvvm\tools\boot_bench.py --before <old-exe> --after <new-exe> --pairs 3 --output rvvm\build\boot-bench\comparison
```

This invokes `run-libernt-withtools.cmd` with one CPU, the tools disk and large
pages requested. It resets disposable copies of both disks for each run and
alternates order. It samples the framebuffer about once per second, records the
first desktop observation and confirms it on the next frame, closes the VM,
and saves screenshots, logs, hashes and timings. The pixel detector is specific
to the supplied 1024x768 live image; it detects the taskbar and wallpaper,
not application readiness. Check the logs for actual RAM backing. Times include
launcher startup and frame capture; scheduling, host load and cache state can
affect them. The runner requires localhost port 1337 and the configured
clipboard port to be free.

On 29 October 2026, using the local `LiberNT.vhd` (SHA-256 beginning
`0171dff8`) and Release LTO GCC builds, all six boots used 2 MiB large pages.
Before timings were 40.82, 42.55 and 41.08 seconds; after deferring inactive
PMU updates to the existing JIT budget they were 32.40, 32.42 and 29.89 seconds.
The medians were 41.08 and 32.40 seconds (21.1% less). A short test build
overlapped the first after run. This is a small, unpinned sample on one host.

An initial Windows sampled CPU trace started after boot began and continued
briefly after desktop. Of approximately 34,600 RVVM CPU samples, roughly 4,700
landed in `rvtimer_clocksource` and 8,200 landed in Windows code with that
function on the stack, mostly below `riscv_pmu_update` on JIT return. JIT code
itself appeared as unregistered native code in the trace. These samples identify
the clock-update overhead, not a complete breakdown of boot wall time. Raw
local results and the trace are in `build/boot-bench`; the trace contains
system-wide process and image information and should be treated accordingly.

### Latest image and rounding-mode optimization

On 29 October 2026 the server's `LiberNT.vhd` (SHA-256
`49aa9a865321920543e7fc0797cc012f255f80bdced24fa97e000a7418912075`)
was benchmarked through the same launcher, using `--guest-image` to select the
downloaded source and fresh copies of both disks on every boot. All six runs
reported 1 GiB of actual 2 MiB large-page RAM. The Release LTO binaries were
alternated in AB/BA/AB order. First desktop observations in seconds were:

| Pair | Before | After |
| --- | ---: | ---: |
| 1 | 49.62 | 47.25 |
| 2 | 47.30 | 47.26 |
| 3 | 48.67 | 47.31 |

The medians were 48.67 and 47.26 seconds, a 2.9% reduction. This is a small,
unpinned sample, with about one-second frame polling, not a precise estimate.
The detector verifies wallpaper and taskbar, not full application readiness.
Raw timings, hashes, logs, and screenshots are in
`build/boot-bench/rounding-comparison/results.json` and its directory.

A Windows CPU trace of the baseline started after boot began and ended around
desktop. Of 34,424 RVVM samples, about 39% were in unregistered generated
code, 23% in `riscv64_run_interpreter`, 15% in `riscv_jit_tlb_lookup`, and
7% in the two main MMU translation/operation functions. About 3.4% were in
the floating-point rounding environment helper. These are CPU sample shares,
not shares of boot wall time; the JIT code cannot be resolved to guest functions
in this trace. Most captured callers of the rounding helper were floating-point
instruction emulation. The change combines the previous get and set into one
operation for static rounding, retaining restoration even if an instruction
changes the mode internally. Configure with `-DRVVM_FPU_TESTS=ON` and run
`ctest --test-dir build -R riscv_fpu_rounding --output-on-failure` to check
mode restoration and sticky exceptions.
