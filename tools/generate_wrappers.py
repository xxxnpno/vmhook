#!/usr/bin/env python3
"""Generate optional owning VMHook wrappers from Java .class files or a JAR.

Emits exact JVM descriptors, snake_case C++ members and unique_ptr object APIs.
No JVM is started. Only public, nonsynthetic members are generated. Generic
Signature metadata is preserved in --schema-json; erased types stay explicit.
"""
from __future__ import annotations

import argparse
from dataclasses import asdict, dataclass
import hashlib
import json
from pathlib import Path
import re
import struct
import sys
import zipfile

PRIMITIVES = {
    'Z': 'bool', 'B': 'std::int8_t', 'C': 'std::uint16_t', 'S': 'std::int16_t',
    'I': 'std::int32_t', 'J': 'std::int64_t', 'F': 'float', 'D': 'double', 'V': 'void',
}
BUILTINS = {
    'java/lang/String': 'vmhook::java_string', 'java/lang/Class': 'vmhook::class_object',
    'java/lang/ClassLoader': 'vmhook::class_loader', 'java/lang/Throwable': 'vmhook::throwable',
    'java/lang/Enum': 'vmhook::java_enum', 'java/util/Collection': 'vmhook::collection',
    'java/util/List': 'vmhook::list', 'java/util/ArrayList': 'vmhook::array_list',
    'java/util/LinkedList': 'vmhook::linked_list', 'java/util/Set': 'vmhook::set',
    'java/util/HashSet': 'vmhook::hash_set', 'java/util/LinkedHashSet': 'vmhook::linked_hash_set',
    'java/util/TreeSet': 'vmhook::tree_set', 'java/util/Map': 'vmhook::map',
    'java/util/HashMap': 'vmhook::hash_map', 'java/util/LinkedHashMap': 'vmhook::linked_hash_map',
    'java/util/TreeMap': 'vmhook::tree_map', 'java/util/concurrent/ConcurrentHashMap': 'vmhook::concurrent_hash_map',
    'java/util/Queue': 'vmhook::queue', 'java/util/Deque': 'vmhook::deque',
    'java/util/ArrayDeque': 'vmhook::array_deque', 'java/util/Iterator': 'vmhook::iterator',
    'java/util/Optional': 'vmhook::optional', 'java/nio/ByteBuffer': 'vmhook::byte_buffer',
    'java/lang/Boolean': 'vmhook::java_boolean', 'java/lang/Byte': 'vmhook::java_byte',
    'java/lang/Character': 'vmhook::java_char', 'java/lang/Short': 'vmhook::java_short',
    'java/lang/Integer': 'vmhook::java_integer', 'java/lang/Long': 'vmhook::java_long',
    'java/lang/Float': 'vmhook::java_float', 'java/lang/Double': 'vmhook::java_double',
}
CPP_KEYWORDS = set(('alignas alignof and asm auto bitand bitor bool break case catch char char8_t '
    'char16_t char32_t class compl concept const consteval constexpr constinit const_cast continue '
    'co_await co_return co_yield decltype default delete do double dynamic_cast else enum explicit '
    'export extern false float for friend goto if inline int long mutable namespace new noexcept '
    'not nullptr operator or private protected public register reinterpret_cast requires return '
    'short signed sizeof static static_assert static_cast struct switch template this thread_local '
    'throw true try typedef typeid typename union unsigned using virtual void volatile wchar_t while '
    'xor get_field get_method static_members get_instance set_native_root self register_java_class').split())


def snake_case(text: str) -> str:
    text = re.sub(r'([A-Z]+)([A-Z][a-z])', r'\1_\2', text)
    text = re.sub(r'([a-z0-9])([A-Z])', r'\1_\2', text)
    text = re.sub(r'[^a-zA-Z0-9_]', '_', text).lower().strip('_') or 'member'
    if text[0].isdigit() or text in CPP_KEYWORDS:
        text = 'java_' + text
    return text


class Reader:
    def __init__(self, data: bytes):
        self.data = data
        self.position = 0

    def take(self, size: int) -> bytes:
        if size < 0 or size > len(self.data) - self.position:
            raise ValueError('truncated Java class file')
        result = self.data[self.position:self.position + size]
        self.position += size
        return result

    def number(self, size: int) -> int:
        return int.from_bytes(self.take(size), 'big')


def modified_utf8(data: bytes) -> str:
    # Constant-pool strings encode UTF-16 code units with modified UTF-8. Decode
    # units explicitly, then combine paired surrogates for JSON diagnostics.
    units: list[int] = []
    index = 0
    while index < len(data):
        first = data[index]
        if 0 < first < 128:
            units.append(first); index += 1
        elif first & 0xe0 == 0xc0 and index + 1 < len(data) and data[index + 1] & 0xc0 == 0x80:
            value = ((first & 0x1f) << 6) | (data[index + 1] & 0x3f)
            if value < 128 and not (first == 0xc0 and data[index + 1] == 0x80):
                raise ValueError('invalid overlong modified UTF-8')
            units.append(value); index += 2
        elif first & 0xf0 == 0xe0 and index + 2 < len(data) and all(value & 0xc0 == 0x80 for value in data[index + 1:index + 3]):
            value = ((first & 0xf) << 12) | ((data[index + 1] & 0x3f) << 6) | (data[index + 2] & 0x3f)
            if value < 0x800:
                raise ValueError('invalid overlong modified UTF-8')
            units.append(value); index += 3
        else:
            raise ValueError('invalid modified UTF-8 in constant pool')
    return b''.join(struct.pack('>H', value) for value in units).decode('utf-16-be', errors='surrogatepass')


@dataclass
class Member:
    name: str
    descriptor: str
    flags: int
    generic_signature: str | None = None


@dataclass
class JavaClass:
    name: str
    major: int
    flags: int
    fields: list[Member]
    methods: list[Member]


def read_class(data: bytes) -> JavaClass:
    reader = Reader(data)
    if reader.take(4) != b'\xca\xfe\xba\xbe':
        raise ValueError('input is not a Java class file')
    reader.number(2)
    major = reader.number(2)
    pool: list[object] = [None] * reader.number(2)
    index = 1
    while index < len(pool):
        tag = reader.number(1)
        if tag == 1:
            pool[index] = modified_utf8(reader.take(reader.number(2)))
        elif tag in (7, 8, 16, 19, 20):
            pool[index] = (tag, reader.number(2))
        elif tag in (3, 4, 9, 10, 11, 12, 17, 18):
            reader.take(4)
        elif tag in (5, 6):
            reader.take(8); index += 1
        elif tag == 15:
            reader.take(3)
        else:
            raise ValueError(f'unsupported constant-pool tag {tag}')
        index += 1

    def utf8(cp_index: int) -> str:
        if not 0 < cp_index < len(pool) or not isinstance(pool[cp_index], str):
            raise ValueError('invalid UTF-8 constant-pool index')
        return pool[cp_index]

    def attributes() -> str | None:
        signature = None
        for _ in range(reader.number(2)):
            name = utf8(reader.number(2))
            value = reader.take(reader.number(4))
            if name == 'Signature':
                if len(value) != 2:
                    raise ValueError('invalid Signature attribute')
                signature = utf8(int.from_bytes(value, 'big'))
        return signature

    flags = reader.number(2)
    this_class = reader.number(2)
    reader.number(2)
    if not 0 < this_class < len(pool) or not isinstance(pool[this_class], tuple) or pool[this_class][0] != 7:
        raise ValueError('invalid class-name constant')
    name = utf8(pool[this_class][1])
    for _ in range(reader.number(2)):
        reader.number(2)

    def members() -> list[Member]:
        result = []
        for _ in range(reader.number(2)):
            member_flags = reader.number(2)
            member_name = utf8(reader.number(2))
            descriptor = utf8(reader.number(2))
            result.append(Member(member_name, descriptor, member_flags, attributes()))
        return result

    fields = members()
    methods = members()
    attributes()
    if reader.position != len(data):
        raise ValueError('trailing bytes after Java class file')
    return JavaClass(name, major, flags, fields, methods)


def descriptor_type(descriptor: str, position: int = 0, *, allow_void: bool = False) -> tuple[str, int]:
    start = position
    if position >= len(descriptor):
        raise ValueError('truncated JVM descriptor')
    while position < len(descriptor) and descriptor[position] == '[':
        position += 1
    dimensions = position - start
    if dimensions > 255 or position == len(descriptor):
        raise ValueError('invalid array descriptor')
    kind = descriptor[position]
    if kind == 'L':
        end = descriptor.find(';', position)
        if end < position + 2 or any(char in descriptor[position + 1:end] for char in '.['):
            raise ValueError('invalid reference descriptor')
        position = end + 1
    elif kind in PRIMITIVES and (kind != 'V' or (allow_void and dimensions == 0)):
        position += 1
    else:
        raise ValueError('invalid JVM descriptor kind')
    return descriptor[start:position], position


def method_descriptor(descriptor: str) -> tuple[list[str], str]:
    if not descriptor.startswith('('):
        raise ValueError('invalid method descriptor')
    arguments = []
    position = 1
    while position < len(descriptor) and descriptor[position] != ')':
        argument, position = descriptor_type(descriptor, position)
        arguments.append(argument)
    if position >= len(descriptor):
        raise ValueError('method descriptor has no closing parenthesis')
    result, position = descriptor_type(descriptor, position + 1, allow_void=True)
    if position != len(descriptor):
        raise ValueError('trailing characters after method descriptor')
    slots = sum(2 if argument in ('J', 'D') else 1 for argument in arguments)
    if slots > 255:
        raise ValueError('method descriptor exceeds 255 parameter slots')
    return arguments, result


def cpp_type(descriptor: str, names: dict[str, str]) -> str:
    parsed, end = descriptor_type(descriptor, allow_void=True)
    if end != len(descriptor):
        raise ValueError('trailing characters after field descriptor')
    if parsed[0] == '[':
        child = cpp_type(parsed[1:], names)
        if child.startswith('std::unique_ptr<'):
            child = child[len('std::unique_ptr<'):-1]
        return f'std::unique_ptr<vmhook::array<{child}>>'
    if parsed[0] == 'L':
        name = parsed[1:-1]
        return f'std::unique_ptr<{names.get(name, BUILTINS.get(name, "vmhook::any_object"))}>'
    return PRIMITIVES[parsed]


def cpp_literal(text: str) -> str:
    # Names and descriptors are identifiers, not arbitrary Java string values.
    # Non-ASCII names remain UTF-8 with explicit bytes; adjacent literals ensure
    # a following hexadecimal character cannot extend a \x escape.
    parts = []
    for byte in text.encode('utf-8', errors='strict'):
        if 32 <= byte < 127 and byte not in (34, 92):
            parts.append(chr(byte))
        else:
            parts.append(f'\\x{byte:02x}""')
    return '"' + ''.join(parts) + '"'


def generate(classes: list[JavaClass], namespace: str, module: str | None = None) -> str:
    if not re.fullmatch(r'[a-z][a-z0-9_]*', namespace) or namespace in CPP_KEYWORDS:
        raise ValueError('namespace must be a non-keyword snake_case identifier')
    names: dict[str, str] = {}
    for java_class in classes:
        name = snake_case(java_class.name)
        if name in names.values():
            name += '_' + hashlib.sha256(java_class.name.encode()).hexdigest()[:8]
        names[java_class.name] = name
    declarations = []
    definitions = []
    for java_class in classes:
        cls = names[java_class.name]
        annotation = ' [[=vmhook::java_class(' + cpp_literal(java_class.name) + ')]]' if module else ''
        declaration = [f'class{annotation} {cls} : public vmhook::object<{cls}>', '{', 'public:',
            f'    using vmhook::object<{cls}>::object;', '    static auto register_java_class() -> bool;']
        registration = '' if module else cpp_literal(java_class.name)
        definitions.append(f'inline auto {cls}::register_java_class() -> bool\n'
            '{\n    return vmhook::register_class<' + cls + '>(' + registration + ');\n}\n')
        used: set[tuple[str, str]] = set()

        def member(name: str, result: str, params: list[tuple[str, str]], body: str, static: bool) -> None:
            signature = ', '.join(kind for kind, _ in params)
            if (name, signature) in used:
                name += '_' + hashlib.sha256((name + signature + body).encode()).hexdigest()[:8]
            used.add((name, signature))
            parameters = ', '.join(kind + ' ' + label for kind, label in params)
            const = '' if static else ' const'
            declaration.append(f'    {"static " if static else ""}auto {name}({parameters}){const} -> {result};')
            definitions.append(f'inline auto {cls}::{name}({parameters}){const} -> {result}\n'
                               '{\n    ' + body + '\n}\n')

        for field in java_class.fields:
            if not field.flags & 0x0001 or field.flags & 0x1000:
                continue
            result = cpp_type(field.descriptor, names)
            if result == 'void':
                raise ValueError('field descriptor cannot be void')
            static = bool(field.flags & 0x0008)
            lookup = 'static_members::get_field' if static else 'get_field'
            expression = lookup + '(' + cpp_literal(field.name) + ')'
            member('get_' + snake_case(field.name), result, [], 'return ' + expression + '->get();', static)
            if not field.flags & 0x0010:
                param_type = 'const ' + result + '&' if result.startswith('std::unique_ptr<') else result
                member('set_' + snake_case(field.name), 'void', [(param_type, 'value')], expression + '->set(value);', static)
        for method in java_class.methods:
            if not method.flags & 0x0001 or method.flags & (0x0040 | 0x1000) or method.name == '<clinit>':
                continue
            args, result_descriptor = method_descriptor(method.descriptor)
            static = bool(method.flags & 0x0008)
            if sum(2 if value in ('J', 'D') else 1 for value in args) + (0 if static else 1) > 255:
                raise ValueError('instance method descriptor exceeds JVM parameter slots')
            params = []
            for index, argument in enumerate(args):
                kind = cpp_type(argument, names)
                if kind.startswith('std::unique_ptr<'):
                    kind = 'const ' + kind + '&'
                params.append((kind, f'arg_{index}'))
            forwarded = ', '.join(name for _, name in params)
            if method.name == '<init>':
                if java_class.flags & (0x0200 | 0x0400):
                    continue
                member('create', 'std::unique_ptr<' + cls + '>', params,
                       'return vmhook::make_unique_with_descriptor<' + cls + '>(' + cpp_literal(method.descriptor) + (', ' + forwarded if forwarded else '') + ');', True)
                continue
            result = cpp_type(result_descriptor, names)
            lookup = 'static_members::get_method' if static else 'get_method'
            invocation = lookup + '(' + cpp_literal(method.name) + ', ' + cpp_literal(method.descriptor) + ')->call(' + forwarded + ')'
            member(snake_case(method.name), result, params,
                   ('(void)' if result == 'void' else 'return ') + invocation + ';', static)
        declaration.append('};\n')
        declarations.append('\n'.join(declaration))
    registrations = ['inline auto register_java_classes() -> bool', '{', '    bool success{true};']
    registrations.extend('    success = ' + name + '::register_java_class() && success;' for name in names.values())
    registrations.extend(['    return success;', '}\n'])
    definitions.append('\n'.join(registrations))
    if module:
        if not re.fullmatch(r'[a-z][a-z0-9_]*(?:\.[a-z][a-z0-9_]*)*', module):
            raise ValueError('module name must contain snake_case identifiers')
        prefix = f'export module {module};\nimport std;\nimport vmhook;\n\nexport '
    else:
        prefix = '#pragma once\n#include <vmhook/vmhook.hpp>\n\n'
    forwards = '\n'.join('class ' + name + ';' for name in names.values())
    return ('// Generated by tools/generate_wrappers.py; regenerate from the Java schema.\n'
            + prefix + 'namespace ' + namespace + '\n{\n' + forwards + '\n\n'
            + '\n'.join(declarations + definitions) + '}\n')


def read_inputs(paths: list[Path], selected: set[str]) -> list[JavaClass]:
    classes: dict[str, JavaClass] = {}
    for path in paths:
        if path.suffix.lower() == '.jar':
            with zipfile.ZipFile(path) as archive:
                for entry in archive.infolist():
                    if not entry.filename.endswith('.class') or entry.filename.startswith('META-INF/versions/'):
                        continue
                    if selected and entry.filename[:-6] not in selected:
                        continue
                    if entry.file_size > 16 * 1024 * 1024:
                        raise ValueError('class file exceeds supported input size')
                    parsed = read_class(archive.read(entry))
                    if parsed.name in classes:
                        raise ValueError('duplicate Java class: ' + parsed.name)
                    classes[parsed.name] = parsed
        else:
            if path.stat().st_size > 16 * 1024 * 1024:
                raise ValueError('class file exceeds supported input size')
            parsed = read_class(path.read_bytes())
            if selected and parsed.name not in selected:
                continue
            if parsed.name in classes:
                raise ValueError('duplicate Java class: ' + parsed.name)
            classes[parsed.name] = parsed
    classes.pop('module-info', None)
    missing = selected - classes.keys()
    if missing:
        raise ValueError('requested classes are absent: ' + ', '.join(sorted(missing)))
    return [classes[name] for name in sorted(classes)]


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('input', nargs='+', type=Path)
    parser.add_argument('--output', type=Path, required=True)
    parser.add_argument('--namespace', default='vmhook_generated')
    parser.add_argument('--module', help='emit a preprocessor-free C++26 module interface')
    parser.add_argument('--class', dest='classes', action='append', default=[], help='select an internal pkg/Class name')
    parser.add_argument('--schema-json', type=Path)
    args = parser.parse_args()
    try:
        classes = read_inputs(args.input, set(args.classes))
        if not classes:
            raise ValueError('no Java classes selected')
        content = generate(classes, args.namespace, args.module)
        args.output.parent.mkdir(parents=True, exist_ok=True)
        args.output.write_text(content, encoding='utf-8', newline='\n')
        if args.schema_json:
            args.schema_json.parent.mkdir(parents=True, exist_ok=True)
            args.schema_json.write_text(json.dumps({'format': 1, 'classes': [asdict(value) for value in classes]},
                ensure_ascii=True, indent=2) + '\n', encoding='utf-8')
    except (OSError, ValueError, zipfile.BadZipFile, UnicodeError) as error:
        print(f'Wrapper generation failed: {error}', file=sys.stderr)
        return 1
    return 0


if __name__ == '__main__':
    sys.exit(main())
