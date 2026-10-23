#!/usr/bin/env python3
"""Verify the stock sagit TZ SCM-IO contract without IDA or hardware access.

This is an offline allowlist/evidence audit of one pinned firmware image.
It neither modifies firmware nor authorizes direct nonsecure register access.
"""
from __future__ import annotations

import argparse
import hashlib
import json
import re
import struct
from dataclasses import dataclass
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
DEFAULT_IMAGE = ROOT / "build/usb/firmware/stock/tz.mbn"
DEFAULT_INITIALIZATION = ROOT / "docs/power/osm-tz-initialization-audit.json"
DEFAULT_HEADER = ROOT / "drivers/clock/QcomOSM/OSMTZContract.h"
IMAGE_SHA256 = "d2c417987bb4210eb774b11c3cd61aa5cd4b1260d1dd3313506490ee9026d911"
BASE_TABLE = 0x1C0009C8
EXTRA_TABLE = 0x1C000A68
BASE_ALLOWLIST = (
    0x01FD3000,
    0x179D141C, 0x179D1420, 0x179D1424, 0x179D1428,
    0x179D142C, 0x179D1430, 0x179D1434,
    0x179880B0, 0x179880B8, 0x179980B0, 0x179980B8,
    0x179A80B0, 0x179A80B8, 0x179B80B0, 0x179B80B8,
    0x178880B0, 0x178880B8, 0x178980B0, 0x178980B8,
    0x178A80B0, 0x178A80B8, 0x178B80B0, 0x178B80B8,
    0x179C0420, 0x179C2420, 0x179C0430, 0x179C2430,
    0x179C03E4, 0x179C03E8, 0x179C033C, 0x179C037C,
    0x179C23E4, 0x179C23E8, 0x179C233C, 0x179C237C,
    0x179C0460, 0x179C2460, 0x179C0380, 0x179C2380,
)
EXTRA_ALLOWLIST = (
    0x179C0304, 0x179C0370, 0x179C2304, 0x179C2370,
    0x179C03BC, 0x179C03C0, 0x179C23BC, 0x179C23C0,
    0x179C031C, 0x179C231C,
)
FUNCTIONS = {
    "monitor_dispatcher": (0x146820F8, 0xEF8,
        "29abb6702515633265ec47c1ca8fe30b856640562d485da12a70f5c63a2b4561"),
    "write_handler": (0x14683960, 0x60,
        "bd3ffa948c448c72082ebdbc986eb4852906349b13d82c0f322708c9588618ad"),
    "allowlist_handler": (0x146839C0, 0x8C,
        "474613dcd812750fa6c8ade880c933ced2568d0393cc70bbfc160791f354571b"),
    "read_handler": (0x14683A4C, 0x38,
        "517c800b7ad57a654a3d6a8f0e03086feeaa655c557c16c62b4428808905800f"),
    "conditional_gate": (0x14681314, 0x14,
        "6624de8a73fe64fa1a3a0fb1b03f692662ea1c22918a5d69dc99ea5b6d19b775"),
}
OSM_WRITABLE_SEQUENCERS = (15, 31, 32, 57, 58, 72, 76, 88)
OSM_CONDITIONAL_SEQUENCERS = (1, 7, 28, 47, 48)
OLD_MARKER_SEQUENCERS = (0, 2, 3, 12, 13, 14, 16, 18, 33, 37, 49)
INITIALIZATION_FUNCTIONS = {
    "initializer": (0x1C094058, 348),
    "version_predicate": (0x1C093D08, 92),
    "version_reader": (0x1C093C70, 32),
    "post_table_setup": (0x1C094D14, 1008),
    "direct_caller": (0x1C0664C8, 140),
    "spm_cc_control_writer": (0x1C03A390, 172),
}
DESCRIPTOR_ARRAYS = {
    "single_write_descriptor_array": (0x1C0EC018, 2, 16),
    "block_descriptor_array": (0x1C0EC048, 12, 32),
}


class AuditError(ValueError):
    """The image or the claimed contract cannot be verified."""


@dataclass(frozen=True)
class Segment:
    offset: int
    virtual: int
    physical: int
    file_size: int
    memory_size: int
    flags: int


@dataclass(frozen=True)
class ELF:
    data: bytes
    segments: tuple[Segment, ...]

    def read(self, address: int, size: int) -> bytes:
        if address < 0 or size <= 0 or address + size > 1 << 64:
            raise AuditError("invalid virtual-address read range")
        matches = [s for s in self.segments
                   if s.virtual <= address and address + size <= s.virtual + s.file_size]
        if len(matches) != 1:
            raise AuditError(f"range {address:#x}+{size:#x} is not uniquely file-backed")
        segment = matches[0]
        offset = segment.offset + address - segment.virtual
        return self.data[offset:offset + size]


def parse_elf(data: bytes) -> ELF:
    """Parse strict little-endian AArch64 ELF64 program headers, without sections."""
    if len(data) < 64 or data[:7] != b"\x7fELF\x02\x01\x01":
        raise AuditError("expected little-endian ELF64")
    fields = struct.unpack_from("<16sHHIQQQIHHHHHH", data)
    (_, etype, machine, version, _entry, phoff, _shoff, _flags, ehsize,
     phentsize, phnum, _shentsize, _shnum, _shstrndx) = fields
    if etype != 2 or machine != 183 or version != 1 or ehsize != 64:
        raise AuditError("unsupported ELF type, machine, version or header size")
    if phentsize != 56 or not 0 < phnum <= 1024 or phoff < 64:
        raise AuditError("invalid ELF program-header layout")
    if phoff + phnum * phentsize > len(data):
        raise AuditError("program headers exceed the file")
    segments = []
    for i in range(phnum):
        kind, flags, offset, virtual, physical, filesz, memsz, align = struct.unpack_from(
            "<IIQQQQQQ", data, phoff + i * phentsize)
        if offset + filesz > len(data):
            raise AuditError(f"program header {i} file range exceeds file")
        if kind != 1:
            continue
        if filesz > memsz or virtual + memsz > 1 << 64 or physical + memsz > 1 << 64:
            raise AuditError(f"program header {i} has invalid memory bounds")
        if align not in (0, 1) and (align & (align - 1) or (virtual - offset) % align):
            raise AuditError(f"program header {i} has invalid alignment")
        if not memsz:
            continue
        if any(virtual < s.virtual + s.memory_size and s.virtual < virtual + memsz
               for s in segments):
            raise AuditError("overlapping load segments")
        segments.append(Segment(offset, virtual, physical, filesz, memsz, flags))
    if not segments:
        raise AuditError("ELF contains no loadable bytes")
    return ELF(data, tuple(segments))


def verify_static_contract(elf: ELF) -> None:
    """Verify exact table order and function bytes independently of whole-image SHA."""
    base = struct.unpack("<40I", elf.read(BASE_TABLE, 40 * 4))
    extra = struct.unpack("<10I", elf.read(EXTRA_TABLE, 10 * 4))
    if base != BASE_ALLOWLIST:
        raise AuditError("fixed SCM IO allowlist differs from audited table")
    if extra != EXTRA_ALLOWLIST:
        raise AuditError("conditional SCM IO allowlist differs from audited table")
    for name, (address, size, expected) in FUNCTIONS.items():
        if hashlib.sha256(elf.read(address, size)).hexdigest() != expected:
            raise AuditError(f"{name} instruction bytes differ from audited function")


def integer(value) -> int:
    if isinstance(value, bool) or not isinstance(value, (str, int)):
        raise AuditError("expected unsigned integer in audit document")
    try:
        result = int(value, 0) if isinstance(value, str) else value
    except ValueError as error:
        raise AuditError("invalid integer in audit document") from error
    if not 0 <= result < 1 << 64:
        raise AuditError("audit integer exceeds uint64")
    return result


def require(condition: bool, message: str) -> None:
    if not condition:
        raise AuditError(message)


def verify_hash(elf: ELF, address: int, size: int, expected: str, label: str) -> bytes:
    require(isinstance(expected, str) and bool(re.fullmatch("[0-9a-f]{64}", expected)),
            f"{label} has invalid SHA256")
    data = elf.read(address, size)
    require(hashlib.sha256(data).hexdigest() == expected, f"{label} SHA256 differs")
    return data


def version_range(data: bytes) -> tuple[int, int]:
    require(len(data) == 4, "invalid version descriptor")
    return ((data[0] << 16) | data[1], (data[2] << 16) | data[3])


def verify_initialization_contract(elf: ELF, document: dict) -> tuple[tuple[int, int, int], ...]:
    """Reproduce descriptor decoding, source hashes and the observable v2 table.

    This verifies static evidence, not completion of the running TZ initializer.
    In particular, the parameter blocks precede fallible post-table setup.
    """
    try:
        require(document["schema"] == "sagit-osm-tz-initialization-audit-v1",
                "unsupported initialization audit schema")
        require(document["firmware"]["sha256"] == IMAGE_SHA256,
                "initialization document firmware SHA256 differs")
        verification = document["verification"]
        functions = verification["functions"]
        require(set(functions) == set(INITIALIZATION_FUNCTIONS),
                "initialization function coverage differs")
        for name, (address, size) in INITIALIZATION_FUNCTIONS.items():
            record = functions[name]
            require((integer(record["address"]), integer(record["size"])) == (address, size),
                    f"{name} function range differs")
            verify_hash(elf, address, size, record["sha256"], name)

        parsed_blocks = []
        for array_name, (address, count, stride) in DESCRIPTOR_ARRAYS.items():
            record = verification[array_name]
            require((integer(record["address"]), integer(record["count"]),
                     integer(record["stride"])) == (address, count, stride),
                    f"{array_name} layout differs")
            raw = verify_hash(elf, address, count * stride, record["sha256"], array_name)
            require((integer(record["null_terminator_address"]), integer(record["null_terminator_size"])) ==
                    (address + count * stride, stride), f"{array_name} terminator layout differs")
            terminator = verify_hash(elf, address + count * stride, stride,
                                     record["null_terminator_sha256"], f"{array_name} terminator")
            require(terminator == bytes(stride), f"{array_name} lacks its null terminator")
            single = stride == 16
            records = document["single_write_descriptors" if single else "block_descriptors"]
            require(len(records) == count, f"{array_name} record count differs")
            for index, item in enumerate(records):
                descriptor = raw[index * stride:(index + 1) * stride]
                source, words = struct.unpack_from("<QI", descriptor)
                require(0 < words <= 65536, "descriptor source count is unbounded")
                require(integer(item["descriptor_va"]) == address + index * stride,
                        "descriptor address or order differs")
                require(integer(item["source_va"]) == source, "descriptor source pointer differs")
                require(integer(item["count" if single else "count_words"]) == words,
                        "descriptor source count differs")
                actual_range = version_range(descriptor[12:16] if single else descriptor[24:28])
                require((integer(item["version_min_inclusive"]),
                         integer(item["version_max_exclusive"])) == actual_range,
                        "descriptor version range differs")
                contents = verify_hash(elf, source, words * (16 if single else 4),
                                       item["source_sha256"], f"source at {source:#x}")
                if single:
                    entries = item["entries"]
                    require(len(entries) == words, "single-write entry count differs")
                    for n, entry in enumerate(entries):
                        physical, value, padding = struct.unpack_from("<QII", contents, n * 16)
                        require(padding == 0 and
                                (integer(entry["address"]), integer(entry["value"])) == (physical, value),
                                "single-write entry differs")
                else:
                    _, _, padding, destination, _, tail = struct.unpack("<QIIQ4sI", descriptor)
                    require(padding == 0 and tail == 0, "block descriptor padding differs")
                    require(integer(item["destination"]) == destination,
                            "block descriptor destination differs")
                    expected_kind = {0x170: "branch_table", 0x300: "parameters",
                                     0x500: "sequencer"}.get(destination & 0x1FFF)
                    require(item["kind"] == expected_kind and expected_kind is not None,
                            "block descriptor kind differs")
                    parsed_blocks.append((destination, actual_range, contents))

        fingerprint = document["v2_cold_fingerprint"]
        banks = (0x179C0000, 0x179C2000)
        require(tuple(map(integer, fingerprint["banks"])) == banks, "fingerprint banks differ")
        parameters = fingerprint["parameters"]
        require(tuple(integer(p["seq"]) for p in parameters) == OSM_WRITABLE_SEQUENCERS,
                "fingerprint sequence set or order differs")
        values = []
        for item in parameters:
            number = integer(item["seq"])
            offset = 0x300 + number * 4
            require(integer(item["offset"]) == offset, "fingerprint register offset differs")
            require(tuple(map(integer, item["physical"])) == tuple(b + offset for b in banks),
                    "fingerprint physical address differs")
            initial = tuple(map(integer, item["initial"]))
            require(len(initial) == 2, "fingerprint initial value count differs")
            for cluster, bank in enumerate(banks):
                for version in (0x20000, 0x20001):
                    blocks = [contents for destination, (low, high), contents in parsed_blocks
                              if destination == bank + 0x300 and low <= version < high]
                    require(len(blocks) == 1, "v2 parameter block is missing or ambiguous")
                    require(struct.unpack_from("<I", blocks[0], number * 4)[0] == initial[cluster],
                            f"fingerprint SEQ{number} value differs from firmware")
                require(bank + offset in BASE_ALLOWLIST, "fingerprint address lacks unconditional SCM access")
            values.append((number, *initial))

        for number in (55, 56):
            claimed = tuple(map(integer, document["v2_inherited_parameters"][f"seq{number}"]["initial"]))
            require(len(claimed) == 2, "inherited parameter count differs")
            for cluster, bank in enumerate(banks):
                blocks = [data for destination, (low, high), data in parsed_blocks
                          if destination == bank + 0x300 and low <= 0x20000 < high]
                require(len(blocks) == 1 and
                        struct.unpack_from("<I", blocks[0], number * 4)[0] == claimed[cluster],
                        f"inherited SEQ{number} value differs")
        # These post-table stores are useful static evidence, but their absence
        # from SCM permission tables prevents using them as runtime proof here.
        tail = document["completion_limit"]["candidate_tail_state"]
        tail_addresses = (0x179C1050, 0x179C3050)
        require(tuple(map(integer, tail["physical"])) == tail_addresses and
                integer(tail["expected"]) == 0x179D4020 and
                tuple(map(integer, tail["store_instructions"])) == (0x1C0950F4, 0x1C0950F8),
                "post-table completion evidence differs")
        require(tail["not_written_by_earlier_descriptors"] is True,
                "post-table descriptor-coverage claim differs")
        for physical in tail_addresses:
            require(physical not in (*BASE_ALLOWLIST, *EXTRA_ALLOWLIST),
                    "post-table marker unexpectedly has SCM permission")
            for destination, (low, high), contents in parsed_blocks:
                if low <= 0x20000 < high:
                    require(not destination <= physical < destination + len(contents),
                            "post-table marker is already covered by an earlier block")
            for descriptor in document["single_write_descriptors"]:
                require(all(integer(entry["address"]) != physical for entry in descriptor["entries"]),
                        "post-table marker is already written by a single-write descriptor")
        return tuple(values)
    except (KeyError, TypeError, ValueError, struct.error) as error:
        if isinstance(error, AuditError):
            raise
        raise AuditError(f"malformed initialization audit: {error}") from error


def verify_header_contract(header: str, parameters: tuple[tuple[int, int, int], ...]) -> None:
    """Check the simple constant table; C++ tests cover permitted() control flow."""
    header = re.sub(r"/\*.*?\*/|//[^\n]*", "", header, flags=re.S)
    number = r"(?:0[xX][0-9a-fA-F]+|[0-9]+)[uU]?"
    for name, expected in (("Profile", 1), ("CompleteChecks", (1 << len(parameters)) - 1)):
        matches = re.findall(rf"\bconstexpr\s+uint32_t\s+{name}\s*=\s*({number})\s*;", header)
        require(len(matches) == 1 and integer(matches[0].rstrip("uU")) == expected,
                f"production {name} differs from verified profile")
    matches = re.findall(r"\bconstexpr\s+Parameter\s+BootParameters\s*\[\s*\]\s*=\s*\{(.*?)\};",
                         header, flags=re.S)
    require(len(matches) == 1, "production BootParameters has unsupported declaration")
    body = matches[0]
    pattern = rf"\{{\s*({number})\s*,\s*\{{\s*({number})\s*,\s*({number})\s*\}}\s*\}}"
    actual = tuple(tuple(integer(n.rstrip("uU")) for n in row) for row in re.findall(pattern, body))
    require(not re.sub(pattern, "", body).strip(" \t\r\n,"),
            "production BootParameters has unparsed entries")
    require(actual == parameters, "production BootParameters differs from verified firmware values")


def evidence() -> dict:
    return {
        "schema": "sagit-osm-tz-scm-audit-v1",
        "scope": "Offline static evidence; no firmware modification or hardware access",
        "firmware": {
            "reference": "build/usb/firmware/stock/tz.mbn",
            "sha256": IMAGE_SHA256,
            "architecture": "AArch64 ELF64 little-endian; no section headers required",
        },
        "verification": {
            "command": "python3 scripts/audit-osm-tz.py",
            "method": "Bounded ELF program-header mapping, whole-image SHA256, exact ordered allowlists and handler byte hashes",
            "paired_initialization_audit": "The command also verifies osm-tz-initialization-audit.json function hashes, descriptor decoding, source-block hashes, v2 parameter values and OSMTZContract.h constants.",
            "functions": {name: {"address": hex(address), "size": size, "sha256": digest}
                          for name, (address, size, digest) in FUNCTIONS.items()},
        },
        "abi": {
            "service": 5,
            "read": {"smc_id": "0x02000501", "parameter_id": 1,
                     "arguments": "x2=physical address", "return": "x0=0; x1=32-bit read value or zero if denied"},
            "write": {"smc_id": "0x02000502", "parameter_id": 2,
                      "arguments": "x2=physical address; low32(x3)=value",
                      "return": "x0=0 even if denied; denied writes do not access the target"},
            "function_id_mask": "0x3f00ffff in monitor dispatcher; atomic/64-bit flags do not change this allowlist",
            "availability": "Service availability is independent of whether a particular register is permitted",
            "dispatcher_evidence": [
                "0x14682388..0x146823b4 selects WRITE and invokes 0x14683960",
                "0x146823f8 stores zero to saved x0 after WRITE",
                "0x14682400..0x1468240c invokes READ, truncates to 32 bits, stores x0=0/x1=value",
            ],
        },
        "allowlist": {
            "shared_by_read_and_write": True,
            "fixed_table": {"address": hex(BASE_TABLE), "count": 40,
                            "addresses": [hex(a) for a in BASE_ALLOWLIST]},
            "conditional_table": {"address": hex(EXTRA_TABLE), "count": 10,
                                  "addresses": [hex(a) for a in EXTRA_ALLOWLIST],
                                  "gate": "((read32(0x01fc8000) >> 8) & 0xff) == 1",
                                  "gate_evidence": "0x14681314..0x14681324 loads a word then extracts bits15:8; caller0x14683a04 compares to1",
                                  "interpretation": "Exact hardware-version field branch; no board-ID condition appears in this handler"},
            "osm_fixed_seq": list(OSM_WRITABLE_SEQUENCERS),
            "osm_conditional_seq": list(OSM_CONDITIONAL_SEQUENCERS),
            "osm_banks": ["0x179c0000", "0x179c2000"],
            "seq_address_formula": "bank + 0x300 + 4 * sequence_number",
            "other_write_special_case": "0x01fd3000 low-nibble-zero writes are suppressed when byte_1c006ba0 equals1; this does not involve OSM",
        },
        "denial_evidence": {
            "read": "0x14683a68 branches around LDR W0,[X19]; denied path0x14683a74 sets W0=0",
            "write": "0x14683980 branches around STR W19,[X20]; both paths reach MOV W0,WZR at0x146839b4",
            "consequence": "SCM status success cannot establish address permission; readback of zero cannot prove a zero write occurred",
        },
        "osm_findings": {
            "unreadable_old_firmware_marker_seq": list(OLD_MARKER_SEQUENCERS),
            "old_marker_read_result": "All eleven markers are excluded from both tables; this firmware returns success/value0 for every marker",
            "unwritable_lower_mem_acc_threshold_seq": [55, 56],
            "lower_threshold_write_result": "Silent no-op on both clusters; these writes in Xiaomi clock-osm.c do not prove this firmware accepts them",
            "permitted_dynamic_writes": list(OSM_WRITABLE_SEQUENCERS),
            "safe_inference": "A rejected marker read cannot show that TZ omitted initialization",
            "direct_mmio": "Not established by this SCM whitelist audit. Do not bypass it with direct nonsecure reads without separate xPU/access evidence.",
            "initialization_evidence": "docs/power/osm-tz-initialization-audit.json reproduces v2 table selection, inherited values, descriptor contents and source-block hashes.",
            "completion_limit": "The eight SCM-readable boot values precede fallible protected post-table setup. The original caller discards its failure return, so matching these values cannot distinguish partial initialization from full completion.",
            "unreadable_tail_state": "Both banks' +0x404, +0x410 and +0x1050 are excluded from both SCM tables. +0x1050=0x179d4020 is written at the examined success tail, but direct nonsecure read permission has not been established.",
            "retained_prerequisite": "Protected post-initialization success remains a stock-bootchain prerequisite; this offline profile and runtime boot-value match do not independently prove it.",
        },
        "source_cross_reference": [
            "docs/power/reference/xiaomi-clock-osm.c:2072-2092 issues SCM writes to55..58 and32 without checking per-address acceptance",
            "build/usb/reference/bsp/trustzone_images/core/securemsm/trustzone/qsee/include/tz_syscall_pub.h:2313-2366 documents the IO service IDs/parameters; actual pinned firmware silently denies disallowed addresses",
        ],
    }


def audit_bytes(data: bytes) -> dict:
    elf = parse_elf(data)
    if hashlib.sha256(data).hexdigest() != IMAGE_SHA256:
        raise AuditError("unsupported firmware SHA256; this audit applies only to the pinned image")
    verify_static_contract(elf)
    return evidence()


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--image", type=Path, default=DEFAULT_IMAGE)
    parser.add_argument("--initialization-audit", type=Path, default=DEFAULT_INITIALIZATION)
    parser.add_argument("--contract-header", type=Path, default=DEFAULT_HEADER)
    parser.add_argument("--output", type=Path)
    args = parser.parse_args()
    try:
        data = args.image.read_bytes()
        result = audit_bytes(data)
        parameters = verify_initialization_contract(parse_elf(data),
                                                    json.loads(args.initialization_audit.read_text()))
        verify_header_contract(args.contract_header.read_text(), parameters)
    except (OSError, AuditError, json.JSONDecodeError) as error:
        parser.exit(1, f"OSM TZ audit failed: {error}\n")
    text = json.dumps(result, indent=2) + "\n"
    if args.output:
        args.output.write_text(text)
    else:
        print(text, end="")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
