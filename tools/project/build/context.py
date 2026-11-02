"""Paths and options for one host command invocation."""
import os
import json
from pathlib import Path
import threading
from project import ROOT

SRC = ROOT / 'kernel'
TOOLS = ROOT / 'tools'
PREFIX = ROOT / 'tools/prefix'
BIN = PREFIX / 'bin'
BUILD = ROOT / 'build'
SDK = BUILD / 'sdk/iPhoneOS18.6.kernel.sdk'
OBJ = BUILD / 'obj'
TARGET = os.environ.get('XNU_MACHINE_CONFIG', 'SDM835')
CONFIG = OBJ / ('DEVELOPMENT_ARM64_' + TARGET)
KERNEL_FILE = 'mach.development.' + TARGET.lower()
NATIVE_KERNELCACHE = None
DRIVERS_ROOT = ROOT / 'drivers'
DRIVER_FILTERS = []
DRIVER_OUTPUT = BUILD / 'output/Extensions'
SELF = ROOT / 'tools/project/build/toolchain.py'
LOCK = ROOT / 'tools/dependencies.json'
JOBS = max(1, min(4, os.cpu_count() or 1))
ABI_ANALYSIS_LOCK = threading.Lock()
ABI_ANALYSES = {}
FIRMWARE_DIR = None
ABI_ROOT = None
FIRMWARE_INCLUDE = None
PROVIDER_IMAGE = None
COLLECTION = None
STAGE = None


def configure(config):
    """Set paths once, before invoking a build component."""
    global BUILD, SDK, OBJ, TARGET, CONFIG, KERNEL_FILE, DRIVER_OUTPUT
    global FIRMWARE_DIR, ABI_ROOT, FIRMWARE_INCLUDE, PROVIDER_IMAGE, COLLECTION
    BUILD = Path(config['workspace'])
    SDK = ROOT / 'build/cache/sdk/iPhoneOS18.6.kernel.sdk'
    OBJ = BUILD / 'obj/kernel'
    TARGET = config['target']
    CONFIG = OBJ / ('DEVELOPMENT_ARM64_' + TARGET)
    KERNEL_FILE = 'mach.development.' + TARGET.lower()
    DRIVER_OUTPUT = BUILD / 'output/Extensions'
    FIRMWARE_DIR = Path(config['firmware_dir'])
    firmware = config['firmware_config']
    ABI_ROOT = FIRMWARE_DIR / firmware['abi']
    FIRMWARE_INCLUDE = FIRMWARE_DIR / firmware['include']
    PROVIDER_IMAGE = BUILD / 'obj/firmware' / (firmware['kernelcache'] + '.macho')
    COLLECTION = config['profile_config'].get('collection')


if os.environ.get('XNU_PROJECT_CONFIG'):
    configure(json.loads(Path(os.environ['XNU_PROJECT_CONFIG']).read_text()))
