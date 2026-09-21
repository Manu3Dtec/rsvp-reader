# RC12 – Vertical Reader Gesture Fix

The reader touch callback itself already works because tap start/stop and
left/right word navigation work. RC12 therefore changes only vertical gesture
recognition.

Changes:
- Up/down is detected during `LV_EVENT_PRESSING`, not only at release.
- Vertical threshold reduced to 12 px.
- One gesture can trigger only once per touch.
- Vertical movement wins whenever |dy| >= |dx|.
- Horizontal word navigation remains at 18 px threshold.
- Release only toggles play/pause if no swipe was fired.
- WPM remains in 25-step increments.

This specifically addresses touch controllers that report an unreliable final
Y coordinate at `LV_EVENT_RELEASED`.
