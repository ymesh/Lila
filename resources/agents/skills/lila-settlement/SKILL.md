---
name: lila-settlement
description: Build settlements in Lila through its lila MCP server: villages, towns, cities and whole regions of them, with streets, districts, walls, lots and greybox buildings, on flat ground or a Terrain, checked and sent to Unreal. Use when the user asks for a town, city, village, hamlet, streets or a street network, districts, a walled or medieval town, a settlement, or to settle a map or terrain.
---

# Settlements in Lila

1. Read the guide first: the `lila://guide/settlement` resource, or the `read_guide` tool with topic `settlement`. It has the recipes, the commands that matter and the checks.
2. Build in one `execute_python` call: on a Terrain, `settlementSuggest` for the site, `settlementCreate` with `ground=`, then `settlementGrade`; on flat ground `settlementCreate` alone. Then `settlementBuildings(mesh, route='massing')`.
3. Check with `settlementCheck(mesh)` until `ok` (each issue names its part by id; `settlementExplain(mesh, id=...)` says why it is there), and look with one `captureView(view='top', display='shaded')`.
4. Change it through the modifier inputs (`setAttr('<modifier>.seed', 4)`, `.overrides`) or the edit commands by stable id, never by rebuilding from scratch. Ship with `settlementSendToUnreal(mesh)` (`dryRun=True` first).
