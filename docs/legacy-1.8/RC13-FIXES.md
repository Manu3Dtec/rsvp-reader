# RC13 – Wi-Fi DMA memory fix + RTC clock

## Wi-Fi exit / AMOLED DMA failure
Observed log:
`spicommon_dma_setup_priv_buffer: Failed to allocate priv TX buffer`

Cause addressed:
- `esp_wifi_stop()` stopped the radio but left the Wi-Fi driver allocated.
- The Home screen was redrawn immediately while Wi-Fi still held scarce
  internal/DMA-capable RAM required by the SPI AMOLED transfer path.

RC13:
- stops HTTP;
- calls `esp_wifi_stop()`;
- calls `esp_wifi_deinit()`;
- destroys the default AP netif;
- waits briefly for allocator/scheduler handoff;
- logs free/largest DMA/internal heap before and after teardown;
- does **not** redraw Home until the Wi-Fi teardown has completed.

## Clock
- Uses the onboard PCF85063A RTC at I2C address 0x51.
- Small `HH:MM` clock is shown below the battery indicator.
- The browser upload page synchronizes the RTC from the phone/PC local time
  whenever `192.168.4.1` is opened.
- The RTC then keeps time independently of Wi-Fi.
