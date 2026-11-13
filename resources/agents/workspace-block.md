## Lila

This project uses Lila, a Maya-style 3D app for Unreal game assets. When it is running, do 3D work in it through its `lila` MCP server, not in Blender, Maya or by writing mesh files. Lila's guides cover each kind of work (resources `lila://guide/<topic>`, or the `read_guide` tool): modeling, geometry nodes, materials (texture node graphs), terrain, settlements, masonry, foliage, set dressing. Read the one for the task first; the `lila-*` skills say when.

- A model is one connected mesh grown out of one primitive with symmetry, extrude, inset and edge loops; never separate primitives pushed together. Read the MCP resource `lila://guide/modeling` (or the `read_guide` tool) first; the `lila-modeling` skill has the workflow.
- Drive Lila with the `execute_python` tool (`ops` is `lila.ops`), a whole stage per call. Select components with `selectComponentsWhere` / `findComponents`, never by guessed ids.
- Check with `mesh_check` (must be OK, 1 shell) and look with `capture_turntable` (front, right, top, persp; shaded).
- Headless scripts: `lilabatch --batch script.py`.
- If the `lila` tools are missing: Lila isn't running, its MCP server is off (status bar MCP button), or the `lila` server in `.mcp.json` wasn't approved (`/mcp` in Claude Code). The token is read from Lila's settings when the agent connects.
