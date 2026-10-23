// SPDX-License-Identifier: BSD-2-Clause
#include "Prototype.h"
#include <algorithm>
#include <chrono>
#include <ctime>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <iterator>
#include <sstream>
#include <cstring>
using namespace GPU::Compiler;
static std::string readFile(const std::string &p){std::ifstream f(p,std::ios::binary);if(!f)throw std::runtime_error("missing input "+p);return {std::istreambuf_iterator<char>(f),{}};}
static void writeFile(const std::string &p,const void *d,size_t n){std::ofstream f(p,std::ios::binary);f.write(static_cast<const char *>(d),n);}
static std::string quote(const std::string &s){std::string o="\"";for(unsigned char c:s){if(c=='\\' || c=='\"'){o+='\\';o+=c;}else if(c=='\n')o+="\\n";else if(c=='\r')o+="\\r";else if(c=='\t')o+="\\t";else if(c>=32)o+=c;}return o+'\"';}
static std::string hash(const void *p,size_t n){uint64_t h=14695981039346656037ull;for(auto b=static_cast<const unsigned char *>(p);n;--n)h=(h^*b++)*1099511628211ull;std::ostringstream s;s<<std::hex<<h;return s.str();}
static double cpu(){timespec t;clock_gettime(CLOCK_THREAD_CPUTIME_ID,&t);return t.tv_sec*1000.0+t.tv_nsec/1e6;}
static double median(std::vector<double> v){std::sort(v.begin(),v.end());return v[v.size()/2];}
static std::vector<FunctionConstantValue> constants(const std::string &file){std::vector<FunctionConstantValue> out;if(file.empty())return out;std::istringstream f(readFile(file));unsigned index,type;std::string hex;
 while(f>>index>>type>>hex){FunctionConstantValue v;v.index=index;v.type=type;if(hex.size()!=2*constantBytes(type))throw std::runtime_error("invalid constant size");for(unsigned n=0;n<hex.size()/2;++n)v.bytes[n]=std::stoul(hex.substr(n*2,2),nullptr,16);out.push_back(v);}return out;}
static void variantJSON(std::ostream &s,const QGPUProgramDescription &p,const uint32_t *code){s<<"{\"code_words\":"<<p.codeWords<<",\"instruction_units\":"<<p.instrlen<<",\"full_regs\":"<<p.fullRegs<<",\"half_regs\":"<<p.halfRegs<<",\"constant_vec4s\":"<<p.constlen<<",\"branch_stack\":"<<p.branchStack<<",\"constant_windows\":"<<p.constantWindowCount<<",\"code_fnv1a64\":"<<quote(hash(code,p.codeWords*4))<<",\"buffer_mask\":"<<p.bufferMask<<",\"sample_mask\":"<<p.sampleMask<<",\"sampler_mask\":"<<p.samplerMask<<",\"framebuffer_mask\":"<<p.framebufferMask<<"}";}
static void artifacts(const std::string &prefix,QGPUProgramImage &image) {
 const char *names[]={"base","msaa","window","msaa-window"};
 for(unsigned i=0;i<4;++i){auto &p=i==0?image.info:i==1?image.msaa.info:i==2?image.constantWindow.info:image.msaaConstantWindow.info;
  auto code=i==0?image.code:i==1?image.msaa.code:i==2?image.constantWindow.code:image.msaaConstantWindow.code;
  if(!p.codeWords)continue;auto path=prefix+"."+names[i];writeFile(path+".bin",code,p.codeWords*4);
  if(!prototype_disassemble(code,p.codeWords,(path+".asm").c_str()))throw std::runtime_error("disassembly failed "+path);
 }
}
static bool sameInterface(const QGPUProgramDescription &a,const QGPUProgramDescription &b) {
 // Register allocation, immediate placement, padding and instruction count
 // intentionally differ. Compare the actual externally visible contract.
 auto p=a,w=b;
 // Reuse the production check after adding its full-window marker to 'w'.
 w.flags|=QGPU_CONSTANT_WINDOWS_REQUIRED;
 // For compute, compare the relevant fields directly (there is no window).
 if(a.stage==GPU::Shader::Fragment)return GPU::JIT::compatibleConstantWindow(p,w);
 return a.stage==b.stage && a.flags==b.flags && a.bufferMask==b.bufferMask && a.writeMask==b.writeMask &&
  a.instanceMask==b.instanceMask && a.sampleMask==b.sampleMask && a.samplerMask==b.samplerMask &&
  a.textureBindings==b.textureBindings && !memcmp(a.strides,b.strides,sizeof(a.strides));
}
int main(int argc,char **argv){
 if(argc!=3)return 64;const std::string w=argv[1],label=argv[2];
 if(label=="diff")return runDifferential(w);
 if(label=="remaining")return runRemainingDifferential(w);
 if(label=="features")return runFeatureDifferential(w);
 if(label=="checks") {
  try {
   auto air=readFile(w+"/inputs/compute.air.ll");std::string error;PrototypeStats stats;QGPUProgramImage image{};unsigned checks=0;
   auto rejected=[&](const std::string &bytes,const std::string &entry,const std::string &reason){
    if(compileAIRNIR(bytes.data(),bytes.size(),entry,{},1,image,error,stats) || error.find(reason)==std::string::npos)
     throw std::runtime_error("missing rejection for "+entry+": "+error);
    std::cout<<entry<<": "<<error<<'\n';++checks;
   };
   rejected(air,"absent_entry","absent");
   rejected("invalid AIR payload","air_add","AIR");
   if(!compileAIRNIR(air.data(),air.size(),"air_control_flow",{},1,image,error,stats,w+"/control-flow.direct.nir"))throw std::runtime_error(error);++checks;
   // Invalid atomic orders remain explicit failures.
   auto changed=air;auto atomicAt=changed.find("atomicrmw min",changed.find("define void @air_atomic_values"));
   if(atomicAt==std::string::npos)throw std::runtime_error("atomic fixture changed");
   auto orderAt=changed.find("monotonic",atomicAt);changed.replace(orderAt,9,"seq_cst");rejected(changed,"air_atomic_values","relaxed");
   if(!qgpu_check_buffer_guards())throw std::runtime_error("production buffer guard checks failed");++checks;
   std::cout<<"offline checks passed: "<<checks<<'\n';return 0;
  }catch(const std::exception &e){std::cerr<<e.what()<<'\n';return 1;}
 }
 struct Case{std::string file,entry,values;};Case cs;
 if(label=="compute")cs={"compute.air","air_add",""};
 else if(label=="welcome")cs={"welcome.air","fixed_frag_lph_cpf","welcome.constants.txt"};
 else if(label=="fluidity")cs={"fluidity.air","fluidity::fragmentShader","fluidity.constants.txt"};else return 64;
 try {
  auto air=readFile(w+"/inputs/"+cs.file);auto values=constants(cs.values.empty()?"":w+"/inputs/"+cs.values);
  std::vector<double> wall[2],cpus[2];std::vector<double> stageParse,stageEmit,stageBackend;
  QGPUProgramImage images[2]{};std::string imageHashes[2],error;PrototypeStats stats;
  std::string glsl;unsigned repeats=31;
  auto perform=[&](unsigned route,bool timed){auto start=std::chrono::steady_clock::now();double beginCPU=cpu();
   if(!route){std::vector<Entry> e;std::vector<QGPUProgramImage> output;
    if(!lowerAIR(air.data(),air.size(),e,error,cs.entry,values) || !compile(e,1,output,error))throw std::runtime_error("GLSL baseline: "+error);
    if(output.size()!=1)throw std::runtime_error("baseline image count");images[0]=output[0];if(!timed)glsl=e[0].glsl;
   }else if(!compileAIRNIR(air.data(),air.size(),cs.entry,values,1,images[1],error,stats,timed?"":w+"/"+label+".direct.nir"))throw std::runtime_error("direct NIR: "+error);
   double endCPU=cpu();auto end=std::chrono::steady_clock::now();
   auto digest=hash(&images[route],sizeof(images[route]));if(imageHashes[route].empty())imageHashes[route]=digest;else if(imageHashes[route]!=digest)throw std::runtime_error("nondeterministic output");
   if(timed){wall[route].push_back(std::chrono::duration<double,std::milli>(end-start).count());cpus[route].push_back(endCPU-beginCPU);if(route){stageParse.push_back(stats.parseMS);stageEmit.push_back(stats.emitMS);stageBackend.push_back(stats.backendMS);}}
  };
  prototype_set_validation(1);
  std::cerr<<label<<": baseline warmup\n";perform(0,false);std::cerr<<label<<": direct warmup\n";perform(1,false);
  bool interfacesMatch=true;
  for(unsigned i=0;i<4;++i){const auto &a=i==0?images[0].info:i==1?images[0].msaa.info:i==2?images[0].constantWindow.info:images[0].msaaConstantWindow.info;
   const auto &d=i==0?images[1].info:i==1?images[1].msaa.info:i==2?images[1].constantWindow.info:images[1].msaaConstantWindow.info;
   if(bool(a.codeWords)!=bool(d.codeWords) || (a.codeWords && !sameInterface(a,d)))interfacesMatch=false;
  }
  if(!interfacesMatch)throw std::runtime_error("route resource interfaces differ");
  prototype_set_validation(0);
  for(unsigned repeat=0;repeat<repeats;++repeat){perform(repeat&1,true);perform(1-(repeat&1),true);}
  std::ostringstream report;report<<std::setprecision(10)<<"{\"case\":"<<quote(label)<<",\"entry\":"<<quote(cs.entry)<<",\"success\":true,\"hardware_tested\":false,\"repeats\":"<<repeats
   <<",\"air_bytes\":"<<air.size()<<",\"interfaces_match\":true,\"deterministic_images\":true,\"nir_validated\":true,\"jit_validated\":true,\"semantic_equivalence_tested\":false,\"specialized_blocks\":"<<stats.blocks<<",\"specialized_instructions\":"<<stats.instructions<<",\"routes\":[";
  for(unsigned route=0;route<2;++route){if(route)report<<',';report<<"{\"name\":"<<quote(route?"air-nir-ir3":"air-glsl-nir-ir3")<<",\"median_wall_ms\":"<<median(wall[route])<<",\"median_cpu_ms\":"<<median(cpus[route])<<",\"image_fnv1a64\":"<<quote(imageHashes[route])<<",\"variants\":[";
   variantJSON(report,images[route].info,images[route].code);report<<',';variantJSON(report,images[route].msaa.info,images[route].msaa.code);report<<',';variantJSON(report,images[route].constantWindow.info,images[route].constantWindow.code);report<<',';variantJSON(report,images[route].msaaConstantWindow.info,images[route].msaaConstantWindow.code);report<<"]}";
   writeFile(w+"/"+label+(route?".direct.image":".glsl.image"),&images[route],sizeof(images[route]));
   artifacts(w+"/"+label+(route?".direct":".glsl"),images[route]);
  }
  report<<"],\"direct_stage_median_wall_ms\":{\"parse_and_specialize\":"<<median(stageParse)<<",\"emit_nir\":"<<median(stageEmit)<<",\"backend\":"<<median(stageBackend)<<"},\"images_identical\":"<<(memcmp(&images[0],&images[1],sizeof(images[0]))?"false":"true")<<",\"cpu_speedup\":"<<median(cpus[0])/median(cpus[1])<<",\"samples\":[";
  for(unsigned i=0;i<repeats;++i){if(i)report<<',';report<<"{\"glsl_cpu_ms\":"<<cpus[0][i]<<",\"nir_cpu_ms\":"<<cpus[1][i]<<",\"glsl_wall_ms\":"<<wall[0][i]<<",\"nir_wall_ms\":"<<wall[1][i]<<"}";}report<<"]}\n";
  writeFile(w+"/"+label+".baseline.glsl",glsl.data(),glsl.size());auto text=report.str();writeFile(w+"/"+label+".result.json",text.data(),text.size());std::cout<<text;
 }catch(const std::exception &e){std::cerr<<e.what()<<"\n";return 1;}
}
