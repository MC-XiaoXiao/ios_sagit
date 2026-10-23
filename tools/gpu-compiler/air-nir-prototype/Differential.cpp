// SPDX-License-Identifier: BSD-2-Clause
// Execute both real prepared NIR programs with identical quad inputs.
// This is a frontend semantic check; it does not emulate the A540 ISA.
#include "Prototype.h"
#include "../AIRNIR.h"
#include "compiler/nir/nir_builder.h"
extern "C" {
#include "compiler/nir/nir_constant_expressions.h"
}
#include "compiler/glsl_types.h"
#include <array>
#include <cmath>
#include <cstring>
#include <dlfcn.h>
#include <fstream>
#include <iostream>
#include <iterator>
#include <map>
#include <limits>
#include <set>
#include <sstream>
#include <stdexcept>

namespace {
nir_shader *captured[2]{};
QGPUProgramDescription descriptions[2]{};
nir_shader *finalized[2][4]{};QGPUProgramDescription finalDescriptions[2][4]{};unsigned activeRoute;
using Quad = std::array<std::array<nir_const_value,16>,4>;
using Color = std::array<float,4>;
using Outputs = std::array<std::array<Color,32>,4>;
uint32_t raw(float value){uint32_t result;memcpy(&result,&value,4);return result;}
std::string readFile(const std::string &path){std::ifstream f(path,std::ios::binary);if(!f)throw std::runtime_error("missing "+path);return {std::istreambuf_iterator<char>(f),{}};}
std::vector<GPU::Compiler::FunctionConstantValue> constants(const std::string &path){
    std::vector<GPU::Compiler::FunctionConstantValue> out;std::istringstream in(readFile(path));unsigned index,type;std::string hex;
    while(in>>index>>type>>hex){GPU::Compiler::FunctionConstantValue v;v.index=index;v.type=type;
        for(unsigned i=0;i<hex.size()/2;++i)v.bytes[i]=std::stoul(hex.substr(2*i,2),nullptr,16);out.push_back(v);}return out;
}
struct Fixture {
    std::array<std::vector<uint8_t>,16> buffers;
    std::array<uint32_t,256> constants{};
    std::array<std::array<Color,16>,4> inputs{};
    std::array<Color,4> position{};
    std::array<int32_t,2> framebufferOrigin{};
    std::array<std::array<uint32_t,3>,4> grid{};
    std::array<GPU::Shader::Sampler,8> samplers;
    unsigned samples=1,sampleID=0,alphaCase=0,seed=0,localSize=64,activeMask=15,baseVertex=0,baseInstance=0;
    std::vector<uint8_t> shared=std::vector<uint8_t>(QGPU_MAX_THREADGROUP_BYTES);
    std::array<std::vector<uint8_t>,4> scratch;
    template<class T> void put(unsigned slot,unsigned at,T value){if(at+sizeof(T)>buffers[slot].size())throw std::runtime_error("fixture write");memcpy(buffers[slot].data()+at,&value,sizeof(T));}
    void sizes(const QGPUProgramDescription &p,unsigned windows){
        for(unsigned slot=0;slot<16;++slot)constants[QGPU_BUFFER_SIZE_CONSTANT_BASE+slot]=buffers[slot].size();
        for(unsigned i=0;i<p.constantWindowCount;++i){const auto &v=p.constantWindows[i];auto &bytes=buffers[8+v.buffer];
            bool available=v.byteOffset+64<=bytes.size();constants[qgpu_constant_window_flag(p.constantWindowCount,i)]=available && (windows&(1u<<i));
            for(unsigned n=0;n<16;++n)if(available)memcpy(&constants[QGPU_CONSTANT_WINDOW_BASE+16*i+n],bytes.data()+v.byteOffset+4*n,4);
        }
    }
    Color texel(unsigned image,int x,int y,unsigned level,unsigned sample)const{
        int size=std::max(1,32>>level);x=std::max(0,std::min(size-1,x));y=std::max(0,std::min(size-1,y));
        Color out;for(unsigned k=0;k<4;++k)out[k]=float((x*17+y*31+image*43+level*29+sample*37+k*61+seed*7)&255)/255;
        out[3]=alphaCase==0?0:alphaCase==1?1:out[3];return out;
    }
};

class Executor {
    nir_shader *shader;const QGPUProgramDescription &description;Fixture &fixture;
    std::map<nir_def *,Quad> values;
    std::map<nir_variable *,std::array<std::vector<nir_const_value>,4>> locals;
    std::array<nir_block *,4> predecessor{};
    std::map<unsigned,unsigned> inputLocations;
    Outputs outputs{};unsigned budget=1000000,terminated=0;
    nir_const_value value(nir_src src,unsigned lane,unsigned component=0){
        auto it=values.find(src.ssa);if(it==values.end())throw std::runtime_error("NIR undefined SSA input");
        return it->second[lane][component];
    }
    uint32_t uint(nir_src src,unsigned lane,unsigned component=0){return value(src,lane,component).u32;}
    float real(nir_src src,unsigned lane,unsigned component=0){return nir_const_value_as_float(value(src,lane,component),src.ssa->bit_size);}
    void result(nir_def *def,const Quad &out,unsigned mask){auto &target=values[def];for(unsigned l=0;l<4;++l)if(mask&(1u<<l))target[l]=out[l];}
    void alu(nir_alu_instr *in,unsigned mask){Quad out{};const auto &info=nir_op_infos[in->op];
        unsigned bitSize=nir_alu_type_get_type_size(info.output_type)?0:in->def.bit_size;
        for(unsigned s=0;s<info.num_inputs && !bitSize;++s)
            if(!nir_alu_type_get_type_size(info.input_types[s]))bitSize=in->src[s].src.ssa->bit_size;
        if(!bitSize)bitSize=32;
        for(unsigned l=0;l<4;++l)if(mask&(1u<<l)){
            nir_const_value sources[NIR_MAX_VEC_COMPONENTS][NIR_MAX_VEC_COMPONENTS]{};nir_const_value *args[NIR_MAX_VEC_COMPONENTS]{};
            for(unsigned s=0;s<info.num_inputs;++s){args[s]=sources[s];unsigned n=info.input_sizes[s]?info.input_sizes[s]:in->def.num_components;
                for(unsigned c=0;c<n;++c)sources[s][c]=value(in->src[s].src,l,in->src[s].swizzle[c]);}
            nir_eval_const_opcode(in->op,out[l].data(),in->def.num_components,bitSize,args,shader->info.float_controls_execution_mode);
        }result(&in->def,out,mask);
    }
    static int address(int v,int size,unsigned mode,bool &border){
        switch(mode){case 0:case 1:return std::max(0,std::min(size-1,v));
            case 4:case 5:if(v<0 || v>=size){border=true;return 0;}return v;
            case 2:return (v%size+size)%size;
            case 3:{int a=(v%(2*size)+2*size)%(2*size);return a<size?a:2*size-1-a;}
            default:throw std::runtime_error("unsupported sampler address mode");}
    }
    Color sample(unsigned image,const GPU::Shader::Sampler &sampler,float x,float y,unsigned level,bool linear){
        int size=std::max(1,32>>level);float px=x*size-(linear?.5f:0),py=y*size-(linear?.5f:0);
        int ix=int(std::floor(px)),iy=int(std::floor(py));float fx=px-ix,fy=py-iy;Color out{};
        for(unsigned j=0;j<(linear?2u:1u);++j)for(unsigned i=0;i<(linear?2u:1u);++i){bool border=false;
            int tx=address(ix+i,size,GPU::Shader::addressMode(sampler,0),border),ty=address(iy+j,size,GPU::Shader::addressMode(sampler,1),border);
            Color color=border?Color{}:fixture.texel(image,tx,ty,level,0);float weight=linear?(i?fx:1-fx)*(j?fy:1-fy):1;
            for(unsigned c=0;c<4;++c)out[c]+=color[c]*weight;
        }return out;
    }
    void texture(nir_tex_instr *in,unsigned mask){
        int coord=nir_tex_instr_src_index(in,nir_tex_src_coord),lod=nir_tex_instr_src_index(in,nir_tex_src_lod),bias=nir_tex_instr_src_index(in,nir_tex_src_bias);
        int dx=nir_tex_instr_src_index(in,nir_tex_src_ddx),dy=nir_tex_instr_src_index(in,nir_tex_src_ddy),ms=nir_tex_instr_src_index(in,nir_tex_src_ms_index);
        if(coord<0 || in->is_shadow)throw std::runtime_error("unsupported differential texture");Quad out{};
        for(unsigned l=0;l<4;++l)if(mask&(1u<<l)){
            Color color{};unsigned image=in->texture_index;
            if(in->op==nir_texop_txf || in->op==nir_texop_txf_ms){unsigned attachment=8;
                for(unsigned i=0;i<8;++i)if((description.framebufferMask&(1u<<i)) && qgpu_framebuffer_slot(description.sampleMask,description.fetchMask,description.framebufferMask,i)==image){attachment=i;break;}
                unsigned layer=in->is_array?uint(in->src[coord].src,l,2):0;
                int x=int32_t(uint(in->src[coord].src,l,0)),y=int32_t(uint(in->src[coord].src,l,1));
                // A tile view contains the same attachment pixels at local
                // coordinates. Ordinary textures retain their original view.
                if(attachment<8){x+=fixture.framebufferOrigin[0];y+=fixture.framebufferOrigin[1];}
                color=fixture.texel(attachment<8?16+attachment:(image-QGPU_FETCH_TEXTURE_BASE)+17*layer,x,y,lod<0?0:uint(in->src[lod].src,l),ms<0?0:uint(in->src[ms].src,l));
            }else{
                GPU::Shader::Sampler sampler=fixture.samplers[in->sampler_index&7];
                for(unsigned i=0;i<QGPU_MAX_CONSTANT_SAMPLERS;++i)if((description.constantSamplerMask&(1u<<i)) &&
                    (((description.constantImplicitSamplerMask&(1u<<i)) && description.constantSamplerSlots[i]==in->sampler_index) ||
                     ((description.constantExplicitSamplerMask&(1u<<i)) && description.constantSamplerLevelSlots[i]==in->sampler_index)))
                    memcpy(&sampler,&description.constantSamplers[i],sizeof(sampler));
                if(!sampler.normalized)throw std::runtime_error("unnormalized fixture sampler");float level=0;
                if(lod>=0)level=real(in->src[lod].src,l);
                else{float gx[2]{},gy[2]{};
                    for(unsigned c=0;c<2;++c){gx[c]=dx>=0?real(in->src[dx].src,l,c):real(in->src[coord].src,1,c)-real(in->src[coord].src,0,c);
                        gy[c]=dy>=0?real(in->src[dy].src,l,c):real(in->src[coord].src,2,c)-real(in->src[coord].src,0,c);}
                    float rho=32*std::sqrt(std::max(gx[0]*gx[0]+gx[1]*gx[1],gy[0]*gy[0]+gy[1]*gy[1]));
                    level=std::log2(std::max(rho,1e-20f));if(bias>=0)level+=real(in->src[bias].src,l);
                }
                level=std::max(sampler.minLOD/256.f,std::min(sampler.maxLOD/256.f,level));level=std::max(0.f,std::min(5.f,level));
                unsigned mip=(sampler.filters>>2)&3;unsigned low=mip?unsigned(mip==1?std::floor(level+.5f):std::floor(level)):0;
                bool linear=bool(sampler.filters&(level>0?1:2));float x=real(in->src[coord].src,l,0),y=real(in->src[coord].src,l,1);
                if(in->sampler_dim==GLSL_SAMPLER_DIM_CUBE) {
                    float z=real(in->src[coord].src,l,2),ax=std::abs(x),ay=std::abs(y),az=std::abs(z),sc,tc,major;unsigned face;
                    if(ax>=ay && ax>=az){face=x<0?1:0;sc=x<0?z:-z;tc=-y;major=ax;}
                    else if(ay>=az){face=y<0?3:2;sc=x;tc=y<0?-z:z;major=ay;}
                    else {face=z<0?5:4;sc=z<0?-x:x;tc=-y;major=az;}
                    x=major?sc/(2*major)+.5f:.5f;y=major?tc/(2*major)+.5f:.5f;
                    unsigned cube=in->is_array?unsigned(std::max(0.f,std::floor(real(in->src[coord].src,l,3)))):0;
                    image+=17*(6*cube+face);
                } else if(in->is_array)image+=17*unsigned(std::max(0.f,std::floor(real(in->src[coord].src,l,2))));
                int offset=nir_tex_instr_src_index(in,nir_tex_src_offset);
                if(offset>=0){x+=int32_t(uint(in->src[offset].src,l,0))/float(std::max(1,32>>low));y+=int32_t(uint(in->src[offset].src,l,1))/float(std::max(1,32>>low));}
                color=sample(image,sampler,x,y,low,linear);
                if(mip==2 && low<5){auto high=sample(image,sampler,x,y,low+1,linear);for(unsigned c=0;c<4;++c)color[c]+=(high[c]-color[c])*(level-low);}
            }
            for(unsigned c=0;c<in->def.num_components;++c)out[l][c]=nir_const_value_for_float(color[c],in->def.bit_size);
        }result(&in->def,out,mask);
    }
    unsigned outputSlot(unsigned location) {
        if(shader->info.stage==MESA_SHADER_VERTEX)return location==VARYING_SLOT_POS?0:location==VARYING_SLOT_PSIZ?31:1+location-VARYING_SLOT_VAR0;
        return location==FRAG_RESULT_DEPTH?8:location==FRAG_RESULT_SAMPLE_MASK?9:location-FRAG_RESULT_DATA0;
    }
    void intrinsic(nir_intrinsic_instr *in,unsigned mask){Quad out{};unsigned n=in->def.num_components;
        if(in->intrinsic==nir_intrinsic_terminate || in->intrinsic==nir_intrinsic_terminate_if) {
            for(unsigned l=0;l<4;++l)if((mask&(1u<<l)) && (in->intrinsic==nir_intrinsic_terminate ||
                nir_const_value_as_bool(value(in->src[0],l),in->src[0].ssa->bit_size)))terminated|=1u<<l;
            return;
        }
        bool derivative=in->intrinsic==nir_intrinsic_ddx || in->intrinsic==nir_intrinsic_ddy || in->intrinsic==nir_intrinsic_ddx_fine || in->intrinsic==nir_intrinsic_ddy_fine || in->intrinsic==nir_intrinsic_ddx_coarse || in->intrinsic==nir_intrinsic_ddy_coarse;
        for(unsigned l=0;l<4;++l)if(mask&(1u<<l)){
            if(derivative){bool y=in->intrinsic==nir_intrinsic_ddy || in->intrinsic==nir_intrinsic_ddy_fine || in->intrinsic==nir_intrinsic_ddy_coarse;
                bool fine=in->intrinsic==nir_intrinsic_ddx_fine || in->intrinsic==nir_intrinsic_ddy_fine;unsigned first=fine?(y?(l&1):(l&2)):0,second=first+(y?2:1);
                for(unsigned c=0;c<n;++c)out[l][c]=nir_const_value_for_float(real(in->src[0],second,c)-real(in->src[0],first,c),in->def.bit_size);continue;}
            switch(in->intrinsic){
                case nir_intrinsic_load_const_ir3:{unsigned at=uint(in->src[0],l)+nir_intrinsic_base(in);if(at+n>fixture.constants.size())throw std::runtime_error("native constant range");
                    for(unsigned c=0;c<n;++c)out[l][c].u32=fixture.constants[at+c];break;}
                case nir_intrinsic_load_ssbo:case nir_intrinsic_store_ssbo:{bool store=in->intrinsic==nir_intrinsic_store_ssbo;unsigned binding=uint(in->src[store?1:0],l),at=uint(in->src[store?2:1],l);
                    auto &bytes=fixture.buffers.at(binding);unsigned bits=store?in->src[0].ssa->bit_size:in->def.bit_size,count=in->num_components;
                    for(unsigned c=0;c<count;++c){if(store && !(nir_intrinsic_write_mask(in)&(1u<<c)))continue;
                        uint64_t offset=uint64_t(at)+c*(bits/8);if(offset+bits/8>bytes.size())throw std::runtime_error("unguarded out-of-range SSBO access binding="+std::to_string(binding)+" offset="+std::to_string(offset)+" size="+std::to_string(bytes.size()));
                        if(store){auto v=value(in->src[0],l,c);memcpy(bytes.data()+offset,&v,bits/8);}else memcpy(&out[l][c],bytes.data()+offset,bits/8);
                    }break;}
                case nir_intrinsic_load_input:case nir_intrinsic_load_interpolated_input:{unsigned s=in->intrinsic==nir_intrinsic_load_input?0:1;
                    unsigned at=nir_intrinsic_base(in)+uint(in->src[s],l),component=nir_intrinsic_component(in);
                    if(!inputLocations.count(at))throw std::runtime_error("unknown varying input location");unsigned loc=inputLocations.at(at)-(shader->info.stage==MESA_SHADER_VERTEX?VERT_ATTRIB_GENERIC0:VARYING_SLOT_VAR0);
                    for(unsigned c=0;c<n;++c)out[l][c]=nir_const_value_for_float(fixture.inputs[l].at(loc).at(component+c),in->def.bit_size);break;}
                case nir_intrinsic_store_output:{unsigned at=outputSlot(nir_intrinsic_io_semantics(in).location)+uint(in->src[1],l),component=nir_intrinsic_component(in);
                    for(unsigned c=0;c<in->num_components;++c)if(nir_intrinsic_write_mask(in)&(1u<<c))outputs[l].at(at).at(component+c)=real(in->src[0],l,c);break;}
                case nir_intrinsic_load_deref:case nir_intrinsic_store_deref:{
                    bool store=in->intrinsic==nir_intrinsic_store_deref;auto deref=nir_src_as_deref(in->src[0]);auto var=nir_deref_instr_get_variable(deref);
                    if(!var)throw std::runtime_error("unexpected differential deref");
                    unsigned index=0;
                    for(auto d=deref;d->deref_type!=nir_deref_type_var;d=nir_deref_instr_parent(d)) {
                        if(d->deref_type!=nir_deref_type_array)throw std::runtime_error("nonarray local deref");
                        index+=uint(d->arr.index,l)*glsl_get_component_slots(d->type);
                    }
                    if(var->data.mode==nir_var_shader_out && store) {
                        unsigned at=outputSlot(var->data.location);
                        for(unsigned c=0;c<in->num_components;++c)if(nir_intrinsic_write_mask(in)&(1u<<c))outputs[l].at(at).at(index+c)=real(in->src[1],l,c);
                    }else if(var->data.mode==nir_var_function_temp) {
                        auto &local=locals[var][l];if(local.empty())local.resize(glsl_get_component_slots(var->type));
                        for(unsigned c=0;c<in->num_components;++c) {
                            if(index+c>=local.size())throw std::runtime_error("unguarded local array access");
                            if(store){if(nir_intrinsic_write_mask(in)&(1u<<c))local[index+c]=value(in->src[1],l,c);}else out[l][c]=local[index+c];
                        }
                    }else throw std::runtime_error("unexpected differential deref mode");
                    break;}
                case nir_intrinsic_load_shared:case nir_intrinsic_store_shared:
                case nir_intrinsic_load_scratch:case nir_intrinsic_store_scratch:{
                    bool store=in->intrinsic==nir_intrinsic_store_shared || in->intrinsic==nir_intrinsic_store_scratch;
                    bool shared=in->intrinsic==nir_intrinsic_load_shared || in->intrinsic==nir_intrinsic_store_shared;
                    auto &bytes=shared?fixture.shared:fixture.scratch[l];if(!shared && bytes.empty())bytes.resize(65536);
                    unsigned at=uint(in->src[store?1:0],l)+(shared?nir_intrinsic_base(in):0);
                    unsigned bits=store?in->src[0].ssa->bit_size:in->def.bit_size;
                    for(unsigned c=0;c<in->num_components;++c){if(store && !(nir_intrinsic_write_mask(in)&(1u<<c)))continue;
                        if(uint64_t(at)+((c+1)*bits/8)>bytes.size())throw std::runtime_error("unguarded shared/private access");
                        if(store){auto v=value(in->src[0],l,c);memcpy(bytes.data()+at+c*bits/8,&v,bits/8);}else memcpy(&out[l][c],bytes.data()+at+c*bits/8,bits/8);}
                    break;}
                case nir_intrinsic_ssbo_atomic:case nir_intrinsic_ssbo_atomic_swap:
                case nir_intrinsic_shared_atomic:case nir_intrinsic_shared_atomic_swap:{
                    bool shared=in->intrinsic==nir_intrinsic_shared_atomic || in->intrinsic==nir_intrinsic_shared_atomic_swap;
                    unsigned at=uint(in->src[shared?0:1],l)+(shared?nir_intrinsic_base(in):0);
                    auto &bytes=shared?fixture.shared:fixture.buffers.at(uint(in->src[0],l));
                    if(uint64_t(at)+4>bytes.size())throw std::runtime_error("unguarded atomic access");
                    uint32_t old;memcpy(&old,bytes.data()+at,4);auto operand=uint(in->src[shared?1:2],l);uint32_t next=old;
                    switch(nir_intrinsic_atomic_op(in)){
                        case nir_atomic_op_iadd:next=old+operand;break;case nir_atomic_op_iand:next=old&operand;break;
                        case nir_atomic_op_ior:next=old|operand;break;case nir_atomic_op_ixor:next=old^operand;break;
                        case nir_atomic_op_umin:next=std::min(old,operand);break;case nir_atomic_op_umax:next=std::max(old,operand);break;
                        case nir_atomic_op_imin:next=uint32_t(std::min(int32_t(old),int32_t(operand)));break;
                        case nir_atomic_op_imax:next=uint32_t(std::max(int32_t(old),int32_t(operand)));break;
                        case nir_atomic_op_xchg:next=operand;break;
                        case nir_atomic_op_cmpxchg:next=old==operand?uint(in->src[shared?2:3],l):old;break;
                        default:throw std::runtime_error("unsupported atomic operation");}
                    memcpy(bytes.data()+at,&next,4);out[l][0].u32=old;break;}
                case nir_intrinsic_barrier:if(mask!=fixture.activeMask)throw std::runtime_error("divergent barrier in differential fixture");break;
                case nir_intrinsic_load_vertex_id:case nir_intrinsic_load_vertex_id_zero_base:out[l][0].u32=fixture.grid[l][0];break;
                case nir_intrinsic_load_instance_id:out[l][0].u32=fixture.seed%3;break;
                case nir_intrinsic_load_first_vertex:case nir_intrinsic_load_base_vertex:out[l][0].u32=fixture.baseVertex;break;
                case nir_intrinsic_load_base_instance:out[l][0].u32=fixture.baseInstance;break;
                case nir_intrinsic_load_local_invocation_index:out[l][0].u32=fixture.grid[l][0]%fixture.localSize;break;
                case nir_intrinsic_load_sample_mask_in:out[l][0].u32=(1u<<fixture.samples)-1;break;
                case nir_intrinsic_load_frag_coord:for(unsigned c=0;c<n;++c)out[l][c]=nir_const_value_for_float(fixture.position[l][c],32);break;
                case nir_intrinsic_load_sample_id:out[l][0].u32=fixture.sampleID;break;
                case nir_intrinsic_load_local_invocation_id:for(unsigned c=0;c<n;++c)out[l][c].u32=c?0:fixture.grid[l][0]%fixture.localSize;break;
                case nir_intrinsic_load_workgroup_id:for(unsigned c=0;c<n;++c)out[l][c].u32=c?0:fixture.grid[l][0]/fixture.localSize;break;
                case nir_intrinsic_load_workgroup_size:out[l][0].u32=fixture.localSize;out[l][1].u32=out[l][2].u32=1;break;
                case nir_intrinsic_load_num_workgroups:out[l][0].u32=16;out[l][1].u32=out[l][2].u32=1;break;
                case nir_intrinsic_load_barycentric_pixel:case nir_intrinsic_load_barycentric_sample:case nir_intrinsic_load_barycentric_centroid:break;
                default:throw std::runtime_error(std::string("unsupported differential intrinsic ")+nir_intrinsic_infos[in->intrinsic].name);
            }
        }if(nir_intrinsic_infos[in->intrinsic].has_dest)result(&in->def,out,mask);
    }
    struct Flow{unsigned live,brk=0,cont=0,ret=0;};
    Flow block(nir_block *block,unsigned mask){
        std::vector<std::pair<nir_def *,Quad>> phis;
        nir_foreach_instr(instr,block)if(instr->type==nir_instr_type_phi){auto in=nir_instr_as_phi(instr);Quad out{};
            for(unsigned l=0;l<4;++l)if(mask&(1u<<l)){bool found=false;nir_foreach_phi_src(src,in)if(src->pred==predecessor[l]){for(unsigned c=0;c<in->def.num_components;++c)out[l][c]=value(src->src,l,c);found=true;break;}
                if(!found)throw std::runtime_error("NIR phi predecessor");}phis.push_back({&in->def,out});}
        for(auto &p:phis)result(p.first,p.second,mask);
        nir_foreach_instr(instr,block){if(!budget--)throw std::runtime_error("NIR execution budget");
            switch(instr->type){
                case nir_instr_type_phi:break;
                case nir_instr_type_deref:break; // Only shader-output pointers remain after SSA lowering.
                case nir_instr_type_load_const:{auto in=nir_instr_as_load_const(instr);Quad out{};for(unsigned l=0;l<4;++l)for(unsigned c=0;c<in->def.num_components;++c)out[l][c]=in->value[c];result(&in->def,out,mask);break;}
                case nir_instr_type_undef:{auto in=nir_instr_as_undef(instr);result(&in->def,Quad{},mask);break;}
                case nir_instr_type_alu:alu(nir_instr_as_alu(instr),mask);break;
                case nir_instr_type_intrinsic:intrinsic(nir_instr_as_intrinsic(instr),mask);break;
                case nir_instr_type_tex:texture(nir_instr_as_tex(instr),mask);break;
                case nir_instr_type_jump:{for(unsigned l=0;l<4;++l)if(mask&(1u<<l))predecessor[l]=block;
                    switch(nir_instr_as_jump(instr)->type){case nir_jump_break:return {0,mask,0,0};case nir_jump_continue:return {0,0,mask,0};case nir_jump_return:case nir_jump_halt:return {0,0,0,mask};default:throw std::runtime_error("NIR goto unsupported");}}
                default:throw std::runtime_error("unsupported differential instruction");
            }
        }for(unsigned l=0;l<4;++l)if(mask&(1u<<l))predecessor[l]=block;return {mask};
    }
    Flow list(struct exec_list *nodes,unsigned mask){Flow out{mask};
        foreach_list_typed(nir_cf_node,node,node,nodes){if(!out.live)break;Flow current{};
            if(node->type==nir_cf_node_block)current=block(nir_cf_node_as_block(node),out.live);
            else if(node->type==nir_cf_node_if){auto in=nir_cf_node_as_if(node);unsigned yes=0;
                for(unsigned l=0;l<4;++l)if((out.live&(1u<<l)) && nir_const_value_as_bool(value(in->condition,l),in->condition.ssa->bit_size))yes|=1u<<l;
                auto a=list(&in->then_list,yes),b=list(&in->else_list,out.live&~yes);current={a.live|b.live,a.brk|b.brk,a.cont|b.cont,a.ret|b.ret};
            }else if(node->type==nir_cf_node_loop){auto in=nir_cf_node_as_loop(node);unsigned active=out.live;current={0};
                while(active){auto step=list(&in->body,active);current.live|=step.brk;current.ret|=step.ret;active=step.live|step.cont;}
            }else throw std::runtime_error("unsupported NIR control node");
            out.live=current.live;out.brk|=current.brk;out.cont|=current.cont;out.ret|=current.ret;
        }return out;
    }
public:
    Executor(nir_shader *nir,const QGPUProgramDescription &p,Fixture &f):shader(nir),description(p),fixture(f){
        nir_foreach_variable_with_modes(var,shader,nir_var_shader_in)inputLocations[var->data.driver_location]=var->data.location;
    }
    Outputs run(){list(&nir_shader_get_entrypoint(shader)->body,fixture.activeMask);for(unsigned l=0;l<4;++l)if(terminated&(1u<<l))outputs[l]={};return outputs;}
};

Fixture fixture(const std::string &label,unsigned seed,const QGPUProgramDescription &p,unsigned windows){
    Fixture f;f.seed=seed;f.samples=seed%3==0?1:seed%3==1?2:4;f.sampleID=seed%f.samples;f.alphaCase=seed%3;
    for(auto &v:f.buffers)v.resize(512);
    for(unsigned s=0;s<16;++s)for(unsigned i=0;i<512;++i)f.buffers[s][i]=(i*13+s*37+seed*19)&255;
    for(unsigned i=0;i<8;++i){f.samplers[i].filters=(seed%2)?7:0;f.samplers[i].maxLOD=1280;
        f.samplers[i].addressModes=((seed&1)?1u:2u)|(((seed&2)?1u:3u)<<3);
        f.constants[i]=GPU::Shader::samplerMirrorFlags(f.samplers[i]);}
    for(unsigned i=0;i<8;++i){unsigned at=QGPU_FRAMEBUFFER_CONSTANT_BASE+4*i;f.constants[at]=f.constants[at+1]=32;
        f.constants[at+2]=f.samples;f.constants[at+3]=seed%2?GPU::Shader::R8:GPU::Shader::RGBA8;}
    f.constants[QGPU_DISPATCH_CONSTANT_BASE+0]=seed%7;
    f.constants[QGPU_DISPATCH_CONSTANT_BASE+4]=1024;f.constants[QGPU_DISPATCH_CONSTANT_BASE+5]=f.constants[QGPU_DISPATCH_CONSTANT_BASE+6]=1;
    f.constants[QGPU_DISPATCH_CONSTANT_BASE+8]=64;f.constants[QGPU_DISPATCH_CONSTANT_BASE+9]=f.constants[QGPU_DISPATCH_CONSTANT_BASE+10]=1;
    f.constants[QGPU_DISPATCH_CONSTANT_BASE+12]=16;f.constants[QGPU_DISPATCH_CONSTANT_BASE+13]=f.constants[QGPU_DISPATCH_CONSTANT_BASE+14]=1;
    for(unsigned l=0;l<4;++l){float x=float((seed%8)*3+(l&1))+.5f,y=float((seed%5)*4+(l>>1))+.5f;
        if(seed%11==10)x-=40;f.position[l]={x,y,.25f,1};f.grid[l]={uint32_t((seed*13)%130+l),0,0};
        for(unsigned i=0;i<16;++i)f.inputs[l][i]={((seed&1)?-1.f:1.f)*(x/32),y/32,.25f,.75f};}
    if(label=="compute"){
        for(unsigned i=0;i<128;++i)f.put<uint32_t>(0,i*4,i*0x10203u+seed);
        unsigned bytes=seed%4==0?4:seed%4==1?256:512;f.buffers[0].resize(bytes);f.buffers[1].resize(bytes);
    }else if(label=="welcome"){
        // Original QuartzCore half4 matrix at 176..215 plus opacity/noise.
        for(unsigned i=0;i<256;++i)f.put<uint16_t>(9,2*i,0);
        for(unsigned i=0;i<4;++i)for(unsigned c=0;c<4;++c)f.put<uint16_t>(9,176+8*i+2*c,i==c?0x3c00:0);
        for(unsigned c=0;c<4;++c)f.put<uint16_t>(9,208+2*c,0x0000);
        f.put<float>(10,0,float(int(seed%5)-2)*.5f);f.put<uint16_t>(13,0,0x3000);
        if(seed%7==6)f.buffers[9].resize(180+2*(seed%4));
    }else{
        for(unsigned i=0;i<64;++i)f.put<float>(8,i*4,.25f+float((seed+i)%5)*.1f);
        f.put<float>(8,0,.1f);f.put<float>(8,4,float(seed)*.03f);
        f.put<float>(8,8,.2f);f.put<float>(8,12,.3f);f.put<float>(8,16,.7f);f.put<float>(8,20,.1f);
        f.put<float>(8,24,.4f);f.put<float>(8,28,.2f);f.put<float>(8,32,.6f);f.put<float>(8,36,-.15f);
        f.put<uint16_t>(8,40,seed%5);f.put<uint16_t>(8,42,(seed+1)%5);f.put<float>(8,44,(seed%5)*.25f);
        for(unsigned at=48;at<120;at+=12){f.put<float>(8,at,.05f);f.put<float>(8,at+4,.95f);f.put<float>(8,at+8,1.2f);}
        f.put<uint16_t>(8,120,seed%5);f.put<uint16_t>(8,122,(seed+2)%5);
        for(unsigned i=0;i<16;++i)f.put<float>(8,144+4*i,i%5==0?1.f:0.f);
        for(unsigned i=0;i<4;++i)f.put<float>(8,208+4*i,.05f);
        if(seed%13==12)f.buffers[8].resize(180+(seed%4)*2);
    }
    f.sizes(p,windows);return f;
}
void prepare(nir_shader *nir){
    NIR_PASS_V(nir,nir_lower_var_copies);NIR_PASS_V(nir,nir_lower_vars_to_ssa);
    for(unsigned i=0;i<3;++i){NIR_PASS_V(nir,nir_copy_prop);NIR_PASS_V(nir,nir_opt_constant_folding);NIR_PASS_V(nir,nir_opt_dce);}
    prototype_validate(nir);
}
void cleanup(){
    for(auto &p:captured){if(p)ralloc_free(p);p=nullptr;}
    for(auto &route:finalized)for(auto &p:route){if(p)ralloc_free(p);p=nullptr;}
}
} // namespace

extern "C" void prototype_capture(nir_shader *nir,const QGPUProgramDescription *p,unsigned route){
    if(route>1)abort();activeRoute=route;if(captured[route])ralloc_free(captured[route]);captured[route]=nir_shader_clone(nullptr,nir);descriptions[route]=*p;
}
extern "C" void prototype_capture_variant(nir_shader *nir,const QGPUProgramDescription *p,unsigned msaa,unsigned window){
    unsigned v=msaa+2*window;auto &shader=finalized[activeRoute][v];if(shader)ralloc_free(shader);shader=nir_shader_clone(nullptr,nir);finalDescriptions[activeRoute][v]=*p;
}
int runDifferential(const std::string &work){
    glsl_type_singleton_init_or_ref();prototype_capture_enable(1);
    std::ostringstream rows;bool first=true;
    auto save=[&](bool passed,const std::string &error){std::ofstream report(work+"/differential.json");
        report<<"{\"executor\":\"Mesa finalized NIR quad interpreter\",\"hardware_tested\":false,\"tolerance\":0,\"passed\":"<<(passed?"true":"false")<<",\"error\":\"";
        for(char c:error){if(c=='\"' || c=='\\')report<<'\\';if(c=='\n')report<<"\\n";else report<<c;}
        report<<"\",\"cases\":["<<rows.str()<<"]}\n";};
    try{
        std::ofstream semantic;
        if(const char *path=getenv("AIR_NIR_SEMANTIC_SNAPSHOT")){
            semantic.open(path,std::ios::binary);
            if(!semantic)throw std::runtime_error("cannot open semantic snapshot");
        }
        const char *runtimePath=getenv("AIR_NIR_RUNTIME_COMPILER");void *runtimeLibrary=nullptr;
        decltype(&qgpu_compile_air_nir) runtimeCompile=nullptr;
        if(runtimePath){runtimeLibrary=dlopen(runtimePath,RTLD_NOW|RTLD_LOCAL);if(!runtimeLibrary)throw std::runtime_error(dlerror());
            runtimeCompile=reinterpret_cast<decltype(runtimeCompile)>(dlsym(runtimeLibrary,"qgpu_compile_air_nir"));
            if(!runtimeCompile)throw std::runtime_error("runtime compiler AIR entry is missing");}
        for(const std::string label:{"compute","welcome","fluidity"}){
          for(unsigned profile=0;profile<(label=="fluidity"?8u:1u);++profile){
            cleanup();
            auto air=readFile(work+"/inputs/"+(label=="compute"?"compute.air":label+".air"));
            auto vals=label=="compute"?std::vector<GPU::Compiler::FunctionConstantValue>{}:constants(work+"/inputs/"+label+".constants.txt");
            if(label=="fluidity")for(auto &v:vals)v.bytes[0]=(profile>>v.index)&1;
            auto entry=label=="compute"?"air_add":label=="welcome"?"fixed_frag_lph_cpf":"fluidity::fragmentShader";
            std::vector<GPU::Compiler::Entry> entries;std::vector<QGPUProgramImage> images;std::string error;PrototypeStats stats;QGPUProgramImage direct{};
            if(!GPU::Compiler::lowerAIR(air.data(),air.size(),entries,error,entry,vals) || !GPU::Compiler::compile(entries,1,images,error) ||
               !GPU::Compiler::compileAIRNIR(air.data(),air.size(),entry,vals,1,direct,error,stats))throw std::runtime_error(error);
            bool runtimeIdentical=false;
            if(runtimeCompile){std::vector<QGPUAIRConstant> values;
                for(const auto &v:vals){QGPUAIRConstant c{};c.index=v.index;c.type=v.type;memcpy(c.bytes,v.bytes.data(),sizeof(c.bytes));values.push_back(c);}
                QGPUProgramImage image{};QGPUAIRNIRTiming timing{};char message[4096]{};
                if(!runtimeCompile(air.data(),air.size(),entry,values.data(),values.size(),1,&image,&timing,message,sizeof(message)))throw std::runtime_error(std::string("actual runtime compiler: ")+message);
                runtimeIdentical=memcmp(&image,&direct,sizeof(image))==0;
                if(!runtimeIdentical){
                    for(auto pair:{std::make_pair("runtime",&image),std::make_pair("prototype",&direct)}){
                        std::ofstream out(work+"/"+label+"-"+std::to_string(profile)+".mismatch."+pair.first+".image",std::ios::binary);
                        out.write(reinterpret_cast<const char *>(pair.second),sizeof(image));
                    }
                    throw std::runtime_error(label+" actual runtime JIT image differs from the validated emitter");
                }
            }
            {std::ofstream out(work+"/"+label+"-"+std::to_string(profile)+".direct.image",std::ios::binary);out.write(reinterpret_cast<const char *>(&direct),sizeof(direct));}
            {std::ofstream out(work+"/"+label+"-"+std::to_string(profile)+".glsl.image",std::ios::binary);out.write(reinterpret_cast<const char *>(&images[0]),sizeof(images[0]));}
            for(unsigned route=0;route<2;++route)for(unsigned variant=0;variant<(label=="compute"?1u:4u);++variant){
                if(!finalized[route][variant])throw std::runtime_error("missing finalized NIR capture");prepare(finalized[route][variant]);
                auto path=work+"/"+label+"-"+std::to_string(profile)+(route?".direct.":".glsl.")+std::to_string(variant)+".final.nir";
                auto file=fopen(path.c_str(),"w");nir_print_shader(finalized[route][variant],file);fclose(file);}
            unsigned count=0,mismatch=0,nonzero=0,referenceMismatches=0;float maxError=0;
            unsigned variantCount[4]{};
            for(unsigned variant=0;variant<(label=="compute"?1u:4u);++variant)
              for(unsigned seed=0;seed<64;++seed)for(unsigned windows:{0u,1u,3u,15u}){
                if(variant>=2 && windows!=15)continue;
                auto &pa=finalDescriptions[0][variant],&pb=finalDescriptions[1][variant];
                auto a=fixture(label,seed,pa,windows),b=fixture(label,seed,pb,windows);
                if(label!="compute" && bool(variant&1)!=(a.samples>1))continue;
                if(variant>=2){bool available=true;
                    for(unsigned i=0;i<pa.constantWindowCount;++i)available &= a.constants[qgpu_constant_window_flag(pa.constantWindowCount,i)]!=0;
                    if(!available)continue;}
                auto expected=a.buffers;
                if(label=="compute")for(unsigned l=0;l<4;++l){uint64_t at=4ull*(a.grid[l][0]+a.constants[QGPU_DISPATCH_CONSTANT_BASE]);
                    if(at+4<=expected[1].size()){uint32_t v=0;if(at+4<=expected[0].size())memcpy(&v,expected[0].data()+at,4);v+=7;memcpy(expected[1].data()+at,&v,4);}}
                auto x=Executor(finalized[0][variant],pa,a).run(),y=Executor(finalized[1][variant],pb,b).run();
                // Persist both routes' complete outputs, including signed zero
                // and NaN payload bits, for comparisons of separate builds.
                if(semantic.is_open()){
                    const uint32_t key[]={label=="compute"?0u:label=="welcome"?1u:2u,profile,variant,seed,windows};
                    semantic.write(reinterpret_cast<const char *>(key),sizeof(key));
                    for(const auto *out:{&x,&y})for(const auto &lane:*out)for(const auto &color:lane)
                        semantic.write(reinterpret_cast<const char *>(color.data()),sizeof(float)*color.size());
                    if(label=="compute")for(const auto *f:{&a,&b})for(const auto &buffer:f->buffers){
                        const uint32_t bytes=buffer.size();
                        semantic.write(reinterpret_cast<const char *>(&bytes),sizeof(bytes));
                        semantic.write(reinterpret_cast<const char *>(buffer.data()),bytes);
                    }
                    if(!semantic)throw std::runtime_error("semantic snapshot write failed");
                }
                if(label=="compute"){if(a.buffers!=b.buffers){++mismatch;std::cerr<<label<<" buffer mismatch seed="<<seed<<"\n";}if(a.buffers!=expected || b.buffers!=expected)++referenceMismatches;}
                else for(unsigned l=0;l<4;++l)for(unsigned t=0;t<8;++t)for(unsigned c=0;c<4;++c){
                    float e=std::abs(x[l][t][c]-y[l][t][c]);maxError=std::max(maxError,e);
                    if(x[l][t][c]!=0 && std::isfinite(x[l][t][c]))++nonzero;
                    if(!(x[l][t][c]==y[l][t][c] || (std::isnan(x[l][t][c]) && std::isnan(y[l][t][c])))){
                        if(mismatch<8)std::cerr<<label<<" mismatch seed="<<seed<<" windows="<<windows<<" lane="<<l<<" color="<<t<<" component="<<c<<" glsl="<<x[l][t][c]<<" nir="<<y[l][t][c]<<"\n";++mismatch;}
                }++count;++variantCount[variant];
            }
            unsigned tileCases=0,tileMismatches=0;bool tileOriginMutationDetected=false;
            if(label!="compute")for(unsigned route=0;route<2;++route)for(unsigned variant=0;variant<4;++variant) {
                const auto &p=finalDescriptions[route][variant];
                if(!p.framebufferMask) {
                    if(p.flags&QGPU_FRAMEBUFFER_TILE_ORIGIN)throw std::runtime_error("tile origin without framebuffer input");
                    continue;
                }
                if(!(p.flags&QGPU_FRAMEBUFFER_TILE_ORIGIN) || GPU::Shader::memoryConstantWords(p)<QGPU_FRAMEBUFFER_TILE_MEMORY_CONSTANT_WORDS)
                    throw std::runtime_error("framebuffer tile origin flag/prefix missing");
                for(unsigned samples:{1u,2u,4u})for(const std::array<int32_t,2> origin:{std::array<int32_t,2>{16,8},{17,19}})
                  for(unsigned edge=0;edge<6;++edge) {
                    if(bool(variant&1)!=(samples>1))continue;
                    auto global=fixture(label,0,p,15);global.samples=samples;global.sampleID=samples-1;
                    for(unsigned i=0;i<8;++i)global.constants[QGPU_FRAMEBUFFER_CONSTANT_BASE+i*4+2]=samples;
                    for(unsigned lane=0;lane<4;++lane) {
                        float x=float(origin[0]+(lane&1))+.5f,y=float(origin[1]+(lane>>1))+.5f;
                        if(edge==1){x=31.5f+(lane&1);y=31.5f+(lane>>1);}
                        if(edge==2)x=32.5f+(lane&1);if(edge==3)x=-1.5f+(lane&1);
                        if(edge==4)y=32.5f+(lane>>1);if(edge==5)y=-1.5f+(lane>>1);
                        global.position[lane]={x,y,.25f,1};
                    }
                    auto tile=global;tile.framebufferOrigin=origin;
                    tile.constants[QGPU_FRAMEBUFFER_TILE_ORIGIN_BASE]=uint32_t(origin[0]);
                    tile.constants[QGPU_FRAMEBUFFER_TILE_ORIGIN_BASE+1]=uint32_t(origin[1]);
                    auto expected=Executor(finalized[route][variant],p,global).run();
                    auto actual=Executor(finalized[route][variant],p,tile).run();
                    // Negative control: an unchanged tile view with a missing
                    // origin update must produce a detectable wrong pixel.
                    auto stale=tile;
                    stale.constants[QGPU_FRAMEBUFFER_TILE_ORIGIN_BASE]=0;
                    stale.constants[QGPU_FRAMEBUFFER_TILE_ORIGIN_BASE+1]=0;
                    auto wrong=Executor(finalized[route][variant],p,stale).run();
                    for(unsigned lane=0;lane<4;++lane)for(unsigned target=0;target<8;++target)for(unsigned c=0;c<4;++c) {
                        float a=expected[lane][target][c],b=actual[lane][target][c];
                        if(!(a==b || (std::isnan(a) && std::isnan(b))))++tileMismatches;
                        float bad=wrong[lane][target][c];
                        if(!(a==bad || (std::isnan(a) && std::isnan(bad))))tileOriginMutationDetected=true;
                    }
                    ++tileCases;
                }
            }
            if(tileMismatches)throw std::runtime_error(label+" tile origin changed global framebuffer/fragment semantics");
            if(tileCases && !tileOriginMutationDetected)throw std::runtime_error(label+" missing tile origin update was not detected");
            bool mutationDetected=true;
            if(label!="compute"){
                auto bad=nir_shader_clone(nullptr,finalized[1][0]);bool mutated=false;
                nir_foreach_function_impl(impl,bad){nir_builder builder=nir_builder_create(impl);nir_foreach_block(block,impl)nir_foreach_instr_safe(instr,block){
                    if(instr->type!=nir_instr_type_intrinsic)continue;auto in=nir_instr_as_intrinsic(instr);
                    if(in->intrinsic!=nir_intrinsic_store_output)continue;builder.cursor=nir_before_instr(instr);
                    nir_src_rewrite(&in->src[0],nir_fadd_imm(&builder,in->src[0].ssa,.25f));mutated=true;
                }}
                auto a=fixture(label,0,finalDescriptions[0][0],0),b=fixture(label,0,finalDescriptions[1][0],0);
                mutationDetected=mutated && Executor(finalized[0][0],finalDescriptions[0][0],a).run()!=Executor(bad,finalDescriptions[1][0],b).run();ralloc_free(bad);
            }
            if(!first)rows<<',';first=false;rows<<"{\"name\":\""<<label<<"\",\"profile\":"<<profile<<",\"quad_cases\":"<<count<<",\"variant_cases\":["<<variantCount[0]<<','<<variantCount[1]<<','<<variantCount[2]<<','<<variantCount[3]<<"],\"mismatches\":"<<mismatch<<",\"max_absolute_error\":"<<maxError<<",\"reference_mismatches\":"<<referenceMismatches<<",\"nonzero_components\":"<<nonzero<<",\"tile_quad_cases\":"<<tileCases<<",\"tile_mismatches\":"<<tileMismatches<<",\"tile_origin_mutation_detected\":"<<(tileOriginMutationDetected?"true":"false")<<",\"output_mutation_detected\":"<<(mutationDetected?"true":"false")<<",\"runtime_compiler_checked\":"<<(runtimeCompile?"true":"false")<<",\"runtime_jit_image_identical\":"<<(runtimeIdentical?"true":"false")<<"}";
            std::cout<<label<<" profile="<<profile<<": "<<count<<" quads, mismatches="<<mismatch<<", max_error="<<maxError<<", reference_mismatches="<<referenceMismatches<<", nonzero_components="<<nonzero<<'\n';
            if(mismatch || referenceMismatches || (label!="compute" && !nonzero))throw std::runtime_error(label+" differential outputs differ");
            if(!mutationDetected)throw std::runtime_error("differential output mutation was not detected");
          }
        }
        save(true,"");prototype_capture_enable(0);cleanup();glsl_type_singleton_decref();return 0;
    }catch(const std::exception &e){save(false,e.what());prototype_capture_enable(0);cleanup();glsl_type_singleton_decref();std::cerr<<"differential failed: "<<e.what()<<'\n';return 1;}
}


// Feature coverage is deliberately separate from the captured UI matrix. Each
// fixture uses a single four-invocation workgroup, not an A540 scheduler model.
int runFeatureDifferential(const std::string &work) {
    glsl_type_singleton_init_or_ref();prototype_capture_enable(1);
    std::ofstream report(work+"/features.tsv");report<<"entry\tcases\tmismatches\tstatus\n";
    unsigned tested=0,totalCases=0,failures=0;
    try {
        for(const std::string file:{"Shaders","Textures","IntegerTextures","SampleShading","ConstantWindows","packed-fluidity","direct-nir-boundaries"}) {
            auto air=readFile(work+"/inputs/"+file+".air.ll");std::vector<GPU::Compiler::Entry> all;std::string error;
            if(!GPU::Compiler::lowerAIR(air.data(),air.size(),all,error))throw std::runtime_error(file+": "+error);
            for(const auto &entry:all) {
                unsigned cases=0,mismatches=0;std::cout<<"Checking "<<entry.name<<std::endl;
                try {
                    cleanup();std::vector<QGPUProgramImage> images;QGPUProgramImage direct{};PrototypeStats stats;
                    if(!GPU::Compiler::compile({entry},1,images,error) ||
                       !GPU::Compiler::compileAIRNIR(air.data(),air.size(),entry.name,{},1,direct,error,stats))throw std::runtime_error(error);
                    QGPUProgramImage runtime{};QGPUAIRNIRTiming timing{};char diagnostic[4096]{};
                    if(!qgpu_compile_air_nir(air.data(),air.size(),entry.name.c_str(),nullptr,0,1,&runtime,&timing,diagnostic,sizeof(diagnostic)) ||
                        std::memcmp(&runtime,&direct,sizeof(runtime)))throw std::runtime_error(std::string("production feature image mismatch: ")+diagnostic);
                    for(unsigned route=0;route<2;++route) {
                        if(!finalized[route][0])throw std::runtime_error("missing finalized feature capture");
                        prepare(finalized[route][0]);auto path=work+"/feature-"+entry.name+(route?".direct.nir":".glsl.nir");
                        auto dump=fopen(path.c_str(),"w");nir_print_shader(finalized[route][0],dump);fclose(dump);
                    }
                    const auto &pa=finalDescriptions[0][0],&pb=finalDescriptions[1][0];
                    for(unsigned seed=0;seed<64;++seed) {
                        auto make=[&](const QGPUProgramDescription &p) {
                            auto f=fixture("compute",seed,p,0);f.samples=1;f.sampleID=seed%4;f.localSize=4;f.baseInstance=seed%5;f.baseVertex=entry.name=="quad_table"?0:seed%3;
                            for(auto &bytes:f.buffers)bytes.resize(131072);
                            for(unsigned slot=0;slot<16;++slot)for(unsigned i=0;i<32768;++i)
                                f.put<uint32_t>(slot,4*i,raw(float(int((i+slot+seed)%17)-8)*.125f));
                            bool integer=entry.name=="air_control_flow" || entry.name=="air_add" || entry.name=="air_half_bits" || entry.name=="air_threadgroup_reduce";
                            if(integer)for(unsigned i=0;i<512;++i)f.put<uint32_t>(0,4*i,(i*0x10203u+seed*7919u)^0x7c017e00u);
                            for(unsigned i=0;i<4;++i) {
                                f.grid[i]={seed%8*4+i,0,0};f.position[i]={float(i&1)+.5f,float(i>>1)+.5f,.25f,1};
                                if(entry.name=="quad_table")f.grid[i][0]=i%3;
                                if(entry.name=="extra_math")f.position[i][2]=seed&1?std::numeric_limits<float>::quiet_NaN():.25f;
                                if(entry.name=="air_threadgroup_reduce" || entry.name=="air_atomic_counter_kernel" || entry.name=="air_dynamic_threadgroup")f.grid[i][0]=i;
                            }
                            f.constants[QGPU_DISPATCH_CONSTANT_BASE]=0;
                            f.constants[QGPU_DISPATCH_CONSTANT_BASE+4]=128;
                            f.constants[QGPU_DISPATCH_CONSTANT_BASE+8]=4;
                            f.constants[QGPU_DISPATCH_CONSTANT_BASE+12]=32;
                            for(unsigned slot=0;slot<8;++slot) {
                                f.constants[slot*2]=slot*256;f.constants[slot*2+1]=128;
                                unsigned at=QGPU_IMAGE_CONSTANT_BASE+slot*4;
                                f.constants[at]=32;f.constants[at+1]=32;f.constants[at+2]=6;
                                const unsigned formats[]={GPU::Shader::RGBA8,GPU::Shader::BGRA8,GPU::Shader::RGBA8SRGB,GPU::Shader::BGRA8SRGB,GPU::Shader::R8,GPU::Shader::RG8,GPU::Shader::RGBA16Float};
                                unsigned format=entry.name.find("uint_")!=std::string::npos?GPU::Shader::R32Uint:formats[seed%7];
                                unsigned cpp=format==GPU::Shader::R8?1:format==GPU::Shader::RG8?2:format==GPU::Shader::RGBA16Float?8:4;
                                unsigned swizzle=seed&1?(5u|(4u<<3)|(3u<<6)|(2u<<9))+1:0;
                                f.constants[at+2]=64*cpp;f.constants[at+3]=format|(6u<<8)|(swizzle<<16);
                                f.constants[QGPU_ARRAY_CONSTANT_BASE+slot*2]=12;
                                f.constants[QGPU_ARRAY_CONSTANT_BASE+slot*2+1]=16384;
                                f.constants[QGPU_SAMPLER_CONSTANT_BASE+slot]=GPU::Shader::samplerMirrorFlags(f.samplers[slot]);
                            }
                            if(p.stage!=GPU::Shader::Compute)for(unsigned slot=0;slot<8;++slot) {
                                f.constants[slot]=GPU::Shader::samplerMirrorFlags(f.samplers[slot]);
                                unsigned at=QGPU_GRAPHICS_TEXTURE_CONSTANT_BASE+slot*4;
                                f.constants[at]=32;f.constants[at+1]=32;f.constants[at+2]=12;f.constants[at+3]=GPU::Shader::RGBA8|(6u<<8);
                            }
                            if(entry.name.find("fetch_")!=std::string::npos) {
                                unsigned slot=p.stage==GPU::Shader::Fragment?8:1;
                                f.put<uint32_t>(slot,0,seed%32);f.put<uint32_t>(slot,4,seed%24);
                                f.put<uint32_t>(slot,8,seed%6);f.put<uint32_t>(slot,12,seed%6);
                                if(entry.name=="air_fetch_cube_array_fragment")f.put<uint32_t>(9,0,seed%6);
                            }
                            if(entry.name=="air_array_image_transform" || entry.name=="air_cube_image_copy")f.put<uint32_t>(0,0,seed%6);
                            if(entry.name=="air_discard_fragment")for(auto &position:f.position)position[0]+=seed&1?32:0;
                            if(seed%8==7)for(auto &bytes:f.buffers)bytes.resize(12+seed%16);
                            f.sizes(p,0);return f;
                        };
                        auto a=make(pa),b=make(pb);auto expected=a.buffers;
                        const bool reference=entry.name=="signed_bool" || entry.name=="nested_loops" || entry.name=="air_control_flow" || entry.name=="air_half_bits" || entry.name=="air_private_array" || entry.name=="air_discard_fragment";
                        auto write=[&](unsigned slot,uint64_t at,uint32_t v){if(at+4<=expected[slot].size())memcpy(expected[slot].data()+at,&v,4);};
                        auto read=[&](unsigned slot,uint64_t at){uint32_t v=0;if(at+4<=expected[slot].size())memcpy(&v,expected[slot].data()+at,4);return v;};
                        for(unsigned lane=0;lane<4;++lane) {
                            unsigned id=a.grid[lane][0];
                            if(entry.name=="nested_loops") {uint32_t sum=0;for(unsigned i=0;i<4;++i)for(unsigned j=0;j<3;++j)if(j!=(id&1))sum+=id+i+j;write(0,4ull*id,sum);}
                            if(entry.name=="signed_bool") {write(0,16ull*id,raw(id&1?-1.f:0.f));write(0,16ull*id+4,raw(id&1?1.f:0.f));write(0,16ull*id+8,id&1?UINT32_MAX:0);}
                            if(entry.name=="air_control_flow") {uint32_t x=read(0,4ull*id),y=x^165;for(unsigned i=0;i<(id&7)+1;++i)std::swap(x,y);
                                uint32_t result=(id&3)==0?x+11:(id&3)==1?x^17:x+y;if(id&1)result+=y;write(1,4ull*id,result);}
                            if(entry.name=="air_half_bits")for(unsigned c=0;c<2;++c){auto x=read(0,8ull*id+4*c);write(1,8ull*id+4*c,(x>>16)|(x<<16));}
                            if(entry.name=="air_private_array")for(unsigned c=0;c<4;++c)write(0,16ull*id+4*c,id^(id&7));
                        }
                        if(entry.name=="air_discard_fragment")write(9,0,seed&1?22:11);
                        auto x=Executor(finalized[0][0],pa,a).run(),y=Executor(finalized[1][0],pb,b).run();
                        bool mismatch=a.buffers!=b.buffers || (reference && (a.buffers!=expected || b.buffers!=expected));
                        for(unsigned l=0;l<4;++l)for(unsigned t=0;t<32;++t)for(unsigned c=0;c<4;++c)
                            if(!(x[l][t][c]==y[l][t][c] || (std::isnan(x[l][t][c]) && std::isnan(y[l][t][c]))))mismatch=true;
                        if(mismatch){++mismatches;if(mismatches<=2)std::cerr<<entry.name<<" mismatch seed="<<seed<<" buffers="<<(a.buffers!=b.buffers)<<" reference="<<(reference && (a.buffers!=expected || b.buffers!=expected))<<'\n';}
                        ++cases;
                    }
                    ++tested;totalCases+=cases;if(mismatches)++failures;
                    report<<entry.name<<'\t'<<cases<<'\t'<<mismatches<<'\t'<<(mismatches?"FAIL":"PASS")<<std::endl;
                } catch(const std::exception &e) {++failures;report<<entry.name<<'\t'<<cases<<'\t'<<mismatches<<"\tERROR: "<<e.what()<<std::endl;std::cerr<<entry.name<<": "<<e.what()<<'\n';}
            }
        }
    } catch(const std::exception &e) {++failures;report<<"suite\t0\t0\tERROR: "<<e.what()<<std::endl;}
    std::ofstream summary(work+"/features.json");summary<<"{\"hardware_tested\":false,\"entries_executed\":"<<tested<<",\"quad_cases\":"<<totalCases<<",\"failed_entries\":"<<failures<<",\"passed\":"<<(failures?"false":"true")<<"}\n";
    prototype_capture_enable(0);cleanup();glsl_type_singleton_decref();return failures?1:0;
}

// These instructions exceed the legacy text frontend's coverage. Compare the
// finalized production NIR with an independent integer/control-flow oracle.
int runRemainingDifferential(const std::string &work) {
    glsl_type_singleton_init_or_ref();prototype_capture_enable(1);
    std::ofstream report(work+"/remaining.tsv");report<<"entry\tcases\tmismatches\tstatus\n";
    unsigned tested=0,total=0,failures=0;
    try {
        auto air=readFile(work+"/inputs/direct-nir-remaining.air.ll");std::string error;
        std::vector<std::string> names;
        if(!GPU::Compiler::inspectAIREntries(air.data(),air.size(),names,error))throw std::runtime_error(error);
        for(const auto &name:names) {
            unsigned cases=0,mismatches=0;std::cout<<"Checking "<<name<<std::endl;
            try {
                cleanup();QGPUProgramImage image{},runtime{};PrototypeStats stats;
                if(!GPU::Compiler::compileAIRNIR(air.data(),air.size(),name,{},1,image,error,stats,work+"/remaining-"+name+".nir"))throw std::runtime_error(error);
                QGPUAIRNIRTiming timing{};char diagnostic[4096]{};
                if(!qgpu_compile_air_nir(air.data(),air.size(),name.c_str(),nullptr,0,1,&runtime,&timing,diagnostic,sizeof(diagnostic)) ||
                    std::memcmp(&image,&runtime,sizeof(image)))throw std::runtime_error(std::string("runtime image mismatch: ")+diagnostic);
                {std::ofstream binary(work+"/remaining-"+name+".image",std::ios::binary);binary.write(reinterpret_cast<const char *>(&image),sizeof(image));}
                if(!finalized[1][0])throw std::runtime_error("no finalized NIR");
                prepare(finalized[1][0]);auto file=fopen((work+"/remaining-"+name+".final.nir").c_str(),"w");nir_print_shader(finalized[1][0],file);fclose(file);
                auto &p=finalDescriptions[1][0];
                for(unsigned seed=0;seed<256;++seed) {
                    auto f=fixture("compute",seed,p,0);f.localSize=4;f.constants[QGPU_DISPATCH_CONSTANT_BASE]=0;
                    const uint64_t boundary[]={0,1,0xffffffffull,0x100000000ull,0xffffffffffffffffull,0x8000000000000000ull,
                        0x7fffffffffffffffull,0x123456789abcdef0ull,0xfedcba9876543210ull,0x8000000100000001ull};
                    for(unsigned slot=0;slot<16;++slot) {
                        f.buffers[slot].resize(32768);
                        for(unsigned n=0;n<4096;++n) {
                            auto x=boundary[(n+seed+slot*3)%10] ^ (seed<32?0ull:uint64_t(seed*131071u+n*17u)<<16);
                            f.put<uint64_t>(slot,n*8,x);
                        }
                    }
                    for(unsigned lane=0;lane<4;++lane) {
                        unsigned id=(seed%16==15?0:seed%16)*4+lane;f.grid[lane]={id,0,0};
                        if(name=="wide_address") {const uint64_t offsets[]={0,4,8,28,32,0x100000000ull,UINT64_MAX-3,UINT64_MAX-7};f.put<uint64_t>(1,id*8,offsets[(seed+lane)%8]);}
                        // Avoid LLVM's one signed division overflow case.
                        uint64_t x,y;memcpy(&x,f.buffers[1].data()+id*8,8);memcpy(&y,f.buffers[2].data()+id*8,8);
                        if(x==0x8000000000000000ull && (y|1)==UINT64_MAX)f.put<uint64_t>(2,id*8,3);
                    }
                    if(seed%16==15)for(auto &bytes:f.buffers)bytes.resize(4+seed%29);
                    f.sizes(p,0);auto expected=f.buffers;
                    auto get32=[&](unsigned slot,uint64_t at){uint32_t v=0;if(at+4<=expected[slot].size())memcpy(&v,expected[slot].data()+at,4);return v;};
                    auto put32=[&](unsigned slot,uint64_t at,uint32_t v){if(at+4<=expected[slot].size())memcpy(expected[slot].data()+at,&v,4);};
                    auto get64=[&](unsigned slot,uint64_t at){return uint64_t(get32(slot,at))|(uint64_t(get32(slot,at+4))<<32);};
                    auto put64=[&](unsigned slot,uint64_t at,uint64_t v){put32(slot,at,uint32_t(v));put32(slot,at+4,uint32_t(v>>32));};
                    for(unsigned lane=0;lane<4;++lane) {
                        unsigned id=f.grid[lane][0];
                        if(name=="multi_exit")put32(0,id*4,id==0?99:((id&7)+1)%2?23+(id&7)+1:11+(id&7)+1);
                        else if(name=="irreducible") {
                            unsigned i=0;uint32_t x=1,y=7;bool left=id&1;
                            while(true){x+=left?id:3;++i;if(i>=(id&7)+2)break;std::swap(x,y);left=!left;}
                            put32(0,id*4,x);
                        } else if(name=="pointer_phi") {
                            unsigned slot=id&1?1:2,other=3-slot;uint32_t sum=0;
                            for(unsigned i=0;i<4;++i){sum+=get32(slot,4ull*(id*8+i));std::swap(slot,other);}
                            auto at=4ull*(id*8+4);auto old=get32(slot,at);put32(slot,at,old+id);put32(0,id*4,sum+old);
                        } else if(name=="pointer_local_null")put32(0,id*4,id&2?0:id&1?13:29);
                        else if(name=="wide_address") {
                            auto at=get64(1,id*8);uint64_t value=0;
                            for(unsigned word=0;word<2;++word) {auto address=at+4*word;
                                if(at<=UINT32_MAX && !(address&3) && address<=UINT32_MAX)value|=uint64_t(get32(2,address))<<(word*32);}
                            put64(0,id*8,value);
                        } else if(name=="wide_shared")put64(0,id*8,get64(1,id*8)+0x100000001ull);
                        else if(name=="wide_vector") {
                            uint64_t x=get64(1,id*16),y=get64(1,id*16+8);
                            put64(0,id*32,x+0x100000001ull);put64(0,id*32+8,y-0x100000001ull);
                            put32(0,id*32+16,raw(float(x)));put32(0,id*32+20,raw(float(y)));
                        } else if(name=="wide_phi") {
                            uint64_t x=get64(1,id*8),y=x^0x100000001ull;
                            for(unsigned i=0;i<=(id&7);++i){auto n=x+y;x=y;y=n;}put64(0,id*8,x);
                        } else {
                            auto x=get64(1,id*8),y=get64(2,id*8)|1;int64_t sx,sy;memcpy(&sx,&x,8);memcpy(&sy,&y,8);
                            unsigned shift=id&63;uint64_t signedShift=x>>shift;if(shift && sx<0)signedShift|=UINT64_MAX<<(64-shift);
                            uint64_t rotation=((x&0xffff)<<48)|(((x>>16)&0xffff)<<32)|(((x>>32)&0xffff)<<16)|(x>>48);
                            uint64_t v[]={x+y,x-y,x*y,x/y,x%y,uint64_t(sx/sy),uint64_t(sx%sy),x&y,x|y,x^y,x<<shift,x>>shift,signedShift,
                                x==y,x!=y,x<y,x<=y,x>y,x>=y,sx<sy,sx<=sy,sx>sy,sx>=sy,
                                raw(float(sx)),raw(float(x)),rotation,x<y?x:y,id&1?UINT64_MAX:0,
                                uint64_t(int64_t(int16_t(x))),uint64_t(uint16_t(x)),uint64_t(int64_t(float(id)*-1.5f)),id};
                            unsigned start=0,end=0;
                            if(name.rfind("wide_div_",0)==0){start=unsigned(name.back()-'0');end=start+1;}
                            else {unsigned group=name.back()-'0';const unsigned begin[]={0,7,10,23},last[]={3,10,23,32};start=begin[group];end=last[group];}
                            for(unsigned i=start;i<end;++i)put64(0,8ull*(id*32+i),v[i]);
                        }
                    }
                    if(seed==0 || seed==1 || seed==15 || seed==31) {
                        const auto stem=work+"/remaining-"+name+"-"+std::to_string(seed);
                        std::ofstream meta(stem+".json");meta<<"{\"entry\":\""<<name<<"\",\"grid_origin\":"<<f.grid[0][0]<<",\"threads\":4,\"threads_per_group\":4,\"buffer_mask\":"<<p.bufferMask<<",\"write_mask\":"<<p.writeMask<<",\"shared_bytes\":"<<p.sharedBytes<<",\"hardware_tested\":false}\n";
                        for(unsigned slot=0;slot<3;++slot) {
                            std::ofstream input(stem+"-buffer"+std::to_string(slot)+".input.bin",std::ios::binary),output(stem+"-buffer"+std::to_string(slot)+".expected.bin",std::ios::binary);
                            input.write(reinterpret_cast<const char *>(f.buffers[slot].data()),f.buffers[slot].size());
                            output.write(reinterpret_cast<const char *>(expected[slot].data()),expected[slot].size());
                        }
                    }
                    Executor(finalized[1][0],p,f).run();
                    if(f.buffers!=expected) {
                        ++mismatches;
                        if(mismatches<=3)for(unsigned slot=0;slot<16;++slot)if(f.buffers[slot]!=expected[slot]) {
                            for(unsigned at=0;at+4<=expected[slot].size();at+=4)if(memcmp(f.buffers[slot].data()+at,expected[slot].data()+at,4)){
                                uint32_t actual,want;memcpy(&actual,f.buffers[slot].data()+at,4);memcpy(&want,expected[slot].data()+at,4);
                                std::cerr<<name<<" seed="<<seed<<" slot="<<slot<<" byte="<<at<<" actual="<<actual<<" expected="<<want<<'\n';break;
                            }break;
                        }
                    }
                    ++cases;
                }
                ++tested;total+=cases;if(mismatches)++failures;
                report<<name<<'\t'<<cases<<'\t'<<mismatches<<'\t'<<(mismatches?"FAIL":"PASS")<<std::endl;
            }catch(const std::exception &e){++failures;report<<name<<'\t'<<cases<<'\t'<<mismatches<<"\tERROR: "<<e.what()<<std::endl;std::cerr<<name<<": "<<e.what()<<'\n';}
        }
    }catch(const std::exception &e){++failures;std::cerr<<e.what()<<'\n';}
    std::ofstream summary(work+"/remaining.json");summary<<"{\"hardware_tested\":false,\"entries_executed\":"<<tested<<",\"quad_cases\":"<<total<<",\"failed_entries\":"<<failures<<",\"passed\":"<<(failures?"false":"true")<<"}\n";
    prototype_capture_enable(0);cleanup();glsl_type_singleton_decref();return failures?1:0;
}
