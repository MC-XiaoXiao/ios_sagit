// SPDX-License-Identifier: GPL-2.0-only
#pragma once
#include "BasebandRMTFSStorage.h"
#include "BasebandSecureIO.h"

namespace Baseband {
Result readRMTFSPolicy(RMTFS::StoragePolicy &, uint64_t epoch);
// IOMedia and one prepared DMA buffer stay retained through every completed
// synchronous operation. The caller serializes this object on its work loop.
// All four original NV partitions require an exact explicit boot policy.
class NativeRMTFSStorage {
    struct Context;
    Context *context = nullptr;
public:
    NativeRMTFSStorage() = default;
    NativeRMTFSStorage(const NativeRMTFSStorage &) = delete;
    NativeRMTFSStorage &operator=(const NativeRMTFSStorage &) = delete;
    ~NativeRMTFSStorage();
    Result bind(IOService *, const RMTFS::StoragePolicy &, const uint32_t *abort);
    Result acquireShared(NativeAssignment &); // MSS reset/resource lease held
    Result retireShared(bool remoteStopped); // after AXI drain/reset only
    bool close();
    void quarantine();
    bool quarantined() const;
    bool ready() const;
    Result open(unsigned part);
    Result close(unsigned part);
    Result size(unsigned part, uint64_t &bytes);
    Result read(unsigned part, uint64_t offset, void *buffer, size_t bytes);
    Result write(unsigned part, uint64_t offset, const void *buffer, size_t bytes);
    Result sync(unsigned part);
    Result readShared(uint64_t physical, void *, size_t);
    Result writeShared(uint64_t physical, const void *, size_t);
};
} // namespace Baseband
