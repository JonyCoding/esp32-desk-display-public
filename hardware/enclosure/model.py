"""Generate the current front frame and its editable OpenSCAD geometry in millimetres."""
from collections import defaultdict
from pathlib import Path
import math
import struct

OUT = Path(__file__).resolve().parent
PANEL_W = PANEL_H = 160.0
PANEL_DEPTH = 15.0
FRONT_LIP = 1.0
SEAT_Z = PANEL_DEPTH + FRONT_LIP
SIDE_CLEARANCE = 0.25
# Keep the outside, lid and foot footprint of the original frame.
FRAME_MARGIN = 5.15
FRAME_WALL = FRAME_MARGIN - SIDE_CLEARANCE
FRAME_DEPTH = 42.0
LID_DEPTH = 3.0
WIDTH = PANEL_W + 2 * FRAME_MARGIN
HEIGHT = PANEL_H + 2 * FRAME_MARGIN
LID_SCREWS = [(x, y) for x in (7.5, WIDTH-7.5) for y in (7.5, HEIGHT-7.5)]

# Horizontal spacing measured on the panel; vertical spacing is approximate.
MOUNT_PITCH_X = 90
MOUNT_PITCH_Y = 146
RAIL_WIDTH = 120
RAIL_REACH = 22
RAIL_THICKNESS = 4
PAD_SIZE = 13
# The installed stock magnetic post leaves a 9 mm gap under its disc.
# Make the stack 0.4 mm thicker so the disc clamps before the post stops.
PAD_EXTRA = 5.4
SLOT_WIDTH = 6.2
SLOT_LENGTH = 7.5
LOW_SLOT_Y = (HEIGHT-MOUNT_PITCH_Y)/2
STAND_ANGLE = 7
STAND_SLOPE = math.tan(math.radians(STAND_ANGLE))
# Keep the underside clear near the rear cable exit at the steeper angle.
STAND_DROP = 12
STAND_DEPTH = FRAME_DEPTH + LID_DEPTH + 5
STAND_FEET = (6, WIDTH-32)
STAND_WIDTH = 26

def box(x, y, z, w, h, d):
    return (x, y, z), (x+w, y+h, z+d)

def mount_pads(mid_x, center_y, seat_z):
    return [
        box(x-PAD_SIZE/2, center_y-PAD_SIZE/2, seat_z+RAIL_THICKNESS,
            PAD_SIZE, PAD_SIZE, PAD_EXTRA)
        for x in (mid_x-MOUNT_PITCH_X/2, mid_x+MOUNT_PITCH_X/2)
    ]


def mount_slots(mid_x, center_y, seat_z):
    return [
        box(x-SLOT_WIDTH/2, center_y-SLOT_LENGTH/2, seat_z-1,
            SLOT_WIDTH, SLOT_LENGTH, RAIL_THICKNESS+PAD_EXTRA+2)
        for x in (mid_x-MOUNT_PITCH_X/2, mid_x+MOUNT_PITCH_X/2)
    ]

def frame():
    solid = [box(0, 0, 0, WIDTH, HEIGHT, FRAME_DEPTH)]
    cuts = [box(FRAME_WALL, FRAME_WALL, -1, WIDTH-2*FRAME_WALL,
                HEIGHT-2*FRAME_WALL, FRAME_DEPTH+2)]
    supports = [
        box((WIDTH-RAIL_WIDTH)/2, y, SEAT_Z, RAIL_WIDTH, RAIL_REACH, RAIL_THICKNESS)
        for y in (0, HEIGHT-RAIL_REACH)
    ]
    for x, y in LID_SCREWS:
        # Each boss overlaps both adjacent walls, behind the panel.
        x0 = 4 if x < WIDTH/2 else WIDTH-12
        y0 = 4 if y < HEIGHT/2 else HEIGHT-12
        supports.append(box(x0, y0, SEAT_Z+1, 8, 8, FRAME_DEPTH-SEAT_Z-1))
    holes = [box(x-1.3, y-1.3, FRAME_DEPTH-9, 2.6, 2.6, 10)
             for x, y in LID_SCREWS]
    for y in (LOW_SLOT_Y, HEIGHT-LOW_SLOT_Y):
        supports.extend(mount_pads(WIDTH/2, y, SEAT_Z))
        holes.extend(mount_slots(WIDTH/2, y, SEAT_Z))
    return solid, cuts, supports, holes

def stand_blocks():
    return [box(x, -STAND_DROP, 0, STAND_WIDTH, STAND_DROP, STAND_DEPTH)
            for x in STAND_FEET]

def stand_y(y, z):
    # The two desk-contact planes rise toward the rear in frame coordinates.
    return y + (-y/STAND_DROP)*STAND_SLOPE*z if y < 0 else y

def covered(point, boxes):
    return any(all(lo[a] < point[a] < hi[a] for a in range(3)) for lo, hi in boxes)

def mesh(groups, shear, integrated_stand=False):
    if integrated_stand:
        groups = groups[0], groups[1], groups[2]+stand_blocks(), groups[3]
    axes = [sorted({round(p[a], 6) for boxes in groups for pair in boxes for p in pair})
            for a in range(3)]
    intervals = [[(a+b)/2 for a,b in zip(axis, axis[1:])] for axis in axes]
    cells = set()
    for i, x in enumerate(intervals[0]):
        for j, y in enumerate(intervals[1]):
            for k, z in enumerate(intervals[2]):
                pt = x,y,z
                if ((covered(pt, groups[0]) and not covered(pt, groups[1]))
                    or covered(pt, groups[2])) and not covered(pt, groups[3]):
                    cells.add((i,j,k))
    face_corners = [
        ((0,0,0),(0,0,1),(0,1,1),(0,1,0)),
        ((1,0,0),(1,1,0),(1,1,1),(1,0,1)),
        ((0,0,0),(1,0,0),(1,0,1),(0,0,1)),
        ((0,1,0),(0,1,1),(1,1,1),(1,1,0)),
        ((0,0,0),(0,1,0),(1,1,0),(1,0,0)),
        ((0,0,1),(1,0,1),(1,1,1),(0,1,1)),
    ]
    dirs = [(-1,0,0),(1,0,0),(0,-1,0),(0,1,0),(0,0,-1),(0,0,1)]
    for cell in sorted(cells):
        for direction, corners in zip(dirs, face_corners):
            if tuple(a+b for a,b in zip(cell,direction)) in cells:
                continue
            verts = []
            for corner in corners:
                x,y,z = (axes[a][cell[a]+corner[a]] for a in range(3))
                if integrated_stand:
                    y = stand_y(y, z)
                verts.append((x, round(y+shear*z, 6), z))
            yield (verts[0],verts[1],verts[2])
            yield (verts[0],verts[2],verts[3])

def write_stl(name, triangles):
    edges = defaultdict(list)
    for i, tri in enumerate(triangles):
        for a,b in zip(tri, tri[1:]+tri[:1]):
            edges[tuple(sorted((a,b)))].append(i)
    assert all(len(pair)==2 for pair in edges.values()), name
    neighbors = defaultdict(set)
    for a,b in edges.values():
        neighbors[a].add(b)
        neighbors[b].add(a)
    seen, pending = set(), [0]
    while pending:
        i = pending.pop()
        if i not in seen:
            seen.add(i)
            pending.extend(neighbors[i])
    assert len(seen)==len(triangles), name
    verts = {v for triangle in triangles for v in triangle}
    spans = [max(p[a] for p in verts)-min(p[a] for p in verts) for a in range(3)]
    assert max(spans) < 256, name
    with (OUT / (name+'.stl')).open('wb') as f:
        f.write(name.encode().ljust(80,b'\0'))
        f.write(struct.pack('<I',len(triangles)))
        for a,b,c in triangles:
            u = [b[i]-a[i] for i in range(3)]
            v = [c[i]-a[i] for i in range(3)]
            cross = [u[1]*v[2]-u[2]*v[1],u[2]*v[0]-u[0]*v[2],u[0]*v[1]-u[1]*v[0]]
            length = math.sqrt(sum(value**2 for value in cross))
            assert length > 0
            f.write(struct.pack('<12fH',*(value/length for value in cross),*a,*b,*c,0))
    print(f'{name}: '+ ' x '.join(f'{span:.2f}' for span in spans)+' mm; closed and connected')

def scad_part(name, groups, shear, integrated_stand=False):
    def shapes(boxes):
        return '\n'.join(f'translate({list(lo)}) cube({[round(b-a,6) for a,b in zip(lo,hi)]});'
                         for lo,hi in boxes)
    a,b,c,d = map(shapes, groups)
    if integrated_stand:
        for (x0,y0,z0),(x1,y1,z1) in stand_blocks():
            points = [[x, stand_y(y,z), z] for x in (x0,x1)
                      for y,z in ((y0,z0),(y1,z0),(y1,z1),(y0,z1))]
            faces = [[0,1,2,3],[7,6,5,4],[4,5,1,0],
                     [5,6,2,1],[6,7,3,2],[7,4,0,3]]
            c += f'\npolyhedron(points={points}, faces={faces});'
    part = f'''module {name}() {{
  multmatrix([[1,0,0,0],[0,1,{shear},0],[0,0,1,0],[0,0,0,1]])
    difference() {{
      union() {{
        difference() {{ union() {{ {a} }} union() {{ {b} }} }}
        {c}
      }}
      union() {{ {d} }}
    }}
}}
'''
    return '\n'.join(line.rstrip() for line in part.splitlines()) + '\n'

if __name__ == '__main__':
    assert FRAME_WALL < LOW_SLOT_Y-PAD_SIZE/2 < LOW_SLOT_Y+PAD_SIZE/2 < RAIL_REACH
    assert 0 < SLOT_WIDTH < SLOT_LENGTH < PAD_SIZE
    assert STAND_DROP > STAND_SLOPE * STAND_DEPTH
    groups = frame()
    write_stl('front_frame', list(mesh(groups, 0, integrated_stand=True)))
    scad = '// Generated from model.py. Current front frame with integrated stand.\n'
    scad += scad_part('front_frame', groups, 0, integrated_stand=True)
    (OUT / 'enclosure.scad').write_text(scad + 'front_frame();\n')
