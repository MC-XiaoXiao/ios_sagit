"""GPU compiler and Metal runtime builds."""
import hashlib
import json
import os
from pathlib import Path
import re
import shutil
import shlex
import subprocess
import sys
from project.build import context as build_state


def build_gpu_client():
    from project.build import clients as _component_clients
    _component_clients.build_iokit_client('gpuctl', ['gpuctl/gpuctl.cpp', 'gpuctl/GPUConnection.cpp'],
                       extra_exports=['IOConnectCallMethod', 'IOObjectConformsTo', 'IORegistryEntryCreateCFProperty',
                                      'IODataQueueDataAvailable', 'IODataQueueDequeue',
                                      'IOConnectSetNotificationPort', 'IODataQueueAllocateNotificationPort'],
                       headers=['gpuctl/GPUConnection.h', 'gpuctl/CPUStatus.h', 'include/IOKitUser.h'],
                       protocol=['PowerUserProtocol.h', 'ClockControllerProtocol.h', 'GPUUserProtocol.h', 'GPUNativeProtocol.h', 'GPURenderPass.h', 'GPUMetalCapabilities.h', 'GPUNativeCompletion.h', 'GPUNativeMemory.h', 'GPUDiagnostics.h', 'GPUHardwarePhases.h', 'GPUMappingBounds.h', 'GPUJobActivity.h', 'GPUOperationActivity.h', 'GPUShaderCounters.h', 'GPUSurfaceProtocol.h', 'GPUShaderInterface.h', 'GPUShaderProtocol.h', 'GPUJITProtocol.h'],
                       adhoc_sign=True,
                       entitlements={'org.darwin.development.gpu-control':True},
                       signing_identifier='org.darwin.development.gpuctl')


def translate_gpu_msl(source):
    """AOT and runtime JIT share the production C++ MSL parser."""
    from project.build import native as _component_native
    root = build_state.BUILD / 'obj/gpu-compiler'; root.mkdir(parents=True, exist_ok=True)
    exe = root / 'msl-frontend'; stamp = root / 'msl-frontend.json'
    sources = [build_state.TOOLS / 'gpu-compiler' / name for name in ['MSLMain.cpp','MSL.cpp','Semantic.h','Compiler.h']]
    sources += [build_state.DRIVERS_ROOT / 'include' / name for name in ['GPUShaderInterface.h','GPUShaderProtocol.h','GPUJITProtocol.h','GPUUserProtocol.h']]
    hashes = {str(path.relative_to(build_state.ROOT)):_component_native.digest(path) for path in sources}
    if not exe.exists() or not stamp.exists() or json.loads(stamp.read_text()).get('inputs') != hashes or json.loads(stamp.read_text()).get('sha256') != _component_native.digest(exe):
        config = _component_native.host('llvm-config')
        flags = shlex.split(subprocess.check_output([config,'--cxxflags','--ldflags','--libs','support'],text=True))
        flags = [flag for flag in flags if flag != '-fno-exceptions' and not flag.startswith('-std=')]
        _component_native.run([_component_native.host('clang++'),build_state.TOOLS/'gpu-compiler/MSLMain.cpp',build_state.TOOLS/'gpu-compiler/MSL.cpp',*flags,
             '-std=c++17','-O2','-Wall','-Wextra','-Werror','-Wno-unused-parameter','-I'+str(build_state.DRIVERS_ROOT/'include'),
             '-o',exe],log=root/'msl-build.log')
        stamp.write_text(json.dumps({'inputs':hashes,'sha256':_component_native.digest(exe)},indent=2)+'\n')
    result = subprocess.run([exe],input=source,capture_output=True,text=True)
    if result.returncode: raise RuntimeError(result.stderr.strip() or 'MSL frontend failed')
    return json.loads(result.stdout)


def patch_gpu_register_delays(mesa):
    """Keep IR3's ALU latency accounting within each physical register file."""
    path = mesa / 'src/freedreno/ir3/ir3_legalize.c'
    text = path.read_text()
    needle = '            for (unsigned matching_size = 0; matching_size < 2; matching_size++) {\n'
    replacement = needle + '''               /* A5xx has independent half/full register files. A texture
                * write must not clear an unrelated ALU result's ready cycle.
                * Only merged register files have cross-size aliases.
                */
               if (!matching_size && !mergedregs)
                  continue;
'''
    if replacement in text:
        return
    if text.count(needle) != 1:
        raise RuntimeError('Mesa IR3 register delay accounting changed')
    path.write_text(text.replace(needle, replacement, 1))


def prepare_gpu_compiler():
    from project.build import native as _component_native
    root = build_state.BUILD / 'obj/gpu-compiler'
    source = root / 'mesa'
    spec = json.loads((build_state.TOOLS / 'gpu-compiler/Source.json').read_text())
    if not (source / '.git').exists():
        _component_native.run([_component_native.host('git'), 'clone', '--depth', '1', '--filter=blob:none', '--sparse',
             '--branch', spec['tag'], spec['repository'], source])
        _component_native.run([_component_native.host('git'), '-C', source, 'sparse-checkout', 'set', *spec['sparse_paths']])
    commit = subprocess.check_output([_component_native.host('git'), '-C', source, 'rev-parse', 'HEAD'],text=True).strip()
    if commit != spec['commit']: raise RuntimeError('Mesa compiler source commit changed')
    patch_gpu_register_delays(source)
    # Modern glibc exposes C23 call_once from stdlib.h. Mesa 24's emulation
    # conflicts with that type; use the detected native C11 API on Linux.
    meson_root = source / 'meson.build'
    text = meson_root.read_text()
    updated = text.replace("if cc.has_function('thrd_create', prefix: '#include <threads.h>')\n  if with_platform_android",
        "if cc.has_function('thrd_create', prefix: '#include <threads.h>')\n  if with_platform_android or host_machine.system() == 'linux'")
    if text != updated: meson_root.write_text(updated)
    # glibc's native mtx_t wraps pthread_mutex_t (as in glibc mtx_init.c).
    # Mesa's monotonic-condvar helper crosses that API boundary explicitly.
    monotonic = source / 'src/util/cnd_monotonic.c'
    text = monotonic.read_text()
    updated = text.replace('&cond->cond, mtx,', '&cond->cond, (pthread_mutex_t *)mtx,')
    updated = updated.replace('&cond->cond, mtx)', '&cond->cond, (pthread_mutex_t *)mtx)')
    if text != updated: monotonic.write_text(updated)
    standalone = source / 'src/compiler/glsl/standalone.cpp'
    text = standalone.read_text()
    # The shared NIR backend implements demotion in software on A5xx. Enable
    # only the language extension here, without advertising a GPU capability.
    if 'ctx->Extensions.EXT_demote_to_helper_invocation = true;' not in text:
        needle = 'ctx->Extensions.ARB_ES3_2_compatibility = true;'
        if needle not in text: raise RuntimeError('Mesa standalone extension initialization changed')
        text = text.replace(needle, needle+'\n   ctx->Extensions.EXT_demote_to_helper_invocation = true;',1)
    # Preserve shadow-array explicit LOD as txl, rather than approximating it
    # with gradients. This enables a private compiler bridge language feature;
    # it does not advertise OpenGL or Metal capabilities on the target.
    if 'ctx->Extensions.EXT_texture_shadow_lod = true;' not in text:
        needle = 'ctx->Extensions.ARB_ES3_2_compatibility = true;'
        if needle not in text: raise RuntimeError('Mesa standalone shadow-LOD initialization changed')
        text = text.replace(needle, needle+'\n   ctx->Extensions.EXT_texture_shadow_lod = true;',1)
    # Explicit binary16 values in translated AIR use Mesa's existing typed
    # arithmetic. This is a private compiler language, not a device API claim.
    if 'ctx->Extensions.AMD_gpu_shader_half_float = true;' not in text:
        needle = 'ctx->Extensions.ARB_ES3_2_compatibility = true;'
        if needle not in text: raise RuntimeError('Mesa standalone half initialization changed')
        text = text.replace(needle, needle+'\n   ctx->Extensions.AMD_gpu_shader_half_float = true;',1)
    # Match IR3's scalar derivative requirement while GLSL constructs NIR.
    for name in ('nir_vs_options','nir_fs_options'):
        text = text.replace(name+' = { 0 };',name+' = { .scalarize_ddx = true };')
    needle = 'ctx->Const.ShaderCompilerOptions[MESA_SHADER_FRAGMENT].NirOptions =\n      &nir_vs_options;'
    addition = '\n   ctx->Const.ShaderCompilerOptions[MESA_SHADER_COMPUTE].NirOptions = &nir_vs_options;'
    if addition not in text:
        if needle not in text: raise RuntimeError('Mesa standalone NIR initialization changed')
        text = text.replace(needle,needle+addition,1)
    limits = '\n   ctx->Const.MaxCombinedShaderStorageBlocks = 24;\n   for (unsigned stage = 0; stage < MESA_SHADER_STAGES; ++stage)\n      ctx->Const.Program[stage].MaxShaderStorageBlocks = 8;'
    if limits not in text:
        text = text.replace(addition, addition+limits, 1)
    sample_limits = '\n   ctx->Const.MaxCombinedTextureImageUnits = 384;\n   ctx->Const.Program[MESA_SHADER_COMPUTE].MaxTextureImageUnits = 32;\n   ctx->Const.Program[MESA_SHADER_VERTEX].MaxTextureImageUnits = 32;\n   ctx->Const.Program[MESA_SHADER_FRAGMENT].MaxTextureImageUnits = 32;'
    text = text.replace('ctx->Const.MaxCombinedTextureImageUnits = 64;', 'ctx->Const.MaxCombinedTextureImageUnits = 384;')
    text = text.replace('ctx->Const.MaxCombinedTextureImageUnits = 128;', 'ctx->Const.MaxCombinedTextureImageUnits = 384;')
    text = text.replace('ctx->Const.MaxCombinedTextureImageUnits = 192;', 'ctx->Const.MaxCombinedTextureImageUnits = 384;')
    if sample_limits not in text:
        # Up to 32 active combined uniforms are split into independent A540
        # texture/sampler slots; physical sampler occupancy is checked later.
        text = text.replace('   ctx->Const.GenerateTemporaryNames = true;',
            '   ctx->Const.GenerateTemporaryNames = true;'+sample_limits,1)
    if standalone.read_text() != text: standalone.write_text(text)
    text = standalone.read_text().replace('ctx->Const.MaxShaderStorageBufferBindings = 4;',
        'ctx->Const.MaxShaderStorageBufferBindings = 8;').replace('ctx->Const.MaxShaderStorageBlockSize = 4096;',
        'ctx->Const.MaxShaderStorageBlockSize = 4194304;')
    text=text.replace('ctx->Const.MaxShaderStorageBufferBindings = 8;','ctx->Const.MaxShaderStorageBufferBindings = 16;')
    if 'ctx->Const.Program[MESA_SHADER_COMPUTE].MaxShaderStorageBlocks = 16;' not in text:
        text=text.replace(limits,limits+'\n   ctx->Const.Program[MESA_SHADER_COMPUTE].MaxShaderStorageBlocks = 16;')
    text = text.replace('ctx->Const.MaxComputeSharedMemorySize = 65536;',
                        'ctx->Const.MaxComputeSharedMemorySize = 32768;')
    if standalone.read_text() != text: standalone.write_text(text)
    # Preserve upstream text and derive these small compiler adaptations on
    # every build. GLSL's discard-loop escape rule does not apply to demote:
    # demoted Metal lanes must keep computing derivatives after the call.
    discard_flow = source / 'src/compiler/glsl/gl_nir_lower_discard_flow.c'
    discard_pristine = root / 'gl_nir_lower_discard_flow.original.c'
    if not discard_pristine.exists(): shutil.copyfile(discard_flow, discard_pristine)
    text = discard_pristine.read_text()
    needle = '''intrin->intrinsic == nir_intrinsic_terminate ||
                intrin->intrinsic == nir_intrinsic_demote_if ||
                intrin->intrinsic == nir_intrinsic_demote)'''
    if needle not in text: raise RuntimeError('Mesa discard-flow lowering changed')
    text = text.replace(needle, 'intrin->intrinsic == nir_intrinsic_terminate)', 1)
    if discard_flow.read_text() != text: discard_flow.write_text(text)
    # The deferred KILL must remain after explicit derivatives and implicit
    # texture derivatives. False dependencies survive both IR3 schedulers;
    # this emits no extra hardware barrier and affects only discarding FSs.
    ir3_nir = source / 'src/freedreno/ir3/ir3_compiler_nir.c'
    ir3_pristine = root / 'ir3_compiler_nir.original.c'
    if not ir3_pristine.exists(): shutil.copyfile(ir3_nir, ir3_pristine)
    text = ir3_pristine.read_text()
    dependency = '''   if (ctx->compiler->gen == 5 && ctx->so->type == MESA_SHADER_FRAGMENT && ctx->s->info.fs.uses_discard) {
      %s->barrier_class |= IR3_BARRIER_ACTIVE_FIBERS_R;
      %s->barrier_conflict |= IR3_BARRIER_ACTIVE_FIBERS_W;
   }
'''
    if text.count('   return sam;') != 1: raise RuntimeError('Mesa texture emission changed')
    text = text.replace('   return sam;', (dependency % ('sam', 'sam'))+'   return sam;', 1)
    for op in ('DSX', 'DSXPP_MACRO', 'DSY', 'DSYPP_MACRO'):
        needle = f'      dst[0] = ir3_{op}(b, src, 0);\n      dst[0]->cat5.type = TYPE_F32;'
        if text.count(needle) != 1: raise RuntimeError('Mesa derivative emission changed')
        text = text.replace(needle, needle+'\n'+(dependency % ('dst[0]', 'dst[0]')), 1)
    # Mesa MR 44343 (69f8c3fd): A5xx needs a live per-sample input even
    # when only sample-qualified varyings request sample-rate execution.
    needle = '      ctx->ij[IJ_PERSP_PIXEL] = create_input(ctx, 0x3);'
    if text.count(needle) != 1: raise RuntimeError('Mesa fragment input setup changed')
    text = text.replace(needle, needle+'''
      if (ctx->compiler->gen == 5 && ctx->s->info.fs.uses_sample_shading) {
         ctx->samp_id = create_sysval_input(ctx, SYSTEM_VALUE_SAMPLE_ID, 0x1);
         ctx->samp_id->dsts[0]->flags |= IR3_REG_HALF;
         array_insert(ctx->samp_id->block, ctx->samp_id->block->keeps, ctx->samp_id);
      }''', 1)
    if ir3_nir.read_text() != text: ir3_nir.write_text(text)
    # A5xx's untyped buffer opcodes carry an element type. Upstream's a4xx
    # emitter assumes 32-bit SSBOs; AIR half loads/stores need 16-bit registers
    # and the matching TYPE_U16 encoding, including their halfword offsets.
    a4xx = source / 'src/freedreno/ir3/ir3_a4xx.c'
    a4xx_pristine = root / 'ir3_a4xx.original.c'
    if not a4xx_pristine.exists(): shutil.copyfile(a4xx, a4xx_pristine)
    text = a4xx_pristine.read_text()
    needle = '   ldgb->cat6.type = TYPE_U32;'
    if text.count(needle) != 1: raise RuntimeError('Mesa A5xx buffer load emission changed')
    text = text.replace(needle, '''   bool half = ctx->compiler->gen == 5 && intr->def.bit_size == 16;
   ldgb->cat6.type = half ? TYPE_U16 : TYPE_U32;
   if (half) ldgb->dsts[0]->flags |= IR3_REG_HALF;''', 1)
    needle = '   stgb->cat6.type = TYPE_U32;'
    if text.count(needle) != 1: raise RuntimeError('Mesa A5xx buffer store emission changed')
    text = text.replace(needle, '''   stgb->cat6.type = ctx->compiler->gen == 5 &&
      nir_src_bit_size(intr->src[0]) == 16 ? TYPE_U16 : TYPE_U32;''', 1)
    if a4xx.read_text() != text: a4xx.write_text(text)
    # Targeted register definitions from the same upstream fixes. Keep the
    # pinned register database; do not silently switch the compiler revision.
    a5xx = source / 'src/freedreno/registers/adreno/a5xx.xml'
    a5xx_pristine = root / 'a5xx.original.xml'
    if not a5xx_pristine.exists(): shutil.copyfile(a5xx, a5xx_pristine)
    text = a5xx_pristine.read_text()
    needle = '\t\t<bitfield name="SAMPLEID" pos="2" type="boolean"/>'
    if text.count(needle) != 1: raise RuntimeError('A5xx sample control definitions changed')
    text = text.replace(needle, needle+'\n\t\t<bitfield name="SAMPLEMODE" low="3" high="4" type="uint"/>\n\t\t<bitfield name="CENTERRHW" pos="5" type="boolean"/>', 1)
    needle = '\t\t<bitfield name="FRAG_WRITES_Z" pos="5" type="boolean"/>'
    if text.count(needle) != 1: raise RuntimeError('A5xx fragment output definitions changed')
    text = text.replace(needle, needle+'\n\t\t<bitfield name="FRAG_WRITES_SAMPMASK" pos="6" type="boolean"/>', 1)
    if a5xx.read_text() != text: a5xx.write_text(text)
    linker = source / 'src/compiler/glsl/gl_nir_linker.c'
    text = linker.read_text()
    if 'qgpu_dynamic_pool' not in text:
        needle = '   if (prog->nir->info.stage == MESA_SHADER_COMPUTE) {\n      NIR_PASS(_, prog->nir, nir_lower_vars_to_explicit_types,'
        if needle not in text: raise RuntimeError('Mesa shared-memory lowering changed')
        # Preserve dynamic pool derefs until Interface.h assigns its aligned
        # base after static objects and trims the reservation to 32 KiB.
        text = text.replace(needle,'''   bool qgpu_dynamic_pool = false;
   nir_foreach_variable_with_modes(var, prog->nir, nir_var_mem_shared)
      if (var->name && !strcmp(var->name, "_qdynamic")) qgpu_dynamic_pool = true;
   if (prog->nir->info.stage == MESA_SHADER_COMPUTE && !qgpu_dynamic_pool) {
      NIR_PASS(_, prog->nir, nir_lower_vars_to_explicit_types,''',1)
        linker.write_text(text)
    venv = root / 'venv'
    if not (venv / 'bin/meson').exists(): _component_native.run([sys.executable, '-m', 'venv', venv])
    if not (venv / '.gpu-build-deps-v1').exists():
        _component_native.run([venv / 'bin/pip', 'install', 'meson==1.7.0', 'Mako==1.3.9', 'PyYAML==6.0.2', 'packaging==25.0'])
        (venv / '.gpu-build-deps-v1').touch()
    # Preserve the upstream compiler and add only an allocation/code exporter.
    original = source / 'src/gallium/drivers/freedreno/ir3/ir3_cmdline.c'
    pristine = root / 'ir3_cmdline.original.c'
    if not pristine.exists(): shutil.copyfile(original, pristine)
    body = pristine.read_text()
    body = body.replace('struct ir3_shader_key key = {};',
        'struct ir3_shader_key key = {.msaa = getenv("QCOM_IR3_MSAA") != NULL};', 1)
    begin = body.index('static void\nfixup_varying_slots(')
    finish = body.index('static struct ir3_compiler *compiler;',begin)
    body = body[:begin] + body[finish:]
    for mode in ['nir_var_shader_out','nir_var_shader_in']:
        body = body.replace('      fixup_varying_slots(nir, '+mode+');\n','')
    body = body.replace('.glsl_version = 310,', '.glsl_version = 430,')
    body = body.replace('nir_shader *nir = prog->_LinkedShaders[stage]->Program->nir;', '''nir_shader *nir = prog->_LinkedShaders[stage]->Program->nir;
   nir->options = nir_options;
   if (!qgpu_capture_interface(nir, &qcom_interface)) errx(1, "unsupported stage interface");
   if (stage == MESA_SHADER_VERTEX) {
      nir_foreach_variable_with_modes(var, nir, nir_var_system_value)
         if (var->data.location == SYSTEM_VALUE_INSTANCE_ID) var->data.location = SYSTEM_VALUE_INSTANCE_INDEX;
   }
   if (stage == MESA_SHADER_FRAGMENT) {
      nir_foreach_variable_with_modes(var, nir, nir_var_shader_in) {
         if (var->data.location == VARYING_SLOT_POS) {
            var->data.mode = nir_var_system_value;
            var->data.location = SYSTEM_VALUE_FRAG_COORD;
         }
      }
      nir_fixup_deref_modes(nir);
   }''', 1)
    io_locations = '   nir_assign_var_locations(nir, nir_var_uniform, &nir->num_uniforms,'
    if io_locations not in body: raise RuntimeError('Mesa uniform location assignment changed')
    body = body.replace(io_locations, '   qgpu_assign_graphics_io(nir);\n'+io_locations, 1)
    body = body.replace('NIR_PASS_V(nir, nir_lower_compute_system_values, NULL);',
        'const nir_lower_compute_system_values_options cs_options = {};\n   NIR_PASS_V(nir, nir_lower_compute_system_values, &cs_options);')
    body = body.replace('   ir3_finalize_nir(compiler, &options, nir);',
        '   qgpu_center_fragment_position(nir, key.msaa);\n   ir3_finalize_nir(compiler, &options, nir);', 1)
    body = body.replace('if (!prog)\n', 'if (!prog || !prog->data->LinkStatus || !prog->_LinkedShaders[stage] || !prog->_LinkedShaders[stage]->Program)\n', 1)
    body = body.replace('static void\ndump_info', '#include "QcomExport.h"\n\nstatic void\ndump_info', 1)
    body = body.replace('NIR_PASS_V(nir, gl_nir_lower_buffers, prog);',
        'NIR_PASS_V(nir, gl_nir_lower_buffers, prog);\n   qcom_remap_ssbos(nir, prog->_LinkedShaders[stage]->Program);', 1)
    body = body.replace('NIR_PASS_V(nir, gl_nir_lower_samplers, prog);', '''if (stage == MESA_SHADER_COMPUTE || stage == MESA_SHADER_FRAGMENT || stage == MESA_SHADER_VERTEX) {
      NIR_PASS_V(nir, gl_nir_lower_samplers_as_deref, prog);
      if (!qgpu_bind_samples(nir, &qcom_interface)) errx(1, "invalid compute sample bindings");
      NIR_PASS_V(nir, nir_lower_samplers);
   } else NIR_PASS_V(nir, gl_nir_lower_samplers, prog);
   if (!qgpu_remap_samples(nir, &qcom_interface)) errx(1, "unsupported compute sampling");''', 1)
    first = body.index('   struct ir3_shader *shader = rzalloc_size(NULL, sizeof(*shader));')
    last = body.index('   dump_info(v, info);', first)
    body = body[:first] + '''   struct ir3_shader_options shader_options = {.num_reserved_user_consts = qgpu_memory_constants(&qcom_interface) / 4};
   struct ir3_shader *shader = ir3_shader_from_nir(compiler, nir, &shader_options, NULL);
   struct ir3_shader_variant *v = ir3_shader_create_variant(shader, &key, true);
   if (!v) errx(1, "IR3 compilation failed");
   info = "NIR compiler";
''' + body[last:]
    body = body.replace('printf("; %s: %s\\n", type, str);', 'qcom_export_program(so, bin);\n   printf("; %s: %s\\n", type, str);', 1)
    body = body.replace('bin = ir3_shader_assemble(so);', 'bin = so->bin;', 1)
    body = body.replace('ir3_shader_disasm(so, bin, stdout);',
        'if (so->disasm_info.disasm) fputs(so->disasm_info.disasm, stdout);', 1)
    if body == pristine.read_text(): raise RuntimeError('Mesa compiler exporter insertion failed')
    if original.read_text() != body: original.write_text(body)
    shutil.copyfile(build_state.TOOLS / 'gpu-compiler/Export.h', original.parent / 'QcomExport.h')
    shutil.copyfile(build_state.TOOLS / 'gpu-compiler/Interface.h', original.parent / 'QcomInterface.h')
    for name in ['GPUJITProtocol.h','GPUShaderInterface.h']:
        shutil.copyfile(build_state.DRIVERS_ROOT / 'include' / name, original.parent / name)
    obj = root / 'host'
    environment = dict(os.environ)
    environment['PATH'] = str(venv / 'bin') + os.pathsep + environment.get('PATH','')
    if not (obj / 'build.ninja').exists():
        _component_native.run([venv / 'bin/meson', 'setup', obj, source, '--buildtype=release',
             '-Dgallium-drivers=freedreno', '-Dfreedreno-kmds=msm', '-Dtools=freedreno',
             '-Dplatforms=', '-Dvulkan-drivers=', '-Dllvm=disabled', '-Dglx=disabled',
             '-Degl=disabled', '-Dgles1=disabled', '-Dgles2=disabled', '-Dgbm=disabled',
             '-Dglvnd=disabled', '-Dgallium-va=disabled', '-Dgallium-vdpau=disabled',
             '-Dgallium-xa=disabled', '-Dbuild-tests=false', '-Dlibunwind=disabled',
             '-Dshader-cache=disabled', '-Dvalgrind=disabled'], env=environment)
    _component_native.run([_component_native.host('ninja'), '-C', obj, '-j', str(build_state.JOBS), 'src/gallium/drivers/freedreno/ir3_compiler'], env=environment)
    return obj / 'src/gallium/drivers/freedreno/ir3_compiler', source


def prepare_gpu_air_compiler():
    """Build a host-only LLVM reader/lowerer; never link LLVM into the Kext."""
    from project.build import native as _component_native
    source = build_state.TOOLS / 'gpu-compiler/AIR.cpp'
    root = build_state.BUILD / 'obj/gpu-compiler'; root.mkdir(parents=True, exist_ok=True)
    config = _component_native.host('llvm-config')
    version = subprocess.check_output([config, '--version'], text=True).strip()
    if version.split('.')[0] != '23':
        raise RuntimeError('AIR import currently requires LLVM 23 development headers/libraries and llvm-as')
    flags = shlex.split(subprocess.check_output([config, '--cxxflags', '--ldflags', '--libs',
                                               'core', 'irreader', 'bitreader', 'analysis'], text=True))
    inputs = {'source_sha256':_component_native.digest(source), 'semantic_sha256':_component_native.digest(build_state.TOOLS/'gpu-compiler/Semantic.h'),
              'control_flow_sha256':_component_native.digest(build_state.TOOLS/'gpu-compiler/AIRControlFlow.h'),
              'library_archive_sha256':_component_native.digest(build_state.TOOLS/'gpu-compiler/LibraryArchive.h'),
              'function_constants_sha256':_component_native.digest(build_state.TOOLS/'gpu-compiler/FunctionConstants.h'),
              'air_specialization_sha256':_component_native.digest(build_state.TOOLS/'gpu-compiler/AIRSpecialization.h'),
              'air_indirect_calls_sha256':_component_native.digest(build_state.TOOLS/'gpu-compiler/AIRIndirectCalls.h'),
              'air_simd_sha256':_component_native.digest(build_state.TOOLS/'gpu-compiler/AIRSIMD.h'),
              'interface_sha256':_component_native.digest(build_state.DRIVERS_ROOT/'include/GPUShaderInterface.h'),
              'protocol_sha256':_component_native.digest(build_state.DRIVERS_ROOT/'include/GPUShaderProtocol.h'),
              'user_protocol_sha256':_component_native.digest(build_state.DRIVERS_ROOT/'include/GPUUserProtocol.h'), 'llvm_version':version, 'flags':flags}
    exe = root / 'air-frontend'; stamp = root / 'air-frontend.json'
    if not (exe.exists() and stamp.exists() and json.loads(stamp.read_text()).get('inputs') == inputs and
            json.loads(stamp.read_text()).get('sha256') == _component_native.digest(exe)):
        log = root / 'air-build.log'; log.unlink(missing_ok=True)
        temporary = root / 'air-frontend.tmp'
        _component_native.run([_component_native.host('clang++'), source, *flags, '-std=c++17', '-fexceptions', '-Wall', '-Wextra', '-Werror',
             '-Wno-unused-parameter', '-I'+str(build_state.DRIVERS_ROOT/'include'), '-O2', '-o', temporary], log=log)
        temporary.replace(exe)
        stamp.write_text(json.dumps({'inputs':inputs, 'sha256':_component_native.digest(exe)},indent=2)+'\n')
    return exe, version


def prepare_gpu_jit_compiler(mesa):
    """In-process iOS shader compiler; share the pinned AOT Mesa checkout."""
    from project.build import native as _component_native
    patch_gpu_register_delays(mesa)
    root = build_state.BUILD / 'obj/gpu-compiler'; venv = root / 'venv'
    sdk = build_state.ROOT / 'tools/sdk/iPhoneOS18.6.sdk'
    # The AIR builder uses LLVM's exported C ABI inside the compiler dylib.
    # Target headers must never put host libc headers on an iOS include path.
    llvm_include = Path(subprocess.check_output([_component_native.host('llvm-config'),'--includedir'],text=True).strip())
    llvm_headers = root/'include'
    shutil.copytree(llvm_include/'llvm-c',llvm_headers/'llvm-c',dirs_exist_ok=True)
    shutil.copytree(llvm_include/'llvm/Config',llvm_headers/'llvm/Config',dirs_exist_ok=True)
    options = mesa / 'src/freedreno/ir3/ir3_compiler.h'
    text = options.read_text().replace('struct ir3_compiler_options {',
        'struct ir3_compiler_options {\n   bool qcom_runtime; // No environment shader overrides in the trusted memory API.',1)
    if 'bool qcom_runtime;' not in options.read_text(): options.write_text(text)
    compiler_source = mesa / 'src/freedreno/ir3/ir3_compiler.c'
    text = compiler_source.read_text().replace('ir3_shader_debug = debug_get_option_ir3_shader_debug();',
        'ir3_shader_debug = options->qcom_runtime ? 0 : debug_get_option_ir3_shader_debug();').replace(
        '__normal_user() ? debug_get_option_ir3_shader_override_path() : NULL;',
        '(!options->qcom_runtime && __normal_user()) ? debug_get_option_ir3_shader_override_path() : NULL;')
    if compiler_source.read_text() != text: compiler_source.write_text(text)
    nir_source=mesa/'src/freedreno/ir3/ir3_nir.c'
    text=nir_source.read_text()
    # The memory compiler exports code/immediates, not an auxiliary constant
    # buffer. Keep finite local function tables in registers; don't introduce
    # a hidden UBO after our resource validation has already run.
    if 'so->compiler->options.qcom_runtime ? UINT32_MAX' not in text:
        text=text.replace('32 /* bytes */);','so->compiler->options.qcom_runtime ? UINT32_MAX : 32 /* bytes */);')
    if nir_source.read_text()!=text:nir_source.write_text(text)
    cache = mesa / 'src/freedreno/ir3/ir3_disk_cache.c'
    text = cache.read_text()
    if '#ifdef ENABLE_SHADER_CACHE\n   if (ir3_shader_debug' not in text:
        text = text.replace('   if (ir3_shader_debug & IR3_DBG_NOCACHE)',
                            '#ifdef ENABLE_SHADER_CACHE\n   if (ir3_shader_debug & IR3_DBG_NOCACHE)',1)
        text = text.replace('   compiler->disk_cache = disk_cache_create(renderer, timestamp, driver_flags);',
                            '   compiler->disk_cache = disk_cache_create(renderer, timestamp, driver_flags);\n#else\n   (void)compiler;\n#endif',1)
        cache.write_text(text)
    standalone = mesa / 'src/compiler/glsl/standalone.cpp'
    text = standalone.read_text()
    if 'ctx->Extensions.KHR_shader_subgroup = true;' not in text:
        needle = 'ctx->Extensions.ARB_ES3_2_compatibility = true;'
        if needle not in text: raise RuntimeError('Mesa standalone subgroup initialization changed')
        text = text.replace(needle,needle+'''
   ctx->Extensions.KHR_shader_subgroup = true;
   ctx->Const.ShaderSubgroupSize = 64;
   ctx->Const.ShaderSubgroupSupportedStages = GL_COMPUTE_SHADER_BIT;
   ctx->Const.ShaderSubgroupSupportedFeatures = GL_SUBGROUP_FEATURE_BASIC_BIT_KHR |
      GL_SUBGROUP_FEATURE_ARITHMETIC_BIT_KHR | GL_SUBGROUP_FEATURE_BALLOT_BIT_KHR |
      GL_SUBGROUP_FEATURE_SHUFFLE_BIT_KHR | GL_SUBGROUP_FEATURE_VOTE_BIT_KHR;
''',1)
    if 'qgpu_standalone_source' not in text:
        signature = 'standalone_compile_shader(const struct standalone_options *_options,\n      unsigned num_files, char* const* files, struct gl_context *ctx)'
        replacement = 'qgpu_standalone_inputs(const struct standalone_options *_options,\n      unsigned num_files, char* const* files, const char *const *sources,\n      struct gl_context *ctx, char *error, unsigned error_bytes)'
        if signature not in text: raise RuntimeError('Mesa standalone compiler signature changed')
        text = text.replace('extern "C" struct gl_shader_program *\n'+signature,
                            'static struct gl_shader_program *\n'+replacement,1)
        text = text.replace('const char *source = load_text_file(whole_program, files[i]);',
                            'const char *source = sources ? sources[i] : load_text_file(whole_program, files[i]);',1)
        text = text.replace('if (strlen(shader->InfoLog) > 0) {',
                            'if (error && strlen(shader->InfoLog)) snprintf(error, error_bytes, "%s", shader->InfoLog);\n      if (!error && strlen(shader->InfoLog) > 0) {',1)
        text = text.replace('if (strlen(whole_program->data->InfoLog) > 0) {',
                            'if (error && strlen(whole_program->data->InfoLog)) snprintf(error, error_bytes, "%s", whole_program->data->InfoLog);\n      if (!error && strlen(whole_program->data->InfoLog) > 0) {',1)
        text += '''
extern "C" struct gl_shader_program *
standalone_compile_shader(const struct standalone_options *options,
      unsigned count, char *const *files, struct gl_context *ctx) {
   return qgpu_standalone_inputs(options, count, files, NULL, ctx, NULL, 0);
}
extern "C" struct gl_shader_program *
qgpu_standalone_source(const char *source, unsigned stage, struct gl_context *ctx,
      char *error, unsigned error_bytes) {
   static const struct standalone_options opts = {430, 0, 0, 0, 1, 1, 0};
   char *files[] = {const_cast<char *>(stage == 0 ? "memory.comp" : stage == 1 ? "memory.vert" : "memory.frag")};
   const char *sources[] = {source};
   return qgpu_standalone_inputs(&opts, 1, files, sources, ctx, error, error_bytes);
}
'''
        if replacement not in text: raise RuntimeError('Mesa memory compiler insertion failed')
        standalone.write_text(text)
    if standalone.read_text()!=text:standalone.write_text(text)
    compiler_meson = mesa / 'src/compiler/meson.build'
    text = compiler_meson.read_text().replace("if with_gallium\n  subdir('glsl')", "if with_gallium or with_tools.contains('freedreno')\n  subdir('glsl')")
    if compiler_meson.read_text() != text: compiler_meson.write_text(text)
    for name in ['Backend.c','AIRNIR.cpp']:
        shutil.copyfile(build_state.TOOLS / 'gpu-compiler' / name, mesa / 'src/freedreno' / ('Qcom' + name))
    for name in ['AIRRuntime.cpp','AIRNIR.h','AIRNIRLowering.h','AIRSpecialization.h','AIRIndirectCalls.h','AIRSIMD.h','AIRMatrixNIR.h','AIRVisibleFunctions.h','AIRControlFlow.h','FunctionConstants.h','Semantic.h']:
        shutil.copyfile(build_state.TOOLS/'gpu-compiler'/name,mesa/'src/freedreno'/name)
    shutil.copyfile(build_state.TOOLS / 'gpu-compiler/Interface.h', mesa / 'src/freedreno/QcomInterface.h')
    # Compiler.h includes the shared kernel wire description, not an IOKit ABI.
    shutil.copyfile(build_state.TOOLS / 'gpu-compiler/Compiler.h', mesa / 'src/freedreno/Compiler.h')
    for name in ['GPUJITProtocol.h','GPUShaderInterface.h']:
        shutil.copyfile(build_state.DRIVERS_ROOT / 'include' / name, mesa / 'src/freedreno' / name)
    meson_file = mesa / 'src/freedreno/meson.build'
    text = meson_file.read_text()
    marker = '\n# Project adapter: only compiler libraries'
    text = text.split(marker)[0]
    text += '''
# Project adapter: only compiler libraries; no Gallium device/DRM backend.
qcom_llvm = host_machine.system() == 'darwin' ? cpp.find_library('LLVM') : cpp.find_library('LLVM-23')
qcom_gpu_compiler = shared_library('QcomGPUCompiler', ['QcomBackend.c', 'QcomAIRNIR.cpp'],
  name_prefix: '', gnu_symbol_visibility: 'hidden',
  cpp_args: ['-fexceptions', '-Wno-deprecated-declarations'],
  link_args: host_machine.system() == 'darwin' ? ['-Wl,-install_name,@loader_path/QcomGPUCompiler.dylib', '-Wl,-adhoc_codesign'] : [],
  include_directories: [inc_freedreno, inc_include, inc_src, inc_mesa, inc_mapi, inc_gallium, inc_gallium_aux,
    include_directories('@QCOM_LLVM_HEADERS@'), include_directories('@QCOM_PROTOCOL_HEADERS@')],
  dependencies: [idep_nir, idep_mesautil, qcom_llvm],
  link_with: [libfreedreno_ir3, libglsl_standalone],
  build_by_default: false)
'''.replace('@QCOM_LLVM_HEADERS@',str(llvm_headers)).replace('@QCOM_PROTOCOL_HEADERS@',str(build_state.DRIVERS_ROOT/'include'))
    if meson_file.read_text() != text: meson_file.write_text(text)
    cross = root / 'ios.ini'
    # Direct Clang invocation keeps Meson probes and final objects on the same
    # target and SDK; build generators still use the native C/C++ compilers.
    c = [_component_native.compiler(), '--target=arm64-apple-ios18.6', '-isysroot', str(sdk), *_component_native.compiler_resource()]
    cpp = [*c, '--driver-mode=g++', '-stdlib=libc++']
    content = "[binaries]\nc = " + repr(c) + "\ncpp = " + repr(cpp) + "\nobjc = " + repr(c) + "\nar = '" + _component_native.host('llvm-ar') + "'\nstrip = '" + _component_native.host('llvm-strip') + "'\n"
    content += "c_ld = '" + _component_native.host('ld64.lld') + "'\ncpp_ld = '" + _component_native.host('ld64.lld') + "'\npkg-config = 'false'\n[host_machine]\nsystem = 'darwin'\ncpu_family = 'aarch64'\ncpu = 'arm64'\nendian = 'little'\n[properties]\nneeds_exe_wrapper = true\n"
    content += "[built-in options]\nc_args = ['-mcpu=generic', '-march=armv8-a+crc+crypto']\ncpp_args = ['-mcpu=generic', '-march=armv8-a+crc+crypto']\nobjc_link_args = ['-fuse-ld="+_component_native.host('ld64.lld')+"', '-Wl,-platform_version,ios,18.6,18.6']\nc_link_args = ['-fuse-ld="+_component_native.host('ld64.lld')+"', '-Wl,-platform_version,ios,18.6,18.6']\ncpp_link_args = ['-fuse-ld="+_component_native.host('ld64.lld')+"', '-Wl,-platform_version,ios,18.6,18.6']\n"
    if not cross.exists() or cross.read_text()!=content: cross.write_text(content)
    obj = root / 'ios'; environment = dict(os.environ)
    environment['PATH'] = str(venv / 'bin') + os.pathsep + environment.get('PATH','')
    log = root / 'jit-build.log'
    if not (obj / 'build.ninja').exists():
        _component_native.run([venv / 'bin/meson', 'setup', obj, mesa, '--cross-file', cross, '--buildtype=release', '--wrap-mode=nofallback',
             '-Dgallium-drivers=', '-Dfreedreno-kmds=', '-Dtools=freedreno',
             '-Dplatforms=', '-Dvulkan-drivers=', '-Dllvm=disabled', '-Dglx=disabled',
             '-Degl=disabled', '-Dgles1=disabled', '-Dgles2=disabled', '-Dgbm=disabled',
             '-Dglvnd=disabled', '-Dgallium-va=disabled', '-Dgallium-vdpau=disabled',
             '-Dgallium-xa=disabled', '-Dbuild-tests=false', '-Dlibunwind=disabled',
             '-Dshader-cache=disabled', '-Dvalgrind=disabled', '-Dzstd=disabled', '-Dexpat=disabled'],
             env=environment, log=log)
    # Meson caches machine-file built-in options at setup. Apply this profile
    # explicitly when upgrading an existing compiler build directory too.
    profile = ['-mcpu=generic','-march=armv8-a+crc+crypto']
    build_options = json.loads(subprocess.check_output([venv/'bin/meson','introspect','--buildoptions',obj],text=True))
    current = {item['name']:item['value'] for item in build_options}
    if current.get('c_args') != profile or current.get('cpp_args') != profile:
        _component_native.run([venv/'bin/meson','configure',obj,'-Dc_args='+ ' '.join(profile),'-Dcpp_args='+ ' '.join(profile)],env=environment,log=log)
    _component_native.run([_component_native.host('ninja'), '-C', obj, '-j', str(build_state.JOBS), 'src/freedreno/QcomGPUCompiler.dylib'], env=environment, log=log)
    # The offline native probes use the same adapter as the iOS compiler.
    # Refresh its host library here too, after synchronizing the shared C
    # sources/headers, so callers cannot accidentally validate a stale backend.
    host_obj=root/'host'
    if (host_obj/'build.ninja').exists():
        _component_native.run([_component_native.host('ninja'),'-C',host_obj,'-j',str(build_state.JOBS),'src/freedreno/QcomGPUCompiler.so'],env=environment,log=root/'jit-host-build.log')
    # The target now contains Mesa code. Consolidate source/header notices
    # from the actual compiler build alongside the adapter's attribution.
    paths = set()
    for item in json.loads((obj/'compile_commands.json').read_text()):
        if (obj/item['output']).exists(): paths.add((obj/item['file']).resolve())
    dependencies = subprocess.check_output([_component_native.host('ninja'),'-C',obj,'-t','deps'],text=True)
    for line in dependencies.splitlines():
        if line.startswith('    '): paths.add((obj/line.strip()).resolve())
    notices = {}
    for path in sorted(paths):
        if not path.is_file() or not path.is_relative_to(mesa): continue
        with path.open(errors='replace') as stream: prefix = stream.read(16384)
        header = re.match(r'\A((?:\s*(?:/\*[\s\S]*?\*/|//[^\n]*(?:\n|$)))+)',prefix)
        if header and re.search(r'copyright|SPDX-License|permission|license',header[1],re.I):
            notices.setdefault(header[1].strip(),[]).append(str(path.relative_to(mesa)))
    notice = (build_state.TOOLS/'gpu-compiler/NOTICE.txt').read_text()
    notice += '\nConsolidated notices from the pinned Mesa compiler build:\n'
    for header, names in notices.items(): notice += '\n'+', '.join(names)+'\n'+header+'\n'
    (root/'NOTICE.Mesa.txt').write_text(notice)
    libraries = [obj / 'src/freedreno/QcomGPUCompiler.dylib']
    llvm_exports = (sdk/'usr/lib/libLLVM.tbd').read_text()
    llvm_symbols = sorted(set(re.findall(r'\b(_LLVM\w+)\b',subprocess.check_output(
        [_component_native.host('llvm-nm'),'--undefined-only',libraries[0]],text=True))))
    for symbol in llvm_symbols:
        if not re.search(r'\b'+re.escape(symbol)+r'\b',llvm_exports):
            raise RuntimeError('iOS LLVM lacks direct AIR compiler C export '+symbol)
    report = {'target':'arm64-apple-ios18.6','cpu_isa':'armv8-a+crc+crypto','mesa_commit':json.loads((build_state.TOOLS/'gpu-compiler/Source.json').read_text())['commit'],
              'runtime_llvm_c_exports':llvm_symbols,
              'hardware_tested':False, 'libraries':{str(p.relative_to(build_state.ROOT)):_component_native.digest(p) for p in libraries},
              'sources':{str(p.relative_to(build_state.ROOT)):_component_native.digest(p) for p in [build_state.SELF,build_state.TOOLS/'gpu-compiler/Source.json',
                  build_state.TOOLS/'gpu-compiler/Backend.c',build_state.TOOLS/'gpu-compiler/Interface.h',build_state.TOOLS/'gpu-compiler/Compiler.h',
                  build_state.TOOLS/'gpu-compiler/AIRNIR.cpp',build_state.TOOLS/'gpu-compiler/AIRNIR.h',build_state.TOOLS/'gpu-compiler/AIRNIRLowering.h',
                  build_state.DRIVERS_ROOT/'include/GPUJITProtocol.h',build_state.DRIVERS_ROOT/'include/GPUShaderInterface.h',
                  standalone,mesa/'src/compiler/glsl/gl_nir_linker.c',mesa/'src/freedreno/ir3/ir3_a4xx.c',compiler_source,nir_source,options,cache,compiler_meson,meson_file]}}
    (root/'jit-compiler.json').write_text(json.dumps(report,indent=2)+'\n')
    return libraries


def extract_gpu_metallib(data):
    """Use the same bounded FAT/MTLB parser as Metal's in-process loader."""
    if not data or len(data) > 64*1024*1024: raise RuntimeError('invalid or oversized metallib header')
    compiler, _ = prepare_gpu_air_compiler()
    result = subprocess.run([compiler, '--library-index'], input=data, capture_output=True)
    if result.returncode: raise RuntimeError(result.stderr.decode(errors='replace').strip())
    records = json.loads(result.stdout)
    for record in records:
        record['hash'] = bytes.fromhex(record['hash'])
        record['data'] = data[record['offset']:record['offset']+record['size']]
    return records


def translate_gpu_air(path, entries, compiler, work):
    from project.build import native as _component_native
    data = path.read_bytes()
    if not data or len(data) > 16*1024*1024: raise RuntimeError('AIR input must be 1 byte..16 MiB')
    if entries and (len(entries) != len(set(entries)) or any(not isinstance(n,str) or not n for n in entries)):
        raise RuntimeError('invalid or duplicate selected AIR entries')
    key = hashlib.sha256(data).hexdigest()[:24]
    aliases = [path]; modules = []
    if data[:4] in (b'MTLB',b'\xca\xfe\xba\xbe',b'\xbe\xba\xfe\xca',b'\xca\xfe\xba\xbf',b'\xbf\xba\xfe\xca'):
        records = extract_gpu_metallib(data)
        if entries and set(entries) - {r['name'] for r in records}: raise RuntimeError('selected metallib function is absent')
        for index,record in enumerate(records):
            if entries and record['name'] not in entries: continue
            if record['type'] not in (0,1,2): raise RuntimeError('metallib contains non-stage functions; select supported entries explicitly')
            module = work / (key+'-'+str(index)+'.air'); module.write_bytes(record['data'])
            modules.append((module,[record['name']],{0:1,1:2,2:0}[record['type']]))
    elif path.suffix == '.ll':
        module = work / (key+'.air')
        _component_native.run([_component_native.host('llvm-as'), path, '-o', module])
        modules.append((module,entries,None)); aliases.append(module)
    else:
        if data[:4] not in (b'BC\xc0\xde',b'\xde\xc0\x17\x0b'): raise RuntimeError('input is not LLVM AIR bitcode or metallib')
        modules.append((path,entries,None))
    functions = []
    for index,(module,selected,expected_stage) in enumerate(modules):
        output = work / (key+'-'+str(index)+'.json'); output.unlink(missing_ok=True)
        _component_native.run([compiler,module,output,*selected])
        lowered = json.loads(output.read_text())
        if expected_stage is not None and any(f['stage'] != expected_stage for f in lowered):
            raise RuntimeError('metallib TYPE disagrees with AIR stage metadata')
        functions.extend(lowered)
    return functions, aliases


def build_gpu_shaders():
    from project.build import native as _component_native
    root = build_state.ROOT / 'drivers/gpu/QcomMTL'
    config = root / 'ShaderBuild.json'
    settings = json.loads(config.read_text())
    if settings['gpu'] != 540 or settings.get('local_size') != 'variable': raise RuntimeError('unsupported shader profile')
    definitions = [(item,[]) if isinstance(item,str) else (item['path'],item.get('entries',[])) for item in settings['sources']]
    sources = [(root / name).resolve() for name,_ in definitions]
    inputs = [config, *sources, build_state.TOOLS / 'gpu-compiler/Export.h', build_state.TOOLS / 'gpu-compiler/Interface.h',
              build_state.TOOLS / 'gpu-compiler/MSL.cpp', build_state.TOOLS / 'gpu-compiler/MSLMain.cpp', build_state.TOOLS / 'gpu-compiler/Compiler.h',
              build_state.TOOLS / 'gpu-compiler/Semantic.h', build_state.TOOLS / 'gpu-compiler/AIRControlFlow.h', build_state.TOOLS / 'gpu-compiler/LibraryArchive.h', build_state.TOOLS / 'gpu-compiler/FunctionConstants.h', build_state.TOOLS / 'gpu-compiler/AIRSpecialization.h', build_state.TOOLS / 'gpu-compiler/AIRIndirectCalls.h', build_state.TOOLS / 'gpu-compiler/AIRSIMD.h', build_state.TOOLS / 'gpu-compiler/AIRMatrixNIR.h', build_state.TOOLS / 'gpu-compiler/AIRVisibleFunctions.h', build_state.TOOLS / 'gpu-compiler/Source.json', build_state.TOOLS / 'gpu-compiler/AIR.cpp', build_state.SELF,
              build_state.DRIVERS_ROOT / 'include/GPUShaderProtocol.h', build_state.DRIVERS_ROOT / 'include/GPUShaderInterface.h', build_state.DRIVERS_ROOT / 'include/GPUJITProtocol.h']
    generated = build_state.BUILD / 'obj/drivers/generated/GPU'
    manifest = generated / 'shaders.json'; header = generated / 'GPUPrograms.h'
    hashes = {str(p.relative_to(build_state.ROOT)):_component_native.digest(p) for p in inputs}
    if manifest.exists() and header.exists():
        previous = json.loads(manifest.read_text())
        cached = previous.get('generated_sha256', {})
        expected_files = {'GPUPrograms.h','A5xxRegs.h','default.qmetallib'} | set(previous.get('air_libraries',[]))
        if previous.get('inputs') == hashes and set(cached) == expected_files and all((generated / name).exists() and
                _component_native.digest(generated / name) == value for name,value in cached.items()):
            return generated
    air_compiler = None; llvm_version = None
    if any(path.suffix != '.metal' for path in sources): air_compiler,llvm_version = prepare_gpu_air_compiler()
    compiler, mesa = prepare_gpu_compiler()
    generate_gpu_registers(mesa, generated)
    programs = []; names = set(); work = build_state.BUILD / 'obj/gpu-compiler/shaders'; work.mkdir(parents=True, exist_ok=True)
    air_work = build_state.BUILD / 'obj/gpu-compiler/air'; air_work.mkdir(parents=True,exist_ok=True)
    binary_libraries = []; generated_libraries = {}
    for path,(_,selected) in zip(sources,definitions):
        if path.suffix == '.metal':
            functions = translate_gpu_msl(path.read_text())
            if selected: raise RuntimeError('MSL entry selection is not supported')
        else:
            functions,aliases = translate_gpu_air(path,selected,air_compiler,air_work)
            for alias in aliases:
                binary_libraries.append({'sha256':_component_native.digest(alias),'bytes':alias.stat().st_size,
                    'names':[f['name'] for f in functions],'format':'metallib' if path.suffix=='.metallib' else 'air'})
            if path.suffix == '.ll':
                name = path.name[:-3]
                if not name.endswith('.air'): name += '.air'
                generated_libraries[name] = aliases[-1]
        for function in functions:
            if function['name'] in names: raise RuntimeError('duplicate shader entry point ' + function['name'])
            names.add(function['name']); identifier = len(programs) + 1
            glsl = work / (str(identifier) + '.' + function['extension'])
            glsl.write_text(function.pop('glsl'))
            variants = []
            for msaa in ([False, True] if function['stage']==2 else [False]):
                suffix = '.msaa' if msaa else ''
                output = work / (str(identifier)+suffix+'.json')
                output.unlink(missing_ok=True)
                compiler_log = work / (str(identifier)+suffix+'.log')
                compiler_log.unlink(missing_ok=True)
                env = {k:v for k,v in os.environ.items() if not k.startswith(('IR3_','FD_MESA_','QCOM_IR3_'))}
                env['QCOM_IR3_OUTPUT'] = str(output)
                if msaa: env['QCOM_IR3_MSAA'] = '1'
                _component_native.run([compiler, '-g', '540', glsl], log=compiler_log, env=env)
                result = json.loads(output.read_text()); result['metadata'].update({
                    'id':identifier, 'name':function['name'], 'bufferMask':function['bufferMask'],
                    'writeMask':function['writeMask'], 'strides':function['strides'], 'instanceMask':function['instanceMask'],
                    'textureBindings':function.get('textureBindings',0),
                    'imageUintMask':function.get('imageUintMask',0)&result['metadata']['imageMask']})
                m=result['metadata'];m['depthTextureMask']=function.get('depthTextureMask',0)&(m['imageMask']|m['sampleMask']|m['fetchMask']|m['queryMask'])
                if m['framebufferMask'] & ~function.get('framebufferMask',0):
                    raise RuntimeError('compiler framebuffer input exceeds its source declarations')
                declarations = function['varyings']
                if len(declarations)>32 or result['metadata']['varyingCount']>len(declarations):
                    raise RuntimeError('compiler stage interface exceeds its source declarations')
                merged = []
                for index in range(32):
                    if index>=len(declarations):
                        merged.append({'semantic':[0]*4,'components':0,'mode':0,'regid':0,'mask':0,'inloc':0,'reserved':0})
                        continue
                    decl = declarations[index]; hardware = result['metadata']['varyings'][index]
                    if hardware['components'] and (hardware['components'] != decl['components'] or
                            (function['stage']==2 and hardware['mode'] != decl['mode'])):
                        raise RuntimeError('compiler stage interface disagrees with source declarations')
                    merged.append({**hardware,**decl,'reserved':0})
                result['metadata']['varyings'] = merged
                result['metadata']['varyingCount'] = len(declarations)
                variants.append(result)
            result = variants[0]
            if len(variants)>1: result['msaa'] = variants[1]
            result['source'] = str(path.relative_to(build_state.ROOT)); result['source_format'] = 'msl' if path.suffix=='.metal' else 'air'
            programs.append(result)
    if not programs or len(programs) > 256: raise RuntimeError('invalid AOT program count')
    registry = int.from_bytes(hashlib.sha256(json.dumps(programs,sort_keys=True).encode()).digest()[:8], 'little')
    fields = ['id','stage','codeWords','instrlen','constlen','fullRegs','halfRegs','branchStack',
        'threadSize','subgroupSize','immediateBase','immediateCount','driverBase','driverCount',
        'localID','groupID','vertexID','position','color','fragCoord','fragMask','bufferMask','writeMask','textureCount','flags','instanceID','instanceMask',
        'varyingCount','totalIn','perspective','linear','centerRHW','sharedBytes','threadgroupMask','imageMask','imageReadMask','imageWriteMask','imageUintMask','sampleMask','samplerMask','explicitSamplerMask','cubeSamplerMask','fetchMask','queryMask']
    sampler_fields = ['implicitSamplerMask','constantSamplerMask','constantImplicitSamplerMask','constantExplicitSamplerMask','constantCubeSamplerMask','framebufferMask']
    sampler_state_fields = ['filters','minLOD','maxLOD','addressModes','maxAnisotropy','borderColor','normalized']
    lines = ['// Generated by tools/project/build/toolchain.py build-gpu-shaders. Do not edit.',
        '// Shader machine code generated by Mesa IR3 (MIT); see tools/gpu-compiler/Source.json.',
        '#pragma once', '#include <GPUShaderProtocol.h>', 'namespace GPUPrograms {',
        f'constexpr uint64_t Registry = 0x{registry:016x}ULL;']
    sample_fields = ['sampleFlags','sampleID','sampleMaskIn','sampleMaskOut','perspectiveCentroid','linearCentroid','perspectiveSample','linearSample','textureBindings']
    def program_initializer(program, symbol, variant='nullptr'):
        m = program['metadata']
        interface = []
        for v in m['varyings']:
            interface.append('{{'+','.join(map(str,v['semantic']))+'},'+','.join(str(v[f]) for f in ['components','mode','regid','mask','inloc','reserved'])+'}')
        return '{'+','.join(str(m[f]) for f in fields)+',{'+','.join(map(str,m['strides']))+'},{'+','.join(interface)+'},'+str(m['attributeMask'])+',{'+','.join('{'+','.join(str(a[f]) for f in ['components','type','regid','mask'])+'}' for a in m['attributes'])+'},'+','.join(str(m[f]) for f in sampler_fields)+',{'+','.join(map(str,m['constantSamplerSlots']))+'},{'+','.join(map(str,m['constantSamplerLevelSlots']))+'},{'+','.join('{'+','.join(str(a[f]) for f in sampler_state_fields)+'}' for a in m['constantSamplers'])+'},'+str(m['colorMask'])+',{'+','.join(map(str,m['colorRegisters']))+'},'+str(m['depthReg'])+','+str(m['multisampleMask'])+','+str(m['depthTextureMask'])+','+str(m['volumeMask'])+','+str(m['imageblockMask'])+','+str(m['imageblockBindings'])+','+str(m['imageblockSampleBytes'])+','+','.join(str(m[f]) for f in sample_fields)+','+str(m['constantWindowCount'])+',{'+','.join('{'+str(v['buffer'])+','+str(v['byteOffset'])+'}' for v in m['constantWindows'])+'},Code'+symbol+',Immediates'+symbol+','+json.dumps(m['name'])+','+variant+'}'
    for owner in programs:
        for suffix,program in [('',owner)]+([('MSAA',owner['msaa'])] if 'msaa' in owner else []):
            symbol=str(program['metadata']['id'])+suffix
            lines.append('static const uint32_t Code'+symbol+'[] = {'+','.join(hex(v) for v in program['code'])+'};')
            lines.append('static const uint32_t Immediates'+symbol+'[] = {'+','.join(hex(v) for v in (program['immediates'] or [0]))+'};')
            if suffix: lines.append('static const GPU::Shader::Program MSAA'+str(owner['metadata']['id'])+' = '+program_initializer(program,symbol)+';')
    lines.append('static const GPU::Shader::Program Programs[] = {')
    for program in programs:
        symbol=str(program['metadata']['id'])
        lines.append(program_initializer(program,symbol,'&MSAA'+symbol if 'msaa' in program else 'nullptr')+',')
    lines += ['};','inline const GPU::Shader::Program *lookup(uint32_t id) {',
              'return id && id <= sizeof(Programs)/sizeof(Programs[0]) ? &Programs[id-1] : nullptr;', '}']
    library = {'schema':'QcomMTLLibrary','version':1,'registry':f'{registry:016x}',
        'sources':[{ 'source':p.read_text(), 'names':[v['metadata']['name'] for v in programs if v['source']==str(p.relative_to(build_state.ROOT))]} for p in sources if p.suffix=='.metal'],
        'binaries':binary_libraries,
        'functions':[{'id':p['metadata']['id'],'name':p['metadata']['name'],'stage':p['metadata']['stage']} for p in programs]}
    blob = json.dumps(library,sort_keys=True,separators=(',',':'))
    lines += ['static const char Library[] = '+json.dumps(blob)+';', '}']
    generated.mkdir(parents=True,exist_ok=True); header.write_text('\n'.join(lines)+'\n')
    (generated / 'default.qmetallib').write_text(blob)
    for name,path in generated_libraries.items(): shutil.copyfile(path,generated / name)
    manifest.write_text(json.dumps({'inputs':hashes,'registry':f'{registry:016x}', 'programs':programs,
        'generated_sha256':{name:_component_native.digest(generated / name) for name in ['GPUPrograms.h','A5xxRegs.h','default.qmetallib',*generated_libraries]},
        'compiler_commit':json.loads((build_state.TOOLS / 'gpu-compiler/Source.json').read_text())['commit'],
        'air_llvm_version':llvm_version, 'air_libraries':list(generated_libraries),
        'hardware_tested':False},indent=2)+'\n')
    print('Compiled '+str(len(programs))+' MSL/AIR entries to real A540 IR3; registry '+f'{registry:016x}')
    return generated


def generate_gpu_registers(mesa, generated):
    """Keep Mesa's integer register encodings, including address-unit shifts."""
    registers = mesa / 'src/freedreno/registers'
    text = subprocess.check_output([sys.executable, registers / 'gen_header.py', '--rnn', registers,
        '--xml', registers / 'adreno/a5xx.xml', 'c-defines'],text=True)
    notice = re.search(r'/\* Autogenerated.*?\*/',text,re.S)
    output = ['#pragma once', '#include <stdint.h>', notice[0] if notice else '']
    output += [line for line in text.splitlines() if re.match(r'#define (REG_A5XX_|A5XX_)',line)]
    for match in re.finditer(r'static inline uint32_t (REG_A5XX_\w+)\(([^)]*)\)\s*\{\s*return ([^;]+);\s*\}',text):
        args = ','.join(part.strip().split()[-1] for part in match[2].split(',') if part.strip())
        output.append('#define '+match[1]+'('+args+') ('+match[3]+')')
    for match in re.finditer(r'static inline uint32_t (A5XX_\w+)\(([^)]*)\)\s*\{([^}]+)\}',text):
        expression = re.search(r'return ([^;]+);',match[3])
        # Floating-point encoders need user-mode math. Kernel uses raw IEEE bits
        # for viewport state and never expands those encoders.
        if expression and 'float' not in match[2] and not re.search(r'\b(fui|util_|fui16)\b',expression[1]):
            arg = match[2].strip().split()[-1]
            output.append('#define '+match[1]+'('+arg+') ('+expression[1]+')')
    generated.mkdir(parents=True,exist_ok=True)
    (generated / 'A5xxRegs.h').write_text('\n'.join(output)+'\n')


def build_gpu_metal():
    from project.build import clients as _component_clients, native as _component_native
    source = build_state.ROOT / 'drivers/gpu/QcomMTL'
    generated = build_gpu_shaders()
    bundle = build_state.BUILD / 'output/QcomMTLDriver.bundle'
    bundle.mkdir(parents=True,exist_ok=True)
    libraries = prepare_gpu_jit_compiler(build_state.BUILD / 'obj/gpu-compiler/mesa')
    runtime = bundle / 'QcomGPUCompiler.dylib'; shutil.copyfile(libraries[0],runtime)
    _component_native.run([sys.executable,build_state.TOOLS/'gpu-compiler/system-shaders.py',
         '--output',generated/'GPUSystemShaders.h','--work',build_state.BUILD/'obj/gpu-system-shaders',
         '--firmware-report',build_state.BUILD/'obj/filesystem/inputs/prepared-firmware.json'],
        log=build_state.BUILD/'logs/gpu-system-shaders-build.log')
    # Copy only LLVM's C declarations/configuration. Adding /usr/include to a
    # target compile would accidentally pick up Linux libc headers.
    llvm_include = Path(subprocess.check_output([_component_native.host('llvm-config'),'--includedir'],text=True).strip())
    headers = build_state.BUILD / 'obj/gpu-compiler/include'
    shutil.copytree(llvm_include/'llvm-c',headers/'llvm-c',dirs_exist_ok=True)
    shutil.copytree(llvm_include/'llvm/Config',headers/'llvm/Config',dirs_exist_ok=True)
    # Keep persisted shader code across render/GMEM tuning and version bumps.
    # Compiler, constant interpretation, archive ABI and AOT registry changes
    # still invalidate it. The runtime adds the OS build to this identity.
    from project.build.gpu_archive import build_context
    archive_build = build_context(build_state.ROOT, runtime, generated,
        subprocess.check_output([build_state.BIN/'clang++-arm64','--version'],text=True).strip())
    context = bytes.fromhex(archive_build['context'])
    (generated/'GPUArchiveBuild.h').write_text('// Generated by the unified graphics build.\n#pragma once\n#include <stdint.h>\n'
        'namespace GPUArchiveBuild { constexpr uint8_t Context[32] = {'+
        ','.join('0x%02x'%byte for byte in context)+'}; }\n')
    (generated/'archive-build.json').write_text(json.dumps(archive_build,indent=2)+'\n')
    _component_clients.build_iokit_client('QcomMTLDriver', ['../drivers/gpu/QcomMTL/QcomMTL.mm', 'gpuctl/GPUConnection.cpp',
                       'gpu-compiler/MSL.cpp','gpu-compiler/AIRRuntime.cpp','gpu-compiler/Compiler.cpp'],
                       extra_exports=['IOConnectCallMethod', 'IOObjectConformsTo', 'IOObjectRetain',
                                      'IORegistryEntryGetRegistryEntryID', 'IORegistryEntryCreateCFProperty',
                                      'IODataQueueDataAvailable', 'IODataQueueDequeue',
                                      'IOConnectSetNotificationPort', 'IODataQueueAllocateNotificationPort'],
                       headers=['gpuctl/GPUConnection.h', 'include/IOKitUser.h',
                                'gpu-compiler/Compiler.h','gpu-compiler/AIRNIR.h','gpu-compiler/AIRNIRLowering.h','gpu-compiler/AIRNIR.cpp','gpu-compiler/Semantic.h','gpu-compiler/AIRControlFlow.h','gpu-compiler/LibraryArchive.h','gpu-compiler/FunctionConstants.h','gpu-compiler/AIRSpecialization.h','gpu-compiler/AIRIndirectCalls.h','gpu-compiler/AIRSIMD.h','gpu-compiler/AIRMatrixNIR.h','gpu-compiler/AIRVisibleFunctions.h','gpu-compiler/Interface.h','gpu-compiler/Source.json','gpu-compiler/NOTICE.txt',
                                '../drivers/gpu/QcomMTL/QcomMTL.h', '../drivers/gpu/QcomMTL/NativeMetal.h', '../drivers/gpu/QcomMTL/MetalSync.h', '../drivers/gpu/QcomMTL/MetalTexture.h', '../drivers/gpu/QcomMTL/MetalColor.h', '../drivers/gpu/QcomMTL/MetalHeap.h', '../drivers/gpu/QcomMTL/MetalReference.h', '../drivers/gpu/QcomMTL/MetalArchive.h', '../drivers/gpu/QcomMTL/CoreFSCache.h', '../drivers/gpu/QcomMTL/MetalBatch.h', '../drivers/gpu/QcomMTL/MetalUploadPool.h', '../drivers/gpu/QcomMTL/MetalRetirement.h',
                                'gpu-compiler/SystemShaderCatalog.h', build_state.BUILD / 'obj/drivers/generated/GPU/GPUSystemShaders.h',
                                build_state.BUILD / 'obj/drivers/generated/GPU/GPUArchiveBuild.h', build_state.BUILD / 'obj/drivers/generated/GPU/archive-build.json',
                                *('sdk/iPhoneOS18.6.sdk/System/Library/Frameworks/Metal.framework/Headers/'+name for name in
                                  ['MTLBinaryArchive.h','MTLCommandQueue.h','MTLCommandBuffer.h','MTLDevice.h','MTLBuffer.h','MTLTexture.h','MTLHeap.h','MTLResource.h','MTLAllocation.h','MTLFunctionDescriptor.h','MTLRenderPipeline.h','MTLComputePipeline.h','MTLLinkedFunctions.h','MTLLibrary.h','MTLArgument.h','MTLFence.h','MTLBlitCommandEncoder.h','MTLComputeCommandEncoder.h','MTLRenderCommandEncoder.h']),
                                '../drivers/gpu/QcomMTL/Info.plist', build_state.ABI_ROOT / 'metal-plugin-abi.json', build_state.ABI_ROOT / 'ui-presentation-abi.json', build_state.ABI_ROOT / 'coreanimation-library-abi.json', build_state.ABI_ROOT / 'function-constants-abi.json', build_state.FIRMWARE_DIR / 'models/gpu-fragment.json', build_state.ABI_ROOT / 'framebuffer-fetch-abi.json', build_state.ABI_ROOT / 'mask-formats-abi.json', build_state.ABI_ROOT / 'draw-batches-abi.json', build_state.ABI_ROOT / 'native-pipelines-abi.json', build_state.FIRMWARE_DIR / 'models/gpu-service-access.json',
                                build_state.BUILD / 'obj/drivers/generated/GPU/GPUPrograms.h'],
                       protocol=['GPUUserProtocol.h', 'GPUNativeProtocol.h', 'GPURenderPass.h', 'GPUMetalCapabilities.h', 'GPUNativeCompletion.h', 'GPUNativeMemory.h', 'GPUSurfaceProtocol.h', 'GPUBlit.h', 'GPUMemoryRanges.h', 'GPUShaderInterface.h', 'GPUShaderProtocol.h', 'GPUJITProtocol.h', 'GPUDiagnostics.h'],
                       bundle=bundle, frameworks=['Foundation', 'Metal', 'IOSurface'],
                       include_dirs=[build_state.TOOLS / 'gpuctl', build_state.TOOLS/'gpu-compiler', headers, generated],
                       libraries=[runtime, build_state.ROOT / 'tools/sdk/iPhoneOS18.6.sdk/usr/lib/libLLVM.tbd'],
                       exception_sources=['MSL.cpp','AIRRuntime.cpp','Compiler.cpp'], adhoc_sign=True)
    shutil.copyfile(source / 'Info.plist', bundle / 'Info.plist')
    shutil.copyfile(generated / 'default.qmetallib', bundle / 'default.qmetallib')
    for name in json.loads((generated / 'shaders.json').read_text()).get('air_libraries',[]):
        shutil.copyfile(generated / name,bundle / name)
    shutil.copyfile(build_state.BUILD / 'obj/gpu-compiler/NOTICE.Mesa.txt', bundle / 'NOTICE.Mesa.txt')
    _component_clients.build_iokit_client('metalctl', ['gpuctl/metalctl.mm'],
                       headers=['gpuctl/AIRNIROutputDiagnostics.h','gpuctl/RenderClearDiagnostics.h','gpuctl/GMEMDiagnostics.h', '../drivers/gpu/QcomMTL/CoreFSCache.h',
                                '../drivers/gpu/QcomMTL/QcomMTL.h', '../drivers/gpu/QcomMTL/NativeMetal.h', 'include/IOKitUser.h'],
                       protocol=['GPUDiagnostics.h', 'GPUUserProtocol.h','GPUNativeProtocol.h','GPURenderPass.h','GPUShaderProtocol.h','GPUShaderInterface.h','GPUJITProtocol.h'],
                       extra_exports=['IORegistryEntryGetRegistryEntryID', 'IORegistryEntryCreateCFProperty', 'IOConnectCallMethod'],
                       frameworks=['Foundation', 'Metal', 'IOSurface', 'QuartzCore', 'CoreGraphics'], include_dirs=[source],
                       adhoc_sign=True)
    # A filesystem overlay is a reviewable build product, never an installer.
    # Metal.framework derives this exact bundle path from MetalPluginName.
    staging = build_state.BUILD / 'output/gpu-rootfs'
    installed = staging / 'System/Library/Extensions' / bundle.name
    shutil.copytree(bundle, installed, dirs_exist_ok=True)
    diagnostic = staging / 'usr/local/bin/metalctl'
    diagnostic.parent.mkdir(parents=True, exist_ok=True)
    shutil.copyfile(build_state.BUILD / 'output/metalctl', diagnostic)
    diagnostic.chmod(0o755)
    from project.firmware.trustcache import development_module, module_info
    trustcache, executables = development_module([installed, diagnostic])
    trustcache_path = build_state.BUILD / 'output/gpu.trustcache'
    trustcache_path.write_bytes(trustcache)
    files = {str(path.relative_to(staging)): _component_native.digest(path) for path in
             [*sorted(p for p in installed.rglob('*') if p.is_file()), diagnostic]}
    manifest = {'status':'staged-offline', 'hardware_tested':False, 'system_load_verified':False,
                'metal_plugin_name':'QcomMTLDriver', 'principal_class':'QcomMTLDevice',
                'device_superclass':'_MTLDevice', 'entry_selector':'initWithAcceleratorPort:',
                'kernel_personality':str((build_state.DRIVERS_ROOT/'gpu/QcomGPU/Info.plist').relative_to(build_state.ROOT)),
                'kernel_personality_sha256':_component_native.digest(build_state.DRIVERS_ROOT/'gpu/QcomGPU/Info.plist'), 'files':files,
                'development_trustcache':{'path':str(trustcache_path.relative_to(staging.parent)),
                                        **module_info(trustcache), 'executables':executables}}
    (staging/'manifest.json').write_text(json.dumps(manifest,indent=2)+'\n')
