#pragma once

// The profiler's hooks (Tracy, built with PSVRECORE_TRACY=ON; nx-foundation's copy of Tracy's client). Off, every
// function here is an empty inline one and every macro expands to nothing: the hooks cost nothing.
//
// Tracy's client is built on demand (TRACY_ON_DEMAND): it records only while a profiler is connected, and a running
// game is profiled by connecting to it (docs/PROFILING.md).
//
// Guest threads run on host threads the kernel reuses, and a guest thread's fibers on its thread, so each guest thread
// and each guest fiber is a lane of its own in Tracy (a Tracy "fiber", named after the guest's name): what a guest
// thread does is in its lane, whichever host thread ran it. A host thread that runs no lane (the renderer's, the
// audio's) is its own thread in Tracy, named after its host name.

#include "core/foundation/core/foundation.h"
#include "core/foundation/strings/utf8_string.h"

#if PSVRECORE_TRACY
#include <tracy/Tracy.hpp>
#endif

namespace psvrecore::profile {

// A lane: a guest thread or fiber, as Tracy shows it.
struct Track;

#if PSVRECORE_TRACY

inline constexpr bool kEnabled = true;

// A lane named after the guest's name: the first with that name not in use, or a new one ("name 2", ...). Lanes are
// kept for the process (Tracy keeps their names); a released one is reused by the next thread or fiber of its name.
Track* acquire(nx::string_view name);
void release(Track* track);
// The calling host thread runs `track` from now on (null: none, its own lane again).
void enter(Track* track);
Track* current();
// Before a zone on a thread that may run a lane: a profiler connected since the lane was entered is told of it.
void announce();
// A profiler is connected (and records).
bool connected();

// The game's markers (Razor's CPU markers, the fibers' markers): zones of its label on the current lane, nested.
// `rgb`: 0xRRGGBB (0: Tracy's default). pop() ends the innermost; false if none is open.
void push(nx::string_view label, u32 rgb = 0);
bool pop();
// A zone of a fixed name and place (PSV_PROFILE_LOCATION) begun here and ended by pop() on the same thread: around
// what does not end in the scope it begins in. Nested with the markers above.
void begin(const tracy::SourceLocationData* where);

// A displayed frame (Tracy's frame mark).
inline void frame() { FrameMark; }
// A message on the timeline.
void message(nx::string_view text, u32 rgb = 0);

#else

inline constexpr bool kEnabled = false;

inline Track* acquire(nx::string_view) { return nullptr; }
inline void release(Track*) {}
inline void enter(Track*) {}
inline Track* current() { return nullptr; }
inline void announce() {}
inline bool connected() { return false; }
inline void push(nx::string_view, u32 = 0) {}
inline bool pop() { return false; }
inline void frame() {}
inline void message(nx::string_view, u32 = 0) {}

#endif

// Razor's colours (0xAABBGGRR, alpha 0x80) as Tracy's 0xRRGGBB.
constexpr u32 rgb_of_abgr(u32 abgr) { return ((abgr & 0xFF) << 16) | (abgr & 0xFF00) | ((abgr >> 16) & 0xFF); }

}  // namespace psvrecore::profile

#if PSVRECORE_TRACY
// A zone named `name` (a string literal) to the end of the scope, on the calling thread's lane.
#define PSV_PROFILE_ZONE(name)          \
    ::psvrecore::profile::announce();   \
    ZoneScopedN(name)
#define PSV_PROFILE_ZONE_COLOR(name, rgb) \
    ::psvrecore::profile::announce();     \
    ZoneScopedNC(name, rgb)
// A static zone location for profile::begin (`name`: a string literal, `rgb`: 0xRRGGBB or 0).
#define PSV_PROFILE_LOCATION(var, name, rgb) \
    static constexpr tracy::SourceLocationData var { name, TracyFunction, TracyFile, (uint32_t)TracyLine, rgb }
// Text shown with the zone opened above in the same scope.
#define PSV_PROFILE_ZONE_TEXT(text, size) ZoneText(text, size)
#define PSV_PROFILE_ZONE_VALUE(value) ZoneValue(value)
// A plot's value (`name`: a string literal).
#define PSV_PROFILE_PLOT(name, value) TracyPlot(name, value)
#else
#define PSV_PROFILE_ZONE(name) ((void)0)
#define PSV_PROFILE_ZONE_COLOR(name, rgb) ((void)0)
#define PSV_PROFILE_ZONE_TEXT(text, size) ((void)0)
#define PSV_PROFILE_ZONE_VALUE(value) ((void)0)
#define PSV_PROFILE_PLOT(name, value) ((void)0)
#endif
