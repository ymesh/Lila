"""Builds the Trim Frieze example, the trim sheets tutorial end to end: a sandstone trim sheet laid out from the
Power-of-Two Ladder, a rosette frieze and a dentil course modelled on its high poly and baked in, textured with Trim
Sandstone, and a wall built as a stack of bands that each carry one strip.

Run from the script editor, or `lilabatch --batch build_trim_frieze.py [out_dir]`. Writes trim_frieze.lila and the sheet
(TrimSheets/T_Frieze.lilatrim with its textures) next to this file, or into out_dir. Units are cm, Y up.
"""

import math
import os
import sys

from lila import ops

here = os.path.dirname(os.path.abspath(__file__)) if "__file__" in globals() else os.getcwd()
out_dir = sys.argv[1] if len(sys.argv) > 1 else here

SHEET_WIDTH = 1024  # px; at the ladder's 512 px per metre the sheet is 200 cm across
ROSETTES = 5  # along the frieze; must divide the sheet's 200 cm so the strip tiles
DENTILS = 20
DENTIL_BEVEL = 0.0  # cm; bevelled sides show in the baked normal too, which hides the comparison
MATERIAL = "Trim Sandstone"

# The strips top to bottom after splitting the ladder's 512 px strip in two: (name, profile style).
STRIPS = [("Frieze", "recessed"), ("Panel", "raised"), ("Courses", "groove"), ("Dentils", "step"), ("Torus", "halfPipe"),
          ("Bead", "halfPipe"), ("Edge", "raised"), ("EdgeB", "doubleBevel")]

# The wall, bottom to top: (name, bottom, height, depth, strip). Each band is as tall as its strip.
BANDS = [("Plinth", 0, 50, 48, "Panel"), ("BaseTorus", 50, 12.5, 46, "Torus"), ("BaseBead", 62.5, 6.25, 43, "Bead"),
         ("Body1", 68.75, 50, 40, "Panel"), ("Body2", 118.75, 50, 40, "Panel"), ("Body3", 168.75, 50, 40, "Panel"),
         ("DentilBand", 218.75, 25, 44, "Dentils"), ("FriezeBand", 243.75, 50, 42, "Frieze"),
         ("Cornice", 293.75, 12.5, 50, "Torus"), ("Cap", 306.25, 6.25, 54, "Bead")]


def first(r):
    return r if isinstance(r, str) else r[0]


ops.newScene()
# Saved first so the sheet, its textures and its bake project are stored relative to the scene.
ops.saveScene(os.path.join(out_dir, "trim_frieze.lila"))
sheet = os.path.join(out_dir, "TrimSheets", "T_Frieze.lilatrim")
ops.trimSheetCreate(preset="ladder", name="T_Frieze", width=SHEET_WIDTH, file=sheet, overwrite=True, bake=False)

ops.trimSheetEdit(split={"trim": 0, "axis": "y", "at": SHEET_WIDTH // 4})
for i, (name, style) in enumerate(STRIPS):
    ops.trimSheetEdit(trim=i, values={"name": name, "profile": {"style": style, "bevelPx": 6}})

# The generated high lies on the ground: x along the sheet, z down the texture, y up off it.
generated = ops.trimSheetGenerate()
ops.hide(generated["low"])
width_cm = SHEET_WIDTH / 512.0 * 100.0
frieze_z = width_cm / 8.0  # the middle of the top 25% of the sheet
dentil_z = width_cm * (768 + 64) / 1024.0
motifs = []
for k in range(ROSETTES):
    x = width_cm / ROSETTES * (k + 0.5)
    ring = first(ops.polyTorus(radius=10.0, sectionRadius=2.2, subdivisionsAxis=32, subdivisionsHeight=12, name=f"Ring{k}"))
    ops.move(ring, to=[x, 0.0, frieze_z])
    boss = first(ops.polySphere(radius=4.5, subdivisionsAxis=24, subdivisionsHeight=12, name=f"Boss{k}"))
    ops.scale(boss, to=[1.0, 0.6, 1.0])
    ops.move(boss, to=[x, 0.0, frieze_z])
    motifs += [ring, boss]
    for p in range(8):
        a = p * math.pi / 4.0
        petal = first(ops.polySphere(radius=2.4, subdivisionsAxis=16, subdivisionsHeight=8, name=f"Petal{k}_{p}"))
        ops.scale(petal, to=[1.6, 0.7, 0.9])
        ops.rotate(petal, to=[0.0, -math.degrees(a), 0.0])
        ops.move(petal, to=[x + 6.5 * math.cos(a), 0.0, frieze_z + 6.5 * math.sin(a)])
        motifs.append(petal)
dentils = []
for k in range(DENTILS):
    block = first(ops.polyCube(width=6.0, height=3.0, depth=14.0, baseAtOrigin=True, name=f"Dentil{k}"))
    # On top of the step, which stands about 3.5 cm tall; sunk into it, a block bakes to nothing.
    ops.move(block, to=[width_cm / DENTILS * (k + 0.5), 3.5, dentil_z])
    dentils.append(block)
# Vertical sides, which a straight-down bake misses: Wall shows them rebuilt from the height, Wall_BakedNormal without.
if DENTIL_BEVEL > 0:
    ops.select(dentils)
    ops.polyBevel(offset=DENTIL_BEVEL, segments=3)
motifs += dentils
ops.group(generated["high"], generated["low"], *motifs, name="SheetHigh")
ops.hide("SheetHigh")

ops.trimSheetBake(high=motifs)
ops.trimSheetTexture(material=MATERIAL, replace=True)

CAP_STRIP = "Panel"  # the tops and bottoms of the bands: flat stone with a chamfered lip


def map_band(band, strip):
    # A cube's faces: front, top, back, bottom, right, left. The sides share one strip, so it runs on round the corners.
    ops.selectMode("face")
    ops.selectComponents(band, faces=[0, 2, 4, 5])
    ops.trimApply(trim=strip)
    ops.selectComponents(band, faces=[1, 3])
    ops.trimApply(trim=CAP_STRIP)
    ops.selectMode("object")
    ops.select(clear=True)


bands = []
for name, y, height, depth, strip in BANDS:
    band = first(ops.polyCube(width=400, height=height, depth=depth, baseAtOrigin=True, name=name))
    ops.move(band, to=[0, y, 0])
    map_band(band, strip)
    bands.append(band)
ops.group(*bands, name="Wall")

# The comparison: the same wall beside it with Normals From Height off, so the baked normal is painted on as is.
COMPARE_OFFSET = 480
plain = first(ops.duplicate("Wall", name="Wall_BakedNormal", translate=[COMPARE_OFFSET, 0, 0]))
plain_bands = ops.listRelatives(plain)

def baked_normal_copy(source):
    copy = ops.rename(ops.createMaterial(), source + "_BakedNormal")
    for attr in ops.listAttr(source):
        ops.setAttr(f"{copy}.{attr}", ops.getAttr(f"{source}.{attr}"))
    ops.setAttr(copy + ".normalFromHeight", False)
    # The full baked normal, not the detail one; the base colour comes from the texture set's preview, since the
    # original draws through the set and the copy has only its maps.
    textures = os.path.dirname(ops.getAttr(source + ".heightMap"))
    ops.setAttr(copy + ".normalMap", ops.getAttr(source + ".normalMap").replace("_detailnormal", "_normal"))
    previews = os.path.join(out_dir, textures, "Preview")
    colour = next((f for f in os.listdir(previews) if f.endswith("_baseColor.png")), None)
    if colour:
        ops.setAttr(copy + ".baseColorMap", f"{textures}/Preview/{colour}")
    return copy


copies = {}
for entry in ops.listMaterials(*plain_bands):
    (slot,) = entry["slots"]  # trimApply puts the sheet's one material on every face
    source = slot["slot"]
    if source not in copies:
        copies[source] = baked_normal_copy(source)
    ops.assignMaterial(copies[source], entry["object"])

ops.select(clear=True)
compare = ops.createNode("camera", name="Cam_Compare", skipSelect=True)
# Close on the dentils and rosettes where the two walls' corners face each other, so both sides of each show.
ops.viewPlace(compare, eye=(COMPARE_OFFSET / 2, 300, 260), lookAt=(COMPARE_OFFSET / 2, 245, 20), up=(0, 1, 0))
cam = ops.createNode("camera", name="Cam_Wall", skipSelect=True)
ops.viewPlace(cam, eye=(-420, 220, 740), lookAt=(0, 160, 0), up=(0, 1, 0))
# The view the scene opens on: the whole wall, a little from the left. Batch runs have no view to set.
try:
    ops.viewCamera(eye=(-430, 260, 820), lookAt=(0, 150, 0))
except Exception:
    pass
ops.saveScene(os.path.join(out_dir, "trim_frieze.lila"))
