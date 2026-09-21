#!/usr/bin/env python3
"""Generate Simple RSVP Reader's LVGL Unicode extension fonts.

Source font: vendored NotoSans-Regular.ttf (SIL OFL 1.1).
ASCII is intentionally omitted and falls back to LVGL Montserrat.
"""
from __future__ import annotations

import argparse
from pathlib import Path
from PIL import Image, ImageDraw, ImageFont

CODEPOINTS = sorted(set(list(range(0x00A0, 0x0100)) + [
    0x2007, 0x2009, 0x200A, 0x200B, 0x2010, 0x2011,
    0x2013, 0x2014, 0x2018, 0x2019, 0x201A,
    0x201C, 0x201D, 0x201E, 0x2022, 0x2026, 0x202F,
    0x2060, 0x20AC, 0x2212,
]))
SIZES = [10, 12, 14, 18, 24, 28, 48]


def pack_4bpp(img: Image.Image) -> bytes:
    px = list(img.get_flattened_data())
    out = bytearray()
    for i in range(0, len(px), 2):
        hi = (px[i] + 8) // 17
        lo = (px[i + 1] + 8) // 17 if i + 1 < len(px) else 0
        out.append((min(15, hi) << 4) | min(15, lo))
    return bytes(out)


def fmt_bytes(data: bytes, indent="    ", cols=16) -> str:
    lines=[]
    for i in range(0, len(data), cols):
        lines.append(indent + ", ".join(f"0x{b:02x}" for b in data[i:i+cols]) + ",")
    return "\n".join(lines)


def generate_size(font_path: Path, size: int) -> str:
    font = ImageFont.truetype(str(font_path), size=size)
    ascent, descent = font.getmetrics()
    legacy_metrics = {10: (12, 2), 12: (14, 3), 14: (16, 3), 18: (21, 4), 24: (28, 5),
                      28: (32, 6), 48: (54, 9)}
    bitmaps = bytearray()
    glyphs = [(0,0,0,0,0,0)]

    for cp in CODEPOINTS:
        ch = chr(cp)
        bbox = font.getbbox(ch, anchor="ls")
        advance = font.getlength(ch)
        if bbox is None:
            glyphs.append((len(bitmaps), round(advance*16), 0,0,0,0))
            continue
        left, top, right, bottom = bbox
        w=max(0,right-left); h=max(0,bottom-top)
        if w == 0 or h == 0:
            glyphs.append((len(bitmaps), round(advance*16), 0,0,left,-bottom))
            continue
        mask, mask_offset = font.getmask2(ch, mode="L", anchor="ls")
        if mask.size != (w, h) or mask_offset != (left, top):
            raise RuntimeError(f"unexpected Pillow glyph geometry for U+{cp:04X}: bbox={bbox}, mask={mask.size}/{mask_offset}")
        img = Image.frombytes("L", (w, h), bytes(mask))
        data=pack_4bpp(img)
        idx=len(bitmaps)
        bitmaps.extend(data)
        glyphs.append((idx, round(advance*16), w,h,left,-bottom))

    base=CODEPOINTS[0]
    offsets=[cp-base for cp in CODEPOINTS]
    maxoff=max(offsets)
    fallback=f"lv_font_montserrat_{size}"
    line_height, base_line = legacy_metrics[size]

    lines=[]
    lines.append(f"/* Unicode extension font {size}px generated from Noto Sans Regular 2.004; ASCII falls back to LVGL Montserrat. */")
    lines.append(f"static LV_ATTRIBUTE_LARGE_CONST const uint8_t rsvp_unicode_{size}_bitmap[] = {{")
    lines.append(fmt_bytes(bytes(bitmaps)))
    lines.append("};\n")
    lines.append(f"static const lv_font_fmt_txt_glyph_dsc_t rsvp_unicode_{size}_glyphs[] = {{")
    for idx,adv,w,h,ox,oy in glyphs:
        lines.append(f"    {{.bitmap_index={idx},.adv_w={adv},.box_w={w},.box_h={h},.ofs_x={ox},.ofs_y={oy}}},")
    lines.append("};\n")
    lines.append(f"static const uint16_t rsvp_unicode_{size}_unicode[] = {{")
    for i in range(0,len(offsets),12):
        lines.append("    " + ", ".join(hex(v) for v in offsets[i:i+12]) + ",")
    lines.append("};\n")
    lines.append(f"static const lv_font_fmt_txt_cmap_t rsvp_unicode_{size}_cmaps[] = {{")
    lines.append("    {")
    lines.append(f"        .range_start = {base}, .range_length = {maxoff+1}, .glyph_id_start = 1,")
    lines.append(f"        .unicode_list = rsvp_unicode_{size}_unicode, .glyph_id_ofs_list = NULL,")
    lines.append(f"        .list_length = {len(offsets)}, .type = LV_FONT_FMT_TXT_CMAP_SPARSE_TINY")
    lines.append("    }")
    lines.append("};\n")
    lines.append(f"static const lv_font_fmt_txt_dsc_t rsvp_unicode_{size}_dsc = {{")
    lines.append(f"    .glyph_bitmap = rsvp_unicode_{size}_bitmap,")
    lines.append(f"    .glyph_dsc = rsvp_unicode_{size}_glyphs,")
    lines.append(f"    .cmaps = rsvp_unicode_{size}_cmaps,")
    lines.append("    .kern_dsc = NULL,")
    lines.append("    .kern_scale = 0,")
    lines.append("    .cmap_num = 1,")
    lines.append("    .bpp = 4,")
    lines.append("    .kern_classes = 0,")
    lines.append("    .bitmap_format = 0,")
    lines.append("};\n")
    lines.append(f"const lv_font_t rsvp_unicode_{size} = {{")
    lines.append("    .get_glyph_dsc = lv_font_get_glyph_dsc_fmt_txt,")
    lines.append("    .get_glyph_bitmap = lv_font_get_bitmap_fmt_txt,")
    lines.append(f"    .line_height = {line_height},")
    lines.append(f"    .base_line = {base_line},")
    lines.append("    .subpx = LV_FONT_SUBPX_NONE,")
    lines.append("    .underline_position = -1,")
    lines.append("    .underline_thickness = 1,")
    lines.append(f"    .dsc = &rsvp_unicode_{size}_dsc,")
    lines.append(f"    .fallback = &{fallback},")
    lines.append("    .user_data = NULL,")
    lines.append("};\n")
    return "\n".join(lines)


def main():
    ap=argparse.ArgumentParser()
    ap.add_argument("--font", default=str(Path(__file__).with_name("NotoSans-Regular.ttf")))
    ap.add_argument("--out", default=str(Path(__file__).parents[2] / "main" / "unicode_fonts.c"))
    args=ap.parse_args()
    font_path=Path(args.font)
    out=Path(args.out)
    header='''/*\n * GENERATED FILE - DO NOT EDIT BY HAND\n *\n * Source font: Noto Sans Regular 2.004\n * Copyright: 2010, 2012-2020 Google Inc.; 2015-2020 Google LLC\n * License: SIL Open Font License 1.1\n * License copy: ../licenses/NotoSans-OFL-1.1.txt\n * Generator: tools/font/generate_unicode_fonts.py\n *\n * The generated bitmap subset is distributed under the SIL OFL 1.1.\n */\n\n#include "unicode_fonts.h"\n\n'''
    body="\n".join(generate_size(font_path,s) for s in SIZES)
    out.write_text(header+body, encoding="utf-8", newline="\n")
    print(f"generated {out}")

if __name__ == "__main__":
    main()
