// SPDX-License-Identifier: BSD-2-Clause
#pragma once
#include "firmware_image.h"
#include "usb_firmware.h"

namespace BridgeUSB {
struct ConfigCalls {
    static EFI_STATUS count(ConfigProtocol *p,UINT8 *out) {return p->getCoreCount(p,out);}
    static EFI_STATUS base(ConfigProtocol *p,UINT64 *out) {return p->getBase(p,5,out);}
    static EFI_STATUS stop(ConfigProtocol *p,UINT32 mode) {return p->stopController(p,0,mode);}
};
// One-way firmware retirement. The model substitutes only calls into ARM64
// firmware; EFI service calls and all identity/state sequencing are shared.
// No clock, PHY, USB MMIO or SMMU write belongs to this step.
template<class Calls=ConfigCalls> class FirmwareStop {
    BridgeFirmware::Memory memory;
    BridgeFirmware::CodePin pin;
    EFI_BOOT_SERVICES *services=nullptr;
    uint8_t *image=nullptr,*output=nullptr;
    ConfigProtocol *root=nullptr,*client=nullptr;
    ConfigProtocol *interfaces[64]{};
    EFI_HANDLE list[64]{},remaining[64]{};
    unsigned count=0;
    unsigned stage=0;
    EFI_HANDLE rootHandle=nullptr;
    UINT64 events[4]{};
    UINT64 originalOverride=0,retiredOverride=0;
    USB::FirmwareStop record{};
    bool started=false,touched=false,stopped=false,published=false,aliasesDetached=false;
    static constexpr unsigned Root=0x112d8,Client=0x11208,Template=0x11140;
    static constexpr unsigned EventOffsets[4]={0x114f0,0x114c0,0x114b8,0x114b0};
    UINT64 read(unsigned offset) const {return BridgeFirmware::pair(image+offset);}
    void store(unsigned offset,UINT64 value) {
        *reinterpret_cast<volatile UINT64 *>(image+offset)=value;
        __atomic_thread_fence(__ATOMIC_SEQ_CST);
    }
    bool prefix(const ConfigProtocol *p) const {
        return memory.contains(p,160) && BridgeFirmware::equal(p,image+Template,136) &&
            BridgeFirmware::equal(reinterpret_cast<const uint8_t *>(p)+144,image+Template+144,16);
    }
    bool identities() const {
        return root==reinterpret_cast<ConfigProtocol *>(image+Root) &&
            client==reinterpret_cast<ConfigProtocol *>(image+Client) && prefix(root) && prefix(client) &&
            root->core==1 && root->mode==0x10000 && client->core==0;
    }
    bool query(const USB::FirmwareController &controller) {
        UINT8 cores=0;UINT64 address=0;
        return !EFI_ERROR(Calls::count(root,&cores)) && cores==1 &&
            !EFI_ERROR(Calls::base(root,&address)) && address==controller.base && identities();
    }
    unsigned preflightStatus() const {
        if (!identities()) return 1;
        if (read(0x112c0)!=reinterpret_cast<uintptr_t>(rootHandle)) return 2;
        const auto mode=client->mode;const auto override=read(0x112a8);
        if (mode!=1 && mode!=4 && mode!=0x10000) return 3;
        if (override!=originalOverride || (override&255)>1 ||
            ((override&255) && UINT32(override>>32)!=1 && UINT32(override>>32)!=4)) return 4;
        const auto host=read(0x111e8),device=read(0x111f0);
        if (mode==0x10000 ? (host || device) : (!host && !device)) return 5;
        const UINT64 owners[2]={host,device};
        for (const auto address:owners) if (address) {
            const auto active=reinterpret_cast<EFI_HANDLE>(address);bool found=false;
            for (unsigned i=0;i<count;++i) if (list[i]==active && active!=rootHandle) found=true;
            if (!found) return 6;
        }
        for (unsigned i=0;i<4;++i) {
            if (read(EventOffsets[i])!=events[i]) return 7;
            if (i && !events[i]) return 8+i;
            for (unsigned j=0;j<i;++j) if (events[i] && events[i]==events[j]) return 12+i;
        }
        return 0;
    }
    bool preflight() const {return preflightStatus()==0;}
    bool detachAliases() {
        if (aliasesDetached) return true;
        for (unsigned i=0;i<count;++i) if (list[i]!=rootHandle &&
            EFI_ERROR(services->DisconnectController(list[i],nullptr,nullptr))) return false;
        aliasesDetached=true;return true;
    }
    bool stopClient() {
        if (!identities() || client->mode!=record.previousMode || !queryControllerState()) return false;
        if (record.previousMode==0x10000) return true;
        const auto host=read(0x111e8),device=read(0x111f0);
        if (record.previousMode==1 ? (host && !device):(!host && device))
            return !EFI_ERROR(Calls::stop(root,record.previousMode));
        // Fastboot's public core/mode rebind can leave the active Device on
        // the old Host handle (QcomFastboot::ConnectPeripheral). Retire the
        // actual DriverBinding owners first. Only successful disconnection
        // of every pinned alias permits retiring the stale library metadata.
        if (!detachAliases() || !identities() || client->mode!=record.previousMode ||
            read(0x111e8)!=host || read(0x111f0)!=device || !queryControllerState()) return false;
        store(0x111e8,0);store(0x111f0,0);store(Client+136,UINT64(0x10000)<<32);
        return client->mode==0x10000 && !read(0x111e8) && !read(0x111f0);
    }
    bool queryControllerState() const {
        if (read(0x112a8)!=retiredOverride || !pin.matches(image,0x13000,memory)) return false;
        for (unsigned i=0;i<4;++i) if (read(EventOffsets[i])) return false;
        return true;
    }
    bool quiet() const {
        if (!image || !identities() || client->mode!=0x10000 || read(0x111e8) || read(0x111f0) || read(0x112c0) ||
            read(0x112a8)!=retiredOverride) return false;
        for (unsigned i=0;i<4;++i) if (read(EventOffsets[i])) return false;
        return pin.matches(image,0x13000,memory);
    }
    bool fail() {
        if (output) for (unsigned i=0;i<sizeof(record);++i) output[i]=0;
        stopped=published=false;return false;
    }
    bool detach() {
        auto guid=ConfigProtocolGUID;
        if (!identities() || client->mode!=0x10000 || read(0x111e8) || read(0x111f0)) return false;
        // StopController disconnects only the active client. Old handles can
        // still alias that same static object, so retire their bindings too.
        if (!detachAliases()) return false;
        unsigned n=0;
        if (!BridgeFirmware::handles(services,guid,remaining,64,n)) return false;
        bool foundRoot=false;
        for (unsigned i=0;i<n;++i) {
            bool known=false;for (unsigned j=0;j<count;++j) if (remaining[i]==list[j]) known=true;
            if (!known) return false;
            void *p=nullptr;
            if (EFI_ERROR(services->HandleProtocol(remaining[i],&guid,&p)) ||
                p!=(remaining[i]==rootHandle ? root:client)) return false;
            if (remaining[i]==rootHandle) foundRoot=true;
        }
        if (!foundRoot || !identities() || client->mode!=0x10000 || read(0x111e8) || read(0x111f0)) return false;
        // Keep the management interface available until every old client is
        // detached. An unknown open/protocol writer prevents publication.
        for (unsigned i=0;i<n;++i) if (remaining[i]!=rootHandle) {
            if (EFI_ERROR(services->UninstallProtocolInterface(remaining[i],&guid,client))) return false;
            ++record.removedInterfaces;
        }
        if (read(0x112c0)!=reinterpret_cast<uintptr_t>(rootHandle) ||
            EFI_ERROR(services->UninstallProtocolInterface(rootHandle,&guid,root))) return false;
        ++record.removedInterfaces;store(0x112c0,0);
        UINTN bytes=sizeof(remaining);
        return services->LocateHandle(ByProtocol,&guid,nullptr,&bytes,remaining)==EFI_NOT_FOUND;
    }
public:
    bool mutated() const {return touched;}
    unsigned diagnosticStage() const {return stage;}
    unsigned diagnosticPreflight() const {return image && stage>=6 ? preflightStatus():0;}
    UINT64 diagnosticOverride() const {return image && stage>=6 ? read(0x112a8):0;}
    UINT32 diagnosticMode() const {return image && stage>=6 ? client->mode:0;}
    UINT32 diagnosticHandles() const {return image && stage>=6 ? (read(0x111e8) ? 1u:0u)|(read(0x111f0) ? 2u:0u):0;}
    bool prepare(EFI_BOOT_SERVICES *bs,const USB::FirmwareController &controller,
                 const void *reference,unsigned referenceBytes,void *destination,unsigned bytes) {
        if (started) return false;
        started=true;services=bs;stage=1;
        if (!controller.valid() || !bs || !bs->RaiseTPL || !bs->RestoreTPL || !bs->CloseEvent ||
            !bs->DisconnectController || !bs->UninstallProtocolInterface || !memory.prepare(bs) ||
            bytes!=sizeof(record) || !memory.contains(destination,bytes,1u<<EfiLoaderData) ||
            !pin.prepare(reference,referenceBytes) || pin.size()!=0x13000 ||
            pin.textRVA()!=0x1000 || pin.textBytes()!=0x10000 ||
            pin.interfaceRVA()!=Template || pin.interfaceBytes()!=160) return false;
        output=static_cast<uint8_t *>(destination);fail();
        stage=2;
        if (!BridgeFirmware::handles(bs,ConfigProtocolGUID,list,64,count)) return false;
        auto guid=ConfigProtocolGUID;
        stage=3;
        for (unsigned i=0;i<count;++i) {
            ConfigProtocol *p=nullptr;
            if (EFI_ERROR(bs->HandleProtocol(list[i],&guid,reinterpret_cast<void **>(&p))) || !memory.contains(p,8)) return false;
            // Read only the common header until an actual loaded image pins
            // this exact management address. A same-revision short object
            // must not cause speculative reads of core/mode or callbacks.
            if (BridgeFirmware::pair(p)!=0x10003) return false;
            interfaces[i]=p;
            if (!image) image=const_cast<uint8_t *>(BridgeFirmware::loadedImage(bs,p,pin,memory,Root));
        }
        stage=4;
        if (!image || (reinterpret_cast<uintptr_t>(image)&4095)) return false;
        stage=5;
        for (unsigned i=0;i<count;++i) {
            auto *p=interfaces[i];
            if (p==reinterpret_cast<ConfigProtocol *>(image+Root)) {
                if (root) return false;
                root=p;rootHandle=list[i];
            } else if (p!=reinterpret_cast<ConfigProtocol *>(image+Client)) return false;
            else client=p;
        }
        if (!root) return false;
        // An inactive controller can have no client handles, but its copied
        // configuration object is still owned by this exact loaded image.
        if (!client) client=reinterpret_cast<ConfigProtocol *>(image+Client);
        for (unsigned i=0;i<4;++i) events[i]=read(EventOffsets[i]);
        originalOverride=read(0x112a8);retiredOverride=originalOverride;
        stage=6;
        if (!preflight()) return false;
        stage=61;if (!query(controller)) return false;
        stage=62;if (!preflight()) return false;
        record.previousMode=client->mode;record.controller=controller.base;record.length=controller.length;
        const auto old=bs->RaiseTPL(TPL_NOTIFY);
        stage=7;
        if (old!=TPL_APPLICATION || !preflight() || client->mode!=record.previousMode || !pin.matches(image,0x13000,memory)) {
            bs->RestoreTPL(old);return false;
        }
        stage=8;
        for (unsigned i=0;i<4;++i) if (events[i]) {
            touched=true;
            if (EFI_ERROR(bs->CloseEvent(reinterpret_cast<EFI_EVENT>(events[i])))) {bs->RestoreTPL(old);return fail();}
            store(EventOffsets[i],0);++record.closedEvents;
        }
        // Fastboot can use Device mode while the firmware retains a Host
        // override. StopController unconditionally consumes that override.
        // Only after all producers are closed under TPL_NOTIFY, retire its
        // enable byte so the original API stops the actual current client.
        // Preserve the other bytes and retain this state across EFI exit.
        if (!identities() || client->mode!=record.previousMode || read(0x112a8)!=originalOverride ||
            !pin.matches(image,0x13000,memory)) {bs->RestoreTPL(old);return fail();}
        retiredOverride=originalOverride&~UINT64(255);
        if (retiredOverride!=originalOverride) {touched=true;store(0x112a8,retiredOverride);}
        if (read(0x112a8)!=retiredOverride) {bs->RestoreTPL(old);return fail();}
        bs->RestoreTPL(old);
        stage=9;
        // The exit callback is now closed as well: its later PHY shutdown
        // and internal-core free must not race post-EFI resource adoption.
        if (!identities() || client->mode!=record.previousMode || !query(controller) ||
            !stopClient() ||
            !detach() || !quiet()) return fail();
        stopped=true;stage=10;return true;
    }
    bool finish() {
        // All addresses were bounded before exit. No EFI callback, protocol
        // lookup or internal USB core pointer is used after exit.
        if (!stopped || !output || !quiet()) return fail();
        if (published) return BridgeFirmware::equal(output,&record,sizeof(record)) ? true:fail();
        record.flags=USB::FirmwareStop::Required;
        if (!record.valid()) return fail();
        const auto *p=reinterpret_cast<const uint8_t *>(&record);
        for (unsigned i=0;i<sizeof(record);++i) output[i]=p[i];
        published=true;return true;
    }
};
}
