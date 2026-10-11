#!/usr/bin/env python3
"""Inspect a Windows HotSpot DLL without loading or executing it.

An exact manifest match is an ABI profile, not proof of the active collector,
target-method support, or a completed runtime test. Use runtime_capabilities()
inside the JVM for those preflight decisions.
"""
from __future__ import annotations

import argparse
import json
from pathlib import Path
import sys

import build_private_profiles as profiles
import private_profiles as pp

ROOT = Path(__file__).resolve().parents[1]


def inspect_image(dll: Path, manifest: dict, pdb: Path | None = None) -> dict:
    image = pp.pe_image(dll)
    identity = {**image.identity, "machine": image.machine, "image_size": image.image_size,
                "pe_timestamp": image.timestamp, "sha256": image.sha256}
    matches = [entry for entry in manifest["profiles"]
               if all(entry["identity"].get(key) == value for key, value in identity.items())]
    if len(matches) > 1:
        raise pp.profile_error("Ambiguous exact DLL identity in the profile manifest")
    match = matches[0] if matches else None
    report = {"schema": 1, "dll": str(dll.resolve()), "identity": identity,
              "exact_profile_match": match is not None,
              "java_feature": match["java_feature"] if match else None,
              "build": match["build"] if match else None,
              "resolution": match.get("resolution") if match else None,
              "publisher_release": match["publisher_release"] if match else None,
              "pdb": None,
              "runtime_validation": "Not performed: no DLL was loaded and no JVM was started",
              "next_step": "Call vmhook::runtime_capabilities() inside the target JVM"
                  if match else "Provide the matching full PDB, extract and validate an exact profile"}
    if pdb is not None:
        symbols = pp.pdb_file(pdb)
        if symbols.identity != image.identity:
            raise pp.profile_error("PDB/DLL identity mismatch")
        report["pdb"] = {"path": str(pdb.resolve()), "identity": symbols.identity,
                         "matches_dll": True,
                         "abi_layout_validation": "Not performed by this identity inspector"}
    return report


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--manifest", type=Path, default=ROOT / "data/native_profiles.json")
    parser.add_argument("--dll", type=Path)
    parser.add_argument("--pdb", type=Path)
    parser.add_argument("--list", action="store_true", help="list pinned exact builds")
    parser.add_argument("--output", type=Path, help="optional JSON output file")
    args = parser.parse_args()
    if args.list == bool(args.dll):
        parser.error("choose exactly one of --list or --dll")
    if args.pdb and not args.dll:
        parser.error("--pdb requires --dll")
    try:
        manifest = profiles.load_manifest(args.manifest)
        if args.list:
            report = {"schema": 1, "target": manifest["target"],
                      "profiles": [{key: entry[key] for key in
                                    ("java_feature", "build", "identity", "resolution",
                                     "publisher_release", "runtime_source")}
                                   for entry in manifest["profiles"]],
                      "scope": "Exact DLL profiles; runtime collector and target checks still apply"}
        else:
            report = inspect_image(args.dll, manifest, args.pdb)
        text = json.dumps(report, indent=2, sort_keys=True) + "\n"
        if args.output:
            args.output.parent.mkdir(parents=True, exist_ok=True)
            args.output.write_text(text, encoding="utf-8")
        else:
            print(text, end="")
        return 0 if args.list or report["exact_profile_match"] else 2
    except (OSError, ValueError, KeyError) as error:
        print("inspect_runtime: " + str(error), file=sys.stderr)
        return 1


if __name__ == "__main__":
    sys.exit(main())
