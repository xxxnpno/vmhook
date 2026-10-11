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

from runtime_evidence import runtime_evidence, read_results


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--java', default='java')
    parser.add_argument('--javac', default='javac')
    parser.add_argument('--injector', type=Path, required=True)
    parser.add_argument('--probe', type=Path, required=True)
    parser.add_argument('--evidence', type=Path, help='write copied hashes, checks and failure diagnostics as JSON')
    args = parser.parse_args()
    if os.name != 'nt':
        parser.error('this runtime harness requires native Windows')
    source = Path(__file__).with_name('interface_fixture.java')
    helper = Path(__file__).resolve().parents[2] / 'java/vmhook/callback_handler.java'
    with tempfile.TemporaryDirectory(prefix='vmhook-interface-') as temporary:
        directory = Path(temporary)
        result = directory / 'result.txt'
        evidence = runtime_evidence(args.evidence, 'interface_fixture')
        process = None
        reader = None
        passed = False
        failure = None
        output = queue.Queue()

        def capture(line):
            evidence.report['java_output'].append(line)
            print(line, flush=True)

        def read_output():
            for line in process.stdout:
                output.put(line.rstrip())

        try:
            evidence.add_input('fixture_source', source)
            evidence.add_input('java_callback_helper', helper)
            evidence.add_runtime(args.java, args.javac, args.injector, args.probe)
            compilation = subprocess.run([args.javac, '-d', str(directory), str(helper), str(source)],
                                         stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True, timeout=90)
            evidence.report['java_compiler_output'] = compilation.stdout
            if compilation.stdout:
                print(compilation.stdout, end='', flush=True)
            compilation.check_returncode()
            evidence.add_classes(directory)
            process = subprocess.Popen([args.java, '-cp', str(directory), 'interface_fixture'],
                                       env=dict(os.environ, VMHOOK_INTERFACE_RESULT=str(result)),
                                       stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True,
                                       cwd=directory)
            reader = threading.Thread(target=read_output, daemon=True)
            reader.start()
            deadline = time.monotonic() + 45
            while time.monotonic() < deadline:
                if process.poll() is not None:
                    raise RuntimeError(f'Java exited before injection: {process.returncode}')
                try:
                    line = output.get(timeout=0.25)
                except queue.Empty:
                    continue
                capture(line)
                if line == 'READY':
                    break
            else:
                raise RuntimeError('Java interface fixture did not become ready')
            evidence.report['windows_pid'] = process.pid
            subprocess.run([str(args.injector.resolve()), str(process.pid),
                            str(args.probe.resolve())], check=True, timeout=15)
            values = {}
            deadline = time.monotonic() + 150
            while time.monotonic() < deadline:
                if result.is_file():
                    values = read_results(result)
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
            evidence.verify_probe(args.probe)
            passed = True
            return 0
        except Exception as error:
            failure = error
            raise
        finally:
            if process is not None and process.poll() is None:
                process.terminate()
                try:
                    process.wait(timeout=5)
                except subprocess.TimeoutExpired:
                    process.kill()
                    process.wait()
            if reader is not None:
                reader.join(timeout=1)
            while not output.empty():
                capture(output.get_nowait())
            evidence.report['checks'] = read_results(result)
            evidence.finish(passed, failure, directory)


if __name__ == '__main__':
    try:
        sys.exit(main())
    except (OSError, RuntimeError, subprocess.SubprocessError) as error:
        print(f'Interface runtime test failed: {error}', file=sys.stderr)
        sys.exit(1)
