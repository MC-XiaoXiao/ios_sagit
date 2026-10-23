// SPDX-License-Identifier: BSD-2-Clause
#include "AdrenoSubmit.h"
#include "AdrenoPipeline.h"
#include "AdrenoMMU.h"
#include "AdrenoMappedPages.h"
#include "AdrenoRecovery.h"
#include "../drivers/security/QcomSCM/SCMPAS.h"
#include "AdrenoFirmware.h"
#include "../boot/bridge/gpu_firmware.h"
#include "../boot/bridge/gpu_smmu.h"
#include "../boot/bridge/gpu_submission.h"
#include <GPUPrograms.h>
#include <GPUNativeProtocol.h>
#include <GPUNativeCompletion.h>
#include <GPUSurfaceAccess.h>
#include <GPUNativeMemory.h>
#include <GPUDiagnostics.h>
#include <GPUHardwarePhases.h>
#include <GPUOperationActivity.h>
#include <GPUMappingBounds.h>
#include <GPUBlit.h>
#include <GPUSurfaceProtocol.h>
#include "GPUMemoryRanges.h"
#include "../drivers/gpu/QcomMTL/MetalSync.h"
#include "../drivers/gpu/QcomMTL/MetalTexture.h"
#include "../drivers/gpu/QcomMTL/MetalColor.h"
#include "../drivers/gpu/QcomMTL/MetalHeap.h"
#include "../drivers/gpu/QcomMTL/MetalArchive.h"
#include "../drivers/gpu/QcomMTL/MetalBatch.h"
#include "../drivers/gpu/QcomMTL/MetalUploadPool.h"
#include "NativeTimeline.h"
#include <array>
#include <algorithm>
#include <cassert>
#include <cstdio>
#include <cstring>
#include <functional>
#include <map>
#include <vector>
#include <utility>
using GPU::Result;

static void nativeSurfaceAccessStates() {
    using namespace GPU::Native;
    using State=SurfaceAccessState;
    assert(surfaceAccessObservation(false,false,0).state==State::Pending);
    assert(surfaceAccessObservation(true,false,0).state==State::Ready);
    for(bool ready:{false,true})for(uint32_t status:{0xe00002bcu,0xe00002ebu}) {
        auto ended=surfaceAccessObservation(ready,true,status);
        assert(ended.state==State::Failed && ended.status==status);
    }
    assert(surfaceAccessObservation(false,true,0).state==State::Pending);
    assert(surfaceAccessObservation(true,true,0).state==State::Ready);
    SurfaceAccessObservation access[3]={{State::Ready,0},{State::Pending,0},{State::Failed,0xe00002ebu}};
    auto read=[&](unsigned i){return access[i];};
    assert(surfaceAccesses(0,read).state==State::Ready);
    assert(surfaceAccesses(1,read).state==State::Ready);
    assert(surfaceAccesses(2,read).state==State::Pending);
    // A pending dependency must not hide a later failed surface.
    assert(surfaceAccesses(3,read).state==State::Failed && surfaceAccesses(3,read).status==0xe00002ebu);
    access[1].state=State::Ready;access[2].state=State::Ready;
    assert(surfaceAccesses(3,read).state==State::Ready);
    // Cancellation releases only never-posted dependencies. GPU-completed
    // but unretired jobs and even malformed issued/cancelled slots retain
    // their physical write ownership until actual CPU retirement.
    Adreno::AdmissionCandidate jobs[2]{};int owner=0;
    jobs[0].owner=jobs[1].owner=&owner;jobs[1].ready=true;
    auto view=[&](unsigned i){return jobs[i];};
    auto alias=[](unsigned,unsigned){return true;};
    assert(Adreno::nextAdmission(2,view,true)==2);
    assert(Adreno::dispatchDependencyBlocked(1,2,view,alias));
    jobs[0].cancelled=true;
    assert(Adreno::nextAdmission(2,view,true)==1);
    assert(!Adreno::dispatchDependencyBlocked(1,2,view,alias));
    jobs[0].issued=true;
    assert(Adreno::dispatchDependencyBlocked(1,2,view,alias));
}
static void uploadPagePool() {
    using namespace GPU;
    MetalUpload::Pool<2,32768> pool;
    auto make=[](uint32_t id,uint64_t bytes=16384) {
        Native::ResourceData value{};value.id=id;value.bytes=bytes;
        value.cpuAddress=0x10000000+uint64_t(id)*65536;value.gpuAddress=0x100000000+uint64_t(id)*65536;
        value.uniqueID=0x7210+id;value.privateInfo[127]=uint8_t(id);return value;
    };
    const Native::ResourceData zero{};
    assert(!pool.take().id && pool.misses()==1);
    auto a=make(1),b=make(2),c=make(3),duplicate=a;
    const auto originalA=a,originalB=b;
    assert(pool.put(a) && !memcmp(&a,&zero,sizeof(a)) && pool.count()==1 && pool.bytes()==16384);
    assert(!pool.put(duplicate) && !memcmp(&duplicate,&originalA,sizeof(duplicate)));
    assert(pool.put(b) && !memcmp(&b,&zero,sizeof(b)) && pool.bytes()==32768 && pool.returns()==2);
    const auto originalC=c;assert(!pool.put(c) && !memcmp(&c,&originalC,sizeof(c)));
    auto recycled=pool.take();assert(!memcmp(&recycled,&originalB,sizeof(recycled)) && pool.hits()==1 && pool.count()==1);
    assert(pool.put(recycled) && !recycled.id && pool.returns()==3 && pool.bytes()==32768);
    recycled=pool.take();assert(!memcmp(&recycled,&originalB,sizeof(recycled)));
    recycled=pool.take();assert(!memcmp(&recycled,&originalA,sizeof(recycled)) && !pool.bytes() && !pool.count());
    assert(!pool.take().id && pool.misses()==2);
    for(unsigned invalid=0;invalid<5;++invalid) {
        auto bad=make(4);
        switch(invalid){case 0:bad.id=0;break;case 1:bad.cpuAddress=0;break;case 2:bad.gpuAddress=0;break;
            case 3:bad.bytes=4095;break;case 4:bad.bytes=32769;break;}
        const auto saved=bad;assert(!pool.put(bad) && !memcmp(&bad,&saved,sizeof(bad)) && !pool.count());
    }
    MetalUpload::Pool<4,24576> charged;
    auto x=make(5),y=make(6);assert(charged.put(x) && !charged.put(y) && y.id==6 && charged.count()==1);
    auto z=make(7,8192);assert(charged.put(z) && charged.bytes()==24576);
}

struct SecureMonitor {
    std::vector<QcomSecure::Registers> calls, replies;
    unsigned sleeps = 0;
    void reply(int32_t code, uint64_t value = 0, uint64_t cookie = 0) {
        QcomSecure::Registers r; r.x[0] = uint32_t(code); r.x[1] = value; r.x[6] = cookie;
        replies.push_back(r);
    }
    void smc(QcomSecure::Registers &r) {
        assert(calls.size() < replies.size()); calls.push_back(r); r = replies[calls.size() - 1];
    }
    void sleep(unsigned ms) { assert(ms == 30); ++sleeps; }
};
static void secureFirmware() {
    using namespace QcomSecure;
    SecureMonitor fallback;
    fallback.reply(-1); fallback.reply(0, 1);
    Transport<SecureMonitor> narrow(fallback);
    assert(narrow.probe() == Status::OK && !narrow.is64());
    assert(fallback.calls[0].x[0] == 0xc2000601 && fallback.calls[1].x[0] == 0x82000601);
    assert(narrow.pas(1, 0x100000000ULL).status == Status::Invalid && fallback.calls.size() == 2);
    fallback.reply(0); assert(narrow.pas(1, 0x80000000).status == Status::OK);
    assert(fallback.calls.back().x[0] == 0x02000201 && fallback.calls.back().x[1] == 0x82);
    assert(fallback.calls.back().x[2] == 13 && fallback.calls.back().x[3] == 0x80000000);
    SecureMonitor monitor; monitor.reply(0, 1);
    Transport<SecureMonitor> transport(monitor); PAS<Transport<SecureMonitor>> pas(transport);
    assert(transport.probe() == Status::OK && transport.is64());
    monitor.reply(-12); monitor.reply(1, 0xdead, 0x1234); monitor.reply(0);
    assert(pas.resume() == Status::OK && pas.phase() == Phase::Warm && monitor.sleeps == 1);
    assert(monitor.calls[1].x[0] == 0x4200010a && monitor.calls[2].x[0] == 0x4200010a);
    assert(monitor.calls[3].x[0] == 1 && monitor.calls[3].x[6] == 0x1234);
    assert(monitor.calls[3].x[1] == 2 && monitor.calls[3].x[2] == 0 && monitor.calls[3].x[3] == 13);
    const auto warmCalls=monitor.calls.size();monitor.reply(0);
    assert(pas.resume()==Status::OK && monitor.calls.size()==warmCalls+1 && monitor.calls.back().x[0]==0x4200010a);
    assert(pas.shutdown() == Status::Invalid && pas.release());
    monitor.reply(0); monitor.reply(0); monitor.reply(0, 7); monitor.reply(0);
    assert(pas.init(0x90000000) == Status::OK && pas.setup(0x91000000, 0x10000) == Status::OK);
    assert(monitor.calls.back().x[0] == 0x42000202 && monitor.calls.back().x[4] == 0x10000);
    assert(pas.authenticate() == Status::Denied && pas.phase() == Phase::Region && !pas.release());
    assert(pas.shutdown() == Status::OK && pas.phase() == Phase::Empty && pas.release());
    monitor.reply(0);monitor.reply(0);monitor.reply(0);
    assert(pas.init(0x90000000)==Status::OK);
    const auto beforeFixed=monitor.calls.size();
    assert(pas.setup(0x91000000,0x10000,false)==Status::OK && monitor.calls.size()==beforeFixed);
    assert(pas.authenticate()==Status::OK && pas.shutdown()==Status::OK);
    monitor.reply(2);
    assert(pas.resume() == Status::Ambiguous && pas.phase() == Phase::Faulted && !pas.release());
    assert(pas.shutdown() == Status::Invalid && monitor.calls.size() == monitor.replies.size());
    SecureMonitor busy; busy.reply(0, 1);
    for (unsigned i = 0; i < 21; ++i) busy.reply(-12);
    Transport<SecureMonitor> bounded(busy); assert(bounded.probe() == Status::OK);
    assert(bounded.resume().status == Status::Busy && busy.sleeps == 20);
    // Packed Qualcomm MDT metadata: header/program headers then hash, while
    // PT_LOAD bytes come from b02. Synthetic signatures are never executed.
    std::vector<uint8_t> archiveBytes(244),metadata(164,0),payload(4096,0xaa);
    auto put=[](std::vector<uint8_t> &b,unsigned at,uint32_t value) {
        for(unsigned i=0;i<4;++i)b[at+i]=uint8_t(value>>(8*i));
    };
    put(metadata,0,0x464c457f);metadata[4]=metadata[5]=metadata[6]=1;
    put(metadata,20,1);put(metadata,28,52);metadata[40]=52;metadata[42]=32;metadata[44]=3;
    put(metadata,52+16,148);
    put(metadata,84,1);put(metadata,88,0x1000);put(metadata,100,16);put(metadata,108,2u<<24);
    put(metadata,116,1);put(metadata,120,0x2000);put(metadata,128,0x80000000);
    put(metadata,132,4);put(metadata,136,4096);put(metadata,140,1u<<27);put(metadata,144,4096);
    for(unsigned i=148;i<164;++i)metadata[i]=uint8_t(i);
    put(archiveBytes,0,0x50415a51);put(archiveBytes,4,1);put(archiveBytes,8,2);put(archiveBytes,12,244);put(archiveBytes,16,16);
    put(archiveBytes,32,UINT32_MAX);put(archiveBytes,36,64);put(archiveBytes,40,164);
    put(archiveBytes,48,2);put(archiveBytes,52,240);put(archiveBytes,56,4);
    std::copy(metadata.begin(),metadata.end(),archiveBytes.begin()+64);
    archiveBytes[240]=1;archiveBytes[241]=2;archiveBytes[242]=3;archiveBytes[243]=4;
    Adreno::ZapArchive archive;Adreno::MDTImage image;
    assert(archive.open(archiveBytes.data(),archiveBytes.size()) && image.parse(archive));
    assert(image.metadataBytes()==164 && image.imageBytes()==4096 && image.relocatable());
    std::vector<uint8_t> copied(164);
    assert(!image.metadataInto(copied.data(),163) && image.metadataInto(copied.data(),164) && copied==metadata);
    assert(image.imageInto(payload.data(),4096,0x90000000));
    assert(payload[0]==1 && payload[1]==2 && payload[2]==3 && payload[3]==4);
    for(unsigned i=4;i<4096;++i)assert(!payload[i]);
    put(archiveBytes,64+140,0);assert(image.parse(archive) && !image.relocatable());
    assert(!image.imageInto(payload.data(),4096,0x90000000) && image.imageInto(payload.data(),4096,0x80000000));
    put(archiveBytes,64+132,4097);assert(!image.parse(archive));
    put(archiveBytes,64+132,4);put(archiveBytes,48,3);assert(!image.parse(archive));
    put(archiveBytes,48,2);put(archiveBytes,52,80);assert(!archive.open(archiveBytes.data(),archiveBytes.size()));
    std::vector<uint8_t> monolithic(48+8196,0);
    put(monolithic,0,0x50415a51);put(monolithic,4,1);put(monolithic,8,1);put(monolithic,12,monolithic.size());put(monolithic,16,16);
    put(monolithic,32,UINT32_MAX);put(monolithic,36,48);put(monolithic,40,8196);
    std::copy(metadata.begin(),metadata.begin()+148,monolithic.begin()+48);
    std::copy(metadata.begin()+148,metadata.end(),monolithic.begin()+48+4096);
    monolithic[48+8192]=1;monolithic[48+8193]=2;monolithic[48+8194]=3;monolithic[48+8195]=4;
    assert(archive.open(monolithic.data(),monolithic.size()) && image.parse(archive));
    assert(image.metadataInto(copied.data(),copied.size()) && copied==metadata);
    assert(image.imageInto(payload.data(),payload.size(),0x90000000) && payload[0]==1 && payload[3]==4 && !payload[4]);
    // Original CP files keep their first file-header word in the archive, but
    // only words after it reach the instruction buffers. GPMU chunks cross
    // the real 127-word type4 packet boundary.
    std::vector<uint8_t> microcode(112+131*4,0);
    put(microcode,0,0x57464751);put(microcode,4,1);put(microcode,8,3);
    put(microcode,12,microcode.size());put(microcode,16,16);
    for(unsigned i=0;i<3;++i){put(microcode,32+16*i,i);put(microcode,36+16*i,80+16*i);}
    put(microcode,40,12);put(microcode,56,16);put(microcode,72,131*4);
    put(microcode,80,0xdead);put(microcode,84,0x12345678);put(microcode,88,0xabcdef01);
    put(microcode,96,0xbeef);put(microcode,100,0x98765432);
    put(microcode,112,130);put(microcode,116,2);
    for(unsigned i=0;i<128;++i)put(microcode,124+4*i,0x100+i);
    BridgeGPU::Firmware fw;
    assert(fw.open(microcode.data(),microcode.size()) && fw.cpBytes(0)==8 && fw.cpBytes(1)==12 && fw.gpmuWords()==130);
    uint32_t cpWords[4]={},gpmuWords[130]={};
    assert(!fw.copyCP(0,cpWords,7) && fw.copyCP(0,cpWords,sizeof(cpWords)));
    assert(cpWords[0]==0x12345678 && cpWords[1]==0xabcdef01 && !cpWords[2] && !cpWords[3]);
    assert(!fw.gpmuCommands(gpmuWords,129) && fw.gpmuCommands(gpmuWords,130));
    assert((gpmuWords[0]&127)==127 && ((gpmuWords[0]>>8)&0x3ffff)==0x8800);
    assert(gpmuWords[1]==0x100 && gpmuWords[127]==0x17e && gpmuWords[129]==0x17f);
    assert((gpmuWords[128]&127)==1 && ((gpmuWords[128]>>8)&0x3ffff)==0x887f);
    put(microcode,120,UINT32_MAX);assert(!fw.open(microcode.data(),microcode.size()) && !fw.cpBytes(0));
    put(microcode,120,0);put(microcode,116,1);assert(!fw.open(microcode.data(),microcode.size()));
    put(microcode,116,2);put(microcode,68,96);assert(!fw.open(microcode.data(),microcode.size()));
}

// Interpret the actual emitted PM4 against a fake MMIO/DMA fabric. Driver
// Engine and Encoder are compiled unchanged; completion is generated here by
// the emulated command processor, never by Engine::post or Engine::poll.
struct Device {
    std::map<uint32_t, uint32_t> regs;
    std::map<uint64_t, std::vector<uint32_t>> memory;
    std::vector<std::pair<uint32_t,uint32_t>> writes;
    bool execute = true, acknowledgeHalt = true, published = false, secureSwitch = true, gpmuRespond = true;
    unsigned gpmuInstructions = 0;
    unsigned delayed = 0, doorbells = 0;
    uint32_t *allocate(uint64_t va, unsigned words) { return memory.emplace(va, std::vector<uint32_t>(words)).first->second.data(); }
    std::function<bool(uint64_t,uint64_t,bool,uint64_t &)> translate;
    uint64_t kernelTTBR=0,activeTTBR=0,phaseScratchVA=0;
    std::vector<uint64_t> selectedRoots;
    bool memoryFault=false,protectedMode=true;
    uint32_t faultWord=0,haltErrors=0;
    uint32_t &word(uint64_t va,bool writeAccess=false) {
        if(translate) {
            uint64_t physical=0;
            if(!translate(activeTTBR,va,writeAccess,physical)) {
                memoryFault=true;regs[Adreno::Status3]|=1u<<24;return faultWord;
            }
            va=physical;
        }
        auto i = memory.upper_bound(va); assert(i != memory.begin()); --i;
        assert(!(va & 3) && va - i->first < i->second.size() * 4); return i->second[(va - i->first) / 4];
    }
    uint64_t pair(uint32_t offset) { return regs[offset] | (uint64_t(regs[offset + 4]) << 32); }
    void pair(uint32_t offset, uint64_t value) { regs[offset] = uint32_t(value); regs[offset + 4] = uint32_t(value >> 32); }
    uint32_t read(uint32_t offset) { return regs[offset]; }
    void publish() { published = true; }
    void acquire() {}
    void delay(unsigned us) { delayed += us; }
    static std::pair<uint32_t,uint32_t> header(uint32_t value) {
        assert((value & 0xf0000000) == 0x70000000);
        uint32_t op = (value >> 16) & 0x7f, count = value & 0x3fff;
        // Independent bit-count check rather than calling packet7/parity.
        assert((__builtin_popcount(count) + ((value >> 15) & 1)) % 2 == 1);
        assert((__builtin_popcount(op) + ((value >> 23) & 1)) % 2 == 1);
        return {op,count};
    }
    void ib(uint64_t address, uint32_t words) {
        for (uint32_t i = 0; i < words;) {
            const auto h = word(address+4*i++);
            if ((h&0xf0000000)==0x40000000) {
                const unsigned n=h&127,base=(h>>8)&0x3ffff;
                assert(n && n<=words-i && base>=0x8800 && base+n<=0x9800);
                assert((__builtin_popcount(n)+((h>>7)&1))%2==1);
                assert((__builtin_popcount(base)+((h>>27)&1))%2==1);
                for(unsigned j=0;j<n;++j)regs[4*(base+j)]=word(address+4*i++);
                gpmuInstructions+=n;continue;
            }
            auto [op,n] = header(h);
            assert(n <= words - i);
            if (op == 0x3d) {
                assert(n >= 3);
                uint64_t target = word(address + 4 * i) | (uint64_t(word(address + 4 * (i + 1))) << 32);
                for (unsigned j = 2; j < n; ++j) word(target + 4 * (j - 2),true) = word(address + 4 * (i + j));
            } else if(op==0x73) {
                assert(n==5 && word(address+4*i)==(1u<<30));
                const uint64_t target=word(address+4*(i+1))|(uint64_t(word(address+4*(i+2)))<<32);
                const uint64_t source=word(address+4*(i+3))|(uint64_t(word(address+4*(i+4)))<<32);
                const auto value=word(source);word(target,true)=value;
            } else assert((op == 0x26 || op == 0x12) && !n);
            i += n;
            if(memoryFault)return;
        }
    }
    void run(unsigned packetLimit=UINT32_MAX) {
        if (regs[Adreno::MEControl] & Adreno::Halt) return;
        uint64_t base = pair(Adreno::RBBase);
        auto next = [&]() {
            uint32_t value = word(base + regs[Adreno::RBRead] * 4);
            regs[Adreno::RBRead] = (regs[Adreno::RBRead] + 1) & (Adreno::RingDwords - 1); return value;
        };
        while (packetLimit-- && regs[Adreno::RBRead] != regs[Adreno::RBWrite]) {
            const auto raw=next();
            if((raw&0xf0000000)==0x40000000) {
                const unsigned n=raw&127,reg=(raw>>8)&0x3ffff;
                assert(n==1 && reg==0x831);
                assert((__builtin_popcount(n)+((raw>>7)&1))%2==1);
                assert((__builtin_popcount(reg)+((raw>>27)&1))%2==1);
                regs[reg*4]=next();assert(regs[reg*4]<=1);continue;
            }
            auto [op,n] = header(raw);
            if (op == 0x3f) {
                assert(n == 3); uint64_t address = next(); address |= uint64_t(next()) << 32;
                uint32_t words = next(); ib(address, words);if(memoryFault)return;
            } else if (op == 0x3d) {
                assert(n==3);uint64_t address=next();address|=uint64_t(next())<<32;
                if(translate)assert(activeTTBR==kernelTTBR);
                word(address,true)=next();
            } else if (op == 0x66) {
                assert(n == 1 && next() == 0); if (secureSwitch) regs[Adreno::TrustControl] = 0;
            } else if (op == 0x26 || op==0x12 || op==0x13) {
                assert(!n);
            } else if (op == 0x48) {
                assert(n==8);const uint32_t init[]={0x2f,3,0x20000000,0,0,0,0,0};
                for(auto value:init)assert(next()==value);
            } else if (op == 0x5f) {
                assert(n==1);auto mode=next();assert(mode<=1);protectedMode=mode;
            } else if (op==0x53) {
                assert(n==3 && !protectedMode && regs[0x831*4]==1);
                uint64_t root=next();root|=uint64_t(next())<<32;assert(!next());
                activeTTBR=root;selectedRoots.push_back(root);
            } else if (op==0x43) {
                assert(n==3 && next()==0x40000 && next()==1 && !next());
            } else if (op==0x3e) {
                assert(n==3);const auto control=next(),reg=control&0x3ffff;
                assert((reg==0x4d2 || reg==0x3a0 || reg==0x3b0) && control==(reg|(1u<<30)|(2u<<18)));
                uint64_t address=next();address|=uint64_t(next())<<32;
                if(translate && activeTTBR!=kernelTTBR)
                    assert(reg==0x4d2 && phaseScratchVA && (address==phaseScratchVA || address==phaseScratchVA+8));
                const uint64_t counter=uint64_t(7)<<32|regs[reg*4];
                word(address,true)=uint32_t(counter);word(address+4,true)=uint32_t(counter>>32);
                regs[reg*4]+=reg==0x4d2?19200:reg==0x3a0?596000:1192000;
            } else {
                assert(op == 0x46 && n == 4); uint32_t event = next();
                assert(event == (4u | (1u << 31)));
                uint64_t address = next(); address |= uint64_t(next()) << 32;
                if(translate)assert(activeTTBR==kernelTTBR);
                word(address,true) = next(); regs[Adreno::InterruptStatus] |= Adreno::CompletionIRQ;
            }
        }
    }
    void write(uint32_t offset, uint32_t value) {
        writes.emplace_back(offset, value);
        if (offset == Adreno::InterruptClear) regs[Adreno::InterruptStatus] &= ~value;
        else regs[offset] = value;
        if(offset==Adreno::MEControl && (value&Adreno::Halt)==Adreno::Halt)regs[Adreno::InterruptStatus]|=haltErrors;
        if (offset==4*BridgeGPU::RBBM_SW_RESET_CMD && value==1) {
            regs[Adreno::RBRead]=regs[Adreno::RBWrite]=0;regs[Adreno::ContextSwitch]=0;
            regs[Adreno::TrustControl]=1;regs[Adreno::InterruptStatus]=0;
            pair(Adreno::MEBase,0);pair(Adreno::PFPBase,0);
        }
        if (offset==4*BridgeGPU::GPMU_CM3_SYSRESET && !value && gpmuRespond) {
            assert(gpmuInstructions);regs[Adreno::GPMUReady]=0xbabeface;
        }
        if (offset == Adreno::VBIFHalt && acknowledgeHalt) regs[Adreno::VBIFHalted] = value;
        if ((offset==Adreno::RBWrite && !(regs[Adreno::MEControl]&Adreno::Halt)) ||
            (offset==Adreno::MEControl && !value && regs[Adreno::RBRead]!=regs[Adreno::RBWrite])) {
            assert(published); published = false; ++doorbells;
            if (execute) run();
        }
    }
    Device() {
        pair(Adreno::MEBase, 0x2000000); pair(Adreno::PFPBase, 0x2100000);
        regs[Adreno::GPMUReady] = 0xbabeface;
        regs[Adreno::MEControl] = Adreno::Halt; regs[Adreno::RBControl] = Adreno::RingControl;
        pair(Adreno::RBBase,0x3000000);
        regs[Adreno::VBIFHalted]=15;
    }
};
static Adreno::PlatformContract contract() { return {1,127,GPU::ChipID,1,0x2000000,0x2100000,0}; }

static void bridgeSubmission() {
    using Probe=BridgeGPU::SubmissionProbe;
    for(unsigned mode=0;mode<6;++mode) {
        Device d;auto *ring=d.allocate(0x3000000,Adreno::RingDwords);
        auto *work=d.allocate(0x3100000,Probe::Bytes/4);
        BridgeGPU::SubmissionIO<Device> io{d};Adreno::Engine<BridgeGPU::SubmissionIO<Device>> engine(io);
        assert(engine.start(contract(),ring,0x3000000,work,0x3100000)==Result::Success);
        if(mode==1)d.execute=false;
        unsigned checks=0;
        auto healthy=[&] {
            if(++checks==2 && mode==2)work[Probe::DestinationOffset/4]^=1; // untouched guard, real fence still succeeds
            if(checks==2 && mode==3)d.regs[Adreno::InterruptStatus]|=1u<<9;
            return mode!=4; // no doorbell when ownership is already lost
        };
        const auto status=Probe::run(engine,io,work,Probe::Bytes,0x3100000,healthy);
        assert(status==(mode==0 || mode==5?Result::Success:mode==1?Result::Timeout:mode==4?Result::Offline:Result::Fault));
        if(mode==1)assert(d.delayed==100000 && *work==1);
        if(mode==4)assert(d.doorbells==1);
        if(mode==5)d.haltErrors=1u<<9;
        assert(engine.stop() && !engine.ownsHardware());
        if(mode==3 || mode==5)assert(io.faulted() && !(d.regs[Adreno::InterruptStatus]&(1u<<9)));
        else assert(!io.faulted());
    }
}

static void commands()
{
    GPU::Session session; uint64_t context, other, buffer, bytes, foreign;
    assert(session.createContext(context) == Result::Success && session.createContext(other) == Result::Success);
    assert(session.createBuffer(context, 4096, 0, buffer, bytes) == Result::Success);
    assert(session.createBuffer(other, 4096, 0, foreign, bytes) == Result::Success);
    uint64_t addresses[GPU::MaxBuffers] = {}; addresses[buffer & 255] = 0x4000000;
    addresses[foreign & 255] = 0x5000000;
    uint32_t output[Adreno::IBDwords] = {}, destinations;
    GPU::Command cmd{GPU::Fill32,0,buffer,4,256,0xdeadbeef};
    auto encode = [&](GPU::Command c, unsigned capacity = Adreno::IBDwords) {
        Adreno::Encoder encoder(output, capacity);
        return Adreno::encodeCommands(session, uint32_t(context), &c, 1, addresses, encoder, destinations);
    };
    assert(encode(cmd) == Result::Success && destinations == (1u << (buffer & 255)));
    assert(Device::header(output[0]) == std::make_pair(0x3du,66u));
    assert(output[1] == 0x4000004 && output[2] == 0 && output[3] == 0xdeadbeef);
    cmd.value = 7; assert(output[3] == 0xdeadbeef); // Private IB is a snapshot.
    auto bad = cmd; bad.destination = foreign; assert(encode(bad) == Result::Invalid);
    bad = cmd; bad.destination |= uint64_t(1) << 32; assert(encode(bad) == Result::Invalid);
    bad = cmd; bad.reserved = 1; assert(encode(bad) == Result::Invalid);
    bad = cmd; bad.offset = UINT64_MAX; assert(encode(bad) == Result::Invalid);
    bad = cmd; bad.bytes = 4096; assert(encode(bad) == Result::Invalid);
    bad = cmd; bad.bytes = 0; assert(encode(bad) == Result::Invalid);
    bad = cmd; bad.opcode = 0x3f; assert(encode(bad) == Result::Unsupported);
    assert(encode(cmd, 1) == Result::NoResources);
    assert(encode({GPU::Barrier,0,0,0,0,0}) == Result::Success && destinations == 0);
    assert(encode({GPU::Barrier,0,buffer,0,0,0}) == Result::Invalid);
    addresses[buffer & 255] = (uint64_t(1) << Adreno::AddressBits) - 4;
    assert(encode(cmd) == Result::Invalid);
    uint64_t queue;
    assert(session.createQueue(context, queue) == Result::Success);
    auto *q = session.queues.find(queue); q->issued = 2; q->completed = 1; q->pending = 1;
    bool done = false;
    assert(session.queryFence(queue, 0, done) == Result::Invalid);
    assert(session.queryFence(queue, 3, done) == Result::Invalid);
    assert(session.queryFence(queue, 1, done) == Result::Success && done);
    assert(session.queryFence(queue, 2, done) == Result::Success && !done);
    assert(session.destroyQueue(queue) == Result::Busy);
    q->faulted = true; assert(session.queryFence(queue, 2, done) == Result::Fault);
    assert(session.queryFence(queue, 1, done) == Result::Success && done);
}
static void submission()
{
    assert(Adreno::haltedPipelineIdle(0x800001c1));
    assert(!Adreno::haltedPipelineIdle(0xc00001c1) && !Adreno::haltedPipelineIdle(0x800001c3));
    Device d;
    auto *ring = d.allocate(0x3000000, Adreno::RingDwords);
    auto *fence = d.allocate(0x3100000, 1024);
    auto *ib = d.allocate(0x3200000, Adreno::IBDwords);
    auto *target = d.allocate(0x4000000, 1024);
    // Start at the final two words to force the startup timestamp across wrap.
    d.regs[Adreno::RBRead] = d.regs[Adreno::RBWrite] = Adreno::RingDwords - 2;
    Adreno::Engine<Device> engine(d);
    assert(engine.start(contract(), ring, 0x3000000, fence, 0x3100000) == Result::Success);
    assert(engine.ready() && *fence == 1 && d.doorbells == 1);
    const auto ringControlWrites=std::count_if(d.writes.begin(),d.writes.end(),[](const auto &w){return w.first==Adreno::RBControl;});
    const auto oldRead=d.regs[Adreno::RBRead],oldMask=d.regs[Adreno::InterruptMask];bool transitioned=false;
    assert(engine.idlePowerTransition([&]() {
        assert((d.regs[Adreno::MEControl]&Adreno::Halt)==Adreno::Halt && d.regs[Adreno::VBIFHalted]==15);
        assert(!d.regs[Adreno::InterruptMask]);transitioned=true;return true;
    })==Result::Success && transitioned && engine.ready());
    assert(*fence==1 && d.regs[Adreno::RBRead]==oldRead && d.regs[Adreno::InterruptMask]==oldMask);
    assert(std::count_if(d.writes.begin(),d.writes.end(),[](const auto &w){return w.first==Adreno::RBControl;})==ringControlWrites);
    {
        Device blocked;auto *r=blocked.allocate(0x3000000,Adreno::RingDwords);
        auto *f=blocked.allocate(0x3100000,1024);Adreno::Engine<Device> held(blocked);
        assert(held.start(contract(),r,0x3000000,f,0x3100000)==Result::Success);
        blocked.acknowledgeHalt=false;bool powerCalled=false;
        assert(held.idlePowerTransition([&](){powerCalled=true;return true;})==Result::Fault);
        assert(!powerCalled && held.ownsHardware() && !held.ready() && *f==1);
        blocked.acknowledgeHalt=true;assert(held.stop());
    }
    Adreno::Encoder encoder(ib, Adreno::IBDwords);
    assert(encoder.fill(0x4000000, 256, 0x12345678) == Result::Success);
    assert(encoder.barrier() == Result::Success);
    d.execute = false; uint32_t sequence = 0, completed = 0;
    assert(engine.post(0x3200000, encoder.size(), sequence) == Result::Success && sequence == 2);
    transitioned=false;
    assert(engine.idlePowerTransition([&](){transitioned=true;return true;})==Result::Fault && !transitioned);
    assert(*fence == 1 && target[0] == 0);
    assert(engine.poll(completed) == Result::Success && completed == 1); // No CPU completion.
    d.run(); assert(engine.poll(completed) == Result::Success && completed == 2);
    for (unsigned i = 0; i < 64; ++i) assert(target[i] == 0x12345678);
    assert(!target[64]);
    uint64_t gpuBegin=0,gpuEnd=0;Adreno::ExecutionToken profile;
    assert(!engine.executionInterval(profile,gpuBegin,gpuEnd));
    d.regs[0x464*4]=1; // Startup already enabled the retained counter.
    d.regs[0x46b*4]=6;
    assert(engine.post(0x3200000,encoder.size(),sequence,nullptr,&profile)==Result::Success && sequence==3);
    uint32_t untouched=0;assert(engine.post(0x3200000,encoder.size(),untouched,nullptr,&profile)==Result::Invalid && !untouched);
    assert(!engine.executionInterval(profile,gpuBegin,gpuEnd) && fence[0]==2);
    d.run();assert(!engine.executionInterval(profile,gpuBegin,gpuEnd)); // poll must validate the real fence
    uint64_t coreBegin=0,coreEnd=0;
    assert(!engine.executionCoreInterval(profile,coreBegin,coreEnd));
    assert(!engine.executionBusyInterval(profile,coreBegin,coreEnd));
    assert(engine.poll(completed)==Result::Success && completed==3);
    assert(engine.executionInterval(profile,gpuBegin,gpuEnd) && gpuBegin==(uint64_t(7)<<32) && gpuEnd-gpuBegin==19200);
    assert(engine.executionCoreInterval(profile,coreBegin,coreEnd) && coreEnd-coreBegin==596000);
    assert(engine.executionBusyInterval(profile,coreBegin,coreEnd) && coreEnd-coreBegin==1192000);
    d.regs[0x46b*4]=0;assert(!engine.executionBusyInterval(profile,coreBegin,coreEnd));d.regs[0x46b*4]=6;
    for(const auto &write:d.writes)assert(write.first!=0xbb0*4 && write.first!=0x464*4 && write.first!=0x46b*4);
    d.regs[0xbb0*4]=1;assert(!engine.executionCoreInterval(profile,coreBegin,coreEnd));
    d.regs[0xbb0*4]=0;d.regs[0x464*4]=0;assert(!engine.executionCoreInterval(profile,coreBegin,coreEnd));
    assert(engine.retireExecution(profile));profile={};
    assert(engine.post(0x3200000,encoder.size(),sequence,nullptr,&profile)==Result::Success);
    d.run();assert(engine.poll(completed)==Result::Success);
    assert(engine.executionInterval(profile,gpuBegin,gpuEnd) && !engine.executionCoreInterval(profile,coreBegin,coreEnd));
    assert(!engine.executionBusyInterval(profile,coreBegin,coreEnd));
    unsigned accepted = 0;
    while (engine.post(0x3200000, encoder.size(), sequence) == Result::Success) ++accepted;
    assert(accepted == (Adreno::RingDwords - 1) / 9);
    assert(engine.poll(completed) == Result::Success && completed == 4);
    d.run(); assert(engine.poll(completed) == Result::Success && completed == engine.lastIssued());
    assert(engine.post(0x3200001, encoder.size(), sequence) == Result::Invalid);
    assert(engine.post(0x3200000, Adreno::IBDwords + 1, sequence) == Result::Invalid);
    d.regs[Adreno::InterruptStatus] |= 1u << 9;
    assert(engine.poll(completed) == Result::Fault && !engine.ready());
    d.acknowledgeHalt = false;
    assert(!engine.stop() && engine.ownsHardware());
    assert(d.regs[Adreno::InterruptMask] == 0 && d.delayed >= 100000);
    d.acknowledgeHalt = true; assert(engine.stop() && !engine.ownsHardware());
    Device secure;
    auto *secureRing=secure.allocate(0x3000000,Adreno::RingDwords), *secureFence=secure.allocate(0x3100000,1024);
    secure.regs[Adreno::TrustControl]=1;
    Adreno::Engine<Device> authenticated(secure);
    assert(authenticated.start(contract(),secureRing,0x3000000,secureFence,0x3100000)==Result::Offline && secure.writes.empty());
    assert(authenticated.start(contract(),secureRing,0x3000000,secureFence,0x3100000,{true,false})==Result::Success);
    assert(!secure.regs[Adreno::TrustControl] && !secure.regs[Adreno::InterruptMask] && authenticated.stop());
    for(auto entry:secure.writes)assert(entry.first!=Adreno::TrustControl);
    secure.regs[Adreno::TrustControl]=1;secure.secureSwitch=false;
    assert(authenticated.start(contract(),secureRing,0x3000000,secureFence,0x3100000,{true,false})==Result::Timeout);
    assert(!authenticated.ready() && authenticated.ownsHardware() && authenticated.stop());
    // Execute the bridge's exact reset -> ME_INIT -> GPMU -> PAS-authorized
    // secure transition packets. This is an offline model, never a PAS claim.
    for(bool responds:{true,false}) {
        Device cold;cold.gpmuRespond=responds;
        auto *bootRing=cold.allocate(0x3000000,Adreno::RingDwords),*bootFence=cold.allocate(0x3100000,1024);
        auto *gpmu=cold.allocate(0x3200000,3);
        gpmu[0]=0x40000000|2|(Adreno::parity(2)<<7)|(0x8800<<8)|(Adreno::parity(0x8800)<<27);
        gpmu[1]=0x12345678;gpmu[2]=0xabcdef01;
        BridgeGPU::Startup<Device> startup(cold);
        cold.regs[Adreno::VBIFHalted]=0;
        assert(!startup.reset() && cold.writes.empty());
        cold.regs[Adreno::VBIFHalted]=15;cold.regs[Adreno::RBRead]=cold.regs[Adreno::RBWrite]=42;
        assert(startup.reset() && !cold.regs[Adreno::RBRead] && !cold.regs[Adreno::GPMUReady]);
        assert(startup.start(contract(),bootRing,0x3000000));
        assert(cold.regs[4*BridgeGPU::TPL1_MODE_CNTL]==0x100 && cold.regs[4*BridgeGPU::UCHE_GMEM_RANGE_MAX_LO]==0x1fffff);
        assert(startup.gpmu({1,300000000,800000,0},0x3200000,3)==responds);
        assert(startup.stop() && cold.gpmuInstructions==2);
        assert(cold.regs[0x8800*4]==0x12345678 && cold.regs[(0x9800+14)*4]==800);
        assert(cold.regs[Adreno::TrustControl]==1 && !cold.regs[Adreno::InterruptMask]);
        for(auto entry:cold.writes)assert(entry.first!=Adreno::TrustControl);
        if(!responds){assert(cold.delayed>=100000);continue;}
        Adreno::Engine<Device> resumed(cold);
        assert(resumed.start(contract(),bootRing,0x3000000,bootFence,0x3100000,{true,false})==Result::Success);
        assert(*bootFence==1 && !cold.regs[Adreno::TrustControl] && resumed.stop());
    }
    for(unsigned failure=0;failure<5;++failure) {
        Device reset;auto *rr=reset.allocate(0x3000000,Adreno::RingDwords),*rf=reset.allocate(0x3100000,1024);
        auto *gpmu=reset.allocate(0x3200000,3),*lost=reset.allocate(0x3400000,16),*target=reset.allocate(0x4000000,1024);
        gpmu[0]=0x40000000|2|(Adreno::parity(2)<<7)|(0x8800<<8)|(Adreno::parity(0x8800)<<27);
        gpmu[1]=0x12345678;gpmu[2]=0xabcdef01;
        Adreno::Engine<Device> engine(reset);assert(engine.start(contract(),rr,0x3000000,rf,0x3100000)==Result::Success);
        Adreno::Encoder old(lost,16);assert(old.fill(0x4000000,4,0xdeadbeef)==Result::Success && old.barrier()==Result::Success);
        reset.execute=false;uint32_t seq=0;assert(engine.post(0x3400000,old.size(),seq)==Result::Success);
        reset.regs[Adreno::InterruptStatus]|=1u<<9;assert(engine.poll(seq)==Result::Fault && engine.stop());
        reset.execute=true;reset.gpmuRespond=failure!=1;reset.secureSwitch=failure!=4;
        reset.regs[0xa88d*4]=reset.regs[0xa88b*4]=1u<<20;
        struct Guard {
            Device &d;unsigned failure,resumes=0;bool access=true;
            bool accessible(){return access;}
            bool healthy(){return access;}
            bool isolated(){return (d.read(Adreno::MEControl)&Adreno::Halt)==Adreno::Halt && (d.read(Adreno::VBIFHalted)&15)==15;}
            bool synchronize(){return failure!=2;}
            bool restoreZAP(){++resumes;if(failure==3)access=false;return access;}
        } guard{reset,failure};
        Adreno::FirmwareHandoff h{1,31,contract().pm4,contract().pfp,0x3200000,4096,4096,3,300000000,800000,{0,0,0}};
        assert(h.valid(contract()));auto invalid=h;invalid.gpmu=h.pm4;assert(!invalid.valid(contract()));
        const auto result=Adreno::recover(contract(),h,reset,guard,engine,rr,0x3000000,rf,0x3100000);
        assert(!*target); // the pending old fill was discarded by reset, never replayed
        if(!failure)assert(result.result==Result::Success && engine.ready() && *rf==1 && guard.resumes==1 && engine.stop());
        else {
            assert(result.result!=Result::Success && !engine.ready() && result.dmaStopped==(failure!=3));
            if(failure==1)assert(result.stage==Adreno::RecoveryStage::GPMU && !guard.resumes);
            if(failure==2)assert(result.stage==Adreno::RecoveryStage::Mapper && !guard.resumes);
            if(failure==3)assert(result.stage==Adreno::RecoveryStage::ZAP);
            if(failure==4)assert(result.stage==Adreno::RecoveryStage::Timestamp && *rf==1 && reset.regs[Adreno::TrustControl]==1);
        }
        for(auto entry:reset.writes)assert(entry.first!=Adreno::TrustControl);
    }
}
static void registerPackets()
{
    // Decode register writes independently, including parity and IB boundaries.
    std::array<uint32_t,130> words{};
    uint32_t values[126];for(unsigned i=0;i<126;++i)values[i]=0x51ac0000+i;
    for(unsigned n:{1u,2u,6u,16u,126u}) {
        words.fill(0xfeedfeed);Adreno::Encoder e(words.data(),words.size(),3);
        assert(e.regs(0x40000-n,values,n)==Result::Success && e.size()==n+1);
        const auto h=words[0],base=(h>>8)&0x3ffff;
        assert(h>>28==4 && (h&127)==n && base==0x40000-n);
        assert((__builtin_popcount(base)+((h>>27)&1))%2==1);
        assert((__builtin_popcount(n)+((h>>7)&1))%2==1);
        for(unsigned i=0;i<n;++i)assert(words[i+1]==values[i]);
        assert(words[n+1]==0xfeedfeed);
    }
    words.fill(0xfeedfeed);Adreno::Encoder e(words.data(),8,2);
    assert(e.reg(0xe010,7)==Result::Success);const auto point=e.checkpoint();const auto original=words;
    assert(e.regs(0xe010,values,4)==Result::NoResources); // reserved sealing words
    assert(e.regs(0x3ffff,values,2)==Result::NoResources);
    assert(e.regs(0xe010,values,0)==Result::NoResources);
    assert(e.regs(0xe010,values,127)==Result::NoResources);
    assert(e.regs(0xe010,nullptr,2)==Result::NoResources);
    assert(e.size()==point.count && words==original);
    assert(e.regs(0xe011,values,3)==Result::Success);
    e.rewind(point);assert(e.regs(0xe020,values+3,3)==Result::Success);
    assert(words[0]==original[0] && words[1]==original[1]);
    assert(((words[2]>>8)&0x3ffff)==0xe020 && words[3]==values[3] && words[5]==values[5]);
    assert(words[6]==0xfeedfeed && words[7]==0xfeedfeed);
}
static void copyEncoding()
{
    // Wire/packet bounds only: this cannot validate CP firmware execution.
    auto command = GPU::Native::copyCommand(7, 12, 9, 20, 8);
    assert(command.opcode == GPU::Copy32 && command.destination == ((uint64_t(7) << 32) | 9));
    assert(command.offset == ((uint64_t(12) << 32) | 20) && !command.reserved && !command.value);
    uint32_t words[12] = {};
    Adreno::Encoder encoder(words, 12);
    assert(encoder.copy(0x100400000ULL, 0x200500000ULL, 8) == Result::Success && encoder.size() == 12);
    assert(Device::header(words[0]) == std::make_pair(0x73u, 5u));
    assert(words[1] == (1u << 30) && words[2] == 0x500000 && words[3] == 2);
    assert(words[4] == 0x400000 && words[5] == 1 && words[8] == 0x500004 && words[10] == 0x400004);
    Adreno::Encoder shortIB(words, 11);
    assert(shortIB.copy(0x4000000, 0x5000000, 8) == Result::NoResources && !shortIB.size());
    assert(shortIB.copy(0x4000001, 0x5000000, 4) == Result::Invalid && !shortIB.size());
    assert(shortIB.copy((uint64_t(1) << Adreno::AddressBits) - 4, 0x5000000, 8) == Result::Invalid);
}
static void blitBatching()
{
    static_assert(GPU::Blit::IBDwords == Adreno::IBDwords,"native IB capacity");
    std::vector<uint32_t> words(Adreno::IBDwords);
    GPU::Blit::Batch batch;
    // A full copy batch must still leave room for the kernel's final barrier.
    uint32_t bytes=batch.capacity(true);
    assert(bytes==10920 && batch.append(GPU::Native::copyCommand(1,0,2,0,bytes)));
    assert(!batch.capacity(true) && !batch.append({GPU::Fill32,0,2,bytes,4,0}));
    Adreno::Encoder copy(words.data(),words.size());
    assert(copy.copy(0x4000000,0x5000000,bytes)==Result::Success && copy.size()==batch.words());
    assert(copy.barrier()==Result::Success && copy.size()<=words.size());
    batch.reset();bytes=batch.capacity(false);
    assert(bytes==65516 && batch.append({GPU::Fill32,0,2,0,bytes,0xaabbccdd}));
    Adreno::Encoder fill(words.data(),words.size());
    assert(fill.fill(0x5000000,bytes,0xaabbccdd)==Result::Success && fill.size()==batch.words());
    assert(fill.barrier()==Result::Success && fill.size()==words.size());
    // Small strided rows share one native submit without writing row padding.
    batch.reset();Device d;const uint64_t from=0x4000000,to=0x5000000;
    auto *source=d.allocate(from,4096),*destination=d.allocate(to,4096);
    for (unsigned i=0;i<4096;++i) {source[i]=i+17;destination[i]=0xdeadbeef;}
    Adreno::Encoder rows(words.data(),words.size());
    for (unsigned row=0;row<GPU::MaxCommands;++row) {
        auto command=GPU::Native::copyCommand(1,row*256,2,row*256,64);
        assert(batch.append(command));
        assert(rows.copy(from+(command.offset>>32),to+uint32_t(command.offset),command.bytes)==Result::Success);
    }
    assert(batch.count()==GPU::MaxCommands && !batch.capacity(true) && !batch.capacity(false));
    assert(!batch.append({GPU::Fill32,0,2,0,4,0}) && rows.size()==batch.words());
    assert(rows.barrier()==Result::Success);
    auto *ib=d.allocate(0x6000000,words.size());std::copy(words.begin(),words.end(),ib);
    d.ib(0x6000000,rows.size());
    for (unsigned i=0;i<4096;++i) assert(destination[i]==(i%64<16?source[i]:0xdeadbeef));
    GPU::Blit::ComputePlan plan;
    assert(GPU::Blit::copyPlan(4,8,512,1024,768,33,plan));
    assert(plan.copy && plan.threads==4224 && plan.sourceBytes==33280 && plan.destinationBytes==25088);
    assert(plan.parameters[0]==128 && plan.parameters[1]==33 && plan.parameters[2]==256 && plan.parameters[3]==192);
    assert(!GPU::Blit::copyPlan(1,0,16384,16384,16384,1,plan));
    assert(!GPU::Blit::copyPlan(0,0,16384,UINT64_MAX,16384,2,plan));
    assert(!GPU::Blit::copyPlan(0,0,16384,16384,16384,UINT64_MAX,plan));
    assert(GPU::Blit::fillPlan(4,16384,0x76543210,plan) && !plan.copy && plan.parameters[3]==0x76543210);
    assert(GPU::Blit::fillPlan(4,4,7,plan,false) && plan.threads==1 && !GPU::Blit::fillPlan(4,4,7,plan));
    assert(!GPU::Blit::fillPlan(GPU::MaxBufferBytes-4,16384,0,plan));
    assert(GPU::Blit::copyPlan(0,4,4,8,8,2,plan,false) && plan.threads==2);
    assert(!GPU::Blit::copyPlan(0,4,4,8,8,2,plan)); // small IOSurface copies still use complete native layout checks
    assert(GPU::Blit::copyBytePlan(1,2,5,7,9,3,plan) && plan.copy && plan.bytewise);
    const uint32_t layout[]={5,3,7,9,1,2,0,0};
    assert(!memcmp(plan.parameters,layout,sizeof(layout)) && plan.sourceOffset==0 && plan.destinationOffset==0);
    assert(plan.sourceBytes==20 && plan.destinationBytes==28 && plan.threads==7);
    assert(GPU::Blit::copyBytePlan(0,3,1,1,1,4,plan) && plan.sourceBytes==4 && plan.destinationBytes==8 && plan.threads==2);
    assert(GPU::Blit::copyBytePlan(1,2,1,1,1,1,plan) && plan.sourceBytes==4 && plan.destinationBytes==4);
    assert(!GPU::stridedOverlap({1,1,1,1},{2,1,1,1})); // disjoint bytes may share a physical word
    assert(!GPU::stridedOverlap({0,1,2,8},{1,1,2,8})); // interleaved rows remain disjoint
    assert(GPU::stridedOverlap({0,2,2,8},{1,1,2,8}));
    assert(!GPU::Blit::copyBytePlan(0,0,4,4,4,1,plan)); // retain the aligned fast path
    assert(!GPU::Blit::copyBytePlan(1,0,4,UINT64_MAX,4,2,plan));
    assert(GPU::Blit::fillBytePlan(3,2,0x12345678,plan) && plan.bytewise && !plan.copy && plan.threads==2);
    assert(plan.parameters[5]==3 && plan.parameters[6]==0x12345678 && plan.destinationBytes==8);
    assert(GPU::Blit::fillBytePlan(GPU::MaxBufferBytes-1,1,0,plan) && plan.destinationBytes==4);
    assert(!GPU::Blit::fillBytePlan(GPU::MaxBufferBytes-1,2,0,plan));
    assert(GPU::Blit::fillRowsPlan(64,52,128,5,0x12345678,plan) && plan.rowFill && plan.threads==65 && plan.destinationBytes==564);
    assert(GPU::Blit::fillRowsPlan(64,3,64,5,0x12121212,plan) && plan.threads==5 && plan.destinationBytes==260);
    assert(GPU::Blit::fillRowsPlan(64,6,64,5,0x12341234,plan) && plan.threads==10 && plan.destinationBytes==264);
    assert(GPU::Blit::fillRowsPlan(1,2,3,3,0x76543210,plan) && plan.threads==3 && plan.destinationOffset==0 && plan.destinationBytes==12);
    GPU::Blit::ComputePlan canonical;assert(GPU::Blit::rowFillParameters(plan.destinationOffset,plan.parameters,canonical));
    assert(!memcmp(plan.parameters,canonical.parameters,sizeof(plan.parameters)));
    auto bad=plan;bad.parameters[5]=1;assert(!GPU::Blit::rowFillParameters(0,bad.parameters,canonical));
    bad=plan;bad.parameters[3]=4;assert(!GPU::Blit::rowFillParameters(0,bad.parameters,canonical));
    assert(!GPU::Blit::rowFillParameters(UINT64_MAX,plan.parameters,canonical));
    assert(!GPU::Blit::fillRowsPlan(0,4,3,2,0,plan) && !GPU::Blit::fillRowsPlan(0,1,1,0,0,plan));
    assert(!GPU::Blit::fillRowsPlan(GPU::MaxBufferBytes-4,3,4,2,0,plan));
    assert(!GPU::Blit::fillRowsPlan(0,1,UINT64_MAX,2,0,plan));
    assert(GPU::Blit::fillPairRowsPlan(64,56,512,5,0xbe123456,0x3fa12345,plan));
    assert(plan.pairFill && !plan.rowFill && plan.threads==70 && plan.destinationBytes==2104);
    assert(GPU::Blit::pairFillParameters(64,plan.parameters,canonical));
    assert(!memcmp(plan.parameters,canonical.parameters,sizeof(plan.parameters)));
    bad=plan;bad.parameters[5]=1;assert(!GPU::Blit::pairFillParameters(64,bad.parameters,canonical));
    assert(!GPU::Blit::fillPairRowsPlan(4,56,512,5,0,0,plan));
    assert(!GPU::Blit::fillPairRowsPlan(64,52,512,5,0,0,plan));
    assert(!GPU::Blit::fillPairRowsPlan(64,56,508,5,0,0,plan));
    assert(!GPU::Blit::fillPairRowsPlan(GPU::MaxBufferBytes-8,16,16,1,0,0,plan));
    // Test vector eligibility at group/tail boundaries, both word patterns,
    // nonzero binding offsets, single rows and padded rows. Byte ownership
    // is checked against an independent row/column reference.
    GPU::Blit::VectorFillGeometry geometry;
    for(bool pairs:{false,true})for(uint32_t width:{4u,8u,12u,16u,20u,24u,28u,32u,1004u,1008u,1012u,1016u,1020u,1024u,1028u,1032u,1036u,1040u,4328u})for(uint32_t rows:{1u,5u,17u}) {
        if(pairs && (width&7))continue;
        const uint32_t offset=64,stride=(width+47)&~15u;
        assert(pairs?GPU::Blit::fillPairRowsPlan(offset,width,stride,rows,0x12345678,0x90abcdef,plan):
            GPU::Blit::fillRowsPlan(offset,width,stride,rows,0x12345678,plan));
        assert(GPU::Blit::vectorFillGeometry(plan,geometry));
        GPU::Shader::Dispatch dispatch{};geometry.apply(dispatch);assert(geometry.matches(dispatch));
        uint64_t elements;unsigned dimensions;
        assert(GPU::Shader::dispatchGeometry(dispatch,elements,dimensions) && elements==(width+15)/16 && dimensions==(rows==1?1u:2u));
        for(unsigned axis=0;axis<3;++axis)for(unsigned field=0;field<3;++field) {
            auto badDispatch=dispatch;auto *values=field==0?badDispatch.groups:field==1?badDispatch.local:badDispatch.grid;
            ++values[axis];assert(!geometry.matches(badDispatch));
        }
        std::vector<uint8_t> reference(offset+plan.destinationBytes+64,0xa5),actual=reference,owners(reference.size(),0);
        const uint32_t patterns[]={0x12345678,pairs?0x90abcdefu:0x12345678u};
        for(uint32_t y=0;y<rows;++y)for(uint32_t x=0;x<width;++x)
            reference[offset+y*plan.parameters[2]+x]=uint8_t(patterns[(x/4)&1]>>((x&3)*8));
        for(uint32_t y=0;y<geometry.grid[1];++y)for(uint32_t x=0;x<geometry.grid[0];++x)for(unsigned lane=0;lane<16;++lane) {
            if(x*16+lane>=width)continue;
            const auto at=offset+y*plan.parameters[2]+x*16+lane;
            actual[at]=uint8_t(patterns[(lane/4)&1]>>((lane&3)*8));assert(++owners[at]==1);
        }
        assert(actual==reference);
    }
    for(auto width:{1u,2u,3u,5u,6u,7u,15u,17u,19u}) {
        assert(GPU::Blit::fillRowsPlan(64,width,64,5,0,plan) && !GPU::Blit::vectorFillGeometry(plan,geometry));
    }
    assert(GPU::Blit::fillRowsPlan(4,32,64,5,0,plan) && !GPU::Blit::vectorFillGeometry(plan,geometry));
    assert(GPU::Blit::fillRowsPlan(64,32,68,5,0,plan) && !GPU::Blit::vectorFillGeometry(plan,geometry));
    assert(GPU::Blit::fillRowsPlan(64,16,16,65536,0,plan) && !GPU::Blit::vectorFillGeometry(plan,geometry));
    assert(GPU::Blit::fillRowsPlan(0,8640,8704,1920,0,plan) && GPU::Blit::vectorFillGeometry(plan,geometry));
    assert(geometry.groups[0]==9 && geometry.groups[1]==1920 && geometry.grid[0]==540);
    assert(GPU::Blit::fillPairRowsPlan(8,32,64,5,0,0,plan) && !GPU::Blit::vectorFillGeometry(plan,geometry));
    for (const char *name:{"blit_copy_words","blit_fill_words","blit_copy_bytes","blit_fill_bytes","blit_fill_rows","blit_fill_pairs","blit_fill_rows_2d","blit_fill_pairs_2d","blit_fill_rows_2d_tail","blit_fill_pairs_2d_tail"}) {
        const GPU::Shader::Program *program=nullptr;
        for (const auto &p:GPUPrograms::Programs) if (!strcmp(p.name,name)) {program=&p;break;}
        assert(program && program->stage==GPU::Shader::Compute && (program->flags&QGPU_BOUNDED_BUFFERS));
        bool isCopy=strstr(name,"copy")!=nullptr;
        assert(program->bufferMask==(isCopy?7u:3u));
        assert(program->writeMask==(isCopy?2u:1u));
        GPU::Shader::Dispatch dispatch{};dispatch.local[0]=64;dispatch.local[1]=dispatch.local[2]=1;
        dispatch.grid[0]=4224;dispatch.grid[1]=dispatch.grid[2]=1;
        dispatch.groups[0]=66;dispatch.groups[1]=dispatch.groups[2]=1;
        uint64_t elements=0;unsigned dimensions=0;
        assert(GPU::Shader::dispatchFootprint(dispatch,*program,elements,dimensions));
        Adreno::ShaderBuffers bindings{};
        for (unsigned slot=0;slot<3;++slot) if (program->bufferMask&(1u<<slot)) {
            bindings.addresses[slot]=0x4000000+slot*0x100000;bindings.bytes[slot]=32768;
        }
        dispatch.registry=GPUPrograms::Registry;dispatch.version=GPU::Shader::Version;dispatch.program=program->id;
        Adreno::Encoder encoded(words.data(),words.size());Adreno::PipelineEncoder pipeline(encoded,0x6000000);
        assert(pipeline.dispatch(dispatch,*program,bindings)==Result::Success && encoded.barrier()==Result::Success);
    }
}
static void executionProfiling()
{
    Device d;auto *ring=d.allocate(0x3000000,Adreno::RingDwords);
    auto *fence=d.allocate(0x3100000,1024),*ib=d.allocate(0x3200000,Adreno::IBDwords);
    auto *target=d.allocate(0x4000000,1024);
    d.regs[Adreno::RBRead]=d.regs[Adreno::RBWrite]=Adreno::RingDwords-3;
    Adreno::Engine<Device> engine(d);
    assert(engine.start(contract(),ring,0x3000000,fence,0x3100000)==Result::Success);
    d.execute=false;d.regs[0x464*4]=1;d.regs[0x46b*4]=6;
    std::array<Adreno::ExecutionToken,Adreno::ExecutionSlots> tokens{};
    uint32_t sequence=0,completed=0;uint64_t begin=0,end=0;
    for(unsigned i=0;i<tokens.size();++i) {
        Adreno::Encoder command(ib+i*16,16);
        assert(command.fill(0x4000000+i*4,4,i+1)==Result::Success);
        assert(engine.post(0x3200000+i*64,command.size(),sequence,nullptr,&tokens[i])==Result::Success);
        assert(sequence==i+2 && tokens[i].sequence==sequence && tokens[i].slot==i && tokens[i].epoch);
        assert(!engine.executionInterval(tokens[i],begin,end) && !engine.retireExecution(tokens[i]));
    }
    assert(*fence==1 && !target[0]); // all16 are published, none completed by CPU
    const auto writes=d.writes.size();const auto doorbells=d.doorbells,issued=engine.lastIssued();
    Adreno::ExecutionToken retry;
    sequence=0;
    assert(engine.post(0x3200000,4,sequence,nullptr,&retry)==Result::Busy && !sequence && !retry.epoch);
    assert(engine.lastIssued()==issued && d.writes.size()==writes && d.doorbells==doorbells);
    // Stop exactly at the first real completion, leaving15 ring jobs pending.
    for(unsigned packets=0;*fence<tokens[0].sequence;++packets){assert(packets<128);d.run(1);}
    assert(!engine.executionInterval(tokens[0],begin,end)); // poll is mandatory
    assert(engine.poll(completed)==Result::Success && completed==tokens[0].sequence);
    assert(engine.executionInterval(tokens[0],begin,end) && end-begin==19200);
    assert(engine.executionCoreInterval(tokens[0],begin,end) && end-begin==596000);
    assert(engine.executionBusyInterval(tokens[0],begin,end) && end-begin==1192000);
    assert(target[0]==1 && !target[1]);
    assert(!engine.executionInterval(tokens[1],begin,end) && !engine.retireExecution(tokens[1]));
    assert(engine.post(0x3200000,4,sequence,nullptr,&retry)==Result::Busy);
    auto invalid=tokens[0];invalid.slot=Adreno::ExecutionSlots;
    assert(!engine.executionCoreInterval(invalid,begin,end) && !engine.retireExecution(invalid));
    invalid=tokens[0];++invalid.epoch;assert(!engine.executionBusyInterval(invalid,begin,end));
    const auto first=tokens[0];
    assert(engine.retireExecution(first) && !engine.retireExecution(first));
    // Refill only the retired cell while the other15 records stay retained.
    assert(engine.post(0x3200000,4,sequence,nullptr,&retry)==Result::Success && sequence==issued+1 && retry.slot==first.slot);
    assert(!engine.executionInterval(first,begin,end));
    d.run();assert(engine.poll(completed)==Result::Success && completed==sequence);
    for(unsigned i=1;i<tokens.size();++i) {
        assert(target[i]==i+1 && engine.executionInterval(tokens[i],begin,end) && end-begin==19200);
        assert(engine.executionCoreInterval(tokens[i],begin,end) && end-begin==596000);
        assert(engine.executionBusyInterval(tokens[i],begin,end) && end-begin==1192000);
    }
    // A global completion alone cannot authorize a mismatched private cell.
    auto base=(Adreno::ExecutionBase+retry.slot*Adreno::ExecutionStride)/4;
    const auto stamp=fence[base+1];fence[base+1]=stamp-1;
    assert(!engine.executionInterval(retry,begin,end) && !engine.retireExecution(retry));
    fence[base+1]=stamp;fence[base]=stamp+1;
    assert(!engine.executionBusyInterval(retry,begin,end) && !engine.retireExecution(retry));
    fence[base]=stamp;
    assert(engine.executionInterval(retry,begin,end) && engine.retireExecution(retry));
    for(unsigned i=1;i<tokens.size();++i)assert(engine.retireExecution(tokens[i]));
    for(const auto &write:d.writes)assert(write.first!=0xbb0*4 && write.first!=0x464*4 && write.first!=0x46b*4);
    // Restart reuses sequence2 but a prior epoch receipt stays permanently stale.
    assert(engine.stop());d.execute=true;
    assert(engine.start(contract(),ring,0x3000000,fence,0x3100000)==Result::Success);
    Adreno::ExecutionToken restarted;
    assert(engine.post(0x3200000,4,sequence,nullptr,&restarted)==Result::Success && sequence==first.sequence);
    assert(engine.poll(completed)==Result::Success && completed==sequence && restarted.epoch!=first.epoch);
    assert(!engine.executionInterval(first,begin,end) && !engine.retireExecution(first));
    assert(engine.executionInterval(restarted,begin,end));
    // Lost halt acknowledgement keeps all in-flight records and DMA owned.
    d.execute=false;Adreno::ExecutionToken pending;
    assert(engine.post(0x3200000,4,sequence,nullptr,&pending)==Result::Success);
    d.acknowledgeHalt=false;d.regs[Adreno::VBIFHalted]=0;
    assert(!engine.stop() && engine.ownsHardware() && !engine.retireExecution(pending));
    assert(engine.start(contract(),ring,0x3000000,fence,0x3100000)==Result::Invalid);
    d.acknowledgeHalt=true;assert(engine.stop());
    {
        Device large;auto *r=large.allocate(0x3000000,Adreno::RingDwords),*f=large.allocate(0x3100000,1024);
        auto *commands=large.allocate(0x3200000,16);large.allocate(0x4000000,16);
        Adreno::Encoder command(commands,16);assert(command.fill(0x4000000,4,7)==Result::Success);
        Adreno::Engine<Device> many(large);assert(many.start(contract(),r,0x3000000,f,0x3100000)==Result::Success);
        large.execute=false;large.regs[0x464*4]=1;large.regs[0x46b*4]=6;
        Adreno::IndirectBuffer chain[Adreno::MaxIndirectBuffers];
        for(auto &item:chain)item={0x3200000,command.size(),0};
        Adreno::ExecutionToken records[16]{};unsigned accepted=0;
        for(;accepted<16;++accepted) {
            auto result=many.postBatch(chain,Adreno::MaxIndirectBuffers,sequence,nullptr,&records[accepted]);
            if(result==Result::Busy)break;
            assert(result==Result::Success);
        }
        assert(accepted==7 && *f==1 && many.lastIssued()==8); // ring fills before16 profiling slots
        const auto doorbells=large.doorbells,write=large.regs[Adreno::RBWrite];
        sequence=0;
        assert(many.postBatch(chain,Adreno::MaxIndirectBuffers,sequence,nullptr,&records[accepted])==Result::Busy);
        assert(!sequence && !records[accepted].epoch && large.doorbells==doorbells && large.regs[Adreno::RBWrite]==write);
        for(unsigned packets=0;*f<records[0].sequence;++packets){assert(packets<300);large.run(1);}
        assert(many.poll(completed)==Result::Success && completed==records[0].sequence);
        assert(many.postBatch(chain,Adreno::MaxIndirectBuffers,sequence,nullptr,&records[accepted])==Result::Success);
        large.run();assert(many.poll(completed)==Result::Success && completed==sequence);
        for(unsigned i=0;i<=accepted;++i)assert(many.executionInterval(records[i],begin,end) && many.retireExecution(records[i]));
        assert(many.stop());
    }
}
static void invalidHandoffs()
{
    for (unsigned which = 0; which < 11; ++which) {
        Device d; auto *ring = d.allocate(0x3000000, Adreno::RingDwords); auto *fence = d.allocate(0x3100000, 1024);
        Adreno::Engine<Device> engine(d); auto p = contract();
        switch (which) {
            case 0: p.flags &= ~Adreno::PlatformContract::ExclusiveMapper; break;
            case 1: p.reserved = 1; break;
            case 2: d.regs[Adreno::TrustControl] = 1; break;
            case 3: d.regs[Adreno::GPMUReady] = 0; break;
            case 4: d.regs[Adreno::GPMUError] = 1; break;
            case 5: d.regs[Adreno::ContextSwitch] = 1; break;
            case 6: d.regs[Adreno::RBWrite] = 1; break;
            case 7: d.regs[Adreno::MEBase] = 0; break;
            case 8: d.regs[Adreno::MEControl] = 0; break;
            case 9: d.regs[Adreno::Status3] = 1u << 24; break;
            case 10: d.pair(Adreno::RBBase,0x3400000); break;
        }
        assert(engine.start(p, ring, 0x3000000, fence, 0x3100000) != Result::Success);
        assert(d.writes.empty() && !engine.ownsHardware());
    }
    Device d; d.execute = false;
    auto *ring = d.allocate(0x3000000, Adreno::RingDwords); auto *fence = d.allocate(0x3100000, 1024);
    Adreno::Engine<Device> engine(d);
    assert(engine.start(contract(), ring, 0x3000000, fence, 0x3100000) == Result::Timeout);
    assert(*fence == 0 && !engine.ready() && engine.ownsHardware());
    assert(engine.stop() && !engine.ownsHardware());
}
static void corruptCompletion()
{
    for (unsigned which = 0; which < 4; ++which) {
        Device d; auto *ring = d.allocate(0x3000000, Adreno::RingDwords); auto *fence = d.allocate(0x3100000, 1024);
        Adreno::Engine<Device> engine(d);
        assert(engine.start(contract(), ring, 0x3000000, fence, 0x3100000) == Result::Success);
        if (which == 0) *fence = 0; // Cannot rewind a completed timeline.
        if (which == 1) *fence = 2; // Cannot acknowledge an unissued command.
        if (which == 2) d.regs[Adreno::RBRead] = Adreno::RingDwords;
        if (which == 3) ++d.regs[Adreno::RBRead]; // In range, but past producer.
        uint32_t completed;
        assert(engine.poll(completed) == Result::Fault && !engine.ready());
        assert(engine.stop());
    }
}
static void operationProfiling()
{
    constexpr uint64_t base=0x3200000,scratch=0x5400000;
    const auto &cs=*GPUPrograms::lookup(1);
    GPU::Shader::Dispatch dispatch{};dispatch.groups[0]=dispatch.groups[1]=dispatch.groups[2]=1;
    dispatch.local[0]=64;dispatch.local[1]=dispatch.local[2]=1;
    Adreno::ShaderBuffers buffers{};buffers.addresses[0]=0x4000000;buffers.bytes[0]=4096;
    // Successful profiling adds only the two 64-bit counter stores. Every
    // original packet, idle and descriptor/code upload remains byte-identical.
    std::vector<uint32_t> baseline(Adreno::IBDwords),sampled(Adreno::IBDwords);
    Adreno::Encoder plain(baseline.data(),baseline.size()),timed(sampled.data(),sampled.size());
    Adreno::PipelineEncoder a(plain,base,scratch),b(timed,base,scratch);
    assert(a.operation(nullptr,false,[&](Adreno::IndirectBufferAllocator *){return a.dispatch(dispatch,cs,buffers);})==Result::Success);
    b.profileNextOperation(63);
    assert(b.operation(nullptr,false,[&](Adreno::IndirectBufferAllocator *){return b.dispatch(dispatch,cs,buffers);})==Result::Success && b.lastOperationTimed());
    assert(timed.size()==plain.size()+8);
    const auto stamp=Device::header(sampled[0]);assert(stamp.first==0x3e && stamp.second==3);
    assert(sampled[1]==(0x4d2u|(1u<<30)|(2u<<18)) && sampled[2]==scratch+128+63*16 && sampled[3]==0);
    assert(!memcmp(sampled.data()+4,baseline.data(),plain.size()*4));
    assert(sampled[timed.size()-2]==scratch+128+63*16+8);
    assert(plain.checkpoint().capacity==timed.checkpoint().capacity);
    assert(!memcmp(baseline.data()+plain.checkpoint().capacity,sampled.data()+timed.checkpoint().capacity,
        (baseline.size()-plain.checkpoint().capacity)*4));
    b.profileNextOperation(64);
    assert(b.operation(nullptr,false,[&](Adreno::IndirectBufferAllocator *){return timed.barrier();})==Result::Success && !b.lastOperationTimed());
    // A failed end-stamp trial must preserve pre-existing ISA-cache entries,
    // counters and uploads. The ordinary retry must equal never-profiled work.
    std::vector<uint32_t> original;
    for(bool profile:{false,true}) {
        std::vector<uint32_t> words(Adreno::IBDwords);Adreno::Encoder e(words.data(),words.size());
        Adreno::PipelineEncoder p(e,base,scratch);
        assert(p.dispatch(dispatch,cs,buffers)==Result::Success);
        const auto old=e.checkpoint();const auto hits=p.reusedShaderObjects();
        assert(p.dispatch(dispatch,cs,buffers)==Result::Success);const auto needed=e.size()-old.count;
        e.rewind(old);
        // Reset the instrumentation accounting by rebuilding the same prefix.
        e=Adreno::Encoder(words.data(),words.size());Adreno::PipelineEncoder q(e,base,scratch);
        assert(q.dispatch(dispatch,cs,buffers)==Result::Success);
        auto limited=e.checkpoint();limited.reserved=limited.capacity-limited.count-needed-4;e.rewind(limited);
        if(profile)q.profileNextOperation(0);
        assert(q.operation(nullptr,false,[&](Adreno::IndirectBufferAllocator *){return q.dispatch(dispatch,cs,buffers);})==Result::Success);
        assert(!q.lastOperationTimed() && q.reusedShaderObjects()==hits+1);
        words.resize(e.size());
        if(!profile)original=words;else assert(original==words);
    }
    using namespace GPU::OperationActivity;
    auto activity=std::make_unique<Snapshot>();Record r;
    describe(r.description.first,cs);auto changed=cs;changed.id+=999;
    Program identity;describe(identity,changed);assert(identity.hash==r.description.first.hash);
    changed.fullRegs+=1;describe(identity,changed);assert(identity.hash!=r.description.first.hash);
    {
        // Same client-local ID can select a different immutable MSAA/window
        // program. Only the actual program object can reuse its description.
        auto variant=cs;variant.fullRegs+=1;
        ProgramDescriptions descriptions;Program stored[4]{};
        assert(!descriptions.describe(stored[0],cs));
        assert(descriptions.describe(stored[1],cs));
        assert(!memcmp(&stored[0],&stored[1],sizeof(Program)));
        assert(!descriptions.describe(stored[2],variant));
        describe(identity,variant);assert(!memcmp(&identity,&stored[2],sizeof(Program)));
        assert(stored[2].hash!=stored[0].hash && descriptions.describe(stored[3],variant));
        assert(!memcmp(&stored[2],&stored[3],sizeof(Program)));
        // A new encoding pass cannot reuse a deleted/replaced program at the
        // same address. The cache itself ends with the old pass.
        ProgramDescriptions next;variant.fullRegs+=1;
        assert(!next.describe(stored[3],variant));
        describe(identity,variant);assert(!memcmp(&identity,&stored[3],sizeof(Program)));
    }
    r.flags=Timed|Compositor;r.begin=100;r.end=30000;r.description.opcode=GPU::ShaderDispatch;
    activity->append(r,99,30001);assert(activity->buckets[0].native.samples==1 && activity->buckets[0].compositor.ticks==29900);
    r.end=30002;activity->append(r,99,30001);assert(activity->buckets[0].native.invalid==1);
    r.flags=0;activity->append(r,99,30001);assert(activity->buckets[0].native.skipped==1);
    r.flags=Timed;r.end=30000;
    for(unsigned i=0;i<SlowCapacity+5;++i){r.sequence=i+1;activity->append(r,99,30001);}
    assert(activity->slowCount==SlowCapacity && activity->oldest(SlowCapacity-1).sequence==SlowCapacity+5);
    for(unsigned i=1;i<=BucketCapacity;++i){r.description.first.hash=i;activity->append(r,99,30001);}
    assert(activity->count==BucketCapacity && activity->overflow==1);
    r.description.first.hash=1;r.sequence=0x9876;r.begin=100;r.end=2000000;
    activity->append(r,99,2000001);assert(activity->topCount==TopCapacity && activity->top[0].sequence==0x9876);
    for(unsigned i=0;i<SlowCapacity*2;++i){r.sequence=i+1;r.end=30000;activity->append(r,99,30001);}
    assert(activity->top[0].sequence==0x9876 && activity->oldest(SlowCapacity-1).sequence==SlowCapacity*2);
    for(unsigned i=1;i<activity->topCount;++i)assert(activity->top[i-1].end-activity->top[i-1].begin>=activity->top[i].end-activity->top[i].begin);
    puts("GPU operation profiling: exact PM4/tail rollback, immutable ISA cache, bounded counters/invalid intervals/slow ring passed");
}

static void compactOperations()
{
    constexpr uint64_t base=0x3200000,scratch=0x5400000;
    struct Batch : Adreno::IndirectBufferAllocator {
        std::vector<std::vector<uint32_t>> chunks{std::vector<uint32_t>(Adreno::IBDwords)};
        std::vector<uint32_t> sizes;unsigned limit=Adreno::MaxIndirectBuffers;
        Result next(Adreno::Encoder &e,uint64_t &address) override {
            if(chunks.size()>=limit)return Result::NoResources;
            assert(e.reserveTail(0) && e.barrier()==Result::Success);sizes.push_back(e.size());
            chunks.emplace_back(Adreno::IBDwords);address=base+(chunks.size()-1)*Adreno::IBDwords*4;
            e=Adreno::Encoder(chunks.back().data(),Adreno::IBDwords,2);return Result::Success;
        }
    };
    const auto &cs=*GPUPrograms::lookup(1),&vs=*GPUPrograms::lookup(2),&fs=*GPUPrograms::lookup(4);
    GPU::Shader::Dispatch dispatch{};dispatch.groups[0]=dispatch.groups[1]=dispatch.groups[2]=1;
    dispatch.local[0]=64;dispatch.local[1]=dispatch.local[2]=1;
    GPU::Shader::Draw draw{};draw.primitive=3;draw.count=3;draw.instances=1;
    draw.target={1,0,64,64,256,GPU::Shader::RGBA8};draw.textures[0]=draw.target;
    draw.state=GPU::Shader::defaultRenderState(64,64);draw.scissor[2]=draw.scissor[3]=64;
    Adreno::ShaderBuffers buffers{};buffers.addresses[0]=0x4000000;buffers.addresses[1]=0x4100000;
    buffers.bytes[0]=buffers.bytes[1]=4096;
    for(bool direct:{false,true}) {
        std::vector<uint32_t> control;
        for(bool compact:{false,true}) {
            Batch batch;Adreno::Encoder e(batch.chunks[0].data(),Adreno::IBDwords,2);
            Adreno::PipelineEncoder p(e,base,scratch,GPU::PageBytes,direct);
            for(unsigned op=0;op<16;++op) {
                if(op && !compact){auto at=p.address();assert(batch.next(e,at)==Result::Success);p.beginOperation(at);}
                uint32_t frozen[8];for(unsigned n=0;n<8;++n)frozen[n]=0xa0000000+op*32+n;
                assert(p.operation(&batch,compact,[&](Adreno::IndirectBufferAllocator *) {
                    uint32_t offset=0;auto result=e.data(frozen,8,offset);if(result!=Result::Success)return result;
                    result=e.copy(p.address()+offset,0x4700000+op*32,32);if(result!=Result::Success)return result;
                    return op&1?p.draw(draw,vs,fs,buffers,0x4200000,Adreno::GraphicsTextures{{0x4300000},{0}}):p.dispatch(dispatch,cs,buffers);
                })==Result::Success);
                assert(p.operation(&batch,compact,[&](Adreno::IndirectBufferAllocator *) {return p.flushShaderWrites();})==Result::Success);
            }
            assert(e.reserveTail(0) && e.barrier()==Result::Success);batch.sizes.push_back(e.size());
            if(compact)assert(batch.chunks.size()<16);else assert(batch.chunks.size()==16);
            printf("GPU compact IB: direct=%u compact=%u operations=16 chunks=%zu\n",unsigned(direct),unsigned(compact),batch.chunks.size());
            std::vector<uint32_t> semantic;unsigned copies=0,draws=0,dispatches=0;uint32_t token=0;
            // Compare the entire emitted register/packet stream except extra
            // IB-sealing waits and relocated private addresses. Also verify the
            // pointed-to code/data, not merely that relocation numbers differ.
            for(unsigned chunk=0;chunk<batch.chunks.size();++chunk) {
                const auto &words=batch.chunks[chunk];const auto address=base+chunk*Adreno::IBDwords*4;
                for(unsigned i=0;i<batch.sizes[chunk];) {
                    const auto h=words[i++];unsigned n=0,opcode=0;
                    if((h>>28)==4) {
                        n=h&127;const auto reg=(h>>8)&0x3ffff;
                        const GPU::Shader::Program *program=reg==REG_A5XX_SP_CS_OBJ_START_LO?&cs:
                            reg==REG_A5XX_SP_VS_OBJ_START_LO?&vs:reg==REG_A5XX_SP_FS_OBJ_START_LO?&fs:nullptr;
                        if(program) {
                            const auto at=words[i];assert(at>=address+batch.sizes[chunk]*4 && at+program->codeWords*4<=address+Adreno::IBDwords*4);
                            assert(!memcmp(words.data()+(at-address)/4,program->code,program->codeWords*4));
                        }
                    } else {auto packet=Device::header(h);opcode=packet.first;n=packet.second;}
                    assert(i+n<=batch.sizes[chunk]);
                    if(opcode==0x73) {
                        const auto destination=words[i+1],source=words[i+3];
                        assert(destination==0x4700000+copies*4 && words[i+2]==0 && words[i+4]==0);
                        assert(source>=address+batch.sizes[chunk]*4 && source+4<=address+Adreno::IBDwords*4);
                        assert(words[(source-address)/4]==0xa0000000+(copies/8)*32+copies%8);++copies;
                    }
                    if(opcode==0x33)++dispatches;
                    if(opcode==0x38)++draws;
                    if(opcode==0x46 && n==4){assert(words[i+3]>token);token=words[i+3];}
                    if(opcode==0x3c && n==6)assert(words[i+3]==token);
                    if(opcode!=0x12 && opcode!=0x26) {
                        semantic.push_back(h);
                        for(unsigned j=0;j<n;++j) {
                            auto value=words[i+j];
                            if(value>=address && value<address+Adreno::IBDwords*4) {
                                assert(value>=address+batch.sizes[chunk]*4);value=0xfeed0000;
                            }
                            semantic.push_back(value);
                        }
                    }
                    i+=n;
                }
            }
            assert(copies==128 && draws==8 && dispatches==8 && token==p.sequence());
            if(!compact)control=semantic;else assert(semantic==control);
        }
    }
    // Force an upload/command collision after a committed prefix. The trial
    // must leave that prefix and its prior uploaded data intact on success OR
    // allocator rejection; the retry uploads into the new IB, never the first.
    for(bool allow:{false,true}) for(bool profile:{false,true}) {
        Batch batch;batch.limit=allow?2:1;
        Adreno::Encoder e(batch.chunks[0].data(),Adreno::IBDwords,2);Adreno::PipelineEncoder p(e,base,scratch);
        assert(e.fill(0x4800000,4,0x98765432)==Result::Success);
        std::vector<uint32_t> data(Adreno::IBDwords-128,0x51ac51ac);uint32_t offset=0;
        assert(e.data(data.data(),data.size(),offset)==Result::Success);
        const auto point=e.checkpoint();const auto original=batch.chunks[0];unsigned attempts=0;
        if(profile)p.profileNextOperation(0);
        const auto result=p.operation(&batch,true,[&](Adreno::IndirectBufferAllocator *) {
            ++attempts;return p.dispatch(dispatch,cs,buffers);
        });
        assert(result==(allow?Result::Success:Result::NoResources));assert(attempts==(profile?(allow?3u:2u):(allow?2u:1u)));
        assert(p.lastOperationTimed()==(profile && allow));
        assert(!memcmp(original.data(),batch.chunks[0].data(),point.count*4));
        assert(!memcmp(original.data()+offset/4,batch.chunks[0].data()+offset/4,data.size()*4));
        if(allow)assert(p.address()==base+Adreno::IBDwords*4 && batch.chunks.size()==2);
        else {auto after=e.checkpoint();assert(after.count==point.count && after.capacity==point.capacity && after.reserved==point.reserved);}
    }
}
struct RasterObservation {
    std::map<uint32_t,uint32_t> registers;
    std::map<std::pair<uint32_t,uint32_t>,std::map<uint32_t,uint32_t>> state;
    std::vector<uint32_t> draw;
    bool operator==(const RasterObservation &other)const {return registers==other.registers && state==other.state && draw==other.draw;}
};
template<class Words> static std::vector<RasterObservation> rasterObservations(const Words &words,unsigned size) {
    RasterObservation current;std::vector<RasterObservation> draws;
    for(unsigned i=0;i<size;) {
        const uint32_t h=words[i++];
        if((h>>28)==4) {
            const uint32_t reg=(h>>8)&0x3ffff,count=h&127;
            for(unsigned n=0;n<count;++n)current.registers[reg+n]=words[i+n];
            i+=count;continue;
        }
        auto packet=Device::header(h);assert(i+packet.second<=size);
        if(packet.first==0x30 && packet.second>=3 && ((words[i]>>16)&3)==0) {
            const uint32_t block=(words[i]>>18)&15,type=words[i+1]&3,offset=words[i]&0xffff;
            const uint32_t stride=block==8 || block==12?(type==0?32:4):block==14?2:type==0?4:12;
            auto &state=current.state[{block,type}];
            for(unsigned n=3;n<packet.second;++n)state[offset*stride+n-3]=words[i+n];
        }
        if(packet.first==0x38) {
            current.draw.assign(words.begin()+i,words.begin()+i+packet.second);draws.push_back(current);
        }
        i+=packet.second;
    }
    return draws;
}
static void pipelineEncoding()
{
    // Validate packet boundaries, relocations and private code storage only;
    // this host fabric does not pretend to execute the shader ISA/rasterizer.
    std::array<uint32_t, Adreno::IBDwords> words{};
    constexpr uint64_t ib = 0x3200000, scratch = 0x5400000;
    auto check = [&](unsigned size, uint32_t opcode, uint32_t objectReg, const GPU::Shader::Program &program) {
        bool executed = false, located = false;
        for (unsigned i=0;i<size;) {
            uint32_t header = words[i++], type = header>>28;
            if (type == 4) {
                unsigned reg = (header>>8)&0x3ffff, n = header&0x7f;
                assert(n > 0 && i+n <= size);
                assert((__builtin_popcount(reg)+((header>>27)&1))%2 == 1);
                assert((__builtin_popcount(n)+((header>>7)&1))%2 == 1);
                if (reg == objectReg) {
                    uint64_t address = words[i];
                    assert(address >= ib+size*4 && address+program.codeWords*4 <= ib+words.size()*4 && !(address&127));
                    assert(!memcmp(words.data()+(address-ib)/4,program.code,program.codeWords*4));
                    located = true;
                }
                i += n;
            } else {
                auto packet = Device::header(header); assert(i+packet.second <= size);
                if (packet.first == 0x43) assert(packet.second == 3 && words[i] == (1u<<18));
                if (packet.first == opcode) {
                    assert(packet.second == (opcode == 0x33 ? 4 : 3));
                    executed = true;
                }
                i += packet.second;
            }
        }
        assert(executed && located);
    };
    const auto &cs = *GPUPrograms::lookup(1), &vs = *GPUPrograms::lookup(2), &fs = *GPUPrograms::lookup(4);
    {
        auto invalid=cs;invalid.sharedBytes=QGPU_MAX_THREADGROUP_BYTES+4;
        GPU::Shader::Dispatch d{};d.groups[0]=d.groups[1]=d.groups[2]=d.local[0]=d.local[1]=d.local[2]=1;
        uint64_t elements;unsigned dimensions;
        assert(!GPU::Shader::dispatchFootprint(d,invalid,elements,dimensions));
    }
    Adreno::ShaderBuffers buffers{};
    buffers.addresses[0] = 0x4000000; buffers.addresses[1] = 0x4100000;
    buffers.bytes[0] = buffers.bytes[1] = 4096;
    GPU::Shader::Dispatch dispatch{}; dispatch.groups[0] = 1; dispatch.local[0] = 64;
    dispatch.groups[1]=dispatch.groups[2]=dispatch.local[1]=dispatch.local[2]=1;
    Adreno::Encoder compute(words.data(),words.size()); Adreno::PipelineEncoder cp(compute,ib,scratch);
    assert(cp.dispatch(dispatch,cs,buffers) == Result::Success);
    check(compute.size(),0x33,REG_A5XX_SP_CS_OBJ_START_LO,cs);
    {
        // A real flush timestamp and its matching wait must precede the
        // next dispatch, not just an invalidate while writes remain dirty.
        Adreno::Encoder e(words.data(),words.size());Adreno::PipelineEncoder p(e,ib,scratch);
        assert(p.dispatch(dispatch,cs,buffers)==Result::Success);
        assert(p.flushShaderWrites()==Result::Success);
        assert(p.dispatch(dispatch,cs,buffers)==Result::Success);
        assert(p.flushShaderWrites()==Result::Success);
        // Repeated ISA references must use the first owned copy, while both
        // state reloads and physical write barriers still appear in PM4.
        assert(p.reusedShaderObjects()==1 && p.avoidedShaderObjectBytes()==cs.codeWords*4);
        uint64_t shaderAddress=0;unsigned shaderReferences=0;
        for(unsigned i=0;i<e.size();) {
            const auto h=words[i++];
            if((h>>28)==4) {
                if(((h>>8)&0x3ffff)==REG_A5XX_SP_CS_OBJ_START_LO) {
                    if(!shaderReferences)shaderAddress=words[i];else assert(shaderAddress==words[i]);
                    ++shaderReferences;
                }
                i+=h&127;
            } else i+=Device::header(h).second;
        }
        assert(shaderReferences==2 && shaderAddress>=ib+e.size()*4 &&
            !memcmp(words.data()+(shaderAddress-ib)/4,cs.code,cs.codeWords*4));
        unsigned dispatches=0,flushes=0,waits=0;uint32_t token=0;bool dirty=false;
        for(unsigned i=0;i<e.size();) {
            const auto h=words[i++];
            if((h>>28)==4){i+=h&127;continue;}
            const auto packet=Device::header(h);
            if(packet.first==0x33){assert(!dirty);dirty=true;++dispatches;}
            if(packet.first==0x46 && packet.second==4 && words[i]==4) {
                assert(dirty && words[i+1]==uint32_t(scratch+16) && words[i+2]==uint32_t((scratch+16)>>32));
                assert(words[i+3]>token);token=words[i+3];++flushes;
            }
            if(packet.first==0x3c){assert(token && words[i+1]==uint32_t(scratch+16) && words[i+3]==token);dirty=false;++waits;}
            i+=packet.second;
        }
        assert(dispatches==2 && flushes==2 && waits==2 && !dirty && p.sequence()==2);
        Adreno::Encoder denied(words.data(),words.size());Adreno::PipelineEncoder noPage(denied,ib,0,0);
        assert(noPage.flushShaderWrites()==Result::Invalid && !denied.size());
    }
    GPU::Shader::Draw draw{}; draw.primitive = 3; draw.count = 3; draw.instances = 1;
    draw.target = {1,0,64,64,256,GPU::Shader::RGBA8}; draw.textures[0] = draw.target;
    draw.state = GPU::Shader::defaultRenderState(64,64);
    draw.scissor[2] = draw.scissor[3] = 64;
    uint64_t bytes;
    assert(GPU::Shader::textureSize(draw.target,bytes) && bytes == 16384);
    auto bad = draw.target; bad.pitch = 4; assert(!GPU::Shader::textureSize(bad,bytes));
    bad = draw.target; bad.width = 1025; assert(!GPU::Shader::textureSize(bad,bytes));
    Adreno::Encoder render(words.data(),words.size()); Adreno::PipelineEncoder rp(render,ib,scratch);
    assert(rp.draw(draw,vs,fs,buffers,0x4200000,Adreno::GraphicsTextures{{0x4300000},{0}}) == Result::Success);
    check(render.size(),0x38,REG_A5XX_SP_FS_OBJ_START_LO,fs);
    {
        const auto originalWords=words;
        using namespace GPU::Shader;const Program *copy=nullptr,*fetch=nullptr;
        for(const auto &program:GPUPrograms::Programs) {
            if(!strcmp(program.name,"framebuffer_snapshot"))copy=&program;
            if(!strcmp(program.name,"framebuffer_source_over"))fetch=&program;
        }
        assert(copy && fetch && fetch->framebufferMask==1 && fetch->textureCount==1);
        assert(qgpu_framebuffer_slot(8,0,5,0)==0 && qgpu_framebuffer_slot(8,0,5,2)==1);
        assert(qgpu_framebuffer_slot(255,255,1,0)==UINT32_MAX);
        {
            auto job=draw;job.target={1,0,1080,1920,4352,RGBA8};
            job.state=defaultRenderState(1080,1920);job.scissor[2]=1080;job.scissor[3]=1920;
            for(auto &texture:job.textures)texture={};
            Adreno::FramebufferStorage plan,control,explicitSnapshot;
            assert(Adreno::framebufferStoragePlan(job,*fetch,true,false,plan) && plan.inplace && plan.bytes==4096);
            assert(Adreno::framebufferStoragePlan(job,*fetch,false,false,control) && !control.inplace);
            assert(control.bytes==8359936 && control.bytes-plan.bytes==8355840);
            Adreno::FramebufferStorage fullInplace;
            assert(Adreno::framebufferStoragePlan(job,*fetch,true,false,fullInplace,false) && fullInplace.inplace && fullInplace.bytes==control.bytes);
            assert(Adreno::framebufferStoragePlan(job,*fetch,true,true,explicitSnapshot) && !explicitSnapshot.inplace && explicitSnapshot.bytes==control.bytes);
            auto malformed=job;malformed.target.pitch=4;
            assert(!Adreno::framebufferStoragePlan(malformed,*fetch,true,false,plan) && !plan.bytes);
            auto bindings=buffers;bindings.addresses[QGPU_FRAGMENT_BUFFER_BASE]=0x4600000;bindings.bytes[QGPU_FRAGMENT_BUFFER_BASE]=16;
            auto encode=[&](const Draw &d,bool inplace,uint32_t bytes,bool primitiveSnapshot=false,bool compact=true) {
                Adreno::Encoder e(words.data(),words.size());Adreno::PipelineEncoder p(e,ib,scratch,bytes);
                p.enableFramebufferInplace(inplace);p.enableFramebufferCompact(compact);
                return p.draw(d,vs,*fetch,bindings,0x4200000,{},0,0,0,nullptr,copy,nullptr,primitiveSnapshot);
            };
            assert(encode(job,true,GPU::PageBytes)==Result::Success);
            assert(encode(job,true,GPU::PageBytes-1)==Result::Invalid);
            assert(encode(job,false,GPU::PageBytes)==Result::Invalid);
            assert(encode(job,true,GPU::PageBytes,true)==Result::Invalid);
            assert(encode(job,true,GPU::PageBytes,false,false)==Result::Invalid);
            assert(encode(job,true,fullInplace.bytes,false,false)==Result::Success);
            auto fallback=job;fallback.target={1,0,16,16,64,R8};fallback.state=defaultRenderState(16,16);
            fallback.scissor[2]=fallback.scissor[3]=16;
            Adreno::FramebufferStorage small;
            assert(Adreno::framebufferStoragePlan(fallback,*fetch,true,false,small) && !small.inplace && small.bytes==8192);
            assert(encode(fallback,true,GPU::PageBytes)==Result::Invalid && encode(fallback,true,small.bytes)==Result::Success);
            // A mixed batch must keep the maximum actual requirement, even
            // when its largest color target is an in-place draw.
            assert(Adreno::framebufferStoragePlan(job,*fetch,true,false,plan));
            const auto mixedBytes=std::max(plan.bytes,small.bytes);
            assert(mixedBytes==8192 && encode(job,true,mixedBytes)==Result::Success && encode(fallback,true,mixedBytes)==Result::Success);
        }
        for(uint32_t format:{uint32_t(RGBA8),uint32_t(BGRA8),uint32_t(RGBA8SRGB),uint32_t(BGRA8SRGB),uint32_t(R8)})for(bool enabled:{false,true}) {
            auto job=draw;job.target={1,0,16,16,format==R8?64u:256u,format};
            job.state=defaultRenderState(16,16);job.scissor[2]=job.scissor[3]=16;job.count=6;job.instances=2;
            for(auto &texture:job.textures)texture={};
            const bool inplace=enabled && format!=R8;
            assert(Adreno::framebufferInplaceEligible(job,*fetch)==(format!=R8));
            auto bad=job;bad.rasterSamples=2;assert(!Adreno::framebufferInplaceEligible(bad,*fetch));
            bad=job;bad.target.samples=2;assert(!Adreno::framebufferInplaceEligible(bad,*fetch));
            bad=job;bad.primitive=1;assert(!Adreno::framebufferInplaceEligible(bad,*fetch));
            auto other=*fetch;other.framebufferMask=3;assert(!Adreno::framebufferInplaceEligible(job,other));
            other=*fetch;other.colorMask=3;assert(!Adreno::framebufferInplaceEligible(job,other));
            FramebufferLayout layout;assert(framebufferLayout(job,*fetch,layout));
            Adreno::FramebufferStorage storage;
            assert(Adreno::framebufferStoragePlan(job,*fetch,enabled,false,storage));
            assert(storage.inplace==inplace && storage.bytes==(inplace?GPU::PageBytes:layout.bytes));
            auto bindings=buffers;bindings.bytes[0]=bindings.bytes[1]=1<<20;
            bindings.addresses[QGPU_FRAGMENT_BUFFER_BASE]=0x4600000;bindings.bytes[QGPU_FRAGMENT_BUFFER_BASE]=16;
            const auto original=words;
            Adreno::Encoder full(words.data(),words.size());Adreno::PipelineEncoder previous(full,ib,scratch,layout.bytes);
            previous.enableFramebufferInplace(enabled);previous.enableFramebufferCompact(false);
            assert(previous.draw(job,vs,*fetch,bindings,0x4200000,{},0,0,0,nullptr,copy)==Result::Success);
            const auto originalStream=words;const auto originalSize=full.size();words=original;
            Adreno::Encoder e(words.data(),words.size());Adreno::PipelineEncoder p(e,ib,scratch,storage.bytes);
            p.enableFramebufferInplace(enabled);
            constexpr uint64_t target=0x4200000;
            assert(p.draw(job,vs,*fetch,bindings,target,{},0,0,0,nullptr,copy)==Result::Success);
            assert(e.size()==originalSize && words==originalStream); // storage reduction must not change PM4/code
            unsigned draws=0,copies=0,lastDraw=0,colorWait=0,ucheWait=0,invalidate=0,ucheFlushes=0;
            uint32_t event=0,sequence=0,previousSequence=0;uint64_t cell=0,view=0;
            for(unsigned i=0;i<e.size();) {
                const auto h=words[i++];
                if((h>>28)==4) {
                    if(((h>>8)&0x3ffff)==REG_A5XX_UCHE_CACHE_INVALIDATE && words[i]==0x12)invalidate=i;
                    i+=h&127;continue;
                }
                const auto packet=Device::header(h);
                if(packet.first==0x46 && packet.second==4) {
                    event=words[i];cell=uint64_t(words[i+1])|(uint64_t(words[i+2])<<32);sequence=words[i+3];
                    if(event==4)++ucheFlushes;
                    assert(sequence>previousSequence && cell>=scratch && cell+8<=scratch+128);previousSequence=sequence;
                }
                if(packet.first==0x3c) {
                    assert(packet.second==6 && words[i]==0x13 && words[i+3]==sequence && words[i+4]==UINT32_MAX);
                    assert((uint64_t(words[i+1])|(uint64_t(words[i+2])<<32))==cell);
                    if(event==29)colorWait=i;
                    if(event==4)ucheWait=i;
                }
                if(packet.first==0x30 && packet.second==15 && ((words[i]>>18)&15)==4) {
                    assert((words[i]&0xffff)==qgpu_framebuffer_slot(fetch->sampleMask,fetch->fetchMask,fetch->framebufferMask,0));
                    view=uint64_t(words[i+7])|(uint64_t(words[i+8]&A5XX_TEX_CONST_5_BASE_HI__MASK)<<32);
                }
                if(packet.first==0x33 || packet.first==0x2c)++copies;
                if(packet.first==0x38) {
                    assert(words[i+1]==1 && words[i+2]==3);
                    assert(colorWait>lastDraw && ucheWait>lastDraw && invalidate>lastDraw);
                    assert(view==(inplace?target:scratch+layout.offsets[0]));
                    lastDraw=i;++draws;
                }
                i+=packet.second;
            }
            assert(draws==4 && copies==(inplace?0u:4u));
            if(inplace)assert(ucheFlushes==draws+1); // initial drain, then one after each real primitive
            const auto observed=rasterObservations(words,e.size());
            std::vector<uint32_t> baseline(words.size());Adreno::Encoder be(baseline.data(),baseline.size());
            Adreno::PipelineEncoder bp(be,ib,scratch,storage.bytes);bp.enableFramebufferInplace(enabled);bp.enableFramebufferStateReuse(false);
            assert(bp.draw(job,vs,*fetch,bindings,target,{},0,0,0,nullptr,copy)==Result::Success);
            assert(observed==rasterObservations(baseline,be.size()));
            assert(p.fullRasterSetups()==(inplace?1u:4u) && p.reusedRasterPrimitives()==(inplace?3u:0u));
            if(inplace)assert(e.size()*2<be.size()); // real state/code packets, not just ISA backing copies
            assert(p.inplaceFramebufferPrimitives()==(inplace?4u:0u) && p.framebufferPrimitives()==(inplace?0u:4u));
            assert(p.copiedFramebufferBytes()==(inplace?0u:uint64_t(job.target.pitch)*job.target.height*4));
        }
        // A different read-only MRT plane is immutable for the whole draw.
        // Verify original descriptor addresses, one unsplit draw, publication
        // waits and zero writes to that plane for both 32- and 64-bit formats.
        for(uint32_t format:{uint32_t(RGBA8),uint32_t(RGBA16Float),uint32_t(RG32Float)})for(bool enabled:{false,true}) {
            auto shader=*fetch;shader.framebufferMask=2;assert(shader.colorMask==1);
            auto job=draw;job.target={1,0,1040,67,4352,RGBA8};
            job.colorTargets[0]={2,0,1040,67,formatBytes(format)==8?8448u:4352u,format};
            job.colorStates[0].writeMask=15; // absence of an FS output is authoritative
            job.state=defaultRenderState(1040,67);job.scissor[0]=7;job.scissor[1]=3;
            job.scissor[2]=1011;job.scissor[3]=61;job.count=6;job.instances=2;
            for(auto &texture:job.textures)texture={};
            assert(Adreno::framebufferReadOnlyEligible(job,shader));
            auto writes=shader;writes.colorMask=3;assert(!Adreno::framebufferReadOnlyEligible(job,writes));
            auto ms=job;ms.rasterSamples=2;assert(!Adreno::framebufferReadOnlyEligible(ms,shader));
            Adreno::FramebufferStorage storage,forced;
            assert(Adreno::framebufferStoragePlan(job,shader,enabled,false,storage) && storage.readOnly==enabled && !storage.inplace);
            assert(storage.bytes==(enabled?GPU::PageBytes:storage.snapshots.bytes));
            assert(Adreno::framebufferStoragePlan(job,shader,true,true,forced) && !forced.readOnly);
            auto bindings=buffers;bindings.addresses[QGPU_FRAGMENT_BUFFER_BASE]=0x4600000;bindings.bytes[QGPU_FRAGMENT_BUFFER_BASE]=16;
            uint64_t additional[QGPU_COLOR_TARGET_COUNT-1]={0x4800000};
            Adreno::Encoder e(words.data(),words.size());Adreno::PipelineEncoder p(e,ib,scratch,storage.bytes);
            p.enableFramebufferInplace(enabled);
            assert(p.draw(job,vs,shader,bindings,0x4200000,{},0,0,0,additional,copy)==Result::Success);
            unsigned draws=0,copies=0,colorWait=0,ucheWait=0,invalidate=0,event=0;
            uint64_t view=0;uint32_t components=0;
            for(unsigned i=0;i<e.size();) {
                auto h=words[i++];
                if((h>>28)==4) {
                    const auto reg=(h>>8)&0x3ffff;
                    if(reg==REG_A5XX_RB_RENDER_COMPONENTS)components=words[i];
                    if(reg==REG_A5XX_UCHE_CACHE_INVALIDATE && words[i]==0x12)invalidate=i;
                    i+=h&127;continue;
                }
                const auto packet=Device::header(h);
                if(packet.first==0x46 && packet.second==4)event=words[i];
                if(packet.first==0x3c){if(event==29)colorWait=i;if(event==4)ucheWait=i;}
                if(packet.first==0x30 && packet.second==15 && ((words[i]>>18)&15)==4)
                    view=uint64_t(words[i+7])|(uint64_t(words[i+8]&A5XX_TEX_CONST_5_BASE_HI__MASK)<<32);
                if(packet.first==0x33 || packet.first==0x2c)++copies;
                if(packet.first==0x38) {
                    assert(colorWait && ucheWait && invalidate && components==15);
                    assert(words[i+1]==(enabled?2u:1u) && words[i+2]==(enabled?6u:3u));
                    assert(view==(enabled?additional[0]:scratch+storage.snapshots.offsets[1]));++draws;
                }
                i+=packet.second;
            }
            assert(draws==(enabled?1u:4u) && copies==(enabled?0u:4u));
            assert(p.copiedFramebufferBytes()==(enabled?0u:4u*job.colorTargets[0].pitch*62u));
        }
        // Compare effective register and LOAD_STATE contents at EVERY draw,
        // including strip winding/restart, rebased indices, instance IDs and
        // adjacent immutable words in a partially updated constant vec4.
        // This models PM4 state, not shader execution; hardware pixels are
        // verified separately by metalctl render-state-test.
        for(unsigned indexSize:{0u,2u,4u})for(bool direct:{false,true})for(bool unaligned:{false,true}) {
            auto vertex=*GPUPrograms::lookup(5);
            const uint32_t adjacentImmediates[]={0x12345678,0x87654321};
            if(unaligned) {
                vertex.driverBase=(vertex.driverBase&~3u)+1;vertex.driverCount=5;
                vertex.immediateBase=vertex.driverBase+5;vertex.immediateCount=2;
                vertex.immediates=adjacentImmediates;
                vertex.constlen=std::max(unsigned(vertex.constlen),((vertex.immediateBase+2+15)/16)*4);
            }
            auto job=draw;job.target={1,0,16,16,256,RGBA8};job.state=defaultRenderState(16,16);
            job.state.cull=CullBack;job.primitive=4;job.instances=2;job.baseInstance=3;job.start=indexSize?0:7;
            job.scissor[2]=job.scissor[3]=16;for(auto &texture:job.textures)texture={};
            uint16_t narrow[]={0,1,2,0xffff,3,4,5};uint32_t wide[]={0,1,2,UINT32_MAX,3,4,5};
            job.indexSize=indexSize;job.count=indexSize?7:4;job.baseVertex=indexSize?-2:0;
            job.indices=indexSize?Binding{2,0,job.count*indexSize,0}:Binding{};
            const void *snapshot=indexSize==2?static_cast<void *>(narrow):static_cast<void *>(wide);
            auto bindings=buffers;bindings.bytes[0]=bindings.bytes[1]=1<<20;
            bindings.addresses[QGPU_FRAGMENT_BUFFER_BASE]=0x4600000;bindings.bytes[QGPU_FRAGMENT_BUFFER_BASE]=16;
            std::vector<uint32_t> full(Adreno::IBDwords),fast(Adreno::IBDwords);
            Adreno::Encoder fe(full.data(),full.size()),re(fast.data(),fast.size());
            Adreno::PipelineEncoder fp(fe,ib,scratch,GPU::PageBytes,direct),rp(re,ib,scratch,GPU::PageBytes,direct);
            fp.enableFramebufferInplace();fp.enableFramebufferStateReuse(false);rp.enableFramebufferInplace();
            for(unsigned phase=0;phase<2;++phase) {
                job.baseInstance+=5;job.state.winding^=1;job.scissor[0]=phase;job.scissor[2]=16-phase;
                const uint64_t indices=indexSize?0x4800000:0,target=0x4200000+phase*0x10000;
                assert(fp.draw(job,vertex,*fetch,bindings,target,{},indices,0,0,nullptr,copy,snapshot)==Result::Success);
                assert(rp.draw(job,vertex,*fetch,bindings,target,{},indices,0,0,nullptr,copy,snapshot)==Result::Success);
            }
            const auto reference=rasterObservations(full,fe.size()),optimized=rasterObservations(fast,re.size());
            assert(reference.size()==8 && reference==optimized);
            assert(rp.fullRasterSetups()==2 && rp.reusedRasterPrimitives()==6 && rp.updatedVertexDriverConstants()==6);
        }
        // Partition a byte copy into exact, non-overfetching R8 rectangles.
        // Include tiny spans, both sides of the4KiB split, padded render
        // targets and the maximum mapped native buffer.
        for(uint32_t bytes:{64u,128u,4032u,4096u,4160u,4352u*67u,uint32_t(GPU::MaxBufferBytes)}) {
            Adreno::Framebuffer2DRegion rectangles[Adreno::Framebuffer2DRegionCount];
            assert(Adreno::framebuffer2DRegions(0x4200000,0x6200000,bytes,rectangles));
            uint32_t end=0;
            for(const auto &r:rectangles)if(r.rows) {
                assert(r.offset==end && r.pitch && !(r.pitch&63) && r.pitch<16384 && r.rows<=1008);
                end+=r.pitch*r.rows;assert(end<=bytes);
            }
            if(bytes<=16320)assert(end==bytes);
            else assert(bytes-end>=65536 && bytes-end<131072 && !(end&65535));
        }
        for(auto span:{0u,63u,65u,16384u,65536u,65536u+64,uint32_t(GPU::MaxBufferBytes)+64}) {
            Adreno::Framebuffer2DRegion rectangles[Adreno::Framebuffer2DRegionCount];assert(!Adreno::framebuffer2DRegions(0x4200000,0x6200000,span,rectangles));
        }
        {
            Adreno::Framebuffer2DRegion rectangles[Adreno::Framebuffer2DRegionCount];
            assert(!Adreno::framebuffer2DRegions(0x4200004,0x6200000,4096,rectangles));
            assert(!Adreno::framebuffer2DRegions(0x4200000,0x4200040,4096,rectangles));
            assert(!Adreno::framebuffer2DRegions((1ULL<<Adreno::AddressBits)-64,0x6200000,128,rectangles));
        }
        for(unsigned mode:{0u,1u,2u}) for(bool compact:{false,true}) {
            const bool use2D=mode==1,inplace=mode==2;
            struct Batch : Adreno::IndirectBufferAllocator {
                std::vector<std::vector<uint32_t>> chunks{std::vector<uint32_t>(Adreno::IBDwords)};
                std::vector<uint32_t> sizes;unsigned limit=Adreno::MaxIndirectBuffers;
                Result next(Adreno::Encoder &e,uint64_t &address) override {
                    if(chunks.size()>=limit)return Result::NoResources;
                    assert(e.reserveTail(0) && e.barrier()==Result::Success);sizes.push_back(e.size());
                    chunks.emplace_back(Adreno::IBDwords);address+=Adreno::IBDwords*4;
                    e=Adreno::Encoder(chunks.back().data(),Adreno::IBDwords,2);return Result::Success;
                }
            } batch;
            auto job=draw;job.count=3*192;job.instances=2;job.baseInstance=5;job.target={1,0,16,16,inplace?256u:64u,inplace?RGBA8:R8};
            job.state=defaultRenderState(16,16);job.scissor[2]=job.scissor[3]=16;for(auto &texture:job.textures)texture={};
            auto bindings=buffers;bindings.bytes[0]=bindings.bytes[1]=1<<20;
            bindings.addresses[QGPU_FRAGMENT_BUFFER_BASE]=0x4600000;bindings.bytes[QGPU_FRAGMENT_BUFFER_BASE]=16;
            const auto &instanced=*GPUPrograms::lookup(5);FramebufferLayout layout;assert(framebufferLayout(job,*fetch,layout));
            Adreno::Encoder e(batch.chunks[0].data(),Adreno::IBDwords,2);Adreno::PipelineEncoder p(e,ib,scratch,inplace?GPU::PageBytes:layout.bytes);
            p.enableFramebuffer2D(use2D);
            p.enableFramebufferInplace(inplace);
            assert(p.operation(&batch,compact,[&](Adreno::IndirectBufferAllocator *chunks) {
                return p.draw(job,instanced,*fetch,bindings,0x4200000,{},0,0,0,nullptr,copy,nullptr,false,chunks);
            })==Result::Success);
            assert(e.reserveTail(0) && e.barrier()==Result::Success);batch.sizes.push_back(e.size());assert(batch.chunks.size()>1);
            unsigned primitives=0,copies=0,sequence=0,start=0,instance=0;
            for(unsigned chunk=0;chunk<batch.chunks.size();++chunk)for(unsigned i=0;i<batch.sizes[chunk];) {
                const auto &data=batch.chunks[chunk];uint32_t h=data[i++];if((h>>28)==4) {
                    if(((h>>8)&0x3ffff)==REG_A5XX_SP_VS_OBJ_START_LO) {
                        uint64_t at=data[i],base=ib+uint64_t(chunk)*Adreno::IBDwords*4;
                        assert(at>=base+batch.sizes[chunk]*4 && at+instanced.codeWords*4<=base+Adreno::IBDwords*4);
                        assert(!memcmp(data.data()+(at-base)/4,instanced.code,instanced.codeWords*4));
                    }
                    i+=h&127;continue;
                }
                auto packet=Device::header(h);
                if(packet.first==0x30 && ((data[i]>>18)&15)==8 && data[i+1]==1) {
                    const uint32_t base=(data[i]&0xffff)*4,words=packet.second-3;
                    if(instanced.driverBase+1>=base && instanced.driverBase+2<base+words) {
                        start=data[i+3+instanced.driverBase+1-base];instance=data[i+3+instanced.driverBase+2-base];
                    }
                }
                if(packet.first==(use2D?0x2cu:0x33u))++copies;
                if(packet.first==0x38){assert(start==(primitives%192)*3 && instance==5+primitives/192 && data[i+1]==1 && data[i+2]==3);++primitives;}
                if(packet.first==0x46 && packet.second==4){assert(data[i+3]>sequence);sequence=data[i+3];}
                i+=packet.second;
            }
            assert(primitives==384 && copies==(inplace?0u:primitives) && sequence==p.sequence());
            assert(p.framebufferPrimitives()==(inplace?0u:384u) && p.copiedFramebufferBytes()==(inplace?0u:384*1024u));
            assert(p.inplaceFramebufferPrimitives()==(inplace?384u:0u));
            assert(p.copiedFramebuffer2DBytes()==(use2D?384*1024u:0));
            // A second draw uses a new IB but retains the same completion
            // cells. No earlier event value may satisfy its waits.
            uint64_t nextAddress=p.address();assert(batch.next(e,nextAddress)==Result::Success);
            p.beginOperation(nextAddress);job.count=3;job.instances=1;
            assert(p.draw(job,instanced,*fetch,bindings,0x4200000,{},0,0,0,nullptr,copy,nullptr,false,&batch)==Result::Success);
            const auto &last=batch.chunks.back();bool relocated=false,flushed=false;
            for(unsigned i=0;i<e.size();) {
                uint32_t h=last[i++];if((h>>28)==4) {
                    if(((h>>8)&0x3ffff)==REG_A5XX_SP_VS_OBJ_START_LO) {
                        uint64_t at=last[i];assert(at>=nextAddress+e.size()*4 && at+instanced.codeWords*4<=nextAddress+Adreno::IBDwords*4);
                        assert(!memcmp(last.data()+(at-nextAddress)/4,instanced.code,instanced.codeWords*4));relocated=true;
                    }
                    i+=h&127;continue;
                }
                auto packet=Device::header(h);
                if(packet.first==0x46 && packet.second==4){assert(last[i+3]>sequence);sequence=last[i+3];flushed=true;}
                if(packet.first==0x3c && packet.second==6)assert(last[i+3]==sequence);
                i+=packet.second;
            }
            assert(relocated && flushed && sequence==p.sequence());
            assert(p.framebufferPrimitives()==(inplace?0u:385u) && p.copiedFramebufferBytes()==(inplace?0u:385*1024u));
            assert(p.inplaceFramebufferPrimitives()==(inplace?385u:0u));job.count=3*192;job.instances=2;
            assert(p.copiedFramebuffer2DBytes()==(use2D?385*1024u:0));
            Batch denied;denied.limit=1;Adreno::Encoder small(denied.chunks[0].data(),Adreno::IBDwords,2);
            Adreno::PipelineEncoder rejected(small,ib,scratch,inplace?GPU::PageBytes:layout.bytes);
            rejected.enableFramebuffer2D(use2D);
            rejected.enableFramebufferInplace(inplace);
            assert(rejected.draw(job,instanced,*fetch,bindings,0x4200000,{},0,0,0,nullptr,copy,nullptr,false,&denied)==Result::NoResources && denied.sizes.empty());
        }
        for(bool word32:{false,true}) {
            auto job=draw;job.target={1,0,1040,67,4352,RGBA8};job.instances=2;
            job.state=defaultRenderState(1040,67);job.scissor[0]=7;job.scissor[1]=3;job.scissor[2]=1019;job.scissor[3]=61;
            for(auto &texture:job.textures)texture={};
            FramebufferLayout layout;assert(framebufferLayout(job,*fetch,layout));
            Adreno::FramebufferCopyRegion region;assert(Adreno::framebufferCopyRegion(job,job.target,region));
            assert(region.offset==8704 && region.bytes==269824);
            auto bindings=buffers;bindings.addresses[QGPU_FRAGMENT_BUFFER_BASE]=0x4600000;bindings.bytes[QGPU_FRAGMENT_BUFFER_BASE]=16;
            Adreno::Encoder e(words.data(),words.size());Adreno::PipelineEncoder p(e,ib,scratch,layout.bytes);p.enableFramebuffer2D();
            p.enableFramebufferWord32(word32);
            constexpr uint64_t target=0x4200040;
            assert(p.draw(job,vs,*fetch,bindings,target,{},0,0,0,nullptr,copy)==Result::Success);
            std::map<uint32_t,uint32_t> registers;uint32_t mode=0,copied=0,blits=0,draws=0,computes=0;bool colorFlushed=false,invalidated=false;
            uint64_t tailSource=0,tailDestination=0;
            std::vector<uint8_t> source(job.target.pitch*job.target.height),snapshot(source.size(),0xa5);
            for(unsigned i=0;i<source.size();++i)source[i]=uint8_t(i*17+i/4096);
            for(unsigned i=0;i<e.size();) {
                const uint32_t h=words[i++];
                if((h>>28)==4) {
                    const auto start=(h>>8)&0x3ffff;
                    for(unsigned n=0;n<(h&127);++n)registers[start+n]=words[i+n];
                    i+=h&127;continue;
                }
                const auto packet=Device::header(h);
                if(packet.first==0x6c)mode=words[i];
                if(packet.first==0x30 && packet.second==35 && ((words[i]>>18)&15)==15 && words[i+1]==2) {
                    tailSource=uint64_t(words[i+3])|(uint64_t(words[i+4])<<32);
                    tailDestination=uint64_t(words[i+5])|(uint64_t(words[i+6])<<32);
                }
                if(packet.first==0x33) {
                    assert(copied==196608 && colorFlushed && tailSource==target+region.offset+copied &&
                        tailDestination==scratch+layout.offsets[0]+region.offset+copied);
                    memcpy(snapshot.data()+region.offset+copied,source.data()+region.offset+copied,region.bytes-copied);
                    copied=region.bytes;++computes;
                }
                if(packet.first==0x2c) {
                    assert(mode==5 && packet.second==5 && words[i]==1 && !words[i+1] && !words[i+3]);
                    const auto src=uint64_t(registers[REG_A5XX_RB_2D_SRC_LO])|(uint64_t(registers[REG_A5XX_RB_2D_SRC_HI])<<32);
                    const auto dst=uint64_t(registers[REG_A5XX_RB_2D_DST_LO])|(uint64_t(registers[REG_A5XX_RB_2D_DST_HI])<<32);
                    assert(src==target+region.offset+copied && dst==scratch+layout.offsets[0]+region.offset+copied);
                    assert(registers[REG_A5XX_RB_2D_SRC_INFO]==(word32?75u:3u) && registers[REG_A5XX_RB_2D_DST_INFO]==(word32?75u:3u));
                    const uint32_t pitch=(registers[REG_A5XX_RB_2D_SRC_SIZE]&0xffff)*64;
                    assert(registers[REG_A5XX_RB_2D_DST_SIZE]==registers[REG_A5XX_RB_2D_SRC_SIZE]);
                    assert(words[i+2]==words[i+4]);
                    const auto width=((words[i+2]&0x3fff)+1)*(word32?4:1),rows=((words[i+2]>>16)&0x3fff)+1;
                    assert(width==pitch && width && rows && width*rows<=region.bytes-copied);
                    assert((registers[REG_A5XX_RB_2D_SRC_SIZE]>>16)==width*rows/64);
                    const auto bytes=width*rows;memcpy(snapshot.data()+region.offset+copied,source.data()+region.offset+copied,bytes);
                    copied+=bytes;++blits;colorFlushed=invalidated=false;
                }
                if(packet.first==0x46 && packet.second==4 && copied) {
                    if(words[i]==29){assert(mode==8);colorFlushed=true;}
                    if(words[i]==4){assert(colorFlushed);invalidated=true;}
                }
                if(packet.first==0x38) {
                    assert(mode==1 && copied==region.bytes && colorFlushed && invalidated);
                    assert(registers[REG_A5XX_SP_ADDR_MODE_CNTL]==1 && registers[REG_A5XX_RB_MODE_CNTL]==0x44);
                    assert(registers[REG_A5XX_SP_MODE_CNTL]==0x1e && registers[REG_A5XX_TPL1_MODE_CNTL]==0x544 && registers[REG_A5XX_HLSQ_MODE_CNTL]==1);
                    for(unsigned byte=0;byte<snapshot.size();++byte)
                        assert(snapshot[byte]==(byte>=region.offset && byte-region.offset<region.bytes?source[byte]:uint8_t(0xa5)));
                    for(auto &byte:source)byte^=0x39; // arbitrary preceding primitive's new destination
                    copied=0;++draws;
                }
                i+=packet.second;
            }
            assert(blits==2 && computes==2 && draws==2 && p.copiedFramebuffer2DBytes()==196608u*2);
        }
        for(uint32_t format:{uint32_t(RGBA8),uint32_t(R8),uint32_t(RG8)})for(uint32_t left:{1u,361u,1059u})for(bool word32:{false,true}) {
            // Replay the actual pitched2D packets plus compute tail into a
            // nonuniform image. Verify helper pixels, untouched columns,
            // row padding and per-instance preceding destination visibility.
            auto job=draw;const uint32_t cpp=formatBytes(format);
            job.target={1,0,1080,1920,(1080*cpp+255)&~255u,format};job.instances=2;
            job.state=defaultRenderState(1080,1920);job.scissor[0]=left;job.scissor[1]=3;job.scissor[2]=13;job.scissor[3]=1914;
            for(auto &texture:job.textures)texture={};
            FramebufferLayout layout;assert(framebufferLayout(job,*fetch,layout));
            Adreno::FramebufferCopyRegion span;assert(Adreno::framebufferCopyRegion(job,job.target,span));
            Adreno::Framebuffer2DRegion plan[Adreno::Framebuffer2DRegionCount];uint32_t covered=0,payload=0;
            constexpr uint64_t target=0x4200040;
            assert(Adreno::framebufferScissor2DRegions(target+span.offset,scratch+layout.offsets[0]+span.offset,job,job.target,span,plan,covered,payload));
            assert(payload+span.bytes-covered<span.bytes/4);
            auto bindings=buffers;bindings.addresses[QGPU_FRAGMENT_BUFFER_BASE]=0x4600000;bindings.bytes[QGPU_FRAGMENT_BUFFER_BASE]=16;
            Adreno::Encoder e(words.data(),words.size());Adreno::PipelineEncoder p(e,ib,scratch,layout.bytes);
            p.enableFramebuffer2D();p.enableFramebufferXCrop();
            p.enableFramebufferWord32(word32);
            assert(p.draw(job,vs,*fetch,bindings,target,{},0,0,0,nullptr,copy)==Result::Success);
            std::vector<uint8_t> source(job.target.pitch*job.target.height),snapshot(source.size(),0xa5);
            for(unsigned byte=0;byte<source.size();++byte)source[byte]=uint8_t(byte*17+byte/4096);
            std::map<uint32_t,uint32_t> registers;uint32_t mode=0,draws=0,tailCopies=0,totalPayload=0;
            uint64_t tailSource=0,tailDestination=0;bool flushed=false;
            for(unsigned i=0;i<e.size();) {
                const auto h=words[i++];
                if((h>>28)==4){const auto start=(h>>8)&0x3ffff;for(unsigned n=0;n<(h&127);++n)registers[start+n]=words[i+n];i+=h&127;continue;}
                const auto packet=Device::header(h);if(packet.first==0x6c)mode=words[i];
                if(packet.first==0x30 && packet.second==35 && ((words[i]>>18)&15)==15 && words[i+1]==2) {
                    tailSource=uint64_t(words[i+3])|(uint64_t(words[i+4])<<32);tailDestination=uint64_t(words[i+5])|(uint64_t(words[i+6])<<32);
                }
                if(packet.first==0x2c) {
                    assert(mode==5 && packet.second==5 && words[i]==1 && words[i+1]==words[i+3] && words[i+2]==words[i+4]);
                    const uint64_t src=uint64_t(registers[REG_A5XX_RB_2D_SRC_LO])|(uint64_t(registers[REG_A5XX_RB_2D_SRC_HI])<<32);
                    const uint64_t dst=uint64_t(registers[REG_A5XX_RB_2D_DST_LO])|(uint64_t(registers[REG_A5XX_RB_2D_DST_HI])<<32);
                    assert(src>=target+span.offset && dst==scratch+layout.offsets[0]+src-target);
                    assert(registers[REG_A5XX_RB_2D_SRC_INFO]==(word32?75u:3u) && registers[REG_A5XX_RB_2D_DST_INFO]==(word32?75u:3u));
                    const uint32_t element=word32?4:1,pitch=(registers[REG_A5XX_RB_2D_SRC_SIZE]&0xffff)*64,x=(words[i+1]&0x3fff)*element;
                    const uint32_t width=((words[i+2]&0x3fff)+1)*element-x,rows=((words[i+2]>>16)&0x3fff)+1;
                    assert(pitch==job.target.pitch && width==plan[0].width && x==plan[0].x && rows<=1008);
                    assert(registers[REG_A5XX_RB_2D_SRC_SIZE]==registers[REG_A5XX_RB_2D_DST_SIZE]);
                    assert((registers[REG_A5XX_RB_2D_SRC_SIZE]>>16)==pitch*rows/64);
                    for(unsigned row=0;row<rows;++row){const auto off=src-target+row*pitch+x;assert(off+width<=span.offset+covered);memcpy(snapshot.data()+off,source.data()+off,width);}
                    totalPayload+=width*rows;flushed=false;
                }
                if(packet.first==0x46 && packet.second==4 && words[i]==29 && mode==8)flushed=true;
                if(packet.first==0x33) {
                    assert(flushed && tailSource==target+span.offset+covered && tailDestination==scratch+layout.offsets[0]+span.offset+covered);
                    memcpy(snapshot.data()+span.offset+covered,source.data()+span.offset+covered,span.bytes-covered);totalPayload+=span.bytes-covered;++tailCopies;
                }
                if(packet.first==0x38) {
                    assert(mode==1 && tailCopies==draws+1);
                    for(unsigned byte=0;byte<snapshot.size();++byte) {
                        const uint32_t x=byte%job.target.pitch;
                        const bool copied=byte>=span.offset && byte-span.offset<span.bytes &&
                            (byte-span.offset>=covered || (x>=plan[0].x && x-plan[0].x<plan[0].width));
                        assert(snapshot[byte]==(copied?source[byte]:uint8_t(0xa5)));
                    }
                    for(auto &byte:source)byte^=0x39;
                    ++draws;
                }
                i+=packet.second;
            }
            assert(draws==2 && totalPayload==2*(payload+span.bytes-covered));
            assert(p.copiedFramebufferBytes()==totalPayload && p.copiedFramebuffer2DBytes()==2*payload && p.uncroppedFramebufferBytes()==2*span.bytes);
            job.target.samples=job.rasterSamples=2;
            assert(!Adreno::framebufferScissor2DRegions(target,scratch,job,job.target,span,plan,covered,payload));
        }
        for(uint32_t primitive:{1u,3u})for(uint32_t format:{uint32_t(RGBA8),uint32_t(R8),uint32_t(RG8),uint32_t(RGBA16Float)})for(uint32_t samples:{0u,2u,4u}) {
            const uint32_t vertices=primitive==1?2:3;
            auto job=draw;job.primitive=primitive;job.count=(samples==2?1:2)*vertices;job.instances=samples==2?2:1;job.baseInstance=samples==2?5:0;
            uint32_t cpp=formatBytes(format),nativeFormat=format==R8?3u:format==RG8?15u:format==RGBA16Float?0x62u:48u;
            job.target={1,0,16,16,64*cpp*(samples?samples:1),format};job.target.samples=job.rasterSamples=samples;
            for(auto &texture:job.textures)texture={};
            job.state=defaultRenderState(16,16);job.scissor[2]=job.scissor[3]=16;
            FramebufferLayout layout;assert(framebufferLayout(job,*fetch,layout) && layout.offsets[0]==4096);
            if(format==RGBA16Float)assert(!Adreno::framebufferInplaceEligible(job,*fetch));
            auto bindings=buffers;bindings.addresses[QGPU_FRAGMENT_BUFFER_BASE]=0x4600000;bindings.bytes[QGPU_FRAGMENT_BUFFER_BASE]=16;
            Adreno::Encoder commands(words.data(),words.size());Adreno::PipelineEncoder pipeline(commands,ib,scratch,layout.bytes);
            pipeline.enableFramebuffer2D(samples!=0); // MSAA retains the verified compute path.
            assert(pipeline.draw(job,vs,*fetch,bindings,0x4200000,{},0,0,0,nullptr,copy)==Result::Success);
            assert(pipeline.copiedFramebuffer2DBytes()==0);
            unsigned copies=0,primitives=0,sequence=0,descriptors=0,mrtFormat=0;
            for(unsigned i=0;i<commands.size();) {
                uint32_t header=words[i++];if((header>>28)==4){
                    if(((header>>8)&0x3ffff)==REG_A5XX_RB_MRT_BUF_INFO(0))mrtFormat=words[i]&A5XX_RB_MRT_BUF_INFO_COLOR_FORMAT__MASK;
                    if(((header>>8)&0x3ffff)==REG_A5XX_GRAS_SU_CNTL)
                        assert((words[i]&A5XX_GRAS_SU_CNTL_LINEHALFWIDTH__MASK)==(primitive==1?2u<<3:0));
                    i+=header&127;continue;}
                auto packet=Device::header(header);
                if(packet.first==0x30 && packet.second==15 && ((words[i]>>18)&15)==4) {
                    assert((words[i+3]&A5XX_TEX_CONST_0_FMT__MASK)==A5XX_TEX_CONST_0_FMT(nativeFormat));
                    if(cpp<4)assert((words[i+3]&A5XX_TEX_CONST_0_SWIZ_W__MASK)==A5XX_TEX_CONST_0_SWIZ_W(5));
                    assert((words[i+5]&A5XX_TEX_CONST_2_PITCHALIGN__MASK)==A5XX_TEX_CONST_2_PITCHALIGN(samples?__builtin_ctz(cpp*samples):0));++descriptors;
                }
                if(packet.first==0x33)++copies;
                if(packet.first==0x38){assert((words[i]&63)==(primitive==1?2u:4u) && words[i+1]==1 && words[i+2]==vertices);++primitives;}
                if(packet.first==0x46 && packet.second==4){assert(words[i+3]>sequence);sequence=words[i+3];}
                i+=packet.second;
            }
            assert(copies==2 && primitives==2 && descriptors==2 && sequence>=6 && mrtFormat==A5XX_RB_MRT_BUF_INFO_COLOR_FORMAT(nativeFormat));
            // Odd scissor boundaries must retain the full 2x2 helper quad,
            // while snapshot SSBO addresses skip the untouched upper rows.
            job.scissor[1]=3;job.scissor[3]=5;
            Adreno::FramebufferCopyRegion region;
            assert(Adreno::framebufferCopyRegion(job,job.target,region));
            assert(region.offset==2*job.target.pitch && region.bytes==6*job.target.pitch);
            Adreno::Encoder clipped(words.data(),words.size());Adreno::PipelineEncoder clippedPipeline(clipped,ib,scratch,layout.bytes);
            assert(clippedPipeline.draw(job,vs,*fetch,bindings,0x4200000,{},0,0,0,nullptr,copy)==Result::Success);
            unsigned snapshotBindings=0;
            for(unsigned i=0;i<clipped.size();) {
                uint32_t header=words[i++];if((header>>28)==4){i+=header&127;continue;}
                auto packet=Device::header(header);
                if(packet.first==0x30 && packet.second==35 && ((words[i]>>18)&15)==15 && words[i+1]==2) {
                    uint64_t source=uint64_t(words[i+3])|(uint64_t(words[i+4])<<32);
                    uint64_t destination=uint64_t(words[i+5])|(uint64_t(words[i+6])<<32);
                    assert(source==0x4200000+region.offset && destination==scratch+layout.offsets[0]+region.offset);
                    ++snapshotBindings;
                }
                i+=packet.second;
            }
            assert(snapshotBindings==2);
            job.scissor[1]=15;job.scissor[3]=1;
            assert(Adreno::framebufferCopyRegion(job,job.target,region) && region.offset==14*job.target.pitch && region.bytes==2*job.target.pitch);
            job.scissor[3]=2;
            assert(!Adreno::framebufferCopyRegion(job,job.target,region));
            job.scissor[1]=0;job.scissor[3]=16;
            Adreno::Encoder small(words.data(),words.size());Adreno::PipelineEncoder rejected(small,ib,scratch);
            assert(rejected.draw(job,vs,*fetch,bindings,0x4200000,{},0,0,0,nullptr,copy)==Result::Invalid && !small.size());
        }
        auto strip=draw;strip.primitive=4;strip.count=9;strip.indexSize=2;
        const uint16_t data[]={0,1,2,3,0xffff,4,5,6,7};PrimitiveIterator iterator(strip,data);Primitive primitive;
        for(uint32_t i=0;i<4;++i){assert(iterator.next(primitive));assert(primitive.first==(i<2?i:i+3) && primitive.windingXor==(i&1));}
        assert(!iterator.next(primitive));
        auto lines=draw;lines.primitive=1;lines.count=5;
        PrimitiveIterator lineIterator(lines,nullptr);
        for(uint32_t i=0;i<2;++i){assert(lineIterator.next(primitive));assert(primitive.first==2*i && primitive.count==2 && !primitive.windingXor);}
        assert(!lineIterator.next(primitive));
        words=originalWords;
    }
    auto regValue = [&](unsigned size, uint32_t wanted) {
        uint32_t result = UINT32_MAX;
        for (unsigned i=0;i<size;) {
            uint32_t header = words[i++];
            if ((header>>28) == 4) {
                unsigned reg = (header>>8)&0x3ffff, n = header&0x7f;
                for (unsigned j=0;j<n;++j) if (reg+j == wanted) result = words[i+j];
                i += n;
            } else i += Device::header(header).second;
        }
        return result;
    };
    assert(regValue(render.size(),REG_A5XX_GRAS_CL_VPORT_XSCALE_0) == 0x42000000); // 32
    assert(regValue(render.size(),REG_A5XX_GRAS_CL_VPORT_YSCALE_0) == 0xc2000000);
    {
        const GPU::Shader::Program *sampledVS=nullptr,*arrayVS=nullptr,*texturedFS=nullptr;
        for(const auto &p:GPUPrograms::Programs) {
            if(!strcmp(p.name,"sampled_vertex"))sampledVS=&p;
            if(!strcmp(p.name,"sampled_array_vertex"))arrayVS=&p;
            if(!strcmp(p.name,"varying_textured"))texturedFS=&p;
        }
        assert(sampledVS && arrayVS && texturedFS && sampledVS->textureCount==1 && sampledVS->driverBase>=16);
        auto both=draw;both.start=3;both.vertexTextures[0]={3,0,16,8,256,GPU::Shader::BGRA8SRGB,5};
        both.vertexSamplers[0].filters=4;both.vertexSamplers[0].addressModes=GPU::Shader::MirrorClampToEdge;both.vertexSamplers[0].minLOD=512;both.vertexSamplers[0].maxLOD=768;
        both.samplers[0].addressModes=GPU::Shader::MirrorClampToEdge<<3;
        Adreno::Encoder bothCommands(words.data(),words.size());Adreno::PipelineEncoder bothPipeline(bothCommands,ib,scratch);
        assert(bothPipeline.draw(both,*sampledVS,*texturedFS,buffers,0x4200000,Adreno::GraphicsTextures{{0x4300000},{0x4400000}},0,0,0)==Result::Success);
        assert(regValue(bothCommands.size(),REG_A5XX_TPL1_VS_TEX_COUNT)==1 && regValue(bothCommands.size(),REG_A5XX_TPL1_FS_TEX_COUNT)==1);
        bool vertexTexture=false,fragmentTexture=false,vertexConstants=false,fragmentConstants=false,unclampedVertexSampler=false;
        for(unsigned i=0;i<bothCommands.size();) {
            auto h=words[i++];if((h>>28)==4){i+=h&127;continue;}auto packet=Device::header(h);unsigned block=(words[i]>>18)&15;
            if(packet.first==0x30 && packet.second==15 && ((words[i]>>22)&3)==1) {
                if(block==0){assert(words[i+7]==0x4400000 && (words[i+3]&A5XX_TEX_CONST_0_SRGB));vertexTexture=true;}
                if(block==4){assert(words[i+7]==0x4300000);fragmentTexture=true;}
            }
            if(packet.first==0x30 && packet.second==7 && block==0 && (words[i]&0xffff)==8) {
                assert(!(words[i+4]&A5XX_TEX_SAMP_1_MIN_LOD__MASK) && (words[i+4]&A5XX_TEX_SAMP_1_MAX_LOD__MASK)==A5XX_TEX_SAMP_1_MAX_LOD__MASK);unclampedVertexSampler=true;
            }
            if(packet.first==0x30 && ((words[i]>>16)&3)==0) {
                if(block==8){assert(words[i+3]==1 && sampledVS->driverBase>=16 && words[i+3+sampledVS->driverBase+1]==both.start);vertexConstants=true;}
                if(block==12){assert(words[i+3]==2);fragmentConstants=true;}
            }
            i+=packet.second;
        }
        assert(vertexTexture && fragmentTexture && vertexConstants && fragmentConstants && unclampedVertexSampler);
        both.vertexTextures[0].layers=3;
        Adreno::Encoder arrays(words.data(),words.size());Adreno::PipelineEncoder arraysPipeline(arrays,ib,scratch);
        assert(arraysPipeline.draw(both,*arrayVS,*texturedFS,buffers,0x4200000,Adreno::GraphicsTextures{{0x4300000},{0x4400000}},0,0,0)==Result::Success);
        Adreno::Encoder mismatch(words.data(),words.size());Adreno::PipelineEncoder mismatchPipeline(mismatch,ib,scratch);
        assert(mismatchPipeline.draw(both,*sampledVS,*texturedFS,buffers,0x4200000,Adreno::GraphicsTextures{{0x4300000},{0x4400000}},0,0,0)==Result::Invalid && !mismatch.size());
        both.vertexTextures[0]={};both.vertexSamplers[0]={};
        Adreno::Encoder reset(words.data(),words.size());Adreno::PipelineEncoder resetPipeline(reset,ib,scratch);
        assert(resetPipeline.draw(both,vs,fs,buffers,0x4200000,Adreno::GraphicsTextures{{0x4300000},{0}})==Result::Success && regValue(reset.size(),REG_A5XX_TPL1_VS_TEX_COUNT)==0);
        both.vertexReserved=1;assert(!GPU::Shader::validDraw(both));
    }
    {
        using namespace GPU::Shader;
        const Program *multiVS=nullptr,*multiFS=nullptr;
        for(const auto &p:GPUPrograms::Programs){if(!strcmp(p.name,"multitextured_vertex"))multiVS=&p;if(!strcmp(p.name,"multitextured_fragment"))multiFS=&p;}
        assert(multiVS && multiFS && multiVS->sampleMask==66 && multiVS->samplerMask==132 && multiFS->sampleMask==66);
        auto multi=draw;multi.textures[0]={};multi.start=3;
        multi.vertexTextures[1]={4,0,16,8,256,RGBA8SRGB,5};multi.vertexTextures[6]={5,0,16,8,256,BGRA8,5,0,3,0};
        multi.textures[1]={6,0,16,8,256,BGRA8SRGB,5};multi.textures[6]={7,0,16,8,256,RGBA8,5};
        multi.vertexSamplers[2].addressModes=MirrorClampToEdge;multi.vertexSamplers[7].addressModes=MirrorClampToEdge<<3;
        multi.samplers[2].addressModes=MirrorClampToEdge<<3;multi.samplers[7].addressModes=MirrorClampToEdge;
        for(auto *samplers:{multi.vertexSamplers,multi.samplers})for(unsigned i:{2u,7u}){samplers[i].filters=4;samplers[i].minLOD=512;samplers[i].maxLOD=768;}
        Adreno::GraphicsTextures addresses;addresses.vertex[1]=0x4400000;addresses.vertex[6]=0x4500000;addresses.fragment[1]=0x4600000;addresses.fragment[6]=0x4700000;
        Adreno::Encoder encoded(words.data(),words.size());Adreno::PipelineEncoder pipeline(encoded,ib,scratch);
        assert(pipeline.draw(multi,*multiVS,*multiFS,buffers,0x4200000,addresses)==Result::Success);
        assert(regValue(encoded.size(),REG_A5XX_TPL1_VS_TEX_COUNT)==7 && regValue(encoded.size(),REG_A5XX_TPL1_FS_TEX_COUNT)==7);
        uint32_t textureSlots[2]={},samplerSlots[2]={};bool vsConstants=false,fsConstants=false;
        for(unsigned i=0;i<encoded.size();) {
            auto h=words[i++];if((h>>28)==4){i+=h&127;continue;}auto packet=Device::header(h);unsigned block=(words[i]>>18)&15,slot=words[i]&0xffff;
            if(packet.first==0x30 && ((words[i]>>22)&3)==1 && (block==0 || block==4)) {
                unsigned stage=block==0?0:1;
                if(packet.second==15){assert(slot==1 || slot==6);textureSlots[stage]|=1u<<slot;
                    assert(words[i+7]==(stage?addresses.fragment[slot]:addresses.vertex[slot]));}
                if(packet.second==7){samplerSlots[stage]|=1u<<slot;if(slot>=8)assert(!(words[i+4]&A5XX_TEX_SAMP_1_MIN_LOD__MASK));}
            }
            if(packet.first==0x30 && ((words[i]>>16)&3)==0) {
                if(block==8){assert(words[i+3+2]==1 && words[i+3+7]==2);vsConstants=true;}
                if(block==12){assert(words[i+3+2]==2 && words[i+3+7]==1);fsConstants=true;}
            }
            i+=packet.second;
        }
        assert(textureSlots[0]==66 && textureSlots[1]==66 && samplerSlots[0]==(132u|(128u<<8)) && samplerSlots[1]==(128u|(4u<<8)) && vsConstants && fsConstants);
        auto reject=[&](const Draw &bad,const Adreno::GraphicsTextures &resolved){
            Adreno::Encoder commands(words.data(),words.size());Adreno::PipelineEncoder backend(commands,ib,scratch);
            assert(backend.draw(bad,*multiVS,*multiFS,buffers,0x4200000,resolved)==Result::Invalid && !commands.size());
        };
        auto bad=multi;bad.vertexTextures[6].layers=0;reject(bad,addresses);
        bad=multi;bad.textures[0]=multi.textures[1];reject(bad,addresses);
        bad=multi;bad.vertexSamplers[0].maxLOD=256;reject(bad,addresses);
        auto missing=addresses;missing.fragment[6]=0;reject(multi,missing);
        // The full-slot packet path is independent of how many pairs NIR uses.
        auto fullVS=*multiVS,fullFS=*multiFS;fullVS.sampleMask=fullFS.sampleMask=fullVS.samplerMask=fullFS.samplerMask=255;
        fullVS.textureCount=fullFS.textureCount=8;fullVS.explicitSamplerMask=fullFS.explicitSamplerMask=255;fullVS.implicitSamplerMask=fullFS.implicitSamplerMask=255;
        for(unsigned i=0;i<8;++i){if(i!=6)multi.vertexTextures[i]=multi.vertexTextures[1];multi.textures[i]=multi.textures[1];
            multi.vertexSamplers[i]=multi.vertexSamplers[2];multi.samplers[i]=multi.samplers[2];
            addresses.vertex[i]=0x4800000+i*0x10000;addresses.fragment[i]=0x5000000+i*0x10000;}
        Adreno::Encoder full(words.data(),words.size());Adreno::PipelineEncoder fullPipeline(full,ib,scratch);
        assert(fullPipeline.draw(multi,fullVS,fullFS,buffers,0x4200000,addresses)==Result::Success);
        assert(regValue(full.size(),REG_A5XX_TPL1_VS_TEX_COUNT)==8 && regValue(full.size(),REG_A5XX_TPL1_FS_TEX_COUNT)==8);
    }
    {
        using namespace GPU::Shader;
        for(const char *name:{"fetch_vertex","fetch_array_fragment","fetch_cube_fragment","fetch_cube_array_vertex","texture_queries_fragment",
            "air_fetch_vertex","air_fetch_cube_fragment","air_fetch_cube_array_fragment","air_cube_array_sample_vertex","air_array_sample_fragment","air_cube_sample_fragment",
            "constant_cube_vertex","constant_sample_fragment","constant_array_fragment","air_constant_cube_vertex","air_constant_array_fragment"}) {
            const Program *program=nullptr;for(const auto &p:GPUPrograms::Programs)if(!strcmp(p.name,name))program=&p;
            assert(program && validInterface(*program));
            auto job=draw;
            for(auto &t:job.textures)t={};
            for(auto &t:job.vertexTextures)t={};
            auto *textures=program->stage==Vertex?job.vertexTextures:job.textures;
            auto *samplers=program->stage==Vertex?job.vertexSamplers:job.samplers;
            Adreno::GraphicsTextures addresses;auto *va=program->stage==Vertex?addresses.vertex:addresses.fragment;
            auto shaderBuffers=buffers;for(unsigned i=0;i<QGPU_GRAPHICS_BUFFER_COUNT;++i) {shaderBuffers.addresses[i]=0x4000000+i*0x10000;shaderBuffers.bytes[i]=4096;}
            for(unsigned i=0;i<8;++i)if(graphicsTextureMask(*program)&(1u<<i)) {
                textures[i]={50+i,4096,16,16,256,BGRA8SRGB,5,packSwizzle(ChannelAlpha,ChannelRed,ChannelZero,ChannelOne),
                    (program->flags&(1u<<(QGPU_ARRAY_TEXTURE_SHIFT+i)))?2u:0u,0,
                    (program->flags&(1u<<(QGPU_CUBE_TEXTURE_SHIFT+i)))?1u:0u};
                TextureLayout layout;assert(textureLayout(textures[i],layout));
                if(textures[i].layers || textures[i].cube)textures[i].layerStride=layout.layerStride;
                va[i]=0x4800000+i*0x100000;
            }
            uint32_t expected[QGPU_MAX_MEMORY_CONSTANT_WORDS]{};graphicsConstants(*program,textures,samplers,expected);
            for(unsigned i=0;i<QGPU_BUFFER_SIZE_CONSTANT_WORDS;++i)expected[QGPU_BUFFER_SIZE_CONSTANT_BASE+i]=shaderBuffers.bytes[i];
            // These bindings are aligned 4 KiB allocations. Small fragment
            // uniforms now also use an available constant-bank window.
            for(unsigned i=0;i<program->constantWindowCount;++i) {
                assert(program->constantWindows[i].byteOffset+QGPU_CONSTANT_WINDOW_BYTES<=4096);
                expected[qgpu_constant_window_flag(program->constantWindowCount,i)]=1;
            }
            Adreno::Encoder commands(words.data(),words.size());Adreno::PipelineEncoder pipeline(commands,ib,scratch);
            // The plain fragment has no input varyings or texture state.
            const Program *plainFS=nullptr;for(const auto &p:GPUPrograms::Programs)if(!strcmp(p.name,"color"))plainFS=&p;
            assert(plainFS);
            assert(pipeline.draw(job,program->stage==Vertex?*program:vs,program->stage==Fragment?*program:*plainFS,
                shaderBuffers,0x4200000,addresses)==Result::Success);
            assert(regValue(commands.size(),program->stage==Vertex?REG_A5XX_TPL1_VS_TEX_COUNT:REG_A5XX_TPL1_FS_TEX_COUNT)==program->textureCount);
            unsigned fetched=0,sampled=0,samplersSeen=0;bool constants=!memoryConstantWords(*program);
            for(unsigned i=0;i<commands.size();) {
                auto h=words[i++];if((h>>28)==4){i+=h&127;continue;}auto packet=Device::header(h);unsigned state=(words[i]>>18)&15,slot=words[i]&0xffff;
                if(packet.first==0x30 && packet.second==7 && state==(program->stage==Vertex?0u:4u) && ((words[i]>>22)&3)==1) {
                    assert(slot<16 && !(samplersSeen&(1u<<slot)));samplersSeen|=1u<<slot;
                    for(unsigned j=0;j<QGPU_MAX_CONSTANT_SAMPLERS;++j)for(unsigned mode=0;mode<2;++mode) {
                        uint32_t mask=mode?program->constantExplicitSamplerMask:program->constantImplicitSamplerMask;
                        uint32_t target=mode?program->constantSamplerLevelSlots[j]:program->constantSamplerSlots[j];
                        if(!(mask&(1u<<j)) || target!=slot)continue;
                        const auto &sampler=program->constantSamplers[j];
                        unsigned mip=(sampler.filters>>2)&3,low=mode?0:sampler.minLOD,high=mode?4095:sampler.maxLOD;
                        if(!mip){if(low>32)low=32;if(high>32)high=32;}
                        assert(((words[i+4]&A5XX_TEX_SAMP_1_MIN_LOD__MASK)>>A5XX_TEX_SAMP_1_MIN_LOD__SHIFT)==low);
                        assert(((words[i+4]&A5XX_TEX_SAMP_1_MAX_LOD__MASK)>>A5XX_TEX_SAMP_1_MAX_LOD__SHIFT)==high);
                    }
                }
                if(packet.first==0x30 && packet.second==15 && state==(program->stage==Vertex?0u:4u) && ((words[i]>>22)&3)==1) {
                    unsigned logical=slot%8;const auto &t=textures[logical];
                    if(slot>=8) {assert(program->fetchMask&(1u<<logical));fetched|=1u<<logical;
                        assert((words[i+5]&A5XX_TEX_CONST_2_TYPE__MASK)==A5XX_TEX_CONST_2_TYPE(1));
                        assert((words[i+8]&A5XX_TEX_CONST_5_DEPTH__MASK)==A5XX_TEX_CONST_5_DEPTH(textureSlices(t)));}
                    else {assert(program->sampleMask&(1u<<logical));sampled|=1u<<logical;}
                    TextureLayout layout;assert(textureLayout(t,layout,false));
                    assert(words[i+6]==A5XX_TEX_CONST_3_ARRAY_PITCH(layout.layerStride) && words[i+7]==uint32_t(va[logical]));
                    assert(words[i+3]&A5XX_TEX_CONST_0_SRGB);
                }
                if(packet.first==0x30 && packet.second==program->constlen*4+3 && state==(program->stage==Vertex?8u:12u) && !(words[i]&0xffff)) {
                    for(unsigned j=0;j<memoryConstantWords(*program);++j)assert(words[i+3+j]==expected[j]);
                    constants=true;
                }
                i+=packet.second;
            }
            uint32_t expectedSamplers=program->implicitSamplerMask|(program->explicitSamplerMask<<8);
            for(unsigned j=0;j<QGPU_MAX_CONSTANT_SAMPLERS;++j) {
                if(program->constantImplicitSamplerMask&(1u<<j))expectedSamplers|=1u<<program->constantSamplerSlots[j];
                if(program->constantExplicitSamplerMask&(1u<<j))expectedSamplers|=1u<<program->constantSamplerLevelSlots[j];
            }
            assert(fetched==program->fetchMask && sampled==program->sampleMask && constants && samplersSeen==expectedSamplers);
            if(program->fetchMask) {auto invalid=*program;invalid.queryMask=0;assert(!validInterface(invalid));}
            // A query-only binding owns no read hazard, even when its pages
            // are a render attachment. Native admission still retains it.
            if(!graphicsReadMask(*program))assert(graphicsTextureMask(*program)==139);
        }
    }
    {
        using namespace GPU::Shader;
        Texture cubes{22,4096,16,16,256,RGBA8SRGB,5,0,2,0,1};TextureLayout root;
        assert(textureLayout(cubes,root) && textureSlices(cubes)==12 && root.bytes==12*root.layerStride);
        cubes.layerStride=root.layerStride;Texture view,face,plane,nested;
        assert(textureView(cubes,1,3,6,6,false,BGRA8,view,0,true) && view.cube && !view.layers && textureSlices(view)==6 && view.layerStride==root.layerStride);
        assert(view.offset==cubes.offset+6*root.layerStride+root.level[1].offset);
        assert(textureView(view,1,1,5,1,false,RGBA8,face) && !face.cube && !face.layers && !face.layerStride);
        assert(face.offset==cubes.offset+11*root.layerStride+root.level[2].offset);
        assert(textureView(cubes,0,5,0,12,true,RGBA8,nested) && nested.layers==12 && !nested.cube);
        assert(textureView(nested,1,2,6,6,true,RGBA8,plane,0,true) && plane.layers==1 && plane.cube);
        assert(textureView(cubes,0,1,1,6,false,RGBA8,plane,0,true) && plane.offset==cubes.offset+root.layerStride && textureSlices(plane)==6);
        assert(!textureView(cubes,0,1,0,5,false,RGBA8,plane,0,true));
        auto nonsquare=cubes;nonsquare.height=8;assert(!textureLayout(nonsquare,root));
        assert(textureLayout(cubes,root));
        assert(textureSlice(cubes,5,face) && !overlap(view,face));
        assert(textureSlice(cubes,6,face) && textureSubview(face,2,1,RGBA8,plane) && overlap(view,plane));
        assert(textureSubview(face,4,1,RGBA8,plane) && !overlap(view,plane));
        const Program *cubeFS=nullptr,*cubeCS=nullptr;
        for(const auto &p:GPUPrograms::Programs){if(!strcmp(p.name,"cube_textured"))cubeFS=&p;if(!strcmp(p.name,"cube_sample"))cubeCS=&p;}
        assert(cubeFS && cubeCS && cubeFS->cubeSamplerMask==4 && cubeCS->cubeSamplerMask==4);
        auto cubeDraw=draw;cubeDraw.textures[0]={};cubeDraw.textures[3]=cubes;cubeDraw.textures[3].layers=0;
        cubeDraw.samplers[2].filters=4;cubeDraw.samplers[2].minLOD=512;cubeDraw.samplers[2].maxLOD=768;
        cubeDraw.samplers[2].addressModes=MirrorClampToEdge;
        Adreno::GraphicsTextures addresses;addresses.fragment[3]=0x4800000;
        Adreno::Encoder cubeCommands(words.data(),words.size());Adreno::PipelineEncoder cubePipeline(cubeCommands,ib,scratch);
        assert(cubePipeline.draw(cubeDraw,vs,*cubeFS,buffers,0x4200000,addresses)==Result::Success);
        bool descriptor=false,ordinary=false,shadow=false;
        for(unsigned i=0;i<cubeCommands.size();) {
            auto h=words[i++];if((h>>28)==4){i+=h&127;continue;}auto packet=Device::header(h);unsigned block=(words[i]>>18)&15,slot=words[i]&0xffff;
            if(packet.first==0x30 && block==4) {
                if(packet.second==15){assert(slot==3 && (words[i+5]&A5XX_TEX_CONST_2_TYPE__MASK)==A5XX_TEX_CONST_2_TYPE(2));
                    assert(words[i+6]==A5XX_TEX_CONST_3_ARRAY_PITCH(root.layerStride) && (words[i+8]&A5XX_TEX_CONST_5_DEPTH__MASK)==A5XX_TEX_CONST_5_DEPTH(1));descriptor=true;}
                if(packet.second==7){assert(!(words[i+4]&A5XX_TEX_SAMP_1_CUBEMAPSEAMLESSFILTOFF));
                    if(slot==2)ordinary=true;else {assert(slot==10 && !(words[i+4]&A5XX_TEX_SAMP_1_MIN_LOD__MASK));shadow=true;}}
            }
            i+=packet.second;
        }
        assert(descriptor && !ordinary && shadow);
        cubeDraw.samplers[2].normalized=0;cubeDraw.samplers[2].filters=cubeDraw.samplers[2].addressModes=0;
        Adreno::Encoder badCommands(words.data(),words.size());Adreno::PipelineEncoder rejectCube(badCommands,ib,scratch);
        assert(rejectCube.draw(cubeDraw,vs,*cubeFS,buffers,0x4200000,addresses)==Result::Invalid && !badCommands.size());
        cubeDraw.textureReserved=1;assert(!validDraw(cubeDraw));
        Dispatch dispatch{};dispatch.groups[0]=dispatch.groups[1]=dispatch.groups[2]=1;dispatch.local[0]=dispatch.local[1]=dispatch.local[2]=1;
        dispatch.images[6]=cubes;dispatch.samplers[2].filters=4;
        for(unsigned i=0;i<3;++i)dispatch.buffers[i]={30+i,0,64,0};
        uint32_t imageConstants[QGPU_IMAGE_CONSTANT_WORDS];assert(imageFootprint(dispatch,*cubeCS,imageConstants));
        auto wrong=dispatch;wrong.images[6].cube=0;assert(!imageFootprint(wrong,*cubeCS,imageConstants));
    }
    {
        using namespace GPU::Shader;
        for(const char *name:{"cube_copy","cube_array_copy"}) {
            const Program *copy=nullptr;for(const auto &p:GPUPrograms::Programs)if(!strcmp(p.name,name))copy=&p;
            assert(copy && copy->codeWords>1024 && copy->instrlen>32 && memoryConstantWords(*copy)==QGPU_MEMORY_CONSTANT_WORDS && !copy->cubeSamplerMask);
            Dispatch job{};job.groups[0]=job.groups[1]=job.groups[2]=1;job.local[0]=job.local[1]=job.local[2]=1;
            job.buffers[0]={40,0,16,0};
            unsigned cubes=!strcmp(name,"cube_array_copy")?2:0;
            job.images[0]={41,0,16,16,256,RGBA8SRGB,5,0,cubes,0,1};job.images[7]=job.images[0];job.images[7].resource=42;
            auto resolved=buffers;resolved.addresses[8]=0x6000000;resolved.addresses[15]=0x6100000;
            uint64_t bytes=0;TextureLayout layout;assert(textureSize(job.images[0],bytes) && textureLayout(job.images[0],layout));
            resolved.bytes[8]=resolved.bytes[15]=uint32_t(bytes);
            Adreno::Encoder commands(words.data(),words.size());Adreno::PipelineEncoder backend(commands,ib,scratch);
            assert(backend.dispatch(job,*copy,resolved)==Result::Success);
            assert(regValue(commands.size(),REG_A5XX_HLSQ_CS_INSTRLEN)==0 && !(regValue(commands.size(),REG_A5XX_SP_CS_CTRL_REG0)&A5XX_SP_CS_CTRL_REG0_BUFFER));
            assert((regValue(commands.size(),REG_A5XX_HLSQ_CS_CNTL)&A5XX_HLSQ_CS_CNTL_INSTRLEN__MASK)==0);
            bool constants=false;unsigned preloads=0;
            for(unsigned i=0;i<commands.size();) {
                auto h=words[i++];if((h>>28)==4){i+=h&127;continue;}auto packet=Device::header(h);
                if(packet.first==0x30 && ((words[i]>>18)&15)==13) {
                    if(((words[i]>>16)&3)==2)++preloads;
                    if(((words[i]>>16)&3)==0){assert(words[i+3+80]==(cubes?12:6) && words[i+3+94]==(cubes?12:6));
                        assert(words[i+3+81]==layout.layerStride && words[i+3+95]==layout.layerStride);constants=true;}
                }
                i+=packet.second;
            }
            assert(constants && !preloads && regValue(commands.size(),REG_A5XX_SP_CS_OBJ_START_LO)>=ib);
            auto bad=job;bad.images[0].cube=0;uint32_t geometry[32];assert(!imageFootprint(bad,*copy,geometry));
        }
        // Only cache state is exercised with these padded synthetic images;
        // no synthetic ISA is executed.
        std::vector<uint32_t> paddedCode(QGPU_MAX_CODE_WORDS,0);
        const unsigned lengths[][4]={{32,32,32,32},{48,32,0,32},{32,80,32,0},{80,80,0,0},{8,64,8,0}};
        for(const auto &length:lengths) {
            auto largeVS=vs,largeFS=fs;largeVS.instrlen=length[0];largeVS.codeWords=length[0]*32;largeVS.code=paddedCode.data();
            largeFS.instrlen=length[1];largeFS.codeWords=length[1]*32;largeFS.code=paddedCode.data();
            Adreno::Encoder commands(words.data(),words.size());Adreno::PipelineEncoder backend(commands,ib,scratch);
            assert(backend.draw(draw,largeVS,largeFS,buffers,0x4200000,Adreno::GraphicsTextures{{0x4300000},{0}})==Result::Success);
            assert(regValue(commands.size(),REG_A5XX_HLSQ_VS_INSTRLEN)==length[2] && regValue(commands.size(),REG_A5XX_HLSQ_FS_INSTRLEN)==length[3]);
            assert(bool(regValue(commands.size(),REG_A5XX_SP_VS_CTRL_REG0)&A5XX_SP_VS_CTRL_REG0_BUFFER)==bool(length[2]));
            assert(bool(regValue(commands.size(),REG_A5XX_SP_FS_CTRL_REG0)&A5XX_SP_FS_CTRL_REG0_BUFFER)==bool(length[3]));
            assert((regValue(commands.size(),REG_A5XX_HLSQ_FS_CONFIG)&A5XX_HLSQ_FS_CONFIG_SHADEROBJOFFSET__MASK)==A5XX_HLSQ_FS_CONFIG_SHADEROBJOFFSET(64-length[3]));
        }
    }
    {
        using namespace GPU::Shader;
        Texture array{21,4096,37,19,256,RGBA8SRGB,6,0,4,0};TextureLayout owner;
        assert(textureLayout(array,owner));array.layerStride=owner.layerStride;
        uint64_t span=0;assert(textureSize(array,span) && span==uint64_t(3)*owner.layerStride+owner.level[5].offset+owner.level[5].pitch);
        Texture view,nested,plane;
        assert(textureView(array,1,3,1,2,true,BGRA8,view) && view.layers==2 && view.layerStride==owner.layerStride &&
            view.offset==array.offset+owner.layerStride+owner.level[1].offset);
        assert(textureView(view,1,1,1,1,false,RGBA8,nested) && !nested.layers && !nested.layerStride &&
            nested.offset==array.offset+2*owner.layerStride+owner.level[2].offset);
        assert(textureSlice(array,3,plane) && plane.offset==array.offset+3*owner.layerStride && !plane.layers);
        Texture mipRead,mipTarget;
        assert(textureView(array,1,1,0,4,true,RGBA8,mipRead) && textureView(array,0,1,2,1,false,RGBA8,mipTarget) &&
            !overlap(mipRead,mipTarget));
        assert(textureView(array,1,1,2,1,false,RGBA8,mipTarget) && overlap(mipRead,mipTarget));
        assert(!textureSlice(array,4,plane) && emptyTexture(plane));
        assert(!textureView(array,0,1,3,2,true,RGBA8,plane));
        auto bad=array;bad.layerStride=64;assert(!textureSize(bad,span));
        bad=array;bad.layers=2049;assert(!textureSize(bad,span));
        bad=array;bad.layerStride=GPU::MaxBufferBytes;assert(!textureSize(bad,span));
        bad=array;bad.layers=0;assert(!textureSize(bad,span));
        const Program *copy=nullptr,*sample=nullptr;
        for(const auto &p:GPUPrograms::Programs){if(!strcmp(p.name,"array_copy"))copy=&p;if(!strcmp(p.name,"array_textured"))sample=&p;}
        assert(copy && sample && ((copy->flags&QGPU_ARRAY_TEXTURE_MASK)>>QGPU_ARRAY_TEXTURE_SHIFT)==129 && copy->driverBase>=96);
        Dispatch d{};d.local[0]=4;d.local[1]=d.local[2]=1;d.groups[0]=10;d.groups[1]=19;d.groups[2]=4;
        d.images[0]=array;d.images[7]=array;d.images[7].resource=22;
        Adreno::ShaderBuffers bindings{};bindings.addresses[0]=0x4000000;bindings.bytes[0]=4;
        for(unsigned slot:{0u,7u}){assert(textureSize(d.images[slot],span));bindings.addresses[8+slot]=0x5000000+slot*0x100000;bindings.bytes[8+slot]=span;}
        Adreno::Encoder arrayCommands(words.data(),words.size());Adreno::PipelineEncoder arrayPipeline(arrayCommands,ib,scratch);
        assert(arrayPipeline.dispatch(d,*copy,bindings)==Result::Success);
        bool arrayConstants=false;
        for(unsigned i=0;i<arrayCommands.size();) {
            uint32_t h=words[i++];if((h>>28)==4){i+=h&127;continue;}auto packet=Device::header(h);
            if(packet.first==0x30 && ((words[i]>>18)&15)==13 && ((words[i]>>16)&3)==0) {
                const auto *v=words.data()+i+3;
                for(unsigned slot:{0u,7u})assert(v[QGPU_ARRAY_CONSTANT_BASE+slot*2]==4 && v[QGPU_ARRAY_CONSTANT_BASE+slot*2+1]==owner.layerStride);
                arrayConstants=true;
            }i+=packet.second;
        }
        assert(arrayConstants);
        auto mismatch=d;mismatch.images[0].layers=0;mismatch.images[0].layerStride=0;
        uint64_t elements;unsigned dimensions;assert(!dispatchFootprint(mismatch,*copy,elements,dimensions));
        auto renderArray=draw;renderArray.textures[0]=array;
        bindings.addresses[QGPU_FRAGMENT_BUFFER_BASE]=0x4800000;bindings.bytes[QGPU_FRAGMENT_BUFFER_BASE]=8;
        Adreno::Encoder arrayRender(words.data(),words.size());Adreno::PipelineEncoder arrayRenderPipeline(arrayRender,ib,scratch);
        assert(arrayRenderPipeline.draw(renderArray,vs,*sample,bindings,0x4200000,Adreno::GraphicsTextures{{0x5000000},{0}})==Result::Success);
        bool arrayDescriptor=false;
        for(unsigned i=0;i<arrayRender.size();) {
            uint32_t h=words[i++];if((h>>28)==4){i+=h&127;continue;}auto packet=Device::header(h);
            if(packet.first==0x30 && packet.second==15 && ((words[i]>>18)&15)==4 && ((words[i]>>22)&3)==1) {
                assert(words[i+6]==A5XX_TEX_CONST_3_ARRAY_PITCH(owner.layerStride));
                assert(words[i+8]==A5XX_TEX_CONST_5_DEPTH(4));arrayDescriptor=true;
            }i+=packet.second;
        }
        assert(arrayDescriptor);
    }
    {
        const GPU::Shader::Program *image=nullptr;
        for(const auto &p:GPUPrograms::Programs)if(!strcmp(p.name,"image_transform"))image=&p;
        assert(image && image->imageMask==129 && image->imageReadMask==1 && image->imageWriteMask==128);
        auto grid=dispatch;grid.images[0]={3,0,17,9,256,GPU::Shader::RGBA8,3};grid.images[7]={4,0,17,9,256,GPU::Shader::BGRA8,3};
        uint32_t geometry[32];assert(GPU::Shader::imageFootprint(grid,*image,geometry));
        assert(geometry[0]==17 && geometry[1]==9 && geometry[2]==256 && geometry[3]==769);
        assert(geometry[28]==17 && geometry[29]==9 && geometry[30]==256 && geometry[31]==770);
        auto resolved=buffers;resolved.addresses[8]=0x4400000;resolved.addresses[15]=0x4500000;resolved.bytes[8]=resolved.bytes[15]=12288;
        Adreno::Encoder commands(words.data(),words.size());Adreno::PipelineEncoder pipeline(commands,ib,scratch);
        assert(pipeline.dispatch(grid,*image,resolved)==Result::Success);
        unsigned checked=0;
        for(unsigned i=0;i<commands.size();) {
            auto header=words[i++];if((header>>28)==4){i+=header&127;continue;}auto packet=Device::header(header);
            if(packet.first==0x30 && ((words[i]>>18)&15)==13 && ((words[i]>>16)&3)==0) {
                assert(!memcmp(words.data()+i+3+16,geometry,sizeof(geometry)) && image->driverBase>=48 && image->immediateBase>=48);checked|=1;
            }
            if(packet.first==0x30 && ((words[i]>>18)&15)==15) {
                assert(packet.second==35 && words[i]>>22==16);
                if(words[i+1]==2){assert(words[i+3+8*2]==0x4400000 && words[i+3+15*2]==0x4500000);checked|=2;}
            }
            i+=packet.second;
        }
        assert(checked==3 && (regValue(commands.size(),REG_A5XX_HLSQ_CS_CNTL)&A5XX_HLSQ_CS_CNTL_SSBO_ENABLE));
        auto halfGrid=grid;
        for(auto slot:{0u,7u})halfGrid.images[slot]={slot+3,0,17,9,512,GPU::Shader::RGBA16Float,3};
        assert(GPU::Shader::imageFootprint(halfGrid,*image,geometry) && geometry[3]==778 && geometry[31]==778);
        Adreno::Encoder halfCommands(words.data(),words.size());Adreno::PipelineEncoder halfPipeline(halfCommands,ib,scratch);
        assert(halfPipeline.dispatch(halfGrid,*image,resolved)==Result::Success);
        grid.images[0].format=GPU::Shader::RGBA8SRGB;grid.images[7].format=GPU::Shader::BGRA8SRGB;
        assert(GPU::Shader::imageFootprint(grid,*image,geometry) && geometry[3]==773 && geometry[31]==774);
        grid.images[0].swizzle=GPU::Shader::packSwizzle(5,2,0,1);
        assert(GPU::Shader::imageFootprint(grid,*image,geometry) && geometry[3]==(773|(grid.images[0].swizzle<<16)));
        auto invalidImage=grid;invalidImage.images[0].swizzle=7;
        assert(!GPU::Shader::imageFootprint(invalidImage,*image,geometry));
        grid.images[1]=grid.images[0];assert(!GPU::Shader::imageFootprint(grid,*image,geometry));
        grid.images[1]={};grid.images[7].pitch=64;assert(!GPU::Shader::imageFootprint(grid,*image,geometry));
    }
    // Production sparse/all-eight output programs feed independent native
    // framebuffer slots, including a pass with color7 and no color0 attachment.
    for(const char *name:{"mrt_fragment","all_colors_fragment","color_only_fragment","air_mrt_fragment","air_color_only_fragment"}) {
        using namespace GPU::Shader;const Program *fragment=nullptr;
        for(const auto &p:GPUPrograms::Programs)if(!strcmp(p.name,name))fragment=&p;
        assert(fragment && validInterface(*fragment));auto job=draw;job.target={};
        for(auto &t:job.textures)t={};
        Adreno::GraphicsTextures sampled;
        uint64_t targets[7]={};auto resolved=buffers;resolved.addresses[8]=0x4600000;resolved.bytes[8]=4096;
        for(unsigned i=0;i<8;++i)if(fragment->colorMask&(1u<<i)) {
            Texture t{100+i,0,64,64,256,i&1?BGRA8SRGB:RGBA8};
            if(!i)job.target=t;
            else {job.colorTargets[i-1]=t;job.colorStates[i-1]=copyColorState(defaultRenderState(64,64));targets[i-1]=0x5000000+i*0x10000;}
        }
        if(job.colorTargets[6].resource){job.colorStates[6].blend=1;job.colorStates[6].destinationRGB=One;job.colorStates[6].writeMask=5;}
        if(fragment->sampleMask){job.textures[5]={50,0,16,16,256,RGBA8,5};sampled.fragment[5]=0x4800000;}
        Adreno::Encoder commands(words.data(),words.size());Adreno::PipelineEncoder pipeline(commands,ib,scratch);
        assert(pipeline.draw(job,vs,*fragment,resolved,job.target.resource?0x4900000:0,sampled,0,0,0,targets)==Result::Success);
        assert(regValue(commands.size(),REG_A5XX_RB_FS_OUTPUT_CNTL)==8);
        assert((regValue(commands.size(),REG_A5XX_RB_BLEND_CNTL)&(A5XX_RB_BLEND_CNTL_INDEPENDENT_BLEND|A5XX_RB_BLEND_CNTL_ENABLE_BLEND__MASK))==
            (A5XX_RB_BLEND_CNTL_INDEPENDENT_BLEND|A5XX_RB_BLEND_CNTL_ENABLE_BLEND(128)));
        uint32_t components=0;
        for(unsigned i=0;i<8;++i) {
            bool active=fragment->colorMask&(1u<<i);uint64_t address=i?targets[i-1]:job.target.resource?0x4900000:0;
            assert(regValue(commands.size(),REG_A5XX_SP_FS_OUTPUT_REG(i))==A5XX_SP_FS_OUTPUT_REG_REGID(fragment->colorRegisters[i]));
            assert(regValue(commands.size(),REG_A5XX_RB_MRT_BASE_LO(i))==uint32_t(address));
            assert((regValue(commands.size(),REG_A5XX_RB_MRT_CONTROL(i))&A5XX_RB_MRT_CONTROL_COMPONENT_ENABLE__MASK)==A5XX_RB_MRT_CONTROL_COMPONENT_ENABLE(active?(i==7?5:15):0));
            if(active)components|=15u<<(4*i);
        }
        assert(regValue(commands.size(),REG_A5XX_RB_RENDER_COMPONENTS)==components);
        auto invalid=job;invalid.colorTargets[6].width=32;
        Adreno::Encoder rejected(words.data(),words.size());Adreno::PipelineEncoder reject(rejected,ib,scratch);
        assert(reject.draw(invalid,vs,*fragment,resolved,job.target.resource?0x4900000:0,sampled,0,0,0,targets)==Result::Invalid && !rejected.size());
        invalid=job;invalid.colorTargets[6].resource=invalid.target.resource;invalid.colorTargets[6].offset=invalid.target.offset;
        if(invalid.target.resource){uint32_t width,height;assert(!validAttachments(invalid,width,height));}
    }
    for(const char *name:{"constant_sample","air_constant_sample"}) {
        using namespace GPU::Shader;const Program *program=nullptr;
        for(const auto &p:GPUPrograms::Programs)if(!strcmp(p.name,name))program=&p;
        assert(program && !program->samplerMask && program->constantExplicitSamplerMask==1);
        auto job=dispatch;for(auto &image:job.images)image={};
        job.images[5]={20,0,16,16,256,RGBA8,5,0,(program->flags&QGPU_ARRAY_TEXTURE_MASK)?2u:0u};
        TextureLayout layout;assert(textureLayout(job.images[5],layout));
        auto resolved=buffers;resolved.addresses[13]=0x4800000;resolved.bytes[13]=uint32_t(layout.bytes);
        Adreno::Encoder commands(words.data(),words.size());Adreno::PipelineEncoder pipeline(commands,ib,scratch);
        assert(pipeline.dispatch(job,*program,resolved)==Result::Success);unsigned stateCount=0;
        for(unsigned i=0;i<commands.size();) {
            auto h=words[i++];if((h>>28)==4){i+=h&127;continue;}auto packet=Device::header(h);
            if(packet.first==0x30 && packet.second==7 && ((words[i]>>18)&15)==5) {
                assert((words[i]&0xffff)==program->constantSamplerLevelSlots[0]);++stateCount;
            }
            i+=packet.second;
        }
        assert(stateCount==1);auto invalid=*program;invalid.constantSamplerLevelSlots[0]=16;
        Adreno::Encoder rejected(words.data(),words.size());Adreno::PipelineEncoder reject(rejected,ib,scratch);
        assert(reject.dispatch(job,invalid,resolved)==Result::Invalid && !rejected.size());
    }
    {
        const GPU::Shader::Program *sample=nullptr;
        for(const auto &p:GPUPrograms::Programs)if(!strcmp(p.name,"sample_texture"))sample=&p;
        assert(sample && sample->sampleMask==32 && sample->samplerMask==4 && sample->explicitSamplerMask==4 && sample->textureCount==6);
        auto grid=dispatch;grid.images[5]={3,0,17,9,256,GPU::Shader::RGBA8,3};grid.images[7]={4,0,17,9,256,GPU::Shader::BGRA8,3};
        grid.samplers[2]={11,64,512};auto resolved=buffers;resolved.addresses[13]=0x4400000;resolved.addresses[15]=0x4500000;
        resolved.bytes[13]=resolved.bytes[15]=12288;
        Adreno::Encoder commands(words.data(),words.size());Adreno::PipelineEncoder pipeline(commands,ib,scratch);
        assert(pipeline.dispatch(grid,*sample,resolved)==Result::Success);
        unsigned checked=0;
        for(unsigned i=0;i<commands.size();) {
            auto header=words[i++];if((header>>28)==4){i+=header&127;continue;}auto packet=Device::header(header);
            if(packet.first==0x30 && ((words[i]>>18)&15)==5) {
                unsigned slot=words[i]&0x3fff;assert(words[i]>>22==1);
                if(packet.second==7){assert(slot==10);
                    assert(words[i+4]==(A5XX_TEX_SAMP_1_CUBEMAPSEAMLESSFILTOFF|(slot==2?(512u<<8)|(64u<<20):(4095u<<8))));checked|=slot==2?1:4;}
                else {assert(packet.second==15 && slot==5 && words[i+7]==0x4400000 && words[i+6]==A5XX_TEX_CONST_3_ARRAY_PITCH(12288));checked|=2;}
            }
            i+=packet.second;
        }
        assert(checked==6 && regValue(commands.size(),REG_A5XX_TPL1_CS_TEX_COUNT)==6 && regValue(commands.size(),REG_A5XX_TPL1_FS_TEX_COUNT)==0);
        assert(regValue(commands.size(),REG_A5XX_HLSQ_UPDATE_CNTL)==0x1f00000);
        uint32_t geometry[32];grid.samplers[0]={11,0,512};assert(!GPU::Shader::imageFootprint(grid,*sample,geometry));
        grid.samplers[0]={};grid.samplers[2].minLOD=513;assert(!GPU::Shader::imageFootprint(grid,*sample,geometry));
        grid.samplers[2]={11,64,512,GPU::Shader::ClampToBorderColor|(GPU::Shader::MirrorClampToEdge<<3),16,2,1};
        Adreno::Encoder border(words.data(),words.size());Adreno::PipelineEncoder bp(border,ib,scratch);
        assert(bp.dispatch(grid,*sample,resolved)==Result::Success);
        uint64_t base=regValue(border.size(),REG_A5XX_TPL1_TP_BORDER_COLOR_BASE_ADDR_LO)|
            (uint64_t(regValue(border.size(),REG_A5XX_TPL1_TP_BORDER_COLOR_BASE_ADDR_HI))<<32);
        assert(!(base&511) && base>=ib && (base-ib)/4+96<=words.size());
        const auto *table=words.data()+(base-ib)/4;
        for(unsigned i=0;i<32;++i)assert(!table[i]);
        assert(table[32+3]==0x3f800000 && table[32+12]==0xff000000 && table[32+14]==0xc0000000);
        assert(table[64+0]==0x3f800000 && table[64+4]==0xffffffff && table[64+8]==0x3c003c00 && table[64+10]==0xffffffff);
        assert(table[64+11]==0xffff && table[64+12]==0xffffffff && table[64+13]==0x7f7f7f7f && table[64+14]==0xffffffff && table[64+15]==0xffffff);
        unsigned samplerChecks=0;
        for(unsigned i=0;i<border.size();) {
            auto header=words[i++];if((header>>28)==4){i+=header&127;continue;}auto packet=Device::header(header);
            if(packet.first==0x30 && ((words[i]>>18)&15)==5 && packet.second==7){
                assert(words[i+3]==(A5XX_TEX_SAMP_0_MIPFILTER_LINEAR_NEAR|A5XX_TEX_SAMP_0_XY_MIN(2)|A5XX_TEX_SAMP_0_XY_MAG(2)|
                    A5XX_TEX_SAMP_0_ANISO(4)|A5XX_TEX_SAMP_0_WRAP_S(3)|A5XX_TEX_SAMP_0_WRAP_T(1)|A5XX_TEX_SAMP_0_WRAP_R(1)));
                assert(words[i+5]==(2u<<7));samplerChecks|=1;
            }
            if(packet.first==0x30 && ((words[i]>>18)&15)==13 && ((words[i]>>16)&3)==0){
                assert(words[i+3+50]==2 && sample->driverBase>=64 && sample->immediateBase>=64);samplerChecks|=2;
            }
            i+=packet.second;
        }
        assert(samplerChecks==3);
        grid.samplers[2].addressModes=GPU::Shader::ClampToBorderColor|(GPU::Shader::ClampToZero<<3);assert(!GPU::Shader::imageFootprint(grid,*sample,geometry));
        grid.samplers[2]={3,0,32,0,1,0,0};assert(GPU::Shader::imageFootprint(grid,*sample,geometry));
        Adreno::Encoder pixel(words.data(),words.size());Adreno::PipelineEncoder pp(pixel,ib,scratch);
        assert(pp.dispatch(grid,*sample,resolved)==Result::Success);
        bool unnormalized=false;
        for(unsigned i=0;i<pixel.size();) {auto header=words[i++];if((header>>28)==4){i+=header&127;continue;}auto packet=Device::header(header);
            if(packet.first==0x30 && ((words[i]>>18)&15)==5 && packet.second==7)unnormalized|=words[i+4]&A5XX_TEX_SAMP_1_UNNORM_COORDS;
            i+=packet.second;}
        assert(unnormalized);grid.samplers[2].addressModes=GPU::Shader::Repeat;assert(!GPU::Shader::imageFootprint(grid,*sample,geometry));

    }
    {
        GPU::MetalHeap::Allocator heap;GPU::MetalHeap::Range first{},second{},reuse{};uint64_t heapBytes=0;
        assert(GPU::MetalHeap::bufferSize(65,heapBytes) && heapBytes==128);
        assert(!GPU::MetalHeap::bufferSize(UINT64_MAX,heapBytes));
        assert(heap.initialize(1024,false) && heap.allocate(128,64,first) && first.offset==0);
        assert(heap.allocate(256,256,second) && second.offset==256 && heap.used()==384 && heap.available(64)==512);
        assert(heap.release(first.token) && heap.allocate(64,64,reuse) && reuse.offset==0 && reuse.token!=first.token);
        assert(!heap.release(first.token) && heap.used()==320 && heap.available(3)==0);
        assert(heap.release(reuse.token) && heap.release(second.token) && heap.used()==0 && heap.available(0)==1024);
        GPU::MetalHeap::Allocator placement;assert(placement.initialize(4096,true));
        assert(placement.allocate(256,64,first,64) && placement.allocate(256,64,second,128));
        assert(placement.used()==320 && placement.available(64)==4096);
        assert(placement.release(first.token) && placement.used()==256);
        assert(!placement.allocate(64,64,reuse,1) && !placement.allocate(128,64,reuse,4032));
        GPU::Shader::Texture linear{};GPU::Shader::TextureLayout checked;
        for(auto format:{GPU::Shader::RGBA8,GPU::Shader::BGRA8,GPU::Shader::RGBA8SRGB,GPU::Shader::BGRA8SRGB,GPU::Shader::R8,GPU::Shader::RG8,GPU::Shader::A8,GPU::Shader::RGBA16Float,GPU::Shader::RG32Float,GPU::Shader::RGBA16Unorm}) {
            assert(GPU::MetalTexture::linearTextureAlignment(format)==64);
            assert(GPU::MetalTexture::bufferTexture(9,768,128,13,5,128,format,0,linear,checked));
            assert(linear.resource==9 && linear.offset==128 && linear.pitch==128 && !linear.mipLevels && checked.accessBytes==640);
            GPU::Shader::Texture child{};
            assert(GPU::Shader::textureView(linear,0,1,0,1,false,format,child) && child.offset==128 && child.resource==9);
            assert(!GPU::MetalTexture::bufferTexture(9,767,128,13,5,128,format,0,linear,checked) && GPU::Shader::emptyTexture(linear));
            assert(GPU::MetalTexture::bufferTexture(9,768,128,13,5,128,format,0,linear,checked,640));
            assert(!GPU::MetalTexture::bufferTexture(9,768,128,13,5,128,format,0,linear,checked,639));
            assert(!GPU::MetalTexture::bufferTexture(9,768,128,13,5,128,format,0,linear,checked,641));
        }
        assert(!GPU::MetalTexture::linearTextureAlignment(GPU::Shader::Depth32));
        assert(!GPU::MetalTexture::bufferTexture(9,1024,UINT64_MAX,13,5,128,GPU::Shader::RGBA8,0,linear,checked));
        assert(!GPU::MetalTexture::bufferTexture(9,1024,64,UINT64_MAX,5,128,GPU::Shader::RGBA8,0,linear,checked));
        assert(!GPU::MetalTexture::bufferTexture(9,1024,1,13,5,128,GPU::Shader::RGBA8,0,linear,checked));
        assert(!GPU::MetalTexture::bufferTexture(9,1024,64,17,5,64,GPU::Shader::RGBA8,0,linear,checked));
        assert(!GPU::MetalTexture::bufferTexture(9,1024,64,13,5,129,GPU::Shader::RGBA8,0,linear,checked));
        assert(!GPU::MetalTexture::bufferTexture(0,1024,64,13,5,128,GPU::Shader::RGBA8,0,linear,checked));
        GPU::Shader::Texture chain{9,4096,130,65,768,GPU::Shader::BGRA8,8};GPU::Shader::TextureLayout layout;
        assert(GPU::Shader::textureLayout(chain,layout) && layout.levels==8 && layout.bytes==86016);
        assert(layout.accessBytes==74496);
        for (auto format:{GPU::Shader::RGBA8,GPU::Shader::BGRA8,GPU::Shader::RGBA8SRGB,GPU::Shader::BGRA8SRGB}) {
            auto color=chain;color.format=format;GPU::Shader::TextureLayout other;
            assert(GPU::Shader::textureLayout(color,other) && !memcmp(&layout,&other,sizeof(layout)));
        }
        const uint32_t offsets[]={0,49920,66304,70400,72448,73472,73984,74240};
        const uint32_t pitches[]={768,512,256,256,256,256,256,256};
        for (unsigned i=0;i<8;++i) {assert(layout.level[i].offset==offsets[i] && layout.level[i].pitch==pitches[i]);}
        GPU::Shader::Texture level{};
        assert(GPU::Shader::textureLevel(chain,7,level) && level.offset==78336 && level.width==1 && level.height==1 && level.pitch==256 && !level.mipLevels);
        assert(GPU::Shader::overlap(chain,level) && !GPU::Shader::textureLevel(chain,8,level));
        GPU::Shader::Texture view{},nested{},expectedLevel{};uint64_t viewBytes=0;
        assert(GPU::Shader::textureSubview(chain,1,2,GPU::Shader::RGBA8SRGB,view));
        assert(view.offset==54016 && view.width==65 && view.height==32 && view.pitch==512 && view.mipLevels==2);
        assert(GPU::Shader::textureSize(view,viewBytes) && viewBytes==20480);
        assert(GPU::Shader::textureSubview(view,1,1,GPU::Shader::BGRA8,nested) &&
            GPU::Shader::textureLevel(chain,2,expectedLevel));
        assert(nested.offset==expectedLevel.offset && nested.width==expectedLevel.width && nested.height==expectedLevel.height &&
            nested.pitch==expectedLevel.pitch && !nested.mipLevels && nested.format==GPU::Shader::BGRA8);
        assert(!GPU::Shader::textureSubview(view,1,2,GPU::Shader::RGBA8,nested) && GPU::Shader::emptyTexture(nested));
        assert(!GPU::Shader::textureSubview(chain,0,0,GPU::Shader::RGBA8,nested));
        assert(!GPU::Shader::textureSubview(chain,0,1,GPU::Shader::Depth32,nested));
        for(auto format:{GPU::Shader::R8,GPU::Shader::RG8}) {
            auto mask=chain;mask.format=format;mask.pitch=192*GPU::Shader::formatBytes(format);GPU::Shader::TextureLayout small;
            assert(GPU::Shader::textureLayout(mask,small) && small.bytes==(format==GPU::Shader::R8?24576u:45056u));
            assert(GPU::Shader::textureSubview(mask,1,2,format,nested) && nested.pitch==128*GPU::Shader::formatBytes(format));
            assert(!GPU::Shader::textureSubview(mask,0,1,GPU::Shader::RGBA8,nested));
        }
        auto overflow=chain;overflow.offset=0xffffffc0;
        assert(!GPU::Shader::textureSubview(overflow,1,1,GPU::Shader::RGBA8,nested));
        assert(GPU::Shader::textureLevel(chain,3,nested) && !GPU::Shader::overlap(view,nested));
        auto parentSwizzle=GPU::Shader::packSwizzle(2,1,1,3);
        auto relativeSwizzle=GPU::Shader::packSwizzle(2,3,5,4);
        assert(GPU::Shader::composeSwizzle(parentSwizzle,relativeSwizzle)==GPU::Shader::packSwizzle(2,1,3,1));
        auto patterned=chain;patterned.swizzle=parentSwizzle;
        assert(GPU::Shader::textureSubview(patterned,1,2,GPU::Shader::RGBA8,view,relativeSwizzle) &&
            view.swizzle==GPU::Shader::packSwizzle(2,1,3,1));
        assert(GPU::Shader::validSwizzle(GPU::Shader::packSwizzle(0,0,0,0)) && GPU::Shader::packSwizzle(0,0,0,0));
        assert(!GPU::Shader::validSwizzle(7) && !GPU::Shader::validSwizzle(4097));
        auto identity=GPU::Shader::packSwizzle(2,3,4,5);assert(!identity);
        for (unsigned a=0;a<6;++a) for (unsigned b=0;b<6;++b) for (unsigned c=0;c<6;++c) for (unsigned d=0;d<6;++d) {
            uint32_t relative=GPU::Shader::packSwizzle(a,b,c,d),effective=GPU::Shader::composeSwizzle(parentSwizzle,relative);
            const unsigned original[]={0,1,17,31,63,127},parent[]={17,1,1,31},selectors[]={a,b,c,d};
            assert(GPU::Shader::validSwizzle(effective));
            for (unsigned i=0;i<4;++i) assert(original[GPU::Shader::swizzleChannel(effective,i)]==
                (selectors[i]<2?selectors[i]:parent[selectors[i]-2]));
        }
        for (auto owner:{chain,GPU::Shader::Texture{10,0,259,129,1280,GPU::Shader::RGBA8,9},
            GPU::Shader::Texture{11,0,1,16384,256,GPU::Shader::BGRA8SRGB,15}}) {
            GPU::Shader::TextureLayout original;assert(GPU::Shader::textureLayout(owner,original));
            for (unsigned first=0;first<original.levels;++first) for (unsigned count=1;count<=original.levels-first;++count) {
                GPU::Shader::Texture child;GPU::Shader::TextureLayout childLayout;
                assert(GPU::Shader::textureSubview(owner,first,count,GPU::Shader::RGBA8,child) &&
                    GPU::Shader::textureLayout(child,childLayout,false));
                for (unsigned i=0;i<count;++i) {
                    const auto &a=original.level[first+i],&b=childLayout.level[i];
                    assert(uint64_t(child.offset)+b.offset==uint64_t(owner.offset)+a.offset &&
                        a.width==b.width && a.height==b.height && a.pitch==b.pitch);
                }
            }
        }
        auto narrow=chain;narrow.width=1;narrow.height=16384;narrow.pitch=256;narrow.mipLevels=15;
        assert(GPU::Shader::textureLayout(narrow,layout) && layout.bytes==8396800);
        for (unsigned i=0;i<15;++i) assert(layout.level[i].pitch==256);
        auto malformed=chain;malformed.mipLevels=9;assert(!GPU::Shader::textureLayout(malformed,layout));
        malformed=chain;malformed.pitch=704;assert(!GPU::Shader::textureLayout(malformed,layout));
        const GPU::Shader::Program *mip=nullptr;
        for (const auto &p:GPUPrograms::Programs) if (!strcmp(p.name,"mip_textured")) mip=&p;
        assert(mip && mip->textureCount==1);
        auto mipDraw=draw;mipDraw.textures[0]=chain;mipDraw.samplers[0]={11,64,1792};
        mipDraw.textures[0].format=GPU::Shader::BGRA8SRGB;mipDraw.target.format=GPU::Shader::RGBA8SRGB;
        mipDraw.textures[0].swizzle=GPU::Shader::packSwizzle(5,2,0,1);
        mipDraw.target.swizzle=GPU::Shader::packSwizzle(0,0,0,0); // Writes ignore the read swizzle.
        Adreno::Encoder commands(words.data(),words.size());Adreno::PipelineEncoder pipeline(commands,ib,scratch);
        assert(pipeline.draw(mipDraw,vs,*mip,buffers,0x4200000,Adreno::GraphicsTextures{{0x4301000},{0}})==Result::Success);
        assert(regValue(commands.size(),REG_A5XX_RB_MRT_BUF_INFO(0))==(A5XX_RB_MRT_BUF_INFO_COLOR_FORMAT(0x30)|A5XX_RB_MRT_BUF_INFO_COLOR_SRGB));
        assert(regValue(commands.size(),REG_A5XX_SP_FS_MRT_REG(0))==(A5XX_SP_FS_MRT_REG_COLOR_FORMAT(0x30)|A5XX_SP_FS_MRT_REG_COLOR_SRGB));
        unsigned tables=0;
        for (unsigned i=0;i<commands.size();) {
            auto header=words[i++];if ((header>>28)==4) {i+=header&127;continue;}
            auto packet=Device::header(header);
            if (packet.first==0x30 && ((words[i]>>18)&15)==4) {
                if (packet.second==7) {
                    assert(words[i+3]&A5XX_TEX_SAMP_0_MIPFILTER_LINEAR_NEAR);
                    unsigned slot=words[i]&0x3fff;assert(slot==8);
                    assert(words[i+4]==(A5XX_TEX_SAMP_1_CUBEMAPSEAMLESSFILTOFF|(slot?(4095u<<8):(1792u<<8)|(64u<<20))));tables|=slot?4:1;
                } else if (packet.second==15) {
                    assert(words[i+3]&A5XX_TEX_CONST_0_SRGB);
                    assert((words[i+3]&(A5XX_TEX_CONST_0_SWIZ_X__MASK|A5XX_TEX_CONST_0_SWIZ_Y__MASK|A5XX_TEX_CONST_0_SWIZ_Z__MASK|A5XX_TEX_CONST_0_SWIZ_W__MASK))==
                        (A5XX_TEX_CONST_0_SWIZ_X(3)|A5XX_TEX_CONST_0_SWIZ_Y(2)|A5XX_TEX_CONST_0_SWIZ_Z(4)|A5XX_TEX_CONST_0_SWIZ_W(5)));
                    assert((words[i+3]&A5XX_TEX_CONST_0_MIPLVLS__MASK)==A5XX_TEX_CONST_0_MIPLVLS(7));
                    assert(words[i+5]==(A5XX_TEX_CONST_2_PITCH(768)|A5XX_TEX_CONST_2_PITCHALIGN(2)|A5XX_TEX_CONST_2_TYPE(1)));
                    assert(words[i+6]==A5XX_TEX_CONST_3_ARRAY_PITCH(86016) && words[i+7]==0x4301000);tables|=2;
                }
            }
            i+=packet.second;
        }
        assert(tables==6);
        // Native UI glyph masks and its half-float lookup texture retain
        // their original bytes. Verify the emitted hardware format/swizzle,
        // including a view which moves logical alpha to red.
        for(unsigned variant=0;variant<6;++variant) {
            bool compressed=variant>=4,wide=variant>=2;
            auto format=variant==5?GPU::Shader::ASTC4x4SRGB:variant==4?GPU::Shader::ASTC4x4:variant==3?GPU::Shader::RGBA16Unorm:variant==2?GPU::Shader::RGBA16Float:GPU::Shader::A8;
            auto sampled=mipDraw;sampled.textures[0]={9,0,17,3,compressed?1024u:wide?192u:64u,format};
            if(variant==1)sampled.textures[0].swizzle=GPU::Shader::packSwizzle(5,1,2,0);
            Adreno::Encoder stream(words.data(),words.size());Adreno::PipelineEncoder encode(stream,ib,scratch);
            assert(encode.draw(sampled,vs,*mip,buffers,0x4200000,Adreno::GraphicsTextures{{0x4301000},{0}})==Result::Success);
            unsigned seen=0;
            for(unsigned at=0;at<stream.size();) {
                auto header=words[at++];if((header>>28)==4){at+=header&127;continue;}auto packet=Device::header(header);
                if(packet.first==0x30 && packet.second==15 && ((words[at]>>18)&15)==4) {
                    uint32_t swizzle=wide?(A5XX_TEX_CONST_0_SWIZ_X(0)|A5XX_TEX_CONST_0_SWIZ_Y(1)|A5XX_TEX_CONST_0_SWIZ_Z(2)|A5XX_TEX_CONST_0_SWIZ_W(3)):
                        variant==1?(A5XX_TEX_CONST_0_SWIZ_X(0)|A5XX_TEX_CONST_0_SWIZ_Y(5)|A5XX_TEX_CONST_0_SWIZ_Z(4)|A5XX_TEX_CONST_0_SWIZ_W(4)):
                        (A5XX_TEX_CONST_0_SWIZ_X(4)|A5XX_TEX_CONST_0_SWIZ_Y(4)|A5XX_TEX_CONST_0_SWIZ_Z(4)|A5XX_TEX_CONST_0_SWIZ_W(0));
                    assert(words[at+3]==(A5XX_TEX_CONST_0_FMT(compressed?0xc1:variant==3?0x60:variant==2?0x62:0x03)|swizzle|
                        (variant==5?A5XX_TEX_CONST_0_SRGB:0)));
                    if(compressed)assert(words[at+5]==(A5XX_TEX_CONST_2_PITCH(1024)|A5XX_TEX_CONST_2_PITCHALIGN(4)|A5XX_TEX_CONST_2_TYPE(1)));
                    ++seen;
                }
                at+=packet.second;
            }
            assert(seen==1);
            auto attachment=sampled;attachment.target=sampled.textures[0];uint32_t w=0,h=0;
            assert(GPU::Shader::validAttachments(attachment,w,h)==(format==GPU::Shader::RGBA16Float));
        }
        // Native GA8/LA08 uses RG8 identity: QuartzCore's image function
        // expands gray/alpha in the shader. Other callers may request an
        // explicit (r,r,r,g) view; do not force that onto native GA8 sampling.
        for(bool grayView:{false,true}) {
            auto sampled=mipDraw;sampled.textures[0]={9,0,768,48,1536,GPU::Shader::RG8};
            if(grayView)sampled.textures[0].swizzle=GPU::Shader::packSwizzle(GPU::Shader::ChannelRed,GPU::Shader::ChannelRed,
                GPU::Shader::ChannelRed,GPU::Shader::ChannelGreen);
            Adreno::Encoder stream(words.data(),words.size());Adreno::PipelineEncoder encode(stream,ib,scratch);
            assert(encode.draw(sampled,vs,*mip,buffers,0x4200000,Adreno::GraphicsTextures{{0x4301000},{0}})==Result::Success);
            unsigned seen=0;
            for(unsigned at=0;at<stream.size();) {
                auto header=words[at++];if((header>>28)==4){at+=header&127;continue;}auto packet=Device::header(header);
                if(packet.first==0x30 && packet.second==15 && ((words[at]>>18)&15)==4) {
                    assert(words[at+3]==(A5XX_TEX_CONST_0_FMT(0x0f)|A5XX_TEX_CONST_0_SWIZ_X(0)|
                        A5XX_TEX_CONST_0_SWIZ_Y(grayView?0:1)|A5XX_TEX_CONST_0_SWIZ_Z(grayView?0:4)|A5XX_TEX_CONST_0_SWIZ_W(grayView?1:5)));
                    assert(words[at+5]==(A5XX_TEX_CONST_2_PITCH(1536)|A5XX_TEX_CONST_2_TYPE(1)));++seen;
                }
                at+=packet.second;
            }
            assert(seen==1);
        }
        mipDraw.samplers[0]={15,0,256};Adreno::Encoder rejected(words.data(),words.size());Adreno::PipelineEncoder reject(rejected,ib,scratch);
        assert(reject.draw(mipDraw,vs,*mip,buffers,0x4200000,Adreno::GraphicsTextures{{0x4301000},{0}})==Result::Invalid && !rejected.size());
    }
    {
        const GPU::Shader::Program *dynamic=nullptr;
        for (const auto &p:GPUPrograms::Programs) if (!strcmp(p.name,"dynamic_threadgroup")) dynamic=&p;
        assert(dynamic && dynamic->sharedBytes==16 && dynamic->threadgroupMask==0x89);
        auto grid=dispatch;grid.groups[0]=3;grid.local[0]=7;
        grid.threadgroupLengths[0]=32;grid.threadgroupLengths[3]=112;grid.threadgroupLengths[7]=16;
        uint32_t memory[16],total=0;uint64_t elements=0;unsigned dimensions=0;
        const uint32_t expected[16]={0,32,0,0,0,0,32,112,0,0,0,0,0,0,144,16};
        assert(GPU::Shader::threadgroupFootprint(grid,*dynamic,memory,total) && total==176 && !memcmp(memory,expected,sizeof(memory)));
        Adreno::Encoder commands(words.data(),words.size());Adreno::PipelineEncoder pipeline(commands,ib,scratch);
        assert(pipeline.dispatch(grid,*dynamic,buffers)==Result::Success);
        bool constants=false;
        for (unsigned i=0;i<commands.size();) {
            auto header=words[i++];if ((header>>28)==4) {i+=header&127;continue;}
            auto packet=Device::header(header);
            if (packet.first==0x30 && ((words[i]>>18)&15)==13 && ((words[i]>>16)&3)==0) {
                const auto *values=words.data()+i+3;
                assert(packet.second==dynamic->constlen*4+3 && !memcmp(values,expected,sizeof(expected)));
                assert(dynamic->driverBase>=16 && values[dynamic->driverBase+8]==7);
                assert(dynamic->immediateBase>=16 && !memcmp(values+dynamic->immediateBase,dynamic->immediates,dynamic->immediateCount*4));
                constants=true;
            }
            i+=packet.second;
        }
        assert(constants);
        grid.threadgroupLengths[0]=64;
        assert(GPU::Shader::threadgroupFootprint(grid,*dynamic,memory,total) && total==208 && memory[6]==64 && memory[14]==176);
        grid.threadgroupLengths[0]=32720;grid.threadgroupLengths[3]=grid.threadgroupLengths[7]=16;
        assert(GPU::Shader::dispatchFootprint(grid,*dynamic,elements,dimensions));
        grid.threadgroupLengths[0]+=16;
        Adreno::Encoder rejected(words.data(),words.size());Adreno::PipelineEncoder reject(rejected,ib,scratch);
        assert(reject.dispatch(grid,*dynamic,buffers)==Result::Invalid && !rejected.size());
        for (uint32_t invalid:{0u,31u,UINT32_MAX}) {
            grid.threadgroupLengths[0]=invalid;
            assert(!GPU::Shader::dispatchFootprint(grid,*dynamic,elements,dimensions));
        }
        grid.threadgroupLengths[0]=16;grid.threadgroupLengths[2]=16;
        assert(!GPU::Shader::dispatchFootprint(grid,*dynamic,elements,dimensions));
    }
    {
        const GPU::Shader::Program *fragment=nullptr;
        for (const auto &p:GPUPrograms::Programs) if (!strcmp(p.name,"buffer_color")) fragment=&p;
        assert(fragment && fragment->bufferMask==131 && (fragment->flags&26)==26);
        auto both=buffers;both.addresses[8]=0x4400000;both.bytes[8]=65552;
        both.addresses[9]=0x4500000;both.bytes[9]=32;both.addresses[15]=0x4600000;both.bytes[15]=256;
        auto fragmentDraw=draw;fragmentDraw.textures[0]={};
        Adreno::Encoder graphics(words.data(),words.size());Adreno::PipelineEncoder graphicsPipeline(graphics,ib,scratch);
        assert(graphicsPipeline.draw(fragmentDraw,vs,*fragment,both,0x4200000,Adreno::GraphicsTextures{{0},{0}})==Result::Success);
        assert(regValue(graphics.size(),REG_A5XX_HLSQ_FS_CNTL)&A5XX_HLSQ_FS_CNTL_SSBO_ENABLE);
        assert(regValue(graphics.size(),REG_A5XX_HLSQ_VS_CNTL)&A5XX_HLSQ_VS_CNTL_SSBO_ENABLE);
        unsigned tables=0;
        for (unsigned i=0;i<graphics.size();) {
            uint32_t header=words[i++];if ((header>>28)==4) {i+=header&127;continue;}
            auto packet=Device::header(header);
            if (packet.first==0x30 && ((words[i]>>18)&15)==14) {
                assert(packet.second==35 && (words[i]>>22)==16);
                if (words[i+1]==1) {
                    assert(words[i+3]==A5XX_SSBO_1_0_WIDTH(4096) && !words[i+4]);
                    assert(words[i+3+8*2]==A5XX_SSBO_1_0_WIDTH(16) && words[i+4+8*2]==A5XX_SSBO_1_1_HEIGHT(1));
                    assert(words[i+3+9*2]==A5XX_SSBO_1_0_WIDTH(32) && words[i+3+15*2]==A5XX_SSBO_1_0_WIDTH(256));tables|=1;
                } else {assert(words[i+1]==2 && words[i+3]==0x4000000 && words[i+3+8*2]==0x4400000 && words[i+3+15*2]==0x4600000);tables|=2;}
            }
            i+=packet.second;
        }
        assert(tables==3);
        auto oldBank=*fragment;oldBank.flags&=~QGPU_FRAGMENT_BUFFER_BANK;
        Adreno::Encoder rejected(words.data(),words.size());Adreno::PipelineEncoder rejectedPipeline(rejected,ib,scratch);
        assert(rejectedPipeline.draw(fragmentDraw,vs,oldBank,both,0x4200000,Adreno::GraphicsTextures{{0},{0}})==Result::Invalid);
    }
    {
        auto grid=dispatch;grid.groups[0]=3;grid.groups[1]=5;grid.groups[2]=2;
        grid.local[0]=7;grid.local[1]=3;grid.local[2]=2;
        uint64_t elements=0;unsigned dimensions=0;
        assert(GPU::Shader::dispatchFootprint(grid,cs,elements,dimensions) && elements==21 && dimensions==3);
        Adreno::Encoder variable(words.data(),words.size());Adreno::PipelineEncoder variablePipeline(variable,ib,scratch);
        assert(variablePipeline.dispatch(grid,cs,buffers)==Result::Success);
        assert(regValue(variable.size(),REG_A5XX_HLSQ_CS_NDRANGE_0)==(3u|(6u<<2)|(2u<<12)|(1u<<22)));
        assert(regValue(variable.size(),REG_A5XX_HLSQ_CS_NDRANGE_1)==21);
        assert(regValue(variable.size(),REG_A5XX_HLSQ_CS_NDRANGE_3)==15);
        assert(regValue(variable.size(),REG_A5XX_HLSQ_CS_NDRANGE_5)==4);
        bool constants=false,launch=false;
        for (unsigned i=0;i<variable.size();) {
            auto header=words[i++];if ((header>>28)==4) { i+=header&0x7f;continue; }
            auto packet=Device::header(header);
            if (packet.first==0x30 && ((words[i]>>18)&15)==13 && ((words[i]>>16)&3)==0) {
                assert(packet.second==cs.constlen*4+3);
                const auto *driver=words.data()+i+3+cs.driverBase;
                assert(cs.driverCount>=11 && driver[0]==3 && driver[1]==5 && driver[2]==2 && driver[3]==3);
                assert(driver[8]==7 && driver[9]==3 && driver[10]==2);constants=true;
            }
            if (packet.first==0x33) { assert(words[i+1]==3 && words[i+2]==5 && words[i+3]==2);launch=true; }
            i+=packet.second;
        }
        assert(constants && launch);
        uint64_t required=0;
        assert(GPU::Shader::bindingBytes(cs,0,100000,0,required) && required==cs.strides[0]);
        auto unguarded=cs;unguarded.flags&=~QGPU_BOUNDED_BUFFERS;
        assert(GPU::Shader::bindingBytes(unguarded,0,100000,0,required) && required==100000ull*cs.strides[0]);
        auto legacy=cs;legacy.flags&=~QGPU_VARIABLE_LOCAL_SIZE;
        assert(!GPU::Shader::dispatchFootprint(grid,legacy,elements,dimensions));
        auto partial=grid;partial.grid[0]=13;partial.grid[1]=7;partial.grid[2]=5;
        partial.local[0]=4;partial.local[1]=3;partial.local[2]=2;
        partial.groups[0]=4;partial.groups[1]=3;partial.groups[2]=3;
        GPU::Shader::DispatchTile tiles[8];uint32_t tileCount=0;
        assert(GPU::Shader::dispatchFootprint(partial,cs,elements,dimensions) && elements==13 && dimensions==3 &&
            GPU::Shader::dispatchTiles(partial,tiles,tileCount) && tileCount==8);
        std::vector<unsigned> coverage(13*7*5,0);
        for (const auto &tile:tiles) for (unsigned z=0;z<tile.groups[2]*tile.local[2];++z)
            for (unsigned y=0;y<tile.groups[1]*tile.local[1];++y) for (unsigned x=0;x<tile.groups[0]*tile.local[0];++x) {
                unsigned gx=tile.baseThread[0]+x,gy=tile.baseThread[1]+y,gz=tile.baseThread[2]+z;
                assert(gx<13 && gy<7 && gz<5 && gx/4==tile.baseGroup[0]+x/tile.local[0] &&
                    gy/3==tile.baseGroup[1]+y/tile.local[1] && gz/2==tile.baseGroup[2]+z/tile.local[2]);
                ++coverage[gx+13*(gy+7*gz)];
            }
        assert(std::all_of(coverage.begin(),coverage.end(),[](unsigned n){return n==1;}));
        Adreno::Encoder tails(words.data(),words.size());Adreno::PipelineEncoder tailPipeline(tails,ib,scratch);
        assert(tailPipeline.dispatch(partial,cs,buffers)==Result::Success);
        unsigned region=0,constantRegions=0;
        for (unsigned i=0;i<tails.size();) {
            auto h=words[i++];if ((h>>28)==4) {i+=h&127;continue;}auto packet=Device::header(h);
            if (packet.first==0x30 && ((words[i]>>18)&15)==13 && ((words[i]>>16)&3)==0) {
                uint32_t expected[16];GPU::Shader::dispatchConstants(partial,tiles[region],expected);
                const auto *values=words.data()+i+3;
                assert(cs.flags&QGPU_DISPATCH_GEOMETRY);
                assert(cs.driverBase>=80 && !memcmp(values+QGPU_DISPATCH_CONSTANT_BASE,expected,sizeof(expected)));
                for (unsigned axis=0;axis<3;++axis) assert(values[cs.driverBase+axis]==tiles[region].groups[axis] &&
                    values[cs.driverBase+8+axis]==tiles[region].local[axis]);
                ++constantRegions;
            }
            if (packet.first==0x33) {for(unsigned a=0;a<3;++a)assert(words[i+1+a]==tiles[region].groups[a]);++region;}
            i+=packet.second;
        }
        assert(region==8 && constantRegions==8);
        auto malformed=partial;malformed.reserved=1;assert(!GPU::Shader::dispatchGeometry(malformed,elements,dimensions));
        malformed=partial;malformed.groups[0]=3;assert(!GPU::Shader::dispatchGeometry(malformed,elements,dimensions));
        malformed=partial;malformed.grid[1]=0;assert(!GPU::Shader::dispatchGeometry(malformed,elements,dimensions));
        grid=dispatch;grid.local[0]=1024;grid.groups[0]=16384;
        assert(GPU::Shader::dispatchFootprint(grid,cs,elements,dimensions) && elements==16777216);
        ++grid.groups[0];assert(!GPU::Shader::dispatchFootprint(grid,cs,elements,dimensions));
        grid=dispatch;grid.local[1]=0;
        Adreno::Encoder invalid(words.data(),words.size());Adreno::PipelineEncoder invalidPipeline(invalid,ib,scratch);
        assert(invalidPipeline.dispatch(grid,cs,buffers)==Result::Invalid && invalid.size()==0);
    }
    auto state = draw.state;
    state.blend = 1; state.writeMask = 9; state.cull = GPU::Shader::CullBack;
    state.rgbOperation = GPU::Shader::Add; state.sourceRGB = GPU::Shader::SourceAlpha;
    state.destinationRGB = GPU::Shader::OneMinusSourceAlpha;
    state.sourceAlpha = GPU::Shader::One; state.destinationAlpha = GPU::Shader::Zero;
    state.viewport[0] = 0x41800000; state.viewport[1] = 0x41000000; // offset 16, scale 8
    state.viewport[2] = 0x41c00000; state.viewport[3] = 0xc1800000; // offset 24, scale -16
    // Compare the kernel's integer conversions with host float32 arithmetic
    // and binary16 conversion, including rounding and tiny constants.
    const float constants[] = {0.0f,0.5f,1.0f,1.0f/3.0f,0x1p-24f,0x1.fffffep-1f,254.0f/255.0f};
    for (float value : constants) {
        uint32_t bits; memcpy(&bits,&value,4);
        for (auto &color : state.blendColor) color = bits;
        draw.state = state;
        Adreno::Encoder custom(words.data(),words.size()); Adreno::PipelineEncoder customPipeline(custom,ib,scratch);
        assert(customPipeline.draw(draw,vs,fs,buffers,0x4200000,Adreno::GraphicsTextures{{0x4300000},{0}}) == Result::Success);
        _Float16 half = (_Float16)value; uint16_t halfBits; memcpy(&halfBits,&half,2);
        uint32_t expected = (uint32_t(halfBits)<<16) | (uint32_t(value*127.0f)<<8) | uint32_t(value*255.0f);
        assert(regValue(custom.size(),REG_A5XX_RB_BLEND_RED) == expected);
        assert(regValue(custom.size(),REG_A5XX_RB_BLEND_RED_F32) == bits);
        assert(regValue(custom.size(),REG_A5XX_RB_MRT_CONTROL(0)) ==
            (A5XX_RB_MRT_CONTROL_ROP_CODE(12)|A5XX_RB_MRT_CONTROL_COMPONENT_ENABLE(9)|A5XX_RB_MRT_CONTROL_BLEND|A5XX_RB_MRT_CONTROL_BLEND2));
        assert(regValue(custom.size(),REG_A5XX_RB_MRT_BLEND_CONTROL(0)) == (6u | (7u<<8) | (1u<<16)));
        assert(regValue(custom.size(),REG_A5XX_GRAS_SU_CNTL) == (A5XX_GRAS_SU_CNTL_CULL_BACK|A5XX_GRAS_SU_CNTL_FRONT_CW));
        assert(regValue(custom.size(),REG_A5XX_GRAS_CL_VPORT_XSCALE_0) == state.viewport[1]);
        assert(regValue(custom.size(),REG_A5XX_SP_BLEND_CNTL) == (0x100u|1));
    }
    draw.state = GPU::Shader::defaultRenderState(64,64);
    Adreno::Encoder reset(words.data(),words.size()); Adreno::PipelineEncoder resetPipeline(reset,ib,scratch);
    assert(resetPipeline.draw(draw,vs,fs,buffers,0x4200000,Adreno::GraphicsTextures{{0x4300000},{0}}) == Result::Success);
    assert(regValue(reset.size(),REG_A5XX_RB_MRT_BLEND_CONTROL(0)) == 0);
    assert(regValue(reset.size(),REG_A5XX_RB_BLEND_RED) == 0);
    assert(regValue(reset.size(),REG_A5XX_RB_MRT_CONTROL(0)) == (A5XX_RB_MRT_CONTROL_ROP_CODE(12)|A5XX_RB_MRT_CONTROL_COMPONENT_ENABLE(15)));
    auto indexed = draw;
    indexed.primitive = 4; indexed.count = 4; indexed.indexSize = 2; indexed.instances = 2;
    indexed.baseInstance = 7; indexed.baseVertex = -1; indexed.indices = {9,4,8,0};
    uint16_t indices[] = {1,2,0xffff,3}; uint64_t elements = 0;
    assert(GPU::Shader::indexFootprint(indexed,indices,elements) && elements == 3);
    const auto &instanced = *GPUPrograms::lookup(5);
    assert(instanced.instanceID != 252 && instanced.instanceMask == 2);
    Adreno::Encoder indexedCommands(words.data(),words.size()); Adreno::PipelineEncoder indexedPipeline(indexedCommands,ib,scratch);
    assert(indexedPipeline.draw(indexed,instanced,fs,buffers,0x4200000,Adreno::GraphicsTextures{{0x4300000},{0}},0x4400000) == Result::Success);
    assert(regValue(indexedCommands.size(),REG_A5XX_PC_RESTART_INDEX) == 0xffff);
    assert(regValue(indexedCommands.size(),REG_A5XX_PC_PRIMITIVE_CNTL)&A5XX_PC_PRIMITIVE_CNTL_PRIMITIVE_RESTART);
    assert(regValue(indexedCommands.size(),REG_A5XX_VFD_INDEX_OFFSET) == UINT32_MAX);
    assert(regValue(indexedCommands.size(),REG_A5XX_VFD_INSTANCE_START_OFFSET) == 7);
    assert((regValue(indexedCommands.size(),REG_A5XX_VFD_CONTROL_1)&A5XX_VFD_CONTROL_1_REGID4INST__MASK) == A5XX_VFD_CONTROL_1_REGID4INST(instanced.instanceID));
    bool indexedPacket = false, instanceConstant = false;
    for (unsigned i=0;i<indexedCommands.size();) {
        uint32_t header = words[i++];
        if ((header>>28) == 4) { i += header&0x7f; continue; }
        auto packet = Device::header(header);
        if (packet.first == 0x38) {
            assert(packet.second == 7 && words[i] == (6u|(1u<<10)) && words[i+1] == 2 && words[i+2] == 4);
            assert(words[i+3] == 0 && words[i+4] == 0x4400000 && !words[i+5] && words[i+6] == 4);
            indexedPacket = true;
        }
        if (packet.first == 0x30 && ((words[i]>>18)&15) == 8 && !((words[i]>>16)&3) && words[i+1] == 1 && packet.second > 3) {
            assert(words[i+3+instanced.driverBase+1] == UINT32_MAX);
            assert(words[i+3+instanced.driverBase+2] == 7); instanceConstant = true;
        }
        i += packet.second;
    }
    assert(indexedPacket && instanceConstant);
    indices[0] = 0; assert(!GPU::Shader::indexFootprint(indexed,indices,elements));
    indexed.baseVertex = 0; indexed.primitive = 3;
    assert(GPU::Shader::indexFootprint(indexed,indices,elements) && elements == 65536); // no list restart
    indexed.indexSize = 4; indexed.indices.bytes = 16; indexed.baseVertex = 1;
    uint32_t large[] = {0,1,UINT32_MAX,2};
    assert(!GPU::Shader::indexFootprint(indexed,large,elements));
    indexed.primitive = 4; assert(GPU::Shader::indexFootprint(indexed,large,elements) && elements == 4);
    // Mercury's SDF draws use 29,184 indices. Exercise the complete scan on
    // an unaligned cached snapshot, including a restart and a bad final index.
    for(uint32_t size:{2u,4u}) {
        auto sdf=indexed;sdf.count=29184;sdf.indexSize=size;sdf.baseVertex=-1;
        sdf.indices.bytes=uint64_t(sdf.count)*size;
        std::vector<uint8_t> snapshot(sdf.indices.bytes+1);auto *copy=snapshot.data()+1;
        for(uint32_t i=0;i<sdf.count;++i)for(uint32_t b=0;b<size;++b)
            copy[uint64_t(i)*size+b]=uint8_t((i+1)>>(8*b));
        for(uint32_t b=0;b<size;++b)copy[17*size+b]=0xff;
        assert(GPU::Shader::indexFootprint(sdf,copy,elements) && elements==sdf.count);
        for(uint32_t b=0;b<size;++b)copy[uint64_t(sdf.count-1)*size+b]=0;
        assert(!GPU::Shader::indexFootprint(sdf,copy,elements));
    }
    indexed.baseInstance = UINT32_MAX; assert(!GPU::Shader::validDraw(indexed));
    const auto &varyingVS=*GPUPrograms::lookup(6), &varyingFS=*GPUPrograms::lookup(8);
    GPU::Shader::Linkage linkage;
    assert(GPU::Shader::link(varyingVS,varyingFS,linkage) && linkage.count==3 && linkage.stride==10);
    Adreno::Encoder varyings(words.data(),words.size()); Adreno::PipelineEncoder varyingPipeline(varyings,ib,scratch);
    assert(varyingPipeline.draw(draw,varyingVS,varyingFS,buffers,0x4200000,Adreno::GraphicsTextures{{0x4300000},{0}})==Result::Success);
    assert(regValue(varyings.size(),REG_A5XX_SP_VS_OUT_REG(0)) ==
        (A5XX_SP_VS_OUT_REG_A_REGID(varyingVS.varyings[1].regid)|A5XX_SP_VS_OUT_REG_A_COMPMASK(3)|
         A5XX_SP_VS_OUT_REG_B_REGID(varyingVS.varyings[0].regid)|A5XX_SP_VS_OUT_REG_B_COMPMASK(15)));
    assert(regValue(varyings.size(),REG_A5XX_SP_VS_VPC_DST_REG(0))==(2u<<8|6u<<16));
    assert(regValue(varyings.size(),REG_A5XX_VPC_VAR_DISABLE(0))==~63u);
    assert(regValue(varyings.size(),REG_A5XX_VPC_PACK)==(A5XX_VPC_PACK_PSIZELOC(255)|A5XX_VPC_PACK_NUMNONPOSVAR(6)));
    assert(regValue(varyings.size(),REG_A5XX_VPC_CNTL_0)==(0x10000|10|A5XX_VPC_CNTL_0_VARYING));
    assert(regValue(varyings.size(),REG_A5XX_GRAS_CNTL)&A5XX_GRAS_CNTL_IJ_PERSP_PIXEL);
    auto malformed=varyingFS;malformed.varyings[1].inloc=0;
    assert(!GPU::Shader::link(varyingVS,malformed,linkage));
    Adreno::Encoder badLink(words.data(),words.size()); Adreno::PipelineEncoder badPipeline(badLink,ib,scratch);
    assert(badPipeline.draw(draw,varyingVS,malformed,buffers,0x4200000,Adreno::GraphicsTextures{{0x4300000},{0}})==Result::Invalid && !badLink.size());
    // A later plain draw must turn varying production/interpolation off.
    Adreno::Encoder plain(words.data(),words.size()); Adreno::PipelineEncoder plainPipeline(plain,ib,scratch);
    assert(plainPipeline.draw(draw,vs,fs,buffers,0x4200000,Adreno::GraphicsTextures{{0x4300000},{0}})==Result::Success);
    assert(!(regValue(plain.size(),REG_A5XX_VPC_CNTL_0)&A5XX_VPC_CNTL_0_VARYING));
    assert(regValue(plain.size(),REG_A5XX_SP_VS_OUT_REG(1))==0 && regValue(plain.size(),REG_A5XX_VPC_VAR_DISABLE(0))==UINT32_MAX);
    // Real IR3 attributes use sparse Metal locations but packed VFD slots.
    const auto &attributeVS=*GPUPrograms::lookup(9),&attributeFS=*GPUPrograms::lookup(10);
    assert(attributeVS.attributeMask==0x89 && !attributeVS.bufferMask);
    auto fetched=draw;fetched.textures[0]={};fetched.instances=3;fetched.baseInstance=2;
    fetched.attributes[0]={30,0,0,0};fetched.attributes[3]={9,12,0,0};fetched.attributes[7]={29,0,1,0};
    fetched.layouts[0]={16,1,1,0};fetched.layouts[1]={8,2,2,0};
    fetched.buffers[0]={8,0,48,0};fetched.buffers[1]={9,0,32,0};uint32_t fetchBindings=0;
    assert(GPU::Shader::vertexFootprint(attributeVS,fetched,3,fetchBindings) && fetchBindings==3);
    auto badFetch=fetched;badFetch.buffers[1].bytes=31;
    assert(!GPU::Shader::vertexFootprint(attributeVS,badFetch,3,fetchBindings));
    badFetch=fetched;badFetch.attributes[3].offset=13;
    assert(!GPU::Shader::vertexFootprint(attributeVS,badFetch,3,fetchBindings));
    badFetch=fetched;badFetch.layouts[0].stride=14;
    assert(!GPU::Shader::vertexLayout(attributeVS,badFetch,fetchBindings));
    badFetch=fetched;badFetch.attributes[0].format=39;
    assert(!GPU::Shader::vertexLayout(attributeVS,badFetch,fetchBindings));
    badFetch=fetched;badFetch.layouts[1].rate=0;
    assert(!GPU::Shader::vertexLayout(attributeVS,badFetch,fetchBindings));
    Adreno::ShaderBuffers fetchBuffers{};fetchBuffers.addresses[0]=0x4400000;fetchBuffers.addresses[1]=0x4500000;
    fetchBuffers.bytes[0]=48;fetchBuffers.bytes[1]=32;
    Adreno::Encoder fetch(words.data(),words.size());Adreno::PipelineEncoder fetchPipeline(fetch,ib,scratch);
    assert(fetchPipeline.draw(fetched,attributeVS,attributeFS,fetchBuffers,0x4200000,Adreno::GraphicsTextures{{0},{0}})==Result::Success);
    assert(regValue(fetch.size(),REG_A5XX_VFD_CONTROL_0)==A5XX_VFD_CONTROL_0_VTXCNT(3));
    assert(regValue(fetch.size(),REG_A5XX_VFD_FETCH_BASE_LO(1))==0x440000c);
    assert(regValue(fetch.size(),REG_A5XX_VFD_FETCH_STRIDE(1))==16);
    assert(regValue(fetch.size(),REG_A5XX_VFD_DECODE_INSTR(1))==(A5XX_VFD_DECODE_INSTR_IDX(1)|
        A5XX_VFD_DECODE_INSTR_FORMAT(0x30)|A5XX_VFD_DECODE_INSTR_UNK30|A5XX_VFD_DECODE_INSTR_FLOAT));
    assert(regValue(fetch.size(),REG_A5XX_VFD_DECODE_INSTR(2))&A5XX_VFD_DECODE_INSTR_INSTANCED);
    assert(regValue(fetch.size(),REG_A5XX_VFD_DECODE_STEP_RATE(2))==2);
    assert(regValue(fetch.size(),REG_A5XX_VFD_DEST_CNTL_INSTR(2))==
        (A5XX_VFD_DEST_CNTL_INSTR_REGID(attributeVS.attributes[7].regid)|A5XX_VFD_DEST_CNTL_INSTR_WRITEMASK(attributeVS.attributes[7].mask)));
    fetched.layouts[1].step=0;fetched.buffers[1].bytes=8;
    assert(GPU::Shader::vertexFootprint(attributeVS,fetched,3,fetchBindings));
    Adreno::Encoder constant(words.data(),words.size());Adreno::PipelineEncoder constantPipeline(constant,ib,scratch);
    assert(constantPipeline.draw(fetched,attributeVS,attributeFS,fetchBuffers,0x4200000,Adreno::GraphicsTextures{{0},{0}})==Result::Success);
    assert(regValue(constant.size(),REG_A5XX_VFD_FETCH_STRIDE(2))==0);
    // D32 and separate S8 may share one allocation without overlapping planes.
    auto zs=draw;zs.depth={5,0,64,64,256,GPU::Shader::Depth32};
    zs.stencil={5,16384,64,64,64,GPU::Shader::Stencil8};
    zs.depthStencil=GPU::Shader::defaultDepthStencilState();
    zs.depthStencil.compare=GPU::Shader::Less;zs.depthStencil.write=1;
    zs.depthStencil.front={1,GPU::Shader::Equal,GPU::Shader::Replace,GPU::Shader::IncrementClamp,GPU::Shader::Invert,15,240};
    zs.depthStencil.back={1,GPU::Shader::Greater,GPU::Shader::Clear,GPU::Shader::DecrementWrap,GPU::Shader::IncrementWrap,255,127};
    zs.depthStencil.frontReference=3;zs.depthStencil.backReference=7;
    zs.state.depthRange[0]=0x3f800000;zs.state.depthRange[1]=0xbf800000; // reversed Z
    Adreno::Encoder zcommands(words.data(),words.size());Adreno::PipelineEncoder zp(zcommands,ib,scratch);
    assert(zp.draw(zs,vs,fs,buffers,0x4200000,Adreno::GraphicsTextures{{0x4300000},{0}},0,0x4500000,0x4504000)==Result::Success);
    assert(regValue(zcommands.size(),REG_A5XX_RB_DEPTH_CNTL)==(A5XX_RB_DEPTH_CNTL_ZFUNC(1)|
        A5XX_RB_DEPTH_CNTL_Z_TEST_ENABLE|A5XX_RB_DEPTH_CNTL_Z_READ_ENABLE|A5XX_RB_DEPTH_CNTL_Z_WRITE_ENABLE));
    assert(regValue(zcommands.size(),REG_A5XX_RB_DEPTH_BUFFER_INFO)==A5XX_RB_DEPTH_BUFFER_INFO_DEPTH_FORMAT(4));
    assert(regValue(zcommands.size(),REG_A5XX_RB_STENCIL_BASE_LO)==0x4504000);
    assert(regValue(zcommands.size(),REG_A5XX_RB_STENCILREFMASK)==(3u|(15u<<8)|(240u<<16)));
    assert(regValue(zcommands.size(),REG_A5XX_RB_STENCILREFMASK_BF)==(7u|(255u<<8)|(127u<<16)));
    assert(regValue(zcommands.size(),REG_A5XX_RB_STENCIL_CONTROL)==(A5XX_RB_STENCIL_CONTROL_STENCIL_READ|
        A5XX_RB_STENCIL_CONTROL_STENCIL_ENABLE|A5XX_RB_STENCIL_CONTROL_STENCIL_ENABLE_BF|
        A5XX_RB_STENCIL_CONTROL_FUNC(2)|A5XX_RB_STENCIL_CONTROL_FAIL(2)|A5XX_RB_STENCIL_CONTROL_ZFAIL(3)|A5XX_RB_STENCIL_CONTROL_ZPASS(5)|
        A5XX_RB_STENCIL_CONTROL_FUNC_BF(4)|A5XX_RB_STENCIL_CONTROL_FAIL_BF(1)|A5XX_RB_STENCIL_CONTROL_ZFAIL_BF(7)|A5XX_RB_STENCIL_CONTROL_ZPASS_BF(6)));
    assert(regValue(zcommands.size(),REG_A5XX_GRAS_CL_VPORT_ZSCALE_0)==0xbf800000);
    bool invalidateDepth=false,flushDepth=false;
    for (unsigned i=0;i<zcommands.size();) {
        auto header=words[i++];if ((header>>28)==4) {i+=header&127;continue;}
        auto packet=Device::header(header);
        if (packet.first==0x46) {invalidateDepth|=words[i]==24;flushDepth|=(words[i]&255)==28 && packet.second==4;}
        if (packet.first==0x46 && packet.second==4) {
            const uint64_t target=uint64_t(words[i+1])|(uint64_t(words[i+2])<<32);
            assert(target==scratch || target==scratch+8); // separate from read-only code/IB
        }
        i+=packet.second;
    }
    assert(invalidateDepth && flushDepth);
    uint32_t width,height;
    auto malformedZS=zs;malformedZS.stencil.offset=64;assert(!GPU::Shader::validAttachments(malformedZS,width,height));
    malformedZS=zs;malformedZS.depth.height=32;assert(!GPU::Shader::validAttachments(malformedZS,width,height));
    malformedZS=zs;malformedZS.depthStencil.front.pass=8;assert(!GPU::Shader::validAttachments(malformedZS,width,height));
    malformedZS=zs;malformedZS.depth.format=GPU::Shader::RGBA8;assert(!GPU::Shader::validAttachments(malformedZS,width,height));
    zs.target={};Adreno::Encoder depthOnly(words.data(),words.size());Adreno::PipelineEncoder only(depthOnly,ib,scratch);
    assert(only.draw(zs,vs,fs,buffers,0,Adreno::GraphicsTextures{{0x4300000},{0}},0,0x4500000,0x4504000)==Result::Success);
    assert(!(regValue(depthOnly.size(),REG_A5XX_RB_MRT_CONTROL(0))&A5XX_RB_MRT_CONTROL_COMPONENT_ENABLE(15)));
    for(uint32_t count:{0u,2u,4u}) {
        const GPU::Shader::Program *sampleVS=nullptr;
        for(const auto &p:GPUPrograms::Programs)if(!strcmp(p.name,"sample_vertex"))sampleVS=&p;
        assert(sampleVS);
        for(const char *name:{"sample_interpolation","centroid_fragment","sample_coverage","sample_store"}) {
            const GPU::Shader::Program *owner=nullptr;
            for(const auto &p:GPUPrograms::Programs)if(!strcmp(p.name,name))owner=&p;
            assert(owner && owner->msaa);const auto &p=GPU::Shader::rasterProgram(*owner,count);
            auto job=draw;job.rasterSamples=count;job.target.samples=count;job.target.pitch=256*(count?count:1);
            for(auto &t:job.textures)t={};
            for(auto &s:job.samplers)s={};
            auto bindings=buffers;bindings.addresses[8]=0x4900000;bindings.bytes[8]=1024;
            Adreno::Encoder commands(words.data(),words.size());Adreno::PipelineEncoder pipeline(commands,ib,scratch);
            assert(pipeline.draw(job,*sampleVS,*owner,bindings,0x4200000,Adreno::GraphicsTextures{{0},{0}})==Result::Success);
            auto reg=[&](uint32_t flag,uint32_t value){return (p.sampleFlags&flag)?value:252u;};
            assert(regValue(commands.size(),REG_A5XX_HLSQ_CONTROL_2_REG)==(A5XX_HLSQ_CONTROL_2_REG_FACEREGID(252)|
                A5XX_HLSQ_CONTROL_2_REG_SAMPLEID(reg(QGPU_SAMPLE_ID,p.sampleID))|A5XX_HLSQ_CONTROL_2_REG_SAMPLEMASK(reg(QGPU_SAMPLE_MASK_IN,p.sampleMaskIn))|
                A5XX_HLSQ_CONTROL_2_REG_CENTERRHW(p.centerRHW)));
            assert(regValue(commands.size(),REG_A5XX_HLSQ_CONTROL_3_REG)==(A5XX_HLSQ_CONTROL_3_REG_IJ_PERSP_PIXEL(p.perspective)|
                A5XX_HLSQ_CONTROL_3_REG_IJ_LINEAR_PIXEL(p.linear)|A5XX_HLSQ_CONTROL_3_REG_IJ_PERSP_CENTROID(reg(QGPU_CENTROID_PERSPECTIVE,p.perspectiveCentroid))|
                A5XX_HLSQ_CONTROL_3_REG_IJ_LINEAR_CENTROID(reg(QGPU_CENTROID_LINEAR,p.linearCentroid))));
            uint32_t mode=regValue(commands.size(),REG_A5XX_RB_RENDER_CONTROL1);
            assert((mode&A5XX_RB_RENDER_CONTROL1_SAMPLEMODE__MASK)==((count && (p.sampleFlags&QGPU_SAMPLE_EXECUTION))?A5XX_RB_RENDER_CONTROL1_SAMPLEMODE(3):0));
            bool mask=count && (p.sampleFlags&QGPU_SAMPLE_MASK_OUT);
            assert(bool(regValue(commands.size(),REG_A5XX_RB_FS_OUTPUT_CNTL)&A5XX_RB_FS_OUTPUT_CNTL_FRAG_WRITES_SAMPMASK)==mask);
            assert((regValue(commands.size(),REG_A5XX_SP_FS_OUTPUT_CNTL)&A5XX_SP_FS_OUTPUT_CNTL_SAMPLEMASK_REGID__MASK)==A5XX_SP_FS_OUTPUT_CNTL_SAMPLEMASK_REGID(mask?p.sampleMaskOut:252));
        }
    }
    for(uint32_t count:{2u,4u}) {
        GPU::Shader::Texture array{53,0,16,8,256*count,GPU::Shader::RGBA8,0,0,3,0,0,count};
        GPU::Shader::TextureLayout layout;assert(GPU::Shader::textureLayout(array,layout));
        GPU::Shader::Texture view{},flat{};
        assert(GPU::Shader::textureView(array,0,1,1,2,true,GPU::Shader::RGBA8,view) && view.samples==count && view.layers==2);
        assert(GPU::Shader::textureSlice(view,1,flat) && flat.offset==2*layout.layerStride && !flat.layers && flat.samples==count);
        auto output=array;output.resource=54;output.samples=0;output.pitch=256;
        const GPU::Shader::Program *copy=nullptr;
        for(const auto &p:GPUPrograms::Programs)if(!strcmp(p.name,"multisample_array_copy"))copy=&p;
        assert(copy && copy->multisampleMask==8 && copy->imageReadMask==8 && copy->imageWriteMask==128);
        GPU::Shader::Dispatch grid{};grid.images[3]=array;grid.images[7]=output;
        grid.local[0]=grid.local[1]=8;grid.local[2]=1;grid.grid[0]=16;grid.grid[1]=8;grid.grid[2]=3;
        grid.groups[0]=2;grid.groups[1]=1;grid.groups[2]=3;
        auto memory=buffers;memory.addresses[11]=0x4b00000;memory.addresses[15]=0x4c00000;
        Adreno::Encoder copied(words.data(),words.size());Adreno::PipelineEncoder copyBackend(copied,ib,scratch);
        assert(copyBackend.dispatch(grid,*copy,memory)==Result::Success);
        uint32_t arrayConstants[QGPU_IMAGE_CONSTANT_WORDS];assert(GPU::Shader::imageFootprint(grid,*copy,arrayConstants));
        auto badArray=array;badArray.cube=1;uint64_t span=0;assert(!GPU::Shader::textureSize(badArray,span));
        for(const char *name:{"multisample_array_vertex","multisample_array_fragment"}) {
            const GPU::Shader::Program *shader=nullptr;for(const auto &p:GPUPrograms::Programs)if(!strcmp(p.name,name))shader=&p;
            assert(shader);bool vertex=shader->stage==GPU::Shader::Vertex;unsigned slot=vertex?2:5;
            assert(shader->multisampleMask==(1u<<slot) && (shader->flags&(1u<<(QGPU_ARRAY_TEXTURE_SHIFT+slot))));
            auto job=draw;for(auto &t:job.textures)t={};for(auto &t:job.vertexTextures)t={};
            (vertex?job.vertexTextures:job.textures)[slot]=array;
            Adreno::GraphicsTextures textures;(vertex?textures.vertex:textures.fragment)[slot]=0x4b00000;
            auto bindings=buffers;
            if(!vertex){bindings.addresses[8]=0x4d00000;bindings.bytes[8]=4;job.fragmentBuffers[0]={56,0,4,0};}
            Adreno::Encoder encoded(words.data(),words.size());Adreno::PipelineEncoder backend(encoded,ib,scratch);
            assert(backend.draw(job,vertex?*shader:vs,vertex?*GPUPrograms::lookup(3):*shader,bindings,0x4200000,textures)==Result::Success);
            bool found=false;
            for(unsigned i=0;i<encoded.size();) {
                auto h=words[i++];if((h>>28)==4){i+=h&127;continue;}auto packet=Device::header(h);
                if(packet.first==0x30 && packet.second==15 && ((words[i]>>18)&15)==(vertex?0u:4u) && (words[i]&0x3fff)==QGPU_FETCH_TEXTURE_BASE+slot) {
                    assert(words[i+6]==A5XX_TEX_CONST_3_ARRAY_PITCH(layout.layerStride));
                    assert((words[i+8]&A5XX_TEX_CONST_5_DEPTH__MASK)==A5XX_TEX_CONST_5_DEPTH(3));found=true;
                }
                i+=packet.second;
            }
            assert(found);
        }
        auto depthArray=array;depthArray.resource=57;depthArray.format=GPU::Shader::Depth32;
        GPU::Shader::TextureLayout depthLayout,stencilLayout;assert(GPU::Shader::planeLayout(depthArray,GPU::Shader::Depth32,depthLayout));
        auto stencilArray=depthArray;stencilArray.format=GPU::Shader::Stencil8;stencilArray.pitch=64*count;stencilArray.offset=depthLayout.bytes;
        assert(GPU::Shader::planeLayout(stencilArray,GPU::Shader::Stencil8,stencilLayout));
        GPU::Shader::Texture depthSlice{},stencilSlice{};
        assert(GPU::Shader::textureSlice(depthArray,2,depthSlice) && GPU::Shader::textureSlice(stencilArray,2,stencilSlice));
        assert(depthSlice.offset==2*depthLayout.layerStride && stencilSlice.offset==depthLayout.bytes+2*stencilLayout.layerStride);
        assert(!GPU::Shader::overlap(depthSlice,stencilSlice) && !depthSlice.layers && !stencilSlice.layers);
        GPU::Shader::DepthStencilResolvePlan selected;auto target=depthSlice;target.resource=58;target.offset=0;target.samples=0;target.pitch=256;
        assert(GPU::Shader::depthStencilResolvePlan(depthSlice,target,nullptr,1,0,selected));
        assert(!GPU::Shader::depthStencilResolvePlan(depthArray,target,nullptr,1,0,selected));
        GPU::Shader::Texture depthSource{50,0,5,3,256*count,GPU::Shader::Depth32,0,0,0,0,0,count};
        GPU::Shader::Texture stencilSource{50,depthSource.pitch*depthSource.height,5,3,64*count,GPU::Shader::Stencil8,0,0,0,0,0,count};
        GPU::Shader::Texture depthTarget{51,0,5,3,256,GPU::Shader::Depth32};
        GPU::Shader::Texture stencilTarget{51,768,5,3,64,GPU::Shader::Stencil8};
        GPU::Shader::DepthStencilResolvePlan sampleZero;
        assert(GPU::Shader::depthStencilResolvePlan(stencilSource,stencilTarget,nullptr,0,0,sampleZero));
        assert(sampleZero.buffers[2].resource==stencilSource.resource && !sampleZero.parameters.depthPitch);
        for(unsigned filter=0;filter<3;++filter)for(unsigned stencil=0;stencil<2;++stencil) {
            GPU::Shader::DepthStencilResolvePlan plan;
            const auto &source=stencil?stencilSource:depthSource,&target=stencil?stencilTarget:depthTarget;
            assert(GPU::Shader::depthStencilResolvePlan(source,target,&depthSource,filter,stencil,plan));
            assert(plan.width==(stencil?2u:5u) && plan.height==3 && plan.parameters.samples==count);
            const GPU::Shader::Program *program=nullptr;
            for(const auto &p:GPUPrograms::Programs)if(!strcmp(p.name,stencil?"resolve_stencil":"resolve_depth"))program=&p;
            assert(program && program->bufferMask==(stencil?15u:11u) && program->writeMask==2);
            GPU::Shader::Dispatch work{};memcpy(work.buffers,plan.buffers,sizeof(plan.buffers));work.buffers[3]={52,0,32,0};
            work.grid[0]=plan.width;work.grid[1]=plan.height;work.grid[2]=1;
            work.groups[0]=work.groups[1]=work.groups[2]=1;work.local[0]=work.local[1]=8;work.local[2]=1;
            auto inputs=buffers;inputs.addresses[0]=0x4700000+source.offset;inputs.addresses[1]=0x4800000+target.offset;
            inputs.addresses[2]=stencil?0x4700000:0;inputs.addresses[3]=0x4900000;
            for(unsigned i=0;i<4;++i)inputs.bytes[i]=work.buffers[i].bytes;
            Adreno::Encoder commands(words.data(),words.size());Adreno::PipelineEncoder backend(commands,ib,scratch);
            assert(backend.dispatch(work,*program,inputs)==Result::Success);
            if(stencil)assert(!GPU::Shader::depthStencilResolvePlan(source,target,nullptr,filter,1,plan));
            auto invalid=target;invalid.samples=count;assert(!GPU::Shader::depthStencilResolvePlan(source,invalid,&depthSource,filter,stencil,plan));
            invalid=source;invalid.samples=0;assert(!GPU::Shader::depthStencilResolvePlan(source,invalid,&depthSource,filter,stencil,plan));
        }
        auto job=draw;job.rasterSamples=count;job.alphaToCoverage=1;
        job.target.samples=count;job.target.pitch=256*count;job.textures[0].resource=2;
        job.depth={5,0,64,64,256*count,GPU::Shader::Depth32,0,0,0,0,0,count};
        job.stencil={5,16384*count,64,64,64*count,GPU::Shader::Stencil8,0,0,0,0,0,count};
        Adreno::Encoder encoded(words.data(),words.size());Adreno::PipelineEncoder backend(encoded,ib,scratch);
        assert(backend.draw(job,vs,fs,buffers,0x4200000,Adreno::GraphicsTextures{{0x4300000},{0}},0,0x4500000,0x4600000)==Result::Success);
        uint32_t ms=count==4?2:1;
        assert(regValue(encoded.size(),REG_A5XX_RB_RAS_MSAA_CNTL)==ms && regValue(encoded.size(),REG_A5XX_RB_DEST_MSAA_CNTL)==ms);
        assert(regValue(encoded.size(),REG_A5XX_GRAS_SC_RAS_MSAA_CNTL)==ms && regValue(encoded.size(),REG_A5XX_GRAS_SC_DEST_MSAA_CNTL)==ms);
        assert(regValue(encoded.size(),REG_A5XX_TPL1_TP_RAS_MSAA_CNTL)==ms && regValue(encoded.size(),REG_A5XX_TPL1_TP_DEST_MSAA_CNTL)==ms);
        assert(regValue(encoded.size(),REG_A5XX_RB_BLEND_CNTL)&A5XX_RB_BLEND_CNTL_ALPHA_TO_COVERAGE);
        assert(regValue(encoded.size(),REG_A5XX_SP_BLEND_CNTL)&A5XX_SP_BLEND_CNTL_ALPHA_TO_COVERAGE);
        uint32_t width,height;auto bad=job;bad.depth.samples=0;assert(!GPU::Shader::validAttachments(bad,width,height));
        const GPU::Shader::Program *resolve=nullptr;
        for(const auto &p:GPUPrograms::Programs)if(!strcmp(p.name,"resolve_color"))resolve=&p;
        assert(resolve && resolve->multisampleMask==1 && resolve->imageReadMask==1 && resolve->imageWriteMask==128);
        GPU::Shader::Dispatch dispatch{};dispatch.images[0]=job.target;dispatch.images[7]=draw.target;dispatch.images[7].resource=6;
        dispatch.local[0]=dispatch.local[1]=8;dispatch.local[2]=1;dispatch.groups[0]=dispatch.groups[1]=8;dispatch.groups[2]=1;
        uint32_t constants[QGPU_IMAGE_CONSTANT_WORDS];assert(GPU::Shader::imageFootprint(dispatch,*resolve,constants));
        assert(((constants[3]>>28)&3)==ms);
        auto resolved=buffers;resolved.addresses[8]=0x4700000;resolved.addresses[15]=0x4800000;
        resolved.bytes[8]=16384*count;resolved.bytes[15]=16384;
        Adreno::Encoder compute(words.data(),words.size());Adreno::PipelineEncoder cp(compute,ib,scratch);
        assert(cp.dispatch(dispatch,*resolve,resolved)==Result::Success);
        auto halfResolve=dispatch;auto halfBuffers=resolved;
        for(auto slot:{0u,7u}) {
            halfResolve.images[slot].format=GPU::Shader::RGBA16Float;halfResolve.images[slot].pitch*=2;
            halfBuffers.bytes[QGPU_COMPUTE_IMAGE_BASE+slot]*=2;
        }
        assert(GPU::Shader::imageFootprint(halfResolve,*resolve,constants) && (constants[3]&255)==GPU::Shader::RGBA16Float);
        Adreno::Encoder halfCompute(words.data(),words.size());Adreno::PipelineEncoder halfCP(halfCompute,ib,scratch);
        assert(halfCP.dispatch(halfResolve,*resolve,halfBuffers)==Result::Success);
        dispatch.images[0].samples=0;assert(!GPU::Shader::imageFootprint(dispatch,*resolve,constants));
        for(const char *name:{"multisample_fragment","multisample_vertex"}) {
            const GPU::Shader::Program *shader=nullptr;for(const auto &p:GPUPrograms::Programs)if(!strcmp(p.name,name))shader=&p;
            assert(shader);bool vertex=shader->stage==GPU::Shader::Vertex;unsigned slot=vertex?2:5;
            auto fetch=draw;for(auto &t:fetch.textures)t={};for(auto &t:fetch.vertexTextures)t={};
            GPU::Shader::Texture source{52,0,16,16,256*count,GPU::Shader::BGRA8SRGB,0,0,0,0,0,count};
            (vertex?fetch.vertexTextures:fetch.textures)[slot]=source;
            Adreno::GraphicsTextures textures;(vertex?textures.vertex:textures.fragment)[slot]=0x4900000;
            auto bindings=buffers;bindings.addresses[8]=0x4a00000;bindings.bytes[8]=16;
            Adreno::Encoder commands(words.data(),words.size());Adreno::PipelineEncoder pipeline(commands,ib,scratch);
            assert(pipeline.draw(fetch,vertex?*shader:vs,vertex?*GPUPrograms::lookup(3):*shader,bindings,0x4200000,textures)==Result::Success);
            bool found=false;
            for(unsigned i=0;i<commands.size();) {
                auto h=words[i++];if((h>>28)==4){i+=h&127;continue;}auto packet=Device::header(h);
                if(packet.first==0x30 && ((words[i]>>18)&15)==(vertex?0u:4u) && (words[i]&0x3fff)==QGPU_FETCH_TEXTURE_BASE+slot) {
                    assert((words[i+3]&A5XX_TEX_CONST_0_SAMPLES__MASK)==A5XX_TEX_CONST_0_SAMPLES(ms));
                    assert((words[i+5]&A5XX_TEX_CONST_2_PITCHALIGN__MASK)==A5XX_TEX_CONST_2_PITCHALIGN(ms+2));found=true;
                }
                i+=packet.second;
            }
            assert(found);
        }
    }
    for(const char *name:{"raster_depth_only","depth_fragment","depth_only_fragment","discard_depth_fragment","discard_only_fragment","air_depth_fragment","air_depth_only_fragment","air_discard_fragment","air_void_fragment"}) {
        const GPU::Shader::Program *fragment=nullptr;
        for(const auto &p:GPUPrograms::Programs)if(!strcmp(p.name,name))fragment=&p;
        assert(fragment && GPU::Shader::validInterface(*fragment));
        auto job=zs;for(auto &t:job.textures)t={};Adreno::GraphicsTextures sampled;
        auto resolved=buffers;resolved.addresses[8]=0x4600000;resolved.addresses[9]=0x4700000;
        resolved.bytes[8]=resolved.bytes[9]=16384;
        if(fragment->sampleMask){job.textures[5]={51,0,16,16,256,GPU::Shader::RGBA8};sampled.fragment[5]=0x4800000;}
        Adreno::Encoder encoded(words.data(),words.size());Adreno::PipelineEncoder backend(encoded,ib,scratch);
        assert(backend.draw(job,vs,*fragment,resolved,0,sampled,0,0x4500000,0x4504000)==Result::Success);
        bool writesDepth=fragment->flags&QGPU_FRAGMENT_DEPTH,late=fragment->flags&(2u|QGPU_FRAGMENT_FLAGS);
        uint32_t plane=late?A5XX_RB_DEPTH_PLANE_CNTL_FRAG_WRITES_Z|(fragment->fragMask?A5XX_RB_DEPTH_PLANE_CNTL_UNK1:0):0;
        assert(regValue(encoded.size(),REG_A5XX_RB_DEPTH_PLANE_CNTL)==plane && regValue(encoded.size(),REG_A5XX_GRAS_SU_DEPTH_PLANE_CNTL)==plane);
        assert(regValue(encoded.size(),REG_A5XX_RB_FS_OUTPUT_CNTL)==(writesDepth?A5XX_RB_FS_OUTPUT_CNTL_FRAG_WRITES_Z:0));
        assert(regValue(encoded.size(),REG_A5XX_SP_FS_OUTPUT_CNTL)==(A5XX_SP_FS_OUTPUT_CNTL_DEPTH_REGID(writesDepth?fragment->depthReg:252)|A5XX_SP_FS_OUTPUT_CNTL_SAMPLEMASK_REGID(252)));
        assert(!regValue(encoded.size(),REG_A5XX_RB_RENDER_COMPONENTS));
    }
    Adreno::Encoder noDepth(words.data(),words.size());Adreno::PipelineEncoder resetZS(noDepth,ib,scratch);
    assert(resetZS.draw(draw,vs,fs,buffers,0x4200000,Adreno::GraphicsTextures{{0x4300000},{0}})==Result::Success);
    assert(regValue(noDepth.size(),REG_A5XX_VFD_CONTROL_0)==0);
    assert(regValue(noDepth.size(),REG_A5XX_RB_DEPTH_CNTL)==0 && regValue(noDepth.size(),REG_A5XX_RB_STENCIL_CONTROL)==0);
    assert(regValue(noDepth.size(),REG_A5XX_RB_DEPTH_BUFFER_BASE_LO)==0 && regValue(noDepth.size(),REG_A5XX_RB_STENCIL_BASE_LO)==0);
    auto invalid = draw.state; invalid.viewport[1] = 0x7fc00000;
    assert(!GPU::Shader::validRenderState(invalid)); invalid = draw.state; invalid.cull = 3;
    assert(!GPU::Shader::validRenderState(invalid)); invalid = draw.state; invalid.sourceRGB = 15;
    assert(!GPU::Shader::validRenderState(invalid)); invalid = draw.state; invalid.blendColor[0] = 0xbf000000;
    assert(!GPU::Shader::validRenderState(invalid));
    draw.state = invalid;
    Adreno::Encoder rejected(words.data(),words.size()); Adreno::PipelineEncoder rejectPipeline(rejected,ib,scratch);
    assert(rejectPipeline.draw(draw,vs,fs,buffers,0x4200000,Adreno::GraphicsTextures{{0x4300000},{0}}) == Result::Invalid && !rejected.size());
    draw.state = GPU::Shader::defaultRenderState(64,64);
    Adreno::Encoder tiny(words.data(),12); Adreno::PipelineEncoder limited(tiny,ib,scratch);
    assert(limited.draw(draw,vs,fs,buffers,0x4200000,Adreno::GraphicsTextures{{0x4300000},{0}}) == Result::NoResources && tiny.size() <= 12);
    {
        using namespace GPU::Shader;
        const Program *depth=nullptr;for(const auto &p:GPUPrograms::Programs)if(!strcmp(p.name,"depth_sample"))depth=&p;
        assert(depth && depth->depthTextureMask==3 && validInterface(*depth));
        Dispatch d{};d.groups[0]=d.groups[1]=d.groups[2]=d.local[1]=d.local[2]=1;d.local[0]=4;
        d.images[0]={1,0,8,4,256,Depth32,4};d.images[1]={2,0,8,4,256,Depth32,4,0,2,12288};
        Adreno::ShaderBuffers bound{};bound.addresses[0]=0x4000000;bound.bytes[0]=64;
        for(unsigned i=0;i<2;++i){bound.addresses[QGPU_COMPUTE_IMAGE_BASE+i]=0x5000000+i*0x10000;bound.bytes[QGPU_COMPUTE_IMAGE_BASE+i]=24576;}
        for(unsigned compare=0;compare<8;++compare) {
            d.samplers[3].filters=3|(compare<<8);Adreno::Encoder encoded(words.data(),words.size());Adreno::PipelineEncoder pipeline(encoded,ib,scratch);
            assert(pipeline.dispatch(d,*depth,bound)==Result::Success);
            unsigned textures=0,samplers=0;
            for(unsigned i=0;i<encoded.size();) {
                auto h=words[i++];if((h>>28)==4){i+=h&127;continue;}auto packet=Device::header(h);
                if(packet.first==0x30 && ((words[i]>>18)&15)==5) {
                    unsigned slot=words[i]&255;
                    if(packet.second==15){assert((words[i+3]&A5XX_TEX_CONST_0_FMT__MASK)==A5XX_TEX_CONST_0_FMT(0x4a));assert((words[i+3]&A5XX_TEX_CONST_0_MIPLVLS__MASK)==A5XX_TEX_CONST_0_MIPLVLS(3));textures|=1u<<slot;}
                    if(packet.second==7){assert(((words[i+4]>>1)&7)==(slot==3?compare:3));samplers|=1u<<slot;}
                }
                i+=packet.second;
            }
            assert(textures==3 && samplers==((1u<<3)|(1u<<depth->constantSamplerLevelSlots[0])));
        }
        uint64_t elements=0;unsigned dimensions=0;
        d.images[0].format=RGBA8;assert(!dispatchFootprint(d,*depth,elements,dimensions));
        d.images[0].format=Stencil8;assert(!dispatchFootprint(d,*depth,elements,dimensions));
        d.images[0].format=Depth32;auto writable=*depth;writable.imageWriteMask=1;assert(!validInterface(writable));
    }
    {
        using namespace GPU::Shader;
        Texture depth{7,64,37,19,256,Depth32,6,0,3},stencil{7,61504,37,19,64,Stencil8,6,0,3};
        TextureLayout dl,sl;assert(planeLayout(depth,Depth32,dl) && planeLayout(stencil,Stencil8,sl));
        assert(dl.bytes==61440 && dl.layerStride==20480 && dl.level[5].offset==8960 && dl.accessBytes==50176);
        assert(sl.bytes==24576 && sl.layerStride==8192 && sl.level[5].offset==2240 && sl.accessBytes==18688);
        Texture dv,sv,dn,sn;
        assert(textureView(depth,1,4,1,2,true,Depth32,dv) && textureView(stencil,1,4,1,2,true,Stencil8,sv));
        assert(dv.offset==25408 && sv.offset==70912 && dv.layerStride==20480 && sv.layerStride==8192);
        assert(textureView(dv,2,1,1,1,false,Depth32,dn) && textureView(sv,2,1,1,1,false,Stencil8,sn));
        assert(dn.offset==49216 && sn.offset==79936 && dn.width==4 && dn.height==2 && sn.width==4 && sn.height==2);
        assert(!dn.mipLevels && !sn.mipLevels && !dn.layerStride && !sn.layerStride && !overlap(dn,sn));
        TextureLayout nested;assert(planeLayout(dn,Depth32,nested,false) && nested.accessBytes==512);
        assert(planeLayout(sn,Stencil8,nested,false) && nested.accessBytes==128);
        assert(!textureView(depth,0,2,0,1,false,RGBA8,dn));
        assert(!textureView(stencil,0,2,0,1,false,Depth32,sn));
        Draw pass{};pass.depth=dv;pass.depthStencil=defaultDepthStencilState();uint32_t w=0,h=0;
        assert(!validAttachments(pass,w,h)); // attachment must identify one mip and one slice
        assert(textureView(depth,3,1,2,1,false,Depth32,pass.depth) && textureView(stencil,3,1,2,1,false,Stencil8,pass.stencil));
        assert(validAttachments(pass,w,h) && w==4 && h==2);
        auto multisampled=depth;multisampled.samples=2;multisampled.pitch=512;assert(!planeLayout(multisampled,Depth32,nested));
        DepthStencilResolvePlan plan;Texture source{8,0,37,19,512,Depth32,0,0,0,0,0,2};
        auto destination=depth;destination.layers=0;
        assert(!depthStencilResolvePlan(source,destination,nullptr,0,0,plan));
        assert(textureLevel(destination,0,dn) && depthStencilResolvePlan(source,dn,nullptr,0,0,plan));
    }

}
// Exercise the same retained tree implementation used by the native mapper.
struct RetainedTables {
    std::map<uint64_t,std::array<uint64_t,512>> pages;
    uint64_t next=0x1000000;unsigned allocations=0,releases=0,syncs=0;
    bool stopped=true,timeout=false,ownsLive=false;
    QcomPower::Status publish(const Adreno::MMUTablePage &p) {return validate(p)?QcomPower::OK:QcomPower::Stale;}
    QcomPower::Status isolated() {return stopped?QcomPower::OK:QcomPower::Busy;}
    QcomPower::Status liveOwnership() {return ownsLive?QcomPower::OK:QcomPower::Denied;}
    void barrier() {}
    QcomPower::Status sync() {++syncs;return timeout?QcomPower::TimedOut:QcomPower::OK;}
    QcomPower::Status borrow(uint64_t pa,Adreno::MMUTablePage &p) {
        auto i=pages.find(pa);if(i==pages.end())return QcomPower::Invalid;
        p={i->second.data(),i->second.data(),pa};return QcomPower::OK;
    }
    QcomPower::Status allocate(Adreno::MMUTablePage &p) {
        auto pa=next;next+=4096;auto &data=pages[pa];p={data.data(),data.data(),pa};++allocations;return QcomPower::OK;
    }
    void release(Adreno::MMUTablePage &p,bool owned) {if(owned){pages.erase(p.physical);++releases;}p={};}
    bool validate(const Adreno::MMUTablePage &p) {
        auto i=pages.find(p.physical);
        return i!=pages.end() && p.owner==i->second.data() && p.entries==i->second.data();
    }

};
struct CachedPrivatePages {
    RetainedTables &storage;
    Adreno::RetiredTablePageCache<RetainedTables> cache;
    bool reusable=false;
    explicit CachedPrivatePages(RetainedTables &s):storage(s),cache(s){}
    void barrier(){storage.barrier();}
    QcomPower::Status publish(const Adreno::MMUTablePage &p){return storage.publish(p);}
    QcomPower::Status allocate(Adreno::MMUTablePage &p){return cache.allocate(p,true);}
    void release(Adreno::MMUTablePage &p,bool){cache.release(p,reusable);}
};
struct MappingBuffer {
    uint64_t address,length;
    const MappingBuffer *root=nullptr;
    unsigned corruptPage=UINT32_MAX;
    const MappingBuffer *backing() const {return root?root:this;}
    bool physicalPage(unsigned n,uint64_t &pa) const {
        if(n>=length/4096)return false;
        pa=0x80000000+uint64_t(n)*8192+(n==corruptPage?4096:0);return true;
    }
};
static void mappedWorkingSets() {
    using namespace QcomPower;
    namespace M=GPU::MappingBounds;
    MappingBuffer root{0x100000000,16*1024*1024};
    MappingBuffer a{root.address+0x1ff080,0x7000,&root},b{root.address+0xc00040,0x2000,&root};
    MappingBuffer *views[]={&a,&b};M::Plan plan;
    assert(plan.recordAddress(views,2,a.address,0x5000) && plan.recordAddress(views,2,b.address+64,64));
    assert(plan.valid(2) && plan.count==2 && plan.known(0) && plan.known(1));
    assert(plan.spans[0].first==root.address+0x1ff000 && plan.spans[0].end==root.address+0x205000);
    assert(plan.spans[1].first==root.address+0xc00000 && plan.spans[1].end==root.address+0xc01000);
    M::RootSelection selected;assert(M::selectRoot(&plan,views,2,0,selected) && !selected.fallback && !selected.overflow);
    unsigned calls=0;assert(!M::visitSelected(&plan,selected,[&](M::PageSpan){++calls;return false;}) && calls==1);
    // Run the production root selection, narrowed page source and private PTE
    // builder against an actual retained tree with noncontiguous physical pages.
    for(bool cursor:{false,true})for(unsigned access:{1u,3u}) {
        RetainedTables storage;storage.pages[0x900000]={};
        struct Hardware {
            Adreno::RetainedPageTables<RetainedTables> tables;
            bool useTranslationCursor;
            Hardware(RetainedTables &s,bool c):tables(s),useTranslationCursor(c){}
        } hardware(storage,cursor);
        Adreno::SMMUContract contract{};contract.version=1;contract.flags=31;contract.asid=5;
        contract.inputBits=40;contract.outputBits=40;contract.streamCount=1;
        contract.ttbr=(5ULL<<48)|0x900000;contract.first=root.address;contract.end=root.address+0x8000000;
        assert(hardware.tables.adopt(contract)==OK);
        struct Source {const MappingBuffer &buffer;Status page(unsigned i,uint64_t &pa){return buffer.physicalPage(i,pa)?OK:Invalid;}} source{root};
        assert(hardware.tables.map(root.address,4096,source,access)==OK);
        Adreno::MappedBufferPages<Hardware,MappingBuffer> proof{hardware,&root};
        assert(proof.validate(4096)==OK);
        Adreno::JobPageTables<RetainedTables> job(storage);assert(job.initialize(40,40,1)==OK);
        unsigned checked=0;
        assert(M::visitSelected(&plan,selected,[&](M::PageSpan span) {
            Adreno::PinnedBufferPages<MappingBuffer> pages{&root,unsigned((span.first-root.address)/4096)};
            const auto n=unsigned((span.end-span.first)/4096);checked+=n;
            return job.map(span.first,n,pages,access)==OK;
        }));
        assert(checked==7 && job.mappedPages()==7); // full backing would be4096 pages
        for(unsigned i=0;i<4096;++i) {
            const auto va=root.address+uint64_t(i)*4096;uint64_t pa=0;
            const bool included=(i>=0x1ff && i<0x205) || i==0xc00;
            assert(job.translate(va+123,pa)==(included?OK:NotReady));
            if(!included)continue;
            uint64_t expected=0;assert(root.physicalPage(i,expected) && pa==expected+123);
            // Independently inspect real AP and XN leaves, not just translate().
            uint64_t table=job.ttbr()&0x0000fffffffff000ULL,pte=0;
            for(unsigned level=0;level<4;++level) {
                pte=storage.pages.at(table)[(va>>(39-level*9))&511];assert((pte&3)==3);
                table=pte&0x0000fffffffff000ULL;
            }
            assert(bool(pte&(1ULL<<7))==!(access&2) && (pte&(3ULL<<53))==(3ULL<<53));
        }
        uint64_t pa=0;assert(job.translate(root.address-4096,pa)==NotReady && job.translate(contract.end,pa)==NotReady);
        assert(job.publish()==OK && job.destroy(false)==Busy && job.destroy(true)==OK);
        // A stale prepared PA must still fail, with and without cursor reuse.
        Adreno::MappedBufferPages<Hardware,MappingBuffer> pages{hardware,&root,0x1ff};
        assert(pages.page(0,pa)==OK);root.corruptPage=0x200;
        assert(pages.page(1,pa)==Stale);
        assert(proof.validate(4096)==Stale);
        Adreno::JobPageTables<RetainedTables> rejected(storage);assert(rejected.initialize(40,40,2)==OK);
        assert(rejected.map(root.address+0x1ff000,2,pages,access)==Stale && rejected.mappedPages()==1);
        assert(rejected.destroy(false)==OK);root.corruptPage=UINT32_MAX;
        assert(pages.page(4096,pa)==Stale && pages.page(UINT32_MAX,pa)==Invalid);
        assert(hardware.tables.unmap(root.address,4096)==OK && hardware.tables.destroy()==OK);
        assert(storage.allocations==storage.releases);
    }
    {
        // One untracked or overflowed view forces its ENTIRE shared backing
        // through the prior path. A complete plan never fills disjoint gaps.
        M::Plan partial;assert(partial.recordAddress(views,2,a.address,4));
        assert(M::selectRoot(&partial,views,2,0,selected) && selected.fallback && !selected.overflow);
        unsigned pages=0;assert(M::visitSelected(&partial,selected,[&](M::PageSpan span){pages+=unsigned((span.end-span.first)/4096);return true;}));
        assert(pages==4096);
        assert(M::selectRoot(static_cast<const M::Plan *>(nullptr),views,2,1,selected) && selected.fallback);
        M::Ranges<2> small;MappingBuffer *whole[]={&root};
        for(unsigned i=0;i<3;++i)assert(small.recordAddress(whole,1,root.address+i*8192,4));
        assert(small.valid(1) && small.count==2 && small.overflow(0));
        assert(M::selectRoot(&small,whole,1,0,selected) && selected.fallback && selected.overflow);
        pages=0;assert(M::visitSelected(&small,selected,[&](M::PageSpan span){pages+=unsigned((span.end-span.first)/4096);return true;}) && pages==4096);
        assert(small.recordAddress(whole,1,root.address,5*4096) && small.count==1 && small.valid(1) && small.overflow(0));
        auto invalid=a;invalid.address=root.address-1;MappingBuffer *bad[]={&invalid};
        assert(!M::selectRoot(&partial,bad,1,0,selected));invalid=a;invalid.length=root.length;
        assert(!M::selectRoot(&partial,bad,1,0,selected));
        assert(!partial.recordAddress(views,2,0,1) && !partial.recordAddress(views,2,a.address,0) &&
            !partial.recordAddress(views,2,a.address,a.length+1) && !partial.recordAddress(views,2,UINT64_MAX-3,8));
        M::Plan damaged=plan;damaged.knownMask|=4;assert(!damaged.valid(2));
        damaged=plan;damaged.spans[1].first=damaged.spans[0].end;assert(!damaged.valid(2));
        damaged=plan;damaged.overflowMask=4;assert(!damaged.valid(2));
        MappingBuffer *sixtyFour[64];for(auto &buffer:sixtyFour)buffer=&root;M::Plan all;
        assert(all.recordAddress(sixtyFour,64,root.address+7,1) && all.valid(64) && all.known(63) && !all.known(64));
    }
    {
        // Compare the actual interval union against independently marked pages
        // across repeated insertions, overlap, adjacency and reverse ordering.
        MappingBuffer *whole[]={&root};M::Plan ranges;std::array<bool,4096> expected{};
        uint32_t random=0x7612ab34;
        for(unsigned n=0;n<700;++n) {
            random=random*1664525+1013904223;const unsigned first=(random>>8)%512;
            random=random*1664525+1013904223;const unsigned bytes=1+(random>>16)%32768;
            const auto address=root.address+uint64_t(first)*4096+17;
            assert(ranges.recordAddress(whole,1,address,bytes) && ranges.valid(1));
            for(unsigned p=first;p<first+(17+bytes+4095)/4096;++p)expected[p]=true;
        }
        assert(!ranges.overflowMask);
        for(unsigned p=0;p<4096;++p) {
            bool found=false;for(unsigned n=0;n<ranges.count;++n)
                found|=root.address+uint64_t(p)*4096>=ranges.spans[n].first && root.address+uint64_t(p)*4096<ranges.spans[n].end;
            assert(found==expected[p]);
        }
    }
    {
        // Shader-visible geometry includes all mip levels/array slices and
        // MSAA samples. A small draw scissor never reduces these mapped bytes.
        using namespace GPU::Shader;
        Texture textures[]={ {1,0,64,32,256,RGBA8,5,0,3,65536},
            {1,0x40000,37,19,512,RGBA8,0,0,0,0,0,2},
            {1,0x80000,64,32,256,Depth32,4,0,2,65536} };
        MappingBuffer *whole[]={&root};
        for(const auto &texture:textures) {
            uint64_t bytes=0;assert(textureBytes(texture,bytes));
            M::Plan ranges;assert(ranges.recordAddress(whole,1,root.address+texture.offset,bytes));
            assert(ranges.count==1 && ranges.spans[0].first==root.address+texture.offset &&
                ranges.spans[0].end==root.address+texture.offset+((bytes+4095)&~uint64_t(4095)));
            if(texture.mipLevels || texture.layers)assert(bytes>uint64_t(texture.pitch)*texture.height);
        }
    }
    M::Work work,delta;delta.jobs=1;delta.roots=2;delta.fullPages=4096;delta.mappedPages=7;delta.fallbackRoots=1;delta.overflowRoots=1;
    work.add(delta);work.add(delta);assert(work.jobs==2 && work.roots==4 && work.fullPages==8192 && work.mappedPages==14 && work.overflowRoots==2);
    work.fullPages=UINT64_MAX-1;work.add(delta);assert(work.fullPages==UINT64_MAX);
}
static void privateAddressSpaces() {
    using namespace QcomPower;
    RetainedTables pages;
    Adreno::JobPageTables<RetainedTables> a(pages),b(pages);
    assert(a.initialize(40,40,1)==OK && b.initialize(40,40,2)==OK && a.ttbr()!=b.ttbr());
    struct Source {uint64_t first;Status page(unsigned index,uint64_t &pa){pa=first+uint64_t(index)*4096;return OK;}};
    {
        // Model a noncoherent walker: CPU stores are invisible until the
        // original table descriptor is cleaned. Initial RAM is stale, and
        // every parent must follow all its children in publication order.
        struct NoncoherentTables : RetainedTables {
            std::map<uint64_t,std::array<uint64_t,512>> visible;
            std::vector<uint64_t> cleaned;
            unsigned fail=UINT32_MAX,barriers=0;
            void barrier(){++barriers;}
            Status publish(const Adreno::MMUTablePage &page) {
                if(!validate(page))return Stale;
                if(cleaned.size()==fail)return Failed;
                constexpr uint64_t mask=0x0000fffffffff000ULL;
                for(auto entry:pages.at(page.physical))if(entry && (entry&~mask)==3) {
                    const auto child=entry&mask;
                    assert(visible.count(child) && visible.at(child)==pages.at(child));
                }
                visible[page.physical]=pages.at(page.physical);cleaned.push_back(page.physical);return OK;
            }
        } store;
        Adreno::JobPageTables<NoncoherentTables> tree(store);Source source{0x9000000};
        assert(tree.initialize(40,40,11)==OK && tree.map(0x3ffff000,514,source,1)==OK);
        const auto root=tree.ttbr()&0x0000fffffffff000ULL;assert(store.visible.empty());
        assert(tree.publish()==OK && tree.published() && !tree.faulted());
        assert(store.barriers>=2 && store.cleaned.size()==tree.tableCount() && store.cleaned.back()==root);
        for(unsigned i=0;i<514;++i) {
            const auto va=0x3ffff000+uint64_t(i)*4096;uint64_t table=root;
            for(unsigned level=0;level<4;++level) {
                const auto entry=store.visible.at(table)[(va>>(39-9*level))&511];
                table=entry&0x0000fffffffff000ULL;
                if(level==3)assert(table==source.first+uint64_t(i)*4096 && (entry&(1ULL<<7)) &&
                    (entry&(3ULL<<53))==(3ULL<<53));
            }
        }
        const auto count=tree.tableCount();
        assert(tree.publish()==Busy && store.cleaned.size()==count && tree.destroy(false)==Busy);
        assert(tree.destroy(true)==OK && store.pages.empty());
        for(unsigned failure=0;failure<count;++failure) {
            NoncoherentTables broken;broken.fail=failure;Adreno::JobPageTables<NoncoherentTables> rejected(broken);
            assert(rejected.initialize(40,40,12)==OK && rejected.map(0x3ffff000,514,source,3)==OK);
            assert(rejected.publish()==Failed && !rejected.published() && rejected.faulted());
            assert(broken.cleaned.size()==failure && broken.visible.count(rejected.ttbr()&0x0000fffffffff000ULL)==0);
            assert(rejected.publish()==NotReady && rejected.destroy(true)==Busy && !broken.releases);
        }
        // Recycled cacheable pages contain a different old job. Zeroing and
        // republishing must erase old data in both CPU and walker views.
        NoncoherentTables recycled;CachedPrivatePages allocator(recycled);
        // Use this wrapper so the model's publication behavior is preserved.
        struct RecycledTables {
            NoncoherentTables &store;CachedPrivatePages &allocator;
            Status allocate(Adreno::MMUTablePage &p){return allocator.allocate(p);}
            void release(Adreno::MMUTablePage &p,bool owned){allocator.release(p,owned);}
            void barrier(){store.barrier();}
            Status publish(const Adreno::MMUTablePage &p){return store.publish(p);}
        } api{recycled,allocator};
        Adreno::JobPageTables<RecycledTables> old(api),next(api);
        assert(old.initialize(40,40,13)==OK && old.map(0x5000000,513,source,3)==OK && old.publish()==OK);
        allocator.reusable=true;assert(old.destroy(true)==OK && allocator.cache.count()==5);
        recycled.visible.clear();recycled.cleaned.clear();
        assert(next.initialize(40,40,14)==OK && next.map(0x7000000,1,source,1)==OK && next.publish()==OK);
        uint64_t pa=0;assert(next.translate(0x5000000,pa)==NotReady && next.translate(0x7001000,pa)==NotReady);
        assert(next.translate(0x7000000,pa)==OK && pa==source.first);
        allocator.reusable=false;assert(next.destroy(true)==OK && allocator.cache.drain(true)==OK && recycled.pages.empty());
    }
    {
        // A live/published tree cannot feed the pool. Unsubmitted cleanup
        // takes the ordinary release path, even if pages came from the pool.
        RetainedTables store;CachedPrivatePages allocator(store);
        namespace AS=GPU::Diagnostics::AddressSpaces;
        AS::Metrics metrics;static uint64_t ticks=0;
        AS::Trace trace{&metrics,nullptr,[]()->uint64_t{return ++ticks;},false};
        Adreno::JobPageTables<CachedPrivatePages> tree(allocator,&trace);
        Source source{0x9000000};assert(tree.initialize(40,40,1)==OK && tree.map(0x5000000,513,source,1)==OK);
        assert(!ticks && !metrics.timing[AS::Source].samples); // no per-page timer reads without sampling
        assert(tree.publish()==OK && tree.destroy(false)==Busy && !allocator.cache.count());
        const auto allocated=store.allocations;assert(tree.destroy(true)==OK && store.releases==allocated && !allocator.cache.count());
        trace.detailed=true;
        assert(tree.initialize(40,40,2)==OK && tree.map(0x5000000,513,source,1)==OK);
        assert(metrics.timing[AS::Source].samples==513 && metrics.timing[AS::Allocate].samples==tree.tableCount() &&
            metrics.timing[AS::Zero].samples==tree.tableCount());
        const auto tables=tree.tableCount();assert(tables==5);
        allocator.reusable=true;assert(tree.destroy(true)==OK && allocator.cache.count()==tables);
        assert(allocator.cache.drain(false)==Busy && allocator.cache.count()==tables);
        allocator.reusable=false;assert(tree.initialize(40,40,3)==OK);
        uint64_t pa=0;assert(tree.translate(0x5000000,pa)==NotReady && tree.translate(0x5200000,pa)==NotReady);
        assert(tree.map(0x5000000,1,source,3)==OK && tree.translate(0x5001000,pa)==NotReady);
        assert(tree.destroy(true)==OK && allocator.cache.count()==1); // unpublished cleanup does not recache
        assert(allocator.cache.drain(true)==OK && store.pages.empty());
    }
    {
        RetainedTables store;Adreno::RetiredTablePageCache<RetainedTables,2> cache(store);
        Adreno::MMUTablePage p[3]{};
        for(auto &page:p)assert(cache.allocate(page,true)==OK);
        for(auto &page:p)cache.release(page,true);
        assert(cache.count()==2 && store.releases==1 && cache.allocations==3);
        // Corrupt one retained descriptor's recorded CPU view. Validation
        // fails before handing it to a new tree; the allocation stays pinned.
        assert(cache.allocate(p[0],true)==OK && cache.hits==1);
        p[0].entries=nullptr;cache.release(p[0],true);
        assert(cache.allocate(p[0],true)==Failed && !p[0].owner && cache.faulted() && cache.count()==2);
        assert(cache.validationFailures==1 && cache.drain(true)==Busy && store.pages.size()==2 && store.releases==1);
    }
    {
        // Exercise the actual page-table builder with the full-screen storage
        // plans, including the guard directly after the compact timestamp page.
        const GPU::Shader::Program *fetch=nullptr;
        for(const auto &p:GPUPrograms::Programs)if(!strcmp(p.name,"framebuffer_source_over"))fetch=&p;
        assert(fetch);GPU::Shader::Draw draw{};draw.primitive=3;draw.target={1,0,1080,1920,4352,GPU::Shader::RGBA8};
        Adreno::FramebufferStorage compact,full;
        assert(Adreno::framebufferStoragePlan(draw,*fetch,true,false,compact) &&
            Adreno::framebufferStoragePlan(draw,*fetch,true,false,full,false));
        unsigned tableCounts[2]{};
        for(unsigned mode=0;mode<2;++mode) {
            const auto bytes=mode?full.bytes:compact.bytes;
            RetainedTables store;Adreno::JobPageTables<RetainedTables> tree(store);
            struct CheckedPages {
                unsigned calls=0;
                Status page(unsigned i,uint64_t &pa){assert(i==calls++);pa=0x9000000+uint64_t(i)*8192;return OK;}
            } source;
            assert(tree.initialize(40,40,3)==OK && tree.map(0x5000000,bytes/4096,source,3)==OK);
            assert(source.calls==bytes/4096 && tree.mappedPages()==bytes/4096);
            tableCounts[mode]=tree.tableCount();uint64_t physical=0;
            assert(tree.translate(0x5000000+127,physical)==OK && physical==0x9000000+127);
            assert(tree.translate(0x5000000+bytes,physical)==NotReady);
            assert(tree.publish()==OK && tree.destroy(false)==Busy && tree.destroy(true)==OK && store.pages.empty());
        }
        assert(compact.bytes/4096==1 && full.bytes/4096==2041 && tableCounts[0]==4 && tableCounts[1]==7);
    }
    {
        RetainedTables storage;Adreno::JobPageTables<RetainedTables> tree(storage);
        assert(tree.initialize(40,40,3)==OK);
        struct CheckedSource {
            unsigned calls=0,fail=UINT32_MAX;
            Status page(unsigned index,uint64_t &pa) {
                assert(index==calls++);pa=0x9000000+uint64_t(index)*8192;
                return index==fail?Stale:OK;
            }
        } checked;
        // Unaligned first L3 index, crossing both 2MiB and1GiB boundaries;
        // independently read every produced descriptor, including RO/XN.
        constexpr uint64_t start=0x3ffff000;
        assert(tree.map(start,514,checked,1)==OK && checked.calls==514);
        for(unsigned i=0;i<514;++i) {
            uint64_t table=tree.ttbr()&0x0000fffffffff000ULL;
            for(unsigned level=0;level<4;++level) {
                auto pte=storage.pages.at(table)[((start+uint64_t(i)*4096)>>(39-9*level))&511];
                assert((pte&3)==3);table=pte&0x0000fffffffff000ULL;
                if(level==3)assert(table==0x9000000+uint64_t(i)*8192 && (pte&(1ULL<<7)) && (pte&(3ULL<<53))==(3ULL<<53));
            }
        }
        checked.calls=0;checked.fail=1;
        assert(tree.map(0x50000000,3,checked,3)==Stale && checked.calls==2);
        uint64_t pa=0;assert(tree.translate(0x50000000,pa)==OK && tree.translate(0x50001000,pa)==NotReady);
        assert(tree.destroy(true)==OK && storage.pages.empty());
    }
    constexpr uint64_t ringVA=0x3000000,ibVA=0x3100000,fenceVA=0x3200000,targetVA=0x4000000;
    Source ring{ringVA},ib{ibVA},dataA{0x8100000},dataB{0x8200000};
    for(auto *tables:{&a,&b}) {
        assert(tables->map(ringVA,8,ring,1)==OK && tables->map(ibVA,4,ib,1)==OK);
        uint64_t pa=0;assert(tables->translate(fenceVA,pa)==NotReady); // private code cannot forge the fence
    }
    assert(a.map(targetVA,1,dataA,3)==OK && b.map(targetVA,1,dataB,3)==OK);
    auto count=a.mappedPages();assert(a.map(targetVA,1,dataA,3)==OK && a.mappedPages()==count);
    assert(a.map(targetVA,1,dataB,3)==Denied);
    assert(a.publish()==OK && b.publish()==OK && a.map(targetVA+4096,1,dataA,3)==Busy);
    assert(a.destroy(false)==Busy && !pages.releases);
    Device device;auto *ringCPU=device.allocate(ringVA,Adreno::RingDwords);
    auto *fence=device.allocate(fenceVA,1024);auto *commands=device.allocate(ibVA,Adreno::IBDwords);
    auto *outA=device.allocate(dataA.first,1024),*outB=device.allocate(dataB.first,1024);
    // Independently walk the actual produced descriptors, including AP and
    // nG, instead of using the driver's translate() to authorize test DMA.
    device.kernelTTBR=device.activeTTBR=0x900000;
    device.translate=[&](uint64_t root,uint64_t va,bool write,uint64_t &pa) {
        if(root==device.kernelTTBR){pa=va;return true;}
        uint64_t physical=root&0x0000fffffffff000ULL;
        for(unsigned level=0;level<4;++level) {
            auto page=pages.pages.find(physical);if(page==pages.pages.end())return false;
            uint64_t pte=page->second[(va>>(39-9*level))&511];
            if((pte&3)!=3)return false;
            physical=pte&0x0000fffffffff000ULL;
            if(level==3) {
                if(!(pte&(1u<<11)) || (write && (pte&(1u<<7))))return false;
                pa=physical|(va&4095);return true;
            }
        }
        return false;
    };
    uint64_t pa=0;
    assert(device.translate(a.ttbr(),ringVA,false,pa) && !device.translate(a.ttbr(),ringVA,true,pa));
    assert(device.translate(a.ttbr(),ibVA,false,pa) && !device.translate(a.ttbr(),ibVA,true,pa));
    assert(!device.translate(a.ttbr(),fenceVA,true,pa) && !device.translate(a.ttbr(),targetVA+4096,true,pa));
    Adreno::Engine<Device> engine(device);
    device.pair(Adreno::RBBase,ringVA); // firmware initialized this exact ring
    assert(engine.start(contract(),ringCPU,ringVA,fence,fenceVA)==Result::Success);
    Adreno::Encoder encoder(commands,Adreno::IBDwords);assert(encoder.fill(targetVA,4,0x12345678)==Result::Success);
    const Adreno::PageTableContext contextA{a.ttbr(),device.kernelTTBR},contextB{b.ttbr(),device.kernelTTBR};
    assert(contextA.valid() && contextB.valid());
    uint32_t sequence=0,completed=0;
    assert(engine.post(ibVA,encoder.size(),sequence,&contextA)==Result::Success && *outA==0x12345678 && !*outB);
    assert(engine.post(ibVA,encoder.size(),sequence,&contextB)==Result::Success && *outB==0x12345678);
    assert(engine.poll(completed)==Result::Success && completed==sequence && device.activeTTBR==device.kernelTTBR);
    assert((device.selectedRoots==std::vector<uint64_t>{a.ttbr(),device.kernelTTBR,b.ttbr(),device.kernelTTBR}));
    {
        Adreno::Encoder second(commands+1024,1024);assert(second.fill(targetVA+4,4,0xabcdef01)==Result::Success);
        Adreno::IndirectBuffer buffers[]={{ibVA,encoder.size(),0},{ibVA+4096,second.size(),0}};
        auto submitted=engine.lastIssued(),doorbells=device.doorbells;auto roots=device.selectedRoots.size();
        buffers[1].reserved=1;
        assert(engine.postBatch(buffers,2,sequence,&contextA)==Result::Invalid && engine.lastIssued()==submitted && device.doorbells==doorbells);
        buffers[1].reserved=0;device.execute=false;device.regs[0x464*4]=1;device.regs[0x46b*4]=6;
        Adreno::ExecutionToken profile;
        assert(engine.postBatch(buffers,2,sequence,&contextA,&profile)==Result::Success && sequence==submitted+1 && *fence==submitted && device.doorbells==doorbells+1);
        device.run();device.execute=true;
        assert(engine.poll(completed)==Result::Success && completed==sequence && device.activeTTBR==device.kernelTTBR);
        uint64_t begin=0,end=0;assert(engine.executionInterval(profile,begin,end) && end-begin==19200);
        assert(engine.executionCoreInterval(profile,begin,end) && end-begin==596000);
        assert(engine.executionBusyInterval(profile,begin,end) && end-begin==1192000);
        assert(outA[1]==0xabcdef01 && !outB[1]);
        assert(device.selectedRoots.size()==roots+2); // one enter/leave and one fence for both IBs
    }
    {
        // Execute real page-table switches and register-to-memory packets.
        // Private timestamps must use this tree's writable scratch, while
        // the global completion/frequency cells remain absent from that tree.
        using namespace GPU::HardwarePhases;
        device.phaseScratchVA=targetVA+ScratchOffset;
        Adreno::ExecutionToken token;const auto roots=device.selectedRoots.size();
        auto issued=engine.lastIssued(),doorbells=device.doorbells;
        assert(engine.post(ibVA,encoder.size(),sequence,&contextA,nullptr,device.phaseScratchVA)==Result::Invalid);
        assert(engine.post(ibVA,encoder.size(),sequence,nullptr,&token,device.phaseScratchVA)==Result::Invalid);
        assert(engine.post(ibVA,encoder.size(),sequence,&contextA,&token,device.phaseScratchVA+1)==Result::Invalid);
        assert(engine.lastIssued()==issued && device.doorbells==doorbells && !token.epoch);
        assert(engine.post(ibVA,encoder.size(),sequence,&contextA,&token,device.phaseScratchVA)==Result::Success);
        assert(engine.poll(completed)==Result::Success && completed==sequence && device.activeTTBR==device.kernelTTBR);
        assert(device.selectedRoots.size()==roots+2 && !device.memoryFault);
        uint64_t begin=0,end=0;
        assert(engine.executionInterval(token,begin,end));
        const uint64_t entered=outA[16]|(uint64_t(outA[17])<<32),drained=outA[18]|(uint64_t(outA[19])<<32);
        assert(entered-begin==19200 && drained-entered==19200 && end-drained==19200);
        assert(!device.translate(a.ttbr(),fenceVA,true,pa) && engine.retireExecution(token));
        Snapshot phases;phases.sample(true,true,begin,entered,drained,end);
        assert(phases.native.enter.samples==1 && phases.compositorRender.commands.samples==1);
        assert(phases.compositor.enter.totalTicks+phases.compositor.commands.totalTicks+phases.compositor.leave.totalTicks==end-begin);
        phases.skip(true,false);assert(phases.native.skipped==1 && phases.compositor.skipped==1 && !phases.compositorRender.skipped);
        phases.sample(true,true,begin,begin-1,drained,end);
        phases.sample(true,true,begin,entered,end+1,end);
        phases.sample(false,false,0,entered,drained,end);
        assert(phases.native.invalid==3 && phases.compositor.invalid==2 && phases.compositorRender.invalid==2);
        assert(phases.native.commands.samples==1 && phases.compositor.commands.samples==1);
        device.phaseScratchVA=0;
    }
    const auto issued=engine.lastIssued();auto invalid=contextA;invalid.job=invalid.kernel;
    assert(engine.post(ibVA,encoder.size(),sequence,&invalid)==Result::Invalid && engine.lastIssued()==issued);
    {
        CachedPrivatePages allocator(pages);
        Adreno::JobPageTables<CachedPrivatePages> c(allocator);
        assert(c.initialize(40,40,3)==OK && c.map(ringVA,8,ring,1)==OK && c.map(ibVA,4,ib,1)==OK &&
            c.map(targetVA,1,dataA,3)==OK && c.publish()==OK);
        const Adreno::PageTableContext contextC{c.ttbr(),device.kernelTTBR};
        Adreno::ExecutionToken sampleC,sampleB;
        device.execute=false;
        assert(engine.post(ibVA,encoder.size(),sequence,&contextC,&sampleC)==Result::Success);
        assert(engine.post(ibVA,encoder.size(),sequence,&contextB,&sampleB)==Result::Success);
        for(unsigned packets=0;*fence<sampleC.sequence;++packets){assert(packets<128);device.run(1);}
        assert(engine.poll(completed)==Result::Success && completed==sampleC.sequence);
        assert(c.destroy(false)==Busy && !allocator.cache.count());
        uint64_t begin=0,end=0;assert(engine.executionInterval(sampleC,begin,end));
        assert(!engine.executionInterval(sampleB,begin,end) && !engine.retireExecution(sampleB));
        for(unsigned packets=0;device.activeTTBR!=b.ttbr();++packets){assert(packets<128);device.run(1);}
        // The previous root can be destroyed while B is selected only after
        // real C completion and its ASID invalidation. B's private mappings
        // and kernel-only profiling destination remain available throughout.
        struct TLB {
            std::vector<std::pair<uint32_t,uint32_t>> writes;
            void barrier(){} void delay(unsigned){assert(false);}
            void write(uint32_t r,uint32_t v){writes.emplace_back(r,v);}
            uint32_t read(uint32_t r){assert(r==0x87f4);return 0;}
        } tlb;
        assert(device.activeTTBR!=c.ttbr() && SMMU::synchronizeContext(tlb,0x8000,3)==OK);
        assert((tlb.writes==std::vector<std::pair<uint32_t,uint32_t>>{{0x8610,3},{0x87f0,UINT32_MAX}}));
        const auto tableCount=c.tableCount(),allocations=pages.allocations;
        allocator.reusable=true;
        assert(c.destroy(true)==OK && engine.retireExecution(sampleC) && allocator.cache.count()==tableCount);
        allocator.reusable=false;
        Adreno::JobPageTables<CachedPrivatePages> d(allocator);
        assert(d.initialize(40,40,4)==OK && (d.ttbr()&0x0000fffffffff000ULL)==(contextC.job&0x0000fffffffff000ULL));
        // Cached root is fully zeroed. Old target, command and ring leaves
        // disappear before constructing D, even while B executes its own tree.
        assert(d.translate(targetVA,pa)==NotReady && d.translate(ibVA,pa)==NotReady && d.translate(ringVA,pa)==NotReady);
        assert(d.map(ringVA,8,ring,1)==OK && d.map(ibVA,4,ib,1)==OK && d.map(targetVA,1,dataB,3)==OK && d.publish()==OK);
        assert(pages.allocations==allocations && allocator.cache.hits==tableCount && !allocator.cache.count());
        assert(!device.translate(d.ttbr(),ibVA,true,pa) && !device.translate(d.ttbr(),fenceVA,true,pa));
        device.run();assert(engine.poll(completed)==Result::Success && completed==sampleB.sequence);
        assert(engine.executionInterval(sampleB,begin,end) && engine.retireExecution(sampleB));
        assert(device.activeTTBR==device.kernelTTBR && !device.memoryFault && outB[0]==0x12345678);
        outA[0]=0x77889900;outB[0]=0;device.execute=true;
        const Adreno::PageTableContext contextD{d.ttbr(),device.kernelTTBR};
        assert(engine.post(ibVA,encoder.size(),sequence,&contextD)==Result::Success &&
            engine.poll(completed)==Result::Success && completed==sequence && device.activeTTBR!=d.ttbr());
        assert(outB[0]==0x12345678 && outA[0]==0x77889900);
        assert(SMMU::synchronizeContext(tlb,0x8000,4)==OK);
        allocator.reusable=true;
        assert(d.destroy(true)==OK && allocator.cache.count()==tableCount && allocator.cache.drain(true)==OK);
    }
    // An address omitted from this job faults before the return-to-kernel
    // footer and cannot update the global success timestamp.
    Adreno::Encoder denied(commands,Adreno::IBDwords);assert(denied.fill(targetVA+4096,4,9)==Result::Success);
    const auto previous=*fence;
    Adreno::ExecutionToken faulted,behindFault;device.execute=false;
    assert(engine.post(ibVA,denied.size(),sequence,&contextA,&faulted)==Result::Success);
    assert(engine.post(ibVA+4096,4,sequence,&contextB,&behindFault)==Result::Success);
    device.run();assert(device.memoryFault);
    assert(*fence==previous && device.activeTTBR==a.ttbr() && engine.poll(completed)==Result::Fault);
    uint64_t begin=0,end=0;
    assert(!engine.executionInterval(faulted,begin,end) && !engine.executionInterval(behindFault,begin,end));
    assert(!engine.retireExecution(faulted) && !engine.retireExecution(behindFault));
    assert(a.destroy(false)==Busy && engine.stop());
    // Production performs a proven halt/drain and kernel-root/TLB restoration
    // before supplying the retirement proof consumed here.
    device.activeTTBR=device.kernelTTBR;
    assert(a.destroy(true)==OK && b.destroy(true)==OK && pages.pages.empty());
}
static void retainedTranslationCursor() {
    using namespace QcomPower;
    for(unsigned bits:{39u,40u}) {
        Adreno::SMMUContract c{};c.version=1;c.flags=31;c.asid=5;c.inputBits=bits;c.outputBits=40;c.streamCount=1;
        c.ttbr=(5ULL<<48)|0x900000;c.first=0x100000000;c.end=c.first+0x8000000;
        RetainedTables storage;storage.pages[0x900000]={};
        Adreno::RetainedPageTables<RetainedTables> tree(storage);
        assert(tree.adopt(c)==OK);
        struct Source {uint64_t base=0x80000000;Status page(unsigned n,uint64_t &pa){pa=base+uint64_t(n)*8192;return OK;}} source;
        const uint64_t first=c.first+0x1ff000;constexpr unsigned pages=514;
        assert(tree.map(first,pages,source,1)==OK);
        decltype(tree)::TranslationCursor cursor;
        auto check=[&](uint64_t va,uint64_t expected) {
            uint64_t slow=0,fast=0;assert(tree.translate(va,slow)==OK && tree.translate(va,fast,cursor)==OK);
            assert(fast==slow && fast==expected);
        };
        // Start at the last entry and cross TWO leaf-table boundaries with
        // noncontiguous prepared physical pages, including byte offsets.
        for(unsigned i=0;i<pages;++i)check(first+uint64_t(i)*4096+7,source.base+uint64_t(i)*8192+7);
        auto entry=[&](uint64_t va,unsigned wanted)->uint64_t& {
            auto *table=storage.pages[0x900000].data();
            for(unsigned level=bits>39?0:1;;++level) {
                auto &pte=table[(va>>(39-9*level))&511];
                if(level==wanted)return pte;
                assert((pte&3)==3);table=storage.pages[pte&0x0000fffffffff000ULL].data();
            }
        };
        check(first,source.base);auto savedParent=entry(first,2);
        entry(first,2)=entry(first+4096,2); // Same VA now reaches a different table.
        check(first,source.base+512*8192);entry(first,2)=savedParent;check(first,source.base);
        // Parent becomes a block, then a cycle. The cached table must never
        // conceal either replacement; restoring the parent permits a new walk.
        entry(first,2)=0x60000001;check(first+123,0x601ff07b);
        entry(first,2)=savedParent;check(first,source.base);
        entry(first,2)=0x900003;uint64_t value=0;assert(tree.translate(first,value,cursor)==Failed);
        entry(first,2)=savedParent;check(first,source.base);
        const uint64_t next=first+4096;check(next,source.base+8192);
        auto &leaf=entry(next+4096,3);const auto savedLeaf=leaf;
        leaf=(leaf&~0x0000fffffffff000ULL)|0x91000000;check(next+4096,0x91000000);
        leaf=0;assert(tree.translate(next+4096,value,cursor)==NotReady);
        leaf=(savedLeaf&~3ULL)|2;assert(tree.translate(next+4096,value,cursor)==Failed);
        leaf=savedLeaf;check(next+4096,source.base+16384);
        assert(tree.translate(1ULL<<bits,value,cursor)==Invalid);
        check(first,source.base); // Preserve a cursor to pages freed below.
        assert(tree.unmap(first,pages)==OK && tree.destroy()==OK);
        assert(tree.adopt(c)==OK);source.base=0x92000000;
        assert(tree.map(first,1,source,1)==OK);check(first,source.base);
        assert(tree.unmap(first,1)==OK && tree.destroy()==OK);
        assert(storage.allocations==storage.releases);
    }
}
static void retainedMMU() {
    using namespace QcomPower;
    struct Transition {
        uint32_t initial=0x802018,final=0x802018;unsigned reads=0,delays=0;
        uint32_t read(uint32_t reg){assert(reg==0x8030);return reads++?final:initial;}
        void delay(unsigned us){assert(us==1);++delays;}
    } transition;
    uint32_t observed=0;
    assert(Adreno::settledTranslationControl(transition,0x8030,0x802018,observed) && !transition.delays);
    transition={0x802098,0x802018};
    assert(Adreno::settledTranslationControl(transition,0x8030,0x802018,observed) && transition.delays==1 && observed==0x802018);
    transition={0x802098,0x802098};
    assert(!Adreno::settledTranslationControl(transition,0x8030,0x802018,observed) && transition.reads==8 && transition.delays==7);
    transition={0x802019,0x802018};
    assert(!Adreno::settledTranslationControl(transition,0x8030,0x802018,observed) && !transition.delays);
    Adreno::SMMUContract c{};c.version=1;c.flags=31;c.asid=5;c.inputBits=39;c.outputBits=40;c.streamCount=1;
    c.ttbr=(5ULL<<48)|0x900000;c.first=0x100000000;c.end=c.first+0x8000000;
    assert(c.valid());auto bad=c;bad.flags=0;assert(!bad.valid());
    RetainedTables memory;auto &root=memory.pages[0x900000];root[0]=0x40000001; // inherited firmware 1GiB block
    Adreno::RetainedPageTables<RetainedTables> tables(memory);
    memory.stopped=false;assert(tables.adopt(c)==Busy);memory.stopped=true;
    {
        RetainedTables live;live.stopped=false;live.pages[0x900000][0]=0x40000001;
        SMMU::RetainedPageTables<RetainedTables> display(live);
        assert(display.adoptLive(c)==Denied);live.ownsLive=true;
        assert(display.adoptLive(c)==OK);
        struct Source {Status page(unsigned,uint64_t &pa){pa=0x81000000;return OK;}} source;
        assert(display.map(c.first+4096,1,source,1)==OK);
        assert(display.unmap(c.first+4096,1)==OK && display.destroy()==Busy);
        assert(live.pages[0x900000][0]==0x40000001);
        live.stopped=true;assert(display.destroy()==OK);
    }
    assert(tables.adopt(c)==OK);uint64_t pa=0;
    assert(tables.translate(0x2000000,pa)==OK && pa==0x42000000);
    assert(tables.translate((1ULL<<39)|0x2000000,pa)==Invalid);
    struct Pages {unsigned fail=UINT32_MAX;Status page(unsigned n,uint64_t &p){p=0x80000000+uint64_t(n)*8192;return n==fail?Invalid:OK;}} source;
    assert(tables.map(c.first+4096,3,source,1)==OK && tables.mappedPages()==3);
    uint64_t leaf=0;assert(tables.readOnlyBootLeaf(c.first+4096,leaf)==OK && (leaf&(1u<<7)));
    assert(tables.readOnlyBootLeaf(0x2000000,leaf)==Denied); // block mappings cannot be firmware pins
    assert(tables.translate(c.first+8192+7,pa)==OK && pa==0x80002007);
    assert(root[0]==0x40000001 && root[4] && memory.allocations==2);
    assert(tables.map(c.first+4096,1,source,3)==Busy);
    assert(tables.unmap(c.first+4096,3)==OK && !tables.mappedPages());
    source.fail=1;assert(tables.map(c.first+4096,3,source,3)==Invalid && !tables.mappedPages());
    assert(tables.translate(c.first+4096,pa)==NotReady && !memory.releases);
    memory.stopped=false;assert(tables.destroy()==Busy && !memory.releases);
    memory.stopped=true;assert(tables.destroy()==OK && memory.releases==2 && !root[4] && root[0]==0x40000001);
    // A busy inherited window is never overwritten or treated as allocator space.
    root[4]=0x80000001;Adreno::RetainedPageTables<RetainedTables> busy(memory);
    assert(busy.adopt(c)==Busy && root[4]==0x80000001 && busy.destroy()==OK);root[4]=0;
    Adreno::RetainedPageTables<RetainedTables> failed(memory);assert(failed.adopt(c)==OK);
    source.fail=UINT32_MAX;memory.timeout=true;
    assert(failed.map(c.first+4096,1,source,3)==TimedOut && failed.faulted());
    auto retained=memory.releases;assert(failed.unmap(c.first+4096,1)==Stale && failed.destroy()==Busy);
    assert(memory.releases==retained && root[0]==0x40000001); // unknown TLB completion pins all tables
    // Exercise the actual bank-0 initializer alongside the retained mapper.
    // The register model covers admission, ownership and failed handshakes;
    // it supplies no evidence of physical SMMU translation.
    struct Registers {
        std::map<uint32_t,uint32_t> words;
        std::vector<std::pair<uint32_t,uint32_t>> writes;
        bool stopped=true,stall=false,refuse=false;unsigned waited=0;
        Registers() {
            words[0x20]=(1u<<30)|(1u<<27)|(8u<<9)|128;words[0x24]=0x20000003;words[0x28]=0x1222;
            words[0]=1;words[0x804]=0x80000011;words[0xc04]=1; // another SID/bank stays intact
            words[0xc00]=0x100ff;
        }
        uint32_t read(uint32_t at) {return at==0x87f4 && stall?1:words[at];}
        void write(uint32_t at,uint32_t value) {
            writes.emplace_back(at,value);if(!(refuse && at==0xc00 && !value))words[at]=value;
            if(at==0x8000 && (value&1))words[0x8058]=0x400; // quiet AArch64 format, observed on hardware
        }
        void barrier() {}
        void delay(unsigned us) {waited+=us;}
        QcomPower::Status isolated() {return stopped?QcomPower::OK:QcomPower::Busy;}
    } regs;
    std::array<uint64_t,512> blank{};
    BridgeGPU::SMMUStartup<Registers> init(regs);
    assert(init.probe(0x10000000,0x14000000) && init.inputBits()==40 && init.outputBits()==40 && init.bankOffset()==0x8000 && init.streamSlot()==0);
    regs.stopped=false;assert(!init.initialize(0x90000000,blank.data()) && regs.writes.empty());regs.stopped=true;
    blank[0]=3;assert(!init.initialize(0x90000000,blank.data()) && regs.writes.empty());blank[0]=0;
    assert(init.initialize(0x90000000,blank.data()) && init.ownsHardware() && init.unchanged());
    assert(regs.words[0x8058]==0x400);regs.words[0x8058]|=2;assert(!init.unchanged());regs.words[0x8058]=0x400;
    assert(regs.words[0x8020]==0x90000000 && !regs.words[0x8024] && regs.words[0x8000]==0x1167);
    assert(regs.words[0x8010]==0x38012 && regs.words[0x8030]==0x802018 && regs.words[0x8038]==0x44);
    assert(regs.words[0x1000]==0x1f300 && regs.words[0x1800]==1 && regs.words[0x800]==0x80000000 && !regs.words[0xc00]);
    assert(regs.words[0x804]==0x80000011 && regs.words[0xc04]==1);
    unsigned invalidations=0,allASIDs=0;
    for(auto w:regs.writes){if(w.first==0x8610){assert(!w.second);++invalidations;}if(w.first==0x8618){assert(!w.second);++allASIDs;}}
    assert(invalidations==1 && allASIDs==1); // old globals removed before M=1
    Adreno::SMMUContract fresh{1,31,0,0,40,40,1,0,0x90000000,0x10000000,0x14000000,{{0,0}}};
    assert(fresh.valid());RetainedTables newMemory;newMemory.pages[0x90000000]={};
    Adreno::RetainedPageTables<RetainedTables> native(newMemory);
    assert(native.adopt(fresh)==OK && native.map(fresh.first,1,source,3)==OK);
    assert(native.translate(fresh.first+12,pa)==OK && pa==0x8000000c);
    for(auto w:regs.writes) assert(w.first!=0x804 && w.first!=0xc04 && w.first!=0x1004 && w.first!=0x1804 &&
        w.first!=0x68 && w.first!=0x6c && w.first!=0x70 && !(w.first>=0x9000 && w.first<0xa000));
    regs.words[0xc04]=2;assert(!init.unchanged());
    Registers stopped;stopped.words[0x8000]=0x1167;BridgeGPU::SMMUStartup<Registers> replacement(stopped);
    stopped.stopped=false;assert(!replacement.probe(0x10000000,0x14000000,true) && stopped.writes.empty());
    stopped.stopped=true;assert(replacement.probe(0x10000000,0x14000000,true));
    assert(replacement.initialize(0x90000000,blank.data()));
    for(unsigned failure=0;failure<5;++failure) {
        Registers r;BridgeGPU::SMMUStartup<Registers> rejected(r);
        if(failure==0)r.words[0x8000]=1; // live bank
        if(failure==1)r.words[0x804]=0x80010000; // masked alias of SID 0
        if(failure==2)r.words[0xc04]=0; // another valid SID uses bank 0
        if(failure<3){assert(!rejected.probe(0x10000000,0x14000000) && r.writes.empty());continue;}
        assert(rejected.probe(0x10000000,0x14000000));r.stall=failure==3;r.refuse=failure==4;
        assert(!rejected.initialize(0x90000000,blank.data()) && rejected.ownsHardware());
        if(r.stall)assert(r.waited>=100000 && !r.words[0x8000]);
        if(r.refuse)assert(r.words[0xc00]==0x100ff);
        assert(!rejected.initialize(0x90000000,blank.data())); // no retry after partial hardware mutation
    }
}
static void surfaceBacking() {
    using namespace GPU;
    {
        Adreno::ProgressWatchdog queued;
        assert(queued.poll(10000,0,0,500)==Result::Success && queued.poll(10499,0,0,500)==Result::Success);
        assert(queued.poll(10500,0,0,500)==Result::Timeout);
        Adreno::ProgressWatchdog active;assert(active.poll(0,0,8,500)==Result::Success);
        assert(active.poll(499,3,8,500)==Result::Success && active.poll(900,5,8,500)==Result::Success);
        assert(active.poll(901,4,8,500)==Result::Fault && active.poll(901,9,8,500)==Result::Fault);
    }
    constexpr uint64_t pitch=7936,image=pitch*1080,offset=4096,allocation=(image+offset+16383)&~uint64_t(16383);
    Surface::Info surface{Surface::Version,Surface::Linear,7,0,1920,1080,pitch,Surface::BGRA,
        offset,allocation,image,4,0};
    assert(Surface::valid(surface));
    // Native IOSurface logical allocations need not end on a page. DMA
    // padding does not enlarge the user/resource range or hide a short tail.
    for(uint64_t logical:std::array<uint64_t,4>{1152,4700224,MaxBufferBytes-1,MaxBufferBytes}) {
        const auto span=pageSpanBytes(logical,16384);
        assert(span && span>=logical && !(span&16383));
        for(auto descriptor:{logical,span})for(auto dma:{descriptor,pageSpanBytes(descriptor,16384)})
            assert(Surface::descriptorLayout(logical,descriptor,dma,0,16384));
        assert(!Surface::descriptorLayout(logical,logical-1,span,0,16384));
        assert(!Surface::descriptorLayout(logical,logical,span+4096,0,16384));
        assert(!Surface::descriptorLayout(logical,logical,span,1,16384));
    }
    assert(!pageSpanBytes(0) && !pageSpanBytes(UINT64_MAX) && !pageSpanBytes(MaxBufferBytes+1));
    assert(!pageSpanBytes(64,3072) && !pageSpanBytes(64,131072));
    Surface::Info tail{Surface::Version,Surface::Linear,9,0,810,1440,3264,Surface::BGRA,64,4700224,4700160,4,0};
    assert(Surface::valid(tail));
    for(auto descriptor:{tail.allocationBytes,pageSpanBytes(tail.allocationBytes,16384)}) {
        for(auto backing:{descriptor,pageSpanBytes(descriptor,16384)})
            assert(Surface::resourceLayout(tail,backing,descriptor-tail.offset,16384));
        assert(!Surface::resourceLayout(tail,descriptor-1,descriptor-tail.offset,16384));
    }
    assert(!Surface::resourceLayout(tail,4700224,4700159,16384));
    assert(!Surface::resourceLayout(tail,4700224,UINT64_MAX,16384));
    assert(!Surface::resourceLayout(tail,pageSpanBytes(tail.allocationBytes,16384)+4096,4700160,16384));
    --tail.allocationBytes;assert(!Surface::valid(tail));
    assert(MappingBounds::validView(0x100000,4700224,0x100040,4700160));
    assert(!MappingBounds::validView(0x100000,4700224,0x100040,4700161));
    MappingBounds::RootSelection tailRoot{0x100000,4700224,true,false};unsigned tailPages=0;
    assert(MappingBounds::visitSelected<256>(nullptr,tailRoot,[&](MappingBounds::PageSpan part){
        assert(part.first==0x100000 && part.end==0x100000+pageSpanBytes(4700224));tailPages=unsigned((part.end-part.first)/4096);return true;
    }) && tailPages==pageSpanBytes(4700224)/4096);
    for(uint32_t format:{Surface::R8,Surface::RG8,Surface::LA08}) {
        auto mask=surface;mask.pixelFormat=format;mask.bytesPerElement=Surface::elementBytes(format);
        mask.width=13;mask.height=7;mask.rowBytes=64;mask.offset=0;mask.imageBytes=448;mask.allocationBytes=4096;
        assert(Surface::valid(mask));mask.bytesPerElement=4;assert(!Surface::valid(mask));
    }
    Surface::Info ga{Surface::Version,Surface::Linear,9,0,768,48,1536,Surface::LA08,0,73728,73728,2,0};
    assert(Surface::valid(ga) && Surface::compatibleColorPixelFormat(ga.pixelFormat,2));
    assert(!Surface::compatibleColorPixelFormat(ga.pixelFormat,1) && !Surface::compatibleColorPixelFormat(ga.pixelFormat,4));
    assert(Surface::compatibleColorPixelFormat(Surface::RG8,2) && Surface::compatibleColorPixelFormat(Surface::R8,1));
    assert(Surface::compatibleColorPixelFormat(Surface::RGBA,4) && Surface::compatibleColorPixelFormat(Surface::BGRA,4,true));
    assert(!Surface::compatibleColorPixelFormat(Surface::BGRA,4) && !Surface::compatibleColorPixelFormat(Surface::RGBA,4,true));
    assert(!Surface::compatibleColorPixelFormat(0,0) && !Surface::compatibleColorPixelFormat(Surface::LA08,8));
    assert(stridedRange(image,0,1920*4,pitch,1080));
    assert(!stridedRange(image-257,0,1920*4,pitch,1080));
    assert(!stridedRange(image,UINT64_MAX,1920*4,pitch,1080));
    assert(!stridedRange(image,0,1920*4,UINT64_MAX,1080));
    assert(Native::surfaceHandle(7,2)==(uint64_t(2)<<32|7));
    assert(Native::validClientBuffer(0x100004000,16384,16384));
    assert(Native::validClientBuffer(0x100001000,4096,4096));
    assert(!Native::validClientBuffer(0,16384,16384) && !Native::validClientBuffer(0x100004000,0,16384));
    assert(!Native::validClientBuffer(0x100001000,16384,16384) && !Native::validClientBuffer(0x100004000,4096,16384));
    assert(!Native::validClientBuffer(UINT64_MAX-16383,16384,16384));
    assert(!Native::validClientBuffer(0x100004000,16384,12288));
    auto bad=surface;bad.offset=allocation-image+64;assert(!Surface::valid(bad));
    bad=surface;bad.rowBytes=7664;assert(!Surface::valid(bad));
    bad=surface;bad.pixelFormat=0x34323076;assert(!Surface::valid(bad));
    bad=surface;bad.plane=1;assert(!Surface::valid(bad));
    Shader::Texture texture{1,0,1920,1080,pitch,Shader::BGRA8};uint64_t bytes=0;
    assert(Shader::textureSize(texture,bytes) && bytes==image);
    auto tooWide=texture;tooWide.width=1;tooWide.height=1;tooWide.pitch=0x400000;
    assert(!Shader::textureSize(tooWide,bytes));
    auto state=Shader::defaultRenderState(1920,1080);float x,y;
    memcpy(&x,&state.viewport[1],4);memcpy(&y,&state.viewport[2],4);
    assert(x==960 && y==540);
    BackingPage a[]={{0x90000000,0},{0x30000000,4096},{0x70000000,8192}};
    BackingPage b[]={{0x70000000,0},{0xd0000000,4096}};
    assert(sortBackingPages(a,3) && sortBackingPages(b,2));
    assert(backingOverlap(a,3,8192+64,8192+128,b,2,64,128));
    assert(!backingOverlap(a,3,8192+64,8192+128,b,2,128,192));
    assert(!backingOverlap(a,3,64,128,b,2,64,128));
    assert(backingOverlap(a,3,8192-64,8192+64,b,2,0,32));
    assert(!backingOverlap(a,3,8192-64,8192,b,2,0,32));
    {
        BackingPage left[]={{0x100000,4096},{0x200000,0},{0x300000,8192},{0x400000,12288}};
        BackingPage right[]={{0x100000,12288},{0x200000,8192},{0x300000,4096},{0x400000,0}};
        StridedRange slices{256,4000,4096,3},gap{160,96,4096,4},edge{159,97,4096,4};
        assert(validStridedRange(slices,16384) && validStridedRange(gap,16384));
        assert(!backingOverlap(left,4,slices,right,4,gap) && backingOverlap(left,4,slices,right,4,edge));
        assert(!stridedOverlap(slices,gap) && stridedOverlap(slices,edge));
        assert(!validStridedRange({UINT64_MAX,4,4096,2},16384) && !validStridedRange({0,4,UINT64_MAX,2},16384));
        unsigned cases=0;
        for(uint64_t first:{0u,63u,256u,4095u})for(uint64_t width:{1u,64u,512u,4096u}) {
            StridedRange x{first,width,8192,2};if(!validStridedRange(x,16384))continue;
            bool expected=false;
            for(auto p:left)for(auto q:right)if(p.physical==q.physical)
                for(uint64_t i=0;i<x.count;++i)for(uint64_t j=0;j<slices.count;++j) {
                    uint64_t begin=x.begin+i*x.stride,end=begin+x.bytes;
                    uint64_t other=slices.begin+j*slices.stride,otherEnd=other+slices.bytes;
                    uint64_t lo=begin>p.offset?begin-p.offset:0,hi=end>p.offset?end-p.offset:0;
                    uint64_t qlo=other>q.offset?other-q.offset:0,qhi=otherEnd>q.offset?otherEnd-q.offset:0;
                    hi=std::min(hi,uint64_t(PageBytes));qhi=std::min(qhi,uint64_t(PageBytes));
                    if(lo<hi && qlo<qhi && lo<qhi && qlo<hi)expected=true;
                }
            assert(backingOverlap(left,4,x,right,4,slices)==expected);++cases;
        }
        assert(cases==16);
    }
    // Sparse full-size texture versus one imported page. Physical order and
    // descriptor offsets intentionally differ; skipping must preserve byte edges.
    {
        std::vector<BackingPage> sparse(MaxBufferBytes/PageBytes);
        for(unsigned i=0;i<sparse.size();++i)sparse[i]={0x80000000+uint64_t(i)*2*PageBytes,(sparse.size()-1-i)*PageBytes};
        for(unsigned i:{0u,1u,7u,255u,2047u,4095u}) {
            BackingPage probe{sparse[i].physical,0};const auto offset=sparse[i].offset;
            assert(backingOverlap(sparse.data(),sparse.size(),offset+32,offset+64,&probe,1,32,64));
            assert(backingOverlap(&probe,1,32,64,sparse.data(),sparse.size(),offset+32,offset+64));
            assert(!backingOverlap(sparse.data(),sparse.size(),offset+32,offset+64,&probe,1,64,96));
            assert(backingOverlap(sparse.data(),sparse.size(),{offset+32,32,0,1},&probe,1,{32,32,0,1}));
            assert(!backingOverlap(sparse.data(),sparse.size(),{offset+32,32,0,1},&probe,1,{64,32,0,1}));
            probe.physical+=PageBytes;
            assert(!backingOverlap(sparse.data(),sparse.size(),0,MaxBufferBytes,&probe,1,0,PageBytes));
            assert(!backingOverlap(&probe,1,{0,PageBytes,0,1},sparse.data(),sparse.size(),{0,MaxBufferBytes,0,1}));
        }
    }
    BackingPage duplicate[]={{0x70000000,0},{0x70000000,4096}};
    assert(!sortBackingPages(duplicate,2));
    std::vector<BackingPage> maximum(MaxBufferBytes/PageBytes);
    for (unsigned i=0;i<maximum.size();++i) maximum[i]={0x80000000+(maximum.size()-i)*PageBytes,uint64_t(i)*PageBytes};
    assert(sortBackingPages(maximum.data(),maximum.size()));
    for (unsigned i=1;i<maximum.size();++i) assert(maximum[i-1].physical<maximum[i].physical);
}
static void binaryArchives() {
    namespace Archive=GPU::MetalArchive;
    Archive::Contents contents,decoded;Archive::Key context{},digest{};context[0]=0x35;digest[0]=0x71;
    const GPU::Shader::Program *compute=nullptr;
    for(const auto &program:GPUPrograms::Programs)if(!std::strcmp(program.name,"add")){compute=&program;break;}
    assert(compute && compute->stage==GPU::Shader::Compute);
    Archive::Function aot;aot.key[0]=1;aot.kind=Archive::AOT;aot.aot=compute->id;aot.stage=compute->stage;aot.maxThreads=64;aot.name=compute->name;
    contents.functions.push_back(aot);Archive::Pipeline pipeline;pipeline.key[0]=2;pipeline.kind=Archive::Compute;pipeline.functions[0]=aot.key;contents.pipelines.push_back(pipeline);
    // A real compiled program's pointer-free representation exercises the
    // production JIT image validator; this fixture never registers AOT as JIT.
    auto image=std::make_unique<QGPUProgramImage>();image->registry=GPUPrograms::Registry;image->version=GPU::JIT::Version;image->gpu=540;
    std::memcpy(&image->info,compute,sizeof(image->info));image->info.id=0;
    std::memcpy(image->code,compute->code,compute->codeWords*4);std::memcpy(image->immediates,compute->immediates,compute->immediateCount*4);
    std::strcpy(image->name,compute->name);assert(GPU::JIT::valid(*image,GPUPrograms::Registry));
    auto jit=aot;jit.key[0]=3;jit.kind=Archive::Compiled;jit.aot=0;
    const auto *bytes=reinterpret_cast<const uint8_t *>(image.get());jit.image.assign(bytes,bytes+sizeof(*image));contents.functions.push_back(jit);
    auto second=pipeline;second.key[0]=4;second.functions[0]=jit.key;contents.pipelines.push_back(second);
    std::vector<uint8_t> encoded;assert(Archive::encode(contents,context,GPUPrograms::Registry,encoded));
    // SHA-256 is supplied by CommonCrypto in the actual Metal file path. This
    // host check verifies the codec's digest seam, schema and payload bounds.
    std::memcpy(encoded.data()+Archive::DigestOffset,digest.data(),digest.size());
    assert(Archive::decode(encoded.data(),encoded.size(),context,GPUPrograms::Registry,digest,decoded));
    assert(decoded.functions.size()==2 && decoded.pipelines.size()==2 && decoded.functions[1].image==jit.image);
    auto wrong=context;wrong[1]=1;assert(!Archive::decode(encoded.data(),encoded.size(),wrong,GPUPrograms::Registry,digest,decoded) && decoded.functions.empty());
    wrong=digest;wrong[1]=1;assert(!Archive::decode(encoded.data(),encoded.size(),context,GPUPrograms::Registry,wrong,decoded));
    assert(!Archive::decode(encoded.data(),encoded.size(),context,GPUPrograms::Registry^1,digest,decoded));
    assert(!Archive::decode(encoded.data(),encoded.size()-1,context,GPUPrograms::Registry,digest,decoded));
    auto malformed=encoded;malformed[24]^=1;assert(!Archive::decode(malformed.data(),malformed.size(),context,GPUPrograms::Registry,digest,decoded));
    auto invalid=contents;invalid.functions.push_back(aot);assert(!Archive::encode(invalid,context,GPUPrograms::Registry,malformed));
    invalid=contents;invalid.pipelines[0].functions[0][1]=1;assert(!Archive::encode(invalid,context,GPUPrograms::Registry,malformed));
    invalid=contents;invalid.functions[1].image.pop_back();assert(!Archive::encode(invalid,context,GPUPrograms::Registry,malformed));
    invalid=contents;invalid.pipelines[0].kind=Archive::Render;assert(!Archive::encode(invalid,context,GPUPrograms::Registry,malformed));
}

int main() {
    {
        uint32_t low=0,high=0;
        const double hdr[]={0.5,-2.0,4.0,1.0};
        GPU::MetalColor::packRGBA16Float(hdr,low,high);
        assert(low==0xc0003800u && high==0x3c004400u);
        const double edges[]={-0.0,0x1p-24,0x1p-25,1.0+0x1p-11};
        GPU::MetalColor::packRGBA16Float(edges,low,high);
        assert(low==0x00018000u && high==0x3c000000u);
        assert(GPU::Shader::renderableColorFormat(GPU::Shader::RGBA16Float) &&
            !GPU::Shader::sampledOnlyFormat(GPU::Shader::RGBA16Float));
    }
    {
        using GPU::MetalBatch::Plan;
        GPU::Shader::Dispatch dispatch{};dispatch.buffers[0].resource=1;dispatch.buffers[1].resource=2;
        Plan repeated(GPU::Native::MaxShmemBytes);
        for(unsigned i=0;i<GPU::Native::MaxBatchCommands;++i)assert(repeated.append(GPU::ShaderDispatch,&dispatch,sizeof(dispatch)));
        assert(repeated.resources()==2 && repeated.commands()==GPU::Native::MaxBatchCommands);
        const auto fullBytes=repeated.bytes();assert(!repeated.append(GPU::ShaderDispatch,&dispatch,sizeof(dispatch)) && repeated.bytes()==fullBytes);
        const auto exact=GPU::Native::commandOffset(64)+sizeof(GPU::Command)+sizeof(dispatch);
        Plan exactFit(exact,64),shortFit(exact-1,64),badTasks(GPU::Native::MaxShmemBytes,65);
        assert(exactFit.append(GPU::ShaderDispatch,&dispatch,sizeof(dispatch)) && exactFit.bytes()==exact);
        assert(!shortFit.append(GPU::ShaderDispatch,&dispatch,sizeof(dispatch)) && !shortFit.commands());
        assert(!badTasks.append(GPU::ShaderDispatch,&dispatch,sizeof(dispatch)));
        Plan limited(GPU::Native::MaxShmemBytes);
        unsigned id=1;
        for(unsigned i=0;i<4;++i) {
            dispatch={};
            for(auto &b:dispatch.buffers)b.resource=id<64?id++:0;
            for(auto &t:dispatch.images)t.resource=id<64?id++:0;
            assert(limited.append(GPU::ShaderDispatch,&dispatch,sizeof(dispatch)));
        }
        assert(id==64 && limited.resources()==63 && limited.commands()==4);
        dispatch={};dispatch.buffers[0].resource=64;dispatch.images[0].resource=65;
        assert(!limited.append(GPU::ShaderDispatch,&dispatch,sizeof(dispatch)) && limited.resources()==63 && limited.commands()==4);
        dispatch.images[0].resource=1;
        assert(limited.append(GPU::ShaderDispatch,&dispatch,sizeof(dispatch)) && limited.resources()==64);
        dispatch.images[0].resource=65;assert(!limited.append(GPU::ShaderDispatch,&dispatch,sizeof(dispatch)) && limited.resources()==64);
        GPU::Shader::Draw draw{};draw.indices.resource=1;draw.indexSize=4;draw.indices.bytes=GPU::Native::IndexSnapshotLimit;
        Plan indices(GPU::Native::MaxShmemBytes);
        assert(indices.append(GPU::ShaderDraw,&draw,sizeof(draw)) && indices.indexBytes()==GPU::Native::IndexSnapshotLimit);
        draw.indices.bytes=1;assert(!indices.append(GPU::ShaderDraw,&draw,sizeof(draw)) && indices.commands()==1);
        // Small draws share a page; a failed append must not move the next
        // draw's offset or consume the remaining snapshot budget.
        uint64_t cursor=0;uint32_t first=99,second=99;
        assert(GPU::Native::appendIndexSnapshot(6,cursor,first) && first==0 && cursor==16);
        assert(GPU::Native::appendIndexSnapshot(12,cursor,second) && second==16 && cursor==32);
        assert(GPU::Native::indexSnapshotAllocation(cursor)==GPU::PageBytes);
        assert(!GPU::Native::appendIndexSnapshot(GPU::Native::IndexSnapshotLimit,cursor,second) && cursor==32);
        Plan packed(GPU::Native::MaxShmemBytes);draw.indices.bytes=12;
        const unsigned packedDraws=GPU::Native::MaxBatchCommands;
        static_assert(GPU::Native::commandOffset(GPU::Native::MaxResponsibleTaskIDs)+
            GPU::Native::MaxBatchCommands*(sizeof(GPU::Command)+sizeof(GPU::Shader::Draw))<=GPU::Native::MaxShmemBytes,
            "native upload must fit the complete draw command budget");
        for(unsigned i=0;i<packedDraws;++i)assert(packed.append(GPU::ShaderDraw,&draw,sizeof(draw)));
        assert(!packed.append(GPU::ShaderDraw,&draw,sizeof(draw)) && packed.commands()==packedDraws && packed.indexBytes()==GPU::PageBytes);
        // All attachment and sampled-image ID fields contribute, including
        // distinct view IDs that happen to alias the same backing pages.
        draw={};draw.target.resource=1;draw.depth.resource=2;draw.stencil.resource=3;draw.indices.resource=4;
        draw.colorTargets[0].resource=5;draw.vertexTextures[0].resource=6;draw.textures[0].resource=7;
        draw.buffers[0].resource=8;draw.fragmentBuffers[0].resource=9;
        Plan fields(GPU::Native::MaxShmemBytes);assert(fields.append(GPU::ShaderDraw,&draw,sizeof(draw)) && fields.resources()==9);
    }
    {
        using namespace GPU::Native;
        const uint32_t offset=commandOffset(2),start=offset+2*sizeof(GPU::Command);
        GPU::Command commands[]={{GPU::ShaderDraw,0,0,start,sizeof(GPU::Shader::Draw),0},
            {GPU::ShaderDispatch,0,0,start+sizeof(GPU::Shader::Draw),sizeof(GPU::Shader::Dispatch),0}};
        const uint32_t bytes=start+sizeof(GPU::Shader::Draw)+sizeof(GPU::Shader::Dispatch);uint32_t end=0;
        assert(shaderBatchLayout(commands,2,offset,bytes,sizeof(GPU::Shader::Dispatch),sizeof(GPU::Shader::Draw),end) && end==bytes);
        assert(!shaderBatchLayout(commands,2,offset,bytes-1,sizeof(GPU::Shader::Dispatch),sizeof(GPU::Shader::Draw),end));
        auto bad=commands[1];commands[1].offset=commands[0].offset; // overlapping client payloads cannot alter the snapshot layout
        assert(!shaderBatchLayout(commands,2,offset,bytes,sizeof(GPU::Shader::Dispatch),sizeof(GPU::Shader::Draw),end));
        commands[1]=bad;commands[1].offset=UINT64_MAX;
        assert(!shaderBatchLayout(commands,2,offset,bytes,sizeof(GPU::Shader::Dispatch),sizeof(GPU::Shader::Draw),end));
        commands[1]=bad;commands[1].opcode=GPU::Barrier;
        assert(!shaderBatchLayout(commands,2,offset,bytes,sizeof(GPU::Shader::Dispatch),sizeof(GPU::Shader::Draw),end));
    }
    binaryArchives();
    {
        using namespace GPU::Native::Memory;
        auto limits=limitsForAperture(256ULL*1024*1024);
        assert(validLimits(limits) && limits.clientBytes==128ULL*1024*1024 && limits.globalBytes==192ULL*1024*1024);
        assert(!validLimits(limitsForAperture(0)) && !validLimits(limitsForAperture(UINT64_MAX)));
        auto wrong=limits;wrong.resourcesPerJob=32;assert(!validLimits(wrong));
        ResourceTable<> table;
        for(uint32_t i=1;i<=ResourcesPerClient;++i)assert(table.add(i,16384,limits.clientBytes));
        assert(table.count()==1024 && table.bytes()==1024*16384 && !table.add(2048,16384,limits.clientBytes));
        assert(table.remove(512) && !table.remove(512) && !table.add(1,16384,limits.clientBytes));
        assert(table.add(2048,16384,limits.clientBytes) && table.count()==1024);
        Usage usage;for(unsigned i=0;i<GPU::MaxClients;++i)assert(usage.addClient());assert(!usage.addClient());
        for(unsigned i=0;i<GlobalResources;++i)assert(usage.add(16384,limits));
        assert(!usage.add(16384,limits));for(unsigned i=0;i<GlobalResources;++i)assert(usage.remove(16384));
        assert(!usage.remove(16384) && !usage.bytes && !usage.resources);
        assert(usage.add(limits.resourceBytes,limits) && !usage.remove(limits.resourceBytes+1));
        assert(writeBit(31)==0x80000000ULL && writeBit(63)==0x8000000000000000ULL && !writeBit(64));
    }
    {
        using namespace GPU::Native;using Observation=CompletionWindow::Observation;
        CompletionWindow stream;assert(stream.admit(1,2) && stream.admit(3,4));
        // Real Family start notifications include both CPU admission times.
        assert(stream.observe({3,11,12,0,0,0})==Observation::Start && stream.pending()==2 && !stream.retired());
        assert(stream.observe({1,10,0,0,0,0})==Observation::Start);
        assert(stream.observe({1,10,13,0,0,0})==Observation::Start && stream.pending()==2 && !stream.retired());
        assert(stream.observe({2,10,20,0,0,0})==Observation::Retired && stream.retired()==2 && stream.pending()==1);
        assert(stream.observe({4,11,21,0,0,0})==Observation::Retired && stream.retired()==4 && !stream.pending());
        assert(!stream.admit(3,4) && stream.admit(5,6));
        assert(stream.observe({6,30,29,0,0,0})==Observation::Malformed && stream.retired()==4 && stream.pending()==1);
        assert(stream.abort()==1 && !stream.admit(7,8));
        CompletionWindow future;assert(future.admit(1,2) && future.admit(3,4));
        assert(future.observe({4,1,2,0,0,0})==Observation::Malformed && !future.retired());
        CompletionWindow failed;assert(failed.admit(1,2));
        assert(failed.observe({2,0,0,8,0,0})==Observation::Failed && !failed.retired());
        CompletionWindow full;
        for(uint32_t i=0;i<GPU::MaxJobs;++i)assert(full.admit(2*i+1,2*i+2));
        assert(full.full() && !full.admit(33,34));
        for(uint32_t i=0;i<GPU::MaxJobs;++i)assert(full.observe({2*i+2,1,2,0,0,0})==Observation::Retired);
        assert(full.admit(33,34) && !full.full());
        assert(admissionBlocked(GPU::MaxJobs,false,false,0,0) && admissionBlocked(1,true,false,0,0));
        assert(admissionBlocked(1,false,true,4096,0) && !admissionBlocked(1,false,false,4096,65536));
        assert(!admissionBlocked(0,true,true,0,0));
        const AdmissionMemory request{8*1024*1024,16*1024*1024,4096},live{16*1024*1024,32*1024*1024,8192};
        assert(admissionBlocked(2,true,true,live.snapshotBytes,live.indirectBytes));
        assert(!pipelinedAdmissionBlocked(2,false,request,live));
        assert(pipelinedAdmissionBlocked(GPU::MaxJobs,false,request,live));
        assert(pipelinedAdmissionBlocked(2,true,request,live));
        auto used=live;used.snapshotBytes=FramebufferSnapshotLimit-request.snapshotBytes;
        assert(!pipelinedAdmissionBlocked(2,false,request,used));
        ++used.snapshotBytes;assert(pipelinedAdmissionBlocked(2,false,request,used));
        used=live;used.indirectBytes=FramebufferIndirectLimit-request.indirectBytes+1;
        assert(pipelinedAdmissionBlocked(2,false,request,used));
        used=live;used.indexBytes=IndexSnapshotLimit-request.indexBytes+1;
        assert(pipelinedAdmissionBlocked(2,false,request,used));
        used=live;used.snapshotBytes=UINT64_MAX;
        assert(pipelinedAdmissionBlocked(2,false,request,used));
        auto huge=request;huge.indirectBytes=UINT64_MAX;
        assert(pipelinedAdmissionBlocked(2,false,huge,live));
        CPUReadHazards sharedReaders(3,1);sharedReaders.use(true,false);sharedReaders.use(true,false);
        assert(!sharedReaders.blocked()); // Two earlier readers plus this job's retained reference.
        CPUReadHazards writer(2,1);writer.use(true,true);assert(writer.blocked());
        CPUReadHazards foreignAlias(0,0);foreignAlias.use(false,true);assert(foreignAlias.blocked());
        CPUReadHazards retired(0,0);assert(!retired.blocked());
        CPUReadHazards unknown(3,1);unknown.use(true,false);assert(unknown.blocked());
        CPUReadHazards invalid(0,1);assert(invalid.blocked());
        CPUReadHazards overflow(UINT32_MAX,UINT32_MAX);overflow.use(true,false);assert(overflow.blocked());
        // A view's index bytes are disjoint from the writer, but its cache
        // operation flushes the whole root. A second descriptor aliases the
        // root's first physical page and must still delay that CPU operation.
        const GPU::BackingPage readRoot[]={{0x100000,0},{0x200000,4096}};
        const GPU::BackingPage producerRoot[]={{0x100000,0}};
        assert(!GPU::backingOverlap(readRoot,2,4096,8192,producerRoot,1,0,4096));
        const bool physicalWriter=GPU::backingOverlap(readRoot,2,0,8192,producerRoot,1,0,4096);
        CPUReadHazards cacheFlush(1,1);cacheFlush.use(false,physicalWriter);assert(cacheFlush.blocked());
        // Deferral is before tokens; retry after real coherent retirement can
        // use exactly the same token pair, never a synthetic completion.
        CompletionWindow producer;assert(producer.admit(1,2));
        CompletionWindow consumer;
        assert(pipelinedAdmissionBlocked(1,foreignAlias.blocked(),request,{}) && !consumer.issued());
        assert(producer.observe({2,10,20,0,0,0})==Observation::Retired);
        assert(!pipelinedAdmissionBlocked(0,retired.blocked(),request,{}) && consumer.admit(1,2));
        assert(consumer.observe({1,21,0,0,0,0})==Observation::Start && !consumer.retired());
        assert(consumer.observe({2,21,30,0,0,0})==Observation::Retired);
        CompletionWindow sliding;
        for(uint32_t i=0;i<GPU::MaxJobs;++i)assert(sliding.admit(2*i+1,2*i+2));
        assert(sliding.full() && !sliding.admit(33,34));
        assert(sliding.observe({2,10,20,0,0,0})==Observation::Retired && sliding.pending()==GPU::MaxJobs-1);
        // One real retirement opens one slot; the other15 jobs need not drain.
        assert(!pipelinedAdmissionBlocked(sliding.pending(),false,request,live));
        assert(sliding.admit(33,34) && sliding.full());
        for(uint32_t i=1;i<GPU::MaxJobs;++i)assert(sliding.observe({2*i+2,21,30,0,0,0})==Observation::Retired);
        assert(sliding.observe({34,31,40,0,0,0})==Observation::Retired && !sliding.pending());
    }
    {
        using namespace GPU::Diagnostics;
        Cache<int *,2> cache;int a=0,b=0,c=0;
        assert(!cache.pop() && !cache.push(nullptr));
        assert(cache.push(&a) && cache.push(&b) && !cache.push(&c) && cache.count()==2);
        assert(cache.pop()==&b && cache.pop()==&a && !cache.count());
        struct Sized {uint64_t length;};Sized small{4},large{8},oversize{13};
        SizedCache<Sized *,2,12> sized;
        assert(!sized.take(4) && !sized.pop() && !sized.push(nullptr) && !sized.push(&oversize));
        assert(sized.push(&small) && sized.push(&large) && sized.bytes()==12 && !sized.push(&small));
        assert(!sized.take(5) && sized.bytes()==12 && sized.count()==2);
        assert(sized.take(4)==&small && sized.bytes()==8 && sized.count()==1);
        assert(!sized.take(4) && sized.push(&small) && sized.pop()==&large && sized.bytes()==4);
        assert(sized.take(4)==&small && !sized.count() && !sized.bytes());
        Timing timing;assert(timing.sample(10,19) && !timing.sample(19,10));
        assert(timing.samples==1 && timing.lastTicks==9 && timing.maxTicks==9 && timing.invalidDeltas==1);
        timing.totalTicks=UINT64_MAX-1;assert(timing.sample(0,3) && timing.totalTicks==UINT64_MAX);
        CoreClock core;
        assert(core.sample(19200,596000) && core.samples==1 && core.minHz==596000000 && core.maxHz==596000000);
        assert(core.sample(38400,596000) && core.samples==2 && core.minHz==298000000 && core.maxHz==596000000);
        assert(!core.sample(0,1) && !core.sample(1,0) && !core.sample(1,UINT64_MAX) && core.invalid==3);
        const auto oldCycles=core.cycles;core.alwaysonTicks=UINT64_MAX;
        assert(!core.sample(1,1) && core.cycles==oldCycles && core.invalid==4);
        CoreClock idleBusy;assert(idleBusy.sample(19200,0,true) && idleBusy.samples==1 && !idleBusy.cycles && !idleBusy.invalid);
        Kinds kinds;kinds.post(Marker);kinds.finish(Marker,true);
        kinds.post(Marker|Transfer|Compute);kinds.finish(Transfer|Compute,false);
        assert(kinds.submitted[0]==1 && kinds.completed[0]==1 && !kinds.failed[0]);
        assert(kinds.submitted[1]==1 && kinds.submitted[2]==1 && kinds.failed[1]==1 && kinds.failed[2]==1);
        assert(commandKind(GPU::ShaderDraw)==Render && normalizeKinds(Marker|Render)==Render);
        Metrics native,compositor;uint64_t ticks=0;auto clock=[&](){return ticks+=5;};
        {AdmissionTrace<decltype(clock)> trace(native,clock);trace.compositor(&compositor);
            trace.sample(PreflightRetire,1,4);trace.mark(IBAcquire);
            assert(!trace.finish(0) && !trace.finish(0));}
        assert(native.accepted==1 && !native.rejected && compositor.accepted==1);
        assert(native.timing[IBAcquire].lastTicks==5 && native.timing[AdmissionTotal].lastTicks==10);
        assert(native.timing[PreflightRetire].lastTicks==3 && compositor.timing[PreflightRetire].lastTicks==3);
        {AdmissionTrace<decltype(clock)> trace(native,clock);assert(trace.finish(7)==7);}
        assert(native.rejected==1 && native.lastResult==7 && !compositor.rejected);
    }
    {
        MetalSync::Point first,later;
        assert(!first.finish(0) && first.pending()); // no success without a native retirement
        assert(later.finish(0,7,4) && !later.pending() && first.pending());
        auto read=[&](uint64_t i){const auto &point=i?later:first;return MetalSync::Observation{point.pending(),point.error()};};
        assert(MetalSync::dependencies(2,read).state==MetalSync::DependencyState::Pending);
        assert(first.finish(0,7,2) && first.queue()==7 && first.token()==2);
        assert(MetalSync::dependencies(2,read).state==MetalSync::DependencyState::Ready);
        assert(!first.finish(0xe00002eb) && !first.error()); // a later pass failure cannot undo a retired update
        MetalSync::Point failed;assert(failed.finish(0xe00002eb) && failed.error()==0xe00002eb);
        auto error=MetalSync::dependencies(2,[&](uint64_t i){const auto &point=i?later:failed;return MetalSync::Observation{point.pending(),point.error()};});
        assert(error.state==MetalSync::DependencyState::Failed && error.error==0xe00002eb);
        assert(!failed.finish(0,7,6));
        assert(MetalSync::renderStages(3) && !MetalSync::renderStages(4) && !MetalSync::renderStages(0));
        assert(MetalSync::barrierScope(3) && !MetalSync::barrierScope(4));
    }
    bridgeSubmission();
    // Native priority changes execution order across queues, never FIFO
    // within one queue. Equal numeric handles from different owners differ.
    int ownerA=0,ownerB=0;
    Adreno::AdmissionCandidate admissions[]={
        {&ownerA,7,2,true,false},{&ownerB,7,0,true,false},{&ownerA,7,5,true,false}};
    auto view=[&](unsigned i){return admissions[i];};
    assert(Adreno::nextAdmission(3,view)==1);
    admissions[1].complete=true;
    assert(Adreno::nextAdmission(3,view)==0); // same-queue realtime cannot overtake
    admissions[0].complete=true;
    assert(Adreno::nextAdmission(3,view)==2);
    admissions[2].complete=true;assert(Adreno::nextAdmission(3,view)==3);
    admissions[0]={&ownerA,7,1,true,false};admissions[1]={&ownerB,7,1,true,false};
    assert(Adreno::nextAdmission(3,view)==0);
    admissions[1].priority=5;assert(Adreno::nextAdmission(3,view)==1);
    admissions[0].priority=4;admissions[1].priority=0;
    assert(Adreno::nextAdmission(2,view)==1); // native high precedes class4
    admissions[0].priority=0;admissions[1].priority=5;
    assert(Adreno::nextAdmission(2,view)==0); // native keys0/5 tie stably
    admissions[1].ready=false;assert(Adreno::nextAdmission(3,view)==0);
    admissions[0]={&ownerA,7,1,false,false,true};admissions[1]={&ownerA,7,1,true,false,false};
    assert(Adreno::nextAdmission(2,view)==2 && Adreno::nextAdmission(2,view,true)==1);
    admissions[0].issued=false;assert(Adreno::nextAdmission(2,view,true)==2);
    {
        // Different descriptors alias the same physical page. A narrow view
        // is insufficient because cache maintenance covers its whole root.
        struct Use {GPU::BackingPage pages[2];unsigned count;bool writer;};
        Use uses[]={{{{0x100000,0},{0x200000,4096}},2,false},{{{0x100000,0}},1,false},{{{0x300000,0}},1,true}};
        Adreno::AdmissionCandidate jobs[3]{};
        auto read=[&](unsigned i){return jobs[i];};
        auto conflict=[&](unsigned a,unsigned b){return (uses[a].writer || uses[b].writer) &&
            GPU::backingOverlap(uses[a].pages,uses[a].count,0,uses[a].count*4096,uses[b].pages,uses[b].count,0,uses[b].count*4096);};
        jobs[0].issued=true;
        assert(!Adreno::dispatchDependencyBlocked(1,3,read,conflict)); // sharedread/read
        uses[1].writer=true;assert(Adreno::dispatchDependencyBlocked(1,3,read,conflict)); // writeafterread
        jobs[0].complete=true;assert(Adreno::dispatchDependencyBlocked(1,3,read,conflict)); // CPdone != coherentretired
        jobs[0].issued=false;assert(Adreno::dispatchDependencyBlocked(1,3,read,conflict)); // earlier unissued still owns order
        assert(!Adreno::dispatchDependencyBlocked(0,3,read,conflict)); // later unissued cannot deadlock predecessor
        jobs[1].issued=true;assert(Adreno::dispatchDependencyBlocked(0,3,read,conflict)); // unsafe active alias fails closed
        assert(!Adreno::dispatchDependencyBlocked(2,3,read,conflict)); // disjoint physical writer
        uses[0].writer=true;uses[1].writer=false;
        assert(Adreno::dispatchDependencyBlocked(1,3,read,conflict)); // readafterwrite
    }
    for(uint32_t priority:{0u,1u,2u,4u,5u})for(uint32_t background:{2u,3u})
        assert(GPU::Native::validQueuePriority({priority,background,0}));
    assert(!GPU::Native::validQueuePriority({3,2,0}) && !GPU::Native::validQueuePriority({6,2,0}) &&
        !GPU::Native::validQueuePriority({0,1,0}) && !GPU::Native::validQueuePriority({0,2,1}));
    GPU::Native::Timeline timeline;uint32_t first=0,second=0;
    assert(timeline.reserve(first) && first==0x100 && timeline.reserve(second) && second==0x200);
    assert(!timeline.retire(second) && !timeline.retire(first+1));
    assert(timeline.retire(first) && !timeline.retire(first) && timeline.retire(second));
    while (!timeline.needsRebase()) {
        assert(timeline.reserve(first));assert(timeline.retire(first));
    }
    assert(!timeline.reserve(first));
    assert(!timeline.rebase(false,true,true) && !timeline.rebase(true,false,true) && !timeline.rebase(true,true,false));
    assert(timeline.rebase(true,true,true) && timeline.epoch()==1 && timeline.completed()==0 && timeline.next()==0x100);
    assert(timeline.reserve(first) && first==0x100 && timeline.retire(first));
    static_assert(sizeof(GPU::Native::Notification)==40,"native notification is not an admission receipt");
    nativeSurfaceAccessStates();surfaceBacking();uploadPagePool();
    secureFirmware();retainedMMU();retainedTranslationCursor();mappedWorkingSets();privateAddressSpaces();commands(); registerPackets();copyEncoding(); blitBatching(); submission(); executionProfiling(); invalidHandoffs(); corruptCompletion(); compactOperations();operationProfiling();pipelineEncoding();
    puts("GPU submit: actual PM4 encoder/MMIO engine, DMA execution, ring wrap/full, 16 retained per-job profiling cells, physical-alias dispatch order, hardware fences, private job page tables/context switches, faults, halt quarantine and native FIFO timeline/epoch passed");
}
