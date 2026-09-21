# 3.49B V2 port notes

## Verified Waveshare V2 mapping used by this port

- LCD CS: GPIO9
- LCD CLK: GPIO10
- LCD D0/D1/D2/D3: GPIO11/12/13/14
- LCD backlight PWM: GPIO42
- system I2C SDA/SCL: GPIO47/GPIO48
- touch I2C SDA/SCL: GPIO17/GPIO18
- touch address: 0x3B
- TCA9554 interrupt: GPIO8
- PWR/SYS_OUT input: GPIO16, active-low button handling
- battery ADC: GPIO4 = ADC1 channel 3
- SDMMC CMD/D0/CLK: GPIO39/GPIO40/GPIO41
- RTC: PCF85063 address 0x51
- TCA9554 SYS_EN: bit 6

## Display orientation

The controller native framebuffer is 172×640. Simple RSVP Reader uses a 640×172 LVGL landscape surface. The BSP rotates dirty areas into the native framebuffer before submitting QSPI transfers to AXS15231B.

## Power behavior

The 1.8-V2 AXP2101 implementation is not reused. The 3.49B V2 implementation polls the official PWR input GPIO16. A ~2 second software threshold toggles standby. Holding continuously beyond 5 seconds drives `SYS_EN` low through the TCA9554, matching Waveshare's board-level power-off mechanism.

## Hardware-dependent checks still required

- AXS15231B pixel byte order and rotation on the physical panel
- touch orientation and edge coordinates
- PWM polarity/brightness range
- SYS_EN long-hold shutdown behavior
- battery percentage curve (the ADC voltage acquisition is based on Waveshare's example; percentage mapping is an application estimate)
- Wi-Fi + display DMA stability under upload traffic
