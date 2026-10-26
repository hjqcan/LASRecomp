"""Moving the data folder (runtime/paths.c): copy, check, switch, remove the old copy - and a move cut off at any step
never loses data: the next start finds all of it in the old or the new folder, and tidies up the other.
Builds tools/test_data_move.c with the real paths.c and this platform's file; scratch folders only.
LARECOMP_TEST_OTHER_FS=<folder on another drive>: also moves there (a real copy, not hard links)."""
import hashlib
import os
from pathlib import Path
import shlex
import shutil
import subprocess
import sys
import tempfile

ROOT = Path(__file__).resolve().parent.parent
plat = 'windows' if os.name == 'nt' else 'macos' if sys.platform == 'darwin' else 'linux'
flags = []
deps = os.environ.get('DEPS')  # (a prefix with SDL3, as for make DEPS=)
if deps:
    flags = [f'-I{deps}/include', f'-L{deps}/lib', f'-Wl,-rpath,{deps}/lib', '-lSDL3']
elif shutil.which('pkg-config') and subprocess.run(['pkg-config', '--exists', 'sdl3']).returncode == 0:
    flags = shlex.split(subprocess.run(['pkg-config', '--cflags', '--libs', 'sdl3'], capture_output=True, text=True,
                                       check=True).stdout)
elif sys.platform == 'darwin':
    brew = subprocess.run(['brew', '--prefix'], capture_output=True, text=True).stdout.strip() or '/opt/homebrew'
    flags = [f'-I{brew}/include', f'-L{brew}/lib', '-lSDL3']
extra = {'macos': ['-framework', 'CoreFoundation'], 'linux': ['-pthread', '-ldl'],
         'windows': ['-D_FILE_OFFSET_BITS=64', '-D_POSIX_THREAD_SAFE_FUNCTIONS', '-D_CRT_RAND_S',
                     '-DWIN32_LEAN_AND_MEAN', '-DNOMINMAX', '-lwinmm', '-lole32', '-lshell32', '-lcomdlg32', '-luuid', '-ladvapi32']}[plat]
NOTES = ('.lasrecomp_move_to', '.lasrecomp_move_from', '.lasrecomp_incomplete')
ENTRIES = ('save', 'rando', 'larecomp.ini', 'fonts', 'cache', 'extracted')


def populate(d: Path):
    rnd = __import__('random').Random(295)
    def put(rel, n):
        p = d / rel
        p.parent.mkdir(parents=True, exist_ok=True)
        p.write_bytes(bytes(rnd.getrandbits(8) for _ in range(n)) if n < 4096 else os.urandom(n))
    put('save/slot0/zelda.sav', 300_000)
    put('rando/My Seed/settings.json', 2_000)
    put('rando/My Seed/save/slot0/zelda.sav', 300_000)
    put('larecomp.ini', 1_500)
    put('fonts/nintendo_udsg-r_std_003.bfttf', 50_000)
    put('cache/shaders.bin', 3_000_000)
    for i in range(24):
        put(f'extracted/romfs/Data/file{i:02}.bin', 400_000)
    put('extracted/exefs/main', 1_000_000)
    put('extracted/.gamebuild/obj/a.o', 200_000)
    if os.name != 'nt':
        os.symlink('romfs/Data', d / 'extracted/data_link')


def snapshot(d: Path):
    out = {}
    for e in ENTRIES:
        top = d / e
        if not (top.exists() or top.is_symlink()):
            continue
        items = [top] if not top.is_dir() or top.is_symlink() else [top] + sorted(top.rglob('*'))
        for p in items:
            rel = p.relative_to(d).as_posix()
            if p.name.startswith('._'):
                continue  # (macOS metadata on drives without extended attributes, e.g. exFAT)
            if p.is_symlink():
                out[rel] = 'link:' + os.readlink(p)
            elif p.is_file():
                out[rel] = hashlib.sha256(p.read_bytes()).hexdigest()
            elif p.is_dir():
                out[rel] = 'dir'
    return out


def ours_left(d: Path):
    return sorted(e for e in ENTRIES + NOTES if (d / e).exists() or (d / e).is_symlink())


with tempfile.TemporaryDirectory() as directory:
    S = Path(directory).resolve()
    exe = S / ('data-move-test.exe' if os.name == 'nt' else 'data-move-test')
    subprocess.run(shlex.split(os.environ.get('CC', 'clang')) +
                   ['-std=gnu11', '-Iruntime', 'runtime/paths.c', f'runtime/platform/{plat}.c',
                    'tools/test_data_move.c', '-o', str(exe)] + flags + extra, cwd=ROOT, check=True)
    start, home = S / 'start', S / 'home'
    start.mkdir()
    home.mkdir()
    populate(home)
    want = snapshot(home)
    env = dict(os.environ, LARECOMP_DATA_HOME=str(home))
    env.pop('LARECOMP_DATA_ROOT', None)

    def run(*args, stall=False, extra_env=None):
        p = subprocess.Popen([str(exe), *map(str, args)], cwd=start, env=dict(env, **(extra_env or {})),
                             stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True)
        if stall:
            line = p.stdout.readline()
            assert line.strip() == 'STALL', f'{args}: no stall ({line!r})'
            p.kill()  # (cut off partway, like a crash or a pulled plug)
            p.wait()
            p.stdout.close(), p.stderr.close()
            return {}
        out, err = p.communicate(timeout=300)
        assert p.returncode == 0, f'{args}: exit {p.returncode}\n{err}'
        return dict((l.split(' ', 1) + [''])[:2] for l in out.splitlines())

    def root():
        return Path(run('root')['ROOT'])

    def check_in(where: Path, why):
        assert root() == where.resolve(), f'{why}: data root {root()} (want {where})'
        got = snapshot(where)
        assert got == want, f'{why}: data in {where} differs: ' + \
            str(sorted(set(got.items()) ^ set(want.items()))[:6])

    assert root() == home
    # the offer for a picked folder
    (S / 'misc').mkdir()
    (S / 'misc/notes.txt').write_text('mine')
    (S / 'empty').mkdir()
    t = run('target', home)
    assert t['REFUSE'] == "That's the data folder already.", t
    t = run('target', home / 'save')
    assert t['REFUSE'] == 'That folder is inside the data folder.', t
    t = run('target', S / 'misc')
    assert Path(t['PATH']) == S / 'misc' / 'LAS Recomp' and not t['REFUSE'] and t['HAS_DATA'] == '0', t
    t = run('target', S / 'empty')
    assert Path(t['PATH']) == S / 'empty' and not t['REFUSE'] and int(t['BYTES']) > 10_000_000, t
    print('target: the folder itself when empty, a "LAS Recomp" folder in a full one; the data folder and folders '
          'in it refused')

    for links in (True, False):  # (same drive: hard links; LARECOMP_MOVE_NO_LINKS: copied as to another drive)
        if not links:
            env['LARECOMP_MOVE_NO_LINKS'] = '1'
        how = 'hard links' if links else 'copies'
        dst = S / ('dst_links' if links else 'dst_copy')
        # cut off while copying, then while checking: the old folder stays in use, the unfinished copy goes
        for step in (0, 1):
            run('move', dst, step, stall=True)
            check_in(home, f'{how}: move cut off at step {step}')
            assert ours_left(dst) == [] and ours_left(home) == sorted(ENTRIES), (ours_left(dst), ours_left(home))
        # cut off while removing the old copy: the new folder is in use, and the next start finishes the removal
        run('move', dst, 2, stall=True)
        check_in(dst, f'{how}: move cut off while removing the old copy')
        assert ours_left(home) == [] and ours_left(dst) == sorted(ENTRIES), (ours_left(home), ours_left(dst))
        assert (home / 'data_root.txt').exists()
        # and back, uninterrupted: no pointer left
        r = run('move', home)
        assert r['MOVE'].startswith('ok'), r
        check_in(home, f'{how}: moved back')
        assert ours_left(dst) == [] and not (home / 'data_root.txt').exists()
        print(f'move ({how}): cut off while copying, checking or removing - the data is whole in one folder')
    env.pop('LARECOMP_MOVE_NO_LINKS')

    other = os.environ.get('LARECOMP_TEST_OTHER_FS')
    if other:
        dst = Path(other) / f'lasrecomp-move-test-{os.getpid()}'
        run('move', dst, 0, stall=True)
        check_in(home, 'other drive: cut off while copying')
        assert ours_left(dst) == []
        r = run('move', dst)
        assert r['MOVE'].startswith('ok'), r
        check_in(dst, 'other drive: moved')
        r = run('move', home)
        assert r['MOVE'].startswith('ok'), r
        check_in(home, 'other drive: moved back')
        shutil.rmtree(dst, ignore_errors=True)
        print(f'move to another drive ({other}): copied, checked, back')

    # start fresh elsewhere: the old data is kept; switching back uses it as it is
    fresh = S / 'fresh'
    r = run('use', fresh)
    assert r['USE'].startswith('ok'), r
    assert root() == fresh and ours_left(fresh) == [] and snapshot(home) == want
    t = run('target', home)
    assert t['HAS_DATA'] == '1' and not t['REFUSE'], t
    r = run('move', home)
    assert r['MOVE'].startswith('failed') and 'already has' in r['MOVE'], r  # (never merged into other data)
    assert root() == fresh and snapshot(home) == want
    r = run('use', home)
    assert r['USE'].startswith('ok'), r
    check_in(home, 'switched back')
    print('start fresh: the old data stays; back to it as it is; never moved into a folder with data')
    print(f'{plat}: data folder moves never lose data')
