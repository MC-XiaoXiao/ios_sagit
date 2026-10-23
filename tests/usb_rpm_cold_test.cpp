// SPDX-License-Identifier: GPL-2.0-only
// Execute the production EFI fence reader against modeled RAM/SRAM. The
// actual GLINK code pin is also relocated for comparison, never executed.
#include <cassert>
#include <cstdio>
#include <cstring>
#include <new>
#include <map>
#include <vector>
#include "../boot/bridge/firmware_rpm_cold.h"
#include "../boot/bridge/usb_hardware_stop.h"
namespace {
alignas(4096) uint8_t image[0xc000]{},heap[4096]{},output[4096]{};
uint32_t sram[0x7000/4]{};
EFI_LOADED_IMAGE_PROTOCOL loaded{};
EFI_BOOT_SERVICES services{};
std::vector<uint8_t> pin;
bool exited=false;
unsigned calls=0;
void word(void *at,uint32_t value) {auto *p=static_cast<uint8_t *>(at);for(unsigned i=0;i<4;++i)p[i]=uint8_t(value>>(8*i));}
void pair(void *at,uint64_t value) {word(at,uint32_t(value));word(static_cast<uint8_t *>(at)+4,uint32_t(value>>32));}
uint64_t address(const void *p) {return reinterpret_cast<uintptr_t>(p);}
struct IO {static uint32_t read(unsigned at) {assert(at<0x7000 && !(at&3));return sram[at/4];}};
using Cold=BridgeFirmware::RPMCold<IO>;
alignas(Cold) uint8_t storage[sizeof(Cold)]{};
Cold *cold=nullptr;
EFI_STATUS EFIAPI memoryMap(UINTN *bytes,EFI_MEMORY_DESCRIPTOR *out,UINTN *key,UINTN *stride,UINT32 *version) {
    assert(!exited);++calls;
    EFI_MEMORY_DESCRIPTOR map[4]{};const void *p[]={image,heap,output,&loaded};
    const unsigned sizes[]={sizeof(image),sizeof(heap),sizeof(output),sizeof(loaded)};
    for(unsigned i=0;i<4;++i) {
        const auto first=address(p[i])&~4095ULL,last=(address(p[i])+sizes[i]+4095)&~4095ULL;
        map[i].Type=i==2 ? EfiLoaderData:EfiBootServicesCode;
        map[i].PhysicalStart=first;map[i].NumberOfPages=(last-first)/4096;
    }
    assert(*bytes>=sizeof(map));std::memcpy(out,map,sizeof(map));*bytes=sizeof(map);*stride=sizeof(map[0]);*key=1;*version=1;return EFI_SUCCESS;
}
EFI_STATUS EFIAPI locate(EFI_LOCATE_SEARCH_TYPE type,EFI_GUID *guid,void *key,UINTN *bytes,EFI_HANDLE *list) {
    assert(!exited && type==ByProtocol && !key);++calls;
    const EFI_GUID images=EFI_LOADED_IMAGE_PROTOCOL_GUID;
    const bool glink=!std::memcmp(guid,&BridgeFirmware::GlinkProtocolGUID,sizeof(*guid));
    assert(glink || !std::memcmp(guid,&images,sizeof(*guid)));
    assert(*bytes==(glink ? 64u:256u)*sizeof(EFI_HANDLE));*bytes=sizeof(EFI_HANDLE);
    list[0]=reinterpret_cast<void *>(glink ? 1:2);return EFI_SUCCESS;
}
EFI_STATUS EFIAPI protocol(EFI_HANDLE handle,EFI_GUID *guid,void **out) {
    assert(!exited);++calls;
    if(!std::memcmp(guid,&BridgeFirmware::GlinkProtocolGUID,sizeof(*guid))) {
        assert(handle==reinterpret_cast<void *>(1));*out=image+0x9090;
    } else {assert(handle==reinterpret_cast<void *>(2));*out=&loaded;}
    return EFI_SUCCESS;
}
void installPin() {
    const auto count=BridgeFirmware::word(pin.data()+28),offset=40+4*count;
    assert(offset+0x8000+0x78==pin.size());
    std::memcpy(image+0x1000,pin.data()+offset,0x8000);std::memcpy(image+0x9090,pin.data()+offset+0x8000,0x78);
    for(unsigned i=0;i<count;++i) {
        const auto rva=BridgeFirmware::word(pin.data()+40+4*i);
        assert((rva>=0x1000 && rva+8<=0x9000) || (rva>=0x9090 && rva+8<=0x9108));
        pair(image+rva,address(image)+BridgeFirmware::pair(image+rva));
    }
}
void fixture() {
    exited=false;calls=0;
    std::memset(image,0,sizeof(image));std::memset(heap,0,sizeof(heap));std::memset(output,0,sizeof(output));std::memset(sram,0,sizeof(sram));
    pin.assign(40+0x8000+0x78,0);
    const unsigned header[]={0x31495046,1,0xc000,0x1000,0x8000,0x9090,0x78,0,0,0};
    for(unsigned i=0;i<10;++i)word(pin.data()+4*i,header[i]);
    pair(pin.data()+40+0x8000,0x10001);installPin();
    std::memcpy(image+0x9020,&BridgeFirmware::GlinkExitGUID,sizeof(EFI_GUID));
    pair(image+0xaa08,0x17911008);pair(image+0xaa10,address(heap));pair(image+0xaa18,1);
    pair(image+0xaa28,0x778000);word(image+0xaa30,0x7000);pair(image+0xaa38,0x778000);pair(image+0xaa40,0x77ef00);
    pair(image+0x9150,0x100);pair(image+0x9170,address(&services));
    const unsigned offsets[]={8,16,24,32,40,48,56,96,120,128,136,144,152};
    const unsigned rvas[]={0x4d54,0x4f28,0x4f80,0x4f88,0x4fe0,0x5018,0x5070,0x50c8,0x5110,0x55ec,0x5150,0x5648,0x5610};
    for(unsigned i=0;i<13;++i)pair(heap+offsets[i],address(image)+rvas[i]);
    pair(heap+184,address(image)+0x7cd8);pair(heap+192,0x778000);pair(heap+200,0x778808);
    pair(heap+208,0x778008);pair(heap+216,0x778810);word(heap+224,2048);word(heap+228,2048);
    const unsigned toc[]={0x67727430,2,0x61703272,0,2048,0x72326170,0x808,2048};
    for(unsigned i=0;i<8;++i)sram[0x6f00/4+i]=toc[i];
    loaded={};loaded.Revision=EFI_LOADED_IMAGE_PROTOCOL_REVISION;loaded.ImageBase=image;loaded.ImageSize=sizeof(image);
    loaded.ImageCodeType=EfiBootServicesCode;loaded.ImageDataType=EfiBootServicesData;
    services={};services.GetMemoryMap=memoryMap;services.LocateHandle=locate;services.HandleProtocol=protocol;
    if(cold)cold->~Cold();
    cold=new(storage) Cold;
}
bool prepare(uint32_t next=17) {return cold->prepare(&services,pin.data(),unsigned(pin.size()),next,output,40);}
bool empty() {for(unsigned i=0;i<40;++i)if(output[i])return false;return true;}
USB::FirmwareStop stop() {
    USB::FirmwareStop r;r.flags=r.Required;r.previousMode=1;r.controller=0xa800000;r.length=0x100000;
    r.removedInterfaces=1;assert(r.valid());return r;
}
QcomPower::RPMTransportCold result() {QcomPower::RPMTransportCold r;std::memcpy(&r,output,sizeof(r));return r;}
}
void USBRPMColdTests() {
    fixture();sram[1]=16;sram[0x808/4+1]=24;assert(prepare() && empty());
    sram[1]=sram[0x808/4+1]=0;exited=true;const auto before=calls;loaded={};services={};
    assert(cold->finish(stop()) && calls==before);auto r=result();
    assert(r.valid() && r.nextMessageID==17 && r.txOffset==0 && r.rxOffset==0x808 && r.ramBytes==0x7000);
    assert(cold->finish(stop()));output[0]^=1;assert(!cold->finish(stop()) && empty());
    for(unsigned f=0;f<17;++f) {
        fixture();
        switch(f) {
        case 0:image[0x1100]^=1;break;
        case 1:pair(image+0xaa10,1);break;
        case 2:pair(image+0xaa18,2);break;
        case 3:pair(image+0xaa28,0x779000);break;
        case 4:pair(image+0xaa08,0x17911010);break;
        case 5:pair(image+0x9150,0);break;
        case 6:pair(image+0x9170,1);break;
        case 7:pair(heap+40,address(image)+0x4fe4);break;
        case 8:pair(heap+192,0x778008);break;
        case 9:word(heap+224,1024);break;
        case 10:sram[0x6f00/4]=0;break;
        case 11:sram[0x6f00/4+1]=21;break;
        case 12:sram[0x6f00/4+6]=0;break;
        case 13:sram[0x6f00/4+2]=0x72326170;break;
        case 14:sram[0x6f00/4+4]=0x7000;break;
        case 15:loaded.ImageSize--;break;
        case 16:image[0x9020]^=1;break;
        }
        assert(!prepare() && empty());
    }
    for(unsigned f=0;f<10;++f) {
        fixture();assert(prepare());exited=true;const auto n=calls;
        switch(f) {
        case 0:sram[0]=8;break;
        case 1:sram[1]=8;break;
        case 2:sram[0x808/4]=8;break;
        case 3:sram[0x808/4+1]=8;break;
        case 4:pair(heap+200,0x778800);break;
        case 5:image[0x1100]^=1;break;
        case 6:sram[0x6f00/4+4]+=8;break;
        case 7:pair(image+0xaa10,1);break;
        case 8:pair(image+0x9150,0);break;
        case 9:image[0x9020]^=1;break;
        }
        assert(!cold->finish(stop()) && empty() && calls==n);
    }
    fixture();assert(!prepare(0));
    for(unsigned f=0;f<12;++f) {
        fixture();assert(prepare());auto bad=stop();
        switch(f) {
        case 0:bad.flags=0;break;
        case 1:bad.grants=1;break;
        case 2:bad.controller=0;break;
        case 3:bad.length=0x1000001;break;
        case 4:bad.controller=UINT64_MAX-4095;break;
        case 5:bad.previousMode=2;break;
        case 6:bad.coreType=0;break;
        case 7:bad.closedEvents=5;break;
        case 8:bad.removedInterfaces=0;break;
        case 9:bad.removedInterfaces=65;break;
        case 10:bad.reserved=1;break;
        case 11:bad.padding=1;break;
        }
        assert(!cold->finish(bad) && empty());
    }
    std::puts("USB RPM cold fence: pinned GLINK transport/TOC, real four-cursor reset, firmware message seed and post-exit revocation passed");
}
void USBRPMColdPinTest(const char *path) {
    fixture();FILE *file=std::fopen(path,"rb");assert(file);std::fseek(file,0,SEEK_END);const auto size=std::ftell(file);std::rewind(file);
    assert(size>40 && size<=0x100000);pin.resize(unsigned(size));assert(std::fread(pin.data(),1,pin.size(),file)==pin.size());std::fclose(file);
    assert(BridgeFirmware::word(pin.data()+28)==83);installPin();assert(prepare());exited=true;assert(cold->finish(stop()));
    std::puts("USB RPM cold fence: actual GlinkDxe code/protocol accepted with all83 load relocations without firmware execution");
}

void USBHardwareStopTests() {
    struct Registers {
        std::map<uint32_t,uint32_t> gcc,core;
        std::vector<uint32_t> writes;
        bool halted=false,stall=false,loseClock=false,loseDomain=false,loseReset=false,ignoreReset=false;
        bool loseResetBits=false,badTimer=false,failed=false;
        bool ignoreStop=false,changeRole=false;
        unsigned coreReads=0,gccReads=0,elapsed=0;
        Registers(bool host=false) {
            gcc[0xf000]=0x200;gcc[0xf004]=0x80000000;
            for(unsigned offset:{0x82024u,0x5018u,0xf008u,0xf010u,0xf00cu})gcc[offset]=0x40000001;
            core[DWC3::GSNPSID]=0x5533290a;core[DWC3::GHWPARAMS0]=(32u<<24)|2;
            core[DWC3::GHWPARAMS1]=2u<<15;core[DWC3::GHWPARAMS3]=(1u<<2)|(32u<<12)|(16u<<18);
            core[DWC3::GCTL]=host ? DWC3::PortHost:DWC3::PortDevice;
            core[DWC3::DCTL]=DWC3::Run|(1u<<19);core[0]=0x1000040;core[0x40]=5;
        }
        uint32_t readGCC(uint32_t offset) {
            assert(exited);++gccReads;
            if(halted && loseClock && offset==0x82024)gcc[offset]^=16;
            if(halted && loseDomain && offset==0xf004)gcc[offset]^=2;
            if(halted && loseReset && offset==0xf000)gcc[offset]^=2;
            return failed ? UINT32_MAX:gcc[offset];
        }
        uint32_t readCore(uint32_t offset) {
            assert(exited && offset<DWC3::Aperture);++coreReads;
            if(halted && changeRole && offset==DWC3::GCTL)core[offset]=DWC3::PortHost;
            return failed ? UINT32_MAX:core[offset];
        }
        void writeGCC(uint32_t offset,uint32_t value) {
            assert(exited && offset==0xf000 && (value&1) && halted);writes.push_back(0x100000+offset);
            if(!ignoreReset)gcc[offset]=loseResetBits ? 1:value;
        }
        void writeCore(uint32_t offset,uint32_t value) {
            assert(exited && offset<DWC3::Aperture);writes.push_back(0xa800000+offset);
            if(ignoreStop && (offset==DWC3::DCTL || offset==0x40))return;
            core[offset]=value;
            if(!stall && ((offset==DWC3::DCTL && !(value&DWC3::Run)) || (offset==0x40 && !(value&1)))) {
                halted=true;core[DWC3::DSTS]=DWC3::Halted;core[0x44]=1;
            }
        }
        void delay(unsigned us) {assert(exited);elapsed+=us;if(badTimer)failed=true;}
    };
    USB::FirmwareController binding;binding.base=0xa800000;binding.length=0x100000;
    binding.interrupt=163;binding.interruptFlags=1;
    auto record=[] {USB::HardwareStop r;std::memcpy(&r,output,sizeof(r));return r;};
    auto zero=[] {for(unsigned n=0;n<64;++n)if(output[n])return false;return true;};
    for(bool host:{false,true}) {
        fixture();Registers io(host);BridgeFirmware::HardwareStop<Registers> hardware(io);
        assert(hardware.prepare(&services,binding,output,64) && !io.coreReads && !io.gccReads && zero());
        const auto callCount=calls;exited=true;services={};
        assert(hardware.finish(stop()) && calls==callCount && io.halted && io.gcc[0xf000]==0x201);
        auto r=record();assert(r.valid() && r.previousRole==(host ? 1u:2u) && !r.grants && r.flags==r.Required);
        assert(io.writes.back()==0x10f000 && hardware.finish(stop()));
        io.gcc[0xf000]&=~1u;assert(!hardware.finish(stop()) && zero());
    }
    fixture();Registers reset;reset.gcc[0xf000]=0x201;reset.gcc[0xf004]=0;
    BridgeFirmware::HardwareStop<Registers> inherited(reset);
    assert(inherited.prepare(&services,binding,output,64));exited=true;
    assert(inherited.finish(stop()) && record().valid() && !record().previousRole &&
        (record().flags&USB::HardwareStop::InheritedReset) && !reset.coreReads && reset.writes.empty());
    output[10]^=1;assert(!inherited.finish(stop()) && zero());
    fixture();Registers retained;retained.gcc[0xf000]=0x201;
    BridgeFirmware::HardwareStop<Registers> owner(retained);
    assert(owner.prepare(&services,binding,output,64));exited=true;assert(owner.finish(stop()));
    auto changedStop=stop();changedStop.closedEvents=1;
    assert(changedStop.valid() && !owner.finish(changedStop) && zero());
    for(unsigned f=0;f<18;++f) {
        fixture();Registers io;BridgeFirmware::HardwareStop<Registers> hardware(io);
        assert(hardware.prepare(&services,binding,output,64));
        switch(f) {
        case 0:io.gcc[0xf000]=UINT32_MAX;break;
        case 1:io.gcc[0xf004]=0;break;
        case 2:io.gcc[0xf004]|=1;break;
        case 3:io.gcc[0x82024]=0;break;
        case 4:io.gcc[0xf008]=UINT32_MAX;break;
        case 5:io.core[DWC3::GSNPSID]=0;break;
        case 6:io.core[DWC3::GCTL]=DWC3::PortMask;break;
        case 7:io.stall=true;break;
        case 8:io.core[DWC3::DCTL]=UINT32_MAX;break;
        case 9:io.loseClock=true;break;
        case 10:io.loseDomain=true;break;
        case 11:io.loseReset=true;break;
        case 12:io.ignoreReset=true;break;
        case 13:io.loseResetBits=true;break;
        case 14:io.badTimer=true;break;
        case 15:io.core[DWC3::GHWPARAMS1]=0;break;
        case 16:io.ignoreStop=true;io.core[DWC3::DSTS]=DWC3::Halted;break;
        case 17:io.changeRole=true;break;
        }
        exited=true;assert(!hardware.finish(stop()) && zero());
        if(f<7 || f==15)assert(io.writes.empty());
        if(f==7)assert(io.elapsed==10000);
        if(f<12 || f>=15)for(auto address:io.writes)assert(address!=0x10f000);
    }
    for(unsigned f=0;f<4;++f) {
        fixture();Registers io;BridgeFirmware::HardwareStop<Registers> hardware(io);
        assert(hardware.prepare(&services,binding,output,64));auto bad=stop();
        if(f==0)bad.flags=0;
        if(f==1)bad.grants=1;
        if(f==2)bad.controller+=4096;
        if(f==3)bad.length+=4096;
        exited=true;assert(!hardware.finish(bad) && zero() && !io.coreReads && !io.gccReads);
    }
    puts("USB hardware stop: post-EFI Host/Device halt before BCR, inherited reset, bounded failures and pre-XNU revocation passed");
}
