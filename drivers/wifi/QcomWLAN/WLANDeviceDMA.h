// SPDX-License-Identifier: BSD-2-Clause
#pragma once
#include "WLANIODMA.h"
#include "WLANCopyEngine.h"
namespace WLAN {
// Kernel allocation backend for all host CE rings. The platform holds this
// object until destroy() succeeds, including any failed native unmap. The
// mapper belongs to WLAN's ANOC2 streams; there is no system/bypass fallback.
class DeviceDMA {
    IOMapper *mapper;
    IOMemoryAPI api;
    DMABuffer<IOMemoryAPI> *rings[CopyEngineCount][2]{};
    DeviceDMA(IOMapper *,Result (*)(void *,uint64_t),void *);
    ~DeviceDMA() = default;
public:
    static DeviceDMA *create(IOMapper *,Result (*)(void *,uint64_t),void *);
    DeviceDMA(const DeviceDMA &) = delete;
    DeviceDMA &operator=(const DeviceDMA &) = delete;
    Result prepare();
    bool ownsMemory() const;
    DMABuffer<IOMemoryAPI> *ring(unsigned engine,bool receive) const;
    Result release();
    Result destroy(); // deletes this only after complete release
};
}
