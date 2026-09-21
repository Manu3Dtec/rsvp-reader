# RC11 – Raw Touch + Unicode + 25 WPM

## Reader touch
Reader gestures no longer depend on an LVGL object receiving `LV_EVENT_GESTURE`.
A callback is registered directly on the BSP's LVGL input device returned by
`bsp_display_get_input_dev()`.

The handler records the raw start/end coordinates and classifies the dominant
movement axis:
- swipe up: +25 WPM
- swipe down: -25 WPM
- swipe left: next word
- swipe right: previous word
- tap in reader area: start/pause

The header/back-button region is excluded from reader gesture handling.

## WPM
All speed changes now use 25-WPM steps. Settings buttons are `-25` and `+25`.

## Unicode
RC11 adds compact Unicode extension fonts at 14/18/24/28 px. ASCII still falls
back to LVGL Montserrat, while Latin-1 and common EPUB punctuation are rendered
as real UTF-8 glyphs. Thus `ä`, `ö`, `ü`, `Ä`, `Ö`, `Ü`, `ß`, `é`, `€`,
typographic quotes, dashes and ellipsis are no longer transliterated.

## WLAN screen
SSID, password and IP address are all shown in 24-px text for easier reading.
