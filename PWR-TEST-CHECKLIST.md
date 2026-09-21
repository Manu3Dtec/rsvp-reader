# 3.49B V2 PWR / standby hardware test checklist

Hardware target: Waveshare ESP32-S3 Touch-LCD-3.49B V2 / Rev 1.1.

1. Boot and serial log
   - expect `3.49B V2 battery/PWR monitor ready`
   - expect `Display ready: 640x172 landscape, AXS15231B QSPI`
2. Normal UI touch
   - library/settings/Wi-Fi controls respond across the full 640×172 area
   - verify coordinate orientation; no mirrored X/Y
3. PWR ~2 seconds
   - reader pauses and position is saved if Reader is active
   - standby clock appears
   - battery indication remains visible
   - touch has no effect while standby is active
4. PWR ~2 seconds again
   - touch is re-enabled
   - previous Reader/UI state returns
5. PWR continuous >5 seconds
   - standby may appear at ~2 s first
   - after >5 s firmware drives TCA9554 `SYS_EN` low
   - board must power off as in Waveshare's official V2 power example
6. Battery
   - compare displayed percentage trend with measured battery voltage
   - ADC voltage uses Waveshare's official ADC1 channel 3 / GPIO4 calibration and ×3 divider compensation
7. SD
   - verify card mount with CMD=GPIO39, D0=GPIO40, CLK=GPIO41
8. RTC
   - verify PCF85063 time persists and standby clock updates

No item above is marked as hardware-passed until tested on the physical board.
