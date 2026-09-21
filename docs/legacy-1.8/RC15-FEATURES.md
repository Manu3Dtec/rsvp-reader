# RC15 – Reader convenience features

Implemented:
- per-book progress percentage plus EPUB chapter x/y
- three recently opened books on Home
- brightness setting (20–100%)
- auto display-off: Off / 1 / 2 / 5 / 10 / 20 min
- reader lock; hold 1 second in reader area to unlock
- configurable sentence-end pause (+0…200%)
- configurable comma/colon/semicolon/dash pause (+0…150%)
- jump back to current sentence start
- delete books from Library with confirmation
- microSD free/total storage display
- RTC clock + battery indicator retained
- RC14 direct touch polling retained
- WPM remains in 25-step increments

EPUB chapter tracking:
The EPUB extraction cache now writes a `.chap` sidecar containing byte offsets
for each readable spine item. The ReaderEngine loads this sidecar and reports
the current chapter. TXT files keep percentage progress only.

Auto-sleep:
The display backlight turns off after the selected idle period. Reader playback
prevents auto-sleep. Touch activity wakes the display.
