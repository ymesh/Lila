"""Builds the Saltmarket example: a desert-town blockout for a two-site attack and defend map, made entirely of
Blockwork blocks and hotspotted live onto two trim sheets (stone in several palettes, and wood), so every block stays
textured as you edit it.

Run from the script editor, or `lilabatch --batch build_saltmarket.py [out_dir]`. Writes saltmarket.lila and the sheets
(TrimSheets/*.lilatrim with their textures) next to this file, or into out_dir. Units are cm, Y up; the attackers start
in the south (+z), the defenders in the north.
"""

import os
import random
import sys

from lila import ops

here = os.path.dirname(os.path.abspath(__file__)) if "__file__" in globals() else os.getcwd()
out_dir = sys.argv[1] if len(sys.argv) > 1 else here

SIZE = 4800  # the square the layout below is drawn in, cm
PLAN = 2.0  # the layout's ground plan is scaled by this; heights and details stay as given, so the map is 96 m across
SITE_A_HEIGHT = 192  # A site's raised platform
PX_PER_M = 128
BEVEL = 3.0  # cm on every exposed edge; 0 for none
SEED = 3

# The stone sheet's palettes: (name, smart material). Default is plain sandstone.
STONE_MATERIAL = "Trim Sandstone"
PALETTES = [("Brick", "Stylized Castle Brick"), ("Plaster", "Trim Painted Concrete"), ("Tiles", "Stylized Glazed Tiles"),
            ("Cobble", "Stylized Cobblestone")]
WOOD_MATERIAL = "Trim Wood"
# The streets: square panels of sand, large so the sand doesn't read as tiles.
GROUND_MATERIAL = "Beach Sand"
GROUND_PX_PER_M = 32

# Buildings on the plan: (name, x0, z0, x1, z1, height, palette). Each gets a plinth, a cornice, a parapet, shuttered
# windows, a door with an awning and roof beams.
HOUSES = [
    ("WestBlock", 0, 1900, 1500, 3500, 640, "Brick"), ("MidWestBlock", 1600, 1500, 2000, 3600, 560, "Plaster"),
    ("MidEastBlock", 2800, 2100, 3300, 3700, 600, "Default"), ("EastBlock", 3800, 2300, 4800, 3700, 520, "Plaster"),
    ("NorthHouse", 2000, 0, 3000, 500, 720, "Brick"), ("BackHouseB", 0, 0, 400, 1800, 700, "Default"),
    ("BackHouseA", 4400, 0, 4800, 2000, 700, "Brick"), ("Courtyard", 3300, 2700, 3800, 3700, 480, "Plaster"),
]
# Plain walls on the plan: (name, x0, z0, x1, z1, height, palette).
WALLS = [
    ("SpawnWallW", 0, 4000, 1300, 4200, 400, "Default"), ("SpawnWallE", 3500, 4000, 4800, 4200, 400, "Default"),
    ("BWallMid", 1500, 300, 1600, 1500, 480, "Plaster"), ("BWallNorth", 400, 0, 1500, 300, 480, "Plaster"),
]
# Crates: (name, x, z, size, stack height in crates), on the plan.
CRATES = [("CrateB1", 700, 900, 96, 2), ("CrateB2", 796, 900, 96, 1), ("CrateB3", 1250, 1450, 128, 1),
          ("CrateA1", 3600, 1000, 128, 1), ("CrateA2", 3900, 700, 96, 2), ("CrateMid", 2250, 1800, 96, 1),
          ("CrateB4", 600, 1300, 128, 2), ("CrateB5", 1000, 500, 96, 1), ("CrateB6", 1064, 500, 96, 2),
          ("CrateA3", 4100, 1450, 128, 2), ("CrateA4", 3460, 650, 96, 1), ("CrateA5", 3800, 1600, 96, 1),
          ("CrateMid2", 2550, 3000, 128, 1), ("CrateEast", 3500, 2000, 96, 2), ("CrateSpawn", 2300, 3900, 128, 1)]

rng = random.Random(SEED)
groups = {}  # (sheet, palette) -> blocks


def plan(x, y, z):
    return [x * PLAN, y, z * PLAN]


def block(name, lo, size, look=("stone", "Default"), **flags):
    """A block from its low corner and size in world cm, hotspotted later onto look (sheet, palette)."""
    b = ops.blockworkBlock(origin=list(lo), size=list(size), name=name, **flags)
    groups.setdefault(look, []).append(b)
    return b


def box(name, x0, z0, x1, z1, height, y0=0.0, look=("stone", "Default"), **flags):
    """A block on the plan: x and z are scaled by PLAN, heights are not."""
    return block(name, plan(x0, y0, z0), [(x1 - x0) * PLAN, (z1 - z0) * PLAN, height], look, **flags)


def carve(name, lo, size, into):
    ops.blockworkCarve(ops.blockworkBlock(origin=list(lo), size=list(size), name=name), blocks=[into])


# Everything a house wall can butt on the plan: (x0, z0, x1, z1). Its decorations only go where nothing does.
OBSTACLES = [h[1:5] for h in HOUSES] + [w[1:5] for w in WALLS] + [
    (-16, -16, SIZE + 16, 0), (-16, SIZE, SIZE + 16, SIZE + 16), (-16, 0, 0, SIZE), (SIZE, 0, SIZE + 16, SIZE),  # perimeter
    (2000, 2400, 2800, 2464), (400, 1800, 1500, 1900), (1284, 3500, 1300, 4000), (1500, 3500, 1516, 4000),  # gate, tunnel
    (3200, 500, 4400, 1800), (3800, 1800, 4200, 2300)]  # A platform and stairs


def exposed(rect, side, margin=0.0):
    """Stretches of a side ("N", "S", "E" or "W") of a plan rectangle that nothing else butts, as (lo, hi) along it in plan
    units, each end pulled in by margin."""
    x0, z0, x1, z1 = rect
    lo, hi = (x0, x1) if side in "NS" else (z0, z1)
    covered = []
    for o in OBSTACLES:
        if tuple(o) == tuple(rect):
            continue
        ox0, oz0, ox1, oz1 = o
        if side in "NS":
            touches = abs((oz0 if side == "S" else oz1) - (z1 if side == "S" else z0)) < 1e-6
            covered.append((ox0, ox1)) if touches else None
        else:
            touches = abs((ox0 if side == "E" else ox1) - (x1 if side == "E" else x0)) < 1e-6
            covered.append((oz0, oz1)) if touches else None
    out, at = [], lo
    for a, b in sorted(covered):
        if b <= at or a >= hi:
            continue
        if a > at:
            out.append((at, a))
        at = max(at, b)
    if at < hi:
        out.append((at, hi))
    return [(a + margin, b - margin) for a, b in out if b - a > 2 * margin]


def at_side(side, rect, t):
    """World x and z of the point t (plan units) along a side of a plan rectangle, on the wall."""
    x0, z0, x1, z1 = rect
    return {"S": (t * PLAN, z1 * PLAN), "N": (t * PLAN, z0 * PLAN), "E": (x1 * PLAN, t * PLAN), "W": (x0 * PLAN, t * PLAN)}[side]


def proud(name, side, rect, t, along, width, y, height, out0, depth, look=("stone", "Default"), **flags):
    """A block on the outside of a side: width cm along it centred along cm from t, depth cm deep starting out0 cm out."""
    x, z = at_side(side, rect, t)
    if side in "NS":
        zz = z + out0 if side == "S" else z - out0 - depth
        lo, size = (x + along - width / 2, y, zz), (width, depth, height)
    else:
        xx = x + out0 if side == "E" else x - out0 - depth
        lo, size = (xx, y, z + along - width / 2), (depth, width, height)
    return block(name, lo, size, look, **flags)


def free(px, pz):
    """Nothing on the plan covers the point."""
    return not any(o[0] < px < o[2] and o[1] < pz < o[3] for o in OBSTACLES)


def open_corner(rect, side, at_end):
    """Whether a north or south band may run on round the corner at its start or end: the east or west side is open
    there and nothing stands diagonally off the corner."""
    x0, z0, x1, z1 = rect
    corner_x, corner_z = (x1, z1 if side == "S" else z0) if at_end else (x0, z1 if side == "S" else z0)
    beyond = (1.0 if at_end else -1.0, 1.0 if side == "S" else -1.0)
    if not free(corner_x + beyond[0], corner_z + beyond[1]):
        return False
    return any(abs((b if side == "S" else a) - corner_z) < 1e-6 for a, b in exposed(rect, "E" if at_end else "W"))


def house(name, x0, z0, x1, z1, height, palette):
    rect = (x0, z0, x1, z1)
    wx0, wz0, wx1, wz1 = x0 * PLAN, z0 * PLAN, x1 * PLAN, z1 * PLAN
    block(name, (wx0, 0, wz0), (wx1 - wx0, wz1 - wz0, height), ("stone", palette))
    # A parapet inside the roof's edge, so it never reaches into a neighbour.
    for side, lo, size in [("N", (wx0, height, wz0), (wx1 - wx0, 24, 64)), ("S", (wx0, height, wz1 - 24), (wx1 - wx0, 24, 64)),
                           ("W", (wx0, height, wz0 + 24), (24, wz1 - wz0 - 48, 64)), ("E", (wx1 - 24, height, wz0 + 24), (24, wz1 - wz0 - 48, 64))]:
        block(f"{name}Parapet{side}", lo, size)

    # Shuttered windows evenly along each open stretch; the house's door takes one ground floor slot, on the first side
    # with room, south first. Beams run under the cornice between the south windows.
    shutters = ("stone", "Tiles") if rng.random() < 0.5 else ("wood", "Default")
    floors = [180] + ([420] if height >= 600 else [])
    door = None
    for side in "SNEW":
        for i, (a, b) in enumerate(exposed(rect, side, 60)):
            count = int((b - a) * PLAN // 420)
            if count < 1:
                continue
            slot = rng.randrange(count) if door is None else -1
            for k in range(count):
                t = a + (k + 0.5) * (b - a) / count
                for y in floors:
                    if k == slot and y == floors[0]:
                        door = (side, t)
                        proud(name + "DoorFrame", side, rect, t, 0, 192, 0, 288, 0, 12)
                        proud(name + "Door", side, rect, t, 0, 144, 0, 252, 12, 8, ("wood", "Default"))
                        proud(name + "Awning", side, rect, t, 0, 280, 300, 36, 0, 96, ("stone", "Tiles"), shape="wedge",
                              direction={"S": 0, "N": 2, "E": 3, "W": 1}[side])
                        continue
                    proud(f"{name}Win{side}{i}_{k}_{y}Sill", side, rect, t, 0, 128, y - 16, 16, 0, 16)
                    proud(f"{name}Win{side}{i}_{k}_{y}Lintel", side, rect, t, 0, 128, y + 128, 20, 0, 12)
                    proud(f"{name}Win{side}{i}_{k}_{y}ShutterL", side, rect, t, -26, 48, y, 128, 0, 8, shutters)
                    proud(f"{name}Win{side}{i}_{k}_{y}ShutterR", side, rect, t, 26, 48, y, 128, 0, 8, shutters)
                if side == "S" and k + 1 < count:
                    proud(f"{name}Beam{i}_{k}", side, rect, a + (k + 1) * (b - a) / count, 0, 24, height - 96, 24, 0, 56, ("wood", "Default"))

    # A plinth (broken for the door) and a cornice along each open stretch. North and south bands run on round an open
    # corner; east and west bands stop at the wall line there, and short of an inner corner, so bands never overlap.
    for side in "SNEW":
        full = (x0, x1) if side in "NS" else (z0, z1)
        for part, y, h, d in (("Plinth", 0, 48, 8), ("Cornice", height - 32, 32, 16)):
            stretches = exposed(rect, side)
            if part == "Plinth" and door and door[0] == side:
                gap = 96 / PLAN
                stretches = [piece for a, b in stretches for piece in ((a, min(b, door[1] - gap)), (max(a, door[1] + gap), b)) if piece[1] - piece[0] > 1e-6]
            for i, (a, b) in enumerate(stretches):
                at_start, at_end = abs(a - full[0]) < 1e-6, abs(b - full[1]) < 1e-6
                if side in "NS":
                    ends = (d if at_start and open_corner(rect, side, False) else 0.0, d if at_end and open_corner(rect, side, True) else 0.0)
                else:
                    ends = (0.0 if at_start else -d, 0.0 if at_end else -d)
                length = (b - a) * PLAN + ends[0] + ends[1]
                if length > 1.0:
                    proud(f"{name}{part}{side}{i}", side, rect, a, length / 2 - ends[0], length, y, h, 0, d)


ops.newScene()
# Saved first so the sheets and their textures are stored relative to the scene.
ops.saveScene(os.path.join(out_dir, "saltmarket.lila"))
sheets = {name: os.path.join(out_dir, "TrimSheets", f"T_Saltmarket{suffix}.lilatrim") for name, suffix in (("stone", ""), ("wood", "Wood"), ("ground", "Ground"))}
ops.trimSheetCreate(preset="monumentalStone", name="T_Saltmarket", width=1024, pxPerM=PX_PER_M, file=sheets["stone"], overwrite=True)
ops.trimSheetTexture(sheet=sheets["stone"], material=STONE_MATERIAL, replace=True)
for name, material in PALETTES:
    ops.trimPaletteAdd(name, sheet=sheets["stone"], material=material)
    ops.trimSheetTexture(sheet=sheets["stone"], palette=name, material=material, replace=True)
ops.trimSheetCreate(preset="woodTrims", name="T_SaltmarketWood", width=1024, pxPerM=PX_PER_M, file=sheets["wood"], overwrite=True)
ops.trimSheetTexture(sheet=sheets["wood"], material=WOOD_MATERIAL, replace=True)
ops.trimSheetCreate(preset="hotspotGrid", name="T_SaltmarketGround", width=1024, pxPerM=GROUND_PX_PER_M, file=sheets["ground"], overwrite=True)
ops.trimSheetTexture(sheet=sheets["ground"], material=GROUND_MATERIAL, replace=True)

box("Ground", 0, 0, SIZE, SIZE, -32, look=("ground", "Default"))
for name, x0, z0, x1, z1 in [("PerimeterN", -16, -16, SIZE + 16, 0), ("PerimeterS", -16, SIZE, SIZE + 16, SIZE + 16),
                             ("PerimeterW", -16, 0, 0, SIZE), ("PerimeterE", SIZE, 0, SIZE + 16, SIZE)]:
    box(name, x0, z0, x1, z1, 800)
for h in HOUSES:
    house(*h)
walls = {}
for name, x0, z0, x1, z1, height, palette in WALLS:
    walls[name] = box(name, x0, z0, x1, z1, height, look=("stone", palette))
    box(name + "Cap", x0, z0, x1, z1, 24, y0=height)

# Mid: a long street closed halfway by a wall with an arched gate, so the long sightline is cut to the arch.
box("MidGate", 2000, 2400, 2800, 2464, 560, shape="arch", thickness=160, sides=16)
box("MidGateCornice", 2000, 2400, 2800, 2464, 40, y0=560)

# West lane: a covered tunnel from the attackers' side up to B, side walls and a timber roof over the lane.
box("TunnelWallW", 1500, 3500, 1516, 4000, 320, look=("stone", "Plaster"))
box("TunnelWallE", 1284, 3500, 1300, 4000, 320, look=("stone", "Plaster"))
box("TunnelRoof", 1284, 3500, 1516, 4000, 24, y0=320, look=("wood", "Default"))
box("TunnelNorthWall", 400, 1800, 1500, 1900, 400, look=("stone", "Plaster"))
box("TunnelNorthRoof", 400, 1800, 1600, 1900, 24, y0=400, look=("wood", "Default"))

# B site: a walled courtyard paved in cobbles, with a doorway from mid.
# Inset from the house on its west side by that house's plinth.
box("BPaving", 404, 300, 1500, 1800, 4, look=("stone", "Cobble"))
carve("BDoorCut", plan(1490, 0, 750), [140 * PLAN, 100 * PLAN, 300], walls["BWallMid"])

# A site: a raised paved platform, reached by stairs from the east lane and a ramp from mid, with a colonnade.
box("APlatform", 3200, 500, 4400, 1800, SITE_A_HEIGHT)
box("APaving", 3200, 500, 4400, 1800, 4, y0=SITE_A_HEIGHT, look=("stone", "Cobble"))
box("AStairs", 3800, 1800, 4200, 2300, SITE_A_HEIGHT, shape="stairs", steps=12, direction=2)
box("ARamp", 2800, 900, 3200, 1500, SITE_A_HEIGHT, shape="wedge", direction=1)
for k, z in enumerate((650, 1000, 1350)):
    block(f"AColumn{k}", plan(3350, SITE_A_HEIGHT + 4, z), [100, 100, 440], shape="cylinder", sides=16)
box("AArchitrave", 3330, 600, 3420, 1450, 48, y0=SITE_A_HEIGHT + 444)

for name, x, z, size, stack in CRATES:
    base = SITE_A_HEIGHT + 4 if 3200 <= x <= 4400 and 500 <= z <= 1800 else (4.0 if 400 <= x <= 1500 and 300 <= z <= 1800 else 0.0)
    for k in range(stack):
        block(f"{name}_{k}", plan(x, base + k * size, z), [size, size, size], ("wood", "Default"))

for (sheet, palette), blocks in groups.items():
    ops.blockworkHotspot(*blocks, sheet=sheets[sheet], palette=palette, seed=SEED)
if BEVEL > 0:
    ops.blockworkBevel(*[b for blocks in groups.values() for b in blocks if not b.startswith("Ground")], width=BEVEL)

# Sites as trigger volumes, and the spawns. The volumes go in a hidden visgroup so they don't cover the sites.
sites = [ops.blockworkEntity(**{"class": "Trigger"}, origin=plan(400, 0, 300), size=plan(1100, 400, 1500), name="SiteB"),
         ops.blockworkEntity(**{"class": "Trigger"}, origin=plan(3200, SITE_A_HEIGHT, 500), size=plan(1200, 400, 1300), name="SiteA")]
ops.blockworkVisgroup("Sites", add=sites)
ops.blockworkVisgroup("Sites", hidden=True)
for i in range(5):
    ops.blockworkEntity(**{"class": "PlayerStart"}, position=plan(1700 + i * 300, 0, 4500), turn=180, name=f"AttackerStart{i}")
    ops.blockworkEntity(**{"class": "PlayerStart"}, position=plan(2100 + i * 150, 0, 800), turn=0, name=f"DefenderStart{i}")

VIEWS = [("Cam_Overview", (2400, 7000, 7000), (2400, 0, 2300)), ("Cam_Mid", (2400, 170, 3900), (2400, 200, 1500)),
         ("Cam_ASite", (2450, 520, 1250), (3800, SITE_A_HEIGHT, 1000)), ("Cam_BSite", (1450, 500, 1700), (700, 0, 700))]
for name, eye, look in VIEWS:
    cam = ops.createNode("camera", name=name, skipSelect=True)
    ops.viewPlace(cam, eye=plan(*eye), lookAt=plan(*look), up=(0, 1, 0))
ops.viewCamera(eye=plan(*VIEWS[0][1]), lookAt=plan(*VIEWS[0][2]))

ops.select(clear=True)
ops.saveScene(os.path.join(out_dir, "saltmarket.lila"))
print("saltmarket: built", sum(len(b) for b in groups.values()), "blocks")
