#!/usr/bin/env python3
"""The race intro's course info box on the lower screen (src/race_bottom_screen.cpp): Mario Kart 7's
flyover box in the standings rows' frame, with a gold medallion holding the course's cup icon, the
cup and the course name. Everything comes from the player's disc: the race font (kart_kanji_font),
the menus' selected button and its border loop (common_w004_menu), the menus' cup icons.

The frame: the rows' frame (the menus' selected button, retail palette slot 1, see-through inside
darkened = cook_race_map.py row_frames) rendered by the menu cook's renderer (menu_layout.py) with
the menus' border loop (BattleSetting's Loop group, common_w004_menu_fuchi_check_loop: fuchi_pattern
RLTS +0.025 a frame, one period of its pattern every 40 frames, moving left), baked as one periodic
strip per long edge and the short edges (with the corners) per phase. Pinned on its top edge, a gold
medallion (a generated ring, its bright arc turned at run time) holds the course's cup icon, one small
file per cup, copied into the frame art's emblem cell when the race loads; a checkered flag
(generated) if none.

Writes include/mkw/flyover_info_assets.hpp and build/flyover-info/ (textures, flyover-info.json: the
packaged files and their hashes).
"""
import argparse
import hashlib
import json
import re
from pathlib import Path

import cook_race_flow
from cook_race_flow import texture_file, font_glyphs, pack_atlas
from cook_race_items import decode_tpl

ROOT = cook_race_flow.ROOT
FLY_PX = (18, 14)          # the name line, the cup line
FLY_BOX_W = 312            # the box's width
FLY_EDGE = 4               # drawn border: cook_race_map.py nine_slice(corner=8, drawn=4)
FLY_CORNER = 8
FLY_SIDE_MID = 26          # stored height of a short edge's middle (stretched at run time)
FLY_PHASES = 30            # short-edge phases per loop period
FLY_LOOP = 40              # source frames per loop period
FLY_ROW_INSIDE = (10, 12, 14, 160)  # cook_race_map.py ROW_INSIDE
FLY_ICON = 24              # a cup icon's size in the ring
FLY_EMBLEM = 26            # the flag's size
FLY_EMBLEM_CELL = 32       # an emblem file (RGBA8, the icon centred) = the frame art's emblem cell
FLY_RING = 40              # the medallion's ring (drawn 1:1 at rest)
# Cups: catalog 0-31 four to a cup, arenas 32-36 Wii Stages and 37-41 Retro Stages (menu_ui.cpp);
# their menu icons (cook_menu_ui.py cup_N / battle_cup_N).
FLY_CUP_NAMES = ['Mushroom Cup', 'Flower Cup', 'Star Cup', 'Special Cup', 'Shell Cup', 'Banana Cup', 'Leaf Cup',
                 'Lightning Cup', 'Wii Stages', 'Retro Stages']
FLY_CUP_ICONS = [f'tt_cup_icon_{n}_00.tpl' for n in ('kinoko', 'flower', 'star', 'oukan', 'koura', 'banana', 'konoha', 'thunder')] + \
                ['tt_wii_icon_128.tpl', 'tt_classic_icon_sphere_128.tpl']


def course_names(root):
    """The catalog's course names, as the runtime's Courses[] has them (include/mkw/course_catalog.hpp)."""
    text = (root / 'include/mkw/course_catalog.hpp').read_text(encoding='utf-8')
    return re.findall(r'^\{"([^"]+)",Kinoko::Course::', text, re.M)


def course_cup(i):
    return i // 4 if i < 32 else 8 + (i - 32) // 5


def fly_button_frames(times, root):
    """The menus' selected button at loop times `times` (frames, may be fractional), as the rows'
    frame: cook_menu_ui.py bake_panel('button', select) with the retail palette slot 1
    (ui_button_retail_select.png), cropped to frame 0's bounds, inside darkened."""
    import struct
    import numpy as np
    from PIL import Image
    from menu_layout import render_layout, controller_animations
    from verify_menu_retail import BASE
    ui = root / 'assets/menu-ui/MenuSingle/button'
    layout = ui / 'blyt/common_w004_menu.brlyt'
    control = next(c for c in sorted((ui / 'ctrl').glob('*.brctr')) if not c.name.startswith('._') and
                   controller_animations(c)['layout'] == layout.stem)
    wanted = {'Select': 'Select', 'Loop': 'Loop', 'SelectIn': 'SelectStop', 'OK': 'OKStop', 'Active': 'Active'}
    anims, groups = [], []
    for g in controller_animations(control)['groups']:
        for e in g['animations']:
            if e['name'] == wanted.get(g['name']):
                anims.append(ui / 'anim' / e['file'])
                groups.append(g['panes'])
    loop = groups.index('fuchi_pattern')
    raw = (root / 'build/itembox-retail.rel').read_bytes()
    c0, c1, base = [list(struct.unpack_from('>4h', raw, 0x8088fd18 - BASE + 24 + i * 8)) for i in range(3)]
    colours = {'fuchi_pattern': [c0, c1, [255, 255, 255, 255]], 'color_base': [base, base, [255, 255, 255, 255]]}
    images = []
    for t in times:
        at = [0] * len(anims)
        at[loop] = t
        image, _, _ = render_layout(layout, lambda p: None, anims, exclude=('text',), material_overrides=colours,
                                    animation_groups=groups, animation_frames=at)
        images.append(image.convert('RGBA'))
    box = images[0].getbbox()
    out = []
    for image in images:
        a = np.asarray(image.crop(box)).copy()
        a[a[..., 3] < 160] = FLY_ROW_INSIDE
        out.append(Image.fromarray(a, 'RGBA'))
    return out


def fly_nine_slice(src, width, height):
    """cook_race_map.py nine_slice (corner 8, drawn 4)."""
    from PIL import Image
    out = Image.new('RGBA', (width, height))
    sw, sh = src.size
    c, d = FLY_CORNER, FLY_EDGE
    xs = [(0, c, 0, d), (c, sw - c, d, width - d), (sw - c, sw, width - d, width)]
    ys = [(0, c, 0, d), (c, sh - c, d, height - d), (sh - c, sh, height - d, height)]
    for a0, a1, b0, b1 in xs:
        for e0, e1, f0, f1 in ys:
            if b1 > b0 and f1 > f0:
                out.paste(src.crop((a0, e0, a1, e1)).resize((b1 - b0, f1 - f0), Image.Resampling.LANCZOS), (b0, f0))
    return out


def fly_coverage(size, r0, r1, ss=4):
    """Per pixel of a size x size image, the share of it between radii r0 and r1 of the centre."""
    import numpy as np
    y, x = np.mgrid[0:size, 0:size].astype(np.float32)
    out = np.zeros((size, size), np.float32)
    for i in range(ss):
        for j in range(ss):
            r = np.hypot(x + (j + .5) / ss - size / 2, y + (i + .5) / ss - size / 2)
            out += (r >= r0) & (r < r1)
    return out / (ss * ss)


def fly_ring(size=FLY_RING):
    """The medallion's ring (generated, 40 px; the race loading screen draws it larger): gold with a
    radial bevel and one bright arc at the top (the box turns it once a border loop), a dark outline, a
    near-opaque dark plate inside for the emblem."""
    import math
    import numpy as np
    from PIL import Image
    s, k = size, size / 36
    y, x = np.mgrid[0:s, 0:s] + .5
    r, angle = np.hypot(x - s / 2, y - s / 2), np.arctan2(y - s / 2, x - s / 2)
    outer, ring, plate = fly_coverage(s, 0, 17.6 * k), fly_coverage(s, 13.2 * k, 17.0 * k), fly_coverage(s, 0, 13.2 * k)
    rim = np.clip(outer - ring - plate, 0, 1)
    arc = np.exp(-(np.angle(np.exp(1j * (angle + math.pi / 2))) / .55) ** 2)
    g = np.clip(.62 + .25 * (1 - np.abs((r - 15.1 * k) / (1.9 * k))) + .45 * arc, 0, 1.25)
    img = np.zeros((s, s, 4), np.float32)
    img[..., :3] = np.stack([g * 235, g * 205, g * 40], -1) * ring[..., None] + np.stack([plate * 16, plate * 18, plate * 26], -1)
    img[..., 3] = np.clip((ring + rim * .95) * 255 + plate * 235, 0, 255)
    return Image.fromarray(np.clip(img + .5, 0, 255).astype(np.uint8), 'RGBA')


def fly_flag():
    """A waved checkered flag on a pole (generated), for a course in no cup."""
    import math
    import numpy as np
    from PIL import Image
    s = FLY_EMBLEM
    y, x = np.mgrid[0:s, 0:s] + .5
    wave = 1.6 * np.sin(x / s * math.pi * 2)
    cell = (np.floor((x - 1) / 4.3) + np.floor((y - 3 + wave) / 4.3)) % 2
    flag = (x > 3) & (x < 23) & (y - wave > 6) & (y - wave < 20)
    pole = (x > 2) & (x < 4) & (y > 4) & (y < 24)
    v = np.where(cell > 0, 240, 25) * (.85 + .15 * np.cos(x / s * math.pi * 2))
    img = np.zeros((s, s, 4), np.float32)
    img[flag] = np.stack([v, v, v, np.full_like(v, 255)], -1)[flag]
    img[pole] = (200, 200, 210, 255)
    return Image.fromarray(np.clip(img + .5, 0, 255).astype(np.uint8), 'RGBA')


def fly_emblems(root, out):
    """The cup emblems, one RGBA8 FLY_EMBLEM_CELL file each (the icon centred, transparent round it):
    the ten cups' menu icons at FLY_ICON, in FlyCup's order. Returns their paths."""
    from PIL import Image
    ui = root / 'assets/menu-ui/MenuSingle/button/timg'
    paths = []
    for i, name in enumerate(FLY_CUP_ICONS):
        icon = decode_tpl(ui / name).convert('RGBA')
        icon = icon.crop(icon.getbbox())
        f = FLY_ICON / max(icon.size)
        icon = icon.resize((max(1, round(icon.width * f)), max(1, round(icon.height * f))), Image.Resampling.LANCZOS)
        cell = Image.new('RGBA', (FLY_EMBLEM_CELL, FLY_EMBLEM_CELL))
        cell.paste(icon, ((FLY_EMBLEM_CELL - icon.width) // 2, (FLY_EMBLEM_CELL - icon.height) // 2))
        paths.append(texture_file(cell, f'flyinfo_cup_{i:03d}', out, 'rgba8'))
    return paths


def fly_art(root):
    """The frame art (RGBA8 256x128): the inside, the two long edges as periodic strips at half
    horizontal resolution (with a wrap texel each end), the short edges with their corners per
    phase, the medallion's ring, the checkered flag and the (empty) emblem cell. Returns the image,
    its rectangles, the loop period, the long edges' start in box pixels and the bottom gold's top."""
    import numpy as np
    from PIL import Image
    half = FLY_LOOP // 2
    frames = fly_button_frames([0, half], root)
    sides = fly_button_frames([k * FLY_LOOP / FLY_PHASES for k in range(FLY_PHASES)], root)
    src_w, src_h = frames[0].size
    # The pattern's period: the 500-unit fuchi_pattern pane under the menus' 608/832 aspect.
    period_src = 500 * 608 / 832
    scale = (FLY_BOX_W - 2 * FLY_EDGE) / (src_w - 2 * FLY_CORNER)
    period_box = period_src * scale
    strip_w = round(period_box / 2)
    f0, fh = (np.asarray(f, np.float32) for f in frames)

    def band(rows, height=FLY_EDGE):
        # One period at quarter source pixels: frame 0's middle columns, the rest from the
        # half-period frame (content at column c of frame t is frame 0's at c + t * period / 40).
        n = round(period_src * 4)
        out = np.zeros((len(rows), n, 4), np.float32)
        for k in range(n):
            u = k * period_src / n
            if FLY_CORNER <= u <= src_w - FLY_CORNER - 1:
                img, c = f0, u
            else:
                img, c = fh, (u - period_src / 2) % period_src
                if not FLY_CORNER <= c <= src_w - FLY_CORNER - 1:
                    raise ValueError('flyover info: the border loop period is not covered')
            i = int(c)
            t = c - i
            out[:, k] = img[rows, i] * (1 - t) + img[rows, min(i + 1, src_w - 1)] * t
        image = Image.fromarray(np.clip(out + .5, 0, 255).astype(np.uint8), 'RGBA')
        strip = np.asarray(image.resize((strip_w, height), Image.Resampling.LANCZOS))
        return Image.fromarray(np.concatenate([strip[:, -1:], strip, strip[:, :1]], axis=1), 'RGBA')

    top = band(list(range(FLY_CORNER)))
    bottom = band(list(range(src_h - FLY_CORNER, src_h)))
    # The bottom gold is the stretched middle's last three rows (the drop shadow below takes the
    # bottom corner's rows): its own strip, kept at its three source rows, drawn at the middle's scale.
    low = src_h - FLY_CORNER - 3
    if (np.asarray(frames[0])[low:low + 3, FLY_CORNER:src_w - FLY_CORNER, 3] < 250).any():
        raise ValueError('flyover info: the button\'s bottom gold moved')
    line = band(list(range(low, low + 3)), 3)
    line_top = (low - FLY_CORNER) / (src_h - 2 * FLY_CORNER)
    art = Image.new('RGBA', (256, 128))
    rects = {}
    art.paste(top, (0, 0))
    rects['top'] = (0, 0, strip_w + 2, FLY_EDGE)
    art.paste(bottom, (0, FLY_EDGE + 2))
    rects['bottom'] = (0, FLY_EDGE + 2, strip_w + 2, FLY_EDGE)
    art.paste(line, (0, 2 * (FLY_EDGE + 2)))
    rects['line'] = (0, 2 * (FLY_EDGE + 2), strip_w + 2, 3)
    inside = Image.new('RGBA', (4, 4), FLY_ROW_INSIDE)
    art.paste(inside, (strip_w + 4, 0))
    rects['inside'] = (strip_w + 5, 1, 2, 2)
    # Short edges: phase k's left then right column, 4 x (4 + 26 + 4), 15 phases to a row.
    side_h = 2 * FLY_EDGE + FLY_SIDE_MID
    y0 = 2 * (FLY_EDGE + 2) + 4
    for k, image in enumerate(sides):
        sliced = fly_nine_slice(image, 16, side_h)
        x, y = 1 + (k % 15) * 10, y0 + 1 + (k // 15) * (side_h + 2)
        art.paste(sliced.crop((0, 0, FLY_EDGE, side_h)), (x, y))
        art.paste(sliced.crop((16 - FLY_EDGE, 0, 16, side_h)), (x + 5, y))
    rects['side'] = (1, y0 + 1, FLY_EDGE, side_h)
    # Right of the edges: the ring (a transparent texel round it: it is drawn turned), the flag, and
    # the emblem cell on whole 8x8 tiles (the race load copies an emblem file's tiles into it).
    ring, flag = fly_ring(), fly_flag()
    rects['ring'] = (161, 17, FLY_RING, FLY_RING)
    rects['flag'] = (161, 17 + FLY_RING + 2, FLY_EMBLEM, FLY_EMBLEM)
    rects['emblem'] = (224, 16, FLY_EMBLEM_CELL, FLY_EMBLEM_CELL)
    art.paste(ring, rects['ring'][:2])
    art.paste(flag, rects['flag'][:2])
    used = [(0, 0, strip_w + 2, 2 * (FLY_EDGE + 2) + 3), (strip_w + 4, 0, 4, 4), (1, y0 + 1, 15 * 10, 2 * (side_h + 2))]
    used += [(x - 1, y - 1, w + 2, h + 2) for x, y, w, h in (rects['ring'], rects['flag'])] + [rects['emblem']]
    for i, (x, y, w, h) in enumerate(used):
        if x + w > 256 or y + h > 128 or any(x < a + c and a < x + w and y < b + d and b < y + h for a, b, c, d in used[:i]):
            raise ValueError('flyover info: the frame art no longer fits 256x128')
    if rects['emblem'][0] % 8 or rects['emblem'][1] % 8:
        raise ValueError('flyover info: the emblem cell is not on whole tiles')
    # The strips at phase 0 against the rows' own frame (the same source, nine-sliced).
    whole = np.asarray(fly_nine_slice(frames[0], FLY_BOX_W, 4 * FLY_EDGE), np.int16)
    check = np.asarray(top.resize(((strip_w + 2) * 2, FLY_EDGE), Image.Resampling.BILINEAR), np.int16)
    print(f'Flyover info frame: border loop period {period_box:.2f} px, strip {strip_w} px; phase-0 top edge vs the '
          f'nine-sliced frame: mean difference {np.abs(whole[:FLY_EDGE, FLY_EDGE:FLY_EDGE + 200] - check[:, 2:202]).mean():.2f}')
    return art, rects, period_box, FLY_CORNER * scale, line_top


def flyover_info(root, out):
    """The info box's glyph atlas (LA4), frame art (RGBA8) and cup emblems, and their header lines.
    Returns (header lines, packaged texture paths)."""
    import numpy as np
    from PIL import Image
    names = course_names(root)
    if len(names) != 42:
        raise SystemExit(f'flyover info: {len(names)} courses in course_catalog.hpp (expected 42)')
    cups = [course_cup(i) for i in range(len(names))]
    chars = ''.join(sorted(set(''.join(names + FLY_CUP_NAMES))))
    info, glyphs = font_glyphs(root / 'assets/race-ui/Font/kart_kanji_font.brfnt', chars, preserve_ink='1')
    cell = info['cellH']
    baseline = info['baseline']
    images, metrics = {}, {}
    for size, px in enumerate(FLY_PX):
        s = px / cell
        for c, g in glyphs.items():
            a = np.asarray(g['image'].convert('RGBA'), np.float32)
            if not g['width'] or not a[..., 3].any():
                metrics[(size, c)] = (0, 0, 0, 0, round(g['advance'] * s * 16))
                continue
            rgba = np.zeros_like(a)
            rgba[..., :3] = a[..., :1]  # intensity: lerp(black, white), the board's colours
            rgba[..., 3] = a[..., 3]
            # The ink's fractional left edge kept: the glyph is drawn at a whole-pixel pen.
            x0 = g['left'] * s
            dx = int(np.floor(x0))
            w = int(np.ceil(x0 + g['width'] * s)) - dx
            big = Image.new('RGBA', (int(round(w / s)) + 2, cell))
            big.alpha_composite(Image.fromarray(rgba.astype(np.uint8), 'RGBA'), (int(round((x0 - dx) / s)), 0))
            image = big.resize((w, px), Image.Resampling.LANCZOS, box=(0, 0, w / s, cell))
            # Whole-pixel baseline: the cell's baseline lands at row round(baseline * s).
            images[(size, c)] = image
            metrics[(size, c)] = (dx, -round(baseline * s), w, px, round(g['advance'] * s * 16))
    atlas, rects = pack_atlas(images)
    font = texture_file(atlas, 'flyinfo_font', out, 'la4')
    art, art_rects, period, start, line_top = fly_art(root)
    art_path = texture_file(art, 'flyinfo_art', out, 'rgba8')
    emblems = fly_emblems(root, out)

    def ink(size, c, below):
        image = images.get((size, c))
        if image is None:
            return 0
        rows = np.nonzero(np.asarray(image)[..., 3].max(axis=1) > 64)[0]
        top = -round(baseline * FLY_PX[size] / cell)
        return int(top + (rows.max() + 1 if below else rows.min()))
    lines = ['#pragma once', '// Generated by tools/cook_flyover_info.py: the race intro\'s course info box (src/race_bottom_screen.cpp).',
             'namespace mkw {',
             '// Glyph: atlas x, y, w, h; dx, dy: its picture from the pen and the baseline; advance in 1/16 px.',
             'struct FlyGlyph {char16_t c;unsigned char x,y,w,h;signed char dx,dy;unsigned short advance;};',
             '// A size: its glyphs (by character), cell px, cap top (H) and descender bottom (g) from the baseline.',
             'struct FlyFont {unsigned first,count,px;int capTop,descent;};',
             'struct FlyRect {unsigned short x,y,w,h;};']
    glyph_lines, fonts = [], []
    for size in range(len(FLY_PX)):
        first = len(glyph_lines)
        for c in sorted(glyphs):
            dx, dy, w, h, adv = metrics[(size, c)]
            x, y = rects[(size, c)][:2] if (size, c) in rects else (0, 0)
            escaped = '\\' + c if c in "'\\" else c if ord(c) < 128 else '\\u%04x' % ord(c)
            glyph_lines.append(f"{{u'{escaped}',{x},{y},{w},{h},{dx},{dy},{adv}}},")
        fonts.append(f'{{{first},{len(glyph_lines) - first},{FLY_PX[size]},{ink(size, "H", False)},{ink(size, "g", True)}}}')
    lines.append('inline constexpr FlyGlyph FlyGlyphs[]={')
    lines += glyph_lines
    lines.append('};')
    lines.append('inline constexpr FlyFont FlyFonts[2]={' + ','.join(fonts) + '}; // the name line, the cup line')
    r = lambda t: '{%d,%d,%d,%d}' % t
    lines.append(f'inline constexpr const char* FlyFontTexture="{font}";inline constexpr unsigned FlyFontSize[2]={{{atlas.width},{atlas.height}}};')
    lines.append(f'inline constexpr const char* FlyArtTexture="{art_path}";inline constexpr unsigned FlyArtSize[2]={{{art.width},{art.height}}};')
    lines.append('// The frame: inside, the long edges\' periodic strips (one wrap texel each end), phase 0\'s short')
    lines.append('// edges (left; right 5 px on; phase k at +10 px x (k % 15), +(h + 2) px y (k / 15)), the medallion.')
    lines.append(f'inline constexpr FlyRect FlyInside{r(art_rects["inside"])},FlyTop{r(art_rects["top"])},'
                 f'FlyBottom{r(art_rects["bottom"])},FlySide{r(art_rects["side"])},FlyLine{r(art_rects["line"])};')
    lines.append(f'inline constexpr float FlyLineTop={line_top:.6f}f; // the bottom gold\'s top in the middle (0 top .. 1 bottom)')
    lines.append('// The medallion: its ring (a transparent texel round it), the flag (a course in no cup), the emblem cell (whole')
    lines.append('// 8x8 tiles: the race load copies emblem file n, cup n as FlyCup, there).')
    lines.append(f'inline constexpr FlyRect FlyRing{r(art_rects["ring"])},FlyFlag{r(art_rects["flag"])},FlyEmblem{r(art_rects["emblem"])};')
    lines.append(f'inline constexpr unsigned FlyEmblemCount={len(emblems)};inline constexpr const char* FlyEmblemPath="textures/flyinfo_cup_%03u.t3x";')
    lines.append(f'inline constexpr unsigned FlyBoxW={FLY_BOX_W},FlyEdge={FLY_EDGE},FlyPhases={FLY_PHASES};')
    lines.append(f'inline constexpr float FlyPeriod={period:.4f}f; // box px per border loop period (the menus\' 40 frames)')
    lines.append(f'inline constexpr float FlyStripStart={start:.4f}f; // the long edges\' first box px in the period (the corner)')
    lines.append('inline constexpr const char* FlyCupNames[10]={' + ','.join(json.dumps(n) for n in FLY_CUP_NAMES) + '};')
    lines.append('// Per catalog course: its cup (0-7 races, 8-9 Wii/Retro Stages).')
    lines.append('inline constexpr unsigned char FlyCup[]={' + ','.join(map(str, cups)) + '};')
    lines.append('inline constexpr bool FlyAvailable=true;')
    lines += ['}', '']
    widest = max(((sum(metrics[(0 if k == 0 else 1, ch)][4] for ch in text) / 16, text) for k, texts in
                  enumerate([names, FLY_CUP_NAMES]) for text in texts), default=(0, ''))
    print(f'Flyover info: {len(glyphs)} glyphs at {FLY_PX} px ({atlas.width}x{atlas.height}), {len(names)} courses, '
          f'widest line {widest[0]:.0f} px ({widest[1]}); {len(emblems)} cup emblems')
    return lines, [font, art_path] + emblems


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('--root', type=Path, default=ROOT, help='the cooked tree to read (default: this one)')
    p.add_argument('--output', type=Path, help='default <root>/build/flyover-info')
    p.add_argument('--header', type=Path, help='default <root>/include/mkw/flyover_info_assets.hpp')
    a = p.parse_args()
    root = a.root.resolve()
    out = (a.output or root / 'build/flyover-info').resolve()
    header = (a.header or root / 'include/mkw/flyover_info_assets.hpp').resolve()
    (out / 'textures').mkdir(parents=True, exist_ok=True)
    lines, paths = flyover_info(root, out)
    files = {path: hashlib.sha256((out / path).read_bytes()).hexdigest() for path in paths}
    (out / 'flyover-info.json').write_text(json.dumps({'version': 1, 'files': files}, indent=1) + '\n')
    header.parent.mkdir(parents=True, exist_ok=True)
    header.write_text('\n'.join(lines), encoding='utf-8')
    print(f'Flyover info: {len(paths)} textures')


if __name__ == '__main__':
    main()
