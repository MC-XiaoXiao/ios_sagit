#!/usr/bin/env python3
"""Freeze, build and compare the offline AIR frontends. No GPU/device access."""
import argparse
import datetime
import hashlib
import json
import os
import platform
import shutil
import subprocess
import sys
from pathlib import Path

SOURCE = Path(__file__).resolve().parent
ROOT = SOURCE.parents[2]


def digest(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def save(path, value):
    path.write_text(json.dumps(value, indent=2, ensure_ascii=False) + '\n')


def freeze(work):
    snapshot, inputs = work / 'snapshot', work / 'inputs'
    snapshot.mkdir(parents=True, exist_ok=True)
    inputs.mkdir(exist_ok=True)
    originals = {}
    compiler_files = ['AIRRuntime.cpp', 'AIRSpecialization.h', 'AIRControlFlow.h',
                      'AIRNIR.h', 'AIRNIR.cpp', 'AIRNIRLowering.h',
                      'Compiler.cpp', 'Compiler.h', 'FunctionConstants.h',
                      'Semantic.h', 'Backend.c', 'Interface.h']
    protocol_files = ['GPUJITProtocol.h', 'GPUShaderInterface.h',
                      'GPUShaderProtocol.h', 'GPUUserProtocol.h']
    for folder, names in [('tools/gpu-compiler', compiler_files),
                          ('drivers/include', protocol_files)]:
        for name in names:
            src = ROOT / folder / name
            originals[str(src)] = digest(src)
            dst = snapshot / name
            if not dst.exists():
                shutil.copyfile(src, dst)
    sources = {
        'compute.air.ll': ROOT / 'drivers/gpu/QcomMTL/Shaders.air.ll',
        'welcome.air': ROOT / 'build/drivers/gpu-native-fragment/fixed_frag_lph_cpf.air',
        'welcome.constants.txt': ROOT / 'build/gpu-runtime/ui-framebuffer-input-snapshot/hot-constants.txt',
        'fluidity.air': ROOT / 'build/gpu-runtime/ui-cold-compile-shared-nir/native-fluidity/fragmentShader.air',
    }
    for name in ['Shaders','Textures','IntegerTextures','SampleShading','ConstantWindows']:
        sources[name+'.air.ll'] = ROOT/'drivers/gpu/QcomMTL'/(name+'.air.ll')
    for name in ['packed-fluidity','static-switch','direct-nir-boundaries','direct-nir-remaining']:
        sources[name+'.air.ll'] = ROOT/'tests/fixtures/gpu'/(name+'.air.ll')
    for name, src in list(sources.items()):
        if not src.exists():
            frozen = ROOT/'build/air-nir-runtime-20261020/candidate-01/validation/inputs'/name
            if frozen.exists(): sources[name] = src = frozen
        if not (inputs / name).exists():
            shutil.copyfile(src, inputs / name)
    constants = inputs / 'fluidity.constants.txt'
    if not constants.exists():
        constants.write_text('0 53 00\n1 53 00\n2 53 00\n')
    if not (inputs / 'compute.air').exists():
        subprocess.run(['llvm-as', str(inputs / 'compute.air.ll'), '-o', str(inputs / 'compute.air')], check=True)
    receipt = {
        'snapshot': {p.name: digest(p) for p in sorted(snapshot.iterdir()) if p.is_file()},
        'inputs': {p.name: {'sha256': digest(p), 'source': str(sources[p.name]) if p.name in sources else
                  'llvm-as compute.air.ll' if p.name == 'compute.air' else 'three boolean function constants set to false'}
                   for p in sorted(inputs.iterdir()) if p.is_file()},
        'hardware_tested': False,
    }
    save(work / 'source-receipt.json', receipt)
    return originals


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--output', type=Path, default=ROOT / 'build' / ('air-nir-validation-' + datetime.date.today().strftime('%Y%m%d')))
    parser.add_argument('--cpu', type=int, help='pin benchmark subprocesses to this allowed CPU')
    parser.add_argument('--differential-only', action='store_true', help='run semantic/guard checks without the 31-repeat benchmarks')
    parser.add_argument('--runtime-compiler', type=Path, help='validate this host compiler library instead of the default build')
    parser.add_argument('--save-semantic-outputs', action='store_true', help='save exact quad outputs and compute buffers for comparisons between builds')
    args = parser.parse_args()
    work = args.output.resolve()
    work.mkdir(parents=True, exist_ok=True)
    originals = freeze(work)
    environment = dict(os.environ)
    if args.runtime_compiler:
        environment['AIR_NIR_RUNTIME_COMPILER'] = str(args.runtime_compiler.resolve())
    print('Building isolated compiler...', flush=True)
    with (work / 'build-driver.log').open('w') as log:
        subprocess.run([sys.executable, str(SOURCE / 'build.py'), str(work)], check=True, stdout=log, stderr=subprocess.STDOUT, env=environment)
    allowed = sorted(os.sched_getaffinity(0))
    cpu = args.cpu if args.cpu is not None else allowed[len(allowed) // 2]
    if cpu not in allowed:
        raise ValueError(f'CPU {cpu} is not allowed: {allowed}')
    # Only the benchmark child is pinned; the system governor is untouched.
    def pin():
        os.sched_setaffinity(0, {cpu})
    environment['AIR_NIR_RUNTIME_COMPILER'] = str(work / 'QcomGPUCompiler.so')
    if args.save_semantic_outputs:
        environment['AIR_NIR_SEMANTIC_SNAPSHOT'] = str(work / 'semantic-outputs.bin')
    for label in ['checks', 'diff', 'features', 'remaining'] + ([] if args.differential_only else ['compute', 'welcome', 'fluidity']):
        print(f'Running {label} on CPU {cpu}...', flush=True)
        with (work / (label + '.run.log')).open('w') as log:
            subprocess.run([str(work / 'benchmark'), str(work), label], check=True,
                           stdout=log, stderr=subprocess.STDOUT, preexec_fn=pin, env=environment)
    differential = json.loads((work / 'differential.json').read_text())
    if not differential['passed'] or not all(row['runtime_jit_image_identical'] for row in differential['cases']):
        raise RuntimeError('Semantic outputs or actual production compiler images differ')
    unchanged = all(digest(Path(p)) == sha for p, sha in originals.items())
    if not unchanged:
        raise RuntimeError('Production inputs changed during validation; rerun with a stable snapshot')
    differential['production_files_unchanged'] = unchanged
    differential['runtime_compiler_sha256'] = digest(work / 'QcomGPUCompiler.so')
    save(work / 'differential.json', differential)
    rows = differential['cases']
    differential_lines = ['# AIR → NIR 输出差分', '',
        '执行两条路径在 IR3 finalize 后的 NIR；CPU quad 解释器采用 Mesa ALU 运算语义，纹理采用确定性的非恒定数据。没有执行 A540 GPU。', '',
        '| 样本 | profile | quad 数 | 差异数 | 最大绝对误差 | 生产 JIT 与被测路径一致 |',
        '| --- | ---: | ---: | ---: | ---: | --- |']
    for row in rows:
        differential_lines.append(f"| {row['name']} | {row['profile']} | {row['quad_cases']} | {row['mismatches']} | {row['max_absolute_error']} | 是 |")
    differential_lines += ['', f"共 {sum(row['quad_cases'] for row in rows)} quads / {4*sum(row['quad_cases'] for row in rows)} lanes，零容差。Compute 另以独立 add7 期望值验证完整缓冲区和 guards；图形输出注入 +0.25 必须能检测差异。", '',
        '覆盖 base/MSAA/window/MSAA-window、半精度、四种常量窗口状态、短 buffer、1/2/4 sample framebuffer，以及 Fluidity 三个布尔常量的全部八种组合。生产主机编译库与被解释 NIR 所生成的完整 JIT image 逐字节一致。', '',
        '这验证当前样本的编译语义与生产库接线；实际纹理硬件、IR3 执行、像素量化和 Snapdragon 性能仍由真机任务验证。', '']
    (work / 'DIFFERENTIAL.md').write_text('\n'.join(differential_lines))
    if args.differential_only:
        print(work / 'DIFFERENTIAL.md')
        return
    cases = [json.loads((work / (label + '.result.json')).read_text()) for label in ['compute', 'welcome', 'fluidity']]
    for case in cases:
        baseline, direct = case['routes']
        case['cpu_reduction_percent'] = 100 * (1 - direct['median_cpu_ms'] / baseline['median_cpu_ms'])
        case['variant_comparison'] = [
            {'variant': name, 'glsl': a, 'direct': b,
             'code_bytes_delta': 4 * (b['code_words'] - a['code_words']),
             'full_regs_delta': b['full_regs'] - a['full_regs'],
             'half_regs_delta': b['half_regs'] - a['half_regs']}
            for name, a, b in zip(['base', 'msaa', 'window', 'msaa-window'], baseline['variants'], direct['variants'])
            if a['code_words'] or b['code_words']
        ]
    unchanged = all(digest(Path(p)) == sha for p, sha in originals.items())
    if not unchanged:
        raise RuntimeError('Production inputs changed during the comparison; rerun with a stable snapshot')
    labels = ['compute', 'welcome', 'fluidity']
    artifacts = {p.name: digest(p) for p in sorted(work.iterdir())
                 if p.is_file() and p.suffix in {'.asm', '.bin', '.image', '.glsl', '.nir'}
                 and any(p.name.startswith(label + '.') for label in labels)
                 and '.invalid.' not in p.name}
    report = {
        'created_utc': datetime.datetime.now(datetime.timezone.utc).isoformat(),
        'scope': 'In-process shader compilation from AIR bytes with decode-template cache and resolved function constants through all production A540 variants and JIT validation. Input file I/O, process startup, metallib extraction, MSL compilation, GPU upload and pipeline creation are outside the timed interval.',
        'samples': {
            'compute': 'Authored air_add kernel in QcomMTL/Shaders.air.ll, assembled to bitcode; not a captured Apple compute shader.',
            'welcome': 'One captured QuartzCore fixed_frag_lph_cpf hot specialization; not every Welcome shader or the entire animation.',
            'fluidity': 'Captured fluidity::fragmentShader, constants 0/1/2=false; not every Fluidity specialization or vertex/SDF shaders.',
        },
        'platform': platform.platform(), 'machine': platform.machine(),
        'cpu_model': next((line.split(':', 1)[1].strip() for line in Path('/proc/cpuinfo').read_text().splitlines() if line.startswith('model name')), 'unknown'),
        'benchmark_cpu_affinity': [cpu], 'repeats_per_route': 31,
        'shader_caches': 'Bounded AIR decode-template cache enabled; specialization and IR3 compilation repeated per call with IR3 cache disabled; one untimed warmup per route; route order alternates.',
        'llvm_version': subprocess.check_output(['llvm-config', '--version'], text=True).strip(),
        'mesa': json.loads((ROOT / 'tools/gpu-compiler/Source.json').read_text())['commit'],
        'prototype_sources_sha256': {p.name: digest(p) for p in sorted(SOURCE.iterdir()) if p.is_file()},
        'production_files_unchanged': unchanged, 'hardware_tested': False,
        'semantic_equivalence_tested': True,
        'semantic_executor': differential['executor'], 'differential': differential,
        'checks': (work / 'checks.run.log').read_text(),
        'limitations': ['Sample outputs are compared by a CPU finalized-NIR interpreter. Actual A540 pixels and compute buffers have not been executed or compared.',
                        'Register numbers are exported IR3 max register counts in vec4 units, not measured occupancy.',
                        'Host compile timings do not predict Snapdragon 835 CPU milliseconds or steady-state FPS.'],
        'cases': cases, 'artifacts_sha256': artifacts,
    }
    save(work / 'report.json', report)
    lines = ['# AIR → NIR 离线原型实测', '',
             '三组均从 AIR 开始计时，包含函数常量特化、完整后端及全部变体。每路预热一次，31 次无着色器缓存编译，交替顺序，固定 CPU 核；下表使用线程 CPU 中位数。', '',
             '| 样本 | GLSL 路径 ms | NIR 直转 ms | 减少 | 倍率 |', '| --- | ---: | ---: | ---: | ---: |']
    for case in cases:
        a, b = case['routes']
        lines.append(f"| {case['case']} | {a['median_cpu_ms']:.3f} | {b['median_cpu_ms']:.3f} | {case['cpu_reduction_percent']:.1f}% | {case['cpu_speedup']:.2f}× |")
    lines += ['', '| 样本/变体 | 机器码字节 GLSL → NIR | fullRegs GLSL → NIR | halfRegs GLSL → NIR |',
              '| --- | ---: | ---: | ---: |']
    for case in cases:
        for row in case['variant_comparison']:
            a, b = row['glsl'], row['direct']
            lines.append(f"| {case['case']}/{row['variant']} | {a['code_words']*4} → {b['code_words']*4} | {a['full_regs']} → {b['full_regs']} | {a['half_regs']} → {b['half_regs']} |")
    lines += ['', 'Compute 是项目自写的 air_add；Welcome 是一个真实 hot fragment 特化；Fluidity 是主 fragment 的三个布尔常量均 false 的特化。这里的“完整”是单个函数 AIR 到全部机器码变体，不是整个应用启动或所有 pipeline。', '',
              '通过 NIR 结构校验、生产 JIT 校验、资源接口一致性、31 次输出确定性检查和离线拒绝/边界检查。另通过 finalized NIR 输出差分与实际生产库 JIT 逐字节核对，详见 DIFFERENTIAL.md。未执行 GPU。', '',
              '直接路径支持这三组所需的半精度、只读/可写 buffer、2D 采样、常量 sampler、附件读取、无环条件分支和 PHI。循环、原子、volatile、更多纹理形态和 vertex 接口由运行时回退至 GLSL；本基准直转失败就报错。', '',
              '当前收益集中在无缓存编译延迟。机器码大小与寄存器没有一致改善，不能宣称稳定帧率提高。运行时保留 GLSL 回退；部署和实际帧率由真机验证另行确认。', '',
              '完整墙钟/CPU 样本、阶段耗时、全部变体、哈希与环境见 report.json；每个变体均保留 .bin 与 .asm。测试不包含文件读取、进程启动、MSL 编译、metallib 提取、GPU 上传或管线对象创建。', '']
    (work / 'REPORT.md').write_text('\n'.join(lines))
    print(work / 'REPORT.md')


if __name__ == '__main__':
    main()
