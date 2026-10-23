"""Offline checks against the original iOS provider, never device access."""
import hashlib
import json
from pathlib import Path
import plistlib
import struct
import unittest

ROOT = Path(__file__).resolve().parents[1]


class AudioIntegrationTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.evidence = json.loads((ROOT / "docs/audio/native-interface.json").read_text())
        cls.contract = json.loads((ROOT / "drivers/abi-22H374/apple-audio2-abi.json").read_text())
        cls.image = (ROOT / cls.evidence["provider_image"]).read_bytes()
        cls.segments = []
        cls.assert_magic = struct.unpack_from("<I", cls.image)[0]
        offset = 32
        for _ in range(struct.unpack_from("<I", cls.image, 16)[0]):
            command, size = struct.unpack_from("<II", cls.image, offset)
            if command == 0x19:
                vm, _, fileoff, filesize = struct.unpack_from("<QQQQ", cls.image, offset + 24)
                cls.segments.append((vm, fileoff, filesize))
            offset += size

    def read_vm(self, address, size):
        for vm, fileoff, length in self.segments:
            if vm <= address and address + size <= vm + length:
                start = fileoff + address - vm
                return self.image[start:start + size]
        self.fail("native evidence address is not file backed")

    def test_pinned_native_and_board_evidence(self):
        self.assertEqual(self.assert_magic, 0xfeedfacf)
        digest = hashlib.sha256(self.image).hexdigest()
        self.assertEqual(digest, self.evidence["sha256"])
        self.assertEqual(digest, self.contract["image_sha256"])
        for path, expected in self.evidence["board_evidence"].items():
            self.assertEqual(hashlib.sha256((ROOT / path).read_bytes()).hexdigest(), expected)
        self.assertFalse(self.evidence["hardware_tested"])
        self.assertFalse(self.evidence["board_backend_implemented"])

    def test_native_dispatch_selectors_and_argument_sizes(self):
        # IOExternalMethod: target, virtual byte offset, is-virtual, flags,
        # count0, count1. Read the original provider independently of our
        # C++ header; detect a plausible but incorrect slot/signature mapping.
        addr = int(self.evidence["evidence_addresses"]["native_dispatch_table"], 16)
        entries = [
            (169, 0, 0, 0), (170, 0, 0, 0), (183, 0, 2, 1),
            (187, 4, 0, 32), (188, 4, 0, 8), (189, 4, 1, 40),
            (190, 0, 2, 0), (191, 0, 3, 0), (184, 3, 0xffffffff, 0xffffffff),
            (185, 0, 3, 0), (171, 0, 1, 1), (172, 0, 1, 1), (192, 0, 2, 0),
        ]
        for selector, (slot, flags, n0, n1) in enumerate(entries):
            with self.subTest(selector=selector):
                fields = struct.unpack("<6Q", self.read_vm(addr + selector * 48, 48))
                self.assertEqual(fields, (0, slot * 8, 1, flags, n0, n1))

    def test_native_newuserclient_and_slot_contract(self):
        bindings = self.contract["virtual_symbols"]
        expected = "__ZN14IOAudio2Device13newUserClientEP4taskPvjP12OSDictionaryPP12IOUserClient"
        self.assertEqual(bindings[expected], {"class": "IOAudio2Device", "slot": 140})
        slots = {v["slot"] for v in bindings.values()}
        self.assertTrue(set(range(168, 197)).issubset(slots))
        self.assertEqual(self.contract["classes"]["IOAudio2Device"],
                         {"super": "IOService", "instance_size": 232, "vtable_slots": 197})
        # Native entitlement requirement is present in the exact hashed binary.
        self.assertIn(b"com.apple.private.audio.driver-host\0", self.image)

    def test_collection_and_matching_do_not_instantiate_abstract_backend(self):
        config = json.loads((ROOT / "drivers/Build.json").read_text())
        self.assertIn("abi-22H374/apple-audio2-abi.json", config["abi_contracts"])
        audio = config["collections"]["audio"]
        self.assertTrue({"org.darwin.driver.QcomAudio", "org.darwin.driver.QcomSMEM"}.issubset(audio["roots"]))
        self.assertTrue({"audio-output", "audio-adsp", "baseband-smem-pool"}.issubset(audio["tests"]))
        p = plistlib.loads((ROOT / "drivers/audio/QcomAudio/Info.plist").read_bytes())
        self.assertEqual(p["OSBundleLibraries"]["com.apple.iokit.IOAudio2Family"], "1.0")
        self.assertEqual(p["OSBundleLibraries"]["org.darwin.driver.QcomSMEM"], "1.0.0")
        personality = p["IOKitPersonalities"]["QcomAudio"]
        self.assertEqual(personality["IOProviderClass"], "AudioOutputBackend")
        for path in (ROOT / "drivers").glob("*/*/Info.plist"):
            info = plistlib.loads(path.read_bytes())
            for entry in info.get("IOKitPersonalities", {}).values():
                self.assertNotEqual(entry.get("IOClass"), "AudioOutputBackend", path)
        vtable = config["vtable_contracts"]["QcomAudio"]
        self.assertEqual(vtable["slots"], 197)
        self.assertEqual(vtable["bindings"]["140"], "IOAudio2Device::newUserClient")
        self.assertEqual(vtable["bindings"]["182"], "IOAudio2Device::clientMemoryForType")


if __name__ == "__main__":
    unittest.main()
