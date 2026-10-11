#!/usr/bin/env python3
"""Inject the probe into a JVM launched without VM tuning/agent arguments.

Native Windows x64 is the release proof. --wine permits an explicitly labelled
supplemental Windows-binary check. Invocation, construction and GC stress must
pass; unsupported operations fail. The metadata/hooks modes run separate
baselines, and the harness only injects the Java subprocess it creates itself.
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
    parser.add_argument('--wine', type=Path, help='supplemental Wine runner on a non-Windows host')
    parser.add_argument('--evidence', type=Path, help='write copied hashes, checks and failure diagnostics as JSON')
    mode = parser.add_mutually_exclusive_group()
    mode.add_argument('--metadata-only', action='store_true')
    mode.add_argument('--hooks-only', action='store_true')
    mode.add_argument('--native-threads', action='store_true',
                      help='test direct calls on DLL-created OS threads without hooks')
    args = parser.parse_args()
    if os.name != 'nt' and not args.wine:
        parser.error('use native Windows or explicitly supply --wine for a supplemental check')
    prefix = [str(args.wine.resolve())] if args.wine else []

    def windows_path(path: Path) -> str:
        if args.wine:
            return 'Z:' + str(path.resolve()).replace('/', '\\')
        return str(path.resolve())

    fixture = 'NativeFixture' if args.native_threads else 'VMHookFixture'
    source = Path(__file__).with_name(fixture + '.java')
    probe_source = Path(__file__).with_name('native_probe.cpp' if args.native_threads else 'probe.cpp')
    selected_mode = ('metadata' if args.metadata_only else 'hooks' if args.hooks_only
                     else 'native' if args.native_threads else 'runtime')
    with tempfile.TemporaryDirectory(prefix='vmhook-test-') as temporary:
        directory = Path(temporary)
        result = directory / 'result.txt'
        evidence = runtime_evidence(args.evidence, fixture, args.wine)
        evidence.report['mode'] = selected_mode
        evidence.report['source_revision'] = os.environ.get('GITHUB_SHA')
        output: queue.Queue[str] = queue.Queue()
        process = None
        reader = None
        passed = False
        failure = None

        def capture(line: str) -> None:
            evidence.report['java_output'].append(line)
            print(line, flush=True)

        def read_output() -> None:
            for line in process.stdout:
                output.put(line.rstrip())

        try:
            evidence.add_input('fixture_source', source)
            evidence.add_input('probe_source', probe_source)
            evidence.add_runtime(args.java, args.javac, args.injector, args.probe)
            compilation = subprocess.run(prefix + [args.javac, '-d', windows_path(directory), windows_path(source)],
                                         stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True, timeout=90)
            evidence.report['java_compiler_output'] = compilation.stdout
            if compilation.stdout:
                print(compilation.stdout, end='', flush=True)
            compilation.check_returncode()
            evidence.add_classes(directory)
            environment = dict(os.environ, VMHOOK_TEST_RESULT=windows_path(result),
                               VMHOOK_TEST_MODE=selected_mode)
            process = subprocess.Popen(prefix + [args.java, '-cp', windows_path(directory), fixture],
                                       env=environment, stdout=subprocess.PIPE,
                                       stderr=subprocess.STDOUT, text=True, cwd=directory)
            reader = threading.Thread(target=read_output, daemon=True)
            reader.start()
            deadline = time.monotonic() + 60
            ready = False
            pid = None
            while time.monotonic() < deadline:
                if process.poll() is not None:
                    raise RuntimeError(f'Java exited before injection: {process.returncode}')
                try:
                    line = output.get(timeout=0.25)
                except queue.Empty:
                    continue
                capture(line)
                if line.startswith('PID='):
                    pid = line[4:]
                if line == 'READY' and pid and pid.isdecimal():
                    ready = True
                    break
            if not ready:
                raise RuntimeError('Java fixture did not become ready with its Windows PID')
            if not args.wine and int(pid) != process.pid:
                raise RuntimeError('Java fixture PID differs from the owned Windows subprocess')
            evidence.report['windows_pid'] = int(pid)
            subprocess.run(prefix + [str(args.injector.resolve()), pid,
                                     windows_path(args.probe)], check=True, timeout=30)
            values = {}
            deadline = time.monotonic() + (300 if args.native_threads else 120)
            while time.monotonic() < deadline:
                while not output.empty():
                    capture(output.get_nowait())
                values = read_results(result)
                if (any(values.get(key) == 'pass' for key in ('shutdown', 'baseline', 'native_complete'))
                    or any(value in ('fail', 'timeout', 'unsupported') for value in values.values())
                    or 'exception' in values):
                    break
                if process.poll() is not None:
                    raise RuntimeError(f'Java exited during the probe: {process.returncode}')
                time.sleep(0.1)
            for key, value in values.items():
                print(f'{key}={value}', flush=True)
            required = ['metadata', 'native_refusal']
            if args.native_threads:
                required += ['native_attach', 'same_thread', 'class_initialization', 'construction',
                             'constructor_refusal', 'dispatch_semantics', 'invocation', 'collections',
                             'object_contract', 'field_memory', 'heap_iteration', 'monitor_locks', 'strings',
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
        print(f'Runtime test failed: {error}', file=sys.stderr)
        sys.exit(1)
