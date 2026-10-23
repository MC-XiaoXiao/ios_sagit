// SPDX-License-Identifier: BSD-2-Clause
#pragma once
#include "BluetoothFirmware.h"

namespace Bluetooth {
struct FirmwareIdentity {
    uint32_t product = 0, soc = 0;
    uint16_t rom = 0;
};
// The platform keeps these immutable views alive until the controller is off.
// TX completion is a UART drain, never software queue admission. Vendor errors
// and timeouts quarantine the sequence; no synthetic HCI events are injected.
template <class UART> class FirmwareLoader {
public:
    enum class Phase {
        Idle,
        VersionDrain,
        Version,
        BaudDrain,
        BaudSettle,
        PatchSend,
        PatchDrain,
        PatchAck,
        NVMPause,
        NVMSend,
        NVMDrain,
        NVMAck,
        Complete,
        Fault
    };

private:
    UART& uart;
    FirmwareView patch { }, nvm { };
    FirmwareIdentity identity { };
    Phase phase = Phase::Idle;
    uint64_t deadline = 0;
    size_t offset = 0, segmentBytes = 0;
    bool ack = false, last = false, needAck = false;
    Result fail(Result r)
    {
        phase = Phase::Fault;
        return r;
    }
    Result segment(uint64_t now, bool isNVM)
    {
        const auto& view = isNVM ? nvm : patch;
        FirmwareSegment s;
        uint8_t command[FirmwareSegmentBytes + 6];
        size_t size = 0;
        if (nextFirmwareSegment(view, offset, s) != Result::Success
            || encodeFirmwareSegment(view, s, command, sizeof(command), size) != Result::Success)
            return fail(Result::Invalid);
        const auto result = uart.write(command, size);
        if (result != Result::Success)
            return result == Result::Busy ? Result::Pending : fail(result);
        if (!deadlineAfter(now, 2000, deadline))
            return fail(Result::Invalid);
        segmentBytes = s.bytes;
        last = s.last;
        needAck = s.vendorEvent;
        ack = false;
        phase = isNVM ? Phase::NVMDrain : Phase::PatchDrain;
        return Result::Pending;
    }

public:
    explicit FirmwareLoader(UART& port)
        : uart(port)
    {
    }
    Phase currentPhase() const
    {
        return phase;
    }
    void resetAfterQuiesce()
    {
        phase = Phase::Idle;
        patch = { };
        nvm = { };
        offset = 0;
        ack = false;
    }
    Result begin(const FirmwareView& p, const FirmwareView& n, FirmwareIdentity id, uint64_t now)
    {
        if (phase != Phase::Idle)
            return Result::Busy;
        if (!p.data || !n.data || p.type != FirmwareType::Patch || n.type != FirmwareType::NVM || !id.soc
            || !id.rom || p.bytes < 28 || read16(p.data + 16) != uint16_t(id.product)
            || read16(p.data + 18) != id.rom || !deadlineAfter(now, 2000, deadline))
            return fail(Result::Invalid);
        patch = p;
        nvm = n;
        identity = id;
        const uint8_t command[] = { Command, 0, 0xfc, 1, 0x19 };
        const auto r = uart.write(command, sizeof(command));
        if (r != Result::Success)
            return fail(r);
        phase = Phase::VersionDrain;
        return Result::Pending;
    }
    Result receive(const uint8_t* packet, size_t size, uint64_t now)
    {
        if (phase == Phase::Fault)
            return Result::Fault;
        if ((phase == Phase::VersionDrain || phase == Phase::Version || phase == Phase::BaudDrain
                || phase == Phase::PatchDrain || phase == Phase::PatchAck || phase == Phase::NVMDrain
                || phase == Phase::NVMAck)
            && now >= deadline)
            return fail(Result::Timeout);
        if (!validPacket(packet, size, false) || packet[0] != Event)
            return fail(Result::Invalid);
        // CC is optional on these ROMs and may follow the VSE after the next
        // timer continuation. It never advances the download or substitutes
        // for a VSE. A reported controller error still aborts the sequence.
        if (packet[1] == 0x0e) {
            if (size != 7 || packet[6])
                return fail(Result::Fault);
            const auto opcode = read16(packet + 4);
            if (opcode == DownloadOpcode && phase != Phase::Idle)
                return Result::Success;
            if (opcode == 0xfc48 && (phase == Phase::BaudDrain || phase == Phase::BaudSettle))
                return Result::Success;
            return fail(Result::Invalid);
        }
        if (phase == Phase::Version || phase == Phase::VersionDrain) {
            SoCVersion found;
            const auto r = parseVersionEvent(packet, size, found);
            if (r != Result::Success || found.product != identity.product || found.soc != identity.soc
                || found.rom != identity.rom)
                return fail(Result::Invalid);
            const uint8_t command[] = { Command, 0x48, 0xfc, 1, 0x11 }; // QCA 3200000 baud
            if (uart.drained() != Result::Success)
                return fail(Result::Fault);
            const auto flow = uart.beginBaudChange();
            if (flow != Result::Success)
                return fail(flow);
            const auto sent = uart.write(command, sizeof(command));
            if (sent != Result::Success)
                return fail(sent);
            phase = Phase::BaudDrain;
            return deadlineAfter(now, 2000, deadline) ? Result::Success : fail(Result::Invalid);
        }
        if (phase == Phase::PatchDrain || phase == Phase::PatchAck || phase == Phase::NVMDrain
            || phase == Phase::NVMAck) {
            const auto r = validateDownloadEvent(packet, size);
            if (r != Result::Success || ack || !needAck)
                return fail(r == Result::Success ? Result::Invalid : r);
            ack = true;
            return Result::Success;
        }
        // A baud-change vendor indication may arrive before the host switches
        // clocks. It is not a patch acknowledgement or proof of firmware load.
        if (phase == Phase::BaudDrain || phase == Phase::BaudSettle)
            return size == 5 && packet[1] == 0xff && packet[3] == 0x92 && !packet[4] ? Result::Success
                                                                                     : fail(Result::Invalid);
        return fail(Result::Invalid);
    }
    Result advance(uint64_t now)
    {
        if (phase == Phase::Complete)
            return Result::Success;
        if (phase == Phase::Fault)
            return Result::Fault;
        if (phase == Phase::Idle)
            return Result::NotReady;
        if (phase == Phase::PatchSend)
            return segment(now, false);
        if (phase == Phase::NVMSend)
            return segment(now, true);
        if (phase == Phase::NVMPause || phase == Phase::BaudSettle) {
            if (now < deadline)
                return Result::Pending;
            if (phase == Phase::BaudSettle) {
                const auto r = uart.runtimeBaud();
                if (r != Result::Success)
                    return fail(r);
                phase = Phase::PatchSend;
            } else
                phase = Phase::NVMSend;
            offset = 0;
            return Result::Pending;
        }
        if (now >= deadline)
            return fail(Result::Timeout);
        if (phase == Phase::Version)
            return Result::Pending;
        if (phase == Phase::VersionDrain || phase == Phase::BaudDrain || phase == Phase::PatchDrain
            || phase == Phase::NVMDrain) {
            const auto r = uart.drained();
            if (r == Result::Pending)
                return r;
            if (r != Result::Success)
                return fail(r);
            if (phase == Phase::VersionDrain) {
                phase = Phase::Version;
                return Result::Pending;
            }
            if (phase == Phase::BaudDrain) {
                phase = Phase::BaudSettle;
                return deadlineAfter(now, 50, deadline) ? Result::Pending : fail(Result::Invalid);
            }
            phase = phase == Phase::PatchDrain ? Phase::PatchAck : Phase::NVMAck;
        }
        if (needAck && !ack)
            return Result::Pending;
        if (last) {
            if (phase == Phase::NVMAck) {
                phase = Phase::Complete;
                return Result::Success;
            }
            phase = Phase::NVMPause;
            return deadlineAfter(now, 10, deadline) ? Result::Pending : fail(Result::Invalid);
        }
        offset += segmentBytes;
        phase = phase == Phase::NVMAck ? Phase::NVMSend : Phase::PatchSend;
        return Result::Pending;
    }
};
}
