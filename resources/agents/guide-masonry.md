# Masonry in Lila, for agents

Masonry lays walls and whole buildings stone by stone (courses, bonded corners, arches, columns, vaults, floors), then ruins, ages, checks and ships them to Unreal as instanced stones. Lengths are cm, Y is up, `collapse` and other directions are degrees about Y from +X. Commands are bare names in `execute_python` (`ops.<name>` works too). Every command returns stats: read them instead of capturing after each step.

## Recipes

- A ruined tower, wall or ring: `masonryWall(points=, closed=True, ...)`, then `ruin`, `age`, `masonryCheck`. See "Ruined round tower".
- A building with several parts (nave, tower, vault, colonnade, floors): one `masonryBuilding(spec=)`. See "Building from one spec".
- Stone over a blockout (keep, tapered tower): `masonryFromMesh('Blockout', ...)`. See "Over a blockout".
- A wall on terrain: `masonryWall(points=, ground='Terrain')`. See "Walls on the ground".
- Change it: `setAttr('<modifier>.<flag>', value)` on a wall; `masonrySpec` then `masonryBuilding(node=, spec=)` on a building.
- Ruin one part or paint damage: `ruin(..., regions=[...])`, `ruinPaint`. See "Ruin".
- Compare ruins: `masonryWedge(mesh, vary={'left': [0.75, 0.5, 0.25]})`. See "Variants".
- Check it: `masonryCheck(mesh)` until `ok`, then one capture. See "Check".
- Ship it: `masonryShip(mesh, wait=True)`. See "To Unreal".
- The MCP prompt `make_ruin` walks through spec, ruin, age, check and ship.

## Ruined round tower

```python
import math
n, r = 16, 300
pts = [[r * math.cos(i / n * math.tau), 0, r * math.sin(i / n * math.tau)] for i in range(n)]
t = masonryWall(points=pts, closed=True, name='Tower', bond='ashlar', height=1200, thickness=90,
                stoneLength=50, stoneHeight=28, stoneDepth=35, joint=1.5, batter=1, cornice=2,
                openings=[{'name': 'door', 'at': 150, 'width': 120, 'height': 240, 'arch': 'round'},
                          {'name': 'window', 'at': 1000, 'width': 60, 'height': 140, 'sill': 650, 'arch': 'pointed'}])
ruin(t['mesh'], left=0.5, collapse=90, gaps=1)
age(t['mesh'], age=0.8, moss=0.6, ivy=0.4)
c = masonryCheck(t['mesh'], maxStones=20000)
t['stones'], t['regions'], c['ok'], c['issues']
```

- Turns gentler than `cornerAngle` (30 degrees) curve the wall, so 16 points make a round tower; sharper turns are bonded corners.
- `at` is cm along the footprint from its first point; the loop's length is about `2 * pi * r`. An opening past the end, or above the top, is skipped.
- A wall from `masonryWall` is one region, `wall_0` (or `region='tower'`), with its openings under it (`wall_0/door`). Name the parts of a whole building with `masonryBuilding`.
- The result: `mesh`, `modifier`, `group`, `stones`, `courses`, `roles`, `regions` (stone count per region), `stoneTriangles`, `collisionHulls`, `ruin`, `bounds` (`min`, `max` of the stones), `seconds`.

## Building from one spec

```python
nave = [[0, 0, 0], [1600, 0, 0], [1600, 0, 800], [0, 0, 800]]
tower = [[-640, 0, 150], [-140, 0, 150], [-140, 0, 650], [-640, 0, 650]]   # clear of the nave's west wall
b = masonryBuilding(name='Chapel', spec={
    'stone': {'length': 60, 'height': 30, 'depth': 35, 'joint': 1.5},
    'bond': 'ashlar',
    'walls': [
        {'name': 'nave', 'points': nave, 'closed': True, 'height': 900, 'thickness': 90, 'cornice': 1,
         'openings': [{'name': 'east_window', 'at': 2000, 'width': 120, 'height': 300, 'sill': 350, 'arch': 'pointed'},
                      {'name': 'south_window', 'at': 1000, 'width': 100, 'height': 280, 'sill': 350, 'arch': 'pointed'}],
         'buttresses': [{'at': 400}, {'at': 800}, {'at': 1200}]},
        {'name': 'tower', 'points': tower, 'closed': True, 'height': 1800, 'thickness': 100, 'bands': [900], 'cornice': 2,
         'openings': [{'name': 'west_door', 'at': 1750, 'width': 160, 'height': 320, 'arch': 'round'},
                      {'name': 'belfry', 'at': 250, 'width': 90, 'height': 200, 'sill': 1450, 'arch': 'round'}]}],
    'vaults': [{'name': 'nave_vault', 'kind': 'barrel', 'from': [45, 900, 400], 'to': [1555, 900, 400], 'span': 710}],
    'floors': [{'name': 'paving', 'points': nave, 'y': 0, 'thickness': 10, 'surface': 'flags'}]})
ruin('Chapel', left=0.5, collapse=0, gaps=1)
age('Chapel', age=0.8, moss=0.6)
b['regions'], masonryCheck('Chapel')['ok']
```

- Every wall, opening, buttress, column, stair, vault, arch and floor is a region named by its `name` (`nave`, `nave/east_window`, `portico/column_3`). Name them the way the user talks about them.
- Edit and rebuild: `s = masonrySpec('Chapel')['spec']`, change `s`, `masonryBuilding(node='Chapel', spec=s)`. Ruin and age settings stay.
- Walls must not share a line: two walls whose thickness overlaps fail `masonryCheck` with interpenetrating stones. Leave the tower's wall clear of the nave's (half of each thickness plus a gap).
- Vaults and freestanding arches spring at the y you give: put them on a wall top or column (a barrel vault's `span` is the inside width), or `masonryCheck` reports floating stones.
- A floor's outline can follow the walls' points: the slab and its paving are cut back to the walls' inside faces. A roof whose underside is at the wall top sits on the walls whole.
- Roofs are floors at the top: `'surface': 'tiles'` with `pitch` and `uphill`.
- Large builders: `'scale': 8` multiplies every length, stones included. Scale the stones with the building, or it reads as human-sized.

### Spec keys

Full schema with defaults: resource `lila://masonry/spec-schema`. Lengths in cm before `scale`; points `[x, y, z]` with y the ground.

| Key | Fields |
| --- | --- |
| top level | `scale` (1), `seed` (1), `stone` {`length` 21.5, `height` 6.5, `depth` 10.25, `joint` 1}, `bond`, `look` {`bevel`, `wear`, `variants`, `tile`, `jitter`, `protrude`} |
| `walls[]` | `name`, `points` (required), `closed`, `height` (200), `heights` (one per point), `thickness` (21.5), `batter` (degrees), `bond`, `bands` [cm], `bandProject`, `cornice` (courses), `corniceProject`, `cornerAngle` (30), `openings[]`, `buttresses[]` |
| `openings[]` | `name`, `kind` (door, window, niche), `at` (required, cm along the wall), `width` (100), `height` (200, arch included), `sill` (windows 100), `arch` (lintel, round, pointed, segmental, flat), `rise`, `ring`, `sillStone` |
| `buttresses[]` | `name`, `at`, `length`, `width`, `height`, `top`, `batter` |
| `columns[]` | `name`, `at` [x, y, z], `height` (600), `diameter` (60), `drums`, `square`, `plinth`, `capital`, `entasis` |
| `colonnades[]` | column fields plus `from`, `to`, `count`; regions `<name>/column_<k>` |
| `stairs[]` | `name`, `at` (bottom step's front edge middle), `direction` (uphill), `width` (200), `steps` (8), `rise` (18), `going` (30) |
| `vaults[]` | `name`, `kind` (barrel, groin, dome); barrel `from`, `to`, `span`, `profile`, `rise`; dome and groin `at`, `radius`; `ring`, `oculus` |
| `arches[]` | `name`, `at` (springing middle), `direction` (across the span), `span`, `kind`, `rise`, `ring`, `depth` |
| `floors[]` | `name`, `points` (y ignored), `y` (top), `thickness` (30), `surface` (slab, cobbles, flags, tiles), `size`, `pitch`, `uphill` |

Bonds: `running`, `stack`, `english`, `flemish`, `header`, `gardenWall`, `herringbone`, `ashlar`, `rubble`, `dryStone`. Stone defaults are a UK brick; for stone use something like 40 to 60 long, 20 to 30 high, 25 to 35 deep. A wall thinner than 1.5 stone depths is one stone thick.

## Over a blockout

```python
polyCube(name='KeepBlockout', width=800, height=1000, depth=600, position=[0, 500, 0])
k = masonryFromMesh('KeepBlockout', name='Keep', bond='ashlar', stoneLength=60, stoneHeight=30, stoneDepth=35, thickness=100, band=250)
```

The blockout is sliced into `band`-high sections (0: eight bands), walls are laid on each section's centre line, stepping in where it tapers, and the blockout is hidden. A hollow blockout gives its own wall thickness; a solid one uses `thickness` (0: a tenth of its narrower side). Every `masonryWall` flag works here.

## Walls on the ground

- Each footprint point's y is the ground there: courses stay level and step over slopes. `heights=[...]` (one per point) slopes or breaks the top.
- `ground='Terrain'` resamples the footprint every `groundStep` (0: two stone lengths) and drops it onto that mesh.

```python
w = masonryWall(points=[[-2500, 0, 4000], [0, 0, 3500], [2500, 0, 4200]], ground='Terrain', name='FieldWall',
                bond='dryStone', height=120, thickness=60, stoneLength=40, stoneHeight=20, stoneDepth=30)
ruin(w['mesh'], left=0.75, rubble=0.8)
```

- Sloped wall with a gate: `masonryWall(points=[[0, 0, 0], [1200, 150, 0]], heights=[400, 250], thickness=60, batter=2, stoneLength=40, stoneHeight=20, stoneDepth=25, openings=[{'name': 'gate', 'at': 600, 'width': 160, 'height': 260, 'arch': 'round'}])`.
- Building bases where walls meet terrain: `lila://guide/contacts` (`seamFind`, `seamScatter`, `seamCheck`).

## Ruin

- `ruin(mesh, left=0.5, collapse=90, gaps=1)`: `left` is the share of stone left standing (0.75, 0.5, 0.25). It stands lowest toward `collapse`; `slope` is how much lower; `roughness` the ragged top; `gaps` breaches to the ground; `survivors`, `loose`, `tip` the stones that hang on and tip; `rubble` the share of fallen stones heaped at the foot (the rest are gone); `settle=True` drops the rubble with the rigid-body solver (slower). `left` is met to within a few percent: the result's `left` is what stands.
- Rubble heaps at the foot and inside closed walls, spreading at a natural slope; a heap filling a tower stops well below its walls and the rest is gone.
- `ruin` sets the modifier's ruin inputs. A wall or building has one ruin: each call changes only the flags it gives, and the rest stay from earlier calls. `regions=['tower']` limits it to those regions until `regions=''` clears it.
- Extra local damage: `ruinPaint(mesh, points=[[x, y, z], ...], radius=150)` knocks out stones near the stroke (`strength=-1` holds them up instead; `clear=True` drops all strokes). Strokes add to the ruin.
- `structural=True` also drops what cannot stand (an arch without its keystone, overhangs).
- On objects that are not masonry (stones placed by hand), `ruin(['a', 'b'], left=0.4)` moves and deletes them in place.
- Result: `removed`, `survivors`, `loose`, `unsupported`, `rubble`, `slabPieces`, `fallenPieces`, `painted`, `left`, `stones`.

## Age

`age(mesh, age=0.8, moss=0.6)` gives every stone age, wet (near the ground, exposed tops) and moss (wet and exposed); damage comes from the ruin. `plaster=` leaves a render coat that peels high up and where damaged; `ivy=` climbs from the ground (`ivyHeight`, `leaf`). Plaster and ivy are their own mesh. Returns `stones`, `mossy`, `meanAge`, `meanMoss`, `meanWet`.

## Look and speed

- Machine-laid look: raise `jitter`, `wear`, `protrude`, `chips`. `variants` sets how many worn copies of each stone exist; `seed` re-rolls wander and wear, `ruinSeed` the break.
- `setAttr('<modifier>.detail', 2)` builds plain boxes while iterating; set it back to 0 for the final look.
- Stones are instances with attributes `role` (0 stretcher, 1 header, 2 bat, 3 closer, 4 filler, 5 voussoir, 6 keystone, 7 lintel, 8 sill, 9 jamb, 10 column, 11 step, 12 band, 13 slab, 14 rubble, 15 paving), `region`, `course`, `age`, `damage`, `wet`, `moss`, `loose` for nodes added after the Masonry node.

## Variants

`masonryWedge(mesh, count=3, vary={'left': [0.75, 0.5, 0.25]}, file='C:/tmp/wedge.png')` builds the variants in parallel and draws them in elevation into one contact sheet (`sheet`); each variant's settings and stats come back. `vary=['ruinSeed']` (the default) steps a setting by one per variant. `masonryWedge(mesh, vary=..., apply=1)` sets variant 1's settings on the modifier.

## Check

1. `masonryCheck(mesh, maxStones=, maxTriangles=)` returns `ok`, `issues`, `floating` and `floatingStones` (the first ten: region, role, at), `overlaps` (standing stones interpenetrating past `tolerance`, 0.1 cm) with `overlapRegions` (pairs per region, `nave + tower` across two) and `overlapPairs` (the first ten), `stacked` (rubble heaped above half the walls' height) and `rubbleHeight`, `rubbleOverlaps` (informational), `collisionHulls`, `stones`, `triangles`. Fix until `ok`.
2. Floating stones mean a vault, arch or column sits in mid-air in the spec: lower it onto a wall, or let `ruin` drop what has no support.
3. `masonryInfo(mesh)` gives the full stats, the current `settings` and `checks`.
4. Look once at the end: `captureTurntable(path=, views=['front', 'right', 'top', 'persp'], display='shaded')`, or `captureView(path=, view='three-quarter', display='shaded')`. Check the ruin's silhouette and that rubble lies on the ground.

## To Unreal

With the editor connected (`unrealConnect()`):

```python
masonryShip('Tower', dryRun=True)['chunks']       # plan only: no files, no Unreal
masonryShip('Tower', wait=True)['unreal']          # {'imported', 'placed', 'errors', 'materials'}
```

- What goes is the wall as it stands (ruin, rubble, age, plaster, ivy). Each stone mesh is imported once to `/Game/Lila/Masonry/<mesh>` with three LODs (worn, clean, box); stones are placed as one HISM per stone mesh per 4 m ground chunk (`chunkSize`), under the Lila outliner folder. The core is its own static mesh carrying the collision (convex UCX, floors included); stones have none. Sending again replaces chunks in place.
- Modes: `hism` (default), `ism`, `merged` (one static mesh per chunk), `lowPoly` (baked shell per chunk, for background and HLOD), `assembly` (UE 5.8 Nanite Assembly through USD; the project needs Interchange OpenUSD and `r.Nanite.AllowAssemblies=1`). Nanite is off unless `nanite=True`.
- Materials: the material function `MF_LilaMasonry` and the master `M_LilaMasonry` on it, in `/Game/LilaLink/Masters`, made on first send and then the user's (`regenerateMaster=True` rebuilds both). For the user's own material, drop `MF_LilaMasonry` into it, or pass `material='/Game/Art/M_MyStone'` (same parameter names). Each wall gets instances `MI_<mesh>_Stone` (LOD0), `MI_<mesh>_StoneFar` (LOD1, LOD2), `MI_<mesh>_Mortar` (core) and, in lowPoly, one per shell. `material='none'` matches or makes materials by slot name (`MasonryStone`, `MasonryStoneFar`, `MasonryMortar`, `MasonryShell`).
- Lila materials named `MasonryStone` and `MasonryMortar` set the instances' Tint, BaseRoughness and tiling textures.
- Per-stone custom data on HISM and ISM: 0 random, 1 height up the wall, 2 age, 3 damage, 4 wet, 5 moss. Merged meshes: random, height, moss, wet in vertex colour RGBA, age and damage in uv2.
- `textures=True` (default) bakes a shared atlas (normal, AO, curvature) into uv0 of every LOD; the tiling projection is in uv1. `textureSize`, `backend='gpu'|'cpu'`; `bakeBackend` in the result says which ran.
- Errors from the editor come back in `['unreal']['errors']` with `wait=True`, else in Lila's log as `masonryShip <wall>: ...`.
- `makeDestructible(mesh, strength=30, impact=[x, y, z], blast=1500)` hands the stones to the Destruction extension's solver instead.

## Typed Python

`import lila.masonry as masonry` wraps the same commands with dataclass results: `masonry.wall(points, ...)` returns a `Build` (`mesh`, `modifier`, `stones`, `regions`, `triangles`), `masonry.ruin(...)` a `RuinStats`, `masonry.check(...)` a `Check` (`ok`, `issues`).
