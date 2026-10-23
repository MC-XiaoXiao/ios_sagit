// SPDX-License-Identifier: BSD-2-Clause
#pragma once
#include <Uefi.h>
#include "../../drivers/usb/QcomUSB/USBFirmware.h"

namespace BridgeUSB {
constexpr EFI_GUID ConfigProtocolGUID={0xe722b03f,0xb250,0x42ce,{0x8e,0xbd,0x5b,0xd5,0x18,0x12,0xd0,0x37}};
// Prefix verified against the project's UsbConfigDxe revision0x10003.
// The entire firmware object is160 bytes; only these144 bytes are consumed.
// CoreType5 identifies the primary DWC3 IP, whereas CoreNum0 selects the
// client controller. The management object has Core1 and invalid Mode.
struct ConfigProtocol {
    UINT64 revision;
    EFI_STATUS (EFIAPI *getBase)(ConfigProtocol *,UINT32,UINT64 *);
    void *legacy[8];
    void *startController;
    EFI_STATUS (EFIAPI *stopController)(ConfigProtocol *,UINT32,UINT32);
    void *enterLPM,*exitLPM,*toggleMode;
    EFI_STATUS (EFIAPI *getCoreCount)(ConfigProtocol *,UINT8 *);
    void *getSupportedMode;
    UINT32 core,mode;
};
static_assert(sizeof(ConfigProtocol)==144 && offsetof(ConfigProtocol,getBase)==8 &&
    offsetof(ConfigProtocol,getCoreCount)==120 && offsetof(ConfigProtocol,core)==136,
    "USB Config revision0x10003 prefix");

// Bind the installed ACPI aperture to the firmware's actual configuration
// instances before EFI exits. Old client handles may alias the same object
// after a disconnect/rebind; distinct objects claiming the same role are
// ambiguous. No firmware pointer or callback survives this function.
// This query grants no clock, stream, context or DMA ownership and does not
// stop the controller. In particular, never call getBase after EFI exit:
// UsbConfigDxe's callback can free its internal core object.
inline bool firmwareController(EFI_BOOT_SERVICES *services,const USB::FirmwareController &controller) {
    if (!services || !services->LocateHandle || !services->HandleProtocol || !controller.valid()) return false;
    EFI_HANDLE handles[64]{};UINTN bytes=sizeof(handles);
    auto guid=ConfigProtocolGUID;
    if (EFI_ERROR(services->LocateHandle(ByProtocol,&guid,nullptr,&bytes,handles)) ||
        !bytes || bytes>sizeof(handles) || bytes%sizeof(EFI_HANDLE)) return false;
    const ConfigProtocol *control=nullptr,*client=nullptr;
    for (UINTN i=0;i<bytes/sizeof(EFI_HANDLE);++i) {
        if (!handles[i]) return false;
        for (UINTN j=0;j<i;++j) if (handles[j]==handles[i]) return false;
        ConfigProtocol *config=nullptr;
        if (EFI_ERROR(services->HandleProtocol(handles[i],&guid,reinterpret_cast<void **>(&config))) || !config) return false;
        // Do not inspect an unverified revision beyond its common header.
        if (config->revision!=0x10003) continue;
        const auto saved=*config;
        if (!saved.getBase || !saved.getCoreCount || saved.core>1 ||
            (saved.core==1 ? saved.mode!=0x10000 : saved.mode!=1 && saved.mode!=4 && saved.mode!=0x10000)) return false;
        UINT8 count=0;UINT64 base=0;
        if (EFI_ERROR(saved.getCoreCount(config,&count)) || count!=1 ||
            EFI_ERROR(saved.getBase(config,5,&base))) return false;
        const auto *a=reinterpret_cast<const UINT8 *>(&saved),*b=reinterpret_cast<const UINT8 *>(config);
        for (unsigned j=0;j<sizeof(saved);++j) if (a[j]!=b[j]) return false;
        if (base!=controller.base) continue;
        auto &found=saved.core ? control:client;
        if (found && found!=config) return false;
        found=config;
    }
    return control || client;
}
}
