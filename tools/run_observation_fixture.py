#!/usr/bin/env python3
"""Verify public method, field and class hooks in a JVM started without VM flags.

The harness only injects its own Java subprocess. Native Windows is the default;
--emulator permits separate Windows ABI checks through a local Wine wrapper.
"""

import argparse
import hashlib
import json
import os
from pathlib import Path
import queue
import subprocess
import sys
import tempfile
import threading
import time


CHECKS = {
    "jit": ["metadata", "compiled_callee", "compiled_caller", "inline_metadata",
            "typed_compiled", "hook", "typed_hooks", "typed_returns", "void_cancellation",
            "callee_deoptimized", "callbacks", "registry_contention",
            "compiled_caller_observed", "direct_observed", "future_caller_observed",
            "callee_stays_interpreted", "caller_recompiled", "unrelated_recompiled",
            "future_caller_compiled", "compiler_restored", "shutdown_original_body",
            "shutdown_no_callbacks", "shutdown_flags_restored", "jit_complete"],
    "fields": ["metadata", "compiled_accessor", "field_registration", "field_refusals",
               "field_callbacks", "field_gc", "future_field_callbacks",
               "field_removal", "field_shutdown", "fields_complete"],
    "classes": ["class_registration", "class_identity", "class_bytebuffer",
                "class_hidden_or_anonymous", "class_arrays", "class_bootstrap",
                "class_failed_definition", "class_callback_state", "class_removal",
                "class_persistent", "class_shutdown", "classes_complete"],
    "callbacks": ["callback_proof"],
}

FIXTURES = {
    "jit": "ObservationJitFixture",
    "fields": "ObservationFieldFixture",
    "classes": "ObservationClassFixture",
    "callbacks": "ObservationCallbackFixture",
}


def checksum(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def execute_mode(args, mode, directory):
    result = directory / (mode + "-result.txt")
    control = directory / (mode + "-control")

    def windows_path(path):
        path = Path(path).resolve()
        return str(path) if os.name == "nt" else "Z:" + str(path).replace("/", "\\")

    prefix = [str(args.emulator)] if args.emulator else []
    environment = dict(os.environ, VMHOOK_OBSERVATION_RESULT=windows_path(result),
                       VMHOOK_OBSERVATION_CONTROL=windows_path(control),
                       VMHOOK_OBSERVATION_MODE=mode, VMHOOK_CALLBACK_RESULT=windows_path(result))
    command = prefix + [str(args.java), "-cp", windows_path(directory), FIXTURES[mode]]
    process = subprocess.Popen(command, env=environment, stdout=subprocess.PIPE,
                               stderr=subprocess.STDOUT, text=True)
    output = queue.Queue()
    captured = []

    def read_output():
        for line in process.stdout:
            output.put(line.rstrip())

    threading.Thread(target=read_output, daemon=True).start()

    def next_line(timeout):
        try:
            line = output.get(timeout=timeout)
        except queue.Empty:
            return None
        captured.append(line)
        print(f"{mode}: {line}", flush=True)
        if line.startswith("Exception in thread") or "fatal error has been detected" in line:
            raise RuntimeError("Java fixture failed: " + line)
        return line

    started = time.monotonic()
    try:
        deadline = started + 90
        pid = None
        while time.monotonic() < deadline:
            if process.poll() is not None:
                raise RuntimeError(f"Java exited before injection: {process.returncode}")
            line = next_line(0.25)
            if line is None:
                continue
            if line.startswith("PID="):
                pid = line.split("=", 1)[1]
            if line == "READY":
                break
        else:
            raise RuntimeError("Java fixture did not become ready")
        if not pid or not pid.isdecimal():
            raise RuntimeError("Java fixture did not publish its Windows process ID")
        probe = args.callback_probe if mode == "callbacks" else args.probe
        subprocess.run(prefix + [str(args.injector.resolve()), pid, windows_path(probe)],
                       check=True, timeout=30)
        values = {}
        deadline = time.monotonic() + args.timeout
        while time.monotonic() < deadline:
            next_line(0.1)
            if result.exists():
                values = dict(line.split("=", 1) for line in result.read_text().splitlines()
                              if "=" in line)
                completed = "callback_proof" if mode == "callbacks" else mode + "_complete"
                if values.get(completed) == "pass":
                    break
                if "exception" in values or any(value in ("fail", "timeout", "unsupported")
                                                for value in values.values()):
                    break
            if process.poll() is not None:
                raise RuntimeError(f"Java exited during the probe: {process.returncode}")
        for key, value in values.items():
            print(f"{mode}.{key}={value}", flush=True)
        failed = [key for key in CHECKS[mode] if values.get(key) != "pass"]
        failed.extend(key for key, value in values.items()
                      if value in ("fail", "timeout", "unsupported") or key == "exception")
        if mode == "callbacks":
            failed.extend(key for key in ("callbacks", "nested", "returns") if values.get(key) != "24")
            gc = values.get("gc_observed", "")
            if not gc.isdecimal() or int(gc) == 0:
                failed.append("gc_observed")
        if failed:
            raise RuntimeError("failed " + mode + " checks: " + ", ".join(sorted(set(failed))))
        return {"mode": mode, "checks": values, "java_output": captured,
                "seconds": round(time.monotonic() - started, 3)}
    finally:
        process.terminate()
        try:
            process.wait(timeout=5)
        except subprocess.TimeoutExpired:
            process.kill()
            process.wait()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--java", type=Path, default=Path("java"))
    parser.add_argument("--javac", type=Path, default=Path("javac"))
    parser.add_argument("--injector", type=Path, required=True)
    parser.add_argument("--probe", type=Path, required=True)
    parser.add_argument("--callback-probe", type=Path,
                        help="also verify rooted, nested method callbacks during moving GC")
    parser.add_argument("--mode", choices=["all", *FIXTURES], default="all")
    parser.add_argument("--emulator", type=Path)
    parser.add_argument("--timeout", type=int, default=180)
    parser.add_argument("--evidence", type=Path)
    args = parser.parse_args()
    if os.name != "nt" and not args.emulator:
        parser.error("native Windows is required unless --emulator is specified")
    if not args.probe.is_file() or not args.injector.is_file():
        parser.error("the built probe and injector must exist")
    if args.callback_probe and not args.callback_probe.is_file():
        parser.error("the callback probe must exist")
    if args.mode == "callbacks" and not args.callback_probe:
        parser.error("--mode callbacks requires --callback-probe")
    digests = {str(args.probe.resolve()): checksum(args.probe)}
    if args.callback_probe:
        digests[str(args.callback_probe.resolve())] = checksum(args.callback_probe)
    prefix = [str(args.emulator)] if args.emulator else []
    modes = [mode for mode in FIXTURES if mode != "callbacks" or args.callback_probe] if args.mode == "all" else [args.mode]
    source = Path(__file__).resolve().parents[1] / "tests" / "runtime"
    results = []
    with tempfile.TemporaryDirectory(prefix="vmhook-observation-") as temporary:
        directory = Path(temporary)
        target = str(directory) if os.name == "nt" else "Z:" + str(directory).replace("/", "\\")
        sources = [str(source / (FIXTURES[mode] + ".java")) for mode in modes]
        subprocess.run(prefix + [str(args.javac), "-d", target, *sources], check=True, timeout=90)
        for mode in modes:
            results.append(execute_mode(args, mode, directory))
    if any(checksum(Path(path)) != digest for path, digest in digests.items()):
        raise RuntimeError("probe DLL changed while its fixtures were running")
    if args.evidence:
        args.evidence.parent.mkdir(parents=True, exist_ok=True)
        args.evidence.write_text(json.dumps({"native_windows": os.name == "nt",
                                            "java": str(args.java), "probe": str(args.probe.resolve()),
                                            "probe_sha256": digests, "results": results}, indent=2) + "\n")
    return 0


if __name__ == "__main__":
    try:
        sys.exit(main())
    except (OSError, RuntimeError, subprocess.SubprocessError) as error:
        print(f"Observation fixture failed: {error}", file=sys.stderr)
        sys.exit(1)
