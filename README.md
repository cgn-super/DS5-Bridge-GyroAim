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

### 4. 双物理量纲档案与 6 轴四元数 + PD 超前控制律

#### 预设双档案参数（定义于 `src/gyro_aim.h`）：

| 参数字段 | `kCombatProfile`（枪械精确瞄准档） | `kGrenadeProfile`（手雷/下挂榴弹广域档） | 物理意义 |
| :--- | :---: | :---: | :--- |
| `full_scale_dps` | `160.0f °/s` | `95.0f °/s` | 满量程角速度基准 |
| `yx_ratio` | `0.60f` | `0.95f` | 垂直(Pitch)相对水平(Yaw)灵敏度比 |
| `gamma` | `0.70f` | `0.75f` | 逆响应曲线幂次压缩指数 |
| `kp_gain` | `1.00f` | `1.00f` | 比例主增益 |
| `kd_lead_sec` | `0.018f s` | `0.012f s` | PD 微分超前时间常数（击穿镜头平滑惯性） |
| `v_low` / `v_high` | `1.5f` / `15.0f °/s` | `0.4f` / `8.0f °/s` | 微速强低通二次钳制上限 / 零延迟直通下限 |
| `anti_deadzone` | `0.12f` | `0.12f` | 85% 径向 + 15% 轴向混合反死区比例 |
| `leaky_tau_sec` | `0.045f s` | `0.035f s` | 短时角度误差漏积分器衰减时间常数 |
| `max_stick_out` | `0.84f` | `0.92f` | 最大摇杆占空上限（避开游戏 85%+ 外圈加速陷阱） |

#### 控制律核心管线（实现于 `src/gyro_aim.cpp`）：
1. **3MHz 硬件时钟 $\Delta t$ 与 `0x05` 出厂校准**：解析 `SensorTimestamp` 精确计算每帧 $\Delta t \in [0.0005\text{s}, 0.02\text{s}]$；零拷贝读取 `Feature Report 0x05` 出厂标定参数，配合 `0.6s` 滑动静止窗口持续收敛温漂 Bias。
2. **最短弧四元数正交玩家空间（$q_{\text{align}}$）**：仅在 $\|\mathbf{a}\| \in [0.85g, 1.15g]$ 时按可信度加权互补滤波跟踪重力向量 $\hat{\mathbf{g}}$，求解旋转至竖直轴 $(0, 1, 0)$ 的最短弧四元数 $q_{\text{align}}$，消除任意握持俯仰角下的 Yaw/Roll 串扰。
3. **壳体振动抑制 + 急停回弹阻尼 + R2 扣扳机微抑制**：
   - 实时监测高频角加速度能量，在马达强烈震动时自动收紧 `v_low`；
   - `40ms` 内出现 `> 45 °/s` 高速甩枪且随后过零反向时，启动 `35ms` `0.30f` 急停回弹阻尼，吞掉肌腱回弹；
   - `R2` 跨过 `45` 上升沿启动 `30ms` `0.25f` 恢复包络，消除第一枪扣扳机震偏。

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
