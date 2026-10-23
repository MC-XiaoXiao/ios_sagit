// SPDX-License-Identifier: BSD-2-Clause
#pragma once
#include <IOKit/IOService.h>
#include <IOKit/IOBufferMemoryDescriptor.h>
#include "AudioOutputSession.h"

// A board/DSP implementation must prove the complete output path before
// publishing this provider. QcomADSP implements the retained-firmware route.
class AudioOutputBackend : public IOService {
    OSDeclareAbstractStructors(AudioOutputBackend);
public:
    // Called after exclusive open. Only a live route with clocks, DMA mapping,
    // codec configuration and speaker calibration may return OK.
    virtual AudioOutput::Result outputConfiguration(AudioOutput::Configuration &) = 0;
    // No sample DMA before startOutput. Retain the descriptor on success.
    // Ordinary failure leaves no resources; Retained explicitly reports an
    // uncertain map/close that still owns memory. The frontend must retain its
    // own state until releaseOutput succeeds. Memory contains untrusted PCM.
    virtual AudioOutput::Result prepareOutput(IOBufferMemoryDescriptor *,
                                             const AudioOutput::Configuration &) = 0;
    virtual AudioOutput::Result startOutput(uint64_t generation) = 0;
    // Synchronous mute/drain/barrier. OK guarantees no more DMA or callbacks.
    // It must be safe after a partially failed start and repeatable on failure.
    virtual AudioOutput::Result stopOutput() = 0;
    virtual AudioOutput::Result readPosition(AudioOutput::Position &) = 0;
    // Teardown mappings and release the descriptor; only called after drain.
    // Failure means mappings/descriptors must remain retained.
    virtual AudioOutput::Result releaseOutput() = 0;
};
