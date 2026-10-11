#!/usr/bin/env python3
"""Validate ordinary Java interface callbacks on an owned Windows JVM.

The JVM receives only its fixture class path; there are no agent, compiler or
collector options. This harness injects only the subprocess it creates.
"""

import argparse
import os
from pathlib import Path
import queue
import subprocess
import sys
import tempfile
import threading
import time


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--java', default='java')
    parser.add_argument('--javac', default='javac')
    parser.add_argument('--injector', type=Path, required=True)
    parser.add_argument('--probe', type=Path, required=True)
    args = parser.parse_args()
    if os.name != 'nt':
        parser.error('this runtime harness requires native Windows')
    source = Path(__file__).with_name('interface_fixture.java')
    helper = Path(__file__).resolve().parents[2] / 'java/vmhook/callback_handler.java'
    with tempfile.TemporaryDirectory(prefix='vmhook-interface-') as temporary:
        directory = Path(temporary)
        result = directory / 'result.txt'
        subprocess.run([args.javac, '-d', str(directory), str(helper), str(source)], check=True)
        process = subprocess.Popen([args.java, '-cp', str(directory), 'interface_fixture'],
                                   env=dict(os.environ, VMHOOK_INTERFACE_RESULT=str(result)),
                                   stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True)
        output = queue.Queue()

        def read_output():
            for line in process.stdout:
                output.put(line.rstrip())

        threading.Thread(target=read_output, daemon=True).start()
        try:
            deadline = time.monotonic() + 45
            while time.monotonic() < deadline:
                if process.poll() is not None:
                    raise RuntimeError(f'Java exited before injection: {process.returncode}')
                try:
                    line = output.get(timeout=0.25)
                except queue.Empty:
                    continue
                print(line, flush=True)
                if line == 'READY':
                    break
            else:
                raise RuntimeError('Java interface fixture did not become ready')
            subprocess.run([str(args.injector.resolve()), str(process.pid),
                            str(args.probe.resolve())], check=True, timeout=15)
            values = {}
            deadline = time.monotonic() + 150
            while time.monotonic() < deadline:
                if result.is_file():
                    values = dict(line.split('=', 1) for line in result.read_text().splitlines()
                                  if '=' in line)
                    if ('interface_proof' in values or 'exception' in values
                            or any(value in ('fail', 'timeout') for value in values.values())):
                        break
                if process.poll() is not None:
                    raise RuntimeError(f'Java exited during the interface probe: {process.returncode}')
                time.sleep(0.1)
            for key, value in values.items():
                print(f'{key}={value}')
            required = ('interface_setup', 'interface_lifetime', 'interface_roundtrip',
                        'interface_errors', 'interface_gc', 'interface_close',
                        'interface_release', 'interface_proof')
            failed = [key for key in required if values.get(key) != 'pass']
            failed += [key for key, value in values.items()
                       if value in ('fail', 'timeout') or key == 'exception']
            if failed:
                raise RuntimeError('failed checks: ' + ', '.join(sorted(set(failed))))
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
        print(f'Interface runtime test failed: {error}', file=sys.stderr)
        sys.exit(1)
