package main

import (
	"bytes"
	"encoding/binary"
	"fmt"
	"os"
	"os/exec"
	"path/filepath"
	"strings"
	"testing"

	"github.com/deploymenttheory/go-apfs-v2/pkg/apfs"
	"github.com/deploymenttheory/go-apfs-v2/pkg/apfswrite"
)

func checkAPFS(t *testing.T, path string, allowCrash ...bool) {
	t.Helper()
	checker := filepath.Join("..", "src", "apfsprogs", "apfsck", "apfsck")
	args := []string{"-c", "-w", path}
	if len(allowCrash) > 0 && allowCrash[0] {
		args = []string{"-w", path}
	}
	if out, err := exec.Command(checker, args...).CombinedOutput(); err != nil {
		t.Fatalf("independent apfsck: %v: %s", err, out)
	}
}

// Start with a real, independently checked image, then reproduce the former
// eight-block reserves by releasing their spare blocks in the allocation maps.
// A stale primary (39) and newer descriptor (41) model the hardware crash.
func legacyCheckpointFixture(t *testing.T) (*os.File, string) {
	t.Helper()
	path := filepath.Join(t.TempDir(), "legacy.apfs")
	f := must(os.Create(path))
	t.Cleanup(func() { f.Close() })
	root := &apfswrite.Entry{Mode: os.ModeDir | 0755, Children: []*apfswrite.Entry{
		{Name: "retained", Mode: 0600, UID: 501, Data: bytes.Repeat([]byte("original bytes"), 2000),
			Xattrs: map[string][]byte{"user.attr": []byte("opaque value")}},
	}}
	must(true, apfswrite.CreateContainer(f, 1<<30, &apfswrite.CreateOptions{Volumes: []apfswrite.VolumeSpec{
		{Name: "System", Role: apfs.VolumeRoleSystem, CaseSensitive: true, Root: root},
		{Name: "Data", Role: apfs.VolumeRoleData, CaseSensitive: true, Root: root},
	}}))
	checkAPFS(t, path)
	c := must(apfs.Open(f, nil))
	defer c.Close()
	s := c.Superblock
	read := func(addr uint64) []byte {
		raw := make([]byte, 4096)
		must(f.ReadAt(raw, int64(addr)*4096))
		return raw
	}
	write := func(addr uint64, raw []byte) { must(f.WriteAt(raw, int64(addr)*4096)) }
	u32, u64 := binary.LittleEndian.Uint32, binary.LittleEndian.Uint64
	p32, p64 := binary.LittleEndian.PutUint32, binary.LittleEndian.PutUint64
	cp := read(s.XPDescBase)
	smAddr := must(c.CheckpointMap.PhysicalAddressByObjectIdentifier(s.SpacemanOID))
	sm := read(smAddr)
	addrOff := u32(sm[80:])
	cibAddr := u64(sm[addrOff:])
	cib := read(cibAddr)
	bitmapAddr := u64(cib[64:]) // first chunk's ci_bitmap_addr
	bitmap := read(bitmapAddr)
	freed := uint64(0)
	for _, span := range [][2]uint64{{s.XPDescBase + 8, uint64(s.XPDescBlocks - 8)}, {s.XPDataBase + 8, uint64(s.XPDataBlocks - 8)}} {
		for addr := span[0]; addr < span[0]+span[1]; addr++ {
			if addr >= 32768 || bitmap[addr/8]&(1<<(addr%8)) == 0 {
				t.Fatal("fixture reserve is outside first allocated chunk")
			}
			bitmap[addr/8] &^= 1 << (addr % 8)
			freed++
		}
	}
	write(bitmapAddr, bitmap)
	p32(cib[60:], u32(cib[60:])+uint32(freed)) // first chunk's ci_free_count
	objectChecksum(cib)
	write(cibAddr, cib)
	p64(sm[72:], u64(sm[72:])+freed)
	write(smAddr, sm)
	for i := uint32(0); i < u32(cp[36:]); i++ {
		addr := u64(cp[40+i*40+32:])
		raw := read(addr)
		p64(raw[16:], 41)
		objectChecksum(raw)
		write(addr, raw)
	}
	p64(cp[16:], 41)
	objectChecksum(cp)
	write(s.XPDescBase, cp)
	sb := read(s.XPDescBase + 1)
	p64(sb[16:], 41)
	p64(sb[96:], 42)
	p32(sb[104:], 8)
	p32(sb[108:], 8)
	objectChecksum(sb)
	write(s.XPDescBase+1, sb)
	p64(sb[16:], 39)
	objectChecksum(sb)
	write(0, sb)
	must(true, f.Sync())
	checkAPFS(t, path, true)
	return f, path
}

func TestCheckpointRepairPreservesNativeStateAndOldCheckpointUntilCommit(t *testing.T) {
	f, path := legacyCheckpointFixture(t)
	before := must(apfs.Open(f, nil))
	if before.Superblock.XID != 41 {
		t.Fatal("did not select descriptor newer than primary")
	}
	sys := must(before.VolumeBySelector("system"))
	oldData := must(sys.ReadFile("retained"))
	oldTree := must(sys.FileEntryByPath("retained"))
	oldExtent := must(oldTree.RawDataStream()).PhysicalExtents[0].PhysicalBlockNumber
	delta, report := checkpointRepair(f, 1<<30)
	if report["old_transaction_id"] != uint64(41) || report["transaction_id"] != uint64(42) || report["data_blocks"].(uint32) < 1024 {
		t.Fatal(report)
	}
	primary := make([]byte, 4096)
	must(f.ReadAt(primary, 0))
	for addr, raw := range delta.blocks {
		if addr != 0 {
			must(f.WriteAt(raw, int64(addr)*4096))
		}
	}
	must(true, f.Sync())
	still := make([]byte, 4096)
	must(f.ReadAt(still, 0))
	if !bytes.Equal(still, primary) {
		t.Fatal("primary changed before commit")
	}
	checkAPFS(t, path, true)
	oldView := must(apfs.Open(f, nil))
	if oldView.Superblock.XID != 41 || !bytes.Equal(must(must(oldView.VolumeBySelector("system")).ReadFile("retained")), oldData) {
		t.Fatal("precommit writes damaged old state")
	}
	oldView.Close()
	must(f.WriteAt(delta.blocks[0], 0))
	must(true, f.Sync())
	checkAPFS(t, path)
	actual := must(apfs.Open(f, nil))
	defer actual.Close()
	if actual.Superblock.XID != 42 || actual.Superblock.OmapOID != before.Superblock.OmapOID || actual.Superblock.UUID != before.Superblock.UUID {
		t.Fatal("container identity or volume mapping changed")
	}
	for _, role := range []string{"system", "data"} {
		v := must(actual.VolumeBySelector(role))
		e := must(v.FileEntryByPath("retained"))
		if !bytes.Equal(must(v.ReadFile("retained")), oldData) || must(e.OwnerIdentifier()) != 501 {
			t.Fatal("file bytes or ownership changed")
		}
		if role == "system" && must(e.RawDataStream()).PhysicalExtents[0].PhysicalBlockNumber != oldExtent {
			t.Fatal("repair relocated file payload")
		}
	}
	_, free := freeTail(actual, f)
	if free*4096 != report["new_free_bytes"] {
		t.Fatal("free-space report differs from the committed allocation bitmap")
	}
	before.Close()
}

func TestCheckpointRepairRejectsCorruptAllocationMetadata(t *testing.T) {
	f, _ := legacyCheckpointFixture(t)
	c := must(apfs.Open(f, nil))
	addr := must(c.CheckpointMap.PhysicalAddressByObjectIdentifier(c.Superblock.SpacemanOID))
	c.Close()
	must(f.WriteAt([]byte{0xff}, int64(addr)*4096+100))
	defer func() {
		if r := recover(); r == nil || !strings.Contains(r.(string), "checksum") {
			t.Fatalf("expected allocation checksum rejection, got %v", r)
		}
	}()
	checkpointRepair(f, 1<<30)
}

func TestWritableCheckpointReserveSurvivesFormatting(t *testing.T) {
	for _, size := range []int64{512 << 10, 8 << 20, 1 << 30, 12020 << 20} {
		t.Run(fmt.Sprintf("%dKiB", size>>10), func(t *testing.T) {
			f := must(os.Create(filepath.Join(t.TempDir(), "reserved.apfs")))
			defer f.Close()
			must(true, apfswrite.CreateContainer(f, size, nil))
			c := must(apfs.Open(f, nil))
			defer c.Close()
			s := c.Superblock
			// Regression for the hardware failure: a stable four-block
			// transaction must leave room for a five-block successor.
			if s.XPDataBlocks-s.XPDataLen < s.XPDataLen+1 || s.XPDescBlocks < 8 {
				t.Fatal("cannot commit a growing successor checkpoint")
			}
			checkAPFS(t, f.Name())
		})
	}
}
