"""Microsoft Hover! (1995) .MAZ parser. MFC CArchive stream of a CMerlinWorld.
Usage: py -3 maz.py [file.MAZ ...]   (default: all four in the game's MAZES dir)
"""
import struct, sys, os

MAZES = os.path.join(os.path.dirname(os.path.dirname(os.path.abspath(__file__))), 'game', 'hover', 'MAZES')


class Ar:  # minimal MFC CArchive reader (load side only)
    def __init__(s, b): s.b, s.p, s.classes = b, 0, [None]  # map index 0 = NULL
    def u8(s): s.p += 1; return s.b[s.p - 1]
    def u16(s): s.p += 2; return struct.unpack_from('<H', s.b, s.p - 2)[0]
    def i16(s): s.p += 2; return struct.unpack_from('<h', s.b, s.p - 2)[0]
    def f64(s): s.p += 8; return struct.unpack_from('<d', s.b, s.p - 8)[0]
    def cstr(s):  # MFC CString: BYTE len (0xFF -> WORD len, 0xFFFF -> DWORD len)
        n = s.u8()
        if n == 0xFF:
            n = s.u16()
            assert n != 0xFFFF, 'unicode/dword CString not expected'
        s.p += n; return s.b[s.p - n:s.p].decode('latin-1')
    def skip(s):  # WORD n + n reserved bytes (forward-compat tail used by every Merlin class)
        n = s.u16(); tail = s.b[s.p:s.p + n]; s.p += n; return tail
    def count(s):  # CObList count
        n = s.u16()
        if n != 0xFFFF: return n
        s.p += 4; return struct.unpack_from('<I', s.b, s.p - 4)[0]
    def obj(s):  # CArchive::ReadObject
        tag = s.u16()
        if tag == 0xFFFF:  # new class: schema, name
            schema, n = s.u16(), s.u16()
            name = s.b[s.p:s.p + n].decode(); s.p += n
            s.classes.append(('class', name)); cls = name
        elif tag & 0x8000:
            cls = s.classes[tag & 0x7FFF][1]
        else:
            raise ValueError('back-reference to object #%d at 0x%x (not expected)' % (tag, s.p))
        o = {'cls': cls, 'ofs': s.p}
        READERS[cls](s, o)
        s.classes.append(('obj', o))
        return o


def r_object(a, o): o['name'] = a.cstr(); o['x0'] = a.skip()
def r_line(a, o):
    r_object(a, o); o['x1'], o['y1'], o['x2'], o['y2'] = (a.i16() for _ in range(4)); o['x1tail'] = a.skip()
def r_static(a, o):
    r_line(a, o)
    o['tex'] = [a.cstr() for _ in range(6)]  # +44 +50 +5c +68 +74 +80
    o['w'] = [a.i16() for _ in range(4)]     # +a4 +a6 +a8 +aa
    o['b'] = [a.u8() for _ in range(3)]      # +b0 +b4 +bc
    n = a.u16(); ext = a.b[a.p:a.p + n]; a.p += n  # version-ish tail, 5 in all shipped files
    o['ext'] = (ext[0], *struct.unpack_from('<hh', ext, 1)) if n >= 5 else ()  # +c0 +b8 +ba
def r_location(a, o):
    r_object(a, o); o['x'], o['y'], o['z'], o['a'] = (a.i16() for _ in range(4)); a.skip()
def r_bsp(a, o):
    r_line(a, o); o['n'] = [a.i16() for _ in range(4)]; o['n60'] = a.i16()
    o['d'] = (a.f64(), a.f64()); a.skip()

READERS = {'CMerlinStatic': r_static, 'CMerlinLocation': r_location, 'CMerlinBSP': r_bsp}


def parse(path):
    a = Ar(open(path, 'rb').read())
    w = {'hdr': [a.i16() for _ in range(4)]}  # stored to world +8,+4,+0xc,+0x10
    for k in ('statics', 'dynamics', 'locations', 'bsp'):  # world +0x14 +0x28 +0x3c +0x50
        w[k] = [a.obj() for _ in range(a.count())]
    assert a.p == len(a.b), '%s: %d trailing bytes' % (path, len(a.b) - a.p)
    return w


def render(w, cols=78, maxrows=40):
    xs = [v for s in w['statics'] for v in (s['x1'], s['x2'])]
    ys = [v for s in w['statics'] for v in (s['y1'], s['y2'])]
    x0, x1, y0, y1 = min(xs), max(xs), min(ys), max(ys)
    sc = max((x1 - x0) / (cols - 1), (y1 - y0) / 2 / (maxrows - 1)); rows = int((y1 - y0) / sc / 2) + 1
    g = [[' '] * cols for _ in range(rows)]
    P = lambda x, y: (round((x - x0) / sc), rows - 1 - round((y - y0) / sc / 2))  # +y = up
    for s in w['statics']:
        (a, b), (c, d) = P(s['x1'], s['y1']), P(s['x2'], s['y2'])
        n = max(abs(c - a), abs(d - b), 1)
        if s['w'][1] <= 0: ch = '.'  # zero-height line: floor-texture boundary, not a wall
        else: ch = '-' if abs(c - a) > 2 * abs(d - b) else '|' if abs(d - b) > 2 * abs(c - a) else '/' if (c - a) * (d - b) < 0 else '\\'
        for i in range(n + 1):
            r, q = b + (d - b) * i // n, a + (c - a) * i // n
            if ch != '.' or g[r][q] == ' ': g[r][q] = ch
    for l in w['locations']:
        u, v = P(l['x'], l['y'])
        if 0 <= v < rows and 0 <= u < cols:
            g[v][u] = {'HUMAN_': 'H', 'ROBOT_': 'R', 'FLAG_HUMAN_': 'f', 'FLAG_ROBOT_': 'F', 'POD_RANDOM_': 'p', 'BEACON_': 'b'}.get(l['name'].rstrip('0123456789'), '*')
    return '\n'.join(''.join(r).rstrip() for r in g)


if __name__ == '__main__':
    files = sys.argv[1:] or [os.path.join(MAZES, f) for f in ('SMALL.MAZ', 'MAZE1.MAZ', 'MAZE2.MAZ', 'MAZE3.MAZ')]
    for f in files:
        w = parse(f)
        # self-check: BSP nodes reference valid nodes, every file consumed exactly
        nb = len(w['bsp'])
        assert all(-1 <= c < nb for n in w['bsp'] for c in n['n'][1:3]), 'bad BSP child'
        from collections import Counter
        locs = Counter(l['name'].rstrip('0123456789') for l in w['locations'])
        print('== %s  hdr=%s statics=%d locations=%d bsp=%d dynamics=%d  %s' % (
            os.path.basename(f), w['hdr'], len(w['statics']), len(w['locations']), nb, len(w['dynamics']), dict(locs)))
        print(render(w))
