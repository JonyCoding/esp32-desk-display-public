"""Build the V3 rear cover and hatch. Run with the manifold3d package installed."""

from math import cos, radians, sin, sqrt

from manifold3d import CrossSection, Manifold

from model import HEIGHT, LID_DEPTH, LID_SCREWS, WIDTH, write_stl


# Assembled rear view: X from the left, Y from the bottom, Z toward the rear.
# Z=0 is the existing front-frame mating plane.
BAY_WIDTH = 119.3
BAY_BOTTOM = 20.0
BAY_PROJECTION = 46.0
BAY_WALL = 3.0
BAY_FLOOR = 4.0
FLOOR_BRACE = 5.0
LEFT = (WIDTH - BAY_WIDTH) / 2
RIGHT = LEFT + BAY_WIDTH
TOP = HEIGHT / 2 + 8
REAR = LID_DEPTH + BAY_PROJECTION
HATCH_THICKNESS = 2.8
HATCH_SEAT = REAR - HATCH_THICKNESS
SEAT_EDGE = 2.8
SEAT_GAP = 0.30
FRONT_EDGE = 5.5
CLEARANCE_M3 = 3.6
PILOT_M3 = 2.8
ESP_PILOT_M3 = 2.75
ESP_STANDOFF = 9.0
ESP_PITCH_X, ESP_PITCH_Z = 47.0, 24.0
# Envelope for checking fit only; the actual board outline can vary.
ESP_FIT_WIDTH, ESP_FIT_DEPTH = 56.0, 38.0
# Install the board component-side down, with its USB-C on the rear-view left.
# Leave half of the previous clearance between the nominal PCB and right wall.
ESP_CENTER_X = (WIDTH / 2 + 10 + RIGHT - BAY_WALL - ESP_FIT_WIDTH / 2) / 2
ESP_CENTER_Z = REAR - 25.5
# The red box in the underside photo is on the right in the rear-view coordinates.
MPU_CENTER_X, MPU_CENTER_Y = 127.0, 146.0
MPU_PITCH = 15.0
MPU_SEAT = 0.0
MPU_PILOT_M3 = 2.85
MPU_PILOT_DEPTH = 5.0
MPU_POCKET_DEPTH = 9.6
MPU_BUMP_Z = 12.4
HATCH_SCREW_X = (LEFT + 8.5, RIGHT - 8.5)
HATCH_SCREW_Y = (BAY_BOTTOM + TOP) / 2
POWER_HOLE_X, POWER_HOLE_DIAMETER = LEFT + 15, 5.0
HATCH_BOTTOM = BAY_BOTTOM + SEAT_EDGE + SEAT_GAP
HATCH_TOP = TOP - SEAT_EDGE - SEAT_GAP
CABLE_Y = HATCH_BOTTOM + 8.0


def block(x0, y0, z0, x1, y1, z1):
    return Manifold.cube((x1 - x0, y1 - y0, z1 - z0)).translate((x0, y0, z0))


def round_z(x, y, z0, z1, diameter):
    return Manifold.cylinder(z1 - z0, diameter / 2, circular_segments=48).translate((x, y, z0))


def round_y(x, y0, y1, z, diameter):
    return (Manifold.cylinder(y1 - y0, diameter / 2, circular_segments=48)
            .rotate((-90, 0, 0)).translate((x, y0, z)))


def printable_pilot_y(x, y0, y1, z, diameter):
    # Keep the nominal thread width and replace the top arc with a 45° roof.
    # The point is +Z in the exported STL, away from the print bed.
    radius = diameter / 2
    arc = [(radius * cos(radians(a)), -radius * sin(radians(a)))
           for a in range(405, 134, -5)]
    profile = CrossSection([arc + [(0, -radius * sqrt(2))]])
    return profile.extrude(y1 - y0).rotate((-90, 0, 0)).translate((x, y0, z))


def build_parts():
    plate = block(0, 0, 0, WIDTH, HEIGHT, LID_DEPTH)
    cover = plate + block(LEFT, BAY_BOTTOM, LID_DEPTH - 0.3, RIGHT, TOP, REAR)
    # The opening goes through the main plate on the display side, leaving
    # continuous walls and a floor for the PCB on the four sides of the rim.
    floor = BAY_BOTTOM + BAY_FLOOR
    ramp_start = HATCH_SEAT - 4.1
    inner = block(LEFT + BAY_WALL, floor, -1,
                  RIGHT - BAY_WALL, TOP - BAY_WALL, ramp_start + 0.1)
    ramp_base = block(LEFT + BAY_WALL, floor, ramp_start - 0.1,
                      RIGHT - BAY_WALL, TOP - BAY_WALL, ramp_start)
    throat = block(LEFT + FRONT_EDGE, BAY_BOTTOM + FRONT_EDGE, HATCH_SEAT - 1.6,
                   RIGHT - FRONT_EDGE, TOP - FRONT_EDGE, REAR + 1)
    # Taper the ledge at the rear opening so it grows gradually on the bed.
    ramp_top = block(LEFT + FRONT_EDGE, BAY_BOTTOM + FRONT_EDGE, HATCH_SEAT - 1.7,
                     RIGHT - FRONT_EDGE, TOP - FRONT_EDGE, HATCH_SEAT - 1.5)
    cover -= inner + (ramp_base + ramp_top).hull() + throat
    cover -= block(LEFT + SEAT_EDGE, BAY_BOTTOM + SEAT_EDGE, HATCH_SEAT,
                   RIGHT - SEAT_EDGE, TOP - SEAT_EDGE, REAR + 1)
    for x, y in LID_SCREWS:
        cover -= round_z(x, y, -1, LID_DEPTH + 1, CLEARANCE_M3)
    # This band sits between the bay and the MPU. Leave a center web to keep
    # the plate continuous across the five long vent rows.
    for row in range(5):
        y = TOP + 9.5 + 6 * row
        for x0, x1 in ((LEFT, WIDTH / 2 - 3.5), (WIDTH / 2 + 3.5, RIGHT)):
            cover -= block(x0, y, -1, x1, y + 3, LID_DEPTH + 1)
    # Reinforce both floor-to-sidewall joints with low 45° ribs. Start behind
    # the mounting plane and stop before the rear rim to leave the opening clear.
    for wall, direction in ((LEFT + BAY_WALL, 1), (RIGHT - BAY_WALL, -1)):
        root_z, end_z = LID_DEPTH + 1, HATCH_SEAT - 4
        inside = wall + direction * FLOOR_BRACE
        outside = wall - direction * 0.25
        foot = block(min(outside, inside), floor - 0.25, root_z,
                     max(outside, inside), floor + 0.1, end_z)
        wall_rib = block(min(outside, wall + direction * 0.1), floor, root_z,
                         max(outside, wall + direction * 0.1), floor + FLOOR_BRACE, end_z)
        cover += (foot + wall_rib).hull()
    board_plane = floor + ESP_STANDOFF
    for dx in (-ESP_PITCH_X / 2, ESP_PITCH_X / 2):
        for dz in (-ESP_PITCH_Z / 2, ESP_PITCH_Z / 2):
            x, z = ESP_CENTER_X + dx, ESP_CENTER_Z + dz
            cover += round_y(x, floor - 0.3, board_plane, z, 7.6)
            # A ramp carries each horizontal boss back to the bay floor.
            foot = block(x - 2.1, floor - 0.25, max(LID_DEPTH + 0.1, z - 10),
                         x + 2.1, floor + 0.1, z + 2.4)
            shoulder = block(x - 2.1, board_plane - 0.35, z + 0.75,
                             x + 2.1, board_plane - 0.08, z + 2.4)
            cover += (foot + shoulder).hull()
            cover -= printable_pilot_y(x, board_plane - 6.5,
                                       board_plane + 0.1, z, ESP_PILOT_M3)
    # Keep the entire screen-facing bed plane flat. The right rear bump is
    # hollow and carries the MPU with flush mounting posts.
    for x in (MPU_CENTER_X, WIDTH - MPU_CENTER_X):
        shoulder = block(x - 15, MPU_CENTER_Y - 14, LID_DEPTH - 0.3,
                         x + 15, MPU_CENTER_Y + 14, MPU_BUMP_Z - 1.3)
        crown = block(x - 13.7, MPU_CENTER_Y - 12.7, MPU_BUMP_Z - 0.25,
                      x + 13.7, MPU_CENTER_Y + 12.7, MPU_BUMP_Z)
        cover += (shoulder + crown).hull()
    pocket = block(MPU_CENTER_X - 12.5, MPU_CENTER_Y - 11.5, -1,
                   MPU_CENTER_X + 12.5, MPU_CENTER_Y + 11.5, MPU_POCKET_DEPTH - 2.9)
    pocket_base = block(MPU_CENTER_X - 12.5, MPU_CENTER_Y - 11.5, MPU_POCKET_DEPTH - 3.1,
                        MPU_CENTER_X + 12.5, MPU_CENTER_Y + 11.5, MPU_POCKET_DEPTH - 3)
    pocket_roof = block(MPU_CENTER_X - 9.5, MPU_CENTER_Y - 8.5, MPU_POCKET_DEPTH - 0.15,
                        MPU_CENTER_X + 9.5, MPU_CENTER_Y + 8.5, MPU_POCKET_DEPTH)
    cover -= pocket + (pocket_base + pocket_roof).hull()
    for dx in (-MPU_PITCH / 2, MPU_PITCH / 2):
        x = MPU_CENTER_X + dx
        cover += round_z(x, MPU_CENTER_Y, MPU_SEAT, MPU_POCKET_DEPTH + 0.3, 6.5)
        cover -= round_z(x, MPU_CENTER_Y, MPU_SEAT - 0.15,
                         MPU_SEAT + MPU_PILOT_DEPTH, MPU_PILOT_M3)
        # A shallow lead-in makes it easier to start the M3 screw after printing.
        lead_in = Manifold.cylinder(0.75, 1.65, MPU_PILOT_M3 / 2, circular_segments=48)
        cover -= lead_in.translate((x, MPU_CENTER_Y, MPU_SEAT - 0.1))

    hatch = block(LEFT + SEAT_EDGE + SEAT_GAP, HATCH_BOTTOM,
                  HATCH_SEAT, RIGHT - SEAT_EDGE - SEAT_GAP, HATCH_TOP, REAR)
    # Both ribs sit above the power hole, which remains fully inside the hatch.
    for y in (HATCH_SCREW_Y - 3, HATCH_SCREW_Y + 7):
        hatch += block(LEFT + 15, y - 1.3, HATCH_SEAT - 1.6,
                       RIGHT - 15, y + 1.3, HATCH_SEAT + 0.2)
    for x, wall_x in zip(HATCH_SCREW_X, (LEFT + 2.5, RIGHT - 2.5)):
        x0, x1 = min(x - 4.4, wall_x), max(x + 4.4, wall_x)
        cover += block(x0, HATCH_SCREW_Y - 4.5, HATCH_SEAT - 6.5,
                       x1, HATCH_SCREW_Y + 4.5, HATCH_SEAT)
        cover -= round_z(x, HATCH_SCREW_Y, HATCH_SEAT - 5.9, REAR + 1, PILOT_M3)
        hatch -= round_z(x, HATCH_SCREW_Y, HATCH_SEAT - 1, REAR + 1, CLEARANCE_M3)
    hatch -= round_z(POWER_HOLE_X, CABLE_Y, HATCH_SEAT - 1, REAR + 1,
                     POWER_HOLE_DIAMETER)
    return cover, hatch


def export(name, solid):
    mesh = solid.to_mesh()
    vertices = [tuple(map(float, row[:3])) for row in mesh.vert_properties]
    write_stl(name, [tuple(vertices[i] for i in tri) for tri in mesh.tri_verts])


if __name__ == "__main__":
    cover, hatch = build_parts()
    assert len(cover.decompose()) == len(hatch.decompose()) == 1
    assert (cover ^ hatch).volume() < 0.001  # No solid interference in the recess.
    assert ESP_CENTER_Z - ESP_FIT_DEPTH / 2 > LID_DEPTH
    assert ESP_CENTER_Z + ESP_FIT_DEPTH / 2 < HATCH_SEAT - 3
    screen_opening = block(LEFT + BAY_WALL + 0.1, BAY_BOTTOM + BAY_FLOOR + 0.1, -0.5,
                           RIGHT - BAY_WALL - 0.1, TOP - BAY_WALL - 0.1, LID_DEPTH + 0.1)
    assert (cover ^ screen_opening).volume() < 0.001
    floor_core = block(LEFT + 0.1, BAY_BOTTOM + 0.2, LID_DEPTH,
                       RIGHT - 0.1, BAY_BOTTOM + BAY_FLOOR - 0.2, HATCH_SEAT - 2)
    assert (floor_core - cover).volume() < 0.001
    for x, y in LID_SCREWS:
        assert (cover ^ round_z(x, y, LID_DEPTH, REAR + 10, 14)).volume() < 0.001
    assert abs(cover.bounding_box()[2]) < 0.00001
    export("back_cover_v3", cover)
    # Put the exterior flat on the print bed, with both stiffening ribs facing up.
    hatch = hatch.rotate((180, 0, 0)).translate((-(LEFT + SEAT_EDGE + SEAT_GAP), HATCH_TOP, REAR))
    export("service_hatch_v3", hatch)
