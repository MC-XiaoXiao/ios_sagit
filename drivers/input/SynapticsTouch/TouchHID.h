// SPDX-License-Identifier: BSD-2-Clause
#pragma once
#include "RMI4.h"
namespace TouchHID {
constexpr unsigned ContactBytes=9, ReportBytes=1+RMI4::MaxContacts*ContactBytes+3;
constexpr uint8_t InputID=1, FeatureID=2;
// Parallel report: ten persistent Finger collections, stable contact IDs,
// Tip/InRange/Confidence, XY, width/height, contact count and scan time.
// This panel has no pressure sensor. Its RMI strength field is diagnostic
// data, not a HID TipPressure axis. Keep one reserved byte per contact so the
// established report layout remains stable while native parsing sees the same
// field set as an uncompressed multitouch surface.
// Native IOHIDEventDriver consumes collectionDispatch as one digitizer frame.
constexpr uint8_t FingerDescriptor[]={
    0x05,0x0d,0x09,0x22,0xa1,0x02,
    0x09,0x42,0x09,0x32,0x09,0x47,0x15,0x00,0x25,0x01,0x75,0x01,0x95,0x03,0x81,0x02,
    0x75,0x05,0x95,0x01,0x81,0x03,
    0x09,0x51,0x25,0x09,0x75,0x08,0x81,0x02,
    0x05,0x01,0x09,0x30,0x09,0x31,0x26,0xff,0x7f,0x75,0x10,0x95,0x02,0x81,0x02,
    0x05,0x0d,0x09,0x48,0x09,0x49,0x26,0xff,0x00,0x75,0x08,0x95,0x02,0x81,0x02,
    0x75,0x08,0x95,0x01,0x81,0x03,
    0xc0
};
constexpr uint8_t DescriptorBegin[]={0x05,0x0d,0x09,0x04,0xa1,0x01,0x85,InputID};
constexpr uint8_t DescriptorEnd[]={
        0x05,0x0d,0x09,0x54,0x15,0x00,0x25,0x0a,0x75,0x08,0x95,0x01,0x81,0x02,
        0x09,0x56,0x27,0xff,0xff,0x00,0x00,0x75,0x10,
        0x55,0x0c,0x66,0x01,0x10,0x81,0x02, // seconds, exponent -4
        0x55,0x00,0x65,0x00, // restore unit-less feature
        0x85,FeatureID,0x09,0x55,0x25,0x0a,0x75,0x08,0x95,0x01,0xb1,0x02,0xc0
};
constexpr unsigned DescriptorBytes=sizeof(DescriptorBegin)+RMI4::MaxContacts*sizeof(FingerDescriptor)+sizeof(DescriptorEnd);
inline unsigned descriptor(uint8_t *out, unsigned capacity) {
    if (!out || capacity<DescriptorBytes) return 0;
    unsigned n=0;
    for (auto b:DescriptorBegin) out[n++]=b;
    for (unsigned i=0;i<RMI4::MaxContacts;++i) for (auto b:FingerDescriptor) out[n++]=b;
    for (auto b:DescriptorEnd) out[n++]=b;
    return n;
}
inline void put16(uint8_t *p,unsigned v) { p[0]=v;p[1]=v>>8; }
struct Transform {
    bool swap=false, invertX=false, invertY=false;
    unsigned rotation=0;
    bool valid() const { return rotation==0 || rotation==90 || rotation==180 || rotation==270; }
};
inline bool report(const RMI4::Frame &frame, uint16_t maxX, uint16_t maxY,
                   Transform transform, uint64_t ns, uint8_t *out) {
    if (!out || !maxX || !maxY || !transform.valid()) return false;
    for (unsigned i=0;i<ReportBytes;++i) out[i]=0;
    out[0]=InputID;unsigned active=0;
    for (unsigned i=0;i<RMI4::MaxContacts;++i) {
        const auto &c=frame.contacts[i];auto *p=out+1+i*ContactBytes;
        p[0]=4; // retain Finger confidence on release; only Tip/InRange clear
        p[1]=i;
        if (c.x>maxX || c.y>maxY) return false;
        unsigned x=uint32_t(c.x)*32767/maxX,y=uint32_t(c.y)*32767/maxY;
        if (transform.swap) { auto temp=x;x=y;y=temp; }
        if (transform.invertX) x=32767-x;
        if (transform.invertY) y=32767-y;
        const unsigned oldX=x;
        if (transform.rotation==90) { x=32767-y;y=oldX; }
        else if (transform.rotation==180) { x=32767-x;y=32767-y; }
        else if (transform.rotation==270) { x=y;y=32767-oldX; }
        put16(p+2,x);put16(p+4,y);p[6]=c.width;p[7]=c.height;
        if (c.active) { p[0]=7;++active; }
    }
    out[1+RMI4::MaxContacts*ContactBytes]=active;
    put16(out+ReportBytes-2,ns/100000); // HID scan time, 100 us, wraps at 16 bits
    return true;
}
// The next frame is committed only once HID accepts it. A failed release
// remains pending and prevents new contacts until delivery succeeds.
class Delivery {
    RMI4::Frame accepted{};
    bool neutralPending=false;
public:
    bool held() const { for (const auto &c:accepted.contacts) if (c.active) return true;return false; }
    bool releasing() const { return neutralPending; }
    void release() { neutralPending=true; }
    template<class Sink> bool send(const RMI4::Frame &frame,Sink sink) {
        RMI4::Frame neutral;
        auto next=neutralPending ? neutral : frame;
        // RMI clears an absent object's bytes. Native IOHIDEventDriver uses
        // the report's XY for the Finger child even on untouch; preserve the
        // last accepted position so UIKit does not end every path at (0, 0).
        for (unsigned i=0;i<RMI4::MaxContacts;++i) if (!next.contacts[i].active) {
            next.contacts[i].x=accepted.contacts[i].x;
            next.contacts[i].y=accepted.contacts[i].y;
            next.contacts[i].width=accepted.contacts[i].width;
            next.contacts[i].height=accepted.contacts[i].height;
        }
        if (!sink(next)) return false;
        accepted=next;neutralPending=false;return true;
    }
};
}
