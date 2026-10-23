// SPDX-License-Identifier: BSD-2-Clause
#pragma once
#include "firmware_image.h"
#include "../../drivers/usb/QcomUSB/USBFirmware.h"
#include "../../drivers/usb/QcomUSB/DWC3Core.h"

namespace BridgeFirmware {
// IO performs ordered, bounded core/GCC accesses and bounded delays. Only the
// old controller stop/masks and its dedicated BCR can be written. Inherited
// clocks, PHYs, NoC votes and SMMU translations are never changed by this owner.
template<class IO> class HardwareStop {
    IO &io;
    Memory memory;
    USB::FirmwareController binding{};
    USB::HardwareStop record{};
    USB::FirmwareStop software{};
    uint8_t *output=nullptr;
    bool prepared=false,published=false;
    static constexpr uint32_t ClockOffsets[]={0x82024,0x5018,0xf008,0xf010,0xf00c};
    struct CoreIO {
        IO &io;
        uint32_t read(uint32_t offset) {return io.readCore(offset);}
        void write(uint32_t offset,uint32_t value) {io.writeCore(offset,value);}
        void delay(unsigned us) {io.delay(us);}
    } coreIO{io};
    bool fail() {
        if(output)for(unsigned n=0;n<sizeof(record);++n)output[n]=0;
        prepared=published=false;return false;
    }
public:
    constexpr explicit HardwareStop(IO &access):io(access) {}
    bool prepare(EFI_BOOT_SERVICES *services,const USB::FirmwareController &controller,void *destination,unsigned bytes) {
        if (prepared || published || !controller.valid() || controller.base!=0xa800000 ||
            !controller.contains(0xa800000,DWC3::Aperture) || bytes!=sizeof(record) || !memory.prepare(services) ||
            !memory.contains(destination,bytes,1u<<EfiLoaderData)) return false;
        output=static_cast<uint8_t *>(destination);fail();binding=controller;record={};
        record.controller=binding.base;record.length=binding.length;prepared=true;return true;
    }
    bool finish(const USB::FirmwareStop &stop) {
        if (!prepared || !stop.valid() || stop.controller!=binding.base || stop.length!=binding.length) return fail();
        if (published) return equal(output,&record,sizeof(record)) && equal(&stop,&software,sizeof(stop)) &&
            io.readGCC(record.resetOffset)==record.resetAfter ? true:fail();
        software=stop;
        record.resetBefore=io.readGCC(record.resetOffset);record.domain=io.readGCC(0xf004);
        if (record.resetBefore==UINT32_MAX || record.domain==UINT32_MAX) return fail();
        if (record.resetBefore&1) record.flags=USB::HardwareStop::Required|USB::HardwareStop::InheritedReset;
        else {
            if ((record.domain&0x80000001u)!=0x80000000u) return fail();
            uint32_t clocks[5]{};
            for(unsigned n=0;n<5;++n) {
                clocks[n]=io.readGCC(ClockOffsets[n]);
                if(clocks[n]==UINT32_MAX || (clocks[n]&0x80000001u)!=1) return fail();
            }
            DWC3::Core<CoreIO> core(coreIO);
            if(core.probe(DWC3::Aperture,40)!=USB::Result::Success || core.halt()!=USB::Result::Success || !core.stopped()) return fail();
            record.previousRole=unsigned(core.hardwareRole());
            for(unsigned n=0;n<5;++n)if(io.readGCC(ClockOffsets[n])!=clocks[n])return fail();
            if(io.readGCC(0xf004)!=record.domain || io.readGCC(record.resetOffset)!=record.resetBefore)return fail();
            if(core.verifyStopped()!=USB::Result::Success)return fail();
            // Halt is a prerequisite, never inferred from the BCR we write.
            io.writeGCC(record.resetOffset,record.resetBefore|1u);io.delay(1);
            record.flags=USB::HardwareStop::Required;
        }
        record.resetAfter=io.readGCC(record.resetOffset);
        if(!record.valid())return fail();
        const auto *p=reinterpret_cast<const uint8_t *>(&record);
        for(unsigned n=0;n<sizeof(record);++n)output[n]=p[n];
        published=true;return true;
    }
};
}
