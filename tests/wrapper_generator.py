#!/usr/bin/env python3
"""Class-file parser and owning wrapper-generator contract tests."""
from __future__ import annotations

import importlib.util
from pathlib import Path
import struct
import sys
import tempfile
import unittest
import zipfile

ROOT = Path(__file__).resolve().parents[1]
spec = importlib.util.spec_from_file_location('generate_wrappers', ROOT / 'tools/generate_wrappers.py')
generator = importlib.util.module_from_spec(spec)
sys.modules[spec.name] = generator
spec.loader.exec_module(generator)


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
    def test_real_class_layout_and_owning_generation(self):
        parsed = generator.read_class(fixture_class())
        self.assertEqual(parsed.name, 'sample/Player')
        self.assertEqual(parsed.major, 52)
        output = generator.generate([parsed], 'fixtures')
        self.assertIn('static_members::get_field("totalCount")->get()', output)
        self.assertIn('const std::unique_ptr<sample_player>& arg_0', output)
        self.assertIn('-> std::unique_ptr<sample_player>', output)
        self.assertEqual(output.count('auto sample_player::find_player('), 1)
        self.assertIn('"(Lsample/Player;)Lsample/Player;"', output)

    def test_method_descriptors_wide_arrays_and_refusals(self):
        arguments, result = generator.method_descriptor('(J[D[[Lsample/Player;)V')
        self.assertEqual(arguments, ['J', '[D', '[[Lsample/Player;'])
        self.assertEqual(result, 'V')
        self.assertEqual(generator.cpp_type('[[I', {}), 'std::unique_ptr<vmhook::array<vmhook::array<std::int32_t>>>')
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
            self.assertIn('[[=vmhook::java_class("sample/Player")]]', module)
            self.assertIn('vmhook::register_class<sample_player>();', module)
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
        self.assertIn('const std::unique_ptr<vmhook::any_object>&', output)
        self.assertEqual(generator.snake_case('HTTPServerCount'), 'http_server_count')
        self.assertEqual(generator.snake_case('class'), 'java_class')


if __name__ == '__main__':
    unittest.main()
