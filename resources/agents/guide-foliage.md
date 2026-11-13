# Trees and plants in Lila, for agents

The Foliage extension (enabled by default) grows trees: a trunk with recursive branches, canopy blobs on the branch tips and a leaf mesh scattered over them, bent onto the canopy. The scene is cm, Y up, but **tree settings hold lengths in metres** (`'trunk.height': 7.0` is 7 m). The same settings and seeds always give the same tree, and the build takes well under a second.

## Recipes

- A few trees of a kind: one leaf card, then `treeBuild` per tree with a preset and its own seeds. See "Windswept pines".
- A broadleaf: `treeBuild(preset='Kaede Mature', leaf=leaf)`. Conifer: `Sugi ...`. Sparse, sculptural pine: `Akamatsu ...`. A bush: see "Presets".
- Change a tree: build it again with new `settings=` (or `treeRebuild(tree)` to reproduce it). Read what it was built with: `treeSettings(tree)`.
- Leaves: `treeLeafPlane(settings={'plane.length': 0.2, ...})`, or your own modelled leaf or cluster. Detail levels: several leaf meshes, densest first. See "Leaves and cards".
- Textures: `treeTextureSet(tree, season='autumn')` makes bark and leaf texture sets from the species palette. See "Textures".
- Check: `meshInfo` per part for triangles and size, `meshCheck(part, expectShells=0)` for mesh problems, then `captureView(display='shaded', frameSelection=True, isolate=True)` from the front and three-quarter. See "Check it".
- To Unreal: `unrealMap(tree, contentPath='/Game/Foliage/SM_Pine')` then `unrealSync(tree)`; many copies with `instance` and `unrealPlaceInstances`; `treeExport` for the bake-ready high and low FBX pair. See "Export".
- Shrubs and grass from a modelled mesh: `bladeExtrude` and friends. See "Blades".

## Windswept pines

```python
import math

leaf = treeLeafPlane(settings={'plane.length': 0.2, 'plane.width': 0.08, 'plane.name': 'pineTuft'})
wind = [1.0, 0.0, 0.0]  # the way the crowns lean, on the ground plane

def lean_downwind(tree, wind):
    # The lean direction is random per trunk seed: turn the tree so its crown points downwind.
    tips = treeBranchTips(tree)
    base = xform(tree, q=True, ws=True, t=True)
    cx = sum(t['point'][0] for t in tips) / len(tips) - base[0]
    cz = sum(t['point'][2] for t in tips) / len(tips) - base[2]
    setAttr(tree + '.rotate', [0, math.degrees(math.atan2(wind[0], wind[2]) - math.atan2(cx, cz)), 0])

trees = []
for i, (x, z) in enumerate([(0, 0), (900, 300), (-700, 500)]):
    tree = treeBuild(preset='Akamatsu Mature', leaf=leaf, name=f'pine{i}', settings={
        'trunk.seed': 11 + i, 'leaf.seed': 11 + i,
        'trunk.lean': 22.0, 'trunk.gnarl': 26.0, 'trunk.up_pull': 0.1,
        'trunk.height': 7.0 + i, 'leaf.align': [0.7, 1.0]})
    setAttr(tree + '.translate', [x, 0, z])
    lean_downwind(tree, wind)
    trees.append(tree)
select(trees)
captureView(view='front', display='shaded', frameSelection=True)
```

- `treeBuild` returns the root, `<name>` with children `<name>_trunk` and `<name>_leaves`. Move, turn and scale the root; the trunk base is at its origin.
- Vary trees with `trunk.seed` (shape) and `leaf.seed` (leaf scatter); vary size with `trunk.height` and `trunk.radius`. Same seeds, same tree.
- Windswept: `trunk.lean` (degrees the trunk tilts), `trunk.gnarl` (bends along limbs), low `trunk.up_pull` (branches stop reaching up), `leaf.align` toward 1 (needle tufts swing along the branch).
- `treeBranchTips(tree)` returns the tips in world space without rebuilding: `[{point, heading, radius}]`.

## Presets

`treePresets()` lists them; `treePresetSettings(name)` returns every setting (metres). `settings=` overrides on top of `preset=`.

| Preset | Tree | Height | Leaves |
| --- | --- | --- | --- |
| `Sugi Young`, `Mature`, `Ancient` | Japanese cedar: tall, narrow, near-horizontal branches shortening to the top | 7, 14, 18 m | 1400 to 3600 |
| `Akamatsu Young`, `Mature`, `Ancient` | Japanese red pine: sparse, leaning, heavy gnarl, needle tufts | 4.5, 7, 8.5 m | 550 to 1500 |
| `Kaede Young`, `Mature`, `Ancient` | Japanese maple: rounded spreading broadleaf crown | 2.8, 5, 6.5 m | 900 to 3400 |
| `Default` | Generic 6 m tree, 400 leaves | 6 m | 400 |

The species (first word of the preset) picks the bark and leaf palette for `treeTextureSet` and the export's `_palette.json`; any other preset name uses the Kaede palette. Pick the nearest preset and override: a spruce is `Sugi` with `trunk.length_falloff` up, an oak is `Kaede Ancient` with `trunk.height` and `trunk.radius` up.

A bush (about 1.7 m tall, 2.4 m across, 19,200 leaf triangles): branches from the ground, longer than their parent, small leaves wrapped hard.

```python
bush = treeBuild(preset='Kaede Young', leaf=leaf, name='bush', settings={
    'trunk.height': 0.5, 'trunk.radius': 0.03, 'trunk.branch_start': 0.0, 'trunk.branches': 7,
    'trunk.branch_angle': 50.0, 'trunk.length_ratio': 1.2, 'trunk.up_pull': 0.3,
    'blob.blob_radius': 0.3, 'leaf.count': 1600, 'leaf.size': 0.1, 'leaf.wrap': [0.7, 1.0]})
```

`treeSavePreset('Windswept Pine', preset='Akamatsu Mature', settings={...})` stores one in the preferences; built-in names are refused.

## Settings that matter

Keys are `section.name`. Ranges are `[low, high]`, each leaf drawing a value between them.

| Want | Settings |
| --- | --- |
| Taller, thicker | `trunk.height` (m), `trunk.radius` (m, base), `trunk.taper` (0 to 1) |
| More or fewer limbs | `trunk.levels` (branch depth), `trunk.branches` (per level), `trunk.branch_start` (0 to 1 up the parent) |
| Crown shape | `trunk.branch_angle` (degrees off the parent), `trunk.length_ratio` (child length), `trunk.length_falloff` (shorter higher up: a cone), `trunk.up_pull` (-0.95 to 0.95) |
| Lean, character | `trunk.lean` (degrees, random direction per seed), `trunk.gnarl`, `trunk.angle_variance` |
| Denser foliage | `leaf.count`, `blob.blob_radius` (m, 0 is auto), `leaf.size` (m, leaf length; 0 keeps the modelled size), `leaf.scale` |
| Bunches with gaps | `leaf.clumps` (count, 0 is even), `leaf.clump_radius`, `leaf.clump_falloff` |
| Leaves hug the crown | `leaf.wrap` (0 to 1), `leaf.normal_blend` (canopy normals for soft shading), `leaf.tilt`, `leaf.curl` (degrees) |
| Leaves toward the tips | `leaf.gather` (0 to 1), `leaf.align` (0 to 1, along the branch) |
| No leaves underneath | `leaf.min_normal_up` (-1 keeps all, 0 drops downward-facing spots) |
| Chunky, stylised | `global.style` (0 to 3): fatter trunk and leaves, fewer of them |
| Trunk detail | `trunk.sides`, `trunk.sections`; `trunk.uv_mode` `packed` (bake) or `tiled` (repeating bark, `trunk.uv_tile` m) |
| Z-up scene | `global.up_axis` `'Z'` (`auto` follows the scene) |

Canopy blobs are deleted after the leaves are scattered; `blob.keep_canopy` keeps them hidden under the tree.

## Leaves and cards

- `treeLeafPlane(settings={'plane.length': 0.12, 'plane.width': 0.05, 'plane.segments': 6, 'plane.taper': 0.6, 'plane.double': False, 'plane.name': 'leafPlane'})` makes a card along +Z, base at the origin, 0 to 1 UVs, segmented so curl and wrap can bend it. Values shown are the defaults (metres).
- Your own leaf or leaf cluster works as `leaf=`: model it with UVs, base at the pivot, lying flat with its longest axis along the leaf. A leaf modelled diagonally needs `leaf.leaf_up` and `leaf.leaf_forward` (`'X'`, `'-Y'`, ...).
- Detail levels: `leaf=[dense, light]`, densest first. The leaves become a group with `<name>_leaves_lod0`, `_lod1`, placed vertex for vertex on each other.
- Every leaf carries the `leafShade` colour set: R exposure (up-facing and proud is light), G a value per leaf, B a value per bunch. Leaf materials and `treeTextureSet` read it.
- `treeOpacityMap(leaf=leaf, settings={'export.path': folder, 'export.name': 'Pine'})` writes `Pine_opacity.png` from the leaf's UV shells, a cutout for a leaf modelled to shape. A plain card wants a painted leaf texture instead.
- `treeExportLeaf(leaf=leaf, settings={'export.path': folder, 'export.name': 'Pine'})` writes the leaf alone (`Pine_Leaf_Lo.fbx`, `_Hi.fbx`, json, opacity) for particle cards.

## Textures

`treeTextureSet(tree, season='summer', target='unreal')` makes two texture sets: `<tree>_Bark` on the `Trunk` material (the species' bark tones over cavity, edges and AO) and `<tree>_Leaves` on `Leaves` (the season's tones over `leafShade`). Seasons: `spring`, `summer`, `autumn`, `lateAutumn`. Returns `{sets, bakeProject, season, species, warnings}`.

- The bark's cavity, edge and AO layers read baked maps. `treeExport` the tree first and `treeTextureSet` makes the bake project from it (`bakeProject`, AO, curvature, cavity and the leaf colour set); `createBakeFromGenerator(tree, kind='tree')` makes one on its own. Without an export the maps read as defaults and `warnings` says so; pass `bake=False` to skip that. The leaf layers need no bake.
- `replace=True` rebuilds sets of the same names.
- Look at one: `texEvaluate(set, resolution=256, channels=['baseColor'], folder=...)`. Write the maps: `texExport(set, folder=...)`. Send to Unreal: `unrealSendTextureSet(set)`.

## Check it

```python
for part in listRelatives(tree):
    print(part, meshInfo(part)['triangles'])
select([tree])
captureView(view='front', display='shaded', frameSelection=True, isolate=True)
captureView(view='three-quarter', display='shaded', frameSelection=True, isolate=True)
```

- `frameSelection` frames the selection and `isolate` hides the rest, so trees built at the origin don't overlap. One front capture shows lean and silhouette; the three-quarter shows the crown's depth.
- Budgets: `Akamatsu Mature` is about 3,200 trunk and 10,800 leaf triangles (900 leaves of the default 6-segment card). Lower `leaf.count` or `plane.segments` for game trees; raise `leaf.count` with `leaf.size` down for denser crowns.
- `treeSettings(tree)` returns `{tree, values, preset, sources}`: what it was built with, including the leaf mesh. It is null for anything the generator did not build.
- The trunk is one shell per limb and the leaves one shell per leaf, so give `meshCheck` `expectShells=0`; it expects one shell by default. `worldBoundsMin` and `worldBoundsMax` give the real height and spread in cm.

## Export

- Straight to Unreal (LilaLink connected): `unrealMap(tree, contentPath='/Game/Foliage/SM_Pine01', kind='static')` maps the root as one static mesh (its meshes combined, `Trunk` and `Leaves` slots), and `unrealSync(tree)` sends it now; saving the scene sends it again. `nanite=True` for Nanite foliage. Many copies of one tree: `copies = instance(tree)` (move each), then `unrealPlaceInstances([tree] + copies, mode='hism')` sends the trunk and leaves once each (`SM_<tree>_trunk`, `SM_<tree>_leaves`) and places them as instanced components.
- Plain FBX: `exportFbx('D:/Out/SM_Pine01.fbx', tree)` (UE5 axes by default).
- Bake pair: `treeExport(tree, settings={'export.path': 'D:/Bakes', 'export.name': 'Pine'})` writes `Pine_Lo.fbx` (lowest detail), `Pine_Hi.fbx` (subdivided, merged), `Pine_tree.json` and `Pine_palette.json` (written once, never overwritten), and assigns the `Trunk`, `Canopy` and `Leaves` materials. The export path is stored on the tree for later calls. `treeBakeHandoff(tree)` returns the paths, materials, face counts and ray distances (`rays`, cm per part: use `out99` and `in99`) a bake needs without writing anything.
- In Unreal the leaf material needs Masked blend mode with the leaf texture's alpha, and Two Sided on unless `plane.double` was on.

## Blades

For shrubs, grass clumps and ferns modelled by hand: grow blades from faces, then vary them.

```python
plane = polyPlane(width=10, depth=10, subdivisionsX=4, subdivisionsZ=4)[0]
bladeExtrude(plane, faces=list(range(8)), thickness=3.0, offset=0.2, divisions=2)
randomizeBladeExtrude(plane, fields={'thickness': [1.0, 5.0]}, seed=7)
selectBladeFaces(plane, stems=True)
addBladeDeformer(kind='bend', low=20, high=60, seed=5)
```

Blade lengths are cm. `bladeExtrudeInfo(mesh)` lists the blades, `setBladeExtrude(mesh, blades=[3], thickness=2.5)` sets one, `randomizeBladeDeformers` and `deleteBladeDeformers` work on the deformers. Seed 0 is random every run; any other seed repeats.

## When it fails

| Error | Fix |
| --- | --- |
| `Select the leaf mesh, or pass leaf=.` | Pass `leaf=` (make one with `treeLeafPlane()`) |
| `No sample points survived the filters` | Lower `leaf.min_distance`, lower `leaf.min_normal_up` toward -1, or raise `leaf.clump_radius` |
| `Blob Clip removed the whole blob` | Lower `blob.blob_clip` below 0.95 |
| `Set an export path and a file name first.` | Pass `export.path` and `export.name` in `settings=` |
| `Select a tree built by this tool.` | Pass the root `treeBuild` returned, or any part of it |
| Tree is huge or tiny | Settings lengths are metres: `trunk.height` 7, not 700 |
| Leaves stick out sideways | The leaf mesh's axes were misread: set `leaf.leaf_up` and `leaf.leaf_forward` |
