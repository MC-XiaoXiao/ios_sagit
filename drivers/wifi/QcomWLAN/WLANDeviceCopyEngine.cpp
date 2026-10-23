// SPDX-License-Identifier: BSD-2-Clause
#include "WLANDeviceCopyEngine.h"
#include "WLANCopyEngineIO.h"
namespace WLAN {
template<unsigned Count,bool Receive> class Ring final : public DeviceCopyEngine {
    CopyEngineIO<RegisterIO,Count,Receive> engine;
    DMABuffer<IOMemoryAPI> &buffer;
    unsigned index;
public:
    Ring(RegisterIO &io,DMABuffer<IOMemoryAPI> &memory,unsigned id) : engine(io),buffer(memory),index(id) {}
    Result configure(uint64_t epoch) override { return engine.configure(index,epoch,buffer,Pipes[index].maximumBytes); }
    Result post(uint64_t epoch,uint64_t cookie,uint64_t address,uint32_t bytes,uint16_t metadata) override {
        return engine.post(epoch,cookie,address,bytes,metadata);
    }
    Result completion(uint64_t epoch,uint64_t &cookie,uint32_t &bytes,uint16_t &metadata) override {
        return engine.completion(epoch,cookie,bytes,metadata);
    }
    Result shutdown(uint64_t epoch) override { return engine.shutdown(epoch); }
    bool ownsHardware() const override { return engine.ownsHardware(); }
};
DeviceCopyEngine *DeviceCopyEngine::create(RegisterIO &io,DeviceDMA &dma,unsigned index,bool receive) {
    if (!io.valid() || index >= CopyEngineCount || index == 6) return nullptr;
    auto *buffer = dma.ring(index,receive);
    if (!buffer) return nullptr;
    const auto count = receive ? Pipes[index].destinationEntries : Pipes[index].sourceEntries;
    if (receive) {
        switch (count) {
        case 2: return new Ring<2,true>(io,*buffer,index);
        case 64: return new Ring<64,true>(io,*buffer,index);
        case 128: return new Ring<128,true>(io,*buffer,index);
        case 512: return new Ring<512,true>(io,*buffer,index);
        default: return nullptr;
        }
    }
    switch (count) {
    case 2: return new Ring<2,false>(io,*buffer,index);
    case 16: return new Ring<16,false>(io,*buffer,index);
    case 32: return new Ring<32,false>(io,*buffer,index);
    case 2048: return new Ring<2048,false>(io,*buffer,index);
    default: return nullptr;
    }
}
Result DeviceCopyEngine::destroy() {
    if (ownsHardware()) return Result::Busy;
    delete this; return Result::Success;
}
}
