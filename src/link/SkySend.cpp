#include "link/SkySend.h"

#include "core/Log.h"
#include "core/Paths.h"
#include "gn/Volume.h"
#include "link/RpcClient.h"
#include "link/UnrealLink.h"
#include "math/MathCore.h"
#include "math/Unreal.h"
#include "ops/Command.h"
#include "scene/Graph.h"
#include "scene/Materials.h"
#include "scene/MeshNodes.h"
#include "scene/PostProcess.h"
#include "scene/SceneIndex.h"
#include "scene/SkyAtmosphere.h"
#include "scene/Transform.h"

#include <cmath>
#include <numbers>

namespace lila::link
{
namespace
{
json vec3(const glm::dvec3& v) { return json::array({v.x, v.y, v.z}); }

json ueVec(const glm::dvec3& lila)
{
    const math::Vector u = math::toUnreal(math::Vector(lila));
    return json::array({u.x, u.y, u.z});
}

std::shared_ptr<RpcClient> connected(const char* command, const char* feature)
{
    try
    {
        Link::get().ensureConnected();
    }
    catch (const std::runtime_error& e)
    {
        throw CommandError(std::string("not connected to Unreal: ") + e.what());
    }
    const json features = Link::get().status().value("features", json::array());
    if (!features.is_array() || std::ranges::find(features, json(feature)) == features.end())
        throw CommandError(std::string(command) + ": the LilaLink plugin predates it; rebuild it from Lila's ue/LilaLink");
    std::shared_ptr<RpcClient> client = Link::get().client();
    if (!client)
        throw CommandError("not connected to Unreal");
    return client;
}

/** The sky atmosphere's attributes under Unreal's property names, colours as [r, g, b]. */
json atmosphereJson(const scene::Node& sky)
{
    namespace sa = scene::skyAtmosphere;
    const int a = sa::firstAttr();
    auto num = [&](sa::Attr attr) { return sa::scaled(sky, attr); };
    auto col = [&](int attr) { return vec3(sky.as<glm::dvec3>(a + attr)); };
    static const char* modes[] = {"PlanetTopAtAbsoluteWorldOrigin", "PlanetTopAtComponentTransform", "PlanetCenterAtComponentTransform"};
    return {{"transformMode", modes[std::clamp<int64_t>(sky.as<int64_t>(a + sa::kTransformMode), 0, 2)]},
            {"location", ueVec(sa::worldPosition(sky))},
            {"bottomRadius", num(sa::kBottomRadius)},
            {"groundAlbedo", col(sa::kGroundAlbedo)},
            {"atmosphereHeight", num(sa::kAtmosphereHeight)},
            {"multiScatteringFactor", num(sa::kMultiScatteringFactor)},
            {"rayleighScatteringScale", num(sa::kRayleighScatteringScale)},
            {"rayleighScattering", col(sa::kRayleighScattering)},
            {"rayleighExponentialDistribution", num(sa::kRayleighExponentialDistribution)},
            {"mieScatteringScale", num(sa::kMieScatteringScale)},
            {"mieScattering", col(sa::kMieScattering)},
            {"mieAbsorptionScale", num(sa::kMieAbsorptionScale)},
            {"mieAbsorption", col(sa::kMieAbsorption)},
            {"mieAnisotropy", num(sa::kMieAnisotropy)},
            {"mieExponentialDistribution", num(sa::kMieExponentialDistribution)},
            {"otherAbsorptionScale", num(sa::kOtherAbsorptionScale)},
            {"otherAbsorption", col(sa::kOtherAbsorption)},
            {"otherTentTipAltitude", num(sa::kOtherTentTipAltitude)},
            {"otherTentTipValue", num(sa::kOtherTentTipValue)},
            {"otherTentWidth", num(sa::kOtherTentWidth)},
            {"skyLuminanceFactor", col(sa::kSkyLuminanceFactor)},
            {"aerialPerspectiveViewDistanceScale", num(sa::kAerialPerspectiveViewDistanceScale)},
            {"aerialPerspectiveStartDepth", num(sa::kAerialPerspectiveStartDepth)},
            {"heightFogContribution", num(sa::kHeightFogContribution)},
            {"transmittanceMinLightElevationAngle", num(sa::kTransmittanceMinLightElevationAngle)}};
}

/**
 * The active post process's height fog in Unreal's terms. Unreal keeps density and falloff a thousand times their per-cm
 * value and falls off in powers of 2; Lila's are per metre and fall off by e.
 */
json fogJson(const scene::Graph& g)
{
    namespace pp = scene::postProcess;
    const scene::NodeId id = pp::active(g);
    if (id == scene::kNoNode)
        return nullptr;
    const scene::Node& n = *g.node(id);
    const int a = pp::firstAttr();
    if (!n.as<bool>(a + pp::kFog))
        return nullptr;
    const double density = n.as<double>(a + pp::kFogDensity), falloff = n.as<double>(a + pp::kFogFalloff);
    const double height = n.as<double>(a + pp::kFogHeight);
    return {{"fogDensity", density * 0.01 * 1000.0},
            {"fogHeightFalloff", falloff * 0.01 * 1000.0 / std::numbers::ln2},
            {"height", height},
            {"inscatteringColor", vec3(n.as<glm::dvec3>(a + pp::kFogColor))},
            {"startDistance", n.as<double>(a + pp::kFogStart)},
            {"directionalInscatteringExponent", 4.0 + 28.0 * std::clamp(n.as<double>(a + pp::kFogAnisotropy), 0.0, 0.95)},
            {"directionalInscatteringIntensity", n.as<double>(a + pp::kFogSunScatter)},
            {"volumetricFog", true}};
}

const scene::Node* volumeMaterialOf(scene::Graph& g, const scene::Node& n)
{
    const scene::MeshPtr mesh = n.as<scene::MeshPtr>(scene::MeshNode::kOutMesh);
    if (!mesh)
        return nullptr;
    for (const std::string& slot : mesh->materialNames())
        if (const scene::Node* m = scene::materials::find(g, slot);
            m && m->as<int64_t>(scene::material::kShadingModel) == int64_t(scene::material::ShadingModel::Volume))
            return m;
    return nullptr;
}
} // namespace

void registerSkySendCommands()
{
    using T = ArgType;
    ops::registerCommand({
        .name = "unrealSendSky",
        .doc = "Send the scene's sky to Unreal's open level: the sky atmosphere as a SkyAtmosphere (Unreal's own model, settings "
               "unchanged), a SkyLight capturing it in real time, the sun as its atmosphere sun light, and the active post "
               "process's height fog as an ExponentialHeightFog with volumetric fog. Each is made when the level has none. "
               "Returns what was set.",
        .flags = {{"sun", "s", T::Bool, true, "Place the sun light too, marked as the atmosphere's sun."},
                  {"fog", "f", T::Bool, true, "Send the height fog too."}},
        .undoable = false,
        .run = [](const Args& a) -> json {
            scene::Graph& g = scene::Graph::current();
            g.evaluate();
            const scene::NodeId sky = scene::skyAtmosphere::active(g);
            const json fog = a.get<bool>("fog", true) ? fogJson(g) : json(nullptr);
            if (sky == scene::kNoNode && fog.is_null())
                throw CommandError("unrealSendSky: the scene has no sky atmosphere and no height fog");
            std::shared_ptr<RpcClient> client = connected("unrealSendSky", "sky");
            json params = {{"atmosphere", sky != scene::kNoNode ? atmosphereJson(*g.node(sky)) : json(nullptr)}, {"heightFog", fog}};
            RpcResult r = client->call("setSky", params, placementTimeoutMs(0));
            if (!r.ok)
                throw CommandError("unrealSendSky: " + r.error);
            json out = r.result;
            if (const scene::NodeId sun = scene::skyAtmosphere::sunLight(g); a.get<bool>("sun", true) && sun != scene::kNoNode && sky != scene::kNoNode)
                out["sun"] = ops::execute("unrealPlaceInstances", {{"args", {g.node(sun)->path()}}});
            return out;
        },
    });

    ops::registerCommand({
        .name = "unrealSendVolumes",
        .doc = "Send meshes with a volume material to Unreal's open level: each grid written as OpenVDB, imported as a Sparse Volume "
               "Texture and shown by a Heterogeneous Volume with a material instance of M_LilaVolume (built on MF_LilaVolume, which "
               "your own Volume materials can use). With no objects, the selected volumes, else every volume in the scene. Needs the "
               "NanoVDB extension. Returns "
               "one entry per volume.",
        .positionalDoc = "objects",
        .flags = {{"folder", "fo", T::String, "/Game/Lila/Volumes", "Content folder for the textures and material instances."}},
        .undoable = false,
        .run = [](const Args& a) -> json {
            scene::Graph& g = scene::Graph::current();
            g.evaluate();
            if (!ops::find("exportVDB"))
                throw CommandError("unrealSendVolumes: needs the NanoVDB extension (Extensions window) to write OpenVDB files");
            std::vector<const scene::Node*> targets;
            for (const json& o : a.positional)
            {
                const std::string name = o.is_string() ? o.get<std::string>() : std::string();
                const scene::Node* n = g.find(name);
                if (!n || !n->type().isA("mesh") || !volumeMaterialOf(g, *n))
                    throw CommandError("unrealSendVolumes: " + name + " is no mesh with a volume material");
                targets.push_back(n);
            }
            // Named objects, else the selected volumes, else every volume in the scene.
            if (a.positional.empty())
                for (const std::string& name : a.objects())
                    if (const scene::Node* n = g.find(name); n && n->type().isA("mesh") && volumeMaterialOf(g, *n))
                        targets.push_back(n);
            if (targets.empty() && a.positional.empty())
                for (const scene::Node* n : scene::sceneIndex::ofType(g, "mesh"))
                    if (volumeMaterialOf(g, *n))
                        targets.push_back(n);
            if (targets.empty())
                throw CommandError("unrealSendVolumes: the scene has no meshes with a volume material");
            std::shared_ptr<RpcClient> client = connected("unrealSendVolumes", "volumes");
            const fs::path dir = cacheDir() / "volumes";
            json out = json::array();
            for (const scene::Node* n : targets)
            {
                const std::shared_ptr<const gn::Volume> v = gn::meshVolume(g, n->id());
                if (!v)
                {
                    out.push_back({{"object", n->path()}, {"error", "no grid"}});
                    continue;
                }
                const std::string file = ops::execute("exportVDB", {{"args", {n->path(), paths::toUtf8(dir / (n->name() + ".vdb"))}}}).get<std::string>();
                const scene::Node& m = *volumeMaterialOf(g, *n);
                using namespace scene::material;
                // The volume's voxel axes and centre voxel in Unreal space; Unreal makes a transform of them, mirrored as Lila to Unreal is.
                const glm::dmat4 toWorld = n->as<glm::dmat4>(scene::Transform::kWorldMatrix) * v->indexToWorld;
                const glm::dvec3 centre = glm::dvec3(toWorld * glm::dvec4(glm::dvec3(v->dims - 1) * 0.5, 1.0));
                const double density = m.as<double>(kVolumeDensity) * 0.01;
                json params = {{"file", file},
                               {"folder", a.get<std::string>("folder", "/Game/Lila/Volumes")},
                               {"name", n->name()},
                               {"label", n->path()},
                               {"centre", ueVec(centre)},
                               {"axes", json::array({ueVec(glm::dvec3(toWorld[0])), ueVec(glm::dvec3(toWorld[1])), ueVec(glm::dvec3(toWorld[2]))})},
                               {"resolution", json::array({v->dims.x, v->dims.y, v->dims.z})},
                               {"densityScale", density},
                               {"scatterColor", vec3(m.as<glm::dvec3>(kVolumeScatterColor))},
                               {"absorptionColor", vec3(m.as<glm::dvec3>(kVolumeAbsorptionColor))},
                               {"emissionColor", vec3(m.as<glm::dvec3>(kVolumeEmissionColor) * m.as<double>(kVolumeEmissionStrength))}};
                RpcResult r = client->call("placeVolume", params, placementTimeoutMs(1));
                out.push_back(r.ok ? json{{"object", n->path()}, {"result", r.result}} : json{{"object", n->path()}, {"error", r.error}});
                if (!r.ok)
                    log::warn("unrealSendVolumes: {}: {}", n->path(), r.error);
            }
            return out;
        },
    });
}
} // namespace lila::link
