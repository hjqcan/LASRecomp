# System fonts

The game draws its text with the Switch's built-in system font, which isn't part of the game files.
Without it, LAS Recomp uses a system font. Windows Chinese modes prefer Microsoft YaHei (simplified) or
Microsoft JhengHei (traditional), since the Japanese Yu Gothic fallback lacks some Chinese glyphs.
Other Windows languages use Yu Gothic; macOS uses Hiragino Sans (the same style of Japanese gothic as
the Switch's Morisawa UD Shin Go), then Arial. No system fonts are copied into or shipped with LAS Recomp.

For the exact look, put a dump from your own console here (Switch system archive *FontStandard*, title
0100000000000811, and *FontNintendoExtension*, 0100000000000810):

- `nintendo_udsg-r_std_003.bfttf`: the main font (a `.ttf` with the same name, or spelled `nintendo_udsgr_std_003`, also works)
- `nintendo_ext_003.bfttf`: button glyphs (optional)

`LARECOMP_FONT=<file>` overrides everything (.ttf / .otf / .ttc / .bfttf).
