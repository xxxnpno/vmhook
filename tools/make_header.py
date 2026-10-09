#!/usr/bin/env python3
"""Generate the standalone C++23 header from VMHook's C++26 module.

The semantic differences are the reflection helpers, annotation storage, and
deleted-function diagnostic strings. Remaining reflection/module syntax fails generation;
compiling the result as C++23 checks other language constructs. No C++ compiler,
Windows SDK, or third-party Python package is needed for generation.
"""

from __future__ import annotations

import argparse
from pathlib import Path
import re
import sys

ROOT = Path(__file__).resolve().parents[1]
STANDARD_HEADERS = (
    "algorithm", "array", "atomic", "bit", "chrono", "concepts", "condition_variable", "cstddef",
    "cstdint", "cstdlib", "cstring", "exception", "expected", "filesystem", "format", "fstream",
    "functional", "future", "initializer_list", "iostream", "limits", "memory", "mutex", "new", "optional",
    "map", "set", "source_location", "span", "stdexcept", "string", "string_view", "thread", "tuple",
    "typeindex", "typeinfo", "type_traits", "unordered_map", "unordered_set",
    "utility", "variant", "vector",
)


# Runtime cache owners have one definition in the C++26 module object.
MODULE_CACHE_FUNCTIONS = (
    "array_layout",
    "c2i_state",
    "call_handle_pool",
    "class_initialized",
    "compact_klass_from_mark",
    "compact_object_header_mode",
    "compressed_klass_mode",
    "decode_klass_pointer",
    "decode_oop_pointer",
    "derive_call_stub_entry",
    "detect_adapter_offset_from_method",
    "emit_log_line",
    "encode_klass_pointer",
    "encode_oop_pointer",
    "find_any_java_thread",
    "find_call_stub_entry",
    "find_class",
    "find_java_thread_by_os_thread_id",
    "find_jvm_module",
    "gc_epoch",
    "get_c2i_entry_from_adapter",
    "get_jvm_module",
    "get_vm_structs",
    "get_vm_types",
    "java_call_layout",
    "java_object_alignment",
    "klass_from_oop",
    "klass_to_class_loader_oop",
    "make_java_object",
    "read_gc_flags",
    "read_klass_from_header_buffer",
    "rt",
    "root_reuse_callback",
    "current_context_lifetime",
    "resolve_code_cache_bounds",
    "resolve_gc_epoch_sources",
    "resolve_jvm_flag_table",
    "safe_klass_methods",
    "validate_adapter_handler_entry",
    "vm_capabilities",
)


def code_mask(source: str) -> str:
    """Blank comments and literals, preserving offsets and newlines."""
    out = list(source)
    index = 0
    while index < len(source):
        end = index
        if source.startswith("//", index):
            end = source.find("\n", index)
            if end < 0:
                end = len(source)
        elif source.startswith("/*", index):
            end = source.find("*/", index + 2)
            if end < 0:
                raise ValueError("unterminated block comment")
            end += 2
        elif source.startswith('R"', index):
            opening = source.find("(", index + 2)
            if opening < 0:
                raise ValueError("unterminated raw string delimiter")
            closing = ")" + source[index + 2:opening] + '"'
            end = source.find(closing, opening + 1)
            if end < 0:
                raise ValueError("unterminated raw string")
            end += len(closing)
        elif source[index] in "\"'":
            # A separator in a numeric literal is not the start of a character.
            if (source[index] == "'" and index > 0 and index + 1 < len(source)
                    and source[index - 1].isalnum() and source[index + 1].isalnum()):
                index += 1
                continue
            quote = source[index]
            end = index + 1
            while end < len(source):
                if source[end] == "\\":
                    end += 2
                elif source[end] == quote:
                    end += 1
                    break
                else:
                    end += 1
            else:
                raise ValueError("unterminated literal")
        if end > index:
            for position in range(index, end):
                if out[position] != "\n":
                    out[position] = " "
            index = end
        else:
            index += 1
    return "".join(out)


def replace_function(source: str, signature: str, replacement: str) -> str:
    masked = code_mask(source)
    matches = list(re.finditer(signature, masked, re.MULTILINE))
    if len(matches) != 1:
        raise ValueError(f"expected one reflection helper matching {signature!r}, found {len(matches)}")
    match = matches[0]
    opening = masked.find("{", match.end())
    if opening < 0 or ";" in masked[match.end():opening]:
        raise ValueError("reflection helper is no longer a function definition")
    depth = 1
    closing = opening + 1
    while closing < len(masked) and depth:
        depth += (masked[closing] == "{") - (masked[closing] == "}")
        closing += 1
    if depth:
        raise ValueError("unterminated reflection helper")
    return source[:match.start()] + replacement.rstrip() + source[closing:]


TYPE_NAME = """        template<typename type>
        inline auto type_name() noexcept -> std::string
        {
            const std::string_view signature{ std::source_location::current().function_name() };
            constexpr std::string_view prefix{ "type = " };
            const auto marker{ signature.find(prefix) };
            if (marker == std::string_view::npos) { return {}; }
            const auto first{ marker + prefix.size() };
            const auto last{ signature.find_first_of(";]", first) };
            return std::string{ signature.substr(first, last - first) };
        }"""

ANNOTATED_CLASS = """        template<typename wrapper_type>
        consteval auto annotated_class_name() noexcept -> std::string_view
        {
            // C++23 wrappers register their Java class name explicitly.
            return {};
        }"""

JAVA_CLASS = """    struct java_class
    {
        std::string_view name{};

        consteval explicit java_class(const std::string_view class_name) noexcept
            : name{ class_name }
        {
        }
    }"""

ENUM_NAME = """        // C++23 cannot enumerate an enum. Diagnostic names cover values 0..127.
        template<auto value>
        [[nodiscard]] consteval auto enumerator_name_of() noexcept -> std::string_view
        {
            const std::string_view signature{ std::source_location::current().function_name() };
            constexpr std::string_view prefix{ "value = " };
            const auto marker{ signature.find(prefix) };
            if (marker == std::string_view::npos) { return {}; }
            const auto first{ marker + prefix.size() };
            const auto last{ signature.find_first_of(";]", first) };
            const auto spelling{ signature.substr(first, last - first) };
            if (spelling.empty() || spelling.front() == '(') { return {}; }
            const auto scope{ spelling.rfind("::") };
            return scope == std::string_view::npos ? spelling : spelling.substr(scope + 2);
        }

        template<typename enum_type, std::size_t... candidates>
        consteval auto enum_names(std::index_sequence<candidates...>) noexcept
            -> std::array<std::string_view, sizeof...(candidates)>
        {
            return { enumerator_name_of<static_cast<enum_type>(candidates)>()... };
        }

        template<typename enum_type>
            requires std::is_enum_v<enum_type>
        [[nodiscard]] inline auto enum_name(const enum_type value) noexcept -> std::string_view
        {
            static constexpr auto names{ enum_names<enum_type>(std::make_index_sequence<128>{}) };
            const auto index{ static_cast<std::make_unsigned_t<std::underlying_type_t<enum_type>>>(value) };
            return index < names.size() ? names[index] : std::string_view{};
        }"""


def strip_module_declarations(source: str, name: str) -> str:
    masked = code_mask(source)
    module = list(re.finditer(rf"^\s*export\s+module\s+{re.escape(name)}\s*;", masked, re.MULTILINE))
    if len(module) != 1:
        raise ValueError(f"expected one export module {name}; declaration")
    # The bridge's global fragment and SDK includes precede its declaration.
    source = source[:module[0].start()] + source[module[0].end():]
    masked = code_mask(source)
    removals = list(re.finditer(r"^\s*(?:module\s*;|(?:export\s+)?import\s+[\w.]+\s*;)\s*", masked, re.MULTILINE))
    for match in reversed(removals):
        source = source[:match.start()] + source[match.end():]
    masked = code_mask(source)
    for match in reversed(list(re.finditer(r"\bexport\s+(?=namespace\b)", masked))):
        source = source[:match.start()] + source[match.end():]
    return source


def generate(module_path: Path, platform_header_path: Path) -> str:
    module = module_path.read_text(encoding="utf-8")
    masked = code_mask(module)
    if re.search(r"^\s*#", masked, re.MULTILINE):
        raise ValueError("vmhook.ixx must contain no preprocessor directives")
    for name in ("std", "vmhook.windows"):
        if len(re.findall(rf"\bimport\s+{re.escape(name)}\s*;", masked)) != 1:
            raise ValueError(f"vmhook.ixx must import {name} exactly once")
    module = strip_module_declarations(module, "vmhook")
    module = replace_function(module,
        r"^[ \t]*template\s*<\s*typename\s+type\s*>\s*inline\s+auto\s+type_name\s*\(\s*\)\s*noexcept\s*->\s*std::string",
        TYPE_NAME)
    module = replace_function(module,
        r"^[ \t]*template\s*<\s*typename\s+wrapper_type\s*>\s*consteval\s+auto\s+annotated_class_name\s*\(\s*\)\s*noexcept\s*->\s*std::string_view",
        ANNOTATED_CLASS)
    module = replace_function(module,
        r"^[ \t]*template\s*<\s*typename\s+enum_type\s*>\s*requires\s+std::is_enum_v\s*<\s*enum_type\s*>\s*\[\[nodiscard\]\]\s*inline\s+auto\s+enum_name\s*\(\s*const\s+enum_type\s+value\s*\)\s*noexcept\s*->\s*std::string_view",
        ENUM_NAME)
    module = replace_function(module, r"^[ \t]*struct\s+java_class\b", JAVA_CLASS)
    # ABI layout assertions use reflection in the module. GNU's built-in
    # offset calculation provides the same compile-time check in C++23.
    module = re.sub(
        r"std::meta::offset_of\(\^\^([A-Za-z_]\w*)::([A-Za-z_]\w*)\)\.bytes",
        r"__builtin_offsetof(\1, \2)", module)
    # GCC's Windows module importer can emit duplicate dynamic local statics
    # for exported inline functions. These cache owners are compiled once in
    # the module; a standalone header needs inline definitions instead.
    for name in MODULE_CACHE_FUNCTIONS:
        matches = list(re.finditer(rf"\bauto\s+{name}\s*\(", code_mask(module)))
        if not matches:
            raise ValueError(f"missing module-owned cache function {name}")
        for match in reversed(matches):
            module = module[:match.start()] + "inline " + module[match.start():]
    # Exported TLS storage belongs to the compiled module. COFF importers must
    # refer to that storage instead of emitting another emulated TLS template.
    # Standalone headers retain one inline variable across translation units.
    for match in reversed(list(re.finditer(r"(?m)^[ \t]*thread_local\b", code_mask(module)))):
        position = match.end() - len("thread_local")
        module = module[:position] + "inline " + module[position:]
    # Deleted-function diagnostic strings are C++26; preserve each as a comment.
    module = re.sub(r'=\s*delete\s*\(\s*((?:"(?:\\.|[^"\\])*"\s*)+)\)',
                    lambda match: "= delete /* " + match[1].strip().replace("*/", "* /") + " */", module)
    masked = code_mask(module)
    unsupported = (
        r"(?m)^\s*(?:export\s+)?(?:import|module)\s+[^;\n]+;",
        r"\bexport\s+(?:namespace|template|class|struct|enum|using|auto)\b",
        r"std::meta\b", r"\^\^", r"\[:",
        r"\btemplate\s+for\b", r"std::define_static_\w+", r"\[\[\s*=",
        r"=\s*delete\s*\(",
    )
    for pattern in unsupported:
        match = re.search(pattern, masked)
        if match:
            line = module.count("\n", 0, match.start()) + 1
            raise ValueError(f"untranslated C++26/module syntax on body line {line}: {match[0]!r}")
    bridge = platform_header_path.read_text(encoding="utf-8")
    bridge = re.sub(r"^#pragma once\s*$", "", bridge, flags=re.MULTILINE)
    prefix = """// Generated by tools/make_header.py from modules/vmhook.ixx. Do not edit.
// Standalone C++23 interface, Windows x64, GNU G++ only.
#pragma once

#if !defined(_WIN32) || !defined(_WIN64) || !defined(__x86_64__)
#error "VMHook requires Windows x64."
#endif
#if !defined(__GNUC__) || defined(__clang__) || defined(_MSC_VER)
#error "VMHook requires GNU G++."
#endif
#if __cplusplus < 202100L
#error "VMHook requires C++23 or newer."
#endif

"""
    includes = "".join(f"#include <{header}>\n" for header in STANDARD_HEADERS)
    return prefix + includes + "\n" + bridge.strip() + "\n\n" + module.strip() + "\n"


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--module", type=Path, default=ROOT / "modules/vmhook.ixx")
    parser.add_argument("--platform-header", type=Path, default=ROOT / "include/vmhook/detail/windows.hpp")
    parser.add_argument("--output", type=Path, default=ROOT / "include/vmhook/vmhook.hpp")
    parser.add_argument("--check", action="store_true", help="fail if the checked-in header differs")
    args = parser.parse_args()
    try:
        generated = generate(args.module, args.platform_header)
        if args.check:
            if not args.output.exists() or args.output.read_text(encoding="utf-8") != generated:
                print("Generated header is stale; run python tools/make_header.py", file=sys.stderr)
                return 1
            print("Generated C++23 header is up to date.")
        else:
            args.output.parent.mkdir(parents=True, exist_ok=True)
            args.output.write_text(generated, encoding="utf-8", newline="\n")
            print(f"Generated {args.output}")
    except (OSError, ValueError) as error:
        print(f"Header generation failed: {error}", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
