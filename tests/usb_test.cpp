// SPDX-License-Identifier: BSD-2-Clause
#include "DWC3Core.h"
#include "DWC3Control.h"
#include "DWC3Isochronous.h"
#include "USBRole.h"
#include "USBPlatform.h"
#include "../drivers/include/IOKit/usb/IOUSBDeviceControllerState.h"
#include <array>
#include <cassert>
#include <cstdio>
#include <vector>
#include <utility>
#include <string>

using USB::Result;
using USB::Role;
struct MMIO {
    std::array<uint32_t, DWC3::Aperture / 4> words{};
    std::vector<std::pair<uint32_t, uint32_t>> writes;
    unsigned elapsed = 0, barriers = 0, resetAt = 0, commandAt = 0, wakeAt = 0, expireStarts = 0;
    unsigned genericAt = 0, genericStatus = 0;
    uint32_t commandRegister = 0, commandStatus = 0, resourceIndex = 3;
    bool stuckHalt = false, stuckReset = false, stuckCommand = false, stuckWake = false;
    bool stuckGeneric = false;
    MMIO(Role role = Role::Device) {
        words[DWC3::GSNPSID / 4] = 0x5533280a;
        words[DWC3::GHWPARAMS0 / 4] = 40u << 24 | 2;
        words[DWC3::GHWPARAMS1 / 4] = 2u << 15;
        words[DWC3::GHWPARAMS3 / 4] = 1 | 1u << 2 | 16u << 12 | 8u << 18;
        words[DWC3::GCTL / 4] = role == Role::Host ? DWC3::PortHost : DWC3::PortDevice;
        words[DWC3::DSTS / 4] = DWC3::Halted;
        words[0] = 0x01000040;
        words[0x44 / 4] = 1;
    }
    uint32_t read(uint32_t offset) { assert(offset < DWC3::Aperture && !(offset & 3)); return words[offset / 4]; }
    void write(uint32_t offset, uint32_t value) {
        assert(offset < DWC3::Aperture && !(offset & 3));
        writes.emplace_back(offset, value);
        if (offset == DWC3::eventCount(0) || offset == DWC3::eventCount(1)) {
            assert(value <= words[offset / 4]); words[offset / 4] -= value; return;
        }
        words[offset / 4] = value;
        if (offset == DWC3::DCTL) {
            if (((value >> 5) & 15) == 8) wakeAt = elapsed + 20;
            if (value & DWC3::SoftReset) resetAt = elapsed + 8;
            if (value & DWC3::Run) words[DWC3::DSTS / 4] &= ~DWC3::Halted;
            else if (!stuckHalt) words[DWC3::DSTS / 4] |= DWC3::Halted;
        }
        if (offset == 0x40 && !(value & 1) && !stuckHalt) words[0x44 / 4] |= 1;
        if (offset == DWC3::DGCMD && (value & DWC3::CommandActive)) genericAt = elapsed + 3;
        if (offset >= DWC3::endpoint(0) && offset < DWC3::endpoint(32) &&
            (offset & 15) == 12 && (value & DWC3::CommandActive)) {
            commandAt = elapsed + 3; commandRegister = offset;
        }
    }
    void delay(unsigned us) {
        elapsed += us;
        if (wakeAt && elapsed >= wakeAt && !stuckWake) {
            words[DWC3::DSTS / 4] &= ~(15u << 18);
            words[DWC3::DCTL / 4] &= ~(15u << 5);
            wakeAt = 0;
        }
        if (resetAt && elapsed >= resetAt && !stuckReset) {
            words[DWC3::DCTL / 4] &= ~DWC3::SoftReset; resetAt = 0;
        }
        if (genericAt && elapsed >= genericAt && !stuckGeneric) {
            words[DWC3::DGCMD / 4] &= ~(DWC3::CommandActive | (15u << 12));
            words[DWC3::DGCMD / 4] |= genericStatus << 12;
            genericAt = 0;
        }
        if (commandAt && elapsed >= commandAt && !stuckCommand) {
            auto status=commandStatus;
            if (expireStarts && (words[commandRegister/4]&15)==unsigned(DWC3::Command::Start)) { --expireStarts;status=2; }
            words[commandRegister / 4] &= ~DWC3::CommandActive;
            words[commandRegister / 4] &= ~((15u<<12)|(0xffffu<<16));
            words[commandRegister / 4] |= status << 12 | resourceIndex << 16;
            commandAt = 0;
        }
    }
    void dmaReadBarrier() { ++barriers; }
    void dmaWriteBarrier() { ++barriers; }
};

static void platform() {
    USB::PlatformRequest request;
    assert(!USB::validPlatformRequest(request)); // sample needs an acquired lease
    request.action = USB::PlatformAction::Acquire;
    assert(USB::validPlatformRequest(request));
    assert(!USB::validPlatformResponse(request, USB::PlatformAction::Acquire, 0));
    request.lease = 7;
    assert(!USB::validPlatformRequest(request)); // acquire cannot recycle an old lease
    assert(USB::validPlatformResponse(request, USB::PlatformAction::Acquire, 0));
    request.action = USB::PlatformAction::Sample;
    assert(USB::validPlatformRequest(request));
    assert(USB::validPlatformResponse(request, USB::PlatformAction::Sample, 7));
    assert(!USB::validPlatformResponse(request, USB::PlatformAction::Sample, 8));
    assert(!USB::validPlatformResponse(request, USB::PlatformAction::Source, 7));
    request.size -= 1; assert(!USB::validPlatformRequest(request)); ++request.size;
    request.version = 1; assert(!USB::validPlatformRequest(request)); request.version = USB::PlatformVersion;
    request.version = 4; assert(!USB::validPlatformRequest(request)); request.version = USB::PlatformVersion;
    request.size = 72; assert(!USB::validPlatformRequest(request)); request.size = sizeof(request);
    request.padding[1] = 1; assert(!USB::validPlatformRequest(request)); request.padding[1] = 0;
    request.action = USB::PlatformAction(unsigned(USB::PlatformAction::Isolate)+1);
    assert(!USB::validPlatformRequest(request));
    request.action = USB::PlatformAction::SafePower; request.next = USB::Role(3);
    assert(!USB::validPlatformRequest(request));
    request.next = USB::Role::None; assert(USB::validPlatformRequest(request));
    USB::PlatformContract c{2, 31, 120000000, 19200000, 40, 4, 1000000, 0};
    assert(c.valid());
    for (unsigned bit = 0; bit < 5; ++bit) { auto b = c; b.flags &= ~(1u << bit); assert(!b.valid()); }
    auto b = c; b.flags = 255; assert(!b.valid());
    b = c; b.addressBits = 41; assert(!b.valid());
    b = c; b.referenceHz = 24000000; assert(!b.valid());
    b = c; b.maximumSpeed = 5; assert(!b.valid());
    b = c; b.version = 1; assert(!b.valid());
    b = c; b.reserved = 1; assert(!b.valid());
    for (unsigned budget : {0, 500000, 750000, 1000000, 1250000, 1500000}) {
        b = c; b.sourceMicroamps = budget; assert(b.valid());
    }
    for (unsigned budget : {1u, 499999u, 900000u, 1500001u, UINT32_MAX}) {
        b = c; b.sourceMicroamps = budget; assert(!b.valid());
    }
    b = c; b.flags |= 96; assert(b.valid());
    assert(USB::dmaRange(0xfffff000, 4096, 32, 4096));
    assert(!USB::dmaRange(0xfffff000, 4097, 32, 4096));
    assert(USB::dmaRange(UINT64_MAX - 3, 4, 64));
    assert(!USB::dmaRange(UINT64_MAX - 3, 5, 64));
    assert(!USB::dmaRange(0x1001, 16, 40, 64));
    assert(!USB::dmaRange(0x1000, 16, 40, 3));
    assert(!USB::dmaRange(0, 0, 40));
    assert(!USB::dmaRange(0, 4, 0));
    uint8_t raw[8] = {0x80, 6, 0, 1, 9, 4, 0x12, 0x34};
    auto setup = USB::SetupPacket::decode(raw);
    assert(setup.in() && setup.request == 6 && setup.value == 0x100 && setup.index == 0x409 && setup.length == 0x3412);
}

static void probeAndStop() {
    MMIO mmio; DWC3::Core<MMIO> core(mmio);
    assert(core.select(Role::Device) == Result::NotReady);
    assert(core.probe(DWC3::Aperture - 1, 40) == Result::Invalid);
    assert(core.probe(DWC3::Aperture, 40) == Result::Success);
    assert(core.probe(DWC3::Aperture, 40) == Result::Busy);
    assert(core.endpointCount() == 16 && core.dmaBits() == 40 && core.version() == 0x280a);
    assert(core.select(Role::Host) == Result::NotReady);
    assert(core.halt() == Result::Success && core.stopped());
    assert(core.select(Role::None) == Result::Invalid);
    assert(core.select(Role::Device) == Result::Success);
    assert(mmio.elapsed == 8 && !(mmio.words[DWC3::GUSB2PHYCFG / 4] & 0x140));
    assert(mmio.words[DWC3::GUCTL1 / 4] & 1u << 17);
    assert(core.installEvents(0x120000040, 256) == Result::Success);
    assert(mmio.words[DWC3::eventAddressHi(0) / 4] == 1);
    assert(core.installEvents(0x120000041, 256) == Result::Invalid);
    assert(core.installEvents(0x120000040, 257) == Result::Invalid);
    assert(core.installEvents(0x120000040, 65536) == Result::Invalid);
    assert(core.installEvents(1ull << 40, 256) == Result::Invalid);
    assert(core.runDevice(USB::Speed::High) == Result::NotReady);
    mmio.words[DWC3::DALEPENA / 4] = 3;
    assert(core.runDevice(USB::Speed::Super) == Result::Success && !core.stopped());
    assert((mmio.words[DWC3::DCFG / 4] & 7) == 4);
    assert(core.installEvents(0x1000, 256) == Result::NotReady);
    assert(core.setAddress(128) == Result::Invalid);
    assert(core.setAddress(127) == Result::Success);
    assert((mmio.words[DWC3::DCFG / 4] & (127u << 3)) == (127u << 3));
    mmio.stuckHalt = true;
    assert(core.halt() == Result::Timeout && !core.stopped());
    assert(core.select(Role::Host) == Result::NotReady);
    mmio.stuckHalt = false;
    assert(core.halt() == Result::Success);
    assert(core.select(Role::Host) == Result::Success);
    assert(core.installEvents(0x1000, 256) == Result::NotReady);
    assert(core.setAddress(5) == Result::NotReady);
    assert(core.halt() == Result::Success);
    assert(mmio.words[DWC3::eventAddressLo(0) / 4] == 0);
    assert(mmio.words[DWC3::eventSize(0) / 4] == DWC3::EventMask);

    MMIO host(Role::Host); DWC3::Core<MMIO> h(host);
    assert(h.probe(DWC3::Aperture, 32) == Result::Success && h.dmaBits() == 32);
    host.words[0x40 / 4] = 5; host.words[0x44 / 4] = 0; host.stuckHalt = true;
    assert(h.halt() == Result::Timeout && !h.stopped());
    host.stuckHalt = false;
    assert(h.halt() == Result::Success);
    assert(h.select(Role::Device) == Result::Success);

    for (auto offset : {DWC3::GSNPSID, DWC3::GHWPARAMS0, DWC3::GHWPARAMS1, DWC3::GHWPARAMS3}) {
        MMIO invalid; invalid.words[offset / 4] = UINT32_MAX; DWC3::Core<MMIO> c(invalid);
        assert(c.probe(DWC3::Aperture, 40) == Result::HardwareError && invalid.writes.empty());
    }
    MMIO dwc2; dwc2.words[DWC3::GSNPSID / 4] = 0x4f54280a; DWC3::Core<MMIO> other(dwc2);
    assert(other.probe(DWC3::Aperture, 40) == Result::Unsupported && dwc2.writes.empty());
    MMIO reset; reset.stuckReset = true; DWC3::Core<MMIO> r(reset);
    assert(r.probe(DWC3::Aperture, 40) == Result::Success && r.halt() == Result::Success);
    assert(r.select(Role::Device) == Result::Timeout && !r.stopped());
}

static void endpointCommands() {
    MMIO mmio; DWC3::Core<MMIO> core(mmio); uint32_t reply = 0;
    assert(core.command(0, DWC3::Command::Configure, 0, 0, 0, 0, reply) == Result::NotReady);
    assert(core.probe(DWC3::Aperture, 40) == Result::Success);
    size_t start = mmio.writes.size();
    assert(core.command(5, DWC3::Command::Start, 0x12, 0x3450, 0x67, 0, reply) == Result::Success);
    assert(((reply >> 16) & 127) == 3);
    assert(mmio.writes[start] == std::make_pair(DWC3::endpoint(5), 0x67u));
    assert(mmio.writes[start + 1] == std::make_pair(DWC3::endpoint(5) + 4, 0x3450u));
    assert(mmio.writes[start + 2] == std::make_pair(DWC3::endpoint(5) + 8, 0x12u));
    assert(core.command(16, DWC3::Command::Start, 0, 0, 0, 0, reply) == Result::Invalid);
    assert(core.command(0, DWC3::Command(3), 0, 0, 0, 0, reply) == Result::Invalid);
    assert(core.command(0, DWC3::Command::Start, 0, 0, 0, 1, reply) == Result::Invalid);
    mmio.commandStatus = 2;
    assert(core.command(2, DWC3::Command::End, 0, 0, 0, DWC3::CommandForce | 3u << 16, reply) == Result::Invalid);
    assert(core.command(2, DWC3::Command::End, 0, 0, 0, DWC3::CommandForce | DWC3::CommandInterrupt | 3u << 16, reply) == Result::HardwareError);
    assert(core.endpointStopping(2));
    assert(core.command(2, DWC3::Command::Start, 0, 0, 0, 0, reply) == Result::Busy);
    mmio.stuckCommand = true;
    assert(core.command(3, DWC3::Command::Start, 0, 0, 0, 0, reply) == Result::Timeout);
    start = mmio.writes.size();
    assert(core.command(3, DWC3::Command::Start, 0, 0, 0, 0, reply) == Result::Busy);
    assert(mmio.writes.size() == start);
}

static void remoteWake() {
    MMIO mmio; DWC3::Core<MMIO> core(mmio);
    assert(core.wakeUSB2() == Result::NotReady);
    assert(core.probe(DWC3::Aperture, 40) == Result::Success);
    assert(core.halt() == Result::Success && core.select(Role::Device) == Result::Success);
    assert(core.installEvents(0x10000, 256) == Result::Success);
    mmio.words[DWC3::DALEPENA / 4] = 3;
    assert(core.runDevice(USB::Speed::High) == Result::Success);
    const uint32_t preserved = mmio.words[DWC3::DCTL / 4] | 1u << 9;
    mmio.words[DWC3::DCTL / 4] = preserved;
    for (unsigned link : {1u, 2u, 3u, 5u, 15u}) {
        mmio.words[DWC3::DSTS / 4] = link << 18;
        const auto before = mmio.writes.size();
        assert(core.wakeUSB2() == Result::Success);
        assert(mmio.writes[before] == std::make_pair(DWC3::DCTL, preserved | 8u << 5));
        assert(mmio.words[DWC3::DCTL / 4] == preserved);
    }
    auto before = mmio.writes.size();
    mmio.words[DWC3::DSTS / 4] = 0;
    assert(core.wakeUSB2() == Result::Success && mmio.writes.size() == before);
    mmio.words[DWC3::DSTS / 4] = 6u << 18;
    assert(core.wakeUSB2() == Result::NotReady && mmio.writes.size() == before);
    mmio.words[DWC3::DSTS / 4] = 4 | 3u << 18;
    assert(core.wakeUSB2() == Result::Unsupported && mmio.writes.size() == before);
    mmio.words[DWC3::DSTS / 4] = UINT32_MAX;
    assert(core.wakeUSB2() == Result::HardwareError && mmio.writes.size() == before);
    mmio.words[DWC3::DSTS / 4] = 3u << 18;
    mmio.stuckWake = true;
    const auto started = mmio.elapsed;
    assert(core.wakeUSB2() == Result::Timeout && mmio.elapsed - started == 10000);
    assert(mmio.words[DWC3::DCTL / 4] & DWC3::Run);
    assert(core.halt() == Result::Success && core.wakeUSB2() == Result::NotReady);
}

static void functionWake() {
    MMIO mmio; DWC3::Core<MMIO> core(mmio);
    assert(core.wakeUSB3(7) == Result::NotReady);
    assert(core.probe(DWC3::Aperture, 40) == Result::Success);
    assert(core.halt() == Result::Success && core.select(Role::Device) == Result::Success);
    assert(core.installEvents(0x10000, 256) == Result::Success);
    mmio.words[DWC3::DALEPENA / 4] = 3;
    assert(core.runDevice(USB::Speed::Super) == Result::Success);
    const auto preserved = mmio.words[DWC3::DCTL / 4] | 1u << 9;
    mmio.words[DWC3::DCTL / 4] = preserved;
    for (unsigned link : {0u, 1u, 2u, 3u}) {
        mmio.words[DWC3::DSTS / 4] = 4 | link << 18;
        const auto before = mmio.writes.size(); const auto time = mmio.elapsed;
        assert(core.wakeUSB3(7) == Result::Success);
        const auto notification = before + (link == 3);
        if (link == 3) {
            assert(mmio.writes[before] == std::make_pair(DWC3::DCTL, preserved | 8u << 5));
            assert(mmio.elapsed - time >= 23); // U0 before the generic command
        }
        assert(mmio.writes[notification] == std::make_pair(DWC3::DGCMDPAR, 0x71u));
        assert(mmio.writes[notification+1] == std::make_pair(DWC3::DGCMD, 7u | DWC3::CommandActive));
        assert(mmio.writes.size() == notification + 2);
        assert(mmio.words[DWC3::DCTL / 4] == preserved);
    }
    auto before = mmio.writes.size();
    assert(core.wakeUSB3(256) == Result::Invalid && mmio.writes.size() == before);
    mmio.words[DWC3::DSTS / 4] = 0;
    assert(core.wakeUSB3(7) == Result::Unsupported && mmio.writes.size() == before);
    mmio.words[DWC3::DSTS / 4] = 4 | 6u << 18;
    assert(core.wakeUSB3(7) == Result::NotReady && mmio.writes.size() == before);
    mmio.words[DWC3::DSTS / 4] = UINT32_MAX;
    assert(core.wakeUSB3(7) == Result::HardwareError && mmio.writes.size() == before);
    mmio.words[DWC3::DSTS / 4] = 4 | 3u << 18; mmio.stuckWake = true;
    assert(core.wakeUSB3(7) == Result::Timeout);
    assert(mmio.writes.size() == before + 1 && mmio.words[DWC3::DGCMDPAR / 4] == 0x71);
    mmio.words[DWC3::DSTS / 4] = 4; mmio.stuckWake = false;
    mmio.genericStatus = 1;
    assert(core.wakeUSB3(31) == Result::HardwareError);
    mmio.genericStatus = 0; mmio.stuckGeneric = true;
    assert(core.wakeUSB3(255) == Result::Timeout);
    before = mmio.writes.size();
    assert(core.wakeUSB3(7) == Result::Busy && mmio.writes.size() == before);
    assert(mmio.words[DWC3::DGCMDPAR / 4] == 0xff1); // outstanding command kept intact
    assert(core.halt() == Result::Success && core.wakeUSB3(7) == Result::NotReady);
}

static void usb3LinkPolicy() {
    uint8_t family[]={3,4,5,6,125,1,1,0};
    IOUSBDeviceFamilyABI::USB3LinkPolicy policy{};
    assert(IOUSBDeviceFamilyABI::decodeUSB3LinkPolicy(family,sizeof(family),policy));
    assert(policy.u1SEL==3 && policy.u1PEL==4 && policy.u2SEL==0x605 && policy.u2PEL==381 &&
        policy.u1Enabled && !policy.u2Enabled);
    assert(!IOUSBDeviceFamilyABI::decodeUSB3LinkPolicy(family,7,policy));
    assert(!IOUSBDeviceFamilyABI::decodeUSB3LinkPolicy(nullptr,8,policy));
    family[6]=2;assert(!IOUSBDeviceFamilyABI::decodeUSB3LinkPolicy(family,8,policy));
    family[6]=0;family[7]=255;assert(!IOUSBDeviceFamilyABI::decodeUSB3LinkPolicy(family,8,policy));
    family[7]=1;assert(IOUSBDeviceFamilyABI::decodeUSB3LinkPolicy(family,8,policy) && !policy.u1Enabled && policy.u2Enabled);
    struct LinkMMIO:MMIO {
        bool rejectDisable=false,rejectEnable=false,loseRun=false,loseSpeed=false;
        void write(uint32_t offset,uint32_t value) {
            if (offset==DWC3::DCTL && rejectDisable && !(value&DWC3::USB3LinkEnable)) return;
            if (offset==DWC3::DCTL && rejectEnable && (value&DWC3::USB3LinkEnable)) return;
            MMIO::write(offset,value);
        }
        void delay(unsigned us) {
            const bool completing=genericAt && !stuckGeneric && elapsed+us>=genericAt;
            MMIO::delay(us);
            if (completing && loseRun) words[DWC3::DCTL/4]&=~DWC3::Run;
            if (completing && loseSpeed) words[DWC3::DSTS/4]&=~7u;
        }
    } io;
    DWC3::Core<LinkMMIO> core(io);
    assert(core.setUSB3LinkPolicy(true,false,10,511)==Result::NotReady);
    assert(core.probe(DWC3::Aperture,40)==Result::Success && core.halt()==Result::Success);
    assert(core.select(Role::Device)==Result::Success);
    assert(core.installEvents(0x40000,256)==Result::Success);
    io.words[DWC3::DALEPENA/4]=3;
    io.words[DWC3::DCTL/4]=DWC3::USB3LinkEnable|(1u<<17);
    assert(core.runDevice(USB::Speed::Super)==Result::Success);
    assert(!(io.words[DWC3::DCTL/4]&DWC3::USB3LinkEnable));
    io.words[DWC3::DSTS/4]=4;
    assert(core.setUSB3LinkPolicy(true,true,10,511)==Result::Success);
    assert((io.words[DWC3::DCTL/4]&DWC3::USB3LinkEnable)==DWC3::USB3LinkEnable);
    assert(io.words[DWC3::DCTL/4]&(1u<<17));
    assert(io.words[DWC3::DGCMDPAR/4]==10 && (io.words[DWC3::DGCMD/4]&15)==2);
    assert(core.setUSB3LinkPolicy(false,true,10,511)==Result::Success);
    assert((io.words[DWC3::DCTL/4]&DWC3::USB3LinkEnable)==(DWC3::AcceptU2|DWC3::InitiateU2));
    assert(!io.words[DWC3::DGCMDPAR/4]);
    for (auto pel:{0u,125u,126u,255u}) {
        assert(core.setUSB3LinkPolicy(true,true,pel,100)==Result::Success);
        assert(io.words[DWC3::DGCMDPAR/4]==(pel>125 ? 0:pel));
    }
    assert(core.setUSB3LinkPolicy(false,true,0,125)==Result::Success && io.words[DWC3::DGCMDPAR/4]==125);
    auto before=io.writes.size();
    assert(core.setUSB3LinkPolicy(true,true,256,0)==Result::Invalid && io.writes.size()==before);
    assert(core.setUSB3LinkPolicy(true,true,0,65536)==Result::Invalid && io.writes.size()==before);
    io.words[DWC3::DSTS/4]=0;
    assert(core.setUSB3LinkPolicy(true,false,10,511)==Result::Unsupported && io.writes.size()==before);
    assert(core.setUSB3LinkPolicy(false,false,10,511)==Result::Success);
    assert(!(io.words[DWC3::DCTL/4]&DWC3::USB3LinkEnable) && !io.words[DWC3::DGCMDPAR/4]);
    io.words[DWC3::DSTS/4]=4;
    for (auto offset:{DWC3::DSTS,DWC3::DCTL,DWC3::DGCMD}) {
        const auto saved=io.words[offset/4];io.words[offset/4]=UINT32_MAX;before=io.writes.size();
        assert(core.setUSB3LinkPolicy(true,true,10,511)==Result::HardwareError && io.writes.size()==before);
        io.words[offset/4]=saved;
    }
    io.words[DWC3::DGCMD/4]=DWC3::CommandActive;before=io.writes.size();
    assert(core.setUSB3LinkPolicy(true,true,10,511)==Result::Busy && io.writes.size()==before);
    io.words[DWC3::DGCMD/4]=0;
    io.words[DWC3::DCTL/4]=DWC3::Run|DWC3::USB3LinkEnable;io.rejectDisable=true;before=io.writes.size();
    assert(core.setUSB3LinkPolicy(true,true,10,511)==Result::HardwareError && io.writes.size()==before);
    io.rejectDisable=false;io.rejectEnable=true;
    assert(core.setUSB3LinkPolicy(true,true,10,511)==Result::HardwareError);
    assert(!(io.words[DWC3::DCTL/4]&DWC3::USB3LinkEnable));io.rejectEnable=false;
    io.genericStatus=1;
    assert(core.setUSB3LinkPolicy(true,true,10,511)==Result::HardwareError);
    assert(!(io.words[DWC3::DCTL/4]&DWC3::USB3LinkEnable));io.genericStatus=0;
    io.loseRun=true;
    assert(core.setUSB3LinkPolicy(true,true,10,511)==Result::NotReady);io.loseRun=false;
    io.words[DWC3::DCTL/4]|=DWC3::Run;io.loseSpeed=true;
    assert(core.setUSB3LinkPolicy(true,true,10,511)==Result::NotReady);io.loseSpeed=false;
    io.words[DWC3::DSTS/4]=4;io.stuckGeneric=true;const auto started=io.elapsed;
    assert(core.setUSB3LinkPolicy(true,true,10,511)==Result::Timeout && io.elapsed-started==500);
    assert(!(io.words[DWC3::DCTL/4]&DWC3::USB3LinkEnable));before=io.writes.size();
    assert(core.setUSB3LinkPolicy(true,true,10,511)==Result::Busy && io.writes.size()==before);
    assert(core.halt()==Result::Success && core.setUSB3LinkPolicy(false,false,0,0)==Result::NotReady);
}

static void endpointConfiguration() {
    MMIO mmio; DWC3::Core<MMIO> core(mmio);
    DWC3::EndpointConfiguration control{0,DWC3::EndpointType::Control,USB::Speed::High,64,0,0};
    assert(core.configureEndpoint(control)==Result::NotReady);
    assert(core.probe(DWC3::Aperture,40)==Result::Success);
    assert(core.beginEndpointConfiguration()==Result::NotReady);
    assert(core.halt()==Result::Success && core.select(Role::Device)==Result::Success);
    assert(core.beginEndpointConfiguration(1)==Result::Invalid);
    assert(core.beginEndpointConfiguration()==Result::Success);
    assert(core.configureEndpoint(control,true)==Result::NotReady);
    assert(core.configureEndpoint(control)==Result::Success);
    control.physicalEndpoint=1;
    assert(core.configureEndpoint(control)==Result::Success);
    assert((mmio.words[DWC3::DALEPENA/4]&3)==3);
    auto bad=control;bad.maximumPacketSize=512;
    auto before=mmio.writes.size();
    assert(core.configureEndpoint(bad)==Result::Invalid && mmio.writes.size()==before);
    control.speed=USB::Speed::Super;control.maximumPacketSize=512;
    assert(core.configureEndpoint(control,true)==Result::Success);
    assert((mmio.words[(DWC3::endpoint(1)+8)/4]&(3u<<30))==(2u<<30));
    DWC3::EndpointConfiguration bulk{3,DWC3::EndpointType::Bulk,USB::Speed::Super,1024,0,15};
    before=mmio.writes.size();
    assert(core.configureEndpoint(bulk)==Result::Success);
    assert(mmio.writes[before+2]==std::make_pair(DWC3::endpoint(3)+8,4u|1024u<<3|1u<<17|15u<<22));
    assert(mmio.writes[before+1]==std::make_pair(DWC3::endpoint(3)+4,3u<<25|1u<<8));
    uint32_t reply=0;
    assert(core.command(3,DWC3::Command::Start,0,0x10000,0,0,reply)==Result::Success);
    assert(core.endpointActive(3));
    assert(core.configureEndpoint(bulk)==Result::Busy && core.disableEndpoint(3)==Result::Busy);
    assert(core.beginEndpointConfiguration(2)==Result::Busy);
    assert(core.command(3,DWC3::Command::End,0,0,0,DWC3::CommandInterrupt|3u<<16,reply)==Result::Pending);
    assert(core.beginEndpointConfiguration(2)==Result::Busy);
    assert(core.halt()==Result::Success && !core.endpointActive(3));
    assert(core.beginEndpointConfiguration(2)==Result::Success);
    assert(mmio.words[DWC3::DALEPENA/4]==3);
    assert(core.disableEndpoint(1)==Result::Success && mmio.words[DWC3::DALEPENA/4]==1);
    bulk.type=DWC3::EndpointType::Isochronous;
    assert(core.configureEndpoint(bulk)==Result::Invalid);
    DWC3::EndpointConfiguration intr{2,DWC3::EndpointType::Interrupt,USB::Speed::Full,64,255,0};
    assert(intr.valid() && ((intr.parameter1()>>16)&255)==13);
    intr.maximumPacketSize=65;assert(!intr.valid());
    intr.maximumPacketSize=64;intr.speed=USB::Speed::High;assert(!intr.valid());
    intr.interval=16;assert(intr.valid());
    intr.burst=1;assert(!intr.valid());
    assert(core.disableEndpoint(32)==Result::Invalid);
}

static void events() {
    MMIO mmio; DWC3::Core<MMIO> core(mmio);
    assert(core.probe(DWC3::Aperture, 40) == Result::Success);
    assert(core.halt() == Result::Success && core.select(Role::Device) == Result::Success);
    assert(core.installEvents(0x10000, 256) == Result::Success);
    uint32_t reply = 0;
    assert(core.command(2, DWC3::Command::End, 0, 0, 0, DWC3::CommandInterrupt | 3u << 16, reply) == Result::Pending);
    assert(core.endpointStopping(2));
    std::array<uint32_t, 64> ring{};
    std::vector<uint32_t> seen;
    auto handler = [&](DWC3::Event event) { seen.push_back(event.word); };
    for (unsigned i = 0; i < ring.size(); ++i) ring[i] = i << 16;
    mmio.words[DWC3::eventCount(0) / 4] = 252;
    assert(core.drainEvents(ring.data(), handler) == Result::Success && seen.size() == 63);
    ring[0] = 0x201;
    mmio.words[DWC3::eventCount(0) / 4] = 8;
    assert(core.drainEvents(ring.data(), handler) == Result::Success);
    assert(seen[63] == (63u << 16) && seen[64] == 0x201 && mmio.barriers == 2);
    assert(mmio.words[DWC3::eventCount(0) / 4] == 0);
    ring[1] = 2u << 1 | 7u << 6 | 8u << 24;
    mmio.words[DWC3::eventCount(0) / 4] = 4;
    assert(core.drainEvents(ring.data(), handler) == Result::Success && !core.endpointStopping(2));
    ring[2] = 31u << 1 | 1u << 6;
    mmio.words[DWC3::eventCount(0) / 4] = 4;
    assert(core.drainEvents(ring.data(), handler) == Result::HardwareError && seen.size() == 66);
    for (uint32_t invalid : {2u, 258u, 260u, UINT32_MAX}) {
        mmio.words[DWC3::eventCount(0) / 4] = invalid;
        auto writes = mmio.writes.size();
        assert(core.drainEvents(ring.data(), handler) == Result::HardwareError);
        assert(mmio.writes.size() == writes && seen.size() == 66);
    }
    DWC3::Event e{1u | 2u << 8};
    assert(e.device() && e.deviceSpecific() && e.type() == 2);
    e.word = 5u << 1 | 3u << 6 | 4u << 12 | 0x1234u << 16;
    assert(!e.device() && e.endpointNumber() == 5 && e.type() == 3 && e.status() == 4 && e.parameters() == 0x1234);
    MMIO scratch;
    scratch.words[DWC3::QScratch::General/4]=0xa0;
    assert(DWC3::setPipeClock(scratch,true)==Result::Success);
    assert(scratch.words[DWC3::QScratch::General/4]==0xa9);
    assert(DWC3::setPipeClock(scratch,false)==Result::Success);
    assert(scratch.words[DWC3::QScratch::General/4]==0xa0);
    scratch.words[DWC3::QScratch::HSControl / 4] = 8;
    scratch.words[DWC3::QScratch::SSControl / 4] = 16;
    assert(DWC3::setVbusOverride(scratch, true) == Result::Success);
    assert(scratch.words[DWC3::QScratch::HSControl / 4] == (8 | 1u << 20 | 1u << 28));
    assert(DWC3::setVbusOverride(scratch, false) == Result::Success);
    assert(scratch.words[DWC3::QScratch::HSControl / 4] == 8 && scratch.words[DWC3::QScratch::SSControl / 4] == 16);
    const auto writes = scratch.writes.size();
    scratch.words[DWC3::QScratch::SSControl / 4] = UINT32_MAX;
    assert(DWC3::setVbusOverride(scratch, true) == Result::HardwareError && scratch.writes.size() == writes);
}

static void controlIntegration() {
    // Drive the real Control -> Transfer -> Core chain through the register
    // model and Core's event dispatcher. This checks shared active/stopping
    // bookkeeping, rather than substituting a controller command stub.
    MMIO mmio; DWC3::Core<MMIO> core(mmio);
    assert(core.probe(DWC3::Aperture,40)==Result::Success);
    assert(core.halt()==Result::Success && core.select(Role::Device)==Result::Success);
    assert(core.beginEndpointConfiguration()==Result::Success);
    DWC3::EndpointConfiguration cfg{0,DWC3::EndpointType::Control,USB::Speed::Super,512,0,0};
    assert(core.configureEndpoint(cfg)==Result::Success);
    cfg.physicalEndpoint=1; assert(core.configureEndpoint(cfg)==Result::Success);
    alignas(16) std::array<DWC3::TRB,4> out{},in{};
    alignas(8) uint8_t setup[8]={0x80,6,0,1,0,0,18,0};
    std::array<uint32_t,64> ring{};
    DWC3::ControlMemory memory{out.data(),in.data(),0x10000,0x11000,4,setup,0x12000};
    DWC3::Control<DWC3::Core<MMIO>,MMIO> control(core,mmio);
    assert(control.initialize(memory,512,42)==Result::Success);
    assert(core.installEvents(0x13000,256)==Result::Success);
    mmio.resourceIndex=0;
    assert(control.start()==Result::Success && core.endpointActive(0));
    assert(core.runDevice(USB::Speed::Super)==Result::Success);
    // CONNECT_DONE negotiated HS while SETUP was already armed. MPS must be
    // modified in place, without discarding SETUP or changing its TRB.
    assert(control.negotiate(USB::Speed::High)==Result::Success && core.endpointActive(0));
    assert(out[0].size==8 && (out[0].control&DWC3::Owned));
    DWC3::ControlNotification message;
    unsigned cursor=0;
    auto receive=[&](uint32_t word){
        ring[cursor++]=word;mmio.words[DWC3::eventCount(0)/4]=4;
        auto handler=[&](DWC3::Event event){
            auto result=control.handle(event,42,message);
            assert(result==Result::Success || result==Result::Pending);
        };
        assert(core.drainEvents(ring.data(),handler)==Result::Success);
    };
    out[0].control &= ~DWC3::Owned;out[0].size=0;
    receive(1u<<6);
    assert(!core.endpointActive(0) && message.kind==DWC3::ControlNotificationKind::Setup);
    auto token=message.transaction;
    mmio.resourceIndex=3;
    DWC3::DMASegment data{0x14000,18};
    assert(control.supplyData(token,&data,1,18)==Result::Success && core.endpointActive(1));
    assert(control.negotiate(USB::Speed::Super)==Result::NotReady);
    cfg.physicalEndpoint=1; cfg.speed=USB::Speed::High; cfg.maximumPacketSize=64;
    assert(core.configureEndpoint(cfg,true)==Result::Busy); // DATA never gets SETUP's exception
    in[0].control &= ~DWC3::Owned;in[0].size=0;
    receive(1u<<1|1u<<6);
    assert(!core.endpointActive(1) && message.kind==DWC3::ControlNotificationKind::Data && message.bytes==18);
    receive(3u<<6|2u<<12); // OUT status readiness arrives before Family approval
    mmio.resourceIndex=0;
    assert(control.allowStatus(token)==Result::Success && core.endpointActive(0));
    cfg.physicalEndpoint=0;
    assert(core.configureEndpoint(cfg,true)==Result::Busy); // resource 0 does not mean inactive
    out[0].control &= ~DWC3::Owned;out[0].size=0;
    receive(1u<<6);
    assert(message.kind==DWC3::ControlNotificationKind::Status && core.endpointActive(0)); // next SETUP armed
    assert(control.stop(message)==Result::Pending && core.endpointStopping(0));
    receive(7u<<6|8u<<24);
    assert(!core.endpointStopping(0) && !core.endpointActive(0) && !control.ownsBuffers());
    assert(control.currentState()==DWC3::ControlState::Stopped);
    assert(core.halt()==Result::Success);
}


static void isochronousIntegration() {
    MMIO mmio;DWC3::Core<MMIO> core(mmio);
    assert(core.probe(DWC3::Aperture,40)==Result::Success);
    assert(core.halt()==Result::Success && core.select(Role::Device)==Result::Success);
    assert(core.beginEndpointConfiguration()==Result::Success);
    DWC3::EndpointConfiguration control{0,DWC3::EndpointType::Control,USB::Speed::High,64,0,0};
    assert(core.configureEndpoint(control)==Result::Success);control.physicalEndpoint=1;
    assert(core.configureEndpoint(control)==Result::Success);
    DWC3::EndpointConfiguration config{3,DWC3::EndpointType::Isochronous,USB::Speed::High,1024,1,0,2};
    assert(core.configureEndpoint(config)==Result::Success);
    assert(core.installEvents(0x20000,256)==Result::Success);
    uint32_t reply=0;
    assert(core.startIsochronous(3,0x10000,0xffff,reply)==Result::NotReady); // no prestarting an ISO endpoint
    assert(core.runDevice(USB::Speed::High)==Result::Success);
    alignas(16) std::array<DWC3::TRB,8> trbs{};
    DWC3::IsochronousRing<DWC3::Core<MMIO>,MMIO> iso(core,mmio);
    assert(iso.initialize(config,trbs.data(),0x10000,8,91)==Result::Success);
    assert(iso.push(0x30000,1500,1500,1)==Result::Success);
    std::array<uint32_t,64> events{};unsigned cursor=0;
    auto receive=[&](uint32_t word) {
        events[cursor++]=word;mmio.words[DWC3::eventCount(0)/4]=4;
        auto handler=[&](DWC3::Event event) {
            const auto result=iso.handle(event,91,100);
            assert(result==Result::Success || result==Result::Pending);
        };
        assert(core.drainEvents(events.data(),handler)==Result::Success);
    };
    mmio.words[DWC3::DSTS/4]=0x3ffdu<<3;
    receive(3u<<1|3u<<6|0x3ffdu<<16);
    mmio.expireStarts=1; // a known Start bus-expiry still retains a hardware resource
    assert(iso.start(100)==Result::Pending && core.endpointStopping(3) && core.endpointActive(3));
    assert(iso.transferResource()==3);
    assert(core.disableEndpoint(3)==Result::Busy);
    receive(3u<<1|7u<<6|8u<<24);
    assert(!core.endpointActive(3) && !core.endpointStopping(3));
    DWC3::IsochronousCompletion done{};
    assert(iso.takeCompletion(1,done)==Result::Pending);
    mmio.words[DWC3::DSTS/4]=5u<<3;
    receive(3u<<1|3u<<6|0x4005u<<16);
    assert(iso.start(100)==Result::Success && core.endpointActive(3));
    bool fullParameter=false;
    for (auto w:mmio.writes) if (w.first==DWC3::endpoint(3)+12 && (w.second&15)==6 &&
        (w.second>>16)==0x4009) fullParameter=true;
    assert(fullParameter && iso.transferResource()==3);
    trbs[0].size=0;trbs[0].control=(trbs[0].control&~DWC3::Owned)|(0x4009u<<14);
    receive(3u<<1|2u<<6|0x4009u<<16);
    assert(iso.takeCompletion(1,done)==Result::Success && done.bytes==1500);
    assert(core.endpointActive(3)); // an ISO interval does not end its persistent resource
    const auto before=mmio.writes.size();
    assert(iso.push(0x31000,512,512,2)==Result::Success);
    assert(mmio.writes.size()==before+1 && mmio.writes.back().first==DWC3::endpoint(3)+12);
    assert(core.startTransfer(3,0x10000,DWC3::TRBType::IsoFirst,reply)==Result::Invalid);
    assert(iso.cancel()==Result::Pending && core.endpointStopping(3));
    assert(iso.takeCompletion(2,done)==Result::Pending);
    receive(3u<<1|7u<<6|8u<<24);
    assert(!core.endpointActive(3) && !core.endpointStopping(3));
    assert(iso.takeCompletion(2,done)==Result::Success && done.result==Result::Aborted);
    assert(core.halt()==Result::Success);
}

int main() {
    platform(); probeAndStop(); endpointCommands(); remoteWake(); functionWake(); usb3LinkPolicy(); endpointConfiguration(); events(); controlIntegration();isochronousIntegration();
    std::puts("USB core: platform contracts, DMA ranges, halt/reset, commands, remote wake, USB3 link policy and events passed");
}
