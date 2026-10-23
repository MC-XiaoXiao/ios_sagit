// SPDX-License-Identifier: BSD-2-Clause
#pragma once
#include <IOKit/IOBufferMemoryDescriptor.h>
#include <IOKit/IODMACommand.h>
#include <IOKit/IOMapper.h>
#include "GPUMemoryRanges.h"
class QcomGPUBuffer : public OSObject {
    OSDeclareDefaultStructors(QcomGPUBuffer);
    IOMemoryDescriptor *fMemory = nullptr;
    void *fBytes = nullptr;
    IODMACommand *fDMA = nullptr;
    IOMapper *fMapper = nullptr;
    QcomGPUBuffer *fParent = nullptr;
    uint64_t fOffset = 0;
    using Page = GPU::BackingPage;
    Page *fPages = nullptr;
    uint16_t *fLogicalPages = nullptr;
    uint32_t fPageCount = 0;
    bool fShared = false;
    bool fPrepared = false,fDMAPrepared = false,fDMAAttempted = false;
    bool mapDMA(IOMapper *);
    bool indexPages();
    const QcomGPUBuffer *root() const { return fParent ? fParent : this; }
public:
    uint64_t address = 0, length = 0;
    uint32_t pending = 0;
    static QcomGPUBuffer *allocate(uint64_t, IOMapper *);
    static QcomGPUBuffer *mapDescriptor(IOMemoryDescriptor *, IOMapper *, bool shared = false);
    static QcomGPUBuffer *view(QcomGPUBuffer *, uint64_t offset, uint64_t bytes=0);
    void *bytes() const { return fBytes; }
    bool copyBytes(uint64_t offset, void *destination, uint64_t bytes) const;
    bool synchronize() const;
    bool gpuWritable() const;
    bool overlaps(const QcomGPUBuffer *, uint64_t, uint64_t, uint64_t, uint64_t) const;
    bool overlapsRanges(const QcomGPUBuffer *,GPU::StridedRange,GPU::StridedRange) const;
    void beginGPUUse() { ++pending; if (fParent) ++fParent->pending; }
    void endGPUUse() { if (pending) --pending; if (fParent && fParent->pending) --fParent->pending; }
    uint32_t pendingGPU() const { return root()->pending; }
    const QcomGPUBuffer *backing() const { return root(); }
    bool mappedBy(const IOMapper *mapper) const {
        const auto *buffer=root();
        return buffer->fMapper==mapper && buffer->fPrepared && buffer->fDMAPrepared;
    }
    bool physicalPage(unsigned,uint64_t &) const;
    void free() APPLE_KEXT_OVERRIDE;
};
