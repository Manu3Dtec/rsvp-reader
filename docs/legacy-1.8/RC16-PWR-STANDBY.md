# RC16 PWR / Standby – Waveshare ESP32-S3 Touch AMOLED 1.8 V2

## Added

- Hardware PWR key handled through the **AXP2101 PMIC**, not by guessing an ESP32 GPIO.
- AXP2101 long-press IRQ threshold: **2 seconds**.
- 2 s PWR hold toggles a standby lockscreen.
- Standby lockscreen shows RTC time and battery state.
- LVGL touch input is disabled while the lockscreen is active.
- Reader is paused and position saved before entering standby.
- A second 2 s PWR hold restores touch and returns to the Reader if standby was entered from the Reader.
- If standby is entered from another screen, wake returns to Home.
- Wi-Fi upload teardown is completed before drawing the standby screen to preserve internal/DMA memory.
- AXP2101 hardware long-press power-off is enabled at **6 seconds** (the first setting longer than 5 s supported by the PMIC).
- AXP2101 hardware power-on hold is configured to **2 seconds**, which is the maximum ONLEVEL supported by the AXP2101. A >5 s cold-start requirement cannot be configured in this PMIC.

## AXP2101 registers used

- `0x22` PWROFF_EN: enable long-press shutdown, select power-off instead of restart.
- `0x27` IRQ/OFF/ON timing: 2 s IRQ, 6 s off, 2 s on.
- `0x41` INTEN2: enable POWERON long-press IRQ.
- `0x49` INTSTS2: poll and clear the latched POWERON long-press event.

The PMU IRQ line is deliberately not used. The status bit is polled over the already initialized Waveshare BSP I2C bus, avoiding any board-revision GPIO assumptions.

## Preserved from RC15-FIX2

All existing reader, EPUB/TXT, Wi-Fi upload, battery, RTC, brightness, auto-sleep, punctuation timing, recent books, chapter progress, deletion, storage display, Unicode, and direct touch-polling features remain. `Satz` and `Sperren` remain removed.
