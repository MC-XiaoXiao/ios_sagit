// SPDX-License-Identifier: BSD-2-Clause
// Production retirement sequence with EFI services and firmware-call mocks.
// Pinned ARM64 code is relocated and compared, never executed by this test.
#include <cassert>
#include <cstdio>
#include <cstring>
#include <new>
#include <vector>
#include "../boot/bridge/usb_firmware_stop.h"
namespace {
alignas(4096) uint8_t image[0x13000]{},output[4096]{};
EFI_LOADED_IMAGE_PROTOCOL loaded{};
EFI_BOOT_SERVICES services{};
std::vector<uint8_t> pin;
bool exited=false,installed[64]{};
unsigned handles=3,serviceCalls=0,firmwareCalls=0,closeCalls=0,stopCalls=0,disconnects=0,uninstalls=0;
unsigned fault=0,failClose=4;
EFI_TPL tpl=TPL_APPLICATION;
UINT64 shortRevision=0x10003;
const unsigned slots[]={0x114f0,0x114c0,0x114b8,0x114b0};
void word(void *at,uint32_t value) {auto *p=static_cast<uint8_t *>(at);for(unsigned n=0;n<4;++n)p[n]=uint8_t(value>>(8*n));}
void pair(void *at,uint64_t value) {word(at,uint32_t(value));word(static_cast<uint8_t *>(at)+4,uint32_t(value>>32));}
uint64_t address(const void *p) {return reinterpret_cast<uintptr_t>(p);}
uint64_t value(unsigned at) {return BridgeFirmware::pair(image+at);}
BridgeUSB::ConfigProtocol *root() {return reinterpret_cast<BridgeUSB::ConfigProtocol *>(image+0x112d8);}
BridgeUSB::ConfigProtocol *client() {return reinterpret_cast<BridgeUSB::ConfigProtocol *>(image+0x11208);}
void closed() {for(auto at:slots)assert(!value(at));}
struct Calls {
    static EFI_STATUS count(BridgeUSB::ConfigProtocol *p,UINT8 *out) {
        assert(!exited && p==root() && address(reinterpret_cast<void *>(p->getCoreCount))==address(image)+0x1e24);
        ++firmwareCalls;*out=fault==15 ? 2:1;return fault==16 ? EFI_DEVICE_ERROR:EFI_SUCCESS;
    }
    static EFI_STATUS base(BridgeUSB::ConfigProtocol *p,UINT64 *out) {
        assert(!exited && p==root() && address(reinterpret_cast<void *>(p->getBase))==address(image)+0x1440);
        ++firmwareCalls;*out=fault==17 ? 0x0b800000:0x0a800000;
        if(fault==18)root()->mode=4;
        return fault==19 ? EFI_DEVICE_ERROR:EFI_SUCCESS;
    }
    static EFI_STATUS stop(BridgeUSB::ConfigProtocol *p,UINT32 mode) {
        assert(!exited && tpl==TPL_APPLICATION && p==root() && client()->mode==mode && (mode==1 || mode==4));
        assert(address(reinterpret_cast<void *>(p->stopController))==address(image)+0x1ad8);closed();
        assert(!(value(0x112a8)&255)); // Override retired only after all events.
        ++firmwareCalls;++stopCalls;
        if(fault==20)return EFI_DEVICE_ERROR;
        if(fault!=21) {client()->mode=0x10000;pair(image+0x111e8,0);pair(image+0x111f0,0);}
        if(fault==22)word(image+0x11294,2);
        return EFI_SUCCESS;
    }
};
using Stop=BridgeUSB::FirmwareStop<Calls>;
alignas(Stop) uint8_t storage[sizeof(Stop)]{};
Stop *stopper=nullptr;
EFI_STATUS EFIAPI memoryMap(UINTN *bytes,EFI_MEMORY_DESCRIPTOR *out,UINTN *key,UINTN *stride,UINT32 *version) {
    assert(!exited);++serviceCalls;
    EFI_MEMORY_DESCRIPTOR map[4]{};const void *p[]={image,output,&loaded,&shortRevision};
    const unsigned sizes[]={sizeof(image),sizeof(output),sizeof(loaded),sizeof(shortRevision)};
    for(unsigned i=0;i<4;++i) {
        const auto first=address(p[i])&~4095ULL,last=(address(p[i])+sizes[i]+4095)&~4095ULL;
        map[i].PhysicalStart=first;map[i].NumberOfPages=(last-first)/4096;
        map[i].Type=i==1 ? EfiLoaderData:EfiBootServicesCode;
    }
    if(fault==1)map[0].Type=EfiMemoryMappedIO;
    if(fault==2)map[1].Type=EfiBootServicesData;
    assert(*bytes>=sizeof(map));std::memcpy(out,map,sizeof(map));*bytes=sizeof(map);*stride=sizeof(map[0]);*key=1;*version=1;
    return fault==3 ? EFI_DEVICE_ERROR:EFI_SUCCESS;
}
EFI_STATUS EFIAPI locate(EFI_LOCATE_SEARCH_TYPE type,EFI_GUID *guid,void *key,UINTN *bytes,EFI_HANDLE *list) {
    assert(!exited && type==ByProtocol && !key);++serviceCalls;
    const EFI_GUID images=EFI_LOADED_IMAGE_PROTOCOL_GUID;
    if(!std::memcmp(guid,&images,sizeof(*guid))) {
        assert(*bytes==256*sizeof(EFI_HANDLE));*bytes=sizeof(EFI_HANDLE);list[0]=reinterpret_cast<void *>(0x300);return EFI_SUCCESS;
    }
    assert(!std::memcmp(guid,&BridgeUSB::ConfigProtocolGUID,sizeof(*guid)) && *bytes==64*sizeof(EFI_HANDLE));
    if(fault==4)return EFI_NOT_FOUND;
    if(fault==5)return EFI_BUFFER_TOO_SMALL;
    unsigned count=0;
    for(unsigned i=0;i<handles;++i)if(installed[i])list[count++]=reinterpret_cast<void *>(uintptr_t(0x100+i));
    if(!count) {
        if(fault==26) {*bytes=sizeof(EFI_HANDLE);list[0]=reinterpret_cast<void *>(0x100);return EFI_SUCCESS;}
        return EFI_NOT_FOUND;
    }
    *bytes=count*sizeof(EFI_HANDLE);
    if(fault==6)list[0]=nullptr;
    if(fault==7)list[1]=list[0];
    if(fault==8)*bytes=3;
    if(fault==27 && stopCalls)list[count-1]=reinterpret_cast<void *>(0x140);
    return EFI_SUCCESS;
}
EFI_STATUS EFIAPI protocol(EFI_HANDLE handle,EFI_GUID *guid,void **out) {
    assert(!exited);++serviceCalls;
    const EFI_GUID images=EFI_LOADED_IMAGE_PROTOCOL_GUID;
    if(!std::memcmp(guid,&images,sizeof(*guid))) {*out=&loaded;return EFI_SUCCESS;}
    if(fault==9)return EFI_DEVICE_ERROR;
    if(fault==10) {*out=nullptr;return EFI_SUCCESS;}
    if(fault==31) {*out=&shortRevision;return EFI_SUCCESS;}
    const auto index=reinterpret_cast<uintptr_t>(handle)-0x100;assert(index<64 && installed[index]);
    *out=index==0 ? root():client();return EFI_SUCCESS;
}
EFI_TPL EFIAPI raise(EFI_TPL next) {
    assert(!exited && next==TPL_NOTIFY);++serviceCalls;const auto old=tpl;tpl=next;
    if(fault==28)client()->mode=1;
    return fault==29 ? TPL_CALLBACK:old;
}
void EFIAPI restore(EFI_TPL old) {assert(!exited && tpl==TPL_NOTIFY);++serviceCalls;tpl=old;}
EFI_STATUS EFIAPI closeEvent(EFI_EVENT event) {
    assert(!exited && tpl==TPL_NOTIFY);++serviceCalls;
    const auto index=reinterpret_cast<uintptr_t>(event)-0x200;assert(index<4 && value(slots[index])==reinterpret_cast<uintptr_t>(event));
    for(unsigned i=0;i<index;++i)assert(!value(slots[i]));
    ++closeCalls;return index==failClose ? EFI_DEVICE_ERROR:EFI_SUCCESS;
}
EFI_STATUS EFIAPI disconnect(EFI_HANDLE handle,EFI_HANDLE driver,EFI_HANDLE child) {
    assert(!exited && tpl==TPL_APPLICATION && !driver && !child &&
        (client()->mode==0x10000 || (!stopCalls && client()->mode==4)));closed();++serviceCalls;++disconnects;
    const auto index=reinterpret_cast<uintptr_t>(handle)-0x100;assert(index && index<handles && installed[index]);
    if(fault==30 && index==handles-1)installed[index]=false;
    return fault==23 ? EFI_DEVICE_ERROR:EFI_SUCCESS;
}
EFI_STATUS EFIAPI uninstall(EFI_HANDLE handle,EFI_GUID *guid,void *p) {
    assert(!exited && tpl==TPL_APPLICATION && disconnects==handles-1);closed();++serviceCalls;
    assert(!std::memcmp(guid,&BridgeUSB::ConfigProtocolGUID,sizeof(*guid)));
    const auto index=reinterpret_cast<uintptr_t>(handle)-0x100;assert(index<handles && installed[index]);
    assert(p==(index ? client():root()));
    if(!index)for(unsigned i=1;i<handles;++i)assert(!installed[i]);
    if((fault==24 && index) || (fault==25 && !index))return EFI_ACCESS_DENIED;
    installed[index]=false;++uninstalls;return EFI_SUCCESS;
}
void installTemplate() {
    const auto count=BridgeFirmware::word(pin.data()+28),offset=40+4*count;
    assert(offset+0x10000+160==pin.size());
    std::memcpy(image+0x1000,pin.data()+offset,0x10000);
    std::memcpy(image+0x11140,pin.data()+offset+0x10000,160);
    for(unsigned i=0;i<count;++i) {
        const auto rva=BridgeFirmware::word(pin.data()+40+4*i);
        assert((rva>=0x1000 && rva+8<=0x11000) || (rva>=0x11140 && rva+8<=0x111e0));
        pair(image+rva,address(image)+BridgeFirmware::pair(image+rva));
    }
    std::memcpy(root(),image+0x11140,160);root()->core=1;root()->mode=0x10000;
    std::memcpy(client(),image+0x11140,160);client()->core=0;client()->mode=4;
}
void fixture() {
    std::memset(image,0,sizeof(image));std::memset(output,0,sizeof(output));
    exited=false;fault=0;failClose=4;handles=3;tpl=TPL_APPLICATION;
    serviceCalls=firmwareCalls=closeCalls=stopCalls=disconnects=uninstalls=0;
    for(auto &i:installed)i=false;
    for(unsigned i=0;i<handles;++i)installed[i]=true;
    pin.assign(40+12+0x10000+160,0);
    const unsigned header[]={0x31495046,1,sizeof(image),0x1000,0x10000,0x11140,160,3,0,0};
    for(unsigned i=0;i<10;++i)word(pin.data()+4*i,header[i]);
    word(pin.data()+40,0x11148);word(pin.data()+44,0x11198);word(pin.data()+48,0x111b8);
    auto *p=pin.data()+52+0x10000;
    pair(p,0x10003);pair(p+8,0x1440);pair(p+88,0x1ad8);pair(p+120,0x1e24);word(p+140,4);
    installTemplate();
    pair(image+0x112c0,0x100);pair(image+0x111f0,0x101);
    for(unsigned i=0;i<4;++i)pair(image+slots[i],0x200+i);
    loaded={};loaded.Revision=EFI_LOADED_IMAGE_PROTOCOL_REVISION;loaded.ImageBase=image;loaded.ImageSize=sizeof(image);
    loaded.ImageCodeType=EfiBootServicesCode;loaded.ImageDataType=EfiBootServicesData;
    services={};services.GetMemoryMap=memoryMap;services.LocateHandle=locate;services.HandleProtocol=protocol;
    services.RaiseTPL=raise;services.RestoreTPL=restore;services.CloseEvent=closeEvent;
    services.DisconnectController=disconnect;services.UninstallProtocolInterface=uninstall;
    if(stopper)stopper->~Stop();
    stopper=new(storage) Stop;
}
bool empty() {for(unsigned i=0;i<64;++i)if(output[i])return false;return true;}
bool prepare() {
    USB::FirmwareController controller;controller.base=0x0a800000;controller.length=0xfffff;controller.interrupt=163;controller.interruptFlags=9;
    return stopper->prepare(&services,controller,pin.data(),unsigned(pin.size()),output,64);
}
USB::FirmwareStop result() {USB::FirmwareStop r;std::memcpy(&r,output,64);return r;}
}
void USBFirmwareStopTests() {
    fixture();assert(prepare() && stopper->mutated() && empty() && tpl==TPL_APPLICATION);
    assert(closeCalls==4 && stopCalls==1 && disconnects==2 && uninstalls==3 && firmwareCalls==5);
    const auto before=serviceCalls;exited=true;loaded={};services={};
    assert(stopper->finish() && serviceCalls==before);auto r=result();
    assert(r.version==1 && r.bytes==64 && r.flags==7 && r.previousMode==4 && r.closedEvents==4 && r.removedInterfaces==3 && !r.grants);
    assert(stopper->finish());output[0]^=1;assert(!stopper->finish() && empty());
    for(unsigned f=1;f<=29;++f) {
        fixture();fault=f;
        if(f==11)image[0x1100]^=1;
        if(f==12)root()->core=2;
        if(f==13)root()->mode=4;
        if(f==14)loaded.ImageSize--;
        assert(!prepare() && empty());
        assert(stopper->mutated()==(f>=20 && f<=27));
        assert(tpl==TPL_APPLICATION || f==29);
    }
    for(unsigned f=0;f<4;++f) {fixture();failClose=f;assert(!prepare() && stopper->mutated() && !stopCalls && !disconnects && !uninstalls && empty() && tpl==TPL_APPLICATION);}
    for(unsigned f=0;f<10;++f) {
        fixture();assert(prepare());exited=true;const auto calls=serviceCalls;
        switch(f) {
        case 0:pair(image+slots[0],0x400);break;
        case 1:pair(image+slots[1],0x400);break;
        case 2:pair(image+slots[2],0x400);break;
        case 3:pair(image+slots[3],0x400);break;
        case 4:pair(image+0x111e8,0x101);break;
        case 5:pair(image+0x112c0,0x100);break;
        case 6:client()->mode=4;break;
        case 7:image[0x1100]^=1;break;
        case 8:image[0x11148]^=1;break;
        case 9:pair(image+0x112a8,1);break;
        }
        assert(!stopper->finish() && empty() && serviceCalls==calls);
    }
    fixture();client()->mode=1;pair(image+0x111e8,0x101);pair(image+0x111f0,0);assert(prepare());exited=true;assert(stopper->finish() && result().previousMode==1);
    fixture();client()->mode=0x10000;pair(image+0x111f0,0);handles=1;assert(prepare() && !stopCalls && !disconnects);exited=true;assert(stopper->finish());
    fixture();pair(image+slots[0],0);assert(prepare());exited=true;assert(stopper->finish() && result().closedEvents==3);
    fixture();fault=30;assert(prepare());exited=true;assert(stopper->finish() && result().removedInterfaces==2);
    // Real Fastboot state: a retained Host override with an active Device.
    for (unsigned forced:{1u,4u}) {
        fixture();pair(image+0x112a8,uint64_t(forced)<<32|1);
        assert(prepare() && stopCalls==1 && value(0x112a8)==uint64_t(forced)<<32);
        exited=true;assert(stopper->finish() && result().previousMode==4);
    }
    fixture();pair(image+0x112a8,uint64_t(1)<<32|1);
    pair(image+0x111e8,0x101);pair(image+0x111f0,0);
    assert(prepare() && !stopCalls && disconnects==2 && client()->mode==0x10000);
    exited=true;assert(stopper->finish() && result().previousMode==4);
    fixture();pair(image+0x111e8,0x101);pair(image+0x111f0,0);fault=23;
    assert(!prepare() && stopper->mutated() && client()->mode==4 && value(0x111e8)==0x101 && !uninstalls);
    fixture();handles=64;for(auto &i:installed)i=true;assert(prepare() && disconnects==63);exited=true;assert(stopper->finish() && result().removedInterfaces==64);
    for(unsigned f=0;f<8;++f) {
        fixture();
        switch(f) {
        case 0:pair(image+slots[1],value(slots[0]));break;
        case 1:pair(image+slots[3],0);break;
        case 2:pair(image+0x111e8,0x180);break;
        case 3:pair(image+0x111f0,0x180);break;
        case 4:pair(image+0x112a8,uint64_t(2)<<32|1);break;
        case 5:pair(image+0x112c0,0x180);break;
        case 6:client()->mode=2;break;
        case 7:word(pin.data()+8,0x14000);break;
        }
        assert(!prepare() && !stopper->mutated() && empty());
    }
    fixture();assert(!stopper->finish() && !stopper->mutated());
    fixture();services.CloseEvent=nullptr;assert(!prepare() && !stopper->mutated());
    fixture();services.DisconnectController=nullptr;assert(!prepare() && !stopper->mutated());
    fixture();services.UninstallProtocolInterface=nullptr;assert(!prepare() && !stopper->mutated());
    fixture();fault=31;assert(!prepare() && !stopper->mutated() && !firmwareCalls && !closeCalls);
    std::puts("USB firmware stop: pinned owner, all four callback events, active and aliased client retirement, one-way failure retention and post-exit revocation passed");
}
void USBFirmwareStopPinTest(const char *path) {
    fixture();FILE *file=std::fopen(path,"rb");assert(file);std::fseek(file,0,SEEK_END);const auto size=std::ftell(file);std::rewind(file);
    assert(size>40 && size<=0x100000);pin.resize(unsigned(size));assert(std::fread(pin.data(),1,pin.size(),file)==pin.size());std::fclose(file);
    assert(BridgeFirmware::word(pin.data()+28)==78);installTemplate();
    assert(prepare() && empty());exited=true;assert(stopper->finish() && !result().grants);
    std::puts("USB firmware stop: actual UsbConfigDxe code and template accepted with all78 load relocations; firmware calls remain modeled");
}
