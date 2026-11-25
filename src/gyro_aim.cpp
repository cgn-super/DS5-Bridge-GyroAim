//
// DS5-Bridge Gyro-Aim v2.1 — Industry-Standard JSM / Motion Studio / GamepadMotion Pipeline
// Target: Raspberry Pi Pico 2 W (RP2350 Cortex-M33 Single-Precision FPU @ 250 MHz)
//
// Pure Port of Industry-Proven Algorithms:
//   - GamepadMotionHelpers::SensorMinMaxWindow (0.5s peak-to-peak stillness auto-calibration)
//   - GamepadMotionHelpers::CalculatePlayerSpaceGyro (100% local Pitch + Gravity-projected Yaw with 1.41f relax)
//   - Motion Studio / FlexInput OneEuro Filter + Decay::smooth (zero filter-memory overhang on fast stop)
//   - Motion Studio / JSM cutoff_factor + Two-Segment Linear Sensitivity Ramp
//   - Motion Studio / JSM pad::virtual_stick Rate-Domain Mixer (combines physical stick & gyro in deg/s)
//   - Motion Studio Layer Modeshift (Base Combat Layer vs. Hold-Circle / Toggle-Underbarrel Grenade Layer)
//

#include "gyro_aim.h"

#include <cmath>
#include <cstddef>
#include <cstdint>
#include <unordered_map>
#include <vector>

#include "bt.h"
#include "pico.h"
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wsign-compare"
#include "utils.h"
#pragma GCC diagnostic pop

// Reference to bt.cpp's cached feature report table (inspected by const-ref, zero allocation)
extern std::unordered_map<uint8_t, std::vector<uint8_t>> feature_data;

// ---------------------------------------------------------------------------
// Global Tunables Exposed by Specification
// ---------------------------------------------------------------------------

float user_sens_scale = 1.0f;
bool gyro_bypass_debug = false;

// ---------------------------------------------------------------------------
// Internal Deterministic State (Zero Heap Allocation)
// ---------------------------------------------------------------------------

namespace {

enum class ActiveLayer : uint8_t {
    kNone    = 0,
    kCombat  = 1,
    kGrenade = 2
};

static ActivationMode active_mode = ActivationMode::kCombatAndGrenade;
static ActiveLayer active_layer = ActiveLayer::kNone;

// 1. Factory Calibration (Feature Report 0x05, fallback: 1/16 deg/s/LSB, 1/8192 g/LSB)
static bool factory_cal_loaded = false;
static uint8_t factory_cal_probe_div = 0;
static float cal_gyro_scale_x = 1.0f / 16.0f; // Pitch (°/s per LSB)
static float cal_gyro_scale_y = 1.0f / 16.0f; // Yaw   (°/s per LSB)
static float cal_gyro_scale_z = 1.0f / 16.0f; // Roll  (°/s per LSB)
static float cal_accel_scale_x = 1.0f / 8192.0f;
static float cal_accel_scale_y = 1.0f / 8192.0f;
static float cal_accel_scale_z = 1.0f / 8192.0f;
static float cal_accel_bias_x = 0.0f;
static float cal_accel_bias_y = 0.0f;
static float cal_accel_bias_z = 0.0f;

// 2. GamepadMotionHelpers::SensorMinMaxWindow (0.5s Peak-to-Peak Stillness AutoCalibration)
static bool bias_seeded = false;
static float bias_gx_dps = 0.0f;
static float bias_gy_dps = 0.0f;
static float bias_gz_dps = 0.0f;

struct SensorMinMaxWindow {
    float min_gx, max_gx, mean_gx;
    float min_gy, max_gy, mean_gy;
    float min_gz, max_gz, mean_gz;
    float min_ax, max_ax;
    float min_ay, max_ay;
    float min_az, max_az;
    uint32_t num_samples;
    float time_sampled;

    void reset() {
        num_samples = 0;
        time_sampled = 0.0f;
    }

    void add_sample(float gx, float gy, float gz, float ax, float ay, float az, float dt) {
        if (num_samples == 0) {
            min_gx = max_gx = mean_gx = gx;
            min_gy = max_gy = mean_gy = gy;
            min_gz = max_gz = mean_gz = gz;
            min_ax = max_ax = ax;
            min_ay = max_ay = ay;
            min_az = max_az = az;
            num_samples = 1;
            time_sampled = dt;
            return;
        }
        if (gx < min_gx) min_gx = gx;
        if (gx > max_gx) max_gx = gx;
        if (gy < min_gy) min_gy = gy;
        if (gy > max_gy) max_gy = gy;
        if (gz < min_gz) min_gz = gz;
        if (gz > max_gz) max_gz = gz;

        if (ax < min_ax) min_ax = ax;
        if (ax > max_ax) max_ax = ax;
        if (ay < min_ay) min_ay = ay;
        if (ay > max_ay) max_ay = ay;
        if (az < min_az) min_az = az;
        if (az > max_az) max_az = az;

        ++num_samples;
        time_sampled += dt;
        const float inv_n = 1.0f / (float)num_samples;
        mean_gx += (gx - mean_gx) * inv_n;
        mean_gy += (gy - mean_gy) * inv_n;
        mean_gz += (gz - mean_gz) * inv_n;
    }
};

static SensorMinMaxWindow still_win{};

// 3. Hardware SensorTimestamp (3 MHz, 1/3 us per tick)
static bool ts_seeded = false;
static uint32_t prev_sensor_ts = 0;

// 4. Gravity Direction Vector in Controller Body Frame (+Y is up when flat on desk)
static float grav_x = 0.0f;
static float grav_y = 1.0f;
static float grav_z = 0.0f;

// 5. Motion Studio / FlexInput cc::OneEuro & cc::Decay Smoothers (cc.rs:171-261)
struct LowPass1P {
    float prev = 0.0f;
    bool started = false;

    void reset() {
        prev = 0.0f;
        started = false;
    }

    float filter(float x, float a) {
        if (!started) {
            prev = x;
            started = true;
            return x;
        }
        prev = a * x + (1.0f - a) * prev;
        return prev;
    }
};

struct OneEuroFilter {
    LowPass1P val{};
    LowPass1P deriv{};
    float prev_x = 0.0f;
    bool started = false;

    void reset() {
        val.reset();
        deriv.reset();
        prev_x = 0.0f;
        started = false;
    }

    static inline float alpha_from_cutoff(float cutoff_hz, float dt) {
        constexpr float kTau = 6.283185307f;
        const float c = cutoff_hz > 1e-4f ? cutoff_hz : 1e-4f;
        const float safe_dt = dt > 1e-5f ? dt : 1e-5f;
        const float tau = 1.0f / (kTau * c);
        return 1.0f / (1.0f + tau / safe_dt);
    }

    float filter(float x, float dt, float min_cutoff_hz, float beta) {
        const float safe_dt = dt > 1e-5f ? dt : 1e-5f;
        const float dx = started ? ((x - prev_x) / safe_dt) : 0.0f;
        prev_x = x;
        started = true;
        // D_CUTOFF = 1.0 Hz in Motion Studio cc.rs:198
        const float edx = deriv.filter(dx, alpha_from_cutoff(1.0f, safe_dt));
        const float cutoff = min_cutoff_hz + beta * fabsf(edx);
        return val.filter(x, alpha_from_cutoff(cutoff, safe_dt));
    }
};

// Motion Studio cc::Decay (cc.rs:224-261):
// Shrinks both the smoothed share AND the effective time constant to zero as speed approaches threshold.
// Guarantees ZERO filter-memory overhang when stopping a fast flick!
struct DecaySmoother {
    float sx = 0.0f;
    float sy = 0.0f;
    bool started = false;

    void reset() {
        sx = 0.0f;
        sy = 0.0f;
        started = false;
    }

    void smooth(float in_x, float in_y, float dt, float smooth_time_s, float threshold_dps,
                float &out_x, float &out_y) {
        if (smooth_time_s <= 0.0f || threshold_dps <= 0.0f) {
            started = false;
            out_x = in_x;
            out_y = in_y;
            return;
        }
        const float speed = sqrtf(in_x * in_x + in_y * in_y);
        float fast = speed / threshold_dps;
        if (fast > 1.0f) fast = 1.0f;
        const float one_minus_fast = 1.0f - fast;
        const float smoothed_share = one_minus_fast * one_minus_fast;
        const float immediate_share = 1.0f - smoothed_share;

        const float target_x = in_x * smoothed_share;
        const float target_y = in_y * smoothed_share;
        if (!started) {
            sx = target_x;
            sy = target_y;
            started = true;
        }
        const float effective_tau = smooth_time_s * smoothed_share;
        const float a = (effective_tau <= 1e-5f) ? 1.0f : (1.0f - expf(-dt / effective_tau));
        sx += a * (target_x - sx);
        sy += a * (target_y - sy);

        out_x = sx + in_x * immediate_share;
        out_y = sy + in_y * immediate_share;
    }
};

static OneEuroFilter one_euro_x{};
static OneEuroFilter one_euro_y{};
static DecaySmoother decay_smoother{};

// Retained desired camera turn rate in deg/s (held across duplicate 500Hz USB polls between BT packets)
static float retained_gyro_x_dps = 0.0f;
static float retained_gyro_y_dps = 0.0f;

// Ergonomic State Machine Timers & Latches
static bool combat_trig_engaged = false;
static bool underbarrel_gl_active = false;
static bool prev_touch_right_active = false;
static float r3_pulse_hold_ms = 0.0f;
static float shortcut_mute_ms = 0.0f;
static float circle_hold_ms = 0.0f;
static float grenade_tail_ms = 0.0f;
static uint8_t prev_shortcut_dpad = 8; // Direction::None

// Lightbar & L2 Adaptive Trigger Wall Management
static uint8_t saved_led_r = 0xFF;
static uint8_t saved_led_g = 0xD7;
static uint8_t saved_led_b = 0x00;
static bool game_has_custom_l2 = false;
static bool wall_armed_on_boot = false;
static float mode_led_flash_ms = 0.0f;
static uint8_t flash_led_r = 0;
static uint8_t flash_led_g = 0;
static uint8_t flash_led_b = 0;

// Host Auto-Boost & Memory Clean Hook state (signals PC daemon via unused UNK2 byte 0xA5..0xAF)
static uint16_t boost_hook_frames = 0;
static uint8_t boost_hook_seq = 0x05;
static bool boost_combo_latch = false;
static bool boost_led_active = false;

// ---------------------------------------------------------------------------
// Single-Precision Math Helpers
// ---------------------------------------------------------------------------

static inline float clampf(float v, float lo, float hi) {
    return v < lo ? lo : (v > hi ? hi : v);
}

static inline float maxf(float a, float b) {
    return a > b ? a : b;
}

static inline float minf(float a, float b) {
    return a < b ? a : b;
}

static inline float smoothstepf(float edge0, float edge1, float x) {
    const float denom = edge1 - edge0;
    if (denom <= 1e-6f) return x >= edge1 ? 1.0f : 0.0f;
    const float t = clampf((x - edge0) / denom, 0.0f, 1.0f);
    return t * t * (3.0f - 2.0f * t);
}

static inline int16_t read_le_i16(const uint8_t *p) {
    return (int16_t)((uint16_t)p[0] | ((uint16_t)p[1] << 8));
}

static inline void reset_filter_state() {
    one_euro_x.reset();
    one_euro_y.reset();
    decay_smoother.reset();
    retained_gyro_x_dps = 0.0f;
    retained_gyro_y_dps = 0.0f;
}

// ---------------------------------------------------------------------------
// L2 Adaptive Trigger Wall & Lightbar Output Helpers
// ---------------------------------------------------------------------------

static void apply_l2_trigger_policy(SetStateData &state) {
    if (game_has_custom_l2) {
        return;
    }
    state.AllowLeftTriggerFFB = 1;
    if (active_mode == ActivationMode::kClassicTwoStage) {
        state.LeftTriggerFFB[0] = 0x02; // Mode 0x02: Section Resistance ("卡手"二段阻尼墙)
        state.LeftTriggerFFB[1] = GA_WALL_START;
        state.LeftTriggerFFB[2] = GA_WALL_END;
        state.LeftTriggerFFB[3] = GA_WALL_FORCE;
    } else {
        state.LeftTriggerFFB[0] = 0x05; // Mode 0x05: Release / No Resistance Wall
        state.LeftTriggerFFB[1] = 0;
        state.LeftTriggerFFB[2] = 0;
        state.LeftTriggerFFB[3] = 0;
    }
}

static void send_gyro_led_and_wall(uint8_t r, uint8_t g, uint8_t b) {
    SetStateData state{};
    state.AllowLedColor = 1;
    state.LedRed = r;
    state.LedGreen = g;
    state.LedBlue = b;
    apply_l2_trigger_policy(state);
    update_state(state);
}

static void get_active_target_led(uint8_t &r, uint8_t &g, uint8_t &b) {
    if (boost_led_active) {
        r = 0; g = 240; b = 255; // Ice Cyan for MSI Boost
        return;
    }
    if (mode_led_flash_ms > 0.0f) {
        r = flash_led_r; g = flash_led_g; b = flash_led_b;
        return;
    }
    if (active_layer == ActiveLayer::kGrenade) {
        r = 255; g = 110; b = 0; // Tactical Orange for Grenade / Underbarrel GL Layer
        return;
    }
    if (active_layer == ActiveLayer::kCombat &&
        active_mode == ActivationMode::kClassicTwoStage) {
        r = 255; g = 20; b = 40; // Classic Crimson Red on Two-Stage wall break
        return;
    }
    r = saved_led_r;
    g = saved_led_g;
    b = saved_led_b;
}

// ---------------------------------------------------------------------------
// Zero-Allocation Feature Report 0x05 Factory Calibration Loader
// ---------------------------------------------------------------------------

static void try_load_factory_calibration() {
    if (factory_cal_loaded) return;
    if (++factory_cal_probe_div != 1) return;

    const auto it = feature_data.find(0x05);
    if (it == feature_data.end() || it->second.size() < 34) {
        return;
    }

    const uint8_t *buf = it->second.data();
    const int16_t pitch_plus  = read_le_i16(buf + 6);
    const int16_t pitch_minus = read_le_i16(buf + 8);
    const int16_t yaw_plus    = read_le_i16(buf + 10);
    const int16_t yaw_minus   = read_le_i16(buf + 12);
    const int16_t roll_plus   = read_le_i16(buf + 14);
    const int16_t roll_minus  = read_le_i16(buf + 16);
    const int16_t speed_plus  = read_le_i16(buf + 18);
    const int16_t speed_minus = read_le_i16(buf + 20);

    const int16_t ax_plus  = read_le_i16(buf + 22);
    const int16_t ax_minus = read_le_i16(buf + 24);
    const int16_t ay_plus  = read_le_i16(buf + 26);
    const int16_t ay_minus = read_le_i16(buf + 28);
    const int16_t az_plus  = read_le_i16(buf + 30);
    const int16_t az_minus = read_le_i16(buf + 32);

    const float speed_2x = (float)((int32_t)speed_plus + (int32_t)speed_minus);
    const int32_t dp = (int32_t)pitch_plus - (int32_t)pitch_minus;
    const int32_t dy = (int32_t)yaw_plus   - (int32_t)yaw_minus;
    const int32_t dr = (int32_t)roll_plus  - (int32_t)roll_minus;

    if (speed_2x > 100.0f && dp > 1000 && dy > 1000 && dr > 1000) {
        const float sx = speed_2x / (float)dp;
        const float sy = speed_2x / (float)dy;
        const float sz = speed_2x / (float)dr;
        if (sx >= 0.03f && sx <= 0.12f) cal_gyro_scale_x = sx;
        if (sy >= 0.03f && sy <= 0.12f) cal_gyro_scale_y = sy;
        if (sz >= 0.03f && sz <= 0.12f) cal_gyro_scale_z = sz;
    }

    const int32_t dax = (int32_t)ax_plus - (int32_t)ax_minus;
    const int32_t day = (int32_t)ay_plus - (int32_t)ay_minus;
    const int32_t daz = (int32_t)az_plus - (int32_t)az_minus;
    if (dax > 4000 && day > 4000 && daz > 4000) {
        cal_accel_bias_x = 0.5f * (float)((int32_t)ax_plus + (int32_t)ax_minus);
        cal_accel_bias_y = 0.5f * (float)((int32_t)ay_plus + (int32_t)ay_minus);
        cal_accel_bias_z = 0.5f * (float)((int32_t)az_plus + (int32_t)az_minus);
        const float asx = 2.0f / (float)dax;
        const float asy = 2.0f / (float)day;
        const float asz = 2.0f / (float)daz;
        if (asx >= (1.0f / 16384.0f) && asx <= (1.0f / 4096.0f)) cal_accel_scale_x = asx;
        if (asy >= (1.0f / 16384.0f) && asy <= (1.0f / 4096.0f)) cal_accel_scale_y = asy;
        if (asz >= (1.0f / 16384.0f) && asz <= (1.0f / 4096.0f)) cal_accel_scale_z = asz;
    }

    factory_cal_loaded = true;
}

} // namespace

// ---------------------------------------------------------------------------
// Public Hooks
// ---------------------------------------------------------------------------

void __not_in_flash_func(host_boost_hook_apply)(USBGetStateData &r) {
    const bool combo_now = (r.ButtonL3 && r.ButtonR3 && r.ButtonL1 && r.ButtonR1);
    if (combo_now) {
        r.ButtonL3 = 0;
        r.ButtonR3 = 0;
        r.ButtonL1 = 0;
        r.ButtonR1 = 0;
        if (!boost_combo_latch) {
            boost_combo_latch = true;
            boost_hook_seq = (uint8_t)(((boost_hook_seq + 1) & 0x0F) | 0x01);
            boost_hook_frames = 300;
            boost_led_active = true;
            send_gyro_led_and_wall(0, 240, 255);
        }
    } else {
        boost_combo_latch = false;
    }

    if (boost_hook_frames > 0) {
        r.UNK2 = (uint8_t)(0xA0 | (boost_hook_seq & 0x0F));
        --boost_hook_frames;
        if (boost_hook_frames == 0 && boost_led_active) {
            boost_led_active = false;
            uint8_t lr, lg, lb;
            get_active_target_led(lr, lg, lb);
            send_gyro_led_and_wall(lr, lg, lb);
        }
    }
}

void gyro_aim_on_host_output_report(SetStateData &state) {
    if (state.AllowLedColor) {
        saved_led_r = state.LedRed;
        saved_led_g = state.LedGreen;
        saved_led_b = state.LedBlue;
        uint8_t lr, lg, lb;
        get_active_target_led(lr, lg, lb);
        state.LedRed = lr;
        state.LedGreen = lg;
        state.LedBlue = lb;
    }
    if (state.AllowLeftTriggerFFB) {
        const uint8_t mode = state.LeftTriggerFFB[0];
        if (mode == 0x00 || mode == 0x05) {
            game_has_custom_l2 = false;
            apply_l2_trigger_policy(state);
        } else {
            game_has_custom_l2 = true;
        }
    }
}

void __not_in_flash_func(gyro_aim_apply)(USBGetStateData &r) {
    // ---- 1. Boot Link Initialization & MSI Auto-Boost Hook ----
    if (!wall_armed_on_boot && r.SensorTimestamp > 0) {
        wall_armed_on_boot = true;
        boost_hook_frames = 500;
        send_gyro_led_and_wall(saved_led_r, saved_led_g, saved_led_b);
    }

    try_load_factory_calibration();
    host_boost_hook_apply(r);

    // ---- 2. Hardware Timestamp Delta-t (3 MHz Clock, 1/3 us per tick) ----
    bool is_new_imu_sample = false;
    float dt_sec = (float)DS5_USB_POLL_INTERVAL_MS * 0.001f;

    if (!ts_seeded && r.SensorTimestamp > 0) {
        prev_sensor_ts = r.SensorTimestamp;
        ts_seeded = true;
        is_new_imu_sample = true;
    } else if (ts_seeded && r.SensorTimestamp != prev_sensor_ts) {
        const uint32_t delta_ticks = r.SensorTimestamp - prev_sensor_ts;
        prev_sensor_ts = r.SensorTimestamp;
        const float raw_dt = (float)delta_ticks * (1.0f / 3000000.0f);
        dt_sec = clampf(raw_dt, 0.0005f, 0.02f);
        is_new_imu_sample = true;
    }

    const float frame_dt_ms = (float)DS5_USB_POLL_INTERVAL_MS;

    if (mode_led_flash_ms > 0.0f) {
        mode_led_flash_ms -= frame_dt_ms;
        if (mode_led_flash_ms <= 0.0f) {
            mode_led_flash_ms = 0.0f;
            uint8_t lr, lg, lb;
            get_active_target_led(lr, lg, lb);
            send_gyro_led_and_wall(lr, lg, lb);
        }
    }
    if (shortcut_mute_ms > 0.0f) {
        shortcut_mute_ms = maxf(0.0f, shortcut_mute_ms - frame_dt_ms);
    }
    if (r3_pulse_hold_ms > 0.0f) {
        r3_pulse_hold_ms = maxf(0.0f, r3_pulse_hold_ms - frame_dt_ms);
    }

    // ---- 3. On-the-Fly Shortcut Combos: Create(Share) + D-Pad ----
    if (r.ButtonCreate) {
        const uint8_t dpad_now = (uint8_t)r.DPad;
        if (dpad_now != (uint8_t)Direction::None && prev_shortcut_dpad == (uint8_t)Direction::None) {
            bool triggered = false;
            if (dpad_now == (uint8_t)Direction::North || dpad_now == (uint8_t)Direction::South) {
                const uint8_t cur = (uint8_t)active_mode;
                const uint8_t next = (dpad_now == (uint8_t)Direction::North)
                                         ? (uint8_t)((cur + 1) & 0x03)
                                         : (uint8_t)((cur + 3) & 0x03);
                active_mode = (ActivationMode)next;
                triggered = true;
                mode_led_flash_ms = 300.0f;
                switch (active_mode) {
                    case ActivationMode::kCombatAndGrenade:
                        flash_led_r = 0; flash_led_g = 230; flash_led_b = 100;
                        break;
                    case ActivationMode::kSoftL2Only:
                        flash_led_r = 0; flash_led_g = 120; flash_led_b = 255;
                        break;
                    case ActivationMode::kAlwaysOnSmartStick:
                        flash_led_r = 255; flash_led_g = 175; flash_led_b = 0;
                        break;
                    case ActivationMode::kClassicTwoStage:
                        flash_led_r = 255; flash_led_g = 20; flash_led_b = 40;
                        break;
                }
                send_gyro_led_and_wall(flash_led_r, flash_led_g, flash_led_b);
            } else if (dpad_now == (uint8_t)Direction::East || dpad_now == (uint8_t)Direction::West) {
                const float step = (dpad_now == (uint8_t)Direction::East) ? 0.10f : -0.10f;
                user_sens_scale = clampf(user_sens_scale + step, 0.50f, 2.00f);
                triggered = true;
                mode_led_flash_ms = 220.0f;
                flash_led_r = 255; flash_led_g = 255; flash_led_b = 255;
                send_gyro_led_and_wall(flash_led_r, flash_led_g, flash_led_b);
            }
            if (triggered) {
                shortcut_mute_ms = 80.0f;
                reset_filter_state();
            }
        }
        prev_shortcut_dpad = dpad_now;
        if (dpad_now != (uint8_t)Direction::None) {
            r.ButtonCreate = 0;
            r.DPad = Direction::None;
        }
    } else {
        prev_shortcut_dpad = (uint8_t)Direction::None;
    }

    // ---- 4. Right-Half Capacitive Touchpad -> R3 & Contextual Underbarrel GL Layer ----
    const bool touch0_active = (r.Touch.Finger[0].NotTouching == 0);
    const uint16_t touch0_x = (uint16_t)r.Touch.Finger[0].FingerX;
    const bool touch_right_active = touch0_active && (touch0_x >= 960);

    if (touch_right_active && !prev_touch_right_active) {
        r3_pulse_hold_ms = 50.0f;
        // Motion Studio LayerMode::Toggle with exit_input = L2 release
        if (r.TriggerLeft >= GA_SOFT_TRIG_ON) {
            underbarrel_gl_active = !underbarrel_gl_active;
        }
    }
    prev_touch_right_active = touch_right_active;

    if (r.TriggerLeft < GA_SOFT_TRIG_OFF) {
        underbarrel_gl_active = false;
    }

    if (touch_right_active || r3_pulse_hold_ms > 0.0f) {
        r.ButtonR3 = 1;
    }

    // ---- 5. Motion Studio Layer Arbitration (Base Combat vs. Grenade Layer) ----
    if (r.ButtonCircle) {
        circle_hold_ms += frame_dt_ms;
        if (circle_hold_ms >= 80.0f) {
            grenade_tail_ms = 150.0f;
        }
    } else {
        circle_hold_ms = 0.0f;
        if (grenade_tail_ms > 0.0f) {
            grenade_tail_ms = maxf(0.0f, grenade_tail_ms - frame_dt_ms);
        }
    }
    const bool grenade_layer_on = (circle_hold_ms >= 80.0f) ||
                                  (grenade_tail_ms > 0.0f) ||
                                  (underbarrel_gl_active && r.TriggerLeft >= GA_SOFT_TRIG_OFF);

    switch (active_mode) {
        case ActivationMode::kCombatAndGrenade:
            if (!combat_trig_engaged && (r.TriggerLeft >= GA_SOFT_TRIG_ON || r.TriggerRight >= GA_SOFT_TRIG_ON)) {
                combat_trig_engaged = true;
            } else if (combat_trig_engaged && (r.TriggerLeft < GA_SOFT_TRIG_OFF && r.TriggerRight < GA_SOFT_TRIG_OFF)) {
                combat_trig_engaged = false;
            }
            break;
        case ActivationMode::kSoftL2Only:
            if (!combat_trig_engaged && r.TriggerLeft >= GA_SOFT_TRIG_ON) {
                combat_trig_engaged = true;
            } else if (combat_trig_engaged && r.TriggerLeft < GA_SOFT_TRIG_OFF) {
                combat_trig_engaged = false;
            }
            break;
        case ActivationMode::kAlwaysOnSmartStick:
            combat_trig_engaged = true;
            break;
        case ActivationMode::kClassicTwoStage:
            if (!combat_trig_engaged && r.TriggerLeft >= GA_L2_ON) {
                combat_trig_engaged = true;
            } else if (combat_trig_engaged && r.TriggerLeft < GA_L2_OFF) {
                combat_trig_engaged = false;
            }
            break;
    }

    ActiveLayer next_layer = ActiveLayer::kNone;
    if (grenade_layer_on) {
        next_layer = ActiveLayer::kGrenade;
    } else if (combat_trig_engaged) {
        next_layer = ActiveLayer::kCombat;
    }

    if (next_layer != active_layer) {
        if (next_layer == ActiveLayer::kNone) {
            // Exact JSM aim.rs:691 block_sampled_gyro behavior: reset filter when gyro turns off
            reset_filter_state();
        }
        active_layer = next_layer;
        if (mode_led_flash_ms <= 0.0f && !boost_led_active) {
            uint8_t lr, lg, lb;
            get_active_target_led(lr, lg, lb);
            send_gyro_led_and_wall(lr, lg, lb);
        }
    }

    // ---- 6. GamepadMotionHelpers IMU Processing & Player Space Projection ----
    // DualSense Y-up right-hand coordinate system (X = Pitch right, Y = Yaw up, Z = Roll back):
    const float raw_gx_dps = (float)r.AngularVelocityX * cal_gyro_scale_x;
    const float raw_gy_dps = (float)r.AngularVelocityZ * cal_gyro_scale_y;
    const float raw_gz_dps = (float)r.AngularVelocityY * cal_gyro_scale_z;

    const float ax_g = ((float)r.AccelerometerX - cal_accel_bias_x) * cal_accel_scale_x;
    const float ay_g = ((float)r.AccelerometerY - cal_accel_bias_y) * cal_accel_scale_y;
    const float az_g = ((float)r.AccelerometerZ - cal_accel_bias_z) * cal_accel_scale_z;

    if (!bias_seeded && r.SensorTimestamp > 0) {
        bias_gx_dps = raw_gx_dps;
        bias_gy_dps = raw_gy_dps;
        bias_gz_dps = raw_gz_dps;
        still_win.reset();
        bias_seeded = true;
    }

    if (is_new_imu_sample) {
        // 6.1 GamepadMotionHelpers::SensorMinMaxWindow Stillness Auto-Calibration (GamepadMotion.hpp:670)
        still_win.add_sample(raw_gx_dps, raw_gy_dps, raw_gz_dps, ax_g, ay_g, az_g, dt_sec);
        const float span_gx = still_win.max_gx - still_win.min_gx;
        const float span_gy = still_win.max_gy - still_win.min_gy;
        const float span_gz = still_win.max_gz - still_win.min_gz;
        const float span_ax = still_win.max_ax - still_win.min_ax;
        const float span_ay = still_win.max_ay - still_win.min_ay;
        const float span_az = still_win.max_az - still_win.min_az;

        if (span_gx > 2.2f || span_gy > 2.2f || span_gz > 2.2f ||
            span_ax > 0.06f || span_ay > 0.06f || span_az > 0.06f) {
            still_win.reset();
        } else if (still_win.time_sampled >= 0.50f && still_win.num_samples >= 20) {
            // Smoothly blend bias toward window mean and start next window
            bias_gx_dps = 0.70f * bias_gx_dps + 0.30f * still_win.mean_gx;
            bias_gy_dps = 0.70f * bias_gy_dps + 0.30f * still_win.mean_gy;
            bias_gz_dps = 0.70f * bias_gz_dps + 0.30f * still_win.mean_gz;
            still_win.reset();
        }

        const float wx_dps = raw_gx_dps - bias_gx_dps;
        const float wy_dps = raw_gy_dps - bias_gy_dps;
        const float wz_dps = raw_gz_dps - bias_gz_dps;

        // 6.2 Gravity Direction Tracking (GamepadMotion.hpp:599 & motion.rs:135)
        constexpr float kDegToRad = 0.01745329252f;
        const float wx_rad = wx_dps * kDegToRad;
        const float wy_rad = wy_dps * kDegToRad;
        const float wz_rad = wz_dps * kDegToRad;

        float gx_pred = grav_x + (grav_y * wz_rad - grav_z * wy_rad) * dt_sec;
        float gy_pred = grav_y + (grav_z * wx_rad - grav_x * wz_rad) * dt_sec;
        float gz_pred = grav_z + (grav_x * wy_rad - grav_y * wx_rad) * dt_sec;

        const float a_norm = sqrtf(ax_g * ax_g + ay_g * ay_g + az_g * az_g);
        if (a_norm >= 0.82f && a_norm <= 1.18f) {
            const float inv_a = 1.0f / a_norm;
            // GRAVITY_TAU = 0.5s from Motion Studio motion.rs:55
            const float alpha_g = 1.0f - expf(-dt_sec * 2.0f);
            gx_pred += alpha_g * (ax_g * inv_a - gx_pred);
            gy_pred += alpha_g * (ay_g * inv_a - gy_pred);
            gz_pred += alpha_g * (az_g * inv_a - gz_pred);
        }

        const float g_norm = sqrtf(gx_pred * gx_pred + gy_pred * gy_pred + gz_pred * gz_pred);
        if (g_norm > 1e-4f) {
            const float inv_g = 1.0f / g_norm;
            grav_x = gx_pred * inv_g;
            grav_y = gy_pred * inv_g;
            grav_z = gz_pred * inv_g;
        }

        // 6.3 GamepadMotion::CalculatePlayerSpaceGyro (GamepadMotion.hpp:1169-1176) + XY-CtoPS5 Roll Mix
        if (gyro_bypass_debug || active_layer == ActiveLayer::kNone || shortcut_mute_ms > 0.0f) {
            reset_filter_state();
        } else {
            const GyroProfile &prof = (active_layer == ActiveLayer::kGrenade)
                                          ? kGrenadeProfile
                                          : kCombatProfile;

            // Pitch is kept 100% on the controller's local X axis (zero gravity cross-axis bleed!)
            const float local_pitch_dps = -wx_dps;

            // Yaw uses Jibb Smart's Player Space gravity projection with yawRelaxFactor = 1.41f
            // plus optional XY-CtoPS5 steering-wheel roll mix:
            const float world_yaw = grav_y * wy_dps + grav_z * wz_dps;
            const float off_pitch_mag = sqrtf(wy_dps * wy_dps + wz_dps * wz_dps);
            const float yaw_sign = (world_yaw >= 0.0f) ? 1.0f : -1.0f;
            const float player_yaw = yaw_sign * minf(fabsf(world_yaw) * 1.41f, off_pitch_mag);
            const float turn_yaw_dps = -player_yaw - (wz_dps * prof.roll_mix);

            // 6.4 Motion Studio One-Euro Filter + Decay Smoother (cc.rs:205, 231)
            const float euro_x = one_euro_x.filter(turn_yaw_dps, dt_sec, prof.one_euro_min_cutoff, prof.one_euro_beta);
            const float euro_y = one_euro_y.filter(local_pitch_dps, dt_sec, prof.one_euro_min_cutoff, prof.one_euro_beta);

            float gx_dps = euro_x;
            float gy_dps = euro_y;
            decay_smoother.smooth(euro_x, euro_y, dt_sec, prof.smooth_time_sec, prof.smooth_threshold_dps,
                                  gx_dps, gy_dps);

            // 6.5 Motion Studio / JSM cutoff_factor (aim.rs:113-121)
            const float speed_dps = sqrtf(gx_dps * gx_dps + gy_dps * gy_dps);
            float cutoff_fac = 1.0f;
            if (prof.cutoff_recovery_dps > prof.cutoff_speed_dps) {
                cutoff_fac = clampf((speed_dps - prof.cutoff_speed_dps) /
                                        (prof.cutoff_recovery_dps - prof.cutoff_speed_dps),
                                    0.0f, 1.0f);
            } else if (prof.cutoff_speed_dps > 0.0f && speed_dps < prof.cutoff_speed_dps) {
                cutoff_fac = 0.0f;
            }
            gx_dps *= cutoff_fac;
            gy_dps *= cutoff_fac;

            // 6.6 Motion Studio / XY-CtoPS5 Two-Segment Linear Sensitivity Ramp (aim.rs:733 & mapping.py:195)
            // Pure 1:1 linear in micro-aim zone (<= min_threshold_dps), smooth linear ramp to max_sens
            const float post_cutoff_speed = speed_dps * cutoff_fac;
            const float span_thresh = maxf(prof.max_threshold_dps - prof.min_threshold_dps, 1.0f);
            const float ramp_t = clampf((post_cutoff_speed - prof.min_threshold_dps) / span_thresh, 0.0f, 1.0f);
            const float sens_gain = (prof.min_sens + (prof.max_sens - prof.min_sens) * ramp_t) * user_sens_scale;

            retained_gyro_x_dps = gx_dps * sens_gain;
            retained_gyro_y_dps = gy_dps * sens_gain * prof.yx_ratio;
        }
    } else if (gyro_bypass_debug || active_layer == ActiveLayer::kNone || shortcut_mute_ms > 0.0f) {
        retained_gyro_x_dps = 0.0f;
        retained_gyro_y_dps = 0.0f;
    }

    // ---- 7. Motion Studio pad::virtual_stick Rate-Domain Mixer (pad.rs:294-342) ----
    float gyro_x_dps = retained_gyro_x_dps;
    float gyro_y_dps = retained_gyro_y_dps;

    // Physical Right Stick in [-1.0 .. +1.0]
    const float push_x = ((float)r.RightStickX - 128.0f) * (1.0f / 127.0f);
    const float push_y = ((float)r.RightStickY - 128.0f) * (1.0f / 127.0f);
    const float push_len = sqrtf(push_x * push_x + push_y * push_y);

    // Mode 3 (kAlwaysOnSmartStick) Smart Stick Clutch: suppress gyro when right stick pushed > 35%
    if (active_mode == ActivationMode::kAlwaysOnSmartStick) {
        if (push_len > 0.35f) {
            gyro_x_dps = 0.0f;
            gyro_y_dps = 0.0f;
        } else if (push_len > 0.22f) {
            const float clutch = 1.0f - smoothstepf(0.22f, 0.35f, push_len);
            gyro_x_dps *= clutch;
            gyro_y_dps *= clutch;
        }
    }

    // When gyro is completely silent, pass the physical right stick through 100% untouched!
    if (fabsf(gyro_x_dps) <= 1e-4f && fabsf(gyro_y_dps) <= 1e-4f) {
        return;
    }

    const float livezone = 1.0f - kGameStickCal.undeadzone_outer - kGameStickCal.undeadzone_inner;
    if (livezone <= 0.05f || kGameStickCal.calibration_dps <= 1.0f) {
        return;
    }

    // 7.1 Convert physical right-stick push into in-game camera angular velocity (deg/s)
    float want_x_dps = 0.0f;
    float want_y_dps = 0.0f;
    if (push_len > kGameStickCal.undeadzone_inner) {
        const float norm_push = clampf((push_len - kGameStickCal.undeadzone_inner) / livezone, 0.0f, 1.0f);
        const float fraction = powf(norm_push, kGameStickCal.unpower);
        const float stick_speed_dps = fraction * kGameStickCal.calibration_dps;
        want_x_dps = (push_x / push_len) * stick_speed_dps;
        want_y_dps = (push_y / push_len) * stick_speed_dps;
    }

    // 7.2 Add gyro camera rate in the angular velocity domain (deg/s) — zero double-deadzone jump!
    want_x_dps += gyro_x_dps;
    want_y_dps += gyro_y_dps;

    const float target_dps = sqrtf(want_x_dps * want_x_dps + want_y_dps * want_y_dps);
    if (target_dps <= 1e-4f) {
        return;
    }

    // 7.3 Convert combined camera rate back into stick travel via inverse unpower & undeadzone_inner
    float strength = clampf(target_dps / kGameStickCal.calibration_dps, 0.0f, 1.0f);
    strength = powf(strength, 1.0f / kGameStickCal.unpower);

    float out_mag = kGameStickCal.undeadzone_inner + strength * livezone;
    // Soft entry across the bottom 1.5% of strength so micro-motions ease onto the deadzone edge smoothly
    if (push_len <= kGameStickCal.undeadzone_inner && strength < 0.015f) {
        out_mag *= (strength * (1.0f / 0.015f));
    }
    out_mag = minf(out_mag, kGameStickCal.max_stick_out);

    const float dir_x = want_x_dps / target_dps;
    const float dir_y = want_y_dps / target_dps;

    const float final_x_lsb = clampf(dir_x * out_mag * 127.0f, -127.0f, 127.0f);
    const float final_y_lsb = clampf(dir_y * out_mag * 127.0f, -127.0f, 127.0f);

    r.RightStickX = (uint8_t)clampf(128.0f + final_x_lsb + 0.5f, 0.0f, 255.0f);
    r.RightStickY = (uint8_t)clampf(128.0f + final_y_lsb + 0.5f, 0.0f, 255.0f);
}
