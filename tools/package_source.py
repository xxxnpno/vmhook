#!/usr/bin/env python3
"""Create a reproducible VMHook source archive and its SHA-256 manifest.

The archive contains tracked source only, including the generated header,
module sources, build tools, profiles, examples, tests and license. GNU CMIs
and archives are deliberately built by the consuming compiler.
"""

from __future__ import annotations

import argparse
import hashlib
import io
import json
from pathlib import Path
import re
import subprocess
import sys
import tarfile
import tempfile
import zipfile

ROOT = Path(__file__).resolve().parents[1]


def git(*arguments: str) -> bytes:
    return subprocess.check_output(["git", "-C", str(ROOT), *arguments])


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args()
    if git("status", "--porcelain", "--untracked-files=no").strip():
        raise ValueError("commit tracked source changes before packaging")
    revision = git("rev-parse", "HEAD").decode().strip()
    # Freeze Git blobs once. A later checkout/edit cannot silently enter an
    # archive labelled with this commit, even during a concurrent build.
    files, modes = {}, {}
    with tarfile.open(fileobj=io.BytesIO(git("-c", "tar.umask=0022", "archive", "--format=tar", revision))) as snapshot:
        for entry in snapshot:
            if entry.isdir():
                continue
            if not entry.isfile():
                raise ValueError("source packages require ordinary tracked files: " + entry.name)
            files[entry.name] = snapshot.extractfile(entry).read()
            modes[entry.name] = entry.mode
    module = files["modules/vmhook.ixx"].decode("utf-8")
    version = re.search(r'version_string\s*\{\s*"([0-9]+\.[0-9]+\.[0-9]+)"', module)
    if not version:
        raise ValueError("canonical module version is missing")
    version = version[1]
    package = json.loads(files["vcpkg.json"])
    if package.get("version-semver") != version:
        raise ValueError("vcpkg and canonical module versions disagree")
    if package.get("license") != "PolyForm-Noncommercial-1.0.0":
        raise ValueError("package metadata does not identify the current license")
    with tempfile.TemporaryDirectory(prefix="vmhook-package-check-") as temporary:
        checked = Path(temporary)
        for name in ("tools/make_header.py", "modules/vmhook.ixx", "modules/vmhook.windows.ixx",
                     "include/vmhook/vmhook.hpp", "include/vmhook/detail/windows.hpp"):
            target = checked / name
            target.parent.mkdir(parents=True, exist_ok=True)
            target.write_bytes(files[name])
        subprocess.run([sys.executable, str(checked / "tools/make_header.py"), "--check"], check=True)
    if any(Path(name).suffix in (".gcm", ".o", ".obj", ".a", ".dll", ".exe") for name in files):
        raise ValueError("tracked build artifacts must not enter the source package")
    manifest = {
        "schema": 1, "name": "vmhook", "version": version,
        "git_revision": revision,
        "git_tree": git("rev-parse", revision + "^{tree}").decode().strip(),
        "license": "PolyForm-Noncommercial-1.0.0",
        "interfaces": {"header": "C++23 / Windows x64 GNU G++ 14+",
                       "module": "C++26 reflection / Windows x64 GNU G++ 16+"},
        "module_distribution": "sources; rebuild CMIs for the consuming compiler and options",
        "runtime_evidence": "Runtime compatibility is established by separate exact-image test evidence",
        "source_sha256": {name: hashlib.sha256(data).hexdigest() for name, data in files.items()},
    }
    output = args.output.resolve()
    output.mkdir(parents=True, exist_ok=True)
    name = f"vmhook-{version}"
    manifest_bytes = (json.dumps(manifest, indent=2, sort_keys=True) + "\n").encode()
    archive = output / f"{name}-source.zip"
    with zipfile.ZipFile(archive, "w", compression=zipfile.ZIP_STORED) as package:
        for path, data in sorted({**files, "SOURCE-MANIFEST.json": manifest_bytes}.items()):
            entry = zipfile.ZipInfo(f"{name}/{path}", date_time=(1980, 1, 1, 0, 0, 0))
            entry.compress_type = zipfile.ZIP_STORED
            entry.create_system = 3
            entry.external_attr = (0o100000 | modes.get(path, 0o644)) << 16
            package.writestr(entry, data)
    (output / "SOURCE-MANIFEST.json").write_bytes(manifest_bytes)
    checksums = "".join(f"{hashlib.sha256(path.read_bytes()).hexdigest()}  {path.name}\n"
                        for path in (archive, output / "SOURCE-MANIFEST.json"))
    (output / "SHA256SUMS").write_text(checksums, encoding="utf-8")
    print(archive)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
