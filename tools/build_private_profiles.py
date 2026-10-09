#!/usr/bin/env python3
"""Reproduce the Windows HotSpot private profiles from publisher artifacts.

The manifest pins each DLL by SHA256 and CodeView identity. Java 9/10 use
documented exact-image audit facts, checked against offline VM metadata and
function bytes. Other builds use their matching publisher PDB. Downloads and
generated intermediates belong in the build/cache directory, outside sources.
"""
from __future__ import annotations

import argparse
import hashlib
import json
from pathlib import Path, PurePosixPath
import shutil
import sys
import urllib.request
import uuid
import zipfile

import private_profiles as pp

ROOT = Path(__file__).resolve().parents[1]
CAPABILITIES = ("new_attaching_factory", "native_transition_async_arg", "cleanup_bool_arg",
                "delete_flags_arg", "guards_subobject", "monitor_owner", "jfr_before_publish",
                "java_calls_by_value", "anchor_thread_arg")


def load_manifest(path: Path) -> dict:
    manifest = json.loads(path.read_text(encoding="utf-8"))
    if manifest.get("schema") != 1:
        raise pp.ProfileError("Unsupported private-profile manifest schema")
    if [entry["java_feature"] for entry in manifest["profiles"]] != list(range(8, 28)):
        raise pp.ProfileError("Manifest must contain exactly one ordered Java 8–27 profile")
    for entry in manifest["profiles"]:
        if entry["identity"]["java_feature"] != entry["java_feature"]:
            raise pp.ProfileError("Manifest Java feature identity mismatch")
        uuid.UUID(entry["identity"]["guid"])
        if len(entry["identity"]["sha256"]) != 64:
            raise pp.ProfileError("Manifest DLL SHA256 is invalid")
    for names in manifest["function_aliases"].values():
        if any("JvmtiExport" in name or "AttachCurrentThread" in name or "DetachCurrentThread" in name for name in names):
            raise pp.ProfileError("JNI/JVMTI entry points cannot be private-profile aliases")
    return manifest


def download(url: str, destination: Path, sha256: str | None = None) -> Path:
    if not url.startswith("https://"):
        raise pp.ProfileError("Publisher artifact URL must use HTTPS")
    destination.parent.mkdir(parents=True, exist_ok=True)
    if not destination.is_file():
        temporary = destination.with_suffix(destination.suffix + ".part")
        print("Downloading " + url, file=sys.stderr, flush=True)
        with urllib.request.urlopen(url, timeout=90) as response, temporary.open("wb") as output:
            shutil.copyfileobj(response, output)
        temporary.replace(destination)
    if sha256:
        digest = hashlib.sha256()
        with destination.open("rb") as archive:
            for chunk in iter(lambda: archive.read(1024 * 1024), b""):
                digest.update(chunk)
        if digest.hexdigest() != sha256:
            raise pp.ProfileError("Publisher archive SHA256 mismatch: " + str(destination))
    return destination


def extract_zip(archive: Path, destination: Path, selected=None) -> None:
    destination.mkdir(parents=True, exist_ok=True)
    with zipfile.ZipFile(archive) as source:
        for entry in source.infolist():
            if selected and not selected(entry.filename):
                continue
            relative = PurePosixPath(entry.filename)
            if relative.is_absolute() or ".." in relative.parts or any(":" in part or "\\" in part for part in relative.parts):
                raise pp.ProfileError("Unsafe publisher archive member")
            target = destination.joinpath(*relative.parts)
            if entry.is_dir():
                target.mkdir(parents=True, exist_ok=True)
            else:
                target.parent.mkdir(parents=True, exist_ok=True)
                with source.open(entry) as reader, target.open("wb") as writer:
                    shutil.copyfileobj(reader, writer)


def verify_image(path: Path, entry: dict) -> pp.PeImage:
    image = pp.PeImage(path)
    actual = {**image.identity, "image_size": image.image_size, "pe_timestamp": image.timestamp,
              "machine": image.machine, "sha256": image.sha256}
    for key, value in actual.items():
        if entry["identity"].get(key) != value:
            raise pp.ProfileError(f"Java {entry['java_feature']} DLL {key} differs from the manifest")
    return image


def prepare_jdk(entry: dict, cache: Path, archive_sha: str | None = None) -> tuple[Path, Path]:
    directory = cache / str(entry["java_feature"]) / "jdk"
    candidates = list(directory.glob("**/bin/server/jvm.dll"))
    if not candidates:
        archive = download(entry["product_download"], cache / str(entry["java_feature"]) / "jdk.zip", archive_sha)
        extract_zip(archive, directory)
        candidates = list(directory.glob("**/bin/server/jvm.dll"))
    matches = []
    for dll in candidates:
        try:
            verify_image(dll, entry)
            matches.append(dll)
        except pp.ProfileError:
            continue
    if len(matches) != 1:
        raise pp.ProfileError("Publisher archive must contain exactly one pinned server JVM")
    roots = [java.parent.parent for java in directory.glob("**/bin/java.exe")
             if (java.parent / "javac.exe").is_file()]
    if len(roots) != 1:
        raise pp.ProfileError("Publisher archive must contain exactly one JDK with javac")
    return roots[0].resolve(), matches[0].resolve()


def prepare_pdb(entry: dict, cache: Path) -> Path:
    directory = cache / str(entry["java_feature"]) / "symbols"
    candidates = list(directory.glob("**/jvm*.pdb"))
    if not candidates:
        archive = download(entry["symbols_download"], cache / str(entry["java_feature"]) / "symbols.zip")
        extract_zip(archive, directory, lambda name: PurePosixPath(name).name in ("jvm.pdb", "jvm.dll.pdb"))
        candidates = list(directory.glob("**/jvm*.pdb"))
    matches = [pdb for pdb in candidates if pp.Pdb(pdb).identity == {
        "guid": entry["identity"]["guid"], "age": entry["identity"]["age"]}]
    if len(matches) != 1:
        raise pp.ProfileError("Publisher symbols must contain exactly one matching JVM PDB")
    return matches[0]


def prepare_dll(entry: dict, cache: Path, archive_sha: str | None = None) -> Path:
    """Profile reproduction needs the DLL only, not an installed JDK."""
    roots = (cache / str(entry["java_feature"]) / "jdk", cache / str(entry["java_feature"]) / "image")
    candidates = [dll for root in roots for dll in root.glob("**/bin/server/jvm.dll")]
    if not candidates:
        archive = download(entry["product_download"], cache / str(entry["java_feature"]) / "jdk.zip", archive_sha)
        extract_zip(archive, roots[1], lambda name: name.endswith("/bin/server/jvm.dll"))
        candidates = list(roots[1].glob("**/bin/server/jvm.dll"))
    matches = []
    for dll in candidates:
        try:
            verify_image(dll, entry)
            matches.append(dll)
        except pp.ProfileError:
            continue
    if len(matches) != 1:
        raise pp.ProfileError("Publisher archive must contain exactly one pinned JVM DLL")
    return matches[0]


def audited_profile(entry: dict, recipe: dict, dll: Path) -> dict:
    image = verify_image(dll, entry)
    if recipe["identity"] != entry["identity"]:
        raise pp.ProfileError("Audit recipe differs from pinned image identity")
    metadata = image.metadata()
    for alias, prefix in recipe["code_prefixes"].items():
        expected = bytes.fromhex(prefix)
        if image.read(recipe["aliases"][alias], len(expected)) != expected:
            raise pp.ProfileError("Exact-image function fingerprint mismatch: " + alias)
    types = {}
    for name, facts in recipe["types"].items():
        observed = metadata.get(name)
        if observed and observed["size"] and observed["size"] != facts["size"]:
            raise pp.ProfileError("Audited VMType size disagrees with DLL: " + name)
        for field, offset in facts["members"].items():
            if observed and field in observed["members"] and observed["members"][field]["offset"] != offset:
                raise pp.ProfileError("Audited VMStruct offset disagrees with DLL: " + name + "::" + field)
        types[name] = {"size": facts["size"], "members": {
            field: {"offset": offset, "type_index": 0} for field, offset in facts["members"].items()},
            "enums": facts["enums"], "bases": []}
    return {"schema": 1, "identity": recipe["identity"], "symbols": {}, "aliases": recipe["aliases"],
            "capabilities": recipe["capabilities"], "types": types}


def resolve_profile(profile: dict, manifest: dict) -> dict:
    aliases = manifest["function_aliases"]
    selected = dict(profile.get("aliases", {}))
    for key, names in aliases.items():
        matches = [name for name in names if name in profile["symbols"]]
        if matches:
            selected[key] = profile["symbols"][matches[0]]
    for forbidden in ("should_post_thread_life", "post_thread_start"):
        selected.pop(forbidden, None)
    symbols, types = profile["symbols"], profile["types"]
    caps = {
        "new_attaching_factory": bool(selected.get("create_attaching_thread")),
        "native_transition_async_arg": any(name.endswith("@@_N@Z") for name in aliases["native_to_vm"] if name in symbols),
        "cleanup_bool_arg": aliases["cleanup_failed"][0] in symbols,
        "delete_flags_arg": aliases["delete_thread"][0] not in symbols,
        "guards_subobject": "StackOverflow" in types,
        "monitor_owner": bool(types["JavaThread"]["members"].get("_monitor_owner_id")),
        "jfr_before_publish": profile["identity"]["java_feature"] >= 23,
        "java_calls_by_value": any("@@VmethodHandle@@" in name for name in aliases["java_calls"] if name in symbols),
        "anchor_thread_arg": "?make_walkable@JavaFrameAnchor@@QEAAXPEAVJavaThread@@@Z" in symbols,
    }
    if not symbols:
        caps.update(profile["capabilities"])
    profile["aliases"], profile["capabilities"] = selected, caps
    return profile


def validate_vm_metadata(profile: dict, dll: Path, manifest: dict) -> None:
    """Cross-check PDB facts wherever the DLL publishes the same fact."""
    metadata = pp.PeImage(dll).metadata()
    for name, facts in profile["types"].items():
        observed = metadata.get(name)
        if not observed:
            continue
        if facts["size"] and observed["size"] and facts["size"] != observed["size"]:
            raise pp.ProfileError("PDB/VMType size disagreement: " + name)
        for field in manifest["required_fields"].get(name, []):
            if field in facts["members"] and field in observed["members"]:
                if facts["members"][field]["offset"] != observed["members"][field]["offset"]:
                    raise pp.ProfileError("PDB/VMStruct field disagreement: " + name + "::" + field)


def cpp_profiles(profiles: list[dict], manifest: dict) -> str:
    result = ["struct symbol_entry{std::string_view name;std::uint32_t rva;};",
              "struct field_entry{std::string_view type,name;std::size_t offset;};",
              "struct size_entry{std::string_view type;std::size_t size;};",
              "struct enum_entry{std::string_view type,name;int value;};",
              "struct capabilities{bool " + ",".join(CAPABILITIES) + ";};",
              "struct profile_entry{std::array<unsigned char,16> guid;std::uint32_t age,image_size,pe_timestamp;int java_feature;capabilities caps;std::span<const symbol_entry> symbols;std::span<const field_entry> fields;std::span<const size_entry> sizes;std::span<const enum_entry> enums;};"]
    for profile in profiles:
        version = str(profile["identity"]["java_feature"])
        items = ["{" + json.dumps(key) + "," + str(value) + "}" for key, value in profile["aliases"].items()]
        result.append("inline constexpr symbol_entry symbols_" + version + "[]{" + ",".join(items) + "};")
        fields, sizes, enums = [], [], []
        for name, facts in profile["types"].items():
            sizes.append("{" + json.dumps(name) + "," + str(facts["size"]) + "}")
            for field, info in facts["members"].items():
                if field in manifest["required_fields"].get(name, []):
                    fields.append("{" + json.dumps(name) + "," + json.dumps(field) + "," + str(info["offset"]) + "}")
            for name2, value in facts["enums"].items():
                if name2 in manifest["required_enums"].get(name, []):
                    enums.append("{" + json.dumps(name) + "," + json.dumps(name2) + "," + str(value) + "}")
        result.append("inline constexpr field_entry fields_" + version + "[]{" + ",".join(fields) + "};")
        result.append("inline constexpr size_entry sizes_" + version + "[]{" + ",".join(sizes) + "};")
        result.append("inline constexpr enum_entry enums_" + version + "[]{" + ",".join(enums) + "};")
        identity = profile["identity"]
        result += ["inline constexpr profile_entry profile_" + version + "{",
                   "{" + ",".join(str(value) for value in uuid.UUID(identity["guid"]).bytes_le) + "}," +
                   str(identity["age"]) + "," + str(identity["image_size"]) + "," + str(identity["pe_timestamp"]) + "," + version + ",",
                   "{" + ",".join(str(int(profile["capabilities"][key])) for key in CAPABILITIES) + "},",
                   "symbols_" + version + ",fields_" + version + ",sizes_" + version + ",enums_" + version + "};"]
    result.append("inline constexpr const profile_entry* all_profiles[]{" + ",".join(
        "&profile_" + str(profile["identity"]["java_feature"]) for profile in profiles) + "};")
    return "\n".join(result) + "\n"


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--manifest", type=Path, default=ROOT / "data/native_profiles.json")
    parser.add_argument("--audits", type=Path, default=ROOT / "data/native_audits.json")
    parser.add_argument("--cache", type=Path, default=ROOT / "build/private-profiles")
    parser.add_argument("--prepare-jdk", type=int, choices=range(8, 28))
    parser.add_argument("--github-env", type=Path)
    parser.add_argument("--input-profiles", type=Path, help="reuse previously extracted profileN.json files")
    parser.add_argument("--output", type=Path, help="write a preprocessor-free C++ profile fragment")
    parser.add_argument("--check", type=Path, help="compare the generated fragment with a module or fragment")
    args = parser.parse_args()
    try:
        manifest = load_manifest(args.manifest)
        audit_list = json.loads(args.audits.read_text(encoding="utf-8"))["profiles"]
        audits = {entry["identity"]["java_feature"]: entry for entry in audit_list}
        if args.prepare_jdk:
            entry = next(entry for entry in manifest["profiles"] if entry["java_feature"] == args.prepare_jdk)
            sha = audits.get(args.prepare_jdk, {}).get("provenance", {}).get("product_sha256")
            home, _ = prepare_jdk(entry, args.cache, sha)
            print("JAVA_HOME=" + str(home))
            if args.github_env:
                with args.github_env.open("a", encoding="utf-8") as environment:
                    environment.write("JAVA_HOME=" + str(home) + "\n")
            return 0
        if not args.output and not args.check:
            parser.error("use --prepare-jdk, --output, or --check")
        profiles = []
        for entry in manifest["profiles"]:
            version = entry["java_feature"]
            if args.input_profiles:
                profile = json.loads((args.input_profiles / f"profile{version}.json").read_text(encoding="utf-8"))
                if profile["identity"] != entry["identity"]:
                    raise pp.ProfileError("Cached profile identity mismatch")
            else:
                dll = prepare_dll(entry, args.cache, audits.get(version, {}).get("provenance", {}).get("product_sha256"))
                if version in audits:
                    profile = audited_profile(entry, audits[version], dll)
                else:
                    profile = pp.extract(prepare_pdb(entry, args.cache), dll, version, tuple(manifest["profile_types"]))
                    validate_vm_metadata(profile, dll, manifest)
            profiles.append(resolve_profile(profile, manifest))
        fragment = cpp_profiles(profiles, manifest)
        if args.check:
            existing = args.check.read_text(encoding="utf-8")
            start = existing.find("struct symbol_entry{")
            marker = "inline constexpr const profile_entry* all_profiles[]{"
            end = existing.find("};", existing.find(marker, start))
            if start < 0 or end < 0 or existing[start:end + 2].strip() != fragment.strip():
                raise pp.ProfileError("Embedded native profile data is not reproducible")
            print("All 20 native profile tables match " + str(args.check))
        if args.output:
            args.output.parent.mkdir(parents=True, exist_ok=True)
            args.output.write_text(fragment, encoding="utf-8")
            print("Wrote " + str(args.output))
        return 0
    except (OSError, ValueError, KeyError, zipfile.BadZipFile) as error:
        print("build_private_profiles: " + str(error), file=sys.stderr)
        return 1


if __name__ == "__main__":
    sys.exit(main())
