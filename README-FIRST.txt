Simple RSVP Reader – 3.49B V2 PORT RC1
Target: Waveshare ESP32-S3 Touch-LCD-3.49B V2 / Rev 1.1
Logical UI: 640x172 landscape
Display: AXS15231B QSPI
Touch: I2C 0x3B
ESP-IDF: 6.0.2

PWR:
- hold ~2s: standby / lockscreen, reader pauses, touch disabled
- hold ~2s again: resume previous Reader/UI state, touch enabled
- keep holding >5s: TCA9554 SYS_EN low -> board power-off request

IMPORTANT:
This package was statically checked but not tested on physical hardware.
Build and first-flash results must be verified on the actual 3.49B V2 board.
