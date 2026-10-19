"""Validate partial generated-store observations; never infer a random-call count."""
import csv
import hashlib
import json
from pathlib import Path
from research_semantics import unsigned, rng_state

FORMAT = 'las_generated_store_trace_v1'


def manifest_read(path):
    path = Path(path)
    if path.stat().st_size > 32*1024*1024:
        raise ValueError('Store manifest exceeds 32 MiB')
    raw = path.read_bytes()
    value = json.loads(raw)
    if value.get('format') != FORMAT or value.get('complete_coverage') is not False:
        raise ValueError('Unsupported store manifest or coverage claim')
    sites = value.get('sites', [])
    if not sites or len(sites) > 262144:
        raise ValueError('Invalid store-site count')
    for i, row in enumerate(sites):
        if row['id'] != i+1 or row['bits'] not in (8, 16, 32, 64, 128) or row['line'] < 1:
            raise ValueError('Invalid store-site identity')
        unsigned(str(int(row['function'], 16)), 64)
    return value, hashlib.sha256(raw).hexdigest()


def read_csv(path, limit, size=128*1024*1024):
    if path.stat().st_size > size:
        raise ValueError('Store CSV exceeds its byte limit')
    with path.open(newline='', encoding='utf-8') as stream:
        rows = []
        for row in csv.DictReader(stream):
            if len(rows) == limit or None in row or any(v is None for v in row.values()):
                raise ValueError('Invalid or oversized store CSV')
            rows.append(row)
    return rows


def words(row, prefix):
    return tuple(unsigned(row[prefix+str(i)]) for i in range(4))


def validate(meta, rows, boundaries, manifest, identity, updates):
    if meta.get('format') != '1' or meta.get('manifest') != identity:
        raise ValueError('Store trace and library manifest differ')
    first, end = int(meta['first_update']), int(meta['end_update'])
    capacity = int(meta['capacity'])
    if not 0 <= first < end <= 12000 or not 1 <= capacity <= 262144:
        raise ValueError('Invalid store trace bounds')
    if int(meta['recorded']) != len(rows) or len(rows) > capacity:
        raise ValueError('Store CSV length differs from its receipt')
    problems = []
    for key in ('dropped', 'pending', 'overlaps', 'outside_update', 'invalid'):
        if unsigned(meta[key], 64):
            problems.append(key)
    if meta['recording_complete'] != '1':
        problems.append('runtime_recording_incomplete')
    if meta['window_opened'] != '1' or meta['window_closed'] != '1':
        problems.append('window_incomplete')
    sites = {s['id']: s for s in manifest['sites']}
    target = int(manifest['target'], 16)
    all_sequences, by_update, parsed = set(), {}, []
    previous_begin = 0
    for i, row in enumerate(rows):
        if int(row['write']) != i:
            raise ValueError('Invalid store ordinal')
        r = {k: unsigned(row[k], 64) for k in ('begin', 'end', 'elapsed_ns', 'site', 'function', 'caller', 'address', 'size', 'window_update', 'thread')}
        r['update'] = int(row['update'])
        if r['begin']>2*capacity+24000 or r['end']>2*capacity+24000:
            raise ValueError('Store sequence exceeds its bounded recording')
        r['before'], r['after'] = words(row, 'before'), words(row, 'after')
        site = sites.get(r['site'])
        if not site or int(site['function'], 16) != r['function'] or site['bits'] != 8*r['size']:
            raise ValueError('Store record does not match its generated source site')
        if not target-r['size'] < r['address'] < target+16:
            raise ValueError('Store record is outside the observed region')
        if not first <= r['window_update'] < end or r['update'] < -1:
            raise ValueError('Invalid store update attribution')
        if r['update'] != r['window_update']:
            problems.append('unowned_write')
        if r['begin'] <= previous_begin:
            raise ValueError('Store starts are not ordered')
        previous_begin = r['begin']
        for sequence in (r['begin'], r['end']):
            if not sequence:
                problems.append('unpaired_write')
                continue
            if sequence in all_sequences:
                raise ValueError('Duplicate observation sequence')
            all_sequences.add(sequence)
        if r['end'] <= r['begin']:
            problems.append('unpaired_write')
        else:
            before = b''.join(x.to_bytes(4, 'little') for x in r['before'])
            after = b''.join(x.to_bytes(4, 'little') for x in r['after'])
            if any(a != b and not r['address'] <= target+n < r['address']+r['size']
                   for n, (a, b) in enumerate(zip(before, after))):
                problems.append('changes_outside_recorded_write')
        by_update.setdefault(r['window_update'], []).append(r)
        parsed.append(r)
    bound_map = {}
    previous_sequence = 0
    for row in boundaries:
        seq, update, thread = (unsigned(row[k], 64) for k in ('sequence', 'update', 'thread'))
        if seq>2*capacity+24000:
            raise ValueError('Boundary sequence exceeds its bounded recording')
        if row['before'] not in ('0', '1') or not first <= update < end or seq <= previous_sequence or seq in all_sequences:
            raise ValueError('Invalid store boundary sequence')
        previous_sequence = seq;all_sequences.add(seq)
        key = (update, row['before'])
        if key in bound_map:
            raise ValueError('Duplicate store update boundary')
        bound_map[key] = (seq, words(row, 'rng'), thread)
    missing, gaps, reconciled = [], [], 0
    for update in range(first, end):
        a, b = bound_map.get((update, '1')), bound_map.get((update, '0'))
        if not a or not b or update >= len(updates):
            missing.append(update)
            continue
        if a[1] != rng_state(updates[update], 'rng_before') or b[1] != rng_state(updates[update]):
            problems.append('boundary_differs_from_update_sample')
        state, sequence = a[1], a[0]
        coherent = a[0] < b[0]
        for w in by_update.get(update, []):
            if (w['before'] != state or w['begin'] <= sequence or not w['begin'] < w['end'] < b[0] or
                    w['update'] != update or w['thread'] != a[2]):
                coherent = False
            state, sequence = w['after'], w['end']
        if state != b[1]:
            coherent = False
        if coherent:
            reconciled += 1
        elif len(gaps) < 12:
            gaps.append(update)
    if missing:
        problems.append('missing_boundaries')
    if reconciled != end-first:
        problems.append('state_chain_gaps')
    if all_sequences and all_sequences != set(range(1, max(all_sequences)+1)):
        problems.append('observation_sequence_gaps')
    report = dict(manifest=identity, coverage=manifest['coverage'], complete_coverage=False,
                  requested_window=[first, end], recorded_writes=len(parsed), reconciled_updates=reconciled,
                  observed_recording_complete=not problems, problems=sorted(set(problems)),
                  first_state_chain_gap_updates=gaps, first_missing_updates=missing[:12],
                  consumption_reconstruction='not_established',
                  note='Source sites are original generated-C lines, not guest instruction PCs. '
                       'A continuous observed state chain does not prove all writers are covered. '
                       'Writes and random-number consumption are different units; no call count is reconstructed.')
    return report, parsed, bound_map


def trace_read(folder, updates):
    if not (folder/'stores-summary.ini').exists():
        return None
    path = folder/'stores-summary.ini'
    if path.stat().st_size > 16384:
        raise ValueError('Oversized store summary')
    meta = {}
    for line in path.read_text().splitlines():
        key, value = line.split('=', 1)
        if key in meta:
            raise ValueError('Duplicate store metadata')
        meta[key] = value
    manifest, identity = manifest_read(folder/'stores-manifest.json')
    report, writes, bounds = validate(meta, read_csv(folder/'stores.csv', 262144),
                                     read_csv(folder/'store-boundaries.csv', 24000, 8*1024*1024), manifest, identity, updates)
    return report, writes, bounds, manifest


def comparison(left, right):
    a_report, a, a_bounds, manifest = left
    b_report, b, b_bounds, _ = right
    if a_report['manifest'] != b_report['manifest'] or a_report['requested_window'] != b_report['requested_window']:
        return dict(comparable=False, verdict='incomparable', reason='Different source manifests or windows')
    sites = {s['id']: s for s in manifest['sites']}
    def describe(w):
        if w is None:
            return None
        site = sites[w['site']]
        return dict(site=w['site'], function=hex(w['function']), caller=hex(w['caller']),
                    source_file=site['file'], source_line=site['line'], address=hex(w['address']), bytes=w['size'],
                    thread=w['thread'], before=list(w['before']), after=list(w['after']))
    def signature(w):
        return tuple(w[k] for k in ('site', 'caller', 'address', 'size', 'before', 'after'))
    first, matching, compared = None, 0, 0
    by_a, by_b = {}, {}
    for rows, by in ((a, by_a), (b, by_b)):
        for row in rows:
            by.setdefault(row['window_update'], []).append(row)
    start, end = a_report['requested_window']
    for update in range(start, end):
        aa, bb = by_a.get(update, []), by_b.get(update, [])
        for i in range(max(len(aa), len(bb))):
            x, y = aa[i] if i < len(aa) else None, bb[i] if i < len(bb) else None
            compared += 1
            if x is not None and y is not None and signature(x) == signature(y):
                matching += 1
            elif first is None:
                first = dict(update=update, write_in_update=i, left_writes=len(aa), right_writes=len(bb),
                             differing_fields=['write_missing'] if x is None or y is None else
                                 [k for k in ('site', 'caller', 'address', 'size', 'before', 'after') if x[k] != y[k]],
                             left=describe(x), right=describe(y),
                             left_context=[describe(w) for w in aa[max(0, i-2):i+3]],
                             right_context=[describe(w) for w in bb[max(0, i-2):i+3]])
    complete = a_report['observed_recording_complete'] and b_report['observed_recording_complete']
    initial_a, initial_b = a_bounds.get((start, '1')), b_bounds.get((start, '1'))
    starts_match = initial_a[1] == initial_b[1] if initial_a and initial_b else None
    return dict(comparable=True, verdict='incomplete' if not complete else 'observed_writes_differ' if first else 'observed_writes_match',
                window_start_rng_match=starts_match, compared_writes=compared, matching_writes=matching,
                first_observed_write_difference=first, complete_coverage=False, consumption_reconstruction='not_established',
                note='Compares observed writes within each captured update. A difference is not an extra RNG call or a proven cause; '
                     'the true first process-state difference may precede this window or lie outside coverage. '
                     'The caller field is live guest X30/LR, not an unwound call stack.')
