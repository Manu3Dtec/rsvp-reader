# Embedded Font Provenance

## Status

**CLEARED FOR SOURCE REDISTRIBUTION** for this release, subject to the SIL Open Font License 1.1 included in `licenses/NotoSans-OFL-1.1.txt`.

The inherited, provenance-unknown bitmap data from RC15-FIX2 has been replaced completely. `main/unicode_fonts.c` is now generated reproducibly from the vendored font described below.

## Source font

- Family/style: Noto Sans Regular
- Font version: 2.004 (`name` table version string)
- PostScript name: `NotoSans-Regular`
- Font file: `tools/font/NotoSans-Regular.ttf`
- SHA-256: `89c3c497f618fdaa0b2d1e98fef93582f28c71debd2c4a8cdf41f190ced2909d`
- Copyright metadata in the vendored font: `Copyright 2015 Google LLC. All Rights Reserved.`
- License: SIL Open Font License 1.1
- License copy: `licenses/NotoSans-OFL-1.1.txt`
- Upstream family: Noto Sans / Noto project
- Current upstream project: https://github.com/notofonts/latin-greek-cyrillic

The font file is distributed with this source archive so that the generated C data is independently reproducible without relying on a system-installed font.

## Generated file

- Output: `main/unicode_fonts.c`
- Generator: `tools/font/generate_unicode_fonts.py`
- Generator dependency used for this release: Pillow 12.3.0
- Pixel formats: 4 bits per pixel grayscale coverage
- Generated sizes: 14, 18, 24 and 28 px
- ASCII U+0020-U+007E: intentionally not embedded; LVGL Montserrat remains the runtime fallback as before.
- Embedded range: U+00A1-U+00FF plus U+2013, U+2014, U+2018, U+2019, U+201A, U+201C, U+201D, U+201E, U+2022, U+2026 and U+20AC.

Generation from the repository root:

```bash
python tools/font/generate_unicode_fonts.py
```

The script validates the glyph geometry returned by Pillow and emits the sparse LVGL font descriptors used by the firmware.

## Compatibility decision

The four LVGL font objects retain the RC15/RC16 line-height and baseline values:

- 14 px: line height 16, baseline 3
- 18 px: line height 21, baseline 4
- 24 px: line height 28, baseline 5
- 28 px: line height 32, baseline 6

This avoids changing the surrounding UI layout while replacing the actual glyph source.

## OFL note

The generated bitmap subset is a transformed/embedded form of the OFL-licensed Font Software and is distributed under the SIL Open Font License 1.1. The project-level MIT license does not replace or relicense the font material. See `licenses/NotoSans-OFL-1.1.txt` and `THIRD_PARTY_NOTICES.md`.
