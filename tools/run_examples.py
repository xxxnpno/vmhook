#!/usr/bin/env python3
"""Run the standalone DLL examples in an ordinary, freshly launched JVM.

Only the Java subprocess created by this harness receives a DLL. No JVM flags
or agents are added. --emulator is optional development ABI validation; native
Windows runs provide the release evidence.
"""

from __future__ import annotations

import argparse
import hashlib
import json
import os
from pathlib import Path
import queue
import subprocess
import tempfile
import threading
import time


EXAMPLES = ("native_worker", "wrappers_collections", "method_hook", "field_class_observer")


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--java", type=Path, required=True)
    parser.add_argument("--javac", type=Path, required=True)
    parser.add_argument("--injector", type=Path, required=True)
    parser.add_argument("--binaries", type=Path, required=True)
    parser.add_argument("--example", choices=EXAMPLES, action="append")
    parser.add_argument("--benchmark", type=Path, help="run this benchmark DLL instead of the four examples")
    parser.add_argument("--iterations", type=int, default=1000)
    parser.add_argument("--emulator", type=Path)
    parser.add_argument("--evidence", type=Path)
    args = parser.parse_args()
    if os.name != "nt" and not args.emulator:
        parser.error("use native Windows, or select an explicit development emulator")
    if not 20 <= args.iterations <= 100000:
        parser.error("--iterations must be in [20, 100000]")
    prefix = [str(args.emulator)] if args.emulator else []

    def windows_path(path: Path) -> str:
        value = str(path.resolve())
        return value if os.name == "nt" else "Z:" + value.replace("/", "\\")

    def digest(path: Path) -> str:
        return hashlib.sha256(path.read_bytes()).hexdigest()

    jvm_candidates = (args.java.parent / "server/jvm.dll",
                      args.java.parent.parent / "jre/bin/server/jvm.dll")
    jvm_image = next((path for path in jvm_candidates if path.is_file()), None)
    runs = []
    binaries = [("benchmark", args.benchmark)] if args.benchmark else [
        (name, args.binaries / f"vmhook_example_{name}.dll")
        for name in (args.example or EXAMPLES)]
    with tempfile.TemporaryDirectory(prefix="vmhook-examples-") as temporary:
        directory = Path(temporary)
        source = Path(__file__).resolve().parents[1] / "examples/ExampleFixture.java"
        subprocess.run(prefix + [str(args.javac), "-d", windows_path(directory), windows_path(source)],
                       check=True, timeout=90)
        for name, binary in binaries:
            result = directory / f"{name}.txt"
            environment = dict(os.environ, VMHOOK_EXAMPLE_RESULT=windows_path(result),
                               VMHOOK_BENCHMARK_ITERATIONS=str(args.iterations))
            command = prefix + [str(args.java), "-cp", windows_path(directory), "ExampleFixture"]
            process = subprocess.Popen(command, env=environment, text=True,
                                       stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
            output: queue.Queue[str] = queue.Queue()

            def read_output() -> None:
                for line in process.stdout:
                    output.put(line.rstrip())

            reader = threading.Thread(target=read_output, daemon=True)
            reader.start()
            captured = []
            before = digest(binary)
            started = time.monotonic()
            try:
                deadline = started + 90
                pid = None
                while time.monotonic() < deadline:
                    if process.poll() is not None:
                        raise RuntimeError(f"Java exited before {name}: {process.returncode}")
                    try:
                        line = output.get(timeout=0.1)
                    except queue.Empty:
                        continue
                    captured.append(line)
                    print(f"{name}: {line}", flush=True)
                    if line.startswith("PID="):
                        pid = line[4:]
                    if line == "READY":
                        break
                else:
                    raise RuntimeError(f"{name}: Java fixture did not become ready")
                if not pid or not pid.isdecimal():
                    raise RuntimeError("missing Windows JVM PID")
                subprocess.run(prefix + [str(args.injector.resolve()), pid, windows_path(binary)],
                               check=True, timeout=30)
                values = {}
                deadline = time.monotonic() + 180
                while time.monotonic() < deadline:
                    if result.is_file():
                        values = dict(line.split("=", 1) for line in result.read_text().splitlines() if "=" in line)
                        if "complete" in values or "exception" in values or "fail" in values.values():
                            break
                    if process.poll() is not None:
                        raise RuntimeError(f"Java exited during {name}: {process.returncode}")
                    time.sleep(0.1)
                for key, value in values.items():
                    print(f"{name}.{key}={value}", flush=True)
                if values.get("complete") != "pass" or "exception" in values or "fail" in values.values():
                    raise RuntimeError(f"{name} failed: {values}")
                if digest(binary) != before:
                    raise RuntimeError(f"{name} DLL changed during verification")
                runs.append({"name": name, "dll": str(binary.resolve()), "sha256": before,
                             "checks_and_metrics": values, "java_output": captured,
                             "seconds": round(time.monotonic() - started, 3)})
            finally:
                process.terminate()
                try:
                    process.wait(timeout=5)
                except subprocess.TimeoutExpired:
                    process.kill()
                    process.wait()
                reader.join(timeout=1)
    if args.evidence:
        args.evidence.parent.mkdir(parents=True, exist_ok=True)
        args.evidence.write_text(json.dumps({
            "schema": 1, "native_windows": os.name == "nt", "java": str(args.java.resolve()),
            "java_launcher_sha256": digest(args.java), "iterations": args.iterations,
            "jvm_image": str(jvm_image.resolve()) if jvm_image else None,
            "jvm_image_sha256": digest(jvm_image) if jvm_image else None,
            "runs": runs,
        }, indent=2) + "\n", encoding="utf-8")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
