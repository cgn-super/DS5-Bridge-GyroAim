//
// Gyro-to-Right-Stick aiming, activated by a hard L2 press.
//
// Pipeline (per outgoing USB report, float on the M33 FPU):
//   1. Zero-bias tracking: while the controller is still and gyro aim is idle,
//      slowly track the per-axis gyro offset (temperature drift).
//   2. Built-in Two-Stage L2 Mechanical Wall ("卡手"段落阻尼):
//      Automatically arms Section Resistance (0x02, 145..205, force=245) on L2
//      whenever no native in-game L2 effect is active.
//   3. Engage with hysteresis: TriggerLeft >= GA_L2_ON engages, < GA_L2_OFF releases.
//      On engage: temporarily sets Lightbar to Crimson Red (255, 20, 40).
//      On release: restores the exact Lightbar RGB color last set by the game/firmware.
//   4. Angular velocity (yaw [+ roll mix], pitch) -> radial soft deadzone ->
//      normalised stick deflection (GA_FULL_SCALE_LSB == full deflection).
//   5. Light EMA smoothing + radial anti-deadzone + additive Right Stick clamp.
//

#include "gyro_aim.h"

#include <cmath>
#include <cstdint>

#include "bt.h"
#include "pico.h"
#include "utils.h"

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
#ifndef GA_FULL_SCALE_LSB
#define GA_FULL_SCALE_LSB 1600.0f   // gyro LSB that maps to full stick deflection (~100 deg/s)
#endif
#ifndef GA_DEADZONE_LSB
#define GA_DEADZONE_LSB 20.0f       // post-bias noise floor (~1.2 deg/s)
#endif
#ifndef GA_ANTI_DEADZONE
#define GA_ANTI_DEADZONE 0.18f      // typical in-game right-stick deadzone to jump over (0..1)
#endif
#ifndef GA_SMOOTH_ALPHA
#define GA_SMOOTH_ALPHA 0.5f        // EMA weight of the newest sample (1.0 = no smoothing)
#endif
#ifndef GA_ROLL_MIX
#define GA_ROLL_MIX 0.0f            // add roll (steering-wheel tilt) to horizontal, 0..1
#endif
#ifndef GA_SENS_X
#define GA_SENS_X 1.0f
#endif
#ifndef GA_SENS_Y
#define GA_SENS_Y 1.0f
#endif
#ifndef GA_INVERT_X
#define GA_INVERT_X 0
#endif
#ifndef GA_INVERT_Y
#define GA_INVERT_Y 0
#endif

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

static inline float clampf(float v, float lo, float hi) {
    return v < lo ? lo : (v > hi ? hi : v);
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
    const float yaw = (float) r.AngularVelocityZ;
    const float roll = (float) r.AngularVelocityY;

    // ---- 1. bias tracking & initial L2 wall arming ----
    if (!bias_seeded) {
        bias_pitch = pitch;
        bias_yaw = yaw;
        bias_roll = roll;
        bias_seeded = true;
    }
    if (!wall_armed_on_boot && r.SensorTimestamp > 0) {
        wall_armed_on_boot = true;
        send_gyro_led_and_wall(saved_led_r, saved_led_g, saved_led_b);
    }

    const float dp = pitch - bias_pitch, dy = yaw - bias_yaw, dr = roll - bias_roll;
    const bool still = fabsf(dp) < 40.0f && fabsf(dy) < 40.0f && fabsf(dr) < 40.0f;
    still_count = still ? still_count + 1 : 0;
    if (!engaged && still_count > 200) {
        bias_pitch += 0.01f * dp;
        bias_yaw += 0.01f * dy;
        bias_roll += 0.01f * dr;
    }

    // ---- 2. engage with hysteresis & Lightbar Red indicator ----
    const bool prev_engaged = engaged;
    if (!engaged && r.TriggerLeft >= GA_L2_ON) engaged = true;
    else if (engaged && r.TriggerLeft < GA_L2_OFF) engaged = false;

    if (engaged != prev_engaged) {
        if (engaged) {
            send_gyro_led_and_wall(255, 20, 40);
        } else {
            send_gyro_led_and_wall(saved_led_r, saved_led_g, saved_led_b);
        }
    }

    if (!engaged) {
        smooth_x = smooth_y = 0.0f;
        return;
    }

    // ---- 3. angular velocity -> normalised deflection ----
    float vx = -(dy + GA_ROLL_MIX * dr);
    float vy = -dp;
    if (GA_INVERT_X) vx = -vx;
    if (GA_INVERT_Y) vy = -vy;

    const float mag = sqrtf(vx * vx + vy * vy);
    float nx = 0.0f, ny = 0.0f;
    if (mag > GA_DEADZONE_LSB) {
        const float scale = (mag - GA_DEADZONE_LSB) / mag / GA_FULL_SCALE_LSB;
        nx = vx * scale * GA_SENS_X;
        ny = vy * scale * GA_SENS_Y;
    }

    // ---- 4. smoothing ----
    smooth_x = GA_SMOOTH_ALPHA * nx + (1.0f - GA_SMOOTH_ALPHA) * smooth_x;
    smooth_y = GA_SMOOTH_ALPHA * ny + (1.0f - GA_SMOOTH_ALPHA) * smooth_y;

    // ---- 5. radial anti-deadzone ----
    float ox = 0.0f, oy = 0.0f;
    const float m = sqrtf(smooth_x * smooth_x + smooth_y * smooth_y);
    if (m > 1e-3f) {
        const float mc = m > 1.0f ? 1.0f : m;
        const float lifted = GA_ANTI_DEADZONE + (1.0f - GA_ANTI_DEADZONE) * mc;
        ox = smooth_x / m * lifted;
        oy = smooth_y / m * lifted;
    }

    // ---- 6. combine with the physical stick ----
    const float sx = (float) r.RightStickX + ox * 127.5f;
    const float sy = (float) r.RightStickY + oy * 127.5f;
    r.RightStickX = (uint8_t) clampf(sx + 0.5f, 0.0f, 255.0f);
    r.RightStickY = (uint8_t) clampf(sy + 0.5f, 0.0f, 255.0f);
}
