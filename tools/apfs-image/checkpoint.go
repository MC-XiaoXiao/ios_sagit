package main

// Offline repair of the project's undersized checkpoint rings. Volume trees,
// file extents, free-queue records and the internal pool stay in place. New
// chunk bitmaps/CIBs use only slots that were free in the old internal pool;
// block zero is the sole write to an old live block.

import (
	"bytes"
	"crypto/sha256"
	"encoding/binary"
	"encoding/hex"
	"flag"
	"fmt"
	"io"
	"math/bits"
	"os"
	"sort"

	"github.com/deploymenttheory/go-apfs-v2/pkg/apfs"
	"github.com/deploymenttheory/go-apfs-v2/pkg/apfswrite"
)

type checkpointWriteRange struct {
	Start uint64 `json:"start_block"`
	Count uint64 `json:"block_count"`
}

type checkpointSourceBlock struct {
	Block  uint64 `json:"block"`
	SHA256 string `json:"sha256"`
}

func repairCheck(ok bool, message string) {
	if !ok {
		panic("checkpoint repair: " + message)
	}
}

func objectChecksum(raw []byte) {
	binary.LittleEndian.PutUint64(raw, must(apfs.CalculateFletcher64(raw[8:], 0)))
}

func checkpointRepair(reader io.ReaderAt, size int64) (*blockDelta, map[string]any) {
	u16, u32, u64 := binary.LittleEndian.Uint16, binary.LittleEndian.Uint32, binary.LittleEndian.Uint64
	p16, p32, p64 := binary.LittleEndian.PutUint16, binary.LittleEndian.PutUint32, binary.LittleEndian.PutUint64
	repairCheck(size >= 4096 && size%4096 == 0, "invalid container size")
	blocks := uint64(size / 4096)
	sourceBlocks := map[uint64]string{}
	read := func(addr, count uint64) []byte {
		repairCheck(addr < blocks && count > 0 && count <= blocks-addr, "read outside container")
		raw := make([]byte, count*4096)
		must(reader.ReadAt(raw, int64(addr)*4096))
		for i := uint64(0); i < count; i++ {
			sum := sha256.Sum256(raw[i*4096 : (i+1)*4096])
			digest := hex.EncodeToString(sum[:])
			previous, exists := sourceBlocks[addr+i]
			repairCheck(!exists || previous == digest, "source metadata changed during planning")
			sourceBlocks[addr+i] = digest
		}
		return raw
	}
	checkObject := func(raw []byte) {
		repairCheck(must(apfs.CalculateFletcher64(raw[8:], 0)) == u64(raw), "metadata checksum mismatch")
	}
	primary := read(0, 1)
	checkObject(primary)
	repairCheck(bytes.Equal(primary[32:36], []byte("NXSB")) && u32(primary[36:]) == 4096 &&
		u32(primary[104:]) > 0 && u32(primary[104:]) <= 4096, "unsupported primary checkpoint descriptor geometry")
	read(u64(primary[112:]), uint64(u32(primary[104:])))
	c := must(apfs.Open(reader, nil))
	defer c.Close()
	s := c.Superblock
	repairCheck(s.BlockSize == 4096 && s.NumberOfBlocks == blocks && s.FusionUUID == ([16]byte{}) &&
		s.ReadOnlyCompatibleFeaturesFlags == 0 && s.IncompatibleFeaturesFlags == 2, "requires a non-Fusion 4096-byte APFS v2 container")
	repairCheck(s.XPDescBlocks&0x80000000 == 0 && s.XPDataBlocks&0x80000000 == 0 &&
		s.XPDescLen == 2 && s.XPDescIndex < s.XPDescBlocks && s.XPDataLen > 0 &&
		s.XPDataLen <= s.XPDataBlocks, "unsupported checkpoint geometry (one mapping block required)")
	mapAddr := s.XPDescBase + uint64(s.XPDescIndex)
	cp := read(mapAddr, 1)
	sb := read(s.XPDescBase+uint64((s.XPDescIndex+1)%s.XPDescBlocks), 1)
	checkObject(cp)
	checkObject(sb)
	repairCheck(u64(cp[16:]) == s.XID && u32(cp[24:]) == 0x4000000c && u32(cp[32:]) == 1 &&
		u64(sb[16:]) == s.XID && bytes.Equal(sb[32:36], []byte("NXSB")), "incomplete latest checkpoint")
	count := u32(cp[36:])
	repairCheck(count > 0 && count <= 101, "invalid checkpoint mapping count")
	type ephemeral struct {
		oid, addr uint64
		raw       []byte
	}
	objects := make([]ephemeral, count)
	var sm []byte
	var total uint32
	seen := map[uint64]bool{}
	for i := range objects {
		m := cp[40+i*40:]
		n, oid, addr := u32(m[8:]), u64(m[24:]), u64(m[32:])
		repairCheck(n > 0 && n%4096 == 0 && n/4096 <= s.XPDataLen && !seen[oid] &&
			addr >= s.XPDataBase && addr-s.XPDataBase+uint64(n/4096) <= uint64(s.XPDataBlocks), "invalid ephemeral mapping")
		seen[oid] = true
		raw := read(addr, uint64(n/4096))
		checkObject(raw)
		repairCheck(u64(raw[8:]) == oid && u64(raw[16:]) == s.XID && u32(raw[24:]) == u32(m) &&
			u32(raw[28:]) == u32(m[4:]) && u32(m)&0xc0000000 == 0x80000000 && u64(m[16:]) == 0,
			"ephemeral identity/type mismatch")
		objects[i] = ephemeral{oid, addr, raw}
		total += n / 4096
		if oid == s.SpacemanOID {
			repairCheck(n == 4096 && u32(m) == 0x80000005, "only single-block spaceman supported")
			sm = raw
		}
	}
	repairCheck(sm != nil && total == s.XPDataLen, "inconsistent checkpoint data length")
	// Do not import/rebuild native volume metadata: even files with multiple
	// extents, snapshots and opaque attributes remain byte-for-byte unchanged.
	identities := []volumeIdentity{}
	for i, n := 0, must(c.NumberOfVolumes()); i < n; i++ {
		v := must(c.Volume(i))
		repairCheck(v.Superblock.FormattedByString() == "go-apfs (apfswrite)", "container was not created by this project")
		identities = append(identities, volumeIdentity{must(v.UTF8Name()), v.Superblock.Role, uuidString(must(v.Identifier())), uuidString(must(v.VolumeGroupIdentifier()))})
	}
	repairCheck(u32(sm[32:]) == 4096 && u32(sm[36:]) == 32768 && u32(sm[40:]) == 126 &&
		u64(sm[48:]) == blocks && u32(sm[68:]) == 0 && u64(sm[96:]) == 0, "unsupported spaceman device geometry")
	poolCount, poolBase := u64(sm[0x98:]), u64(sm[0xb0:])
	bmBase, bmCount := u64(sm[0xa8:]), u32(sm[0xa4:])
	repairCheck(poolCount > 0 && poolCount <= 32768 && poolBase > 0 && poolBase < blocks && poolCount <= blocks-poolBase &&
		u32(sm[0xa0:]) == 1 && bmCount == 16, "unsupported internal-pool geometry")
	indexOff, xidOff, nextOff := u32(sm[0x148:]), u32(sm[0x144:]), u32(sm[0x14c:])
	repairCheck(indexOff >= 0x150 && indexOff+2 <= 4096 && xidOff >= 0x150 && xidOff+8 <= 4096 &&
		nextOff >= 0x150 && nextOff+2*bmCount <= 4096, "internal-pool arrays outside spaceman")
	index := u16(sm[indexOff:])
	repairCheck(uint32(index) < bmCount, "invalid current internal-pool bitmap")
	oldIP := read(bmBase+uint64(index), 1)
	newIP := bytes.Clone(oldIP)
	used := func(bitmap []byte, bit uint64) bool { return bitmap[bit/8]&(1<<(bit%8)) != 0 }
	set := func(bitmap []byte, bit uint64, value bool) {
		if value {
			bitmap[bit/8] |= 1 << (bit % 8)
		} else {
			bitmap[bit/8] &^= 1 << (bit % 8)
		}
	}
	oldMetadata := map[uint64]bool{}
	poolObject := func(addr uint64) {
		repairCheck(addr >= poolBase && addr-poolBase < poolCount && used(oldIP, addr-poolBase) && !oldMetadata[addr],
			"duplicate, free or out-of-pool allocation metadata")
		oldMetadata[addr] = true
	}
	type chunkInfo struct {
		start uint64
		count uint32
		bits  []byte
	}
	chunks := []chunkInfo{}
	cibCount, addrOff := u32(sm[64:]), u32(sm[80:])
	repairCheck(cibCount > 0 && addrOff >= 0x150 && uint64(addrOff)+uint64(cibCount)*8 <= 4096, "invalid CIB address array")
	var covered, oldFree uint64
	for i := uint32(0); i < cibCount; i++ {
		addr := u64(sm[uint64(addrOff)+8*uint64(i):])
		poolObject(addr)
		cib := read(addr, 1)
		checkObject(cib)
		n := u32(cib[36:])
		repairCheck(u64(cib[8:]) == addr && u32(cib[24:]) == 0x40000007 && u32(cib[32:]) == i &&
			n > 0 && n <= 126 && (i == cibCount-1 || n == 126), "invalid chunk-info block")
		for j := uint32(0); j < n; j++ {
			ci := cib[40+32*j:]
			start, n, free, bmap := u64(ci[8:]), u32(ci[16:]), u32(ci[20:]), u64(ci[24:])
			repairCheck(start == covered && uint64(n) == min(uint64(32768), blocks-start) && n > 0 && free <= n,
				"invalid chunk range")
			bitmap := make([]byte, 4096)
			if bmap != 0 {
				poolObject(bmap)
				bitmap = read(bmap, 1)
			}
			var allocated uint32
			for k := uint32(0); k < n; k++ {
				if used(bitmap, uint64(k)) {
					allocated++
				}
			}
			repairCheck(allocated == n-free, "bitmap and chunk free count disagree")
			chunks = append(chunks, chunkInfo{start, n, bitmap})
			covered += uint64(n)
			oldFree += uint64(free)
		}
	}
	repairCheck(covered == blocks && uint64(len(chunks)) == u64(sm[56:]) && oldFree == u64(sm[72:]), "spaceman totals disagree")
	original := make([][]byte, len(chunks))
	for i := range chunks {
		original[i] = bytes.Clone(chunks[i].bits)
	}
	wasFree := func(addr uint64) bool { return !used(original[addr/32768], addr%32768) }
	// Include free-queue growth in the same policy used by the image writer.
	budget := total
	for _, off := range []int{0xc8, 0xf0} {
		limit := uint32(u16(sm[off+24:]))
		if limit > 0 {
			budget += limit - 1
		}
	}
	descBlocks, dataBlocks := apfswrite.CheckpointAreaBlocks(blocks, budget)
	repairCheck(s.XPDescBlocks < descBlocks || s.XPDataBlocks < dataBlocks, "checkpoint areas already meet writable reserve policy")
	descBlocks, dataBlocks = max(descBlocks, s.XPDescBlocks), max(dataBlocks, s.XPDataBlocks)
	need := uint64(descBlocks) + uint64(dataBlocks) + uint64(bmCount)
	var start, run uint64
	for addr := uint64(1); addr < blocks; addr++ {
		if wasFree(addr) {
			run++
			if run == need {
				start = addr + 1 - run
				break
			}
		} else {
			run = 0
		}
	}
	repairCheck(start != 0, "no contiguous free range large enough for checkpoint reserve")
	dataBase, newBMBase := start+uint64(descBlocks), start+uint64(descBlocks)+uint64(dataBlocks)
	markGlobal := func(addr, count uint64, value bool) {
		repairCheck(addr > 0 && addr < blocks && count <= blocks-addr, "allocation range outside container")
		for k := addr; k < addr+count; k++ {
			repairCheck(used(chunks[k/32768].bits, k%32768) != value, "overlapping or unallocated metadata range")
			set(chunks[k/32768].bits, k%32768, value)
		}
	}
	markGlobal(start, need, true)
	markGlobal(s.XPDescBase, uint64(s.XPDescBlocks), false)
	markGlobal(s.XPDataBase, uint64(s.XPDataBlocks), false)
	markGlobal(bmBase, uint64(bmCount), false)
	delta := &blockDelta{base: reader, blocks: map[uint64][]byte{}, size: size}
	write := func(addr uint64, raw []byte) { must(delta.WriteAt(raw, int64(addr)*4096)) }
	// Initialize all spare ring slots, so stale bytes in free space cannot be
	// interpreted as another checkpoint by a later mount.
	for k := start; k < start+need; k++ {
		write(k, make([]byte, 4096))
	}
	allocatedIP := map[uint64]bool{}
	allocateIP := func() uint64 {
		for k := uint64(0); k < poolCount; k++ {
			if !used(oldIP, k) && !allocatedIP[poolBase+k] {
				allocatedIP[poolBase+k] = true
				set(newIP, k, true)
				return poolBase + k
			}
		}
		panic("checkpoint repair: insufficient unused internal-pool slots for COW")
	}
	for addr := range oldMetadata {
		set(newIP, addr-poolBase, false)
	}
	xid := s.XID + 1
	repairCheck(xid > s.XID, "transaction id overflow")
	var newFree uint64
	for i := uint32(0); i < cibCount; i++ {
		cibAddr := allocateIP()
		cib := make([]byte, 4096)
		p64(cib[8:], cibAddr)
		p64(cib[16:], xid)
		p32(cib[24:], 0x40000007)
		p32(cib[32:], i)
		n := min(126, len(chunks)-int(i)*126)
		p32(cib[36:], uint32(n))
		for j := 0; j < n; j++ {
			chunk := &chunks[int(i)*126+j]
			var allocated uint32
			for _, b := range chunk.bits {
				allocated += uint32(bits.OnesCount8(b))
			}
			// Bits beyond a short final chunk are reserved/zero in project
			// images; reject unknown encodings rather than guessing at totals.
			repairCheck(allocated <= chunk.count, "noncanonical last-chunk bitmap")
			ci := cib[40+j*32:]
			p64(ci, xid)
			p64(ci[8:], chunk.start)
			p32(ci[16:], chunk.count)
			p32(ci[20:], chunk.count-allocated)
			newFree += uint64(chunk.count - allocated)
			if allocated != 0 {
				addr := allocateIP()
				p64(ci[24:], addr)
				write(addr, chunk.bits)
			}
		}
		objectChecksum(cib)
		write(cibAddr, cib)
		p64(sm[uint64(addrOff)+8*uint64(i):], cibAddr)
	}
	p64(sm[72:], newFree)
	p64(sm[0xa8:], newBMBase)
	p16(sm[indexOff:], 0)
	p64(sm[xidOff:], xid)
	p16(sm[0x140:], 1)
	p16(sm[0x142:], uint16(bmCount-1))
	for i := uint32(0); i < bmCount; i++ {
		next := uint16(i + 1)
		if i == 0 || i == bmCount-1 {
			next = 0xffff
		}
		p16(sm[nextOff+2*i:], next)
	}
	write(newBMBase, newIP)
	next := dataBase
	for i := range objects {
		obj := &objects[i]
		p64(obj.raw[16:], xid)
		objectChecksum(obj.raw)
		write(next, obj.raw)
		p64(cp[40+i*40+32:], next)
		next += uint64(len(obj.raw) / 4096)
	}
	// As in the COW writer, keep a predecessor descriptor for native APFS's
	// raised-XID mount path; the newest checkpoint alone owns the live data.
	p32(sb[104:], descBlocks)
	p32(sb[108:], dataBlocks)
	p64(sb[112:], start)
	p64(sb[120:], dataBase)
	p32(sb[132:], total)
	p32(sb[140:], 2)
	p32(sb[144:], 0)
	p32(sb[148:], total)
	for i := uint64(0); i < 2; i++ {
		cxid, index := xid-1+i, uint32(2*i)
		p64(cp[8:], start+uint64(index))
		p64(cp[16:], cxid)
		objectChecksum(cp)
		write(start+uint64(index), cp)
		p64(sb[16:], cxid)
		p64(sb[96:], cxid+1)
		p32(sb[128:], index+2)
		p32(sb[136:], index)
		objectChecksum(sb)
		write(start+uint64(index)+1, sb)
	}
	write(0, sb)
	// Independently check each write against the ORIGINAL allocation maps.
	// Newly released old metadata is never eligible for a precommit write.
	keys := make([]uint64, 0, len(delta.blocks)-1)
	for addr := range delta.blocks {
		if addr == 0 {
			continue
		}
		repairCheck(wasFree(addr) || allocatedIP[addr], "plan overwrites an old live block")
		keys = append(keys, addr)
	}
	sort.Slice(keys, func(i, j int) bool { return keys[i] < keys[j] })
	ranges := []checkpointWriteRange{}
	for _, addr := range keys {
		if len(ranges) > 0 && ranges[len(ranges)-1].Start+ranges[len(ranges)-1].Count == addr {
			ranges[len(ranges)-1].Count++
		} else {
			ranges = append(ranges, checkpointWriteRange{addr, 1})
		}
	}
	actual := must(apfs.Open(delta, nil))
	defer actual.Close()
	repairCheck(actual.Superblock.XID == xid && actual.Superblock.OmapOID == s.OmapOID && actual.Superblock.UUID == s.UUID,
		"repaired checkpoint identity mismatch")
	_, free := freeTail(actual, delta)
	repairCheck(free == newFree, "repaired allocation totals disagree")
	fingerprint := sha256.Sum256(primary)
	sourceKeys := make([]uint64, 0, len(sourceBlocks))
	for block := range sourceBlocks {
		sourceKeys = append(sourceKeys, block)
	}
	sort.Slice(sourceKeys, func(i, j int) bool { return sourceKeys[i] < sourceKeys[j] })
	structures := make([]checkpointSourceBlock, 0, len(sourceKeys))
	identity := sha256.New()
	for _, block := range sourceKeys {
		var key [8]byte
		p64(key[:], block)
		must(identity.Write(key[:]))
		must(identity.Write(must(hex.DecodeString(sourceBlocks[block]))))
		structures = append(structures, checkpointSourceBlock{block, sourceBlocks[block]})
	}
	report := map[string]any{"schema": 1, "repair": "checkpoint-area", "container_bytes": size,
		"source_superblock_sha256": hex.EncodeToString(fingerprint[:]), "old_transaction_id": s.XID,
		"transaction_id": xid, "old_descriptor_blocks": s.XPDescBlocks, "old_data_blocks": s.XPDataBlocks,
		"descriptor_blocks": descBlocks, "data_blocks": dataBlocks, "checkpoint_start_block": start,
		"old_free_bytes": oldFree * 4096, "new_free_bytes": newFree * 4096, "changed_blocks": len(delta.blocks),
		"metadata_start_block": keys[0], "write_ranges": ranges, "volumes": identities,
		"source_identity_kind": "apfs-checkpoint-structures-sha256", "source_structures": structures,
		"source_structure_sha256": hex.EncodeToString(identity.Sum(nil)), "source_structure_bytes": len(structures) * 4096,
		"commit_block": 0, "volume_trees_preserved": true, "device_written": false}
	return delta, report
}

func checkpointPlan(args []string) {
	flags := flag.NewFlagSet("checkpoint-plan", flag.ExitOnError)
	source := flags.String("source", "", "offline project container (read only)")
	offset := flags.Int64("source-offset", 0, "container byte offset")
	size := flags.Int64("source-size", 0, "container byte length; required for devices")
	output := flags.String("output", "", "checkpoint repair block delta")
	flags.Parse(args)
	if *source == "" || *output == "" {
		panic("checkpoint-plan requires source and output")
	}
	f := must(os.Open(*source))
	defer f.Close()
	info := must(f.Stat())
	if *size == 0 && info.Mode().IsRegular() {
		*size = info.Size() - *offset
	}
	repairCheck(*offset >= 0 && *offset%4096 == 0 && *size >= 4096 && *size%4096 == 0 &&
		*offset <= int64(^uint64(0)>>1)-*size && (!info.Mode().IsRegular() || *offset+*size <= info.Size()), "invalid source range")
	reader := io.NewSectionReader(f, *offset, *size)
	delta, report := checkpointRepair(reader, *size)
	report["source"], report["source_offset"] = *source, *offset
	delta.save(*output)
	report["delta_sha256"] = hash(*output)
	report["patch_bytes"] = 24 + len(delta.blocks)*4104
	writeJSON(*output+".json", report)
	fmt.Printf("Checkpoint repair: %d blocks; descriptor/data reserve %v/%v blocks; volume trees retained\n",
		len(delta.blocks), report["descriptor_blocks"], report["data_blocks"])
}
