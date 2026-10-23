// SPDX-License-Identifier: GPL-2.0-only
#include "RPMModemClocks.h"
#include <ClockControllerProtocol.h>
#include <cassert>
#include <cstdio>
using namespace QcomPower;
static void policy(RPMClockHandoff &c) {
    for(unsigned i=0; i<RPMHandoffClockCount; ++i) {
        const auto active=i==6 ? 1u:i==7 ? 19200u:i==19 ? 1u:0u;
        assert(c.configure(i,active,0));
        if(rpmClockHasLimit(i))assert(c.configureLimit(i,i==19 ? 4u:0u,i==19 ? 4u:0u));
    }
}
int main() {
    auto request=ClockController::targeted(ClockController::RPM,ClockController::SetModemClockVote,
                                         ModemQDSS,4,ClockController::BasebandConsumer,1);
    assert(ClockController::valid(request.request));
    request.consumer=ClockController::USBControllerConsumer;
    assert(!ClockController::valid(request.request));
    request.consumer=ClockController::BasebandConsumer;request.request.value=5;
    assert(!ClockController::valid(request.request));
    request.request.id=ModemXO;request.request.value=2;
    assert(!ClockController::valid(request.request));
    RPMClockHandoff c; policy(c); RPMModemClocks votes;
    unsigned writes=0; uint32_t lastType=0,lastResource=0,lastSet=0,lastValue=0,lastCap=0;
    Status result=OK;
    auto send=[&](uint32_t type,uint32_t resource,unsigned set,const uint8_t *p,uint32_t bytes) {
        ++writes;lastType=type;lastResource=resource;lastSet=set;lastValue=le32(p+8);
        assert(bytes==24 && le32(p+4)==4 && le32(p+12)==0x0078616d && le32(p+16)==4);
        assert(le32(p)==(type==0x306b6c63 && resource==1 ? 0x54415453u:0x62616e45u));
        lastCap=le32(p+20);return result;
    };
    assert(votes.validate(ModemQDSS,0,15,4,c)==OK);
    assert(votes.vote(ModemQDSS,0,15,4,c,send)==NotReady && !writes);
    assert(c.activate([](uint32_t,uint32_t,unsigned,const uint8_t *,uint32_t){return OK;})==OK);
    uint32_t applied=0;
    assert(votes.get(ModemQDSS,0,c,applied)==OK && applied==1);
    assert(votes.vote(ModemQDSS,0,15,4,c,send)==OK && writes==1);
    assert(lastType==0x306b6c63 && lastResource==1 && lastSet==0 && lastValue==4 && lastCap==4);
    assert(votes.get(ModemQDSS,0,c,applied)==OK && applied==4);
    assert(votes.vote(ModemQDSS,1,15,4,c,send)==OK && lastSet==1);
    assert(votes.vote(ModemQDSS,0,15,0,c,send)==OK && lastValue==1); // Retained boot/AP floor.
    assert(votes.get(ModemQDSS,1,c,applied)==OK && applied==4); // Sleep is independent.
    assert(votes.vote(ModemQDSS,1,15,0,c,send)==OK && lastValue==0);
    const auto before=writes;
    assert(votes.vote(ModemXO,0,15,1,c,send)==OK && writes==before); // Already ACKed enable.
    assert(votes.vote(ModemXO,1,15,1,c,send)==OK && lastType==0x306b6c63 && !lastResource && lastValue==1);
    assert(votes.vote(ModemXO,1,15,0,c,send)==OK && !lastValue);
    assert(votes.vote(ModemXO,0,15,0,c,send)==OK && votes.get(ModemXO,0,c,applied)==OK && applied==1);
    const auto unchanged=writes;
    assert(votes.vote(ModemQDSS,0,14,4,c,send)==Invalid && writes==unchanged);
    assert(votes.vote(ModemQDSS,0,15,5,c,send)==Invalid && writes==unchanged);
    assert(votes.vote(0x2002,0,15,1,c,send)==Unsupported && writes==unchanged);
    // A definite rejection keeps the acknowledged value and permits a retry.
    result=Invalid;
    assert(votes.vote(ModemQDSS,0,15,3,c,send)==Invalid);
    assert(votes.get(ModemQDSS,0,c,applied)==OK && applied==1);
    result=OK;assert(votes.vote(ModemQDSS,0,15,3,c,send)==OK);
    result=TimedOut;assert(votes.vote(ModemQDSS,0,15,0,c,send)==TimedOut);
    const auto faultWrites=writes;
    assert(votes.get(ModemQDSS,0,c,applied)==Stale);
    assert(votes.vote(ModemQDSS,0,15,0,c,send)==Stale && writes==faultWrites);
    assert(votes.get(ModemQDSS,1,c,applied)==OK && !applied);
    // A valid baseline cap can still forbid a modem's requested boot state.
    RPMClockHandoff capped;
    for(unsigned i=0;i<RPMHandoffClockCount;++i) {
        assert(capped.configure(i,i==6 ? 1u:i==7 ? 19200u:0u,0));
        if(rpmClockHasLimit(i))assert(capped.configureLimit(i,i==19 ? 2u:0u,i==19 ? 2u:0u));
    }
    RPMModemClocks limited;
    assert(limited.validate(ModemQDSS,0,15,4,capped)==Invalid);
    assert(limited.vote(ModemQDSS,0,15,4,capped,send)==Invalid && writes==faultWrites);
    std::puts("baseband RPM XO/QDSS votes/boot floors/caps/ACK quarantine checks passed");
}
