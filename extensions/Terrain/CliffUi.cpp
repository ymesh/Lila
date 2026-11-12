// Level > Build > Cliffs and the Level Toolkit's Cliffs section.

#include "CliffScene.h"

#include <lila/Actions.h>
#include <lila/Extension.h>
#include <lila/LevelToolkit.h>
#include <lila/Menus.h>

using namespace lila;
using namespace lila::ui;

namespace terrain::cliffs
{
void registerCliffUi(ext::Context& ctx)
{
    const struct
    {
        const char* id;
        const char* label;
        const char* command;
        const char* doc;
        const char* python;
    } commands[] = {
        {"cliffs.find", "Find Cliffs", "terrainCliffs", "Find the selected terrain's cliffs as regions cliff_1, cliff_2, ...", "terrainCliffs('Terrain', slope=50)"},
        {"cliffs.mesh", "Cliff Mesh", "terrainCliffMesh", "Build displaced, layered cliff meshes over the selected terrain's cliffs.",
         "terrainCliffMesh('Terrain', region='all')"},
        {"cliffs.extrude", "Extrude Cliff", "terrainCliffExtrude", "Raise a cliff wall along a curve, with a plateau behind and the base cut in front.",
         "terrainCliffExtrude('Terrain', curve='Escarpment', height=2000)"},
        {"cliffs.kit", "Cliff Kit", "terrainCliffKit", "Fit your own rock and cliff pieces along a cliff region.", "terrainCliffKit('Terrain', region='cliff_1', kit=['Rock_A'])"},
        {"cliffs.scatter", "Cliff Scatter", "terrainCliffScatter", "Scatter boulders, rocks and debris on cliff ledges and talus by tiers.",
         "terrainCliffScatter('Terrain')"},
        {"cliffs.send", "Send Cliffs", "terrainSendCliffs", "Send the cliffs to Unreal as World Partition tiles, one at a time.", "terrainSendCliffs('Terrain')"},
    };
    for (const auto& c : commands)
        actions::add(ctx, {.id = c.id, .label = c.label, .category = "Terrain", .doc = c.doc, .python = c.python, .command = c.command, .call = nlohmann::json::object()});

    menus::addItem(ctx, {.id = "cliffs.menu", .menu = levelToolkit::kMenu, .kind = menus::Kind::Submenu, .label = "Cliffs", .before = levelToolkit::kDressSection});
    int separators = 0;
    for (const char* a : {"cliffs.find", "cliffs.mesh", "cliffs.extrude", "", "cliffs.kit", "cliffs.scatter", "", "cliffs.send"})
    {
        if (*a)
            menus::addItem(ctx, {.id = std::string("cliffs.menu.") + a, .menu = "Level/Cliffs", .action = a});
        else
            menus::addItem(ctx, {.id = "cliffs.menu.sep" + std::to_string(separators++), .menu = "Level/Cliffs", .kind = menus::Kind::Separator});
    }

    // Daily: find and mesh as tiles; extrude, kit, scatter and the send as buttons below them.
    levelToolkit::add(ctx, {.id = "cliffs",
                            .title = "Cliffs",
                            .items = {{"cliffs.find", "scan", "Find", "Cliffs"}, {"cliffs.mesh", "mountain", "Mesh", "Cliff Mesh"}, {"cliffs.extrude", "chart-area-line", "Extrude"}, {},
                                      {"cliffs.kit", "box-multiple", "Kit"}, {"cliffs.scatter", "grain", "Scatter"}, {"cliffs.send", "send", "Unreal"}},
                            .order = 12,
                            .docs = "terrain-cliffs"});
}
} // namespace terrain::cliffs
