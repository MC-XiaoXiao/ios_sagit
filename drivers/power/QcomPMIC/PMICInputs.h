// SPDX-License-Identifier: GPL-2.0-only
#pragma once
#include <stdint.h>
namespace QcomPMICInput {
constexpr unsigned MaxIRQs = 64;
struct IRQ { uint32_t sid, peripheral, bit, trigger; };
struct Button { uint32_t usage, kind, sid, line, debounce; };
struct GPIORequest { uint32_t sid, pin, pull, output, value; };
inline bool validTrigger(unsigned t) { return t==1 || t==2 || t==3 || t==4 || t==8; }
inline bool valid(const IRQ &r) { return r.sid<16 && r.peripheral<256 && r.bit<8 && validTrigger(r.trigger); }
inline bool valid(const Button &b) {
    return (b.usage==0x30 || b.usage==0xe9 || b.usage==0xea) && b.sid<16 &&
        b.debounce>=1 && b.debounce<=100 && ((b.kind==0 && b.line<2) ||
        (b.kind==1 && b.line>=1 && b.line<=26));
}
inline IRQ interrupt(const Button &b) {
    return {b.sid,b.kind==0 ? 8U : 0xc0U+b.line-1,b.kind==0 ? b.line : 0U,3};
}
template<class Bus> class Hardware {
    Bus &bus;
    bool read8(unsigned sid, unsigned addr, uint8_t &v) { return bus.read(sid,addr,&v,1); }
    bool write8(unsigned sid, unsigned addr, uint8_t v) { return bus.write(sid,addr,&v,1); }
    bool update(unsigned sid,unsigned addr,uint8_t mask,uint8_t value) {
        uint8_t old;
        return read8(sid,addr,old) && (((old&mask)==value) || write8(sid,addr,(old&~mask)|value));
    }
public:
    explicit Hardware(Bus &access) : bus(access) {}
    bool mask(const IRQ &r) { return valid(r) && write8(r.sid,(r.peripheral<<8)+0x16,1U<<r.bit); }
    bool unmask(const IRQ &r) { return valid(r) && write8(r.sid,(r.peripheral<<8)+0x15,1U<<r.bit); }
    bool ack(const IRQ &r) { return valid(r) && write8(r.sid,(r.peripheral<<8)+0x14,1U<<r.bit); }
    bool exclusive(const IRQ &r,unsigned owned) {
        uint8_t enabled;
        return valid(r) && read8(r.sid,(r.peripheral<<8)+0x15,enabled) && !(enabled&~owned);
    }
    bool configureIRQ(const IRQ &r) {
        if (!valid(r) || !mask(r) || !ack(r)) return false;
        uint8_t mode[3]; unsigned base=(r.peripheral<<8)+0x11, bit=1U<<r.bit;
        if (!bus.read(r.sid,base,mode,3)) return false;
        for (auto &v : mode) v &= ~bit;
        if (r.trigger&3) mode[0] |= bit;
        if ((r.trigger&1) || r.trigger==4) mode[1] |= bit;
        if ((r.trigger&2) || r.trigger==8) mode[2] |= bit;
        return bus.write(r.sid,base,mode,3);
    }
    bool gpioIdentity(unsigned sid,unsigned pin,bool &lv) {
        if (sid>15 || pin<1 || pin>26) return false;
        uint8_t id[2];
        if (!bus.read(sid,0xc000+0x100*(pin-1)+4,id,2) || id[0]!=0x10) return false;
        lv=id[1]>=0x10 && id[1]<=0x13;
        return lv || id[1]==1 || id[1]==5 || id[1]==9 || id[1]==13;
    }
    bool configureGPIO(unsigned sid,unsigned pin,bool output,bool value,unsigned pull) {
        bool lv;
        if (pull>5 || !gpioIdentity(sid,pin,lv)) return false;
        unsigned base=0xc000+0x100*(pin-1);
        // Retain firmware VIN/drive strength; never change the pad supply.
        // Disable the pad before switching its function/direction.
        if (!update(sid,base+0x46,0x80,0) || !update(sid,base+0x42,7,pull) ||
            !write8(sid,base+0x43,0)) return false;
        if (lv) {
            if (!write8(sid,base+0x44,value ? 0x80 : 0) ||
                !update(sid,base+0x40,3,output ? 1 : 0)) return false;
        } else if (!write8(sid,base+0x40,(output ? 0x10 : 0)|(value ? 1 : 0))) return false;
        return update(sid,base+0x46,0x80,0x80);
    }
    bool readGPIO(unsigned sid,unsigned pin,bool &value) {
        bool lv; uint8_t enabled,mode,status,source;
        if (!gpioIdentity(sid,pin,lv)) return false;
        unsigned base=0xc000+0x100*(pin-1);
        if (!read8(sid,base+0x46,enabled) || !(enabled&0x80) || !read8(sid,base+0x40,mode)) return false;
        unsigned direction=lv ? mode&3 : (mode>>4)&7;
        if (direction==0 || direction==2) {
            if (!read8(sid,base+0x10,status)) return false;
            value=status&1; return true;
        }
        if (direction!=1) return false;
        if (lv) { if (!read8(sid,base+0x44,source) || (source&15)) return false; value=source&0x80; }
        else { if (mode&14) return false; value=mode&1; }
        return true;
    }
    bool prepareButton(const Button &b) {
        if (!valid(b)) return false;
        if (b.kind==1) return configureGPIO(b.sid,b.line,false,false,0); // 30uA pull-up
        uint8_t subtype;
        if (!read8(b.sid,0x805,subtype) || (subtype!=1 && subtype!=3 && subtype!=4)) return false;
        // Only pulls are changed. PON debounce and long-hold reset configuration
        // belong to firmware; per-key debounce is implemented in the HID driver.
        unsigned bit=b.line==0 ? 2 : 1;
        return update(b.sid,0x870,bit,bit);
    }
    bool readButton(const Button &b,bool &down) {
        if (!valid(b)) return false;
        uint8_t value;
        if (b.kind==0) { if (!read8(b.sid,0x810,value)) return false; down=value&(1U<<b.line); return true; }
        bool high;
        if (!readGPIO(b.sid,b.line,high)) return false;
        down=!high; return true;
    }
};
}
