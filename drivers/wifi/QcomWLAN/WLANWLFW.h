// SPDX-License-Identifier: BSD-2-Clause
#pragma once
#include "WLANFirmware.h"

namespace WLAN {
inline uint64_t read64(const void *p) {
    const auto *b = static_cast<const uint8_t *>(p);
    return uint64_t(read32(b)) | uint64_t(read32(b + 4)) << 32;
}
inline void write64(void *p, uint64_t v) {
    auto *b = static_cast<uint8_t *>(p); write32(b,uint32_t(v)); write32(b + 4,uint32_t(v >> 32));
}
class TLVWriter {
    uint8_t *bytes; size_t capacity, used = 0;
    bool failed = false;
public:
    TLVWriter(void *buffer,size_t length) : bytes(static_cast<uint8_t *>(buffer)),capacity(length) {}
    bool append(uint8_t tag,const void *data,size_t length) {
        if (failed || !bytes || length > UINT16_MAX || (length && !data) || capacity - used < 3 || length > capacity - used - 3)
            return !(failed = true);
        bytes[used] = tag; write16(bytes + used + 1,uint16_t(length));
        const auto *source = static_cast<const uint8_t *>(data);
        for (size_t i = 0; i < length; ++i) bytes[used + 3 + i] = source[i];
        used += 3 + length; return true;
    }
    bool scalar(uint8_t tag,uint64_t value,unsigned width) {
        if (width != 1 && width != 2 && width != 4 && width != 8) return !(failed = true);
        uint8_t data[8]; write64(data,value); return append(tag,data,width);
    }
    bool array(uint8_t tag,const void *data,size_t length,unsigned countBytes,unsigned count) {
        if ((countBytes != 1 && countBytes != 2) || count >= (1u << (countBytes * 8)) ||
            length > UINT16_MAX - countBytes || (length && !data) || failed || !bytes ||
            capacity - used < 3 + countBytes || length > capacity - used - 3 - countBytes)
            return !(failed = true);
        bytes[used] = tag; write16(bytes + used + 1,uint16_t(length + countBytes));
        bytes[used + 3] = uint8_t(count);
        if (countBytes == 2) bytes[used + 4] = uint8_t(count >> 8);
        for (size_t i = 0; i < length; ++i) bytes[used + 3 + countBytes + i] = static_cast<const uint8_t *>(data)[i];
        used += 3 + countBytes + length; return true;
    }
    size_t length() const { return used; }
    bool valid() const { return !failed; }
};
struct MSARegion { uint64_t address; uint32_t size; bool secure; };
struct FirmwareConfiguration {
    uint64_t msaAddress = 0; uint32_t msaBytes = 0;
    uint32_t chipID = 0, boardID = 0;
    const uint8_t *board = nullptr; uint32_t boardBytes = 0;
    bool hostCapability8Bit = true, noMSAReadyIndicator = true;
    bool valid() const {
        return msaAddress && !(msaAddress & 4095) && msaBytes >= MinimumMSABytes && !(msaBytes & 4095) &&
            msaAddress < (1ULL << 40) && msaBytes <= (1ULL << 40) - msaAddress && board &&
            boardBytes && boardBytes <= 1024 * 1024;
    }
};

// WLFW boot/configuration protocol. Transport implements a real, correlated
// QMI exchange and ownership calls; the same protocol is driven by failure
// injection offline. No Linux driver or userspace pointer is linked here.
template<class Transport> class WLFW {
public:
    enum class Phase : uint32_t { Empty, Registered, MemoryShared, BoardLoaded, Ready, Enabled, Fault };
private:
    Transport &io;
    FirmwareConfiguration configuration{};
    uint64_t generation = 0;
    Phase phase = Phase::Empty;
    uint8_t request[QMI::MaximumPayload]{};
    MSARegion regions[2]{}; unsigned regionCount = 0;
    bool permissions = false, registered = false;
    Result exchange(uint16_t id,TLVWriter &writer,QMI::Message &response) {
        if (!writer.valid()) return Result::Invalid;
        auto result = io.exchange(generation,id,request,writer.length(),response);
        if (result != Result::Success) return result;
        if (response.type != QMI::Response || response.id != id) return Result::Invalid;
        uint16_t error = 0; return QMI::responseStatus(response,error);
    }
    Result fault(Result result) { phase = Phase::Fault; return result; }
    Result registrationStatus(const QMI::Message &message) {
        QMI::TLVs tlvs(message); uint8_t tag; uint16_t bytes; const uint8_t *data;
        bool seen = false;
        while (tlvs.next(tag,data,bytes)) if (tag == 0x10) {
            if (seen || bytes != 8) return Result::Invalid;
            seen = true;
            // Adopting an already registered/ready remote needs a separate
            // retained-resource handoff. Do not initialize or power it off.
            if (read64(data)) return Result::Busy;
        }
        return tlvs.valid() ? Result::Success : Result::Invalid;
    }
    bool memoryResponse(const QMI::Message &message) {
        QMI::TLVs tlvs(message); uint8_t tag; uint16_t bytes; const uint8_t *data;
        bool found = false;
        while (tlvs.next(tag,data,bytes)) if (tag == 3) {
            if (found || !bytes || data[0] == 0 || data[0] > 2 || bytes != 1 + 13 * data[0]) return false;
            found = true; regionCount = data[0];
            for (unsigned i = 0; i < regionCount; ++i) {
                const auto *p = data + 1 + 13 * i;
                const uint64_t address = read64(p); const uint32_t length = read32(p + 8);
                if (!length || p[12] > 1 || address < configuration.msaAddress ||
                    address - configuration.msaAddress >= configuration.msaBytes ||
                    length > configuration.msaBytes - (address - configuration.msaAddress) ||
                    ((address | length) & 4095)) return false;
                if (i && address < regions[0].address + regions[0].size && regions[0].address < address + length) return false;
                regions[i] = {address,length,p[12] != 0};
            }
        }
        return found && tlvs.valid();
    }
    bool capabilities(const QMI::Message &message) {
        QMI::TLVs tlvs(message); uint8_t tag; uint16_t bytes; const uint8_t *data;
        bool chip = false, board = false;
        while (tlvs.next(tag,data,bytes)) {
            if (tag == 0x10) {
                if (chip || bytes != 8 || read32(data) != configuration.chipID) return false;
                chip = true;
            } else if (tag == 0x11) {
                if (board || bytes != 4 || read32(data) != configuration.boardID) return false;
                board = true;
            }
        }
        return chip && board && tlvs.valid();
    }
public:
    explicit WLFW(Transport &transport) : io(transport) {}
    Phase currentPhase() const { return phase; }
    bool ownsPermissions() const { return permissions; }
    bool ownsSession() const { return registered; }
    Result bootstrap(const FirmwareConfiguration &config,uint64_t epoch) {
        if (phase != Phase::Empty || permissions) return Result::Busy;
        if (!config.valid() || !epoch) return Result::Invalid;
        configuration = config; generation = epoch;
        QMI::Message response{};
        TLVWriter registration(request,sizeof(request));
        registration.scalar(0x10,1,1); registration.scalar(0x13,1,1);
        registration.scalar(0x15,0x4b4e454c,4);
        auto result = exchange(QMI::IndicationRegister,registration,response);
        if (result != Result::Success) return fault(result);
        result = registrationStatus(response);
        if (result != Result::Success) return fault(result);
        registered = true;
        phase = Phase::Registered;
        TLVWriter host(request,sizeof(request));
        host.scalar(0x10,0,config.hostCapability8Bit ? 1 : 4); // daemon_support
        host.scalar(0x14,1,1); // board-data download supported
        result = exchange(QMI::HostCapabilities,host,response);
        if (result != Result::Success) return fault(result);
        TLVWriter msa(request,sizeof(request));
        msa.scalar(1,config.msaAddress,8); msa.scalar(2,config.msaBytes,4);
        result = exchange(QMI::MSAInfo,msa,response);
        if (result != Result::Success || !memoryResponse(response)) return fault(result == Result::Success ? Result::Invalid : result);
        // A failed SCM transaction may have granted only part of the range.
        // Treat the attempt as ownership until unshare verifies CPU ownership
        // of every region, including a share that failed before making changes.
        permissions = true;
        result = io.shareMSA(generation,regions,regionCount);
        if (result != Result::Success) return fault(result);
        phase = Phase::MemoryShared;
        TLVWriter empty(request,sizeof(request));
        result = exchange(QMI::MSAReady,empty,response);
        if (result != Result::Success) return fault(result);
        TLVWriter capability(request,sizeof(request));
        result = exchange(QMI::Capabilities,capability,response);
        if (result != Result::Success || !capabilities(response)) return fault(result == Result::Success ? Result::Invalid : result);
        if (!config.noMSAReadyIndicator) {
            result = io.waitIndication(generation,QMI::MSAReadyIndication);
            if (result != Result::Success) return fault(result);
        }
        // Board data is pinned to the exact chip/board IDs before transmission.
        for (uint32_t offset = 0, segment = 0; offset < config.boardBytes; ++segment) {
            const uint32_t count = config.boardBytes - offset > 6144 ? 6144 : config.boardBytes - offset;
            TLVWriter download(request,sizeof(request));
            download.scalar(1,1,1); download.scalar(0x10,0,4);
            download.scalar(0x11,config.boardBytes,4); download.scalar(0x12,segment,4);
            download.array(0x13,config.board + offset,count,2,count);
            download.scalar(0x14,offset + count == config.boardBytes,1);
            result = exchange(QMI::BoardDownload,download,response);
            if (result != Result::Success) return fault(result);
            offset += count;
        }
        TLVWriter calibration(request,sizeof(request));
        calibration.scalar(1,0,1); // zero-length calibration metadata array
        result = exchange(0x26,calibration,response);
        if (result != Result::Success) return fault(result);
        phase = Phase::BoardLoaded;
        result = io.waitIndication(generation,QMI::FirmwareReady);
        if (result != Result::Success) return fault(result);
        phase = Phase::Ready; return Result::Success;
    }
    Result configure(const void *payload,size_t bytes) {
        if (phase != Phase::Ready || !payload || bytes > sizeof(request)) return Result::NotReady;
        QMI::Message configurationMessage{QMI::Request,1,QMI::WLANConfiguration,static_cast<const uint8_t *>(payload),bytes};
        QMI::TLVs tlvs(configurationMessage); uint8_t tag; uint16_t count; const uint8_t *value;
        unsigned seen = 0;
        while (tlvs.next(tag,value,count)) {
            if (tag < 0x10 || tag > 0x13 || (seen & (1u << (tag - 0x10)))) return Result::Invalid;
            seen |= 1u << (tag - 0x10);
            if (tag == 0x10) { if (!count || count > 16) return Result::Invalid; }
            else {
                const unsigned stride = tag == 0x11 ? 20 : tag == 0x12 ? 12 : 4;
                const unsigned maximum = tag == 0x11 ? 12 : 24;
                if (!count || !value[0] || value[0] > maximum || count != 1 + stride * value[0]) return Result::Invalid;
            }
        }
        if (!tlvs.valid() || seen != 15) return Result::Invalid;
        for (size_t i = 0; i < bytes; ++i) request[i] = static_cast<const uint8_t *>(payload)[i];
        QMI::Message response{};
        auto result = io.exchange(generation,QMI::WLANConfiguration,request,bytes,response);
        uint16_t error = 0;
        if (result == Result::Success) result = response.type == QMI::Response && response.id == QMI::WLANConfiguration ? QMI::responseStatus(response,error) : Result::Invalid;
        if (result != Result::Success) return fault(result);
        TLVWriter mode(request,sizeof(request)); mode.scalar(1,0,4); // mission mode
        result = exchange(QMI::WLANMode,mode,response);
        if (result != Result::Success) return fault(result);
        phase = Phase::Enabled; return Result::Success;
    }
    Result shutdown() {
        if (!registered && !permissions) { phase = Phase::Empty; return Result::Success; }
        TLVWriter mode(request,sizeof(request)); mode.scalar(1,4,4); // OFF, explicit firmware ACK
        QMI::Message response{}; auto result = exchange(QMI::WLANMode,mode,response);
        if (result != Result::Success) return fault(result);
        if (permissions) {
            result = io.unshareMSA(generation,regions,regionCount);
            if (result != Result::Success) return fault(result);
        }
        permissions = registered = false; regionCount = 0; phase = Phase::Empty; return Result::Success;
    }
};
}
