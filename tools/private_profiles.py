#!/usr/bin/env python3
"""Extract and verify private HotSpot layouts from a matching Windows PDB.

No addresses are inferred from a Java version. The DLL's RSDS identity must
match the PDB before any RVA or layout is emitted. Output is data, not an ABI
promise: callers still need the corresponding HotSpot lifecycle adapter.
"""
from __future__ import annotations

import argparse
import hashlib
import json
from pathlib import Path
import struct
import uuid


class ProfileError(ValueError):
    pass


def integer(data: bytes, offset: int, fmt: str = "I") -> int:
    try:
        return struct.unpack_from("<" + fmt, data, offset)[0]
    except struct.error as error:
        raise ProfileError("Truncated PDB or PE record") from error


def spelling(data: bytes, offset: int) -> tuple[str, int]:
    end = data.find(b"\0", offset)
    if end < 0:
        raise ProfileError("Unterminated CodeView string")
    return data[offset:end].decode("utf-8", "strict"), end + 1


def numeric(data: bytes, offset: int) -> tuple[int, int]:
    leaf = integer(data, offset, "H")
    if leaf < 0x8000:
        return leaf, offset + 2
    formats = {0x8000: "b", 0x8001: "h", 0x8002: "H", 0x8003: "i",
               0x8004: "I", 0x8009: "q", 0x800A: "Q"}
    if leaf not in formats:
        raise ProfileError(f"Unsupported integer CodeView leaf {leaf:#x}")
    fmt = formats[leaf]
    return integer(data, offset + 2, fmt), offset + 2 + struct.calcsize(fmt)


class PeImage:
    def __init__(self, path: Path):
        data = path.read_bytes()
        self.data = data
        if data[:2] != b"MZ":
            raise ProfileError("DLL is not a PE image")
        pe = integer(data, 0x3C)
        if data[pe:pe + 4] != b"PE\0\0":
            raise ProfileError("Invalid PE signature")
        self.machine = integer(data, pe + 4, "H")
        if self.machine != 0x8664 or integer(data, pe + 24, "H") != 0x20B:
            raise ProfileError("Only Windows x64 HotSpot images are supported")
        self.timestamp = integer(data, pe + 8)
        self.image_size = integer(data, pe + 24 + 56)
        self.base = integer(data, pe + 24 + 24, "Q")
        self.header_size = integer(data, pe + 24 + 60)
        self.export_directory = integer(data, pe + 24 + 112)
        self.sha256 = hashlib.sha256(data).hexdigest()
        section_count = integer(data, pe + 6, "H")
        section_start = pe + 24 + integer(data, pe + 20, "H")
        self.sections = []
        for i in range(section_count):
            entry = section_start + i * 40
            self.sections.append({"rva": integer(data, entry + 12),
                                  "virtual_size": integer(data, entry + 8),
                                  "raw_size": integer(data, entry + 16),
                                  "raw_offset": integer(data, entry + 20)})

        def file_offset(rva: int, size: int) -> int:
            for section in self.sections:
                delta = rva - section["rva"]
                if 0 <= delta <= section["raw_size"] and size <= section["raw_size"] - delta:
                    result = section["raw_offset"] + delta
                    if result <= len(data) and size <= len(data) - result:
                        return result
            raise ProfileError("CodeView RVA is not backed by DLL bytes")

        directory = pe + 24 + 112 + 6 * 8
        debug_rva, debug_size = integer(data, directory), integer(data, directory + 4)
        debug = file_offset(debug_rva, debug_size)
        identities = []
        for entry in range(debug, debug + debug_size - 27, 28):
            if integer(data, entry + 12) != 2:
                continue
            size, offset = integer(data, entry + 16), integer(data, entry + 24)
            if size < 24 or offset > len(data) or size > len(data) - offset:
                continue
            if data[offset:offset + 4] == b"RSDS":
                identities.append({"guid": str(uuid.UUID(bytes_le=data[offset + 4:offset + 20])),
                                   "age": integer(data, offset + 20)})
        if len(identities) != 1:
            raise ProfileError("DLL must have exactly one RSDS identity")
        self.identity = identities[0]

    def read(self, rva: int, size: int) -> bytes:
        """Read an image RVA, including its zero-filled virtual section tail."""
        if rva < 0 or size < 0 or rva > self.image_size or size > self.image_size - rva:
            raise ProfileError("RVA outside PE image")
        if rva < self.header_size and size <= self.header_size - rva:
            result = self.data[rva:rva + size]
            if len(result) != size:
                raise ProfileError("Truncated PE header")
            return result
        for section in self.sections:
            delta = rva - section["rva"]
            length = max(section["virtual_size"], section["raw_size"])
            if 0 <= delta <= length and size <= length - delta:
                backed = min(size, max(0, section["raw_size"] - delta))
                start = section["raw_offset"] + delta
                result = self.data[start:start + backed]
                if len(result) != backed:
                    raise ProfileError("Truncated PE section")
                return result + bytes(size - backed)
        raise ProfileError("RVA does not belong to a PE section")

    def rva_number(self, rva: int, fmt: str = "Q") -> int:
        return integer(self.read(rva, struct.calcsize(fmt)), 0, fmt)

    def string(self, rva: int) -> str:
        result = bytearray()
        for offset in range(65536):
            byte = self.read(rva + offset, 1)
            if byte == b"\0":
                return result.decode("utf-8", "strict")
            result.extend(byte)
        raise ProfileError("Unterminated PE string")

    def exports(self) -> dict[str, int]:
        table = self.export_directory
        if not table:
            raise ProfileError("PE exports are missing")
        count, function_count = self.rva_number(table + 24, "I"), self.rva_number(table + 20, "I")
        functions = self.rva_number(table + 28, "I")
        names, ordinals = self.rva_number(table + 32, "I"), self.rva_number(table + 36, "I")
        if max(count, function_count) > 1_000_000:
            raise ProfileError("PE export count out of bounds")
        result = {}
        for index in range(count):
            name = self.string(self.rva_number(names + index * 4, "I"))
            ordinal = self.rva_number(ordinals + index * 2, "H")
            if ordinal >= function_count:
                raise ProfileError("PE export ordinal out of bounds")
            result[name] = self.rva_number(functions + ordinal * 4, "I")
        return result

    def metadata(self) -> dict[str, dict]:
        """Read VMTypes/VMStructs offline; no JVM or Wine process is launched."""
        exports = self.exports()

        def exported(name: str) -> int:
            if name not in exports:
                raise ProfileError("Required HotSpot metadata export missing: " + name)
            return self.rva_number(exports[name])

        def rows(table: str, stride: str, limit: int):
            start, step = exported(table) - self.base, exported(stride)
            if not 8 <= step <= 256:
                raise ProfileError("VM metadata stride out of bounds")
            for index in range(limit):
                row = start + index * step
                if not self.rva_number(row):
                    return
                yield row
            raise ProfileError("VM metadata table has no bounded terminator")

        result = {}
        name_offset = exported("gHotSpotVMTypeEntryTypeNameOffset")
        size_offset = exported("gHotSpotVMTypeEntrySizeOffset")
        for row in rows("gHotSpotVMTypes", "gHotSpotVMTypeEntryArrayStride", 10000):
            name = self.string(self.rva_number(row + name_offset) - self.base)
            result[name] = {"size": self.rva_number(row + size_offset), "members": {}, "enums": {}, "bases": []}
        type_offset = exported("gHotSpotVMStructEntryTypeNameOffset")
        name_offset = exported("gHotSpotVMStructEntryFieldNameOffset")
        static_offset = exported("gHotSpotVMStructEntryIsStaticOffset")
        field_offset = exported("gHotSpotVMStructEntryOffsetOffset")
        for row in rows("gHotSpotVMStructs", "gHotSpotVMStructEntryArrayStride", 20000):
            if self.rva_number(row + static_offset, "I"):
                continue
            owner = self.string(self.rva_number(row + type_offset) - self.base)
            field = self.string(self.rva_number(row + name_offset) - self.base)
            result.setdefault(owner, {"size": 0, "members": {}, "enums": {}, "bases": []})["members"][field] = {
                "offset": self.rva_number(row + field_offset), "type_index": 0}
        return result


class Pdb:
    def __init__(self, path: Path):
        data = path.read_bytes()
        if data[:32] != b"Microsoft C/C++ MSF 7.00\r\n\x1aDS\0\0\0":
            raise ProfileError("PDB is not MSF 7")
        block_size = integer(data, 32)
        blocks = integer(data, 40)
        directory_size = integer(data, 44)
        block_map = integer(data, 52)
        if block_size not in (512, 1024, 2048, 4096, 8192) or blocks * block_size > len(data):
            raise ProfileError("Invalid PDB block geometry")

        def read_blocks(indexes: list[int], size: int) -> bytes:
            if size > len(data) or any(index >= blocks for index in indexes):
                raise ProfileError("PDB block index out of bounds")
            return b"".join(data[index * block_size:(index + 1) * block_size]
                            for index in indexes)[:size]

        directory_blocks = (directory_size + block_size - 1) // block_size
        block_index_offset = block_map * block_size
        if directory_blocks * 4 > block_size:
            raise ProfileError("Unsupported multi-page PDB directory block map")
        indexes = [integer(data, block_index_offset + i * 4) for i in range(directory_blocks)]
        directory = read_blocks(indexes, directory_size)
        stream_count = integer(directory, 0)
        if stream_count > 65536 or stream_count > directory_size // 4:
            raise ProfileError("PDB stream count out of bounds")
        sizes = [integer(directory, 4 + i * 4) for i in range(stream_count)]
        if sum(size for size in sizes if size != 0xFFFFFFFF) > len(data):
            raise ProfileError("PDB streams exceed file capacity")
        cursor = 4 + stream_count * 4
        self.streams = []
        for size in sizes:
            count = 0 if size == 0xFFFFFFFF else (size + block_size - 1) // block_size
            indexes = [integer(directory, cursor + i * 4) for i in range(count)]
            cursor += count * 4
            self.streams.append(b"" if size == 0xFFFFFFFF else read_blocks(indexes, size))
        if len(self.streams) < 4 or len(self.streams[1]) < 28:
            raise ProfileError("PDB identity/DBI/TPI streams are missing")
        info = self.streams[1]
        self.identity = {"guid": str(uuid.UUID(bytes_le=info[12:28])), "age": integer(info, 8)}
        tpi = self.streams[2]
        cursor, first, last = integer(tpi, 4), integer(tpi, 8), integer(tpi, 12)
        if last < first or last - first > 10_000_000:
            raise ProfileError("PDB type index range invalid")
        self.records = {}
        self.classes = {}
        for index in range(first, last):
            length = integer(tpi, cursor, "H")
            if length < 2 or cursor + length + 2 > len(tpi):
                raise ProfileError("PDB type record out of bounds")
            leaf = integer(tpi, cursor + 2, "H")
            record = tpi[cursor + 4:cursor + 2 + length]
            self.records[index] = (leaf, record)
            cursor += length + 2
            if leaf in (0x1504, 0x1505, 0x1506, 0x1507):
                definition = self.class_info(index)
                if not definition["forward"]:
                    self.classes.setdefault(definition["name"], []).append(definition)

    def class_info(self, index: int) -> dict:
        leaf, record = self.records[index]
        definition = {"type_index": index, "forward": bool(integer(record, 2, "H") & 0x80)}
        if leaf == 0x1507:
            definition["size"] = 0
            definition["field_index"] = integer(record, 8)
            definition["name"], _ = spelling(record, 12)
        else:
            definition["field_index"] = integer(record, 4)
            definition["size"], cursor = numeric(record, 16 if leaf in (0x1504, 0x1505) else 8)
            definition["name"], _ = spelling(record, cursor)
        return definition

    def fields(self, index: int, seen: frozenset[int] = frozenset()) -> list[dict]:
        if not index:
            return []
        if index in seen:
            raise ProfileError("Cyclic PDB field continuation")
        seen = seen | {index}
        leaf, record = self.records[index]
        if leaf != 0x1203:
            raise ProfileError("Expected CodeView field list")
        result, cursor = [], 0
        while cursor < len(record):
            if record[cursor] >= 0xF0:
                padding = record[cursor] & 0xF
                if not padding:
                    raise ProfileError("Invalid CodeView padding")
                cursor += padding
                continue
            kind = integer(record, cursor, "H")
            cursor += 2
            if kind == 0x150D:  # LF_MEMBER
                type_index = integer(record, cursor + 2)
                offset, cursor = numeric(record, cursor + 6)
                name, cursor = spelling(record, cursor)
                result.append({"kind": "member", "name": name, "type_index": type_index, "offset": offset})
            elif kind == 0x1400:  # LF_BCLASS
                type_index = integer(record, cursor + 2)
                offset, cursor = numeric(record, cursor + 6)
                result.append({"kind": "base", "type_index": type_index, "offset": offset})
            elif kind in (0x1401, 0x1402):  # virtual bases: not used by HotSpot layout adapters
                cursor += 10
                _, cursor = numeric(record, cursor)
                _, cursor = numeric(record, cursor)
            elif kind in (0x150E, 0x1510, 0x1512, 0x150F):
                cursor += 6
                _, cursor = spelling(record, cursor)
            elif kind == 0x1511:  # LF_ONEMETHOD
                attributes = integer(record, cursor, "H")
                cursor += 6
                if (attributes >> 2) & 7 in (4, 6):
                    cursor += 4
                _, cursor = spelling(record, cursor)
            elif kind == 0x1409:  # LF_VFUNCTAB
                cursor += 6
            elif kind == 0x1404:  # LF_INDEX
                continuation = integer(record, cursor + 2)
                cursor += 6
                result.extend(self.fields(continuation, seen))
            elif kind == 0x1502:  # LF_ENUMERATE
                value, cursor = numeric(record, cursor + 2)
                name, cursor = spelling(record, cursor)
                result.append({"kind": "enumerator", "name": name, "value": value})
            elif kind == 0x1513:  # LF_VFUNCOFF
                cursor += 10
            elif kind == 0x150C:  # LF_FRIENDCLS
                cursor += 6
            else:
                raise ProfileError(f"Unsupported CodeView field leaf {kind:#x}")
        return result

    def describe(self, name: str, seen: frozenset[str] = frozenset()) -> dict:
        if name in seen:
            raise ProfileError("Cyclic PDB class inheritance")
        candidates = self.classes.get(name)
        if not candidates:
            raise ProfileError(f"Required PDB type missing: {name}")
        descriptions = []
        for definition in candidates:
            result = {"size": definition["size"], "members": {}, "bases": [], "enums": {}}
            for field in self.fields(definition["field_index"]):
                kind = field["kind"]
                if kind == "base":
                    base_name = self.class_info(field["type_index"])["name"]
                    result["bases"].append({"type": base_name, "offset": field["offset"]})
                    base = self.describe(base_name, seen | {name})
                    for member, value in base["members"].items():
                        result["members"][member] = {**value, "offset": value["offset"] + field["offset"]}
                elif kind == "member":
                    result["members"][field["name"]] = {"offset": field["offset"], "type_index": field["type_index"]}
                elif kind == "enumerator":
                    result["enums"][field["name"]] = field["value"]
            descriptions.append(result)
        first = descriptions[0]
        if any(description != first for description in descriptions[1:]):
            raise ProfileError(f"Conflicting PDB type definitions: {name}")
        return first

    def symbols(self, sections: list[dict], image_size: int) -> dict[str, int]:
        dbi = self.streams[3]
        stream_index = integer(dbi, 20, "H")
        if stream_index >= len(self.streams):
            raise ProfileError("PDB symbol stream index out of bounds")
        records, cursor, result = self.streams[stream_index], 0, {}
        while cursor + 4 <= len(records):
            length, kind = integer(records, cursor, "H"), integer(records, cursor + 2, "H")
            if length < 2 or cursor + length + 2 > len(records):
                raise ProfileError("PDB symbol record out of bounds")
            record = records[cursor + 4:cursor + length + 2]
            cursor += length + 2
            if kind != 0x110E:  # S_PUB32: decorated private function/data names
                continue
            segment, offset = integer(record, 8, "H"), integer(record, 4)
            name, _ = spelling(record, 10)
            if not segment or segment > len(sections):
                continue
            rva = sections[segment - 1]["rva"] + offset
            if rva >= image_size:
                raise ProfileError(f"Symbol RVA outside DLL: {name}")
            if name in result and result[name] != rva:
                raise ProfileError(f"Conflicting PDB symbol: {name}")
            result[name] = rva
        return result


DEFAULT_TYPES = ("Thread", "ThreadShadow", "JavaThread", "JavaFrameAnchor", "OSThread",
                 "HandleMark", "HandleArea", "JNIHandleBlock", "JavaCallArguments", "JavaValue",
                 "methodHandle", "Handle", "Arena", "Chunk", "Mutex", "Monitor", "JVMFlag", "Flag",
                 "StackOverflow", "MemoryType", "MEMFLAGS", "MemTag", "JavaThreadState", "BasicType",
                 "JavaThread::JNIAttachStates", "JavaThread::TerminatedTypes",
                 "JavaThread::ExitType", "JavaThread::StackGuardState", "StackOverflow::StackGuardState")


def extract(pdb_path: Path, dll_path: Path, java_feature: int,
            names: tuple[str, ...] = DEFAULT_TYPES) -> dict:
    if not 8 <= java_feature <= 27:
        raise ProfileError("Java feature must be between 8 and 27")
    image, pdb = PeImage(dll_path), Pdb(pdb_path)
    if image.identity != pdb.identity:
        raise ProfileError(f"PDB/DLL identity mismatch: {pdb.identity} != {image.identity}")
    types = {name: pdb.describe(name) for name in names if name in pdb.classes}
    return {"schema": 1, "identity": {**image.identity, "machine": image.machine,
            "image_size": image.image_size, "pe_timestamp": image.timestamp,
            "sha256": image.sha256, "java_feature": java_feature},
            "symbols": pdb.symbols(image.sections, image.image_size), "types": types}


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--pdb", type=Path, required=True)
    parser.add_argument("--dll", type=Path, required=True)
    parser.add_argument("--java-feature", type=int, required=True)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--type", action="append", dest="types")
    arguments = parser.parse_args()
    try:
        profile = extract(arguments.pdb, arguments.dll, arguments.java_feature,
                          tuple(arguments.types) if arguments.types else DEFAULT_TYPES)
    except (ProfileError, OSError, UnicodeError) as error:
        parser.exit(1, f"private_profiles: {error}\n")
    arguments.output.parent.mkdir(parents=True, exist_ok=True)
    arguments.output.write_text(json.dumps(profile, indent=2, sort_keys=True) + "\n", encoding="utf-8")
    print(f"Java {arguments.java_feature}: {len(profile['symbols'])} symbols, "
          f"{len(profile['types'])} types, GUID {profile['identity']['guid']}")


if __name__ == "__main__":
    main()
