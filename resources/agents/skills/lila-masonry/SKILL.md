---
name: lila-masonry
description: Build stone and brick masonry in Lila through its lila MCP server: stone walls, castle walls, towers, keeps, chapels, temples and ruins laid stone by stone with arches, columns, stairs and vaults, then ruined, aged, checked and sent to Unreal as instanced stones. Use when the user asks for a stone wall, brick wall, ruin, castle, tower, ruined building, masonry, or to turn a blockout into stonework.
---

# Masonry in Lila

1. Read the guide first: the `lila://guide/masonry` resource, or the `read_guide` tool with topic `masonry`. It has the recipes, the building spec keys, checks and shipping. Every spec key with its default is in `lila://masonry/spec-schema`.
2. Build in one `execute_python` call: `masonryWall(points=...)` for one wall or a round tower, `masonryBuilding(spec={...})` for a building with named parts, `masonryFromMesh('Blockout')` over a blockout. Lengths are cm, Y is up; pick stone sizes for stone (around 50 x 25 x 35), not the brick defaults.
3. Read the returned stats (`stones`, `regions`) and adjust: `setAttr('<modifier>.<flag>', v)` on a wall, or edit `masonrySpec(mesh)['spec']` and call `masonryBuilding(node=mesh, spec=s)`.
4. Ruin and age it: `ruin(mesh, left=0.5, collapse=90, gaps=1)`, `age(mesh, age=0.8, moss=0.6)`.
5. Check: `masonryCheck(mesh)` until `ok` is true, then one `captureTurntable(path=, views=['front', 'right', 'top', 'persp'], display='shaded')`.
6. Ship when asked: `masonryShip(mesh, wait=True)` with Unreal connected (`dryRun=True` without it). Materials build on `MF_LilaMasonry`.
