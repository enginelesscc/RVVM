# SPDX-License-Identifier: MPL-2.0
"""Alternate ordinary/large-page diskless RV64 benchmarks, retaining raw observations."""
import argparse
import ctypes
import hashlib
import json
import os
from pathlib import Path
import platform
import statistics
import subprocess
import time


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--exe', type=Path, required=True)
    parser.add_argument('--output', type=Path, required=True)
    parser.add_argument('--repeats', type=int, default=7)
    parser.add_argument('--iterations', type=int, default=20000000)
    parser.add_argument('--require-large', action='store_true')
    args = parser.parse_args()
    if args.repeats < 2 or args.iterations < 1:
        parser.error('need at least two repeats and positive iterations')
    args.output.mkdir(parents=True, exist_ok=True)
    exe = args.exe.resolve()
    affinity = None
    if os.name == 'nt':
        kernel = ctypes.WinDLL('kernel32', use_last_error=True)
        kernel.GetCurrentProcess.restype = ctypes.c_void_p
        kernel.GetProcessAffinityMask.argtypes = [ctypes.c_void_p, ctypes.POINTER(ctypes.c_size_t), ctypes.POINTER(ctypes.c_size_t)]
        kernel.SetProcessAffinityMask.argtypes = [ctypes.c_void_p, ctypes.c_size_t]
        process = kernel.GetCurrentProcess()
        allowed, system = ctypes.c_size_t(), ctypes.c_size_t()
        if not kernel.GetProcessAffinityMask(process, ctypes.byref(allowed), ctypes.byref(system)):
            raise ctypes.WinError(ctypes.get_last_error())
        cpus = [1 << i for i in range(64) if allowed.value & (1 << i)]
        affinity = cpus[min(2, len(cpus) - 1)]
        if not kernel.SetProcessAffinityMask(process, affinity):
            raise ctypes.WinError(ctypes.get_last_error())
        # Windows child processes inherit the parent's processor affinity.
    metadata = {'host': platform.platform(), 'processor': platform.processor(), 'affinity_mask': affinity,
                'exe': str(exe), 'sha256': hashlib.sha256(exe.read_bytes()).hexdigest(),
                'repeats': args.repeats, 'iterations': args.iterations,
                'started_local': time.strftime('%Y-%m-%dT%H:%M:%S%z')}
    observations = []

    def run(mode, profile, repetition):
        command = [str(exe), '-iterations', str(args.iterations), '-v']
        if mode == 'large-request':
            command.append('-largepages')
        if profile:
            command.append('-tlb_stats')
        start = time.perf_counter()
        completed = subprocess.run(command, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, timeout=120)
        log = completed.stdout.decode('utf-8', errors='replace')
        (args.output / f'{mode}-{profile}-{repetition}.log').write_text(log, encoding='utf-8')
        if completed.returncode or 'FAIL' in log:
            raise RuntimeError(log)
        rows = [json.loads(line[7:]) for line in log.splitlines() if line.startswith('RESULT ')]
        if len(rows) != 3:
            raise RuntimeError('Missing benchmark results: ' + log)
        for row in rows:
            row.update(request=mode, repetition=repetition)
            observations.append(row)
        if mode == 'large-request' and args.require_large and any(row['backing'] != 'large' for row in rows):
            raise RuntimeError('Large pages fell back; no genuine large-page comparison possible. See log.')
        (args.output / 'raw.json').write_text(json.dumps({'metadata': metadata, 'observations': observations}, indent=2))
        print(f'{mode} profile={profile} run={repetition} actual={rows[0]["backing"]} wall={time.perf_counter()-start:.2f}s', flush=True)

    # Counter runs are separate from timing runs so instrumentation doesn't bias the comparison.
    for mode in ('ordinary', 'large-request'):
        run(mode, True, 0)
    for repetition in range(args.repeats):
        order = ('ordinary', 'large-request') if repetition % 2 == 0 else ('large-request', 'ordinary')
        for mode in order:
            run(mode, False, repetition)
    summary = []
    for case in ('tlb-fit-spread', 'page-random', 'cacheline-stream'):
        entry = {'case': case}
        for mode in ('ordinary', 'large-request'):
            timings = [r['ns'] / 1e6 for r in observations if r['case'] == case and r['request'] == mode and not r['profile']]
            counts = [r for r in observations if r['case'] == case and r['request'] == mode and r['profile']][0]
            entry[mode] = {'actual_backing': counts['backing'], 'median_ms': statistics.median(timings),
                           'min_ms': min(timings), 'max_ms': max(timings), 'samples_ms': timings,
                           'read_misses': counts['ram_read_misses'],
                           'read_miss_percent': 100 * counts['ram_read_misses'] / counts['iterations']}
        entry['elapsed_reduction_percent'] = 100 * (1 - entry['large-request']['median_ms'] / entry['ordinary']['median_ms'])
        # Pair adjacent A/B trials to expose sensitivity to changing host load/frequency.
        paired = [100 * (1 - b / a) for a, b in zip(entry['ordinary']['samples_ms'], entry['large-request']['samples_ms'])]
        entry['paired_elapsed_reduction_percent'] = paired
        entry['paired_median_reduction_percent'] = statistics.median(paired)
        summary.append(entry)
    (args.output / 'summary.json').write_text(json.dumps({'metadata': metadata, 'results': summary}, indent=2))
    print(json.dumps(summary, indent=2))


if __name__ == '__main__':
    main()
