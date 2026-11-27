#!/usr/bin/env python3
"""The race lower screen's Standings view (src/race_bottom_screen.cpp): per race course its
silhouette, background colour and emblem tile, plus the view's small atlas (the 12 rank numbers in
their row boxes, the player's heading pointer) and its name font. Everything is made from what the
port already cooks from the player's disc (map_model meshes, KMP routes, KCL roads, the course
maps, cup icons, HUD rank numerals, name glyphs).

- Silhouette: the course's minimap mesh (map_model.brres), exported without its bone matrices (the
  Wii draws the map shape untransformed; our exporter applies the bind pose, which moves about a
  hundred maps by their posRU / posLD+posRU bones). Checked against the KMP enemy route: a mesh with
  under 90% of the route on it tries the exporter's posed vertices, then a scale+offset registration
  onto the KCL road (FFT cross-correlation over a scale sweep), and if neither fits, a hybrid (the
  KCL road inside a corridor around the route, plus a route ribbon). Turned to the angle (5 degree
  steps) that draws it largest in the map area (BOX), near-ties (2%) to the start pointing up, and
  centred on its bounding box. Drawn as the Wii's minimap road: white with a black 2 px edge and a
  soft drop shadow, as one LA4 picture of the area (the road's top-to-bottom grey comes from vertex
  colours at run time). The header holds each course's world -> screen affine (the runtime places
  the faces, start line and pointer with it).
- Colour: a dark per-track colour (the user's order): the course art's standout colour (the most
  saturated hue of its top-down map, grass, road and dirt excluded, at least 15% of the map), else
  the dominant colour of the whole-course map's outer ring (not green or grey), else a palette entry
  from the course key; brought into one dark band (V .34, S .45-.78) so the white road, gold rows and
  numbers keep their contrast. COLOUR_OVERRIDES: hand-set misses.
- Emblem: the course's cup icon as a flat stencil tile (tools/standings_art.py).

Writes include/mkw/race_standings_assets.hpp and build/race-standings/ (textures, race-standings.json:
the packaged files and per-course records, sheets/ for checking by eye).
"""
import argparse
import colorsys
import concurrent.futures
import hashlib
import json
import math
import os
import re
import struct
import subprocess
import sys
import zlib
from pathlib import Path

import numpy as np
from PIL import Image, ImageDraw, ImageFilter, ImageChops

TOOLS = Path(__file__).resolve().parent
sys.path.insert(0, str(TOOLS))
ROOT = TOOLS.parent
import standings_art as art

# The Standings map area (screen px; the column ends at 146, the time sits above y 26).
BOX = (154, 26, 316, 236)
# The silhouette texture's top-left on the screen, and its size (it holds BOX with the edge and shadow).
SIL_X, SIL_Y, SIL = 150, 22, 256
ROUTE_FIT = .90
# Hand-set colours for the misses of the automatic pick (by course name, "Wii " ignored), in the dark band.
COLOUR_OVERRIDES = {
    'Moonview Highway': ((36, 30, 86), 'night indigo (its standout pick is dark green)'),
    'Coconut Mall': ((86, 47, 47), 'its pink surround (the standout pick is green)'),
}
# 12 dark colours, picked by the course key's CRC when nothing else gives one.
PALETTE = [(20, 34, 96), (44, 30, 104), (78, 26, 86), (96, 22, 40), (104, 46, 18), (92, 72, 14),
           (62, 74, 20), (20, 74, 38), (14, 78, 80), (16, 56, 86), (46, 56, 74), (40, 40, 46)]


# ---------------------------------------------------------------- course data

def kmp_sections(path):
    b = path.read_bytes(); n, h = struct.unpack_from('>HH', b, 8); out = {}
    for i in range(n):
        o = h + struct.unpack_from('>I', b, 16 + 4 * i)[0]; out[b[o:o + 4].decode()] = (o, struct.unpack_from('>H', b, o + 4)[0])
    return b, out


def enpt(path):
    b, S = kmp_sections(path)
    if 'ENPT' not in S:
        return []
    o, c = S['ENPT']
    return [struct.unpack_from('>3f', b, o + 8 + 20 * i) for i in range(c)]


def enph(path):
    b, S = kmp_sections(path)
    if 'ENPH' not in S:
        return []
    o, c = S['ENPH']
    return [(b[o + 8 + 16 * i], b[o + 9 + 16 * i], list(b[o + 16 + 16 * i:o + 22 + 16 * i])) for i in range(c)]


def ckpt(path):
    b, S = kmp_sections(path); o, c = S['CKPT']
    return [(struct.unpack_from('>2f', b, o + 8 + 20 * k), struct.unpack_from('>2f', b, o + 16 + 20 * k)) for k in range(c)]


def exporter(source, out_dir, raw):
    """build/export_brres.mjs with its root-parent assertion relaxed and, raw, without the bind pose
    on the vertices."""
    text = source.read_text(encoding='utf-8')
    old = '    assert(id > 0);\n    parentNodeId = (entryOffs + toParentNode - baseOffs) / 208;'
    if old not in text:
        raise SystemExit('export_brres.mjs: the root-parent assertion moved')
    text = text.replace(old, '    if (id > 0) parentNodeId = (entryOffs + toParentNode - baseOffs) / 208;')
    if raw:
        pose = '        const transform = pose[matrix];\n'
        if pose not in text:
            raise SystemExit('export_brres.mjs: the position matrix lookup moved')
        text = text.replace(pose, '        const transform = pose[matrix] && new Float32Array([1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1]);\n')
    path = out_dir / ('export_map_raw.mjs' if raw else 'export_map_posed.mjs')
    path.write_text(text, encoding='utf-8')
    return path


def export_mesh(js, brres, work):
    work.mkdir(parents=True, exist_ok=True)
    r = subprocess.run(['node', str(js), str(brres), str(work)], capture_output=True, text=True)
    if r.returncode:
        return None
    m = json.loads((work / 'model.json').read_text())
    for f in work.iterdir():
        f.unlink()
    tris = [[(v[0], v[2]) for v in s['vertices'][i:i + 3]] for mo in m['models'] for s in mo['shapes'] for i in range(0, len(s['vertices']) - 2, 3)]
    return tris or None


def bbox_fill(tris):
    """How much of its own bounding box a mesh covers: a road shape stays well under 70%; a map that
    is one textured quad (a minimap drawn as a picture) covers it all."""
    xs = [p[0] for t in tris for p in t]; zs = [p[1] for t in tris for p in t]
    x0, x1, z0, z1 = min(xs), max(xs), min(zs), max(zs); S = 256; s = S / max(x1 - x0, z1 - z0, 1)
    im = Image.new('L', (S, S)); d = ImageDraw.Draw(im)
    for t in tris:
        d.polygon([((x - x0) * s, (z - z0) * s) for x, z in t], fill=255)
    w, h = max(1, int((x1 - x0) * s)), max(1, int((z1 - z0) * s))
    return float((np.asarray(im)[:h, :w] > 127).mean())


def near_route(tris, pts):
    """The mesh without stray pieces far from the course (Moonview Highway's map has a few tiny
    triangles 60,000 units past its road): triangles with no corner within 15% of the route's span
    (at least 5,000 units) of the route's bounding box."""
    if not tris or not pts:
        return tris
    xs = [p[0] for p in pts]; zs = [p[2] for p in pts]
    m = max(5000.0, .15 * max(max(xs) - min(xs), max(zs) - min(zs)))
    x0, x1, z0, z1 = min(xs) - m, max(xs) + m, min(zs) - m, max(zs) + m
    return [t for t in tris if any(x0 <= x <= x1 and z0 <= z <= z1 for x, z in t)]


def usable(tris):
    return tris and len(tris) >= 8 and bbox_fill(tris) <= .70


def route_fit(tris, pts, sc=1.0, dx=0.0, dz=0.0):
    """Share of the enemy route's points on the mesh (400 px raster, 2 px tolerance)."""
    T = [[(x * sc + dx, z * sc + dz) for x, z in t] for t in tris]
    xs = [p[0] for t in T for p in t] + [p[0] for p in pts]; zs = [p[1] for t in T for p in t] + [p[2] for p in pts]
    x0, x1, z0, z1 = min(xs), max(xs), min(zs), max(zs); S = 400; s = S / max(x1 - x0, z1 - z0, 1)
    im = Image.new('L', (S + 2, S + 2)); d = ImageDraw.Draw(im)
    for t in T:
        d.polygon([((x - x0) * s, (z - z0) * s) for x, z in t], fill=255)
    im = im.filter(ImageFilter.MaxFilter(5))
    return sum(1 for p in pts if im.getpixel((min(S, int((p[0] - x0) * s)), min(S, int((p[2] - z0) * s)))) > 0) / len(pts)


def kcl_road(folder):
    import race_map_bake as rb
    a, b, c, kind, face = rb.kcl_triangles(folder / 'course.kcl')
    road = np.isin(kind, list(rb.ROAD_TYPES)) & (face[:, 1] > 0.2)
    return a[road], b[road], c[road]


def register(mesh, road):
    """A uniform scale and offset that lays the mesh on the KCL road: (score, scale, dx, dz)."""
    N = 512
    def raster(tris, x0, z0, cell):
        im = Image.new('L', (N, N)); d = ImageDraw.Draw(im)
        for t in tris:
            d.polygon([((x - x0) / cell, (z - z0) / cell) for x, z in t], fill=255)
        return np.asarray(im, dtype=np.float32) / 255
    rx = [p[0] for t in road for p in t]; rz = [p[1] for t in road for p in t]
    rx0, rx1, rz0, rz1 = min(rx), max(rx), min(rz), max(rz)
    span = max(rx1 - rx0, rz1 - rz0) * 2.2; cell = span / N
    ox, oz = (rx0 + rx1) / 2 - span / 2, (rz0 + rz1) / 2 - span / 2
    R = raster(road, ox, oz, cell); FR = np.fft.rfft2(R)
    mx = [p[0] for t in mesh for p in t]; mz = [p[1] for t in mesh for p in t]
    mcx, mcz = (min(mx) + max(mx)) / 2, (min(mz) + max(mz)) / 2
    best = (-1, 1, 0, 0)
    base = (rx1 - rx0 + rz1 - rz0) / max(max(mx) - min(mx) + max(mz) - min(mz), 1)
    for s in sorted(set([1.0] + [base * f for f in np.linspace(.8, 1.25, 19)])):
        T = [[((x - mcx) * s + (rx0 + rx1) / 2, (z - mcz) * s + (rz0 + rz1) / 2) for x, z in t] for t in mesh]
        M = raster(T, ox, oz, cell)
        if M.sum() < 1:
            continue
        c = np.fft.irfft2(FR * np.conj(np.fft.rfft2(M)), s=R.shape)
        iy, ix = np.unravel_index(np.argmax(c), c.shape)
        sy = iy if iy < N // 2 else iy - N; sx = ix if ix < N // 2 else ix - N
        score = c[iy, ix] / math.sqrt(M.sum() * R.sum())
        if score > best[0]:
            best = (score, s, (rx0 + rx1) / 2 - mcx * s + sx * cell, (rz0 + rz1) / 2 - mcz * s + sy * cell)
    return best


def hybrid(folder, pts):
    """The fallback shape in world space: the KCL road inside a corridor around the enemy route,
    plus a route ribbon. Returns (mask, x0, z0, cell): a raster with `cell` world units a pixel."""
    ph = enph(folder / 'course.kmp')
    A, B, C = kcl_road(folder)
    xs = [p[0] for p in pts]; zs = [p[2] for p in pts]
    pad = 2500; x0, x1, z0, z1 = min(xs) - pad, max(xs) + pad, min(zs) - pad, max(zs) + pad
    N = 1024; cell = max(x1 - x0, z1 - z0) / N
    # the ribbon is 4 display px and the corridor 8 beyond it at the study's ~156 px frame
    disp = max(x1 - x0, z1 - z0) / 180
    P = lambda x, z: ((x - x0) / cell, (z - z0) / cell)
    km = Image.new('L', (N, N)); dk = ImageDraw.Draw(km)
    for t in zip(A, B, C):
        dk.polygon([P(v[0], v[2]) for v in t], fill=255)
    rib = Image.new('L', (N, N)); dr = ImageDraw.Draw(rib)
    links = []
    for st, ln, nx in ph:
        seg = list(range(st, st + ln)); links += list(zip(seg, seg[1:]))
        for n in nx:
            if n != 255 and n < len(ph):
                links.append((st + ln - 1, ph[n][0]))
    if not ph:
        links = [(i, (i + 1) % len(pts)) for i in range(len(pts))]
    width = 4 * disp / cell
    for i, j in links:
        if i >= len(pts) or j >= len(pts):
            continue
        p, q = P(pts[i][0], pts[i][2]), P(pts[j][0], pts[j][2])
        if math.dist(p, q) * cell > 40 * disp:
            continue  # a jump to a far point (respawn shortcuts and the like)
        dr.line([p, q], fill=255, width=max(1, int(round(width))))
        for e in (p, q):
            r = width / 2; dr.ellipse((e[0] - r, e[1] - r, e[0] + r, e[1] + r), fill=255)
    k = int(2 * 8 * disp / cell) | 1
    corridor = rib.filter(ImageFilter.MaxFilter(min(k, 61)))
    m = ImageChops.lighter(ImageChops.multiply(km, corridor), rib)
    return m, x0, z0, cell


# ---------------------------------------------------------------- orientation

def fit_angle(pts, ang):
    c, s = math.cos(ang), math.sin(ang)
    us = [x * c - z * s for x, z in pts]; vs = [x * s + z * c for x, z in pts]
    w, h = max(max(us) - min(us), 1e-3), max(max(vs) - min(vs), 1e-3)
    bw, bh = BOX[2] - BOX[0], BOX[3] - BOX[1]
    return min(bw / w, bh / h) * .94, (min(us) + max(us)) / 2, (min(vs) + max(vs)) / 2


def best_angle(pts, start):
    """Any angle (5 degree steps) with the largest scale; among near-ties (2%) the start up."""
    (l0, r0), (l1, r1) = start
    dx = (l1[0] + r1[0] - l0[0] - r0[0]) / 2; dz = (l1[1] + r1[1] - l0[1] - r0[1]) / 2; L = math.hypot(dx, dz) or 1
    dx, dz = dx / L, dz / L
    up = lambda a: -(dx * math.sin(a) + dz * math.cos(a))
    cands = [math.radians(a) for a in range(0, 360, 5)]
    scs = {a: fit_angle(pts, a)[0] for a in cands}; best = max(scs.values())
    return max([a for a in cands if scs[a] >= best * .98], key=up)


def affine(pts, ang):
    """World (x, z) -> screen: X = a x + b z + e, Y = c x + d z + f."""
    sc, cu, cv = fit_angle(pts, ang); co, si = math.cos(ang), math.sin(ang)
    bx, by = (BOX[0] + BOX[2]) / 2, (BOX[1] + BOX[3]) / 2
    return (co * sc, -si * sc, si * sc, co * sc, bx - cu * sc, by - cv * sc)


# ---------------------------------------------------------------- the picture

def coverage(shape, A, SS=4):
    """The shape's coverage of each screen pixel (320x240), from triangles or a world raster."""
    a, b, c, d, e, f = A
    if shape['kind'] == 'raster':
        m, x0, z0, cell = shape['mask']
        # screen (X, Y) -> world: invert the affine, then the raster's pixel
        det = a * d - b * c; ia, ib, ic, id_ = d / det, -b / det, -c / det, a / det
        # output pixel (u, v) at SS: X = u / SS; world x = ia (X - e) + ib (Y - f)
        coef = (ia / SS / cell, ib / SS / cell, (-ia * e - ib * f - x0) / cell,
                ic / SS / cell, id_ / SS / cell, (-ic * e - id_ * f - z0) / cell)
        big = m.transform((320 * SS, 240 * SS), Image.AFFINE, coef, resample=Image.BILINEAR)
        return big.resize((320, 240), Image.BOX)
    big = Image.new('L', (320 * SS, 240 * SS)); dr = ImageDraw.Draw(big)
    for t in shape['tris']:
        dr.polygon([((a * x + b * z + e) * SS, (c * x + d * z + f) * SS) for x, z in t], fill=255)
    return big.resize((320, 240), Image.BOX)


def silhouette_la(mk):
    """The Wii minimap's road from a coverage mask: black soft shadow (+2, +3), black 2 px edge,
    the fill at 92% over it. -> (L, A) float arrays over the screen; L is the fill's share (the
    runtime's vertex colours give it the top-to-bottom grey)."""
    sh = np.asarray(ImageChops.offset(mk.filter(ImageFilter.MaxFilter(5)).filter(ImageFilter.GaussianBlur(1.5)), 2, 3)) / 255 * 150 / 255
    sh[:3, :] = 0; sh[:, :2] = 0
    edge = np.asarray(mk.filter(ImageFilter.MaxFilter(5))) / 255
    fill = np.asarray(mk) / 255 * 235 / 255
    a1 = fill + edge * (1 - fill)
    A = a1 + sh * (1 - a1)
    L = np.where(A > 0, fill / np.maximum(A, 1e-6), 0)
    return L, A


def preview(L, A, base, tile_levels, grad=True):
    """The Standings background and silhouette as the console draws them (sheets only)."""
    bg = np.zeros((240, 320, 3))
    t = art.tile_colours(tile_levels, base).astype(float)
    bg[:] = np.tile(t, (4, 5, 1))[:240, :320]
    bg *= (1 - (np.arange(240) / 239 * 60 / 255))[:, None, None]
    g = (250 - 34 * np.arange(240) / 240)[:, None] if grad else 255
    col = np.stack([L * g, L * g, L * np.minimum(255, g + 4)], 2)
    out = bg * (1 - A[:, :, None]) + col * A[:, :, None]
    return Image.fromarray(out.clip(0, 255).astype(np.uint8), 'RGB')


# ---------------------------------------------------------------- colours

def band(rgb):
    h, s, v = colorsys.rgb_to_hsv(*[c / 255 for c in rgb])
    r, g, b = colorsys.hsv_to_rgb(h, min(.78, max(.45, s * 1.1)) if s >= .12 else s, .34)
    return tuple(int(c * 255) for c in (r, g, b))


def standout_colour(png):
    im = Image.open(png).convert('RGB').resize((160, 160)); a = np.asarray(im).astype(float) / 255
    hsv = np.array([colorsys.rgb_to_hsv(*px) for px in a.reshape(-1, 3)]).reshape(160, 160, 3)
    h, s, v = hsv[:, :, 0] * 360, hsv[:, :, 1], hsv[:, :, 2]
    grey = s < .18
    grass = (h > 65) & (h < 165) & (s < .80)
    dirt = (h > 12) & (h < 50) & (v < .70) & (s < .70)
    ok = ~(grey | grass | dirt) & (v > .15)
    if ok.sum() < 30:
        return None, 0
    wgt = s * v * ok
    hist = np.zeros(36)
    np.add.at(hist, (h[ok] / 10).astype(int) % 36, wgt[ok])
    hist = np.convolve(np.concatenate([hist[-1:], hist, hist[:1]]), [.25, .5, .25], 'valid')
    peak = int(np.argmax(hist)); sel = ok & ((h / 10).astype(int) % 36 == peak)
    rgb = (a[sel] * 255).mean(0) if sel.any() else (a[ok] * 255).mean(0)
    return tuple(int(c) for c in rgb), float(ok.mean())


def edge_colour(png):
    """The dominant colour of the overview map's outer 12% ring, and its class."""
    from collections import Counter
    im = Image.open(png).convert('RGBA'); w, h = im.size; px = np.asarray(im)
    b = max(2, int(min(w, h) * .12))
    ring = np.ones((h, w), bool); ring[b:h - b, b:w - b] = False
    p = px[ring]; tot = len(p); opaque = p[p[:, 3] >= 128]
    if not len(opaque) or tot - len(opaque) > tot * .6:
        return None, 'transparent'
    cnt = Counter(map(tuple, (opaque[:, :3] >> 4).tolist()))
    (r, g, bl), _ = cnt.most_common(1)[0]
    share = sum(v for k, v in cnt.items() if abs(k[0] - r) <= 1 and abs(k[1] - g) <= 1 and abs(k[2] - bl) <= 1) / len(opaque)
    H, S, V = colorsys.rgb_to_hsv(r / 15, g / 15, bl / 15)
    if V < .12:
        c = 'black/void'
    elif S < .18:
        c = 'grey/white'
    else:
        hd = H * 360
        c = 'green' if 70 <= hd < 170 else 'blue/cyan' if 170 <= hd < 260 else 'brown/orange/yellow' if 20 <= hd < 70 else 'red/pink/purple'
    if share < .35:
        c += ' (mixed edge)'
    return (r * 17, g * 17, bl * 17), c


def track_colour(root, index, course):
    name = course['name'][4:] if course['name'].startswith('Wii ') else course['name']
    if name in COLOUR_OVERRIDES:
        return COLOUR_OVERRIDES[name][0], 'override: ' + COLOUR_OVERRIDES[name][1]
    maps = root / 'build/race-map/textures'
    so, share = standout_colour(maps / f'race_map_{index:02d}.png')
    if so and share >= .15:
        return band(so), f'standout {so} ({share:.0%} of the map)'
    ed, cls = edge_colour(maps / f'race_map_{index:02d}_overview.png')
    if ed and not cls.startswith(('green', 'grey', 'black', 'transparent')):
        return band(ed), f'edge {cls}'
    return PALETTE[zlib.crc32(course['key'].encode()) % len(PALETTE)], 'palette'


# ---------------------------------------------------------------- one course

def cook_course(job):
    root, out, index, course, posed_js, raw_js, start = job
    folder = root / 'assets/courses' / course['key']
    pts = enpt(folder / 'course.kmp')
    work = out / 'work' / str(index)
    record = {'index': index, 'key': course['key'], 'name': course['name']}
    shape = None
    brres = folder / 'map_model.brres'
    tris_raw = near_route(export_mesh(raw_js, brres, work), pts) if brres.is_file() else None
    fits = {}
    if tris_raw and not usable(tris_raw):
        record['meshRejected'] = f'{len(tris_raw)} triangles covering {bbox_fill(tris_raw):.0%} of their box'; tris_raw = None
    if tris_raw and pts:
        fits['raw'] = route_fit(tris_raw, pts)
        if fits['raw'] >= ROUTE_FIT:
            shape = {'kind': 'mesh', 'tris': tris_raw, 'source': 'raw'}
    if shape is None and brres.is_file() and pts:
        tris_posed = near_route(export_mesh(posed_js, brres, work), pts)
        if tris_posed and not usable(tris_posed):
            tris_posed = None
        if tris_posed:
            fits['posed'] = route_fit(tris_posed, pts)
            if fits['posed'] >= ROUTE_FIT:
                shape = {'kind': 'mesh', 'tris': tris_posed, 'source': 'posed'}
        if shape is None:
            cands = [t for t in (tris_raw, tris_posed) if t]
            if cands:
                A, B, C = kcl_road(folder)
                road = [[(v[0], v[2]) for v in t] for t in zip(A, B, C)]
                for t in cands:
                    score, s, dx, dz = register(t, road)
                    f = route_fit(t, pts, s, dx, dz)
                    fits.setdefault('registered', f); fits['registered'] = max(fits['registered'], f)
                    if f >= ROUTE_FIT and (shape is None or f > shape['fit']):
                        shape = {'kind': 'mesh', 'tris': [[(x * s + dx, z * s + dz) for x, z in tri] for tri in t], 'source': 'registered',
                                 'fit': f, 'transform': [round(s, 4), round(dx), round(dz)]}
    if shape is None:
        if not pts:
            record['error'] = 'no enemy route'; return record, None
        m = hybrid(folder, pts)
        shape = {'kind': 'raster', 'mask': m, 'source': 'hybrid'}
    record['shape'] = shape['source']; record['routeFit'] = {k: round(v, 3) for k, v in fits.items()}
    if 'transform' in shape:
        record['transform'] = shape['transform']
    if shape['kind'] == 'mesh':
        ptsxz = [p for t in shape['tris'] for p in t]; ptsxz = ptsxz[::max(1, len(ptsxz) // 30000)]
    else:
        m, x0, z0, cell = shape['mask']; a = np.asarray(m) > 127; ys, xs = np.nonzero(a)
        step = max(1, len(xs) // 6000)
        ptsxz = [(x0 + (x + .5) * cell, z0 + (y + .5) * cell) for x, y in zip(xs[::step], ys[::step])]
    ang = best_angle(ptsxz, start)
    A = affine(ptsxz, ang)
    record['angle'] = round(math.degrees(ang)) % 360; record['affine'] = A
    mk = coverage(shape, A)
    L, Al = silhouette_la(mk)
    # the texture: BOX and its edge and shadow, from (SIL_X, SIL_Y)
    la = np.zeros((SIL, SIL, 2))
    h, w = min(SIL, 240 - SIL_Y), min(SIL, 320 - SIL_X)
    la[:h, :w, 0] = L[SIL_Y:SIL_Y + h, SIL_X:SIL_X + w]; la[:h, :w, 1] = Al[SIL_Y:SIL_Y + h, SIL_X:SIL_X + w]
    outside = Al.copy(); outside[SIL_Y:SIL_Y + h, SIL_X:SIL_X + w] = 0
    record['outsideTexture'] = float(outside.max())
    q = np.round(la * 255).astype(np.uint8)
    img = Image.fromarray(np.dstack([q[:, :, 0], q[:, :, 0], q[:, :, 0], q[:, :, 1]]), 'RGBA')
    name = f'race_map_{index:02d}_standings'
    img.save(out / 'textures' / (name + '.png'))
    on = 0
    for p in pts:
        X, Y = A[0] * p[0] + A[1] * p[2] + A[4], A[2] * p[0] + A[3] * p[2] + A[5]
        on += 0 <= int(Y) < 240 and 0 <= int(X) < 320 and np.asarray(mk)[int(Y), int(X)] > 60
    record['routeOnPicture'] = round(float(on) / max(1, len(pts)), 3)
    record['colour'], record['colourWhy'] = track_colour(root, index, course)
    return record, (L, Al)


def tex3ds(png, fmt):
    import tex3ds_cache
    from devkitpro import tool
    t3x = png.with_suffix('.t3x')
    tex3ds_cache.run([str(tool('tex3ds')), '-f', fmt, '-z', 'none', '-o', str(t3x), str(png)], check=True, stdout=subprocess.DEVNULL)
    return t3x


def cfloat(v):
    """A C++ float literal."""
    s = f'{v:.9g}'
    return s + ('f' if any(c in s for c in '.e') else '.f')


def digest(path):
    return hashlib.sha256(Path(path).read_bytes()).hexdigest()


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('--root', type=Path, default=ROOT, help='the cooked tree to read (default: this one)')
    p.add_argument('--output', type=Path, help='default <root>/build/race-standings')
    p.add_argument('--header', type=Path, help='default <root>/include/mkw/race_standings_assets.hpp')
    p.add_argument('--jobs', type=int, default=6)
    p.add_argument('--courses', default='', help='comma-separated catalog indices (checks; no header)')
    a = p.parse_args()
    root = a.root.resolve(); out = (a.output or root / 'build/race-standings').resolve()
    header = (a.header or root / 'include/mkw/race_standings_assets.hpp').resolve()
    (out / 'textures').mkdir(parents=True, exist_ok=True); (out / 'sheets').mkdir(exist_ok=True)
    tex = root / 'sd-card/3ds/mkw-3ds/textures'
    art.configure(tex)
    import course_catalog
    course_catalog.ROOT = root
    catalog = course_catalog.courses()
    races = [i for i, c in enumerate(catalog) if c['kind'] == 'race']
    wanted = [int(i) for i in a.courses.split(',')] if a.courses else races
    posed_js = exporter(root / 'build/export_brres.mjs', out, False); raw_js = exporter(root / 'build/export_brres.mjs', out, True)

    # Emblems: each race course's cup icon (the catalog lists them four to a cup).
    def cup_of(i):
        return f'ui_cup_{i // 4}.png', None
    tiles, tile_of, tile_info = [], {}, {}
    for i in wanted:
        icon, fallback = cup_of(i)
        if (icon, fallback) not in tile_info:
            lev, ok, used = art.stencil_tile(str(tex / icon), str(tex / fallback) if fallback else None)
            used = os.path.basename(used)
            if used not in tile_of:
                tile_of[used] = len(tiles); tiles.append((used, lev))
            tile_info[(icon, fallback)] = (tile_of[used], bool(ok))
    for n, (used, lev) in enumerate(tiles):
        Image.fromarray((lev.astype(np.uint16) * 255 // 2).astype(np.uint8), 'L').save(out / 'textures' / f'standings_tile_{n}.png')
        tex3ds(out / 'textures' / f'standings_tile_{n}.png', 'l8')

    # Courses.
    jobs = []
    for i in wanted:
        folder = root / 'assets/courses' / catalog[i]['key']
        jobs.append((root, out, i, catalog[i], posed_js, raw_js, ckpt(folder / 'course.kmp')[:2]))
    records, pictures = {}, {}
    with concurrent.futures.ProcessPoolExecutor(max_workers=a.jobs) as pool:
        for record, pic in pool.map(cook_course, jobs):
            records[record['index']] = record; pictures[record['index']] = pic
            print(f"{record['index']:3d} {record['name'][:30]:30s} {record.get('shape', record.get('error'))} {record.get('routeFit')} "
                  f"{record.get('angle')} deg route on picture {record.get('routeOnPicture')} {record.get('colourWhy', '')}", flush=True)
    for i in wanted:
        r = records[i]
        if 'error' in r:
            continue
        r['tile'], r['emblemSurvives'] = tile_info[cup_of(i)]
        r['emblem'] = tiles[r['tile']][0]
        tex3ds(out / 'textures' / f'race_map_{i:02d}_standings.png', 'la4')

    # By-eye sheets: every course's Standings background and silhouette.
    order = [i for i in wanted if 'error' not in records[i]]
    for s in range(0, len(order), 48):
        part = order[s:s + 48]; cols = 8
        sheet = Image.new('RGB', (cols * 174, ((len(part) + cols - 1) // cols) * 236), (16, 16, 16)); d = ImageDraw.Draw(sheet)
        for k, i in enumerate(part):
            L, Al = pictures[i]
            frame = preview(L, Al, records[i]['colour'], tiles[records[i]['tile']][1]).crop((150, 22, 320, 240))
            x, y = (k % cols) * 174, (k // cols) * 236
            sheet.paste(frame, (x + 2, y + 16))
            d.text((x + 2, y + 2), f'{i} {records[i]["name"][:17]} {records[i]["shape"][:4]}', fill=(255, 255, 0))
        sheet.save(out / 'sheets' / f'standings-{s // 48:02d}.png')

    # The atlas: the 12 number boxes (21x19) and the pointer (48x48).
    glyphs = art.Glyphs(root / 'include/mkw', tex)
    hdr = (root / 'include/mkw/race_map_assets.hpp').read_text(encoding='utf-8')
    ui = hdr.split('inline constexpr RaceMapAtlas RaceMapUi{')[1].split('};')[0]
    nums = [int(x) for x in re.findall(r'-?\d+', ui.split('{256,256},', 1)[1])]
    rects = [tuple(nums[i:i + 4]) for i in range(0, len(nums) - 6, 4)]
    # faces 24, row faces 24, the items (their count from the struct), digits 10, then colon, dot, timeLabel,
    # searchlight, fakeBox, white, rowPlayer, rowOther
    items = int(re.search(r'std::array<RaceMapRect,(\d+)> items;', hdr)[1])
    row_other = rects[24 + 24 + items + 10 + 7]
    ui_png = Image.open(root / 'build/race-map/textures/race_bottom_ui_256.png').convert('RGBA')
    other = ui_png.crop((row_other[0], row_other[1], row_other[0] + row_other[2], row_other[1] + row_other[3]))
    atlas = Image.new('RGBA', (128, 128)); number_rects = []
    for n in range(1, 13):
        x, y = ((n - 1) % 5) * 24 + 1, ((n - 1) // 5) * 21 + 1
        atlas.alpha_composite(art.number_box(glyphs, other, n), (x, y)); number_rects.append((x, y, 21, 19))
    atlas.alpha_composite(art.pointer(), (1, 66)); pointer_rect = (1, 66, 48, 48)
    atlas.save(out / 'textures/standings_ui.png'); tex3ds(out / 'textures/standings_ui.png', 'rgba8')

    # The name font: the quarter-size name glyphs' coverage, by character code.
    font = art.name_font(glyphs)
    codes = sorted(font)
    blob = bytearray(b'STNF') + struct.pack('<HH', len(codes), 14)
    data = bytearray(); table = bytearray()
    for code in codes:
        w, h, px = font[code]
        table += struct.pack('<HBBI', code, w, h, len(data)); data += px
    blob += table + data
    (out / 'textures/standings_names.bin').write_bytes(bytes(blob))
    L, Acap = art.text_coverage(font, 'MH'); rows = np.nonzero(Acap.max(1) > 40 / 255)[0]
    cap_top, cap_bottom = int(rows.min()), int(rows.max())

    if a.courses:
        print('checked', len(wanted), 'courses (no header)'); return
    files = [f'textures/race_map_{i:02d}_standings.t3x' for i in order] + [f'textures/standings_tile_{n}.t3x' for n in range(len(tiles))] + \
            ['textures/standings_ui.t3x', 'textures/standings_names.bin']
    manifest = {'version': 1, 'box': BOX, 'silhouette': [SIL_X, SIL_Y, SIL], 'files': {f: digest(out / f) for f in files},
                'tiles': [u for u, _ in tiles], 'courses': [records[i] for i in sorted(records)]}
    (out / 'race-standings.json').write_text(json.dumps(manifest, indent=1, default=lambda o: o.item() if hasattr(o, 'item') else str(o)) + '\n')

    lines = ['#pragma once', '// Generated by tools/cook_race_standings.py: the race lower screen\'s Standings view.',
             '#include <array>', '#include <cstdint>', 'namespace mkw {',
             '// Per catalog course (races only; arenas and courses without a route have no silhouette): its',
             '// LA4 silhouette texture, the world (x, z) -> screen affine (X = a x + b z + e, Y = c x + d z + f),',
             '// the background colour and its emblem tile.',
             'struct StandingsCourse {const char* silhouette;float a,b,c,d,e,f;uint8_t r,g,b_,tile;};',
             f'inline constexpr std::array<StandingsCourse,{len(catalog)}> StandingsCourses{{{{']
    for i, c in enumerate(catalog):
        r = records.get(i)
        if not r or 'error' in r:
            lines.append(f'{{nullptr,0,0,0,0,0,0,0,0,0,0}}, // {c["name"]}'); continue
        A = r['affine']; col = r['colour']
        lines.append(f'{{"textures/race_map_{i:02d}_standings.t3x",{",".join(cfloat(v) for v in A)},{col[0]},{col[1]},{col[2]},{r["tile"]}}}, '
                     f'// {r["name"]}: {r["shape"]}, {r["angle"]} deg, {r["emblem"]}')
    lines.append('}};')
    lines.append(f'inline constexpr std::array<const char*,{len(tiles)}> StandingsTiles{{{{' + ','.join(f'"textures/standings_tile_{n}.t3x"' for n in range(len(tiles))) + '}};')
    lines.append('struct StandingsRect {uint16_t x,y,w,h;};')
    lines.append('// The atlas (128x128 RGBA8): slot n\'s number box (the rows\' frame with the rank numeral), the heading pointer (pointing up, centred on the face).')
    lines.append('inline constexpr const char* StandingsAtlas="textures/standings_ui.t3x";inline constexpr unsigned StandingsAtlasSize=128;')
    lines.append('inline constexpr std::array<StandingsRect,12> StandingsNumbers{{' + ','.join('{%d,%d,%d,%d}' % r for r in number_rects) + '}};')
    lines.append('inline constexpr StandingsRect StandingsPointer{%d,%d,%d,%d};' % pointer_rect)
    lines.append('// The silhouette texture\'s top-left on the screen and its size; the map area (BOX).')
    lines.append(f'inline constexpr float StandingsSilX={SIL_X}.f,StandingsSilY={SIL_Y}.f,StandingsSilSize={SIL}.f;')
    lines.append(f'inline constexpr float StandingsBox[4]={{{BOX[0]}.f,{BOX[1]}.f,{BOX[2]}.f,{BOX[3]}.f}};')
    lines.append('// The name font ("STNF", u16 count, u16 cell height; per glyph u16 code, u8 w, u8 h, u32 offset; then')
    lines.append('// 8-bit coverage rows): the menus\' quarter-size name glyphs. Capitals (MH) span these cell rows.')
    lines.append(f'inline constexpr const char* StandingsFont="textures/standings_names.bin";inline constexpr int StandingsCapTop={cap_top},StandingsCapBottom={cap_bottom};')
    lines.append('// The emblem tint (lerp from the track colour): colour x TintScale + TintAdd, per channel.')
    lines.append(f'inline constexpr float StandingsTintScale={art.TINT[0]}f,StandingsTintAdd={art.TINT[1]}.f;')
    lines.append('inline constexpr bool StandingsAvailable=true;')
    lines += ['}', '']
    header.parent.mkdir(parents=True, exist_ok=True)
    header.write_text('\n'.join(lines), encoding='utf-8')
    kinds = {}
    for r in records.values():
        kinds[r.get('shape', 'error')] = kinds.get(r.get('shape', 'error'), 0) + 1
    print(json.dumps({'courses': len(order), 'shapes': kinds, 'tiles': len(tiles),
                      'fallbacks': sum(1 for r in records.values() if r.get('emblemSurvives') is False)}))


if __name__ == '__main__':
    main()
