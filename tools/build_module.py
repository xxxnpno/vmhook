#!/usr/bin/env python3
"""Build VMHook's GNU C++26 module and Win32 SDK header unit in dependency order.

CMake does not support header-unit dependency scanning. This small compiler
orchestrator uses GNU's documented file mapper; it builds every module with the
same flags and publishes a mapper that consumers can use from any directory.
"""

from __future__ import annotations

import argparse
import json
from pathlib import Path
import re
import shutil
import subprocess
import sys

from make_header import code_mask

ROOT = Path(__file__).resolve().parents[1]


def run(command: list[str], cwd: Path, capture: bool = False) -> str:
    result = subprocess.run(command, cwd=cwd, check=True, text=True,
                            stdout=subprocess.PIPE if capture else None)
    return result.stdout.strip() if capture else ""


def discover_std(compiler: str, flags: list[str], cwd: Path) -> Path:
    manifest = Path(run([compiler, *flags, "-print-file-name=libstdc++.modules.json"], cwd, True))
    if not manifest.is_file():
        raise ValueError("G++ must ship libstdc++.modules.json and a usable std module source")
    data = json.loads(manifest.read_text(encoding="utf-8"))
    if data.get("version") != 1:
        raise ValueError("unsupported libstdc++ module manifest version")
    matches = [entry for entry in data.get("modules", []) if entry.get("logical-name") == "std"]
    if len(matches) != 1:
        raise ValueError("libstdc++ manifest must describe exactly one std module")
    source = (manifest.parent / matches[0]["source-path"]).resolve()
    if not source.is_file() or not re.search(r"\bexport\s+module\s+std\s*;", code_mask(source.read_text(encoding="utf-8"))):
        raise ValueError("libstdc++ std module source is missing or empty")
    return source


def build(args: argparse.Namespace) -> None:
    output = args.build_dir.resolve()
    output.mkdir(parents=True, exist_ok=True)
    module = (args.source_dir / "modules/vmhook.ixx").resolve()
    bridge = (args.source_dir / "modules/vmhook.windows.ixx").resolve()
    platform = (args.source_dir / "include/vmhook/detail/windows.hpp").resolve()
    # GCC's file mapper splits only the key at whitespace and takes the rest
    # of the line as the filename. It does not parse libCody protocol quotes.
    # Keep the header-unit key relative and free of spaces even when either
    # the checkout or build directory contains spaces.
    platform_import = Path("sdk/vmhook/detail/windows.hpp")
    copied_platform = output / platform_import
    copied_platform.parent.mkdir(parents=True, exist_ok=True)
    shutil.copyfile(platform, copied_platform)
    for path in (module, bridge):
        if re.search(r"^\s*#", code_mask(path.read_text(encoding="utf-8")), re.MULTILINE):
            raise ValueError(f"{path.name} must contain no preprocessor directives")
    standard = args.std_source.resolve() if args.std_source else discover_std(args.compiler, args.flag, output)
    cache = output / "gcm"
    cache.mkdir(exist_ok=True)
    mapper = output / "module.mapper"
    mappings = {
        "std": cache / "std.gcm",
        platform_import.as_posix(): cache / "windows-sdk.gcm",
        "./" + platform_import.as_posix(): cache / "windows-sdk.gcm",
        "vmhook.windows": cache / "vmhook.windows.gcm",
        "vmhook": cache / "vmhook.gcm",
    }
    mapper.write_text("".join(f"{name} {path.as_posix()}\n"
                              for name, path in mappings.items()), encoding="utf-8", newline="\n")
    flags = [*args.flag, "-std=c++26", "-freflection", "-fmodules",
             f"-fmodule-mapper={mapper.as_posix()}", "-Isdk"]
    units = ((standard, "std.o", "c++"), (platform_import, None, "c++-header"),
             (bridge, "vmhook.windows.o", "c++"), (module, "vmhook.o", "c++"))
    for source, object_name, language in units:
        print(f"Building {source.name}", flush=True)
        command = [args.compiler, *flags, "-x", language, "-c", source.as_posix()]
        if object_name:
            command += ["-o", (output / object_name).as_posix()]
        run(command, output)
    # Do not retain stale members when source units or compiler flags change.
    archive = output / "libvmhook.a"
    archive.unlink(missing_ok=True)
    run([args.ar, "rcs", archive.as_posix(), *(str(output / name) for name in
         ("std.o", "vmhook.windows.o", "vmhook.o"))], output)
    for artifact in (*mappings.values(), archive):
        if not artifact.is_file():
            raise ValueError(f"compiler did not produce {artifact}")
    print(f"Built {archive}\nConsumer flags: -std=c++26 -freflection -fmodules -fmodule-mapper={mapper.as_posix()}")


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--compiler", default="g++")
    parser.add_argument("--ar", default="ar")
    parser.add_argument("--source-dir", type=Path, default=ROOT)
    parser.add_argument("--build-dir", type=Path, default=ROOT / "build/module")
    parser.add_argument("--std-source", type=Path, help="override the compiler's std module source location")
    parser.add_argument("--flag", action="append", default=[], help="extra compiler flag; use --flag=-O2")
    args = parser.parse_args()
    try:
        build(args)
    except (OSError, ValueError, subprocess.CalledProcessError) as error:
        print(f"Module build failed: {error}", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
