package main

import (
	"encoding/hex"
	"fmt"
	"github.com/deploymenttheory/go-apfs-v2/pkg/apfswrite"
	"io/fs"
	"os"
	"path/filepath"
	"strings"
)

func parseUUID(s string) [16]byte {
	b := must(hex.DecodeString(strings.ReplaceAll(s, "-", "")))
	if len(b) != 16 {
		panic("invalid identity UUID")
	}
	var u [16]byte
	copy(u[:], b)
	return u
}

func addOverlay(root *apfswrite.Entry, directory string, count *stats) {
	err := filepath.WalkDir(directory, func(path string, d fs.DirEntry, err error) error {
		if err != nil {
			return err
		}
		relative := must(filepath.Rel(directory, path))
		if relative == "." {
			return nil
		}
		parts := strings.Split(filepath.ToSlash(relative), "/")
		target := root
		for _, name := range parts[:len(parts)-1] {
			target = dir(target, name)
		}
		name := parts[len(parts)-1]
		info := must(os.Lstat(path))
		if info.IsDir() {
			dir(target, name)
			return nil
		}
		entry := &apfswrite.Entry{Name: name, Mode: info.Mode(), ModeExplicit: true, UID: 0, GID: 0}
		if info.Mode()&os.ModeSymlink != 0 {
			entry.Data = []byte(must(os.Readlink(path)))
		} else if info.Mode().IsRegular() {
			entry.Data = must(os.ReadFile(path))
			count.Bytes += (uint64(len(entry.Data)) + 4095) &^ 4095
		} else {
			return fmt.Errorf("unsupported overlay file: %s", path)
		}
		replace(target, entry)
		count.Entries++
		return nil
	})
	if err != nil {
		panic(err)
	}
}
