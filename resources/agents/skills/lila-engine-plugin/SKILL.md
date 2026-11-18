---
name: lila-engine-plugin
description: Connect Lila to a game engine other than Unreal (Godot, Unity, Bevy, Defold, O3DE, Stride, a custom engine) the way LilaLink does for Unreal - Send to <Engine> and Connect to <Engine> in Lila's File menu, re-import of .lila files on save, and live Sync of object moves over Lila's change stream. Use when the user wants a "LilaLink" for their engine, an importer for .lila files, or Lila edits to show up in an engine.
---

# Lila engine plugins

Lila's only official engine plugin is LilaLink for Unreal. For another engine, build the same three parts, in this order. Each works without the others.

| Part | Lives in | Does |
|---|---|---|
| Import on save | Engine plugin | `.lila` files in the project become engine assets, re-imported on every save in Lila |
| Send | Lila extension (one Python file) | **File > Send to <Engine>** and **File > Connect to <Engine>...** export the selection into the project |
| Sync | Engine plugin | A toggle that follows Lila's change stream and moves objects live |

Working references, read them before writing code (in Lila's install folder and at https://github.com/lila-3d/Lila/tree/main/resources/engines):

- Godot: `godot/addons/lila_scene` (`lila_scene_import.gd` import, `lila_sync.gd` + `plugin.gd` Sync toolbar toggle), `godot/godot_link.py` (Send).
- Unity: `unity/LilaSceneImporter.cs` (import), `unity/LilaSync.cs` (Tools > Lila > Sync), `unity/unity_link.py` (Send).

User docs: `godot-plugin-tutorial.md`, `unity-plugin-tutorial.md` (step by step, every file explained), `godot-scenes.md`, `unity-scenes.md`, `change-stream.md`.

## Import on save

The engine never reads `.lila` itself. `lilabatch` (installed beside `lila`) opens the scene and exports a format the engine already imports:

```
lilabatch --quiet --export <scene> <out>.glb
lilabatch --quiet --export <scene> <out>.fbx --export-args "profile='unity', units='m', textures=True"
```

`--export` opens the scene and exports all of it in the format the output's extension names; `--export-args` passes the format's export flags as Python keyword arguments. Exit code 0 means the file was written; 1 comes with an `[error]` line saying why. `lilabatch --help` lists every option. Run the command in a terminal against a real scene before writing any plugin code.

If the user doesn't want a plugin at all: **File > Export on Save...** in Lila (`exportOnSave(path=..., options=...)` from Python) writes a plain FBX or glTF beside the scene on every save, which the engine imports like any other file.

1. **Hook the engine's asset pipeline** for `.lila` and `.lilaa`: an import plugin, asset processor, file watcher or build step.
2. **Pick the format** the engine's own importer handles best. glTF: Y up, +Z forward, metres. FBX: `profile='unity'` (the same axes as `ue6`) gives Y up, +Z forward; `units='m'` gives a scale of 1. Other exporters: `exportObj`, `exportAlembic`, `exportCollada`, `exportFile(path)` by extension. Flags are in the generated `lila.ops.md` reference. Export functions take the path first, then objects.
3. **Export to a temp path**, then hand the result to the engine's importer. Never write into the `.lila`.
4. **Find lilabatch** in this order: an editor setting, `LILA_BATCH`, `%LOCALAPPDATA%\Programs\Lila`, `%ProgramFiles%\Lila`, then `PATH`.
5. **Run it** with `LILA_CRASH_NO_UI=1` in its environment, stdout and stderr captured, and a timeout. Pass the scene and output paths as separate arguments (quoted on a command line). Exit code 0 and the output file existing means success.
6. **Report failures** with lilabatch's output: errors go to stderr as `[error]` lines, info to stdout. A missing executable gets its own message naming the setting.

Exports are synchronous and an animated scene is evaluated at every frame of its playback range: offer `--export-args "animation=False"` or running off the editor's main thread.

## Send (Lila extension)

One `.py` file in `%APPDATA%\Lila\extensions` (every project), or in `<workspace>/.lila/extensions` to share it with a team through the project, loaded once with `loadExtension("<name>", autoload=True)`. Copy `godot_link.py` and change the project check and the export call.

- Register commands (`extension.register_command`) for connect and send, so scripts can call them; the menu actions call the commands.
- Store the project folder with `lila.prefs.set("general", "/<engine>Link/project", path)`. Validate it by the engine's marker file (`project.godot`, `Assets` + `ProjectSettings`).
- Send exports each selected object (`ls(selection=True)`) to a fixed folder in the project, named after the object (`res://lila/<name>.glb`, `Assets/Lila/<name>.fbx`). The engine's own importer picks it up; sync matches by that name.
- Menu rows: `ui.add_action(id, fn, label, category=, doc=)`, then `ui.add_menu_item("File", kind="section", label="<Engine>", after="File/unreal.connect")` and each row `after=` the previous id. Rows of an extension loaded mid-session go to the end of the menu; they sit in place from the next start.
- Wrap the `lila.ui` calls in `try/except RuntimeError`: lilabatch has no UI library and raises there.
- Raise `lila.CommandError` with an actionable message for no project or no selection.

## Sync (engine plugin, change stream)

The user turns on **Preferences > General > Change Stream** in Lila; it listens on `127.0.0.1:8742`. Newline-delimited JSON both ways. Full reference: `change-stream.md`.

1. A toggle in the engine (toolbar switch, menu item) connects. Time out after about 3 seconds and tell the user to turn on the change stream; also detect Lila closing.
2. Send `{"op": "subscribe", "snapshot": true, "world": true, "live": true, "types": ["transform"]}`.
3. Read on a background thread or by polling; apply on the main thread.
4. Take `world` from each `snapshot` node and every `world` change in `group`, `live` and `frame` messages. On `resync`, send `{"op": "snapshot"}`. World matrices are absolute, so applying the newest per object is enough; no seq bookkeeping is needed for transforms.
5. `matrix` is 16 numbers, column by column, translation at 12 to 14, in centimetres, right-handed Y up. Godot: same axes, divide translation by 100. Unity (matches its FBX importer): mirror X, so column x = `(m0, -m1, -m2)`, y = `(-m4, m5, m6)`, z = `(-m8, m9, m10)`, position = `(-m12, m13, m14) / 100`, rotation = `LookRotation(z, y)`.
6. Match by the last segment of `path` (the object name), only inside instances of assets from Lila (the send folder, or assets made from `.lila` files). Set the world transform directly, then mark the scene modified so a save keeps it.
7. More than transforms: `"evaluated": true` with `attrs: ["outMesh"]` reports mesh changes as a hash (whole meshes with `"full": true`); re-send on change. The `event` message `sceneSaved` carries the saved path.

Remote machines need **Localhost only** off and `{"op": "auth", "token": "..."}` first. For automated tests, `lilabatch --change-stream <port> -c "..."` serves the stream while a script runs (`time.sleep` between `setAttr` calls to give the engine time).

## Check before handing back

- A saved `.lila` with a mesh, a hierarchy and a texture shows in the engine at the right scale and facing; saving again updates every instance.
- Rename `lilabatch.exe` temporarily: the error names the setting. Break the scene: the error includes the `[error]` lines.
- In Lila, Send rows sit under the Unreal rows after a restart; Send without a project or selection gives a clear message.
- Sync with the change stream off warns and turns itself off. With it on, an object rotated 30 degrees about Y and moved to (100, 200, 300) in Lila lands at the same place and rotation as the imported asset does, and follows drags.
