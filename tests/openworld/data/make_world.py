"""Synthetic Sinaloa-style world for the Open World acceptance run: two places as ESRI ASCII elevation (degrees) plus
Overpass JSON, and the world specs that remix them into one map. No network.

coast (a Mazatlan stand-in, 16 x 16 km): sea and two islands to the west, a mangrove lagoon to the north-west, the coastal
city on the plain (seafront boulevard, colonial centre, modern grid, resort strip, cargo port, cruise pier), a farm
valley with a highway and a river to the south-east, foothills rising east with a reservoir valley, tracks and fords.
capital (a Culiacan stand-in, 7 x 7 km): a grid city with a civic district, a mansion district and a cemetery; the
remix cuts it out and moves it onto the coast map's inland plain.

Real-looking names and brands are in the OSM on purpose: every one must come out fictional.

    python make_world.py --size tiny --out world_tiny     (the test fixture, checked in)
    python make_world.py --size full --out <scratch dir>   (the acceptance map, about 12 x 12 km of land)

Writes coast.asc, coast.json, capital.asc, capital.json, world.json (the spec for these files) and, with --size full,
world_real.json (the same world pointed at real data for Mazatlan and Culiacan, fetched only with consent).
"""
import argparse
import json
import math
import os
import random

LAT0, LON0 = 23.22, -106.42      # coast source centre (Mazatlan area)
CLAT0, CLON0 = 24.80, -107.39    # capital source centre (Culiacan area)
M_LAT = 110574.0


def m_lon(lat):
    return 111320.0 * math.cos(math.radians(lat))


def smooth(a, b, x):
    t = min(1.0, max(0.0, (x - a) / (b - a)))
    return t * t * (3 - 2 * t)


def noise(u, v, seed):
    """Cheap value-free noise: a few fixed sine waves."""
    r = random.Random(seed)
    s = 0.0
    for k in range(5):
        fx, fy, ph = r.uniform(3, 14) * (k + 1), r.uniform(3, 14) * (k + 1), r.uniform(0, 6.28)
        s += math.sin(u * fx + ph) * math.cos(v * fy + ph * 0.7) / (k + 1)
    return s


class Place:
    """One source: u east 0..1, v north 0..1 over span_m metres centred on (lat0, lon0)."""

    def __init__(self, lat0, lon0, span_m, cell_m):
        self.lat0, self.lon0, self.span = lat0, lon0, span_m
        self.dlat, self.dlon = span_m / M_LAT, span_m / m_lon(lat0)
        self.cell_m = cell_m
        self.els, self.next = [], 10_000_000

    def ll(self, u, v):
        return self.lat0 + (v - 0.5) * self.dlat, self.lon0 + (u - 0.5) * self.dlon

    def world(self, u, v, at=(0.0, 0.0)):
        """Base-map cm for (u, v) when the source's centre is placed at `at` (north is -z)."""
        return [round(at[0] + (u - 0.5) * self.span * 100.0), round(at[1] - (v - 0.5) * self.span * 100.0)]

    def write_asc(self, path, height):
        n = int(self.span / self.cell_m) + 1
        d_lon, d_lat = self.dlon / (n - 1), self.dlat / (n - 1)
        # ESRI ASCII takes one square cell size: use the latitude step and widen the columns to cover the longitude span.
        cols = int(round(self.dlon / d_lat)) + 1
        x0, y0 = self.lon0 - self.dlon / 2, self.lat0 - self.dlat / 2
        with open(path, "w", newline="\n") as o:
            o.write(f"ncols {cols}\nnrows {n}\nxllcorner {x0 - d_lat / 2:.7f}\nyllcorner {y0 - d_lat / 2:.7f}\ncellsize {d_lat:.9f}\nNODATA_value -9999\n")
            for r in range(n):
                v = 1 - r / (n - 1)
                o.write(" ".join(f"{height(c / (cols - 1), v):.1f}" for c in range(cols)) + "\n")

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

    def rect(self, u0, v0, u1, v1, tags):
        return self.way([(u0, v0), (u1, v0), (u1, v1), (u0, v1)], tags, closed=True)

    def m(self, metres):
        """metres as a fraction of the span."""
        return metres / self.span

    def write_json(self, path):
        with open(path, "w", newline="\n") as o:
            json.dump({"version": 0.6, "generator": "make_world.py", "elements": self.els}, o, separators=(",", ":"))


SHOPS = [("shop", "convenience", "OXXO"), ("amenity", "pharmacy", "Farmacias Guadalajara"), ("shop", "supermarket", "Soriana"),
         ("amenity", "restaurant", "Mariscos El Camaron Dorado"), ("amenity", "bank", "BBVA"), ("shop", "hardware", "Ferreteria Sinaloa"),
         ("amenity", "fast_food", "Tacos Chuy"), ("shop", "clothes", "Coppel"), ("amenity", "cafe", "Cafe Pacifico"), ("amenity", "bar", "Cantina Los Arcos")]


def shoreline(v):
    return 0.20 + 0.015 * math.sin(v * 9.0) + 0.01 * math.sin(v * 23.0)


def shore_max(v0, v1):
    """The shore's furthest point east between two latitudes, so a grid laid east of it stays on land."""
    return max(shoreline(v0 + (v1 - v0) * k / 50) for k in range(51))


ISLANDS = [(0.075, 0.33, 0.022, 26.0), (0.10, 0.72, 0.03, 18.0)]  # u, v, radius (fraction), height m
LAGOON = (0.255, 0.885, 0.035, 0.045)  # u, v, rx, ry
RESERVOIR_V = 0.44
DAM_U = 0.83


def coast_height(u, v, tiny):
    xs = shoreline(v)
    # Islands first: they stand in the sea.
    for iu, iv, r, h in ISLANDS:
        d = math.hypot(u - iu, v - iv)
        if d < r:
            return max(1.5, h * (1.0 - d / r) ** 0.7)
    if u < xs:
        return max(-80.0, -0.5 - (xs - u) * 450.0)
    lu, lv, rx, ry = LAGOON
    e = ((u - lu) / rx) ** 2 + ((v - lv) / ry) ** 2
    if e < 1.0:
        return -1.2 + 0.8 * e
    # Inlet from the sea into the lagoon.
    if abs(v - lv) < 0.006 and u < lu:
        return -1.5
    land = u - xs
    h = 2.0 + land * 110.0 + 3.0 * noise(u, v, 3) * smooth(0.0, 0.04, land)
    # Barrier and mangrove flats round the lagoon stay low.
    if e < 2.2:
        h = min(h, 0.6 + (e - 1.0) * 2.0)
    # The farm valley is the plain itself: smooth, falling gently west to the sea so its rivers drain.
    valley = smooth(0.42, 0.48, u) * (1 - smooth(0.74, 0.78, u)) * (1 - smooth(0.40, 0.46, v))
    h -= 3.0 * noise(u, v, 3) * valley
    # Foothills to the east (Sierra Madre stand-in).
    hills = smooth(0.74, 1.0, u)
    ridged = 1.0 - abs(noise(u * 1.7, v * 1.7, 11))
    h += hills * (650.0 * ridged + 120.0 * noise(u * 3, v * 3, 13))
    # River valley along v=0.08 to the sea, and the reservoir valley in the foothills.
    h -= 10.0 * math.exp(-((v - 0.08) / 0.012) ** 2) * smooth(0.21, 0.3, u)
    # Reservoir: a valley cut into a 400 m plateau, rising east to a closed head; the dam crosses it at DAM_U.
    rv = RESERVOIR_V
    d = abs(v - rv)
    if 0.76 < u < 0.97 and d < 0.07:
        floor = 200.0 + (u - 0.80) * 800.0 if u >= 0.80 else 200.0 - (0.80 - u) * 1500.0
        if u > 0.93:
            floor += (u - 0.93) * 6000.0
        side = smooth(0.012, 0.03, d)
        valley = floor + (400.0 - floor) * side
        edge = smooth(0.05, 0.07, d) if u > 0.80 else max(smooth(0.05, 0.07, d), smooth(0.80, 0.76, u))
        h = valley * (1 - edge) + max(h, 400.0 if u > 0.80 else h) * edge
    # Plain where the capital lands: held near 60 m.
    plain = smooth(0.60, 0.66, u) * (1 - smooth(0.86, 0.9, u)) * smooth(0.50, 0.55, v) * (1 - smooth(0.86, 0.9, v))
    return h * (1 - plain) + (58.0 + 2.0 * noise(u, v, 17)) * plain


def capital_height(u, v):
    return 55.0 + 8.0 * noise(u, v, 21) + 12.0 * u - 6.0 * math.exp(-((v - 0.35) / 0.03) ** 2)


def grid_city(p, u0, v0, cols, rows, block_m, street_m, house, names, tags_street, per_block, rng, start_date=None, levels=(1, 3), join=None, skip=()):
    """A street grid with buildings round each block's edge; returns the buildings made. join(v) gives where a row
    street starts west of the grid (to meet a road there); blocks in skip stay empty."""
    bu, sv = p.m(block_m), p.m(block_m)
    made = 0
    for c in range(cols + 1):
        u = u0 + c * bu
        p.way([(u, v0 + k * sv * rows / 4) for k in range(5)], dict(tags_street, name=names[c % len(names)]))
    for r in range(rows + 1):
        v = v0 + r * sv
        start = join(v) if join else u0
        p.way([(start, v)] + [(u0 + k * bu * cols / 4, v) for k in range(5)], dict(tags_street, name=names[(r + 3) % len(names)]))
    half = p.m(street_m) / 2
    for c in range(cols):
        for r in range(rows):
            if (c, r) in skip:
                continue
            bu0, bv0 = u0 + c * bu + half, v0 + r * sv + half
            bu1, bv1 = u0 + (c + 1) * bu - half, v0 + (r + 1) * sv - half
            side = (bu1 - bu0)
            n = per_block
            depth = side * 0.3
            for k in range(n):
                edge = k % 4
                t0, t1 = (k // 4) / max(1, (n + 3) // 4), (k // 4 + 0.85) / max(1, (n + 3) // 4)
                if edge == 0:
                    q = (bu0 + t0 * side, bv0, bu0 + t1 * side, bv0 + depth)
                elif edge == 1:
                    inner = side - 2.0 * depth
                    q = (bu1 - depth, bv0 + depth + t0 * inner, bu1, bv0 + depth + t1 * inner)
                elif edge == 2:
                    q = (bu0 + t0 * side, bv1 - depth, bu0 + t1 * side, bv1)
                else:
                    inner = side - 2.0 * depth
                    q = (bu0, bv0 + depth + t0 * inner, bu0 + depth, bv0 + depth + t1 * inner)
                tags = {"building": house, "building:levels": str(rng.randint(*levels))}
                if start_date:
                    tags["start_date"] = start_date
                p.rect(*q, tags)
                made += 1
    return made


def build(size, out):
    tiny = size == "tiny"
    rng = random.Random(7)
    os.makedirs(out, exist_ok=True)
    span = 16000.0
    coast = Place(LAT0, LON0, span, 80.0 if tiny else 40.0)
    coast.write_asc(os.path.join(out, "coast.asc"), lambda u, v: coast_height(u, v, tiny))

    xs = shoreline
    # Seafront boulevard, colonial centre, modern grid, resort strip.
    coast.way([(xs(v) + 0.008, v) for v in [0.10 + k * 0.012 for k in range(41)]], {"highway": "primary", "name": "Avenida del Mar"})
    names = ["Calle Benito Juarez", "Calle Aquiles Serdan", "Calle Constitucion", "Calle Belisario Dominguez", "Calle Angel Flores", "Calle Carnaval"]
    cc, cr = (2, 2) if tiny else (6, 8)
    centre_v = 0.16
    centre_u = shore_max(centre_v, centre_v + cr * coast.m(85.0)) + 0.014
    malecon = lambda v: xs(v) + 0.008
    buildings = grid_city(coast, centre_u, centre_v, cc, cr, 85.0, 12.0, "house", names, {"highway": "residential"}, 8 if tiny else 16, rng, "1840", (1, 2), join=malecon)
    plaza = (centre_u + coast.m(85.0), centre_v + coast.m(85.0))
    coast.rect(plaza[0] + 0.001, plaza[1] + 0.001, plaza[0] + 0.004, plaza[1] + 0.004, {"leisure": "park", "name": "Plazuela Machado"})
    coast.rect(plaza[0] + 0.0015, plaza[1] + 0.0045, plaza[0] + 0.0035, plaza[1] + 0.0052,
               {"building": "cathedral", "amenity": "place_of_worship", "name": "Catedral Basilica de la Inmaculada Concepcion", "building:levels": "3"})
    mc, mr = (2, 2) if tiny else (11, 14)
    modern_v = centre_v + cr * coast.m(85.0) + coast.m(30.0)
    buildings += grid_city(coast, shore_max(modern_v, modern_v + mr * coast.m(130.0)) + 0.014, modern_v, mc, mr, 130.0, 16.0, "residential",
                           ["Avenida Ejercito Mexicano", "Calle Rio Presidio", "Avenida Gutierrez Najera", "Calle Kena Moreno"], {"highway": "residential"},
                           4 if tiny else 6, rng, None, (1, 3), join=malecon)
    hotels = 6 if tiny else 30
    for k in range(hotels):
        v = 0.47 + k * (0.11 / hotels)
        u = xs(v) + 0.011
        coast.rect(u, v, u + 0.003, v + 0.0025, {"building": "hotel", "tourism": "hotel", "building:levels": str(rng.randint(8, 18)), "name": f"Hotel Playa {k}"})
        buildings += 1
    coast.way([(xs(0.47) + 0.006, 0.47), (xs(0.58) + 0.006, 0.58)], {"natural": "beach", "name": "Playa Gaviotas"})
    # POIs with real-looking names and brands, on the centre's streets.
    for k in range(20 if tiny else 120):
        key, val, name = SHOPS[k % len(SHOPS)]
        u = centre_u + rng.uniform(0.0, cc * coast.m(85.0))
        v = centre_v + rng.uniform(0.0, cr * coast.m(85.0))
        tags = {key: val, "name": name}
        if name in ("OXXO", "Soriana", "Coppel", "BBVA", "Farmacias Guadalajara"):
            tags["brand"] = name
        coast.node(u, v, tags)
    # Cargo port and cruise pier area (the site kits lay them out; the OSM marks the land use).
    coast.rect(xs(0.11) + 0.002, 0.105, xs(0.11) + 0.03, 0.14, {"landuse": "industrial", "industrial": "port", "name": "Puerto de Mazatlan"})
    coast.node(xs(0.30) + 0.004, 0.30, {"amenity": "ferry_terminal", "name": "Terminal de Cruceros"})
    # Highway east through the farm valley to the capital, with the river crossing.
    coast.way([(xs(0.35) + 0.02, 0.35)] + [(0.3 + k * 0.035, 0.35 - 0.1 * math.sin(k * 0.3)) for k in range(21)], {"highway": "trunk", "ref": "MEX 15", "name": "Carretera Internacional"})
    coast.way([(1.0 - k * 0.04, 0.08 + 0.004 * math.sin(k)) for k in range(21)] + [(xs(0.08) - 0.01, 0.08)], {"waterway": "river", "name": "Rio Presidio"})
    coast.way([(0.6, 0.30), (0.6, 0.05)], {"highway": "secondary", "bridge": "yes", "name": "Camino a Villa Union"})
    # The road the moved capital's highway joins on both sides (its stretch under the capital is replaced by the paste).
    coast.way([(0.55 + k * 0.02, 0.69) for k in range(21)], {"highway": "trunk", "name": "Carretera a Culiacan"})
    coast.way([(0.55, 0.69), (0.5, 0.52), (0.45, 0.35)], {"highway": "trunk", "name": "Libramiento Norte"})
    # Farms and packing plants in the valley.
    for k in range(4 if tiny else 10):
        u0, v0 = 0.47 + (k % 5) * 0.055, 0.12 + (k // 5) * 0.12
        coast.rect(u0, v0, u0 + 0.045, v0 + 0.08, {"landuse": "farmland", "crop": "tomato"})
        if k % 3 == 0:
            coast.rect(u0 + 0.005, v0 + 0.085, u0 + 0.015, v0 + 0.092, {"building": "industrial", "name": f"Empacadora {k}"})
            buildings += 1
    # Foothill tracks to the villages, fording the river, and an unpaved strip.
    for k, (u, v) in enumerate([(0.86, 0.18), (0.92, 0.6), (0.8, 0.9)]):
        coast.way([(0.72, 0.35), ((0.72 + u) / 2, (0.35 + v) / 2 + 0.03), (u, v)], {"highway": "track", "surface": "dirt", "name": f"Brecha {k}"})
        coast.node(u, v, {"place": "village", "name": ["El Habal", "La Noria", "El Quelite"][k]})
    coast.way([(0.9, 0.08), (0.9, 0.12)], {"highway": "track", "ford": "yes"})
    coast.way([(0.93, 0.7), (0.95, 0.75)], {"aeroway": "runway", "surface": "dirt"})
    # Reservoir and dam in the foothill valley; mangroves and the lagoon.
    coast.rect(DAM_U + 0.005, RESERVOIR_V - 0.015, 0.92, RESERVOIR_V + 0.015, {"natural": "water", "water": "reservoir", "name": "Presa Picachos"})
    coast.way([(DAM_U, RESERVOIR_V - 0.03), (DAM_U, RESERVOIR_V + 0.03)], {"waterway": "dam", "name": "Cortina Picachos"})
    lu, lv, rx, ry = LAGOON
    coast.way([(lu + rx * 1.3 * math.cos(a / 12 * 6.283), lv + ry * 1.3 * math.sin(a / 12 * 6.283)) for a in range(12)], {"natural": "wetland", "wetland": "mangrove", "name": "Estero del Yugo"}, closed=True)
    coast.way([(xs(v), v) for v in [k / 40 for k in range(41)]], {"natural": "coastline"})
    for iu, iv, r, h in ISLANDS:
        coast.way([(iu + r * math.cos(a / 16 * 6.283), iv + r * math.sin(a / 16 * 6.283)) for a in range(16)], {"natural": "coastline", "place": "island",
                  "name": "Isla de Venados" if iv < 0.5 else "Isla de los Pajaros"}, closed=True)
    coast.write_json(os.path.join(out, "coast.json"))

    cspan = 7000.0
    cap = Place(CLAT0, CLON0, cspan, 80.0 if tiny else 40.0)
    cap.write_asc(os.path.join(out, "capital.asc"), capital_height)
    cap.way([(0.0, 0.5), (1.0, 0.52)], {"highway": "trunk", "name": "Boulevard Emiliano Zapata"})
    cap.way([(k / 20, 0.35 + 0.01 * math.sin(k)) for k in range(21)], {"waterway": "river", "name": "Rio Tamazula"})
    gc, gr = (3, 3) if tiny else (14, 14)
    cu0, cv0 = 0.5 - gc * cap.m(110.0) / 2, 0.5 - gr * cap.m(110.0) / 2
    civic = (gc // 2, gr // 2)
    buildings += grid_city(cap, cu0, cv0, gc, gr, 110.0, 16.0, "residential", ["Avenida Obregon", "Calle Rosales", "Calle Hidalgo", "Avenida Insurgentes"],
                           {"highway": "residential"}, 4 if tiny else 6, rng, None, (1, 2), skip={civic})
    # Civic district in the block the grid left empty: palacio, municipal palace, police.
    cb_u, cb_v = cu0 + civic[0] * cap.m(110.0) + cap.m(12.0), cv0 + civic[1] * cap.m(110.0) + cap.m(12.0)
    for k, (b, n) in enumerate([("government", "Palacio de Gobierno"), ("public", "Palacio Municipal"), ("police", "Secretaria de Seguridad Publica")]):
        u, v = cb_u + k * cap.m(28.0), cb_v + cap.m(20.0)
        tags = {"building": b, "name": n, "building:levels": "3"}
        if b == "police":
            tags["amenity"] = "police"
        else:
            tags["office"] = "government"
        cap.rect(u, v, u + cap.m(24.0), v + cap.m(40.0), tags)
        buildings += 1
    # Mansion district: few large houses on big lots to the north-east.
    north = cv0 + gr * cap.m(110.0)
    for k in range(4 if tiny else 12):
        u, v = 0.42 + (k % 6) * 0.027, north + 0.012 + (k // 6) * 0.022
        cap.rect(u, v, u + 0.012, v + 0.01, {"building": "house", "building:levels": "2", "luxury": "yes"})
        buildings += 1
    cap.way([(0.42, north + 0.008), (0.58, north + 0.008)], {"highway": "residential", "name": "Calle Los Pinos"})
    cap.way([(0.5, north), (0.5, north + 0.008)], {"highway": "residential", "name": "Privada Los Pinos"})
    # Cemetery (the site kit lays its mausoleums out; this is the land use).
    cap.rect(0.2, 0.62, 0.28, 0.72, {"landuse": "cemetery", "name": "Jardines del Humaya"})
    for k in range(10 if tiny else 60):
        key, val, name = SHOPS[k % len(SHOPS)]
        tags = {key: val, "name": name}
        cap.node(cu0 + rng.uniform(0.0, gc * cap.m(110.0)), cv0 + rng.uniform(0.0, gr * cap.m(110.0)), tags)
    cap.write_json(os.path.join(out, "capital.json"))

    # The world spec. The coast source sits at the map's centre; the capital is placed off the map to the east and its
    # city cut and moved onto the coast's inland plain.
    cap_at = (2600000.0, 0.0)
    target = coast.world(0.76, 0.69)
    half = (gc * 110.0 / 2 + 600.0) * 100.0
    capital_poly = [[cap_at[0] - half, cap_at[1] - half], [cap_at[0] + half, cap_at[1] - half], [cap_at[0] + half, cap_at[1] + half], [cap_at[0] - half, cap_at[1] + half]]
    city_lo, city_hi = coast.world(xs(0.2) - 0.005, 0.62), coast.world(shore_max(0.09, 0.62) + 0.014 + mc * coast.m(130.0) + 0.02, 0.09)
    cap_lo = [target[0] - half, target[1] - half]
    cap_hi = [target[0] + half, target[1] + half]
    sea_x = lambda v: coast.world(xs(v), v)[0]
    spec = {
        "name": "Sinaloa" if not tiny else "SinaloaTiny",
        "description": "Synthetic Sinaloa-style world from make_world.py: coastal city, capital moved toward the coast, farm valley, foothills, coast, islands, lagoon, reservoir.",
        "culture": "mexican",
        "remix": {
            "scale": {"horizontal": 1.0, "vertical": 1.0},
            "sources": [{"id": "coast", "dem": "coast.asc", "osm": "coast.json", "at": [0, 0]},
                        {"id": "capital", "dem": "capital.asc", "osm": "capital.json", "at": list(cap_at)}],
            "regions": [{"id": "capital", "op": "cut", "polygon": capital_poly, "translate": [target[0] - cap_at[0], target[1] - cap_at[1]]}],
            "map": {"center": [0, 0], "size": [span * 100.0, span * 100.0], "cell": 3200.0 if tiny else 800.0, "maxResolution": 2049},
            "seams": {"width": 3000.0, "roadReach": 60000.0},
        },
        "terrain": {
            "water": {"sea": {"level": 0}},
            "coast": {
                "bathymetry": True,
                "masks": True,
                "mangroves": True,
                "reservoirs": [{"id": "picachos", "a": coast.world(DAM_U, RESERVOIR_V - 0.03), "b": coast.world(DAM_U, RESERVOIR_V + 0.03), "crest": 30000}],
                "channels": [{"id": "estero", "craft": "skiff", "path": [coast.world(LAGOON[0] - 0.05, LAGOON[1]), coast.world(LAGOON[0], LAGOON[1]), coast.world(LAGOON[0] + 0.02, LAGOON[1] + 0.02)]}],
                "routes": {},
                "sceneObjects": False,
            },
        },
        "settlements": [
            {"name": "Puerto", "osm": "remix", "area": [city_lo[0], city_lo[1], city_hi[0], city_hi[1]], "profile": "usTown", "fill": "none" if tiny else "sparse",
             "buildings": {"cell": 6400, "districts": {}}},
            {"name": "Capital", "osm": "remix", "area": [cap_lo[0], cap_lo[1], cap_hi[0], cap_hi[1]], "profile": "usTown", "fill": "none" if tiny else "sparse", "buildings": {"lod": "near"}},
        ],
        "sites": [
            {"kind": "port", "id": "port", "at": [sea_x(0.135) + 7000, coast.world(0.2, 0.135)[1]], "heading": 90, "params": {"quayLength": 50000 if not tiny else 40000, "tiers": 3}},
            {"kind": "cruise", "id": "cruise", "at": [sea_x(0.30) + 4000, coast.world(0.2, 0.30)[1]], "heading": 90},
            {"kind": "lighthouse", "id": "faro", "at": coast.world(ISLANDS[0][0] + 0.008, ISLANDS[0][1]), "heading": 90},
            {"kind": "island", "id": "isla", "at": coast.world(ISLANDS[1][0], ISLANDS[1][1]), "heading": 90,
             "params": {"islandRadius": round(ISLANDS[1][2] * span * 100 * 0.8), "dockLength": 16000, "raiseIsland": False}},
            {"kind": "cemetery", "id": "panteon", "at": [target[0] - half - 15000, target[1] + 20000]},
            {"kind": "compound", "id": "mansion", "at": [target[0] + half + 15000, target[1] - 10000]},
            {"kind": "farm", "id": "farm1", "at": coast.world(0.5, 0.18)},
            {"kind": "airstrip", "id": "pista1", "at": coast.world(0.42, 0.52), "heading": 90},
            {"kind": "village", "id": "habal", "at": coast.world(0.74, 0.12), "params": {"houses": 10, "villageRadius": 12000}},
        ] + ([] if tiny else [
            {"kind": "farm", "id": "farm2", "at": coast.world(0.64, 0.12), "heading": 90},
            {"kind": "airstrip", "id": "pista2", "at": coast.world(0.42, 0.68), "heading": 90},
            {"kind": "village", "id": "noria", "at": coast.world(0.70, 0.94), "params": {"houses": 12, "villageRadius": 14000}},
            {"kind": "village", "id": "quelite", "at": coast.world(0.50, 0.94), "params": {"houses": 12, "villageRadius": 14000}},
        ]),
        "buildings": {"lod": "near",
                      # A few walk-in buildings: the capital's police and government palaces, a bar, a mansion and a church.
                      "enterable": [{"programs": ["police", "government"]}, {"programs": ["bar"], "limit": 2}, {"programs": ["mansion"], "limit": 1},
                                    {"programs": ["church"], "limit": 1}]},
        "slots": {"rules": [{"id": "signs", "name": "Fictional signs", "type": "sign.storefront", "payload": {"poolTag": "fictional_signs"}},
                            {"id": "palms", "name": "Palms", "type": "tree.palm", "payload": {"poolTag": "palms"}}]},
        "zones": {"name": "Zones", "fromDistricts": True, "fromWater": True,
                  "spec": {"layers": [{"id": "police", "type": "police", "zones": [
                      {"id": "puerto", "name": "Policia Municipal del Puerto", "polygons": [[coast.world(0.0, 1.0), coast.world(0.5, 1.0), coast.world(0.5, 0.0), coast.world(0.0, 0.0)]]},
                      {"id": "sierra", "name": "Policia Estatal Serrana", "polygons": [[coast.world(0.5, 1.0), coast.world(1.0, 1.0), coast.world(1.0, 0.0), coast.world(0.5, 0.0)]]}]}]}},
        "check": True,
        "export": {"target": "files", "map": {"resolution": 512 if tiny else 4096}},
    }
    with open(os.path.join(out, "world.json"), "w", newline="\n") as o:
        json.dump(spec, o, indent=1)
    if not tiny:
        real = json.loads(json.dumps(spec))
        real["name"] = "SinaloaReal"
        real["description"] = ("Real data for Mazatlan and Culiacan: elevation (Copernicus 30 m), OSM (Overpass), WorldCover land cover and GEBCO "
                               "bathymetry (through OpenTopography, needs your free API key). One call downloads whatever is missing straight into "
                               "the paths below (relative to this file) and builds: worldBuild(file='sinaloa_real.json', estimate=True) lists each "
                               "download and its size, then worldBuild(file='sinaloa_real.json', fetch=True, consent=True, apiKey='...', "
                               "saveKey=True, replace=True) fetches and builds (consent=True allows more than 25 MB; the key is saved once, later "
                               "calls leave it out). In the UI: Level > Open World > Build World with Fetch Missing Data on. Settlement areas and "
                               "site positions follow the synthetic map: check them on worldMap first.")
        # The real Mazatlan box is about 22 km across: 1:1.4 fits it to the 16 km map; the capital cut is downtown Culiacan.
        real["remix"]["scale"] = {"horizontal": "1:1.4", "vertical": "1:1.2"}
        h = 250000.0
        real["remix"]["regions"][0]["polygon"] = [[cap_at[0] - h, cap_at[1] - h], [cap_at[0] + h, cap_at[1] - h], [cap_at[0] + h, cap_at[1] + h], [cap_at[0] - h, cap_at[1] + h]]
        real["remix"]["sources"] = [
            {"id": "coast", "bbox": [-106.52, 23.13, -106.30, 23.32], "dem": "real/mazatlan_dem.tif", "osm": "real/mazatlan.osm.pbf",
             "landcover": "real/mazatlan_worldcover.tif", "bathymetry": "real/gebco_mazatlan.tif", "at": [0, 0]},
            {"id": "capital", "bbox": [-107.47, 24.74, -107.33, 24.86], "dem": "real/culiacan_dem.tif", "osm": "real/culiacan.osm.pbf",
             "landcover": "real/culiacan_worldcover.tif", "at": list(cap_at)}]
        real["terrain"]["coast"]["bathymetry"] = {"file": "real/gebco_mazatlan.tif"}
        real["fetch"] = None
        del real["fetch"]
        with open(os.path.join(out, "world_real.json"), "w", newline="\n") as o:
            json.dump(real, o, indent=1)
    print(f"{size}: {buildings} buildings in the OSM, files in {out}")


if __name__ == "__main__":
    a = argparse.ArgumentParser()
    a.add_argument("--size", choices=["tiny", "full"], default="tiny")
    a.add_argument("--out", default="world_tiny")
    args = a.parse_args()
    build(args.size, args.out)
