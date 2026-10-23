package main

import (
	"bytes"
	"fmt"
	"github.com/deploymenttheory/go-apfs-v2/pkg/apfs"
	"github.com/deploymenttheory/go-apfs-v2/pkg/apfswrite"
	"os"
	"os/exec"
	"path/filepath"
	"strings"
	"testing"

	"howett.net/plist"
)

func TestInitialPersonaUsesNativeVersionOneSchema(t *testing.T) {
	data := &apfswrite.Entry{Mode: os.ModeDir | 0755}
	s := stats{}
	if !seedInitialPersona(data, &s) {
		t.Fatal("missing initial state was not created")
	}
	directory := child(data, "keybags")
	file := child(directory, "persona.kb")
	if directory.Mode.Perm() != 0700 || directory.UID != 0 || file.Mode.Perm() != 0600 || file.UID != 0 || !file.ModeExplicit {
		t.Fatal("persona metadata has incorrect ownership or permissions")
	}
	var metadata struct {
		Version    uint64         `plist:"UsePersonaManifestVersion"`
		Generation uint64         `plist:"UsePersonaGenerationID"`
		Users      map[string]any `plist:"UserPersonaDictionary"`
	}
	if _, err := plist.Unmarshal(file.Data, &metadata); err != nil {
		t.Fatal(err)
	}
	if metadata.Version != 1 || metadata.Generation != 1 || metadata.Users == nil || len(metadata.Users) != 0 {
		t.Fatalf("invalid initial manifest: %#v", metadata)
	}
	if s.Entries != 2 || s.Bytes != 4096 {
		t.Fatal("seed was not included in image allocation accounting")
	}
}

func TestInitialPersonaPreservesExistingState(t *testing.T) {
	file := &apfswrite.Entry{Name: "persona.kb", Mode: 0600, Data: []byte("existing native state")}
	directory := &apfswrite.Entry{Name: "keybags", Mode: os.ModeDir | 0700, Children: []*apfswrite.Entry{file}}
	data := &apfswrite.Entry{Children: []*apfswrite.Entry{directory}}
	s := stats{}
	if seedInitialPersona(data, &s) || child(directory, "persona.kb") != file || !bytes.Equal(file.Data, []byte("existing native state")) || s != (stats{}) {
		t.Fatal("existing native metadata was replaced or counted again")
	}
}

func TestInitialPersonaRejectsUnsafeDirectory(t *testing.T) {
	for _, entry := range []*apfswrite.Entry{
		{Name: "keybags", Mode: 0644},
		{Name: "keybags", Mode: os.ModeDir | 0777},
		{Name: "keybags", Mode: os.ModeDir | 0700, UID: 501},
	} {
		func() {
			defer func() {
				if recover() == nil {
					t.Fatal("accepted an invalid initial-state directory")
				}
			}()
			seedInitialPersona(&apfswrite.Entry{Children: []*apfswrite.Entry{entry}}, &stats{})
		}()
	}
}

func TestSetupCompletionSeedsAndMergesPurpleBuddyPreferences(t *testing.T) {
	data := &apfswrite.Entry{Mode: os.ModeDir | 0755}
	s := stats{}
	seedSetupCompletion(data, &s)
	mobile := child(data, "mobile")
	library := child(mobile, "Library")
	prefs := child(library, "Preferences")
	if mobile.UID != 501 || library.UID != 501 || prefs.UID != 501 {
		t.Fatal("new mobile preference directories have incorrect ownership")
	}
	file := child(prefs, "com.apple.purplebuddy.plist")
	if file.UID != 501 || file.GID != 501 || file.Mode.Perm() != 0600 || !file.ModeExplicit {
		t.Fatal("PurpleBuddy preference has incorrect ownership or permissions")
	}
	var values map[string]any
	if _, err := plist.Unmarshal(file.Data, &values); err != nil {
		t.Fatal(err)
	}
	if values["SetupDone"] != true || values["SetupFinishedAllSteps"] != true ||
		fmt.Sprint(values["SetupVersion"]) != "11" || values["daemonizeNeedsToRun"] != false {
		t.Fatalf("unexpected PurpleBuddy completion plist: %#v", values)
	}
	miniBuddy := child(prefs, "com.apple.purplebuddy.notbackedup.plist")
	var miniValues map[string]any
	if _, err := plist.Unmarshal(miniBuddy.Data, &miniValues); err != nil {
		t.Fatal(err)
	}
	if len(miniValues) != 7 {
		t.Fatalf("unexpected mini-buddy settings: %#v", miniValues)
	}
	for key, value := range miniValues {
		if value != false {
			t.Fatalf("mini-buddy flag %s is not disabled", key)
		}
	}
	if s.Entries != 5 || s.Bytes != 8192 {
		t.Fatalf("Setup preferences were not included in APFS allocation accounting: %#v", s)
	}
	before := s
	seedSetupCompletion(data, &s)
	if s != before {
		t.Fatalf("repeated seeding changed allocation accounting: before=%#v after=%#v", before, s)
	}
}

func TestSetupCompletionPreservesUnrelatedPurpleBuddyPreferences(t *testing.T) {
	encoded, err := plist.Marshal(map[string]any{"preserved": "existing"}, plist.BinaryFormat)
	if err != nil {
		t.Fatal(err)
	}
	file := &apfswrite.Entry{Name: "com.apple.purplebuddy.plist", Mode: 0600,
		ModeExplicit: true, UID: 501, GID: 501, Data: encoded}
	prefs := &apfswrite.Entry{Name: "Preferences", Mode: os.ModeDir | 0755,
		Children: []*apfswrite.Entry{file}}
	library := &apfswrite.Entry{Name: "Library", Mode: os.ModeDir | 0711,
		Children: []*apfswrite.Entry{prefs}}
	mobile := &apfswrite.Entry{Name: "mobile", Mode: os.ModeDir | 0755,
		Children: []*apfswrite.Entry{library}}
	data := &apfswrite.Entry{Mode: os.ModeDir | 0755, Children: []*apfswrite.Entry{mobile}}
	s := stats{Entries: 1, Bytes: uint64(len(encoded)+4095) &^ 4095}
	seedSetupCompletion(data, &s)
	updated := child(child(child(data, "mobile"), "Library"), "Preferences")
	var values map[string]any
	if _, err := plist.Unmarshal(child(updated, "com.apple.purplebuddy.plist").Data, &values); err != nil {
		t.Fatal(err)
	}
	if values["preserved"] != "existing" || values["SetupDone"] != true {
		t.Fatalf("existing preference keys were not merged: %#v", values)
	}
}

func TestHashCollisionWithDifferentNameLengthsPassesIndependentChecker(t *testing.T) {
	checker := filepath.Join("..", "src", "apfsprogs", "apfsck", "apfsck")
	if _, err := os.Stat(checker); err != nil {
		t.Skip("build apfsck to run independent validation")
	}
	hashes := map[uint32]string{}
	var a, b string
	for i := 0; i < 100000; i++ {
		name := fmt.Sprintf("%0*s%d", i%23, "x", i)
		h := apfs.CalculateNameHash([]byte(name), false)
		if prev, ok := hashes[h]; ok && len(prev) != len(name) && ((prev < name) != (len(prev) < len(name))) {
			a, b = prev, name
			break
		}
		hashes[h] = name
	}
	if a == "" {
		t.Fatal("no deterministic collision found")
	}
	t.Logf("collision: %q / %q", a, b)
	path := filepath.Join(t.TempDir(), "collision.apfs")
	f, err := os.Create(path)
	if err != nil {
		t.Fatal(err)
	}
	err = apfswrite.CreateContainer(f, 1024*1024*1024, &apfswrite.CreateOptions{CaseSensitive: true, RootFiles: []apfswrite.RootFile{{Name: a, Data: []byte("first")}, {Name: b, Data: []byte("second")}}})
	f.Close()
	if err != nil {
		t.Fatal(err)
	}
	if out, err := exec.Command(checker, "-c", "-w", path).CombinedOutput(); err != nil {
		t.Fatalf("independent checker: %v: %s", err, out)
	}
}

func TestFirmwareImportRetainsMetadataAndDataRoot(t *testing.T) {
	f, err := os.Create(filepath.Join(t.TempDir(), "source.apfs"))
	if err != nil {
		t.Fatal(err)
	}
	defer f.Close()
	root := &apfswrite.Entry{Mode: os.ModeDir | 0755, Children: []*apfswrite.Entry{
		{Name: "private", Mode: os.ModeDir | 0755, Children: []*apfswrite.Entry{
			{Name: "var", Mode: os.ModeDir | 0755, Children: []*apfswrite.Entry{{Name: "owned", Mode: 0600, UID: 501, GID: 20, Data: []byte("private data"), Xattrs: map[string][]byte{"user.test": []byte("attribute")}}}},
		}},
		{Name: "one", Mode: os.ModeSetuid | 0755, UID: 0, GID: 0, Data: []byte("linked"), LinkGroup: 42},
		{Name: "two", Mode: os.ModeSetuid | 0755, Data: []byte("linked"), LinkGroup: 42},
		{Name: "var", Mode: os.ModeSymlink | 0777, Data: []byte("private/var")},
	}}
	if err := apfswrite.CreateContainer(f, 1024*1024*1024, &apfswrite.CreateOptions{Root: root}); err != nil {
		t.Fatal(err)
	}
	c := must(apfs.Open(f, nil))
	defer c.Close()
	v := must(c.Volume(0))
	s := stats{}
	imported := copyEntry(must(v.RootDirectory()), "", &s, map[uint64]*apfswrite.Entry{})
	if child(imported, "one").LinkGroup != child(imported, "two").LinkGroup {
		t.Fatal("hard links lost")
	}
	if child(imported, "one").Mode&os.ModeSetuid == 0 {
		t.Fatal("setuid lost")
	}
	owned := child(child(child(imported, "private"), "var"), "owned")
	if owned.UID != 501 || owned.GID != 20 {
		t.Fatal("ownership lost")
	}
	b := make([]byte, 12)
	if _, err := owned.DataValue.ReadAt(b, 0); err != nil {
		t.Fatal(err)
	}
	if string(b) != "private data" {
		t.Fatal("content lost")
	}
	if string(child(imported, "var").Data) != "private/var" {
		t.Fatal("symlink lost")
	}
	a := make([]byte, 9)
	if _, err := owned.XattrValues["user.test"].ReadAt(a, 0); err != nil {
		t.Fatal(err)
	}
	if string(a) != "attribute" {
		t.Fatal("xattr lost")
	}
}

func TestFstabRetainsOtherMountsAndIsIdempotent(t *testing.T) {
	original := "# firmware\n/dev/disk0s1s1 / apfs ro 0 1\n/dev/disk0s1s2 /private/var apfs rw 0 2\n/dev/disk0s1s3 /private/preboot apfs rw 0 0\n"
	result := fstab(original, "SYSTEM", "DATA")
	if strings.Contains(result, "disk0s1s1") || strings.Contains(result, "disk0s1s2") || !strings.Contains(result, "/private/preboot") {
		t.Fatal(result)
	}
	if fstab(result, "SYSTEM", "DATA") != result {
		t.Fatal("not idempotent")
	}
}

func TestMinimumFreeSpaceIncludesMetadata(t *testing.T) {
	opts := &apfswrite.CreateOptions{RootFiles: []apfswrite.RootFile{{Name: "firmware", Data: make([]byte, 1024*1024)}}, MinimumFreeBytes: 1024 * 1024 * 1024}
	if _, err := apfswrite.PlanContainer(1024*1024*1024, opts); err == nil {
		t.Fatal("accepted full-size free-space demand despite allocated files/metadata")
	}
	// Two MiB of headroom cannot also hold writable checkpoint rings. The
	// actual free-space promise must account for that reserve on every build.
	if _, err := apfswrite.PlanContainer(1026*1024*1024, opts); err == nil {
		t.Fatal("checkpoint reserve was counted as free space")
	}
	if _, err := apfswrite.PlanContainer(1032*1024*1024, opts); err != nil {
		t.Fatal(err)
	}
}
