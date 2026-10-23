"""Offline checks of the Apple Kext, iOS transport and pinned model evidence."""
import hashlib
import importlib.util
import json
import os
from pathlib import Path
import plistlib
import struct
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
spec = importlib.util.spec_from_file_location('gpu_toolchain', ROOT / 'scripts/toolchain.py')
toolchain = importlib.util.module_from_spec(spec)
spec.loader.exec_module(toolchain)


class GPUIntegrationTests(unittest.TestCase):
    def test_kext_uses_apple_matching_and_declared_kpis(self):
        source = ROOT / 'drivers/gpu/QcomGPU/Info.plist'
        bundle = Path(os.environ.get('QCOM_GPU_KEXT_BUNDLE', ROOT / 'build/output/Extensions/QcomGPU.kext')) / 'Contents'
        self.assertEqual(source.read_bytes(), (bundle / 'Info.plist').read_bytes())
        info = plistlib.loads(source.read_bytes())
        p = info['IOKitPersonalities']['Adreno']
        self.assertEqual(p['IOProviderClass'], 'AppleARMIODevice')
        self.assertEqual(p['IOClass'], 'QcomGPU')
        self.assertEqual(p['IONameMatch'], ['qcom,adreno-540.1'])
        self.assertEqual(p['MetalPluginName'], 'QcomMTLDriver')
        self.assertEqual(p['MetalPluginClassName'], 'QcomMTLDevice')
        binary = toolchain.driver_macho(bundle / 'MacOS/QcomGPU')
        self.assertIn('__ZN7QcomGPU10gMetaClassE', binary['defined'])
        self.assertIn('__ZN17QcomGPUUserClient10gMetaClassE', binary['defined'])
        self.assertFalse(any('AGX' in s for s in binary['imports']))
        self.assertIn('__ZN5IOGPU10gMetaClassE', binary['imports'])
        self.assertEqual(p['IOMatchCategory'], 'IOAcceleratorES')
        dependency=p['IOResourceMatch']
        self.assertEqual(dependency['IOProviderClass'],'IOResources')
        self.assertEqual(dependency['IOResourceMatch'],['GCCControllerReady','RPMControllerReady','GPUCCControllerReady','SCMControllerReady'])
        self.assertEqual(info['OSBundleLibraries']['org.darwin.driver.QcomSCM'], '1.0.0')
        secure = ROOT/'build/output/Extensions/QcomSCM.kext/Contents'
        secure_info = plistlib.loads((secure/'Info.plist').read_bytes())
        self.assertEqual(secure_info, plistlib.loads((ROOT/'drivers/security/QcomSCM/Info.plist').read_bytes()))
        personality = secure_info['IOKitPersonalities']['SecureFirmware']
        self.assertEqual(personality['IOProviderClass'], 'AppleARMIODevice')
        self.assertEqual(personality['IONameMatch'], ['qcom,scm-msm8998'])
        self.assertIn('__ZN7QcomSCM10gMetaClassE', toolchain.driver_macho(secure/'MacOS/QcomSCM')['defined'])
        self.assertEqual((secure/'Resources/NOTICE.txt').read_bytes(), (ROOT/'drivers/security/QcomSCM/NOTICE.txt').read_bytes())
        self.assertEqual(info['OSBundleLibraries']['com.apple.iokit.IOGPUFamily'], '2.0.0')
        self.assertEqual(info['OSBundleLibraries']['com.apple.iokit.IOSurface'], '1.0.0')
        self.assertIn('__ZN5IOGPU24nativeCopySurfaceForTaskEjP4task', binary['imports'])
        self.assertIn('__ZN9IOSurface18nativeMarkGPUWriteEjj', binary['imports'])
        self.assertIn('__ZN17IOGPUEventMachine17nativeSignalStampEjj', binary['imports'])
        self.assertIn('__ZN17IOGPUFenceMachine12nativeRetireEv', binary['imports'])
        self.assertIn('__ZN13QcomGPUBuffer10gMetaClassE', binary['defined'])
        self.assertIn('__ZN13QcomGPUMapper10gMetaClassE', binary['defined'])
        self.assertIn('__ZN8IOMapper10gMetaClassE', binary['imports'])
        self.assertEqual((bundle/'Resources/NOTICE.SMMU.txt').read_bytes(),(ROOT/'drivers/gpu/QcomGPU/NOTICE.SMMU.txt').read_bytes())
        for interface in ('IODMACommand', 'IOMapper', 'IOInterruptEventSource', 'IOTimerEventSource'):
            self.assertTrue(any(interface in s for s in binary['imports']), interface)

    def test_real_ios_user_transport_and_input_hashes(self):
        binary = ROOT / 'build/output/gpuctl'
        data = binary.read_bytes()
        report = json.loads(Path(str(binary) + '.json').read_text())
        self.assertEqual(report['sha256'], hashlib.sha256(data).hexdigest())
        self.assertFalse(report['hardware_tested'])
        self.assertIn('IOConnectCallMethod', report['iokit_exports_verified'])
        for relative, digest in report['inputs'].items():
            self.assertEqual(hashlib.sha256((ROOT / relative).read_bytes()).hexdigest(), digest)
        magic, cpu, subtype, kind, ncmds, size, _, _ = struct.unpack_from('<8I', data)
        self.assertEqual((magic, cpu, subtype, kind), (0xfeedfacf, 0x100000c, 0, 2))
        pos = 32; libraries = []; ios = False
        for _ in range(ncmds):
            cmd, length = struct.unpack_from('<II', data, pos)
            self.assertGreaterEqual(length, 8)
            self.assertLessEqual(pos + length, 32 + size)
            if cmd == 0xc:
                offset = struct.unpack_from('<I', data, pos + 8)[0]
                libraries.append(data[pos + offset:pos + length].split(b'\0')[0].decode())
            if cmd == 0x32:
                ios = struct.unpack_from('<I', data, pos + 8)[0] == 2
            pos += length
        self.assertEqual(pos, 32 + size)
        self.assertTrue(ios)
        self.assertTrue(any('IOKit.framework' in name for name in libraries))
        self.assertTrue(any('CoreFoundation.framework' in name for name in libraries))
        self.assertIn('/usr/lib/libSystem.B.dylib', libraries)

    def test_pinned_hardware_references(self):
        directory = ROOT / 'docs/gpu/reference'
        sources = json.loads((directory / 'sources.json').read_text())
        for name, source in sources.items():
            self.assertEqual(hashlib.sha256((directory / name).read_bytes()).hexdigest(), source['sha256'])
        catalog = (directory / 'a5xx_catalog.c').read_text()
        self.assertIn('0x05040001', catalog)
        self.assertIn('a540_gpmu.fw2', catalog)
        self.assertIn('a540_zap.mdt', catalog)
        dt = (directory / 'msm8998.dtsi').read_text()
        self.assertIn('reg = <0x05000000 0x40000>', dt)
        self.assertIn('GIC_SPI 300', dt)

    def test_metal_adapter_is_native_plugin_with_explicit_factory(self):
        bundle = ROOT / 'build/output/QcomMTLDriver.bundle'
        binary = bundle / 'QcomMTLDriver'
        data = binary.read_bytes()
        report = json.loads(Path(str(binary) + '.json').read_text())
        self.assertEqual(report['sha256'], hashlib.sha256(data).hexdigest())
        self.assertFalse(report['hardware_tested'])
        self.assertFalse(report['system_metal_plugin_registered'])
        self.assertTrue(report['system_metal_plugin_declared'])
        self.assertTrue(report['system_metal_plugin_entry_implemented'])
        self.assertFalse(report['system_metal_load_verified'])
        self.assertEqual(report['device_superclass'], '_MTLDevice')
        self.assertEqual(report['system_entry_selector'], 'initWithAcceleratorPort:')
        self.assertEqual(report['native_limits_bytes'], 264)
        self.assertIn('IOObjectRetain', report['iokit_exports_verified'])
        self.assertTrue(report['iosurface_import'])
        self.assertEqual(report['iosurface_planes'], [0])
        self.assertIn('IOSurface', report['frameworks'])
        self.assertFalse(report['iosurface_copy_required'])
        self.assertEqual(report['native_admission'], 'asynchronous except guarded direct-drawable real admission')
        self.assertTrue(report['native_direct_drawable_commit_admission'])
        self.assertEqual(report['native_completion_version'], 1)
        self.assertTrue(report['native_private_address_spaces'])
        self.assertEqual(report['native_address_space_lifetime'], 'per-submission')
        self.assertTrue(report['scheduled_handler_after_native_admission'])
        self.assertTrue(report['shader_compilation'])
        self.assertEqual(report['shader_compilation_mode'], 'aot-and-runtime-jit-msl-air-profile')
        self.assertTrue(report['air_compilation'])
        self.assertTrue(report['metallib_import'])
        self.assertTrue(report['runtime_air_compilation'])
        self.assertTrue(report['runtime_msl_compilation'])
        self.assertEqual(report['jit_version'], 39)
        self.assertEqual(report['program_description_bytes'], 2468)
        self.assertEqual(report['jit_program_image_bytes'], 83720)
        self.assertTrue(report['constant_samplers'])
        self.assertTrue(report['multiple_color_attachments'])
        self.assertEqual(report['max_color_attachments'], 8)
        self.assertEqual(report['hardware_sampler_slots'], 16)
        self.assertEqual(report['constant_sampler_states_max'], 16)
        self.assertEqual(report['shader_command_version'], 44)
        self.assertEqual(plistlib.loads((bundle/'Info.plist').read_bytes())['QcomMTLShaderCommandVersion'],44)
        self.assertEqual(plistlib.loads((bundle/'Info.plist').read_bytes())['QcomMTLJITVersion'],39)
        self.assertTrue(report['graphics_texture_fetch'])
        self.assertTrue(report['graphics_texture_queries'])
        self.assertTrue(report['vertex_texture_sampling'])
        self.assertEqual(report['vertex_texture_bindings'], 16)
        self.assertEqual(report['fragment_texture_bindings'], 16)
        self.assertTrue(report['client_buffer_import'])
        self.assertFalse(report['client_buffer_copy_required'])
        self.assertFalse(report['client_buffer_runtime_verified'])
        self.assertTrue(report['graphics_independent_sampler_pairs'])
        self.assertTrue(report['cube_textures'])
        self.assertTrue(report['cube_face_blit_render_mipmap'])
        self.assertTrue(report['cube_image_operations'])
        self.assertTrue(report['cube_image_queries'])
        self.assertTrue(report['cube_image_read_write'])
        self.assertTrue(report['cube_air_intrinsics_verified'])
        self.assertTrue(report['air_texture_current_read_sampler'])
        self.assertTrue(report['vertex_fragment_texture_bindings_independent'])
        self.assertTrue(report['texture_arrays'])
        self.assertTrue(report['texture_array_slice_views'])
        self.assertTrue(report['texture_views'])
        self.assertTrue(report['texture_view_mip_subranges'])
        self.assertTrue(report['texture_view_parent_retained'])
        self.assertTrue(report['texture_view_arbitrary_swizzle'])
        self.assertTrue(report['texture_swizzle_composition'])
        self.assertEqual(report['compute_texture_bindings'],8)
        self.assertEqual(report['fragment_buffer_bindings'],8)
        self.assertEqual(report['fragment_inline_bytes'],4096)
        self.assertEqual(report['max_threadgroup_memory_bytes'],32768)
        self.assertTrue(report['threadgroup_barriers'])
        self.assertTrue(report['dynamic_threadgroup_memory'])
        self.assertTrue(report['gpu_mipmap_generation'])
        self.assertTrue(report['texture_mip_upload_readback_blit'])
        self.assertEqual(report['dynamic_threadgroup_bindings'],8)
        self.assertEqual(report['threadgroup_alignment_bytes'],16)
        self.assertTrue(report['integer_atomics'])
        self.assertTrue(report['vertex_descriptor'])
        self.assertEqual(report['vertex_fetch'],'A540 VFD')
        self.assertTrue(report['indexed_draw'])
        self.assertTrue(report['instanced_draw'])
        self.assertTrue(report['private_index_snapshot'])
        self.assertTrue(report['depth_stencil'])
        self.assertTrue(report['depth_stencil_aspect_blits'])
        self.assertEqual(len(report['depth_stencil_formats']),3)
        self.assertTrue(report['varying_linkage'])
        self.assertEqual(set(report['interpolation']),{'perspective','linear','flat'})
        self.assertTrue(report['shared_msl_frontend'])
        self.assertEqual(set(report['render_state_bridge']),{'blend','color-write-mask','viewport','cull','front-winding','depth-range','depth-stencil'})
        self.assertTrue(report['runtime_llvm_exports_verified'])
        self.assertIn('real A540 code', toolchain.test_gpu_compiler())
        compiler = bundle / 'QcomGPUCompiler.dylib'
        self.assertEqual(struct.unpack_from('<4I',compiler.read_bytes()),(0xfeedfacf,0x100000c,0,6))
        self.assertTrue(report['render_compute'])
        self.assertTrue(report['compute_variable_workgroups'])
        self.assertTrue(report['bounded_buffer_access'])
        self.assertEqual(report['inline_compute_vertex_bytes'],4096)
        self.assertEqual(report['compute_grid_dimensions'],3)
        self.assertEqual(report['compute_max_threads_per_group'],1024)
        self.assertTrue(report['compute_nonuniform_workgroups'])
        self.assertFalse(report['full_metal_family_supported'])
        self.assertEqual(report['native_command_version'], 4)
        generated = ROOT / 'build/drivers/generated/GPU'
        shaders = json.loads((generated / 'shaders.json').read_text())
        library = json.loads((bundle / 'default.qmetallib').read_text())
        self.assertEqual(library['registry'], shaders['registry'])
        self.assertEqual((bundle / 'default.qmetallib').read_bytes(), (generated / 'default.qmetallib').read_bytes())
        self.assertEqual({p['metadata']['stage'] for p in shaders['programs']}, {0, 1, 2})
        self.assertEqual({p['source_format'] for p in shaders['programs']}, {'msl','air'})
        for name in ('Shaders.air', 'Textures.air'):
            binary_hash = hashlib.sha256((bundle / name).read_bytes()).hexdigest()
            matches = [entry for entry in library['binaries'] if entry['sha256'] == binary_hash]
            self.assertEqual(len(matches), 1)
            self.assertEqual(matches[0]['format'], 'air')
            self.assertTrue(matches[0]['names'])
        for p in shaders['programs']:
            self.assertEqual(len(p['code']), p['metadata']['codeWords'])
            self.assertGreater(p['metadata']['instrlen'], 0)
        for name, digest in shaders['generated_sha256'].items():
            self.assertEqual(hashlib.sha256((generated / name).read_bytes()).hexdigest(), digest)
        for relative, digest in shaders['inputs'].items():
            self.assertEqual(hashlib.sha256((ROOT / relative).read_bytes()).hexdigest(), digest)
        # Reuse the small SDK library to check a real Apple MTLB container and
        # its AIR hashes; the CoreImage extern functions are not stage shaders.
        sdk_library = ROOT / 'tools/sdk/iPhoneOS18.6.sdk/System/Library/Frameworks/CoreImage.framework/CoreImage.metallib'
        container = bytearray(sdk_library.read_bytes())
        thin = toolchain.extract_gpu_metallib(bytes(container))
        self.assertTrue(thin)
        # A native universal Metal library can carry AIR64 beside a GPU
        # executable. Select the portable slice and preserve its exact bytes.
        fat = struct.pack('>II5I',0xcafebabe,1,0x01000017,11,32,len(container),3)+bytes(4)+container
        self.assertEqual([(f['name'],f['data']) for f in toolchain.extract_gpu_metallib(fat)],
                         [(f['name'],f['data']) for f in thin])
        malformed = bytearray(fat)
        struct.pack_into('>I',malformed,16,8)
        with self.assertRaisesRegex(RuntimeError,'FAT slice'):
            toolchain.extract_gpu_metallib(bytes(malformed))
        bitcode_offset = struct.unpack_from('<Q',container,72)[0]
        container[bitcode_offset+32] ^= 1
        with self.assertRaisesRegex(RuntimeError, 'hash mismatch'):
            toolchain.extract_gpu_metallib(bytes(container))
        with tempfile.TemporaryDirectory() as directory:
            source = (ROOT / 'drivers/gpu/QcomMTL/Shaders.air.ll').read_text()
            source = source.replace('%wide = zext i32 %index to i64', '%shifted = add i32 %index, 1\n  %wide = zext i32 %shifted to i64')
            path = Path(directory) / 'shifted.ll'; path.write_text(source)
            result = subprocess.run([ROOT / 'build/gpu-compiler/air-frontend', path, Path(directory) / 'output.json'],capture_output=True,text=True)
            self.assertEqual(result.returncode,0,result.stderr)
            self.assertTrue((Path(directory) / 'output.json').exists())
        self.assertEqual((bundle / 'Info.plist').read_bytes(),
                         (ROOT / 'drivers/gpu/QcomMTL/Info.plist').read_bytes())
        for relative, digest in report['inputs'].items():
            self.assertEqual(hashlib.sha256((ROOT / relative).read_bytes()).hexdigest(), digest)
        h = struct.unpack_from('<8I', data)
        self.assertEqual(h[:4], (0xfeedfacf, 0x100000c, 0, 6))
        pos = 32; libraries = []; symbols = None; ios = False
        for _ in range(h[4]):
            cmd, length = struct.unpack_from('<II', data, pos)
            self.assertGreaterEqual(length, 8)
            self.assertLessEqual(pos + length, 32 + h[5])
            if cmd == 0xc:
                offset = struct.unpack_from('<I', data, pos + 8)[0]
                libraries.append(data[pos + offset:pos + length].split(b'\0')[0].decode())
            elif cmd == 0x32:
                ios = struct.unpack_from('<I', data, pos + 8)[0] == 2
            elif cmd == 2:
                symbols = struct.unpack_from('<4I', data, pos + 8)
            pos += length
        self.assertEqual(pos, 32 + h[5])
        self.assertTrue(ios)
        for framework in ('Metal', 'Foundation', 'IOKit', 'CoreFoundation'):
            self.assertTrue(any(framework + '.framework/' in name for name in libraries), framework)
        self.assertIsNotNone(symbols)
        symoff, count, stroff, strsize = symbols
        exports = set(); imports = set()
        for i in range(count):
            string, kind, _, _, _ = struct.unpack_from('<IBBHQ', data, symoff + i * 16)
            if string and kind == 0x0f:
                end = data.index(b'\0', stroff + string, stroff + strsize)
                exports.add(data[stroff + string:end].decode())
            elif string and kind == 0x01:
                end = data.index(b'\0', stroff + string, stroff + strsize)
                imports.add(data[stroff + string:end].decode())
        self.assertIn('_QcomMTLCreateDevice', exports)
        self.assertIn('_QcomMTLCreateSystemDefaultDevice', exports)
        self.assertIn('_QcomMTLEncodeComposition', exports)
        self.assertIn('_OBJC_CLASS_$_QcomMTLDevice', exports)
        self.assertNotIn('_MTLCreateSystemDefaultDevice', exports)
        self.assertIn('_OBJC_CLASS_$__MTLDevice', imports)
        self.assertIn('_OBJC_METACLASS_$__MTLDevice', imports)
        info = plistlib.loads((bundle/'Info.plist').read_bytes())
        self.assertEqual(info['NSPrincipalClass'], 'QcomMTLDevice')
        staging = ROOT/'build/output/gpu-rootfs'
        manifest = json.loads((staging/'manifest.json').read_text())
        self.assertFalse(manifest['system_load_verified'])
        for name, expected in manifest['files'].items():
            self.assertEqual(hashlib.sha256((staging/name).read_bytes()).hexdigest(), expected)
        # The system probe must start without first loading this driver.
        probe_imports = subprocess.check_output([toolchain.host('llvm-nm'), '--undefined-only',
                                               ROOT/'build/output/metalctl'], text=True).split()
        self.assertIn('_MTLCreateSystemDefaultDevice', probe_imports)
        self.assertNotIn('_QcomMTLCreateSystemDefaultDevice', probe_imports)
        self.assertNotIn(b'@rpath/QcomMTLDriver.bundle/QcomMTLDriver', (ROOT/'build/output/metalctl').read_bytes())

    def test_native_model_is_tied_to_the_inspected_image(self):
        plugin = json.loads((ROOT/'drivers/abi-22H374/metal-plugin-abi.json').read_text())
        self.assertFalse(plugin['runtime_verified'])
        for item in plugin['images'].values():
            self.assertEqual(hashlib.sha256((ROOT/item['path']).read_bytes()).hexdigest(), item['sha256'])
        metal = ROOT/plugin['images']['Metal']['path']
        for function in plugin['functions']:
            code = bytearray()
            for offset in range(0, function['size'], 256):
                code += toolchain.provider_function_bytes(metal, int(function['address'], 0)+offset,
                                                          min(256, function['size']-offset))
            self.assertEqual(hashlib.sha256(code).hexdigest(), function['sha256'])
        model = json.loads((ROOT / 'docs/gpu/ios18-model.json').read_text())
        self.assertEqual(hashlib.sha256((ROOT / model['image']).read_bytes()).hexdigest(), model['image_sha256'])
        inventory = json.loads((ROOT / 'build/ios18-kext-analysis/prelink-info.json').read_text())
        by_id = {x['CFBundleIdentifier']: x for x in inventory['_PrelinkInfoDictionary']}
        for identifier, properties in model['bundles'].items():
            for key, value in properties.items():
                self.assertEqual(by_id[identifier][key], value)
        self.assertEqual(model['classes']['IOGPU']['super'], 'IOAcceleratorES')
        self.assertEqual(model['classes']['IOGPU']['vtable_slots'], 215)


if __name__ == '__main__':
    unittest.main()
