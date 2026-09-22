<p align="center"><img src="docs/assets/logo.png" width="520" alt="Simple RSVP Reader logo"></p>

<p align="center"><a href="https://ko-fi.com/manu3dtec"><img src="https://ko-fi.com/img/githubbutton_sm.svg" alt="Support Manu3Dtec on Ko-fi"></a></p>

# Simple RSVP Reader

Open-source RSVP speed-reading firmware for the **Waveshare ESP32-S3 Touch-LCD-3.49B V2 / Rev 1.1**.

## Install without development tools

**[Open the browser installer](https://manu3dtec.github.io/rsvp-reader/installer/)** using desktop Chrome or Edge. Connect the board by USB, click **Connect & install**, select its serial port, and follow the prompts. No ESP-IDF setup or command line is required.

The installer files are stored in `docs/installer`. Advanced users can still build and flash the project with ESP-IDF using the scripts in this repository.

## Hardware target

- ESP32-S3
- 3.49-inch 172×640 LCD, used by this firmware as **640×172 landscape**
- AXS15231B QSPI display controller
- touch controller at I2C address `0x3B`
- TCA9554 I/O expander
- PCF85063 RTC (`0x51`)
- microSD through 1-bit SDMMC
- battery ADC on GPIO4 / ADC1 channel 3
- PWR button on GPIO16
- board power-off through TCA9554 `SYS_EN`

The hardware mapping is based on Waveshare's official `ESP32-S3-Touch-LCD-3.49-V2` examples. No 1.8-inch GPIO mapping is reused.

## Main functions retained from RC16

- EPUB and TXT library/reader
- RSVP word rendering and adjustable WPM
- reading position persistence and recent books
- Wi-Fi upload portal
- RTC clock and battery indicator
- brightness and auto-display-off settings
- punctuation timing
- Unicode font set generated from Noto Sans (OFL-1.1)
- PWR standby lockscreen: hold about 2 s
- touch disabled while standby is active
- second ~2 s PWR hold returns to previous Reader/UI state
- continuous PWR hold beyond 5 s requests board power-off through `SYS_EN`

## Build target

Designed for **ESP-IDF 6.0.2**.

```powershell
cd C:\esp-idf-v6.0.2
.\export.ps1
cd C:\Simple-RSVP-Reader-3.49B-V2
python C:\esp-idf-v6.0.2\tools\idf.py fullclean
python C:\esp-idf-v6.0.2\tools\idf.py set-target esp32s3
python C:\esp-idf-v6.0.2\tools\idf.py build
```

Flash with the correct COM port:

```powershell
python C:\esp-idf-v6.0.2\tools\idf.py -p COM5 flash monitor
```

## Validation status

The source has undergone static consistency checks in this package. A physical 3.49B V2 board was **not** available in the assistant environment, so no hardware test is claimed. See `PWR-TEST-CHECKLIST.md` and `PORTING-349B-V2.md` for the remaining hardware validation points.

## Origin

Simple RSVP Reader is an independent open-source port and adaptation inspired by and derived in part from RSVP Nano. It is not an official RSVP Nano release. RSVP Nano is licensed under the MIT License; the original copyright and license notice are retained in this repository.

Original project: https://github.com/ionutdecebal/rsvpnano
