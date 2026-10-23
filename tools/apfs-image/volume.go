// SPDX-License-Identifier: MIT
package main

import (
	"bytes"
	"crypto/sha256"
	"encoding/hex"
	"flag"
	"fmt"
	"os"
	"path/filepath"

	"github.com/deploymenttheory/go-apfs-v2/pkg/apfswrite"
	"github.com/deploymenttheory/go-apfs-v2/pkg/hfsplus"
	"github.com/deploymenttheory/go-apfs-v2/pkg/osversion"
)

// Create a standalone auxiliary volume; no source or device is modified.
func createVolume(args []string) {
	flags := flag.NewFlagSet("create-volume", flag.ExitOnError)
	overlay := flags.String("overlay", "", "root-owned volume contents")
	output := flags.String("output", "", "new raw filesystem image (must not exist)")
	name := flags.String("name", "Drivers", "volume name")
	filesystem := flags.String("filesystem", "apfs", "apfs or hfs (case-sensitive HFSX)")
	size := flags.Uint64("size-mib", 16, "container size in MiB (8..1024)")
	flags.Parse(args)
	if *overlay == "" || *output == "" || *name == "" || *size < 8 || *size > 1024 || flags.NArg() != 0 {
		panic("create-volume requires overlay, output, name and size-mib in 8..1024")
	}
	if *filesystem != "apfs" && *filesystem != "hfs" {
		panic("filesystem must be apfs or hfs")
	}
	root := &apfswrite.Entry{Mode: os.ModeDir | 0755, ModeExplicit: true}
	s := stats{}
	addOverlay(root, *overlay, &s)
	if s.Entries == 0 {
		panic("auxiliary volume must contain files")
	}
	containerID, volumeID := uuid(), uuid()
	opts := &apfswrite.CreateOptions{TargetVersion: osversion.Version{Major: 15},
		ContainerUUID: containerID, Volumes: []apfswrite.VolumeSpec{{
			Name: *name, UUID: volumeID, CaseSensitive: true, Root: root,
		}}}
	volumeBytes := int64(*size * 1024 * 1024)
	if *filesystem == "apfs" {
		must(apfswrite.PlanContainer(volumeBytes, opts))
	}
	f := must(os.OpenFile(*output, os.O_RDWR|os.O_CREATE|os.O_EXCL, 0644))
	defer f.Close()
	var err error
	if *filesystem == "apfs" {
		err = apfswrite.CreateContainer(f, volumeBytes, opts)
	} else {
		var convert func(*apfswrite.Entry) *hfsplus.Entry
		convert = func(e *apfswrite.Entry) *hfsplus.Entry {
			h := &hfsplus.Entry{Name: e.Name, Mode: e.Mode, ModeExplicit: true, UID: e.UID, GID: e.GID, Data: e.Data}
			for _, child := range e.Children {
				h.Children = append(h.Children, convert(child))
			}
			return h
		}
		if err = f.Truncate(volumeBytes); err == nil {
			err = hfsplus.CreateImage(f, volumeBytes, *name, convert(root), &hfsplus.CreateOptions{VolumeUUID: volumeID})
		}
	}
	if err != nil {
		panic(err)
	}
	if err := f.Sync(); err != nil {
		panic(err)
	}
	files := map[string]string{}
	var record func(string, *apfswrite.Entry)
	var hfs *hfsplus.Volume
	if *filesystem == "hfs" {
		hfs = must(hfsplus.New(f))
	}
	record = func(prefix string, entry *apfswrite.Entry) {
		path := filepath.ToSlash(filepath.Join(prefix, entry.Name))
		if entry.Mode.IsRegular() {
			sum := sha256.Sum256(entry.Data)
			files[path] = hex.EncodeToString(sum[:])
			if hfs != nil && !bytes.Equal(must(hfs.ReadFile(path)), entry.Data) {
				panic("HFS image readback mismatch: " + path)
			}
		}
		for _, child := range entry.Children {
			record(path, child)
		}
	}
	for _, entry := range root.Children {
		record("", entry)
	}
	volume := ""
	if *filesystem == "apfs" {
		volume = uuidString(volumeID)
	}
	report := map[string]any{
		"image": filepath.Base(*output), "image_sha256": hash(*output),
		"container_bytes": volumeBytes,
		"filesystem":      *filesystem, "volume_uuid": volume, "volume_name": *name,
		"source": *overlay, "contents": s,
		"file_sha256": files,
	}
	if *filesystem == "apfs" {
		report["container_uuid"] = uuidString(containerID)
	}
	writeJSON(*output+".json", report)
	fmt.Println(*output)
}
