// SPDX-License-Identifier: BSD-2-Clause
#pragma once
#include "USBProtocol.h"

namespace DWC3 {
enum class EndpointType : uint8_t { Control, Isochronous, Bulk, Interrupt };
struct EndpointConfiguration {
    unsigned physicalEndpoint;
    EndpointType type;
    USB::Speed speed;
    unsigned maximumPacketSize, interval, burst; // burst is bMaxBurst, 0..15
    unsigned mult=0; // additional transactions per service interval, 0..2

    bool valid() const {
        if (physicalEndpoint > 31 || !maximumPacketSize || maximumPacketSize > 1024 ||
            burst > 15 || mult>2 || (speed != USB::Speed::Super && burst) ||
            (speed == USB::Speed::Full && mult)) return false;
        if (speed != USB::Speed::Full && speed != USB::Speed::High && speed != USB::Speed::Super)
            return false;
        if (type == EndpointType::Control) {
            if (physicalEndpoint > 1 || interval || burst || mult) return false;
            if (speed == USB::Speed::Super) return maximumPacketSize == 512;
            if (speed == USB::Speed::High) return maximumPacketSize == 64;
            return maximumPacketSize == 8 || maximumPacketSize == 16 ||
                maximumPacketSize == 32 || maximumPacketSize == 64;
        }
        if (physicalEndpoint < 2) return false;
        if (type == EndpointType::Bulk) {
            if (mult) return false;
            if (speed == USB::Speed::Super) return !interval && maximumPacketSize == 1024;
            if (speed == USB::Speed::High) return !interval && maximumPacketSize == 512;
            return !interval && (maximumPacketSize == 8 || maximumPacketSize == 16 ||
                maximumPacketSize == 32 || maximumPacketSize == 64);
        }
        if (type == EndpointType::Interrupt) {
            if (!interval || interval > (speed == USB::Speed::Full ? 255u : 16u)) return false;
            return maximumPacketSize <= (speed == USB::Speed::Full ? 64u : 1024u);
        }
        if (type==EndpointType::Isochronous)
            return interval>=1 && interval<=16 && maximumPacketSize<=(speed==USB::Speed::Full ? 1023u:1024u);
        return false;
    }
    unsigned bytesPerInterval() const { return maximumPacketSize*(burst+1)*(mult+1); }
    unsigned intervalTicks() const { return interval>=1 && interval<=16 ? 1u<<(interval-1) : 0; }
    uint32_t parameter0(bool modify) const {
        return unsigned(type) << 1 | maximumPacketSize << 3 |
            ((physicalEndpoint & 1) ? physicalEndpoint / 2 << 17 : 0) |
            burst << 22 | (modify ? 2u << 30 : 0);
    }
    uint32_t parameter1() const {
        // One finite TD at a time uses XferComplete/LST. Control endpoints
        // also need XferNotReady to distinguish DATA and STATUS readiness.
        unsigned exponent = interval ? interval - 1 : 0;
        // The hardware field permits 0..13 even when the wire descriptor
        // carries bInterval15/16. Scheduling retains the full wire interval.
        if (exponent>13) exponent=13;
        return physicalEndpoint << 25 | 1u << 8 |
            (type==EndpointType::Isochronous ? (1u<<9)|(1u<<10) : physicalEndpoint < 2 ? 1u << 10 : 0) | exponent << 16;
    }
};
}
