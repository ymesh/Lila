# Saltmarket

A desert-town blockout for a two-site attack and defend map: three lanes, a raised colonnaded A site, a walled B site and an arched gate cutting the long mid sightline. Every piece is a Blockwork block, hotspotted live onto three trim sheets (stone in brick, plaster, tile and cobble palettes, wood, and sand) and bevelled where no seam can open, so the blocks stay textured as you resize, extend, clip or carve them.

## Use it

1. Open `saltmarket.lila` and look through `Cam_Overview`, `Cam_Mid`, `Cam_ASite` or `Cam_BSite` (in the view, Panels > Look Through Selected).
2. Pick Level > Blockwork > Extend Tool and drag a face of any block. Its textures and bevels follow as it grows.
3. Select some crates and move them: blocks snap to the Blockwork grid. Hold J to move them freely.
4. To see the site volumes, open Level > Blockwork > Visgroups and show `Sites`.

## Troubleshooting

- The blocks are plain grey: the sheets' textures are in `TrimSheets` beside this scene. If they're missing, select nothing and run `trimSheetBake()`.
- A block you added is untextured: only the blocks the script built are hotspotted. Select it and click **Hotspot** in Level > Blockwork > Block Textures.
- A block you moved against another shows a groove along the join: bevels skip edges that touch another block, found when the bevel is added. Select both and run Level > Blockwork > Bevel Blocks again.

## Tune it

At the top of `build_saltmarket.py`: `HOUSES` and `WALLS` (the layout and each building's palette), `CRATES`, `PLAN` (how far the layout is spread out), `PALETTES`, `WOOD_MATERIAL` and `GROUND_MATERIAL` (the looks), `BEVEL` (cm, 0 for none) and `SEED` (another variation). Run it again from the script editor.
