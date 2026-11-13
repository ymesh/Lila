---
name: lila-set-dressing
description: Dress scenes with props in Lila through its lila MCP server: pick assets from the library, place, scatter, pile, fill shelves, snap modular kits and settle everything with physics. Use when the user asks to dress, furnish, clutter, decorate or populate a room, area or level.
---

# Set dressing in Lila

1. Read the guide first: the `lila://guide/set-dressing` resource, or the `read_guide` tool with topic `setDressing`.
2. `librarySearch(...)` for asset ids; never invent them.
3. Place or arrange, then `dressCheck([meshes])` until `ok` (nothing floating, overlapping or unstable).
4. Look with a capture of the area before reporting back.

Units are cm, Y up. Calls are deterministic for the same scene and seed.
