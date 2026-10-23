#!/usr/bin/env bash
# Isolated LG V40 / SDM845 build; never builds in the Mi 6 checkout.
set -euo pipefail
task_root=$(cd -- "$(dirname -- "$0")/.." && pwd)
task_repo="$task_root/boot/edk2-sdm845"
task_build="$task_root/build/edk2-judypn"
mkdir -p "$task_build/host-bin" "$task_build/logs"
exec 9>"$task_build/build.lock"
flock -n 9 || { echo "Another V40 build is running" >&2; exit 1; }
if (( $# != 0 )); then
    echo "Usage: JOBS=3 scripts/build-judypn.sh (fixed judypn, DEBUG, no UART)" >&2
    exit 2
fi
python3 "$task_root/scripts/verify-edk2-judypn.py" --snapshot-mi6
python3 "$task_root/scripts/prepare-edk2-judypn.py" --output "$task_build"
if [[ ! -x "$task_build/venv/bin/python3" ]]; then
    uv venv --python 3.10 "$task_build/venv"
    uv pip install --python "$task_build/venv/bin/python" pillow==12.3.0 GitPython==3.2.0
fi
ln -sfn "$task_root/scripts/edk2-host-gcc.sh" "$task_build/host-bin/gcc"
ln -sfn "$task_root/scripts/edk2-host-gcc.sh" "$task_build/host-bin/cc"
ln -sfn "$task_root/scripts/edk2-target-gcc.sh" "$task_build/host-bin/aarch64-none-linux-gnu-gcc"
export PATH="$task_build/host-bin:$task_build/venv/bin:$PATH"
export CROSS_COMPILE=aarch64-none-linux-gnu-
export PYTHON_COMMAND=python3
export BUILD_JOBS="${JOBS:-3}"
[[ "$BUILD_JOBS" =~ ^[1-9][0-9]*$ ]] || { echo "JOBS must be positive" >&2; exit 2; }
# Avoid inherited setup from another EDK2 build selecting its workspace or tools.
unset WORKSPACE EDK_TOOLS_PATH CONF_PATH PACKAGES_PATH EDK2 EDK2_PLATFORMS SIMPLE_INIT
task_cpus=$(python3 -c 'import os; print(",".join(map(str, sorted(os.sched_getaffinity(0))[:int(os.environ["BUILD_JOBS"])])))')
taskset -c "$task_cpus" bash "$task_repo/build.sh" \
    --device judypn --release DEBUG --toolchain GCC5 --outputdir "$task_build" \
    2>&1 | tee "$task_build/logs/build.log"
task_fv="$task_build/workspace/Build/judypn/DEBUG_GCC5/FV"
cp "$task_fv/SDM845_UEFI.fd" "$task_build/uefi-judypn.fd"
cp "$task_build/workspace/Build/judypn/DEBUG_GCC5/AARCH64/AndroidFastbootApp.efi" "$task_build/AndroidFastbootApp.efi"
cp "$task_build/workspace/Build/judypn/DEBUG_GCC5/AARCH64/FastbootPayloadProbe.efi" "$task_build/FastbootPayloadProbe.efi"
python3 "$task_root/scripts/verify-edk2-judypn.py"
(cd "$task_build" && sha256sum boot-judypn.img uefi-judypn.fd AndroidFastbootApp.efi FastbootPayloadProbe.efi > SHA256SUMS)
