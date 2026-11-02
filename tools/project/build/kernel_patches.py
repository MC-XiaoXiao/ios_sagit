"""Replay the ordered XNU patches without confusing overlap with local edits.

The old per-patch reverse probe examined every patch against the final tree.
That fails when a later patch changes an earlier patch's output. Reconstruct
the complete stack in isolation, recognize a complete prefix, then publish
only the remaining changes after every source and output has been checked.
"""
from __future__ import annotations

import fcntl
import hashlib
import os
from pathlib import Path, PurePosixPath
import stat
import subprocess
import tarfile
import tempfile


class PatchStackError(RuntimeError):
    """The source tree or patch recipe cannot be safely updated."""


def order_kernel_patches(patches: list[Path]) -> list[Path]:
    """Keep filename order except for documented prerequisites in old patches."""
    pending = sorted(map(Path, patches))
    names = {patch.name for patch in pending}
    dependencies = {
        # The development policy's IOUserClient hunk includes trace_open.
        "xnu-iokit-development.patch": {
            "zzzzzzzzzzzzzzzz-development-iokit-open-trace.patch"
        },
    }
    result = []
    done = set()
    while pending:
        for patch in pending:
            required = dependencies.get(patch.name, set())
            missing = required - names
            if missing:
                raise PatchStackError(f"{patch.name} requires {', '.join(sorted(missing))}")
            if required <= done:
                result.append(patch)
                done.add(patch.name)
                pending.remove(patch)
                break
        else:
            raise PatchStackError("cyclic kernel patch dependencies")
    return result


def _patch_path(value: str) -> str | None:
    value = value.split("\t", 1)[0].split(" ", 1)[0]
    if value == "/dev/null":
        return None
    if not value.startswith(("a/", "b/")):
        raise PatchStackError(f"patch filename must use a/ or b/: {value!r}")
    name = value[2:]
    path = PurePosixPath(name)
    if not name or path.is_absolute() or ".." in path.parts or str(path) != name:
        raise PatchStackError(f"unsafe patch filename: {value!r}")
    return name


def _patch_files(patch: Path) -> set[str]:
    lines = patch.read_text().splitlines()
    result = set()
    for index, line in enumerate(lines):
        if line.startswith("--- ") and index + 1 < len(lines) and lines[index + 1].startswith("+++ "):
            old = _patch_path(line[4:])
            new = _patch_path(lines[index + 1][4:])
            if old is not None and new is not None and old != new:
                raise PatchStackError(f"renames are not supported: {patch.name}")
            result.update(name for name in (old, new) if name is not None)
    if not result:
        raise PatchStackError(f"no unified-diff files in {patch}")
    return result


def _read_regular(root: Path, name: str) -> bytes | None:
    path = root
    for part in PurePosixPath(name).parts:
        path = path / part
        if path.is_symlink():
            raise PatchStackError(f"refusing symlink in patched path: {path}")
    if not path.exists():
        return None
    if not path.is_file():
        raise PatchStackError(f"patched path is not a regular file: {path}")
    return path.read_bytes()


def _snapshot(root: Path, names: set[str]) -> dict[str, bytes | None]:
    return {name: _read_regular(root, name) for name in sorted(names)}


def _install_file(path: Path, contents: bytes, mode: int) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    descriptor, temporary = tempfile.mkstemp(prefix="." + path.name + ".patch-", dir=path.parent)
    try:
        with os.fdopen(descriptor, "wb") as stream:
            stream.write(contents)
            stream.flush()
            os.fsync(stream.fileno())
        os.chmod(temporary, mode)
        os.replace(temporary, path)
    finally:
        if os.path.exists(temporary):
            os.unlink(temporary)


def ensure_kernel_patch_stack(
    source: Path, patches: list[Path], archive: Path, archive_sha256: str,
    log_path: Path | None = None,
) -> dict:
    """Apply missing patches only from a recognized complete stack prefix.

    ``patches`` must be supplied in application order. The locked archive is
    expected to have one top-level directory, as Apple's release archives do.
    Any unexpected source edit or failed staged patch leaves source untouched.
    """
    source, archive = Path(source).resolve(), Path(archive).resolve()
    patches = [Path(patch).resolve() for patch in patches]
    if not source.is_dir():
        raise PatchStackError(f"kernel source directory missing: {source}")
    with archive.open("rb") as stream:
        actual_digest = hashlib.file_digest(stream, "sha256").hexdigest()
    if actual_digest != archive_sha256:
        raise PatchStackError(f"kernel archive checksum mismatch: {archive}")
    names = set().union(*(_patch_files(patch) for patch in patches)) if patches else set()
    log = []
    lock_path = source.parent / ("." + source.name + ".patch-stack.lock")
    with lock_path.open("a+b") as lock:
        fcntl.flock(lock, fcntl.LOCK_EX)
        current = _snapshot(source, names)
        modes = {name: stat.S_IMODE((source / name).stat().st_mode)
                 for name in names if current[name] is not None}
        with tempfile.TemporaryDirectory(prefix="xnu-patch-stack-") as temporary:
            stage = Path(temporary)
            with tarfile.open(archive) as bundle:
                roots = {PurePosixPath(member.name).parts[0] for member in bundle.getmembers()
                         if PurePosixPath(member.name).parts}
                if len(roots) != 1:
                    raise PatchStackError("kernel archive must have one top-level directory")
                archive_root = roots.pop()
                for name in sorted(names):
                    try:
                        member = bundle.getmember(archive_root + "/" + name)
                    except KeyError:
                        continue  # A patch may create this file.
                    if not member.isfile():
                        raise PatchStackError(f"archive patch input is not a regular file: {name}")
                    path = stage / name
                    path.parent.mkdir(parents=True, exist_ok=True)
                    with bundle.extractfile(member) as stream:
                        path.write_bytes(stream.read())
                    modes.setdefault(name, member.mode & 0o777)
            expected = _snapshot(stage, names)
            prefix = 0 if current == expected else None
            known_versions = {name: {expected[name]} for name in names}
            for index, patch in enumerate(patches, 1):
                result = subprocess.run(
                    ["patch", "--batch", "--forward", "--no-backup-if-mismatch",
                     "-p1", "-i", str(patch)], cwd=stage,
                    capture_output=True, text=True,
                )
                log.append(f"[{index}/{len(patches)}] {patch.name}\n{result.stdout}{result.stderr}")
                if result.returncode:
                    if log_path:
                        Path(log_path).parent.mkdir(parents=True, exist_ok=True)
                        Path(log_path).write_text("\n".join(log))
                    raise PatchStackError(
                        f"kernel patch stack failed in isolation at {patch.name}; "
                        f"source unchanged\n{result.stdout}{result.stderr}"
                    )
                expected = _snapshot(stage, names)
                for name in names:
                    known_versions[name].add(expected[name])
                if current == expected:
                    prefix = index
            if log_path:
                Path(log_path).parent.mkdir(parents=True, exist_ok=True)
                Path(log_path).write_text("\n".join(log))
            if prefix is None:
                unknown = sorted(name for name in names if current[name] not in known_versions[name])
                detail = ", ".join(unknown or sorted(
                    name for name in names if current[name] != expected[name]))
                raise PatchStackError(
                    "kernel source does not match any complete patch-stack prefix; "
                    "local changes preserved. Review: " + detail
                )
            changed = sorted(name for name in names if current[name] != expected[name])
            # Recheck all inputs, not just outputs, after the potentially lengthy replay.
            if _snapshot(source, names) != current:
                raise PatchStackError("kernel sources changed during patch preflight; source unchanged")
            published = []
            try:
                for name in changed:
                    path = source / name
                    if expected[name] is None:
                        path.unlink()
                    else:
                        _install_file(path, expected[name], modes.get(name, 0o644))
                    published.append(name)
            except Exception:
                # Preflight avoids patch failures after mutation. Restore the prior
                # bytes if a filesystem failure occurs while publishing the files.
                for name in reversed(published):
                    path = source / name
                    if current[name] is None:
                        path.unlink(missing_ok=True)
                    else:
                        _install_file(path, current[name], modes.get(name, 0o644))
                raise
            return {"matched_prefix": prefix, "patches": len(patches), "updated_files": changed}
