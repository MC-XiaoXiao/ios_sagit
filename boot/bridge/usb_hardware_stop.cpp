// SPDX-License-Identifier: BSD-2-Clause
#include "usb_hardware_stop.h"
extern "C" {
#include "bridge.h"
}
namespace {
struct Registers {
    bool failed=false;
    static uint32_t read(uintptr_t address) {
        const auto value=*reinterpret_cast<volatile uint32_t *>(address);
        __asm__ volatile("dmb oshld" ::: "memory");return value;
    }
    static void write(uintptr_t address,uint32_t value) {
        __asm__ volatile("dmb oshst" ::: "memory");
        *reinterpret_cast<volatile uint32_t *>(address)=value;
        __asm__ volatile("dsb sy" ::: "memory");
    }
    uint32_t readGCC(uint32_t offset) {return !failed && offset<0x94000 && !(offset&3) ? read(0x100000+offset):UINT32_MAX;}
    void writeGCC(uint32_t offset,uint32_t value) {
        if(failed || offset!=0xf000 || !(value&1)) {failed=true;return;}
        write(0x100000+offset,value);
    }
    uint32_t readCore(uint32_t offset) {return !failed && offset<=DWC3::Aperture-4 && !(offset&3) ? read(0xa800000+offset):UINT32_MAX;}
    void writeCore(uint32_t offset,uint32_t value) {
        const bool host=offset>=0x20 && offset<=0x80 && !(offset&3);
        const bool event=offset>=DWC3::eventSize(0) && offset<=DWC3::eventSize(31) &&
            (offset-DWC3::eventSize(0))%16==0;
        if(failed || (!host && !event && offset!=DWC3::DEVTEN && offset!=DWC3::DCTL)) {failed=true;return;}
        write(0xa800000+offset,value);
    }
    void delay(unsigned us) {
        uint64_t first,now,hz;
        __asm__ volatile("mrs %0,cntpct_el0" : "=r"(first));
        __asm__ volatile("mrs %0,cntfrq_el0" : "=r"(hz));
        if(!hz || hz>UINT32_MAX || us>16000) {failed=true;return;}
        for(unsigned spin=0;spin<65536;++spin) {
            __asm__ volatile("mrs %0,cntpct_el0" : "=r"(now));
            if(now-first>=(hz*us+999999)/1000000)return;
        }
        failed=true;
    }
} registers;
BridgeFirmware::HardwareStop<Registers> hardware(registers);
const void *stopRecord=nullptr;
}
extern "C" int bridge_usb_hardware_stop_prepare(EFI_BOOT_SERVICES *services,void *tree) {
    uint32_t bytes=0;auto *source=dt_property(tree,"chosen",USB::FirmwareControllerProperty,&bytes);
    USB::FirmwareController controller;
    if(!USB::FirmwareController::decode(source,bytes,controller))return -1;
    stopRecord=dt_property(tree,"chosen",USB::FirmwareStopProperty,&bytes);
    if(!stopRecord || bytes!=sizeof(USB::FirmwareStop))return -2;
    auto *out=dt_property(tree,"chosen",USB::HardwareStopProperty,&bytes);
    return hardware.prepare(services,controller,out,bytes) ? 0:-3;
}
extern "C" int bridge_usb_hardware_stop_finish(void) {
    if(!stopRecord)return -1;
    USB::FirmwareStop stop;
    const auto *p=static_cast<const uint8_t *>(stopRecord);auto *q=reinterpret_cast<uint8_t *>(&stop);
    for(unsigned n=0;n<sizeof(stop);++n)q[n]=p[n];
    return hardware.finish(stop) ? 0:-1;
}
