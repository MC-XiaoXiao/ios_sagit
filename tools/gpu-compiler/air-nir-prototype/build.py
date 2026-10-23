#!/usr/bin/env python3
"""Build the isolated host prototype against the existing pinned Mesa objects.
Does not run Ninja builds, change production sources, or install anything.
"""
import hashlib,json,os,shlex,shutil,subprocess,sys
from pathlib import Path
root=Path(__file__).resolve().parents[3]
source=Path(__file__).resolve().parent
work=Path(sys.argv[1]).resolve() if len(sys.argv)>1 else root/'build/air-nir-prototype-20261020'
host=root/'build/gpu-compiler/host';snapshot=work/'snapshot';work.mkdir(exist_ok=True,parents=True)
assert snapshot.is_dir(), 'Freeze the production sources and input cases first'
(snapshot/'QcomInterface.h').write_bytes((snapshot/'Interface.h').read_bytes())
backend=(snapshot/'Backend.c').read_text()
needle='    ir3_finalize_nir(compiler,&(struct ir3_shader_nir_options){},nir);'
assert backend.count(needle)==1
backend=backend.replace(needle,needle+'\n    if(prototype_validating)prototype_validate(nir);\n    if(prototype_capturing)prototype_capture_variant(nir,description,msaa,fullWindow);')
# Capture independently finalized NIR for every variant, including pairs the
# production compiler can reuse. The differential runner compares their full
# JIT images with the optimized production library as well as their semantics.
reuse='    if (!invariant)'
if 'static bool compile_variants(' in backend:
 assert backend.count(reuse)==1
 backend=backend.replace(reuse,'    if (!invariant || prototype_capturing)')
 # Force a fresh finalization for the MSAA capture even when production now
 # shares sample-independent finalized NIR within the pair.
 if 'struct ir3_shader *shared=NULL;' in backend:
  needle='        ok=compile_variant(prepared,kind,&msaa->info'
  assert backend.count(needle)==1
  backend=backend.replace(needle,'        if(prototype_capturing && shared){ir3_shader_destroy(shared);shared=NULL;}\n'+needle)
else:
 # Older frozen snapshots predate variant reuse. Keep their independent
 # compiler calls when using the current adapter with a historical snapshot.
 backend+='''
static bool compile_variants(struct qgpu_prepared_shader *prepared, unsigned kind,
    QGPUProgramDescription *description, uint32_t *code, uint32_t *immediates, QGPUProgramVariant *msaa,
    bool fullWindow, char *error, unsigned errorBytes) {
    return compile_variant(prepared,kind,description,code,immediates,false,fullWindow,error,errorBytes) &&
        (kind!=2 || compile_variant(prepared,kind,&msaa->info,msaa->code,msaa->immediates,true,fullWindow,error,errorBytes));
}
'''
capture='    ir3_nir_lower_io_to_temporaries(nir);'
assert backend.count(capture)>=1
backend=backend.replace(capture,'    if(prototype_capturing)prototype_capture(nir,description,0);\n'+capture,1)
(work/'CheckedBackend.c').write_text(backend)
command=next(x for x in json.loads((host/'compile_commands.json').read_text()) if x.get('output')=='src/freedreno/QcomGPUCompiler.so.p/QcomBackend.c.o')
base=shlex.split(command['command']);argv=base.copy()
argv[argv.index('-o')+1]=str(work/'backend.o');argv[argv.index('-MF')+1]=str(work/'backend.d');argv[-1]=str(source/'BackendAdapter.c')
argv[1:1]=['-I'+str(work),'-I'+str(snapshot),'-I'+str(source)]
receipts=[];executed=[]
cache_file=work/'build-signatures.json'
cache=json.loads(cache_file.read_text()) if cache_file.exists() else {}
def compile_cached(args,file,output):
 receipts.append(args)
 dependencies=[Path(file),source/'Prototype.h',work/'CheckedBackend.c',*sorted(snapshot.glob('*'))]
 depfile=Path(args[args.index('-MF')+1])
 if depfile.exists():
  for name in shlex.split(depfile.read_text().replace('\\\n',' ').split(':',1)[1]):
   path=Path(name);path=path if path.is_absolute() else host/path
   if path.is_file():dependencies.append(path.resolve())
 signature=hashlib.sha256(json.dumps(args).encode()+b''.join(p.read_bytes() for p in sorted(set(dependencies)) if p.is_file())).hexdigest()
 if Path(output).is_file() and cache.get(str(output))==signature:
  return
 run(args,record=False);cache[str(output)]=signature;cache_file.write_text(json.dumps(cache,indent=2)+'\n')
def run(args,record=True):
 if record:receipts.append(args)
 executed.append(args)
 result=subprocess.run(args,cwd=host,stdout=subprocess.PIPE,stderr=subprocess.STDOUT,text=True)
 with (work/'build.log').open('a') as f:f.write(result.stdout)
 if result.returncode:print(result.stdout[-18000:]);raise SystemExit(result.returncode)
(work/'build.log').write_text('');compile_cached(argv,source/'BackendAdapter.c',work/'backend.o')
cpp_command=next(x for x in json.loads((host/'compile_commands.json').read_text()) if x.get('output')=='src/freedreno/QcomGPUCompiler.so.p/QcomAIRNIR.cpp.o')
# Match the production C++ compiler and flags: GCC and Clang may legally emit
# builder argument expressions in different orders, producing different ISA.
cpp=shlex.split(cpp_command['command'])
cpp[1:1]=['-g','-I'+str(snapshot),'-I'+str(source)]
validation=argv.copy();validation[validation.index('-o')+1]=str(work/'validate.o');validation[validation.index('-MF')+1]=str(work/'validate.d');validation[-1]=str(source/'Validate.c')
compile_cached(validation,source/'Validate.c',work/'validate.o')
objects=[work/'backend.o',work/'validate.o']
for file,label in [(source/'DirectNIR.cpp','direct'),(snapshot/'Compiler.cpp','compiler'),(source/'Differential.cpp','differential'),(source/'Benchmark.cpp','benchmark')]:
 args=cpp.copy();args[args.index('-o')+1]=str(work/(label+'.o'));args[args.index('-MF')+1]=str(work/(label+'.d'));args[-1]=str(file);compile_cached(args,file,work/(label+'.o'));objects.append(work/(label+'.o'))
commands=subprocess.check_output(['ninja','-C',str(host),'-t','commands','src/freedreno/QcomGPUCompiler.so'],text=True).splitlines()
link=shlex.split(next(x for x in reversed(commands) if ' -o src/freedreno/QcomGPUCompiler.so ' in x))
link[link.index('-o')+1]=str(work/'benchmark')
link=[x for x in link if x!='-shared' and not x.startswith('-Wl,-soname,') and not x.endswith('/QcomAIRNIR.cpp.o')]
at=link.index('src/freedreno/QcomGPUCompiler.so.p/QcomBackend.c.o');link[at:at+1]=list(map(str,objects))
runtime=Path(os.environ.get('AIR_NIR_RUNTIME_COMPILER',host/'src/freedreno/QcomGPUCompiler.so')).resolve()
frozen_runtime=work/'QcomGPUCompiler.so'
shutil.copyfile(runtime,frozen_runtime)
link += [str(frozen_runtime),'-Wl,-rpath,'+str(work)]
link+=shlex.split(subprocess.check_output(['llvm-config','--ldflags','--libs','core','irreader','analysis','passes'],text=True));run(link)
libraries={str((host/x).resolve()):hashlib.sha256((host/x).read_bytes()).hexdigest() for x in link if x.endswith('.a')}
(work/'build-receipt.json').write_text(json.dumps({'commands':receipts,'executed':executed,'static_libraries_sha256':libraries,'runtime_compiler_sha256':hashlib.sha256(frozen_runtime.read_bytes()).hexdigest(),'hardware_tested':False},indent=2)+'\n')
print(work/'benchmark')
