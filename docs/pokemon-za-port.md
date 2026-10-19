# Pokemon Legends: Z-A development port

Z-A 1.0.0 is a development adapter on `multi`. It is not playable and is
not available through normal installation. Update 2.0.2 and DLC are unsupported.

## Adapter

- Application ID: `0100f43008c44000`.
- Generic generator profile; no LAS gameplay patches or randomizer hooks.
- Main module at `0x7000000000`; SDK at `0x7008000000`.
- Exact module identities and runtime requirements are in
  [`adapters/pokemon_za/adapter.json`](../adapters/pokemon_za/adapter.json).

The shared runtime supports the port's dynamic memory layout, relative
relocations, exclusive loads/stores and bounded relative jump-table patterns.
These facilities are shared core behavior selected through the adapter.
Z-A also has a guarded generation rule for its six-case rotation table. It
checks the exact main Build ID and instruction words before extending the
range-check search at that one branch. Other branches keep the default window.

## Remaining blockers

- GPU surface stores/reductions (SUST/SURED) remain unsupported. Startup images
  are black. Extending CPU discovery alone will not make the game playable.

Keep unsupported capabilities explicit in `adapters/core-capabilities.json`.
Do not enable normal installation until startup, gameplay, rendering and saving
work. See [adapter development](adapter-development.md) for the shared workflow.

## Developer commands

Use your own game files and a separate writable directory:

```sh
python tools/adapter.py probe --exefs <local-exefs> --output <inventory.json>
python tools/gen_game.py --game-dir <local-game-directory> --outdir <local-generated> --simd c
python tools/adapter.py doctor --game pokemon_za --log <runtime.log>
```

The game directory must contain `exefs/main` and `exefs/sdk`. Use the complete
module plan above or the launcher so the reviewed Z-A generation rule is applied.

Use `LARECOMP_STRICT_HLE=1` and `LARECOMP_STRICT_IPC=1` when investigating missing
services. Keep game files, generated code, libraries, keys and saves out of Git.
