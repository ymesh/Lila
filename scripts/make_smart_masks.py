"""Writes Lila's built-in smart masks into resources/smartmaterials (mask*.lilasmart). Rerunning reproduces the files exactly."""
import json
import os

OUT = os.path.join(os.path.dirname(os.path.dirname(os.path.abspath(__file__))), "resources", "smartmaterials")

FAMILIES = {"dirt", "dust", "edges", "rust", "moss", "sand", "paint", "drips", "stains", "scratches", "cracks", "moisture",
            "occlusion", "fabric", "concrete", "ground"}


class Ids:
    def __init__(self):
        self.n = 0

    def __call__(self):
        self.n += 1
        return f"M{self.n}"


ID = Ids()


def G(type, name, mode="normal", opacity=1.0, invert=False, bind=None, **values):
    """A generator: a gen* graph or any texture node, its inputs bound to mesh maps by name unless bind says otherwise."""
    m = {"id": ID(), "kind": "generator", "name": name, "type": type, "mode": mode, "opacity": opacity, "invert": invert, "values": values}
    if bind:
        m["bindings"] = bind
    return m


def F(type, name, mode="normal", opacity=1.0, invert=False, **values):
    """A filter over the masks below it."""
    return {"id": ID(), "kind": "filter", "name": name, "type": type, "mode": mode, "opacity": opacity, "invert": invert, "values": values}


def LV(name="Levels", **values):
    return {"id": ID(), "kind": "levels", "name": name, "mode": "normal", "opacity": 1.0, "invert": False, "values": values}


def MF(value=1.0, name="Fill", mode="normal"):
    return {"id": ID(), "kind": "fill", "name": name, "value": value, "mode": mode, "opacity": 1.0, "invert": False}


# Building blocks -----------------------------------------------------------------------------------------------------

def remap(role, name, lo, hi, gamma=1.0, **kw):
    """A mesh map with its lo..hi range stretched to 0..1."""
    return G("levels", name, bind={"input": role}, inLow=lo, inHigh=hi, gamma=gamma, **kw)


def crevices(name, lo=0.55, hi=0.95, **kw):
    """Concave areas from the cavity map (1 is open surface), white in the crevices."""
    return G("levels", name, invert=True, bind={"input": "cavity"}, inLow=lo, inHigh=hi, **kw)


def facing(name, elevation=90.0, two_sided=False, **kw):
    """How much the surface faces up (elevation 90) or sideways (0, two-sided)."""
    return G("maskFacing", name, bind={"normal": "worldNormal"}, elevation=elevation, twoSided=two_sided, **kw)


def from_ground(name, top, breakup=0.4, scale=4, seed=0, **kw):
    """White at the bottom of the bake bounds fading out by top (0..1 of the height)."""
    return G("genPositionGradient", name, start=top, end=0.0, breakup=breakup, noiseScale=scale, seed=seed, **kw)


def from_top(name, bottom, breakup=0.4, scale=4, seed=0, **kw):
    return G("genPositionGradient", name, start=bottom, end=1.0, breakup=breakup, noiseScale=scale, seed=seed, **kw)


def clouds(name, scale, seed, mode="normal", opacity=1.0, kind="soft", octaves=5, contrast=1.0, invert=False):
    return {"id": ID(), "kind": "generator", "name": name, "type": "clouds", "mode": mode, "opacity": opacity, "invert": invert,
            "values": {"scale": scale, "octaves": octaves, "mode": kind, "contrast": contrast, "seed": seed}}


def cells(name, scale, seed, cmode="f1", mode="normal", opacity=1.0, invert=False, width=0.08, jitter=1.0):
    return {"id": ID(), "kind": "generator", "name": name, "type": "cells", "mode": mode, "opacity": opacity, "invert": invert,
            "values": {"scale": scale, "mode": cmode, "width": width, "jitter": jitter, "seed": seed}}


def thresh(name, t, soft=0.05, **kw):
    return F("threshold", name, threshold=t, softness=soft, **kw)


# Library -------------------------------------------------------------------------------------------------------------

MASKS = []


def mask(file, name, tags, desc):
    assert file.startswith("mask") and file[4].isupper(), file
    assert any(t in FAMILIES for t in tags), file
    assert all(t == t.lower() for t in tags), file

    def wrap(fn):
        MASKS.append((file, name, tags + ["mask"], desc, fn))
        return fn

    return wrap


# Dirt

@mask("maskDirtCavities", "Dirt in Cavities", ["dirt"], "Grime packed into crevices and recesses, lightly broken up.")
def _():
    return [G("genCavityGrime", "Cavity Grime", level=0.25, softness=0.5, breakup=0.4, noiseScale=12, seed=101),
            G("genDirt", "Spill", mode="lighten", opacity=0.7, level=0.4, softness=0.6, breakup=0.6, seed=102)]


@mask("maskDirtDry", "Dry Caked Dirt", ["dirt"], "Crusty, patchy dirt drying out in occluded areas with hard broken edges.")
def _():
    return [G("genDirt", "Occluded Dirt", level=0.4, softness=0.3, breakup=0.9, noiseScale=6, seed=111),
            G("dirt", "Crust", mode="multiply", scale=6, amount=0.65, contrast=4.0, grain=0.7, seed=112)]


@mask("maskDirtDusty", "Dusty Dirt", ["dirt", "dust"], "Soft, pale dirt in the recesses blending into a dust film on top.")
def _():
    return [G("genDirt", "Dirt", level=0.45, softness=0.8, breakup=0.4, noiseScale=4, seed=121),
            G("genDust", "Dust", mode="lighten", opacity=0.55, level=0.55, softness=0.7, breakup=0.3, seed=122),
            F("blur", "Soften", radius=0.006)]


@mask("maskDirtGroundUp", "Dirt Rising from the Ground", ["dirt", "ground"], "Dirt splashed up from the ground, thick at the base and thinning with height.")
def _():
    return [from_ground("Ground", 0.5, breakup=0.5, scale=5, seed=131),
            G("dirt", "Grain", mode="multiply", opacity=0.7, scale=5, amount=0.7, contrast=2.5, grain=0.6, seed=132),
            G("genDirt", "Occluded Dirt", mode="lighten", opacity=0.5, level=0.4, seed=133),
            LV("Range", inLow=0.1, inHigh=0.6)]


@mask("maskDirtHeavy", "Heavy Grime", ["dirt"], "Thick grime covering occluded areas and spreading well onto open surfaces.")
def _():
    return [G("genDirt", "Grime", level=0.22, softness=0.45, breakup=0.55, aoWeight=1.4, cavityWeight=0.9, noiseScale=4, seed=141)]


@mask("maskDirtLeaks", "Grime Leaking from Crevices", ["dirt", "drips"], "Grime collected in crevices and running down below them in streaks.")
def _():
    return [crevices("Crevices", 0.5, 0.95),
            F("directionalBlur", "Run Down", angle=90.0, length=0.08),
            LV("Strengthen", inLow=0.15, inHigh=0.5),
            G("genDrips", "Streaks", mode="multiply", opacity=1.0, density=36, length=2, level=0.45, softness=0.4, seed=151),
            G("genCavityGrime", "Cavity Grime", mode="lighten", level=0.3, softness=0.3, seed=152)]


@mask("maskDirtSoftEdges", "Soft Grime Halo", ["dirt", "occlusion"], "A soft halo of grime around recesses and contact areas, no hard lines.")
def _():
    return [G("genAoDarken", "Occlusion", level=0.25, softness=0.7),
            F("blur", "Soften", radius=0.02),
            G("dirt", "Grime Noise", mode="multiply", opacity=0.6, scale=4, amount=0.6, contrast=1.5, grain=0.3, seed=161)]


@mask("maskDirtSplashes", "Mud Splashes", ["dirt", "ground"], "Speckled mud splashes thrown up onto the lower part of a surface.")
def _():
    return [G("dirt", "Specks", scale=10, amount=0.35, contrast=6.0, grain=0.2, seed=171),
            from_ground("Lower Part", 0.5, breakup=0.6, scale=6, seed=172, mode="multiply"),
            F("dilate", "Spread", radius=0.002)]


@mask("maskDirtSpots", "Dirt Spots", ["dirt"], "Scattered dark spots of dirt across the whole surface, denser in occlusion.")
def _():
    return [cells("Spots", 18, 181, invert=True),
            thresh("Spot Size", 0.82, 0.08),
            clouds("Clusters", 4, 182, mode="multiply", contrast=1.6),
            G("genAoDarken", "Occlusion", mode="screen", opacity=0.3, level=0.4)]


@mask("maskDirtSharp", "Sharp Crevice Dirt", ["dirt"], "Dirt confined to the deepest crevices with crisp edges.")
def _():
    return [crevices("Deep Crevices", 0.72, 0.85),
            thresh("Crisp", 0.35, 0.1)]


@mask("maskDirtBuildup", "Grime Buildup on Upper Faces", ["dirt", "dust"], "Greasy grime settling on upward faces and in their corners.")
def _():
    return [facing("Upward", 90.0),
            LV("Range", inLow=0.4, inHigh=0.9),
            G("genAoDarken", "Corners", mode="multiply", invert=True, opacity=0.6, level=0.5, softness=0.8),
            G("dirt", "Grime Noise", mode="multiply", opacity=0.8, scale=4, amount=0.6, contrast=2.0, grain=0.4, seed=191)]


# Dust

@mask("maskDustOcclusion", "Dust in Occlusion", ["dust", "occlusion"], "Fine dust gathered in sheltered, occluded areas.")
def _():
    return [G("genAoDarken", "Occlusion", level=0.3, softness=0.6),
            clouds("Breakup", 6, 201, mode="multiply", opacity=0.5)]


@mask("maskDustSoft", "Soft Dust Cover", ["dust"], "An even, soft film of dust over everything facing up.")
def _():
    return [G("genDust", "Dust", level=0.4, softness=0.9, breakup=0.15, aoInfluence=0.3, noiseScale=4, seed=211)]


@mask("maskDustSubtle", "Light Dusting", ["dust"], "A faint, thin dusting on top surfaces for a barely used look.")
def _():
    return [G("genDust", "Dust", opacity=0.6, level=0.55, softness=0.6, breakup=0.5, noiseScale=8, seed=221)]


@mask("maskDustTop", "Settled Dust on Tops", ["dust"], "Dense dust settled on upward-facing surfaces with a firm boundary.")
def _():
    return [G("genDust", "Dust", level=0.5, softness=0.2, breakup=0.45, aoInfluence=0.8, noiseScale=6, seed=231)]


@mask("maskDustWideEdges", "Dust Along Wide Edges", ["dust", "edges"], "Dust caught in a broad band along edges and ridges.")
def _():
    return [G("genEdgeWear", "Edges", strength=1.5, level=0.3, softness=0.3, breakup=0.2, seed=241),
            F("blur", "Widen", radius=0.025),
            LV("Range", inLow=0.12, inHigh=0.5),
            clouds("Breakup", 5, 242, mode="multiply", opacity=0.6)]


@mask("maskDustStained", "Stained Dust", ["dust", "stains"], "Dust with blotchy tide marks where moisture has stained it.")
def _():
    return [G("genDust", "Dust", level=0.5, softness=0.6, breakup=0.3, seed=251),
            clouds("Blotches", 4, 252, mode="multiply", kind="billow", contrast=1.4),
            LV("Range", inLow=0.05, inHigh=0.7)]


# Edges

@mask("maskEdgeDamage", "Battered Edges", ["edges"], "Hard chips and dents knocked out of the edges.")
def _():
    return [G("genPaintChips", "Chips", level=0.7, softness=0.03, chipScale=22, edgeWeight=1.6, chipWeight=0.5, seed=301)]


@mask("maskEdgeScratched", "Scratched Edges", ["edges", "scratches"], "Edges worn bright with fine scratches trailing off them.")
def _():
    return [G("genEdgeWear", "Edge Band", strength=1.2, level=0.15, softness=0.5, breakup=0.2, seed=311),
            G("scratches", "Scratches", mode="multiply", scale=8, count=8, length=0.6, width=0.004, angleRandom=1.0, seed=312),
            G("genEdgeWear", "Worn Edge", mode="lighten", strength=2.5, level=0.4, softness=0.1, breakup=0.4, seed=313)]


@mask("maskEdgeStrong", "Heavy Edge Wear", ["edges"], "Wide, strong wear eating into every edge.")
def _():
    return [G("genEdgeWear", "Edges", strength=3.5, level=0.25, softness=0.12, breakup=0.3, noiseScale=8, seed=321)]


@mask("maskEdgeSoft", "Gentle Edge Wear", ["edges"], "Soft, faded wear along edges with gradual falloff.")
def _():
    return [G("genEdgeWear", "Edges", strength=1.2, level=0.25, softness=0.7, breakup=0.15, noiseScale=6, seed=331)]


@mask("maskEdgeBlurred", "Blurred Edge Glow", ["edges"], "A smooth, blurred band along edges, clean with no breakup.")
def _():
    return [G("genEdgeWear", "Edges", strength=2.0, level=0.25, softness=0.3, breakup=0.0, seed=341),
            F("blur", "Blur", radius=0.02),
            LV("Range", inLow=0.0, inHigh=0.6)]


@mask("maskEdgeChippedPaint", "Chipped Paint Edges", ["edges", "paint"], "Paint flaking off edges in sharp chips with clean wear between them.")
def _():
    return [G("genPaintChips", "Chips", level=0.66, softness=0.02, chipScale=16, seed=351),
            G("genEdgeWear", "Worn Line", mode="lighten", strength=3.0, level=0.45, softness=0.06, breakup=0.3, seed=352)]


@mask("maskEdgeRusty", "Rust-Eaten Edges", ["edges", "rust"], "Rust creeping in from the edges in blotchy patches.")
def _():
    return [G("genRustSpread", "Edge Rust", edgeWeight=1.8, cavityWeight=0.1, aoWeight=0.0, level=0.25, softness=0.35, breakup=1.1, noiseScale=7, seed=361),
            G("dirt", "Pitting", mode="multiply", opacity=0.6, scale=10, amount=0.7, contrast=2.0, grain=0.8, seed=362)]


@mask("maskEdgeStoneCracks", "Cracked Stone Edges", ["edges", "cracks", "concrete"], "Stone and concrete edges crumbling with cracks running in from them.")
def _():
    return [G("genEdgeWear", "Edge Zone", strength=1.2, level=0.15, softness=0.4, breakup=0.3, seed=371),
            F("blur", "Widen", radius=0.02),
            cells("Cracks", 9, 372, cmode="crackle", mode="multiply", width=0.06),
            G("genPaintChips", "Crumbles", mode="lighten", level=0.7, softness=0.03, chipScale=14, seed=373)]


@mask("maskEdgeFine", "Fine Edge Highlight", ["edges"], "A thin, crisp line on only the sharpest edges.")
def _():
    return [G("genEdgeWear", "Edge Line", strength=4.0, level=0.5, softness=0.05, breakup=0.05, seed=381)]


@mask("maskEdgeWornBroad", "Rubbed Edges", ["edges"], "Broad, uneven rubbing on edges and raised areas, like years of handling.")
def _():
    return [G("genEdgeWear", "Rubbed", strength=1.0, level=0.2, softness=0.35, breakup=0.55, noiseScale=4, seed=391)]


# Rust

@mask("maskRustCavity", "Rust in Crevices", ["rust"], "Rust starting in crevices and occluded corners.")
def _():
    return [G("genRustSpread", "Crevice Rust", edgeWeight=0.0, cavityWeight=1.6, aoWeight=0.8, level=0.3, softness=0.35, breakup=1.3, noiseScale=7, seed=401)]


@mask("maskRustSurface", "Surface Rust Patches", ["rust"], "Rust patches blooming across open surfaces.")
def _():
    return [clouds("Patches", 5, 411, kind="billow", contrast=1.3),
            F("histogramScan", "Coverage", position=0.62, contrast=0.75),
            G("dirt", "Pitting", mode="multiply", opacity=0.6, scale=8, amount=0.7, contrast=2.0, grain=0.8, seed=412)]


@mask("maskRustDrips", "Rust Streaks", ["rust", "drips"], "Rust running down from crevices and edges in long streaks.")
def _():
    return [G("genRustSpread", "Rust Sources", level=0.55, softness=0.2, breakup=0.6, seed=421),
            F("directionalBlur", "Run Down", angle=90.0, length=0.12),
            LV("Strengthen", inLow=0.15, inHigh=0.55),
            G("genDrips", "Streaks", mode="multiply", opacity=1.0, density=30, length=2, level=0.4, softness=0.5, seed=422),
            G("genRustSpread", "Rust", mode="lighten", level=0.55, softness=0.2, breakup=0.6, seed=421)]


@mask("maskRustGroundUp", "Rust from the Ground Up", ["rust", "ground"], "Rust climbing from the base where water sits, fading with height.")
def _():
    return [from_ground("Base", 0.8, breakup=0.7, scale=5, seed=431),
            clouds("Patches", 6, 432, mode="multiply", kind="billow", contrast=1.4),
            LV("Range", inLow=0.05, inHigh=0.45),
            G("genRustSpread", "Crevice Rust", mode="lighten", opacity=0.5, edgeWeight=0.2, level=0.6, seed=433)]


@mask("maskRustOxidation", "Light Oxidation", ["rust"], "A thin, uneven oxidation tarnish over the surface.")
def _():
    return [G("genRustSpread", "Tarnish", level=0.3, softness=0.8, breakup=0.6, noiseScale=8, seed=441),
            G("perlin", "Variation", mode="multiply", opacity=0.6, scale=6, octaves=6, seed=442),
            LV("Range", inLow=0.0, inHigh=0.8, outHigh=0.75)]


@mask("maskRustPitting", "Pitted Corrosion", ["rust"], "Small corrosion pits clustering into rough spots.")
def _():
    return [cells("Pits", 26, 451, invert=True),
            thresh("Pit Size", 0.78, 0.1),
            G("genRustSpread", "Spread", mode="multiply", level=0.35, softness=0.5, breakup=1.0, seed=452),
            G("genRustSpread", "Cores", mode="lighten", level=0.6, softness=0.15, breakup=1.0, seed=452)]


@mask("maskRustHeavy", "Heavy Corrosion", ["rust"], "Rust covering most of the surface, leaving only scattered clean islands.")
def _():
    return [G("genRustSpread", "Rust", level=0.3, softness=0.35, breakup=1.0, noiseScale=4, seed=461),
            clouds("Spread", 4, 462, mode="lighten", kind="billow", contrast=1.5),
            F("histogramScan", "Coverage", position=0.72, contrast=0.7)]


# Moss and growth

@mask("maskMossTop", "Moss from Above", ["moss"], "Moss growing over upward-facing surfaces in soft-edged patches.")
def _():
    return [G("genMoss", "Moss", level=0.55, softness=0.3, breakup=0.7, noiseScale=4, seed=501)]


@mask("maskMossCavities", "Moss in Crevices", ["moss"], "Moss tucked into crevices and sheltered joints.")
def _():
    return [crevices("Crevices", 0.5, 0.95),
            G("genAoDarken", "Shelter", mode="lighten", opacity=0.7, level=0.35, softness=0.5),
            G("dirt", "Clumps", mode="multiply", scale=7, amount=0.75, contrast=2.0, grain=0.5, seed=511),
            F("dilate", "Grow", radius=0.003)]


@mask("maskMossPatches", "Scattered Growth Patches", ["moss"], "Irregular patches of growth anywhere the surface faces up, with ragged borders.")
def _():
    return [clouds("Patches", 6, 521, kind="billow", contrast=1.5),
            F("histogramScan", "Coverage", position=0.68, contrast=0.85),
            facing("Upward", 60.0, mode="multiply", opacity=0.8)]


@mask("maskMossLichen", "Lichen Rosettes", ["moss"], "Round lichen rosettes dotted over exposed surfaces.")
def _():
    return [cells("Rosettes", 9, 531, invert=True, jitter=0.9),
            thresh("Rosette Size", 0.72, 0.08),
            clouds("Clusters", 3, 532, mode="multiply", contrast=1.8),
            F("histogramScan", "Coverage", position=0.4, contrast=0.6)]


@mask("maskMossDampBase", "Damp Growth at the Base", ["moss", "ground", "moisture"], "Green growth creeping up from the damp base of an object.")
def _():
    return [from_ground("Base", 0.6, breakup=0.8, scale=6, seed=541),
            G("genMoss", "Upper Moss", mode="lighten", opacity=0.5, level=0.7, softness=0.3, seed=542),
            clouds("Breakup", 8, 543, mode="multiply", kind="billow", opacity=0.5),
            LV("Range", inLow=0.05, inHigh=0.5)]


# Sand

@mask("maskSandCavities", "Sand in Crevices", ["sand"], "Fine grainy sand settled in crevices and low corners.")
def _():
    return [G("genCavityGrime", "Crevices", level=0.3, softness=0.5, breakup=0.5, noiseScale=20, seed=601),
            G("genAoDarken", "Corners", mode="lighten", opacity=0.6, level=0.35, softness=0.4),
            G("dirt", "Grains", mode="multiply", scale=24, amount=0.75, contrast=1.5, grain=1.0, seed=602)]


@mask("maskSandDust", "Windblown Sand", ["sand", "dust"], "Sand blown across top surfaces in streaks along the wind.")
def _():
    return [G("genDust", "Settled", level=0.45, softness=0.4, breakup=0.2, seed=611),
            G("directional", "Wind Streaks", mode="multiply", angle=10.0, scaleAlong=3, scaleAcross=20, octaves=4, seed=612),
            LV("Range", inLow=0.1, inHigh=0.6)]


@mask("maskSandDrift", "Sand Drifts at the Base", ["sand", "ground"], "Sand banked up against the base and caught on ledges.")
def _():
    return [from_ground("Bank", 0.3, breakup=0.3, scale=3, seed=621),
            G("genDust", "Ledges", mode="lighten", opacity=0.6, level=0.6, softness=0.3, seed=622),
            G("dirt", "Grains", mode="multiply", opacity=0.5, scale=20, amount=0.8, contrast=1.5, grain=1.0, seed=623)]


# Paint aging

@mask("maskPaintOld", "Old Paint Wear", ["paint"], "Paint worn thin on edges and in large tired patches.")
def _():
    return [clouds("Thin Patches", 4, 702, kind="billow", contrast=1.3),
            F("histogramScan", "Coverage", position=0.7, contrast=0.6),
            G("genEdgeWear", "Edges", mode="lighten", strength=1.6, level=0.3, softness=0.3, breakup=0.6, seed=701)]


@mask("maskPaintDamaged", "Damaged Paint", ["paint"], "Paint knocked off in sharp flakes across edges and open areas.")
def _():
    return [G("genPaintChips", "Edge Chips", level=0.58, softness=0.02, chipScale=14, chipWeight=0.9, seed=711),
            G("crystal", "Flakes", mode="lighten", scale=10, octaves=1, seed=712),
            thresh("Flake Edge", 0.6, 0.03),
            clouds("Damage Zones", 3, 713, mode="multiply", contrast=1.8),
            thresh("Crisp", 0.45, 0.04)]


@mask("maskPaintSmallCracks", "Fine Paint Cracks", ["paint", "cracks"], "A fine craquelure of cracks through aged paint, strongest in patches.")
def _():
    return [cells("Craquelure", 28, 721, cmode="crackle", width=0.05, jitter=0.9),
            clouds("Aged Areas", 3, 722, mode="multiply", contrast=1.5),
            LV("Range", inLow=0.05, inHigh=0.8)]


@mask("maskPaintSubtleScratches", "Subtle Paint Scratches", ["paint", "scratches"], "Faint, thin scratches scattered over a painted finish.")
def _():
    return [G("scratches", "Scratches", opacity=0.7, scale=6, count=4, length=0.5, width=0.002, angleRandom=1.0, seed=731)]


@mask("maskPaintFlaking", "Flaking Paint", ["paint"], "Large flakes peeling away in clusters, as on weathered boards.")
def _():
    return [cells("Flakes", 8, 741, invert=True, jitter=1.0),
            thresh("Flake Shape", 0.5, 0.02),
            clouds("Peeling Zones", 3, 742, mode="multiply", kind="billow", contrast=2.0),
            thresh("Crisp", 0.3, 0.02)]


@mask("maskPaintSunFaded", "Sun-Faded Paint", ["paint"], "Paint bleached where the sun strikes it, sheltered areas left untouched.")
def _():
    return [G("genSunBleach", "Sun", azimuth=45.0, elevation=55.0, aoInfluence=0.8, level=0.45, softness=0.6),
            clouds("Unevenness", 5, 751, mode="multiply", opacity=0.3)]


# Drips and leaks

@mask("maskDripsWater", "Water Streaks", ["drips", "moisture"], "Pale water streaks running down vertical surfaces.")
def _():
    return [G("genDrips", "Streaks", density=28, length=2, level=0.6, softness=0.3, seed=801)]


@mask("maskDripsGrime", "Grime Streaks", ["drips", "dirt"], "Dark grime washed down walls from the crevices above.")
def _():
    return [G("genDrips", "Streaks", density=16, length=3, level=0.62, softness=0.3, seed=811),
            G("genCavityGrime", "Sources", mode="lighten", opacity=0.7, level=0.3, softness=0.4, seed=812),
            G("genAoDarken", "Occlusion", mode="multiply", invert=True, opacity=0.4, level=0.3, softness=0.9)]


@mask("maskDripsRunoff", "Runoff Below Top Edges", ["drips"], "Runoff stains hanging from the top of the object, fading as they fall.")
def _():
    return [G("directional", "Runs", angle=90.0, scaleAlong=2, scaleAcross=40, octaves=4, seed=821),
            thresh("Run Width", 0.55, 0.2),
            from_top("From Top", 0.45, breakup=0.5, scale=10, seed=822, mode="multiply")]


@mask("maskDripsThin", "Thin Drip Lines", ["drips"], "Many thin, sharp drip lines, like dried paint or oil runs.")
def _():
    return [G("genDrips", "Lines", density=64, length=1, level=0.6, softness=0.12, seed=831)]


@mask("maskDripsLeak", "Leak Trails", ["drips", "stains"], "A few heavy leak trails with stained halos, as from a leaking seam.")
def _():
    return [G("genDrips", "Trails", density=10, length=3, level=0.66, softness=0.08, seed=841),
            F("blur", "Halo", radius=0.004),
            LV("Range", inLow=0.0, inHigh=0.7)]


# Stains

@mask("maskStainsWater", "Water Rings", ["stains", "moisture"], "Dried water marks with darker tide lines at their rims.")
def _():
    return [clouds("Puddles", 4, 901, contrast=1.5),
            thresh("Shape", 0.62, 0.02),
            LV("Faint Fill", outHigh=0.3),
            F("edgeDetect", "Rims", mode="lighten", width=2.0, intensity=3.0),
            F("blur", "Soften", radius=0.003)]


@mask("maskStainsBlotchy", "Blotchy Stains", ["stains"], "Large irregular stains with soft, uneven edges.")
def _():
    return [clouds("Blotches", 4, 911, kind="billow", contrast=1.6),
            F("histogramScan", "Coverage", position=0.6, contrast=0.55)]


@mask("maskStainsSpots", "Spot Stains", ["stains"], "Scattered small stains of varied size.")
def _():
    return [G("worley", "Spots", scale=10, octaves=2, jitter=1.0, invert=True, seed=921),
            thresh("Spot Size", 0.82, 0.1),
            G("perlin", "Fade", mode="multiply", opacity=0.5, scale=8, seed=922)]


@mask("maskStainsGrease", "Grease Smudges", ["stains", "dirt"], "Smeared grease marks around handled and occluded areas.")
def _():
    return [G("perlin", "Smears", scale=5, octaves=6, persistence=0.6, seed=931),
            F("directionalBlur", "Smear", angle=20.0, length=0.04),
            LV("Range", inLow=0.45, inHigh=0.7),
            G("genAoDarken", "Handled Areas", mode="multiply", opacity=0.6, level=0.5, softness=0.9)]


@mask("maskStainsTideLine", "Tide Line", ["stains", "moisture", "ground"], "A dried band left by a past water level, sharp on top and fading below.")
def _():
    return [G("genPositionGradient", "Band", start=0.45, end=0.36, breakup=0.1, noiseScale=8, seed=941),
            G("genPositionGradient", "Below", mode="multiply", start=0.15, end=0.45, breakup=0.0, seed=941),
            LV("Range", inLow=0.45, inHigh=0.7)]


# Scratches

@mask("maskScratchesFine", "Fine Scratches", ["scratches"], "Dense, fine scratches in every direction, as on handled metal.")
def _():
    return [G("scratches", "Scratches", scale=10, count=8, length=0.5, width=0.0015, angleRandom=1.0, seed=1001)]


@mask("maskScratchesBrushed", "Brushed Scratches", ["scratches"], "Long parallel scratches running one way, like brushed or sanded metal.")
def _():
    return [G("scratches", "Parallel", scale=8, count=12, length=1.2, width=0.0015, angle=0.0, angleRandom=0.03, seed=1011),
            G("anisotropic", "Grain", mode="lighten", opacity=0.2, scaleX=2, scaleY=96, octaves=3, seed=1012)]


@mask("maskScratchesDeep", "Deep Gouges", ["scratches"], "A few long, deep gouges cut across the surface.")
def _():
    return [G("scratches", "Gouges", scale=3, count=2, length=1.2, width=0.006, angleRandom=0.6, seed=1021),
            F("dilate", "Width", radius=0.002)]


@mask("maskScratchesScuffs", "Scuff Marks", ["scratches"], "Short, soft scuffs in clusters, as from shoes and crates.")
def _():
    return [G("anisotropic", "Scuffs", scaleX=6, scaleY=48, octaves=3, seed=1031),
            thresh("Scuff Width", 0.62, 0.1),
            clouds("Clusters", 4, 1032, mode="multiply", contrast=1.8),
            F("blur", "Soften", radius=0.002)]


# Cracks

@mask("maskCracksHairline", "Hairline Cracks", ["cracks"], "Sparse hairline cracks wandering across the surface.")
def _():
    return [cells("Cracks", 7, 1101, cmode="crackle", width=0.03, jitter=1.0),
            G("perlin", "Breaks", mode="multiply", scale=6, octaves=4, seed=1102),
            LV("Range", inLow=0.1, inHigh=0.45)]


@mask("maskCracksDry", "Dried Mud Cracks", ["cracks", "ground"], "A full network of shrinkage cracks, like dried mud or old plaster.")
def _():
    return [cells("Network", 14, 1111, cmode="crackle", width=0.1, jitter=1.0),
            cells("Fine Network", 32, 1112, cmode="crackle", mode="lighten", opacity=0.5, width=0.06)]


# Moisture

@mask("maskMoistureWetBase", "Wet from the Base", ["moisture", "ground"], "Wet where water wicks up from the ground, with a soft waterline.")
def _():
    return [G("genWetness", "Waterline", waterLine=0.55, softness=0.15, puddles=0.0, seed=1201)]


@mask("maskMoisturePuddles", "Puddles on Top", ["moisture"], "Puddles pooled on flat, upward-facing areas.")
def _():
    return [facing("Flat Tops", 90.0),
            LV("Flatness", inLow=0.6, inHigh=0.9),
            clouds("Pools", 4, 1211, mode="multiply", contrast=1.5),
            thresh("Shoreline", 0.25, 0.08)]


@mask("maskMoistureDamp", "Damp Patches", ["moisture"], "Damp, darkened patches gathering in sheltered areas.")
def _():
    return [G("genAoDarken", "Shelter", level=0.45, softness=0.8),
            clouds("Patches", 4, 1221, mode="multiply", contrast=1.6),
            F("histogramScan", "Coverage", position=0.7, contrast=0.4)]


@mask("maskMoistureDroplets", "Condensation Droplets", ["moisture"], "Small beads of condensation on side-facing surfaces.")
def _():
    return [cells("Beads", 30, 1231, invert=True, jitter=1.0),
            thresh("Bead Size", 0.84, 0.06),
            facing("Sides", 90.0, mode="multiply", invert=True),
            G("perlin", "Fog", mode="multiply", scale=4, seed=1232)]


# Occlusion

@mask("maskOcclusionSoft", "Soft Occlusion", ["occlusion"], "Broad, soft ambient occlusion for gentle shading.")
def _():
    return [G("genAoDarken", "Occlusion", level=0.4, softness=1.0)]


@mask("maskOcclusionContact", "Tight Contact Shadow", ["occlusion"], "Only the tightest contact areas and deep corners.")
def _():
    return [G("genAoDarken", "Contact", level=0.25, softness=0.2),
            crevices("Crevices", 0.7, 0.9, mode="lighten", opacity=0.6)]


@mask("maskOcclusionThin", "Thin Areas", ["occlusion"], "Thin parts of the mesh, where light would pass through.")
def _():
    return [G("genThickness", "Thin", level=0.35, softness=0.4, invert=True)]


# Fabric

@mask("maskFabricEdges", "Frayed Fabric Edges", ["fabric", "edges"], "Fabric worn at seams and folds with the weave showing through.")
def _():
    return [G("genEdgeWear", "Wear", strength=1.4, level=0.22, softness=0.45, breakup=0.6, seed=1401),
            G("weave", "Threads", mode="multiply", opacity=0.7, count=48, width=0.75)]


@mask("maskFabricThreads", "Loose Threads", ["fabric"], "Stray threads and pulled fibres scattered over the cloth.")
def _():
    return [G("scratches", "Threads", scale=5, count=3, length=0.35, width=0.0025, angleRandom=1.0, seed=1411),
            G("anisotropic", "Pulls", mode="lighten", opacity=0.6, scaleX=48, scaleY=3, octaves=2, seed=1412),
            thresh("Sparse", 0.45, 0.1)]


@mask("maskFabricFibres", "Fabric Fuzz", ["fabric"], "A fine fibrous fuzz that collects in folds and recesses.")
def _():
    return [G("dirt", "Fuzz", scale=32, amount=0.55, contrast=1.2, grain=1.0, seed=1421),
            G("genAoDarken", "Folds", mode="multiply", level=0.6, softness=0.9),
            LV("Range", inLow=0.0, inHigh=0.4)]


@mask("maskFabricCrumples", "Crumpled Creases", ["fabric"], "Sharp crease lines of crumpled cloth.")
def _():
    return [clouds("Creases", 5, 1431, kind="ridged", octaves=4, contrast=1.3),
            LV("Range", inLow=0.55, inHigh=0.95),
            G("genEdgeWear", "Fold Ridges", mode="lighten", opacity=0.5, strength=1.5, level=0.3, softness=0.3, seed=1432)]


@mask("maskFabricPilling", "Pilled Fabric", ["fabric"], "Little bobbles of pilled fibre on rubbed, raised areas.")
def _():
    return [cells("Pills", 40, 1441, invert=True),
            thresh("Pill Size", 0.8, 0.15),
            G("genEdgeWear", "Rubbed Areas", mode="multiply", strength=1.2, level=0.15, softness=0.6, breakup=0.5, seed=1442),
            F("blur", "Spread", radius=0.004),
            LV("Range", inLow=0.0, inHigh=0.5)]


# Concrete

@mask("maskConcreteEdges", "Chipped Concrete Edges", ["concrete", "edges"], "Concrete corners broken into chunky, angular chips.")
def _():
    return [G("genEdgeWear", "Edges", strength=1.8, level=0.25, softness=0.2, breakup=0.3, seed=1501),
            G("crystal", "Chunks", mode="multiply", scale=14, octaves=1, seed=1502),
            thresh("Chunk Edge", 0.3, 0.04)]


@mask("maskConcretePores", "Concrete Pores", ["concrete"], "Air holes and pores dotted over cast concrete.")
def _():
    return [cells("Pores", 34, 1511, invert=True, jitter=1.0),
            thresh("Pore Size", 0.9, 0.04),
            G("perlin", "Density", mode="multiply", scale=5, seed=1512),
            thresh("Keep", 0.2, 0.05)]


@mask("maskConcreteStreaks", "Concrete Weather Streaks", ["concrete", "drips", "stains"], "Dark weather streaks hanging below ledges on concrete and stone walls.")
def _():
    return [G("genDust", "Ledges", level=0.6, softness=0.3, breakup=0.3, seed=1521),
            F("directionalBlur", "Run Down", angle=90.0, length=0.1),
            G("directional", "Streak Texture", mode="multiply", angle=90.0, scaleAlong=3, scaleAcross=30, octaves=4, seed=1522),
            LV("Range", inLow=0.02, inHigh=0.35)]


# Write ---------------------------------------------------------------------------------------------------------------

def build():
    names = set()
    for file, name, tags, desc, fn in MASKS:
        assert name not in names, name
        names.add(name)
        ID.n = 0
        doc = {"format": "lilasmart", "version": 1, "name": name, "description": desc, "tags": tags, "kind": "mask", "masks": fn()}
        with open(os.path.join(OUT, file + ".lilasmart"), "w", newline="\n") as f:
            json.dump(doc, f, indent=2)
            f.write("\n")
    print(len(MASKS), "written")


if __name__ == "__main__":
    build()
