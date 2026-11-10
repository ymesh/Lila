# Trim Frieze

A sandstone wall built from one trim sheet: a plinth with a moulded base, three ashlar courses, a dentil course, a rosette frieze and a cornice, each a plain box carrying one strip of the sheet. The rosettes and dentils were modelled on the sheet's high poly and baked in; this is the trim sheets tutorial in the docs, finished.

## Use it

1. Open `trim_frieze.lila` and look through `Cam_Wall` (in the view, Panels > Look Through Selected).
2. Open the Level Toolkit's Trim Sheets section: the sheet is current, so its palette shows the strips. Select a band of the wall and click another strip to see it there.
3. Compare normals: look through `Cam_Compare`. `Wall` (left) has **Normals From Height** on, so the dentils' sides light as walls; `Wall_BakedNormal` (right) uses the baked normal alone, and the dentils read as flat stickers. Toggle **Normals From Height** on either wall's material in the Material Editor to flip it.
4. Unhide `SheetHigh` to see the generated high poly and the rosettes and dentils modelled on it. Move or reshape them, then press **Bake** in the Trim Sheets section: every band updates.

## Troubleshooting

- The wall is plain grey: the sheet's textures are in `TrimSheets/T_Frieze_Textures` beside this scene. If they're missing, select nothing and run `trimSheetBake()`.
- The rosettes or dentils vanish after a bake: they bake from the meshes in `SheetHigh`, or from the copy in `TrimSheets/T_Frieze_Textures/T_Frieze_high.obj` when those aren't in the scene.

## Tune it

At the top of `build_trim_frieze.py`: `ROSETTES` and `DENTILS` (how many along the 200 cm sheet; keep them dividing it evenly so the strips tile), `MATERIAL` (any smart material), `STRIPS` (each strip's profile style) and `BANDS` (the wall's bands: height, depth and the strip each carries). Run it again from the script editor to rebuild the sheet and the wall.
