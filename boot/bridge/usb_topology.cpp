// SPDX-License-Identifier: BSD-2-Clause
// Capture firmware's installed ACPI topology. No USB/PHY/SMMU register is
// touched, and the record does not grant DMA or transfer context ownership.
#include <Uefi.h>
#include <Guid/Acpi.h>
#include <Protocol/AcpiSystemDescriptionTable.h>
extern "C" {
#include "bridge.h"
void *memcpy(void *,const void *,size_t);
void *memset(void *,int,size_t);
}
#include "../../drivers/bus/QcomSMMU/IORT.h"
#include "../../drivers/usb/QcomUSB/USBFirmware.h"
#include "usb_firmware.h"
namespace {
alignas(16) uint8_t descriptors[65536];
UINTN descriptorBytes=0,descriptorStride=0;
const uint8_t *original=nullptr;
uint8_t *destination=nullptr;
uint8_t *controllerDestination=nullptr;
uint32_t originalBytes=0;
SMMU::IORTTopology topology;
USB::FirmwareController controllerSnapshot;
struct Capture { const uint8_t *source;unsigned offset,bytes; } captures[12]{};
uint8_t capturedBytes[16384]{};
unsigned captureCount=0,captureBytes=0;
bool same(const void *a,const void *b,unsigned bytes) {
    const auto *x=static_cast<const uint8_t *>(a),*y=static_cast<const uint8_t *>(b);
    for (unsigned i=0;i<bytes;++i) if (x[i]!=y[i]) return false;
    return true;
}
bool checksum(const uint8_t *p,unsigned bytes) {
    uint8_t sum=0;for (unsigned i=0;i<bytes;++i) sum=uint8_t(sum+p[i]);return !sum;
}
bool remember(const uint8_t *p,unsigned bytes) {
    if (!p || !bytes || captureCount==12 || bytes>sizeof(capturedBytes)-captureBytes) return false;
    memcpy(capturedBytes+captureBytes,p,bytes);
    captures[captureCount++]={p,captureBytes,bytes};captureBytes+=bytes;return true;
}
const uint8_t *acpi(uint64_t address,unsigned bytes) {
    if (!address || !bytes || address>UINT64_MAX-bytes) return nullptr;
    for (UINTN offset=0;offset<descriptorBytes;offset+=descriptorStride) {
        const auto *d=reinterpret_cast<const EFI_MEMORY_DESCRIPTOR *>(descriptors+offset);
        if (d->Type!=EfiACPIReclaimMemory && d->Type!=EfiACPIMemoryNVS) continue;
        if (d->NumberOfPages>(UINT64_MAX>>12)) continue;
        const auto size=d->NumberOfPages<<12;
        if (address>=d->PhysicalStart && address-d->PhysicalStart<=size &&
            bytes<=size-(address-d->PhysicalStart)) return reinterpret_cast<const uint8_t *>(address);
    }
    return nullptr;
}
const uint8_t *table(uint64_t address,uint32_t signature,unsigned maximum=SMMU::IORTMaximumBytes) {
    using namespace SMMU::iort_detail;
    const auto *header=acpi(address,36);
    if (!header || word(header)!=signature) return nullptr;
    const auto bytes=word(header+4);
    if (bytes<36 || bytes>maximum) return nullptr;
    const auto *full=acpi(address,bytes);
    return full && checksum(full,bytes) ? full:nullptr;
}
// Read only the literal resource constructor used by the installed platform.
// Namespace lookup belongs to the standard firmware SDT protocol, not a byte
// search for USB0 inside arbitrary AML buffers. Dynamic resource methods are
// refused rather than executed or treated as a static ownership grant.
struct AML {
    const uint8_t *next,*end;
    bool byte(uint8_t value) { if (next==end || *next!=value) return false;++next;return true; }
    bool name(const char *value) {
        if (end-next<4 || !same(next,value,4)) return false;
        next+=4;return true;
    }
    bool integer(uint32_t &value) {
        if (next==end) return false;
        const auto op=*next++;
        if (op<2) {value=op;return true;}
        const unsigned bytes=op==0x0a ? 1:op==0x0b ? 2:op==0x0c ? 4:0;
        if (!bytes || unsigned(end-next)<bytes) return false;
        value=0;for (unsigned i=0;i<bytes;++i) value|=uint32_t(*next++)<<(8*i);return true;
    }
    bool package(uint8_t opcode,AML &contents) {
        if (!byte(opcode) || next==end) return false;
        const auto *first=next;const auto lead=*next++;const unsigned following=lead>>6;
        if (unsigned(end-next)<following || (following && (lead&0x30))) return false;
        unsigned size=lead&(following ? 15:63);
        for (unsigned i=0;i<following;++i) size|=unsigned(*next++)<<(4+8*i);
        if (size<following+1 || size>unsigned(end-first)) return false;
        contents={next,first+size};next=first+size;return true;
    }
    bool buffer(AML &data) {
        AML body{};uint32_t bytes=0;
        if (!package(0x11,body) || !body.integer(bytes) || bytes!=unsigned(body.end-body.next)) return false;
        data=body;return true;
    }
};
bool resources(AML data,USB::FirmwareController &controller,bool primary) {
    const auto *begin=data.next;bool memory=false,irq=false;unsigned interrupts=0;
    while (data.next<data.end) {
        const auto tag=*data.next++;
        if (tag==0x79) {
            if (data.end-data.next!=1 || (*data.next && !checksum(begin,unsigned(data.end-begin)))) return false;
            return primary ? memory && irq : !memory && interrupts;
        }
        if (data.end-data.next<2) return false;
        const unsigned bytes=unsigned(data.next[0])|unsigned(data.next[1])<<8;data.next+=2;
        if (bytes>unsigned(data.end-data.next)) return false;
        const auto *p=data.next;data.next+=bytes;
        if (tag==0x86 && primary && !memory && !irq && bytes==9 && p[0]==1) {
            controller.base=SMMU::iort_detail::word(p+1);controller.length=SMMU::iort_detail::word(p+5);memory=true;
        } else if (tag==0x89 && bytes==6 && p[1]==1 && (p[0]&1) && !(p[0]&~0x19u)) {
            const auto id=SMMU::iort_detail::word(p+2);
            if (id<32 || id>1019 || (primary && (!memory || irq || (p[0]&16))) ||
                (!primary && (id==controller.interrupt || !(p[0]&16)))) return false;
            if (primary) {controller.interrupt=id;controller.interruptFlags=p[0];irq=true;}
            if (++interrupts>3) return false;
        } else return false;
    }
    return false;
}
bool controllerMethod(const uint8_t *p,unsigned available,USB::FirmwareController &controller,unsigned &bytes) {
    AML source{p,p+available},method{};
    if (!source.package(0x14,method) || !method.name("_CRS") || !method.byte(0) ||
        !method.byte(8) || !method.name("RBUF")) return false;
    AML primary{};
    if (!method.buffer(primary) || !resources(primary,controller,true) ||
        !method.byte(0x70) || !method.name("RBUF") || !method.byte(0x60) ||
        !method.byte(0x84) || !method.byte(0x60)) return false;
    AML wake{};
    if (!method.buffer(wake) || !resources(wake,controller,false) || !method.byte(0x61) ||
        !method.byte(0xa4) || !method.byte(0x61) || method.next!=method.end) return false;
    bytes=unsigned(source.next-p);return bytes<=512 && controller.valid();
}
bool binding(EFI_SYSTEM_TABLE *system,const uint8_t *fadt,const char *component,USB::FirmwareController &out) {
    using namespace SMMU::iort_detail;
    if (!fadt || word(fadt+4)<44) return false;
    const uint64_t extended=word(fadt+4)>=148 ? pair(fadt+140):0;
    const auto *dsdt=table(extended ? extended:word(fadt+40),0x54445344,2*1024*1024);
    if (!dsdt || !remember(fadt,word(fadt+4)) || !remember(dsdt,36)) return false;
    const EFI_GUID guid=EFI_ACPI_SDT_PROTOCOL_GUID;EFI_ACPI_SDT_PROTOCOL *sdt=nullptr;
    if (!system->BootServices->LocateProtocol ||
        EFI_ERROR(system->BootServices->LocateProtocol(const_cast<EFI_GUID *>(&guid),nullptr,reinterpret_cast<void **>(&sdt))) ||
        !sdt || !sdt->GetAcpiTable || !sdt->OpenSdt || !sdt->FindPath || !sdt->GetChild ||
        !sdt->GetOption || !sdt->Close) return false;
    UINTN tableKey=0;unsigned found=0;
    for (unsigned index=0;index<=512;++index) {
        EFI_ACPI_SDT_HEADER *entry=nullptr;EFI_ACPI_TABLE_VERSION version=0;UINTN key=0;
        const auto status=sdt->GetAcpiTable(index,&entry,&version,&key);
        if (status==EFI_NOT_FOUND) break;
        if (EFI_ERROR(status) || index==512) return false;
        if (reinterpret_cast<const uint8_t *>(entry)==dsdt) {if (++found!=1 || !version) return false;tableKey=key;}
    }
    if (found!=1) return false;
    EFI_ACPI_HANDLE root=nullptr;
    if (EFI_ERROR(sdt->OpenSdt(tableKey,&root)) || !root) return false;
    // Resolve the component once. Firmware's FindPath rebuilds and prints
    // the entire AML namespace on each call, even for a missing _ADR. The
    // standard GetChild/GetOption API can enumerate the four direct leaves
    // without repeating that global traversal and its serial dump.
    bool good=true;EFI_ACPI_HANDLE device=nullptr,child=nullptr;
    const char *leaves[]={"_HID","_ADR","_CCA","_CRS"};
    const uint8_t *objects[4]{};
    if (EFI_ERROR(sdt->FindPath(root,const_cast<char *>(component),&device)) || !device) good=false;
    bool ended=false;
    for (unsigned i=0;good && i<256;++i) {
        const auto previous=child;
        const auto status=sdt->GetChild(device,&child);
        if (previous && EFI_ERROR(sdt->Close(previous))) good=false;
        if (EFI_ERROR(status) || (child && child==previous)) {child=nullptr;good=false;break;}
        if (!child) {ended=true;break;}
        const void *data=nullptr;UINTN size=0;EFI_ACPI_DATA_TYPE type=0;
        if (EFI_ERROR(sdt->GetOption(child,0,&type,&data,&size)) ||
            type!=EFI_ACPI_DATA_TYPE_OPCODE || !data || !size || size>2) {good=false;break;}
        const auto address=reinterpret_cast<uintptr_t>(data),start=reinterpret_cast<uintptr_t>(dsdt);
        if (address<start+36 || address-start>=word(dsdt+4) || size>word(dsdt+4)-(address-start)) {good=false;break;}
        const auto *object=static_cast<const uint8_t *>(data);
        if (size!=1 || (*object!=8 && *object!=0x14)) continue;
        if (EFI_ERROR(sdt->GetOption(child,1,&type,&data,&size)) || type!=EFI_ACPI_DATA_TYPE_NAME_STRING ||
            !data || size!=4) continue;
        const auto nameAddress=reinterpret_cast<uintptr_t>(data);
        if (nameAddress<start+36 || nameAddress-start>=word(dsdt+4) ||
            size>word(dsdt+4)-(nameAddress-start)) {good=false;break;}
        for (unsigned n=0;n<4;++n) if (same(data,leaves[n],4)) {
            if (objects[n]) good=false;
            objects[n]=object;
        }
    }
    if (child && EFI_ERROR(sdt->Close(child))) good=false;
    if (device && EFI_ERROR(sdt->Close(device))) good=false;
    good=good && ended;
    for (unsigned n=0;good && n<4;++n) {
        const auto *leaf=leaves[n];const auto *p=objects[n];
        // A platform device identified by _HID need not have a bus address.
        // The installed QCOM2430 device omits _ADR. SDT reports an absent
        // object from the direct child list; protocol failures and an
        // explicitly present nonzero/malformed _ADR must still be rejected.
        if (same(leaf,"_ADR",4) && !p) continue;
        bool valid=p!=nullptr;
        const auto address=reinterpret_cast<uintptr_t>(p),start=reinterpret_cast<uintptr_t>(dsdt);
        if (!valid || address<start+36 || address-start>=word(dsdt+4)) valid=false;
        unsigned bytes=0;
        if (valid) {
            const unsigned available=word(dsdt+4)-unsigned(address-start);
            if (same(leaf,"_CRS",4)) valid=controllerMethod(p,available,out,bytes);
            else if (available<6 || p[0]!=8 || !same(p+1,leaf,4)) valid=false;
            else if (same(leaf,"_HID",4)) {bytes=15;valid=available>=bytes && p[5]==0x0d && same(p+6,"QCOM2430",9);}
            else {bytes=6;valid=p[5]<2;if (valid && same(leaf,"_CCA",4)) out.coherent=p[5];else if (valid) valid=p[5]==0;}
            if (valid) valid=remember(p,bytes);
        }
        if (!valid) {good=false;break;}
    }
    if (EFI_ERROR(sdt->Close(root))) good=false;
    return good && out.valid() && out.coherent==topology.coherent;
}
}
extern "C" int bridge_usb_topology_prepare(EFI_SYSTEM_TABLE *system,void *tree) {
    if (!system || !system->BootServices || original || destination || system->NumberOfTableEntries>1024 ||
        (!system->ConfigurationTable && system->NumberOfTableEntries)) return -1;
    uint32_t bytes=0,nameBytes=0;
    auto *record=static_cast<uint8_t *>(dt_property(tree,"chosen","qcom,usb-firmware-iort",&bytes));
    const auto *name=static_cast<const char *>(dt_property(tree,"chosen","usb-iort-component",&nameBytes));
    uint32_t controllerBytes=0;
    auto *controller=static_cast<uint8_t *>(dt_property(tree,"chosen",USB::FirmwareControllerProperty,&controllerBytes));
    if (!record || bytes!=SMMU::IORTMaximumBytes || !name || nameBytes<2 || nameBytes>65 || name[nameBytes-1]) return -2;
    if (!controller || controllerBytes!=sizeof(USB::FirmwareController)) return -2;
    for (unsigned i=0;i+1<nameBytes;++i) if (!name[i]) return -2;
    // A static device tree or a previous failed capture cannot supply a
    // seemingly live record. Only the installed, validated table is copied.
    for (unsigned i=0;i<bytes;++i) if (record[i]) return -3;
    for (unsigned i=0;i<controllerBytes;++i) if (controller[i]) return -3;
    captureCount=0;captureBytes=0;
    static_assert(offsetof(EFI_SYSTEM_TABLE,ConfigurationTable)==offsetof(EFI_SYSTEM_TABLE,NumberOfTableEntries)+sizeof(UINTN),
        "UEFI configuration-table list ABI");
    if (!remember(reinterpret_cast<const uint8_t *>(&system->NumberOfTableEntries),
        sizeof(system->NumberOfTableEntries)+sizeof(system->ConfigurationTable))) return -3;
    descriptorBytes=sizeof(descriptors);UINTN key=0;UINT32 version=0;
    const auto status=system->BootServices->GetMemoryMap(&descriptorBytes,
        reinterpret_cast<EFI_MEMORY_DESCRIPTOR *>(descriptors),&key,&descriptorStride,&version);
    if (EFI_ERROR(status) || descriptorBytes>sizeof(descriptors) || descriptorStride<sizeof(EFI_MEMORY_DESCRIPTOR) ||
        descriptorStride>4096 || descriptorStride%alignof(EFI_MEMORY_DESCRIPTOR) ||
        descriptorBytes%descriptorStride) return -4;
    const EFI_GUID guid=EFI_ACPI_20_TABLE_GUID;
    const uint8_t *rsdp=nullptr;
    for (UINTN i=0;i<system->NumberOfTableEntries;++i) {
        const auto &entry=system->ConfigurationTable[i];
        if (!same(&entry.VendorGuid,&guid,sizeof(guid))) continue;
        if (rsdp) return -5;
        rsdp=acpi(reinterpret_cast<uintptr_t>(entry.VendorTable),36);
        if (!rsdp || !remember(reinterpret_cast<const uint8_t *>(&entry),sizeof(entry))) return -6;
    }
    using namespace SMMU::iort_detail;
    if (!rsdp || !same(rsdp,"RSD PTR ",8) || rsdp[15]<2 || !checksum(rsdp,20)) return -7;
    const auto rsdpBytes=word(rsdp+20);
    if (rsdpBytes<36 || rsdpBytes>4096 ||
        !acpi(reinterpret_cast<uintptr_t>(rsdp),rsdpBytes) || !checksum(rsdp,rsdpBytes)) return -8;
    const auto *xsdt=table(pair(rsdp+24),0x54445358);
    if (!xsdt) return -9;
    const auto entries=word(xsdt+4)-36;
    if (entries%8 || entries/8>512) return -10;
    const uint8_t *iort=nullptr,*fadt=nullptr;
    for (unsigned offset=0;offset<entries;offset+=8) {
        const auto address=pair(xsdt+36+offset);
        const auto *header=acpi(address,36);
        if (!header) return -11;
        if (word(header)==0x50434146) {
            if (fadt || !(fadt=table(address,0x50434146,4096))) return -13;
        }
        if (word(header)!=0x54524f49) continue;
        if (iort) return -12;
        iort=table(address,0x54524f49);
        if (!iort) return -13;
    }
    if (!iort || SMMU::parseIORT(iort,word(iort+4),name,0,topology)!=SMMU::IORTResult::Success) return -14;
    USB::FirmwareController resource;
    if (!remember(rsdp,rsdpBytes) || !remember(xsdt,word(xsdt+4)) || !binding(system,fadt,name,resource)) return -15;
#ifndef BRIDGE_USB_FIRMWARE_STOP
    if (!BridgeUSB::firmwareController(system->BootServices,resource)) return -16;
#endif
    // Firmware retirement binds this same aperture through its whole-image
    // pinned owner before EFI exit. Do not query an unpinned Config object
    // first when the stronger retirement path has been selected.
    original=iort;originalBytes=word(iort+4);destination=record;controllerDestination=controller;
    memcpy(record,iort,originalBytes);
    controllerSnapshot=resource;memcpy(controller,&resource,sizeof(resource));
    bridge_puts("[USB topology] installed IORT: SMMU=");bridge_hex(topology.base);
    bridge_puts(" stream=");bridge_hex(topology.stream);bridge_puts(" (no DMA grant)\n");
    return 0;
}
extern "C" int bridge_usb_topology_finish(void) {
    bool unchanged=original && destination && controllerDestination && originalBytes &&
        same(original,destination,originalBytes) && same(controllerDestination,&controllerSnapshot,sizeof(controllerSnapshot));
    for (unsigned i=0;i<captureCount;++i)
        if (!same(captures[i].source,capturedBytes+captures[i].offset,captures[i].bytes)) unchanged=false;
    if (!unchanged) {
        if (destination) memset(destination,0,SMMU::IORTMaximumBytes);
        if (controllerDestination) memset(controllerDestination,0,sizeof(USB::FirmwareController));
        return -1;
    }
    return 0;
}
