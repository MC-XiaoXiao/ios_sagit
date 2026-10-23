package main

// Small path-copy editor for the unencrypted, unsealed trees used by this
// project. Unvisited nodes and every unrelated volume stay byte-for-byte intact.
import (
	"bytes"
	"encoding/binary"
	"fmt"
	"github.com/deploymenttheory/go-apfs-v2/pkg/apfs"
	"sort"
)

var nu16 = binary.LittleEndian.Uint16
var nu32 = binary.LittleEndian.Uint32
var nu64 = binary.LittleEndian.Uint64
var np16 = binary.LittleEndian.PutUint16
var np32 = binary.LittleEndian.PutUint32
var np64 = binary.LittleEndian.PutUint64

const nativeMask = uint64(1<<60) - 1

type nativeRecord struct{ key, value []byte }
type nativeNode struct {
	oid, physical, oldPhysical uint64
	flags, level               uint16
	typ, subtype               uint32
	records                    []nativeRecord
	dirty, dropped             bool
}
type nativeTree struct {
	read                     func(uint64) []byte
	allocate                 func(uint64) uint64
	release                  func(uint64, uint64)
	write                    func(uint64, []byte)
	virtual                  bool
	resolve                  map[uint64]uint64
	nextOID                  uint64
	root                     *nativeNode
	cache                    map[uint64]*nativeNode
	dirty                    map[*nativeNode]bool
	compare                  func([]byte, []byte) int
	footer                   []byte
	keyDelta, nodeDelta      int64
	xid                      uint64
	longestKey, longestValue uint32
}

func nativeFSCompare(a, b []byte) int {
	ah, bh := nu64(a), nu64(b)
	if ah&nativeMask != bh&nativeMask {
		if ah&nativeMask < bh&nativeMask {
			return -1
		}
		return 1
	}
	if ah>>60 != bh>>60 {
		if ah>>60 < bh>>60 {
			return -1
		}
		return 1
	}
	switch ah >> 60 {
	case 9:
		x, y := nu32(a[8:])&^1023, nu32(b[8:])&^1023
		if x != y {
			if x < y {
				return -1
			}
			return 1
		}
		return bytes.Compare(a[12:], b[12:])
	case 4:
		return bytes.Compare(a[10:], b[10:])
	case 5, 8:
		x, y := nu64(a[8:]), nu64(b[8:])
		if x != y {
			if x < y {
				return -1
			}
			return 1
		}
		return 0
	default:
		return bytes.Compare(a[8:], b[8:])
	}
}
func nativeIDCompare(a, b []byte) int {
	x, y := nu64(a)&nativeMask, nu64(b)&nativeMask
	if x < y {
		return -1
	}
	if x > y {
		return 1
	}
	return 0
}
func nativeOmapCompare(a, b []byte) int {
	if nu64(a) < nu64(b) {
		return -1
	}
	if nu64(a) > nu64(b) {
		return 1
	}
	if nu64(a[8:]) < nu64(b[8:]) {
		return -1
	}
	if nu64(a[8:]) > nu64(b[8:]) {
		return 1
	}
	return 0
}
func newNativeTree(read func(uint64) []byte, allocate func(uint64) uint64, release func(uint64, uint64), write func(uint64, []byte), root uint64, virtual bool, resolve map[uint64]uint64, xid uint64, compare func([]byte, []byte) int) *nativeTree {
	t := &nativeTree{read: read, allocate: allocate, release: release, write: write, virtual: virtual, resolve: resolve, cache: map[uint64]*nativeNode{}, dirty: map[*nativeNode]bool{}, compare: compare, xid: xid}
	for oid := range resolve {
		if oid >= t.nextOID {
			t.nextOID = oid + 1
		}
	}
	t.root = t.load(root)
	raw := read(t.root.physical)
	t.footer = bytes.Clone(raw[4056:])
	repairCheck(t.root.flags&1 != 0 && nu32(t.footer[4:]) == 4096, "unsupported B-tree root")
	return t
}
func (t *nativeTree) load(ref uint64) *nativeNode {
	if n := t.cache[ref]; n != nil {
		return n
	}
	p := ref
	if t.virtual {
		var ok bool
		p, ok = t.resolve[ref]
		repairCheck(ok, "unmapped virtual node")
	}
	b := t.read(p)
	repairCheck(nu64(b) == mustChecksum(b), "B-tree checksum")
	n := &nativeNode{oid: nu64(b[8:]), physical: p, oldPhysical: p, flags: nu16(b[32:]), level: nu16(b[34:]), typ: nu32(b[24:]), subtype: nu32(b[28:])}
	repairCheck(n.flags&^uint16(7) == 0 && n.level < 32 && (n.typ&0xffff == 2 || n.typ&0xffff == 3), "unsupported B-tree node")
	repairCheck((n.level == 0) == (n.flags&2 != 0), "B-tree level flags disagree")
	if t.virtual {
		repairCheck(n.oid == ref && n.typ>>30 == 0, "unexpected virtual identity")
	} else {
		repairCheck(n.oid == p && n.typ>>30 == 1, "unexpected physical identity")
	}
	count := int(nu32(b[36:]))
	to, tl := int(nu16(b[40:])), int(nu16(b[42:]))
	width := 8
	if n.flags&4 != 0 {
		width = 4
		repairCheck(n.subtype == 11, "only object maps use fixed records")
	}
	end := 4096
	if n.flags&1 != 0 {
		end -= 40
	}
	base := 56 + to + tl
	repairCheck(count*width <= tl && base <= end, "invalid node table")
	for i := 0; i < count; i++ {
		at := 56 + to + i*width
		ko, kl, vo, vl := int(nu16(b[at:])), 16, int(nu16(b[at+2:])), 16
		if width == 8 {
			kl = int(nu16(b[at+2:]))
			vo = int(nu16(b[at+4:]))
			vl = int(nu16(b[at+6:]))
		} else if n.level > 0 {
			vl = 8
		}
		kp, vp := base+ko, end-vo
		repairCheck(kl > 0 && kp >= base && kp+kl <= end && vp >= base && vp+vl <= end, "invalid node record")
		if n.level > 0 {
			repairCheck(vl == 8, "invalid index pointer")
		}
		n.records = append(n.records, nativeRecord{bytes.Clone(b[kp : kp+kl]), bytes.Clone(b[vp : vp+vl])})
		if i > 0 {
			repairCheck(t.compare(n.records[i-1].key, n.records[i].key) < 0, "nonunique or unsorted tree keys")
		}
	}
	t.cache[ref] = n
	return n
}
func mustChecksum(b []byte) uint64 { return must(apfs.CalculateFletcher64(b[8:], 0)) }
func (t *nativeTree) ref(n *nativeNode) uint64 {
	if t.virtual {
		return n.oid
	}
	return n.physical
}
func (t *nativeTree) touch(n *nativeNode) {
	if n.dirty {
		return
	}
	n.dirty = true
	n.physical = t.allocate(1)
	if !t.virtual {
		n.oid = n.physical
		t.cache[n.physical] = n
	}
	t.release(n.oldPhysical, 1)
	t.dirty[n] = true
}
func (t *nativeTree) makeNode(level uint16, flags uint16, typ, subtype uint32) *nativeNode {
	p := t.allocate(1)
	oid := p
	if t.virtual {
		repairCheck(t.nextOID < nativeMask, "virtual object ID overflow")
		oid = t.nextOID
		t.nextOID++
	}
	n := &nativeNode{oid: oid, physical: p, level: level, flags: flags, typ: typ, subtype: subtype, dirty: true}
	t.cache[t.ref(n)] = n
	t.dirty[n] = true
	t.nodeDelta++
	return n
}
func nativeTOC(n *nativeNode) int {
	if n.flags&4 != 0 {
		value := 16
		if n.level > 0 {
			value = 8
		}
		return (4096 - 56) / (16 + value + 4) * 4
	}
	return max(8, len(n.records)*8)
}
func nativeNodeSize(n *nativeNode) int {
	size := 56 + nativeTOC(n)
	if n.flags&1 != 0 {
		size += 40
	}
	for _, r := range n.records {
		size += len(r.key) + len(r.value)
	}
	return size
}
func (t *nativeTree) find(key []byte) ([]byte, bool) {
	n := t.root
	for n.level > 0 {
		repairCheck(len(n.records) > 0, "empty index")
		i := sort.Search(len(n.records), func(i int) bool { return t.compare(n.records[i].key, key) > 0 }) - 1
		if i < 0 {
			i = 0
		}
		n = t.load(nu64(n.records[i].value))
	}
	i := sort.Search(len(n.records), func(i int) bool { return t.compare(n.records[i].key, key) >= 0 })
	if i < len(n.records) && t.compare(n.records[i].key, key) == 0 {
		return bytes.Clone(n.records[i].value), true
	}
	return nil, false
}
func (t *nativeTree) put(key, value []byte) {
	if value != nil {
		t.longestKey = max(t.longestKey, uint32(len(key)))
		t.longestValue = max(t.longestValue, uint32(len(value)))
	}
	children := t.edit(t.root, key, value)
	if len(children) == 1 {
		t.root = children[0]
	} else {
		left := children[0]
		left.flags &^= 1
		left.typ = (left.typ &^ 0xffff) | 3
		right := children[1]
		root := t.makeNode(left.level+1, (left.flags&4)|1, (left.typ&^0xffff)|2, left.subtype)
		for _, ch := range children {
			v := make([]byte, 8)
			np64(v, t.ref(ch))
			root.records = append(root.records, nativeRecord{bytes.Clone(ch.records[0].key), v})
		}
		t.root = root
		_ = right
	}
	// Collapse a one-child root after deletion. The promoted child is copied
	// before adding its root footer; no old node is modified in place.
	for t.root.level > 0 && len(t.root.records) == 1 {
		old := t.root
		ch := t.load(nu64(old.records[0].value))
		promoted := *ch
		promoted.flags |= 1
		if nativeNodeSize(&promoted) > 4096 {
			break
		}
		t.touch(ch)
		ch.flags |= 1
		ch.typ = (ch.typ &^ 0xffff) | 2
		old.dropped = true
		t.release(old.physical, 1)
		t.nodeDelta--
		t.root = ch
	}
	if t.root.level > 0 && len(t.root.records) == 0 {
		t.root.level = 0
		t.root.flags |= 2
	}
	repairCheck(nativeNodeSize(t.root) <= 4096, "root footer overflow")
}
func (t *nativeTree) edit(n *nativeNode, key, value []byte) []*nativeNode {
	t.touch(n)
	if n.level == 0 {
		i := sort.Search(len(n.records), func(i int) bool { return t.compare(n.records[i].key, key) >= 0 })
		found := i < len(n.records) && t.compare(n.records[i].key, key) == 0
		if value == nil {
			repairCheck(found, "deleting missing tree record")
			n.records = append(n.records[:i], n.records[i+1:]...)
			t.keyDelta--
		} else if found {
			n.records[i] = nativeRecord{bytes.Clone(key), bytes.Clone(value)}
		} else {
			n.records = append(n.records, nativeRecord{})
			copy(n.records[i+1:], n.records[i:])
			n.records[i] = nativeRecord{bytes.Clone(key), bytes.Clone(value)}
			t.keyDelta++
		}
	} else {
		i := sort.Search(len(n.records), func(i int) bool { return t.compare(n.records[i].key, key) > 0 }) - 1
		if i < 0 {
			i = 0
		}
		children := t.edit(t.load(nu64(n.records[i].value)), key, value)
		replacement := []nativeRecord{}
		for _, ch := range children {
			if len(ch.records) == 0 {
				ch.dropped = true
				t.release(ch.physical, 1)
				t.nodeDelta--
				continue
			}
			v := make([]byte, 8)
			np64(v, t.ref(ch))
			replacement = append(replacement, nativeRecord{bytes.Clone(ch.records[0].key), v})
		}
		tail := append([]nativeRecord{}, n.records[i+1:]...)
		n.records = append(n.records[:i], replacement...)
		n.records = append(n.records, tail...)
	}
	if nativeNodeSize(n) <= 4096 {
		return []*nativeNode{n}
	}
	repairCheck(len(n.records) > 1, "oversized B-tree record")
	// Pick a split by bytes, not record count (CAML/plist names vary in length).
	at := len(n.records) / 2
	best := int(^uint(0) >> 1)
	for i := 1; i < len(n.records); i++ {
		a, b := *n, *n
		a.flags &^= 1
		b.flags &^= 1
		a.records = n.records[:i]
		b.records = n.records[i:]
		sa, sb := nativeNodeSize(&a), nativeNodeSize(&b)
		if sa <= 4096 && sb <= 4096 && max(sa, sb) < best {
			best = max(sa, sb)
			at = i
		}
	}
	repairCheck(best < int(^uint(0)>>1), "cannot split B-tree node")
	right := t.makeNode(n.level, n.flags&^1, (n.typ&^0xffff)|3, n.subtype)
	right.records = append([]nativeRecord{}, n.records[at:]...)
	n.records = n.records[:at]
	return []*nativeNode{n, right}
}
func (t *nativeTree) walk(fn func(nativeRecord)) {
	var visit func(*nativeNode)
	visit = func(n *nativeNode) {
		for _, r := range n.records {
			if n.level == 0 {
				fn(r)
			} else {
				visit(t.load(nu64(r.value)))
			}
		}
	}
	visit(t.root)
}
func (t *nativeTree) flush() {
	t.touch(t.root)
	for n := range t.dirty {
		if n.dropped {
			continue
		}
		b := make([]byte, 4096)
		np64(b[8:], n.oid)
		np64(b[16:], t.xid)
		np32(b[24:], n.typ)
		np32(b[28:], n.subtype)
		np16(b[32:], n.flags)
		np16(b[34:], n.level)
		np32(b[36:], uint32(len(n.records)))
		width := 8
		if n.flags&4 != 0 {
			width = 4
		}
		tl := nativeTOC(n)
		np16(b[42:], uint16(tl))
		base := 56 + tl
		kp, end := base, 4096
		if n.flags&1 != 0 {
			end -= 40
		}
		vp := end
		for i, r := range n.records {
			vp -= len(r.value)
			copy(b[kp:], r.key)
			copy(b[vp:], r.value)
			at := 56 + i*width
			np16(b[at:], uint16(kp-base))
			if width == 4 {
				repairCheck(len(r.key) == 16 && len(r.value) == map[bool]int{true: 16, false: 8}[n.level == 0], "invalid fixed record")
				np16(b[at+2:], uint16(end-vp))
			} else {
				np16(b[at+2:], uint16(len(r.key)))
				np16(b[at+4:], uint16(end-vp))
				np16(b[at+6:], uint16(len(r.value)))
			}
			kp += len(r.key)
		}
		repairCheck(kp <= vp, "packed B-tree overflow")
		np16(b[44:], uint16(kp-base))
		np16(b[46:], uint16(vp-kp))
		np16(b[48:], 0xffff)
		np16(b[52:], 0xffff)
		if n.flags&1 != 0 {
			copy(b[4056:], t.footer)
			keys := int64(nu64(t.footer[24:])) + t.keyDelta
			nodes := int64(nu64(t.footer[32:])) + t.nodeDelta
			repairCheck(keys >= 0 && nodes > 0, "invalid tree totals")
			np64(b[4080:], uint64(keys))
			np64(b[4088:], uint64(nodes))
			np32(b[4072:], max(nu32(b[4072:]), t.longestKey))
			np32(b[4076:], max(nu32(b[4076:]), t.longestValue))
		}
		objectChecksum(b)
		t.write(n.physical, b)
	}
}
func nativeKey(id, kind uint64, extra ...uint64) []byte {
	b := make([]byte, 8+8*len(extra))
	np64(b, id|kind<<60)
	for i, x := range extra {
		np64(b[8+8*i:], x)
	}
	return b
}
func (t *nativeTree) required(key []byte) []byte {
	v, ok := t.find(key)
	if !ok {
		panic(fmt.Sprintf("missing native record %x", key))
	}
	return v
}
