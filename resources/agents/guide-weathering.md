# Weathering in Lila, for agents

Weathering (an extension that ships enabled, on top of Stagehand) ages, damages and overgrows meshes, Blockwork levels, Masonry walls and Settlement buildings: dressing round their base, ground presets, grunge, edge decals and chipping, ivy, and roof, window, wire and rooftop generators. Every call is deterministic per `seed`. Lengths are cm, Y up.

## The loop

1. `weatherPresets()` lists the presets (name, description, kind, style, slots). Each is one effect (a scatter, a surface effect, a growth or a generator) and comes in a realistic and a ` Toon` version.
2. `weather(objects, preset="Concrete Debris", style="realistic"|"toon", seed=)` applies one as one undo step and returns `group`. Objects may be meshes, groups (every mesh under them) or object sets such as Blockwork visgroups. Build a look by calling it once per effect: `Concrete Debris`, then `Grunge`, then `Worn Edges`, then `Ivy`.
3. `capture_view` the result. Change `seed`, the preset or a slot and run again.
4. Users do the same by dragging presets from the Library's Presets tab (`window.libraryPresets`) onto meshes.

## Pieces: the user's own first

- Every step that places or builds from pieces takes the user's meshes. In `weather`, fill the scatter presets' slot: `slots={"pieces": ["MyChunks_grp"]}`. In single commands, the `items=` / pieces flag takes scene meshes, groups, object sets, library ids, `tag:<a+b>` and weighted entries `{"piece": name, "weight": 3}`.
- `weatheringAddPieces(meshes, role="rubble", style="toon")` puts meshes in the library tagged `weathering` + role + style; from then on every preset using that role picks them instead of the placeholders.
- Placeholders (procedural, tier `placeholder`) fill any role nobody supplied: `librarySearch(tags=["weathering", "rubble"])`. `weatheringPlaceholders(force=True)` rebuilds them.
- Roles: rubble, rock, brick, plank, beam, charred, plant, grass, moss, bush, pebble, litter, leaf.

## Base dressing (round where things meet the ground)

- `baseDressing(objects, items=, width=80, density=6, falloff=1.5, leanChance=0.6, ground=, blockers=, realize=False, settle=False)`: a band round the objects' footprint, densest and biggest at the wall; tall thin items (planks) lean on it. Any outline works. Returns the Stagehand arrangement result plus `ground` (the meshes used, or `"plane"`).
- Live by default (follows edits to the objects); `realize=True` for real instances you can `push`, `settle` and `dressCheck`.

## Ground

- `groundDressing(ground, preset="Rubble Ground"|"Asphalt Crack"|"Dirt and Litter"|"Grass Patches"|"Moss Floor", style=, items=, material=)`: scatter plus a ground material plus tint. Returns `group`, `scatter`, `tint`, `material`.
- `groundTint(ground, sources=, spotsFrom=, reach=, wetness=)`: darkens the ground round what stands on it and wets dips, live for `sources`; `spotsFrom` meshes are taken as they lie. Writes the `weathering` colour set (R tint, G wet).

## Levels

- `weather` on more than 12 meshes dresses in tiles (`tile=1500` cm) so editing one block rebuilds only its tile; neighbours become `blockers` so no tile dresses inside another's blocks. When Blockwork is loaded the result joins the `Weathering` visgroup (`visgroup=""` to skip).

## Unreal

- `weatheringUnreal()` builds MF_LilaGroundTint and the other weathering material functions with generated materials in the linked project; `skipped` says what this LilaLink can't build. Dressing goes over with Stagehand's send as instances.
