#include "vcolor/VertexColors.h"

#include "geo/Attribute.h"
#include "geo/ColorSets.h"
#include "ops/Command.h"
#include "ops/Undo.h"
#include "scene/Graph.h"
#include "scene/Materials.h"
#include "scene/MeshNodes.h"
#include "scene/Modifiers.h"

#include <algorithm>
#include <map>
#include <set>

namespace lila::vcolor
{
using scene::Graph;
using scene::MeshPtr;
using scene::Node;

namespace
{
constexpr const char* kChannelNames = "rgba";

std::vector<Node*> maskTargets(Graph& g, const Args& a)
{
    std::vector<Node*> out;
    auto add = [&](scene::NodeId id) {
        if (Node* n = g.node(id); n && n->type().isA("mesh") && std::ranges::find(out, n) == out.end())
            out.push_back(n);
    };
    for (const json& v : a.positional)
        if (v.is_string())
        {
            const std::vector<scene::NodeId> found = g.findAll(v.get<std::string>());
            if (found.empty())
                throw CommandError("vertexColorMasks: no object named " + v.get<std::string>());
            for (scene::NodeId id : found)
                add(id);
        }
    if (a.positional.empty())
    {
        for (scene::NodeId id : g.selection())
            add(id);
        if (out.empty())
            for (scene::NodeId id : g.nodes())
                add(id);
    }
    return out;
}

/** The material slot name of every face, by face id; faces without the attribute use the mesh's first slot. */
std::vector<std::string> faceMaterials(const geo::Mesh& m)
{
    std::vector<std::string> out(m.faceCapacity());
    const std::string fallback = m.materialNames().empty() ? scene::materials::kDefaultSlot : m.materialNames()[0];
    const auto ids = m.faceAttrs().has(geo::attr::kMaterial) ? m.faceAttrs().get<int32_t>(geo::attr::kMaterial) : std::span<const int32_t>();
    for (geo::Index f = 0; f < m.faceCapacity(); ++f)
        out[f] = f < ids.size() ? scene::materials::slotName(m, ids[f]) : fallback;
    return out;
}
} // namespace

void registerMaskCommands()
{
    using T = ArgType;
    ops::registerCommand({
        .name = "vertexColorMasks",
        .doc = "Writes a 0/1 mask per object or per material into vertex color channels, packed into sets named prefix0, prefix1, ... "
               "Masks are numbered by name, alphabetically, so the bake's masks map packs the same names into the same channels. "
               "Every mesh gets every set, so one material can read them all. Works on the given meshes, else the selected ones, "
               "else every mesh. Returns {name: \"set.channel\"}.",
        .positionalDoc = "meshes",
        .flags = {{"source", "", T::String, "object", "What gets a mask: object or material.", {"object", "material"}},
                  {"channels", "", T::Int, 4, "Masks per set: 3 (RGB) or 4 (RGBA).", {}, {}, 3.0, 4.0},
                  {"prefix", "", T::String, "mask", "Set names: the prefix, then the set's number."}},
        .run = [](const Args& a) -> json {
            Graph& g = Graph::current();
            const std::vector<Node*> meshes = maskTargets(g, a);
            if (meshes.empty())
                throw CommandError("vertexColorMasks: no meshes");
            const bool byMaterial = a.get<std::string>("source", "object") == "material";
            const int perSet = std::clamp(a.get<int>("channels", 4), 3, 4);
            const std::string prefix = a.get<std::string>("prefix", "mask");
            if (prefix.empty())
                throw CommandError("vertexColorMasks: prefix is empty");

            g.evaluate();
            std::vector<std::shared_ptr<geo::Mesh>> bases;
            std::set<std::string> names;
            for (Node* n : meshes)
            {
                const MeshPtr& m = std::get<MeshPtr>(g.storedValue(scene::modifiers::editPlug(g, n->id())));
                bases.push_back(std::make_shared<geo::Mesh>(m ? *m : geo::Mesh()));
                if (byMaterial)
                    for (const std::string& s : faceMaterials(*bases.back()))
                        names.insert(s);
                else
                    names.insert(n->name());
            }
            std::map<std::string, int> index;
            for (const std::string& s : names)
                index.emplace(s, int(index.size()));
            const int setCount = (int(names.size()) + perSet - 1) / perSet;

            UndoChunk chunk("Vertex Color Masks");
            for (size_t i = 0; i < meshes.size(); ++i)
            {
                geo::Mesh& m = *bases[i];
                const std::vector<std::string> faceNames = byMaterial ? faceMaterials(m) : std::vector<std::string>();
                for (int s = 0; s < setCount; ++s)
                {
                    const std::string set = prefix + std::to_string(s);
                    std::span<glm::vec4> values = geo::colors::add(m, set, glm::vec4(0.0f));
                    std::ranges::fill(values, glm::vec4(0.0f));
                    geo::colors::setData(m, set, true);
                    for (geo::Index c = 0; c < m.cornerCapacity(); ++c)
                    {
                        if (!m.cornerValid(c))
                            continue;
                        const int id = index.at(byMaterial ? faceNames[m.cornerFace(c)] : meshes[i]->name());
                        if (id / perSet == s)
                            values[c][id % perSet] = 1.0f;
                    }
                    if (perSet == 3)
                        for (glm::vec4& v : values)
                            v.a = 1.0f;
                }
                g.setValue(scene::modifiers::editPlug(g, meshes[i]->id()), MeshPtr(std::move(bases[i])));
            }
            json out = json::object();
            for (const auto& [name, id] : index)
                out[name] = prefix + std::to_string(id / perSet) + "." + kChannelNames[id % perSet];
            return out;
        },
    });
}
} // namespace lila::vcolor
