package main

import (
	"bytes"
	"encoding/binary"
	"encoding/json"
	"fmt"
	"io"
	"os"
	"os/exec"
	"path/filepath"
	"testing"

	"github.com/deploymenttheory/go-apfs-v2/pkg/apfs"
	"github.com/deploymenttheory/go-apfs-v2/pkg/apfswrite"
	"howett.net/plist"
)

func TestCOWUpdateCanSeedSetupCompletionInDataOnly(t *testing.T) {
	d := t.TempDir()
	path := filepath.Join(d, "fixture.apfs")
	f := must(os.Create(path))
	defer f.Close()
	system := &apfswrite.Entry{Mode: os.ModeDir | 0755, Children: []*apfswrite.Entry{
		{Name: "private", Mode: os.ModeDir | 0755, Children: []*apfswrite.Entry{{Name: "etc", Mode: os.ModeDir | 0755, Children: []*apfswrite.Entry{{Name: "fstab", Mode: 0644, Data: []byte("# fixture mount table\n")}}}}},
	}}
	data := &apfswrite.Entry{Mode: os.ModeDir | 0755, Children: []*apfswrite.Entry{{Name: "userdata", Mode: 0600, Data: []byte("preserved")}}}
	if err := apfswrite.CreateContainer(f, 1<<30, &apfswrite.CreateOptions{Volumes: []apfswrite.VolumeSpec{
		{Name: "System", Role: apfs.VolumeRoleSystem, CaseSensitive: true, Root: system},
		{Name: "Data", Role: apfs.VolumeRoleData, CaseSensitive: true, Root: data},
	}}); err != nil {
		t.Fatal(err)
	}
	patch := filepath.Join(d, "setup.delta")
	updatePlan([]string{"--source", path, "--development-setup-complete", "--output", patch})

	raw := must(os.ReadFile(patch))
	count := binary.LittleEndian.Uint64(raw[16:])
	var commit []byte
	for i := uint64(0); i < count; i++ {
		record := raw[24+i*4104 : 24+(i+1)*4104]
		block := binary.LittleEndian.Uint64(record)
		if block == 0 {
			commit = append([]byte(nil), record[8:]...)
			continue
		}
		must(f.WriteAt(record[8:], int64(block)*4096))
	}
	must(true, f.Sync())
	if len(commit) != 4096 {
		t.Fatal("missing primary commit")
	}
	must(f.WriteAt(commit, 0))
	must(true, f.Sync())

	result := must(apfs.Open(f, nil))
	defer result.Close()
	newData := must(result.VolumeBySelector("data"))
	if string(must(newData.ReadFile("userdata"))) != "preserved" {
		t.Fatal("existing Data file was not preserved")
	}
	var prefs map[string]any
	if _, err := plist.Unmarshal(must(newData.ReadFile("mobile/Library/Preferences/com.apple.purplebuddy.plist")), &prefs); err != nil {
		t.Fatal(err)
	}
	if prefs["SetupDone"] != true || prefs["SetupFinishedAllSteps"] != true {
		t.Fatalf("Setup completion preferences are missing: %#v", prefs)
	}
	var miniBuddy map[string]any
	if _, err := plist.Unmarshal(must(newData.ReadFile("mobile/Library/Preferences/com.apple.purplebuddy.notbackedup.plist")), &miniBuddy); err != nil {
		t.Fatal(err)
	}
	if miniBuddy["ShouldRunStandardMiniBuddy"] != false {
		t.Fatalf("mini-buddy marker is missing: %#v", miniBuddy)
	}
	var report map[string]any
	if err := json.Unmarshal(must(os.ReadFile(patch+".json")), &report); err != nil {
		t.Fatal(err)
	}
	if report["development_setup_complete"] != true || report["device_written"] != false {
		t.Fatalf("unexpected Setup patch report: %#v", report)
	}
}

func TestCOWUpdateRetainsStreamsAndCommitsLast(t *testing.T) {
	d := t.TempDir()
	path := filepath.Join(d, "fixture.apfs")
	f := must(os.Create(path))
	defer f.Close()
	content := bytes.Repeat([]byte("retained data"), 90000)
	attr := bytes.Repeat([]byte("large xattr"), 2000)
	root := &apfswrite.Entry{Mode: os.ModeDir | 0755, Children: []*apfswrite.Entry{
		{Name: "private", Mode: os.ModeDir | 0755, Children: []*apfswrite.Entry{{Name: "etc", Mode: os.ModeDir | 0755, Children: []*apfswrite.Entry{{Name: "fstab", Mode: 0644, Data: []byte("# fixture mount table\n")}}}}},
		{Name: "old", Mode: os.ModeSetuid | 0750, UID: 501, GID: 20, Data: content, LinkGroup: 42, Xattrs: map[string][]byte{"user.small": []byte("inline"), "user.large": attr}},
		{Name: "alias", Mode: os.ModeSetuid | 0750, UID: 501, GID: 20, Data: content, LinkGroup: 42, Xattrs: map[string][]byte{"user.small": []byte("inline"), "user.large": attr}},
		{Name: "link", Mode: os.ModeSymlink | 0777, Data: []byte("old")},
	}}
	group := parseUUID("299E8090-2570-403D-A7EF-FB5BED8B5758")
	opts := &apfswrite.CreateOptions{Volumes: []apfswrite.VolumeSpec{
		{Name: "System", Role: apfs.VolumeRoleSystem, CaseSensitive: true, VolumeGroupID: group, Root: root},
		{Name: "Data", Role: apfs.VolumeRoleData, CaseSensitive: true, VolumeGroupID: group, Root: &apfswrite.Entry{Mode: os.ModeDir | 0700, Children: []*apfswrite.Entry{{Name: "userdata", Mode: 0600, Data: []byte("Data survives")}}}},
	}}
	if err := apfswrite.CreateContainer(f, 1<<30, opts); err != nil {
		t.Fatal(err)
	}
	baseline := must(apfs.Open(f, nil))
	sys := must(baseline.VolumeBySelector("system"))
	old := must(sys.FileEntryByPath("old"))
	wantDataBlocks := uint64((len(content)+4095)/4096 + (len(attr)+4095)/4096 + 1)
	if sys.Superblock.TotalBlocksAllocated != wantDataBlocks {
		t.Fatalf("initial stream block counter: got %d want %d", sys.Superblock.TotalBlocksAllocated, wantDataBlocks)
	}
	originalBlock := must(old.RawDataStream()).PhysicalExtents[0].PhysicalBlockNumber
	originalID := must(sys.Identifier())
	baseline.Close()
	overlay := filepath.Join(d, "overlay")
	must(true, os.MkdirAll(filepath.Join(overlay, "usr/bin"), 0755))
	must(true, os.WriteFile(filepath.Join(overlay, "usr/bin/bash"), []byte("small overlay payload"), 0755))
	patch := filepath.Join(d, "update.delta")
	updatePlan([]string{"--source", path, "--overlay", overlay, "--output", patch})
	raw := must(os.ReadFile(patch))
	count := binary.LittleEndian.Uint64(raw[16:])
	var commit []byte
	primary := make([]byte, 4096)
	must(f.ReadAt(primary, 0))
	for i := uint64(0); i < count; i++ {
		r := raw[24+i*4104 : 24+(i+1)*4104]
		block := binary.LittleEndian.Uint64(r)
		if block == 0 {
			commit = append([]byte(nil), r[8:]...)
			continue
		}
		must(f.WriteAt(r[8:], int64(block)*4096))
	}
	must(true, f.Sync())
	unchanged := make([]byte, 4096)
	must(f.ReadAt(unchanged, 0))
	if !bytes.Equal(primary, unchanged) {
		t.Fatal("commit overwritten early")
	}
	// A power loss before publishing block zero still exposes the old filesystem.
	oldView := must(apfs.Open(f, nil))
	oldSys := must(oldView.VolumeBySelector("system"))
	if !bytes.Equal(must(oldSys.ReadFile("old")), content) {
		t.Fatal("old checkpoint damaged")
	}
	oldView.Close()
	checker := filepath.Join("..", "src", "apfsprogs", "apfsck", "apfsck")
	check := func() {
		t.Helper()
		if out, err := exec.Command(checker, "-c", "-w", path).CombinedOutput(); err != nil {
			t.Fatalf("apfsck: %v: %s", err, out)
		}
	}
	check()
	if len(commit) != 4096 {
		t.Fatal("missing primary commit")
	}
	must(f.WriteAt(commit, 0))
	must(true, f.Sync())
	check()
	result := must(apfs.Open(f, nil))
	defer result.Close()
	newSys := must(result.VolumeBySelector("system"))
	if result.Superblock.XPDataBlocks-result.Superblock.XPDataLen < result.Superblock.XPDataLen+1 {
		t.Fatal("COW update lost writable checkpoint reserve")
	}
	if newSys.Superblock.TotalBlocksAllocated != wantDataBlocks+1 || newSys.Superblock.TotalBlocksFreed != 0 {
		t.Fatalf("COW stream accounting lost retained extents or counted metadata: allocated=%d freed=%d", newSys.Superblock.TotalBlocksAllocated, newSys.Superblock.TotalBlocksFreed)
	}
	if must(newSys.Identifier()) != originalID {
		t.Fatal("volume identity changed")
	}
	if !bytes.Equal(must(newSys.ReadFile("old")), content) {
		t.Fatal("retained data changed")
	}
	if string(must(newSys.ReadFile("usr/bin/bash"))) != "small overlay payload" {
		t.Fatal("overlay missing")
	}
	e := must(newSys.FileEntryByPath("old"))
	alias := must(newSys.FileEntryByPath("alias"))
	if must(e.Identifier()) != must(alias.Identifier()) || must(e.OwnerIdentifier()) != 501 || must(e.GroupIdentifier()) != 20 || must(e.FileMode())&04000 == 0 {
		t.Fatal("hardlink or ownership/mode changed")
	}
	if must(e.RawDataStream()).PhysicalExtents[0].PhysicalBlockNumber != originalBlock {
		t.Fatal("data was copied instead of retained")
	}
	for i, n := 0, must(e.NumberOfExtendedAttributes()); i < n; i++ {
		a := must(e.ExtendedAttributeByIndex(i))
		name := must(a.UTF8Name())
		b := make([]byte, must(a.Size()))
		must(a.ReadAt(b, 0))
		if name == "user.large" && !bytes.Equal(b, attr) {
			t.Fatal("streamed xattr changed")
		}
		if name == "user.small" && string(b) != "inline" {
			t.Fatal("inline xattr changed")
		}
	}
	if string(must(must(result.VolumeBySelector("data")).ReadFile("userdata"))) != "Data survives" {
		t.Fatal("Data volume changed")
	}
	if len(raw) > 8<<20 {
		t.Fatal("small update copied original container")
	}
}

func TestCOWLayoutAddsRoleVolumeAndRemovesGroupingWithoutCopyingPayload(t *testing.T) {
	d := t.TempDir()
	path := filepath.Join(d, "fixture.apfs")
	f := must(os.Create(path))
	defer f.Close()
	group := parseUUID("299E8090-2570-403D-A7EF-FB5BED8B5758")
	root := &apfswrite.Entry{Mode: os.ModeDir | 0755, Children: []*apfswrite.Entry{
		{Name: "private", Mode: os.ModeDir | 0755, Children: []*apfswrite.Entry{{Name: "etc", Mode: os.ModeDir | 0755, Children: []*apfswrite.Entry{{Name: "fstab", Mode: 0644, Data: []byte("# retained\n")}}}}},
		{Name: "payload", Mode: 0644, Data: bytes.Repeat([]byte("original"), 8192)},
	}}
	opts := &apfswrite.CreateOptions{Volumes: []apfswrite.VolumeSpec{
		{Name: "System", Role: apfs.VolumeRoleSystem, VolumeGroupID: group, CaseSensitive: true, Root: root},
		{Name: "Data", Role: apfs.VolumeRoleData, VolumeGroupID: group, CaseSensitive: true},
	}}
	if err := apfswrite.CreateContainer(f, 1536<<20, opts); err != nil {
		t.Fatal(err)
	}
	before := must(apfs.Open(f, nil))
	old := must(before.VolumeBySelector("system"))
	id := must(old.Identifier())
	block := must(must(old.FileEntryByPath("payload")).RawDataStream()).PhysicalExtents[0].PhysicalBlockNumber
	before.Close()
	overlay := filepath.Join(d, "overlay")
	must(true, os.Mkdir(overlay, 0755))
	must(true, os.WriteFile(filepath.Join(overlay, "new"), []byte("file"), 0644))
	preboot := filepath.Join(d, "preboot")
	must(true, os.Mkdir(preboot, 0755))
	must(true, os.WriteFile(filepath.Join(preboot, "boot-policy"), []byte("original firmware policy"), 0644))
	config := filepath.Join(d, "volumes.json")
	must(true, os.WriteFile(config, []byte(fmt.Sprintf(`[{"name":"Preboot","role":"preboot","uuid":"11111111-2222-4333-8444-555555555555","overlay":%q}]`, preboot)), 0644))
	patch := filepath.Join(d, "layout.delta")
	updatePlan([]string{"--source", path, "--overlay", overlay, "--output", patch, "--volume-group", "00000000-0000-0000-0000-000000000000", "--add-volumes", config})
	second := filepath.Join(d, "cached-layout.delta")
	updatePlan([]string{"--source", path, "--overlay", overlay, "--output", second, "--volume-group", "00000000-0000-0000-0000-000000000000", "--add-volumes", config, "--source-identity", patch + ".json"})
	var originalReport, cachedReport map[string]any
	must(true, json.Unmarshal(must(os.ReadFile(patch+".json")), &originalReport))
	must(true, json.Unmarshal(must(os.ReadFile(second+".json")), &cachedReport))
	if originalReport["source_sha256"] != cachedReport["source_sha256"] || cachedReport["source_hash_reused_from"] != patch+".json" {
		t.Fatal("source identity was not bound/reused")
	}
	raw := must(os.ReadFile(patch))
	count := binary.LittleEndian.Uint64(raw[16:])
	for i := uint64(0); i < count; i++ {
		record := raw[24+i*4104 : 24+(i+1)*4104]
		must(f.WriteAt(record[8:], int64(binary.LittleEndian.Uint64(record))*4096))
	}
	after := must(apfs.Open(f, nil))
	defer after.Close()
	if must(after.NumberOfVolumes()) != 3 {
		t.Fatal("Preboot not added")
	}
	system := must(after.VolumeBySelector("system"))
	data := must(after.VolumeBySelector("data"))
	if must(system.Identifier()) != id || must(system.VolumeGroupIdentifier()) != ([16]byte{}) || must(data.VolumeGroupIdentifier()) != ([16]byte{}) {
		t.Fatal("wrong role/group identities")
	}
	if must(must(system.FileEntryByPath("payload")).RawDataStream()).PhysicalExtents[0].PhysicalBlockNumber != block {
		t.Fatal("payload was copied")
	}
	if string(must(must(after.VolumeBySelector("preboot")).ReadFile("boot-policy"))) != "original firmware policy" {
		t.Fatal("new volume payload missing")
	}
	verifyUpdate([]string{"--device", path, "--report", patch + ".json", "--output", filepath.Join(d, "verified.json")})
}

func TestCOWPlanReadsOnlyBoundedDeviceRange(t *testing.T) {
	d := t.TempDir()
	path := filepath.Join(d, "disk")
	f := must(os.Create(path))
	defer f.Close()
	const prefix = 65536
	const size = 1 << 30
	root := &apfswrite.Entry{Mode: os.ModeDir | 0755, Children: []*apfswrite.Entry{{Name: "private", Mode: os.ModeDir | 0755, Children: []*apfswrite.Entry{{Name: "etc", Mode: os.ModeDir | 0755, Children: []*apfswrite.Entry{{Name: "fstab", Mode: 0644, Data: []byte("# test\n")}}}}}}}
	opts := &apfswrite.CreateOptions{Volumes: []apfswrite.VolumeSpec{{Name: "System", Role: apfs.VolumeRoleSystem, CaseSensitive: true, Root: root}, {Name: "Data", Role: apfs.VolumeRoleData, CaseSensitive: true}}}
	if err := apfswrite.CreateContainer(io.NewOffsetWriter(f, prefix), size, opts); err != nil {
		t.Fatal(err)
	}
	sentinel := bytes.Repeat([]byte{0x5a}, prefix)
	must(f.WriteAt(sentinel, 0))
	overlay := filepath.Join(d, "overlay")
	must(true, os.Mkdir(overlay, 0755))
	must(true, os.WriteFile(filepath.Join(overlay, "new"), []byte("incremental"), 0644))
	output := filepath.Join(d, "bounded.delta")
	updatePlan([]string{"--source", path, "--source-offset", "65536", "--source-size", "1073741824", "--overlay", overlay, "--output", output})
	var report map[string]any
	if err := json.Unmarshal(must(os.ReadFile(output+".json")), &report); err != nil {
		t.Fatal(err)
	}
	if report["source_offset"] != float64(prefix) || report["container_bytes"] != float64(size) {
		t.Fatal("source range lost")
	}
	actual := make([]byte, prefix)
	must(f.ReadAt(actual, 0))
	if !bytes.Equal(actual, sentinel) {
		t.Fatal("source was modified")
	}
}
