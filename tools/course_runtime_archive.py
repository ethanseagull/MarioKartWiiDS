"""Course archives as the race pack stores them.

Kinoko keeps a course's whole archive in its arena for the race, but it reads
only the collision, the map data and the object files from it: the course,
sky and minimap models are cooked into the pack's own model files at build
time and never read from the archive at run time. The pack therefore stores each course archive with those three
files emptied (the same U8 tree, so every other file keeps its name and
bytes), as plain U8 instead of Yaz0: Kinoko then uses the file in place
(DvdArchive::load) instead of holding the compressed file and its expanded
copy at once, and the pack's LZ4 compresses it.

Usage: course_runtime_archive.py            every catalog course into build/course-runtime
       course_runtime_archive.py --out DIR  the same into DIR (a cook check)
       course_runtime_archive.py SOURCE.szs OUT.szs  one archive
"""
from pathlib import Path
import hashlib
import json
import struct
import sys

ROOT = Path(__file__).resolve().parents[1]
FOLDER = ROOT / 'build/course-runtime'

# Models cooked from the archive at build time, never read from it at run time.
RUNTIME_DROPPED = {'course_model.brres', 'vrcorn_model.brres', 'map_model.brres'}


def yaz0_expand(data):
    """Yaz0 -> raw bytes (a raw U8 archive passes through)."""
    if data[:4] != b'Yaz0':
        return data
    size, out, at = struct.unpack_from('>I', data, 4)[0], bytearray(), 16
    while len(out) < size:
        flags = data[at]; at += 1
        for bit in range(7, -1, -1):
            if len(out) >= size:
                break
            if flags >> bit & 1:
                out.append(data[at]); at += 1
            else:
                b1, b2 = data[at], data[at + 1]; at += 2
                back = ((b1 & 15) << 8 | b2) + 1
                count = (b1 >> 4) + 2
                if count == 2:
                    count = data[at] + 0x12; at += 1
                for _ in range(count):
                    out.append(out[-back])
    return bytes(out)


def runtime_archive(data, dropped=RUNTIME_DROPPED):
    """The U8 tree of `data` with the files named in `dropped` emptied, as
    plain U8. Every other file keeps its bytes; files stay 32-byte aligned."""
    u8 = yaz0_expand(data)
    if u8[:4] != b'U\xaa8-':
        raise ValueError('not a U8 archive')
    root, header_size, data_at = struct.unpack_from('>III', u8, 4)
    count = struct.unpack_from('>I', u8, root + 8)[0]
    strings = root + count * 12
    head = bytearray(u8[:data_at])
    body = bytearray()
    for i in range(count):
        node = root + i * 12
        info, offset, size = struct.unpack_from('>III', u8, node)
        if info >> 24:
            continue
        start = strings + (info & 0xffffff)
        name = u8[start:u8.index(0, start)].decode('latin1')
        payload = u8[offset:offset + size] if name not in dropped else b''
        struct.pack_into('>II', head, node + 4, data_at + len(body), len(payload))
        body += payload
        body += b'\0' * (-len(body) % 32)
    return bytes(head) + bytes(body)


def files(u8):
    """{path: bytes} of every file in a plain U8 archive (for checks)."""
    root, header_size, data_at = struct.unpack_from('>III', u8, 4)
    count = struct.unpack_from('>I', u8, root + 8)[0]
    strings = root + count * 12
    out, ends, path = {}, [], []
    for i in range(1, count):
        while ends and ends[-1] <= i:
            ends.pop(); path.pop()
        info, offset, size = struct.unpack_from('>III', u8, root + i * 12)
        start = strings + (info & 0xffffff)
        name = u8[start:u8.index(0, start)].decode('latin1')
        if info >> 24:
            ends.append(size); path.append(name)
        else:
            out['/'.join(path + [name])] = u8[offset:offset + size]
    return out


def key(source_sha256):
    return source_sha256 + ' u8 ' + ','.join(sorted(RUNTIME_DROPPED))


def runtime_path(archive, source_sha256, folder=FOLDER):
    """The cooked copy of catalog archive `archive` (Race/Course/x.szs), or an
    error when it is missing or was made from another source."""
    runtime = Path(folder) / Path(archive).name
    stamp = runtime.with_suffix('.key')
    if not runtime.is_file() or not stamp.is_file() or stamp.read_text() != key(source_sha256):
        raise ValueError('Run tools/course_runtime_archive.py before packaging: ' + str(archive))
    return runtime


def cook_all(folder=FOLDER):
    catalog = json.loads((ROOT / 'sd-card/3ds/mkw-3ds/models/course-catalog.json').read_text())
    folder = Path(folder)
    folder.mkdir(parents=True, exist_ok=True)
    rows = []
    for course in catalog['courses']:
        archive = Path(course['archive'])
        source = ROOT / 'data/extracted' / archive
        data = source.read_bytes()
        if hashlib.sha256(data).hexdigest() != course['archive_sha256']:
            raise ValueError('Source archive changed: ' + course['name'])
        runtime = folder / archive.name
        stamp = runtime.with_suffix('.key')
        made = runtime_archive(data)
        runtime.write_bytes(made)
        stamp.write_text(key(course['archive_sha256']))
        u8 = yaz0_expand(data)
        kept = files(u8)
        dropped = sorted(n for n in kept if n.rsplit('/', 1)[-1] in RUNTIME_DROPPED)
        # Every file not dropped keeps its bytes.
        after = files(made)
        if sorted(after) != sorted(kept) or any(after[n] != kept[n] for n in kept if n not in dropped) or any(after[n] for n in dropped):
            raise ValueError('Runtime archive differs beyond the dropped models: ' + course['name'])
        rows.append({'id': course['id'], 'archive': archive.as_posix(), 'source_bytes': len(data), 'u8_bytes': len(u8),
                     'runtime_bytes': len(made), 'dropped': dropped, 'runtime_sha256': hashlib.sha256(made).hexdigest()})
    (folder / 'receipt.json').write_text(json.dumps({'dropped': sorted(RUNTIME_DROPPED), 'courses': rows}, indent=1) + chr(10))
    print('course runtime archives:', len(rows), 'courses,', sum(r['source_bytes'] for r in rows) >> 10, 'KB Yaz0 ->',
          sum(r['runtime_bytes'] for r in rows) >> 10, 'KB U8 (expanded before:', sum(r['u8_bytes'] for r in rows) >> 10, 'KB)')


if __name__ == '__main__':
    if len(sys.argv) == 3 and sys.argv[1] != '--out':
        Path(sys.argv[2]).write_bytes(runtime_archive(Path(sys.argv[1]).read_bytes()))
    elif len(sys.argv) == 3:
        cook_all(Path(sys.argv[2]))
    elif len(sys.argv) == 1:
        cook_all()
    else:
        raise SystemExit(__doc__)
