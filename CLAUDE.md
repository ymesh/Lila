# Lila

Maya-workflow 3D app for game work with a first-class Unreal pipeline. C++20, Qt 6 UI shell, Vulkan, TBB, embedded Python, built-in MCP.

Plan: `C:\Users\Jared\.claude\plans\enumerated-watching-quilt.md` is an index; each stage is its own file in `plans\lila\`. Read only the stage for the work at hand.

## Where things live

- `src/core` platform, prefs, logging. `src/ops` command registry, undo, option boxes (`CommandOptions`). `src/scene` DG: nodes, plugs (`Plug.h`: `AttrDef`, `AttrFlags`), graph, transforms, rig, anim. `src/geo` half-edge mesh and mesh ops. `src/curve` curves and planar/text nodes. `src/gn` geometry nodes. `src/uv`, `src/bake`, `src/tex`, `src/retopo`, `src/sculpt`, `src/deform`, `src/vcolor`, `src/seq`, `src/fbx`, `src/gltf`, `src/maya`, `src/io` file formats and references. `src/render` Vulkan. `src/ui` Qt. `src/mcp` MCP server. `src/link` + `ue/LilaLink` Unreal link.
- Extensions: `extensions/<Name>` (SDK headers only, `sdk/include/lila`). Built-in registrations use the same registries with owner "lila".
- DLLs: LilaFoundation (core, ops), LilaScene (scene and everything non-UI), LilaUI. Exported symbols need `LILA_<LIB>_API`.

## Adding a feature (the usual touch points)

- Command: `ops::registerCommand` (`src/ops/Command.h`), usually in a `register*Commands()` next to the feature. Typed flags, long names, a doc string. Undoable via the command's do/undo.
- Node type and attributes: `AttrDef` list with `AttrFlags` (`src/scene/Plug.h`); see `registerPlanar` in `src/curve/PlanarNodes.cpp` for a compact example.
- Option box: `ops::options::setOverride` (labels, ranges, layout, editors) in `src/ops/CommandOptions.*`, fields drawn by `src/ui/FlagFields.cpp`.
- Attribute Editor: plain attributes appear automatically; a custom section is `attributeEditor::addSection(type, title, factory)` (`src/ui/AttributeEditor.h`).
- Action, menu item, hotkey: `actions::add` (`src/ui/Actions.h`). Panels: `panels::add` (`src/ui/Panels.h`).
- Tests: doctest suites under `tests/` (`lila_tests` plus per-area executables such as `tests/curve`, `tests/docks`); add new files to that folder's CMakeLists.
- Python stubs `python/lila/ops.pyi` regenerate at startup of lila/lilabatch; don't hand-edit.
- Unreal materials (LilaLink, any feature): always ship a material function (`MF_...`) users drop into their own materials, and offer the generated material or instance as a second option built on that function. Never only a provided material; when touching an existing feature that ships only a material (VAT, Masonry, Trim Sheets, imposters), add its function.
- User docs: `X:\Lila-3D\docs` (not this repo). Essentials only for day-one things; document only what the UI doesn't already show. A new page in `_sidebar.md` needs its icon in `docs/theme.css` (the `::before` selector list and an `--icon` line); Lila-3D's pre-commit hook rejects a page without one.

## Build and test

- Build with `scripts\b.cmd [dir] [config] [-nounity] [target...]` and test with `scripts\t.cmd [dir] [config] [ctest args]`: they print only errors, failures and a summary (full logs in `<dir>\last_build.log`, `last_test.log`). Iterate on one target, then build all targets before handing back; partial builds leave extension DLLs stale.
- Test only the suites for the area you touched (`t.cmd <dir> <config> -R <area>`). A full ctest run is for the release gate or before pushing a large body of work, never a routine end-of-task step and never from a subagent; give it `--timeout 1200` (some suites, like groomlab, can run for hours). Never leave a test run going when you report back.
- `build` is Jared's only: no session builds there, runs lila.exe from it or starts test instances out of it, whether or not his Lila is running. Never kill or launch his lila.exe.
- Sessions build in the pool `build-a1` to `build-a6`. Claim one with `mkdir <dir>\.lock` (atomic; if it fails, try the next) and `rmdir` it when done, including on handback. A lock whose dir no running process uses is stale: remove it and claim. `b.cmd` configures a fresh pool dir itself; sccache makes it mostly cache hits. Worktree sessions use their own `build-wt` instead. `build-night` is the nightly's.
- Always pass your pool dir to `b.cmd`/`t.cmd`; with no dir they use `build`.
- Run executables only from the pool dirs or your `build-wt` (new paths trigger firewall prompts). No popups, ever.
- Compiles go through sccache (shared cache in `X:\sccache`) and link with lld-link, so a fresh build dir or worktree is mostly cache hits.
- Test instances: set `LILA_CONFIG_DIR=<scratch dir>` and `LILA_MCP_PORT=<free port>`, drive over MCP, kill only that instance's PID afterwards.
- Known failing: none. A test that has to be parked carries the CTest label `known_failing`, which the release gate and nightly skip (`-LE known_failing`); fix it, drop its label. `lila_material_pack_tests_0..3` and `_smart` are labelled `slow` (about 9 minutes each, run them with `-j`).
- Nightly: scheduled task `LilaNightly` (04:00, `scripts\nightly_task.ps1` registers it) runs `scripts\nightly.py`: full build and tests in build-night, line coverage on Sundays in `build-coverage` (unoptimized RelWithDebInfo, the only other dir that may run executables; OpenCppCoverage in `%LOCALAPPDATA%\OpenCppCoverage`, report in `build-coverage\report`). Results in `build-night\nightly\latest.txt`; a failure there is a regression to chase. It skips if another session is using build-night.
- Unity builds are on (PCH too when the compiler cache is off): use `Q_EMIT`/`Q_SIGNALS`/`Q_SLOTS`; a unity clash means rename the file-local helper or add the source to `cmake/LilaUnitySkip.cmake`.
- Line endings are mixed per file. `sed -i` in git-bash strips CRLF; restore with `sed -i 's/$/\r/'` or edit with the Edit tool.
- Other Claude sessions often work in this repo at the same time. Check `git status` and peer sessions before large moves or renames of shared files.

## Verifying UI work cheaply

- Check state, not pixels, by default: over MCP `execute_python`, read attributes (`ops.getAttr`), widget state (find widgets by class and read text, checked state, enabled, items) and command results. Assert the numbers.
- Take a screenshot (`captureView`, or a widget grab) only where the visual result is the point: viewport drawing, layout, styling, icons. One capture at the end of a check, not one per step. `captureView` draws curves, locators and Soft Nets only with `overlays=True`.

## Token hygiene

- Find symbols with the LSP tool (clangd: workspaceSymbol, goToDefinition, findReferences) before grepping; then Read with offset/limit. Large sources: `src/maya/MayaImport.cpp`, `src/render/Renderer.cpp`, `extensions/OpenUSD/UsdStage.cpp`.
- Never print raw build or test output; use `b.cmd`/`t.cmd`, or grep the log for `error|FAILED|LNK`.
- Commit only when asked: subject line only.
