# Settlements in Lila, for agents

A settlement is a mesh with a geometry-nodes modifier around a Settlement node: main roads routed over the ground, streets grown by a style, blocks, lots with uses and landmarks, and a building envelope on each lot. It re-lays itself whenever an input changes, so change it through its inputs and edit commands, never by deleting and rebuilding. Lengths are cm, Y up. Points are `[x, z]` on the ground or `[x, y, z]`. Commands are called bare here, as in `execute_python` (`ops.settlementCreate` works too).

## Recipes

- A town on flat ground: `settlementCreate(profile='medievalTown', size=60000)`, then `settlementBuildings(mesh, route='massing')` and `settlementCheck(mesh)`. See "Flat ground".
- A town on a Terrain: `settlementSuggest` for the site, `settlementCreate(..., ground='Terrain')`, `settlementGrade(mesh)`, check both. See "On a Terrain".
- A whole map of cities, towns and villages: `settlementRegion(ground='Terrain', cities=1, towns=3, villages=8)`. See "A region".
- Another layout on the same site: `setAttr(mod + '.seed', 4)`. Smaller or younger: `.growth`. Taller: `.overrides` `{"maxFloors": 6}` or `.heightScale`.
- Walls, eras, districts, street patterns: flags on `settlementCreate`. See "Style".
- Change one street, lot or landmark: find its id, then edit by it. See "Edit by id".
- Why is this here: `settlementExplain(mesh, id='centre' | '<street id>' | 'lot:<lotId>' | 'block:<blockId>' | 'landmark:<lotId>')`.
- Ship it: `settlementSendToUnreal(mesh)` (`dryRun=True` works without Unreal), or `settlementExport(mesh, path=)`. See "Out".

## Flat ground

```python
town = settlementCreate(profile='medievalTown', size=60000, seed=2)
mesh, mod = town['mesh'], town['modifier']
settlementBuildings(mesh, route='massing')
c = settlementCheck(mesh)
c['ok'], c['counts'], c['issues'][:5]
```

- `settlementCreate` returns `mesh`, `modifier`, `group`, `checks`, `metrics` and stats: `lots`, `buildings`, `blocks`, `streetMetresByClass`, `lotUses`, `radiusM`, `bridges`, `refusedStreets`. Read `checks['ok']` and `checks['warnings']` straight away.
- `outline=[[x, z], ...]` instead of `size=` gives the site's shape. `attractors=[[x, z], ...]`: the first is the centre, the rest are places main roads run to (a bridge, harbour, gate). `roads=[[...], ...]` are main roads from outside, `streets=` streets kept as drawn, `water=[[...], ...]` rivers with `waterWidth=`.
- Sizes: a hamlet or village 20000 to 40000, a medieval town 40000 to 80000, a city 150000 and up. Bigger is slower; lay out small, then grow.
- Profiles: `hamlet`, `streetVillage`, `medievalTown`, `usTown`, `manhattan`, `europeanCity`, `modernCity`, `scifiMegacity`. `settlementProfiles()[name]` lists every field; any field goes in `overrides`.
- Every layout flag is a keyable input on the modifier: `profile`, `pattern`, `overrides` (JSON text), `growth` (0 to 1), `seed`, `size`, `heightScale`, `widthScale`, `lotScale`, `blocks`, `buildings`. `setAttr(mod + '.overrides', '{"maxFloors": 6}')` re-lays it; `settlementInfo(mesh)` reads the result.

## On a Terrain

```python
T = 'Terrain'                                   # from terrainCreate / terrainSpec, eroded and with water added first
site = settlementSuggest(ground=T, count=3, kind='market')[0]
x, _, z = site['position']                      # [x, y, z]; site['reasons']['terms'] says why it won
r = 25000
town = settlementCreate(outline=[[x - r, z - r], [x + r, z - r], [x + r, z + r], [x - r, z + r]],
                        attractors=[[x, z]], ground=T, profile='medievalTown', eras='walledTown', name='Town')
mesh = town['mesh']
g = settlementGrade(mesh)                       # streets become terrain roads, lots building pads
settlementBuildings(mesh, route='massing')
g['overGrade'], g['errors']
settlementCheck(mesh)['ok'], terrainCheck(T)['ok']
```

- Order: terrain shape, erosion and water first (read `lila://guide/terrain`), then the settlement and its edits, then `settlementGrade`. The buildings route can change before or after grading. Grading stamps in the terrain's `City` layer after erosion, so erosion never re-cuts it.
- `kind` for siting: `town`, `market`, `port`, `mining`, `fort`, `farming`, `mill`; `rules={'defense': 0.9}` weights terms yourself. On a Terrain the terms come from its own analysis (fords, harbours, buildable land).
- On a Terrain, main roads are routed with `terrainRoute` and rivers crossing the outline come in as water (`terrainWater=False` leaves them out). Only main roads bridge rivers; no lot lies in water.
- Grading takes about a second for a 500 m town of 700 lots and 230 streets on a 257 terrain. `lots=False` or `streets=False` grades only part. `settlementCheck` works before grading.
- `settlementGrade` returns counts made and sent per kind (`streetsSent`, `stampsSent`, `pads`, `bridgeStreets`), `unchanged`, `removed` and `overGrade` (streets the terrain could not keep within grade: street `id`, terrain `road`, `grade`, `maxGrade`, `why`). `padMode='terraced'` steps pads down slopes; `roundabouts='none'` skips roundabouts. Grading again sends only what changed.
- Grading pins the plan. After terrain edits, `settlementReplan(mesh)` re-plans only the blocks near what changed (`mode`, `why`, `keptBlocks`, `newBlocks`), then `settlementGrade(mesh)` again. A change to the settlement's own layout inputs after grading (seed, profile, a street or lot edit) makes the next replan `full`; a buildings route does not.
- After grading, `settlementCheck` judges each street by the grade its terrain road was built at, so its `grade` warnings are the streets in `overGrade`.
- Steep hillsides stay empty past the profile's `terraceGrade` (0.5): raise it in `overrides`.
- Keep the settlement mesh at the origin; the ground is read in world space.

## A region

```python
reg = settlementRegion(ground='Terrain', cities=1, towns=3, villages=8, kind='market')
[(s['name'], s['tier'], s['lots']) for s in reg['settlements']], reg.get('roadNotes')
```

Cities go on the best sites, towns between, villages filling in, roads routed between neighbours. Each place is its own settlement mesh (check each); roads and rivers land on a `Region` mesh. `size=` or `outline=` without a ground; `cityProfile`, `townProfile`, `villageProfile`, `cityRadius`, `townRadius`, `villageRadius` tune it. `settlementExplain(place, id='site')` gives the siting reasons.

## Style

All of these are `settlementCreate` flags, and `settlementGenerators(mesh, ...)` reads or changes them later.

- `pattern=`: `organic`, `grid`, `radial`, `linear`, `organicGrid` (organic core, grid beyond a ring road), `tensor` (streets following blended fields: `tensor={'fields': [{'kind': 'grid', 'angle': 20}, {'kind': 'boundary', 'follow': 'water', 'weight': 2}]}`), `contour` (along a Terrain's contours), `osm` (`osmFile=`), `example` (another network's statistics), `none` (only what you give).
- `eras=`: `walledTown`, `medievalToModern`, `gridToSprawl`, or a list of `{name, profile, pattern, inner, outer, walls, ...}`. Streets, lots and buildings carry their era. `settlementEras(mesh)` lists what each laid.
- `walls=`: `True`, `{'moat': 1200}`, `{'starFort': True}`, or `{'polygon': [...], 'towerSpacing': ...}`. Gates go where main roads cross. `settlementWalls(mesh, curves=True)` makes curves; `masonry=True` builds stone.
- `districts=[{'name', 'polygon', 'profile', 'pattern', 'overrides'}]`: their own generator inside the polygon.
- Keep a structure clear: `exclude=['Temple'], exclusionMargin=2000`; routes onto it with `walkable='Temple'` and `settlementRoute(mesh, start=, end=[x, y, z])`.

## Edit by id

Every street, block, lot and landmark has a stable id that survives re-runs and unrelated edits. Edits are undoable and save with the scene.

```python
main = settlementStreets(mesh, streetClass='arterial')[0]['id']   # near=[x, z], radius= narrows it
settlementEditStreet(mesh, id=main, profile='avenue')
settlementAddStreet(mesh, points=[[-20000, -10000], [20000, -8000]], profile='street')
settlementDelete(mesh, ids=[settlementStreets(mesh, streetClass='local')[0]['id']])
lots = settlementLots(mesh, use='residential', limit=20)['lots']
settlementLandUse(mesh, lot=lots[0]['lotId'], use='commercial')
settlementPlaceLandmark(mesh, at=lots[5]['center'], name='castle', use='civic')
settlementSubdivide(mesh, at=lots[9]['center'], type='courtyard')
```

- Lots have two ids: `lotId` (stable, for `lot=` and `settlementExplain(mesh, id='lot:<lotId>')`) and `lot` (its index in this layout, also taken by explain). An `at=` point off every lot in `settlementLandUse` or `settlementPlaceLandmark` takes the nearest lot within 100 m and returns it as `snapped`.
- `settlementStreets` returns a list: `id`, `class`, `profile`, `name`, `width`, `lengthM`, `path`, `points`. Classes: `arterial`, `collector`, `local`, `alley`, `path`. Road profiles: `settlementRoadProfiles()`.
- A street runs on through junctions until another street of its own class meets it, so side streets and drawn streets joining or crossing it keep its id. A street of its own class laid across it splits it into new ids. An edit whose street the layout no longer makes shows in `settlementInfo(mesh)['checks']['staleOverrides']`.

## Buildings and streetscape

- `settlementBuildings(mesh, route=)`: `outline` (envelopes only), `massing` (greybox blocks with roofs, in the modifier's output, no new objects), `library` (Stagehand assets fitted per lot; lots nothing fits are listed), `masonry` (stone landmarks), `unreal` (envelopes go as PCG data). `polygon=` applies it to one district. `contacts=True` joins placed buildings to the ground with Seamwork.
- `settlementNames(mesh, culture='french')`, `settlementRoadMesh(mesh)` (road, sidewalk, marking meshes), `settlementFurniture(mesh, realize=True)`, `settlementTransit(mesh, busRoutes=2)`, `settlementLanes(mesh)`.

## Check it

- `settlementCheck(mesh)`: `ok`, `counts` per check (`frontage`, `overlap`, `grade`, `water`, `envelope`, `foundation`, `lanes`, `laneGraph`, `phases`), `issues` (each `check`, `severity`, `id`, `message`, where), `warnings` (checks that could not run). Fix until `ok`; warnings count too. `report=True` also lists them in the Validation panel.
- `settlementInfo(mesh)['checks']`: `components` (separate street networks; want 1), `crossings` (streets crossing without a junction), `steepStreets`, `landlockedLots`, `badEnvelopes`, `failedPaths`, `warnings`. `sharpJunctions` is informational.
- On a Terrain also `terrainCheck(T)` until `ok` (pads, street grades, flooding).
- Look once at the end. `select(mesh)` first and pass `frameSelection=True`, or a town on a big terrain is a speck: `captureView(view='top', display='shaded', frameSelection=True, path=...)` for the plan, `captureView(view='persp', yaw=30, pitch=-35, display='shaded', frameSelection=True, path=...)` for the massing.

Fixes for common results:

- `components` above 1: drawn streets that end early, or no way over the ground to the rest. Extend them or raise `growth`.
- Few buildings: lots below the profile's `minLot` or without frontage stay open. Lower `lotScale` or `minLot`, or raise `growth`.
- `overGrade` after grading: lower the ground there, raise `maxGrade` in `overrides`, or delete that street by id.
- An outline error "has no area": the loop crosses itself or has fewer than three points.

## Out

- `settlementSendToUnreal(mesh, roads='landscape', pcg=True, zoneGraph=True, massing=True)`: cut into World Partition cells (`cell=12800`), only what changed goes on later sends. `roads=`: `landscape`, `splines`, `meshes`, `none`. `dryRun=True` returns `cells`, `items`, `kinds` and the `diff` without a connection. Needs the `LilaLinkSettlement` plugin next to LilaLink in the project; `pcgGraph=` names the graph each cell runs (`makeGraph=True` makes an empty one).
- Send the Terrain first (`terrainSendToUnreal(T)`) when roads go as landscape splines.
- `settlementExport(mesh, path='town.json')`: `json`, `geojson`, `usd`, `dxf`, `unity`, `godot` by extension or `format=`.
