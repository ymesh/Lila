// Level > Terrain and the Level Toolkit's Terrain section.

#include "Terrain.h"

#include <lila/Actions.h>
#include <lila/Command.h>
#include <lila/Extension.h>
#include <lila/FileDialog.h>
#include <lila/LevelToolkit.h>
#include <lila/Log.h>
#include <lila/Menus.h>
#include <lila/Names.h>
#include <lila/OptionBox.h>
#include <lila/ViewportTool.h>

#include <QApplication>

#include <tuple>

using namespace lila;
using namespace lila::ui;

namespace terrain
{
void registerUi(ext::Context& ctx)
{
    const struct
    {
        const char* id;
        const char* label;
        const char* command;
        const char* doc;
        const char* python;
    } commands[] = {
        {"terrain.create", "Terrain", "terrainCreate", "Make a fractal noise terrain as a geometry-nodes heightfield.", "terrainCreate(size=[100000, 100000])"},
        {"terrain.erode", "Erode Terrain", "terrainErode", "Carve rivers and shed steep slopes on the selected terrain.", "terrainErode('Terrain', iterations=80)"},
        {"terrain.addRiver", "Add River", "terrainAddRiver", "Carve a river along a curve, or every channel past a drainage threshold, into the selected terrain.",
         "terrainAddRiver('Terrain', auto=True, threshold=2e6)"},
        {"terrain.addLake", "Add Lake", "terrainAddLake", "Fill a basin of the selected terrain to its spill level, or dig a lake inside an outline.",
         "terrainAddLake('Terrain', auto=True)"},
        {"terrain.seaLevel", "Sea Level", "terrainSeaLevel", "Set the selected terrain's sea level, shore and beaches.", "terrainSeaLevel('Terrain', level=0)"},
        {"terrain.landform", "Landform Step", "terrainLandform", "Add an erosion or shaping step (debris, scree, rills, deposition, snow, filter, terrace, strata, ...) to the selected terrain.",
         "terrainLandform('Terrain', process='debris')"},
        {"terrain.toMesh", "Terrain to Mesh", "terrainToMesh", "A plain mesh copy of the selected terrain as it is now.", "terrainToMesh('Terrain', step=2)"},
        {"terrain.tiles", "Terrain Tiles", "terrainTiles", "Cut the selected terrain into LOD mesh tiles with collision.", "terrainTiles('Terrain', tileSize=257)"},
        {"terrain.bake", "Bake Terrain", "terrainBake", "Collapse the selected terrain's node graph into a .lilahf file.", "terrainBake('Terrain')"},
        {"terrain.export", "Export Terrain", "terrainExport", "Write the selected terrain to files for an engine or another tool.", "terrainExport('Terrain', target='files')"},
        {"terrain.sendToUnreal", "Send Terrain to Unreal", "terrainSendToUnreal", "Send the selected terrain to the connected Unreal editor as a Landscape.",
         "terrainSendToUnreal('Terrain')"},
        {"terrain.cave", "Cave", "terrainCave", "Carve a cave along a curve into the selected terrain.", "terrainCave('Terrain', curve='CaveLine', radius=300)"},
        {"terrain.split", "Split Volume", "terrainSplit", "Split the selected terrain's caves and overhangs into mesh pieces over holes.",
         "terrainSplit('Terrain', lods=3)"},
        {"terrain.liveLink", "Live Link Terrain", "terrainLiveLink", "Re-run the selected terrain whenever its source file is saved.", "terrainLiveLink('Terrain')"},
    };
    for (const auto& c : commands)
        actions::add(ctx, {.id = c.id, .label = c.label, .category = "Terrain", .doc = c.doc, .python = c.python, .command = c.command, .call = nlohmann::json::object()});

    actions::add(ctx, {.id = "terrain.import",
                       .label = "Import Terrain",
                       .category = "Terrain",
                       .run =
                           [] {
                               QString file = fileDialog::getOpenFileName(QApplication::activeWindow(), "Import Terrain", {},
                                                                          "Elevation and images (*.tif *.tiff *.hgt *.asc *.xyz *.png *.exr);;All files (*)");
                               if (!file.isEmpty())
                                   actions::runWithOptions("terrainImport", {{"args", {file.toStdString()}}});
                           },
                       .doc = "Make a terrain from a DEM tile or a painted height image.",
                       .python = "terrainImport('dem/N46E007.hgt')"});

    for (const auto& [id, label, command, doc, python] :
         {std::tuple{"terrain.importDem", "Real Place", "terrainImportDem", "Make a terrain from real elevation, by area or place name.",
                     "terrainImportDem(place='Matterhorn', size=[1200000, 1200000])"},
          std::tuple{"terrain.importOsm", "OSM Features", "terrainImportOsm",
                     "Drape OpenStreetMap roads, rivers, lakes, harbour works, buildings and places on a terrain, with fictional names, "
                     "and add land cover masks.",
                     "terrainImportOsm('area.osm', terrain='Terrain')"}})
    {
        const std::string cmd = command;
        actions::add(ctx, {.id = id, .label = label, .category = "Terrain", .run = [cmd] { actions::runWithOptions(cmd, nlohmann::json::object()); }, .doc = doc, .python = python});
    }

    {
        // The import's naming culture: a fixed list with "from where the data lies" first.
        ops::options::Override o;
        if (const ops::options::Override* existing = ops::options::findOverride("terrainImportOsm"))
            o = *existing;
        o.order = {"terrain", "kinds", "culture", "origin", "fetch", "landcover", "curves", "perFeature", "lift", "step", "name"};
        o.labels["culture"] = "Names";
        o.shown = {"origin", "kinds"};
        o.labels["origin"] = "Origin Lat, Lon";
        std::vector<std::pair<std::string, std::string>> cultures = {{"", "From Location"}};
        for (const std::string& c : names::cultures())
            cultures.push_back({c, names::cultureLabel(c)});
        o.choices["culture"] = cultures;
        ops::options::setOverride("terrainImportOsm", o);
    }

    registerSculptTool(ctx);
    actions::add(ctx, {.id = "terrain.sculpt",
                       .label = "Sculpt Terrain",
                       .category = "Terrain",
                       .run = [] { viewportTools::activate("terrain.sculptTool"); },
                       .doc = "Brush the selected terrain: raise, lower, smooth, flatten, noise, ramp, terrace, stamp, erode, uplift, relax and mask, "
                              "before or after erosion, or sculpt a region with the mesh sculpt brushes.",
                       .python = "terrainSculpt('Terrain', op='raise', points=[[0, 0]], radius=3000, strength=300)"});

    menus::addItem(ctx, {.id = "terrain.menu",.menu = levelToolkit::kMenu, .kind = menus::Kind::Submenu, .label = "Terrain",
                         .after = levelToolkit::kBuildSection});
    int separators = 0;
    for (const char* a : {"terrain.create", "terrain.import", "terrain.importDem", "terrain.importOsm", "terrain.liveLink", "terrain.erode", "terrain.landform", "terrain.sculpt", "terrain.cave", "terrain.split", "", "terrain.addRiver", "terrain.addLake", "terrain.seaLevel", "", "terrain.toMesh", "terrain.tiles", "terrain.bake", "terrain.export", "terrain.sendToUnreal"})
    {
        if (*a)
            menus::addItem(ctx, {.id = std::string("terrain.menu.") + a, .menu = "Level/Terrain", .action = a});
        else
            menus::addItem(ctx, {.id = "terrain.menu.sep" + std::to_string(separators++), .menu = "Level/Terrain", .kind = menus::Kind::Separator});
    }

    levelToolkit::add(ctx, {.id = "terrain",
                            .title = "Terrain",
                            .items = {{"terrain.create", "mountain", "Create", "Terrain"}, {"terrain.import", "file-import", "Import"}, {"terrain.erode", "droplet", "Erode", "Erode"}, {"terrain.sculpt", "brush", "Sculpt"}, {},
                                      {"terrain.toMesh", "grid-dots", "To Mesh"}, {"terrain.bake", "file-zip", "Bake"}, {"terrain.export", "file-export", "Export"},
                                      {"terrain.sendToUnreal", "send", "Unreal"}},
                            .order = 10});
}
} // namespace terrain
