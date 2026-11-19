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
    bool gyro_bias_correction = true, magnetometer = false;
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
i32 sceMotionMagnetometerOn() {
    Motion& m = motion();
    nx::scoped_lock<nx::mutex> held(m.lock);
    m.magnetometer = true;
    return 0;
}

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

// The rest: the angle threshold and gyro bias correction kept and read back; the magnetometer's state (on or off, as
// set: there is none); the basic orientation (lying face up: 0, 0, 1); the sensors' physical places (not known: the
// origin); a yaw rotation accepted (the orientation stays the identity: nothing is turned).
i32 sceMotionSetAngleThreshold(f32 angle) {
    Motion& m = motion();
    nx::scoped_lock<nx::mutex> held(m.lock);
    m.angle_threshold = angle;
    return 0;
}
f32 sceMotionGetAngleThreshold() {
    Motion& m = motion();
    nx::scoped_lock<nx::mutex> held(m.lock);
    return m.angle_threshold;
}
i32 sceMotionSetGyroBiasCorrection(u32 on) {
    Motion& m = motion();
    nx::scoped_lock<nx::mutex> held(m.lock);
    m.gyro_bias_correction = on != 0;
    return 0;
}
u32 sceMotionGetGyroBiasCorrection() {
    Motion& m = motion();
    nx::scoped_lock<nx::mutex> held(m.lock);
    return m.gyro_bias_correction ? 1 : 0;
}
i32 sceMotionMagnetometerOff() {
    Motion& m = motion();
    nx::scoped_lock<nx::mutex> held(m.lock);
    m.magnetometer = false;
    return 0;
}
u32 sceMotionGetMagnetometerState() {
    Motion& m = motion();
    nx::scoped_lock<nx::mutex> held(m.lock);
    return m.magnetometer ? 1 : 0;
}
i32 sceMotionGetBasicOrientation(Ptr<f32> out) {
    if (!out || !writable(out.address, 12)) return err::code(0x80360200);  // (SCE_MOTION_ERROR_DATA_INVALID)
    out.get()[0] = 0.0f, out.get()[1] = 0.0f, out.get()[2] = 1.0f;
    return 0;
}
// SceMotionDeviceLocation: the accelerometer's and the gyro's positions (3 floats each), reserved.
i32 sceMotionGetDeviceLocation(Ptr<u8> out) {
    if (!out || !writable(out.address, 48)) return err::code(0x80360200);
    std::memset(out.get(), 0, 48);
    return 0;
}
i32 sceMotionRotateYaw(f32) { return 0; }

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
    HLE_FUNCTION(0xDACB2A41, sceMotionSetAngleThreshold),
    HLE_FUNCTION(0x499B6C87, sceMotionGetAngleThreshold),
    HLE_FUNCTION(0xA4A7E6E2, sceMotionSetGyroBiasCorrection),
    HLE_FUNCTION(0xE8A28DA5, sceMotionGetGyroBiasCorrection),
    HLE_FUNCTION(0xC1A7395A, sceMotionMagnetometerOff),
    HLE_FUNCTION(0x3D4813AE, sceMotionGetMagnetometerState),
    HLE_FUNCTION(0x4F28BFE0, sceMotionGetBasicOrientation),
    HLE_FUNCTION(0x047F060A, sceMotionGetDeviceLocation),
    HLE_FUNCTION(0x20F00078, sceMotionRotateYaw),
};

}  // namespace

void register_motion() {
    {
        Motion& m = motion();
        nx::scoped_lock<nx::mutex> held(m.lock);
        m.sampling = false;
    }
    register_library({"SceMotion", kSceMotion, kFunctions, {}, true});
}

}  // namespace psvrecore::runtime::hle
