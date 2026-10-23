// SPDX-License-Identifier: BSD-2-Clause
#pragma once
#include <GPUNativeProtocol.h>
#include <GPUNativeCompletion.h>
#include <GPUNativeMemory.h>
#include <GPUShaderProtocol.h>

namespace GPU { namespace MetalBatch {
// Planning only: the kernel still validates access, physical aliases and
// actual resources. Repeated bindings/views of one native ID consume one
// resource slot, just as submitNative's retainBuffer does.
class Plan {
    uint32_t ids_[Native::Memory::ResourcesPerJob]{};
    uint32_t resources_=0,commands_=0,offset_=0,capacity_=0,payloadBytes_=0;
    uint64_t indexBytes_=0;
    bool resource(uint32_t id) {
        if(!id)return true;
        for(unsigned i=0;i<resources_;++i)if(ids_[i]==id)return true;
        if(resources_==Native::Memory::ResourcesPerJob)return false;
        ids_[resources_++]=id;return true;
    }
public:
    explicit Plan(uint32_t capacity,uint32_t tasks=0,uint32_t version=Native::BatchCommandVersion):offset_(Native::commandOffset(tasks,version)),
        capacity_(capacity<Native::MaxShmemBytes?capacity:Native::MaxShmemBytes){}
    uint32_t resources() const{return resources_;}
    uint32_t commands() const{return commands_;}
    uint32_t bytes() const{return offset_+commands_*sizeof(Command)+payloadBytes_;}
    uint64_t indexBytes() const{return Native::indexSnapshotAllocation(indexBytes_);}
    bool append(uint32_t opcode,const void *payload,size_t size) {
        const auto expected=opcode==ShaderDraw?sizeof(Shader::Draw):opcode==ShaderDispatch?sizeof(Shader::Dispatch):0;
        if(!offset_ || !payload || !expected || size!=expected || commands_==Native::MaxBatchCommands ||
            uint64_t(bytes())+sizeof(Command)+size>capacity_)return false;
        const auto saved=resources_;uint64_t indices=indexBytes_;bool fits=true;
        if(opcode==ShaderDraw) {
            const auto &d=*static_cast<const Shader::Draw *>(payload);
            fits=resource(d.indices.resource) && resource(d.depth.resource) && resource(d.stencil.resource);
            for(unsigned i=0;i<QGPU_COLOR_TARGET_COUNT && fits;++i)fits=resource(Shader::colorTarget(d,i).resource);
            for(unsigned i=0;i<Shader::MaxBindings && fits;++i)fits=resource(d.buffers[i].resource) && resource(d.fragmentBuffers[i].resource);
            for(unsigned i=0;i<QGPU_STAGE_TEXTURE_COUNT && fits;++i)fits=resource(d.vertexTextures[i].resource) && resource(d.textures[i].resource);
            if(d.indexSize) {uint32_t offset=0;fits&=Native::appendIndexSnapshot(d.indices.bytes,indices,offset);}
        } else {
            const auto &d=*static_cast<const Shader::Dispatch *>(payload);
            for(unsigned i=0;i<Shader::MaxBindings && fits;++i)fits=resource(d.buffers[i].resource);
            for(unsigned i=0;i<QGPU_COMPUTE_IMAGE_COUNT && fits;++i)fits=resource(d.images[i].resource);
        }
        if(!fits){resources_=saved;return false;}
        ++commands_;payloadBytes_+=uint32_t(size);indexBytes_=indices;return true;
    }
};
} }
