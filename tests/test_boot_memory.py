"""Exercise firmware RAM range merging without touching device memory."""
from pathlib import Path
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]


class BootMemoryTests(unittest.TestCase):
    def test_sparse_ranges_preserve_holes_and_page_boundaries(self):
        source = r'''
#include <assert.h>
#include "memory_ranges.h"
int main(void) {
    struct boot_memory_range ranges[BOOT_MEMORY_MAX_RANGES];
    unsigned n = 0;
    assert(!boot_memory_add(ranges, &n, 0x100000000ULL, 0x180000000ULL));
    assert(!boot_memory_add(ranges, &n, 0x40000000, 0x80000000));
    assert(!boot_memory_add(ranges, &n, 0x80000000, 0x81ac0000));
    assert(!boot_memory_add(ranges, &n, 0xa0000000, 0xffe50000));
    assert(n == 3 && ranges[0].start == 0x40000000 && ranges[0].end == 0x81ac0000);
    assert(ranges[1].start == 0xa0000000 && ranges[1].end == 0xffe50000);
    assert(ranges[2].start == 0x100000000ULL && ranges[2].end == 0x180000000ULL);
    assert(!boot_memory_add(ranges, &n, 0x96001000, 0x97003000));
    assert(n == 4 && ranges[1].start == 0x96004000 && ranges[1].end == 0x97000000);
    assert(!boot_memory_add(ranges, &n, 0x41000000, 0x42000000));
    assert(n == 4 && ranges[0].end == 0x81ac0000);
    assert(!boot_memory_add(ranges, &n, 0x90001000, 0x90002000));
    assert(n == 4);
    /* A 4 KiB retained table excludes its entire 16 KiB CPU alias. */
    assert(!boot_memory_remove(ranges, &n, 0x44001000, 4096));
    assert(n == 5 && ranges[0].end == 0x44000000 && ranges[1].start == 0x44004000);
    assert(!boot_memory_remove(ranges, &n, 0x40000000, 0x4000));
    assert(ranges[0].start == 0x40004000);
    assert(!boot_memory_remove(ranges, &n, 0x44004000, 0x3dac0000));
    assert(n == 4 && ranges[1].start == 0x96004000);
    assert(boot_memory_remove(ranges, &n, UINT64_MAX - 4095, 4096));
    return 0;
}
'''
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory)
            (path / 'test.c').write_text(source)
            subprocess.run(['cc', '-std=c11', '-Wall', '-Wextra', '-Werror',
                            '-I' + str(ROOT / 'boot/bridge'), str(path / 'test.c'),
                            '-o', str(path / 'test')], check=True)
            subprocess.run([str(path / 'test')], check=True)
