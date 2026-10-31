# Live check for the gyro-L2 firmware: prints L2 / right stick / raw gyro for 20 s.
# Hard-press L2 and rotate the controller: RX/RY should move away from ~128.
import ctypes, struct, time, winreg

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


h = find_path()
if not h:
    raise SystemExit("DualSense (DS5-Bridge) not found")
buf = (ctypes.c_ubyte * 64)()
n = ctypes.c_ulong()
t_end = time.time() + 20
last = 0
while time.time() < t_end:
    k32.ReadFile(h, ctypes.byref(buf), 64, ctypes.byref(n), None)
    b = bytes(buf[: n.value])
    if not b or b[0] != 0x01 or time.time() - last < 0.1:
        continue
    last = time.time()
    p, y, r = struct.unpack_from("<hhh", b, 16)
    state = "GYRO ON " if b[5] >= 230 else "        "
    print(f"{state} L2={b[5]:3d}  RX={b[3]:3d} RY={b[4]:3d}  gyro(pitch,yaw,roll)=({p:6d},{y:6d},{r:6d})")
k32.CloseHandle(h)
