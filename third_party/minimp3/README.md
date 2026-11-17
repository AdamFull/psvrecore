# minimp3

A single-header MPEG audio decoder (layers I, II and III; MPEG-1, 2 and 2.5). SceAudiodecUser decodes MP3 with it
(`libs/runtime/src/media/mp3.cpp` holds its implementation).

- Origin: https://github.com/lieff/minimp3, commit `9f1f7c67d596e94ca1b8be70b58fddc94820bf15`.
- License: CC0 1.0 Universal (`LICENSE`, as in the repository).
- Taken: `minimp3.h`, unchanged. Left out: `minimp3_ex.h` (file and stream helpers), the player, tests and vectors.
