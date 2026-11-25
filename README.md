# 🎮 DS5-Bridge Gyro-Aim v2.0 — 250MHz 6-Axis Quaternion & PD Lead Control Firmware

基于树莓派 **Raspberry Pi Pico 2 W (`RP2350`)** 的索尼 PS5 DualSense / DualSense Edge 无线接收器 **v2.0 终极体感控制架构固件**。

---

## 🌟 v2.0 架构核心特性

### 1. 底层硬件、250MHz 超频与反作弊安全约束
- **纯正原装手柄身份（100% 反作弊安全）**：向 PC 呈现为唯一一个合法的**原装有线 DualSense 手柄（`VID: 054C, PID: 0CE6`）**，**零额外 USB Endpoint、零驱动拦截、零常驻瞄准软件**，完美通过 EAC / BattlEye / Vanguard 等内核级反作弊检测，绝无键鼠/手柄图标闪烁冲突。
- **250MHz 安全超频与单精度 FPU 加速**：
  - 调用 `set_sys_clock_khz(250000, true)`（配合 `VREG_VOLTAGE_1_20`、`PICO_FLASH_SPI_CLKDIV = 2` 与 `CYW43_PIO_CLOCK_DIV = 3`），将 RP2350 双核 Cortex-M33 稳定超频至 **250 MHz**，蓝牙与 HD 触觉音频零爆音；
  - 全部控制律严格使用 `float` 单精度硬件 FPU 指令（`sqrtf`, `powf`, `fabsf`），驻留 SRAM `.time_critical` 段（`__not_in_flash_func`），**零动态内存分配（0 Heap Allocations）**；
  - 默认保持 **`500Hz`（`bInterval = 2`）** 低抖动 USB HID 报告轮询率。
- **原生特性 100% 无损透传**：USB 四声道音频的 **HD 触觉反馈（Haptics）**、手柄扬声器、**R2 原生自适应扳机特效** 以及**物理按下触摸板按键（`ButtonPad` 打开地图）** 100% 原样直通。
- **微星雷影 17 (MSI Alpha 17) 狂暴散热与内存清理联动 Hook**：完整保留 `host_boost_hook_apply`，在连接建立及按下 **`L3 + R3 + L1 + R1`** 时通过 HID `0x01` 报告 `UNK2` 保留字节发送触发脉冲并闪烁冰蓝色灯条。

---

### 2. 四种体感激活模式与 `Create + D-Pad` 在线热切换

无需重刷固件或打开任何上位机，游戏中随时使用 **`Create (Share) + 十字键`** 切换模式与调节灵敏度（触发时自动吞掉组合键、静默体感 `80ms` 并清零积分/微分历史）：

| 快捷键 | 功能说明 | 灯条反馈 |
| :--- | :--- | :--- |
| **`Create + D-Pad Up / Down`** | 循环切换 4 种 `ActivationMode`（Mode 1 ~ Mode 4） | 闪烁对应模式颜色 `300ms` |
| **`Create + D-Pad Left / Right`** | 以 `0.10f` 步进实时调节总灵敏度倍率 `user_sens_scale`（`0.50f ~ 2.00f`） | 闪烁纯白光 `220ms` |
| **`L3 + R3 + L1 + R1`** | 触发 PC 端 MSI Cooler Boost 满速风扇 & RAMMap 内存清理 | 闪烁冰青蓝 `600ms` |

#### 四种激活模式（`ActivationMode`）详解：
1. **Mode 1：`kCombatAndGrenade`（默认，翠绿灯提示）**
   - `L2 >= 30` 或 `R2 >= 30`（滞回释放 `< 18`，无阻力墙）轻触即激活枪械精瞄档 `kCombatProfile`；
   - 长按 `○` 键（`>= 80ms`，松开后含 `150ms` 尾迹缓冲）或举枪下挂榴弹态自动无缝切入广域抛物线档 `kGrenadeProfile`；
   - 全局启用右半区触摸板电容轻触映射 `R3`。
2. **Mode 2：`kSoftL2Only`（宝蓝灯提示）**
   - 仅在 `L2 >= 30`（释放 `< 18`）轻触开镜时激活体感，无扳机阻力墙，腰射 `R2` 不触发体感。
3. **Mode 3：`kAlwaysOnSmartStick`（琥珀黄灯提示）**
   - 常开体感瞄准 + **右摇杆智能离合**：当大拇指大幅推右摇杆转视角（模长 `> 35%`）时自动将体感抑制为 `0` 并清零漏积分器。
4. **Mode 4：`kClassicTwoStage`（深红灯提示）**
   - 经典 v0.7.2 电控二段阻尼墙：在 L2 行程 `145 ~ 205` 生成高强度机械段落阻尼墙，轻按常规摇杆开镜，重按突破阻尼墙（`L2 >= 230`）灯条变深红并开启体感。

---

### 3. 全局右半区触摸板电容映射 `R3` 与上下文化下挂榴弹联动

- **右半区防误触识别**：解析 `0x31` 报告第一触点接触位与 12-bit `X` 坐标（`0 ~ 1919`）；仅当手指接触 **右半区（`X >= 960`）** 时触发（防止左手推左摇杆时虎口误触左半区）。
- **轻触即按 `R3`（锁定 / 处决 / 近战）**：无需费力按压摇杆帽，右手拇指轻触右半区触摸板即输出 `R3`（内置 `50ms` 最小保底脉冲，与物理 `R3` 逻辑或合并；物理按下触摸板按键仍正常打开地图）。
- **`60ms` 落指防抖静默**：每次轻触右触摸板上升沿自动静默陀螺仪输出 `60ms`，彻底吸收拇指移向触摸板瞬间的手部微颤。
- **上下文化下挂榴弹联动**：
  - **未按 `L2` 时**轻触右触摸板：仅触发 `R3`（锁定敌人或处决）；
  - **按住 `L2 >= 30`（举枪开镜状态）时**轻触右触摸板：触发 `R3` 切换下挂榴弹的同时，自动翻转 `underbarrel_gl_active` 状态，将体感无缝切换为高仰角抛物线档 `kGrenadeProfile`（松开 `L2 < 18` 退出举枪时自动复位为常规枪械档）。

---

### 4. 移植自 `Motion Studio (JSM/FlexInput)` + `GamepadMotion` + `XY-CtoPS5` 的工业级体感管线

彻底摒弃破坏手眼肌肉记忆的“四元数全轴旋转、PD 微分放大、漏积分拖尾、扣扳机砍增益、过零回弹砍增益”，100% 遵循业界成熟开源方案（`hotuns/MotionStudio`、`JibbSmart/GamepadMotionHelpers`、`SWTiCC/XY-CtoPS5`）：

#### (1) 全局唯一的游戏引擎右摇杆逆向标定层（`OutStickCalibration kGameStickCal`）
| 参数字段 | 默认标定值 | 物理意义（对应 `Motion Studio` `pad::OutStick`） |
| :--- | :---: | :--- |
| `calibration_dps` | `240.0f °/s` | `VIRTUAL_STICK_CALIBRATION`：右摇杆推满 100% 对应的游戏内视角最大转速 |
| `undeadzone_inner` | `0.13f` | `RIGHT_UNDEADZONE_INNER`：游戏右摇杆 13% 内死区逆抬升 |
| `undeadzone_outer` | `0.03f` | `RIGHT_UNDEADZONE_OUTER`：游戏右摇杆外圈饱和余量 |
| `unpower` | `1.30f` | `RIGHT_UNPOWER`：通过 `powf(strength, 1.0f / unpower)` 抵消游戏右摇杆的幂次加速曲线 |
| `max_stick_out` | `0.92f` | 避开部分主机游戏 `>92%` 外圈转身突变加速陷阱 |

#### (2) 基础射击层 + 投掷动作覆盖层（`Motion Studio` `Base + Layer Modeshift` 架构）
| 参数字段 | `kCombatProfile`（普通射击基础层） | `kGrenadeProfile`（手雷/下挂榴弹覆盖层） | 物理意义 |
| :--- | :---: | :---: | :--- |
| `min_sens` / `max_sens` | `1.00x` -> `1.65x` | `1.55x` -> `2.25x` | 微调区 **1:1 纯线性灵敏度** / 高速甩动上限倍率 |
| `min_threshold_dps` / `max_threshold_dps` | `15.0` / `75.0 °/s` | `10.0` / `65.0 °/s` | 双段线性加速起点 / 满额转速 |
| `yx_ratio` | **`0.60f`**（水平扫射防上下晃） | **`1.00f`**（1:1 垂直等比大仰角抛物线） | 垂直(Pitch)相对水平(Yaw)灵敏度比例 |
| `cutoff_speed_dps` / `cutoff_recovery_dps` | `0.55` / `1.80 °/s` | `0.40` / `1.40 °/s` | JSM 静止底噪截止速度 / 线性淡入恢复转速 |
| `smooth_time_sec` / `smooth_threshold_dps` | `12 ms` / `45.0 °/s` | `10 ms` / `40.0 °/s` | `Decay::smooth` 零残留衰减平滑窗口（急停零拖尾） |
| `one_euro_min_cutoff` / `one_euro_beta` | `5.5 Hz` / `0.30` | `6.5 Hz` / `0.35` | `One-Euro Filter` 一欧元速度自适应滤波器 |
| `roll_mix` | `0.15f` | `0.20f` | `XY-CtoPS5` / JSM 方向盘横滚辅助转向混入比例 |

#### (3) 核心算法五大支柱（实现于 `src/gyro_aim.cpp`）：
1. **`GamepadMotion::SensorMinMaxWindow` 稳健静止校准**：采集 `0.5s` 窗口内三轴陀螺仪与加速度计的峰峰值（`Max - Min`），仅在真静止时向窗口均值 `MeanGyro` 收敛，绝不在匀速慢速跟枪时误吞瞄准输入。
2. **`GamepadMotion::CalculatePlayerSpaceGyro` 正交玩家空间**：**Pitch（俯仰）100% 保持手柄本地 X 轴**（彻底杜绝重力加速度计与手柄震动污染垂直压枪）；**Yaw（水平转向）**投影至重力平面并乘以 `1.41f`（$45^\circ$ 握持倾角宽容度）以 `hypot(gyroY, gyroZ)` 封顶，平端或翘起 $45^\circ$ 握持均保持 100% 水平转向灵敏度。
3. **`Motion Studio` `OneEuro` + `Decay::smooth` 零拖尾平滑**：高速甩枪时 `smoothed_share -> 0`，高速分量不充入低通滤波器电容而走 `immediate_share` 直出，手腕急停瞬间滤波器内部零残余电荷，准星急停即停。
4. **`Motion Studio` `pad::virtual_stick` 角速度域合成（Rate-Domain Mixing）**：先将物理右摇杆推杆量按 `unpower` 正向还原为游戏内视角转速 $\boldsymbol{\omega}_{\text{stick}}$（$^\circ/\text{s}$），在**角速度域**与陀螺仪角速度 $\boldsymbol{\omega}_{\text{gyro}}$ 直接相加，最后统一做唯一一次反死区与逆幂次映射，彻底消除“推右摇杆+体感”时的双重反死区跳变。

---

## 🛠️ 编译与实时遥测验证

### 1. 一键本地编译（WSL2 Ubuntu）
```bash
wsl bash /mnt/c/Users/cgnnb/.gemini/antigravity/scratch/DS5-Bridge-GyroAim/build_firmware.sh
```
编译产物自动输出至：`firmware/ds5-bridge-v0.7.2-gyroL2.uf2`。

### 2. 实时遥测与按键/触摸板校验
```powershell
python tools/ds5_gyro_stick_check.py
```
可实时观测 `L2/R2`、`RX/RY`、6 轴 `Gyro/Accel`、右半区触摸板 `R-HALF(R3)` 坐标触发、`O-GRENADE` 状态及 `BOOST` 脉冲。
