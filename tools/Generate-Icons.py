"""Generate the quick settings line icons as native Direct2D path data.

Shapes are Lucide icons (https://lucide.dev, ISC licence; see
THIRD_PARTY_NOTICES.md), split into named parts so the dock can animate them
separately. SVG path data is converted to absolute move/line/cubic commands
(arcs and circles become cubic Beziers). Uses only Python's standard library.
"""
import math
import re
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent

# Icon -> parts (each part one or more SVG shapes). 24 x 24 box, drawn as 2-unit
# round-capped strokes.
ICONS = {
    'wifi': {'dot': ['M12 20h.01'], 'bar1': ['M8.5 16.429a5 5 0 0 1 7 0'],
             'bar2': ['M5 12.859a10 10 0 0 1 14 0'], 'bar3': ['M2 8.82a15 15 0 0 1 20 0']},
    'bluetooth': {'rune': ['m7 7 10 10-5 5V2l5 5L7 17']},
    'mic': {'capsule': ['M12 2a3 3 0 0 0-3 3v7a3 3 0 0 0 6 0V5a3 3 0 0 0-3-3Z'],
            'stand': ['M19 10v2a7 7 0 0 1-14 0v-2'], 'foot': ['line 12 19 12 22'],
            'slash': ['line 2 2 22 22']},
    'clapper': {'top': ['M20.2 6 3 11l-.9-2.4c-.3-1.1.3-2.2 1.3-2.5l13.5-4c1.1-.3 2.2.3 2.5 1.3Z',
                        'm6.2 5.3 3.1 3.9', 'm12.4 3.4 3.1 4'],
                'base': ['M3 11h18v8a2 2 0 0 1-2 2H5a2 2 0 0 1-2-2Z']},
    'timer': {'crown': ['line 10 2 14 2'], 'hand': ['line 12 14 15 11'], 'face': ['circle 12 14 8']},
    'refresh': {'arrows': ['M3 12a9 9 0 0 1 9-9 9.75 9.75 0 0 1 6.74 2.74L21 8', 'M21 3v5h-5',
                           'M21 12a9 9 0 0 1-9 9 9.75 9.75 0 0 1-6.74-2.74L3 16', 'M8 16H3v5']},
    'chevron': {'mark': ['m9 18 6-6-6-6']},
    'back': {'mark': ['m12 19-7-7 7-7', 'M19 12H5']},
    'plus': {'mark': ['M5 12h14', 'M12 5v14']},
    'volume': {'speaker': ['M11 4.702a.705.705 0 0 0-1.203-.498L6.413 7.587A1.4 1.4 0 0 1 5.416 8H3a1 1 0 0 0-1 1v6a1 1 0 0 0 1 1h2.416a1.4 1.4 0 0 1 .997.413l3.383 3.384A.705.705 0 0 0 11 19.298z'],
               'wave1': ['M16 9a5 5 0 0 1 0 6'], 'wave2': ['M19.364 18.364a9 9 0 0 0 0-12.728'],
               'mute': ['m22 9-6 6', 'm16 9 6 6']},
    'gear': {'cog': ['M12.22 2h-.44a2 2 0 0 0-2 2v.18a2 2 0 0 1-1 1.73l-.43.25a2 2 0 0 1-2 0l-.15-.08a2 2 0 0 0-2.73.73l-.22.38a2 2 0 0 0 .73 2.73l.15.1a2 2 0 0 1 1 1.72v.51a2 2 0 0 1-1 1.74l-.15.09a2 2 0 0 0-.73 2.73l.22.38a2 2 0 0 0 2.73.73l.15-.08a2 2 0 0 1 2 0l.43.25a2 2 0 0 1 1 1.73V20a2 2 0 0 0 2 2h.44a2 2 0 0 0 2-2v-.18a2 2 0 0 1 1-1.73l.43-.25a2 2 0 0 1 2 0l.15.08a2 2 0 0 0 2.73-.73l.22-.39a2 2 0 0 0-.73-2.73l-.15-.08a2 2 0 0 1-1-1.74v-.5a2 2 0 0 1 1-1.74l.15-.09a2 2 0 0 0 .73-2.73l-.22-.38a2 2 0 0 0-2.73-.73l-.15.08a2 2 0 0 1-2 0l-.43-.25a2 2 0 0 1-1-1.73V4a2 2 0 0 0-2-2z'],
             'hub': ['circle 12 12 3']},
    # Launcher.
    'search': {'lens': ['circle 11 11 8'], 'handle': ['m21 21-4.3-4.3']},
    'lock': {'body': ['rect 3 11 18 11 2'], 'shackle': ['M7 11V7a5 5 0 0 1 10 0v4']},
    'moon': {'crescent': ['M12 3a6 6 0 0 0 9 9 9 9 0 1 1-9-9Z']},
    'restart': {'arrow': ['M3 12a9 9 0 1 0 9-9 9.75 9.75 0 0 0-6.74 2.74L3 8', 'M3 3v5h5']},
    'power': {'line': ['M12 2v10'], 'ring': ['M18.4 6.6a9 9 0 1 1-12.77.04']},
    'signout': {'frame': ['M9 21H5a2 2 0 0 1-2-2V5a2 2 0 0 1 2-2h4'], 'arrow': ['m16 17 5-5-5-5', 'line 21 12 9 12']},
    'zap': {'bolt': ['M4 14a1 1 0 0 1-.78-1.63l9.9-10.2a.5.5 0 0 1 .86.46l-1.92 6.02A1 1 0 0 0 13 10h7a1 1 0 0 1 .78 1.63l-9.9 10.2a.5.5 0 0 1-.86-.46l1.92-6.02A1 1 0 0 0 11 14z']},
    'equal': {'bars': ['line 5 9 19 9', 'line 5 15 19 15']},
}


def arc_to_cubics(x1, y1, rx, ry, phi, large, sweep, x2, y2):
    """SVG elliptical arc (implementation notes F.6.5) as cubic Beziers."""
    if (x1, y1) == (x2, y2):
        return []
    if rx == 0 or ry == 0:
        return [('L', x2, y2)]
    rx, ry = abs(rx), abs(ry)
    cp, sp = math.cos(math.radians(phi)), math.sin(math.radians(phi))
    dx, dy = (x1-x2)/2, (y1-y2)/2
    x1p, y1p = cp*dx+sp*dy, -sp*dx+cp*dy
    lam = (x1p/rx)**2+(y1p/ry)**2
    if lam > 1:
        rx, ry = rx*math.sqrt(lam), ry*math.sqrt(lam)
    num = rx*rx*ry*ry-rx*rx*y1p*y1p-ry*ry*x1p*x1p
    den = rx*rx*y1p*y1p+ry*ry*x1p*x1p
    coef = math.sqrt(max(0, num/den))*(-1 if large == sweep else 1)
    cxp, cyp = coef*rx*y1p/ry, -coef*ry*x1p/rx
    cx, cy = cp*cxp-sp*cyp+(x1+x2)/2, sp*cxp+cp*cyp+(y1+y2)/2

    def angle(ux, uy, vx, vy):
        a = math.atan2(ux*vy-uy*vx, ux*vx+uy*vy)
        return a
    t1 = angle(1, 0, (x1p-cxp)/rx, (y1p-cyp)/ry)
    dt = angle((x1p-cxp)/rx, (y1p-cyp)/ry, (-x1p-cxp)/rx, (-y1p-cyp)/ry)
    if not sweep and dt > 0:
        dt -= 2*math.pi
    elif sweep and dt < 0:
        dt += 2*math.pi
    count = max(1, math.ceil(abs(dt)/(math.pi/2)-1e-9))
    step = dt/count
    k = 4/3*math.tan(step/4)
    out = []
    for i in range(count):
        a1, a2 = t1+i*step, t1+(i+1)*step
        e1x, e1y = math.cos(a1), math.sin(a1)
        e2x, e2y = math.cos(a2), math.sin(a2)
        p = [(e1x-k*e1y, e1y+k*e1x), (e2x+k*e2y, e2y-k*e2x), (e2x, e2y)]
        pts = [(cx+rx*u*cp-ry*v*sp, cy+rx*u*sp+ry*v*cp) for u, v in p]
        out.append(('C', *pts[0], *pts[1], *pts[2]))
    return out


def parse(d):
    """SVG path data -> list of (ops, closed) figures with absolute M/L/C ops."""
    if d.startswith('line '):
        x1, y1, x2, y2 = map(float, d.split()[1:])
        return [([('M', x1, y1), ('L', x2, y2)], False)]
    if d.startswith('rect '):
        x, y, w, h, r = map(float, d.split()[1:])
        ops = [('M', x+r, y), ('L', x+w-r, y)]
        ops += arc_to_cubics(x+w-r, y, r, r, 0, 0, 1, x+w, y+r)+[('L', x+w, y+h-r)]
        ops += arc_to_cubics(x+w, y+h-r, r, r, 0, 0, 1, x+w-r, y+h)+[('L', x+r, y+h)]
        ops += arc_to_cubics(x+r, y+h, r, r, 0, 0, 1, x, y+h-r)+[('L', x, y+r)]
        ops += arc_to_cubics(x, y+r, r, r, 0, 0, 1, x+r, y)
        return [(ops, True)]
    if d.startswith('circle '):
        cx, cy, r = map(float, d.split()[1:])
        ops = [('M', cx+r, cy)]
        for a in range(4):
            ops += arc_to_cubics(*ops[-1][-2:], r, r, 0, 0, 1, *(
                (cx+r*math.cos((a+1)*math.pi/2), cy+r*math.sin((a+1)*math.pi/2))))
        return [(ops, True)]
    tokens = re.findall(r'[MmLlHhVvCcSsQqAaZz]|-?(?:\d*\.\d+|\d+\.?)(?:e-?\d+)?', d)
    figures, ops, closed = [], [], False
    x = y = sx = sy = 0.0
    last_ctrl = None
    i, cmd = 0, None

    def num():
        nonlocal i
        v = float(tokens[i]); i += 1
        return v
    while i < len(tokens):
        if re.fullmatch(r'[A-Za-z]', tokens[i]):
            cmd = tokens[i]; i += 1
        rel = cmd.islower(); c = cmd.upper()
        if c == 'Z':
            closed = True; x, y = sx, sy
            figures.append((ops, True)); ops = []; closed = False
            cmd = None
            continue
        if c == 'M':
            nx, ny = num(), num()
            if rel: nx, ny = x+nx, y+ny
            if ops: figures.append((ops, False))
            ops = [('M', nx, ny)]; x, y = sx, sy = nx, ny
            cmd = 'l' if rel else 'L'
        elif c in 'LHV':
            if c == 'L':
                nx, ny = num(), num()
                if rel: nx, ny = x+nx, y+ny
            elif c == 'H':
                nx = num(); nx = x+nx if rel else nx; ny = y
            else:
                ny = num(); ny = y+ny if rel else ny; nx = x
            ops.append(('L', nx, ny)); x, y = nx, ny
        elif c in 'CS':
            if c == 'C':
                x1, y1 = num(), num()
                if rel: x1, y1 = x+x1, y+y1
            else:
                x1, y1 = (2*x-last_ctrl[0], 2*y-last_ctrl[1]) if last_ctrl else (x, y)
            x2, y2, nx, ny = num(), num(), num(), num()
            if rel: x2, y2, nx, ny = x+x2, y+y2, x+nx, y+ny
            ops.append(('C', x1, y1, x2, y2, nx, ny)); last_ctrl = (x2, y2); x, y = nx, ny
            continue
        elif c == 'A':
            rx, ry, phi, large, sweep, nx, ny = num(), num(), num(), num(), num(), num(), num()
            if rel: nx, ny = x+nx, y+ny
            ops += arc_to_cubics(x, y, rx, ry, phi, int(large), int(sweep), nx, ny); x, y = nx, ny
        else:
            raise ValueError(f'unsupported command {cmd}')
        last_ctrl = None
    if ops: figures.append((ops, False))
    return figures


def f(v):
    return f'{v:.4f}f'.replace('-0.0000f', '0.0000f')


lines = ['// Generated by tools/Generate-Icons.py. Lucide icon shapes (ISC licence,',
         '// https://lucide.dev); see THIRD_PARTY_NOTICES.md.',
         '#pragma once', '#include <cstddef>', '', 'namespace minima::icon_paths {',
         "// op: 'M' move, 'L' line, 'C' cubic (3 points), 'Z' close figure.",
         'struct Op {char op;float v[6];};', 'struct Part {const Op* ops;std::size_t count;};']
index = []
for icon, parts in ICONS.items():
    for part, shapes in parts.items():
        ops = []
        for shape in shapes:
            for figure, closed in parse(shape):
                for op in figure:
                    ops.append(op)
                if closed:
                    ops.append(('Z',))
        name = f'{icon}_{part}'
        body = ','.join('{' + f"'{o[0]}'" + ',{' + ','.join(f(v) for v in o[1:]) + '}}' for o in ops)
        lines.append(f'inline constexpr Op {name}[]={{{body}}};')
        index.append((icon, part, name, len(ops)))
lines.append('')
for icon, parts in ICONS.items():
    names = [n for i, p, n, c in index if i == icon]
    counts = [c for i, p, n, c in index if i == icon]
    lines.append(f'inline constexpr Part {icon}[]={{' + ','.join(f'{{{n},{c}}}' for n, c in zip(names, counts)) + '};')
    lines.append(f'enum class {icon.capitalize()}Part {{' + ','.join(p.capitalize() for p in parts) + '};')
lines.append('}')
(ROOT/'src/rendering/icon_paths.h').write_text('\n'.join(lines)+'\n', encoding='utf-8')
print(f'Generated {len(index)} icon parts for {len(ICONS)} icons.')
