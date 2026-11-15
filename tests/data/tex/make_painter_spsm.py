"""Writes painter_tiny.spsm: a synthetic Substance 3D Painter smart material for lila_tex_tests.

The same preset twice under one uuid (the current layout, dataversion 88, and the older one, dataversion 22, labelled
differently so the test can tell which was kept), plus one embedded image. Needs h5py and Pillow: python make_painter_spsm.py
"""
import os
import struct

import io

import h5py
import numpy as np
from PIL import Image

F32, VEC3, I32, ENUM, BOOL, U64, STR, LIST, OBJ, OBJS = 0x01, 0x03, 0x0B, 0x09, 0x0A, 0x0C, 0x10, 0x11, 0x12, 0x13
ALL = 2**64 - 1


def o(cls, *fields):
    return (cls, list(fields))


def blend(mode, channels, opacity=1.0):
    return o("DataBlending", ("blendingMode", ENUM, mode), ("channelTypes", U64, channels), ("opacity", F32, opacity))


def uniform(channels, rgb):
    return o("DataSourceUniform", ("channelTypes", U64, channels), ("color", VEC3, rgb), ("opacity", F32, 1.0), ("uid", U64, 7))


def procedural(url, tweaks):
    ts = [o("DataTweakFloat", ("identifier", STR, k), ("uid", U64, 1), ("value", F32, v)) for k, v in tweaks]
    proc = o("DataProcedural", ("tweaks", OBJS, ts), ("uid", U64, 2), ("urlToSbsRes", STR, url))
    return o("DataSourceProcedural", ("channelTypes", U64, 0), ("procedural", OBJ, proc), ("uid", U64, 3))


def bitmap(url):
    bmp = o("DataBitmap", ("uid", U64, 4), ("urlToBitmapRes", STR, url))
    return o("DataSourceBitmap", ("bitmap", OBJ, bmp), ("channelTypes", U64, 0), ("uid", U64, 5))


def fill(sources, pcb=(), channels=ALL):
    return o("DataActionFill", ("channelTypes", U64, channels), ("enabled", BOOL, True), ("label", STR, ""),
             ("perChannelBlending", LIST, list(pcb)), ("sources", OBJS, sources), ("uid", U64, 6))


def paint():
    return o("DataActionPaint", ("enabled", BOOL, True), ("label", STR, ""), ("uid", U64, 8))


def stack(items):
    return o("DataStackActions", ("items", OBJS, items), ("uid", U64, 9))


def layer(label, actions, masks=(), pcb=(), mask_initial=1):
    return o("DataLayerColor", ("actions", OBJ, stack(actions)), ("enabled", BOOL, True), ("label", STR, label),
             ("maskActions", OBJ, stack(list(masks)) if masks else None), ("maskEnabled", BOOL, True),
             ("maskInitial", ENUM, mask_initial), ("perChannelBlending", LIST, list(pcb)), ("uid", U64, 10))


def preset(label, version):
    base = layer("Base", [fill([uniform(1, [0.2, 0.3, 0.4]), uniform(0x80, [0.6, 0.6, 0.6]), uniform(2, [0.1, 0.1, 0.1]),
                                uniform(4, [0.04, 0.04, 0.04])])])
    edges = layer("Edges", [fill([uniform(1, [0.9, 0.8, 0.7])])],
                  masks=[fill([procedural("/mg_curvature/mg_curvature?version=aa.sbsar", [("global_balance", 0.5)])], [blend(2, 0)]),
                         fill([bitmap("/stain?version=bb.image")], [blend(4, 0)]), paint()],
                  pcb=[blend(10, 2, 0.5), blend(5, 1)], mask_initial=0)
    group = o("DataLayerGroup", ("enabled", BOOL, True), ("label", STR, label), ("maskActions", OBJ, None),
              ("maskEnabled", BOOL, False), ("perChannelBlending", LIST, []),
              ("subStack", OBJ, o("DataStackLayers", ("items", OBJS, [base, edges]), ("uid", U64, 11))), ("uid", U64, 12))
    return o("DataLayersPreset", ("layerGroup", OBJ, group), ("preset_dataversion", I32, version), ("uid", U64, 1),
             ("uuid", STR, "11111111-2222-3333-4444-555555555555"))


def s(v):
    b = v.encode()
    return struct.pack("<I", len(b)) + b


def scalar(t, v, enum64):
    if t == F32:
        return struct.pack("<f", v)
    if t == VEC3:
        return struct.pack("<3f", *v)
    if t == I32:
        return struct.pack("<i", v)
    if t == ENUM:
        return struct.pack("<q" if enum64 else "<i", v)
    if t == BOOL:
        return b"\xff" if v else b"\x00"
    if t == U64:
        return struct.pack("<Q", v)
    if t == STR:
        return s(v)
    raise ValueError(t)


class V1:
    def __init__(self):
        self.classes = {}
        self.out = bytearray()

    def classref(self, cls, fields):
        if cls in self.classes:
            self.out += struct.pack("<i", self.classes[cls])
            return
        self.classes[cls] = len(self.classes)
        self.out += struct.pack("<i", -1) + s(cls) + struct.pack("<I", len(fields))
        for name, t, _ in fields:
            self.out += s(name) + struct.pack("<I", t)

    def values(self, fields):
        for _, t, v in fields:
            self.value(t, v)

    def obj(self, node):
        if node is None:
            self.out += struct.pack("<I", 0)
            return
        at = len(self.out)
        self.out += b"\0\0\0\0"
        cls, fields = node
        self.classref(cls, fields)
        self.values(fields)
        struct.pack_into("<I", self.out, at, len(self.out))

    def value(self, t, v):
        if t == OBJ:
            self.obj(v)
        elif t == OBJS:
            self.out += struct.pack("<I", len(v))
            for x in v:
                self.obj(x)
        elif t == LIST:
            self.out += struct.pack("<I", len(v))
            if v:
                self.classref(v[0][0], v[0][1])
                for x in v:
                    self.values(x[1])
        else:
            self.out += scalar(t, v, False)

    def encode(self, root):
        self.out += struct.pack("<5I", 0x1B7C2FDD, 1, 0, 0, 0)
        self.obj(root)
        return bytes(self.out)


class V0:
    def __init__(self):
        self.out = bytearray()

    def typed(self, t, v):
        self.out.append(t)
        if t == OBJ:
            if v is None:
                self.out.append(1)
                return
            self.out.append(0)
            at = len(self.out)
            self.out += b"\0\0\0\0"
            cls, fields = v
            self.out += s(cls) + struct.pack("<H", len(fields))
            for name, ft, fv in fields:
                self.out += s(name)
                self.typed(ft, fv)
            struct.pack_into("<I", self.out, at, len(self.out))
        elif t in (OBJS, LIST):
            at = len(self.out)
            self.out += b"\0\0\0\0" + struct.pack("<I", len(v))
            for x in v:
                self.typed(OBJ, x)
            struct.pack_into("<I", self.out, at, len(self.out))
        else:
            self.out += scalar(t, v, True)

    def encode(self, root):
        self.out += struct.pack("<3I", 0x1B7C2FDD, 0, 22)
        self.typed(OBJ, root)
        return bytes(self.out)


def qimage(width, height, rgba):
    def qs(t):
        b = t.encode("utf-16-be")
        return struct.pack(">I", len(b)) + b

    props = [("width", 3, struct.pack(">I", width)), ("height", 3, struct.pack(">I", height)),
             ("pitch", 4, struct.pack(">q", width * 4)), ("format", 10, qs("RGBA8")),
             ("dataSizeRaw", 5, struct.pack(">Q", len(rgba))), ("compression", 10, qs("none"))]
    out = struct.pack(">I", len(props))
    for k, t, v in props:
        out += qs(k) + struct.pack(">IB", t, 0) + v
    return out + rgba


here = os.path.dirname(os.path.abspath(__file__))
with h5py.File(os.path.join(here, "painter_tiny.spsm"), "w", libver="earliest") as f:
    f["preset.bin"] = np.frombuffer(V0().encode(preset("Tiny Old", 22)), dtype=np.uint8)
    f["preset_1.bin"] = np.frombuffer(V1().encode(preset("Tiny", 88)), dtype=np.uint8)
    px = bytes([255, 0, 0, 255, 0, 255, 0, 255, 0, 0, 255, 255, 255, 255, 255, 128])
    f["resources/bb.image"] = np.frombuffer(qimage(2, 2, px), dtype=np.uint8)
    # Painter's metadata keeps its preview as a WebP file somewhere inside; a 4x4 orange square stands in.
    webp = io.BytesIO()
    Image.new("RGBA", (4, 4), (255, 128, 0, 255)).save(webp, "WEBP", lossless=True)
    f[".alg_meta/preset.bin"] = np.frombuffer(bytes(4) + b"previews" + webp.getvalue() + bytes(2), dtype=np.uint8)

# A smart mask: a curvature generator, then levels over it.
levels = o("DataActionFilterLevels", ("enabled", BOOL, True), ("label", STR, ""),
           ("levels", OBJ, o("DataLevels", ("gamma", VEC3, [1.0, 1.0, 1.0]), ("inputMaximum", VEC3, [0.8, 0.8, 0.8]),
                             ("inputMinimum", VEC3, [0.2, 0.2, 0.2]), ("outputMaximum", VEC3, [1.0, 1.0, 1.0]),
                             ("outputMinimum", VEC3, [0.0, 0.0, 0.0]), ("uid", U64, 30))), ("uid", U64, 31))
smart_mask = o("DataSmartMask", ("actionsStack", OBJ, stack([fill([procedural("/mg_curvature/mg_curvature?version=aa.sbsar", [])], [blend(2, 0)]), levels])),
               ("preset_dataversion", I32, 88), ("uid", U64, 1), ("uuid", STR, "22222222-2222-3333-4444-555555555555"))
with h5py.File(os.path.join(here, "painter_tiny.spmsk"), "w", libver="earliest") as f:
    f["preset.bin"] = np.frombuffer(V1().encode(smart_mask), dtype=np.uint8)

# A brush preset: stamp settings and a scratches tip.
emitter = o("DataEmitterStroke", ("followPath", BOOL, True), ("positionJitter", F32, 0.25), ("pressureFlowEnabled", BOOL, True),
            ("pressureSizeEnabled", BOOL, False), ("rotation", F32, 0.0), ("rotationJitter", F32, 3.1415927), ("spacing", F32, 0.1),
            ("uid", U64, 40))
stamp = o("DataBrushStamp", ("brushSizeJitter", F32, 0.1), ("flow", F32, 0.5), ("flowJitter", F32, 0.48), ("stampsEmitter", OBJ, emitter),
          ("uid", U64, 41))
tip = procedural("/alpha_scratches_01/alpha_scratches_01?version=cc.sbsar", [])
paint_action = o("DataActionPaint", ("brush", OBJ, stamp), ("enabled", BOOL, True), ("sourceTransparent", OBJS, [tip]), ("uid", U64, 42))
tool = o("DataToolPreset", ("preset", OBJ, paint_action), ("preset_dataversion", I32, 88), ("type", I32, 2), ("uid", U64, 1),
         ("uuid", STR, "33333333-2222-3333-4444-555555555555"))
with h5py.File(os.path.join(here, "painter_tiny.sppr"), "w", libver="earliest") as f:
    f["preset.bin"] = np.frombuffer(V1().encode(tool), dtype=np.uint8)
