# 🎮 DS5-Bridge Gyro-Aim & Two-Stage Adaptive Trigger

[简体中文](#-简体中文) | [English](#-english) | [日本語](#-日本語) | [Español](#-español) | [Français](#-français)

---

## 🇨🇳 简体中文

基于树莓派 Pico 2 W (`RP2350`) 的索尼 PS5 DualSense / DualSense Edge 无线接收器扩展固件。

### 🌟 核心特性与独创机制

1. **虚拟电控“机械二段卡手”段落墙（Two-Stage Adaptive Trigger Wall）**：
   - 彻底摒弃传统精英手柄在后壳打孔加塑料拨片的粗糙改装方式；
   - 巧妙利用 DualSense 内部的电控自适应扳机马达，在 L2 行程约 55% 位置（`145 ~ 205`）程序化生成一道 **高强度机械段落阻尼墙**；
   - **轻按 L2（0 ~ 55%）**：阻尼轻盈，像微单相机“半按对焦”一样进行常规摇杆开镜；
   - **重按突破阻尼墙（80% ~ 100%，`L2 >= 230`）**：手指清晰感受到“咔哒”击穿机械段落感，手柄灯条秒变**深红锁定指示灯**，瞬时强开底层陀螺仪瞄准！
   - **松开 L2（`< 200`，带滞后防抖）**：陀螺仪静默，手柄灯条无缝恢复游戏原本的设定颜色。

2. **硬件级右摇杆注入（Gyro-to-Right-Stick）**：
   - 陀螺仪角速度在芯片固件底层经过 **零偏校准 + 径向软阈值死区 + EMA 指数平滑 + 18% 径向反死区提升** 后，直接加算进原装手柄报告的右摇杆（`RightStickX / RightStickY`）；
   - 电脑只识别为**唯一一个合法的纯原装有线 PS5 手柄**（`VID: 054C, PID: 0CE6`）；
   - **零驱动拦截、零软件驻留、不封号**，绝不会出现手柄/鼠标同时输入导致的画面与按键提示符抽搐卡顿！

3. **原生游戏特性 100% 完美无损**：
   - **HD 触觉反馈（Haptics 细腻震感）** 与手柄扬声器走独立 USB 音频通道，不受任何影响；
   - **R2 右扳机开火特效** 100% 直通给游戏；
   - 若进入支持原生 L2 特效的索尼大作，芯片智能识别并让位给游戏；待游戏松开特效后自动挂载二段段落墙。

4. **微星雷影 17 (MSI Alpha 17) 插入自动狂暴风扇 & RAMMap 内存清理 Hook（100% 反作弊安全）**：
   - **固件内嵌无感 Hook（`host_boost_hook_apply`）**：不增加任何额外 USB 接口，利用标准 HID `0x01` 报告的 `UNK2` 保留字节在手柄连接时及按下 **`L3 + R3 + L1 + R1`** 组合键时发送触发脉冲（手柄灯条闪烁冰蓝色确认）；
   - **零内核驱动风险**：PC 侧守护脚本 `tools/ds5_msi_auto_boost.py` 直接通过微星原厂正版签名服务 `MSI.CentralServer.exe`（本地 TCP IPC）开启 **Cooler Boost 满速风扇**，绝不加载 `WinRing0` 等高危第三方驱动；
   - **安全白名单杀后台 + 微软官方 RAMMap 联动**：自动清理高占用闲置后台进程，并调用微软 Sysinternals `RAMMap-64bit.exe` (`-Ew / -Et`) 释放物理内存工作集与备用列表；拔出接收器可自动恢复风扇自动温控。

### 📁 目录结构

* `firmware/`
  * `ds5-bridge-v0.7.2-gyroL2.uf2`：即刷即用的最终定制固件。
  * `ds5-bridge-v0.7.2-official-stock.uf2`：官方原版固件，便于随时刷回出厂。
* `src/`
  * `gyro_aim.h` & `gyro_aim.cpp`：500Hz 陀螺仪姿态解算、二段阻尼发生器与主机狂暴联动 Hook (`host_boost_hook_apply`) 核心 C++ 源码。
  * `patch_against_official_v0.7.2.patch`：对官方 `DS5Dongle v0.7.2` 的一键 Git 补丁。
* `tools/`
  * `ds5_msi_auto_boost.py` & `start_msi_auto_boost.bat`：微星雷影 17 自动满速风扇 (Cooler Boost) + 杀闲置后台 + RAMMap 物理内存释放守护工具。
  * `ds5_gyro_stick_check.py`：实时右摇杆与陀螺仪数值检测工具。
  * `ds5_gyro_hud.html` & `ds5_gyro_mouse_mode_engine.py`：PC 端高精压枪靶场与可视化调参工具。

---

## 🇬🇧 English

Custom firmware for Raspberry Pi Pico 2 W (`RP2350`) acting as an ultra-low-latency wireless USB bridge for Sony PlayStation 5 DualSense / DualSense Edge controllers.

### 🌟 Key Innovations & Features

1. **Programmable Two-Stage Adaptive Trigger Wall**:
   - Replaces crude physical plastic trigger stop switches with DualSense's internal force-feedback servomotors.
   - Creates a crisp mechanical resistance wall at ~55% trigger travel (`145 ~ 205`).
   - **Soft Pull (0 ~ 55%)**: Normal smooth trigger stroke for regular thumbstick aiming (similar to half-pressing a camera shutter button to focus).
   - **Hard Break-Through (80% ~ 100%, `L2 >= 230`)**: Clear tactile "click" sensation breaking past the resistance wall. The lightbar instantly turns **tactical crimson red**, activating 500Hz hardware gyro aiming!
   - **Release (`< 200`, with hysteresis)**: Disengages gyro aiming and seamlessly restores the in-game lightbar color.

2. **Native Hardware Gyro-to-Right-Stick Fusion**:
   - The gyro angular rate is processed on-chip via **zero-bias auto-calibration + radial continuous soft deadzone + EMA smoothing + 18% radial anti-deadzone boost**, and added directly to `RightStickX / RightStickY`.
   - Windows recognizes the device as **a single genuine wired DualSense controller** (`VID: 054C, PID: 0CE6`).
   - **Zero PC background processes required for aiming, anti-cheat safe, no input flickering** caused by simultaneous gamepad/mouse inputs.

3. **Zero Impact on Native PS5 Controller Features**:
   - **HD Haptics (dual voice-coil haptic motors)** and controller audio stream untouched through the native USB audio interface.
   - **R2 trigger effects** remain 100% under the game's control.
   - When playing official games that send custom L2 adaptive effects, the firmware automatically yields; once the game releases L2, the two-stage wall automatically rearms.

4. **MSI Alpha 17 Auto Cooler Boost & RAMMap Memory Cleaner Hook (100% Anti-Cheat Safe)**:
   - **In-Band Firmware Hook (`host_boost_hook_apply`)**: Uses the reserved `UNK2` byte in standard HID Input Report `0x01` without adding any extra USB endpoints. Signals on initial controller link and whenever **`L3 + R3 + L1 + R1`** is pressed (with an Ice Cyan lightbar flash).
   - **Driverless Fan Control & Memory Purge**: `tools/ds5_msi_auto_boost.py` communicates with the official signed `MSI.CentralServer.exe` local TCP IPC to enable maximum fan speed (**Cooler Boost ON**), terminates whitelisted background bloatware, and invokes Microsoft Sysinternals `RAMMap-64bit.exe` (`-Ew / -Et`) to free physical RAM.

---

## 🇯🇵 日本語

Raspberry Pi Pico 2 W (`RP2350`) を使用し、PS5 DualSense / DualSense Edge コントローラーを PC で超低遅延ワイヤレス接続するための高機能カスタムファームウェアです。

### 🌟 主な特徴と革新技術

1. **電子制御式 2段階アダプティブトリガー・ウォール（Two-Stage Wall）**：
   - 従来のエリートコントローラーのように物理的なストッパー改造を施すことなく、DualSense 内蔵のモーターを活用して L2 トリガーの約 55%（`145 ~ 205`）に明確な**機械的クリック感（段落抵抗）**を生成します。
   - **浅押し（0 〜 55%）**：カメラの「半押しAF」のようにスムーズな通常エイム操作。
   - **押し込み突破（80 〜 100%、`L2 >= 230`）**：抵抗をカチッと押し抜けると、ライトバーが即座に**深紅（ロックオン）**に変化し、500Hz の超低遅延ジャイロエイムが発動！
   - **リリース（`< 200`、ヒステリシス制御）**：ジャイロがオフになり、ゲーム本来の LED カラーへ自動復帰。

2. **ハードウェア直接マッピング（ジャイロ → 右スティック）**：
   - 取得した角速度は、マイコン内部で**ゼロ点自動補正 ＋ 連続ソフトデッドゾーン ＋ EMA スムージング ＋ 18% ラジアル反デッドゾーン補正**を経て、右スティック（`RightStickX / RightStickY`）へ直接合成されます。
   - PC 側からは**正真正銘の純正 PS5 有線コントローラー**として認識されます。
   - **チート対策（アンチチート）安全・ゲーム内のキーマウ/パッド表示のチラつきなし**！

3. **PS5 純正機能（HDハプティクス / トリガー）との完全両立**：
   - **HD ハプティクス（繊細な触覚振動）**やコントローラースピーカーは独立した USB オーディオ経路を通るため、一切劣化しません。
   - **R2（射撃トリガー）**のゲーム側エフェクトは 100% そのまま機能します。

4. **MSI Alpha 17 自動 Cooler Boost ＆ RAMMap メモリ解放フック**：
   - 接続時および **`L3 + R3 + L1 + R1`** 同時押し時に、MSI 公式 IPC 経由でファンを最大回転（Cooler Boost）に切り替え、Microsoft 公式 `RAMMap` と連携して物理メモリを自動解放します。

---

## 🇪🇸 Español

Firmware personalizado para Raspberry Pi Pico 2 W (`RP2350`) que funciona como un puente inalámbrico USB de latencia ultra baja para mandos Sony PS5 DualSense / DualSense Edge en PC.

### 🌟 Innovaciones y Características Principales

1. **Muro de Resistencia Mecánica de Dos Etapas en L2 (Two-Stage Adaptive Trigger)**:
   - Elimina la necesidad de modificaciones físicas o topes de plástico en el gatillo. Utiliza los servomotores de retroalimentación de fuerza del propio DualSense para generar un muro mecánico configurable al ~55% del recorrido (`145 ~ 205`).
   - **Pulsación Suave (0 ~ 55%)**: Recorrido suave para apuntado tradicional con el stick (similar al enfoque de media pulsación en cámaras).
   - **Pulsación Fuerte / Ruptura (80% ~ 100%, `L2 >= 230`)**: Se supera la resistencia física con una sensación de "clic" mecánico. La barra de luz cambia inmediatamente a **rojo carmesí táctico**, activando el apuntado giroscópico a 500 Hz en tiempo real.
   - **Liberación (`< 200`, con histéresis)**: Desactiva el giroscopio y restaura automáticamente el color RGB original del juego.

2. **Fusión de Giroscopio a Stick Derecho en Hardware (Gyro-to-Right-Stick)**:
   - La velocidad angular del giroscopio se filtra en el propio chip con **calibración de deriva cero + zona muerta radial suave + suavizado EMA + compensación de zona muerta del 18%**, inyectándose directamente en `RightStickX / RightStickY`.
   - El PC detecta el dispositivo como un **mando DualSense original con cable** (`VID: 054C, PID: 0CE6`).
   - **Seguro contra antitrampas (anti-cheat safe) y sin parpadeos de interfaz** por conflicto entre ratón y mando.

3. **Compatibilidad Total con Funciones Nativas**:
   - La **vibración háptica HD** y el altavoz del mando pasan intactos por la interfaz de audio USB nativa.
   - El gatillo derecho **R2** mantiene íntegros todos los efectos adaptativos del juego.

4. **Hook Automático de Cooler Boost (MSI Alpha 17) y Limpieza de Memoria RAMMap**:
   - Activa automáticamente los ventiladores al máximo (Cooler Boost) mediante el IPC oficial de MSI Center y libera memoria RAM física con Microsoft `RAMMap` al conectar el receptor o pulsar **`L3 + R3 + L1 + R1`**.

---

## 🇫🇷 Français

Firmware sur mesure pour Raspberry Pi Pico 2 W (`RP2350`) servant de pont sans fil USB à ultra-faible latence pour les manettes Sony PS5 DualSense / DualSense Edge sur PC.

### 🌟 Innovations et Fonctionnalités Clés

1. **Mur de Résistance Électromécanique à Deux Paliers sur L2**:
   - Remplace les butées physiques en plastique grossières en exploitant les servomoteurs de retour d'effort internes de la DualSense. Génère un mur de résistance mécanique net à environ 55% de course (`145 ~ 205`).
   - **Pression Légère (0 ~ 55%)**: Course fluide pour la visée traditionnelle au joystick (similaire au déclencheur à mi-course d'un appareil photo).
   - **Franchissement Franc (80% ~ 100%, `L2 >= 230`)**: Sensation de clic mécanique en franchissant le mur. La barre lumineuse passe instantanément au **rouge écarlate tactique**, activant la visée gyroscopique matérielle à 500 Hz.
   - **Relâchement (`< 200`, avec hystérésis)**: Coupe le gyroscope et rétablit instantanément la couleur LED originale définie par le jeu.

2. **Injection Matérielle du Gyroscope vers le Joystick Droit**:
   - La vitesse angulaire du gyroscope est traitée directement sur le microcontrôleur via **autocalibrage du biais zéro + zone morte radiale progressive + lissage EMA + compensation de zone morte de 18%**, puis injectée dans `RightStickX / RightStickY`.
   - Windows ne voit qu'une **authentique manette filaire officielle DualSense** (`VID: 054C, PID: 0CE6`).
   - **Sans risque de ban anti-triche, et aucune saccade d'interface** causée par un conflit manette/souris.

3. **Préservation Totale des Technologies PS5 Natives**:
   - Les **retours haptiques HD** et le haut-parleur intégré sont préservés à 100% via le canal audio USB dédié.
   - La gâchette droite **R2** conserve intacts tous les effets de tir envoyés par le jeu.

4. **Hook Automatique Cooler Boost (MSI Alpha 17) & Purge Mémoire RAMMap**:
   - Déclenche automatiquement la vitesse maximale des ventilateurs via l'IPC officiel MSI Center et libère la mémoire vive avec Microsoft `RAMMap` dès la connexion ou via le raccourci **`L3 + R3 + L1 + R1`**.

---

## 🛠️ Build & Compile / 编译指南

```bash
git clone https://github.com/awalol/DS5Dongle.git
cd DS5Dongle
git checkout v0.7.2-hotfix
git submodule update --init --recursive

# Apply our custom gyro & two-stage trigger patch
git apply patch_against_official_v0.7.2.patch

# Build using Ninja & ARM Toolchain
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build -j 16
```

## 🚀 MSI Alpha 17 (微星雷影17) 自动风扇 & 内存清理守护用法

```powershell
# 方式 1：直接双击运行（自动申请管理员权限以支持 RAMMap 静默释放内存，拔出接收器自动恢复风扇温控）
tools\start_msi_auto_boost.bat

# 方式 2：终端单次立即执行（风扇拉满 6486 RPM + 杀闲置后台 + RAMMap 清理后退出）
python tools\ds5_msi_auto_boost.py --once --uac-rammap

# 方式 3：将风扇恢复为自动温控模式 (Cooler Boost OFF)
python tools\ds5_msi_auto_boost.py --fan-auto
```

## 📜 License / 开源协议

Released under the **MIT License**. Based upon and derived from [awalol/DS5Dongle](https://github.com/awalol/DS5Dongle).
