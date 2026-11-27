"""Builds resources/names/brands.txt (the real-name blocklist) from the OpenStreetMap name-suggestion-index.

Fetch the index first (about 12 MB, BSD-3-Clause):
    curl -L -o nsi.min.json https://cdn.jsdelivr.net/npm/name-suggestion-index@6/dist/nsi.min.json
then:
    python scripts/nsi_blocklist.py nsi.min.json resources/names/brands.txt
"""
import json
import sys

KEYS = ("brand", "name", "operator", "brand:en", "brand:es", "name:en", "name:es", "operator:en", "operator:es", "official_name")


def main(src, dst):
    with open(src, encoding="utf-8") as f:
        nsi = json.load(f)
    version = nsi.get("_meta", {}).get("version", "?")
    names = {}
    for path, cat in nsi["nsi"].items():
        if not (path.startswith("brands/") or path.startswith("operators/")):
            continue
        for item in cat.get("items", []):
            tags = item.get("tags", {})
            for v in [item.get("displayName")] + [tags.get(k) for k in KEYS]:
                if not v:
                    continue
                v = " ".join(v.split())
                if len(v) < 2:
                    continue
                names.setdefault(v.casefold(), v)
    with open(dst, "w", encoding="utf-8", newline="\n") as f:
        f.write(f"# Real brand and operator names from the OpenStreetMap name-suggestion-index {version} (BSD-3-Clause, resources/licenses/name-suggestion-index.txt).\n")
        f.write("# Regenerate with scripts/nsi_blocklist.py. One name per line.\n")
        for k in sorted(names):
            f.write(names[k] + "\n")
    print(len(names), "names")


if __name__ == "__main__":
    main(sys.argv[1], sys.argv[2])
