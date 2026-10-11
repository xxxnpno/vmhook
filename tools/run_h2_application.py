#!/usr/bin/env python3
"""Inject the owned VMHook DLL into a pinned H2 TCP server and run real JDBC work.

The launcher starts only the ordinary H2 application. All connections, SQL,
transactions, validation and measurements execute in the injected C++ worker.
"""
from __future__ import annotations

import argparse
from collections import Counter
import hashlib
import json
import os
from pathlib import Path
import queue
import struct
import subprocess
import sys
import tempfile
import threading
import time
from urllib.request import urlopen
from zipfile import ZipFile

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / 'tests/runtime'))
from runtime_evidence import read_results, runtime_evidence

ROOT = Path(__file__).resolve().parents[1]
PIN = ROOT / 'tests/dependencies/h2-2.2.224.json'
REQUIRED = ('h2_context', 'h2_native_worker', 'h2_tcp_application', 'h2_version',
            'h2_java_null_owner', 'h2_prepared_inserts', 'h2_unicode_null_arrays',
            'h2_rollback_isolation', 'h2_sql_exceptions', 'h2_committed_transfers',
            'h2_live_gc_roots', 'h2_closed_resources', 'h2_complete')
METRICS = ('context_first_ns', 'two_connections_first_ns', 'transactions',
           'transferred_amount', 'workload_wall_ns', 'transaction_p50_ns',
           'transaction_p99_ns', 'transaction_min_ns', 'transaction_max_ns',
           'gc_collection_delta', 'heap_used_before_bytes', 'heap_used_after_bytes')


def dependency(pin: dict, supplied: Path | None, cache: Path) -> tuple[Path, dict]:
    """Resolve the exact artifact; arbitrary versions are never silently accepted."""
    jar = supplied.resolve() if supplied else cache.resolve() / 'h2-2.2.224.jar'
    downloaded = False
    if not supplied and not jar.is_file():
        cache.mkdir(parents=True, exist_ok=True)
        with urlopen(pin['publisher_sha256_url'], timeout=30) as response:
            publisher = response.read(1024).decode('ascii').strip()
        if publisher != pin['sha256']:
            raise RuntimeError('H2 publisher checksum differs from the pinned artifact')
        with urlopen(pin['jar_url'], timeout=60) as response:
            data = response.read(pin['bytes'] + 1)
        if len(data) != pin['bytes'] or hashlib.sha256(data).hexdigest() != pin['sha256']:
            raise RuntimeError('Downloaded H2 JAR does not match its pinned size/SHA256')
        # A runner never leaves a partial JAR in its reusable cache.
        with tempfile.NamedTemporaryFile(dir=cache, prefix='h2-', suffix='.tmp', delete=False) as temporary:
            temporary.write(data)
            temporary_path = Path(temporary.name)
        try:
            temporary_path.replace(jar)
        finally:
            temporary_path.unlink(missing_ok=True)
        downloaded = True
    if jar.stat().st_size != pin['bytes']:
        raise RuntimeError('Supplied/cached H2 JAR does not match pinned size/SHA256')
    raw = jar.read_bytes()
    if hashlib.sha256(raw).hexdigest() != pin['sha256']:
        raise RuntimeError('Supplied/cached H2 JAR does not match pinned size/SHA256')
    base_versions: Counter[int] = Counter()
    variants: dict[str, int] = {}
    with ZipFile(jar) as archive:
        manifest = archive.read('META-INF/MANIFEST.MF').decode('utf-8')
        for name in archive.namelist():
            if not name.endswith('.class'):
                continue
            header = archive.read(name)[:8]
            if header[:4] != b'\xca\xfe\xba\xbe':
                raise RuntimeError('H2 contains an invalid Java class header')
            major = struct.unpack('>H', header[6:8])[0]
            if name.startswith('META-INF/versions/'):
                feature = int(name.split('/')[2])
                if feature < 9 or major > feature + 44:
                    raise RuntimeError('H2 MultiRelease class exceeds its feature directory')
                variants[name] = major
            else:
                base_versions[major] += 1
        if dict(base_versions) != {pin['base_class_major']: pin['base_class_count']}:
            raise RuntimeError('H2 base classes do not match the Java8 class-version audit')
        if 'Multi-Release: true' not in manifest or 'Implementation-Version: 2.2.224' not in manifest:
            raise RuntimeError('H2 manifest differs from the pinned MultiRelease version')
    return jar, {'coordinates': pin['maven_coordinates'], 'sha256': pin['sha256'],
                 'publisher_sha256_url': pin['publisher_sha256_url'],
                 'license_expression': pin['license_expression'], 'license_url': pin['license_url'],
                 'downloaded': downloaded, 'base_class_versions': dict(base_versions),
                 'multi_release_classes': variants, 'manifest': manifest}


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--java', default='java')
    parser.add_argument('--javac', default='javac')
    parser.add_argument('--injector', type=Path, required=True)
    parser.add_argument('--probe', type=Path, required=True)
    parser.add_argument('--jar', type=Path, help='offline copy of the exact pinned H2 JAR')
    parser.add_argument('--cache', type=Path, default=Path(tempfile.gettempdir()) / 'vmhook-h2-dependencies',
                        help='download cache outside the repository; used only without --jar')
    parser.add_argument('--transactions', type=int, default=32)
    parser.add_argument('--timeout', type=float, default=300)
    parser.add_argument('--wine', type=Path, help='supplemental Windows-binary validation on a non-Windows host')
    parser.add_argument('--evidence', type=Path, required=True)
    args = parser.parse_args()
    if not 1 <= args.transactions <= 10000 or args.timeout <= 0:
        parser.error('transactions must be 1..10000 and timeout must be positive')
    if os.name != 'nt' and not args.wine:
        parser.error('use native Windows or explicitly supply --wine for a supplemental check')
    prefix = [str(args.wine.resolve())] if args.wine else []

    def windows_path(path: Path) -> str:
        return 'Z:' + str(path.resolve()).replace('/', '\\') if args.wine else str(path.resolve())

    with tempfile.TemporaryDirectory(prefix='vmhook-h2-application-') as temporary:
        directory = Path(temporary)
        destination = directory / 'result.txt'
        launcher = ROOT / 'tests/runtime/H2Application.java'
        evidence = runtime_evidence(args.evidence, 'H2Application', args.wine)
        evidence.report['workload'] = {'application': 'H2 TCP server', 'transactions': args.transactions,
            'transport': 'loopback TCP; two distinct JDBC sessions',
            'sql_implementation': 'injected C++ VMHook worker; public H2/JDBC Java methods',
            'timing_scope': 'complete committed transfer plus audit insert and independent-session result checks; excludes requested GC between samples',
            'comparison': 'no alternate bridge/baseline; no universal overhead inference'}
        process = None
        reader = None
        passed = False
        failure = None
        lines: queue.Queue[str] = queue.Queue()

        def capture(line: str) -> None:
            evidence.report['java_output'].append(line)
            print(line, flush=True)

        def read_output() -> None:
            for line in process.stdout:
                lines.put(line.rstrip())

        try:
            evidence.add_input('dependency_pin', PIN)
            evidence.add_input('application_launcher_source', launcher)
            evidence.add_input('native_probe_source', ROOT / 'tests/runtime/h2_application_probe.cpp')
            evidence.add_input('application_runner_source', Path(__file__))
            pin = json.loads(PIN.read_text())
            jar, audit = dependency(pin, args.jar, args.cache)
            evidence.report['dependency'] = audit
            evidence.add_input('h2_application_jar', jar)
            evidence.add_runtime(args.java, args.javac, args.injector, args.probe)
            compilation = subprocess.run(prefix + [args.javac, '-cp', windows_path(jar), '-d', windows_path(directory), windows_path(launcher)],
                stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True, timeout=90)
            evidence.report['java_compiler_output'] = compilation.stdout
            if compilation.stdout:
                print(compilation.stdout, end='', flush=True)
            compilation.check_returncode()
            evidence.add_classes(directory)
            classpath = windows_path(directory) + ';' + windows_path(jar)
            java_arguments = ['-cp', classpath, 'H2Application']
            evidence.report['java_arguments'] = java_arguments
            environment = dict(os.environ, VMHOOK_H2_RESULT=windows_path(destination),
                               VMHOOK_H2_TRANSACTIONS=str(args.transactions))
            process = subprocess.Popen(prefix + [args.java] + java_arguments,
                stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True, env=environment, cwd=directory)
            reader = threading.Thread(target=read_output, daemon=True)
            reader.start()
            deadline = time.monotonic() + 90
            pid = None
            port = None
            while time.monotonic() < deadline:
                try:
                    line = lines.get(timeout=0.25)
                except queue.Empty:
                    if process.poll() is not None:
                        raise RuntimeError('H2 application exited before readiness')
                    continue
                capture(line)
                if line.startswith('PID='):
                    pid = line[4:]
                if line.startswith('PORT='):
                    port = line[5:]
                if line == 'READY' and pid and pid.isdecimal() and port and port.isdecimal():
                    break
            else:
                raise RuntimeError('H2 TCP application readiness timeout')
            evidence.report['windows_pid'] = int(pid)
            evidence.report['h2_tcp_port'] = int(port)
            subprocess.run(prefix + [str(args.injector.resolve()), pid, windows_path(args.probe)], check=True, timeout=30)
            values = {}
            deadline = time.monotonic() + args.timeout
            while time.monotonic() < deadline:
                while not lines.empty():
                    capture(lines.get_nowait())
                values = read_results(destination)
                if 'h2_complete' in values or 'exception' in values or 'fail' in values.values():
                    break
                if process.poll() is not None:
                    raise RuntimeError('H2 application exited during the native workload')
                time.sleep(0.1)
            for key, value in values.items():
                print(f'{key}={value}', flush=True)
            missing = [name for name in REQUIRED if values.get(name) != 'pass']
            if missing or 'exception' in values:
                raise RuntimeError('H2 application checks failed: ' + ', '.join(missing)
                    + ('; ' + values['exception'] if 'exception' in values else ''))
            measurements = {name: int(values[name]) for name in METRICS}
            measurements['transaction_samples_ns'] = [
                int(values[f'transaction_sample_{index}_ns']) for index in range(args.transactions)]
            if measurements['transactions'] != args.transactions or measurements['gc_collection_delta'] <= 0:
                raise RuntimeError('H2 workload measurements do not match the validated run')
            evidence.report['measurements'] = measurements
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
            evidence.finish(passed, failure, directory)


if __name__ == '__main__':
    try:
        sys.exit(main())
    except (OSError, RuntimeError, subprocess.SubprocessError, ValueError) as error:
        print(f'H2 application validation failed: {error}', file=sys.stderr)
        sys.exit(1)
