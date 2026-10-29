# SPDX-License-Identifier: MPL-2.0
"""Build and run the diskless OpenSBI PMU test with bounded execution."""
import argparse
from pathlib import Path
import subprocess

parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument('--rvvm', type=Path, required=True)
parser.add_argument('--firmware', type=Path, required=True)
parser.add_argument('--output', type=Path, required=True)
parser.add_argument('--clang', default='clang')
parser.add_argument('--objcopy', default='llvm-objcopy')
args = parser.parse_args()
source = Path(__file__).resolve().parent
output = args.output.resolve()
output.mkdir(parents=True, exist_ok=True)
elf = output / 'pmu-sbi.elf'
payload = output / 'pmu-sbi.bin'
subprocess.run([args.clang, '--target=riscv64-unknown-elf', '-march=rv64imac_zicsr',
                '-mabi=lp64', '-mcmodel=medany', '-mno-relax', '-O2', '-ffreestanding', '-nostdlib',
                '-Wl,-T,' + str(source / 'pmu-sbi.ld'), str(source / 'pmu-sbi.S'),
                str(source / 'pmu-sbi.c'), '-o', str(elf)], check=True, timeout=45)
subprocess.run([args.objcopy, '-O', 'binary', str(elf), str(payload)], check=True, timeout=45)
for mode in ('jit', 'interpreter'):
    command = [str(args.rvvm.resolve()), str(args.firmware.resolve()), '-k', str(payload),
               '-m', '128M', '-smp', '1', '-gui', 'none']
    if mode == 'interpreter':
        command.append('-nojit')
    completed = subprocess.run(command, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, timeout=45)
    log = completed.stdout.decode('utf-8', errors='replace')
    (output / ('pmu-sbi-' + mode + '.log')).write_text(log, encoding='utf-8')
    print('\n'.join(line for line in log.splitlines() if line.startswith(('PASS', 'FAIL'))), flush=True)
    if completed.returncode or 'FAIL' in log or 'PASS SBI PMU integration' not in log:
        raise SystemExit('SBI PMU test failed: ' + mode)
    print('PASS OpenSBI PMU (' + mode + ')', flush=True)
