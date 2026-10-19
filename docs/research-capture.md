# Experimental capture tools

The `multi` branch includes developer tools for investigating LAS input,
timing, RNG state and scene transitions. They are disabled during ordinary play
and do not provide deterministic replay, higher-rate simulation or online co-op.

## Tools

| Tool | Purpose |
| --- | --- |
| `tools/research_run.py` | Capture/replay with isolated settings and copied saves |
| `tools/research_analyze.py` | Compare input, timing and observed game state |
| `tools/research_semantics.py` | Inspect RNG, actor and external-return observations |
| `tools/research_causality.py` | Inspect startup seed, save bytes and loading activity |
| `tools/research_instrument.py` | Build a separate diagnostic source tree with store probes |
| `tools/research_stores.py` | Compare bounded store traces and state chains |

Use each script's `--help` for its arguments. Start with a compatible LAS
installation and a copy of that version's save. Capture and replay must use the
same executable, library, resources and settings. Keep outputs outside the
checkout. Store probes use a separate diagnostic library; do not replace a
normal game's library with instrumented code.

## Limits

Matching input or sampled positions does not establish identical game state.
RNG writes, callbacks, loading and scheduling may still diverge. Partial traces,
buffer overflow and missing observations must remain explicit. Camera matrix
observations do not provide a frozen free camera or rendering replay.

The [workshop route viewer](workshop.md#route-viewer) displays recorded samples;
it does not restore or drive game state. The [local co-op prototype](local-coop.md)
is a separate opt-in feature. Keep game assets, saves and capture outputs private.
