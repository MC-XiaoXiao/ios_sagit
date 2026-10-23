// SPDX-License-Identifier: GPL-2.0-only
#pragma once
#include <QcomPowerHardware.h>
#include "RPMInterconnect.h"

namespace QcomPower {
constexpr const char *USBClockBaselineProperty="qcom,rpm-usb-core-clock-hz";
template<class Regulators> Status validateUSBClockBaseline(const Regulators &regulators,uint32_t targetHz,uint64_t inheritedHz) {
    if(!usbCoreCXLevel(targetHz))return Invalid;
    if(!inheritedHz)return NotReady;
    const auto covered=usbCoreCoveredClock(targetHz,inheritedHz);
    if(!covered)return Unsupported;
    uint32_t floor=0;const auto status=regulators.minimumDomainLevel(0x1000,0,floor);
    if(status!=OK)return status;
    return floor>=usbCoreCXLevel(covered) ? OK:Unsupported;
}
// The caller first validates the consumer operation and declares complete
// boot policy. A cache observation never substitutes for a successful ACK.
// Baseline acknowledgement runs outside the consumer's vote transaction.
template<class Transport,class Regulators,class Clocks,class Interconnect,class Preflight>
Status initializeRPMResources(Transport &transport,Regulators &regulators,
    Clocks &clocks,Interconnect &interconnect,bool bus,Preflight preflight) {
    if(bus && (!clocks.configured() || !regulators.domainsConfigured()))return Unsupported;
    if(bus)for(unsigned i=0;i<3;++i)for(unsigned set=0;set<2;++set) {
        const auto &r=rpmBusResources[i];uint32_t request=0,cap=0;uint64_t floor=0,maximum=0;
        if(clocks.floor(r.type,r.resource,0x007a484b,set,request)!=OK || clocks.limit(r.type,r.resource,set,cap)!=OK ||
            interconnect.floor(r.id,set,floor)!=OK || interconnect.limit(r.id,true,maximum)!=OK)return Unsupported;
        if(floor!=uint64_t(request)*1000 || (cap && maximum>uint64_t(cap)*1000))return Invalid;
    }
    // Every consumer, including voltage-only requests, must preserve the
    // USB clock still running under the declared common AP boot policy.
    const auto proof=preflight();if(proof!=OK)return proof;
    if(!transport.ready()) {
        const auto status=transport.connect();if(status!=OK)return status;
    }
    auto status=transport.check();if(status!=OK)return status;
    auto send=[&](uint32_t type,uint32_t id,unsigned set,const uint8_t *data,uint32_t bytes) {
        return transport.write(type,id,set,data,bytes);
    };
    status=regulators.activateRails(send);if(status!=OK)return status;
    if(regulators.domainsConfigured()) {
        status=regulators.activateDomains(send);if(status!=OK)return status;
    }
    if(bus) {
        // Global scaling follows ACKs for every declared rail/domain/clock
        // and each owned interconnect baseline, in both active/sleep sets.
        status=clocks.activate(send,[&] {return interconnect.activate(send);});
        if(status!=OK)return status;
        status=interconnect.activate(send);if(status!=OK)return status;
    }
    // A close queued with the last ACK also revokes a cached/no-send success.
    return transport.check();
}
template<class Transport,class Regulators,class Clocks,class Interconnect>
Status initializeRPMResources(Transport &transport,Regulators &regulators,
    Clocks &clocks,Interconnect &interconnect,bool bus) {
    return initializeRPMResources(transport,regulators,clocks,interconnect,bus,[]{return OK;});
}
// A transient vote by another consumer cannot authorize a faster USB clock:
// its release would expose the lower boot floor. Check immutable policy
// before connecting, then establish the normal complete ACKed baseline.
template<class Transport,class Regulators,class Clocks,class Interconnect,class Preflight>
Status prepareUSBRPMResources(Transport &transport,Regulators &regulators,
    Clocks &clocks,Interconnect &interconnect,uint32_t coreHz,Preflight preflight) {
    const uint32_t required=usbCoreCXLevel(coreHz);uint32_t floor=0;
    if(!required)return Invalid;
    const auto status=regulators.minimumDomainLevel(0x1000,0,floor);
    if(status!=OK)return status;
    if(floor<required)return Unsupported;
    return initializeRPMResources(transport,regulators,clocks,interconnect,true,preflight);
}
template<class Transport,class Regulators,class Clocks,class Interconnect>
Status prepareUSBRPMResources(Transport &transport,Regulators &regulators,
    Clocks &clocks,Interconnect &interconnect,uint32_t coreHz) {
    return prepareUSBRPMResources(transport,regulators,clocks,interconnect,coreHz,[]{return OK;});
}
}
