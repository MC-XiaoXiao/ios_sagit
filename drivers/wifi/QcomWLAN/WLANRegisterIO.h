// SPDX-License-Identifier: BSD-2-Clause
#pragma once
#include <IOKit/IOMemoryDescriptor.h>
#include <IOKit/IOService.h>
#include <WLANProtocol.h>
namespace WLAN {
class RegisterIO {
    IOMemoryMap *map;
    void *owner;
    bool (*accessible)(void *);
    Result (*isolation)(void *,uint64_t);
public:
    RegisterIO(IOMemoryMap *memory,void *context,bool (*access)(void *),Result (*isolated)(void *,uint64_t)) :
        map(memory),owner(context),accessible(access),isolation(isolated) { if (map) map->retain(); }
    ~RegisterIO() { if (map) map->release(); }
    RegisterIO(const RegisterIO &) = delete;
    RegisterIO &operator=(const RegisterIO &) = delete;
    bool valid() const {
        return map && owner && accessible && isolation && map->getPhysicalAddress() == RegisterBase &&
            map->getLength() == RegisterSize && map->getVirtualAddress();
    }
    uint32_t read(uint32_t offset) {
        if (!valid() || !accessible(owner) || (offset & 3) || offset > RegisterSize - 4) return UINT32_MAX;
        auto *base = reinterpret_cast<volatile uint32_t *>(map->getVirtualAddress());
        const auto value = base[offset / 4]; __asm__ volatile("dmb oshld" ::: "memory"); return value;
    }
    void write(uint32_t offset,uint32_t value) {
        if (!valid() || !accessible(owner) || (offset & 3) || offset > RegisterSize - 4) return;
        auto *base = reinterpret_cast<volatile uint32_t *>(map->getVirtualAddress());
        __asm__ volatile("dmb oshst" ::: "memory"); base[offset / 4] = value;
    }
    Result isolated(uint64_t epoch) { return valid() && accessible(owner) ? isolation(owner,epoch) : Result::NotReady; }
    void publishBarrier() { __asm__ volatile("dsb oshst" ::: "memory"); }
    void consumeBarrier() { __asm__ volatile("dsb oshld" ::: "memory"); }
};
}
