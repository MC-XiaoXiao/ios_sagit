import plistlib
from pathlib import Path
import struct
import sys
import unittest

sys.path.insert(0, str(Path(__file__).resolve().parents[1]/'scripts'))
from macho_development_sign import resign_macho, verify_macho, signature_blobs, sign_new_macho
from bridge_trustcache import code_directory_entries, der_item


def fixture():
    off = 8192; capacity = 8192
    code = bytearray(bytes(range(256))*32)
    code[:32] = struct.pack('<8I', 0xfeedfacf, 0x100000c, 0, 2, 2, 88, 0, 0)
    code[32:48] = struct.pack('<4I', 0x1d, 16, off, capacity)
    code[48:120] = struct.pack('<II16sQQQQiiII', 0x19, 72, b'__TEXT', 0x100000000, off, 0, off, 5, 5, 0, 0)
    ent = plistlib.dumps({'com.apple.private.iowatchdog.user-access': True})
    blobs = {0: struct.pack('>II', 0xfade0c02, 8), 2: struct.pack('>III', 0xfade0c01, 12, 0),
             5: struct.pack('>II', 0xfade7171, 8+len(ent))+ent,
             7: struct.pack('>II', 0xfade7172, 10)+b'\x70\0'}
    cursor = 12+8*len(blobs); indexes = []; bodies = []
    for kind, blob in blobs.items():
        indexes.append(struct.pack('>II', kind, cursor)); bodies.append(blob); cursor += len(blob)
    signature = struct.pack('>III', 0xfade0cc0, cursor, len(blobs))+b''.join(indexes+bodies)
    return bytes(code)+signature+bytes(capacity-len(signature))


class MachODevelopmentSignTests(unittest.TestCase):
    def new_fixture(self):
        data=bytearray(fixture())
        struct.pack_into('<II',data,16,3,160)
        data[120:192]=struct.pack('<II16sQQQQiiII',0x19,72,b'__LINKEDIT',0x100002000,8192,8192,8192,1,1,0,0)
        directory=struct.pack('>II',0xfade0c02,8)
        signature=struct.pack('>5I',0xfade0cc0,28,1,0,20)+directory
        data[8192:]=signature+bytes(8192-len(signature))
        return bytes(data)

    def test_new_tool_signing_xml_der_and_linkedit_coverage(self):
        policy={'org.darwin.bluetooth.bootstrap':True}
        source=self.new_fixture()
        signed,report=sign_new_macho(source,'org.darwin.development.bluetoothbootstrap',policy)
        self.assertEqual(report['entitlements'],policy)
        self.assertEqual(code_directory_entries(signed),[(bytes.fromhex(report['cdhash']),2)])
        self.assertFalse(report['cms_present']);self.assertFalse(report['platform_identity_claimed'])
        signature_offset,capacity=struct.unpack_from('<II',signed,40)
        self.assertEqual(signature_offset+capacity,len(signed))
        self.assertEqual(struct.unpack_from('<Q',signed,168)[0],len(signed)-8192)
        blobs,_=signature_blobs(signed[signature_offset:])
        # Decode DER independently, using the existing strict DER parser.
        root,end=der_item(blobs[7],8,0x70);self.assertEqual(end,len(blobs[7]))
        version,pos=der_item(root,0,2);self.assertEqual(version,b'\1')
        dictionary,end=der_item(root,pos,0xb0);self.assertEqual(end,len(root))
        entry,end=der_item(dictionary,0,0x30);self.assertEqual(end,len(dictionary))
        key,pos=der_item(entry,0,0x0c);boolean,end=der_item(entry,pos,1)
        self.assertEqual((key,boolean,end),(b'org.darwin.bluetooth.bootstrap',b'\xff',len(entry)))
        for offset in (1000,8191,signed.index(b'org.darwin.bluetooth.bootstrap'),len(signed)-1):
            modified=bytearray(signed);modified[offset]^=1
            with self.assertRaises(ValueError):code_directory_entries(modified)
        # Rehashing a conflicting DER dictionary still must not authenticate it.
        modified=bytearray(signed)
        der_offset=signed.index(blobs[7]);modified[der_offset+len(blobs[7])-1]=0
        with self.assertRaises(ValueError):resign_macho(modified,'org.darwin.development.bluetoothbootstrap')

    def test_new_signer_refuses_native_policy_and_foreign_entitlements(self):
        with self.assertRaises(ValueError):sign_new_macho(fixture(),'org.darwin.development.fixture',{'org.darwin.test':True})
        for policy in ({'com.apple.bluetooth.control':True},{'org.darwin.test':1},{'org.darwin.test':'true'},{}):
            with self.assertRaises(ValueError):sign_new_macho(self.new_fixture(),'org.darwin.development.fixture',policy)

    def test_compact_tool_signature_keeps_verified_policy_and_page_coverage(self):
        source=self.new_fixture()
        identifier='org.darwin.development.fixture'
        policy={'org.darwin.test':True}
        ordinary,_=sign_new_macho(source,identifier,policy)
        compact,report=sign_new_macho(source,identifier,policy,compact=True)
        self.assertEqual(len(ordinary)-len(compact),4096)
        self.assertEqual(verify_macho(compact)['entitlements'],policy)
        self.assertEqual(code_directory_entries(compact),[(bytes.fromhex(report['cdhash']),2)])
        for offset in (1000,len(compact)-1):
            changed=bytearray(compact);changed[offset]^=1
            with self.assertRaises(ValueError):verify_macho(changed)

    def test_native_display_rights_are_bound_to_diagnostic_identity(self):
        policy={'com.apple.QuartzCore.displayable-context':True,
                'com.apple.QuartzCore.secure-mode':True,
                'com.apple.QuartzCore.display-state':True,
                'com.apple.QuartzCore.global-capture':True,
                'com.apple.QuartzCore.secure-capture':True}
        identifier='org.darwin.development.displayctl'
        signed,report=sign_new_macho(self.new_fixture(),identifier,policy)
        self.assertEqual(verify_macho(signed)['entitlements'],policy)
        self.assertEqual(code_directory_entries(signed),[(bytes.fromhex(report['cdhash']),2)])
        self.assertFalse(report['platform_identity_claimed'])
        for other in ('org.darwin.development.fixture','org.darwin.development.displayctl.other'):
            with self.assertRaises(ValueError):sign_new_macho(self.new_fixture(),other,policy)
        for extra in ({'platform-application':True},{'com.apple.system-task-ports':True},
                      {'com.apple.QuartzCore.secure-mode':'true'}):
            with self.assertRaises(ValueError):sign_new_macho(self.new_fixture(),identifier,{**policy,**extra})
        with self.assertRaises(ValueError):resign_macho(signed,'org.darwin.development.fixture')

    def test_input_diagnostics_rights_are_bound_to_tool_identity(self):
        identifier='org.darwin.development.inputctl'
        policy={'com.apple.private.hid.client.event-monitor':True,
                'com.apple.private.hid.client.admin':True,
                'com.apple.private.hid.client.debug-tool':True,
                'com.apple.private.hid.client.event-dispatch':True,
                'com.apple.hid.system.user-access-service':True}
        signed,report=sign_new_macho(self.new_fixture(),identifier,policy)
        self.assertEqual(report['entitlements'],policy)
        self.assertEqual(code_directory_entries(signed),[(bytes.fromhex(report['cdhash']),2)])
        for key in ('com.apple.private.hid.client.event-filter','platform-application'):
            with self.assertRaises(ValueError):sign_new_macho(self.new_fixture(),identifier,{**policy,key:True})
        with self.assertRaises(ValueError):resign_macho(signed,'org.darwin.development.fixture')
        with self.assertRaises(ValueError):sign_new_macho(self.new_fixture(),'org.darwin.development.fixture',policy)

    def test_native_service_identity_rejects_unrelated_policy(self):
        with self.assertRaisesRegex(ValueError, 'native service signing policy'):
            resign_macho(fixture(), 'org.darwin.development.datamigrator')

    def test_setup_read_diagnostic_rights_are_bound_to_its_identity(self):
        policy={'task_for_pid-allow':True,'com.apple.system-task-ports':True}
        identifier='org.darwin.development.uipatchctl'
        signed,report=sign_new_macho(self.new_fixture(),identifier,policy)
        self.assertEqual(verify_macho(signed)['entitlements'],policy)
        self.assertEqual(code_directory_entries(signed),[(bytes.fromhex(report['cdhash']),2)])
        for other in ('org.darwin.development.fixture','org.darwin.development.displayctl',
                      'org.darwin.development.uipatchctl.other'):
            with self.assertRaises(ValueError):sign_new_macho(self.new_fixture(),other,policy)
        with self.assertRaises(ValueError):
            sign_new_macho(self.new_fixture(),identifier,{**policy,'platform-application':True})

    def test_size_code_and_both_entitlement_blobs_are_preserved(self):
        source = fixture(); signed, report = resign_macho(source, 'org.mi6.development.fixture')
        self.assertEqual(len(signed), len(source)); self.assertEqual(source[:8192], signed[:8192])
        a, _ = signature_blobs(source[8192:]); b, _ = signature_blobs(signed[8192:])
        for slot in (2, 5, 7): self.assertEqual(a[slot], b[slot])
        self.assertTrue(report['entitlements']['com.apple.private.iowatchdog.user-access'])
        self.assertFalse(report['cms_present']); self.assertFalse(report['platform_identity_claimed'])
        self.assertEqual(code_directory_entries(signed), [(bytes.fromhex(report['cdhash']), 2)])

    def test_pages_entitlements_and_padding_tampering_are_rejected(self):
        signed, _ = resign_macho(fixture(), 'org.mi6.development.fixture')
        ent = signed.index(b'com.apple.private.iowatchdog.user-access')
        for offset in (1000, 4095, 4096, 8191, ent, len(signed)-1):
            changed = bytearray(signed); changed[offset] ^= 1
            with self.assertRaises(ValueError): verify_macho(changed)
            with self.assertRaises(ValueError): code_directory_entries(changed)

    def test_non_engineering_identity_and_invalid_allocations_are_rejected(self):
        source = fixture()
        with self.assertRaises(ValueError): resign_macho(source, 'com.apple.watchdogd')
        with self.assertRaises(ValueError): resign_macho(source[:-1], 'org.mi6.development.fixture')


if __name__ == '__main__': unittest.main()
