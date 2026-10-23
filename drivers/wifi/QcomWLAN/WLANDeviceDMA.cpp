// SPDX-License-Identifier: BSD-2-Clause
#include "WLANDeviceDMA.h"
namespace WLAN {
DeviceDMA::DeviceDMA(IOMapper *deviceMapper,Result (*isolation)(void *,uint64_t),void *owner) :
    mapper(deviceMapper),api(deviceMapper,isolation,owner) { mapper->retain(); }
DeviceDMA *DeviceDMA::create(IOMapper *mapper,Result (*isolation)(void *,uint64_t),void *owner) {
    if (!mapper || !isolation || !owner || mapper->getPageSize() != 4096) return nullptr;
    return new DeviceDMA(mapper,isolation,owner);
}
Result DeviceDMA::prepare() {
    for (unsigned engine = 0; engine < CopyEngineCount; ++engine)
        for (unsigned direction = 0; direction < 2; ++direction)
            if (rings[engine][direction]) return Result::Busy;
    for (unsigned engine = 0; engine < CopyEngineCount; ++engine) {
        for (unsigned receive = 0; receive < 2; ++receive) {
            const unsigned entries = receive ? Pipes[engine].destinationEntries : Pipes[engine].sourceEntries;
            if (!entries) continue;
            auto *&buffer = rings[engine][receive];
            buffer = new DMABuffer<IOMemoryAPI>(api);
            const auto result = buffer ? buffer->allocate(entries * sizeof(CEDescriptor),64) : Result::NotReady;
            if (result != Result::Success) {
                const auto cleanup = release();
                return cleanup == Result::Success ? result : cleanup;
            }
        }
    }
    return Result::Success;
}
bool DeviceDMA::ownsMemory() const {
    for (const auto &pair : rings) for (const auto *buffer : pair)
        if (buffer && buffer->ownsMemory()) return true;
    return false;
}
DMABuffer<IOMemoryAPI> *DeviceDMA::ring(unsigned engine,bool receive) const {
    return engine < CopyEngineCount ? rings[engine][receive ? 1 : 0] : nullptr;
}
Result DeviceDMA::release() {
    for (unsigned engine = CopyEngineCount; engine; --engine) {
        for (unsigned direction = 2; direction; --direction) {
            auto *&buffer = rings[engine - 1][direction - 1];
            if (!buffer) continue;
            const auto result = buffer->release();
            if (result != Result::Success) return result;
            delete buffer; buffer = nullptr;
        }
    }
    return Result::Success;
}
Result DeviceDMA::destroy() {
    const auto result = release();
    if (result != Result::Success) return result;
    mapper->release(); mapper = nullptr; delete this;
    return Result::Success;
}
}
