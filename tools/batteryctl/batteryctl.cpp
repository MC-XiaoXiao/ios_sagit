// SPDX-License-Identifier: BSD-2-Clause
#include <IOKitUser.h>
#include <cstdio>
#include <cstring>
extern "C" {
kern_return_t IORegistryEntryCreateCFProperties(mach_port_t, CFMutableDictionaryRef *, CFAllocatorRef, uint32_t);
CFTypeRef IOPSCopyPowerSourcesInfo(void);
CFArrayRef IOPSCopyPowerSourcesList(CFTypeRef);
CFDictionaryRef IOPSGetPowerSourceDescription(CFTypeRef, CFTypeRef);
}
static bool print(CFPropertyListRef value) {
    CFErrorRef error=nullptr;
    auto data=CFPropertyListCreateData(kCFAllocatorDefault,value,kCFPropertyListXMLFormat_v1_0,0,&error);
    if(error)CFRelease(error);
    if(!data)return false;
    bool ok=fwrite(CFDataGetBytePtr(data),1,CFDataGetLength(data),stdout)==size_t(CFDataGetLength(data));
    CFRelease(data);return ok;
}
int main(int argc,char **argv) {
    if(argc>2 || (argc==2 && strcmp(argv[1],"--system"))) {
        fprintf(stderr,"usage: batteryctl [--system]\n");return 64;
    }
    if(argc==2) {
        auto info=IOPSCopyPowerSourcesInfo();
        auto list=info ? IOPSCopyPowerSourcesList(info) : nullptr;
        bool ok=list && CFArrayGetCount(list)>0;
        if(list)for(CFIndex i=0;i<CFArrayGetCount(list);++i) {
            auto d=IOPSGetPowerSourceDescription(info,CFArrayGetValueAtIndex(list,i));
            if(!d || !print(d))ok=false;
        }
        if(list)CFRelease(list);
        if(info)CFRelease(info);
        if(!ok)fprintf(stderr,"System power source data unavailable; check powerd discovery separately.\n");
        return ok ? 0:2;
    }
    bool valid=false, error=false, found=false;
    const char *classes[]={"QcomBattery","QcomBatteryPowerSource"};
    for(const char *name : classes) {
        mach_port_t iterator=MACH_PORT_NULL;
        auto match=IOServiceMatching(name);
        if(!match || IOServiceGetMatchingServices(MACH_PORT_NULL,match,&iterator)!=KERN_SUCCESS) {
            error=true;continue;
        }
        while(auto service=IOIteratorNext(iterator)) {
            found=true;CFMutableDictionaryRef properties=nullptr;
            if(IORegistryEntryCreateCFProperties(service,&properties,kCFAllocatorDefault,0)!=KERN_SUCCESS ||
               !properties)error=true;
            else {
                if(!strcmp(name,"QcomBatteryPowerSource") &&
                   CFDictionaryGetValue(properties,CFSTR("BatteryTelemetryValid"))==kCFBooleanTrue)valid=true;
                if(!print(properties))error=true;
                CFRelease(properties);
            }
            IOObjectRelease(service);
        }
        IOObjectRelease(iterator);
    }
    if(!found)fprintf(stderr,"No QcomBattery service. Check collection and qcom,battery-observer device tree configuration.\n");
    else if(!valid)fprintf(stderr,"Battery service found, but no valid current sample. Inspect BatteryReadError/BatteryLastIOError.\n");
    return error ? 1 : (valid ? 0:2);
}
