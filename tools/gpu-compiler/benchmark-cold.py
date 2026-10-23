#!/usr/bin/env python3
"""Compare frozen host compiler libraries without shader caches or device I/O.

Requires the project's existing generated GLSL and captured iOS 18 AIR fixtures.
Each library runs in its own process, pinned to the same allowed CPU. Timings
exclude input reads, process/library startup, IPC and output hashing.
"""
import argparse
import ctypes as C
import hashlib
import json
import os
from pathlib import Path
import platform
import shutil
import statistics
import subprocess
import sys
import time

ROOT = Path(__file__).resolve().parents[2]


def digest(data):
    return hashlib.sha256(data).hexdigest()


def worker(args):
    os.sched_setaffinity(0, {args.cpu})
    library = C.CDLL(str(args.worker.resolve()))
    glsl = library.qgpu_compile_glsl
    glsl.argtypes = [C.c_char_p, C.c_uint, C.c_void_p, C.c_void_p, C.c_uint]
    glsl.restype = C.c_int

    class Constant(C.Structure):
        _fields_ = [('index', C.c_uint32), ('type', C.c_uint32), ('bytes', C.c_uint8 * 32)]

    class Timing(C.Structure):
        _fields_ = [('lowerNS', C.c_uint64), ('backendNS', C.c_uint64)]

    air = library.qgpu_compile_air_nir
    air.argtypes = [C.c_void_p, C.c_size_t, C.c_char_p, C.POINTER(Constant), C.c_uint,
                   C.c_uint64, C.c_void_p, C.POINTER(Timing), C.c_void_p, C.c_uint]
    air.restype = C.c_int
    inputs = {}
    for line in sys.stdin:
        case = json.loads(line)
        path = case['input']
        if path not in inputs:
            inputs[path] = Path(path).read_bytes()
        source = inputs[path]
        values = (Constant * len(case.get('constants', [])))()
        for at, (index, kind, value) in zip(values, case.get('constants', [])):
            at.index, at.type = index, kind
            for i, byte in enumerate(bytes.fromhex(value)):
                at.bytes[i] = byte
        image = C.create_string_buffer(args.image_bytes)
        error = C.create_string_buffer(4096)
        phases = Timing()
        start_cpu, start_wall = time.thread_time_ns(), time.perf_counter_ns()
        if case['route'] == 'air':
            ok = air(source, len(source), case['entry'].encode(), values, len(values),
                     1, image, C.byref(phases), error, len(error))
        else:
            ok = glsl(source, case['stage'], image, error, len(error))
        wall, cpu = time.perf_counter_ns() - start_wall, time.thread_time_ns() - start_cpu
        print(json.dumps({'ok': bool(ok), 'image_sha256': digest(image.raw) if ok else None,
                          'error': error.value.decode(errors='replace'), 'cpu_ms': cpu / 1e6,
                          'wall_ms': wall / 1e6, 'lower_ms': phases.lowerNS / 1e6,
                          'backend_ms': phases.backendNS / 1e6}), flush=True)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--baseline', type=Path)
    parser.add_argument('--candidate', type=Path)
    parser.add_argument('--output', type=Path)
    parser.add_argument('--repeats', type=int, default=31)
    parser.add_argument('--fresh-process', action='store_true',
                        help='time each selected input in a new process, including the first AIR decode')
    parser.add_argument('--cpu', type=int, default=sorted(os.sched_getaffinity(0))[len(os.sched_getaffinity(0)) // 2])
    parser.add_argument('--worker', type=Path, help=argparse.SUPPRESS)
    parser.add_argument('--image-bytes', type=int, help=argparse.SUPPRESS)
    args = parser.parse_args()
    if args.worker:
        worker(args)
        return
    if not all((args.baseline, args.candidate, args.output)) or args.repeats < 3:
        parser.error('--baseline, --candidate, --output and at least 3 repeats are required')
    work = args.output.resolve()
    work.mkdir(parents=True, exist_ok=False)
    (work / 'inputs').mkdir()
    probe = work / 'size.c'
    probe.write_text('#include <stdio.h>\n#include "GPUJITProtocol.h"\n'
                     'int main(void){printf("%zu\\n",sizeof(QGPUProgramImage));}\n')
    subprocess.run(['cc', '-I' + str(ROOT / 'drivers/include'), str(probe),
                    '-o', str(work / 'size')], check=True)
    image_bytes = int(subprocess.check_output([str(work / 'size')]))
    cases = []

    def add(path, route, label=None, **fields):
        data = path.read_bytes()
        frozen = work / 'inputs' / (digest(data) + path.suffix)
        if not frozen.exists():
            frozen.write_bytes(data)
        case = dict(name=label or str(path.relative_to(ROOT)), source=str(path),
                    source_sha256=digest(data), input=str(frozen), route=route, **fields)
        cases.append(case)
        return case

    for folder in [ROOT / 'build/gpu-compiler',
                   ROOT / 'build/gpu-runtime/ui-cold-compile-shared-nir/native-fluidity']:
        for path in sorted(folder.iterdir()):
            if path.suffix in {'.comp', '.vert', '.frag'}:
                add(path, 'glsl', stage={'.comp': 0, '.vert': 1, '.frag': 2}[path.suffix])
    fixture = ROOT / 'build/air-nir-validation-20261020/inputs'
    add(fixture / 'compute.air', 'air', 'AIR compute', entry='air_add')
    constants = [[int(i), int(t), h] for i, t, h in
                 (line.split() for line in (fixture / 'welcome.constants.txt').read_text().splitlines() if line.strip())]
    add(fixture / 'welcome.air', 'air', 'AIR QuartzCore', entry='fixed_frag_lph_cpf', constants=constants)
    for profile in range(8):
        add(fixture / 'fluidity.air', 'air', 'AIR Fluidity ' + str(profile), entry='fluidity::fragmentShader',
            constants=[[i, 53, '01' if profile & (1 << i) else '00'] for i in range(3)])
    (work / 'cases.json').write_text(json.dumps(cases, indent=2) + '\n')
    processes, logs, receipts = [], [], {}
    try:
        for label, path in [('baseline', args.baseline), ('candidate', args.candidate)]:
            frozen = work / (label + '.so')
            shutil.copy2(path, frozen)
            receipts[label] = dict(source=str(path.resolve()), sha256=digest(frozen.read_bytes()))
            log = (work / (label + '.stderr')).open('w')
            logs.append(log)
            processes.append(subprocess.Popen([sys.executable, str(Path(__file__).resolve()),
                '--worker', str(frozen), '--cpu', str(args.cpu), '--image-bytes', str(image_bytes)],
                stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=log, text=True, bufsize=1))

        def run(index, case):
            process = processes[index]
            process.stdin.write(json.dumps(case) + '\n')
            process.stdin.flush()
            line = process.stdout.readline()
            if not line:
                raise RuntimeError('Compiler worker exited; inspect its stderr log')
            return json.loads(line)

        def run_fresh(index, case):
            label = ['baseline', 'candidate'][index]
            result = subprocess.run([sys.executable, str(Path(__file__).resolve()),
                '--worker', str(work / (label + '.so')), '--cpu', str(args.cpu),
                '--image-bytes', str(image_bytes)], input=json.dumps(case) + '\n',
                stdout=subprocess.PIPE, stderr=logs[index], text=True, check=True)
            return json.loads(result.stdout)

        checks = []
        for case in cases:
            a, b = run(0, case), run(1, case)
            same = a['ok'] and b['ok'] and a['image_sha256'] == b['image_sha256']
            checks.append(dict(name=case['name'], identical=same, baseline=a, candidate=b))
        # Every input was compiled once above; exclude that warmup from timing.
        selected = [c for c in cases if c['route'] == 'air' or
                    Path(c['source']).name in {'display-math-f32.frag', 'constant_sample_fragment.frag', 'sdfFragmentShader.frag'}]
        timings = []
        for case in selected:
            rows = [[], []]
            for repeat in range(args.repeats):
                for index in [repeat & 1, 1 - (repeat & 1)]:
                    rows[index].append((run_fresh if args.fresh_process else run)(index, case))
            expected = next(c['baseline']['image_sha256'] for c in checks if c['name'] == case['name'])
            if not expected or any(not r['ok'] or r['image_sha256'] != expected for runs in rows for r in runs):
                raise RuntimeError('Compilation failed or output changed during timing: ' + case['name'])
            medians = [{key: statistics.median(r[key] for r in runs)
                        for key in ['cpu_ms', 'wall_ms', 'lower_ms', 'backend_ms']} for runs in rows]
            timings.append(dict(name=case['name'], baseline=medians[0], candidate=medians[1],
                                cpu_reduction_percent=100 * (1 - medians[1]['cpu_ms'] / medians[0]['cpu_ms']), samples=rows))
        passed = all(c['identical'] for c in checks)
        report = dict(passed=passed, hardware_tested=False, image_bytes=image_bytes,
            fresh_process=args.fresh_process,
            platform=platform.platform(), cpu=args.cpu, repeats=args.repeats, libraries=receipts,
            script_sha256=digest(Path(__file__).read_bytes()),
            scope='Host uncached GLSL or AIR-to-complete-JIT-image calls; includes all variants, excludes process/library startup, file I/O, reflection, upload and pipeline creation. Alternating library order. IR3 cache disabled. ' +
                ('Each timed call runs in a new process with an empty AIR decode cache.' if args.fresh_process else
                 'One warmup per input; the AIR decode-template cache may hit.'),
            checks=checks, timings=timings)
        (work / 'report.json').write_text(json.dumps(report, indent=2) + '\n')
        print(json.dumps(dict(passed=passed, cases=len(checks), successful=sum(c['baseline']['ok'] for c in checks),
                              timings=[{k: v for k, v in c.items() if k != 'samples'} for c in timings]), indent=2))
        if not passed:
            raise SystemExit(1)
    finally:
        for process in processes:
            if process.poll() is None:
                process.stdin.close()
                try:
                    process.wait(timeout=10)
                except subprocess.TimeoutExpired:
                    process.kill()
                    process.wait()
        for log in logs:
            log.close()


if __name__ == '__main__':
    main()
