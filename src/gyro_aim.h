//
// Gyro-to-Right-Stick aiming, activated by a hard L2 press.
//
// Runs on the per-USB-report copy (after button_remap_apply), so the host only
// ever sees one genuine DualSense whose right stick already contains the gyro
// contribution. Haptics / adaptive triggers / audio paths are untouched.
// Also provides automatic Lightbar Red indicator on hard-press with seamless
// restoration of the game's original Lightbar RGB color on release.
//

#pragma once

#include <cstdint>

struct USBGetStateData;
struct SetStateData;

void gyro_aim_apply(USBGetStateData &report);
void gyro_aim_on_host_output_report(SetStateData &state);
