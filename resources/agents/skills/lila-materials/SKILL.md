---
name: lila-materials
description: Build procedural materials in Lila through its lila MCP server: texture graphs (material node setups) for base colour, roughness, metallic, normal and height, driven by noise, patterns and baked mesh maps, previewed on the mesh, exported as maps and sent to Unreal. Use when the user asks for a material, a shader or texture node setup, a procedural texture, or to rebuild a Substance Designer or Blender shader graph in Lila.
---

# Materials in Lila

1. Read the guide first: the `lila://guide/materials` resource, or the `read_guide` tool with topic `materials`. It has the spec format and the full texture node catalogue (every parameter, socket, type and default).
2. Write the whole graph as one spec and call `texBuildGraph(spec, assign=[meshes])` in one `execute_python` call. If it fails, the error lists every problem with the valid names: fix them all and call again.
3. Check with `texGraphInfo(set)` (compile error, resolved ports) and `material_lint`, then look with a capture of the mesh.
4. `texEvaluate(set)` writes the maps; the guide covers export presets and Unreal.

Units are cm, Y up. Colours are linear `[r, g, b, a]`.
