#!/usr/bin/env python3
"""Build VMHook's GNU C++26 module and Win32 SDK header unit in dependency order.

CMake does not support header-unit dependency scanning. This small compiler
orchestrator uses GNU's documented file mapper; it builds every module with the
same flags and publishes a mapper that consumers can use from any directory.
"""

from __future__ import annotations

import argparse
import hashlib
import json
from pathlib import Path
import re
import shutil
import subprocess
import sys

from make_header import code_mask

ROOT = Path(__file__).resolve().parents[1]


def run(command: list[str], cwd: Path, capture: bool = False, input_text: str | None = None) -> str:
    result = subprocess.run(command, cwd=cwd, check=True, text=True,
                            input=input_text, stdout=subprocess.PIPE if capture else None)
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


def prepare_standard_overlay(standard: Path, output: Path, compiler: str, flags: list[str]) -> Path | None:
    """Preserve libstdc++ ABI linkage in affected GNU Windows C++26 builds.

    The shared_ptr tag needs COMDAT linkage after module import. GNU 16's
    constexpr exception copy/move constructors also need the same gnu_inline
    annotation already used by their other constructors; otherwise the C2
    alias collides with the compiler's own static runtime at -O0. Only exact
    recognized layouts are changed, in a build-directory overlay. The variant
    error table has the same local-static import linkage defect as the tag;
    its exact ABI symbol and four diagnostic strings are preserved.
    """
    header = standard.parent / "shared_ptr_base.h"
    if not header.is_file():
        raise ValueError("the GNU std module source must be beside shared_ptr_base.h")
    original = header.read_text(encoding="utf-8")
    before = """    static const type_info&
    _S_ti() noexcept _GLIBCXX_VISIBILITY(default)
    {
      alignas(type_info) static constexpr char __tag[sizeof(type_info)] = { };
      return reinterpret_cast<const type_info&>(__tag);
    }"""
    after = """    alignas(type_info) static inline constexpr char __tag[sizeof(type_info)]
      __asm__("_ZZNSt19_Sp_make_shared_tag5_S_tiEvE5__tag") = { };
    static const type_info&
    _S_ti() noexcept _GLIBCXX_VISIBILITY(default)
    {
      return reinterpret_cast<const type_info&>(__tag);
    }"""
    overlays: dict[str, str] = {}
    if before in original:
        if original.count(before) != 1:
            raise ValueError("unrecognized libstdc++ shared_ptr tag layout; review the GNU workaround")
        overlays["bits/shared_ptr_base.h"] = original.replace(before, after)
    elif re.search(r"alignas\s*\(\s*type_info\s*\)\s*static\s+constexpr\s+char\s+__tag", original):
        raise ValueError("unrecognized libstdc++ shared_ptr tag layout; review the GNU workaround")

    configuration = run([compiler, *flags, "-dM", "-E", "-x", "c++",
                         "-include", "bits/c++config.h", "-"], output, True, "")
    macros = dict(re.findall(r"^#define\s+(\w+)\s+([^\n]+)", configuration, re.MULTILINE))
    exceptions = standard.parent / "stdexcept_except.h"
    if macros.get("__GNUC__") == "16" and exceptions.is_file():
        original_exceptions = exceptions.read_text(encoding="utf-8")
        constructors = (
            "constexpr logic_error(logic_error&& __arg) noexcept = default;",
            "constexpr logic_error(const logic_error&) noexcept = default;",
            "constexpr runtime_error(runtime_error&&) noexcept = default;",
            "constexpr runtime_error(const runtime_error&) noexcept = default;",
        )
        matches = [declaration for declaration in constructors if declaration in original_exceptions]
        if matches:
            if len(matches) != 4 or any(original_exceptions.count(declaration) != 1 for declaration in matches):
                raise ValueError("unrecognized GNU 16 constexpr exception layout; review the ABI workaround")
            for declaration in matches:
                original_exceptions = original_exceptions.replace(declaration,
                    "[[__gnu__::__gnu_inline__]] " + declaration.replace("constexpr ", "constexpr inline "))
            overlays["bits/stdexcept_except.h"] = original_exceptions
    variant_header = standard.parent.parent / "variant"
    if macros.get("__GNUC__") == "16" and variant_header.is_file():
        variant = variant_header.read_text(encoding="utf-8")
        local_table = """      [[maybe_unused]] static constexpr const char* __reasons[] = {
\t"std::get: wrong index for variant",
\t"std::get: variant is valueless",
\t"std::visit: variant is valueless",
\t"std::visit<R>: variant is valueless",
      };"""
        friend_declaration = """    friend _GLIBCXX_CONSTEXPR_BAD_VARIANT_ACCESS void
    __throw_bad_variant_access([[maybe_unused]] unsigned __n)"""
        if "static constexpr const char* __reasons[]" in variant:
            if variant.count(local_table) != 1 or variant.count(friend_declaration) != 1:
                raise ValueError("unrecognized GNU 16 variant reason table; review the module ABI workaround")
            inline_table = local_table.replace("[[maybe_unused]] static constexpr", "static inline constexpr")
            inline_table = inline_table.replace("__reasons[] =", '__reasons[]\n      __asm__("_ZZSt26__throw_bad_variant_accessjE9__reasons") =')
            variant = variant.replace(local_table + "\n", "")
            overlays["variant"] = variant.replace(friend_declaration,
                inline_table + "\n\n" + friend_declaration)
    format_header = standard.parent.parent / "format"
    if macros.get("__GNUC__") == "16" and format_header.is_file():
        formatting = format_header.read_text(encoding="utf-8")
        declarations = (
            "      static constexpr char32_t __replace = U'\\uFFFD';",
            "      static constexpr _Str_view __replace_rep = []",
        )
        if any(declaration in formatting for declaration in declarations):
            if any(formatting.count(declaration) != 1 for declaration in declarations):
                raise ValueError("unrecognized GNU 16 format replacement table; review the module workaround")
            # Both values are used only during this function call. The view
            # still refers to the same static string literals; removing its
            # local-static storage avoids duplicate imported instantiations.
            for declaration in declarations:
                formatting = formatting.replace(declaration, declaration.replace("static constexpr", "constexpr"))
            overlays["format"] = formatting
    if overlays and (macros.get("_GLIBCXX_INLINE_VERSION") != "0"
                     or "_WIN64" not in macros or "__x86_64__" not in macros):
        raise ValueError("the GNU ABI workarounds require unversioned libstdc++ on Windows x64")
    overlay = output / "stdlib-overlay"
    for name in ("bits/shared_ptr_base.h", "bits/stdexcept_except.h", "variant", "format"):
        copied = overlay / name
        if name in overlays:
            copied.parent.mkdir(parents=True, exist_ok=True)
            copied.write_text(overlays[name], encoding="utf-8", newline="\n")
        else:
            copied.unlink(missing_ok=True)
    return overlay if overlays else None


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
    standard_overlay = prepare_standard_overlay(standard, output, args.compiler, args.flag)
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
    if standard_overlay:
        flags.append("-I" + standard_overlay.as_posix())
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
    consumer_flags = [*args.flag, "-std=c++26", "-freflection", "-fmodules",
                      "-fmodule-mapper=" + mapper.as_posix()]
    if standard_overlay:
        consumer_flags.append("-I" + standard_overlay.as_posix())
    manifest = {
        "schema": 1,
        "compiler": {
            "path": args.compiler,
            "file_sha256": hashlib.sha256(Path(args.compiler).read_bytes()).hexdigest()
                if Path(args.compiler).is_file() else None,
            "version": run([args.compiler, "-dumpfullversion"], output, True),
            "target": run([args.compiler, "-dumpmachine"], output, True),
        },
        "compile_options": args.flag,
        "consumer_flags": consumer_flags,
        "source_sha256": {
            str(path): hashlib.sha256(path.read_bytes()).hexdigest()
            for path in (module, bridge, platform, standard)
        },
        "artifact_sha256": {
            path.relative_to(output).as_posix(): hashlib.sha256(path.read_bytes()).hexdigest()
            for path in sorted(set(mappings.values()) | {archive})
        },
        "stdlib_overlay": standard_overlay is not None,
        "stdlib_overlay_sha256": {
            path.relative_to(standard_overlay).as_posix(): hashlib.sha256(path.read_bytes()).hexdigest()
            for path in sorted(path for path in standard_overlay.rglob("*") if path.is_file())
        } if standard_overlay else {},
        "scope": "Compiler/configuration-specific local build; no runtime compatibility claim",
    }
    (output / "build-info.json").write_text(
        json.dumps(manifest, indent=2) + "\n", encoding="utf-8")
    print(f"Built {archive}\nConsumer flags: {subprocess.list2cmdline(consumer_flags)}")


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
