package main

import (
	"crypto/sha256"
	"encoding/binary"
	"encoding/hex"
	"encoding/json"
	"flag"
	"fmt"
	"github.com/deploymenttheory/go-apfs-v2/pkg/apfs"
	"github.com/deploymenttheory/go-apfs-v2/pkg/apfswrite"
	"github.com/deploymenttheory/go-apfs-v2/pkg/appledouble"
	"github.com/deploymenttheory/go-apfs-v2/pkg/osversion"
	"howett.net/plist"
	"io"
	"os"
	"path/filepath"
	"sort"
)

var reuseInput bool

type addedVolume struct {
	Name    string `json:"name"`
	Role    string `json:"role"`
	UUID    string `json:"uuid"`
	Overlay string `json:"overlay"`
}

type volumeIdentity struct {
	Name      string `json:"name"`
	Role      uint16 `json:"role"`
	UUID      string `json:"uuid"`
	GroupUUID string `json:"volume_group_uuid"`
}

type reusedValue struct {
	value
	block, blocks uint64
}

func (v reusedValue) ExistingExtent() (uint64, uint64, bool) { return v.block, v.blocks, true }
func sourceValue(reader io.ReaderAt, size int64) appledouble.Value {
	v := value{reader, size}
	if !reuseInput || size == 0 {
		return v
	}
	var stream *apfs.DataStream
	switch r := reader.(type) {
	case *apfs.DataStream:
		stream = r
	case *apfs.ExtendedAttribute:
		stream = must(r.RawDataStream())
		if stream != nil && len(stream.PhysicalExtents) == 0 {
			return v
		}
	}
	if stream == nil || len(stream.PhysicalExtents) != 1 {
		panic("full-tree COW update requires single-extent unencrypted source streams; use system-update-plan for System file patches on native Data")
	}
	extent := stream.PhysicalExtents[0]
	blocks := (uint64(size) + 4095) / 4096
	if extent.LogicalOffset != 0 || extent.EncryptionIdentifier != 0 || extent.DataSize != blocks*4096 {
		panic("COW update rejects sparse/encrypted/noncanonical extents")
	}
	return reusedValue{v, extent.PhysicalBlockNumber, blocks}
}

type blockDelta struct {
	base   io.ReaderAt
	blocks map[uint64][]byte
	size   int64
}

func (d *blockDelta) WriteAt(p []byte, off int64) (int, error) {
	if off < 0 || off%4096 != 0 || len(p)%4096 != 0 || int64(len(p)) > d.size-off {
		return 0, fmt.Errorf("unaligned/out-of-bounds delta write")
	}
	for i := 0; i < len(p); i += 4096 {
		d.blocks[uint64(off+int64(i))/4096] = append([]byte(nil), p[i:i+4096]...)
	}
	return len(p), nil
}
func (d *blockDelta) ReadAt(p []byte, off int64) (int, error) {
	if off < 0 || off >= d.size {
		return 0, io.EOF
	}
	n := len(p)
	if int64(n) > d.size-off {
		n = int(d.size - off)
	}
	for copied := 0; copied < n; {
		pos := off + int64(copied)
		block := uint64(pos) / 4096
		within := int(pos % 4096)
		length := min(n-copied, 4096-within)
		if data := d.blocks[block]; data != nil {
			copy(p[copied:copied+length], data[within:within+length])
		} else {
			got, err := d.base.ReadAt(p[copied:copied+length], pos)
			if got != length {
				return copied + got, err
			}
		}
		copied += length
	}
	if n < len(p) {
		return n, io.EOF
	}
	return n, nil
}
func (d *blockDelta) save(path string) {
	f := must(os.OpenFile(path, os.O_CREATE|os.O_EXCL|os.O_WRONLY, 0600))
	defer f.Close()
	keys := make([]uint64, 0, len(d.blocks))
	for k := range d.blocks {
		keys = append(keys, k)
	}
	sort.Slice(keys, func(i, j int) bool { return keys[i] < keys[j] })
	header := make([]byte, 24)
	copy(header, []byte("APFSCOW1"))
	binary.LittleEndian.PutUint64(header[8:], uint64(d.size))
	binary.LittleEndian.PutUint64(header[16:], uint64(len(keys)))
	must(f.Write(header))
	for _, k := range keys {
		var b [8]byte
		binary.LittleEndian.PutUint64(b[:], k)
		must(f.Write(b[:]))
		must(f.Write(d.blocks[k]))
	}
	if err := f.Sync(); err != nil {
		panic(err)
	}
}
func freeTail(c *apfs.Container, reader io.ReaderAt) (uint64, uint64) {
	sm := apfs.NewSpaceManager()
	p := must(c.CheckpointMap.PhysicalAddressByObjectIdentifier(c.Superblock.SpacemanOID))
	raw := make([]byte, 4096)
	must(reader.ReadAt(raw, int64(p)*4096))
	checkObject := func(raw []byte) {
		if must(apfs.CalculateFletcher64(raw[8:], 0)) != binary.LittleEndian.Uint64(raw) {
			panic("COW allocation metadata checksum mismatch")
		}
	}
	checkObject(raw)
	if err := sm.ReadData(raw); err != nil {
		panic(err)
	}
	dev := sm.Devices[0]
	if dev.CABCount != 0 || dev.BlockCount != c.Superblock.NumberOfBlocks {
		panic("unsupported COW space-manager geometry")
	}
	var highest, next, totalFree uint64
	for i := uint32(0); i < dev.CIBCount; i++ {
		off := uint64(dev.AddrOffset) + uint64(i)*8
		if off+8 > 4096 {
			panic("CIB address outside spaceman")
		}
		cib := make([]byte, 4096)
		must(reader.ReadAt(cib, int64(binary.LittleEndian.Uint64(raw[off:]))*4096))
		checkObject(cib)
		count := binary.LittleEndian.Uint32(cib[36:])
		if uint64(count)*32+40 > 4096 {
			panic("invalid CIB")
		}
		for j := uint32(0); j < count; j++ {
			info := cib[40+32*j:]
			start := binary.LittleEndian.Uint64(info[8:])
			blocks := binary.LittleEndian.Uint32(info[16:])
			bmap := binary.LittleEndian.Uint64(info[24:])
			free := binary.LittleEndian.Uint32(info[20:])
			if start != next || blocks == 0 || blocks > 32768 || uint64(blocks) > dev.BlockCount-start || free > blocks {
				panic("invalid COW chunk range")
			}
			next += uint64(blocks)
			totalFree += uint64(free)
			if bmap == 0 {
				if free != blocks {
					panic("missing allocation bitmap for used chunk")
				}
				continue
			}
			bits := make([]byte, 4096)
			must(reader.ReadAt(bits, int64(bmap)*4096))
			var used uint32
			for k := uint32(0); k < blocks; k++ {
				if bits[k/8]&(1<<(k%8)) != 0 {
					used++
					if start+uint64(k)+1 > highest {
						highest = start + uint64(k) + 1
					}
				}
			}
			if used != blocks-free {
				panic("COW bitmap and free count disagree")
			}
		}
	}
	if next != dev.BlockCount || totalFree != dev.FreeCount {
		panic("COW allocation totals disagree")
	}
	return highest, dev.FreeCount
}
func updatePlan(args []string) {
	flags := flag.NewFlagSet("update-plan", flag.ExitOnError)
	source := flags.String("source", "", "existing raw APFS container")
	sourceOffset := flags.Int64("source-offset", 0, "container byte offset within a UAS device")
	sourceSize := flags.Int64("source-size", 0, "container byte length; required for block devices")
	overlay := flags.String("overlay", "", "System overlay directory")
	setupComplete := flags.Bool("development-setup-complete", false, "seed Setup Assistant completion preferences in Data for this development port")
	group := flags.String("volume-group", "", "explicit System/Data group UUID; zero UUID removes macOS grouping")
	addVolumes := flags.String("add-volumes", "", "JSON array of new volume names, roles, UUIDs and optional overlay directories")
	sourceIdentity := flags.String("source-identity", "", "reuse a prior plan's source hash with identical geometry/superblock; validation and installation still hash the complete source")
	output := flags.String("output", "", "small block delta, never a new container image")
	flags.Parse(args)
	if *source == "" || *output == "" || (*overlay == "" && !*setupComplete) {
		panic("update-plan requires source, output, and an overlay or --development-setup-complete")
	}
	f := must(os.Open(*source))
	defer f.Close()
	info := must(f.Stat())
	size := *sourceSize
	if size == 0 && info.Mode().IsRegular() {
		size = info.Size() - *sourceOffset
	}
	if *sourceOffset < 0 || *sourceOffset%4096 != 0 || size < 4096 || size%4096 != 0 ||
		*sourceOffset > int64(^uint64(0)>>1)-size || (info.Mode().IsRegular() && *sourceOffset+size > info.Size()) {
		panic("invalid source container range")
	}
	reader := io.NewSectionReader(f, *sourceOffset, size)
	c := must(apfs.Open(reader, nil))
	defer c.Close()
	if c.IOHandle.BlockSize != 4096 || c.Superblock.NumberOfBlocks*4096 != uint64(size) {
		panic("update requires a raw 4096-byte APFS container")
	}
	if c.Superblock.FusionUUID != ([16]byte{}) || c.Superblock.ReadOnlyCompatibleFeaturesFlags != 0 {
		panic("COW update rejects Fusion and unknown container features")
	}
	start, oldFree := freeTail(c, reader)
	stats := stats{}
	reuseInput = true
	defer func() { reuseInput = false }()
	specs := []apfswrite.VolumeSpec{}
	roles := map[uint16]bool{}
	identities := map[[16]byte]bool{}
	for i, count := 0, must(c.NumberOfVolumes()); i < count; i++ {
		v := must(c.Volume(i))
		if v.Superblock.FormattedByString() != "go-apfs (apfswrite)" ||
			must(v.NumberOfSnapshots()) != 0 || must(v.IsEncrypted()) ||
			v.Superblock.ReadOnlyCompatibleFeaturesFlags != 0 ||
			v.Superblock.IncompatibleFeaturesFlags != apfs.VolumeIncompatNormalizationInsens {
			panic("COW update requires unencrypted, unsealed, case-sensitive volumes without snapshots or unknown features")
		}
		root := copyEntry(must(v.RootDirectory()), "", &stats, map[uint64]*apfswrite.Entry{})
		role := v.Superblock.Role
		if role == apfs.VolumeRoleSystem && *overlay != "" {
			addOverlay(root, *overlay, &stats)
		}
		if role == apfs.VolumeRoleData && *setupComplete {
			seedSetupCompletion(root, &stats)
		}
		groupID := must(v.VolumeGroupIdentifier())
		if *group != "" && (role == apfs.VolumeRoleSystem || role == apfs.VolumeRoleData) {
			groupID = parseUUID(*group)
		}
		id := must(v.Identifier())
		roles[role] = true
		identities[id] = true
		specs = append(specs, apfswrite.VolumeSpec{Name: must(v.UTF8Name()), UUID: id, Role: role, VolumeGroupID: groupID, CaseSensitive: true, Root: root})
	}
	if !roles[apfs.VolumeRoleSystem] || !roles[apfs.VolumeRoleData] {
		panic("update requires System and Data volumes")
	}
	if *addVolumes != "" {
		var additions []addedVolume
		if err := json.Unmarshal(must(os.ReadFile(*addVolumes)), &additions); err != nil {
			panic(err)
		}
		for _, addition := range additions {
			role := must(apfs.ParseVolumeRole(addition.Role))
			id := parseUUID(addition.UUID)
			if role == 0 || roles[role] || id == ([16]byte{}) || identities[id] || addition.Name == "" {
				panic("new volume must have a unique role and nonzero UUID/name")
			}
			root := &apfswrite.Entry{Mode: os.ModeDir | 0755}
			if addition.Overlay != "" {
				addOverlay(root, addition.Overlay, &stats)
			}
			specs = append(specs, apfswrite.VolumeSpec{Name: addition.Name, UUID: id, Role: role, CaseSensitive: true, Root: root})
			roles[role] = true
			identities[id] = true
		}
	}
	opts := &apfswrite.CreateOptions{TargetVersion: osversion.Version{Major: 15}, ContainerUUID: c.Superblock.UUID, Volumes: specs, MetadataStartBlock: start, TransactionID: c.Superblock.XID + 1}
	delta := &blockDelta{base: reader, blocks: map[uint64][]byte{}, size: size}
	if err := apfswrite.CreateContainer(delta, size, opts); err != nil {
		panic(err)
	}
	// All noncommit writes must occupy the previously verified free tail.
	for block := range delta.blocks {
		if block != 0 && block < start {
			panic("COW plan would overwrite old live blocks")
		}
	}
	actual := must(apfs.Open(delta, nil))
	defer actual.Close()
	delta.save(*output)
	before := make([]byte, 4096)
	must(reader.ReadAt(before, 0))
	fingerprint := sha256.Sum256(before)
	files := map[string]any{}
	newSystem := must(actual.VolumeBySelector("system"))
	if *overlay != "" {
		err := filepath.WalkDir(*overlay, func(path string, entry os.DirEntry, err error) error {
			if err != nil {
				return err
			}
			if entry.IsDir() {
				return nil
			}
			rel := filepath.ToSlash(must(filepath.Rel(*overlay, path)))
			if entry.Type()&os.ModeSymlink != 0 {
				target := must(os.Readlink(path))
				if must(must(newSystem.FileEntryByPath(rel)).SymbolicLinkTarget()) != target {
					panic("overlay symlink mismatch")
				}
				files[rel] = map[string]string{"symlink": target}
			} else {
				want := hash(path)
				got := sha256.Sum256(must(newSystem.ReadFile(rel)))
				if hex.EncodeToString(got[:]) != want {
					panic("overlay contents mismatch")
				}
				files[rel] = map[string]string{"sha256": want}
			}
			return nil
		})
		if err != nil {
			panic(err)
		}
	}
	_, newFree := freeTail(actual, delta)
	sourceHash := ""
	if *sourceIdentity != "" {
		var identity struct {
			SHA256     string `json:"source_sha256"`
			Superblock string `json:"source_superblock_sha256"`
			Bytes      int64  `json:"container_bytes"`
			Offset     int64  `json:"source_offset"`
		}
		if err := json.Unmarshal(must(os.ReadFile(*sourceIdentity)), &identity); err != nil {
			panic(err)
		}
		if identity.Bytes != size || identity.Offset != *sourceOffset || identity.Superblock != hex.EncodeToString(fingerprint[:]) || len(must(hex.DecodeString(identity.SHA256))) != 32 {
			panic("cached source identity does not match container geometry/superblock")
		}
		sourceHash = identity.SHA256
	} else {
		h := sha256.New()
		must(io.CopyBuffer(h, io.NewSectionReader(reader, 0, size), make([]byte, 4<<20)))
		sourceHash = hex.EncodeToString(h.Sum(nil))
	}
	report := map[string]any{"schema": 1, "source": *source, "source_offset": *sourceOffset, "source_sha256": sourceHash, "delta_sha256": hash(*output), "source_superblock_sha256": hex.EncodeToString(fingerprint[:]), "container_bytes": size, "metadata_start_block": start, "old_free_bytes": oldFree * 4096, "new_free_bytes": newFree * 4096, "changed_blocks": len(delta.blocks), "patch_bytes": 24 + len(delta.blocks)*4104, "transaction_id": c.Superblock.XID + 1, "commit_block": 0, "overlay": *overlay, "overlay_files": files, "development_setup_complete": *setupComplete, "device_written": false}
	if *setupComplete {
		data := must(actual.VolumeBySelector("data"))
		var prefs map[string]any
		if _, err := plist.Unmarshal(must(data.ReadFile("mobile/Library/Preferences/com.apple.purplebuddy.plist")), &prefs); err != nil {
			panic(err)
		}
		if prefs["SetupDone"] != true || prefs["SetupFinishedAllSteps"] != true {
			panic("Setup completion preferences missing from planned Data volume")
		}
		report["setup_completion_files"] = []string{
			"mobile/Library/Preferences/com.apple.purplebuddy.plist",
			"mobile/Library/Preferences/com.apple.purplebuddy.notbackedup.plist",
		}
	}
	if *sourceIdentity != "" {
		report["source_hash_reused_from"] = *sourceIdentity
	}
	volumeIDs := []volumeIdentity{}
	for _, spec := range specs {
		volumeIDs = append(volumeIDs, volumeIdentity{spec.Name, spec.Role, uuidString(spec.UUID), uuidString(spec.VolumeGroupID)})
		if spec.Role == apfs.VolumeRoleSystem {
			report["system_uuid"] = uuidString(spec.UUID)
			report["volume_group_uuid"] = uuidString(spec.VolumeGroupID)
		} else if spec.Role == apfs.VolumeRoleData {
			report["data_uuid"] = uuidString(spec.UUID)
		}
	}
	report["volumes"] = volumeIDs
	originalFstab := must(must(c.VolumeBySelector("system")).ReadFile("private/etc/fstab"))
	if string(must(newSystem.ReadFile("private/etc/fstab"))) != string(originalFstab) {
		panic("fstab changed unexpectedly")
	}
	report["fstab"] = string(originalFstab)
	writeJSON(*output+".json", report)
	fmt.Printf("COW delta: %d blocks, %d bytes; original data extents retained\n", len(delta.blocks), 24+len(delta.blocks)*4104)
}

// Verify the actual UAS device using a bounded container view, not the plan's
// virtual reader. This checks every overlay file and the original mount IDs.
func verifyUpdate(args []string) {
	flags := flag.NewFlagSet("verify-update", flag.ExitOnError)
	device := flags.String("device", "", "device or read-only composed view")
	offset := flags.Int64("offset", 0, "container byte offset")
	reportPath := flags.String("report", "", "COW plan JSON")
	output := flags.String("output", "", "verification JSON")
	flags.Parse(args)
	if *device == "" || *reportPath == "" || *output == "" || *offset < 0 {
		panic("missing verify-update arguments")
	}
	var report struct {
		ContainerBytes int64             `json:"container_bytes"`
		SystemUUID     string            `json:"system_uuid"`
		DataUUID       string            `json:"data_uuid"`
		GroupUUID      string            `json:"volume_group_uuid"`
		Fstab          string            `json:"fstab"`
		Free           uint64            `json:"new_free_bytes"`
		Volumes        []volumeIdentity  `json:"volumes"`
		Preserved      map[string]string `json:"preserved_volume_superblocks"`
		OverlayVolume  string            `json:"overlay_volume"`
		Files          map[string]struct {
			SHA256  string `json:"sha256"`
			Symlink string `json:"symlink"`
		} `json:"overlay_files"`
	}
	if err := json.Unmarshal(must(os.ReadFile(*reportPath)), &report); err != nil {
		panic(err)
	}
	if report.ContainerBytes < 4096 || report.SystemUUID == "" || report.DataUUID == "" || report.GroupUUID == "" || len(report.Files) == 0 {
		panic("incomplete verification report")
	}
	f := must(os.Open(*device))
	defer f.Close()
	reader := io.NewSectionReader(f, *offset, report.ContainerBytes)
	c := must(apfs.Open(reader, nil))
	defer c.Close()
	expectedCount := len(report.Volumes)
	if expectedCount == 0 {
		expectedCount = 2
	}
	if must(c.NumberOfVolumes()) != expectedCount {
		panic("volume count mismatch")
	}
	for _, expected := range report.Volumes {
		var actual *apfs.Volume
		for i := 0; i < expectedCount; i++ {
			v := must(c.Volume(i))
			if uuidString(must(v.Identifier())) == expected.UUID {
				actual = v
				break
			}
		}
		if actual == nil || actual.Superblock.Role != expected.Role || must(actual.UTF8Name()) != expected.Name || uuidString(must(actual.VolumeGroupIdentifier())) != expected.GroupUUID {
			panic("volume identity/role mismatch")
		}
		must(actual.RootDirectory())
		if expectedHash := report.Preserved[expected.UUID]; expectedHash != "" {
			p := must(c.ObjectMapBTree.PhysicalAddressForOID(reader, actual.Superblock.OID, c.Superblock.XID))
			block := make([]byte, 4096)
			must(reader.ReadAt(block, int64(p)*4096))
			sum := sha256.Sum256(block)
			if hex.EncodeToString(sum[:]) != expectedHash {
				panic("preserved volume superblock changed: " + expected.Name)
			}
		}
	}
	sys := must(c.VolumeBySelector("system"))
	data := must(c.VolumeBySelector("data"))
	if uuidString(must(sys.Identifier())) != report.SystemUUID || uuidString(must(data.Identifier())) != report.DataUUID || uuidString(must(sys.VolumeGroupIdentifier())) != report.GroupUUID || uuidString(must(data.VolumeGroupIdentifier())) != report.GroupUUID {
		panic("volume identity mismatch")
	}
	if string(must(sys.ReadFile("private/etc/fstab"))) != report.Fstab {
		panic("fstab mismatch")
	}
	fileVolume := sys
	if report.OverlayVolume == "data" {
		fileVolume = data
	} else if report.OverlayVolume != "" && report.OverlayVolume != "system" {
		panic("unsupported overlay volume")
	}
	for path, want := range report.Files {
		entry := must(fileVolume.FileEntryByPath(path))
		if want.Symlink != "" {
			if must(entry.SymbolicLinkTarget()) != want.Symlink {
				panic("symlink mismatch: " + path)
			}
		} else {
			got := sha256.Sum256(must(fileVolume.ReadFile(path)))
			if hex.EncodeToString(got[:]) != want.SHA256 {
				panic("file hash mismatch: " + path)
			}
		}
	}
	must(data.RootDirectory())
	_, free := freeTail(c, reader)
	if free*4096 != report.Free {
		panic("space accounting mismatch")
	}
	writeJSON(*output, map[string]any{"device": *device, "container_byte_offset": *offset, "system_uuid": report.SystemUUID, "data_uuid": report.DataUUID, "volume_group_uuid": report.GroupUUID, "verified_overlay_volume": report.OverlayVolume, "verified_overlay_files": len(report.Files), "preserved_volume_superblocks_verified": len(report.Preserved), "fstab_matches": true, "shared_free_bytes": free * 4096, "full_ios_boot_verified": false})
	fmt.Println("System/Data identity, fstab and every overlay file verified from the actual reader.")
}
