from pathlib import Path
from PIL import Image, ImageDraw, ImageFont
import sys

source, font_path, project = map(Path, sys.argv[1:])
assets = project / 'assets' / 'images'
original = Image.open(source).convert('RGB')
assert original.size == (940, 940)
canvas = Image.new('RGB', (320, 240), (255, 255, 255))
canvas.paste(original.resize((240, 240), Image.Resampling.LANCZOS), (40, 0))
raw = bytearray()
for r, g, b in canvas.getdata():
    raw.extend((((r >> 3) << 11) | ((g >> 2) << 5) | (b >> 3)).to_bytes(2, 'big'))
(assets / 'live_control_standby.rgb565').write_bytes(raw)

portrait = Image.new('RGB', (240, 320), (255, 255, 255))
portrait.paste(original.resize((240, 240), Image.Resampling.LANCZOS), (0, 40))
raw = bytearray()
for r, g, b in portrait.getdata():
    raw.extend((((r >> 3) << 11) | ((g >> 2) << 5) | (b >> 3)).to_bytes(2, 'big'))
(assets / 'live_control_standby_portrait.rgb565').write_bytes(raw)

size = 16
font = ImageFont.truetype(str(font_path), size)
font.set_variation_by_name('SemiBold')
labels = [ch for ch in (project / 'main' / 'screen_app.c').read_text() if '\u4e00' <= ch <= '\u9fff']
glyphs = []
for letter in sorted(set(labels)):
    glyph = Image.new('L', (size, size))
    left, top, right, bottom = font.getbbox(letter)
    ImageDraw.Draw(glyph).text(((size - right - left) // 2, (size - bottom - top) // 2), letter, font=font, fill=255)
    rows = [sum((glyph.getpixel((x, y)) >= 96) << (size - 1 - x) for x in range(size)) for y in range(size)]
    assert any(rows), letter
    glyphs.append(f'    {{0x{ord(letter):04x}, {{' + ', '.join(f'0x{row:04x}' for row in rows) + '}},')
header = '''// Generated from Noto Serif SC (SIL Open Font License 1.1).
#pragma once

#include <stdint.h>

#define MENU_GLYPH_SIZE 16

static const struct {
    uint16_t codepoint;
    uint16_t rows[MENU_GLYPH_SIZE];
} screen_menu_glyphs[] = {
''' + '\n'.join(glyphs) + '\n};\n'
(project / 'main' / 'screen_menu_glyphs.h').write_text(header)
