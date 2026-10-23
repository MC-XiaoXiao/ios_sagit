// SPDX-License-Identifier: BSD-2-Clause
#pragma once
#include "USBProtocol.h"

namespace USB {
constexpr const char *FirmwareControllerProperty="qcom,usb-firmware-controller";
// Firmware's installed ACPI namespace binds an IORT component to its actual
// controller resources. This record describes that binding only. It grants
// neither a DMA window nor ownership of a stream, context or clock.
struct FirmwareController {
    uint32_t version=1,bytes=sizeof(FirmwareController);
    uint64_t base=0,length=0;
    uint32_t interrupt=0,interruptFlags=0,coherent=0,reserved=0;
    bool valid() const {
        return version==1 && bytes==sizeof(*this) && !reserved && base && !(base&4095) &&
            length && length<=0x1000000 && base<=UINT64_MAX-length &&
            interrupt>=32 && interrupt<=1019 && (interruptFlags&1) &&
            !(interruptFlags&~0x19u) && coherent<=1;
    }
    bool contains(uint64_t address,uint64_t bytes) const {
        return valid() && bytes && address>=base && address-base<=length && bytes<=length-(address-base);
    }
    static bool decode(const void *data,size_t size,FirmwareController &out) {
        out={};if (!data || size!=sizeof(out)) return false;
        const auto *p=static_cast<const uint8_t *>(data);auto *q=reinterpret_cast<uint8_t *>(&out);
        for (unsigned n=0;n<sizeof(out);++n) q[n]=p[n];
        return out.valid();
    }
};
static_assert(sizeof(FirmwareController)==40 && offsetof(FirmwareController,interrupt)==24,
    "firmware USB controller binding ABI");

constexpr const char *FirmwareStopProperty="qcom,usb-firmware-stop";
// Closing the firmware's events and detaching its drivers removes software
// producers. This is not a DWC3 halt/reset, fabric/clock lease or DMA grant.
// Only a successful EFI exit and final immutable/state checks publish it.
struct FirmwareStop {
    enum : uint32_t { EventsClosed=1, DriversDetached=2, EFIExited=4, Required=7 };
    uint32_t version=1,bytes=sizeof(FirmwareStop),flags=0,previousMode=0;
    uint64_t controller=0,length=0;
    uint32_t coreType=5,closedEvents=0,removedInterfaces=0,reserved=0;
    uint64_t grants=0,padding=0;
    bool valid() const {
        return version==1 && bytes==sizeof(*this) && flags==Required &&
            (previousMode==1 || previousMode==4 || previousMode==0x10000) &&
            controller && !(controller&4095) && length && length<=0x1000000 &&
            controller<=UINT64_MAX-length && coreType==5 && closedEvents<=4 &&
            removedInterfaces && removedInterfaces<=64 && !reserved && !grants && !padding;
    }
};
static_assert(sizeof(FirmwareStop)==64,"EFI USB firmware software stop ABI");

constexpr const char *HardwareStopProperty="qcom,usb-hardware-stop";
// Produced after EFI exit and checked again immediately before XNU entry.
// It proves controller DMA isolation only. Fabric/clock leases and the DMA
// stream/context/address window are acquired and verified separately.
struct HardwareStop {
    enum : uint32_t { Quiesced=1, ResetHeld=2, EFIExited=4, InheritedReset=8, Required=7 };
    uint32_t version=1,bytes=sizeof(HardwareStop),flags=0,previousRole=0;
    uint64_t controller=0,length=0,gcc=0x100000;
    uint32_t resetOffset=0xf000,resetBefore=0,resetAfter=0,domain=0;
    uint64_t grants=0;
    bool valid() const {
        return version==1 && bytes==sizeof(*this) && (flags&Required)==Required && !(flags&~15u) &&
            controller==0xa800000 && length>=0xcd00 && length<=0x1000000 && gcc==0x100000 &&
            resetOffset==0xf000 && resetBefore!=UINT32_MAX && resetAfter!=UINT32_MAX &&
            resetAfter==(resetBefore|1u) && domain!=UINT32_MAX && !grants &&
            ((flags&InheritedReset) ? ((resetBefore&1) && !previousRole) :
                (!(resetBefore&1) && (previousRole==1 || previousRole==2) && (domain&0x80000001u)==0x80000000u));
    }
};
static_assert(sizeof(HardwareStop)==64,"EFI USB hardware DMA stop ABI");
}
