// SPDX-License-Identifier: BSD-2-Clause
#include <IOKitUser.h>
#include <cstdio>
#include <cstring>
#include <cstdlib>
extern "C" {
extern mach_port_t bootstrap_port;
kern_return_t bootstrap_look_up(mach_port_t, const char *, mach_port_t *);
kern_return_t IORegistryEntryCreateCFProperties(mach_port_t, CFMutableDictionaryRef *, CFAllocatorRef, uint32_t);
kern_return_t IOPSCopyPowerSourcesInfoPrecise(CFTypeRef *);
kern_return_t IOPSGetPercentRemaining(int *, bool *, bool *);
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
static bool printSummary(const char *name,CFDictionaryRef properties) {
    const CFStringRef fields[]={CFSTR("BatteryTelemetryValid"),CFSTR("CurrentCapacity"),
        CFSTR("Voltage"),CFSTR("Amperage"),CFSTR("Temperature"),CFSTR("ExternalConnected"),
        CFSTR("IsCharging"),CFSTR("InputCurrentLimited"),CFSTR("ChargeEnablePinControlled"),
        CFSTR("USBInputStatusValid"),CFSTR("USBInputSettledLimitMA"),CFSTR("USBSDPCurrentLimitMA"),
        CFSTR("USBHighCurrentMode"),
        CFSTR("ChargeLimit"),CFSTR("ChargeInhibitReasons"),CFSTR("ChargeControlCommandEnabled"),
        CFSTR("ChargeControlReadbackValid"),CFSTR("ChargeControlFault"),CFSTR("ChargeControlError")};
    fputs(name,stdout);
    for(auto field:fields) {
        auto value=CFDictionaryGetValue(properties,field);
        if(!value)continue;
        char key[64];
        if(!CFStringGetCString(field,key,sizeof(key),kCFStringEncodingUTF8))return false;
        if(CFGetTypeID(value)==CFBooleanGetTypeID())
            printf(" %s=%s",key,value==kCFBooleanTrue?"true":"false");
        else if(CFGetTypeID(value)==CFNumberGetTypeID()) {
            int64_t number=0;
            if(!CFNumberGetValue(static_cast<CFNumberRef>(value),kCFNumberSInt64Type,&number))return false;
            printf(" %s=%lld",key,static_cast<long long>(number));
        }
    }
    return fputc('\n',stdout)!=EOF && !ferror(stdout);
}
int main(int argc,char **argv) {
    const char *usage="usage: batteryctl --system | batteryctl [--summary] [--charging on|off] [--limit 50..100]\n";
    bool system=false, summary=false, changeCharging=false, enabled=false;
    unsigned limit=0;
    for(int i=1;i<argc;++i) {
        if(!strcmp(argv[i],"--help") && argc==2) { fputs(usage,stdout);return 0; }
        if(!strcmp(argv[i],"--system") && !system) system=true;
        else if(!strcmp(argv[i],"--summary") && !summary) summary=true;
        else if(!strcmp(argv[i],"--charging") && !changeCharging && i+1<argc) {
            const char *v=argv[++i];
            if(strcmp(v,"on") && strcmp(v,"off")) { fputs(usage,stderr);return 64; }
            changeCharging=true;enabled=!strcmp(v,"on");
        } else if(!strcmp(argv[i],"--limit") && !limit && i+1<argc) {
            const char *v=argv[++i];
            if(!*v || strlen(v)>3 || strspn(v,"0123456789")!=strlen(v)) { fputs(usage,stderr);return 64; }
            limit=unsigned(strtoul(v,nullptr,10));
            if(limit<50 || limit>100) { fputs(usage,stderr);return 64; }
        } else { fputs(usage,stderr);return 64; }
    }
    if(system && (summary || changeCharging || limit)) { fputs(usage,stderr);return 64; }
    if(changeCharging || limit) {
        mach_port_t iterator=MACH_PORT_NULL, selected=MACH_PORT_NULL;
        auto match=IOServiceMatching("QcomBattery");
        if(!match || IOServiceGetMatchingServices(MACH_PORT_NULL,match,&iterator)!=KERN_SUCCESS) return 1;
        unsigned count=0;
        while(auto service=IOIteratorNext(iterator)) {
            if(!count++) selected=service;
            else IOObjectRelease(service);
        }
        IOObjectRelease(iterator);
        if(count!=1) {
            if(selected) IOObjectRelease(selected);
            fprintf(stderr,"Expected one QcomBattery, found %u. No control applied.\n",count);return 2;
        }
        auto request=CFDictionaryCreateMutable(kCFAllocatorDefault,0,
            &kCFTypeDictionaryKeyCallBacks,&kCFTypeDictionaryValueCallBacks);
        if(!request) { IOObjectRelease(selected);return 1; }
        if(changeCharging) CFDictionarySetValue(request,CFSTR("ChargingEnabled"),enabled?kCFBooleanTrue:kCFBooleanFalse);
        if(limit) {
            int value=int(limit);
            auto number=CFNumberCreate(kCFAllocatorDefault,kCFNumberIntType,&value);
            if(!number) { CFRelease(request);IOObjectRelease(selected);return 1; }
            CFDictionarySetValue(request,CFSTR("ChargeLimit"),number);CFRelease(number);
        }
        const auto status=IORegistryEntrySetCFProperties(selected,request);
        CFRelease(request);IOObjectRelease(selected);
        if(status!=KERN_SUCCESS) {
            fprintf(stderr,"Charge policy request failed: 0x%08x. Inspect ChargeControlError/ChargeControlFault.\n",unsigned(status));
            return 1;
        }
    }
    if(system) {
        int percent=-1; bool charging=false,full=false;
        const auto notifyStatus=IOPSGetPercentRemaining(&percent,&charging,&full);
        printf("SystemBatteryNotification status=0x%08x valid=%s percent=%d charging=%s full=%s\n",
            unsigned(notifyStatus),notifyStatus==KERN_SUCCESS?"true":"false",
            notifyStatus==KERN_SUCCESS?percent:-1,charging?"true":"false",full?"true":"false");
        mach_port_t server=MACH_PORT_NULL;
        const auto lookup=bootstrap_look_up(bootstrap_port,"com.apple.PowerManagement.control",&server);
        printf("PowerManagementService lookup=0x%08x available=%s\n",unsigned(lookup),
               lookup==KERN_SUCCESS && server!=MACH_PORT_NULL?"true":"false");
        if(server!=MACH_PORT_NULL)mach_port_deallocate(mach_task_self(),server);
        // The mobile notification path can be live without the separate
        // PowerManagement MIG service used by Precise. Report each independently.
        if(lookup!=KERN_SUCCESS || server==MACH_PORT_NULL) {
            fputs("PrecisePowerSources unavailable: PowerManagement service not registered.\n",stdout);
            return notifyStatus==KERN_SUCCESS ? 0:2;
        }
        CFTypeRef info=nullptr;
        // Apple's iOS pmset uses the Precise API; retain its error instead of
        // interpreting an empty coarse result as failed powerd discovery.
        const auto status=IOPSCopyPowerSourcesInfoPrecise(&info);
        auto list=info ? IOPSCopyPowerSourcesList(info) : nullptr;
        const auto count=list ? CFArrayGetCount(list):0;
        bool ok=status==KERN_SUCCESS && count>0;
        if(list)for(CFIndex i=0;i<CFArrayGetCount(list);++i) {
            auto d=IOPSGetPowerSourceDescription(info,CFArrayGetValueAtIndex(list,i));
            if(!d || !print(d))ok=false;
        }
        if(list)CFRelease(list);
        if(info)CFRelease(info);
        if(!ok)fprintf(stderr,"Precise power source query: status=0x%08x sources=%ld.\n",
                       unsigned(status),static_cast<long>(count));
        return ok || notifyStatus==KERN_SUCCESS ? 0:2;
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
                if(!(summary ? printSummary(name,properties):print(properties)))error=true;
                CFRelease(properties);
            }
            IOObjectRelease(service);
        }
        IOObjectRelease(iterator);
    }
    if(!found)fprintf(stderr,"No QcomBattery service. Check collection and qcom,battery-observer device tree configuration.\n");
    else if(!valid)fprintf(stderr,"Battery service found, but no valid current sample. Inspect BatteryReadError/BatteryLastIOError.\n");
    return error ? 1 : ((valid || changeCharging || limit) ? 0:2);
}

// The USB development console can load the same client before an executable
// has been installed. Layout follows Bash's documented loadable-builtin ABI.
struct BatteryWord { char *word; int flags; };
struct BatteryWordList { BatteryWordList *next; BatteryWord *word; };
static int batteryBuiltin(BatteryWordList *words) {
    char name[]="batteryctl";
    char *args[7]={name};
    int count=1;
    for(auto *entry=words;entry;entry=entry->next) {
        if(!entry->word || !entry->word->word || count==6) return 64;
        args[count++]=entry->word->word;
    }
    fflush(stdout);fflush(stderr);
    int result=main(count,args);
    if(fflush(stdout) || fflush(stderr)) return result ? result:1;
    return result;
}
static const char *batteryDocumentation[]={"Read battery telemetry and apply protected charge policy.",nullptr};
extern "C" {
__attribute__((visibility("default"))) struct {
    const char *name;
    int (*function)(BatteryWordList *);
    int flags;
    const char **documentation;
    const char *usage;
    void *handle;
} batteryctl_struct={"batteryctl",batteryBuiltin,1,batteryDocumentation,
    "batteryctl --system | batteryctl [--summary] [--charging on|off] [--limit 50..100]",nullptr};
}
