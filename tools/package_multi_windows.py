"""Package a Windows Multi build from explicit distributable inputs only.

The output directory must not exist. Game installations and previous full app
directories are never copied wholesale. DLLs may come from an existing release,
but FFmpeg must be its LGPL build, as with the upstream `make dist` target.
"""
import argparse
import ctypes
import hashlib
import json
from pathlib import Path
import re
import shutil
import subprocess
import zipfile


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--runtime', type=Path, required=True, help='Directory with lasrecomp.exe and its import library')
    parser.add_argument('--helpers', type=Path, required=True, help='Fresh lasgen/ and rando-gen/ build output')
    parser.add_argument('--zig', type=Path, required=True, help='Prepared slim Zig distribution with rt/')
    parser.add_argument('--dlls', type=Path, required=True, help='Directory containing the required redistributable DLLs')
    parser.add_argument('--output', type=Path, required=True, help='New staging/output directory')
    args = parser.parse_args()
    root = Path(__file__).resolve().parents[1]
    version_line = next(line for line in (root/'runtime/ui/launcher.mm').read_text(encoding='utf-8').splitlines()
                        if line.startswith('#define LARECOMP_VERSION '))
    version = version_line.split('"')[1]
    if not re.fullmatch(r'multi-beta[1-9][0-9]*\.[1-9][0-9]*', version):
        raise RuntimeError('Expected a Multi release tag (multi-beta<N>.<revision>)')
    commit = subprocess.check_output(['git', 'rev-parse', 'HEAD'], cwd=root, text=True).strip()
    source_dirty = bool(subprocess.check_output(['git', 'status', '--porcelain'], cwd=root, text=True).strip())
    if source_dirty:
        raise RuntimeError('Commit reviewed source changes before packaging a release')
    # Check the actual redistributed binary, not the build machine's import libs.
    import os
    with os.add_dll_directory(str(args.dlls.resolve())):
        codec = ctypes.CDLL(str((args.dlls/'avcodec-63.dll').resolve()))
        codec.avcodec_license.restype = ctypes.c_char_p
        codec.avcodec_configuration.restype = ctypes.c_char_p
        license_text = codec.avcodec_license().decode()
        configuration = codec.avcodec_configuration().decode()
    if not license_text.startswith('LGPL') or '--enable-gpl' in configuration or '--enable-nonfree' in configuration:
        raise RuntimeError('This package requires the LGPL FFmpeg build')
    args.output.mkdir(parents=True, exist_ok=False)
    stage = args.output/'Multi'
    stage.mkdir()

    def copy(src, relative):
        dest = stage/relative
        dest.parent.mkdir(parents=True, exist_ok=True)
        if src.is_dir():
            shutil.copytree(src, dest)
        else:
            shutil.copy2(src, dest)

    for name in ('lasrecomp.exe', 'lasrecomp.dll.a'):
        copy(args.runtime/name, name)
    for name in ('lasgen', 'rando-gen'):
        copy(args.helpers/name, name)
    copy(args.zig, 'zig')
    for name in ('SDL3.dll', 'libopus-0.dll', 'avcodec-63.dll', 'avformat-63.dll', 'avutil-61.dll', 'swscale-10.dll',
                 'libiconv-2.dll', 'libshaderc_shared.dll', 'libgcc_s_seh-1.dll', 'libstdc++-6.dll', 'libwinpthread-1.dll'):
        copy(args.dlls/name, name)
    for name in ('cpu.h', 'simd.h', 'simd_fp.h', 'gamelib.h', 'guest_jump.h'):
        copy(root/'runtime'/name, 'gamelib-src/'+name)
    copy(root/'runtime/hle/hle_symbols.txt', 'gamelib-src/hle_symbols.txt')
    copy(root/'tools/gamelib_entry.c', 'gamelib-src/gamelib_entry.c')
    copy(root/'docs/icon/icon_1024.png', 'icon.png')
    for folder, font, license_name in (('Baloo2', 'Baloo2-SemiBold.ttf', 'OFL.txt'),
                                      ('DejaVu', 'DejaVuSans.ttf', 'LICENSE')):
        copy(root/'third_party/fonts'/folder/font, font)
        copy(root/'third_party/fonts'/folder/license_name, folder+'-LICENSE.txt')
    for name in ('README.md', 'LICENSE', 'CONTRIBUTING.md'):
        copy(root/name, name)
    for name in ('multi.md', 'NSZ-and-v110.md', 'photo-mods-randomizer.md',
                 'photo-mode.md', 'resource-mods.md', 'multi-game.md',
                 'adapter-development.md', 'ACNH-port.md', 'pokemon-za-port.md',
                 'local-coop.md', 'workshop.md', 'research-capture.md'):
        copy(root/'docs'/name, 'docs/'+name)
    copy(root/'docs/multi.md', 'START-HERE-使用说明.md')
    copy(root/'examples/mods/template', 'examples/mods/template')
    copy(root/'examples/workshop/leave-house.laschallenge', 'examples/workshop/leave-house.laschallenge')
    copy(root/'fonts/README.md', 'fonts/README.md')
    copy(root/'third_party/release-licenses', 'licenses')
    copy(root/'third_party/ryujinx/LICENSE.txt', 'licenses/Ryujinx-LICENSE.txt')
    copy(root/'adapters/acnh/speedfix60/LICENSE', 'licenses/ACNH-SpeedFix-LICENSE.txt')
    copy(root/'adapters/acnh/speedfix60/README.md', 'licenses/ACNH-SpeedFix-README.md')
    files = sorted(p for p in stage.rglob('*') if p.is_file())
    banned = {'.gamebuild', 'extracted', 'games', 'save', 'saves', 'seeds', 'photos', 'prod.keys', 'title.keys',
              'lasgame.dll', 'lasrecomp.log', 'larecomp.ini', 'settings.ini', 'romfs.bin', 'exefs', 'romfs'}
    records = []
    for path in files:
        rel = path.relative_to(stage)
        if banned.intersection(part.lower() for part in rel.parts) or path.suffix.lower() in ('.nca', '.nsp', '.nsz', '.xci', '.xcz', '.keys'):
            raise RuntimeError(f'Unexpected private/game file: {rel}')
        with path.open('rb') as stream:
            digest = hashlib.file_digest(stream, 'sha256').hexdigest()
        records.append(dict(path=rel.as_posix(), bytes=path.stat().st_size, sha256=digest))
    manifest = dict(version=version, commit=commit, ffmpeg_license=license_text,
                    source_dirty=source_dirty,
                    ffmpeg_configuration_without_build_prefix=re.sub(r'--prefix=\S+\s*', '', configuration),
                    files=records)
    (stage/'package-manifest.json').write_text(json.dumps(manifest, indent=2)+'\n', encoding='utf-8')
    archive = args.output/f'Multi-{version.removeprefix("multi-")}-windows-x64.zip'
    with zipfile.ZipFile(archive, 'w', zipfile.ZIP_DEFLATED, compresslevel=6) as z:
        for path in sorted(p for p in stage.rglob('*') if p.is_file()):
            z.write(path, path.relative_to(args.output).as_posix())
    with archive.open('rb') as f:
        digest = hashlib.file_digest(f, 'sha256').hexdigest()
    archive.with_suffix('.zip.sha256').write_text(f'{digest}  {archive.name}\n', encoding='ascii')
    print(json.dumps(dict(archive=str(archive), bytes=archive.stat().st_size, sha256=digest, files=len(records)+1)))


if __name__ == '__main__':
    main()
