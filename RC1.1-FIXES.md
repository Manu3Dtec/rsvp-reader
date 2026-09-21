# RC1.1 fixes

- Updated `espressif/esp_lcd_axs15231b` from `^1.0.1` to `^2.1.0` for ESP-IDF 6.0.x compatibility.
- ESP-IDF 6.0 removed `esp_lcd_panel_dev_config_t::color_space`; the 2.x AXS15231B driver uses `rgb_ele_order`.
- No application feature changes from RC1.

## Required clean rebuild
Delete stale generated dependency/build state before building RC1.1:

```powershell
Remove-Item -Recurse -Force .\build -ErrorAction SilentlyContinue
Remove-Item -Recurse -Force .\managed_components -ErrorAction SilentlyContinue
Remove-Item -Force .\dependencies.lock -ErrorAction SilentlyContinue
idf.py reconfigure
idf.py build
```
