"""Trace a monochrome reference into the dock's cached native vector geometry.

Requires Pillow. Outputs a transparent SVG and a C++ header; the raster source
is read only. Coordinates use a 24-DIP square with no extra source margin.
"""
import sys
from collections import defaultdict
from pathlib import Path
from PIL import Image


def simplify(points, tolerance=1.0):
    if len(points) <= 2:
        return points
    ax, ay = points[0]
    bx, by = points[-1]
    dx, dy = bx - ax, by - ay
    denominator = dx * dx + dy * dy
    def distance(p):
        t = max(0, min(1, ((p[0]-ax)*dx+(p[1]-ay)*dy)/denominator)) if denominator else 0
        return ((p[0]-ax-t*dx)**2 + (p[1]-ay-t*dy)**2)**0.5
    at = max(range(1, len(points)-1), key=lambda i: distance(points[i]))
    if distance(points[at]) <= tolerance:
        return [points[0], points[-1]]
    return simplify(points[:at+1], tolerance)[:-1] + simplify(points[at:], tolerance)


source, svg_path, header_path = map(Path, sys.argv[1:])
image = Image.open(source).convert('L')
ink = {(x,y) for y in range(image.height) for x in range(image.width) if image.getpixel((x,y)) < 128}
edges = defaultdict(set)
for x,y in ink:
    if (x,y-1) not in ink: edges[(x,y)].add((x+1,y))
    if (x+1,y) not in ink: edges[(x+1,y)].add((x+1,y+1))
    if (x,y+1) not in ink: edges[(x+1,y+1)].add((x,y+1))
    if (x-1,y) not in ink: edges[(x,y+1)].add((x,y))
contours = []
while edges:
    start = min(edges)
    current = start
    previous = (start[0]-1,start[1])
    points = [start]
    while True:
        choices = edges[current]
        dx,dy = current[0]-previous[0],current[1]-previous[1]
        # Prefer a right turn at a diagonal pixel junction.
        directions = [(-dy,dx),(dx,dy),(dy,-dx),(-dx,-dy)]
        order = {v:i for i,v in enumerate(directions)}
        next_point = min(choices, key=lambda p: order.get((p[0]-current[0],p[1]-current[1]),4))
        choices.remove(next_point)
        if not choices: del edges[current]
        previous,current = current,next_point
        if current == start: break
        points.append(current)
    area = abs(sum(a[0]*b[1]-b[0]*a[1] for a,b in zip(points,points[1:]+points[:1])))/2
    if area < 4: continue
    far = max(range(1,len(points)),key=lambda i:(points[i][0]-start[0])**2+(points[i][1]-start[1])**2)
    contours.append(simplify(points[:far+1])[:-1]+simplify(points[far:]+[start])[:-1])
min_x,min_y = min(x for x,y in ink),min(y for x,y in ink)
extent = max(max(x for x,y in ink)-min_x+1,max(y for x,y in ink)-min_y+1)
contours = [[((x-min_x)*24/extent,(y-min_y)*24/extent) for x,y in points] for points in contours]
path = ' '.join('M '+' L '.join(f'{x:.4f},{y:.4f}' for x,y in points)+' Z' for points in contours)
svg_path.parent.mkdir(parents=True,exist_ok=True)
svg_path.write_text('<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 24 24">\n'
                    '<title>MinimaDock crossed swords launcher</title>\n'
                    f'<path fill="currentColor" fill-rule="evenodd" d="{path}"/>\n</svg>\n')
vertices = [p for contour in contours for p in contour]
offsets = [0]
for contour in contours: offsets.append(offsets[-1]+len(contour))
header_path.write_text('// Generated alongside assets/launcher-swords.svg by tools/Trace-LauncherLogo.py.\n'
                      '// Traced from the user-selected reference. No raster resampling at runtime.\n'
                      '#pragma once\n#include <d2d1.h>\n#include <array>\n#include <cstddef>\nnamespace minima::launcher_logo {\n'
                      f'inline constexpr std::array<D2D1_POINT_2F,{len(vertices)}> points = {{{{\n'
                      + ''.join(f'    {{{x:.4f}f,{y:.4f}f}},\n' for x,y in vertices)
                      + '}};\n'
                      + f'inline constexpr std::array<std::size_t,{len(offsets)}> offsets = {{{{'
                      + ','.join(map(str,offsets)) + '}};\n}\n')
print(f'Traced {len(contours)} contours, {len(vertices)} vertices; extent {extent}px -> 24 DIP.')
