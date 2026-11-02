package main

import (
	"crypto/sha256"
	"encoding/hex"
	"encoding/json"
	"flag"
	"fmt"
	"github.com/deploymenttheory/go-apfs-v2/pkg/apfs"
	"io"
	"os"
)

// Read the actual device using a bounded view; the host need not refresh its
// partition nodes, and the firmware's UFS logical sector size may be 4096.
func verifyDevice(args []string) {
	flags := flag.NewFlagSet("verify-device", flag.ExitOnError)
	device := flags.String("device", "", "whole UFS LUN")
	offset := flags.Int64("offset", -1, "partition byte offset from the checked GPT")
	expected := flags.String("report", "", "expected apfs-image JSON report")
	output := flags.String("output", "", "verification JSON destination")
	flags.Parse(args)
	if *offset < 0 || *device == "" || *expected == "" || *output == "" {
		panic("verify-device requires device, offset, report and output")
	}
	var report struct {
		ContainerBytes int64            `json:"container_bytes"`
		SystemUUID     string           `json:"system_uuid"`
		DataUUID       string           `json:"data_uuid"`
		GroupUUID      string           `json:"volume_group_uuid"`
		Fstab          string           `json:"fstab"`
		DataFreeMiB    uint64           `json:"data_free_mib"`
		Volumes        []volumeIdentity `json:"volumes"`
		CryptexImages  map[string]struct {
			SHA256 string `json:"sha256"`
		} `json:"cryptex_images"`
	}
	if err := json.Unmarshal(must(os.ReadFile(*expected)), &report); err != nil {
		panic(err)
	}
	f := must(os.Open(*device))
	defer f.Close()
	reader := io.NewSectionReader(f, *offset, report.ContainerBytes)
	c := must(apfs.Open(reader, nil))
	defer c.Close()
	expectedCount := 2
	if len(report.Volumes) != 0 {
		expectedCount = len(report.Volumes)
	}
	if must(c.NumberOfVolumes()) != expectedCount {
		panic("unexpected volume count")
	}
	for _, expected := range report.Volumes {
		volume := must(c.VolumeBySelector(expected.UUID))
		if uuidString(must(volume.Identifier())) != expected.UUID || uint16(must(volume.Role())) != expected.Role ||
			uuidString(must(volume.VolumeGroupIdentifier())) != expected.GroupUUID {
			panic("device role volume identity mismatch")
		}
	}
	system := must(c.VolumeBySelector("system"))
	data := must(c.VolumeBySelector("data"))
	if uuidString(must(system.Identifier())) != report.SystemUUID || uuidString(must(data.Identifier())) != report.DataUUID {
		panic("device volume UUID mismatch")
	}
	if uuidString(must(system.VolumeGroupIdentifier())) != report.GroupUUID || uuidString(must(data.VolumeGroupIdentifier())) != report.GroupUUID {
		panic("device volume group mismatch")
	}
	if string(must(system.ReadFile("private/etc/fstab"))) != report.Fstab {
		panic("device fstab mismatch")
	}
	for name, expected := range report.CryptexImages {
		if name != "SystemOS.dmg" && name != "AppOS.dmg" {
			panic("unexpected retained Cryptex name")
		}
		entry := must(system.FileEntryByPath("usr/standalone/firmware/Cryptex1/" + name))
		h := sha256.New()
		must(io.CopyBuffer(h, io.NewSectionReader(entry, 0, int64(must(entry.Size()))), make([]byte, 4<<20)))
		if hex.EncodeToString(h.Sum(nil)) != expected.SHA256 {
			panic("retained Cryptex payload hash mismatch: " + name)
		}
	}
	must(data.RootDirectory())
	sm := apfs.NewSpaceManager()
	block := must(c.CheckpointMap.PhysicalAddressByObjectIdentifier(c.Superblock.SpacemanOID))
	if err := sm.ReadFrom(reader, int64(block)*int64(c.IOHandle.BlockSize)); err != nil {
		panic(err)
	}
	free := sm.Devices[apfs.SpaceManagerDeviceMain].FreeCount * uint64(sm.BlockSize)
	if free < report.DataFreeMiB*1024*1024 {
		panic("device Data headroom is insufficient")
	}
	writeJSON(*output, map[string]any{"device": *device, "partition_byte_offset": *offset, "container_bytes": report.ContainerBytes, "system_uuid": report.SystemUUID, "data_uuid": report.DataUUID, "volume_group_uuid": report.GroupUUID, "fstab_matches": true, "device_filesystem_read_verified": true, "shared_free_bytes": free, "data_headroom_verified": true, "full_ios_boot_verified": false})
	fmt.Println("APFS reader: volume identities, fstab, Cryptex payloads and free space verified.")
}
