"""Writes w1_town.osm, w1_town.json (Overpass) and w1_town.osm.pbf: one small coastal town for the OSM buildings and
places tests. Run from this folder: python make_w1_town.py

A grid of streets 1.2 units apart (a unit is 0.001 degrees) round (23.2, -106.42). North-east block: nine buildings that
cover it (dense), two of them holding shops of one chain and a pharmacy. North-west: a cathedral with two building parts
and an apartment block round a courtyard (a multipolygon with a hole), covering little (sparse). South-west: a cemetery
and school grounds. South-east: a taqueria with no building, and the town's own place node. Water and harbour works
round the edge.
"""
import json
import struct
import zlib

LAT0, LON0, U = 23.2, -106.42, 0.001

nodes = []  # (id, lat, lon, tags)
ways = []  # (id, refs, tags)
relations = []  # (id, [(type, ref, role)], tags)
_next = [1]


def node(x, y, tags=None):
    nid = _next[0]
    _next[0] += 1
    nodes.append((nid, round(LAT0 + y * U, 7), round(LON0 + x * U, 7), tags or {}))
    return nid


def line(wid, pts, tags):
    ways.append((wid, [node(x, y) for x, y in pts], tags))


def rect(wid, x0, y0, x1, y1, tags):
    refs = [node(x0, y0), node(x1, y0), node(x1, y1), node(x0, y1)]
    ways.append((wid, refs + [refs[0]], tags))


# Streets.
line(100, [(-1.8, 1.2), (1.8, 1.2)], {"highway": "residential", "name": "Calle Benito Juárez"})
line(101, [(-1.8, 0.0), (1.8, 0.0)], {"highway": "primary", "name": "Avenida Olas Altas", "lanes": "4"})
line(102, [(-1.8, -1.2), (1.8, -1.2)], {"highway": "residential", "name": "Calle Ángel Flores"})
line(103, [(-1.2, -1.8), (-1.2, 1.8)], {"highway": "residential", "name": "Calle Constitución"})
line(104, [(0.0, -1.8), (0.0, 1.8)], {"highway": "secondary", "name": "Avenida Miguel Alemán"})
line(105, [(1.2, -1.8), (1.2, 1.8)], {"highway": "residential", "name": "Calle Aquiles Serdán"})

# North-east: nine buildings filling the block.
b = 1001
for j, y in enumerate((0.15, 0.5, 0.85)):
    for i, x in enumerate((0.15, 0.5, 0.85)):
        tags = {"building": "house"}
        if b == 1001:
            tags = {"building": "commercial", "building:levels": "3", "roof:shape": "hipped", "building:colour": "#d4a373",
                    "name": "Edificio Belmar"}
        elif b == 1002:
            tags = {"building": "house", "height": "7.5", "roof:shape": "gabled", "roof:colour": "#8b3a2b"}
        elif b == 1003:
            tags = {"building": "apartments", "building:levels": "5", "roof:shape": "flat", "building:material": "concrete"}
        rect(b, x, y, x + 0.3, y + 0.3, tags)
        b += 1
node(0.3, 0.65, {"amenity": "pharmacy", "name": "Farmacias Similares", "brand": "Farmacias Similares"})  # in 1004
node(0.65, 0.65, {"shop": "convenience", "name": "OXXO", "brand": "OXXO"})  # in 1005
node(1.0, 1.0, {"shop": "convenience", "name": "OXXO", "brand": "OXXO"})  # in 1009

# North-west: a cathedral in two parts and an apartment block round a courtyard.
rect(1010, -0.9, 0.3, -0.5, 0.6, {"building": "cathedral", "amenity": "place_of_worship",
                                  "name": "Catedral Basílica de la Inmaculada Concepción", "start_date": "1856"})
rect(1011, -0.9, 0.3, -0.75, 0.45, {"building:part": "yes", "height": "45", "roof:shape": "dome"})
rect(1012, -0.75, 0.3, -0.5, 0.6, {"building:part": "yes", "height": "18", "min_height": "0", "roof:shape": "gabled"})
rect(1013, -0.45, 0.7, -0.1, 1.0, {})
rect(1014, -0.35, 0.8, -0.2, 0.9, {})
relations.append((2001, [("way", 1013, "outer"), ("way", 1014, "inner")],
                  {"type": "multipolygon", "building": "apartments", "height": "12", "name": "Casa del Marino"}))
node(-0.6, 0.15, {"place": "neighbourhood", "name": "Colonia Centro"})

# South-west: cemetery and school grounds.
rect(3001, -1.1, -1.1, -0.1, -0.6, {"landuse": "cemetery", "name": "Panteón Municipal Número Uno"})
rect(3002, -1.1, -0.5, -0.1, -0.1, {"amenity": "school", "name": "Escuela Primaria Benito Juárez"})

# South-east: a taqueria standing on no building, and the town itself.
node(0.6, -0.1, {"amenity": "restaurant", "cuisine": "mexican;tacos", "name": "Taquería El Güero"})
node(0.6, -0.6, {"place": "town", "name": "Mazatlán"})

# Water and harbour works round the edge.
line(110, [(-1.8, -1.6), (0.0, -1.65), (1.8, -1.7)], {"waterway": "river", "name": "Río Presidio"})
line(111, [(1.5, 1.5), (1.7, 1.7)], {"man_made": "pier", "name": "Muelle Fiscal"})
line(112, [(1.5, -1.5), (1.7, -1.3)], {"man_made": "breakwater"})
line(113, [(1.4, 1.3), (1.4, 1.6)], {"man_made": "quay"})
line(114, [(-1.7, 1.5), (-1.5, 1.7)], {"waterway": "dam", "name": "Presa Picachos"})
rect(3003, 1.4, 0.2, 1.7, 0.8, {"natural": "beach", "name": "Playa Olas Altas"})
rect(3004, -1.75, -1.0, -1.45, -0.4, {"landuse": "reservoir", "name": "Presa Sanalona"})


def write_xml(path):
    esc = lambda v: v.replace("&", "&amp;").replace('"', "&quot;").replace("<", "&lt;")
    with open(path, "w", encoding="utf-8", newline="\n") as f:
        f.write("<?xml version='1.0' encoding='UTF-8'?>\n<osm version=\"0.6\" generator=\"make_w1_town.py\">\n")
        for nid, lat, lon, tags in nodes:
            if tags:
                f.write(f'  <node id="{nid}" lat="{lat:.7f}" lon="{lon:.7f}">\n')
                for k, v in tags.items():
                    f.write(f'    <tag k="{k}" v="{esc(v)}"/>\n')
                f.write("  </node>\n")
            else:
                f.write(f'  <node id="{nid}" lat="{lat:.7f}" lon="{lon:.7f}"/>\n')
        for wid, refs, tags in ways:
            f.write(f'  <way id="{wid}">\n')
            for r in refs:
                f.write(f'    <nd ref="{r}"/>\n')
            for k, v in tags.items():
                f.write(f'    <tag k="{k}" v="{esc(v)}"/>\n')
            f.write("  </way>\n")
        for rid, members, tags in relations:
            f.write(f'  <relation id="{rid}">\n')
            for t, r, role in members:
                f.write(f'    <member type="{t}" ref="{r}" role="{role}"/>\n')
            for k, v in tags.items():
                f.write(f'    <tag k="{k}" v="{esc(v)}"/>\n')
            f.write("  </relation>\n")
        f.write("</osm>\n")


def write_json(path):
    el = []
    for nid, lat, lon, tags in nodes:
        e = {"type": "node", "id": nid, "lat": lat, "lon": lon}
        if tags:
            e["tags"] = tags
        el.append(e)
    for wid, refs, tags in ways:
        e = {"type": "way", "id": wid, "nodes": refs}
        if tags:
            e["tags"] = tags
        el.append(e)
    for rid, members, tags in relations:
        el.append({"type": "relation", "id": rid, "members": [{"type": t, "ref": r, "role": role} for t, r, role in members], "tags": tags})
    with open(path, "w", encoding="utf-8", newline="\n") as f:
        json.dump({"version": 0.6, "generator": "make_w1_town.py", "elements": el}, f, ensure_ascii=False, indent=1)
        f.write("\n")


def varint(v):
    out = bytearray()
    while v >= 0x80:
        out.append((v & 0x7F) | 0x80)
        v >>= 7
    out.append(v)
    return bytes(out)


def zz(v):
    return (v << 1) ^ (v >> 63)


def key(f, w):
    return varint(f << 3 | w)


def field_bytes(f, b):
    return key(f, 2) + varint(len(b)) + b


def packed(f, values):
    return field_bytes(f, b"".join(varint(v) for v in values))


def write_pbf(path):
    strings = [""]

    def sid(s):
        if s not in strings:
            strings.append(s)
        return strings.index(s)

    # Dense nodes with their tags in keys_vals.
    ids, lats, lons, kv = [], [], [], []
    pid = plat = plon = 0
    for nid, lat, lon, tags in nodes:
        la, lo = round(lat * 1e7), round(lon * 1e7)
        ids.append(zz(nid - pid))
        lats.append(zz(la - plat))
        lons.append(zz(lo - plon))
        pid, plat, plon = nid, la, lo
        for k, v in tags.items():
            kv += [sid(k), sid(v)]
        kv.append(0)
    group = field_bytes(2, packed(1, ids) + packed(8, lats) + packed(9, lons) + packed(10, kv))
    for wid, refs, tags in ways:
        prev, deltas = 0, []
        for r in refs:
            deltas.append(zz(r - prev))
            prev = r
        body = key(1, 0) + varint(wid) + packed(2, [sid(k) for k in tags]) + packed(3, [sid(v) for v in tags.values()]) + packed(8, deltas)
        group += field_bytes(3, body)
    for rid, members, tags in relations:
        prev, deltas = 0, []
        for _, r, _ in members:
            deltas.append(zz(r - prev))
            prev = r
        body = (key(1, 0) + varint(rid) + packed(2, [sid(k) for k in tags]) + packed(3, [sid(v) for v in tags.values()]) +
                packed(8, [sid(role) for _, _, role in members]) + packed(9, deltas) + packed(10, [1 for _ in members]))
        group += field_bytes(4, body)
    table = b"".join(field_bytes(1, s.encode("utf-8")) for s in strings)
    block = field_bytes(1, table) + field_bytes(2, group) + key(17, 0) + varint(100)

    def blob(kind, raw, compress):
        b = key(2, 0) + varint(len(raw)) + field_bytes(3, zlib.compress(raw)) if compress else field_bytes(1, raw)
        h = field_bytes(1, kind.encode()) + key(3, 0) + varint(len(b))
        return struct.pack(">I", len(h)) + h + b

    header = field_bytes(4, b"OsmSchema-V0.6") + field_bytes(4, b"DenseNodes")
    with open(path, "wb") as f:
        f.write(blob("OSMHeader", header, False) + blob("OSMData", block, True))


write_xml("w1_town.osm")
write_json("w1_town.json")
write_pbf("w1_town.osm.pbf")
print(len(nodes), "nodes", len(ways), "ways", len(relations), "relations")
