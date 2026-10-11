#!/usr/bin/env python3
"""Build an isolated find_package consumer of an already installed VMHook.

The copied consumer has no checkout include path. Module sources and tools
must come from the package and their CMIs must be created in the new build.
"""

from __future__ import annotations

import argparse
import json
from pathlib import Path
import shutil
import subprocess


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--prefix", type=Path, required=True)
    parser.add_argument("--work-dir", type=Path, required=True)
    parser.add_argument("--module", action="store_true")
    parser.add_argument("--cmake", default="cmake")
    parser.add_argument("--compiler")
    parser.add_argument("--toolchain", type=Path)
    parser.add_argument("--std-source", type=Path)
    parser.add_argument("--config", default="Release")
    parser.add_argument("--flag", action="append", default=[])
    args = parser.parse_args()
    root = Path(__file__).resolve().parents[1]
    work = args.work_dir.resolve()
    source, build = work / "consumer", work / "build"
    work.mkdir(parents=True, exist_ok=True)
    if build.exists():
        raise ValueError("use an empty verification work directory")
    shutil.copytree(root / "tests/installed_consumer", source, dirs_exist_ok=True)
    command = [args.cmake, "-S", str(source), "-B", str(build), "-G", "Ninja",
               f"-DCMAKE_PREFIX_PATH={args.prefix.resolve()}",
               f"-DCMAKE_BUILD_TYPE={args.config}",
               f"-DVMHOOK_CONSUMER_MODULE={'ON' if args.module else 'OFF'}"]
    if args.compiler:
        command += [f"-DCMAKE_CXX_COMPILER={args.compiler}"]
    if args.toolchain:
        command += [f"-DCMAKE_TOOLCHAIN_FILE={args.toolchain.resolve()}"]
    if args.std_source:
        command += [f"-DVMHOOK_STD_MODULE_SOURCE={args.std_source.resolve()}"]
    if args.flag:
        command += ["-DCMAKE_CXX_FLAGS=" + " ".join(args.flag)]
    subprocess.run(command, check=True)
    subprocess.run([args.cmake, "--build", str(build), "--config", args.config], check=True)
    subprocess.run([str(Path(args.cmake).with_name("ctest")) if "/" in args.cmake else "ctest",
                    "--test-dir", str(build), "-C", args.config,
                    "--output-on-failure", "--no-tests=error"], check=True)
    manifests = list(build.glob("vmhook-module/*/build-info.json"))
    if args.module:
        if len(manifests) != 1:
            raise ValueError("installed consumer did not produce its own module manifest")
        manifest = json.loads(manifests[0].read_text(encoding="utf-8"))
        installed = args.prefix.resolve()
        for path in manifest["source_sha256"]:
            if Path(path).name.startswith("vmhook") and not Path(path).is_relative_to(installed):
                raise ValueError("module build read a VMHook source outside the installation")
    (work / "verification.json").write_text(json.dumps({
        "schema": 1, "installed_prefix": str(args.prefix.resolve()),
        "interface": "module" if args.module else "header",
        "configuration": args.config, "extra_flags": args.flag,
        "module_manifests": [str(path) for path in manifests],
        "result": "passed",
    }, indent=2) + "\n", encoding="utf-8")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
