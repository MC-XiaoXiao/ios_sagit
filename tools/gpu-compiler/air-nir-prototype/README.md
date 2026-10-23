# Offline AIR → NIR experiment

Run from the project root:

```sh
python3 tools/gpu-compiler/air-nir-prototype/run.py --output build/air-nir-validation-20261020
# Output checks only, without repeating the timing benchmark:
python3 tools/gpu-compiler/air-nir-prototype/run.py --output build/air-nir-validation-20261020 --differential-only
```

Requires the existing pinned Mesa host build at `build/gpu-compiler/host`,
LLVM C API/tooling, and the locally captured AIR/constant files listed in
`run.py`. Nothing is downloaded. A fresh result directory can be selected
with `--output /absolute/path`; an existing directory keeps its frozen
production sources and AIR inputs. `--cpu N` selects the benchmark core.

`--runtime-compiler /absolute/path/QcomGPUCompiler.so` freezes and verifies a
specific matching production runtime. `--save-semantic-outputs` also writes
the complete color and compute output bits for comparisons between two frozen
builds. These files are CPU NIR interpreter results, not hardware readbacks.

`run.py` snapshots the production frontend, backend and ABI headers, builds an
isolated host executable using the existing Mesa archives, runs offline checks,
compares outputs, then benchmarks three representative functions. It writes `REPORT.md`,
`report.json`, all raw timing samples, source/build receipts, direct NIR,
baseline GLSL, JIT images and per-variant A540 binaries/disassembly. It does not
build or install a driver or touch a device. The runtime now uses the shared
emitter through `Compiler::compileAIR`, retaining the original GLSL fallback.
Use a fresh output directory after changing the compiler or JIT ABI; existing
snapshots are intentionally preserved. Historical prototype results retain
their original sources and numbers.

The two routes share the frozen AIR reader/function-constant specialization and
the production memory guards, descriptor remapping, IR3 compiler, variant
selection and JIT validation. `DirectNIR.cpp` constructs NIR with the builder
API through the frozen `AIRNIRLowering.h`; it never emits or parses GLSL.
`BackendAdapter.c` adapts NIR to the same
backend. The original `lowerAIR`/`qgpu_compile_glsl` path remains the baseline.
`Validate.c` enables the pinned Mesa debug validator for untimed checks.
The build uses the production host C++ compiler and flags, freezes a copy of
the production compiler shared library, and checks its complete JIT images
against the exact NIR emitter/backend exercised by the differential runner.
This prevents a successful test of an obsolete duplicate emitter. It also
avoids spurious ISA differences from GCC/Clang builder argument ordering.

During semantic capture, both sample modes are compiled independently even
when the production backend can reuse MSAA-invariant code. Their complete
images must still match the optimized runtime library. Timed runs keep the
production reuse decision. To compare two frozen runtime compiler libraries
(including all eight Fluidity profiles), use `../benchmark-cold.py`; it freezes
the existing local fixtures, compares complete images, and alternates uncached
compilations in separate processes on one host CPU.

The timed interval starts with AIR bytes and resolved function constants in
memory and ends with the validated complete JIT image, including base/MSAA and
optional constant-window variants. Each call specializes a private AIR module and disables the IR3 cache.
The bounded immutable decode-template cache is enabled, so warm in-process
timings include clone/specialization rather than repeated parsing for large AIR. Process/library warmup, input I/O, MSL compilation, metallib extraction,
upload and Metal pipeline object creation are outside the interval. Timings are
from the host CPU, not the Snapdragon 835.

Timing coverage remains limited to authored `air_add`, one captured QuartzCore
`fixed_frag_lph_cpf` hot specialization and `fluidity::fragmentShader` with
booleans 0/1/2=false. These are not whole-app startup measurements.

The feature suite additionally exercises authored compute/vertex/fragment AIR:
natural/nested loops, parallel PHIs, system inputs, shared/dynamic storage,
relaxed atomics and barriers, bounded private arrays, constant tables, packed
integer reads, half bitcasts, image reads/writes and shaped texture sampling,
MRT/depth/sample-mask/discard, splat constants and display math.
The remaining-features suite also tests multi-exit and irreducible loops,
buffer/private pointer PHIs/selects and null comparisons, general scalar/vector
int64 arithmetic/conversions and bounded buffer/private/shared 64-bit accesses.
The runtime compiles entire multi-entry libraries through NIR atomically and
resolves named constants per entry. GLSL fallback remains available.
The prototype calls the direct compiler without automatic fallback.
`features.tsv` records each entry's result; `features.json` records totals.
`remaining.tsv` and `remaining.json` record 256 deterministic quad fixtures per
new entry against a separate C++ integer/control-flow oracle, with short buffers,
signed/high-bit boundaries, PHI swaps and dynamic resource selection. The actual
runtime JIT image must match the tested emitter byte for byte. Division and
remainder are separate entries because combining them can exceed the production
A540 code-size limit. `remaining-*.image` and selected input/expected buffer files
are retained for the hardware task, alongside dispatch metadata. These fixtures
have not themselves been executed on A540 by this runner.

`Differential.cpp` interprets the finalized NIR from both routes, including
all applicable base/MSAA/window variants. It uses Mesa's ALU evaluator,
structured masked quad control flow, guarded SSBOs, constant windows,
varyings/derivatives, deterministic nonconstant mipmapped texture samples and
1/2/4-sample framebuffer reads. The eight Fluidity boolean profiles are all
checked. Graphics use exact numerical comparisons (matching NaNs allowed);
Compute also checks the complete buffers and guards against an independent
add7 reference. Feature cases compare complete buffers and all stage outputs;
bool conversion (including positive zero), loop PHIs, nested loops, half word
shuffles, private arrays and discard-side effects also have independent references.
The shared-memory interpreter models one four-invocation workgroup with uniform
barriers; it does not model arbitrary inter-workgroup schedules or races. An injected output change must be detected. Outputs, case
counts and limitations are recorded in `differential.json` and
`DIFFERENTIAL.md`, with the final NIR and paired JIT images retained.

These are host semantic checks. They do not execute IR3 instructions or the
A540 texture hardware and do not establish full application or GPU output
equivalence. No frame rate is measured. Keep the GLSL fallback while the
separate target-device task validates actual outputs. Register counts use the
existing IR3 export's vec4 units; they do not by themselves determine occupancy.
