# ACNH 60 FPS + SpeedFix by tmouh

Source: <https://github.com/tmouh/acnh-60fps/tree/c3f162eb3e7e76b6cd30d2366d21233840673a9d>

Credit and downloads: <https://gamebanana.com/mods/722295>

The patches in this directory are **CC BY-NC 4.0**, copyright 2026 tmouh;
see [LICENSE](LICENSE). They are separate from the runtime's MIT code. They
must retain this attribution and may not be used commercially.

The original SDK and 3.0.1 main pchtxt files and StaticParam IPS are unchanged.
The three BCSV resources have been converted to 107 guarded four-byte edits
in `manifest.json`; the complete resource files are not included. The runtime
prepares all four resources from the player's own unmodified game files.

LAS Recomp's NVN backend implements the SDK patch's one-refresh presentation
policy after verifying the complete prepared installation. The module patches
are applied before native compilation, including the 83 words in read-only and
data segments. Normal and patched builds have distinct native library identities
and installation directories. Switching modes requires restarting the game.

This is a fixed 60 FPS speed correction, not dynamic frame-rate compensation.
Below 60 FPS the game slows down. Default game behavior remains 30 FPS.
