#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
RP2350 DS5-Bridge 500Hz Hardware-in-the-Loop Gyro-Aim & Two-Stage L2 Adaptive Trigger Engine
=============================================================================================
Route A Implementation:
- Connects non-exclusively to the Pico 2 W (ds5-bridge-v0.7.2-hotfix.uf2) USB DualSense HID
  interface (VID_054C & PID_0CE6) at 500 Hz (2.0 ms polling interval).
- Programs the DualSense L2 Adaptive Trigger with a physical 2-stage mechanical resistance wall
  (default wall: 140..190, activation threshold: L2 >= 195).
- Half-pressing L2 (L2 < 195): Normal in-game ADS with Right Stick, Gyro OFF (Cyan Lightbar).
- Hard-pressing L2 past the mechanical wall (L2 >= 195): Engages 500Hz Sub-Pixel EMA-Filtered
  Gyro-to-Mouse Aiming via Win32 Input Desktop injection (Crimson Red Lightbar).
- Provides a real-time HTTP telemetry & configuration API on http://127.0.0.1:8788.
"""

import ctypes
import ctypes.wintypes
import json
import math
import os
import struct
import threading
import time
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from urllib.parse import urlparse

# ---------------------------------------------------------------------------
# Win32 HID & Input Desktop Setup
# ---------------------------------------------------------------------------
kernel32 = ctypes.windll.kernel32
user32 = ctypes.windll.user32
hid = ctypes.windll.hid
setupapi = ctypes.windll.setupapi

GENERIC_READ = 0x80000000
GENERIC_WRITE = 0x40000000
FILE_SHARE_READ = 0x00000001
FILE_SHARE_WRITE = 0x00000002
OPEN_EXISTING = 3
MOUSEEVENTF_MOVE = 0x0001
MAXIMUM_ALLOWED = 0x02000000


class GUID(ctypes.Structure):
    _fields_ = [
        ("Data1", ctypes.c_ulong),
        ("Data2", ctypes.c_ushort),
        ("Data3", ctypes.c_ushort),
        ("Data4", ctypes.c_ubyte * 8),
    ]


class SP_DEVICE_INTERFACE_DATA(ctypes.Structure):
    _fields_ = [
        ("cbSize", ctypes.c_ulong),
        ("InterfaceClassGuid", GUID),
        ("Flags", ctypes.c_ulong),
        ("Reserved", ctypes.c_void_p),
    ]


class SP_DEVICE_INTERFACE_DETAIL_DATA_W(ctypes.Structure):
    _fields_ = [
        ("cbSize", ctypes.c_ulong),
        ("DevicePath", ctypes.c_wchar * 512),
    ]


setupapi.SetupDiGetClassDevsW.restype = ctypes.c_void_p
setupapi.SetupDiDestroyDeviceInfoList.argtypes = [ctypes.c_void_p]
setupapi.SetupDiEnumDeviceInterfaces.argtypes = [
    ctypes.c_void_p,
    ctypes.c_void_p,
    ctypes.POINTER(GUID),
    ctypes.c_ulong,
    ctypes.POINTER(SP_DEVICE_INTERFACE_DATA),
]
setupapi.SetupDiGetDeviceInterfaceDetailW.argtypes = [
    ctypes.c_void_p,
    ctypes.POINTER(SP_DEVICE_INTERFACE_DATA),
    ctypes.c_void_p,
    ctypes.c_ulong,
    ctypes.POINTER(ctypes.c_ulong),
    ctypes.c_void_p,
]


def find_ds5_hid_path():
    """Locate the connected DualSense / DS5-Bridge HID device path."""
    import winreg
    # Fast registry enumeration of active HID device interfaces for VID_054C & PID_0CE6 / 0DF2
    guid_str = "{4d1e55b2-f16f-11cf-88cb-001111000030}"
    guid = GUID()
    hid.HidD_GetHidGuid(ctypes.byref(guid))
    hdev = setupapi.SetupDiGetClassDevsW(ctypes.byref(guid), None, None, 0x02 | 0x10)
    if hdev and hdev != -1 and hdev != 0xFFFFFFFFFFFFFFFF:
        try:
            idx = 0
            while True:
                did = SP_DEVICE_INTERFACE_DATA()
                did.cbSize = ctypes.sizeof(SP_DEVICE_INTERFACE_DATA)
                if not setupapi.SetupDiEnumDeviceInterfaces(
                    hdev, None, ctypes.byref(guid), idx, ctypes.byref(did)
                ):
                    break
                idx += 1
                detail = SP_DEVICE_INTERFACE_DETAIL_DATA_W()
                detail.cbSize = 8 if ctypes.sizeof(ctypes.c_void_p) == 8 else 6
                req = ctypes.c_ulong(0)
                setupapi.SetupDiGetDeviceInterfaceDetailW(
                    hdev,
                    ctypes.byref(did),
                    ctypes.byref(detail),
                    ctypes.sizeof(detail),
                    ctypes.byref(req),
                    None,
                )
                path = detail.DevicePath
                if "vid_054c" in path.lower() and ("pid_0ce6" in path.lower() or "pid_0df2" in path.lower()):
                    return path
        finally:
            setupapi.SetupDiDestroyDeviceInfoList(hdev)

    # Fallback: query HKLM\SYSTEM\CurrentControlSet\Enum\HID for VID_054C&PID_0CE6
    try:
        base_key = r"SYSTEM\CurrentControlSet\Enum\HID"
        with winreg.OpenKey(winreg.HKEY_LOCAL_MACHINE, base_key) as k_hid:
            i = 0
            while True:
                try:
                    sub = winreg.EnumKey(k_hid, i)
                    i += 1
                except OSError:
                    break
                if "VID_054C" in sub.upper() and ("PID_0CE6" in sub.upper() or "PID_0DF2" in sub.upper()):
                    with winreg.OpenKey(k_hid, sub) as k_dev:
                        j = 0
                        while True:
                            try:
                                inst = winreg.EnumKey(k_dev, j)
                                j += 1
                            except OSError:
                                break
                            candidate = f"\\\\?\\HID#{sub}#{inst}#{guid_str}"
                            h_test = kernel32.CreateFileW(
                                candidate,
                                GENERIC_READ | GENERIC_WRITE,
                                FILE_SHARE_READ | FILE_SHARE_WRITE,
                                None,
                                OPEN_EXISTING,
                                0,
                                None,
                            )
                            if h_test != -1 and h_test != 0xFFFFFFFFFFFFFFFF:
                                kernel32.CloseHandle(h_test)
                                return candidate
    except Exception:
        pass
    return None


def bind_to_interactive_desktop():
    """Bind current thread to the active interactive Windows desktop (WinSta0\\Default)."""
    hdesk = user32.OpenInputDesktop(0, False, MAXIMUM_ALLOWED)
    if hdesk:
        user32.SetThreadDesktop(hdesk)
        user32.CloseDesktop(hdesk)
        return True
    return False


# ---------------------------------------------------------------------------
# Global Engine State & Configurable Parameters
# ---------------------------------------------------------------------------
STATE_LOCK = threading.Lock()

CONFIG = {
    "enabled": True,               # Master switch for Gyro Aiming
    "l2_threshold": 195,           # Hard-press L2 threshold (0..255) to engage Gyro
    "wall_enabled": True,          # Enable 2-stage L2 Adaptive Trigger mechanical wall
    "wall_start": 135,             # Start position of physical resistance wall (0..255)
    "wall_end": 185,               # End position of physical resistance wall (0..255)
    "wall_force": 235,             # Strength of physical wall (0..255)
    "sens_x": 0.0180,              # Horizontal sensitivity (deg/s LSB -> pixels/frame)
    "sens_y": 0.0150,              # Vertical sensitivity (deg/s LSB -> pixels/frame)
    "yaw_roll_mix": 0.35,          # Mix of Roll (gz) into horizontal Yaw (gy): 0.0=Pure Yaw, 1.0=Equal
    "deadzone": 8.0,               # Gyro raw LSB deadzone after zero-bias removal
    "ema_alpha": 0.65,             # Exponential moving average smoothing (1.0 = no smoothing, 0.2 = heavy)
    "invert_x": False,             # Invert horizontal gyro axis
    "invert_y": False,             # Invert vertical gyro axis
    "always_on_test": False,       # Optional: test gyro without holding L2
}

TELEMETRY = {
    "connected": False,
    "device_path": "",
    "polling_hz": 0.0,
    "l2_raw": 0,
    "r2_raw": 0,
    "gyro_active": False,
    "gx": 0.0,
    "gy": 0.0,
    "gz": 0.0,
    "bias_x": 0.0,
    "bias_y": 0.0,
    "bias_z": 0.0,
    "dx_accum": 0.0,
    "dy_accum": 0.0,
    "last_dx": 0,
    "last_dy": 0,
    "lx": 128,
    "ly": 128,
    "rx": 128,
    "ry": 128,
    "calibrated": False,
    "total_frames": 0,
}

CALIBRATE_REQUESTED = True


def build_dualsense_output_report(gyro_engaged: bool, cfg: dict, release_only: bool = False) -> bytes:
    """
    Construct the 48-byte USB Output Report (ID 0x02) for DualSense via DS5-Bridge:
    - Leaves R2 trigger bit (0x04) and rumble bits (0x01|0x02) UNSET (0) so native in-game
      R2 adaptive trigger effects and rumble are NEVER overwritten!
    - When release_only=True (user switched to Native Game Pass-Through mode), releases L2
      and LED override once, after which the engine operates in 100% Read-Only mode.
    """
    out = bytearray(48)
    out[0] = 0x02

    if release_only:
        out[1] = 0x08  # Only touch L2 to release our custom wall
        out[2] = 0x08  # Release LED override back to firmware/game
        out[22] = 0x05 # Release L2 motor
        return bytes(out)

    out[1] = 0x08          # ONLY touch L2 trigger motor (0x08); leave R2 (0x04) & Rumble (0x03) to the game!
    out[2] = 0x04 | 0x10   # Enable Lightbar RGB (0x04) and Player LEDs (0x10)

    # L2 Trigger (offset 22): Mode 0x02 (Section Resistance Wall)
    out[22] = 0x02
    out[23] = max(10, min(240, int(cfg.get("wall_start", 135))))
    out[24] = max(out[23] + 10, min(250, int(cfg.get("wall_end", 185))))
    out[25] = max(0, min(255, int(cfg.get("wall_force", 235))))

    # Player LEDs (offset 44) & Lightbar RGB (offsets 45, 46, 47)
    if gyro_engaged:
        out[44] = 0x1F  # All 5 Player LEDs ON
        out[45] = 255   # R: Tactical Lock-On Red
        out[46] = 25    # G
        out[47] = 50    # B
    else:
        out[44] = 0x04  # Center Player LED ON
        out[45] = 0     # R: Standby Cyber Cyan
        out[46] = 210   # G
        out[47] = 255   # B

    return bytes(out)


def gyro_worker_loop():
    """
    500Hz High-Priority Hardware-in-the-Loop Thread:
    Reads 64-byte DualSense input reports from Pico 2 W DS5-Bridge, runs zero-bias
    calibration + EMA smoothing + sub-pixel remainder integration, and injects
    relative mouse deltas when L2 >= l2_threshold.
    """
    global CALIBRATE_REQUESTED

    bind_to_interactive_desktop()

    # Elevate thread priority for jitter-free 500Hz polling
    try:
        kernel32.SetThreadPriority(kernel32.GetCurrentThread(), 2)  # THREAD_PRIORITY_HIGHEST
    except Exception:
        pass

    bias_x, bias_y, bias_z = 0.0, 0.0, -2.0
    smooth_vx, smooth_vy = 0.0, 0.0
    rem_x, rem_y = 0.0, 0.0

    calib_samples = []
    last_led_state = None
    last_wall_signature = None
    last_desktop_rebind = time.perf_counter()

    while True:
        path = find_ds5_hid_path()
        if not path:
            with STATE_LOCK:
                TELEMETRY["connected"] = False
                TELEMETRY["device_path"] = ""
                TELEMETRY["polling_hz"] = 0.0
            time.sleep(0.5)
            continue

        h = kernel32.CreateFileW(
            path,
            GENERIC_READ | GENERIC_WRITE,
            FILE_SHARE_READ | FILE_SHARE_WRITE,
            None,
            OPEN_EXISTING,
            0,
            None,
        )
        if h == -1 or h == 0xFFFFFFFFFFFFFFFF:
            with STATE_LOCK:
                TELEMETRY["connected"] = False
            time.sleep(0.5)
            continue

        with STATE_LOCK:
            TELEMETRY["connected"] = True
            TELEMETRY["device_path"] = path

        buf = (ctypes.c_ubyte * 64)()
        nread = ctypes.c_ulong(0)
        written = ctypes.c_ulong(0)

        frame_count = 0
        t_rate_start = time.perf_counter()
        last_led_state = None
        last_wall_signature = None

        try:
            while True:
                ok = kernel32.ReadFile(h, ctypes.byref(buf), 64, ctypes.byref(nread), None)
                if not ok or nread.value < 32:
                    break

                now = time.perf_counter()
                frame_count += 1
                if now - t_rate_start >= 0.25:
                    hz = frame_count / (now - t_rate_start)
                    with STATE_LOCK:
                        TELEMETRY["polling_hz"] = round(hz, 1)
                    frame_count = 0
                    t_rate_start = now

                # Re-verify input desktop every 2 seconds in case user alt-tabs or UAC pops up
                if now - last_desktop_rebind > 2.0:
                    bind_to_interactive_desktop()
                    last_desktop_rebind = now

                b = bytes(buf[:nread.value])
                if b[0] != 0x01:
                    continue

                lx, ly, rx, ry, l2, r2 = b[1], b[2], b[3], b[4], b[5], b[6]
                gx, gy, gz, ax, ay, az = struct.unpack_from("<hhhhhh", b, 16)

                # Auto-calibration of stationary gyro zero-bias
                if CALIBRATE_REQUESTED:
                    calib_samples.append((gx, gy, gz))
                    if len(calib_samples) >= 80:  # ~160ms at 500Hz
                        n_s = len(calib_samples)
                        bias_x = sum(s[0] for s in calib_samples) / n_s
                        bias_y = sum(s[1] for s in calib_samples) / n_s
                        bias_z = sum(s[2] for s in calib_samples) / n_s
                        calib_samples.clear()
                        CALIBRATE_REQUESTED = False
                        with STATE_LOCK:
                            TELEMETRY["bias_x"] = round(bias_x, 2)
                            TELEMETRY["bias_y"] = round(bias_y, 2)
                            TELEMETRY["bias_z"] = round(bias_z, 2)
                            TELEMETRY["calibrated"] = True
                else:
                    # Continuous slow stationary drift tracking when controller is completely still on desk
                    if abs(gx - bias_x) < 6 and abs(gy - bias_y) < 6 and abs(gz - bias_z) < 6:
                        bias_x = 0.9995 * bias_x + 0.0005 * gx
                        bias_y = 0.9995 * bias_y + 0.0005 * gy
                        bias_z = 0.9995 * bias_z + 0.0005 * gz

                with STATE_LOCK:
                    cfg = dict(CONFIG)

                # Subtract zero-bias
                cgx = gx - bias_x
                cgy = gy - bias_y
                cgz = gz - bias_z

                # Determine if L2 hard-press threshold is met
                gyro_engaged = bool(
                    cfg["enabled"]
                    and (l2 >= cfg["l2_threshold"] or cfg.get("always_on_test", False))
                )

                # Update DualSense Adaptive Trigger wall & Lightbar ONLY when wall_enabled is True.
                # When wall_enabled is False (Native Game Pass-Through Mode), send 1 release packet
                # and then become 100% Read-Only so native PS5 games control L2/R2/LEDs undisturbed.
                if cfg["wall_enabled"]:
                    wall_sig = (
                        True,
                        cfg["wall_start"],
                        cfg["wall_end"],
                        cfg["wall_force"],
                    )
                    if gyro_engaged != last_led_state or wall_sig != last_wall_signature:
                        out_rep = build_dualsense_output_report(gyro_engaged, cfg, release_only=False)
                        out_cb = (ctypes.c_ubyte * len(out_rep)).from_buffer_copy(out_rep)
                        kernel32.WriteFile(
                            h, ctypes.byref(out_cb), len(out_rep), ctypes.byref(written), None
                        )
                        last_led_state = gyro_engaged
                        last_wall_signature = wall_sig
                else:
                    if last_wall_signature is not None and last_wall_signature[0] is True:
                        out_rep = build_dualsense_output_report(False, cfg, release_only=True)
                        out_cb = (ctypes.c_ubyte * len(out_rep)).from_buffer_copy(out_rep)
                        kernel32.WriteFile(
                            h, ctypes.byref(out_cb), len(out_rep), ctypes.byref(written), None
                        )
                    last_wall_signature = (False, 0, 0, 0)
                    last_led_state = None

                int_dx, int_dy = 0, 0
                if gyro_engaged:
                    # DualSense coordinate frame:
                    # +Y is vertical up -> gy is Yaw (CCW/Left is +gy, CW/Right is -gy)
                    # +Z is back toward player -> gz is Roll (tilt Left is +gz, tilt Right is -gz)
                    # +X is right -> gx is Pitch (nose Up is +gx, nose Down is -gx)
                    yaw_roll = -(cgy + cfg["yaw_roll_mix"] * cgz)
                    pitch = -cgx

                    if cfg["invert_x"]:
                        yaw_roll = -yaw_roll
                    if cfg["invert_y"]:
                        pitch = -pitch

                    # Radial deadzone with smooth ramp-in (C^0 continuous soft-thresholding!)
                    mag = math.hypot(yaw_roll, pitch)
                    dz = cfg["deadzone"]
                    if mag <= dz:
                        eff_x, eff_y = 0.0, 0.0
                    else:
                        scale = (mag - dz) / mag
                        eff_x = yaw_roll * scale
                        eff_y = pitch * scale

                    # Exponential Moving Average (EMA) low-pass filter
                    alpha = max(0.05, min(1.0, cfg["ema_alpha"]))
                    smooth_vx = alpha * (eff_x * cfg["sens_x"]) + (1.0 - alpha) * smooth_vx
                    smooth_vy = alpha * (eff_y * cfg["sens_y"]) + (1.0 - alpha) * smooth_vy

                    # Sub-pixel remainder accumulation
                    tot_x = smooth_vx + rem_x
                    tot_y = smooth_vy + rem_y
                    int_dx = int(tot_x)
                    int_dy = int(tot_y)
                    rem_x = tot_x - int_dx
                    rem_y = tot_y - int_dy

                    if int_dx != 0 or int_dy != 0:
                        user32.mouse_event(MOUSEEVENTF_MOVE, int_dx, int_dy, 0, 0)
                else:
                    smooth_vx = 0.0
                    smooth_vy = 0.0
                    rem_x = 0.0
                    rem_y = 0.0

                with STATE_LOCK:
                    TELEMETRY["l2_raw"] = l2
                    TELEMETRY["r2_raw"] = r2
                    TELEMETRY["gyro_active"] = gyro_engaged
                    TELEMETRY["gx"] = round(cgx, 1)
                    TELEMETRY["gy"] = round(cgy, 1)
                    TELEMETRY["gz"] = round(cgz, 1)
                    TELEMETRY["dx_accum"] = round(smooth_vx, 2)
                    TELEMETRY["dy_accum"] = round(smooth_vy, 2)
                    TELEMETRY["last_dx"] = int_dx
                    TELEMETRY["last_dy"] = int_dy
                    TELEMETRY["lx"] = lx
                    TELEMETRY["ly"] = ly
                    TELEMETRY["rx"] = rx
                    TELEMETRY["ry"] = ry
                    TELEMETRY["total_frames"] += 1

        finally:
            kernel32.CloseHandle(h)


# ---------------------------------------------------------------------------
# HTTP Telemetry & Live Tuning Server (Port 8788)
# ---------------------------------------------------------------------------
HTML_PATH = os.path.join(os.path.dirname(os.path.abspath(__file__)), "ds5_gyro_hud.html")


class GyroHTTPHandler(BaseHTTPRequestHandler):
    def log_message(self, format, *args):
        pass  # Keep console clean

    def _send_json(self, data, status=200):
        raw = json.dumps(data, ensure_ascii=False).encode("utf-8")
        self.send_response(status)
        self.send_header("Content-Type", "application/json; charset=utf-8")
        self.send_header("Access-Control-Allow-Origin", "*")
        self.send_header("Cache-Control", "no-store")
        self.send_header("Content-Length", str(len(raw)))
        self.end_headers()
        self.wfile.write(raw)

    def do_OPTIONS(self):
        self.send_response(204)
        self.send_header("Access-Control-Allow-Origin", "*")
        self.send_header("Access-Control-Allow-Methods", "GET, POST, OPTIONS")
        self.send_header("Access-Control-Allow-Headers", "Content-Type")
        self.end_headers()

    def do_GET(self):
        parsed = urlparse(self.path)
        if parsed.path in ("/", "/index.html"):
            if os.path.exists(HTML_PATH):
                with open(HTML_PATH, "rb") as f:
                    content = f.read()
                self.send_response(200)
                self.send_header("Content-Type", "text/html; charset=utf-8")
                self.send_header("Cache-Control", "no-store")
                self.send_header("Content-Length", str(len(content)))
                self.end_headers()
                self.wfile.write(content)
            else:
                self._send_json({"error": "HUD HTML not found"}, 404)
        elif parsed.path == "/api/state":
            with STATE_LOCK:
                payload = {"config": dict(CONFIG), "telemetry": dict(TELEMETRY)}
            self._send_json(payload)
        else:
            self._send_json({"error": "Not found"}, 404)

    def do_POST(self):
        global CALIBRATE_REQUESTED
        parsed = urlparse(self.path)
        length = int(self.headers.get("Content-Length", "0"))
        body = self.rfile.read(length) if length > 0 else b"{}"
        try:
            req = json.loads(body.decode("utf-8"))
        except Exception:
            req = {}

        if parsed.path == "/api/config":
            with STATE_LOCK:
                for k, v in req.items():
                    if k in CONFIG:
                        CONFIG[k] = v
                payload = {"ok": True, "config": dict(CONFIG)}
            self._send_json(payload)
        elif parsed.path == "/api/calibrate":
            CALIBRATE_REQUESTED = True
            self._send_json({"ok": True, "message": "Zero-bias calibration started"})
        else:
            self._send_json({"error": "Unknown endpoint"}, 404)


def main():
    t = threading.Thread(target=gyro_worker_loop, daemon=True)
    t.start()
    server = ThreadingHTTPServer(("127.0.0.1", 8788), GyroHTTPHandler)
    print("=================================================================")
    print(" RP2350 DS5-Bridge 500Hz L2 Two-Stage Gyro-Aim Engine Running!")
    print(" Web HUD & Target Range: http://127.0.0.1:8788")
    print("=================================================================")
    server.serve_forever()


if __name__ == "__main__":
    main()
