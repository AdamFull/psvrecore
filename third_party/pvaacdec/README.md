# pvaacdec

The PacketVideo (OpenCORE) MPEG-4 AAC decoder: AAC-LC, with SBR and parametric stereo. AvPlayer decodes the AAC
tracks of MP4 files with it.

- Origin: the Android Open Source Project, `platform/frameworks/base`, tag `android-4.0.4_r2.1`, folder
  `media/libstagefright/codecs/aacdec` (https://android.googlesource.com/platform/frameworks/base).
- License: Apache License 2.0 (`LICENSE`, as in that tag's `NOTICE`); every source file carries PacketVideo's header.
- Taken: the decoder's sources (`src/`), unchanged. Left out: `SoftAAC.cpp`, `SoftAAC.h` and `Android.mk` (Android's
  OpenMAX wrapper and build file).
- Built (libs/runtime/CMakeLists.txt) with the defines Android's `Android.mk` gave it: `AAC_PLUS`, `HQ_SBR`,
  `PARAMETRICSTEREO`, and `OSCL_IMPORT_REF`, `OSCL_EXPORT_REF`, `OSCL_UNUSED_ARG` empty.
