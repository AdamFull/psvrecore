# Noto Sans JP

ScePvf's Japanese system font (`scePvfOpen` of the third system font, the default Japanese font, and the searches for
Japanese), built into the runtime (`libs/runtime/CMakeLists.txt` turns it into a byte array). It has kana, the JIS
kanji, full-width forms and Latin glyphs; an OpenType font with CID-keyed CFF outlines.

- Origin: Noto Sans CJK, the Japanese subset, version 2.004, https://github.com/notofonts/noto-cjk at f8d1575
  (`Sans/SubsetOTF/JP/NotoSansJP-Regular.otf`, and `Sans/LICENSE`).
- License: SIL Open Font License 1.1 (`LICENSE`, copied unchanged from that repository).
- Taken: `NotoSansJP-Regular.otf`, unchanged. The bold weight (4.7 MB more in every program) is not taken: a Japanese
  bold search gets the regular one.
