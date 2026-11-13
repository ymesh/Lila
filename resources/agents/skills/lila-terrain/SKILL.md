---
name: lila-terrain
description: Build game terrain in Lila through its lila MCP server: landscapes, mountains, valleys, islands, rivers, lakes, roads, cliffs, caves, erosion, real places from elevation data, building pads, and sending the landscape with paint layers to Unreal. Use when the user asks for terrain, a landscape, a heightmap or a level's ground.
---

# Terrain in Lila

1. Read the guide first: the `lila://guide/terrain` resource, or the `read_guide` tool with topic `terrain`. Its recipes map each request to the command.
2. Prefer one spec: `terrainSpec(spec={...})` builds the whole terrain and checks it; start from a preset (`terrainSpec(list=True)`).
3. Check with `terrainCheck(t)` until `ok`, and look with `terrainMap(t, file=)` or a capture.
4. Ship with `terrainSendToUnreal(t)` or `terrainExport(t, target=...)`.

Units are cm, Y up. Name places with `terrainQuery(..., store=)` and refer to them as `region:<name>` instead of guessing coordinates.
