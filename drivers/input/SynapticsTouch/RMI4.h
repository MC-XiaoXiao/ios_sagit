// SPDX-License-Identifier: GPL-2.0-only
// RMI4 F01/F11/F12 protocol, based on the pinned Xiaomi Synaptics DSX source.
#pragma once
#include <stdint.h>
#include <stddef.h>
namespace RMI4 {
constexpr unsigned MaxContacts = 10, MaxIRQs = 32;
enum class Result { Success, IO, Invalid, Unsupported, Reset, Bootloader };
struct Contact {
    bool active = false;
    uint16_t x = 0, y = 0;
    uint8_t pressure = 0, width = 0, height = 0;
};
struct Frame { Contact contacts[MaxContacts]{}; };
struct Function {
    uint16_t query = 0, command = 0, control = 0, data = 0;
    uint32_t irqs = 0;
    uint8_t number = 0;
};
inline uint16_t le16(const uint8_t *p) { return p[0] | uint16_t(p[1]) << 8; }
inline bool range(uint16_t address, size_t length) {
    return length && length <= 255 && (address & 255) + length <= 255;
}
inline bool present(const uint8_t *query, unsigned length, unsigned bit) {
    return 1 + bit/8 < length && (query[1+bit/8] & (1U << (bit%8)));
}
inline unsigned offset(const uint8_t *query, unsigned length, unsigned bit) {
    unsigned value = 0;
    for (unsigned i = 0; i < bit; ++i) value += present(query,length,i);
    return value;
}

// Bus operations return false on error and never retry writes implicitly.
template<class Bus> class Device {
    Bus &bus;
    Function f01{}, touch{};
    uint8_t irqBytes = 0, objects = 0;
    uint16_t dataAddress = 0, reportAddress = 0;
    uint8_t savedControl = 0, savedIRQs[4]{}, savedReport = 0;
    bool modified = false, reportModified = false, ready = false;
    bool read(uint16_t a, uint8_t *p, size_t n) { return range(a,n) && bus.read(a,p,n); }
    bool write(uint16_t a, const uint8_t *p, size_t n) { return range(a,n) && bus.write(a,p,n); }
    Result status(uint8_t value) const {
        if (value & 0x40) return Result::Bootloader;
        if ((value & 0x80) || (value & 15) == 1) return Result::Reset;
        return (value & 15) ? Result::IO : Result::Success;
    }
    Result discover() {
        f01 = {}; touch = {};
        Function f11{}, f12{};
        unsigned interrupts = 0;
        for (unsigned page = 0; page < 10; ++page) {
            for (int pdt = 0xe9; pdt >= 5; pdt -= 6) {
                uint8_t d[6];
                if (!read((page << 8) | pdt,d,6)) return Result::IO;
                if (!d[5]) break;
                unsigned count = d[4] & 7;
                if (interrupts + count > MaxIRQs) return Result::Unsupported;
                Function f{uint16_t((page<<8)|d[0]),uint16_t((page<<8)|d[1]),
                    uint16_t((page<<8)|d[2]),uint16_t((page<<8)|d[3]),
                    count ? uint32_t(((uint64_t(1)<<count)-1)<<interrupts) : 0,d[5]};
                Function *target = d[5]==1 ? &f01 : d[5]==0x11 ? &f11 : d[5]==0x12 ? &f12 : nullptr;
                if (target) { if (target->number) return Result::Unsupported; *target = f; }
                interrupts += count;
            }
        }
        irqBytes = (interrupts+7)/8;
        touch = f12.number ? f12 : f11;
        return f01.number && irqBytes ? Result::Success : Result::Unsupported;
    }
public:
    uint16_t maxX = 0, maxY = 0;
    char product[11]{};
    explicit Device(Bus &transport) : bus(transport) {}
    unsigned contactCount() const { return objects; }
    unsigned function() const { return touch.number; }
    uint16_t reportDataAddress() const { return dataAddress; }
    uint8_t lastRaw[MaxContacts*8]{}, lastStatus[5]{};
    unsigned lastRawLength=0;
    void deviceReset() { ready=modified=reportModified=false; }
    Result initialize() {
        // A failed rollback must not become the next "original" configuration.
        if (modified && !restore()) return Result::IO;
        ready = modified = reportModified = false;
        maxX = maxY = 0;
        uint8_t s;
        unsigned crcWaits=0;
        for (unsigned scans=0;;++scans) {
            auto result=discover();
            if (result!=Result::Success) return result;
            bool waited=false;
            for (;;) {
                if (!read(f01.data,&s,1)) return Result::IO;
                if ((s&15)!=6) break;
                if (++crcWaits>10) return Result::IO;
                waited=true;bus.delayMS(10);
            }
            if (!waited) break;
            // CRC completion can replace the PDT: never configure its old bases.
            if (scans==2) return Result::IO;
        }
        if (s&0x40) return Result::Bootloader;
        if ((s&15)>1) return Result::IO;
        if (!touch.number || !touch.irqs) return Result::Unsupported;
        uint8_t identity[21];
        if (!read(f01.query,identity,sizeof(identity))) return Result::IO;
        if (identity[0]!=1) return Result::Unsupported;
        for (unsigned i=0;i<10;++i) product[i]=identity[11+i]>=32 && identity[11+i]<127 ? identity[11+i] : '_';
        product[10]=0;
        if (touch.number==0x12) {
            uint8_t size, controls[9]{}, data[5]{}, bounds[4], objectConfig[2];
            if (!read(touch.query+4,&size,1)) return Result::IO;
            if (size<5 || size>sizeof(controls)) return Result::Unsupported;
            if (!read(touch.query+5,controls,size)) return Result::IO;
            unsigned controlSize=size;
            if (!present(controls,size,23) || !present(controls,size,28) ||
                (!present(controls,size,8) && !present(controls,size,31))) return Result::Unsupported;
            if (!read(touch.control+offset(controls,size,23),objectConfig,2)) return Result::IO;
            objects=objectConfig[1];
            if (!(objectConfig[0]&1) || !objects || objects>MaxContacts) return Result::Unsupported;
            unsigned boundControl=present(controls,size,8) ? 8 : 31;
            if (!read(touch.control+offset(controls,size,boundControl),bounds,4)) return Result::IO;
            maxX=le16(bounds);maxY=le16(bounds+2);
            reportAddress=touch.control+offset(controls,controlSize,28);
            if (!read(touch.query+7,&size,1)) return Result::IO;
            if (size<2 || size>sizeof(data)) return Result::Unsupported;
            if (!read(touch.query+8,data,size)) return Result::IO;
            if (!present(data,size,1)) return Result::Unsupported;
            dataAddress=touch.data+present(data,size,0);
            if (!range(dataAddress,objects*8)) return Result::Unsupported;
            if (!read(reportAddress,&savedReport,1)) return Result::IO;
        } else {
            uint8_t query[6], bounds[4];
            if (!read(touch.query,query,6) || !read(touch.control+6,bounds,4)) return Result::IO;
            unsigned count=query[1]&7;
            if (count>5 || !(query[1]&0x10) || (query[5]&3)) return Result::Unsupported;
            objects=count==5 ? 10 : count+1;
            maxX=bounds[0] | uint16_t(bounds[1]&15)<<8;
            maxY=bounds[2] | uint16_t(bounds[3]&15)<<8;
            dataAddress=touch.data;
            if (!range(dataAddress,(objects+3)/4+objects*5)) return Result::Unsupported;
        }
        if (!maxX || !maxY) return Result::Invalid;
        if (!read(f01.control,&savedControl,1) || !read(f01.control+1,savedIRQs,irqBytes)) return Result::IO;
        modified=true; // also restore after a partial write/failure
        if (touch.number==0x12) {
            uint8_t reports=0xff; // object, XY, Z, WX, WY; exactly 8 bytes/object
            reportModified=true;
            if (!write(reportAddress,&reports,1)) { restore(); return Result::IO; }
        }
        uint32_t mask=f01.irqs|touch.irqs;
        uint8_t enables[4]{}, control=(savedControl&~3U)|0x80;
        for (unsigned i=0;i<irqBytes;++i) enables[i]=mask>>(8*i);
        if (!write(f01.control+1,enables,irqBytes) || !write(f01.control,&control,1)) { restore();return Result::IO; }
        ready=true;
        return Result::Success;
    }
    bool restore() {
        ready=false;
        bool ok=true;
        if (modified) {
            if (reportModified && !write(reportAddress,&savedReport,1)) ok=false;
            if (!write(f01.control+1,savedIRQs,irqBytes)) ok=false;
            if (!write(f01.control,&savedControl,1)) ok=false;
        }
        if (ok) modified=reportModified=false;
        return ok;
    }
    Result sample(Frame &frame) {
        if (!ready) return Result::Invalid;
        uint8_t state[5]{};
        if (!read(f01.data,state,irqBytes+1)) return Result::IO; // acknowledges IRQs
        for(unsigned i=0;i<irqBytes+1U;++i)lastStatus[i]=state[i];
        auto result=status(state[0]);
        if (result!=Result::Success) return result;
        uint8_t bytes[MaxContacts*8]{};
        const unsigned prefix=touch.number==0x11 ? (objects+3)/4 : 0;
        unsigned length=prefix+objects*(touch.number==0x12 ? 8 : 5);
        if (!read(dataAddress,bytes,length)) return Result::IO;
        lastRawLength=length;
        for(unsigned i=0;i<length;++i)lastRaw[i]=bytes[i];
        Frame next;
        for (unsigned i=0;i<objects;++i) {
            auto &c=next.contacts[i];
            if (touch.number==0x12) {
                auto *p=bytes+8*i;
                // Palm, stylus, hover and unknown object types never become fingers.
                c.active=p[0]==1 || p[0]==6;
                if (!c.active) continue;
                c.x=le16(p+1);c.y=le16(p+3);c.pressure=p[5];c.width=p[6];c.height=p[7];
            } else {
                unsigned state=(bytes[i/4]>>(2*(i%4)))&3;
                if (state==3) return Result::Invalid;
                c.active=state==1 || state==2;
                if (!c.active) continue;
                auto *p=bytes+prefix+5*i;
                c.x=(uint16_t(p[0])<<4)|(p[2]&15);c.y=(uint16_t(p[1])<<4)|(p[2]>>4);
                c.width=p[3]&15;c.height=p[3]>>4;c.pressure=p[4];
            }
            if (c.x>maxX || c.y>maxY) return Result::Invalid;
        }
        frame=next; // failures must never partially overwrite a valid frame
        return Result::Success;
    }
};
}
