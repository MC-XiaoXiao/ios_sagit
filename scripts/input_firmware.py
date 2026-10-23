"""Pinned input firmware preparation and bridge handoff (no device access)."""
import hashlib
import json
from pathlib import Path
import struct
import urllib.request
import zipfile

FIRMWARE_URL = ('https://github.com/XiaomiFirmwareUpdaterReleases/firmware_xiaomi_sagit/releases/download/'
                'stable-23.03.2020/fw_sagit_miui_MI6Global_V11.0.6.0.PCAMIXM_47fd8ee4ef_9.0.zip')
FIRMWARE_SHA256 = '2de2cd41548c91f0190b612597181177a8187d2d22d7005ef08770546aab2eb3'
APPLICATIONS = {'fpcsgt': 'fpc-firmware', 'goodixfp': 'goodix-firmware'}
APPLICATION_SHA256 = {
    'fpcsgt': '8a069a1f53d4e8323da69cd42249276e2b86461c63e16a6f299f1e9eac4db42e',
    'goodixfp': '16a5d18b44551c5c9cd55c10476fcc665bc15b2291416d34d2f6da28d2304749',
}


class FAT16:
    """Read the stock NON-HLOS FAT16 image, checking every chain and extent."""
    def __init__(self, data):
        self.data = data
        if len(data) < 512 or data[510:512] != b'\x55\xaa':
            raise ValueError('invalid FAT boot sector')
        self.sector, self.cluster_sectors, self.reserved, self.fats, self.entries = struct.unpack_from('<HBHBH', data, 11)
        total = struct.unpack_from('<H', data, 19)[0] or struct.unpack_from('<I', data, 32)[0]
        fat_sectors = struct.unpack_from('<H', data, 22)[0]
        if self.sector not in (512, 1024, 2048, 4096) or not self.cluster_sectors or self.cluster_sectors & (self.cluster_sectors-1):
            raise ValueError('invalid FAT geometry')
        self.fat = self.reserved*self.sector
        self.root = (self.reserved+self.fats*fat_sectors)*self.sector
        root_size = (self.entries*32+self.sector-1)//self.sector*self.sector
        self.start = self.root+root_size
        self.cluster_size = self.cluster_sectors*self.sector
        self.clusters = (total*self.sector-self.start)//self.cluster_size
        if (not self.reserved or not self.fats or not self.entries or not fat_sectors or
                self.start > len(data) or len(data) > total*self.sector or not 4085 <= self.clusters < 65525 or
                fat_sectors*self.sector < (self.clusters+2)*2):
            raise ValueError('not a bounded FAT16 volume')

    def chain(self, cluster):
        result = bytearray(); seen = set()
        while cluster < 0xfff8:
            if cluster < 2 or cluster >= self.clusters+2 or cluster in seen:
                raise ValueError('invalid or cyclic FAT chain')
            seen.add(cluster)
            off = self.start+(cluster-2)*self.cluster_size
            # Stock flash images omit the unused tail of the FAT volume.
            # Allocated file clusters must still be fully present.
            if off+self.cluster_size > len(self.data): raise ValueError('truncated allocated FAT cluster')
            result.extend(self.data[off:off+self.cluster_size])
            cluster = struct.unpack_from('<H', self.data, self.fat+cluster*2)[0]
        return bytes(result)

    def read(self, path):
        directory = self.data[self.root:self.root+self.entries*32]
        parts = path.upper().split('/')
        for index, part in enumerate(parts):
            found = None
            for off in range(0, len(directory)-31, 32):
                entry = directory[off:off+32]
                if entry[0] == 0: break
                if entry[0] == 0xe5 or entry[11] & 8: continue
                name = entry[:8].decode('ascii').rstrip()
                ext = entry[8:11].decode('ascii').rstrip()
                if ext: name += '.'+ext
                if name == part:
                    found = entry; break
            if found is None: raise ValueError('missing firmware '+path)
            is_dir = bool(found[11] & 16)
            if is_dir != (index < len(parts)-1): raise ValueError('invalid firmware directory')
            cluster = struct.unpack_from('<H', found, 26)[0]
            directory = self.chain(cluster)
            if not is_dir:
                size = struct.unpack_from('<I', found, 28)[0]
                if size > len(directory): raise ValueError('short firmware file')
                return directory[:size]
        raise ValueError('invalid firmware path')


def application_packet(mdt, segments):
    if len(mdt) < 52 or mdt[:7] != b'\x7fELF\x01\x01\x01' or struct.unpack_from('<H', mdt, 18)[0] != 40:
        raise ValueError('expected ARM32 little-endian signed trusted application')
    phoff = struct.unpack_from('<I', mdt, 28)[0]
    entsize, count = struct.unpack_from('<HH', mdt, 42)
    if entsize != 32 or not count or count > 64 or phoff+count*entsize > len(mdt) or len(segments) != count:
        raise ValueError('invalid application program headers')
    for i, segment in enumerate(segments):
        if len(segment) != struct.unpack_from('<I', mdt, phoff+i*entsize+16)[0]:
            raise ValueError('trusted application segment size differs from MDT')
    data = struct.pack('<I', len(mdt))+mdt+b''.join(segments)
    if len(data) > 0x3c0000: raise ValueError('trusted application exceeds transport buffer')
    return data


def prepare_input_firmware(output, archive=None):
    output = Path(output); output.mkdir(parents=True, exist_ok=True)
    archive = Path(archive) if archive else output/'firmware.zip'
    if not archive.exists():
        temporary = archive.with_suffix('.download')
        try:
            with urllib.request.urlopen(FIRMWARE_URL, timeout=60) as response, temporary.open('wb') as dest:
                while block := response.read(1024*1024): dest.write(block)
            if hashlib.sha256(temporary.read_bytes()).hexdigest() != FIRMWARE_SHA256:
                raise ValueError('input firmware archive hash mismatch')
            temporary.replace(archive)
        finally:
            temporary.unlink(missing_ok=True)
    if hashlib.sha256(archive.read_bytes()).hexdigest() != FIRMWARE_SHA256:
        raise ValueError('input firmware archive hash mismatch')
    with zipfile.ZipFile(archive) as z:
        fat = FAT16(z.read('firmware-update/NON-HLOS.bin'))
    manifest = {'url': FIRMWARE_URL, 'archive_sha256': FIRMWARE_SHA256, 'applications': {}}
    for name, prop in APPLICATIONS.items():
        mdt = fat.read('image/'+name+'.mdt')
        count = struct.unpack_from('<H', mdt, 44)[0]
        packet = application_packet(mdt, [fat.read('image/'+name+'.b%02d'%i) for i in range(count)])
        if hashlib.sha256(packet).hexdigest()!=APPLICATION_SHA256[name]:
            raise ValueError('extracted input application differs from analyzed firmware')
        path = output/(name+'.bin'); path.write_bytes(packet)
        manifest['applications'][prop] = {'file': path.name, 'sha256': hashlib.sha256(packet).hexdigest(), 'bytes': len(packet)}
    (output/'manifest.json').write_text(json.dumps(manifest, indent=2)+'\n')
    return manifest


def configure_input_firmware(tree, directory):
    """Enable one shared physical provider; Apple matching chooses one chip Kext."""
    directory = Path(directory)
    manifest = json.loads((directory/'manifest.json').read_text())
    if manifest.get('archive_sha256') != FIRMWARE_SHA256: raise ValueError('unexpected input firmware origin')
    def nodes(node):
        yield node
        for child in node['children']: yield from nodes(child)
    all_nodes = list(nodes(tree))
    homes = [n for n in all_nodes if b'goodix,fingerprint' in n['props'].get('compatible', b'').split(b'\0')]
    gpios = [n for n in all_nodes if b'qcom,msm8998-pinctrl' in n['props'].get('compatible', b'').split(b'\0')]
    clocks = [n for n in all_nodes if b'qcom,gcc-msm8998' in n['props'].get('compatible', b'').split(b'\0')]
    if len(homes)!=1 or len(gpios)!=1 or len(clocks)!=1: raise ValueError('ambiguous input hardware providers')
    props = homes[0]['props']
    for name, prop in APPLICATIONS.items():
        record = manifest['applications'][prop]
        # Fixed filenames avoid arbitrary paths in a generated manifest.
        if record['file'] != name+'.bin': raise ValueError('invalid input firmware filename')
        if record['sha256']!=APPLICATION_SHA256[name]: raise ValueError('unrecognized input application')
        blob = (directory/record['file']).read_bytes()
        if len(blob)!=record['bytes'] or hashlib.sha256(blob).hexdigest()!=record['sha256']:
            raise ValueError('modified input firmware packet')
        props[prop] = blob
    gpio = gpios[0]['props']
    pins = []
    for key, flags in [('power-gpios',0),('reset-gpios',1),('irq-gpios',0)]:
        handle,pin,polarity=struct.unpack('>III',props[key])
        if struct.pack('>I',handle)!=gpio.get('phandle') or polarity!=flags:
            raise ValueError('fingerprint GPIO controller or polarity mismatch')
        pins.append(pin)
    if len(set(pins))!=3 or any(p>=150 for p in pins): raise ValueError('invalid fingerprint GPIO routing')
    owned = gpio.get('qcom,owned-gpios', b'')
    if len(owned)%4: raise ValueError('invalid GPIO ownership')
    old = set(struct.unpack('>'+str(len(owned)//4)+'I', owned))
    gpio['qcom,owned-gpios'] = b''.join(struct.pack('>I', p) for p in sorted(old|set(pins)))
    gcc = clocks[0]['props']
    version = gcc.get('qcom,gcc-handoff-version', struct.pack('>I',1))
    if version != struct.pack('>I',1): raise ValueError('unsupported GCC ownership version')
    gcc['qcom,gcc-handoff-version'] = version
    for key, additions in [('qcom,owned-clocks',{54:0,66:0}), ('qcom,owned-roots',{26:50000000})]:
        data = gcc.get(key,b'')
        if len(data)%8: raise ValueError('invalid clock ownership')
        entries = dict(struct.iter_unpack('>II',data))
        for resource, limit in additions.items():
            if resource in entries and entries[resource]!=limit: raise ValueError('conflicting fingerprint clock ownership')
            entries[resource] = limit
        gcc[key] = b''.join(struct.pack('>II',*item) for item in sorted(entries.items()))
    props['status'] = b'okay\0'
    return manifest
