"""XNUProject: build, prepare firmware, inspect devices and run diagnostics."""
import argparse
import json
import os
from pathlib import Path
import subprocess
import sys

from project import ROOT
from project import config
from project.execution import external, invoke, stage

BUILD = {
    'kernel': ('build.toolchain', ['build']),
    'drivers': ('build.toolchain', ['build-drivers']),
    'kernelcache': ('build.toolchain', ['pack-drivers']),
    'bridge': ('build.bridge', []),
    'boot': ('build.boot_image', []),
    'firmware-tools': ('firmware.apfs', ['bootstrap']),
    'usb-storage': ('build.usb_storage', ['build']),
    'gpu-client': ('build.toolchain', ['build-gpu-client']),
    'gpu-metal': ('build.toolchain', ['build-gpu-metal']),
    'gpu-shaders': ('build.toolchain', ['build-gpu-shaders']),
    'wifi-driverkit': ('build.toolchain', ['build-wifi-driverkit']),
    'bluetooth-stack': ('build.toolchain', ['prepare-bluetooth-stack']),
}
ANALYZE = {
    'kernelcache': 'analysis.kernelcache', 'panic': 'analysis.runtime',
    'identity': 'analysis.identity', 'haptics': 'analysis.haptics',
    'osm': 'analysis.osm', 'armv8': 'analysis.armv8', 'cprh': 'analysis.cprh',
    'baseband-dyld': 'analysis.baseband_dyld', 'baseband-abi': 'analysis.baseband_abi',
    'baseband-notifications': 'analysis.baseband_notifications',
}
DEVICE = {
    'flash': ('device.flash', []), 'install': ('device.apfs', ['install']),
    'disk': ('device.apfs', []),
    'cow': ('device.cow', []),
    'storage': ('device.storage', []), 'console': ('device.console', []),
    'usb-console': ('device.usb_console', []),
    'bridge': ('device.bridge', []),
}


def selection(parser):
    for name in ('board', 'firmware', 'profile', 'workspace'):
        parser.add_argument('--' + name, default=argparse.SUPPRESS,
                            help={'board': 'board ID from boards/', 'firmware': 'Apple firmware ID from firmware/',
                                  'profile': 'named board profile', 'workspace': 'output workspace override'}[name])


def parser():
    root = argparse.ArgumentParser(prog='./project.sh', description=__doc__,
        epilog='Inputs: board / platform / firmware configuration. Outputs: build/<board>/<firmware>/<profile>/. Component options follow the command.')
    selection(root)
    commands = root.add_subparsers(dest='command', required=True)
    for name, help_text in (
        ('configure', 'save board, firmware and profile selection'),
        ('config', 'show the resolved selection; --list shows supported descriptions'),
        ('doctor', 'check host tools and selected input paths'),
        ('bootstrap', 'prepare pinned tools and kernel SDK'),
        ('build', 'compile components or assemble the complete images'),
        ('firmware', 'inspect original IPSW, prepare filesystems and package images'),
        ('device', 'USB Fastboot, storage installation and console commands'),
        ('analyze', 'offline analysis with explicit input files'),
        ('verify', 'verify kernel, drivers, kernelcache or firmware outputs')):
        child = commands.add_parser(name, help=help_text, description=help_text)
        selection(child)
        child.epilog = 'Inputs and outputs: use the command-specific --help after selecting a component.'
        if name in ('config', 'configure'):
            child.add_argument('--list', action='store_true', help='list board and firmware descriptions without modifying files')
        elif name == 'build':
            child.add_argument('component', nargs='?', default='complete', choices=[*BUILD, 'edk2', 'client', 'complete'])
            child.add_argument('-j', '--jobs', type=int, default=3)
            child.add_argument('--ipsw', type=Path, help='original IPSW for the complete build')
        elif name == 'firmware':
            child.add_argument('action', choices=['inspect', 'prepare', 'build', 'image', 'verify', 'release', 'package', 'bridge', 'data-policy'])
        elif name == 'device':
            child.add_argument('action', choices=['info', *DEVICE])
        elif name == 'analyze':
            child.add_argument('subject', choices=ANALYZE)
        elif name == 'verify':
            child.add_argument('component', choices=['kernel', 'drivers', 'kernelcache', 'firmware'])
    return root


def activate(args):
    selected = config.select(**{key: getattr(args, key, None) for key in ('board', 'firmware', 'profile', 'workspace')})
    from project.build import context
    context.configure(selected)
    record = config.record(selected)
    os.environ['XNU_PROJECT_CONFIG'] = str(record)
    os.environ['XNU_PROJECT_WORKSPACE'] = selected['workspace']
    print(f"Board: {selected['board']}; firmware: {selected['firmware']}; profile: {selected['profile']}; output: {selected['workspace']}")
    return selected


def main(argv=None):
    arguments = list(sys.argv[1:] if argv is None else argv)
    # Component help must work without a configured workspace or device access.
    if '--help' in arguments and len(arguments) >= 3:
        group, component = arguments[:2]
        target = None
        if group == 'build' and component in BUILD:
            target = BUILD[component]
        elif group == 'device' and component in DEVICE:
            target = DEVICE[component]
        elif group == 'firmware' and component != 'inspect':
            target = ('firmware.data_policy', []) if component == 'data-policy' else ('firmware.image', [component])
        elif group == 'analyze' and component in ANALYZE:
            target = (ANALYZE[component], [])
        if target:
            module, prefix = target
            invoke('project.' + module, [*prefix, *arguments[2:]])
            return 0
        if group == 'firmware' and component == 'inspect':
            from project.firmware.inspect import main as inspect_firmware
            return inspect_firmware(['--help'])
    command_parser = parser()
    args, extra = command_parser.parse_known_args(arguments)
    if args.command == 'build' and args.ipsw is not None:
        if args.component != 'complete':
            command_parser.error('--ipsw requires the complete build')
        extra = ['--ipsw', str(args.ipsw), *extra]
    try:
        if args.command in ('config', 'configure'):
            if extra:
                command_parser.error('unknown arguments: ' + ' '.join(extra))
            if args.list:
                print(json.dumps(config.available(), indent=2))
                return 0
            selected = config.select(**{key: getattr(args, key, None) for key in ('board', 'firmware', 'profile', 'workspace')})
            if args.command == 'configure':
                config.write_selection(selected)
            print(json.dumps(selected, indent=2))
            return 0
        if args.command == 'analyze':
            invoke('project.' + ANALYZE[args.subject], extra)
            return 0
        if args.command == 'device':
            if args.action == 'info':
                external(['fastboot', 'devices', *extra])
            else:
                module, prefix = DEVICE[args.action]
                invoke('project.' + module, prefix + extra)
            return 0
        if args.command == 'firmware' and args.action == 'inspect':
            from project.firmware.inspect import main as inspect_firmware
            return inspect_firmware(extra)
        selected = activate(args)
        if args.command == 'build' and args.jobs < 1:
            command_parser.error('jobs must be positive')
        name = args.command + ('-' + args.component if hasattr(args, 'component') else '')
        with stage(selected, name):
            if args.command in ('doctor', 'bootstrap'):
                from project.build.references import prepare_platform
                prepare_platform(Path(selected['platform_dir']), download=args.command == 'bootstrap')
                if args.command == 'doctor':
                    from project.build.board_inputs import check
                    check(selected)
                    print('Board firmware inputs: hashes verified', flush=True)
                invoke('project.build.toolchain', [args.command, *extra])
            elif args.command == 'build':
                if args.component == 'complete':
                    from project.build.pipeline import complete
                    complete(selected, args.jobs, extra)
                elif args.component == 'edk2':
                    script = Path(selected['board_dir']) / selected['board_config']['edk2_builder']
                    external(['bash', script, *extra], cwd=ROOT)
                elif args.component == 'client':
                    if not extra or extra[0] not in ['power', 'usb', 'battery', 'haptics', 'input', 'wifi', 'bluetooth', 'display', 'wallpaper', 'gyro', 'gpu']:
                        raise ValueError('build client requires a client name: power, usb, battery, haptics, input, wifi, bluetooth, display, wallpaper, gyro, gpu')
                    invoke('project.build.toolchain', ['build-' + extra[0] + '-client', *extra[1:]])
                else:
                    module, prefix = BUILD[args.component]
                    if module == 'build.toolchain':
                        prefix = [*prefix, '-j', str(args.jobs)]
                        if args.component == 'kernelcache' and '--collection' not in extra:
                            prefix += ['--collection', selected['profile_config']['collection']]
                    if args.component == 'bridge':
                        from project.build import drivers, context
                        collection = drivers.driver_configuration()['collections'][context.COLLECTION]
                        prefix += ['--dts', selected['device_tree'], '--cpus', str(selected['profile_config']['cpus']),
                                   '--kernel', collection['output'], '--output', str(Path(selected['workspace']) / 'obj/bridge')]
                    if args.component == 'boot':
                        directory = Path(selected['workspace'])
                        prefix += ['--edk2', str(directory / 'obj/edk2' / selected['board_config']['edk2_boot_image']),
                                   '--bridge', str(directory / 'obj/bridge/XnuBridge.efi'), '--output', str(directory / 'output/boot.img')]
                    invoke('project.' + module, prefix + extra)
            elif args.command == 'firmware' and args.action == 'data-policy':
                invoke('project.firmware.data_policy', extra)
            elif args.command == 'firmware':
                from project.build.pipeline import firmware_command
                firmware_command(selected, args.action, extra)
            elif args.command == 'verify':
                if args.component == 'firmware':
                    from project.build.pipeline import firmware_command
                    firmware_command(selected, 'verify', extra)
                else:
                    command = {'kernel': 'verify', 'drivers': 'verify-drivers', 'kernelcache': 'verify-collection'}[args.component]
                    defaults = ['--collection', selected['profile_config']['collection']] if args.component == 'kernelcache' else []
                    invoke('project.build.toolchain', [command, *defaults, *extra])
        return 0
    except (OSError, ValueError, KeyError, RuntimeError, subprocess.SubprocessError) as error:
        print(f'XNUProject: {error}', file=sys.stderr)
        return 1
