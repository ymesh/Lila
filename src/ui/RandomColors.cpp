#include "ui/ViewportDisplay.h"

#include "core/Events.h"
#include "core/PrefStore.h"
#include "io/SceneFile.h"
#include "ops/Command.h"
#include "ops/CommandOptions.h"
#include "ops/Undo.h"
#include "scene/Graph.h"
#include "scene/Materials.h"

#include <nlohmann/json.hpp>

namespace lila::ui::viewportDisplay
{
namespace
{
using json = nlohmann::json;

std::vector<scene::Node*> bakeTargets(scene::Graph& g, const Args& a)
{
    std::vector<scene::Node*> out;
    auto addMesh = [&](scene::NodeId id) {
        if (scene::Node* n = g.node(id); n && n->type().isA("mesh") && std::find(out.begin(), out.end(), n) == out.end())
            out.push_back(n);
    };
    for (const json& v : a.positional)
        if (v.is_string())
        {
            const std::vector<scene::NodeId> found = g.findAll(v.get<std::string>());
            if (found.empty())
                throw CommandError("bakeRandomColors: no object named " + v.get<std::string>());
            for (scene::NodeId id : found)
                addMesh(id);
        }
    if (a.positional.empty())
    {
        for (scene::NodeId id : g.selection())
            addMesh(id);
        if (out.empty())
            for (scene::NodeId id : g.nodes())
                addMesh(id);
    }
    return out;
}
} // namespace

void registerRandomColors()
{
    auto setSeed = [](int seed) { PrefStore::get().set("general", kRandomColorsSeed, seed); };
    setSeed(0);
    io::registerUiSection(
        "randomColors", [] { return randomSeed() ? json{{"seed", randomSeed()}} : json(); },
        [setSeed](const json& j) { setSeed(j.is_object() ? j.value("seed", 0) : 0); });
    events::subscribe(events::Event::SceneNew, [setSeed](const events::EventData&) { setSeed(0); });

    using T = ArgType;
    ops::registerCommand({
        .name = "bakeRandomColors",
        .doc = "Keeps the Random Colors display colours: a material per mesh with its colour, assigned to it, or a vertex color set filled "
               "with it. Works on the given meshes, else the selected ones, else every mesh. Returns the materials or color sets.",
        .positionalDoc = "meshes",
        .flags = {{"to", "", T::String, "materials", "materials or vertexColors.", {"materials", "vertexColors"}},
                  {"palette", "", T::String, "viewport", "viewport (the display's palette and seed), contrast or pastel.",
                   {"viewport", "contrast", "pastel"}},
                  {"seed", "", T::Int, 0, "Colour seed for the contrast and pastel palettes.", {}, {}, 0.0, 99999.0},
                  {"materialPrefix", "", T::String, "M_Random", "Material names: the prefix, then the mesh name. A material of that name is reused."},
                  {"colorSet", "", T::String, "randomColor", "Vertex color set to fill, made when missing."}},
        .run = [](const Args& a) -> json {
            scene::Graph& g = scene::Graph::current();
            const std::vector<scene::Node*> meshes = bakeTargets(g, a);
            if (meshes.empty())
                throw CommandError("bakeRandomColors: no meshes");
            const std::string to = a.get<std::string>("to", "materials");
            const std::string paletteName = a.get<std::string>("palette", "viewport");
            const bool viewport = paletteName != "pastel" && paletteName != "contrast";
            const RandomPalette palette = viewport ? randomPalette() : paletteName == "pastel" ? RandomPalette::Pastel : RandomPalette::Contrast;
            const int seed = viewport ? randomSeed() : a.get<int>("seed", 0);
            const std::string prefix = a.get<std::string>("materialPrefix", "M_Random");
            const std::string colorSet = a.get<std::string>("colorSet", "randomColor");

            UndoChunk chunk("Bake Random Colors");
            json out = json::array();
            for (scene::Node* n : meshes)
            {
                const glm::vec3 c = randomColor(n->name(), palette, seed);
                const std::string path = n->path();
                if (to == "vertexColors")
                {
                    ops::execute("vertexColorFill",
                                 {{"args", {path}}, {"colorSet", colorSet}, {"color", {c.r, c.g, c.b, 1.0f}}, {"target", "all"}});
                    out.push_back(path + "." + colorSet);
                    continue;
                }
                std::string mat = prefix + n->name();
                if (scene::materials::find(g, mat))
                    ops::execute("setAttr", {{"args", {mat + ".baseColor", {c.r, c.g, c.b}}}});
                else
                    mat = ops::execute("createMaterial", {{"args", {mat}}, {"baseColor", {c.r, c.g, c.b}}}).get<std::string>();
                ops::execute("assignMaterial", {{"args", {mat, path}}});
                out.push_back(mat);
            }
            return out;
        },
    });
    ops::options::Override o;
    o.enabledWhen["seed"] = {"palette", {"contrast", "pastel"}};
    o.enabledWhen["materialPrefix"] = {"to", {"materials"}};
    o.enabledWhen["colorSet"] = {"to", {"vertexColors"}};
    ops::options::setOverride("bakeRandomColors", std::move(o));
}
} // namespace lila::ui::viewportDisplay
