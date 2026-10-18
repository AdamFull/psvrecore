#pragma once

// psvrecore's log categories, for nx::log: nx::logi(logs::kernel, "...", ...), nx::logw(...) for problems.
// Products of the tools (listings, --help) and output while crashing are written directly instead.

#include "core/foundation/diagnostics/log.h"

namespace psvrecore::logs {

inline const nx::log::Category loader = nx::log::category("loader");      // modules, relocations, imports
inline const nx::log::Category kernel = nx::log::category("kernel");      // threads, synchronisation, memory blocks
inline const nx::log::Category hle = nx::log::category("hle");            // system library calls
inline const nx::log::Category io = nx::log::category("io");              // files, mounts, archives, save data
inline const nx::log::Category gxm = nx::log::category("gxm");            // GXM state, scenes, shader programs
inline const nx::log::Category gpu = nx::log::category("gpu");            // the Vulkan backend
inline const nx::log::Category display = nx::log::category("display");    // window, presenting
inline const nx::log::Category audio = nx::log::category("audio");
inline const nx::log::Category input = nx::log::category("input");
inline const nx::log::Category perf = nx::log::category("perf");          // measurements, profiling reports
inline const nx::log::Category frontend = nx::log::category("frontend");  // the game's front end, tests
inline const nx::log::Category tool = nx::log::category("tool");          // host tools

}  // namespace psvrecore::logs
