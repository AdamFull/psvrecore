# Roboto

ScePvf's Latin system fonts (`scePvfOpen`, the font list, the default Latin font): Roboto Regular and Bold, built
into the runtime (`libs/runtime/CMakeLists.txt` turns them into byte arrays). They have Latin, Greek and Cyrillic
glyphs; the Japanese, Korean and Chinese ones are Noto Sans CJK (`third_party/noto-cjk`).

- Origin: Roboto (Roboto Classic) version 3.009, https://github.com/googlefonts/roboto-classic; the copies here were
  taken unchanged from Tracy (`libs/nx-foundation/third_party/tracy/profiler/src/font`, Tracy v0.13.1).
- License: SIL Open Font License 1.1 (`OFL.txt`; the fonts' own name tables say so). The licence text there is the
  one SDL ships with its test font.
- Taken: `Roboto-Regular.ttf`, `Roboto-Bold.ttf`, unchanged.
