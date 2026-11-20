// grungeSend: the grunge looks of meshes as Unreal material instances of MF_LilaGrunge's generated materials. The masks
// themselves travel with the mesh (vertex colour and UV sets the grunge modifier writes) on any mesh send.

#include "Masks.h"

#include <lila/Command.h>
#include <lila/Deformer.h>
#include <lila/Extension.h>
#include <lila/Graph.h>
#include <lila/Mesh.h>
#include <lila/Profile.h>
#include <lila/UnrealLink.h>

#include <algorithm>
#include <cctype>

using namespace lila;
using namespace lila::scene;

namespace weathering::masks
{
namespace
{
/** Lila's mask attribute prefixes and the parameter names MF_LilaGrunge gives them. */
constexpr std::array<std::pair<const char*, const char*>, 6> kUnrealMasks = {
    {{"edge", "Edge"}, {"cavity", "Cavity"}, {"ao", "Occlusion"}, {"dust", "Dust"}, {"streak", "Streaks"}, {"fade", "Fade"}}};

json rgb(const glm::dvec3& c) { return json::array({c.x, c.y, c.z}); }

std::string instanceName(const Node& mesh)
{
    std::string n = "MI_Grunge_" + mesh.name();
    std::replace_if(n.begin(), n.end(), [](char c) { return !std::isalnum(static_cast<unsigned char>(c)) && c != '_'; }, '_');
    return n;
}

/** A grunge node's look as MF_LilaGrunge instance parameters, with the noise seed offsets Lila's viewport uses. */
json instanceParams(const Node& grunge, const Node& mesh, json& warnings)
{
    const auto at = [&](const std::string& n) { return grunge.type().attrIndex(n); };
    json scalars = json::object(), vectors = json::object();
    const int64_t seed = grunge.as<int64_t>(at("seed"));
    for (size_t k = 0; k < kUnrealMasks.size(); ++k)
    {
        const std::string l = kUnrealMasks[k].first, u = kUnrealMasks[k].second;
        scalars[u + "Enabled"] = grunge.as<bool>(at(l + "Enabled")) ? 1.0 : 0.0;
        for (const char* s : {"Level", "Softness", "Breakup", "NoiseScale", "Cut", "Outline", "Strength", "Roughness"})
            scalars[u + s] = grunge.as<double>(at(l + s));
        scalars[u + "Bands"] = double(grunge.as<int64_t>(at(l + "Bands")));
        scalars[u + "Multiply"] = grunge.as<bool>(at(l + "Multiply")) ? 1.0 : 0.0;
        scalars[u + "Seed"] = double(uint32_t(seed * 7 + int64_t(k)) % 4096u) * 17.31;
        vectors[u + "Color"] = rgb(grunge.as<glm::dvec3>(at(l + "Color")));
        vectors[u + "Low"] = rgb(grunge.as<glm::dvec3>(at(l + "Low")));
    }
    vectors["GrungeOutlineColor"] = rgb(grunge.as<glm::dvec3>(at("outlineColor")));
    scalars["EdgeWidth"] = grunge.as<double>(at("edgeWidth"));
    scalars["CavityWidth"] = grunge.as<double>(at("cavityWidth"));
    scalars["AoWidth"] = grunge.as<double>(at("aoRadius")) * 0.5;
    scalars["FadeHeight"] = grunge.as<double>(at("fadeHeight"));
    scalars["StreakLength"] = grunge.as<double>(at("streakLength"));
    const std::string layout = grunge.as<std::string>(at("unrealLayout"));
    if (layout == "off" || layout.empty())
        warnings.push_back(mesh.name() + ": its masks go to no channel (unrealChannels is off), so the instance shows no grunge");
    else if (grunge.as<int64_t>(at("uvSet")) != 3)
        warnings.push_back(mesh.name() + ": the generated materials read UV 3 onwards (edge distances in UV 4-5, or 6-7 in the uv layout) but its masks start at uv" +
                           std::to_string(grunge.as<int64_t>(at("uvSet"))) + "; wire GrungeMasksA/B yourself or set uvSet=3");
    if (layout == "uv")
        warnings.push_back(mesh.name() + ": it has its own vertex colours, so its masks fill uv3-uv7 and cavity, occlusion, fade and streaks are "
                                         "per vertex in Unreal (per pixel needs the vertex colour layout)");
    const json channels = layout == "uv" ? json{{"masks", "uv3-uv5"}, {"edgeDistance", "uv6-uv7"}}
                                         : json{{"masks", "vertex colour + uv3"}, {"edgeDistance", "uv4-uv5"}, {"fields", "uv6-uv7"}};
    return {{"name", instanceName(mesh)}, {"layout", layout == "uv" ? "uv" : "color"}, {"channels", channels}, {"scalars", scalars}, {"vectors", vectors}};
}
} // namespace

void registerMaskUnreal(ext::Context& ctx)
{
    ctx.registerCommand({
        .name = "grungeSend",
        .doc = "Send meshes' grunge looks to Unreal: builds MF_LilaGrunge with its materials (M_LilaGrunge reads vertex colour and "
               "UV 3, M_LilaGrungeUV reads UV 3 to 5; edge wear takes the per-pixel distance to sharp edges from UV 4-5, or 6-7) when missing or older, and a material instance per mesh (MI_Grunge_<mesh> in "
               "/Game/LilaLink/Grunge) with the mesh's levels, bands, cuts, outlines and colours. The masks travel with the mesh "
               "itself on any mesh send. To use your own material, drop MF_LilaGrunge into it and feed BaseColor and Roughness "
               "through. dryRun returns the instances without sending. Returns {materials, instances, warnings}; dryRun instances name the channels they read.",
        .positionalDoc = "meshes",
        .flags = {{"regenerate", "rg", ArgType::Bool, false, "Rebuild the function and materials even when current."},
                  {"dryRun", "dr", ArgType::Bool, false, "Only return what would be sent."}},
        .undoable = false,
        .run = [](const Args& a) -> json {
            LILA_ZONE();
            Graph& g = Graph::current();
            g.evaluate();
            json instances = json::array(), warnings = json::array();
            for (const std::string& name : a.objects())
            {
                Node* mesh = g.find(name);
                if (!mesh)
                    throw CommandError("grungeSend: no node named " + name);
                Node* grunge = nullptr;
                for (NodeId id : modifiers::stack(g, mesh->id()))
                    if (Node* n = g.node(id); n && n->type().name == kGrungeType)
                        grunge = n;
                if (!grunge)
                    throw CommandError("grungeSend: " + mesh->name() + " has no grunge masks (grungeMask adds them)");
                instances.push_back(instanceParams(*grunge, *mesh, warnings));
            }
            if (instances.empty())
                throw CommandError("grungeSend: select or name meshes with grunge masks");
            if (a.get<bool>("dryRun", false))
                return {{"instances", instances}, {"warnings", warnings}};
            try
            {
                link::Link::get().ensureConnected();
            }
            catch (const std::exception& e)
            {
                throw CommandError(std::string("grungeSend: Unreal is not connected (") + e.what() + ")");
            }
            auto client = link::Link::get().client();
            const json features = link::Link::get().status().value("features", json::array());
            if (!features.is_array() || std::find(features.begin(), features.end(), json("grunge")) == features.end())
                throw CommandError("grungeSend: this LilaLink has no grunge feature; update the plugin");
            link::RpcResult r = client->call("grungeMaterials", {{"regenerate", a.get<bool>("regenerate", false)}, {"instances", instances}},
                                             link::importCallOptions());
            if (!r.ok)
                throw CommandError("grungeSend: grungeMaterials: " + r.error);
            if (r.result.contains("warnings") && r.result["warnings"].is_array())
                for (const json& w : r.result["warnings"])
                    warnings.push_back(w);
            return {{"materials", r.result.value("materials", json::object())}, {"instances", r.result.value("instances", json::object())},
                    {"warnings", warnings}};
        },
        .examples = {"grungeSend('Wall')", "grungeSend('Wall', dryRun=True)"},
    });
}
} // namespace weathering::masks
