#!/usr/bin/env python3
"""Summary of a Dynamic resolution run's log (tools/dynres_run.sh; Metal or Vulkan): render-scale changes, hitches
(LARECOMP_LOG_HITCH_MS), fps and the factor, counted from the first warp on, hitches and fps away from the warps."""
import re
import sys

START, STEP, N = 2400, 1800, 9  # (tools/dynres_run.sh's warps)
WARPS = [START + i * STEP for i in range(N)]


def near_warp(f):
    return any(w - 30 <= f < w + 300 for w in WARPS)


for path in sys.argv[1:]:
    t = open(path, errors='replace').read()
    sc = [(int(f), d, float(s)) for f, d, s in re.findall(r'\[scale\] frame (\d+) drawable (\S+) -> render scale ([\d.]+)x', t)]
    changes = [b for a, b in zip(sc, sc[1:]) if b[0] >= START + 300 and b[1] == a[1] and b[2] != a[2]]
    hit = [(int(f), float(g)) for f, g in re.findall(r'\[hitch\] t=[\d.]+ frame (\d+) gap (\d+) ms', t)]
    play = [h for h in hit if h[0] >= START + 300]
    away = [g for f, g in play if not near_warp(f)]
    fps = [float(v) for f, v in re.findall(r'\[(?:metal|vk)\] t=[\d.]+ frame (\d+) ([\d.]+) fps', t)
           if int(f) >= START + 300 and not near_warp(int(f))]
    dyn = re.findall(r'\[dynres\] ([\d.]+) -> ([\d.]+) \(frame [\d.]+ ms.*?gpu ([\d.-]+) ms', t)
    facs = [float(b) for _, b, _ in dyn]
    gpu = [float(g) for *_, g in dyn if float(g) > 0]
    frames = re.findall(r'\[(?:metal|vk)\] t=[\d.]+ frame (\d+)', t)
    nwarps = len(re.findall(r'\[warp\] jumping to', t))
    print(f'{path}: up to frame {frames[-1] if frames else "?"}, drawable {sc[0][1] if sc else "?"}, {nwarps} warps')
    full = sc[0][2] if sc else 0  # (Dynamic starts at the full Match scale)
    print(f'  render-scale changes in play: {len(changes)}' +
          (f' (factors: {" ".join(f"{s / full:.2f}" for _, _, s in changes[:30])})' if full and changes else ''))
    print(f'  hitches over 40 ms in play: {len(play)}; away from warps {len(away)} (over 100 ms {sum(g > 100 for g in away)}, '
          f'over 250 ms {sum(g > 250 for g in away)}' + (f', worst {max(away):.0f} ms)' if away else ')'))
    if fps:
        print(f'  fps away from warps: mean {sum(fps) / len(fps):.1f}, lowest second {min(fps):.1f}, '
              f'{sum(v < 58 for v in fps)} of {len(fps)} seconds under 58')
    if facs:
        print(f'  factor per decision: mean {sum(facs) / len(facs):.2f}, lowest {min(facs):.2f}; GPU per frame '
              f'(G + W): mean {sum(gpu) / max(1, len(gpu)):.1f} ms, highest {max(gpu, default=0):.1f}')
    bad = re.findall(r'FATAL.*|\*\*\* .*', t)
    if bad:
        print('  !!', bad[:3])
