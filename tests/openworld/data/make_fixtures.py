"""Writes the World Remix test fixtures: two small synthetic places (ESRI ASCII elevation in degrees and Overpass JSON).

coast: sea to the west, land rising east, an east-west ridge with a primary road on its crest, a river in the valley to
the north draining to the sea, coastline, a north-south secondary road, a town area and a cafe.
inland: rolling hills with a trunk road east-west and a small grid town.
Run from this folder: python make_fixtures.py
"""
import json
import math

def asc(path, w, h, x0, y0, d, f):
    with open(path, "w", newline="\n") as o:
        o.write(f"ncols {w}\nnrows {h}\nxllcorner {x0 - d / 2:.6f}\nyllcorner {y0 - d / 2:.6f}\ncellsize {d}\nNODATA_value -9999\n")
        for r in range(h):
            row = []
            for c in range(w):
                u, v = c / (w - 1), 1 - r / (h - 1)  # u east, v north, 0..1
                row.append(f"{f(u, v):.1f}")
            o.write(" ".join(row) + "\n")

def coast_height(u, v):
    if u < 0.3:
        return -20.0 - (0.3 - u) * 100.0
    land = (u - 0.3) / 0.7
    base = 5.0 + land * 300.0
    ridge = 180.0 * math.exp(-((v - 0.5) / 0.08) ** 2)
    valley = -60.0 * math.exp(-((v - 0.75) / 0.05) ** 2)
    return base + ridge * min(1.0, land * 4) + valley * min(1.0, land * 3)

def inland_height(u, v):
    return 200.0 + 60.0 * math.sin(u * 6.0) * math.cos(v * 5.0) + 40.0 * u

class Osm:
    def __init__(self, x0, y0, span):
        self.x0, self.y0, self.span, self.els, self.next = x0, y0, span, [], 1000

    def ll(self, u, v):
        return self.y0 + v * self.span, self.x0 + u * self.span

    def node(self, u, v, tags=None):
        self.next += 1
        lat, lon = self.ll(u, v)
        e = {"type": "node", "id": self.next, "lat": round(lat, 7), "lon": round(lon, 7)}
        if tags:
            e["tags"] = tags
        self.els.append(e)
        return self.next

    def way(self, pts, tags, closed=False):
        ids = [self.node(u, v) for u, v in pts]
        if closed:
            ids.append(ids[0])
        self.next += 1
        self.els.append({"type": "way", "id": self.next, "nodes": ids, "tags": tags})
        return self.next

    def write(self, path):
        with open(path, "w", newline="\n") as o:
            json.dump({"version": 0.6, "elements": self.els}, o, indent=0)

D = 0.0005
N = 121
X0, Y0 = -107.0, 24.0
asc("coast.asc", N, N, X0, Y0, D, coast_height)
o = Osm(X0, Y0, D * (N - 1))
o.way([(0.31 + 0.69 * k / 40, 0.5) for k in range(41)], {"highway": "primary", "name": "Ridge Road"})
o.way([(1.0 - 0.75 * k / 40, 0.75) for k in range(41)], {"waterway": "river", "name": "Rio Uno"})
o.way([(0.3, 1.0 - k / 20) for k in range(21)], {"natural": "coastline"})
o.way([(0.7, 0.05 + 0.4 * k / 10) for k in range(11)] + [(0.7, 0.55 + 0.15 * k / 5) for k in range(6)], {"highway": "secondary"})
o.way([(0.55, 0.2), (0.65, 0.2), (0.65, 0.3), (0.55, 0.3)], {"landuse": "residential"}, closed=True)
o.node(0.6, 0.25, {"amenity": "cafe", "name": "Cafe Real"})
o.write("coast.json")

N2 = 81
X1, Y1 = -107.4, 25.0
asc("inland.asc", N2, N2, X1, Y1, D, inland_height)
o = Osm(X1, Y1, D * (N2 - 1))
o.way([(0.02 + 0.96 * k / 30, 0.5) for k in range(31)], {"highway": "trunk", "name": "Highway"})
for k in range(4):
    o.way([(0.35 + 0.1 * k, 0.3), (0.35 + 0.1 * k, 0.7)], {"highway": "residential"})
    o.way([(0.3, 0.35 + 0.1 * k), (0.7, 0.35 + 0.1 * k)], {"highway": "residential"})
o.way([(0.3, 0.3), (0.7, 0.3), (0.7, 0.7), (0.3, 0.7)], {"landuse": "residential"}, closed=True)
o.node(0.5, 0.5, {"amenity": "townhall", "name": "Palacio"})
o.write("inland.json")

# harbour: W1 kinds over the coast's area: a courtyard building (multipolygon) with a restaurant in it, a domed tower
# (building with a building:part), a park, a pier, a quay, a dam across the river and a large farmland crossing the
# region the tests cut.
o = Osm(X0, Y0, D * (N - 1))
outer = o.way([(0.55, 0.30), (0.62, 0.30), (0.62, 0.37), (0.55, 0.37)], {}, closed=True)
inner = o.way([(0.57, 0.32), (0.60, 0.32), (0.60, 0.35), (0.57, 0.35)], {}, closed=True)
o.next += 1
o.els.append({"type": "relation", "id": o.next, "members": [{"type": "way", "ref": outer, "role": "outer"}, {"type": "way", "ref": inner, "role": "inner"}],
              "tags": {"type": "multipolygon", "building": "civic", "name": "Palacio Real"}})
o.node(0.56, 0.31, {"amenity": "restaurant", "name": "Fonda Real"})
o.way([(0.64, 0.30), (0.68, 0.30), (0.68, 0.34), (0.64, 0.34)], {"building": "church", "name": "Templo"}, closed=True)
o.way([(0.65, 0.31), (0.66, 0.31), (0.66, 0.32), (0.65, 0.32)], {"building:part": "yes", "roof:shape": "dome", "height": "30"}, closed=True)
o.way([(0.64, 0.36), (0.68, 0.36), (0.68, 0.39), (0.64, 0.39)], {"leisure": "park", "name": "Parque Real"}, closed=True)
o.way([(0.29, 0.62), (0.33, 0.62)], {"man_made": "pier", "name": "Muelle Real"})
o.way([(0.31, 0.40), (0.31, 0.46)], {"man_made": "quay"})
o.way([(0.80, 0.72), (0.80, 0.78)], {"waterway": "dam", "name": "Presa Real"})
o.way([(0.40, 0.05), (0.90, 0.05), (0.90, 0.45), (0.40, 0.45)], {"landuse": "farmland"}, closed=True)
o.write("harbour.json")
