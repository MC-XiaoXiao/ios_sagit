// SPDX-License-Identifier: BSD-2-Clause
#pragma once
#include <GPUNativeMemory.h>

namespace GPU { namespace MetalCapabilities {
constexpr uint32_t Version=1,Selector=0x1007;
enum : uint32_t {
    Ready=1,AddressSpaces=2,ImmutableBlit=4,MixedBatch=8,Pipeline=16,
    Notifications=32,SurfaceFences=64,Diagnostics=128,ShaderCapture=256,Timing=512,GMEM=1024
};
// One native-connection snapshot, independent of IORegistry property filtering.
// All versions and limits describe this connection's actual kernel backend.
struct Snapshot {
    uint32_t version=Version,bytes=sizeof(Snapshot),command=0,completion=0;
    uint32_t shader=0,jit=0,blit=0,maxJobs=0,backpressure=0,maxShmem=0,batch=0,flags=0;
    uint64_t registry=0;
    Native::Memory::Limits memory{};
};
static_assert(sizeof(Snapshot)==120,"native Metal capability snapshot ABI");
} }
