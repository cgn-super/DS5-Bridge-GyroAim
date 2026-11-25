//
// DS5-Bridge Gyro-Aim v2.0 — Ultimate 6-Axis IMU Attitude & PD Lead Control Architecture
// Target: Raspberry Pi Pico 2 W (RP2350 Cortex-M33 Single-Precision FPU @ 250 MHz)
//
// Strict Constraints:
//   - Genuine DualSense USB Identity (VID: 054C, PID: 0CE6), zero extra USB endpoints.
//   - 100% pass-through for HD Haptics, controller speaker, R2 adaptive effects, and ButtonPad.
//   - Zero dynamic memory allocation, zero compiler warnings.
//

#pragma once

#include <cstdint>

#ifndef DS5_USB_POLL_INTERVAL_MS
#define DS5_USB_POLL_INTERVAL_MS 2 // Default 500Hz USB HID polling interval (bInterval = 2)
#endif

// Classic Two-Stage Mechanical Wall Constants (for ActivationMode::kClassicTwoStage)
#ifndef GA_L2_ON
#define GA_L2_ON 230
#endif
#ifndef GA_L2_OFF
#define GA_L2_OFF 200
#endif
#ifndef GA_WALL_START
#define GA_WALL_START 145
#endif
#ifndef GA_WALL_END
#define GA_WALL_END 205
#endif
#ifndef GA_WALL_FORCE
#define GA_WALL_FORCE 245
#endif

// Soft Trigger Hysteresis Thresholds (for Mode 1 & Mode 2)
#ifndef GA_SOFT_TRIG_ON
#define GA_SOFT_TRIG_ON 30
#endif
#ifndef GA_SOFT_TRIG_OFF
#define GA_SOFT_TRIG_OFF 18
#endif

enum class ActivationMode : uint8_t {
    kCombatAndGrenade   = 0, // Mode 1 (默认): L2/R2 轻触射击 + 长按 O 抛物线投掷 + 右触摸板全局映射 R3
    kSoftL2Only         = 1, // Mode 2: 仅 L2 >= 30 轻触开镜激活 (无阻力墙)
    kAlwaysOnSmartStick = 2, // Mode 3: 常开体感 + 右摇杆大角度推杆 (>35%) 自动抑制离合
    kClassicTwoStage    = 3  // Mode 4: 保留 v0.7.2 经典电控二段阻尼墙 (145~205 阻尼, >=230 突破)
};

struct GyroProfile {
    float full_scale_dps;  // 物理角速度满量程基准 (°/s)
    float yx_ratio;        // 垂直(Pitch)相对水平(Yaw)灵敏度比例
    float gamma;           // 逆响应曲线幂次压缩指数
    float kp_gain;         // 比例主增益
    float kd_lead_sec;     // PD 微分超前补偿时间常数 (秒)，击穿游戏镜头平滑惯性
    float v_low;           // 微速钳制与强低通滤波上限 (°/s)
    float v_high;          // 零延迟直通下限 (°/s)
    float anti_deadzone;   // 径向反死区比例
    float leaky_tau_sec;   // 短时角度误差漏积分器衰减时间常数 (秒)
    float max_stick_out;   // 陀螺仪最大摇杆占空上限 (避开游戏 85%+ 外圈转向加速陷阱)
};

// 1. 枪械精确瞄准档 (Combat Precision Profile)
inline constexpr GyroProfile kCombatProfile = {
    160.0f, // full_scale_dps (°/s)
    0.60f,  // yx_ratio
    0.70f,  // gamma
    1.0f,   // kp_gain
    0.018f, // kd_lead_sec (s)
    1.5f,   // v_low (°/s)
    15.0f,  // v_high (°/s)
    0.12f,  // anti_deadzone
    0.045f, // leaky_tau_sec (s)
    0.84f   // max_stick_out
};

// 2. 手雷/下挂榴弹抛物线广域档 (Grenade / Underbarrel GL Profile)
inline constexpr GyroProfile kGrenadeProfile = {
    95.0f,  // full_scale_dps (°/s)
    0.95f,  // yx_ratio
    0.75f,  // gamma
    1.0f,   // kp_gain
    0.012f, // kd_lead_sec (s)
    0.4f,   // v_low (°/s)
    8.0f,   // v_high (°/s)
    0.12f,  // anti_deadzone
    0.035f, // leaky_tau_sec (s)
    0.92f   // max_stick_out
};

struct USBGetStateData;
struct SetStateData;

void gyro_aim_apply(USBGetStateData &report);
void gyro_aim_on_host_output_report(SetStateData &state);
void host_boost_hook_apply(USBGetStateData &report);
