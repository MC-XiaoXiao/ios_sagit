"""Actual EFI allocator/finalizer and real staged firmware; no device access."""
import copy
import hashlib
import json
from pathlib import Path
import struct
import subprocess
import tempfile
import unittest
from test_smem_handoff import bridge_module, unpack, walk

ROOT = Path(__file__).resolve().parents[1]


class BasebandHandoffTests(unittest.TestCase):
    def test_complete_stock_catalog_is_readable_by_the_native_store(self):
        with tempfile.TemporaryDirectory() as directory:
            out = Path(directory)
            source = out / 'read-all.cpp'
            source.write_text(r'''
#include "BasebandTFTPFiles.h"
#include <cassert>
#include <fstream>
#include <iterator>
#include <memory>
#include <string>
#include <vector>
using namespace Baseband;
static std::vector<unsigned char> load(const std::string &path) {
    std::ifstream f(path,std::ios::binary);assert(f);return {std::istreambuf_iterator<char>(f),{}};
}
int main(int argc,char **argv) {
    assert(argc==3);auto raw=load(argv[1]);auto files=std::make_unique<TFTP::Files>();
    assert(files->bind(raw.data(),raw.size())==Result::OK && files->immutableCount()==113);
    assert(files->start(1)==Result::OK);
    for(unsigned i=0;i<113;++i) {
        const std::string path(reinterpret_cast<const char *>(raw.data()+16+144*i));
        const auto expected=load(std::string(argv[2])+"/"+path.substr(25));
        unsigned h=0;assert(files->open(0,path.c_str(),false,false,h)==Result::OK);
        uint64_t bytes=0;assert(files->size(h,bytes)==Result::OK && bytes==expected.size());
        std::vector<unsigned char> actual(bytes);
        assert(files->read(h,0,actual.data(),actual.size())==Result::OK && actual==expected);
        assert(files->close(h)==Result::OK);
    }
}
''')
            executable = out / 'read-all'
            subprocess.run(['c++', '-std=c++17', '-Wall', '-Wextra', '-Werror',
                            '-I' + str(ROOT / 'drivers/include'),
                            '-I' + str(ROOT / 'drivers/baseband/QcomBaseband'), str(source), '-o', str(executable)], check=True)
            subprocess.run([str(executable), str(ROOT / 'build/baseband/firmware/tftp-files.bin'),
                            str(ROOT / 'build/baseband/stock-files')], check=True)

    def test_tftp_catalog_cross_language_and_pinned_file_rejection(self):
        bridge_module()
        from baseband_handoff import encode_tftp_catalog, prepare_tftp_catalog
        # The exact Python-produced wire is consumed by the native C++ parser
        # and compared to both actual sagit descriptor files, byte for byte.
        with tempfile.TemporaryDirectory() as directory:
            out = Path(directory)
            files = {f'/readonly/firmware/image/{name}': (ROOT / 'firmware/sagit-baseband' / name).read_bytes()
                     for name in ('modemr.jsn', 'modemuw.jsn')}
            encoded = encode_tftp_catalog(files)
            raw = out / 'catalog.bin'; raw.write_bytes(encoded)
            source = out / 'decode.cpp'
            source.write_text(r'''
#include "BasebandTFTPFiles.h"
#include <cassert>
#include <fstream>
#include <iterator>
#include <memory>
#include <string>
#include <vector>
static std::vector<unsigned char> load(const char *path) {
    std::ifstream f(path,std::ios::binary);assert(f);
    return {std::istreambuf_iterator<char>(f),{}};
}
int main(int argc,char **argv) {
    assert(argc==4);auto blob=load(argv[1]);auto files=std::make_unique<Baseband::TFTP::Files>();
    assert(files->bind(blob.data(),blob.size())==Baseband::Result::OK);
    assert(files->start(1)==Baseband::Result::OK);
    const char *names[]={"/readonly/firmware/image/modemr.jsn","/readonly/firmware/image/modemuw.jsn"};
    for(unsigned i=0;i<2;++i) {
        unsigned handle=0;assert(files->open(0,names[i],false,false,handle)==Baseband::Result::OK);
        auto expected=load(argv[i+2]);std::vector<unsigned char> result(expected.size());
        assert(files->read(handle,0,result.data(),result.size())==Baseband::Result::OK && result==expected);
        assert(files->close(handle)==Baseband::Result::OK);
    }
}
''')
            executable = out / 'decode'
            subprocess.run(['c++', '-std=c++17', '-Wall', '-Wextra', '-Werror',
                            '-I' + str(ROOT / 'drivers/include'),
                            '-I' + str(ROOT / 'drivers/baseband/QcomBaseband'), str(source), '-o', str(executable)], check=True)
            subprocess.run([str(executable), str(raw), *(str(ROOT / 'firmware/sagit-baseband' / name)
                            for name in ('modemr.jsn', 'modemuw.jsn'))], check=True)
            for name in ('/readonly/firmware/image/../nv', '/readonly/firmware/image//empty',
                         '/readwrite/test', '/readonly/firmware/image/' + 'x' * 128):
                with self.assertRaises(ValueError): encode_tftp_catalog({name: b'bad'})
            # A staged descriptor cannot diverge from the checked extraction.
            staged = json.loads((ROOT / 'build/baseband/firmware/manifest.json').read_text())
            staged.pop('tftp_catalog', None)  # Exercise the retained version-1 small-catalog format.
            pin = json.loads((ROOT / 'docs/baseband/firmware-sources.json').read_text())
            for name in ('modemr.jsn', 'modemuw.jsn'):
                (out / name).write_bytes((ROOT / 'build/baseband/firmware' / name).read_bytes())
            good, _ = prepare_tftp_catalog(out, staged, pin)
            self.assertEqual(good, encoded)
            (out / 'modemr.jsn').write_bytes(files['/readonly/firmware/image/modemr.jsn'] + b' ')
            with self.assertRaisesRegex(ValueError, 'staged/pinned'):
                prepare_tftp_catalog(out, staged, pin)

    def test_explicit_nv_identity_policy_and_atomic_rejection(self):
        bridge = bridge_module()
        record = bytearray(224)
        struct.pack_into('<IHHIIQ', record, 0, 0x4f484242, 1, 224, 0, 0, 42)
        tree = {'children': [{'name': 'chosen', 'props': {'qcom,baseband-handoff': bytes(record)}}]}
        labels = ('modemst1', 'modemst2', 'fsc', 'fsg')
        policy = {'schema': 1, 'partitions': {
            name: {'uuid': f'00112233-4455-6677-8899-aabbccddee{i:02x}', 'lun': 0,
                   'start': 0x100000 + i * 0x10000, 'bytes': 0x10000,
                   'access': 'read-only' if i == 3 else 'read-write'} for i, name in enumerate(labels)}}
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / 'policy.json'
            path.write_text(json.dumps(policy))
            report = bridge.configure_rmtfs_storage_policy(tree, path)
            encoded = tree['children'][0]['props']['qcom,rmtfs-storage-policy']
            self.assertEqual(struct.unpack_from('<IHHQIIQ', encoded), (0x50534652, 1, 224, 42, 4, 0, 0))
            for i in range(4):
                self.assertEqual(struct.unpack_from('<QQIIQ', encoded, 48 + i * 48),
                                 (0x100000 + i * 0x10000, 0x10000, 0, 1 if i == 3 else 3, 0))
            self.assertFalse(report['nv_contents_embedded'])
            self.assertTrue(report['native_media_match_required'])
            self.assertEqual(report['source_sha256'], hashlib.sha256(path.read_bytes()).hexdigest())
            # Execute the production C++ decoder against the Python-produced
            # record. The backing file contains only synthetic policy fields.
            raw = Path(directory) / 'policy.bin'; raw.write_bytes(encoded)
            source = Path(directory) / 'decoder.cpp'
            source.write_text(r'''
#include "BasebandRMTFSStorage.h"
#include <cassert>
#include <fstream>
int main(int argc,char **argv) {
    assert(argc==2);unsigned char bytes[224];std::ifstream f(argv[1],std::ios::binary);
    assert(f.read(reinterpret_cast<char *>(bytes),sizeof(bytes)));
    Baseband::RMTFS::StoragePolicy p;assert(p.decode(bytes,sizeof(bytes))==Baseband::Result::OK);
    assert(p.epoch==42 && p.media[3].access==1 && p.media[2].start==0x120000);
}
''')
            executable = Path(directory) / 'decoder'
            subprocess.run(['c++', '-std=c++17', '-Wall', '-Wextra', '-Werror',
                            '-I' + str(ROOT / 'drivers/include'),
                            '-I' + str(ROOT / 'drivers/baseband/QcomBaseband'), str(source), '-o', str(executable)], check=True)
            subprocess.run([str(executable), str(raw)], check=True)
            mutations = []
            for key, value in [('uuid', '00000000-0000-0000-0000-000000000000'), ('uuid', 'abc'),
                               ('lun', True), ('start', 513), ('bytes', 513), ('access', 'discard-writes')]:
                bad = copy.deepcopy(policy); bad['partitions']['fsc'][key] = value; mutations.append(bad)
            bad = copy.deepcopy(policy); bad['partitions']['fsc']['uuid'] = bad['partitions']['modemst1']['uuid']; mutations.append(bad)
            bad = copy.deepcopy(policy); bad['partitions']['fsc']['start'] = 0x100200; mutations.append(bad)
            bad = copy.deepcopy(policy); del bad['partitions']['fsc']; mutations.append(bad)
            bad = copy.deepcopy(policy); bad['schema'] = True; mutations.append(bad)
            for bad in mutations:
                path.write_text(json.dumps(bad))
                with self.assertRaises(ValueError): bridge.configure_rmtfs_storage_policy(tree, path)
                self.assertEqual(tree['children'][0]['props']['qcom,rmtfs-storage-policy'], encoded)

    def test_staged_payload_and_native_dt_are_separate(self):
        bridge = bridge_module()
        with tempfile.TemporaryDirectory() as directory:
            out = Path(directory)
            dtb = out / 'sagit.dtb'
            subprocess.run(['dtc', '-q', '-I', 'dts', '-O', 'dtb', '-i', str(ROOT / 'device_trees'),
                            '-o', str(dtb), str(ROOT / 'device_trees/xiaomi-sagit.dts')], check=True)
            original = bridge.parse_fdt(dtb.read_bytes())
            tree = copy.deepcopy(original)
            bridge.configure_smem_handoff(tree)
            report = bridge.configure_baseband_handoff(tree, ROOT / 'build/baseband/firmware', out, ROOT)
            payload = (out / 'baseband-firmware.bin').read_bytes()
            mba = (ROOT / 'firmware/sagit-baseband/mba.mbn').read_bytes()
            modem = (ROOT / 'firmware/sagit-baseband/modem.mbn').read_bytes()
            self.assertEqual(payload[:len(mba)], mba)
            offset = report['modem_offset']
            self.assertEqual(payload[offset:offset+len(modem)], modem)
            self.assertFalse(any(payload[len(mba):offset]))
            files_offset = report['tftp_catalog_offset']
            catalog_size = report['tftp_catalog']['bytes']
            self.assertFalse(any(payload[offset+len(modem):files_offset]))
            self.assertFalse(any(payload[files_offset+catalog_size:]))
            self.assertEqual(hashlib.sha256(payload).hexdigest(), report['archive_sha256'])
            blob = bridge.apple_dt(tree)
            native, _ = unpack(blob)
            by_name = {n['props']['name'].rstrip(b'\0').decode(): n['props'] for n in walk(native)}
            record = by_name['chosen']['qcom,baseband-handoff']
            self.assertEqual(struct.unpack_from('<IHHIIQ', record), (0x4f484242, 2, 272, 0, 0, 1))
            self.assertEqual(record[152:184], hashlib.sha256(mba).digest())
            self.assertEqual(record[184:216], hashlib.sha256(modem).digest())
            self.assertEqual(struct.unpack_from('<4Q', record, 104), (0, len(mba), offset, len(modem)))
            self.assertLess(len(blob), 0x400000)
            self.assertNotIn(mba, blob)
            self.assertNotIn('qcom,tftp-files', by_name['chosen'])
            catalog = payload[files_offset:files_offset+catalog_size]
            self.assertEqual(hashlib.sha256(catalog).hexdigest(), report['tftp_catalog']['sha256'])
            self.assertEqual(struct.unpack_from('<IHHII', catalog), (0x46544251, 1, 144, 113, len(catalog)))
            self.assertEqual(struct.unpack_from('<QQ32s', record, 224),
                             (files_offset, catalog_size, hashlib.sha256(catalog).digest()))
            self.assertNotIn(catalog, blob)
            for i in range(113):
                path, offset, size, reserved = struct.unpack_from('<128sIIQ', catalog, 16 + 144 * i)
                name = path.rstrip(b'\0').decode().removeprefix('/readonly/firmware/image/')
                self.assertEqual(catalog[offset:offset + size], (ROOT / 'build/baseband/stock-files' / name).read_bytes())
                self.assertEqual(reserved, 0)
            self.assertFalse(report['tftp_catalog']['dynamic_module_directory_provided'])
            self.assertEqual(by_name['modem@4080000']['status'], b'disabled\0')
            for name in ('QcomMPSS', 'QcomMBA', 'QcomBBMetadata', 'QcomBBFirmware', 'QcomRMTFS'):
                self.assertEqual(by_name['memory-map'][name], bytes(16))
            # Altered bytes may not replace an already produced payload.
            staged = out / 'bad'; staged.mkdir()
            manifest = json.loads((ROOT / 'build/baseband/firmware/manifest.json').read_text())
            (staged / 'manifest.json').write_text(json.dumps(manifest))
            (staged / 'mba.mbn').write_bytes(mba[:-1] + bytes([mba[-1] ^ 1]))
            (staged / 'modem.mbn').write_bytes(modem)
            with self.assertRaises(ValueError): bridge.configure_baseband_handoff(copy.deepcopy(original), staged, out, ROOT)
            self.assertEqual((out / 'baseband-firmware.bin').read_bytes(), payload)

    def test_actual_efi_allocation_map_retry_and_abort(self):
        source = r'''
#include <Uefi.h>
#include "BasebandMemory.h"
#include <sys/mman.h>
#include <cassert>
#include <cstring>
#include <vector>
#include <map>
extern "C" int bridge_baseband_prepare(EFI_BOOT_SERVICES *,void *);
extern "C" int bridge_baseband_finalize(void *,const void *,size_t,size_t,uint64_t,uint64_t);
extern "C" void bridge_baseband_abort(EFI_BOOT_SERVICES *,void *);
using namespace Baseband;
struct Tree {uint8_t record[MemoryHandoff::MaxBytes]{},ranges[5][16]{};} tree;
#ifdef TEST_CATALOG_HANDOFF
constexpr unsigned recordBytes=272;
#else
constexpr unsigned recordBytes=224;
#endif
std::vector<EFI_MEMORY_DESCRIPTOR> descriptors;
std::map<uint64_t,uint64_t> allocated;
unsigned allocations=0,failAllocation=0,cleaned=0,frees=0;
extern "C" void *dt_property(void *p,const char *node,const char *key,uint32_t *length) {
    assert(p==&tree);
    if(!strcmp(node,"chosen")) {assert(!strcmp(key,"qcom,baseband-handoff"));*length=recordBytes;return tree.record;}
    assert(!strcmp(node,"memory-map"));
    const char *names[]={"QcomMPSS","QcomMBA","QcomBBMetadata","QcomBBFirmware","QcomRMTFS"};
    for(unsigned i=0;i<5;++i)if(!strcmp(key,names[i])){*length=16;return tree.ranges[i];}
    assert(false);return nullptr;
}
extern "C" void bridge_clean(uint64_t a,uint64_t n) {assert(allocated.count(a) && allocated[a]==n);++cleaned;}
static EFI_MEMORY_DESCRIPTOR desc(unsigned type,uint64_t a,uint64_t n) {
    EFI_MEMORY_DESCRIPTOR d{};d.Type=type;d.PhysicalStart=a;d.NumberOfPages=n/4096;return d;
}
static EFI_STATUS EFIAPI getMap(UINTN *bytes,EFI_MEMORY_DESCRIPTOR *p,UINTN *key,UINTN *stride,UINT32 *version) {
    assert(*bytes>=descriptors.size()*sizeof(*p));*bytes=descriptors.size()*sizeof(*p);
    memcpy(p,descriptors.data(),*bytes);*key=1;*stride=sizeof(*p);*version=1;return EFI_SUCCESS;
}
static EFI_STATUS EFIAPI allocate(EFI_ALLOCATE_TYPE kind,EFI_MEMORY_TYPE type,UINTN pages,EFI_PHYSICAL_ADDRESS *a) {
    assert(kind==AllocateAddress && type==EfiLoaderData && !(*a&0x3fff));
    if(++allocations==failAllocation)return EFI_OUT_OF_RESOURCES;
    const uint64_t n=pages*4096;
    for(size_t i=0;i<descriptors.size();++i) {
        const auto old=descriptors[i];const auto end=old.PhysicalStart+old.NumberOfPages*4096;
        if(old.Type!=EfiConventionalMemory || *a<old.PhysicalStart || *a>end || n>end-*a)continue;
        void *mapped=mmap(reinterpret_cast<void *>(*a),n,PROT_READ|PROT_WRITE,
                          MAP_PRIVATE|MAP_ANONYMOUS|MAP_FIXED_NOREPLACE,-1,0);
        assert(mapped==reinterpret_cast<void *>(*a));allocated[*a]=n;
        descriptors[i]=desc(type,*a,n);
        if(*a>old.PhysicalStart)descriptors.push_back(desc(EfiConventionalMemory,old.PhysicalStart,*a-old.PhysicalStart));
        if(*a+n<end)descriptors.push_back(desc(EfiConventionalMemory,*a+n,end-*a-n));
        return EFI_SUCCESS;
    }
    return EFI_OUT_OF_RESOURCES;
}
static EFI_STATUS EFIAPI freePages(EFI_PHYSICAL_ADDRESS a,UINTN pages) {
    assert(allocated.count(a) && allocated[a]==pages*4096);
    assert(!munmap(reinterpret_cast<void *>(a),pages*4096));allocated.erase(a);++frees;
    for(auto &d:descriptors)if(d.PhysicalStart==a && d.NumberOfPages==pages){d.Type=EfiConventionalMemory;return EFI_SUCCESS;}
    assert(false);return EFI_DEVICE_ERROR;
}
static void initial() {
    assert(allocated.empty());allocations=failAllocation=frees=cleaned=0;
    descriptors={desc(0,0x86000000,0x200000),desc(0,0x8d000000,0x7000000),desc(0,0x94500000,0x200000),
                 desc(2,0xa0000000,0x10000000),desc(7,0xb0000000,0x1000000),desc(0,0x88f00000,0x200000)};
    MemoryHandoff h;h.mbaBytes=52;h.modemBytes=52;h.modemOffset=64;h.mbaHash[0]=1;h.modemHash[0]=2;
#ifdef TEST_CATALOG_HANDOFF
    h.version=2;h.filesOffset=h.bootImageBytes();h.filesBytes=16;h.filesHash[0]=3;
#endif
    assert(h.encode(tree.record,recordBytes,false)==Result::OK);memset(tree.ranges,0,sizeof(tree.ranges));
}
static int finalize() {return bridge_baseband_finalize(&tree,descriptors.data(),descriptors.size()*sizeof(descriptors[0]),sizeof(descriptors[0]),0xa0000000,0x10000000);}
int main() {
    EFI_BOOT_SERVICES bs{};bs.GetMemoryMap=getMap;bs.AllocatePages=allocate;bs.FreePages=freePages;
    initial();failAllocation=1;
    assert(bridge_baseband_prepare(&bs,&tree) && allocated.empty() && !frees);
    initial();failAllocation=2;
    assert(bridge_baseband_prepare(&bs,&tree) && allocated.empty() && frees==1);
    initial();assert(!bridge_baseband_prepare(&bs,&tree) && allocated.size()==2 && cleaned==1);
    assert(bridge_baseband_prepare(&bs,&tree)); // No second owner/allocation.
    assert(!finalize());MemoryHandoff h;assert(h.decode(tree.record,recordBytes)==Result::OK);
#ifdef TEST_CATALOG_HANDOFF
    assert(h.version==2 && h.filesOffset==0x4000 && h.filesBytes==16 && h.image.bytes==0x8000);
#endif
    assert(h.layout.metadata.address==0xb0000000 && h.image.address==0xb0004000);
    for(unsigned i=0;i<5;++i)assert(read64(tree.ranges[i]) && read64(tree.ranges[i]+8));
    assert(read64(tree.ranges[4])==0x88f00000 && read64(tree.ranges[4]+8)==0x200000);
    descriptors.push_back(desc(7,0x88f00000,0x200000));assert(finalize());
    assert(!read32(tree.record+8) && !read64(tree.ranges[4]));descriptors.pop_back();assert(!finalize());
    // On an ExitBootServices retry, any conventional alias revokes all proof.
    descriptors.push_back(desc(7,0x8d001000,4096));assert(finalize());
    assert(!read32(tree.record+8));for(const auto &r:tree.ranges)assert(!read64(r) && !read64(r+8));
    descriptors.pop_back();assert(!finalize());
    descriptors.push_back(desc(2,h.image.address,0x4000));descriptors.back().Attribute=1ULL<<63;
    assert(finalize());descriptors.pop_back();
    descriptors[1].NumberOfPages-=1;assert(finalize());descriptors[1].NumberOfPages+=1;
    assert(!finalize());bridge_baseband_abort(&bs,&tree);
    assert(allocated.empty() && frees==2 && !read32(tree.record+8));
    assert(h.decode(tree.record,recordBytes,false)==Result::OK);
    // Abort leaves a reusable empty template and never frees MPSS/MBA/arena.
    assert(!bridge_baseband_prepare(&bs,&tree) && !finalize());bridge_baseband_abort(&bs,&tree);
    assert(allocated.empty() && frees==4);
}
'''
        with tempfile.TemporaryDirectory() as directory:
            out = Path(directory)
            (out / 'test.cpp').write_text(source)
            (out / 'payload.S').write_text('.section .rodata,"a"\n.globl baseband_firmware_blob\nbaseband_firmware_blob:\n.zero 16384\n.globl baseband_firmware_end\nbaseband_firmware_end:\n.section .note.GNU-stack,"",@progbits\n')
            inc = ROOT / 'boot/edk2-msm8998/Common/edk2/MdePkg/Include'
            command = ['c++', '-std=c++17', '-Wall', '-Wextra', '-Werror', '-fno-strict-aliasing', '-fshort-wchar',
                       '-I' + str(ROOT / 'boot/bridge'), '-I' + str(ROOT / 'drivers/include'),
                       '-I' + str(ROOT / 'drivers/baseband/QcomBaseband'), '-I' + str(inc), '-I' + str(inc / 'AArch64'),
                       str(out / 'test.cpp'), str(ROOT / 'boot/bridge/baseband_handoff.cpp'), str(out / 'payload.S'),
                       '-o', str(out / 'test')]
            subprocess.run(command, check=True)
            subprocess.run([str(out / 'test')], check=True)
            (out / 'payload.S').write_text((out / 'payload.S').read_text().replace('.zero 16384', '.zero 32768'))
            command.insert(1, '-DTEST_CATALOG_HANDOFF')
            subprocess.run(command, check=True)
            subprocess.run([str(out / 'test')], check=True)


if __name__ == '__main__':
    unittest.main()
