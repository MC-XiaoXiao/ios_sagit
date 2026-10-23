package main

// Same-size stream edits retain unchanged data extents. All changed data and
// metadata are copied to original free space before publishing a checkpoint.
import (
	"bytes"
	"crypto/sha256"
	"encoding/hex"
	"encoding/json"
	"io"
	"os"
	"path/filepath"
	"sort"
	"strings"

	"github.com/deploymenttheory/go-apfs-v2/pkg/apfs"
)

type nativePatchBlock struct {
	index         uint64
	before, after []byte
}
type nativeFilePatch struct {
	Path          string `json:"path"`
	Delta         string `json:"delta"`
	SourceSHA256  string `json:"source_sha256"`
	DerivedSHA256 string `json:"derived_sha256"`
	size          uint64
	blocks        []nativePatchBlock
}
type nativeSystemInput struct {
	path     string
	payload  []byte
	mode     os.FileMode
	patch    *nativeFilePatch
	uid, gid uint32
}

func nativeSystemInputs(overlay string, descriptors []string) []nativeSystemInput {
	inputs := []nativeSystemInput{}
	if overlay != "" {
		must(true, filepath.WalkDir(overlay, func(p string, e os.DirEntry, err error) error {
			if err != nil {
				return err
			}
			if e.IsDir() {
				return nil
			}
			st := must(os.Lstat(p))
			repairCheck(st.Mode().IsRegular(), "System overlay requires regular files")
			repairCheck(st.Size() > 0 && st.Size() <= 16<<20, "System overlay stream size unsupported")
			rel := filepath.ToSlash(must(filepath.Rel(overlay, p)))
			inputs = append(inputs, nativeSystemInput{path: rel, payload: must(os.ReadFile(p)), mode: st.Mode()})
			return nil
		}))
	}
	for _, descriptor := range descriptors {
		if descriptor == "" {
			continue
		}
		var p nativeFilePatch
		must(true, json.Unmarshal(must(os.ReadFile(descriptor)), &p))
		for _, h := range []string{p.SourceSHA256, p.DerivedSHA256} {
			repairCheck(len(must(hex.DecodeString(h))) == 32, "block patch requires full source and derived hashes")
		}
		deltaPath := p.Delta
		if !filepath.IsAbs(deltaPath) {
			deltaPath = filepath.Join(filepath.Dir(descriptor), deltaPath)
		}
		st := must(os.Stat(deltaPath))
		repairCheck(st.Size() >= 24 && st.Size() <= 32<<20, "block patch size unsupported")
		raw := must(os.ReadFile(deltaPath))
		repairCheck(string(raw[:8]) == "APFSPAT1", "invalid same-size block patch")
		p.size = nu64(raw[8:])
		count := nu64(raw[16:])
		repairCheck(p.size > 0 && p.size%4096 == 0 && count > 0 && count <= 4095 && uint64(len(raw)) == 24+count*8200, "invalid block patch geometry")
		for i := uint64(0); i < count; i++ {
			at := 24 + i*8200
			index := nu64(raw[at:])
			before, after := raw[at+8:at+4104], raw[at+4104:at+8200]
			repairCheck(index < p.size/4096 && (i == 0 || index > p.blocks[i-1].index) && !bytes.Equal(before, after), "unsorted, duplicate, unchanged or out-of-bounds file patch block")
			p.blocks = append(p.blocks, nativePatchBlock{index, before, after})
		}
		inputs = append(inputs, nativeSystemInput{path: p.Path, patch: &p})
	}
	sort.Slice(inputs, func(i, j int) bool { return inputs[i].path < inputs[j].path })
	repairCheck(len(inputs) > 0, "empty System update")
	for i, input := range inputs {
		repairCheck(input.path != "." && input.path != "" && !strings.HasPrefix(input.path, "/") && filepath.ToSlash(filepath.Clean(input.path)) == input.path && !strings.HasPrefix(input.path, "../") && !strings.HasPrefix(input.path, "private/var/"), "invalid System update path")
		repairCheck(i == 0 || input.path != inputs[i-1].path, "duplicate System update target")
	}
	return inputs
}

func nativeStreamHash(entry *apfs.FileEntry) string {
	stream := must(entry.RawDataStream())
	h := sha256.New()
	must(io.Copy(h, io.NewSectionReader(entry, 0, int64(stream.Size()))))
	return hex.EncodeToString(h.Sum(nil))
}

func (n *nativeEditor) patchSystemStream(fs, ext *nativeTree, target *apfs.FileEntry, inode []byte, path string, patch *nativeFilePatch) uint64 {
	id := must(target.Identifier())
	stream := must(target.RawDataStream())
	repairCheck(nu32(inode[68:])&0x20 == 0 && !must(target.HasExtendedAttributeByName("com.apple.decmpfs")) && stream.Size() == patch.size && len(stream.PhysicalExtents) > 0, "block patch requires an uncompressed, same-size stream")
	// Hash data without caching every unchanged page in the metadata identity.
	// Every changed source page is separately tracked and checked before apply.
	h := sha256.New()
	covered := uint64(0)
	for _, old := range stream.PhysicalExtents {
		repairCheck(old.LogicalOffset == covered && old.EncryptionIdentifier == 0 && old.PhysicalBlockNumber > 0 && old.DataSize > 0 && old.DataSize%4096 == 0 && old.DataSize <= patch.size-covered, "sparse or unsupported block patch extent")
		ref := ext.required(nativeKey(old.PhysicalBlockNumber, 2))
		repairCheck(len(ref) == 20 && nu64(ref) == old.DataSize/4096|1<<60 && nu64(ref[8:]) == id && nu32(ref[16:]) == 1, "shared or noncanonical block patch extent")
		must(io.Copy(h, io.NewSectionReader(n.source.base, int64(old.PhysicalBlockNumber*4096), int64(old.DataSize))))
		covered += old.DataSize
	}
	repairCheck(covered == patch.size && hex.EncodeToString(h.Sum(nil)) == patch.SourceSHA256, "block patch source file hash mismatch: "+path)
	for _, b := range patch.blocks {
		found := false
		for _, old := range stream.PhysicalExtents {
			if b.index*4096 >= old.LogicalOffset && b.index*4096 < old.LogicalOffset+old.DataSize {
				repairCheck(bytes.Equal(n.read(old.PhysicalBlockNumber+b.index-old.LogicalOffset/4096), b.before), "block patch original data mismatch")
				found = true
				break
			}
		}
		repairCheck(found, "block patch has no source extent")
	}
	put := func(logical, physical, count uint64) {
		if count == 0 {
			return
		}
		f := make([]byte, 24)
		np64(f, count*4096)
		np64(f[8:], physical)
		fs.put(nativeKey(id, 8, logical*4096), f)
		r := make([]byte, 20)
		np64(r, count|1<<60)
		np64(r[8:], id)
		np32(r[16:], 1)
		ext.put(nativeKey(physical, 2), r)
	}
	index := 0
	for _, old := range stream.PhysicalExtents {
		startBlock, endBlock := old.LogicalOffset/4096, (old.LogicalOffset+old.DataSize)/4096
		if index == len(patch.blocks) || patch.blocks[index].index >= endBlock {
			continue
		}
		fs.put(nativeKey(id, 8, old.LogicalOffset), nil)
		ext.put(nativeKey(old.PhysicalBlockNumber, 2), nil)
		cursor := startBlock
		for index < len(patch.blocks) && patch.blocks[index].index < endBlock {
			start := patch.blocks[index].index
			put(cursor, old.PhysicalBlockNumber+cursor-startBlock, start-cursor)
			end := index + 1
			for end < len(patch.blocks) && patch.blocks[end].index < endBlock && patch.blocks[end].index == patch.blocks[end-1].index+1 {
				end++
			}
			count := uint64(end - index)
			p := n.allocate(count)
			raw := make([]byte, 0, count*4096)
			for _, b := range patch.blocks[index:end] {
				raw = append(raw, b.after...)
			}
			n.write(p, raw)
			put(start, p, count)
			n.release(old.PhysicalBlockNumber+start-startBlock, count)
			cursor = start + count
			index = end
		}
		put(cursor, old.PhysicalBlockNumber+cursor-startBlock, endBlock-cursor)
	}
	repairCheck(index == len(patch.blocks), "incomplete block patch")
	return uint64(len(patch.blocks))
}
