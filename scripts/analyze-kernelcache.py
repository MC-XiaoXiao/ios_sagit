#!/usr/bin/env python3
"""Read a prelinked Mach-O's actual plist, including Apple's XML IDREFs.

Writes an inventory and proposed dependency closures, never a bootable image.
Missing dependencies remain explicit; no libraries are silently discarded.
"""
import argparse
import base64
import hashlib
import json
from pathlib import Path
import re
import struct
import xml.etree.ElementTree as ET




def decode_plist(raw):
    root = ET.fromstring(raw.rstrip(b"\0"))
    ids = {e.attrib["ID"]: e for e in root.iter() if "ID" in e.attrib}

    def convert(e, visiting=frozenset()):
        if "IDREF" in e.attrib:
            key = e.attrib["IDREF"]
            if key in visiting or key not in ids:
                raise ValueError(f"cyclic or missing XML IDREF {key}")
            return convert(ids[key], visiting | {key})
        if e.tag == "plist":
            return convert(e[0], visiting)
        if e.tag == "dict":
            children = list(e)
            if len(children) % 2 or any(x.tag != "key" for x in children[::2]):
                raise ValueError("invalid plist dictionary")
            return {convert(children[i], visiting): convert(children[i + 1], visiting)
                    for i in range(0, len(children), 2)}
        if e.tag == "array":
            return [convert(x, visiting) for x in e]
        if e.tag == "integer":
            text = e.text or "0"
            return int(text, 16 if text.lower().startswith("0x") else 10)
        if e.tag in ("true", "false"):
            return e.tag == "true"
        if e.tag == "data":
            return {"base64": base64.b64encode(base64.b64decode(e.text or "")).decode()}
        return e.text or ""

    return convert(root)


def inspect(path):
    data = path.read_bytes()
    if len(data) < 32:
        raise ValueError("truncated Mach-O")
    header = struct.unpack_from("<8I", data)
    if header[0] != 0xfeedfacf:
        raise ValueError("expected decompressed little-endian 64-bit Mach-O")
    pos, end = 32, 32 + header[5]
    if end > len(data):
        raise ValueError("load commands outside file")
    segments, info = [], None
    for _ in range(header[4]):
        if pos + 8 > end:
            raise ValueError("truncated load command")
        cmd, size = struct.unpack_from("<II", data, pos)
        if size < 8 or pos + size > end:
            raise ValueError("invalid load command size")
        if cmd == 0x19:
            if size < 72:
                raise ValueError("short segment command")
            s = struct.unpack_from("<II16sQQQQiiII", data, pos)
            name = s[2].rstrip(b"\0").decode()
            if s[5] + s[6] > len(data) or 72 + s[9] * 80 > size:
                raise ValueError("invalid segment range")
            segments.append({"name": name, "vmaddr": hex(s[3]), "vmsize": s[4],
                             "fileoff": s[5], "filesize": s[6]})
            for i in range(s[9]):
                section = struct.unpack_from("<16s16sQQIIIIIIII", data, pos + 72 + i * 80)
                if name == "__PRELINK_INFO" and section[0].rstrip(b"\0") == b"__info":
                    offset, length = section[4], section[3]
                    if offset + length > len(data):
                        raise ValueError("prelink info outside file")
                    info = data[offset:offset + length]
        pos += size
    if info is None:
        raise ValueError("no traditional prelink plist; fileset needs a collection reader")
    version = re.search(rb"Darwin Kernel Version [^\0]+", data)
    metadata = {"file": path.name, "sha256": hashlib.sha256(data).hexdigest(),
                "size": len(data), "cputype": hex(header[1]), "cpusubtype": hex(header[2]),
                "filetype": header[3], "segments": segments,
                "darwin_version": version[0].decode() if version else None}
    return metadata, decode_plist(info)["_PrelinkInfoDictionary"]


def architecture_property(kext, key, architecture="arm64", default=None):
    """Match OSKext::getPropertyForHostArch; CFBundle keys never vary."""
    if key.startswith(("OS", "IO")) and key + "_" + architecture in kext:
        return kext[key + "_" + architecture]
    return kext.get(key, default)


def architecture_libraries(kext, architecture="arm64"):
    """Use OSKext::getPropertyForHostArch's replacement, never merge keys.

    Keep the source dictionary unchanged for serialized metadata comparison.
    An empty architecture dictionary is an explicit empty dependency set.
    """
    libraries = architecture_property(kext, "OSBundleLibraries", architecture, {})
    if not isinstance(libraries, dict) or any(not isinstance(k, str) or not isinstance(v, str)
                                               for k, v in libraries.items()):
        raise ValueError("invalid architecture-specific OSBundleLibraries")
    return libraries


def image_architecture(metadata):
    cpu = int(str(metadata["cputype"]), 0)
    if cpu == 0x100000c:
        return "arm64"
    if cpu == 0x1000007:
        return "x86_64"
    raise ValueError("unsupported kernelcache CPU architecture")


def closure(by_id, roots, architecture="arm64"):
    order, active, done, missing = [], set(), set(), set()

    def visit(name):
        if name in done:
            return
        if name not in by_id:
            missing.add(name)
            return
        if name in active:
            raise ValueError(f"dependency cycle at {name}")
        active.add(name)
        for dependency in sorted(architecture_libraries(by_id[name], architecture)):
            visit(dependency)
        active.remove(name)
        done.add(name)
        order.append(name)

    for root in roots:
        visit(root)
    return {"roots": roots, "dependency_order": order, "missing": sorted(missing),
            "status": "blocked-missing-dependencies" if missing else "candidate-not-boot-validated"}


def dependency_audit(path, metadata, kexts, identifier):
    """Find resolved pointers/branches into one provider without guessed symbols.

    A traditional prelink image has no per-Kext undefined symbol table. These
    are address references, not a proof of all runtime dependencies or behavior.
    """
    data = path.read_bytes()
    by_id = {k['CFBundleIdentifier']: k for k in kexts}
    if identifier not in by_id:
        raise ValueError('dependency provider absent from kernelcache')

    def number(value):
        return int(value, 0) if isinstance(value, str) else value

    def mapped(address, size):
        for s in metadata['segments']:
            start = number(s['vmaddr'])
            if start <= address and size <= s['filesize'] - (address - start):
                offset = s['fileoff'] + address - start
                if offset + size <= len(data):
                    return offset
        raise ValueError('audit address outside file-backed segments')

    def regions(kext):
        address = number(kext['_PrelinkExecutableLoadAddr'])
        offset = mapped(address, 32)
        h = struct.unpack_from('<8I', data, offset)
        if h[0] != 0xfeedfacf:
            raise ValueError('invalid embedded Kext header')
        pos, end = offset + 32, offset + 32 + h[5]
        mapped(address, 32 + h[5])
        result, sections = [], []
        for _ in range(h[4]):
            if pos + 8 > end:
                raise ValueError('truncated audit command')
            cmd, size = struct.unpack_from('<II', data, pos)
            if size < 8 or pos + size > end:
                raise ValueError('invalid audit command')
            if cmd == 0x19:
                if size < 72:
                    raise ValueError('short audit segment')
                s = struct.unpack_from('<II16sQQQQiiII', data, pos)
                if 72 + 80*s[9] > size:
                    raise ValueError('audit sections outside command')
                if s[2].rstrip(b'\0') != b'__LINKEDIT' and s[6]:
                    mapped(s[3], s[6])
                    result.append((s[3], s[6], s[7]))
                for i in range(s[9]):
                    q = struct.unpack_from('<16s16sQQIIIIIIII', data, pos + 72 + 80*i)
                    if q[0].rstrip(b'\0') == b'__stubs':
                        mapped(q[2], q[3])
                        sections.append((q[2], q[3]))
            pos += size
        if pos != end:
            raise ValueError('audit command count mismatch')
        return result, sections

    provider, _ = regions(by_id[identifier])

    def belongs(address):
        return any(start <= address < start + size for start, size, _ in provider)

    clients = []
    for kext in kexts:
        if identifier not in architecture_libraries(kext, image_architecture(metadata)):
            continue
        areas, stubs = regions(kext)
        references, imports = [], {}
        for start, size, protection in areas:
            offset = mapped(start, size)
            # Scan pointers only in non-executable sections, avoiding pairs of
            # instructions that merely resemble a 64-bit address.
            if not protection & 4:
                for i in range((-start) % 8, size - 7, 8):
                    target, = struct.unpack_from('<Q', data, offset + i)
                    if belongs(target):
                        imports[start+i] = target
                        references.append({'kind': 'pointer', 'address': hex(start+i), 'target': hex(target)})
            else:
                for i in range(0, size - 3, 4):
                    word, = struct.unpack_from('<I', data, offset + i)
                    if word & 0x7c000000 == 0x14000000:
                        delta = word & 0x3ffffff
                        if delta & 0x2000000:
                            delta -= 0x4000000
                        target = start + i + 4*delta
                        if belongs(target):
                            references.append({'kind': 'branch', 'address': hex(start+i), 'target': hex(target)})
        imported_stubs = {}
        for start, size in stubs:
            for i in range(0, size - 11, 12):
                address = start + i
                x, y, z = struct.unpack_from('<III', data, mapped(address, 12))
                # ADRP X16; LDR X16,[X16,#imm]; BR X16, arm64 (not arm64e).
                if x & 0x9f00001f != 0x90000010 or y & 0xffc003ff != 0xf9400210 or z != 0xd61f0200:
                    continue
                delta = (x >> 29 & 3) | (x >> 5 & 0x7ffff) << 2
                if delta & 0x100000:
                    delta -= 0x200000
                got = (address & ~4095) + delta*4096 + (y >> 10 & 4095)*8
                if got in imports:
                    imported_stubs[address] = (got, imports[got])
        for start, size, protection in areas:
            if not protection & 4:
                continue
            offset = mapped(start, size)
            for i in range(0, size - 3, 4):
                word, = struct.unpack_from('<I', data, offset+i)
                if word & 0xfc000000 != 0x94000000:
                    continue
                delta = word & 0x3ffffff
                if delta & 0x2000000:
                    delta -= 0x4000000
                stub = start + i + 4*delta
                if stub in imported_stubs:
                    got, target = imported_stubs[stub]
                    references.append({'kind': 'import-call', 'address': hex(start+i),
                                       'stub': hex(stub), 'got': hex(got), 'target': hex(target)})
        clients.append({'bundle': kext['CFBundleIdentifier'], 'references': references})
    return {'image': metadata['file'], 'sha256': metadata['sha256'], 'provider': identifier,
            'coverage': 'resolved file-backed pointers, B/BL and arm64 import stubs; excludes dynamic matching and indirect dataflow',
            'clients': clients}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("kernelcache", type=Path)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--collections", type=Path,
                        default=Path(__file__).resolve().parents[1] / "tools/kext-collections.json",
                        help="named collection roots; bundle identities belong to this data file")
    parser.add_argument("--root", action="append", default=[], help="additional ad-hoc dependency root; repeatable")
    parser.add_argument("--audit-dependency", help="write only a resolved-reference audit for this provider bundle")
    args = parser.parse_args()
    metadata, kexts = inspect(args.kernelcache)
    if args.audit_dependency:
        audit = dependency_audit(args.kernelcache, metadata, kexts, args.audit_dependency)
        args.output.mkdir(parents=True, exist_ok=True)
        (args.output / 'dependency-audit.json').write_text(json.dumps(audit, indent=2) + '\n')
        return
    by_id = {k["CFBundleIdentifier"]: k for k in kexts}
    if len(by_id) != len(kexts):
        raise ValueError("duplicate bundle identifiers")
    architecture = image_architecture(metadata)
    missing = [{"bundle": name, "dependency": dep} for name, k in by_id.items()
               for dep in architecture_libraries(k, architecture) if dep not in by_id]
    inventory = {"kernel": metadata, "count": len(kexts), "missing_dependencies": missing,
                 "kexts": kexts}
    definitions = json.loads(args.collections.read_text()) if args.collections.exists() else {}
    if args.root:
        definitions["requested"] = args.root
    if any(not re.fullmatch(r'[A-Za-z0-9_-]+', name) for name in definitions):
        raise ValueError("invalid collection directory name")
    stages = {name: closure(by_id, roots, architecture) for name, roots in definitions.items()}
    args.output.mkdir(parents=True, exist_ok=True)
    for name, obj in [("inventory.json", inventory), ("closures.json", stages)]:
        (args.output / name).write_text(json.dumps(obj, ensure_ascii=False, indent=2) + "\n")
    rows = ["bundle_id\tversion\tOSBundleRequired\tdependencies\tpersonality_count"]
    for name, k in sorted(by_id.items()):
        rows.append("\t".join([name, str(k.get("CFBundleVersion", "")), str(k.get("OSBundleRequired", "")),
                               ",".join(sorted(architecture_libraries(k, architecture))),
                               str(len(k.get("IOKitPersonalities", {})))]))
    (args.output / "inventory.tsv").write_text("\n".join(rows) + "\n")
    for name, stage in stages.items():
        directory = args.output / "stages" / name
        directory.mkdir(parents=True, exist_ok=True)
        manifest = directory / "kexts.txt"
        # A blocked closure must not masquerade as a runnable patcher manifest.
        if stage["missing"]:
            manifest.unlink(missing_ok=True)
            (directory / "missing.txt").write_text("\n".join(stage["missing"]) + "\n")
        else:
            (directory / "missing.txt").unlink(missing_ok=True)
            manifest.write_text("# Candidate dependency closure; linking and hardware are unverified.\n"
                                + "\n".join(stage["dependency_order"]) + "\n")
        print(name, len(stage["dependency_order"]), stage["status"], stage["missing"])
    print(f"{len(kexts)} Kexts; {len(missing)} missing dependency edges; {metadata['sha256']}")


if __name__ == "__main__":
    main()
