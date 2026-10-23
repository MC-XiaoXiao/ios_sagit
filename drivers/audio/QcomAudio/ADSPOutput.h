// SPDX-License-Identifier: GPL-2.0-only
#pragma once
#include "ADSPProtocol.h"
#include <AudioOutputTypes.h>

namespace ADSP {
// The retained board route is an already configured AFE/MI2S/calibrated amp
// and ADM COPP. This implementation owns a fresh ASM session and maps only
// its explicitly reserved, DSP-addressable PCM window. It never treats an
// arbitrary host physical address as an ADSP IOVA.
class Output {
    static constexpr uint32_t RingFrames = 4096, PeriodFrames = 512, PeriodBytes = 2048;
    static constexpr unsigned Slots = 8, Ahead = 4;
    struct Slot { uint32_t token = 0; uint64_t sequence = 0; bool done = false; } slots[Slots];
    uint16_t port_ = 0, session_ = 0, copp_ = 0;
    uint32_t handle_ = 0;
    uint64_t address_ = 0, submitted_ = 0, retired_ = 0, rendered_ = 0, epoch_ = 0, baseline_ = 0, lastUs_ = 0;
    bool mapUnknown_ = false, open_ = false, active_ = false, faulted_ = false;
    template<class IO>
    Result simple(IO &io, uint8_t service, uint16_t port, uint32_t opcode,
                  const void *payload = nullptr, size_t bytes = 0) {
        uint8_t reply[64]{}; size_t length = sizeof(reply);
        return io.request(service, port, opcode, 0, payload, bytes, reply, length);
    }
    template<class IO> Result time(IO &io, uint64_t &us) {
        uint8_t reply[64]{}; size_t length = sizeof(reply);
        Result r = io.request(ASM, port_, SessionTime, SessionTimeResponse, nullptr, 0, reply, length);
        if (r != Result::OK) return r;
        if (length != 20 || read32(reply)) return Result::Invalid;
        us = uint64_t(read32(reply + 4)) | uint64_t(read32(reply + 8)) << 32;
        // DSP absolute time is in its AVTimer domain, not mach host ticks.
        // Do not publish it as a host timestamp without a clock conversion.
        return Result::OK;
    }
public:
    bool retained() const { return mapUnknown_ || handle_ || open_ || active_; }
    bool active() const { return active_; }
    bool faulted() const { return faulted_; }
    uint32_t handle() const { return handle_; }
    uint64_t generation() const { return epoch_; }
    template<class IO> Result prepare(IO &io, const Handoff &h) {
        if (retained() || faulted_) return Result::Busy;
        if (!validHandoff(h) || h.flags != OutputFlags) return Result::Invalid;
        address_ = h.pcmDSP; session_ = h.session; copp_ = h.copp;
        port_ = uint16_t(session_ << 8) | 1;
        uint8_t payload[24]{}, reply[64]{};
        write16(payload, 3); write16(payload + 2, 1); // 4K shared memory pool, one region
        write64(payload + 8, address_); write32(payload + 16, h.pcmBytes);
        size_t length = sizeof(reply);
        Result r = io.request(ASM, 0, Map, MapResponse, payload, 20, reply, length);
        if (r != Result::OK) {
            mapUnknown_ = io.uncertain(); faulted_ = mapUnknown_;
            return r;
        }
        if (length != 4 || !(handle_ = read32(reply))) {
            mapUnknown_ = faulted_ = true; return Result::Quarantined;
        }
        for (auto &b : payload) b = 0;
        write16(payload + 6, 16); write32(payload + 8, 0x10c68); write32(payload + 12, PCMFormat);
        open_ = true; // until a definite rejection or acknowledged CLOSE
        r = simple(io, ASM, port_, OpenWrite, payload, 16);
        if (r != Result::OK) {
            if (!io.uncertain()) open_ = false;
            faulted_ = io.uncertain(); return r;
        }
        for (auto &b : payload) b = 0;
        write32(payload, 20); write16(payload + 4, 2); write16(payload + 6, 16);
        write32(payload + 8, 48000); write16(payload + 12, 1);
        payload[16] = 1; payload[17] = 2; // front left / front right
        r = simple(io, ASM, port_, FormatUpdate, payload, 24);
        if (r != Result::OK) { faulted_ = io.uncertain(); return r; }
        for (auto &b : payload) b = 0;
        write32(payload + 4, 1); write16(payload + 8, session_);
        write16(payload + 10, 1); write16(payload + 12, copp_);
        r = simple(io, ADM, 0, MatrixMap, payload, 16);
        if (r != Result::OK) { faulted_ = io.uncertain(); return r; }
        uint64_t current = 0;
        r = time(io, current); // never advertise a synthetic timestamp fallback
        if (r != Result::OK) faulted_ = io.uncertain();
        return r;
    }
    template<class IO> Result start(IO &io, uint64_t generation) {
        if (!open_ || !handle_ || mapUnknown_ || faulted_) return Result::NotReady;
        if (active_ || !generation || generation <= epoch_) return Result::Invalid;
        Result r = time(io, baseline_);
        if (r != Result::OK) return r;
        lastUs_ = baseline_; submitted_ = retired_ = rendered_ = 0; epoch_ = generation;
        for (auto &s : slots) s = {};
        uint8_t payload[12]{};
        active_ = true; // RUN or subsequent send may publish before reporting failure
        r = simple(io, ASM, port_, Run, payload, sizeof(payload));
        if (r != Result::OK) return r;
        return fill(io);
    }
    template<class IO> Result fill(IO &io) {
        if (!active_ || faulted_) return Result::NotReady;
        while (submitted_ - retired_ < Ahead) {
            if (submitted_ == UINT64_MAX / PeriodFrames) return Result::Invalid;
            // WRITE_DONE may precede physical rendering. Never prefetch more
            // than the advertised 2048-frame safety offset ahead of the DSP
            // rendering clock, even if all memory buffers completed early.
            if ((submitted_ + 1) * PeriodFrames - rendered_ > Ahead * PeriodFrames) break;
            unsigned index = submitted_ % Slots;
            auto &slot = slots[index];
            if (slot.token) return Result::Invalid;
            Result r = io.copyPCM((submitted_ * PeriodFrames % RingFrames) * 4,
                                  index * PeriodBytes, PeriodBytes);
            if (r != Result::OK) return r;
            uint32_t token = 0;
            r = io.token(token);
            if (r != Result::OK) return r;
            uint8_t payload[32]{};
            write64(payload, address_ + index * PeriodBytes); write32(payload + 8, handle_);
            write32(payload + 12, PeriodBytes); write32(payload + 16, uint32_t(submitted_));
            r = io.send(ASM, port_, token, Write, payload, sizeof(payload));
            if (r == Result::Busy) return Result::OK; // no cursor published; retry later with a fresh token
            if (r != Result::OK) { faulted_ = true; return r; }
            slot = {token, submitted_, false}; ++submitted_;
        }
        return Result::OK;
    }
    Result completion(const Packet &p) {
        const bool writeResult = p.opcode == BasicResponse && p.type == Message::Response &&
            p.bytes == 8 && read32(p.payload) == Write;
        if (p.service != ASM || (!writeResult && (p.opcode != WriteDone || p.type != Message::Event)) ||
            p.destinationPort != port_ || p.sourcePort != port_) return Result::Stale;
        Slot *selected = nullptr;
        unsigned index = 0;
        for (unsigned i = 0; i < Slots; ++i) if (slots[i].token == p.token && p.token) {
            selected = &slots[i]; index = i; break;
        }
        if (!selected || !active_) return Result::Stale; // old completion or closed stream
        if (writeResult) {
            if (read32(p.payload + 4)) { faulted_ = true; return Result::IO; }
            return Result::OK; // an accepted WRITE still needs WRITE_DONE
        }
        if (selected->done || p.bytes != 16 ||
            (uint64_t(read32(p.payload)) | uint64_t(read32(p.payload + 4)) << 32) != address_ + index * PeriodBytes ||
            read32(p.payload + 8) != handle_ || read32(p.payload + 12)) {
            faulted_ = true; return Result::Invalid;
        }
        selected->done = true;
        while (retired_ < submitted_) {
            auto &s = slots[retired_ % Slots];
            if (!s.token || !s.done || s.sequence != retired_) break;
            s = {}; ++retired_;
        }
        return Result::OK;
    }
    template<class IO> Result position(IO &io, AudioOutput::Position &p) {
        p = {};
        if (!active_ || faulted_) return Result::NotReady;
        uint64_t us = 0;
        Result r = time(io, us);
        if (r != Result::OK) return r;
        if (us < baseline_ || us < lastUs_ || us - baseline_ > UINT64_MAX / 48) return Result::Invalid;
        uint64_t frames = ((us - baseline_) * 48) / 1000;
        if (frames > submitted_ * PeriodFrames) return Result::Invalid; // renderer underrun / invalid clock
        lastUs_ = us; rendered_ = frames;
        r = fill(io);
        if (r != Result::OK) return r;
        // Frames come from DSP session rendering time, never WRITE_DONE or a
        // software timer. Host time is the observation time of this response;
        // exact AVTimer-to-mach correlation still requires a board clock hook.
        p = {epoch_, frames, io.now()};
        return Result::OK;
    }
    template<class IO> Result stop(IO &io) {
        if (!active_) return Result::OK;
        if (io.uncertain()) return Result::Quarantined;
        Result r = simple(io, ASM, port_, Pause);
        if (r != Result::OK) return r;
        r = simple(io, ASM, port_, Flush);
        if (r != Result::OK) return r;
        active_ = false;
        for (auto &s : slots) s = {};
        submitted_ = retired_ = 0;
        // A malformed data event need not prevent a confirmed PAUSE+FLUSH
        // from retiring DMA, but it still blocks subsequent start operations.
        return Result::OK;
    }
    template<class IO> Result release(IO &io) {
        if (mapUnknown_ || io.uncertain()) return Result::Quarantined;
        Result r = stop(io);
        if (r != Result::OK) return r;
        if (open_) {
            r = simple(io, ASM, port_, Close);
            if (r != Result::OK) return r;
            open_ = false;
        }
        if (handle_) {
            uint8_t payload[4]; write32(payload, handle_);
            r = simple(io, ASM, 0, Unmap, payload, sizeof(payload));
            if (r != Result::OK) return r;
            handle_ = 0;
        }
        return Result::OK;
    }
};
}
