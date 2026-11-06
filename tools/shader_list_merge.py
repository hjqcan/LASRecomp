#!/usr/bin/env python3
"""Merge shader variant records (runtime/nvn/shader_prep.h: G / C lines, hashes and pipeline state only) into a list.

    tools/shader_list_merge.py OUT IN [IN ...]

Each IN is a list in the shipped format, a player's cache/shader_seen.txt (its "# shader_seen" header and any other
comment lines are dropped) or a lasrecomp.log: its "[shader] record G ..." / "... C ..." lines (each record the shipped
list lacks, logged once a run). OUT gets every record once, sorted, so a re-run changes only what's new. OUT is read
too when it exists: records are only ever added.

    tools/shader_list_merge.py runtime/nvn/shader_variants.txt lasrecomp.log
"""
import os
import sys

TAG = '[shader] record '


def records(path):
    with open(path, encoding='utf-8', errors='replace') as f:
        for line in f:
            line = line.strip()
            k = line.find(TAG)
            if k >= 0:
                line = line[k + len(TAG):]
            if line[:1] in ('G', 'C') and line[1:2] == ' ':
                yield line


def main():
    if len(sys.argv) < 3:
        sys.exit(__doc__)
    out, ins = sys.argv[1], sys.argv[2:]
    have = set(records(out)) if os.path.exists(out) else set()
    before = len(have)
    for p in ins:
        have.update(records(p))
    with open(out, 'w', encoding='utf-8', newline='\n') as f:
        for r in sorted(have):
            f.write(r + '\n')
    g = sum(1 for r in have if r[0] == 'G')
    print(f'{out}: {len(have)} records ({g} pipelines, {len(have) - g} compute), {len(have) - before} new')


if __name__ == '__main__':
    main()
