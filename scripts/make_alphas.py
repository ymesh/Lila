"""Writes Lila's built-in brush alphas into resources/alphas/<category>/ as grayscale PNGs.

White is where the brush acts and black where it doesn't, so every feature is white on black: grooves, pores and holes
are cut in with Ctrl.

Everything is drawn here from shapes and seeded noise, so the set carries no third-party licence. Run from the repo root:
    python scripts/make_alphas.py [category ...]
"""
from pathlib import Path

import numpy as np
from PIL import Image

OUT = Path(__file__).resolve().parent.parent / "resources" / "alphas"
rng = np.random.default_rng(7)
ONLY = set()


def grid(n):
    y, x = np.mgrid[0:n, 0:n]
    u = (x + 0.5) / n * 2 - 1
    v = (y + 0.5) / n * 2 - 1
    return u, v


def smooth(t):
    t = np.clip(t, 0, 1)
    return t * t * (3 - 2 * t)


def edge(d, soft=0.08):
    """1 inside (d < 0), 0 outside, a smooth rim at d = 0."""
    return smooth(0.5 - d / soft)


def disc(u, v, radius=0.92, soft=0.12):
    return edge(np.sqrt(u * u + v * v) - radius, soft)


def value_noise(u, v, cells, octaves=4, seed=0):
    r = np.random.default_rng(seed)
    total = np.zeros_like(u)
    amp, norm = 1.0, 0.0
    for o in range(octaves):
        c = cells * 2 ** o
        g = r.random((c + 2, c + 2))
        gx, gy = (u + 1) / 2 * c, (v + 1) / 2 * c
        x0, y0 = np.clip(gx.astype(int), 0, c), np.clip(gy.astype(int), 0, c)
        fx, fy = smooth(gx - x0), smooth(gy - y0)
        a = g[y0, x0] * (1 - fx) + g[y0, x0 + 1] * fx
        b = g[y0 + 1, x0] * (1 - fx) + g[y0 + 1, x0 + 1] * fx
        total += amp * (a * (1 - fy) + b * fy)
        norm += amp
        amp *= 0.5
    return total / norm


def worley(u, v, cells, seed=0, jitter=1.0):
    """F1 and F2 distances to scattered cell points, in cell units."""
    r = np.random.default_rng(seed)
    pts = r.random((cells + 2, cells + 2, 2)) * jitter + (1 - jitter) * 0.5
    gx, gy = (u + 1) / 2 * cells, (v + 1) / 2 * cells
    ix, iy = np.floor(gx).astype(int), np.floor(gy).astype(int)
    f1 = np.full(u.shape, 9.0)
    f2 = np.full(u.shape, 9.0)
    for dy in (-1, 0, 1):
        for dx in (-1, 0, 1):
            cx, cy = ix + dx, iy + dy
            p = pts[np.clip(cy + 1, 0, cells + 1), np.clip(cx + 1, 0, cells + 1)]
            d = np.sqrt((cx + p[..., 0] - gx) ** 2 + (cy + p[..., 1] - gy) ** 2)
            f2 = np.where(d < f1, f1, np.minimum(f2, d))
            f1 = np.minimum(f1, d)
    return f1, f2


def scatter(u, v, count, rmin, rmax, seed, profile=lambda t: t, soft=0.3, box=0.85):
    """Round bumps at random places; profile shapes each bump from rim (0) to centre (1)."""
    r = np.random.default_rng(seed)
    out = np.zeros_like(u)
    for cx, cy, rad in r.uniform([-box, -box, rmin], [box, box, rmax], (count, 3)):
        d = np.sqrt((u - cx) ** 2 + (v - cy) ** 2) / rad
        out = np.maximum(out, profile(smooth((1 - d) / soft)))
    return out


_names = set()


def save(category, name, img):
    if ONLY and category not in ONLY:
        return
    assert name not in _names, f"duplicate alpha name: {name}"
    _names.add(name)
    d = OUT / category
    d.mkdir(parents=True, exist_ok=True)
    Image.fromarray((np.clip(img, 0, 1) * 255).round().astype(np.uint8), "L").save(d / f"{name}.png", optimize=True)


def shapes():
    u, v = grid(256)
    r = np.sqrt(u * u + v * v)
    a = np.arctan2(v, u)
    save("shapes", "round hard", edge(r - 0.9, 0.03))
    save("shapes", "round soft", np.exp(-(r / 0.45) ** 2))
    save("shapes", "dome", np.sqrt(np.clip(1 - (r / 0.92) ** 2, 0, 1)))
    save("shapes", "square", edge(np.maximum(abs(u), abs(v)) - 0.9, 0.06))
    save("shapes", "square soft", smooth(1 - np.maximum(abs(u), abs(v)) / 0.95) ** 0.7)
    save("shapes", "diamond", edge(abs(u) + abs(v) - 0.9, 0.06))
    save("shapes", "triangle", edge(np.maximum(v * 0.5 - 0.45 + abs(u) * 0.866, -v - 0.45), 0.05))
    save("shapes", "cross", edge(np.minimum(np.maximum(abs(u) - 0.22, abs(v) - 0.9), np.maximum(abs(v) - 0.22, abs(u) - 0.9)), 0.05))
    save("shapes", "star", edge(r - (0.55 + 0.35 * np.cos(a * 5)), 0.08))
    save("shapes", "ring", edge(abs(r - 0.65) - 0.2, 0.08))
    save("shapes", "rings", (0.5 + 0.5 * np.cos(r * np.pi * 8)) * disc(u, v))
    save("shapes", "line", edge(abs(v) - 0.12, 0.08) * edge(abs(u) - 0.9, 0.1))
    save("shapes", "ledge", smooth((v + 0.1) / 0.3) * disc(u, v))


def skin():
    u, v = grid(512)
    f1, _ = worley(u, v, 40, seed=11)
    save("skin", "pores fine", smooth(1 - f1 / 0.35) * disc(u, v))
    f1, _ = worley(u, v, 18, seed=12)
    save("skin", "pores coarse", smooth(1 - f1 / 0.4) * disc(u, v))
    warp = value_noise(u, v, 4, 3, seed=13) - 0.5
    lines = np.abs(np.sin((v + warp * 0.25) * np.pi * 14))
    save("skin", "wrinkles", (1 - lines) ** 6 * disc(u, v))
    a = np.arctan2(v, u)
    rays = np.abs(np.sin(a * 6 + value_noise(u, v, 3, 2, seed=14) * 2))
    r = np.sqrt(u * u + v * v)
    save("skin", "crows feet", (1 - rays) ** 12 * smooth((r - 0.15) / 0.2) * disc(u, v))
    f1, f2 = worley(u, v, 7, seed=15)
    save("skin", "cells", smooth((f2 - f1) / 0.25) * disc(u, v))
    vein = np.zeros_like(u)
    for seed in range(5):
        w = value_noise(u, v, 3, 3, seed=40 + seed) - 0.5
        vein = np.maximum(vein, smooth(1 - np.abs(w) / 0.025))
    save("skin", "veins", vein * disc(u, v))


def organic():
    u, v = grid(512)
    f1, f2 = worley(u, v, 9, seed=21)
    save("organic", "cracks", smooth(1 - (f2 - f1) / 0.06) * disc(u, v))
    f1, f2 = worley(u, v, 6, seed=22)
    n = value_noise(u, v, 8, 3, seed=23)
    save("organic", "dry mud", np.clip(smooth((f2 - f1) / 0.18) * (0.75 + 0.25 * n), 0, 1) * disc(u, v))
    warp = value_noise(u, v, 3, 4, seed=24) - 0.5
    grain = 0.5 + 0.5 * np.sin((u + warp * 0.5) * np.pi * 18)
    save("organic", "wood grain", (0.3 + 0.7 * grain ** 3) * disc(u, v))
    streak = value_noise(u * 6, v * 0.4, 6, 4, seed=25)
    save("organic", "bark", smooth((streak - 0.35) / 0.4) * disc(u, v))
    f1, f2 = worley(u, v, 14, seed=26)
    save("organic", "leather", smooth(f1 / 0.6) ** 0.5 * (1 - smooth(1 - (f2 - f1) / 0.12)) * disc(u, v))
    # Reptile scales: hex-ish cells, each a rounded plate.
    f1, f2 = worley(u, v, 10, seed=27, jitter=0.35)
    save("organic", "reptile scales", np.sqrt(np.clip((f2 - f1) / 0.5, 0, 1)) * disc(u, v))
    r = np.sqrt(u * u + v * v)
    a = np.arctan2(v, u)
    save("organic", "fish scales", np.clip(1 - (np.mod(r * 6 + 0.5 * np.sin(a * 9), 1.0) * 1.4), 0, 1) * disc(u, v))
    save("organic", "sponge", smooth((worley(u, v, 12, seed=28)[0] - 0.25) / 0.3) * disc(u, v))


def stone():
    u, v = grid(512)
    save("stone", "noise", np.clip((value_noise(u, v, 8, seed=31) - 0.2) * 1.6, 0, 1) * disc(u, v))
    save("stone", "fine noise", np.clip((value_noise(u, v, 32, 3, seed=32) - 0.15) * 1.4, 0, 1) * disc(u, v))
    save("stone", "rock", np.clip((value_noise(u, v, 3, 6, seed=33) - 0.25) * 2.0, 0, 1) * disc(u, v))
    ridged = 1 - np.abs(value_noise(u, v, 4, 5, seed=34) * 2 - 1)
    save("stone", "ridged", ridged ** 2 * disc(u, v))
    save("stone", "gravel", scatter(u, v, 260, 0.03, 0.08, seed=35, profile=np.sqrt) * disc(u, v))
    save("stone", "pebbles", scatter(u, v, 45, 0.08, 0.2, seed=36, profile=np.sqrt) * disc(u, v))
    f1, f2 = worley(u, v, 5, seed=37)
    save("stone", "cobbles", np.sqrt(smooth((f2 - f1) / 0.35)) * disc(u, v))
    save("stone", "erosion", smooth((value_noise(u * 0.3, v * 3, 10, 4, seed=38) - 0.3) / 0.5) * disc(u, v))


def fabric():
    u, v = grid(512)
    k = 22
    over = np.sin(u * np.pi * k) * np.sin(v * np.pi * k)
    warp = 0.5 + 0.5 * np.cos(v * np.pi * k * 2)
    weft = 0.5 + 0.5 * np.cos(u * np.pi * k * 2)
    save("fabric", "plain weave", np.where(over > 0, warp, weft) * disc(u, v))
    diag = np.mod((u + v) * k * 0.5, 1.0)
    save("fabric", "twill", (0.25 + 0.75 * smooth(1 - np.abs(diag - 0.5) / 0.4)) * (0.8 + 0.2 * weft) * disc(u, v))
    cx = np.mod(u * k * 0.5, 1.0) - 0.5
    cy = np.mod(v * k * 0.5 + np.abs(cx), 1.0)
    save("fabric", "knit", smooth(1 - np.abs(cy - 0.5) / 0.45) * smooth(1 - np.abs(cx) / 0.55) * disc(u, v))
    coarse = np.where(np.sin(u * np.pi * 9) * np.sin(v * np.pi * 9) > 0, 0.5 + 0.5 * np.cos(v * np.pi * 18), 0.5 + 0.5 * np.cos(u * np.pi * 18))
    save("fabric", "burlap", np.clip(coarse * (0.7 + 0.5 * value_noise(u, v, 20, 2, seed=41)), 0, 1) * disc(u, v))
    dash = smooth(1 - np.abs(np.mod(u * 5, 1.0) - 0.5) / 0.32) * edge(np.abs(v) - 0.12, 0.1)
    save("fabric", "stitches", dash * edge(np.abs(u) - 0.92, 0.08))
    quilt = np.minimum(np.abs(np.mod((u + v) * 3, 1.0) - 0.5), np.abs(np.mod((u - v) * 3, 1.0) - 0.5))
    save("fabric", "quilting", smooth(quilt / 0.5) ** 0.6 * disc(u, v))
    folds = np.abs(np.sin((u + 0.3 * (value_noise(u, v, 2, 2, seed=42) - 0.5)) * np.pi * 4))
    save("fabric", "folds", folds ** 0.7 * disc(u, v))


def hard_surface():
    u, v = grid(256)
    r = np.sqrt(u * u + v * v)
    head = np.sqrt(np.clip(1 - (r / 0.75) ** 2, 0, 1))
    save("hard surface", "rivet", head)
    slot = edge(np.maximum(np.abs(v) - 0.07, np.abs(u) - 0.55), 0.04)
    save("hard surface", "screw", head * (1 - 0.8 * slot))
    save("hard surface", "phillips screw", head * (1 - 0.8 * np.maximum(slot, edge(np.maximum(np.abs(u) - 0.07, np.abs(v) - 0.55), 0.04))))
    hexd = np.maximum(np.abs(u) * 0.866 + np.abs(v) * 0.5, np.abs(v)) - 0.72
    save("hard surface", "hex bolt", edge(hexd, 0.04) * (0.75 + 0.25 * edge(r - 0.35, 0.05)))
    save("hard surface", "rivet row", np.max([np.sqrt(np.clip(1 - (((u - cx) ** 2 + v * v) / 0.12 ** 2), 0, 1)) for cx in np.linspace(-0.75, 0.75, 5)], axis=0))
    save("hard surface", "button", np.minimum(1.0, smooth((0.7 - r) / 0.12)) * (0.85 + 0.15 * edge(r - 0.45, 0.05)))
    save("hard surface", "panel", smooth((0.85 - np.maximum(np.abs(u), np.abs(v))) / 0.15))
    save("hard surface", "panel line", edge(np.abs(v) - 0.03, 0.03) * edge(np.abs(u) - 0.92, 0.06))
    vents = edge(np.abs(np.mod(v * 3.5, 1.0) - 0.5) - 0.22, 0.08) * edge(np.abs(u) - 0.75, 0.08) * edge(np.abs(v) - 0.85, 0.05)
    save("hard surface", "vent", vents)
    hx = np.mod(u * 6, 1.0) - 0.5
    hy = np.mod(v * 6, 1.0) - 0.5
    save("hard surface", "grate", edge(np.maximum(np.abs(hx), np.abs(hy)) - 0.32, 0.06) * disc(u, v, 0.9, 0.05))
    save("hard surface", "perforated", edge(np.sqrt(hx * hx + hy * hy) - 0.28, 0.06) * disc(u, v, 0.9, 0.05))
    # Hexagonal honeycomb walls.
    q = u * 7
    s = v * 7 / 0.866
    row = np.floor(s)
    qx = np.mod(q + 0.5 * np.mod(row, 2), 1.0) - 0.5
    sy = np.mod(s, 1.0) - 0.5
    save("hard surface", "honeycomb", (1 - edge(np.maximum(np.abs(qx), np.abs(sy) * 0.866 + np.abs(qx) * 0.5) - 0.42, 0.05)) * disc(u, v, 0.9, 0.05))
    a = np.mod(u * 5 + v * 5, 2.0)
    b = np.mod(u * 5 - v * 5, 2.0)
    tread = np.where((np.floor(u * 2.5 + 10) + np.floor(v * 2.5 + 10)) % 2 == 0, edge(np.abs(a - 1) - 0.18, 0.12) * edge(np.abs(np.mod(b, 2.0) - 1) - 0.7, 0.1),
                     edge(np.abs(b - 1) - 0.18, 0.12) * edge(np.abs(np.mod(a, 2.0) - 1) - 0.7, 0.1))
    save("hard surface", "tread plate", tread * disc(u, v, 0.9, 0.05))
    save("hard surface", "knurling", np.maximum(0.5 + 0.5 * np.cos((u + v) * np.pi * 7), 0.5 + 0.5 * np.cos((u - v) * np.pi * 7)) ** 3 * disc(u, v))
    save("hard surface", "stripes", (0.5 + 0.5 * np.cos(u * np.pi * 8)) * disc(u, v))


def damage():
    u, v = grid(512)
    lines = np.zeros_like(u)
    r = np.random.default_rng(51)
    for _ in range(26):
        x0, y0 = r.uniform(-0.8, 0.8, 2)
        ang = r.uniform(-0.5, 0.5) + 0.6
        length = r.uniform(0.2, 0.9)
        dx, dy = np.cos(ang), np.sin(ang)
        t = np.clip((u - x0) * dx + (v - y0) * dy, 0, length)
        d = np.sqrt((u - x0 - t * dx) ** 2 + (v - y0 - t * dy) ** 2)
        lines = np.maximum(lines, smooth(1 - d / r.uniform(0.004, 0.012)) * smooth(np.minimum(t, length - t) / 0.08))
    save("damage", "scratches", lines * disc(u, v))
    save("damage", "dents", scatter(u, v, 70, 0.06, 0.16, seed=52, profile=lambda t: t * t) * disc(u, v))
    save("damage", "hammered", scatter(u, v, 220, 0.04, 0.09, seed=53, profile=lambda t: t) * disc(u, v))
    chips = np.clip(value_noise(u, v, 6, 4, seed=54), 0, 1)
    save("damage", "chips", smooth((0.58 - chips) / 0.06) * disc(u, v))
    f1, f2 = worley(u, v, 4, seed=55)
    hole = edge(np.sqrt(u * u + v * v) - 0.18, 0.05)
    rays = 1 - smooth(1 - (f2 - f1) / 0.05)
    save("damage", "impact", np.clip(hole + 0.6 * (1 - rays) * smooth(1 - np.sqrt(u * u + v * v) / 0.9), 0, 1) * disc(u, v))
    f1, f2 = worley(u * 0.6, v * 0.6, 12, seed=56)
    save("damage", "fine cracks", smooth(1 - (f2 - f1) / 0.05) * smooth((value_noise(u, v, 4, 3, seed=57) - 0.35) / 0.15) * disc(u, v))


# Helpers for the extended library. Noise here is periodic over the image so the noise category tiles.

def to_px(c, n):
    return (c + 1) / 2 * n - 0.5


def crop(n, x0, x1, y0, y1):
    """Row and column slices of an n x n image covering the uv box, or None when it falls outside."""
    i0, i1 = max(int(np.floor(to_px(y0, n))), 0), min(int(np.ceil(to_px(y1, n))) + 1, n)
    j0, j1 = max(int(np.floor(to_px(x0, n))), 0), min(int(np.ceil(to_px(x1, n))) + 1, n)
    if i0 >= i1 or j0 >= j1:
        return None
    return slice(i0, i1), slice(j0, j1)


def hash2(a, b, seed=0):
    h = np.sin(a * 127.1 + b * 311.7 + seed * 74.7) * 43758.5453
    return h - np.floor(h)


def fade(t):
    return t * t * t * (t * (t * 6 - 15) + 10)


def _cells(cells):
    return (cells, cells) if np.isscalar(cells) else cells


def tnoise(u, v, cells, octaves=4, seed=0, gain=0.5):
    """Periodic value noise in [0, 1]; cells may be (across, down)."""
    r = np.random.default_rng(seed)
    cx, cy = _cells(cells)
    total, amp, norm = np.zeros_like(u), 1.0, 0.0
    for o in range(octaves):
        nx, ny = int(cx * 2 ** o), int(cy * 2 ** o)
        g = r.random((ny, nx))
        x, y = (u + 1) / 2 * nx, (v + 1) / 2 * ny
        x0, y0 = np.floor(x), np.floor(y)
        fx, fy = smooth(x - x0), smooth(y - y0)
        i0, j0 = x0.astype(int) % nx, y0.astype(int) % ny
        i1, j1 = (i0 + 1) % nx, (j0 + 1) % ny
        a = g[j0, i0] * (1 - fx) + g[j0, i1] * fx
        b = g[j1, i0] * (1 - fx) + g[j1, i1] * fx
        total += amp * (a * (1 - fy) + b * fy)
        norm += amp
        amp *= gain
    return total / norm


def tperlin(u, v, cells, octaves=4, seed=0, gain=0.5):
    """Periodic gradient noise, centred on 0.5 and roughly within [0, 1]."""
    r = np.random.default_rng(seed)
    cx, cy = _cells(cells)
    total, amp, norm = np.zeros_like(u), 1.0, 0.0
    for o in range(octaves):
        nx, ny = int(cx * 2 ** o), int(cy * 2 ** o)
        ang = r.random((ny, nx)) * 2 * np.pi
        gx, gy = np.cos(ang), np.sin(ang)
        x, y = (u + 1) / 2 * nx, (v + 1) / 2 * ny
        x0, y0 = np.floor(x), np.floor(y)
        fx, fy = x - x0, y - y0
        i0, j0 = x0.astype(int) % nx, y0.astype(int) % ny
        i1, j1 = (i0 + 1) % nx, (j0 + 1) % ny
        n00 = gx[j0, i0] * fx + gy[j0, i0] * fy
        n10 = gx[j0, i1] * (fx - 1) + gy[j0, i1] * fy
        n01 = gx[j1, i0] * fx + gy[j1, i0] * (fy - 1)
        n11 = gx[j1, i1] * (fx - 1) + gy[j1, i1] * (fy - 1)
        sx, sy = fade(fx), fade(fy)
        total += amp * ((n00 * (1 - sx) + n10 * sx) * (1 - sy) + (n01 * (1 - sx) + n11 * sx) * sy)
        norm += amp
        amp *= gain
    return 0.5 + total / norm * 0.8


def ridged(u, v, cells, octaves=5, seed=0, gain=0.5):
    return 1 - np.abs(tperlin(u, v, cells, octaves, seed, gain) * 2 - 1)


def tworley(u, v, cells, seed=0, jitter=1.0):
    """Periodic F1, F2 (cell units), nearest cell id and the offset (dx, dy) from the pixel to the nearest point.

    The F2 returned is F1 plus twice the distance to the border between the two nearest cells, so F2 - F1 measures
    the distance to the cell edge and stays thin where two points nearly coincide.
    """
    r = np.random.default_rng(seed)
    pts = r.random((cells, cells, 2)) * jitter + (1 - jitter) * 0.5
    gx, gy = (u + 1) / 2 * cells, (v + 1) / 2 * cells
    ix, iy = np.floor(gx).astype(int), np.floor(gy).astype(int)
    f1, f2 = np.full(u.shape, 9.0), np.full(u.shape, 9.0)
    ident = np.zeros(u.shape, int)
    ox, oy = np.zeros_like(u), np.zeros_like(u)
    sx, sy = np.zeros_like(u), np.zeros_like(u)
    for dy in (-2, -1, 0, 1, 2):
        for dx in (-2, -1, 0, 1, 2):
            cx, cy = ix + dx, iy + dy
            wx, wy = cx % cells, cy % cells
            p = pts[wy, wx]
            px, py = cx + p[..., 0] - gx, cy + p[..., 1] - gy
            d = np.sqrt(px * px + py * py)
            closer = d < f1
            second = ~closer & (d < f2)
            sx, sy = np.where(closer, ox, np.where(second, px, sx)), np.where(closer, oy, np.where(second, py, sy))
            f2 = np.where(closer, f1, np.minimum(f2, d))
            ident = np.where(closer, wy * cells + wx, ident)
            ox, oy = np.where(closer, px, ox), np.where(closer, py, oy)
            f1 = np.where(closer, d, f1)
    border = (f2 * f2 - f1 * f1) / (2 * np.maximum(np.sqrt((sx - ox) ** 2 + (sy - oy) ** 2), 1e-6))
    return f1, f1 + 2 * border, ident, ox, oy


def cellvals(ident, cells, seed):
    return np.random.default_rng(seed).random(cells * cells)[ident]


def stretch(x, lo=1.0, hi=99.0):
    a, b = np.percentile(x, [lo, hi])
    return np.clip((x - a) / max(b - a, 1e-6), 0, 1)


def blur(img, radius):
    """Wrapping box blur, three passes."""
    out = img.copy()
    for _ in range(3):
        for axis in (0, 1):
            acc = np.zeros_like(out)
            for k in range(-radius, radius + 1):
                acc += np.roll(out, k, axis)
            out = acc / (2 * radius + 1)
    return out


def dome_p(s):
    return np.sqrt(np.clip(1 - s * s, 0, 1))


def vee_p(s):
    return np.clip(1 - s, 0, 1)


def flat_p(s):
    return smooth((1 - s) / 0.35)


def round_p(s):
    return smooth(1 - s)


def strokes(u, v, lines, profile=dome_p, img=None):
    """Draws polylines given as (points, widths[, height]) with a cross profile of s = distance / width."""
    n = u.shape[0]
    img = np.zeros_like(u) if img is None else img
    for line in lines:
        pts = np.asarray(line[0], float)
        wid = np.broadcast_to(np.asarray(line[1], float), (len(pts),))
        h = line[2] if len(line) > 2 else 1.0
        for (ax, ay), (bx, by), wa, wb in zip(pts[:-1], pts[1:], wid[:-1], wid[1:]):
            m = max(wa, wb)
            c = crop(n, min(ax, bx) - m, max(ax, bx) + m, min(ay, by) - m, max(ay, by) + m)
            if c is None:
                continue
            uu, vv = u[c], v[c]
            dx, dy = bx - ax, by - ay
            t = np.clip(((uu - ax) * dx + (vv - ay) * dy) / (dx * dx + dy * dy + 1e-12), 0, 1)
            d = np.sqrt((uu - ax - t * dx) ** 2 + (vv - ay - t * dy) ** 2)
            img[c] = np.maximum(img[c], h * profile(d / np.maximum(wa + (wb - wa) * t, 1e-6)))
    return img


def stamp(u, v, img, cx, cy, size, angle, fn, mode="max", reach=1.6):
    """Evaluates fn(lu, lv) in a frame centred at (cx, cy), rotated by angle and scaled by size; 'over' takes (value, cover)."""
    e = size * reach
    c = crop(u.shape[0], cx - e, cx + e, cy - e, cy + e)
    if c is None:
        return img
    du, dv = u[c] - cx, v[c] - cy
    ca, sa = np.cos(angle), np.sin(angle)
    lu, lv = (du * ca + dv * sa) / size, (-du * sa + dv * ca) / size
    val = fn(lu, lv)
    if mode == "max":
        img[c] = np.maximum(img[c], val)
    elif mode == "add":
        img[c] += val
    elif mode == "over":
        val, cover = val
        img[c] = img[c] * (1 - cover) + val * cover
    return img


def bumps(u, v, xs, ys, rs, profile=np.sqrt, soft=0.3, heights=None, img=None):
    img = np.zeros_like(u) if img is None else img
    for i, (x, y, rad) in enumerate(zip(xs, ys, rs)):
        c = crop(u.shape[0], x - rad, x + rad, y - rad, y + rad)
        if c is None:
            continue
        d = np.sqrt((u[c] - x) ** 2 + (v[c] - y) ** 2) / rad
        img[c] = np.maximum(img[c], profile(smooth((1 - d) / soft)) * (1.0 if heights is None else heights[i]))
    return img


def walk(r, x, y, ang, length, steps=24, wiggle=0.15, curl=0.0, pull=0.2):
    """A wandering polyline; curl bends it steadily, pull keeps it near its heading."""
    a0, pts, step = ang, [(x, y)], length / steps
    for i in range(steps):
        ang += r.normal(0, wiggle)
        ang += (a0 + curl * (i + 1) - ang) * pull
        x, y = x + np.cos(ang) * step, y + np.sin(ang) * step
        pts.append((x, y))
    return np.array(pts)


def taper(n, w, ends="both", p=0.6):
    t = np.linspace(0, 1, n)
    if ends == "both":
        return w * np.sin(np.pi * t) ** p + w * 0.06
    return w * (1 - t) ** p + w * 0.06


def crack_lines(r, x, y, ang, length, width, steps=28, wiggle=0.3, branch=0.1, depth=0, out=None):
    out = [] if out is None else out
    a0, pts, ws, step = ang, [(x, y)], [width], length / steps
    for i in range(steps):
        ang += r.normal(0, wiggle)
        ang += (a0 - ang) * 0.25
        x, y = x + np.cos(ang) * step, y + np.sin(ang) * step
        pts.append((x, y))
        ws.append(width * (1 - (i + 1) / steps) ** 0.7 + 0.0025)
        if depth < 2 and r.random() < branch:
            crack_lines(r, x, y, ang + r.choice([-1, 1]) * r.uniform(0.4, 1.1), length * (1 - i / steps) * r.uniform(0.3, 0.6),
                        ws[-1] * 0.75, max(steps // 2, 4), wiggle, branch * 0.6, depth + 1, out)
    out.append((pts, ws))
    return out


def path_coords(u, v, pts):
    """Arc length along the polyline and signed distance across it, from its nearest segment."""
    pts = np.asarray(pts, float)
    seg = np.diff(pts, axis=0)
    seglen = np.sqrt((seg ** 2).sum(1))
    cum = np.concatenate([[0], np.cumsum(seglen)])
    best, s, d = np.full(u.shape, np.inf), np.zeros_like(u), np.zeros_like(u)
    for i, ((ax, ay), (dx, dy)) in enumerate(zip(pts[:-1], seg)):
        t = np.clip(((u - ax) * dx + (v - ay) * dy) / (dx * dx + dy * dy + 1e-12), 0, 1)
        px, py = u - ax - t * dx, v - ay - t * dy
        dist = np.sqrt(px * px + py * py)
        closer = dist < best
        best = np.where(closer, dist, best)
        s = np.where(closer, cum[i] + t * seglen[i], s)
        d = np.where(closer, np.sign(dx * (v - ay) - dy * (u - ax)) * dist, d)
    return s, d, cum[-1]


def curl_path(k, length, n=300, power=1.5, mode="s"):
    """A curve whose curvature grows towards its ends: 's' curls the ends opposite ways, 'c' the same way, 'spiral' one end."""
    s = np.linspace(-length, length, n) if mode != "spiral" else np.linspace(0, length, n)
    kap = k * np.abs(s) ** power * (np.sign(s) if mode in ("s", "spiral") else 1.0)
    ds = s[1] - s[0]
    th = np.cumsum(kap) * ds
    th -= th[n // 2] if mode != "spiral" else th[0]
    x, y = np.cumsum(np.cos(th)) * ds, np.cumsum(np.sin(th)) * ds
    pts = np.stack([x, y], 1)
    return pts - (pts.max(0) + pts.min(0)) / 2


def ngon(u, v, k, radius, rot=0.0):
    """Signed distance-ish to a regular k-gon of circumradius radius."""
    a = np.arctan2(v, u) - rot
    seg = 2 * np.pi / k
    a = np.mod(a, seg) - seg / 2
    return np.sqrt(u * u + v * v) * np.cos(a) - radius * np.cos(np.pi / k)


def convex_sdf(lu, lv, pts):
    d = np.full(lu.shape, -np.inf)
    for (ax, ay), (bx, by) in zip(pts, np.roll(pts, -1, 0)):
        ex, ey = bx - ax, by - ay
        ln = np.hypot(ex, ey) + 1e-12
        d = np.maximum(d, ((lu - ax) * ey - (lv - ay) * ex) / ln)
    return d


def chip_shape(r, k=7, rough=0.4):
    ang = (np.arange(k) + r.uniform(-0.3, 0.3, k)) * 2 * np.pi / k + r.uniform(0, 2 * np.pi)
    rad = r.uniform(1 - rough, 1, k)
    return np.stack([np.cos(ang) * rad, np.sin(ang) * rad], 1)


def chip_fn(pts, bevel=0.3, height=1.0, power=1.0):
    return lambda lu, lv: height * np.clip(-convex_sdf(lu, lv, pts) / bevel, 0, 1) ** power


def rbox(u, v, hx, hy, rad):
    qx, qy = np.abs(u) - hx + rad, np.abs(v) - hy + rad
    return np.sqrt(np.maximum(qx, 0) ** 2 + np.maximum(qy, 0) ** 2) + np.minimum(np.maximum(qx, qy), 0) - rad


def hexgrid(u, v, scale):
    """Offset from the nearest centre of a pointy-top hex grid (neighbours 1 apart, scale across the image) and that centre."""
    x, y = (u + 1) / 2 * scale, (v + 1) / 2 * scale
    sx, sy = 1.0, np.sqrt(3)
    ax, ay = np.mod(x, sx) - sx / 2, np.mod(y, sy) - sy / 2
    bx, by = np.mod(x - sx / 2, sx) - sx / 2, np.mod(y - sy / 2, sy) - sy / 2
    useA = ax * ax + ay * ay < bx * bx + by * by
    lx, ly = np.where(useA, ax, bx), np.where(useA, ay, by)
    return lx, ly, np.round((x - lx) * 2), np.round((y - ly) * 2)


def hexd(lx, ly):
    """0 at a cell centre, 0.5 on its edge."""
    return np.maximum(np.abs(lx), np.abs(lx) * 0.5 + np.abs(ly) * 0.866)


def bricks(u, v, cols, rows, offset=0.5):
    """Local position in uv units inside a brick, its half extents, and its row and column."""
    bw, bh = 2.0 / cols, 2.0 / rows
    y = (v + 1) / bh
    row = np.floor(y)
    x = (u + 1) / bw + offset * np.mod(row, 2)
    col = np.floor(x)
    return (x - col - 0.5) * bw, (y - row - 0.5) * bh, bw / 2, bh / 2, row, col


def band_knot(u, v, pts, z, width, edge_soft=0.3):
    """A ribbon along pts where the higher z wins at crossings, with a gap along each edge so over and under read."""
    n, m = u.shape[0], len(pts)
    img, zb, owner = np.zeros_like(u), np.full(u.shape, -np.inf), np.full(u.shape, -10 ** 6)
    seglen = np.median(np.sqrt((np.diff(pts, axis=0) ** 2).sum(1)))
    near_span = int(3 * width / max(seglen, 1e-9)) + 2
    for i in range(m - 1):
        (ax, ay), (bx, by) = pts[i], pts[i + 1]
        c = crop(n, min(ax, bx) - width, max(ax, bx) + width, min(ay, by) - width, max(ay, by) + width)
        if c is None:
            continue
        uu, vv = u[c], v[c]
        dx, dy = bx - ax, by - ay
        t = np.clip(((uu - ax) * dx + (vv - ay) * dy) / (dx * dx + dy * dy + 1e-12), 0, 1)
        s = np.sqrt((uu - ax - t * dx) ** 2 + (vv - ay - t * dy) ** 2) / width
        zz = z[i] + (z[i + 1] - z[i]) * t
        sub_i, sub_z, sub_o = img[c], zb[c], owner[c]
        val = smooth((1 - s) / edge_soft) * (0.7 + 0.3 * dome_p(s))
        # Segments close along the curve are the same stretch of ribbon: they merge by value, only crossings compare z.
        gap = np.abs(sub_o - i)
        same = (s < 1) & (np.minimum(gap, m - gap) <= near_span)
        win = (same & (val > sub_i)) | ((s < 1) & ~same & (zz > sub_z))
        sub_i[win], sub_z[win], sub_o[win] = val[win], zz[win], i
    return img


def strip(u, v, half=0.32, soft=0.06):
    return edge(np.abs(u) - 0.92, 0.08) * edge(np.abs(v) - half, soft)


# ---------------------------------------------------------------------------------------------------------------------

def damage_more():
    u, v = grid(512)
    D = disc(u, v)
    R = np.sqrt(u * u + v * v)
    r = np.random.default_rng(101)
    lines = []
    for k in range(5):
        crack_lines(r, 0, 0, k * 2 * np.pi / 5 + r.uniform(-0.3, 0.3), 0.85, 0.022, out=lines)
    save("damage", "branching cracks", strokes(u, v, lines, vee_p) * D)

    r = np.random.default_rng(102)
    lines = []
    angs = np.sort(r.uniform(0, 2 * np.pi, 11))
    for a in angs:
        crack_lines(r, 0.04 * np.cos(a), 0.04 * np.sin(a), a, r.uniform(0.6, 0.85), 0.016, branch=0.05, out=lines)
    for rad in (0.22, 0.42, 0.62):
        for a0, a1 in zip(angs, np.roll(angs, -1) + np.where(np.arange(11) == 10, 2 * np.pi, 0)):
            if r.random() < 0.75:
                t = np.linspace(a0, a1, 8)
                rr = rad * (1 + r.normal(0, 0.04, 8))
                lines.append((np.stack([np.cos(t) * rr, np.sin(t) * rr], 1), 0.006))
    save("damage", "spider cracks", np.maximum(strokes(u, v, lines, vee_p), edge(R - 0.05, 0.03)) * D)

    r = np.random.default_rng(103)
    lines = []
    for _ in range(30):
        crack_lines(r, *r.uniform(-0.8, 0.8, 2), r.uniform(0, 2 * np.pi), r.uniform(0.4, 1.0), 0.006, steps=30, branch=0.05, out=lines)
    save("damage", "hairline cracks", strokes(u, v, lines, vee_p) * D)

    r = np.random.default_rng(104)
    a = r.uniform(0, np.pi)
    lines = crack_lines(r, 0, 0, a, 0.9, 0.055, branch=0.15)
    crack_lines(r, 0, 0, a + np.pi, 0.9, 0.055, branch=0.15, out=lines)
    save("damage", "deep crack", strokes(u, v, lines, vee_p) * D)

    w = tperlin(u, v, 3, 3, seed=105) - 0.5
    f1, f2, *_ = tworley(u + w * 0.15, v + w * 0.15, 8, seed=106)
    save("damage", "crack web", smooth(1 - (f2 - f1) / 0.04) * smooth((tperlin(u, v, 3, 3, seed=107) - 0.4) / 0.15) * D)

    r = np.random.default_rng(108)
    img = np.zeros_like(u)
    for x in np.linspace(-0.75, 0.75, 9) + r.uniform(-0.05, 0.05, 9):
        stamp(u, v, img, x, r.uniform(-0.05, 0.05), r.uniform(0.08, 0.17), r.uniform(0, 6.3), chip_fn(chip_shape(r), 0.35, r.uniform(0.6, 1.0), 0.7))
    save("damage", "edge chips", img * D)

    w = tperlin(u, v, 2, 3, seed=109) - 0.5
    n = tperlin(u + w * 0.4, v + w * 0.4, 5, 5, seed=110)
    save("damage", "chipped paint", smooth((n - 0.55) / 0.025) * (0.9 + 0.1 * tnoise(u, v, 40, 2, seed=111)) * D)

    r = np.random.default_rng(112)
    pts = chip_shape(r, 9, 0.3)
    save("damage", "single chip", stamp(u, v, np.zeros_like(u), 0, 0, 0.75, 0.3,
                                        lambda lu, lv: np.clip(-convex_sdf(lu, lv, pts) / 0.6, 0, 1) * (0.85 + 0.15 * np.cos(np.hypot(lu - 0.3, lv) * 25))) * D)

    save("damage", "large dents", scatter(u, v, 12, 0.2, 0.35, seed=113, profile=lambda t: t * t, box=0.6) * D)
    save("damage", "creased dent", np.clip(np.exp(-(u / 0.5) ** 2 - (v / 0.28) ** 2) * (0.8 + 0.2 * smooth(1 - np.abs(v + 0.1 * u) / 0.03)), 0, 1) * D)

    r = np.random.default_rng(114)
    lines = []
    for _ in range(7):
        p = walk(r, *r.uniform(-0.7, 0.3, 2), 0.4 + r.normal(0, 0.3), r.uniform(0.4, 0.9), 20, 0.03, r.uniform(-0.01, 0.01))
        lines.append((p, taper(len(p), r.uniform(0.02, 0.04)), r.uniform(0.6, 1)))
    save("damage", "gouges", strokes(u, v, lines, vee_p) * D)

    r = np.random.default_rng(115)
    p = walk(r, -0.75, 0.2, -0.25, 1.5, 60, 0.02, 0.003)
    wv = taper(len(p), 0.08, p=0.5) * (0.8 + 0.4 * r.random(len(p)))
    save("damage", "gouge", strokes(u, v, [(p, wv)], vee_p) * D)

    r = np.random.default_rng(116)
    lines = []
    for k in range(4):
        p = walk(r, -0.55 + k * 0.17, -0.6 + abs(k - 1.5) * 0.06, 1.15, 1.15, 30, 0.0, -0.012)
        lines.append((p, taper(len(p), 0.035, p=0.8)))
    save("damage", "claw marks", strokes(u, v, lines, vee_p) * D)

    def bullet(lu, lv, seed=0):
        rr = np.sqrt(lu * lu + lv * lv)
        aa = np.arctan2(lv, lu)
        hole = np.sqrt(smooth(1 - rr / 0.16))
        crown = (0.45 + 0.25 * np.cos(aa * 7 + 2 * np.sin(aa * 3 + seed))) * smooth(1 - np.abs(rr - 0.24) / 0.1)
        ray = (1 - np.abs(np.sin(aa * 5 + seed + 0.6 * np.sin(rr * 9)))) ** 18 * smooth((rr - 0.2) / 0.1) * smooth((0.9 - rr) / 0.4)
        return np.maximum(np.maximum(hole, crown), ray * 0.6)

    save("damage", "bullet hole", stamp(u, v, np.zeros_like(u), 0, 0, 0.85, 0, bullet) * D)
    r = np.random.default_rng(117)
    img = np.zeros_like(u)
    for i in range(7):
        stamp(u, v, img, *r.uniform(-0.55, 0.55, 2), r.uniform(0.25, 0.38), r.uniform(0, 6), lambda lu, lv, s=i: bullet(lu, lv, s))
    save("damage", "bullet holes", img * D)

    r = np.random.default_rng(118)
    xy = np.clip(r.normal(0, 0.28, (70, 2)), -0.8, 0.8)
    save("damage", "shotgun spread", bumps(u, v, xy[:, 0], xy[:, 1], r.uniform(0.015, 0.035, 70), np.sqrt) * D)

    def scratch_set(seed, count, ang, spread, lmin, lmax, curved=False):
        r = np.random.default_rng(seed)
        lines = []
        for _ in range(count):
            a = ang + r.normal(0, spread)
            x, y = r.uniform(-0.85, 0.85, 2)
            ln = r.uniform(lmin, lmax)
            p = walk(r, x - np.cos(a) * ln / 2, y - np.sin(a) * ln / 2, a, ln, 12, 0.0, r.uniform(-0.04, 0.04) if curved else 0.0)
            lines.append((p, taper(len(p), r.uniform(0.003, 0.009)), r.uniform(0.4, 1.0)))
        return strokes(u, v, lines, vee_p) * D

    save("damage", "dense scratches", scratch_set(119, 140, 0, 3, 0.1, 0.6))
    save("damage", "parallel scratches", scratch_set(120, 90, 0.2, 0.06, 0.2, 1.0))
    save("damage", "curved scratches", scratch_set(121, 50, 0.5, 1.5, 0.3, 0.8, curved=True))
    r = np.random.default_rng(122)
    lines = []
    for _ in range(25):
        p = walk(r, *r.uniform(-0.7, 0.7, 2), r.normal(0.2, 0.3), r.uniform(0.15, 0.35), 8, 0.05)
        lines.append((p, taper(len(p), r.uniform(0.03, 0.06), p=0.4), r.uniform(0.2, 0.5)))
    save("damage", "scuffs", np.maximum(strokes(u, v, lines, round_p), scratch_set(123, 30, 0.2, 0.2, 0.1, 0.3) * 0.6) * D)

    f1, _, ident, *_ = tworley(u, v, 30, seed=124)
    rad = 0.1 + 0.35 * cellvals(ident, 30, 125)
    keep = cellvals(ident, 30, 126) > 0.35
    pits = smooth(1 - f1 / rad) * keep
    save("damage", "pitting", pits * D)
    w = tperlin(u, v, 3, 3, seed=127) - 0.5
    blobs = smooth((tperlin(u + w * 0.3, v + w * 0.3, 4, 5, seed=128) - 0.5) / 0.12)
    save("damage", "corrosion", np.clip(blobs * 0.6 + pits * blobs * 0.5 + 0.15 * blobs * tnoise(u, v, 64, 2, seed=129), 0, 1) * D)

    r = np.random.default_rng(130)
    region = smooth((tperlin(u, v, 2, 3, seed=131) - 0.45) / 0.1) * smooth((0.75 - R) / 0.1)
    xy = r.uniform(-0.85, 0.85, (500, 2))
    px = ((xy + 1) / 2 * 511).astype(int)
    xy = xy[region[px[:, 1], px[:, 0]] > 0.5]
    save("damage", "blistered", bumps(u, v, xy[:, 0], xy[:, 1], r.uniform(0.02, 0.07, len(xy)), np.sqrt, 0.5, r.uniform(0.5, 1, len(xy))) * D)

    f1, f2, ident, *_ = tworley(u, v, 7, seed=132)
    save("damage", "shattered", (0.35 + 0.65 * cellvals(ident, 7, 133)) * smooth((f2 - f1) / 0.03) * D)

    g = v + 0.25 * (tperlin(u, v, 3, 5, seed=134) - 0.5) + 0.06 * (tnoise(u, v, 30, 2, seed=135) - 0.5)
    save("damage", "worn edge", smooth((-g) / 0.03) * (0.85 + 0.15 * tnoise(u, v, 20, 3, seed=136)) * D)

    r = np.random.default_rng(137)
    xy = r.uniform(-0.8, 0.8, (16, 2))
    save("damage", "dings", np.maximum(bumps(u, v, xy[:, 0], xy[:, 1], r.uniform(0.04, 0.08, 16), lambda t: t, 0.8), scratch_set(138, 20, 0, 3, 0.05, 0.15)) * D)


def stone_more():
    u, v = grid(512)
    D = disc(u, v)
    n = tperlin(u, v, 4, 6, seed=201, gain=0.55)
    save("stone", "rough face", np.clip((0.6 * n + 0.4 * ridged(u, v, 3, 5, seed=202) - 0.3) * 1.8, 0, 1) * D)
    n = tperlin(u, v, 12, 5, seed=203, gain=0.55)
    save("stone", "rough face fine", np.clip((n - 0.25) * 2.0, 0, 1) * D)

    f1, f2, ident, ox, oy = tworley(u, v, 6, seed=204)
    gx, gy = cellvals(ident, 6, 205) * 2 - 1, cellvals(ident, 6, 206) * 2 - 1
    plane = cellvals(ident, 6, 207) + 0.6 * (gx * ox + gy * oy)
    save("stone", "faceted rock", np.clip((0.35 + 0.45 * plane) * smooth((f2 - f1) / 0.05) + 0.1 * tperlin(u, v, 16, 3, seed=208), 0, 1) * D)

    def strata(rot, k, seed, grooves=0.0):
        ca, sa = np.cos(rot), np.sin(rot)
        y = -u * sa + v * ca + 0.15 * (tperlin(u, v, 2, 3, seed=seed) - 0.5)
        f = (y + 2) * k
        layer, t = np.floor(f), f - np.floor(f)
        h = 0.5 + 0.5 * hash2(layer, 0, seed)
        out = h * smooth(t / 0.12) * smooth((1 - t) / 0.12) + 0.12 * tperlin(u, v, 20, 3, seed=seed + 1)
        if grooves:
            out *= 1 - grooves * smooth((tperlin(u, v, (12, 2), 3, seed=seed + 2) - 0.55) / 0.2)
        return np.clip(out, 0, 1) * D

    save("stone", "strata", strata(0.0, 4.5, 209))
    save("stone", "tilted strata", strata(0.4, 8, 210, 0.6))
    y = v + 0.1 * (tperlin(u, v, 2, 3, seed=211) - 0.5)
    f = (y + 2) * 12 + 0.6 * tperlin(u, v, 10, 3, seed=212)
    t = f - np.floor(f)
    save("stone", "layered shale", np.clip(0.3 + 0.7 * t ** 2 * hash2(np.floor(f), 1, 213) ** 0.5, 0, 1) * D)

    def pebble(asp, power):
        return lambda lu, lv: np.clip(1 - (lu * lu + (lv * asp) ** 2), 0, 1) ** power

    r = np.random.default_rng(214)
    img = np.zeros_like(u)
    for _ in range(240):
        stamp(u, v, img, *r.uniform(-0.85, 0.85, 2), r.uniform(0.03, 0.07), r.uniform(0, np.pi), pebble(r.uniform(1, 1.6), 0.6))
    save("stone", "dense pebbles", img * D)
    r = np.random.default_rng(215)
    img = np.zeros_like(u)
    for _ in range(30):
        h = r.uniform(0.7, 1)
        stamp(u, v, img, *r.uniform(-0.7, 0.7, 2), r.uniform(0.1, 0.2), r.uniform(0, np.pi), lambda lu, lv, a=r.uniform(1.2, 1.8), h=h: h * pebble(a, 0.35)(lu, lv))
    save("stone", "river stones", img * D)

    lx, ly, bw, bh, row, col = bricks(u, v, 7, 9, 0.5)
    sett = smooth(-rbox(lx, ly, bw - 0.02, bh - 0.02, 0.05) / 0.06) ** 0.6
    save("stone", "setts", np.clip(sett * (0.7 + 0.3 * hash2(row, col, 216)) + 0.12 * tperlin(u, v, 20, 3, seed=217) * sett, 0, 1) * D)

    f1, f2, ident, *_ = tworley(u, v, 7, seed=218, jitter=0.8)
    save("stone", "rounded cobbles", np.clip(np.sqrt(smooth((f2 - f1) / 0.5)) * (0.7 + 0.3 * cellvals(ident, 7, 219)) + 0.08 * tperlin(u, v, 24, 3, seed=220), 0, 1) * D)
    w = tperlin(u, v, 3, 3, seed=221) - 0.5
    f1, f2, ident, *_ = tworley(u + w * 0.2, v + w * 0.2, 4, seed=222, jitter=0.7)
    save("stone", "flagstones", np.clip(smooth((f2 - f1 - 0.03) / 0.04) * (0.75 + 0.25 * cellvals(ident, 4, 223)) + 0.08 * tperlin(u, v, 16, 4, seed=224), 0, 1) * D)

    lx, ly, bw, bh, row, col = bricks(u, v, 5, 12, 0.5)
    brick = smooth(-rbox(lx, ly, bw - 0.012, bh - 0.012, 0.01) / 0.03)
    save("stone", "brick wall", np.clip(brick * (0.75 + 0.25 * hash2(row, col, 225)) * (0.85 + 0.15 * tperlin(u, v, 24, 3, seed=226)), 0, 1) * D)
    w = tperlin(u, v, 3, 3, seed=227) - 0.5
    f1, f2, ident, *_ = tworley(u * 0.8 + w * 0.15, v * 1.8 + w * 0.15, 5, seed=228, jitter=0.9)
    save("stone", "stone wall", np.clip(np.sqrt(smooth((f2 - f1) / 0.3)) * (0.65 + 0.35 * cellvals(ident, 5, 229)) + 0.1 * tperlin(u, v, 20, 3, seed=230), 0, 1) * D)

    save("stone", "water erosion", smooth((tperlin(u, v, (6, 1), 5, seed=231) - 0.35) / 0.35) * (0.8 + 0.2 * smooth(-v)) * D)
    f1, _, ident, *_ = tworley(u, v, 18, seed=232)
    pits = smooth(1 - f1 / (0.15 + 0.3 * cellvals(ident, 18, 233)))
    save("stone", "weathered", np.clip(0.35 + 0.5 * tperlin(u, v, 4, 5, seed=234) - 0.45 * pits, 0, 1) * D)

    g = v + 0.25 * (tperlin(u, v, 2, 6, seed=235) - 0.5)
    save("stone", "rock edge", smooth(-g / 0.05) * (0.75 + 0.25 * tperlin(u, v, 12, 4, seed=236)) * D)
    g = v + 0.45 * (ridged(u, v, 2, 5, seed=237) - 0.6)
    save("stone", "jagged rock edge", smooth(-g / 0.012) * (0.7 + 0.3 * ridged(u, v, 8, 4, seed=238)) * D)
    save("stone", "cliff ridges", ridged(u, v, (6, 1), 5, seed=239) ** 1.5 * (0.8 + 0.2 * tperlin(u, v, 12, 3, seed=240)) * D)

    f1, _, ident, *_ = tworley(u, v, 22, seed=241)
    holes = smooth(1 - f1 / (0.15 + 0.3 * cellvals(ident, 22, 242))) * (cellvals(ident, 22, 243) > 0.25)
    save("stone", "lava rock", np.clip((0.4 + 0.6 * tperlin(u, v, 5, 5, seed=244)) * (1 - 0.9 * holes), 0, 1) * D)
    bands = 0.5 + 0.5 * np.sin((v + 0.1 * tperlin(u, v, 2, 3, seed=245)) * np.pi * 14)
    save("stone", "sandstone", np.clip(0.35 + 0.25 * bands + 0.35 * tnoise(u, v, 96, 2, seed=246), 0, 1) * D)
    f1, f2, ident, *_ = tworley(u, v, 50, seed=247)
    save("stone", "granite grain", np.clip((0.4 + 0.6 * cellvals(ident, 50, 248)) * smooth((f2 - f1) / 0.1) * 0.8 + 0.2 * tperlin(u, v, 6, 3, seed=249), 0, 1) * D)

    def crystal(lu, lv):
        t = lu
        wd = 0.16 * np.minimum(1, (1 - t) / 0.3)
        ins = (t > -0.05) & (t < 1)
        return np.where(ins, np.clip(1 - np.abs(lv) / np.maximum(wd, 1e-4), 0, 1) * (0.5 + 0.5 * np.clip(t, 0, 1)), 0)

    r = np.random.default_rng(250)
    img = np.zeros_like(u)
    for a in np.linspace(-1.1, 1.1, 11) + r.normal(0, 0.08, 11):
        sz = r.uniform(0.7, 1.15)
        stamp(u, v, img, 0, 0.6, sz, a - np.pi / 2, crystal, reach=1.1)
    save("stone", "crystals", img * D)

    R = np.sqrt(u * u + v * v) + 0.15 * (tperlin(u, v, 2, 4, seed=251) - 0.5)
    save("stone", "boulder", np.clip(np.sqrt(np.clip(1 - (R / 0.75) ** 2, 0, 1)) * (0.8 + 0.2 * plane) + 0.05 * tperlin(u, v, 16, 3, seed=252), 0, 1) * D)
    r = np.random.default_rng(253)
    img = np.zeros_like(u)
    for _ in range(40):
        stamp(u, v, img, *r.uniform(-0.75, 0.75, 2), r.uniform(0.06, 0.2), r.uniform(0, 6.3), chip_fn(chip_shape(r, 6), 0.5, r.uniform(0.5, 1)))
    save("stone", "rubble", img * D)
    r = np.random.default_rng(254)
    img = np.zeros_like(u)
    for _ in range(170):
        stamp(u, v, img, *r.uniform(-0.85, 0.85, 2), r.uniform(0.025, 0.06), r.uniform(0, 6.3), chip_fn(chip_shape(r, 6), 0.5, r.uniform(0.5, 1)))
    save("stone", "coarse gravel", img * D)


def leaf(lu, lv, width=0.4, power=0.8, veins=9, serr=0.0):
    """A leaf along lu from base (-1) to tip (1): blade, midrib and side veins."""
    t = np.clip((lu + 1) / 2, 0, 1)
    half = width * np.sin(np.pi * t) ** power * (1 - 0.25 * t)
    if serr:
        half = half * (1 - serr * np.abs(np.sin(t * np.pi * 16)))
    inside = smooth((half - np.abs(lv)) / 0.04) * (np.abs(lu) <= 1)
    blade = 0.3 + 0.5 * np.sqrt(np.clip(1 - (np.abs(lv) / np.maximum(half, 1e-4)) ** 2, 0, 1))
    mid = smooth(1 - np.abs(lv) / 0.03)
    vein = smooth(1 - np.abs(np.mod((lu - np.abs(lv) * 1.3) * veins * 0.5, 1) - 0.5) / 0.08) * 0.5
    stalk = smooth(1 - np.abs(lv) / 0.025) * (lu < -0.95) * (lu > -1.25)
    return np.maximum(inside * np.maximum(blade, np.maximum(mid, vein)), stalk * 0.8)


def organic_more():
    u, v = grid(512)
    D = disc(u, v)
    R = np.sqrt(u * u + v * v)
    A = np.arctan2(v, u)
    w = tperlin(u, v, (2, 1), 3, seed=301) - 0.5
    save("organic", "furrowed bark", ridged(u + w * 0.3, v, (8, 1), 4, seed=302) ** 1.5 * (0.7 + 0.3 * tperlin(u, v, 16, 3, seed=303)) * D)
    w = tperlin(u, v, 3, 3, seed=304) - 0.5
    f1, f2, ident, *_ = tworley(u + w * 0.15, v * 0.45 + w * 0.1, 7, seed=305)
    save("organic", "plated bark", np.clip(smooth((f2 - f1) / 0.12) ** 0.6 * (0.6 + 0.4 * cellvals(ident, 7, 306)) + 0.12 * tperlin(u, v, 24, 3, seed=307), 0, 1) * D)
    r = np.random.default_rng(308)
    img = 0.35 + 0.1 * smooth((tperlin(u, v, (1, 8), 3, seed=309) - 0.5) / 0.1)
    for _ in range(45):
        stamp(u, v, img, *r.uniform(-0.85, 0.85, 2), r.uniform(0.04, 0.13), r.normal(0, 0.05), lambda lu, lv: np.clip(1 - (lu * lu + (lv * 9) ** 2), 0, 1) ** 0.5)
    save("organic", "papery bark", np.clip(img, 0, 1) * D)
    save("organic", "stringy bark", smooth((tperlin(u + 0.1 * w, v, (24, 1), 4, seed=310) - 0.3) / 0.4) * D)

    def flake(lu, lv):
        ins = smooth((1 - np.maximum(np.abs(lu), np.abs(lv))) / 0.2) * (1 - 0.3 * lu ** 4)
        return 0.3 + 0.7 * smooth((lv + 1) / 2), ins

    r = np.random.default_rng(311)
    img = np.zeros_like(u)
    for j in range(10, -1, -1):
        for i in range(9):
            stamp(u, v, img, -1 + i * 0.25 + (j % 2) * 0.125 + r.normal(0, 0.02), -1 + j * 0.2 + r.normal(0, 0.02), r.uniform(0.11, 0.15), r.normal(0, 0.15), flake, "over")
    save("organic", "flaky bark", img * D)

    w = tperlin(u, v, 2, 3, seed=312) - 0.5
    dist = np.sqrt(u * u + (v + 3) ** 2) + w * 0.15
    save("organic", "fine wood grain", (0.3 + 0.7 * (0.5 + 0.5 * np.sin(dist * np.pi * 40)) ** 3) * D)
    r = np.random.default_rng(313)
    rings = 0.5 + 0.5 * np.cos((R + 0.04 * (tperlin(u, v, 3, 3, seed=314) - 0.5)) * np.pi * 26)
    lines = []
    for _ in range(5):
        crack_lines(r, 0, 0, r.uniform(0, 6.3), r.uniform(0.4, 0.75), 0.03, steps=16, wiggle=0.15, branch=0, out=lines)
    cracks = strokes(u, v, lines, vee_p)
    save("organic", "end grain", np.clip((0.35 + 0.65 * rings ** 2) * (1 - cracks), 0, 1) * D)

    lx, ly, bw, bh, row, col = bricks(u, v, 2, 6, 0.37)
    ph = hash2(row, col, 315) * 10
    grain = 0.5 + 0.5 * np.sin((ly * 40 + 0.6 * np.sin(lx * 3 + ph) + ph) * np.pi)
    plank = smooth(-rbox(lx, ly, bw - 0.012, bh - 0.012, 0.01) / 0.02)
    save("organic", "wood planks", plank * (0.6 + 0.4 * grain ** 2) * D)

    def knot_grain(cx, cy, rad):
        rr = np.sqrt((u - cx) ** 2 + (v - cy) ** 2)
        bend = 0.25 * np.exp(-(rr / (rad * 2.2)) ** 2) * np.sign(v - cy + 1e-6)
        g = 0.5 + 0.5 * np.sin((v + bend + 0.03 * (tperlin(u, v, 3, 3, seed=316) - 0.5)) * np.pi * 22)
        k = (0.5 + 0.5 * np.cos(rr * np.pi * 40)) * smooth((rad - rr) / 0.02)
        return np.where(rr < rad, 0.4 + 0.6 * k, 0.3 + 0.7 * g ** 3), rr

    img, rr = knot_grain(0, 0, 0.22)
    save("organic", "wood knot", img * D)
    save("organic", "knot hole", np.maximum(img * (1 - smooth((0.12 - rr) / 0.02)), smooth((0.11 - rr) / 0.03) ** 0.5) * D)

    save("organic", "oval leaf", stamp(u, v, np.zeros_like(u), 0.05, 0, 0.8, -0.6, lambda lu, lv: leaf(lu, lv, 0.5, 0.7)) * D)
    save("organic", "pointed leaf", stamp(u, v, np.zeros_like(u), 0.05, 0, 0.8, -0.8, lambda lu, lv: leaf(lu, lv, 0.25, 1.2, 12)) * D)
    save("organic", "serrated leaf", stamp(u, v, np.zeros_like(u), 0.05, 0, 0.8, -0.7, lambda lu, lv: leaf(lu, lv, 0.4, 0.8, 10, 0.12)) * D)
    lobe = 0.45 + 0.3 * np.abs(np.cos(2.5 * (A + np.pi / 2))) ** 0.6
    veinr = (1 - np.abs(np.sin(2.5 * (A + np.pi / 2)))) ** 20
    save("organic", "lobed leaf", smooth((lobe - R) / 0.03) * (0.5 + 0.5 * np.maximum(veinr, 0.6 * smooth(1 - R / 0.3))) * D)
    r = np.random.default_rng(317)
    img = np.zeros_like(u)
    for a in np.linspace(0, 2 * np.pi, 8)[:-1] + r.normal(0, 0.1, 7):
        stamp(u, v, img, np.cos(a) * 0.38, np.sin(a) * 0.38, 0.38, a, lambda lu, lv: leaf(lu, lv, 0.4, 0.8, 8))
    save("organic", "leaf cluster", img * D)
    lines = [(np.stack([0.12 * np.sin(np.linspace(0, 2, 30)) - 0.05, np.linspace(0.85, -0.85, 30)], 1), np.linspace(0.025, 0.006, 30))]
    img = strokes(u, v, lines)
    for i, t in enumerate(np.linspace(0.05, 0.92, 16)):
        y = 0.85 - t * 1.7
        x = 0.12 * np.sin(t * 2) - 0.05
        side = 1 if i % 2 else -1
        sz = 0.22 * (1 - t) + 0.05
        stamp(u, v, img, x + side * sz, y - sz * 0.25, sz, (0 if side > 0 else np.pi) - side * 0.35, lambda lu, lv: leaf(lu, lv, 0.3, 0.9, 6))
    save("organic", "fern frond", img * D)
    r = np.random.default_rng(318)
    lines = []
    for _ in range(40):
        x = r.uniform(-0.7, 0.7)
        ang = -np.pi / 2 + r.normal(0, 0.25)
        p = walk(r, x, 0.9, ang, r.uniform(0.6, 1.4), 20, 0.0, r.uniform(-0.03, 0.03), 0.0)
        lines.append((p, taper(len(p), 0.03, "end", 0.9)))
    save("organic", "grass blades", strokes(u, v, lines) * D)

    r = np.random.default_rng(319)
    main = walk(r, 0, 0.85, -np.pi / 2, 1.7, 30, 0.02)
    lines = [(main, np.linspace(0.03, 0.008, len(main)))]
    for i in range(3, 27, 3):
        for side in (-1, 1):
            crack_lines(r, *main[i], -np.pi / 2 + side * 0.9, 0.55 * (1 - i / 34), 0.014, 14, 0.15, 0.25, out=lines)
    save("organic", "leaf veins", strokes(u, v, lines) * D)
    w = tperlin(u, v, 3, 3, seed=320) - 0.5
    a = tworley(u + w * 0.1, v + w * 0.1, 5, seed=321)
    b = tworley(u + w * 0.1, v + w * 0.1, 14, seed=322)
    save("organic", "leaf skeleton", np.maximum(smooth(1 - (a[1] - a[0]) / 0.06), 0.6 * smooth(1 - (b[1] - b[0]) / 0.07)) * D)

    r = np.random.default_rng(323)
    lines = []
    for x in (-0.3, 0.0, 0.25):
        crack_lines(r, x, -0.9, np.pi / 2 + r.normal(0, 0.3), 1.6, 0.05, 30, 0.2, 0.2, out=lines)
    save("organic", "roots", strokes(u, v, lines) * D)
    r = np.random.default_rng(324)
    lines = []
    for _ in range(14):
        p = walk(r, *r.uniform(-0.9, 0.9, 2), r.uniform(0, 6.3), r.uniform(0.8, 1.6), 40, 0.2, 0.0, 0.05)
        lines.append((p, taper(len(p), r.uniform(0.02, 0.05), p=0.3), r.uniform(0.6, 1)))
    save("organic", "root tangle", strokes(u, v, lines) * D)

    f1, *_ = tworley(u, v, 50, seed=325)
    fine = (1 - np.clip(f1, 0, 1)) ** 2
    blob = smooth((0.6 - R + 0.3 * (tperlin(u, v, 3, 4, seed=326) - 0.5)) / 0.15)
    save("organic", "moss clump", blob * (0.4 + 0.6 * fine) * (0.6 + 0.4 * tperlin(u, v, 8, 3, seed=327)) * D)
    patches = smooth((tperlin(u, v, 3, 5, seed=328) - 0.5) / 0.08)
    save("organic", "moss patches", patches * (0.4 + 0.6 * fine) * D)
    r = np.random.default_rng(329)
    img = np.zeros_like(u)
    for _ in range(9):
        ph = r.uniform(0, 6)
        stamp(u, v, img, *r.uniform(-0.6, 0.6, 2), r.uniform(0.15, 0.35), 0,
              lambda lu, lv, ph=ph: smooth((1 - np.hypot(lu, lv) * (1 + 0.15 * np.sin(np.arctan2(lv, lu) * 7 + ph))) / 0.15)
              * (0.5 + 0.5 * np.cos(np.hypot(lu, lv) * 30 + 3 * np.sin(np.arctan2(lv, lu) * 7 + ph))))
    save("organic", "lichen", img * D)
    r = np.random.default_rng(330)
    lines = crack_lines(r, 0, 0.9, -np.pi / 2, 1.5, 0.06, 26, 0.25, 0.35)
    save("organic", "branching coral", strokes(u, v, lines) * D)
    img = np.zeros_like(u)
    r = np.random.default_rng(331)
    lines = []
    for k in range(3):
        x = np.linspace(-0.9, 0.9, 80)
        y = -0.45 + k * 0.45 + 0.12 * np.sin(x * 5 + k * 2)
        lines.append((np.stack([x, y], 1), 0.018))
        for j in range(4, 80, 9):
            side = 1 if (j // 9) % 2 else -1
            stamp(u, v, img, x[j], y[j] + side * 0.07, 0.08, side * np.pi / 2 + r.normal(0, 0.3), lambda lu, lv: leaf(lu, lv, 0.45, 0.7, 6))
    save("organic", "vines", np.maximum(img, strokes(u, v, lines)) * D)
    r = np.random.default_rng(332)
    lines = []
    for _ in range(70):
        a = r.uniform(0, np.pi)
        x, y = r.uniform(-0.75, 0.75, 2)
        ln = r.uniform(0.3, 0.5)
        lines.append(([(x - np.cos(a) * ln / 2, y - np.sin(a) * ln / 2), (x + np.cos(a) * ln / 2, y + np.sin(a) * ln / 2)], [0.01, 0.004]))
    save("organic", "pine needles", strokes(u, v, lines) * D)
    gills = (1 - np.abs(np.sin(A * 45))) ** 4
    short = (1 - np.abs(np.sin(A * 45 + np.pi / 2))) ** 4 * smooth((R - 0.55) / 0.05)
    save("organic", "gills", np.maximum(gills, short) * smooth((R - 0.12) / 0.04) * D)
    lx, ly, *_ = hexgrid(u, v, 9)
    save("organic", "wax comb", (smooth((hexd(lx, ly) - 0.4) / 0.08) * 0.9 + 0.1 * tperlin(u, v, 10, 3, seed=333)) * D)
    k = np.arange(400)
    rad = 0.88 * np.sqrt(k / 400)
    th = k * 2.39996
    save("organic", "seed spiral", bumps(u, v, rad * np.cos(th), rad * np.sin(th), 0.025 + 0.025 * rad, np.sqrt, 0.5) * D)


def scale_rows(u, v, fn, cols, rows, size, seed, jitter=0.01, stagger=0.5, angle=0.0):
    """Overlapping scales stamped row by row from the bottom up so each row tucks under the one above."""
    r = np.random.default_rng(seed)
    img = np.zeros_like(u)
    for j in range(rows, -2, -1):
        for i in range(-1, cols + 1):
            x = -1 + (i + stagger * (j % 2)) * 2 / cols + r.normal(0, jitter)
            y = -1 + j * 2 / rows + r.normal(0, jitter)
            stamp(u, v, img, x, y, size, angle + r.normal(0, 0.04), fn, "over", 2.0)
    return img


def skin_more():
    u, v = grid(512)
    D = disc(u, v)
    R = np.sqrt(u * u + v * v)
    f1, _, ident, *_ = tworley(u, v, 60, seed=401)
    a = smooth(1 - f1 / 0.3) * (cellvals(ident, 60, 402) > 0.3)
    f1, _, ident, *_ = tworley(u, v, 20, seed=403)
    b = smooth(1 - f1 / 0.25) * (cellvals(ident, 20, 404) > 0.6)
    save("skin", "mixed pores", np.maximum(a * 0.7, b) * D)
    f1, *_ = tworley(u * 2.5, v, 30, seed=405)
    save("skin", "stretched pores", smooth(1 - f1 / 0.3) * D)
    f1, f2, *_ = tworley(u, v, 30, seed=406)
    p1, *_ = tworley(u, v, 45, seed=407)
    save("skin", "skin grooves", np.maximum(smooth(1 - (f2 - f1) / 0.06) * 0.8, smooth(1 - p1 / 0.2)) * D)
    w = tperlin(u, v, 4, 3, seed=408) - 0.5
    f1, f2, *_ = tworley(u + w * 0.05, v + w * 0.05, 60, seed=409)
    save("skin", "skin micro", smooth(1 - (f2 - f1) / 0.1) * D)

    r = np.random.default_rng(410)
    lines = []
    for k in range(5):
        p = walk(r, -0.85, -0.45 + k * 0.22 + r.normal(0, 0.02), r.normal(0, 0.05), 1.7, 40, 0.03, 0.0, 0.1)
        lines.append((p, taper(len(p), r.uniform(0.02, 0.035), p=0.4)))
    save("skin", "forehead wrinkles", strokes(u, v, lines, round_p) * D)
    r = np.random.default_rng(411)
    lines = []
    for _ in range(90):
        a = r.choice([0.5, -0.5]) + r.normal(0, 0.1)
        p = walk(r, *r.uniform(-0.85, 0.85, 2), a, r.uniform(0.1, 0.3), 8, 0.08)
        lines.append((p, taper(len(p), 0.007), r.uniform(0.5, 1)))
    save("skin", "fine wrinkles", strokes(u, v, lines, round_p) * D)
    w = tperlin(u, v, 4, 3, seed=412) - 0.5
    rr = np.sqrt(u * u + (v * 2.2) ** 2) + w * 0.06
    gaps = smooth((tperlin(u, v, 6, 2, seed=413) - 0.35) / 0.1)
    save("skin", "knuckle wrinkles", (1 - np.abs(np.sin(rr * np.pi * 7))) ** 8 * gaps * smooth((0.75 - np.abs(u)) / 0.3) * smooth((rr - 0.1) / 0.1) * D)
    save("skin", "neck folds", (0.5 + 0.5 * np.cos((v + 0.15 * u * u + 0.05 * w) * np.pi * 5)) ** 3 * D)
    l1 = (1 - np.abs(np.sin((u * 0.77 + v * 0.64 + w * 0.1) * np.pi * 10))) ** 10
    l2 = (1 - np.abs(np.sin((-u * 0.77 + v * 0.64 + w * 0.1) * np.pi * 10))) ** 10
    save("skin", "crosshatch wrinkles", np.maximum(l1, l2) * D)

    f1, f2, ident, *_ = tworley(u, v, 22, seed=414, jitter=0.9)
    beads = np.sqrt(smooth(((f2 - f1) / (f2 + f1)) / 0.5)) * (0.7 + 0.3 * cellvals(ident, 22, 415))
    save("skin", "lizard beads", beads * D)
    lx, ly, cx, cy = hexgrid(u, v, 12)
    save("skin", "lizard scales", np.sqrt(smooth((0.46 - hexd(lx, ly)) / 0.3)) * (0.75 + 0.25 * hash2(cx, cy, 416)) * D)
    k = 7
    fa, fb = np.mod((u + v) * k, 1) - 0.5, np.mod((v - u) * k, 1) - 0.5
    ins = smooth((0.5 - np.maximum(np.abs(fa), np.abs(fb))) / 0.07)
    save("skin", "snake scales", ins * np.clip(0.3 + 0.55 * smooth((fa + fb + 1) / 2) + 0.25 * smooth(1 - np.abs(fa - fb) / 0.1), 0, 1) * D)
    t = np.mod(v * 5, 1)
    save("skin", "snake belly", t ** 1.5 * smooth((1 - t) / 0.06) * smooth(-rbox(u, v * 0, 0.7, 1, 0.0) / 0.15) * D)

    def fish(lu, lv):
        d = np.hypot(lu, lv)
        return (0.35 + 0.65 * smooth((lv + 1) / 2)) * smooth((1 - d) / 0.25), smooth((1 - d) / 0.06)

    save("skin", "overlapping scales", scale_rows(u, v, fish, 10, 14, 0.13, 417) * D)

    def dragon(lu, lv, ridge=0.3):
        wd = np.where(lv < 0, np.sqrt(np.clip(1 - lv * lv, 0, 1)), np.clip(1 - lv / 1.6, 0, 1))
        val = (0.35 + 0.65 * np.clip((lv + 1) / 2.6, 0, 1)) + ridge * smooth(1 - np.abs(lu) / 0.12) * np.clip((lv + 0.5) / 2, 0, 1)
        return np.clip(val, 0, 1) * smooth((wd - np.abs(lu)) / 0.3), smooth((wd - np.abs(lu)) / 0.05) * (lv > -1) * (lv < 1.6)

    save("skin", "dragon scales", scale_rows(u, v, dragon, 7, 9, 0.17, 418) * D)
    save("skin", "single scale", stamp(u, v, np.zeros_like(u), 0, -0.15, 0.5, 0, lambda lu, lv: np.prod(dragon(lu, lv), axis=0), reach=2.0) * D)

    def keeled(lu, lv):
        d = np.hypot(lu, lv / 1.7)
        return np.clip((0.35 + 0.5 * smooth((lv + 1.7) / 3.4)) * smooth((1 - d) / 0.3) + 0.3 * smooth(1 - np.abs(lu) / 0.12) * (d < 1), 0, 1), smooth((1 - d) / 0.08)

    save("skin", "keeled scales", scale_rows(u, v, keeled, 10, 10, 0.1, 419) * D)
    lx, ly, bw, bh, row, col = bricks(u, v, 5, 6, 0.15)
    plate = smooth(-rbox(lx, ly, bw - 0.03, bh - 0.03, 0.06) / 0.08)
    save("skin", "scutes", np.clip(plate * (0.55 + 0.45 * smooth(1 - np.abs(lx) / (bw * 0.35))) * (0.85 + 0.15 * hash2(row, col, 420)) + 0.08 * tperlin(u, v, 20, 3, seed=421) * plate, 0, 1) * D)
    lx, ly, bw, bh, row, col = bricks(u, v, 14, 7, 0.5)
    tb = (ly + bh) / (2 * bh)
    save("skin", "armadillo bands", smooth(-rbox(lx, ly, bw - 0.012, bh - 0.01, 0.02) / 0.02) * (0.35 + 0.65 * tb ** 1.5) * D)
    lx, ly, cx, cy = hexgrid(u, v, 3.2)
    h = hexd(lx, ly)
    save("skin", "shell plates", smooth((0.47 - h) / 0.04) * np.clip(0.55 + 0.25 * np.cos(h * np.pi * 22) + 0.2 * (1 - h * 2), 0, 1) * D)
    lx, ly, *_ = hexgrid(u, v, 40)
    save("skin", "denticles", smooth((0.42 - hexd(lx, ly)) / 0.12) * (0.6 + 0.4 * np.abs(np.cos(lx * np.pi * 5))) * D)

    w = tperlin(u, v, 4, 3, seed=422) - 0.5
    f = (v + 1) * 8 + 0.6 * tperlin(u, v, (2, 4), 3, seed=423)
    t = f - np.floor(f)
    save("skin", "horn rings", np.clip(smooth(t / 0.6) * smooth((1 - t) / 0.08) * (0.85 + 0.15 * tperlin(u, v, (40, 2), 2, seed=424)), 0, 1) * D)
    xh = u / (1.15 - 0.55 * (v + 1) / 2)
    sil = smooth((1 - np.abs(xh)) / 0.15)
    save("skin", "horn ridges", sil * (0.4 + 0.6 * np.abs(np.sin((xh + 0.05 * w) * np.pi * 6)) ** 0.5) * (0.85 + 0.15 * smooth(t / 0.3)) * D)

    def feather(lu, lv, seed=0):
        x = lu - 0.1 * lv * lv
        t = np.clip((1 - lv) / 2, 0, 1)
        wd = 0.42 * np.sin(np.pi * np.clip(t * 0.95 + 0.05, 0, 1)) ** 0.6 * np.where(x > 0, 1.0, 0.75)
        ins = smooth((wd - np.abs(x)) / 0.04) * (np.abs(lv) < 1)
        split = tperlin(lu, lv, (2, 6), 2, seed=seed) * 0.25
        barbs = (0.5 + 0.5 * np.cos((lv + np.abs(x) * 0.7 + split) * np.pi * 40)) ** 0.5
        shaft = smooth(1 - np.abs(x) / 0.025) * (lv < 1.2) * (lv > -1)
        return np.maximum(ins * (0.35 + 0.45 * barbs) * (1 - 0.3 * np.abs(x) / 0.42), shaft)

    save("skin", "feather", stamp(u, v, np.zeros_like(u), 0, 0, 0.85, 0.35, feather, reach=1.3) * D)
    r = np.random.default_rng(425)
    lines = []
    for a in np.linspace(0, 2 * np.pi, 90, endpoint=False):
        p = walk(r, 0, 0, a + r.normal(0, 0.05), r.uniform(0.5, 0.85), 14, 0.12, r.normal(0, 0.03))
        lines.append((p, taper(len(p), 0.012, "end", 0.5), r.uniform(0.4, 0.9)))
    save("skin", "down feather", np.maximum(strokes(u, v, lines, round_p), smooth(1 - R / 0.08)) * D)

    def tip(lu, lv):
        d = np.hypot(lu, np.maximum(lv, 0) * 0.8 + np.minimum(lv, 0) * 0.5)
        barbs = (0.5 + 0.5 * np.cos((lv * 0.6 + np.abs(lu)) * np.pi * 18)) ** 0.5
        shaft = smooth(1 - np.abs(lu) / 0.05)
        return np.maximum((0.3 + 0.4 * barbs) * smooth((lv + 1.5) / 3), shaft * 0.9) * smooth((1 - d) / 0.2), smooth((1 - d) / 0.05)

    save("skin", "overlapping feathers", scale_rows(u, v, tip, 7, 9, 0.17, 426) * D)
    r = np.random.default_rng(427)
    xy = r.uniform(-0.85, 0.85, (320, 2))
    hide = bumps(u, v, xy[:, 0], xy[:, 1], r.uniform(0.02, 0.07, 320), np.sqrt, 0.6, r.uniform(0.5, 1, 320))
    save("skin", "bumpy hide", np.clip(hide + 0.1 * tperlin(u, v, 30, 2, seed=428), 0, 1) * D)
    r = np.random.default_rng(429)
    centres = r.uniform(-0.7, 0.7, (18, 2))
    warts = np.zeros_like(u)
    for cx, cy in centres:
        k = r.integers(3, 9)
        xs, ys = cx + r.normal(0, 0.06, k), cy + r.normal(0, 0.06, k)
        bumps(u, v, xs, ys, r.uniform(0.02, 0.05, k), np.sqrt, 0.7, img=warts)
        bumps(u, v, [cx], [cy], [r.uniform(0.06, 0.1)], np.sqrt, 0.7, img=warts)
    save("skin", "warts", np.clip(warts * (1 - 0.3 * smooth(1 - tworley(u, v, 60, seed=430)[0] / 0.3)) + 0.12 * tperlin(u, v, 12, 3, seed=431), 0, 1) * D)
    w = tperlin(u, v, 3, 3, seed=432) - 0.5
    a = tworley(u + w * 0.2, v + w * 0.2, 6, seed=433)
    b = tworley(u + w * 0.2, v + w * 0.2, 18, seed=434)
    save("skin", "elephant skin", np.maximum(smooth(1 - (a[1] - a[0]) / 0.07), 0.55 * smooth(1 - (b[1] - b[0]) / 0.08)) * D)


def fabric_more():
    u, v = grid(512)
    D = disc(u, v)
    s = 9
    fx, fy = np.mod((u + 1) / 2 * s, 1), np.mod((v + 1) / 2 * s, 1)
    even = (np.floor((u + 1) / 2 * s) + np.floor((v + 1) / 2 * s)) % 2 == 0
    h = np.abs(np.sin(np.pi * fy * 3)) ** 0.6 * (0.6 + 0.4 * smooth(np.minimum(fx, 1 - fx) / 0.15))
    vv = np.abs(np.sin(np.pi * fx * 3)) ** 0.6 * (0.6 + 0.4 * smooth(np.minimum(fy, 1 - fy) / 0.15))
    save("fabric", "basket weave", np.where(even, h, vv) * D)
    col = np.floor((u + 1) * 5)
    dirn = np.where(col % 2 == 0, 1, -1)
    t = np.mod((v + dirn * u) * 12, 1)
    save("fabric", "herringbone", np.sin(np.pi * t) ** 0.7 * smooth(np.minimum(np.mod((u + 1) * 5, 1), 1 - np.mod((u + 1) * 5, 1)) / 0.08) * D)
    k = 28
    base = np.abs(np.sin(np.pi * (v + 1) / 2 * k)) ** 0.5
    rowi = np.floor((v + 1) / 2 * k)
    dip = smooth(np.abs(np.mod((u + 1) / 2 * k / 5 + rowi * 0.4, 1) - 0.5) / 0.12)
    save("fabric", "satin weave", base * (0.52 + 0.48 * dip) * D)
    k = 40
    over = np.sin(u * np.pi * k) * np.sin(v * np.pi * k)
    thick = 0.8 + 0.4 * tperlin(u, v, (4, 40), 2, seed=501)
    warp = 0.5 + 0.5 * np.cos(v * np.pi * k * 2)
    weft = 0.5 + 0.5 * np.cos(u * np.pi * k * 2)
    save("fabric", "canvas", np.clip(np.where(over > 0, warp, weft) * thick, 0, 1) * D)
    diag = np.mod((u + v) * 18, 1.0)
    slub = 0.75 + 0.25 * tperlin(u, v, (2, 30), 3, seed=502)
    save("fabric", "denim", np.clip((0.25 + 0.75 * smooth(1 - np.abs(diag - 0.5) / 0.4)) * slub, 0, 1) * D)

    k = 10
    cx = np.mod(u * k * 0.5, 1.0) - 0.5
    colk = np.floor(u * k * 0.5)
    cy = np.mod(v * k * 0.7 + np.abs(cx) * 1.2, 1.0)
    knit = smooth(1 - np.abs(cy - 0.5) / 0.45) * smooth(1 - np.abs(cx) / 0.55)
    save("fabric", "rib knit", knit * np.where(colk % 3 == 0, 0.35, 1.0) * D)
    ph = v * np.pi * 3
    s1, s2 = 0.13 * np.sin(ph), -0.13 * np.sin(ph)
    z1 = np.cos(ph)
    b1 = smooth(1 - np.abs(u - s1) / 0.13)
    b2 = smooth(1 - np.abs(u - s2) / 0.13)
    cable = np.where(z1 > 0, np.maximum(b1, b2 * (1 - b1)), np.maximum(b2, b1 * (1 - b2)))
    tex = 0.75 + 0.25 * (0.5 + 0.5 * np.cos(v * np.pi * 40))
    side = knit * smooth((np.abs(u) - 0.42) / 0.04) * 0.7
    save("fabric", "cable knit", np.maximum(cable * tex * smooth((0.36 - np.abs(u)) / 0.02), side) * D)
    k = 24
    gx, gy = np.mod((u + 1) / 2 * k, 1), np.mod((v + 1) / 2 * k + 0.5 * (np.floor((u + 1) / 2 * k) % 2), 1)
    save("fabric", "seed stitch", dome_p(np.hypot(gx - 0.5, gy - 0.5) / 0.45) * D)
    k = 12
    gi, gj = np.floor((u + 1) / 2 * k) - k / 2 + 0.5, np.floor((v + 1) / 2 * k) - k / 2 + 0.5
    lx, ly = np.mod((u + 1) / 2 * k, 1) - 0.5, np.mod((v + 1) / 2 * k, 1) - 0.5
    x1 = smooth(1 - np.abs(lx - ly) / 0.14) * smooth((0.45 - np.abs(lx + ly)) / 0.08)
    x2 = smooth(1 - np.abs(lx + ly) / 0.14) * smooth((0.45 - np.abs(lx - ly)) / 0.08)
    motif = (np.abs(gi) + np.abs(gj) <= 4.5) & ((np.abs(gi) + np.abs(gj)) % 2 < 1.5)
    save("fabric", "cross stitch", np.maximum(x1, x2 * 0.9) * motif * D)
    xs = np.linspace(-0.85, 0.85, 29)
    ys = np.where(np.arange(29) % 2 == 0, -0.08, 0.08)
    thread = (0.85 + 0.15 * np.cos((u + v) * np.pi * 60))
    save("fabric", "zigzag stitch", strokes(u, v, [(np.stack([xs, ys], 1), 0.018)]) * thread * D)
    lines = [([(-0.88, 0.1), (0.88, 0.1)], 0.016)]
    for x in np.linspace(-0.8, 0.8, 13):
        lines.append(([(x, 0.1), (x, -0.18)], 0.016))
    save("fabric", "blanket stitch", strokes(u, v, lines) * thread * edge(np.abs(u) - 0.92, 0.08))
    dash = smooth(1 - np.abs(np.mod(u * 6, 1.0) - 0.5) / 0.32)
    rows = np.maximum(edge(np.abs(v - 0.1) - 0.035, 0.05), edge(np.abs(v + 0.1) - 0.035, 0.05))
    save("fabric", "double stitches", dash * rows * edge(np.abs(u) - 0.92, 0.08))

    def seam(y):
        pipe = dome_p(np.abs(y) / 0.08) * (0.9 + 0.1 * np.sin(u * np.pi * 14))
        st = smooth(1 - np.abs(np.mod(u * 7, 1.0) - 0.5) / 0.3) * edge(np.abs(np.abs(y) - 0.17) - 0.022, 0.03) * 0.6
        fold = 0.25 * smooth(1 - np.abs(y) / 0.35)
        return np.maximum(np.maximum(pipe, st), fold) * edge(np.abs(u) - 0.92, 0.08) * edge(np.abs(y) - 0.4, 0.1)

    save("fabric", "seam", seam(v))
    save("fabric", "curved seam", seam(np.sqrt(u * u + (v - 1.4) ** 2) - 1.4) * D)
    k = 4
    fx, fy = np.mod((u + 1) / 2 * k, 1) * 2 - 1, np.mod((v + 1) / 2 * k, 1) * 2 - 1
    pillow = np.sqrt(np.clip((1 - fx ** 4) * (1 - fy ** 4), 0, 1))
    save("fabric", "quilted squares", pillow * (1 - 0.4 * smooth(1 - np.hypot(np.abs(fx) - 1, np.abs(fy) - 1) / 0.25)) * D)
    save("fabric", "channel quilting", np.abs(np.sin(np.pi * (v + 1) / 2 * 5)) ** 0.5 * D)
    f1, f2, *_ = tworley(u, v, 40, seed=503)
    save("fabric", "fine leather", np.clip(smooth((f2 - f1) / 0.2) * 0.7 + 0.3 * tperlin(u, v, 30, 2, seed=504), 0, 1) * D)
    f1, f2, ident, *_ = tworley(u, v, 26, seed=505)
    save("fabric", "pebbled leather", np.clip(np.sqrt(smooth((f2 - f1) / 0.45)) * (0.75 + 0.25 * cellvals(ident, 26, 506)) + 0.1 * tperlin(u, v, 40, 2, seed=507), 0, 1) * D)
    r = np.random.default_rng(508)
    lines = []
    for _ in range(9):
        p = walk(r, *r.uniform(-0.7, 0.7, 2), r.uniform(0, 6.3), r.uniform(0.4, 0.9), 20, 0.12)
        lines.append((p, taper(len(p), r.uniform(0.02, 0.04)), r.uniform(0.6, 1)))
    creases = strokes(u, v, lines, round_p)
    save("fabric", "creased leather", np.clip((smooth((f2 - f1) / 0.3) * 0.6 + 0.4) * (1 - 0.6 * creases), 0, 1) * D)
    A = np.arctan2(v + 1.1, u)
    dist = np.sqrt(u * u + (v + 1.1) ** 2)
    save("fabric", "gathered folds", np.abs(np.sin(A * 16 + 0.6 * tperlin(u, v, 3, 3, seed=509))) ** 0.7 * smooth((1.9 - dist) / 0.6) * D)
    w = tperlin(u, v, (2, 1), 3, seed=510) - 0.5
    x = u + w * 0.2
    save("fabric", "drapery", smooth(0.5 + 0.25 * np.sin(x * 9) + 0.15 * np.sin(x * 17 + 1) + 0.1 * np.sin(x * 31 + v * 2)) * D)
    save("fabric", "crumpled", np.clip(0.5 * ridged(u, v, 3, 4, seed=511) + 0.5 * tperlin(u, v, 2, 3, seed=512), 0, 1) ** 1.3 * D)
    save("fabric", "corduroy", np.abs(np.sin(np.pi * u * 12)) ** 0.4 * (0.85 + 0.15 * tnoise(u, v, (16, 128), 2, seed=513)) * D)
    w = tperlin(u, v, 8, 3, seed=514) - 0.5
    save("fabric", "felt", np.clip(stretch(tperlin(u + w * 0.08, v - w * 0.08, 48, 3, seed=515)) * 0.8 + 0.2 * tperlin(u, v, 4, 3, seed=516), 0, 1) * D)
    k = 7
    l1 = smooth(1 - np.abs(np.mod((u + v) * k, 1) - 0.5) / 0.1)
    l2 = smooth(1 - np.abs(np.mod((u - v) * k, 1) - 0.5) / 0.1)
    save("fabric", "net", np.clip(np.maximum(l1, l2) * 0.8 + l1 * l2 * 0.4, 0, 1) * D)


def hard_more():
    u, v = grid(256)
    R = np.sqrt(u * u + v * v)
    A = np.arctan2(v, u)
    P = disc(u, v, 0.9, 0.05)
    hexh = np.maximum(np.abs(u) * 0.866 + np.abs(v) * 0.5, np.abs(v))
    washer = smooth((0.85 - R) / 0.04) * 0.45
    head = edge(hexh - 0.55, 0.03) * (0.8 + 0.2 * smooth((0.55 - R) / 0.15))
    save("hard surface", "flange bolt", np.maximum(washer, head))
    ca, sa = np.cos(0.2), np.sin(0.2)
    ru, rv = u * ca + v * sa, -u * sa + v * ca
    nut = edge(np.maximum(np.abs(ru), np.abs(rv)) - 0.7, 0.04) * (0.85 + 0.15 * smooth((0.85 - R) / 0.3))
    save("hard surface", "square nut", nut * (1 - smooth((0.3 - R) / 0.03)) + smooth((0.3 - R) / 0.03) * 0.25 * (0.5 + 0.5 * np.cos(R * np.pi * 30)))
    dome = np.sqrt(np.clip(1 - (R / 0.75) ** 2, 0, 1))
    star = edge(R - (0.24 + 0.08 * np.cos(A * 6)), 0.03)
    save("hard surface", "star screw", dome * (1 - 0.75 * star))
    cyl = smooth((0.75 - R) / 0.08) * 0.9
    save("hard surface", "socket screw", cyl * (1 - 0.8 * edge(np.maximum(np.abs(u) * 0.866 + np.abs(v) * 0.5, np.abs(v)) - 0.3, 0.03)))
    save("hard surface", "washer", smooth((0.8 - R) / 0.05) * smooth((R - 0.35) / 0.05) * (0.85 + 0.15 * smooth(1 - np.abs(R - 0.57) / 0.25)))
    gx, gy = np.mod((u + 1) / 2 * 5, 1) - 0.5, np.mod((v + 1) / 2 * 5, 1) - 0.5
    save("hard surface", "rivet grid", dome_p(np.hypot(gx, gy) / 0.3) * P)
    rr = np.zeros_like(u)
    for a in np.linspace(0, 2 * np.pi, 12, endpoint=False):
        rr = np.maximum(rr, dome_p(np.hypot(u - 0.68 * np.cos(a), v - 0.68 * np.sin(a)) / 0.11))
    save("hard surface", "rivet ring", rr)
    save("hard surface", "countersunk rivet", smooth((0.7 - R) / 0.05) * (0.75 + 0.25 * dome_p(R / 0.7)) * (1 - 0.3 * edge(np.abs(R - 0.7) - 0.03, 0.03)))
    rings = edge(np.abs(np.mod(R * 7, 1) - 0.5) - 0.22, 0.12) * (R > 0.2) * edge(R - 0.82, 0.04)
    spokes = edge(np.abs(np.sin(A * 2)) * R - 0.04, 0.03)
    save("hard surface", "round vent", rings * (1 - spokes))
    lx, ly, bw, bh, row, col = bricks(u * 1.1, v * 1.1, 3, 8, 0.5)
    slot = edge(np.hypot(np.maximum(np.abs(lx) - (bw - 0.16), 0), ly) - 0.06, 0.05) * edge(rbox(u, v, 0.86, 0.86, 0.1), 0.05)
    save("hard surface", "slotted vent", slot)
    t = np.mod(v * 4, 1)
    save("hard surface", "louvre vent", t * smooth((1 - t) / 0.05) * edge(rbox(u, v, 0.8, 0.8, 0.08), 0.05))
    k = 5
    l1 = edge(np.abs(np.mod((u + v) * k, 1) - 0.5) - 0.1, 0.06)
    l2 = edge(np.abs(np.mod((u - v) * k, 1) - 0.5) - 0.1, 0.06)
    save("hard surface", "diamond grille", np.maximum(l1, l2) * P)
    t = np.mod((v + 1) * 4, 1) * 2 - 1
    save("hard surface", "grille bars", np.maximum(dome_p(np.abs(t) / 0.75) * edge(np.abs(u) - 0.75, 0.04), edge(np.abs(rbox(u, v, 0.86, 0.86, 0.06)) - 0.04, 0.03)) * (np.abs(v) < 0.92))
    lx, ly, *_ = hexgrid(u, v, 16)
    save("hard surface", "speaker grille", edge(np.hypot(lx, ly) - 0.32, 0.08) * disc(u, v, 0.86, 0.04))
    gw = 0.035
    corner = np.minimum(np.where(u > 0, np.abs(v), np.hypot(u, v)), np.where(v > 0, np.abs(u), np.hypot(u, v)))
    save("hard surface", "panel corner", edge(corner - gw, 0.03) * edge(np.maximum(u, v) - 0.92, 0.06))
    tj = np.minimum(np.abs(v), np.where(v > 0, np.abs(u), 9))
    save("hard surface", "panel tee", edge(tj - gw, 0.03) * edge(np.maximum(np.abs(u), np.abs(v)) - 0.92, 0.06))
    save("hard surface", "panel cross", edge(np.minimum(np.abs(u), np.abs(v)) - gw, 0.03) * edge(np.maximum(np.abs(u), np.abs(v)) - 0.92, 0.06))
    save("hard surface", "panel inset", edge(np.abs(rbox(u, v, 0.72, 0.5, 0.15)) - gw, 0.03))
    handle = edge(rbox(u, v - 0.45, 0.3, 0.06, 0.06), 0.03)
    save("hard surface", "hatch", np.maximum(edge(np.abs(rbox(u, v, 0.8, 0.8, 0.2)) - gw, 0.03), handle))
    lx, ly, cx, cy = hexgrid(u, v, 6)
    save("hard surface", "hex tiles", smooth((0.46 - hexd(lx, ly)) / 0.06) * (0.7 + 0.3 * hash2(cx, cy, 601)) * P)

    gx, gy = np.mod((u + 1) / 2 * 4, 1) * 2 - 1, np.mod((v + 1) / 2 * 4, 1) * 2 - 1
    grip = np.where((np.floor((u + 1) * 2) + np.floor((v + 1) * 2)) % 2 == 0, rbox(gx, gy, 0.8, 0.3, 0.3), rbox(gx, gy, 0.3, 0.8, 0.3))
    save("hard surface", "grip pads", np.sqrt(np.clip(-grip / 0.15, 0, 1)) * P)
    lx, ly, bw, bh, *_ = bricks(u, v, 4, 4, 0.5)
    dia = np.abs(lx) / bw + np.abs(ly) / bh - 0.8
    save("hard surface", "embossed diamonds", np.clip(-dia / 0.25, 0, 1) * P)
    save("hard surface", "cooling fins", edge(np.abs(np.mod((u + 1) * 5, 1) - 0.5) - 0.08, 0.05) * edge(np.abs(v) - 0.8, 0.05) * edge(np.abs(u) - 0.85, 0.05))
    pins = np.zeros_like(u)
    for a in (-np.pi / 2, np.pi / 6, 5 * np.pi / 6):
        pins = np.maximum(pins, dome_p(np.hypot(u - 0.32 * np.cos(a), v - 0.32 * np.sin(a)) / 0.1))
    save("hard surface", "socket", np.maximum(edge(np.abs(R - 0.72) - 0.08, 0.04), pins) * 1.0)
    bolts = np.zeros_like(u)
    for a in np.linspace(0, 2 * np.pi, 8, endpoint=False) + np.pi / 8:
        bx, by = 0.66 * np.cos(a), 0.66 * np.sin(a)
        bolts = np.maximum(bolts, edge(np.maximum(np.abs(u - bx) * 0.866 + np.abs(v - by) * 0.5, np.abs(v - by)) - 0.08, 0.02))
    save("hard surface", "pipe flange", np.maximum(smooth((0.88 - R) / 0.04) * smooth((R - 0.42) / 0.03) * 0.55, bolts))
    port = np.maximum(edge(np.abs(R - 0.5) - 0.03, 0.03), edge(R - 0.35, 0.04) * 0.7)
    for a in np.linspace(0, 2 * np.pi, 6, endpoint=False):
        port = np.maximum(port, dome_p(np.hypot(u - 0.7 * np.cos(a), v - 0.7 * np.sin(a)) / 0.08))
    save("hard surface", "access port", port)

    u, v = grid(512)
    P = disc(u, v, 0.9, 0.05)
    r = np.random.default_rng(602)
    lines, pads = [], []
    for _ in range(22):
        x, y = np.round(r.uniform(-0.8, 0.8, 2) * 12) / 12
        pts, d = [(x, y)], r.integers(0, 4) * np.pi / 2
        for _ in range(r.integers(2, 5)):
            ln = r.integers(2, 7) / 12
            x, y = x + np.cos(d) * ln, y + np.sin(d) * ln
            pts.append((x, y))
            d += r.choice([-np.pi / 4, np.pi / 4, -np.pi / 2, np.pi / 2])
        lines.append((pts, 0.014))
        pads += [pts[0], pts[-1]]
    pads = np.array(pads)
    traces = strokes(u, v, lines, flat_p)
    padimg = bumps(u, v, pads[:, 0], pads[:, 1], np.full(len(pads), 0.035), lambda t: t, 0.25)
    save("hard surface", "circuit traces", np.maximum(traces * 0.8, padimg) * P)
    r = np.random.default_rng(603)
    img = np.zeros_like(u)

    def greeble(x0, x1, y0, y1, depth):
        if depth == 0 or (x1 - x0 < 0.2 and y1 - y0 < 0.2):
            hx, hy = (x1 - x0) / 2 - 0.02, (y1 - y0) / 2 - 0.02
            if hx > 0.02 and hy > 0.02:
                np.maximum(img, r.uniform(0.3, 1) * smooth(-rbox(u - (x0 + x1) / 2, v - (y0 + y1) / 2, hx, hy, 0.02) / 0.015), out=img)
            return
        t = r.uniform(0.3, 0.7)
        if x1 - x0 > y1 - y0:
            m = x0 + (x1 - x0) * t
            greeble(x0, m, y0, y1, depth - 1)
            greeble(m, x1, y0, y1, depth - 1)
        else:
            m = y0 + (y1 - y0) * t
            greeble(x0, x1, y0, m, depth - 1)
            greeble(x0, x1, m, y1, depth - 1)

    greeble(-0.85, 0.85, -0.85, 0.85, 5)
    save("hard surface", "tech panel", img * P)
    knuck = dome_p(np.abs(v) / 0.12) * edge(np.abs(np.mod((u + 1) * 2.5, 1) - 0.5) - 0.44, 0.02) * edge(np.abs(u) - 0.85, 0.02)
    plate = edge(np.abs(v) - 0.6, 0.03) * edge(np.abs(u) - 0.8, 0.03) * 0.4
    holes = np.zeros_like(u)
    for x in (-0.5, 0, 0.5):
        for y in (-0.38, 0.38):
            holes = np.maximum(holes, smooth((0.07 - np.hypot(u - x, v - y)) / 0.03))
    save("hard surface", "hinge", np.maximum(knuck, plate * (1 - holes)))
    rim = edge(np.abs(rbox(u, v, 0.82, 0.5, 0.12)) - 0.03, 0.03)
    body = edge(rbox(u, v, 0.82, 0.5, 0.12), 0.03) * 0.45
    bars = np.zeros_like(u)
    for i, ln in enumerate((0.5, 0.35, 0.42)):
        bars = np.maximum(bars, edge(rbox(u + 0.15, v + 0.15 - i * 0.15, ln, 0.035, 0.03), 0.02) * 0.85)
    cr = np.zeros_like(u)
    for x in (-0.68, 0.68):
        for y in (-0.36, 0.36):
            cr = np.maximum(cr, dome_p(np.hypot(u - x, v - y) / 0.06))
    save("hard surface", "embossed plate", np.maximum(np.maximum(body, rim), np.maximum(bars, cr)))
    t = np.mod((u + 1) * 12 + (v / 0.12) ** 2 * 0.4, 1)
    save("hard surface", "weld bead", dome_p(np.abs(v + 0.02 * np.sin(u * 7)) / 0.12) * (0.75 + 0.25 * t) * edge(np.abs(u) - 0.88, 0.06))


def runes(seed, count):
    """Invented glyphs: a stem plus a few strokes between neighbouring points of a 3 x 4 lattice."""
    r = np.random.default_rng(seed)
    nodes = [(x, y) for y in (-0.6, -0.2, 0.2, 0.6) for x in (-0.35, 0.0, 0.35)]
    pairs = [(i, j) for i in range(12) for j in range(i + 1, 12)
             if abs(nodes[i][0] - nodes[j][0]) <= 0.36 and abs(nodes[i][1] - nodes[j][1]) <= 0.41]
    out = []
    for _ in range(count):
        c = r.choice([0, 1])
        lines = [[nodes[c], nodes[9 + c]]]
        want = r.integers(3, 6)
        for k in r.permutation(len(pairs)):
            i, j = pairs[k]
            if nodes[i][0] == nodes[j][0] == nodes[c][0]:
                continue
            lines.append([nodes[i], nodes[j]])
            if len(lines) >= want:
                break
        out.append(lines)
    return out


def ornament():
    u, v = grid(512)
    D = disc(u, v)
    R = np.sqrt(u * u + v * v)
    A = np.arctan2(v, u)

    def torus_knot(p, q, rr=0.5, width=0.065, n=1600):
        t = np.linspace(0, 2 * np.pi, n)
        rad = 1 + rr * np.cos(q * t)
        pts = np.stack([rad * np.cos(p * t), rad * np.sin(p * t)], 1) * (0.8 / (1 + rr))
        return band_knot(u, v, pts, np.sin(q * t), width)

    t = np.linspace(0, 2 * np.pi, 1600)
    pts = np.stack([np.sin(t) + 2 * np.sin(2 * t), np.cos(t) - 2 * np.cos(2 * t)], 1) * (0.8 / 3)
    save("ornament", "trefoil knot", band_knot(u, v, pts, -np.sin(3 * t), 0.07) * D)
    save("ornament", "five lobed knot", torus_knot(2, 5, 0.45, 0.055) * D)
    save("ornament", "seven lobed knot", torus_knot(2, 7, 0.4, 0.045) * D)
    save("ornament", "woven ring knot", torus_knot(3, 4, 0.35, 0.045) * D)
    t = np.linspace(0, 2 * np.pi, 2000)
    pts = np.stack([np.cos(3 * t + 0.7), np.cos(2 * t + 0.2)], 1) * 0.66
    save("ornament", "looped knot", band_knot(u, v, pts, np.cos(5 * t + 1.1), 0.05) * D)

    def plait(strands, amp, freq, width):
        x = np.linspace(-1.1, 1.1, 1400)
        pts_all, z_all = [], []
        for k in range(strands):
            ph = x * np.pi * freq + k * 2 * np.pi / strands
            pts_all.append(np.stack([x, amp * np.sin(ph)], 1))
            z_all.append(np.cos(ph))
        # Strands are rendered into one z-buffer by joining them with jumps far outside the image.
        pts, z = [], []
        for p, zz in zip(pts_all, z_all):
            pts += [np.array([[p[0, 0], 5.0]]), p, np.array([[p[-1, 0], 5.0]])]
            z += [np.array([-9.0]), zz, np.array([-9.0])]
        return band_knot(u, v, np.concatenate(pts), np.concatenate(z), width) * strip(u, v, amp + width + 0.05)

    save("ornament", "twist border", plait(2, 0.16, 2.5, 0.07))
    save("ornament", "braid border", plait(3, 0.2, 1.6, 0.065))

    glyphs = runes(701, 22)
    for i, g in enumerate(glyphs[:8]):
        save("ornament", f"rune {i + 1}", strokes(u, v, [(np.array(s) * 1.1, 0.075) for s in g], vee_p) * D)
    band = []
    for k, g in enumerate(glyphs[8:13]):
        band += [(np.array(s) * 0.32 + [(k - 2) * 0.34, 0], 0.028) for s in g]
    save("ornament", "rune band", strokes(u, v, band, vee_p) * strip(u, v, 0.3))
    ring = []
    for k, g in enumerate(glyphs[10:22]):
        a = k * 2 * np.pi / 12
        ca, sa = np.cos(a + np.pi / 2), np.sin(a + np.pi / 2)
        for s in g:
            p = np.array(s) * 0.17
            ring.append((np.stack([p[:, 0] * ca - p[:, 1] * sa + 0.66 * np.cos(a), p[:, 0] * sa + p[:, 1] * ca + 0.66 * np.sin(a)], 1), 0.018))
    rim = np.maximum(edge(np.abs(R - 0.86) - 0.015, 0.02), edge(np.abs(R - 0.46) - 0.015, 0.02))
    save("ornament", "rune ring", np.maximum(strokes(u, v, ring, vee_p), rim) * D)

    def scroll(pts, w0, w1):
        n = len(pts)
        t = np.abs(np.linspace(-1, 1, n))
        return strokes(u, v, [(pts, w0 + (w1 - w0) * t)])

    sp = curl_path(9, 1.6, 400, 1.6, "spiral")
    sp = sp * (1.3 / np.ptp(sp, axis=0).max())
    save("ornament", "scroll spiral", strokes(u, v, [(sp, np.linspace(0.07, 0.015, 400))]) * D)
    save("ornament", "s scroll", scroll(curl_path(5, 1.4, 400, 1.5, "s") * 0.95, 0.07, 0.015) * D)
    save("ornament", "c scroll", scroll(curl_path(5, 1.4, 400, 1.5, "c") * 0.95, 0.07, 0.015) * D)
    lines = [([(-0.95, 0.25), (0.95, 0.25)], 0.025)]
    sp = curl_path(30, 0.8, 200, 1.6, "spiral")
    sp = (sp - sp[0]) * (0.36 / np.ptp(sp, axis=0).max())
    for x in np.linspace(-0.85, 0.45, 4):
        lines.append((sp * [1, -1] + [x, 0.25], np.linspace(0.03, 0.012, 200)))
    save("ornament", "wave scroll border", strokes(u, v, lines) * strip(u, v, 0.35))

    def rosette(k, layers, pointed=False):
        img = np.zeros_like(u)
        for i, (r0, h) in enumerate(layers):
            a = A + i * np.pi / k
            c = np.abs(np.cos(k * a / 2))
            rho = r0 * ((0.35 + 0.65 * c ** 3) if pointed else (0.65 + 0.35 * c ** 0.6))
            petal = smooth((rho - R) / 0.03) * h * (0.6 + 0.4 * (1 - R / rho))
            crease = smooth(1 - np.abs(np.sin(k * a / 2)) * R / 0.03) * smooth((rho - R) / 0.05) * 0.15
            img = np.maximum(img, np.clip(petal + crease, 0, 1))
        centre = dome_p(R / 0.16)
        seeds = (0.85 + 0.15 * np.cos(u * 90) * np.cos(v * 90))
        return np.maximum(img, centre * seeds) * D

    save("ornament", "five petal rosette", rosette(5, [(0.88, 0.6), (0.55, 0.85)]))
    save("ornament", "six petal rosette", rosette(6, [(0.88, 0.6), (0.55, 0.85)]))
    save("ornament", "eight petal rosette", rosette(8, [(0.88, 0.55), (0.6, 0.8)]))
    save("ornament", "layered rosette", rosette(12, [(0.9, 0.45), (0.68, 0.65), (0.45, 0.85)]))
    save("ornament", "pointed rosette", rosette(8, [(0.9, 0.55), (0.6, 0.8)], True))
    beads = (0.5 + 0.5 * np.cos(A * 36)) ** 0.5 * edge(np.abs(R - 0.72) - 0.05, 0.03)
    med = np.maximum.reduce([edge(np.abs(R - 0.86) - 0.03, 0.03) * 0.8, beads * 0.9, edge(np.abs(R - 0.58) - 0.02, 0.03) * 0.7, rosette(6, [(0.5, 0.6), (0.32, 0.85)]) * (R < 0.5)])
    save("ornament", "medallion", med * D)

    rails = np.maximum(edge(np.abs(v - 0.22) - 0.02, 0.02), edge(np.abs(v + 0.22) - 0.02, 0.02)) * 0.7
    fx = np.mod((u + 1) * 4, 1) - 0.5
    save("ornament", "bead border", np.maximum(dome_p(np.hypot(fx / 4, v) / 0.11), rails) * strip(u, v, 0.28))
    fx2 = np.mod((u + 1) * 3, 1) * 2 - 1
    bead = dome_p(np.hypot(fx2 / 3, v) / 0.11)
    reel = dome_p(np.hypot((np.abs(fx2) - 1) / 3 / 0.09, v / 0.11)) * 0.9
    save("ornament", "bead and reel border", np.maximum(np.maximum(bead, reel), rails) * strip(u, v, 0.28))
    egg = dome_p(np.hypot(fx / 4 / 0.09, (v + 0.02) / 0.15))
    dart = np.clip(1 - np.abs(np.abs(fx) - 0.5) * 4 / 0.08 - np.maximum(v + 0.05, 0) * 4, 0, 1) * (v < 0.15) * 0.8
    save("ornament", "egg and dart border", np.maximum(np.maximum(egg, dart), rails) * strip(u, v, 0.28))
    unit = np.array([(0, 1), (0, 0), (0.8, 0), (0.8, 0.65), (0.3, 0.65), (0.3, 0.35), (0.55, 0.35)])
    lines = []
    for k in range(-3, 3):
        x0 = k * 0.36
        lines.append((unit * 0.36 * [1, 0.95] + [x0, -0.17], 0.022))
        lines.append(([(x0, 0.17), (x0 + 0.36, 0.17)], 0.022))
    save("ornament", "key border", strokes(u, v, lines, flat_p) * strip(u, v, 0.26))
    t = np.mod((u + v * 0.8) * 8, 1)
    save("ornament", "rope border", dome_p(np.abs(v) / 0.16) * np.sin(np.pi * t) ** 0.5 * strip(u, v, 0.2))
    fx3 = np.mod((u + 1) * 7, 1) - 0.5
    save("ornament", "dentil border", np.maximum(edge(np.abs(fx3) - 0.3, 0.05) * edge(np.abs(v - 0.05) - 0.12, 0.03) * 0.8, edge(np.abs(v + 0.18) - 0.04, 0.02)) * strip(u, v, 0.26))
    sc = np.hypot(fx / 4, v + 0.1) - 0.12
    save("ornament", "scallop border", np.maximum(edge(sc, 0.03) * (v > -0.1) * (0.6 + 0.4 * edge(np.abs(sc + 0.05) - 0.015, 0.02)), rails) * strip(u, v, 0.28))
    x = np.linspace(-1, 1, 200)
    stem = np.stack([x, 0.08 * np.sin(x * np.pi * 3)], 1)
    img = strokes(u, v, [(stem, 0.018)])
    r = np.random.default_rng(702)
    for i, xx in enumerate(np.linspace(-0.85, 0.85, 14)):
        side = 1 if i % 2 else -1
        y = 0.08 * np.sin(xx * np.pi * 3)
        stamp(u, v, img, xx + 0.04, y + side * 0.09, 0.1, side * 1.1 + r.normal(0, 0.1), lambda lu, lv: leaf(lu, lv, 0.4, 0.8, 6))
    save("ornament", "leaf garland border", img * strip(u, v, 0.3))
    t = np.abs(np.mod((u + 1) * 3, 1) - 0.5)
    save("ornament", "chevron border", edge(np.abs(v - (t * 0.5 - 0.12)) - 0.04, 0.03) * strip(u, v, 0.26))


def shapes_more():
    u, v = grid(256)
    R = np.sqrt(u * u + v * v)
    A = np.arctan2(v, u)
    save("shapes", "pentagon", edge(ngon(u, v, 5, 0.92, -np.pi / 2), 0.05))
    save("shapes", "hexagon", edge(ngon(u, v, 6, 0.92), 0.05))
    save("shapes", "octagon", edge(ngon(u, v, 8, 0.92, np.pi / 8), 0.05))
    save("shapes", "rounded square", edge(rbox(u, v, 0.86, 0.86, 0.25), 0.05))
    save("shapes", "ellipse", edge((np.hypot(u / 0.9, v / 0.5) - 1) * 0.5, 0.04))
    save("shapes", "capsule", edge(np.hypot(np.maximum(np.abs(u) - 0.55, 0), v) - 0.32, 0.05))
    save("shapes", "crescent", edge(np.maximum(R - 0.85, 0.72 - np.hypot(u - 0.35, v + 0.12)), 0.05))
    px, py = np.abs(u) / 1.35, -(v - 0.72) / 1.35
    mm = 0.5 * np.maximum(px + py, 0)
    lobe = np.sqrt((px - 0.25) ** 2 + (py - 0.75) ** 2) - np.sqrt(2) / 4
    tip = np.sqrt(np.minimum(px * px + (py - 1) ** 2, (px - mm) ** 2 + (py - mm) ** 2)) * np.sign(px - py)
    save("shapes", "heart", edge(np.where(px + py > 1, lobe, tip) * 1.35, 0.05))
    px, py = np.abs(u), -(v - 0.35)
    r1, h = 0.5, 1.25
    b = r1 / h
    a = np.sqrt(1 - b * b)
    k = -b * px + a * py
    drop = np.where(k < 0, np.hypot(px, py) - r1, np.where(k > a * h, np.hypot(px, py - h), a * px + b * py - r1))
    save("shapes", "teardrop", edge(drop, 0.05))
    tri = np.maximum(np.abs(v) - (0.9 - u) * 0.75, 0.1 - u)
    shaft = np.maximum(np.abs(v) - 0.17, np.maximum(-0.9 - u, u - 0.2))
    save("shapes", "arrow", edge(np.minimum(tri, shaft), 0.05))
    save("shapes", "chevron", edge(np.maximum(np.abs(v - (0.55 * np.abs(u) - 0.25)) - 0.18, np.abs(u) - 0.85), 0.05))
    gear = R - (0.72 + 0.12 * np.clip(np.cos(A * 12) * 3, -1, 1))
    save("shapes", "gear", edge(gear, 0.04) * smooth((R - 0.22) / 0.04))
    save("shapes", "thin ring", edge(np.abs(R - 0.75) - 0.05, 0.04))
    save("shapes", "soft ring", np.exp(-((R - 0.6) / 0.18) ** 2) * edge(R - 0.95, 0.05))

    def star(k, inner):
        m = np.mod(A + np.pi / 2, 2 * np.pi / k) - np.pi / k
        return edge(R - 0.92 * (inner + (1 - inner) * (1 - np.abs(m) / (np.pi / k))), 0.04)

    save("shapes", "four point star", star(4, 0.3))
    save("shapes", "six point star", star(6, 0.5))
    save("shapes", "eight point star", star(8, 0.55))
    s = R * 4 - (A + np.pi) / (2 * np.pi)
    save("shapes", "spiral", edge(np.abs(np.mod(s, 1) - 0.5) - 0.2, 0.08) * edge(R - 0.9, 0.06))
    D = disc(u, v)
    save("shapes", "linear gradient", (u + 1) / 2 * D)
    save("shapes", "angular gradient", (A + np.pi) / (2 * np.pi) * D)
    save("shapes", "cone", np.clip(1 - R / 0.9, 0, 1))
    save("shapes", "pyramid", np.clip(1 - np.maximum(np.abs(u), np.abs(v)) / 0.9, 0, 1))
    save("shapes", "plateau", smooth((0.9 - R) / 0.35))
    save("shapes", "sharp falloff", np.clip(1 - R / 0.9, 0, 1) ** 3)
    save("shapes", "half dome", np.sqrt(np.clip(1 - (R / 0.9) ** 2, 0, 1)) * edge(u, 0.04))
    save("shapes", "torus", dome_p((R - 0.6) / 0.3))
    save("shapes", "cylinder", dome_p(v / 0.5) * edge(np.abs(u) - 0.9, 0.04))
    save("shapes", "wedge", np.clip((u + 0.9) / 1.8, 0, 1) * edge(np.maximum(np.abs(u), np.abs(v)) - 0.9, 0.04))
    save("shapes", "frame", edge(np.abs(np.maximum(np.abs(u), np.abs(v)) - 0.72) - 0.15, 0.04))
    gx, gy = np.mod((u + 1) / 2 * 6, 1) - 0.5, np.mod((v + 1) / 2 * 6, 1) - 0.5
    save("shapes", "dot grid", edge(np.hypot(gx, gy) - 0.25, 0.08) * disc(u, v, 0.9, 0.05))


def paint_stroke(u, v, pts, width, seed, dry=0.0, ridge=0.35):
    s, d, L = path_coords(u, v, pts)
    t = s / L
    wd = width * (0.8 + 0.2 * np.sin(np.pi * t)) * (1 + 0.15 * (tperlin(s, s * 0, (6, 1), 2, seed=seed) - 0.5))
    rag = 0.03 * width / 0.12 * (tperlin(u, v, 24, 2, seed=seed + 1) - 0.5)
    inside = smooth((wd - np.abs(d) + rag) / 0.012)
    bristle = tnoise(s, d / width, (8, 26), 2, seed=seed + 2)
    keep = smooth((tnoise(s, d / width, (3, 40), 1, seed=seed + 3) - dry * t ** 2) / 0.08) if dry else 1.0
    return np.clip(inside * keep * (0.45 + 0.35 * bristle + ridge * np.clip(np.abs(d) / wd, 0, 1) ** 4), 0, 1)


def stylized():
    u, v = grid(512)
    D = disc(u, v)
    R = np.sqrt(u * u + v * v)
    A = np.arctan2(v, u)
    x = np.linspace(-0.75, 0.75, 40)
    save("stylized", "brush stroke", paint_stroke(u, v, np.stack([x, 0.08 * np.sin(x * 2)], 1), 0.16, 801) * D)
    a = np.linspace(-2.2, 0.6, 50)
    save("stylized", "curved brush stroke", paint_stroke(u, v, np.stack([np.cos(a) * 0.6, np.sin(a) * 0.6 + 0.15], 1), 0.12, 802) * D)
    save("stylized", "dry brush", paint_stroke(u, v, np.stack([x, -0.05 * np.sin(x * 3)], 1), 0.2, 803, dry=0.9) * D)
    save("stylized", "paint dab", paint_stroke(u, v, np.array([[-0.2, 0.0], [0.0, 0.03], [0.2, 0.0]]), 0.38, 804, ridge=0.5) * D)
    save("stylized", "paint smear", paint_stroke(u, v, np.stack([x, 0.15 * np.sin(x * 1.5 + 0.5)], 1), 0.32, 805, dry=0.5, ridge=0.15) * D)

    r = np.random.default_rng(806)
    img = np.zeros_like(u)
    for _ in range(26):
        stamp(u, v, img, *r.uniform(-0.7, 0.7, 2), r.uniform(0.08, 0.2), r.uniform(0, 6.3), chip_fn(chip_shape(r, 5, 0.3), 0.18, r.uniform(0.5, 1)))
    save("stylized", "chunky chips", img * D)
    r = np.random.default_rng(807)
    outer, inner = chip_shape(r, 6, 0.25), chip_shape(r, 5, 0.2) * 0.55 + [0.1, -0.05]
    save("stylized", "chunky chip", stamp(u, v, np.zeros_like(u), 0, 0, 0.75, 0.2,
                                          lambda lu, lv: np.maximum(np.clip(-convex_sdf(lu, lv, outer) / 0.12, 0, 1) * 0.6, np.clip(-convex_sdf(lu, lv, inner) / 0.08, 0, 1))) * D)

    r = np.random.default_rng(808)
    lines = []
    for y in np.linspace(-0.6, 0.6, 6):
        p = walk(r, r.uniform(-0.8, -0.3), y + r.normal(0, 0.05), r.normal(0, 0.1), r.uniform(0.7, 1.3), 10, 0.12)
        lines.append((p, taper(len(p), r.uniform(0.03, 0.05), p=0.8)))
        if r.random() < 0.6:
            i = r.integers(3, 8)
            q = walk(r, *p[i], r.choice([-0.6, 0.6]), 0.25, 5, 0.1)
            lines.append((q, taper(len(q), 0.02, "end", 0.8)))
    save("stylized", "stylized wood cracks", strokes(u, v, lines, vee_p) * D)
    f = (v + 1) * 6 + 0.8 * tperlin(u, v, (1, 2), 3, seed=809)
    t = np.abs(np.mod(f, 1) - 0.5)
    w = 0.14 * smooth((tperlin(u, v, (3, 6), 2, seed=810) - 0.25) / 0.4)
    save("stylized", "stylized wood grain", smooth((w - t + 0.0) / 0.02) * D)
    r = np.random.default_rng(811)
    lines = []
    for k in range(5):
        crack_lines(r, *r.uniform(-0.4, 0.4, 2), r.uniform(0, 6.3), r.uniform(0.5, 0.9), 0.05, steps=6, wiggle=0.6, branch=0.3, out=lines)
    save("stylized", "stylized rock cracks", strokes(u, v, lines, vee_p) * D)
    f1, f2, ident, ox, oy = tworley(u, v, 4, seed=812)
    plane = 0.55 + 0.25 * (cellvals(ident, 4, 813) * 2 - 1) * ox + 0.25 * (cellvals(ident, 4, 814) * 2 - 1) * oy
    save("stylized", "stylized rock face", np.clip(plane * (0.6 + 0.4 * cellvals(ident, 4, 815)) * smooth((f2 - f1) / 0.12), 0, 1) * D)
    lx, ly, bw, bh, row, col = bricks(u, v, 4, 8, 0.5)
    rot = (hash2(row, col, 816) - 0.5) * 0.15
    bx, by = lx * np.cos(rot) - ly * np.sin(rot), lx * np.sin(rot) + ly * np.cos(rot)
    save("stylized", "stylized bricks", np.clip(-rbox(bx, by, bw - 0.03, bh - 0.03, 0.03) / 0.06, 0, 1) * (0.7 + 0.3 * hash2(row, col, 817)) * D)
    f1, f2, ident, *_ = tworley(u, v, 5, seed=818, jitter=0.8)
    save("stylized", "stylized cobbles", np.clip((f2 - f1) / 0.25, 0, 1) ** 0.7 * (0.7 + 0.3 * cellvals(ident, 5, 819)) * D)

    def cloud(lu, lv, seed=0):
        rr = np.random.default_rng(seed)
        d = np.full(lu.shape, 9.0)
        for cx, rad in zip(np.linspace(-0.6, 0.6, 5), (0.3, 0.42, 0.5, 0.4, 0.28)):
            d = np.minimum(d, np.hypot(lu - cx, lv + 0.05 - rad * 0.3 + rr.normal(0, 0.03)) - rad)
        d = np.maximum(d, lv - 0.25)
        return np.sqrt(smooth(-d / 0.3))

    save("stylized", "cartoon cloud", stamp(u, v, np.zeros_like(u), 0, 0, 0.9, 0, cloud, reach=1.1) * D)
    img = np.zeros_like(u)
    for i, (cx, cy, sz) in enumerate([(-0.45, -0.35, 0.4), (0.4, -0.1, 0.45), (-0.25, 0.4, 0.38)]):
        stamp(u, v, img, cx, cy, sz, 0, lambda lu, lv, i=i: cloud(lu, lv, i + 1), reach=1.1)
    save("stylized", "cartoon clouds", img * D)
    puff = R - (0.62 + 0.12 * np.abs(np.cos(A * 2.5)))
    save("stylized", "cartoon puff", np.sqrt(smooth(-puff / 0.35)) * D)
    w = tperlin(u, v, (2, 1), 2, seed=820) - 0.5
    xs = (u + 0.25 * w) * 4
    col = np.floor(xs)
    fx = xs - col
    shift = hash2(col, 0, 821)
    fy = np.mod(v * (1 + shift) + shift * 3, 1.2)
    lens = smooth(np.minimum(fx, 1 - fx) / 0.12) * smooth(np.minimum(fy, 1.2 - fy) / 0.15)
    save("stylized", "stylized bark", lens * (0.6 + 0.4 * hash2(col, np.floor(v * (1 + shift) + shift * 3), 822)) * D)
    r = np.random.default_rng(823)
    img = np.zeros_like(u)

    def bold_leaf(lu, lv):
        t = np.clip((lu + 1) / 2, 0, 1)
        half = 0.42 * np.sin(np.pi * t) ** 0.9
        body = np.clip((half - np.abs(lv)) / 0.12, 0, 1) * (np.abs(lu) < 1)
        return body * (1 - 0.6 * smooth(1 - np.abs(lv) / 0.04) * (np.abs(lu) < 0.8))

    for a in np.linspace(0, 2 * np.pi, 7)[:-1] + r.normal(0, 0.15, 6):
        stamp(u, v, img, np.cos(a) * 0.4, np.sin(a) * 0.4, 0.42, a, bold_leaf)
    save("stylized", "stylized leaves", img * D)
    r = np.random.default_rng(824)
    lines = []
    for cx in (-0.45, 0.0, 0.45):
        for _ in range(7):
            ang = -np.pi / 2 + r.normal(0, 0.45)
            p = walk(r, cx + r.normal(0, 0.04), 0.7, ang, r.uniform(0.6, 1.0), 10, 0.0, np.sign(ang + np.pi / 2) * 0.03, 0.0)
            lines.append((p, taper(len(p), 0.06, "end", 1.0)))
    save("stylized", "stylized grass", strokes(u, v, lines, flat_p) * D)
    s = R * 3.5 - (A + np.pi) / (2 * np.pi)
    save("stylized", "stylized swirl", np.sin(np.pi * np.mod(s, 1)) ** 0.6 * smooth((0.9 - R) / 0.2) * smooth(R / 0.1) * D)
    sp = curl_path(18, 1.0, 300, 1.6, "spiral") * 0.9
    sp -= sp[0]
    lines = [(sp * [1, -1] + [-0.55, 0.3], np.linspace(0.07, 0.02, 300))]
    for k in range(3):
        xx = np.linspace(-0.85, 0.85, 50)
        lines.append((np.stack([xx, 0.45 + k * 0.14 + 0.03 * np.sin(xx * 6 + k)], 1), 0.03 - k * 0.006))
    save("stylized", "stylized wave", strokes(u, v, lines) * D)
    r = np.random.default_rng(825)
    img = np.zeros_like(u)
    for k in range(5):
        xx = -0.6 + k * 0.3
        y = np.linspace(-0.85, 0.85, 60)
        p = np.stack([xx + 0.12 * np.sin(y * 3 + k), y], 1)
        sc, dd, L = path_coords(u, v, p)
        wd = 0.13 * np.sin(np.pi * np.clip(sc / L, 0, 1)) ** 0.5
        body = np.clip((wd - np.abs(dd)) / 0.05, 0, 1)
        grooves = 0.75 + 0.25 * np.cos(dd / np.maximum(wd, 1e-3) * np.pi * 3)
        img = np.maximum(img, body * grooves * r.uniform(0.7, 1))
    save("stylized", "stylized hair strands", img * D)
    r = np.random.default_rng(826)
    lines = []
    for j in range(9):
        for i in range(9):
            x0, y0 = -0.8 + i * 0.2 + (j % 2) * 0.1 + r.normal(0, 0.02), -0.85 + j * 0.2 + r.normal(0, 0.02)
            p = walk(r, x0, y0, np.pi / 2 + r.normal(0, 0.2), 0.25, 6, 0.0, r.normal(0, 0.05), 0.0)
            lines.append((p, taper(len(p), 0.05, "end", 0.9), r.uniform(0.6, 1)))
    save("stylized", "stylized fur clumps", strokes(u, v, lines, round_p) * D)

    def sparkle(lu, lv):
        return np.clip(1 - (np.abs(lu) ** 0.5 + np.abs(lv) ** 0.5) ** 2, 0, 1) ** 0.7

    img = stamp(u, v, np.zeros_like(u), 0, 0, 0.85, 0, sparkle, reach=1.1)
    for cx, cy, sz in ((0.55, -0.5, 0.25), (-0.55, 0.5, 0.2), (0.6, 0.55, 0.15)):
        stamp(u, v, img, cx, cy, sz, 0.3, sparkle, reach=1.1)
    save("stylized", "stylized sparkle", img * D)
    r = np.random.default_rng(827)
    body = smooth((-v + 0.1) / 0.06)
    for x in np.linspace(-0.8, 0.8, 7) + r.normal(0, 0.05, 7):
        pts = chip_shape(r, 5, 0.3)
        sz = r.uniform(0.1, 0.2)
        bite = np.zeros_like(u)
        stamp(u, v, bite, x, -0.1, sz, r.uniform(0, 6.3), lambda lu, lv: np.clip(-convex_sdf(lu, lv, pts) / 0.2, 0, 1))
        body = body * (1 - 0.6 * bite)
    save("stylized", "stylized chipped edge", np.clip(body, 0, 1) * D)


def noise():
    u, v = grid(512)
    save("noise", "value noise", stretch(tnoise(u, v, 8, 5, seed=901)))
    save("noise", "fine value noise", stretch(tnoise(u, v, 32, 3, seed=902)))
    save("noise", "gradient noise", stretch(tperlin(u, v, 4, 6, seed=903)))
    save("noise", "fine gradient noise", stretch(tperlin(u, v, 16, 4, seed=904)))
    save("noise", "billow noise", stretch(sum(np.abs(tperlin(u, v, 4 * 2 ** o, 1, seed=905 + o) * 2 - 1) * 0.5 ** o for o in range(5))))
    save("noise", "ridged noise", stretch(sum(ridged(u, v, 3 * 2 ** o, 1, seed=910 + o) ** 2 * 0.5 ** o for o in range(5))))
    save("noise", "turbulence", stretch(sum(np.abs(tperlin(u, v, 2 * 2 ** o, 1, seed=915 + o) - 0.5) * 0.55 ** o for o in range(6))))
    w1, w2 = tperlin(u, v, 3, 4, seed=921) - 0.5, tperlin(u, v, 3, 4, seed=922) - 0.5
    save("noise", "warped noise", stretch(tperlin(u + w1 * 0.8, v + w2 * 0.8, 4, 5, seed=923)))
    save("noise", "warped ridges", stretch(ridged(u + w1 * 0.6, v + w2 * 0.6, 4, 4, seed=924) ** 2))
    f1, f2, ident, *_ = tworley(u, v, 8, seed=925)
    save("noise", "cellular", stretch(f1))
    save("noise", "cell edges", smooth(1 - (f2 - f1) / 0.08))
    save("noise", "cell plateaus", cellvals(ident, 8, 926) * smooth((f2 - f1) / 0.04))
    save("noise", "cell domes", np.sqrt(smooth((f2 - f1) / 0.6)))
    f1, f2, *_ = tworley(u, v, 24, seed=927)
    save("noise", "fine cellular", stretch(1 - f1))
    f1, f2, *_ = tworley(u + w1 * 0.1, v + w2 * 0.1, 12, seed=928)
    save("noise", "crackle", np.clip(smooth((f2 - f1) / 0.1) * (0.7 + 0.3 * tperlin(u, v, 16, 3, seed=929)), 0, 1))
    save("noise", "horizontal streaks", stretch(tperlin(u, v, (2, 32), 4, seed=930)))
    save("noise", "fibres", stretch(tnoise(u + w1 * 0.3, v + w2 * 0.3, (64, 4), 3, seed=931)) ** 1.5)
    r = np.random.default_rng(932)
    save("noise", "soft grain", stretch(blur(r.random(u.shape), 1)))
    sp = (r.random(u.shape) > 0.985).astype(float)
    save("noise", "speckle", np.clip(blur(sp, 1) * 6, 0, 1))
    f1, _, ident, *_ = tworley(u, v, 16, seed=933, jitter=0.5)
    save("noise", "scattered dots", smooth(1 - f1 / (0.15 + 0.15 * cellvals(ident, 16, 934))) * (cellvals(ident, 16, 935) > 0.3))
    turb = sum(np.abs(tperlin(u, v, 2 * 2 ** o, 1, seed=936 + o) - 0.5) * 0.5 ** o for o in range(5))
    save("noise", "marble", 0.5 + 0.5 * np.sin((u + v) * np.pi * 3 + turb * 8))
    n = stretch(tperlin(u, v, 3, 5, seed=941))
    save("noise", "terraced noise", np.floor(n * 7) / 6 * 0.85 + 0.15 * smooth(np.mod(n * 7, 1) / 0.3))
    save("noise", "clouds", smooth(stretch(tperlin(u, v, 2, 7, seed=942, gain=0.6), 5, 95)))


def grain(u, v, cells, seed, lo=0.35, soft=0.25):
    """Paper-tooth grain: 1 on the peaks a dry medium catches, 0 in the pits."""
    return smooth((tnoise(u, v, cells, 3, seed=seed) - lo) / soft)


def dots(u, v, xs, ys, rs, vals=None, soft=0.35):
    return bumps(u, v, xs, ys, rs, profile=lambda t: t, soft=soft, heights=vals)


def brush_tips():
    """Tips for texture painting presets. With followStroke the stroke travels along u, so bristles sit along v."""
    n = 256
    u, v = grid(n)
    R = np.sqrt(u * u + v * v)
    D = disc(u, v, 0.92, 0.06)

    r = np.random.default_rng(1001)
    k = 70
    rad, ang = np.sqrt(r.random(k)) * 0.8, r.uniform(0, 2 * np.pi, k)
    save("brush tips", "bristle round", dots(u, v, rad * np.cos(ang), rad * np.sin(ang), r.uniform(0.04, 0.09, k), r.uniform(0.45, 1, k)))
    r = np.random.default_rng(1002)
    k = 44
    ys = np.linspace(-0.85, 0.85, k) + r.normal(0, 0.01, k)
    save("brush tips", "bristle flat", dots(u, v, r.normal(0, 0.05, k), ys, r.uniform(0.035, 0.06, k), r.uniform(0.5, 1, k)))
    r = np.random.default_rng(1003)
    k = 14
    ys = np.sort(r.uniform(-0.85, 0.85, k))
    save("brush tips", "bristle sparse", dots(u, v, r.normal(0, 0.04, k), ys, r.uniform(0.03, 0.07, k), r.uniform(0.4, 1, k)))
    r = np.random.default_rng(1004)
    k = 36
    a = np.linspace(-1.0, 1.0, k) + r.normal(0, 0.02, k)
    save("brush tips", "bristle fan", dots(u, v, np.cos(a) * 0.8 - 0.5, np.sin(a) * 0.8, r.uniform(0.03, 0.055, k), r.uniform(0.5, 1, k)))
    r = np.random.default_rng(1005)
    lines = [(np.array([[r.uniform(-0.6, -0.2), y], [r.uniform(0.2, 0.6), y + r.normal(0, 0.03)]]), r.uniform(0.012, 0.03), r.uniform(0.5, 1))
             for y in r.uniform(-0.8, 0.8, 22)]
    save("brush tips", "hairy streaks", strokes(u, v, lines, round_p) * D)

    f1, f2, *_ = tworley(u, v, 9, seed=1006)
    save("brush tips", "sponge dense", smooth((f1 - 0.28) / 0.1) * disc(u, v, 0.85 + 0.08 * (tnoise(u, v, 6, 2, seed=1007) - 0.5), 0.1))
    f1, f2, *_ = tworley(u, v, 5, seed=1008)
    save("brush tips", "sponge coarse", smooth((f1 - 0.33) / 0.12) * smooth((f2 - f1) / 0.05) * disc(u, v, 0.8 + 0.15 * (tnoise(u, v, 4, 2, seed=1009) - 0.5), 0.1))
    rg = np.random.default_rng(1010)
    save("brush tips", "powder puff", np.exp(-(R / 0.5) ** 2) * np.clip(blur(rg.random(u.shape), 1) * 2.6 - 0.9, 0, 1))
    save("brush tips", "chalk grain", disc(u, v, 0.82 + 0.06 * (tnoise(u, v, 12, 2, seed=1011) - 0.5), 0.05) * grain(u, v, 48, 1012))
    rough = disc(u * 0.7, v * 1.3, 0.75 + 0.15 * (tnoise(u, v, 10, 3, seed=1013) - 0.5), 0.06)
    save("brush tips", "charcoal grain", rough * smooth((tnoise(u, v, (16, 64), 3, seed=1014) - 0.4) / 0.2))
    save("brush tips", "pencil grain", disc(u, v, 0.8, 0.08) * (0.35 + 0.65 * grain(u, v, 64, 1015, 0.3, 0.3)))
    save("brush tips", "pastel block", edge(rbox(u, v, 0.55, 0.85, 0.15), 0.06) * grain(u, v, 40, 1016, 0.3, 0.2))
    hatch = lambda x, count, width: smooth(1 - np.abs(np.mod(x * count / 2, 1) - 0.5) / width)
    save("brush tips", "hatch lines", hatch((u + v) / np.sqrt(2), 7, 0.14) * D)
    save("brush tips", "cross hatch", np.maximum(hatch((u + v) / np.sqrt(2), 7, 0.12), hatch((u - v) / np.sqrt(2), 7, 0.12)) * D)
    save("brush tips", "dense hatch", hatch(u * 0.95 + v * 0.3, 16, 0.18) * (0.7 + 0.3 * tnoise(u, v, 8, 2, seed=1017)) * D)

    w1, w2 = tperlin(u, v, 2, 3, seed=1018) - 0.5, tperlin(u, v, 2, 3, seed=1019) - 0.5
    wisp = stretch(ridged(u + w1 * 1.2, v + w2 * 1.2, 3, 4, seed=1020) ** 3)
    save("brush tips", "smoke wisp", wisp * np.exp(-(R / 0.6) ** 2))
    r = np.random.default_rng(1021)
    k = 260
    ang, rad = r.uniform(0, 2 * np.pi, k), np.abs(r.normal(0, 0.38, k))
    save("brush tips", "spray speckle", dots(u, v, rad * np.cos(ang), rad * np.sin(ang), r.uniform(0.01, 0.03, k), r.uniform(0.5, 1, k), 0.5) * D)
    r = np.random.default_rng(1022)
    k = 600
    ang, rad = r.uniform(0, 2 * np.pi, k), np.sqrt(r.random(k)) * 0.85
    save("brush tips", "fine spray", dots(u, v, rad * np.cos(ang), rad * np.sin(ang), r.uniform(0.006, 0.014, k), r.uniform(0.3, 1, k), 0.6))

    def splat(seed, blob, spikes, drops, spread):
        rr = np.random.default_rng(seed)
        a = np.arctan2(v, u)
        body = blob * (1 + 0.25 * (tnoise(np.cos(a), np.sin(a), 5, 2, seed=seed + 1) - 0.5))
        img = edge(R - body, 0.03)
        lines = []
        for ang in rr.uniform(0, 2 * np.pi, spikes):
            ln = rr.uniform(0.15, spread)
            lines.append((np.array([[np.cos(ang) * blob * 0.8, np.sin(ang) * blob * 0.8], [np.cos(ang) * (blob + ln), np.sin(ang) * (blob + ln)]]),
                          np.array([rr.uniform(0.03, 0.07), 0.008])))
        img = strokes(u, v, lines, flat_p, img)
        ang, dist = rr.uniform(0, 2 * np.pi, drops), rr.uniform(blob + 0.05, blob + spread, drops)
        sp = dots(u, v, np.cos(ang) * dist, np.sin(ang) * dist, rr.uniform(0.015, 0.05, drops), soft=0.15)
        return np.clip(np.maximum(img, sp), 0, 1) * disc(u, v, 0.95, 0.04)

    save("brush tips", "ink splat", splat(1023, 0.32, 14, 18, 0.55))
    save("brush tips", "splatter cluster", splat(1025, 0.18, 5, 40, 0.65))
    save("brush tips", "paint drop", np.maximum(edge(R - 0.42, 0.04), strokes(u, v, [(np.array([[0.0, 0.3], [0.05, 0.85]]), np.array([0.12, 0.05]))], flat_p)))
    r = np.random.default_rng(1027)
    k = 9
    save("brush tips", "round spots", dots(u, v, r.uniform(-0.65, 0.65, k), r.uniform(-0.65, 0.65, k), r.uniform(0.06, 0.2, k), r.uniform(0.6, 1, k), 0.12))

    mottle = 0.55 + 0.25 * tperlin(u, v, 5, 3, seed=1028)
    body = 0.82 + 0.08 * (tnoise(np.cos(np.arctan2(v, u)), np.sin(np.arctan2(v, u)), 6, 3, seed=1029) - 0.5)
    rim = smooth(1 - np.abs(R - body + 0.04) / 0.06)
    save("brush tips", "watercolour bloom", np.clip(edge(R - body, 0.03) * mottle + rim * 0.45, 0, 1))
    save("brush tips", "wet edge", np.clip(edge(R - 0.85, 0.03) * 0.35 + smooth(1 - np.abs(R - 0.8) / 0.08) * 0.65, 0, 1))
    save("brush tips", "felt tip", edge(rbox(u, v, 0.7, 0.7, 0.35), 0.05) * (0.9 + 0.1 * tnoise(u, v, 24, 2, seed=1030)))
    save("brush tips", "chisel marker", edge(rbox(u, v, 0.22, 0.88, 0.08), 0.04))
    knife = edge(rbox(u, v, 0.3, 0.88, 0.05), 0.03)
    ridge = smooth(1 - np.abs(np.abs(v) - 0.84) / 0.05) * 0.6
    save("brush tips", "palette knife", np.clip(knife * (0.55 + 0.25 * tperlin(u * 4, v, (2, 6), 2, seed=1031)) + ridge * (np.abs(u) < 0.3), 0, 1))

    r = np.random.default_rng(1032)
    lines = []
    for _ in range(60):
        x0 = r.uniform(-0.6, 0.6)
        p = walk(r, x0, 0.85, -np.pi / 2 + (x0 * 0.5) + r.normal(0, 0.12), r.uniform(0.9, 1.6), 12, 0.08)
        lines.append((p, taper(len(p), r.uniform(0.012, 0.025), "end", 0.7), r.uniform(0.5, 1)))
    save("brush tips", "fur strands", strokes(u, v, lines, round_p) * D)
    lines = [(np.array([[-0.8, 0.0], [0.85, 0.0]]), np.array([0.04, 0.012]), 1.0)]
    r = np.random.default_rng(1033)
    for x in np.linspace(-0.7, 0.7, 26):
        for side in (-1, 1):
            ln = 0.55 * np.sin(np.pi * (x + 0.8) / 1.65) + 0.05
            lines.append((np.array([[x, 0.0], [x + 0.35 * ln, side * ln]]), np.array([0.014, 0.006]), r.uniform(0.55, 0.9)))
    save("brush tips", "feather barbs", strokes(u, v, lines, round_p) * D)
    r = np.random.default_rng(1034)
    lines = []
    for _ in range(45):
        p = walk(r, *r.uniform(-0.6, 0.6, 2), r.uniform(0, 6.3), r.uniform(0.25, 0.5), 10, 0.5, curl=r.choice([-0.3, 0.3]), pull=0.05)
        lines.append((p, taper(len(p), 0.016, p=0.5), r.uniform(0.5, 1)))
    save("brush tips", "moss fibres", strokes(u, v, lines, round_p) * D)
    r = np.random.default_rng(1035)
    lines = []
    for _ in range(30):
        a0 = r.choice([0.6, -0.6]) + r.normal(0, 0.15)
        x0, y0 = r.uniform(-0.6, 0.6, 2)
        p = walk(r, x0 - np.cos(a0) * 0.6, y0 - np.sin(a0) * 0.6, a0, 1.2, 14, 0.06)
        lines.append((p, 0.012, r.uniform(0.5, 1)))
    save("brush tips", "interlaced fibres", strokes(u, v, lines, round_p) * D)
    r = np.random.default_rng(1036)
    x = np.linspace(-0.9, 0.9, 40)
    lines = [(np.stack([x, y + 0.04 * np.sin(x * r.uniform(3, 6) + r.uniform(0, 6))], 1), r.uniform(0.008, 0.016), r.uniform(0.5, 1))
             for y in np.linspace(-0.7, 0.7, 11) + r.normal(0, 0.02, 11)]
    save("brush tips", "hair lines", strokes(u, v, lines, round_p) * D)

    warp = 0.06 * (tperlin(u, v, 3, 2, seed=1037) - 0.5)
    ell = np.sqrt((u / 0.8) ** 2 + (v / 1.0) ** 2) + warp
    breaks = smooth((tnoise(u, v, 14, 2, seed=1038) - 0.2) / 0.1)
    save("brush tips", "whorl print", smooth(1 - np.abs(np.mod(ell * 11, 1) - 0.5) / 0.22) * breaks * disc(u, v, 0.85, 0.15))
    loop = np.where(v < 0.1, np.abs(u) + warp, np.sqrt(u * u + (v - 0.1) ** 2) + warp)
    save("brush tips", "loop print", smooth(1 - np.abs(np.mod(loop * 12, 1) - 0.5) / 0.22) * breaks * disc(u, v, 0.85, 0.15))

    save("brush tips", "running stitch", edge(rbox(u, v, 0.55, 0.07, 0.07), 0.04) * (0.75 + 0.25 * dome_p(v / 0.07)))
    save("brush tips", "cross stitch mark", strokes(u, v, [(np.array([[-0.5, -0.5], [0.5, 0.5]]), 0.08), (np.array([[-0.5, 0.5], [0.5, -0.5]]), 0.08)], dome_p))
    staple = strokes(u, v, [(np.array([[-0.6, 0.2], [-0.6, -0.15], [0.6, -0.15], [0.6, 0.2]]), 0.07)], dome_p)
    save("brush tips", "staple", staple)
    save("brush tips", "zigzag segment", strokes(u, v, [(np.array([[-0.9, 0.0], [-0.45, 0.45], [0.0, 0.0], [0.45, -0.45], [0.9, 0.0]]), 0.06)], dome_p))
    save("brush tips", "hazard band", edge(np.abs(u - v * 0.8) - 0.22, 0.03) * edge(np.abs(v) - 0.6, 0.03))
    save("brush tips", "stripe bar", edge(np.abs(u) - 0.12, 0.03) * edge(np.abs(v) - 0.85, 0.03))
    chev = edge(np.abs(u - np.abs(v) * 0.7 + 0.2) - 0.16, 0.04) * edge(np.abs(v) - 0.8, 0.04)
    save("brush tips", "tread chevron", chev * (0.8 + 0.2 * dome_p((u - np.abs(v) * 0.7 + 0.2) / 0.16)))
    pads = np.maximum(edge(rbox(u + 0.25, v - 0.45, 0.2, 0.3, 0.05), 0.03), edge(rbox(u - 0.25, v + 0.45, 0.2, 0.3, 0.05), 0.03))
    save("brush tips", "track cleats", np.maximum(pads, edge(rbox(u, v, 0.08, 0.85, 0.03), 0.03) * 0.6))
    arc = np.linspace(np.pi * 1.05, np.pi * 1.95, 24)
    plate = np.concatenate([np.stack([np.cos(arc) * 0.8, np.sin(arc) * 0.8 + 0.05], 1), [[0.55, 0.45], [0.0, 0.88], [-0.55, 0.45]]])
    sd = convex_sdf(u, v, plate)
    save("brush tips", "scale plate", np.clip(-sd / 0.12, 0, 1) ** 0.6 * (0.7 + 0.3 * smooth((0.4 - v) / 0.8)))
    hx = lambda lu, lv: smooth(1 - np.abs(ngon(lu, lv, 6, 0.8) + 0.07) / 0.07)
    save("brush tips", "hex cell", stamp(u, v, np.zeros_like(u), 0, 0, 1.0, np.pi / 6, hx) + 0.35 * edge(ngon(u, v, 6, 0.6, np.pi / 6), 0.04))

    r = np.random.default_rng(1039)
    blob = disc(u, v, 0.55 + 0.25 * (tnoise(u, v, 4, 3, seed=1040) - 0.5), 0.08)
    holes = smooth((tworley(u, v, 7, seed=1041)[0] - 0.1) / 0.08)
    save("brush tips", "grit clump", blob * holes * (0.6 + 0.4 * grain(u, v, 32, 1042, 0.3, 0.3)))
    body = disc(u, v, 0.45 + 0.15 * (tnoise(u, v, 5, 2, seed=1043) - 0.5), 0.05)
    drips = strokes(u, v, [(np.array([[x, 0.0], [x + r.normal(0, 0.02), r.uniform(0.55, 0.9)]]), np.array([r.uniform(0.05, 0.09), 0.04]))
                            for x in r.uniform(-0.35, 0.35, 4)], flat_p)
    save("brush tips", "mud splash", np.maximum(body, drips) * (0.75 + 0.25 * tnoise(u, v, 16, 2, seed=1044)))
    r = np.random.default_rng(1045)
    k = 16
    xs, ys, rs = r.uniform(-0.6, 0.6, k), r.uniform(-0.6, 0.6, k), r.uniform(0.07, 0.22, k)
    fuzz = 0.6 + 0.4 * tnoise(u, v, 40, 2, seed=1046)
    save("brush tips", "mold colony", bumps(u, v, xs, ys, rs, profile=lambda t: t ** 0.5, soft=0.6) * fuzz * D)
    rg = np.random.default_rng(1047)
    save("brush tips", "sandpaper grain", (rg.random(u.shape) > 0.8).astype(float) * (0.5 + 0.5 * rg.random(u.shape)) * D)
    r = np.random.default_rng(1048)
    k = 80
    save("brush tips", "cement speckle", np.clip(dots(u, v, r.uniform(-0.85, 0.85, k), r.uniform(-0.85, 0.85, k), r.uniform(0.01, 0.05, k),
                                                     r.uniform(0.3, 1, k), 0.4) + 0.25 * grain(u, v, 48, 1049), 0, 1) * D)
    r = np.random.default_rng(1050)
    k = 60
    save("brush tips", "dust motes", dots(u, v, r.uniform(-0.8, 0.8, k), r.uniform(-0.8, 0.8, k), r.uniform(0.03, 0.09, k), r.uniform(0.5, 1, k), 0.6) * D)

    save("brush tips", "leaf sprig", stamp(u, v, np.zeros_like(u), 0, 0, 0.8, 0.0, lambda lu, lv: leaf(lu, lv, 0.35, 0.7, 7)))
    save("brush tips", "grass tuft", strokes(u, v, [(walk(np.random.default_rng(1051 + i), x, 0.85, -np.pi / 2 + x * 0.6, 1.4, 10, 0.05, curl=x * 0.02),
                                                     taper(11, 0.05, "end", 0.9)) for i, x in enumerate(np.linspace(-0.4, 0.4, 7))], round_p) * D)
    save("brush tips", "ink nib", edge(np.sqrt((u / 0.25) ** 2 + (v / 0.9) ** 2) - 1 + 0.06 * (tnoise(u, v, 20, 2, seed=1052) - 0.5), 0.08))


NEW_GENERATORS = (damage_more, stone_more, organic_more, skin_more, fabric_more, hard_more, ornament, shapes_more, stylized, noise, brush_tips)


if __name__ == "__main__":
    import sys
    ONLY.update(sys.argv[1:])
    for gen in (shapes, skin, organic, stone, fabric, hard_surface, damage) + NEW_GENERATORS:
        gen()
    files = sorted(OUT.rglob("*.png"))
    print(len(files), "alphas,", sum(f.stat().st_size for f in files) // 1024, "KB")
