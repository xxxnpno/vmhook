#!/usr/bin/env python3
"""Verify inferred constructor descriptors and typed hooks under moving GC.

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

from runtime_evidence import runtime_evidence, read_results

REQUIRED = ('exact_descriptors', 'string_constructor_upcasts', 'array_constructor_upcasts', 'nested_constructor', 'collection_constructor_upcasts', 'descriptor_refusals', 'inferred_hook_arguments', 'descriptor_complete')


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--java', default='java')
    parser.add_argument('--javac', default='javac')
    parser.add_argument('--injector', type=Path, required=True)
    parser.add_argument('--probe', type=Path, required=True)
    parser.add_argument('--wine', type=Path, help='supplemental Wine runner on a non-Windows host')
    parser.add_argument('--evidence', type=Path, help='write copied hashes, checks and failure diagnostics as JSON')
    args = parser.parse_args()
    if os.name != 'nt' and not args.wine:
        parser.error('use native Windows or explicitly supply --wine for a supplemental check')
    prefix = [str(args.wine.resolve())] if args.wine else []

    def windows_path(path: Path) -> str:
        if args.wine:
            return 'Z:' + str(path.resolve()).replace('/', '\\')
        return str(path.resolve())

    with tempfile.TemporaryDirectory(prefix='vmhook-argument-descriptors-') as temporary:
        directory = Path(temporary)
        destination = directory / 'result.txt'
        fixture = Path(__file__).with_name('ArgumentDescriptorFixture.java')
        evidence = runtime_evidence(args.evidence, 'ArgumentDescriptorFixture', args.wine)
        lines: queue.Queue[str] = queue.Queue()
        process = None
        reader = None
        passed = False
        failure = None

        def capture(line: str) -> None:
            evidence.report['java_output'].append(line)
            print(line, flush=True)

        def read_output() -> None:
            for line in process.stdout:
                lines.put(line.rstrip())

        try:
            evidence.add_input('fixture_source', fixture)
            evidence.add_runtime(args.java, args.javac, args.injector, args.probe)
            compilation = subprocess.run(prefix + [args.javac, '-d', windows_path(directory), windows_path(fixture)],
                                         stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True, timeout=90)
            evidence.report['java_compiler_output'] = compilation.stdout
            if compilation.stdout:
                print(compilation.stdout, end='', flush=True)
            compilation.check_returncode()
            evidence.add_classes(directory)
            environment = dict(os.environ, VMHOOK_TEST_RESULT=windows_path(destination))
            process = subprocess.Popen(prefix + [args.java, '-cp', windows_path(directory), 'ArgumentDescriptorFixture'],
                                       stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                                       text=True, env=environment, cwd=directory)
            reader = threading.Thread(target=read_output, daemon=True)
            reader.start()
            deadline = time.monotonic() + 60
            pid = None
            while time.monotonic() < deadline:
                try:
                    line = lines.get(timeout=0.25)
                except queue.Empty:
                    if process.poll() is not None:
                        raise RuntimeError('Java exited before fixture readiness')
                    continue
                capture(line)
                if line.startswith('PID='):
                    pid = line[4:]
                if line == 'READY' and pid and pid.isdecimal():
                    break
            else:
                raise RuntimeError('Argument descriptor fixture readiness timeout')
            evidence.report['windows_pid'] = int(pid)
            subprocess.run(prefix + [str(args.injector.resolve()), pid, windows_path(args.probe)],
                           check=True, timeout=30)
            values: dict[str, str] = {}
            deadline = time.monotonic() + 300
            while time.monotonic() < deadline:
                while not lines.empty():
                    line = lines.get_nowait()
                    capture(line)
                if destination.is_file():
                    values = read_results(destination)
                    if 'descriptor_complete' in values or 'exception' in values or 'fail' in values.values():
                        break
                if process.poll() is not None:
                    raise RuntimeError('Java exited during the argument descriptor probe')
                time.sleep(0.1)
            for key, value in values.items():
                print(f'{key}={value}', flush=True)
            missing = [key for key in REQUIRED if values.get(key) != 'pass']
            failed = [key for key, value in values.items() if value in ('fail', 'timeout')]
            if missing or failed or 'exception' in values:
                raise RuntimeError('Argument descriptor checks failed: ' + ', '.join(missing)
                                   + ('; ' + ', '.join(failed) if failed else '')
                                   + ('; ' + values['exception'] if 'exception' in values else ''))
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
            while not lines.empty():
                capture(lines.get_nowait())
            evidence.report['checks'] = read_results(destination)
            evidence.report['java_descriptor_fixture_ready'] = 'READY' in evidence.report['java_output']
            evidence.finish(passed, failure, directory)


if __name__ == '__main__':
    try:
        sys.exit(main())
    except (OSError, RuntimeError, subprocess.SubprocessError) as error:
        print(f'Argument descriptor test failed: {error}', file=sys.stderr)
        sys.exit(1)
