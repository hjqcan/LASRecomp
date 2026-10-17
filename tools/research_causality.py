"""Bounded seed, save-byte and loading observations; correlations are not full causality."""
import re
from research_semantics import rng_distance, rng_state, unsigned
SAVE_WINDOW_SIZE = 0x259


def seed_initial(seed):
    result = []
    for value in (123456789, 362436069, 521288629, 88675123):
        value ^= seed & 0xffffffff
        value ^= value >> 21
        value ^= value >> 4
        result.append(value * 0x4f6cdd1d & 0xffffffff)
    return tuple(result)


def seed_report(events, phases, start):
    for i, row in enumerate(events):
        if int(row['event']) != i or row['startup'] not in ('0', '1'):
            raise ValueError('Invalid seed event sequence')
        for key in ('caller', 'thread', 'elapsed_ns', 'original', 'effective'):
            unsigned(row[key], 64)
        if tuple(unsigned(row['initial'+str(k)]) for k in range(4)) != seed_initial(int(row['effective'])):
            raise ValueError('Seed initializer model mismatch')
        if row['startup'] == '0' and row['original'] != row['effective']:
            raise ValueError('Seed control changed an unrelated time call')
    calls = [r for r in events if r['startup'] == '1']
    result = {'time_calls': len(events), 'startup_calls': len(calls)}
    if len(calls) == 1:
        call = calls[0]
        initial = seed_initial(int(call['effective']))
        first = next((r for r in phases if r['phase'] == '0' and r['before'] == '1' and
                      int(r['elapsed_ns']) >= int(call['elapsed_ns']) and rng_state(r) is not None), None)
        gate = rng_state(start) if start else None
        result.update(original_return=int(call['original']), effective_seed=int(call['effective']) & 0xffffffff,
                      expected_initial_state=initial,
                      first_update_counter=None if first is None else int(first['counter']),
                      inferred_initial_to_first_advances=None if first is None else rng_distance(initial, rng_state(first), 65536),
                      inferred_initial_to_gate_advances=None if gate is None else rng_distance(initial, gate, 65536))
    result['note'] = ('The complete initializer is guarded. Advance inference searches at most 65536 steps; '
                      'a miss may reflect more consumption, reset or concurrent observations. '
                      'Fixing this seed does not restore actors or align other clocks/loading.')
    return result


def fnv64(data):
    value = 14695981039346656037
    for byte in data:
        value = (value ^ byte) * 1099511628211 & 0xffffffffffffffff
    return value


def save_windows(rows, updates, start):
    if len(rows) != len(updates) + 1 or start is None:
        raise ValueError('Incomplete save-window sequence')
    values = []
    for i, row in enumerate(rows):
        if int(row['update']) != i-1 or row['available'] not in ('0', '1'):
            raise ValueError('Invalid save-window ordinal or flag')
        digest = int((start if i == 0 else updates[i-1])['save_digest'])
        if row['available'] == '0':
            if row['hex'] or digest != 0:
                raise ValueError('Unavailable save window has content or a nonzero digest')
            values.append(None)
            continue
        if not re.fullmatch(rf'[0-9a-f]{{{2*SAVE_WINDOW_SIZE}}}', row['hex']):
            raise ValueError('Invalid save-window encoding or size')
        data = bytes.fromhex(row['hex'])
        if fnv64(data) != digest:
            raise ValueError('Save-window content does not match its observation digest')
        values.append(data)
    return values


def byte_comparison(left, right, left_external=(), right_external=()):
    available, matching, changed, first = 0, 0, set(), None
    for i, (a, b) in enumerate(zip(left, right)):
        if a is None or b is None:
            continue
        available += 1
        offsets = [k for k, (x, y) in enumerate(zip(a, b)) if x != y]
        if not offsets:
            matching += 1
            continue
        changed.update(offsets)
        if first is None:
            ranges = []
            for offset in offsets:
                if ranges and ranges[-1][1] == offset:
                    ranges[-1][1] += 1
                else:
                    ranges.append([offset, offset+1])
            candidates = []
            for width in (4, 8):
                mask = (1 << (width*8))-1
                clocks = [{(int(r['value']) & mask).to_bytes(width, 'little') for r in rows if r['kind'] == '1'}
                          for rows in (left_external, right_external)]
                for offset in range(len(a)-width+1):
                    if a[offset:offset+width] != b[offset:offset+width] and a[offset:offset+width] in clocks[0] and b[offset:offset+width] in clocks[1]:
                        candidates.append({'save_offset': hex(0x5388+offset), 'bytes': width})
            first = {'update': i-1, 'changed_ranges': [
                {'save_offset': hex(0x5388+a0), 'bytes': b0-a0, 'left': a[a0:b0].hex(), 'right': b[a0:b0].hex()}
                for a0, b0 in ranges], 'clock_value_candidates': candidates}
    return {'available_pairs_including_start': available, 'matching_pairs_including_start': matching,
            'changed_save_offsets': [hex(0x5388+n) for n in sorted(changed)], 'first_difference': first,
            'note': 'Exact bytes of the existing 0x5388..0x55e0 window, validated against the captured digest. '
                    'Clock candidates are byte correlations, not independently established field definitions.'}


def loading_report(rows):
    result, previous, peak, last_io = [], None, 0, None
    for i, row in enumerate(rows):
        if 'io_started' not in row:
            continue
        io = {k: unsigned(row['io_'+k], 64) for k in ('started', 'completed', 'bytes', 'failed')}
        if io['completed'] > io['started'] or io['failed'] > io['completed'] or (last_io and any(io[k] < last_io[k] for k in io)):
            raise ValueError('Invalid RomFS observation counters')
        last_io = io
        peak = max(peak, io['started']-io['completed'])
        level = row['level'] if row['observed'] == row['playing'] == '1' else None
        if level and level != previous:
            position = None
            for j in range(i, len(rows)):
                candidate = rows[j]
                if candidate['observed'] != '1' or candidate['playing'] != '1':
                    continue
                if candidate['level'] and candidate['level'] != level:
                    break
                if candidate['level'] == level and candidate['position'] == '1':
                    position = j
                    break
            result.append({'update': i, 'level': level, 'romfs_at_entry': io,
                           'first_position_update': position})
            previous = level
    return {'level_entries': result, 'peak_observed_reads_in_flight': peak, 'last_romfs_counters': last_io,
            'note': 'Only the RomFS IStorage read span. Zero reads in flight does not establish scene readiness: '
                    'decompression, actor creation, GPU uploads and other tasks are not covered. Player position '
                    'availability also includes the existing locator delay of 30 presented frames after a level change.'}
