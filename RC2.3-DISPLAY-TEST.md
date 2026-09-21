# RC2.3 Display Test

Diagnostic build for Waveshare ESP32-S3 Touch-LCD-3.49B V2.

Before LVGL starts, the firmware sends four native 172x640 full-screen frames directly through the AXS15231B driver: WHITE, RED, GREEN, BLUE, each for 2 seconds.

Interpretation:
- Colors visible: raw panel/QSPI path works; investigate LVGL rotation/flush next.
- Still black: investigate panel reset/init/QSPI/backlight against Waveshare V2 reference.

This is a diagnostic build, not a release build.
