"""Writes Lila's built-in procedural materials (resources/materials) and the Meadowlight and Paintbox extension packs as
.lilamat texture graphs, plus Meadowlight's smart masks (.lilasmart). Those files are what ships; this script is how they are authored.

    python tools/materials/make_materials.py [--only substring]
"""

import argparse
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))

import common  # noqa: E402
from matgraph import ROOT, write  # noqa: E402

PACKS = [
    ("resources/materials", ["builtin_metals", "builtin_woods", "builtin_stone", "builtin_ground", "builtin_fabrics", "builtin_misc", "builtin_layers"]),
    ("extensions/Meadowlight/resources/materials", ["meadowlight"]),
    ("extensions/Paintbox/resources/materials", ["paintbox"]),
]


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--only", default="")
    args = ap.parse_args()
    written = []
    for folder, modules in PACKS:
        for mod in modules:
            if not (Path(__file__).parent / f"{mod}.py").exists():
                continue
            common.REGISTRY.clear()
            __import__(mod)
            for stem, build in common.REGISTRY:
                if args.only and args.only.lower() not in stem.lower():
                    continue
                written.append(write(build(), ROOT / folder, stem))
    if not args.only:
        import meadowlight_masks

        written += meadowlight_masks.write_all()
    for p in written:
        print(p.relative_to(ROOT).as_posix())
    print(len(written), "files")


if __name__ == "__main__":
    main()
