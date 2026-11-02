"""Native IOKit diagnostic client builds."""
import json
from pathlib import Path
import re
import subprocess
import struct
from project.build import context as build_state


def build_power_client():
    build_iokit_client('powerctl', ['powerctl/powerctl.cpp'],
                       extra_exports=['IORegistryEntryCreateCFProperty'],
                       headers=['include/IOKitUser.h', 'gpuctl/CPUStatus.h'],
                       protocol='PowerUserProtocol.h', adhoc_sign=True,
                       entitlements={'com.apple.security.exception.iokit-user-client-class': ['PowerUserClient']},
                       signing_identifier='org.darwin.development.powerctl')


def build_iokit_client(name, source_paths, extra_exports=(), headers=(), protocol=None,
                       bundle=None, frameworks=(), include_dirs=(), libraries=(), exception_sources=(),
                       adhoc_sign=False, loadable=False, blocks=False, entitlements=None, signing_identifier=None):
    """Cross-link internal iOS IOKit tools through one build path; no execution."""
    from project.build import native as _component_native
    _component_native.aliases()
    user_sdk = build_state.ROOT / 'tools/sdk/iPhoneOS18.6.sdk'
    sources = [(build_state.TOOLS / path).resolve() for path in source_paths]
    output = bundle / name if bundle else build_state.BUILD / 'output' / (name + '.dylib' if loadable else name)
    output.parent.mkdir(parents=True, exist_ok=True)
    objdir = build_state.BUILD / 'obj/drivers/obj' / name
    objdir.mkdir(parents=True, exist_ok=True)
    tbd = user_sdk / 'System/Library/Frameworks/IOKit.framework/Versions/A/IOKit.tbd'
    exports = tbd.read_text()
    symbols = ['IOServiceMatching', 'IOServiceGetMatchingServices', 'IOIteratorNext', 'IOObjectRelease',
               'IOServiceOpen', 'IOServiceClose', 'IOConnectCallScalarMethod', *extra_exports]
    for symbol in symbols:
        if not re.search(r'\b_' + symbol + r'\b', exports):
            raise RuntimeError('user SDK lacks IOKit export ' + symbol)
    objects = []
    for source in sources:
        obj = objdir / (source.name + '.o')
        objc = ['-fobjc-arc', '-fblocks', '-fobjc-exceptions', '-fvisibility=hidden'] if source.suffix == '.mm' else []
        exceptions = '-fexceptions' if source.name in exception_sources else '-fno-exceptions'
        cpu = ['-mcpu=generic','-march=armv8-a+crc+crypto'] if name in ('QcomMTLDriver','metalctl') else []
        _component_native.run([build_state.BIN / 'clang++-arm64', '-isysroot', user_sdk, '-std=c++17', exceptions, '-fno-rtti',
             '-Wall', '-Wextra', '-Werror', '-Oz' if name == 'displayctl' else '-O2', *cpu, '-I' + str(build_state.DRIVERS_ROOT / 'include'),
             *(['-fblocks'] if blocks else []),
             '-I' + str(build_state.TOOLS / 'include'), *('-I' + str(path) for path in include_dirs), *objc,
             '-c', source, '-o', obj])
        objects.append(obj)
    llvm_symbols = set()
    if name == 'QcomMTLDriver':
        exports = (user_sdk/'usr/lib/libLLVM.tbd').read_text()
        for obj in objects:
            undefined = subprocess.check_output([_component_native.host('llvm-nm'),'--undefined-only',obj],text=True)
            for symbol in re.findall(r'\b(_LLVM\w+)\b',undefined):
                if not re.search(r'\b'+re.escape(symbol)+r'\b',exports):
                    raise RuntimeError('iOS LLVM lacks runtime AIR C export '+symbol)
                llvm_symbols.add(symbol)
    _component_native.run([_component_native.host('ld64.lld'), '-arch', 'arm64', '-syslibroot', user_sdk,
         '-platform_version', 'ios', '18.6', '18.6', '-o', output, *objects,
         *(['-adhoc_codesign'] if adhoc_sign or entitlements or loadable else []),
         *(['-x'] if name == 'displayctl' and not loadable else []),
         *(['-dylib', '-install_name', '@rpath/' + name + '.dylib', '-dead_strip', '-x', '-lobjc',
            '-no_function_starts', '-no_data_in_code_info',
            '-exported_symbol', '_' + name + '_struct'] if loadable else []),
         *(['-dylib', '-install_name', '@rpath/QcomMTLDriver.bundle/QcomMTLDriver', '-lobjc', '-lc++',
            '-exported_symbol', '_QcomMTLCreateDevice', '-exported_symbol', '_QcomMTLCreateSystemDefaultDevice', '-exported_symbol', '_QcomMTLEncodeComposition',
            '-exported_symbol', '_OBJC_CLASS_$_QcomMTLDevice', '-exported_symbol', '_OBJC_METACLASS_$_QcomMTLDevice'] if bundle else []),
         '-lSystem', '-framework', 'IOKit', '-framework', 'CoreFoundation',
         *(arg for framework in frameworks for arg in ['-framework', framework]),
         *libraries, *(['-rpath', '@executable_path'] if libraries else []),
         *(['-lobjc', '-lc++'] if libraries or any(source.suffix == '.mm' for source in sources) else [])])
    signature=None
    if entitlements:
        from project.firmware.macho import sign_new_macho
        data,signature=sign_new_macho(output.read_bytes(),signing_identifier,entitlements,
                                     compact=name == 'displayctl')
        output.write_bytes(data)
    data = output.read_bytes()
    if struct.unpack_from('<4I', data) != (0xfeedfacf,0x100000c,0,6 if bundle or loadable else 2):
        raise RuntimeError('user binary has the wrong arm64 Mach-O type')
    report = {'status':'cross-linked-offline', 'hardware_tested':False, 'path':str(output.relative_to(build_state.ROOT)),
              'sha256':_component_native.digest(output),'source_sha256':_component_native.digest(sources[0]),'iokit_exports_verified':symbols,
              'inputs':{str(path.relative_to(build_state.ROOT)):_component_native.digest(path) for path in
                        [*sources, *((build_state.TOOLS / path).resolve() for path in headers)]}}
    if signature:report['signature']=signature
    if entitlements:
        for path in [build_state.SELF,build_state.ROOT/'tools/project/firmware/macho.py',
                     build_state.ROOT / 'tools/sdk/iPhoneOS18.6.sdk/usr/lib/system/libxpc.tbd']:
            report['inputs'][str(path.relative_to(build_state.ROOT))]=_component_native.digest(path)
    if bundle:
        protocol_versions={name:int(re.search(r'\bVersion\s*=\s*(\d+)',
            (build_state.DRIVERS_ROOT/('include/GPU'+name+'Protocol.h')).read_text()).group(1)) for name in ('Shader','JIT','Surface')}
        report.update({'stage':'native-system-plugin-shader-render-compute-linear-textures',
                       'universal_air64_metallib_import':True,'metallib_parser_shared_with_aot':True,
                       'metallib_function_compilation':'on-demand, exact selected AIR entry',
                       'bundled_system_shaders':{key:value for key,value in json.loads((build_state.BUILD/'obj/gpu-system-shaders/report.json').read_text()).items()
                           if key in ('registry','attempted_entry_count','entry_count','compiled_entry_count','variant_count','packed_bytes','header_sha256')},
                       'bundled_system_shader_match':'full AIR SHA-256, entry name, resolved constant indices/types/bytes; current registry',
                       'bundled_system_shader_miss':'normal exact compiler/cache path; no substitute shader',
                       'native_submission_dispatch_target':True,'native_completion_dispatch_target':True,
                       'native_dispatch_target_configuration':'before first submission; serial wrappers retained',
                       'native_commit_and_wait_until_submitted':True,'native_commit_and_wait_return_type':'BOOL',
                       'native_inline_fifo_drain':True,'native_inline_fifo_scope':'guarded direct drawable and private full-submission helper; actual pixel admission',
                       'native_direct_drawable_commit_admission':True,
                       'native_direct_drawable_guards':'no uncommitted prefix, future event wait, external pending fence or explicit presentation barrier',
                       'native_direct_drawable_scheduled':'whole real operation list admitted before raw present follows commit',
                       'native_commit_submission_marker':False,
                       'native_commit_uncommitted_prefix':'return NO without waiting; caller can commit the predecessor on its serial submission target',
                       'native_commit_submission_scope':'drain FIFO through the requested buffer; YES only after complete real operation-list admission',
                       'native_commit_private_direct_drawable_wait':False,
                       'native_bounded_async_execution':True,'native_device_pending_jobs_max':16,
                       'native_resource_namespace_limit':1024,'native_resources_per_job':64,
                       'native_memory_limits':'version1 IORegistry data,64 bytes; effective client/global byte quotas from actual IOVA aperture',
                       'native_memory_accounting':'tracked namespace charges; actual live native objects and retained DMA mappings reported separately',
                       'native_mapper_capacity':4096,'native_mapper_allocator':'stable records with address-ordered index; one pass for first fit',
                       'native_single_resource_limit_bytes':16777216,
                       'native_async_completion_consumer':'actual Family IODataQueue, matching FIFO end records, separate retirement executor',
                       'native_admission_backpressure':'pre-Family vendor status, unchanged tokens/fences; bounded asynchronous retry',
                       'native_cpu_index_boundary':'all device jobs retired before private CPU index snapshot; kernel checks other clients',
                       'native_blit_parameter_reuse':'all5 immutable builtin layouts frozen in job IB before admission returns',
                       'native_partial_blit_resume':'accepted row/byte cursor; no duplicate GPU batch after capacity wake',
                       'native_callback_waits':'public waits include registered handler delivery; terminal buffers retire in FIFO; private helper returns NO until full operation-list admission',
                       'native_marker_scheduled_state':'every per-operation admission stays private; public Scheduled and helper YES require complete operation-list admission',
                       'native_display_submission_boundary':'retain drawable/FIFO barrier through real retirement; native IOSurface fence capability permits once-only presentation at whole-list Scheduled',
                       'native_fences':True,'native_fence_passes':['blit','compute','render vertex/fragment'],
                       'native_fence_capture':'snapshot all captured producing points in device commit order; prune retired points on the next update; producers committed before consuming waits',
                       'native_fence_multiple_producers':True,'native_fence_wait_future_updates':False,
                       'native_fence_completion':'successful real IOGPU end token plus native queue identity; producer errors propagate',
                       'native_fence_waits':'suspend FIFO head and resume by point callback; no busy wait or occupied submission target',
                       'native_event_waits':'bounded background wait, suspended FIFO head; original event strong or zeroing weak according to retainedReferences',
                       'native_memory_barriers':'validated resource/scope/stages; actual native CP Barrier submission',
                       'native_command_buffer_retained_references':True,
                       'native_command_buffer_reference_interfaces':['commandBuffer','commandBufferWithUnretainedReferences','commandBufferWithDescriptor:'],
                       'native_external_resource_ownership':'strong through real retirement or zeroing weak for caller-owned resources; actual retainedReferences query',
                       'native_borrowed_identity_validation':'commit and per-operation admission/retry; missing objects fail InvalidResource without resolving recycled IDs',
                       'native_internal_resource_ownership':'inline bytes, resolve parameters and helper-owned composition objects retained until real retirement',
                       'native_reference_snapshot_release':'partial and complete CPU snapshots released after condition/order/transport locks; encoder bindings cleared at endEncoding',
                       'native_reference_runtime_verified':False,
                       'native_command_buffer_quota_policy':'nonblocking bring-up limit; nil when no capacity',
                       'coreanimation_function_constants':True,
                       'function_constant_scope':'AIR inside MTLB/FAT, scalar/vector constants and bounded static initialization',
                       'native_function_constant_values_reused':True,'native_function_constant_reflection':True,
                       'function_constant_async_creation':True,'function_constant_cache':'effective typed values; index precedence; unrelated values ignored',
                       'native_function_descriptor':True,'native_function_descriptor_async':True,
                       'native_specialized_function_names':True,'native_pipeline_descriptor_options':True,
                       'native_render_pipeline_async':True,'native_compute_pipeline_async':True,
                       'native_compute_pipeline_descriptor':True,'native_pipeline_reflection':False,
                       'native_pipeline_binary_archives':True,'native_archive_only_miss_returns_error':True,
                       'native_binary_archive_profile':'A540 Compute/Render pipeline functions; lazy AIR function lookup before lowering/IR3; original Metal descriptors and archive-only options',
                       'native_binary_archive_format':'version1 bounded pointer-free QMTLARC1 with SHA-256, exact registry/Shader/JIT, build-source/compiler fingerprint and native OS context',
                       'native_binary_archive_aot':'validated catalog references only; never re-register builtin AOT code as mutable-parameter JIT',
                       'native_binary_archive_limits':{'functions':256,'pipelines':1024,'bytes':16777216,'lookup_archives':16},
                       'native_binary_archive_runtime_verified':False,
                       'native_pipeline_inputs':'copied original Apple descriptors and constant values; existing A540 AIR/JIT registration and IOGPU submission',
                       'function_constant_indices':65536,'function_constants_per_module':256,
                       'msl_function_constant_source_syntax':False,'half_shader_arithmetic':True,
                       'half_shader_scope':'AIR scalar/vector binary16 arithmetic, conversions, typed SSBO loads/stores, half texture sampling/read/write and float32 stage transport',
                       'half_shared_memory':False,'msl_half_source_syntax':False,
                       'unsigned_integer_textures':['R32Uint'],
                       'unsigned_integer_texture_scope':'compute read/write/query; 2D, arrays, cube and cube arrays; mip levels, exact uint swizzles, BGRA/RGBA raw word views and IOSurface imports',
                       'unsigned_integer_texture_filtering':False,'unsigned_integer_render_targets':False,'unsigned_integer_msaa':False,
                       'air_private_array_bytes_max':1024,'air_unorm4x8_pack':True,
                       'native_drawable_present_bridge':True,'drawable_present_after_gpu_completion':'legacy or non-IOSurface; native surface-fence capability queues at Scheduled',
                       'native_surface_producer_fence':'IOGPUNativeSurfaceFenceVersion1; real kernel IOSurface access fences, success only after A540 completion and coherence',
                       'drawable_presentation_modes':['immediate','atTime','afterMinimumDuration','native options'],
                       'native_drawable_texture_descriptor':True,'framebuffer_only_texture_restrictions':True,
                       'native_shared_event_command_bridge':True,'shared_event_wait_timeout_ms':5000,
                       'shared_event_wait_releases_device_transport_lock':True,
                       'gpu_layer_composition':True,'composition_layers_max':8,
                       'composition_features':['premultiplied alpha','opacity','affine geometry','contents crop','scissor','GPU background draw'],
                       'system_coreanimation_compositor_selected':False,'system_ui_gpu_acceleration_verified':False,
                       'system_metal_plugin_registered':False, 'system_metal_plugin_declared':True,
                       'system_metal_plugin_entry_implemented':True, 'system_metal_load_verified':False,
                       'system_metal_plugin_path':'/System/Library/Extensions/QcomMTLDriver.bundle',
                       'device_superclass':'_MTLDevice', 'principal_class':'QcomMTLDevice',
                       'system_entry_selector':'initWithAcceleratorPort:',
                       'shader_control_flow':True, 'msl_lexical_scopes':True,
                       'bounded_buffer_access':True,'buffer_load_oob':'zero','buffer_store_oob':'discard',
                       'constant_scalar_vector_parameters':True,'constant_struct_fields':True,
                       'inline_compute_vertex_bytes':4096,'shader_isa_sandbox':False,
                       'compute_variable_workgroups':True,'compute_grid_dimensions':3,
                       'compute_max_threads_per_group':1024,'compute_max_local_dimensions':[1024,1024,64],
                       'compute_max_invocations':16777216,'compute_buffer_index_proof':'NIR byte-range guards from SSBO descriptors',
                       'compute_nonuniform_workgroups':True,'compute_nonuniform_backend':'up to eight exact-size CP regions in one native submission, original grid/group coordinates and requested local size in native constants, no padded barrier lanes','compile_option_thread_limit_preserved':True,
                       'air_cfg_max_blocks':512, 'air_cfg_max_instructions':8192, 'air_phi_parallel_copies':True,
                       'air_texture_3d':'direct NIR; uncompressed color sample/read/write/query; mip chains and depth-plane views',
                       'texture_3d_mipmap_generation':False,
                       'air_tile_shader':'implicit half4/float4 imageblocks with dynamic 1/2/4 samples via compute and render attachment barriers',
                       'air_simd':'compute subgroup arithmetic, shuffle, vote and ballot; A540 subgroup width 64',
                       'air_indirect_calls':'finite local function-pointer selects, PHIs and tables lowered to direct-call dispatch',
                       'metal_visible_function_tables':True,'metal_visible_function_targets_max':64,'metal_visible_function_table_entries_max':256,'air_simd_matrix_8x8':True,
                       'air_cyclic_cfg':'per-invocation dispatch without an artificial iteration limit',
                       'runtime_llvm_compatibility':{'branch_opcodes':[2,70,71],
                           'switch_case_access':'legacy operand pairs or optional exported LLVMGetSwitchCaseValue'},
                       'native_limits_bytes':264, 'native_feature_profile':'no complete Apple GPU family',
                       'shader_compilation':True, 'shader_compilation_mode':'aot-and-runtime-jit-msl-air-profile', 'render_compute':True, 'multiple_color_attachments':True, 'max_color_attachments':8,
                       'multisample_rendering':True, 'texture_sample_counts':[1,2,4], 'gpu_color_resolve':True, 'msl_multisample_read':True, 'alpha_to_coverage':True,
                       'multisample_arrays':True, 'depth_stencil_arrays':True, 'multisample_array_stage_reads':['compute','vertex','fragment'],
                       'depth_texture_read_sample_compare':True, 'depth_texture_mip_chains':True, 'depth_stencil_mip_views':True, 'depth_texture_mip_levels_max':15,
                       'depth_texture_formats':['Depth32Float','Depth32Float_Stencil8 depth plane'],
                       'depth_texture_shapes':['2d','2d-array','2d-ms','2d-ms-array profile extension'],
                       'gpu_depth_stencil_resolve':True, 'depth_resolve_filters':['sample0','min','max'], 'stencil_resolve_filters':['sample0','depth-resolved-sample'],
                       'msl_as_type_32bit_same_lanes':True,
                       'sample_frequency_shading':True, 'centroid_sample_interpolation':True, 'fragment_sample_mask':True,
                       'fragment_single_msaa_variants':True, 'program_description_bytes':2484, 'jit_program_image_bytes':83784, 'fragment_constant_window_variants':True,
                       'fragment_depth_output':True, 'fragment_colorless_output':True, 'depth_only_nil_fragment':True,
                       'fragment_discard':True, 'fragment_software_demote':True, 'fragment_derivatives':True, 'air_float_comparisons':True,
                       'air_compilation':True, 'metallib_import':True, 'runtime_air_compilation':True,
                       'runtime_air_direct_nir':True, 'runtime_air_direct_nir_fallback':'existing AIR-to-GLSL compiler',
                       'runtime_air_direct_nir_scope':'selected/single-entry Compute and Fragment DAG profile; unsupported input uses GLSL',
                       'runtime_air_direct_nir_hardware_verified':False,
                       'constant_samplers':True, 'constant_sampler_states_max':16, 'hardware_sampler_slots':16,
                       'runtime_msl_compilation':True, 'jit_version':protocol_versions['JIT'], 'trusted_admin_compiler':True, 'jit_programs_max':256,
                       'shader_command_version':protocol_versions['Shader'], 'texture_arrays':True, 'texture_array_layers_max':2048,
                       'graphics_texture_fetch':True, 'graphics_texture_queries':True, 'graphics_fetch_alias_base':8, 'graphics_texture_descriptors':16,
                       'air_graphics_texture_binding_indices':16, 'graphics_enabled_textures_max':8, 'graphics_texture_binding_remap':True,
                       'air_texture_fetch_queries':True, 'air_texture_current_read_sampler':True, 'air_texture_legacy_read':True,
                       'air_texture_array_cube_operations':True, 'air_texture_gradient_sampling':True,
                       'shader_code_bytes_max':16384, 'shader_memory_fetch':True, 'graphics_shader_cache_partition':True,
                       'framebuffer_draw_ib_batches':True,'framebuffer_draw_ib_count_max':256,'framebuffer_draw_ib_bytes_max':16777216,'framebuffer_live_ib_quota_bytes':67108864,'framebuffer_draw_completion':'one native end token after all private read-only IBs','framebuffer_progress_watchdog_ms':500,
                       'cube_image_operations':True, 'cube_image_queries':True, 'cube_image_read_write':True,
                       'cube_textures':True, 'cube_arrays':True, 'cube_face_blit_render_mipmap':True, 'cube_msl_sampling':True, 'cube_air_intrinsics_verified':True,
                       'vertex_texture_sampling':True, 'vertex_texture_bindings':16, 'vertex_sampler_bindings':8, 'fragment_texture_bindings':16, 'fragment_sampler_bindings':8, 'graphics_independent_sampler_pairs':True, 'graphics_sparse_texture_slots':True,
                       'vertex_texture_msl_air_2d':True, 'vertex_texture_msl_arrays':True,
                       'vertex_fragment_texture_bindings_independent':True,
                       'texture_array_msl_read_write_sample':True, 'texture_array_air_intrinsics_verified':True,
                       'texture_array_slice_views':True, 'texture_array_render_slices':True,
                       'render_state_bridge':['blend','color-write-mask','viewport','cull','front-winding','depth-range','depth-stencil'],
                       'compute_texture_bindings':8,'compute_texture_access':['read','write','read_write','sample'],
                       'compute_texture_formats':['rgba8unorm','bgra8unorm','rgba8unorm_srgb','bgra8unorm_srgb','r8unorm','rg8unorm','rgba16float'],'compute_texture_mip_access':True, 'compute_texture_sampling':True, 'compute_sampler_bindings':8,
                       'compute_texture_backend':'guarded SSBO bank 8..15 for images; hardware CS texture state block 5 with independent texture/sampler slots; native-owned geometry constants for image/query access',
                       'texture_views':True,'texture_view_formats':['rgba8unorm','bgra8unorm','rgba8unorm_srgb','bgra8unorm_srgb','r8unorm','rg8unorm'],'texture_view_equal_element_size_required':True,'texture_view_mip_subranges':True,'texture_view_parent_retained':True,'texture_view_arbitrary_swizzle':True,'texture_swizzle_composition':True,'texture_swizzle_read_only':True,'texture_swizzle_mip_generation_ignored':True,'texture_view_storage':'shared original native backing; parent-relative metadata, rebased mip planes and bounded access span without allocating padding',
                       'buffer_backed_textures':True,'buffer_texture_type':'2d, single mip/layer/sample, existing6 color formats',
                       'buffer_texture_alignment_bytes':64,'buffer_texture_storage':'same native buffer resource ID and allocation; bounds checked against logical buffer length; buffer retains lifetime',
                       'native_linear_texture_entrypoint':'newLinearTextureWithDescriptor:offset:bytesPerRow:bytesPerImage:',
                       'native_linear_texture_image_stride':'2D, one mip/layer/sample; zero or complete bounded plane; original buffer backing retained',
                       'buffer_texture_operations':['CPU access for shared storage','blit','compute images','hardware sampling','render target','nested format/swizzle views'],
                       'buffer_texture_metadata':'direct buffer/offset/rowBytes; texture-created child reports parentTexture; rootResource resolves original buffer',
                       'buffer_texture_runtime_verified':False,
                       'native_texture_layout_spi':True,
                       'native_texture_layout_selector':'newTextureLayoutWithDescriptor:isHeapOrBufferBacked:',
                       'native_texture_layout_storage':'same checked A540 linear color mip/slice layout as allocated textures;16-byte adapter watermark trailer',
                       'native_texture_layout_copy':'checked bidirectional2D region copies, all supported single-sample color formats, arrays/cube faces; no GPU allocation',
                       'native_texture_layout_alignment_bytes':64,'native_texture_layout_watermark_bytes':16,
                       'native_texture_layout_runtime_verified':False,
                       'astc_4x4_textures':True,'astc_4x4_formats':['ASTC_4x4_LDR','ASTC_4x4_sRGB'],
                       'astc_4x4_storage':'16-byte blocks,1024-byte aligned base block rows,A540 pitch alignment4,last mip32 block rows',
                       'astc_4x4_access':'sample/read only; checked block-aligned copies with right/bottom edge exceptions; no runtime compression',
                       'native_blit_layout_version':2,'immutable_builtin_blit_layouts':5,
                       'render_load_clear':'dedicated immutable GPU row-fill; active width times format/sample bytes; stride and original backing offset retained',
                       'render_clear_padding':'preserve per-row and outer bytes; unique active-word threads on aligned rows, unique global-word merge for unaligned rows',
                       'render_clear_pattern':'32-bit or paired 64-bit pattern repeats from byte zero of each logical row',
                       'rgba16float_render_targets':True,'rgba16float_render_samples':[1,2,4],
                       'rgba16float_clear':'RGBA binary16 pairs; signed/HDR values retained; direct double conversion',
                       'rgba16float_image_storage':'two packed half2 words per texel; signed/HDR read/write and MSAA resolve',
                       'rgba16float_framebuffer_read':'per-primitive private snapshots; native 0x62 texture conversion; linear color',
                       'render_clear_cpu_pixel_edges':False,'render_clear_runtime_verified':False,
                       'native_resource_heaps':True,'heap_types':['automatic','placement'],'heap_storage_modes':['shared','private'],
                       'heap_backing':'one original IOGPU buffer per heap;64-byte suballocations and bounded resource-relative offsets rebased into backing',
                       'heap_resource_formats':'existing texture shapes, color/depth/stencil formats, mip/array/MSAA layouts and buffer views',
                       'heap_aliasing':'automatic makeAliasable permits reuse; unique lease tokens prevent stale frees; placement overlaps tracked independently',
                       'heap_lifetime':'resources and texture views retain allocation/heap/backing; commands retain resources through real native retirement',
                       'heap_size_limit':'native maxBufferLength, at most16MiB per backing; descriptor rounded to native page granularity',
                       'heap_purgeability':'existing KeepCurrent/NonVolatile only; no volatile/empty or sparse support',
                       'heap_runtime_verified':False,
                       'color_texture_mipmaps':True,'max_texture_mip_levels':15,'texture_mip_layout':'A5xx linear fdl5, 64-pixel base pitch alignment per format, aligned-base-pitch minification, padded last level, page-aligned layer',
                       'packed_color_formats':['r8unorm','rg8unorm'],'packed_color_render_blend':True,'packed_color_image_store':'per-word CAS preserves adjacent pixels and padding','packed_color_default_channels':'R8=(r,0,0,1),RG8=(r,g,0,1), including unspecified components of out-of-bounds reads',
                       'gpu_mipmap_generation':True,'srgb_color_formats':True,'srgb_compute_read_write':True,'srgb_render_targets':True,'srgb_cpu_clear_encoding':True,'srgb_alpha_linear':True,'mipmap_generation_filter':'per-level hardware bilinear sampling in linear color space, guarded image stores; sRGB RGB encode/decode and linear alpha',
                       'texture_mip_upload_readback_blit':True,'mip_render_attachments':True,'sampler_mip_filters':['none','nearest','linear'], 'sampler_address_modes':['clamp-to-edge','mirror-clamp-to-edge','repeat','mirror-repeat','clamp-to-zero','clamp-to-border-color'],
                       'sampler_max_anisotropy':16, 'sampler_pixel_coordinates':True, 'sampler_border_colors':['transparent-black','opaque-black','opaque-white'],
                       'sampler_explicit_level_ignores_lod_clamp':True, 'msl_gradient2d':True,
                       'sampler_lod_clamps':'unsigned 8.8 fixed-point; none clamps to 0.125 as fd5',
                       'shader_sampling_lod':['implicit','float32 explicit level','fragment float32 bias'],
                       'vertex_descriptor':True, 'vertex_attribute_locations':16, 'vertex_buffer_bindings':8, 'fragment_buffer_bindings':8,
                       'fragment_inline_bytes':4096, 'integer_atomics':True, 'atomic_memory_orders':['relaxed'], 'atomic_integer_bits':32,
                       'static_threadgroup_memory':True, 'max_threadgroup_memory_bytes':32768,
                       'threadgroup_barriers':True, 'dynamic_threadgroup_memory':True,
                       'dynamic_threadgroup_bindings':8,'threadgroup_alignment_bytes':16,
                       'threadgroup_constant_words':16,'threadgroup_allocation':'static prefix plus per-dispatch aligned lengths; combined <=32768 bytes',
                       'graphics_buffer_banks':{'vertex':[0,7],'fragment':[8,15]},
                       'vertex_fetch':'A540 VFD', 'indexed_draw':True, 'instanced_draw':True, 'private_index_snapshot':True,
                       'depth_stencil':True, 'depth_stencil_formats':['depth32float','stencil8','depth32float_stencil8'],
                       'depth_stencil_aspect_blits':True, 'varying_linkage':True, 'interpolation':['perspective','linear','flat'], 'shared_msl_frontend':True,
                       'runtime_llvm_exports_verified':sorted(llvm_symbols),
                       'full_metal_family_supported':False,
                       'native_command_version':int(re.search(r'\bCommandVersion\s*=\s*(\d+)',
                           (build_state.DRIVERS_ROOT/'include/GPUNativeProtocol.h').read_text())[1]),
                       'iosurface_import':True, 'iosurface_formats':['rgba8unorm','bgra8unorm','r8unorm','rg8unorm'],
                       'iosurface_pixel_formats':['RGBA','BGRA','L008','2C08','LA08'],
                       'iosurface_la08_mapping':'rg8unorm; preserve bytes and caller swizzle; QuartzCore shader interprets gray/alpha',
                       'iosurface_mask_fourcc':['L008','2C08','LA08'], 'iosurface_metadata_version':protocol_versions['Surface'],
                       'iosurface_planes':[0], 'iosurface_copy_required':False,
                       'client_buffer_import':True, 'client_buffer_storage_mode':'shared',
                       'client_buffer_cache_mode':'default', 'client_buffer_requires':'native page-aligned writable whole allocation',
                       'client_buffer_copy_required':False, 'client_buffer_runtime_verified':False,
                       'native_admission':'asynchronous except guarded direct-drawable real admission', 'native_completion_version':1,
                       'native_private_address_spaces':True, 'native_address_space_lifetime':'per-submission',
                       'scheduled_handler_after_native_admission':True,
                       'completion_source':'Family end token after A540 timestamp/cache/IOSurface retirement',
                       'frameworks':['IOKit', 'CoreFoundation', *frameworks],
                       'exports':['QcomMTLCreateDevice', 'QcomMTLCreateSystemDefaultDevice', 'QcomMTLEncodeComposition']})
    for library in libraries:
        report['inputs'][str(library.relative_to(build_state.ROOT))] = _component_native.digest(library)
    if protocol:
        for name in ([protocol] if isinstance(protocol, str) else protocol):
            path = build_state.DRIVERS_ROOT / 'include' / name
            report['inputs'][str(path.relative_to(build_state.ROOT))] = _component_native.digest(path)
    Path(str(output)+'.json').write_text(json.dumps(report,indent=2)+'\n')
    print('Built arm64 iOS ' + ('Metal adapter: ' if bundle else 'IOKit client: ') + str(output))
