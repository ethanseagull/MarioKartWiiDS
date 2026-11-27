#!/usr/bin/env python3
"""Race loading screen (src/loading_screen.cpp, the course reveal): the coming course on the top screen
(the game's logo, the cup medallion and cup, the course name, its Standings map drawn by a small gold pen
along the enemy route) and on the lower screen the cup's courses with the coming one in the course info
box's gold frame, or in Time Trials the course's records. Everything is made from what the port already
cooks from the player's disc: the Standings silhouettes and colours (cook_race_standings.py), the course
info box's frame art and medallion (cook_flyover_info.py), the race font, the menus' cup icons, the lower
screen's character faces, the KMP routes and start lines, and the logo.

The game composes the screens on the CPU straight into the framebuffers while the race loads, so the files
hold plain pixels (premultiplied) and small tables; nothing is built at load time.

Writes sd-card/3ds/mkw-3ds/loading/reveal/ (packed into menu.pak):
- art.bin: the logo; the frame's long edges as periodic strips (4 samples per screen px over one border
  loop period); its short edges per phase.
- medal-NN.bin (cup NN): the gold ring with the cup icon (64 px; the battle cups 128 px).
- faces.bin: the 24 character faces at 24 px.
- font-SS.bin: the race font at scale SS/100 (the footer line, the record times).
- course-NN.bin (catalog course NN): colour, cup, slot, the name, cup name and the cup's course names as
  pictures; races also the map: its pixels in the order the pen reveals them and the pen's path
  (tools/loading_reveal.mjs).
Formats: see write_image, font_file and course_file below (little endian).
"""
import argparse
import concurrent.futures
import json
import math
import os
import re
import struct
import subprocess
import sys
import tempfile
import zlib
from pathlib import Path

import numpy as np
from PIL import Image, ImageDraw

TOOLS = Path(__file__).resolve().parent
sys.path.insert(0, str(TOOLS))
ROOT = TOOLS.parent

# Layout shared with src/loading_screen.cpp (screen px).
LOGO_W = 172                 # the logo's width (its place: loading_screen.cpp)
MEDAL = (64, 128)            # a race cup's medallion, a battle cup's
ICON_SHARE = (.6, .66)       # the cup icon's size in its medallion
NAME_SCALE, NAME_MIN, NAME_WIDTH = .82, .46, 206   # the course name: one line, stepping down to fit
CUP_SCALE = .42
ROW_SCALE, ROW_MIN, ROW_SPACING, ROW_WIDTH = .40, .30, .4, 290
FONTS = ((30, .3), (44, .4))  # runtime fonts: the footer line, the record times (scale x 100, spacing)
FACE = 24
SS = 4                       # supersampling of the course pictures' lettering
SIL_X, SIL_Y = 150, 22       # the Standings silhouette texture's place on the lower screen (cook_race_standings.py)


def le(fmt, *values):
    return struct.pack('<' + fmt, *values)


def premultiplied(image):
    a = np.asarray(image.convert('RGBA'), np.float32)
    a[..., :3] *= a[..., 3:] / 255
    return np.clip(a + .5, 0, 255).astype(np.uint8)


def write_image(pixels, cap=(0, 0)):
    """An image block: u16 w, h, u8 channels (4 premultiplied RGBA, 2 premultiplied grey + alpha), u8 0,
    i16 cap top, cap bottom (lettering: the capitals' rows from the top, 1/16 px), then rows top first."""
    h, w, c = pixels.shape
    return le('HHBBhh', w, h, c, 0, round(cap[0] * 16), round(cap[1] * 16)) + np.ascontiguousarray(pixels).tobytes()


# ---------------------------------------------------------------- lettering

class Font:
    """The race font (kart_kanji_font): white fill, black edge; grey = the fill's share."""

    def __init__(self, root, chars):
        from cook_race_flow import font_glyphs
        self.info, self.glyphs = font_glyphs(root / 'assets/race-ui/Font/kart_kanji_font.brfnt', chars, preserve_ink='1')
        self.cell = self.info['cellH']
        h = np.asarray(self.glyphs['H']['image'].convert('RGBA'))[..., 3]
        rows = np.nonzero(h.max(axis=1) > 64)[0]
        self.cap = (int(rows.min()), int(rows.max()) + 1)   # the capitals' rows in the cell

    def glyph(self, c):
        return self.glyphs.get(c) or self.glyphs['?']

    def width(self, text, s, sp):
        return sum(self.glyph(c)['advance'] * s + sp for c in text)

    def picture(self, text, s, sp):
        """The text at scale s (spacing sp px after each glyph) as premultiplied grey + alpha, with the
        capitals' rows. Drawn at SS x and reduced, so glyphs sit at fractional places."""
        k = s * SS
        h = math.ceil(self.cell * s) + 2
        w = math.ceil(self.width(text, s, sp) + 4)
        big = Image.new('RGBA', (w * SS, h * SS))
        pen = 1.0
        for c in text:
            g = self.glyph(c)
            a = np.asarray(g['image'].convert('RGBA')).copy()
            a[..., 1] = a[..., 2] = a[..., 0]
            if g['width'] and a[..., 3].any():
                im = Image.fromarray(a, 'RGBA').convert('RGBa')
                im = im.resize((max(1, round(im.width * k)), max(1, round(im.height * k))), Image.Resampling.LANCZOS).convert('RGBA')
                big.alpha_composite(im, (max(0, round((pen + g['left'] * s) * SS)), SS))
            pen += g['advance'] * s + sp
        small = np.asarray(big.convert('RGBa').reduce(SS))
        grey = np.stack([small[..., 0], small[..., 3]], -1)
        return grey, (1 + self.cap[0] * s, 1 + self.cap[1] * s)

    def runtime(self, s, chars):
        """font-SS.bin: u16 cell px, i16 cap top, cap bottom (1/16 px), u16 glyphs; per glyph u16 code,
        i8 dx (its picture from the pen), u8 w, u16 advance (1/16 px), u32 offset; then the pictures
        (w x cell px, premultiplied grey + alpha)."""
        px = round(self.cell * s)
        table, data = b'', b''
        for c in sorted(set(chars)):
            g = self.glyph(c)
            adv = round(g['advance'] * s * 16)
            a = np.asarray(g['image'].convert('RGBA')).copy()
            if not g['width'] or not a[..., 3].any():
                table += le('HbBHI', ord(c), 0, 0, adv, len(data)); continue
            a[..., 1] = a[..., 2] = a[..., 0]
            x0 = g['left'] * s
            dx = math.floor(x0)
            w = math.ceil(x0 + g['width'] * s) - dx
            big = Image.new('RGBA', (round(w / s) + 2, self.cell))
            big.alpha_composite(Image.fromarray(a, 'RGBA'), (round((x0 - dx) / s), 0))
            im = big.convert('RGBa').resize((w, px), Image.Resampling.LANCZOS, box=(0, 0, w / s, self.cell))
            q = np.asarray(im)
            table += le('HbBHI', ord(c), dx, w, adv, len(data))
            data += np.ascontiguousarray(np.stack([q[..., 0], q[..., 3]], -1)).tobytes()
        k = px / self.cell
        return le('HhhH', px, round(self.cap[0] * k * 16), round(self.cap[1] * k * 16), len(set(chars))) + table + data


# ---------------------------------------------------------------- art

def flyover_rects(root):
    """The course info box's art rectangles and loop constants (include/mkw/flyover_info_assets.hpp)."""
    text = (root / 'include/mkw/flyover_info_assets.hpp').read_text(encoding='utf-8')
    rects = {name: tuple(int(v) for v in re.search(r'\b' + name + r'\{(\d+),(\d+),(\d+),(\d+)\}', text).groups())
             for name in ('FlyTop', 'FlyBottom', 'FlySide', 'FlyLine')}
    period = float(re.search(r'FlyPeriod=([\d.]+)f', text)[1])
    start = float(re.search(r'FlyStripStart=([\d.]+)f', text)[1])
    phases = int(re.search(r'FlyPhases=(\d+)', text)[1])
    return rects, period, start, phases


def strip(art, rect, period):
    """One long edge over one loop period, 4 samples per box px (bilinear, as the box draws it)."""
    x0, y0, w, h = rect
    per = (w - 2) / period
    n = math.ceil(period * 4)
    out = np.zeros((h, n, 4), np.float32)
    for k in range(n):
        f = x0 + 1 + (k / 4 + .5) * per - .5
        i = int(math.floor(f)); t = f - i
        i0, i1 = max(x0, min(x0 + w - 1, i)), max(x0, min(x0 + w - 1, i + 1))
        out[:, k] = art[y0:y0 + h, i0] * (1 - t) + art[y0:y0 + h, i1] * t
    return np.clip(out + .5, 0, 255).astype(np.uint8)


def art_file(root):
    """art.bin: u32 "MKRA", f32 period, f32 strip start, u16 phases, u16 0; the logo (RGBA); the top,
    line and bottom strips (RGBA, n = ceil(4 x period) samples wide); the short edges: per phase, left
    then right (RGBA 4 x 34)."""
    logo = Image.open(root / 'tools/release/artwork/logo.png').convert('RGBA')
    logo = logo.crop(logo.getbbox())
    logo = logo.convert('RGBa').resize((LOGO_W, round(LOGO_W * logo.height / logo.width)), Image.Resampling.LANCZOS).convert('RGBA')
    rects, period, start, phases = flyover_rects(root)
    art_png = Image.open(root / 'build/flyover-info/textures/flyinfo_art.png').convert('RGBA')
    art = premultiplied(art_png).astype(np.float32)
    data = b'MKRA' + le('ffHH', period, start, phases, 0) + write_image(premultiplied(logo))
    for name in ('FlyTop', 'FlyLine', 'FlyBottom'):
        data += write_image(strip(art, rects[name], period))
    sx, sy, sw, sh = rects['FlySide']
    for k in range(phases):
        x, y = sx + (k % 15) * 10, sy + (k // 15) * (sh + 2)
        for right in (0, 5):
            data += write_image(art[y:y + sh, x + right:x + right + sw].astype(np.uint8))
    return data, logo.size


def medal_file(root, cup):
    import cook_flyover_info as fly
    from cook_race_items import decode_tpl
    size = MEDAL[cup >= 8]
    ring = fly.fly_ring(size)
    icon = decode_tpl(root / 'assets/menu-ui/MenuSingle/button/timg' / fly.FLY_CUP_ICONS[cup]).convert('RGBA')
    icon = icon.crop(icon.getbbox())
    # Its share of the medallion, no farther out than the ring's dark plate (a square icon's corners).
    a = np.asarray(icon)[..., 3]
    ys, xs = np.nonzero(a > 32)
    reach = np.hypot(xs + .5 - icon.width / 2, ys + .5 - icon.height / 2).max()
    fit = min(size * ICON_SHARE[cup >= 8] / max(icon.size), .92 * size * 13.2 / 36 / reach)
    icon = icon.convert('RGBa').resize((max(1, round(icon.width * fit)), max(1, round(icon.height * fit))), Image.Resampling.LANCZOS).convert('RGBA')
    ring.alpha_composite(icon, ((size - icon.width) // 2, (size - icon.height) // 2))
    return write_image(premultiplied(ring))


def faces_file(root):
    """faces.bin: the lower screen's 24 character faces (retail character id order), RGBA 24 x 24 each."""
    text = (root / 'include/mkw/race_map_assets.hpp').read_text(encoding='utf-8')
    ui = text.split('inline constexpr RaceMapAtlas RaceMapUi{')[1].split('};')[0]
    nums = [int(x) for x in re.findall(r'-?\d+', ui.split('{256,256},', 1)[1])]
    atlas = Image.open(root / 'build/race-map/textures/race_bottom_ui_256.png').convert('RGBA')
    data = b''
    for i in range(24):
        x, y, w, h = nums[4 * i:4 * i + 4]
        face = atlas.crop((x, y, x + w, y + h)).convert('RGBa').resize((FACE, FACE), Image.Resampling.LANCZOS).convert('RGBA')
        data += write_image(premultiplied(face))
    return data


# ---------------------------------------------------------------- one course

def route_points(folder, affine):
    """The closed enemy route (ENPT along the ENPH next links) in silhouette texture px."""
    from cook_race_standings import enpt, enph
    pts, groups = enpt(folder / 'course.kmp'), enph(folder / 'course.kmp')
    if not pts or not groups:
        return []
    order, g, seen = [], 0, set()
    while g not in seen and g < len(groups):
        seen.add(g)
        first, count, nxt = groups[g]
        order += range(first, first + count)
        links = [n for n in nxt if n != 0xFF]
        if not links:
            break
        g = links[0]
    a, b, c, d, e, f = affine
    return [(a * pts[i][0] + b * pts[i][2] + e - SIL_X, c * pts[i][0] + d * pts[i][2] + f - SIL_Y) for i in order]


def checker(affine, start):
    """The start line as the Standings draws it (KMP checkpoint 0, the road's middle 13 px, two rows of
    2.6 px cells in a black frame), as an RGBA layer over the silhouette texture."""
    a, b, c, d, e, f = affine
    (lx, lz), (rx, rz) = start
    ax, ay = a * lx + b * lz + e - SIL_X, c * lx + d * lz + f - SIL_Y
    bx, by = a * rx + b * rz + e - SIL_X, c * rx + d * rz + f - SIL_Y
    l = math.hypot(bx - ax, by - ay)
    k = 8
    layer = Image.new('RGBA', (256 * k, 256 * k))
    if l < 1:
        return np.zeros((256, 256, 4), np.uint8)
    ux, uy = (bx - ax) / l, (by - ay) / l
    nx, ny = -uy, ux
    mx, my = (ax + bx) / 2, (ay + by) / 2
    half = min(l / 2, 6.5)
    draw = ImageDraw.Draw(layer)

    def cell(s0, s1, o0, o1, colour):
        pts = [(mx + ux * s + nx * o, my + uy * s + ny * o) for s, o in ((s0, o0), (s1, o0), (s1, o1), (s0, o1))]
        draw.polygon([(x * k, y * k) for x, y in pts], fill=colour)
    cell(-half - 1, half + 1, -3.6, 3.6, (0, 0, 0, 255))
    n = max(3, int(2 * half / 2.6))
    for i in range(min(n, 10)):
        for row in range(2):
            cell(-half + 2 * half * i / n, -half + 2 * half * (i + 1) / n, (row - 1) * 2.6, row * 2.6,
                 (24, 24, 24, 255) if (i + row) % 2 else (255, 255, 255, 255))
    return np.asarray(layer.convert('RGBa').reduce(k).convert('RGBA'))


def reveal(job):
    """The map block of one race course: its pixels by reveal order and the pen's path."""
    root, index, key, affine, start, work = job
    folder = root / 'assets/courses' / key
    sil = np.asarray(Image.open(root / f'build/race-standings/textures/race_map_{index:02d}_standings.png').convert('RGBA')).astype(np.float32)
    # The Standings' grey: 250 at the screen's top to 216 at its bottom (race_bottom_screen.cpp), blue + 4.
    rows = 22 + np.arange(256, dtype=np.float32)
    g = np.round(250 - 34 * rows / 240)
    shade = np.stack([g, g, np.minimum(255, g + 4)], -1)[:, None, :]
    rgba = sil.copy()
    rgba[..., :3] = np.round(sil[..., :3] * shade / 255)
    rgba = np.clip(rgba, 0, 255).astype(np.uint8)
    route = route_points(folder, affine)
    if len(route) < 3:
        return index, None
    work.mkdir(parents=True, exist_ok=True)
    src, dst = work / f'{index:02d}.in', work / f'{index:02d}.out'
    src.write_bytes(le('I', len(route)) + b''.join(le('ff', x, y) for x, y in route) + rgba.tobytes())
    subprocess.run(['node', str(TOOLS / 'loading_reveal.mjs'), str(src), str(dst)], check=True)
    out = dst.read_bytes()
    n, length, top = struct.unpack_from('<Iff', out, 0)
    path = np.frombuffer(out, np.float32, n * 3, 12).reshape(n, 3)
    at = 12 + 12 * n
    arc = np.frombuffer(out, np.float32, 65536, at).reshape(256, 256)
    rest = np.frombuffer(out, np.uint8, 65536, at + 65536 * 4).reshape(256, 256)
    # The start line over the map, shown from the pen's start.
    flag = checker(affine, start).astype(np.float32)
    fa = flag[..., 3:] / 255
    pic = rgba.astype(np.float32)
    pic[..., :3] = pic[..., :3] * (1 - fa) + flag[..., :3]       # the layer is premultiplied
    pic[..., 3] = pic[..., 3] * (1 - fa[..., 0]) + flag[..., 3]
    pic = np.clip(pic + .5, 0, 255).astype(np.uint8)
    arc = np.where(flag[..., 3] > 0, 0, arc)
    alpha = pic[..., 3]
    ys, xs = np.nonzero(alpha > 0)
    px0, py0 = min(xs.min(), math.floor(path[:, 0].min()) - 6), min(ys.min(), math.floor(path[:, 1].min()) - 6)
    px1, py1 = max(xs.max() + 1, math.ceil(path[:, 0].max()) + 6), max(ys.max() + 1, math.ceil(path[:, 1].max()) + 6)
    px0, py0, px1, py1 = max(0, px0), max(0, py0), min(256, px1), min(256, py1)
    shown = arc[ys, xs] >= 0
    fixed = np.where(shown, np.minimum(65534, np.round(arc[ys, xs] * 16)), 65535).astype(np.uint16)
    order = np.lexsort((xs, ys, fixed))
    xs, ys, fixed = xs[order], ys[order], fixed[order]
    first_rest = int(np.searchsorted(fixed, 65535))
    if first_rest < len(fixed):
        rx, ry = xs[first_rest:], ys[first_rest:]
        rest_box = (int(rx.min()) - px0, int(ry.min()) - py0, int(rx.max() + 1 - rx.min()), int(ry.max() + 1 - ry.min()))
    else:
        rest_box = (0, 0, 0, 0)
    p = pic[ys, xs]
    pixels = np.zeros(len(xs), dtype=[('x', 'u1'), ('y', 'u1'), ('g', 'u1'), ('b', 'u1'), ('a', 'u1'), ('pad', 'u1'), ('arc', '<u2')])
    pixels['x'], pixels['y'] = xs - px0, ys - py0
    pixels['g'], pixels['b'], pixels['a'] = p[:, 1], p[:, 2], p[:, 3]   # premultiplied; red = green
    pixels['arc'] = fixed
    pts = path.copy(); pts[:, 0] -= px0; pts[:, 1] -= py0
    block = le('HHHH', px0, py0, px1 - px0, py1 - py0) + le('II', len(xs), first_rest) + le('HHHH', *rest_box)
    block += le('Iff', n, float(length), float(top)) + pixels.tobytes() + np.ascontiguousarray(pts, np.float32).tobytes()
    stats = {'pixels': int(len(xs)), 'rest': int(len(xs) - first_rest), 'box': [int(px0), int(py0), int(px1 - px0), int(py1 - py0)],
             'path': int(n), 'length': round(float(length), 1), 'arcMax': round(float(top), 1)}
    return index, (block, stats)


def course_file(kind, cup, slot, colour, pictures, members, block):
    """course-NN.bin: u32 "MKRC", u8 kind (0 race, 1 arena), cup, slot, members, u8 r, g, b, 0; the
    name, the cup's name, each member's name (images); a race then has, from the next multiple of 4 bytes, its map: u16 box x, y, w, h (in
    the silhouette texture), u32 pixels, u32 first rest pixel, u16 rest box x, y, w, h, u32 path
    samples, f32 path length, f32 last arc; the pixels by reveal order (u8 x, y in the box, u8 premultiplied
    green (= red), blue, alpha, u8 0, u16 arc in 1/16 px of path, 0xFFFF: the rest, which fades in once
    the lap is closed); the path (f32 x, y, length so far, in the box)."""
    data = b'MKRC' + bytes([kind, cup, slot, len(members)]) + bytes(colour) + b'\0'
    for picture, cap in pictures + members:
        data += write_image(picture, cap)
    if block:
        data += bytes(-len(data) % 4)   # the map's 16- and 32-bit fields on whole words
    return data + (block or b'')


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('--root', type=Path, default=ROOT, help='the cooked tree to read (default: this one)')
    p.add_argument('--output', type=Path, help='default <root>/sd-card/3ds/mkw-3ds/loading/reveal')
    p.add_argument('--report', type=Path, help='default <root>/build/loading-reveal.json (per-course numbers)')
    p.add_argument('--jobs', type=int, default=6)
    p.add_argument('--courses', default='', help='comma-separated catalog indices (checks)')
    a = p.parse_args()
    root = a.root.resolve()
    out = (a.output or root / 'sd-card/3ds/mkw-3ds/loading/reveal').resolve()
    needed = [root / 'build/race-standings/race-standings.json', root / 'build/flyover-info/textures/flyinfo_art.png',
              root / 'include/mkw/flyover_info_assets.hpp', root / 'build/race-map/textures/race_bottom_ui_256.png']
    if not all(n.is_file() for n in needed):
        print('race loading reveal: the Standings, course info box or lower screen cooks are missing, not cooked')
        return
    import cook_flyover_info as fly
    import course_catalog
    from cook_race_standings import ckpt, PALETTE
    course_catalog.ROOT = root
    catalog = course_catalog.courses()
    names = fly.course_names(root)
    standings = {r['index']: r for r in json.loads((root / 'build/race-standings/race-standings.json').read_text())['courses']}
    footer = ['Grand Prix', 'VS', 'Time Trial', 'Balloon Battle', 'Coin Runners', 'Race', 'Battle', 'Best', 'Mirror', 'cc', '/']
    font = Font(root, ''.join(sorted(set(''.join(names + fly.FLY_CUP_NAMES + footer) + '0123456789:.? '))))
    out.mkdir(parents=True, exist_ok=True)
    files = {}
    art, logo_size = art_file(root)
    files['art.bin'] = art
    for cup in range(len(fly.FLY_CUP_NAMES)):
        files[f'medal-{cup:02d}.bin'] = medal_file(root, cup)
    files['faces.bin'] = faces_file(root)
    runtime_chars = ''.join(sorted(set(''.join(footer) + '0123456789:.- ')))
    for scale, _ in FONTS:
        files[f'font-{scale}.bin'] = font.runtime(scale / 100, runtime_chars)

    wanted = [int(i) for i in a.courses.split(',')] if a.courses else list(range(len(catalog)))
    work = Path(tempfile.mkdtemp(prefix='loading-reveal-'))
    jobs = [(root, i, catalog[i]['key'], standings[i]['affine'], ckpt(root / 'assets/courses' / catalog[i]['key'] / 'course.kmp')[0], work)
            for i in wanted if catalog[i]['kind'] == 'race' and i in standings and 'affine' in standings[i]]
    maps, report = {}, {}
    with concurrent.futures.ProcessPoolExecutor(max_workers=a.jobs) as pool:
        for index, result in pool.map(reveal, jobs):
            if result:
                maps[index] = result[0]; report[index] = result[1]
    for f in work.iterdir():
        f.unlink()
    work.rmdir()
    for i in wanted:
        cup = fly.course_cup(i)
        members = [j for j in range(len(catalog)) if fly.course_cup(j) == cup]
        if catalog[i]['kind'] == 'race' and i in standings and 'colour' in standings[i]:
            colour = standings[i]['colour']
        else:
            colour = PALETTE[zlib.crc32(catalog[i]['key'].encode()) % len(PALETTE)]
        s = NAME_SCALE
        while s > NAME_MIN and font.width(names[i], s, 0) > NAME_WIDTH:
            s -= .02
        rows = []
        for j in members:
            r = ROW_SCALE
            while r > ROW_MIN and font.width(names[j], r, ROW_SPACING) > ROW_WIDTH:
                r -= .01
            rows.append(font.picture(names[j], r, ROW_SPACING))
        kind = 0 if i in maps else 1
        files[f'course-{i:02d}.bin'] = course_file(kind, cup, members.index(i), colour,
                                                   [font.picture(names[i], s, 0), font.picture(fly.FLY_CUP_NAMES[cup], CUP_SCALE, 0)],
                                                   rows, maps.get(i))
        report.setdefault(i, {})['name'] = f'{names[i]} at {s:.2f}'
    total = 0
    for name, data in files.items():
        target = out / name
        if not target.is_file() or target.read_bytes() != data:
            target.write_bytes(data)
        total += len(data)
    (a.report or root / 'build/loading-reveal.json').write_text(json.dumps({'logo': logo_size, 'courses': {str(k): report[k] for k in sorted(report)}}, indent=1) + '\n')
    print(f'race loading reveal: {len(files)} files, {total // 1024} KB, {len(maps)} maps')


if __name__ == '__main__':
    main()
