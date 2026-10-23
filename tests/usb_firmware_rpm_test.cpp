// SPDX-License-Identifier: GPL-2.0-only
// Execute production snapshot code against bounded RAM and EFI mocks. The
// synthetic PE code is never called, and there is no MMIO or firmware boot.
#include <cassert>
#include <cstdio>
#include <cstring>
#include <vector>
#include "../boot/bridge/firmware_rpm.h"
namespace {
alignas(4096) uint8_t image[0x1b000]{},heap[0x20000]{},output[24576]{};
EFI_LOADED_IMAGE_PROTOCOL loaded{};
BridgeFirmware::RPMRequests snapshot;
std::vector<uint8_t> pin;
bool exited=false;
unsigned calls=0;
int fault=0;
void word(void *at,uint32_t v) {auto *p=static_cast<uint8_t *>(at);for(unsigned n=0;n<4;++n)p[n]=uint8_t(v>>(8*n));}
void pair(void *at,uint64_t v) {word(at,uint32_t(v));word(static_cast<uint8_t *>(at)+4,uint32_t(v>>32));}
uint64_t address(const void *p) {return reinterpret_cast<uintptr_t>(p);}
constexpr unsigned recordBytes=sizeof(QcomPower::FirmwareRequests);
EFI_STATUS EFIAPI memoryMap(UINTN *bytes,EFI_MEMORY_DESCRIPTOR *out,UINTN *key,UINTN *stride,UINT32 *version) {
    assert(!exited);++calls;
    if (fault==1) return EFI_DEVICE_ERROR;
    EFI_MEMORY_DESCRIPTOR map[4]{};
    const void *starts[]={image,heap,output,&loaded};
    const unsigned lengths[]={sizeof(image),sizeof(heap),sizeof(output),sizeof(loaded)};
    for(unsigned i=0;i<4;++i) {
        const auto first=address(starts[i])&~4095ULL,last=(address(starts[i])+lengths[i]+4095)&~4095ULL;
        map[i].Type=i==0 ? EfiBootServicesCode:EfiBootServicesData;
        if(i==2)map[i].Type=fault==18 ? EfiBootServicesData:EfiLoaderData;
        map[i].PhysicalStart=first;map[i].NumberOfPages=(last-first)/4096;
    }
    if (fault==2) map[0].Type=EfiMemoryMappedIO;
    if (fault==3) map[1].NumberOfPages=1;
    if (fault==4) map[0].PhysicalStart=UINT64_MAX-4095,map[0].NumberOfPages=UINT64_MAX;
    assert(*bytes>=sizeof(map));std::memcpy(out,map,sizeof(map));*bytes=sizeof(map);*key=1;
    *stride=fault==5 ? 1:sizeof(map[0]);*version=fault==6 ? 2:1;return EFI_SUCCESS;
}
EFI_STATUS EFIAPI locate(EFI_LOCATE_SEARCH_TYPE search,EFI_GUID *guid,void *key,UINTN *bytes,EFI_HANDLE *handles) {
    assert(!exited && search==ByProtocol && !key);++calls;
    const EFI_GUID images=EFI_LOADED_IMAGE_PROTOCOL_GUID;
    const bool npa=!std::memcmp(guid,&BridgeFirmware::NPAProtocolGUID,sizeof(*guid));
    assert(npa || !std::memcmp(guid,&images,sizeof(*guid)));
    if (fault==7) return EFI_NOT_FOUND;
    if (fault==8) return EFI_BUFFER_TOO_SMALL;
    assert(*bytes==(npa ? 64u:256u)*sizeof(EFI_HANDLE));
    *bytes=(fault==9 ? 0:fault==10 ? 3:sizeof(EFI_HANDLE));
    handles[0]=reinterpret_cast<void *>(npa ? 1:2);
    if (fault==11) handles[0]=nullptr;
    if (fault==12 || fault==13 || fault==14) {
        *bytes=2*sizeof(EFI_HANDLE);handles[1]=fault==12 ? handles[0]:reinterpret_cast<void *>(npa ? 3:4);
    }
    return EFI_SUCCESS;
}
EFI_STATUS EFIAPI protocol(EFI_HANDLE handle,EFI_GUID *guid,void **out) {
    assert(!exited);++calls;
    if (fault==15) return EFI_DEVICE_ERROR;
    if (fault==16) {*out=nullptr;return EFI_SUCCESS;}
    if (!std::memcmp(guid,&BridgeFirmware::NPAProtocolGUID,sizeof(*guid)))
        *out=image+(fault==13 && handle==reinterpret_cast<void *>(3) ? 0x180f8:0x180f0);
    else *out=&loaded;
    if(fault==17)loaded.Revision=0;
    return EFI_SUCCESS;
}
EFI_BOOT_SERVICES services{};
void fixture() {
    fault=0;exited=false;calls=0;std::memset(image,0,sizeof(image));std::memset(heap,0,sizeof(heap));
    std::memset(output,0,sizeof(output));loaded={};
    pin.assign(40+8+0x17000+0x1b0,0);
    const uint32_t header[]={0x31495046,1,sizeof(image),0x1000,0x17000,0x180f0,0x1b0,2,0,0};
    for(unsigned i=0;i<10;++i)word(pin.data()+4*i,header[i]);
    word(pin.data()+40,0x1010);word(pin.data()+44,0x180f8);
    pair(pin.data()+48+0x10,0x19000);pair(pin.data()+48+0x17000,0x10002);
    pair(pin.data()+48+0x17000+8,0x1000);
    std::memcpy(image+0x1000,pin.data()+48,0x17000);
    std::memcpy(image+0x180f0,pin.data()+48+0x17000,0x1b0);
    pair(image+0x1010,address(image)+0x19000);pair(image+0x180f8,address(image)+0x1000);
    loaded.ImageBase=image;loaded.ImageSize=sizeof(image);
    loaded.Revision=EFI_LOADED_IMAGE_PROTOCOL_REVISION;
    loaded.ImageCodeType=EfiBootServicesCode;loaded.ImageDataType=EfiBootServicesData;
    word(image+0x18bf8,1);image[0x18c4c]=1;
    pair(image+0x18c00,address(heap));
    for(unsigned i=0;i<4;++i) {
        pair(heap+8+8*i,address(heap+0x100+24*i));word(heap+0x100+24*i,32);
    }
    word(heap,7);
    // One real resource with a requested active value distinct from the
    // submitted value; a pending sleep request and a next-active request.
    word(image+0x18c08,1);pair(image+0x18c10,address(heap+0x200));
    pair(heap+0x200,address(heap+0x300));pair(heap+0x300,uint64_t(0x72676761)<<32|1);
    word(heap+0x308,1);pair(heap+0x310,address(heap+0x400));heap[0x318]=heap[0x319]=heap[0x31a]=1;
    word(heap+0x400,0x007a484b);
    const unsigned offsets[]={8,24,40,56,72};
    for(unsigned i=0;i<5;++i) {
        auto *entry=heap+0x400+offsets[i];auto *array=heap+0x500+24*i;
        pair(entry,address(array));entry[8]=i==2 ? 0:1;
        word(array+4,i==2 ? 0:4);word(array+8,4);pair(array+16,address(heap+0x600+8*i));
        word(heap+0x600+8*i,100+i);
    }
    heap[0x458]=heap[0x459]=heap[0x45a]=1;
    services={};services.GetMemoryMap=memoryMap;services.LocateHandle=locate;services.HandleProtocol=protocol;
}
bool prepare() {return snapshot.prepare(&services,pin.data(),unsigned(pin.size()),output,recordBytes);}
bool empty() {for(unsigned i=0;i<recordBytes;++i)if(output[i])return false;return true;}
QcomPower::FirmwareRequests result() {
    QcomPower::FirmwareRequests r;std::memcpy(&r,output,sizeof(r));return r;
}
}
void USBFirmwareRPMTests() {
    fixture();assert(prepare());auto r=result();
    assert(r.version==2 && r.bytes==recordBytes && r.count==134 && !r.grants && r.initialized==1 && r.channelUp==1);
    const auto &a=r.rows[0];assert(a.presence==3 && a.activeOrNextSubmitted.value==100 && a.activeRequested.value==101);
    assert(!a.sleepSubmitted.present && a.sleepRequested.value==103 && a.nextRequested.value==104);
    assert(a.dirty[0]==1 && a.dirty[1]==1 && a.dirty[2]==1 && a.resourceDirty[0]==1 && !r.rows[1].presence);
    const auto before=calls;exited=true;assert(snapshot.finish() && calls==before);
    // Closing/uninstalling EFI protocols does not authorize a late query.
    loaded={};services={};assert(snapshot.finish() && calls==before);
    // PIMEM is a distinct firmware clock resource, with independent sleep
    // observations. Its presence never turns the cache into an ACK or lease.
    fixture();pair(heap+0x300,uint64_t(0x6d656d70)<<32);assert(prepare());r=result();
    assert(!r.rows[0].presence && r.rows[26].type==0x6d656d70 && !r.rows[26].resource &&
        r.rows[26].key==0x007a484b && r.rows[26].presence==3 && !r.grants &&
        r.rows[26].activeRequested.value==101 && r.rows[26].sleepRequested.value==103);
    const auto pimemCalls=calls;exited=true;assert(snapshot.finish() && calls==pimemCalls);
    heap[0x618]^=1;assert(!snapshot.finish() && empty() && calls==pimemCalls);
    // MAX is a separate retained RPM field, never a substitute for a
    // requested rate. Capture each supported clock cap in both sets.
    unsigned limitRow=27;
    for(unsigned i=0;i<QcomPower::RPMHandoffClockCount;++i)if(QcomPower::rpmClockHasLimit(i)) {
        fixture();const auto &clock=QcomPower::rpmHandoffClocks[i];
        pair(heap+0x300,uint64_t(clock.type)<<32|clock.resource);word(heap+0x400,0x0078616d);
        assert(prepare());r=result();
        const auto &cap=r.rows[limitRow++];
        assert(cap.type==clock.type && cap.resource==clock.resource && cap.key==0x0078616d && cap.presence==3);
        assert(cap.activeOrNextSubmitted.value==100 && cap.activeRequested.value==101 && cap.sleepRequested.value==103);
        assert(r.rows[i].presence==1 && !r.rows[i].activeRequested.present && !r.grants);
        const auto queries=calls;exited=true;assert(snapshot.finish() && calls==queries);
        heap[0x608]^=1;assert(!snapshot.finish() && empty() && calls==queries);
    }
    assert(limitRow==39);
    for(int f=1;f<=18;++f) {
        fixture();fault=f;
        if(f==3)pair(heap+0x510,address(heap+0x2000));
        if(f==13)pair(image+0x180f8,0x10002); // distinct instance with matching revision
        if(f==14) { // NPA aliases are permitted; duplicate matching images are not.
            assert(!prepare() && empty());continue;
        }
        assert(!prepare() && empty());
    }
    for(unsigned f=0;f<24;++f) {
        fixture();
        switch(f) {
        case 0:image[0x1100]^=1;break;
        case 1:pair(image+0x1010,address(image)+0x19008);break;
        case 2:pair(image+0x180f8,address(image)+0x1008);break;
        case 3:loaded.ImageSize--;break;
        case 4:loaded.ImageDataType=EfiRuntimeServicesData;break;
        case 5:loaded.ImageCodeType=EfiLoaderCode;break;
        case 6:word(image+0x18bf8,0);break;
        case 7:image[0x18c4c]=2;break;
        case 8:image[0x18c64]=2;break;
        case 9:word(image+0x18c08,2049);break;
        case 10:pair(image+0x18c10,1);break;
        case 11:pair(heap+0x200,1);break;
        case 12:word(heap+0x308,129);break;
        case 13:pair(heap+0x310,1);break;
        case 14:heap[0x318]=2;break;
        case 15:heap[0x458]=2;break;
        case 16:heap[0x410]=2;break;
        case 17:pair(heap+0x408,1);break;
        case 18:word(heap+0x504,3);break;
        case 19:word(heap+0x508,2);break;
        case 20:pair(heap+0x510,1);break;
        case 21:word(heap+0x100,24);break;
        case 22:word(heap+0x104,2049);break;
        case 23:word(heap+0x104,1);break;
        }
        assert(!prepare() && empty());
    }
    for(unsigned f=0;f<9;++f) {
        fixture();assert(prepare());exited=true;const auto queries=calls;
        switch(f) {
        case 0:heap[0x600]^=1;break;
        case 1:heap[0x608]^=1;break;
        case 2:heap[0x618]^=1;break;
        case 3:heap[0x620]^=1;break;
        case 4:heap[0x400]^=1;break;
        case 5:image[0x18c08]^=1;break;
        case 6:image[0x1100]^=1;break;
        case 7:image[0x180f8]^=1;break;
        case 8:output[0]^=1;break;
        }
        assert(!snapshot.finish() && empty() && calls==queries);
    }
    // A firmware error can retire a queue entry; even the empty-queue and
    // autocomplete cases remain observations with grants==0.
    fixture();image[0x18c64]=1;assert(prepare());assert(!result().grants && result().autocomplete==1);
    fixture();word(image+0x18c08,0);pair(image+0x18c10,0);assert(prepare());
    for(const auto &row:result().rows)assert(!row.presence && !row.activeOrNextSubmitted.present && !row.activeRequested.present);
    fixture();word(heap+0x308,0);assert(prepare());assert(result().rows[0].presence==1);
    fixture();word(heap+0x308,2);std::memcpy(heap+0x460,heap+0x400,96);assert(!prepare() && empty());
    fixture();word(image+0x18c08,2);pair(heap+0x208,address(heap+0x300));assert(!prepare() && empty());
    fixture();word(pin.data()+40,0x1011);assert(!prepare() && empty());
    fixture();word(pin.data()+44,0x1010);assert(!prepare() && empty());
    fixture();pin.pop_back();assert(!prepare() && empty());
    fixture();services.HandleProtocol=nullptr;assert(!prepare() && empty());
    fixture();services.LocateHandle=nullptr;assert(!prepare() && empty());
    fixture();services.GetMemoryMap=nullptr;assert(!prepare() && empty());
    fixture();assert(!snapshot.prepare(&services,pin.data(),unsigned(pin.size()),output,recordBytes-1));
    fixture();assert(!snapshot.prepare(&services,pin.data(),unsigned(pin.size()),output,16448)); // old version1 frame
    std::puts("USB EFI RPM: relocated code/protocol pins,134 selected fields including12 MAX caps, bounded requested/submitted/next snapshots, absent-key semantics and post-exit revocation passed");
}
void USBFirmwareRPMPinTest(const char *path) {
    fixture();FILE *file=std::fopen(path,"rb");assert(file);
    std::fseek(file,0,SEEK_END);const auto length=std::ftell(file);std::rewind(file);
    assert(length>40 && length<=0x100000);pin.resize(unsigned(length));
    assert(std::fread(pin.data(),1,pin.size(),file)==pin.size());std::fclose(file);
    const auto count=BridgeFirmware::word(pin.data()+28),offset=40+4*count;
    assert(count==212 && offset+0x17000+0x1b0==pin.size());
    std::memcpy(image+0x1000,pin.data()+offset,0x17000);
    std::memcpy(image+0x180f0,pin.data()+offset+0x17000,0x1b0);
    for(unsigned i=0;i<count;++i) {
        const auto rva=BridgeFirmware::word(pin.data()+40+4*i);
        assert((rva>=0x1000 && rva+8<=0x18000) || (rva>=0x180f0 && rva+8<=0x182a0));
        const auto original=BridgeFirmware::pair(image+rva);assert(original<=sizeof(image));
        pair(image+rva,address(image)+original);
    }
    assert(prepare() && !result().grants);
    const auto relocation=BridgeFirmware::word(pin.data()+40);
    image[relocation]^=1;assert(!prepare() && empty());image[relocation]^=1;
    assert(prepare());exited=true;assert(snapshot.finish());
    std::puts("USB EFI RPM: actual NpaDxe code and protocol accepted with all212 load relocations; altered relocation rejected without execution");
}
