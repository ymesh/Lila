// Crowds: the crowd node and its solver on the base simulation framework, spawn groups, and the commands that build
// and inspect a crowd. The runtime itself (CrowdCore) knows nothing of the scene; this file reads the scene into it.

#include "Crowd.h"

#include <lila/Command.h>
#include <lila/Curves.h>
#include <lila/Log.h>
#include <lila/Mesh.h>
#include <lila/Motion.h>
#include <lila/Profile.h>
#include <lila/Relocate.h>
#include <lila/Tasks.h>
#include <lila/Anim.h>
#include <lila/Transform.h>

#include <glm/gtc/matrix_transform.hpp>

#include <algorithm>
#include <cmath>
#include <map>
#include <mutex>
#include <numbers>
#include <set>
#include <sstream>

using namespace lila;
using namespace lila::scene;

namespace crowds
{
namespace
{
constexpr double kPiD = std::numbers::pi;

int attr(const Node& n, const char* name) { return n.type().attrIndex(name); }
double num(const Node& n, const char* name) { return n.as<double>(attr(n, name)); }
int64_t integer(const Node& n, const char* name) { return n.as<int64_t>(attr(n, name)); }
bool flag(const Node& n, const char* name) { return n.as<bool>(attr(n, name)); }
const std::string& text(const Node& n, const char* name) { return n.as<std::string>(attr(n, name)); }

AttrDef number(const char* name, double def, const char* doc, std::optional<double> lo = {}, std::optional<double> hi = {})
{
    AttrDef a{.name = name, .type = PlugType::Float, .defaultValue = def, .flags = AttrInput | AttrStorable, .doc = doc};
    a.minValue = lo;
    a.maxValue = hi;
    return a;
}
AttrDef whole(const char* name, int64_t def, const char* doc, std::optional<double> lo = {}, std::optional<double> hi = {})
{
    AttrDef a{.name = name, .type = PlugType::Int, .defaultValue = def, .flags = AttrInput | AttrStorable, .doc = doc};
    a.minValue = lo;
    a.maxValue = hi;
    return a;
}
AttrDef boolean(const char* name, bool def, const char* doc)
{
    return {.name = name, .type = PlugType::Bool, .defaultValue = def, .flags = AttrInput | AttrStorable, .doc = doc};
}
AttrDef string(const char* name, const char* doc, uint32_t extra = 0)
{
    return {.name = name, .type = PlugType::String, .defaultValue = std::string(), .flags = AttrInput | AttrStorable | extra, .doc = doc};
}
AttrDef choice(const char* name, int64_t def, std::vector<std::string> names, const char* doc)
{
    return {.name = name, .type = PlugType::Enum, .defaultValue = def, .flags = AttrInput | AttrStorable, .enumNames = std::move(names), .doc = doc};
}

Graph& graph() { return Graph::current(); }

/** Node paths in a list attr: one per line or comma separated. */
std::vector<std::string> splitList(const std::string& s)
{
    std::vector<std::string> out;
    std::string cur;
    for (char c : s + ",")
    {
        if (c == ',' || c == '\n' || c == ';')
        {
            const size_t a = cur.find_first_not_of(" \t\r"), b = cur.find_last_not_of(" \t\r");
            if (a != std::string::npos)
                out.push_back(cur.substr(a, b - a + 1));
            cur.clear();
        }
        else
            cur += c;
    }
    return out;
}

Node* named(Graph& g, const std::string& path)
{
    try
    {
        return path.empty() ? nullptr : g.find(path);
    }
    catch (const std::exception&)
    {
        return nullptr;
    }
}

glm::dmat4 worldOf(const Node& n)
{
    return n.type().isA("transform") ? n.as<glm::dmat4>(Transform::kWorldMatrix) : glm::dmat4(1.0);
}

glm::vec2 groundM(const glm::dvec3& cm) { return {float(cm.x / kCm), float(cm.z / kCm)}; }

/** World polyline of a curve node, meters on the ground. Empty when the node isn't a curve. */
std::vector<glm::vec2> curvePoints(Graph& g, const Node& n)
{
    const int ws = n.type().attrIndex("worldSpace");
    if (ws < 0)
        return {};
    std::vector<glm::vec2> out;
    for (const curve::Spline& s : curve::splinesOf(g.value({n.id(), ws, -1})))
        for (const glm::dvec3& p : curve::tessellate(s).points)
        {
            const glm::vec2 q = groundM(p);
            if (out.empty() || glm::length(out.back() - q) > 1e-3f)
                out.push_back(q);
        }
    return out;
}

MeshPtr meshOf(const Node& n)
{
    const int a = n.type().attrIndex("outMesh");
    return a >= 0 ? n.as<MeshPtr>(a) : MeshPtr();
}

/** Convex hull, counter-clockwise seen from above (x, z as a right-handed plane with z up the page). */
std::vector<glm::dvec2> hull(std::vector<glm::dvec2> pts)
{
    std::sort(pts.begin(), pts.end(), [](const glm::dvec2& a, const glm::dvec2& b) { return a.x < b.x || (a.x == b.x && a.y < b.y); });
    pts.erase(std::unique(pts.begin(), pts.end()), pts.end());
    if (pts.size() < 3)
        return pts;
    auto cross = [](glm::dvec2 o, glm::dvec2 a, glm::dvec2 b) { return (a.x - o.x) * (b.y - o.y) - (a.y - o.y) * (b.x - o.x); };
    std::vector<glm::dvec2> h(pts.size() * 2);
    size_t k = 0;
    for (size_t i = 0; i < pts.size(); ++i)
    {
        while (k >= 2 && cross(h[k - 2], h[k - 1], pts[i]) <= 0)
            --k;
        h[k++] = pts[i];
    }
    for (size_t i = pts.size() - 1, t = k + 1; i > 0; --i)
    {
        while (k >= t && cross(h[k - 2], h[k - 1], pts[i - 1]) <= 0)
            --k;
        h[k++] = pts[i - 1];
    }
    h.resize(k - 1);
    return h;
}

/** Footprints of a node and the meshes below it: one convex polygon each (meters). */
void footprints(Graph& g, NodeId id, std::vector<orca::Obstacle>& out, uint64_t& h)
{
    std::vector<NodeId> nodes{id};
    for (NodeId d : g.descendants(id))
        nodes.push_back(d);
    for (NodeId nid : nodes)
    {
        const Node* n = g.node(nid);
        const MeshPtr m = n ? meshOf(*n) : MeshPtr();
        if (!m)
            continue;
        const glm::dmat4 w = worldOf(*n);
        std::vector<glm::dvec2> pts;
        for (geo::Index p = 0; p < m->pointCapacity(); ++p)
            if (m->pointValid(p))
            {
                const glm::dvec3 q = glm::dvec3(w * glm::dvec4(m->position(p), 1.0));
                pts.push_back({q.x / kCm, q.z / kCm});
            }
        orca::Obstacle o{hull(std::move(pts))};
        // Counter-clockwise for the runtime: hull() turns the other way in (x, z).
        std::reverse(o.points.begin(), o.points.end());
        if (o.points.size() >= 3)
        {
            h = sim::hashBytes(o.points.data(), o.points.size() * sizeof(glm::dvec2), h);
            out.push_back(std::move(o));
        }
    }
}

/** Rasterises the ground's triangles into a grid of heights (meters). */
Ground rasterGround(Graph& g, const Node& ground, glm::dvec2 lo, glm::dvec2 hi)
{
    LILA_ZONE();
    Ground gr;
    std::vector<NodeId> nodes{ground.id()};
    for (NodeId d : g.descendants(ground.id()))
        nodes.push_back(d);
    const double extent = std::max(hi.x - lo.x, hi.y - lo.y);
    gr.cell = std::max(0.5, extent / 1024.0);
    gr.origin = lo;
    gr.nx = std::max(1, int(std::ceil((hi.x - lo.x) / gr.cell)) + 1);
    gr.nz = std::max(1, int(std::ceil((hi.y - lo.y) / gr.cell)) + 1);
    gr.heights.assign(size_t(gr.nx) * size_t(gr.nz), std::numeric_limits<float>::quiet_NaN());
    for (NodeId nid : nodes)
    {
        const Node* n = g.node(nid);
        const MeshPtr m = n ? meshOf(*n) : MeshPtr();
        if (!m)
            continue;
        const glm::dmat4 w = worldOf(*n);
        for (geo::Index f = 0; f < m->faceCapacity(); ++f)
        {
            if (!m->faceValid(f))
                continue;
            std::vector<glm::dvec3> poly;
            for (geo::Index p : m->facePoints(f))
                poly.push_back(glm::dvec3(w * glm::dvec4(m->position(p), 1.0)) / kCm);
            for (size_t k = 1; k + 1 < poly.size(); ++k)
            {
                const glm::dvec3 a = poly[0], b = poly[k], c = poly[k + 1];
                const double minX = std::min({a.x, b.x, c.x}), maxX = std::max({a.x, b.x, c.x});
                const double minZ = std::min({a.z, b.z, c.z}), maxZ = std::max({a.z, b.z, c.z});
                const int x0 = std::max(0, int(std::floor((minX - lo.x) / gr.cell))), x1 = std::min(gr.nx - 1, int(std::ceil((maxX - lo.x) / gr.cell)));
                const int z0 = std::max(0, int(std::floor((minZ - lo.y) / gr.cell))), z1 = std::min(gr.nz - 1, int(std::ceil((maxZ - lo.y) / gr.cell)));
                const double den = (b.z - c.z) * (a.x - c.x) + (c.x - b.x) * (a.z - c.z);
                if (std::abs(den) < 1e-12)
                    continue;
                for (int z = z0; z <= z1; ++z)
                    for (int x = x0; x <= x1; ++x)
                    {
                        const double px = lo.x + x * gr.cell, pz = lo.y + z * gr.cell;
                        const double u = ((b.z - c.z) * (px - c.x) + (c.x - b.x) * (pz - c.z)) / den;
                        const double v = ((c.z - a.z) * (px - c.x) + (a.x - c.x) * (pz - c.z)) / den;
                        const double t = 1.0 - u - v;
                        constexpr double kE = -1e-6;
                        if (u < kE || v < kE || t < kE)
                            continue;
                        const float y = float(u * a.y + v * b.y + t * c.y);
                        float& cellH = gr.heights[size_t(z) * size_t(gr.nx) + size_t(x)];
                        if (std::isnan(cellH) || y > cellH)
                            cellH = y;
                    }
            }
        }
    }
    return gr;
}

const char* detailName(Detail d) { return d == Detail::Generated ? "generated" : d == Detail::Cycles ? "cycles" : "auto"; }

/** The engine as the runtime's generator; reports each batch to the progress card. */
class EngineGenerator : public Generator
{
public:
    explicit EngineGenerator(motion::Engine& e) : mEngine(e) {}
    /** Batches are padded to this many requests: a GPU session compiles once per batch size, so keep it to one. */
    void setBatch(int n) { mBatch = std::max(1, n); }
    std::vector<Result> generate(std::span<const Request> requests) override
    {
        progress::Scope bar("Generating crowd motion", 0.0, progress::None, 0.0);
        bar.setStatus(std::to_string(requests.size()) + " agents on " + mEngine.info().device);
        if (requests.empty() || int(requests.size()) >= mBatch)
            return mEngine.generate(requests);
        std::vector<Request> padded(requests.begin(), requests.end());
        while (int(padded.size()) < mBatch)
            padded.push_back(requests.back());
        std::vector<Result> out = mEngine.generate(padded);
        out.resize(requests.size());
        return out;
    }
    std::string device() const override { return mEngine.info().device; }

private:
    motion::Engine& mEngine;
    int mBatch = 16;
};

std::mutex gViewsMutex;
std::map<NodeId, std::shared_ptr<CrowdView>>& views()
{
    static auto* v = new std::map<NodeId, std::shared_ptr<CrowdView>>;
    return *v;
}

/** Reads a stored group into the runtime's form (meters); point nodes are added to points as they're met. */
Group groupFrom(Graph& g, const json& j, std::vector<NodeId>& points, const std::map<std::string, int>& groupIndex, uint64_t& h)
{
    Group gr;
    gr.name = j.value("name", "group");
    gr.behaviour = behaviourFrom(j.value("behaviour", "wander"));
    gr.style = j.value("style", "walk");
    if (j.contains("center"))
        gr.center = {float(j["center"][0].get<double>() / kCm), float(j["center"][1].get<double>() / kCm)};
    gr.radius = float(j.value("radius", 1000.0) / kCm);
    gr.yaw = float(j.value("yaw", 0.0) * kPiD / 180.0);
    gr.pauseMin = float(j.value("pauseMin", 1.0));
    gr.pauseMax = float(j.value("pauseMax", 4.0));
    gr.loop = j.value("loop", false);
    gr.spacing = float(j.value("spacing", 90.0) / kCm);
    gr.serve = float(j.value("serve", 6.0));
    gr.stay = float(j.value("stay", 0.0));
    gr.range = float(j.value("range", 800.0) / kCm);
    gr.useNetwork = j.value("network", false);
    gr.then = behaviourFrom(j.value("then", gr.behaviour == Behaviour::Queue || gr.behaviour == Behaviour::Spot ? "wander" : "stand"));
    for (const json& p : j.value("area", json::array()))
        gr.area.push_back({float(p[0].get<double>() / kCm), float(p[1].get<double>() / kCm)});
    auto pointInput = [&](const std::string& key) -> int {
        if (!j.contains(key))
            return -1;
        const json& v = j[key];
        if (v.is_array() && v.size() >= 3)
        {
            gr.goal = {float(v[0].get<double>() / kCm), float(v[2].get<double>() / kCm)};
            return -1;
        }
        if (Node* n = v.is_string() ? named(g, v.get<std::string>()) : nullptr)
        {
            auto it = std::find(points.begin(), points.end(), n->id());
            if (it != points.end())
                return int(it - points.begin());
            points.push_back(n->id());
            return int(points.size()) - 1;
        }
        return -1;
    };
    gr.input = pointInput(j.contains("goal") ? "goal" : j.contains("threat") ? "threat" : "target");
    if (j.contains("path"))
        if (Node* n = named(g, j["path"].get<std::string>()))
        {
            gr.path = curvePoints(g, *n);
            h = sim::hashBytes(gr.path.data(), gr.path.size() * sizeof(glm::vec2), h);
        }
    for (const json& s : j.value("spots", json::array()))
        if (Node* n = named(g, s.get<std::string>()); n && n->type().isA(motion::kSpotType))
        {
            const auto [at, heading] = motion::groundFrame(g, n->id());
            PlacedSpot ps{.targets = motion::placeSpot(motion::spotOf(g, n->id()), at, heading), .at = at, .heading = heading};
            for (const auto& t : ps.targets)
                for (const Pose& p : t.poses)
                    h = sim::hashBytes(&p[0].p, sizeof(glm::vec3), h);
            gr.spots.push_back(std::move(ps));
        }
    if (j.contains("prey"))
        if (auto it = groupIndex.find(j["prey"].get<std::string>()); it != groupIndex.end())
            gr.prey = it->second;
    return gr;
}

/** The lanes of a settlement (sidewalks, footpaths, crosswalks) from its lane graph, meters. */
void settlementLanes(const std::string& settlement, Network& net)
{
    const json r = ops::execute("settlementLanes", {{"args", {settlement}}, {"lanes", true}});
    std::map<int64_t, size_t> laneOf;
    for (const json& l : r.value("laneList", json::array()))
    {
        if (l.value("mode", "") != "pedestrian")
            continue;
        Network::Lane lane;
        for (const json& p : l["points"])
            lane.points.push_back({float(p[0].get<double>() / kCm), float(p[2].get<double>() / kCm)});
        lane.width = float(l.value("width", 200.0) / kCm);
        if (lane.points.size() < 2)
            continue;
        laneOf[l["id"].get<int64_t>()] = net.lanes.size();
        net.lanes.push_back(std::move(lane));
    }
    // A junction's phases: as many as its links name.
    std::map<int64_t, int> phases;
    for (const json& k : r.value("linkList", json::array()))
        if (const int64_t j = k.value("junction", int64_t(-1)); j >= 0)
            phases[j] = std::max(phases[j], int(k.value("phase", -1)) + 1);
    // Crossings and corners join lanes whose ends don't meet: walk straight across between the nearest ends.
    for (const json& k : r.value("linkList", json::array()))
    {
        auto a = laneOf.find(k["from"].get<int64_t>()), b = laneOf.find(k["to"].get<int64_t>());
        if (a == laneOf.end() || b == laneOf.end() || a->second == b->second)
            continue;
        const auto& la = net.lanes[a->second].points;
        const auto& lb = net.lanes[b->second].points;
        glm::vec2 pa = la.front(), pb = lb.front();
        float best = 1e30f;
        for (glm::vec2 x : {la.front(), la.back()})
            for (glm::vec2 y : {lb.front(), lb.back()})
                if (glm::length(x - y) < best)
                    best = glm::length(x - y), pa = x, pb = y;
        if (best < 0.5f)
            continue;
        const std::string move = k.value("move", "");
        Network::Lane link{.points = {pa, pb}, .width = 3.0f, .crossing = move == "crosswalk" || move == "crossing"};
        if (const int64_t j = k.value("junction", int64_t(-1)); link.crossing && j >= 0 && k.value("phase", -1) >= 0)
        {
            link.junction = settlement + "#" + std::to_string(j);
            link.phase = k.value("phase", -1);
            link.phases = phases[j];
        }
        net.lanes.push_back(std::move(link));
    }
}

class CrowdSolver : public sim::Solver
{
public:
    explicit CrowdSolver(NodeId node) : mNode(node) {}
    ~CrowdSolver() override
    {
        std::lock_guard lock(gViewsMutex);
        views().erase(mNode);
    }

    sim::Setup configure(Graph& g) override
    {
        LILA_ZONE();
        Node* n = g.node(mNode);
        if (!n)
            throw std::invalid_argument("the crowd node is gone");
        auto view = std::make_shared<CrowdView>();
        Setup setup;
        uint64_t h = 0x51ed2701c0ffee11ull;
        auto hashText = [&](const std::string& s) { h = sim::hashBytes(s.data(), s.size(), h); };
        for (const char* a : {"groups", "network", "obstacles", "ground", "focus"})
            hashText(text(*n, a));
        Options& o = setup.options;
        o.seed = uint32_t(integer(*n, "seed"));
        o.batch = int(std::clamp<int64_t>(integer(*n, "batch"), 1, 64));
        o.lookahead = num(*n, "lookahead");
        o.avoidance = flag(*n, "avoidance");
        o.orca.timeHorizon = num(*n, "timeHorizon");
        o.detail = Detail(integer(*n, "detail"));
        o.generatedRadius = num(*n, "generatedRadius") / kCm;
        o.generatedMax = int(integer(*n, "generatedMax"));
        for (double v : {double(o.seed), double(o.batch), o.lookahead, double(o.avoidance), o.orca.timeHorizon, double(int(o.detail)), o.generatedRadius,
                         double(o.generatedMax)})
            h = sim::hashBytes(&v, sizeof v, h);

        std::vector<NodeId> points;
        if (Node* f = named(g, text(*n, "focus")))
        {
            points.push_back(f->id());
            o.focusInput = 0;
        }

        // Groups and their agents.
        json groups = json::parse(text(*n, "groups").empty() ? "[]" : text(*n, "groups"), nullptr, false);
        if (!groups.is_array())
            groups = json::array();
        std::map<std::string, int> index;
        for (size_t k = 0; k < groups.size(); ++k)
            index[groups[k].value("name", "")] = int(k);
        for (size_t k = 0; k < groups.size(); ++k)
        {
            setup.groups.push_back(groupFrom(g, groups[k], points, index, h));
            for (const json& a : groups[k].value("agents", json::array()))
            {
                AgentSpec s;
                s.group = int(k);
                s.start = {float(a["p"][0].get<double>() / kCm), float(a["p"][1].get<double>() / kCm)};
                s.heading = float(a.value("h", 0.0) * kPiD / 180.0);
                s.seed = a.value("seed", uint32_t(setup.agents.size() + 1));
                s.character = a.value("character", 0);
                s.speed = float(a.value("speed", 1.0));
                s.radius = float(a.value("radius", 28.0) / kCm);
                s.appear = float(a.value("appear", 0.0));
                s.along = float(a.value("along", 0.0) / kCm);
                s.slot = a.value("slot", int(setup.agents.size()));
                setup.agents.push_back(s);
            }
        }

        // Walkable network: settlements' sidewalks and crosswalks, and curves as lanes.
        for (const std::string& p : splitList(text(*n, "network")))
        {
            Node* src = named(g, p);
            if (!src)
                continue;
            if (std::vector<glm::vec2> pts = curvePoints(g, *src); pts.size() >= 2)
            {
                setup.network.lanes.push_back({.points = std::move(pts)});
                continue;
            }
            const MeshPtr m = meshOf(*src);
            const std::string key = src->path() + "#" + std::to_string(uintptr_t(m.get()));
            auto it = mNetworks.find(key);
            if (it == mNetworks.end())
            {
                Network net;
                try
                {
                    settlementLanes(src->path(), net);
                }
                catch (const std::exception& e)
                {
                    throw std::invalid_argument(src->path() + " gives no walkable lanes: " + e.what());
                }
                it = mNetworks.emplace(key, std::move(net)).first;
            }
            setup.network.lanes.insert(setup.network.lanes.end(), it->second.lanes.begin(), it->second.lanes.end());
        }
        for (const Network::Lane& l : setup.network.lanes)
            h = sim::hashBytes(l.points.data(), l.points.size() * sizeof(glm::vec2), h);
        setup.network.connect(1.0f);

        for (const std::string& p : splitList(text(*n, "obstacles")))
            if (Node* ob = named(g, p))
                footprints(g, ob->id(), setup.obstacles, h);

        // Ground under everything the agents can reach.
        glm::dvec2 lo(1e30), hi(-1e30);
        auto grow = [&](glm::vec2 p, double r) {
            lo = glm::min(lo, glm::dvec2(p) - r);
            hi = glm::max(hi, glm::dvec2(p) + r);
        };
        for (const AgentSpec& a : setup.agents)
            grow(a.start, 20.0);
        for (const Group& gr : setup.groups)
        {
            if (gr.behaviour == Behaviour::Wander || gr.behaviour == Behaviour::Idle)
                grow(gr.center, gr.radius + 10.0);
            for (glm::vec2 p : gr.path)
                grow(p, 10.0);
        }
        for (const Network::Lane& l : setup.network.lanes)
            for (glm::vec2 p : l.points)
                grow(p, 10.0);
        if (Node* gn = named(g, text(*n, "ground")); gn && lo.x < hi.x)
        {
            const MeshPtr m = meshOf(*gn);
            const glm::dmat4 w = worldOf(*gn);
            uint64_t key = sim::hashBytes(&w, sizeof w, uintptr_t(m.get()));
            key = sim::hashBytes(&lo, sizeof lo, key);
            key = sim::hashBytes(&hi, sizeof hi, key);
            if (key != mGroundKey)
            {
                mGround = rasterGround(g, *gn, lo, hi);
                mGroundKey = key;
            }
            view->ground = mGround;
            h = sim::hashBytes(&key, sizeof key, h);
        }

        // The engine, or why there is none.
        motion::Engine* engine = o.detail == Detail::Cycles ? nullptr : motion::Engine::shared();
        std::string why;
        if (o.detail != Detail::Cycles && !engine)
            why = "motion engine unavailable (" + motion::Engine::status().message + "): every agent plays gait cycles";
        const std::string device = engine ? engine->info().device : "";
        hashText(device);

        view->fps = sim::framesPerSecond();
        view->startFrame = integer(*n, "startFrame");
        view->groups = groups;
        view->characters = splitList(text(*n, "characters"));
        view->points = points;
        if (h != mHash || !mRuntime)
        {
            if (engine && (!mGenerator || mEngine != engine))
            {
                mGenerator = std::make_unique<EngineGenerator>(*engine);
                mEngine = engine;
            }
            if (mGenerator)
                mGenerator->setBatch(engine && engine->info().device == "CPU" ? 1 : setup.options.batch);
            mRuntime = std::make_shared<Runtime>(std::move(setup), motion::GaitLibrary::shipped(), engine ? mGenerator.get() : nullptr, why);
            mHash = h;
        }
        view->runtime = mRuntime;
        view->status = !mRuntime->stats().note.empty() ? mRuntime->stats().note : "motion on " + device;
        {
            std::lock_guard lock(gViewsMutex);
            views()[mNode] = view;
        }
        mView = view;

        sim::Setup su;
        su.startFrame = view->startFrame;
        su.layout = {{sim::Channel::Ignore, uint32_t(mRuntime->stateSize())}};
        su.paramsHash = h;
        su.reads = points;
        su.loops = false;
        return su;
    }

    void sample(Graph& g, std::vector<double>& inputs) override
    {
        for (NodeId p : mView->points)
        {
            const Node* n = g.node(p);
            const glm::dvec3 t = n ? glm::dvec3(worldOf(*n)[3]) : glm::dvec3(0.0);
            inputs.push_back(t.x / kCm);
            inputs.push_back(t.z / kCm);
        }
    }

    void reset(const std::vector<double>&, std::vector<double>& state) override { mRuntime->reset(state); }

    void step(const std::vector<double>& inputs, std::vector<double>& state, const sim::StepInfo& at) override
    {
        std::vector<glm::vec2> pts;
        for (size_t k = 0; k + 1 < inputs.size(); k += 2)
            pts.push_back({float(inputs[k]), float(inputs[k + 1])});
        const double start = double(mView->startFrame) / std::max(mView->fps, 1e-6);
        mRuntime->step(state, at.dt, at.seconds - start, pts);
    }

    void pose(const std::vector<double>&, const std::vector<double>& state, std::vector<glm::dmat4>& world) override
    {
        const size_t n = mRuntime->setup().agents.size();
        world.resize(n);
        for (size_t i = 0; i < n; ++i)
            world[i] = agentMatrix(*mView, state, i);
    }

    bool inputsMayHaveChanged(Graph&) override { return !mView || !mView->points.empty(); }

private:
    NodeId mNode;
    uint64_t mHash = 0;
    std::shared_ptr<Runtime> mRuntime;
    std::shared_ptr<CrowdView> mView;
    std::unique_ptr<EngineGenerator> mGenerator;
    motion::Engine* mEngine = nullptr;
    std::map<std::string, Network> mNetworks;
    Ground mGround;
    uint64_t mGroundKey = 0;
};

// Spawning -----------------------------------------------------------------------------------------------------------

uint64_t mixHash(uint64_t x)
{
    x += 0x9E3779B97F4A7C15ull;
    x = (x ^ (x >> 30)) * 0xBF58476D1CE4E5B9ull;
    x = (x ^ (x >> 27)) * 0x94D049BB133111EBull;
    return x ^ (x >> 31);
}

/** Deterministic random numbers for spawning. */
struct Rand
{
    uint64_t s;
    double next() { return double((s = mixHash(s)) >> 11) * (1.0 / 9007199254740992.0); }
};

struct Spawned
{
    glm::dvec2 p; // cm
    double h = 0.0; // degrees
    double along = 0.0;
};

/** Points at least `gap` apart, picked by `pick` (up to 30 tries each). */
template <class F> std::vector<Spawned> scatter(int count, double gap, F&& pick)
{
    std::vector<Spawned> out;
    for (int i = 0; i < count; ++i)
    {
        Spawned best{};
        double bestGap = -1.0;
        for (int t = 0; t < 30; ++t)
        {
            Spawned c = pick();
            double d = 1e30;
            for (const Spawned& o : out)
                d = std::min(d, glm::length(o.p - c.p));
            if (d >= gap)
            {
                best = c, bestGap = d;
                break;
            }
            if (d > bestGap)
                best = c, bestGap = d;
        }
        out.push_back(best);
    }
    return out;
}
} // namespace

double Ground::at(double x, double z) const
{
    if (heights.empty())
        return fallback;
    const double fx = (x - origin.x) / cell, fz = (z - origin.y) / cell;
    const int x0 = std::clamp(int(std::floor(fx)), 0, nx - 1), z0 = std::clamp(int(std::floor(fz)), 0, nz - 1);
    const int x1 = std::min(x0 + 1, nx - 1), z1 = std::min(z0 + 1, nz - 1);
    const double u = std::clamp(fx - x0, 0.0, 1.0), v = std::clamp(fz - z0, 0.0, 1.0);
    double sum = 0.0, wsum = 0.0;
    for (auto [xi, zi, w] : {std::tuple{x0, z0, (1 - u) * (1 - v)}, std::tuple{x1, z0, u * (1 - v)}, std::tuple{x0, z1, (1 - u) * v}, std::tuple{x1, z1, u * v}})
    {
        const float hgt = heights[size_t(zi) * size_t(nx) + size_t(xi)];
        if (!std::isnan(hgt) && w > 0.0)
            sum += hgt * w, wsum += w;
    }
    return wsum > 0.0 ? sum / wsum : fallback;
}

std::shared_ptr<const CrowdView> viewOf(Graph& g, NodeId crowd)
{
    const Node* n = g.node(crowd);
    if (!n || !n->type().isA(kCrowdType))
        return nullptr;
    // Configuring is what makes the view; simulating the start frame does that and costs only a reset once cached.
    try
    {
        sim::simulate(g, crowd, n->as<int64_t>(n->type().attrIndex("startFrame")), sim::LoopMode::Off);
    }
    catch (const std::exception&)
    {
    }
    std::lock_guard lock(gViewsMutex);
    auto it = views().find(crowd);
    return it == views().end() ? nullptr : it->second;
}

glm::dmat4 agentMatrix(const CrowdView& v, const std::vector<double>& state, size_t i)
{
    const AgentState& a = v.runtime->agent(state, i);
    if (a.alive < 0.5)
        return glm::dmat4(0.0);
    const double y = v.ground.at(a.x, a.z);
    glm::dmat4 m = glm::rotate(glm::dmat4(1.0), std::atan2(a.hx, a.hz), glm::dvec3(0, 1, 0));
    m[3] = glm::dvec4(a.x * kCm, y * kCm, a.z * kCm, 1.0);
    return m;
}

bool agentPose(const CrowdView& v, const std::vector<double>& state, size_t i, motion::Pose& out)
{
    if (!v.runtime->pose(state, i, out))
        return false;
    out[0].p.y += float(v.ground.at(out[0].p.x, out[0].p.z));
    return true;
}

NodeId crowdArg(Graph& g, const std::vector<std::string>& objects, const char* cmd)
{
    for (const std::string& o : objects)
    {
        Node* n = named(g, o);
        for (Node* cur = n; cur; cur = g.node(g.parent(cur->id())))
            if (cur->type().isA(kCrowdType))
                return cur->id();
    }
    if (objects.empty())
        for (NodeId id : sim::simulations(g))
            if (g.node(id)->type().isA(kCrowdType))
            {
                std::vector<NodeId> all;
                for (NodeId c : sim::simulations(g))
                    if (g.node(c)->type().isA(kCrowdType))
                        all.push_back(c);
                if (all.size() == 1)
                    return id;
                break;
            }
    throw CommandError(std::string(cmd) + ": select a crowd or name it");
}

namespace
{
json groupsOf(const Node& n)
{
    json groups = json::parse(text(n, "groups").empty() ? "[]" : text(n, "groups"), nullptr, false);
    return groups.is_array() ? groups : json::array();
}

void setText(Graph& g, NodeId id, const char* a, const std::string& v) { g.setValue({id, attr(*g.node(id), a), -1}, v); }

std::string joinList(const std::vector<std::string>& v)
{
    std::string s;
    for (const std::string& x : v)
        s += (s.empty() ? "" : ", ") + x;
    return s;
}

/** Node path args of a list flag. */
std::vector<std::string> nodeList(Graph& g, const Args& a, const char* f, const char* cmd)
{
    std::vector<std::string> out;
    if (!a.has(f))
        return out;
    const json& v = a.flags[f];
    for (const json& x : v.is_array() ? v : json::array({v}))
    {
        Node* n = named(g, x.get<std::string>());
        if (!n)
            throw CommandError(std::string(cmd) + ": no node named '" + x.get<std::string>() + "'");
        out.push_back(n->path());
    }
    return out;
}

json spawnGroup(Graph& g, NodeId crowd, const Args& a, const char* cmd)
{
    const Node& cn = *g.node(crowd);
    json groups = groupsOf(cn);
    const Behaviour b = [&] {
        try
        {
            return behaviourFrom(a.get<std::string>("behaviour", "wander"));
        }
        catch (const std::exception& e)
        {
            throw CommandError(std::string(cmd) + ": " + e.what());
        }
    }();
    std::string name = a.get<std::string>("name", "");
    if (name.empty())
        for (int k = 1;; ++k)
        {
            name = std::string(behaviourName(b)) + std::to_string(k);
            if (std::none_of(groups.begin(), groups.end(), [&](const json& x) { return x.value("name", "") == name; }))
                break;
        }
    const int64_t seed = a.get<int64_t>("seed", int64_t(groups.size() + 1));
    Rand r{mixHash(uint64_t(seed) * 7919ull + 17)};
    const std::vector<std::string> chars = splitList(text(cn, "characters"));
    const double sMin = a.get<double>("speedMin", 0.85), sMax = std::max(sMin, a.get<double>("speedMax", 1.15));
    const double gap = a.get<double>("spacing", 90.0);
    const double radius = a.get<double>("radius", 1000.0);
    glm::dvec2 center(0.0);
    if (a.has("center"))
        center = {a.flags["center"][0].get<double>(), a.flags["center"][2].get<double>()};

    json gj = {{"name", name}, {"behaviour", behaviourName(b)}, {"style", a.get<std::string>("style", b == Behaviour::Flee || b == Behaviour::Chase ? "run" : "walk")},
               {"center", {center.x, center.y}}, {"radius", radius}, {"pauseMin", a.get<double>("pauseMin", 1.0)}, {"pauseMax", a.get<double>("pauseMax", 4.0)},
               {"spacing", gap}, {"serve", a.get<double>("serve", 6.0)}, {"stay", a.get<double>("stay", 0.0)}, {"range", a.get<double>("range", 800.0)},
               {"loop", a.get<bool>("loop", false)}};
    for (const char* key : {"goal", "threat", "target"})
        if (a.has(key))
        {
            const json& v = a.flags[key];
            if (v.is_string())
            {
                Node* n = named(g, v.get<std::string>());
                if (!n)
                    throw CommandError(std::string(cmd) + ": no node named '" + v.get<std::string>() + "'");
                gj[key] = n->path();
            }
            else
                gj[key] = v;
        }
    if (a.has("then"))
        gj["then"] = a.get<std::string>("then");
    if (a.has("prey"))
        gj["prey"] = a.get<std::string>("prey");
    std::vector<glm::vec2> path;
    if (a.has("path"))
    {
        Node* p = named(g, a.get<std::string>("path"));
        if (!p || (path = curvePoints(g, *p)).size() < 2)
            throw CommandError(std::string(cmd) + ": path must be a curve");
        gj["path"] = p->path();
    }
    std::vector<std::string> spots = nodeList(g, a, "spots", cmd);
    if (!spots.empty())
        gj["spots"] = spots;
    if (b == Behaviour::Spot && spots.empty())
        throw CommandError(std::string(cmd) + ": spot needs spots (motionSpot nodes)");
    if ((b == Behaviour::FollowPath || b == Behaviour::Queue) && path.size() < 2)
        throw CommandError(std::string(cmd) + ": " + behaviourName(b) + " needs a path curve");
    if ((b == Behaviour::GoTo && !gj.contains("goal")) || (b == Behaviour::Flee && !gj.contains("threat")) ||
        (b == Behaviour::Chase && !gj.contains("target") && !gj.contains("prey")))
        throw CommandError(std::string(cmd) + ": " + behaviourName(b) + " needs " + (b == Behaviour::GoTo ? "a goal" : b == Behaviour::Flee ? "a threat" : "a target or prey"));
    const bool onNetwork = a.get<bool>("onNetwork", false);
    gj["network"] = onNetwork;

    int count = int(a.get<int64_t>("count", b == Behaviour::Spot ? int64_t(spots.size()) : 20));
    count = std::clamp(count, 1, 5000);
    std::vector<Spawned> placed;
    Node* source = a.has("source") ? named(g, a.get<std::string>("source")) : nullptr;
    if (a.has("source") && !source)
        throw CommandError(std::string(cmd) + ": no node named '" + a.get<std::string>("source") + "'");
    auto randomHeading = [&] { return r.next() * 360.0 - 180.0; };

    if (b == Behaviour::Idle)
    {
        // A ring facing its centre.
        const double ring = std::max(60.0, double(count) * gap / (2.0 * kPiD));
        for (int i = 0; i < count; ++i)
        {
            const double ang = 2.0 * kPiD * double(i) / double(count) + (r.next() - 0.5) * 0.3;
            const glm::dvec2 p = center + glm::dvec2(std::sin(ang), std::cos(ang)) * ring * (0.9 + 0.2 * r.next());
            const glm::dvec2 in = center - p;
            placed.push_back({p, std::atan2(in.x, in.y) * 180.0 / kPiD});
        }
    }
    else if (b == Behaviour::Queue && !source)
    {
        // Lined up from the head, the path's end.
        std::vector<glm::dvec2> cm;
        for (glm::vec2 p : path)
            cm.push_back(glm::dvec2(p) * kCm);
        std::vector<double> at{0.0};
        for (size_t k = 1; k < cm.size(); ++k)
            at.push_back(at.back() + glm::length(cm[k] - cm[k - 1]));
        for (int i = 0; i < count; ++i)
        {
            const double s = std::max(0.0, at.back() - double(i) * gap);
            size_t k = 1;
            while (k + 1 < at.size() && at[k] < s)
                ++k;
            const double t = (s - at[k - 1]) / std::max(at[k] - at[k - 1], 1e-9);
            const glm::dvec2 p = glm::mix(cm[k - 1], cm[k], t);
            const glm::dvec2 d = cm[k] - cm[k - 1];
            placed.push_back({p, std::atan2(d.x, d.y) * 180.0 / kPiD});
        }
    }
    else if (source && !curvePoints(g, *source).empty())
    {
        // Along a curve, spread over its length with a little sideways jitter.
        std::vector<glm::vec2> pts = curvePoints(g, *source);
        std::vector<double> at{0.0};
        for (size_t k = 1; k < pts.size(); ++k)
            at.push_back(at.back() + glm::length(pts[k] - pts[k - 1]) * kCm);
        for (int i = 0; i < count; ++i)
        {
            const double s = at.back() * (double(i) + 0.5 + (r.next() - 0.5) * 0.6) / double(count);
            size_t k = 1;
            while (k + 1 < at.size() && at[k] < s)
                ++k;
            const double t = (s - at[k - 1]) / std::max(at[k] - at[k - 1], 1e-9);
            const glm::dvec2 p = glm::mix(glm::dvec2(pts[k - 1]), glm::dvec2(pts[k]), t) * kCm;
            const glm::dvec2 d = glm::normalize(glm::dvec2(pts[k] - pts[k - 1]) + glm::dvec2(1e-9, 0.0));
            const glm::dvec2 side(-d.y, d.x);
            placed.push_back({p + side * ((r.next() - 0.5) * 120.0), std::atan2(d.x, d.y) * 180.0 / kPiD, s});
        }
    }
    else if (source && meshOf(*source))
    {
        // On a surface: area-weighted over its upward-facing faces.
        const MeshPtr m = meshOf(*source);
        const glm::dmat4 w = worldOf(*source);
        std::vector<std::array<glm::dvec3, 3>> tris;
        std::vector<double> cum;
        double total = 0.0;
        for (geo::Index f = 0; f < m->faceCapacity(); ++f)
        {
            if (!m->faceValid(f))
                continue;
            std::vector<glm::dvec3> poly;
            for (geo::Index p : m->facePoints(f))
                poly.push_back(glm::dvec3(w * glm::dvec4(m->position(p), 1.0)));
            for (size_t k = 1; k + 1 < poly.size(); ++k)
            {
                const glm::dvec3 nrm = glm::cross(poly[k] - poly[0], poly[k + 1] - poly[0]);
                const double area = 0.5 * glm::length(nrm);
                if (area < 1e-9 || std::abs(nrm.y) / (2.0 * area) < 0.7)
                    continue;
                tris.push_back({poly[0], poly[k], poly[k + 1]});
                total += area;
                cum.push_back(total);
            }
        }
        if (tris.empty())
            throw CommandError(std::string(cmd) + ": " + source->path() + " has no ground to stand on (faces facing up)");
        placed = scatter(count, gap, [&] {
            const double u = r.next() * total;
            const size_t k = size_t(std::lower_bound(cum.begin(), cum.end(), u) - cum.begin());
            const auto& t = tris[std::min(k, tris.size() - 1)];
            double a1 = r.next(), a2 = r.next();
            if (a1 + a2 > 1.0)
                a1 = 1.0 - a1, a2 = 1.0 - a2;
            const glm::dvec3 p = t[0] + (t[1] - t[0]) * a1 + (t[2] - t[0]) * a2;
            return Spawned{{p.x, p.z}, randomHeading()};
        });
    }
    else if (onNetwork || (source && !source->type().isA("transform")))
    {
        std::shared_ptr<const CrowdView> v = viewOf(g, crowd);
        if (!v || v->runtime->setup().network.empty())
            throw CommandError(std::string(cmd) + ": the crowd has no network; set its network to a settlement or curves first");
        const Network& net = v->runtime->setup().network;
        placed = scatter(count, gap, [&] {
            int lane = 0;
            const glm::vec2 p = net.pointAt(r.next(), &lane);
            const float off = (float(r.next()) - 0.5f) * net.lanes[size_t(lane)].width * 0.6f;
            const auto [l, s] = net.nearest(p);
            const auto& pts = net.lanes[size_t(l)].points;
            glm::vec2 d = pts.back() - pts.front();
            d = glm::length(d) > 1e-6f ? glm::normalize(d) : glm::vec2(0, 1);
            const glm::vec2 q = p + glm::vec2(-d.y, d.x) * off;
            return Spawned{glm::dvec2(q) * kCm, randomHeading()};
        });
        gj["network"] = true;
    }
    else
    {
        if (source)
            center = {worldOf(*source)[3].x, worldOf(*source)[3].z};
        gj["center"] = {center.x, center.y};
        placed = scatter(count, gap, [&] {
            const double rr = radius * std::sqrt(r.next()), ang = 2.0 * kPiD * r.next();
            return Spawned{center + glm::dvec2(std::cos(ang), std::sin(ang)) * rr, randomHeading()};
        });
    }

    const double appearOver = a.get<double>("appearOver", 0.0);
    std::vector<double> weights;
    if (a.has("characterWeights"))
        for (const json& x : a.flags["characterWeights"])
            weights.push_back(std::max(0.0, x.get<double>()));
    weights.resize(std::max<size_t>(chars.size(), 1), weights.empty() ? 1.0 : 0.0);
    double wsum = 0.0;
    for (double x : weights)
        wsum += x;
    json agents = json::array();
    for (size_t i = 0; i < placed.size(); ++i)
    {
        double pick = r.next() * std::max(wsum, 1e-9);
        int character = 0;
        for (size_t c = 0; c < weights.size(); ++c)
            if ((pick -= weights[c]) <= 0.0)
            {
                character = int(c);
                break;
            }
        if (a.has("character"))
            character = int(a.get<int64_t>("character"));
        json ag = {{"p", {std::round(placed[i].p.x * 10.0) / 10.0, std::round(placed[i].p.y * 10.0) / 10.0}},
                   {"h", std::round(placed[i].h * 10.0) / 10.0},
                   {"seed", uint32_t(mixHash(uint64_t(seed) * 1000003ull + i) & 0x7fffffff)},
                   {"character", character},
                   {"speed", std::round((sMin + (sMax - sMin) * r.next()) * 1000.0) / 1000.0},
                   {"slot", int(i)}};
        if (placed[i].along > 0.0)
            ag["along"] = placed[i].along;
        if (appearOver > 0.0)
            ag["appear"] = std::round(appearOver * double(i) / double(placed.size()) * 100.0) / 100.0;
        agents.push_back(ag);
    }
    gj["agents"] = agents;
    bool replaced = false;
    for (json& x : groups)
        if (x.value("name", "") == name)
            x = gj, replaced = true;
    if (!replaced)
        groups.push_back(gj);
    setText(g, crowd, "groups", groups.dump());
    return {{"crowd", g.path(crowd)}, {"group", name}, {"agents", int(placed.size())}, {"replaced", replaced}};
}

/**
 * The crowd's groups hold world-space data (centres, areas, agent starts and headings, point goals), rewritten by the
 * move; everything they name by node follows its node. Crowds stand on the ground, so only moves that keep +Y up can
 * carry them.
 */
relocate::Edit relocateCrowd(const relocate::Move& move, NodeId node)
{
    Graph& g = *move.graph;
    const Node& n = *g.node(node);
    const glm::dvec3 up = move.direction({0, 1, 0});
    if (up.y < 1.0 - 1e-6)
        throw CommandError(n.path() + ": a crowd stands on the ground, so it can only follow moves that keep +Y up");
    const glm::dvec3 fwd = move.direction({0, 0, 1});
    const double yaw = std::atan2(fwd.x, fwd.z) * 180.0 / kPiD;
    json groups = groupsOf(n);
    if (groups.empty())
        return {};
    auto point2 = [&](json& p) {
        const glm::dvec3 q = move.point({p[0].get<double>(), 0.0, p[1].get<double>()});
        p = json::array({q.x, q.z});
    };
    for (json& gr : groups)
    {
        if (gr.contains("center"))
            point2(gr["center"]);
        for (json& p : gr["area"].is_array() ? gr["area"] : gr["area"] = json::array())
            point2(p);
        if (gr["area"].empty())
            gr.erase("area");
        for (const char* key : {"goal", "threat", "target"})
            if (gr.contains(key) && gr[key].is_array() && gr[key].size() >= 3)
            {
                const glm::dvec3 q = move.point({gr[key][0].get<double>(), gr[key][1].get<double>(), gr[key][2].get<double>()});
                gr[key] = json::array({q.x, q.y, q.z});
            }
        gr["yaw"] = gr.value("yaw", 0.0) - yaw;
        for (json& a : gr["agents"])
        {
            point2(a["p"]);
            a["h"] = a.value("h", 0.0) + yaw;
        }
    }
    const std::string text = groups.dump();
    return [&g, node, text] { setText(g, node, "groups", text); };
}

json infoOf(Graph& g, NodeId crowd)
{
    std::shared_ptr<const CrowdView> v = viewOf(g, crowd);
    if (!v)
    {
        const sim::Info i = sim::info(g, crowd);
        throw CommandError("crowdInfo: " + (i.error.empty() ? std::string("the crowd could not be set up") : i.error));
    }
    const sim::Info i = sim::info(g, crowd);
    const Stats& s = v->runtime->stats();
    json groups = json::array();
    for (const json& gr : v->groups)
        groups.push_back({{"name", gr.value("name", "")}, {"behaviour", gr.value("behaviour", "")}, {"style", gr.value("style", "")},
                          {"agents", gr.value("agents", json::array()).size()}});
    const Setup& su = v->runtime->setup();
    return {{"agents", su.agents.size()},
            {"groups", groups},
            {"characters", v->characters},
            {"device", s.device},
            {"status", v->status},
            {"generations", s.generations},
            {"batches", s.batches},
            {"fullBatches", s.fullBatches},
            {"meanBatch", s.batches ? double(s.generations) / double(s.batches) : 0.0},
            {"inferenceMs", s.inferenceMs},
            {"msPerBatch", s.batches ? s.inferenceMs / double(s.batches) : 0.0},
            {"generatedAgents", s.generatedAgents},
            {"cycleAgents", s.cycleAgents},
            {"alive", s.alive},
            {"clips", v->runtime->clipCount()},
            {"networkLanes", su.network.lanes.size()},
            {"networkLengthM", su.network.length()},
            {"obstacles", su.obstacles.size()},
            {"cachedFrom", i.cachedFrom},
            {"cachedTo", i.cachedTo},
            {"error", i.error}};
}
} // namespace

bool registerScene(ext::Context& ctx)
{
    const NodeType* transform = types::find("transform");
    if (!transform)
        return false;
    NodeType t{.name = kCrowdType,
               .base = transform,
               .dag = true,
               .doc = "A crowd: agents in spawn groups with behaviours (wander, go to, follow a path, queue, idle groups, spots, flee, "
                      "chase), avoiding each other and obstacles, their motion generated in batches and cached per frame. Agents are "
                      "in world space; moving the node does not move them.",
               .icon = "users-group",
               .create = transform->create};
    t.ownAttrs = {
        boolean("enabled", true, "Simulate the crowd."),
        whole("startFrame", 1, "Frame the crowd starts from: every agent stands where it was spawned."),
        whole("seed", 1, "Seed of every random choice the agents make: the same seed gives the same crowd on every machine."),
        string("characters", "Characters the agents wear (root nodes of skinned characters), comma separated; spawn groups pick among them."),
        string("ground", "Mesh or terrain the agents stand on; empty stands them at height 0."),
        string("network", "Where agents walk when a group is on the network: Settlement nodes (sidewalks and crosswalks) and curves, comma separated."),
        string("obstacles", "Nodes whose footprints the agents walk around, comma separated."),
        string("focus", "Node the detail is measured from (a camera, a hero): agents near it generate their motion, the rest play cycles."),
        choice("detail", 0, {"auto", "generated", "cycles"},
               "auto: generated motion near the focus within the budget, gait cycles elsewhere; generated: every agent within the "
               "budget; cycles: no generation (fast previews)."),
        number("generatedRadius", 2500.0, "Distance from the focus within which agents generate their motion, cm.", 0.0),
        whole("generatedMax", 64, "Most agents generating motion at once; the nearest to the focus win.", 0.0, 5000.0),
        whole("batch", 32, "Requests per inference: on a GPU a batch costs about the same at any size, so larger is faster; the CPU runs one at a time.", 1.0, 64.0),
        number("lookahead", 0.15, "Seconds early an agent may ask for its next motion so requests share a batch.", 0.0, 1.0),
        boolean("avoidance", true, "Agents steer round each other and the obstacles."),
        number("timeHorizon", 2.0, "Seconds ahead agents look for each other when avoiding.", 0.2, 10.0),
        choice("quality", 0, {"auto", "full", "proxy", "points"},
               "Viewport drawing: full characters, a proxy skeleton, or a point per agent; auto draws the nearest in full."),
        whole("fullLimit", 64, "Auto quality: most agents drawn as full characters.", 0.0, 5000.0),
        boolean("showPaths", false, "Draw each agent's route and goal."),
        boolean("preview", true, "Draw the agents, simulating as the time changes."),
        string("groups", "The spawn groups and their agents, as JSON (crowdSpawn writes it).", AttrHidden),
        string(sim::kRecordingsAttr, "Layers recorded from this simulation.", AttrHidden),
    };
    if (!types::registerType(ctx, std::move(t)))
        return false;
    relocate::registerHandler(kCrowdType, {.plan = relocateCrowd});
    if (!sim::registerSolverType(ctx, {.nodeType = kCrowdType, .create = [](Graph&, NodeId node) -> std::unique_ptr<sim::Solver> {
                                           return std::make_unique<CrowdSolver>(node);
                                       }}))
        return false;

    using T = ArgType;
    ctx.registerCommand({
        .name = "crowdCreate",
        .doc = "Make a crowd: a node that holds spawn groups of agents and simulates them. Then add agents with crowdSpawn.",
        .flags = {
            {"name", "n", T::String, "crowd", "Name of the crowd node."},
            nodeFlag({"characters", "c", T::List, nullptr, "Characters the agents wear (skinned characters' root nodes)."}, "node"),
            nodeFlag({"ground", "g", T::String, nullptr, "Mesh or terrain the agents stand on."}, "mesh"),
            nodeFlag({"network", "nw", T::List, nullptr, "Settlements and curves agents can walk along."}, "node"),
            nodeFlag({"obstacles", "o", T::List, nullptr, "Nodes the agents walk around."}, "node"),
            nodeFlag({"focus", "f", T::String, nullptr, "Node detail is measured from (a camera or hero character)."}, "transform"),
            {"startFrame", "sf", T::Int, nullptr, "Frame the crowd starts from; default the playback start."},
        },
        .run = [](const Args& a) -> json {
            Graph& g = graph();
            const NodeId id = g.createNode(*types::find(kCrowdType), a.get<std::string>("name", "crowd"))->id();
            for (const char* f : {"network", "obstacles"})
                if (a.has(f))
                    setText(g, id, f, joinList(nodeList(g, a, f, "crowdCreate")));
            if (a.has("characters"))
                setCharacters(g, id, nodeList(g, a, "characters", "crowdCreate"));
            for (const char* f : {"ground", "focus"})
                if (a.has(f))
                {
                    const std::vector<std::string> one = nodeList(g, a, f, "crowdCreate");
                    setText(g, id, f, one.empty() ? "" : one.front());
                }
            const int64_t start = a.has("startFrame") ? a.get<int64_t>("startFrame") : sim::frameOf(anim::playback().minTime);
            g.setValue({id, attr(*g.node(id), "startFrame"), -1}, start);
            g.select({id});
            return g.path(id);
        },
        .examples = {"crowdCreate(characters=['Manny', 'Quinn'], ground='terrain1')"},
    });

    ctx.registerCommand({
        .name = "crowdSpawn",
        .doc = "Add a spawn group of agents to a crowd with a behaviour: wander an area or the network, go to a goal, follow a path, "
               "queue along a path, stand in idle groups, perform spots (sit, pick up), flee a threat or chase a target. Agents are "
               "placed in a disc, along a curve, on a mesh or terrain, or on the network's sidewalks and crosswalks. A group of the "
               "same name is replaced.",
        .positionalDoc = "the crowd (default the selected one)",
        .flags = {
            {"count", "n", T::Int, nullptr, "Agents in the group (default 20; spot: one per spot).", {}, {}, 1.0, 5000.0},
            {"behaviour", "b", T::String, "wander", "What they do.", {"wander", "goTo", "followPath", "queue", "idle", "spot", "flee", "chase", "stand"}},
            {"style", "st", T::String, nullptr, "Gait: walk, run, crouch, strafeLeft, strafeRight, backward (default walk; flee and chase run)."},
            nodeFlag({"source", "src", T::String, nullptr, "Where agents start: a curve (along it), a mesh or terrain (on it), or a node (around it)."}, "node"),
            {"center", "c", T::Vec3, nullptr, "Centre of the spawn disc, wander area or idle group (cm)."},
            {"radius", "r", T::Float, 1000.0, "Radius of the spawn disc and wander area (cm).", {}, {}, 0.0},
            {"onNetwork", "on", T::Bool, false, "Start on the crowd's network and walk along it (sidewalks, crosswalks, paths)."},
            {"goal", "gl", T::Any, nullptr, "goTo: a node to walk to (followed when animated) or a point [x, y, z] (cm)."},
            nodeFlag({"path", "p", T::String, nullptr, "followPath, queue: the curve; a queue's head is its end."}, "curve"),
            {"loop", "lp", T::Bool, false, "followPath: go round and round."},
            nodeFlag({"spots", "sp", T::List, nullptr, "spot: motionSpot nodes, one agent each in turn."}, "motionSpot"),
            {"stay", "sy", T::Float, 0.0, "spot: seconds to stay after the last target, then leave; 0 stays.", {}, {}, 0.0},
            {"threat", "th", T::Any, nullptr, "flee: the node (or point) to run from."},
            {"target", "tg", T::Any, nullptr, "chase: the node (or point) to run after."},
            {"prey", "py", T::String, nullptr, "chase: run after the nearest agent of this group instead."},
            {"range", "rg", T::Float, 800.0, "flee: distance at which the threat is noticed; chase: distance that counts as caught (cm).", {}, {}, 0.0},
            {"then", "tn", T::String, nullptr, "What goTo, queue and spot agents do afterwards (default stand; queue and spot wander).",
             {"stand", "wander", "goTo", "followPath", "idle", "flee", "chase"}},
            {"pauseMin", "pmn", T::Float, 1.0, "wander: shortest pause at a goal, seconds.", {}, {}, 0.0},
            {"pauseMax", "pmx", T::Float, 4.0, "wander: longest pause at a goal, seconds.", {}, {}, 0.0},
            {"spacing", "sc", T::Float, 90.0, "Gap between agents as spawned, and a queue's spacing (cm).", {}, {}, 20.0},
            {"serve", "sv", T::Float, 6.0, "queue: seconds the head waits before it leaves.", {}, {}, 0.5},
            {"speedMin", "smn", T::Float, 0.85, "Slowest agent, as a share of the gait's speed.", {}, {}, 0.2, 3.0},
            {"speedMax", "smx", T::Float, 1.15, "Fastest agent, as a share of the gait's speed.", {}, {}, 0.2, 3.0},
            {"character", "ch", T::Int, nullptr, "Every agent wears this character (index into the crowd's characters)."},
            {"characterWeights", "cw", T::List, nullptr, "Relative share of each of the crowd's characters."},
            {"appearOver", "ao", T::Float, 0.0, "Agents appear one after another over this many seconds.", {}, {}, 0.0},
            {"seed", "sd", T::Int, nullptr, "Seed of the placement and the agents' own seeds."},
            {"name", "nm", T::String, nullptr, "Group name; default the behaviour and a number."},
        },
        .run = [](const Args& a) -> json {
            Graph& g = graph();
            return spawnGroup(g, crowdArg(g, a.objects(), "crowdSpawn"), a, "crowdSpawn");
        },
        .examples = {"crowdSpawn('crowd', count=60, behaviour='wander', radius=2000)",
                     "crowdSpawn('crowd', count=200, behaviour='wander', onNetwork=True)",
                     "crowdSpawn('crowd', count=12, behaviour='queue', path='queueCurve', then='wander')",
                     "crowdSpawn('crowd', count=40, behaviour='flee', threat='monster', range=1200)"},
    });

    ctx.registerCommand({
        .name = "crowdGroups",
        .doc = "List a crowd's spawn groups, or remove one.",
        .positionalDoc = "the crowd",
        .flags = {{"remove", "rm", T::String, nullptr, "Name of the group to remove."}},
        .run = [](const Args& a) -> json {
            Graph& g = graph();
            const NodeId c = crowdArg(g, a.objects(), "crowdGroups");
            json groups = groupsOf(*g.node(c));
            if (a.has("remove"))
            {
                const std::string name = a.get<std::string>("remove");
                const size_t before = groups.size();
                groups.erase(std::remove_if(groups.begin(), groups.end(), [&](const json& x) { return x.value("name", "") == name; }), groups.end());
                if (groups.size() == before)
                    throw CommandError("crowdGroups: no group named '" + name + "'");
                setText(g, c, "groups", groups.dump());
            }
            json out = json::array();
            for (const json& x : groups)
            {
                json y = x;
                y["agents"] = x.value("agents", json::array()).size();
                out.push_back(y);
            }
            return out;
        },
    });

    ctx.registerCommand({
        .name = "crowdSimulate",
        .doc = "Simulate a crowd up to a frame (default the playback end) so it plays back and scrubs from the cache. Reports the "
               "inference device, generations, batches and how full they were, and how many agents generated their motion.",
        .positionalDoc = "the crowd",
        .flags = {{"frame", "f", T::Int, nullptr, "Last frame; default the playback end."}},
        .undoable = false,
        .run = [](const Args& a) -> json {
            Graph& g = graph();
            const NodeId c = crowdArg(g, a.objects(), "crowdSimulate");
            const int64_t end = a.has("frame") ? a.get<int64_t>("frame") : sim::frameOf(anim::playback().maxTime);
            const auto t0 = std::chrono::steady_clock::now();
            try
            {
                sim::simulate(g, c, end, sim::LoopMode::Off);
            }
            catch (const std::exception& e)
            {
                throw CommandError(std::string("crowdSimulate: ") + e.what());
            }
            json out = infoOf(g, c);
            out["seconds"] = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
            return out;
        },
        .examples = {"r = crowdSimulate('crowd')\nr['device'], r['generations'], r['meanBatch']"},
    });

    ctx.registerCommand({
        .name = "crowdInfo",
        .doc = "What a crowd holds and how its last simulation went: agents and groups, the inference device (or why agents play "
               "cycles), generations, batches, cached frames.",
        .positionalDoc = "the crowd",
        .undoable = false,
        .run = [](const Args& a) -> json {
            Graph& g = graph();
            return infoOf(g, crowdArg(g, a.objects(), "crowdInfo"));
        },
    });

    ctx.registerCommand({
        .name = "crowdAgents",
        .doc = "Where every agent of a crowd is at a frame (simulating up to it): position (cm), heading (degrees), what it is "
               "doing, and whether its motion is generated or a cycle.",
        .positionalDoc = "the crowd",
        .flags = {{"frame", "f", T::Int, nullptr, "Frame; default the current one."}},
        .undoable = false,
        .run = [](const Args& a) -> json {
            Graph& g = graph();
            const NodeId c = crowdArg(g, a.objects(), "crowdAgents");
            const int64_t f = a.has("frame") ? a.get<int64_t>("frame") : sim::frameOf(g.time());
            std::shared_ptr<const sim::Frame> fr;
            try
            {
                fr = sim::simulate(g, c, f, sim::LoopMode::Off);
            }
            catch (const std::exception& e)
            {
                throw CommandError(std::string("crowdAgents: ") + e.what());
            }
            std::shared_ptr<const CrowdView> v = viewOf(g, c);
            json out = json::array();
            for (size_t i = 0; v && i < v->runtime->setup().agents.size(); ++i)
            {
                const AgentState& s = v->runtime->agent(fr->state, i);
                const glm::dmat4 m = agentMatrix(*v, fr->state, i);
                const MotionKind k = MotionKind(int(s.motion));
                out.push_back({{"agent", i},
                               {"alive", s.alive > 0.5},
                               {"position", {m[3].x, m[3].y, m[3].z}},
                               {"heading", std::atan2(s.hx, s.hz) * 180.0 / kPiD},
                               {"speed", std::hypot(s.vx, s.vz) * kCm},
                               {"doing", v->runtime->label(fr->state, i)},
                               {"motion", k == MotionKind::Cycle ? "cycle" : k == MotionKind::Hold ? "hold" : "generated"},
                               {"group", v->runtime->setup().agents[i].group},
                               {"character", v->runtime->setup().agents[i].character}});
            }
            return out;
        },
    });
    return true;
}
} // namespace crowds

LILA_EXTENSION_EXPORT int lilaExtensionInit(lila::ext::Context& ctx)
{
    if (!crowds::registerScene(ctx))
        return 1;
    crowds::registerBake(ctx);
    crowds::registerUi(ctx);
    return 0;
}
