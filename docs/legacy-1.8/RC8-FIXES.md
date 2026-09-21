# RC8 fixes

- Reader gestures now finish on both `LV_EVENT_RELEASED` and `LV_EVENT_PRESS_LOST`. LVGL often emits `PRESS_LOST` once a finger movement is recognized as a gesture; RC7 discarded those swipes.
- A dedicated transparent reader input layer now captures all swipes consistently above the word labels.
- Gesture threshold reduced to 26 px and dominant-axis detection simplified for natural finger movement.
- WPM +/- controls use the same stable press/release guard as menu buttons instead of `LV_EVENT_CLICKED`.
- Menu taps have a 7 px edge guard and 10 px movement limit. Border taps are ignored rather than risking the neighbouring item.
- Home cards are shorter with larger gaps to improve touch separation.
- USB book transfer removed completely. Wi-Fi hotspot upload is now the only transfer method.
- Battery indicator moved 14 px farther left to prevent clipping at the right display edge.
