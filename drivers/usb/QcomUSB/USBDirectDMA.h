// SPDX-License-Identifier: BSD-2-Clause
#pragma once
#include "USBProtocol.h"

namespace USB {
constexpr const char *DirectDMAProperty = "qcom,usb-dma-window";
// Explicit capture policy:1 for an architected BYPASS route,2 for an
// existing exclusive disabled stage1/S2-bypass bank,3 for no matched stream
// with actual USFCFG=0. None creates a route or changes global policy.
constexpr const char *DirectDMACaptureProperty = "qcom,usb-dma-capture";
// Explicit board bring-up trial of the original UsbfnDwc3Dxe physical-buffer
// ABI. This is not evidence of an architected SMMU bypass. It is restricted to
// 32-bit uncached allocations and a retained, completely cold guest SMMU.
// Actual device transfers must validate the firmware's physical DMA route.
constexpr const char *FirmwarePhysical32Property = "qcom,usb-firmware-physical32";
// Trusted topology, not a readiness flag. The resource owner must establish that
// USB bus addresses refer to these system physical addresses without a live
// translator. Absence of iommu-parent alone does not establish this mapping.
// Wire representation: little-endian version/size, bus/physical base, length.
struct DirectDMAWindow {
    uint32_t version=0,size=0;
    uint64_t busBase=0,physicalBase=0,length=0;
    static bool decode(const void *bytes,unsigned count,DirectDMAWindow &out) {
        if (!bytes || count!=32) return false;
        const auto *p=static_cast<const uint8_t *>(bytes);
        auto word=[&](unsigned offset) {
            uint64_t value=0;
            for (unsigned i=0;i<8;++i) value|=uint64_t(p[offset+i])<<(8*i);
            return value;
        };
        const auto header=word(0);
        out={uint32_t(header),uint32_t(header>>32),word(8),word(16),word(24)};
        return true;
    }
    bool valid(unsigned bits) const {
        // The existing physical allocator constrains by address width. Reject
        // offsets, holes and subwindows until an allocator can enforce them.
        return version==1 && size==32 && bits>=32 && bits<=40 &&
            !busBase && !physicalBase && length==(uint64_t(1)<<bits);
    }
    bool same(const DirectDMAWindow &other) const {
        return version==other.version && size==other.size && busBase==other.busBase &&
            physicalBase==other.physicalBase && length==other.length;
    }
};
static_assert(sizeof(DirectDMAWindow)==32,"direct DMA topology ABI");

// Backend reads the scoped provider's current topology, rejects any mapper,
// checks a retained NoC grant before inspecting any live route, and reads the
// actual controller BCR. All calls run under the resource owner's lock. No
// global SMMU/bypass register is set. Legacy explicit windows have no observer;
// captured windows keep their exact hardware route pinned and rechecked.
template<class Backend> class DirectDMA {
    Backend &backend;
    DirectDMAWindow window{};
    bool held=false,fault=false;
    Result route() {
        const auto r=backend.dmaAccessible();
        return r==Result::Success ? backend.dmaRoute():r;
    }
    Result description() {
        DirectDMAWindow current{};
        const auto r=backend.readDMAWindow(current);
        return r!=Result::Success ? r : current.same(window) ? Result::Success : Result::HardwareError;
    }
public:
    explicit DirectDMA(Backend &operations):backend(operations) {}
    bool owned() const { return held; }
    bool faulted() const { return fault; }
    Result acquire(unsigned bits) {
        if (held) return Result::Busy;
        DirectDMAWindow current{};
        auto r=backend.readDMAWindow(current);if (r!=Result::Success) return r;
        if (!current.valid(bits)) return Result::Unsupported;
        r=route();if (r!=Result::Success) return r;
        r=backend.dmaIsolated();if (r!=Result::Success) return r;
        window=current;
        r=description();if (r!=Result::Success) return r;
        r=route();if (r!=Result::Success) return r;
        held=true;fault=false;return Result::Success;
    }
    Result check() {
        if (!held) return Result::NotReady;
        if (fault) return Result::HardwareError;
        auto r=description();
        if (r==Result::Success) r=route();
        if (r!=Result::Success) fault=true;
        return r;
    }
    Result release() {
        if (!held) return Result::Success;
        // Retry after a fault only with fresh reset/fabric/topology proof.
        // Failure keeps the original grant and its dependent resources held.
        auto r=route();if (r!=Result::Success) return r;
        r=backend.dmaIsolated();if (r!=Result::Success) return r;
        r=description();if (r!=Result::Success) return r;
        held=false;fault=false;window={};return Result::Success;
    }
};
}
