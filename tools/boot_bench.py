"""Repeat the workspace launcher boot using disposable copies of both disks.

The desktop detector is specific to the 1024x768 LiberNT live image wallpaper.
Run on an otherwise idle host, outside a sandbox that blocks RVVM executables.
"""
import argparse
import hashlib
import json
import os
from pathlib import Path
import shutil
import subprocess
import time

from guest_control import Guest
from PIL import Image


def digest(path):
    h = hashlib.sha256()
    with open(path, 'rb') as f:
        for chunk in iter(lambda: f.read(1024 * 1024), b''):
            h.update(chunk)
    return h.hexdigest()


def desktop(path):
    with Image.open(path) as im:
        return (im.size == (1024, 768) and im.getpixel((200, 200)) == (0, 29, 85)
                and im.getpixel((500, 750)) == (25, 36, 56))


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('--before', type=Path, required=True)
    parser.add_argument('--after', type=Path, required=True)
    parser.add_argument('--pairs', type=int, default=3)
    parser.add_argument('--timeout', type=int, default=180)
    parser.add_argument('--guest-image', type=Path, help='source VHD (default: workspace LiberNT.vhd)')
    parser.add_argument('--output', type=Path, required=True)
    args = parser.parse_args()
    root = Path(__file__).resolve().parents[2]
    out = args.output.resolve()
    out.mkdir(parents=True, exist_ok=True)
    image = (args.guest_image or root / 'LiberNT.vhd').resolve()
    tools = root / 'clipboard-tools' / 'clipboard-tools.vhd'
    guest_copy, tools_copy = out / 'guest.vhd', out / 'tools.vhd'
    results = {'guest_sha256': digest(image), 'tools_sha256': digest(tools),
               'executables': {k: {'path': str(v.resolve()), 'sha256': digest(v)}
                               for k, v in [('before', args.before), ('after', args.after)]},
               'runs': []}
    env = os.environ.copy()
    env['PATH'] = r'C:\Strawberry\c\bin;' + env['PATH']
    for pair in range(args.pairs):
        for label, exe in ([('before', args.before), ('after', args.after)] if pair % 2 == 0
                           else [('after', args.after), ('before', args.before)]):
            shutil.copyfile(image, guest_copy)
            shutil.copyfile(tools, tools_copy)
            name = f'{pair + 1}-{label}'
            command = ['cmd.exe', '/c', str(root / 'run-libernt-withtools.cmd'),
                       '-GuestImage', str(guest_copy), '-ToolsImage', str(tools_copy),
                       '-Emulator', str(exe.resolve()), '-gui', 'none', '-control', '1337', '-v']
            start = time.perf_counter()
            with (out / f'{name}.log').open('wb') as log:
                proc = subprocess.Popen(command, cwd=root, env=env, stdout=log, stderr=subprocess.STDOUT,
                                        creationflags=subprocess.CREATE_NO_WINDOW)
                frame = out / f'{name}.png'
                seen = 0
                try:
                    while time.perf_counter() - start < args.timeout:
                        if proc.poll() is not None:
                            raise RuntimeError(f'{name}: launcher exited {proc.returncode}')
                        try:
                            with Guest() as guest:
                                guest.frame(frame)
                            now = time.perf_counter()
                            seen = seen + 1 if desktop(frame) else 0
                            if seen == 1:
                                first = now - start
                            if seen == 2:
                                with Guest() as guest:
                                    guest.request('close')
                                proc.wait(timeout=20)
                                if proc.returncode:
                                    raise RuntimeError(f'{name}: launcher exited {proc.returncode}')
                                result = {'pair': pair + 1, 'variant': label, 'desktop_seconds': first,
                                          'confirmed_seconds': now - start, 'exit': proc.returncode}
                                results['runs'].append(result)
                                (out / 'results.json').write_text(json.dumps(results, indent=2))
                                print(json.dumps(result), flush=True)
                                break
                        except (ConnectionError, OSError):
                            pass
                        time.sleep(1)
                    else:
                        raise RuntimeError(f'{name}: desktop not detected within {args.timeout}s')
                finally:
                    if proc.poll() is None:
                        try:
                            with Guest() as guest:
                                guest.request('close')
                            proc.wait(timeout=20)
                        except Exception:
                            subprocess.run(['taskkill', '/PID', str(proc.pid), '/T', '/F'],
                                           stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    print('Results:', out / 'results.json')


if __name__ == '__main__':
    main()
