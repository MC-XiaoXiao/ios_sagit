package main

import (
	"bytes"
	"crypto/sha256"
	"encoding/hex"
	"encoding/json"
	"os"
	"path/filepath"
	"testing"

	"github.com/deploymenttheory/go-apfs-v2/pkg/apfs"
	"github.com/deploymenttheory/go-apfs-v2/pkg/apfswrite"
)

func TestSystemBlockPatchRetainsUnchangedExtentsAndOldCheckpoint(t *testing.T) {
	d := t.TempDir()
	path := filepath.Join(d, "native.apfs")
	f := must(os.Create(path))
	defer f.Close()
	before := bytes.Repeat([]byte("0123456789abcdef"), 32768)
	after := bytes.Clone(before)
	indices := []uint64{0, 1, 64, 127}
	raw := make([]byte, 24)
	copy(raw, "APFSPAT1")
	np64(raw[8:], uint64(len(before)))
	np64(raw[16:], uint64(len(indices)))
	for _, index := range indices {
		block := make([]byte, 8200)
		np64(block, index)
		copy(block[8:], before[index*4096:(index+1)*4096])
		copy(after[index*4096:(index+1)*4096], bytes.Repeat([]byte{byte(index + 50)}, 4096))
		copy(block[4104:], after[index*4096:(index+1)*4096])
		raw = append(raw, block...)
	}
	root := &apfswrite.Entry{Mode: os.ModeDir | 0755, Children: []*apfswrite.Entry{
		{Name: "private", Mode: os.ModeDir | 0755, Children: []*apfswrite.Entry{{Name: "etc", Mode: os.ModeDir | 0755, Children: []*apfswrite.Entry{{Name: "fstab", Mode: 0644, Data: []byte("# retained\n")}}}}},
		{Name: "image.dmg", Mode: 0444, Data: before},
	}}
	must(true, apfswrite.CreateContainer(f, 1<<30, &apfswrite.CreateOptions{Volumes: []apfswrite.VolumeSpec{
		{Name: "System", Role: apfs.VolumeRoleSystem, CaseSensitive: true, Root: root},
		{Name: "Data", Role: apfs.VolumeRoleData, CaseSensitive: true, Root: root},
	}}))
	checkAPFS(t, path)
	sourceHash, derivedHash := sha256.Sum256(before), sha256.Sum256(after)
	descriptor := nativeFilePatch{Path: "image.dmg", Delta: "patch.bin", SourceSHA256: hex.EncodeToString(sourceHash[:]), DerivedSHA256: hex.EncodeToString(derivedHash[:])}
	must(true, os.WriteFile(filepath.Join(d, "patch.bin"), raw, 0600))
	manifest := filepath.Join(d, "patch.json")
	must(true, os.WriteFile(manifest, must(json.Marshal(descriptor)), 0600))
	for _, kind := range []string{"source-hash", "before-bytes"} {
		t.Run(kind, func(t *testing.T) {
			saved := bytes.Clone(raw)
			bad := descriptor
			if kind == "source-hash" {
				bad.SourceSHA256 = string(bytes.Repeat([]byte("0"), 64))
			} else {
				raw[32] ^= 1
				must(true, os.WriteFile(filepath.Join(d, "patch.bin"), raw, 0600))
			}
			must(true, os.WriteFile(manifest, must(json.Marshal(bad)), 0600))
			n := newNativeEditor(f, 1<<30)
			defer n.container.Close()
			func() {
				defer func() {
					if recover() == nil {
						t.Error("accepted incompatible source")
					}
				}()
				n.applySystem("", manifest)
			}()
			raw = saved
			must(true, os.WriteFile(filepath.Join(d, "patch.bin"), raw, 0600))
			must(true, os.WriteFile(manifest, must(json.Marshal(descriptor)), 0600))
		})
	}
	n := newNativeEditor(f, 1<<30)
	defer n.container.Close()
	n.applySystem("", manifest)
	if len(n.delta.blocks) > 100 {
		t.Fatalf("copied unchanged file blocks: %d", len(n.delta.blocks))
	}
	for p, b := range n.delta.blocks {
		if p != 0 {
			must(f.WriteAt(b, int64(p)*4096))
		}
	}
	must(true, f.Sync())
	checkAPFS(t, path)
	old := must(apfs.Open(f, nil))
	if !bytes.Equal(must(must(old.VolumeBySelector("system")).ReadFile("image.dmg")), before) {
		t.Fatal("old checkpoint file changed")
	}
	old.Close()
	must(f.WriteAt(n.delta.blocks[0], 0))
	must(true, f.Sync())
	checkAPFS(t, path)
	actual := must(apfs.Open(f, nil))
	defer actual.Close()
	if !bytes.Equal(must(must(actual.VolumeBySelector("system")).ReadFile("image.dmg")), after) {
		t.Fatal("new System file mismatch")
	}
	if !bytes.Equal(must(must(actual.VolumeBySelector("data")).ReadFile("image.dmg")), before) {
		t.Fatal("Data file changed")
	}
	// Patch again across the boundary between a copied extent and retained data.
	final := bytes.Clone(after)
	raw = make([]byte, 24)
	copy(raw, "APFSPAT1")
	np64(raw[8:], uint64(len(after)))
	np64(raw[16:], 2)
	for _, index := range []uint64{1, 2} {
		block := make([]byte, 8200)
		np64(block, index)
		copy(block[8:], after[index*4096:(index+1)*4096])
		copy(final[index*4096:(index+1)*4096], bytes.Repeat([]byte{200}, 4096))
		copy(block[4104:], final[index*4096:(index+1)*4096])
		raw = append(raw, block...)
	}
	finalHash := sha256.Sum256(final)
	descriptor.SourceSHA256 = descriptor.DerivedSHA256
	descriptor.DerivedSHA256 = hex.EncodeToString(finalHash[:])
	must(true, os.WriteFile(filepath.Join(d, "patch.bin"), raw, 0600))
	must(true, os.WriteFile(manifest, must(json.Marshal(descriptor)), 0600))
	second := newNativeEditor(f, 1<<30)
	defer second.container.Close()
	second.applySystem("", manifest)
	for p, b := range second.delta.blocks {
		if p != 0 {
			must(f.WriteAt(b, int64(p)*4096))
		}
	}
	must(true, f.Sync())
	checkAPFS(t, path)
	stillOld := must(apfs.Open(f, nil))
	if !bytes.Equal(must(must(stillOld.VolumeBySelector("system")).ReadFile("image.dmg")), after) {
		t.Fatal("repeat patch changed precommit data")
	}
	stillOld.Close()
	must(f.WriteAt(second.delta.blocks[0], 0))
	must(true, f.Sync())
	checkAPFS(t, path)
	last := must(apfs.Open(f, nil))
	defer last.Close()
	if !bytes.Equal(must(must(last.VolumeBySelector("system")).ReadFile("image.dmg")), final) {
		t.Fatal("repeat patch mismatch")
	}
	if !bytes.Equal(must(must(last.VolumeBySelector("data")).ReadFile("image.dmg")), before) {
		t.Fatal("repeat patch changed Data")
	}
}
