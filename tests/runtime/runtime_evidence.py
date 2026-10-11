"""Copied provenance and results for independent owned-JVM runtime fixtures."""

from __future__ import annotations

from datetime import datetime, timezone
import hashlib
import json
import os
from pathlib import Path
import platform
import shutil
import time


def executable_path(value: str) -> Path:
    """Resolve PATH defaults without guessing which launcher subprocess uses."""
    return Path(shutil.which(value) or value).resolve()


def file_record(path: Path) -> dict[str, str]:
    with path.open('rb') as stream:
        digest = hashlib.sha256()
        for chunk in iter(lambda: stream.read(1024 * 1024), b''):
            digest.update(chunk)
    return {'path': str(path.resolve()), 'sha256': digest.hexdigest()}


def read_results(path: Path) -> dict[str, str]:
    if not path.is_file():
        return {}
    # The native writer may flush a key and value separately. Only LF commits
    # a record; read bytes so an isolated CR cannot look like a complete CRLF.
    contents = path.read_bytes()
    committed = contents[:contents.rfind(b'\n') + 1].decode('utf-8', errors='replace')
    return dict(line.split('=', 1) for line in committed.splitlines() if '=' in line)


class runtime_evidence:
    """Write a result even when compilation, injection or the owned JVM fails."""

    def __init__(self, destination: Path | None, fixture: str, emulator: Path | None = None):
        self.destination = destination
        self.started = time.monotonic()
        self.report = {
            'schema': 1,
            'fixture': fixture,
            'started_utc': datetime.now(timezone.utc).isoformat(),
            'passed': False,
            'native_windows': os.name == 'nt' and emulator is None,
            'execution': 'supplemental Wine check' if emulator else 'native Windows',
            'host': {'platform': platform.platform(), 'machine': platform.machine(),
                     'logical_cpu_count': os.cpu_count()},
            'input_files': {},
            'compiled_classes': [],
            'java_arguments': ['-cp', '<temporary compiled fixture>', fixture],
            'java_output': [],
            'checks': {},
            'fatal_reports': [],
        }
        if emulator:
            self.report['emulator'] = str(emulator.resolve())

    def add_input(self, name: str, path: Path) -> None:
        self.report['input_files'][name] = file_record(path)

    def add_runtime(self, java: str, javac: str, injector: Path, probe: Path) -> None:
        launcher = executable_path(java)
        self.add_input('java_launcher', launcher)
        self.add_input('java_compiler', executable_path(javac))
        self.add_input('injector', injector)
        self.add_input('probe', probe)
        home = launcher.parent.parent
        if (home / 'release').is_file():
            self.add_input('java_release', home / 'release')
            self.report['java_release'] = (home / 'release').read_text(encoding='utf-8', errors='replace')
        candidates = (launcher.parent / 'server/jvm.dll', home / 'jre/bin/server/jvm.dll')
        image = next((path for path in candidates if path.is_file()), None)
        if image:
            self.add_input('jvm_image', image)

    def add_classes(self, directory: Path) -> None:
        self.report['compiled_classes'] = [
            {'name': path.relative_to(directory).as_posix(), 'sha256': file_record(path)['sha256']}
            for path in sorted(directory.rglob('*.class'))
        ]

    def verify_probe(self, probe: Path) -> None:
        if file_record(probe)['sha256'] != self.report['input_files']['probe']['sha256']:
            raise RuntimeError('probe DLL changed during verification')

    def finish(self, passed: bool, error: Exception | None, directory: Path) -> None:
        self.report['passed'] = passed
        self.report['seconds'] = round(time.monotonic() - self.started, 3)
        if error:
            self.report['error'] = f'{type(error).__name__}: {error}'
        for path in sorted(directory.glob('hs_err_pid*.log')):
            record = file_record(path)
            record['name'] = path.name
            record['text'] = path.read_text(encoding='utf-8', errors='replace')
            self.report['fatal_reports'].append(record)
        if self.destination:
            self.destination.parent.mkdir(parents=True, exist_ok=True)
            temporary = self.destination.with_name(self.destination.name + '.tmp')
            temporary.write_text(json.dumps(self.report, indent=2) + '\n', encoding='utf-8')
            temporary.replace(self.destination)
