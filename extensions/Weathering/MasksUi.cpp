// Level > Weathering entries for the live grunge masks, with their option boxes.

#include "Masks.h"

#include <lila/Actions.h>
#include <lila/Extension.h>
#include <lila/LevelToolkit.h>
#include <lila/Menus.h>
#include <lila/OptionBox.h>

using namespace lila;
using namespace lila::ui;

namespace weathering::masks
{
void registerMaskUi(ext::Context& ctx)
{
    actions::add(ctx, {.id = "weathering.grungeMask",
                       .label = "Grunge Masks",
                       .category = "Weathering",
                       .doc = "Live edge wear, cavity dirt, occlusion, dust, streaks and ground fade on the selected meshes, found from "
                              "their shape and their neighbours; no UVs or bake.",
                       .python = "grungeMask('Wall', style='toon')",
                       .command = "grungeMask",
                       .call = nlohmann::json::object(),
                       .icon = "spray",
                       .keywords = {"edge wear", "dirt", "grime", "grunge", "weathering", "curvature", "cavity", "ambient occlusion", "dust",
                                    "streaks", "drips", "smart mask", "generator", "vertex color", "procedural wear"}});
    actions::add(ctx, {.id = "weathering.grungeBake",
                       .label = "Bake Grunge",
                       .category = "Weathering",
                       .doc = "Bake the selected meshes' live grunge masks into texture layers with matching generators.",
                       .python = "grungeBake('Wall')",
                       .command = "grungeBake",
                       .call = nlohmann::json::object(),
                       .icon = "photo-down",
                       .keywords = {"bake grunge", "smart mask", "texture layers", "generators"}});
    actions::add(ctx, {.id = "weathering.grungeSend",
                       .label = "Send Grunge to Unreal",
                       .category = "Weathering",
                       .doc = "Send the selected meshes' grunge looks to Unreal as material instances of MF_LilaGrunge.",
                       .python = "grungeSend('Wall')",
                       .command = "grungeSend",
                       .call = nlohmann::json::object(),
                       .icon = "send",
                       .keywords = {"MF_LilaGrunge", "unreal grunge", "material instance"}});

    ops::options::Override o;
    o.order = {"style", "masks", "seed", "edgeWidth", "cavityWidth", "aoRadius", "streakLength", "fadeHeight", "groundPlane", "groundHeight",
               "neighbours", "radius", "unreal"};
    o.hidden = {"look", "edgeAngle"};
    o.sections = {{"edgeWidth", "Shape"}, {"neighbours", "Neighbours"}, {"unreal", "Unreal"}};
    o.choices = {{"style", {{"realistic", "Realistic"}, {"toon", "Toon"}}},
                 {"unreal", {{"auto", "Auto"}, {"color", "Vertex Colour"}, {"uv", "UV Sets"}, {"off", "Off"}}}};
    o.labels = {{"aoRadius", "Occlusion Radius"}, {"radius", "Search Radius"}, {"neighbours", "See Neighbours"}};
    o.ranges = {{"edgeWidth", {0.0, 50.0, 0.5, " cm"}},   {"cavityWidth", {0.0, 100.0, 1.0, " cm"}}, {"aoRadius", {0.0, 500.0, 5.0, " cm"}},
                {"streakLength", {0.0, 1000.0, 10.0, " cm"}}, {"fadeHeight", {0.0, 500.0, 5.0, " cm"}},  {"groundHeight", {-10000.0, 10000.0, 1.0, " cm"}},
                {"radius", {0.0, 2000.0, 10.0, " cm"}}};
    ops::options::setOverride("grungeMask", o);
    ctx.track("optionOverride:grungeMask", [] { ops::options::setOverride("grungeMask", {}); });

}
} // namespace weathering::masks
