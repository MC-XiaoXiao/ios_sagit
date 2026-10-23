package main

// Offline COW updates to one volume. Unrelated native trees, sparse streams,
// extents, object maps and free queues are retained rather than imported.
import (
	"bytes"
	"crypto/sha256"
	"encoding/hex"
	"flag"
	"fmt"
	"io"
	"math/bits"
	"os"
	"path/filepath"
	"sort"

	"github.com/deploymenttheory/go-apfs-v2/pkg/apfs"
)

type nativeTracked struct {
	base   io.ReaderAt
	size   int64
	blocks map[uint64][]byte
}

func (t *nativeTracked) ReadAt(p []byte, off int64) (int, error) {
	if off < 0 || off+int64(len(p)) > t.size {
		return 0, io.ErrUnexpectedEOF
	}
	done := 0
	for done < len(p) {
		pos := off + int64(done)
		id := uint64(pos / 4096)
		b := t.blocks[id]
		if b == nil {
			b = make([]byte, 4096)
			n, e := t.base.ReadAt(b, int64(id)*4096)
			repairCheck(n == 4096 && (e == nil || e == io.EOF), "short source block")
			t.blocks[id] = b
		}
		within := int(pos % 4096)
		n := min(len(p)-done, 4096-within)
		copy(p[done:done+n], b[within:within+n])
		done += n
	}
	return done, nil
}

type nativeChunk struct {
	start            uint64
	count            uint32
	original, bitmap []byte
}
type nativeEphemeral struct {
	oid, addr uint64
	raw       []byte
}
type nativeEditor struct {
	source                          *nativeTracked
	container                       *apfs.Container
	delta                           *blockDelta
	chunks                          []nativeChunk
	allocated                       map[uint64]bool
	released                        map[uint64]bool
	oldIP, newIP                    []byte
	poolBase, poolCount, bmBase     uint64
	bmCount                         uint32
	ipAllocated                     map[uint64]bool
	oldPoolMetadata                 map[uint64]bool
	sm, cp, sb, primary             []byte
	objects                         []nativeEphemeral
	xid, oldFree                    uint64
	scan                            uint64
	systemBalance                   int64
	systemScope                     bool
	descStart, dataStart, newBitmap uint64
}

func (n *nativeEditor) read(p uint64) []byte {
	b := make([]byte, 4096)
	must(n.source.ReadAt(b, int64(p)*4096))
	return b
}
func (n *nativeEditor) write(p uint64, b []byte) { must(n.delta.WriteAt(b, int64(p)*4096)) }
func nativeUsed(b []byte, k uint64) bool         { return b[k/8]&(1<<(k%8)) != 0 }
func nativeSet(b []byte, k uint64, v bool) {
	if v {
		b[k/8] |= 1 << (k % 8)
	} else {
		b[k/8] &^= 1 << (k % 8)
	}
}
func (n *nativeEditor) isFree(p uint64) bool { return !nativeUsed(n.chunks[p/32768].original, p%32768) }
func (n *nativeEditor) allocate(count uint64) uint64 {
	repairCheck(count > 0, "zero allocation")
	var run uint64
	for p := max(n.scan, 1); p < uint64(n.source.size/4096); p++ {
		if n.isFree(p) && !n.allocated[p] {
			run++
		} else {
			run = 0
		}
		if run == count {
			start := p + 1 - count
			for k := start; k <= p; k++ {
				n.allocated[k] = true
				nativeSet(n.chunks[k/32768].bitmap, k%32768, true)
			}
			n.scan = p + 1
			if n.systemScope {
				n.systemBalance += int64(count)
			}
			return start
		}
	}
	// A failed large run search must not hide smaller holes from later callers.
	panic(fmt.Sprintf("System COW: no original free run of %d blocks", count))
}
func (n *nativeEditor) release(p, count uint64) {
	repairCheck(p > 0 && count > 0 && p+count <= uint64(n.source.size/4096), "invalid retired range")
	for k := p; k < p+count; k++ {
		repairCheck(!n.released[k] && nativeUsed(n.chunks[k/32768].bitmap, k%32768), "duplicate or free retired block")
		n.released[k] = true
		nativeSet(n.chunks[k/32768].bitmap, k%32768, false)
	}
	if n.systemScope {
		n.systemBalance -= int64(count)
	}
}
func newNativeEditor(reader io.ReaderAt, size int64) *nativeEditor {
	n := &nativeEditor{source: &nativeTracked{reader, size, map[uint64][]byte{}}, allocated: map[uint64]bool{}, released: map[uint64]bool{}, ipAllocated: map[uint64]bool{}, oldPoolMetadata: map[uint64]bool{}}
	n.container = must(apfs.Open(n.source, nil))
	s := n.container.Superblock
	repairCheck(s.BlockSize == 4096 && s.NumberOfBlocks*4096 == uint64(size) && s.FusionUUID == ([16]byte{}) && s.ReadOnlyCompatibleFeaturesFlags == 0 && s.IncompatibleFeaturesFlags == 2, "unsupported native container")
	repairCheck(s.XPDescBlocks&0x80000000 == 0 && s.XPDataBlocks&0x80000000 == 0 && s.XPDescLen == 2 && s.XPDescIndex+1 < s.XPDescBlocks, "unsupported checkpoint descriptor geometry")
	n.xid = s.XID + 1
	repairCheck(n.xid > s.XID, "XID overflow")
	n.primary = n.read(0)
	n.cp = n.read(s.XPDescBase + uint64(s.XPDescIndex))
	n.sb = n.read(s.XPDescBase + uint64(s.XPDescIndex) + 1)
	repairCheck(nu64(n.cp) == mustChecksum(n.cp) && nu64(n.sb) == mustChecksum(n.sb) && nu64(n.cp[16:]) == s.XID && nu64(n.sb[16:]) == s.XID && nu32(n.cp[32:]) == 1, "invalid latest checkpoint")
	count := nu32(n.cp[36:])
	repairCheck(count > 0 && count <= 101, "unsupported checkpoint mapping count")
	var total uint64
	seen := map[uint64]bool{}
	for i := uint32(0); i < count; i++ {
		m := n.cp[40+40*i:]
		length, oid, p := nu32(m[8:]), nu64(m[24:]), nu64(m[32:])
		repairCheck(length > 0 && length%4096 == 0 && !seen[oid] && nu64(m[16:]) == 0 && p >= s.XPDataBase && p+uint64(length/4096) <= s.XPDataBase+uint64(s.XPDataBlocks), "invalid checkpoint mapping")
		seen[oid] = true
		raw := make([]byte, length)
		must(n.source.ReadAt(raw, int64(p)*4096))
		repairCheck(nu64(raw) == mustChecksum(raw) && nu64(raw[8:]) == oid && nu64(raw[16:]) == s.XID && nu32(raw[24:]) == nu32(m), "invalid ephemeral object")
		n.objects = append(n.objects, nativeEphemeral{oid, p, raw})
		total += uint64(length / 4096)
		if oid == s.SpacemanOID {
			repairCheck(length == 4096, "multi-block spaceman unsupported")
			n.sm = raw
		}
	}
	repairCheck(n.sm != nil && total == uint64(s.XPDataLen), "incomplete checkpoint data")
	sm := n.sm
	repairCheck(nu32(sm[32:]) == 4096 && nu32(sm[36:]) == 32768 && nu32(sm[40:]) == 126 && nu64(sm[48:]) == uint64(size/4096) && nu32(sm[68:]) == 0 && nu64(sm[96:]) == 0, "unsupported allocator geometry")
	n.poolCount, n.poolBase, n.bmBase, n.bmCount = nu64(sm[0x98:]), nu64(sm[0xb0:]), nu64(sm[0xa8:]), nu32(sm[0xa4:])
	repairCheck(n.poolCount > 0 && n.poolCount <= 32768 && n.poolBase > 0 && n.poolBase+n.poolCount <= uint64(size/4096) && nu32(sm[0xa0:]) == 1 && n.bmCount >= 2 && n.bmCount <= 16, "unsupported internal pool")
	indexOff, xidOff, nextOff := nu32(sm[0x148:]), nu32(sm[0x144:]), nu32(sm[0x14c:])
	repairCheck(indexOff >= 0x150 && indexOff+2 <= 4096 && xidOff >= 0x150 && xidOff+8 <= 4096 && nextOff >= 0x150 && nextOff+2*n.bmCount <= 4096, "invalid pool arrays")
	index := nu16(sm[indexOff:])
	repairCheck(uint32(index) < n.bmCount, "invalid bitmap ring index")
	n.oldIP = n.read(n.bmBase + uint64(index))
	n.newIP = bytes.Clone(n.oldIP)
	n.newBitmap = n.bmBase + uint64((uint32(index)+1)%n.bmCount)
	poolObject := func(p uint64) {
		repairCheck(p >= n.poolBase && p < n.poolBase+n.poolCount && nativeUsed(n.oldIP, p-n.poolBase) && !n.oldPoolMetadata[p], "invalid source pool object")
		n.oldPoolMetadata[p] = true
	}
	cibCount, addrOff := nu32(sm[64:]), nu32(sm[80:])
	repairCheck(cibCount > 0 && addrOff >= 0x150 && uint64(addrOff)+8*uint64(cibCount) <= 4096, "unsupported CIB array")
	var covered uint64
	for i := uint32(0); i < cibCount; i++ {
		p := nu64(sm[uint64(addrOff)+8*uint64(i):])
		poolObject(p)
		b := n.read(p)
		repairCheck(nu64(b) == mustChecksum(b) && nu32(b[24:]) == 0x40000007 && nu32(b[32:]) == i, "invalid CIB")
		cnt := nu32(b[36:])
		repairCheck(cnt > 0 && cnt <= 126 && (i == cibCount-1 || cnt == 126), "unsupported CIB count")
		for j := uint32(0); j < cnt; j++ {
			ci := b[40+32*j:]
			start, length, free, bp := nu64(ci[8:]), nu32(ci[16:]), nu32(ci[20:]), nu64(ci[24:])
			repairCheck(start == covered && uint64(length) == min(uint64(32768), uint64(size/4096)-start) && free <= length, "invalid chunk")
			bitmap := make([]byte, 4096)
			if bp != 0 {
				poolObject(bp)
				bitmap = n.read(bp)
			}
			var used uint32
			for _, v := range bitmap {
				used += uint32(bits.OnesCount8(v))
			}
			repairCheck(used == length-free, "chunk bitmap mismatch")
			n.chunks = append(n.chunks, nativeChunk{start, length, bytes.Clone(bitmap), bytes.Clone(bitmap)})
			covered += uint64(length)
			n.oldFree += uint64(free)
		}
	}
	repairCheck(covered == uint64(size/4096) && n.oldFree == nu64(sm[72:]), "allocator totals mismatch")
	n.delta = &blockDelta{base: reader, blocks: map[uint64][]byte{}, size: size}
	// New descriptor ring is outside the old ring: APFS scans descriptors, so
	// writing a newer superblock into an old slot would commit before block zero.
	n.descStart = n.allocate(uint64(s.XPDescBlocks))
	n.scan = 1
	for k := uint64(0); k < uint64(s.XPDescBlocks); k++ {
		n.write(n.descStart+k, make([]byte, 4096))
	}
	n.release(s.XPDescBase, uint64(s.XPDescBlocks))
	active := map[uint64]bool{}
	for _, o := range n.objects {
		for k := o.addr; k < o.addr+uint64(len(o.raw)/4096); k++ {
			active[k] = true
		}
	}
	found := false
	for at := uint64(0); at+total <= uint64(s.XPDataBlocks); at++ {
		ok := true
		for k := uint64(0); k < total; k++ {
			if active[s.XPDataBase+at+k] {
				ok = false
				break
			}
		}
		if ok {
			n.dataStart = s.XPDataBase + at
			found = true
			break
		}
	}
	repairCheck(found, "no inactive checkpoint data span")
	return n
}
func (n *nativeEditor) poolAllocate() uint64 {
	for k := uint64(0); k < n.poolCount; k++ {
		p := n.poolBase + k
		if !nativeUsed(n.oldIP, k) && !n.ipAllocated[p] {
			n.ipAllocated[p] = true
			nativeSet(n.newIP, k, true)
			return p
		}
	}
	panic("System COW: internal pool COW capacity exhausted")
}
func (n *nativeEditor) finish(omap uint64) map[string]any {
	s := n.container.Superblock
	sm := n.sm
	xid := n.xid
	for p := range n.oldPoolMetadata {
		nativeSet(n.newIP, p-n.poolBase, false)
	}
	cibCount, addrOff := nu32(sm[64:]), nu32(sm[80:])
	var free uint64
	for i := uint32(0); i < cibCount; i++ {
		p := n.poolAllocate()
		b := make([]byte, 4096)
		np64(b[8:], p)
		np64(b[16:], xid)
		np32(b[24:], 0x40000007)
		np32(b[32:], i)
		count := min(126, len(n.chunks)-int(i)*126)
		np32(b[36:], uint32(count))
		for j := 0; j < count; j++ {
			chunk := &n.chunks[int(i)*126+j]
			var used uint32
			for _, v := range chunk.bitmap {
				used += uint32(bits.OnesCount8(v))
			}
			repairCheck(used <= chunk.count, "allocator overflow")
			ci := b[40+j*32:]
			np64(ci, xid)
			np64(ci[8:], chunk.start)
			np32(ci[16:], chunk.count)
			np32(ci[20:], chunk.count-used)
			free += uint64(chunk.count - used)
			if used > 0 {
				bp := n.poolAllocate()
				np64(ci[24:], bp)
				n.write(bp, chunk.bitmap)
			}
		}
		objectChecksum(b)
		n.write(p, b)
		np64(sm[uint64(addrOff)+8*uint64(i):], p)
	}
	np64(sm[72:], free)
	indexOff, xidOff, nextOff := nu32(sm[0x148:]), nu32(sm[0x144:]), nu32(sm[0x14c:])
	index := uint16(n.newBitmap - n.bmBase)
	np16(sm[indexOff:], index)
	np64(sm[xidOff:], xid)
	available := []uint16{}
	for i := uint16(0); i < uint16(n.bmCount); i++ {
		if i != index {
			available = append(available, i)
		}
	}
	np16(sm[0x140:], available[0])
	np16(sm[0x142:], available[len(available)-1])
	for i := uint32(0); i < n.bmCount; i++ {
		np16(sm[nextOff+2*i:], 0xffff)
	}
	for i := 0; i+1 < len(available); i++ {
		np16(sm[nextOff+2*uint32(available[i]):], available[i+1])
	}
	n.write(n.newBitmap, n.newIP)
	next := n.dataStart
	for i := range n.objects {
		o := &n.objects[i]
		np64(o.raw[16:], xid)
		objectChecksum(o.raw)
		n.write(next, o.raw)
		np64(n.cp[40+i*40+32:], next)
		next += uint64(len(o.raw) / 4096)
	}
	sb := n.sb
	np64(sb[112:], n.descStart)
	np64(sb[160:], omap)
	np32(sb[144:], uint32(n.dataStart-s.XPDataBase))
	np32(sb[148:], s.XPDataLen)
	np32(sb[132:], uint32((n.dataStart-s.XPDataBase+uint64(s.XPDataLen))%uint64(s.XPDataBlocks)))
	// Match the project's independently checked two-checkpoint publication
	// policy while retaining native volume crash flags and pending free queues.
	for i := uint64(0); i < 2; i++ {
		cxid := xid - 1 + i
		np64(n.cp[8:], n.descStart+2*i)
		np64(n.cp[16:], cxid)
		objectChecksum(n.cp)
		n.write(n.descStart+2*i, n.cp)
		np64(sb[16:], cxid)
		np64(sb[96:], cxid+1)
		np32(sb[128:], uint32(2*i+2))
		np32(sb[136:], uint32(2*i))
		np32(sb[140:], 2)
		objectChecksum(sb)
		n.write(n.descStart+2*i+1, sb)
	}
	n.write(0, sb)
	// Whitelist writes against the ORIGINAL maps, never the maps with retired
	// System nodes marked free. Current descriptor/data/IP bitmap stay intact.
	inactive := map[uint64]bool{n.newBitmap: true}
	for k := n.dataStart; k < next; k++ {
		inactive[k] = true
	}
	keys := []uint64{}
	for p := range n.delta.blocks {
		if p == 0 {
			continue
		}
		repairCheck(n.isFree(p) || n.ipAllocated[p] || inactive[p], "System update would overwrite an old live block")
		keys = append(keys, p)
	}
	sort.Slice(keys, func(i, j int) bool { return keys[i] < keys[j] })
	ranges := []checkpointWriteRange{}
	for _, p := range keys {
		if len(ranges) > 0 && ranges[len(ranges)-1].Start+ranges[len(ranges)-1].Count == p {
			ranges[len(ranges)-1].Count++
		} else {
			ranges = append(ranges, checkpointWriteRange{p, 1})
		}
	}
	sourceKeys := []uint64{}
	for p := range n.source.blocks {
		sourceKeys = append(sourceKeys, p)
	}
	sort.Slice(sourceKeys, func(i, j int) bool { return sourceKeys[i] < sourceKeys[j] })
	identity := sha256.New()
	structures := []checkpointSourceBlock{}
	for _, p := range sourceKeys {
		sum := sha256.Sum256(n.source.blocks[p])
		var key [8]byte
		np64(key[:], p)
		must(identity.Write(key[:]))
		must(identity.Write(sum[:]))
		structures = append(structures, checkpointSourceBlock{p, hex.EncodeToString(sum[:])})
	}
	fingerprint := sha256.Sum256(n.primary)
	return map[string]any{"schema": 1, "repair": "system-overlay", "container_bytes": n.source.size, "source_superblock_sha256": hex.EncodeToString(fingerprint[:]), "old_transaction_id": s.XID, "transaction_id": xid, "old_free_bytes": n.oldFree * 4096, "new_free_bytes": free * 4096, "changed_blocks": len(n.delta.blocks), "metadata_start_block": keys[0], "write_ranges": ranges, "source_identity_kind": "apfs-checkpoint-structures-sha256", "source_structures": structures, "source_structure_sha256": hex.EncodeToString(identity.Sum(nil)), "source_structure_bytes": len(structures) * 4096, "commit_block": 0, "native_volume_state_preserved": true, "non_system_volume_trees_preserved": true, "device_written": false}
}

func systemUpdatePlan(args []string) {
	nativeUpdatePlan(args, "system")
}

func dataUpdatePlan(args []string) {
	nativeUpdatePlan(args, "data")
}

func nativeUpdatePlan(args []string, selector string) {
	flags := flag.NewFlagSet(selector+"-update-plan", flag.ExitOnError)
	source := flags.String("source", "", "offline container")
	offset := flags.Int64("source-offset", 0, "container byte offset")
	size := flags.Int64("source-size", 0, "container size")
	overlay := flags.String("overlay", "", "regular-file System overlay")
	patch := flags.String("file-block-patch", "", "same-size System file block patch descriptor")
	setupComplete := flags.Bool("development-setup-complete", false, "merge Setup completion preferences in the native Data volume")
	output := flags.String("output", "", "COW delta")
	flags.Parse(args)
	repairCheck(*source != "" && *output != "", "native update requires source and output")
	if selector == "data" {
		repairCheck(*setupComplete && *overlay == "" && *patch == "", "data-update-plan requires --development-setup-complete")
	} else {
		repairCheck(!*setupComplete && (*overlay != "" || *patch != ""), "system-update-plan requires overlay or file-block-patch")
	}
	f := must(os.Open(*source))
	defer f.Close()
	info := must(f.Stat())
	if *size == 0 && info.Mode().IsRegular() {
		*size = info.Size() - *offset
	}
	repairCheck(*offset >= 0 && *offset%4096 == 0 && *size > 0 && *size%4096 == 0 && (!info.Mode().IsRegular() || *offset+*size <= info.Size()), "invalid source geometry")
	reader := io.NewSectionReader(f, *offset, *size)
	n := newNativeEditor(reader, *size)
	defer n.container.Close()
	var report map[string]any
	if selector == "data" {
		report = n.applyVolume("data", nativeSetupInputs(must(n.container.VolumeBySelector("data"))), "")
		report["development_setup_complete"] = true
	} else {
		report = n.applySystem(*overlay, *patch)
	}
	report["source"], report["source_offset"] = *source, *offset
	n.delta.save(*output)
	report["delta_sha256"] = hash(*output)
	report["patch_bytes"] = 24 + len(n.delta.blocks)*4104
	writeJSON(*output+".json", report)
	fmt.Printf("%s COW: %d blocks; other volume trees retained\n", selector, len(n.delta.blocks))
}

func nativeXFields(inode []byte) map[byte][]byte {
	repairCheck(len(inode) >= 96, "short inode")
	count := int(nu16(inode[92:]))
	cursor := 96 + 4*count
	fields := map[byte][]byte{}
	for i := 0; i < count; i++ {
		at := 96 + 4*i
		length := int(nu16(inode[at+2:]))
		repairCheck(cursor+length <= len(inode), "invalid inode extension")
		fields[inode[at]] = inode[cursor : cursor+length]
		cursor += (length + 7) &^ 7
	}
	return fields
}
func nativeNewInode(parent, id uint64, name string, size uint64, mode uint16) []byte {
	nameBytes := append([]byte(name), 0)
	padded := (len(nameBytes) + 7) &^ 7
	b := make([]byte, 104+padded+40)
	np64(b, parent)
	np64(b[8:], id)
	np64(b[48:], 0x8000)
	np32(b[56:], 1)
	np16(b[80:], mode)
	np16(b[92:], 2)
	np16(b[94:], uint16(padded+40))
	b[96], b[97] = 4, 2
	np16(b[98:], uint16(len(nameBytes)))
	b[100], b[101] = 8, 0x20
	np16(b[102:], 40)
	copy(b[104:], nameBytes)
	np64(b[104+padded:], size)
	np64(b[112+padded:], (size+4095)&^4095)
	np64(b[128+padded:], size)
	return b
}
func nativeDrecKey(parent uint64, name string) []byte {
	b := make([]byte, 13+len(name))
	np64(b, parent|9<<60)
	np32(b[8:], apfs.CalculateNameHash([]byte(name), false)<<10|uint32(len(name)+1))
	copy(b[12:], name)
	return b
}
func nativeXattrKey(id uint64, name string) []byte {
	b := make([]byte, 11+len(name))
	np64(b, id|4<<60)
	np16(b[8:], uint16(len(name)+1))
	copy(b[10:], name)
	return b
}

// Repack extensions to retain unrelated inode metadata when adding DSTREAM
// to an inline-compressed or empty file that did not previously have one.
func nativeSetStream(inode []byte, stream []byte) []byte {
	fields := nativeXFields(inode)
	count := int(nu16(inode[92:]))
	headers := bytes.Clone(inode[96 : 96+4*count])
	values := []byte{}
	found := false
	for i := 0; i < count; i++ {
		h := headers[4*i:]
		value := fields[h[0]]
		if h[0] == 8 {
			value = stream
			h[1] = 0x20
			np16(h[2:], 40)
			found = true
		}
		values = append(values, value...)
		for len(values)%8 != 0 {
			values = append(values, 0)
		}
	}
	if !found {
		headers = append(headers, 8, 0x20, 40, 0)
		values = append(values, stream...)
		count++
	}
	result := bytes.Clone(inode[:96])
	np16(result[92:], uint16(count))
	np16(result[94:], uint16(len(values)))
	result = append(result, headers...)
	return append(result, values...)
}
func nativeOmapKey(oid, xid uint64) []byte {
	b := make([]byte, 16)
	np64(b, oid)
	np64(b[8:], xid)
	return b
}

func (n *nativeEditor) applySystem(overlay string, patches ...string) map[string]any {
	return n.applyVolume("system", nativeSystemInputs(overlay, patches), overlay)
}

func (n *nativeEditor) applyVolume(selector string, inputs []nativeSystemInput, overlay string) map[string]any {
	c := n.container
	v := must(c.VolumeBySelector(selector))
	vs := v.Superblock
	repairCheck(vs.FormattedByString() == "go-apfs (apfswrite)" && must(v.NumberOfSnapshots()) == 0 && !must(v.IsEncrypted()) && vs.ReadOnlyCompatibleFeaturesFlags == 0 && vs.IncompatibleFeaturesFlags == apfs.VolumeIncompatNormalizationInsens, "native update requires a project-created, unencrypted, case-sensitive volume without snapshots")
	identities := []volumeIdentity{}
	opaque := map[string]string{}
	for i, count := 0, must(c.NumberOfVolumes()); i < count; i++ {
		vol := must(c.Volume(i))
		identities = append(identities, volumeIdentity{must(vol.UTF8Name()), vol.Superblock.Role, uuidString(must(vol.Identifier())), uuidString(must(vol.VolumeGroupIdentifier()))})
		if vol.Superblock.Role != vs.Role {
			p := must(c.ObjectMapBTree.PhysicalAddressForOID(n.source, vol.Superblock.OID, c.Superblock.XID))
			sum := sha256.Sum256(n.read(p))
			opaque[uuidString(must(vol.Identifier()))] = hex.EncodeToString(sum[:])
		}
	}
	omapRaw := n.read(vs.OmapOID)
	repairCheck(nu64(omapRaw) == mustChecksum(omapRaw) && nu32(omapRaw[24:]) == 0x4000000b && nu32(omapRaw[36:]) == 0, "unsupported target volume object map")
	n.systemScope = true
	om := newNativeTree(n.read, n.allocate, n.release, n.write, nu64(omapRaw[48:]), false, nil, n.xid, nativeOmapCompare)
	mappings := map[uint64]uint64{}
	mapKeys := map[uint64][]byte{}
	om.walk(func(r nativeRecord) {
		oid := nu64(r.key)
		repairCheck(mappings[oid] == 0 && len(r.value) == 16 && nu32(r.value) == 0 && nu32(r.value[4:]) == 4096, "System map has versions, deleted or non-page objects")
		mappings[oid] = nu64(r.value[8:])
		mapKeys[oid] = bytes.Clone(r.key)
	})
	fs := newNativeTree(n.read, n.allocate, n.release, n.write, vs.RootTreeOID, true, mappings, n.xid, nativeFSCompare)
	// Virtual tree IDs use the container namespace, including other volumes
	// and ephemeral objects; a volume-local maximum can collide.
	fs.nextOID = c.Superblock.NextOID
	ext := newNativeTree(n.read, n.allocate, n.release, n.write, vs.ExtentrefTreeOID, false, nil, n.xid, nativeIDCompare)
	nextInode := vs.NextObjID
	var addedFiles, allocatedData, freedData uint64
	files := map[string]any{}
	for _, input := range inputs {
		relative, payload := input.path, input.payload
		stMode := os.FileMode(0644)
		if input.patch == nil {
			stMode = input.mode
		}
		parentPath, name := filepath.ToSlash(filepath.Dir(relative)), filepath.Base(relative)
		if parentPath == "." {
			parentPath = "/"
		}
		parent := must(v.FileEntryByPath(parentPath))
		repairCheck(must(parent.FileMode())&0170000 == 0040000, "parent must exist as directory")
		repairCheck(parent.Inode.Flags&2 == 0, "System overlay parent has unsupported directory statistics")
		parentID := must(parent.Identifier())
		target, err := v.FileEntryByPath(relative)
		var id uint64
		var inode []byte
		if err == nil {
			repairCheck(target.Inode.Flags&2 == 0, "System overlay target has unsupported directory statistics")
			repairCheck(must(target.FileMode())&0170000 == 0100000 && must(target.OwnerIdentifier()) == input.uid && must(target.GroupIdentifier()) == input.gid, "target is not a regular file with the expected ownership: "+relative)
			id = must(target.Identifier())
			inode = fs.required(nativeKey(id, 3))
			if selector == "data" {
				np16(inode[80:], uint16(0100000|stMode.Perm()))
			}
			repairCheck(nu32(inode[56:]) == 1 && nu64(inode[8:]) == id, "hardlinked/shared target unsupported: "+relative)
			repairCheck(!must(target.HasExtendedAttributeByName("com.apple.ResourceFork")), "resource-fork replacement unsupported")
			stream := must(target.RawDataStream())
			if input.patch != nil {
				changed := n.patchSystemStream(fs, ext, target, inode, relative, input.patch)
				allocatedData += changed
				freedData += changed
				files[relative] = map[string]string{"sha256": input.patch.DerivedSHA256, "block_patch": "true"}
				continue
			}
			compressed := nu32(inode[68:])&0x20 != 0
			if compressed {
				attribute := must(target.ExtendedAttributeByName("com.apple.decmpfs"))
				a := attribute.AttributeValues
				repairCheck(a.Flags == 2 && len(a.ValueData) >= 16 && nu32(a.ValueData) == 0x636d7066 && stream.Size() == 0 && len(stream.PhysicalExtents) == 0, "only inline compression with an empty raw stream is supported")
				// A successful logical read verifies the original compression header.
				must(v.ReadFile(relative))
				fs.put(nativeXattrKey(id, "com.apple.decmpfs"), nil)
				np32(inode[68:], nu32(inode[68:])&^0x20)
				np64(inode[48:], nu64(inode[48:])&^0x40000)
				np64(inode[84:], 0)
			} else {
				repairCheck(!must(target.HasExtendedAttributeByName("com.apple.decmpfs")), "stale compression metadata unsupported")
			}
			repairCheck(len(stream.PhysicalExtents) <= 1, "replacement target must be empty or a single extent")
			if len(stream.PhysicalExtents) == 1 {
				old := stream.PhysicalExtents[0]
				repairCheck(old.LogicalOffset == 0 && old.EncryptionIdentifier == 0 && old.PhysicalBlockNumber > 0 && old.DataSize%4096 == 0, "unsupported replacement extent")
				blockKey := nativeKey(old.PhysicalBlockNumber, 2)
				record := ext.required(blockKey)
				repairCheck(len(record) == 20 && nu64(record) == old.DataSize/4096|1<<60 && nu64(record[8:]) == id && nu32(record[16:]) == 1, "shared or noncanonical replacement extent")
				ext.put(blockKey, nil)
				n.release(old.PhysicalBlockNumber, old.DataSize/4096)
				freedData += old.DataSize / 4096
			} else {
				repairCheck(stream.Size() == 0, "missing raw extents")
			}
			oldField := nativeXFields(inode)[8]
			repairCheck((len(oldField) == 0 && stream.Size() == 0) || (len(oldField) == 40 && nu64(oldField) == stream.Size()), "target stream metadata mismatch")
			field := make([]byte, 40)
			np64(field, uint64(len(payload)))
			np64(field[8:], uint64((len(payload)+4095)&^4095))
			np64(field[24:], uint64(len(payload)))
			inode = nativeSetStream(inode, field)
			fs.put(nativeKey(id, 3), inode)
			if record, exists := fs.find(nativeKey(id, 6)); exists {
				repairCheck(len(record) == 4 && nu32(record) == 1, "shared stream unsupported")
			} else {
				record = make([]byte, 4)
				np32(record, 1)
				fs.put(nativeKey(id, 6), record)
			}
		} else {
			repairCheck(input.patch == nil, "block patch requires an existing System file")
			// Distinguish ENOENT from malformed/corrupt lookups by searching the
			// native dentry index. A missing regular-file path alone is permitted.
			key := nativeDrecKey(parentID, name)
			_, exists := fs.find(key)
			repairCheck(!exists, "unreadable existing target")
			id = nextInode
			nextInode++
			inode = nativeNewInode(parentID, id, name, uint64(len(payload)), uint16(0100000|stMode.Perm()))
			np32(inode[72:], input.uid)
			np32(inode[76:], input.gid)
			fs.put(nativeKey(id, 3), inode)
			ds := make([]byte, 4)
			np32(ds, 1)
			fs.put(nativeKey(id, 6), ds)
			drec := make([]byte, 18)
			np64(drec, id)
			np16(drec[16:], 8)
			fs.put(key, drec)
			pinode := fs.required(nativeKey(parentID, 3))
			repairCheck(nu16(pinode[80:])&0170000 == 0040000 && nu64(pinode[48:])&2 == 0 && nu32(pinode[56:]) < ^uint32(0), "invalid parent or unsupported directory statistics")
			np32(pinode[56:], nu32(pinode[56:])+1)
			fs.put(nativeKey(parentID, 3), pinode)
			addedFiles++
		}
		count := uint64((len(payload) + 4095) / 4096)
		p := n.allocate(count)
		raw := make([]byte, count*4096)
		copy(raw, payload)
		n.write(p, raw)
		allocatedData += count
		fileExtent := make([]byte, 24)
		np64(fileExtent, count*4096)
		np64(fileExtent[8:], p)
		fs.put(nativeKey(id, 8, 0), fileExtent)
		ref := make([]byte, 20)
		np64(ref, count|1<<60)
		np64(ref[8:], id)
		np32(ref[16:], 1)
		ext.put(nativeKey(p, 2), ref)
		sum := sha256.Sum256(payload)
		files[relative] = map[string]string{"sha256": hex.EncodeToString(sum[:])}
	}
	fs.flush()
	np64(n.sb[88:], fs.nextOID)
	ext.flush()
	for node := range fs.dirty {
		old := mapKeys[node.oid]
		if old != nil {
			om.put(old, nil)
		}
		if node.dropped {
			continue
		}
		value := make([]byte, 16)
		np32(value[4:], 4096)
		np64(value[8:], node.physical)
		om.put(nativeOmapKey(node.oid, n.xid), value)
	}
	om.flush()
	newOmap := n.allocate(1)
	n.release(vs.OmapOID, 1)
	np64(omapRaw[8:], newOmap)
	np64(omapRaw[16:], n.xid)
	np64(omapRaw[48:], om.root.physical)
	objectChecksum(omapRaw)
	n.write(newOmap, omapRaw)
	vsPaddr := must(c.ObjectMapBTree.PhysicalAddressForOID(n.source, vs.OID, c.Superblock.XID))
	rawVS := n.read(vsPaddr)
	newVS := n.allocate(1)
	n.release(vsPaddr, 1)
	np64(rawVS[16:], n.xid)
	np64(rawVS[128:], newOmap)
	np64(rawVS[136:], fs.root.oid)
	np64(rawVS[144:], ext.root.physical)
	np64(rawVS[176:], nextInode)
	np64(rawVS[184:], vs.NumberOfFiles+addedFiles)
	// FsAllocCount excludes the volume superblock itself; its relocation has
	// zero net balance. Allocated/freed lifetime counters describe stream data.
	repairCheck(int64(vs.NumberOfAllocatedBlocks)+n.systemBalance > 0, "System allocation underflow")
	np64(rawVS[88:], uint64(int64(vs.NumberOfAllocatedBlocks)+n.systemBalance))
	np64(rawVS[224:], vs.TotalBlocksAllocated+allocatedData)
	np64(rawVS[232:], vs.TotalBlocksFreed+freedData)
	objectChecksum(rawVS)
	n.write(newVS, rawVS)
	n.systemScope = false
	containerOmapRaw := n.read(c.Superblock.OmapOID)
	cm := newNativeTree(n.read, n.allocate, n.release, n.write, nu64(containerOmapRaw[48:]), false, nil, n.xid, nativeOmapCompare)
	descriptor := must(c.ObjectMapBTree.DescriptorByObjectIdentifier(n.source, vs.OID, c.Superblock.XID))
	cm.put(nativeOmapKey(vs.OID, descriptor.Key.XID), nil)
	cv := make([]byte, 16)
	np32(cv[4:], 4096)
	np64(cv[8:], newVS)
	cm.put(nativeOmapKey(vs.OID, n.xid), cv)
	cm.flush()
	newContainerOmap := n.allocate(1)
	n.release(c.Superblock.OmapOID, 1)
	np64(containerOmapRaw[8:], newContainerOmap)
	np64(containerOmapRaw[16:], n.xid)
	np64(containerOmapRaw[48:], cm.root.physical)
	objectChecksum(containerOmapRaw)
	n.write(newContainerOmap, containerOmapRaw)
	// Capture fstab before closing the original tree and before building the
	// source-structure digest, so its actual source pages are bound too.
	system := must(c.VolumeBySelector("system"))
	fstab := must(system.ReadFile("private/etc/fstab"))
	report := n.finish(newContainerOmap)
	report["repair"] = selector + "-overlay"
	report["overlay_volume"] = selector
	report["non_target_volume_trees_preserved"] = true
	delete(report, "non_system_volume_trees_preserved")
	if selector == "system" {
		report["non_system_volume_trees_preserved"] = true
	}
	report["system_uuid"] = uuidString(must(system.Identifier()))
	report["data_uuid"] = uuidString(must(must(c.VolumeBySelector("data")).Identifier()))
	report["volume_group_uuid"] = uuidString(must(system.VolumeGroupIdentifier()))
	report["volumes"] = identities
	report["fstab"] = string(fstab)
	report["overlay"] = overlay
	report["overlay_files"] = files
	report["preserved_volume_superblocks"] = opaque
	uncachedDelta := &blockDelta{base: n.source.base, blocks: n.delta.blocks, size: n.delta.size}
	actual := must(apfs.Open(uncachedDelta, nil))
	defer actual.Close()
	result := must(actual.VolumeBySelector(selector))
	repairCheck(bytes.Equal(must(must(actual.VolumeBySelector("system")).ReadFile("private/etc/fstab")), fstab), "fstab changed")
	for relative, meta := range files {
		entry := must(result.FileEntryByPath(relative))
		repairCheck(nativeStreamHash(entry) == meta.(map[string]string)["sha256"], "target file readback mismatch")
	}
	for i, count := 0, must(actual.NumberOfVolumes()); i < count; i++ {
		vol := must(actual.Volume(i))
		key := uuidString(must(vol.Identifier()))
		if expected := opaque[key]; expected != "" {
			p := must(actual.ObjectMapBTree.PhysicalAddressForOID(n.delta, vol.Superblock.OID, actual.Superblock.XID))
			b := make([]byte, 4096)
			must(n.delta.ReadAt(b, int64(p)*4096))
			sum := sha256.Sum256(b)
			repairCheck(hex.EncodeToString(sum[:]) == expected, "unrelated volume superblock changed")
		}
	}
	_, free := freeTail(actual, n.delta)
	repairCheck(free*4096 == report["new_free_bytes"], "final allocation mismatch")
	return report
}
