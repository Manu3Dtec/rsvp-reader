# RC2 stabilization pass

Target: Waveshare ESP32-S3 Touch-LCD-3.49B V2 (Rev1.1), ESP-IDF 6.0.2.

RC2 consolidates all RC1.x build fixes and adds a full static stabilization pass:

- AXS15231B dependency updated to the ESP-IDF-6 compatible 2.x line.
- `esp_timer` component dependency declared for the local BSP and main component.
- ESP-IDF 6 GPIO sentinel types use `GPIO_NUM_NC` rather than raw `-1`.
- C++ array sizing no longer depends on `std::size`.
- All UI/navigation functions used before their definitions have forward declarations.
- Missing `delete_ui_timers()` and `show_loading()` helpers are present.
- Removed the incomplete two-command AXS15231B custom init table; the driver's complete default initialization is used.
- Explicit `esp_lcd_panel_disp_on_off(..., true)` after panel initialization.
- Touch packet length corrected to one AXS15231B point (8 bytes).
- Touch coordinates corrected from native 172x640 portrait to 640x172 landscape.
- Added explicit QSPI RASET before every DMA strip to make rotated strip rendering deterministic even with AXS15231B driver versions that skip RASET in QSPI partial transfers.
- Removed duplicate `esp_heap_caps.h` include.

Hardware validation is still required on the physical board. A successful compiler build alone cannot validate display orientation, touch orientation, battery calibration, SD electrical behavior, or PWR hold timing.
