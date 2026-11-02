// SPDX-License-Identifier: BSD-2-Clause
#pragma once
#include <Uefi.h>
#include <Protocol/LoadedImage.h>
#include <stdint.h>
#include <stddef.h>

namespace BridgeFirmware {
inline uint32_t word(const void *data) {
    const auto *p=static_cast<const uint8_t *>(data);
    return uint32_t(p[0])|uint32_t(p[1])<<8|uint32_t(p[2])<<16|uint32_t(p[3])<<24;
}
inline uint64_t pair(const void *data) {
    const auto *p=static_cast<const uint8_t *>(data);return word(p)|uint64_t(word(p+4))<<32;
}
inline bool equal(const void *a,const void *b,unsigned bytes) {
    const auto *x=static_cast<const uint8_t *>(a),*y=static_cast<const uint8_t *>(b);
    for (unsigned i=0;i<bytes;++i) if(x[i]!=y[i]) return false;
    return true;
}
class Memory {
    alignas(16) uint8_t descriptors[65536]{};
    UINTN bytes=0,stride=0;
public:
    bool prepare(EFI_BOOT_SERVICES *services) {
        bytes=sizeof(descriptors);stride=0;UINTN key=0;UINT32 version=0;
        return services && services->GetMemoryMap &&
            !EFI_ERROR(services->GetMemoryMap(&bytes,reinterpret_cast<EFI_MEMORY_DESCRIPTOR *>(descriptors),&key,&stride,&version)) &&
            bytes && bytes<=sizeof(descriptors) && stride>=sizeof(EFI_MEMORY_DESCRIPTOR) &&
            !(stride&7) && !(bytes%stride) && version==EFI_MEMORY_DESCRIPTOR_VERSION;
    }
    bool contains(const void *data,uint64_t size,uint32_t types=(1u<<EfiBootServicesCode)|(1u<<EfiBootServicesData)|
                  (1u<<EfiLoaderCode)|(1u<<EfiLoaderData)) const {
        auto address=reinterpret_cast<uintptr_t>(data);
        if (!address || !size || address>UINT64_MAX-size || !stride) return false;
        // Sections can have distinct code/data descriptors. Cover the entire
        // span without reading through a hole, MMIO, runtime or reclaimed RAM.
        while (size) {
            uint64_t available=0;
            for (UINTN offset=0;offset<bytes;offset+=stride) {
                const auto *d=reinterpret_cast<const EFI_MEMORY_DESCRIPTOR *>(descriptors+offset);
                if (d->Type>=32 || !(types&(1u<<d->Type))) continue;
                if (d->NumberOfPages>(UINT64_MAX>>12)) continue;
                const auto length=d->NumberOfPages<<12;
                if (address>=d->PhysicalStart && address-d->PhysicalStart<length) {
                    const auto remaining=length-(address-d->PhysicalStart);
                    if (remaining>available) available=remaining;
                }
            }
            if (!available) return false;
            const auto n=available<size ? available:size;address+=n;size-=n;
        }
        return true;
    }
};
// Build-time SHA-256 pins the original PE. The compact pin contains its whole
// code section and immutable protocol table, with every DIR64 relocation in
// those ranges. Runtime comparison accounts for the actual load address.
// No private RVA is used until this exact loaded image has been identified.
class CodePin {
    const uint8_t *pin=nullptr,*relocations=nullptr,*code=nullptr,*protocol=nullptr;
    uint32_t imageBytes=0,codeRVA=0,codeBytes=0,protocolRVA=0,protocolBytes=0,count=0;
    bool range(uint32_t rva,uint32_t bytes) const {
        return bytes && rva<=imageBytes && bytes<=imageBytes-rva;
    }
    bool segment(const uint8_t *actual,const uint8_t *reference,uint32_t rva,uint32_t bytes,uint64_t base) const {
        unsigned copied=0;
        for (unsigned i=0;i<count;++i) {
            const auto location=word(relocations+4*i);
            if (location<rva || location-rva>=bytes) continue;
            const auto offset=location-rva;
            if (offset<copied || bytes-offset<8 || !equal(actual+copied,reference+copied,offset-copied)) return false;
            const auto value=pair(reference+offset);
            if (value>imageBytes || base>UINT64_MAX-value || pair(actual+offset)!=base+value) return false;
            copied=offset+8;
        }
        return equal(actual+copied,reference+copied,bytes-copied);
    }
public:
    bool prepare(const void *data,unsigned bytes) {
        pin=nullptr;if (!data || bytes<40 || bytes>0x100000) return false;
        const auto *p=static_cast<const uint8_t *>(data);
        if (word(p)!=0x31495046 || word(p+4)!=1 || word(p+36)) return false;
        imageBytes=word(p+8);codeRVA=word(p+12);codeBytes=word(p+16);
        protocolRVA=word(p+20);protocolBytes=word(p+24);count=word(p+28);
        if (word(p+32) || !imageBytes || imageBytes>0x100000 || !range(codeRVA,codeBytes) ||
            !range(protocolRVA,protocolBytes) || protocolRVA<codeRVA+codeBytes || count>4096 ||
            uint64_t(40)+4*count+codeBytes+protocolBytes!=bytes) return false;
        relocations=p+40;code=relocations+4*count;protocol=code+codeBytes;
        uint32_t previous=0;
        for (unsigned i=0;i<count;++i) {
            const auto rva=word(relocations+4*i);
            if ((rva&7) || (i && rva<previous+8) ||
                !((rva>=codeRVA && rva-codeRVA<=codeBytes && 8<=codeBytes-(rva-codeRVA)) ||
                  (rva>=protocolRVA && rva-protocolRVA<=protocolBytes && 8<=protocolBytes-(rva-protocolRVA)))) return false;
            previous=rva;
        }
        pin=p;return true;
    }
    uint32_t interfaceRVA() const { return protocolRVA; }
    uint32_t interfaceBytes() const { return protocolBytes; }
    uint32_t textRVA() const { return codeRVA; }
    uint32_t textBytes() const { return codeBytes; }
    uint32_t size() const { return imageBytes; }
    bool matches(const void *base,uint64_t bytes,const Memory &memory) const {
        if (!pin || bytes!=imageBytes || !memory.contains(base,bytes)) return false;
        const auto *p=static_cast<const uint8_t *>(base);const auto address=reinterpret_cast<uintptr_t>(base);
        return segment(p+codeRVA,code,codeRVA,codeBytes,address) &&
            segment(p+protocolRVA,protocol,protocolRVA,protocolBytes,address);
    }
};
inline bool handles(EFI_BOOT_SERVICES *services,EFI_GUID guid,EFI_HANDLE *out,unsigned capacity,unsigned &count) {
    count=0;if (!services || !services->LocateHandle || !services->HandleProtocol) return false;
    UINTN bytes=capacity*sizeof(EFI_HANDLE);
    if (EFI_ERROR(services->LocateHandle(ByProtocol,&guid,nullptr,&bytes,out)) ||
        !bytes || bytes>capacity*sizeof(EFI_HANDLE) || bytes%sizeof(EFI_HANDLE)) return false;
    count=unsigned(bytes/sizeof(EFI_HANDLE));
    for (unsigned i=0;i<count;++i) {
        if (!out[i]) return false;
        for (unsigned j=0;j<i;++j) if (out[j]==out[i]) return false;
    }
    return true;
}
inline const uint8_t *loadedImage(EFI_BOOT_SERVICES *services,void *interface,const CodePin &pin,const Memory &memory,
                                 uint32_t interfaceRVA=0) {
    if (!interfaceRVA) interfaceRVA=pin.interfaceRVA();
    EFI_HANDLE list[256]{};unsigned count=0;
    EFI_GUID guid=EFI_LOADED_IMAGE_PROTOCOL_GUID;
    if (!handles(services,guid,list,256,count)) return nullptr;
    const uint8_t *found=nullptr;
    for (unsigned i=0;i<count;++i) {
        EFI_LOADED_IMAGE_PROTOCOL *image=nullptr;
        if (EFI_ERROR(services->HandleProtocol(list[i],&guid,reinterpret_cast<void **>(&image))) ||
            !memory.contains(image,sizeof(*image))) return nullptr;
        const auto base=reinterpret_cast<uintptr_t>(image->ImageBase);
        if (!base || interfaceRVA>=pin.size() || base>UINT64_MAX-interfaceRVA ||
            base+interfaceRVA!=reinterpret_cast<uintptr_t>(interface)) continue;
        if (found || image->Revision!=EFI_LOADED_IMAGE_PROTOCOL_REVISION ||
            image->ImageCodeType!=EfiBootServicesCode || image->ImageDataType!=EfiBootServicesData ||
            !pin.matches(image->ImageBase,image->ImageSize,memory)) return nullptr;
        found=static_cast<const uint8_t *>(image->ImageBase);
    }
    return found;
}
}
