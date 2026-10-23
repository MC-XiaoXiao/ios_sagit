#!/usr/bin/env python3
"""Compile a host differential test against bodies from the pinned vendor C.

The original algorithm bodies are extracted, not copied into an independent
lookalike oracle. The shim supplies already-selected DTS properties and the
Linux allocation/logging infrastructure; it cannot perform MMIO.
"""
import argparse
import importlib.util
import os
import re
import subprocess
import tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
spec = importlib.util.spec_from_file_location("tables", ROOT/"scripts/generate-cprh-tables.py")
tables = importlib.util.module_from_spec(spec)
spec.loader.exec_module(tables)


def function(text, name):
    match = re.search(r"(?:static )?(?:int|u64|void)\s+"+name+r"\([^;]*?\)\s*\{", text, re.S)
    if not match:
        raise ValueError(name)
    start, pos, depth = match.start(), match.end(), 1
    while depth:
        depth += (text[pos] == "{") - (text[pos] == "}")
        pos += 1
    return text[start:pos]


SHIM = r'''
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
typedef uint64_t u64;
typedef int64_t s64;
typedef uint32_t u32;
typedef int32_t s32;
typedef int bool;
#define GFP_KERNEL 0
#define kcalloc(n,s,f) calloc(n,s)
#define kfree free
#define true 1
#define false 0
#define cpr3_debug(...) ((void)0)
#define cpr3_err(...) ((void)0)
#define cpr3_info(...) ((void)0)
#define max(a,b) ((a)>(b)?(a):(b))
#define min(a,b) ((a)<(b)?(a):(b))
#define do_div(a,b) ((a)/=(b))
#define CPR3_ROUND(n,d) (((n)+(d)-1)/(d)*(d))
#define CPRH_KBSS_POWER_CLUSTER_ID 0
#define CPRH_KBSS_PERFORMANCE_CLUSTER_ID 1
#define CPRH_KBSS_FUSE_STEP_VOLT 10000
#define CPRH_KBSS_VOLTAGE_FUSE_SIZE 6
#define CPRH_KBSS_QUOT_OFFSET_SCALE 5
#define CPRH_SDM660_POWER_KBSS_FUSE_CORNER_LOWSVS 0
#define CPRH_SDM660_POWER_KBSS_FUSE_CORNER_TURBO_L1 3
#define CPRH_SDM660_PERF_KBSS_FUSE_CORNER_SVS 0
#define CPRH_SDM660_PERF_KBSS_FUSE_CORNER_TURBO_L2 3
#define CPRH_SDM630_POWER_KBSS_FUSE_CORNER_LOWSVS 0
#define CPRH_SDM630_POWER_KBSS_FUSE_CORNER_TURBO_L1 3
#define CPRH_SDM630_PERF_KBSS_FUSE_CORNER_LOWSVS 0
#define CPRH_SDM630_PERF_KBSS_FUSE_CORNER_TURBO_L2 3
#define CPRH_MSM8998_KBSS_FUSE_CORNER_LOWSVS 0
#define CPRH_MSM8998_KBSS_FUSE_CORNER_TURBO_L1 3
enum soc_id { SDM660_SOC_ID, SDM630_SOC_ID, MSM8998_V1_SOC_ID, MSM8998_V2_SOC_ID };
struct device_node { int unused; };
static const int sdm660_kbss_fuse_ref_volt[2][4]={{0}};
static const int sdm630_kbss_fuse_ref_volt[2][4]={{0}};
static const int sdm630_perf_kbss_speed_bin_2_fuse_ref_volt[4]={0};
static const int msm8998_v1_kbss_fuse_ref_volt[4]={0};
static const int msm8998_v2_kbss_fuse_ref_volt[2][4] =
    {{688000,756000,828000,1056000},{756000,756000,828000,1056000}};
static const char *const cprh_sdm660_power_kbss_fuse_corner_name[4]={0};
static const char *const cprh_sdm660_perf_kbss_fuse_corner_name[4]={0};
static const char *const cprh_sdm630_power_kbss_fuse_corner_name[4]={0};
static const char *const cprh_sdm630_perf_kbss_fuse_corner_name[4]={0};
static const char *const cprh_msm8998_kbss_fuse_corner_name[4]={0};
struct cprh_kbss_fuses { u64 ro_sel[4], init_voltage[4], target_quot[4], quot_offset[4]; };
struct cpr3_corner {
    u32 proc_freq;
    int cpr_fuse_corner, open_loop_volt, floor_volt, ceiling_volt;
    u32 target_quot[16];
};
struct cpr3_controller {
    int ctrl_id; enum soc_id soc_revision;
    int step_volt, apm_threshold_volt, apm_adj_volt, mem_acc_threshold_volt;
};
struct cpr3_thread { struct cpr3_controller *ctrl; };
struct cpr3_regulator {
    struct device_node *of_node;
    struct cprh_kbss_fuses *platform_fuses;
    struct cpr3_thread *thread;
    int fuse_corner_count, corner_count, speed_bin_fuse;
    struct cpr3_corner corner[40];
    const int64_t *open_fuse, *open_adjust, *closed_fuse, *closed_adjust, *range, *scale;
};
static int of_property_read_bool(struct device_node *n, const char *p) {
    (void)n; (void)p; return 1;
}
static void *of_find_property(struct device_node *n, const char *p, void *len) {
    (void)n; (void)len;
    return strstr(p,"min-diff") ? NULL : (void*)1;
}
static int cpr3_parse_array_property(struct cpr3_regulator *v, const char *name,
                                   int size, u32 *out) {
    (void)name;
    for (int i=0;i<size;i++) out[i]=(u32)v->open_fuse[i];
    return 0;
}
static int cpr3_parse_corner_array_property(struct cpr3_regulator *v, const char *name,
                                          int size, u32 *out) {
    (void)size;
    const int64_t *a=strstr(name,"max-range") ? v->range : v->open_adjust;
    for (int i=0;i<v->corner_count;i++) out[i]=(u32)a[i];
    return 0;
}
static int cpr3_parse_closed_loop_voltage_adjustments(struct cpr3_regulator *v,
                       u64 *ro, int *adj, int *fadj, int *scale) {
    for (int i=0;i<v->corner_count;i++) adj[i]=(int)v->closed_adjust[i];
    for (int i=0;i<4;i++) {
        fadj[i]=(int)v->closed_fuse[i]; scale[i]=(int)v->scale[i*16+ro[i]];
    }
    return 0;
}
static int cprh_kbss_set_no_interpolation_quotients(struct cpr3_regulator *v,
                           int *a,int *b,int *c) { return -1; }
'''

WRAPPER = r'''
// This ABI is intentionally plain C, separate from the production header.
int vendor_calibrate(unsigned cluster, unsigned n, unsigned bin, const unsigned *ro,
        const unsigned *voltage, const unsigned *target, const unsigned *offset,
        const int64_t *fmax, const int64_t *hz, const int64_t *floor,
        const int64_t *ceiling, const int64_t *range, const int64_t *open_fuse,
        const int64_t *open_adjust, const int64_t *closed_fuse,
        const int64_t *closed_adjust, const int64_t *scale, uint32_t *out) {
    struct cpr3_controller ctrl={cluster, MSM8998_V2_SOC_ID,4000,800000,0,852000};
    struct cpr3_thread thread={&ctrl};
    struct cprh_kbss_fuses f={0};
    struct cpr3_regulator v={0};
    for (unsigned i=0;i<4;i++) {
        f.ro_sel[i]=ro[i]; f.init_voltage[i]=voltage[i];
        f.target_quot[i]=target[i]; f.quot_offset[i]=offset[i];
    }
    v.platform_fuses=&f; v.thread=&thread; v.fuse_corner_count=4;
    v.corner_count=n; v.speed_bin_fuse=bin;
    v.open_fuse=open_fuse; v.open_adjust=open_adjust;
    v.closed_fuse=closed_fuse; v.closed_adjust=closed_adjust;
    v.range=range; v.scale=scale;
    for (unsigned i=0,fc=0;i<n;i++) {
        while (i>=(unsigned)fmax[fc]) fc++;
        v.corner[i].proc_freq=(u32)hz[i]; v.corner[i].cpr_fuse_corner=fc;
        v.corner[i].floor_volt=(int)floor[i]; v.corner[i].ceiling_volt=(int)ceiling[i];
    }
    if (cprh_kbss_calculate_open_loop_voltages(&v) ||
        cpr3_limit_open_loop_voltages(&v)) return -1;
    cprh_adjust_voltages_for_apm(&v); cprh_adjust_voltages_for_mem_acc(&v);
    cpr3_open_loop_voltage_as_ceiling(&v);
    if (cpr3_limit_floor_voltages(&v) || cprh_kbss_calculate_target_quotients(&v)) return -1;
    for (unsigned i=0;i<n;i++) {
        out[4*i]=v.corner[i].open_loop_volt; out[4*i+1]=v.corner[i].floor_volt;
        out[4*i+2]=v.corner[i].ceiling_volt;
        out[4*i+3]=v.corner[i].target_quot[ro[v.corner[i].cpr_fuse_corner]];
    }
    return 0;
}
'''

util = tables.checked("xiaomi-cpr3-util.c")
kbss = tables.checked("xiaomi-cprh-kbss-regulator.c")
names = [
    "cpr3_convert_open_loop_voltage_fuse", "cpr3_interpolate", "cpr3_quot_adjustment",
    "cpr3_adjust_fused_open_loop_voltages", "cpr3_adjust_open_loop_voltages",
    "cpr3_limit_open_loop_voltages", "cpr3_open_loop_voltage_as_ceiling",
    "cpr3_limit_floor_voltages", "cprh_adjust_voltages_for_apm",
    "cprh_adjust_voltages_for_mem_acc",
]
oracle = SHIM + "\n".join(function(util, n) for n in names)
oracle += function(kbss, "cprh_kbss_calculate_open_loop_voltages")
oracle += function(kbss, "cprh_kbss_calculate_target_quotients") + WRAPPER
subprocess.run(["python3", str(ROOT/"scripts/generate-cprh-tables.py"), "--check"], check=True)
parser = argparse.ArgumentParser()
parser.add_argument("--sanitize", action="store_true")
args = parser.parse_args()
flags = ["-fsanitize=address,undefined", "-fno-omit-frame-pointer"] if args.sanitize else []
with tempfile.TemporaryDirectory(prefix="cprh-test-") as temp:
    folder = Path(temp)
    (folder/"oracle.c").write_text(oracle)
    subprocess.run([os.getenv("CC", "cc"), "-std=c11", "-O2", *flags, "-Wno-incompatible-pointer-types",
                    "-c", str(folder/"oracle.c"), "-o", str(folder/"oracle.o")], check=True)
    subprocess.run([os.getenv("CXX", "c++"), "-std=c++17", "-O2", *flags, "-Wall", "-Wextra", "-Werror",
                    str(ROOT/"tests/cprh_calibration_test.cpp"), str(folder/"oracle.o"),
                    "-o", str(folder/"test")], check=True)
    subprocess.run([str(folder/"test")], check=True)
