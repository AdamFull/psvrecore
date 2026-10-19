#pragma once

#include "core/foundation/core/foundation.h"

namespace psvrecore::usse::detail {

inline constexpr u32 kConstantWordCount = 130;

extern const u32 kConstantWords[kConstantWordCount];
extern const char* const kConstantF32Text[kConstantWordCount];
extern const char* const kConstantF16Text[kConstantWordCount * 2];  // half i of word i / 2 (low half first)

}  // namespace psvrecore::usse::detail
