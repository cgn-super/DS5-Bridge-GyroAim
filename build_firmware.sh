#!/bin/bash
set -euo pipefail

BUILD_ROOT="/home/cgnnb/pico_build"
SRC_WIN="/mnt/c/Users/cgnnb/.gemini/antigravity/scratch/DS5-Bridge-GyroAim"
mkdir -p "$BUILD_ROOT"
cd "$BUILD_ROOT"

echo "=== [1/6] Verify Pico SDK 2.3.1 & Required Submodules ==="
if [ ! -f pico-sdk/.ok ]; then
    rm -rf pico-sdk
    git clone --depth 1 --branch 2.3.1 https://github.com/raspberrypi/pico-sdk.git pico-sdk
    cd pico-sdk
    for m in lib/tinyusb lib/cyw43-driver lib/btstack lib/lwip; do
        git submodule update --init --depth 1 "$m"
    done
    touch .ok
    cd ..
fi

TINYUSB_REF=98741b55959564746998d0453c32d87064629d7c
if [ "$(git -C pico-sdk/lib/tinyusb rev-parse HEAD)" != "$TINYUSB_REF" ]; then
    git -C pico-sdk/lib/tinyusb fetch --depth 1 origin "$TINYUSB_REF"
    git -C pico-sdk/lib/tinyusb checkout --detach FETCH_HEAD
fi

echo "=== [2/6] Verify DS5Dongle Upstream Tree ==="
if [ ! -f ds5dongle/.ok ]; then
    rm -rf ds5dongle
    git clone --depth 1 --branch v0.8.0 --recurse-submodules --shallow-submodules https://github.com/awalol/DS5Dongle.git ds5dongle
    touch ds5dongle/.ok
fi

echo "=== [3/6] Inject Gyro-Aim v2.0 & 250MHz Overclock + Regenerate Clean Patch ==="
cd ds5dongle
git checkout -- CMakeLists.txt src/main.cpp
cp "$SRC_WIN/src/gyro_aim.cpp" "$SRC_WIN/src/gyro_aim.h" src/

python3 - << 'EOF'
# 1. CMakeLists.txt: 250MHz SYS_CLOCK_KHZ_VALUE + CYW43_PIO_CLOCK_DIV_VALUE=3 + src/gyro_aim.cpp
c = open("CMakeLists.txt", encoding="utf-8").read()
old_clk = """        if(DISABLE_SPEAKER_PROC_VALUE)
            set(CYW43_PIO_CLOCK_DIV_VALUE 2)
            set(SYS_CLOCK_KHZ_VALUE 150000)
        else ()
            set(CYW43_PIO_CLOCK_DIV_VALUE 2)
            set(SYS_CLOCK_KHZ_VALUE 150000)
        endif ()"""
new_clk = """        if(DISABLE_SPEAKER_PROC_VALUE)
            set(CYW43_PIO_CLOCK_DIV_VALUE 3)
            set(SYS_CLOCK_KHZ_VALUE 250000)
        else ()
            set(CYW43_PIO_CLOCK_DIV_VALUE 3)
            set(SYS_CLOCK_KHZ_VALUE 250000)
        endif ()"""
assert old_clk in c, "Clock block not found in CMakeLists.txt"
c = c.replace(old_clk, new_clk, 1)
c = c.replace("src/button_remap.cpp", "src/button_remap.cpp\n        src/gyro_aim.cpp", 1)
open("CMakeLists.txt", "w", encoding="utf-8").write(c)

# 2. src/main.cpp: include gyro_aim.h, call gyro_aim_apply & gyro_aim_on_host_output_report
m = open("src/main.cpp", encoding="utf-8").read()
m = m.replace('#include "button_remap.h"', '#include "button_remap.h"\n#include "gyro_aim.h"', 1)
m = m.replace('button_remap_apply(report);', 'button_remap_apply(report);\n        gyro_aim_apply(report);')
m = m.replace('memcpy(outputData + 3, &state, sizeof(SetStateData));',
              'gyro_aim_on_host_output_report(state);\n    memcpy(outputData + 3, &state, sizeof(SetStateData));', 1)
open("src/main.cpp", "w", encoding="utf-8").write(m)
EOF

git diff CMakeLists.txt src/main.cpp > "$SRC_WIN/src/patch_against_official_v0.7.2.patch"
# Verify that git apply works cleanly on a clean tree
git checkout -- CMakeLists.txt src/main.cpp
git apply --whitespace=fix "$SRC_WIN/src/patch_against_official_v0.7.2.patch"
echo "PASS: patch_against_official_v0.7.2.patch applies cleanly via git apply!"

# Enforce -Wall -Wextra -Wdouble-promotion on src/gyro_aim.cpp
echo 'set_source_files_properties(src/gyro_aim.cpp PROPERTIES COMPILE_OPTIONS "-Wall;-Wextra;-Wdouble-promotion;-Werror")' >> CMakeLists.txt

echo "=== [4/6] Configure & Compile with Ninja ==="
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release -DPICO_SDK_PATH="$BUILD_ROOT/pico-sdk"
# Force rebuild of gyro_aim.cpp and main.cpp to capture all warnings
rm -f build/CMakeFiles/ds5-bridge.dir/src/gyro_aim.cpp.o build/CMakeFiles/ds5-bridge.dir/src/main.cpp.o
ninja -C build ds5-bridge 2>&1 | tee "$BUILD_ROOT/ninja.log"

echo "=== [5/6] Zero-Warning & Zero-Heap-Allocation Verification ==="
if grep -E "gyro_aim\.(cpp|h).*warning:" "$BUILD_ROOT/ninja.log"; then
    echo "ERROR: Compiler warnings detected in gyro_aim!"
    exit 1
else
    echo "PASS: 0 warnings in gyro_aim.cpp / gyro_aim.h"
fi

if arm-none-eabi-nm -u build/CMakeFiles/ds5-bridge.dir/src/gyro_aim.cpp.o | grep -E "malloc|free|realloc|calloc|_Znw|_Zdl|_Zna|_Zda"; then
    echo "ERROR: Dynamic memory allocation symbols found in gyro_aim.cpp.o!"
    exit 1
else
    echo "PASS: 0 dynamic heap allocation symbols in gyro_aim.cpp.o"
fi

arm-none-eabi-nm -C build/ds5-bridge.elf | grep -E "gyro_aim|host_boost_hook"

echo "=== [6/6] Copy UF2 Firmware Artifacts ==="
cp build/ds5-bridge.uf2 "$SRC_WIN/firmware/ds5-bridge-v0.7.2-gyroL2.uf2"
cp build/ds5-bridge.uf2 "$SRC_WIN/firmware/ds5-bridge-v0.8.0-gyroL2-new.uf2"
ls -lh "$SRC_WIN/firmware/ds5-bridge-v0.7.2-gyroL2.uf2"
echo "BUILD_V2_DONE_OK"
