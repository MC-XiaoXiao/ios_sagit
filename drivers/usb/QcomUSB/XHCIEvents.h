// SPDX-License-Identifier: BSD-2-Clause
#pragma once
#include "XHCICommands.h"
#include "XHCIFrames.h"

namespace XHCI {
struct EventBatch {
    unsigned count=0,commands=0,transfers=0,ports=0,frameRollovers=0;
    bool more=false,interruptPending=false;
};

// Runs only within the controller/role gates. Sink callbacks update HCD
// metadata only: no Native Family calls, releases, allocation or DMA cleanup.
// A successfully consumed command remains retained in CommandQueue until its
// client acknowledges it. A fault never frees quarantined command payloads.
template<class Core,class Ring,class Commands,class Sink>
USB::Result drainEvents(Core &core,Ring &ring,Commands &commands,Sink &sink,
    uint64_t epoch,unsigned budget,EventBatch &batch) {
    batch={};
    if (!epoch || epoch!=commands.generation() || !budget || budget>4096)
        return USB::Result::Invalid;
    auto fail=[&](USB::Result reason) {
        commands.quarantine(reason);
        core.maskInterrupts();
        return reason;
    };
    uint32_t status=0;
    auto result=core.interruptStatus(status,batch.interruptPending);
    if (result!=USB::Result::Success) return fail(result);
    TRB event{};
    while (batch.count<budget && ring.peek(event)) {
        switch (event.type()) {
        case CommandEvent: {
            CommandRecord record{};
            result=commands.complete(event,epoch,record);
            if (result==USB::Result::Success) {
                sink.command(record);++batch.commands;
            }
            break;
        }
        case PortEvent: {
            const auto port=event.lo>>24;
            if ((event.lo&0xffffff) || event.hi || event.status!=(1u<<24) ||
                (event.control&~(Cycle|(63u<<10))) || !port || port>core.maxPorts())
                result=USB::Result::HardwareError;
            else {
                result=sink.port(port);
                if (result==USB::Result::Success) ++batch.ports;
            }
            break;
        }
        case TransferEvent:
            result=sink.transfer(event,epoch);
            if (result==USB::Result::Success) ++batch.transfers;
            break;
        case MicroFrameWrapEvent: {
            if (!validMicroFrameWrapEvent(event)) result=USB::Result::HardwareError;
            else {
                uint32_t raw=0;
                result=core.microFrameCount(raw);
                if (result==USB::Result::Success) result=sink.frameRollover(raw);
                if (result==USB::Result::Success) ++batch.frameRollovers;
            }
            break;
        }
        default:result=USB::Result::HardwareError;break;
        }
        if (result!=USB::Result::Success) return fail(result);
        ring.consume();++batch.count;
    }
    batch.more=ring.peek(event);
    // Hardware can post after the initial filter/timer scan. Advance the
    // read-only frame observer through everything just consumed before ERDP
    // allows hardware to overwrite those slots in the next cycle.
    result=sink.prepareEventAcknowledgement();
    if (result!=USB::Result::Success) return fail(result);
    // Even an empty/spurious level interrupt must release EHB. A full budget
    // returns its consumed slots and lets hardware signal the remaining work.
    result=core.acknowledgeEvents(ring.dequeueAddress());
    return result==USB::Result::Success ? result : fail(result);
}
}
