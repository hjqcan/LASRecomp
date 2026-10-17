"""An empty queue fence must never name an unsubmitted ring generation."""
import os
from pathlib import Path
import shlex
import subprocess
import tempfile

root = Path(__file__).resolve().parents[1]
source = (root / 'runtime/nvn/vulkan/exec.c').read_text()
function = source[source.index('uint64_t renderer_fence_seq(void) {'):source.index('\nvoid renderer_wait_seq(')]
harness = r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <pthread.h>
#include <stdio.h>
static pthread_mutex_t seq_lock=PTHREAD_MUTEX_INITIALIZER;
typedef struct { uint64_t seq; bool main_begun,upload_begun; } Sub;
static Sub subs[8];
static unsigned cur_sub;
static uint64_t submit_seq;
''' + function + r'''
int main(void) {
    assert(renderer_fence_seq()==0);
    for (unsigned generation=1;generation<=33;generation++) {
        // Returning to a ring slot leaves the completed generation's begun
        // flags set. sub() clears them only when the next recording starts.
        assert(renderer_fence_seq()==submit_seq);
        subs[cur_sub].seq=generation;
        subs[cur_sub].main_begun=false; subs[cur_sub].upload_begun=true;
        assert(renderer_fence_seq()==generation);
        subs[cur_sub].upload_begun=false; subs[cur_sub].main_begun=true;
        assert(renderer_fence_seq()==generation);
        submit_seq=generation; cur_sub=(cur_sub+1)%8;
        assert(renderer_fence_seq()==submit_seq); // empty QueueFenceSync + submit
    }
    // A prepared but empty slot also has no pending submission.
    subs[cur_sub].seq=submit_seq+1;
    subs[cur_sub].main_begun=subs[cur_sub].upload_begun=false;
    assert(renderer_fence_seq()==submit_seq);
    puts("PASS: initial, pending upload/main, empty fences and repeated ring wraps");
}
'''
with tempfile.TemporaryDirectory(prefix='fence-sequence-') as temporary:
    temporary = Path(temporary)
    cfile=temporary/'fence.c'; cfile.write_text(harness)
    exe=temporary/('fence.exe' if os.name=='nt' else 'fence')
    subprocess.run(shlex.split(os.environ.get('CC','clang'))+['-std=gnu11','-pthread',str(cfile),'-o',str(exe)],check=True)
    subprocess.run([str(exe)],check=True,timeout=5)
