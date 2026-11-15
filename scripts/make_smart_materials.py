"""Writes Lila's built-in stylized and realistic smart materials into resources/smartmaterials. Colours are given in sRGB and stored linear."""
import json
import os
import sys

OUT = os.path.join(os.path.dirname(os.path.dirname(os.path.abspath(__file__))), "resources", "smartmaterials")

GRAY_CH = {"roughness", "metallic", "height", "ao", "opacity", "mask"}


def _lin(v):
    """Colours are authored in sRGB; the files hold linear values."""
    if v <= 0.04045:
        return v / 12.92
    return ((v + 0.055) / 1.055) ** 2.4


def rgb(c, raw=False):
    if raw:
        return [round(c[0], 4), round(c[1], 4), round(c[2], 4), 1]
    return [round(_lin(c[0]), 4), round(_lin(c[1]), 4), round(_lin(c[2]), 4), 1]


class Ids:
    def __init__(self):
        self.n = 0

    def __call__(self, p):
        self.n += 1
        return f"{p}{self.n}"


ID = Ids()


# Masks ---------------------------------------------------------------------------------------------------------------

def G(type, mode="normal", opacity=1.0, invert=False, **values):
    return {"id": ID("M"), "kind": "generator", "type": type, "mode": mode, "opacity": opacity, "invert": invert, "values": values}


def MF(value=1.0, mode="normal"):
    return {"id": ID("M"), "kind": "fill", "value": value, "mode": mode}


def LV(**values):
    return {"id": ID("M"), "kind": "levels", "mode": "normal", "opacity": 1.0, "values": values}


# Layers --------------------------------------------------------------------------------------------------------------

def fill(name, mat, masks=(), ch=None, hb=None, anchor=None):
    m = {}
    for k, v in mat.items():
        m[k] = rgb(v) if isinstance(v, (tuple, list)) else v
    l = {"id": ID("L"), "name": name, "kind": "fill", "material": m, "masks": list(masks)}
    if ch:
        l["channels"] = {k: ({"mode": v[0], "opacity": v[1]} if isinstance(v, tuple) else {"mode": v, "opacity": 1.0}) for k, v in ch.items()}
    if hb:
        l["heightBlend"] = {"enabled": True, "sharpness": hb[0], "influence": hb[1]}
    if anchor:
        l["anchor"] = anchor
    return l


def proc(name, g, masks=(), ch=None):
    l = {"id": ID("L"), "name": name, "kind": "procedural", "graph": g.json(), "masks": list(masks)}
    if ch:
        l["channels"] = {k: ({"mode": v[0], "opacity": v[1]} if isinstance(v, tuple) else {"mode": v, "opacity": 1.0}) for k, v in ch.items()}
    return l


class Gr:
    """A procedural layer's texture graph."""

    def __init__(self, name):
        self.name = name
        self.nodes = [{"name": "GroupInput", "type": "GroupInput", "position": [-800, 0]}]
        self.links = []
        self.outs = []
        self.k = 0
        self.params = []
        self.internal = set()

    def n(self, type, **values):
        self.k += 1
        name = f"{type[0].upper()}{type[1:]}{self.k}"
        self.nodes.append({"name": name, "type": type, "position": [-650 + 150 * (self.k % 6), 120 * (self.k // 6)], "values": values})
        return name

    def expose(self, name, type, default, targets, **limits):
        """A setting shown on the layer: changing it sets every target ("Node.param")."""
        self.params.append(dict({"name": name, "kind": "param", "type": type, "default": default, "targets": targets}, **limits))

    def l(self, a, b, bin, aout="out"):
        self.links.append([a, aout, b, bin])

    def out(self, ch, a, aout="out"):
        self.outs.append(ch)
        self.l(a, "GroupOutput", ch, aout)

    # Shorthands
    def tint(self, src, stops, srcOut="out", raw=False):
        c = self.n("colorize", gradient=[[p, rgb(col, raw)] for p, col in stops])
        self.l(src, c, "input", srcOut)
        return c

    def remap(self, src, lo, hi, srcOut="out"):
        return self.tint(src, [(0.0, (lo, lo, lo)), (1.0, (hi, hi, hi))], srcOut, raw=True)

    def mix(self, fg, bg, mask, mode="normal", opacity=1.0, maskOut="out", fgOut="out", bgOut="out"):
        b = self.n("blend", mode=mode, opacity=opacity)
        self.l(fg, b, "foreground", fgOut)
        self.l(bg, b, "background", bgOut)
        if mask:
            self.l(mask, b, "mask", maskOut)
        return b

    def pebbles(self, scale, seed, size=0.75, keep=0.45):
        """An irregular scattered-stone mask: warped cell dots thinned by noise."""
        c = self.n("cells", scale=scale, mode="f1", jitter=1.0, seed=seed)
        wn = self.n("perlin", scale=scale // 2 + 1, octaves=3, seed=seed + 1)
        w = self.n("warp", mode="gradient", intensity=0.35)
        self.l(c, w, "input")
        self.l(wn, w, "warp")
        inv = self.n("invert")
        self.l(w, inv, "input")
        dots = self.n("threshold", threshold=size, softness=0.04)
        self.l(inv, dots, "input")
        thin = self.n("perlin", scale=5, octaves=2, seed=seed + 2)
        tm = self.n("threshold", threshold=1.0 - keep, softness=0.05)
        self.l(thin, tm, "input")
        self.internal |= {f"{wn}.scale", f"{thin}.scale"}
        tag = "Pebble" if not any(q["name"].startswith("Pebble Scale") for q in self.params) else "Pebble 2"
        self.expose(f"{tag} Scale", "int", scale, [f"{c}.scale"], min=1, softMax=64)
        self.expose(f"{tag} Size", "float", size, [f"{dots}.threshold"], min=0.0, max=1.0)
        self.expose(f"{tag} Spacing", "float", 1.0 - keep, [f"{tm}.threshold"], min=0.0, max=1.0)
        return self.mix(dots, tm, None, mode="multiply")

    def const(self, v):
        return self.n("constant", value=v)

    def json(self):
        outs = [{"name": c, "type": "gray" if c in GRAY_CH else "color"} for c in self.outs]
        nodes = self.nodes + [{"name": "GroupOutput", "type": "GroupOutput", "position": [400, 0]}]
        return {"name": self.name, "typeVersion": 1, "inputs": self.settings(), "outputs": outs, "nodes": nodes, "links": self.links}

    NOISES = {"clouds": "Clouds", "perlin": "Noise", "simplex": "Noise", "valueNoise": "Noise", "cells": "Cells", "crystal": "Crystal",
              "worley": "Worley", "voronoi": "Voronoi", "dirt": "Grain", "directional": "Streaks", "scratches": "Scratches"}

    def settings(self):
        """The named settings, then a scale for every noise not already covered and one seed for them all."""
        out = list(self.params)
        covered = {t for q in out for t in q["targets"]} | self.internal
        counts = {}
        seeds = []
        for n in self.nodes:
            label = self.NOISES.get(n["type"])
            vals = n.get("values", {})
            if "seed" in vals:
                seeds.append(f'{n["name"]}.seed')
            if not label or "scale" not in vals or f'{n["name"]}.scale' in covered:
                continue
            counts[label] = counts.get(label, 0) + 1
            name = f"{label} Scale" if counts[label] == 1 else f"{label} Scale {counts[label]}"
            out.append({"name": name, "kind": "param", "type": "int", "default": vals["scale"], "targets": [f'{n["name"]}.scale'], "min": 1, "softMax": 64})
        if seeds:
            first = next(n for n in self.nodes if f'{n["name"]}.seed' == seeds[0])
            out.append({"name": "Seed", "kind": "param", "type": "seed", "default": first["values"]["seed"], "targets": seeds})
        return out


# Stylized finishing passes -------------------------------------------------------------------------------------------

def top_light(color, opacity=0.35, start=0.25, end=1.0, breakup=0.4, seed=0):
    return fill("Top Light", {"baseColor": color},
                [G("genPositionGradient", start=start, end=end, breakup=breakup, noiseScale=3, seed=seed)],
                ch={"baseColor": ("normal", opacity)})


def cavity_shade(color, opacity=0.85, level=0.35, seed=0):
    return fill("Cavity Shade", {"baseColor": color},
                [G("genAoDarken", level=level, softness=0.7),
                 G("genCavityGrime", mode="lighten", level=0.3, softness=0.4, breakup=0.15, noiseScale=6, seed=seed)],
                ch={"baseColor": ("multiply", opacity)})


def edge_light(color, rough=None, level=0.32, softness=0.2, breakup=0.2, strength=2.2, opacity=1.0, seed=0, extra=None):
    mat = {"baseColor": color}
    if rough is not None:
        mat["roughness"] = rough
    if extra:
        mat.update(extra)
    return fill("Edge Highlight", mat,
                [G("genEdgeWear", level=level, softness=softness, breakup=breakup, strength=strength, noiseScale=5, seed=seed)],
                ch={k: ("normal", opacity) for k in mat})


def soft_variation(color, opacity=0.35, scale=3, seed=0, kind="clouds", mode="normal"):
    """Large soft colour blotches through a noise mask."""
    if kind == "clouds":
        m = G("clouds", scale=scale, octaves=4, persistence=0.5, contrast=1.4, seed=seed)
    else:
        m = G("perlin", scale=scale, octaves=3, persistence=0.45, seed=seed)
    return fill("Colour Variation", {"baseColor": color}, [m], ch={"baseColor": (mode, opacity)})


# Materials -----------------------------------------------------------------------------------------------------------

MATS = []


def mat(file, name, tags, desc):
    def deco(fn):
        MATS.append((file, name, tags, desc, fn))
        return fn
    return deco


def base_fill(name, color, rough, metal=0.0, height=0.5, **extra):
    m = {"baseColor": color, "roughness": rough, "metallic": metal, "height": height}
    m.update(extra)
    return fill(name, m)


# ---------------- Stylized ----------------

@mat("stylizedMossyRock", "Stylized Mossy Rock", ["stone", "foliage", "stylized"],
     "Blue-grey painted rock with chunky cracks, pale chipped edges and bright moss tufts on top.")
def _():
    g = Gr("rock")
    tone = g.n("clouds", scale=3, octaves=4, persistence=0.45, contrast=1.3, seed=11)
    g.out("baseColor", g.tint(tone, [(0.0, (0.25, 0.27, 0.33)), (1.0, (0.42, 0.44, 0.5))]))
    cr = g.n("cells", scale=4, mode="f2MinusF1", jitter=0.9, seed=11)
    g.out("height", g.tint(cr, [(0.0, (0.25,) * 3), (0.12, (0.5,) * 3), (1.0, (0.72,) * 3)], raw=True))
    g.out("roughness", g.const(0.85))
    return [proc("Rock", g),
            cavity_shade((0.45, 0.4, 0.6), seed=3),
            edge_light((0.72, 0.74, 0.8), level=0.3, seed=3),
            top_light((0.55, 0.58, 0.62), 0.3),
            fill("Moss", {"baseColor": (0.36, 0.62, 0.12), "roughness": 0.95, "height": 0.6},
                 [G("genMoss", level=0.5, softness=0.2, breakup=0.6, noiseScale=3, seed=5)], hb=(0.6, 1.0)),
            fill("Moss Tips", {"baseColor": (0.6, 0.8, 0.25)},
                 [G("genMoss", level=0.62, softness=0.15, breakup=0.6, noiseScale=3, seed=5),
                  G("genPositionGradient", mode="multiply", start=0.4, end=1.0)],
                 ch={"baseColor": ("normal", 0.7)})]


@mat("stylizedCastleBrick", "Stylized Castle Brick", ["brick", "stylized"],
     "Warm sandstone bricks in varied tones with deep mortar, pale rounded edges and cool shadows.")
def _():
    g = Gr("bricks")
    b = g.n("bricks", rows=14, columns=6, offset=0.5, mortar=0.06, bevel=0.15, seed=7)
    col = g.tint(b, [(0.0, (0.55, 0.36, 0.26)), (0.5, (0.72, 0.52, 0.36)), (1.0, (0.84, 0.68, 0.46))], srcOut="random")
    mortar = g.n("color", color=rgb((0.24, 0.2, 0.2)))
    g.out("baseColor", g.mix(col, mortar, b, maskOut="mask"))
    g.out("height", g.remap(b, 0.3, 0.68, srcOut="mask"))
    g.out("roughness", g.const(0.8))
    return [proc("Bricks", g),
            soft_variation((0.85, 0.7, 0.5), 0.25, scale=4, seed=8),
            cavity_shade((0.5, 0.4, 0.55), seed=4),
            edge_light((0.95, 0.82, 0.62), level=0.3, seed=4),
            top_light((0.95, 0.85, 0.65), 0.25)]


@mat("stylizedCobblestone", "Stylized Cobblestone", ["stone", "stylized"],
     "Rounded grey-violet cobbles with dark gaps, sunlit tops and soft painted highlights.")
def _():
    g = Gr("cobbles")
    c = g.n("cells", scale=6, mode="f2MinusF1", jitter=0.85, seed=21)
    h = g.tint(c, [(0.0, (0.15,) * 3), (0.08, (0.45,) * 3), (0.35, (0.66,) * 3), (1.0, (0.72,) * 3)], raw=True)
    g.out("height", h)
    cc = g.n("cells", scale=6, mode="cells", jitter=0.85, seed=21)
    stones = g.tint(cc, [(0.0, (0.42, 0.4, 0.46)), (0.5, (0.5, 0.48, 0.5)), (1.0, (0.58, 0.55, 0.52))], srcOut="random")
    gap = g.n("color", color=rgb((0.16, 0.14, 0.18)))
    gapMask = g.n("threshold", threshold=0.06, softness=0.04)
    g.l(c, gapMask, "input")
    g.out("baseColor", g.mix(stones, gap, gapMask))
    g.out("roughness", g.const(0.82))
    return [proc("Cobbles", g),
            cavity_shade((0.45, 0.42, 0.62), seed=6),
            edge_light((0.78, 0.76, 0.74), level=0.34, seed=6),
            top_light((0.7, 0.68, 0.62), 0.3)]


@mat("stylizedOakPlanks", "Stylized Oak Planks", ["wood", "stylized"],
     "Honey-brown planks with bold painted grain, per-plank tone shifts and light-catching edges.")
def _():
    g = Gr("planks")
    p = g.n("planks", rows=9, planks=2, gap=0.05, bevel=0.15, seed=31)
    grain = g.n("directional", angle=0.0, scaleAlong=2, scaleAcross=20, octaves=3, persistence=0.5, seed=31)
    wood = g.tint(grain, [(0.0, (0.42, 0.24, 0.12)), (0.55, (0.6, 0.38, 0.2)), (1.0, (0.72, 0.5, 0.28))])
    shift = g.tint(p, [(0.0, (0.62, 0.6, 0.6)), (1.0, (1.0, 1.0, 1.0))], srcOut="random")
    woodv = g.mix(shift, wood, None, mode="multiply")
    gap = g.n("color", color=rgb((0.16, 0.08, 0.05)))
    g.out("baseColor", g.mix(woodv, gap, p, maskOut="mask"))
    gh = g.remap(grain, 0.46, 0.54)
    ph = g.remap(p, 0.25, 0.6, srcOut="mask")
    g.out("height", g.mix(gh, ph, None, mode="overlay"))
    g.out("roughness", g.const(0.75))
    return [proc("Planks", g),
            cavity_shade((0.55, 0.38, 0.38), seed=9),
            edge_light((0.9, 0.7, 0.45), level=0.33, seed=9),
            top_light((0.85, 0.66, 0.4), 0.25)]


@mat("stylizedTreeBark", "Stylized Tree Bark", ["wood", "stylized"],
     "Deep vertical bark ridges in warm browns with lit ridge tops and dark violet furrows.")
def _():
    g = Gr("bark")
    r = g.n("directional", angle=90.0, scaleAlong=2, scaleAcross=16, octaves=3, persistence=0.45, seed=41)
    w = g.n("clouds", scale=4, octaves=3, mode="ridged", seed=41)
    warped = g.n("warp", mode="gradient", intensity=0.6)
    g.l(r, warped, "input")
    g.l(w, warped, "warp")
    g.out("height", g.tint(warped, [(0.0, (0.1,) * 3), (0.35, (0.25,) * 3), (0.5, (0.6,) * 3), (1.0, (0.8,) * 3)], raw=True))
    g.out("baseColor", g.tint(warped, [(0.0, (0.08, 0.05, 0.05)), (0.38, (0.16, 0.09, 0.07)), (0.5, (0.4, 0.25, 0.16)), (1.0, (0.58, 0.4, 0.26))]))
    g.out("roughness", g.const(0.9))
    return [proc("Bark", g),
            soft_variation((0.4, 0.42, 0.25), 0.25, scale=3, seed=42),
            cavity_shade((0.42, 0.32, 0.5), seed=10),
            edge_light((0.72, 0.56, 0.4), level=0.36, seed=10),
            top_light((0.65, 0.5, 0.35), 0.2)]


@mat("stylizedPaintedWood", "Stylized Painted Wood", ["wood", "paint", "stylized", "worn"],
     "Teal paint over planks, rubbed back to warm wood on the edges with chipped flakes.")
def _():
    g = Gr("planks")
    p = g.n("planks", rows=7, planks=1, gap=0.04, bevel=0.12, seed=51)
    grain = g.n("directional", angle=0.0, scaleAlong=2, scaleAcross=26, seed=51)
    wood = g.tint(grain, [(0.0, (0.45, 0.28, 0.15)), (1.0, (0.7, 0.5, 0.3))])
    gap = g.n("color", color=rgb((0.1, 0.07, 0.05)))
    g.out("baseColor", g.mix(wood, gap, p, maskOut="mask"))
    gh = g.remap(grain, 0.47, 0.53)
    ph = g.remap(p, 0.3, 0.6, srcOut="mask")
    g.out("height", g.mix(gh, ph, None, mode="overlay"))
    g.out("roughness", g.const(0.8))
    pg = Gr("paint")
    tone = pg.n("clouds", scale=3, octaves=3, seed=52)
    pg.out("baseColor", pg.tint(tone, [(0.0, (0.1, 0.42, 0.45)), (1.0, (0.18, 0.55, 0.55))]))
    pg.out("roughness", pg.const(0.6))
    pg.out("height", pg.const(0.53))
    return [proc("Wood", g),
            proc("Paint", pg, [MF(1.0),
                               G("genEdgeWear", mode="subtract", level=0.28, softness=0.1, breakup=0.6, noiseScale=6, seed=53),
                               G("genPaintChips", mode="subtract", level=0.8, softness=0.04, chipScale=10, seed=53)],
                 ch={"height": ("normal", 1.0)}),
            cavity_shade((0.45, 0.42, 0.55), seed=11),
            top_light((0.5, 0.78, 0.72), 0.25)]


@mat("stylizedGoldTrim", "Stylized Gold Trim", ["metal", "stylized"],
     "Saturated painted gold with near-white edge glints, amber falloff and deep brown recesses.")
def _():
    return [base_fill("Gold", (0.95, 0.66, 0.2), 0.32, 1.0),
            soft_variation((1.0, 0.82, 0.36), 0.4, scale=3, seed=61),
            cavity_shade((0.6, 0.35, 0.2), seed=12),
            edge_light((1.0, 0.93, 0.7), rough=0.2, level=0.3, softness=0.15, seed=12),
            top_light((1.0, 0.88, 0.5), 0.3)]


@mat("stylizedBronzeRelic", "Stylized Bronze Relic", ["metal", "stylized", "worn"],
     "Old bronze with turquoise patina pooled in the recesses and polished, light edges.")
def _():
    return [base_fill("Bronze", (0.62, 0.4, 0.2), 0.45, 1.0),
            soft_variation((0.45, 0.3, 0.16), 0.4, scale=4, seed=71),
            fill("Patina", {"baseColor": (0.25, 0.68, 0.58), "metallic": 0.0, "roughness": 0.85, "height": 0.52},
                 [G("genCavityGrime", level=0.2, softness=0.35, breakup=0.5, noiseScale=5, seed=72),
                  G("genAoDarken", mode="lighten", level=0.45, softness=0.6),
                  G("clouds", mode="multiply", scale=5, contrast=1.6, seed=72)]),
            edge_light((0.95, 0.75, 0.45), rough=0.25, level=0.3, seed=13),
            top_light((0.85, 0.65, 0.4), 0.2)]


@mat("stylizedIronPlate", "Stylized Iron Plate", ["metal", "stylized"],
     "Blue-black iron panels with bright steel edges, inky seams and a cool sheen on top.")
def _():
    g = Gr("plates")
    t = g.n("tiles", countX=2, countY=2, gap=0.025, bevel=0.06, seed=81)
    g.out("height", g.remap(t, 0.35, 0.58, srcOut="mask"))
    tone = g.n("clouds", scale=3, octaves=3, seed=81)
    g.out("baseColor", g.tint(tone, [(0.0, (0.2, 0.22, 0.28)), (1.0, (0.32, 0.34, 0.4))]))
    g.out("roughness", g.const(0.5))
    g.out("metallic", g.const(1.0))
    return [proc("Iron", g),
            cavity_shade((0.35, 0.35, 0.5), seed=14),
            edge_light((0.8, 0.84, 0.9), rough=0.3, level=0.3, softness=0.15, seed=14),
            top_light((0.5, 0.55, 0.65), 0.3)]


@mat("stylizedCopperPot", "Stylized Copper Pot", ["metal", "stylized"],
     "Glowing orange copper with rosy highlights, dark umber cavities and soft blotchy tone.")
def _():
    return [base_fill("Copper", (0.85, 0.42, 0.22), 0.35, 1.0),
            soft_variation((0.6, 0.25, 0.15), 0.45, scale=3, seed=91),
            cavity_shade((0.5, 0.28, 0.25), seed=15),
            edge_light((1.0, 0.75, 0.6), rough=0.22, level=0.3, seed=15),
            top_light((1.0, 0.65, 0.45), 0.25)]


@mat("stylizedGrassTurf", "Stylized Grass Turf", ["foliage", "ground", "stylized"],
     "Lush painted grass in layered greens, sun-yellowed on top and shaded blue-green below.")
def _():
    g = Gr("grass")
    blades = g.n("directional", angle=90.0, scaleAlong=6, scaleAcross=60, octaves=3, seed=101)
    clump = g.n("clouds", scale=5, octaves=3, seed=102)
    bc = g.tint(clump, [(0.0, (0.08, 0.28, 0.1)), (0.5, (0.2, 0.48, 0.12)), (1.0, (0.42, 0.66, 0.16))])
    bl = g.tint(blades, [(0.0, (0.7, 0.7, 0.7)), (1.0, (1.1, 1.1, 1.0))])
    g.out("baseColor", g.mix(bl, bc, None, mode="multiply"))
    g.out("height", g.remap(blades, 0.42, 0.58))
    g.out("roughness", g.const(0.9))
    return [proc("Grass", g),
            soft_variation((0.62, 0.72, 0.2), 0.35, scale=3, seed=103),
            soft_variation((0.1, 0.32, 0.12), 0.45, scale=5, seed=104),
            cavity_shade((0.3, 0.45, 0.45), seed=16),
            top_light((0.85, 0.85, 0.35), 0.25, start=0.35)]


@mat("stylizedDirtPath", "Stylized Dirt Path", ["ground", "stylized"],
     "Warm trodden earth scattered with pale pebbles and shaded with soft painted blotches.")
def _():
    g = Gr("dirt")
    tone = g.n("clouds", scale=4, octaves=4, seed=111)
    soil = g.tint(tone, [(0.0, (0.36, 0.24, 0.16)), (1.0, (0.55, 0.38, 0.24))])
    pm = g.pebbles(16, 112, size=0.78, keep=0.5)
    stone = g.n("color", color=rgb((0.7, 0.62, 0.52)))
    g.out("baseColor", g.mix(stone, soil, pm))
    hb = g.remap(tone, 0.45, 0.55)
    hp = g.remap(pm, 0.0, 0.3)
    g.out("height", g.mix(hp, hb, None, mode="add"))
    g.out("roughness", g.const(0.92))
    return [proc("Earth", g),
            soft_variation((0.3, 0.2, 0.14), 0.3, scale=3, seed=113),
            cavity_shade((0.5, 0.38, 0.45), seed=17),
            top_light((0.75, 0.58, 0.4), 0.25)]


@mat("stylizedDesertSand", "Stylized Desert Sand", ["sand", "stylized"],
     "Golden sand with wind-swept ripples, peach highlights on the crests and rosy shade.")
def _():
    g = Gr("sand")
    rip = g.n("directional", angle=15.0, scaleAlong=2, scaleAcross=22, octaves=1, persistence=0.4, seed=121)
    wv = g.n("perlin", scale=3, octaves=2, seed=122)
    w = g.n("warp", mode="gradient", intensity=0.4)
    g.l(rip, w, "input")
    g.l(wv, w, "warp")
    g.out("height", g.tint(w, [(0.35, (0.3,) * 3), (0.65, (0.7,) * 3)], raw=True))
    g.out("baseColor", g.tint(w, [(0.35, (0.72, 0.46, 0.26)), (0.55, (0.9, 0.68, 0.42)), (0.65, (0.99, 0.84, 0.58))]))
    g.out("roughness", g.const(0.9))
    return [proc("Dunes", g),
            soft_variation((0.85, 0.5, 0.35), 0.25, scale=2, seed=123),
            cavity_shade((0.75, 0.55, 0.55), seed=18),
            top_light((1.0, 0.88, 0.62), 0.3)]


@mat("stylizedSnowdrift", "Stylized Snowdrift", ["snow", "stylized"],
     "Soft white snow with powder-blue shadows, lumpy drifts and bright sparkling tops.")
def _():
    g = Gr("snow")
    lump = g.n("clouds", scale=4, octaves=3, mode="billow", seed=131)
    g.out("height", g.remap(lump, 0.4, 0.62))
    g.out("baseColor", g.tint(lump, [(0.0, (0.66, 0.76, 0.9)), (1.0, (0.93, 0.95, 0.98))]))
    g.out("roughness", g.const(0.7))
    return [proc("Snow", g),
            cavity_shade((0.6, 0.7, 0.92), seed=19),
            edge_light((1.0, 1.0, 1.0), level=0.35, seed=19),
            top_light((1.0, 1.0, 1.0), 0.45, start=0.3)]


@mat("stylizedGlacierIce", "Stylized Glacier Ice", ["snow", "crystal", "stylized"],
     "Glossy cyan ice with white fracture lines, deep blue hollows and frosted edges.")
def _():
    g = Gr("ice")
    cr = g.n("cells", scale=5, mode="crackle", width=0.03, seed=141)
    tone = g.n("clouds", scale=3, octaves=3, seed=142)
    ice = g.tint(tone, [(0.0, (0.15, 0.45, 0.7)), (1.0, (0.45, 0.78, 0.9))])
    white = g.n("color", color=rgb((0.85, 0.95, 1.0)))
    g.out("baseColor", g.mix(white, ice, cr))
    g.out("height", g.remap(cr, 0.52, 0.4))
    g.out("roughness", g.const(0.12))
    return [proc("Ice", g),
            cavity_shade((0.35, 0.45, 0.8), seed=20),
            edge_light((0.9, 0.97, 1.0), rough=0.5, level=0.33, seed=20),
            top_light((0.75, 0.92, 1.0), 0.3)]


@mat("stylizedAmethyst", "Stylized Amethyst", ["crystal", "stylized"],
     "Faceted violet crystal with a magenta inner glow, pale facet edges and deep purple shadows.")
def _():
    g = Gr("amethyst")
    f = g.n("crystal", scale=4, octaves=2, seed=151)
    g.out("baseColor", g.tint(f, [(0.0, (0.3, 0.12, 0.5)), (0.6, (0.55, 0.28, 0.8)), (1.0, (0.78, 0.55, 0.95))]))
    g.out("height", g.remap(f, 0.4, 0.6))
    g.out("roughness", g.const(0.15))
    glow = Gr("glow")
    n = glow.n("clouds", scale=3, octaves=2, contrast=1.5, seed=152)
    glow.out("emissive", glow.tint(n, [(0.0, (0.0, 0.0, 0.0)), (1.0, (0.45, 0.1, 0.5))]))
    return [proc("Facets", g),
            proc("Inner Glow", glow, ch={"emissive": ("normal", 1.0)}),
            cavity_shade((0.4, 0.25, 0.65), seed=21),
            edge_light((0.95, 0.85, 1.0), rough=0.3, level=0.3, seed=21)]


@mat("stylizedClothTunic", "Stylized Cloth Tunic", ["fabric", "stylized"],
     "Rich crimson woven cloth with soft folds of shade and a faded lighter top.")
def _():
    g = Gr("weave")
    w = g.n("weave", count=24, width=0.85)
    g.out("height", g.remap(w, 0.45, 0.56, srcOut="height"))
    tone = g.n("clouds", scale=4, octaves=3, seed=161)
    cloth = g.tint(tone, [(0.0, (0.5, 0.08, 0.1)), (1.0, (0.72, 0.15, 0.15))])
    thread = g.remap(w, 0.8, 1.0, srcOut="height")
    g.out("baseColor", g.mix(thread, cloth, None, mode="multiply"))
    g.out("roughness", g.const(0.9))
    return [proc("Cloth", g),
            cavity_shade((0.5, 0.3, 0.45), seed=22),
            edge_light((0.92, 0.45, 0.4), level=0.36, softness=0.3, seed=22, opacity=0.6),
            top_light((0.95, 0.4, 0.3), 0.25)]


@mat("stylizedLeatherStrap", "Stylized Leather Strap", ["leather", "stylized", "worn"],
     "Chestnut leather with tan scuffed edges, a faint pebbled grain and dark creases.")
def _():
    g = Gr("leather")
    grain = g.n("cells", scale=40, mode="f1", seed=171)
    g.out("height", g.remap(grain, 0.52, 0.47))
    tone = g.n("clouds", scale=4, octaves=4, seed=172)
    g.out("baseColor", g.tint(tone, [(0.0, (0.3, 0.14, 0.07)), (1.0, (0.48, 0.25, 0.12))]))
    g.out("roughness", g.const(0.65))
    return [proc("Leather", g),
            cavity_shade((0.45, 0.3, 0.3), seed=23),
            edge_light((0.82, 0.58, 0.36), rough=0.75, level=0.3, breakup=0.5, seed=23),
            top_light((0.7, 0.45, 0.25), 0.2)]


@mat("stylizedSkin", "Stylized Skin", ["skin", "stylized"],
     "Smooth warm skin with rosy blushes, peach highlights and soft red-violet shading.")
def _():
    return [base_fill("Skin", (0.88, 0.62, 0.5), 0.6),
            soft_variation((0.9, 0.45, 0.42), 0.3, scale=3, seed=181),
            cavity_shade((0.75, 0.45, 0.5), seed=24),
            edge_light((1.0, 0.82, 0.7), level=0.35, softness=0.35, seed=24, opacity=0.6),
            top_light((1.0, 0.82, 0.7), 0.25)]


@mat("stylizedToyPlastic", "Stylized Toy Plastic", ["plastic", "stylized", "clean"],
     "Glossy candy-yellow plastic with crisp white edge glints and warm orange shading.")
def _():
    return [base_fill("Plastic", (0.95, 0.72, 0.08), 0.25),
            cavity_shade((0.9, 0.5, 0.35), seed=25),
            edge_light((1.0, 0.97, 0.8), level=0.3, softness=0.12, breakup=0.05, seed=25),
            top_light((1.0, 0.9, 0.5), 0.3)]


@mat("stylizedGlazedTiles", "Stylized Glazed Tiles", ["ceramic", "stylized"],
     "Hand-glazed turquoise tiles of uneven tone with cream grout and glossy pale rims.")
def _():
    g = Gr("tiles")
    t = g.n("tiles", countX=8, countY=8, gap=0.05, bevel=0.15, seed=191)
    col = g.tint(t, [(0.0, (0.1, 0.45, 0.55)), (0.5, (0.15, 0.55, 0.6)), (1.0, (0.25, 0.65, 0.65))], srcOut="random")
    grout = g.n("color", color=rgb((0.8, 0.74, 0.62)))
    g.out("baseColor", g.mix(col, grout, t, maskOut="mask"))
    g.out("height", g.remap(t, 0.3, 0.62, srcOut="mask"))
    rg = g.n("color", color=rgb((0.12, 0.12, 0.12), True))
    rr = g.n("color", color=rgb((0.85, 0.85, 0.85), True))
    g.out("roughness", g.mix(rg, rr, t, maskOut="mask"))
    return [proc("Tiles", g),
            soft_variation((0.3, 0.75, 0.7), 0.25, scale=5, seed=192),
            cavity_shade((0.5, 0.5, 0.6), seed=26),
            edge_light((0.75, 0.95, 0.9), level=0.32, seed=26),
            top_light((0.6, 0.9, 0.85), 0.2)]


@mat("stylizedBone", "Stylized Bone", ["bone", "stylized"],
     "Ivory bone with cream highlights, ochre staining in the pores and warm brown recesses.")
def _():
    g = Gr("bone")
    pores = g.n("cells", scale=24, mode="f1", seed=201)
    g.out("height", g.remap(pores, 0.46, 0.53))
    tone = g.n("clouds", scale=4, octaves=4, seed=202)
    g.out("baseColor", g.tint(tone, [(0.0, (0.72, 0.62, 0.45)), (1.0, (0.9, 0.84, 0.68))]))
    g.out("roughness", g.const(0.6))
    return [proc("Bone", g),
            soft_variation((0.65, 0.5, 0.3), 0.3, scale=3, seed=203),
            cavity_shade((0.6, 0.45, 0.35), seed=27),
            edge_light((1.0, 0.97, 0.88), level=0.3, seed=27),
            top_light((1.0, 0.95, 0.85), 0.2)]


@mat("stylizedLavaRock", "Stylized Lava Rock", ["stone", "stylized"],
     "Charcoal volcanic rock split by glowing orange cracks, with warm rims and soot-dark hollows.")
def _():
    g = Gr("lava")
    c = g.n("cells", scale=5, mode="f2MinusF1", jitter=0.9, seed=211)
    g.out("height", g.tint(c, [(0.0, (0.2,) * 3), (0.1, (0.5,) * 3), (1.0, (0.7,) * 3)], raw=True))
    tone = g.n("clouds", scale=4, octaves=3, seed=212)
    g.out("baseColor", g.tint(tone, [(0.0, (0.1, 0.08, 0.09)), (1.0, (0.24, 0.2, 0.2))]))
    g.out("roughness", g.const(0.9))
    crack = g.n("threshold", threshold=0.045, softness=0.03)
    g.l(c, crack, "input")
    inv = g.n("invert")
    g.l(crack, inv, "input")
    g.out("emissive", g.tint(inv, [(0.0, (0.0, 0.0, 0.0)), (1.0, (1.0, 0.35, 0.05))]))
    return [proc("Rock", g),
            edge_light((0.45, 0.32, 0.28), level=0.3, seed=28),
            top_light((0.35, 0.3, 0.3), 0.25)]


@mat("stylizedPaintedMetal", "Stylized Painted Metal", ["metal", "paint", "stylized", "worn"],
     "Bold red enamel over steel, chipped to silver at the edges with dark grimy seams.")
def _():
    return [base_fill("Steel", (0.7, 0.72, 0.75), 0.35, 1.0),
            fill("Paint", {"baseColor": (0.75, 0.12, 0.1), "metallic": 0.0, "roughness": 0.45, "height": 0.52},
                 [MF(1.0),
                  G("genEdgeWear", mode="subtract", level=0.28, softness=0.08, breakup=0.5, noiseScale=6, seed=29),
                  G("genPaintChips", mode="subtract", level=0.78, softness=0.03, chipScale=14, seed=29)]),
            soft_variation((0.55, 0.08, 0.1), 0.3, scale=3, seed=221),
            cavity_shade((0.4, 0.3, 0.4), seed=29),
            top_light((1.0, 0.45, 0.35), 0.25)]


# ---------------- Realistic ----------------

@mat("brushedSteel", "Brushed Steel", ["metal", "realistic", "clean"],
     "Bright steel with fine linear brushing in the roughness and faint smudges.")
def _():
    g = Gr("brushed")
    a = g.n("anisotropic", scaleX=2, scaleY=180, octaves=4, persistence=0.6, seed=301)
    g.out("roughness", g.remap(a, 0.22, 0.4))
    g.out("height", g.remap(a, 0.495, 0.505))
    sm = Gr("smudges")
    c = sm.n("clouds", scale=4, contrast=1.5, seed=302)
    sm.out("roughness", sm.remap(c, 0.25, 0.45))
    return [base_fill("Steel", (0.84, 0.85, 0.86), 0.3, 1.0),
            proc("Brushing", g),
            proc("Smudges", sm, ch={"roughness": ("normal", 0.35)})]


@mat("polishedGold", "Polished Gold", ["metal", "realistic", "clean"],
     "Mirror-polished yellow gold with subtle polishing swirls and slightly duller recesses.")
def _():
    g = Gr("swirls")
    s = g.n("gabor", scale=10, impulses=8, frequency=3.0, angleRandom=1.0, seed=311)
    g.out("roughness", g.remap(s, 0.08, 0.16))
    return [base_fill("Gold", (1.0, 0.86, 0.5), 0.1, 1.0),
            proc("Polish Swirls", g),
            fill("Recess Dulling", {"roughness": 0.35, "baseColor": (0.9, 0.72, 0.4)},
                 [G("genAoDarken", level=0.4, softness=0.8)], ch={"roughness": ("normal", 0.6), "baseColor": ("normal", 0.5)})]


@mat("wornCopper", "Worn Copper", ["metal", "realistic", "worn"],
     "Oxidized copper darkened to brown, with verdigris in the crevices and bright worn edges.")
def _():
    g = Gr("oxide")
    c = g.n("clouds", scale=5, octaves=6, persistence=0.6, seed=321)
    g.out("baseColor", g.tint(c, [(0.0, (0.42, 0.2, 0.12)), (1.0, (0.7, 0.36, 0.22))]))
    g.out("roughness", g.remap(c, 0.6, 0.38))
    return [base_fill("Copper", (0.98, 0.8, 0.7), 0.3, 1.0),
            proc("Tarnish", g),
            fill("Verdigris", {"baseColor": (0.3, 0.55, 0.45), "metallic": 0.0, "roughness": 0.9, "height": 0.53},
                 [G("genRustSpread", edgeWeight=0.1, cavityWeight=1.2, aoWeight=0.6, level=0.4, softness=0.3, breakup=0.9, noiseScale=6, seed=322)],
                 hb=(0.6, 1.0)),
            edge_light((0.98, 0.78, 0.66), rough=0.25, level=0.32, softness=0.25, breakup=0.6, seed=323, extra={"metallic": 1.0})]


@mat("agedBronze", "Aged Bronze", ["metal", "realistic", "worn", "dirty"],
     "Dark statuary bronze with green-grey patina streaks, grime in the recesses and rubbed highlights.")
def _():
    return [base_fill("Bronze", (0.55, 0.38, 0.22), 0.45, 1.0),
            fill("Dark Patina", {"baseColor": (0.18, 0.15, 0.1), "roughness": 0.6, "metallic": 0.6},
                 [G("clouds", scale=4, octaves=6, contrast=1.2, seed=331)], ch={"baseColor": ("normal", 0.8), "roughness": ("normal", 0.8), "metallic": ("normal", 0.8)}),
            fill("Green Streaks", {"baseColor": (0.3, 0.42, 0.35), "metallic": 0.0, "roughness": 0.85},
                 [G("genDrips", density=30, length=3, wallsOnly=0.6, level=0.55, seed=332),
                  G("genCavityGrime", mode="lighten", level=0.25, breakup=0.6, seed=332)], ch={"baseColor": ("normal", 0.75), "metallic": ("normal", 0.75), "roughness": ("normal", 0.75)}),
            fill("Grime", {"baseColor": (0.06, 0.05, 0.04), "metallic": 0.0, "roughness": 0.9},
                 [G("genDirt", level=0.4, seed=333)], ch={"baseColor": ("normal", 0.7), "metallic": ("normal", 0.7), "roughness": ("normal", 0.7)}),
            edge_light((0.75, 0.55, 0.35), rough=0.3, level=0.3, breakup=0.7, seed=334, extra={"metallic": 1.0})]


@mat("castIron", "Cast Iron", ["metal", "realistic"],
     "Dark, rough cast iron with a pitted sand-cast surface and slightly smoother high points.")
def _():
    g = Gr("cast")
    d = g.n("dirt", scale=12, amount=0.5, contrast=2.0, grain=0.8, seed=341)
    p = g.n("worley", scale=30, octaves=2, invert=True, seed=341)
    g.out("height", g.mix(g.remap(p, 0.47, 0.53), g.remap(d, 0.47, 0.53), None, mode="overlay"))
    g.out("roughness", g.remap(d, 0.55, 0.8))
    g.out("baseColor", g.tint(d, [(0.0, (0.12, 0.12, 0.12)), (1.0, (0.24, 0.23, 0.22))]))
    return [base_fill("Iron", (0.2, 0.2, 0.2), 0.7, 1.0),
            proc("Casting", g),
            fill("Worn Highs", {"roughness": 0.4, "baseColor": (0.38, 0.38, 0.38)},
                 [G("genEdgeWear", level=0.35, softness=0.4, breakup=0.8, seed=342)])]


@mat("galvanizedSteel", "Galvanized Steel", ["metal", "realistic"],
     "Zinc-coated steel with a crystalline spangle pattern of varying sheen and a little white oxide.")
def _():
    g = Gr("spangle")
    c = g.n("cells", scale=10, mode="cells", jitter=1.0, seed=351)
    g.out("roughness", g.remap(c, 0.2, 0.55, srcOut="random"))
    g.out("baseColor", g.tint(c, [(0.0, (0.74, 0.76, 0.78)), (1.0, (0.88, 0.89, 0.9))], srcOut="random"))
    return [base_fill("Zinc", (0.81, 0.82, 0.84), 0.35, 1.0),
            proc("Spangle", g),
            fill("White Oxide", {"baseColor": (0.75, 0.76, 0.75), "metallic": 0.0, "roughness": 0.9},
                 [G("genDirt", level=0.45, breakup=0.9, noiseScale=6, seed=352)], ch={"baseColor": ("normal", 0.6), "metallic": ("normal", 0.6), "roughness": ("normal", 0.6)})]


@mat("darkWalnut", "Dark Walnut", ["wood", "realistic", "clean"],
     "Finished dark walnut with wavy fine grain, warm figure and a satin sheen.")
def _():
    g = Gr("walnut")
    grain = g.n("directional", angle=0.0, scaleAlong=3, scaleAcross=70, octaves=5, persistence=0.55, seed=361)
    fig = g.n("perlin", scale=3, octaves=3, seed=362)
    w = g.n("warp", mode="gradient", intensity=0.5)
    g.l(grain, w, "input")
    g.l(fig, w, "warp")
    g.out("baseColor", g.tint(w, [(0.0, (0.06, 0.035, 0.02)), (0.5, (0.14, 0.08, 0.045)), (1.0, (0.24, 0.14, 0.08))]))
    g.out("height", g.remap(w, 0.49, 0.51))
    g.out("roughness", g.remap(w, 0.32, 0.45))
    return [proc("Walnut", g),
            fill("Recess Darkening", {"baseColor": (0.6, 0.55, 0.5)}, [G("genAoDarken", level=0.4)], ch={"baseColor": ("multiply", 0.6)})]


@mat("weatheredPinePlanks", "Weathered Pine Planks", ["wood", "realistic", "worn"],
     "Sun-greyed pine boards with raised grain, splits between planks and dirt in the gaps.")
def _():
    g = Gr("pine")
    p = g.n("planks", rows=8, planks=2, gap=0.025, bevel=0.06, seed=371)
    grain = g.n("directional", angle=0.0, scaleAlong=2, scaleAcross=40, octaves=4, seed=371)
    wood = g.tint(grain, [(0.0, (0.3, 0.27, 0.22)), (0.5, (0.45, 0.4, 0.33)), (1.0, (0.58, 0.52, 0.43))])
    var = g.tint(p, [(0.0, (0.8, 0.78, 0.75)), (1.0, (1.05, 1.0, 0.95))], srcOut="random")
    gap = g.n("color", color=rgb((0.05, 0.04, 0.03)))
    g.out("baseColor", g.mix(g.mix(var, wood, None, mode="multiply"), gap, p, maskOut="mask"))
    g.out("height", g.mix(g.remap(grain, 0.44, 0.56), g.remap(p, 0.2, 0.55, srcOut="mask"), None, mode="overlay"))
    g.out("roughness", g.remap(grain, 0.9, 0.75))
    return [proc("Boards", g),
            fill("Sun Bleach", {"baseColor": (0.62, 0.6, 0.56)}, [G("genSunBleach", level=0.5)], ch={"baseColor": ("normal", 0.5)}),
            fill("Dirt", {"baseColor": (0.12, 0.1, 0.07), "roughness": 0.95}, [G("genDirt", level=0.4, breakup=0.8, seed=372)], ch={"baseColor": ("normal", 0.45), "roughness": ("normal", 0.45)})]


@mat("pineBark", "Pine Bark", ["wood", "realistic"],
     "Thick plated pine bark with reddish-brown scales separated by deep dark fissures.")
def _():
    g = Gr("bark")
    c = g.n("cells", scale=7, mode="f2MinusF1", jitter=1.0, metric="manhattan", seed=381)
    g.out("height", g.tint(c, [(0.0, (0.1,) * 3), (0.1, (0.45,) * 3), (0.5, (0.62,) * 3), (1.0, (0.68,) * 3)], raw=True))
    d = g.n("dirt", scale=8, amount=0.5, contrast=2.0, seed=382)
    plate = g.tint(d, [(0.0, (0.2, 0.11, 0.07)), (1.0, (0.42, 0.24, 0.15))])
    fis = g.n("color", color=rgb((0.04, 0.03, 0.025)))
    m = g.n("threshold", threshold=0.08, softness=0.08)
    g.l(c, m, "input")
    g.out("baseColor", g.mix(plate, fis, m))
    g.out("roughness", g.const(0.92))
    return [proc("Bark", g),
            fill("Lichen", {"baseColor": (0.45, 0.5, 0.38), "roughness": 0.95},
                 [G("genMoss", level=0.78, softness=0.2, breakup=0.9, noiseScale=8, seed=383)], ch={"baseColor": ("normal", 0.4)})]


@mat("speckledGranite", "Speckled Granite", ["stone", "realistic"],
     "Polished grey granite flecked with black, white and pink mineral grains.")
def _():
    g = Gr("granite")
    base = g.n("clouds", scale=5, octaves=4, seed=391)
    stone = g.tint(base, [(0.0, (0.42, 0.4, 0.4)), (1.0, (0.58, 0.56, 0.55))])
    dark = g.n("cells", scale=110, mode="cells", seed=392)
    pink = g.n("cells", scale=80, mode="cells", seed=393)
    dm = g.n("threshold", threshold=0.75, softness=0.02)
    g.l(dark, dm, "input", "random")
    pm = g.n("threshold", threshold=0.8, softness=0.02)
    g.l(pink, pm, "input", "random")
    c1 = g.mix(g.n("color", color=rgb((0.06, 0.06, 0.07))), stone, dm)
    g.out("baseColor", g.mix(g.n("color", color=rgb((0.66, 0.48, 0.44))), c1, pm))
    g.out("roughness", g.remap(dm, 0.2, 0.3))
    g.out("height", g.remap(base, 0.495, 0.505))
    return [proc("Granite", g)]


@mat("redBrickWall", "Red Brick Wall", ["brick", "realistic", "dirty"],
     "Fired red bricks of uneven colour in recessed grey mortar, stained with soot and grime.")
def _():
    g = Gr("bricks")
    b = g.n("bricks", rows=12, columns=5, offset=0.5, mortar=0.035, bevel=0.06, seed=401)
    var = g.tint(b, [(0.0, (0.3, 0.1, 0.06)), (0.5, (0.42, 0.16, 0.09)), (1.0, (0.5, 0.24, 0.14))], srcOut="random")
    d = g.n("dirt", scale=10, amount=0.5, contrast=2.0, grain=0.7, seed=402)
    pits = g.remap(d, 0.75, 1.0)
    brick = g.mix(pits, var, None, mode="multiply")
    mortar = g.tint(d, [(0.0, (0.38, 0.36, 0.33)), (1.0, (0.5, 0.48, 0.45))])
    g.out("baseColor", g.mix(brick, mortar, b, maskOut="mask"))
    g.out("height", g.mix(g.remap(b, 0.3, 0.6, srcOut="mask"), g.remap(d, 0.47, 0.53), None, mode="overlay"))
    g.out("roughness", g.const(0.85))
    return [proc("Bricks", g),
            fill("Soot", {"baseColor": (0.05, 0.04, 0.04), "roughness": 0.9},
                 [G("genPositionGradient", invert=True, start=0.0, end=0.6, breakup=0.8, noiseScale=5, seed=403),
                  G("clouds", mode="multiply", scale=4, contrast=1.3, seed=403)], ch={"baseColor": ("normal", 0.6)}),
            fill("Grime", {"baseColor": (0.08, 0.07, 0.05), "roughness": 0.95}, [G("genCavityGrime", level=0.3, seed=404)], ch={"baseColor": ("normal", 0.7)})]


@mat("pouredConcrete", "Poured Concrete", ["concrete", "realistic"],
     "Grey cast concrete with tiny air pores, blotchy curing stains and faint water marks.")
def _():
    g = Gr("concrete")
    c = g.n("clouds", scale=6, octaves=6, persistence=0.6, seed=411)
    con = g.tint(c, [(0.0, (0.38, 0.38, 0.37)), (1.0, (0.55, 0.55, 0.53))])
    pores = g.n("cells", scale=70, mode="f1", seed=412)
    pm = g.n("threshold", threshold=0.08, softness=0.04)
    g.l(pores, pm, "input")
    pinv = g.n("invert")
    g.l(pm, pinv, "input")
    g.out("baseColor", g.mix(g.n("color", color=rgb((0.18, 0.18, 0.18))), con, pinv))
    g.out("height", g.mix(g.remap(pinv, 0.5, 0.35), g.remap(c, 0.49, 0.51), None, mode="overlay"))
    g.out("roughness", g.remap(c, 0.95, 0.82))
    return [proc("Concrete", g),
            fill("Water Marks", {"baseColor": (0.3, 0.3, 0.29), "roughness": 0.75},
                 [G("genDrips", density=20, length=3, level=0.6, seed=413)], ch={"baseColor": ("normal", 0.35), "roughness": ("normal", 0.35)}),
            fill("Dirt", {"baseColor": (0.2, 0.18, 0.15)}, [G("genDirt", level=0.4, seed=414)], ch={"baseColor": ("normal", 0.5)})]


@mat("muddyGround", "Muddy Ground", ["ground", "realistic", "dirty"],
     "Dark wet soil with small stones and glossy puddles collecting in the low spots.")
def _():
    g = Gr("soil")
    c = g.n("clouds", scale=5, octaves=6, seed=421)
    soil = g.tint(c, [(0.0, (0.16, 0.11, 0.08)), (1.0, (0.32, 0.24, 0.17))])
    sm = g.pebbles(22, 422, size=0.82, keep=0.4)
    g.out("baseColor", g.mix(g.n("color", color=rgb((0.3, 0.27, 0.24))), soil, sm))
    g.out("height", g.mix(g.remap(sm, 0.0, 0.25), g.remap(c, 0.4, 0.6), None, mode="add"))
    g.out("roughness", g.remap(c, 0.6, 0.9))
    return [proc("Soil", g),
            fill("Puddles", {"baseColor": (0.06, 0.05, 0.04), "roughness": 0.05, "height": 0.45},
                 [G("genWetness", waterLine=0.25, puddles=1.0, puddleScale=3, seed=423),
                  G("clouds", mode="multiply", scale=3, contrast=2.5, seed=423)],
                 ch={"height": ("normal", 1.0)})]


@mat("beachSand", "Beach Sand", ["sand", "realistic"],
     "Fine pale sand with soft wind ripples, a sprinkling of darker grains and shell fragments.")
def _():
    g = Gr("sand")
    grain = g.n("dirt", scale=40, amount=0.5, contrast=1.5, grain=1.0, seed=431)
    rip = g.n("directional", angle=80.0, scaleAlong=2, scaleAcross=18, octaves=2, seed=432)
    base = g.tint(grain, [(0.0, (0.55, 0.48, 0.37)), (1.0, (0.78, 0.71, 0.58))])
    g.out("baseColor", base)
    g.out("height", g.mix(g.remap(rip, 0.43, 0.57), g.remap(grain, 0.48, 0.52), None, mode="overlay"))
    g.out("roughness", g.const(0.95))
    return [proc("Sand", g),
            fill("Shell Bits", {"baseColor": (0.9, 0.86, 0.8), "roughness": 0.6},
                 [G("splatter", countX=10, countY=10, perCell=2, size=0.12, sizeRandom=0.6, seed=433)], ch={"baseColor": ("normal", 0.8)}),
            fill("Damp", {"baseColor": (0.4, 0.34, 0.25), "roughness": 0.6},
                 [G("genPositionGradient", invert=True, start=0.0, end=0.4, breakup=0.6, seed=434)], ch={"baseColor": ("normal", 0.6), "roughness": ("normal", 0.6)})]


@mat("packedSnow", "Packed Snow", ["snow", "realistic"],
     "Compacted white snow with fine glittering crust, soft lumps and blue-grey hollows.")
def _():
    g = Gr("snow")
    c = g.n("clouds", scale=6, octaves=6, mode="billow", seed=441)
    crust = g.n("dirt", scale=50, amount=0.5, contrast=1.5, grain=1.0, seed=442)
    g.out("height", g.mix(g.remap(crust, 0.48, 0.52), g.remap(c, 0.42, 0.58), None, mode="overlay"))
    g.out("baseColor", g.tint(c, [(0.0, (0.7, 0.74, 0.8)), (1.0, (0.9, 0.91, 0.93))]))
    g.out("roughness", g.remap(crust, 0.85, 0.5))
    return [proc("Snow", g),
            fill("Shadowed Hollows", {"baseColor": (0.75, 0.82, 0.95)}, [G("genAoDarken", level=0.4)], ch={"baseColor": ("multiply", 0.8)})]


@mat("forestMoss", "Forest Moss", ["foliage", "realistic"],
     "Dense springy moss in deep and yellow greens, with dry brown patches and fine fibres.")
def _():
    g = Gr("moss")
    fib = g.n("dirt", scale=40, amount=0.5, contrast=1.2, grain=1.0, seed=451)
    clump = g.n("clouds", scale=6, octaves=5, mode="billow", seed=452)
    base = g.tint(clump, [(0.0, (0.14, 0.22, 0.06)), (0.6, (0.28, 0.4, 0.1)), (1.0, (0.45, 0.52, 0.14))])
    g.out("baseColor", g.mix(g.remap(fib, 0.7, 1.05), base, None, mode="multiply"))
    g.out("height", g.mix(g.remap(fib, 0.46, 0.54), g.remap(clump, 0.4, 0.62), None, mode="overlay"))
    g.out("roughness", g.const(0.95))
    return [proc("Moss", g),
            fill("Dry Patches", {"baseColor": (0.32, 0.24, 0.12)},
                 [G("perlin", scale=4, octaves=4, seed=453), LV(inLow=0.55, inHigh=0.75)], ch={"baseColor": ("normal", 0.8)})]


@mat("cottonCanvas", "Cotton Canvas", ["fabric", "realistic", "clean"],
     "Undyed cotton canvas with a tight plain weave, slubby threads and soft matte finish.")
def _():
    g = Gr("canvas")
    w = g.n("weave", count=64, width=0.85)
    slub = g.n("anisotropic", scaleX=2, scaleY=64, octaves=3, seed=461)
    base = g.tint(slub, [(0.0, (0.66, 0.62, 0.54)), (1.0, (0.8, 0.77, 0.69))])
    g.out("baseColor", g.mix(g.remap(w, 0.75, 1.0, srcOut="height"), base, None, mode="multiply"))
    g.out("height", g.remap(w, 0.44, 0.56, srcOut="height"))
    g.out("roughness", g.const(0.95))
    return [proc("Canvas", g)]


@mat("indigoDenim", "Indigo Denim", ["fabric", "realistic", "worn"],
     "Indigo twill denim with white weft showing through, faded on the raised areas and seams.")
def _():
    g = Gr("denim")
    w = g.n("weave", count=80, width=0.9)
    tw = g.n("directional", angle=45.0, scaleAlong=1, scaleAcross=120, octaves=2, seed=471)
    indigo = g.tint(tw, [(0.0, (0.1, 0.15, 0.28)), (1.0, (0.2, 0.28, 0.45))])
    weft = g.n("color", color=rgb((0.6, 0.62, 0.66)))
    dir = g.remap(w, 0.0, 0.15, srcOut="direction")
    g.out("baseColor", g.mix(weft, indigo, dir))
    g.out("height", g.mix(g.remap(w, 0.45, 0.55, srcOut="height"), g.remap(tw, 0.47, 0.53), None, mode="overlay"))
    g.out("roughness", g.const(0.92))
    return [proc("Twill", g),
            fill("Fade", {"baseColor": (0.55, 0.62, 0.75)},
                 [G("genEdgeWear", level=0.3, softness=0.5, breakup=0.8, noiseScale=4, seed=472),
                  G("clouds", mode="lighten", scale=3, contrast=1.0, opacity=0.4, seed=473)], ch={"baseColor": ("normal", 0.7)})]


@mat("chunkyWool", "Chunky Wool", ["fabric", "realistic"],
     "Thick heather-grey wool in a chunky basket weave with a soft, fuzzy matte finish.")
def _():
    g = Gr("knit")
    k = g.n("weave", count=14, width=0.95)
    heather = g.n("dirt", scale=30, amount=0.5, contrast=1.0, grain=1.0, seed=483)
    base = g.tint(heather, [(0.0, (0.5, 0.48, 0.5)), (1.0, (0.62, 0.6, 0.6))])
    shade = g.remap(k, 0.3, 1.0, srcOut="height")
    g.out("baseColor", g.mix(shade, base, None, mode="multiply"))
    g.out("height", g.remap(k, 0.3, 0.7, srcOut="height"))
    g.out("roughness", g.const(1.0))
    return [proc("Knit", g)]


@mat("silkSatin", "Silk Satin", ["fabric", "realistic", "clean"],
     "Lustrous emerald satin with a smooth sheen running along the threads and faint creases.")
def _():
    g = Gr("satin")
    a = g.n("anisotropic", scaleX=1, scaleY=256, octaves=3, persistence=0.5, seed=491)
    cr = g.n("clouds", scale=3, octaves=3, mode="ridged", seed=492)
    g.out("baseColor", g.tint(cr, [(0.0, (0.02, 0.2, 0.12)), (1.0, (0.05, 0.32, 0.2))]))
    g.out("roughness", g.remap(a, 0.22, 0.35))
    g.out("height", g.mix(g.remap(cr, 0.47, 0.53), g.remap(a, 0.495, 0.505), None, mode="overlay"))
    return [proc("Satin", g)]


@mat("agedLeather", "Aged Leather", ["leather", "realistic", "worn"],
     "Brown full-grain leather with pebbled pores, darkened creases and lighter rubbed edges.")
def _():
    g = Gr("leather")
    pores = g.n("cells", scale=50, mode="f1", jitter=1.0, seed=501)
    cr = g.n("cells", scale=12, mode="crackle", width=0.02, seed=502)
    tone = g.n("clouds", scale=5, octaves=5, seed=503)
    base = g.tint(tone, [(0.0, (0.12, 0.06, 0.03)), (1.0, (0.26, 0.14, 0.07))])
    g.out("baseColor", g.mix(g.n("color", color=rgb((0.07, 0.04, 0.02))), base, cr, opacity=0.6))
    g.out("height", g.mix(g.remap(pores, 0.53, 0.47), g.remap(cr, 0.5, 0.44), None, mode="overlay"))
    g.out("roughness", g.remap(tone, 0.7, 0.5))
    return [proc("Leather", g),
            fill("Rubbed", {"baseColor": (0.4, 0.25, 0.14), "roughness": 0.4},
                 [G("genEdgeWear", level=0.3, softness=0.45, breakup=0.7, seed=504)], ch={"baseColor": ("normal", 0.8), "roughness": ("normal", 0.8)}),
            fill("Grime", {"baseColor": (0.04, 0.03, 0.02), "roughness": 0.8}, [G("genCavityGrime", level=0.3, seed=505)], ch={"baseColor": ("normal", 0.6)})]


@mat("naturalSkin", "Natural Skin", ["skin", "realistic"],
     "Light human skin with fine pores, faint freckles and redness variation.")
def _():
    g = Gr("skin")
    pores = g.n("cells", scale=90, mode="f1", seed=511)
    wr = g.n("anisotropic", scaleX=60, scaleY=8, octaves=3, seed=512)
    tone = g.n("clouds", scale=4, octaves=4, seed=513)
    base = g.tint(tone, [(0.0, (0.68, 0.45, 0.36)), (1.0, (0.78, 0.56, 0.46))])
    fr = g.n("cells", scale=140, mode="cells", seed=514)
    fm = g.n("threshold", threshold=0.93, softness=0.03)
    g.l(fr, fm, "input", "random")
    g.out("baseColor", g.mix(g.n("color", color=rgb((0.5, 0.3, 0.2))), base, fm, opacity=0.3))
    g.out("height", g.mix(g.remap(pores, 0.52, 0.48), g.remap(wr, 0.49, 0.51), None, mode="overlay"))
    g.out("roughness", g.remap(pores, 0.38, 0.5))
    return [proc("Skin", g),
            soft_variation((0.75, 0.38, 0.33), 0.3, scale=4, seed=515)]


@mat("matteRubber", "Matte Rubber", ["rubber", "realistic", "worn"],
     "Black matte rubber with a fine moulded grain, grey scuffs and dust in the recesses.")
def _():
    g = Gr("rubber")
    grain = g.n("dirt", scale=50, amount=0.5, contrast=1.2, grain=1.0, seed=521)
    g.out("height", g.remap(grain, 0.49, 0.51))
    g.out("roughness", g.remap(grain, 0.75, 0.9))
    return [base_fill("Rubber", (0.03, 0.03, 0.03), 0.85),
            proc("Grain", g),
            fill("Scuffs", {"baseColor": (0.12, 0.12, 0.12), "roughness": 0.6},
                 [G("genScratches", scale=4, count=4, length=0.5, width=0.008, angleRandom=1.0, seed=522)], ch={"baseColor": ("normal", 0.8), "roughness": ("normal", 0.8)}),
            fill("Dust", {"baseColor": (0.3, 0.28, 0.26), "roughness": 0.95}, [G("genDust", level=0.65, seed=523)], ch={"baseColor": ("normal", 0.6)})]


@mat("glossyAbsPlastic", "Glossy ABS Plastic", ["plastic", "realistic", "clean"],
     "Moulded white plastic with a light satin texture and faint fingerprint smudges.")
def _():
    g = Gr("texture")
    t = g.n("dirt", scale=60, amount=0.5, contrast=1.0, grain=1.0, seed=531)
    g.out("height", g.remap(t, 0.497, 0.503))
    sm = g.n("clouds", scale=6, contrast=1.8, seed=532)
    g.out("roughness", g.remap(sm, 0.22, 0.4))
    return [base_fill("Plastic", (0.82, 0.82, 0.8), 0.25), proc("Satin Texture", g)]


@mat("crackleGlaze", "Crackle Glaze Ceramic", ["ceramic", "realistic", "clean"],
     "Cream ceramic glaze with a fine crackle network darkened by age and a glassy finish.")
def _():
    g = Gr("glaze")
    cr = g.n("cells", scale=9, mode="crackle", width=0.012, seed=541)
    cr2 = g.n("cells", scale=22, mode="crackle", width=0.008, seed=542)
    tone = g.n("clouds", scale=3, octaves=3, seed=543)
    base = g.tint(tone, [(0.0, (0.74, 0.7, 0.6)), (1.0, (0.86, 0.83, 0.74))])
    line = g.n("color", color=rgb((0.35, 0.3, 0.24)))
    c1 = g.mix(line, base, cr, opacity=0.8)
    g.out("baseColor", g.mix(line, c1, cr2, opacity=0.4))
    g.out("height", g.remap(cr, 0.5, 0.47))
    g.out("roughness", g.const(0.06))
    return [proc("Glaze", g)]


@mat("weatheredBone", "Weathered Bone", ["bone", "realistic", "dirty"],
     "Old yellowed bone with porous pits, hairline cracks and earth stains in the hollows.")
def _():
    g = Gr("bone")
    pits = g.n("worley", scale=20, octaves=2, seed=551)
    cracks = g.n("anisotropic", scaleX=60, scaleY=3, octaves=2, seed=552)
    tone = g.n("clouds", scale=4, octaves=5, seed=553)
    g.out("baseColor", g.tint(tone, [(0.0, (0.55, 0.48, 0.35)), (1.0, (0.78, 0.72, 0.58))]))
    g.out("height", g.mix(g.remap(pits, 0.46, 0.53), g.remap(cracks, 0.47, 0.53), None, mode="overlay"))
    g.out("roughness", g.const(0.75))
    return [proc("Bone", g),
            fill("Earth Stain", {"baseColor": (0.25, 0.17, 0.1), "roughness": 0.9},
                 [G("genDirt", level=0.35, breakup=0.7, seed=554)], ch={"baseColor": ("normal", 0.7)}),
            fill("Pit Grime", {"baseColor": (0.15, 0.1, 0.06)}, [G("genCavityGrime", level=0.25, seed=555)], ch={"baseColor": ("normal", 0.6)})]


@mat("clearQuartz", "Clear Quartz", ["crystal", "realistic", "clean"],
     "Pale glassy quartz with sharp facets, milky inclusions and a near-mirror surface.")
def _():
    g = Gr("quartz")
    f = g.n("crystal", scale=3, octaves=2, seed=561)
    inc = g.n("clouds", scale=6, octaves=4, contrast=1.5, seed=562)
    base = g.tint(f, [(0.0, (0.55, 0.6, 0.66)), (0.7, (0.78, 0.82, 0.86)), (1.0, (0.92, 0.94, 0.96))])
    g.out("baseColor", g.mix(g.n("color", color=rgb((0.97, 0.97, 0.98))), base, inc, opacity=0.5))
    g.out("height", g.remap(f, 0.35, 0.65))
    g.out("roughness", g.remap(inc, 0.03, 0.12))
    return [proc("Quartz", g)]


@mat("chippedPaintedSteel", "Chipped Painted Steel", ["metal", "paint", "realistic", "worn"],
     "Olive-green industrial paint over steel, flaking at the edges with rust blooming in the chips.")
def _():
    return [base_fill("Steel", (0.5, 0.5, 0.5), 0.45, 1.0),
            fill("Rust Bloom", {"baseColor": (0.3, 0.12, 0.05), "metallic": 0.0, "roughness": 0.9},
                 [G("genRustSpread", level=0.5, breakup=1.0, seed=571)]),
            fill("Paint", {"baseColor": (0.2, 0.24, 0.13), "metallic": 0.0, "roughness": 0.65, "height": 0.52},
                 [MF(1.0),
                  G("genPaintChips", mode="subtract", level=0.72, softness=0.03, chipScale=16, seed=572),
                  G("genEdgeWear", mode="subtract", level=0.32, softness=0.1, breakup=0.8, seed=572)]),
            fill("Grime", {"baseColor": (0.06, 0.05, 0.04), "roughness": 0.9, "metallic": 0.0},
                 [G("genDirt", level=0.4, seed=573)], ch={"baseColor": ("normal", 0.6), "roughness": ("normal", 0.6), "metallic": ("normal", 0.6)})]


# Write ---------------------------------------------------------------------------------------------------------------

def build():
    written = []
    for file, name, tags, desc, fn in MATS:
        ID.n = 0
        children = fn()
        folder = {"id": ID("L"), "name": name, "kind": "folder", "children": children, "masks": []}
        doc = {"format": "lilasmart", "version": 1, "name": name, "description": desc, "tags": tags, "kind": "material", "layer": folder}
        path = os.path.join(OUT, file + ".lilasmart")
        with open(path, "w", newline="\n") as f:
            json.dump(doc, f, indent=2)
            f.write("\n")
        written.append(path)
    print(len(written), "written")


if __name__ == "__main__":
    build()
