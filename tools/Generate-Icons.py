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
    # Quick settings editing and the extra tiles.
    'pencil': {'mark': ['M21.174 6.812a1 1 0 0 0-3.986-3.987L3.842 16.174a2 2 0 0 0-.5.83l-1.321 4.352a.5.5 0 0 0 .623.622l4.353-1.32a2 2 0 0 0 .83-.497z', 'm15 5 4 4']},
    'minus': {'mark': ['M5 12h14']},
    'monitor': {'screen': ['rect 2 3 20 14 2'], 'stand': ['line 8 21 16 21', 'line 12 17 12 21']},
    'plane': {'body': ['M17.8 19.2 16 11l3.5-3.5C21 6 21.5 4 21 3c-1-.5-3 0-4.5 1.5L13 8 4.8 6.2c-.5-.1-.9.1-1.1.5l-.3.5c-.2.5-.1 1 .3 1.3L9 12l-2 3H4l-1 1 3 2 2 3 1-1v-3l3-2 3.5 5.3c.3.4.8.5 1.3.3l.5-.2c.4-.3.6-.7.5-1.2z']},
    'tower': {'mast': ['circle 12 9 2', 'M9.5 18h5', 'm8 22 4-11 4 11'],
              'inner': ['M7.8 4.7a6.14 6.14 0 0 0-.8 7.5', 'M16.2 4.8c2 2 2.26 5.11.8 7.47'],
              'outer': ['M4.9 16.1C1 12.2 1 5.8 4.9 1.9', 'M19.1 1.9a9.96 9.96 0 0 1 0 14.1']},
    'contrast': {'ring': ['circle 12 12 10'], 'half': ['M12 18a6 6 0 0 0 0-12v12z']},
    'captions': {'frame': ['rect 3 5 18 14 2'], 'lines': ['M7 15h4', 'M15 15h2', 'M7 11h2', 'M13 11h4']},
    'keyboard': {'frame': ['rect 2 4 20 16 2'], 'keys': ['M6 8h.01', 'M10 8h.01', 'M14 8h.01', 'M18 8h.01', 'M8 12h.01', 'M12 12h.01', 'M16 12h.01', 'M7 16h10']},
    'cast': {'frame': ['M2 8V6a2 2 0 0 1 2-2h16a2 2 0 0 1 2 2v12a2 2 0 0 1-2 2h-6'], 'waves': ['M2 12a9 9 0 0 1 8 8', 'M2 16a5 5 0 0 1 4 4', 'line 2 20 2.01 20']},
    'leaf': {'leaf': ['M11 20A7 7 0 0 1 9.8 6.1C15.5 5 17 4.48 19 2c1 2 2 4.18 2 8 0 5.5-4.78 10-10 10Z', 'M2 21c0-3 1.85-5.36 5.08-6C9.5 14.52 12 13 13 12']},
    'shield': {'shield': ['M20 13c0 5-3.5 7.5-7.66 8.95a1 1 0 0 1-.67-.01C7.5 20.5 4 18 4 13V6a1 1 0 0 1 1-1c2 0 4.5-1.2 6.24-2.72a1.17 1.17 0 0 1 1.52 0C14.51 3.81 17 5 19 5a1 1 0 0 1 1 1z']},
    'share': {'dots': ['circle 18 5 3', 'circle 6 12 3', 'circle 18 19 3'], 'links': ['line 8.59 13.51 15.42 17.49', 'line 15.41 6.51 8.59 10.49']},
    'access': {'figure': ['circle 16 4 1', 'm18 19 1-7-6 1', 'm5 8 3-3 5.5 3-2.36 3.5', 'M4.24 14.5a5 5 0 0 0 6.88 6', 'M13.76 17.5a5 5 0 0 0-6.88-6']},
    'blend': {'circles': ['circle 9 9 7', 'circle 15 15 7']},
    'belloff': {'bell': ['M10.268 21a2 2 0 0 0 3.464 0', 'M17 17H4a1 1 0 0 1-.74-1.673C4.59 13.956 6 12.499 6 8a6 6 0 0 1 .258-1.742', 'M8.668 3.01A6 6 0 0 1 18 8c0 2.687.77 4.653 1.707 6.05'], 'slash': ['m2 2 20 20']},
    'sun': {'disc': ['circle 12 12 4'], 'rays': ['M12 2v2', 'M12 20v2', 'm4.93 4.93 1.41 1.41', 'm17.66 17.66 1.41 1.41', 'M2 12h2', 'M20 12h2', 'm6.34 17.66-1.41 1.41', 'm19.07 4.93-1.41 1.41']},
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
