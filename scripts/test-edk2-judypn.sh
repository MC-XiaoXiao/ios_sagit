#!/usr/bin/env bash
set -euo pipefail
task_root=$(cd -- "$(dirname -- "$0")/.." && pwd)
task_edk="$task_root/boot/edk2-sdm845/Common/edk2"
task_build="$task_root/build/edk2-judypn"
mkdir -p "$task_build"
gcc -std=gnu17 -O1 -g -fshort-wchar -ffunction-sections -fdata-sections \
    -fsanitize=address,undefined --param=asan-globals=0 -fno-omit-frame-pointer \
    -I"$task_edk/MdePkg/Include" -I"$task_edk/MdePkg/Include/X64" \
    -I"$task_edk/EmbeddedPkg/Include" \
    "$task_root/tests/edk2_judypn_fastboot_test.c" \
    -Wl,--gc-sections -lz -o "$task_build/fastboot-policy-test"
"$task_build/fastboot-policy-test"
