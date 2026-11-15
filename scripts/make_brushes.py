"""Writes Lila's built-in texture brush presets into resources/brushes/<Category>/<id>.lilabrush.

Every preset names every brush setting it depends on (falloff, spacing, flow, jitter, tip, alpha...), so picking one
never inherits leftovers from the last. Tips are library alphas (scripts/make_alphas.py). Run from anywhere:
    python scripts/make_brushes.py
"""
import json
import zlib
from pathlib import Path

OUT = Path(__file__).resolve().parent.parent / "resources" / "brushes"

BASE = {
    "falloff": "smooth",
    "strength": 1.0,
    "flow": 1.0,
    "spacing": 0.1,
    "pressureSize": False,
    "pressureOpacity": False,
    "pressureFlow": False,
    "jitter": {"position": 0.0, "size": 0.0, "opacity": 0.0, "angle": 0.0},
    "angle": 0.0,
    "followStroke": False,
    "roundness": 1.0,
    "alpha": "",
    "strokeMode": "freehand",
    "lazyRadius": 0.0,
    "smoothing": 0.0,
}

PRESETS = []


def alpha(ref):
    return f"alpha://lila/{ref}.png"


def jit(position=0.0, size=0.0, opacity=0.0, angle=0.0):
    return {"position": position, "size": size, "opacity": opacity, "angle": angle}


def preset(cat, id, name, doc, tip=None, blend="normal", particles=None, curve=None, **brush):
    b = dict(BASE)
    if tip:
        b["alpha"] = alpha(tip)
    b.update(brush)
    if curve:
        b["pressureCurve"] = curve
    if b["jitter"] != BASE["jitter"]:
        b["seed"] = zlib.crc32(id.encode()) & 0xFFFF
    p = {"format": "lilabrush", "version": 1, "name": name, "category": cat, "doc": doc, "brush": b,
         "payload": {"kind": "paint", "blend": blend}}
    if particles:
        p["particles"] = particles
    PRESETS.append((cat, id, p))


def size_curve(floor=0.1, exponent=1.0):
    return {"size": {"floor": floor, "exponent": exponent}}


def artistic():
    c = "Artistic"
    preset(c, "loadedBristle", "Loaded Bristle", "A round bristle brush full of paint; streaks along the stroke.", "brush tips/bristle round",
           followStroke=True, spacing=0.04, jitter=jit(opacity=0.1), pressureSize=True, curve=size_curve(0.3))
    preset(c, "flatBristleSweep", "Flat Bristle Sweep", "A wide flat bristle laying parallel streaks.", "brush tips/bristle flat",
           followStroke=True, spacing=0.03, pressureOpacity=True)
    preset(c, "thirstyBristle", "Thirsty Bristle", "A nearly dry bristle that skips and breaks up.", "brush tips/bristle sparse",
           followStroke=True, spacing=0.03, flow=0.6, jitter=jit(opacity=0.35))
    preset(c, "fanBlender", "Fan Blender", "A fan of bristles for soft, feathered blending.", "brush tips/bristle fan",
           followStroke=True, spacing=0.05, flow=0.3)
    preset(c, "longHairStreak", "Long Hair Streak", "Fine hairs dragging uneven streaks.", "brush tips/hairy streaks",
           followStroke=True, angle=90.0, spacing=0.05, flow=0.7)
    preset(c, "spongeDabber", "Sponge Dabber", "Dabs of a fine open-cell sponge.", "brush tips/sponge dense",
           falloff="constant", spacing=0.6, jitter=jit(position=0.2, size=0.2, angle=180))
    preset(c, "coarseSponge", "Coarse Sponge", "A holey sponge for mottled texture.", "brush tips/sponge coarse",
           falloff="constant", spacing=0.8, jitter=jit(position=0.3, size=0.3, angle=180))
    preset(c, "powderPuff", "Powder Puff", "Soft grainy powder that builds slowly.", "brush tips/powder puff",
           falloff="bell", flow=0.2, spacing=0.15, jitter=jit(position=0.15, angle=180))
    preset(c, "wornBlockPrint", "Worn Block Print", "Stamped blocks with a broken, inked texture.", "brush tips/pastel block",
           falloff="constant", spacing=1.2, jitter=jit(opacity=0.3, angle=4))
    preset(c, "driftingSmoke", "Drifting Smoke", "Wisps of smoke that curl and fade.", "brush tips/smoke wisp",
           falloff="bell", flow=0.25, spacing=0.2, jitter=jit(position=0.3, size=0.4, angle=180))
    preset(c, "thinSmokeTrail", "Thin Smoke Trail", "A narrow ribbon of smoke along the stroke.", "brush tips/smoke wisp",
           followStroke=True, roundness=0.45, flow=0.15, spacing=0.08)
    preset(c, "sprayCan", "Spray Can", "Speckled spray paint; pressure sets the flow.", "brush tips/spray speckle",
           falloff="constant", flow=0.35, spacing=0.08, pressureFlow=True, jitter=jit(position=0.1, angle=180))
    preset(c, "fatCapSpray", "Fat Cap Spray", "A wide, dense spray with a fine mist.", "brush tips/fine spray",
           falloff="bell", flow=0.5, spacing=0.06, jitter=jit(angle=180))
    preset(c, "softPastel", "Soft Pastel", "A pastel stick dragged on its edge, catching the grain.", "brush tips/pastel block",
           followStroke=True, spacing=0.06, pressureOpacity=True)
    preset(c, "oilPastelSmear", "Oil Pastel Smear", "Waxy, grainy colour pushed along the stroke.", "brush tips/chalk grain",
           followStroke=True, roundness=0.6, flow=0.8, spacing=0.05)
    preset(c, "thickSmear", "Thick Smear", "Heavy paint smeared with ridged edges.", "stylized/paint smear",
           followStroke=True, spacing=0.05)
    preset(c, "impastoDab", "Impasto Dab", "Separate loaded dabs of thick paint.", "stylized/paint dab",
           falloff="constant", spacing=0.7, jitter=jit(size=0.2, angle=40))


def basic():
    c = "Basic"
    for hard, falloff, label in ((True, "sharp", "Hard Round"), (False, "smooth", "Soft Round")):
        pre = "hardRound" if hard else "softRound"
        preset(c, pre + "PressureSize", label + ", Pressure Size", "Pen pressure sets the size.", falloff=falloff, spacing=0.08,
               pressureSize=True, curve=size_curve(0.1))
        preset(c, pre + "PressureOpacity", label + ", Pressure Opacity", "Pen pressure sets the opacity.", falloff=falloff, spacing=0.08,
               pressureOpacity=True)
        preset(c, pre + "PressureBoth", label + ", Pressure Size and Opacity", "Pen pressure sets the size and the opacity.",
               falloff=falloff, spacing=0.08, pressureSize=True, pressureOpacity=True,
               curve={"size": {"floor": 0.15, "exponent": 1.0}, "opacity": {"floor": 0.0, "exponent": 1.3}})
    preset(c, "softBuildUp", "Soft Build-Up", "Very low flow: colour deepens with every pass.", flow=0.1, spacing=0.05)
    preset(c, "hardSquare", "Hard Square", "A square tip with crisp edges.", "shapes/square", falloff="constant", spacing=0.08)
    preset(c, "hardEllipse", "Hard Ellipse", "A slanted hard ellipse, like a calligraphy pen.", falloff="sharp", roundness=0.4,
           angle=45.0, spacing=0.05)
    preset(c, "softFlat", "Soft Flat", "A soft flat tip turning with the stroke.", roundness=0.3, followStroke=True, spacing=0.05)
    preset(c, "hardEraser", "Hard Eraser", "Removes paint with a crisp edge.", falloff="sharp", blend="erase", spacing=0.08)
    preset(c, "softEraserPressure", "Soft Eraser, Pressure", "Removes paint softly; pressure sets how much.", blend="erase",
           spacing=0.08, pressureOpacity=True)
    preset(c, "bellAirbrushPressure", "Airbrush, Pressure Size", "Low-flow bell airbrush; pressure sets size and flow.",
           falloff="bell", flow=0.2, spacing=0.05, pressureSize=True, pressureFlow=True, curve=size_curve(0.2))
    preset(c, "solidRound", "Solid Round", "A round tip with no falloff at all.", falloff="constant", spacing=0.06)


def dirt():
    c = "Dirt"
    preset(c, "cementDust", "Cement Dust", "Fine grey speckle of dried cement.", "brush tips/cement speckle",
           falloff="bell", flow=0.6, spacing=0.5, jitter=jit(position=0.3, size=0.3, angle=180))
    preset(c, "concreteStain", "Concrete Stain", "Blotchy weathering on poured surfaces.", "stone/weathered",
           falloff="bell", flow=0.5, spacing=0.4, jitter=jit(position=0.2, size=0.3, angle=180))
    preset(c, "pittedConcrete", "Pitted Concrete", "Small air pits and pocks.", "damage/pitting",
           falloff="smooth", spacing=0.6, jitter=jit(position=0.3, angle=180))
    preset(c, "hairlineCracks", "Hairline Cracks", "Thin cracks stamped along the stroke.", "damage/hairline cracks",
           falloff="constant", spacing=1.2, jitter=jit(size=0.2, angle=180))
    preset(c, "spiderCrackStamp", "Spider Crack Stamp", "Isolated burst cracks, well spaced.", "damage/spider cracks",
           falloff="constant", spacing=2.5, jitter=jit(position=0.4, size=0.3, angle=180))
    preset(c, "dirtSpots", "Dirt Spots", "Scattered round grime spots.", "brush tips/round spots",
           falloff="constant", flow=0.7, spacing=0.8, jitter=jit(position=0.6, size=0.5, opacity=0.3, angle=180))
    preset(c, "dirtSplash", "Dirt Splash", "Kicked-up dirt splashes.", "brush tips/splatter cluster",
           falloff="constant", flow=0.8, spacing=1.5, jitter=jit(position=0.4, size=0.4, angle=180))
    preset(c, "mudSplash", "Mud Splash", "Wet mud slapped on and running down.", "brush tips/mud splash",
           falloff="constant", spacing=0.9, jitter=jit(position=0.2, size=0.3, angle=20))
    preset(c, "cakedMud", "Caked Mud", "Dried, cracked mud crust.", "organic/dry mud",
           falloff="smooth", spacing=0.6, jitter=jit(size=0.2, angle=180))
    preset(c, "settlingDust", "Settling Dust", "Sparse soft motes of dust.", "brush tips/dust motes",
           falloff="smooth", flow=0.8, spacing=0.4, jitter=jit(position=0.5, angle=180))
    preset(c, "dustVeil", "Dust Veil", "A fine grainy film of dust.", "noise/soft grain",
           falloff="bell", flow=0.12, spacing=0.2, jitter=jit(angle=180))
    preset(c, "moldBloom", "Mold Bloom", "Fuzzy colonies of mold.", "brush tips/mold colony",
           falloff="smooth", spacing=0.7, jitter=jit(position=0.4, size=0.4, angle=180))
    preset(c, "dampMildew", "Damp Mildew", "Faint cellular mildew patches.", "noise/fine cellular",
           falloff="bell", flow=0.3, spacing=0.35, jitter=jit(position=0.2, angle=180))
    preset(c, "sandpaperScuff", "Sandpaper Scuff", "Sharp grit marks of an abrasive.", "brush tips/sandpaper grain",
           falloff="smooth", flow=0.5, spacing=0.2, jitter=jit(angle=180))
    preset(c, "gritClumps", "Grit Clumps", "Clumps of coarse grit.", "brush tips/grit clump",
           falloff="constant", spacing=0.9, jitter=jit(position=0.5, size=0.5, angle=180))
    preset(c, "cornerGrime", "Corner Grime", "Turbulent grime for creases and corners.", "noise/turbulence",
           falloff="bell", flow=0.25, spacing=0.3, jitter=jit(position=0.2, angle=180))


def dry():
    c = "Dry"
    preset(c, "chalkStick", "Chalk Stick", "The tip of a chalk stick on rough paper.", "brush tips/chalk grain",
           falloff="smooth", spacing=0.06, pressureSize=True, curve=size_curve(0.4), jitter=jit(angle=180))
    preset(c, "chalkSide", "Chalk on Its Side", "A broad side stroke of chalk.", "brush tips/chalk grain",
           roundness=0.35, followStroke=True, angle=90.0, spacing=0.05)
    preset(c, "vineCharcoal", "Vine Charcoal", "Light, dusty charcoal; pressure sets the darkness.", "brush tips/charcoal grain",
           followStroke=True, flow=0.7, spacing=0.05, pressureOpacity=True)
    preset(c, "compressedCharcoal", "Compressed Charcoal", "Dense, dark charcoal.", "brush tips/charcoal grain",
           spacing=0.04, pressureSize=True, curve=size_curve(0.3), jitter=jit(angle=180))
    preset(c, "charcoalHaze", "Charcoal Haze", "Charcoal dust rubbed into a haze.", "brush tips/charcoal grain",
           falloff="bell", flow=0.15, spacing=0.1, jitter=jit(position=0.2, angle=180))
    preset(c, "hardPencil", "Hard Pencil", "A fine, light graphite line.", "brush tips/pencil grain",
           radius=4.0, flow=0.6, spacing=0.05, pressureOpacity=True, curve={"opacity": {"floor": 0.1, "exponent": 1.5}})
    preset(c, "softPencil", "Soft Pencil", "A broad, dark graphite line.", "brush tips/pencil grain",
           radius=8.0, flow=0.85, spacing=0.05, pressureOpacity=True, pressureSize=True, curve=size_curve(0.5))
    preset(c, "sketchPencil", "Sketch Pencil", "A smoothed pencil for loose sketching.", "brush tips/pencil grain",
           radius=5.0, spacing=0.05, smoothing=0.3, pressureSize=True, curve=size_curve(0.2))
    preset(c, "diagonalHatching", "Diagonal Hatching", "Parallel diagonal hatch lines.", "brush tips/hatch lines",
           falloff="constant", flow=0.8, spacing=1.6, jitter=jit(position=0.05, angle=4))
    preset(c, "crossHatching", "Cross Hatching", "Crossed hatch lines for darker shading.", "brush tips/cross hatch",
           falloff="constant", flow=0.8, spacing=1.6, jitter=jit(position=0.05, angle=4))
    preset(c, "denseHatching", "Dense Hatching", "Tight hatching turning with the stroke.", "brush tips/dense hatch",
           followStroke=True, falloff="constant", spacing=1.2)
    preset(c, "scribbleHatch", "Scribble Hatch", "Loose, overlapping hatch marks.", "brush tips/hatch lines",
           falloff="smooth", spacing=0.9, jitter=jit(position=0.3, opacity=0.2, angle=30))
    preset(c, "waxCrayon", "Wax Crayon", "Streaky waxy colour.", "noise/horizontal streaks",
           followStroke=True, angle=90.0, roundness=0.7, spacing=0.05)
    preset(c, "squareConte", "Square Conte", "A square stick held at a fixed angle.", "brush tips/pastel block",
           angle=20.0, spacing=0.05, pressureOpacity=True)


def wet():
    c = "Wet"
    preset(c, "watercolourWash", "Watercolour Wash", "Translucent wash with mottled pigment.", "brush tips/watercolour bloom",
           falloff="smooth", flow=0.25, spacing=0.1, jitter=jit(size=0.15, angle=180))
    preset(c, "watercolourBloom", "Watercolour Bloom", "Separate blooms with dark drying rims.", "brush tips/watercolour bloom",
           falloff="constant", flow=0.4, spacing=0.7, jitter=jit(position=0.3, size=0.3, angle=180))
    preset(c, "wetEdgeWash", "Wet Edge Wash", "Pigment pooling at the edges of the stroke.", "brush tips/wet edge",
           falloff="constant", flow=0.3, spacing=0.08)
    preset(c, "granulatingWash", "Granulating Wash", "A wash that settles into granules.", "noise/cellular",
           falloff="bell", flow=0.2, spacing=0.12, jitter=jit(angle=180))
    preset(c, "gouacheFlat", "Gouache Flat", "Opaque matte paint from a flat tip.", "brush tips/felt tip",
           followStroke=True, roundness=0.5, flow=0.9, spacing=0.04)
    preset(c, "gouacheDrag", "Gouache Drag", "Drying gouache dragged until it breaks.", "stylized/dry brush",
           followStroke=True, spacing=0.04)
    preset(c, "broadWash", "Broad Wash", "Thin even wash; pressure sets the flow.", falloff="bell", flow=0.08, spacing=0.06,
           pressureFlow=True)
    preset(c, "thinGlaze", "Thin Glaze", "A multiplying glaze that deepens what is under it.", falloff="bell", flow=0.15,
           strength=0.6, spacing=0.06, blend="multiply")
    preset(c, "chiselMarker", "Chisel Marker", "A chisel nib at a fixed angle.", "brush tips/chisel marker",
           falloff="constant", angle=30.0, flow=0.6, spacing=0.04)
    preset(c, "roundMarker", "Round Marker", "A rounded marker nib, slightly streaky.", "brush tips/felt tip",
           falloff="constant", flow=0.5, spacing=0.05)
    preset(c, "fineFeltTip", "Fine Felt Tip", "A thin, even felt-tip line.", "brush tips/felt tip",
           radius=4.0, falloff="constant", spacing=0.1)
    preset(c, "brushPen", "Brush Pen", "A tapering ink line; pressure sets the width.", falloff="sharp", spacing=0.04,
           smoothing=0.3, pressureSize=True, curve=size_curve(0.05, 1.4))
    preset(c, "paletteKnife", "Palette Knife", "Flat knife-laid paint with ridged edges.", "brush tips/palette knife",
           followStroke=True, falloff="constant", spacing=0.03)
    preset(c, "knifeScrape", "Knife Scrape", "Scrapes paint away with a knife edge.", "brush tips/palette knife",
           followStroke=True, falloff="constant", flow=0.6, spacing=0.03, blend="erase")
    preset(c, "wetSponge", "Wet Sponge", "A damp sponge leaving soft pores.", "brush tips/sponge dense",
           falloff="bell", flow=0.4, spacing=0.3, jitter=jit(position=0.2, angle=180))
    preset(c, "wetHardRound", "Wet Hard Round", "Hard-edged transparent layers that build up.", falloff="sharp", flow=0.35,
           spacing=0.05, pressureOpacity=True)
    preset(c, "wetSoftRound", "Wet Soft Round", "Soft transparent paint; pressure sets the flow.", flow=0.2, spacing=0.05,
           pressureFlow=True, smoothing=0.2)
    preset(c, "bleedingInk", "Bleeding Ink", "Ink blooms that run a little.", "brush tips/watercolour bloom",
           falloff="smooth", flow=0.5, spacing=0.4, jitter=jit(size=0.2, angle=180),
           particles={"kind": "drips", "count": 1, "length": 3, "size": 0.2, "rate": 0.15})


def fiber():
    c = "Fiber"
    preset(c, "denseFur", "Dense Fur", "Thick fur laid along the stroke.", "brush tips/fur strands",
           followStroke=True, angle=90.0, spacing=0.3, jitter=jit(angle=10, opacity=0.2))
    preset(c, "sparseFur", "Sparse Fur", "Scattered tufts of fur.", "brush tips/fur strands",
           followStroke=True, angle=90.0, spacing=0.9, jitter=jit(position=0.5, angle=25, opacity=0.4))
    preset(c, "shortFuzz", "Short Fuzz", "Short fur pointing every way.", "brush tips/fur strands",
           spacing=0.4, jitter=jit(size=0.3, angle=180))
    preset(c, "featherStamp", "Feather Stamp", "Single feathers along the stroke.", "brush tips/feather barbs",
           followStroke=True, falloff="constant", spacing=1.5, jitter=jit(size=0.2, angle=10))
    preset(c, "downyFeathers", "Downy Feathers", "Soft down scattered about.", "skin/down feather",
           falloff="smooth", spacing=0.8, jitter=jit(position=0.5, size=0.4, angle=180))
    preset(c, "interlacedFibres", "Interlaced Fibres", "Crossing fibres like matted felt.", "brush tips/interlaced fibres",
           spacing=0.5, jitter=jit(angle=180))
    preset(c, "mossFibres", "Moss Fibres", "Curly short fibres of moss.", "brush tips/moss fibres",
           spacing=0.4, jitter=jit(position=0.4, size=0.4, angle=180))
    preset(c, "hairLines", "Hair Lines", "Fine parallel hairs following the stroke.", "brush tips/hair lines",
           followStroke=True, spacing=0.05)
    preset(c, "wavyLocks", "Wavy Locks", "Stylised strands of hair.", "stylized/stylized hair strands",
           followStroke=True, spacing=0.08)
    preset(c, "paperFibres", "Paper Fibres", "Fibrous paper texture.", "noise/fibres",
           falloff="bell", flow=0.5, spacing=0.4, jitter=jit(angle=180))
    preset(c, "furClumps", "Fur Clumps", "Stylised clumps of fur.", "stylized/stylized fur clumps",
           followStroke=True, spacing=0.6, jitter=jit(angle=20))
    preset(c, "feltedWool", "Felted Wool", "The fuzz of felted wool.", "fabric/felt",
           falloff="bell", flow=0.5, spacing=0.4, jitter=jit(angle=180))


def organic():
    c = "Organic"
    preset(c, "furrowedBark", "Furrowed Bark", "Deep bark furrows along the stroke.", "organic/furrowed bark",
           followStroke=True, angle=90.0, spacing=0.6, jitter=jit(size=0.2))
    preset(c, "flakyBark", "Flaky Bark", "Flaking plates of bark.", "organic/flaky bark",
           spacing=0.6, jitter=jit(size=0.2, angle=180))
    preset(c, "peelingBark", "Peeling Bark", "Thin papery bark curling off.", "organic/papery bark",
           followStroke=True, angle=90.0, spacing=0.6)
    preset(c, "leatherGrain", "Leather Grain", "The grain of tanned hide.", "organic/leather",
           spacing=0.5, jitter=jit(angle=180))
    preset(c, "pebbledLeather", "Pebbled Leather", "Pebbled leather texture.", "fabric/pebbled leather",
           spacing=0.5, jitter=jit(angle=180))
    preset(c, "creasedLeather", "Creased Leather", "Worn creases in leather.", "fabric/creased leather",
           spacing=0.6, jitter=jit(size=0.2, angle=180))
    preset(c, "skinPores", "Skin Pores", "Fine pores for skin.", "skin/pores fine",
           flow=0.6, spacing=0.5, jitter=jit(angle=180))
    preset(c, "fineWrinkles", "Fine Wrinkles", "Fine wrinkles across the stroke.", "skin/fine wrinkles",
           followStroke=True, spacing=0.5)
    preset(c, "surfaceVeins", "Surface Veins", "Branching veins under the skin.", "skin/veins",
           flow=0.7, spacing=1.0, jitter=jit(size=0.2, angle=180))
    preset(c, "whorlPrint", "Whorl Print", "A swirling ridged print.", "brush tips/whorl print",
           falloff="constant", spacing=2.5, jitter=jit(size=0.2, angle=180))
    preset(c, "loopPrint", "Loop Print", "A looped ridge print.", "brush tips/loop print",
           falloff="constant", spacing=2.5, jitter=jit(size=0.2, angle=180))
    preset(c, "lichenPatch", "Lichen Patch", "Crusty lichen spreading on stone.", "organic/lichen",
           spacing=0.6, jitter=jit(position=0.3, size=0.3, angle=180))
    preset(c, "reptileHide", "Reptile Hide", "Beaded scales of a lizard.", "skin/lizard scales",
           spacing=0.5, jitter=jit(angle=180))
    preset(c, "woodGrainStreak", "Wood Grain Streak", "Wood grain running with the stroke.", "organic/fine wood grain",
           followStroke=True, angle=90.0, spacing=0.1)


def pattern():
    c = "Pattern"
    preset(c, "stripeRoller", "Stripe Roller", "Even stripes across the stroke.", "brush tips/stripe bar",
           followStroke=True, falloff="constant", spacing=0.6)
    preset(c, "pinstripeRoller", "Pinstripe Roller", "Tight thin stripes.", "brush tips/stripe bar",
           followStroke=True, falloff="constant", roundness=0.5, angle=90.0, spacing=0.3)
    preset(c, "hazardStripes", "Hazard Stripes", "Slanted warning bands.", "brush tips/hazard band",
           followStroke=True, falloff="constant", spacing=1.0)
    preset(c, "chevronTread", "Chevron Tread", "A V-block tread track.", "brush tips/tread chevron",
           followStroke=True, falloff="constant", spacing=1.0)
    preset(c, "trackCleats", "Track Cleats", "Staggered cleats of a heavy track.", "brush tips/track cleats",
           followStroke=True, falloff="constant", spacing=1.9)
    preset(c, "gripPadTrack", "Grip Pad Track", "Rows of grip pads.", "hard surface/grip pads",
           followStroke=True, falloff="constant", spacing=1.9)
    preset(c, "sciFiScales", "Sci-Fi Scales", "Overlapping armour scales.", "brush tips/scale plate",
           followStroke=True, angle=90.0, falloff="constant", spacing=1.2)
    preset(c, "hexCellRoller", "Hex Cell Roller", "A chain of hexagonal cells.", "brush tips/hex cell",
           followStroke=True, falloff="constant", spacing=1.75)
    preset(c, "rivetLine", "Rivet Line", "Evenly spaced rivets.", "hard surface/rivet",
           followStroke=True, falloff="constant", spacing=2.5)
    preset(c, "dottedLine", "Dotted Line", "Round dots with gaps.", "shapes/round hard",
           falloff="constant", spacing=3.0)
    preset(c, "dashedLine", "Dashed Line", "Dashes along the stroke.", "brush tips/running stitch",
           followStroke=True, falloff="constant", spacing=2.0)
    preset(c, "ropeRoller", "Rope Roller", "A twisted rope border.", "ornament/rope border",
           followStroke=True, falloff="constant", spacing=1.8)
    preset(c, "braidRoller", "Braid Roller", "A braided band.", "ornament/braid border",
           followStroke=True, falloff="constant", spacing=1.8)
    preset(c, "keyPatternRoller", "Key Pattern Roller", "A meander key band.", "ornament/key border",
           followStroke=True, falloff="constant", spacing=1.8)
    preset(c, "beadRoller", "Bead Roller", "A row of beads.", "ornament/bead border",
           followStroke=True, falloff="constant", spacing=1.8)
    preset(c, "dotGridStamp", "Dot Grid Stamp", "A grid of dots, stamped edge to edge.", "shapes/dot grid",
           falloff="constant", spacing=1.8)


def scratches():
    c = "Scratches"
    preset(c, "fineScratches", "Fine Scratches", "Random fine scratches.", "damage/scratches",
           falloff="smooth", spacing=1.0, jitter=jit(size=0.2, angle=180))
    preset(c, "denseScratches", "Dense Scratches", "Heavily scratched wear.", "damage/dense scratches",
           falloff="smooth", spacing=0.8, jitter=jit(angle=180))
    preset(c, "parallelScratches", "Parallel Scratches", "Scratches dragged along the stroke.", "damage/parallel scratches",
           followStroke=True, spacing=0.8)
    preset(c, "swirlScratches", "Swirl Scratches", "Curved polishing swirls.", "damage/curved scratches",
           falloff="smooth", spacing=0.9, jitter=jit(angle=180))
    preset(c, "clawRake", "Claw Rake", "Parallel claw gashes.", "damage/claw marks",
           followStroke=True, falloff="constant", spacing=2.0)
    preset(c, "scuffMarks", "Scuff Marks", "Soft rubbing scuffs.", "damage/scuffs",
           falloff="smooth", spacing=0.4, jitter=jit(size=0.3, angle=180))
    preset(c, "deepGouges", "Deep Gouges", "Deep gouged grooves.", "damage/gouges",
           falloff="smooth", spacing=1.2, jitter=jit(angle=180))
    preset(c, "singleScratch", "Single Scratch", "One thin scratch line; pressure sets the width.", radius=3.0, falloff="sharp",
           roundness=0.3, followStroke=True, spacing=0.05, smoothing=0.4, pressureSize=True, curve=size_curve(0.0, 1.5))
    preset(c, "edgeWear", "Edge Wear", "Worn paint along edges.", "damage/worn edge",
           followStroke=True, spacing=0.6)
    preset(c, "paintChips", "Paint Chips", "Chipped flakes of paint.", "damage/chipped paint",
           falloff="smooth", spacing=0.9, jitter=jit(size=0.3, angle=180))
    preset(c, "rakeLines", "Rake Lines", "Long parallel lines like a rake.", "brush tips/bristle sparse",
           followStroke=True, falloff="constant", spacing=0.02)


def splatter():
    c = "Splatter"
    preset(c, "inkSplatter", "Ink Splatter", "Bold ink splats with spikes and droplets.", "brush tips/ink splat",
           falloff="constant", spacing=2.0, jitter=jit(position=0.6, size=0.4, angle=180))
    preset(c, "inkFlick", "Ink Flick", "Small flicked splatters with many drops.", "brush tips/splatter cluster",
           falloff="constant", spacing=3.0, jitter=jit(position=0.8, size=0.5, angle=180))
    preset(c, "paintDrops", "Paint Drops", "Fallen drops of paint.", "brush tips/paint drop",
           falloff="constant", spacing=1.5, jitter=jit(position=0.8, size=0.5, angle=30))
    preset(c, "runningDrips", "Running Drips", "Paint that runs down from the stroke.", "shapes/round hard",
           falloff="constant", spacing=0.2, particles={"kind": "drips", "count": 3, "length": 6, "size": 0.2, "rate": 0.4})
    preset(c, "sprayMist", "Spray Mist", "A fine soft mist of droplets.", "brush tips/fine spray",
           falloff="smooth", flow=0.7, spacing=0.12, jitter=jit(angle=180))
    preset(c, "spatterSpray", "Spatter Spray", "Coarse spray with stray drops.", "brush tips/spray speckle",
           falloff="constant", spacing=0.4, jitter=jit(position=0.4, angle=180),
           particles={"kind": "rain", "count": 6, "size": 0.08, "spread": 1.5, "rate": 0.6})
    preset(c, "roundSpots", "Round Spots", "Clusters of round spots.", "brush tips/round spots",
           falloff="constant", spacing=1.2, jitter=jit(position=0.8, size=0.5, angle=180))
    preset(c, "freckleSpots", "Freckle Spots", "Tiny scattered spots of varied size.", "shapes/round hard",
           falloff="constant", spacing=1.5, roundness=0.85, jitter=jit(position=2.0, size=0.8, opacity=0.5, angle=180))
    preset(c, "stormSpatter", "Storm Spatter", "A burst of small drops around the brush.", "shapes/round soft",
           falloff="smooth", spacing=0.5, particles={"kind": "rain", "count": 14, "size": 0.08, "spread": 2.5, "rate": 1.0})
    preset(c, "driedWaterRings", "Dried Water Rings", "Rings left by evaporated drops.", "brush tips/wet edge",
           falloff="constant", flow=0.6, spacing=2.0, jitter=jit(position=1.5, size=0.7, angle=180))
    preset(c, "flickStreaks", "Flick Streaks", "Streaks flung out from the stroke.", "shapes/round soft",
           falloff="smooth", spacing=0.4, particles={"kind": "sparks", "count": 6, "length": 4, "size": 0.1, "rate": 0.6})


def stitches():
    c = "Stitches"
    preset(c, "runningStitch", "Running Stitch", "Single stitches with gaps.", "brush tips/running stitch",
           followStroke=True, falloff="constant", spacing=1.6)
    preset(c, "doubleStitchLine", "Double Stitch Line", "Two parallel stitch rows.", "fabric/double stitches",
           followStroke=True, falloff="constant", spacing=1.8)
    preset(c, "crossStitchRow", "Cross Stitch Row", "A row of crosses.", "brush tips/cross stitch mark",
           followStroke=True, falloff="constant", spacing=1.3)
    preset(c, "zigzagStitch", "Zigzag Stitch", "A machine zigzag seam.", "fabric/zigzag stitch",
           followStroke=True, falloff="constant", spacing=1.8)
    preset(c, "blanketStitch", "Blanket Stitch", "An edging blanket stitch.", "fabric/blanket stitch",
           followStroke=True, falloff="constant", spacing=1.8)
    preset(c, "stapleRow", "Staple Row", "Evenly spaced staples.", "brush tips/staple",
           followStroke=True, falloff="constant", spacing=1.6)
    preset(c, "looseStaples", "Loose Staples", "Staples at uneven angles.", "brush tips/staple",
           followStroke=True, falloff="constant", spacing=2.5, jitter=jit(position=0.2, angle=25))
    preset(c, "seamLine", "Seam Line", "A stitched seam.", "fabric/seam",
           followStroke=True, falloff="constant", spacing=1.8)
    preset(c, "quiltStitches", "Quilt Stitches", "Short hand stitches.", "fabric/stitches",
           followStroke=True, falloff="constant", spacing=1.8)


def dynamic():
    c = "Dynamic"
    preset(c, "leafStrokes", "Leaf Strokes", "Leaves laid along the stroke.", "brush tips/leaf sprig",
           followStroke=True, falloff="constant", spacing=1.2, jitter=jit(size=0.3, angle=30))
    preset(c, "scatteredLeaves", "Scattered Leaves", "Leaves strewn every way.", "organic/oval leaf",
           falloff="constant", spacing=1.0, jitter=jit(position=0.8, size=0.5, angle=180))
    preset(c, "pointedLeafTrail", "Pointed Leaf Trail", "Pointed leaves angled off the stroke.", "organic/pointed leaf",
           followStroke=True, angle=45.0, falloff="constant", spacing=1.0, jitter=jit(angle=20, size=0.2))
    preset(c, "grassTufts", "Grass Tufts", "Upright tufts of grass.", "brush tips/grass tuft",
           falloff="constant", spacing=0.8, jitter=jit(position=0.3, size=0.4, angle=10))
    preset(c, "zigzagLine", "Zigzag Line", "A continuous zigzag along the stroke.", "brush tips/zigzag segment",
           followStroke=True, falloff="constant", spacing=1.8)
    preset(c, "shakyZigzag", "Shaky Zigzag", "An uneven hand-drawn zigzag.", "brush tips/zigzag segment",
           followStroke=True, falloff="constant", spacing=1.6, jitter=jit(size=0.2, angle=12))
    preset(c, "inkLine", "Ink Line", "A smoothed tapering ink line.", falloff="sharp", spacing=0.04, smoothing=0.5,
           lazyRadius=10.0, pressureSize=True, curve=size_curve(0.0, 1.6))
    preset(c, "calligraphyNib", "Calligraphy Nib", "A broad nib held at 45 degrees.", "brush tips/ink nib",
           angle=45.0, spacing=0.03, pressureSize=True, curve=size_curve(0.4))
    preset(c, "wobblyInk", "Wobbly Ink", "A nib line that wavers.", "brush tips/ink nib",
           followStroke=True, angle=20.0, spacing=0.04, jitter=jit(size=0.2, angle=15))
    preset(c, "cleanCurve", "Clean Curve", "Draw freely; the line is smoothed on release.", falloff="sharp", spacing=0.05,
           strokeMode="curve")
    preset(c, "twistingRibbon", "Twisting Ribbon", "A thin flat ribbon that twists.", falloff="sharp", roundness=0.15,
           followStroke=True, angle=30.0, spacing=0.03, jitter=jit(angle=40))
    preset(c, "sparkleTrail", "Sparkle Trail", "Sparkles scattered along the stroke.", "stylized/stylized sparkle",
           falloff="constant", spacing=1.5, jitter=jit(position=1.0, size=0.6, angle=45))
    preset(c, "confetti", "Confetti", "Flat confetti pieces flung about.", "shapes/square",
           falloff="constant", roundness=0.5, spacing=1.0, jitter=jit(position=1.5, size=0.6, opacity=0.3, angle=180))


def main():
    for gen in (artistic, basic, dirt, dry, wet, fiber, organic, pattern, scratches, splatter, stitches, dynamic):
        gen()
    ids = [(c, i) for c, i, _ in PRESETS]
    assert len(set(ids)) == len(ids), "duplicate preset id"
    names = [p["name"] for _, _, p in PRESETS]
    assert len(set(names)) == len(names), "duplicate preset name"
    for cat, id, p in PRESETS:
        d = OUT / cat
        d.mkdir(parents=True, exist_ok=True)
        f = d / f"{id}.lilabrush"
        f.write_text(json.dumps(p, indent=2) + "\n", encoding="utf-8", newline="\n")
    print(len(PRESETS), "presets")


if __name__ == "__main__":
    main()
