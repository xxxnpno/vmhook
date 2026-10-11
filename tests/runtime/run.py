#!/usr/bin/env python3
"""Inject the probe into a JVM launched without VM tuning/agent arguments.

Run on native Windows x64. Invocation, construction and GC stress must pass.
Unsupported operations fail this test. --metadata-only and --hooks-only run
separate baselines and do not claim to validate Java invocation/construction.
This harness only injects the Java subprocess it creates itself.
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
    mode = parser.add_mutually_exclusive_group()
    mode.add_argument('--metadata-only', action='store_true')
    mode.add_argument('--hooks-only', action='store_true')
    mode.add_argument('--native-threads', action='store_true',
                      help='test direct calls on DLL-created OS threads without hooks')
    args = parser.parse_args()
    if os.name != 'nt':
        parser.error('this runtime harness requires native Windows')
    fixture = 'NativeFixture' if args.native_threads else 'VMHookFixture'
    source = Path(__file__).with_name(fixture + '.java')
    with tempfile.TemporaryDirectory(prefix='vmhook-test-') as temporary:
        directory = Path(temporary)
        result = directory / 'result.txt'
        subprocess.run([args.javac, '-d', str(directory), str(source)], check=True)
        environment = dict(os.environ, VMHOOK_TEST_RESULT=str(result),
                           VMHOOK_TEST_MODE='metadata' if args.metadata_only else
                           'hooks' if args.hooks_only else 'native' if args.native_threads else 'runtime')
        process = subprocess.Popen([args.java, '-cp', str(directory), fixture],
                                   env=environment, stdout=subprocess.PIPE,
                                   stderr=subprocess.STDOUT, text=True)
        output = queue.Queue()
        def read_output():
            for line in process.stdout:
                output.put(line.rstrip())
        threading.Thread(target=read_output, daemon=True).start()
        try:
            deadline = time.monotonic() + 45
            ready = False
            while time.monotonic() < deadline:
                if process.poll() is not None:
                    raise RuntimeError(f'Java exited before injection: {process.returncode}')
                try:
                    line = output.get(timeout=0.25)
                except queue.Empty:
                    continue
                print(line, flush=True)
                if line == 'READY':
                    ready = True
                    break
            if not ready:
                raise RuntimeError('Java fixture did not become ready')
            subprocess.run([str(args.injector.resolve()), str(process.pid),
                            str(args.probe.resolve())], check=True, timeout=15)
            values = {}
            deadline = time.monotonic() + (300 if args.native_threads else 120)
            while time.monotonic() < deadline:
                if result.is_file():
                    values = dict(line.split('=', 1) for line in result.read_text().splitlines()
                                  if '=' in line)
                    if 'shutdown' in values or 'baseline' in values or 'native_complete' in values or any(value in ('fail', 'timeout', 'unsupported') for value in values.values()) or 'exception' in values:
                        break
                if process.poll() is not None:
                    raise RuntimeError(f'Java exited during the probe: {process.returncode}')
                time.sleep(0.1)
            for key, value in values.items():
                print(f'{key}={value}')
            required = ['metadata', 'native_refusal']
            if args.native_threads:
                required += ['native_attach', 'same_thread', 'class_initialization', 'construction',
                             'constructor_refusal', 'dispatch_semantics', 'invocation', 'collections', 'strings',
                             'root_growth', 'java_exception', 'idle_gc', 'wrong_thread',
                             'compiled_invocation', 'context_closed', 'repeated_attach',
                             'concurrent_attach', 'no_hooks', 'native_complete']
            elif not args.metadata_only:
                required += ['hook', 'dispatch', 'argument_mutation', 'cancellation', 'shutdown']
            if not args.metadata_only and not args.hooks_only and not args.native_threads:
                required += ['invocation', 'construction', 'java_exception', 'string_read', 'gc_stress']
            failed = [key for key in required if values.get(key) != 'pass']
            failed += [key for key, value in values.items() if value in ('fail', 'timeout') or key == 'exception']
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
        print(f'Runtime test failed: {error}', file=sys.stderr)
        sys.exit(1)
