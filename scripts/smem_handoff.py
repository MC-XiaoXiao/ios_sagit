"""Reserve a final-map-verified EFI SMEM handoff; never assume modem ownership."""
import struct


def configure_smem_handoff(tree):
    def walk(node):
        yield node
        for child in node['children']:
            yield from walk(child)

    if b'qcom,msm8998' not in tree['props'].get('compatible', b'').split(b'\0'):
        raise ValueError('SMEM handoff requires an MSM8998 tree')
    nodes = [n for n in walk(tree) if b'qcom,msm8998-smem' in
             n['props'].get('compatible', b'').split(b'\0')]
    expected = struct.pack('>8I', 0, 0x86000000, 0, 0x200000, 0, 0x1f40000, 0, 0x20000)
    if len(nodes) != 1 or nodes[0]['name'] != 'shared-memory@86000000' or nodes[0]['props'].get('reg') != expected:
        raise ValueError('SMEM requires the exact sagit RAM and TCSR mutex windows')
    chosen = [n for n in tree['children'] if n['name'] == 'chosen']
    maps = [n for n in chosen[0]['children'] if n['name'] == 'memory-map'] if len(chosen) == 1 else []
    if len(maps) != 1 or sum(n['name'] == 'memory-map' for n in walk(tree)) != 1:
        raise ValueError('SMEM requires one chosen/memory-map handoff node')
    # A zero placeholder grants nothing. The raw entry never fills it. The EFI
    # entry aborts unless its final memory map proves the complete reservation.
    maps[0]['props']['QcomSMEM'] = bytes(16)
    nodes[0]['props']['status'] = b'okay\0'
    return {'requested': True, 'efi_only': True, 'base': 0x86000000, 'bytes': 0x200000,
            'final_memory_map_required_type': 'EfiReservedMemoryType',
            'bootstrap_arena_overlap_permitted': False, 'raw_entry_publishes_reservation': False,
            'modem_enabled': False, 'remote_ownership_granted': False, 'hardware_tested': False}
