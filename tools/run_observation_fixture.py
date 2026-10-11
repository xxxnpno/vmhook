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
import re
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
    "virtual_thread": ["context", "registration", "hooks", "real_virtual_thread",
                       "virtual_original_method", "virtual_original_fields", "refusal_count",
                       "platform_callbacks", "removal", "originals_after_remove", "virtual_thread_proof"],
    "callback_throw": ["context", "registration", "hooks", "same_throwable_primitive",
                       "same_throwable_void", "same_throwable_reference", "body_skip_and_finally",
                       "unrelated_object_refused", "java_null_refused", "exception_wins_return", "allocation_failure_propagated",
                       "synchronized_resume_after_gc", "synchronized_early_return_after_gc",
                       "synchronized_instance_throw_after_gc", "removal",
                       "synchronized_original_after_remove", "callback_throw_proof"],
    "executor": ["metadata", "backpressure", "queued_cancel", "worker_context", "queued_deadline",
                 "retained_argument", "retained_result", "running_cancel", "self_wait_refused",
                 "joined_close", "parallel_workers", "cancelled_close", "preparation_cpp_oom", "copied_java_failure", "retained_throwable", "context_allocation_coverage", "context_cpp_oom", "context_failure_cleanup", "context_failure_retry", "executor_task_cpp_oom", "legacy_access_cpp_oom", "legacy_anchor_cpp_oom", "legacy_failure_retry", "executor_complete"],
    "management": ["passive_preflight", "exact_runtime_identity", "preflight_no_attachment",
                   "memory_observation", "runtime_counters", "real_gc_counters", "memory_pool_observation",
                   "real_allocation_counters", "bounded_allocation_snapshot", "java_thread_identity",
                   "compiled_java_stack", "zero_depth", "bounded_snapshot", "context_closed",
                   "copied_observations_survive_close", "management_complete"],
    "lifetime": ["metadata", "native_attach", "ambiguous_name", "loader_binding", "constructor_identity",
                 "null_contract", "context_loader", "source_closed", "cross_context_gc",
                 "released_resource", "proxy_binding_lifetime", "loader_heap_snapshot", "rebound_heap_snapshot", "root_accounting", "builtin_identity", "builtin_heap", "unregister", "lifetime_complete"],
    "class_inventory": ["metadata", "native_attach", "inventory_bounds", "duplicate_loaders",
                        "rooted_mirrors", "inventory_context", "inventory_complete"],
    "event_stream": ["context", "queued_observer", "deferred_registration", "initial_wait",
                     "copied_class_record", "deferred_native_install", "deferred_callback",
                     "deferred_removal", "deferred_original_body", "deferred_reclamation",
                     "throwable_observer", "exact_stack_trace_semantics", "throwable_removal",
                     "class_removal", "class_capture_reclamation", "event_stream_proof"],
    "hook_composition": ["context", "three_observers", "shared_policy", "priority_and_first_return",
                         "remove_one", "remaining_original_body", "remove_remaining", "original_body",
                         "recursive_observer", "suppressed_reentry", "same_thread_suppression",
                         "remove_recursive", "callback_side_removal", "native_reclamation",
                         "disabled_resources", "shutdown_original_body", "shutdown_resources",
                         "resident_return_markers", "remove_object_hook", "resident_markers_after_shutdown",
                         "hook_composition_proof"],
}

FIXTURES = {
    "jit": "ObservationJitFixture",
    "fields": "ObservationFieldFixture",
    "classes": "ObservationClassFixture",
    "callbacks": "ObservationCallbackFixture",
    "hook_composition": "HookCompositionFixture",
    "lifetime": "LifetimeFixture",
    "class_inventory": "LifetimeFixture",
    "event_stream": "EventStreamFixture",
    "management": "ManagementFixture",
    "executor": "ExecutorFixture",
    "callback_throw": "CallbackThrowFixture",
    "virtual_thread": "VirtualThreadFixture",
}


OPTIONAL_PROBES = {
    "callbacks": "callback_probe",
    "hook_composition": "composition_probe",
    "lifetime": "lifetime_probe",
    "class_inventory": "class_inventory_probe",
    "event_stream": "event_stream_probe",
    "management": "management_probe",
    "executor": "executor_probe",
    "callback_throw": "callback_throw_probe",
    "virtual_thread": "virtual_thread_probe",
}
TERMINAL_CHECK = {
    "callbacks": "callback_proof",
    "callback_throw": "callback_throw_proof",
    "virtual_thread": "virtual_thread_proof",
    "hook_composition": "hook_composition_proof",
    "event_stream": "event_stream_proof",
    "class_inventory": "inventory_complete",
}


def checksum(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def report_failure(args, mode, directory, result, pid, error, process_alive=False):
    """Keep the last native checks and the JVM's fatal report on a failed run."""
    values = {}
    if result.is_file():
        values = dict(line.split("=", 1) for line in result.read_text(errors="replace").splitlines()
                      if "=" in line)
        for key, value in values.items():
            print(f"{mode}.{key}={value}", flush=True)
    if args.evidence:
        args.evidence.parent.mkdir(parents=True, exist_ok=True)
        args.evidence.write_text(json.dumps({"native_windows": os.name == "nt",
                                            "java": str(args.java), "failed_mode": mode,
                                            "passed": False, "error": str(error),
                                            "partial_checks": values}, indent=2) + "\n")
    if os.environ.get("GITHUB_ACTIONS") == "true":
        message = f"Observation {mode}: {error}; last native checks: {values}"
        escaped = message.replace("%", "%25").replace("\r", "%0D").replace("\n", "%0A")
        print("::error::" + escaped, flush=True)
    if process_alive and pid and pid.isdecimal():
        executable = args.java.parent / ("jcmd.exe" if args.java.suffix == ".exe" else "jcmd")
        if executable.is_file():
            prefix = [str(args.emulator)] if args.emulator else []
            try:
                diagnostic = subprocess.run(prefix + [str(executable), pid, "Thread.print", "-l"],
                                            capture_output=True, text=True, timeout=10)
                print(f"{mode}: thread diagnostic (exit {diagnostic.returncode})\n"
                      f"{diagnostic.stdout}{diagnostic.stderr}", flush=True)
            except subprocess.TimeoutExpired:
                print(f"{mode}: thread diagnostic did not finish within 10 seconds", flush=True)
            except OSError as diagnostic_error:
                print(f"{mode}: thread diagnostic unavailable: {diagnostic_error}", flush=True)
    # HotSpot's default fatal-error path uses its process working directory.
    # Read only this fixture's PID, preserving the normal JVM launch arguments.
    if pid and pid.isdecimal():
        paths = {directory / f"hs_err_pid{pid}.log", Path.cwd() / f"hs_err_pid{pid}.log"}
        for path in sorted(paths):
            if path.is_file():
                text = path.read_text(errors="replace")
                print(f"{mode}: fatal JVM report {path}\n{text[:100000]}", flush=True)
                if len(text) > 100000:
                    print(f"{mode}: fatal report truncated after 100000 characters", flush=True)


def execute_mode(args, mode, directory):
    result = directory / (mode + "-result.txt")
    control = directory / (mode + "-control")

    def windows_path(path):
        path = Path(path).resolve()
        return str(path) if os.name == "nt" else "Z:" + str(path).replace("/", "\\")

    prefix = [str(args.emulator)] if args.emulator else []
    environment = dict(os.environ, VMHOOK_OBSERVATION_RESULT=windows_path(result),
                       VMHOOK_OBSERVATION_CONTROL=windows_path(control),
                       VMHOOK_OBSERVATION_MODE=mode, VMHOOK_CALLBACK_RESULT=windows_path(result),
                       VMHOOK_HOOK_COMPOSITION_REPORT=windows_path(result),
                       VMHOOK_EVENT_STREAM_REPORT=windows_path(result),
                       VMHOOK_TEST_RESULT=windows_path(result),
                       VMHOOK_MANAGEMENT_REPORT=windows_path(result),
                       VMHOOK_CALLBACK_THROW_REPORT=windows_path(result),
                       VMHOOK_VIRTUAL_THREAD_REPORT=windows_path(result))
    command = prefix + [str(args.java), "-cp", windows_path(directory), FIXTURES[mode]]
    process = subprocess.Popen(command, env=environment, stdout=subprocess.PIPE,
                               stderr=subprocess.STDOUT, text=True)
    output = queue.Queue()
    captured = []

    def read_output():
        for line in process.stdout:
            output.put(line.rstrip())

    reader = threading.Thread(target=read_output, daemon=True)
    reader.start()

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
    pid = None
    try:
        deadline = started + 90
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
        probe = getattr(args, OPTIONAL_PROBES.get(mode, "probe"))
        subprocess.run(prefix + [str(args.injector.resolve()), pid, windows_path(probe)],
                       check=True, timeout=30)
        values = {}
        deadline = time.monotonic() + args.timeout
        while time.monotonic() < deadline:
            next_line(0.1)
            if result.exists():
                values = dict(line.split("=", 1) for line in result.read_text().splitlines()
                              if "=" in line)
                completed = TERMINAL_CHECK.get(mode, mode + "_complete")
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
    except (OSError, RuntimeError, subprocess.SubprocessError) as error:
        if process.poll() is not None:
            reader.join(timeout=0.5)
        while True:
            try:
                line = output.get_nowait()
            except queue.Empty:
                break
            captured.append(line)
            print(f"{mode}: {line}", flush=True)
        report_failure(args, mode, directory, result, pid, error, process.poll() is None)
        raise
    finally:
        process.terminate()
        try:
            process.wait(timeout=5)
        except subprocess.TimeoutExpired:
            process.kill()
            process.wait()



def java_feature(java, prefix):
    """Read the supplied runtime's feature version before choosing Java sources."""
    release = java.resolve().parent.parent / "release"
    if release.is_file():
        version = re.search(r'^JAVA_VERSION="([^"]+)"', release.read_text(encoding="utf-8"), re.MULTILINE)
        if version:
            match = re.match(r"(?:1\.)?(\d+)", version.group(1))
            if match:
                return int(match.group(1))
    # This separate launcher probe does not add a flag to the injected fixture.
    result = subprocess.run(prefix + [str(java), "-version"], check=True, text=True,
                            stdout=subprocess.PIPE, stderr=subprocess.STDOUT, timeout=30)
    match = re.search(r'(?:openjdk|java)\s+version\s+"(?:1\.)?(\d+)', result.stdout)
    if not match:
        raise RuntimeError("cannot identify the supplied Java runtime's feature version")
    return int(match.group(1))

def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--java", type=Path, default=Path("java"))
    parser.add_argument("--javac", type=Path, default=Path("javac"))
    parser.add_argument("--injector", type=Path, required=True)
    parser.add_argument("--probe", type=Path, required=True)
    for mode, attribute in OPTIONAL_PROBES.items():
        parser.add_argument("--" + attribute.replace("_", "-"), type=Path,
                            help="also verify the " + mode.replace("_", " ") + " fixture")
    parser.add_argument("--mode", choices=["all", *FIXTURES], default="all")
    parser.add_argument("--emulator", type=Path)
    parser.add_argument("--timeout", type=int, default=180)
    parser.add_argument("--evidence", type=Path)
    args = parser.parse_args()
    if os.name != "nt" and not args.emulator:
        parser.error("native Windows is required unless --emulator is specified")
    if not args.probe.is_file() or not args.injector.is_file():
        parser.error("the built probe and injector must exist")
    digests = {str(args.probe.resolve()): checksum(args.probe)}
    for mode, attribute in OPTIONAL_PROBES.items():
        probe = getattr(args, attribute)
        if probe:
            if not probe.is_file():
                parser.error("the " + mode + " probe must exist")
            digests[str(probe.resolve())] = checksum(probe)
        elif args.mode == mode:
            parser.error("--mode " + mode + " requires --" + attribute.replace("_", "-"))
    prefix = [str(args.emulator)] if args.emulator else []
    modes = [mode for mode in FIXTURES if mode not in OPTIONAL_PROBES
             or getattr(args, OPTIONAL_PROBES[mode])] if args.mode == "all" else [args.mode]
    if "virtual_thread" in modes:
        feature = java_feature(args.java, prefix)
        if feature < 21:
            if args.mode == "virtual_thread":
                parser.error("virtual_thread requires Java 21 or newer without preview arguments")
            modes.remove("virtual_thread")
            print(f"virtual_thread skipped: Java {feature} is below the stable Java 21 API", flush=True)
    source = Path(__file__).resolve().parents[1] / "tests" / "runtime"
    results = []
    with tempfile.TemporaryDirectory(prefix="vmhook-observation-") as temporary:
        directory = Path(temporary)
        target = str(directory) if os.name == "nt" else "Z:" + str(directory).replace("/", "\\")
        fixture_names = dict.fromkeys(FIXTURES[mode] for mode in modes)
        sources = [str(source / (name + ".java")) for name in fixture_names]
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
