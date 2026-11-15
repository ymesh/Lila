# Materials in Lila, for agents

A material's textures come from a texture set: a node graph (like Substance Designer's) whose Material Output channels are the maps. The set drives the material of the same slot: every mesh with that material shows the graph live in the viewport, with no evaluate step. Maps are exported or sent to Unreal from the set.

## Recipes

- A procedural material from a description: one `texBuildGraph` spec, below.
- Put it on meshes: `assign=['Crate', 'Barrel']` on the same call (creates the material when missing).
- Wear, dirt and grime that follow the mesh's shape: give the set a bake project and use the `mesh*` nodes, see "Mesh maps".
- A layered, Painter-style set (fills, masks, smart materials, paint): `texAddLayer`, `texAddGenerator` (edge wear, dirt, dust, moss, rust...), `texApplySmartMaterial`; `texLayerStack(set)` shows it. A graph built with `texBuildGraph` replaces the stack.
- Files: `texExport(set)` through the set's export preset (`unreal` packs ORM). Unreal: `unrealSendTextureSet(set, wait=True)`.

## The fast path: one spec, one call

`texBuildGraph(spec)` builds a material's whole graph from compact JSON, creating its texture set when missing. Nothing changes unless the whole spec is valid; the error lists every problem at once with the valid names (outputs, inputs, parameters, close node types), so fix them all and call again.

```python
spec = {
    "material": "MossyStone",                  # the material slot; the set is MossyStoneTex
    "settings": {"width": 1024, "height": 1024},
    "nodes": [
        {"name": "Cells", "type": "voronoi", "params": {"scale": 6}},
        {"name": "Grime", "type": "dirt", "params": {"amount": 0.4}},
        {"name": "StoneCol", "type": "colorize", "params": {"gradient": {"stops": [[0.0, [0.18, 0.17, 0.16, 1]], [1.0, [0.45, 0.43, 0.4, 1]]]}}},
        {"name": "Stone", "type": "material", "values": {"roughness": 0.8}},
        {"name": "Moss", "type": "material", "values": {"baseColor": [0.12, 0.25, 0.05, 1], "roughness": 0.95}},
        {"name": "Mix", "type": "materialBlend"},
        {"name": "Nrm", "type": "heightToNormal"},
    ],
    "links": [
        "Cells.f1 -> StoneCol.input", "StoneCol.out -> Stone.baseColor", "Cells.f1 -> Stone.height",
        "Stone.material -> Mix.a", "Moss.material -> Mix.b", "Grime.out -> Mix.mask",
        "Mix.material -> Output.material", "Cells.f1 -> Nrm.height", "Nrm.normal -> Output.normal",
    ],
}
r = texBuildGraph(spec, assign=["Rock"])   # {set, material, channels, nodes, links, passes, error?}
```

Spec rules:

- `material` names the material slot; `set` defaults to the set already bound to the material, else `<material>Tex` without the material's `M_` prefix (`M_Rock` gets `RockTex`; a set and its material can't share a name). Rebuilding with the same material replaces the graph and keeps the set's settings unless `settings` changes them.
- `nodes[].type` is a type from the catalogue below. `params` are the node's settings; `values` are unlinked input values. A node named `Output` of type `materialOutput` is added when missing.
- Material Output inputs: `baseColor`, `roughness`, `metallic`, `normal`, `height`, `ao`, `emissive`, `opacity`, and `material` (a whole material; channels linked directly win over it). Unlinked channels take their `values` on Output.
- Links are `"Node.output -> Node.input"`, `[from, to]` or `[fromNode, fromSocket, toNode, toSocket]`. An input takes one link.
- Colours are linear `[r, g, b, a]`. Normals are tangent space, OpenGL (green up); exports convert per preset.
- `settings` keys: `width`, `height`, `tiles` (UDIMs), `channels`, `exportPreset`, `bakeProject`, `normalConvention`, `heightDepth`. `texSetSettings(set, {...})` changes them later.
- Positions are laid out in columns when omitted.

## Building blocks

- Patterns with per-piece outputs: `bricks`, `tiles`, `planks` and `hex` give a `mask`, a `random` value per piece and each piece's own uv; `scales` gives `height` and `random`, `weave` gives `height`, `mask` and `direction`. Drive colour variation from `random` through `colorize` or `hsv`.
- Noise: `perlin`, `voronoi` (`f1`, `f2`, `edge`, `random`), `clouds`, `cells`, `gabor`, `dirt`, `scratches`, `crystal`. All tile.
- Shaping: `levels`, `curves`, `threshold`, `math`, `blend`, `blur`, `warp`, `slopeBlur`, `erode`, `makeTileable`.
- Height to normal: `heightToNormal` (depth in cm; sizeU/sizeV the world size of the UV square). Combine normals with `normalBlend`.
- Whole materials: `material` bundles channels; `materialBlend` mixes two by a mask; `materialHeightBlend` lets one fill cracks or top bumps by height; `materialTiling` repeats one; `materialChannels` splits one.
- No UVs, no seams: `triplanar` projects any input from three axes.

## Mesh maps

The `mesh*` nodes read the set's bake: `meshAo`, `meshCavity`, `meshCurvature`, `meshThickness`, `meshHeight`, `meshWorldNormal`, `meshId`, `meshUvIslands`, `meshVertexColor`, `meshPanelLayout`. Set the bake project with `settings: {"bakeProject": "<project>"}`; without one they read (u, v, 0), not the mesh. `describe_texture_set` lists the maps the graph reads and which the bake doesn't make yet.

Typical masks: edge wear from `meshCurvature` through `levels`; dirt in crevices from `meshCavity` inverted times `meshAo`; moss on top from `meshWorldNormal` (y) times a noise.

## Checking

- `texBuildGraph`'s result has `error` when the graph doesn't compile; `texGraphInfo(set)` shows every node with resolved ports and values, and the plan.
- `material_lint(material=set)` checks physical ranges (albedo, metal reflectance, roughness, normal validity, tile seams); `fix=true` clamps what it can.
- Look at it: a capture of the mesh (`capture_turntable` or `captureView`) after the set has evaluated. `texEvaluate(set, channels=['baseColor'])` writes images to read when no viewport is running.
- Small edits without a rebuild: `texSetValue(set, 'Cells.scale', 8)`, `texConnect`, `texDisconnect`.
