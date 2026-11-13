---
name: lila-foliage
description: Grow procedural trees and plants in Lila through its lila MCP server with the Foliage extension: pines, cedars, maples, oaks, bushes, windswept or stylised trees, leaf cards, bark and leaf texture sets, and export of the trees to Unreal. Use when the user asks for a tree, pine, oak, bush, shrub, plant, foliage or vegetation, or a forest's worth of tree variations.
---

# Trees and plants in Lila

1. Read the guide first: the `lila://guide/foliage` resource, or the `read_guide` tool with topic `foliage`. It has the recipes, the presets and the settings that matter.
2. In one `execute_python` call: make a leaf card with `treeLeafPlane()`, then `treeBuild(preset=, leaf=, name=, settings={...})` per tree, each with its own `trunk.seed` and `leaf.seed`. Start from the nearest preset (`treePresets()`); setting lengths are metres, the scene is cm.
3. Check: `meshInfo` on `<tree>_trunk` and `<tree>_leaves` for triangles and size, then select one tree and `captureView(view='front', display='shaded', frameSelection=True, isolate=True)` plus one `three-quarter`. Adjust settings and build again until the shape matches the request.
4. Finish what was asked: `treeTextureSet(tree, season=)` for textures, `unrealMap(tree, contentPath=)` and `unrealSync(tree)` for Unreal, or `treeExport` for the bake pair.
