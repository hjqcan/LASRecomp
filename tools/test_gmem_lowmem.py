"""The guest heap goes lazy where its system memory heap can't hold one import (issue #205), and only there."""
import os
from pathlib import Path
import re
import shlex
import subprocess
import tempfile

root = Path(__file__).resolve().parents[1]
source = (root / 'runtime/nvn/vulkan/device.c').read_text()
helper = source[source.index('typedef struct { uint64_t size, budget;'):source.index('\n// Whether one import of the whole heap fits')]
heap_max = re.search(r'#define HEAP_MAX\s+(0x[0-9A-Fa-f]+)ull', (root / 'runtime/rt.h').read_text()).group(1)
harness = r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#define HEAP_MAX ''' + heap_max + r'''ull
#define MB (1ull << 20)
''' + helper + r'''
static bool lazy(const ImportHeap *h, uint32_t n) {
    uint64_t room = 0;
    int heap = import_heap_room(h, n, &room);
    return heap >= 0 && room < HEAP_MAX + IMPORT_MARGIN;
}
int main(void) {
    // issue #205: Ryzen 5 5500U, AMD Windows driver, 8 GB (5.8 usable)
    ImportHeap apu[] = {{1792 * MB, 1702 * MB, true, true}, {2738 * MB, 2601 * MB, false, true}, {256 * MB, 243 * MB, true, true}};
    uint64_t room = 0;
    assert(import_heap_room(apu, 3, &room) == 1 && room == 2601 * MB);
    assert(lazy(apu, 3));
    // the same heaps with no budget extension: the heap size alone is too small too
    ImportHeap apu_nobudget[] = {{1792 * MB, 0, true, true}, {2738 * MB, 0, false, true}, {256 * MB, 0, true, true}};
    assert(lazy(apu_nobudget, 3));
    // RTX, 32 GB: one import as before
    ImportHeap rtx[] = {{12 * 1024 * MB, 11 * 1024 * MB, true, false}, {16 * 1024 * MB, 15 * 1024 * MB, false, true}, {214 * MB, 200 * MB, true, true}};
    assert(!lazy(rtx, 3));
    // 8 GB desktop with its own GPU (system heap = half the RAM): one import as before
    ImportHeap dgpu8[] = {{6 * 1024 * MB, 5800 * MB, true, false}, {3950 * MB, 3840 * MB, false, true}, {256 * MB, 240 * MB, true, true}};
    assert(!lazy(dgpu8, 3));
    // ... but close: one import is tried at the start (exec.c whole_import_probe_oom), lazy chunks if it runs out
    assert(import_heap_room(dgpu8, 3, &room) == 1 && room < IMPORT_PROBE_BELOW);
    // the RTX has plenty: no probe, the device as before
    assert(import_heap_room(rtx, 3, &room) == 1 && room >= IMPORT_PROBE_BELOW);
    // a big system heap whose budget other programs took: lazy
    ImportHeap busy[] = {{8 * 1024 * MB, 7 * 1024 * MB, true, false}, {8 * 1024 * MB, 3000 * MB, false, true}};
    assert(lazy(busy, 2));
    // unified memory, every heap device-local: the largest host-visible one decides
    ImportHeap uma_big[] = {{24 * 1024 * MB, 0, true, true}, {256 * MB, 0, true, true}};
    assert(import_heap_room(uma_big, 2, &room) == 0 && !lazy(uma_big, 2));
    ImportHeap uma_small[] = {{3 * 1024 * MB, 0, true, true}};
    assert(lazy(uma_small, 1));
    // no host-visible heap at all: no opinion (the import fails on its own)
    ImportHeap none[] = {{4 * 1024 * MB, 0, true, false}};
    assert(import_heap_room(none, 1, &room) < 0 && !lazy(none, 1));
    puts("PASS: issue #205 laptop lazy; RTX, 8 GB desktop and big unified memory keep one import, 8 GB desktop probed");
}
'''
with tempfile.TemporaryDirectory(prefix='gmem-lowmem-') as temporary:
    temporary = Path(temporary)
    cfile = temporary / 'lowmem.c'; cfile.write_text(harness)
    exe = temporary / ('lowmem.exe' if os.name == 'nt' else 'lowmem')
    subprocess.run(shlex.split(os.environ.get('CC', 'clang')) + ['-std=gnu11', str(cfile), '-o', str(exe)], check=True)
    subprocess.run([str(exe)], check=True, timeout=5)
