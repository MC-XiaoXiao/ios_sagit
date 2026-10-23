"""Firmware framing and opt-in DT handoff must fail before malformed input is used."""
import importlib.util
import json
from pathlib import Path
import struct
import tempfile
import unittest
import hashlib
from unittest import mock

ROOT = Path(__file__).resolve().parents[1]
spec = importlib.util.spec_from_file_location('input_firmware',ROOT/'scripts/input_firmware.py')
module = importlib.util.module_from_spec(spec); spec.loader.exec_module(module)


def application():
    mdt=bytearray(84); mdt[:7]=b'\x7fELF\x01\x01\x01'
    struct.pack_into('<H',mdt,18,40); struct.pack_into('<I',mdt,28,52)
    struct.pack_into('<HH',mdt,42,32,1); struct.pack_into('<I',mdt,68,4)
    return bytes(mdt)


def tree():
    return {'name':'','props':{},'children':[
        {'name':'fingerprint','children':[],'props':{'compatible':b'goodix,fingerprint\0fpc,fpc1020\0','status':b'disabled\0',
            **{key:struct.pack('>III',0x100,value,flags) for key,value,flags in [('power-gpios',35,0),('reset-gpios',37,1),('irq-gpios',121,0)]}}},
        {'name':'gpio','children':[],'props':{'compatible':b'qcom,msm8998-pinctrl\0','phandle':struct.pack('>I',0x100),'qcom,owned-gpios':struct.pack('>I',99)}},
        {'name':'gcc','children':[],'props':{'compatible':b'qcom,gcc-msm8998\0','qcom,owned-clocks':struct.pack('>II',116,0)}}]}


class InputFirmwareTests(unittest.TestCase):
    def test_signed_application_framing(self):
        packet=module.application_packet(application(),[b'abcd'])
        self.assertEqual(struct.unpack_from('<I',packet)[0],84)
        self.assertEqual(packet[4:88],application()); self.assertEqual(packet[88:],b'abcd')
        for mdt,segments in [(b'not elf',[b'abcd']),(application(),[]),(application(),[b'short'])]:
            with self.subTest(mdt=mdt,segments=segments),self.assertRaises(ValueError): module.application_packet(mdt,segments)

    def test_opt_in_handoff_preserves_other_owners_and_checks_packets(self):
        packet=module.application_packet(application(),[b'abcd'])
        expected={name:hashlib.sha256(packet).hexdigest() for name in module.APPLICATIONS}
        with tempfile.TemporaryDirectory() as temp,mock.patch.dict(module.APPLICATION_SHA256,expected):
            directory=Path(temp)
            manifest={'archive_sha256':module.FIRMWARE_SHA256,'applications':{}}
            for name,prop in module.APPLICATIONS.items():
                (directory/(name+'.bin')).write_bytes(packet)
                manifest['applications'][prop]={'file':name+'.bin','sha256':hashlib.sha256(packet).hexdigest(),'bytes':len(packet)}
            (directory/'manifest.json').write_text(json.dumps(manifest))
            t=tree(); module.configure_input_firmware(t,directory)
            self.assertEqual(t['children'][0]['props']['status'],b'okay\0')
            self.assertEqual(list(struct.iter_unpack('>I',t['children'][1]['props']['qcom,owned-gpios'])),[(35,),(37,),(99,),(121,)])
            self.assertEqual(dict(struct.iter_unpack('>II',t['children'][2]['props']['qcom,owned-clocks'])),{54:0,66:0,116:0})
            module.configure_input_firmware(t,directory) # idempotent merge
            (directory/'goodixfp.bin').write_bytes(b'changed')
            with self.assertRaisesRegex(ValueError,'modified'): module.configure_input_firmware(tree(),directory)
            manifest['applications']['goodix-firmware']['sha256']=hashlib.sha256(b'changed').hexdigest()
            (directory/'manifest.json').write_text(json.dumps(manifest))
            with self.assertRaisesRegex(ValueError,'unrecognized'): module.configure_input_firmware(tree(),directory)

    def test_fat_allocated_extent_and_loop_are_checked_even_with_trimmed_tail(self):
        # A 4096-cluster volume with an omitted unused tail, as in NON-HLOS.
        sector=512; data=bytearray(21*sector+sector)
        data[510:512]=b'\x55\xaa'; struct.pack_into('<HBHBH',data,11,sector,1,1,1,16)
        struct.pack_into('<H',data,19,4118); struct.pack_into('<H',data,22,17)
        root=18*sector; data[root:root+11]=b'TEST    BIN'; struct.pack_into('<H',data,root+26,2)
        struct.pack_into('<I',data,root+28,4); struct.pack_into('<H',data,sector+4,0xffff)
        start=19*sector; data[start:start+4]=b'test'
        self.assertEqual(module.FAT16(bytes(data)).read('test.bin'),b'test')
        struct.pack_into('<H',data,sector+4,2)
        with self.assertRaisesRegex(ValueError,'cyclic'): module.FAT16(bytes(data)).read('test.bin')
        struct.pack_into('<H',data,sector+4,100)
        with self.assertRaisesRegex(ValueError,'truncated'): module.FAT16(bytes(data)).read('test.bin')


if __name__=='__main__': unittest.main()
