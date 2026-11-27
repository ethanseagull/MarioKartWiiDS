# CPU sweep with the PC simulator (tools/cpu_sim): races on every course, then the places
# where CPUs fall or stall, clustered per course. Judge CPU changes by clustered falls (several
# racers falling at one spot, lap after lap), not by totals.
#
#   py -3 tools/cpu_sim/sweep.py --extract RACE_PAK ROOT        course data from your race.pak
#   py -3 tools/cpu_sim/sweep.py EXE ROOT OUTDIR [--seeds N] [--jobs J] [--courses 0-31] [--extra "--cc 0"]
#   py -3 tools/cpu_sim/sweep.py --compare OUT_A OUT_B          clusters only in A, only in B
#
# A sweep writes OUTDIR/<course>-s<seed>.json (one race each), OUTDIR/clusters.json and clusters.md.
import json, os, struct, subprocess, sys, concurrent.futures as cf
from pathlib import Path
from collections import defaultdict

NAMES = ["Luigi Circuit", "Moo Moo Meadows", "Mushroom Gorge", "Toad's Factory", "Mario Circuit", "Coconut Mall",
         "DK Summit", "Wario's Gold Mine", "Daisy Circuit", "Koopa Cape", "Maple Treeway", "Grumble Volcano",
         "Dry Dry Ruins", "Moonview Highway", "Bowser's Castle", "Rainbow Road", "GCN Peach Beach", "DS Yoshi Falls",
         "SNES Ghost Valley 2", "N64 Mario Raceway", "N64 Sherbet Land", "GBA Shy Guy Beach", "DS Delfino Square",
         "GCN Waluigi Stadium", "DS Desert Hills", "GBA Bowser Castle 3", "N64 DK's Jungle Parkway", "GCN Mario Circuit",
         "SNES Mario Circuit 3", "DS Peach Gardens", "GCN DK Mountain", "N64 Bowser's Castle"]


# ---- race.pak (MPK2: an index, entries stored raw or as LZ4 blocks) ----
def pak_index(pak):
    with open(pak, 'rb') as d:
        head = d.read(12)
        if head[:4] != b'MPK2':
            raise ValueError(f'{pak} is not a pack')
        count, nb = struct.unpack_from('<II', head, 4)
        table = d.read(16 * count)
        names = d.read(nb)
    out = {}
    for i in range(count):
        na, off, size, stored = struct.unpack_from('<IIII', table, 16 * i)
        out[names[na:names.index(b'\0', na)].decode()] = (off, size, stored & 0x7fffffff)
    return out


def unlz4(src, size):
    out = bytearray()
    i, n = 0, len(src)
    while i < n:
        t = src[i]; i += 1
        lit = t >> 4
        if lit == 15:
            while True:
                b = src[i]; i += 1; lit += b
                if b != 255: break
        out += src[i:i + lit]; i += lit
        if i >= n: break
        off = src[i] | src[i + 1] << 8; i += 2
        ml = t & 15
        if ml == 15:
            while True:
                b = src[i]; i += 1; ml += b
                if b != 255: break
        ml += 4
        start = len(out) - off
        for k in range(ml): out.append(out[start + k])
    if len(out) != size:
        raise ValueError('bad LZ4 entry')
    return bytes(out)


def extract(pak, root):
    index = pak_index(pak)
    names = [k for k in index if k == 'Race/Common.szs' or k.startswith('Race/Course/')]
    for k in names:
        off, size, stored = index[k]
        with open(pak, 'rb') as d:
            d.seek(off); b = d.read(stored)
        p = Path(root) / k
        p.parent.mkdir(parents=True, exist_ok=True)
        p.write_bytes(b if stored == size else unlz4(b, size))
    print(len(names), 'files extracted to', root)


# ---- races ----
def p0end(r):
    """The tick player 0 finished (the race the player sees ends on its results page); the whole
    race when player 0 never finished."""
    return r.get('p0Finish') or r['ticks']


def counted(r, f):
    # A fall or stuck spell the player can see: before player 0's finish and before the racer's own.
    return not f.get('after') and f['tick'] <= p0end(r)


def run_one(exe, root, out, course, seed, extra):
    path = out / f'{course}-s{seed}.json'
    if path.exists():
        try: return json.loads(path.read_text())
        except Exception: pass
    cmd = [str(exe), '--root', str(root), '--course', str(course), '--seed', str(seed), '--out', str(path)] + extra
    p = subprocess.run(cmd, capture_output=True, text=True, timeout=1800)
    if p.returncode != 0 or not path.exists():
        r = {'course': course, 'seed': seed, 'error': f'exit {p.returncode}', 'stderr': (p.stderr or p.stdout)[-400:]}
        path.write_text(json.dumps(r))
        return r
    return json.loads(path.read_text())


# ---- clusters ----
NEAR = 1500.0          # falls within this distance (x/z) are one place
SPOT_RATE = 1.0        # flagged: a spot where self falls (or slow spells) happen once per race or more


def clusters(results):
    """results: one course's race JSONs (several seeds). Returns (flagged spots, all spots)."""
    races = max(1, sum(1 for r in results if 'error' not in r))
    events = []
    for r in results:
        if 'error' in r: continue
        for f in r['falls']:
            if not counted(r, f): continue
            # A fall within 3 s of an item hit or other action (spin, tumble) is the item's;
            # the bug signal is falls the racer drove into itself.
            kind = 'fall' if f.get('hitAgo', 99999) >= 180 else 'hitfall'
            back = next((x for x in r.get('respawns', []) if x['racer'] == f['racer'] and x['tick'] > f['tick']), None)
            events.append((kind, r['seed'], f['racer'], f['lap'], f['point'], f['pos'], f['tick'], back and (tuple(back['pos']), back['point'])))
        for s in r['stuck']:
            if s['tick'] > p0end(r): continue
            events.append(('stuck', r['seed'], s['racer'], s['lap'], s['point'], s['pos'], s['tick'], None))
    spots = []
    for e in events:
        for s in spots:
            if s['kind'] == e[0] and abs(s['pos'][0] - e[5][0]) < NEAR and abs(s['pos'][2] - e[5][2]) < NEAR and abs(s['pos'][1] - e[5][1]) < 3 * NEAR:
                s['events'].append(e); break
        else:
            spots.append({'kind': e[0], 'pos': e[5], 'events': [e]})
    out = []
    for s in spots:
        ev = s['events']
        racers = {(e[1], e[2]) for e in ev}          # (seed, racer)
        laps_per_racer = defaultdict(set)
        for e in ev: laps_per_racer[(e[1], e[2])].add(e[3])
        per_lap = defaultdict(int)
        for e in ev: per_lap[(e[1], e[2], e[3])] += 1
        pc = defaultdict(int)
        for e in ev: pc[e[4]] += 1
        backs = defaultdict(int)
        for e in ev:
            if e[7]: backs[(tuple(round(v / 100) * 100 for v in e[7][0]), e[7][1])] += 1
        out.append({'kind': s['kind'], 'pos': [round(v) for v in s['pos']], 'count': len(ev), 'racers': len(racers),
                    'seeds': len({e[1] for e in ev}), 'maxLapsOneRacer': max(len(v) for v in laps_per_racer.values()),
                    'loop': max(per_lap.values()), 'points': [p for p, _ in sorted(pc.items(), key=lambda kv: -kv[1])][:8],
                    'laps': sorted({e[3] for e in ev}), 'respawn': max(backs.items(), key=lambda kv: kv[1])[0] if backs else None,
                    'perRace': round(len(ev) / races, 2)})
    # Flagged: a respawn loop (3+ falls of one racer in one lap), or a spot several racers hit
    # once per race or more on average (over all seeds). Item falls are listed, never flagged.
    flagged = [c for c in out if c['kind'] != 'hitfall' and (c['loop'] >= 3 or (c['perRace'] >= SPOT_RATE and c['racers'] >= 3))]
    flagged.sort(key=lambda c: -c['perRace'])
    return flagged, out


def summarize(ids, res):
    summary = []
    for c in ids:
        rs = sorted(res[c], key=lambda r: r.get('seed', 0))
        ok = [r for r in rs if 'error' not in r]
        flagged, spots = clusters(rs)
        n = max(len(ok), 1)
        falls = sum(1 for r in ok for x in r['falls'] if counted(r, x))
        selff = sum(1 for r in ok for x in r['falls'] if counted(r, x) and x.get('hitAgo', 99999) >= 180)
        stuck = sum(1 for r in ok for x in r['stuck'] if x['tick'] <= p0end(r))
        summary.append({'course': c, 'name': NAMES[c] if c < len(NAMES) else '?', 'races': len(ok),
                        'errors': [r.get('error') for r in rs if 'error' in r],
                        'fallsPerRace': round(falls / n, 2), 'selfFallsPerRace': round(selff / n, 2), 'stuckPerRace': round(stuck / n, 2),
                        'unfinished': sum(1 for r in ok for x in r['racers'] if not x['finished']),
                        'p0Unfinished': sum(1 for r in ok if not r['racers'][0]['finished']),
                        'clusterEventsPerRace': round(sum(x['count'] for x in flagged) / n, 2),
                        'loops': sum(1 for x in flagged if x['loop'] >= 3), 'clusters': flagged, 'spots': spots})
    return summary


def write_report(out, summary):
    (out / 'clusters.json').write_text(json.dumps(summary, indent=1))
    with open(out / 'clusters.md', 'w', encoding='utf-8') as f:
        f.write('Spots: falls a racer drove into itself (no item hit or spin in the 3 s before) or slow spells '
                '(speed under 5 for 2 s+), grouped within 1500 units, counted up to player 0\'s finish. Flagged: '
                'a respawn loop (3+ falls of one racer in one lap) or 1+ event per race from 3+ racers. '
                'r = racers (seed x racer), s = seeds, loop = most falls of one racer in one lap.\n\n')
        f.write('| course | name | races | falls/race | self falls/race | slow spells/race | unfinished (p0) | clustered events/race | flagged spots |\n')
        f.write('|---|---|---|---|---|---|---|---|---|\n')
        for s in sorted(summary, key=lambda s: (-s['clusterEventsPerRace'], s['course'])):
            top = '; '.join(f"{c['kind']} ENPT {c['points'][:3]} at {c['pos']} {c['perRace']}/race r{c['racers']} s{c['seeds']} loop{c['loop']}" for c in s['clusters'][:4])
            err = ' ERR ' + str(s['errors'][:1]) if s['errors'] else ''
            f.write(f"| {s['course']} | {s['name']}{err} | {s['races']} | {s['fallsPerRace']} | {s['selfFallsPerRace']} | {s['stuckPerRace']} | "
                    f"{s['unfinished']} ({s['p0Unfinished']}) | {s['clusterEventsPerRace']} | {top} |\n")


def same_spot(a, b):
    return a['kind'] == b['kind'] and abs(a['pos'][0] - b['pos'][0]) < NEAR and abs(a['pos'][2] - b['pos'][2]) < NEAR


def compare(a_dir, b_dir):
    """Flagged clusters of sweep A with no spot of the same kind near them in sweep B (and the reverse),
    and each course's clustered events per race in both."""
    A = {s['course']: s for s in json.loads((Path(a_dir) / 'clusters.json').read_text())}
    B = {s['course']: s for s in json.loads((Path(b_dir) / 'clusters.json').read_text())}
    print(f'| course | name | A clustered/race | B clustered/race | A falls/race | B falls/race | only in A | only in B |')
    print('|---|---|---|---|---|---|---|---|')
    for c in sorted(set(A) | set(B)):
        a, b = A.get(c), B.get(c)
        if not a or not b: continue
        def only(x, y):
            return [k for k in x['clusters'] if not any(same_spot(k, z) and z['perRace'] >= k['perRace'] / 2 for z in y['spots'])]
        oa, ob = only(a, b), only(b, a)
        fmt = lambda l: '; '.join(f"{k['kind']} ENPT {k['points'][:2]} {k['perRace']}/race r{k['racers']} loop{k['loop']}" for k in l) or '-'
        print(f"| {c} | {a['name']} | {a['clusterEventsPerRace']} | {b['clusterEventsPerRace']} | {a['fallsPerRace']} | {b['fallsPerRace']} | {fmt(oa)} | {fmt(ob)} |")


def main():
    a = sys.argv[1:]
    if a and a[0] == '--extract':
        return extract(a[1], a[2])
    if a and a[0] == '--compare':
        return compare(a[1], a[2])
    exe, root, out = Path(a[0]).resolve(), Path(a[1]).resolve(), Path(a[2])
    out.mkdir(parents=True, exist_ok=True)
    seeds = int(a[a.index('--seeds') + 1]) if '--seeds' in a else 4
    jobs = int(a[a.index('--jobs') + 1]) if '--jobs' in a else 4
    extra = a[a.index('--extra') + 1].split() if '--extra' in a else []
    ids = list(range(32))
    if '--courses' in a:
        ids = []
        for part in a[a.index('--courses') + 1].split(','):
            lo, _, hi = part.partition('-')
            ids += list(range(int(lo), int(hi or lo) + 1))
    work = [(c, s) for c in ids for s in range(1, seeds + 1)]
    res = defaultdict(list)
    with cf.ThreadPoolExecutor(jobs) as ex:
        futs = {ex.submit(run_one, exe, root, out, c, s, extra): (c, s) for c, s in work}
        for f in cf.as_completed(futs):
            c, s = futs[f]
            try: r = f.result()
            except Exception as e: r = {'course': c, 'seed': s, 'error': repr(e)}
            res[c].append(r)
    write_report(out, summarize(ids, res))
    print('done', len(work), 'races ->', out / 'clusters.md')


if __name__ == '__main__':
    main()
