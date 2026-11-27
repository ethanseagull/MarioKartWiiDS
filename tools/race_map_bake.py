"""Offline top-down render of a cooked course for the bottom-screen map.

Reads exactly what the 3DS draws: the course's GMDL parts (models/course_NN.list,
build/course-materials overrides first), their cooked textures (the PNG each
.t3x was encoded from) and every submesh's material as src/renderer.cpp
Renderer::drawNow sets it up: the flag modes, the menuReflection stage sets,
the compiled colour programs (GMDL TEV1), the split programs' later passes
(lighting bit 31), the light-texture combiners (renderer_retail.cpp
retailCombiners, channel 2), alpha equations, alpha tests and blend factors,
run through a model of the PICA texture combiners (tev_run). Material
animations are taken at frame 0 (texture patterns, UVM1 matrices) or at their
mean over the loop (CLR0 colours: a pulse is drawn at its average). Inputs
the console takes from the camera are held at their mean: EnvCamera
reflections and the statues' third map at the texture's; camera-projected
water highlights (a glare spot fixed on the screen) at their value away from
the spot (glare_free); the EGG light textures at black. No fog: the map shows the
course's colour. The course's water objects (WATER_OBJECTS: Peach Beach's
tide, the Venice water) are drawn too; other course objects are not.
Nothing here runs on the console.

The view is orthographic, looking straight down: +x to the right and -z up,
the retail minimap's orientation (Map2DRenderer, posLD below posRU). Depth is
height, so the highest surface wins, as seen from above.

Water (the projected-water materials, the flag-29 water grade, and any blended
surface lying on the course's KCL water: moving water and the water variants
1/3 of the solid and non-solid out-of-bounds planes) keeps its translucency
over the geometry beneath it; where nothing lies beneath it blends over the
course's sky colour (sky_colour: what the race camera sees through it), not
the empty surround.

Roofs, tunnels and banners over the road are removed with the course's own
collision: a fragment more than CLEARANCE units above the highest drivable
road surface beneath it (KCL road types) is discarded, so the road shows
through, as on Mario Kart 7's maps. Road under a KCL water plane is not
drivable (a kart falls out at the water) and does not count, so the sea over a
modelled sea floor stays. This reads course.kcl as data; the game itself never
queries KCL for presentation.

The outermost band of the map fades into the surround's edge colour (exact
over the last BORDER_EXACT texels), which the console also draws past the
map's edges (GPU_CLAMP_TO_BORDER with RaceMapCourse::border).
"""
import math
import struct
from pathlib import Path

from gx_material import GMDL_SUBMESH_BYTES
import numpy as np
from PIL import Image

ROOT = Path(__file__).resolve().parents[1]
SD = ROOT / 'sd-card/3ds/mkw-3ds'
OVERLAY = ROOT / 'build/course-materials'

# KCL collision types (attribute & 0x1f) that are road a kart drives on.
# Off-road (0x02-0x04), walls and triggers do not cut geometry above them.
ROAD_TYPES = {0x00, 0x01, 0x05, 0x06, 0x07, 0x08, 0x09, 0x13, 0x15, 0x16, 0x17, 0x1D}
# Every ground type, for the road-surface statistics (area / route length).
FLOOR_TYPES = ROAD_TYPES | {0x02, 0x03, 0x04, 0x0A, 0x0B}
# KCL water: moving water (0x0B), and the solid (0x0A) and non-solid (0x10)
# out-of-bounds planes with variant 1 or 3 (attribute bits 5-7; Kinoko
# KartCollide: those fall variants select the water height).
WATER_TYPES = {0x0A, 0x10}
WATER_VARIANTS = {1, 3}
MOVING_WATER = 0x0B
CLEARANCE = 700.0
# A model fragment this close to a KCL floor or water surface under it is
# ground (or sea), never a roof: the roof cut keeps it. Up to FLOOR_LAYERS
# such surfaces are kept per pixel.
GROUND_BAND = 300.0
FLOOR_LAYERS = 4
# A blended fragment this close to the KCL water level beneath it is water.
WATER_BAND = 400.0
# Map edge, in texels of the final map: the last BORDER_EXACT are the
# surround's edge colour, the BORDER_FADE inside them fade into it.
BORDER_EXACT = 8
BORDER_FADE = 24

# GMDL submesh, padded to the v5 record (src/renderer.hpp Submesh).
SUBMESH = struct.Struct('<IIiIiI4fIIII3I6f')


def course_path(relative):
    for base in (OVERLAY, SD):
        p = base / relative
        if p.is_file():
            return p
    raise FileNotFoundError(relative)


class Part:
    """One GMDL v3/v4/v5 part as numpy arrays, with the chunks the material
    path reads (src/renderer.cpp Model::load): MAN1 and UVM1 at frame 0, TEV1, CUL1."""

    def __init__(self, path):
        b = path.read_bytes()
        magic, version, nv, ni, ns, nt = struct.unpack_from('<4s5I', b)
        if magic != b'GMDL' or version not in GMDL_SUBMESH_BYTES:
            raise ValueError(f'Not a GMDL v3/v4/v5 model: {path}')
        self.name = path.name
        record = GMDL_SUBMESH_BYTES[version]
        self.submeshes = []
        for i in range(ns):
            raw = b[24 + i * record:24 + (i + 1) * record]
            self.submeshes.append(SUBMESH.unpack(raw + bytes(SUBMESH.size - len(raw))))
        at = 24 + ns * record
        self.textures = [b[at + i * 64:at + (i + 1) * 64].split(b'\0')[0].decode() for i in range(nt)]
        at += nt * 64
        v = np.frombuffer(b, dtype=np.dtype([('p', '<f4', 3), ('n', '<f4', 3), ('uv', '<f4', 2),
                                             ('uv1', '<f4', 2), ('c', 'u1', 4)]), count=nv, offset=at)
        self.position = v['p'].astype(np.float64)
        self.normal = v['n'].astype(np.float64)
        self.uv = v['uv'].astype(np.float64)
        self.uv1 = v['uv1'].astype(np.float64)
        self.color = v['c'].astype(np.float64) / 255.0
        at += nv * 44
        self.indices = np.frombuffer(b, dtype='<u2', count=ni, offset=at).astype(np.int64)
        at += ni * 2
        self.programs, self.cull, self.overrides, self.uv_rows = {}, {}, {}, {}
        if version == 5 and any(s[14] == 11 for s in self.submeshes):
            count = struct.unpack_from('<4sI', b, at)[1]
            at += 8
            head = struct.Struct('<IIIi6h11fI')
            for _ in range(count):
                w, h, *rest = head.unpack_from(b, at)
                at += head.size + sum(max(1, w >> k) * max(1, h >> k) * 3 for k in range(rest[-1]))
        while at < len(b):
            tag, n = struct.unpack_from('<4sI', b, at)
            at += 8
            if tag == b'MAN1':
                for _ in range(n):
                    sub, word, frames, loop = struct.unpack_from('<4I', b, at)
                    at += 16
                    kind, flags = word & 255, word >> 8
                    o = self.overrides.setdefault(sub, {})
                    if kind == 2:
                        p, q = struct.unpack_from('<hh', b, at)
                        if p >= 0:
                            o['texture'] = p
                        if q >= 0:
                            o['secondary'] = q
                        at += 4 * frames
                    else:
                        # A colour track at its mean over the loop (a pulse or a
                        # fade is drawn at its average, not at frame 0).
                        words = np.frombuffer(b, '<u4', count=3 * frames, offset=at).reshape(frames, 3)
                        mean = (words[:, :, None] >> np.array([0, 8, 16, 24]) & 255).mean(axis=0)
                        packed = [int(sum(int(round(v)) << (8 * k) for k, v in enumerate(row))) for row in mean]
                        if flags & 1:
                            o['constant'] = packed[0]
                        if flags & 2:
                            o['tint'] = mean[1] / 255.0
                        if flags & 8:
                            o['add'] = packed[2]
                        at += 12 * frames
            elif tag == b'TEV1':
                for _ in range(n):
                    sub, stages, mask = struct.unpack_from('<3I', b, at)
                    at += 12
                    self.programs[sub] = (mask, [struct.unpack_from('<2I', b, at + 8 * k) for k in range(stages)])
                    at += 8 * stages
            elif tag == b'CUL1':
                for k in range(n):
                    sub, mode = struct.unpack_from('<2I', b, at + 8 * k)
                    self.cull[sub] = mode
                at += 8 * n
            elif tag == b'SMP1':
                at += 24 * n
            elif tag == b'UVM1':
                for _ in range(n):
                    sub, frames, loop = struct.unpack_from('<3I', b, at)
                    at += 12
                    self.uv_rows[sub] = np.array(struct.unpack_from('<12f', b, at)).reshape(4, 3)
                    at += 48 * frames
            else:
                raise ValueError(f'{path}: unknown GMDL chunk {tag!r}')


    def placed(self, record, height):
        """A copy at a KMP GOBJ placement (EGG makeRT: Rz*Ry*Rx, then the
        record's scale), its origin raised to the record's y + height."""
        rx, ry, rz = np.radians(record['rotation'])
        sx, sy, sz = np.sin([rx, ry, rz])
        cx, cy, cz = np.cos([rx, ry, rz])
        r = np.array([[cy * cz, sx * sy * cz - cx * sz, cx * cz * sy + sx * sz],
                      [cy * sz, sx * sy * sz + cx * cz, cx * sz * sy - sx * cz],
                      [-sy, sx * cy, cx * cy]]) * np.array(record['scale'])[None, :]
        out = Part.__new__(Part)
        out.__dict__.update(self.__dict__)
        x, y, z = record['position']
        out.position = self.position @ r.T + np.array([x, y + height, z])
        out.normal = self.normal @ np.linalg.inv(r)
        return out


# Course objects that are the course's water (src/course_object_kinds.cpp,
# drawn at race start): GOBJ id -> resource, (model, height over the record's
# y). Psea (0x2): ObjectPsea's 6.75*96 offset, drawTide's sand +48 and
# sparkles +1. Venice water (0x4): ObjectVeniceWater::layerHeight at time 0.
WATER_OBJECTS = {
    0x2: ('psea', (('psea1sand', 696.0), ('psea2dark', 648.0), ('psea3nami', 648.0), ('psea4tex', 648.0), ('psea5spc', 649.0))),
    0x4: ('venice_nami', (('venice_nami', 0.0), ('venice_dark', -260.0), ('venice_sand', 10.0), ('venice_tex', 10.0))),
}


def water_objects(archive_key, model_list):
    """The course's water objects as placed Parts, and their names."""
    from import_courses import kmp_database
    if not model_list.startswith('course_'):
        return [], []
    index = int(model_list.split('_')[1])
    kmp = kmp_database(ROOT / 'assets/courses' / archive_key / 'course.kmp')
    parts, names = [], []
    for record in kmp.get('GOBJ', {}).get('records', []):
        if record['id'] not in WATER_OBJECTS:
            continue
        resource, layers = WATER_OBJECTS[record['id']]
        for model, height in layers:
            listing = SD / f'models/obj{index}_{resource}__{model}.list'
            if not listing.is_file():
                continue
            for name in listing.read_text().split():
                parts.append(Part(course_path(f'models/{name}.gmdl')).placed(record, height))
                names.append(name)
    return parts, names


class Texture:
    """A cooked course texture with box-filtered mip levels (RGBA float)."""

    cache = {}
    missing = set()

    @classmethod
    def get(cls, name):
        if name not in cls.cache:
            try:
                cls.cache[name] = cls(name)
            except FileNotFoundError:
                cls.cache[name] = None
        if cls.cache[name] is None:
            cls.missing.add(name)
        return cls.cache[name]

    def __init__(self, name):
        image = Image.open(course_path(f'textures/{name}.png')).convert('RGBA')
        level = np.asarray(image, dtype=np.float32) / 255.0
        self.levels = [level]
        while min(level.shape[0], level.shape[1]) > 1:
            h, w = level.shape[0] // 2 * 2, level.shape[1] // 2 * 2
            level = level[:h, :w].reshape(h // 2, 2, w // 2, 2, 4).mean(axis=(1, 3))
            self.levels.append(level)
        self.mean = self.levels[0].reshape(-1, 4).mean(axis=0)

    def sample(self, u, v, wrap_s, wrap_t, lod):
        """Bilinear sample at GMDL UVs (V stored as 1-v), GX wrap modes 0 clamp, 1 repeat, 2 mirror."""
        tex = self.levels[min(max(int(lod), 0), len(self.levels) - 1)]
        h, w = tex.shape[:2]
        x = u * w - 0.5
        y = (1.0 - v) * h - 0.5

        def wrap(c, n, mode):
            if mode == 1:
                return np.mod(c, n)
            if mode == 2:
                c = np.mod(c, 2 * n)
                return np.where(c < n, c, 2 * n - 1 - c)
            return np.clip(c, 0, n - 1)
        x0 = np.floor(x)
        y0 = np.floor(y)
        fx = (x - x0)[:, None]
        fy = (y - y0)[:, None]
        x0 = x0.astype(np.int64)
        y0 = y0.astype(np.int64)
        xa, xb = wrap(x0, w, wrap_s), wrap(x0 + 1, w, wrap_s)
        ya, yb = wrap(y0, h, wrap_t), wrap(y0 + 1, h, wrap_t)
        return ((tex[ya, xa] * (1 - fx) + tex[ya, xb] * fx) * (1 - fy) +
                (tex[yb, xa] * (1 - fx) + tex[yb, xb] * fx) * fy)


def kcl_triangles(path, attributes=False):
    """Course collision prisms as (A, B, C) triangles plus their type (and, with
    attributes=True, the whole attribute word)."""
    b = path.read_bytes()
    pos_off, nrm_off, prism_off, block_off = struct.unpack_from('>4I', b, 0)
    positions = np.frombuffer(b, '>f4', count=(nrm_off - pos_off) // 4, offset=pos_off).reshape(-1, 3).astype(np.float64)
    normals = np.frombuffer(b, '>f4', count=(prism_off + 0x10 - nrm_off) // 4, offset=nrm_off)
    normals = normals[:len(normals) // 3 * 3].reshape(-1, 3).astype(np.float64)
    count = (block_off - prism_off - 0x10) // 0x10
    prisms = np.frombuffer(b, np.dtype([('h', '>f4'), ('p', '>u2'), ('f', '>u2'), ('e1', '>u2'), ('e2', '>u2'),
                                        ('e3', '>u2'), ('a', '>u2')]), count=count, offset=prism_off + 0x10)
    a = positions[prisms['p']]
    face = normals[prisms['f']]
    cross_a = np.cross(normals[prisms['e1']], face)
    cross_b = np.cross(normals[prisms['e2']], face)
    e3 = normals[prisms['e3']]
    h = prisms['h'].astype(np.float64)[:, None]
    with np.errstate(divide='ignore', invalid='ignore'):
        bpt = a + cross_b * (h / np.sum(cross_b * e3, axis=1, keepdims=True))
        cpt = a + cross_a * (h / np.sum(cross_a * e3, axis=1, keepdims=True))
    ok = np.isfinite(bpt).all(axis=1) & np.isfinite(cpt).all(axis=1)
    out = (a[ok], bpt[ok], cpt[ok], (prisms['a'][ok] & 0x1f).astype(np.int64), face[ok])
    return out + (prisms['a'][ok].astype(np.int64),) if attributes else out


class Frame:
    """World XZ rectangle -> pixel grid (x right, -z up)."""

    def __init__(self, x0, z0, x1, z1, width, height):
        self.x0, self.z0, self.x1, self.z1 = x0, z0, x1, z1
        self.width, self.height = width, height
        self.sx = width / (x1 - x0)
        self.sz = height / (z1 - z0)

    def to_pixels(self, p):
        return np.stack([(p[..., 0] - self.x0) * self.sx, (p[..., 2] - self.z0) * self.sz], axis=-1)


def raster(tri_xy, width, height):
    """Pixel centres covered by one 2D triangle: (iy, ix, w0, w1, w2) or None."""
    (ax, ay), (bx, by), (cx, cy) = tri_xy
    area = (bx - ax) * (cy - ay) - (by - ay) * (cx - ax)
    if abs(area) < 1e-12:
        return None
    x0 = max(int(math.floor(min(ax, bx, cx))), 0)
    x1 = min(int(math.ceil(max(ax, bx, cx))), width - 1)
    y0 = max(int(math.floor(min(ay, by, cy))), 0)
    y1 = min(int(math.ceil(max(ay, by, cy))), height - 1)
    if x0 > x1 or y0 > y1:
        return None
    ys, xs = np.mgrid[y0:y1 + 1, x0:x1 + 1]
    px = xs.ravel() + 0.5
    py = ys.ravel() + 0.5
    w0 = ((bx - px) * (cy - py) - (by - py) * (cx - px)) / area
    w1 = ((cx - px) * (ay - py) - (cy - py) * (ax - px)) / area
    w2 = 1.0 - w0 - w1
    inside = (w0 >= -1e-9) & (w1 >= -1e-9) & (w2 >= -1e-9)
    if not inside.any():
        return None
    return ys.ravel()[inside], xs.ravel()[inside], w0[inside], w1[inside], w2[inside]


def course_lighting(archive):
    """Lights (flags, space, direction, colour) and ambient colours of one course
    from include/mkw/course_lighting.hpp (tools/cook_course_lighting.py)."""
    import re
    text = (ROOT / 'include/mkw/course_lighting.hpp').read_text()
    start = text.index('{"' + archive + '"')
    body = text[start:text.index('\n', start)]
    lights = [(int(f), int(s), (float(x), float(y), float(z)), int(c, 16))
              for f, s, t, x, y, z, c, d in re.findall(
                  r'\{(\d+),(\d+),(\d+),\{([-\d.e]+)f,([-\d.e]+)f,([-\d.e]+)f\},0x([0-9a-f]+)u,0x([0-9a-f]+)u\}', body)][:16]
    ambient_block = body[body.index('}},{', body.index('{{')) + 4:]
    ambient = [int(v, 16) for v in re.findall(r'0x([0-9a-f]+)u', ambient_block)[:16]]
    return lights, ambient


# The top-down camera: view rows right (+x), up (-z) and back toward the eye (+y).
VIEW = np.array([[1.0, 0.0, 0.0], [0.0, 0.0, -1.0], [0.0, 1.0, 0.0]])


def light_toward(light):
    """L (toward the light) in view space, as retailLightViewDirection."""
    flags, space, direction, colour = light
    d = np.array(direction)
    if space == 2:
        v = d
    elif space == 3:
        v = np.array([0.0, 0.0, -1.0])
    elif space == 4:
        v = np.array([0.0, 0.0, 1.0])
    elif space == 1:
        forward = -VIEW[2]
        half = -d / max(np.linalg.norm(d), 1e-9) + -forward
        v = -(VIEW @ (half / max(np.linalg.norm(half), 1e-9)))
    else:
        v = VIEW @ d
    return -v


def unit(colour, i):
    return ((colour >> (24 - 8 * i)) & 255) / 255.0


def primary_colour(sub, part_colors, normals, lights, ambient):
    """GX colour channel 0: vertex colour times the retail channel (or as authored)."""
    lighting, amb = sub[11], sub[12]
    if not (lighting & 1):
        return part_colors
    channel = (lighting >> 2) & 3
    if channel == 0:
        return part_colors
    light_set = (lighting >> 4) & 7
    alpha_lit = bool(lighting & (1 << 7))
    amb_rgba = [math.floor(0.5 + unit(ambient[0], i) * 255 * unit(amb, i)) / 255.0 for i in range(4)]
    if not alpha_lit:
        amb_rgba[3] = 1.0
    light = lights[light_set]
    on = (light[0] & 0x41) == 0x41
    diffuse = [unit(light[3], i) if on and (light[0] & 0x200) else 0.0 for i in range(3)]
    diffuse.append(unit(light[3], 3) if alpha_lit and on and (light[0] & 0x400) else 0.0)
    toward = light_toward(light)
    fn = (lighting >> 8) & 3
    a, b, c = [(0, 1, 0), (1, 0, -2), (1, 0, 0)][fn if fn < 3 else 2]
    nv = normals @ VIEW.T
    nv /= np.maximum(np.linalg.norm(nv, axis=1, keepdims=True), 1e-6)
    df = np.maximum(nv @ toward * a + b, c)
    scale = np.minimum(np.array(amb_rgba)[None, :] + np.array(diffuse)[None, :] * df[:, None], 1.0)
    return part_colors * scale


# ---------------------------------------------------------------------------
# PICA texture combiners (citro3d C3D_TexEnv) and the material setup of
# src/renderer.cpp Renderer::drawNow / renderer_retail.cpp retailCombiners.

# GPU_TEVSRC
P, T0, T1, T2, PB, K, PR = 0, 3, 4, 5, 13, 14, 15
# GPU_TEVOP_RGB / GPU_TEVOP_A
C, IC, A, IA = 0, 1, 2, 3
A_A, A_R = 0, 2
# GPU_COMBINEFUNC
REPLACE, MODULATE, ADD, ADD_SIGNED, INTERPOLATE, SUBTRACT, MULTIPLY_ADD = 0, 1, 2, 3, 4, 5, 8
RGB, ALPHA, BOTH = 1, 2, 3
SCALE = (1.0, 2.0, 4.0)


class Env:
    """One C3D_TexEnv after C3D_TexEnvInit: PREVIOUS, REPLACE, colour 0xFFFFFFFF."""

    def __init__(self):
        self.rs, self.as_ = [PR, P, P], [PR, P, P]
        self.ro, self.ao = [0, 0, 0], [0, 0, 0]
        self.rf = self.af = REPLACE
        self.c = 0xFFFFFFFF
        self.rsc = self.asc = 0

    def src(self, mode, a, b=P, c=P):
        if mode & RGB:
            self.rs = [a, b, c]
        if mode & ALPHA:
            self.as_ = [a, b, c]

    def op_rgb(self, a, b=C, c=C):
        self.ro = [a, b, c]

    def op_alpha(self, a, b=A_A, c=A_A):
        self.ao = [a, b, c]

    def func(self, mode, f):
        if mode & RGB:
            self.rf = f
        if mode & ALPHA:
            self.af = f

    def scale(self, mode, s):
        if mode & RGB:
            self.rsc = s
        if mode & ALPHA:
            self.asc = s

    def passthrough(self, alpha):
        if alpha:
            return self.as_[0] == PR and self.ao[0] == A_A and self.af == REPLACE and self.asc == 0
        return self.rs[0] == PR and self.ro[0] == C and self.rf == REPLACE and self.rsc == 0


def scale_index(v):
    """drawNow's scales[]: GX scale field 3 (divide by 2 on the Wii) draws at 1."""
    return v if v < 3 else 0


def _combine(f, a, b, c):
    if f == REPLACE:
        return a
    if f == MODULATE:
        return a * b
    if f == ADD:
        return a + b
    if f == ADD_SIGNED:
        return a + b - 0.5
    if f == INTERPOLATE:
        return a * c + b * (1 - c)
    if f == SUBTRACT:
        return a - b
    if f in (6, 7):
        d = 4 * ((a - 0.5) * (b - 0.5)).sum(axis=-1, keepdims=True) if a.ndim == 2 else 4 * (a - 0.5) * (b - 0.5)
        return np.broadcast_to(d, a.shape)
    if f == MULTIPLY_ADD:
        return a * b + c
    return np.minimum(a + b, 1) * c


def tev_run(stages, buffer, inputs, n):
    """The six combiners on n fragments. inputs: source -> (rgb (n,3), alpha (n,)).
    buffer: (RGB mask, alpha mask) of C3D_TexEnvBufUpdate (bit k: stage k's
    output enters the buffer the stages after it read). Returns rgb, alpha."""
    zero3, zero = np.zeros((n, 3)), np.zeros(n)
    prev_c, prev_a = zero3, zero
    buf_c, buf_a = zero3, zero

    def fetch(s, e):
        if s == PR:
            return prev_c, prev_a
        if s == PB:
            return buf_c, buf_a
        if s == K:
            k = np.array([(e.c >> sh & 255) / 255.0 for sh in (0, 8, 16, 24)])
            return np.broadcast_to(k[:3], (n, 3)), np.broadcast_to(k[3], (n,))
        return inputs.get(s, (zero3, zero))

    for index, e in enumerate(stages):
        if index:
            if index - 1 < 4 and buffer[0] >> (index - 1) & 1:
                buf_c = prev_c
            if index - 1 < 4 and buffer[1] >> (index - 1) & 1:
                buf_a = prev_a
        if e.passthrough(False):
            out_c = prev_c
        else:
            args = []
            for s, o in zip(e.rs, e.ro):
                c, a = fetch(s, e)
                if o in (0, 1):
                    v = c
                elif o in (2, 3):
                    v = np.broadcast_to(a[:, None], (n, 3))
                else:
                    v = np.broadcast_to(c[:, (o >> 2) - 1][:, None], (n, 3))
                args.append(1 - v if o & 1 else v)
            out_c = np.clip(_combine(e.rf, *args) * SCALE[e.rsc], 0, 1)
        if e.passthrough(True):
            out_a = prev_a
        else:
            args = []
            for s, o in zip(e.as_, e.ao):
                c, a = fetch(s, e)
                v = a if o in (0, 1) else c[:, (o >> 1) - 1]
                args.append(1 - v if o & 1 else v)
            out_a = np.clip(_combine(e.af, *args) * SCALE[e.asc], 0, 1)
        prev_c, prev_a = out_c, out_a
    return prev_c, prev_a


# GMDL lighting word bits (include/mkw/retail_lighting.hpp, renderer.hpp)
RETAIL = 1
MENU = 1 << 21
PRIMARY_ENVIRONMENT = 1 << 22
FIXED_ENVIRONMENT = 1 << 29
AUTHORED_OPAQUE_ORDER = 1 << 30
PROGRAM_PASS = 1 << 31


def normal_environment(lighting, mode, constant):
    """retail_lighting.cpp normalEnvironment."""
    if lighting & PRIMARY_ENVIRONMENT or (lighting >> 2) & 3 == 2:
        return True
    if mode in (0, 3, 4, 7, 11, 12, 13, 14, 15, 19, 21, 22, 23, 24, 25):
        return False
    if mode in (10, 20):
        return not constant & 512
    return True


def material_stages(sub, program, override):
    """The combiners drawNow builds for one course submesh (no binder, no Star,
    no fog): returns (six Env, (RGB, alpha) buffer masks, unsupported note or None)."""
    first, count, tex, flags, sec, constant, *_ = sub
    anim, lighting, amb, add, refl, rconst, radd = sub[10:17]
    ruv = sub[17:23]
    if 'constant' in override:
        constant = override['constant']
    e = [Env() for _ in range(6)]
    e0, e1 = e[0], e[1]
    buf = [0, 0]
    note = None
    e0.src(BOTH, T0, P)
    e0.func(BOTH, MODULATE)
    e0.scale(RGB, scale_index((flags >> 4) & 3))
    water_grade = bool(flags & (1 << 29)) and not flags & 1
    if water_grade:
        e0.c = 0xff885a40
        e0.src(RGB, K)
        e0.func(RGB, REPLACE)
    mode = (flags >> 12) & 3
    bit25 = bool(flags & (1 << 25))
    metal = bit25 and mode == 3
    lerp = bit25 and mode in (1, 2)
    if bit25 and mode == 0:
        e0.c = constant
        e0.src(RGB, P, K, T0)
        e0.func(RGB, INTERPOLATE)
    if water_grade:
        pass
    elif lerp:
        e0.c = constant
        e0.src(RGB, T0, K, P)
        e0.func(RGB, INTERPOLATE)
        if mode == 1:
            e1.src(RGB, T0, P, PR)
            e1.func(RGB, MULTIPLY_ADD)
    elif mode == 1:
        e0.c = constant
        e0.src(RGB, P, K, T1)
        e0.func(RGB, INTERPOLATE)
        e0.src(ALPHA, T1, P)
        e0.scale(RGB, scale_index((flags >> 14) & 3))
        e1.src(RGB, PR, T0)
        e1.func(RGB, MODULATE)
        e1.scale(RGB, scale_index((flags >> 4) & 3))
    elif mode == 2:
        e1.c = constant
        e1.src(RGB, T1, K, PR)
        e1.op_rgb(C, A)
        e1.func(RGB, MULTIPLY_ADD)
    elif metal:
        e0.c = constant
        e0.src(RGB, T0, K)
        e0.func(RGB, MODULATE)
        e1.src(RGB, PR, T1)
        e1.func(RGB, ADD_SIGNED)
        e[2].src(RGB, PR, P)
        e[2].func(RGB, MODULATE)
    elif mode == 3:
        e0.c = constant
        e0.src(RGB, T1, K)
        e0.src(ALPHA, T1, P)
        e1.src(BOTH, T0, PR)
        e1.func(BOTH, ADD)
    special = (anim >> 16) & 255 if not flags & 1 else 0
    if special == 1:
        e0.src(RGB, T0)
        e0.func(RGB, REPLACE)
        e1.src(RGB, T1, T0, PR)
        e1.op_rgb(C, A)
        e1.func(RGB, MULTIPLY_ADD)
    if special == 2:
        e1.src(RGB, T1, PR, PR)
        e1.op_rgb(C, IC, C)
        e1.func(RGB, MULTIPLY_ADD)
    if special == 3:
        e0.src(RGB, T0, P)
        e0.func(RGB, ADD)
    if special == 4:
        e0.c = constant
        e0.src(RGB, T0, K, K)
        e0.op_rgb(C, C, A)
        e0.func(RGB, MULTIPLY_ADD)
        e0.scale(RGB, 0)
        e1.src(RGB, PR, T1, PR)
        e1.op_rgb(C, C, C)
        e1.func(RGB, MULTIPLY_ADD)
        e1.scale(RGB, 1)
    if special == 5:
        e0.c = constant
        e0.src(RGB, K, T0, P)
        e0.op_rgb(C, IC, C)
        e0.func(RGB, MULTIPLY_ADD)
        e0.scale(RGB, 0)
        e1.c = constant
        e1.src(RGB, T1, K, PR)
        e1.op_rgb(C, A, C)
        e1.func(RGB, MULTIPLY_ADD)
    add_override = override.get('add')
    if refl in (26, 27, 28, 29, 30, 31):
        note = f'menuReflection {refl} (item/award stage set) drawn with the generic stages'
    elif refl == 24:
        e0.c = rconst
        e0.src(RGB, P, K, T0)
        e0.func(RGB, INTERPOLATE)
        e1.src(RGB, PR, T0)
        e1.func(RGB, ADD)
    elif refl == 25:
        e0.c = constant
        e0.src(RGB, K)
        e0.func(RGB, REPLACE)
    elif refl == 20:
        e0.src(RGB, T0, P)
        e0.func(RGB, MODULATE)
        e0.scale(RGB, 1)
        e1.c = constant
        e1.src(RGB, T1, K, PR)
        e1.func(RGB, MULTIPLY_ADD)
    elif refl == 19:
        e0.src(RGB, T0, P)
        e0.func(RGB, MODULATE)
        e0.scale(RGB, 0)
        e1.src(RGB, P, PR, T1)
        e1.func(RGB, INTERPOLATE)
        e1.scale(RGB, 1)
    elif refl == 18:
        e0.c = constant
        e0.src(RGB, T0, K, P)
        e0.func(RGB, MULTIPLY_ADD)
        e0.scale(RGB, 0)
        e1.c = constant
        e1.src(RGB, T1, K, PR)
        e1.func(RGB, MULTIPLY_ADD)
        e[2].c = radd
        e[2].src(RGB, PR, K)
        e[2].func(RGB, ADD)
        e[3].src(ALPHA, T0, T1)
        e[3].func(ALPHA, ADD)
    elif refl == 17:
        e0.c = rconst
        e0.src(RGB, T1, K)
        e0.func(RGB, MODULATE)
        e0.scale(RGB, 0)
        e1.src(RGB, T0, P, PR)
        e1.func(RGB, MULTIPLY_ADD)
        e1.scale(RGB, 1)
    elif refl == 16 and radd & 1:
        e0.src(RGB, P, P)
        e0.op_rgb(C, IC)
        e0.func(RGB, MODULATE)
        e0.scale(RGB, 0)
        e1.src(RGB, T0, P, PR)
        e1.func(RGB, MULTIPLY_ADD)
        e1.scale(RGB, 2)
    elif refl == 16:
        e0.src(RGB, T0, P)
        e0.func(RGB, MODULATE)
        e0.scale(RGB, 2)
        e1.c = rconst
        e1.src(RGB, P, K, PR)
        e1.func(RGB, MULTIPLY_ADD)
        e1.scale(RGB, 1)
    elif refl == 15:
        e0.src(RGB, T0)
        e0.func(RGB, REPLACE)
        e0.scale(RGB, 0)
        e1.src(RGB, PR, T1)
        e1.op_rgb(C, A)
        e1.func(RGB, ADD)
        e1.scale(RGB, 1)
    elif refl == 14:
        e0.c = constant
        e0.src(RGB, P, K, T0)
        e0.func(RGB, INTERPOLATE)
        e0.scale(RGB, 0)
        e1.c = add_override if add_override is not None else radd
        raster_alpha = rconst & 1
        e1.src(RGB, P if raster_alpha else T1, K, PR)
        if raster_alpha:
            e1.op_rgb(A, C, C)
        e1.func(RGB, MULTIPLY_ADD)
    elif refl == 13:
        e0.scale(RGB, 1)
        e1.c = 0xffffffff
        e1.src(RGB, K, T1, PR)
        e1.func(RGB, INTERPOLATE)
    elif refl == 12:
        e0.c = constant
        e0.src(RGB, P, K, T0)
        e0.func(RGB, INTERPOLATE)
        e0.scale(RGB, 0)
        waterfall = rconst & 1
        if waterfall:
            e1.c = 0
            e1.src(RGB, PR, K)
            e1.func(RGB, ADD_SIGNED)
            e1.scale(RGB, 1)
        layer = e[2 if waterfall else 1]
        layer.src(RGB, T1, T0, PR)
        layer.op_rgb(C, A, C)
        layer.func(RGB, MULTIPLY_ADD)
    elif refl == 11:
        half_glow = bool(radd & 2)
        buf[0] = 2 if half_glow else 1
        e0.c = constant
        e0.src(RGB, K, T0, K)
        e0.op_rgb(IC, C, C)
        e0.func(RGB, MULTIPLY_ADD)
        e0.scale(RGB, 0)
        if radd & 4:
            e0.c = (constant & 0xffffff) | 0x80000000
            e0.src(RGB, K, K, T0)
            e0.op_rgb(A, C, C)
            e0.func(RGB, INTERPOLATE)
        if half_glow:
            e1.c = 0x80808080
            e1.src(RGB, PR, K)
            e1.func(RGB, MODULATE)
        wave = e[2 if half_glow else 1]
        wave.c = constant
        wave.src(RGB, P if radd & 1 else PR, K, T1)
        wave.func(RGB, INTERPOLATE)
        total = e[3 if half_glow else 2]
        total.src(RGB, PR, PB)
        total.func(RGB, ADD)
    elif refl == 10:
        buf[0] = 1
        e0.c = constant
        e0.src(RGB, T0, K)
        e0.func(RGB, MODULATE)
        e0.scale(RGB, 0)
        e1.c = (rconst & 255) * 0x010101
        e1.src(RGB, T1, K)
        e1.func(RGB, MODULATE)
        e[2].src(RGB, PB, PR)
        e[2].func(RGB, ADD_SIGNED if rconst & 256 else ADD)
        e[3].src(RGB, PR, P)
        e[3].func(RGB, MODULATE)
        e[4].c = radd
        e[4].src(RGB, PR, K)
        e[4].func(RGB, ADD)
        e[4].scale(RGB, scale_index((flags >> 4) & 3))
    elif refl == 22:
        e0.c = constant
        e0.src(RGB, T1, K)
        e0.func(RGB, MODULATE)
        e0.scale(RGB, 1)
        e1.c = radd
        e1.src(RGB, T0, K, PR)
        e1.func(RGB, MULTIPLY_ADD)
    elif refl == 23:
        e0.src(ALPHA, T0, P)
        e0.func(ALPHA, MODULATE)
        e1.src(RGB, T1, T0, PR)
        e1.op_rgb(C, C, A)
        e1.func(RGB, INTERPOLATE)
        e[2].src(RGB, PR, P)
        e[2].func(RGB, MODULATE)
        e[2].scale(RGB, 1)
    elif refl == 21:
        e0.c = rconst
        e0.src(RGB, K)
        e0.func(RGB, REPLACE)
        e0.scale(RGB, 0)
        e1.c = radd
        e1.src(RGB, K, PR, T0)
        e1.func(RGB, INTERPOLATE)
        e[2].c = 0
        e[2].src(RGB, PR, K)
        e[2].func(RGB, ADD_SIGNED if ruv[0] > 0 else ADD)
        e[2].scale(RGB, scale_index((flags >> 4) & 3))
    elif refl == 7:
        e0.c = constant
        e0.src(RGB, K)
        e0.func(RGB, REPLACE)
        e0.scale(RGB, 0)
        e1.c = add_override if add_override is not None else radd
        e1.src(RGB, K, PR, T0)
        e1.func(RGB, INTERPOLATE)
        e1.scale(RGB, scale_index((flags >> 14) & 3))
        e[2].src(RGB, PR, P)
        e[2].func(RGB, MODULATE)
        e[2].scale(RGB, scale_index((flags >> 4) & 3))
    elif refl == 8:
        e0.src(RGB, T0, P)
        e0.func(RGB, MODULATE)
        e0.scale(RGB, 0)
        e1.c = 0x80808080
        e1.src(RGB, PR, K)
        e1.func(RGB, MODULATE)
        e[2].c = constant
        e[2].src(RGB, T1, PR, K)
        e[2].func(RGB, INTERPOLATE)
        e[3].c = 0x80808080
        e[3].src(RGB, PR, K)
        e[3].func(RGB, MODULATE)
        e[4].c = add_override if add_override is not None else radd
        e[4].src(RGB, T2, K, PR)
        e[4].func(RGB, MULTIPLY_ADD)
        e[4].scale(RGB, 1)
    elif refl:
        e0.c = radd
        e0.src(RGB, T0, P, K)
        e0.func(RGB, MULTIPLY_ADD)
        if refl == 9:
            e0.scale(RGB, 0)
        e0.src(ALPHA, T0, K)
        e0.func(ALPHA, MODULATE)
        if refl == 4:
            e0.src(RGB, T0, K, P)
            e0.func(RGB, INTERPOLATE)
        elif refl == 6:
            e0.src(ALPHA, T0, P)
        if refl != 3:
            mask = refl in (1, 4, 6)
            e1.c = rconst
            e1.src(RGB, T1, PR if mask else K, PR)
            e1.op_rgb(C, A if mask else C)
            e1.func(RGB, MULTIPLY_ADD)
            if refl == 9:
                e1.scale(RGB, scale_index((flags >> 4) & 3))
    alpha_mode = (flags >> 20) & 7
    alpha = e[3]
    if refl == 25:
        e[2].c = constant
        e[2].src(ALPHA, T1, P, K)
        e[2].func(ALPHA, MULTIPLY_ADD)
        alpha.src(ALPHA, T0, PR)
        alpha.func(ALPHA, MODULATE)
    elif alpha_mode in (1, 2):
        alpha.src(ALPHA, T0 if alpha_mode == 1 and refl != 22 else T1, P)
        alpha.func(ALPHA, MODULATE)
    elif alpha_mode in (3, 6):
        alpha.src(ALPHA, T0, T1)
        alpha.func(ALPHA, MODULATE)
        if alpha_mode == 6:
            alpha.scale(ALPHA, 1)
        if not (refl == 14 and rconst & 1):
            e[4].src(ALPHA, PR, P)
            e[4].func(ALPHA, MODULATE)
    elif alpha_mode == 7:
        alpha.c = constant
        alpha.src(ALPHA, T0 if refl == 11 else T1, P, K)
        alpha.func(ALPHA, MULTIPLY_ADD)
    elif alpha_mode == 4:
        menu = lighting & MENU
        if menu:
            alpha.c = constant
        alpha.src(ALPHA, K if menu else P)
        alpha.func(ALPHA, REPLACE)
    elif alpha_mode == 5:
        alpha.c = (constant & 0xff000000) | 0x00808080 if refl == 8 else constant
        alpha.src(ALPHA, T0, T1, K)
        alpha.func(ALPHA, INTERPOLATE)
        if refl in (10, 30):
            e[4].src(ALPHA, PR, P)
            e[4].func(ALPHA, MODULATE)
    if add_override is not None and refl not in (7, 8, 14):
        # A course-object CLR0 glow (GX C1): stage 5 adds it.
        star = e[5] = Env()
        star.c = add_override & 0xffffff
        star.src(RGB, PR, K)
        star.func(RGB, ADD)
    if program:
        mask, words = program
        for k, (w, c) in enumerate(words):
            st = e[k]
            st.src(RGB, w & 15, (w >> 4) & 15, (w >> 8) & 15)
            st.op_rgb((w >> 12) & 15, (w >> 16) & 15, (w >> 20) & 15)
            st.func(RGB, (w >> 24) & 15)
            st.scale(RGB, scale_index((w >> 28) & 3))
            st.c = (c & 0x00ffffff) | (st.c & 0xff000000)
        buf[0] |= mask
    if lighting & RETAIL and (lighting >> 2) & 3 == 2 and refl not in (8, 10):
        e, buf = light_texture_stages(sub, constant)
    if lighting & PROGRAM_PASS and rconst & 8:
        # A split program's alpha pass: framebuffer alpha = its colour (R).
        e[5] = Env()
        e[5].src(ALPHA, PR)
        e[5].op_alpha(A_R)
    return e, tuple(buf), note


def light_texture_stages(sub, constant):
    """renderer_retail.cpp retailCombiners, channel 2, without fog or Star: lm_0
    on unit 1, lm_1 on unit 2."""
    lighting, add = sub[11], sub[13]
    primary = (lighting >> 11) & 3
    primary_scale = (lighting >> 13) & 3
    secondary = (lighting >> 15) & 3
    albedo_scale = (lighting >> 18) & 3
    e = [Env() for _ in range(6)]
    n = 0
    st = None
    if primary and primary_scale == 3:
        st = e[n]; n += 1
        st.c = 0x80808080
        st.src(BOTH, P, K)
        st.func(BOTH, MODULATE)
        if primary == 2:
            st.op_rgb(A)
        st = e[n]; n += 1
        st.src(RGB, T1, PR, PR)
        st.op_rgb(C, A, C)
        st.func(RGB, MULTIPLY_ADD)
    elif primary:
        st = e[n]; n += 1
        st.src(RGB, T1, P, P)
        st.op_rgb(C, A, A if primary == 2 else C)
        st.func(RGB, MULTIPLY_ADD)
        st.scale(RGB, scale_index(primary_scale))
    previous = PR if primary else P
    st = e[n]; n += 1
    if lighting & (1 << 17):
        st.c = sub[5]
        st.src(RGB, T0, K, previous)
        st.func(RGB, INTERPOLATE)
    else:
        st.src(RGB, T0, previous)
        st.func(RGB, MODULATE)
    st.scale(RGB, scale_index(albedo_scale))
    st.src(ALPHA, T0, P)
    st.func(ALPHA, MODULATE)
    if lighting & (1 << 20):
        factor = (1.0, 2.0, 4.0, 0.5)[albedo_scale]
        value = 0
        for i in range(3):
            c = ((add >> (24 - 8 * i)) & 255) / 255.0 * 255.0
            value |= min(255, int(math.floor(c * factor + 0.5))) << (8 * i)
        st = e[n]; n += 1
        st.c = value | 0xff000000
        st.src(RGB, PR, K)
        st.func(RGB, ADD)
    if lighting & (1 << 24):
        st = e[n]; n += 1
        st.c = constant
        st.src(RGB, PR, K)
        st.func(RGB, MODULATE)
    if secondary == 2:
        st = e[n]; n += 1
        st.src(RGB, T2, P, PR)
        st.func(RGB, MULTIPLY_ADD)
    elif secondary == 1:
        st = e[n]; n += 1
        st.src(RGB, T2, PR, PR)
        st.op_rgb(C, A, C)
        st.func(RGB, MULTIPLY_ADD)
    st.src(ALPHA, T0)
    st.func(ALPHA, REPLACE)
    if lighting & MENU and ((sub[3] >> 20) & 7) == 4:
        st = e[n]; n += 1
        st.c = sub[5]
        st.src(ALPHA, K)
        st.func(ALPHA, REPLACE)
    return e, [0, 0]


BLEND_REFERENCES = (0, 128, 165, 255)


def blend_factor(code, src_c, src_a, dst_c, dst_a):
    """GPU_BLENDFACTOR as an (n,3) multiplier."""
    n = len(src_a)
    if code == 0:
        return np.zeros((n, 3))
    if code == 1:
        return np.ones((n, 3))
    if code in (2, 3):
        f = src_c
    elif code in (4, 5):
        f = dst_c
    elif code in (6, 7):
        f = np.broadcast_to(src_a[:, None], (n, 3))
    elif code in (8, 9):
        f = np.broadcast_to(dst_a[:, None], (n, 3))
    elif code == 14:
        f = np.broadcast_to(np.minimum(src_a, 1 - dst_a)[:, None], (n, 3))
        return f
    else:
        f = np.zeros((n, 3))  # constant colour: the blend colour register is 0 for course draws
    return 1 - f if code & 1 and code < 14 else f


def glare_free(texture):
    """A camera-projected highlight seen from above: the value the projection
    gives away from its spot (the map's outermost texel ring; the sampler
    clamps). The spot is a glare fixed on the screen (ef_prj_glow: the sun's
    reflection toward the race camera), not the water's colour."""
    return ring(texture.levels[0], 1).mean(axis=0)


class Material:
    """Everything one submesh's draw needs, once per submesh."""

    def __init__(self, part, index, textures, notes):
        sub = part.submeshes[index]
        self.sub = sub
        self.owner = f"{part.name}#{index}"
        flags, lighting, refl, rconst = sub[3], sub[11], sub[14], sub[15]
        override = part.overrides.get(index, {})
        self.tint = override.get('tint')
        self.uv_rows = part.uv_rows.get(index)
        self.stages, self.buffer, note = material_stages(sub, part.programs.get(index), override)
        if note:
            notes.add(f'{part.name} submesh {index}: {note}')
        tex = override.get('texture', sub[2])
        sec = override.get('secondary', sub[4])
        primary = textures[tex] if 0 <= tex < len(textures) else None
        secondary = textures[sec] if 0 <= sec < len(textures) else None
        wrap0 = ((flags >> 8) & 3, (flags >> 10) & 3)
        wrap1 = ((flags >> 16) & 3, (flags >> 18) & 3)
        channel = (lighting >> 2) & 3 if lighting & RETAIL else 0
        mode, bit25 = (flags >> 12) & 3, bool(flags & (1 << 25))

        def unit(texture, uvset, wrap, mean=False):
            if texture is None:
                return ('const', np.ones(4))
            return ('mean', texture) if mean else ('sample', texture, uvset, wrap)
        if refl in (11, 22):
            # Camera-projected water: unit 0 samples the second map across the
            # screen (drawNow's envRow rows), a glare spot: held at its value
            # away from the spot (glare_free).
            glow = ('const', np.ones(4)) if secondary is None else ('const', glare_free(secondary))
            self.units = [glow, unit(primary, 0, wrap0), ('const', np.ones(4))]
        else:
            env1 = (normal_environment(lighting, refl, rconst) and not lighting & PRIMARY_ENVIRONMENT) or (bit25 and mode == 3)
            self.units = [unit(primary, 0, wrap0, bool(lighting & PRIMARY_ENVIRONMENT)),
                          unit(secondary, 1, wrap1, env1), ('const', np.ones(4))]
            if refl == 8:
                third = textures[(rconst & 0xffff) - 1] if 0 < (rconst & 0xffff) <= len(textures) else None
                self.units[2] = unit(third, 1, (0, 0), True)
        if channel == 2 and refl not in (8, 10):
            # EGG light textures lm_0/lm_1: rendered from the camera; held at black.
            self.units[1] = self.units[2] = ('const', np.array([0.0, 0.0, 0.0, 1.0]))
            notes.add(f'{part.name} submesh {index}: light textures held at black')
        self.water = refl in (11, 22) or (bool(flags & (1 << 29)) and not flags & 1)
        # Blend state (drawNow C3D_AlphaBlend).
        source_state = bool(flags & (1 << 31))
        blended = bool(flags & 2)
        self.blended = blended
        extended = sub[10] >> 16 if source_state else 0
        band = bool(flags & 1) and source_state and not (flags >> 26) & 7 and (sub[10] >> 16) & 255
        src = 6 if blended and not (source_state and flags & 128) else 1
        dst = (1 if source_state and flags & 64 else 7) if blended else 0
        if blended and not band and (extended >> 8) & 15:
            src = ((extended >> 8) & 15) - 1
        if blended and not band and (extended >> 12) & 15:
            dst = ((extended >> 12) & 15) - 1
        program_pass = bool(lighting & PROGRAM_PASS)
        self.reverse = program_pass and bool(rconst & 4)
        keep_alpha = program_pass and not rconst & (8 | 32)
        replace_alpha = program_pass and bool(rconst & (8 | 32))
        self.factors = (src, dst, 0 if keep_alpha else 1, 1 if keep_alpha else 0 if replace_alpha else dst)
        self.depth_test = not source_state or bool(flags & (1 << 23))
        self.depth_write = bool(flags & (1 << 24)) if source_state else not blended
        # Drawn after the opaque layer, lowest first (as seen from above).
        self.late = blended and not (lighting & AUTHORED_OPAQUE_ORDER or (program_pass and rconst & 16))
        # The fog overlay pass adds nothing without fog (the map has none).
        self.skip = program_pass and bool(rconst & 2)


def sky_colour(model_list, lights, ambient):
    """What the race camera sees through translucent water with nothing modelled
    beneath it: the course's sky model, as its materials draw it with every
    texture at its mean, averaged by area over its opaque layer's lower half
    (the half below the dome's centre; all of it when it has none). None
    without a sky model."""
    listing = SD / f'models/{model_list}_sky.list'
    if not listing.is_file():
        return None
    sums = {True: np.zeros(3), False: np.zeros(3)}
    weights = {True: 0.0, False: 0.0}
    notes = set()
    for name in listing.read_text().split():
        part = Part(course_path(f'models/{name}.gmdl'))
        textures = [Texture.get(n) for n in part.textures]
        centre = part.position[:, 1].mean()
        for index, sub in enumerate(part.submeshes):
            first, count = sub[0], sub[1]
            if count == 0:
                continue
            mat = Material(part, index, textures, notes)
            if mat.skip or mat.blended:
                continue
            tris = part.indices[first:first + count].reshape(-1, 3)
            p = part.position[tris]
            area = 0.5 * np.linalg.norm(np.cross(p[:, 1] - p[:, 0], p[:, 2] - p[:, 0]), axis=1)
            n = len(tris)
            vc = np.stack([primary_colour(sub, part.color[t], part.normal[t], lights, ambient).mean(axis=0) for t in tris])
            if mat.tint is not None:
                vc = vc * mat.tint
            inputs = {P: (vc[:, :3], vc[:, 3])}
            for k, spec in enumerate(mat.units):
                value = spec[1] if spec[0] == 'const' else spec[1].mean
                value = np.broadcast_to(value, (n, 4))
                inputs[T0 + k] = (value[:, :3], value[:, 3])
            rgb, _ = tev_run(mat.stages, mat.buffer, inputs, n)
            low = p[:, :, 1].mean(axis=1) <= centre
            for half in (True, False):
                w = area * (low if half else 1.0)
                sums[half] += (rgb * w[:, None]).sum(axis=0)
                weights[half] += w.sum()
    for half in (True, False):
        if weights[half] > 0:
            return np.clip(sums[half] / weights[half], 0, 1)
    return None


class FrameBuffer:
    """Colour, framebuffer alpha and height planes of the top-down render."""

    def __init__(self, frame, background):
        H, W = frame.height, frame.width
        self.colour = np.empty((H, W, 3))
        self.colour[:] = background
        self.alpha = np.zeros((H, W))
        self.depth = np.full((H, W), -np.inf)   # written by depth-writing draws
        self.cover = np.full((H, W), -np.inf)   # highest drawn fragment of any kind
        self.top = np.full((H, W), -np.inf)     # highest model fragment, ignoring the roof cut
        self.wet = np.zeros((H, W), bool)       # last drawn by water (statistics)
        self.owner = None                       # debugging: the last draw per pixel (Material.owner)
        self.void = None                        # seen through water over nothing (sky_colour)
        self.over_void = np.zeros((H, W), bool) # water drawn over nothing (statistics)


def render(parts, frame, ceiling, layers, lights, ambient, background, water_level, notes, void=None):
    """Top-down render into a FrameBuffer: the opaque layer in model order, then
    the blended draws lowest first."""
    fb = FrameBuffer(frame, background)
    fb.void = void
    late = []
    for part in parts:
        textures = [Texture.get(name) for name in part.textures]
        pix = frame.to_pixels(part.position)
        for index, sub in enumerate(part.submeshes):
            first, count = sub[0], sub[1]
            if count == 0 or part.cull.get(index) == 3:
                continue
            mat = Material(part, index, textures, notes)
            if mat.skip:
                continue
            tris = part.indices[first:first + count].reshape(-1, 3)
            if mat.late:
                for t in tris:
                    late.append((part.position[t, 1].mean(), part, mat, t, pix))
                continue
            for t in tris:
                draw_triangle(part, mat, t, pix, frame, ceiling, layers, lights, ambient, fb, water_level)
    late.sort(key=lambda e: e[0])
    for _, part, mat, t, pix in late:
        draw_triangle(part, mat, t, pix, frame, ceiling, layers, lights, ambient, fb, water_level)
    return fb


def lod_for(uv, xy, texture):
    """Mip level from the triangle's texel/pixel area ratio."""
    if texture is None:
        return 0
    base = texture.levels[0]
    (a, b, c) = xy
    pixel_area = abs((b[0] - a[0]) * (c[1] - a[1]) - (b[1] - a[1]) * (c[0] - a[0]))
    ua, ub, uc = uv
    texel_area = abs((ub[0] - ua[0]) * (uc[1] - ua[1]) - (ub[1] - ua[1]) * (uc[0] - ua[0])) * base.shape[0] * base.shape[1]
    if pixel_area <= 1e-9:
        return len(texture.levels) - 1
    return max(0.0, 0.5 * math.log2(max(texel_area / pixel_area, 1e-9)))


ALPHA_COMPARE = [lambda a, r: np.zeros_like(a, bool), lambda a, r: a < r, lambda a, r: a == r, lambda a, r: a <= r,
                 lambda a, r: a > r, lambda a, r: a != r, lambda a, r: a >= r, lambda a, r: np.ones_like(a, bool)]


def alpha_pass(sub, alpha8):
    """Alpha test of drawNow (sourceState: exact GX compare/reference)."""
    flags, anim = sub[3], sub[10]
    if not flags & 1:
        return np.ones_like(alpha8, bool)
    if flags & (1 << 31):
        extended = anim >> 16
        if not (flags >> 26) & 7 and extended & 255:
            # Two-sided band (tools/gx_material.py alpha_band): A <= lo or A >= hi.
            return (alpha8 <= (extended & 255) - 1) | (alpha8 >= extended >> 8)
        ref = (extended & 255) or BLEND_REFERENCES[(flags >> 29) & 3]
        return ALPHA_COMPARE[(flags >> 26) & 7](alpha8, ref)
    return alpha8 > 127


def on_ground(layers, iy, ix, y):
    """Fragments lying on a KCL floor or water surface of their pixel."""
    with np.errstate(invalid='ignore'):
        return (np.abs(layers[iy, ix] - y[:, None]) <= GROUND_BAND).any(axis=1)


def draw_triangle(part, mat, t, pix, frame, ceiling, layers, lights, ambient, fb, water_level):
    sub = mat.sub
    xy = pix[t]
    cov = raster(xy, frame.width, frame.height)
    if cov is None:
        return
    iy, ix, w0, w1, w2 = cov
    p = part.position[t]
    y = w0 * p[0, 1] + w1 * p[1, 1] + w2 * p[2, 1]
    np.maximum.at(fb.top, (iy, ix), y)
    keep = y <= ceiling[iy, ix]
    if not keep.all():
        keep |= on_ground(layers, iy, ix, y)
    if mat.depth_test:
        keep &= y >= fb.depth[iy, ix]
    if not keep.any():
        return
    iy, ix, w0, w1, w2, y = iy[keep], ix[keep], w0[keep], w1[keep], w2[keep], y[keep]
    n = len(y)
    w = np.stack([w0, w1, w2], axis=1)
    vc = w @ primary_colour(sub, part.color[t], part.normal[t], lights, ambient)
    if mat.tint is not None:
        vc = vc * mat.tint
    uvs = [part.uv[t], part.uv1[t]]
    if mat.uv_rows is not None:
        r = mat.uv_rows
        uvs = [np.stack([uvs[0] @ r[0, :2] + r[0, 2], uvs[0] @ r[1, :2] + r[1, 2]], axis=1),
               np.stack([uvs[1] @ r[2, :2] + r[2, 2], uvs[1] @ r[3, :2] + r[3, 2]], axis=1)]
    inputs = {P: (vc[:, :3], vc[:, 3])}
    for k, spec in enumerate(mat.units):
        if spec[0] == 'const':
            value = np.broadcast_to(spec[1], (n, 4))
        elif spec[0] == 'mean':
            value = np.broadcast_to(spec[1].mean, (n, 4))
        else:
            texture, uvset, (ws, wt) = spec[1], spec[2], spec[3]
            uv = w @ uvs[uvset]
            value = texture.sample(uv[:, 0], uv[:, 1], ws, wt, lod_for(uvs[uvset], xy, texture))
        inputs[T0 + k] = (value[:, :3], value[:, 3])
    rgb, alpha = tev_run(mat.stages, mat.buffer, inputs, n)
    ok = alpha_pass(sub, np.round(alpha * 255))
    if not ok.any():
        return
    iy, ix, y, rgb, alpha = iy[ok], ix[ok], y[ok], rgb[ok], alpha[ok]
    dst_c, dst_a = fb.colour[iy, ix], fb.alpha[iy, ix]
    sf, df, saf, daf = mat.factors
    src_term = rgb * blend_factor(sf, rgb, alpha, dst_c, dst_a)
    dst_term = dst_c * blend_factor(df, rgb, alpha, dst_c, dst_a)
    out = np.clip(dst_term - src_term if mat.reverse else src_term + dst_term, 0, 1)
    out_a = np.clip(alpha * saf + dst_a * (blend_factor(daf, rgb, alpha, dst_c, dst_a)[:, 0] if daf > 1 else daf), 0, 1)
    water = np.full(len(y), mat.water)
    if mat.blended:
        # Water keeps its translucency over what lies beneath. Over nothing it
        # blends over what the race camera sees through it there, the course's
        # sky (fb.void); without a sky model it is its own colour.
        level = water_level[iy, ix]
        water |= np.isfinite(level) & (np.abs(y - level) <= WATER_BAND)
        empty = water & ~np.isfinite(fb.cover[iy, ix])
        if empty.any():
            if fb.void is None:
                out[empty] = rgb[empty]
            else:
                e_c, e_a = rgb[empty], alpha[empty]
                v_c, v_a = np.broadcast_to(fb.void, e_c.shape), np.ones(len(e_a))
                s_t = e_c * blend_factor(sf, e_c, e_a, v_c, v_a)
                d_t = v_c * blend_factor(df, e_c, e_a, v_c, v_a)
                out[empty] = np.clip(d_t - s_t if mat.reverse else s_t + d_t, 0, 1)
            out_a[empty] = 1.0
            fb.over_void[iy[empty], ix[empty]] = True
    fb.colour[iy, ix] = out
    fb.alpha[iy, ix] = out_a
    fb.wet[iy, ix] = water
    if fb.owner is not None:
        fb.owner[iy, ix] = mat.owner
    if mat.depth_write:
        fb.depth[iy, ix] = y
    fb.cover[iy, ix] = np.maximum(fb.cover[iy, ix], y)


def blur(values, radius):
    """Separable box blur, approximately Gaussian after three passes (edges clamped)."""
    out = values.astype(np.float64)
    if radius < 1:
        return out
    k = 2 * radius + 1
    for _ in range(3):
        for axis in (0, 1):
            pad = [(0, 0), (0, 0)]
            pad[axis] = (radius + 1, radius)
            c = np.cumsum(np.pad(out, pad, mode='edge'), axis=axis)
            if axis == 0:
                out = (c[k:] - c[:-k]) / k
            else:
                out = (c[:, k:] - c[:, :-k]) / k
    return out


def finish(colour, height, frame, background, ground_height, supersample):
    """Map look: occlusion and relief from the height buffer, soft shadows under
    raised pieces, a dark surround where the course model has no geometry, and
    the map's outer band faded into the surround's edge colour (returned).

    height: the highest drawn surface per pixel (-inf where none)."""
    upp = (frame.x1 - frame.x0) / frame.width
    covered = np.isfinite(height)
    h = np.where(covered, height, ground_height)
    # Ambient occlusion: how far a pixel lies below its neighbourhood, at two radii.
    ao = np.ones_like(h)
    for radius_world, strength in ((350.0, 0.45), (1400.0, 0.30)):
        r = max(1, int(round(radius_world / upp)))
        lower = np.clip(blur(h, r) - h, 0, None)
        ao -= strength * np.clip(lower / (radius_world * 1.2), 0, 1)
    # Relief: light from the upper left (north-west, above) on the height field.
    gy, gx = np.gradient(blur(h, max(1, int(round(120.0 / upp)))), upp)
    n = np.stack([-gx, np.ones_like(h), -gy], axis=-1)
    n /= np.linalg.norm(n, axis=-1, keepdims=True)
    light = np.array([-0.45, 0.80, -0.40])
    light /= np.linalg.norm(light)
    relief = 0.82 + 0.18 * np.clip(n @ light / light[1], 0, 1.25)
    # Soft drop shadow toward the lower right of anything raised above its surroundings.
    shift = max(1, int(round(220.0 / upp)))
    raised = np.full_like(h, -np.inf)
    raised[shift:, shift:] = h[:-shift, :-shift]
    shadow = np.clip((raised - h - 250.0) / 900.0, 0, 1)
    shadow = blur(shadow, max(1, int(round(180.0 / upp))))
    shade = np.clip(ao, 0.35, 1) * relief * (1 - 0.35 * shadow)
    out = colour * shade[..., None]
    # Gentle lift toward MK7's brighter, saturated map look.
    mean = out.mean(axis=-1, keepdims=True)
    out = np.clip(mean + (out - mean) * 1.12, 0, 1) ** 0.92
    # The surround takes the course's own edge colour, darkened, so the model's
    # border fades into it instead of ending at a hard line.
    border = covered & (blur(covered.astype(float), max(2, frame.width // 128)) < 0.9)
    if border.any():
        background = 0.42 * np.median(out[border], axis=0)
    surround = background[None, None, :] * (0.8 + 0.2 * blur(covered.astype(float), max(2, frame.width // 64)))[..., None]
    edge = blur(covered.astype(float), 2)[..., None]
    out = out * edge + surround * (1 - edge)
    # The map's edge: the surround's far colour (u8, what the console draws past
    # the edge), exact over the last BORDER_EXACT texels, faded in over BORDER_FADE.
    edge_colour = np.round(np.clip(background * 0.8, 0, 1) * 255) / 255
    H, W = height.shape
    ys = np.minimum(np.arange(H), H - 1 - np.arange(H))[:, None]
    xs = np.minimum(np.arange(W), W - 1 - np.arange(W))[None, :]
    d = (np.minimum(ys, xs) + 0.5) / supersample
    t = np.clip((d - BORDER_EXACT) / BORDER_FADE, 0, 1)
    t = (t * t * (3 - 2 * t))[..., None]
    return edge_colour + (out - edge_colour) * t, edge_colour


def floor_maps(kcl, frame):
    """Per pixel: the highest drivable road surface, the highest ground of any
    kind and the highest KCL water surface (-inf where none). Road under a water
    plane is no road: a kart falls out at the water. Then every road (the rule
    before Nov 25, for the statistics), and the heights of the KCL floors and
    water under each pixel (up to FLOOR_LAYERS, highest first, -inf padded):
    a model surface on one of them is ground, never a roof."""
    a, b, c, kind, face, attr = kcl
    road = np.full((frame.height, frame.width), -np.inf)
    road_all = road.copy()
    ground = np.full((frame.height, frame.width), -np.inf)
    water = np.full((frame.height, frame.width), -np.inf)
    layers = np.full((frame.height, frame.width, FLOOR_LAYERS), -np.inf, np.float32)

    def layer(iy, ix, y):
        cur = layers[iy, ix]
        new = ~(np.abs(cur - y[:, None]) < 100.0).any(axis=1)
        if new.any():
            merged = np.concatenate([cur[new], y[new, None]], axis=1)
            layers[iy[new], ix[new]] = -np.sort(-merged, axis=1)[:, :FLOOR_LAYERS]
    variant = (attr >> 5) & 7
    wet = (np.isin(kind, list(WATER_TYPES)) & np.isin(variant, list(WATER_VARIANTS))) | (kind == MOVING_WATER)
    for i in np.nonzero(wet & (np.abs(face[:, 1]) > 0.2))[0]:
        tri = np.stack([a[i], b[i], c[i]])
        cov = raster(frame.to_pixels(tri), frame.width, frame.height)
        if cov is None:
            continue
        iy, ix, w0, w1, w2 = cov
        y = w0 * tri[0, 1] + w1 * tri[1, 1] + w2 * tri[2, 1]
        np.maximum.at(water, (iy, ix), y)
        layer(iy, ix, y)
    upward = face[:, 1] > 0.2
    for i in np.nonzero(upward & np.isin(kind, list(FLOOR_TYPES)))[0]:
        tri = np.stack([a[i], b[i], c[i]])
        cov = raster(frame.to_pixels(tri), frame.width, frame.height)
        if cov is None:
            continue
        iy, ix, w0, w1, w2 = cov
        y = w0 * tri[0, 1] + w1 * tri[1, 1] + w2 * tri[2, 1]
        np.maximum.at(ground, (iy, ix), y)
        layer(iy, ix, y)
        if kind[i] in ROAD_TYPES:
            np.maximum.at(road_all, (iy, ix), y)
            dry = ~(y < water[iy, ix] - 50.0)
            np.maximum.at(road, (iy[dry], ix[dry]), y[dry])
    return road, ground, water, road_all, layers


def dilate_max(values, radius):
    out = values.copy()
    for dy in range(-radius, radius + 1):
        for dx in range(-radius, radius + 1):
            if dx * dx + dy * dy > radius * radius:
                continue
            shifted = np.full_like(values, -np.inf)
            ys = slice(max(dy, 0), values.shape[0] + min(dy, 0))
            yd = slice(max(-dy, 0), values.shape[0] + min(-dy, 0))
            xs = slice(max(dx, 0), values.shape[1] + min(dx, 0))
            xd = slice(max(-dx, 0), values.shape[1] + min(-dx, 0))
            shifted[yd, xd] = values[ys, xs]
            np.maximum(out, shifted, out=out)
    return out


def ceiling_map(road, ground, frame):
    """Highest visible height per pixel: CLEARANCE over the drivable road beneath,
    the course's highest road where no ground lies within ~3000 units (floating
    decorations over the void), and unbounded elsewhere."""
    upp = (frame.x1 - frame.x0) / frame.width
    top = np.max(road[np.isfinite(road)]) if np.isfinite(road).any() else np.inf
    ceiling = dilate_max(road, max(1, int(round(150.0 / upp)))) + CLEARANCE
    near_ground = blur(np.isfinite(ground).astype(float), max(1, int(round(1000.0 / upp)))) > 1e-4
    ceiling[~np.isfinite(ceiling)] = np.inf
    ceiling[~near_ground] = np.minimum(ceiling[~near_ground], top)
    return ceiling


def course_frame(kmp, margin=3000.0):
    """World rectangle around everything a racer can reach (KMP checkpoints, enemy and
    item routes, respawn and start points) plus a margin, and a texture size (512 or
    1024 a side, at most 1024x1024 = 512 KB of ETC1) with square texels."""
    pts = []
    for r in kmp['CKPT']['records']:
        pts += [r['left'], r['right']]
    for section in ('ENPT', 'ITPT', 'JGPT', 'KTPT'):
        pts += [(q['position'][0], q['position'][2]) for q in kmp.get(section, {}).get('records', [])]
    pts = np.array(pts)
    x0, x1 = pts[:, 0].min() - margin, pts[:, 0].max() + margin
    z0, z1 = pts[:, 1].min() - margin, pts[:, 1].max() + margin
    w, h = x1 - x0, z1 - z0
    size = [1024, 1024]
    if w >= h * 1.6:
        size[1] = 512
    elif h >= w * 1.6:
        size[0] = 512
    # Grow the shorter side so both axes share one texel size.
    texel = max(w / size[0], h / size[1])
    cx, cz = (x0 + x1) / 2, (z0 + z1) / 2
    return (cx - texel * size[0] / 2, cz - texel * size[1] / 2, cx + texel * size[0] / 2, cz + texel * size[1] / 2), tuple(size)


def road_width(kcl, route_length):
    """Mean drivable road width: road surface area seen from above over the lap length."""
    a, b, c, kind, face = kcl
    road = np.isin(kind, list(ROAD_TYPES)) & (face[:, 1] > 0.2)
    ab, ac = b[road] - a[road], c[road] - a[road]
    area = 0.5 * np.abs(ab[:, 0] * ac[:, 2] - ab[:, 2] * ac[:, 0]).sum()
    return area / max(route_length, 1.0)


# KCL walls (attribute & 0x1f): wall, invisible, item, wall 2, player-only,
# weak, special and second invisible walls.
WALL_TYPES = {0x0C, 0x0D, 0x0E, 0x0F, 0x14, 0x19, 0x1E, 0x1F}


def road_span(kcl, left, right, anchor, step=25.0, gap=300.0):
    """The part of a KMP checkpoint segment (left, right; world XZ) that crosses
    drivable road, seen from above: the run of road containing the sample nearest
    `anchor` (the first start position: x, y, z), bridging gaps shorter than `gap`
    units and ending at the first wall either side. Checkpoints reach well past
    the road's edges; Mario Kart 7 draws its start line across the road only."""
    a, b, c, kind, face = kcl
    road = np.isin(kind, list(ROAD_TYPES)) & (face[:, 1] > 0.2)
    p0, p1 = np.asarray(left, float), np.asarray(right, float)
    lo_xz, hi_xz = np.minimum(p0, p1), np.maximum(p0, p1)
    xs = np.stack([a[:, 0], b[:, 0], c[:, 0]], axis=1)
    zs = np.stack([a[:, 2], b[:, 2], c[:, 2]], axis=1)
    near = road & (xs.max(axis=1) >= lo_xz[0]) & (xs.min(axis=1) <= hi_xz[0]) & (zs.max(axis=1) >= lo_xz[1]) & (zs.min(axis=1) <= hi_xz[1])
    ta, tb, tc = a[near][:, [0, 2]], b[near][:, [0, 2]], c[near][:, [0, 2]]
    length = float(np.hypot(*(p1 - p0)))
    n = max(2, int(length / step) + 1)
    t = np.linspace(0.0, 1.0, n)
    pts = p0[None, :] + t[:, None] * (p1 - p0)[None, :]

    def side(p, u, v):
        return (u[None, :, 0] - p[:, None, 0]) * (v[None, :, 1] - p[:, None, 1]) - (u[None, :, 1] - p[:, None, 1]) * (v[None, :, 0] - p[:, None, 0])
    s0, s1, s2 = side(pts, ta, tb), side(pts, tb, tc), side(pts, tc, ta)
    on = (((s0 >= 0) & (s1 >= 0) & (s2 >= 0)) | ((s0 <= 0) & (s1 <= 0) & (s2 <= 0))).any(axis=1)
    if not on.any():
        return tuple(p0), tuple(p1)
    # Bridge short gaps (seams, a narrow strip of another surface).
    idx = np.nonzero(on)[0]
    bridge = int(gap / (length / (n - 1)))
    for i, j in zip(idx[:-1], idx[1:]):
        if j - i <= bridge:
            on[i:j] = True
    d = (p1 - p0) / max(length, 1e-9)
    anchor_xz = np.asarray([anchor[0], anchor[2]], float)
    k = int(np.clip(round(float(np.dot(anchor_xz - p0, d)) / length * (n - 1)), 0, n - 1))
    if not on[k]:
        k = int(idx[np.argmin(np.abs(idx - k))])
    # Walls at the start's height that cross the segment end the run.
    wall = np.isin(kind, list(WALL_TYPES)) & (np.abs(face[:, 1]) < 0.7)
    ys = np.stack([a[:, 1], b[:, 1], c[:, 1]], axis=1)
    wall &= (ys.max(axis=1) > anchor[1] - 300.0) & (ys.min(axis=1) < anchor[1] + 1500.0)
    wall &= (xs.max(axis=1) >= lo_xz[0]) & (xs.min(axis=1) <= hi_xz[0]) & (zs.max(axis=1) >= lo_xz[1]) & (zs.min(axis=1) <= hi_xz[1])
    cuts = []
    r = p1 - p0
    for u, v in ((a, b), (b, c), (c, a)):
        q0, q1 = u[wall][:, [0, 2]], v[wall][:, [0, 2]]
        s = q1 - q0
        den = r[0] * s[:, 1] - r[1] * s[:, 0]
        with np.errstate(divide='ignore', invalid='ignore'):
            tt = ((q0[:, 0] - p0[0]) * s[:, 1] - (q0[:, 1] - p0[1]) * s[:, 0]) / den
            uu = ((q0[:, 0] - p0[0]) * r[1] - (q0[:, 1] - p0[1]) * r[0]) / den
        hit = np.isfinite(tt) & (tt >= 0) & (tt <= 1) & (uu >= 0) & (uu <= 1)
        cuts += list(tt[hit])
    for x in cuts:
        i = int(round(x * (n - 1)))
        if i != k:
            on[np.clip(i, 0, n - 1)] = False
    lo = k
    while lo > 0 and on[lo - 1]:
        lo -= 1
    hi = k
    while hi < n - 1 and on[hi + 1]:
        hi += 1
    half = 0.5 / (n - 1)
    ends = np.clip([t[lo] - half, t[hi] + half], 0.0, 1.0)
    return tuple(p0 + ends[0] * (p1 - p0)), tuple(p0 + ends[1] * (p1 - p0))


def ring(values, width):
    """The outermost `width` pixels of a 2D array."""
    mask = np.zeros(values.shape[:2], bool)
    mask[:width] = mask[-width:] = True
    mask[:, :width] = mask[:, -width:] = True
    return values[mask]


def bake(archive_key, model_list, lighting_archive, frame_rect, size, supersample=2):
    """The finished map image (RGB uint8, rows -z first) and render statistics."""
    kcl = kcl_triangles(ROOT / 'assets/courses' / archive_key / 'course.kcl', attributes=True)
    parts = [Part(course_path(f'models/{n}.gmdl')) for n in (SD / f'models/{model_list}.list').read_text().split()]
    water_parts, water_names = water_objects(archive_key, model_list)
    parts += water_parts
    x0, z0, x1, z1 = frame_rect
    frame = Frame(x0, z0, x1, z1, size[0] * supersample, size[1] * supersample)
    road, ground, water, road_all, layers = floor_maps(kcl, frame)
    ceiling = ceiling_map(road, ground, frame)
    lights, ambient = course_lighting(lighting_archive)
    background = np.array([0.10, 0.13, 0.18])
    notes = set()
    Texture.missing = set()
    void = sky_colour(model_list, lights, ambient)
    fb = render(parts, frame, ceiling, layers, lights, ambient, background, water, notes, void)
    finite_ground = ground[np.isfinite(ground)]
    ground_height = np.median(finite_ground) if len(finite_ground) else 0.0
    image, edge = finish(fb.colour, fb.cover, frame, background, ground_height, supersample)
    full = Image.fromarray((np.clip(image, 0, 1) * 255 + 0.5).astype(np.uint8))
    top = np.isfinite(fb.top)
    kept = on_ground(layers, *np.indices(top.shape).reshape(2, -1), fb.top.ravel()).reshape(top.shape)
    return full.resize(size, Image.Resampling.BOX), {
        'parts': len(parts) - len(water_parts), 'triangles': int(sum(len(p.indices) for p in parts) // 3),
        'coverage': float(np.isfinite(fb.cover).mean()),
        # Share of the map with drivable road beneath (before Nov 25 this was named cutPixels).
        'roadUnder': float(np.isfinite(road).mean()),
        # Share of the map whose topmost model surface the roof cut removes.
        'cutPixels': float((top & (fb.top > ceiling) & ~kept).mean()),
        # The rule before Nov 25 (every KCL road counts, no ground kept), same measure.
        'cutPixelsBefore': float((top & (fb.top > ceiling_map(road_all, ground, frame))).mean()),
        'water': float(np.isfinite(water).mean()),
        # Share of the map drawn as water, and where it is densest (final texels x, y).
        'waterDrawn': float(fb.wet.mean()),
        # Share of the map drawn as water over nothing (blended over the sky's colour).
        'waterOverVoid': float(fb.over_void.mean()),
        'void': None if void is None else [int(round(v * 255)) for v in void],
        'waterCentre': [int(v) // supersample for v in np.unravel_index(np.argmax(blur(fb.wet.astype(float), 32)), fb.wet.shape)[::-1]],
        # Course geometry in the outermost texel ring (what clamping used to smear).
        'edgeGeometry': float(ring(np.isfinite(fb.cover), supersample).mean()),
        'border': [int(round(v * 255)) for v in edge],
        'waterObjects': water_names,
        'missingTextures': sorted(Texture.missing), 'notEvaluated': sorted(notes)}
