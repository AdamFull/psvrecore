# Noto Sans CJK (Japanese, Korean, Simplified and Traditional Chinese)

ScePvf's system fonts for Japanese, Korean and Chinese (`scePvfOpen` of the third to sixth system fonts, the default
Japanese font, and the searches by language), built into the runtime (`libs/runtime/CMakeLists.txt` turns each into a
byte array). OpenType fonts with CID-keyed CFF outlines, each with Latin glyphs as well:

- `NotoSansJP-Regular.otf`: kana, the JIS kanji, full-width forms (Japanese).
- `NotoSansKR-Regular.otf`: Hangul, the KS X 1001 hanja, kana (Korean, and the Japanese-and-Korean searches).
- `NotoSansSC-Regular.otf`: Simplified Chinese (the Chinese searches).
- `NotoSansTC-Regular.otf`: Traditional Chinese (listed, and found by the exact Chinese search after SC).

Origin: Noto Sans CJK version 2.004, the per-region subsets, https://github.com/notofonts/noto-cjk at f8d1575
(`Sans/SubsetOTF/<JP|KR|SC|TC>/NotoSans<JP|KR|SC|TC>-Regular.otf`, and `Sans/LICENSE`).

License: SIL Open Font License 1.1 (`LICENSE`, copied unchanged from that repository).

Taken: the four Regular fonts, unchanged. The bold weights (4.6 to 8.6 MB more each in every program) are not taken: a
bold search in these languages gets the regular font.
