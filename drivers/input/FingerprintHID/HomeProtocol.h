// SPDX-License-Identifier: BSD-2-Clause
#pragma once
#include <stdint.h>
#include <stddef.h>

namespace QcomHomeProtocol {
// Consumer Menu is the embedded Home key, distinct from keyboard Home (0x4a)
// and the browser's AC Home (0x223). IOHIDEventDriver emits keyboard events.
constexpr uint8_t ReportDescriptor[] = {
    0x05,0x0c,0x09,0x01,0xa1,0x01,0x15,0x00,0x25,0x01,
    0x09,0x40,0x75,0x01,0x95,0x01,0x81,0x02,
    0x75,0x07,0x95,0x01,0x81,0x03,0xc0
};
inline uint32_t read32(const void *p) {
    const auto *b=static_cast<const uint8_t *>(p);
    return uint32_t(b[0])|uint32_t(b[1])<<8|uint32_t(b[2])<<16|uint32_t(b[3])<<24;
}
inline void write32(void *p,uint32_t v) {
    auto *b=static_cast<uint8_t *>(p);
    for (unsigned i=0;i<4;++i) b[i]=uint8_t(v>>(i*8));
}
enum class Result { Success, Invalid, Transport, Busy, Unsupported, Sensor };
enum class Variant { None, FPC, Goodix };
// All three measured Milan HAL branches share the same init/IRQ/navigation
// messages. Oswego M (0) additionally downloads a configuration and is rejected.
inline bool goodixProfileSupported(uint32_t profile) { return profile>=1 && profile<=3; }
class State {
    bool fDesired=false,fReported=false;
public:
    void update(bool down,bool changed) { if (changed) fDesired=down; }
    void release() { fDesired=false; }
    bool desired() const { return fDesired; }
    bool reported() const { return fReported; }
    bool pending() const { return fDesired!=fReported; }
    void delivered(bool down) { fReported=down; }
};
// The HAL's sensor_command(group 10, command 0) tests finger LOST.
// Its successful result is 0 for present and 1 for absent; errors are signed.
inline Result fpcPresence(int32_t status,bool &down) {
    if (status==-12) return Result::Busy;
    if (status!=0 && status!=1) return Result::Sensor;
    down=status==0; return Result::Success;
}
// Only the two FINGER_DOWN/UP flags represent touch state. Image-ready,
// reset, navigation motion and other interrupt causes never become Home.
inline Result goodixPresence(uint32_t flags,bool &down,bool &changed) {
    changed=false;
    if ((flags&6)==6) return Result::Sensor; // Ambiguous coalesced state.
    if (flags&2) { down=true; changed=true; }
    if (flags&4) { down=false; changed=true; }
    return Result::Success;
}
// fpc_tac_alloc_shared rounds its 608-byte logical message to a 4 KiB ION
// allocation; fpc_tac_transfer passes that allocation length in the envelope.
constexpr unsigned FPCPayloadSize=4096;
constexpr unsigned GoodixIRQSize=223776;
constexpr unsigned GoodixIRQType=96;
constexpr unsigned GoodixIRQStage=223768;
// Small, independently checked codecs for the original HAL wire protocols.
inline bool fpcRequest(void *payload,size_t length,uint32_t group,uint32_t command) {
    if (!payload || length<FPCPayloadSize) return false;
    auto *b=static_cast<uint8_t *>(payload);
    for (size_t i=0;i<length;++i) b[i]=0;
    write32(b,group); write32(b+4,command); write32(b+8,0xffffffff);
    return true;
}
inline bool goodixEnvelope(void *request,size_t length,uint32_t sequence,uint32_t command,
    uint32_t payloadAddress,uint32_t payloadSize,uint64_t milliseconds) {
    if (!request || length!=64 || !payloadAddress || !payloadSize) return false;
    auto *b=static_cast<uint8_t *>(request);
    for (size_t i=0;i<length;++i) b[i]=0;
    write32(b,sequence); write32(b+4,1); // HAL command target (not authentication).
    write32(b+8,uint32_t(milliseconds)); write32(b+12,uint32_t(milliseconds>>32));
    write32(b+16,command); write32(b+20,payloadAddress); write32(b+24,payloadSize);
    return true;
}
}
