# RC2.1 Display Fix

Changes relative to RC2:

- Match Waveshare V2 AXS15231B wake sequence: Sleep Out (0x11), 100 ms, Display On (0x29), 100 ms.
- Backlight PWM changed to the official 50 kHz and startup brightness set to 100% for bring-up.
- BL_EN remains enabled through TCA9554.
- Removed the custom RASET workaround from the flush path.
- Full-frame 640x172 software rotation is sent as native 172x640 DMA strips.
- RGB565 bytes are swapped before transfer, matching Waveshare's official LVGL v9 example.
- Added serial logs for panel init, backlight enable, and first completed LVGL frame.

This release addresses a black-screen condition seen on real 3.49B V2 hardware after a successful boot.
