#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
DS5-Bridge Gyro-Aim v2.0 Live Telemetry & Ergonomic State Verifier
==================================================================
Prints real-time 500Hz HID Input Report 0x01 telemetry for 30 seconds:
  - L2 / R2 analog triggers & Right Stick (RX, RY)
  - 6-Axis IMU: Gyro (Pitch, Yaw, Roll) & Accel (X, Y, Z) + 3MHz SensorTimestamp
  - Touchpad Finger[0]: Contact state, 12-bit (X, Y), Right-Half R3 zone (X >= 960)
  - Buttons: R3 (Physical / Touchpad-Mapped), Circle (O Grenade Hold), Create, PadClick, UNK2 Boost Hook
"""

import ctypes
import struct
import time
import winreg

k32 = ctypes.windll.kernel32
GUID = "{4d1e55b2-f16f-11cf-88cb-001111000030}"


def find_path():
    with winreg.OpenKey(winreg.HKEY_LOCAL_MACHINE, r"SYSTEM\CurrentControlSet\Enum\HID") as root:
        i = 0
        while True:
            try:
                sub = winreg.EnumKey(root, i)
            except OSError:
                return None
            i += 1
            if "VID_054C" in sub.upper() and ("PID_0CE6" in sub.upper() or "PID_0DF2" in sub.upper()):
                with winreg.OpenKey(root, sub) as dev:
                    j = 0
                    while True:
                        try:
                            inst = winreg.EnumKey(dev, j)
                        except OSError:
                            break
                        j += 1
                        p = f"\\\\?\\HID#{sub}#{inst}#{GUID}"
                        h = k32.CreateFileW(p, 0x80000000, 3, None, 3, 0, None)
                        if h not in (-1, 0xFFFFFFFFFFFFFFFF):
                            return h


def main():
    h = find_path()
    if not h:
        raise SystemExit("DualSense (DS5-Bridge VID_054C) not found. Ensure controller is paired or connected.")

    print("======================================================================================================")
    print(" DS5-Bridge Gyro-Aim v2.0 Live Telemetry (30s) | Shortcuts: Create+DPad Up/Down (Mode), L/R (Sens)   ")
    print("======================================================================================================")

    buf = (ctypes.c_ubyte * 64)()
    n = ctypes.c_ulong()
    t_end = time.time() + 30.0
    last = 0.0

    while time.time() < t_end:
        k32.ReadFile(h, ctypes.byref(buf), 64, ctypes.byref(n), None)
        b = bytes(buf[: n.value])
        if len(b) < 40 or b[0] != 0x01 or (time.time() - last) < 0.08:
            continue
        last = time.time()

        rx, ry = b[3], b[4]
        l2, r2 = b[5], b[6]
        btn_b8, btn_b9, btn_b10 = b[8], b[9], b[10]
        unk2 = b[11]

        circle_pressed = bool(btn_b8 & 0x40)
        create_pressed = bool(btn_b9 & 0x10)
        r3_pressed = bool(btn_b9 & 0x80)
        pad_clicked = bool(btn_b10 & 0x02)

        # 6-Axis IMU (Report 0x01 offsets: +1 from USBGetStateData struct offsets)
        gp, gy_yaw, gr_roll = struct.unpack_from("<hhh", b, 16)
        ax, ay, az = struct.unpack_from("<hhh", b, 22)
        sensor_ts = struct.unpack_from("<I", b, 28)[0]

        # Touchpad Finger 0 (USBGetStateData offset 32 -> Report 0x01 offset 33)
        t0_byte0 = b[33]
        touch_active = (t0_byte0 & 0x80) == 0
        touch_x = b[34] | ((b[35] & 0x0F) << 8)
        touch_y = (b[35] >> 4) | (b[36] << 4)
        touch_zone = "R-HALF(R3)" if (touch_active and touch_x >= 960) else ("L-HALF    " if touch_active else "IDLE      ")

        flags = []
        if l2 >= 30 or r2 >= 30:
            flags.append("COMBAT")
        if circle_pressed:
            flags.append("O-GRENADE")
        if r3_pressed:
            flags.append("R3")
        if pad_clicked:
            flags.append("MAP-PAD")
        if create_pressed:
            flags.append("CREATE")
        if (unk2 & 0xF0) == 0xA0:
            flags.append(f"BOOST(0x{unk2:02X})")
        flag_str = "|".join(flags) if flags else "IDLE"

        print(
            f"L2={l2:3d} R2={r2:3d} | RX={rx:3d} RY={ry:3d} | "
            f"Gyro(P,Y,R)=({gp:6d},{gy_yaw:6d},{gr_roll:6d}) Accel=({ax:6d},{ay:6d},{az:6d}) | "
            f"Touch={touch_zone}({touch_x:4d},{touch_y:4d}) | [{flag_str}] (ts={sensor_ts})"
        )

    k32.CloseHandle(h)


if __name__ == "__main__":
    main()
