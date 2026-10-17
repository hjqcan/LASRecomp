"""Compare bounded research observations; report divergence rather than calling a replay deterministic."""
import argparse
import configparser
import csv
import json
import math
import re
from collections import Counter, defaultdict
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


def phase_report(rows):
    """Pair entry/exit observations by guest thread and callback, including nested calls."""
    names = ('update_dispatch', 'controller_update', 'task_callback', 'frame_timer')
    stacks, pairs, incomplete = defaultdict(list), defaultdict(list), 0
    for i, r in enumerate(rows):
        kind = int(r['phase'])
        if int(r['event']) != i or kind not in range(4) or r['before'] not in ('0', '1'):
            raise ValueError('Invalid phase sequence')
        if not all(math.isfinite(float(r[k])) for k in ('delta', 'x', 'y', 'z')):
            raise ValueError('Non-finite phase data')
        key = (kind, r['thread'], r['object'], r['caller'])
        if r['before'] == '1':
            stacks[key].append(r)
        elif stacks[key]:
            pairs[kind].append((stacks[key].pop(), r))
        else:
            incomplete += 1
    result = {'samples': len(rows), 'unpaired_events': incomplete + sum(map(len, stacks.values())), 'callbacks': {}}
    for kind, name in enumerate(names):
        complete = pairs[kind]
        gameplay = [(a, b) for a, b in complete if all(r['observed'] == r['playing'] == '1' for r in (a, b))]
        available = [(a, b) for a, b in gameplay if a['position'] == b['position'] == '1' and a['level'] == b['level']]
        distances = [math.sqrt(sum((float(b[k]) - float(a[k])) ** 2 for k in ('x', 'y', 'z'))) for a, b in available]
        deltas = [float(b['delta']) for a, b in gameplay]
        result['callbacks'][name] = {
            'complete_pairs': len(complete), 'gameplay_pairs': len(gameplay),
            'counter_advances': dict(Counter(str((int(b['counter']) - int(a['counter'])) & 0xffffffff) for a, b in gameplay)),
            'hid_polls_per_call': dict(Counter(str(int(b['poll_count']) - int(a['poll_count'])) for a, b in gameplay)),
            'position_pairs': len(available), 'position_changes': sum(d > 1e-6 for d in distances),
            'max_position_change': max(distances, default=0),
            'observed_delta_range': [min(deltas), max(deltas)] if deltas else None,
        }
    result['note'] = 'Observed callback spans; asynchronous work may complete inside a span without being owned by that callback.'
    return result


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


def observations(path, filename='observations.csv', ordinal='poll'):
    rows = load_csv(path / filename, 120000)
    for i, r in enumerate(rows):
        if int(r[ordinal]) != i or any(r[k] not in ('0', '1') for k in ('observed', 'playing', 'position')):
            raise ValueError('Invalid observation sequence or flags')
        for key in ('x', 'y', 'z'):
            if not math.isfinite(float(r[key])):
                raise ValueError('Non-finite position')
    return rows


def state_key(r):
    return (r['observed'], r['playing'], r['level'], r['hp'], r['position'], r['save_digest'],
            *(round(float(r[k]), 4) if r['position'] == '1' else None for k in ('x', 'y', 'z')))


def divergence(a, b, unit='polls'):
    compared = min(len(a), len(b))
    first = next((i for i in range(compared) if state_key(a[i]) != state_key(b[i])), None)
    return {f'compared_{unit}': compared, f'left_{unit}': len(a), f'right_{unit}': len(b),
            f'equal_prefix_{unit}': compared if first is None else first,
            'all_observations_match': first is None and len(a) == len(b) and compared > 0,
            'first_difference': None if first is None else {
                'comparison_index': first,
                'left': {k: a[first][k] for k in ('poll', 'update', 'frame', 'level', 'hp', 'position', 'x', 'y', 'z', 'save_digest') if k in a[first]},
                'right': {k: b[first][k] for k in ('poll', 'update', 'frame', 'level', 'hp', 'position', 'x', 'y', 'z', 'save_digest') if k in b[first]}}}


def pose_comparison(a, b):
    count, maximum, first = 0, 0.0, None
    for i, (x, y) in enumerate(zip(a, b)):
        if x['position'] != '1' or y['position'] != '1' or x['level'] != y['level']:
            continue
        count += 1
        distance = math.sqrt(sum((float(x[k]) - float(y[k])) ** 2 for k in ('x', 'y', 'z')))
        maximum = max(maximum, distance)
        if first is None and distance > 1e-4:
            key = 'update' if 'update' in x and 'update' in y else 'poll'
            first = {'comparison_index': i, f'left_{key}': int(x[key]), f'right_{key}': int(y[key]), 'distance': distance}
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
                  original_intervals=sorted({int(r['interval']) for r in play if 'interval' in r}))
    if seconds > 0:
        result['presentation_fps'] = result['presented_frames'] / seconds
        result['observed_vsync_hz'] = result['vsyncs'] / seconds
    positions = [r for r in play if r['position'] == '1']
    result['position_samples'] = len(positions)
    result['xz_path_length'] = sum(math.hypot(float(b['x']) - float(a['x']), float(b['z']) - float(a['z']))
                                   for a, b in zip(positions, positions[1:]) if a['level'] == b['level'])
    return result


def update_timing(rows):
    report = timing(rows)
    report['gameplay_updates'] = report.pop('gameplay_polls')
    report['captured_updates'] = len(rows)
    report.pop('original_intervals', None)
    deltas = [float(r['delta']) for r in rows]
    if not all(math.isfinite(x) for x in deltas):
        raise ValueError('Non-finite update delta')
    report['dispatcher_delta_range'] = [min(deltas), max(deltas)] if deltas else None
    report['dispatcher_delta_sum'] = sum(deltas)
    report['counter_advances'] = dict(Counter(str((int(r['counter_after']) - int(r['counter_before'])) & 0xffffffff) for r in rows))
    if len(rows) > 1:
        seconds = (int(rows[-1]['elapsed_ns']) - int(rows[0]['elapsed_ns'])) / 1e9
        if seconds > 0:
            report['observed_update_hz'] = (len(rows) - 1) / seconds
    return report


def start_state(path):
    if not path.exists():
        return None
    if path.stat().st_size > 4096:
        raise ValueError('Oversized start-state metadata')
    parser = configparser.ConfigParser()
    parser.read_string('[state]\n' + path.read_text(encoding='utf-8'))
    result = dict(parser['state'])
    if not all(math.isfinite(float(result[k])) for k in ('x', 'y', 'z')):
        raise ValueError('Invalid start-state position')
    return result


def input_motion_segments(rows, values):
    """Measure observed motion during held left-stick input, without assuming a simulation-time unit."""
    segments, start = [], 0
    count = min(len(rows), len(values))
    while start < count:
        end = start + 1
        while end < count and values[end] == values[start]:
            end += 1
        buttons, lx, ly, rx, ry = values[start]
        if lx or ly:
            distances = []
            for i in range(max(1, start), end):
                a, b = rows[i - 1], rows[i]
                if (a['observed'] == b['observed'] == a['position'] == b['position'] == '1' and a['level'] == b['level']):
                    distances.append(math.hypot(float(b['x']) - float(a['x']), float(b['z']) - float(a['z'])))
            seconds = (int(rows[end - 1]['elapsed_ns']) - int(rows[start - 1]['elapsed_ns'])) / 1e9 if start else None
            path = sum(distances)
            complete = len(distances) == end - start
            segments.append({'start_update': start, 'updates': end - start, 'buttons': buttons,
                             'lx': lx, 'ly': ly, 'rx': rx, 'ry': ry, 'available_pairs': len(distances),
                             'host_seconds': seconds, 'xz_path_length': path,
                             'path_units_per_host_second': path / seconds if complete and seconds and seconds > 0 else None})
        start = end
    return segments


def inputs(path):
    tape = path / 'inputs.lastape'
    if tape.stat().st_size > 16 * 1024 * 1024:
        raise ValueError('Input tape exceeds 16 MiB')
    with tape.open(encoding='utf-8') as f:
        magic = f.readline().strip()
        if magic not in ('LASINPUT1', 'LASINPUT2'):
            raise ValueError('Unknown input tape')
        identity = f.readline().strip()
        if not re.fullmatch(r'build_id=[0-9a-f]{40}', identity):
            raise ValueError('Invalid tape executable identity')
        boundary = f.readline().strip() if magic == 'LASINPUT2' else 'boundary=fullkey_hid_poll'
        if magic == 'LASINPUT2' and boundary != 'boundary=las_update_dispatch_v1':
            raise ValueError('Unknown input boundary')
        ordinal = 'update' if magic == 'LASINPUT2' else 'poll'
        reader = csv.DictReader(f)
        if reader.fieldnames != [ordinal, 'buttons', 'lx', 'ly', 'rx', 'ry']:
            raise ValueError('Invalid tape header')
        rows = []
        for row in reader:
            if len(rows) >= 120000 or None in row or any(v is None for v in row.values()) or int(row[ordinal]) != len(rows):
                raise ValueError('Invalid input tape sequence or size')
            if not 0 <= int(row['buttons']) <= 0xffffff or any(not -32767 <= int(row[k]) <= 32767 for k in ('lx', 'ly', 'rx', 'ry')):
                raise ValueError('Invalid input bounds')
            rows.append(row)
    if not rows:
        raise ValueError('Input tape is empty')
    return identity, boundary, [tuple(int(r[k]) for k in ('buttons', 'lx', 'ly', 'rx', 'ry')) for r in rows]


def analyze(left, right=None):
    a_folder = folder(left)
    a = observations(a_folder)
    result = {'left': timing(a), 'left_camera': camera_report(load_csv(a_folder / 'camera.csv', 12000)),
              'scope': 'HID-poll observations and selected serialized save fields; not full simulation state'}
    if (a_folder / 'phases.csv').exists():
        result['left_phases'] = phase_report(load_csv(a_folder / 'phases.csv', 120000))
    a_updates = None
    if (a_folder / 'updates.csv').exists():
        a_updates = observations(a_folder, 'updates.csv', 'update')
        result['left_updates'] = update_timing(a_updates)
        result['left_start_state'] = start_state(a_folder / 'start-state.ini')
        if (a_folder / 'inputs.lastape').exists():
            result['left_motion_segments'] = input_motion_segments(a_updates, inputs(a_folder)[2])
    if right is not None:
        b_folder = folder(right)
        b = observations(b_folder)
        a_id, a_boundary, a_inputs = inputs(a_folder)
        b_id, b_boundary, b_inputs = inputs(b_folder)
        count = min(len(a_inputs), len(b_inputs))
        result['input_prefix'] = {'same_build_id': a_id == b_id, 'same_boundary': a_boundary == b_boundary,
                                  'compared': count, 'matches': a_boundary == b_boundary and a_inputs[:count] == b_inputs[:count]}
        result['right'] = timing(b)
        result['right_camera'] = camera_report(load_csv(b_folder / 'camera.csv', 12000))
        if (b_folder / 'phases.csv').exists():
            result['right_phases'] = phase_report(load_csv(b_folder / 'phases.csv', 120000))
        result['poll_alignment'] = divergence(a, b)
        a_play = next((i for i, r in enumerate(a) if r['playing'] == '1'), len(a))
        b_play = next((i for i, r in enumerate(b) if r['playing'] == '1'), len(b))
        result['gameplay_start_alignment'] = divergence(a[a_play:], b[b_play:])
        result['gameplay_start_positions'] = pose_comparison(a[a_play:], b[b_play:])
        if (b_folder / 'updates.csv').exists():
            b_updates = observations(b_folder, 'updates.csv', 'update')
            result['right_updates'] = update_timing(b_updates)
            result['right_start_state'] = start_state(b_folder / 'start-state.ini')
            result['right_motion_segments'] = input_motion_segments(b_updates, b_inputs)
            if a_updates is not None:
                result['update_alignment'] = divergence(a_updates, b_updates, 'updates')
                result['update_positions'] = pose_comparison(a_updates, b_updates)
                x, y = result['left_start_state'], result['right_start_state']
                if x and y:
                    result['start_state_comparison'] = {
                        'level_health_position_match': x['level'] == y['level'] and x['hp'] == y['hp'] and
                            all(abs(float(x[k]) - float(y[k])) <= 1e-4 for k in ('x', 'y', 'z')),
                        'raw_region_digest_match': x['save_digest'] == y['save_digest'],
                        'note': 'This subset does not establish equality of the complete process state.'}
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
