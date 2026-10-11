#!/usr/bin/env python3
"""Create a reproducible VMHook source archive and its SHA-256 manifest.

The archive contains tracked source only, including the generated header,
module sources, build tools, profiles, examples, tests and license. GNU CMIs
and archives are deliberately built by the consuming compiler.
"""

from __future__ import annotations

import argparse
import hashlib
import json
from pathlib import Path
import re
import subprocess
import sys
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
    module = (ROOT / "modules/vmhook.ixx").read_text(encoding="utf-8")
    version = re.search(r'version_string\s*\{\s*"([0-9]+\.[0-9]+\.[0-9]+)"', module)
    if not version:
        raise ValueError("canonical module version is missing")
    version = version[1]
    package = json.loads((ROOT / "vcpkg.json").read_text(encoding="utf-8"))
    if package.get("version-semver") != version:
        raise ValueError("vcpkg and canonical module versions disagree")
    if package.get("license") != "PolyForm-Noncommercial-1.0.0":
        raise ValueError("package metadata does not identify the current license")
    subprocess.run([sys.executable, str(ROOT / "tools/make_header.py"), "--check"], check=True)
    paths = [Path(path.decode("utf-8")) for path in git("ls-files", "-z").split(b"\0") if path]
    if any(path.suffix in (".gcm", ".o", ".obj", ".a", ".dll", ".exe") for path in paths):
        raise ValueError("tracked build artifacts must not enter the source package")
    files = {path.as_posix(): (ROOT / path).read_bytes() for path in sorted(paths)}
    manifest = {
        "schema": 1, "name": "vmhook", "version": version,
        "git_revision": git("rev-parse", "HEAD").decode().strip(),
        "git_tree": git("rev-parse", "HEAD^{tree}").decode().strip(),
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
            entry.external_attr = 0o100644 << 16
            package.writestr(entry, data)
    (output / "SOURCE-MANIFEST.json").write_bytes(manifest_bytes)
    checksums = "".join(f"{hashlib.sha256(path.read_bytes()).hexdigest()}  {path.name}\n"
                        for path in (archive, output / "SOURCE-MANIFEST.json"))
    (output / "SHA256SUMS").write_text(checksums, encoding="utf-8")
    print(archive)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
