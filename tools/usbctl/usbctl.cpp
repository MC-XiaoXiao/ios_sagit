// SPDX-License-Identifier: BSD-2-Clause
// USB bring-up diagnostics and an explicit Native reconnect command.
#include <IOKitUser.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

extern "C" mach_port_t IORegistryEntryFromPath(mach_port_t, const char *);
extern "C" kern_return_t IORegistryCreateIterator(mach_port_t, const char *, uint32_t, mach_port_t *);
extern "C" kern_return_t IORegistryEntryGetName(mach_port_t, char *);

static void property(mach_port_t entry, const char *key) {
    CFStringRef name=CFStringCreateWithCString(nullptr,key,kCFStringEncodingUTF8);
    if (!name) return;
    CFTypeRef value=IORegistryEntryCreateCFProperty(entry,name,nullptr,0);
    CFRelease(name);
    if (!value) return;
    printf("  %s=",key);
    if (CFGetTypeID(value)==CFDataGetTypeID()) {
        auto data=static_cast<CFDataRef>(value);
        const auto size=CFDataGetLength(data);
        const auto *bytes=CFDataGetBytePtr(data);
        printf("data[%ld]:",static_cast<long>(size));
        for (CFIndex i=0;i<size && i<96;++i) printf("%02x",bytes[i]);
        if (size>96) printf("...");
    } else if (CFGetTypeID(value)==CFBooleanGetTypeID()) {
        printf("%s",CFBooleanGetValue(static_cast<CFBooleanRef>(value)) ? "true":"false");
    } else if (CFGetTypeID(value)==CFNumberGetTypeID()) {
        int64_t number=0;
        if (CFNumberGetValue(static_cast<CFNumberRef>(value),kCFNumberSInt64Type,&number))
            printf("%lld",static_cast<long long>(number));
    } else {
        CFStringRef text=CFGetTypeID(value)==CFStringGetTypeID() ? static_cast<CFStringRef>(CFRetain(value)):CFCopyDescription(value);
        char buffer[4096];
        if (text && CFStringGetCString(text,buffer,sizeof(buffer),kCFStringEncodingUTF8)) printf("%s",buffer);
        else printf("<description too long>");
        if (text) CFRelease(text);
    }
    putchar('\n');CFRelease(value);
}

static int registry(const char *name,const char *selectedProperty=nullptr) {
    mach_port_t iterator=0;
    // Include registered nubs still waiting for IOResourceMatch. Ordinary
    // matching queries expose only services whose matching has completed.
    constexpr uint32_t iterateRecursively=1;
    auto status=IORegistryCreateIterator(0,"IOService",iterateRecursively,&iterator);
    printf("USB_REGISTRY class=%s status=0x%x\n",name,status);
    if (status) return 1;
    unsigned count=0;
    for (mach_port_t entry;(entry=IOIteratorNext(iterator));) {
        if (!IOObjectConformsTo(entry,name)) {IOObjectRelease(entry);continue;}
        ++count;uint64_t identity=0;
        IORegistryEntryGetRegistryEntryID(entry,&identity);
        char entryName[128]={};IORegistryEntryGetName(entry,entryName);
        printf("USB_SERVICE id=%llu name=%s\n",static_cast<unsigned long long>(identity),entryName);
        if (selectedProperty) {
            property(entry,selectedProperty);IOObjectRelease(entry);continue;
        }
        const char *keys[]={"IOClass","CFBundleIdentifier","compatible","USBRole","USBDevicePersonality",
            "RPMStartupStage","ClockControllerKind","PowerControlCapabilities","BootTransportAdopted",
            "InterconnectRPMStatus","InterconnectRPMCapabilities","InterconnectSupportStatus",
            "USBConnectorPowerReady","USBConnectorKind","USBOverCurrent",
            "USBConnectorAcquireStage","USBConnectorAcquireStatus","USBConnectorInterruptIndex",
            "USBConnectorInitialRegisters","USBConnectorProbeStatus","USBResourceAcquireStatus",
            "USBResourceWait","USBResourceFault","USBDMAOwnershipMode","USBDMAMemoryMode",
            "USBControllerWait",
            "USBResourceAcquireStage","USBSMMUControl","USBSMMUIDR0","USBSMMUIDR1","USBDMACaptureStatus",
            "USBPhyKind","USBPhyReady","USBPhyFault","USBConnection","USBOrientation","USBPowerRole",
            "USBConnectorReady","USBConnectorFault","USBConnectorLastError","USBInterconnectReady",
            "USBInterconnectFault","USBControllerReady","USBControllerFault","USBDeviceState",
            "USBDeviceConfiguration","IOUSBDeviceControllerName","IOUserClientClass","IOPowerManagement",
            "qcom,usb-controller-resources","qcom,usb-dma-capture","qcom,usb-smmu-bypass",
            "qcom,usb-smmu-handoff","qcom,usb-connector","qcom,usb-resource-handoff"};
        for (const auto *key:keys) property(entry,key);
        IOObjectRelease(entry);
    }
    IOObjectRelease(iterator);
    printf("USB_REGISTRY_COUNT class=%s count=%u\n",name,count);return 0;
}

static int nativeCommand(bool debug) {
    if(geteuid()!=0) {fprintf(stderr,"reconnect requires root\n");return 1;}
    auto matching=IOServiceMatching("QcomUSBDevice");
    if(!matching)return 1;
    mach_port_t iterator=0;
    auto status=IOServiceGetMatchingServices(0,matching,&iterator);
    if(status) {fprintf(stderr,"reconnect matching failed:0x%x\n",status);return 1;}
    auto device=IOIteratorNext(iterator),extra=IOIteratorNext(iterator);
    IOObjectRelease(iterator);
    if(!device || extra) {
        if(device)IOObjectRelease(device);
        if(extra)IOObjectRelease(extra);
        fprintf(stderr,"reconnect requires exactly one QcomUSBDevice\n");return 1;
    }
    // The pinned Family's gated_setProperties recognizes this command and
    // uses its own off/on timer, configuration policy and hardware hooks.
    const int32_t flags=0x4000000f;
    auto number=debug ? CFNumberCreate(nullptr,kCFNumberSInt32Type,&flags):nullptr;
    const void *keys[]={CFSTR("USBDeviceCommand"),CFSTR("USBDeviceCommandParameter")};
    const void *values[]={debug ? CFSTR("SetDebugFlag"):CFSTR("GoOffAndOnBus"),number};
    auto command=(!debug || number) ? CFDictionaryCreate(nullptr,keys,values,debug ? 2:1,
        &kCFTypeDictionaryKeyCallBacks,&kCFTypeDictionaryValueCallBacks):nullptr;
    status=command ? IORegistryEntrySetCFProperties(device,command):KERN_RESOURCE_SHORTAGE;
    if(command)CFRelease(command);
    if(number)CFRelease(number);
    IOObjectRelease(device);
    printf("USB_NATIVE_%s status=0x%x\n",debug ? "DEBUG":"RECONNECT",status);
    return status ? 1:0;
}

static int muxDebug(bool enabled) {
    if(geteuid()!=0) {fprintf(stderr,"mux-debug requires root\n");return 1;}
    auto matching=IOServiceMatching("AppleUSBDeviceMux");
    if(!matching)return 1;
    mach_port_t iterator=0;
    auto status=IOServiceGetMatchingServices(0,matching,&iterator);
    if(status)return 1;
    auto device=IOIteratorNext(iterator),extra=IOIteratorNext(iterator);
    IOObjectRelease(iterator);
    if(!device || extra) {
        if(device)IOObjectRelease(device);
        if(extra)IOObjectRelease(extra);
        fprintf(stderr,"mux-debug requires exactly one AppleUSBDeviceMux\n");return 1;
    }
    // Original Mux setPropertiesGated accepts this numeric debug level.
    const int32_t level=enabled ? 8:0;
    auto number=CFNumberCreate(nullptr,kCFNumberSInt32Type,&level);
    const void *keys[]={CFSTR("DebugLevel")};
    const void *values[]={number};
    auto command=number ? CFDictionaryCreate(nullptr,keys,values,1,
        &kCFTypeDictionaryKeyCallBacks,&kCFTypeDictionaryValueCallBacks):nullptr;
    status=command ? IORegistryEntrySetCFProperties(device,command):KERN_RESOURCE_SHORTAGE;
    if(command)CFRelease(command);
    if(number)CFRelease(number);
    IOObjectRelease(device);
    printf("USB_MUX_DEBUG level=%d status=0x%x\n",level,status);
    return status ? 1:0;
}

int main(int argc,char **argv) {
    if(argc==2 && !strcmp(argv[1],"reconnect"))return nativeCommand(false);
    if(argc==2 && !strcmp(argv[1],"native-debug"))return nativeCommand(true);
    if(argc==3 && !strcmp(argv[1],"mux-debug") &&
        (!strcmp(argv[2],"on") || !strcmp(argv[2],"off")))return muxDebug(!strcmp(argv[2],"on"));
    if (argc<2 || argc>4 || strcmp(argv[1],"registry")) {
        fprintf(stderr,"usage: usbctl registry [IOKit-class [property]] | reconnect | native-debug | mux-debug on|off\n");return 2;
    }
    if (argc==4) return registry(argv[2],argv[3]);
    if (argc==3) return registry(argv[2]);
    auto chosen=IORegistryEntryFromPath(0,"IODeviceTree:/chosen");
    if (chosen) {
        puts("USB_CHOSEN");
        const char *keys[]={"usb-iort-component","qcom,usb-firmware-controller","qcom,usb-firmware-stop",
            "qcom,usb-hardware-stop","qcom,usb-firmware-iort"};
        for (const auto *key:keys) property(chosen,key);
        IOObjectRelease(chosen);
    }
    const char *classes[]={"QcomUSBResources","QcomUSBPHY","QcomUSBConnector","QcomInterconnect",
        "QcomUSB","QcomUSBController","IOUSBDeviceController","IOUSBDeviceInterface","AppleUSBDeviceMux"};
    int result=0;for (const auto *name:classes) result|=registry(name);
    return result;
}
