//
// DS5-Bridge Gyro-Aim v2.0 — Ultimate 6-Axis IMU Attitude & PD Lead Control Architecture
// Target: Raspberry Pi Pico 2 W (RP2350 Cortex-M33 Single-Precision FPU @ 250 MHz)
//
// Zero dynamic memory allocation, zero compiler warnings, 100% SRAM time-critical hot path.
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

enum class ActiveProfileState : uint8_t {
    kNone    = 0,
    kCombat  = 1,
    kGrenade = 2
};

// Active mode (defaults to Mode 1: kCombatAndGrenade)
static ActivationMode active_mode = ActivationMode::kCombatAndGrenade;
static ActiveProfileState active_profile_state = ActiveProfileState::kNone;

// Factory Calibration (Feature Report 0x05, fallback: 1/16 deg/s/LSB, 1/8192 g/LSB)
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

// Online Thermal Drift Bias (in °/s) & 0.6s Sliding Stillness Window
static bool bias_seeded = false;
static float bias_gx_dps = 0.0f;
static float bias_gy_dps = 0.0f;
static float bias_gz_dps = 0.0f;
static float prev_ax_g = 0.0f;
static float prev_ay_g = 1.0f;
static float prev_az_g = 0.0f;
static float still_accum_sec = 0.0f;

// Hardware SensorTimestamp (3 MHz, 1/3 us per tick)
static bool ts_seeded = false;
static uint32_t prev_sensor_ts = 0;
static float dt_smooth_sec = 0.002f;

// Gravity Unit Vector in Controller Body Frame (+Y is up when flat on desk)
static float grav_x = 0.0f;
static float grav_y = 1.0f;
static float grav_z = 0.0f;

// Control Law States (Reset on profile transition or shortcut trigger)
static float filt_yaw_dps = 0.0f;
static float filt_pitch_dps = 0.0f;
static float prev_filt_yaw_dps = 0.0f;
static float prev_filt_pitch_dps = 0.0f;
static float deriv_yaw_dps2 = 0.0f;
static float deriv_pitch_dps2 = 0.0f;
static float leaky_yaw_dps = 0.0f;
static float leaky_pitch_dps = 0.0f;
static float cached_out_norm_x = 0.0f;
static float cached_out_norm_y = 0.0f;

// Chassis Vibration Energy Monitor
static float prev_raw_yaw_dps = 0.0f;
static float prev_raw_pitch_dps = 0.0f;
static float vib_energy_ema = 0.0f;

// Fast-Stop Rebound Damping State (40ms flick memory -> 35ms 0.30f damping)
static float fast_flick_timer_ms = 0.0f;
static int8_t fast_flick_sign_x = 0;
static int8_t fast_flick_sign_y = 0;
static float rebound_damp_ms = 0.0f;

// R2 Trigger Pull Instant Suppression (R2 rising across 45 -> 30ms @ 0.25f envelope)
static uint8_t prev_r2_val = 0;
static float r2_suppress_ms = 0.0f;

// Ergonomic State Machine Timers & Latches
static bool combat_trig_engaged = false;
static bool underbarrel_gl_active = false;
static bool prev_touch_right_active = false;
static float r3_pulse_hold_ms = 0.0f;
static float touch_landing_mute_ms = 0.0f;
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

static inline float smoothstepf(float edge0, float edge1, float x) {
    const float denom = edge1 - edge0;
    if (denom <= 1e-6f) return x >= edge1 ? 1.0f : 0.0f;
    const float t = clampf((x - edge0) / denom, 0.0f, 1.0f);
    return t * t * (3.0f - 2.0f * t);
}

static inline int16_t read_le_i16(const uint8_t *p) {
    return (int16_t)((uint16_t)p[0] | ((uint16_t)p[1] << 8));
}

static inline void reset_control_history() {
    filt_yaw_dps = 0.0f;
    filt_pitch_dps = 0.0f;
    prev_filt_yaw_dps = 0.0f;
    prev_filt_pitch_dps = 0.0f;
    deriv_yaw_dps2 = 0.0f;
    deriv_pitch_dps2 = 0.0f;
    leaky_yaw_dps = 0.0f;
    leaky_pitch_dps = 0.0f;
    cached_out_norm_x = 0.0f;
    cached_out_norm_y = 0.0f;
    fast_flick_timer_ms = 0.0f;
    rebound_damp_ms = 0.0f;
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
    if (active_profile_state == ActiveProfileState::kGrenade) {
        r = 255; g = 110; b = 0; // Tactical Orange for Grenade / Underbarrel GL
        return;
    }
    if (active_profile_state == ActiveProfileState::kCombat &&
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
    if (++factory_cal_probe_div != 1) return; // Probe once every 256 reports until loaded

    const auto it = feature_data.find(0x05);
    if (it == feature_data.end() || it->second.size() < 34) {
        return;
    }

    const uint8_t *buf = it->second.data(); // Starts at byte 1 of Feature Report 0x05
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
            boost_hook_frames = 300; // ~0.6s pulse at 500Hz
            boost_led_active = true;
            send_gyro_led_and_wall(0, 240, 255); // Ice Cyan visual confirmation
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
        boost_hook_frames = 500; // ~1.0s initial auto-boost signal pulse on controller link
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
        const float clamped_dt = clampf(raw_dt, 0.0005f, 0.02f);
        // Smooth sudden timestamp jumps while tracking true crystal interval
        dt_smooth_sec = (raw_dt >= 0.0005f && raw_dt <= 0.02f)
                            ? (0.85f * dt_smooth_sec + 0.15f * clamped_dt)
                            : dt_smooth_sec;
        dt_sec = clamped_dt;
        is_new_imu_sample = true;
    }

    const float frame_dt_ms = (float)DS5_USB_POLL_INTERVAL_MS;

    // ---- 3. Update Visual Flash & Suppression Countdown Timers ----
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
    if (touch_landing_mute_ms > 0.0f) {
        touch_landing_mute_ms = maxf(0.0f, touch_landing_mute_ms - frame_dt_ms);
    }
    if (r3_pulse_hold_ms > 0.0f) {
        r3_pulse_hold_ms = maxf(0.0f, r3_pulse_hold_ms - frame_dt_ms);
    }
    if (fast_flick_timer_ms > 0.0f) {
        fast_flick_timer_ms = maxf(0.0f, fast_flick_timer_ms - frame_dt_ms);
    }
    if (rebound_damp_ms > 0.0f) {
        rebound_damp_ms = maxf(0.0f, rebound_damp_ms - frame_dt_ms);
    }
    if (r2_suppress_ms > 0.0f) {
        r2_suppress_ms = maxf(0.0f, r2_suppress_ms - frame_dt_ms);
    }

    // ---- 4. On-the-Fly Shortcut Combos: Create(Share) + D-Pad ----
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
                        flash_led_r = 0; flash_led_g = 230; flash_led_b = 100; // Mode 1: Emerald Green
                        break;
                    case ActivationMode::kSoftL2Only:
                        flash_led_r = 0; flash_led_g = 120; flash_led_b = 255; // Mode 2: Sapphire Blue
                        break;
                    case ActivationMode::kAlwaysOnSmartStick:
                        flash_led_r = 255; flash_led_g = 175; flash_led_b = 0; // Mode 3: Amber Yellow
                        break;
                    case ActivationMode::kClassicTwoStage:
                        flash_led_r = 255; flash_led_g = 20; flash_led_b = 40; // Mode 4: Crimson Red
                        break;
                }
                send_gyro_led_and_wall(flash_led_r, flash_led_g, flash_led_b);
            } else if (dpad_now == (uint8_t)Direction::East || dpad_now == (uint8_t)Direction::West) {
                const float step = (dpad_now == (uint8_t)Direction::East) ? 0.10f : -0.10f;
                user_sens_scale = clampf(user_sens_scale + step, 0.50f, 2.00f);
                triggered = true;
                mode_led_flash_ms = 220.0f;
                flash_led_r = 255; flash_led_g = 255; flash_led_b = 255; // White Flash
                send_gyro_led_and_wall(flash_led_r, flash_led_g, flash_led_b);
            }

            if (triggered) {
                shortcut_mute_ms = 80.0f;
                reset_control_history();
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

    // ---- 5. Right-Half Capacitive Touchpad -> R3 & Contextual Underbarrel GL ----
    // Touchpad resolution is 1920x1080 (X: 0..1919). Finger[0].NotTouching == 0 means active contact.
    const bool touch0_active = (r.Touch.Finger[0].NotTouching == 0);
    const uint16_t touch0_x = (uint16_t)r.Touch.Finger[0].FingerX;
    const bool touch_right_active = touch0_active && (touch0_x >= 960);

    if (touch_right_active && !prev_touch_right_active) {
        // Rising edge of right-half touchpad tap
        r3_pulse_hold_ms = 50.0f;       // Minimum 50ms R3 pulse guarantee
        touch_landing_mute_ms = 60.0f;  // 60ms gyro mute to absorb thumb landing tremor
        if (r.TriggerLeft >= GA_SOFT_TRIG_ON) {
            underbarrel_gl_active = !underbarrel_gl_active;
        }
    }
    prev_touch_right_active = touch_right_active;

    if (r.TriggerLeft < GA_SOFT_TRIG_OFF) {
        underbarrel_gl_active = false; // Auto-reset underbarrel GL state when releasing L2 ADS
    }

    if (touch_right_active || r3_pulse_hold_ms > 0.0f) {
        r.ButtonR3 = 1; // Logical OR with physical R3; physical ButtonPad remains 100% untouched
    }

    // ---- 6. Circle (O) Hold Grenade Throw & Priority Arbitration ----
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
    const bool grenade_throw_active = (circle_hold_ms >= 80.0f) || (grenade_tail_ms > 0.0f);

    // Evaluate mode-dependent combat trigger engagement with hysteresis
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

    // Priority Arbitration: Grenade Throw > Underbarrel GL (while ADS) > Combat Trigger > None
    ActiveProfileState next_profile_state = ActiveProfileState::kNone;
    if (grenade_throw_active || (underbarrel_gl_active && r.TriggerLeft >= GA_SOFT_TRIG_OFF)) {
        next_profile_state = ActiveProfileState::kGrenade;
    } else if (combat_trig_engaged) {
        next_profile_state = ActiveProfileState::kCombat;
    }

    if (next_profile_state != active_profile_state) {
        active_profile_state = next_profile_state;
        reset_control_history();
        if (mode_led_flash_ms <= 0.0f && !boost_led_active) {
            uint8_t lr, lg, lb;
            get_active_target_led(lr, lg, lb);
            send_gyro_led_and_wall(lr, lg, lb);
        }
    }

    // Detect R2 trigger pull rising edge across 45 for 30ms first-shot recoil suppression
    if (prev_r2_val < 45 && r.TriggerRight >= 45) {
        r2_suppress_ms = 30.0f;
    }
    prev_r2_val = r.TriggerRight;

    // ---- 7. 6-Axis IMU Scaling, Online Bias Tracking & Gravity Quaternion Alignment ----
    // Map utils.h struct fields to DualSense's physical right-hand coordinate system (X=Right, Y=Up, Z=Back):
    //   Offset 15 (AngularVelocityX) -> Pitch rate around +X
    //   Offset 17 (AngularVelocityZ) -> Yaw rate around +Y (Vertical axis!)
    //   Offset 19 (AngularVelocityY) -> Roll rate around +Z (Longitudinal axis!)
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
        prev_ax_g = ax_g;
        prev_ay_g = ay_g;
        prev_az_g = az_g;
        bias_seeded = true;
    }

    if (is_new_imu_sample) {
        const float wx_dps = raw_gx_dps - bias_gx_dps;
        const float wy_dps = raw_gy_dps - bias_gy_dps;
        const float wz_dps = raw_gz_dps - bias_gz_dps;

        // 0.6s Sliding Window Stillness Detector for Continuous Thermal Drift Convergence
        const float da_g = fabsf(ax_g - prev_ax_g) + fabsf(ay_g - prev_ay_g) + fabsf(az_g - prev_az_g);
        prev_ax_g = ax_g;
        prev_ay_g = ay_g;
        prev_az_g = az_g;

        const bool micro_still = (fabsf(wx_dps) < 1.8f && fabsf(wy_dps) < 1.8f && fabsf(wz_dps) < 1.8f && da_g < 0.045f);
        if (micro_still) {
            still_accum_sec += dt_sec;
            if (still_accum_sec >= 0.60f) {
                const float alpha_bias = clampf(dt_sec / 1.4f, 0.0005f, 0.015f);
                bias_gx_dps += alpha_bias * wx_dps;
                bias_gy_dps += alpha_bias * wy_dps;
                bias_gz_dps += alpha_bias * wz_dps;
            }
        } else {
            still_accum_sec = 0.0f;
        }

        // Complementary Filter for Unit Gravity Vector g_hat in Body Frame
        constexpr float kDegToRad = 0.01745329252f;
        const float wx_rad = wx_dps * kDegToRad;
        const float wy_rad = wy_dps * kDegToRad;
        const float wz_rad = wz_dps * kDegToRad;

        // Gyro kinematics: dg/dt = g x w
        float gx_pred = grav_x + (grav_y * wz_rad - grav_z * wy_rad) * dt_sec;
        float gy_pred = grav_y + (grav_z * wx_rad - grav_x * wz_rad) * dt_sec;
        float gz_pred = grav_z + (grav_x * wy_rad - grav_y * wx_rad) * dt_sec;

        const float a_norm = sqrtf(ax_g * ax_g + ay_g * ay_g + az_g * az_g);
        // Confidence-weighted accelerometer fusion: only trust accel when ||a|| in [0.85g, 1.15g]
        if (a_norm >= 0.85f && a_norm <= 1.15f) {
            const float inv_a = 1.0f / a_norm;
            const float ax_u = ax_g * inv_a;
            const float ay_u = ay_g * inv_a;
            const float az_u = az_g * inv_a;
            const float conf = 1.0f - (fabsf(a_norm - 1.0f) * (1.0f / 0.15f));
            const float beta = conf * clampf(dt_sec * 8.0f, 0.002f, 0.12f);
            gx_pred = (1.0f - beta) * gx_pred + beta * ax_u;
            gy_pred = (1.0f - beta) * gy_pred + beta * ay_u;
            gz_pred = (1.0f - beta) * gz_pred + beta * az_u;
        }

        const float g_norm = sqrtf(gx_pred * gx_pred + gy_pred * gy_pred + gz_pred * gz_pred);
        if (g_norm > 1e-4f) {
            const float inv_g = 1.0f / g_norm;
            grav_x = gx_pred * inv_g;
            grav_y = gy_pred * inv_g;
            grav_z = gz_pred * inv_g;
        }

        // Shortest-Arc Quaternion q_align rotating g_hat -> Reference Vertical Axis u = (0, 1, 0)
        // Cross product: g_hat x (0, 1, 0) = (-grav_z, 0, grav_x), Dot product: grav_y
        float qw = 1.0f, qx = 0.0f, qy = 0.0f, qz = 0.0f;
        if (grav_y > -0.999f) {
            const float s = sqrtf(2.0f * (1.0f + grav_y));
            const float inv_s = 1.0f / s;
            qw = 0.5f * s;
            qx = -grav_z * inv_s;
            qy = 0.0f;
            qz = grav_x * inv_s;
        } else {
            qw = 0.0f; qx = 0.0f; qy = 0.0f; qz = 1.0f;
        }

        // Rotate body angular velocity (wx_dps, wy_dps, wz_dps) by q_align into Orthogonal Player Space:
        // v' = v + 2*qw*(q_v x v) + 2*q_v x (q_v x v)
        const float tx = 2.0f * (qy * wz_dps - qz * wy_dps);
        const float ty = 2.0f * (qz * wx_dps - qx * wz_dps);
        const float tz = 2.0f * (qx * wy_dps - qy * wx_dps);

        const float wx_player = wx_dps + qw * tx + (qy * tz - qz * ty);
        const float wy_player = wy_dps + qw * ty + (qz * tx - qx * tz);

        // DualSense Right-Hand Polarity to Right Stick (Right = +X, Down = +Y):
        // Turning right -> wy_player < 0 -> +StickX = -wy_player
        // Tilting up    -> wx_player > 0 -> -StickY = -wx_player
        const float raw_yaw_dps   = -wy_player;
        const float raw_pitch_dps = -wx_player;

        // ---- 8. Control Law Pipeline (Active on Fresh IMU Sample) ----
        if (gyro_bypass_debug ||
            active_profile_state == ActiveProfileState::kNone ||
            shortcut_mute_ms > 0.0f ||
            touch_landing_mute_ms > 0.0f) {
            cached_out_norm_x = 0.0f;
            cached_out_norm_y = 0.0f;
            prev_raw_yaw_dps = raw_yaw_dps;
            prev_raw_pitch_dps = raw_pitch_dps;
        } else {
            const GyroProfile &prof = (active_profile_state == ActiveProfileState::kGrenade)
                                          ? kGrenadeProfile
                                          : kCombatProfile;

            // 8.1 Chassis Vibration Interference Monitor
            const float hf_jerk = fabsf(raw_yaw_dps - prev_raw_yaw_dps) + fabsf(raw_pitch_dps - prev_raw_pitch_dps);
            prev_raw_yaw_dps = raw_yaw_dps;
            prev_raw_pitch_dps = raw_pitch_dps;
            vib_energy_ema = 0.88f * vib_energy_ema + 0.12f * hf_jerk;

            const float vib_boost = (vib_energy_ema > 10.0f)
                                        ? clampf(1.0f + 0.05f * (vib_energy_ema - 10.0f), 1.0f, 2.0f)
                                        : 1.0f;
            const float eff_v_low = prof.v_low * vib_boost;

            // 8.2 Speed-Adaptive Low-Pass Filter + Micro-Speed Quadratic Clamping
            const float raw_speed_dps = sqrtf(raw_yaw_dps * raw_yaw_dps + raw_pitch_dps * raw_pitch_dps);
            constexpr float kAlphaLow = 0.16f;
            const float speed_t = smoothstepf(eff_v_low, prof.v_high, raw_speed_dps);
            const float alpha = kAlphaLow + (1.0f - kAlphaLow) * speed_t;

            float micro_clamp = 1.0f;
            if (raw_speed_dps < eff_v_low && eff_v_low > 1e-4f) {
                const float r_ratio = raw_speed_dps / eff_v_low;
                micro_clamp = r_ratio * r_ratio; // Quadratic micro-speed attenuation
            }

            const float clamped_yaw_dps   = raw_yaw_dps * micro_clamp;
            const float clamped_pitch_dps = raw_pitch_dps * micro_clamp;

            filt_yaw_dps   = alpha * clamped_yaw_dps   + (1.0f - alpha) * filt_yaw_dps;
            filt_pitch_dps = alpha * clamped_pitch_dps + (1.0f - alpha) * filt_pitch_dps;

            // 8.3 Fast-Stop Rebound Damping (40ms >45 deg/s flick -> zero-crossing -> 35ms @ 0.30f)
            const float filt_speed_dps = sqrtf(filt_yaw_dps * filt_yaw_dps + filt_pitch_dps * filt_pitch_dps);
            if (filt_speed_dps > 45.0f) {
                fast_flick_timer_ms = 40.0f;
                fast_flick_sign_x = (filt_yaw_dps > 4.0f) ? 1 : ((filt_yaw_dps < -4.0f) ? -1 : 0);
                fast_flick_sign_y = (filt_pitch_dps > 4.0f) ? 1 : ((filt_pitch_dps < -4.0f) ? -1 : 0);
            } else if (fast_flick_timer_ms > 0.0f) {
                const bool rev_x = (fast_flick_sign_x != 0) && ((filt_yaw_dps * (float)fast_flick_sign_x) < -0.6f);
                const bool rev_y = (fast_flick_sign_y != 0) && ((filt_pitch_dps * (float)fast_flick_sign_y) < -0.6f);
                if (rev_x || rev_y) {
                    rebound_damp_ms = 35.0f;
                    fast_flick_timer_ms = 0.0f;
                }
            }
            const float rebound_gain = (rebound_damp_ms > 0.0f) ? 0.30f : 1.0f;

            // 8.4 PD Lead Control Law (with derivative spike clamp against BT packet jitter)
            const float inv_dt = 1.0f / maxf(dt_sec, 0.0005f);
            constexpr float kMaxDerivDps2 = 1600.0f;
            deriv_yaw_dps2   = clampf((filt_yaw_dps   - prev_filt_yaw_dps)   * inv_dt, -kMaxDerivDps2, kMaxDerivDps2);
            deriv_pitch_dps2 = clampf((filt_pitch_dps - prev_filt_pitch_dps) * inv_dt, -kMaxDerivDps2, kMaxDerivDps2);
            prev_filt_yaw_dps   = filt_yaw_dps;
            prev_filt_pitch_dps = filt_pitch_dps;

            const float pd_yaw_dps   = prof.kp_gain * filt_yaw_dps   + prof.kd_lead_sec * deriv_yaw_dps2;
            const float pd_pitch_dps = prof.kp_gain * filt_pitch_dps + prof.kd_lead_sec * deriv_pitch_dps2;

            // 8.5 R2 Pull 30ms Suppression Envelope & Right-Stick Dynamic Dodge
            float r2_env_gain = 1.0f;
            if (r2_suppress_ms > 0.0f) {
                const float p = 1.0f - (r2_suppress_ms * (1.0f / 30.0f));
                r2_env_gain = 0.25f + 0.75f * smoothstepf(0.0f, 1.0f, p);
            }

            const float phys_rx = ((float)r.RightStickX - 128.0f) * (1.0f / 127.0f);
            const float phys_ry = ((float)r.RightStickY - 128.0f) * (1.0f / 127.0f);
            const float stick_mag = sqrtf(phys_rx * phys_rx + phys_ry * phys_ry);

            float stick_dodge_gain = 1.0f;
            if (active_mode == ActivationMode::kAlwaysOnSmartStick) {
                if (stick_mag > 0.35f) {
                    stick_dodge_gain = 0.0f;
                    leaky_yaw_dps = 0.0f;
                    leaky_pitch_dps = 0.0f;
                } else {
                    stick_dodge_gain = 1.0f - smoothstepf(0.22f, 0.35f, stick_mag);
                }
            } else {
                stick_dodge_gain = 1.0f - 0.65f * smoothstepf(0.15f, 0.90f, stick_mag);
            }

            const float total_gain = user_sens_scale * rebound_gain * r2_env_gain * stick_dodge_gain;
            const float eff_yaw_dps   = pd_yaw_dps * total_gain;
            const float eff_pitch_dps = pd_pitch_dps * total_gain * prof.yx_ratio;

            // 8.6 Short-Term Leaky Integrator & Gamma Power Compression
            const float tau = maxf(prof.leaky_tau_sec, 0.001f);
            const float leak_decay = tau / (tau + dt_sec);
            leaky_yaw_dps   = leak_decay * leaky_yaw_dps   + (1.0f - leak_decay) * eff_yaw_dps;
            leaky_pitch_dps = leak_decay * leaky_pitch_dps + (1.0f - leak_decay) * eff_pitch_dps;

            const float comb_yaw_dps   = 0.78f * eff_yaw_dps   + 0.22f * leaky_yaw_dps;
            const float comb_pitch_dps = 0.78f * eff_pitch_dps + 0.22f * leaky_pitch_dps;
            const float comb_speed_dps = sqrtf(comb_yaw_dps * comb_yaw_dps + comb_pitch_dps * comb_pitch_dps);

            if (comb_speed_dps > 1e-4f) {
                const float u_norm = clampf(comb_speed_dps / prof.full_scale_dps, 0.0f, 1.0f);
                const float u_gamma = powf(u_norm, prof.gamma);

                // 0 ~ 2.5 deg/s Zero-Crossing Soft Ramp
                const float zero_ramp = smoothstepf(0.0f, 2.5f, comb_speed_dps);
                const float adz = prof.anti_deadzone * zero_ramp;

                const float dir_x = comb_yaw_dps / comb_speed_dps;
                const float dir_y = comb_pitch_dps / comb_speed_dps;

                // 85% Radial + 15% Axial Hybrid Anti-Deadzone
                const float radial_x = dir_x * (adz * 0.85f);
                const float radial_y = dir_y * (adz * 0.85f);

                const float axial_ramp_x = smoothstepf(0.0f, 1.5f, fabsf(comb_yaw_dps));
                const float axial_ramp_y = smoothstepf(0.0f, 1.5f, fabsf(comb_pitch_dps));
                const float axial_x = ((comb_yaw_dps >= 0.0f) ? 1.0f : -1.0f) * (adz * 0.15f) * axial_ramp_x;
                const float axial_y = ((comb_pitch_dps >= 0.0f) ? 1.0f : -1.0f) * (adz * 0.15f) * axial_ramp_y;

                const float dyn_span = (prof.max_stick_out - adz) * u_gamma * zero_ramp;
                cached_out_norm_x = clampf(radial_x + axial_x + dir_x * dyn_span, -prof.max_stick_out, prof.max_stick_out);
                cached_out_norm_y = clampf(radial_y + axial_y + dir_y * dyn_span, -prof.max_stick_out, prof.max_stick_out);
            } else {
                cached_out_norm_x = 0.0f;
                cached_out_norm_y = 0.0f;
            }
        }
    } else if (gyro_bypass_debug ||
               active_profile_state == ActiveProfileState::kNone ||
               shortcut_mute_ms > 0.0f ||
               touch_landing_mute_ms > 0.0f) {
        cached_out_norm_x = 0.0f;
        cached_out_norm_y = 0.0f;
    }

    // ---- 9. Superpose onto Physical Right Stick with 2D Circular Clamp (hypot <= 127.0f) ----
    if (cached_out_norm_x == 0.0f && cached_out_norm_y == 0.0f) {
        return;
    }

    float dx_lsb = ((float)r.RightStickX - 128.0f) + cached_out_norm_x * 127.0f;
    float dy_lsb = ((float)r.RightStickY - 128.0f) + cached_out_norm_y * 127.0f;

    const float hypot_lsb = sqrtf(dx_lsb * dx_lsb + dy_lsb * dy_lsb);
    if (hypot_lsb > 127.0f) {
        const float scale = 127.0f / hypot_lsb;
        dx_lsb *= scale;
        dy_lsb *= scale;
    }

    r.RightStickX = (uint8_t)clampf(128.0f + dx_lsb + 0.5f, 0.0f, 255.0f);
    r.RightStickY = (uint8_t)clampf(128.0f + dy_lsb + 0.5f, 0.0f, 255.0f);
}
