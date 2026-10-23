// SPDX-License-Identifier: BSD-2-Clause
#pragma once
#include "AdrenoBackend.h"
#include "AdrenoAddressSpace.h"

namespace Adreno {
// XML registers are DWORD indices; all offsets exported here are bytes.
enum : uint32_t {
    RBBase = 0x0800 * 4, RBControl = 0x0802 * 4, RBRead = 0x0806 * 4, RBWrite = 0x0807 * 4,
    MEControl = 0x0832 * 4, PFPBase = 0x0835 * 4, MEBase = 0x0838 * 4,
    ContextSwitch = 0x083b * 4, CPInterruptStatus = 0x0b18 * 4,
    InterruptClear = 0x0037 * 4, InterruptMask = 0x0038 * 4,
    InterruptStatus = 0x04e1 * 4, Status = 0x04f5 * 4, Status3 = 0x0530 * 4,
    TrustControl = 0xf400 * 4, GPMUReady = 0xa8e0 * 4, GPMUError = 0xa8e1 * 4,
    VBIFHalt = 0x3080 * 4, VBIFHalted = 0x3081 * 4,
    RingDwords = 8192, IBDwords = 16384, MaxIndirectBuffers = 256,
    // A540 CP_PFP_ME_CNTL reset/stopped control state. The A530v1 snapshot's
    // bits 27/28 do not latch on A540; retaining just the low pair also starts
    // an engine before its instruction base is installed. Keep both pairs.
    // This control readback is used together with VBIF's real drain ACK.
    Halt = 0x303,
    RingControl = 12 | (2 << 8) | (1u << 27), // 32 KiB, 32-byte block, no RPTR shadow
    CompletionIRQ = 1u << 20,
    ErrorIRQs = 0x7e | (1u << 9) | (1u << 23) | (1u << 24) | (1u << 25) | (1u << 28),
    OwnedIRQs = CompletionIRQ | ErrorIRQs
};
// A halted, previously initialized CP asserts CP_BUSY/CP_BUSY_IGN_HYST and
// their GPU/HYST aggregate bits even with an empty ring. Check this only
// with both CP stop controls and a real VBIF drain ACK. All individual
// engines/pipelines and GPU_BUSY_IGN_AHB_CP (bit30) must still be quiet.
inline bool haltedPipelineIdle(uint32_t status) {return !(status&~0x800001c1u);}
struct PlatformContract {
    uint32_t version, flags, chip, firmwareMode;
    uint64_t pm4, pfp, reserved;
    enum : uint32_t {
        ClocksPinned = 1, PowerPinned = 2, NoCPinned = 4, ExclusiveMapper = 8,
        FirmwareInitialized = 16, NonSecure = 32, FirmwareQuiesced = 64,
        Required = 127
    };
    bool valid() const {
        return version == 1 && flags == Required && chip == GPU::ChipID && firmwareMode == 1 &&
            !reserved && pm4 >= MinimumVA && pfp >= MinimumVA && !(pm4 & 3) && !(pfp & 3) &&
            pm4 < (uint64_t(1) << AddressBits) && pfp < (uint64_t(1) << AddressBits);
    }
};
static_assert(sizeof(PlatformContract) == 40, "GPU handoff layout");
inline bool deviceRange(uint64_t address, uint64_t bytes) {
    constexpr uint64_t end = uint64_t(1) << AddressBits;
    return bytes && address >= MinimumVA && address < end && bytes <= end - address;
}
inline uint32_t parity(uint32_t value) {
    value ^= value >> 16; value ^= value >> 8; value ^= value >> 4;
    value ^= value >> 2; value ^= value >> 1;
    return (~value) & 1; // odd parity
}
inline uint32_t packet7(uint32_t opcode, uint32_t count) {
    return 0x70000000 | count | (parity(count) << 15) | (opcode << 16) | (parity(opcode) << 23);
}
class Encoder {
    uint32_t *words;
    uint32_t count = 0, capacity,reserved;
    uint32_t available() const {return count<=capacity && reserved<=capacity-count?capacity-count-reserved:0;}
public:
    Encoder(uint32_t *out, uint32_t size,uint32_t tail=0) : words(out), capacity(size),reserved(tail) {}
    struct Checkpoint {uint32_t count,capacity,reserved;};
    Checkpoint checkpoint() const {return {count,capacity,reserved};}
    void rewind(const Checkpoint &point) {count=point.count;capacity=point.capacity;reserved=point.reserved;}
    bool reserveTail(uint32_t n) {if(count>capacity || n>capacity-count)return false;reserved=n;return true;}
    uint32_t size() const { return count; }
    GPU::Result packet(uint32_t opcode, const uint32_t *data, uint32_t n) {
        if (n > 0x3fff || n+1 > available()) return GPU::Result::NoResources;
        words[count++] = packet7(opcode,n);
        for (uint32_t i=0;i<n;++i) words[count++] = data[i];
        return GPU::Result::Success;
    }
    GPU::Result packetData(uint32_t opcode,const uint32_t *prefix,uint32_t prefixWords,
        const uint32_t *data,uint32_t dataWords) {
        // A direct state upload has a small fixed prefix followed by shader
        // words. Copy both into the IB without a shader-sized stack buffer.
        if((prefixWords && !prefix) || (dataWords && !data) || prefixWords>0x3fff ||
            dataWords>0x3fff-prefixWords || prefixWords+dataWords+1>available())return GPU::Result::NoResources;
        words[count++]=packet7(opcode,prefixWords+dataWords);
        for(uint32_t i=0;i<prefixWords;++i)words[count++]=prefix[i];
        for(uint32_t i=0;i<dataWords;++i)words[count++]=data[i];
        return GPU::Result::Success;
    }
    GPU::Result reg(uint32_t address, uint32_t value) {
        if (address > 0x3ffff || available() < 2) return GPU::Result::NoResources;
        words[count++] = 0x40000000 | 1 | (parity(1)<<7) | (address<<8) | (parity(address)<<27);
        words[count++] = value; return GPU::Result::Success;
    }
    GPU::Result regs(uint32_t address, const uint32_t *values, uint32_t n) {
        // A5xx Type4 writes consecutive DWORD register indices in order.
        if (!values || !n || n >= 0x7f || address > 0x40000-n || n+1 > available())
            return GPU::Result::NoResources;
        words[count++] = 0x40000000 | n | (parity(n)<<7) | (address<<8) | (parity(address)<<27);
        for (uint32_t i=0;i<n;++i) words[count++] = values[i];
        return GPU::Result::Success;
    }
    GPU::Result data(const uint32_t *source, uint32_t n, uint32_t &offset,uint32_t alignment=32) {
        if (!source || !n || n > capacity || alignment<32 || (alignment&(alignment-1))) return GPU::Result::NoResources;
        uint32_t start = (capacity-n)&~(alignment-1); // 128-byte shader objects; larger state tables may request more.
        if (start < count || reserved>start-count) return GPU::Result::NoResources;
        for (uint32_t i=0;i<n;++i) words[start+i] = source[i];
        capacity = start; offset = start*4; return GPU::Result::Success;
    }
    GPU::Result fill(uint64_t address, uint32_t bytes, uint32_t value) {
        if ((address & 3) || (bytes & 3) || !deviceRange(address, bytes)) return GPU::Result::Invalid;
        uint32_t n = bytes / 4;
        if (n > 0x3ffd || n + 3 > available()) return GPU::Result::NoResources;
        words[count++] = packet7(0x3d, n + 2); // CP_MEM_WRITE
        words[count++] = uint32_t(address); words[count++] = uint32_t(address >> 32);
        for (uint32_t i = 0; i < n; ++i) words[count++] = value;
        return GPU::Result::Success;
    }
    GPU::Result barrier() {
        if (available() < 2) return GPU::Result::NoResources;
        words[count++] = packet7(0x12, 0); // CP_WAIT_MEM_WRITES
        words[count++] = packet7(0x26, 0); return GPU::Result::Success;
    }
    GPU::Result copy(uint64_t source, uint64_t destination, uint32_t bytes) {
        if ((source & 3) || (destination & 3) || (bytes & 3) ||
            !deviceRange(source, bytes) || !deviceRange(destination, bytes)) return GPU::Result::Invalid;
        // A5xx has CP_MEM_TO_MEM, not A6xx's bulk CP_MEMCPY. One source,
        // 32-bit mode, wait for earlier CP writes before each load/store.
        uint32_t n = bytes / 4;
        if (n > available() / 6) return GPU::Result::NoResources;
        for (uint32_t i = 0; i < n; ++i, source += 4, destination += 4) {
            words[count++] = packet7(0x73, 5); words[count++] = 1u << 30;
            words[count++] = uint32_t(destination); words[count++] = uint32_t(destination >> 32);
            words[count++] = uint32_t(source); words[count++] = uint32_t(source >> 32);
        }
        return GPU::Result::Success;
    }
    GPU::Result pageTable(uint64_t ttbr) {
        // Kernel-only A5xx SMMU transition, never exposed as a vendor opcode.
        // Idle/ME ordering and APRIV follow the Qualcomm A5xx MMU model.
        uint32_t zero=0,one=1,table[]={uint32_t(ttbr),uint32_t(ttbr>>32),0};
        uint32_t invalidate[]={0x40000,1,0};
        if (barrier()!=GPU::Result::Success || packet(0x5f,&zero,1)!=GPU::Result::Success ||
            reg(0x831,1)!=GPU::Result::Success || packet(0x13,nullptr,0)!=GPU::Result::Success ||
            packet(0x53,table,3)!=GPU::Result::Success || packet(0x43,invalidate,3)!=GPU::Result::Success ||
            reg(0x831,0)!=GPU::Result::Success || packet(0x5f,&one,1)!=GPU::Result::Success ||
            packet(0x13,nullptr,0)!=GPU::Result::Success) return GPU::Result::NoResources;
        return GPU::Result::Success;
    }
};
// Trusted kernel-owned IBs, submitted at one ring level under a single private
// page-table context and completion timestamp. Never accepted from user memory.
struct IndirectBuffer {uint64_t address;uint32_t words,reserved;};
static_assert(sizeof(IndirectBuffer)==16,"private IB descriptor");
struct ProgressWatchdog {
    uint64_t observedAt=0;uint32_t sequence=0;bool armed=false;
    GPU::Result poll(uint64_t now,uint32_t value,uint32_t limit,uint64_t timeout) {
        if(value<sequence || value>limit || (armed && now<observedAt))return GPU::Result::Fault;
        if(!armed || value!=sequence){armed=true;sequence=value;observedAt=now;return GPU::Result::Success;}
        return now-observedAt>=timeout?GPU::Result::Timeout:GPU::Result::Success;
    }
};
// Admission slots stay in native timeline order. Select GPU execution order
// independently; completed slots are released only as an admission prefix.
struct AdmissionCandidate {
    const void *owner=nullptr;
    uint32_t queue=0,priority=1;
    bool ready=false,complete=false,issued=false,cancelled=false;
};
inline unsigned priorityRank(uint32_t priority) {
    // iOS18 AGX maps Family classes0/5,4,1,2 to ascending WorkQueue keys
    // 0,1,2,3. nextAdmission uses descending ranks for the same ordering.
    // Family has already downgraded unauthorized realtime requests (class5).
    switch(priority) {case 0:case 5:return 4;case 4:return 3;case 1:return 2;default:return 1;}
}
template<class View> unsigned nextAdmission(unsigned count,const View &view,bool pipeline=false) {
    unsigned selected=count,rank=0;
    for(unsigned i=0;i<count;++i) {
        const auto current=view(i);
        if(!current.ready || current.complete || current.cancelled)continue;
        bool blocked=false;
        for(unsigned j=0;j<i && !blocked;++j) {
            const auto prior=view(j);
            blocked=!prior.complete && !prior.cancelled && (!pipeline || !prior.issued) && prior.owner==current.owner && prior.queue==current.queue;
        }
        if(blocked)continue;
        unsigned candidate=priorityRank(current.priority);
        if(selected==count || candidate>rank){selected=i;rank=candidate;}
    }
    return selected;
}
// A future unissued job cannot hold an earlier job hostage. Earlier jobs and
// all already-issued jobs retain cache ownership until CPU retirement, even
// after their GPU timestamp arrives. The supplied conflict checks physical
// full-root aliases with at least one writer (not just descriptor identity).
template<class View,class Conflict> bool dispatchDependencyBlocked(unsigned candidate,unsigned count,
    const View &view,const Conflict &conflict) {
    if(candidate>=count)return true;
    for(unsigned i=0;i<count;++i)
        if(i!=candidate && !(view(i).cancelled && !view(i).issued) &&
            (i<candidate || view(i).issued) && conflict(candidate,i))return true;
    return false;
}
inline GPU::Result encodeCommands(GPU::Session &session, uint32_t context, const GPU::Command *commands,
    uint32_t count, const uint64_t (&addresses)[GPU::MaxBuffers], Encoder &encoder, uint32_t &destinations) {
    destinations = 0;
    if (!commands || !count || count > GPU::MaxCommands) return GPU::Result::Invalid;
    for (uint32_t i = 0; i < count; ++i) {
        const auto &command = commands[i];
        if (command.reserved) return GPU::Result::Invalid;
        GPU::Result status;
        if (command.opcode == GPU::Barrier) {
            if (command.destination || command.offset || command.bytes || command.value) return GPU::Result::Invalid;
            status = encoder.barrier();
        } else if (command.opcode == GPU::Fill32) {
            auto *resource = session.buffers.find(command.destination);
            if (!resource || resource->parent != context || (command.offset & 3) || (command.bytes & 3) ||
                !GPU::range(resource->bytes, command.offset, command.bytes)) return GPU::Result::Invalid;
            uint32_t slot = resource->handle & 0xff;
            if (!deviceRange(addresses[slot], resource->bytes)) return GPU::Result::Invalid;
            status = encoder.fill(addresses[slot] + command.offset, command.bytes, command.value);
            destinations |= 1u << slot;
        } else return GPU::Result::Unsupported;
        if (status != GPU::Result::Success) return status;
    }
    return GPU::Result::Success;
}

// Shared by the real MMIO driver and the hardware emulator in host tests.
// Registers provides read/write, publish/acquire barriers and bounded delays.
struct EngineStartOptions {
    bool authenticatedZAP = false; // caller has a successful PAS-13 auth/resume
    bool interrupts = true;
};
// Kernel-only receipt for one retained profiling cell. Sequences never wrap
// within an engine epoch; a successful restart invalidates every old receipt.
struct ExecutionToken {uint64_t epoch=0;uint32_t sequence=0,slot=0;};
enum : uint32_t {ExecutionSlots=GPU::MaxJobs,ExecutionBase=64,ExecutionStride=64};
static_assert(ExecutionBase+ExecutionSlots*ExecutionStride<=4096,"profiling fits private fence page");
template <class Registers> class Engine {
    Registers &r;
    volatile uint32_t *ring = nullptr, *fence = nullptr;
    uint64_t fenceVA = 0;
    uint32_t read = 0, write = 0, issued = 0, completed = 0;
    uint64_t executionEpoch=0;
    struct ExecutionRecord {uint32_t sequence=0;bool core=false,busy=false;} executions[ExecutionSlots]{};
    bool owned = false, online = false, broken = false;
    uint64_t read64(uint32_t reg) { return r.read(reg) | (uint64_t(r.read(reg + 4)) << 32); }
    void put(uint32_t value) { ring[write] = value; write = (write + 1) & (RingDwords - 1); }
    void timestamp(uint32_t sequence) {
        put(packet7(0x46, 4)); put(4 | (1u << 31)); // CACHE_FLUSH_TS + IRQ
        put(uint32_t(fenceVA)); put(uint32_t(fenceVA >> 32)); put(sequence);
    }
    void registerTimestamp(uint64_t destination,uint32_t reg) {
        put(packet7(0x3e,3));put(reg|(1u<<30)|(2u<<18));
        put(uint32_t(destination));put(uint32_t(destination>>32));
    }
    void executionTimestamp(uint32_t offset,uint32_t reg=0x4d2) {
        // A540 profiling ABI: two consecutive register DWORDs, 64-bit
        // destination. Both samples run under the kernel tree; user shaders
        // cannot map or forge this fence backing. No counter is enabled here.
        registerTimestamp(fenceVA+offset,reg);
    }
    void executionStamp(uint32_t offset,uint32_t sequence) {
        put(packet7(0x3d,3));put(uint32_t(fenceVA+offset));put(uint32_t((fenceVA+offset)>>32));put(sequence);
    }
    bool executionComplete(const ExecutionToken &token) {
        if(!ready() || !token.epoch || token.epoch!=executionEpoch || !token.sequence ||
            token.slot>=ExecutionSlots || executions[token.slot].sequence!=token.sequence ||
            token.sequence>completed)return false;
        r.acquire();const auto base=(ExecutionBase+token.slot*ExecutionStride)/4;
        return fence[base]==token.sequence && fence[base+1]==token.sequence;
    }
    bool executionSample(const ExecutionToken &token,unsigned word,uint64_t &begin,uint64_t &end,bool allowZero=false) {
        if(!executionComplete(token))return false;
        const auto base=(ExecutionBase+token.slot*ExecutionStride)/4+word;
        begin=fence[base]|(uint64_t(fence[base+1])<<32);end=fence[base+2]|(uint64_t(fence[base+3])<<32);
        return (allowZero || begin) && end>=begin;
    }
    bool refreshRead() {
        uint32_t current = r.read(RBRead);
        if (current >= RingDwords || ((current - read) & (RingDwords - 1)) > ((write - read) & (RingDwords - 1))) {
            broken = true; return false;
        }
        read = current; return true;
    }
public:
    explicit Engine(Registers &registers) : r(registers) {}
    bool ready() const { return online && !broken; }
    bool ownsHardware() const { return owned; }
    uint32_t lastIssued() const { return issued; }
    uint32_t lastCompleted() const { return completed; }
    GPU::Result start(const PlatformContract &p, volatile uint32_t *ringCPU, uint64_t ringVA,
        volatile uint32_t *fenceCPU, uint64_t completionVA, EngineStartOptions options = {}) {
        if (owned || executionEpoch==UINT64_MAX || !p.valid() || !ringCPU || !fenceCPU || (ringVA & 4095) || (completionVA & 4095) ||
            !deviceRange(ringVA, RingDwords * 4) || !deviceRange(completionVA, 4096) ||
            GPU::range(RingDwords * 4, completionVA >= ringVA ? completionVA - ringVA : UINT64_MAX, 1) ||
            GPU::range(4096, ringVA >= completionVA ? ringVA - completionVA : UINT64_MAX, 1))
            return GPU::Result::Invalid;
        // Retained-firmware path: no cold-boot register tables or SCM bypass.
        // A secure CP needs a confirmed PAS-13 operation and a firmware-handled
        // CP_SET_SECURE_MODE transition. Never write the trust register.
        const auto trust = r.read(TrustControl);
        if (read64(MEBase) != p.pm4 || read64(PFPBase) != p.pfp ||
            trust > 1 || (trust && !options.authenticatedZAP) || r.read(GPMUReady) != 0xbabeface || r.read(GPMUError) ||
            r.read(ContextSwitch) || (r.read(MEControl) & Halt) != Halt ||
            (r.read(VBIFHalted)&15)!=15 || !haltedPipelineIdle(r.read(Status)) ||
            (r.read(Status3) & (1u << 24)) || (r.read(InterruptStatus) & ErrorIRQs) ||
            (r.read(CPInterruptStatus)&0x3f) ||
            r.read(RBControl)!=RingControl || read64(RBBase)!=ringVA) return GPU::Result::Offline;
        read = r.read(RBRead);
        if (read >= RingDwords || read != r.read(RBWrite)) return GPU::Result::Busy;
        ring = ringCPU; fence = fenceCPU; fenceVA = completionVA; write = read;
        issued = completed = 0;++executionEpoch;
        for(auto &record:executions)record={};
        broken = false; owned = true;
        r.write(InterruptMask, 0);
        // Reprogramming RB_CNTL resets RPTR without retiring the retained
        // PFP fetch state. Reuse the already owned ring after ME_INIT/GPMU;
        // preserve its retired prefix and append at the verified empty cursor.
        *fence = 0;
        r.publish();
        write=read;r.write(RBWrite,write);r.write(InterruptClear,OwnedIRQs);
        if (options.authenticatedZAP) {
            put(packet7(0x66, 1)); put(0); // CP_SET_SECURE_MODE(non-secure)
            put(packet7(0x26, 0)); // wait for the transition before fence DMA
        }
        // Startup succeeds only after a CP-generated timestamp and drained RB.
        timestamp(++issued);r.publish();r.write(RBWrite,write);
        // Publish the entire first stream while CP is still stopped. Releasing
        // it before construction can expose stale/cleared ring words.
        r.write(VBIFHalt,0);r.write(MEControl,0);
        for (unsigned waited = 0; waited < 100000; waited += 10) {
            r.acquire();
            if (*fence == issued && r.read(RBRead) == write && !(r.read(Status) & ~1u) && !r.read(TrustControl)) {
                completed = issued; read = write; r.write(InterruptClear, CompletionIRQ);
                r.write(InterruptMask, options.interrupts ? uint32_t(OwnedIRQs) : 0u); online = true; return GPU::Result::Success;
            }
            if ((r.read(InterruptStatus)&ErrorIRQs) || (r.read(CPInterruptStatus)&0x3f)) {
                broken=true;return GPU::Result::Fault;
            }
            r.delay(10);
        }
        broken = true; return GPU::Result::Timeout;
    }
    GPU::Result post(uint64_t ibVA, uint32_t words, uint32_t &sequence,const PageTableContext *context=nullptr,ExecutionToken *profile=nullptr,uint64_t phaseVA=0) {
        const IndirectBuffer buffer{ibVA,words,0};return postBatch(&buffer,1,sequence,context,profile,phaseVA);
    }
    bool submissionSpace(uint32_t count,bool profile) {
        if(!ready() || !count || count>MaxIndirectBuffers || !refreshRead())return false;
        if(profile) {
            unsigned slot=0;while(slot<ExecutionSlots && executions[slot].sequence)++slot;
            if(slot==ExecutionSlots)return false;
        }
        // Includes the maximum context switch, timestamps and counter packets
        // emitted by postBatch(). The caller reserves this admission until post.
        return ((read-write-1)&(RingDwords-1))>=4*count+128;
    }
    GPU::Result postBatch(const IndirectBuffer *buffers,uint32_t count,uint32_t &sequence,const PageTableContext *context=nullptr,ExecutionToken *profile=nullptr,uint64_t phaseVA=0) {
        if (!ready()) return GPU::Result::Offline;
        if(profile && (profile->epoch || profile->sequence || profile->slot))return GPU::Result::Invalid;
        if(phaseVA && (!profile || !context || (phaseVA&7) || !deviceRange(phaseVA,16)))return GPU::Result::Invalid;
        if(!buffers || !count || count>MaxIndirectBuffers)return GPU::Result::Invalid;
        for(uint32_t i=0;i<count;++i) {
            const auto &b=buffers[i];
            if(b.reserved || !b.words || b.words>IBDwords || (b.address&3) || !deviceRange(b.address,uint64_t(b.words)*4))return GPU::Result::Invalid;
        }
        uint32_t before[32]={},after[32]={};Encoder enter(before,32),leave(after,32);
        if (context && (!context->valid() || enter.pageTable(context->job)!=GPU::Result::Success ||
            leave.pageTable(context->kernel)!=GPU::Result::Success)) return GPU::Result::Invalid;
        // Reuse the existing leave barrier; do not add an extra idle/drain to
        // obtain a phase timestamp. Keep this guard coupled to pageTable().
        if(phaseVA && (leave.size()<2 || after[0]!=packet7(0x12,0) || after[1]!=packet7(0x26,0)))return GPU::Result::Invalid;
        if (!refreshRead()) return GPU::Result::Fault;
        unsigned slot=0;
        if(profile) {
            while(slot<ExecutionSlots && executions[slot].sequence)++slot;
            // Completed samples remain owned until their actual job retires.
            // Backpressure must not overwrite an unread or live receipt.
            if(slot==ExecutionSlots)return GPU::Result::Busy;
        }
        // Only consume the counter already initialized by Startup::configure.
        // A foreign selector/disabled counter makes frequency unavailable;
        // ordinary work and the trusted always-on timing remain unchanged.
        const bool core=profile && r.read(0x0bb0*4)==0 && (r.read(0x0464*4)&1);
        const bool busy=profile && r.read(0x046b*4)==6 && (r.read(0x0464*4)&1);
        if(issued==UINT32_MAX)return GPU::Result::NoResources;
        if (((read - write - 1) & (RingDwords - 1)) < 4*count+5+enter.size()+leave.size()+(profile?21:0)+(core?8:0)+(busy?8:0)+(phaseVA?8:0))
            return GPU::Result::Busy;
        const uint32_t next=issued+1,offset=ExecutionBase+slot*ExecutionStride;
        if(profile) {
            for(unsigned i=0;i<ExecutionStride/4;++i)fence[offset/4+i]=0;
            put(packet7(0x26,0));put(packet7(0x13,0));
            executionStamp(offset,next);executionTimestamp(offset+8);
            if(core)executionTimestamp(offset+24,0x3a0);
            if(busy)executionTimestamp(offset+40,0x3b0);
        }
        // IB is kernel-owned and immutable from this barrier until retirement.
        for (unsigned i=0;i<enter.size();++i) put(before[i]);
        // These two clock writes use this job's existing private flush page,
        // NEVER the global fence while its kernel tree is not selected. The
        // samples are diagnostic only and cannot authorize completion.
        if(phaseVA)registerTimestamp(phaseVA,0x4d2);
        for(uint32_t i=0;i<count;++i) {
            const auto &b=buffers[i];put(packet7(0x3f,3));put(uint32_t(b.address));put(uint32_t(b.address>>32));put(b.words);
        }
        // The completion timestamp uses the kernel tree. A successful fence
        // therefore also proves the private table is no longer selected.
        unsigned leaveFirst=0;
        if(phaseVA) {
            put(after[0]);put(after[1]);registerTimestamp(phaseVA+8,0x4d2);leaveFirst=2;
        }
        for (unsigned i=leaveFirst;i<leave.size();++i) put(after[i]);
        if(profile) {
            put(packet7(0x26,0)); put(packet7(0x13,0));
            if(busy)executionTimestamp(offset+48,0x3b0);
            if(core)executionTimestamp(offset+32,0x3a0);
            executionTimestamp(offset+16);executionStamp(offset+4,next);
            put(packet7(0x12,0)); // retire all profiling writes before the global success event
        }
        sequence = ++issued;
        if(profile){executions[slot]={sequence,core,busy};*profile={executionEpoch,sequence,slot};}
        timestamp(sequence);
        r.publish(); r.write(RBWrite, write); return GPU::Result::Success;
    }
    bool executionInterval(const ExecutionToken &token,uint64_t &begin,uint64_t &end) {
        // A timing sample never substitutes for the ordinary hardware fence.
        return executionSample(token,2,begin,end);
    }
    bool executionCoreInterval(const ExecutionToken &token,uint64_t &begin,uint64_t &end) {
        if(!executionComplete(token) || !executions[token.slot].core ||
            r.read(0x0bb0*4)!=0 || !(r.read(0x0464*4)&1))return false;
        return executionSample(token,6,begin,end);
    }
    bool executionBusyInterval(const ExecutionToken &token,uint64_t &begin,uint64_t &end) {
        if(!executionComplete(token) || !executions[token.slot].busy ||
            r.read(0x046b*4)!=6 || !(r.read(0x0464*4)&1))return false;
        return executionSample(token,10,begin,end,true); // zero busy cycles are legitimate
    }
    bool retireExecution(const ExecutionToken &token) {
        if(!executionComplete(token))return false;
        executions[token.slot]={};return true;
    }
    GPU::Result poll(uint32_t &sequence) {
        if (!ready()) return GPU::Result::Offline;
        uint32_t status = r.read(InterruptStatus);
        if (status & ErrorIRQs || r.read(Status3) & (1u << 24) || (r.read(CPInterruptStatus)&0x3f)) {
            broken = true; return GPU::Result::Fault;
        }
        if (status & CompletionIRQ) r.write(InterruptClear, CompletionIRQ);
        r.acquire(); uint32_t value = *fence;
        if (value < completed || value > issued || !refreshRead()) {
            broken = true; return GPU::Result::Fault;
        }
        completed = value; sequence = value; return GPU::Result::Success;
    }
    template<class Transition> GPU::Result idlePowerTransition(Transition transition) {
        if(!ready() || !owned || !fence)return GPU::Result::Offline;
        r.acquire();
        if(!refreshRead() || read!=write || *fence!=issued || (r.read(Status)&~1u) ||
            (r.read(Status3)&(1u<<24)) || r.read(TrustControl) ||
            (r.read(InterruptStatus)&ErrorIRQs) || (r.read(CPInterruptStatus)&0x3f))return GPU::Result::Fault;
        const uint32_t mask=r.read(InterruptMask);
        r.write(InterruptMask,0);r.write(MEControl,Halt);r.write(VBIFHalt,15);
        bool halted=false;
        for(unsigned elapsed=0;elapsed<100000;elapsed+=10) {
            if((r.read(MEControl)&Halt)==Halt && (r.read(VBIFHalted)&15)==15) {halted=true;break;}
            r.delay(10);
        }
        if(!halted || !haltedPipelineIdle(r.read(Status)) || r.read(RBRead)!=write ||
            r.read(RBWrite)!=write || !transition()) {
            broken=true;return GPU::Result::Fault; // caller retains ownership and proves stop/drain
        }
        // Resume the same empty ring. Reinitializing RB_CNTL would reset RPTR
        // underneath the retained PFP state and break its sequence/fence ABI.
        r.publish();r.write(VBIFHalt,0);r.write(MEControl,0);
        if(r.read(MEControl) || r.read(VBIFHalt) || r.read(RBRead)!=write || r.read(RBWrite)!=write) {
            broken=true;return GPU::Result::Fault;
        }
        r.write(InterruptMask,mask);return GPU::Result::Success;
    }
    bool stop() {
        online = false;
        if (!owned) return true;
        r.write(InterruptMask, 0); r.write(MEControl, Halt); r.write(VBIFHalt, 0xf);
        // Halt acknowledgement drains AXI before any DMA descriptor is unmapped.
        for (unsigned waited = 0; waited < 100000; waited += 10) {
            if ((r.read(MEControl) & Halt) == Halt && (r.read(VBIFHalted) & 0xf) == 0xf) {
                r.write(InterruptClear, OwnedIRQs & ~2u); owned = false; return true;
            }
            r.delay(10);
        }
        broken = true; return false; // Caller must retain every DMA allocation.
    }
};
}
