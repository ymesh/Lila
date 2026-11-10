"""Renders Lila's built-in sculpt matcaps into resources/matcaps: lit spheres, 512 px, sRGB PNG."""
import pathlib

import numpy as np
from PIL import Image

SIZE = 512
OUT = pathlib.Path(__file__).resolve().parent.parent / "resources" / "matcaps"


def to_srgb(c):
    c = np.clip(c, 0.0, 1.0)
    return np.where(c <= 0.0031308, c * 12.92, 1.055 * np.power(c, 1.0 / 2.4) - 0.055)


def render(base, key=(-0.55, 0.6, 0.58), key_power=1.0, fill=0.18, rim=0.35, rim_color=None, spec=0.25, gloss=40.0, wrap=0.0,
           ambient=0.06, scatter=None, sheen=0.0, sheen_gloss=6.0, sheen_color=None, edge_dark=0.0, bounce=None):
    base = np.array(base)
    rim_color = np.array(rim_color if rim_color is not None else base * 1.2 + 0.1)
    y, x = np.mgrid[0:SIZE, 0:SIZE].astype(np.float64)
    nx = (x + 0.5) / SIZE * 2.0 - 1.0
    ny = 1.0 - (y + 0.5) / SIZE * 2.0
    r2 = nx * nx + ny * ny
    inside = r2 <= 1.0
    nz = np.sqrt(np.clip(1.0 - r2, 0.0, 1.0))
    n = np.stack([nx, ny, nz], -1)
    k = np.array(key) / np.linalg.norm(key)
    ndl = (n @ k + wrap) / (1.0 + wrap)
    diff = np.clip(ndl, 0.0, 1.0)[..., None] * key_power
    f = np.array([0.6, -0.3, 0.6])
    f /= np.linalg.norm(f)
    fill_term = np.clip(n @ f, 0.0, 1.0)[..., None] * fill
    hemi = (ny * 0.5 + 0.5)[..., None]
    amb = ambient * (0.6 + 0.4 * hemi)
    c = base * (diff + fill_term + amb)
    if scatter is not None:
        # Light bleeding into the shadow edge, warm: wax and skin.
        edge = np.clip(1.0 - np.abs(n @ k) * 2.5, 0.0, 1.0)[..., None]
        c += np.array(scatter) * edge * 0.35
    h = k + np.array([0.0, 0.0, 1.0])
    h /= np.linalg.norm(h)
    c += spec * np.power(np.clip(n @ h, 0.0, 1.0), gloss)[..., None]
    if sheen > 0.0:
        # A broad soft highlight under the hot spot: the waxy glow.
        sc = np.array(sheen_color if sheen_color is not None else (1.0, 1.0, 1.0))
        c += sc * sheen * np.power(np.clip(n @ h, 0.0, 1.0), sheen_gloss)[..., None]
    if bounce is not None:
        b = np.array([0.55, -0.55, 0.45])
        b /= np.linalg.norm(b)
        c += np.array(bounce) * np.power(np.clip(n @ b, 0.0, 1.0), 2.0)[..., None]
    fres = np.power(1.0 - nz, 3.0)[..., None]
    c += rim_color * fres * rim
    if edge_dark > 0.0:
        c *= 1.0 - edge_dark * np.power(1.0 - nz, 1.5)[..., None]
    img = to_srgb(c)
    img[~inside] = to_srgb(base * ambient)
    return Image.fromarray((img * 255.0 + 0.5).astype(np.uint8), "RGB")


MATCAPS = {
    "clay": dict(base=(0.36, 0.33, 0.30), spec=0.12, gloss=20.0, rim=0.25, wrap=0.15),
    "redWax": dict(base=(0.50, 0.065, 0.028), key=(-0.5, 0.55, 0.67), key_power=1.0, wrap=0.12, fill=0.04, ambient=0.035, rim=0.0,
                   spec=0.32, gloss=45.0, sheen=0.14, sheen_gloss=12.0, sheen_color=(1.0, 0.5, 0.32), bounce=(0.07, 0.012, 0.004),
                   edge_dark=0.6),
    "skin": dict(base=(0.62, 0.40, 0.33), spec=0.18, gloss=30.0, rim=0.3, rim_color=(1.0, 0.8, 0.75), scatter=(0.7, 0.25, 0.15), wrap=0.3),
    "chalk": dict(base=(0.72, 0.72, 0.70), spec=0.04, gloss=8.0, rim=0.15, wrap=0.1),
    "gloss": dict(base=(0.20, 0.20, 0.22), spec=0.9, gloss=120.0, rim=0.6, rim_color=(0.8, 0.85, 0.95), ambient=0.1),
    "cavity": dict(base=(0.50, 0.50, 0.50), key=(0.0, 0.2, 1.0), key_power=0.9, fill=0.0, rim=0.0, spec=0.0, ambient=0.02),
}

if __name__ == "__main__":
    OUT.mkdir(parents=True, exist_ok=True)
    for name, params in MATCAPS.items():
        render(**params).save(OUT / f"{name}.png")
        print(OUT / f"{name}.png")
