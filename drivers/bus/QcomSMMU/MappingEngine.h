// SPDX-License-Identifier: BSD-2-Clause
#pragma once
#include "MappingLedger.h"
namespace SMMU {
// References retains the actual descriptor/command, not a Family request tag.
// The adapter serializes this engine with context handoff and all callbacks.
template<class Tables,class Context,class References,unsigned Capacity=512> class MappingEngine {
    Tables &tables;
    Context &context;
    References &references;
    MappingLedger<Capacity> ledger;
    bool running=false,fault=false;
    Status unhealthy(Status status) { fault=true;return status==TimedOut || status==Failed ? status : Stale; }
public:
    using Record=typename MappingLedger<Capacity>::Record;
    using Phase=typename MappingLedger<Capacity>::Phase;
    MappingEngine(Tables &t,Context &c,References &r):tables(t),context(c),references(r) {}
    Status initialize(unsigned bits) { return ledger.initialize(bits); }
    bool faulted() const { return fault || tables.faulted(); }
    unsigned count() const { return ledger.count(); }
    Status begin() {
        if (!ledger.initialized()) return NotReady;
        if (ledger.count() || tables.mappedPages() || tables.faulted() || !tables.root() || context.rootAddress()!=tables.root()) return Busy;
        const auto status=context.check();if (status!=OK) return status;
        running=true;fault=false;return OK;
    }
    template<class Source> Status map(uintptr_t memory,uintptr_t command,uint64_t length,unsigned pageOffset,
                                     uint64_t alignment,unsigned bits,unsigned access,Source &source,Record &result) {
        result={};
        if (!running) return NotReady;
        if (faulted()) return Stale;
        if (!access || (access&~3u)) return Invalid;
        auto status=context.check();if (status!=OK) return unhealthy(status);
        Record *record=nullptr;
        status=ledger.reserve(memory,command,length,pageOffset,alignment,bits,record);
        if (status!=OK) return status;
        references.retain(memory,command);
        status=tables.map(record->address&~4095ULL,unsigned(record->length/4096),source,access);
        if (status!=OK) {
            if (tables.faulted()) { ledger.quarantine(record->identity);result=*record;return unhealthy(status); }
            const auto identity=record->identity;
            ledger.cancel(identity);references.release(memory,command);return status;
        }
        const auto mapped=ledger.mapped(record->identity);
        if (mapped!=OK) { ledger.quarantine(record->identity);result=*record;return unhealthy(mapped); }
        result=*record;return OK;
    }
    Status unmap(uintptr_t memory,uintptr_t command,uint64_t address,uint64_t length) {
        const auto *record=ledger.lookup(memory,command,address,length);
        if (!record) return Invalid;
        if (record->phase!=Phase::Mapped) return Busy;
        const auto identity=record->identity;
        auto status=ledger.beginUnmap(identity,memory,command,address,length);
        if (status!=OK) return status;
        status=tables.unmap(address&~4095ULL,unsigned(length/4096));
        const auto retired=ledger.finishUnmap(identity,status);
        if (retired!=OK) return unhealthy(retired);
        references.release(memory,command);return OK;
    }
    // Root removal is independent of command completion. The native command
    // may already have consumed its mapping metadata on a failed complete().
    Status isolate() {
        const auto status=ledger.isolate([&] {return context.rootDetached();});
        if (status==OK) running=false;
        return status;
    }
    bool retired(uintptr_t memory,uintptr_t command,uint64_t address,uint64_t length) const {
        const auto *record=ledger.lookup(memory,command,address,length);
        return record && record->phase==Phase::Retired;
    }
    Status acknowledge(uint64_t identity,uintptr_t memory,uintptr_t command) {
        const auto status=ledger.acknowledge(identity,memory,command);
        if (status==OK) references.release(memory,command);
        return status;
    }
};
}
