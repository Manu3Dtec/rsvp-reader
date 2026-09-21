#pragma once

#include <cstdint>

namespace BatteryMonitor {

struct Status {
    bool available = false;
    bool battery_present = false;
    bool vbus_present = false;   // Not exposed by the 3.49B V2 reference circuit/API used here.
    bool charging = false;       // Not exposed by the 3.49B V2 reference circuit/API used here.
    uint8_t percent = 0;
};

// ESP32-S3 Touch-LCD-3.49B V2 implementation:
// - battery voltage: ADC1 channel 3 / GPIO4, 12 dB attenuation, calibrated,
//   multiplied by 3 exactly as Waveshare's official V2 ADC example.
// - PWR button: GPIO16, active low, as in Waveshare's official V2 PWR example.
// - A new hold beyond 2 s requests power-off; the boot hold is ignored.
// - BOOT GPIO0 short press requests the main menu.
bool start();
Status status();
bool consume_poweroff_request();
bool consume_home_request();
bool power_key_ready();

}  // namespace BatteryMonitor
