/* SPDX-License-Identifier: BSD-2-Clause */
#ifndef _QCOM_DISPLAY_PROTOCOL_H
#define _QCOM_DISPLAY_PROTOCOL_H
#include <stdint.h>
#include <stddef.h>
#include <DisplayFrameStatistics.h>

namespace Display {
enum : uint32_t {
    // Offsets from MDSS MDP (0x0c901000), not the MDSS top-level base.
    InterruptEnable = 0x10, InterruptStatus = 0x14, InterruptClear = 0x18,
    BGRA = 0x42475241, RGBA = 0x52474241,
    MaxDimension = 4096
};
struct Mode {
    uint32_t width, height, rowBytes, refreshHz;
    uint64_t framebufferAddress, framebufferSize;
    bool valid() const {
        return width && height && width <= MaxDimension && height <= MaxDimension &&
            refreshHz >= 24 && refreshHz <= 120 && rowBytes >= uint64_t(width) * 4 &&
            rowBytes <= 65536 && !(rowBytes & 3) && !(framebufferAddress & 4095) &&
            framebufferAddress && framebufferAddress < (1ULL << 40) &&
            framebufferSize >= uint64_t(rowBytes) * height &&
            framebufferSize <= (1ULL << 28) &&
            framebufferSize <= (1ULL << 40) - framebufferAddress;
    }
};
struct Surface {
    uint64_t width, height, rowBytes, allocation, descriptorSize;
    uint32_t format;
    bool tiled = false;
    uint32_t tileFormat = 0;
    bool valid(const Mode &m) const {
        // A compositor must render a single, full-canvas linear BGRA surface.
        // IOSurface tiling flags are checked independently of its pixel format.
        return width == m.width && height == m.height && format == BGRA &&
            !tiled && !tileFormat &&
            rowBytes >= width * 4 && rowBytes <= 65536 && !(rowBytes & 3) &&
            allocation >= rowBytes * height && descriptorSize >= rowBytes * height;
    }
};
inline bool singleSurfaceUpdate(uint32_t changed, uint32_t surfaces) {
    // Native iOS 18 bits 0..3 update surfaces, including null clears. Bit 31
    // updates the packed background color and is ready without a surface.
    // Require a new primary surface; full-canvas geometry is checked separately.
    return !(changed & ~0x8000000fu) && (changed & 1) && surfaces == 1;
}
enum class SwapContent { Unsupported, Surface, Black };
inline SwapContent nativeSwapContent(uint32_t intent, uint32_t changed,
        uint32_t surfaces, uint32_t background) {
    // Native cached transactions use -1 for all changed/ready fields. Keep
    // that sentinel confined to the two audited internal intents (6 and 7).
    if (intent == 0)
        return singleSurfaceUpdate(changed, surfaces) ? SwapContent::Surface : SwapContent::Unsupported;
    if (intent != 6 && intent != 7) return SwapContent::Unsupported;
    uint32_t mask = changed == UINT32_MAX ? 0x8000000fu : changed;
    if (intent == 6)
        return singleSurfaceUpdate(mask, surfaces) ? SwapContent::Surface : SwapContent::Unsupported;
    // Clear every retained plane and explicitly replace the background.
    // Other colors/intents require their own native state interpretation.
    return mask == 0x8000000fu && !surfaces && !background ? SwapContent::Black : SwapContent::Unsupported;
}
struct PlatformContract {
    uint32_t version, flags, interface, control, source, interruptBit;
    uint32_t pingPong = 0;
    enum : uint32_t { ClocksPinned = 1, PowerPinned = 2, LinkReady = 4,
        FirmwareQuiesced = 8, ScanoutReserved = 16, DoubleBufferMapped = 32, CommandMode = 64 };
    bool commandMode() const { return flags & CommandMode; }
    uint32_t ownedInterrupts() const {
        return (1u << interruptBit) | (commandMode() ? 1u << (8 + pingPong) : 0);
    }
    bool valid() const {
        // Only a single DSI interface, CTL0 and VIG0/DMA0 are
        // admitted. Other topologies require their own routing implementation.
        return version == 1 && (flags & 31) == 31 && !(flags & ~127u) && interface >= 1 && interface <= 2 &&
            control == 0 && (source == 0 || source == 8) &&
            pingPong < 4 && interruptBit == (commandMode() ? 12 + pingPong : 25 + 2 * interface);
    }
};
inline uint64_t bufferSpan(const Mode &mode) {
    return (uint64_t(mode.rowBytes) * mode.height + 4095) & ~uint64_t(4095);
}
inline bool mappedRaster(const Mode &mode, uint64_t address, uint32_t rowBytes, uint64_t mappedBytes) {
    // A single display IOMapper segment, including row padding, must be
    // addressable by the 32-bit SSPP fetcher. GPU IOVAs are never an input.
    return mode.width && mode.height && address && address <= UINT32_MAX && !(address & 63) &&
        rowBytes >= uint64_t(mode.width) * 4 && rowBytes <= 65535 && !(rowBytes & 63) &&
        mappedBytes && uint64_t(rowBytes) * mode.height <= mappedBytes &&
        mappedBytes <= (1ULL << 32) - address;
}
struct NativeSwapLinks {
    void *next;
    void **previous;
};
inline bool detachNativeSwapHead(void *&head, void **&tail, void *request,
        NativeSwapLinks &links, NativeSwapLinks *nextLinks) {
    // Original iOS 18 TAILQ ownership: the active slot takes the queue's
    // reference only after both forward/back links have been detached.
    if (!request || head != request || links.previous != &head || *links.previous != request ||
            bool(links.next) != bool(nextLinks) ||
            (nextLinks ? nextLinks->previous != &links.next : tail != &links.next)) return false;
    if (nextLinks) nextLinks->previous = links.previous;
    else tail = links.previous;
    head = links.next;
    return true;
}
inline bool linearBGRA(uint32_t format, uint32_t unpack, uint32_t operation) {
    // Tight interleaved B,G,R,A/X, four 8-bit components; no UBWC, rotation,
    // scaling, planar fetch or solid-fill interpretation of the backing bytes.
    return (format & ~0x100u) == 0x236ff && unpack == 0x03020001 &&
        !(operation & ((1u << 0) | (1u << 13) | (1u << 14) | (1u << 22)));
}
template <class Registers> class Scanout {
    Registers &r;
    uint32_t source, flush;
public:
    Scanout(Registers &registers, unsigned pipe) : r(registers),
        source(pipe == 0 ? 0x4000 : 0x24000), flush(pipe == 0 ? 1 : 1u << 11) {}
    bool stage(uint64_t address, uint32_t rowBytes = 0) {
        if (!address || address > UINT32_MAX || (address & 63) ||
            (rowBytes && (rowBytes > 65535 || (rowBytes & 3))) || (r.read(0x1018) & flush)) return false;
        r.write(source + 0x14, uint32_t(address));
        if (rowBytes) r.write(source + 0x24, (r.read(source + 0x24) & 0xffff0000u) | rowBytes);
        r.write(0x1018, flush); // CTL0: latch the source shadow registers on VBL.
        return true;
    }
    bool startCommand() {
        if (r.read(0x101c) & 1) return false;
        r.write(0x1018, flush | (1u << 17));
        r.write(0x101c, 1);
        return true;
    }
    bool latched(uint64_t address, uint32_t rowBytes = 0) {
        return !(r.read(0x1018) & flush) && r.read(source + 0x14) == address &&
            (!rowBytes || (r.read(source + 0x24) & 0xffff) == rowBytes);
    }
};
template <class Registers> class Interrupts {
    Registers &r;
    uint32_t owned, saved;
public:
    Interrupts(Registers &registers, unsigned bit) : r(registers), owned(1u << bit), saved(0) {}
    void enable() {
        saved = r.read(InterruptEnable);
        r.write(InterruptClear, owned);
        r.write(InterruptEnable, saved | owned);
    }
    bool acknowledge() {
        if (!(r.read(InterruptStatus) & owned)) return false;
        r.write(InterruptClear, owned);
        return true;
    }
    void disable() {
        uint32_t current = r.read(InterruptEnable);
        r.write(InterruptEnable, (current & ~owned) | (saved & owned));
        r.write(InterruptClear, owned);
    }
};
}
#endif
