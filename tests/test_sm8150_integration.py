"""Real device tree conversion and target/console selection boundaries."""
import copy
import importlib.util
from pathlib import Path
import struct
import subprocess
import tempfile
import unittest
from unittest import mock

ROOT = Path(__file__).resolve().parents[1]
spec = importlib.util.spec_from_file_location("sm8150_bridge", ROOT / "scripts/build-bridge.py")
bridge = importlib.util.module_from_spec(spec)
spec.loader.exec_module(bridge)
from test_boot_bridge import apple_nodes
import toolchain


class SM8150IntegrationTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        with tempfile.TemporaryDirectory() as tmp:
            output = Path(tmp) / "sm8150.dtb"
            subprocess.run(["dtc", "-q", "-I", "dts", "-O", "dtb", "-i",
                            str(ROOT / "device_trees"), "-o", str(output),
                            str(ROOT / "device_trees/sm8150-generic.dts")], check=True)
            cls.tree = bridge.parse_fdt(output.read_bytes())

    def test_real_tree_cpu_topology_and_gic(self):
        nodes = apple_nodes(bridge.apple_dt(self.tree, bytes(64)))
        cpus = [v for v in nodes.values() if v.get("device_type") == b"cpu\0"]
        self.assertEqual(sorted(struct.unpack("<I", v["reg"])[0] for v in cpus),
                         list(range(0, 0x800, 0x100)))
        for node in cpus:
            cpu = struct.unpack("<I", node["reg"])[0] >> 8
            group = 0 if cpu < 4 else 1 if cpu < 7 else 2
            self.assertEqual(struct.unpack("<I", node["die-cluster-id"])[0], group)
            self.assertEqual(struct.unpack("<I", node["l2-cache-id"])[0], cpu)
        gic = next(v for v in nodes.values()
                   if b"arm,gic-v3" in v.get("compatible", b"").split(b"\0"))
        self.assertEqual(struct.unpack("<4Q", gic["reg"]),
                         (0x17a00000, 0x10000, 0x17a60000, 0x100000))
        chosen = nodes["/device-tree/chosen"]
        self.assertEqual(struct.unpack("<I", chosen["xnu,allow-no-uart"])[0], 1)
        self.assertNotIn("serial-device", chosen)
        self.assertNotIn(b"qcom.usb-cdc=1", chosen["bootargs"])
        self.assertIn(b"cpus=1", chosen["bootargs"])
        for v in nodes.values():
            compatible = v.get("compatible", b"")
            if b"dwc3" in compatible or b"geni" in compatible:
                self.assertEqual(v.get("status"), b"disabled\0")

    def test_target_collection_does_not_import_835_peripherals(self):
        with mock.patch.object(toolchain, "TARGET", "SM8150"):
            config = toolchain.driver_configuration()
        self.assertEqual(set(config["collections"]), {"platform"})
        roots = config["collections"]["platform"]["roots"]
        self.assertIn("org.mi6.driver.SM8150", roots)
        for predecessor in ("org.mi6.driver.SDM835", "org.darwin.driver.QcomGCC",
                            "org.darwin.driver.QcomSPMI"):
            self.assertNotIn(predecessor, roots)
        self.assertIn("sm8150", config["collections"]["platform"]["output"])
        with mock.patch.object(toolchain, "TARGET", "SDM835"):
            self.assertIn("usb", toolchain.driver_configuration()["collections"])

    def test_cdc_opt_in_roundtrip_does_not_enable_hardware(self):
        tree = copy.deepcopy(self.tree)
        before = copy.deepcopy(tree["children"])
        # Synthetic non-shipping identity only; no hardware is exercised.
        bridge.configure_usb_cdc_console(tree, 0x1234, 0x5678)
        bridge.configure_usb_cdc_console(tree, 0x1234, 0x5678)
        nodes = apple_nodes(bridge.apple_dt(tree, bytes(64)))
        args = nodes["/device-tree/chosen"]["bootargs"].rstrip(b"\0").split()
        self.assertEqual(args.count(b"qcom.usb-cdc=1"), 1)
        self.assertEqual(args.count(b"serial=3"), 1)
        self.assertIn(b"qcom.usb-cdc-vid=4660", args)
        self.assertIn(b"qcom.usb-cdc-pid=22136", args)
        self.assertEqual([n for n in before if n["name"] != "chosen"],
                         [n for n in tree["children"] if n["name"] != "chosen"])

    def test_cdc_invalid_ids_do_not_mutate_tree(self):
        for vid, pid in ((0, 1), (1, 0), (-1, 1), (65535, 1), (65536, 1), (True, 1), ("1", 2)):
            tree = copy.deepcopy(self.tree)
            with self.assertRaises(ValueError):
                bridge.configure_usb_cdc_console(tree, vid, pid)
            self.assertEqual(tree, self.tree)


if __name__ == "__main__":
    unittest.main()
