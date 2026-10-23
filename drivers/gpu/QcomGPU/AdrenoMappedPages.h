// SPDX-License-Identifier: BSD-2-Clause
#pragma once
#include "AdrenoMMU.h"
namespace Adreno {
// Verify the prepared physical index against its retained DMA mapping before
// publishing the buffer. Its descriptor and DMA command remain prepared until
// the last job releases the buffer, so that mapping cannot change between jobs.
template<class Hardware,class Buffer> struct MappedBufferPages {
    Hardware &hardware;const Buffer *buffer;
    unsigned firstPage=0;
    typename decltype(hardware.tables)::TranslationCursor cursor{};
    QcomPower::Status page(unsigned index,uint64_t &pa) {
        if(index>UINT32_MAX-firstPage)return QcomPower::Invalid;
        const auto logical=firstPage+index;uint64_t actual=0;
        if(!buffer->physicalPage(logical,pa) ||
            (hardware.useTranslationCursor?hardware.tables.translate(buffer->address+uint64_t(logical)*4096,actual,cursor):
                hardware.tables.translate(buffer->address+uint64_t(logical)*4096,actual))!=QcomPower::OK || actual!=pa)
            return QcomPower::Stale;
        return QcomPower::OK;
    }
    QcomPower::Status validate(unsigned count) {
        for(unsigned i=0;i<count;++i) {
            uint64_t pa=0;auto status=page(i,pa);
            if(status!=QcomPower::OK)return status;
        }
        return QcomPower::OK;
    }
};
// The caller holds the validated buffer for the entire private-tree lifetime.
// Keep the logical offset when mapping a narrowed view of its pinned backing.
template<class Buffer> struct PinnedBufferPages {
    const Buffer *buffer;
    unsigned firstPage=0;
    QcomPower::Status page(unsigned index,uint64_t &pa) {
        if(index>UINT32_MAX-firstPage)return QcomPower::Invalid;
        return buffer->physicalPage(firstPage+index,pa)?QcomPower::OK:QcomPower::Stale;
    }
};
}
