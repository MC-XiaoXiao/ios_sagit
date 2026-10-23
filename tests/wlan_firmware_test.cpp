// SPDX-License-Identifier: BSD-2-Clause
#include "WLANFirmware.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>

using namespace WLAN;
static void firmware() {
    uint8_t buffer[64] = {}; size_t written = 123;
    const uint8_t payload[] = {2,4,0,0,0,0,0};
    assert(QMI::encodeRequest(buffer,sizeof(buffer),0x1234,QMI::Capabilities,payload,sizeof(payload),written) == Result::Success);
    assert(written == 14 && buffer[1] == 0x34 && buffer[2] == 0x12 && buffer[3] == 0x24);
    QMI::Message message = {}; assert(QMI::parse(buffer,written,message) == Result::Success);
    assert(message.transaction == 0x1234 && message.type == QMI::Request);
    buffer[0] = QMI::Response;
    assert(QMI::parse(buffer,written,message) == Result::Success);
    uint16_t error = 99; assert(QMI::responseStatus(message,error) == Result::Success && !error);
    for (size_t bytes = 0; bytes < written; ++bytes) assert(QMI::parse(buffer,bytes,message) == Result::Invalid);
    assert(QMI::parse(buffer,written + 1,message) == Result::Invalid);
    buffer[0] = 1; assert(QMI::parse(buffer,written,message) == Result::Invalid); buffer[0] = QMI::Response;
    assert(QMI::encodeRequest(buffer,6,1,1,nullptr,0,written) == Result::Invalid && !written);
    assert(QMI::encodeRequest(buffer,sizeof(buffer),0,1,nullptr,0,written) == Result::Invalid);
    assert(QMI::encodeRequest(buffer,sizeof(buffer),1,1,nullptr,1,written) == Result::Invalid);
    assert(QMI::encodeRequest(buffer,sizeof(buffer),1,1,payload,SIZE_MAX,written) == Result::Invalid);
    memcpy(buffer,payload,sizeof(payload));
    assert(QMI::encodeRequest(buffer,sizeof(buffer),1,1,buffer,sizeof(payload),written) == Result::Success);
    assert(!memcmp(buffer + QMI::HeaderBytes,payload,sizeof(payload)));
    memcpy(buffer + 12,payload,sizeof(payload));
    assert(QMI::encodeRequest(buffer,sizeof(buffer),1,1,buffer + 12,sizeof(payload),written) == Result::Success);
    assert(!memcmp(buffer + QMI::HeaderBytes,payload,sizeof(payload)));
    QMI::Message invalid = {QMI::Response,1,1,nullptr,7};
    assert(QMI::responseStatus(invalid,error) == Result::Invalid);
    QMI::Transaction transaction; uint16_t id;
    assert(transaction.begin(7,QMI::Capabilities,100,10,id) == Result::Success && id == 1);
    assert(transaction.begin(7,1,100,10,id) == Result::Busy);
    assert(QMI::encodeRequest(buffer,sizeof(buffer),id,QMI::Capabilities,payload,sizeof(payload),written) == Result::Success);
    buffer[0] = QMI::Response; assert(QMI::parse(buffer,written,message) == Result::Success);
    assert(transaction.receive(6,101,message,error) == Result::NotReady && transaction.busy());
    message.id++; assert(transaction.receive(7,101,message,error) == Result::NotReady); message.id--;
    assert(transaction.receive(7,109,message,error) == Result::Success && !transaction.busy());
    assert(transaction.receive(7,109,message,error) == Result::NotReady);
    assert(transaction.begin(7,1,UINT64_MAX,10,id) == Result::Invalid);
    assert(transaction.begin(7,1,100,10,id) == Result::Success && id == 2);
    assert(transaction.expire(7,109) == Result::Busy);
    assert(transaction.expire(7,110) == Result::Timeout && !transaction.busy());
    assert(transaction.begin(7,QMI::Capabilities,200,10,id) == Result::Success);
    message.transaction = id;
    assert(transaction.receive(7,210,message,error) == Result::Timeout);
    // Malformed/duplicate mandatory result TLVs cannot confirm firmware boot.
    uint8_t duplicate[14]; memcpy(duplicate,payload,7); memcpy(duplicate + 7,payload,7);
    message = {QMI::Response,1,1,duplicate,14}; assert(QMI::responseStatus(message,error) == Result::Invalid);
    for (size_t n = 0; n < 7; ++n) {
        message = {QMI::Response,1,1,payload,n}; assert(QMI::responseStatus(message,error) == Result::Invalid);
    }
    uint8_t failed[] = {2,4,0,1,0,42,0};
    message = {QMI::Response,1,1,failed,sizeof(failed)};
    assert(QMI::responseStatus(message,error) == Result::Fault && error == 42);
    failed[3] = 0; assert(QMI::responseStatus(message,error) == Result::Invalid);
    // Exhausted 16-bit IDs are never reused on one service connection.
    QMI::Transaction exhausted;
    for (unsigned i = 1; i <= UINT16_MAX; ++i) {
        assert(exhausted.begin(1,1,0,1,id) == Result::Success && id == i); exhausted.cancel();
    }
    assert(exhausted.begin(1,1,0,1,id) == Result::NotReady);
    assert(exhausted.begin(2,1,0,1,id) == Result::Success && id == 1);
    exhausted.cancel(); assert(exhausted.begin(1,1,0,1,id) == Result::NotReady);
}
static void htc() {
    uint8_t bytes[] = {2,0,3,0,0,0,0,0,1,2,3}; HTC::Packet packet = {};
    assert(HTC::parse(bytes,sizeof(bytes),packet) == Result::Success && packet.length == 3);
    for (size_t n = 0; n < sizeof(bytes); ++n) assert(HTC::parse(bytes,n,packet) == Result::Invalid);
    bytes[1] = 0x10; assert(HTC::parse(bytes,sizeof(bytes),packet) == Result::Unsupported);
    bytes[1] = 2; bytes[4] = 4; assert(HTC::parse(bytes,sizeof(bytes),packet) == Result::Invalid);
    bytes[4] = 3; bytes[8] = 1; bytes[9] = 1;
    assert(HTC::parse(bytes,sizeof(bytes),packet) == Result::Success && packet.length == 0);
    bytes[9] = 2; assert(HTC::parse(bytes,sizeof(bytes),packet) == Result::Invalid);
    bytes[0] = HTC::MaximumEndpoints; assert(HTC::parse(bytes,sizeof(bytes),packet) == Result::Invalid);
    // Bounded deterministic malformed corpus, also run with ASan/UBSan.
    uint32_t seed = 1; uint8_t corpus[128];
    for (unsigned n = 0; n < 10000; ++n) {
        for (auto &byte : corpus) { seed = seed * 1664525U + 1013904223U; byte = uint8_t(seed >> 24); }
        const size_t length = n % sizeof(corpus);
        QMI::Message message = {};
        if (QMI::parse(corpus,length,message) == Result::Success) {
            uint16_t error; QMI::responseStatus(message,error);
        }
        HTC::parse(corpus,length,packet);
        // Keep the outer framing valid too, so fuzzing reaches TLV and
        // trailer validation rather than almost always rejecting the header.
        if (length >= QMI::HeaderBytes) {
            corpus[0] = QMI::Response; write16(corpus + 1,1);
            write16(corpus + 3,QMI::Capabilities);
            write16(corpus + 5,uint16_t(length - QMI::HeaderBytes));
            assert(QMI::parse(corpus,length,message) == Result::Success);
            uint16_t error; QMI::responseStatus(message,error);
        }
        if (length >= HTC::HeaderBytes) {
            corpus[0] = uint8_t(n % HTC::MaximumEndpoints); corpus[1] = n & 1 ? 2 : 0;
            write16(corpus + 2,uint16_t(length - HTC::HeaderBytes));
            corpus[4] = uint8_t(n % (length - HTC::HeaderBytes + 1));
            HTC::parse(corpus,length,packet);
        }
    }
}
int main() { firmware(); htc(); puts("WLAN: QMI bounds, response correlation, TLVs, HTC framing and malformed corpus passed"); }
