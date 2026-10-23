// SPDX-License-Identifier: BSD-2-Clause
#pragma once
#include "GPUNativeProtocol.h"
#include "GPUShaderProtocol.h"

namespace GPU { namespace Blit {
// The native backend reserves two final PM4 words for its completion barrier.
// A5xx CP_MEM_TO_MEM costs six words per copied DWORD; CP_MEM_WRITE costs
// three header/address words plus one word per filled DWORD.
constexpr uint32_t IBDwords = 16384, TailDwords = 2, ComputeThreshold = 16 * 1024;
constexpr uint32_t NativeLayoutVersion=2;
class Batch {
    Command commands_[MaxCommands]{};
    uint32_t count_ = 0, words_ = 0;
public:
    const Command *commands() const { return commands_; }
    uint32_t count() const { return count_; }
    uint32_t words() const { return words_; }
    void reset() { count_ = words_ = 0; }
    uint32_t capacity(bool copy) const {
        if (count_ == MaxCommands) return 0;
        uint32_t free = IBDwords - TailDwords - words_;
        if (copy) return (free / 6) * 4;
        if (free <= 3) return 0;
        uint32_t n = free - 3;
        return (n < 0x3ffd ? n : 0x3ffd) * 4;
    }
    bool append(const Command &command) {
        if (!command.bytes || (command.bytes & 3) || command.reserved) return false;
        const bool copy = command.opcode == Copy32;
        if ((!copy && command.opcode != Fill32) || command.bytes > capacity(copy)) return false;
        words_ += copy ? (command.bytes / 4) * 6 : command.bytes / 4 + 3;
        commands_[count_++] = command;
        return true;
    }
};

struct ComputePlan {
    uint32_t parameters[8]{};
    uint32_t sourceOffset = 0, destinationOffset = 0;
    uint32_t sourceBytes = 0, destinationBytes = 0, threads = 0;
    bool copy = false, bytewise = false, rowFill = false, pairFill = false;
};
inline bool copyPlan(uint64_t sourceOffset, uint64_t destinationOffset, uint64_t bytes,
    uint64_t sourceStride, uint64_t destinationStride, uint64_t rows, ComputePlan &out, bool bulkOnly = true) {
    out = {};
    if (rows == 1) sourceStride = destinationStride = bytes;
    if (!bytes || !rows || ((sourceOffset | destinationOffset | bytes | sourceStride | destinationStride) & 3) ||
        !stridedRange(MaxBufferBytes, sourceOffset, bytes, sourceStride, rows) ||
        !stridedRange(MaxBufferBytes, destinationOffset, bytes, destinationStride, rows) ||
        sourceStride > UINT32_MAX || destinationStride > UINT32_MAX || rows > UINT32_MAX ||
        rows > MaxBufferBytes / bytes || (bulkOnly && bytes * rows < ComputeThreshold)) return false;
    out.parameters[0] = uint32_t(bytes / 4); out.parameters[1] = uint32_t(rows);
    out.parameters[2] = uint32_t(sourceStride / 4); out.parameters[3] = uint32_t(destinationStride / 4);
    out.sourceOffset = uint32_t(sourceOffset); out.destinationOffset = uint32_t(destinationOffset);
    out.sourceBytes = uint32_t((rows - 1) * sourceStride + bytes);
    out.destinationBytes = uint32_t((rows - 1) * destinationStride + bytes);
    out.threads = uint32_t(bytes * rows / 4); out.copy = true;
    return true;
}
inline bool fillPlan(uint64_t offset, uint64_t bytes, uint32_t pattern, ComputePlan &out,bool bulkOnly=true) {
    out = {};
    if (!bytes || (bulkOnly && bytes < ComputeThreshold) || ((offset | bytes) & 3) || !range(MaxBufferBytes, offset, bytes)) return false;
    out.parameters[0] = uint32_t(bytes / 4); out.parameters[3] = pattern;
    out.destinationOffset = uint32_t(offset); out.destinationBytes = uint32_t(bytes);
    out.threads = uint32_t(bytes / 4);
    return true;
}
// Byte kernels bind complete words, then mask the logical byte range. One
// invocation owns each destination word across all rows, so even sub-word
// row strides cannot race on adjoining bytes. The native backend snapshots
// these parameters and checks the logical strided ranges before submission.
inline bool copyBytePlan(uint64_t sourceOffset, uint64_t destinationOffset, uint64_t bytes,
    uint64_t sourceStride, uint64_t destinationStride, uint64_t rows, ComputePlan &out) {
    out = {};
    if (rows == 1) sourceStride = destinationStride = bytes;
    if (!((sourceOffset | destinationOffset | bytes | sourceStride | destinationStride) & 3) ||
        !stridedRange(MaxBufferBytes, sourceOffset, bytes, sourceStride, rows) ||
        !stridedRange(MaxBufferBytes, destinationOffset, bytes, destinationStride, rows) ||
        sourceStride > UINT32_MAX || destinationStride > UINT32_MAX || rows > UINT32_MAX) return false;
    uint64_t from = sourceOffset & ~uint64_t(3), to = destinationOffset & ~uint64_t(3);
    uint64_t sourceSpan = ((rows - 1) * sourceStride + bytes + (sourceOffset & 3) + 3) & ~uint64_t(3);
    uint64_t destinationSpan = ((rows - 1) * destinationStride + bytes + (destinationOffset & 3) + 3) & ~uint64_t(3);
    if (!range(MaxBufferBytes, from, sourceSpan) || !range(MaxBufferBytes, to, destinationSpan)) return false;
    out.parameters[0] = uint32_t(bytes); out.parameters[1] = uint32_t(rows);
    out.parameters[2] = uint32_t(sourceStride); out.parameters[3] = uint32_t(destinationStride);
    out.parameters[4] = uint32_t(sourceOffset & 3); out.parameters[5] = uint32_t(destinationOffset & 3);
    out.sourceOffset = uint32_t(from); out.destinationOffset = uint32_t(to);
    out.sourceBytes = uint32_t(sourceSpan); out.destinationBytes = uint32_t(destinationSpan);
    out.threads = uint32_t(destinationSpan / 4); out.copy = out.bytewise = true;
    return true;
}
inline bool fillBytePlan(uint64_t offset, uint64_t bytes, uint32_t pattern, ComputePlan &out) {
    out = {};
    if (!((offset | bytes) & 3) || !range(MaxBufferBytes, offset, bytes)) return false;
    uint64_t first = offset & ~uint64_t(3), span = (bytes + (offset & 3) + 3) & ~uint64_t(3);
    if (!range(MaxBufferBytes, first, span)) return false;
    out.parameters[0] = uint32_t(bytes); out.parameters[1] = 1;
    out.parameters[5] = uint32_t(offset & 3); out.parameters[6] = pattern;
    out.destinationOffset = uint32_t(first); out.destinationBytes = uint32_t(span);
    out.threads = uint32_t(span / 4); out.bytewise = true;
    return true;
}
// Dedicated row fill leaves all original blit layouts unchanged. Pattern
// byte zero is relative to each row, not to the aligned binding address.
inline bool fillRowsPlan(uint64_t offset,uint64_t bytes,uint64_t stride,uint64_t rows,uint32_t pattern,ComputePlan &out) {
    out={};if(rows==1)stride=bytes;
    if(!stridedRange(MaxBufferBytes,offset,bytes,stride,rows) || stride>UINT32_MAX || rows>UINT32_MAX)return false;
    const uint64_t first=offset&~uint64_t(3),bias=offset&3;
    const uint64_t span=((rows-1)*stride+bytes+bias+3)&~uint64_t(3);
    if(!range(MaxBufferBytes,first,span))return false;
    out.parameters[0]=uint32_t(bytes);out.parameters[1]=uint32_t(rows);out.parameters[2]=uint32_t(stride);
    out.parameters[3]=uint32_t(bias);out.parameters[4]=pattern;
    out.destinationOffset=uint32_t(first);out.destinationBytes=uint32_t(span);
    out.threads=uint32_t(!bias && (rows==1 || !(stride&3))?((bytes+3)/4)*rows:span/4);
    out.rowFill=true;return true;
}
inline bool rowFillParameters(uint64_t bindingOffset,const uint32_t (&values)[8],ComputePlan &out) {
    if(bindingOffset>MaxBufferBytes || values[3]>3){out={};return false;}
    if(!fillRowsPlan(bindingOffset+values[3],values[0],values[2],values[1],values[4],out) ||
        bindingOffset!=out.destinationOffset)return false;
    for(unsigned i=0;i<8;++i)if(values[i]!=out.parameters[i])return false;
    return true;
}
inline bool fillPairRowsPlan(uint64_t offset,uint64_t bytes,uint64_t stride,uint64_t rows,
    uint32_t low,uint32_t high,ComputePlan &out) {
    out={};if(rows==1)stride=bytes;
    if((offset|bytes|stride)&7 || !stridedRange(MaxBufferBytes,offset,bytes,stride,rows) ||
        stride>UINT32_MAX || rows>UINT32_MAX)return false;
    out.parameters[0]=uint32_t(bytes);out.parameters[1]=uint32_t(rows);out.parameters[2]=uint32_t(stride);
    out.parameters[3]=low;out.parameters[4]=high;
    out.destinationOffset=uint32_t(offset);out.destinationBytes=uint32_t((rows-1)*stride+bytes);
    out.threads=uint32_t((bytes/4)*rows);out.pairFill=true;return true;
}
inline bool pairFillParameters(uint64_t offset,const uint32_t (&values)[8],ComputePlan &out) {
    if(!fillPairRowsPlan(offset,values[0],values[2],values[1],values[3],values[4],out))return false;
    for(unsigned i=0;i<8;++i)if(values[i]!=out.parameters[i])return false;
    return true;
}
// Each 2D invocation owns up to four consecutive words in one row. Aligned
// widths use uint4 stores; word-aligned tails use scalar stores to active
// words only. Byte fragments and unaligned rows retain the original kernel.
struct VectorFillGeometry {
    uint32_t groups[3]{},local[3]{},grid[3]{};
    bool matches(const Shader::Dispatch &d) const {
        for(unsigned i=0;i<3;++i)
            if(groups[i]!=d.groups[i] || local[i]!=d.local[i] || grid[i]!=d.grid[i])return false;
        return true;
    }
    void apply(Shader::Dispatch &d) const {
        for(unsigned i=0;i<3;++i){d.groups[i]=groups[i];d.local[i]=local[i];d.grid[i]=grid[i];}
    }
};
inline bool vectorFillGeometry(const ComputePlan &p,VectorFillGeometry &out) {
    out={};
    const auto width=p.parameters[0],rows=p.parameters[1],stride=p.parameters[2];
    if(p.copy || p.bytewise || p.rowFill==p.pairFill || (p.rowFill && p.parameters[3]) ||
        !width || !rows || (width&3) || ((p.destinationOffset|(rows>1?stride:0))&15) ||
        rows>Shader::MaxComputeGroups ||
        !stridedRange(MaxBufferBytes,p.destinationOffset,width,stride,rows))return false;
    const uint32_t columns=(width+15)/16,groups=(columns+63)/64;
    if(!groups || groups>Shader::MaxComputeGroups || uint64_t(columns)*rows>Shader::MaxComputeInvocations)return false;
    out.groups[0]=groups;out.groups[1]=rows;out.groups[2]=1;
    out.local[0]=64;out.local[1]=out.local[2]=1;
    out.grid[0]=columns;out.grid[1]=rows;out.grid[2]=1;return true;
}
inline const char *vectorFillKernel(const ComputePlan &p) {
    return p.pairFill ? (p.parameters[0]&15?"blit_fill_pairs_2d_tail":"blit_fill_pairs_2d") :
        (p.parameters[0]&15?"blit_fill_rows_2d_tail":"blit_fill_rows_2d");
}
} }
