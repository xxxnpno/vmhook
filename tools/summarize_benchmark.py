#!/usr/bin/env python3
"""Summarize measured benchmark phases; preserve environment and limitations."""

from __future__ import annotations

import argparse
import json
from pathlib import Path


def summarize(evidence: dict) -> dict:
    runs = [run for run in evidence["runs"] if run["name"] == "benchmark"]
    if len(runs) != 1:
        raise ValueError("expected exactly one benchmark run")
    metrics = runs[0]["checks_and_metrics"]
    if metrics.get("complete") != "pass":
        raise ValueError("only complete successful benchmark evidence can be summarized")
    comparisons = {}
    for workload in ("target", "unrelated"):
        phases = {}
        for phase in ("before_hook", "active_hook", "after_shutdown"):
            prefix = f"java_{workload}_loop_{phase}"
            phases[phase] = {name: int(metrics[f"{prefix}.{name}"])
                             for name in ("first_ns", "samples", "repeated_p50_ns", "repeated_p99_ns")}
        baseline = phases["before_hook"]["repeated_p50_ns"]
        comparisons[workload] = {"phases": phases,
                                "active_to_before_p50_ratio": phases["active_hook"]["repeated_p50_ns"] / baseline if baseline else None,
                                "after_to_before_p50_ratio": phases["after_shutdown"]["repeated_p50_ns"] / baseline if baseline else None}
    return {"schema": 1, "native_windows": evidence["native_windows"],
            "scope": "fixed-workload observations; no statistical significance or application-wide overhead claim",
            "performance_evidence": "native" if evidence["native_windows"] else "provisional emulator execution",
            "host": evidence.get("host"), "dll_sha256": runs[0]["sha256"],
            "jvm_image_sha256": evidence.get("jvm_image_sha256"),
            "parameters": {key: evidence.get(key) for key in ("iterations", "soak_cycles", "soak_batch")},
            "jit_transitions": {key: int(value) for key, value in metrics.items() if key.startswith("jit_")},
            "install_wall_ns": int(metrics["method_hook.install_wall_ns"]),
            "shutdown_wall_ns": int(metrics["method_hook.shutdown_wall_ns"]),
            "java_operations_per_sample": int(metrics["java_loop.operations_per_sample"]),
            "loop_comparisons": comparisons,
            "root_resources": {key: int(value) for key, value in metrics.items() if key.startswith("roots_")},
            "soak": {key: value for key, value in metrics.items()
                     if key.startswith(("soak.", "soak_before.", "soak_after."))}}


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("evidence", type=Path)
    parser.add_argument("--output", type=Path)
    args = parser.parse_args()
    summary = summarize(json.loads(args.evidence.read_text(encoding="utf-8")))
    text = json.dumps(summary, indent=2) + "\n"
    if args.output:
        args.output.parent.mkdir(parents=True, exist_ok=True)
        args.output.write_text(text, encoding="utf-8")
    print(text, end="")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
