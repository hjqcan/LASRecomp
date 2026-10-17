"""Crash/restart and two-process tests of the production installation publisher."""
import os
from pathlib import Path
import shlex
import subprocess
import tempfile
import hashlib
from host_paths import io_path, logical_path

ROOT = Path(__file__).resolve().parents[1]
with tempfile.TemporaryDirectory(prefix='installation-', dir=io_path(tempfile.gettempdir())) as temporary:
    root = logical_path(temporary).resolve()
    executable = root / ('install.exe' if os.name == 'nt' else 'install')
    subprocess.run(shlex.split(os.environ.get('CXX', 'clang++')) + ['-std=c++17', '-DGAME_INSTALL_TESTING',
        '-x', 'c++', str(ROOT / 'runtime/ui/game_install.mm'), str(ROOT / 'runtime/ui/title_content.mm'),
        str(ROOT / 'tools/test_game_install.cpp'), '-o', str(executable)], check=True)
    slot = root / 'slot'; slot.mkdir(); (slot / 'content').write_text('legacy')
    def run(mode, *args, code=0, crash=None):
        env = dict(os.environ)
        if crash: env['LARECOMP_INSTALL_CRASH_AT'] = crash
        result = subprocess.run([str(executable), str(slot), mode, *args], env=env, capture_output=True, text=True)
        assert result.returncode == code, (mode, result.returncode, result.stdout, result.stderr)
        return result.stdout.strip()
    assert Path(run('resolve')) == slot
    # Interrupted initialization must not hide the still-valid legacy installation.
    (slot / '.installations').mkdir()
    (slot / '.installations/owner.writing').write_bytes(b'LASINSTALL2\n')
    assert Path(run('resolve')) == slot
    first = Path(run('stage', 'first'))
    run('pending'); run('publish', code=1)
    assert Path(run('resolve')) == slot and (slot / 'content').read_text() == 'legacy'
    assert Path(run('complete')) == first
    run('publish'); assert Path(run('resolve')) == first
    run('pending', code=1)
    with subprocess.Popen([str(executable), str(slot), 'watch'], stdout=subprocess.PIPE,
                          stderr=subprocess.PIPE, text=True) as reader:
        assert reader.stdout.readline().strip() == 'watching'
        watched = Path(run('stage', 'readers')); run('complete'); run('publish')
        (slot / 'watch-stop').write_bytes(b'')
        reader.communicate(timeout=5); assert reader.returncode == 0
    first = watched
    second = Path(run('stage', 'second')); run('complete')
    run('publish', code=86, crash='receipt')
    assert Path(run('resolve')) == first and (first / 'content').read_text() == 'readers'
    assert (second / 'installation-receipt').exists()
    run('publish'); assert Path(run('resolve')) == second
    third = Path(run('stage', 'third')); run('complete')
    run('publish', code=86, crash='published')
    assert Path(run('resolve')) == third
    run('pending', code=1)  # crash after the atomic pointer switch is already committed
    with subprocess.Popen([str(executable), str(slot), 'lock'], stdin=subprocess.PIPE,
                          stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True) as locked:
        assert locked.stdout.readline().strip() == 'locked'
        run('stage', 'conflicting', code=2)
        run('clear', code=1)
        assert Path(run('resolve')) == third
        locked.communicate('\n', timeout=5)
        assert locked.returncode == 0
    meta = slot / '.installations'
    current = (meta / 'current').read_bytes()
    (meta / 'current').write_text('LASINSTALL2\n../outside\n')
    run('resolve', code=1)
    (meta / 'current').write_bytes(current)
    outside = root / 'outside'; outside.mkdir(); (outside / 'keep').write_text('untouched')
    unsafe = meta / 'build-ffff'
    try:
        unsafe.symlink_to(outside, target_is_directory=True)
    except OSError:
        pass
    else:
        run('clear', code=1)
        assert (outside / 'keep').read_text() == 'untouched'
        unsafe.unlink()
    # Cloning an active installation keeps empty directories and independent source bytes.
    third = Path(run('stage', 'long-path')); run('complete')
    (third / 'exefs').mkdir(); (third / 'exefs/main').write_text('original module')
    (third / 'romfs/Empty').mkdir(parents=True)
    # Candidate depth pushes ordinary game asset paths beyond MAX_PATH, even
    # when the installation slot itself is a normal short path.
    deep = Path('romfs/Empty') / ('directory-' + 'x'*80) / ('asset-' + 'y'*100 + '.bin')
    assert len(str(third / deep)) > 260
    io_path(third / deep).parent.mkdir(parents=True)
    io_path(third / deep).write_bytes(b'long asset')
    run('publish') # hashing the full candidate must include the long asset
    tree = hashlib.sha256()
    native = io_path(third)
    for name in sorted(p.relative_to(native).as_posix() for p in native.rglob('*')
                       if p.is_file() and p.name != 'installation-receipt'):
        tree.update(name.encode('utf-8') + b'\0')
        tree.update(hashlib.sha256((native / name).read_bytes()).hexdigest().encode('ascii'))
    assert (third / 'installation-receipt').read_text().splitlines()[2] == tree.hexdigest()
    run('content')
    clone = Path(run('clone'))
    assert (clone / 'romfs/Empty').is_dir()
    (clone / 'exefs/main').write_text('changed candidate')
    assert (third / 'exefs/main').read_text() == 'original module'
    assert io_path(clone / deep).read_bytes() == b'long asset'
    run('clear'); assert Path(run('resolve')) == slot
    assert (slot / 'content').read_text() == 'legacy' and (outside / 'keep').read_text() == 'untouched'
    # A first install has no launchable generation until publication, and remains resumable.
    slot = root / 'first-install'
    pending = Path(run('stage', 'initial')); run('publish', code=1)
    assert Path(run('resolve')) == slot
    assert Path(run('complete')) == pending
    run('publish'); assert Path(run('resolve')) == pending
    print('PASS: candidate isolation, failure preservation, two crash boundaries, restart/resume, process locking, clone isolation and safe cleanup')
