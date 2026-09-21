# RC2.2 Power Optimization (3.49B V2)

- Dynamic frequency scaling enabled: 80-240 MHz, automatic light-sleep disabled for peripheral stability.
- Standby brightness capped at 10%.
- Standby clock refresh reduced from 1 s to 60 s.
- Battery ADC sampling reduced from ~1 s to 30 s.
- Battery UI refresh reduced from 3 s to 15 s.
- Auto display-off now disables both backlight and LCD panel; panel is re-enabled on touch activity.
- Wi-Fi teardown behavior retained (stop + deinit after transfer).
- Existing PWR/SYS_EN behavior retained.
