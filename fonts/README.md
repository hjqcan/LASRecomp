# System fonts

The game draws its text with the Switch's built-in system font, which isn't part of the game files.
Without it, Multi uses a system font. Windows Chinese modes prefer Microsoft YaHei (simplified) or
Microsoft JhengHei (traditional), since the Japanese Yu Gothic fallback lacks some Chinese glyphs.
Other Windows languages use Yu Gothic; macOS uses Hiragino Sans (the same style of Japanese gothic as
the Switch's Morisawa UD Shin Go), then Arial. No system fonts are copied into or shipped with Multi.
On Linux it uses bundled BIZ UDPGothic, then DejaVu Sans, with Switch-compatible vertical metrics.

For the exact look, put your own copies of the system fonts here:

- `nintendo_udsg-r_std_003.bfttf`: the main font (a `.ttf` with the same name, or spelled `nintendo_udsgr_std_003`, also works)
- `nintendo_ext_003.bfttf`: button glyphs (optional)

`LARECOMP_FONT=<file>` overrides everything (.ttf / .otf / .ttc / .bfttf).

## Settings menu

The menu also supports English, Simplified Chinese and Traditional Chinese. Its Chinese glyphs are merged into
the shipped Latin font before rendering, so changing languages takes effect immediately without rebuilding the
font atlas. Windows uses Microsoft YaHei / JhengHei, macOS uses PingFang / STHeiti, and Linux uses Noto Sans CJK
or WenQuanYi Micro Hei. Install a CJK font on Linux if none is present.

`LARECOMP_UI_FONT=<file>` selects a `.ttf` or `.ttc` for menu glyphs; alternatively, put `ui-cjk.ttf` or `ui-cjk.ttc`
in the data folder's `fonts/` directory. This is independent of the game's `LARECOMP_FONT` setting. System fonts
are read locally, never copied into the distribution.
