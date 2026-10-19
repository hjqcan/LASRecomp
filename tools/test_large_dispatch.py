"""Execute a large every-instruction dispatch graph through the native compiler."""
from pathlib import Path
import os
import shlex
import subprocess
import tempfile
import unittest

from gen import Module, fname
from test_guest_jump import BASE, module

ROOT = Path(__file__).resolve().parent.parent


class LargeDispatchTests(unittest.TestCase):
    def test_dense_dispatch_keeps_every_entry_and_fallback(self):
        count = Module.MAX_OPTIMIZED_DISPATCH_INSNS + 1
        # BR X0 enters any instruction; the remaining ADDs fall through to RET.
        # A dense fallback must preserve middle-of-function entries and wrapping
        # arithmetic while avoiding the optimizer's pathological memory growth.
        words = [0xd61f0000] + [0x91000442] * (count - 2) + [0xd65f03c0]
        m = module(words)
        body = '\n'.join(m.emit_function(BASE, m.hi, {BASE}, {BASE: 'all'}, {}))
        harness = r'''
#include <assert.h>
#include "cpu.h"
volatile int g_fnprof;
static int fallbacks;
void fnprof_hit(uint64_t a, uint64_t b, uint64_t d) { (void)a; (void)b; (void)d; }
void rt_dispatch(Cpu *c) { (void)c; fallbacks++; }
BODY
int main(void) {
    const unsigned targets[] = {1, 257, COUNT-2, COUNT-1};
    const uint64_t initial[] = {0, UINT64_MAX-7};
    for (unsigned j=0; j<2; j++) for (unsigned i=0; i<4; i++) {
        Cpu c={0}; c.x[0]=BASE+4*targets[i]; c.x[1]=123; c.x[2]=initial[j]; c.x[3]=456;
        FUNCTION(&c);
        assert(c.x[2] == initial[j] + COUNT-1-targets[i]);
        assert(c.x[1]==123 && c.x[3]==456 && !fallbacks);
    }
    Cpu c={0}; c.x[0]=BASE-4; FUNCTION(&c); assert(fallbacks==1 && c.pc==BASE-4);
    c.x[0]=BASE+5; FUNCTION(&c); assert(fallbacks==2 && c.pc==BASE+5);
    return 0;
}
'''.replace('BODY', body).replace('COUNT', str(count)).replace('BASE', hex(BASE)+'ull').replace('FUNCTION', fname(BASE))
        with tempfile.TemporaryDirectory(prefix='large-dispatch-') as temporary:
            folder = Path(temporary); source = folder/'graph.c'
            executable = folder/('graph.exe' if os.name == 'nt' else 'graph')
            source.write_text(harness)
            subprocess.run(shlex.split(os.environ.get('CC', 'clang')) +
                           ['-std=gnu11', '-O1', '-Wall', '-Wextra', '-Werror',
                            '-Wno-unused-label', '-I'+str(ROOT/'runtime'), str(source), '-o', str(executable)],
                           check=True, timeout=60)
            subprocess.run([str(executable)], check=True, timeout=10)


if __name__ == '__main__':
    unittest.main()
