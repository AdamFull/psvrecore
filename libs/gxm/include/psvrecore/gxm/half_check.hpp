#pragma once

// The device's own conversion to half precision checked against the interpreter's rounding (translate.hpp,
// HalfConversion): the fastest way that gives the interpreter's bits on this device.

#include "psvrecore/gxm/translate.hpp"
#include "psvrecore/vulkan/device.hpp"

namespace psvrecore::gxm {

struct HalfCheck {
    HalfConversion conversion = HalfConversion::Integer;
    bool ran = false;
    bool differ = false, nans_differ = false;  // (values whose bits differ: not NaNs; NaNs)
    u32 example = 0;                          // (one of the former)
    f64 ms = 0;
};

// Runs half_check_shader over all its values on the device (waited for: about a billion values, a fraction of a
// second on a phone's GPU). PSVRECORE_HALF=integer skips it, keeping the integer arithmetic.
HalfCheck check_half_conversion(vulkan::Device& device);

}  // namespace psvrecore::gxm
