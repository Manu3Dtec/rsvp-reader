# 3.49B V2 RC1 status

This package is the first complete source port candidate for the Waveshare ESP32-S3 Touch-LCD-3.49B V2 / Rev 1.1.

## Completed in source

- replaced the 1.8-inch Waveshare BSP dependency with a dedicated 3.49B V2 BSP
- AXS15231B QSPI display path for a 640×172 logical landscape surface
- touch I2C on GPIO17/GPIO18, address 0x3B
- system I2C on GPIO47/GPIO48
- TCA9554 LCD reset/backlight/SYS_EN handling
- SDMMC on GPIO39/GPIO40/GPIO41
- PCF85063 RTC retained
- battery ADC moved to ADC1 channel 3 / GPIO4 using Waveshare's calibrated ×3 method
- PWR moved to GPIO16 and software hold timing
- >5 s board shutdown request through TCA9554 SYS_EN
- Reader/UI screens adapted to 640×172
- RC16 EPUB/TXT/Reader/Wi-Fi/settings logic retained
- existing MIT/OFL open-source packaging retained

## Checks performed without hardware

- source tree/braces and duplicate-definition consistency scan
- old 1.8 BSP dependency removed from active manifests
- active source contains no AXP2101 power implementation
- dimensions scan found no remaining portrait 3.49-incompatible large UI containers in active Reader UI
- ZIP integrity check is performed for the packaged artifact

## Not claimed

No successful ESP-IDF 6.0.2 compile is claimed from the assistant environment because that toolchain is not installed there. No physical display/touch/SD/PWR test is claimed.

The first real validation must therefore be:

1. `idf.py build` under ESP-IDF 6.0.2
2. flash to the physical 3.49B V2
3. serial-log review
4. display rotation/color test
5. touch orientation test
6. SD/RTC/battery test
7. PWR ~2 s standby/resume and >5 s SYS_EN shutdown test
