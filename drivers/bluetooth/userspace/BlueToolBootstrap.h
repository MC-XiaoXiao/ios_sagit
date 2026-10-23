// SPDX-License-Identifier: BSD-2-Clause
#pragma once
#include "BluetoothFirmware.h"
#include <string.h>

namespace Bluetooth {
namespace BlueTool {
constexpr const char* Service = "com.apple.BlueTool";
constexpr const char* MessageKey = "kMsgId";
constexpr const char* ArgumentsKey = "kMsgArgs";
constexpr const char* RunBuiltin = "RunBuiltinScript";
constexpr uint16_t CompanyID = 29; // pinned hci_qca.c, not an Apple vendor identity
constexpr int64_t Vendor = 0x71636f6d; // new BlueTool vendor tag 'qcom'; only used for reporting
enum class Script { Invalid, Boot, Init, DeepSleep };
enum class Error : int64_t { Success = 0, Failed = 1, Invalid = 2, Busy = 3, Timeout = 4, Identity = 5 };
inline Script script(const char* value)
{
    if (!value) return Script::Invalid;
    if (!strcmp(value,"boot")) return Script::Boot;
    if (!strcmp(value,"init")) return Script::Init;
    if (!strcmp(value,"deepsleep")) return Script::DeepSleep;
    return Script::Invalid;
}
inline bool parseAddress(const char* text, uint64_t& address)
{
    if (!text || strlen(text) != 17) return false;
    uint64_t result = 0;
    for (unsigned i = 0; i < 17; ++i) {
        if (i % 3 == 2) { if (text[i] != ':') return false; continue; }
        const unsigned char c = text[i];
        const int digit = c >= '0' && c <= '9' ? c-'0' : c >= 'a' && c <= 'f' ? c-'a'+10 :
            c >= 'A' && c <= 'F' ? c-'A'+10 : -1;
        if (digit < 0) return false;
        result = (result << 4) | unsigned(digit);
    }
    if (!result || result == 0xffffffffffffULL || ((result >> 40) & 1)) return false;
    address = result;
    return true;
}
struct Snapshot {
    uint64_t v[BootstrapScalars] {};
    bool valid() const {
        return v[BootstrapVersion] == 1 && v[BootstrapState] <= uint32_t(State::Fault) &&
            !(v[BootstrapResources] & ~uint64_t(AllResources)) &&
            v[BootstrapFault] <= uint32_t(Result::Ignored) && v[BootstrapResult] <= uint32_t(Result::Ignored) &&
            !(v[BootstrapFlags] & ~uint64_t(31)) && v[BootstrapProduct] <= UINT32_MAX &&
            v[BootstrapSoC] <= UINT32_MAX && v[BootstrapROM] <= UINT16_MAX &&
            v[BootstrapPatch] <= UINT16_MAX && v[BootstrapAddress] <= 0xffffffffffffULL;
    }
    bool configured() const { return valid() && (v[BootstrapFlags] & BootstrapConfigured) &&
        v[BootstrapSoC] && v[BootstrapROM] && v[BootstrapAddress] &&
        v[BootstrapAddress] != 0xffffffffffffULL && !((v[BootstrapAddress] >> 40) & 1); }
    bool ready() const { return configured() && v[BootstrapGeneration] &&
        v[BootstrapState] == uint32_t(State::Ready) && v[BootstrapResources] == AllResources &&
        v[BootstrapFault] == uint32_t(Result::Success) &&
        (v[BootstrapFlags] & (BootstrapEndpoints | BootstrapPowerRequested)) ==
            (BootstrapEndpoints | BootstrapPowerRequested); }
    bool same(const Snapshot& other) const {
        if (v[BootstrapGeneration] != other.v[BootstrapGeneration]) return false;
        for (unsigned i = BootstrapProduct; i <= BootstrapAddress; ++i)
            if (v[i] != other.v[i]) return false;
        return true;
    }
};
struct Proof {
    uint64_t generation = 0, address = 0;
    SoCVersion soc {};
    uint16_t company = 0, hciRevision = 0, lmpSubversion = 0, aclBytes = 0, aclPackets = 0;
    uint8_t hciVersion = 0, lmpVersion = 0, commands[64] {}, features[8] {};
    uint16_t leBytes = 0;
    uint8_t lePackets = 0;
    bool complete = false;
};

// This backend is synchronous only in the user process's dedicated serial
// queue. The kernel workloop stays asynchronous. Each command gets its own
// deadline inside the native BlueTool 30 s / 4 s operation budgets.
template <class Backend> class Bootstrap {
    Backend& io;
    Snapshot policy;
    Proof proof;
    uint8_t event[258] {};
    size_t length = 0;
    bool vendorQueried = false;
    uint64_t operationDeadline = 0;
    Error command(uint16_t opcode, bool vendor = false) {
        const uint8_t bytes[] = { Command, uint8_t(opcode), uint8_t(opcode >> 8), uint8_t(vendor ? 1 : 0), 0x19 };
        uint64_t deadline;
        if (!deadlineAfter(io.now(),2000,deadline)) return Error::Invalid;
        if (deadline > operationDeadline) deadline = operationDeadline;
        if (io.now() >= deadline) return Error::Timeout;
        if (!io.send(bytes,vendor ? 5 : 4,deadline)) return io.now() >= deadline ? Error::Timeout : Error::Failed;
        for (unsigned budget = 0; budget < 32; ++budget) {
            const auto result = io.receive(event,sizeof(event),length,deadline);
            if (result != Error::Success) return result;
            if (!validPacket(event,length,false) || event[0] != Event) return Error::Failed;
            if (event[1] == 0x10) return Error::Failed;
            if (vendor && event[1] == 0xff) return Error::Success;
            if (event[1] == 0x0f) {
                if (length != 7 || read16(event+5) != opcode || event[3]) return Error::Failed;
                continue; // Command Status is not completion
            }
            if (event[1] == 0x0e) {
                if (length < 7) return Error::Failed;
                const auto found = read16(event+4);
                if (found == DownloadOpcode && (vendor || vendorQueried)) {
                    if (length != 7 || !event[3] || event[6]) return Error::Failed;
                    continue; // optional QCA CC, never proof of a version reply
                }
                if (found != opcode || !event[3] || event[6]) return Error::Failed;
                return Error::Success;
            }
            // No unsolicited ACL/SCO or unrelated command reply can complete
            // the operation. Bounded asynchronous events are consumed here;
            // the controller has no host connections during this session.
        }
        return Error::Failed;
    }
    Error probe() {
        auto e = command(0x1001); // Read Local Version Information
        if (e != Error::Success) return e;
        if (length != 15 || read16(event+11) != CompanyID) return Error::Identity;
        proof.hciVersion=event[7];proof.hciRevision=read16(event+8);proof.lmpVersion=event[10];
        proof.company=read16(event+11);proof.lmpSubversion=read16(event+13);
        e=command(DownloadOpcode,true);
        if (e != Error::Success) return e;
        if (parseVersionEvent(event,length,proof.soc) != Result::Success ||
            proof.soc.product != policy.v[BootstrapProduct] || proof.soc.soc != policy.v[BootstrapSoC] ||
            proof.soc.rom != policy.v[BootstrapROM] || proof.soc.patch != policy.v[BootstrapPatch])
            return Error::Identity;
        vendorQueried=true;
        e=command(0x1009); // Read BD_ADDR; HCI carries the reverse of DT order
        if (e != Error::Success) return e;
        if (length != 13) return Error::Identity;
        for (unsigned i=0;i<6;++i) proof.address=(proof.address<<8)|event[12-i];
        if (proof.address != policy.v[BootstrapAddress]) return Error::Identity;
        e=command(0x1002);
        if (e != Error::Success) return e;
        if (length != 71) return Error::Failed;
        memcpy(proof.commands,event+7,sizeof(proof.commands));
        e=command(0x1003);
        if (e != Error::Success) return e;
        if (length != 15 || !(event[11]&0x40) || (event[11]&0x20)) return Error::Identity;
        memcpy(proof.features,event+7,sizeof(proof.features));
        e=command(0x1005);
        if (e != Error::Success) return e;
        if (length != 14) return Error::Failed;
        proof.aclBytes=read16(event+7);proof.aclPackets=read16(event+10);
        if (proof.aclBytes < 27 || proof.aclBytes > MaxACLPayload || !proof.aclPackets) return Error::Identity;
        e=command(0x2002);
        if (e != Error::Success) return e;
        if (length != 10) return Error::Failed;
        proof.leBytes=read16(event+7);proof.lePackets=event[9];
        if (bool(proof.leBytes) != bool(proof.lePackets) || proof.leBytes > MaxACLPayload ||
            (proof.leBytes && proof.leBytes < 27)) return Error::Identity;
        return Error::Success;
    }
public:
    explicit Bootstrap(Backend& backend):io(backend) {}
    const Proof& evidence() const { return proof; }
    Error run(Script operation, const char* address) {
        proof={};vendorQueried=false;
        uint64_t requested=0;
        if (operation == Script::Invalid || (address && !parseAddress(address,requested))) return Error::Invalid;
        Snapshot state;
        if (!io.snapshot(state) || !state.configured()) return Error::Failed;
        if (address && requested != state.v[BootstrapAddress]) return Error::Identity;
        if (state.v[BootstrapFlags] & BootstrapPortOpen) return Error::Busy;
        const bool on = operation != Script::DeepSleep;
        uint64_t deadline;
        if (!deadlineAfter(io.now(),on ? 20000 : 3000,deadline)) return Error::Invalid;
        if (!deadlineAfter(io.now(),on ? 25000 : 3000,operationDeadline)) return Error::Invalid;
        uint64_t accepted=0;
        if (!io.power(on,state.v[BootstrapGeneration],accepted)) return Error::Failed;
        struct Cancel {
            Backend& io; uint64_t epoch; bool armed;
            ~Cancel() { uint64_t ignored=0; if (armed) io.power(false,epoch,ignored); }
        } cancel{io,accepted,on};
        for (;;) {
            if (!io.snapshot(state) || !state.configured() || state.v[BootstrapGeneration] != accepted) return Error::Failed;
            if (on ? state.ready() : (state.v[BootstrapState] == uint32_t(State::Off) &&
                !state.v[BootstrapResources] && !(state.v[BootstrapFlags]&BootstrapPowerRequested))) break;
            if (state.v[BootstrapState] == uint32_t(State::Fault) ||
                state.v[BootstrapFault] != uint32_t(Result::Success)) return Error::Failed;
            if (io.now() >= deadline) return Error::Timeout;
            io.pause();
        }
        if (!on) return Error::Success; // full shutdown, not unsupported deep retention
        if (state.v[BootstrapFlags] & BootstrapPortOpen) return Error::Busy;
        policy=state;
        if (!io.open()) return Error::Busy;
        const auto result=probe();
        const bool closed=io.close(result == Error::Success);
        if (result != Error::Success) return result;
        if (!closed || !io.snapshot(state) || !state.ready() || !policy.same(state) ||
            (state.v[BootstrapFlags]&BootstrapPortOpen)) return Error::Failed;
        proof.generation=state.v[BootstrapGeneration];proof.complete=true;
        cancel.armed=false;
        return Error::Success;
    }
};
}
}
