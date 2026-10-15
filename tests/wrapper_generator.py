#!/usr/bin/env python3
"""Class-file parser and owning wrapper-generator contract tests."""
from __future__ import annotations

import importlib.util
import argparse
import hashlib
import json
import os
from pathlib import Path
import shutil
import struct
import subprocess
import sys
import tempfile
import unittest
import zipfile

ROOT = Path(__file__).resolve().parents[1]
spec = importlib.util.spec_from_file_location('generate_wrappers', ROOT / 'tools/generate_wrappers.py')
generator = importlib.util.module_from_spec(spec)
sys.modules[spec.name] = generator
spec.loader.exec_module(generator)

JAVA_FIXTURE = ROOT / 'tests/runtime/GeneratedWrapperFixture.java'


def java_compiler() -> str | None:
    found = shutil.which('javac')
    if found:
        return found
    if os.environ.get('JAVA_HOME'):
        candidate = Path(os.environ['JAVA_HOME']) / 'bin' / ('javac.exe' if os.name == 'nt' else 'javac')
        if candidate.is_file():
            return str(candidate)
    return None


def build_fixture(directory: Path, javac: str):
    directory.mkdir(parents=True, exist_ok=True)
    classes = directory / 'classes'
    if classes.exists():
        shutil.rmtree(classes)
    classes.mkdir()
    subprocess.run([javac, '-encoding', 'UTF-8', '-d', str(classes), str(JAVA_FIXTURE)],
                   check=True, capture_output=True, text=True, timeout=120)
    jar = directory / 'generator-fixture.jar'
    with zipfile.ZipFile(jar, 'w') as archive:
        for path in sorted(classes.rglob('*.class')):
            archive.write(path, path.relative_to(classes).as_posix())
    parsed = generator.read_inputs([jar], set())
    header = directory / 'generated_wrappers.hpp'
    module = directory / 'generated_wrappers.ixx'
    header.write_text(generator.generate(parsed, 'generator_fixtures'), encoding='utf-8')
    module.write_text(generator.generate(parsed, 'generator_fixtures', 'vmhook.generated_fixture'), encoding='utf-8')
    return parsed, header, module, jar


def fixture_class() -> bytes:
    # A minimal Java 8 class with a public static field, public constructor and
    # covariant-return synthetic bridge. The generator must ignore the bridge.
    constants = ('sample/Player', 'java/lang/Object', 'totalCount', 'I', '<init>', '()V',
                 'findPlayer', '(Lsample/Player;)Lsample/Player;', 'Signature', 'Ljava/util/List<Lsample/Player;>;')
    pool = b''.join(b'\x01' + struct.pack('>H', len(value)) + value.encode() for value in constants)
    pool += b'\x07\x00\x01\x07\x00\x02'
    header = b'\xca\xfe\xba\xbe\x00\x00\x00\x34' + struct.pack('>H', len(constants) + 3)
    class_header = struct.pack('>HHHH', 0x0021, 11, 12, 0)
    fields = struct.pack('>H', 1) + struct.pack('>HHHH', 0x0009, 3, 4, 0)
    methods = struct.pack('>H', 3)
    methods += struct.pack('>HHHH', 0x0001, 5, 6, 0)
    methods += struct.pack('>HHHH', 0x0001, 7, 8, 0)
    methods += struct.pack('>HHHH', 0x1041, 7, 8, 0)
    return header + pool + class_header + fields + methods + b'\x00\x00'


class WrapperGeneratorTests(unittest.TestCase):
    @unittest.skipUnless(java_compiler(), 'a JDK is required for real Java class/JAR fixtures')
    def test_real_java_reserved_names_and_metadata_encoding(self):
        with tempfile.TemporaryDirectory(prefix='vmhook-generator-real-') as temporary:
            directory = Path(temporary)
            classes, header, module, jar = build_fixture(directory, java_compiler())
            source = header.read_text(encoding='utf-8')
            names = {value.name for value in classes}
            self.assertIn('GeneratedWrapperFixture$\U00010400Type', names)
            self.assertEqual({value.name for value in generator.read_inputs([jar], set())}, names)
            for token in ('and_eq', 'or_eq', 'not_eq', 'xor_eq'):
                self.assertIn('auto java_' + token + '(', source)
                self.assertNotIn('auto ' + token + '(', source)
            self.assertIn('auto java_generated_wrapper_fixture(', source)
            self.assertIn('auto java_get_native_root(', source)
            self.assertIn('auto java_construct(', source)
            self.assertNotIn('auto construct(', source)
            self.assertNotIn('snake__case', '\n'.join(line for line in source.splitlines() if line.strip().startswith('auto ')))
            self.assertIn('::std::unique_ptr<::vmhook::iterable>', source)
            self.assertIn('::std::unique_ptr<::vmhook::map_entry>', source)
            # The JVM stores U+10400 as two three-byte UTF-16 surrogate units.
            # Compare with actual javac output, not a synthetic class parser.
            raw = next(path for path in (directory / 'classes').glob('*Type.class')).read_bytes()
            self.assertIn(bytes.fromhex('eda081edb080'), raw)
            self.assertNotIn(bytes.fromhex('f0909080'), raw)
            self.assertIn('\\xed""\\xa0""\\x81""\\xed""\\xb0""\\x80""Type', source)
            self.assertIn('\\xed""\\xa0""\\x81""\\xed""\\xb0""\\x80""Method', source)
            self.assertNotIn('\\xf0""\\x90""\\x90""\\x80', source)
            self.assertNotIn('#', module.read_text(encoding='utf-8'))
            self.assertEqual(source, generator.generate(classes, 'generator_fixtures'))

    def test_namespace_refusals(self):
        for namespace in ('std', 'class', 'or_eq', 'reserved__name'):
            with self.subTest(namespace=namespace), self.assertRaises(ValueError):
                generator.generate([], namespace)

    def test_real_class_layout_and_owning_generation(self):
        parsed = generator.read_class(fixture_class())
        self.assertEqual(parsed.name, 'sample/Player')
        self.assertEqual(parsed.major, 52)
        output = generator.generate([parsed], 'fixtures')
        self.assertIn('static_members::get_field("totalCount")->get()', output)
        self.assertIn('const ::std::unique_ptr<::fixtures::sample_player>& arg_0', output)
        self.assertIn('-> ::std::unique_ptr<::fixtures::sample_player>', output)
        self.assertEqual(output.count('auto sample_player::find_player('), 1)
        self.assertIn('"(Lsample/Player;)Lsample/Player;"', output)

    def test_method_descriptors_wide_arrays_and_refusals(self):
        arguments, result = generator.method_descriptor('(J[D[[Lsample/Player;)V')
        self.assertEqual(arguments, ['J', '[D', '[[Lsample/Player;'])
        self.assertEqual(result, 'V')
        self.assertEqual(generator.cpp_type('[[I', {}), '::std::unique_ptr<::vmhook::array<::vmhook::array<::std::int32_t>>>')
        for invalid in ('(V)V', '([V)V', '(L;)I', '(I)Vtail', '(I', '(I)'):
            with self.subTest(invalid=invalid), self.assertRaises(ValueError):
                generator.method_descriptor(invalid)
        with self.assertRaises(ValueError):
            generator.method_descriptor('(' + 'J' * 128 + ')V')

    def test_modified_utf8_and_malformed_class_refusal(self):
        self.assertEqual(generator.modified_utf8(b'A\xc0\x80\xed\xa0\xbd\xed\xb8\x80'), 'A\0\U0001f600')
        for invalid in (b'\0', b'\xc1\x81', b'\xe0\x80\x80', b'\xf0\x9f\x98\x80'):
            with self.subTest(invalid=invalid), self.assertRaises(ValueError):
                generator.modified_utf8(invalid)
        data = fixture_class()
        for length in (0, 4, 9, len(data) - 1):
            with self.subTest(length=length), self.assertRaises(ValueError):
                generator.read_class(data[:length])
        with self.assertRaises(ValueError):
            generator.read_class(data + b'trailing')

    def test_jar_selection_module_and_duplicates(self):
        with tempfile.TemporaryDirectory() as temporary:
            jar = Path(temporary) / 'fixture.jar'
            with zipfile.ZipFile(jar, 'w') as archive:
                archive.writestr('sample/Player.class', fixture_class())
                archive.writestr('META-INF/versions/11/sample/Player.class', fixture_class())
            classes = generator.read_inputs([jar], {'sample/Player'})
            self.assertEqual(len(classes), 1)
            module = generator.generate(classes, 'fixtures', 'fixture.wrappers')
            self.assertIn('export module fixture.wrappers;', module)
            self.assertNotIn('#', module)
            self.assertIn('[[=::vmhook::java_class("sample/Player")]]', module)
            self.assertIn('::vmhook::register_class<::fixtures::sample_player>();', module)
            with self.assertRaises(ValueError):
                generator.read_inputs([jar, jar], set())
            with self.assertRaises(ValueError):
                generator.read_inputs([jar], {'missing/Class'})

    def test_overload_erasure_name_collisions_are_explicit(self):
        methods = [generator.Member('doWork', '(La/One;)V', 1), generator.Member('doWork', '(Lb/Two;)V', 1)]
        java_class = generator.JavaClass('sample/Overloads', 52, 1, [], methods)
        output = generator.generate([java_class], 'fixtures')
        self.assertIn('auto do_work(', output)
        self.assertRegex(output, r'auto do_work_[0-9a-f]{8}\(')
        self.assertIn('const ::std::unique_ptr<::vmhook::any_object>&', output)
        self.assertEqual(generator.snake_case('HTTPServerCount'), 'http_server_count')
        self.assertEqual(generator.snake_case('class'), 'java_class')


def compile_generated(args) -> None:
    directory = args.output_dir.resolve()
    compiler = str(args.compiler.resolve())
    inputs = [Path(__file__), ROOT / 'tools/generate_wrappers.py', JAVA_FIXTURE,
              ROOT / 'tests/api/generated_wrappers.cpp', ROOT / 'tests/runtime/generated_wrapper_probe.cpp']
    input_hashes = {str(path): hashlib.sha256(path.read_bytes()).hexdigest() for path in inputs}
    javac = args.javac or java_compiler()
    if not javac:
        raise ValueError('generated-wrapper compiler checks require javac')
    classes, header, module, jar = build_fixture(directory, javac)
    commands = []

    def run(command: list[str], label: str) -> None:
        completed = subprocess.run(command, capture_output=True, text=True, timeout=240, cwd=directory)
        commands.append({'label': label, 'command': command, 'exit_code': completed.returncode})
        (directory / (label + '.log')).write_text(completed.stdout + completed.stderr, encoding='utf-8')
        if completed.returncode:
            raise RuntimeError(label + ' failed: ' + completed.stderr[-6000:])

    module_object = []
    libraries = []
    if args.module_build_dir:
        backend = args.module_build_dir.resolve()
        information = json.loads((backend / 'build-info.json').read_text(encoding='utf-8'))
        mapper = directory / 'module.mapper'
        mapper.write_text((backend / 'module.mapper').read_text(encoding='utf-8')
            + 'vmhook.generated_fixture ' + (directory / 'generated_fixture.gcm').as_posix() + '\n', encoding='utf-8')
        options = [value for value in information['consumer_flags'] if not value.startswith('-fmodule-mapper=')]
        options += ['-fmodule-mapper=' + mapper.as_posix(), '-DVMHOOK_TEST_MODULE']
        generated_object = directory / 'generated_fixture.o'
        run([compiler, *options, '-Wall', '-Wextra', '-Werror', '-x', 'c++', '-c', str(module),
             '-o', str(generated_object)], 'module_interface')
        module_object = [str(generated_object)]
        libraries = [str(backend / 'libvmhook.a')]
    else:
        options = ['-std=c++23', '-O2', '-I' + str(args.include_dir.resolve()), '-I' + str(directory)]
    executable = directory / 'generated_consumer.exe'
    run([compiler, *options, '-Wall', '-Wextra', '-Werror',
         str(ROOT / 'tests/api/generated_wrappers.cpp'), *module_object, *libraries,
         '-static-libgcc', '-static-libstdc++', '-static', '-o', str(executable)], 'generated_consumer')
    if os.name == 'nt' or args.emulator:
        run(([str(args.emulator.resolve())] if args.emulator else []) + [str(executable)], 'consumer_execution')
    probe = args.probe_output.resolve()
    probe.parent.mkdir(parents=True, exist_ok=True)
    run([compiler, *options, '-Wall', '-Wextra', '-Werror', '-shared',
         str(ROOT / 'tests/runtime/generated_wrapper_probe.cpp'), *module_object, *libraries,
         '-static-libgcc', '-static-libstdc++', '-static', '-o', str(probe)], 'runtime_probe')
    if any(hashlib.sha256(path.read_bytes()).hexdigest() != input_hashes[str(path)] for path in inputs):
        raise RuntimeError('Generator fixture source changed during compilation')
    files = [*inputs, header, module, jar, executable, probe]
    evidence = {'schema': 1, 'interface': 'module' if args.module_build_dir else 'header',
        'java_classes': [value.name for value in classes], 'commands': commands,
        'consumer_executed': os.name == 'nt' or bool(args.emulator),
        'native_windows': os.name == 'nt',
        'sha256': {str(path): hashlib.sha256(path.read_bytes()).hexdigest() for path in files}}
    (directory / 'verification.json').write_text(json.dumps(evidence, indent=2) + '\n', encoding='utf-8')
    print(json.dumps({'generated_wrapper_compile': 'pass', 'interface': evidence['interface'],
                      'evidence': str(directory / 'verification.json')}))


if __name__ == '__main__':
    if '--compile-generated' in sys.argv:
        parser = argparse.ArgumentParser(description='Compile real javac-generated VMHook wrapper fixtures.')
        parser.add_argument('--compile-generated', action='store_true')
        parser.add_argument('--compiler', type=Path, required=True)
        parser.add_argument('--javac')
        parser.add_argument('--output-dir', type=Path, required=True)
        parser.add_argument('--include-dir', type=Path, default=ROOT / 'include')
        parser.add_argument('--module-build-dir', type=Path)
        parser.add_argument('--probe-output', type=Path, required=True)
        parser.add_argument('--emulator', type=Path)
        try:
            compile_generated(parser.parse_args())
        except (OSError, ValueError, RuntimeError, subprocess.SubprocessError) as failure:
            parser.exit(1, 'Generated wrapper check failed: ' + str(failure) + '\n')
    else:
        unittest.main()
