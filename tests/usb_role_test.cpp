// SPDX-License-Identifier: BSD-2-Clause
#include "USBRole.h"
#include "USBPlatform.h"
#include "USBPower.h"
#include <cassert>
#include <cstdio>
#include <string>
#include <vector>

using USB::Result;
using USB::Role;
using USB::PowerRole;
using USB::Orientation;
USB::ConnectorState signal(uint64_t id, bool host = true, PowerRole power = PowerRole::Source,
                           Orientation orientation = Orientation::Normal) {
    return {true, host, power == PowerRole::Sink, false, orientation, power, {}, id};
}
struct Backend {
    std::vector<std::string> calls;
    USB::ConnectorState target{};
    Result off = Result::Success, on = Result::Success, power = Result::Success, safeOff = Result::Success;
    Result prepare = Result::Success, publish = Result::Success;
    bool stopOK = true, dma = false, sourceEnabled = false;
    uint64_t epoch = 0, connection = 0;
    Result source(bool enabled) {
        calls.push_back(enabled ? "on" : "off");
        const auto r = enabled ? on : off;
        if (enabled) assert(target.power == PowerRole::Source);
        if (r == Result::Success) sourceEnabled = enabled;
        return r;
    }
    bool stop(Role, uint64_t) {
        calls.push_back("stop"); if (stopOK) dma = false; return stopOK;
    }
    Result safePower(Role role, const USB::ConnectorState &snapshot) {
        calls.push_back(role == Role::None ? "safe-off" : "power");
        assert(!dma);
        if (role != Role::None) { assert(snapshot.desired() == role); target = snapshot; }
        return role == Role::None ? safeOff : power;
    }
    Result configure(Role role, const USB::ConnectorState &snapshot) {
        calls.push_back(role == Role::Host ? "host" : "device");
        assert(!dma && snapshot.sameRoute(target)); return prepare;
    }
    Result start(Role, uint64_t generation, const USB::ConnectorState &snapshot) {
        calls.push_back("start"); assert(snapshot.sameRoute(target));
        dma = true; epoch = generation; connection = snapshot.connection;
        return publish;
    }
};
static void attach(USB::RoleSwitch<Backend> &roles, USB::ConnectorState snapshot, uint64_t now) {
    assert(roles.update(snapshot, now) == Result::Busy);
    assert(roles.update(snapshot, now + 20000) == Result::Success);
    assert(roles.current() == snapshot.desired() && roles.accepts(roles.epoch()));
}

static void debounceAndPowerRoles() {
    for (bool host : {false, true}) for (auto power : {PowerRole::Sink, PowerRole::Source}) {
        Backend b; USB::RoleSwitch<Backend> roles(b);
        auto route = signal(1, host, power);
        attach(roles, route, 100);
        assert(b.sourceEnabled == (power == PowerRole::Source));
        const std::vector<std::string> expected = power == PowerRole::Source ?
            std::vector<std::string>{"off","power",host ? "host" : "device","on","start"} :
            std::vector<std::string>{"off","power",host ? "host" : "device","start"};
        assert(b.calls == expected);
        const auto epoch = roles.epoch(); b.calls.clear();
        assert(roles.update(route, 20101) == Result::Success && b.calls.empty());
        assert(roles.update({}, 20102) == Result::Success && roles.current() == Role::None);
        assert((b.calls == std::vector<std::string>{"off","stop","safe-off"}) && !roles.accepts(epoch));
    }
    Backend b; USB::RoleSwitch<Backend> roles(b);
    auto one = signal(1), two = signal(2, false, PowerRole::Sink), three = signal(3);
    assert(roles.update(one, 100) == Result::Busy);
    assert(roles.update(two, 200) == Result::Busy);
    assert(roles.update(three, 300) == Result::Busy);
    assert(roles.update(three, 20299) == Result::Busy && b.calls.empty());
    assert(roles.update(three, 20300) == Result::Success);
}

static void sameRoleReplacement() {
    for (bool reverse : {false, true}) {
        Backend b; USB::RoleSwitch<Backend> roles(b);
        attach(roles, signal(1), 0); const auto old = roles.epoch(); b.calls.clear();
        auto replacement = signal(3, true, PowerRole::Source, reverse ? Orientation::Reverse : Orientation::Normal);
        // The detached generation2 was coalesced, yet generation3 still
        // shuts down old DMA immediately instead of treating Host as unchanged.
        assert(roles.update(replacement, 21000) == Result::Busy);
        assert((b.calls == std::vector<std::string>{"off","stop","safe-off"}));
        assert(!b.dma && !roles.accepts(old) && roles.current() == Role::None);
        b.calls.clear();
        assert(roles.update(replacement, 40999) == Result::Busy && b.calls.empty());
        assert(roles.update(replacement, 41000) == Result::Success && b.connection == 3 && roles.epoch() != old);
        b.calls.clear();
        assert(roles.update(signal(1), 41001) == Result::Busy && b.calls.empty()); // delayed old sample
    }
}

static void roleSwapAndSessionLoss() {
    Backend b; USB::RoleSwitch<Backend> roles(b);
    attach(roles, signal(1), 0); const auto old = roles.epoch(); b.calls.clear();
    auto device = signal(2, false, PowerRole::Sink);
    assert(roles.update(device, 21000) == Result::Busy && !b.dma && !roles.accepts(old));
    assert((b.calls == std::vector<std::string>{"off","stop","safe-off"}));
    assert(roles.update(device, 41000) == Result::Success && roles.current() == Role::Device && !b.sourceEnabled);
    auto lost = device; lost.connection = 3; lost.vbus = false; b.calls.clear();
    assert(roles.update(lost, 41001) == Result::Success && !b.dma && roles.current() == Role::None);
    assert((b.calls == std::vector<std::string>{"off","stop","safe-off"}));
    attach(roles, signal(4), 42000);
    auto oc = signal(5); oc.overCurrent = true; b.calls.clear();
    assert(roles.update(oc, 62001) == Result::Success && !b.dma && !b.sourceEnabled);
    assert((b.calls == std::vector<std::string>{"off","stop","safe-off"}));
}

static void stopFailureAndMalformedRoutes() {
    Backend b; USB::RoleSwitch<Backend> roles(b);
    attach(roles, signal(1), 0); b.calls.clear(); b.stopOK = false;
    assert(roles.update(signal(2), 21000) == Result::Timeout && roles.failed());
    assert((b.calls == std::vector<std::string>{"off","stop"}));
    assert(b.dma && roles.current() == Role::Host && !roles.accepts(roles.epoch()));
    b.calls.clear(); assert(roles.update(signal(3), 41000) == Result::NotReady && b.calls.empty());
    b.stopOK = true; b.dma = false; // external isolation before explicit recovery
    assert(roles.resetAfterIsolation() == Result::Success);
    attach(roles, signal(3), 50000); b.calls.clear();
    auto invalid = signal(3, true, PowerRole::Source, Orientation::Reverse); // same generation changed routing
    assert(roles.update(invalid, 71000) == Result::Invalid && roles.failed());
    assert((b.calls == std::vector<std::string>{"off","stop","safe-off"}) && !b.dma);
    Backend off; USB::RoleSwitch<Backend> offRoles(off); attach(offRoles, signal(1), 0);
    off.calls.clear(); off.off = Result::HardwareError;
    assert(offRoles.update({}, 21000) == Result::HardwareError);
    assert((off.calls == std::vector<std::string>{"off","stop"}) && !off.dma); // still attempt halt
    Backend clock; USB::RoleSwitch<Backend> clockRoles(clock); attach(clockRoles, signal(1), 100);
    assert(clockRoles.update(signal(1), 20000) == Result::Invalid && !clock.dma && clockRoles.failed());
}

static void staleTransitionRollback() {
    for (unsigned phase = 0; phase < 4; ++phase) {
        Backend b; USB::RoleSwitch<Backend> roles(b);
        if (phase == 0) b.power = Result::Aborted;
        if (phase == 1) b.prepare = Result::Aborted;
        if (phase == 2) b.on = Result::Aborted;
        if (phase == 3) b.publish = Result::Aborted;
        assert(roles.update(signal(1), 0) == Result::Busy);
        assert(roles.update(signal(1), 20000) == Result::Busy && !roles.failed());
        assert(!b.dma && !b.sourceEnabled && roles.current() == Role::None);
        const auto n = b.calls.size();
        assert(n >= 3 && b.calls[n-3] == "off" && b.calls[n-2] == "stop" && b.calls[n-1] == "safe-off");
        b.power = b.prepare = b.on = b.publish = Result::Success;
        attach(roles, signal(2), 21000);
    }
    Backend b; USB::RoleSwitch<Backend> roles(b); b.publish = Result::Aborted; b.stopOK = false;
    assert(roles.update(signal(1), 0) == Result::Busy);
    assert(roles.update(signal(1), 20000) == Result::HardwareError && roles.failed() && b.dma);
    assert(roles.current() == Role::Host);
}

static void notificationFences() {
    USB::ConnectorFence fence;
    auto first = signal(1);
    assert(fence.sample(first, true) == Result::Success);
    const auto stamp = fence.epoch(); assert(fence.accepts(first, stamp));
    fence.notice(3); assert(!fence.accepts(first, stamp));
    assert(fence.sample(first, true) == Result::Busy && !fence.accepts(first));
    auto next = signal(3, true, PowerRole::Source, Orientation::Reverse);
    assert(fence.sample(next, true) == Result::Success && fence.takeInterruption());
    const auto latest = fence.epoch(); assert(fence.accepts(next, latest));
    fence.notice(1); fence.notice(3); assert(fence.accepts(next, latest)); // old/benign repeat notice
    fence.notice(0); assert(!fence.accepts(next, latest));
    assert(fence.sample(next, true) == Result::Success && fence.takeInterruption());
    assert(fence.accepts(next) && !fence.accepts(next, latest)); // never reopen old nub after a transient fault
    auto malformed = next; malformed.host = false;
    assert(fence.sample(malformed, true) == Result::Invalid && !fence.accepts(next));
    Backend b; USB::RoleSwitch<Backend> roles(b); USB::ConnectorFence shared;
    assert(shared.sample(first, true) == Result::Success);
    attach(roles, first, 0); const auto roleEpoch = roles.epoch(), admission = shared.epoch();
    shared.notice(0); // a transient provider fault closes the old child
    assert(!shared.accepts(first, admission));
    assert(shared.sample(first, true) == Result::Success); // recovered before the role worker's next sample
    b.calls.clear();
    assert(roles.update(first, 21000, shared.takeInterruption()) == Result::Busy && !b.dma);
    assert((b.calls == std::vector<std::string>{"off","stop","safe-off"}));
    assert(!roles.accepts(roleEpoch) && !shared.accepts(first, admission));
    assert(roles.update(first, 41000) == Result::Success && roles.epoch() != roleEpoch);
}

static void platformTargets() {
    USB::PlatformRequest request;
    request.lease = 7; request.action = USB::PlatformAction::Prepare; request.next = Role::Host;
    assert(!USB::validPlatformRequest(request));
    request.connector = signal(2);
    assert(USB::validPlatformRequest(request));
    assert(USB::platformTargetCurrent(request, signal(2)));
    assert(!USB::platformTargetCurrent(request, signal(3)));
    assert(!USB::platformTargetCurrent(request, signal(2, true, PowerRole::Source, Orientation::Reverse)));
    request.action = USB::PlatformAction::Source; request.source = true;
    assert(USB::validPlatformRequest(request));
    request.connector.power = PowerRole::Sink;
    assert(!USB::validPlatformRequest(request));
    request.connector.vbus = true; request.action = USB::PlatformAction::Validate; request.source = false;
    assert(USB::validPlatformRequest(request)); // a Host may be a power sink
    auto noVbus = request.connector; noVbus.vbus = false;
    assert(!USB::platformTargetCurrent(request, noVbus));
    request.action = USB::PlatformAction::Source; request.next = Role::None; request.connector = {};
    assert(USB::platformTargetCurrent(request, {})); // unknown connector never prevents source removal
    for (auto action:{USB::PlatformAction::Suspend,USB::PlatformAction::Resume,USB::PlatformAction::Isolate}) {
        request.action=action;assert(USB::validPlatformRequest(request));
        assert(USB::validPlatformResponse(request,action,7));
        request.source=true;assert(!USB::validPlatformRequest(request));request.source=false;
        request.next=Role::Device;assert(!USB::validPlatformRequest(request));request.next=Role::None;
    }
    request.version = 1; assert(!USB::validPlatformRequest(request));
}

struct PowerBackend {
    Backend hardware;
    USB::RoleSwitch<Backend> roles{hardware};
    std::vector<std::string> calls;
    Result suspendResult=Result::Success,resumeResult=Result::Success,restartResult=Result::Success;
    bool admitted=true,drained=false,resources=true,fault=false;
    void closePowerAdmission() { calls.push_back("close");admitted=false; }
    void drainRoleWork() { assert(!admitted);calls.push_back("join");drained=true; }
    Result parkRole() { assert(drained);calls.push_back("park");return roles.suspend(); }
    Result suspendResources() {
        assert(drained && !admitted && roles.suspended() && !hardware.dma && !hardware.sourceEnabled);
        calls.push_back("release");resources=false;return suspendResult;
    }
    Result resumeResources() {
        assert(!admitted && roles.suspended());calls.push_back("acquire");
        if (resumeResult!=Result::NotReady && resumeResult!=Result::Busy) resources=true;
        return resumeResult;
    }
    Result restartRole() {
        assert(resources && !fault);calls.push_back("sample");
        if (restartResult!=Result::Success) return restartResult;
        assert(roles.resume()==Result::Success);admitted=true;drained=false;return Result::Success;
    }
    void powerFault() { calls.push_back("fault");fault=true;admitted=false; }
};
static void controllerPowerLifecycle() {
    for (bool host:{false,true}) for (auto power:{PowerRole::Sink,PowerRole::Source}) {
        PowerBackend b;USB::ControllerPower<PowerBackend> pm(b);
        attach(b.roles,signal(7,host,power),100);const auto old=b.roles.epoch();b.hardware.calls.clear();
        assert(pm.suspend()==Result::Success && pm.suspended() && !b.resources && !b.admitted);
        assert((b.calls==std::vector<std::string>{"close","join","park","release"}));
        assert((b.hardware.calls==std::vector<std::string>{"off","stop","safe-off"}));
        assert(!b.roles.accepts(old) && b.roles.update(signal(8),30000)==Result::NotReady);
        const auto count=b.calls.size();assert(pm.suspend()==Result::Success && b.calls.size()==count);
        b.resumeResult=Result::NotReady;assert(pm.resume()==Result::NotReady && pm.suspended() && !pm.failed());
        b.resumeResult=Result::Busy;assert(pm.resume()==Result::Busy && !b.resources && !b.admitted);
        b.resumeResult=Result::Success;assert(pm.resume()==Result::Success && b.resources && b.admitted);
        assert(b.roles.current()==Role::None && !b.roles.accepts(old));
        // A fresh connector resource lease can start its own observation
        // serial at1. It cannot reopen the old controller incarnation.
        attach(b.roles,signal(1,!host,PowerRole::Sink),40000);assert(b.roles.epoch()>old);
    }
    for (unsigned failure=0;failure<6;++failure) {
        PowerBackend b;USB::ControllerPower<PowerBackend> pm(b);attach(b.roles,signal(1),100);
        if (failure==0) b.hardware.off=Result::HardwareError;
        if (failure==1) b.hardware.stopOK=false;
        if (failure==2) b.hardware.safeOff=Result::HardwareError;
        if (failure==3) b.suspendResult=Result::HardwareError;
        if (failure<4) {
            assert(pm.suspend()!=Result::Success && pm.failed() && b.fault && !b.admitted);
            if (failure<3) assert(b.resources); // clocks are never released without the complete stop proof
            assert(!b.hardware.calls.empty() && b.hardware.calls[5]=="off");
            // Even failed source-off still attempts the DMA stop.
            if (!failure) assert(!b.hardware.dma && b.hardware.sourceEnabled);
        } else {
            assert(pm.suspend()==Result::Success);
            if (failure==4) b.resumeResult=Result::HardwareError;else b.restartResult=Result::HardwareError;
            assert(pm.resume()==Result::HardwareError && pm.failed() && b.fault && !b.admitted);
        }
        const auto count=b.calls.size();
        assert(pm.resume()==Result::NotReady && pm.suspend()==Result::NotReady && b.calls.size()==count);
    }
}

int main() {
    debounceAndPowerRoles(); sameRoleReplacement(); roleSwapAndSessionLoss(); stopFailureAndMalformedRoutes(); staleTransitionRollback();
    notificationFences(); platformTargets();controllerPowerLifecycle();
    std::puts("USB OTG: connection epochs, direction, independent power roles, immediate quiescence, stale RPC rollback and notification admission passed");
}
