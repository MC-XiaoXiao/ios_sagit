// SPDX-License-Identifier: BSD-2-Clause
#pragma once
#include "USBProtocol.h"
namespace USB {
// One recovery attempt per failed cleanup. Hardware isolation cannot justify
// replaying a broken queue acknowledgement or an uncertain Family handoff.
class CleanupRecovery {
    enum class Phase:uint8_t { None,Pending,Used,Blocked } phase=Phase::None;
public:
    void failed(bool retryable) {
        phase=retryable && (phase==Phase::None || phase==Phase::Pending) ? Phase::Pending:Phase::Blocked;
    }
    bool retry(bool rootDetached) {
        if (!rootDetached || phase!=Phase::Pending) return false;
        phase=Phase::Used;return true;
    }
};
// A composite MD can complete some children and still report an error. The
// Native API does not authorize retrying that preparation as a whole. Tests
// with an explicitly non-consuming failure can retain their retry contract.
class MemoryCompletionState {
    enum class Phase:uint8_t { Empty,Active,Faulted } phase=Phase::Empty;
    Result error=Result::HardwareError;
public:
    void prepared(Result result) {
        if (phase!=Phase::Empty) {phase=Phase::Faulted;return;}
        if (result==Result::Success) phase=Phase::Active;
    }
    bool faulted() const {return phase==Phase::Faulted;}
    template<class Complete> Result complete(Complete callback,bool preservesOnError) {
        if (phase==Phase::Empty) return Result::Success;
        if (phase==Phase::Faulted) return error;
        const auto status=callback();
        if (status==Result::Success) phase=Phase::Empty;
        else if (!preservesOnError) {error=status;phase=Phase::Faulted;}
        return status;
    }
};
// Keep native command completion separate from descriptor clearing. Native
// clear(autoComplete=true) discards complete() errors; complete() consumes its
// preparation even if unmapping fails, so an unknown result must stay sticky.
class DMACommandState {
    enum class Phase:uint8_t { Empty, Attempted, Active, Completed, Faulted } phase=Phase::Empty;
public:
    void prepared(Result result,bool mapped) {
        if (phase!=Phase::Empty) { phase=Phase::Faulted; return; }
        if (result==Result::Success) phase=Phase::Active;
        else if (mapped && (result==Result::HardwareError || result==Result::Timeout)) phase=Phase::Faulted;
        else phase=Phase::Attempted;
    }
    bool faulted() const { return phase==Phase::Faulted; }
    template<class Proof> Result recover(Proof rootDetachedAndAcknowledged) {
        if (phase!=Phase::Faulted) return Result::Invalid;
        const auto status=rootDetachedAndAcknowledged();
        if (status==Result::Success) phase=Phase::Completed;
        return status;
    }
    template<class Complete,class Clear> Result clear(Complete complete,Clear clearWithoutCompletion) {
        if (phase==Phase::Faulted) return Result::HardwareError;
        if (phase==Phase::Active || phase==Phase::Attempted) {
            const auto result=complete();
            if (result!=Result::Success && !(result==Result::NotReady && phase==Phase::Attempted)) {
                phase=Phase::Faulted; return result;
            }
            phase=Phase::Completed;
        }
        const auto result=clearWithoutCompletion();
        if (result==Result::Success) phase=Phase::Empty;
        return result;
    }
};
}
