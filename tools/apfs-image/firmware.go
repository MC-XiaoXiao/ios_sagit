// SPDX-License-Identifier: MIT
package main

import (
	"encoding/json"
	"flag"
	"fmt"
	"io"
	"os"
	"path"
	"path/filepath"
	"strings"

	"github.com/deploymenttheory/go-apfs-v2/pkg/apfs"
	"github.com/deploymenttheory/go-apfs-v2/pkg/apfswrite"
)

// Use the same role recipe for fresh images and offline updates.
func appendVolumes(specs []apfswrite.VolumeSpec, additions []addedVolume, count *stats) []apfswrite.VolumeSpec {
	roles, ids, names := map[uint16]bool{}, map[[16]byte]bool{}, map[string]bool{}
	for _, spec := range specs {
		roles[spec.Role], ids[spec.UUID], names[spec.Name] = true, true, true
	}
	for _, addition := range additions {
		role, id := must(apfs.ParseVolumeRole(addition.Role)), parseUUID(addition.UUID)
		if role == 0 || roles[role] || id == ([16]byte{}) || ids[id] || addition.Name == "" || names[addition.Name] {
			panic("new volume must have a unique role, nonzero UUID and unique name")
		}
		root := &apfswrite.Entry{Mode: os.ModeDir | 0755, ModeExplicit: true}
		if addition.Overlay != "" {
			addOverlay(root, addition.Overlay, count)
		}
		specs = append(specs, apfswrite.VolumeSpec{Name: addition.Name, UUID: id, Role: role, CaseSensitive: true, Root: root})
		roles[role], ids[id], names[addition.Name] = true, true, true
	}
	return specs
}

type extractRequest struct {
	Path         string `json:"path"`
	Recursive    bool   `json:"recursive"`
	Suffix       string `json:"suffix"`
	Optional     bool   `json:"optional"`
	MetadataOnly bool   `json:"metadata_only"`
}

func lookupExtractEntry(volume *apfs.Volume, name string) *apfs.FileEntry {
	entry := must(volume.RootDirectory())
	for _, part := range strings.Split(safeExtractPath(name), "/") {
		if !mode(must(entry.FileMode())).IsDir() {
			panic("extraction parent is not a directory")
		}
		var found *apfs.FileEntry
		for i, n := 0, must(entry.NumberOfSubFileEntries()); i < n; i++ {
			candidate := must(entry.SubFileEntryByIndex(i))
			if must(candidate.UTF8Name()) == part {
				found = candidate
				break
			}
		}
		if found == nil {
			return nil
		}
		entry = found
	}
	return entry
}

func safeExtractPath(name string) string {
	if name == "" || name == "." || strings.Contains(name, "\\") || strings.HasPrefix(name, "/") || path.Clean(name) != name || strings.HasPrefix(name, "../") {
		panic("extraction requires a clean volume-relative path")
	}
	return name
}

// Extract only explicitly requested inputs, decoding native compressed files.
// Neither the source image nor any host filesystem symlink is followed/written.
func extractFiles(args []string) {
	flags := flag.NewFlagSet("extract-files", flag.ExitOnError)
	source := flags.String("source", "", "read-only firmware APFS image or DMG")
	selector := flags.String("volume", "system", "APFS volume selector")
	manifest := flags.String("manifest", "", "JSON array of path/recursive/suffix requests")
	output := flags.String("output", "", "new extraction directory")
	flags.Parse(args)
	if *source == "" || *manifest == "" || *output == "" || flags.NArg() != 0 {
		panic("extract-files requires source, manifest and output")
	}
	var requests []extractRequest
	if err := json.Unmarshal(must(os.ReadFile(*manifest)), &requests); err != nil || len(requests) == 0 {
		panic("invalid or empty extraction manifest")
	}
	for _, request := range requests {
		safeExtractPath(request.Path)
	}
	c, closer, err := apfs.OpenImage(*source, nil)
	if err != nil {
		panic(err)
	}
	defer closer.Close()
	defer c.Close()
	volume := must(c.VolumeBySelector(*selector))
	if err := os.Mkdir(*output, 0755); err != nil {
		panic(err)
	}
	records := map[string]string{}
	metadata := map[string]any{}
	var visit func(*apfs.FileEntry, string, extractRequest)
	visit = func(entry *apfs.FileEntry, name string, request extractRequest) {
		safeExtractPath(name)
		mode := mode(must(entry.FileMode()))
		if mode.IsDir() {
			if !request.Recursive {
				panic("directory extraction requires recursive=true")
			}
			for i, n := 0, must(entry.NumberOfSubFileEntries()); i < n; i++ {
				child := must(entry.SubFileEntryByIndex(i))
				leaf := must(child.UTF8Name())
				if leaf == "." || leaf == ".." || strings.ContainsAny(leaf, "/\\") {
					panic("unsafe firmware directory entry")
				}
				visit(child, name+"/"+leaf, request)
			}
			return
		}
		if request.Suffix != "" && !strings.HasSuffix(name, request.Suffix) {
			return
		}
		if !mode.IsRegular() {
			panic(fmt.Errorf("extraction refuses nonregular input: %s", name))
		}
		if must(entry.Size()) > 64*1024*1024 {
			panic("extraction is limited to small firmware inputs (64 MiB/file)")
		}
		target := filepath.Join(*output, filepath.FromSlash(name))
		if err := os.MkdirAll(filepath.Dir(target), 0755); err != nil {
			panic(err)
		}
		file := must(os.OpenFile(target, os.O_WRONLY|os.O_CREATE|os.O_EXCL, mode.Perm()))
		size := must(entry.Size())
		_, copyErr := io.CopyN(file, io.NewSectionReader(entry, 0, int64(size)), int64(size))
		closeErr := file.Close()
		if copyErr != nil {
			panic(copyErr)
		}
		if closeErr != nil {
			panic(closeErr)
		}
		records[name] = hash(target)
	}
	for _, request := range requests {
		entry := lookupExtractEntry(volume, request.Path)
		if entry == nil {
			if !request.Optional {
				panic("requested extraction path is missing: " + request.Path)
			}
			metadata[request.Path] = map[string]any{"exists": false}
			continue
		}
		metadata[request.Path] = map[string]any{"exists": true, "mode": must(entry.FileMode()), "uid": must(entry.OwnerIdentifier()), "gid": must(entry.GroupIdentifier())}
		if !request.MetadataOnly {
			visit(entry, request.Path, request)
		}
	}
	writeJSON(*output+".json", records)
	writeJSON(*output+".metadata.json", metadata)
}
