"""Exercise the real EFI reservation check and DT conversion, entirely offline."""
import copy
import importlib.util
from pathlib import Path
import struct
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]


def bridge_module():
    spec = importlib.util.spec_from_file_location('smem_bridge', ROOT / 'scripts/build-bridge.py')
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


def unpack(data, offset=0):
    properties, children = struct.unpack_from('<II', data, offset)
    offset += 8
    node = {'props': {}, 'children': []}
    for _ in range(properties):
        name = data[offset:offset+32].split(b'\0')[0].decode()
        size, = struct.unpack_from('<I', data, offset+32)
        node['props'][name] = data[offset+36:offset+36+size]
        offset += 36 + ((size+3) & ~3)
    for _ in range(children):
        child, offset = unpack(data, offset)
        node['children'].append(child)
    return node, offset


def walk(node):
    yield node
    for child in node['children']:
        yield from walk(child)


class SMEMHandoffTests(unittest.TestCase):
    def test_final_map_and_retry_clear_stale_reservation(self):
        source = r'''
#include <assert.h>
#include <string.h>
#include "smem_reservation.h"
int bridge_smem_finalize(void *, const void *, size_t, size_t, uint64_t, uint64_t);
struct Tree { unsigned char before[4], record[16], after[4]; unsigned length; };
void *dt_property(void *object, const char *node, const char *key, uint32_t *length) {
    struct Tree *t = object;
    assert(!strcmp(node, "memory-map") && !strcmp(key, "QcomSMEM"));
    *length = t->length; return t->record;
}
static void put64(unsigned char *p, uint64_t v) {
    for (unsigned n=0; n<8; ++n) p[n] = (unsigned char)(v >> (n*8));
}
static void desc(unsigned char *p, unsigned type, uint64_t start, uint64_t pages) {
    memset(p, 0, 48); p[0] = type; put64(p+8, start); put64(p+24, pages);
}
int main(void) {
    unsigned char map[4*48]; struct Tree t;
    memset(&t, 0xa5, sizeof(t)); t.length = 16;
    /* Unsorted adjacent reserved fragments, and unrelated conventional RAM. */
    desc(map, 0, 0x86100000, 256); desc(map+48, 7, 0xa0000000, 65536);
    desc(map+96, 0, 0x86000000, 256);
    assert(bridge_smem_reserved(map, 144, 48, 0xa0000000, 0x10000000));
    assert(!bridge_smem_finalize(&t, map, 144, 48, 0xa0000000, 0x10000000));
    assert(smem_map_u64(t.record)==BRIDGE_SMEM_BASE && smem_map_u64(t.record+8)==BRIDGE_SMEM_BYTES);
    /* A changed final map on the ExitBootServices retry cannot reuse proof. */
    desc(map+96, 7, 0x86000000, 256);
    assert(bridge_smem_finalize(&t, map, 144, 48, 0xa0000000, 0x10000000));
    assert(!smem_map_u64(t.record) && !smem_map_u64(t.record+8));
    for (unsigned n=0; n<4; ++n) assert(t.before[n]==0xa5 && t.after[n]==0xa5);
    desc(map, 0, 0x86000000, 512);
    /* Contradictory overlapping descriptors must not be ignored. */
    for (unsigned type=1; type<16; ++type) {
        desc(map+48, type, 0x861ff000, 1);
        assert(!bridge_smem_reserved(map, 96, 48, 0xa0000000, 0x10000000));
    }
    desc(map+48, 0, 0x861ff000, 1);
    assert(bridge_smem_reserved(map, 96, 48, 0xa0000000, 0x10000000));
    desc(map, 0, 0x86000000, 255); desc(map+48, 0, 0x86100000, 256);
    assert(!bridge_smem_reserved(map, 96, 48, 0xa0000000, 0x10000000)); // 4K hole.
    desc(map, 0, 0x85f00000, 1024);
    assert(bridge_smem_reserved(map, 48, 48, 0xa0000000, 0x10000000));
    assert(!bridge_smem_reserved(map, 48, 48, 0x85ffffff, 2));
    assert(!bridge_smem_reserved(map, 48, 48, 0x861ff000, 0x1000));
    assert(bridge_smem_reserved(map, 48, 48, 0x86200000, 0x1000));
    assert(!bridge_smem_reserved(map, 48, 48, UINT64_MAX-4095, 4096));
    assert(!bridge_smem_reserved(map, 48, 48, 0xa0000000, 0));
    assert(!bridge_smem_reserved(0, 48, 48, 0xa0000000, 0x10000000));
    for (unsigned stride=0; stride<48; ++stride)
        assert(!bridge_smem_reserved(map, 48, stride, 0xa0000000, 0x10000000));
    assert(!bridge_smem_reserved(map, 47, 48, 0xa0000000, 0x10000000));
    assert(!bridge_smem_reserved(map, 65537, 48, 0xa0000000, 0x10000000));
    desc(map, 0, 0x86000001, 512);
    assert(!bridge_smem_reserved(map, 48, 48, 0xa0000000, 0x10000000));
    desc(map, 0, 0xfffffffffffff000ULL, 1);
    assert(!bridge_smem_reserved(map, 48, 48, 0xa0000000, 0x10000000));
    desc(map, 0, 0x86000000, UINT64_MAX);
    assert(!bridge_smem_reserved(map, 48, 48, 0xa0000000, 0x10000000));
    desc(map, 0, 0x86000000, 512); t.length = 15;
    assert(bridge_smem_finalize(&t, map, 48, 48, 0xa0000000, 0x10000000));
    return 0;
}
'''
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory)
            (path / 'test.c').write_text(source)
            subprocess.run(['cc', '-std=c11', '-Wall', '-Wextra', '-Werror',
                            '-I' + str(ROOT / 'boot/bridge'), str(path / 'test.c'),
                            str(ROOT / 'boot/bridge/smem_handoff.c'), '-o', str(path / 'test')], check=True)
            subprocess.run([str(path / 'test')], check=True)

    def test_native_tree_keeps_proof_zero_and_remote_nodes_disabled(self):
        bridge = bridge_module()
        with tempfile.TemporaryDirectory() as directory:
            dtb = Path(directory) / 'sagit.dtb'
            subprocess.run(['dtc', '-q', '-I', 'dts', '-O', 'dtb', '-i', str(ROOT / 'device_trees'),
                            '-o', str(dtb), str(ROOT / 'device_trees/xiaomi-sagit.dts')], check=True)
            tree = bridge.parse_fdt(dtb.read_bytes())
        smem = next(n for n in walk(tree) if n['name'] == 'shared-memory@86000000')
        self.assertEqual(smem['props']['status'], b'disabled\0')
        original = copy.deepcopy(tree)
        report = bridge.configure_smem_handoff(tree)
        self.assertTrue(report['efi_only'])
        self.assertFalse(report['modem_enabled'])
        native, end = unpack(bridge.apple_dt(tree))
        nodes = {n['props']['name'].rstrip(b'\0').decode(): n['props'] for n in walk(native)}
        self.assertGreater(end, 0)
        self.assertEqual(nodes['memory-map']['QcomSMEM'], bytes(16))
        self.assertEqual(nodes['shared-memory@86000000']['status'], b'okay\0')
        self.assertEqual(struct.unpack('<4Q', nodes['shared-memory@86000000']['reg']),
                         (0x86000000, 0x200000, 0x1f40000, 0x20000))
        for name in ('modem@4080000', 'audio-transport@17911000'):
            self.assertEqual(nodes[name]['status'], b'disabled\0')
        # Reject alternate maps and ambiguous nodes before mutating the tree.
        for variant in ('wrong-reg', 'duplicate-smem', 'duplicate-map', 'wrong-soc'):
            changed = copy.deepcopy(original)
            target = next(n for n in walk(changed) if n['name'] == 'shared-memory@86000000')
            if variant == 'wrong-reg': target['props']['reg'] = bytes(32)
            elif variant == 'duplicate-smem': changed['children'].append(copy.deepcopy(target))
            elif variant == 'duplicate-map': changed['children'].append({'name': 'memory-map', 'props': {}, 'children': []})
            else: changed['props']['compatible'] = b'qcom,other\0'
            with self.subTest(variant=variant), self.assertRaises(ValueError):
                bridge.configure_smem_handoff(changed)


if __name__ == '__main__':
    unittest.main()
