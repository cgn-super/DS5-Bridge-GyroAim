//
// DS5-Bridge Gyro-Aim v2.1 — Industry-Standard JSM / Motion Studio / GamepadMotion Pipeline
// Target: Raspberry Pi Pico 2 W (RP2350 Cortex-M33 Single-Precision FPU @ 250 MHz)
//
// Core References Ported:
//   1. Jibb Smart's GamepadMotionHelpers (SensorMinMaxWindow + CalculatePlayerSpaceGyro)
//   2. Motion Studio / FlexInput JSM Engine (OneEuro + Decay Smoother + Linear Cutoff + Layer Modeshift)
//   3. Motion Studio pad::virtual_stick (Rate-Domain Stick + Gyro Mixing in deg/s)
//   4. SWTiCC/XY-CtoPS5 (Two-segment linear sensitivity & optional Yaw+Roll mix)
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

// 全局唯一的游戏引擎右摇杆逆向标定参数 (对应 Motion Studio pad::OutStick & Settings::calibration)
// 同一款游戏内右摇杆的死区和响应曲线是引擎固有属性，绝不随枪械/手雷切换而改变。
struct OutStickCalibration {
    float calibration_dps;  // VIRTUAL_STICK_CALIBRATION: 右摇杆推满 100% 对应的游戏视角转速 (°/s)
    float undeadzone_inner; // RIGHT_UNDEADZONE_INNER: 游戏右摇杆内死区比例 (0..1)
    float undeadzone_outer; // RIGHT_UNDEADZONE_OUTER: 游戏右摇杆外圈饱和死区比例 (0..1)
    float unpower;          // RIGHT_UNPOWER: 游戏右摇杆幂次曲线逆补偿指数 (1.0=线性, 1.35=抵消常见主机游戏加速)
    float max_stick_out;    // 最大摇杆输出上限 (避开部分游戏 >90% 外圈转身突变加速区)
};

inline constexpr OutStickCalibration kGameStickCal = {
    240.0f, // calibration_dps (°/s at full stick deflection)
    0.13f,  // undeadzone_inner (13% in-game inner deadzone lift)
    0.03f,  // undeadzone_outer
    1.30f,  // unpower (linearizes game's x^1.30 stick response curve via strength^(1/unpower))
    0.92f   // max_stick_out
};

// Motion Studio / JSM 动作层参数 (Base Shooting Profile vs. Grenade / Underbarrel GL Layer)
struct GyroProfile {
    float min_sens;             // 低速微调区线性灵敏度倍率 (1:1 恒定线性，不扭曲微调肌肉记忆)
    float max_sens;             // 高速甩动区上限灵敏度倍率 (双段线性插值)
    float min_threshold_dps;    // 双段加速起点角速度 (°/s)，低于此速度保持纯线性 min_sens
    float max_threshold_dps;    // 双段加速满额角速度 (°/s)
    float yx_ratio;             // 垂直(Pitch)相对水平(Yaw)灵敏度比例
    float cutoff_speed_dps;     // GYRO_CUTOFF_SPEED: 静止底噪截止死区 (°/s)
    float cutoff_recovery_dps;  // GYRO_CUTOFF_RECOVERY: 线性淡入完全恢复转速 (°/s)
    float smooth_time_sec;      // Decay 衰减平滑器时间常数 (秒，高速时自动缩为 0，急停零拖尾)
    float smooth_threshold_dps; // 完全退出平滑直通的角速度阈值 (°/s)
    float one_euro_min_cutoff;  // One-Euro Filter 最小截止频率 (Hz)
    float one_euro_beta;        // One-Euro Filter 速度自适应开度系数
    float roll_mix;             // XY-CtoPS5 / JSM ROLL_CONTRIBUTION: 方向盘横滚混入比例
};

// 1. 普通射击基础层 (Combat Base Layer — 沉稳锁头 + 水平微调防上下晃动)
inline constexpr GyroProfile kCombatProfile = {
    1.00f,  // min_sens (微调区 1:1 纯线性)
    1.65f,  // max_sens (快速甩枪平滑提速)
    15.0f,  // min_threshold_dps (°/s)
    75.0f,  // max_threshold_dps (°/s)
    0.60f,  // yx_ratio (JSM 经典 0.60 垂直比，水平扫射准星不上下飘)
    0.55f,  // cutoff_speed_dps (°/s)
    1.80f,  // cutoff_recovery_dps (°/s)
    0.012f, // smooth_time_sec (12ms Decay Smoother，急停零残留)
    45.0f,  // smooth_threshold_dps (°/s)
    5.5f,   // one_euro_min_cutoff (Hz)
    0.30f,  // one_euro_beta
    0.15f   // roll_mix (15% 方向盘倾斜辅助转向)
};

// 2. 手雷 / 下挂榴弹覆盖层 (Grenade Modeshift Layer — 1:1 垂直大仰角抛物线 + 广域转向)
inline constexpr GyroProfile kGrenadeProfile = {
    1.55f,  // min_sens (广域快速转移投掷落点)
    2.25f,  // max_sens
    10.0f,  // min_threshold_dps (°/s)
    65.0f,  // max_threshold_dps (°/s)
    1.00f,  // yx_ratio (1:1 垂直等比，手腕轻抬即可拉出高仰角抛物线)
    0.40f,  // cutoff_speed_dps (°/s)
    1.40f,  // cutoff_recovery_dps (°/s)
    0.010f, // smooth_time_sec (10ms)
    40.0f,  // smooth_threshold_dps (°/s)
    6.5f,   // one_euro_min_cutoff (Hz)
    0.35f,  // one_euro_beta
    0.20f   // roll_mix
};

struct USBGetStateData;
struct SetStateData;

void gyro_aim_apply(USBGetStateData &report);
void gyro_aim_on_host_output_report(SetStateData &state);
void host_boost_hook_apply(USBGetStateData &report);
