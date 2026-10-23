"""ADSP source, FDT and Apple matching integration; strictly offline."""
import hashlib
import importlib.util
import json
from pathlib import Path
import plistlib
import struct
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]


def load_bridge():
    spec = importlib.util.spec_from_file_location("adsp_bridge", ROOT / "scripts/build-bridge.py")
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


def unpack_apple(data, at=0):
    count, children = struct.unpack_from("<II", data, at)
    at += 8
    props = {}
    for _ in range(count):
        key = data[at:at + 32].split(b"\0")[0].decode()
        size, = struct.unpack_from("<I", data, at + 32)
        props[key] = data[at + 36:at + 36 + size]
        at += 36 + ((size + 3) & ~3)
    nodes = []
    for _ in range(children):
        child, at = unpack_apple(data, at)
        nodes.append(child)
    return {"props": props, "children": nodes}, at


def find_node(node, name):
    if node["props"].get("name", b"").split(b"\0")[0].decode() == name:
        return node
    for child in node["children"]:
        found = find_node(child, name)
        if found:
            return found
    return None


class ADSPIntegrationTests(unittest.TestCase):
    def test_pinned_protocol_references(self):
        folder = ROOT / "docs/audio/reference"
        manifest = json.loads((folder / "sources.json").read_text())
        self.assertTrue({"apr_audio-v2.h", "qcom_glink_native.c", "qcom_glink_smem.c",
                         "q6asm.c", "q6adm.c", "q6core.c", "smp2p.c"}.issubset(manifest))
        for name, record in manifest.items():
            with self.subTest(name=name):
                self.assertRegex(record["url"], r"/[0-9a-f]{40}/")
                self.assertEqual(hashlib.sha256((folder / name).read_bytes()).hexdigest(), record["sha256"])

    def test_default_board_and_native_endianness(self):
        bridge = load_bridge()
        with tempfile.TemporaryDirectory() as directory:
            dtb = Path(directory) / "sagit.dtb"
            subprocess.run(["dtc", "-q", "-I", "dts", "-O", "dtb", "-i", str(ROOT / "device_trees"),
                            "-o", str(dtb), str(ROOT / "device_trees/xiaomi-sagit.dts")], check=True)
            native, end = unpack_apple(bridge.apple_dt(bridge.parse_fdt(dtb.read_bytes())))
        node = find_node(native, "audio-transport@17911000")
        self.assertIsNotNone(node)
        self.assertEqual(node["props"]["status"], b"disabled\0")
        self.assertNotIn("qcom,adsp-handoff", node["props"])
        self.assertEqual(struct.unpack("<2Q", node["props"]["reg"]), (0x17911000, 0x1000))
        shared = find_node(native, "shared-memory@86000000")
        self.assertEqual(shared["props"]["status"], b"disabled\0")
        self.assertEqual(struct.unpack("<4Q", shared["props"]["reg"]),
                         (0x86000000, 0x200000, 0x01f40000, 0x20000))
        self.assertEqual(struct.unpack("<2I", node["props"]["interrupts"]), (189, 0))
        self.assertGreater(end, 0)

    def test_handoff_is_an_opaque_little_endian_record(self):
        # Test the actual Apple-DT converter. The source FDT uses an incbin
        # style opaque record, whereas chosen/memory-map uses numeric tuples.
        bridge = load_bridge()
        handoff = struct.pack("<IHHIIQQQQQIHHII", 0x50534441, 1, 72, 127, 0, 19,
                              0x8b200000, 0x1000000, 0x90000000, 0x10000000,
                              16384, 1, 2, 0x1006, 512)
        self.assertEqual(len(handoff), 72)
        tree = {"name": "", "props": {}, "children": [
            {"name": "chosen", "props": {}, "children": [
                {"name": "memory-map", "props": {
                    "ADSPFirmware": struct.pack(">QQ", 0x8b200000, 0x1000000),
                    "ADSPPCM": struct.pack(">QQ", 0x90000000, 16384),
                    "QcomSMEM": struct.pack(">QQ", 0x86000000, 0x200000)}, "children": []}]},
            {"name": "audio", "props": {"qcom,adsp-handoff": handoff}, "children": []}]}
        native, _ = unpack_apple(bridge.apple_dt(tree))
        self.assertEqual(find_node(native, "audio")["props"]["qcom,adsp-handoff"], handoff)
        props = find_node(native, "memory-map")["props"]
        self.assertEqual(struct.unpack("<2Q", props["ADSPPCM"]), (0x90000000, 16384))
        self.assertEqual(struct.unpack("<2Q", props["ADSPFirmware"]), (0x8b200000, 0x1000000))
        self.assertEqual(struct.unpack("<2Q", props["QcomSMEM"]), (0x86000000, 0x200000))

    def test_backend_matching_and_compiler_contracts(self):
        plist = plistlib.loads((ROOT / "drivers/audio/QcomAudio/Info.plist").read_bytes())
        p = plist["IOKitPersonalities"]
        self.assertEqual(p["QcomADSP"]["IOProviderClass"], "AppleARMIODevice")
        self.assertEqual(p["QcomADSP"]["IONameMatch"], "qcom,msm8998-adsp-transport")
        self.assertEqual(p["QcomAudio"]["IOPropertyMatch"], {"AudioOutputReady": True})
        config = json.loads((ROOT / "drivers/Build.json").read_text())
        layout = config["vtable_contracts"]["QcomAudio"]["additional_classes"]["QcomADSP"]
        self.assertEqual(layout["slots"], 174)
        self.assertEqual(layout["bindings"]["168"], "QcomADSP::outputConfiguration")
        self.assertEqual(layout["bindings"]["173"], "QcomADSP::releaseOutput")
        self.assertIn("audio-adsp", config["collections"]["audio"]["tests"])
        self.assertIn("../docs/audio/reference/sources.json", config["source_manifests"])


if __name__ == "__main__":
    unittest.main()
