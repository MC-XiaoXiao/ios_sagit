package main

import (
	"bytes"
	"os"
	"path/filepath"
	"testing"

	"github.com/deploymenttheory/go-apfs-v2/pkg/apfs"
	"github.com/deploymenttheory/go-apfs-v2/pkg/apfswrite"
	"howett.net/plist"
)

func TestDataSetupPathCopyPreservesPreferencesAndOtherVolumes(t *testing.T) {
	d := t.TempDir()
	path := filepath.Join(d, "data.apfs")
	f := must(os.Create(path))
	defer f.Close()
	system := &apfswrite.Entry{Mode: os.ModeDir | 0755}
	etc := dir(dir(system, "private"), "etc")
	etc.Children = append(etc.Children, &apfswrite.Entry{Name: "fstab", Mode: 0644, Data: []byte("# preserved\n")})
	data := &apfswrite.Entry{Mode: os.ModeDir | 0755}
	prefs := dir(dir(dir(data, "mobile"), "Library"), "Preferences")
	original := must(plist.Marshal(map[string]any{"preserved": "native", "SetupDone": false}, plist.BinaryFormat))
	prefs.Children = append(prefs.Children, &apfswrite.Entry{
		Name: "com.apple.purplebuddy.plist", Mode: 0644, ModeExplicit: true,
		UID: 501, GID: 501, Data: original,
	})
	data.Children = append(data.Children, &apfswrite.Entry{Name: "untouched", Mode: 0600, Data: []byte("retained data")})
	must(true, apfswrite.CreateContainer(f, 1<<30, &apfswrite.CreateOptions{Volumes: []apfswrite.VolumeSpec{
		{Name: "System", Role: apfs.VolumeRoleSystem, CaseSensitive: true, Root: system},
		{Name: "Data", Role: apfs.VolumeRoleData, CaseSensitive: true, Root: data},
	}}))
	patch := filepath.Join(d, "setup.delta")
	dataUpdatePlan([]string{"--source", path, "--development-setup-complete", "--output", patch})
	raw := must(os.ReadFile(patch))
	count := nu64(raw[16:])
	for i := uint64(1); i < count; i++ {
		record := raw[24+i*4104 : 24+(i+1)*4104]
		must(f.WriteAt(record[8:], int64(nu64(record))*4096))
	}
	must(true, f.Sync())
	checkAPFS(t, path)
	before := must(apfs.Open(f, nil))
	if !bytes.Equal(must(must(before.VolumeBySelector("data")).ReadFile("mobile/Library/Preferences/com.apple.purplebuddy.plist")), original) {
		t.Fatal("old checkpoint changed before commit")
	}
	before.Close()
	must(f.WriteAt(raw[32:4128], 0))
	must(true, f.Sync())
	checkAPFS(t, path)
	actual := must(apfs.Open(f, nil))
	defer actual.Close()
	v := must(actual.VolumeBySelector("data"))
	for name, key := range map[string]string{
		"com.apple.purplebuddy.plist":             "SetupDone",
		"com.apple.purplebuddy.notbackedup.plist": "ShouldRunStandardMiniBuddy",
	} {
		relative := "mobile/Library/Preferences/" + name
		entry := must(v.FileEntryByPath(relative))
		if must(entry.OwnerIdentifier()) != 501 || must(entry.GroupIdentifier()) != 501 || must(entry.FileMode()) != 0100600 {
			t.Fatal("mobile preference metadata changed")
		}
		var values map[string]any
		must(plist.Unmarshal(must(v.ReadFile(relative)), &values))
		if name == "com.apple.purplebuddy.plist" {
			if values[key] != true || values["SetupFinishedAllSteps"] != true || values["preserved"] != "native" {
				t.Fatalf("native preferences were not merged: %#v", values)
			}
		} else if values[key] != false {
			t.Fatal("missing mini-buddy completion marker")
		}
	}
	if string(must(v.ReadFile("untouched"))) != "retained data" {
		t.Fatal("unrelated Data file changed")
	}
	verifyUpdate([]string{"--device", path, "--report", patch + ".json", "--output", filepath.Join(d, "verified.json")})
}
