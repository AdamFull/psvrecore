// The motion sensors (SceMotion): the host has none, so the console reads as held still, lying level and face up:
// gravity straight down its back (acceleration 0, 0, -1 g), no rotation, the orientation the identity (quaternion,
// rotation matrix, magnetic north matrix). Sampling has to be started first, as on the console; the game's settings
// (deadband, tilt correction, angle threshold) are kept and read back, and change nothing.

#include "hle_common.hpp"

#include <cstring>

namespace psvrecore::runtime::hle {

namespace {

constexpr u32 kSceMotion = 0xDC571B3F;
constexpr i32 kNotStarted = err::code(0x80360202);  // (SCE_MOTION_ERROR_NON_INIT_ERR)

// SceMotionState (248 bytes)
struct MotionState {
    u32 timestamp;  // (device time, microseconds)
    f32 acceleration[3], angular_velocity[3];
    u8 reserved1[12];
    f32 quaternion[4];  // x, y, z, w
    f32 rotation[16], ned[16];
    u8 reserved2[4];
    f32 basic_orientation[3];
    u64 host_timestamp;
    u8 reserved3[36];
    u8 mag_field_stability, data_info, reserved4[2];
};
static_assert(sizeof(MotionState) == 0xF8);

struct Motion {
    nx::mutex lock;
    bool sampling = false;
    i32 deadband = 1, tilt_correction = 1;
    f32 angle_threshold = 0;
};
Motion& motion() {
    static Motion m;
    return m;
}

i32 sceMotionStartSampling() {
    Motion& m = motion();
    nx::scoped_lock<nx::mutex> held(m.lock);
    m.sampling = true;
    nx::logi(logs::hle, "motion sensors: sampling (the host has none: the console still and level)");
    return 0;
}
i32 sceMotionStopSampling() {
    Motion& m = motion();
    nx::scoped_lock<nx::mutex> held(m.lock);
    m.sampling = false;
    return 0;
}

i32 sceMotionGetState(Ptr<MotionState> out) {
    if (!out || !writable(out.address, sizeof(MotionState))) return err::kFault;
    Motion& m = motion();
    {
        nx::scoped_lock<nx::mutex> held(m.lock);
        if (!m.sampling) return kNotStarted;
    }
    MotionState s{};
    const u64 now = kernel().process_time();
    s.timestamp = static_cast<u32>(now);
    s.acceleration[2] = -1.0f;
    s.quaternion[3] = 1.0f;
    for (u32 k = 0; k < 4; ++k) s.rotation[k * 5] = s.ned[k * 5] = 1.0f;
    s.basic_orientation[2] = 1.0f;  // (face up)
    s.host_timestamp = now;
    s.mag_field_stability = 0;  // (unknown: no magnetometer)
    std::memcpy(out.get(), &s, sizeof s);
    return 0;
}

// SceMotionSensorState (64 bytes): the sensors' own readings, the newest last: as still and level (gravity 1 g down
// the back, no rotation), one record per call's count, a microsecond apart.
i32 sceMotionGetSensorState(Ptr<u8> out, i32 count) {
    if (count < 1 || count > 64 || !out || !writable(out.address, 64u * static_cast<u32>(count))) return err::kFault;
    Motion& m = motion();
    {
        nx::scoped_lock<nx::mutex> held(m.lock);
        if (!m.sampling) return kNotStarted;
    }
    const u64 now = kernel().process_time();
    for (i32 i = 0; i < count; ++i) {
        u8* r = out.get() + 64 * i;
        std::memset(r, 0, 64);
        const f32 z = -1.0f;
        std::memcpy(r + 8, &z, 4);
        const u64 at = now - static_cast<u64>(count - 1 - i);
        const u32 stamp = static_cast<u32>(at), counter = static_cast<u32>(i);
        std::memcpy(r + 36, &stamp, 4);
        std::memcpy(r + 40, &counter, 4);
        std::memcpy(r + 48, &at, 8);
    }
    return 0;
}
// (no magnetometer: turning it on changes nothing, the field's stability stays unknown)
i32 sceMotionMagnetometerOn() { return 0; }

i32 sceMotionSetDeadband(i32 value) {
    Motion& m = motion();
    nx::scoped_lock<nx::mutex> held(m.lock);
    m.deadband = value;
    return 0;
}
i32 sceMotionGetDeadband() {
    Motion& m = motion();
    nx::scoped_lock<nx::mutex> held(m.lock);
    return m.deadband;
}
i32 sceMotionSetTiltCorrection(i32 value) {
    Motion& m = motion();
    nx::scoped_lock<nx::mutex> held(m.lock);
    m.tilt_correction = value;
    return 0;
}
i32 sceMotionGetTiltCorrection() {
    Motion& m = motion();
    nx::scoped_lock<nx::mutex> held(m.lock);
    return m.tilt_correction;
}
i32 sceMotionReset() { return 0; }  // (nothing has moved)

const Function kFunctions[] = {
    HLE_FUNCTION(0x28034AC9, sceMotionStartSampling),
    HLE_FUNCTION(0xAF32CB1D, sceMotionStopSampling),
    HLE_FUNCTION(0xBDB32767, sceMotionGetState),
    HLE_FUNCTION(0x917EA390, sceMotionSetDeadband),
    HLE_FUNCTION(0x112E0EAE, sceMotionGetDeadband),
    HLE_FUNCTION(0xAF09FCDB, sceMotionSetTiltCorrection),
    HLE_FUNCTION(0xC1652201, sceMotionGetTiltCorrection),
    HLE_FUNCTION(0x0FD2CDA2, sceMotionReset),
    HLE_FUNCTION(0x47D679EA, sceMotionGetSensorState),
    HLE_FUNCTION(0x122A79F8, sceMotionMagnetometerOn),
};

}  // namespace

void register_motion() {
    {
        Motion& m = motion();
        nx::scoped_lock<nx::mutex> held(m.lock);
        m.sampling = false;
    }
    register_library({"SceMotion", kSceMotion, kFunctions});
}

}  // namespace psvrecore::runtime::hle
