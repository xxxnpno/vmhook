#!/usr/bin/env python3
"""Check source contracts that compilation alone cannot enforce."""

from pathlib import Path
import re
import subprocess
import sys

ROOT = Path(__file__).resolve().parents[1]


def without_comments(source: str) -> str:
    # Match literals before comments so URLs and comment-like bytes inside a
    # string never hide an invocation-table lookup from the source check.
    tokens = re.compile(
        r'R"([^\s()\\]{0,16})\([\s\S]*?\)\1"'
        r'|"(?:\\[\s\S]|[^"\\])*"'
        r"|'(?:\\[\s\S]|[^'\\\n])*'"
        r'|//[^\n]*|/\*[\s\S]*?\*/'
    )
    return tokens.sub(
        lambda match: '\n' * match.group().count('\n')
        if match.group().startswith(('//', '/*')) else match.group(), source
    )


def main() -> int:
    errors = []
    modules = sorted((ROOT / 'modules').glob('*.ixx'))
    if not modules:
        errors.append('no module sources found')
    prohibited = re.compile(
        r'\b(?:JNI_GetCreatedJavaVMs|JNI_CreateJavaVM|JNIEnv|JavaVM|jvmtiEnv|JvmtiExport|'
        r'AttachCurrentThread|AttachCurrentThreadAsDaemon|DetachCurrentThread|'
        r'jni_bridge|jni_slots|scoped_hook|hook_handle)\b'
    )
    runtime = ROOT / 'include/vmhook/vmhook.hpp'
    for path in [*modules, runtime]:
        if not path.is_file():
            errors.append(f'{path.relative_to(ROOT)} does not exist')
            continue
        code = without_comments(path.read_text(encoding='utf-8'))
        for match in prohibited.finditer(code):
            errors.append(f'{path.relative_to(ROOT)} contains prohibited API {match.group()}')
        if path.suffix == '.ixx' and re.search(r'^\s*#', code, re.MULTILINE):
            errors.append(f'{path.relative_to(ROOT)} contains a preprocessor directive')
    generated = subprocess.run(
        [sys.executable, str(ROOT / 'tools/make_header.py'), '--check'],
        cwd=ROOT, check=False,
    )
    if generated.returncode:
        errors.append('the checked-in C++23 header is not reproducible from the module')
    for error in errors:
        print(f'FAIL: {error}', file=sys.stderr)
    return 1 if errors else 0


if __name__ == '__main__':
    raise SystemExit(main())
