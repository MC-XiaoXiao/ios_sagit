// SPDX-License-Identifier: BSD-2-Clause
#pragma once
#include "DWC3Control.h"

namespace DWC3 {
// A USB bus reset must keep DCTL.Run asserted. It is not a core/role reset.
// Preserve an armed EP0 SETUP TD so USBRST, CONNECT_DONE and the first SETUP
// in the same event snapshot cannot be lost. Other control stages and data
// endpoints need their real END completions before reuse. The adapter keeps
// DMA mappings live and continues consuming events while this object waits.
template<class Controller, class EP0, class TD> class DeviceReset {
    Controller &controller;
    EP0 &control;
    TD *data[30]{};
    unsigned count=0;
    bool pending = false, restartControl = false;
    uint64_t deadline = 0;
    uint32_t stalled = 0;
    static bool accepted(USB::Result r) { return r == USB::Result::Success || r == USB::Result::Pending; }
public:
    DeviceReset(Controller &core, EP0 &ep0, TD &out, TD &in, TD &notify) :
        controller(core), control(ep0), data{&out, &in, &notify},count(3) {}
    DeviceReset(Controller &core,EP0 &ep0):controller(core),control(ep0) {}
    bool add(TD &endpoint) {
        if(pending || count==30 || endpoint.physicalEndpoint()<2 || endpoint.physicalEndpoint()>31)return false;
        for(unsigned n=0;n<count;++n)if(data[n]->physicalEndpoint()==endpoint.physicalEndpoint())return false;
        data[count++]=&endpoint;return true;
    }
    bool active() const { return pending; }
    bool stoppingControl() const { return pending && restartControl; }
    void isolated() { pending = restartControl = false; deadline = 0; stalled = 0; }
    USB::Result begin(uint64_t now, uint32_t stalledEndpoints) {
        if (!pending) deadline = now + 10000;
        pending = true; stalled |= stalledEndpoints;
        restartControl = control.currentState() != ControlState::Setup;
        if (restartControl) {
            ControlNotification ignored;
            const auto r = control.stop(ignored);
            if (!accepted(r)) return r;
        }
        for (unsigned n=0;n<count;++n) if (auto *td=data[n];td->ownsBuffers() && td->currentState() != TransferState::Stopping) {
            const auto r = td->cancel();
            if (!accepted(r)) return r;
        }
        return USB::Result::Pending;
    }
    USB::Result finish(uint64_t now, USB::Speed speed) {
        if (!pending) return USB::Result::Success;
        bool waiting = restartControl && control.currentState() != ControlState::Stopped;
        for (unsigned n=0;n<count;++n) waiting |= data[n]->ownsBuffers();
        if (waiting) return now >= deadline ? USB::Result::Timeout : USB::Result::Pending;
        for (unsigned n=0;n<count;++n) if (auto *td=data[n];stalled & (1u << td->physicalEndpoint())) {
            uint32_t reply = 0;
            const auto r = controller.command(td->physicalEndpoint(), Command::ClearStall, 0, 0, 0, 0, reply);
            if (r != USB::Result::Success) return r;
        }
        auto r = controller.beginEndpointConfiguration(2);
        if (r == USB::Result::Success) r = controller.setAddress(0);
        // A just-completed SETUP is in Dispatch but has no data/status DMA.
        // negotiate permits this idle state without discarding its token.
        if (r == USB::Result::Success) r = control.negotiate(speed);
        if (r == USB::Result::Success && restartControl) r = control.start();
        if (r == USB::Result::Success) isolated();
        return r;
    }
};
}
