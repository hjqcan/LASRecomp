"""Generate the complete module plan for a recognized local game installation."""
import argparse
from pathlib import Path
import subprocess
import sys
from game_catalog import inspect_exefs, render_entry


def main():
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--game-dir',type=Path,default=Path('extracted'))
    parser.add_argument('--outdir',type=Path,default=Path('generated'))
    parser.add_argument('--simd',choices=('asm','c'))
    parser.add_argument('--metadata-only',action='store_true')
    args=parser.parse_args()
    plan=inspect_exefs(args.game_dir/'exefs')
    args.outdir.mkdir(parents=True,exist_ok=True)
    (args.outdir/'gamelib_modules.inc').write_text(render_entry(plan),encoding='utf-8',newline='\n')
    (args.outdir/'modules.mk').write_text('GAME_MODULES := '+' '.join(m['name'] for m in plan['modules'])+'\n',encoding='utf-8')
    if args.metadata_only:
        return
    tools=Path(__file__).resolve().parent
    for module in plan['modules']:
        command=[sys.executable,str(tools/'gen.py'),module['name'],str(args.game_dir/'exefs'/module['name']),
                 module['base'],str(args.outdir/module['name']),'--profile',plan['generator_profile'],
                 '--adapter',plan['game_id'],'--release',plan['version'],'--adapter-id',plan['code_identity']]
        if module['hle']:
            command+=['--hle',str(tools.parent/'runtime/hle/hle_symbols.txt')]
        if args.simd:
            command+=['--simd',args.simd]
        subprocess.run(command,check=True)


if __name__=='__main__':
    main()
