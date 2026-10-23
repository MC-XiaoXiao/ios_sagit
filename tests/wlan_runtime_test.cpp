// SPDX-License-Identifier: BSD-2-Clause
#include "WLANQRTR.h"
#include "WLANTarget.h"
#include "WLANBoardData.h"
#include "WLANDMA.h"
#include "WLANCopyEngineIO.h"
#include "WLANFirmwareAssets.h"
#include <cassert>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <vector>
#include <map>
using namespace WLAN;

struct Endpoint {
    uint64_t ticks = 100;
    uint32_t localPort = 122;
    unsigned requests = 0, boardSegments = 0, resumes = 0;
    uint16_t failID = 0;
    bool wrongChip = false, badMSA = false, shareFail = false, unshareFail = false,
         shared = false, dropped = false, staleResponse = true, existingFirmware = false;
    std::vector<uint8_t> board;
    std::deque<std::vector<uint8_t>> packets;
    uint64_t now() { return ticks; }
    Result allocatePort(uint32_t node,uint64_t epoch,uint32_t &out) {
        assert(node == 1 && epoch); out = ++localPort; return Result::Success;
    }
    void enqueue(uint32_t type,const void *data,unsigned bytes,bool confirm = false) {
        std::vector<uint8_t> packet(QRTR::MaximumPacket); size_t written = 0;
        QRTR::Packet message{type,2,type == QRTR::Data ? 77u : QRTR::ControlPort,1,type == QRTR::Data ? localPort : QRTR::ControlPort,confirm,
            static_cast<const uint8_t *>(data),bytes};
        assert(QRTR::encode(packet.data(),packet.size(),message,written) == Result::Success);
        packet.resize(written); packets.push_back(packet);
    }
    void indicate(uint16_t id) {
        uint8_t wire[7]{}; wire[0] = QMI::Indication; write16(wire + 3,id); enqueue(QRTR::Data,wire,sizeof(wire));
    }
    Result send(const void *data,size_t bytes,uint64_t) {
        QRTR::Packet packet{}; assert(QRTR::parse(data,bytes,packet) == Result::Success);
        if (packet.type == QRTR::Hello) {
            assert(packet.sourcePort == QRTR::ControlPort && packet.destinationPort == QRTR::ControlPort);
            assert(read32(packet.payload) == QRTR::Hello);
            uint8_t hello[20]{}; write32(hello,QRTR::Hello); enqueue(QRTR::Hello,hello,sizeof(hello));
            uint8_t server[20]{}; write32(server,QRTR::NewServer); write32(server + 4,QMI::Service);
            write32(server + 8,1); write32(server + 12,2); write32(server + 16,77);
            enqueue(QRTR::NewServer,server,sizeof(server)); return Result::Success;
        }
        if (packet.type == QRTR::ResumeTX) {
            assert(packet.sourcePort == localPort && packet.destinationPort == 77);
            assert(read32(packet.payload + 4) == 1 && read32(packet.payload + 8) == localPort);
            ++resumes; return Result::Success;
        }
        if (packet.type == QRTR::DeleteClient) {
            assert(packet.sourcePort == QRTR::ControlPort && packet.destinationPort == QRTR::ControlPort);
            assert(read32(packet.payload + 8) == localPort); return Result::Success;
        }
        assert(packet.type == QRTR::Data && packet.destinationNode == 2 && packet.destinationPort == 77);
        QMI::Message request{}; assert(QMI::parse(packet.payload,packet.length,request) == Result::Success);
        assert(request.type == QMI::Request); ++requests;
        uint8_t reply[256]{}; TLVWriter writer(reply,sizeof(reply)); writer.scalar(2,request.id == failID ? 1 : 0,4);
        if (request.id == QMI::IndicationRegister && existingFirmware) writer.scalar(0x10,3,8);
        if (request.id == QMI::MSAInfo) {
            uint8_t regions[14]{}; regions[0] = 1;
            write64(regions + 1,badMSA ? 0x90100000 : 0x90000000); write32(regions + 9,0x100000);
            writer.append(3,regions,sizeof(regions));
        } else if (request.id == QMI::Capabilities) {
            uint8_t chip[8]{}; write32(chip,wrongChip ? 1 : 0x3990);
            writer.append(0x10,chip,sizeof(chip)); writer.scalar(0x11,0x123,4);
        } else if (request.id == QMI::BoardDownload && request.id != failID) {
            assert(shared);
            QMI::TLVs tlvs(request); uint8_t tag; const uint8_t *value; uint16_t length;
            bool segment = false, contents = false, end = false;
            while (tlvs.next(tag,value,length)) {
                if (tag == 0x12) { assert(length == 4 && read32(value) == boardSegments); segment = true; }
                if (tag == 0x13) {
                    assert(length >= 3 && read16(value) == length - 2 && length <= 6146);
                    board.insert(board.end(),value + 2,value + length); contents = true;
                }
                if (tag == 0x14) { assert(length == 1 && value[0] == (board.size() == 13001)); end = true; }
            }
            assert(tlvs.valid() && segment && contents && end); ++boardSegments;
        } else if (request.id == QMI::WLANConfiguration) {
            QMI::TLVs tlvs(request); uint8_t tag; const uint8_t *value; uint16_t length;
            unsigned seen = 0;
            while (tlvs.next(tag,value,length)) {
                if (tag == 0x11) { assert(length == 241 && value[0] == 12 && read32(value + 1 + 5 * 20 + 4) == 2); ++seen; }
                if (tag == 0x12) { assert(length == 253 && value[0] == 21); ++seen; }
                if (tag == 0x13) { assert(length == 49 && value[0] == 12); ++seen; }
            }
            assert(tlvs.valid() && seen == 3);
        }
        uint8_t wire[263]{}; size_t count = 0;
        assert(QMI::encodeRequest(wire,sizeof(wire),request.transaction,request.id,reply,writer.length(),count) == Result::Success);
        wire[0] = QMI::Response;
        if (staleResponse) { write16(wire + 1,request.transaction + 1); enqueue(QRTR::Data,wire,unsigned(count)); write16(wire + 1,request.transaction); staleResponse = false; }
        if (request.id == QMI::MSAReady) indicate(QMI::MSAReadyIndication); // arrives before the ACK
        enqueue(QRTR::Data,wire,unsigned(count),true);
        if (request.id == 0x26) indicate(QMI::FirmwareReady);
        return Result::Success;
    }
    Result receive(void *data,size_t capacity,size_t &bytes,uint64_t end) {
        if (packets.empty() || dropped) { ticks = end; return Result::Timeout; }
        ++ticks; assert(packets.front().size() <= capacity); bytes = packets.front().size();
        memcpy(data,packets.front().data(),bytes); packets.pop_front(); return Result::Success;
    }
    Result shareMSA(uint64_t,const MSARegion *regions,unsigned count) {
        assert(!shared && count == 1 && regions[0].address == 0x90000000);
        if (shareFail) { shared = true; return Result::Fault; } // partially completed secure ownership call
        shared = true; return Result::Success;
    }
    Result unshareMSA(uint64_t,const MSARegion *,unsigned) {
        assert(shared); if (unshareFail) return Result::Timeout;
        shared = false; return Result::Success;
    }
};
static void bootstrap() {
    std::vector<uint8_t> board(13001); for (unsigned i = 0; i < board.size(); ++i) board[i] = uint8_t(i * 17);
    FirmwareConfiguration config; config.msaAddress = 0x90000000; config.msaBytes = 0x100000;
    config.chipID = 0x3990; config.boardID = 0x123; config.board = board.data(); config.boardBytes = board.size();
    config.noMSAReadyIndicator = false;
    for (unsigned failure = 0; failure < 9; ++failure) {
        Endpoint endpoint; QRTR::Session<Endpoint> session(endpoint,1); WLFW<decltype(session)> firmware(session);
        assert(session.connect(1) == Result::Success && session.connected());
        endpoint.wrongChip = failure == 1; endpoint.badMSA = failure == 2; endpoint.shareFail = failure == 3;
        endpoint.failID = failure == 4 ? QMI::BoardDownload : failure == 5 ? QMI::MSAReady : failure == 6 ? 0x26 : 0;
        endpoint.dropped = failure == 7;
        endpoint.existingFirmware = failure == 8;
        const auto result = firmware.bootstrap(config,1);
        if (failure >= 1) {
            assert(result != Result::Success && firmware.currentPhase() == decltype(firmware)::Phase::Fault);
            assert(firmware.ownsPermissions() == (failure == 1 || failure == 3 || failure == 4 || failure == 5 || failure == 6));
            if (firmware.ownsPermissions()) {
                endpoint.failID = QMI::WLANMode;
                assert(firmware.shutdown() == Result::Fault && endpoint.shared);
                endpoint.failID = 0; assert(firmware.shutdown() == Result::Success && !endpoint.shared);
            }
            if (failure == 8) {
                assert(result == Result::Busy && endpoint.requests == 1);
                assert(firmware.shutdown() == Result::Success && endpoint.requests == 1);
            }
            continue;
        }
        assert(result == Result::Success && firmware.currentPhase() == decltype(firmware)::Phase::Ready);
        assert(endpoint.board == board && endpoint.boardSegments == 3 && endpoint.resumes >= 8);
        uint8_t cfg[1024]; size_t count = 0;
        assert(targetConfiguration(cfg,sizeof(cfg),count) == Result::Success);
        assert(firmware.configure(cfg,count - 1) == Result::Invalid);
        assert(firmware.configure(cfg,count) == Result::Success && firmware.currentPhase() == decltype(firmware)::Phase::Enabled);
        endpoint.unshareFail = true;
        assert(firmware.shutdown() == Result::Timeout && endpoint.shared && firmware.ownsPermissions());
        assert(firmware.bootstrap(config,2) == Result::Busy);
        endpoint.unshareFail = false;
        assert(firmware.shutdown() == Result::Success && !endpoint.shared);
        // Leave an old transaction-1 response in the raw channel. A fresh
        // wire port, not just a software epoch, prevents its reuse.
        uint8_t old[14] = {QMI::Response,1,0,0x20,0,7,0,2,4,0,0,0,0,0};
        endpoint.enqueue(QRTR::Data,old,sizeof(old));
        assert(session.close() == Result::Success);
        assert(session.connect(1) == Result::Invalid);
        assert(session.connect(2) == Result::Success && endpoint.localPort == 124);
        assert(session.close() == Result::Success);
    }
    // Every possible prefix is rejected; the framing never exposes bytes
    // outside the caller's bounded datagram, even with an overflowing size.
    uint8_t bytes[40]{}; QRTR::Packet packet{};
    for (unsigned n = 0; n < sizeof(bytes); ++n) assert(QRTR::parse(bytes,n,packet) != Result::Success);
    write32(bytes,1); write32(bytes + 4,1); write32(bytes + 20,UINT32_MAX);
    assert(QRTR::parse(bytes,sizeof(bytes),packet) == Result::Invalid);
}

struct MemoryAPI {
    struct Memory { alignas(4096) uint8_t data[4096]; };
    struct Command { bool poison = false; };
    unsigned fail = 0, memoryCount = 0, commandCount = 0, barriers = 0;
    Memory *fixtureMemory = nullptr;
    Command *fixtureCommand = nullptr;
    bool stopped = false;
    // Fixture destruction is host memory cleanup, not a modeled device ACK.
    ~MemoryAPI() { delete fixtureCommand; delete fixtureMemory; }
    bool ready() const { return fail != 1; }
    Memory *allocate(uint32_t,unsigned) { if (fail == 2) return nullptr; ++memoryCount; return fixtureMemory = new Memory; }
    Result prepare(Memory *) { return fail == 3 ? Result::NotReady : Result::Success; }
    void *bytes(Memory *m) { return fail == 4 ? nullptr : m->data; }
    Command *createCommand(uint32_t,unsigned) { if (fail == 5) return nullptr; ++commandCount; return fixtureCommand = new Command; }
    Result bind(Command *,Memory *) { return fail == 6 ? Result::NotReady : Result::Success; }
    Result range(Command *,uint64_t &address,uint64_t &bytes) {
        address = fail == 9 ? 1ULL << 35 : 0x10000; bytes = fail == 8 ? 4095 : 4096;
        return fail == 7 ? Result::Fault : Result::Success;
    }
    Result clear(Command *c) { if (fail == 10) c->poison = true; return c->poison ? Result::Fault : Result::Success; }
    Result complete(Memory *) { return fail == 11 ? Result::Timeout : Result::Success; }
    void releaseMemory(Memory *m) { --memoryCount; delete m; fixtureMemory = nullptr; }
    void releaseCommand(Command *c) { --commandCount; delete c; fixtureCommand = nullptr; }
    Result isolated(uint64_t) { return stopped ? Result::Success : Result::Timeout; }
    void publishBarrier() { ++barriers; }
    void consumeBarrier() { ++barriers; }
};
static void dma() {
    for (unsigned fail = 0; fail <= 9; ++fail) {
        MemoryAPI api; api.fail = fail; DMABuffer<MemoryAPI> buffer(api);
        auto result = buffer.allocate(4096,64);
        if (fail) { assert(result != Result::Success && !buffer.ownsMemory() && !api.memoryCount && !api.commandCount); continue; }
        assert(result == Result::Success && api.memoryCount == 1 && api.commandCount == 1);
        assert(buffer.publish(2) == Result::Success && api.barriers == 1);
        assert(buffer.release() == Result::Busy && buffer.retire(1) == Result::NotReady);
        buffer.quarantine(); assert(buffer.retire(2) == Result::NotReady);
        assert(buffer.isolate(2) == Result::Timeout && api.memoryCount == 1);
        api.stopped = true; assert(buffer.isolate(1) == Result::NotReady && buffer.isolate(2) == Result::Success);
        assert(buffer.publish(1) == Result::Invalid && buffer.publish(2) == Result::Success);
        assert(buffer.retire(2) == Result::Success);
        api.fail = 11; assert(buffer.release() == Result::Timeout && api.memoryCount == 1 && !api.commandCount);
        api.fail = 0; assert(buffer.release() == Result::Success && !api.memoryCount);
    }
    MemoryAPI api; DMABuffer<MemoryAPI> buffer(api); assert(buffer.allocate(4096,64) == Result::Success);
    api.fail = 10; assert(buffer.release() == Result::Fault && api.memoryCount == 1 && api.commandCount == 1);
    api.fail = 0; assert(buffer.release() == Result::Fault); // native complete failure is sticky
    assert(buffer.ownsMemory()); // owner stays pinned; retry is no isolation proof
}

static void boardData() {
    constexpr char magic[] = "QCA-ATH10K-BOARD", name[] = "bus=snoc,qmi-board-id=123,qmi-chip-id=3990";
    std::vector<uint8_t> bytes(magic,magic + sizeof(magic));
    bytes.resize((bytes.size() + 3) & ~size_t(3));
    auto append = [](std::vector<uint8_t> &to,uint32_t kind,const void *data,size_t count) {
        const auto start = to.size(); to.resize(start + 8 + ((count + 3) & ~size_t(3)));
        write32(to.data() + start,kind); write32(to.data() + start + 4,uint32_t(count)); memcpy(to.data() + start + 8,data,count);
    };
    std::vector<uint8_t> entry; append(entry,0,name,sizeof(name)-1); const uint8_t data[] = {1,2,3,4}; append(entry,1,data,sizeof(data));
    append(bytes,0,entry.data(),entry.size()); BoardView view;
    assert(selectBoard(bytes.data(),bytes.size(),name,sizeof(name)-1,view) == Result::Success && view.length == 4 && view.data[0] == 1);
    for (size_t n = 0; n < bytes.size(); ++n) assert(selectBoard(bytes.data(),n,name,sizeof(name)-1,view) != Result::Success);
    assert(selectBoard(bytes.data(),bytes.size(),"missing",7,view) == Result::NotReady);
    append(bytes,0,entry.data(),entry.size()); assert(selectBoard(bytes.data(),bytes.size(),name,sizeof(name)-1,view) == Result::Invalid);
    assert(!view.data && !view.length);
    FirmwareAssets assets; uint8_t configuration[24]{};
    write32(configuration,1); write32(configuration + 4,24); write32(configuration + 8,0x3990);
    write32(configuration + 12,0x123); write32(configuration + 16,sizeof(data));
    assert(assets.bind(configuration,sizeof(configuration),data,sizeof(data)) == Result::Success && assets.valid());
    const auto resources = assets.forMSA(0x90000000,0x100000,true,true);
    assert(resources.valid() && resources.chipID == 0x3990 && resources.boardID == 0x123);
    configuration[20] = 1;
    assert(assets.bind(configuration,sizeof(configuration),data,sizeof(data)) == Result::Invalid && !assets.valid());
}
struct RingMemory {
    alignas(64) uint8_t data[64]{};
    DMAState state = DMAState::Allocated;
    uint64_t address = 0x700010000, epoch = 0;
    DMAState currentState() const { return state; }
    uint64_t busAddress() const { return address; }
    unsigned capacity() const { return sizeof(data); }
    void *bytes() { return data; }
    Result publish(uint64_t e) { assert(state == DMAState::Allocated); state = DMAState::DeviceOwned; epoch = e; return Result::Success; }
    void quarantine() { state = DMAState::Quarantined; }
    Result isolate(uint64_t e) { assert(e == epoch); state = DMAState::Allocated; return Result::Success; }
};
struct Registers {
    std::map<uint32_t,uint32_t> values;
    unsigned barriers = 0;
    uint32_t reject = 0;
    bool stopped = true;
    uint32_t read(uint32_t offset) { return values[offset]; }
    void write(uint32_t offset,uint32_t value) { if (offset != reject) values[offset] = value; }
    Result isolated(uint64_t) { return stopped ? Result::Success : Result::Timeout; }
    void publishBarrier() { ++barriers; }
    void consumeBarrier() { ++barriers; }
};
static void hardware() {
    Registers io; RingMemory ring; CopyEngineIO<Registers,4,false> tx(io);
    assert(tx.configure(6,1,ring,2048) == Result::Invalid);
    io.stopped = false; assert(tx.configure(3,1,ring,2048) == Result::Timeout);
    io.stopped = true; assert(tx.configure(3,1,ring,2048) == Result::Success);
    assert(io.values[0x243004] == 7 && io.values[0x243008] == 4 && ring.state == DMAState::DeviceOwned);
    assert(tx.post(1,99,0x700020000,100,0xabc) == Result::Success && io.values[0x24303c] == 1);
    assert(read16(ring.data + 10) == 0xabc0 && io.barriers >= 2);
    uint64_t cookie = 0; uint32_t bytes = 0; uint16_t metadata = 0;
    assert(tx.completion(1,cookie,bytes,metadata) == Result::NotReady);
    io.values[0x243044] = 1;
    assert(tx.completion(1,cookie,bytes,metadata) == Result::Success && cookie == 99 && bytes == 100 && metadata == 0xabc);
    assert(tx.post(1,100,0x700020000,100,0) == Result::Success);
    io.values[0x243044] = 0; // impossible hardware advancement beyond pending
    assert(tx.completion(1,cookie,bytes,metadata) == Result::Fault && tx.quarantined() && ring.state == DMAState::Quarantined);
    io.stopped = false; assert(tx.shutdown(1) == Result::Timeout && tx.ownsHardware());
    io.stopped = true; assert(tx.shutdown(2) == Result::NotReady && tx.shutdown(1) == Result::Success && !tx.ownsHardware());
    assert(ring.state == DMAState::Allocated);
    Registers rxIO; RingMemory rxRing; CopyEngineIO<Registers,4,true> rx(rxIO);
    assert(rx.configure(1,1,rxRing,2048) == Result::Success);
    assert(rx.post(1,7,0x700030000,2048) == Result::Success && !read16(rxRing.data + 8));
    assert(rx.completion(1,cookie,bytes,metadata) == Result::NotReady);
    write16(rxRing.data + 8,2000); write16(rxRing.data + 10,0x3210);
    assert(rx.completion(1,cookie,bytes,metadata) == Result::Success && cookie == 7 && bytes == 2000 && metadata == 0x321);
    assert(rx.post(1,8,0x700030000,100) == Result::Success);
    write16(rxRing.data + 16 + 8,101);
    assert(rx.completion(1,cookie,bytes,metadata) == Result::Fault && rx.pending() == 1);
    assert(rx.shutdown(1) == Result::Success);
    Registers failedIO; RingMemory failedRing; CopyEngineIO<Registers,4,false> failed(failedIO);
    failedIO.reject = 0x240008;
    assert(failed.configure(0,1,failedRing,2048) == Result::Fault && failed.ownsHardware());
    assert(failedRing.state == DMAState::Quarantined && failed.shutdown(1) == Result::Success);
}
int main() { bootstrap(); dma(); boardData(); hardware(); puts("WLAN: QRTR/WLFW boot, board selection, CE MMIO/completions and DMA failure lifetimes passed"); }
