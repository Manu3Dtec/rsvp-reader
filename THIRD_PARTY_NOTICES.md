# Third-Party Notices

## RSVP Nano
- Upstream: https://github.com/ionutdecebal/rsvpnano
- License: MIT
- Original copyright/license notice retained in `LICENSE`.

## Espressif ESP-IDF
- Framework: ESP-IDF
- License: Apache License 2.0 (with component-specific notices where applicable)
- https://github.com/espressif/esp-idf

## LVGL
- Dependency: `lvgl/lvgl`
- License: MIT
- https://github.com/lvgl/lvgl

## Espressif AXS15231B LCD component
- Dependency: `espressif/esp_lcd_axs15231b` (`^2.1.0`)
- Used for the AXS15231B panel transport/driver.
- License information is supplied by the ESP Component Registry package and must remain available with redistributed dependency source/binaries as required by that package.

## Espressif TCA9554 I/O-expander component
- Dependency: `espressif/esp_io_expander_tca9554` (`^2.0.1`)
- Used for LCD reset/backlight enable and board `SYS_EN` control.
- License information is supplied by the ESP Component Registry package.

## Waveshare hardware reference material
- Hardware mappings and board behavior were verified against Waveshare's official repository:
  https://github.com/waveshareteam/ESP32-S3-Touch-LCD-3.49-V2
- This repository does not vendor the Waveshare example project wholesale; the Simple RSVP Reader BSP is an independent implementation using the documented/official board mapping and public ESP-IDF APIs.

## Noto Sans
- Font source used to generate `main/unicode_fonts.c`: Noto Sans Regular
- License: SIL Open Font License 1.1
- Full license: `licenses/NotoSans-OFL-1.1.txt`
- Provenance: `FONT_PROVENANCE.md`
