"""Read an existing completed capture only; never contacts a device or edits drivers."""
from pathlib import Path
import hashlib
import importlib.util
import json
import plistlib
import sys

ROOT = Path(__file__).resolve().parents[2]
BASE = ROOT / "build/gpu-runtime"
spec = importlib.util.spec_from_file_location("live", BASE / "ui-poll-validation/analyze-live.py")
live = importlib.util.module_from_spec(spec)
spec.loader.exec_module(live)
capture = sys.argv[1] if len(sys.argv) > 1 else "round-146"
assert capture in ("round-144-wake", "round-146")
source = BASE / capture / "uart.txt"
raw = source.read_bytes()
analysis = json.loads((BASE / capture / "analysis.json").read_text())
assert hashlib.sha256(raw).hexdigest() == analysis["source_sha256"]
cleaned, repairs = live.repair_known_interleaving(raw.decode(errors="replace"))
snapshots = live.snapshots(cleaned)
assert len(snapshots) == 2
lo, hi = [s["header"]["begin_ticks"] for s in snapshots]
jobs = [live.fields(l) for l in cleaned.splitlines() if l.startswith("GPU_JOB ")]
frames = [live.fields(l) for l in cleaned.splitlines() if l.startswith("DISPLAY_FRAME ")]
header = analysis["history_header"]
ms = header["cpu_timebase_ns"] / header["cpu_timebase_ticks"] / 1e6
assert len(jobs) == header["count"] and jobs[0]["retire_end"] < lo
assert all(j["ordinal"] == header["total"] - len(jobs) + i + 1 for i, j in enumerate(jobs))
assert all(a["retire_end"] <= b["retire_end"] for a, b in zip(jobs, jobs[1:]))
assert snapshots[-1]["header"]["sequence"] == header["sequence"]
selected = [j for j in jobs if j["flags"] & 1 and lo <= j["retire_end"] <= hi]
assert len(selected) == analysis["selected_jobs"]
assert all(j["flags"] & 2 and j["flags"] & 4 for j in selected)

def gpu_ms(j):
    return (j["gpu_end"] - j["gpu_begin"]) / 19200

total_gpu = sum(map(gpu_ms, selected))
assert abs(total_gpu - analysis["job_distributions"]["gpu_ms"]["total"]) < 1e-6
concentration = []
for threshold in (16.67, 50, 100):
    subset = [j for j in selected if gpu_ms(j) > threshold]
    duration = sum(map(gpu_ms, subset))
    concentration.append(dict(threshold_ms=threshold, jobs=len(subset),
                              job_fraction=len(subset)/len(selected),
                              gpu_duration_ms=duration, duration_fraction=duration/total_gpu))

visible = [f for f in frames if lo <= f["ticks"] <= hi]
assert len(visible) == analysis["display"]["selected_rows"]
long = []
for before, after in zip(visible, visible[1:]):
    a, b = before["ticks"], after["ticks"]
    if (b-a)*ms <= 100:
        continue
    spans = sorted((max(a,j["submitted"]),min(b,j["observed"])) for j in jobs
                   if j["submitted"] and j["submitted"] < b and j["observed"] > a)
    cursor, holes = a, []
    for start, end in spans:
        if start > cursor:
            holes.append((cursor,start))
        cursor = max(cursor,end)
    if cursor < b:
        holes.append((cursor,b))
    crossing = [j for j in jobs if j["prepare"] <= a < j["retire_end"]]
    first = min((j["prepare"] for j in jobs if a < j["prepare"] < b), default=b)
    observed = [j for j in selected if a < j["observed"] <= b]
    nearby = [j for j in jobs if j["prepare"] < b and j["retire_end"] > a]
    long.append(dict(
        before_token=before["token"], after_token=after["token"],
        start_from_window_seconds=(a-lo)*ms/1000, display_interval_ms=(b-a)*ms,
        prepared_unretired_jobs_at_start=[j["ordinal"] for j in crossing],
        prefix_without_recorded_prepare_or_unretired_work_ms=0 if crossing else (first-a)*ms,
        no_posted_unobserved_work_ms=sum((y-x)*ms for x,y in holes),
        longest_observed_gpu_duration_ms=max((gpu_ms(j) for j in observed),default=0),
        observed_gpu_duration_sum_ms=sum(map(gpu_ms,observed)),
        nearby_jobs=[dict(ordinal=j["ordinal"], shader_operations=j["ops"], kinds=j["kinds"],
                          surfaces=j["surfaces"],gpu_ms=gpu_ms(j),
                          **{k+"_relative_ms":(j[k]-a)*ms if j[k] else None
                             for k in ("prepare","admitted","submitted","observed","retire_end")})
                     for j in nearby]))
assert len(long) == analysis["display"]["over100ms"]

paths = [
    "drivers/gpu/QcomGPU/Info.plist", "drivers/gpu/QcomMTL/Info.plist",
    "drivers/display/QcomDisplay/Info.plist",
    "drivers/gpu/QcomGPU/AdrenoPipeline.h", "drivers/gpu/QcomGPU/QcomGPUNative.cpp",
    "drivers/gpu/QcomGPU/QcomGPUHardware.cpp", "drivers/gpu/QcomMTL/QcomMTL.mm",
    "drivers/display/QcomDisplay/QcomDisplay.cpp", "drivers/display/QcomDisplay/DisplayCommandTransfer.h",
    "build/gpu-runtime/ui-hardware-phases/apple-command-did-complete.json",
    "build/gpu-runtime/ui-hardware-phases/apple-command-storage-reset.json",
    "build/gpu-runtime/ui-hardware-phases/apple-command-storage-release.json",
    "build/gpu-runtime/ui-hardware-phases/apple-resource-pool-create.json",
    "build/gpu-runtime/ui-framework-lifecycle/lifecycle-audit.json",
    "build/gpu-runtime/ui-inline-constants/apple-audit.json",
    "build/gpu-runtime/ui-shader-object-reuse/audit.json",
    "build/gpu-runtime/ui-shader-object-reuse/hardware-evidence.json",
    f"build/gpu-runtime/{capture}/analysis.json",
    "build/gpu-runtime/ui-poll-validation/analyze-live.py",
]
versions = {label:plistlib.loads((ROOT/p).read_bytes())["CFBundleVersion"]
            for label,p in zip(("GPU","Metal","Display"),paths[:3])}
result = dict(
    scope="Static review and recomputation of existing captures; no device action or driver edits",
    source_versions=versions, measured_versions=dict(GPU="1.132.0" if capture=="round-146" else "1.131.0",Metal="0.93.0",Display="1.0.17"),
    source=str(source), source_sha256=hashlib.sha256(raw).hexdigest(),
    window_ticks=[lo,hi], history_header=header, capture_repairs=repairs,
    selected_jobs=len(selected), concentration=concentration,
    distributions=analysis["job_distributions"], cpu_phases=analysis["live"]["cpu_elapsed_timings"],
    hardware_phases=analysis["live"]["hardware_phases"],
    shader_objects=analysis.get("shader_objects"), core_clock=analysis["core_clock"],
    resources=analysis["resources"], display_summary={k:v for k,v in analysis["display"].items() if k!="longest"},
    long_display_intervals=long,
    source_and_reference_sha256={p:hashlib.sha256((ROOT/p).read_bytes()).hexdigest() for p in paths},
    limitations=[
        "prepare is submitNative entry AFTER the shared kernel gate; no-prepare time is not CPU busy time or pure userspace time.",
        "History contains admitted retired jobs; rejected attempts, wrapper entry/gate wait and Metal/CA CPU work have no per-event timeline here.",
        "No-posted-unobserved intervals are a software timeline classification, not an RBBM hardware idle measurement.",
        "GPU and CPU epochs differ; only durations are compared, and observed GPU durations need not lie wholly in a display interval.",
        "Surface/time proximity is not exact frame-to-job or animation-phase attribution.",
        "Cumulative nested CPU phases cannot be added to a frame or converted into CPU utilization.",
        "Apple superclass completion call preceding storage cleanup does not prove exact asynchronous callback execution order.",
        "Different captures contain different animation content and holds; do not infer controlled A/B improvement from aggregate rates.",
    ])
target = Path(__file__).with_name("hello-long-frame-review-1.132" + ("-round144" if capture=="round-144-wake" else "") + "-evidence.json")
target.write_text(json.dumps(result,ensure_ascii=False,indent=2)+"\n")
print(json.dumps(dict(source_versions=versions,selected_jobs=len(selected),long_intervals=len(long),
                     concentration=concentration, evidence=str(target)),ensure_ascii=False,indent=2))
