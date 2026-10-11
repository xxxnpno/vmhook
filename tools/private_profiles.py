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


class profile_error(ValueError):
    pass


def integer(data: bytes, offset: int, fmt: str = "I") -> int:
    try:
        return struct.unpack_from("<" + fmt, data, offset)[0]
    except struct.error as error:
        raise profile_error("Truncated PDB or PE record") from error


def spelling(data: bytes, offset: int) -> tuple[str, int]:
    end = data.find(b"\0", offset)
    if end < 0:
        raise profile_error("Unterminated CodeView string")
    return data[offset:end].decode("utf-8", "strict"), end + 1


def numeric(data: bytes, offset: int) -> tuple[int, int]:
    leaf = integer(data, offset, "H")
    if leaf < 0x8000:
        return leaf, offset + 2
    formats = {0x8000: "b", 0x8001: "h", 0x8002: "H", 0x8003: "i",
               0x8004: "I", 0x8009: "q", 0x800A: "Q"}
    if leaf not in formats:
        raise profile_error(f"Unsupported integer CodeView leaf {leaf:#x}")
    fmt = formats[leaf]
    return integer(data, offset + 2, fmt), offset + 2 + struct.calcsize(fmt)


class pe_image:
    def __init__(self, path: Path):
        data = path.read_bytes()
        self.data = data
        if data[:2] != b"MZ":
            raise profile_error("DLL is not a PE image")
        pe = integer(data, 0x3C)
        if data[pe:pe + 4] != b"PE\0\0":
            raise profile_error("Invalid PE signature")
        self.machine = integer(data, pe + 4, "H")
        if self.machine != 0x8664 or integer(data, pe + 24, "H") != 0x20B:
            raise profile_error("Only Windows x64 HotSpot images are supported")
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
            raise profile_error("CodeView RVA is not backed by DLL bytes")

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
            raise profile_error("DLL must have exactly one RSDS identity")
        self.identity = identities[0]

    def read(self, rva: int, size: int) -> bytes:
        """Read an image RVA, including its zero-filled virtual section tail."""
        if rva < 0 or size < 0 or rva > self.image_size or size > self.image_size - rva:
            raise profile_error("RVA outside PE image")
        if rva < self.header_size and size <= self.header_size - rva:
            result = self.data[rva:rva + size]
            if len(result) != size:
                raise profile_error("Truncated PE header")
            return result
        for section in self.sections:
            delta = rva - section["rva"]
            length = max(section["virtual_size"], section["raw_size"])
            if 0 <= delta <= length and size <= length - delta:
                backed = min(size, max(0, section["raw_size"] - delta))
                start = section["raw_offset"] + delta
                result = self.data[start:start + backed]
                if len(result) != backed:
                    raise profile_error("Truncated PE section")
                return result + bytes(size - backed)
        raise profile_error("RVA does not belong to a PE section")

    def rva_number(self, rva: int, fmt: str = "Q") -> int:
        return integer(self.read(rva, struct.calcsize(fmt)), 0, fmt)

    def string(self, rva: int) -> str:
        result = bytearray()
        for offset in range(65536):
            byte = self.read(rva + offset, 1)
            if byte == b"\0":
                return result.decode("utf-8", "strict")
            result.extend(byte)
        raise profile_error("Unterminated PE string")

    def exports(self) -> dict[str, int]:
        table = self.export_directory
        if not table:
            raise profile_error("PE exports are missing")
        count, function_count = self.rva_number(table + 24, "I"), self.rva_number(table + 20, "I")
        functions = self.rva_number(table + 28, "I")
        names, ordinals = self.rva_number(table + 32, "I"), self.rva_number(table + 36, "I")
        if max(count, function_count) > 1_000_000:
            raise profile_error("PE export count out of bounds")
        result = {}
        for index in range(count):
            name = self.string(self.rva_number(names + index * 4, "I"))
            ordinal = self.rva_number(ordinals + index * 2, "H")
            if ordinal >= function_count:
                raise profile_error("PE export ordinal out of bounds")
            result[name] = self.rva_number(functions + ordinal * 4, "I")
        return result

    def metadata(self) -> dict[str, dict]:
        """Read VMTypes/VMStructs offline; no JVM or Wine process is launched."""
        exports = self.exports()

        def exported(name: str) -> int:
            if name not in exports:
                raise profile_error("Required HotSpot metadata export missing: " + name)
            return self.rva_number(exports[name])

        def rows(table: str, stride: str, limit: int):
            start, step = exported(table) - self.base, exported(stride)
            if not 8 <= step <= 256:
                raise profile_error("VM metadata stride out of bounds")
            for index in range(limit):
                row = start + index * step
                if not self.rva_number(row):
                    return
                yield row
            raise profile_error("VM metadata table has no bounded terminator")

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


class pdb_file:
    def __init__(self, path: Path):
        data = path.read_bytes()
        if data[:32] != b"Microsoft C/C++ MSF 7.00\r\n\x1aDS\0\0\0":
            raise profile_error("PDB is not MSF 7")
        block_size = integer(data, 32)
        blocks = integer(data, 40)
        directory_size = integer(data, 44)
        block_map = integer(data, 52)
        if block_size not in (512, 1024, 2048, 4096, 8192) or blocks * block_size > len(data):
            raise profile_error("Invalid PDB block geometry")

        def read_blocks(indexes: list[int], size: int) -> bytes:
            if size > len(data) or any(index >= blocks for index in indexes):
                raise profile_error("PDB block index out of bounds")
            return b"".join(data[index * block_size:(index + 1) * block_size]
                            for index in indexes)[:size]

        directory_blocks = (directory_size + block_size - 1) // block_size
        block_index_offset = block_map * block_size
        if directory_blocks * 4 > block_size:
            raise profile_error("Unsupported multi-page PDB directory block map")
        indexes = [integer(data, block_index_offset + i * 4) for i in range(directory_blocks)]
        directory = read_blocks(indexes, directory_size)
        stream_count = integer(directory, 0)
        if stream_count > 65536 or stream_count > directory_size // 4:
            raise profile_error("PDB stream count out of bounds")
        sizes = [integer(directory, 4 + i * 4) for i in range(stream_count)]
        if sum(size for size in sizes if size != 0xFFFFFFFF) > len(data):
            raise profile_error("PDB streams exceed file capacity")
        cursor = 4 + stream_count * 4
        self.streams = []
        for size in sizes:
            count = 0 if size == 0xFFFFFFFF else (size + block_size - 1) // block_size
            indexes = [integer(directory, cursor + i * 4) for i in range(count)]
            cursor += count * 4
            self.streams.append(b"" if size == 0xFFFFFFFF else read_blocks(indexes, size))
        if len(self.streams) < 4 or len(self.streams[1]) < 28:
            raise profile_error("PDB identity/DBI/TPI streams are missing")
        info = self.streams[1]
        self.identity = {"guid": str(uuid.UUID(bytes_le=info[12:28])), "age": integer(info, 8)}
        tpi = self.streams[2]
        cursor, first, last = integer(tpi, 4), integer(tpi, 8), integer(tpi, 12)
        if last < first or last - first > 10_000_000:
            raise profile_error("PDB type index range invalid")
        self.records = {}
        self.classes = {}
        for index in range(first, last):
            length = integer(tpi, cursor, "H")
            if length < 2 or cursor + length + 2 > len(tpi):
                raise profile_error("PDB type record out of bounds")
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
            raise profile_error("Cyclic PDB field continuation")
        seen = seen | {index}
        leaf, record = self.records[index]
        if leaf != 0x1203:
            raise profile_error("Expected CodeView field list")
        result, cursor = [], 0
        while cursor < len(record):
            if record[cursor] >= 0xF0:
                padding = record[cursor] & 0xF
                if not padding:
                    raise profile_error("Invalid CodeView padding")
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
            elif kind in (0x150E, 0x1510, 0x1512):
                cursor += 6
                _, cursor = spelling(record, cursor)
            elif kind == 0x150F:  # LF_METHOD: overloaded method list
                count, method_index = integer(record, cursor, "H"), integer(record, cursor + 2)
                cursor += 6
                name, cursor = spelling(record, cursor)
                method_leaf, method_record = self.records[method_index]
                if method_leaf != 0x1206 or count > 1024:
                    raise profile_error("Invalid bounded PDB method list")
                position = 0
                for _ in range(count):
                    attributes = integer(method_record, position, "H")
                    type_index = integer(method_record, position + 4)
                    position += 8
                    virtual_offset = None
                    if (attributes >> 2) & 7 in (4, 6):
                        virtual_offset = integer(method_record, position)
                        position += 4
                    result.append({"kind": "method", "name": name, "type_index": type_index,
                                   "virtual_offset": virtual_offset})
            elif kind == 0x1511:  # LF_ONEMETHOD
                attributes = integer(record, cursor, "H")
                type_index = integer(record, cursor + 2)
                cursor += 6
                virtual_offset = None
                if (attributes >> 2) & 7 in (4, 6):
                    virtual_offset = integer(record, cursor)
                    cursor += 4
                name, cursor = spelling(record, cursor)
                result.append({"kind": "method", "name": name, "type_index": type_index,
                               "virtual_offset": virtual_offset})
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
                raise profile_error(f"Unsupported CodeView field leaf {kind:#x}")
        return result

    def storage(self, index: int, seen: frozenset[int] = frozenset()) -> dict:
        """Resolve exact field storage, including forward types and bitfields."""
        if index in seen:
            raise profile_error("Cyclic PDB field storage type")
        seen = seen | {index}
        if index < 0x1000:
            if index >> 8 == 6:
                return {"storage_size": 8}
            widths = {0x10: 1, 0x20: 1, 0x11: 2, 0x21: 2, 0x12: 4, 0x22: 4,
                      0x13: 8, 0x23: 8, 0x30: 1, 0x40: 4, 0x41: 8, 0x70: 1,
                      0x71: 2, 0x74: 4, 0x75: 4, 0x76: 8, 0x77: 8}
            return {"storage_size": widths.get(index & 0xFF, 0)}
        leaf, record = self.records[index]
        if leaf == 0x1001:  # LF_MODIFIER
            return self.storage(integer(record, 0), seen)
        if leaf == 0x1002:  # LF_POINTER (the supported target is Windows x64)
            return {"storage_size": 8}
        if leaf == 0x1205:  # LF_BITFIELD
            return {**self.storage(integer(record, 0), seen),
                    "bit_length": record[4], "bit_position": record[5]}
        if leaf == 0x1503:  # LF_ARRAY
            size, _ = numeric(record, 8)
            return {"storage_size": size}
        if leaf == 0x1507:  # LF_ENUM
            return self.storage(integer(record, 4), seen)
        if leaf in (0x1504, 0x1505, 0x1506):
            definition = self.class_info(index)
            if not definition["forward"]:
                return {"storage_size": definition["size"]}
            candidates = self.classes.get(definition["name"], [])
            sizes = {candidate["size"] for candidate in candidates}
            if len(sizes) > 1:
                raise profile_error("Conflicting PDB field storage definitions")
            return {"storage_size": next(iter(sizes)) if sizes else 0}
        return {"storage_size": 0}

    def points_to(self, index: int, name: str, depth: int,
                  seen: frozenset[int] = frozenset()) -> bool:
        """Identify an exact pointer target without relying on overload order."""
        if index in seen or len(seen) > 16 or index not in self.records:
            return False
        seen = seen | {index}
        leaf, record = self.records[index]
        if leaf == 0x1001:  # LF_MODIFIER
            return self.points_to(integer(record, 0), name, depth, seen)
        if depth:
            if leaf != 0x1002 or (integer(record, 4) >> 5) & 7:
                return False  # LF_POINTER, excluding references/member pointers
            return self.points_to(integer(record, 0), name, depth - 1, seen)
        return leaf in (0x1504, 0x1505, 0x1506) and self.class_info(index)["name"] == name

    def wide_oop_closure(self) -> dict:
        """Resolve do_oop(oopDesc**) and its Microsoft vtable slot from TPI."""
        layouts = []
        for definition in self.classes.get("OopClosure", []):
            wide, slots = [], []
            for method in self.fields(definition["field_index"]):
                if method["kind"] != "method" or method["virtual_offset"] is None:
                    continue
                offset = method["virtual_offset"]
                if offset % 8 or offset > 1024:
                    raise profile_error("Invalid bounded OopClosure virtual slot")
                slots.append(offset // 8)
                if method["name"] != "do_oop":
                    continue
                leaf, record = self.records[method["type_index"]]
                if leaf != 0x1009 or integer(record, 0) != 3 or integer(record, 14, "H") != 1:
                    raise profile_error("OopClosure do_oop is not a void member function")
                arguments_leaf, arguments = self.records[integer(record, 16)]
                if arguments_leaf != 0x1201 or integer(arguments, 0) != 1:
                    raise profile_error("OopClosure do_oop does not have one argument")
                if self.points_to(integer(arguments, 4), "oopDesc", 2):
                    wide.append(offset // 8)
            if len(wide) != 1 or not slots:
                raise profile_error("Ambiguous OopClosure wide do_oop overload")
            layouts.append({"vtable_do_oop_wide": wide[0], "vtable_count": max(slots) + 1})
        if not layouts or any(layout != layouts[0] for layout in layouts[1:]):
            raise profile_error("Conflicting OopClosure virtual layouts")
        return layouts[0]

    def object_closure(self) -> dict:
        """Prove the exact ObjectClosure::do_object(oopDesc*) MSVC ABI."""
        layouts = []
        for definition in self.classes.get("ObjectClosure", []):
            slots, object_slots = [], []
            if definition["size"] not in (8, 16):
                raise profile_error("ObjectClosure has an unsupported bounded layout")
            layout = self.describe("ObjectClosure")
            if set(layout["members"]) - {"_abort"}:
                raise profile_error("ObjectClosure has an unexpected state member")
            abort = layout["members"].get("_abort")
            if abort and (abort["offset"] != 8 or abort.get("storage_size") != 1):
                raise profile_error("Legacy ObjectClosure abort flag has an unsupported layout")
            for method in self.fields(definition["field_index"]):
                if method["kind"] != "method" or method["virtual_offset"] is None:
                    continue
                offset = method["virtual_offset"]
                if offset % 8 or not 0 <= offset <= 1024:
                    raise profile_error("Invalid bounded ObjectClosure virtual slot")
                slots.append(offset // 8)
                if method["name"] != "do_object":
                    continue
                leaf, record = self.records[method["type_index"]]
                if leaf != 0x1009 or integer(record, 0) != 3 or integer(record, 14, "H") != 1:
                    raise profile_error("ObjectClosure do_object must be a void member with one argument")
                arguments_leaf, arguments = self.records[integer(record, 16)]
                if arguments_leaf != 0x1201 or integer(arguments, 0) != 1 \
                        or not self.points_to(integer(arguments, 4), "oopDesc", 1):
                    raise profile_error("ObjectClosure do_object does not take an oopDesc pointer")
                object_slots.append(offset // 8)
            if len(object_slots) != 1 or not slots:
                raise profile_error("ObjectClosure virtual callback is ambiguous")
            layouts.append({"vtable_do_object": object_slots[0], "vtable_count": max(slots) + 1})
        if not layouts or any(layout != layouts[0] for layout in layouts):
            raise profile_error("Missing or conflicting ObjectClosure layout")
        return layouts[0]

    def describe(self, name: str, seen: frozenset[str] = frozenset()) -> dict:
        if name in seen:
            raise profile_error("Cyclic PDB class inheritance")
        candidates = self.classes.get(name)
        if not candidates:
            raise profile_error(f"Required PDB type missing: {name}")
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
                    result["members"][field["name"]] = {"offset": field["offset"], "type_index": field["type_index"],
                                                        **self.storage(field["type_index"])}
                elif kind == "enumerator":
                    result["enums"][field["name"]] = field["value"]
                elif kind == "method" and name in ("VM_Operation", "ThreadClosure", "MetadataClosure", "NMethodClosure"):
                    offset = field["virtual_offset"]
                    if offset is not None:
                        if offset % 8 or offset > 1024:
                            raise profile_error("PDB virtual slot offset out of bounds")
                        result["enums"]["vtable_" + field["name"]] = offset // 8
            if name in ("VM_Operation", "ThreadClosure", "MetadataClosure", "NMethodClosure"):
                slots = [value for key, value in result["enums"].items() if key.startswith("vtable_")]
                if slots:
                    result["enums"]["vtable_count"] = max(slots) + 1
            for member, facts in result["members"].items():
                if facts.get("storage_size"):
                    result["enums"]["storage_size_" + member] = facts["storage_size"]
                if "bit_position" in facts:
                    result["enums"]["bit_position_" + member] = facts["bit_position"]
                    result["enums"]["bit_length_" + member] = facts["bit_length"]
            descriptions.append(result)
        first = descriptions[0]
        if any(description != first for description in descriptions[1:]):
            raise profile_error(f"Conflicting PDB type definitions: {name}")
        return first

    def symbols(self, sections: list[dict], image_size: int) -> dict[str, int]:
        dbi = self.streams[3]
        stream_index = integer(dbi, 20, "H")
        if stream_index >= len(self.streams):
            raise profile_error("PDB symbol stream index out of bounds")
        records, cursor, result = self.streams[stream_index], 0, {}
        while cursor + 4 <= len(records):
            length, kind = integer(records, cursor, "H"), integer(records, cursor + 2, "H")
            if length < 2 or cursor + length + 2 > len(records):
                raise profile_error("PDB symbol record out of bounds")
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
                raise profile_error(f"Symbol RVA outside DLL: {name}")
            if name in result and result[name] != rva:
                raise profile_error(f"Conflicting PDB symbol: {name}")
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
        raise profile_error("Java feature must be between 8 and 27")
    image, pdb = pe_image(dll_path), pdb_file(pdb_path)
    if image.identity != pdb.identity:
        raise profile_error(f"PDB/DLL identity mismatch: {pdb.identity} != {image.identity}")
    types = {name: pdb.describe(name) for name in names if name in pdb.classes}
    symbols = pdb.symbols(image.sections, image.image_size)
    if "frame" in names:
        frame = types["frame"]
        offsets = pdb.describe("frame::<unnamed-enum-pc_return_offset>")["enums"]
        for name in ("interpreter_frame_method_offset", "interpreter_frame_last_sp_offset",
                     "interpreter_frame_initial_sp_offset", "interpreter_frame_monitor_block_top_offset"):
            frame["enums"][name] = offsets[name]
        bcp_offsets = [offsets[name] for name in ("interpreter_frame_bcp_offset", "interpreter_frame_bcx_offset")
                       if name in offsets]
        if len(bcp_offsets) != 1:
            raise profile_error("Ambiguous interpreter-frame bytecode-pointer offset")
        frame["enums"]["interpreter_frame_bcp_offset"] = bcp_offsets[0]
    if "OopClosure" in names:
        types["OopClosure"]["enums"].update(pdb.wide_oop_closure())
    if "ObjectClosure" in names and "ObjectClosure" in types:
        types["ObjectClosure"]["enums"].update(pdb.object_closure())
    if "ObservationABI" in names:
        # This explicitly synthetic profile record contains signature/layout
        # selectors, not a native object layout. See derived_profile_facts in
        # the manifest for the schema and each selector's meaning.
        old_setter = "?set_not_compilable@Method@@QEAAXH_NPEBD@Z" in symbols
        new_setter = "?set_not_compilable@Method@@QEAAXPEBDH_N@Z" in symbols
        callback_metadata = any(name.startswith("?metadata_do@nmethod@@") and
                                "P6AXPEAVMetadata@@@Z" in name for name in symbols)
        closure_metadata = any(name.startswith("?metadata_do@nmethod@@") and
                               "PEAVMetadataClosure@@" in name for name in symbols)
        scope = "?deoptimize_marked@DeoptimizationScope@@QEAAXXZ" in symbols
        marked = "?deoptimize_all_marked@Deoptimization@@SAXPEAVnmethod@@@Z" in symbols
        vm_deoptimize = "?doit@VM_Deoptimize@@UEAAXXZ" in symbols
        if old_setter == new_setter or callback_metadata == closure_metadata or \
                sum((scope, marked, vm_deoptimize)) != 1:
            raise profile_error("Ambiguous observation ABI signatures")
        method = types["Method"]
        if "_dont_inline" in method["members"]:
            flags = method["members"]["_dont_inline"]
            if flags["bit_length"] != 1:
                raise profile_error("Unexpected Method dont-inline bitfield")
            flag_offset, flag_width = flags["offset"], flags["storage_size"]
            flag_mask = 1 << flags["bit_position"]
        elif "Method::Flags" in types:
            flags = method["members"]["_flags"]
            flag_offset, flag_width = flags["offset"], flags["storage_size"]
            flag_mask = types["Method::Flags"]["enums"]["_dont_inline"]
        else:
            flags = types["MethodFlags"]["members"]["_status"]
            flag_offset = method["members"]["_flags"]["offset"] + flags["offset"]
            flag_width = flags["storage_size"]
            flag_mask = types["MethodFlags::<unnamed-enum-_misc_has_monitor_bytecodes>"]["enums"]["_misc_dont_inline"]
        if flag_width not in (1, 2, 4) or flag_mask <= 0 or flag_mask >= 1 << (8 * flag_width):
            raise profile_error("Invalid bounded Method dont-inline layout")
        java_thread_slots = {field["virtual_offset"] for definition in pdb.classes["Thread"]
                             for field in pdb.fields(definition["field_index"])
                             if field["kind"] == "method" and field["name"] == "is_Java_thread"}
        if len(java_thread_slots) != 1:
            raise profile_error("Ambiguous Thread is_Java_thread virtual slot")
        java_thread_slot = next(iter(java_thread_slots))
        if java_thread_slot is None or java_thread_slot % 8 or java_thread_slot > 1024:
            raise profile_error("Invalid bounded Thread is_Java_thread virtual slot")
        types["ObservationABI"] = {"size": 0, "members": {}, "bases": [],
            "synthetic": "signature/layout-derived selectors", "enums": {
                "set_not_compilable_old_args": int(old_setter),
                "nmethod_metadata_closure": int(closure_metadata),
                "deopt_mode": 2 if scope else 1 if marked else 0,
                "method_dont_inline_mask": flag_mask,
                "method_dont_inline_offset": flag_offset,
                "method_dont_inline_storage_size": flag_width,
                "thread_is_java_slot": java_thread_slot // 8}}
        if "frame" in names:
            legacy_scan = "?oops_interpreted_do@frame@@QEAAXPEAVOopClosure@@PEAVCLDClosure@@PEBVRegisterMap@@_N@Z" in symbols
            ordinary_scans = tuple(name in symbols for name in (
                "?oops_interpreted_do@frame@@QEAAXPEAVOopClosure@@PEBVRegisterMap@@_N@Z",
                "?oops_interpreted_do@frame@@QEBAXPEAVOopClosure@@PEBVRegisterMap@@_N@Z"))
            template_scan = "??$oops_interpreted_do@VRegisterMap@@@frame@@QEBAXPEAVOopClosure@@PEBVRegisterMap@@_N@Z" in symbols
            small_scan = tuple(name in symbols for name in (
                "??$oops_interpreted_do@V?$SmallRegisterMapType@$0A@@@@frame@@QEBAXPEAVOopClosure@@PEBV?$SmallRegisterMapType@$0A@@@_N@Z",
                "??$oops_interpreted_do@V?$SmallRegisterMapType@$00@@@frame@@QEBAXPEAVOopClosure@@PEBV?$SmallRegisterMapType@$00@@_N@Z"))
            if sum((legacy_scan, *ordinary_scans, template_scan)) != 1 or any(flag != template_scan for flag in small_scan):
                raise profile_error("Ambiguous interpreted-frame scanning signatures")
            types["ObservationABI"]["enums"].update({
                "frame_scan_legacy_cld_arg": int(legacy_scan),
                "frame_scan_small_maps": int(template_scan)})
            # Physical interpreter-stack slots can hold either absolute pointers
            # or signed word offsets from FP. The named getter's exact bytes,
            # checked against TPI field/slot offsets, distinguish the three ABIs.
            frame = types["frame"]
            fp = frame["members"]["_fp"]["offset"]
            monitor = 8 * frame["enums"]["interpreter_frame_monitor_block_top_offset"]
            if not 0 <= fp < 128 or not -128 <= monitor < 0:
                raise profile_error("Interpreter monitor getter offsets exceed audited encodings")
            getter = "?interpreter_frame_monitor_end@frame@@QEBAPEAVBasicObjectLock@@XZ"
            if getter not in symbols:
                raise profile_error("Interpreted-frame monitor getter is missing from matching PDB")
            code = image.read(symbols[getter], 32)
            absolute = b"\x48\x8b\x41" + bytes([fp]) + b"\x48\x8b\x40" + bytes([monitor & 255]) + b"\xc3"
            relative = b"\x48\x8b\x51" + bytes([fp]) + b"\x48\x8b\x42" + bytes([monitor & 255]) + b"\x48\x8d\x04\xc2\xc3"
            heap = frame["members"].get("_on_heap")
            conditional = b""
            if heap and 0 <= heap["offset"] < 128:
                conditional = b"\x80\x79" + bytes([heap["offset"]]) + b"\0" + relative[:8] + b"\x74\x04" + relative[8:]
            if code.startswith(relative):
                slots_relative = True
            elif code.startswith(absolute) or conditional and code.startswith(conditional):
                slots_relative = False  # Conditional relocation concerns heap frames only.
            else:
                raise profile_error("Interpreted-frame monitor getter differs from audited ABI patterns")
            size_getters = [name for name in symbols if name.startswith("?interpreter_frame_expression_stack_size@frame@@")]
            if len(size_getters) != 1:
                raise profile_error("Ambiguous interpreted expression-stack-size getter")
            code = image.read(symbols[size_getters[0]], 96)
            last = 8 * frame["enums"]["interpreter_frame_last_sp_offset"]
            if not -128 <= last < 0:
                raise profile_error("Interpreter last_sp offset exceeds audited encodings")
            last_byte = bytes([last & 255])
            relative_last = b"\x48\x8b\x51" + last_byte + b"\x48\x85\xd2\x74\x09\x48\x8d\x14\xd1"
            absolute_last = (b"\x48\x8b\x53" + bytes([fp]) + b"\x4c\x8b\x42" + last_byte + b"\x4d\x85\xc0\x75\x03\x4c\x8b\x03",
                             b"\x48\x8b\x43" + bytes([fp]) + b"\x4c\x8b\x40" + last_byte + b"\x4d\x85\xc0\x75\x03\x4c\x8b\x03")
            conditional_last = (b"\x74\x0a\x49\x8b\x50" + last_byte + b"\x4d\x8d\x04\xd0\xeb\x04\x4d\x8b\x40" + last_byte,
                                b"\x74\x0a\x49\x8b\x40" + last_byte + b"\x4d\x8d\x04\xc0\xeb\x04\x4d\x8b\x40" + last_byte)
            if slots_relative:
                matches_last = relative_last in code
            elif heap:
                heap_load = b"\x80\x7b" + bytes([heap["offset"]]) + b"\0\x4c\x8b\x43" + bytes([fp])
                matches_last = heap_load in code and any(pattern in code for pattern in conditional_last)
            else:
                matches_last = any(pattern in code for pattern in absolute_last)
            if not matches_last:
                raise profile_error("Physical last_sp conversion disagrees with the monitor-slot ABI")
            types["ObservationABI"]["enums"]["frame_stack_slots_relative"] = int(slots_relative)
        special_klass = {f"{kind}_klass_{part}": 0
                         for kind in ("anonymous", "hidden")
                         for part in ("offset", "width", "mask")}
        anonymous = types.get("InstanceKlass::<unnamed-enum-_misc_rewritten>", {}).get("enums", {})
        anonymous_masks = [anonymous[name] for name in ("_misc_is_anonymous", "_misc_is_unsafe_anonymous")
                           if name in anonymous]
        if anonymous_masks:
            if len(anonymous_masks) != 1:
                raise profile_error("Ambiguous anonymous Klass predicate")
            flags = types["InstanceKlass"]["members"]["_misc_flags"]
            special_klass.update(anonymous_klass_offset=flags["offset"],
                                 anonymous_klass_width=flags["storage_size"],
                                 anonymous_klass_mask=anonymous_masks[0])
        hidden = types.get("KlassFlags::<unnamed-enum-_misc_is_hidden_class>", {}).get("enums", {})
        if "_misc_is_hidden_class" in hidden:
            outer = types["Klass"]["members"]["_misc_flags"]
            flags = types["KlassFlags"]["members"]["_flags"]
            special_klass.update(hidden_klass_offset=outer["offset"] + flags["offset"],
                                 hidden_klass_width=flags["storage_size"],
                                 hidden_klass_mask=hidden["_misc_is_hidden_class"])
        elif "JVM_IsHiddenClass" in image.exports():
            # These builds store the hidden bit in the Klass AccessFlags.
            # Derive the bit from the exact exported getter's load/shift/and,
            # while the displacement and storage width come independently
            # from TPI. The getter is inspected offline and is never invoked.
            flags = types["Klass"]["members"]["_access_flags"]
            if flags["storage_size"] != 4:
                raise profile_error("Unexpected hidden Klass AccessFlags width")
            getter = image.read(image.exports()["JVM_IsHiddenClass"], 480)
            shifts = []
            for offset in range(len(getter) - 13):
                modrm = getter[offset + 1]
                register = (modrm >> 3) & 7
                if getter[offset] != 0x8b or modrm & 0xc0 != 0x80 or modrm & 7 == 4 or \
                        getter[offset + 2:offset + 6] != struct.pack("<I", flags["offset"]) or \
                        getter[offset + 6:offset + 8] != bytes((0xc1, 0xe8 + register)):
                    continue
                shift = getter[offset + 8]
                tail = getter[offset + 9:offset + 13]
                if not 0 < shift < 32 or not (tail.startswith(bytes((0x83, 0xe0 + register, 1))) or \
                        tail.startswith(bytes((0x40, 0x80, 0xe0 + register, 1))) or \
                        (register == 0 and tail.startswith(b"\x24\x01"))):
                    continue
                shifts.append(shift)
            if len(shifts) != 1:
                raise profile_error("Ambiguous bounded hidden Klass getter predicate")
            special_klass.update(hidden_klass_offset=flags["offset"],
                                 hidden_klass_width=flags["storage_size"],
                                 hidden_klass_mask=1 << shifts[0])
        for kind in ("anonymous", "hidden"):
            width, mask = special_klass[kind + "_klass_width"], special_klass[kind + "_klass_mask"]
            if width and (width not in (1, 2, 4) or not 0 < mask < 1 << (8 * width)):
                raise profile_error("Invalid bounded special Klass predicate")
        types["ObservationABI"]["enums"].update(special_klass)
        # Follow the actual CLD pointer type and flatten its TPI bases. The
        # GrowableArray payload moved when its allocator/view hierarchy
        # changed, so neither its size nor its members are version guesses.
        deallocate = types["ClassLoaderData"]["members"]["_deallocate_list"]
        pointer_leaf, pointer_record = pdb.records[deallocate["type_index"]]
        if pointer_leaf != 0x1002 or deallocate["storage_size"] != 8:
            raise profile_error("Unexpected ClassLoaderData deallocate-list pointer")
        array_name = pdb.class_info(integer(pointer_record, 0))["name"]
        if array_name != "GrowableArray<Metadata *>":
            raise profile_error("Unexpected deallocate-list array element type")
        array = pdb.describe(array_name)
        data, length = array["members"]["_data"], array["members"]["_len"]
        if data["storage_size"] != 8 or length["storage_size"] != 4 or not 0 < array["size"] <= 128 or \
                any(member["offset"] < 0 or member["offset"] + member["storage_size"] > array["size"]
                    for member in (data, length)):
            raise profile_error("Invalid bounded deallocate-list array layout")
        types["ObservationABI"]["enums"].update({
            "deallocate_list_size": array["size"],
            "deallocate_list_data_offset": data["offset"],
            "deallocate_list_data_width": data["storage_size"],
            "deallocate_list_length_offset": length["offset"],
            "deallocate_list_length_width": length["storage_size"]})
        if java_feature == 20:
            # Java20's VM-to-Java destructor is inlined. Its poll selector is
            # a static source constant, absent from TPI. Extract the actual
            # immediate from the bounded, PDB-named should_process body and
            # verify its load agrees with the independently extracted layout.
            poll_offset = types["JavaThread"]["members"]["_poll_data"]["offset"] + \
                types["SafepointMechanism::ThreadData"]["members"]["_polling_word"]["offset"]
            code = image.read(symbols["?should_process@SafepointMechanism@@SA_NPEAVJavaThread@@_N@Z"], 32)
            load_test = b"\x48\x8b\x81" + struct.pack("<I", poll_offset) + b"\x48\x8b\xd9\xa8"
            offset = code.find(load_test)
            if offset < 0 or offset + len(load_test) >= len(code):
                raise profile_error("Java20 polling-word audit pattern differs")
            poll_bit = code[offset + len(load_test)]
            if not poll_bit or poll_bit & (poll_bit - 1):
                raise profile_error("Invalid Java20 local-poll bit")
            types["ObservationABI"]["enums"]["callback_poll_bit"] = poll_bit
    return {"schema": 1, "identity": {**image.identity, "machine": image.machine,
            "image_size": image.image_size, "pe_timestamp": image.timestamp,
            "sha256": image.sha256, "java_feature": java_feature},
            "symbols": symbols, "types": types}


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
    except (profile_error, OSError, UnicodeError) as error:
        parser.exit(1, f"private_profiles: {error}\n")
    arguments.output.parent.mkdir(parents=True, exist_ok=True)
    arguments.output.write_text(json.dumps(profile, indent=2, sort_keys=True) + "\n", encoding="utf-8")
    print(f"Java {arguments.java_feature}: {len(profile['symbols'])} symbols, "
          f"{len(profile['types'])} types, GUID {profile['identity']['guid']}")


if __name__ == "__main__":
    main()
