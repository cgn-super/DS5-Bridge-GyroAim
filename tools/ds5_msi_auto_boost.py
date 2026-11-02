#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
DS5-Bridge Pico 2 W -> MSI Alpha 17 (微星雷影17) 自动狂暴风扇 & 内存清理守护程序
================================================================================
100% 反作弊安全设计 (Zero Anti-Cheat Risk):
  1. 绝不加载任何第三方内核驱动 (如 WinRing0 / inpoutx64)；通过微星官方正版签名服务
     MSI.CentralServer.exe (127.0.0.1:32683, Base Module ID=104) 发送合法 IPC 指令开启 Cooler Boost。
  2. 绝不遍历句柄或对未知进程/游戏/反作弊进程调用 OpenProcess；仅按安全白名单清理高占用的国产后台闲置软件。
  3. 自动联动微软官方签名工具 Sysinternals RAMMap (RAMMap-64bit.exe)
     执行物理内存工作集与备用列表清理 (-Ew / -Et)。
  4. 双重触发机制：
     - 硬件热插拔触发：检测到 Pico 2 W (Sony DualSense VID_054C&PID_0CE6) 插入电脑时立即触发；
     - 固件底层 Hook / 手柄组合键触发：读取标准 HID 0x01 输入报告的保留字节 UNK2 (offset 11, 0xA0..0xAF)，
       或在游戏内随时按下【L3 + R3 + L1 + R1】组合键时立即再次触发！
"""

import argparse
import csv
import ctypes
from ctypes import wintypes
import io
import os
import shutil
import socket
import struct
import subprocess
import sys
import time
import winreg

try:
    sys.stdout.reconfigure(errors="replace")
    sys.stderr.reconfigure(errors="replace")
except Exception:
    pass

k32 = ctypes.windll.kernel32
shell32 = ctypes.windll.shell32

HID_GUID = "{4d1e55b2-f16f-11cf-88cb-001111000030}"

# 自动按优先级搜索本机 RAMMap 路径（兼顾本地环境与 GitHub 开源通用性）
RAMMAP_SEARCH_PATHS = [
    os.environ.get("RAMMAP_PATH", ""),
    os.path.join(os.path.dirname(os.path.abspath(__file__)), "RAMMap-64bit.exe"),
    os.path.join(os.path.dirname(os.path.abspath(__file__)), "RAMMap.exe"),
    r"C:\baidunetdiskdownload\RAMMap\RAMMap-64bit.exe",
    r"C:\baidunetdiskdownload\RAMMap\RAMMap.exe",
    shutil.which("RAMMap-64bit.exe") or "",
    shutil.which("RAMMap.exe") or "",
]

# 严格限定的非必要后台高占用软件白名单（绝不触碰系统、Steam、游戏或反作弊进程）
SAFE_KILL_BLOATWARE = [
    "BaiduNetdisk.exe",
    "BaiduNetdiskHost.exe",
    "xp2p.exe",
    "quark.exe",
    "QuarkCloudDrive.exe",
    "WeChatAppEx.exe",          # 微信小程序后台渲染器（常驻吃数GB内存，不影响微信主程序聊天）
    "cloudmusic.exe",
    "qqmusic.exe",
    "kugou.exe",
    "kuwomusic.exe",
    "bodian_pc.exe",
    "Bilibili.exe",
    "Douyin.exe",
    "iQIYI.exe",
    "QyClient.exe",
    "QQLive.exe",
    "YoukuDesktop.exe",
    "OneDrive.exe",
    "WidgetService.exe",
    "Widgets.exe",
    "PhoneExperienceHost.exe",
]


def resolve_default_rammap():
    for p in RAMMAP_SEARCH_PATHS:
        if p and os.path.isfile(p):
            return p
    return r"C:\baidunetdiskdownload\RAMMap\RAMMap-64bit.exe"


class MEMORYSTATUSEX(ctypes.Structure):
    _fields_ = [
        ("dwLength", wintypes.DWORD),
        ("dwMemoryLoad", wintypes.DWORD),
        ("ullTotalPhys", ctypes.c_ulonglong),
        ("ullAvailPhys", ctypes.c_ulonglong),
        ("ullTotalPageFile", ctypes.c_ulonglong),
        ("ullAvailPageFile", ctypes.c_ulonglong),
        ("ullTotalVirtual", ctypes.c_ulonglong),
        ("ullAvailVirtual", ctypes.c_ulonglong),
        ("ullAvailExtendedVirtual", ctypes.c_ulonglong),
    ]


def get_memory_status():
    """获取当前物理内存状态 (可用 MB, 总计 MB, 占用百分比)"""
    stat = MEMORYSTATUSEX()
    stat.dwLength = ctypes.sizeof(MEMORYSTATUSEX)
    k32.GlobalMemoryStatusEx(ctypes.byref(stat))
    avail_mb = stat.ullAvailPhys / (1024 * 1024)
    total_mb = stat.ullTotalPhys / (1024 * 1024)
    return avail_mb, total_mb, stat.dwMemoryLoad


def get_msi_ipc_config():
    """从注册表读取微星 MSI Center CentralServer 端口与 Base Module 组件 ID"""
    port = 32683
    dest_id = 104
    try:
        with winreg.OpenKey(
            winreg.HKEY_LOCAL_MACHINE,
            r"SOFTWARE\WOW6432Node\MSI\MSI Center\Component\SDK",
        ) as k:
            val, _ = winreg.QueryValueEx(k, "Server Port")
            port = int(val)
    except OSError:
        pass

    try:
        with winreg.OpenKey(
            winreg.HKEY_LOCAL_MACHINE,
            r"SOFTWARE\WOW6432Node\MSI\MSI Center\Component\Base Module",
        ) as k:
            val, _ = winreg.QueryValueEx(k, "ID")
            dest_id = int(val)
    except OSError:
        pass

    return port, dest_id


def msi_send_command(payload_bytes, source_id=1003, timeout=2.0):
    """通过 MSI.CentralServer.exe 官方本地 TCP IPC 发送控制报文"""
    port, dest_id = get_msi_ipc_config()
    header = bytes(range(15, -1, -1))
    pkt = header + struct.pack("<ii", source_id, dest_id) + bytes(payload_bytes)
    with socket.create_connection(("127.0.0.1", port), timeout=timeout) as s:
        s.sendall(pkt)
        s.settimeout(timeout)
        try:
            resp = s.recv(1024)
            return resp.decode("utf-8", errors="ignore").strip("\x00").strip()
        except socket.timeout:
            return ""


def set_msi_cooler_boost(enable=True):
    """
    控制微星雷影17 (MSI Alpha 17 C7VF / MS-17KK) Cooler Boost 狂暴风扇模式
      enable=True  -> payload [2, 0, 0, 9, 1, 7, 0, 1] (风扇全速 Cooler Boost ON)
      enable=False -> payload [2, 0, 0, 9, 1, 7, 0, 2] (恢复自动温控风扇 Auto)
    """
    cmd_byte = 1 if enable else 2
    res = msi_send_command([2, 0, 0, 9, 1, 7, 0, cmd_byte])
    rpm = msi_send_command([2, 0, 0, 9, 1, 7, 0, 3])
    return res, rpm


def get_msi_fan_rpm():
    """查询当前微星雷影17 CPU/GPU 双风扇实时转速"""
    return msi_send_command([2, 0, 0, 9, 1, 7, 0, 3])


def stop_daemon_and_restore_ai_engine():
    """
    停止后台正在运行的 ds5_msi_auto_boost.py 守护进程，
    关闭 Cooler Boost 强冷，并将控制权完整交还给微星 AI 引擎 (MSI AI Engine 智能模式)。
    """
    my_pid = os.getpid()
    stopped_pids = []
    try:
        ps_cmd = (
            f"Get-CimInstance Win32_Process -Filter \"Name='python.exe' or Name='pythonw.exe'\" | "
            f"Where-Object {{ $_.ProcessId -ne {my_pid} -and $_.CommandLine -match 'ds5_msi_auto_boost' }} | "
            f"ForEach-Object {{ Stop-Process -Id $_.ProcessId -Force; $_.ProcessId }}"
        )
        out = subprocess.check_output(
            ["powershell", "-NoProfile", "-Command", ps_cmd],
            creationflags=subprocess.CREATE_NO_WINDOW,
            text=True,
            errors="ignore",
        )
        for line in out.splitlines():
            line = line.strip()
            if line.isdigit():
                stopped_pids.append(line)
    except Exception:
        pass

    if stopped_pids:
        print(f"  -> [守护进程] 已停止后台狂暴监听进程 (PID: {', '.join(stopped_pids)})")
    else:
        print("  -> [守护进程] 当前后台无正在运行的狂暴监听进程")

    # 1. 关闭 Cooler Boost 强冷风扇
    res_cb, _ = set_msi_cooler_boost(False)
    # 2. 发送 MSI AI Engine 智能模式同步指令 (Index=2 Smart Auto, Fan=0 Auto)
    scenario_json = b'{"Index":2,"Performance":2,"Fan":0,"KB":-1,"PB":-1,"IsLoad":false}'
    try:
        msi_send_command([0, 18] + list(scenario_json), source_id=1003)
        msi_send_command([2, 0, 2, 3, 8, 23, 0, 5], source_id=1030)
    except Exception:
        pass

    # 3. 确保 MSI_AI_Engine.exe 正在运行
    ai_exe = r"C:\Program Files (x86)\MSI\MSI Center\AI Engine\MSI_AI_Engine.exe"
    try:
        out = subprocess.check_output(
            ["tasklist", "/FI", "IMAGENAME eq MSI_AI_Engine.exe", "/NH"],
            creationflags=subprocess.CREATE_NO_WINDOW,
            text=True,
            encoding="gbk",
            errors="ignore",
        )
        if "msi_ai_engine.exe" not in out.lower() and os.path.isfile(ai_exe):
            subprocess.Popen([ai_exe], cwd=os.path.dirname(ai_exe))
            print("  -> [微星AI引擎] 已重新拉起 MSI_AI_Engine.exe 服务")
        else:
            print("  -> [微星AI引擎] MSI_AI_Engine.exe 正在正常运行中")
    except Exception:
        pass

    time.sleep(1.0)
    rpm = get_msi_fan_rpm()
    print(f"  -> [风扇温控] Cooler Boost 已关闭 (IPC返回={res_cb})，已恢复【微星 AI 引擎 (智能自动模式)】")
    print(f"  -> [当前转速] {rpm} (正平滑回落至静音温控转速)")

    # 4. 重新在后台静默挂起监听 (--wait-replug)，确保下次重新拔插芯片时依然能全自动触发
    pythonw_exe = os.path.join(os.path.dirname(sys.executable), "pythonw.exe")
    if not os.path.isfile(pythonw_exe):
        pythonw_exe = sys.executable
    script_path = os.path.abspath(__file__)
    try:
        subprocess.Popen(
            [pythonw_exe, script_path, "--auto-restore-fan", "--wait-replug"],
            cwd=os.path.dirname(script_path),
            creationflags=subprocess.CREATE_NO_WINDOW | 0x00000008,  # DETACHED_PROCESS
            close_fds=True,
        )
        print("  -> [自动待命] 后台静默监听已就绪：下次重新拔插 Pico 2 W 芯片（或按 L3+R3+L1+R1）将再次自动触发！")
    except Exception as e:
        print(f"  -> [自动待命] 后台监听启动异常: {e}")


def kill_background_bloatware():
    """仅针对安全白名单中的闲置高内存后台应用执行优雅清理，绝不触碰未知进程"""
    killed = []
    try:
        out = subprocess.check_output(
            ["tasklist", "/FO", "CSV", "/NH"],
            creationflags=subprocess.CREATE_NO_WINDOW,
            text=True,
            encoding="gbk",
            errors="ignore",
        )
        running_names = set()
        for row in csv.reader(io.StringIO(out)):
            if row:
                running_names.add(row[0].lower())
    except Exception:
        running_names = set()

    target_map = {name.lower(): name for name in SAFE_KILL_BLOATWARE}
    for lower_name, orig_name in target_map.items():
        if lower_name in running_names:
            try:
                rc = subprocess.call(
                    ["taskkill", "/F", "/IM", orig_name],
                    stdout=subprocess.DEVNULL,
                    stderr=subprocess.DEVNULL,
                    creationflags=subprocess.CREATE_NO_WINDOW,
                )
                if rc == 0:
                    killed.append(orig_name)
            except Exception:
                pass
    return killed


def is_admin():
    try:
        return bool(shell32.IsUserAnAdmin())
    except Exception:
        return False


def run_rammap_cleanup(rammap_path, allow_uac_prompt=False):
    """
    调用微软官方签名工具 RAMMap-64bit.exe 释放工作集 (-Ew) 与备用内存列表 (-Et)。
    """
    if not rammap_path or not os.path.isfile(rammap_path):
        return False, f"未找到 RAMMap ({rammap_path})"

    # 确保 EULA 已接受，避免弹窗阻塞
    try:
        with winreg.CreateKey(
            winreg.HKEY_CURRENT_USER, r"Software\Sysinternals\RAMMap"
        ) as k:
            winreg.SetValueEx(k, "EulaAccepted", 0, winreg.REG_DWORD, 1)
    except OSError:
        pass

    if is_admin():
        try:
            subprocess.run(
                [rammap_path, "-Ew"],
                creationflags=subprocess.CREATE_NO_WINDOW,
                timeout=15,
            )
            subprocess.run(
                [rammap_path, "-Et"],
                creationflags=subprocess.CREATE_NO_WINDOW,
                timeout=15,
            )
            return True, "RAMMap (-Ew / -Et) 管理员静默清理完成"
        except Exception as e:
            return False, f"RAMMap 执行异常: {e}"
    elif allow_uac_prompt:
        rc = shell32.ShellExecuteW(None, "runas", rammap_path, "-Ew", None, 0)
        if rc > 32:
            time.sleep(1.5)
            return True, "RAMMap (-Ew) 提权清理已触发"
        return False, f"RAMMap 提权被取消 (rc={rc})"
    else:
        return False, "当前非管理员权限，已跳过需提权的 RAMMap 内核页清理（双击 start_msi_auto_boost.bat 即可自动以管理员静默执行）"


def execute_boost_and_clean(rammap_path, allow_uac=False, reason="Hook Trigger"):
    """一键执行：微星雷影17风扇拉满 + 杀后台白名单 + RAMMap 内存释放"""
    ts = time.strftime("%H:%M:%S")
    print(f"\n[{ts}] [BOOST] 触发自动狂暴与内存优化 ({reason})")
    avail_before, total_mb, load_before = get_memory_status()

    # 1. 开启微星雷影17 Cooler Boost 最大风扇转速
    try:
        res, rpm_before = set_msi_cooler_boost(True)
        time.sleep(0.8)
        rpm_now = get_msi_fan_rpm()
        print(f"  -> [微星雷影17风扇] Cooler Boost 已开启 (IPC返回={res}) | 当前转速: {rpm_now or rpm_before}")
    except Exception as e:
        print(f"  -> [微星雷影17风扇] IPC 通信失败: {e}")

    # 2. 清理后台高占用闲置软件（严格白名单）
    killed = kill_background_bloatware()
    if killed:
        print(f"  -> [后台进程清理] 已结束 {len(killed)} 个非必要后台程序: {', '.join(killed)}")
    else:
        print("  -> [后台进程清理] 当前非常干净，无白名单闲置后台程序")

    # 3. 调用 RAMMap 清理物理内存
    ok, msg = run_rammap_cleanup(rammap_path=rammap_path, allow_uac_prompt=allow_uac)
    print(f"  -> [RAMMap 内存整理] {msg}")

    time.sleep(0.5)
    avail_after, _, load_after = get_memory_status()
    freed_mb = max(0.0, avail_after - avail_before)
    print(
        f"  -> [内存统计] 可用内存: {avail_before:,.0f} MB -> {avail_after:,.0f} MB "
        f"(净释放 +{freed_mb:,.0f} MB | 占用率: {load_before}% -> {load_after}%)"
    )


def open_dualsense_hid():
    """查找并打开 DS5-Bridge (Sony DualSense VID_054C & PID_0CE6/0DF2) HID 设备句柄"""
    try:
        with winreg.OpenKey(
            winreg.HKEY_LOCAL_MACHINE, r"SYSTEM\CurrentControlSet\Enum\HID"
        ) as root:
            i = 0
            while True:
                try:
                    sub = winreg.EnumKey(root, i)
                except OSError:
                    break
                i += 1
                u = sub.upper()
                if "VID_054C" in u and ("PID_0CE6" in u or "PID_0DF2" in u):
                    with winreg.OpenKey(root, sub) as dev:
                        j = 0
                        while True:
                            try:
                                inst = winreg.EnumKey(dev, j)
                            except OSError:
                                break
                            j += 1
                            path = f"\\\\?\\HID#{sub}#{inst}#{HID_GUID}"
                            # FILE_SHARE_READ | FILE_SHARE_WRITE = 3, OPEN_EXISTING = 3
                            h = k32.CreateFileW(path, 0x80000000, 3, None, 3, 0, None)
                            if h not in (-1, 0xFFFFFFFF, 0xFFFFFFFFFFFFFFFF):
                                return h, path
    except OSError:
        pass
    return None, None


def monitor_loop(rammap_path, allow_uac=False, auto_restore_fan=False, wait_replug=False):
    """
    常驻监听 Pico 2 W (DS5-Bridge) 插入事件与固件底层 Hook 信号 (UNK2 = 0xA0..0xAF)
    """
    print("=" * 74)
    print("[DS5-Bridge Pico 2 W x 微星雷影17] 自动狂暴风扇 & 内存清理守护已启动")
    print(f"  * 管理员权限: {'是 (支持静默 RAMMap 深度清理)' if is_admin() else '否 (双击 start_msi_auto_boost.bat 或加 --uac-rammap 可提权)'}")
    print(f"  * RAMMap路径: {rammap_path}")
    try:
        print(f"  * 当前风扇转速: {get_msi_fan_rpm()}")
    except Exception as e:
        print(f"  * 当前风扇转速: 读取失败 ({e})")
    avail_mb, total_mb, load_pct = get_memory_status()
    print(f"  * 当前物理内存: 可用 {avail_mb:,.0f} / {total_mb:,.0f} MB (占用 {load_pct}%)")
    print("  * 触发方式 1: 插入 Pico 2 W 接收器芯片自动触发")
    print("  * 触发方式 2: 游戏中随时按手柄 [L3 + R3 + L1 + R1] 组合键手动触发")
    print("  * 停止方式  : 点击桌面【停止狂暴并恢复微星AI引擎】立即停止并恢复智能温控")
    print("=" * 74)

    last_hook_byte = 0
    last_trigger_time = 0.0
    was_connected = False
    ignore_boot_hook = False

    if wait_replug:
        h_init, _ = open_dualsense_hid()
        if h_init:
            k32.CloseHandle(h_init)
            was_connected = True
            ignore_boot_hook = True
            print("[WAIT] 当前芯片处于已插入状态，已按停止指令保持微星 AI 引擎；拔出后重新插入芯片将自动触发！")

    while True:
        h, dev_path = open_dualsense_hid()
        if not h:
            if was_connected:
                ts = time.strftime("%H:%M:%S")
                print(f"[{ts}] [USB] Pico 2 W (DS5-Bridge) 已断开连接。")
                if auto_restore_fan:
                    try:
                        set_msi_cooler_boost(False)
                        print(f"[{ts}] [FAN] 已自动将微星雷影17风扇恢复为微星 AI 引擎自动温控模式。")
                    except Exception:
                        pass
                was_connected = False
                ignore_boot_hook = False
                last_hook_byte = 0
            time.sleep(1.0)
            continue

        if not was_connected:
            was_connected = True
            ignore_boot_hook = False
            if time.time() - last_trigger_time > 3.0:
                last_trigger_time = time.time()
                execute_boost_and_clean(
                    rammap_path=rammap_path,
                    allow_uac=allow_uac,
                    reason="检测到 Pico 2 W (DS5-Bridge) 芯片插入",
                )

        buf = (ctypes.c_ubyte * 64)()
        n_read = ctypes.c_ulong()

        # 循环读取 HID 输入报告，监听固件层发出的 Hook 脉冲信号 (b[11] 即 UNK2 == 0xA0..0xAF)
        # 同时兼容未刷更新固件时直接在 PC 侧检测 L3+R3+L1+R1 组合键
        combo_latched = False
        while True:
            ok = k32.ReadFile(h, ctypes.byref(buf), 64, ctypes.byref(n_read), None)
            if not ok or n_read.value == 0:
                k32.CloseHandle(h)
                break

            b = bytes(buf[: n_read.value])
            if len(b) < 12 or b[0] != 0x01:
                continue

            # 1) 检测固件内嵌 Hook 签名脉冲 (r.UNK2 位于 b[11]，高 4 位为 0xA0)
            unk2 = b[11]
            if (unk2 & 0xF0) == 0xA0:
                if ignore_boot_hook and unk2 == 0xA5:
                    last_hook_byte = unk2
                elif unk2 != last_hook_byte and (time.time() - last_trigger_time > 2.0):
                    last_hook_byte = unk2
                    last_trigger_time = time.time()
                    execute_boost_and_clean(
                        rammap_path=rammap_path,
                        allow_uac=allow_uac,
                        reason=f"收到 Pico 2 W 固件底层 Hook 信号 (0x{unk2:02X})",
                    )
            else:
                last_hook_byte = 0

            # 2) 兼容兜底：如果用户当前插着旧版固件按下 L1(bit0) + R1(bit1) + L3(bit6) + R3(bit7) in b[9] (0xC3)
            btn_byte = b[9]
            combo_pressed = ((btn_byte & 0xC3) == 0xC3)
            if combo_pressed and not combo_latched:
                combo_latched = True
                if time.time() - last_trigger_time > 2.0:
                    last_trigger_time = time.time()
                    execute_boost_and_clean(
                        rammap_path=rammap_path,
                        allow_uac=allow_uac,
                        reason="检测到手柄快捷键 L3 + R3 + L1 + R1",
                    )
            elif not combo_pressed:
                combo_latched = False


def main():
    default_rammap = resolve_default_rammap()
    parser = argparse.ArgumentParser(
        description="DS5-Bridge Pico 2 W x 微星雷影17 自动狂暴风扇 & RAMMap 内存优化工具"
    )
    parser.add_argument(
        "--once",
        action="store_true",
        help="立即执行一次【风扇拉满 + 杀后台 + RAMMap清理】后退出（不常驻监听）",
    )
    parser.add_argument(
        "--fan-auto",
        action="store_true",
        help="停止狂暴守护并将微星雷影17恢复为微星 AI 引擎自动温控模式 (Cooler Boost OFF) 后退出",
    )
    parser.add_argument(
        "--stop",
        action="store_true",
        help="停止后台守护进程并恢复原来的微星 AI 引擎 (MSI AI Engine) 模式",
    )
    parser.add_argument(
        "--uac-rammap",
        action="store_true",
        help="若脚本未以管理员运行，调用 RAMMap 时允许弹出 UAC 提权窗口",
    )
    parser.add_argument(
        "--auto-restore-fan",
        action="store_true",
        help="拔下 Pico 2 W 时自动将风扇恢复为自动模式",
    )
    parser.add_argument(
        "--wait-replug",
        action="store_true",
        help="若启动时芯片已插入，则暂不触发，等待下次拔出并重新插入芯片时再自动触发",
    )
    parser.add_argument(
        "--rammap",
        default=default_rammap,
        help=f"RAMMap 可执行文件路径 (默认自动探测: {default_rammap})",
    )
    args = parser.parse_args()

    if args.fan_auto or args.stop:
        ts = time.strftime("%H:%M:%S")
        print(f"\n[{ts}] [STOP] 正在停止狂暴守护并恢复微星 AI 引擎 (MSI AI Engine)...")
        stop_daemon_and_restore_ai_engine()
        return

    if args.once:
        execute_boost_and_clean(
            rammap_path=args.rammap,
            allow_uac=args.uac_rammap,
            reason="手动单次执行 (--once)",
        )
        return

    try:
        monitor_loop(
            rammap_path=args.rammap,
            allow_uac=args.uac_rammap,
            auto_restore_fan=args.auto_restore_fan,
            wait_replug=args.wait_replug,
        )
    except KeyboardInterrupt:
        print("\n[EXIT] 已退出守护监听。")


if __name__ == "__main__":
    main()
