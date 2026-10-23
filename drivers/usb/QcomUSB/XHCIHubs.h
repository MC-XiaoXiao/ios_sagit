// SPDX-License-Identifier: BSD-2-Clause
#pragma once
#include "XHCIContexts.h"
#include <IOKit/usb/StandardUSB.h>

namespace XHCI {
struct HubProfile {
    unsigned ports=0,think=0;
    bool multi=false;
};
inline USB::Result hubProfile(const StandardUSB::HubDescriptor *d,USB::Speed speed,unsigned flags,HubProfile &out) {
    if (!d || d->bDescriptorType!=0x29 || !d->bNumberPorts || d->bNumberPorts>15 ||
        d->bLength<7+2*((unsigned(d->bNumberPorts)+8)/8) || flags&~1u) return USB::Result::Invalid;
    if (speed!=USB::Speed::Full && speed!=USB::Speed::High) return USB::Result::Invalid;
    if (flags && speed!=USB::Speed::High) return USB::Result::Invalid;
    out={d->bNumberPorts,speed==USB::Speed::High ? unsigned((d->wHubCharacteristics>>5)&3) : 0,bool(flags&1)};
    return USB::Result::Success;
}
inline USB::Result hubProfile(const StandardUSB::SuperSpeedHubDescriptor *d,USB::Speed speed,HubProfile &out) {
    if (!d || d->bDescriptorType!=0x2a || d->bLength<12 || !d->bNumberPorts || d->bNumberPorts>15 ||
        speed!=USB::Speed::Super) return USB::Result::Invalid;
    out={d->bNumberPorts,0,false};return USB::Result::Success;
}
inline unsigned routeDepth(uint32_t route) { unsigned n=0;while (route) { ++n;route>>=4; }return n; }
inline uint32_t parentLocation(uint32_t location,unsigned depth) {
    return depth && depth<=5 ? location&~(15u<<(20-4*depth)) : 0;
}
inline bool sameHubProfile(const SlotConfiguration &a,const SlotConfiguration &b) {
    return a.hub==b.hub && a.ports==b.ports && a.ttThink==b.ttThink && a.multiTT==b.multiTT;
}

// This is the HCD Device's actual topology state, not a second shadow ledger.
// A parent lease is acquired before the first sleeping preparation/command.
// detach is called only after Disable Slot/isolation AND successful DMA unmap.
struct TopologyNode {
    SlotConfiguration configuration;
    uint32_t location;
    unsigned slot=0,children=0;
    TopologyNode *parent=nullptr;
    bool retiring=false,hubRegistered=false,ttResetRequired=false;
    explicit TopologyNode(uint32_t identifier):location(identifier) {}
    bool descendsFrom(const TopologyNode &ancestor) const {
        auto *at=parent;
        for (unsigned n=0;at && n<5;++n,at=at->parent) if (at==&ancestor) return true;
        return false;
    }
    bool ancestorsReady() const {
        auto *at=parent;
        for (unsigned n=0;at && n<5;++n,at=at->parent) if (at->retiring || !at->hubRegistered) return false;
        return at==nullptr && !retiring;
    }
    const TopologyNode *ttOwner() const {
        auto *at=parent;
        for (unsigned n=0;at && n<5;++n,at=at->parent) if (at->slot==configuration.ttHub)
            return at->configuration.speed==USB::Speed::High ? at : nullptr;
        return nullptr;
    }
    bool ttDomainAlive() const {
        const auto *owner=ttOwner();
        return owner && owner->hubRegistered && owner->ancestorsReady();
    }
    void unregisterHub(bool hadTTDescendants) {
        hubRegistered=false;
        // Unregister does not issue a physical hub/TT reset. If translated
        // devices are being torn down, require a new USB device/slot lifetime
        // before accepting this same HS hub again with possibly stale state.
        ttResetRequired|=hadTTDescendants && configuration.speed==USB::Speed::High;
    }
    const TopologyNode *root() const {
        const auto *at=this;
        for (unsigned n=0;at->parent && n<5;++n) at=at->parent;
        return at->parent ? nullptr : at;
    }
    USB::Result initialize(const FamilyLocation &path,TopologyNode *upstream,USB::Speed speed,
        const PortProtocol &protocol,unsigned rootSpeedID) {
        if (configuration.speed!=USB::Speed::Unknown || parent || slot || children) return USB::Result::Busy;
        FamilyLocation actual;
        if (path.bus>255 || decodeFamilyLocation(location,path.bus<<24,15,actual)!=USB::Result::Success ||
            actual.rootPort!=path.rootPort || actual.route!=path.route || actual.depth!=path.depth ||
            !path.rootPort || path.depth>5 || routeDepth(path.route)!=path.depth ||
            (protocol.major!=2 && protocol.major!=3) || !rootSpeedID || rootSpeedID>15 ||
            protocol.speeds[rootSpeedID]==USB::Speed::Unknown ||
            (protocol.major==3 ? speed!=USB::Speed::Super : speed==USB::Speed::Super)) return USB::Result::Invalid;
        SlotConfiguration c;c.rootPort=path.rootPort;c.route=path.route;c.speed=speed;
        if (!upstream) {
            if (path.depth || protocol.speeds[rootSpeedID]!=speed) return USB::Result::Invalid;
            c.speedID=rootSpeedID;
        } else {
            const auto &p=upstream->configuration;
            const unsigned port=(path.route>>(4*(path.depth ? path.depth-1 : 0)))&15;
            if (!path.depth || upstream->location!=parentLocation(location,path.depth) ||
                routeDepth(p.route)+1!=path.depth || p.rootPort!=path.rootPort ||
                (path.route&((1u<<(4*(path.depth-1)))-1))!=p.route || !port)
                return USB::Result::Invalid;
            if (!upstream->slot || !p.hub || !upstream->hubRegistered || !upstream->ancestorsReady() ||
                upstream->children==UINT32_MAX) return USB::Result::NotReady;
            if (port>p.ports) return USB::Result::Invalid;
            const auto *root=upstream->root();
            if (!root || root->configuration.speedID!=rootSpeedID) return USB::Result::NotReady;
            if ((p.speed==USB::Speed::Super && speed!=USB::Speed::Super) ||
                (p.speed==USB::Speed::High && speed!=USB::Speed::High && speed!=USB::Speed::Full && speed!=USB::Speed::Low) ||
                (p.speed==USB::Speed::Full && speed!=USB::Speed::Full && speed!=USB::Speed::Low) ||
                (p.speed!=USB::Speed::Super && p.speed!=USB::Speed::High && p.speed!=USB::Speed::Full)) return USB::Result::Invalid;
            for (unsigned n=1;n<16;++n) if (protocol.speeds[n]==speed) {
                if (c.speedID) return USB::Result::Unsupported;
                c.speedID=n;
            }
            if (!c.speedID) return USB::Result::Unsupported;
            if (speed==USB::Speed::Low || speed==USB::Speed::Full) {
                if (p.speed==USB::Speed::High) { c.ttHub=upstream->slot;c.ttPort=port;c.multiTT=p.multiTT; }
                else if (p.ttHub) {
                    // A full-speed hub has MTT clear in its OWN context. Find
                    // the real high-speed ancestor to recover the TT policy.
                    const auto *tt=upstream;
                    for (unsigned n=0;tt && n<5 && tt->slot!=p.ttHub;++n) tt=tt->parent;
                    if (!tt || tt->slot!=p.ttHub || !tt->hubRegistered || tt->configuration.speed!=USB::Speed::High)
                        return USB::Result::Invalid;
                    c.ttHub=p.ttHub;c.ttPort=p.ttPort;c.multiTT=tt->configuration.multiTT;
                }
            }
        }
        configuration=c;parent=upstream;if (parent) ++parent->children;
        return USB::Result::Success;
    }
    USB::Result hubCandidate(const HubProfile &profile,SlotConfiguration &out) const {
        if (ttResetRequired) return USB::Result::NotReady;
        if (!profile.ports || profile.ports>15 || profile.think>3 || routeDepth(configuration.route)>=5 ||
            configuration.speed==USB::Speed::Low || configuration.speed==USB::Speed::Unknown ||
            (configuration.speed!=USB::Speed::High && (profile.multi || profile.think))) return USB::Result::Invalid;
        auto c=configuration;c.hub=true;c.ports=profile.ports;c.ttThink=profile.think;
        c.multiTT=configuration.speed==USB::Speed::High && profile.multi;
        // Hub-related context fields remain immutable after installation.
        // Unregister closes child admission; it does not clear hardware Hub.
        if (configuration.hub && !sameHubProfile(configuration,c)) return USB::Result::Busy;
        out=c;return USB::Result::Success;
    }
    USB::Result detach() {
        if (children) return USB::Result::Busy;
        if (parent) {
            if (!parent->children) return USB::Result::Invalid;
            --parent->children;parent=nullptr;
        }
        return USB::Result::Success;
    }
};
}
