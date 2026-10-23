"""Pack pinned modem inputs for EFI-only staging outside the Apple DT."""
import hashlib
import json
from pathlib import Path
import struct
import uuid


def encode_tftp_catalog(files):
    """Canonical immutable catalog, shared with BasebandTFTPFiles.h."""
    if len(files) > 256:
        raise ValueError('TFTP catalog exceeds its file count')
    result = bytearray(16 + 144 * len(files))
    struct.pack_into('<IHHI', result, 0, 0x46544251, 1, 144, len(files))
    for i, (name, raw) in enumerate(sorted(files.items())):
        encoded = name.encode('ascii')
        if (len(encoded) >= 128 or not name.startswith('/readonly/firmware/image/') or
                any(not segment or segment in ('.', '..') or
                    any(not (c.isascii() and (c.isalnum() or c in '_-.')) for c in segment)
                    for segment in name[25:].split('/'))):
            raise ValueError('TFTP catalog requires canonical readonly firmware paths')
        struct.pack_into('<128sIIQ', result, 16 + 144 * i, encoded, len(result), len(raw), 0)
        result.extend(raw)
        result.extend(bytes(-len(result) % 4))
    if len(result) > 8 * 1024 * 1024:
        raise ValueError('TFTP catalog exceeds its byte limit')
    struct.pack_into('<I', result, 12, len(result))
    return bytes(result)


def stock_tftp_catalog(root):
    from baseband_stock import prepare
    root = Path(root)
    stock = prepare(root)
    if not all(stock['matches_existing_boot_inputs'].values()):
        raise ValueError('stock TFTP files must match the existing boot firmware variant')
    sources = {('/readonly/firmware/image/' + name): row for name, row in stock['files'].items()
               if name.startswith('modem_pr/') or name.endswith('.jsn')}
    images = {path: (root / 'build/baseband/stock-files' / path[25:]).read_bytes() for path in sources}
    return encode_tftp_catalog(images), sources, stock


def prepare_tftp_catalog(directory, staged, pin, root=None):
    images, sources = {}, {}
    for name in ('modemr.jsn', 'modemuw.jsn'):
        raw = (directory / name).read_bytes()
        measured = {'bytes': len(raw), 'sha256': hashlib.sha256(raw).hexdigest()}
        if measured != staged['files'].get(name) or any(measured[k] != pin['files'][name][k] for k in measured):
            raise ValueError('TFTP file differs from its staged/pinned source: ' + name)
        json.loads(raw)
        path = '/readonly/firmware/image/' + name
        images[path] = raw; sources[path] = measured
    catalog = encode_tftp_catalog(images)
    if staged.get('tftp_catalog'):
        if root is None: root = Path(__file__).resolve().parents[1]
        expected, sources, stock = stock_tftp_catalog(root)
        actual = (directory / 'tftp-files.bin').read_bytes()
        measured = {'bytes': len(actual), 'sha256': hashlib.sha256(actual).hexdigest()}
        if (actual != expected or any(staged['tftp_catalog'].get(k) != v for k, v in measured.items()) or
                staged['tftp_catalog'].get('source_archive_sha256') != stock['archive_sha256']):
            raise ValueError('TFTP catalog differs from pinned stock files')
        return actual, {**measured, 'files': sources, 'source_archive_sha256': stock['archive_sha256'],
                        'immutable_copy_required': True, 'missing_files_return_error': True,
                        'mcfg_files_provided': True, 'dynamic_module_directory_provided': False,
                        'temporary_files_persist_across_boot': False, 'hardware_tested': False}
    return catalog, {'bytes': len(catalog), 'sha256': hashlib.sha256(catalog).hexdigest(), 'files': sources,
                     'immutable_copy_required': True, 'missing_files_return_error': True,
                     'dynamic_module_directory_provided': False, 'temporary_files_persist_across_boot': False,
                     'hardware_tested': False}


def configure_baseband_handoff(tree, directory, output, root):
    directory, output, root = Path(directory), Path(output), Path(root)
    pin = json.loads((root / 'docs/baseband/firmware-sources.json').read_text())
    staged = json.loads((directory / 'manifest.json').read_text())
    if staged.get('schema') != 1 or staged.get('status') != 'pinned-inputs-staged':
        raise ValueError('baseband requires prepare-baseband output')
    images = {}
    for name in ('mba.mbn', 'modem.mbn'):
        raw = (directory / name).read_bytes()
        measured = {'bytes': len(raw), 'sha256': hashlib.sha256(raw).hexdigest()}
        if measured != staged['files'][name] or any(measured[k] != pin['files'][name][k] for k in measured):
            raise ValueError('baseband firmware differs from its staged/pinned source: ' + name)
        images[name] = raw
    mba, modem = images['mba.mbn'], images['modem.mbn']
    catalog, tftp_report = prepare_tftp_catalog(directory, staged, pin, root)
    extended = bool(staged.get('tftp_catalog'))
    if not 0 < len(mba) <= 0x200000 or not 0 < len(modem) <= 0x4000000:
        raise ValueError('baseband firmware exceeds its bounded image layout')
    modem_offset = (len(mba) + 15) & ~15
    image_bytes = (modem_offset + len(modem) + 0x3fff) & ~0x3fff
    payload = mba + bytes(modem_offset - len(mba)) + modem
    payload += bytes(image_bytes - len(payload))
    files_offset = image_bytes if extended else 0
    if extended:
        payload += catalog
        image_bytes = (len(payload) + 0x3fff) & ~0x3fff
        payload += bytes(image_bytes - len(payload))
    matches = []
    def walk(node):
        if b'qcom,msm8998-mss-pil' in node['props'].get('compatible', b'').split(b'\0'):
            matches.append(node)
        for child in node['children']: walk(child)
    walk(tree)
    expected_reg = struct.pack('>16I', 0, 0x4080000, 0, 0x1000, 0, 0x4180000, 0, 0x1000,
                               0, 0x17911000, 0, 0x1000, 0, 0x1f60000, 0, 0x20000)
    if len(matches) != 1 or matches[0]['props'].get('reg') != expected_reg or matches[0]['props'].get('status') != b'disabled\0':
        raise ValueError('baseband handoff requires the exact disabled sagit modem provider')
    chosen = next(n for n in tree['children'] if n['name'] == 'chosen')
    memory = next(n for n in chosen['children'] if n['name'] == 'memory-map')
    record = bytearray(272 if extended else 224)
    struct.pack_into('<IHHIIQ', record, 0, 0x4f484242, 2 if extended else 1, len(record), 0, 0, 1)
    struct.pack_into('<4Q', record, 24, 0x8d000000, 0x7000000, 0x94500000, 0x200000)
    struct.pack_into('<4Q', record, 104, 0, len(mba), modem_offset, len(modem))
    struct.pack_into('<3I', record, 136, 512, 512, 4)
    record[152:184] = hashlib.sha256(mba).digest()
    record[184:216] = hashlib.sha256(modem).digest()
    if extended:
        struct.pack_into('<QQ32s', record, 224, files_offset, len(catalog), hashlib.sha256(catalog).digest())
    output.mkdir(parents=True, exist_ok=True)
    pending = output / 'baseband-firmware.bin.pending'
    pending.write_bytes(payload)
    pending.replace(output / 'baseband-firmware.bin')
    chosen['props']['qcom,baseband-handoff'] = bytes(record)
    if extended: chosen['props'].pop('qcom,tftp-files', None)
    else: chosen['props']['qcom,tftp-files'] = catalog
    for name in ('QcomMPSS', 'QcomMBA', 'QcomBBMetadata', 'QcomBBFirmware', 'QcomRMTFS'):
        memory['props'][name] = bytes(16)
    return {'schema': 1, 'source': str(directory.resolve()), 'source_kind': pin['source_kind'],
            'images': {name: staged['files'][name] for name in ('mba.mbn', 'modem.mbn')}, 'archive_bytes': image_bytes,
            'tftp_catalog': tftp_report,
            'handoff_version': 2 if extended else 1, 'handoff_bytes': len(record),
            'tftp_catalog_in_independent_archive': extended, 'tftp_catalog_offset': files_offset,
            'archive_sha256': hashlib.sha256(payload).hexdigest(), 'modem_offset': modem_offset,
            'efi_only': True, 'raw_entry_contains_boot_firmware': False,
            'raw_entry_contains_readonly_tftp_catalog': not extended, 'modem_enabled': False,
            'static_regions_require_reserved_memory_type': True,
            'metadata_requires_real_efi_allocation': True, 'initial_secure_ownership_proven': False,
            'rmtfs_requires_reserved_memory_type': True, 'rmtfs_storage_policy_provided': False,
            'device_authentication_verified': False, 'hardware_tested': False}


def configure_rmtfs_storage_policy(tree, path):
    """Encode explicit partition identities; never read or invent NV contents."""
    raw = Path(path).read_bytes()
    document = json.loads(raw)
    labels = ('modemst1', 'modemst2', 'fsc', 'fsg')
    if (not isinstance(document, dict) or set(document) != {'schema', 'partitions'} or
            type(document['schema']) is not int or document['schema'] != 1):
        raise ValueError('RMTFS storage policy requires schema 1 and explicit partitions')
    partitions = document['partitions']
    if not isinstance(partitions, dict) or set(partitions) != set(labels):
        raise ValueError('RMTFS policy must identify modemst1, modemst2, fsc and fsg')
    chosen = next(n for n in tree['children'] if n['name'] == 'chosen')
    handoff = chosen['props'].get('qcom,baseband-handoff', b'')
    if (len(handoff) not in (224, 272) or struct.unpack_from('<IHHI', handoff) !=
            (0x4f484242, 2 if len(handoff) == 272 else 1, len(handoff), 0)):
        raise ValueError('RMTFS policy requires a prepared EFI baseband handoff')
    epoch, = struct.unpack_from('<Q', handoff, 16)
    if not epoch: raise ValueError('RMTFS policy has no boot generation')
    record = bytearray(224)
    struct.pack_into('<IHHQIIQ', record, 0, 0x50534652, 1, 224, epoch, 4, 0, 0)
    previous = []
    for i, label in enumerate(labels):
        entry = partitions[label]
        if not isinstance(entry, dict) or set(entry) != {'uuid', 'lun', 'start', 'bytes', 'access'}:
            raise ValueError('RMTFS partition needs uuid/lun/start/bytes/access: ' + label)
        try:
            identity = uuid.UUID(entry['uuid'])
            if str(identity).lower() != entry['uuid'].lower() or not identity.int: raise ValueError()
        except (ValueError, TypeError, AttributeError) as error:
            raise ValueError('RMTFS requires a canonical nonzero UUID: ' + label) from error
        lun, start, size = entry['lun'], entry['start'], entry['bytes']
        if (any(type(v) is not int for v in (lun, start, size)) or not 0 <= lun < 8 or
                not 0 < start < 2**64 or not 0 < size < 2**64 or start + size > 2**64 - 1 or
                start % 512 or size % 512 or entry['access'] not in ('read-only', 'read-write')):
            raise ValueError('RMTFS partition range/access is invalid: ' + label)
        for old in previous:
            if identity == old[0] or (lun == old[1] and start < old[2] + old[3] and old[2] < start + size):
                raise ValueError('RMTFS partition identities/ranges overlap: ' + label)
        previous.append((identity, lun, start, size))
        struct.pack_into('<16sQQIIQ', record, 32 + 48 * i, identity.bytes, start, size, lun,
                         3 if entry['access'] == 'read-write' else 1, 0)
    chosen['props']['qcom,rmtfs-storage-policy'] = bytes(record)
    return {'schema': 1, 'source': str(Path(path).resolve()), 'source_sha256': hashlib.sha256(raw).hexdigest(),
            'partitions': partitions, 'native_media_match_required': True, 'nv_contents_embedded': False,
            'hardware_tested': False}
