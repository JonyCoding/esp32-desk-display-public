"""Build the stick-on V3 support. Run with manifold3d installed."""

from manifold3d import CrossSection, Manifold

from model import FRAME_DEPTH, STAND_DEPTH, STAND_DROP, STAND_SLOPE, WIDTH, stand_y
from model_back_cover_v3 import BAY_BOTTOM, LEFT, REAR, RIGHT, block, build_parts, export


SUPPORT_WIDTH = 96.0
SUPPORT_DEPTH = 28.0
# Leave 1 mm at the rear edge so the support does not catch the service hatch.
SUPPORT_BACK = REAR - 1.0
SUPPORT_FRONT = SUPPORT_BACK - SUPPORT_DEPTH
SUPPORT_LEFT = (WIDTH - SUPPORT_WIDTH) / 2
ADHESIVE_THICKNESS = 0.2
CORNER_RADIUS = 3.0
PAD_WIDTH = 18.0
CENTER_RELIEF = 1.2


def desk_y(cover_z):
    return stand_y(-STAND_DROP, FRAME_DEPTH + cover_z)


def height_at(cover_z):
    return BAY_BOTTOM - ADHESIVE_THICKNESS - desk_y(cover_z)


def wedge(width, depth, front_height, rear_height):
    # STL axes: X left/right, Y front/rear, Z printed height. The adhesive
    # face is flat at Z=0; the two desk pads face up in the slicer.
    profile = CrossSection([[(0, 0), (depth, 0), (depth, rear_height), (0, front_height)]])
    return profile.extrude(width).transform(((0, 0, 1, 0), (1, 0, 0, 0), (0, 1, 0, 0)))


def build_support():
    front_h, rear_h = height_at(SUPPORT_FRONT), height_at(SUPPORT_BACK)
    shape = wedge(SUPPORT_WIDTH, SUPPORT_DEPTH, front_h, rear_h)
    corners = [
        Manifold.cylinder(front_h + 1, CORNER_RADIUS, circular_segments=32).translate((x, y, 0))
        for x in (CORNER_RADIUS, SUPPORT_WIDTH - CORNER_RADIUS)
        for y in (CORNER_RADIUS, SUPPORT_DEPTH - CORNER_RADIUS)
    ]
    shape ^= Manifold.batch_hull(corners)

    gap = SUPPORT_WIDTH - 2 * PAD_WIDTH
    relief_bottom = wedge(gap, SUPPORT_DEPTH + 2,
                          front_h - CENTER_RELIEF + STAND_SLOPE,
                          rear_h - CENTER_RELIEF - STAND_SLOPE)
    relief = block(0, 0, 0, gap, SUPPORT_DEPTH + 2, front_h + 1) - relief_bottom
    return shape - relief.translate((PAD_WIDTH, -1, 0))


if __name__ == "__main__":
    support = build_support()
    assert LEFT < SUPPORT_LEFT < SUPPORT_LEFT + SUPPORT_WIDTH < RIGHT
    assert FRAME_DEPTH + SUPPORT_FRONT > STAND_DEPTH
    assert SUPPORT_BACK < REAR
    assert abs((height_at(SUPPORT_FRONT) - height_at(SUPPORT_BACK)) /
               SUPPORT_DEPTH - STAND_SLOPE) < 1e-8
    placed = support.rotate((90, 0, 0))
    placed = placed.translate((SUPPORT_LEFT, BAY_BOTTOM - ADHESIVE_THICKNESS, SUPPORT_FRONT))
    cover, hatch = build_parts()
    assert ((cover + hatch) ^ placed).volume() < 0.001
    for x, y, z in placed.to_mesh().vert_properties[:, :3]:
        assert y >= desk_y(z) - 0.00001
    export("rear_support_v3", support)
