import importlib.util
from pathlib import Path
import unittest
import subprocess
import json
import os
import pty
import sys
import tempfile
from unittest import mock

spec = importlib.util.spec_from_file_location('test_bridge_script', Path(__file__).resolve().parents[1]/'scripts/test-bridge.py')
bridge = importlib.util.module_from_spec(spec); spec.loader.exec_module(bridge)


class UserlandEvidenceTests(unittest.TestCase):
    def test_passive_capture_never_writes_or_dispatches_a_payload(self):
        master,slave=pty.openpty()
        try:
            with tempfile.TemporaryDirectory() as temporary:
                root=Path(temporary);profile=root/'profile.json';profile.write_text(json.dumps({'uart':os.ttyname(slave)}))
                argv=['test-bridge.py','--observe-only','--profile',str(profile),'--payload',str(root/'missing.efi'),
                      '--output',str(root/'capture'),'--seconds','0.01']
                with mock.patch.object(sys,'argv',argv),mock.patch.object(bridge.os,'write',side_effect=AssertionError('UART write')) as write, \
                        mock.patch.object(bridge.subprocess,'Popen',side_effect=AssertionError('Fastboot dispatch')) as dispatch:
                    self.assertEqual(bridge.main(),0)
                write.assert_not_called();dispatch.assert_not_called()
                report=json.loads((root/'capture/report.json').read_text())
                self.assertTrue(report['passive_uart_capture']);self.assertIsNone(report['payload_sha256'])
                self.assertEqual(report['commands'],[]);self.assertEqual(report['bash_probe_attempts'],0)
        finally:
            os.close(master);os.close(slave)

    def test_expected_watchdog_requires_shell_native_timeout_and_recovery(self):
        good = {key: True for key in ('serial_bash_interactive', 'watchdog_backboardd_timeout',
                'native_panic_report_completed', 'watchdog_recovered', 'multicore_verified', 'benchmark_verified')}
        self.assertTrue(bridge.expected_watchdog_result(good, 8, True))
        for key in good:
            bad = dict(good); bad[key] = False
            self.assertFalse(bridge.expected_watchdog_result(bad, 8, True), key)
        self.assertFalse(bridge.expected_watchdog_result({'watchdog_recovered': True}))

    def test_boot_timing_excludes_previous_boot_and_probe_echo(self):
        stale = b'[XNU bridge] EDK2 entry\nMi 6 XNU serial Bash (development console)\n'
        entry = stale + b'[XNU bridge] EDK2 entry\n[XNU] start_first_cpu\n'
        banner = entry + b'Mi 6 XNU serial Bash (development console)\n'
        echo = banner + b'printf "MI6_BASH_VERIFIED version=%s uid=%s pid=%s cwd=%s"\n'
        actual = echo + b'MI6_BASH_VERIFIED version=5.2.21 uid=0 pid=94 cwd=/\n'
        events = [{'seconds': seconds, 'end_offset': len(data)}
                  for seconds, data in ((1, stale), (10, entry), (50, banner), (51, echo), (52, actual))]
        parsed = bridge.boot_timing_evidence(actual, events, len(stale))
        self.assertEqual(parsed['efi_to_bash_seconds'], 40)
        self.assertEqual(parsed['efi_to_verified_bash_seconds'], 42)
        self.assertNotIn('bash_verified', bridge.boot_timing_evidence(echo, events[:-1], len(stale))['capture_seconds'])

    def test_bash_benchmark_checks_each_child_without_external_programs(self):
        command = bridge.bash_benchmark_command(jobs=2, loops=17, repeats=2)
        result = subprocess.run(['bash', '--noprofile', '--norc', '-c', command],
                                capture_output=True, text=True, check=True, timeout=10)
        parsed = bridge.benchmark_evidence(result.stdout, jobs=2, loops=17, repeats=2)
        self.assertTrue(parsed['benchmark_verified'], result.stdout + result.stderr)
        self.assertGreater(parsed['benchmark_median_us'], 0)
        self.assertFalse(bridge.benchmark_evidence(command)['benchmark_verified'])
        self.assertFalse(bridge.benchmark_evidence(result.stdout.replace('verified=1', 'verified=0'),
                                                 jobs=2, loops=17, repeats=2)['benchmark_verified'])
        self.assertFalse(bridge.benchmark_evidence(result.stdout.replace('ARM_SMP_BENCH_DONE', ''),
                                                 jobs=2, loops=17, repeats=2)['benchmark_verified'])
    def test_psci_success_and_echoed_probes_do_not_prove_smp(self):
        evidence = bridge.multicore_evidence('[ARM SMP] CPU_ON affinity=0x1 status=0\n', 2)
        self.assertFalse(evidence['multicore_verified'])
        self.assertEqual(evidence['online_cpus'], [])

    def test_requires_online_timer_and_ipi_on_every_cpu(self):
        for count in (1, 2, 4, 8):
            log = ''.join(f'[ARM SMP] {stage} cpu={cpu}\n'
                          for stage in ('registered', 'online', 'interrupts', 'timer', 'ipi')
                          for cpu in range(count))
            self.assertTrue(bridge.multicore_evidence(log, count)['multicore_verified'])
            self.assertFalse(bridge.multicore_evidence(log)['multicore_verified'])
            self.assertFalse(bridge.multicore_evidence(log.replace('[ARM SMP] timer cpu=0\n', ''), count)['multicore_verified'])
            if count > 1:
                self.assertFalse(bridge.multicore_evidence(log.replace('[ARM SMP] ipi cpu=1\n', ''), count)['multicore_verified'])
        self.assertFalse(bridge.multicore_evidence(log, 4)['multicore_verified'])

    def test_echoed_probe_command_is_not_interactive_evidence(self):
        self.assertFalse(bridge.userland_evidence('printf "MI6_BASH_VERIFIED version=%s uid=%s pid=%s cwd=%s"')['serial_bash_interactive'])
        self.assertTrue(bridge.userland_evidence('MI6_BASH_VERIFIED version=5.2.21(1)-release uid=0 pid=42 cwd=/')['serial_bash_interactive'])

    def test_watchdog_recovery_requires_a_later_fastboot_banner(self):
        initial = b'Entering UEFI Fastboot\r\n'
        running = initial + b'[XNU bridge] watchdog seconds=0x78\r\n'
        self.assertFalse(bridge.watchdog_recovered(initial))
        self.assertFalse(bridge.watchdog_recovered(running))
        self.assertTrue(bridge.watchdog_recovered(running + initial))
        self.assertTrue(bridge.watchdog_recovered(b'watchdog self-test begin\n' + initial))
        self.assertFalse(bridge.userland_evidence('backboardd has not run since boot / load')['watchdog_backboardd_timeout'])
        self.assertTrue(bridge.userland_evidence('Debugger message: panic\nPanicked task: launchd')['panic_detected'])
        self.assertTrue(bridge.userland_evidence('panic(cpu 1 caller 0xffff0000): userspace watchdog timeout: no successful checkins from backboardd (0 induced crashes)')['watchdog_backboardd_timeout'])

    def test_exec_attempt_is_not_user_mode_or_shell_execution(self):
        result = bridge.userland_evidence('load_init_program: attempting to load /sbin/launchd\n')
        self.assertTrue(result['init_exec_attempted'])
        self.assertFalse(result['launchd_ignition_entered'])
        self.assertFalse(result['serial_bash_started'])

    def test_mount_and_trust_cache_evidence(self):
        result = bridge.userland_evidence('BSD root: disk6s1\n'
            'apfs_log: disk6s1 mount-complete volume System, requested by: kernel_task (pid 0); parent: kernel_task (pid 0)\n'
            'loaded external trust cache module: 0\nloaded external trust cache module: 1\n'
            'Darwin Ignition Sequence Version 1.0.0\n')
        self.assertTrue(result['apfs_root_mounted'])
        self.assertTrue(result['launchd_ignition_entered'])
        self.assertEqual(result['external_trust_cache_modules'], [0, 1])
        self.assertFalse(result['serial_bash_started'])

    def test_ignition_error_can_precede_bootstrap_and_clean_fsck(self):
        result = bridge.userland_evidence('ignite() returned 62\n'
            'Darwin Bootstrapper Version 7.0.0\n'
            '** QUICKCHECK ONLY; FILESYSTEM CLEAN\n'
            'launchd (mount-phase-1) <Notice>: Doing boot task\n')
        self.assertTrue(result['launchd_bootstrap_entered'])
        self.assertTrue(result['fsck_quickcheck_clean'])
        self.assertTrue(result['mount_phase_1_started'])
        self.assertFalse(result['serial_bash_started'])


if __name__ == '__main__': unittest.main()
