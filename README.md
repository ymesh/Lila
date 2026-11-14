# Lila

> [!CAUTION]
> **Under construction: pre-alpha.** Expect crashes, missing features, lost work, and an excessive and rapid quantity of commits. Testers welcome: report issues on the [Lila Dev Discord](https://discord.gg/kx3jxkksmf) (open until release).
> <br>**Use `git clone`, not Download ZIP.** Pre-alpha has no version numbers, so the commit is the only way to tell whether a bug you report is already fixed. A zip has no commit. Run `git pull` before reporting.

3D app for game assets: Maya-style workflow, modern multithreaded core, Unreal-first pipeline.

> [!NOTE]
> Source-available, not open source: build and modify it for yourself or your studio, but don't publish builds or forks. No pull requests; request changes through issues. See [LICENSE](LICENSE) and the [FAQ](https://lila-3d.github.io/docs/#/faq?id=license).

## Get it running (Windows)

Needs Visual Studio 2022 (C++ workload with its default MSVC 14.44, CMake 3.28+, vcpkg). Qt comes from vcpkg with the other dependencies; no Qt installer or Qt account. An installed Python 3.14 is found in `C:\Python314` or python.org's default folders, otherwise the first build downloads it into `.deps`. Defaults live in `scripts/env.cmd`; set `LILA_VS`, `LILA_PYTHON_DIR` or `VCPKG_ROOT` before calling it to override. Lila and its vcpkg dependencies all build with the MSVC toolset pinned in `cmake/triplets/x64-windows.cmake`; `scripts\toolchain.cmd` shows what the build gets. sccache and lld-link are used when found, and are optional.

```
build.bat                              # configures on first run (vcpkg deps; Qt and USD make this long), then builds
build\bin\RelWithDebInfo\lila.exe      # or: lila.exe <folder>  /  lila.exe scene.lila
```

Don't create the `build` folder by hand; the first `build.bat` makes it. `build.bat <dir> <config>` builds into another folder or configuration.

To skip the heaviest dependencies, configure once before `build.bat` with `cmake --preset windows -DLILA_USD=OFF -DLILA_VOLUMES=OFF` (either or both). `LILA_USD=OFF` drops OpenUSD: USD files, live stages, MaterialX and Hydra viewport renderers; Lila's own viewport doesn't need it. `LILA_VOLUMES=OFF` drops OpenVDB and the VDB volume nodes.

After the dependencies, Lila itself takes about 7 minutes cold on 32 cores, and about 1 minute in a new build folder once sccache is warm. Disk: about 115 GB for Debug, RelWithDebInfo and Release builds with their dependencies, plus about 90 GB of vcpkg build trees in `vcpkg_installed\vcpkg\blds` that you can delete once dependencies are built (`-DVCPKG_INSTALL_OPTIONS=--clean-after-build` doesn't keep them). `-DLILA_BUILD_TESTS=OFF` skips the tests, almost 40% of the build folder.

To build an installer instead, also install Inno Setup 6 (`winget install JRSoftware.InnoSetup`), then:

```
scripts\build.cmd release
scripts\installer\build.cmd            # writes build-installer\Lila-<version>-Setup.exe
```

Tests: `scripts\t.cmd build RelWithDebInfo -LE slow`. `-L slow` runs the slow suites (every shipped material evaluated and linted, about 20 minutes).

### Linux (untested)

Experimental: nobody has built Lila on Linux yet, and Ubuntu LTS is the only supported distro. Needs CMake 3.28+, Ninja, a C++20 compiler and a [vcpkg](https://github.com/microsoft/vcpkg) clone after its `bootstrap-vcpkg.sh`. Qt comes from vcpkg and Python 3.14 is downloaded into `.deps`, so the distro's Qt and Python aren't used. USD is off.

```
VCPKG_ROOT=<vcpkg clone> bash build.sh       # or: bash build.sh <dir> <config>
```

Tested setups, distro packages, Clang and container builds, and troubleshooting: [BUILD.md](BUILD.md).

### Unreal

1. Copy `ue/LilaLink` into `<Project>/Plugins/LilaLink` and build the project.
2. Open the editor; its log shows `LogLilaLink: Listening on 127.0.0.1:8741`.
3. In Lila: select meshes, File > Send to Unreal, enter content paths. From then on every save re-exports and reimports the mapped objects.

## Troubleshooting

- **vcpkg fails with `Operation REMOVE_DUPLICATES not recognized`:** the vcpkg that ships with Visual Studio is older than Lila's pinned ports. Update Visual Studio 2022 in the Visual Studio Installer (or point `VCPKG_ROOT` at a fresh clone of github.com/microsoft/vcpkg after running its `bootstrap-vcpkg.bat`), delete the `build` folder and run `build.bat` again.
- **`could not open 'msvcrtd.lib'` / `oldnames.lib`, or the compiler shows as Clang:** `vcvars64.bat` failed, so CMake ran without MSVC. Run `"C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Auxiliary\Build\vcvars64.bat"` in a Command Prompt and read its `[ERROR...]` lines; usually the MSVC toolset is incomplete, fixed by Visual Studio Installer > Modify > "MSVC v143 - VS 2022 C++ x64/x86 build tools (Latest)", or Repair. Then delete the `build` folder and run `build.bat` again; `vcpkg_installed` is kept, so the deps don't rebuild.
- **`Could not set up Python` or vcpkg fails with `usd[python] needs a Python install`:** the Python download failed (network or proxy). Install Python 3.14 from python.org (not the Microsoft Store version; it installs alongside other versions) and `set LILA_PYTHON_DIR=<its folder>` before `build.bat`.
- **Link error LNK1168 while building:** Lila is running. Close it (or `taskkill /IM lila.exe /F`).
- **Viewport stays black:** check the Script Output for a `vulkan:` line. Lila needs a Vulkan 1.3 GPU and driver.
- **Python errors at startup:** `LILA_PYTHON_DIR` must point at the exact Python 3.14 the build used.
- **Script editor shows no completions:** the first open installs basedpyright and ruff into `%LOCALAPPDATA%\Lila\tools`; the status line says when it's ready. It needs pip and a network connection once.
- **Unreal doesn't connect:** run `unrealStatus(scan=True)` in the script editor; check Editor Preferences > Plugins > Lila Link is enabled; the plugin tries ports 8741 to 8748.
- **Save fails on a read-only file:** the file is under Perforce and not checked out; Lila asks to check it out (pref `sourceControl` `/autoCheckout`: `always`, `prompt`, `never`).
- **Crashes:** every crash, including heap corruption and other fail-fast crashes, writes `%APPDATA%\Lila\crashes\<time>_<pid>\report.txt` (what crashed, every thread's stack, the last log lines and commands, system and GPU) plus `minidump.dmp`. Help > Copy Last Crash Report puts the report on the clipboard. No report after a fail-fast crash means `lilawer.dll` or `lilacrash.exe` is missing next to `lila.exe`, or Windows Error Reporting is disabled.

## Tuning

Prefs are JSON in `%APPDATA%\Lila\prefs` (Help > Open Prefs Folder), written as you change things and reloaded when edited outside Lila. A workspace's `.lila/` folder overrides them per project.

- `general.json`: `/scene/upAxis` (`"y"` or `"z"`), `/scene/defaultFormat` (`"binary"` or `"ascii"`), `/autosave/intervalSeconds` (default 30), `/viewport/env/*` (HDRI path, rotation, blur, background), `/manipulator/*` (size, spaces, axis tints; `/manipulator/snap/{move,rotate,scale}/{enabled,step}` and `/manipulator/snap/move/absolute` for the viewport toolbar's increment snapping), `/softSelect/*`, `/references/autoReload`, `/terminal/shell`, `/crash/keepReports` (crash folders kept, default 20), `/crash/fullDump` (full-memory minidumps, default off).
- `hotkeys.json`: every binding, by context (Windows > Hotkey Editor edits it).
- `markingMenus/*.json`, `shelves/*.json`: menus and shelves (Windows > Marking Menu Editor; right-click a shelf).
- `naming.json` (per workspace): naming conventions; joints snake_case, everything else PascalCase by default.
- `sourceControl.json`: Perforce/git settings.
- `unreal.json` (per workspace): Unreal projects and asset mappings.
- MCP for Claude: toggle with the MCP button in the status bar; connection details in `%APPDATA%\Lila\mcp.json`.
