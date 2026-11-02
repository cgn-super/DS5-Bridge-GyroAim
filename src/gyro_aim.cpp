//
// Gyro-to-Right-Stick Aiming (Industrial-Grade Jibb Smart / Flydigi Inspired Pipeline)
// Activated by a hard L2 press with Two-Stage Adaptive Trigger Wall.
//
// Pipeline (per outgoing 500Hz USB report, float32 on RP2350 Cortex-M33 FPU):
//   1. Zero-bias tracking: while the controller is still and gyro aim is idle,
//      slowly track the per-axis gyro offset (temperature & mechanical drift).
//   2. Built-in Two-Stage L2 Mechanical Wall ("卡手"段落阻尼):
//      Automatically arms Section Resistance (0x02, 145..205, force=245) on L2
//      whenever no native in-game L2 effect is active.
//   3. Engage with hysteresis: TriggerLeft >= GA_L2_ON engages, < GA_L2_OFF releases.
//      On engage: sets Lightbar to Crimson Red (255, 20, 40).
//      On release: restores the exact Lightbar RGB color last set by the game/firmware.
//   4. Adaptive Dual-Threshold Tightening (Jibb Smart Noise Gate & Dynamic Filter):
//      - Sub-threshold (< GA_NOISE_GATE): zeroed out, completely eliminating hand pulse jitter.
//      - Micro-aiming zone (GA_NOISE_GATE .. GA_TIGHTEN_THRESH): dynamic tightening factor +
//        heavier EMA smoothing (Alpha ~ 0.22) for rock-solid precision tracking.
//      - Flick / Fast-turn zone (> GA_FAST_THRESH): dynamic acceleration + near-instant
//        straight-through response (Alpha ~ 0.90) with zero lag.
//   5. Hermite Smoothstep Anti-Deadzone (飞智同款缓入式反死区):
//      Replaces crude step-discontinuity with a C1-continuous smooth ramp that breaks
//      through the in-game right-stick deadzone without any sudden snapping or stepping.
//   6. Power Curve Linearization (反向曲率预补偿):
//      Cancels out the typical game engine quadratic/exponential stick acceleration,
//      restoring a true 1:1 linear physical feel.
//   7. Vector-based Stick Fusion:
//      Radial vector addition with physical right stick to avoid diagonal clipping.
//   8. Host Auto-Boost Hook (host_boost_hook_apply):
//      Signals the PC-side MSI Cooler Boost & RAMMap cleaner via the unused UNK2
//      field in standard HID Input Report 0x01.
//

#include "gyro_aim.h"

#include <cmath>
#include <cstdint>

#include "bt.h"
#include "pico.h"
#include "utils.h"

// ---------------------------------------------------------------------------
// Tuning Parameters (All values calibrated for 500Hz/1000Hz DualSense Gyro IMU)
// ---------------------------------------------------------------------------

#ifndef GA_L2_ON
#define GA_L2_ON 230            // L2 value (0..255) that engages gyro aim ("hard press" past wall)
#endif
#ifndef GA_L2_OFF
#define GA_L2_OFF 200           // release below this (hysteresis, avoids flicker at the edge)
#endif
#ifndef GA_WALL_START
#define GA_WALL_START 145       // L2 2-stage wall start position
#endif
#ifndef GA_WALL_END
#define GA_WALL_END 205         // L2 2-stage wall break-through position (right before GA_L2_ON=230)
#endif
#ifndef GA_WALL_FORCE
#define GA_WALL_FORCE 245       // L2 2-stage wall resistance strength (0..255)
#endif

// Gyro Scale & Sensitivity
#ifndef GA_FULL_SCALE_LSB
#define GA_FULL_SCALE_LSB 1600.0f   // Gyro LSB that maps to full stick deflection (~100 deg/s)
#endif
#ifndef GA_SENS_X
#define GA_SENS_X 1.15f             // Horizontal sensitivity multiplier
#endif
#ifndef GA_SENS_Y
#define GA_SENS_Y 1.00f             // Vertical sensitivity multiplier
#endif
#ifndef GA_ROLL_MIX
#define GA_ROLL_MIX 0.0f            // Add roll (steering-wheel tilt) to horizontal, 0..1
#endif
#ifndef GA_INVERT_X
#define GA_INVERT_X 0
#endif
#ifndef GA_INVERT_Y
#define GA_INVERT_Y 0
#endif

// Adaptive Noise Gate & Tightening Filter (Jibb Smart Precision Framework)
#ifndef GA_NOISE_GATE_LSB
#define GA_NOISE_GATE_LSB 16.0f     // Physiological hand tremor deadband (~1.0 deg/s)
#endif
#ifndef GA_TIGHTEN_THRESH_LSB
#define GA_TIGHTEN_THRESH_LSB 120.0f // Micro-aim tightening ceiling (~7.5 deg/s)
#endif
#ifndef GA_FAST_THRESH_LSB
#define GA_FAST_THRESH_LSB 480.0f   // Flick / fast sweep threshold (~30.0 deg/s)
#endif
#ifndef GA_SMOOTH_ALPHA_MIN
#define GA_SMOOTH_ALPHA_MIN 0.22f   // Heavier smoothing for micro-aim stability (lower = smoother)
#endif
#ifndef GA_SMOOTH_ALPHA_MAX
#define GA_SMOOTH_ALPHA_MAX 0.92f   // Near-zero latency smoothing for fast flicks
#endif

// Anti-Deadzone & Curve Linearization
#ifndef GA_ANTI_DEADZONE
#define GA_ANTI_DEADZONE 0.16f      // In-game right-stick mechanical deadzone to lift over (0..1)
#endif
#ifndef GA_ANTI_DEADZONE_RAMP
#define GA_ANTI_DEADZONE_RAMP 0.08f // Smoothstep transition width to avoid step snapping (0..1)
#endif
#ifndef GA_CURVE_EXPONENT
#define GA_CURVE_EXPONENT 0.72f     // Pre-distortion exponent: cancels game's quadratic stick curve
#endif

// ---------------------------------------------------------------------------
// Internal States
// ---------------------------------------------------------------------------

static float bias_pitch = 0.0f, bias_yaw = 0.0f, bias_roll = 0.0f;
static bool bias_seeded = false;
static uint32_t still_count = 0;
static bool engaged = false;
static float smooth_x = 0.0f, smooth_y = 0.0f;

// Saved in-game Lightbar color (defaults to DS5Dongle's Nijika Gold 0xFF, 0xD7, 0x00)
static uint8_t saved_led_r = 0xFF;
static uint8_t saved_led_g = 0xD7;
static uint8_t saved_led_b = 0x00;

// Track whether the host game is actively driving a non-idle L2 trigger effect
static bool game_has_custom_l2 = false;
static bool wall_armed_on_boot = false;

// Host Auto-Boost & Memory Clean Hook state (signals PC daemon via unused UNK2 byte 0xA5..0xAF)
static uint16_t boost_hook_frames = 0;
static uint8_t boost_hook_seq = 0x05;
static bool boost_combo_latch = false;
static bool boost_led_active = false;

// ---------------------------------------------------------------------------
// Math Utilities
// ---------------------------------------------------------------------------

static inline float clampf(float v, float lo, float hi) {
    return v < lo ? lo : (v > hi ? hi : v);
}

// Hermite smoothstep for C1-continuous transition: 3t^2 - 2t^3
static inline float smoothstep(float edge0, float edge1, float x) {
    const float t = clampf((x - edge0) / (edge1 - edge0), 0.0f, 1.0f);
    return t * t * (3.0f - 2.0f * t);
}

static void fill_default_l2_wall(SetStateData &state) {
    state.AllowLeftTriggerFFB = 1;
    state.LeftTriggerFFB[0] = 0x02; // Mode 0x02: Section Resistance ("卡手"二段阻尼墙)
    state.LeftTriggerFFB[1] = GA_WALL_START;
    state.LeftTriggerFFB[2] = GA_WALL_END;
    state.LeftTriggerFFB[3] = GA_WALL_FORCE;
}

static void send_gyro_led_and_wall(uint8_t r, uint8_t g, uint8_t b) {
    SetStateData state{};
    state.AllowLedColor = 1;
    state.LedRed = r;
    state.LedGreen = g;
    state.LedBlue = b;
    if (!game_has_custom_l2) {
        fill_default_l2_wall(state);
    }
    update_state(state);
}

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
            if (engaged) {
                send_gyro_led_and_wall(255, 20, 40);
            } else {
                send_gyro_led_and_wall(saved_led_r, saved_led_g, saved_led_b);
            }
        }
    }
}

void gyro_aim_on_host_output_report(SetStateData &state) {
    if (state.AllowLedColor) {
        saved_led_r = state.LedRed;
        saved_led_g = state.LedGreen;
        saved_led_b = state.LedBlue;
        if (engaged) {
            state.LedRed = 255;
            state.LedGreen = 20;
            state.LedBlue = 40;
        }
    }
    if (state.AllowLeftTriggerFFB) {
        const uint8_t mode = state.LeftTriggerFFB[0];
        // Mode 0x00 or 0x05 means "Off / Release" (no active in-game L2 effect).
        // Substitute our 2-stage mechanical resistance wall so the user always feels the click!
        if (mode == 0x00 || mode == 0x05) {
            game_has_custom_l2 = false;
            fill_default_l2_wall(state);
        } else {
            game_has_custom_l2 = true;
        }
    }
}

void __not_in_flash_func(gyro_aim_apply)(USBGetStateData &r) {
    const float pitch = (float) r.AngularVelocityX;
    const float yaw   = (float) r.AngularVelocityZ;
    const float roll  = (float) r.AngularVelocityY;

    // ---- 1. Zero-Bias Tracking & Initial Arms ----
    if (!bias_seeded) {
        bias_pitch = pitch;
        bias_yaw = yaw;
        bias_roll = roll;
        bias_seeded = true;
    }
    if (!wall_armed_on_boot && r.SensorTimestamp > 0) {
        wall_armed_on_boot = true;
        boost_hook_frames = 500; // ~1.0s initial auto-boost signal pulse on controller link
        send_gyro_led_and_wall(saved_led_r, saved_led_g, saved_led_b);
    }

    host_boost_hook_apply(r);

    const float dp = pitch - bias_pitch;
    const float dy = yaw   - bias_yaw;
    const float dr = roll  - bias_roll;
    const bool still = (fabsf(dp) < 35.0f && fabsf(dy) < 35.0f && fabsf(dr) < 35.0f);
    still_count = still ? (still_count + 1) : 0;
    if (!engaged && still_count > 150) {
        // Continuous slow tracking of thermal/zero drift
        bias_pitch += 0.008f * dp;
        bias_yaw   += 0.008f * dy;
        bias_roll  += 0.008f * dr;
    }

    // ---- 2. Engage Hysteresis & Tactical Red Lightbar ----
    const bool prev_engaged = engaged;
    if (!engaged && r.TriggerLeft >= GA_L2_ON) {
        engaged = true;
    } else if (engaged && r.TriggerLeft < GA_L2_OFF) {
        engaged = false;
    }

    if (engaged != prev_engaged) {
        if (engaged) {
            send_gyro_led_and_wall(255, 20, 40); // Tactical Crimson Red on ADS engage
        } else {
            send_gyro_led_and_wall(saved_led_r, saved_led_g, saved_led_b); // Restore game color
        }
    }

    if (!engaged) {
        smooth_x = smooth_y = 0.0f;
        return;
    }

    // ---- 3. Coordinate Formulation ----
    float vx = -(dy + GA_ROLL_MIX * dr);
    float vy = -dp;
    if (GA_INVERT_X) vx = -vx;
    if (GA_INVERT_Y) vy = -vy;

    const float raw_mag = sqrtf(vx * vx + vy * vy);

    // ---- 4. Adaptive Dual-Threshold Noise Gate & Tightening (Jibb Smart System) ----
    float norm_x = 0.0f;
    float norm_y = 0.0f;
    float dynamic_alpha = GA_SMOOTH_ALPHA_MIN;

    if (raw_mag > GA_NOISE_GATE_LSB) {
        // Effective magnitude after deducting baseline physiological jitter
        const float active_mag = raw_mag - GA_NOISE_GATE_LSB;

        // Dynamic Tightening: in the micro-aim region, smoothly scale precision factor
        float tighten_factor = 1.0f;
        if (raw_mag < GA_TIGHTEN_THRESH_LSB) {
            // Smoothly ease precision from 0.55 up to 1.0
            tighten_factor = 0.55f + 0.45f * smoothstep(GA_NOISE_GATE_LSB, GA_TIGHTEN_THRESH_LSB, raw_mag);
        }

        // Full scale deflection mapping
        float base_norm = (active_mag / GA_FULL_SCALE_LSB) * tighten_factor;
        base_norm = clampf(base_norm, 0.0f, 1.25f);

        // Pre-distortion / Power Curve Linearization: cancels game engine quadratic curve
        if (base_norm > 1e-4f && GA_CURVE_EXPONENT > 0.0f) {
            base_norm = powf(base_norm, GA_CURVE_EXPONENT);
        }

        const float unit_x = vx / raw_mag;
        const float unit_y = vy / raw_mag;

        norm_x = unit_x * base_norm * GA_SENS_X;
        norm_y = unit_y * base_norm * GA_SENS_Y;

        // Dynamic EMA filter: heavier smoothing during micro-aim, near zero lag during fast sweeps
        const float speed_ratio = smoothstep(GA_TIGHTEN_THRESH_LSB, GA_FAST_THRESH_LSB, raw_mag);
        dynamic_alpha = GA_SMOOTH_ALPHA_MIN + (GA_SMOOTH_ALPHA_MAX - GA_SMOOTH_ALPHA_MIN) * speed_ratio;
    }

    // ---- 5. Dynamic EMA Smoothing Filter ----
    smooth_x = dynamic_alpha * norm_x + (1.0f - dynamic_alpha) * smooth_x;
    smooth_y = dynamic_alpha * norm_y + (1.0f - dynamic_alpha) * smooth_y;

    // ---- 6. Hermite Smoothstep Anti-Deadzone (No-Snap Smooth Breakthrough) ----
    float gyro_stick_x = 0.0f;
    float gyro_stick_y = 0.0f;
    const float smooth_mag = sqrtf(smooth_x * smooth_x + smooth_y * smooth_y);

    if (smooth_mag > 1e-4f) {
        const float mc = clampf(smooth_mag, 0.0f, 1.0f);

        // Instead of instantaneous jump to GA_ANTI_DEADZONE, ramp up C1-continuously
        // across the buffer zone [0, GA_ANTI_DEADZONE_RAMP]
        const float ramp = smoothstep(0.0f, GA_ANTI_DEADZONE_RAMP, mc);
        const float lifted = (ramp * GA_ANTI_DEADZONE) + ((1.0f - GA_ANTI_DEADZONE) * mc);

        const float dir_x = smooth_x / smooth_mag;
        const float dir_y = smooth_y / smooth_mag;

        gyro_stick_x = dir_x * lifted;
        gyro_stick_y = dir_y * lifted;
    }

    // ---- 7. Radial Vector-based Stick Fusion (Physical Thumbstick + Gyro) ----
    // Physical stick normalized from [0..255] to [-1.0 .. +1.0] (center at 128)
    const float phys_x = ((float) r.RightStickX - 128.0f) / 127.0f;
    const float phys_y = ((float) r.RightStickY - 128.0f) / 127.0f;

    // Vectorial combination
    float final_x = phys_x + gyro_stick_x;
    float final_y = phys_y + gyro_stick_y;

    // Radial saturation clamp to prevent diagonal distortion/overshoot
    const float final_mag = sqrtf(final_x * final_x + final_y * final_y);
    if (final_mag > 1.0f) {
        final_x /= final_mag;
        final_y /= final_mag;
    }

    // Convert back to DualSense 8-bit Stick Range [0..255]
    r.RightStickX = (uint8_t) clampf(final_x * 127.0f + 128.0f + 0.5f, 0.0f, 255.0f);
    r.RightStickY = (uint8_t) clampf(final_y * 127.0f + 128.0f + 0.5f, 0.0f, 255.0f);
}
