package main

import (
	"bytes"
	"compress/zlib"
	"fmt"
	"github.com/deploymenttheory/go-apfs-v2/pkg/apfs"
	"github.com/deploymenttheory/go-apfs-v2/pkg/apfswrite"
	"os"
	"os/exec"
	"path/filepath"
	"strings"
	"testing"
)

func TestSystemPathCopyPreservesOtherVolumesAndPrecommit(t *testing.T) {
	for _, count := range []int{2, 450, 1800} {
		t.Run(fmt.Sprint(count), func(t *testing.T) {
			d := t.TempDir()
			path := filepath.Join(d, "native.apfs")
			f := must(os.Create(path))
			defer f.Close()
			compressed := bytes.NewBuffer(nil)
			zw := zlib.NewWriter(compressed)
			must(zw.Write([]byte("original compressed config")))
			must(true, zw.Close())
			header := make([]byte, 16)
			copy(header, "fpmc")
			np32(header[4:], 3)
			np64(header[8:], uint64(len("original compressed config")))
			root := &apfswrite.Entry{Mode: os.ModeDir | 0755, Children: []*apfswrite.Entry{
				{Name: "private", Mode: os.ModeDir | 0755, Children: []*apfswrite.Entry{{Name: "etc", Mode: os.ModeDir | 0755, Children: []*apfswrite.Entry{{Name: "fstab", Mode: 0644, Data: []byte("# preserved\n")}}}}},
				{Name: "wallpapers", Mode: os.ModeDir | 0755, Children: []*apfswrite.Entry{{Name: "existing", Mode: 0644, Data: []byte("original config")}, {Name: "compressed", Mode: 0644, Xattrs: map[string][]byte{"com.apple.decmpfs": append(header, compressed.Bytes()...), "user.retained": []byte("keep attr")}}}},
				{Name: "retained", Mode: 0600, UID: 501, Data: bytes.Repeat([]byte("untouched"), 1000), Xattrs: map[string][]byte{"user.attr": []byte("opaque")}},
			}}
			must(true, apfswrite.CreateContainer(f, 1<<30, &apfswrite.CreateOptions{Volumes: []apfswrite.VolumeSpec{
				{Name: "System", Role: apfs.VolumeRoleSystem, CaseSensitive: true, Root: root},
				{Name: "Data", Role: apfs.VolumeRoleData, CaseSensitive: true, Root: root},
			}}))
			checkAPFS(t, path)
			overlay := filepath.Join(d, "overlay")
			must(true, os.MkdirAll(filepath.Join(overlay, "wallpapers"), 0755))
			must(true, os.WriteFile(filepath.Join(overlay, "wallpapers/existing"), []byte("replacement wallpaper configuration"), 0644))
			must(true, os.WriteFile(filepath.Join(overlay, "wallpapers/compressed"), []byte("replacement plain config"), 0644))
			for i := 0; i < count; i++ {
				must(true, os.WriteFile(filepath.Join(overlay, fmt.Sprintf("wallpapers/new-%04d", i)), bytes.Repeat([]byte("new config"), i%50+1), 0644))
			}
			editor := newNativeEditor(f, 1<<30)
			defer editor.container.Close()
			report := editor.applySystem(overlay)
			if report["non_system_volume_trees_preserved"] != true {
				t.Fatal(report)
			}
			for p, b := range editor.delta.blocks {
				if p != 0 {
					must(f.WriteAt(b, int64(p)*4096))
				}
			}
			must(true, f.Sync())
			checkAPFS(t, path)
			old := must(apfs.Open(f, nil))
			if !bytes.Equal(must(must(old.VolumeBySelector("system")).ReadFile("wallpapers/existing")), []byte("original config")) {
				t.Fatal("old checkpoint changed before block zero")
			}
			old.Close()
			must(f.WriteAt(editor.delta.blocks[0], 0))
			must(true, f.Sync())
			checkAPFS(t, path)
			actual := must(apfs.Open(f, nil))
			defer actual.Close()
			sys := must(actual.VolumeBySelector("system"))
			data := must(actual.VolumeBySelector("data"))
			e := must(sys.FileEntryByPath("wallpapers/compressed"))
			if must(e.BSDFlags())&0x20 != 0 || must(e.HasExtendedAttributeByName("com.apple.decmpfs")) || !must(e.HasExtendedAttributeByName("user.retained")) || string(must(sys.ReadFile("wallpapers/compressed"))) != "replacement plain config" {
				t.Fatal("compressed replacement retained stale attributes or damaged unrelated metadata")
			}
			if string(must(data.ReadFile("wallpapers/compressed"))) != "original compressed config" {
				t.Fatal("Data compression changed")
			}
			if !bytes.Equal(must(data.ReadFile("wallpapers/existing")), []byte("original config")) {
				t.Fatal("Data changed")
			}
			for i := 0; i < count; i++ {
				if !bytes.Equal(must(sys.ReadFile(fmt.Sprintf("wallpapers/new-%04d", i))), bytes.Repeat([]byte("new config"), i%50+1)) {
					t.Fatal("missing new config")
				}
			}
			// Repeat from the committed native layout. Replacing every stream
			// deletes old extent records, collapses nodes and reuses free holes.
			second := newNativeEditor(f, 1<<30)
			defer second.container.Close()
			second.applySystem(overlay)
			for p, b := range second.delta.blocks {
				if p != 0 {
					must(f.WriteAt(b, int64(p)*4096))
				}
			}
			must(true, f.Sync())
			checkAPFS(t, path)
			must(f.WriteAt(second.delta.blocks[0], 0))
			must(true, f.Sync())
			checkAPFS(t, path)
		})
	}
}

func TestIndependentCheckerNativePurgeableEncoding(t *testing.T) {
	path := filepath.Join(t.TempDir(), "purgeable.apfs")
	f := must(os.Create(path))
	defer f.Close()
	root := &apfswrite.Entry{Mode: os.ModeDir | 0755, Children: []*apfswrite.Entry{{Name: "cached", Mode: 0644, UID: 501, Data: []byte("cache")}}}
	must(true, apfswrite.CreateContainer(f, 1<<30, &apfswrite.CreateOptions{Volumes: []apfswrite.VolumeSpec{{Name: "Data", Role: apfs.VolumeRoleData, CaseSensitive: true, Root: root}}}))
	checkAPFS(t, path)
	c := must(apfs.Open(f, nil))
	defer c.Close()
	v := must(c.VolumeBySelector("data"))
	e := must(v.FileEntryByPath("cached"))
	id := must(e.Identifier())
	oid := v.Superblock.RootTreeOID
	p := must(v.ObjectMapBTree.PhysicalAddressForOID(f, oid, v.Superblock.XID))
	read := func(p uint64) []byte { b := make([]byte, 4096); must(f.ReadAt(b, int64(p)*4096)); return b }
	write := func(p uint64, b []byte) { must(f.WriteAt(b, int64(p)*4096)) }
	// A tiny single-leaf fixture may be re-encoded in place: this is test
	// construction only; the production editor always allocates fresh blocks.
	tree := newNativeTree(read, func(uint64) uint64 { return p }, func(uint64, uint64) {}, write, oid, true, map[uint64]uint64{oid: p}, c.Superblock.XID, nativeFSCompare)
	if tree.root.level != 0 {
		t.Fatal("fixture must fit one leaf")
	}
	inode := tree.required(nativeKey(id, 3))
	np64(inode[48:], nu64(inode[48:])|0x80000)
	tree.put(nativeKey(id, 3), inode)
	name := fmt.Sprintf("0x1000-0x%x:501", id)
	key := nativeDrecKey(7, name)
	val := make([]byte, 18)
	np64(val, id)
	np16(val[16:], 0xc004)
	tree.put(key, val)
	tree.flush()
	must(true, f.Sync())
	checkAPFS(t, path)
	// An eligible file need not be present in the synthetic purgeable index.
	tree.put(key, nil)
	tree.flush()
	must(true, f.Sync())
	checkAPFS(t, path)
	tree.put(key, val)
	// Only the observed encoding is supported, not arbitrary high flag bits.
	np16(val[16:], 0xc008)
	tree.put(key, val)
	tree.flush()
	must(true, f.Sync())
	checker := filepath.Join("..", "src", "apfsprogs", "apfsck", "apfsck")
	out, err := exec.Command(checker, "-c", "-w", path).CombinedOutput()
	if err == nil || !strings.Contains(string(out), "reserved flags") {
		t.Fatalf("unexpected marker accepted: %v %s", err, out)
	}
	np16(val[16:], 0xc004)
	tree.put(key, val)
	inode = tree.required(nativeKey(id, 3))
	np64(inode[48:], nu64(inode[48:])&^0x80000)
	tree.put(nativeKey(id, 3), inode)
	tree.flush()
	must(true, f.Sync())
	out, err = exec.Command(checker, "-c", "-w", path).CombinedOutput()
	if err == nil || !strings.Contains(string(out), "wrong purgeability flag") {
		t.Fatalf("non-purgeable index member accepted: %v %s", err, out)
	}
}

func TestIndependentCheckerInheritedFileDirectoryStats(t *testing.T) {
	path := filepath.Join(t.TempDir(), "dirstats.apfs")
	f := must(os.Create(path))
	defer f.Close()
	root := &apfswrite.Entry{Mode: os.ModeDir | 0755, Children: []*apfswrite.Entry{{Name: "Caches", Mode: os.ModeDir | 0755, Children: []*apfswrite.Entry{{Name: "cached", Mode: 0644, LinkGroup: 42, Data: []byte("cache")}, {Name: "alias", Mode: 0644, LinkGroup: 42, Data: []byte("cache")}}}}}
	must(true, apfswrite.CreateContainer(f, 1<<30, &apfswrite.CreateOptions{Volumes: []apfswrite.VolumeSpec{{Name: "Data", Role: apfs.VolumeRoleData, CaseSensitive: true, Root: root}}}))
	checkAPFS(t, path)
	c := must(apfs.Open(f, nil))
	defer c.Close()
	v := must(c.VolumeBySelector("data"))
	dir := must(v.FileEntryByPath("Caches"))
	file := must(v.FileEntryByPath("Caches/cached"))
	id := must(dir.Identifier())
	oid := v.Superblock.RootTreeOID
	p := must(v.ObjectMapBTree.PhysicalAddressForOID(f, oid, v.Superblock.XID))
	read := func(p uint64) []byte { b := make([]byte, 4096); must(f.ReadAt(b, int64(p)*4096)); return b }
	write := func(p uint64, b []byte) { must(f.WriteAt(b, int64(p)*4096)) }
	tree := newNativeTree(read, func(uint64) uint64 { return p }, func(uint64, uint64) {}, write, oid, true, map[uint64]uint64{oid: p}, c.Superblock.XID, nativeFSCompare)
	if tree.root.level != 0 {
		t.Fatal("single leaf required")
	}
	addStats := func(ino uint64, flags uint64) {
		b := tree.required(nativeKey(ino, 3))
		count := int(nu16(b[92:]))
		data := bytes.Clone(b[96+4*count:])
		headers := bytes.Clone(b[96 : 96+4*count])
		out := bytes.Clone(b[:96])
		np64(out[48:], nu64(out[48:])|flags)
		np16(out[92:], uint16(count+1))
		np16(out[94:], nu16(out[94:])+8)
		headers = append(headers, 10, 0x22, 8, 0)
		out = append(out, headers...)
		out = append(out, data...)
		key := make([]byte, 8)
		np64(key, id)
		out = append(out, key...)
		tree.put(nativeKey(ino, 3), out)
	}
	addStats(id, 6)
	addStats(must(file.Identifier()), 2|0x10)
	// A clone-created file need not have subsequently been cloned itself.
	// Mark the epoch current so the former implication check would reject it.
	vp := must(c.ObjectMapBTree.PhysicalAddressForOID(f, v.Superblock.OID, c.Superblock.XID))
	vb := read(vp)
	np64(vb[992:], c.Superblock.XID)
	objectChecksum(vb)
	write(vp, vb)
	stats := make([]byte, 32)
	np64(stats, 2)
	np64(stats[8:], 8192)
	tree.put(nativeKey(id, 10), stats)
	tree.flush()
	must(true, f.Sync())
	checkAPFS(t, path)
	// The expected byte count remains independently checked.
	np64(stats[8:], 8193)
	tree.put(nativeKey(id, 10), stats)
	tree.flush()
	must(true, f.Sync())
	checker := filepath.Join("..", "src", "apfsprogs", "apfsck", "apfsck")
	out, err := exec.Command(checker, "-c", "-w", path).CombinedOutput()
	if err == nil || !strings.Contains(string(out), "wrong total size") {
		t.Fatalf("bad statistics accepted: %v %s", err, out)
	}
}

func TestIndependentCheckerSeparatePrivateStreamID(t *testing.T) {
	path := filepath.Join(t.TempDir(), "private-stream.apfs")
	f := must(os.Create(path))
	defer f.Close()
	root := &apfswrite.Entry{Mode: os.ModeDir | 0755, Children: []*apfswrite.Entry{{Name: "cached", Mode: 0644, Data: []byte("cache")}}}
	must(true, apfswrite.CreateContainer(f, 1<<30, &apfswrite.CreateOptions{Volumes: []apfswrite.VolumeSpec{{Name: "Data", Role: apfs.VolumeRoleData, CaseSensitive: true, Root: root}}}))
	checkAPFS(t, path)
	c := must(apfs.Open(f, nil))
	defer c.Close()
	v := must(c.VolumeBySelector("data"))
	e := must(v.FileEntryByPath("cached"))
	id := must(e.Identifier())
	private := v.Superblock.NextObjID
	oid := v.Superblock.RootTreeOID
	p := must(v.ObjectMapBTree.PhysicalAddressForOID(f, oid, v.Superblock.XID))
	read := func(p uint64) []byte { b := make([]byte, 4096); must(f.ReadAt(b, int64(p)*4096)); return b }
	write := func(p uint64, b []byte) { must(f.WriteAt(b, int64(p)*4096)) }
	tree := newNativeTree(read, func(uint64) uint64 { return p }, func(uint64, uint64) {}, write, oid, true, map[uint64]uint64{oid: p}, c.Superblock.XID, nativeFSCompare)
	if tree.root.level != 0 {
		t.Fatal("single leaf required")
	}
	inode := tree.required(nativeKey(id, 3))
	np64(inode[8:], private)
	tree.put(nativeKey(id, 3), inode)
	for _, key := range [][]byte{nativeKey(id, 6), nativeKey(id, 8, 0)} {
		value := tree.required(key)
		tree.put(key, nil)
		replacement := bytes.Clone(key)
		np64(replacement, private|nu64(key)&^nativeMask)
		tree.put(replacement, value)
	}
	tree.flush()
	ep := v.Superblock.ExtentrefTreeOID
	ext := newNativeTree(read, func(uint64) uint64 { return ep }, func(uint64, uint64) {}, write, ep, false, nil, c.Superblock.XID, nativeIDCompare)
	if ext.root.level != 0 {
		t.Fatal("single leaf required")
	}
	block := must(e.RawDataStream()).PhysicalExtents[0].PhysicalBlockNumber
	ref := ext.required(nativeKey(block, 2))
	np64(ref[8:], private)
	ext.put(nativeKey(block, 2), ref)
	ext.flush()
	vp := must(c.ObjectMapBTree.PhysicalAddressForOID(f, v.Superblock.OID, c.Superblock.XID))
	vb := read(vp)
	np64(vb[176:], private+1)
	objectChecksum(vb)
	write(vp, vb)
	must(true, f.Sync())
	checkAPFS(t, path)
}
