"""Writes the built-in lens dirt images into resources/lensdirt: dust, smudge, streaks and rain, all procedural.

Run from the repo root: python scripts/make_lens_dirt.py. Deterministic (fixed seeds), so reruns give the same files.
"""
from pathlib import Path

import numpy as np
from PIL import Image

W, H = 1280, 720
OUT = Path(__file__).resolve().parent.parent / "resources" / "lensdirt"


def grid():
    y, x = np.mgrid[0:H, 0:W].astype(np.float32)
    return x, y


def blobs(rng, count, rmin, rmax, soft, ring=0.0):
    x, y = grid()
    img = np.zeros((H, W), np.float32)
    for _ in range(count):
        cx, cy = rng.uniform(0, W), rng.uniform(0, H)
        r = rng.uniform(rmin, rmax)
        a = rng.uniform(0.15, 1.0)
        d = np.sqrt((x - cx) ** 2 + (y - cy) ** 2) / r
        disc = np.clip((1.0 - d) / soft, 0.0, 1.0)
        if ring > 0.0:
            disc = disc * (0.5 + ring * np.clip(d, 0, 1) ** 4)
        img += a * disc
    return img


def smear(rng, count, length, width):
    x, y = grid()
    img = np.zeros((H, W), np.float32)
    for _ in range(count):
        cx, cy = rng.uniform(0, W), rng.uniform(0, H)
        ang = rng.uniform(0, np.pi)
        dx, dy = np.cos(ang), np.sin(ang)
        u = (x - cx) * dx + (y - cy) * dy
        v = -(x - cx) * dy + (y - cy) * dx
        l = rng.uniform(0.4, 1.0) * length
        img += rng.uniform(0.2, 0.8) * np.exp(-(u / l) ** 2 - (v / (width * rng.uniform(0.5, 1.5))) ** 2)
    return img


def vignette():
    x, y = grid()
    d = np.sqrt(((x - W / 2) / (W / 2)) ** 2 + ((y - H / 2) / (H / 2)) ** 2)
    return 0.35 + 0.65 * np.clip(d, 0, 1)


def save(name, img):
    img = np.clip(img / max(np.percentile(img, 99.7), 1e-6), 0.0, 1.0)
    Image.fromarray((img * 255.0 + 0.5).astype(np.uint8), "L").convert("RGB").save(OUT / f"{name}.png", optimize=True)


def main():
    OUT.mkdir(parents=True, exist_ok=True)
    rng = np.random.default_rng(11)
    save("dust", (blobs(rng, 260, 2, 9, 0.6) + 0.6 * blobs(rng, 40, 10, 30, 0.9, ring=1.5)) * vignette())
    rng = np.random.default_rng(23)
    save("smudge", (smear(rng, 26, 260, 70) + 0.5 * blobs(rng, 60, 6, 18, 0.8)) * vignette())
    rng = np.random.default_rng(37)
    x, y = grid()
    streaks = smear(rng, 40, 520, 6) + 0.3 * smear(rng, 14, 300, 40)
    save("streaks", streaks * vignette())
    rng = np.random.default_rng(41)
    save("rain", blobs(rng, 140, 6, 26, 0.35, ring=2.5) + 0.4 * blobs(rng, 400, 1, 4, 0.5))


if __name__ == "__main__":
    main()
