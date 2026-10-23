// SPDX-License-Identifier: GPL-2.0-only
#pragma once
#include <IOKit/IOService.h>
class QcomGPU;
class IOMemoryMap;
class QcomGPUNoC {
    struct State;
    State *fState = nullptr;

  public:
    static QcomGPUNoC *create(QcomGPU *, IOService *, IOMemoryMap *);
    ~QcomGPUNoC(); // only after shutdown() proves no owned state remains
    IOReturn acquire();
    bool healthy();
    bool shutdown();
};
