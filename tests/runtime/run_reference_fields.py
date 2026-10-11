#!/usr/bin/env python3
"""Verify reference field ASM events under nested Java calls and moving GC.

Launches its own fixture without JVM tuning/agent flags. Native Windows is the
release proof; --wine also permits a supplemental Windows-binary check on Linux.
"""
from __future__ import annotations

import argparse
import os
from pathlib import Path
import queue
import subprocess
import sys
import tempfile
import threading
import time

REQUIRED = ('reference_install',)


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--java', default='java')
    parser.add_argument('--javac', default='javac')
    parser.add_argument('--injector', type=Path, required=True)
    parser.add_argument('--probe', type=Path, required=True)
    parser.add_argument('--wine', type=Path, help='supplemental Wine runner on a non-Windows host')
    args = parser.parse_args()
    if os.name != 'nt' and not args.wine:
        parser.error('use native Windows or explicitly supply --wine for a supplemental check')
    prefix = [str(args.wine.resolve())] if args.wine else []

    def windows_path(path: Path) -> str:
        if args.wine:
            return 'Z:' + str(path.resolve()).replace('/', '\\')
        return str(path.resolve())

    with tempfile.TemporaryDirectory(prefix='vmhook-reference-fields-') as temporary:
        directory = Path(temporary)
        destination = directory / 'result.txt'
        control = directory / 'control'
        fixture = Path(__file__).with_name('ReferenceFieldFixture.java')
        subprocess.run(prefix + [args.javac, '-d', windows_path(directory), windows_path(fixture)],
                       check=True, timeout=90)
        environment = dict(os.environ, VMHOOK_TEST_RESULT=windows_path(destination), VMHOOK_REFERENCE_CONTROL=windows_path(control))
        process = subprocess.Popen(prefix + [args.java, '-cp', windows_path(directory), 'ReferenceFieldFixture'],
                                   stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                                   text=True, env=environment)
        lines: queue.Queue[str] = queue.Queue()

        def read_output() -> None:
            for line in process.stdout:
                lines.put(line.rstrip())

        threading.Thread(target=read_output, daemon=True).start()
        try:
            deadline = time.monotonic() + 60
            pid = None
            while time.monotonic() < deadline:
                try:
                    line = lines.get(timeout=0.25)
                except queue.Empty:
                    if process.poll() is not None:
                        raise RuntimeError('Java exited before fixture readiness')
                    continue
                print(line, flush=True)
                if line.startswith('PID='):
                    pid = line[4:]
                if line == 'READY' and pid:
                    break
            else:
                raise RuntimeError('Interop fixture readiness timeout')
            subprocess.run(prefix + [str(args.injector.resolve()), pid, windows_path(args.probe)],
                           check=True, timeout=30)
            values: dict[str, str] = {}
            java_verified = False
            deadline = time.monotonic() + 300
            while time.monotonic() < deadline:
                while not lines.empty():
                    line = lines.get_nowait()
                    print(line, flush=True)
                    java_verified = java_verified or line == 'REFERENCE_FIELDS=pass'
                if destination.is_file():
                    values = dict(line.split('=', 1) for line in destination.read_text().splitlines()
                                  if '=' in line)
                    if java_verified or 'exception' in values:
                        break
                if process.poll() is not None:
                    raise RuntimeError('Java exited during the interop probe')
                time.sleep(0.1)
            for key, value in values.items():
                print(f'{key}={value}', flush=True)
            missing = [key for key in REQUIRED if values.get(key) != 'pass']
            if missing or 'exception' in values or not java_verified:
                raise RuntimeError('Reference field checks failed: ' + ', '.join(missing)
                                   + ('; ' + values['exception'] if 'exception' in values else ''))
            return 0
        finally:
            process.terminate()
            try:
                process.wait(timeout=5)
            except subprocess.TimeoutExpired:
                process.kill()
                process.wait()


if __name__ == '__main__':
    try:
        sys.exit(main())
    except (OSError, RuntimeError, subprocess.SubprocessError) as error:
        print(f'Reference field test failed: {error}', file=sys.stderr)
        sys.exit(1)
