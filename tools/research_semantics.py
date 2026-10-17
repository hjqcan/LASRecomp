"""Read-only evidence for one identified LAS RNG and selected external returns.

No state is restored. Inferring an advance count is a bounded hypothesis, not an
observation of the callers, a proof of atomic sampling, or full RNG coverage.
"""
from collections import Counter
from functools import lru_cache
import math
import re


def unsigned(value, bits=32):
    n = int(value)
    if not 0 <= n < 1 << bits:
        raise ValueError('Semantic observation is outside its unsigned range')
    return n


def rng_state(row, prefix='rng'):
    flag = row.get(prefix + '_observed')
    if flag is None:
        return None  # Captures made before the semantic probe remain readable.
    if flag not in ('0', '1'):
        raise ValueError('Invalid RNG observation flag')
    values = tuple(unsigned(row[prefix + str(i)]) for i in range(4))
    return values if flag == '1' else None


def rng_next(state):
    x, y, z, w = state
    t = (x ^ (x << 11)) & 0xffffffff
    return y, z, w, (w ^ (w >> 19) ^ t ^ (t >> 8)) & 0xffffffff


@lru_cache(maxsize=65536)
def rng_distance(before, after, limit=4096):
    if not any(before) or not any(after):
        return None  # The all-zero absorbing state cannot establish consumption.
    state = before
    for count in range(limit + 1):
        if state == after:
            return count
        state = rng_next(state)
    return None


def rng_report(rows):
    within, between = Counter(), Counter()
    unexplained, observed, zero, previous = [], 0, 0, None
    for i, row in enumerate(rows):
        before, after = rng_state(row, 'rng_before'), rng_state(row)
        if before is not None and after is not None:
            observed += 1
            zero += not any(before) or not any(after)
            n = rng_distance(before, after)
            within['unexplained' if n is None else str(n)] += 1
            if n is None and len(unexplained) < 8:
                unexplained.append({'update': i, 'before': before, 'after': after})
        if previous is not None and before is not None:
            n = rng_distance(previous, before)
            between['unexplained' if n is None else str(n)] += 1
        previous = after
    return {'updates': len(rows), 'observed_pairs': observed,
            'within_dispatch_advances': dict(within), 'between_dispatch_advances': dict(between),
            'zero_state_pairs': zero, 'first_unexplained_pairs': unexplained,
            'note': 'Only the guarded global xorshift128 state. Counts search at most 4096 steps; '
                    'unexplained spans may exceed that bound, reset, race or use another transition. '
                    'Counts do not identify callers or certify a coherent snapshot.'}


def rng_comparison(left, right):
    count, matched, prefix, first, prefix_open = 0, 0, 0, None, True
    for i, (a, b) in enumerate(zip(left, right)):
        x, y = rng_state(a), rng_state(b)
        if x is None or y is None:
            prefix_open = False
            continue
        count += 1
        if x == y:
            matched += 1
            if prefix_open:
                prefix += 1
        else:
            prefix_open = False
            if first is None:
                first = {'update': i, 'left': x, 'right': y}
    return {'available_pairs': count, 'matching_pairs': matched, 'equal_prefix_updates': prefix,
            'all_observed_states_match': bool(count) and count == matched == len(left) == len(right),
            'first_difference': first}


def external_report(rows):
    counts = Counter()
    for i, row in enumerate(rows):
        if int(row['event']) != i or row['kind'] not in ('0', '1') or row['gate_ready'] not in ('0', '1'):
            raise ValueError('Invalid external observation sequence')
        for key in ('pc', 'thread', 'index', 'value', 'elapsed_ns', 'completed_updates'):
            unsigned(row[key], 64)
        counts['entropy' if row['kind'] == '0' else 'user_system_clock'] += 1
    return {'samples': len(rows), 'by_source': dict(counts),
            'before_gate': sum(r['gate_ready'] == '0' for r in rows),
            'note': 'Observes GetInfo(11) and the StandardUserSystemClock HLE return only; '
                    'not all entropy or clocks, and no claim that entropy seeds the game RNG.'}


def external_comparison(left, right):
    result = {}
    for kind, name in (('0', 'entropy'), ('1', 'user_system_clock')):
        a, b = ([r for r in rows if r['kind'] == kind] for rows in (left, right))
        first = next((i for i, (x, y) in enumerate(zip(a, b))
                      if any(x[k] != y[k] for k in ('pc', 'index', 'value'))), None)
        result[name] = {'left_samples': len(a), 'right_samples': len(b),
                        'all_returns_match': bool(a) and len(a) == len(b) and first is None,
                        'first_difference': None if first is None else {
                            'source_ordinal': first, 'left': a[first], 'right': b[first]}}
    return result


def actor_report(samples, rows):
    index, grouped = {}, {}
    for i, sample in enumerate(samples):
        update = unsigned(sample['update'])
        if update != i * 10 or sample['complete'] not in ('0', '1'):
            raise ValueError('Invalid actor sampling sequence')
        count, registry, issue = (unsigned(sample[k]) for k in ('selected_count', 'registry_count', 'issue'))
        if count > 64 or registry > 2048 or issue > 4 or (sample['complete'] == '1' and issue != 0):
            raise ValueError('Invalid actor sampling bounds')
        index[update] = sample
        grouped[update] = {}
    for row in rows:
        update, key = unsigned(row['update']), unsigned(row['key'], 64)
        unsigned(row['vtable'], 64)
        if update not in index or index[update]['complete'] != '1' or key in grouped[update]:
            raise ValueError('Actor belongs to an absent, partial or duplicate snapshot')
        if row['position'] not in ('0', '1') or not all(math.isfinite(float(row[k])) for k in ('x', 'y', 'z')):
            raise ValueError('Invalid actor position')
        if not re.fullmatch(r'(Npc|Enemy)[A-Za-z0-9_]*-[0-9A-Fa-f]{16}', row['name']) or int(row['name'][-16:], 16) != key:
            raise ValueError('Actor name/key mismatch')
        grouped[update][key] = row
    for update, sample in index.items():
        if sample['complete'] == '1' and len(grouped[update]) != int(sample['selected_count']):
            raise ValueError('Actor snapshot count mismatch')
    complete = sum(s['complete'] == '1' for s in samples)
    return {'samples': len(samples), 'complete_samples': complete, 'partial_samples': len(samples)-complete,
            'selected_actor_rows': len(rows), 'position_rows': sum(r['position'] == '1' for r in rows),
            'actor_names': sorted({r['name'] for r in rows}),
            'note': 'Every tenth captured update; placed Npc/Enemy entries and their +0x80 component coordinates. '
                    'Excludes other components, unregistered/dynamic actors, AI and animation state; not an atomic world snapshot.'}, grouped


def actor_comparison(left_samples, left_rows, right_samples, right_rows):
    _, a = actor_report(left_samples, left_rows)
    _, b = actor_report(right_samples, right_rows)
    right_index = {int(s['update']): s for s in right_samples}
    pairs, actors, position_pairs, maximum, first = 0, 0, 0, 0.0, None
    first_position, largest_position = None, None
    for sample in left_samples:
        update = int(sample['update'])
        other = right_index.get(update)
        if other is None or sample['complete'] != '1' or other['complete'] != '1':
            continue
        pairs += 1
        x, y = a[update], b[update]
        reason = 'level_or_roster' if sample['level'] != other['level'] or x.keys() != y.keys() else None
        for key in sorted(x.keys() & y.keys()):
            actors += 1
            l, r = x[key], y[key]
            same_identity = all(l[k] == r[k] for k in ('name', 'vtable', 'component'))
            if not same_identity or l['position'] != r['position']:
                reason = reason or 'actor_identity_or_availability'
            if same_identity and sample['level'] == other['level'] and l['position'] == r['position'] == '1':
                position_pairs += 1
                distance = math.sqrt(sum((float(l[k])-float(r[k]))**2 for k in ('x', 'y', 'z')))
                detail = {'update': update, 'actor': l['name'], 'level': sample['level'], 'distance': distance,
                          'left': l, 'right': r}
                if distance > maximum:
                    maximum, largest_position = distance, detail
                if distance > 1e-4:
                    if first_position is None:
                        first_position = detail
                    reason = reason or 'component_position'
                    if first is None:
                        first = {'update': update, 'reason': reason, 'actor': l['name'], 'distance': distance,
                                 'left': l, 'right': r}
        if reason and first is None:
            first = {'update': update, 'reason': reason, 'left_level': sample['level'], 'right_level': other['level']}
    return {'available_samples': pairs, 'selected_actor_pairs': actors, 'position_pairs': position_pairs,
            'max_component_position_difference': maximum, 'first_difference': first,
            'first_position_difference': first_position, 'largest_position_difference': largest_position,
            'all_samples_match': bool(pairs) and pairs == len(left_samples) == len(right_samples) and first is None}
