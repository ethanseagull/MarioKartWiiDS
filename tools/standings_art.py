"""The Standings lower-screen view's art (tools/cook_race_standings.py), from the port's own cooked
assets only: the 12 rank numbers in their row boxes, the background emblem stencils (one per cup
icon), the player's heading pointer, and the small name font.

- Numbers: the race font's digits (ui_name_glyphs_0, white fill only), sheared and emboldened as
  the Wii HUD numerals, 1st-3rd filled with their own hud_rank_N fill texture, 4th-12th with one
  family profile measured row by row off the Wii's hud_rank_4..12; a thin white rim, the Wii's
  black edge (5.5% of the height), its grey 0x393939 3D block, a 1 px black edge after the shrink;
  in 10-12 the second digit keeps its own black edge over the first. 15 px tall, at most 20 wide,
  centred in the rows' frame (RaceMapUi.rowOther nine-sliced to 21 px).
- Stencils: the cup icon (ui_cup_N) as a flat two-tone emblem with negative-space cut-outs, at
  36 px and 20 px in a 64x64 tile; levels only (0 background, 1 one mid tone, 2 tint): the runtime
  colours them through the material (lerp(track colour, tint, level)). Shape icons cut their dark
  cluster, enclosed features or off-colour pieces (whichever gives a design-like cut), round badge
  icons become geometric coins with a ring gap and the symbol; contours smoothed at 512 px, near-symmetric
  emblems mirrored exactly, a 1 px tint wall keeps every cut inside the outline, too-small cuts are
  filled per size, visual weight evened by drawn size. OVERRIDES9 / OVERRIDES10 hold the per-emblem
  calls the automatic pick gets wrong. An icon whose picture does not survive falls back to the
  retail emblem of its course's slot.
"""
import math
import os
import re
from collections import Counter, deque

import numpy as np
from PIL import Image, ImageChops, ImageDraw, ImageFilter

TEX = None  # the cooked texture PNGs (sd-card/3ds/mkw-3ds/textures); set by configure()
HI = 128    # analysis size
BIG = 512   # contour size


def configure(textures):
    global TEX
    TEX = str(textures)


# ---------------------------------------------------------------- masks

def otsu(v):
    v = v[np.isfinite(v)]
    if v.size < 10:
        return None
    hist, edges = np.histogram(v, 64, (0, 1)); p = hist / hist.sum(); best = (0, None)
    for k in range(1, 64):
        w0, w1 = p[:k].sum(), p[k:].sum()
        if w0 < .02 or w1 < .02:
            continue
        m0 = (p[:k] * edges[:k]).sum() / w0; m1 = (p[k:] * edges[k:64]).sum() / w1
        s = w0 * w1 * (m0 - m1) ** 2
        if s > best[0]:
            best = (s, edges[k])
    return best[1]


def components(mask):
    H, W = mask.shape; lab = np.zeros((H, W), np.int32); sizes = [0]; n = 0
    for y in range(H):
        for x in range(W):
            if mask[y, x] and not lab[y, x]:
                n += 1; q = deque([(y, x)]); lab[y, x] = n; c = 0
                while q:
                    cy, cx = q.popleft(); c += 1
                    for dy, dx in ((1, 0), (-1, 0), (0, 1), (0, -1)):
                        ny, nx = cy + dy, cx + dx
                        if 0 <= ny < H and 0 <= nx < W and mask[ny, nx] and not lab[ny, nx]:
                            lab[ny, nx] = n; q.append((ny, nx))
                sizes.append(c)
    return lab, sizes


def smooth_mask(m, r=2.0, th=.5):
    im = Image.fromarray((m * 255).astype(np.uint8)).filter(ImageFilter.GaussianBlur(r))
    return np.asarray(im) > th * 255


def fill_holes(m, max_px):
    lab, sizes = components(~m)
    border = set(np.unique(np.concatenate([lab[0], lab[-1], lab[:, 0], lab[:, -1]])))
    out = m.copy()
    for k in range(1, len(sizes)):
        if k not in border and sizes[k] <= max_px:
            out[lab == k] = True
    return out


def keep_largest(m, n=3, min_share=.04):
    lab, sizes = components(m); tot = max(1, m.sum())
    order = sorted(range(1, len(sizes)), key=lambda k: -sizes[k])[:n]
    keep = [k for k in order if sizes[k] / tot >= min_share]
    return np.isin(lab, keep)


def idx_of(png):
    m = re.match(r'ui_cup_(\d+)\.png$', os.path.basename(png)); return int(m.group(1)) if m else -1


def analyse(png):
    """-> (shape, cut, kind, ok): HIxHI masks and whether the icon's own picture survives."""
    im = Image.open(png).convert('RGBA').resize((HI, HI), Image.LANCZOS)
    a = np.asarray(im).astype(float); alpha = a[:, :, 3] / 255; rgb = a[:, :, :3] / 255
    lum = rgb @ np.array([.3, .59, .11]); shape = alpha > .5
    if shape.sum() < 50:
        return shape, np.zeros_like(shape), 'empty', False
    ys, xs = np.nonzero(shape); cy, cx = ys.mean(), xs.mean()
    r = np.hypot(ys - cy, xs - cx); R = r.max()
    roundness = shape.sum() / (np.pi * R * R)
    yy, xx = np.mgrid[0:HI, 0:HI]; rr = np.hypot(yy - cy, xx - cx)
    # retail cup emblems (ui_cup_0..7) are shapes, even the round mushroom: never badges
    retail = 0 <= idx_of(png) <= 7
    # a badge's outline is a circle: its edge pixels all sit near R (badges .89-.98; the round
    # 3DS mushroom ui_cup_32 is .82 and is a shape)
    er = np.asarray(Image.fromarray(shape.astype(np.uint8) * 255).filter(ImageFilter.MinFilter(3))) > 0
    by, bx = np.nonzero(shape & ~er); br = np.hypot(by - cy, bx - cx)
    circular = br.min() / max(br.max(), 1e-6) >= .87
    if roundness > .86 and circular and not retail:
        parts = badge_parts(png) if idx_of(png) not in BADGE_V10 else None
        if parts is not None:
            sil, detail, ok = parts
            gap = shape & (rr > .74 * R) & (rr < .80 * R)
            return shape, sil | gap, 'badge', ok
        # a badge: inner area = 0.70 R; its background colour = the median of the inner ring
        inner = shape & (rr < .70 * R)
        ring = inner & (rr > .55 * R)
        bgc = np.median(rgb[ring], 0)
        dist = np.linalg.norm(rgb - bgc, axis=2)
        t = otsu(np.where(inner, dist / max(1e-6, dist[inner].max()), np.nan))
        sym = inner & (dist / max(1e-6, dist[inner].max()) > (t or .5))
        sym = smooth_mask(sym, 2.5) & inner
        sym = fill_holes(keep_largest(sym, 3, .06), 60)
        share = sym.sum() / max(1, inner.sum())
        lab, sizes = components(sym); big = max(sizes[1:] or [0]) / max(1, sym.sum())
        ok = .12 <= share <= .55 and big >= .70
        gap = shape & (rr > .74 * R) & (rr < .80 * R)
        return shape, sym | gap, 'badge', ok
    # a shape icon: cut the darker luminance cluster inside it
    t = otsu(np.where(shape, lum, np.nan))
    dark = shape & (lum < (t if t is not None else 0))
    dark = smooth_mask(dark, 2.0) & shape
    # never cut the whole outline away: drop dark pixels touching the shape's edge band
    edge = shape & ~np.asarray(Image.fromarray(shape.astype(np.uint8) * 255).filter(ImageFilter.MinFilter(7))).astype(bool)
    cut = fill_holes(keep_largest(dark & ~edge, 6, .02), 30)
    share = cut.sum() / shape.sum()
    ok = share <= .45
    if share < .03:
        cut = np.zeros_like(cut)   # nothing worth cutting: the plain silhouette is the design
    return shape, cut, 'shape', ok


def segments(rgb, mask, step):
    """Labels of the regions joined by 4-neighbour colour steps under `step` (min-label spread)."""
    H, W = mask.shape; lab = np.where(mask, np.arange(H * W).reshape(H, W), -1)
    big = H * W + 1
    joins = []
    for dy, dx in ((1, 0), (0, 1)):
        a = np.linalg.norm(rgb[dy:, dx:] - rgb[:H - dy, :W - dx], axis=2) < step
        a &= mask[dy:, dx:] & mask[:H - dy, :W - dx]
        joins.append((dy, dx, a))
    while True:
        old = lab.copy()
        for dy, dx, a in joins:
            l1 = lab[dy:, dx:]; l0 = lab[:H - dy, :W - dx]
            m = np.where(a, np.minimum(l0, l1), big)
            lab[dy:, dx:] = np.where(a, np.minimum(l1, m), l1)
            lab[:H - dy, :W - dx] = np.where(a, np.minimum(lab[:H - dy, :W - dx], m), lab[:H - dy, :W - dx])
        if (lab == old).all():
            return lab


def kmeans(px, k, it=12):
    """Plain k-means on colour samples (deterministic start: spread along luminance)."""
    px = px.reshape(-1, 3)
    if len(px) <= k:
        return np.vstack([px, np.zeros((k - len(px), 3))]) if len(px) else np.zeros((k, 3))
    order = np.argsort(px @ np.array([.3, .59, .11]))
    c = px[order[np.linspace(0, len(px) - 1, k).astype(int)]].copy()
    for _ in range(it):
        lab = np.argmin(np.linalg.norm(px[:, None] - c[None], axis=2), 1)
        for j in range(k):
            if (lab == j).any():
                c[j] = px[lab == j].mean(0)
    return c


_BADGE_PARTS = {}


def badge_parts(png):
    if png not in _BADGE_PARTS:
        _BADGE_PARTS[png] = _badge_parts(png)
    return _BADGE_PARTS[png]


def _badge_parts(png):
    """A round badge's picture at HI px -> (silhouette, detail, ok), or None when the badge has
    no clear inner disc. Replaces the fixed reading (background = the median of .55-.70 R, symbol =
    what differs from it), which on the Mario Kart badges sampled their dark inner border and on
    the shaded badges took the whole disc for the symbol (18 cup icons fell back, others came out
    as empty rings).
    - Inner disc edge: radii .60-.92 R where the band just inside is even round the circle and
      differs from the band just outside (the badge's border or halo); each candidate is tried.
    - Colour models: the background's 3 colours from the band just inside the edge (shading,
      sparkles), the symbol's 4 from the middle without the background's; each inner pixel goes
      to the nearer model. Pieces lying along the rim are shading, not symbol.
    - The candidate whose symbol fills a plausible share of the disc (about 40%) and keeps off
      its rim wins.
    - Silhouette: the symbol with its holes filled. Detail (drawn back in tint inside the cut):
      background-coloured areas the symbol encloses (the triangle between the Triforce's three)
      and its own dark features (eyes, inner lines), off its outline."""
    im = Image.open(png).convert('RGBA').resize((HI, HI), Image.LANCZOS)
    a = np.asarray(im).astype(float); shape = a[:, :, 3] > 127; rgb = a[:, :, :3] / 255
    lum = rgb @ np.array([.3, .59, .11])
    ys, xs = np.nonzero(shape); cy, cx = ys.mean(), xs.mean(); R = np.hypot(ys - cy, xs - cx).max()
    yy, xx = np.mgrid[0:HI, 0:HI]; rr = np.hypot(yy - cy, xx - cx) / R
    NA = 48
    ang = ((np.arctan2(yy - cy, xx - cx) + np.pi) / (2 * np.pi) * NA).astype(int) % NA
    def rim(r0, r1):
        m = shape & (rr >= r0) & (rr < r1); out = np.zeros((NA, 3)); have = np.zeros(NA, bool)
        for k in range(NA):
            q = m & (ang == k)
            if q.sum():
                out[k] = np.median(rgb[q], 0); have[k] = True
        return out, have
    cands = []
    for r in np.arange(.60, .94, .02):
        c_in, h_in = rim(r - .07, r - .02); c_out, h_out = rim(r + .01, r + .06)
        if h_in.mean() < .9 or h_out.mean() < .9:
            continue
        sm = np.median(np.linalg.norm(c_in - np.roll(c_in, 1, 0), axis=1))
        contrast = np.median(np.linalg.norm(c_in - c_out, axis=1))
        if contrast - 2 * sm >= .08:
            cands.append(r)
    if not cands:
        return None
    best = None
    for r_in in cands:
        inner = shape & (rr < r_in - .02)
        bgc = kmeans(rgb[shape & (rr >= r_in - .10) & (rr < r_in - .02)], 3)
        d_bg = np.min(np.linalg.norm(rgb[:, :, None, :] - bgc[None, None], axis=3), 2)
        mid = shape & (rr < .30) & (d_bg > .15)
        if mid.sum() < 20:
            mid = inner & (d_bg > .15)
        if mid.sum() < 20:
            continue
        fgc = kmeans(rgb[mid], 4)
        d_fg = np.min(np.linalg.norm(rgb[:, :, None, :] - fgc[None, None], axis=3), 2)
        bgm = inner & ((d_bg <= d_fg) | (d_bg < .08))
        raw = smooth_mask(inner & ~bgm, 1.2) & inner
        lab, sizes = components(raw); outer = rr > r_in - .14
        for k in range(1, len(sizes)):
            piece = lab == k
            if (piece & outer).sum() > .5 * sizes[k]:
                raw[piece] = False
        raw = keep_largest(raw, 4, .05)
        if not raw.any():
            continue
        sil = smooth_mask(fill_holes(raw, 10 ** 6), 1.5) & inner
        share = sil.sum() / max(1, inner.sum())
        touch = (sil & (rr > r_in - .08)).sum() / max(1, sil.sum())
        score = -abs(share - .40) - 2 * touch
        if best is None or score > best[0]:
            best = (score, r_in, inner, raw, sil, share)
    if best is None:
        return np.zeros_like(shape), np.zeros_like(shape), False
    _, r_in, inner, raw, sil, share = best; BADGE_INFO[png] = r_in
    core = np.asarray(Image.fromarray(sil.astype(np.uint8) * 255).filter(ImageFilter.MinFilter(7))) > 0
    enclosed = smooth_mask(sil & ~raw, 1.0) & core
    t1 = otsu(np.where(raw, lum, np.nan))
    dark = raw & core & (lum < (t1 if t1 is not None else 0) * .80)
    dark = smooth_mask(dark, 1.0) & raw & core
    detail = enclosed | dark
    lab, sizes = components(detail); tot = max(1, sil.sum()); keep = np.zeros_like(detail)
    for k in range(1, len(sizes)):
        if sizes[k] / tot >= .012:
            keep |= lab == k
    detail = keep & sil
    if detail.sum() / tot > .40:          # the detail is the symbol's body: the plain silhouette
        detail = np.zeros_like(detail)
    lab, sizes = components(sil); big = max(sizes[1:] or [0]) / max(1, sil.sum())
    ok = .05 <= share <= .80 and big >= .45
    return sil, detail, ok


def up(m):
    """A 128 px mask to BIG px with smooth contours."""
    im = Image.fromarray(m.astype(np.uint8) * 255).resize((BIG, BIG), Image.BILINEAR).filter(ImageFilter.GaussianBlur(BIG / 128 * 1.6))
    return np.asarray(im) > 127


def symmetric(m):
    f = m[:, ::-1]; inter = (m & f).sum(); uni = max(1, (m | f).sum())
    return inter / uni


def symmetrize(shape, cut):
    # centre on the shape's bounding box first, then average with the mirror
    xs = np.nonzero(shape.any(0))[0]; c = (xs[0] + xs[-1]) / 2; dx = int(round(BIG / 2 - c))
    shape = np.roll(shape, dx, 1); cut = np.roll(cut, dx, 1)
    if symmetric(shape) > .90:
        fs = (shape.astype(float) + shape[:, ::-1]) / 2 >= .5
        fc = (cut.astype(float) + cut[:, ::-1]) / 2 >= .5 if cut.any() and symmetric(cut) > .88 else cut
        return fs, fc & fs, True
    return shape, cut & shape, False


BADGE_INFO = {}    # png -> the inner disc radius badge_parts found (checks)
COIN_DETAIL = {}   # png -> the coin symbol's detail (BIG px), set by coin()


def coin(png):
    """A round badge icon as a geometric coin: disc, ring gap (radii .78-.86), the symbol fitted inside .66."""
    shape, cut, kind, ok = analyse(png)
    if kind != 'badge':
        return None
    yy, xx = np.mgrid[0:BIG, 0:BIG]; c = BIG / 2; R = BIG * .48
    rr = np.hypot(yy - c, xx - c)
    disc = rr <= R; gap = (rr > .78 * R) & (rr < .86 * R)
    if idx_of(png) in BADGE_V10:
        # the v10 reading (its symbol cut, inside .72 R), no detail
        ys, xs = np.nonzero(shape); cy, cx = ys.mean(), xs.mean(); r0 = np.hypot(ys - cy, xs - cx).max()
        yy0, xx0 = np.mgrid[0:HI, 0:HI]; sym = cut & (np.hypot(yy0 - cy, xx0 - cx) < .72 * r0); det = np.zeros_like(sym)
    else:
        parts = badge_parts(png)
        if parts is None:
            return None
        sym, det, _ = parts
    if sym.sum() < 10:
        return None
    b = Image.fromarray(sym.astype(np.uint8) * 255); bb = b.getbbox()
    side = int(2 * .66 * R / 1.0); sc = side / max(bb[2] - bb[0], bb[3] - bb[1])
    def fit(m):
        im = Image.fromarray(m.astype(np.uint8) * 255).crop(bb)
        im = im.resize((max(1, int(im.width * sc)), max(1, int(im.height * sc))), Image.BILINEAR).filter(ImageFilter.GaussianBlur(4))
        canvas = Image.new('L', (BIG, BIG)); canvas.paste(im, (int(c - im.width / 2), int(c - im.height / 2)))
        return np.asarray(canvas) > 127
    symb, detb = fit(sym), fit(det)
    if symmetric(symb) > .85:
        symb = (symb.astype(float) + symb[:, ::-1]) / 2 >= .5
        if detb.any() and symmetric(detb) > .80:
            detb = (detb.astype(float) + detb[:, ::-1]) / 2 >= .5
    COIN_DETAIL[png] = detb & symb
    return disc, symb & disc, gap & disc


def edge_band(shp):
    e = np.asarray(Image.fromarray(shp.astype(np.uint8) * 255).filter(ImageFilter.MinFilter(3))) > 0
    return shp & ~e


def levels(px, shape, cut, ring=None, detail=None):
    """One size: 0 background, 1 mid tone, 2 tint. `detail`: tint islands inside the cut."""
    ink = shape & ~cut
    if detail is not None:
        ink |= detail & shape
    if ring is not None and px >= 30:
        ink &= ~ring
    cov = np.asarray(Image.fromarray(ink.astype(np.uint8) * 255).resize((px, px), Image.BOX)) / 255
    lev = np.where(cov > .70, 2, np.where(cov > .30, 1, 0))
    # cuts that came out too small at this size: fill them (20 px keeps only the clearest cuts)
    min_cut = 3 if px >= 30 else 5
    shp = np.asarray(Image.fromarray(shape.astype(np.uint8) * 255).resize((px, px), Image.BOX)) > 127
    holes = shp & (lev < 2)
    lab, sizes = components(holes & ~edge_band(shp))
    for k in range(1, len(sizes)):
        if sizes[k] < min_cut:
            lev[lab == k] = 2
    # strays: a lone tint pixel with no tint 4-neighbour goes; a lone hole in tint is filled
    for _ in range(2):
        t = lev == 2; nb = np.zeros_like(t, int)
        for dy, dx in ((1, 0), (-1, 0), (0, 1), (0, -1)):
            nb += np.roll(np.roll(t, dy, 0), dx, 1)
        lev[t & (nb == 0)] = 0
        lev[(~t) & (nb == 4)] = 2
    # mid tone only where it touches tint
    t = lev == 2; nb = np.zeros_like(t, int)
    for dy, dx in ((1, 0), (-1, 0), (0, 1), (0, -1)):
        nb += np.roll(np.roll(t, dy, 0), dx, 1)
    lev[(lev == 1) & (nb == 0)] = 0
    return lev


def colour_cut(png):
    """Pieces whose colour differs strongly from the shape's dominant colour (the S on its diamond)."""
    im = Image.open(png).convert('RGBA').resize((128, 128), Image.LANCZOS)
    a = np.asarray(im).astype(float); shape = a[:, :, 3] > 127; rgb = a[:, :, :3] / 255
    dom = np.median(rgb[shape], 0); dist = np.linalg.norm(rgb - dom, axis=2)
    t = otsu(np.where(shape, dist / max(1e-6, dist[shape].max()), np.nan))
    lum = rgb @ np.array([.3, .59, .11])
    cand = shape & (dist / max(1e-6, dist[shape].max()) > (t or .5)) & (lum > .15)   # not the black outline
    cand = smooth_mask(cand, 1.5) & shape
    band = shape & ~(np.asarray(Image.fromarray(shape.astype(np.uint8) * 255).filter(ImageFilter.MinFilter(5))) > 0)
    lab, sizes = components(cand); cut = np.zeros_like(shape); tot = shape.sum()
    for k in range(1, len(sizes)):
        piece = lab == k
        if (piece & band).sum() / sizes[k] < .2 and .004 <= sizes[k] / tot <= .30:
            cut |= piece & ~band
    return cut


def features_cut(png):
    """Enclosed small features at both ends of the icon's luminance (eyes, spots), each piece
    0.4%-12% of the shape and not touching its outline."""
    im = Image.open(png).convert('RGBA').resize((128, 128), Image.LANCZOS)
    a = np.asarray(im).astype(float); shape = a[:, :, 3] > 127; lum = (a[:, :, :3] / 255) @ np.array([.3, .59, .11])
    t1 = otsu(np.where(shape, lum, np.nan))
    if t1 is None:
        return shape, np.zeros_like(shape)
    t0 = otsu(np.where(shape & (lum < t1), lum, np.nan)) or t1 * .6
    t2 = otsu(np.where(shape & (lum >= t1), lum, np.nan)) or (1 + t1) / 2
    band = shape & ~(np.asarray(Image.fromarray(shape.astype(np.uint8) * 255).filter(ImageFilter.MinFilter(5))) > 0)
    cut = np.zeros_like(shape); tot = shape.sum()
    for cand in (shape & (lum < t0), shape & (lum > t2)):
        cand = smooth_mask(cand, 1.2) & shape
        lab, sizes = components(cand)
        for k in range(1, len(sizes)):
            piece = lab == k
            if not (piece & band).any() and .004 <= sizes[k] / tot <= .12:
                cut |= piece
    return shape, cut


# The automatic cut pick's own corrections (lightning plain, the S diamond and the SNES fire flower
# take their dark cluster).
AUTO_OVERRIDE = {7: 'none'}


def prepare_auto(png):
    """-> dict(shape, cut, ring, sym, kind, ok) at BIG px: the automatic pick."""
    c = coin(png)
    if c is not None:
        disc, symb, gap = c; _, _, _, ok = analyse(png)
        return dict(shape=disc, cut=symb, ring=gap, sym=True, kind='coin', ok=ok, detail=COIN_DETAIL.get(png))
    shape, cut7, kind, ok = analyse(png)
    _, cutf = features_cut(png)
    # pick per icon by which gives a design-like cut (about a fifth of the shape, a few pieces)
    def score(c):
        share = c.sum() / max(1, shape.sum()); _, sz = components(c); pieces = sum(1 for z in sz[1:] if z >= 6)
        return -abs(share - .18) * 4 + (0.3 if 2 <= pieces <= 10 else 0) - (2 if share > .40 else 0) - (1 if share < .01 else 0)
    cutc = colour_cut(png)
    cut = max([cut7, cutf, cut7 | cutf, cutc, cutc | cutf], key=score); ok = True
    ov = AUTO_OVERRIDE.get(idx_of(png))
    if ov == 'none':
        cut = np.zeros_like(cut)
    elif ov == 'dark':
        cut = cut7
    sh, ct = up(shape), up(cut)
    sh, ct, sy = symmetrize(sh, ct)
    return dict(shape=sh, cut=ct, ring=None, sym=sy, kind=kind, ok=ok)


# Per-emblem calls on top of the automatic pick: cut = which detector; open = px (at 512) of an
# opening on the shape; small_cut = keep cut-outs at 20 px.
OVERRIDES = {
    3: dict(cut='features', open=10, small_cut=False, note='crown: clean spikes; jewels and eyes at 36 px only'),
    7: dict(cut='none', note='lightning: a plain bolt reads best'),
    4: dict(cut='colour', note='green shell: its white rim band (the dark cut read as a face)'),
}
# Emblems mirrored whatever their cut-outs, and badges read the older way (v10) instead of
# badge_parts': none of the retail cup icons needs either.
MIRROR = set()
BADGE_V10 = set()
# Recipes: mushrooms get even round spots; coins whose own picture is mush at 20 px get a stand-in
# symbol in the same coin.
RECIPES = {
    0: dict(recipe='spots', note='retail mushroom: even round spots and eyes'),
}
TINT = (1.42, 14)          # one tint for all: base x 1.42 + 14 (the runtime's material colour)
TARGET_INK = .44           # visual weight target: share of the cell in tint


def prepare_overridden(png):
    i = idx_of(png); ov = OVERRIDES.get(i, {})
    c = coin(png)
    if c is not None and not ov:
        disc, symb, gap = c; _, _, _, ok = analyse(png)
        return dict(shape=disc, cut=symb, ring=gap, sym=True, kind='coin', ok=ok, ov='', detail=COIN_DETAIL.get(png))
    p = prepare_auto(png)
    if ov:
        shape128, cut7, _, _ = analyse(png)
        cut = {'none': np.zeros_like(cut7), 'features': features_cut(png)[1], 'colour': colour_cut(png), 'dark': cut7}[ov['cut']]
        sh, ct = up(shape128), up(cut)
        if ov.get('open'):
            r = ov['open']; im = Image.fromarray(sh.astype(np.uint8) * 255)
            sh = np.asarray(im.filter(ImageFilter.MinFilter(2 * r + 1)).filter(ImageFilter.MaxFilter(2 * r + 1))) > 0
        sh, ct, sy = symmetrize(sh, ct)
        p = dict(shape=sh, cut=ct & sh, ring=None, sym=sy, kind=p['kind'], ok=True, ov=ov.get('note', ''), small_cut=ov.get('small_cut', True))
    else:
        p['ov'] = ''
    return p


def shape_small(shape, px):
    return np.asarray(Image.fromarray(shape.astype(np.uint8) * 255).resize((px, px), Image.BOX)) > 127


def levels_walled(px, prep):
    shape, cut, ring = prep['shape'], prep['cut'], prep['ring']
    if px < 30 and not prep.get('small_cut', True):
        cut = np.zeros_like(cut)
    # the wall: cut-outs end at least one output pixel inside the outline
    wall = int(round(BIG / px * 1.0))
    inside = np.asarray(Image.fromarray(shape.astype(np.uint8) * 255).filter(ImageFilter.MinFilter(2 * wall + 1))) > 0
    cut = cut & inside
    if prep['kind'] == 'coin' and px < 30:
        # small coins: keep the symbol only if it stays legible at 20 px, else an inner ring
        test = levels(px, shape, cut, None); holes = shape_small(shape, px) & (test < 2)
        lab, sizes = components(holes); pieces = sum(1 for z in sizes[1:] if z >= 3); area = sum(z for z in sizes[1:] if z >= 3)
        if pieces > 3 or area < 10:
            yy, xx = np.mgrid[0:BIG, 0:BIG]; rr = np.hypot(yy - BIG / 2, xx - BIG / 2); R = BIG * .48
            cut = (rr > .50 * R) & (rr < .64 * R); prep = dict(prep, detail=None)
    det = prep.get('detail')
    if det is not None:
        det = det & cut
        if px < 30 and not det_survives(px, shape, cut, det):
            det = None
    return levels(px, shape, cut, ring, det)


def det_survives(px, shape, cut, det):
    """Small sizes keep the detail only if it adds a readable piece (>= 3 px of tint) and still
    leaves the symbol's cut open around it."""
    a = levels(px, shape, cut, None, det); b = levels(px, shape, cut, None, None)
    return ((a == 2) & (b < 2)).sum() >= 3 and (a < 2).sum() >= .5 * (b < 2).sum()


def disc_masks():
    yy, xx = np.mgrid[0:BIG, 0:BIG]; c = BIG / 2; R = BIG * .48; rr = np.hypot(yy - c, xx - c)
    return rr <= R, (rr > .78 * R) & (rr < .86 * R), R, yy, xx, c


def symbol_mask(kind):
    disc, gap, R, yy, xx, c = disc_masks()
    if kind == 'cherries':
        m = Image.new('L', (BIG, BIG)); d = ImageDraw.Draw(m); r = R * .22
        for cx, cy in ((c - R * .24, c + R * .22), (c + R * .22, c + R * .30)):
            d.ellipse((cx - r, cy - r, cx + r, cy + r), fill=255)
        w = int(R * .07)
        d.line([(c - R * .24, c + R * .02), (c + R * .02, c - R * .42)], fill=255, width=w)
        d.line([(c + R * .22, c + R * .10), (c + R * .02, c - R * .42)], fill=255, width=w)
        d.ellipse((c - R * .02 - w, c - R * .42 - w, c + R * .02 + w, c - R * .42 + w), fill=255)
        return np.asarray(m) > 127
    if kind == 'mushroom':
        # a domed cap over a short stem, a clear gap between them
        m = Image.new('L', (BIG, BIG)); d = ImageDraw.Draw(m)
        d.pieslice((c - R * .58, c - R * .52, c + R * .58, c + R * .40), 180, 360, fill=255)
        d.rectangle((c - R * .58, c - R * .07, c + R * .58, c - R * .02), fill=255)
        d.rounded_rectangle((c - R * .24, c + R * .06, c + R * .24, c + R * .50), radius=int(R * .10), fill=255)
        return np.asarray(m) > 127
    if kind.startswith('icon:'):
        # another badge's symbol as badge_parts reads it (the same M or L drawn better there)
        a = Image.fromarray(badge_parts(os.path.join(TEX, f'ui_cup_{int(kind.split(":")[1])}.png'))[0].astype(np.uint8) * 255)
        a = a.crop(a.getbbox()); side = 2 * .62 * R; sc = side / max(a.width, a.height)
        a = a.resize((max(1, int(a.width * sc * 4)), max(1, int(a.height * sc * 4))), Image.BILINEAR).filter(ImageFilter.GaussianBlur(6)).resize((max(1, int(a.width * sc)), max(1, int(a.height * sc))), Image.LANCZOS)
        m = Image.new('L', (BIG, BIG)); m.paste(a, (int(c - a.width / 2), int(c - a.height / 2)))
        sym = np.asarray(m) > 127
        if symmetric(sym) > .85:
            sym = (sym.astype(float) + sym[:, ::-1]) / 2 >= .5
        return sym
    # a retail emblem's silhouette, fitted into .62 R
    png = os.path.join(TEX, f'ui_cup_{int(kind.split(":")[1])}.png')
    a = Image.open(png).convert('RGBA').split()[3]; a = a.crop(a.getbbox())
    side = 2 * .62 * R; sc = side / max(a.width, a.height)
    a = a.resize((max(1, int(a.width * sc)), max(1, int(a.height * sc))), Image.LANCZOS)
    m = Image.new('L', (BIG, BIG)); m.paste(a, (int(c - a.width / 2), int(c - a.height / 2)))
    sym = np.asarray(m) > 127
    if symmetric(sym) > .85:
        sym = (sym.astype(float) + sym[:, ::-1]) / 2 >= .5
    return sym


def spots_recipe(png):
    """Mushroom: the silhouette (symmetrised); light cap spots as even round cut-outs at their
    centres; the two eyes as even ovals."""
    im = Image.open(png).convert('RGBA').resize((128, 128), Image.LANCZOS); a = np.asarray(im).astype(float)
    shape = a[:, :, 3] > 127; rgb = a[:, :, :3] / 255; lum = rgb @ np.array([.3, .59, .11])
    sat = rgb.max(2) - rgb.min(2)
    ys, xs = np.nonzero(shape); top, bot = ys.min(), ys.max(); h = bot - top
    cap = shape & (np.arange(128)[:, None] < top + .62 * h)
    light = cap & (lum > .70) & (sat < .25)
    lab, sizes = components(light)
    sh = up(shape)
    sh, _, _ = symmetrize(sh, np.zeros_like(sh))
    xs2 = np.nonzero(sh.any(0))[0]; dx = (BIG / 2) - (xs2[0] + xs2[-1]) / 2
    W = xs2[-1] - xs2[0]
    cut = Image.new('L', (BIG, BIG)); d = ImageDraw.Draw(cut)
    r = W * .085                                     # one spot size for every mushroom
    centres = []
    for k in range(1, len(sizes)):
        if sizes[k] < 12:
            continue
        py, px = np.argwhere(lab == k).mean(0)
        centres.append((px * BIG / 128 + dx, py * BIG / 128))
    for x, y in [p for (x, y) in centres for p in ((x, y), (BIG - x, y))]:
        d.ellipse((x - r, y - r, x + r, y + r), fill=255)
    face = shape & (np.arange(128)[:, None] >= top + .55 * h) & (lum < .25)
    lab2, sz2 = components(face)
    eyes = [np.argwhere(lab2 == k).mean(0) for k in range(1, len(sz2)) if 3 <= sz2[k] <= 120]
    if len(eyes) >= 2:
        ey = np.mean([e[0] for e in eyes]) * BIG / 128; ex = abs(np.mean([abs(e[1] * BIG / 128 + dx - BIG / 2) for e in eyes]))
        ew, eh = W * .045, W * .085
        for sx in (-1, 1):
            d.ellipse((BIG / 2 + sx * ex - ew, ey - eh, BIG / 2 + sx * ex + ew, ey + eh), fill=255)
    return sh, (np.asarray(cut) > 127) & sh


def prepare(png):
    """The emblem's masks with every table applied, and its weight factor."""
    i = idx_of(png); ov = RECIPES.get(i, {})
    if ov.get('recipe') == 'spots':
        sh, ct = spots_recipe(png)
        p = dict(shape=sh, cut=ct, ring=None, sym=True, kind='shape', ok=True, ov=ov['note'])
    elif ov.get('recipe') == 'coin':
        disc, gap, *_ = disc_masks(); sym = symbol_mask(ov['symbol'])
        p = dict(shape=disc, cut=sym & disc, ring=gap & disc, sym=True, kind='coin', ok=True, ov=ov['note'])
    else:
        p = prepare_overridden(png)
    p['mirror'] = i in MIRROR
    # visual weight: an emblem whose ink share at 36 px is over TARGET_INK is drawn a little smaller
    lev = levels_walled(36, p)
    ink = (lev == 2).mean() + .5 * (lev == 1).mean()
    p['weight'] = float(np.clip(math.sqrt(TARGET_INK / max(ink, 1e-3)), .84, 1.0))
    return p


def scaled(prep, f):
    def sc(m):
        if m is None:
            return None
        im = Image.fromarray(m.astype(np.uint8) * 255); n = int(BIG * f)
        im = im.resize((n, n), Image.BILINEAR); out = Image.new('L', (BIG, BIG)); out.paste(im, ((BIG - n) // 2, (BIG - n) // 2))
        return np.asarray(out) > 127
    q = dict(prep); q['shape'] = sc(prep['shape']); q['cut'] = sc(prep['cut']); q['ring'] = sc(prep['ring']); q['detail'] = sc(prep.get('detail')); return q


def emblem_levels(prep, px):
    margin = (px - 2) / px                              # a 1 px clear margin inside every cell
    lev = levels_walled(px, scaled(prep, margin * prep.get('weight', 1.0)))
    if prep.get('sym'):
        # exactly mirror-symmetric, but only as a clean-up: when the mirror moves at most 15 of
        # the 36 px emblem's pixels (4 at 20 px). The flag says only that the outline is
        # symmetric; an asymmetric picture inside it (the S on its diamond, a bolt, the L, a shell
        # in a coin) used to be mirrored into a different one.
        h = px // 2; m = lev.copy(); m[:, px - h:] = lev[:, :h][:, ::-1]
        if prep.get('mirror') or (m != lev).sum() <= .0116 * px * px:
            lev = m
    lev[0, :] = 0; lev[-1, :] = 0; lev[:, 0] = 0; lev[:, -1] = 0
    return lev


def stencil_tile(png, fallback=None):
    """The 64x64 tile's levels (0, 1, 2): the 36 px emblem at (3, 3), the 20 px one at (42, 42).
    Returns (levels, survives, used png)."""
    p = prepare(png); used = png; own = bool(p['ok'])
    if not own and fallback:
        p = prepare(fallback); used = fallback
    t = np.zeros((64, 64), np.uint8)
    t[3:39, 3:39] = emblem_levels(p, 36); t[42:62, 42:62] = emblem_levels(p, 20)
    return t, own, used


def tile_colours(levels_, base):
    """The tile as the runtime colours it (sheets and checks)."""
    b = np.array(base, float); tint = np.minimum(255, b * TINT[0] + TINT[1])
    out = np.zeros(levels_.shape + (3,)); out[:] = b; out[levels_ == 1] = (b + tint) / 2; out[levels_ == 2] = tint
    return out.clip(0, 255).astype(np.uint8)


# ---------------------------------------------------------------- numbers

class Glyphs:
    """The menus' name glyphs (tools/cook_menu_names.py): the full pages, and each glyph a quarter
    the size (box filtered), for the names drawn small."""
    def __init__(self, include, textures):
        places = [tuple(map(int, p)) for p in re.findall(r'\{(\d+),(\d+),(\d+)\}', (include / 'menu_name_glyph_atlas.hpp').read_text().split('UiGlyphPlaces[]=')[1])]
        rows = re.findall(r'\{"name_glyph_([0-9a-f]{4})(_s)?","[^"]+",([\d.]+)f,([\d.]+)f', (include / 'menu_name_glyphs.hpp').read_text(encoding='utf-8'))
        self.full = [(int(c, 16), float(w), float(h)) for c, s, w, h in rows if not s]
        self.places = places
        self.page0 = Image.open(textures / 'ui_name_glyphs_0.png').convert('RGBA')
        pages = {0: self.page0}; self.small = {}
        for i, (code, w, h) in enumerate(self.full):
            pg, x, y = places[i]
            if pg not in pages:
                pages[pg] = Image.open(textures / f'ui_name_glyphs_{pg}.png').convert('RGBA')
            glyph = pages[pg].crop((x, y, x + int(w), y + int(h)))
            self.small[code] = glyph.resize((max(1, -(-int(w) // 4)), max(1, -(-int(h) // 4))), Image.Resampling.BOX)

    def big(self, ch):
        for i, (code, w, h) in enumerate(self.full):
            if code == ord(ch):
                pg, x, y = self.places[i]
                if pg != 0:
                    raise KeyError(ch)
                return self.page0.crop((x, y, x + int(w), y + int(h)))
        raise KeyError(ch)


def _rank_png(n):
    im = Image.open(os.path.join(TEX, f'hud_rank_{n}.png')).convert('RGBA'); return im.crop(im.getbbox())


def wii_texture(n):
    """The original numeral's own fill as a texture (its diagonal bands and dark tones), with the
    black edge, white rim and grey block filled in from neighbouring fill colours."""
    arr = np.asarray(_rank_png(n)).astype(float); H, W = arr.shape[:2]
    lum = arr[:, :, :3] @ np.array([.3, .59, .11])
    ok = (arr[:, :, 3] > 230) & (lum > 72)
    ok[:, int(W * (.5 if n < 10 else .72)):] = False
    ys, xs = np.nonzero(ok); y0, y1, x0, x1 = ys.min(), ys.max(), xs.min(), xs.max()
    solid = Image.fromarray(((arr[:, :, 3] > 200) * 255).astype(np.uint8)).filter(ImageFilter.MinFilter(13))
    ok &= np.asarray(solid) > 0
    okc = ok[y0:y1 + 1, x0:x1 + 1].astype(float); rgb = arr[y0:y1 + 1, x0:x1 + 1, :3] * okc[:, :, None]
    def blur(a, r):
        k = np.exp(-0.5 * (np.arange(-3 * r, 3 * r + 1) / r) ** 2); k /= k.sum()
        a = np.apply_along_axis(lambda v: np.convolve(np.pad(v, 3 * r, mode='edge'), k, mode='valid'), 0, a)
        return np.apply_along_axis(lambda v: np.convolve(np.pad(v, 3 * r, mode='edge'), k, mode='valid'), 1, a)
    num = np.stack([blur(rgb[:, :, c], 3) for c in range(3)], 2); den = blur(okc, 3)[:, :, None]
    filled = np.where(okc[:, :, None] > 0, rgb, num / np.maximum(den, 1e-3))
    for r in (8, 16):
        num = np.stack([blur(filled[:, :, c] * (den[:, :, 0] > .05), r) for c in range(3)], 2); d2 = blur((den[:, :, 0] > .05).astype(float), r)[:, :, None]
        filled = np.where(den > .05, filled, num / np.maximum(d2, 1e-3)); den = np.maximum(den, d2)
    return Image.fromarray(filled.clip(0, 255).astype(np.uint8), 'RGB')


def row_profile(img, numeral_cols=.5, erode=3, rows=64):
    """Top-to-bottom colour profile of a numeral face: per row, the median of its opaque, non-edge pixels."""
    arr = np.asarray(img.convert('RGBA')).astype(float); H, W = arr.shape[:2]
    lum = arr[:, :, :3] @ np.array([.3, .59, .11])
    solid = np.asarray(Image.fromarray(((arr[:, :, 3] > 200) * 255).astype(np.uint8)).filter(ImageFilter.MinFilter(2 * erode + 1))) > 0
    ok = solid & (lum > 72); ok[:, int(W * numeral_cols):] = False
    ys = [y for y in range(H) if ok[y].sum() >= 2]
    top, bot = ys[0], ys[-1]; out = []
    for i in range(rows):
        y = int(round(top + (bot - top) * i / (rows - 1))); px = arr[y][ok[y]][:, :3]
        out.append(np.median(px, 0) if len(px) else (out[-1] if out else np.array([255., 255, 255])))
    return np.array(out)


_family = None
def family_profile():
    """One profile for 4th-12th: the per-row median of the nine Wii numerals' own profiles."""
    global _family
    if _family is None:
        _family = np.median(np.array([row_profile(_rank_png(n), .5 if n < 10 else .72) for n in range(4, 13)]), 0)
    return _family


def numeral_full(glyphs, n, H=96):
    """The redrawn numeral at H px (before shrinking)."""
    gl = [glyphs.big(c) for c in str(n)]
    gl = [ImageChops.multiply(g.convert('L'), g.split()[3]) for g in gl]     # the white fill only
    gl = [g.crop(g.getbbox()) for g in gl]
    gl = [g.resize((max(1, round(g.width * H / g.height)), H), Image.LANCZOS) for g in gl]
    gap = -int(H * .06); w = sum(g.width for g in gl) + gap * (len(gl) - 1)
    pad = int(H * .5)
    def shaped(sel):
        mk = Image.new('L', (w + pad, H + 16)); x = pad // 4
        for k, g in enumerate(gl):
            if k in sel:
                mk.paste(g, (x, 8), g)
            x += g.width + gap
        sh = .22   # italic like the HUD numerals: shear right by 0.22 of the height
        mk = mk.transform(mk.size, Image.AFFINE, (1, sh, -sh * mk.height, 0, 1, 0), resample=Image.BICUBIC)
        return mk.point(lambda v: 255 if v > 100 else 0).filter(ImageFilter.MaxFilter(5))  # bold like the HUD numerals
    mask = shaped(range(len(gl)))
    # the joint of a two-digit number: the second digit's own black edge where it lies over the first
    joint = None
    if len(gl) == 2:
        m2 = shaped([1]); joint = ImageChops.multiply(ImageChops.subtract(m2.filter(ImageFilter.MaxFilter(13)), m2), mask)
    Wm, Hm = mask.size
    if n >= 4:
        prof = family_profile(); bb = mask.getbbox()
        col = np.zeros((Hm, Wm, 4), np.uint8)
        for y in range(Hm):
            t = (y - bb[1]) / max(1, bb[3] - 1 - bb[1]); k = int(round(min(1, max(0, t)) * (len(prof) - 1)))
            col[y, :, :3] = prof[k].clip(0, 255)
        col[:, :, 3] = np.asarray(mask); fill = Image.fromarray(col)
    else:
        bb = mask.getbbox(); tex = wii_texture(n).resize((bb[2] - bb[0], bb[3] - bb[1]), Image.BICUBIC)
        canvas = Image.new('RGB', mask.size, (0, 0, 0)); canvas.paste(tex, bb[:2])
        fill = canvas.convert('RGBA'); fill.putalpha(mask)
    rim_m = ImageChops.subtract(mask, mask.filter(ImageFilter.MinFilter(7)))
    rim = Image.new('RGBA', mask.size, (255, 255, 240, 0)); rim.putalpha(rim_m.point(lambda v: v * (90 if n < 4 else 60) // 255))
    e = max(3, int(H * .055)) * 2 + 1
    edge = Image.new('RGBA', mask.size, (8, 8, 8, 0)); edge.putalpha(mask.filter(ImageFilter.MaxFilter(e)))
    blk = mask.filter(ImageFilter.MaxFilter(e))
    drop = Image.new('RGBA', mask.size, (57, 57, 57, 0)); drop.putalpha(blk)
    dedge = Image.new('RGBA', mask.size, (0, 0, 0, 0)); dedge.putalpha(blk.filter(ImageFilter.MaxFilter(5)))
    dx, dy = int(H * .03), int(H * .08)
    out = Image.new('RGBA', (Wm + dx + 4, Hm + dy + 4))
    for k in range(0, dy + 1, 2):
        out.alpha_composite(dedge, (int(dx * k / dy), k))
    for k in range(0, dy + 1, 2):
        out.alpha_composite(drop, (int(dx * k / dy), k))
    out.alpha_composite(edge, (0, 0)); out.alpha_composite(fill, (0, 0)); out.alpha_composite(rim, (0, 0))
    if joint is not None:
        j = Image.new('RGBA', mask.size, (0, 0, 0, 0)); j.putalpha(joint); out.alpha_composite(j, (0, 0))
    return out.crop(out.getbbox())


def outline1(im):
    pad = Image.new('RGBA', (im.width + 2, im.height + 2)); pad.alpha_composite(im, (1, 1))
    a = pad.split()[3].point(lambda v: 255 if v > 60 else 0).filter(ImageFilter.MaxFilter(3))
    o = Image.new('RGBA', pad.size, (0, 0, 0, 0)); o.putalpha(a); o.alpha_composite(pad); return o


def numeral(glyphs, n, height=15, maxw=20):
    img = numeral_full(glyphs, n)
    sm = img.resize((max(1, round(img.width * height / img.height)), height), Image.LANCZOS)
    if sm.width > maxw:
        sm = sm.resize((maxw, height), Image.LANCZOS)
    return outline1(sm)


def nine(src, w):
    """Row art nine-sliced to w px: 5 px caps, the middle stretched (as the runtime's three quads)."""
    c = 5; out = Image.new('RGBA', (w, src.height))
    out.paste(src.crop((0, 0, c, src.height)), (0, 0))
    out.paste(src.crop((c, 0, src.width - c, src.height)).resize((w - 2 * c, src.height), Image.BILINEAR), (c, 0))
    out.paste(src.crop((src.width - c, 0, src.width, src.height)), (w - c, 0))
    return out


def vis(img):
    """Visible bounds (alpha > 40): (left, top, right, bottom) inclusive."""
    b = img.split()[3].point(lambda v: 255 if v > 40 else 0).getbbox()
    return (b[0], b[1], b[2] - 1, b[3] - 1) if b else (0, 0, img.width - 1, img.height - 1)


# The row art (RaceMapUi.rowOther/rowPlayer, 58x19): rows 1..14 the framed box, 3..13 its inside.
FRAME_TOP, FRAME_BOT, IN_TOP, IN_BOT = 1, 14, 3, 13
CENTRE = (IN_TOP + IN_BOT) / 2


def number_box(glyphs, row_other, n):
    """Slot n's fixed box: the row frame nine-sliced to 21 px, the numeral centred on the frame."""
    box = nine(row_other, 21); g = numeral(glyphs, n)
    l, t, r, b = vis(g)
    box.alpha_composite(g, (int(math.floor(10.5 - (l + r) / 2 + .5)), int(math.floor((FRAME_TOP + FRAME_BOT) / 2 - (t + b) / 2 + .5))))
    return box


# ---------------------------------------------------------------- the heading pointer

def _shrink(poly, px):
    cx = sum(p[0] for p in poly) / len(poly); cy = sum(p[1] for p in poly) / len(poly)
    out = []
    for u, v in poly:
        dx, dy = u - cx, v - cy; L = math.hypot(dx, dy) or 1; out.append((u - dx / L * px, v - dy / L * px))
    return out


def pointer(r_tip=15.5, r_base=9.0, half=5.0):
    """The player's heading pointer pointing up, 48x48 centred on the face: a yellow Wii-style
    arrowhead (black edge, lit half, soft shadow) on a ring around the face. The runtime turns it."""
    S = 4; size = 48; c = size / 2; hx, hy = 0.0, -1.0
    im = Image.new('RGBA', (size * S, size * S)); d = ImageDraw.Draw(im)
    px, py = -hy, hx
    tip = (c + hx * r_tip, c + hy * r_tip)
    b1 = (c + hx * r_base + px * half, c + hy * r_base + py * half); b2 = (c + hx * r_base - px * half, c + hy * r_base - py * half)
    notch = (c + hx * (r_base + 2.2), c + hy * (r_base + 2.2))
    poly = [tip, b1, notch, b2]
    d.polygon([(u * S + 3, v * S + 4) for u, v in poly], fill=(0, 0, 0, 120))           # drop shadow
    d.polygon([(u * S, v * S) for u, v in poly], fill=(0, 0, 0, 255))
    d.polygon([(u * S, v * S) for u, v in _shrink(poly, 1.3)], fill=(255, 210, 0, 255))
    d.polygon([(u * S, v * S) for u, v in _shrink([tip, b1, notch], 1.6)], fill=(255, 246, 150, 255))  # lit half
    return im.resize((size, size), Image.LANCZOS)


# ---------------------------------------------------------------- names

def name_font(glyphs):
    """The quarter-size name glyphs as coverage (the glyph's white fill: L x A): code -> (w, h, bytes)."""
    out = {}
    for code, im in glyphs.small.items():
        a = ImageChops.multiply(im.split()[3], im.convert('L'))
        out[code] = (im.width, im.height, a.tobytes())
    return out


def text_coverage(font, text):
    """A name as the runtime composes it (coverage, white over a 1 px black outline): returns (L, A) images."""
    ims = [font.get(ord(c)) for c in text]
    wsum = sum(g[0] - 1 for g in ims if g) + 2
    cov = np.zeros((14, max(1, wsum)), float); x = 0
    for g in ims:
        if not g:
            x += 3; continue
        w, h, data = g; a = np.frombuffer(data, np.uint8).reshape(h, w) / 255.
        reg = cov[:h, x:x + w]; reg[:] = reg + a[:, :reg.shape[1]] * (1 - reg); x += w - 1
    dil = np.asarray(Image.fromarray((cov * 255).astype(np.uint8)).filter(ImageFilter.MaxFilter(3))) / 255.
    A = cov + dil * (1 - cov); L = np.where(A > 0, cov / np.maximum(A, 1e-6), 0)
    return L, A
