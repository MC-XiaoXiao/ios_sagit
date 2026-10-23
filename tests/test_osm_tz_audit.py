"""Offline SCM-IO evidence and hostile ELF bounds tests; never opens a device."""
import importlib.util
import copy
import json
import struct
import sys
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
SPEC = importlib.util.spec_from_file_location("osm_tz_audit", ROOT / "scripts/audit-osm-tz.py")
AUDIT = importlib.util.module_from_spec(SPEC)
sys.modules[SPEC.name] = AUDIT
SPEC.loader.exec_module(AUDIT)


def synthetic_elf():
    data = bytearray(0x200)
    ident = b"\x7fELF\x02\x01\x01" + bytes(9)
    struct.pack_into("<16sHHIQQQIHHHHHH", data, 0, ident, 2, 183, 1,
                     0x1000, 64, 0, 0, 64, 56, 1, 0, 0, 0)
    struct.pack_into("<IIQQQQQQ", data, 64, 1, 5, 0x100, 0x1000, 0x1000, 0x100, 0x100, 1)
    return data


class ELFBoundsTests(unittest.TestCase):
    def test_minimal_file_without_sections(self):
        elf = AUDIT.parse_elf(bytes(synthetic_elf()))
        self.assertEqual(elf.read(0x1000, 16), bytes(16))

    def test_header_truncation_and_endianness(self):
        for length in (0, 7, 63, 100):
            with self.subTest(length=length), self.assertRaises(AUDIT.AuditError):
                AUDIT.parse_elf(bytes(synthetic_elf()[:length]))
        image = synthetic_elf()
        image[5] = 2
        with self.assertRaises(AUDIT.AuditError):
            AUDIT.parse_elf(bytes(image))

    def test_program_header_bounds(self):
        for offset in (0, 0x1FF, (1 << 64) - 1):
            image = synthetic_elf()
            struct.pack_into("<Q", image, 32, offset)
            with self.subTest(offset=offset), self.assertRaises(AUDIT.AuditError):
                AUDIT.parse_elf(bytes(image))
        image = synthetic_elf()
        struct.pack_into("<H", image, 56, 0xFFFF)
        with self.assertRaises(AUDIT.AuditError):
            AUDIT.parse_elf(bytes(image))

    def test_segment_file_bounds(self):
        for field, value in ((72, 0x201), (96, 0x200), (96, (1 << 64) - 1)):
            image = synthetic_elf()
            struct.pack_into("<Q", image, field, value)
            with self.subTest(field=field, value=value), self.assertRaises(AUDIT.AuditError):
                AUDIT.parse_elf(bytes(image))

    def test_virtual_overflow_and_alignment(self):
        image = synthetic_elf()
        struct.pack_into("<Q", image, 80, (1 << 64) - 0x80)
        with self.assertRaises(AUDIT.AuditError):
            AUDIT.parse_elf(bytes(image))
        image = synthetic_elf()
        struct.pack_into("<Q", image, 112, 3)
        with self.assertRaises(AUDIT.AuditError):
            AUDIT.parse_elf(bytes(image))

    def test_overlapping_segments(self):
        image = synthetic_elf()
        struct.pack_into("<H", image, 56, 2)
        image[120:176] = image[64:120]
        with self.assertRaises(AUDIT.AuditError):
            AUDIT.parse_elf(bytes(image))

    def test_reads_must_be_file_backed_and_bounded(self):
        image = synthetic_elf()
        struct.pack_into("<Q", image, 104, 0x200)
        elf = AUDIT.parse_elf(bytes(image))
        for address, size in ((0xFFF, 4), (0x10FF, 2), (0x1100, 4),
                              (-1, 4), (0x1000, 0), (0x1000, -1), ((1 << 64) - 1, 4)):
            with self.subTest(address=address, size=size), self.assertRaises(AUDIT.AuditError):
                elf.read(address, size)

    def test_unrecognized_elf_is_rejected_by_audit(self):
        with self.assertRaisesRegex(AUDIT.AuditError, "SHA256"):
            AUDIT.audit_bytes(bytes(synthetic_elf()))


class HeaderContractTests(unittest.TestCase):
    PARAMETERS = ((15, 0, 0), (31, 0, 0), (32, 0, 0), (57, 0x10, 0x0B),
                  (58, 0x11, 0x0C), (72, 0x16, 0x19), (76, 0x4B9, 0x5B9), (88, 0, 0))

    def test_header_matches_document_even_without_firmware(self):
        document = json.loads(AUDIT.DEFAULT_INITIALIZATION.read_text())
        declared = tuple((p["seq"], *(int(n, 0) for n in p["initial"]))
                         for p in document["v2_cold_fingerprint"]["parameters"])
        self.assertEqual(declared, self.PARAMETERS)
        AUDIT.verify_header_contract(AUDIT.DEFAULT_HEADER.read_text(), declared)

    def test_header_drift_is_rejected(self):
        header = AUDIT.DEFAULT_HEADER.read_text()
        mutations = (
            ("Profile = 1", "Profile = 2"),
            ("CompleteChecks = 0xff", "CompleteChecks = 0x7f"),
            ("{57, {0x10, 0x0b}}", "{55, {0x10, 0x0b}}"),
            ("{57, {0x10, 0x0b}}", "{57, {0x10, 0x0c}}"),
            ("{15, {0, 0}}", "{15, {1 << 0, 0}}"),
        )
        for old, new in mutations:
            self.assertIn(old, header)
            with self.subTest(new=new), self.assertRaises(AUDIT.AuditError):
                AUDIT.verify_header_contract(header.replace(old, new), self.PARAMETERS)


@unittest.skipUnless(AUDIT.DEFAULT_IMAGE.is_file(), "pinned stock TZ image not present")
class PinnedFirmwareTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.image = AUDIT.DEFAULT_IMAGE.read_bytes()
        cls.elf = AUDIT.parse_elf(cls.image)
        cls.initialization = json.loads(AUDIT.DEFAULT_INITIALIZATION.read_text())

    def mutated(self, address):
        segment = next(s for s in self.elf.segments
                       if s.virtual <= address < s.virtual + s.file_size)
        data = bytearray(self.image)
        data[segment.offset + address - segment.virtual] ^= 1
        return bytes(data)

    def test_checked_document_is_reproducible(self):
        actual = AUDIT.audit_bytes(self.image)
        saved = json.loads((ROOT / "docs/power/osm-tz-scm-audit.json").read_text())
        self.assertEqual(actual, saved)

    def test_tables_and_handlers_are_verified_separately(self):
        for address in (AUDIT.BASE_TABLE, AUDIT.EXTRA_TABLE):
            with self.subTest(address=address):
                image = self.mutated(address)
                with self.assertRaisesRegex(AUDIT.AuditError, "allowlist"):
                    AUDIT.verify_static_contract(AUDIT.parse_elf(image))
                with self.assertRaisesRegex(AUDIT.AuditError, "SHA256"):
                    AUDIT.audit_bytes(image)
        for name, (address, _, _) in AUDIT.FUNCTIONS.items():
            with self.subTest(name=name), self.assertRaisesRegex(AUDIT.AuditError, name):
                AUDIT.verify_static_contract(AUDIT.parse_elf(self.mutated(address)))

    def test_exact_osm_permissions_and_rejected_markers(self):
        for base in (0x179C0000, 0x179C2000):
            fixed = {a for a in AUDIT.BASE_ALLOWLIST if base <= a < base + 0x2000}
            self.assertEqual(fixed, {base + 0x300 + 4*n for n in AUDIT.OSM_WRITABLE_SEQUENCERS})
            extra = {a for a in AUDIT.EXTRA_ALLOWLIST if base <= a < base + 0x2000}
            self.assertEqual(extra, {base + 0x300 + 4*n for n in AUDIT.OSM_CONDITIONAL_SEQUENCERS})
            for number in (*AUDIT.OLD_MARKER_SEQUENCERS, 55, 56):
                self.assertNotIn(base + 0x300 + 4*number, fixed | extra)
            for offset in (0x404, 0x410, 0x1050):
                self.assertNotIn(base + offset, fixed | extra)

    def test_initialization_tables_and_production_constants(self):
        parameters = AUDIT.verify_initialization_contract(self.elf, self.initialization)
        self.assertEqual(parameters, HeaderContractTests.PARAMETERS)
        AUDIT.verify_header_contract(AUDIT.DEFAULT_HEADER.read_text(), parameters)

    def test_initialization_binary_changes_are_rejected(self):
        addresses = {address for address, _ in AUDIT.INITIALIZATION_FUNCTIONS.values()}
        for name in AUDIT.DESCRIPTOR_ARRAYS:
            addresses.add(int(self.initialization["verification"][name]["null_terminator_address"], 0))
        for group in ("single_write_descriptors", "block_descriptors"):
            for descriptor in self.initialization[group]:
                addresses.add(int(descriptor["descriptor_va"], 0))
                addresses.add(int(descriptor["source_va"], 0))
        for address in sorted(addresses):
            with self.subTest(address=hex(address)), self.assertRaises(AUDIT.AuditError):
                AUDIT.verify_initialization_contract(AUDIT.parse_elf(self.mutated(address)),
                                                     self.initialization)

    def test_initialization_document_drift_is_rejected(self):
        mutations = (
            (("schema",), "unknown"),
            (("firmware", "sha256"), "0" * 64),
            (("verification", "block_descriptor_array", "stride"), 16),
            (("verification", "block_descriptor_array", "null_terminator_size"), 16),
            (("verification", "functions", "initializer", "sha256"), "0" * 64),
            (("single_write_descriptors", 0, "source_va"), "0xfffffffffffffff0"),
            (("single_write_descriptors", 0, "count"), 0xFFFFFFFF),
            (("single_write_descriptors", 0, "entries", 0, "value"), "0x1"),
            (("block_descriptors", 0, "descriptor_va"), "0x1c0ec068"),
            (("block_descriptors", 6, "version_min_inclusive"), "0x20001"),
            (("block_descriptors", 6, "kind"), "parameters"),
            (("block_descriptors", 10, "destination"), "0x179c2300"),
            (("block_descriptors", 10, "source_sha256"), "0" * 64),
            (("v2_cold_fingerprint", "parameters", 0, "physical", 0), "0x179c0300"),
            (("v2_cold_fingerprint", "parameters", 3, "initial", 1), "0xc"),
            (("v2_inherited_parameters", "seq55", "initial", 0), "0xff"),
            (("completion_limit", "candidate_tail_state", "expected"), "0x0"),
        )
        for path, value in mutations:
            document = copy.deepcopy(self.initialization)
            node = document
            for key in path[:-1]:
                node = node[key]
            node[path[-1]] = value
            with self.subTest(path=path), self.assertRaises(AUDIT.AuditError):
                AUDIT.verify_initialization_contract(self.elf, document)
        document = copy.deepcopy(self.initialization)
        document["block_descriptors"].pop()
        with self.assertRaises(AUDIT.AuditError):
            AUDIT.verify_initialization_contract(self.elf, document)


if __name__ == "__main__":
    unittest.main()
