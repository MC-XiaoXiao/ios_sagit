// SPDX-License-Identifier: BSD-2-Clause
#include <IORT.h>
#include <cassert>
#include <cstdio>
#include <cstring>
#include <vector>
#include <utility>
using SMMU::IORTResult;
static void word(std::vector<uint8_t> &b,unsigned offset,uint32_t value) {
    for (unsigned i=0;i<4;++i) b[offset+i]=uint8_t(value>>(8*i));
}
static void checksum(std::vector<uint8_t> &b) {
    b[9]=0;unsigned sum=0;
    for (unsigned i=0;i<SMMU::iort_detail::word(b.data()+4);++i) sum+=b[i];
    b[9]=uint8_t(-sum);
}
std::vector<uint8_t> USBIORTFixture() {
    // Two nodes: a SMMUv2 with two IRQs, followed by a named USB component.
    std::vector<uint8_t> b(48+92+64,0);
    word(b,0,0x54524f49);word(b,4,unsigned(b.size()));word(b,36,2);word(b,40,48);
    b[48]=3;b[49]=92;word(b,64,0x1680000);word(b,72,0x10000);word(b,80,1);word(b,84,1);
    word(b,88,60);word(b,92,2);word(b,96,76);
    word(b,108,263);word(b,116,261);word(b,124,396);word(b,132,397);word(b,136,1);
    b[140]=1;b[141]=64;word(b,148,1);word(b,152,44);b[168]=36;
    std::memcpy(b.data()+169,"\\_SB_.USB0",11);
    word(b,184,0);word(b,188,0);word(b,192,0x1400);word(b,196,48);
    checksum(b);return b;
}
static std::vector<uint8_t> table() { return USBIORTFixture(); }
void USBTopologyCaptureTests();
void USBFirmwareRPMTests();
void USBFirmwareRPMPinTest(const char *);
void USBFirmwareStopTests();
void USBFirmwareStopPinTest(const char *);
void USBRPMColdTests();
void USBHardwareStopTests();
void USBRPMColdPinTest(const char *);
int main(int argc,char **argv) {
    SMMU::IORTTopology topology{};auto b=table();
    auto parse=[&](const char *name="\\_SB_.USB0",unsigned input=0) {
        return SMMU::parseIORT(b.data(),b.size(),name,input,topology);
    };
    assert(parse()==IORTResult::Success && topology.base==0x1680000 && topology.span==0x10000 &&
        topology.stream==0x1400 && topology.contextCount==2 && topology.interrupts[0]==396 &&
        topology.interrupts[1]==397 && topology.interruptFlags[1]==1 && topology.addressBits==36 && !topology.coherent);
    assert(parse("\\_SB_.OTHER")==IORTResult::NotFound && !topology.base);
    assert(parse("\\_SB_.USB0",1)==IORTResult::NotFound);
    word(b,188,7);checksum(b);
    assert(parse("\\_SB_.USB0",7)==IORTResult::Success && topology.stream==0x1407);
    word(b,200,1);checksum(b);
    assert(parse("\\_SB_.USB0",200)==IORTResult::Success && topology.stream==0x1400);
    for (unsigned offset:{0u,4u,9u,36u,40u,44u,49u,141u,169u,196u}) {
        b=table();b[offset]^=1;
        assert(parse()!=IORTResult::Success && !topology.base);
    }
    for (auto change:std::vector<std::pair<unsigned,uint32_t>>{
        {4,65537},{36,257},{40,47},{44,1},{88,59},{92,129},{96,0xfffffffc},
        {96,60},{148,0xffffffff},{152,12},{156,1},{160,2},{192,65536},{196,49},{200,2}}) {
        b=table();word(b,change.first,change.second);
        if (change.first!=4) checksum(b);
        assert(parse()!=IORTResult::Success && !topology.base);
    }
    // Duplicate matching mappings cannot choose an arbitrary stream.
    b=table();b.resize(b.size()+20);b[141]=84;word(b,4,unsigned(b.size()));word(b,148,2);
    std::memcpy(b.data()+204,b.data()+184,20);checksum(b);
    assert(parse()==IORTResult::Invalid);
    // Even if a different node is individually valid, duplicate names fail.
    b=table();b.resize(b.size()+64);word(b,4,unsigned(b.size()));word(b,36,3);
    std::memcpy(b.data()+204,b.data()+140,64);checksum(b);
    assert(parse()==IORTResult::Invalid);
    // Exercise byte-level corruptions with repaired checksums too: a valid
    // checksum cannot excuse malformed nested lengths, offsets or references.
    // ASan/UBSan checks every access in this deterministic mutation corpus.
    uint32_t random=0x71409a31;
    for (unsigned n=0;n<4096;++n) {
        b=table();
        for (unsigned j=0;j<3;++j) {
            random=random*1664525+1013904223;
            b[random%b.size()]^=uint8_t((random>>24)|1);
        }
        word(b,4,unsigned(b.size()));checksum(b);
        (void)parse();
    }
    USBTopologyCaptureTests();
    USBFirmwareRPMTests();
    USBFirmwareStopTests();
    USBRPMColdTests();
    USBHardwareStopTests();
    if (argc==3 && !std::strcmp(argv[1],"--npa-pin")) USBFirmwareRPMPinTest(argv[2]);
    if (argc==3 && !std::strcmp(argv[1],"--usb-config-pin")) USBFirmwareStopPinTest(argv[2]);
    if (argc==3 && !std::strcmp(argv[1],"--glink-pin")) USBRPMColdPinTest(argv[2]);
    // The real source blob has a bad checksum. Only this test models the
    // ACPI table installer's checksum repair; production never repairs input.
    if (argc==2) {
        FILE *file=std::fopen(argv[1],"rb");assert(file);
        b.resize(SMMU::IORTMaximumBytes);const auto bytes=std::fread(b.data(),1,b.size(),file);std::fclose(file);
        b.resize(bytes);assert(parse()==IORTResult::Invalid);
        checksum(b);
        assert(parse()==IORTResult::Success && topology.base==0x1680000 && topology.stream==0x1400 &&
            topology.contextCount==8 && topology.interrupts[0]==396 && topology.addressBits==36 && !topology.coherent);
        assert(parse("\\_SB_.UFS0")==IORTResult::Success && topology.stream==0x1440);
    }
    puts("USB IORT: checksummed named-component topology, stream ranges, bounded IRQs and malformed/ambiguous table rejection passed");
}
