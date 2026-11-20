Age {{area}} in Lila: {{brief}}.

Work in Lila through its MCP tools only. Follow this workflow:

1. Read lila://guide/weathering (or call read_guide with topic "weathering") if you have not yet. Call scene_summary to see what is there.
2. Plan in a few lines: which objects, which presets in which order (each is one effect), which style (realistic or toon), and whether the user supplied their own pieces (look for them first: librarySearch(tags=["weathering"]) and the selection).
3. Run weather once per effect (or its parts) in as few execute_python calls as you can, filling the pieces slot with the user's pieces where they exist.
4. capture_view the result from two angles. Adjust seed, density or width until it reads believably, without burying the shapes.

Report what you ran, which pieces came from the user and which were placeholders, and anything missing.
