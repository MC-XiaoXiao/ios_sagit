#!/usr/bin/env python3
"""Stage an EFI bridge without flashing, capture UART, and record evidence."""
import argparse
import fcntl
import json
import hashlib
import sys
import os
from pathlib import Path
import re
import select
import subprocess
import termios
import time
import zlib
import statistics
import secrets


class UARTTransportError(RuntimeError):
    pass


def uart_readable(fd, timeout):
    # select() reports a disconnected USB tty as readable forever; read() may
    # then return an empty buffer. Detect the actual hangup/error so an old fd
    # cannot turn a host USB disconnect into a GPU diagnostic timeout.
    poller = select.poll()
    poller.register(fd, select.POLLIN)
    events = poller.poll(max(0, int(timeout * 1000)))
    for _, flags in events:
        if flags & (select.POLLHUP | select.POLLERR | select.POLLNVAL):
            raise UARTTransportError(f'UART transport disconnected (poll flags=0x{flags:x}); no further console input')
    return any(flags & select.POLLIN for _, flags in events)


GPU_CLOCK_ROWS = re.compile(rb'QcomGPU: (?:cpu-inherited-clock|clock-observation|execution-observation|core-clock-observation|rbbm-busy-observation|gpucc-clock-tuple|render-work-observation) [^\r\n]*\r?\n')


def console_reply(uart, index, token=None):
    """Decode an actual shell reply, preserving raw capture and fault logs."""
    suffix = b' token=' + re.escape(token) if token is not None else b''
    pattern = rb'(?:^|[\r\n])DEV_CONSOLE_RESULT index=' + str(index).encode('ascii') + rb' status=(\d+)' + suffix + rb'(?:\r|\n)'
    reply = re.search(pattern, uart)
    if reply:
        return reply, 0
    # These complete periodic GPU observer rows can interrupt the shell's
    # write between any two characters. Remove only their exact known tags
    # from the matching view, never from uart.bin or the printed/raw log.
    view, removed = GPU_CLOCK_ROWS.subn(b'', uart)
    return re.search(pattern, view), removed


def console_request(command, index, token):
    # Diagnostics and kernel messages do not always terminate their last line.
    # Emit our own leading newline while expanding the command's original $?;
    # otherwise a successful command can wait for the full timeout unnoticed.
    return (command + f'; printf "\\nDEV_CONSOLE_RESULT index={index} status=%s token={token.decode("ascii")}\\n" "$?"\r').encode('ascii')


def bash_reply(uart, token):
    pattern = rb'(?:^|[\r\n])DEV_BASH_VERIFIED version=[0-9][^\s]* uid=0 pid=[1-9][0-9]* cwd=[^\s]+ token=' + re.escape(token) + rb'(?:\r|\n)'
    return re.search(pattern, uart) is not None or re.search(pattern, GPU_CLOCK_ROWS.sub(b'', uart)) is not None


def panic_evidence(clean):
    header = re.search(r'panic\(cpu \d+ caller 0x[0-9a-fA-F]+\): software panic output self-test: value=42 hex=0x835', clean) is not None
    frames = re.findall(r'lr: 0x[0-9a-fA-F]+\s+fp: 0x[0-9a-fA-F]+', clean)
    metadata = all(s in clean for s in ('Debugger message: panic', 'Kernel version: Darwin Kernel Version', 'Panicked thread:'))
    completed = re.search(r'\*\* Stackshot (?:Succeeded|Incomplete) \*\*|!! Stackshot Failed !!', clean) is not None
    blocked = any(s in clean for s in ('spinning forever...', 'gPanicBase is still not initialized', 'nested panic detected'))
    return dict(native_panic_header=header, native_backtrace_frames=len(frames),
                native_panic_metadata=metadata, native_panic_report_completed=completed,
                software_panic=header and len(frames) >= 3 and metadata and completed and not blocked)


def userland_evidence(clean):
    """Keep exec attempts, actual user-mode output and service startup distinct."""
    return {
        'apfs_root_mounted': 'BSD root:' in clean and re.search(
            r'mount-complete volume [^\r\n]+requested by: kernel_task \(pid 0\)', clean) is not None,
        'init_exec_attempted': 'load_init_program: attempting to load /sbin/launchd' in clean,
        'launchd_ignition_entered': 'Darwin Ignition Sequence Version' in clean,
        'launchd_bootstrap_entered': 'Darwin Bootstrapper Version' in clean,
        'fsck_quickcheck_clean': 'QUICKCHECK ONLY; FILESYSTEM CLEAN' in clean,
        'mount_phase_1_started': re.search(r'\(mount-phase-1\)[^\r\n]*Doing boot task', clean) is not None,
        'serial_bash_started': 'Mi 6 XNU serial Bash (development console)' in clean,
        'serial_bash_interactive': re.search(r'(?:MI6|DEV)_BASH_VERIFIED version=[0-9][^\s]* uid=0 pid=[1-9][0-9]* cwd=/', clean) is not None,
        'external_trust_cache_modules': [int(i) for i in re.findall(
            r'loaded external trust cache module: (\d+)', clean)],
        'generic_aes_cbc_self_test': 'GenericAES: corecrypto AES-CBC self-test passed' in clean,
        'panic_detected': re.search(r'panic\(cpu \d+ caller ', clean) is not None or 'Debugger message: panic' in clean,
        'watchdog_backboardd_timeout': re.search(
            r'panic\(cpu \d+ caller [^\r\n]+userspace watchdog timeout: no successful checkins from backboardd', clean) is not None,
    }


def watchdog_recovered(uart):
    """A startup Fastboot banner must not terminate the later XNU capture."""
    markers = (b'watchdog seconds=', b'watchdog self-test begin')
    start = max(uart.find(marker) for marker in markers)
    return start >= 0 and b'Entering UEFI Fastboot' in uart[start:]


def multicore_evidence(clean, expected=None):
    """CPU_ON success alone does not prove that a CPU reached XNU or took an IRQ."""
    stages = {stage: sorted({int(cpu) for cpu in re.findall(
        rf'\[ARM SMP\] {stage} cpu=(\d+)\b', clean)})
        for stage in ('registered', 'online', 'interrupts', 'timer', 'ipi')}
    required = set(range(expected)) if expected is not None else None
    passed = required is not None and all(set(stages[stage]) == required
        for stage in ('registered', 'online', 'interrupts', 'timer'))
    if required is not None and expected > 1:
        passed = passed and required.issubset(stages['ipi'])
    return {'cpu_count_expected': expected,
            **{stage + '_cpus': cpus for stage, cpus in stages.items()},
            'multicore_verified': passed}


def bash_benchmark_command(jobs=8, loops=20000, repeats=3):
    """Fixed total work and verified child exit codes; only Bash builtins."""
    if not 1 <= jobs <= 8 or not 1 <= loops <= 100000 or not 1 <= repeats <= 5:
        raise ValueError('invalid Bash benchmark dimensions')
    value = 1
    for _ in range(loops):
        value = (value * 1664525 + 1013904223) & 2147483647
    return (f'for ((arm_bench_r=1;arm_bench_r<={repeats};arm_bench_r++)); do '
        'arm_bench_start=${EPOCHREALTIME/./}; arm_bench_pids=(); '
        f'for ((arm_bench_j=0;arm_bench_j<{jobs};arm_bench_j++)); do '
        f'(arm_bench_x=1; for ((arm_bench_i=0;arm_bench_i<{loops};arm_bench_i++)); do '
        '((arm_bench_x=(arm_bench_x*1664525+1013904223)&2147483647)); done; '
        f'[[ $arm_bench_x -eq {value} ]]) & arm_bench_pids+=($!); done; '
        'arm_bench_ok=1; for arm_bench_pid in "${arm_bench_pids[@]}"; do '
        'wait "$arm_bench_pid" || arm_bench_ok=0; done; arm_bench_end=${EPOCHREALTIME/./}; '
        f'printf "\\nARM_SMP_BENCH r=%s elapsed_us=%s jobs={jobs} loops={loops} verified=%s\\n" '
        '"$arm_bench_r" "$((arm_bench_end-arm_bench_start))" "$arm_bench_ok"; done; '
        'printf "ARM_SMP_BENCH_DONE\\n"')


def benchmark_evidence(clean, jobs=8, loops=20000, repeats=3):
    rows = [{'repeat': int(r), 'elapsed_us': int(t), 'jobs': int(j), 'loops': int(n), 'verified': ok == '1'}
            for r, t, j, n, ok in re.findall(
                r'(?:^|[\r\n])ARM_SMP_BENCH r=(\d+) elapsed_us=(\d+) jobs=(\d+) loops=(\d+) verified=([01])', clean)]
    complete = re.search(r'(?:^|[\r\n])ARM_SMP_BENCH_DONE(?:[\r\n]|$)', clean) is not None
    valid = complete and len(rows) == repeats and {r['repeat'] for r in rows} == set(range(1, repeats + 1)) and all(
        r['jobs'] == jobs and r['loops'] == loops and r['verified'] and r['elapsed_us'] > 0 for r in rows)
    return {'benchmark_samples': rows, 'benchmark_verified': valid,
            'benchmark_median_us': statistics.median(r['elapsed_us'] for r in rows) if valid else None}


def boot_timing_evidence(uart, events, payload_start=0):
    """Host UART timestamps, excluding transfer and any preceding boot."""
    patterns = {
        'efi_entry': re.escape(b'[XNU bridge] EDK2 entry'),
        'kernel_entry': re.escape(b'[XNU] start_first_cpu'),
        'bash_banner': re.escape(b'Mi 6 XNU serial Bash (development console)'),
        'bash_verified': rb'(?:MI6|DEV)_BASH_VERIFIED version=[0-9][^\s]* uid=0 pid=[1-9][0-9]* cwd=/',
    }
    times = {}
    for name, pattern in patterns.items():
        match = re.search(pattern, uart[payload_start:])
        if match:
            end = payload_start + match.end()
            times[name] = next((event['seconds'] for event in events
                                if event['end_offset'] >= end), None)
    def elapsed(first, last):
        if times.get(first) is None or times.get(last) is None:
            return None
        return round(times[last] - times[first], 3)
    return {'capture_seconds': times,
            'efi_to_bash_seconds': elapsed('efi_entry', 'bash_banner'),
            'kernel_to_bash_seconds': elapsed('kernel_entry', 'bash_banner'),
            'efi_to_verified_bash_seconds': elapsed('efi_entry', 'bash_verified')}


def expected_watchdog_result(report, expected_cpus=None, benchmark=False):
    return (all(report.get(key) for key in ('serial_bash_interactive',
                'watchdog_backboardd_timeout', 'native_panic_report_completed', 'watchdog_recovered'))
            and (expected_cpus is None or report.get('multicore_verified'))
            and (not benchmark or report.get('benchmark_verified')))


def main():
    p = argparse.ArgumentParser()
    p.add_argument('--payload', type=Path, default=Path('build/bridge/XnuBridge.efi'))
    p.add_argument('--profile', type=Path, default=Path('tools/device-profiles/sagit.json'))
    p.add_argument('--output', type=Path, default=Path('build/bridge/hardware'))
    p.add_argument('--from-shell', action='store_true')
    p.add_argument('--attach-running', action='store_true',
                   help='capture an already running system; verify root Bash, skip all Fastboot operations')
    p.add_argument('--observe-only', action='store_true',
                   help='passively capture UART; no serial writes, Bash probe or Fastboot operation, no payload identity claim')
    p.add_argument('--running-report', type=Path,
                   help='preceding boot report required by --attach-running; recovered firmware is rejected')
    p.add_argument('--seconds', type=float, default=60)
    p.add_argument('--iterations', type=int, default=1)
    p.add_argument('--bash-probe', action='store_true', help='verify a Bash command over UART after its startup banner')
    p.add_argument('--console-command', action='append', default=[],
                   help='run a diagnostic Bash command after the interactive probe; may be repeated')
    p.add_argument('--console-command-timeout', type=float, default=90,
                   help='stop further input if a diagnostic command has no completion reply in this many seconds')
    p.add_argument('--stop-after-console', action='store_true',
                   help='finish capture after every diagnostic command has actually returned')
    p.add_argument('--console-byte-delay', type=float,
                   help='send console input one byte at a time with this delay in seconds')
    p.add_argument('--expect-cpus', type=int, choices=range(1, 9), help='require live XNU and timer/IPI evidence for this CPU count')
    p.add_argument('--benchmark', action='store_true', help='after Bash verification, time the same eight-worker workload three times')
    p.add_argument('--expect-backboardd-watchdog', action='store_true',
                   help='require interactive Bash followed by the native backboardd timeout panic and firmware recovery')
    args = p.parse_args(); profile = json.loads(args.profile.read_text())
    if args.console_byte_delay is not None and not 0 < args.console_byte_delay <= 1:
        p.error('--console-byte-delay must be positive and at most one second')
    if args.console_command_timeout <= 0:
        p.error('--console-command-timeout must be positive')
    if args.stop_after_console and not args.console_command:
        p.error('--stop-after-console requires --console-command')
    if args.benchmark or args.expect_backboardd_watchdog or args.console_command: args.bash_probe = True
    if args.iterations < 1: p.error('--iterations must be positive')
    if args.observe_only and (args.from_shell or args.attach_running or args.running_report is not None or
            args.iterations!=1 or args.bash_probe or args.console_command or args.expect_cpus is not None or
            args.benchmark or args.expect_backboardd_watchdog or args.stop_after_console):
        p.error('--observe-only cannot be combined with boot, identity, probe, console or verification actions')
    if args.attach_running:
        if args.from_shell or args.iterations != 1 or args.expect_cpus is not None:
            p.error('--attach-running cannot reboot firmware, repeat boots or verify CPU bring-up')
        if args.running_report is None:
            p.error('--attach-running requires --running-report')
        previous = json.loads(args.running_report.read_text())
        if (not previous.get('kernel_serial') or previous.get('panic_detected') or
                previous.get('watchdog_recovered') or previous.get('payload_sha256') != hashlib.sha256(args.payload.read_bytes()).hexdigest()):
            p.error('preceding report does not describe this payload still running in XNU')
        args.bash_probe = True
    elif args.running_report is not None:
        p.error('--running-report requires --attach-running')
    if args.iterations > 1:
        args.output.mkdir(parents=True, exist_ok=True); rounds = []
        for i in range(args.iterations):
            dest = args.output/f'round-{i+1}'
            cmd = [sys.executable, __file__, '--payload', str(args.payload), '--profile', str(args.profile),
                   '--output', str(dest), '--seconds', str(args.seconds)]
            if args.from_shell and i == 0: cmd.append('--from-shell')
            if args.bash_probe: cmd.append('--bash-probe')
            if args.console_byte_delay is not None:
                cmd += ['--console-byte-delay', str(args.console_byte_delay)]
            cmd += ['--console-command-timeout', str(args.console_command_timeout)]
            if args.stop_after_console: cmd.append('--stop-after-console')
            if args.expect_cpus is not None: cmd += ['--expect-cpus', str(args.expect_cpus)]
            if args.benchmark: cmd.append('--benchmark')
            for diagnostic in args.console_command: cmd += ['--console-command', diagnostic]
            if args.expect_backboardd_watchdog: cmd.append('--expect-backboardd-watchdog')
            result = subprocess.run(cmd)
            report = json.loads((dest/'report.json').read_text()) if (dest/'report.json').exists() else {}
            rounds.append({'index': i+1, 'returncode': result.returncode, 'report': report})
            if result.returncode or not report.get('watchdog_recovered'): break
        success = len(rounds) == args.iterations and all(r['returncode'] == 0 and r['report'].get('watchdog_recovered') for r in rounds)
        (args.output/'report.json').write_text(json.dumps({'success': success, 'rounds': rounds}, indent=2)+'\n')
        return 0 if success else 1
    args.output.mkdir(parents=True, exist_ok=True)
    fd = os.open(profile['uart'], os.O_RDWR|os.O_NOCTTY|os.O_NONBLOCK)
    try:
        # Every invocation shares the physical UART; never consume another run's log.
        fcntl.flock(fd, fcntl.LOCK_EX | fcntl.LOCK_NB)
    except BlockingIOError:
        os.close(fd)
        p.error('UART is in use by another bridge test; retry after it completes')
    a = termios.tcgetattr(fd); a[0] = a[1] = a[3] = 0
    a[2] = termios.CS8|termios.CREAD|termios.CLOCAL; a[4] = a[5] = termios.B115200
    a[6][termios.VMIN] = a[6][termios.VTIME] = 0; termios.tcsetattr(fd, termios.TCSANOW, a)
    uart = bytearray(); commands = []; report = {}; events = []; started = time.monotonic()
    metadata_path = args.payload.parent/'build.json'
    metadata = json.loads(metadata_path.read_text()) if metadata_path.exists() and not args.attach_running and not args.observe_only else {}
    requested_hash = None if args.observe_only else hashlib.sha256(args.payload.read_bytes()).hexdigest()
    report['payload_sha256'] = None if args.attach_running or args.observe_only else requested_hash
    if args.observe_only:report['passive_uart_capture']=True
    if args.attach_running:
        report.update(attached_running=True, requested_payload_sha256=requested_hash,
                      running_payload_identity_verified=False)
    probe_sent = False
    probe_retry_after = 0
    probe_attempts = 0
    probe_pending = b''
    probe_next = 0
    payload_running = False
    payload_uart_start = 0
    benchmark_sent = False
    bash_verified = False
    diagnostics_sent = False
    diagnostic_active = None
    diagnostic_deadline = 0
    diagnostic_results = []
    diagnostic_timeout = False
    console_blocked = False
    console_token = secrets.token_hex(8).encode('ascii')
    report['console_reply_token'] = console_token.decode('ascii')
    def capture(timeout=0.1):
        nonlocal probe_sent, probe_retry_after, probe_attempts, probe_pending, probe_next, benchmark_sent, bash_verified, diagnostics_sent, console_blocked
        nonlocal diagnostic_active, diagnostic_deadline, diagnostic_timeout
        if probe_pending:
            timeout = min(timeout, max(0, probe_next-time.monotonic()))
        if uart_readable(fd, timeout):
            data = os.read(fd, 65536); uart.extend(data)
            events.append({'seconds': round(time.monotonic()-started, 3), 'end_offset': len(uart)})
            (args.output/'uart.bin').write_bytes(uart)
            print(data.decode('utf8', 'replace'), end='', flush=True)
            if payload_running:
                current_uart=bytes(uart[payload_uart_start:])
                if b'panic(cpu' in current_uart or b'EFI payload returned:' in current_uart or watchdog_recovered(current_uart):
                    console_blocked=True;probe_pending=b''
                if args.attach_running and re.search(rb'Shell>|Android Fastboot mode|Entering UEFI|UEFI firmware',current_uart):
                    console_blocked=True;probe_pending=b''
            if args.bash_probe and payload_running and not bash_verified and bash_reply(
                    bytes(uart[payload_uart_start:]), console_token):
                bash_verified = True
                progress = {'boot_timing': boot_timing_evidence(bytes(uart), events, payload_uart_start),
                            **multicore_evidence(bytes(uart[payload_uart_start:]).decode('utf8', 'replace'), args.expect_cpus)}
                (args.output/'boot-progress.json').write_text(json.dumps(progress, indent=2)+'\n')
                print('\n[bridge test] Interactive Bash: ' + str(progress['boot_timing']['efi_to_verified_bash_seconds']) + ' seconds from EFI entry', flush=True)
            # A long transfer may finish after its deadline. Retain that
            # actual reply while keeping further input blocked after timeout.
            if diagnostic_active is not None:
                # Match a real reply, not the terminal's echo of the wrapper.
                current_uart = bytes(uart[payload_uart_start:])
                reply, removed = console_reply(current_uart, diagnostic_active, console_token)
                if reply:
                    diagnostic_results.append({'index': diagnostic_active, 'status': int(reply[1]),
                        'seconds': round(time.monotonic()-started, 3),
                        'interleaved_clock_rows_removed_for_reply': removed})
                    diagnostic_active = None
                    (args.output/'console-progress.json').write_text(json.dumps(diagnostic_results, indent=2)+'\n')
            if args.benchmark and payload_running and not console_blocked and not benchmark_sent and not probe_pending and userland_evidence(
                    bytes(uart[payload_uart_start:]).decode('utf8', 'replace'))['serial_bash_interactive']:
                benchmark_sent = True
                probe_pending = (bash_benchmark_command() + '\r').encode('ascii')
        # Probe deadlines must also run when the UART has gone quiet.
        if args.bash_probe and payload_running and not console_blocked and not bash_verified and not probe_pending and (
                not probe_sent or time.monotonic() >= probe_retry_after):
            current_uart = bytes(uart[payload_uart_start:])
            bash_exec_complete = re.search(
                rb'exec image trace: pid=\d+ caller=\d+ path=/usr/bin/bash stage=image activator leave result=0',
                current_uart) is not None
            bash_launch_attempt = b'/usr/bin/bash, launch type' in current_uart
            # Kernel logging can split the banner. A completed exec is
            # enough to try. With exec tracing disabled, the AMFI launch
            # message also permits a probe, but only its reply proves Bash.
            # SERIALMODE_NO_IOLOG can hide the AMFI exec line; launchd's
            # early-boot boundary permits a harmless probe as well. Only
            # the actual Bash reply verifies an interactive console.
            early_boot_complete = b'Early boot complete. Continuing system boot.' in current_uart
            if args.attach_running or b'Mi 6 XNU serial Bash (development console)' in current_uart or bash_exec_complete or bash_launch_attempt or early_boot_complete:
                probe_sent = True
                probe_attempts += 1
                # Avoid quotes/backslashes: a dropped input byte must not
                # leave the development console waiting for quote closure.
                probe_pending = b'\recho DEV_BASH_VERIFIED version=$BASH_VERSION uid=$EUID pid=$$ cwd=$PWD token=' + console_token + b'\r'
                probe_retry_after = time.monotonic() + max(30, len(probe_pending) * (args.console_byte_delay or 0.05) + 10)
        if diagnostic_active is not None and not probe_pending and not console_blocked and time.monotonic() >= diagnostic_deadline:
            diagnostic_timeout = console_blocked = True
            print('\n[bridge test] Diagnostic command timed out; further console input stopped', flush=True)
        if args.console_command and bash_verified and not console_blocked and not probe_pending and diagnostic_active is None and not diagnostics_sent:
            diagnostic_active = len(diagnostic_results)
            diagnostic_deadline = time.monotonic() + args.console_command_timeout
            command_text = args.console_command[diagnostic_active]
            probe_pending = console_request(command_text, diagnostic_active, console_token)
            diagnostics_sent = diagnostic_active + 1 == len(args.console_command)
        # The kernel polls RX every 16 ms. Pace input to avoid FIFO overrun.
        if probe_pending and time.monotonic() >= probe_next:
            written = os.write(fd, probe_pending[:1 if args.console_byte_delay is not None else 8])
            probe_pending = probe_pending[written:]
            probe_next = time.monotonic() + (args.console_byte_delay if args.console_byte_delay is not None else 0.05)
    def command(*cmd):
        argv = ['fastboot', '-s', profile['firmware_serial'], *map(str, cmd)]
        with (args.output/f'command-{len(commands)}.log').open('w') as log:
            proc = subprocess.Popen(argv, stdout=log, stderr=subprocess.STDOUT)
            end = time.monotonic()+40
            while proc.poll() is None:
                capture()
                if time.monotonic()>end:
                    proc.kill(); proc.wait(); raise TimeoutError(argv)
        text = (args.output/f'command-{len(commands)}.log').read_text()
        commands.append({'argv': argv, 'returncode': proc.returncode, 'output': text})
        print(text, flush=True)
        if proc.returncode: raise RuntimeError(text)
        return text
    try:
        if args.from_shell:
            # A key can be consumed by the firmware's startup.nsh countdown.
            # Wait for a fresh prompt before transmitting the path. RETURN also
            # exits a stale Fastboot instance so its USB transport can restart.
            shell_start = len(uart)
            os.write(fd, b'\r')
            shell_deadline = time.monotonic()+20
            while b'Shell>' not in uart[shell_start:]:
                capture(0.1)
                if time.monotonic() > shell_deadline:
                    raise TimeoutError('UEFI Shell prompt unavailable')
            for byte in b'fs0:\\AndroidFastbootApp.efi\r':
                os.write(fd, bytes([byte])); capture(0.025)
        if args.attach_running or args.observe_only:
            payload_running = True
        else:
            # The profile already identifies the transport. Query the startup
            # Fastboot instance directly; enumerating every USB device adds a
            # separate failure/timeout path and does not verify the product.
            product = command('getvar', 'product')
            if 'product: '+profile['product'] not in product: raise RuntimeError('wrong device')
            command('stage', args.payload)
            crc = command('oem', 'crc32')
            expected = f'{zlib.crc32(args.payload.read_bytes()):08x}'
            if 'crc32='+expected not in crc.lower(): raise RuntimeError('payload CRC mismatch')
            report['crc32'] = expected
            payload_uart_start = len(uart)
            payload_running = True
            command('oem', 'run-efi')
        end = time.monotonic()+args.seconds
        panic_drain_started = None
        while time.monotonic()<end:
            capture()
            if args.attach_running and console_blocked:
                # A UART read can end in the middle of the panic header.
                # Stop console input immediately, but keep a bounded passive
                # tail so a requested reboot retains the native panic data.
                if b'panic(cpu' in uart[payload_uart_start:]:
                    now = time.monotonic()
                    if panic_drain_started is None:
                        panic_drain_started = now
                    if now-panic_drain_started < 15:
                        continue
                    report['terminal_panic_drain_seconds'] = round(now-panic_drain_started, 3)
                break
            if args.stop_after_console and len(diagnostic_results) == len(args.console_command):
                break
            if watchdog_recovered(uart[payload_uart_start:]):
                break
            if b'EFI payload returned:' in uart[payload_uart_start:]:
                break
    except Exception as e:
        report['error'] = str(e)
        if isinstance(e, UARTTransportError):
            console_blocked = True
            report['uart_transport_disconnected'] = True
            report['uart_transport_path'] = profile['uart']
            report['uart_transport_loss_seconds'] = round(time.monotonic()-started, 3)
        raise
    finally:
        os.close(fd)
        clean = re.sub(rb'\x1b\[[0-?]*[ -/]*[@-~]', b'', bytes(uart)).decode('utf8', 'replace')
        (args.output/'uart.txt').write_text(clean)
        # Firmware recovery can consume UART bytes from a preceding boot.
        # Only the bytes after this payload's dispatch count as live evidence.
        current = re.sub(rb'\x1b\[[0-?]*[ -/]*[@-~]', b'', bytes(uart[payload_uart_start:]) if payload_running else b'').decode('utf8', 'replace')
        recovered = watchdog_recovered(bytes(uart[payload_uart_start:])) if payload_running else False
        report.update(commands=commands, events=events, build=metadata, uart_bytes=len(uart),
                      bash_probe_attempts=probe_attempts, console_byte_delay=args.console_byte_delay,
                      console_stopped_after_failure=console_blocked,
                      console_commands=args.console_command,
                      console_results=diagnostic_results,
                      console_command_timeout=diagnostic_timeout,
                      payload_uart_start=payload_uart_start if payload_running else None,
                      watchdog_recovered=recovered,
                      kernel_stages=re.findall(r'\[XNU(?: VM)?\] ([^\r\n]+)', current),
                      exit_boot_services='ExitBootServices OK' in current,
                      kernel_serial=('kprintf initialized' in current or ('Darwin Kernel Version' in current and 'vm_page_bootstrap:' in current)),
                      kernel_entry='[XNU] start_first_cpu' in current,
                      efi_payload_returned=re.findall(r'EFI payload returned: ([^\r\n]+)', current),
                      framebuffer_handoff='[XNU bridge] framebuffer base=' in current,
                      dual_console='[XNU console] UART + framebuffer text output enabled' in current,
                      **panic_evidence(current), **userland_evidence(current))
        report.update(multicore_evidence(current, args.expect_cpus))
        # An observer row can split the raw Bash response. The nonce-aware
        # matcher already verified its normalized view during capture; retain
        # that result rather than reclassifying the same raw bytes as failure.
        if args.bash_probe:
            report['serial_bash_interactive'] = bash_verified
        report.update(benchmark_evidence(current))
        report['boot_timing'] = boot_timing_evidence(bytes(uart), events, payload_uart_start) if payload_running else {}
        report['expected_backboardd_watchdog'] = args.expect_backboardd_watchdog
        if args.expect_backboardd_watchdog:
            report['backboardd_watchdog_test_passed'] = expected_watchdog_result(report, args.expect_cpus, args.benchmark)
        (args.output/'report.json').write_text(json.dumps(report, indent=2)+'\n')
    if metadata.get('panic_test'):
        return 0 if report.get('software_panic') and report.get('watchdog_recovered') else 1
    if metadata.get('watchdog_test'):
        return 0 if report.get('watchdog_recovered') else 1
    if args.expect_backboardd_watchdog:
        return 0 if report.get('backboardd_watchdog_test_passed') else 1
    if args.observe_only:return 0
    return 0 if (args.attach_running or report.get('kernel_serial')) and not report.get('panic_detected') and not diagnostic_timeout and (not args.stop_after_console or len(diagnostic_results) == len(args.console_command)) and (not args.bash_probe or report.get('serial_bash_interactive')) and (args.expect_cpus is None or report.get('multicore_verified')) and (not args.benchmark or report.get('benchmark_verified')) else 1

if __name__ == '__main__': raise SystemExit(main())
