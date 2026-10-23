"""Import the firmware's filesystem policy without its hardware DT nodes."""
import hashlib
import json
from pathlib import Path
import plistlib
import struct
import subprocess
import zipfile


def parse_apple_dt(blob):
    def node(pos, depth=0):
        if depth > 64 or pos + 8 > len(blob):
            raise ValueError('invalid Apple device tree')
        np, nc = struct.unpack_from('<II', blob, pos); pos += 8
        props = {}; children = []
        for _ in range(np):
            if pos + 36 > len(blob):
                raise ValueError('truncated Apple DT property')
            name = blob[pos:pos+32].split(b'\0')[0].decode('ascii')
            size = struct.unpack_from('<I', blob, pos+32)[0] & 0x7fffffff
            if name in props or pos + 36 + size > len(blob):
                raise ValueError('duplicate or truncated Apple DT property')
            props[name] = blob[pos+36:pos+36+size]; pos += 36 + ((size+3) & ~3)
        for _ in range(nc):
            child, pos = node(pos, depth+1); children.append(child)
        name = props.get('name', b'').rstrip(b'\0').decode('ascii')
        return {'name': name, 'props': props, 'children': children, 'apple_native': True}, pos
    root, end = node(0)
    if end != len(blob):
        raise ValueError('trailing Apple device tree bytes')
    return root


def configure_firmware_filesystems(tree, report_path, out, ipsw_tool):
    report = json.loads(report_path.read_text())
    with zipfile.ZipFile(report['ipsw']) as archive:
        manifest = plistlib.loads(archive.read('BuildManifest.plist'))
        identity = next(b for b in manifest['BuildIdentities']
                        if b['Info']['DeviceClass'] == report['board'] and b['Info']['RestoreBehavior'] == 'Erase')
        component = identity['Manifest']['DeviceTree']
        data = archive.read(component['Info']['Path']); digest = component['Digest']
        algorithm = {20:'sha1',32:'sha256',48:'sha384'}.get(len(digest))
        if not algorithm or hashlib.new(algorithm, data).digest() != digest:
            raise ValueError('DeviceTree BuildManifest digest mismatch')
    source = out/'firmware-devicetree.im4p'; raw = out/'firmware-devicetree.apple-dt'
    source.write_bytes(data)
    subprocess.run([str(ipsw_tool), 'img4', 'im4p', 'extract', '--output', str(raw), str(source)], check=True)
    original = parse_apple_dt(raw.read_bytes())
    filesystems = next(n for n in original['children'] if n['name'] == 'filesystems')
    def remove_handles(node):
        for key in ('AAPL,phandle', 'phandle', 'linux,phandle'):
            node['props'].pop(key, None)
        for child in node['children']:
            remove_handles(child)
    remove_handles(filesystems)
    tree['children'] = [n for n in tree['children'] if n['name'] != 'filesystems'] + [filesystems]
    return {'zip_entry': component['Info']['Path'], 'sha256': hashlib.sha256(data).hexdigest(),
            'source': 'firmware filesystems subtree', 'tables': [n['name'] for n in filesystems['children']]}
