#include "link/UnrealLink.h"

#include "ops/Command.h"
#include "ops/Undo.h"
#include "core/ExtensionContext.h"
#include "core/Log.h"
#include "core/MainQueue.h"
#include "core/PrefStore.h"
#include "core/Profile.h"
#include "fbx/Fbx.h"
#include "gn/Geometry.h"
#include "gn/Gn.h"
#include "gn/InstanceView.h"
#include "math/MathCore.h"
#include "link/Exporters.h"
#include "math/Matrix.h"
#include "math/Transform.h"
#include "math/Unreal.h"
#include "scene/ArrayGroup.h"
#include "scene/AttrComponents.h"
#include "scene/Graph.h"
#include "scene/Instancing.h"
#include "scene/Light.h"
#include "scene/LightingChannels.h"
#include "scene/Materials.h"
#include "scene/MeshNodes.h"
#include "scene/Transform.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <format>
#include <map>
#include <random>
#include <unordered_map>

namespace lila::link
{
namespace
{
std::string assetSafe(const std::string& name)
{
    std::string out;
    for (char c : name)
        out += std::isalnum(static_cast<unsigned char>(c)) || c == '_' ? c : '_';
    return out.empty() ? "Unnamed" : out;
}

/** UCX_ and the other collision proxies Unreal reads by name: part of their parent's asset, never placed themselves. */
bool collisionProxy(const std::string& name)
{
    return name.starts_with("UCX_") || name.starts_with("UBX_") || name.starts_with("USP_") || name.starts_with("UCP_");
}

bool placeable(const scene::Node* n) { return n && n->type().isA("mesh") && !n->type().isA("lattice") && !collisionProxy(n->name()); }

json vec(const math::Vector& v) { return json::array({v.x, v.y, v.z}); }

json placementAt(const glm::dmat4& world, json id, std::string name)
{
    const math::Transform t = math::toUnreal(math::Transform(math::Matrix(world)));
    return {{"id", std::move(id)},
            {"name", std::move(name)},
            {"location", vec(t.translation)},
            {"rotation", json::array({t.rotation.x, t.rotation.y, t.rotation.z, t.rotation.w})},
            {"scale", vec(t.scale3d)}};
}
std::string newLinkId()
{
    static std::mt19937_64 rng{std::random_device{}()};
    return std::format("{:016x}", rng());
}

/** Placement keys by node: each node's linkId, unless an older node holds the same one (a pasted or imported copy). */
class LinkKeys
{
public:
    LinkKeys(scene::Graph& g, bool assign) : mGraph(g), mAssign(assign)
    {
        for (scene::NodeId id : g.nodes())
            if (const std::string* s = linkIdOf(id); s && !s->empty())
                if (auto [it, fresh] = mOwner.try_emplace(*s, id); !fresh && id < it->second)
                    it->second = id;
    }

    std::string operator()(scene::NodeId id)
    {
        const std::string* s = linkIdOf(id);
        if (s && !s->empty() && mOwner.at(*s) == id)
            return *s;
        if (!mAssign || !s)
            return std::to_string(id);
        std::string fresh = newLinkId();
        mGraph.setValue({id, scene::Transform::kLinkId, -1}, fresh);
        mOwner[fresh] = id;
        return fresh;
    }

private:
    const std::string* linkIdOf(scene::NodeId id) const
    {
        const scene::Node* n = mGraph.node(id);
        if (!n || !n->type().dag || n->type().attrs.size() <= size_t(scene::Transform::kLinkId))
            return nullptr;
        return std::get_if<std::string>(&n->value(scene::Transform::kLinkId));
    }

    scene::Graph& mGraph;
    bool mAssign;
    std::unordered_map<std::string, scene::NodeId> mOwner;
};

constexpr const char* kAssetAttr = "unrealAsset";

/**
 * Asset names by instance source: the one a previous send saved on it, else SM_<name>, with the first eight characters of
 * its key appended when another source in the scene has the same name. A name a source saved stays its own (an older
 * node wins a copied one), so adding a second "Wall" later never renames the first one's asset.
 */
class AssetNames
{
public:
    AssetNames(scene::Graph& g, bool assign) : mGraph(g), mAssign(assign)
    {
        for (scene::NodeId id : g.nodes())
        {
            const scene::Node* n = g.node(id);
            if (!placeable(n) || scene::instancing::root(g, id) != id)
                continue;
            ++mBaseCount[base(*n)];
            if (std::string s = stored(*n); !s.empty())
                if (auto [it, fresh] = mOwner.try_emplace(s, id); !fresh && id < it->second)
                    it->second = id;
        }
    }

    std::string operator()(scene::NodeId src, const std::string& key)
    {
        const scene::Node& n = *mGraph.node(src);
        if (std::string s = stored(n); !s.empty() && mOwner.at(s) == src)
            return s;
        const std::string b = base(n);
        std::string name = b;
        if (mBaseCount[b] > 1 || mOwner.contains(b))
            name = b + "_" + assetSafe(key.substr(0, 8));
        while (mOwner.contains(name) && mOwner[name] != src)
            name += "_";
        mOwner[name] = src;
        if (mAssign)
        {
            int i = n.type().attrIndex(kAssetAttr);
            if (i < 0)
                i = mGraph.addAttr(src, scene::AttrDef{.name = kAssetAttr, .type = scene::PlugType::String, .defaultValue = std::string(),
                                                       .flags = scene::AttrHidden, .doc = "The Unreal asset unrealPlaceInstances sends this mesh as."});
            mGraph.setValue({src, i, -1}, name);
        }
        return name;
    }

private:
    static std::string base(const scene::Node& n) { return "SM_" + assetSafe(n.name()); }

    static std::string stored(const scene::Node& n)
    {
        const int i = n.type().attrIndex(kAssetAttr);
        if (i < 0)
            return {};
        const std::string* s = std::get_if<std::string>(&n.value(i));
        return s ? *s : std::string();
    }

    scene::Graph& mGraph;
    bool mAssign;
    std::map<std::string, int> mBaseCount;
    std::map<std::string, scene::NodeId> mOwner;
};

constexpr const char* kCustomDataAttr = "unrealCustomData";
constexpr const char* kRandomData = "random";

/** 0 to 1, the same for a key every send: per-instance variation (colour, wear) a material reads as custom primitive data 0. */
double stableRandom(const std::string& key)
{
    uint64_t h = 1469598103934665603ull;
    for (char c : key)
        h = (h ^ uint8_t(c)) * 1099511628211ull;
    return double(h >> 11) / double(1ull << 53);
}

/** What the registered extras add to a member's placements (Stagehand's physics), merged into one object. */
json extrasOf(scene::Graph& g, scene::NodeId id)
{
    json out = json::object();
    for (const std::string& extraId : placementExtras().idsInOrder())
        if (const PlacementExtra* extra = placementExtras().find(extraId); extra && *extra)
            if (const json more = (*extra)(g, id); more.is_object())
                for (const auto& [k, v] : more.items())
                    out[k] = v;
    return out;
}

bool hasUcxChildren(scene::Graph& g, scene::NodeId src)
{
    for (scene::NodeId c : g.children(src))
        if (const scene::Node* n = g.node(c); n && n->type().isA("mesh") && collisionProxy(n->name()))
            return true;
    return false;
}

/**
 * auto: ucx when the source carries hulls (UCX_ children, or a collision source's such as a Stagehand body, written
 * into its FBX), complexAsSimple when its body asks for that or it has none. Any other choice stands.
 */
std::string resolvedCollision(scene::Graph& g, scene::NodeId src, const std::string& choice, bool ucxChildren, const json& extras)
{
    if (choice != "auto")
        return choice;
    if (extras.is_object() && extras.contains("physics") && extras["physics"].value("collision", "") == "complexAsSimple")
        return "complexAsSimple";
    if (ucxChildren)
        return "ucx";
    for (const std::string& id : fbx::collisionSources().idsInOrder())
        if (const fbx::CollisionSource* s = fbx::collisionSources().find(id); s && *s && !(*s)(g, src).empty())
            return "ucx";
    return "complexAsSimple";
}

/** Simulated members need actors: instances of an (H)ISM can't each simulate. */
bool simulates(const json& placements)
{
    for (const json& p : placements)
        if (p.contains("physics") && p["physics"].is_object() && p["physics"].value("simulate", false))
            return true;
    return false;
}

/** One custom primitive data entry of a group: a name, the first slot it fills and how many floats. */
struct DataSlot
{
    std::string name;
    int slot = 0;
    int width = 1;
};

/** Floats a node attr gives: 3 for a vector, 1 for a number, bool or enum, 0 for a missing or non-numeric attr. */
int attrWidth(const scene::Node& n, const std::string& name)
{
    const int i = n.type().attrIndex(name);
    if (i < 0)
        return 0;
    switch (n.type().attrs[size_t(i)].type)
    {
    case scene::PlugType::Vec3:
        return 3;
    case scene::PlugType::Float:
    case scene::PlugType::Int:
    case scene::PlugType::Bool:
    case scene::PlugType::Enum:
        return 1;
    default:
        return 0;
    }
}

/** The group's custom data slots: the declared names, each as wide as its attribute, in order from slot 0. */
std::vector<DataSlot> dataSlots(const scene::Node& source, const gn::Instances* inst)
{
    std::vector<DataSlot> out;
    int slot = 0;
    for (const std::string& name : customDataNames(source))
    {
        int width = name == kRandomData ? 1 : 0;
        if (const geo::Attribute* a = inst && width == 0 ? inst->attrs.find(name) : nullptr)
            width = geo::attrTypeComponents(a->type());
        if (width == 0)
            width = std::max(1, attrWidth(source, name));
        out.push_back({name, slot, width});
        slot += width;
    }
    return out;
}

/** A node attr's floats, padded or cut to width; zeros when the node has no such attr. */
void nodeFloats(scene::Graph& g, scene::NodeId id, const std::string& name, int width, json& out)
{
    const scene::Node& n = *g.node(id);
    std::vector<double> v;
    if (const int i = n.type().attrIndex(name); i >= 0)
        std::visit(
            [&v](const auto& x) {
                using T = std::decay_t<decltype(x)>;
                if constexpr (std::is_same_v<T, glm::dvec3>)
                    v = {x.x, x.y, x.z};
                else if constexpr (std::is_same_v<T, double> || std::is_same_v<T, int64_t> || std::is_same_v<T, bool>)
                    v = {double(x)};
            },
            g.value({id, i, -1}));
    v.resize(size_t(width), 0.0);
    for (double d : v)
        out.push_back(d);
}

/** The top-level instance's floats of a gn instance attribute; false when the instances have no such attribute. */
bool instanceFloats(const gn::Instances& inst, const std::string& name, size_t top, int width, json& out)
{
    const geo::Attribute* a = inst.attrs.find(name);
    if (!a || top >= a->size())
        return false;
    std::vector<double> v;
    switch (a->type())
    {
    case geo::AttrType::Float: v = {a->values<float>()[top]}; break;
    case geo::AttrType::Float2: v = {a->values<glm::vec2>()[top].x, a->values<glm::vec2>()[top].y}; break;
    case geo::AttrType::Float3:
    {
        const glm::vec3 c = a->values<glm::vec3>()[top];
        v = {c.x, c.y, c.z};
        break;
    }
    case geo::AttrType::Float4:
    {
        const glm::vec4 c = a->values<glm::vec4>()[top];
        v = {c.x, c.y, c.z, c.w};
        break;
    }
    case geo::AttrType::Int: v = {double(a->values<int32_t>()[top])}; break;
    case geo::AttrType::Bool: v = {a->values<bool>()[top] ? 1.0 : 0.0}; break;
    }
    v.resize(size_t(width), 0.0);
    for (double d : v)
        out.push_back(d);
    return true;
}

/**
 * A placement's custom data: per slot the stable random of its key, the gn instance's attribute (inst, top) or else the
 * member's attr of that name.
 */
json customData(scene::Graph& g, const std::vector<DataSlot>& slots, scene::NodeId member, const std::string& key, const gn::Instances* inst = nullptr,
                size_t top = 0)
{
    json out = json::array();
    for (const DataSlot& d : slots)
    {
        if (d.name == kRandomData)
            out.push_back(stableRandom(key));
        else if (!inst || !instanceFloats(*inst, d.name, top, d.width, out))
            nodeFloats(g, member, d.name, d.width, out);
    }
    return out;
}

json slotsJson(const std::vector<DataSlot>& slots)
{
    json out = json::array();
    for (const DataSlot& d : slots)
        out.push_back({{"name", d.name}, {"slot", d.slot}, {"width", d.width}});
    return out;
}

/**
 * A light actor's settings: position and the directions it shines down (-Z) and holds up (+Y) in Unreal space, so
 * the actor's X axis points where it shines and a rect light's height runs along its Z.
 */
json lightPlacement(scene::Graph& g, scene::NodeId id, const std::string& key)
{
    namespace lights = scene::lights;
    const glm::dmat4 world = std::get<glm::dmat4>(g.value({id, scene::Transform::kWorldMatrix, -1}));
    auto axis = [&](const glm::dvec4& local) { return vec(math::toUnreal(math::Vector(glm::normalize(glm::dvec3(world * local))))); };
    const lights::Settings s = lights::worldSettings(*g.node(id));
    return {{"id", key},
            {"name", g.node(id)->path()},
            {"kind", lights::kindName(s.kind)},
            {"location", vec(math::toUnreal(math::Vector(glm::dvec3(world[3]))))},
            {"forward", axis({0, 0, -1, 0})},
            {"up", axis({0, 1, 0, 0})},
            {"color", vec(s.color)},
            {"intensity", s.intensity},
            {"units", std::array{"unitless", "candela", "lumens", "lux", "nits", "watts"}.at(size_t(s.units))},
            {"useTemperature", s.useTemperature},
            {"temperature", s.temperature},
            {"range", s.range},
            {"innerConeAngle", s.innerConeAngle},
            {"outerConeAngle", s.outerConeAngle},
            {"width", s.width},
            {"height", s.height},
            {"sourceRadius", s.sourceRadius},
            {"castShadows", s.castShadows}};
}

json channelBits(const std::array<bool, 3>& b) { return json::array({b[0], b[1], b[2]}); }

/** Says in the log which lighting channels a send carries, so one nobody meant to send is easy to spot. */
void logChannels(const json& report)
{
    if (report["lights"].empty() && report["objects"].empty())
        return;
    std::string list;
    for (const json& c : report["channels"])
        list += (list.empty() ? "" : ", ") + c.get<std::string>();
    log::info("unrealPlaceInstances: lighting channels {} on {} light(s) and {} object(s) {}", list.empty() ? "default" : list,
              report["lights"].size(), report["objects"].size(), report["written"].get<bool>() ? "sent" : "left out (lightingChannels=False)");
    if (report.contains("note"))
        log::warn("unrealPlaceInstances: {}", report["note"].get<std::string>());
}

bool hasFeature(const char* feature)
{
    const json features = Link::get().status().value("features", json::array());
    return features.is_array() && std::ranges::find(features, json(feature)) != features.end();
}

/** One instance of a geometry nodes result, nested instances flattened: object-space transform and a stable id. */
struct GnInstance
{
    glm::dmat4 xf{1.0};
    /** The top-level instance it comes from, for its attributes. */
    size_t top = 0;
    std::string id;
};

/** Every instance of one referenced mesh. */
struct GnPart
{
    scene::MeshPtr mesh;
    std::string hash;
    std::vector<GnInstance> instances;
};

struct GnResult
{
    gn::GeometryPtr geometry;
    /** The geometry beside the instances, realized; null when there is none. */
    scene::MeshPtr base;
    std::vector<GnPart> parts;
};

/** 16 hex digits of the mesh's points and topology: the same mesh hashes the same in every session. */
std::string meshHash(const geo::Mesh& m)
{
    uint64_t h = 1469598103934665603ull;
    auto mix = [&h](const void* data, size_t size) {
        for (const auto* p = static_cast<const uint8_t*>(data); size--; ++p)
            h = (h ^ *p) * 1099511628211ull;
    };
    const std::array<geo::Index, 3> counts{m.numPoints(), m.numFaces(), m.numCorners()};
    mix(counts.data(), sizeof(counts));
    mix(m.positions().data(), m.positions().size_bytes());
    for (geo::Index f = 0; f < m.numFaces(); ++f)
    {
        const std::vector<geo::Index> corners = m.faceCorners(f);
        mix(corners.data(), corners.size() * sizeof(geo::Index));
    }
    return std::format("{:016x}", h);
}

scene::MeshPtr cleanedMesh(const std::shared_ptr<const geo::Mesh>& m)
{
    geo::Mesh copy = *m;
    gn::stripAnonymous(copy);
    copy.compact();
    return std::make_shared<const geo::Mesh>(std::move(copy));
}

void collectGn(const gn::Instances& inst, const glm::dmat4& xf, size_t top, const std::string& prefix, std::map<const geo::Mesh*, size_t>& index,
               std::vector<GnPart>& parts, int depth)
{
    // Instances nest but never cycle; the cap only guards a malformed geometry.
    if (depth > 32)
        return;
    const std::span<const int32_t> ids = depth == 0 ? inst.attrs.get<int32_t>("id") : std::span<const int32_t>{};
    for (size_t k = 0; k < inst.size(); ++k)
    {
        const int r = inst.reference[k];
        if (r < 0 || size_t(r) >= inst.references.size() || !inst.references[size_t(r)])
            continue;
        const gn::Geometry& ref = *inst.references[size_t(r)];
        const glm::dmat4 m = xf * inst.transforms[k];
        const size_t t = depth == 0 ? k : top;
        const std::string id = prefix + std::to_string(k < ids.size() ? int64_t(ids[k]) : int64_t(k));
        if (ref.mesh && ref.mesh->numFaces() > 0)
        {
            auto [it, added] = index.try_emplace(ref.mesh.get(), parts.size());
            if (added)
            {
                scene::MeshPtr clean = cleanedMesh(ref.mesh);
                parts.push_back({clean, meshHash(*clean), {}});
            }
            parts[it->second].instances.push_back({m, t, id});
        }
        if (ref.instances)
            collectGn(*ref.instances, m, t, id + ".", index, parts, depth + 1);
    }
}

/**
 * The geometry nodes result the mesh shows, its instances grouped by the mesh they reference (identical meshes from
 * different references merged by hash), when a gn modifier's instances are what the node shows. Nullopt otherwise.
 */
std::optional<GnResult> gnResultOf(scene::Graph& g, scene::NodeId mesh)
{
    gn::InstanceViewPtr view = gn::instanceViewFor(g, mesh);
    if (!view)
        return std::nullopt;
    auto src = g.source({mesh, scene::MeshNode::kInMesh, -1});
    gn::GeometryPtr geometry = src ? gn::modifierGeometry(src->node) : nullptr;
    if (!geometry || !geometry->instances)
        return std::nullopt;
    GnResult r{geometry, view->base, {}};
    std::map<const geo::Mesh*, size_t> index;
    std::vector<GnPart> parts;
    collectGn(*geometry->instances, glm::dmat4(1.0), 0, "", index, parts, 0);
    for (GnPart& p : parts)
    {
        auto same = std::ranges::find(r.parts, p.hash, &GnPart::hash);
        if (same == r.parts.end())
            r.parts.push_back(std::move(p));
        else
            same->instances.insert(same->instances.end(), p.instances.begin(), p.instances.end());
    }
    if (r.parts.empty())
        return std::nullopt;
    return r;
}

/** materialParams for one mesh: its slots but the default one. */
json meshMaterials(scene::Graph& g, const geo::Mesh& mesh)
{
    json out = json::array();
    for (const auto& [slot, count] : scene::materials::slotUsage(mesh))
        if (count > 0 && slot != scene::materials::kDefaultSlot)
            out.push_back(materialInputs(g, slot));
    return out;
}

/** The components every placement carries with the same values: what an instanced group's holder actor takes. */
json sharedComponents(const json& placements)
{
    json out = json::array();
    if (placements.empty())
        return out;
    for (const json& c : placements[0].value("components", json::array()))
        if (std::ranges::all_of(placements, [&c](const json& p) {
                const json& list = p.value("components", json::array());
                return std::ranges::find(list, c) != list.end();
            }))
            out.push_back(c);
    return out;
}

/** Where a member is placed: itself, then each of its array copies keyed "<node key>:<copy>". */
struct Spot
{
    glm::dmat4 world;
    std::string key, name;
};

std::vector<Spot> spotsOf(scene::Graph& g, scene::NodeId id, const std::string& key)
{
    const glm::dmat4 world = std::get<glm::dmat4>(g.value({id, scene::Transform::kWorldMatrix, -1}));
    std::vector<Spot> out{{world, key, g.node(id)->path()}};
    const std::vector<scene::arrays::Copy> copies = scene::arrays::copiesOf(g, id);
    for (size_t k = 0; k < copies.size(); ++k)
        out.push_back({copies[k].transform * world, std::format("{}:{}", key, k + 1), std::format("{}_{}", g.node(id)->path(), k + 1)});
    return out;
}

/** placementPlan, with the meshes geometry nodes groups and bases export kept in meshes (a group's gnMesh indexes it). */
json buildPlan(scene::Graph& g, const std::vector<scene::NodeId>& roots, const PlacementOptions& o, std::vector<scene::MeshPtr>& meshes)
{
    LILA_ZONE();

    if (o.mode != "auto" && o.mode != "actors" && o.mode != "ism" && o.mode != "hism")
        throw CommandError("unrealPlaceInstances: mode is auto, actors, ism or hism, not '" + o.mode + "'");
    if (o.collision != "auto" && o.collision != "none" && o.collision != "box" && o.collision != "convex" && o.collision != "complexAsSimple" && o.collision != "ucx")
        throw CommandError("unrealPlaceInstances: collision is auto, none, box, convex, complexAsSimple or ucx, not '" + o.collision + "'");
    g.evaluate();
    // Every mesh under the roots counts with its whole instance group: Unreal drops a group's placements Lila no longer sends.
    std::vector<scene::NodeId> sources, lightNodes;
    auto touch = [&](scene::NodeId id) {
        if (const scene::Node* n = g.node(id); n && n->type().isA("light"))
        {
            if (std::ranges::find(lightNodes, id) == lightNodes.end() && scene::effectivelyVisible(g, id))
                lightNodes.push_back(id);
            return;
        }
        if (!placeable(g.node(id)))
            return;
        const scene::NodeId src = scene::instancing::root(g, id);
        if (std::ranges::find(sources, src) == sources.end())
            sources.push_back(src);
    };
    for (scene::NodeId r : roots)
    {
        touch(r);
        for (scene::NodeId d : g.descendants(r))
            touch(d);
    }
    std::string folder = o.folder.empty() ? "/Game/Lila/Instances" : o.folder;
    while (folder.size() > 1 && folder.back() == '/')
        folder.pop_back();
    LinkKeys keys(g, o.assignIds);
    AssetNames assetNames(g, o.assignIds);
    std::vector<std::vector<scene::NodeId>> placedOf;
    std::vector<scene::NodeId> carried = lightNodes;
    for (scene::NodeId src : sources)
    {
        std::vector<scene::NodeId>& placed = placedOf.emplace_back(std::vector<scene::NodeId>{src});
        for (scene::NodeId m : scene::instancing::members(g, src))
            if (m != src && scene::instancing::root(g, m) == src)
                placed.push_back(m);
        carried.insert(carried.end(), placed.begin(), placed.end());
    }
    namespace lc = scene::lightingChannels;
    const lc::Usage use = lc::usage(g, carried);
    const lc::UnrealChannels unreal = lc::unrealChannels(use);
    std::string note;
    if (o.lightingChannels && !unreal.dropped.empty())
    {
        for (const std::string& d : unreal.dropped)
            note += (note.empty() ? "" : ", ") + d;
        note = "Unreal has three lighting channels: " + note + " dropped; lights and objects left in none of the three go on channel 0";
    }
    json groups = json::array();
    json gnSources = json::array();
    auto group = [&](const std::string& key, const scene::Node& n, const std::string& asset, json list) {
        const std::string mode = o.mode != "auto" ? o.mode : int(list.size()) > o.threshold && !simulates(list) ? "hism" : "actors";
        json shared = mode != "actors" ? sharedComponents(list) : json();
        json out{{"key", key},
                 {"source", n.fullPath()},
                 {"sourceId", n.id()},
                 {"contentPath", folder + "/" + asset},
                 {"objectPath", folder + "/" + asset + "." + asset},
                 {"mode", mode},
                 {"collision", o.collision},
                 {"ucxChildren", false},
                 {"nanite", o.nanite},
                 {"placements", std::move(list)}};
        // Instances can't hold components: the holder actor takes the ones they all share.
        if (!shared.is_null())
            out["components"] = std::move(shared);
        return out;
    };
    for (size_t k = 0; k < sources.size(); ++k)
    {
        const scene::NodeId src = sources[k];
        const scene::Node* n = g.node(src);
        const std::string groupKey = keys(src);
        const std::string asset = assetNames(src, groupKey);
        // Hidden members stay out of the level: a library asset's master only supplies the mesh.
        std::vector<scene::NodeId> placed;
        for (scene::NodeId id : placedOf[k])
            if (scene::effectivelyVisible(g, id))
                placed.push_back(id);
        std::vector<std::vector<Spot>> spots;
        std::vector<json> components;
        std::vector<json> bits;
        std::vector<json> extras;
        for (scene::NodeId id : placed)
        {
            spots.push_back(spotsOf(g, id, keys(id)));
            components.push_back(scene::attrComponents::unrealPayload(g, id));
            bits.push_back(o.lightingChannels ? channelBits(lc::unrealBits(*g.node(id), unreal)) : json());
            extras.push_back(extrasOf(g, id));
        }
        const std::vector<DataSlot> slots = dataSlots(*n, nullptr);
        auto finish = [&](json& p, size_t member) {
            p["components"] = components[member];
            if (o.lightingChannels)
                p["lightingChannels"] = bits[member];
        };

        const std::optional<GnResult> gnr = gnResultOf(g, src);
        json list = json::array();
        if (!gnr || gnr->base)
            for (size_t m = 0; m < placed.size(); ++m)
                for (const Spot& s : spots[m])
                {
                    json p = placementAt(s.world, s.key, s.name);
                    p["customData"] = customData(g, slots, placed[m], s.key);
                    // Array copies are the member again, so they carry what extras gave it.
                    for (const auto& [field, value] : extras[m].items())
                        p[field] = value;
                    finish(p, m);
                    list.push_back(std::move(p));
                }
        json own = group(groupKey, *n, asset, std::move(list));
        own["ucxChildren"] = hasUcxChildren(g, src);
        own["collision"] = resolvedCollision(g, src, o.collision, own["ucxChildren"].get<bool>(), extras.empty() ? json() : extras.front());
        own["customData"] = slotsJson(slots);
        if (gnr && gnr->base)
        {
            own["gnMesh"] = meshes.size();
            meshes.push_back(gnr->base);
        }
        else if (gnr)
            own["remove"] = true;
        groups.push_back(std::move(own));
        if (!gnr)
            continue;

        // Each mesh the geometry nodes instance is an asset of its own, its instances keyed by node, copy and instance id.
        json keep = json::array();
        const gn::Instances& top = *gnr->geometry->instances;
        const std::vector<DataSlot> gnSlots = dataSlots(*n, &top);
        for (const GnPart& part : gnr->parts)
        {
            const std::string partKey = groupKey + ":gn:" + part.hash.substr(0, 8);
            json gl = json::array();
            for (size_t m = 0; m < placed.size(); ++m)
                for (const Spot& s : spots[m])
                    for (const GnInstance& inst : part.instances)
                    {
                        const std::string id = s.key + ":" + inst.id;
                        json p = placementAt(s.world * inst.xf, id, s.name + "_" + inst.id);
                        p["customData"] = customData(g, gnSlots, placed[m], id, &top, inst.top);
                        finish(p, m);
                        gl.push_back(std::move(p));
                    }
            json pg = group(partKey, *n, asset + "_" + part.hash.substr(0, 8), std::move(gl));
            pg["label"] = n->fullPath() + "_" + part.hash.substr(0, 8);
            pg["collision"] = o.collision == "auto" ? "complexAsSimple" : o.collision;
            pg["customData"] = slotsJson(gnSlots);
            pg["gnMesh"] = meshes.size();
            meshes.push_back(part.mesh);
            groups.push_back(std::move(pg));
            keep.push_back(partKey);
        }
        gnSources.push_back({{"prefix", groupKey + ":gn:"}, {"keep", std::move(keep)}});
    }
    json lightList = json::array();
    json skipped = json::array();
    for (scene::NodeId id : lightNodes)
    {
        if (scene::lights::settings(*g.node(id)).kind == scene::lights::Kind::Dome)
        {
            skipped.push_back(g.node(id)->path() + ": dome lights have no Unreal light actor here (use a Sky Light)");
            continue;
        }
        json l = lightPlacement(g, id, keys(id));
        if (o.lightingChannels)
            l["lightingChannels"] = channelBits(lc::unrealBits(*g.node(id), unreal));
        lightList.push_back(std::move(l));
    }
    return {{"groups", std::move(groups)},
            {"gnSources", std::move(gnSources)},
            {"lights", std::move(lightList)},
            {"skipped", std::move(skipped)},
            {"lightingChannels", lc::report(g, use, o.lightingChannels && !use.empty(), note, &unreal)}};
}

} // namespace

Registry<PlacementExtra>& placementExtras()
{
    static Registry<PlacementExtra> r;
    return r;
}

bool addPlacementExtra(ext::Context& ctx, const std::string& id, PlacementExtra extra)
{
    if (placementExtras().contains(id))
    {
        log::error("extension {}: placement extra '{}' is already registered", ctx.owner(), id);
        return false;
    }
    ctx.add(placementExtras(), "placementExtra", id, std::move(extra));
    return true;
}

json placementPlan(scene::Graph& g, const std::vector<scene::NodeId>& roots, const PlacementOptions& o)
{
    std::vector<scene::MeshPtr> meshes;
    return buildPlan(g, roots, o, meshes);
}

json placementPlan(scene::Graph& g, const std::vector<scene::NodeId>& roots, const PlacementOptions& o, std::vector<scene::MeshPtr>& meshes)
{
    return buildPlan(g, roots, o, meshes);
}

bool writeMeshFbx(const scene::MeshPtr& mesh, const std::string& name, const fs::path& path, std::string* err)
{
    fbx::ExportScene scene;
    scene.upAxis = math::upAxis();
    fbx::ExportNode node;
    node.name = name;
    node.mesh = mesh;
    scene.nodes.push_back(std::move(node));
    return fbx::writeFbx(scene, path, fbx::ExportOptions{}, err);
}

std::vector<std::string> customDataNames(const scene::Node& source)
{
    const int i = source.type().attrIndex(kCustomDataAttr);
    if (i < 0)
        return {kRandomData};
    const std::string* s = std::get_if<std::string>(&source.value(i));
    const json j = s ? json::parse(*s, nullptr, false) : json();
    std::vector<std::string> out;
    if (j.is_array())
        for (const json& n : j)
            if (n.is_string() && !n.get<std::string>().empty())
                out.push_back(n.get<std::string>());
    return out;
}

void setCustomDataNames(scene::Graph& g, scene::NodeId source, const std::vector<std::string>& names)
{
    const scene::Node* n = g.node(source);
    if (!n)
        throw std::invalid_argument("no such node");
    int i = n->type().attrIndex(kCustomDataAttr);
    if (i < 0)
        i = g.addAttr(source, scene::AttrDef{.name = kCustomDataAttr, .type = scene::PlugType::String, .defaultValue = std::string(),
                                             .flags = scene::AttrHidden, .doc = "What unrealPlaceInstances sends as custom primitive data, as JSON."});
    g.setValue({source, i, -1}, json(names).dump());
}

int placementTimeoutMs(size_t count)
{
    return int(std::min<size_t>(30'000 + count * 20, 30 * 60'000));
}

void registerPlacementCommands()
{
    using T = ArgType;

    ops::registerCommand({
        .name = "unrealPlaceInstances",
        .doc = "Send meshes to Unreal as one static mesh asset per instance source, placed in the open level: an actor per "
               "member referencing that asset, or one instanced static mesh component (ISM or HISM) for a large group. "
               "Lights among them go as light actors (point, spot, directional, rect; discs as square rect lights, domes skipped). "
               "Lighting channels map onto Unreal's three: default is channel 0, the next two channels the sent nodes use (in "
               "scene order) 1 and 2; any more are dropped, with a warning naming them, and what is left in none goes on 0. "
               "Placements are keyed by each node's linkId, given on the first send and saved with the scene, so sending "
               "a group again moves, adds and removes its actors to match, also after a rename or reopening the scene. "
               "Validators run first: a blocking issue stops the send, others come back in issues. "
               "Returns the plan, with each group's import and placement results when wait=True.",
        .positionalDoc = "objects",
        .flags = {{"folder", "f", T::String, "/Game/Lila/Instances", "Content folder the assets go in; each is SM_<source name>, with part of the source's link id appended when another mesh in the scene has the same name. The name is saved on the source, so it never changes."},
                  {"mode", "m", T::String, "auto",
                   "How members are placed: actors, ism, hism, or auto (actors up to threshold members, then hism).",
                   {"auto", "actors", "ism", "hism"}},
                  {"threshold", "t", T::Int, nullptr, "auto switches to hism above this many members (default the unreal pref /instanceThreshold, 50).", {}, {}, 0.0},
                  {"lightingChannels", "lc", T::Bool, true,
                   "Set the actors' and lights' Lighting Channels from Lila's channels. The plan's lightingChannels lists the "
                   "channels, lights and objects, the Unreal slot of each channel and any dropped."},
                  {"collision", "col", T::String, "auto",
                   "Collision the assets get: auto (ucx for a source with UCX_ children or a Stagehand body's hulls, else "
                   "complexAsSimple), none (the placed actors and instances don't collide either), box, convex (Unreal's auto "
                   "convex decomposition), complexAsSimple (the render mesh) or ucx (the hulls only; a source's UCX_ child "
                   "meshes go with it, and its other children into the asset too).",
                   {"auto", "none", "box", "convex", "complexAsSimple", "ucx"}},
                  {"nanite", "na", T::Bool, false, "Build Nanite data for the assets."},
                  {"hlodLayer", "hl", T::String, "", "HLOD layer asset path for the placed actors."},
                  {"validate", "va", T::Bool, true,
                   "Run the validators first: an issue blocking unrealPlaceInstances, \"unreal\" or \"*\" on what is sent stops "
                   "the send; other issues come back in the result's issues."},
                  {"dryRun", "", T::Bool, false, "Return the plan without exporting or contacting Unreal."},
                  {"wait", "w", T::Bool, false, "Block until Unreal has imported and placed everything."}},
        .undoable = false,
        .run = [](const Args& a) -> json {
            scene::Graph& g = scene::Graph::current();
            std::vector<scene::NodeId> roots;
            for (const std::string& name : a.objects())
            {
                scene::Node* n = g.find(name);
                if (!n)
                    throw CommandError("unrealPlaceInstances: no node named '" + name + "'");
                roots.push_back(n->id());
            }
            PlacementOptions o;
            o.folder = a.get<std::string>("folder", o.folder);
            o.mode = a.get<std::string>("mode", o.mode);
            o.threshold = a.has("threshold") ? a.get<int>("threshold") : PrefStore::get().value<int>("unreal", "/instanceThreshold", 50);
            o.assignIds = !a.get<bool>("dryRun");
            o.lightingChannels = a.get<bool>("lightingChannels", true);
            o.collision = a.get<std::string>("collision", o.collision);
            o.nanite = a.get<bool>("nanite", false);
            json plan;
            std::vector<scene::MeshPtr> meshes;
            {
                UndoChunk chunk("Unreal Link Ids");
                plan = buildPlan(g, roots, o, meshes);
            }
            if (plan["groups"].empty() && plan["lights"].empty())
                throw CommandError("unrealPlaceInstances: select meshes or lights to send");
            std::vector<scene::NodeId> sent = roots;
            for (const json& grp : plan["groups"])
                for (scene::NodeId m : scene::instancing::members(g, scene::NodeId(grp["sourceId"].get<uint64_t>())))
                    sent.push_back(m);
            plan["issues"] = preflight(g, sent, "unrealPlaceInstances", a.get<bool>("validate", true));
            for (const json& i : plan["issues"])
                log::warn("unrealPlaceInstances: {}: {}", i["target"].get<std::string>(), i["message"].get<std::string>());
            logChannels(plan["lightingChannels"]);
            if (a.get<bool>("dryRun"))
                return plan;

            // A source with UCX_ children goes with its children, so they reach the import.
            ExportFn withChildren = findExporter("fbx");
            ExportFn alone = findExporter("fbx:instanceSource");
            if (!alone)
                alone = withChildren;
            if (!alone)
                throw CommandError("unrealPlaceInstances: no FBX exporter is registered");
            for (json& grp : plan["groups"])
                grp["materials"] = grp.contains("gnMesh") ? meshMaterials(g, *meshes[grp["gnMesh"].get<size_t>()])
                                                          : materialParams(g, {scene::NodeId(grp["sourceId"].get<uint64_t>())});
            try
            {
                Link::get().ensureConnected();
            }
            catch (const std::runtime_error& e)
            {
                throw CommandError(std::string("not connected to Unreal: ") + e.what());
            }
            if (!plan["lights"].empty() && !hasFeature("lights"))
            {
                log::warn("unrealPlaceInstances: the LilaLink plugin predates lights; rebuild it from Lila's ue/LilaLink to send them");
                plan["lights"] = json::array();
            }

            const std::string hlod = a.get<std::string>("hlodLayer", "");
            auto job = [plan, meshes, alone, withChildren, hlod, dir = cacheDir()]() mutable {
                LILA_ZONE_N("Unreal placements");
                std::shared_ptr<RpcClient> client = Link::get().client();
                if (!client)
                    throw std::runtime_error("not connected");
                std::error_code ec;
                fs::create_directories(dir, ec);
                for (json& grp : plan["groups"])
                {
                    const std::string source = grp["source"].get<std::string>();
                    if (grp.value("remove", false))
                    {
                        // Its geometry nodes instance everything, so the source itself has nothing left to place.
                        RpcResult removed = client->call("removePlacements", {{"groups", {grp["key"]}}}, placementTimeoutMs(0));
                        grp["placed"] = removed.ok ? removed.result : json{{"error", removed.error}};
                        continue;
                    }
                    const std::string asset = grp["contentPath"].get<std::string>();
                    const fs::path file = dir / (assetSafe(asset.substr(asset.rfind('/') + 1)) + "_instance.fbx");
                    std::string err;
                    const bool written = grp.contains("gnMesh")
                                             ? writeMeshFbx(meshes[grp["gnMesh"].get<size_t>()], asset.substr(asset.rfind('/') + 1), file, &err)
                                             : (grp.value("ucxChildren", false) && withChildren ? withChildren : alone)(
                                                   {scene::NodeId(grp["sourceId"].get<uint64_t>())}, file, &err);
                    if (!written)
                    {
                        grp["error"] = "export failed: " + err;
                        continue;
                    }
                    const json mapping = {{"contentPath", grp["contentPath"]}, {"kind", "static"},       {"collision", grp["collision"]},
                                          {"nanite", grp["nanite"]},           {"reapplyOptions", true}};
                    RpcResult imported = client->call("importAsset", importParams(file, mapping, grp["materials"]), importCallOptions());
                    grp["import"] = imported.ok ? imported.result : json{{"error", imported.error}};
                    if (!imported.ok)
                        continue;
                    std::string mesh = grp["objectPath"].get<std::string>();
                    if (json assets = imported.result.value("assets", json::array()); assets.is_array() && !assets.empty() && assets[0].is_string())
                        mesh = assets[0].get<std::string>();
                    json params = {{"group", grp["key"]},
                                   {"label", grp.value("label", source)},
                                   {"mesh", mesh},
                                   {"mode", grp["mode"]},
                                   {"collision", grp["collision"]},
                                   {"customData", grp.value("customData", json::array())},
                                   {"components", grp.value("components", json::array())},
                                   {"placements", grp["placements"]}};
                    if (!hlod.empty())
                        params["hlodLayer"] = hlod;
                    RpcResult placed = client->call("placeInstances", params, placementTimeoutMs(grp["placements"].size()));
                    grp["placed"] = placed.ok ? placed.result : json{{"error", placed.error}};
                }
                // Meshes a source's geometry nodes no longer instance leave the level.
                for (json& src : plan["gnSources"])
                {
                    RpcResult r = client->call("removePlacements", {{"prefixes", {src["prefix"]}}, {"keep", src["keep"]}}, placementTimeoutMs(0));
                    src["removed"] = r.ok ? r.result : json{{"error", r.error}};
                }
                if (!plan["lights"].empty())
                {
                    RpcResult placed = client->call("placeLights", {{"lights", plan["lights"]}}, placementTimeoutMs(plan["lights"].size()));
                    plan["lightsPlaced"] = placed.ok ? placed.result : json{{"error", placed.error}};
                }
                return plan;
            };
            if (a.get<bool>("wait"))
                return job();
            Link::get().post([job]() mutable {
                json done = job();
                mainq::post([done] {
                    for (const json& grp : done["groups"])
                    {
                        const std::string source = grp.value("source", std::string());
                        if (grp.contains("error"))
                            log::error("Unreal placements of {}: {}", source, grp["error"].get<std::string>());
                        else if (grp.contains("placed") && grp["placed"].contains("error"))
                            log::error("Unreal placements of {}: {}", source, grp["placed"]["error"].get<std::string>());
                        else if (grp.contains("import") && grp["import"].contains("error"))
                            log::error("Unreal import of {}: {}", source, grp["import"]["error"].get<std::string>());
                        else
                            log::info("Unreal: placed {} member(s) of {} as {}", grp["placements"].size(), source, grp["mode"].get<std::string>());
                        if (grp.contains("placed"))
                            for (const json& w : grp["placed"].value("warnings", json::array()))
                                log::warn("Unreal placements of {}: {}", source, w.is_string() ? w.get<std::string>() : w.dump());
                    }
                    if (const json lit = done.value("lightsPlaced", json()); lit.contains("error"))
                        log::error("Unreal lights: {}", lit["error"].get<std::string>());
                    else if (!lit.is_null())
                        log::info("Unreal: placed {} light(s)", done["lights"].size());
                });
            });
            return {{"queued", true}, {"groups", plan["groups"].size()}, {"lights", plan["lights"].size()},
                    {"lightingChannels", plan["lightingChannels"]}, {"issues", plan["issues"]}};
        },
        .examples = {"unrealPlaceInstances(\"Bolt\", folder=\"/Game/Kit\")", "unrealPlaceInstances(mode=\"hism\", wait=True)"},
    });

    ops::registerCommand({
        .name = "unrealCustomData",
        .doc = "Name what unrealPlaceInstances sends as each placement's custom primitive data, in slot order, on the instance "
               "sources of the given meshes: random (a stable 0 to 1 per placement), or a number, bool or vector attribute on "
               "the members (a vector takes 3 slots), or for geometry nodes instances their instance attribute of that name. "
               "A material reads slot n with PerInstanceCustomData (ISM/HISM) or CustomPrimitiveData (actors). "
               "With query=True, returns the names. Default [\"random\"].",
        .positionalDoc = "objects",
        .flags = {{"data", "d", T::Any, nullptr, "The names, a list: [\"random\", \"wear\", \"tint\"]. An empty list sends none."},
                  {"query", "q", T::Bool, false, "Return the names of the first object's source."}},
        .run = [](const Args& a) -> json {
            scene::Graph& g = scene::Graph::current();
            std::vector<scene::NodeId> sources;
            for (const std::string& name : a.objects())
            {
                scene::Node* n = g.find(name);
                if (!n || !placeable(n))
                    throw CommandError("unrealCustomData: '" + name + "' is not a mesh");
                const scene::NodeId src = scene::instancing::root(g, n->id());
                if (std::ranges::find(sources, src) == sources.end())
                    sources.push_back(src);
            }
            if (sources.empty())
                throw CommandError("unrealCustomData: give meshes");
            if (a.get<bool>("query"))
                return customDataNames(*g.node(sources.front()));
            const json data = a.has("data") ? a.flags.at("data") : json();
            std::vector<std::string> names;
            if (!data.is_array())
                throw CommandError("unrealCustomData: data is a list of names");
            for (const json& d : data)
            {
                if (!d.is_string() || d.get<std::string>().empty())
                    throw CommandError("unrealCustomData: data holds names, not " + d.dump());
                names.push_back(d.get<std::string>());
            }
            for (scene::NodeId src : sources)
                setCustomDataNames(g, src, names);
            return names;
        },
        .examples = {"unrealCustomData(\"Boulder\", data=[\"random\", \"wear\", \"tint\"])"},
    });

    ops::registerCommand({
        .name = "unrealRemovePlacements",
        .doc = "Remove from Unreal's open level what unrealPlaceInstances placed for the instance groups of the given meshes. "
               "Returns how many actors and instances went.",
        .positionalDoc = "objects",
        .undoable = false,
        .run = [](const Args& a) -> json {
            scene::Graph& g = scene::Graph::current();
            std::vector<scene::NodeId> roots;
            for (const std::string& name : a.objects())
                if (scene::Node* n = g.find(name))
                    roots.push_back(n->id());
            const json plan = placementPlan(g, roots, PlacementOptions{});
            json groups = json::array(), ids = json::array();
            size_t count = plan["lights"].size();
            for (const json& grp : plan["groups"])
            {
                groups.push_back(grp["key"]);
                count += grp["placements"].size();
            }
            for (const json& l : plan["lights"])
                ids.push_back(l["id"]);
            json prefixes = json::array();
            for (const json& src : plan["gnSources"])
                prefixes.push_back(src["prefix"]);
            if (groups.empty() && ids.empty())
                throw CommandError("unrealRemovePlacements: select meshes or lights whose placements to remove");
            try
            {
                Link::get().ensureConnected();
            }
            catch (const std::runtime_error& e)
            {
                throw CommandError(std::string("not connected to Unreal: ") + e.what());
            }
            std::shared_ptr<RpcClient> client = Link::get().client();
            if (!client)
                throw CommandError("not connected to Unreal");
            RpcResult r = client->call("removePlacements", {{"groups", groups}, {"ids", ids}, {"prefixes", prefixes}}, placementTimeoutMs(count));
            if (!r.ok)
                throw CommandError("unrealRemovePlacements: " + r.error);
            return r.result;
        },
    });
}
} // namespace lila::link
