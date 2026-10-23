// SPDX-License-Identifier: BSD-2-Clause
#pragma once
#include "USBProtocol.h"

namespace USB {
// USB CDC 1.10, PSTN ACM 1.2. This is one exclusive USB2 debug personality,
// with communication interface 0 and data interface 1; no Apple ABI is used.
class CDCACM {
public:
    static constexpr unsigned Out = 2, In = 3, Notify = 5, BufferBytes = 512;
    enum class Action { Stall, Reply, ReceiveLineCoding, Status, Address, Configuration, Halt, Interface };
    struct Response { Action action = Action::Stall; unsigned length = 0, value = 0, endpoint = 0; };
private:
    uint16_t vendor, product;
    uint8_t configuration = 0, address = 0;
    uint16_t controlLines = 0;
    uint8_t coding[7] = {0x00, 0xc2, 0x01, 0, 0, 0, 8}; // 115200 8N1
    uint8_t stagedCoding[7]{};
    uint32_t halted = 0;
    bool notification = false, codingPending = false;
    static unsigned minimum(unsigned a, unsigned b) { return a < b ? a : b; }
    static void copy(uint8_t *out, const uint8_t *in, unsigned length) {
        for (unsigned i = 0; i < length; ++i) out[i] = in[i];
    }
    Response reply(uint8_t *out, const uint8_t *data, unsigned length, unsigned requested) const {
        length = minimum(length, requested); copy(out, data, length);
        return {Action::Reply, length};
    }
    Response string(uint8_t *out, const char *text, unsigned requested) const {
        unsigned count = 0; while (text[count] && count < 63) ++count;
        out[0] = uint8_t(2 + count * 2); out[1] = 3;
        for (unsigned i = 0; i < count; ++i) { out[2 + i * 2] = uint8_t(text[i]); out[3 + i * 2] = 0; }
        return {Action::Reply, minimum(2 + count * 2, requested)};
    }
public:
    CDCACM(uint16_t vid, uint16_t pid) : vendor(vid), product(pid) {}
    static bool validIdentity(unsigned vid, unsigned pid) {
        return vid > 0 && vid < 0xffff && pid > 0 && pid <= 0xffff;
    }
    template<class Sink> static Result drainReceive(const uint8_t *bytes, unsigned length,
                                                    unsigned &offset, Sink sink) {
        if (!bytes || length > BufferBytes || offset > length) return Result::Invalid;
        const unsigned accepted = sink(bytes + offset, length - offset);
        if (accepted > length - offset) return Result::Invalid;
        offset += accepted;
        return offset == length ? Result::Success : Result::Pending;
    }
    static unsigned endpoint(unsigned address) {
        if (address == 0x01) return Out;
        if (address == 0x81) return In;
        if (address == 0x82) return Notify;
        return 32;
    }
    void reset() { configuration = address = 0; controlLines = 0; halted = 0; notification = codingPending = false; }
    void abortControl() { codingPending = false; }
    unsigned configured() const { return configuration; }
    bool terminalOpen() const { return configuration && (controlLines & 1); }
    bool stalled(unsigned ep) const { return ep < 32 && (halted & (1u << ep)); }
    bool needsNotification() const { return configuration && notification; }
    void notified() { notification = false; }
    void requestNotification() { notification = true; }
    void serialState(uint8_t *out, bool overrun = false) const {
        const uint8_t data[] = {0xa1, 0x20, 0, 0, 0, 0, 2, 0,
            uint8_t((terminalOpen() ? 3 : 0) | (overrun ? 0x40 : 0)), 0};
        copy(out, data, sizeof(data));
    }
    // State changes are committed after the successful status stage.
    void commit(const SetupPacket &setup) {
        if (setup.requestType == 0x21 && setup.request == 0x20 && codingPending) copy(coding, stagedCoding, 7);
        codingPending = false;
        if (setup.requestType == 0 && setup.request == 5) address = uint8_t(setup.value);
        if (setup.requestType == 0 && setup.request == 9) {
            configuration = uint8_t(setup.value); controlLines = 0; halted = 0; notification = !!configuration;
        }
        if (setup.requestType == 0x21 && setup.request == 0x22) {
            controlLines = setup.value; notification = true;
        }
        if (setup.requestType == 2 && (setup.request == 1 || setup.request == 3)) {
            const unsigned ep = endpoint(setup.index);
            if (ep < 32) {
                if (setup.request == 3) halted |= 1u << ep;
                else halted &= ~(1u << ep);
            }
        }
        if (setup.requestType == 1 && setup.request == 11) {
            halted &= setup.index ? ~((1u << Out) | (1u << In)) : ~(1u << Notify);
        }
    }
    bool stageLineCoding(const uint8_t *data, unsigned length) {
        // CDC permits 5, 6, 7, 8 or 16 data bits, three stop formats, five
        // parity formats. Speed is informational, including a hangup value 0.
        if (!data || length != 7 || data[4] > 2 || data[5] > 4 ||
            (data[6] != 5 && data[6] != 6 && data[6] != 7 && data[6] != 8 && data[6] != 16)) return false;
        copy(stagedCoding, data, 7); codingPending = true; return true;
    }
    Response setup(const SetupPacket &s, Speed speed, uint8_t *out) const {
        if (!out || (speed != Speed::Full && speed != Speed::High)) return {};
        const unsigned type = s.value >> 8, index = s.value & 255;
        if (s.requestType == 0x80 && s.request == 6) {
            if (type != 3 && s.index) return {};
            if (type == 1 && !index) {
                const uint8_t data[] = {18, 1, 0, 2, 2, 0, 0, 64,
                    uint8_t(vendor), uint8_t(vendor >> 8), uint8_t(product), uint8_t(product >> 8),
                    0, 1, 1, 2, 0, 1};
                return reply(out, data, sizeof(data), s.length);
            }
            if ((type == 2 || type == 7) && !index) {
                const bool high = (speed == Speed::High) != (type == 7);
                const unsigned packet = high ? 512 : 64;
                const uint8_t data[] = {
                    9, uint8_t(type), 67, 0, 2, 1, 0, 0x80, 50,
                    9, 4, 0, 0, 1, 2, 2, 1, 0,
                    5, 0x24, 0, 0x10, 1, // CDC header
                    5, 0x24, 1, 0, 1, // call management: no modem call handling
                    4, 0x24, 2, 2, // ACM: line coding/control lines + serial state
                    5, 0x24, 6, 0, 1, // union
                    7, 5, 0x82, 3, 16, 0, uint8_t(high ? 9 : 16),
                    9, 4, 1, 0, 2, 0x0a, 0, 0, 0,
                    7, 5, 0x01, 2, uint8_t(packet), uint8_t(packet >> 8), 0,
                    7, 5, 0x81, 2, uint8_t(packet), uint8_t(packet >> 8), 0};
                return reply(out, data, sizeof(data), s.length);
            }
            if (type == 6 && !index) {
                const uint8_t data[] = {10, 6, 0, 2, 2, 0, 0, 64, 1, 0};
                return reply(out, data, sizeof(data), s.length);
            }
            if (type == 3) {
                if (!index && !s.index) { const uint8_t data[] = {4, 3, 9, 4}; return reply(out, data, 4, s.length); }
                if (s.index != 0x0409) return {};
                if (index == 1) return string(out, "MI6 iOS development", s.length);
                if (index == 2) return string(out, "Kernel USB console", s.length);
            }
            return {};
        }
        if (s.requestType == 0 && s.request == 5 && !s.index && !s.length && s.value < 128 && !configuration)
            return {Action::Address, 0, s.value};
        if (s.requestType == 0 && s.request == 9 && !s.index && !s.length && s.value <= 1 && address)
            return {Action::Configuration, 0, s.value};
        if (s.requestType == 0x80 && s.request == 8 && !s.value && !s.index && s.length == 1 && address) {
            out[0] = configuration; return {Action::Reply, 1};
        }
        if (s.request == 0 && !s.value && s.length == 2) {
            out[0] = out[1] = 0;
            if (s.requestType == 0x80 && !s.index) return {Action::Reply, 2};
            if (s.requestType == 0x81 && configuration && s.index < 2) return {Action::Reply, 2};
            if (s.requestType == 0x82 && (s.index == 0 || s.index == 0x80)) return {Action::Reply, 2};
            const unsigned ep = endpoint(s.index);
            if (s.requestType == 0x82 && configuration && ep < 32) {
                out[0] = stalled(ep); return {Action::Reply, 2};
            }
        }
        if (s.requestType == 2 && (s.request == 1 || s.request == 3) && !s.value && !s.length && configuration) {
            const unsigned ep = endpoint(s.index);
            if (ep < 32) return {Action::Halt, 0, unsigned(s.request == 3), ep};
        }
        if (s.requestType == 0x81 && s.request == 10 && !s.value && s.index < 2 && s.length == 1 && configuration) {
            out[0] = 0; return {Action::Reply, 1};
        }
        if (s.requestType == 1 && s.request == 11 && !s.value && s.index < 2 && !s.length && configuration)
            return {Action::Interface, 0, s.index};
        if (configuration && s.index == 0) {
            if (s.requestType == 0xa1 && s.request == 0x21 && !s.value && s.length == 7)
                return reply(out, coding, 7, s.length);
            if (s.requestType == 0x21 && s.request == 0x20 && !s.value && s.length == 7)
                return {Action::ReceiveLineCoding, 7};
            if (s.requestType == 0x21 && s.request == 0x22 && s.value <= 3 && !s.length)
                return {Action::Status};
        }
        return {};
    }
};
}
