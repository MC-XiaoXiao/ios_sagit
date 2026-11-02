package main

import (
	"encoding/json"
	"fmt"
	"io"
	"os"
	"path/filepath"
	"sort"
	"strings"

	"github.com/deploymenttheory/go-apfs-v2/pkg/apfswrite"
	"github.com/deploymenttheory/go-apfs-v2/pkg/appledouble"
)

// Prepared payloads carry filesystem metadata, never product policy.
type dataOverlayEntry struct {
	Path     string `json:"path"`
	Type     string `json:"type"`
	Source   string `json:"source,omitempty"`
	SHA256   string `json:"sha256,omitempty"`
	Mode     uint32 `json:"mode"`
	UID      uint32 `json:"uid"`
	GID      uint32 `json:"gid"`
	Existing string `json:"existing"`
}

func entryData(entry *apfswrite.Entry) []byte {
	if entry.DataValue == nil {
		return entry.Data
	}
	size := entry.DataValue.Size()
	if size < 0 || size > 64<<20 {
		panic("overlay input exceeds 64 MiB")
	}
	data := make([]byte, size)
	n, err := entry.DataValue.ReadAt(data, 0)
	if err != nil && err != io.EOF {
		panic(err)
	}
	if n != len(data) {
		panic("short overlay input read")
	}
	return data
}

func addDataOverlay(root *apfswrite.Entry, manifest string, count *stats) {
	var spec struct {
		Schema  int                `json:"schema"`
		Entries []dataOverlayEntry `json:"entries"`
	}
	decoder := json.NewDecoder(strings.NewReader(string(must(os.ReadFile(manifest)))))
	decoder.DisallowUnknownFields()
	if err := decoder.Decode(&spec); err != nil {
		panic(err)
	}
	if spec.Schema != 1 {
		panic("unsupported Data overlay manifest")
	}
	seen := map[string]bool{}
	payloads := map[string][]byte{}
	for _, item := range spec.Entries {
		safeExtractPath(item.Path)
		if seen[item.Path] || item.Mode > 0777 || (item.Type != "file" && item.Type != "directory") ||
			(item.Existing != "keep" && item.Existing != "replace") {
			panic("invalid or duplicate Data overlay entry")
		}
		seen[item.Path] = true
		if item.Type == "directory" {
			if item.Source != "" || item.SHA256 != "" || item.Existing != "keep" {
				panic("invalid directory overlay entry")
			}
			continue
		}
		safeExtractPath(item.Source)
		source := filepath.Join(filepath.Dir(manifest), filepath.FromSlash(item.Source))
		for p := source; p != filepath.Dir(manifest); p = filepath.Dir(p) {
			if must(os.Lstat(p)).Mode()&os.ModeSymlink != 0 {
				panic("Data overlay source crosses a symlink")
			}
		}
		info := must(os.Stat(source))
		if !info.Mode().IsRegular() || info.Size() > 64<<20 || len(item.SHA256) != 64 || hash(source) != item.SHA256 {
			panic("Data overlay source changed or is invalid")
		}
		payloads[item.Path] = must(os.ReadFile(source))
	}
	sort.SliceStable(spec.Entries, func(i, j int) bool {
		a, b := spec.Entries[i].Path, spec.Entries[j].Path
		if strings.Count(a, "/") != strings.Count(b, "/") {
			return strings.Count(a, "/") < strings.Count(b, "/")
		}
		return a < b
	})
	// Validate all existing parents and targets before changing the imported tree.
	for _, item := range spec.Entries {
		target := root
		parts := strings.Split(item.Path, "/")
		for i, name := range parts {
			current := child(target, name)
			if i < len(parts)-1 {
				if current == nil {
					prefix := strings.Join(parts[:i+1], "/")
					found := false
					for _, parent := range spec.Entries {
						if parent.Path == prefix && parent.Type == "directory" {
							found = true
						}
					}
					if !found {
						panic("Data overlay parent not declared: " + prefix)
					}
					target = &apfswrite.Entry{}
				} else {
					if !current.Mode.IsDir() {
						panic("Data overlay parent is not a directory")
					}
					target = current
				}
				continue
			}
			if current != nil && ((item.Type == "directory" && !current.Mode.IsDir()) || (item.Type == "file" && !current.Mode.IsRegular())) {
				panic("Data overlay target type conflict")
			}
		}
	}
	for _, item := range spec.Entries {
		parts := strings.Split(item.Path, "/")
		parent := root
		for _, name := range parts[:len(parts)-1] {
			parent = child(parent, name)
		}
		name := parts[len(parts)-1]
		previous := child(parent, name)
		if previous != nil && item.Existing == "keep" {
			continue
		}
		entry := &apfswrite.Entry{Name: name, Mode: os.FileMode(item.Mode), ModeExplicit: true, UID: item.UID, GID: item.GID}
		if item.Type == "directory" {
			entry.Mode |= os.ModeDir
		} else {
			entry.Data = payloads[item.Path]
		}
		if previous != nil {
			entry.Times, entry.BSDFlags = previous.Times, previous.BSDFlags
			entry.XattrValues = make(map[string]appledouble.Value, len(previous.XattrValues))
			old := (uint64(len(entryData(previous))) + 4095) &^ 4095
			_, compressed := previous.XattrValues["com.apple.decmpfs"]
			for name, value := range previous.XattrValues {
				// The replacement has an ordinary data fork. Retire the old
				// compression storage and keep every unrelated attribute.
				if compressed && (name == "com.apple.decmpfs" || name == "com.apple.ResourceFork") {
					old += (uint64(value.Size()) + 4095) &^ 4095
					continue
				}
				entry.XattrValues[name] = value
			}
			if compressed && previous.BSDFlags != nil {
				flags := *previous.BSDFlags &^ uint32(0x20) // UF_COMPRESSED
				entry.BSDFlags = &flags
			}
			if old > count.Bytes {
				panic(fmt.Errorf("invalid overlay allocation accounting"))
			}
			count.Bytes -= old
		} else {
			count.Entries++
		}
		count.Bytes += (uint64(len(entry.Data)) + 4095) &^ 4095
		replace(parent, entry)
	}
}
