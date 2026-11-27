// The agent guide lila://guide/buildings: making, editing and checking procedural buildings over MCP.

#include "BuildingsScene.h"

#include <lila/Extension.h>
#include <lila/Mcp.h>

namespace buildings
{
namespace
{
constexpr const char* kGuide = R"(# Buildings

Procedural buildings from Settlement envelopes, footprint curves, OSM buildings (through Settlement's osm pattern) or a
mesh's footprint. Units cm, Y up. Every command is undoable and echoed; Python: `import lila.buildings as bld`.

## Make them

- From a settlement: `settlementBuildings('Town', via='buildings')` (or `buildingCreate(settlement='Town')`). New settlements
  can do it in one go: `settlementFromOsm('town.osm', ground='Terrain', buildingRoute='buildings')`.
- From closed curves: `buildingCreate(curve='Footprints', floors=3, style='residential')`; from a mesh:
  `buildingCreate(mesh='Block', style='warehouse')`.
- Output: a group `<settlement>_Buildings` with one live mesh per 64 m cell (four per Settlement World Partition cell),
  `cell=` to change, `standalone=True` for one mesh per building.
- Styles: `buildingStyles()` lists them; `buildingStyles(style='colonial')` gives the JSON to copy.
  Settlement's style guesses name them (police program -> policeStation). Regional styles go in
  `<workspace>/BuildingStyles`; a style id the library lacks falls back to the nearest shipped one with a warning.

## Ids

- Building id: `building:<lot key hex>` (Settlement lotId), or a hash of the curve/OSM id or mesh.
- Slot id: `<building id>/t<tier>/f<facade>/l<floor>/b<bay>/<kind>`, e.g. `building:1a2b/t0/f0/l0/b2/door`.
  Facades are footprint edges (f3x is a chamfered corner, h0f2 a courtyard edge); `t<n>/roof`, `t0/f1/cornice`,
  `t0/f0/l0/sign`, `site/gate`, `site/pool`.
- Ids stay the same when anything else changes; running buildingCreate again keeps edits, slot overrides and pins.

## Edit

- `buildingInfo(id, bays=True, modules=True)`: style, floors, palette, facades, bays (paths and corners), modules
  with slot ids, floor plates, core, openings.
- `buildingEdit(id, floors=3, style='seafront', seed=2, bayWidth=300, roof='hip', palette={'render': '#e4572e'})`.
- Bays: `buildingEdit(id, bays={'t0/f0/l0/b2': 'door'})` (window, door, shopfront, shopdoor, wall, arcade, garage);
  parts: `toggles={'t0/f0/l1/b1': {'balcony': True}}` (balcony, awning, ac, grille, shutter); `facadeBays={'t0/f0': 5}`;
  `clearBays=[...]`, `clear=True`.
- District: `buildingStyle(polygon=[[x, z], ...], style='colonial')`; `buildingAge(polygon=..., dirt=0.7)` or
  `preset='Grunge'` (Weathering).
- Swap parts with W0 slots: `slotRule('add', type='sign.storefront', poolTag='fictional_signs')`,
  `slotSet(id='building:1a2b/t0/f0/l0/b2/door', unrealClass='/Game/BP/BP_Door.BP_Door_C', data={'locked': True})`.
  Shell parts (wall.bay, cornice, pilaster, plinth, roof.*) drop out of the shell when a rule gives them a mesh.
- Pin a building so regeneration leaves it: `slotPin('owner:building:1a2b')`.
- Sign text is in each sign slot's data (`text`), from W1's fictional business names; never a real name.

## Check and ship

- `buildingCheck()`: modulesIntersecting, doorNotReachingGround, overlappingNeighbours, slotFloating, realName, and the
  interior issues below.
- LOD: `buildingLod(lod='far')` per mesh; `buildingProxies()` makes `<mesh>_HLOD` and `<mesh>_Far` for export.
- Unreal: `buildingSend()` sends shells (Nanite), proxies, slots and per-building colours (MF_LilaBuildingVariation),
  interiors as Level Instances, and window glass (fake rooms where there is no interior).
- Trim sheet: `buildingTrimSheet()` copies and bakes LilaBuilding.lilatrim.

## Interiors

- `buildingMakeEnterable(id)` (or a list): rooms from the building's own grammar, by program (shop, house, apartment,
  office, hotel, warehouse, police, mansion, bar, garage, church; `generic` rooms when the use is none of them, said in
  `warnings`), a stair core with a lift on taller buildings, doors, the facade's windows cut through, furnishing and
  ceiling lamp slots. `program=` forces one. The building is pinned against regeneration (rules still swap its slots); its street doors that open into rooms get the slot
  tag `entry` (`slotRule('add', type='door', tag='entry', unrealClass='/Game/BP/BP_Door.BP_Door_C')`). When no door opens
  into a room one is painted on the street (`entryAdded`).
- `buildingInterior(id)`: rooms (`l0/sales`, `l1/bedroom2`, `l3/u2-living`), doors (`l0/door/backroom+sales`), stairs,
  entries, room meshes and whether each is hand-edited; `check=True` adds the issues; `programs=True` lists programs,
  room kinds and presets.
- Layout: `buildingRoom(id, room='l0/backroom', kind='kitchen')`, `split='auto'|'u'|'v', at=0.5, newKind='office'`,
  `merge='l0/toilet'`, `grow='l0/backroom', by=80` (moves the shared wall), `swap='l0/x'`, `program='bar'`,
  `reset=True`. Doors: `buildingDoor(id, add=['l0/sales', 'l0/toilet'], at=[x, y, z])`, `remove='l0/door/a+b'`.
  Kept by room id in the building's edits; stale ones come back in `orphanEdits`.
- Room meshes live in `Interior_<hex>` beside the building mesh; model them freely: a hand-edited room is kept when the
  interior regenerates (`keptEdited`), `buildingMakeEnterable(id, reset=True)` regenerates it.
- Slots: owner `interior:<hex>`, types `door.interior(.double)`, `lamp.ceiling`, `lift`, `furnishing.*` (swap with rules
  or the library). Room presets: `buildingRoomPresets()`, `save={kind: {pieces: [{type, place}]}}`; `roomFurnish(mesh,
  kind=)` furnishes any floor (Set Dressing presets "Room: <Kind>" run it).
- Character: `buildingCharacterProfile(height=, radius=, maxStep=, maxSlope=)` (Unreal's default character unless set);
  stairs and doors are built to it. `buildingCheck(maxStep=30)` checks against another without rebuilding.
- `buildingCheck()` adds roomUnreachable, doorNoOpening, stairSteep, stairLow, doorNarrow, doorLow, noEntry,
  interiorMissing.
- Back: `buildingMakeExterior(id)` (undoable; layout edits stay unless clear=True). Windows of buildings without rooms
  carry slot data `interior: card, room: <kind>` for fake interiors.
- Unreal: `buildingSend(interiors='levelInstance'|'dataLayer'|'none')`.
- `buildingInfo` still gives the raw hooks: `plates`, `core`, `openings`, `enterable`.

## Site kits

- Site kits use the styles (mausoleum, portShed, packingPlant, hangar) through `buildingCreate(envelopes=[...], style=...)`.
)";
} // namespace

void registerAgent(lila::ext::Context& ctx)
{
    lila::mcp::Server::get().addResource(ctx, {.uri = "lila://guide/buildings",
                                               .name = "Buildings guide",
                                               .description = "Procedural buildings: make them from settlements, curves or OSM, ids, edits, bay paint, "
                                                              "styles, slots, enterable interiors and room layout, checks, LOD and Unreal. Read before "
                                                              "making or editing buildings.",
                                               .read = [] { return std::string(kGuide); }});
}
} // namespace buildings
