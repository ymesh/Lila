// Commands for the landform stage: terrainLandform adds erosion and shaping nodes to a terrain's group, terrainErode's
// preset path, terrainStroke (terrainSculpt by brush name), terrainSculptRegion (a patch sculpted as a mesh and baked
// back) and terrainStampLibrary.

#include "Brushes.h"
#include "LandformCommands.h"

#include <lila/Brush.h>
#include <lila/Command.h>
#include <lila/Deformer.h>
#include <lila/Extension.h>
#include <lila/GeometryNodes.h>
#include <lila/GpuCompute.h>
#include <lila/Graph.h>
#include <lila/ImageIO.h>
#include <lila/Log.h>
#include <lila/Math.h>
#include <lila/Mesh.h>
#include <lila/Node.h>
#include <lila/Profile.h>
#include <lila/Sculpt.h>
#include <lila/Transform.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <filesystem>
#include <limits>
#include <set>

using namespace lila;

namespace terrain
{
namespace lcmd_
{
using json = nlohmann::json;
using scene::Graph;
using scene::Node;
using T = ArgType;

bool upZ() { return math::upAxis() == math::UpAxis::Z; }
glm::dvec3 fromGround2(glm::dvec2 g, double up = 0.0) { return upZ() ? glm::dvec3(g.x, g.y, up) : glm::dvec3(g.x, up, g.y); }
glm::dvec2 groundOf2(const glm::dvec3& w) { return upZ() ? glm::dvec2(w.x, w.y) : glm::dvec2(w.x, w.z); }
double upOf2(const glm::dvec3& w) { return upZ() ? w.z : w.y; }

glm::dvec3 pointArg(const json& p, const std::string& cmd)
{
    if (!p.is_array() || (p.size() != 2 && p.size() != 3))
        throw CommandError(cmd + ": points are [x, " + (upZ() ? "y" : "z") + "] or [x, y, z]");
    return p.size() == 2 ? fromGround2({p[0].get<double>(), p[1].get<double>()}) : glm::dvec3(p[0].get<double>(), p[1].get<double>(), p[2].get<double>());
}

/** A terrain: its mesh, the modifier making the heightfield, the heightfield in its own space and its placement. */
struct Target
{
    Node* mesh = nullptr;
    Node* modifier = nullptr;
    std::shared_ptr<const Heightfield> h;
    glm::dmat4 world{1.0};
    glm::dmat4 inv{1.0};

    std::string path() const { return mesh ? mesh->path() : modifier->path(); }
    glm::dvec2 local(const glm::dvec3& w) const
    {
        const glm::dvec4 q = inv * glm::dvec4(w, 1.0);
        return {q.x, q.z};
    }
    double across() const { return 0.5 * (glm::length(glm::dvec3(world[0])) + glm::length(glm::dvec3(world[2]))); }
};

Target findTarget(const std::string& name, const std::string& cmd)
{
    Graph& g = Graph::current();
    Node* n = g.find(name);
    if (!n)
        throw CommandError(cmd + ": no node named '" + name + "'");
    g.evaluate();
    Target t;
    if (gn::isGnModifier(*n))
    {
        t.modifier = n;
        t.h = gn::modifierHeightfield(n->id());
        const scene::NodeId meshId = scene::modifiers::meshOf(g, n->id());
        t.mesh = meshId != scene::kNoNode ? g.node(meshId) : nullptr;
    }
    else
    {
        t.mesh = n;
        const std::vector<scene::NodeId> stack = scene::modifiers::stack(g, n->id());
        for (auto it = stack.rbegin(); it != stack.rend() && !t.h; ++it)
            if (Node* m = g.node(*it); m && gn::isGnModifier(*m))
                if (auto hf = gn::modifierHeightfield(m->id()))
                {
                    t.modifier = m;
                    t.h = std::move(hf);
                }
    }
    if (!t.h || t.h->empty())
        throw CommandError(cmd + ": " + n->path() + " makes no heightfield (make a terrain with terrainCreate)");
    t.world = t.mesh ? t.mesh->as<glm::dmat4>(scene::Transform::kWorldMatrix) : glm::dmat4(1.0);
    t.inv = glm::inverse(t.world);
    return t;
}

json infoOf(const std::string& path) { return ops::execute("terrainInfo", {{"args", {path}}}); }

std::string nodeIdentifier(const std::string& s)
{
    std::string out;
    for (char c : s)
        out += std::isalnum(static_cast<unsigned char>(c)) ? c : '_';
    if (out.empty() || !std::isalpha(static_cast<unsigned char>(out[0])))
        out = "Node" + out;
    return out;
}

struct GroupEdit
{
    Node* node = nullptr;
    gn::Group group;

    gn::GNode* find(const std::string& name)
    {
        for (gn::GNode& n : group.nodes)
            if (n.name == name)
                return &n;
        return nullptr;
    }
    gn::GNode* findType(const std::string& type)
    {
        for (gn::GNode& n : group.nodes)
            if (n.type == type)
                return &n;
        return nullptr;
    }
    gn::GLink* linkInto(const std::string& toNode, const std::string& socket)
    {
        for (gn::GLink& l : group.links)
            if (l.toNode == toNode && l.toSocket == socket)
                return &l;
        return nullptr;
    }

    /** A new node spliced into the link l (which then comes from it), placed between the two ends. */
    gn::GNode& splice(gn::GLink& l, const std::string& type, const std::string& name)
    {
        const gn::GNode* to = find(l.toNode);
        const gn::GNode* from = find(l.fromNode);
        const glm::dvec2 at = to && from ? (to->position + from->position) * 0.5 + glm::dvec2(0.0, 160.0) : glm::dvec2(0.0);
        const std::string fromNode = l.fromNode, fromSocket = l.fromSocket;
        l.fromNode = name;
        l.fromSocket = "heightfield";
        group.links.push_back({fromNode, fromSocket, name, "heightfield"});
        gn::GNode n;
        n.name = name;
        n.type = type;
        n.position = at;
        group.nodes.push_back(std::move(n));
        return group.nodes.back();
    }

    /** Spliced into the heightfield chain before the trailing run of nodes whose types are in skip. */
    gn::GNode& insertBeforeTrailing(const std::string& type, const std::string& name, const std::set<std::string>& skip, const std::string& cmd)
    {
        std::string to = "GroupOutput", socket = "geometry";
        for (gn::GLink* l = linkInto(to, socket); l; l = linkInto(to, socket))
        {
            const gn::GNode* from = find(l->fromNode);
            if (!from || !skip.count(from->type))
                break;
            to = from->name;
            socket = "heightfield";
        }
        gn::GLink* l = linkInto(to, socket);
        if (!l)
            throw CommandError(cmd + ": the terrain's group has no heightfield running into " + to);
        return splice(*l, type, name);
    }

    /** Spliced in right after node (every link from its heightfield output then comes from the new node). */
    gn::GNode& insertAfter(const std::string& after, const std::string& type, const std::string& name, const std::string& cmd)
    {
        std::vector<gn::GLink*> outs;
        for (gn::GLink& l : group.links)
            if (l.fromNode == after && (l.fromSocket == "heightfield" || l.fromSocket == "geometry"))
                outs.push_back(&l);
        if (outs.empty())
            throw CommandError(cmd + ": nothing follows " + after + " in the terrain's group");
        gn::GNode& n = splice(*outs.front(), type, name);
        for (size_t k = 1; k < outs.size(); ++k)
        {
            outs[k]->fromNode = name;
            outs[k]->fromSocket = "heightfield";
        }
        return n;
    }

    void remove(const std::string& name)
    {
        gn::GLink* in = linkInto(name, "heightfield");
        const std::string fromNode = in ? in->fromNode : std::string(), fromSocket = in ? in->fromSocket : std::string();
        for (gn::GLink& l : group.links)
            if (l.fromNode == name)
            {
                l.fromNode = fromNode;
                l.fromSocket = fromSocket;
            }
        std::erase_if(group.links, [&](const gn::GLink& l) { return l.toNode == name || l.fromNode.empty(); });
        std::erase_if(group.nodes, [&](const gn::GNode& n) { return n.name == name; });
    }
};

GroupEdit groupOf(const Target& t, const std::string& cmd)
{
    Node* node = t.modifier ? gn::groupOfModifier(Graph::current(), *t.modifier) : nullptr;
    if (!node)
        throw CommandError(cmd + ": " + t.path() + " has no node group");
    try
    {
        return {node, gn::readGroup(*node)};
    }
    catch (const std::exception& e)
    {
        throw CommandError(cmd + ": " + e.what());
    }
}

void commit(GroupEdit& e, const std::string& cmd)
{
    try
    {
        gn::commitGroup(Graph::current(), *e.node, std::move(e.group));
    }
    catch (const std::exception& ex)
    {
        throw CommandError(cmd + ": " + ex.what());
    }
}

/** Evaluates the terrain and returns how long it took, ms. */
double evaluateMs()
{
    const auto t0 = std::chrono::steady_clock::now();
    Graph::current().evaluate();
    return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
}

struct Process
{
    const char* name;
    const char* type;
    const char* node;
};

constexpr Process kProcesses[] = {
    {"analytic", "TerrainAnalyticErosion", "Analytic"}, {"amplify", "TerrainAmplify", "Amplify"},     {"debris", "TerrainDebrisFlow", "Debris"},
    {"thermal", "ThermalErosion", "Thermal"},           {"hydraulic", "TerrainHydraulic", "Hydraulic"}, {"deposition", "TerrainDeposition", "Deposition"},
    {"snow", "TerrainSnow", "Snow"},                     {"filter", "TerrainErosionFilter", "Filter"},  {"terrace", "TerrainTerrace", "Terrace"},
    {"strata", "TerrainStrata", "Strata"},               {"fold", "TerrainFold", "Fold"},               {"warp", "TerrainWarp", "Warp"},
    {"ridgeSharpen", "TerrainRidgeSharpen", "RidgeSharpen"}, {"cliff", "TerrainCliff", "Cliff"},       {"preset", "TerrainErodePreset", "Preset"},
};

std::vector<std::string> processNames()
{
    std::vector<std::string> v;
    for (const Process& p : kProcesses)
        v.push_back(p.name);
    return v;
}

const Process* processNamed(const std::string& name)
{
    for (const Process& p : kProcesses)
        if (name == p.name)
            return &p;
    return nullptr;
}

/** Layers that sit after the landform steps: stamps and the final sculpt. */
const std::set<std::string>& trailingTypes()
{
    static const std::set<std::string> s = {"TerrainStamps", "TerrainSculpt"};
    return s;
}

void setValues(gn::GNode& n, const json& settings, const std::string& cmd)
{
    if (settings.is_null())
        return;
    if (!settings.is_object())
        throw CommandError(cmd + ": settings is a dict of the node's inputs");
    const gn::NodeDecl* d = gn::findDecl(n.type);
    for (const auto& [key, value] : settings.items())
    {
        const bool input = d && d->sockets.inputIndex(key) >= 0 && key != "heightfield";
        const bool prop = d && d->prop(key);
        if (!input && !prop)
        {
            std::string valid;
            if (d)
            {
                for (const gn::SocketDecl& s : d->sockets.inputs)
                    if (s.id != "heightfield")
                        valid += (valid.empty() ? "" : ", ") + s.id;
                for (const gn::PropDecl& p : d->props)
                    valid += (valid.empty() ? "" : ", ") + p.id;
            }
            throw CommandError(cmd + ": " + n.type + " has no input '" + key + "' (" + valid + ")");
        }
        n.values[key] = value;
    }
}

void checkDevice(const std::string& device, const std::string& cmd)
{
    if (device == "gpu" && !gpu::available())
        throw CommandError(cmd + ": no GPU here (it needs Lila's renderer running); use device='cpu' or 'auto'");
}

/** The mesh name a terrain's Sculpt Region proxy takes. */
std::string regionName(const Target& t) { return (t.mesh ? t.mesh->name() : std::string("Terrain")) + "_region"; }

constexpr const char* kRegionAttr = "terrainRegion";

json regionInfo(Node& proxy)
{
    if (proxy.type().attrIndex(kRegionAttr) < 0)
        return nullptr;
    const json v = ops::execute("getAttr", {{"args", {proxy.path() + "." + kRegionAttr}}});
    if (!v.is_string())
        return nullptr;
    json j = json::parse(v.get<std::string>(), nullptr, false);
    return j.is_object() ? j : json();
}

std::vector<FlagDef> strokeFlags()
{
    std::vector<FlagDef> flags;
    if (const CommandDef* d = ops::find("terrainSculpt"))
        for (FlagDef f : d->flags)
        {
            if (f.name == "op")
            {
                f.name = "brush";
                f.shortName = "b";
            }
            flags.push_back(std::move(f));
        }
    return flags;
}

/**
 * After an uplift stroke: the terrain's Erode Terrain lifts by the painted mask, with some uplift and erosion steps when
 * they are off, so erosion follows the painting live. Says what it set, or why nothing lifts.
 */
json followUplift(const std::string& terrain, const std::string& mask)
{
    const std::string cmd = "terrainStroke";
    Target t = findTarget(terrain, cmd);
    GroupEdit e = groupOf(t, cmd);
    gn::GNode* erode = e.findType("ErodeTerrain");
    json out = {{"upliftMask", mask}};
    if (!erode)
    {
        out["note"] = "the terrain has no Erode Terrain node, so nothing lifts by the painted mask; add one (terrainCreate makes it)";
        log::warn("{}: {}", cmd, out["note"].get<std::string>());
        return out;
    }
    if (erode->values.value("upliftMask", std::string()) != mask)
    {
        erode->values["upliftMask"] = mask;
        commit(e, cmd);
        t = findTarget(terrain, cmd);
    }
    const std::string mod = t.modifier->path();
    auto attr = [&](const char* name) { return t.modifier->type().attrIndex(name); };
    if (attr("uplift") >= 0 && t.modifier->as<double>(attr("uplift")) <= 0.0)
    {
        ops::execute("setAttr", {{"args", {mod + ".uplift", 20.0}}});
        out["upliftSet"] = 20.0;
    }
    if (attr("erosion") >= 0 && t.modifier->as<int64_t>(attr("erosion")) <= 0)
    {
        ops::execute("setAttr", {{"args", {mod + ".erosion", 60}}});
        out["erosionSet"] = 60;
    }
    return out;
}

/** The Sculpt Region bake: the proxy's surface against the terrain under it, as a patch stroke, plus what overhangs. */
json finishRegion(Node& proxy, const json& info, bool keep, bool cancel, bool keepOverhangs)
{
    LILA_ZONE();

    Graph& g = Graph::current();
    const std::string cmd = "terrainSculptRegion";
    const std::string terrain = info.value("terrain", std::string());
    const std::string proxyPath = proxy.path();
    // The proxy's sculpt session closes first, so its last strokes are in the mesh.
    if (auto s = sculpt::active(); s && s->node == proxy.id() && ops::find("sculptExit"))
    {
        // The bake reads the proxy's grid, which is its level 0.
        if (!cancel && s->level > 0)
            ops::execute("sculptLevel", {{"level", 0}});
        ops::execute(cancel ? "sculptCancel" : "sculptExit", json::object());
    }
    const Target t = findTarget(terrain, cmd);
    const int x0 = info.value("x0", 0), z0 = info.value("z0", 0), step = std::max(1, info.value("step", 1));
    const int nx = info.value("nx", 0), nz = info.value("nz", 0);
    json out = json::object();
    int64_t changed = 0;
    double most = 0.0;
    json overhangs = json::array();
    json overhangVolume;
    if (!cancel)
    {
        g.evaluate();
        Node* p = g.find(proxyPath);
        scene::MeshPtr meshPtr = p ? p->as<scene::MeshPtr>(scene::MeshNode::kOutMesh) : nullptr;
        if (!meshPtr)
            throw CommandError(cmd + ": " + proxyPath + " has no mesh");
        geo::Mesh m = *meshPtr;
        // The proxy's points are in world space; the patch is baked in the heightfield's own.
        gn::transformMesh(m, t.inv * p->as<glm::dmat4>(scene::Transform::kWorldMatrix));
        const Heightfield& h = *t.h;
        const double cell = h.cell * step;
        const glm::dvec2 origin = h.samplePos(x0, z0);
        size_t covered = 0;
        const Heightfield top = geo::meshToHeightfield(m, nx, nz, cell, origin, std::numeric_limits<float>::quiet_NaN(), &covered);
        std::vector<float> delta(size_t(nx) * size_t(nz), 0.0f);
        for (int z = 0; z < nz; ++z)
            for (int x = 0; x < nx; ++x)
            {
                const float v = top.at(x, z);
                if (std::isnan(v))
                    continue;
                const float d = v - h.at(std::min(h.nx - 1, x0 + x * step), std::min(h.nz - 1, z0 + z * step));
                delta[size_t(z) * size_t(nx) + size_t(x)] = d;
                if (std::abs(d) > 1e-3f)
                    ++changed;
                most = std::max(most, double(std::abs(d)));
            }
        // Downward faces under the top surface are what a heightfield can't hold: overhangs, kept as cells for the volume.
        std::vector<float> under(delta.size(), std::numeric_limits<float>::infinity());
        for (geo::Index f = 0; f < m.faceCapacity(); ++f)
        {
            if (!m.faceValid(f) || m.faceNormal(f).y > -0.1)
                continue;
            const std::vector<geo::Index> pts = m.facePoints(f);
            for (size_t k = 1; k + 1 < pts.size(); ++k)
            {
                const glm::dvec3 a = m.position(pts[0]), b = m.position(pts[k]), c = m.position(pts[k + 1]);
                const double lx = std::min({a.x, b.x, c.x}), hx = std::max({a.x, b.x, c.x});
                const double lz = std::min({a.z, b.z, c.z}), hz = std::max({a.z, b.z, c.z});
                const int ix0 = std::max(0, int(std::ceil((lx - origin.x) / cell))), ix1 = std::min(nx - 1, int(std::floor((hx - origin.x) / cell)));
                const int iz0 = std::max(0, int(std::ceil((lz - origin.y) / cell))), iz1 = std::min(nz - 1, int(std::floor((hz - origin.y) / cell)));
                const double den = (b.z - c.z) * (a.x - c.x) + (c.x - b.x) * (a.z - c.z);
                if (std::abs(den) < 1e-12)
                    continue;
                for (int iz = iz0; iz <= iz1; ++iz)
                    for (int ix = ix0; ix <= ix1; ++ix)
                    {
                        const double px = origin.x + ix * cell, pz = origin.y + iz * cell;
                        const double wa = ((b.z - c.z) * (px - c.x) + (c.x - b.x) * (pz - c.z)) / den;
                        const double wb = ((c.z - a.z) * (px - c.x) + (a.x - c.x) * (pz - c.z)) / den;
                        const double wc = 1.0 - wa - wb;
                        if (wa < -1e-9 || wb < -1e-9 || wc < -1e-9)
                            continue;
                        float& u = under[size_t(iz) * size_t(nx) + size_t(ix)];
                        u = std::min(u, float(wa * a.y + wb * b.y + wc * c.y));
                    }
            }
        }
        std::vector<uint8_t> flag(delta.size(), 0);
        for (size_t k = 0; k < delta.size(); ++k)
        {
            const float v = top.heights[k];
            flag[k] = !std::isnan(v) && std::isfinite(under[k]) && v - under[k] > float(cell * 0.25);
        }
        // Under each overhang, the highest upward face below its underside: the ground its hollow stands on.
        std::vector<float> floorUnder(delta.size(), -std::numeric_limits<float>::infinity());
        if (keepOverhangs && std::find(flag.begin(), flag.end(), uint8_t(1)) != flag.end())
            for (geo::Index f = 0; f < m.faceCapacity(); ++f)
            {
                if (!m.faceValid(f) || m.faceNormal(f).y < 0.1)
                    continue;
                const std::vector<geo::Index> pts = m.facePoints(f);
                for (size_t k = 1; k + 1 < pts.size(); ++k)
                {
                    const glm::dvec3 a = m.position(pts[0]), b = m.position(pts[k]), c = m.position(pts[k + 1]);
                    const double den = (b.z - c.z) * (a.x - c.x) + (c.x - b.x) * (a.z - c.z);
                    if (std::abs(den) < 1e-12)
                        continue;
                    const int ix0 = std::max(0, int(std::ceil((std::min({a.x, b.x, c.x}) - origin.x) / cell)));
                    const int ix1 = std::min(nx - 1, int(std::floor((std::max({a.x, b.x, c.x}) - origin.x) / cell)));
                    const int iz0 = std::max(0, int(std::ceil((std::min({a.z, b.z, c.z}) - origin.y) / cell)));
                    const int iz1 = std::min(nz - 1, int(std::floor((std::max({a.z, b.z, c.z}) - origin.y) / cell)));
                    for (int iz = iz0; iz <= iz1; ++iz)
                        for (int ix = ix0; ix <= ix1; ++ix)
                        {
                            const size_t k2 = size_t(iz) * size_t(nx) + size_t(ix);
                            if (!flag[k2])
                                continue;
                            const double px = origin.x + ix * cell, pz = origin.y + iz * cell;
                            const double wa = ((b.z - c.z) * (px - c.x) + (c.x - b.x) * (pz - c.z)) / den;
                            const double wb = ((c.z - a.z) * (px - c.x) + (a.x - c.x) * (pz - c.z)) / den;
                            const double wc = 1.0 - wa - wb;
                            if (wa < -1e-9 || wb < -1e-9 || wc < -1e-9)
                                continue;
                            const float y = float(wa * a.y + wb * b.y + wc * c.y);
                            if (y < under[k2] - float(cell * 0.05))
                                floorUnder[k2] = std::max(floorUnder[k2], y);
                        }
                }
            }
        std::vector<uint8_t> seen(delta.size(), 0);
        json volume = json::array();
        for (size_t s = 0; s < flag.size(); ++s)
        {
            if (!flag[s] || seen[s])
                continue;
            std::vector<size_t> stack = {s}, cells;
            seen[s] = 1;
            while (!stack.empty())
            {
                const size_t k = stack.back();
                stack.pop_back();
                cells.push_back(k);
                const int x = int(k % size_t(nx)), z = int(k / size_t(nx));
                for (auto [dx, dz] : {std::pair{1, 0}, {-1, 0}, {0, 1}, {0, -1}})
                {
                    const int xx = x + dx, zz = z + dz;
                    if (xx < 0 || zz < 0 || xx >= nx || zz >= nz)
                        continue;
                    const size_t j = size_t(zz) * size_t(nx) + size_t(xx);
                    if (flag[j] && !seen[j])
                    {
                        seen[j] = 1;
                        stack.push_back(j);
                    }
                }
            }
            json list = json::array();
            glm::dvec3 lo(std::numeric_limits<double>::infinity()), hi(-lo);
            for (size_t k : cells)
            {
                const int x = int(k % size_t(nx)), z = int(k / size_t(nx));
                list.push_back({x0 + x * step, z0 + z * step, under[k], top.heights[k]});
                for (double y : {double(under[k]), double(top.heights[k])})
                {
                    const glm::dvec3 w(t.world * glm::dvec4(origin.x + x * cell, y, origin.y + z * cell, 1.0));
                    lo = glm::min(lo, w);
                    hi = glm::max(hi, w);
                }
            }
            volume.push_back({{"cells", list}});
            overhangs.push_back({{"cells", cells.size()},
                                 {"min", json::array({groundOf2(lo).x, groundOf2(lo).y})},
                                 {"max", json::array({groundOf2(hi).x, groundOf2(hi).y})},
                                 {"bottom", upOf2(lo)},
                                 {"top", upOf2(hi)}});
        }
        if (changed > 0 || !volume.empty())
        {
            const glm::dvec3 centre(t.world * glm::dvec4(origin.x + (nx - 1) * cell * 0.5, 0.0, origin.y + (nz - 1) * cell * 0.5, 1.0));
            json patch = {{"x0", x0}, {"z0", z0}, {"step", step}, {"nx", nx}, {"nz", nz}, {"delta", packFloats(delta)}};
            if (!volume.empty())
                patch["volume"] = volume;
            ops::execute("terrainSculpt", {{"args", {terrain}},
                                           {"op", "patch"},
                                           {"points", json::array({json::array({centre.x, centre.y, centre.z})})},
                                           {"radius", std::max(1.0, cell * std::max(nx, nz))},
                                           {"strength", 1.0},
                                           {"patch", patch},
                                           {"stage", info.value("stage", std::string("final"))}});
        }
        if (keepOverhangs && !volume.empty())
        {
            // The hollow under each overhang, cell by cell, carved from the terrain's volume so the overhang survives the
            // bake as a volume piece. The mesh stays, hidden, because volume stamps follow their mesh.
            json points = json::array(), faces = json::array();
            int64_t boxes = 0;
            for (size_t k = 0; k < flag.size(); ++k)
            {
                if (!flag[k])
                    continue;
                const int x = int(k % size_t(nx)), z = int(k / size_t(nx));
                const double gx = origin.x + x * cell, gz = origin.y + z * cell;
                const double roof = under[k];
                double bottom = floorUnder[k];
                if (!std::isfinite(bottom))
                    bottom = std::min(double(t.h->at(std::min(t.h->nx - 1, x0 + x * step), std::min(t.h->nz - 1, z0 + z * step))), roof - cell);
                if (roof - bottom < 1e-3)
                    continue;
                const double hx = cell * 0.5;
                const size_t base = points.size();
                for (const glm::dvec3& q : {glm::dvec3(gx - hx, bottom, gz - hx), glm::dvec3(gx + hx, bottom, gz - hx), glm::dvec3(gx + hx, bottom, gz + hx),
                                            glm::dvec3(gx - hx, bottom, gz + hx), glm::dvec3(gx - hx, roof, gz - hx), glm::dvec3(gx + hx, roof, gz - hx),
                                            glm::dvec3(gx + hx, roof, gz + hx), glm::dvec3(gx - hx, roof, gz + hx)})
                {
                    const glm::dvec3 w(t.world * glm::dvec4(q, 1.0));
                    points.push_back({w.x, w.y, w.z});
                }
                static const size_t kBoxFaces[6][4] = {{0, 1, 2, 3}, {4, 7, 6, 5}, {0, 4, 5, 1}, {3, 2, 6, 7}, {0, 3, 7, 4}, {1, 5, 6, 2}};
                for (const auto& f : kBoxFaces)
                    faces.push_back({base + f[0], base + f[1], base + f[2], base + f[3]});
                ++boxes;
            }
            if (boxes > 0)
            {
                const std::string terrainName = t.mesh ? t.mesh->name() : std::string("Terrain");
                const json made = ops::execute("createMesh", {{"points", points}, {"faces", faces}, {"name", terrainName + "_overhangHollow"}});
                const std::string hollow = made.is_string() ? made.get<std::string>() : made.at(0).get<std::string>();
                ops::execute("setAttr", {{"args", {hollow + ".visibility", false}}});
                const json stamped = ops::execute("terrainVolumeStamp", {{"args", {terrain}}, {"object", hollow}, {"mode", "subtract"}, {"id", hollow}});
                overhangVolume = {{"mesh", hollow}, {"id", hollow}, {"cells", boxes}, {"volume", stamped.value("volume", json())}};
            }
        }
    }
    if (Node* tm = t.mesh)
        ops::execute("setAttr", {{"args", {tm->path() + ".visibility", info.value("terrainVisible", true)}}});
    if (keep)
    {
        json done = info;
        done["done"] = true;
        ops::execute("setAttr", {{"args", {proxyPath + "." + kRegionAttr, done.dump()}}});
    }
    else if (g.find(proxyPath))
        ops::execute("delete", {{"args", {proxyPath}}});
    out = infoOf(terrain);
    out["changedSamples"] = changed;
    out["largestChange"] = most;
    out["overhangs"] = overhangs;
    if (!overhangVolume.is_null())
        out["overhangVolume"] = overhangVolume;
    out["cancelled"] = cancel;
    return out;
}
} // namespace lcmd_

std::set<std::string> withLandformTypes(std::set<std::string> types)
{
    for (const lcmd_::Process& p : lcmd_::kProcesses)
        types.insert(p.type);
    return types;
}

json brushSettings(const Args& a, const std::function<double(double)>& localHeight, double vertical)
{
    json s = json::object();
    if (a.has("plane"))
        s["plane"] = a.get<bool>("plane", false);
    if (a.has("endHeight"))
        s["endHeight"] = localHeight(a.get<double>("endHeight", 0.0));
    if (a.has("step"))
        s["step"] = a.get<double>("step", 1000.0) / std::max(1e-9, vertical);
    if (a.has("sharpness"))
        s["sharpness"] = a.get<double>("sharpness", 0.8);
    for (const char* k : {"stamp", "blend", "alpha", "mask"})
        if (a.has(k))
            s[k] = a.get<std::string>(k, "");
    for (const char* k : {"angle", "value"})
        if (a.has(k))
            s[k] = a.get<double>(k, 0.0);
    if (a.has("iterations"))
        s["iterations"] = a.get<int64_t>("iterations", 4);
    if (a.has("patch"))
        s["patch"] = a.flags["patch"];
    if (s.contains("stamp"))
    {
        const std::string st = s["stamp"].get<std::string>();
        if (!isStampName(st) && !std::filesystem::exists(brush::alphas::resolve(st)))
            throw CommandError("terrainSculpt: no stamp or image '" + st + "' (stamps: peak, crater, dune, rocks, mesa, ridge, volcano)");
    }
    return s.empty() ? json() : s;
}

json erodePresetCommand(const std::string& terrain, const Args& a)
{
    LILA_ZONE();
    using namespace lcmd_;

    const std::string cmd = "terrainErode";
    if (a.has("preset") && !isErodePreset(a.get<std::string>("preset", "")))
        throw CommandError(cmd + ": unknown preset '" + a.get<std::string>("preset", "") + "' (natural, alpine, desert, hills)");
    Target t = findTarget(terrain, cmd);
    GroupEdit e = groupOf(t, cmd);
    gn::GNode* n = e.findType("TerrainErodePreset");
    bool edited = false;
    if (!n)
    {
        if (gn::GNode* erode = e.findType("ErodeTerrain"))
            n = &e.insertAfter(erode->name, "TerrainErodePreset", e.group.uniqueNodeName("Preset"), cmd);
        else
            n = &e.insertBeforeTrailing("TerrainErodePreset", e.group.uniqueNodeName("Preset"), trailingTypes(), cmd);
        edited = true;
    }
    const std::string nodeName = n->name;
    const bool exposedAge = t.modifier->type().attrIndex("age") >= 0;
    const bool exposedPreset = t.modifier->type().attrIndex("erosionPreset") >= 0;
    auto value = [&](const char* key, const json& v) {
        n->values[key] = v;
        edited = true;
    };
    if (a.has("age") && !exposedAge)
        value("age", a.get<double>("age", 0.5));
    if (a.has("preset") && !exposedPreset)
        value("preset", a.get<std::string>("preset", "natural"));
    if (!a.has("age") && !exposedAge && !n->values.contains("age"))
        value("age", 0.5);
    if (a.has("seed"))
        value("seed", a.get<int64_t>("seed", 0));
    if (a.has("hardnessMask"))
        value("hardnessMask", a.get<std::string>("hardnessMask", ""));
    if (a.has("device"))
    {
        checkDevice(a.get<std::string>("device", "cpu"), cmd);
        value("device", a.get<std::string>("device", "cpu"));
    }
    if (edited)
        commit(e, cmd);
    t = findTarget(terrain, cmd);
    const std::string mod = t.modifier->path();
    if (a.has("age") && exposedAge)
        ops::execute("setAttr", {{"args", {mod + ".age", a.get<double>("age", 0.5)}}});
    else if (!a.has("age") && exposedAge && t.modifier->as<double>(t.modifier->type().attrIndex("age")) <= 0.0)
        ops::execute("setAttr", {{"args", {mod + ".age", 0.5}}});
    if (a.has("preset") && exposedPreset)
        ops::execute("setAttr", {{"args", {mod + ".erosionPreset", a.get<std::string>("preset", "natural")}}});
    if (a.has("iterations") && t.modifier->type().attrIndex("erosion") >= 0)
        ops::execute("setAttr", {{"args", {mod + ".erosion", a.get<int64_t>("iterations", 0)}}});
    const double ms = evaluateMs();
    json out = infoOf(t.path());
    json report = presetReport(t.modifier->id(), nodeName);
    out["preset"] = report.is_null() ? json::object() : report;
    out["node"] = nodeName;
    out["ms"] = ms;
    return out;
}

void registerLandformCommands(ext::Context& ctx)
{
    using namespace lcmd_;

    ctx.registerCommand({
        .name = "terrainLandform",
        .doc = "Add an erosion or shaping step to a terrain, or change or remove one.\n\n"
               "process= picks it: analytic (stream power at an age), amplify (more samples keeping the drainage), debris "
               "(debris flow scars and cones), thermal (scree, with a talus angle per material mask), hydraulic (droplet "
               "rills), deposition (fans, valley and lake fill), snow, filter (the erosion filter: instant gullies), terrace, "
               "strata, fold, warp, ridgeSharpen, cliff, preset (the Erode chain). settings= sets the node's inputs by name "
               "(lengths and heights in the terrain's own units, cm). The step goes after erosion, before stamps and final "
               "sculpting (stage='base' puts it before Erode Terrain; after= right after a named node). Calling again with "
               "the same id changes it. Returns terrainInfo's stats (masks with coverage), the node and the evaluation time.",
        .positionalDoc = "terrain mesh or its modifier",
        .flags = {{"process", "p", T::String, nullptr, "The step.", processNames()},
                  {"id", "i", T::String, "", "Node name; default from the process (Debris, Snow, ...)."},
                  {"settings", "s", T::Any, nullptr, "{input: value} for the node, e.g. {'age': 0.6} or {'step': 1500}."},
                  {"device", "d", T::String, nullptr, "filter, thermal and preset: cpu (exact), gpu or auto.", {"cpu", "gpu", "auto"}},
                  {"stage", "sg", T::String, "final", "final (after erosion) or base (before it).", {"base", "final"}},
                  {"after", "af", T::String, nullptr, "Put it right after this node of the group."},
                  {"remove", "rm", T::Bool, false, "Remove the node named id (or the process's default name)."}},
        .run = [](const Args& a) -> json {
            LILA_ZONE();

            const std::string cmd = "terrainLandform";
            std::vector<std::string> names = a.objects();
            if (names.size() != 1)
                throw CommandError(cmd + ": give one terrain");
            Target t = findTarget(names[0], cmd);
            const std::string processName = a.get<std::string>("process", "");
            const Process* proc = processNamed(processName);
            if (!proc && !(a.get<bool>("remove", false) && a.has("id")))
                throw CommandError(cmd + ": give process= (" + [] {
                    std::string s;
                    for (const std::string& n : processNames())
                        s += (s.empty() ? "" : ", ") + n;
                    return s;
                }() + ")");
            const std::string nodeName = nodeIdentifier(a.get<std::string>("id", "").empty() ? std::string(proc ? proc->node : "") : a.get<std::string>("id", ""));
            GroupEdit e = groupOf(t, cmd);
            if (a.get<bool>("remove", false))
            {
                if (!e.find(nodeName))
                    throw CommandError(cmd + ": the terrain's group has no node " + nodeName);
                e.remove(nodeName);
                commit(e, cmd);
                json out = infoOf(t.path());
                out["removed"] = nodeName;
                return out;
            }
            gn::GNode* n = e.find(nodeName);
            if (n && n->type != proc->type)
                throw CommandError(cmd + ": " + nodeName + " is a " + n->type + " node; give another id");
            if (!n)
            {
                if (a.has("after"))
                {
                    const std::string after = a.get<std::string>("after", "");
                    if (!e.find(after))
                        throw CommandError(cmd + ": the terrain's group has no node " + after);
                    n = &e.insertAfter(after, proc->type, nodeName, cmd);
                }
                else if (a.get<std::string>("stage", "final") == "base")
                {
                    gn::GNode* erode = e.findType("ErodeTerrain");
                    gn::GLink* l = erode ? e.linkInto(erode->name, "heightfield") : nullptr;
                    if (!l)
                        throw CommandError(cmd + ": stage='base' needs an Erode Terrain node to go before");
                    n = &e.splice(*l, proc->type, nodeName);
                }
                else
                    n = &e.insertBeforeTrailing(proc->type, nodeName, trailingTypes(), cmd);
            }
            setValues(*n, a.has("settings") ? a.flags["settings"] : json(), cmd);
            if (a.has("device"))
            {
                const gn::NodeDecl* d = gn::findDecl(n->type);
                if (!d || !d->prop("device"))
                    throw CommandError(cmd + ": " + processName + " runs on the CPU only");
                checkDevice(a.get<std::string>("device", "cpu"), cmd);
                n->values["device"] = a.get<std::string>("device", "cpu");
            }
            const std::string type = n->type;
            commit(e, cmd);
            const double ms = evaluateMs();
            json out = infoOf(t.path());
            out["node"] = nodeName;
            out["type"] = type;
            out["ms"] = ms;
            if (type == "TerrainErodePreset")
                out["preset"] = presetReport(findTarget(t.path(), cmd).modifier->id(), nodeName);
            return out;
        },
        .examples = {"terrainLandform('Terrain', process='debris', settings={'strength': 1.5})",
                     "terrainLandform('Terrain', process='filter', settings={'scale': 3000, 'strength': 0.15}, device='auto')",
                     "terrainLandform('Terrain', process='thermal', settings={'talusAngle': 34, 'materialMask': 'strata', 'materialAngle': 65})",
                     "terrainLandform('Terrain', process='snow', settings={'autoSnowline': False, 'snowline': 30000})['masks']['snow']",
                     "terrainLandform('Terrain', id='Debris', remove=True)"},
    });

    {
        std::vector<FlagDef> flags = strokeFlags();
        ctx.registerCommand({
            .name = "terrainStroke",
            .doc = "A brush stroke on a terrain: terrainSculpt with the brush named by brush= (raise, lower, smooth, flatten, noise, "
                   "ramp, terrace, stamp, erode, uplift, relax, mask). Every flag is terrainSculpt's; the Sculpt Terrain tool "
                   "records its strokes this way, so a stroke replays exactly from its call. Returns terrainSculpt's result.",
            .positionalDoc = "terrain mesh or its modifier",
            .flags = flags,
            .run = [](const Args& a) -> json {
                json call = json::object();
                for (const auto& [k, v] : a.flags.items())
                    if (!v.is_null())
                        call[k == "brush" ? "op" : k] = v;
                call["args"] = a.positional;
                const bool uplift = call.value("op", std::string()) == "uplift";
                if (uplift)
                    call["stage"] = "base";
                json out = ops::execute("terrainSculpt", call);
                if (uplift && !a.get<bool>("clear", false) && !a.get<bool>("removeLast", false))
                    out["erosion"] = followUplift(a.objects().at(0), a.get<std::string>("mask", "uplift"));
                return out;
            },
            .examples = {"terrainStroke('Terrain', brush='raise', path=[[-20000, 0], [20000, 0]], radius=5000, strength=800, pressure=[0.2, 1.0])",
                         "terrainStroke('Terrain', brush='ramp', points=[[0, 0], [30000, 0]], radius=2500, height=1000, endHeight=6000, strength=1, stage='final')",
                         "terrainStroke('Terrain', brush='mask', mask='rock', points=[[5000, 5000]], radius=8000, strength=1, value=1)"},
        });
    }

    ctx.registerCommand({
        .name = "terrainSculptRegion",
        .doc = "Sculpt part of a terrain with the mesh sculpt brushes.\n\n"
               "Copies the patch around center= (size= cm square) to a mesh, <terrain>_region, hides the terrain and enters "
               "sculpt mode on it. Sculpt with any brush, then finish=True (or Exit Sculpt in the app) bakes it back: "
               "the change in height goes into a patch stroke in the terrain's sculpt layer (stage final by default, so after "
               "erosion), faded at the patch rim, and the proxy is deleted. Where the sculpt folds under itself (an overhang a "
               "heightfield can't hold) the cells are listed in the result's overhangs, and the hollow under them is carved from "
               "the terrain's volume (a hidden <terrain>_overhangHollow mesh stamped with mode subtract, listed as overhangVolume) "
               "so the overhang becomes a volume piece; keepOverhangs=False only lists them. "
               "cancel=True drops the proxy without baking. Returns the proxy and patch size, or on finish terrainInfo's stats "
               "with changedSamples, largestChange and overhangs.",
        .positionalDoc = "terrain mesh (or the region proxy, for finish and cancel)",
        .flags = {{"center", "c", T::List, nullptr, "Middle of the patch: [x, z] or [x, y, z]."},
                  {"size", "sz", T::Float, 20000.0, "Width of the square patch, cm.", {}, {}, 1.0},
                  {"resolution", "r", T::Int, int64_t(256), "Most samples along a side of the proxy; denser terrains are skipped through.", {}, {}, 8.0, 2048.0},
                  {"stage", "sg", T::String, "final", "Where the bake goes: final (after erosion) or base (before it).", {"base", "final"}},
                  {"enter", "e", T::Bool, true, "Enter sculpt mode on the proxy."},
                  {"finish", "f", T::Bool, false, "Bake the proxy back into the terrain."},
                  {"cancel", "cn", T::Bool, false, "Drop the proxy without baking."},
                  {"keep", "k", T::Bool, false, "finish: keep the proxy mesh."},
                  {"keepOverhangs", "ko", T::Bool, true, "finish: carve the hollow under each overhang into the terrain's volume, so it survives as a mesh piece."}},
        .run = [](const Args& a) -> json {
            LILA_ZONE();

            const std::string cmd = "terrainSculptRegion";
            std::vector<std::string> names = a.objects();
            if (names.size() != 1)
                throw CommandError(cmd + ": give one terrain");
            Graph& g = Graph::current();
            Node* given = g.find(names[0]);
            if (!given)
                throw CommandError(cmd + ": no node named '" + names[0] + "'");
            if (a.get<bool>("finish", false) || a.get<bool>("cancel", false))
            {
                Node* proxy = given;
                json info = regionInfo(*proxy);
                if (info.is_null())
                {
                    proxy = g.find(regionName(findTarget(names[0], cmd)));
                    info = proxy ? regionInfo(*proxy) : json();
                }
                if (info.is_null() || info.value("done", false))
                    throw CommandError(cmd + ": " + names[0] + " has no open sculpt region");
                return finishRegion(*proxy, info, a.get<bool>("keep", false), a.get<bool>("cancel", false), a.get<bool>("keepOverhangs", true));
            }
            if (!a.has("center"))
                throw CommandError(cmd + ": give center=");
            const Target t = findTarget(names[0], cmd);
            const Heightfield& h = *t.h;
            const glm::dvec2 c = t.local(pointArg(a.flags["center"], cmd));
            const double half = std::max(1.0, a.get<double>("size", 20000.0)) * 0.5 / std::max(1e-9, t.across());
            const int x0 = std::clamp(int(std::floor((c.x - half - h.origin.x) / h.cell)), 0, h.nx - 1);
            const int z0 = std::clamp(int(std::floor((c.y - half - h.origin.y) / h.cell)), 0, h.nz - 1);
            const int x1 = std::clamp(int(std::ceil((c.x + half - h.origin.x) / h.cell)), 0, h.nx - 1);
            const int z1 = std::clamp(int(std::ceil((c.y + half - h.origin.y) / h.cell)), 0, h.nz - 1);
            if (x1 - x0 < 2 || z1 - z0 < 2)
                throw CommandError(cmd + ": the patch misses the terrain or is under three samples wide");
            const int most = int(std::clamp<int64_t>(a.get<int64_t>("resolution", 256), 8, 2048));
            const int step = std::max(1, int(std::ceil(double(std::max(x1 - x0, z1 - z0)) / double(most - 1))));
            const int nx = (x1 - x0) / step + 1, nz = (z1 - z0) / step + 1;
            Heightfield patch = geo::makeHeightfield(nx, nz, h.cell * step, h.samplePos(x0, z0));
            for (int z = 0; z < nz; ++z)
                for (int x = 0; x < nx; ++x)
                    patch.heights[patch.index(x, z)] = h.at(x0 + x * step, z0 + z * step);
            geo::HeightfieldMeshOptions mo;
            mo.masks = false;
            geo::Mesh m = geo::heightfieldToMesh(patch, mo);
            gn::transformMesh(m, t.world);

            const std::string name = regionName(t);
            if (Node* old = g.find(name))
                ops::execute("delete", {{"args", {old->path()}}});
            json made = ops::execute("createMesh", {{"points", json::array()}, {"faces", json::array()}, {"name", name}});
            const std::string path = made.is_string() ? made.get<std::string>() : made.at(0).get<std::string>();
            Node* mesh = g.find(path);
            g.setValue({mesh->id(), scene::MeshNode::kInMesh, -1}, scene::MeshPtr(std::make_shared<geo::Mesh>(std::move(m))));
            const bool visible = t.mesh ? t.mesh->as<bool>(t.mesh->type().attrIndex("visibility")) : true;
            const json info = {{"terrain", t.path()}, {"x0", x0}, {"z0", z0}, {"step", step}, {"nx", nx}, {"nz", nz},
                               {"stage", a.get<std::string>("stage", "final")}, {"terrainVisible", visible}};
            ops::execute("addAttr", {{"args", {path}}, {"longName", kRegionAttr}, {"dataType", "string"}, {"hidden", true}});
            ops::execute("setAttr", {{"args", {path + "." + kRegionAttr, info.dump()}}});
            if (t.mesh)
                ops::execute("setAttr", {{"args", {t.mesh->path() + ".visibility", false}}});
            bool sculpting = false;
            std::string note;
            if (a.get<bool>("enter", true))
            {
                if (ops::find("sculptEnter"))
                {
                    ops::execute("select", {{"args", {path}}});
                    ops::execute("sculptEnter", {{"args", {path}}});
                    sculpting = true;
                }
                else
                {
                    note = "sculpt mode isn't available here; sculpt the proxy another way, then finish=True";
                    log::warn("{}: {}", cmd, note);
                }
            }
            const glm::dvec3 lo(t.world * glm::dvec4(patch.origin.x, 0.0, patch.origin.y, 1.0));
            const glm::dvec3 hi(t.world * glm::dvec4(patch.origin.x + patch.extent().x, 0.0, patch.origin.y + patch.extent().y, 1.0));
            json out = {{"mesh", path},
                        {"terrain", t.path()},
                        {"samples", json::array({nx, nz})},
                        {"step", step},
                        {"min", json::array({std::min(groundOf2(lo).x, groundOf2(hi).x), std::min(groundOf2(lo).y, groundOf2(hi).y)})},
                        {"max", json::array({std::max(groundOf2(lo).x, groundOf2(hi).x), std::max(groundOf2(lo).y, groundOf2(hi).y)})},
                        {"sculpting", sculpting}};
            if (!note.empty())
                out["note"] = note;
            return out;
        },
        .examples = {"r = terrainSculptRegion('Terrain', center=[12000, -4000], size=15000)\n# sculpt r['mesh'] with any brush, then:\nterrainSculptRegion('Terrain', finish=True)['overhangs']"},
    });

    ctx.registerCommand({
        .name = "terrainStampLibrary",
        .doc = "The terrain brushes and the stamp library (procedural peaks, craters, dunes, rocks, mesas, ridges, volcanoes). "
               "With folder=, writes each stamp as a 16-bit PNG (mid grey is no change) for other tools or previews. Returns "
               "brushes, stamps and the files written.",
        .flags = {{"folder", "f", T::String, nullptr, "Where to write the stamp images."},
                  {"size", "sz", T::Int, int64_t(256), "Image size, pixels.", {}, {}, 8.0, 4096.0},
                  {"seed", "sd", T::Int, int64_t(0), "Seed for the stamps' detail."}},
        .undoable = false,
        .run = [](const Args& a) -> json {
            json files = json::array();
            if (a.has("folder"))
            {
                const std::filesystem::path dir = a.get<std::string>("folder", "");
                std::error_code ec;
                std::filesystem::create_directories(dir, ec);
                const int size = int(std::clamp<int64_t>(a.get<int64_t>("size", 256), 8, 4096));
                for (const std::string& n : stampNames())
                {
                    const std::vector<float> v = stampImage(n, size, uint32_t(a.get<int64_t>("seed", 0)));
                    std::vector<float> rgba(v.size() * 4);
                    for (size_t k = 0; k < v.size(); ++k)
                    {
                        rgba[k * 4] = rgba[k * 4 + 1] = rgba[k * 4 + 2] = v[k];
                        rgba[k * 4 + 3] = 1.0f;
                    }
                    const std::filesystem::path file = dir / ("terrain_" + n + ".png");
                    try
                    {
                        image::writePng(file, size, size, rgba, 16);
                    }
                    catch (const std::exception& e)
                    {
                        throw CommandError(std::string("terrainStampLibrary: ") + e.what());
                    }
                    files.push_back(file.generic_string());
                }
            }
            return {{"brushes", sculptOpNames()}, {"stamps", stampNames()}, {"files", files}};
        },
        .examples = {"terrainStampLibrary()['stamps']", "terrainStampLibrary(folder='stamps', size=512)['files']"},
    });
}
} // namespace terrain
