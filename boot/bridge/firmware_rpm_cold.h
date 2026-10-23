// SPDX-License-Identifier: GPL-2.0-only
#pragma once
#include "firmware_image.h"
#include "../../drivers/power/QcomRPM/RPMTransport.h"
#include "../../drivers/usb/QcomUSB/USBFirmware.h"

namespace BridgeFirmware {
constexpr EFI_GUID GlinkProtocolGUID={0x3bfcf64d,0x00cd,0x41cb,{0xbe,0xa5,0xa7,0x6c,0x3a,0x24,0x3d,0xed}};
constexpr EFI_GUID GlinkExitGUID={0x27abf055,0xb1b8,0x4c26,{0x80,0x48,0x74,0x8f,0x37,0xba,0xa2,0xdf}};
constexpr const char *RPMColdProperty="qcom,rpm-transport-cold-handoff";
// IO reads the always-on RPM SRAM, never USB/PHY/SMMU registers. The pinned
// GLINK EBS callback requests APSS cleanup then waits for all four cursors
// to reset. Publication still independently checks that exact final fence.
template<class IO> class RPMCold {
    Memory memory;
    CodePin pin;
    const uint8_t *image=nullptr,*transport=nullptr;
    uint8_t *output=nullptr;
    QcomPower::RPMTransportCold record{};
    uint32_t toc[62]{},tocWords=0;
    uint64_t event=0;
    uint64_t servicesAddress=0;
    bool prepared=false,published=false;
    static constexpr unsigned OpOffsets[]={8,16,24,32,40,48,56,96,120,128,136,144,152};
    static constexpr unsigned OpRVAs[]={0x4d54,0x4f28,0x4f80,0x4f88,0x4fe0,0x5018,0x5070,0x50c8,0x5110,0x55ec,0x5150,0x5648,0x5610};
    uint64_t global(unsigned at) const {return pair(image+at);}
    bool fail() {
        if (output) for(unsigned i=0;i<sizeof(record);++i)output[i]=0;
        prepared=published=false;return false;
    }
    bool immutable() const {
        if (!image || !transport || !pin.matches(image,0xc000,memory) ||
            !equal(image+0x9020,&GlinkExitGUID,sizeof(GlinkExitGUID)) ||
            global(0xaa10)!=reinterpret_cast<uintptr_t>(transport) || global(0xaa18)!=1 ||
            global(0xaa08)!=0x17911008 || global(0xaa28)!=0x778000 || word(image+0xaa30)!=0x7000 ||
            global(0xaa38)!=0x778000 || global(0xaa40)!=0x77ef00 || global(0x9150)!=event ||
            global(0x9170)!=servicesAddress) return false;
        for(unsigned i=0;i<sizeof(OpOffsets)/sizeof(OpOffsets[0]);++i)
            if(pair(transport+OpOffsets[i])!=reinterpret_cast<uintptr_t>(image)+OpRVAs[i])return false;
        return pair(transport+184)==reinterpret_cast<uintptr_t>(image)+0x7cd8 &&
            pair(transport+192)==0x778000+record.txOffset && pair(transport+200)==0x778000+record.rxOffset &&
            pair(transport+208)==0x778008+record.txOffset && pair(transport+216)==0x778008+record.rxOffset &&
            word(transport+224)==record.txLength && word(transport+228)==record.rxLength;
    }
    bool tocUnchanged() const {
        for(unsigned i=0;i<tocWords;++i)if(IO::read(0x6f00+4*i)!=toc[i])return false;
        return true;
    }
    bool layout() {
        toc[0]=IO::read(0x6f00);toc[1]=IO::read(0x6f04);
        if(toc[0]!=0x67727430 || !toc[1] || toc[1]>20)return false;
        tocWords=2+3*toc[1];for(unsigned i=2;i<tocWords;++i)toc[i]=IO::read(0x6f00+4*i);
        bool tx=false,rx=false;
        for(unsigned i=0;i<toc[1];++i) {
            const auto id=toc[2+3*i],at=toc[3+3*i],length=toc[4+3*i];
            if(id!=0x61703272 && id!=0x72326170)continue;
            if((at|length)&7 || length<512 || length>0x6ef8 || at>0x6ef8-length)return false;
            if(id==0x61703272) {if(tx)return false;tx=true;record.txOffset=at;record.txLength=length;}
            else {if(rx)return false;rx=true;record.rxOffset=at;record.rxLength=length;}
        }
        return tx && rx && (record.txOffset+record.txLength+8<=record.rxOffset ||
            record.rxOffset+record.rxLength+8<=record.txOffset) && tocUnchanged();
    }
public:
    bool prepare(EFI_BOOT_SERVICES *services,const void *reference,unsigned bytes,uint32_t nextMessageID,
                 void *destination,unsigned outputBytes) {
        if(prepared || published || !nextMessageID || outputBytes!=sizeof(record) || !memory.prepare(services) ||
            !memory.contains(destination,outputBytes,1u<<EfiLoaderData) || !pin.prepare(reference,bytes) ||
            pin.size()!=0xc000 || pin.textRVA()!=0x1000 || pin.textBytes()!=0x8000 ||
            pin.interfaceRVA()!=0x9090 || pin.interfaceBytes()!=0x78)return false;
        output=static_cast<uint8_t *>(destination);fail();
        servicesAddress=reinterpret_cast<uintptr_t>(services);
        EFI_HANDLE list[64]{};unsigned n=0;auto guid=GlinkProtocolGUID;void *protocol=nullptr;
        if(!handles(services,guid,list,64,n))return false;
        for(unsigned i=0;i<n;++i) {
            void *p=nullptr;
            if(EFI_ERROR(services->HandleProtocol(list[i],&guid,&p)) || !memory.contains(p,8) ||
                pair(p)!=0x10001 || (protocol && protocol!=p))return false;
            protocol=p;
        }
        image=loadedImage(services,protocol,pin,memory);
        if(!image)return false;
        const auto address=global(0xaa10);event=global(0x9150);
        if(!event || !memory.contains(reinterpret_cast<const void *>(address),232))return false;
        transport=reinterpret_cast<const uint8_t *>(address);
        record.ramBytes=0x7000;record.nextMessageID=nextMessageID;
        if(!layout() || !immutable())return fail();
        prepared=true;return true;
    }
    bool finish(const USB::FirmwareStop &stop) {
        if(!prepared || !stop.valid() || !immutable() || !tocUnchanged() || IO::read(record.txOffset) || IO::read(record.txOffset+4) ||
            IO::read(record.rxOffset) || IO::read(record.rxOffset+4))return fail();
        if(published)return equal(output,&record,sizeof(record)) ? true:fail();
        record.flags=QcomPower::RPMTransportCold::Required;
        if(!record.valid())return fail();
        const auto *p=reinterpret_cast<const uint8_t *>(&record);
        for(unsigned i=0;i<sizeof(record);++i)output[i]=p[i];
        published=true;return true;
    }
};
}
