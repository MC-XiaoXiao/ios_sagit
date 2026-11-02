// SPDX-License-Identifier: GPL-2.0-only
#pragma once
#include <stdint.h>

namespace TouchDiagnostic {
// Versioned local diagnostic ABI. Coordinates are normalized sensor coordinates,
// before the normal TouchHID rotation. Only an opted-in root client can send it.
struct Gesture {
    uint16_t version, fromX, fromY, toX, toY, durationMS;
    uint8_t pressure, width, height, reserved;
    bool valid() const {
        return version==1 && fromX<=32767 && fromY<=32767 && toX<=32767 && toY<=32767 &&
            durationMS>=80 && durationMS<=1000 && pressure && width && height && !reserved;
    }
};
static_assert(sizeof(Gesture)==16,"touch diagnostic gesture ABI");
struct Point {uint16_t x,y;};
class Stroke {
    Gesture request{};
    uint64_t start=0;
    bool active=false,endpoint=false;
public:
    bool begin(const Gesture &g,uint64_t now) {
        if(active || !g.valid())return false;
        request=g;start=now;active=true;endpoint=false;return true;
    }
    void cancel() {active=false;}
    const Gesture &gesture() const {return request;}
    bool next(uint64_t now,Point &point) {
        if(!active)return false;
        const uint64_t duration=uint64_t(request.durationMS)*1000000;
        // A delayed timer must not prolong a held contact. Rollback is an abort.
        if(now<start || endpoint || now-start>duration+100000000) {cancel();return false;}
        uint64_t elapsed=now-start;
        if(elapsed>=duration){elapsed=duration;endpoint=true;}
        auto coordinate=[&](uint16_t from,uint16_t to) {
            return uint16_t(int64_t(from)+(int64_t(to)-from)*int64_t(elapsed)/int64_t(duration));
        };
        point={coordinate(request.fromX,request.toX),coordinate(request.fromY,request.toY)};
        return true;
    }
};
}
