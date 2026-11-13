// Crowds in the viewport: every agent as its character (rigid pieces, instanced), a proxy skeleton or a point, read
// from the simulation cache without making edits wait on the solver.

#include "CrowdDraw.h"

#include <lila/Deformer.h>
#include <lila/Interchange.h>
#include <lila/Mesh.h>
#include <lila/Profile.h>
#include <lila/Rig.h>
#include <lila/SimPreview.h>
#include <lila/Transform.h>
#include <lila/Viewport.h>

#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtx/quaternion.hpp>

#include <algorithm>
#include <map>
#include <numeric>

using namespace lila;
using namespace lila::scene;
using namespace lila::ui;

namespace crowds
{
namespace
{
/** Agent tints by character, linear; neutral enough to read next to selection colours. */
const glm::vec3 kTints[] = {{0.50f, 0.42f, 0.36f}, {0.34f, 0.40f, 0.50f}, {0.46f, 0.46f, 0.44f}, {0.38f, 0.46f, 0.36f},
                            {0.52f, 0.38f, 0.42f}, {0.40f, 0.36f, 0.50f}};

glm::vec3 tintFor(int character) { return kTints[size_t(std::max(character, 0)) % std::size(kTints)]; }

int attrOf(const Node& n, const char* name) { return n.type().attrIndex(name); }

int jointIndex(const retarget::SceneSkeleton& sk, NodeId joint)
{
    const auto it = std::find(sk.joints.begin(), sk.joints.end(), joint);
    return it == sk.joints.end() ? -1 : int(it - sk.joints.begin());
}

std::map<std::string, std::shared_ptr<CharacterParts>>& partsCache()
{
    static auto* m = new std::map<std::string, std::shared_ptr<CharacterParts>>;
    return *m;
}

/** Skins whose influences are joints of the skeleton. */
std::vector<NodeId> skinsOf(Graph& g, const retarget::SceneSkeleton& sk)
{
    std::vector<NodeId> out;
    for (NodeId id : g.nodes())
    {
        const Node* n = g.node(id);
        if (!n || !n->type().isA("mesh"))
            continue;
        const NodeId skin = skin::find(g, id);
        if (!skin)
            continue;
        for (const skin::Influence& i : skin::influences(g, skin))
            if (i.joint && jointIndex(sk, i.joint) >= 0)
            {
                out.push_back(skin);
                break;
            }
    }
    return out;
}

uint64_t skinKey(Graph& g, const std::vector<NodeId>& skins)
{
    uint64_t h = 1469598103934665603ull;
    for (NodeId s : skins)
    {
        const MeshPtr m = skin::inputMesh(g, s);
        const uint64_t v = uint64_t(uintptr_t(m.get())) ^ (uint64_t(s) << 40);
        h = sim::hashBytes(&v, sizeof v, h);
    }
    return h;
}

std::shared_ptr<CharacterParts> buildParts(Graph& g, NodeId joint, const std::vector<NodeId>& skins)
{
    LILA_ZONE();
    auto parts = std::make_shared<CharacterParts>();
    parts->map.emplace(motion::CharacterMap::forJoint(g, joint));
    const retarget::SceneSkeleton& sk = parts->map->scene();
    g.evaluate();
    for (NodeId s : skins)
    {
        const MeshPtr rest = skin::inputMesh(g, s);
        const NodeId meshNode = skin::meshOf(g, s);
        if (!rest || !meshNode)
            continue;
        const std::vector<skin::Influence> infs = skin::influences(g, s);
        const std::vector<glm::dmat4> object = skin::objectMatrices(g, s);
        const WeightTable table = skin::table(g, s);
        const glm::dmat4 meshWorld = g.node(meshNode)->as<glm::dmat4>(Transform::kWorldMatrix);
        std::map<uint32_t, const skin::Influence*> byIndex;
        for (const skin::Influence& i : infs)
            byIndex[i.index] = &i;
        // Faces to the influence with the most weight over their points.
        std::map<uint32_t, std::vector<geo::Index>> facesOf;
        for (geo::Index f = 0; f < rest->faceCapacity(); ++f)
        {
            if (!rest->faceValid(f))
                continue;
            std::map<uint32_t, float> sum;
            for (geo::Index p : rest->facePoints(f))
                if (p < table.points.size())
                    for (const auto& [inf, w] : table.points[p])
                        sum[inf] += w;
            if (sum.empty())
                continue;
            const auto best = std::max_element(sum.begin(), sum.end(), [](const auto& a, const auto& b) { return a.second < b.second; });
            facesOf[best->first].push_back(f);
        }
        for (const auto& [inf, faces] : facesOf)
        {
            auto it = byIndex.find(inf);
            if (it == byIndex.end() || !it->second->joint || inf >= object.size())
                continue;
            const int j = jointIndex(sk, it->second->joint);
            if (j < 0)
                continue;
            const glm::dmat4 jointWorld = g.node(it->second->joint)->as<glm::dmat4>(Transform::kWorldMatrix);
            const glm::dmat4 toJoint = glm::inverse(jointWorld) * meshWorld * object[inf];
            auto piece = std::make_shared<geo::Mesh>();
            std::map<geo::Index, geo::Index> remap;
            for (geo::Index f : faces)
            {
                std::vector<geo::Index> pts;
                for (geo::Index p : rest->facePoints(f))
                {
                    auto [r, fresh] = remap.try_emplace(p, 0);
                    if (fresh)
                        r->second = piece->addPoint(glm::dvec3(toJoint * glm::dvec4(rest->position(p), 1.0)));
                    pts.push_back(r->second);
                }
                piece->addFace(std::span<const geo::Index>(pts));
            }
            parts->pieces.push_back({.mesh = renderMeshFor(MeshPtr(std::move(piece))), .joint = j});
        }
    }
    if (parts->pieces.empty())
        parts->problem = "no skinned mesh on its skeleton: drawn as a proxy";
    return parts;
}

/** The SOMA bones the proxy draws: model joints and their model-joint parents, with a radius each (m). */
struct Bone
{
    int joint = -1, parent = -1;
    float radius = 0.05f;
};

const std::vector<Bone>& proxyBones()
{
    static const std::vector<Bone> bones = [] {
        const motion::SomaSkeleton& s = motion::SomaSkeleton::shipped();
        std::vector<int> model;
        for (const char* n : motion::modelJointNames())
            model.push_back(s.find(n));
        std::vector<Bone> out;
        for (size_t k = 0; k < model.size(); ++k)
        {
            const int j = model[k];
            int p = s.parents[size_t(j)];
            while (p >= 0 && std::find(model.begin(), model.end(), p) == model.end())
                p = s.parents[size_t(p)];
            if (p < 0)
                continue;
            const std::string& name = s.names[size_t(j)];
            float r = 0.045f;
            if (name.find("Spine") != std::string::npos || name.find("Chest") != std::string::npos)
                r = 0.11f;
            else if (name.find("Neck") != std::string::npos)
                r = 0.05f;
            else if (name.find("Head") != std::string::npos)
                r = 0.09f;
            else if (name.find("Leg") != std::string::npos || name.find("Shin") != std::string::npos)
                r = 0.065f;
            else if (name.find("Shoulder") != std::string::npos)
                r = 0.06f;
            out.push_back({j, p, r});
        }
        return out;
    }();
    return bones;
}

std::shared_ptr<const render::RenderMesh> unitCapsule()
{
    static std::shared_ptr<const render::RenderMesh> m = [] {
        geo::CapsuleOptions o;
        o.radius = 0.5;
        o.height = 2.0;
        o.subdivAxis = 10;
        o.subdivCaps = 3;
        return renderMeshFor(std::make_shared<const geo::Mesh>(geo::makeCapsule(o)));
    }();
    return m;
}

/** A capsule from a to b (cm), radius r (cm), for the unit capsule (Y axis, half height 1). */
glm::mat4 boneMatrix(const glm::dvec3& a, const glm::dvec3& b, double r)
{
    const glm::dvec3 d = b - a;
    const double len = glm::length(d);
    const glm::dvec3 y = len > 1e-9 ? d / len : glm::dvec3(0, 1, 0);
    const glm::dvec3 ref = std::abs(y.y) < 0.9 ? glm::dvec3(0, 1, 0) : glm::dvec3(1, 0, 0);
    const glm::dvec3 x = glm::normalize(glm::cross(ref, y)), z = glm::cross(x, y);
    const double half = std::max(len * 0.5 + r * 0.5, r);
    glm::dmat4 m(1.0);
    m[0] = glm::dvec4(x * (2.0 * r), 0.0);
    m[1] = glm::dvec4(y * half, 0.0);
    m[2] = glm::dvec4(z * (2.0 * r), 0.0);
    m[3] = glm::dvec4((a + b) * 0.5, 1.0);
    return glm::mat4(m);
}

enum Quality : int64_t
{
    kAuto,
    kFull,
    kProxy,
    kPoints,
};

void drawCrowd(drawOverrides::DrawContext& c)
{
    LILA_ZONE();
    Graph& g = Graph::current();
    const Node& n = c.node;
    if (!n.as<bool>(attrOf(n, "preview")) || !n.as<bool>(attrOf(n, "enabled")))
        return;
    std::shared_ptr<const sim::Frame> f;
    try
    {
        f = simPreviewFrame(g, n.id());
    }
    catch (const std::exception&)
    {
        return;
    }
    std::shared_ptr<const CrowdView> v = viewOf(g, n.id());
    if (!v)
        return;
    const Setup& setup = v->runtime->setup();
    const size_t count = setup.agents.size();
    const bool selected = c.highlight == drawOverrides::Highlight::Selected;
    const glm::vec4 wire = selected ? c.color : glm::vec4(0.0f);

    if (!f)
    {
        // Not simulated yet: where each agent starts.
        for (const AgentSpec& a : setup.agents)
        {
            const glm::dvec3 p(a.start.x * kCm, v->ground.at(a.start.x, a.start.y) * kCm, a.start.y * kCm);
            const glm::dvec3 d(std::sin(a.heading) * 30.0, 0.0, std::cos(a.heading) * 30.0);
            c.frame.lines.push_back({glm::vec3(p), c.color});
            c.frame.lines.push_back({glm::vec3(p + glm::dvec3(0, 170, 0)), c.color});
            c.frame.lines.push_back({glm::vec3(p + glm::dvec3(0, 100, 0)), c.color});
            c.frame.lines.push_back({glm::vec3(p + glm::dvec3(0, 100, 0) + d), c.color});
        }
        return;
    }

    const int64_t quality = n.as<int64_t>(attrOf(n, "quality"));
    const int64_t fullLimit = n.as<int64_t>(attrOf(n, "fullLimit"));

    // Which agents draw in full: all (Full), none (Proxy, Points), or the nearest fullLimit (Auto).
    std::vector<uint8_t> full(count, 0);
    std::vector<std::shared_ptr<const CharacterParts>> parts;
    for (const std::string& ch : v->characters)
        parts.push_back(partsFor(g, ch));
    auto hasParts = [&](int ch) { return ch >= 0 && size_t(ch) < parts.size() && parts[size_t(ch)] && !parts[size_t(ch)]->pieces.empty(); };
    if (quality == kFull || quality == kAuto)
    {
        std::vector<std::pair<double, size_t>> order;
        for (size_t i = 0; i < count; ++i)
            if (f->world.size() > i && f->world[i][3].w != 0.0 && hasParts(setup.agents[i].character))
                order.push_back({glm::length(glm::dvec3(f->world[i][3]) - c.view.eye), i});
        std::sort(order.begin(), order.end());
        const size_t limit = quality == kFull ? order.size() : size_t(std::max<int64_t>(0, fullLimit));
        for (size_t k = 0; k < order.size() && k < limit; ++k)
            full[order[k].second] = 1;
    }

    // Instances: per character and piece for full agents, per character tint for proxies.
    std::map<std::pair<int, size_t>, std::vector<glm::mat4>> pieceInstances;
    std::map<int, std::vector<glm::mat4>> proxyInstances;
    motion::Pose pose;
    const motion::SomaSkeleton& soma = motion::SomaSkeleton::shipped();
    for (size_t i = 0; i < count; ++i)
    {
        const int ch = setup.agents[i].character;
        if (quality == kPoints)
        {
            const glm::dmat4 m = i < f->world.size() ? f->world[i] : glm::dmat4(0.0);
            if (m[3].w == 0.0)
                continue;
            const glm::dvec3 p(m[3]);
            const glm::dvec3 fwd = glm::dvec3(m[2]) * 35.0;
            const glm::vec4 col(tintFor(ch) * 1.6f, 1.0f);
            c.frame.lines.push_back({glm::vec3(p), col});
            c.frame.lines.push_back({glm::vec3(p + glm::dvec3(0, 175, 0)), col});
            c.frame.lines.push_back({glm::vec3(p + glm::dvec3(0, 120, 0)), col});
            c.frame.lines.push_back({glm::vec3(p + glm::dvec3(0, 120, 0) + fwd), col});
            continue;
        }
        if (!agentPose(*v, f->state, i, pose))
            continue;
        if (full[i])
        {
            const CharacterParts& cp = *parts[size_t(ch)];
            const retarget::Pose rp = cp.map->toCharacter(pose);
            const std::vector<glm::dmat4> world = retarget::worldOf(cp.map->skeleton(), rp);
            for (size_t k = 0; k < cp.pieces.size(); ++k)
                pieceInstances[{ch, k}].push_back(glm::mat4(world[size_t(cp.pieces[k].joint)]));
            continue;
        }
        const motion::Pose w = soma.world(pose);
        auto& list = proxyInstances[ch];
        for (const Bone& b : proxyBones())
            list.push_back(boneMatrix(glm::dvec3(w[size_t(b.parent)].p) * kCm, glm::dvec3(w[size_t(b.joint)].p) * kCm, b.radius * kCm));
    }
    for (auto& [key, inst] : pieceInstances)
    {
        render::MeshDraw d;
        d.mesh = parts[size_t(key.first)]->pieces[key.second].mesh;
        d.instances = std::make_shared<const std::vector<glm::mat4>>(std::move(inst));
        d.baseColor = tintFor(key.first);
        d.flags = render::DrawShaded | (selected ? render::DrawWire : 0u);
        d.wireColor = wire;
        d.node = n.id();
        c.frame.meshes.push_back(std::move(d));
    }
    for (auto& [ch, inst] : proxyInstances)
    {
        render::MeshDraw d;
        d.mesh = unitCapsule();
        d.instances = std::make_shared<const std::vector<glm::mat4>>(std::move(inst));
        d.baseColor = tintFor(ch);
        d.flags = render::DrawShaded | (selected ? render::DrawWire : 0u);
        d.wireColor = wire;
        d.node = n.id();
        c.frame.meshes.push_back(std::move(d));
    }

    if (n.as<bool>(attrOf(n, "showPaths")))
        for (size_t i = 0; i < count && i < f->world.size(); ++i)
        {
            const AgentState& a = v->runtime->agent(f->state, i);
            if (a.alive < 0.5 || a.moving < 0.5)
                continue;
            const glm::dvec3 from(f->world[i][3]);
            const glm::dvec3 to(a.goalX * kCm, v->ground.at(a.goalX, a.goalZ) * kCm, a.goalZ * kCm);
            c.frame.lines.push_back({glm::vec3(from + glm::dvec3(0, 5, 0)), c.color});
            c.frame.lines.push_back({glm::vec3(to + glm::dvec3(0, 5, 0)), c.color});
        }
}

std::optional<double> pickCrowd(const drawOverrides::PickContext& c)
{
    Graph& g = Graph::current();
    std::shared_ptr<const sim::Frame> f = sim::cached(g, c.node.id(), sim::frameOf(g.time()));
    std::shared_ptr<const CrowdView> v = viewOf(g, c.node.id());
    if (!v)
        return std::nullopt;
    std::vector<glm::dvec3> at;
    if (f)
        for (const glm::dmat4& m : f->world)
        {
            if (m[3].w != 0.0)
                at.push_back(glm::dvec3(m[3]) + glm::dvec3(0, 90, 0));
        }
    else
        for (const AgentSpec& a : v->runtime->setup().agents)
            at.push_back({a.start.x * kCm, v->ground.at(a.start.x, a.start.y) * kCm + 90.0, a.start.y * kCm});
    double best = 1e30;
    const QRectF box = QRectF(c.a, c.b).normalized();
    for (const glm::dvec3& p : at)
        if (std::optional<QPointF> s = c.view.project(p))
        {
            if (!c.click)
            {
                if (box.contains(*s))
                    return 0.0;
                continue;
            }
            const double r = 25.0 / std::max(c.view.unitsPerPixel(p), 1e-9);
            best = std::min(best, std::max(0.0, std::hypot(s->x() - c.a.x(), s->y() - c.a.y()) - r));
        }
    return best < 1e29 && c.click ? std::optional<double>(best) : std::nullopt;
}
} // namespace

NodeId characterJoint(Graph& g, NodeId node)
{
    std::vector<NodeId> stack{node};
    while (!stack.empty())
    {
        const NodeId id = stack.back();
        stack.pop_back();
        if (const Node* n = g.node(id); n && rig::isJoint(*n))
            return id;
        const auto& kids = g.children(id);
        for (auto it = kids.rbegin(); it != kids.rend(); ++it)
            stack.push_back(*it);
    }
    return kNoNode;
}

std::shared_ptr<const CharacterParts> partsFor(Graph& g, const std::string& character)
{
    Node* n = nullptr;
    try
    {
        n = g.find(character);
    }
    catch (const std::exception&)
    {
    }
    auto& cache = partsCache();
    auto failed = [&](const std::string& why) {
        auto p = std::make_shared<CharacterParts>();
        p->problem = why;
        cache[character] = p;
        return p;
    };
    if (!n)
        return failed("not in the scene");
    const NodeId joint = characterJoint(g, n->id());
    if (!joint)
        return failed("has no skeleton");
    retarget::SceneSkeleton sk;
    try
    {
        sk = retarget::sceneSkeleton(g, joint);
    }
    catch (const std::exception& e)
    {
        return failed(e.what());
    }
    // Characterising edits the scene, so drawing leaves that to the commands that set characters.
    if (!retarget::rigNodeFor(g, sk.top))
        return failed("not characterised yet: set it again as a crowd character");
    const std::vector<NodeId> skins = skinsOf(g, sk);
    const uint64_t key = skinKey(g, skins) ^ uint64_t(sk.top);
    if (auto it = cache.find(character); it != cache.end() && it->second->key == key && it->second->map)
        return it->second;
    try
    {
        auto p = buildParts(g, joint, skins);
        p->key = key;
        cache[character] = p;
        return p;
    }
    catch (const std::exception& e)
    {
        return failed(e.what());
    }
}

void registerDraw(ext::Context& ctx)
{
    drawOverrides::add(ctx, {.id = "crowds.agents", .nodeType = kCrowdType, .draw = drawCrowd, .pick = pickCrowd});
}
} // namespace crowds
