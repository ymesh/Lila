#include "tex/ShadingChannels.h"

#include "bake/Targets.h"
#include "ops/Command.h"
#include "scene/Graph.h"
#include "tex/TextureSet.h"

#include <algorithm>

namespace lila::tex::shading
{
namespace
{
using P = PortType;
const ChannelInfo kChannels[] = {
    {"sheenColor", P::Color, 8, {0, 0, 0, 1}, "Sheen Color", "fuzz_color", "KHR_materials_sheen.sheenColorTexture", "FuzzColor", "SHC"},
    {"sheenRoughness", P::Gray, 8, {0.5f, 0, 0, 1}, "Sheen Roughness", "fuzz_roughness", "KHR_materials_sheen.sheenRoughnessTexture.a", "FuzzRoughness", "SHR"},
    {"coat", P::Gray, 8, {0, 0, 0, 1}, "Coat", "coat_weight", "KHR_materials_clearcoat.clearcoatTexture.r", "ClearCoat", "CC"},
    {"coatRoughness", P::Gray, 8, {0.05f, 0, 0, 1}, "Coat Roughness", "coat_roughness", "KHR_materials_clearcoat.clearcoatRoughnessTexture.g",
     "ClearCoatRoughness", "CCR"},
    {"coatNormal", P::Color, 8, {0.5f, 0.5f, 1, 1}, "Coat Normal", "geometry_coat_normal", "KHR_materials_clearcoat.clearcoatNormalTexture",
     "ClearCoatNormal", "CCN"},
    {"subsurfaceColor", P::Color, 8, {0, 0, 0, 1}, "Subsurface Color", "subsurface_color",
     "KHR_materials_diffuse_transmission.diffuseTransmissionColorTexture", "SSSMFP", "SSC"},
    {"subsurfaceRadius", P::Gray, 16, {1, 0, 0, 1}, "Subsurface Radius", "subsurface_radius", "KHR_materials_volume.thicknessTexture.g", "SSSMFPScale",
     "SSR"},
    {"anisotropy", P::Gray, 8, {0, 0, 0, 1}, "Anisotropy", "specular_anisotropy", "KHR_materials_anisotropy.anisotropyTexture.b", "Anisotropy", "AN"},
    {"anisotropyDirection", P::Color, 8, {1, 0.5f, 0.5f, 1}, "Anisotropy Direction", "geometry_tangent", "KHR_materials_anisotropy.anisotropyTexture.rg",
     "Tangent", "AND"},
    {"transmission", P::Gray, 8, {0, 0, 0, 1}, "Transmission", "transmission_weight", "KHR_materials_transmission.transmissionTexture.r", "Transmission",
     "TR"},
    {"ior", P::Gray, 8, {0.25f, 0, 0, 1}, "IOR", "specular_ior", "KHR_materials_ior.ior", "Refraction", "IOR"},
    {"specularWeight", P::Gray, 8, {1, 0, 0, 1}, "Specular Weight", "specular_weight", "KHR_materials_specular.specularTexture.a", "Specular", "SW"},
    {"specularColor", P::Color, 8, {1, 1, 1, 1}, "Specular Color", "specular_color", "KHR_materials_specular.specularColorTexture", "SpecularColor", "SC"},
};
} // namespace

std::span<const ChannelInfo> channels() { return kChannels; }

const ChannelInfo* find(std::string_view name)
{
    for (const ChannelInfo& c : kChannels)
        if (name == c.name)
            return &c;
    return nullptr;
}

std::string familyOf(const bake::TargetProfile& target)
{
    std::string n = target.name;
    std::ranges::transform(n, n.begin(), [](unsigned char c) { return char(std::tolower(c)); });
    if (n.find("openpbr") != std::string::npos)
        return "openpbr";
    if (n.find("gltf") != std::string::npos || n.find("godot") != std::string::npos)
        return "gltf";
    if (n.find("unreal") != std::string::npos)
        return "unreal";
    return "";
}

std::string nameFor(const ChannelInfo& c, std::string_view family)
{
    if (family == "openpbr")
        return c.openPbr;
    if (family == "gltf")
        return c.gltf;
    if (family == "unreal")
        return c.unreal;
    return c.name;
}

void addExportMaps(std::vector<bake::TargetMap>& maps, const bake::TargetProfile& target)
{
    const std::string family = familyOf(target);
    for (const ChannelInfo& c : kChannels)
    {
        if (std::ranges::any_of(maps, [&](const bake::TargetMap& m) { return m.settings.map == c.name; }))
            continue;
        bake::TargetMap m;
        m.settings.map = c.name;
        // OpenPBR files are named after the input; the others keep their usual short suffixes.
        m.settings.suffix = family == "openpbr" ? std::string(c.openPbr) : std::string(c.suffix);
        if (c.bits > 8)
            m.settings.bits = c.bits;
        m.slot = nameFor(c, family);
        m.optional = true;
        maps.push_back(std::move(m));
    }
}

void registerCommands()
{
    using T = ArgType;
    std::vector<std::string> names;
    for (const ChannelInfo& c : kChannels)
        names.push_back(c.name);
    ops::registerCommand({
        .name = "texAddShadingChannels",
        .doc = "Add shading channels to a texture set: sheen, coat, subsurface, anisotropy, transmission, IOR and specular. Each becomes a "
               "Material Output input, shades in the viewport and the path-traced view, and exports under the name the preset's "
               "renderer uses. The ior channel holds (IOR - 1) / 2. Returns the channels added.",
        .positionalDoc = "texture set",
        .flags = {{"channels", "c", T::List, nullptr, "Channel names; all of them when left out.", names}},
        .run = [names](const Args& a) -> json {
            if (a.positional.empty() || !a.positional[0].is_string())
                throw CommandError("texAddShadingChannels: name a texture set");
            scene::Graph& g = scene::Graph::current();
            scene::Node* set = findSet(g, a.positional[0].get<std::string>());
            if (!set)
                throw CommandError("texAddShadingChannels: no texture set " + a.positional[0].get<std::string>());
            std::vector<std::string> wanted = names;
            if (a.has("channels"))
                wanted = a.flags["channels"].get<std::vector<std::string>>();
            SetSettings s = readSettings(*set);
            json added = json::array();
            for (const std::string& w : wanted)
            {
                const ChannelInfo* c = find(w);
                if (!c)
                    throw CommandError("texAddShadingChannels: no shading channel " + w);
                if (s.channel(w))
                    continue;
                s.channels.push_back({c->name, c->type, c->bits});
                added.push_back(w);
            }
            if (added.empty())
                return added;
            writeSettings(g, *set, s);
            // Unlinked, an extra Material Output input reads 0: start each at the value that leaves the shading as it was.
            Graph graph = readGraph(*set);
            for (GNode& n : graph.nodes)
                if (n.type == kMaterialOutputType)
                    for (const json& name : added)
                    {
                        const ChannelInfo* c = find(name.get<std::string>());
                        if (n.values.contains(c->name))
                            continue;
                        if (c->type == PortType::Gray)
                            n.values[c->name] = c->fallback[0];
                        else
                            n.values[c->name] = {c->fallback[0], c->fallback[1], c->fallback[2], c->fallback[3]};
                    }
            writeGraph(g, *set, graph);
            return added;
        },
        .examples = {"texAddShadingChannels(\"RockTex\")", "texAddShadingChannels(\"RockTex\", channels=[\"coat\", \"coatRoughness\"])"},
    });
}
} // namespace lila::tex::shading
