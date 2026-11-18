"""Texture set helpers: build layer stacks, evaluate channels as numpy arrays, export, and send to Unreal.

    import lila.texture as tex
    s = tex.create("RockTex", material="Rock", resolution=1024)
    tex.add_fill(s, "Moss", {"baseColor": "#4A6B2A", "roughness": 0.8}, generator="genMoss")
    base = tex.channel(s, "baseColor")          # (H, W, 4) float32, row 0 is the top scanline
    rough = tex.channel(s, "roughness", gray=True)  # (H, W)
    tex.export(s, incremental=True)
    tex.send_to_unreal(s)

Channels are evaluated by texEvaluate into 32-bit EXR files in a temporary folder and read back, so values are the graph's
floats. Every function that changes a set runs a command, so it is undoable and echoed like the panels' edits.
"""

import os
import shutil
import tempfile

from . import ops
from .bake import read

__all__ = ["sets", "create", "settings", "set_settings", "stack", "add_layer", "add_fill", "add_mask", "add_generator", "apply_smart_material",
           "smart_materials", "evaluate", "channel", "output", "export", "export_image", "export_psd", "import_images", "lint",
           "paint_stroke", "send_to_unreal"]


def sets():
    """Names of the texture sets in the scene."""
    return ops.texSets()


def _set(name):
    if name is not None:
        return name
    names = ops.texSets()
    if len(names) != 1:
        raise ValueError("no texture sets" if not names else "several texture sets; name one")
    return names[0]


def create(name, material=None, resolution=None, settings=None):
    """Makes a texture set (a Material Output and the standard channels) and returns its name. material (a name or a list) picks it as its Texture Set."""
    flags = {"settings": dict(settings)} if settings else {}
    if material is not None:
        flags["materials"] = [material] if isinstance(material, str) else list(material)
    if resolution:
        flags["resolution"] = resolution
    return ops.texCreateSet(name, **flags)


def settings(set=None):
    """The set's settings: channels, width, height, tiles, normalConvention, bakeProject, exportPreset."""
    return ops.texSetSettings(_set(set), {})


def set_settings(set=None, **changes):
    """Changes settings (undoable) and returns them all."""
    return ops.texSetSettings(_set(set), changes)


def stack(set=None):
    """The layer stack as JSON (texLayerStack): layers bottom first with their channels and masks, the base and the maps it reads."""
    return ops.texLayerStack(_set(set))


def add_layer(set, kind, **flags):
    """Adds a layer (fill, paint, procedural, folder, instance, filter) and returns its id. flags as texAddLayer takes them."""
    return ops.texAddLayer(_set(set), kind, **flags)["id"]


def add_fill(set, name, material, generator=None, bindings=None, values=None, **flags):
    """A fill layer with channel values, masked by a generator (genEdgeWear, genMoss, ...) when given. Returns the layer id."""
    layer = add_layer(set, "fill", name=name, material=material, **flags)
    if generator:
        add_generator(set, layer, generator, bindings=bindings, values=values)
    return layer


def add_mask(set, layer, kind, **flags):
    """Adds a mask (fill, generator, filter, levels, paint, anchor) to a layer and returns its id."""
    return ops.texAddMask(_set(set), layer, kind, **flags)["id"]


def add_generator(set, layer, type, bindings=None, values=None, **flags):
    """Adds a generator mask; bindings map its inputs to mesh maps ({"position": "vertexColor"}). Returns the mask id."""
    if bindings:
        flags["bindings"] = bindings
    if values:
        flags["values"] = values
    return ops.texAddGenerator(_set(set), layer, type, **flags)["id"]


def apply_smart_material(set, name, **flags):
    """Applies a smart material (or a smart mask with layer=) by name or path. Returns texApplySmartMaterial's result."""
    return ops.texApplySmartMaterial(_set(set), name, **flags)


def smart_materials():
    """Every smart material and mask in the libraries."""
    return ops.texSmartMaterials()


def _read_folder(result):
    out = {}
    for f in result["files"]:
        stem = os.path.splitext(os.path.basename(f))[0]
        out[stem] = read(f)
    return out


def evaluate(set=None, channels=None, resolution=None, backend=None):
    """{channel: (H, W, 4) float32 array} for the set's channels (all by default), at its tile size or resolution. UDIM sets give
    {"channel.1001": ...} per tile."""
    s = _set(set)
    folder = tempfile.mkdtemp(prefix="lila_tex_")
    try:
        flags = {"folder": folder, "format": "exr", "bits": 32}
        if channels:
            flags["channels"] = list(channels)
        if resolution:
            flags["resolution"] = resolution
        if backend:
            flags["backend"] = backend
        r = ops.texEvaluate(s, **flags)
        prefix = s + "_"
        return {k[len(prefix):] if k.startswith(prefix) else k: v for k, v in _read_folder(r).items()}
    finally:
        shutil.rmtree(folder, ignore_errors=True)


def channel(set, name, resolution=None, gray=False, backend=None):
    """One channel as an (H, W, 4) float32 array, or (H, W) with gray=True (its red)."""
    a = evaluate(set, [name], resolution, backend)
    key = name if name in a else sorted(k for k in a if k.startswith(name + "."))[0]
    return a[key][..., 0] if gray else a[key]


def output(set, source, resolution=None, udim=None, backend=None):
    """Any node output ("Noise.out") or channel as an (H, W, 4) float32 array, for one tile."""
    folder = tempfile.mkdtemp(prefix="lila_tex_")
    try:
        path = os.path.join(folder, "out.exr")
        flags = {"bits": 32}
        if resolution:
            flags["resolution"] = resolution
        if udim:
            flags["udim"] = udim
        if backend:
            flags["backend"] = backend
        ops.texExportImage(_set(set), source, path, **flags)
        return read(path)
    finally:
        shutil.rmtree(folder, ignore_errors=True)


def export(set=None, preset=None, folder=None, incremental=False, **flags):
    """Writes the set through an export preset (unreal by default, as the set says). Returns {files, outputs, written, unchanged, ...}."""
    if preset:
        flags["preset"] = preset
    if folder:
        flags["folder"] = folder
    return ops.texExport(_set(set), incremental=incremental, **flags)


def export_image(set, source, path, **flags):
    """Writes a channel or node output to an image file."""
    return ops.texExportImage(_set(set), source, path, **flags)


def export_psd(set=None, channels=None, folder=None, **flags):
    """One layered PSD per channel of the layer stack. Returns {files, warnings}."""
    if channels:
        flags["channels"] = list(channels)
    if folder:
        flags["folder"] = folder
    return ops.texExportPsd(_set(set), **flags)


def import_images(set, *files, **flags):
    """Images (PSD layers each) as paint layers (kind="fill" for fill layers). Returns {layers, files}."""
    return ops.texImportImage(_set(set), *files, **flags)


def lint(set=None, fix=False):
    """materialLint on the set: {passed, failures, warnings, checks}."""
    return ops.materialLint(_set(set), fix=fix)


def paint_stroke(set, stroke, **flags):
    """Paints one stroke record (texPaintStroke; needs the renderer). Returns {layer, stroke, dabs, pages}."""
    return ops.texPaintStroke(_set(set), stroke=stroke, **flags)


def send_to_unreal(set=None, **flags):
    """Exports and sends the set to Unreal (unrealSendTextureSet): only what changed unless all=True."""
    return ops.unrealSendTextureSet(_set(set), **flags)
