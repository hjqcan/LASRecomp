"""Compare bounded research observations; report divergence rather than calling a replay deterministic."""
import argparse
import csv
import json
import math
from pathlib import Path


def matrix_product(a, b):
    return [sum(a[r * 4 + k] * b[k * 4 + c] for k in range(4)) for r in range(4) for c in range(4)]


def matrix_error(a, b):
    return max(abs(x - y) for x, y in zip(a, b)) / max(1, max(abs(x) for x in b))


def camera_report(rows):
    errors, reverse, orthogonality, determinants = [], [], [], []
    for row in rows:
        v, p, vp = ([float(row[f'{prefix}{i}']) for i in range(16)] for prefix in ('v', 'p', 'vp'))
        if not all(math.isfinite(x) for values in (v, p, vp) for x in values):
            raise ValueError('Non-finite camera matrix')
        errors.append(matrix_error(matrix_product(p, v), vp))
        reverse.append(matrix_error(matrix_product(v, p), vp))
        orthogonality.append(max(abs(sum(v[r * 4 + k] * v[c * 4 + k] for k in range(3)) - (r == c)) for r in range(3) for c in range(3)))
        determinants.append(v[0] * (v[5] * v[10] - v[6] * v[9]) - v[1] * (v[4] * v[10] - v[6] * v[8]) + v[2] * (v[4] * v[9] - v[5] * v[8]))
    return {'samples': len(rows), 'p_times_v_max_relative_error': max(errors, default=None),
            'v_times_p_max_relative_error': max(reverse, default=None),
            'p_times_v_consistent': bool(errors) and max(errors) < 1e-4,
            'view_rotation_max_orthogonality_error': max(orthogonality, default=None),
            'view_rotation_determinant_range': [min(determinants), max(determinants)] if determinants else None,
            'note': 'First recognized scene UBO per presented frame; excludes culling, shadow and inverse-camera contracts.'}


def load_csv(path, limit):
    if path.stat().st_size > 32 * 1024 * 1024:
        raise ValueError('Research CSV exceeds 32 MiB')
    with path.open(newline='', encoding='utf-8') as stream:
        rows = []
        for row in csv.DictReader(stream):
            if len(rows) >= limit or None in row or any(v is None for v in row.values()):
                raise ValueError('Invalid or oversized research CSV')
            rows.append(row)
        return rows


def folder(path):
    if (path / 'observations.csv').is_file():
        return path
    found = list(path.glob('workshop/runs/*/research'))
    if len(found) != 1:
        raise ValueError('Expected exactly one research capture')
    return found[0]


def observations(path):
    rows = load_csv(path / 'observations.csv', 120000)
    for i, r in enumerate(rows):
        if int(r['poll']) != i or any(r[k] not in ('0', '1') for k in ('observed', 'playing', 'position')):
            raise ValueError('Invalid observation sequence or flags')
        for key in ('x', 'y', 'z'):
            if not math.isfinite(float(r[key])):
                raise ValueError('Non-finite position')
    return rows


def state_key(r):
    return (r['observed'], r['playing'], r['level'], r['hp'], r['position'], r['save_digest'],
            *(round(float(r[k]), 4) if r['position'] == '1' else None for k in ('x', 'y', 'z')))


def divergence(a, b):
    compared = min(len(a), len(b))
    first = next((i for i in range(compared) if state_key(a[i]) != state_key(b[i])), None)
    return {'compared_polls': compared, 'left_polls': len(a), 'right_polls': len(b),
            'equal_prefix_polls': compared if first is None else first,
            'all_observations_match': first is None and len(a) == len(b) and compared > 0,
            'first_difference': None if first is None else {
                'comparison_index': first,
                'left': {k: a[first][k] for k in ('poll', 'frame', 'level', 'hp', 'position', 'x', 'y', 'z', 'save_digest')},
                'right': {k: b[first][k] for k in ('poll', 'frame', 'level', 'hp', 'position', 'x', 'y', 'z', 'save_digest')}}}


def pose_comparison(a, b):
    count, maximum, first = 0, 0.0, None
    for i, (x, y) in enumerate(zip(a, b)):
        if x['position'] != '1' or y['position'] != '1' or x['level'] != y['level']:
            continue
        count += 1
        distance = math.sqrt(sum((float(x[k]) - float(y[k])) ** 2 for k in ('x', 'y', 'z')))
        maximum = max(maximum, distance)
        if first is None and distance > 1e-4:
            first = {'comparison_index': i, 'left_poll': int(x['poll']), 'right_poll': int(y['poll']), 'distance': distance}
    return {'available_pairs': count, 'max_position_difference': maximum, 'first_position_difference': first,
            'note': 'Only compares available positions in the same level; the raw save-region digest may include non-gameplay fields.'}


def timing(rows):
    play = [r for r in rows if r['playing'] == '1']
    result = {'gameplay_polls': len(play)}
    if len(play) < 2:
        return result
    first, last = play[0], play[-1]
    seconds = (int(last['elapsed_ns']) - int(first['elapsed_ns'])) / 1e9
    result.update(seconds=seconds, presented_frames=int(last['frame']) - int(first['frame']),
                  timer_calls=int(last['timer_calls']) - int(first['timer_calls']),
                  vsyncs=int(last['vsyncs']) - int(first['vsyncs']),
                  original_intervals=sorted({int(r['interval']) for r in play}))
    if seconds > 0:
        result['presentation_fps'] = result['presented_frames'] / seconds
        result['observed_vsync_hz'] = result['vsyncs'] / seconds
    positions = [r for r in play if r['position'] == '1']
    result['position_samples'] = len(positions)
    result['xz_path_length'] = sum(math.hypot(float(b['x']) - float(a['x']), float(b['z']) - float(a['z']))
                                   for a, b in zip(positions, positions[1:]) if a['level'] == b['level'])
    return result


def inputs(path):
    tape = path / 'inputs.lastape'
    if tape.stat().st_size > 16 * 1024 * 1024:
        raise ValueError('Input tape exceeds 16 MiB')
    with tape.open(encoding='utf-8') as f:
        if f.readline().strip() != 'LASINPUT1':
            raise ValueError('Unknown input tape')
        identity = f.readline().strip()
        rows = []
        for row in csv.DictReader(f):
            if len(rows) >= 120000:
                raise ValueError('Input tape exceeds limit')
            rows.append(row)
    return identity, [tuple(int(r[k]) for k in ('buttons', 'lx', 'ly', 'rx', 'ry')) for r in rows]


def analyze(left, right=None):
    a_folder = folder(left)
    a = observations(a_folder)
    result = {'left': timing(a), 'left_camera': camera_report(load_csv(a_folder / 'camera.csv', 12000)),
              'scope': 'HID-poll observations and selected serialized save fields; not full simulation state'}
    if right is not None:
        b_folder = folder(right)
        b = observations(b_folder)
        a_id, a_inputs = inputs(a_folder)
        b_id, b_inputs = inputs(b_folder)
        count = min(len(a_inputs), len(b_inputs))
        result['input_prefix'] = {'same_build_id': a_id == b_id, 'compared': count,
                                  'matches': a_inputs[:count] == b_inputs[:count]}
        result['right'] = timing(b)
        result['right_camera'] = camera_report(load_csv(b_folder / 'camera.csv', 12000))
        result['poll_alignment'] = divergence(a, b)
        a_play = next((i for i, r in enumerate(a) if r['playing'] == '1'), len(a))
        b_play = next((i for i, r in enumerate(b) if r['playing'] == '1'), len(b))
        result['gameplay_start_alignment'] = divergence(a[a_play:], b[b_play:])
        result['gameplay_start_positions'] = pose_comparison(a[a_play:], b[b_play:])
        result['deterministic_replay'] = 'not_established' # Even matching this subset cannot certify the whole simulation.
    return result


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('left', type=Path)
    parser.add_argument('right', type=Path, nargs='?')
    parser.add_argument('--output', type=Path)
    args = parser.parse_args()
    report = analyze(args.left, args.right)
    encoded = json.dumps(report, indent=2) + '\n'
    if args.output:
        with args.output.open('x', encoding='utf-8') as stream:
            stream.write(encoded)
    print(encoded)
