# RC10 – Touch / WLAN UI cleanup

- Custom SafeTap state machine completely removed.
- Buttons and book rows now use native LVGL `LV_EVENT_CLICKED`.
- Touch coordinates and hit-testing are left to the official Waveshare BSP 2.0.3.
- Home cards enlarged from 50 to 56 px.
- Library rows enlarged from 60 to 68 px.
- Reader gestures remain native LVGL `LV_EVENT_GESTURE`.
- WLAN IP address is shown at 24 px; SSID at 18 px.
- German umlauts and common European/special characters are converted to
  readable ASCII fallbacks for the built-in Montserrat fonts.
- EPUB/TXT source data is never changed; conversion is display-only.
