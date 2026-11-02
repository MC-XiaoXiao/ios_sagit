// SPDX-License-Identifier: MIT
// Stream a firmware volume into an iOS System/Data container, without a host mount.
package main

import (
	"crypto/rand"
	"crypto/sha256"
	"encoding/hex"
	"encoding/json"
	"flag"
	"fmt"
	"io"
	"os"
	"path/filepath"
	"strings"
	"time"

	"github.com/deploymenttheory/go-apfs-v2/pkg/apfs"
	"github.com/deploymenttheory/go-apfs-v2/pkg/apfswrite"
	"github.com/deploymenttheory/go-apfs-v2/pkg/appledouble"
	"github.com/deploymenttheory/go-apfs-v2/pkg/hostdata"
	"github.com/deploymenttheory/go-apfs-v2/pkg/osversion"
)

type value struct {
	io.ReaderAt
	length int64
}

func (v value) Size() int64 { return v.length }

type stats struct {
	Bytes   uint64 `json:"allocated_payload_bytes"`
	Entries uint64 `json:"entries"`
}

func must[T any](v T, err error) T {
	if err != nil {
		panic(err)
	}
	return v
}
func uuid() [16]byte {
	var u [16]byte
	must(rand.Read(u[:]))
	u[6] = (u[6] & 15) | 64
	u[8] = (u[8] & 63) | 128
	return u
}
func uuidString(u [16]byte) string {
	h := hex.EncodeToString(u[:])
	return strings.ToUpper(h[:8] + "-" + h[8:12] + "-" + h[12:16] + "-" + h[16:20] + "-" + h[20:])
}
func mode(m uint16) os.FileMode {
	r := os.FileMode(m & 0777)
	if m&04000 != 0 {
		r |= os.ModeSetuid
	}
	if m&02000 != 0 {
		r |= os.ModeSetgid
	}
	if m&01000 != 0 {
		r |= os.ModeSticky
	}
	switch m & 0170000 {
	case 0040000:
		r |= os.ModeDir
	case 0120000:
		r |= os.ModeSymlink
	case 0100000:
	default:
		panic(fmt.Errorf("unsupported inode mode %#o", m))
	}
	return r
}
func copyEntry(fe *apfs.FileEntry, name string, s *stats, seen map[uint64]*apfswrite.Entry) *apfswrite.Entry {
	id := must(fe.Identifier())
	m := mode(must(fe.FileMode()))
	e := &apfswrite.Entry{Name: name, Mode: m, ModeExplicit: true, UID: must(fe.OwnerIdentifier()), GID: must(fe.GroupIdentifier())}
	if m.IsRegular() {
		if prev := seen[id]; prev != nil {
			*e = *prev
			e.Name = name
			return e
		}
		seen[id] = e
		e.LinkGroup = id
	}
	s.Entries++
	flags := must(fe.BSDFlags())
	e.BSDFlags = &flags
	e.Times = &hostdata.FileTimes{Birth: time.Unix(0, must(fe.CreationTime())), Modify: time.Unix(0, must(fe.ModificationTime())), Change: time.Unix(0, must(fe.InodeChangeTime())), Access: time.Unix(0, must(fe.AccessTime()))}
	e.XattrValues = map[string]appledouble.Value{}
	for i, n := 0, must(fe.NumberOfExtendedAttributes()); i < n; i++ {
		a := must(fe.ExtendedAttributeByIndex(i))
		an := must(a.UTF8Name())
		if an == "com.apple.fs.symlink" {
			continue
		}
		size := must(a.Size())
		e.XattrValues[an] = sourceValue(a, int64(size))
		s.Bytes += (size + 4095) &^ 4095
	}
	if m.IsDir() {
		for i, n := 0, must(fe.NumberOfSubFileEntries()); i < n; i++ {
			child := must(fe.SubFileEntryByIndex(i))
			cn := must(child.UTF8Name())
			e.Children = append(e.Children, copyEntry(child, cn, s, seen))
		}
	} else if m&os.ModeSymlink != 0 {
		e.Data = []byte(must(fe.SymbolicLinkTarget()))
	} else {
		stream := must(fe.RawDataStream())
		e.DataValue = sourceValue(stream, int64(stream.Size()))
		s.Bytes += (stream.Size() + 4095) &^ 4095
	}
	return e
}
func child(e *apfswrite.Entry, name string) *apfswrite.Entry {
	for _, c := range e.Children {
		if c.Name == name {
			return c
		}
	}
	return nil
}
func dir(e *apfswrite.Entry, name string) *apfswrite.Entry {
	c := child(e, name)
	if c == nil {
		c = &apfswrite.Entry{Name: name, Mode: os.ModeDir | 0755}
		e.Children = append(e.Children, c)
	}
	if !c.Mode.IsDir() {
		panic(fmt.Errorf("%s is not a directory", name))
	}
	return c
}
func replace(e *apfswrite.Entry, file *apfswrite.Entry) {
	for i, c := range e.Children {
		if c.Name == file.Name {
			e.Children[i] = file
			return
		}
	}
	e.Children = append(e.Children, file)
}

func fstab(original, sys, data string) string {
	var lines []string
	for _, line := range strings.Split(original, "\n") {
		f := strings.Fields(line)
		if len(f) > 1 && !strings.HasPrefix(f[0], "#") && (f[1] == "/" || f[1] == "/private/var" || f[1] == "/var") {
			continue
		}
		if strings.TrimSpace(line) != "" {
			lines = append(lines, line)
		}
	}
	return strings.Join(append(lines, "UUID="+sys+" / apfs ro 0 1", "UUID="+data+" /private/var apfs rw,nosuid,nodev 0 2"), "\n") + "\n"
}
func hash(path string) string {
	f := must(os.Open(path))
	defer f.Close()
	h := sha256.New()
	must(io.Copy(h, f))
	return hex.EncodeToString(h.Sum(nil))
}
func writeJSON(path string, v any) {
	b := must(json.MarshalIndent(v, "", "  "))
	if err := os.WriteFile(path, append(b, '\n'), 0644); err != nil {
		panic(err)
	}
}
func run() {
	source := flag.String("source", "", "decrypted firmware APFS image or DMG")
	output := flag.String("output", "", "new raw APFS image (must not exist)")
	minimum := flag.Uint64("system-min-mib", 0, "BuildManifest MinimumSystemPartition in MiB")
	dataFree := flag.Uint64("data-free-mib", 2048, "minimum writable Data headroom in MiB")
	inspect := flag.Bool("inspect-only", false, "inventory source without creating an image")
	cryptexOS := flag.String("cryptex-os", "", "original decrypted SystemOS Cryptex image to retain")
	cryptexApp := flag.String("cryptex-app", "", "original AppOS Cryptex image to retain")
	overlay := flag.String("system-overlay", "", "root-owned files to add to System")
	addVolumes := flag.String("add-volumes", "", "JSON array of additional roles, UUIDs and overlays")
	identity := flag.String("identity-report", "", "retain existing System/Data/container/group UUIDs when rebuilding")
	dataOverlay := flag.String("data-overlay", "", "prepared Data payload manifest with explicit ownership and modes")
	flag.Parse()
	if *source == "" || *output == "" {
		panic("--source and --output are required")
	}
	c, closer, err := apfs.OpenImage(*source, nil)
	if err != nil {
		panic(err)
	}
	defer closer.Close()
	defer c.Close()
	v := must(c.VolumeBySelector("system"))
	rootfe := must(v.RootDirectory())
	s := stats{}
	root := copyEntry(rootfe, "", &s, map[uint64]*apfswrite.Entry{})
	if *overlay != "" {
		addOverlay(root, *overlay, &s)
	}
	cryptexes := map[string]any{}
	for name, path := range map[string]string{"SystemOS.dmg": *cryptexOS, "AppOS.dmg": *cryptexApp} {
		if path == "" {
			continue
		}
		image := must(os.Open(path))
		defer image.Close()
		info := must(image.Stat())
		if !info.Mode().IsRegular() {
			panic("Cryptex input must be a regular image")
		}
		// Preserve original sealed filesystem images. Activation/boot-policy handoff
		// belongs to the restore userspace; do not turn a seal into an unsealed tree.
		payload := dir(dir(dir(dir(root, "usr"), "standalone"), "firmware"), "Cryptex1")
		replace(payload, &apfswrite.Entry{Name: name, Mode: 0444, DataValue: value{image, info.Size()}})
		s.Bytes += (uint64(info.Size()) + 4095) &^ 4095
		s.Entries++
		cryptexes[name] = map[string]any{"source": path, "sha256": hash(path), "bytes": info.Size(), "activation_verified": false}
	}
	fmt.Fprintf(os.Stderr, "Imported %d entries, %d allocated payload bytes\n", s.Entries, s.Bytes)
	private := dir(root, "private")
	data := dir(private, "var")
	if *dataOverlay != "" {
		addDataOverlay(data, *dataOverlay, &s)
	}
	dataName := data.Name
	data.Name = ""
	// The Data volume root is /private/var, so its children must not gain another var prefix.
	for i, e := range private.Children {
		if e == data {
			private.Children[i] = &apfswrite.Entry{Name: dataName, Mode: os.ModeDir | 0755}
			break
		}
	}
	// Preserve the source filesystem's grouping semantics. iOS System payloads
	// commonly use roles without the macOS unified inode/volume-group space.
	systemUUID, dataUUID, containerUUID, groupUUID := uuid(), uuid(), uuid(), must(v.VolumeGroupIdentifier())
	if *identity != "" {
		var ids map[string]any
		if err := json.Unmarshal(must(os.ReadFile(*identity)), &ids); err != nil {
			panic(err)
		}
		systemUUID = parseUUID(ids["system_uuid"].(string))
		dataUUID = parseUUID(ids["data_uuid"].(string))
		containerUUID = parseUUID(ids["container_uuid"].(string))
		groupUUID = parseUUID(ids["volume_group_uuid"].(string))
		if systemUUID == dataUUID {
			panic("System and Data identities must differ")
		}
	}
	sys, dat := uuidString(systemUUID), uuidString(dataUUID)
	etc := dir(private, "etc")
	old := ""
	if f := child(etc, "fstab"); f != nil {
		if f.DataValue != nil {
			b := make([]byte, f.DataValue.Size())
			must(f.DataValue.ReadAt(b, 0))
			old = string(b)
		} else {
			old = string(f.Data)
		}
	}
	mounts := fstab(old, sys, dat)
	replace(etc, &apfswrite.Entry{Name: "fstab", Mode: 0644, Data: []byte(mounts)})
	opts := &apfswrite.CreateOptions{TargetVersion: osversion.Version{Major: 15}, ContainerUUID: containerUUID, Volumes: []apfswrite.VolumeSpec{
		{Name: "System", UUID: systemUUID, Role: apfs.VolumeRoleSystem, VolumeGroupID: groupUUID, CaseSensitive: true, Root: root},
		{Name: "Data", UUID: dataUUID, Role: apfs.VolumeRoleData, VolumeGroupID: groupUUID, CaseSensitive: true, Root: data},
	}}
	if *addVolumes != "" {
		var additions []addedVolume
		if err := json.Unmarshal(must(os.ReadFile(*addVolumes)), &additions); err != nil {
			panic(err)
		}
		opts.Volumes = appendVolumes(opts.Volumes, additions, &s)
	}
	lower := int64(*minimum)
	if lower < 1024 {
		lower = 1024
	}
	upper := (must(apfswrite.PlanContainer(0, opts)) + 1024*1024 - 1) / (1024 * 1024)
	if upper < lower {
		upper = lower
	}
	for lower < upper {
		middle := (lower + upper) / 2
		if _, err := apfswrite.PlanContainer(middle*1024*1024, opts); err != nil {
			if !strings.Contains(err.Error(), "do not fit") {
				panic(err)
			}
			lower = middle + 1
		} else {
			upper = middle
		}
	}
	must(apfswrite.PlanContainer(lower*1024*1024, opts))
	size := (uint64(lower) + *dataFree) * 1024 * 1024
	opts.MinimumFreeBytes = *dataFree * 1024 * 1024
	for {
		_, err := apfswrite.PlanContainer(int64(size), opts)
		if err == nil {
			break
		}
		if !strings.Contains(err.Error(), "insufficient shared free space") {
			panic(err)
		}
		size += 1024 * 1024
	}
	fmt.Fprintf(os.Stderr, "Minimum layout: %d MiB; with Data headroom: %d MiB\n", lower, size/(1024*1024))
	report := map[string]any{"schema": 1, "source": *source, "source_sha256": hash(*source), "source_volume_uuid": uuidString(must(v.Identifier())), "inventory": s, "cryptex_images": cryptexes, "minimum_layout_mib": lower, "system_min_mib": *minimum, "data_free_mib": *dataFree, "container_bytes": size, "container_uuid": uuidString(containerUUID), "volume_group_uuid": uuidString(groupUUID), "system_uuid": sys, "data_uuid": dat, "fstab": mounts, "hardware_boot_verified": false, "authenticated_root_preserved": false}
	report["system_overlay"] = *overlay
	report["identity_report"] = *identity
	var volumeIDs []volumeIdentity
	for _, spec := range opts.Volumes {
		volumeIDs = append(volumeIDs, volumeIdentity{spec.Name, uint16(spec.Role), uuidString(spec.UUID), uuidString(spec.VolumeGroupID)})
	}
	report["volumes"] = volumeIDs
	report["data_overlay"] = *dataOverlay
	if *inspect {
		writeJSON(*output, report)
		return
	}
	f := must(os.OpenFile(*output, os.O_CREATE|os.O_EXCL|os.O_RDWR, 0644))
	good := false
	defer func() {
		f.Close()
		if !good {
			os.Remove(*output)
		}
	}()
	if err := apfswrite.CreateContainer(f, int64(size), opts); err != nil {
		panic(err)
	}
	if err := f.Sync(); err != nil {
		panic(err)
	}
	// Read the actual output and check identities and the written mount table.
	actual := must(apfs.Open(f, nil))
	defer actual.Close()
	sm := apfs.NewSpaceManager()
	smBlock := must(actual.CheckpointMap.PhysicalAddressByObjectIdentifier(actual.Superblock.SpacemanOID))
	if err := sm.ReadFrom(f, int64(smBlock)*int64(actual.IOHandle.BlockSize)); err != nil {
		panic(err)
	}
	freeBytes := sm.Devices[apfs.SpaceManagerDeviceMain].FreeCount * uint64(sm.BlockSize)
	if freeBytes < opts.MinimumFreeBytes {
		panic("actual shared free space is below the requested Data headroom")
	}
	report["shared_free_bytes"] = freeBytes
	av := must(actual.VolumeBySelector("system"))
	dv := must(actual.VolumeBySelector("data"))
	if must(av.Identifier()) != systemUUID || must(dv.Identifier()) != dataUUID || string(must(av.ReadFile("private/etc/fstab"))) != mounts {
		panic("output volume verification failed")
	}
	good = true
	report["image_sha256"] = hash(*output)
	report["image"] = filepath.Base(*output)
	writeJSON(*output+".json", report)
	if err := os.WriteFile(*output+".fstab", []byte(mounts), 0644); err != nil {
		panic(err)
	}
	fmt.Println(*output)
}
func main() {
	defer func() {
		if e := recover(); e != nil {
			fmt.Fprintln(os.Stderr, "apfs-image:", e)
			os.Exit(1)
		}
	}()
	if len(os.Args) > 1 && os.Args[1] == "extract-files" {
		extractFiles(os.Args[2:])
		return
	}
	if len(os.Args) > 1 && os.Args[1] == "create-volume" {
		createVolume(os.Args[2:])
		return
	}
	if len(os.Args) > 1 && os.Args[1] == "verify-device" {
		verifyDevice(os.Args[2:])
		return
	}
	if len(os.Args) > 1 && os.Args[1] == "update-plan" {
		updatePlan(os.Args[2:])
		return
	}
	if len(os.Args) > 1 && os.Args[1] == "checkpoint-plan" {
		checkpointPlan(os.Args[2:])
		return
	}
	if len(os.Args) > 1 && os.Args[1] == "system-update-plan" {
		systemUpdatePlan(os.Args[2:])
		return
	}
	if len(os.Args) > 1 && os.Args[1] == "data-update-plan" {
		dataUpdatePlan(os.Args[2:])
		return
	}
	if len(os.Args) > 1 && os.Args[1] == "verify-update" {
		verifyUpdate(os.Args[2:])
		return
	}
	run()
}
