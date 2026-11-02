"""Strict, offline reader and same-size derivative builder for SHA-256 Cryptexes.

The source is never written. Changes are kept as block overlays, including new
file hashes, all ancestor hashes, and the integrity root. A derivative is not an
Apple-authenticated image. This module does not install images or relax checks.

Layout reference: Apple's Apple File System Reference (2020), Sealed Volumes.
Hashed child OIDs are relative to the root OID (also used by apfs-fuse).
"""
import hashlib
import mmap
import struct

BLOCK = 4096
MASK = (1 << 60) - 1


def require(condition, message):
    if not condition:
        raise ValueError(message)


def u64(data, offset=0):
    return struct.unpack_from('<Q', data, offset)[0]


def digest(data):
    return hashlib.sha256(data).digest()


def checksum(data):
    a = b = 0
    for word in struct.unpack('<1022I', data[8:]):
        a += word
        b += a
    a %= 0xffffffff
    b %= 0xffffffff
    c = 0xffffffff - (a + b) % 0xffffffff
    return c | ((0xffffffff - (a + c) % 0xffffffff) << 32)


def entries(data):
    require(len(data) == BLOCK, 'wrong B-tree block size')
    flags, level, count, to, tl = struct.unpack_from('<HHIHH', data, 32)
    width = 4 if flags & 4 else 8
    end = BLOCK - (40 if flags & 1 else 0)
    base = 56 + to + tl
    require(level < 16 and count * width <= tl and base <= end, 'invalid B-tree table')
    result = []
    for i in range(count):
        if flags & 4:
            ko, vo = struct.unpack_from('<HH', data, 56 + to + width * i)
            kl, vl = 16, 8 if level else 16
        else:
            ko, kl, vo, vl = struct.unpack_from('<HHHH', data, 56 + to + width * i)
        kp, vp = base + ko, end - vo
        require(base <= kp < kp + kl <= end and base <= vp <= vp + vl <= end,
                'invalid B-tree record bounds')
        result.append((data[kp:kp + kl], data[vp:vp + vl], vp))
    return flags, level, result


class SealedImage:
    def __init__(self, path):
        self.file = open(path, 'rb')
        self.raw = mmap.mmap(self.file.fileno(), 0, access=mmap.ACCESS_READ)
        self.changes = {}
        self.nodes = {}
        nx = self.block(0, checked=True)
        require(nx[32:36] == b'NXSB' and struct.unpack_from('<I', nx, 36)[0] == BLOCK,
                'expected raw APFS with 4096-byte blocks')
        require(u64(nx, 40) * BLOCK == len(self.raw), 'container size mismatch')
        self.xid = u64(nx, 16)
        cmap = self.omap(u64(nx, 160), self.xid)
        volumes = [u64(nx, p) for p in range(184, 184 + 100 * 8, 8) if u64(nx, p)]
        require(len(volumes) == 1, 'expected single-volume Cryptex')
        self.volume_block = cmap[volumes[0]]
        vs = self.block(self.volume_block, checked=True)
        require(vs[32:36] == b'APSB' and u64(vs, 56) == 0x28 and u64(vs, 216) == 0,
                'expected case-sensitive sealed volume without snapshots')
        self.volume_uuid = vs[240:256].hex()
        self.vmap = self.omap(u64(vs, 128), u64(vs, 16))
        self.root_oid = u64(vs, 136)
        self.integrity_block = self.vmap[u64(vs, 1024)]
        im = self.block(self.integrity_block, checked=True)
        version, flags, algorithm, self.root_hash_offset = struct.unpack_from('<4I', im, 32)
        require(version == 2 and flags == 0 and algorithm == 1 and u64(im, 48) == 0,
                'expected intact SHA-256 integrity metadata v2')
        require(128 <= self.root_hash_offset <= BLOCK - 32, 'invalid root hash offset')
        self.original_root_hash = im[self.root_hash_offset:self.root_hash_offset + 32]
        self.records = {}
        self.walk_files(self.root_oid, self.original_root_hash)
        self.extents = {}
        self.walk_extents(u64(vs, 1032), set())
        self.paths = {}
        self.walk_paths(2, '')

    def close(self):
        self.raw.close()
        self.file.close()

    def block(self, number, checked=False):
        require(0 <= number < len(self.raw) // BLOCK, 'block outside image')
        data = self.changes.get(number, self.raw[number * BLOCK:(number + 1) * BLOCK])
        if checked:
            require(u64(data) == checksum(data), 'object checksum mismatch')
        return data

    def omap(self, number, xid):
        obj = self.block(number, checked=True)
        require(struct.unpack_from('<I', obj, 24)[0] & 0xffff == 11, 'expected object map')
        found, seen = {}, set()
        def walk(p):
            require(p not in seen, 'object-map cycle')
            seen.add(p)
            b = self.block(p, checked=True)
            flags, level, records = entries(b)
            require(flags & 4, 'expected fixed object-map records')
            for k, v, _ in records:
                if level:
                    walk(u64(v))
                else:
                    oid, tx = struct.unpack('<QQ', k)
                    f, size, physical = struct.unpack('<IIQ', v)
                    if tx <= xid and (oid not in found or tx > found[oid][0]):
                        require(not f & 1 and size == BLOCK, 'unsupported object-map record')
                        found[oid] = (tx, physical)
        walk(u64(obj, 48))
        return {k: v[1] for k, v in found.items()}

    def walk_files(self, oid, expected, expected_level=None):
        p = self.vmap[oid]
        require(p not in self.nodes, 'filesystem B-tree cycle or shared node')
        b = self.block(p)
        require(digest(b) == expected and b[:32] == bytes(32), 'filesystem node hash mismatch')
        flags, level, records = entries(b)
        require(flags & 0x18 == 0x18 and not flags & 4, 'unsupported sealed node')
        require(expected_level is None or level == expected_level, 'filesystem level mismatch')
        self.nodes[p] = (level, records)
        for k, v, vp in records:
            if level:
                require(len(v) == 40, 'expected SHA-256 child record')
                self.walk_files(self.root_oid + u64(v), v[8:], level - 1)
            else:
                self.records.setdefault(u64(k) & MASK, []).append((p, k, v, vp))

    def walk_extents(self, p, seen):
        require(p not in seen, 'extent B-tree cycle')
        seen.add(p)
        b = self.block(p, checked=True)
        require(struct.unpack_from('<I', b, 28)[0] == 31, 'expected extent tree')
        _, level, records = entries(b)
        for k, v, _ in records:
            if level:
                require(len(v) == 8, 'invalid extent child')
                self.walk_extents(u64(v), seen)
            else:
                require(len(k) == len(v) == 16, 'invalid sealed extent record')
                owner, logical = struct.unpack('<QQ', k)
                length, physical = struct.unpack('<QQ', v)
                require(length >> 56 == 0 and length % BLOCK == 0 and logical % BLOCK == 0,
                        'unsupported sealed extent')
                self.extents.setdefault(owner, []).append((logical, length, physical))

    def walk_paths(self, oid, path, ancestors=()):
        require(oid not in ancestors, 'directory cycle')
        for _, k, v, _ in self.records.get(oid, []):
            if u64(k) >> 60 == 9:
                n = struct.unpack_from('<I', k, 8)[0] & 0x3ff
                require(n > 0 and 12 + n == len(k) and k[-1] == 0, 'invalid directory name')
                name = k[12:-1].decode('utf8')
                child, kind = u64(v), struct.unpack_from('<H', v, 16)[0] & 15
                full = path + '/' + name
                require(full not in self.paths, 'duplicate pathname')
                self.paths[full] = child
                if kind == 4:
                    self.walk_paths(child, full, ancestors + (oid,))

    def inode(self, oid):
        rows = [v for _, k, v, _ in self.records[oid] if u64(k) >> 60 == 3]
        require(len(rows) == 1, 'inode not unique')
        return rows[0]

    def file_info(self, path):
        oid = self.paths[path]
        inode = self.inode(oid)
        owner = u64(inode, 8)
        # DSTREAM xfield follows the 92-byte packed j_inode_val_t.
        count, used = struct.unpack_from('<HH', inode, 92)
        cursor = 96 + 4 * count
        size = None
        for i in range(count):
            kind, flags, length = struct.unpack_from('<BBH', inode, 96 + 4 * i)
            require(cursor + length <= len(inode), 'invalid inode xfield')
            if kind == 8:
                require(length == 40, 'invalid data stream xfield')
                size = u64(inode, cursor)
            cursor += (length + 7) & ~7
        require(size is not None, 'file has no data stream')
        extents = sorted(self.extents[owner])
        covered = 0
        for logical, length, physical in extents:
            require(logical == covered and physical * BLOCK + length <= len(self.raw),
                    'sparse or out-of-bounds file')
            covered += length
        require(size <= covered < size + BLOCK, 'file extent coverage mismatch')
        return {'oid': oid, 'owner': owner, 'size': size, 'extents': extents}

    def read_file(self, path, offset=0, size=None):
        info = self.file_info(path)
        size = info['size'] - offset if size is None else size
        require(0 <= offset <= offset + size <= info['size'], 'file read bounds')
        out = bytearray()
        for logical, length, physical in info['extents']:
            start, end = max(offset, logical), min(offset + size, logical + length)
            while start < end:
                within = start % BLOCK
                take = min(end - start, BLOCK - within)
                out += self.block(physical + (start - logical) // BLOCK)[within:within + take]
                start += take
        require(len(out) == size, 'short file read')
        return bytes(out)

    def patch_file(self, path, offset, before, after):
        info = self.file_info(path)
        require(len(before) == len(after) > 0 and self.read_file(path, offset, len(before)) == before,
                'patch size or original bytes mismatch')
        cursor = 0
        for logical, length, physical in info['extents']:
            start, end = max(offset, logical), min(offset + len(after), logical + length)
            while start < end:
                within = start % BLOCK
                take = min(end - start, BLOCK - within)
                p = physical + (start - logical) // BLOCK
                b = bytearray(self.block(p))
                b[within:within + take] = after[cursor:cursor + take]
                self.changes[p] = bytes(b)
                cursor += take
                start += take
        require(cursor == len(after), 'short patch')

    def rehash_file(self, path):
        info = self.file_info(path)
        hashes = sorted((u64(k, 8) & 0x00ffffffffffffff, p, k, v, vp)
                        for p, k, v, vp in self.records[info['oid']] if u64(k) >> 60 == 13)
        require(hashes, 'file has no integrity records')
        covered = changed = 0
        for lba, p, k, v, vp in hashes:
            blocks, size = struct.unpack_from('<HB', v)
            require(u64(k, 8) >> 56 == 1 and size == 32 and len(v) == 35 and blocks > 0,
                    'invalid file hash record')
            # info_and_lba stores the logical byte offset; hashed_len is in blocks.
            start, length = lba, blocks * BLOCK
            require(start == covered and start + length <= info['size'], 'file hash coverage mismatch')
            # Verify the source hash even for ranges which were not changed.
            old = bytearray()
            for logical, extent_length, physical in info['extents']:
                a, b = max(start, logical), min(start + length, logical + extent_length)
                if a < b:
                    at = physical * BLOCK + a - logical
                    old += self.raw[at:at + b - a]
            require(len(old) == length and digest(old) == v[3:], 'source file hash mismatch')
            new_hash = digest(self.read_file(path, start, length))
            if new_hash != v[3:]:
                block = bytearray(self.block(p))
                block[vp + 3:vp + 35] = new_hash
                self.changes[p] = bytes(block)
                changed += 1
            covered += length
        require(covered == info['size'], 'incomplete file hash coverage')
        return changed

    def reseal(self):
        # Leaf changes are already hashed in their parent; process children first.
        for p, (level, records) in sorted(self.nodes.items(), key=lambda row: row[1][0]):
            if not level:
                continue
            b = bytearray(self.block(p))
            for _, v, vp in records:
                child = self.vmap[self.root_oid + u64(v)]
                b[vp + 8:vp + 40] = digest(self.block(child))
            if bytes(b) != self.block(p):
                self.changes[p] = bytes(b)
        new_hash = digest(self.block(self.vmap[self.root_oid]))
        im = bytearray(self.block(self.integrity_block, checked=True))
        im[self.root_hash_offset:self.root_hash_offset + 32] = new_hash
        struct.pack_into('<Q', im, 0, checksum(im))
        self.changes[self.integrity_block] = bytes(im)
        return new_hash

    def verify_seal(self):
        im = self.block(self.integrity_block, checked=True)
        def walk(oid, expected):
            p = self.vmap[oid]
            b = self.block(p)
            require(digest(b) == expected, 'derived filesystem hash mismatch')
            _, level, records = entries(b)
            if level:
                for _, v, _ in records:
                    walk(self.root_oid + u64(v), v[8:])
        walk(self.root_oid, im[self.root_hash_offset:self.root_hash_offset + 32])
