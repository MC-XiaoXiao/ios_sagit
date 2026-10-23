// SPDX-License-Identifier: BSD-2-Clause
// Exercise the production EFI capture routine against an in-memory firmware
// map and real ACPI bytes. No MMIO, USB controller or EFI boot is executed.
#include <cassert>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <vector>
#include <Uefi.h>
#include <Guid/Acpi.h>
#include <Protocol/AcpiSystemDescriptionTable.h>
#include <IORT.h>
#include "../drivers/usb/QcomUSB/USBFirmware.h"
#include "../boot/bridge/usb_firmware.h"
std::vector<uint8_t> USBIORTFixture();
extern "C" int bridge_usb_topology_prepare(EFI_SYSTEM_TABLE *,void *);
extern "C" int bridge_usb_topology_finish(void);
namespace {
alignas(4096) uint8_t firmware[0x8000]{};
uint8_t record[65536]{};
uint8_t controllerRecord[40]{};
uint32_t controllerBytes=40;
const uint8_t *leaves[4]{};
unsigned openHandles=0;
int protocolFault=0;
int usbFault=0;
unsigned configHandles=2,configQueries=0;
bool exitedEFI=false;
BridgeUSB::ConfigProtocol configs[3]{};
UINT64 unknownConfigRevision=0x10002;
EFI_STATUS EFIAPI coreCount(BridgeUSB::ConfigProtocol *,UINT8 *count) {
    assert(!exitedEFI);++configQueries;
    if (usbFault==12) return EFI_DEVICE_ERROR;
    *count=usbFault==13 ? 2:1;return EFI_SUCCESS;
}
EFI_STATUS EFIAPI coreBase(BridgeUSB::ConfigProtocol *config,UINT32 type,UINT64 *base) {
    assert(!exitedEFI && type==5);++configQueries;
    if (usbFault==14) return EFI_DEVICE_ERROR;
    *base=usbFault==15 ? 0x0b800000:0x0a800000;
    if (usbFault==18) ++config->mode;
    return EFI_SUCCESS;
}
EFI_STATUS EFIAPI locateUSB(EFI_LOCATE_SEARCH_TYPE type,EFI_GUID *guid,void *key,UINTN *bytes,EFI_HANDLE *handles) {
    assert(!exitedEFI && type==ByProtocol && !key && *bytes==64*sizeof(EFI_HANDLE));
    assert(!std::memcmp(guid,&BridgeUSB::ConfigProtocolGUID,sizeof(*guid)));
    if (usbFault==1) return EFI_NOT_FOUND;
    if (usbFault==2) return EFI_BUFFER_TOO_SMALL;
    *bytes=usbFault==3 ? 0:usbFault==4 ? 3:configHandles*sizeof(EFI_HANDLE);
    for (unsigned i=0;i<configHandles && i<64;++i) handles[i]=reinterpret_cast<void *>(uintptr_t(0x100+i));
    if (usbFault==5) handles[0]=nullptr;
    if (usbFault==6) handles[1]=handles[0];
    return EFI_SUCCESS;
}
EFI_STATUS EFIAPI usbProtocol(EFI_HANDLE handle,EFI_GUID *guid,void **out) {
    assert(!exitedEFI && !std::memcmp(guid,&BridgeUSB::ConfigProtocolGUID,sizeof(*guid)));
    const auto index=reinterpret_cast<uintptr_t>(handle)-0x100;assert(index<64);
    if (usbFault==7) return EFI_DEVICE_ERROR;
    if (usbFault==21) { *out=&unknownConfigRevision;return EFI_SUCCESS; }
    *out=usbFault==8 ? nullptr:&configs[index<2 ? index:usbFault==19 || usbFault==20 ? 2:1];
    return EFI_SUCCESS;
}
char component[16]="\\_SB_.USB0";
uint32_t componentBytes=11;
EFI_MEMORY_DESCRIPTOR descriptor{};
UINTN stride=sizeof(descriptor);
EFI_STATUS mapStatus=EFI_SUCCESS;
void word(uint8_t *p,uint32_t value) { for (unsigned i=0;i<4;++i) p[i]=uint8_t(value>>(8*i)); }
void pair(uint8_t *p,uint64_t value) { word(p,uint32_t(value));word(p+4,uint32_t(value>>32)); }
void checksum(uint8_t *p,unsigned bytes,unsigned field=9) {
    p[field]=0;unsigned sum=0;for (unsigned i=0;i<bytes;++i) sum+=p[i];p[field]=uint8_t(-sum);
}
EFI_STATUS EFIAPI memoryMap(UINTN *bytes,EFI_MEMORY_DESCRIPTOR *out,UINTN *key,UINTN *size,UINT32 *version) {
    assert(*bytes>=sizeof(descriptor));
    std::memcpy(out,&descriptor,sizeof(descriptor));
    *bytes=sizeof(descriptor);*key=1;*size=stride;*version=1;return mapStatus;
}
EFI_STATUS EFIAPI getTable(UINTN index,EFI_ACPI_SDT_HEADER **out,EFI_ACPI_TABLE_VERSION *version,UINTN *key) {
    if (index>unsigned(protocolFault==2 ? 1:0)) return EFI_NOT_FOUND;
    *out=reinterpret_cast<EFI_ACPI_SDT_HEADER *>(firmware+(protocolFault==3 ? 0x3000:0x4000));
    *version=EFI_ACPI_TABLE_VERSION_2_0;*key=index+1;return EFI_SUCCESS;
}
EFI_STATUS EFIAPI openTable(UINTN key,EFI_ACPI_HANDLE *out) {
    assert(key==1);*out=reinterpret_cast<void *>(1);++openHandles;return EFI_SUCCESS;
}
EFI_STATUS EFIAPI findPath(EFI_ACPI_HANDLE root,void *path,EFI_ACPI_HANDLE *out) {
    assert(root==reinterpret_cast<void *>(1));
    const char *names[]={"\\_SB_.USB0._HID","\\_SB_.USB0._ADR","\\_SB_.USB0._CCA","\\_SB_.USB0._CRS"};
    if (protocolFault==4) return EFI_NOT_FOUND;
    for (unsigned i=0;i<4;++i) if (!std::strcmp(static_cast<const char *>(path),names[i])) {
        if (i==1 && protocolFault==7) { *out=nullptr;return EFI_SUCCESS; }
        if (i==1 && protocolFault==8) return EFI_DEVICE_ERROR;
        *out=const_cast<uint8_t *>(leaves[i]);++openHandles;return EFI_SUCCESS;
    }
    return EFI_NOT_FOUND;
}
EFI_STATUS EFIAPI getOption(EFI_ACPI_HANDLE object,UINTN index,EFI_ACPI_DATA_TYPE *type,const void **data,UINTN *bytes) {
    assert(index==0);*type=protocolFault==5 ? EFI_ACPI_DATA_TYPE_UINT:EFI_ACPI_DATA_TYPE_OPCODE;
    *data=protocolFault==6 ? record:object;*bytes=1;return EFI_SUCCESS;
}
EFI_STATUS EFIAPI closeHandle(EFI_ACPI_HANDLE) {assert(openHandles);--openHandles;return EFI_SUCCESS;}
EFI_ACPI_SDT_PROTOCOL sdt{};
EFI_STATUS EFIAPI locate(EFI_GUID *guid,void *,void **out) {
    const EFI_GUID expected=EFI_ACPI_SDT_PROTOCOL_GUID;
    assert(!std::memcmp(guid,&expected,sizeof(expected)));
    if (protocolFault==1) return EFI_NOT_FOUND;
    *out=&sdt;return EFI_SUCCESS;
}
std::vector<uint8_t> package(uint8_t opcode,std::vector<uint8_t> body) {
    const unsigned bytes=body.size()+1<64 ? 1:2,size=unsigned(body.size())+bytes;
    std::vector<uint8_t> result{opcode,uint8_t(bytes==1 ? size:0x40|(size&15))};
    if (bytes==2) result.push_back(uint8_t(size>>4));
    result.insert(result.end(),body.begin(),body.end());return result;
}
std::vector<uint8_t> buffer(const std::vector<uint8_t> &resource) {
    std::vector<uint8_t> body{0x0a,uint8_t(resource.size())};body.insert(body.end(),resource.begin(),resource.end());
    return package(0x11,body);
}
std::vector<uint8_t> crs() {
    std::vector<uint8_t> primary{0x86,9,0,1,0,0,0x80,0x0a,0xff,0xff,0x0f,0,
        0x89,6,0,9,1,0xa3,0,0,0,0x79,0};
    const auto first=buffer(primary),wake=buffer({0x89,6,0,0x19,1,0x7b,1,0,0,0x89,6,0,0x19,1,0x13,1,0,0,0x79,0});
    std::vector<uint8_t> body{'_','C','R','S',0,8,'R','B','U','F'};
    body.insert(body.end(),first.begin(),first.end());
    for (auto byte:{0x70,'R'+0,'B'+0,'U'+0,'F'+0,0x60,0x84,0x60}) body.push_back(uint8_t(byte));
    body.insert(body.end(),wake.begin(),wake.end());body.push_back(0x61);body.push_back(0xa4);body.push_back(0x61);
    return package(0x14,body);
}
void setup() {
    assert(!openHandles);protocolFault=0;controllerBytes=40;
    usbFault=0;configHandles=2;configQueries=0;exitedEFI=false;
    for (auto &config:configs) {config={};config.revision=0x10003;config.getBase=coreBase;config.getCoreCount=coreCount;config.mode=4;}
    configs[0].core=1;configs[0].mode=0x10000;
    sdt={};sdt.GetAcpiTable=getTable;sdt.OpenSdt=openTable;sdt.FindPath=findPath;sdt.GetOption=getOption;sdt.Close=closeHandle;
    std::memset(firmware,0,sizeof(firmware));std::memset(record,0,sizeof(record));std::memset(controllerRecord,0,sizeof(controllerRecord));
    descriptor={};descriptor.Type=EfiACPIReclaimMemory;
    descriptor.PhysicalStart=reinterpret_cast<uintptr_t>(firmware);descriptor.NumberOfPages=8;
    stride=sizeof(descriptor);mapStatus=EFI_SUCCESS;componentBytes=11;
    std::memcpy(component,"\\_SB_.USB0",11);
    auto iort=USBIORTFixture();std::memcpy(firmware+0x2000,iort.data(),iort.size());
    std::memcpy(firmware,"RSD PTR ",8);firmware[15]=2;
    word(firmware+20,36);pair(firmware+24,reinterpret_cast<uintptr_t>(firmware+0x1000));
    checksum(firmware,20,8);checksum(firmware,36,32);
    word(firmware+0x1000,0x54445358);word(firmware+0x1004,52);
    pair(firmware+0x1024,reinterpret_cast<uintptr_t>(firmware+0x2000));
    pair(firmware+0x102c,reinterpret_cast<uintptr_t>(firmware+0x3000));checksum(firmware+0x1000,52);
    word(firmware+0x3000,0x50434146);word(firmware+0x3004,148);
    pair(firmware+0x3000+140,reinterpret_cast<uintptr_t>(firmware+0x4000));checksum(firmware+0x3000,148);
    auto *dsdt=firmware+0x4000;word(dsdt,0x54445344);
    const uint8_t hid[]={8,'_','H','I','D',0x0d,'Q','C','O','M','2','4','3','0',0};
    const uint8_t adr[]={8,'_','A','D','R',0},cca[]={8,'_','C','C','A',0};
    leaves[0]=dsdt+36;std::memcpy(dsdt+36,hid,sizeof(hid));
    leaves[1]=dsdt+51;std::memcpy(dsdt+51,adr,sizeof(adr));
    leaves[2]=dsdt+57;std::memcpy(dsdt+57,cca,sizeof(cca));
    leaves[3]=dsdt+63;const auto method=crs();std::memcpy(dsdt+63,method.data(),method.size());
    word(dsdt+4,63+unsigned(method.size()));checksum(dsdt,63+unsigned(method.size()));
}
}
extern "C" void *dt_property(void *,const char *node,const char *key,uint32_t *bytes) {
    assert(!std::strcmp(node,"chosen"));
    if (!std::strcmp(key,"qcom,usb-firmware-iort")) { *bytes=sizeof(record);return record; }
    if (!std::strcmp(key,"usb-iort-component")) { *bytes=componentBytes;return component; }
    if (!std::strcmp(key,USB::FirmwareControllerProperty)) { *bytes=controllerBytes;return controllerRecord; }
    return nullptr;
}
extern "C" void bridge_puts(const char *) {}
extern "C" void bridge_hex(uint64_t) {}
void USBTopologyCaptureTests() {
    EFI_BOOT_SERVICES services{};services.GetMemoryMap=memoryMap;services.LocateProtocol=locate;
    services.LocateHandle=locateUSB;services.HandleProtocol=usbProtocol;
    EFI_CONFIGURATION_TABLE entries[2]{};
    entries[0].VendorGuid=EFI_ACPI_20_TABLE_GUID;entries[0].VendorTable=firmware;
    entries[1]=entries[0];
    EFI_SYSTEM_TABLE system{};system.BootServices=&services;
    system.ConfigurationTable=entries;system.NumberOfTableEntries=1;
    auto failed=[&] {
        assert(bridge_usb_topology_prepare(&system,nullptr)!=0);
        for (auto byte:record) assert(!byte);
        for (auto byte:controllerRecord) assert(!byte);
        assert(!openHandles);
    };
    setup();mapStatus=EFI_DEVICE_ERROR;failed();
    setup();stride=0;failed();
    setup();stride=sizeof(descriptor)+1;failed();
    setup();descriptor.Type=EfiBootServicesData;failed();
    setup();descriptor.NumberOfPages=1;failed();
    setup();firmware[8]^=1;failed();
    setup();word(firmware+20,4097);checksum(firmware,36,32);failed();
    setup();firmware[0x1009]^=1;failed();
    setup();word(firmware+0x1004,43);checksum(firmware+0x1000,43);failed();
    setup();pair(firmware+0x1024,reinterpret_cast<uintptr_t>(firmware+0x3fff));checksum(firmware+0x1000,44);failed();
    setup();firmware[0x2009]^=1;failed();
    setup();component[5]=0;failed();
    for (int fault=1;fault<=6;++fault) {setup();protocolFault=fault;failed();}
    setup();protocolFault=8;failed();
    for (int fault=1;fault<=21;++fault) {
        setup();usbFault=fault;
        if (fault==9) for (auto &c:configs) c.revision=0x10002;
        if (fault==10) configs[0].getBase=nullptr;
        if (fault==11) configs[0].getCoreCount=nullptr;
        if (fault==16) configs[1].mode=2;
        if (fault==17) configs[0].core=2;
        if (fault==19 || fault==20) {
            configHandles=3;configs[2]=configs[fault==19 ? 1:0];
        }
        failed();
    }
    // The same firmware object can remain on several disconnected handles.
    // Query binding accepts such aliases, and the real control-only object
    // before any USB client driver starts. No active DMA permission follows.
    USB::FirmwareController expectedController;expectedController.base=0x0a800000;
    expectedController.length=0xfffff;expectedController.interrupt=163;expectedController.interruptFlags=9;
    setup();configHandles=64;assert(BridgeUSB::firmwareController(&services,expectedController));
    setup();configHandles=65;assert(!BridgeUSB::firmwareController(&services,expectedController) && !configQueries);
    setup();configHandles=1;assert(BridgeUSB::firmwareController(&services,expectedController));
    setup();configs[0]=configs[1];assert(!BridgeUSB::firmwareController(&services,expectedController));
    setup();configs[0].revision=0x10002;assert(BridgeUSB::firmwareController(&services,expectedController));
    setup();services.LocateHandle=nullptr;assert(!BridgeUSB::firmwareController(&services,expectedController));services.LocateHandle=locateUSB;
    setup();services.HandleProtocol=nullptr;assert(!BridgeUSB::firmwareController(&services,expectedController));services.HandleProtocol=usbProtocol;
    setup();controllerBytes=39;failed();
    setup();firmware[0x3009]^=1;failed();
    setup();firmware[0x4009]^=1;failed();
    setup();firmware[0x4000+50]='x';checksum(firmware+0x4000,SMMU::iort_detail::word(firmware+0x4004));failed();
    setup();firmware[0x4000+56]=1;checksum(firmware+0x4000,SMMU::iort_detail::word(firmware+0x4004));failed();
    setup();firmware[0x4000+62]=1;checksum(firmware+0x4000,SMMU::iort_detail::word(firmware+0x4004));failed();
    const auto method=crs();
    // Exact USB0._CRS bytes from the pinned platform DSDT. Production finds
    // this object through SDT namespace lookup, never by searching for bytes.
    const char reference[]="\x14\x4a\x04\x5f\x43\x52\x53\x00\x08\x52\x42\x55\x46\x11\x1a\x0a\x17\x86\x09\x00\x01\x00\x00\x80\x0a\xff\xff\x0f\x00\x89\x06\x00\x09\x01\xa3\x00\x00\x00\x79\x00\x70\x52\x42\x55\x46\x60\x84\x60\x11\x17\x0a\x14\x89\x06\x00\x19\x01\x7b\x01\x00\x00\x89\x06\x00\x19\x01\x13\x01\x00\x00\x79\x00\x61\xa4\x61";
    assert(sizeof(reference)==method.size()+1 && !std::memcmp(method.data(),reference,method.size()));
    for (unsigned i:{0u,1u,2u,3u,7u,8u,9u,13u,14u,15u,16u,17u,18u,19u,20u,
        29u,30u,31u,32u,33u,38u,40u,41u,45u,46u,47u,48u,49u,50u,51u,52u,
        53u,54u,55u,56u,61u,62u,63u,64u,65u,70u,72u,73u,74u}) {
        setup();firmware[0x4000+63+i]=0xff;checksum(firmware+0x4000,SMMU::iort_detail::word(firmware+0x4004));
        assert(i<method.size());failed();
    }
    setup();system.NumberOfTableEntries=2;failed();system.NumberOfTableEntries=1;
    setup();system.ConfigurationTable=nullptr;failed();system.ConfigurationTable=entries;
    setup();record[30]=1;assert(bridge_usb_topology_prepare(&system,nullptr)!=0 && record[30]==1);
    setup();protocolFault=7; // Real platform: _HID and _CRS, no _ADR.
    assert(bridge_usb_topology_prepare(&system,nullptr)==0);
    assert(configQueries==4);
    exitedEFI=true;
    // Firmware may remove the protocol and free its internal object during
    // ExitBootServices. Finish must inspect only ACPI/captured output bytes.
    for (auto &config:configs) config={};
    const auto expected=USBIORTFixture();assert(!std::memcmp(record,expected.data(),expected.size()));
    assert(bridge_usb_topology_finish()==0);
    USB::FirmwareController controller;
    assert(USB::FirmwareController::decode(controllerRecord,sizeof(controllerRecord),controller) &&
        controller.base==0x0a800000 && controller.length==0xfffff && controller.interrupt==163 && !controller.coherent);
    assert(controller.contains(0x0a800000,0xcd00) && controller.contains(0x0a8f8800,0x400));
    assert(!controller.contains(0x0a800000,0x100000) && !openHandles);
    const auto savedRecord=std::vector<uint8_t>(record,record+sizeof(record));
    const auto savedController=std::vector<uint8_t>(controllerRecord,controllerRecord+sizeof(controllerRecord));
    // Refill only the test's output arrays between independent checks. The
    // EFI caller halts immediately on the first failed post-exit verification.
    auto revoked=[&] {
        assert(bridge_usb_topology_finish()!=0);
        for (auto byte:record) assert(!byte);
        for (auto byte:controllerRecord) assert(!byte);
        std::memcpy(record,savedRecord.data(),sizeof(record));
        std::memcpy(controllerRecord,savedController.data(),sizeof(controllerRecord));
    };
    controllerRecord[24]^=1;revoked();
    for (auto *source:{firmware+8,firmware+0x102c,firmware+0x3000+140,firmware+0x4000+9,
        firmware+0x4000+36,firmware+0x4000+62,firmware+0x4000+63+21}) {
        *source^=1;revoked();*source^=1;
    }
    // A firmware callback that changes the table invalidates and clears the
    // captured record. A completed capture cannot be silently reused.
    firmware[0x2040]^=1;
    assert(bridge_usb_topology_finish()!=0);
    for (auto byte:record) assert(!byte);
    assert(bridge_usb_topology_prepare(&system,nullptr)!=0);
    std::puts("USB firmware topology: ACPI resources, EFI USB instance binding, pre-exit-only protocol queries and post-exit revocation passed");
}
