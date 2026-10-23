// SPDX-License-Identifier: BSD-2-Clause
#pragma once
#include <GPUUserProtocol.h>
namespace GPU { namespace Surface {
constexpr uint32_t Version = 2, Linear = 1;
constexpr uint32_t RGBA = 0x52474241, BGRA = 0x42475241;
// CoreVideo's native single-plane OneComponent8 / TwoComponent8 formats.
constexpr uint32_t R8 = 0x4c303038, RG8 = 0x32433038;
// CoreUI's gray/alpha surface stores the same two bytes as an RG8 texture.
// Preserve its native FourCC in metadata; the shader interprets gray/alpha.
constexpr uint32_t LA08 = 0x4c413038;
inline uint32_t elementBytes(uint32_t format) {return format==R8?1u:format==RG8 || format==LA08?2u:format==RGBA || format==BGRA?4u:0u;}
inline uint32_t colorPixelFormat(uint32_t bytes,bool bgra=false) {return bytes==1?R8:bytes==2?RG8:bytes==4?(bgra?BGRA:RGBA):0;}
inline bool compatibleColorPixelFormat(uint32_t format,uint32_t bytes,bool bgra=false) {
    const auto expected=colorPixelFormat(bytes,bgra);
    return expected && (format==expected || (bytes==2 && format==LA08));
}
constexpr uint32_t MaxDimension = MaxTextureDimension;
// Vendor metadata inside native ResourceData.privateInfo, not an Apple ABI
// extension. All values describe the task-authorized native IOSurface backing.
struct Info {
    uint32_t version, flags, id, plane, width, height, rowBytes, pixelFormat;
    uint64_t offset, allocationBytes, imageBytes;
    uint32_t bytesPerElement, reserved;
};
static_assert(sizeof(Info) == 64, "surface metadata layout");
inline bool valid(const Info &s) {
    return s.version == Version && s.flags == Linear && s.id && !s.plane &&
        s.width && s.height && s.width <= MaxDimension && s.height <= MaxDimension &&
        elementBytes(s.pixelFormat) && s.bytesPerElement == elementBytes(s.pixelFormat) && !s.reserved &&
        !(s.rowBytes & 63) && s.rowBytes <= MaxTextureRowBytes && s.rowBytes >= uint64_t(s.width) * s.bytesPerElement &&
        !(s.offset & 63) && s.allocationBytes && s.allocationBytes <= MaxBufferBytes &&
        s.imageBytes == uint64_t(s.rowBytes) * s.height &&
        range(s.allocationBytes, s.offset, s.imageBytes);
}
// Native IOSurface allocation size is logical and may include a small prefix.
// A descriptor may retain the rounded backing. getDMAMapLength can return its
// logical length before preparation or its native-page span; neither grants
// shader access to padding. The original prepared pages are checked on map.
inline bool descriptorLayout(uint64_t allocation,uint64_t descriptor,uint64_t dmaBytes,
    uint64_t dmaOffset,uint64_t nativePageBytes) {
    const auto allocationSpan=pageSpanBytes(allocation,nativePageBytes),descriptorSpan=pageSpanBytes(descriptor,nativePageBytes);
    return allocationSpan && descriptorSpan && descriptor>=allocation && descriptor<=allocationSpan && !dmaOffset &&
        (dmaBytes==descriptor || dmaBytes==descriptorSpan);
}
// Pinned Family ABI: bytes follows the cache root's saved DMA length;
// mappedBytes is map.length() minus the surface offset. Keep both native
// values, and validate their relation to the logical IOSurface allocation.
inline bool resourceLayout(const Info &s,uint64_t backingBytes,uint64_t mappedBytes,uint64_t nativePageBytes) {
    return valid(s) && mappedBytes<=MaxBufferBytes-s.offset &&
        descriptorLayout(s.allocationBytes,mappedBytes+s.offset,backingBytes,0,nativePageBytes);
}
} }
