# RC14 – Reliable Reader Touch Polling

RC14 removes reader gesture handling from LVGL input-device event callbacks.

The reader now polls the BSP touch input device every 8 ms using:
- `lv_indev_get_state()`
- `lv_indev_get_point()`

This is independent from `LV_EVENT_GESTURE`, `LV_EVENT_PRESSING`, and
`lv_indev_add_event_cb()` delivery.

Reader controls:
- swipe up: +25 WPM
- swipe down: -25 WPM
- swipe left: next word
- swipe right: previous word
- tap: start/pause

Gestures are recognized while the finger is still down at 16 px movement.
Only one action can fire per touch. Horizontal word navigation pauses reading.
Vertical WPM changes also pause reading to make the result immediately visible.

Serial diagnostics:
- `Reader touch DOWN ...`
- `Reader POLL swipe UP ...`
- `Reader POLL swipe DOWN ...`
- `Reader POLL swipe LEFT ...`
- `Reader POLL swipe RIGHT ...`
- `Reader POLL tap ...`
