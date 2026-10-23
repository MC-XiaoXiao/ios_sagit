"""Compile the production scheduler bridge and worker against a deterministic
IOKit harness; extract the real XNU registration wrappers, not a second policy.
No device, firmware connection, or kernel execution is involved.
"""
import importlib.util
from pathlib import Path
import re
import os
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]


def registration_fixture():
    source = (ROOT/'kernel/osfmk/arm/machine_routines_common.c').read_text()
    start = source.index('// Configuration is serialized in thread context only.')
    end = source.index('\n\nstatic void\nmachine_switch_populate_perfcontrol_thread_data', start)
    registration = source[start:end]
    # Preserve all four production arbitration entry points and the CPU-state
    # snapshot. Only Apple's existing install implementation is modeled here.
    install_start = registration.index('static void\nsched_perfcontrol_install_callbacks(')
    wrappers_start = registration.index('\nvoid\nsched_perfcontrol_register_callbacks(', install_start)
    registration = registration[:install_start] + registration[wrappers_start:]
    generic = ['offcore','switch','oncore','thread_group_init','thread_group_deinit',
               'thread_group_flags_update','max_runnable_latency','work_interval_notify',
               'work_interval_ctl','deadline_passed','thread_group_blocked','thread_group_unblocked']
    slots = {name:('context_switch' if name=='switch' else name) for name in generic}
    prelude = '''#include <KernelShim.h>
#include <XNUPerfControlCapture.h>
#define _Atomic(T) std::atomic<T>
#define LCK_GRP_DECLARE(name,desc) [[maybe_unused]] static int name=0
#define LCK_MTX_DECLARE(name,grp) static std::mutex name
#define lck_mtx_lock(p) (p)->lock()
#define lck_mtx_unlock(p) (p)->unlock()
#define os_atomic_store(p,v,o) (p)->store(v,std::memory_order_##o)
#define os_atomic_load(p,o) (p)->load(std::memory_order_##o)
struct perfcontrol_state { uint64_t state[16]; };
'''
    defaults = {name:('thread_group' if name in ['thread_group_init','thread_group_deinit','thread_group_flags_update'] else name) for name in generic}
    for default in sorted(set(defaults.values())):
        prelude += f'static void sched_perfcontrol_{default}_default() {{}}\n'
    for name in generic:
        prelude += f'static GenericCallback sched_perfcontrol_{name}=sched_perfcontrol_{defaults[name]}_default;\n'
    prelude += '''static void sched_perfcontrol_csw_default(perfcontrol_event,uint32_t,uint64_t,uint32_t,perfcontrol_thread_data *,perfcontrol_thread_data *,perfcontrol_cpu_counters *,void *) {}
static void sched_perfcontrol_state_update_default(perfcontrol_event,uint32_t,uint64_t,uint32_t,perfcontrol_thread_data *,void *) {}
static sched_perfcontrol_csw_t sched_perfcontrol_csw=sched_perfcontrol_csw_default;
static sched_perfcontrol_state_update_t sched_perfcontrol_state_update=sched_perfcontrol_state_update_default;
static void sched_perfcontrol_install_callbacks(sched_perfcontrol_callbacks_t c,unsigned long) {
'''
    for name, field in slots.items():
        prelude += f'sched_perfcontrol_{name}=c && c->{field} ? c->{field} : sched_perfcontrol_{defaults[name]}_default;\n'
    prelude += '''sched_perfcontrol_csw=c && c->csw ? c->csw : sched_perfcontrol_csw_default;
sched_perfcontrol_state_update=c && c->state_update ? c->state_update : sched_perfcontrol_state_update_default;
}
constexpr int FALSE=0;
constexpr unsigned MAX_SCHED_CPUS=64;
constexpr int PROCESSOR_IDLE=4,PROCESSOR_RUNNING=6;
using processor_set_t=std::mutex *;
struct processor {processor_set_t processor_set;int state=4;perfcontrol_class_t current_perfctl_class=PERFCONTROL_CLASS_IDLE;};
using processor_t=processor *;
constexpr processor_t PROCESSOR_NULL=nullptr;
static std::mutex pset;
static processor cpus[64];
static processor_t processor_array[64]={};
static bool ml_set_interrupts_enabled(bool value) {return value;}
static void pset_lock(processor_set_t p) {p->lock();}
static void pset_unlock(processor_set_t p) {p->unlock();}
'''
    return prelude + registration + '''
void test_cpu(unsigned cpu,bool online,perfcontrol_class_t qos) {
    assert(cpu<64);cpus[cpu].processor_set=&pset;cpus[cpu].state=!online ? 0 : qos==PERFCONTROL_CLASS_IDLE ? 4 : 6;
    cpus[cpu].current_perfctl_class=qos;processor_array[cpu]=&cpus[cpu];
}
static void foreign_oncore() {}
void test_native_registration(bool installed) {
    static sched_perfcontrol_callbacks native={};native.version=5;native.oncore=foreign_oncore;
    sched_perfcontrol_register_callbacks(installed ? &native : nullptr,0);
}
'''


class PerfControlIntegrationTests(unittest.TestCase):
    def test_production_adapter_and_kernel_registration(self):
        with tempfile.TemporaryDirectory(prefix='perfcontrol-',dir=ROOT/'build') as tmp:
            tmp = Path(tmp)
            fixture = tmp/'registration.cpp'
            fixture.write_text(registration_fixture())
            command = ['c++','-std=c++17','-O2','-Wall','-Wextra','-Werror',
                       '-I'+str(ROOT/'tests/perfcontrol_shim'),
                       '-I'+str(ROOT/'drivers/include'),
                       '-I'+str(ROOT/'drivers/clock/GenericClockController'),
                       '-I'+str(ROOT/'drivers/clock/QcomOSM'),
                       str(ROOT/'tests/cpu_performance_runtime_test.cpp'),
                       str(ROOT/'drivers/clock/GenericClockController/CPUPerformanceAdapter.cpp'),
                       str(ROOT/'drivers/clock/GenericClockController/XNUPerfControlCapture.cpp'),
                       str(fixture),'-o',str(tmp/'test')]
            if os.environ.get('PERFCONTROL_SANITIZE'):
                command[1:1]=['-fsanitize=address,undefined','-fno-omit-frame-pointer','-g']
            compiled=subprocess.run(command,capture_output=True,text=True)
            self.assertEqual(compiled.returncode,0,compiled.stderr)
            result = subprocess.run([tmp/'test'],capture_output=True,text=True)
            self.assertEqual(result.returncode,0,result.stderr)
            self.assertIn('production csw/state callbacks',result.stdout)
            print(result.stdout.strip())

    def test_callback_object_has_no_blocking_or_hardware_imports(self):
        # Inspect a real arm64 Kext object, compiled with the production SDK.
        obj = ROOT/'build/drivers/obj/GenericClockController/XNUPerfControlCapture.cpp.o'
        self.assertTrue(obj.exists(),'build GenericClockController before this check')
        nm = ROOT/'tools/prefix/bin/arm-apple-darwin11-nm'
        result = subprocess.run([nm,'-u',obj],check=True,capture_output=True,text=True)
        imported = {line.strip() for line in result.stdout.splitlines() if line.strip()}
        # Stack protection is a compiler ABI requirement. It is not a worker
        # wakeup, clock request, allocator, runtime atomic fallback or MMIO API.
        self.assertLessEqual(imported,{'___stack_chk_fail','___stack_chk_guard'})

    def test_callback_control_flow_is_acyclic(self):
        obj=ROOT/'build/drivers/obj/GenericClockController/XNUPerfControlCapture.cpp.o'
        output=subprocess.run(['llvm-objdump','-dr','--no-show-raw-insn',obj],check=True,capture_output=True,text=True).stdout
        names={}
        symbols=subprocess.run([ROOT/'tools/prefix/bin/arm-apple-darwin11-nm','-n',obj],check=True,capture_output=True,text=True).stdout
        for line in symbols.splitlines():
            m=re.match(r'([0-9a-f]+) [Tt] (.+)',line)
            if m:names[m[2]]=int(m[1],16)
        instructions={};relocations={}
        for line in output.splitlines():
            m=re.match(r'\s*([0-9a-f]+):\s+([a-z][a-z0-9.]*)\s*(.*)',line)
            if m:instructions[int(m[1],16)]=(m[2],m[3])
            r=re.match(r'\s*([0-9a-f]+):\s+ARM64_RELOC_BRANCH26\s+(\S+)',line)
            if r:relocations[int(r[1],16)]=r[2]
        visiting=set();complete=set()
        def visit(pc):
            if pc in complete:return
            self.assertNotIn(pc,visiting,'scheduler callback has an instruction retry loop')
            self.assertIn(pc,instructions)
            visiting.add(pc);op,args=instructions[pc]
            self.assertNotIn(op,{'bl','blr','br'},'scheduler callback calls or jumps to an unaudited target')
            if op!='ret':
                if op=='b' or op.startswith('b.') or op in {'cbz','cbnz','tbz','tbnz'}:
                    if pc in relocations:
                        self.assertIn(relocations[pc],names);target=names[relocations[pc]]
                    else:target=int(re.findall(r'0x([0-9a-f]+)',args.split('<')[0])[-1],16)
                    visit(target)
                    if op!='b':visit(pc+4)
                else:visit(pc+4)
            visiting.remove(pc);complete.add(pc)
        for name in ['_cpu_performance_csw','_cpu_performance_state_update']:visit(names[name])
        self.assertGreater(len(complete),50)

    def test_kernel_extension_and_driver_imports_match(self):
        exported = (ROOT/'kernel/config/Private.arm64.exports').read_text().splitlines()
        for symbol in ['sched_perfcontrol_register_callbacks_exclusive','sched_perfcontrol_callbacks_owned',
                       'sched_perfcontrol_release_callbacks','sched_perfcontrol_cpu_state']:
            self.assertIn('_'+symbol,exported)
        # Make sure a cold checkout retains the production kernel change.
        patch = (ROOT/'tools/patches/xnu-perfcontrol-policy.patch').read_text()
        self.assertIn('sched_perfcontrol_register_callbacks_exclusive',patch)
        self.assertIn('perfcontrol_registration_lock',patch)
        self.assertIn('sched_perfcontrol_cpu_state',patch)

if __name__=='__main__':unittest.main()
