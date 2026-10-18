# Pokemon Legends: Z-A development port

Z-A 1.0.0 is a development adapter on `enhanced`. It is not playable and is
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

## Remaining blockers

- CPU discovery stops at `main+0x1748400`, reached from `main+0x1749040`.
  This six-case byte table has a range guard 34 instructions before its indirect
  branch, beyond the current guarded fallback's 16-instruction search window.
- GPU surface stores/reductions (SUST/SURED) remain unsupported. Startup images
  are black. Extending CPU discovery alone will not make the game playable.

Keep unsupported capabilities explicit in `adapters/core-capabilities.json`.
Do not enable normal installation until startup, gameplay, rendering and saving
work. See [adapter development](adapter-development.md) for the shared workflow.

## Developer commands

Use your own game files and a separate writable directory:

```sh
python tools/adapter.py probe --archive <base.nsz> --output <inventory.json> --exefs-output <local-exefs>
python tools/gen.py main <local-exefs>/main 0x7000000000 <local-generated>/main --profile generic --simd c
python tools/gen.py sdk <local-exefs>/sdk 0x7008000000 <local-generated>/sdk --profile generic --hle runtime/hle/hle_symbols.txt --simd c
python tools/adapter.py doctor --game pokemon_za --log <runtime.log>
```

Use `LARECOMP_STRICT_HLE=1` and `LARECOMP_STRICT_IPC=1` when investigating missing
services. Keep game files, generated code, libraries, keys and saves out of Git.
