// SPDX-License-Identifier: GPL-2.0-only
#pragma once
#include "firmware_image.h"
#include "../../drivers/power/QcomRPM/RPMFirmwareRequests.h"
#include "../../drivers/power/QcomRPM/RPMClockHandoff.h"
#include "../../drivers/power/QcomRPM/RPMInterconnect.h"
#include "../../drivers/power/QcomRPM/RPMRails.h"

namespace BridgeFirmware {
constexpr EFI_GUID NPAProtocolGUID={0x79d6c870,0x725e,0x489e,{0xa0,0xa1,0x27,0xe7,0xa5,0xd0,0xcb,0x35}};
class RPMRequests {
    Memory memory;
    CodePin pin;
    struct Capture { const uint8_t *source;unsigned offset,bytes; } captures[2048]{};
    uint8_t saved[524288]{};
    unsigned captureCount=0,captureBytes=0;
    QcomPower::FirmwareRequests result{};
    uint8_t *destination=nullptr;
    bool ready=false;
    uint32_t nextID=0;
    const uint8_t *read(const void *data,unsigned bytes) {
        if (!memory.contains(data,bytes)) return nullptr;
        const auto *p=static_cast<const uint8_t *>(data);
        for (unsigned i=0;i<captureCount;++i) {
            const auto &c=captures[i];
            if (c.source==p && c.bytes==bytes) return equal(p,saved+c.offset,bytes) ? saved+c.offset:nullptr;
        }
        if (captureCount==2048 || bytes>sizeof(saved)-captureBytes) return nullptr;
        for (unsigned i=0;i<bytes;++i) saved[captureBytes+i]=p[i];
        const auto offset=captureBytes;
        captures[captureCount++]={p,offset,bytes};captureBytes+=bytes;return saved+offset;
    }
    const uint8_t *pointer(uint64_t address,unsigned bytes) {return read(reinterpret_cast<const void *>(address),bytes);}
    void row(uint32_t type,uint32_t resource,uint32_t key) {
        auto &r=result.rows[result.count++];r.type=type;r.resource=resource;r.key=key;
    }
    void selectors() {
        using namespace QcomPower;
        static_assert(RPMHandoffClockCount+RPMHandoffClockLimitCount+5+4+2*RPMRailCount<=160,"bounded firmware request selectors");
        for (const auto &c:rpmHandoffClocks) row(c.type,c.resource,c.key);
        for(unsigned i=0;i<RPMHandoffClockCount;++i)if(rpmClockHasLimit(i)) {
            const auto &c=rpmHandoffClocks[i];row(c.type,c.resource,0x0078616d);
        }
        for (const auto &b:rpmBusResources) if (!b.clock) row(b.type,b.resource,0x00007762);
        const uint32_t domains[]={0x78637772,0x786d7772,0x63737772,0x6d737772};
        for (auto type:domains) row(type,0,0x6c766c76);
        for (const auto &r:rpmRails) {row(r.type,r.resource,0x6e657773);row(r.type,r.resource,0x00007675);}
    }
    bool value(const uint8_t *view,QcomPower::FirmwareRequestValue &out) {
        const auto address=pair(view);const auto present=view[8];
        if (present>1 || (!address && present)) return false;
        if (!address) return true;
        const auto *array=pointer(address,24);if (!array) return false;
        const auto used=word(array+4),capacity=word(array+8);
        if (used>capacity || capacity>4096 || (!present && used) || (present && used!=4 && used!=8)) return false;
        if (!present) return true;
        const auto *data=pointer(pair(array+16),used);if (!data) return false;
        out.present=1;out.bytes=used;out.value=used==8 ? pair(data):word(data);return true;
    }
    bool resource(const uint8_t *data) {
        const auto identity=pair(data);const auto keys=word(data+8);
        if (keys>128) return false;
        for (unsigned n=0;n<3;++n) if (data[24+n]>1) return false;
        bool selected=false;
        for (unsigned i=0;i<result.count;++i) {
            auto &r=result.rows[i];
            if ((uint64_t(r.type)<<32|r.resource)==identity) {
                r.presence=1;selected=true;
                for (unsigned n=0;n<3;++n) r.resourceDirty[n]=data[24+n];
            }
        }
        if (!selected) return true;
        if (!keys) return true;
        const auto *entries=pointer(pair(data+16),keys*96);if (!entries) return false;
        uint32_t previous=0;
        for (unsigned k=0;k<keys;++k) {
            const auto *entry=entries+96*k;const auto key=word(entry);
            if (k && key<=previous) return false;
            previous=key;
            for (unsigned i=0;i<result.count;++i) {
                auto &r=result.rows[i];
                if ((uint64_t(r.type)<<32|r.resource)!=identity || r.key!=key) continue;
                r.presence=3;
                for (unsigned n=0;n<3;++n) {if (entry[88+n]>1) return false;r.dirty[n]=entry[88+n];}
                if (!value(entry+8,r.activeOrNextSubmitted) || !value(entry+24,r.activeRequested) ||
                    !value(entry+40,r.sleepSubmitted) || !value(entry+56,r.sleepRequested) ||
                    !value(entry+72,r.nextRequested)) return false;
            }
        }
        return true;
    }
    bool queues(const uint8_t *base) {
        const auto *global=read(base+0x18c00,8);if (!global) return false;
        const auto *mq=pointer(pair(global),40);if (!mq || !word(mq)) return false;
        nextID=word(mq);
        for (unsigned i=0;i<3;++i) {
            const auto *queue=pointer(pair(mq+8+8*i),24);if (!queue) return false;
            const auto count=word(queue+4);const auto first=pair(queue+8),last=pair(queue+16);
            if (word(queue)!=32 || count>2048 || bool(count)!=bool(first) || bool(count)!=bool(last)) return false;
            result.queueCounts[i]=count;
        }
        return true;
    }
    bool unchanged() const {
        for (unsigned i=0;i<captureCount;++i) {
            const auto &c=captures[i];if (!equal(c.source,saved+c.offset,c.bytes)) return false;
        }
        return true;
    }
    void clear() {
        if (destination) for (unsigned i=0;i<sizeof(result);++i) destination[i]=0;
        destination=nullptr;ready=false;
    }
public:
    uint32_t nextMessageID() const {return ready ? nextID:0;}
    // The pin is embedded by the unified builder after hashing NpaDxe. All
    // private offsets below refer exclusively to that binary, revision10002.
    // No NPA/GLINK callback is invoked, before or after ExitBootServices.
    bool prepare(EFI_BOOT_SERVICES *services,const void *reference,unsigned referenceBytes,void *output,unsigned outputBytes) {
        clear();captureCount=captureBytes=0;
        // Keep the EFI stack small; aggregate assignment of this record can
        // introduce a20 KiB temporary and a platform stack-probe dependency.
        auto *reset=reinterpret_cast<uint8_t *>(&result);
        for (unsigned i=0;i<sizeof(result);++i) reset[i]=0;
        result.version=2;result.bytes=sizeof(result);
        if (outputBytes!=sizeof(result) || !memory.prepare(services) || !memory.contains(output,outputBytes,1u<<EfiLoaderData) ||
            !pin.prepare(reference,referenceBytes) || pin.interfaceRVA()!=0x180f0 || pin.interfaceBytes()!=0x1b0) return false;
        destination=static_cast<uint8_t *>(output);clear();destination=static_cast<uint8_t *>(output);
        EFI_HANDLE list[64]{};unsigned count=0;
        if (!handles(services,NPAProtocolGUID,list,64,count)) {clear();return false;}
        const uint8_t *base=nullptr;void *interface=nullptr;auto guid=NPAProtocolGUID;
        for (unsigned i=0;i<count;++i) {
            void *candidate=nullptr;
            if (EFI_ERROR(services->HandleProtocol(list[i],&guid,&candidate)) || !memory.contains(candidate,8)) {clear();return false;}
            if (pair(candidate)!=0x10002) continue;
            if (interface && interface!=candidate) {clear();return false;}
            interface=candidate;
        }
        if (!interface || !(base=loadedImage(services,interface,pin,memory)) ||
            !read(base+pin.textRVA(),pin.textBytes()) || !read(interface,pin.interfaceBytes())) {clear();return false;}
        selectors();
        const auto *initialized=read(base+0x18bf8,4),*state=read(base+0x18c4c,4),*automatic=read(base+0x18c64,1);
        const auto *globals=read(base+0x18c08,16);
        if (!initialized || !state || !automatic || !globals || word(initialized)!=1 || *state>1 || *automatic>1) {clear();return false;}
        result.initialized=1;result.channelUp=*state;result.autocomplete=*automatic;
        for (unsigned i=0;i<3;++i) {
            if (state[i+1]>1) {clear();return false;}
            result.setDirty[i]=state[i+1];
        }
        const auto resources=word(globals);const auto address=pair(globals+8);
        if (resources>2048 || (!resources && address)) {clear();return false;}
        const auto *array=resources ? pointer(address,resources*8):nullptr;
        if ((resources && !array) || !queues(base)) {clear();return false;}
        uint64_t previous=0;
        for (unsigned i=0;i<resources;++i) {
            const auto *entry=pointer(pair(array+8*i),32);
            if (!entry || (i && pair(entry)<=previous) || !resource(entry)) {clear();return false;}
            previous=pair(entry);
        }
        if (!unchanged() || !pin.matches(base,0x1b000,memory)) {clear();return false;}
        const auto *p=reinterpret_cast<const uint8_t *>(&result);
        for (unsigned i=0;i<sizeof(result);++i) destination[i]=p[i];
        ready=true;return true;
    }
    bool finish() {
        // EFI objects may be uninstalled now. Only previously bounded RAM is
        // compared; never rediscover a protocol or dereference a new pointer.
        if (!ready || !destination || !unchanged() || !equal(destination,&result,sizeof(result))) {clear();return false;}
        return true;
    }
};
}
