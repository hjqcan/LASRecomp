"""Native local-co-op acceptance cases, using research_run's isolated save/capture runner."""
import argparse
import json
from pathlib import Path
import re
import research_run


def main():
    parser = argparse.ArgumentParser(description=__doc__, add_help=False)
    parser.add_argument('--case', required=True, choices=('move', 'attack', 'idle-enemy', 'keyboard', 'transition', 'off'))
    args, rest = parser.parse_known_args()
    case = args.case
    env = {} if case == 'off' else {'LARECOMP_COOP': '1', 'LARECOMP_COOP_TRACE': '1'}
    updates = 1200 if case in ('attack', 'idle-enemy') else 4400 if case == 'transition' else 2400
    if case == 'move':
        env['LARECOMP_COOP_INPUT'] = '120:RIGHT:90,300:DOWN:90,480:LEFT:90,660:UP:90'
    elif case in ('attack', 'idle-enemy'):
        env['LARECOMP_COOP_TEST_ENEMY'] = '1'
        env['LARECOMP_COOP_INPUT'] = '120:ATTACK:800' if case == 'attack' else '0:WAIT:100000'
    elif case == 'keyboard':
        env['LARECOMP_HOST_INPUT'] = ','.join(f'{frame}:sdlkey:Keypad {key}:80' for frame,key in
                                           ((2200,6),(2400,5),(2600,4),(2800,8)))
    elif case == 'transition':
        env['LARECOMP_COOP_INPUT'] = ','.join(f'{100+i*400}:{key}:60' for i,key in
                                           enumerate(('RIGHT','LEFT','UP','DOWN','RIGHT','LEFT','UP','DOWN','RIGHT')))
    flags = ['--boundary', 'update', '--updates', str(updates), '--frames', '8000' if case == 'transition' else '6000',
             '--scenario', 'house-exit' if case == 'transition' else 'house', *rest]
    # The shared runner rejects existing output folders and hashes the original save before/after.
    research_run.main(extra_env=env, argv=flags)
    output = Path(flags[flags.index('--output')+1]).resolve()
    analyze(case,output)


def analyze(case,output):
    output=Path(output)
    report = json.loads((output/'report.json').read_text())
    log = (output/'runtime.log').read_text(encoding='utf-8', errors='replace')
    rows = [tuple(map(float, values)) for values in re.findall(
        r'\[coop-control\] step (\d+) dir ([-.\d]+) ([-.\d]+) state \d+ pos ([-.\d]+) ([-.\d]+) distance ([-.\d]+)', log)]
    travel = {}
    for name,axis,sign in (('right',0,1),('down',1,1),('left',0,-1),('up',1,-1)):
        travel[name] = sum(max(0,(b[3+axis]-a[3+axis])*sign) for a,b in zip(rows,rows[1:])
                           if a[1+axis]*sign > .9 and b[1+axis]*sign > .9)
    attacks = len(re.findall(r'\[coop-attack\]', log))
    defeated = bool(re.search(r'\[coop-test\] enemy defeated', log))
    enemies = re.findall(r'\[coop-test-enemy\] flags (0x[0-9a-f]+)', log)
    followers = [actor for actor in re.findall(r'\[coop\] follower ([0-9a-f]+)', log) if actor != '0']
    case_passed = {
        'move': all(value > .2 for value in travel.values()),
        'keyboard': all(value > .2 for value in travel.values()),
        'attack': attacks > 0 and defeated,
        'idle-enemy': attacks == 0 and not defeated and len(enemies) >= 5 and not (int(enemies[-1],16)&0x40000000),
        'transition': len(followers) >= 2 and report.get('scenario_transition',{}).get('house_to_field_observed',False),
        'off': '[coop]' not in log and report.get('scenario_movement',{}).get('all_windows_moved',False),
    }[case]
    result = {'case': case, 'passed': report['passed'] and case_passed,
              'source_unchanged': report['source_unchanged'], 'native_attacks': attacks, 'enemy_defeated': defeated,
              'enemy_alive_samples': len(enemies), 'ownership_epochs': len(followers), 'distinct_followers': len(set(followers)),
              'direction_travel': travel, 'control_samples': len(rows),
              'max_observed_distance': max((r[5] for r in rows), default=None),
              'limits': 'Selected native scenarios only; physical gamepads and full-game co-op are not certified.'}
    (output/'coop-report.json').write_text(json.dumps(result, indent=2)+'\n')
    print(json.dumps(result, indent=2))
    if not result['passed']:
        raise SystemExit(1)


if __name__ == '__main__':
    main()
