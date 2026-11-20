// The Level > Weathering menu and toolkit section: the library of presets to drag onto meshes, and each effect as an
// action, so they are searchable, scriptable and get option boxes from their flags.

#include "Weathering.h"

#include <lila/Actions.h>
#include <lila/Command.h>
#include <lila/Extension.h>
#include <lila/LevelToolkit.h>
#include <lila/Menus.h>

using namespace lila;
using namespace lila::ui;

namespace weathering
{
constexpr const char* kWeatheringMenu = "Level/Weathering";

void registerWeatheringUi(ext::Context& ctx)
{
    struct Entry
    {
        const char* id;
        const char* label;
        const char* command;
        const char* icon;
        const char* doc;
        const char* python;
        std::vector<std::string> keywords;
    };
    const Entry entries[] = {
        {"weathering.baseDressing", "Base Dressing", "baseDressing", "stack-2",
         "Scatter pieces in a band round where the selection meets the ground: rubble, planks leaning on walls, weeds.",
         "baseDressing('Shed', items=['tag:weathering+rubble'])", {"scatter around", "debris", "rubble", "foot of wall", "skirt"}},
        {"weathering.groundDressing", "Ground Dressing", "groundDressing", "grain",
         "Scatter pieces over the selected ground, give it a ground material and darken it round walls and pieces.",
         "groundDressing('Ground', preset='Asphalt Crack')", {"scatter ground", "litter", "pebbles", "grass", "floor"}},
        {"weathering.groundTint", "Ground Tint", "groundTint", "droplet",
         "Darken the selected ground round what stands on it and wet its dips, live.", "groundTint('Ground')",
         {"ground colorize", "dirt", "wet", "puddle", "contact", "ambient occlusion"}},
        {"weathering.addPieces", "Add Pieces", "weatheringAddPieces", "books",
         "Make the selected meshes the library's pieces for a role (rubble, plank, ivy-leaf), so presets use them.",
         "weatheringAddPieces(['Chunk1', 'Chunk2'], role='rubble')", {"custom", "own meshes", "kit", "library", "replace placeholders"}},
        {"weathering.unreal", "Build Unreal Materials", "weatheringUnreal", "world-upload",
         "Build the weathering material functions and materials in the linked Unreal project.", "weatheringUnreal()",
         {"material function", "MF_LilaGroundTint", "unreal", "ue5"}},
    };
    for (const Entry& e : entries)
        actions::add(ctx, {.id = e.id,
                           .label = e.label,
                           .category = "Weathering",
                           .doc = e.doc,
                           .python = e.python,
                           .command = e.command,
                           .call = json::object(),
                           .icon = e.icon,
                           .keywords = e.keywords});
    for (const bool live : {false, true})
        actions::add(ctx, {.id = live ? "weathering.resume" : "weathering.pause",
                           .label = live ? "Resume Weathering" : "Pause Weathering",
                           .category = "Weathering",
                           .doc = live ? "Bring the selection's weathering back, rebuilt for the blocks as they are now."
                                       : "Hide the selection's weathering and turn off its grunge and chipping while you block out.",
                           .python = live ? "weatheringLive('Level')" : "weatheringLive('Level', live=False)",
                           .command = "weatheringLive",
                           .call = {{"live", live}},
                           .icon = live ? "player-play" : "player-pause",
                           .keywords = {"freeze weathering", "disable weathering", "performance", "blockout"}});
    // The library is the daily entry: drag a preset onto a mesh. The rest run one effect on the selection with options.
    levelToolkit::add(ctx, {.id = "weathering",
                            .title = "Weathering",
                            .items = {{"window.libraryPresets", "photo", "Library", "Weathering"},
                                      {"weathering.baseDressing", "stack-2", "Base", {}, "Dressing"},
                                      {"weathering.groundDressing", "grain", "Ground", {}, "Dressing"},
                                      {"weathering.groundTint", "droplet", "Tint", {}, "Dressing"},
                                      {"weathering.pile", "pyramid", "Pile", {}, "Dressing"},
                                      {"weathering.grungeMask", "spray", "Grunge", {}, "Surfaces"},
                                      {"weathering.edgeDecals", "border-style-2", "Edge Decals", {}, "Surfaces"},
                                      {"weathering.edgeChip", "hammer", "Chip", {}, "Surfaces"},
                                      {"weathering.ivy", "plant-2", "Ivy", {}, "Growth"},
                                      {"weathering.ivyBrush", "brush", "Ivy Brush", {}, "Growth"},
                                      {"weathering.roofing", "home", "Roofing", {}, "Generators"},
                                      {"weathering.windows", "window", "Windows", {}, "Generators"},
                                      {"weathering.wires", "line-dashed", "Wires", {}, "Generators"},
                                      {"weathering.rooftop", "building", "Rooftop", {}, "Generators"},
                                      {"weathering.truss", "building-broadcast-tower", "Truss", {}, "Generators"},
                                      {"weathering.pause", "player-pause", "Pause", {}, {}, true},
                                      {"weathering.resume", "player-play", "Resume", {}, {}, true},
                                      {"weathering.addPieces", "books", "Add Pieces", {}, {}, true},
                                      {"weathering.grungeBake", "", "Bake Grunge", {}, {}, true},
                                      {"weathering.unreal", "world-upload", "Unreal Materials", {}, {}, true},
                                      {"weathering.grungeSend", "", "Send Grunge", {}, {}, true},
                                      {"weathering.ivySend", "", "Send Ivy", {}, {}, true},
                                      {"weathering.interiorSend", "", "Send Interiors", {}, {}, true}},
                            .order = 44,
                            .docs = "weathering"});
    // The whole Weathering menu, in one order: every area registers only its actions.
    menus::addItem(ctx, {.id = "weathering.menu", .menu = levelToolkit::kMenu, .kind = menus::Kind::Submenu, .label = "Weathering",
                         .before = levelToolkit::kAssetsSection});
    const std::vector<std::pair<const char*, std::vector<const char*>>> sections = {
        {"", {"window.libraryPresets"}},
        {"Dressing", {"weathering.baseDressing", "weathering.groundDressing", "weathering.groundTint", "weathering.pile", "weathering.rooftop"}},
        {"Surfaces", {"weathering.grungeMask", "weathering.grungeBake", "weathering.edgeDecals", "weathering.edgeChip"}},
        {"Growth", {"weathering.ivy", "weathering.ivyBrush"}},
        {"Generators", {"weathering.roofing", "weathering.windows", "weathering.wires", "weathering.truss"}},
        {"Manage", {"weathering.pause", "weathering.resume", "weathering.addPieces", "weathering.unreal", "weathering.grungeSend", "weathering.ivySend", "weathering.interiorSend"}},
    };
    for (const auto& [title, ids] : sections)
    {
        if (*title)
            menus::addItem(ctx, {.id = std::string("weathering.menu.section.") + title, .menu = kWeatheringMenu, .kind = menus::Kind::Section, .label = title});
        for (const char* a : ids)
            if (actions::find(a))
                menus::addItem(ctx, {.id = std::string("weathering.menu.") + a, .menu = kWeatheringMenu, .action = a});
    }
}
} // namespace weathering
