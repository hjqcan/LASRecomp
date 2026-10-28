"""Menu sounds stay with the selected LAS installation; no audio device or game assets needed."""
import os
from pathlib import Path
import shlex
import shutil
import subprocess
import sys
import tempfile

ROOT = Path(__file__).resolve().parents[1]
HARNESS = r'''
#include <assert.h>
#include "menu_sfx.c"
Settings g_settings;
const char *g_romfs_dir = "missing-old-game/romfs";
static bool las;
bool title_is_las(void) { return las; }
int main(int argc, char **argv) {
    assert(argc == 2);
    menu_sfx_set_game_dir(argv[1]);
    menu_sfx_load_now();
    assert(g_state == 0 && g_retry_ns == 0);  // other titles never try Richard.bfsar
    las = true;
    menu_sfx_load_now();
    assert(g_state == 2);  // selected directory's invalid archive, not the missing old g_romfs_dir
    g_state = 1;
    g_sfx[0] = (Sfx){malloc(8 * sizeof(float)), 4};
    assert(g_sfx[0].pcm);
    float *pcm = g_sfx[0].pcm;
    g_voice[0] = (Voice){0, 0, 0}; g_nvoice = 1;
    menu_sfx_set_game_dir(argv[1]);
    assert(g_state == 1 && g_sfx[0].pcm == pcm && g_nvoice == 1);  // same installation keeps its sound
    menu_sfx_set_game_dir("other-game");
    assert(g_state == 0 && !g_retry_ns && !g_nvoice && !g_sfx[0].pcm && !g_sfx[0].frames);
    las = false;
    g_state = 1;  // even a previously loaded LAS cache is silent on another title
    menu_sfx_play(MENU_SFX_CURSOR);
    assert(!g_stream && !g_nvoice);
    puts("PASS: menu sounds respect title, selected directory, cache reuse and installation changes");
}
'''
deps = os.environ.get('DEPS')
flags = ['-lSDL3']
if deps:
    flags = [f'-I{deps}/include', f'-L{deps}/lib', f'-Wl,-rpath,{deps}/lib', '-lSDL3']
elif shutil.which('pkg-config'):
    flags = shlex.split(subprocess.check_output(['pkg-config', '--cflags', '--libs', 'sdl3'], text=True))
elif sys.platform == 'darwin':
    prefix = subprocess.check_output(['brew', '--prefix'], text=True).strip()
    flags = [f'-I{prefix}/include', f'-L{prefix}/lib', '-lSDL3']
with tempfile.TemporaryDirectory(prefix='menu-sfx-') as scratch:
    temp = Path(scratch)
    source = temp / 'test.c'
    source.write_text(HARNESS)
    exe = temp / ('test.exe' if os.name == 'nt' else 'test')
    subprocess.run(shlex.split(os.environ.get('CC', 'clang')) +
                   ['-std=gnu11', '-pthread', '-I' + str(ROOT / 'runtime'), str(source), '-o', str(exe), *flags, '-lm'],
                   check=True)
    game = temp / 'selected game'
    archive = game / 'romfs/region_common/audio/Richard.bfsar'
    archive.parent.mkdir(parents=True)
    archive.write_bytes(b'not an audio archive')
    subprocess.run([str(exe), str(game)], check=True, timeout=10)
