#!/usr/bin/env python3
"""Verify public runtime operations using an ordinary JVM launch by default.

The harness only injects its own Java subprocess. Native Windows is the default;
--emulator permits separate Windows ABI checks through a local Wine wrapper.
The opt-in heap liveness gate case adds its documented test-only JVM flag.
"""

import argparse
import hashlib
import os
from pathlib import Path
import queue
import re
import subprocess
import sys
import tempfile
import threading
import time

# Keep provenance consistent with the independent owned-JVM fixture runners.
sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "tests" / "runtime"))
from runtime_evidence import runtime_evidence, read_results


CHECKS = {
    "coalesced_jit": [
        "context", "registration", "compiled_baseline", "visible_inline_metadata",
        "first_hook", "first_global_invalidation", "direct_first_hook", "inlined_first_hook",
        "indirect_first_hook", "unrelated_recompiled", "caller_recompiled_under_policy", "second_hook",
        "coalesced_code_retention", "coalesced_caller_code_retention", "coalesced_direct_semantics", "coalesced_inlined_semantics",
        "coalesced_indirect_semantics", "rejected_addition_keeps_policy", "remove_second", "remove_first",
        "dormant_policy_reuse", "incomplete_invalidation_guard", "changed_identity_guard", "field_only_policy_guard",
        "restoration_clears_invalidation_proof", "public_incomplete_policy_recovery", "folded_leaf_fallback", "observer_lifecycle",
        "shutdown_restores_policy", "shutdown_recompilation", "coalesced_jit_proof"],
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
                 "joined_close", "parallel_workers", "cancelled_close", "preparation_cpp_oom", "copied_java_failure", "retained_throwable", "context_allocation_coverage", "context_cpp_oom", "context_failure_cleanup", "context_failure_retry", "executor_context_cpp_oom", "executor_task_cpp_oom", "legacy_access_cpp_oom", "legacy_anchor_cpp_oom", "legacy_failure_retry", "class_observer_cpp_oom", "class_observer_failure_retry", "executor_complete"],
    "management": ["passive_preflight", "exact_runtime_identity", "preflight_no_attachment",
                   "memory_observation", "runtime_counters", "real_gc_counters", "memory_pool_observation",
                   "real_allocation_counters", "bounded_allocation_snapshot", "java_thread_identity",
                   "compiled_java_stack", "zero_depth", "bounded_snapshot", "context_closed",
                   "copied_observations_survive_close", "management_complete"],
    "lifetime": ["metadata", "native_attach", "ambiguous_name", "loader_binding", "constructor_identity",
                 "null_contract", "context_loader", "source_closed", "cross_context_gc",
                 "released_resource", "proxy_binding_lifetime", "loader_heap_snapshot", "rebound_heap_snapshot", "root_accounting", "builtin_identity", "builtin_heap", "empty_array_loader_identity", "loader_array_heap_snapshot", "unregister", "lifetime_complete"],
    "class_inventory": ["metadata", "native_attach", "inventory_bounds", "duplicate_loaders",
                        "rooted_mirrors", "inventory_context", "inventory_complete"],
    "runtime_init": ["fresh_runtime", "fault_consumed", "passive_preflight",
                     "native_context_result", "registration_result", "observer_result",
                     "failure_cleanup", "runtime_init_complete"],
    "monitor_fault": ["monitor_cpp_allocation", "monitor_pending_cleanup",
                      "monitor_fault_balance", "monitor_fault_retry", "native_complete"],
    "heap_liveness": ["heap_liveness", "native_complete"],
    "heap_concurrent": ["heap_concurrent_ready", "mark_completed", "heap_concurrent", "native_complete"],
    "special_reference": ["reference_read_refusal", "reference_write_refusal", "reference_atomic_refusal",
                          "reference_raw_refusal", "reference_hook_refusal", "ordinary_referent",
                          "reference_java_api", "native_complete"],
    "hook_roots": ["context", "preparation_cpp_oom", "preparation_failure_cleanup", "preparation_retry",
                   "ticket_cpp_oom", "ticket_failure_cleanup", "method_pin_cpp_oom",
                   "field_binding_cpp_oom", "field_metadata_cpp_oom", "method_policy_cpp_oom",
                   "field_policy_cpp_oom", "failure_cleanup", "method_retry", "method_original",
                   "field_retry", "field_original", "closed", "contextless_cpp_oom",
                   "contextless_retry", "hook_roots_complete"],
    "reference_graph": ["graph_cycles", "graph_arrays", "graph_inheritance",
                        "graph_reference_boundaries", "graph_class_identity", "graph_budgets",
                        "graph_cpp_allocation", "graph_resources", "graph_gc", "graph_refusal", "native_complete"],
    "direct_array": ["direct_array_batch", "direct_array_cpp_allocation",
                     "direct_array_recovery", "direct_array_refusal",
                     "direct_array_string_policy", "native_complete"],
    "argument_descriptor": ["exact_descriptors", "string_constructor_upcasts",
                            "array_constructor_upcasts", "nested_constructor",
                            "collection_constructor_upcasts", "descriptor_refusals",
                            "inferred_hook_arguments", "descriptor_complete"],
    "method_binding": ["context", "registration", "two_loader_identity",
                       "wrong_loader_receiver_refusal", "wrong_loader_argument_refusal",
                       "wrong_loader_array_refusal", "base_to_subtype_refusal", "hook_a",
                       "captured_rebinding", "hook_b", "array_argument_scope", "nested_binding_scope",
                       "callback_constructor_scope", "java_null_wrapper", "callback_tls_restored",
                       "original_after_removal", "reclamation", "method_binding_proof"],
    "method_exit": ["context", "registration", "observers", "original_results_and_monitors",
                    "all_primitive_bits", "reference_kind_only", "empty_void_original",
                    "exception_has_no_normal_exit", "resident_stubs", "composition",
                    "same_event_identity", "explicit_removal", "queue_capture_reclaimed",
                    "bounded_drops", "forced_results", "forced_origin_distinguished",
                    "original_after_forced", "nested_forced_result", "nested_origins",
                    "new_thread", "virtual_refusal", "concurrent_gc", "concurrent_records",
                    "origin_saturation", "forced_decision_saturation", "shutdown",
                    "shutdown_original", "method_exit_proof"],
    "event_stream": ["context", "queued_observer", "deferred_registration", "initial_wait",
                     "copied_class_record", "deferred_native_install", "deferred_callback",
                     "deferred_removal", "deferred_original_body", "deferred_reclamation",
                     "throwable_observer", "exact_stack_trace_semantics", "throwable_removal",
                     "shared_nonsynchronized_observer", "shared_cds_physical_stub",
                     "shared_nonsynchronized_body", "shared_nonsynchronized_removal", "synchronized_entry_gc",
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
    "coalesced_jit": "CoalescedJitFixture",
    "jit": "ObservationJitFixture",
    "fields": "ObservationFieldFixture",
    "classes": "ObservationClassFixture",
    "callbacks": "ObservationCallbackFixture",
    "hook_composition": "HookCompositionFixture",
    "lifetime": "LifetimeFixture",
    "class_inventory": "LifetimeFixture",
    "runtime_init": "ExecutorFixture",
    "monitor_fault": "NativeFixture",
    "heap_liveness": "NativeFixture",
    "heap_concurrent": "HeapConcurrentFixture",
    "special_reference": "SpecialReferenceFixture",
    "hook_roots": "RootFixture",
    "method_binding": "MethodBindingFixture",
    "argument_descriptor": "ArgumentDescriptorFixture",
    "reference_graph": "ReferenceGraphFixture",
    "direct_array": "DirectArrayFixture",
    "method_exit": "MethodExitFixture",
    "event_stream": "EventStreamFixture",
    "management": "ManagementFixture",
    "executor": "ExecutorFixture",
    "callback_throw": "CallbackThrowFixture",
    "virtual_thread": "VirtualThreadFixture",
}


OPTIONAL_PROBES = {
    "coalesced_jit": "coalesced_jit_probe",
    "callbacks": "callback_probe",
    "hook_composition": "composition_probe",
    "lifetime": "lifetime_probe",
    "class_inventory": "class_inventory_probe",
    "runtime_init": "runtime_init_probe",
    "monitor_fault": "monitor_fault_probe",
    "heap_liveness": "heap_liveness_probe",
    "heap_concurrent": "heap_concurrent_probe",
    "special_reference": "special_reference_probe",
    "hook_roots": "hook_roots_probe",
    "method_binding": "method_binding_probe",
    "argument_descriptor": "argument_descriptor_probe",
    "reference_graph": "reference_graph_probe",
    "direct_array": "direct_array_probe",
    "method_exit": "method_exit_probe",
    "event_stream": "event_stream_probe",
    "management": "management_probe",
    "executor": "executor_probe",
    "callback_throw": "callback_throw_probe",
    "virtual_thread": "virtual_thread_probe",
}
TERMINAL_CHECK = {
    "coalesced_jit": "coalesced_jit_proof",
    "callbacks": "callback_proof",
    "callback_throw": "callback_throw_proof",
    "virtual_thread": "virtual_thread_proof",
    "hook_composition": "hook_composition_proof",
    "event_stream": "event_stream_proof",
    "class_inventory": "inventory_complete",
    "runtime_init": "runtime_init_complete",
    "monitor_fault": "native_complete",
    "heap_liveness": "native_complete",
    "heap_concurrent": "native_complete",
    "special_reference": "native_complete",
    "hook_roots": "hook_roots_complete",
    "method_binding": "method_binding_proof",
    "argument_descriptor": "descriptor_complete",
    "reference_graph": "native_complete",
    "direct_array": "native_complete",
    "method_exit": "method_exit_proof",
}


def checksum(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def report_failure(args, mode, directory, result, pid, error, process_alive=False):
    """Keep the last native checks and the JVM's fatal report on a failed run."""
    values = {}
    if result.is_file():
        values = read_results(result)
        for key, value in values.items():
            print(f"{mode}.{key}={value}", flush=True)
    args.evidence_report.update({"failed_mode": mode,
                                 "runtime_init_fault": args.runtime_init_fault,
                                 "heap_liveness_class_unloading_disabled": args.heap_liveness_class_unloading_disabled,
                                 "partial_checks": values})
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
                       VMHOOK_RUNTIME_INIT_FAULT=args.runtime_init_fault,
                       VMHOOK_TEST_HEAP_REFUSAL="1" if args.heap_liveness_expect_refusal else "0",
                       VMHOOK_MANAGEMENT_REPORT=windows_path(result),
                       VMHOOK_CALLBACK_THROW_REPORT=windows_path(result),
                       VMHOOK_METHOD_EXIT_REPORT=windows_path(result),
                       VMHOOK_METHOD_BINDING_REPORT=windows_path(result),
                       VMHOOK_COALESCED_JIT_REPORT=windows_path(result),
                       VMHOOK_VIRTUAL_THREAD_REPORT=windows_path(result))
    java_flags = (["-XX:-ClassUnloadingWithConcurrentMark"]
                  if mode == "heap_liveness" and args.heap_liveness_class_unloading_disabled else [])
    command = prefix + [str(args.java), *java_flags, "-cp", windows_path(directory), FIXTURES[mode]]
    process = subprocess.Popen(command, env=environment, stdout=subprocess.PIPE,
                               stderr=subprocess.STDOUT, text=True, cwd=directory)
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
                values = read_results(result)
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
        if mode == "heap_concurrent":
            captures = values.get("satb_active_captures", "")
            rooted = values.get("rooted_captures", "")
            if not captures.isdecimal() or int(captures) < 1:
                failed.append("satb_active_captures")
            if rooted != "64":
                failed.append("rooted_captures")
        if failed:
            raise RuntimeError("failed " + mode + " checks: " + ", ".join(sorted(set(failed))))
        return {"mode": mode, "checks": values, "java_output": captured,
                "runtime_init_fault": args.runtime_init_fault if mode == "runtime_init" else None,
                "java_arguments": [*java_flags, "-cp", "<compiled owned fixture>", FIXTURES[mode]],
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
        args.evidence_report["java_output"].extend(captured)
        args.evidence_report["failed_java_arguments"] = [*java_flags, "-cp", "<compiled owned fixture>", FIXTURES[mode]]
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
    parser.add_argument("--runtime-init-fault", default="path",
                        choices=["path", "profile", "diagnostic", "persistent_path", "unsupported", "builtin"],
                        help="select the fresh-runtime fault case; runtime_init mode only")
    parser.add_argument("--heap-liveness-class-unloading-disabled", action="store_true",
                        help="test-only heap_liveness launch with class unloading disabled; ordinary modes keep default JVM arguments")
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
    args.heap_liveness_expect_refusal = False
    if args.heap_liveness_class_unloading_disabled:
        if args.mode != "heap_liveness":
            parser.error("the test-only class-unloading option requires --mode heap_liveness")
        feature = java_feature(args.java, prefix)
        if feature < 9:
            parser.error("the class-unloading gate case requires Java 9 or newer")
        args.heap_liveness_expect_refusal = feature <= 16
    if "virtual_thread" in modes or "heap_concurrent" in modes:
        feature = java_feature(args.java, prefix)
        if "virtual_thread" in modes and feature < 21:
            if args.mode == "virtual_thread":
                parser.error("virtual_thread requires Java 21 or newer without preview arguments")
            modes.remove("virtual_thread")
            print(f"virtual_thread skipped: Java {feature} is below the stable Java 21 API", flush=True)
        if "heap_concurrent" in modes and feature not in (21, 27):
            if args.mode == "heap_concurrent":
                parser.error("heap_concurrent requires the exact shipped Java 21 or 27 diagnostic image")
            modes.remove("heap_concurrent")
            print(f"heap_concurrent skipped: Java {feature} has no active-mark diagnostic witness", flush=True)
    source = Path(__file__).resolve().parents[1] / "tests" / "runtime"
    results = []
    evidence = runtime_evidence(args.evidence, "observation fixtures", args.emulator)
    evidence.report.update({"java": str(args.java), "probe": str(args.probe.resolve()),
                            "probe_sha256": digests, "results": results})
    # Every successful mode records its actual arguments in results below.
    del evidence.report["java_arguments"]
    args.evidence_report = evidence.report
    with tempfile.TemporaryDirectory(prefix="vmhook-observation-") as temporary:
        directory = Path(temporary)
        passed = False
        failure = None
        try:
            evidence.add_runtime(str(args.java), str(args.javac), args.injector, args.probe)
            evidence.add_input("observation_runner", Path(__file__))
            evidence.add_input("evidence_helper", source / "runtime_evidence.py")
            for mode in modes:
                attribute = OPTIONAL_PROBES.get(mode)
                if attribute:
                    evidence.add_input(mode + "_probe", getattr(args, attribute))
            target = str(directory) if os.name == "nt" else "Z:" + str(directory).replace("/", "\\")
            fixture_names = dict.fromkeys(FIXTURES[mode] for mode in modes)
            sources = [source / (name + ".java") for name in fixture_names]
            for fixture in sources:
                evidence.add_input(fixture.stem + "_source", fixture)
            if "coalesced_jit" in modes:
                evidence.add_input("coalesced_jit_native_source", source / "coalesced_jit_probe.cpp")
                evidence.add_input("coalesced_jit_canonical_source", source.parents[1] / "modules" / "vmhook.ixx")
            compilation = subprocess.run(prefix + [str(args.javac), "-d", target, *map(str, sources)],
                                         stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                                         text=True, timeout=90)
            evidence.report["java_compiler_output"] = compilation.stdout
            if compilation.stdout:
                print(compilation.stdout, end="", flush=True)
            compilation.check_returncode()
            evidence.add_classes(directory)
            for mode in modes:
                result = execute_mode(args, mode, directory)
                results.append(result)
                evidence.report["java_output"].extend(result["java_output"])
            if any(checksum(Path(path)) != digest for path, digest in digests.items()):
                raise RuntimeError("probe DLL changed while its fixtures were running")
            passed = True
            return 0
        except Exception as error:
            failure = error
            raise
        finally:
            evidence.finish(passed, failure, directory)



if __name__ == "__main__":
    try:
        sys.exit(main())
    except (OSError, RuntimeError, subprocess.SubprocessError) as error:
        print(f"Observation fixture failed: {error}", file=sys.stderr)
        sys.exit(1)
