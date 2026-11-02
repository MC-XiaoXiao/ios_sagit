package main

import (
	"os"
	"path/filepath"
	"strings"
)

// Repair payloads use volume-relative paths. Existing parents are checked by
// the native editor; new parents use the explicit target ownership and never traverse symlinks.
func nativeDataInputs(overlay string, uid, gid uint32) []nativeSystemInput {
	inputs := []nativeSystemInput{}
	must(true, filepath.WalkDir(overlay, func(p string, e os.DirEntry, err error) error {
		if err != nil {
			return err
		}
		if e.IsDir() {
			return nil
		}
		st := must(os.Lstat(p))
		repairCheck(st.Mode().IsRegular() && st.Size() > 0 && st.Size() <= 64<<20, "Data overlay requires regular nonempty files up to 64 MiB")
		rel := filepath.ToSlash(must(filepath.Rel(overlay, p)))
		repairCheck(rel != "." && rel != "" && !strings.HasPrefix(rel, "/") && filepath.ToSlash(filepath.Clean(rel)) == rel && !strings.HasPrefix(rel, "../"), "invalid Data overlay path")
		inputs = append(inputs, nativeSystemInput{path: rel, payload: must(os.ReadFile(p)), mode: st.Mode(), uid: uid, gid: gid})
		return nil
	}))
	repairCheck(len(inputs) > 0, "empty Data overlay")
	return inputs
}
