// Tailor commands. A garment is addressed by its tailorGarment node or the mesh it feeds; with no garment given, the
// selection's. Every edit goes through Graph::setValue, so undo comes from the graph.

#include "Blocks.h"
#include "GarmentCodeImport.h"
#include "Pattern.h"
#include "Tailor.h"
#include "TailorCore.h"

#include <lila/Command.h>
#include <lila/Curves.h>
#include <lila/Diagnostics.h>
#include <lila/Extension.h>
#include <lila/Graph.h>
#include <lila/ImageIO.h>
#include <lila/Paths.h>
#include <lila/Log.h>
#include <lila/Mesh.h>
#include <lila/Node.h>
#include <lila/Transform.h>
#include <lila/UnrealLink.h>
#include <lila/Uv.h>

#include <algorithm>
#include <functional>
#include <map>
#include <tuple>
#include <cmath>
#include <numbers>
#include <optional>

using namespace lila;
using namespace lila::scene;
using json = nlohmann::json;

namespace tailor
{
using T = ArgType;

namespace
{

int plugIndex(const Node& n, const char* name)
{
    const int k = n.type().attrIndex(name);
    if (k < 0)
        throw CommandError(std::string("tailor: ") + n.name() + " has no attribute " + name);
    return k;
}

Node* garmentFeeding(Graph& g, const Node& n)
{
    if (n.type().isA(kGarmentType))
        return g.node(n.id());
    const int in = n.type().attrIndex("inMesh");
    if (in < 0)
        return nullptr;
    const std::optional<PlugRef> src = g.source({n.id(), in});
    Node* s = src ? g.node(src->node) : nullptr;
    return s && s->type().isA(kGarmentType) ? s : nullptr;
}

/** The garment named by the first positional argument, else the first selected one. */
Node& garmentArg(Graph& g, const Args& a, const char* cmd)
{
    if (!a.positional.empty() && a.positional[0].is_string())
    {
        const std::string path = a.positional[0].get<std::string>();
        Node* n = g.find(path);
        if (!n)
            throw CommandError(std::string(cmd) + ": no node " + path);
        if (Node* gn = garmentFeeding(g, *n))
            return *gn;
        throw CommandError(std::string(cmd) + ": " + path + " is not a garment or a mesh fed by one");
    }
    for (NodeId id : g.selection())
        if (Node* n = g.node(id))
            if (Node* gn = garmentFeeding(g, *n))
                return *gn;
    throw CommandError(std::string(cmd) + ": name a garment or select one");
}

Garment readGarment(const Node& n)
{
    const std::string& text = n.as<std::string>(plugIndex(n, "garment"));
    return text.empty() ? Garment{} : garmentFromJson(json::parse(text));
}

void writeGarment(Graph& g, const Node& n, const Garment& gm) { g.setValue({n.id(), plugIndex(n, "garment")}, garmentToJson(gm).dump()); }

/** A panel by name or index. */
int panelIndex(const Garment& gm, const json& ref, const char* cmd)
{
    if (ref.is_number_integer())
    {
        const int k = ref.get<int>();
        if (k >= 0 && size_t(k) < gm.panels.size())
            return k;
    }
    else if (ref.is_string())
    {
        const std::string s = ref.get<std::string>();
        for (size_t k = 0; k < gm.panels.size(); ++k)
            if (gm.panels[k].name == s)
                return int(k);
        if (!s.empty() && s.find_first_not_of("0123456789") == std::string::npos)
            return panelIndex(gm, json(std::stoi(s)), cmd);
    }
    throw CommandError(std::string(cmd) + ": no panel " + ref.dump());
}

/** The panels a List flag names; all of them when it is missing or empty. */
std::vector<int> panelsArg(const Garment& gm, const Args& a, const char* flag, const char* cmd)
{
    std::vector<int> out;
    const json list = a.has(flag) ? a.flags[flag] : json::array();
    if (list.is_array() && list.empty())
    {
        for (size_t k = 0; k < gm.panels.size(); ++k)
            out.push_back(int(k));
        return out;
    }
    if (!list.is_array())
        return {panelIndex(gm, list, cmd)};
    for (const json& r : list)
        out.push_back(panelIndex(gm, r, cmd));
    return out;
}

/** "Front:0:4" (panel, first and last outline vertex), several joined by commas for a chain. */
std::vector<SeamSide> parseSides(const Garment& gm, const std::string& text, const char* cmd)
{
    std::vector<SeamSide> out;
    size_t start = 0;
    while (start <= text.size())
    {
        const size_t end = std::min(text.find(',', start), text.size());
        const std::string part = text.substr(start, end - start);
        const size_t c1 = part.find(':'), c2 = c1 == std::string::npos ? c1 : part.find(':', c1 + 1);
        if (c2 == std::string::npos)
            throw CommandError(std::string(cmd) + ": '" + part + "' is not panel:from:to");
        SeamSide s;
        s.panel = panelIndex(gm, json(part.substr(0, c1)), cmd);
        try
        {
            s.from = std::stoi(part.substr(c1 + 1, c2 - c1 - 1));
            s.to = std::stoi(part.substr(c2 + 1));
        }
        catch (const std::exception&)
        {
            throw CommandError(std::string(cmd) + ": '" + part + "' is not panel:from:to");
        }
        const int n = int(gm.panels[size_t(s.panel)].outline.size());
        if (s.from < 0 || s.to < 0 || s.from >= n || s.to >= n)
            throw CommandError(std::string(cmd) + ": " + part + ": the panel's outline has " + std::to_string(n) + " points");
        out.push_back(s);
        start = end + 1;
    }
    return out;
}

/** [[x, y], ...] in cm to pattern points in metres. */
std::vector<Vec2> pointsArg(const json& list, const char* cmd)
{
    std::vector<Vec2> out;
    if (list.is_array())
        for (const json& p : list)
            if (p.is_array() && p.size() >= 2 && p[0].is_number() && p[1].is_number())
                out.push_back({float(p[0].get<double>() / kCmPerM), float(p[1].get<double>() / kCmPerM)});
            else
                throw CommandError(std::string(cmd) + ": points are [x, y] pairs in cm");
    if (out.empty())
        throw CommandError(std::string(cmd) + ": give points as [[x, y], ...] in cm");
    return out;
}

std::vector<std::string> presetChoices()
{
    std::vector<std::string> out;
    for (const Fabric& f : fabricPresets())
        out.push_back(f.name);
    return out;
}

const Fabric& fabricArg(const std::string& name, const char* cmd)
{
    const Fabric* f = findFabric(name);
    if (!f)
        throw CommandError(std::string(cmd) + ": no fabric preset " + name);
    return *f;
}

Node* colliderArg(Graph& g, const Args& a)
{
    if (a.has("collider"))
    {
        const std::string path = a.get<std::string>("collider");
        if (path.empty())
            return nullptr;
        Node* n = g.find(path);
        if (!n || !n->type().isA("mesh"))
            throw CommandError("tailor: collider " + path + " is not a mesh");
        return n;
    }
    for (NodeId id : g.selection())
        if (Node* n = g.node(id); n && n->type().isA("mesh") && !garmentFeeding(g, *n))
            return n;
    return nullptr;
}

/** The collider's body measurements and where its feet stand, in metres. neck (above the feet), when set, overrides the measured one. */
Body measureCollider(Graph& g, Node& collider, Vec3& offset, float neck = 0.f)
{
    g.evaluate();
    const PlugValue v = g.value({collider.id(), MeshNode::kOutMesh});
    const MeshPtr* mesh = std::get_if<MeshPtr>(&v);
    if (!mesh || !*mesh || (*mesh)->empty())
        return {};
    std::vector<Vec3> pts;
    std::vector<std::array<int, 3>> tris;
    colliderTriangles(**mesh, std::get<glm::dmat4>(g.value({collider.id(), Transform::kWorldMatrix})), pts, tris);
    return measureBody(pts, tris, &offset, neck);
}

/** Where panels meet on a garment mesh (edges between faces of different panels), as world polylines in cm. */
json seamGuides(const geo::Mesh& m)
{
    const std::span<const int32_t> panel = m.faceAttrs().get<int32_t>("panel");
    if (panel.empty())
        return json::array();
    std::vector<std::vector<geo::Index>> next(m.pointCapacity());
    for (geo::Index e = 0; e < m.edgeCapacity(); ++e)
    {
        if (!m.edgeValid(e) || m.edgeFaceCount(e) != 2)
            continue;
        const geo::Index c = m.edgeCorner(e);
        const geo::Index f0 = m.cornerFace(c), f1 = m.cornerFace(m.cornerRadial(c));
        if (f0 >= panel.size() || f1 >= panel.size() || panel[f0] == panel[f1])
            continue;
        const geo::Index a = m.edgePoint(e, 0), b = m.edgePoint(e, 1);
        next[a].push_back(b);
        next[b].push_back(a);
    }
    json lines = json::array();
    std::vector<uint8_t> used(m.pointCapacity(), 0);
    auto point = [&](geo::Index p) {
        const glm::dvec3 q = m.position(p);
        return json::array({q.x, q.y, q.z});
    };
    // Start at chain ends first so open seams come out whole, then sweep up closed loops.
    for (int pass = 0; pass < 2; ++pass)
        for (geo::Index s = 0; s < m.pointCapacity(); ++s)
        {
            if (used[s] || next[s].empty() || (pass == 0 && next[s].size() == 2))
                continue;
            json line = json::array({point(s)});
            used[s] = 1;
            for (geo::Index cur = s;;)
            {
                geo::Index step = geo::kInvalid;
                for (geo::Index nb : next[cur])
                    if (!used[nb])
                    {
                        step = nb;
                        break;
                    }
                if (step == geo::kInvalid)
                    break;
                used[step] = 1;
                line.push_back(point(step));
                cur = step;
            }
            if (line.size() >= 2)
                lines.push_back(std::move(line));
        }
    return lines;
}

/**
 * Point float attribute clothMaxDistance: how far each point sits from the collider's surface, cm. Near zero where the
 * garment rests on the body, large where it hangs free; Chaos Cloth's Max Distance mask reads it directly.
 */
void writeMaxDistance(Graph& g, const Node& collider, geo::Mesh& garment)
{
    const PlugValue v = g.value({collider.id(), MeshNode::kOutMesh});
    const MeshPtr* body = std::get_if<MeshPtr>(&v);
    if (!body || !*body || (*body)->empty())
        return;
    std::vector<Vec3> pts;
    std::vector<std::array<int, 3>> tris;
    colliderTriangles(**body, std::get<glm::dmat4>(g.value({collider.id(), Transform::kWorldMatrix})), pts, tris);
    const Sdf sdf = sdfFromMesh(pts, tris, 0.02f, 0.4f);
    std::span<float> dist = garment.pointAttrs().getOrAdd<float>("clothMaxDistance");
    for (geo::Index p = 0; p < garment.pointCapacity(); ++p)
    {
        const glm::dvec3 q = garment.position(p) / kCmPerM;
        dist[p] = std::max(0.f, sdf.sample({float(q.x), float(q.y), float(q.z)})) * float(kCmPerM);
    }
}

/** Chaos's 0 to 1 weights for a Tailor fabric; Tailor's cotton (4000 N/m, 2e-5 N m) lands near the middle. */
json chaosConfig(const Fabric& f)
{
    return {{"density", f.density},
            {"edgeStiffness", std::clamp(0.5 * (f.stretchWarp + f.stretchWeft) / 7000.0, 0.05, 1.0)},
            {"bendStiffness", std::clamp(0.5 * (f.bendWarp + f.bendWeft) / 4e-5, 0.01, 1.0)},
            {"damping", f.damping},
            {"friction", f.friction},
            {"collisionThickness", std::max(0.2, f.thickness * kCmPerM + 0.5)}};
}

/** Checks a garment against its collider's measurements and posts the findings as diagnostics on its mesh. */
/**
 * Bakes maps from the drape (high) onto the game mesh (low) through a bake project of their own. The high is hidden and
 * coloured by panel, so the id map tells the panels apart. Returns the bake's result.
 */
json bakeDrapeMaps(Graph& g, const std::string& high, const std::string& low, const std::string& name, const json& maps, int64_t size,
                   const std::string& folder)
{
    Node* h = g.find(high);
    if (!h)
        throw CommandError("tailorBake: the drape to bake from is gone");
    g.evaluate();
    const PlugValue hv = g.value({h->id(), MeshNode::kOutMesh});
    if (const MeshPtr* hm = std::get_if<MeshPtr>(&hv); hm && *hm)
    {
        auto coloured = std::make_shared<geo::Mesh>(**hm);
        const std::span<const int32_t> panel = coloured->faceAttrs().get<int32_t>("panel");
        std::span<glm::vec4> colour = coloured->pointAttrs().getOrAdd<glm::vec4>(geo::attr::kColor);
        if (!panel.empty())
            for (geo::Index f = 0; f < coloured->faceCapacity(); ++f)
            {
                if (!coloured->faceValid(f) || size_t(f) >= panel.size())
                    continue;
                // Golden-ratio hues: neighbouring panel indices get far-apart colours.
                const float hue = float(std::fmod(0.07 + 0.618034 * double(panel[size_t(f)]), 1.0)) * 6.f;
                const glm::vec3 rgb = glm::clamp(glm::abs(glm::mod(hue + glm::vec3(0.f, 4.f, 2.f), 6.f) - 3.f) - 1.f, 0.f, 1.f);
                const glm::vec3 c = 0.9f * glm::mix(glm::vec3(1.f), rgb, 0.7f);
                for (geo::Index p : coloured->facePoints(f))
                    colour[size_t(p)] = {c, 1.f};
            }
        g.setValue({h->id(), MeshNode::kInMesh, -1}, MeshPtr(std::move(coloured)));
    }
    g.setValue({h->id(), plugIndex(*h, "visibility")}, false);
    const json project = ops::execute("createBakeProject", {{"name", name + "_maps"}, {"folder", folder}});
    ops::execute("setBakeOutput", {{"project", project}, {"resolution", size}, {"name", "T_{mesh}_{map}"}});
    ops::execute("bakeGroup", {{"args", {name + "_group"}}, {"project", project}, {"lows", {low}}, {"highs", {high}}});
    for (const json& m : maps)
        if (m.is_string())
            ops::execute("setBakeMap", {{"args", {m}}, {"project", project}, {"enabled", true}});
    if (std::find(maps.begin(), maps.end(), json("id")) != maps.end())
        ops::execute("setBakeMap", {{"args", {"id"}}, {"project", project}, {"options", {{"source", "vertexColor"}}}});
    return ops::execute("bake", {{"project", project}, {"maps", maps}});
}

/** [[x, y, z], ...] world cm to garment space metres. */
std::vector<glm::dvec3> brushPoints(Graph& g, const Node& op, const Args& a, const char* cmd)
{
    const json list = a.flags.value("points", json::array());
    const glm::dmat4 toLocal = glm::inverse(garmentWorld(g, op.id()));
    std::vector<glm::dvec3> out;
    if (list.is_array())
        for (const json& p : list)
            if (p.is_array() && p.size() == 3 && p[0].is_number() && p[1].is_number() && p[2].is_number())
                out.push_back(glm::dvec3(toLocal * glm::dvec4(p[0].get<double>(), p[1].get<double>(), p[2].get<double>(), 1.0)) / kCmPerM);
    if (out.empty())
        throw CommandError(std::string(cmd) + ": give points, [[x, y, z], ...] in world cm");
    return out;
}

/** The garment's draped surface as evaluated now, garment space cm. */
MeshPtr drapedMesh(Graph& g, const Node& op, const char* cmd)
{
    {
        DrapeNow now;
        g.evaluate();
    }
    const PlugValue v = g.value({op.id(), plugIndex(op, "outMesh")});
    const MeshPtr* mesh = std::get_if<MeshPtr>(&v);
    if (!mesh || !*mesh || (*mesh)->empty())
        throw CommandError(std::string(cmd) + ": " + op.name() + " has no drape");
    return *mesh;
}

/** Attaches the garment's new pins to its collider as the collider stands now. */
void attachToCollider(Graph& g, const Node& op, Garment& gm)
{
    const std::optional<PlugRef> src = g.source({op.id(), plugIndex(op, "colliderMesh")});
    const Node* body = src ? g.node(src->node) : nullptr;
    if (!body)
        return;
    const PlugValue bv = g.value({body->id(), MeshNode::kOutMesh});
    const PlugValue bw = g.value({body->id(), Transform::kWorldMatrix});
    const MeshPtr* bm = std::get_if<MeshPtr>(&bv);
    if (!bm || !*bm || !std::get_if<glm::dmat4>(&bw))
        return;
    std::vector<Vec3> pts;
    std::vector<std::array<int, 3>> tris;
    colliderTriangles(**bm, std::get<glm::dmat4>(bw), pts, tris);
    attachPins(gm, pts, tris);
}

/** Where a point (metres) lies on the drape's outer side: panel and pattern point, when within reach (metres). */
std::optional<std::pair<int, Vec2>> patternPointAt(const geo::Mesh& m, const glm::dvec3& at, double reach)
{
    const std::span<const int32_t> panel = m.faceAttrs().get<int32_t>("panel");
    const std::span<const int32_t> side = m.faceAttrs().get<int32_t>("side");
    const std::span<const glm::vec2> fabric = m.cornerAttrs().get<glm::vec2>(geo::attr::uvName(1));
    if (panel.empty() || fabric.empty())
        return std::nullopt;
    const glm::dvec3 p = at * kCmPerM;
    double best = reach * kCmPerM;
    std::optional<std::pair<int, Vec2>> out;
    for (geo::Index f = 0; f < m.faceCapacity(); ++f)
    {
        if (!m.faceValid(f) || (!side.empty() && side[f] != int32_t(GarmentSurface::Outer)))
            continue;
        const std::vector<geo::Index> c = m.faceCorners(f);
        if (c.size() != 3)
            continue;
        const glm::dvec3 a = m.position(m.cornerPoint(c[0])), b = m.position(m.cornerPoint(c[1])), d = m.position(m.cornerPoint(c[2]));
        // Closest point by projecting onto the plane and clamping into the triangle's barycentric range.
        const glm::dvec3 e1 = b - a, e2 = d - a, ap = p - a;
        const double d11 = glm::dot(e1, e1), d12 = glm::dot(e1, e2), d22 = glm::dot(e2, e2);
        const double den = d11 * d22 - d12 * d12;
        if (den <= 1e-20)
            continue;
        double v = (d22 * glm::dot(ap, e1) - d12 * glm::dot(ap, e2)) / den, w = (d11 * glm::dot(ap, e2) - d12 * glm::dot(ap, e1)) / den;
        v = std::clamp(v, 0.0, 1.0);
        w = std::clamp(w, 0.0, 1.0 - v);
        const double dist = glm::length(a + e1 * v + e2 * w - p);
        if (dist >= best)
            continue;
        best = dist;
        const glm::vec2 uv = fabric[c[0]] * float(1.0 - v - w) + fabric[c[1]] * float(v) + fabric[c[2]] * float(w);
        out = std::pair{int(panel[f]), Vec2{uv.x, uv.y}};
    }
    return out;
}

/**
 * A drawn outline flattened the way the body curves under it: the body faces inside the outline (as unrolled) are
 * unwrapped with little stretch, the outline is read off them, then turned and moved onto the unrolled outline it
 * replaces. Empty when the body under it can't be cut out.
 */
std::vector<Vec2> flattenOnBody(const std::vector<Vec3>& bodyPts, const std::vector<std::array<int, 3>>& bodyTris,
                                const std::vector<glm::dvec3>& outline, const std::vector<Vec2>& unrolled,
                                const std::function<Vec2(const glm::dvec3&)>& unroll)
{
    const size_t n = outline.size();
    auto inside = [&](const Vec2& q) {
        bool in = false;
        for (size_t i = 0, j = n - 1; i < n; j = i++)
            if ((unrolled[i][1] > q[1]) != (unrolled[j][1] > q[1]) &&
                q[0] < (unrolled[j][0] - unrolled[i][0]) * (q[1] - unrolled[i][1]) / (unrolled[j][1] - unrolled[i][1]) + unrolled[i][0])
                in = !in;
        return in;
    };
    auto pos = [&](int v) { return glm::dvec3(bodyPts[size_t(v)][0], bodyPts[size_t(v)][1], bodyPts[size_t(v)][2]); };
    std::vector<char> in(bodyTris.size(), 0);
    int seed = -1;
    double seedD = 1e30;
    for (size_t t = 0; t < bodyTris.size(); ++t)
    {
        const glm::dvec3 c = (pos(bodyTris[t][0]) + pos(bodyTris[t][1]) + pos(bodyTris[t][2])) / 3.0;
        if (!inside(unroll(c)))
            continue;
        in[t] = 1;
        // The patch is the piece of body under the drawn points, not an arm that unrolls to the same place behind it.
        if (const double d = glm::length(c - outline[0]); d < seedD)
            seedD = d, seed = int(t);
    }
    if (seed < 0)
        return {};
    std::map<std::pair<int, int>, std::vector<int>> byEdge;
    for (size_t t = 0; t < bodyTris.size(); ++t)
        if (in[t])
            for (int k = 0; k < 3; ++k)
                byEdge[std::minmax(bodyTris[t][size_t(k)], bodyTris[t][size_t((k + 1) % 3)])].push_back(int(t));
    std::vector<int> patch{seed}, stack{seed};
    std::vector<char> seen(bodyTris.size(), 0);
    seen[size_t(seed)] = 1;
    while (!stack.empty())
    {
        const int t = stack.back();
        stack.pop_back();
        for (int k = 0; k < 3; ++k)
            for (int o : byEdge[std::minmax(bodyTris[size_t(t)][size_t(k)], bodyTris[size_t(t)][size_t((k + 1) % 3)])])
                if (!seen[size_t(o)])
                {
                    seen[size_t(o)] = 1;
                    patch.push_back(o);
                    stack.push_back(o);
                }
    }
    if (patch.size() < 4)
        return {};

    std::map<int, geo::Index> local;
    std::vector<glm::dvec3> pts;
    std::vector<geo::Index> sizes(patch.size(), 3), idx;
    for (int t : patch)
        for (int v : bodyTris[size_t(t)])
        {
            auto [it, fresh] = local.emplace(v, geo::Index(pts.size()));
            if (fresh)
                pts.push_back(pos(v));
            idx.push_back(it->second);
        }
    const geo::Mesh mesh = geo::Mesh::fromPolygons(pts, sizes, idx);
    std::vector<uv::Chart> charts = uv::buildCharts(mesh);
    if (charts.size() != 1 || charts[0].euler() != 1)
        return {};
    uv::Chart& chart = charts[0];
    if (!uv::lscm(chart, false))
        return {};
    uv::slim(chart, 30, false);
    uv::matchSurfaceArea(chart);
    std::vector<glm::dvec2> pointUv(pts.size());
    for (size_t v = 0; v < chart.vertexPoint.size(); ++v)
        pointUv[chart.vertexPoint[v]] = chart.uv[v];

    // Each drawn point takes the flat position of the closest point on the patch.
    std::vector<glm::dvec2> flat(n);
    for (size_t i = 0; i < n; ++i)
    {
        double best = 1e30;
        for (size_t t = 0; t < patch.size(); ++t)
        {
            const geo::Index ia = idx[3 * t], ib = idx[3 * t + 1], ic = idx[3 * t + 2];
            const glm::dvec3 a = pts[ia], e1 = pts[ib] - a, e2 = pts[ic] - a, ap = outline[i] - a;
            const double d11 = glm::dot(e1, e1), d12 = glm::dot(e1, e2), d22 = glm::dot(e2, e2), den = d11 * d22 - d12 * d12;
            if (den <= 1e-20)
                continue;
            double v = (d22 * glm::dot(ap, e1) - d12 * glm::dot(ap, e2)) / den, w = (d11 * glm::dot(ap, e2) - d12 * glm::dot(ap, e1)) / den;
            v = std::clamp(v, 0.0, 1.0);
            w = std::clamp(w, 0.0, 1.0 - v);
            if (const double d = glm::length(a + e1 * v + e2 * w - outline[i]); d < best)
            {
                best = d;
                flat[i] = pointUv[ia] * (1.0 - v - w) + pointUv[ib] * v + pointUv[ic] * w;
            }
        }
    }

    // Fit onto the unrolled outline: same winding, then the rotation and shift that best overlay it.
    auto area = [](const auto& poly, auto x, auto y) {
        double s = 0.0;
        for (size_t i = 0; i < poly.size(); ++i)
            s += double(x(poly[i])) * double(y(poly[(i + 1) % poly.size()])) - double(x(poly[(i + 1) % poly.size()])) * double(y(poly[i]));
        return s;
    };
    const double flatArea = area(flat, [](const glm::dvec2& p) { return p.x; }, [](const glm::dvec2& p) { return p.y; });
    const double unrolledArea = area(unrolled, [](const Vec2& p) { return p[0]; }, [](const Vec2& p) { return p[1]; });
    if (flatArea * unrolledArea < 0.0)
        for (glm::dvec2& p : flat)
            p.x = -p.x;
    glm::dvec2 cf(0.0), cu(0.0);
    for (size_t i = 0; i < n; ++i)
    {
        cf += flat[i] / double(n);
        cu += glm::dvec2(unrolled[i][0], unrolled[i][1]) / double(n);
    }
    double dotSum = 0.0, crossSum = 0.0;
    for (size_t i = 0; i < n; ++i)
    {
        const glm::dvec2 f = flat[i] - cf, u = glm::dvec2(unrolled[i][0], unrolled[i][1]) - cu;
        dotSum += f.x * u.x + f.y * u.y;
        crossSum += f.x * u.y - f.y * u.x;
    }
    const double ang = std::atan2(crossSum, dotSum), c = std::cos(ang), s = std::sin(ang);
    std::vector<Vec2> out(n);
    for (size_t i = 0; i < n; ++i)
    {
        const glm::dvec2 f = flat[i] - cf;
        out[i] = {float(cu.x + c * f.x - s * f.y), float(cu.y + s * f.x + c * f.y)};
    }
    return out;
}

/** A surface point of the drape for placing a trim: where (garment space cm) and which way the cloth faces. */
struct TrimFrame
{
    glm::dvec3 at{0.0};
    glm::dvec3 normal{0.0, 0.0, 1.0};
};

/** The nearest outer face of the drape to p (cm), as a trim frame. */
TrimFrame frameNear(const geo::Mesh& d, const glm::dvec3& p)
{
    TrimFrame out{p, {0.0, 0.0, 1.0}};
    double best = 1e30;
    const std::span<const int32_t> side = d.faceAttrs().get<int32_t>("side");
    for (geo::Index f = 0; f < d.faceCapacity(); ++f)
    {
        if (!d.faceValid(f) || (!side.empty() && side[f] != int32_t(GarmentSurface::Outer)))
            continue;
        const std::vector<geo::Index> fp = d.facePoints(f);
        if (fp.size() != 3)
            continue;
        const glm::dvec3 o = d.position(fp[0]), e1 = d.position(fp[1]) - o, e2 = d.position(fp[2]) - o, rel = p - o;
        const double d11 = glm::dot(e1, e1), d12 = glm::dot(e1, e2), d22 = glm::dot(e2, e2), den = d11 * d22 - d12 * d12;
        if (den <= 1e-20)
            continue;
        double v = (d22 * glm::dot(rel, e1) - d12 * glm::dot(rel, e2)) / den, w = (d11 * glm::dot(rel, e2) - d12 * glm::dot(rel, e1)) / den;
        v = std::clamp(v, 0.0, 1.0);
        w = std::clamp(w, 0.0, 1.0 - v);
        const glm::dvec3 at = o + e1 * v + e2 * w;
        if (const double dist = glm::length(at - p); dist < best)
        {
            best = dist;
            out = {at, glm::normalize(glm::cross(e1, e2))};
        }
    }
    return out;
}

/** Where a pattern point of a panel lies on the drape (outer side), as a trim frame; nullopt when the panel isn't there. */
std::optional<TrimFrame> frameAtPattern(const geo::Mesh& d, int panel, const Vec2& at)
{
    const std::span<const int32_t> owner = d.faceAttrs().get<int32_t>("panel");
    const std::span<const int32_t> side = d.faceAttrs().get<int32_t>("side");
    const std::span<const glm::vec2> fabric = d.cornerAttrs().get<glm::vec2>(geo::attr::uvName(1));
    if (owner.empty() || fabric.empty())
        return std::nullopt;
    std::optional<TrimFrame> out;
    double best = 1e30;
    const glm::dvec2 q(at[0], at[1]);
    for (geo::Index f = 0; f < d.faceCapacity(); ++f)
    {
        if (!d.faceValid(f) || owner[f] != panel || (!side.empty() && side[f] != int32_t(GarmentSurface::Outer)))
            continue;
        const std::vector<geo::Index> c = d.faceCorners(f);
        if (c.size() != 3)
            continue;
        const glm::dvec2 a(fabric[c[0]]), e1 = glm::dvec2(fabric[c[1]]) - a, e2 = glm::dvec2(fabric[c[2]]) - a, rel = q - a;
        const double den = e1.x * e2.y - e2.x * e1.y;
        if (std::abs(den) < 1e-14)
            continue;
        double v = (rel.x * e2.y - e2.x * rel.y) / den, w = (e1.x * rel.y - rel.x * e1.y) / den;
        // Outside the triangle by how far, in pattern metres; inside scores zero.
        const double off = std::max({0.0, -v, -w, v + w - 1.0});
        if (off >= best)
            continue;
        best = off;
        v = std::clamp(v, 0.0, 1.0);
        w = std::clamp(w, 0.0, 1.0 - v);
        const glm::dvec3 p0 = d.position(d.cornerPoint(c[0])), p1 = d.position(d.cornerPoint(c[1])), p2 = d.position(d.cornerPoint(c[2]));
        out = TrimFrame{p0 + (p1 - p0) * v + (p2 - p0) * w, glm::normalize(glm::cross(p1 - p0, p2 - p0))};
    }
    return out;
}

/** frameAtPattern for many lookups: each panel's outer triangles bucketed in a grid over the flat pattern. */
class PatternLocator
{
public:
    explicit PatternLocator(const geo::Mesh& d) : mMesh(d)
    {
        const std::span<const int32_t> owner = d.faceAttrs().get<int32_t>("panel");
        const std::span<const int32_t> side = d.faceAttrs().get<int32_t>("side");
        const std::span<const glm::vec2> fabric = d.cornerAttrs().get<glm::vec2>(geo::attr::uvName(1));
        if (owner.empty() || fabric.empty())
            return;
        for (geo::Index f = 0; f < d.faceCapacity(); ++f)
        {
            if (!d.faceValid(f) || (!side.empty() && side[f] != int32_t(GarmentSurface::Outer)))
                continue;
            const std::vector<geo::Index> c = d.faceCorners(f);
            if (c.size() != 3)
                continue;
            mPanels[owner[f]].tris.push_back({{glm::dvec2(fabric[c[0]]), glm::dvec2(fabric[c[1]]), glm::dvec2(fabric[c[2]])},
                                              {d.cornerPoint(c[0]), d.cornerPoint(c[1]), d.cornerPoint(c[2])}});
        }
        for (auto& [panel, grid] : mPanels)
            grid.build();
    }

    std::optional<TrimFrame> at(int panel, const Vec2& p) const
    {
        const auto it = mPanels.find(panel);
        if (it == mPanels.end())
            return std::nullopt;
        const Grid& g = it->second;
        const glm::dvec2 q(p[0], p[1]);
        const int cx = std::clamp(int((q.x - g.lo.x) / g.cell), 0, g.nx - 1), cy = std::clamp(int((q.y - g.lo.y) / g.cell), 0, g.ny - 1);
        Hit best;
        for (int y = std::max(0, cy - 1); y <= std::min(g.ny - 1, cy + 1); ++y)
            for (int x = std::max(0, cx - 1); x <= std::min(g.nx - 1, cx + 1); ++x)
                for (int t : g.cells[size_t(y * g.nx + x)])
                    test(g.tris[size_t(t)], q, best);
        // Off the panel or in a sparse corner: every triangle of it.
        if (best.off > 0.0)
            for (const Tri& t : g.tris)
                test(t, q, best);
        if (!best.tri)
            return std::nullopt;
        const glm::dvec3 p0 = mMesh.position(best.tri->pt[0]), p1 = mMesh.position(best.tri->pt[1]), p2 = mMesh.position(best.tri->pt[2]);
        return TrimFrame{p0 + (p1 - p0) * best.v + (p2 - p0) * best.w, glm::normalize(glm::cross(p1 - p0, p2 - p0))};
    }

private:
    struct Tri
    {
        std::array<glm::dvec2, 3> uv;
        std::array<geo::Index, 3> pt;
    };
    struct Hit
    {
        const Tri* tri = nullptr;
        double off = 1e30, v = 0.0, w = 0.0;
    };
    struct Grid
    {
        std::vector<Tri> tris;
        glm::dvec2 lo{0.0};
        double cell = 1.0;
        int nx = 1, ny = 1;
        std::vector<std::vector<int>> cells;

        void build()
        {
            glm::dvec2 hi(-1e30);
            lo = glm::dvec2(1e30);
            double area = 0.0;
            for (const Tri& t : tris)
            {
                for (const glm::dvec2& u : t.uv)
                {
                    lo = glm::min(lo, u);
                    hi = glm::max(hi, u);
                }
                const glm::dvec2 e1 = t.uv[1] - t.uv[0], e2 = t.uv[2] - t.uv[0];
                area += std::abs(e1.x * e2.y - e2.x * e1.y) * 0.5;
            }
            cell = std::max(1e-5, 2.0 * std::sqrt(area / std::max<size_t>(1, tris.size())));
            nx = std::clamp(int((hi.x - lo.x) / cell) + 1, 1, 1024);
            ny = std::clamp(int((hi.y - lo.y) / cell) + 1, 1, 1024);
            cells.assign(size_t(nx * ny), {});
            for (size_t k = 0; k < tris.size(); ++k)
            {
                glm::dvec2 a = glm::min(glm::min(tris[k].uv[0], tris[k].uv[1]), tris[k].uv[2]);
                glm::dvec2 b = glm::max(glm::max(tris[k].uv[0], tris[k].uv[1]), tris[k].uv[2]);
                for (int y = std::clamp(int((a.y - lo.y) / cell), 0, ny - 1); y <= std::clamp(int((b.y - lo.y) / cell), 0, ny - 1); ++y)
                    for (int x = std::clamp(int((a.x - lo.x) / cell), 0, nx - 1); x <= std::clamp(int((b.x - lo.x) / cell), 0, nx - 1); ++x)
                        cells[size_t(y * nx + x)].push_back(int(k));
            }
        }
    };

    static void test(const Tri& t, const glm::dvec2& q, Hit& best)
    {
        const glm::dvec2 e1 = t.uv[1] - t.uv[0], e2 = t.uv[2] - t.uv[0], rel = q - t.uv[0];
        const double den = e1.x * e2.y - e2.x * e1.y;
        if (std::abs(den) < 1e-14)
            return;
        double v = (rel.x * e2.y - e2.x * rel.y) / den, w = (e1.x * rel.y - rel.x * e1.y) / den;
        const double off = std::max({0.0, -v, -w, v + w - 1.0});
        if (off >= best.off)
            return;
        v = std::clamp(v, 0.0, 1.0);
        w = std::clamp(w, 0.0, 1.0 - v);
        best = {&t, off, v, w};
    }

    const geo::Mesh& mMesh;
    std::map<int, Grid> mPanels;
};

/** Builds a mesh from quads and triangles given as point lists, cm. */
geo::Mesh polyMesh(const std::vector<glm::dvec3>& pts, const std::vector<std::vector<geo::Index>>& faces)
{
    std::vector<geo::Index> sizes, idx;
    for (const auto& f : faces)
    {
        sizes.push_back(geo::Index(f.size()));
        idx.insert(idx.end(), f.begin(), f.end());
    }
    return geo::Mesh::fromPolygons(pts, sizes, idx);
}

/** A box from lo to hi, cm, faces outward. */
void addBox(std::vector<glm::dvec3>& pts, std::vector<std::vector<geo::Index>>& faces, glm::dvec3 lo, glm::dvec3 hi)
{
    const geo::Index b = geo::Index(pts.size());
    for (int k = 0; k < 8; ++k)
        pts.push_back({k & 1 ? hi.x : lo.x, k & 2 ? hi.y : lo.y, k & 4 ? hi.z : lo.z});
    for (const auto& f : std::vector<std::vector<geo::Index>>{{0, 2, 3, 1}, {4, 5, 7, 6}, {0, 1, 5, 4}, {2, 6, 7, 3}, {0, 4, 6, 2}, {1, 3, 7, 5}})
    {
        std::vector<geo::Index> q;
        for (geo::Index v : f)
            q.push_back(b + v);
        faces.push_back(q);
    }
}

/** A surface of revolution about +Z from a profile of (radius, height) points, cm, closed when the profile ends on the axis. */
void addLathe(std::vector<glm::dvec3>& pts, std::vector<std::vector<geo::Index>>& faces, const std::vector<glm::dvec2>& profile, int sides,
              bool loop)
{
    const geo::Index b = geo::Index(pts.size());
    const int rows = int(profile.size());
    for (const glm::dvec2& rz : profile)
        for (int s = 0; s < sides; ++s)
        {
            const double t = 6.283185307179586 * s / sides;
            pts.push_back({rz.x * std::cos(t), rz.x * std::sin(t), rz.y});
        }
    for (int r = 0; r + 1 < rows + (loop ? 1 : 0); ++r)
        for (int s = 0; s < sides; ++s)
        {
            const int r1 = (r + 1) % rows;
            faces.push_back({b + geo::Index(r * sides + s), b + geo::Index(r * sides + (s + 1) % sides), b + geo::Index(r1 * sides + (s + 1) % sides),
                             b + geo::Index(r1 * sides + s)});
        }
}

/** The built-in trims, modelled at the origin with +Z out of the cloth, cm. */
geo::Mesh trimShape(const std::string& shape)
{
    std::vector<glm::dvec3> pts;
    std::vector<std::vector<geo::Index>> faces;
    if (shape == "grommet")
    {
        // A ring: a small circle swept round a larger one.
        std::vector<glm::dvec2> profile;
        for (int s = 0; s < 8; ++s)
        {
            const double t = 6.283185307179586 * s / 8;
            profile.push_back({0.5 + 0.15 * std::cos(t), 0.15 + 0.15 * std::sin(t)});
        }
        addLathe(pts, faces, profile, 16, true);
    }
    else if (shape == "rivet")
    {
        std::vector<glm::dvec2> profile;
        for (int s = 0; s <= 5; ++s)
        {
            const double t = 1.5707963267948966 * s / 5;
            profile.push_back({0.45 * std::cos(t), 0.3 * std::sin(t) + 0.001});
        }
        profile.push_back({0.0, 0.301});
        addLathe(pts, faces, profile, 12, false);
    }
    else if (shape == "toggle")
    {
        // A barrel lying along X: a lathe about Z, turned onto X.
        std::vector<glm::dvec2> profile{{0.0, -1.25}, {0.35, -1.2}, {0.4, -0.9}, {0.4, 0.9}, {0.35, 1.2}, {0.0, 1.25}};
        addLathe(pts, faces, profile, 10, false);
        for (glm::dvec3& p : pts)
            p = {p.z, p.y, p.x + 0.4};
    }
    else if (shape == "buckle")
    {
        addBox(pts, faces, {-2.0, -1.5, 0.0}, {-1.6, 1.5, 0.4});
        addBox(pts, faces, {1.6, -1.5, 0.0}, {2.0, 1.5, 0.4});
        addBox(pts, faces, {-1.6, -1.5, 0.0}, {1.6, -1.1, 0.4});
        addBox(pts, faces, {-1.6, 1.1, 0.0}, {1.6, 1.5, 0.4});
        addBox(pts, faces, {-0.15, -1.1, 0.1}, {0.15, 1.1, 0.35});
    }
    else if (shape == "zipTooth")
        addBox(pts, faces, {-0.25, -0.15, 0.0}, {0.25, 0.15, 0.25});
    else if (shape == "slider")
    {
        addBox(pts, faces, {-0.6, -1.2, 0.0}, {0.6, 1.2, 0.5});
        addBox(pts, faces, {-0.35, -2.6, 0.5}, {0.35, -0.4, 0.62});
    }
    else
    {
        // A button: a flat disc with a rim.
        addLathe(pts, faces, {{0.0, 0.0}, {0.8, 0.0}, {0.8, 0.25}, {0.65, 0.3}, {0.0, 0.22}}, 20, false);
    }
    return polyMesh(pts, faces);
}

const std::vector<std::string>& trimShapes()
{
    static const std::vector<std::string> s{"button", "grommet", "rivet", "toggle", "buckle", "zipTooth", "slider"};
    return s;
}

/** The pattern point s metres along a chain of outline runs, inset metres in from the edge, with its panel. */
std::pair<int, Vec2> pointAlongSides(const Garment& gm, const std::vector<SeamSide>& sides, float s, float inset)
{
    for (const SeamSide& side : sides)
    {
        const auto& o = gm.panels[size_t(side.panel)].outline;
        const int n = int(o.size());
        for (int v = side.from; v != side.to; v = (v + 1) % n)
        {
            const Vec2 &p = o[size_t(v)], &q = o[size_t((v + 1) % n)];
            const float l = std::hypot(q[0] - p[0], q[1] - p[1]);
            if (l <= 0.f)
                continue;
            if (s <= l)
            {
                const float t = s / l;
                // Inward is left of travel round a counter-clockwise outline.
                return {side.panel, {p[0] + (q[0] - p[0]) * t - (q[1] - p[1]) / l * inset, p[1] + (q[1] - p[1]) * t + (q[0] - p[0]) / l * inset}};
            }
            s -= l;
        }
    }
    const SeamSide& last = sides.back();
    return {last.panel, gm.panels[size_t(last.panel)].outline[size_t(last.to)]};
}

/** A new mesh node holding made (drape space, cm), latched to the garment's cloth (rigidly per piece, or bending with it). Returns [mesh, binding]. */
json latchMesh(Graph& g, Node& op, const MeshPtr& made, const MeshPtr& drape, const std::string& name, bool rigid)
{
    Node* out = g.createNode(*types::find("mesh"), name, kNoNode, true);
    g.setValue({out->id(), MeshNode::kInMesh, -1}, made);
    Node* bind = g.createNode(*types::find(kBindingType), {}, kNoNode, true);
    g.setValue({bind->id(), plugIndex(*bind, "restMesh")}, made);
    g.setValue({bind->id(), plugIndex(*bind, "bindSim")}, drape);
    g.setValue({bind->id(), plugIndex(*bind, "rigid")}, rigid);
    g.connect({op.id(), plugIndex(op, "outMesh")}, {bind->id(), plugIndex(*bind, "simMesh")});
    g.connect({bind->id(), plugIndex(*bind, "outMesh")}, {out->id(), MeshNode::kInMesh});
    return json::array({out->path(), bind->path()});
}

/**
 * Copies of a trim, each standing on the cloth at a frame with its +Z out of the cloth and +Y upright, latched rigidly to
 * the cloth. along (when set) turns each copy's +Y to that direction instead. Returns [mesh, binding].
 */
json placeTrims(Graph& g, Node& op, const geo::Mesh& trim, const std::vector<TrimFrame>& frames, const MeshPtr& drape, std::string name,
                double spinDegrees, const std::vector<glm::dvec3>& along = {}, double scale = 1.0)
{
    std::vector<glm::dvec3> all;
    std::vector<geo::Index> sizes, idx;
    const double spin = spinDegrees * std::numbers::pi / 180.0;
    for (size_t k = 0; k < frames.size(); ++k)
    {
        const glm::dvec3 nrm = frames[k].normal;
        glm::dvec3 up = k < along.size() ? along[k] : glm::dvec3(0.0, 1.0, 0.0);
        up -= nrm * glm::dot(up, nrm);
        if (glm::length(up) < 1e-6)
            up = glm::dvec3(0.0, 0.0, 1.0) - nrm * nrm.z;
        up = glm::normalize(up);
        const glm::dvec3 side = glm::cross(up, nrm);
        const glm::dvec3 ax = side * std::cos(spin) + up * std::sin(spin), ay = up * std::cos(spin) - side * std::sin(spin);
        std::vector<geo::Index> remap(trim.pointCapacity(), geo::kInvalid);
        for (geo::Index pi = 0; pi < trim.pointCapacity(); ++pi)
        {
            const glm::dvec3 l = trim.position(pi) * scale;
            remap[pi] = geo::Index(all.size());
            all.push_back(frames[k].at + ax * l.x + ay * l.y + nrm * l.z);
        }
        for (geo::Index f = 0; f < trim.faceCapacity(); ++f)
        {
            if (!trim.faceValid(f))
                continue;
            const std::vector<geo::Index> fp = trim.facePoints(f);
            sizes.push_back(geo::Index(fp.size()));
            for (geo::Index v : fp)
                idx.push_back(remap[v]);
        }
    }
    return latchMesh(g, op, std::make_shared<geo::Mesh>(geo::Mesh::fromPolygons(all, sizes, idx)), drape, Graph::validName(name) ? name : "Trims", true);
}

/**
 * Thread along chains of outline runs, laid out on the flat pattern (rows going inward from insetCm) and lifted onto the
 * drape, latched to the cloth so it bends with it. Returns [mesh, binding, stitches].
 */
json stitchAlong(Graph& g, Node& op, const Garment& gm, const std::vector<std::vector<SeamSide>>& chains, const MeshPtr& drape,
                 curve::StitchOptions o, double insetCm, const std::string& name)
{
    const PatternLocator locate(*drape);
    o.align = curve::StitchAlign::Left;
    o.offset += insetCm;
    geo::Mesh all;
    int count = 0;
    for (const std::vector<SeamSide>& sides : chains)
    {
        const double len = double(seamSideLength(gm, sides)) * kCmPerM;
        const auto surface = [&](double s, double x) -> std::optional<curve::SurfacePoint> {
            const auto [panel, at] = pointAlongSides(gm, sides, float(std::min(s, len) / kCmPerM), float(x / kCmPerM));
            const std::optional<TrimFrame> f = locate.at(panel, at);
            if (!f)
                return std::nullopt;
            return curve::SurfacePoint{f->at, f->normal};
        };
        curve::StitchResult r = curve::buildStitches(len, false, surface, o);
        count += r.stitches;
        all.append(r.mesh);
    }
    if (count == 0)
        throw CommandError("tailorAddStitching: no stitches fit on those edges");
    json out = latchMesh(g, op, std::make_shared<geo::Mesh>(std::move(all)), drape, Graph::validName(name) ? name : "Stitching", false);
    out.push_back(count);
    return out;
}

/** Trims at pattern points, placed where the drape has them now. */
json trimsAtPattern(Graph& g, Node& op, const std::string& shape, const std::vector<std::pair<int, Vec2>>& at, const std::string& name,
                    const char* cmd, const std::vector<std::pair<int, Vec2>>& towards = {})
{
    const MeshPtr drape = drapedMesh(g, op, cmd);
    std::vector<TrimFrame> frames;
    std::vector<glm::dvec3> along;
    for (size_t k = 0; k < at.size(); ++k)
    {
        const auto f = frameAtPattern(*drape, at[k].first, at[k].second);
        if (!f)
            continue;
        frames.push_back(*f);
        if (k < towards.size())
            if (const auto t = frameAtPattern(*drape, towards[k].first, towards[k].second))
                along.push_back(t->at - f->at);
    }
    if (frames.empty())
        throw CommandError(std::string(cmd) + ": nowhere on the drape to put the " + shape + "s");
    if (along.size() != frames.size())
        along.clear();
    return placeTrims(g, op, trimShape(shape), frames, drape, name, 0.0, along);
}

/**
 * Sends the drape as a Chaos Cloth Asset: the outer sheet's triangles over the flat pattern (a 2D point per panel and
 * pattern position) and over the welded drape (Unreal space), the panel per triangle and max distance per drape point.
 */
json sendClothAsset(Graph& g, Node& op, link::RpcClient& client, const std::string& path, const Args& a)
{
    const MeshPtr drape = drapedMesh(g, op, "tailorSendToUnreal");
    auto measured = std::make_shared<geo::Mesh>(*drape);
    if (const std::optional<PlugRef> src = g.source({op.id(), plugIndex(op, "colliderMesh")}))
        if (const Node* collider = g.node(src->node))
            writeMaxDistance(g, *collider, *measured);
    const geo::Mesh& m = *measured;
    const glm::dmat4 world = garmentWorld(g, op.id());
    const std::span<const int32_t> panel = m.faceAttrs().get<int32_t>("panel");
    const std::span<const int32_t> side = m.faceAttrs().get<int32_t>("side");
    const std::span<const glm::vec2> fabric = m.cornerAttrs().get<glm::vec2>(geo::attr::uvName(1));
    const std::span<const float> dist = m.pointAttrs().get<float>("clothMaxDistance");
    if (panel.empty() || fabric.empty())
        throw CommandError("tailorSendToUnreal: the drape carries no pattern");
    json v2 = json::array(), v3 = json::array(), tris = json::array(), pattern = json::array(), maxDist = json::array(), stitches = json::array();
    std::map<geo::Index, std::vector<int>> onPoint;
    std::map<std::tuple<int, int64_t, int64_t>, int> point2;
    for (geo::Index f = 0; f < m.faceCapacity(); ++f)
    {
        if (!m.faceValid(f) || (!side.empty() && side[f] != int32_t(GarmentSurface::Outer)))
            continue;
        const std::vector<geo::Index> c = m.faceCorners(f);
        if (c.size() != 3)
            continue;
        json tri = json::array();
        for (geo::Index corner : c)
        {
            // A point per panel and pattern position, with where the drape has it: welded seams split back into each
            // panel's own points, stitched together below.
            const geo::Index p = m.cornerPoint(corner);
            const glm::vec2 uv = fabric[corner];
            const auto key = std::make_tuple(int(panel[f]), int64_t(std::llround(uv.x * 1e5)), int64_t(std::llround(uv.y * 1e5)));
            const auto [it2, new2] = point2.emplace(key, int(point2.size()));
            if (new2)
            {
                v2.push_back({double(uv.x) * kCmPerM, double(uv.y) * kCmPerM});
                const math::Vector u = math::toUnreal(math::Vector(glm::dvec3(world * glm::dvec4(m.position(p), 1.0))));
                v3.push_back({u.x, u.y, u.z});
                maxDist.push_back(p < dist.size() ? double(dist[p]) : 0.0);
                onPoint[p].push_back(it2->second);
            }
            tri.push_back(it2->second);
        }
        tris.push_back(tri);
        pattern.push_back(panel[f]);
    }
    for (const auto& [p, list] : onPoint)
        for (size_t k = 1; k < list.size(); ++k)
            stitches.push_back({list[0], list[k]});
    const Garment gm = readGarment(op);
    const Fabric& fab = gm.fabrics.empty() ? *findFabric("Cotton") : gm.fabrics.front();
    json params = {{"contentPath", path}, {"vertices2D", v2}, {"vertices3D", v3}, {"triangles", tris}, {"pattern", pattern},
                   {"stitches", stitches}, {"maxDistance", maxDist}, {"config", chaosConfig(fab)}, {"save", a.get<bool>("save")}};
    if (a.has("skeletalMesh"))
        params["skeletalMesh"] = a.get<std::string>("skeletalMesh");
    if (a.has("material"))
        params["material"] = a.get<std::string>("material");
    const link::RpcResult r = client.call("createClothAsset", params, link::importCallOptions());
    if (!r.ok)
        throw CommandError("tailorSendToUnreal: building the cloth asset in Unreal failed: " + r.error);
    return {{"asset", r.result.value("asset", path)}, {"cloth", r.result}};
}

json checkGarment(Graph& g, Node& op)
{
    const Garment gm = readGarment(op);
    std::optional<Body> body;
    if (const std::optional<PlugRef> src = g.source({op.id(), plugIndex(op, "colliderMesh")}))
        if (Node* collider = g.node(src->node))
        {
            Vec3 feet;
            body = measureCollider(g, *collider, feet);
        }
    const std::vector<PatternIssue> issues = checkPattern(gm, body ? &*body : nullptr);
    const std::vector<PlugRef> fed = g.destinations({op.id(), plugIndex(op, "outMesh")});
    const Node* mesh = fed.empty() ? &op : g.node(fed.front().node);
    diagnostics::clear("tailor", mesh->id());
    json out = json::array();
    for (const PatternIssue& i : issues)
    {
        diagnostics::Issue d;
        d.source = "tailor";
        d.severity = i.level == PatternIssue::Error ? diagnostics::Severity::Error
                     : i.level == PatternIssue::Warning ? diagnostics::Severity::Warning
                                                         : diagnostics::Severity::Info;
        d.target = mesh->path();
        d.message = i.message + ". " + i.tip;
        diagnostics::report(d);
        json j = {{"level", i.level == PatternIssue::Error ? "error" : i.level == PatternIssue::Warning ? "warning" : "info"},
                  {"message", i.message},
                  {"tip", i.tip}};
        if (i.panel >= 0)
            j["panel"] = gm.panels[size_t(i.panel)].name;
        if (i.seam >= 0)
            j["seam"] = i.seam;
        out.push_back(j);
    }
    return out;
}

/** A tailorGarment node holding gm, fed by the collider flag (or selection) and feeding a new mesh. Returns [mesh, garment]. */
json createGarment(const Garment& gm, const std::string& block, const Args& a, const char* cmd, const std::string& meshName = {})
{
    const std::string fabric = a.has("fabric") ? a.get<std::string>("fabric") : std::string();
    if (!fabric.empty())
        fabricArg(fabric, cmd);

    Graph& g = Graph::current();
    Node* collider = colliderArg(g, a);
    Node* op = g.createNode(*types::find(kGarmentType), {}, kNoNode, true);
    g.setValue({op->id(), plugIndex(*op, "garment")}, garmentToJson(gm).dump());
    g.setValue({op->id(), plugIndex(*op, "block")}, block);
    g.setValue({op->id(), plugIndex(*op, "fabric")}, fabric);
    g.setValue({op->id(), plugIndex(*op, "particleDistance")}, a.get<double>("particleDistance"));
    g.setValue({op->id(), plugIndex(*op, "simulate")}, a.get<bool>("simulate"));
    g.setValue({op->id(), plugIndex(*op, "frames")}, a.get<int64_t>("frames"));
    if (collider)
    {
        g.connect({collider->id(), MeshNode::kOutMesh}, {op->id(), plugIndex(*op, "colliderMesh")});
        g.connect({collider->id(), Transform::kWorldMatrix}, {op->id(), plugIndex(*op, "colliderMatrix")});
    }
    Node* mesh = g.createNode(*types::find("mesh"), meshName.empty() ? a.get<std::string>("name") : meshName, kNoNode, true);
    g.connect({op->id(), plugIndex(*op, "outMesh")}, {mesh->id(), MeshNode::kInMesh});
    g.select({mesh->id()});
    if (a.get<bool>("wait"))
    {
        DrapeNow now;
        g.touch(op->id());
        g.evaluate();
    }
    return json::array({mesh->path(), op->path()});
}

json meshStats(Graph& g, const Node& op)
{
    g.evaluate();
    const PlugValue v = g.value({op.id(), plugIndex(op, "outMesh")});
    const MeshPtr& m = std::get<MeshPtr>(v);
    return {{"vertices", m ? int64_t(m->numPoints()) : 0},
            {"triangles", m ? int64_t(m->numFaces()) : 0},
            {"warning", op.as<std::string>(plugIndex(op, "warning"))}};
}
} // namespace

void registerCommands(ext::Context& ctx)
{
    diagnostics::validators().add("tailor", {.doc = "Pattern checks on every garment (tailorCheck).", .run = [] {
        Graph& g = Graph::current();
        std::vector<NodeId> garments;
        for (NodeId id : g.nodes())
            if (const Node* n = g.node(id); n && n->type().isA(kGarmentType))
                garments.push_back(id);
        for (NodeId id : garments)
            if (Node* n = g.node(id))
                checkGarment(g, *n);
    }}, ctx.owner());

    ctx.registerCommand({
        .name = "tailorCreateBlock",
        .doc = "Start a garment from a parametric block (tee, tunic, cloak, ...) and drape it onto a collider mesh.\n\n"
               "Makes a tailorGarment node feeding a new mesh. With no collider, the first selected mesh is used. Returns [mesh, garment].",
        .flags = {{"block", "b", T::String, "tee", "The block to start from.", blockNames()},
                  nodeFlag({"collider", "c", T::String, json(), "The body mesh to drape onto; empty for none."}, "mesh"),
                  {"name", "n", T::String, "Garment", "Name of the new mesh."},
                  {"fabric", "fab", T::String, json(), "Fabric preset; the block's own when left out.", presetChoices()},
                  {"particleDistance", "pd", T::Float, 2.0, "Target cloth edge length, cm.", {}, {}, 0.1, 50.0},
                  {"simulate", "sim", T::Bool, true, "Drape it; off leaves the panels where the block placed them."},
                  {"frames", "fr", T::Int, int64_t(90), "Solver frames run to settle the drape.", {}, {}, 0.0, 10000.0},
                  {"wait", "w", T::Bool, false, "Drape before returning instead of in the background."},
                  {"fit", "ft", T::Bool, true, "Size the block to the collider's measurements and stand it where the collider stands."},
                  {"neck", "nk", T::Float, json(), "Height of the base of the neck above the collider's feet, cm, when the measured one is wrong."},
                  {"params", "pr", T::Any, json::object(),
                   "Block parameters by name, metres and 0/1 for switches, e.g. {'sleeveStyle': 9, 'sleeveLength': 0.6}. "
                   "Sleeve styles: 0 set-in, 1 cuffed, 2 bishop, 3 bell, 4 puff, 5 cap, 6 flutter, 7 leg-of-mutton, 8 poet, "
                   "9 drop-shoulder, 10 batwing, 11 raglan, 12 marie, 13 flounce, 14 slit, 15 tiered, 16 petal, 17 cape."}},
        .run = [](const Args& a) -> json {
            Garment gm;
            std::string error;
            Body body;
            Vec3 offset{0.f, 0.f, 0.f};
            Graph& g = Graph::current();
            if (Node* collider = colliderArg(g, a); collider && a.get<bool>("fit"))
                body = measureCollider(g, *collider, offset, a.has("neck") ? float(a.get<double>("neck") / kCmPerM) : 0.f);
            else if (a.has("neck"))
                body.neckHeight = float(a.get<double>("neck") / kCmPerM);
            BlockParamValues values;
            if (const json pj = a.flags.value("params", json::object()); pj.is_object())
                for (const auto& [k, v] : pj.items())
                    if (v.is_number() || v.is_boolean())
                        values[k] = v.is_boolean() ? (v.get<bool>() ? 1.f : 0.f) : v.get<float>();
            if (!buildBlock(a.get<std::string>("block"), gm, &error, body, values))
                throw CommandError("tailorCreateBlock: " + error);
            for (Panel& p : gm.panels)
                for (int k = 0; k < 3; ++k)
                    p.origin[k] += offset[k];
            return createGarment(gm, a.get<std::string>("block"), a, "tailorCreateBlock");
        },
        .examples = {"tailorCreateBlock(block='tunic', collider='Body')"},
    });

    ctx.registerCommand({
        .name = "tailorImportGarmentCode",
        .doc = "Import a GarmentCode / ChatGarment sewing pattern (pygarment JSON) as a garment and drape it onto a collider mesh.\n\n"
               "Makes a tailorGarment node feeding a new mesh. With no collider, the first selected mesh is used. Returns [mesh, garment].",
        .flags = {{"file", "f", T::String, json(), "The pattern specification JSON."},
                  nodeFlag({"collider", "c", T::String, json(), "The body mesh to drape onto; empty for none."}, "mesh"),
                  {"name", "n", T::String, "Garment", "Name of the new mesh."},
                  {"fabric", "fab", T::String, "Cotton", "Fabric preset.", presetChoices()},
                  {"particleDistance", "pd", T::Float, 2.0, "Target cloth edge length, cm.", {}, {}, 0.1, 50.0},
                  {"simulate", "sim", T::Bool, true, "Drape it; off leaves the panels where the file placed them."},
                  {"frames", "fr", T::Int, int64_t(90), "Solver frames run to settle the drape.", {}, {}, 0.0, 10000.0},
                  {"wait", "w", T::Bool, false, "Drape before returning instead of in the background."}},
        .run = [](const Args& a) -> json {
            if (!a.has("file"))
                throw CommandError("tailorImportGarmentCode: give a file");
            Garment gm;
            std::string error;
            if (!importGarmentCodeFile(a.get<std::string>("file"), gm, &error))
                throw CommandError("tailorImportGarmentCode: " + error);
            return createGarment(gm, std::string(), a, "tailorImportGarmentCode");
        },
        .examples = {"tailorImportGarmentCode(file='C:/patterns/tee_spec.json', collider='Body')"},
    });

    ctx.registerCommand({
        .name = "tailorSimulate",
        .doc = "Drape a garment: run the solver for a number of frames from its placed panels and keep the result.\n\n"
               "Returns the draped mesh's vertex and triangle counts and any warning.",
        .positionalDoc = "garment",
        .flags = {{"frames", "fr", T::Int, json(), "Solver frames to run; the garment's own setting when left out.", {}, {}, 0.0, 10000.0},
                  {"simulate", "sim", T::Bool, true, "Off shows the panels undraped."},
                  {"wait", "w", T::Bool, false, "Drape before returning instead of in the background."}},
        .run = [](const Args& a) -> json {
            Graph& g = Graph::current();
            Node& op = garmentArg(g, a, "tailorSimulate");
            if (a.has("frames"))
                g.setValue({op.id(), plugIndex(op, "frames")}, a.get<int64_t>("frames"));
            g.setValue({op.id(), plugIndex(op, "simulate")}, a.get<bool>("simulate", true));
            std::optional<DrapeNow> now;
            if (a.get<bool>("wait"))
            {
                now.emplace();
                g.touch(op.id());
            }
            return meshStats(g, op);
        },
        .examples = {"tailorSimulate('Garment', frames=200, wait=True)"},
    });

    ctx.registerCommand({
        .name = "tailorSetFabric",
        .doc = "Set the fabric of a garment, or of some of its panels.\n\n"
               "Without panels it sets the garment's fabric preset; with them, those panels get the preset as their own fabric.",
        .positionalDoc = "garment",
        .flags = {{"fabric", "fab", T::String, "Cotton", "Fabric preset.", presetChoices()},
                  {"panel", "p", T::List, json(), "Panels by name or index; all panels' shared fabric when left out."}},
        .run = [](const Args& a) -> json {
            Graph& g = Graph::current();
            Node& op = garmentArg(g, a, "tailorSetFabric");
            const std::string name = a.get<std::string>("fabric");
            const Fabric& preset = fabricArg(name, "tailorSetFabric");
            if (!a.has("panel"))
            {
                g.setValue({op.id(), plugIndex(op, "fabric")}, name);
                return name;
            }
            Garment gm = readGarment(op);
            int index = -1;
            for (size_t k = 0; k < gm.fabrics.size(); ++k)
                if (gm.fabrics[k].name == name)
                    index = int(k);
            if (index < 0)
            {
                index = int(gm.fabrics.size());
                gm.fabrics.push_back(preset);
            }
            json changed = json::array();
            for (int p : panelsArg(gm, a, "panel", "tailorSetFabric"))
            {
                gm.panels[size_t(p)].fabric = index;
                changed.push_back(gm.panels[size_t(p)].name);
            }
            writeGarment(g, op, gm);
            return changed;
        },
        .examples = {"tailorSetFabric('Garment', fabric='Leather', panel=['Belt'])"},
    });

    ctx.registerCommand({
        .name = "tailorSew",
        .doc = "Sew two runs of panel outline together, point to point by arc length.\n\n"
               "Each side is panel:from:to (outline vertex indices, walking forward), several joined by commas for a chain. "
               "Returns the new seam's index.",
        .positionalDoc = "garment",
        .flags = {{"sideA", "a", T::String, json(), "First side, e.g. \"Front:0:4\" or \"Front:0:4,Sleeve:2:5\"."},
                  {"sideB", "b", T::String, json(), "Second side, same form."},
                  {"reversed", "r", T::Bool, false, "Walk side B backwards."},
                  {"foldAngle", "fa", T::Float, 0.0, "Rest angle across the seam, degrees; 0 lies flat.", {}, {}, -180.0, 180.0},
                  {"shrink", "sh", T::Float, 1.0, "Rest length scale across the seam.", {}, {}, 0.0, 2.0}},
        .run = [](const Args& a) -> json {
            if (!a.has("sideA") || !a.has("sideB"))
                throw CommandError("tailorSew: give sideA and sideB");
            Graph& g = Graph::current();
            Node& op = garmentArg(g, a, "tailorSew");
            Garment gm = readGarment(op);
            Seam s;
            s.a = parseSides(gm, a.get<std::string>("sideA"), "tailorSew");
            s.b = parseSides(gm, a.get<std::string>("sideB"), "tailorSew");
            s.reversed = a.get<bool>("reversed");
            s.foldAngle = float(a.get<double>("foldAngle") * std::numbers::pi / 180.0);
            s.shrink = float(a.get<double>("shrink"));
            gm.seams.push_back(std::move(s));
            writeGarment(g, op, gm);
            return int64_t(gm.seams.size() - 1);
        },
        .examples = {"tailorSew('Garment', sideA='Front:1:2', sideB='Back:3:4', reversed=True)"},
    });

    ctx.registerCommand({
        .name = "tailorMannequin",
        .doc = "Make a stand-in body with the measurements the blocks are cut for, to drape garments onto.\n\n"
               "Feet on the ground, facing +Z, in an A-pose. Blocks measure whatever they drape onto, so any size fits. Returns the new mesh.",
        .flags = {{"name", "n", T::String, "Mannequin", "Name of the new mesh."},
                  {"height", "ht", T::Float, 180.0, "Standing height, cm.", {}, {}, 100.0, 230.0},
                  {"chest", "ch", T::Float, 96.0, "Chest girth, cm.", {}, {}, 60.0, 160.0},
                  {"waist", "wa", T::Float, 82.0, "Waist girth, cm.", {}, {}, 50.0, 160.0},
                  {"hip", "hp", T::Float, 98.0, "Hip girth, cm.", {}, {}, 60.0, 170.0}},
        .run = [](const Args& a) -> json {
            Body body;
            const float s = float(a.get<double>("height")) / (body.height * float(kCmPerM));
            body.height *= s;
            body.shoulderWidth *= s;
            body.armLength *= s;
            body.inseam *= s;
            body.neck *= s;
            body.head *= s;
            body.chest = float(a.get<double>("chest") / kCmPerM);
            body.waist = float(a.get<double>("waist") / kCmPerM);
            body.hip = float(a.get<double>("hip") / kCmPerM);
            std::vector<Vec3> pts;
            std::vector<std::array<int, 3>> tris;
            mannequin(body, pts, tris);
            std::vector<glm::dvec3> positions(pts.size());
            for (size_t k = 0; k < pts.size(); ++k)
                positions[k] = glm::dvec3(pts[k][0], pts[k][1], pts[k][2]) * kCmPerM;
            std::vector<geo::Index> sizes(tris.size(), 3), idx;
            idx.reserve(tris.size() * 3);
            for (const auto& t : tris)
                for (int v : t)
                    idx.push_back(geo::Index(v));
            Graph& g = Graph::current();
            const std::string name = a.get<std::string>("name");
            Node* n = g.createNode(*types::find("mesh"), Graph::validName(name) ? name : "Mannequin", kNoNode, true);
            g.setValue({n->id(), MeshNode::kInMesh, -1},
                       MeshPtr(std::make_shared<geo::Mesh>(geo::Mesh::fromPolygons(positions, sizes, idx))));
            g.select({n->id()});
            return n->path();
        },
        .examples = {"tailorMannequin()", "tailorCreateBlock(block='cloakTunicBelt', collider=tailorMannequin())"},
    });

    ctx.registerCommand({
        .name = "tailorBake",
        .doc = "Turn a draped garment into a plain mesh for the game: the drape as it stands, seams welded and thickness "
               "applied as set on the garment, uv0 packed, uv1 the fabric pattern, and point attribute clothMaxDistance (cm from the body) as a runtime cloth mask "
               "unless clothMask is off, for garments that hold their shape with no cloth at runtime.\n\n"
               "When the collider is skinned, the new mesh is bound to the same joints and takes its weights by closest "
               "point, ready to export with the character. Returns the new mesh.",
        .positionalDoc = "garment",
        .flags = {{"name", "n", T::String, json(), "Name of the new mesh; the garment mesh's name plus _baked when left out."},
                  {"skin", "sk", T::Bool, true, "Bind to the collider's joints and copy its weights when it is skinned."},
                  {"clothMask", "cm", T::Bool, true, "Write clothMaxDistance for runtime cloth; off for a mesh that only follows the skin."},
                  {"maxInfluences", "mi", T::Int, int64_t(4), "Influences per point.", {}, {}, 1.0, 16.0},
                  {"retopo", "rt", T::Int, int64_t(0), "Quad remesh to about this many faces, seams kept as edge loops; 0 keeps the cloth triangles.",
                   {}, {}, 0.0, 200000.0},
                  {"lods", "lod", T::Int, int64_t(0), "Extra levels of detail to generate in a LOD group.", {}, {}, 0.0, 7.0},
                  {"maps", "mp", T::List, json::array(),
                   "Texture maps to bake from the drape onto the game mesh (ids from bakeMaps: normal, ao, curvature, id, ...). id "
                   "colours each panel. The drape is kept, hidden, as the bake's high."},
                  {"mapSize", "ms", T::Int, int64_t(2048), "Map width and height, texels.", {}, {}, 16.0, 16384.0},
                  {"mapFolder", "mf", T::String, "textures", "Where the maps go; relative paths are under the workspace."}},
        .run = [](const Args& a) -> json {
            Graph& g = Graph::current();
            Node& op = garmentArg(g, a, "tailorBake");
            {
                DrapeNow now;
                g.evaluate();
            }
            const PlugValue v = g.value({op.id(), plugIndex(op, "outMesh")});
            const MeshPtr* mesh = std::get_if<MeshPtr>(&v);
            if (!mesh || !*mesh || (*mesh)->empty())
                throw CommandError("tailorBake: " + op.name() + " has no mesh: " + op.as<std::string>(plugIndex(op, "warning")));

            std::string name = a.has("name") ? a.get<std::string>("name") : std::string();
            if (name.empty())
            {
                const std::vector<PlugRef> out = g.destinations({op.id(), plugIndex(op, "outMesh")});
                const Node* fed = out.empty() ? nullptr : g.node(out.front().node);
                name = (fed ? fed->name() : op.name()) + "_baked";
            }
            const std::optional<PlugRef> src = g.source({op.id(), plugIndex(op, "colliderMesh")});
            const Node* collider = src ? g.node(src->node) : nullptr;
            const json guides = seamGuides(**mesh);
            if (!Graph::validName(name))
                name = "Garment_baked";
            const bool remesh = a.get<int64_t>("retopo") > 0;
            const json maps = a.flags.value("maps", json::array());
            const bool bakeMaps = maps.is_array() && !maps.empty();
            Node* baked = g.createNode(*types::find("mesh"), remesh ? name + "_drape" : name, kNoNode, true);
            g.setValue({baked->id(), MeshNode::kInMesh, -1}, MeshPtr(std::make_shared<geo::Mesh>(**mesh)));
            std::string high;
            if (const int64_t faces = a.get<int64_t>("retopo"); faces > 0)
            {
                const std::string drape = baked->path();
                const json r = ops::execute("quadRemesh", {{"args", {drape}},
                                                           {"targetFaces", faces},
                                                           {"guidePoints", guides},
                                                           {"symmetry", "off"},
                                                           {"timeout", 120.0},
                                                           {"transferUvs", true},
                                                           {"keepSource", bakeMaps},
                                                           {"name", name}});
                baked = g.find(r.value("name", drape));
                if (!baked)
                    throw CommandError("tailorBake: the remesh left no mesh");
                if (bakeMaps)
                    high = drape;
            }
            else if (bakeMaps)
            {
                Node* h = g.createNode(*types::find("mesh"), name + "_drape", kNoNode, true);
                g.setValue({h->id(), MeshNode::kInMesh, -1}, MeshPtr(std::make_shared<geo::Mesh>(**mesh)));
                high = h->path();
            }
            if (collider && a.get<bool>("clothMask"))
            {
                g.evaluate();
                const PlugValue bv = g.value({baked->id(), MeshNode::kOutMesh});
                if (const MeshPtr* bm = std::get_if<MeshPtr>(&bv); bm && *bm)
                {
                    auto out = std::make_shared<geo::Mesh>(**bm);
                    writeMaxDistance(g, *collider, *out);
                    g.setValue({baked->id(), MeshNode::kInMesh, -1}, MeshPtr(std::move(out)));
                }
            }

            json result = {{"mesh", baked->path()}, {"skinned", false}};
            if (collider && a.get<bool>("skin"))
            {
                json info;
                try
                {
                    info = ops::execute("skinInfo", {{"args", {collider->path()}}});
                }
                catch (const std::exception&)
                {
                    info = json();
                }
                if (info.is_object() && info.contains("influences") && !info["influences"].empty())
                {
                    json bindArgs = info["influences"];
                    bindArgs.push_back(baked->path());
                    ops::execute("skinCluster", {{"args", bindArgs}, {"bindTo", "selected"},
                                                 {"maxInfluences", a.get<int64_t>("maxInfluences")}});
                    ops::execute("copySkinWeights", {{"args", {collider->path(), baked->path()}}});
                    result["skinned"] = true;
                }
            }
            if (bakeMaps)
                result["maps"] = bakeDrapeMaps(g, high, baked->path(), name, maps, a.get<int64_t>("mapSize"), a.get<std::string>("mapFolder"));
            if (const int64_t lods = a.get<int64_t>("lods"); lods > 0)
            {
                result["lodGroup"] = ops::execute("lodGroup", {{"args", {baked->path()}}, {"generate", lods}});
                result["mesh"] = baked->path();
            }
            g.select({baked->id()});
            return result;
        },
        .examples = {"tailorBake('Outfit')", "tailorBake('Cloak', name='SM_Cloak', skin=False)", "tailorBake('Tunic', retopo=3000, lods=2)",
                     "tailorBake('Jacket', clothMask=False)"},
    });

    ctx.registerCommand({
        .name = "tailorEditPanel",
        .doc = "Change a panel: its outline points (same count), grain, particle distance, layer or name.\n\n"
               "Points are pattern space, cm, counter-clockwise. Seams keep their vertex indices. Returns the panel's name.",
        .positionalDoc = "garment",
        .flags = {{"panel", "p", T::String, json(), "Panel by name or index."},
                  {"outline", "o", T::List, json(), "Outline points [[x, y], ...], cm; the count must match."},
                  {"grainAngle", "ga", T::Float, json(), "Warp direction in pattern space, degrees."},
                  {"particleDistance", "pd", T::Float, json(), "This panel's cloth edge length, cm.", {}, {}, 0.1, 50.0},
                  {"layer", "l", T::Int, json(), "Layer: higher layers sit outside lower ones."},
                  {"rename", "rn", T::String, json(), "New name."}},
        .run = [](const Args& a) -> json {
            Graph& g = Graph::current();
            Node& op = garmentArg(g, a, "tailorEditPanel");
            Garment gm = readGarment(op);
            Panel& p = gm.panels[size_t(panelIndex(gm, a.flags.value("panel", json()), "tailorEditPanel"))];
            if (a.has("outline"))
            {
                const std::vector<Vec2> pts = pointsArg(a.flags["outline"], "tailorEditPanel");
                if (pts.size() != p.outline.size())
                    throw CommandError("tailorEditPanel: the outline has " + std::to_string(p.outline.size()) + " points, not " +
                                       std::to_string(pts.size()) + "; use tailorInsertPoint or tailorDeletePoint to change the count");
                setOutline(gm, int(&p - gm.panels.data()), pts);
            }
            if (a.has("grainAngle"))
                p.grainAngle = float(a.get<double>("grainAngle") * std::numbers::pi / 180.0);
            if (a.has("particleDistance"))
                p.particleDistance = float(a.get<double>("particleDistance") / kCmPerM);
            if (a.has("layer"))
                p.layer = int(a.get<int64_t>("layer"));
            if (a.has("rename"))
            {
                const std::string name = a.get<std::string>("rename");
                for (const Panel& other : gm.panels)
                    if (&other != &p && other.name == name)
                        throw CommandError("tailorEditPanel: there already is a panel " + name);
                p.name = name;
            }
            const std::string name = p.name;
            writeGarment(g, op, gm);
            return name;
        },
        .examples = {"tailorEditPanel('Garment', panel='Front', grainAngle=90)"},
    });

    ctx.registerCommand({
        .name = "tailorInsertPoint",
        .doc = "Insert an outline point into a panel before point index, at a position in pattern space (cm). Seams keep "
               "sewing the same stretch of outline. Returns the new point's index.",
        .positionalDoc = "garment",
        .flags = {{"panel", "p", T::String, json(), "Panel by name or index."},
                  {"index", "i", T::Int, json(), "The new point's index; the old point there moves up one."},
                  {"position", "pos", T::List, json(), "[x, y], cm."}},
        .run = [](const Args& a) -> json {
            Graph& g = Graph::current();
            Node& op = garmentArg(g, a, "tailorInsertPoint");
            Garment gm = readGarment(op);
            const int pi = panelIndex(gm, a.flags.value("panel", json()), "tailorInsertPoint");
            Panel& p = gm.panels[size_t(pi)];
            const int n = int(p.outline.size());
            const int k = int(a.get<int64_t>("index"));
            if (k < 0 || k > n)
                throw CommandError("tailorInsertPoint: index out of 0.." + std::to_string(n));
            const std::vector<Vec2> pos = pointsArg(json::array({a.flags.value("position", json())}), "tailorInsertPoint");
            insertOutlinePoint(gm, pi, k, pos.front());
            writeGarment(g, op, gm);
            return int64_t(k);
        },
        .examples = {"tailorInsertPoint('Garment', panel='Front', index=2, position=[10, 40])"},
    });

    ctx.registerCommand({
        .name = "tailorDeletePoint",
        .doc = "Delete an outline point from a panel (it keeps at least 3). Seams ending there end at the next point; a seam "
               "left without length is removed. Returns the number of seams removed.",
        .positionalDoc = "garment",
        .flags = {{"panel", "p", T::String, json(), "Panel by name or index."}, {"index", "i", T::Int, json(), "The point to delete."}},
        .run = [](const Args& a) -> json {
            Graph& g = Graph::current();
            Node& op = garmentArg(g, a, "tailorDeletePoint");
            Garment gm = readGarment(op);
            const int pi = panelIndex(gm, a.flags.value("panel", json()), "tailorDeletePoint");
            Panel& p = gm.panels[size_t(pi)];
            const int n = int(p.outline.size());
            const int k = int(a.get<int64_t>("index"));
            if (k < 0 || k >= n)
                throw CommandError("tailorDeletePoint: index out of 0.." + std::to_string(n - 1));
            if (n <= 3)
                throw CommandError("tailorDeletePoint: a panel needs at least 3 points");
            const int removed = deleteOutlinePoint(gm, pi, k);
            writeGarment(g, op, gm);
            return int64_t(removed);
        },
        .examples = {"tailorDeletePoint('Garment', panel='Front', index=2)"},
    });

    ctx.registerCommand({
        .name = "tailorAddPanel",
        .doc = "Add a rectangular panel, or one with a drawn outline, placed flat in front of the garment's other panels, ready to reshape and sew.\n\n"
               "Returns the new panel's name.",
        .positionalDoc = "garment",
        .flags = {{"name", "n", T::String, "Panel", "Name; made unique."},
                  {"width", "w", T::Float, 40.0, "cm.", {}, {}, 1.0, 1000.0},
                  {"height", "ht", T::Float, 60.0, "cm.", {}, {}, 1.0, 1000.0},
                  {"layer", "l", T::Int, int64_t(0), "Layer: higher layers sit outside lower ones."},
                  {"outline", "o", T::List, json(), "Outline points [[x, y], ...], cm, instead of a rectangle; either winding."}},
        .run = [](const Args& a) -> json {
            Graph& g = Graph::current();
            Node& op = garmentArg(g, a, "tailorAddPanel");
            Garment gm = readGarment(op);
            Panel p;
            std::string base = a.get<std::string>("name");
            p.name = base;
            for (int k = 2; std::any_of(gm.panels.begin(), gm.panels.end(), [&](const Panel& o) { return o.name == p.name; }); ++k)
                p.name = base + std::to_string(k);
            float w = float(a.get<double>("width") / kCmPerM), h = float(a.get<double>("height") / kCmPerM);
            p.outline = {{0.f, 0.f}, {w, 0.f}, {w, h}, {0.f, h}};
            if (a.has("outline"))
            {
                p.outline = pointsArg(a.flags["outline"], "tailorAddPanel");
                if (p.outline.size() < 3)
                    throw CommandError("tailorAddPanel: an outline needs at least 3 points");
                float area = 0.f, x0 = 1e30f, y0 = 1e30f, x1 = -1e30f, y1 = -1e30f;
                for (size_t i = 0; i < p.outline.size(); ++i)
                {
                    const Vec2 &u = p.outline[i], &v = p.outline[(i + 1) % p.outline.size()];
                    area += u[0] * v[1] - v[0] * u[1];
                    x0 = std::min(x0, u[0]), y0 = std::min(y0, u[1]), x1 = std::max(x1, u[0]), y1 = std::max(y1, u[1]);
                }
                if (area < 0.f)
                    std::reverse(p.outline.begin(), p.outline.end());
                for (Vec2& v : p.outline)
                    v = {v[0] - x0, v[1] - y0};
                w = x1 - x0;
                h = y1 - y0;
            }
            p.layer = int(a.get<int64_t>("layer"));
            // In front of everything placed so far, centred on it, facing +Z.
            float zMax = 0.f, xMid = 0.f, yMid = 1.f;
            if (!gm.panels.empty())
            {
                float x0 = 1e9f, x1 = -1e9f, y0 = 1e9f, y1 = -1e9f;
                zMax = -1e9f;
                for (const Panel& o : gm.panels)
                {
                    x0 = std::min(x0, o.origin[0]);
                    x1 = std::max(x1, o.origin[0]);
                    y0 = std::min(y0, o.origin[1]);
                    y1 = std::max(y1, o.origin[1]);
                    zMax = std::max(zMax, o.origin[2]);
                }
                xMid = 0.5f * (x0 + x1);
                yMid = 0.5f * (y0 + y1);
            }
            p.origin = {xMid - 0.5f * w, yMid - 0.5f * h, zMax + 0.1f};
            p.right = {1.f, 0.f, 0.f};
            p.up = {0.f, 1.f, 0.f};
            const std::string name = p.name;
            gm.panels.push_back(std::move(p));
            writeGarment(g, op, gm);
            return name;
        },
        .examples = {"tailorAddPanel('Garment', name='Pocket', width=15, height=18)"},
    });

    ctx.registerCommand({
        .name = "tailorDeletePanel",
        .doc = "Delete panels and every seam that sews them. Returns the number of seams removed.",
        .positionalDoc = "garment",
        .flags = {{"panel", "p", T::List, json(), "Panels by name or index."}},
        .run = [](const Args& a) -> json {
            Graph& g = Graph::current();
            Node& op = garmentArg(g, a, "tailorDeletePanel");
            Garment gm = readGarment(op);
            if (!a.has("panel") || a.flags["panel"].empty())
                throw CommandError("tailorDeletePanel: name the panels");
            const int removed = removePanels(gm, panelsArg(gm, a, "panel", "tailorDeletePanel"));
            writeGarment(g, op, gm);
            return int64_t(removed);
        },
        .examples = {"tailorDeletePanel('Garment', panel=['Pocket'])"},
    });

    ctx.registerCommand({
        .name = "tailorUnsew",
        .doc = "Remove seams by index. Returns how many were removed.",
        .positionalDoc = "garment",
        .flags = {{"seam", "s", T::List, json(), "Seam indices, as tailorSew returned them."}},
        .run = [](const Args& a) -> json {
            Graph& g = Graph::current();
            Node& op = garmentArg(g, a, "tailorUnsew");
            Garment gm = readGarment(op);
            std::vector<int64_t> idx;
            json list = a.flags.value("seam", json::array());
            if (!list.is_array())
                list = json::array({list});
            for (const json& v : list)
                if (v.is_number_integer() && v.get<int64_t>() >= 0 && size_t(v.get<int64_t>()) < gm.seams.size())
                    idx.push_back(v.get<int64_t>());
            std::sort(idx.begin(), idx.end());
            idx.erase(std::unique(idx.begin(), idx.end()), idx.end());
            for (auto it = idx.rbegin(); it != idx.rend(); ++it)
                gm.seams.erase(gm.seams.begin() + *it);
            writeGarment(g, op, gm);
            return int64_t(idx.size());
        },
        .examples = {"tailorUnsew('Garment', seam=[3])"},
    });

    ctx.registerCommand({
        .name = "tailorAddDart",
        .doc = "Cut a dart into a panel edge: two legs `width` apart meeting at a point `depth` inside, sewn together. A seam "
               "along the edge keeps sewing it either side of the dart. A linked twin gets the mirrored dart. Returns the dart's seam index.",
        .positionalDoc = "garment",
        .flags = {{"panel", "p", T::String, json(), "Panel by name or index."},
                  {"edge", "e", T::Int, json(), "The outline edge from this point to the next."},
                  {"at", "t", T::Float, 0.5, "Where along the edge, 0 to 1.", {}, {}, 0.0, 1.0},
                  {"width", "w", T::Float, 3.0, "Dart intake at the edge, cm.", {}, {}, 0.1, 50.0},
                  {"depth", "d", T::Float, 10.0, "Leg length into the panel, cm.", {}, {}, 0.5, 100.0}},
        .run = [](const Args& a) -> json {
            Graph& g = Graph::current();
            Node& op = garmentArg(g, a, "tailorAddDart");
            Garment gm = readGarment(op);
            const int pi = panelIndex(gm, a.flags.value("panel", json()), "tailorAddDart");
            const int n = int(gm.panels[size_t(pi)].outline.size());
            const int edge = a.has("edge") ? int(a.get<int64_t>("edge")) : -1;
            if (edge < 0 || edge >= n)
                throw CommandError("tailorAddDart: edge out of 0.." + std::to_string(n - 1));
            const int seam = addDart(gm, pi, edge, float(a.get<double>("at")), float(a.get<double>("width") / kCmPerM),
                                     float(a.get<double>("depth") / kCmPerM));
            if (seam < 0)
                throw CommandError("tailorAddDart: the edge is too short for a dart that wide");
            writeGarment(g, op, gm);
            return int64_t(seam);
        },
        .examples = {"tailorAddDart('Skirt', panel='Front', edge=2, width=2.5, depth=12)"},
    });

    ctx.registerCommand({
        .name = "tailorAddNotch",
        .doc = "Mark a notch on a panel's outline at the point nearest a position (adding a point there if needed). Seams "
               "with the same number of notches on both sides sew notch to notch. Returns the notch's point index.",
        .positionalDoc = "garment",
        .flags = {{"panel", "p", T::String, json(), "Panel by name or index."}, {"position", "pos", T::List, json(), "[x, y], cm."}},
        .run = [](const Args& a) -> json {
            Graph& g = Graph::current();
            Node& op = garmentArg(g, a, "tailorAddNotch");
            Garment gm = readGarment(op);
            const int pi = panelIndex(gm, a.flags.value("panel", json()), "tailorAddNotch");
            const std::vector<Vec2> pos = pointsArg(json::array({a.flags.value("position", json())}), "tailorAddNotch");
            const int k = addNotch(gm, pi, pos.front());
            writeGarment(g, op, gm);
            return int64_t(k);
        },
        .examples = {"tailorAddNotch('Shirt', panel='Sleeve', position=[20, 45])"},
    });

    ctx.registerCommand({
        .name = "tailorRemoveNotch",
        .doc = "Remove a panel's notch at an outline point. Returns whether there was one.",
        .positionalDoc = "garment",
        .flags = {{"panel", "p", T::String, json(), "Panel by name or index."}, {"index", "i", T::Int, json(), "The notch's point index."}},
        .run = [](const Args& a) -> json {
            Graph& g = Graph::current();
            Node& op = garmentArg(g, a, "tailorRemoveNotch");
            Garment gm = readGarment(op);
            Panel& p = gm.panels[size_t(panelIndex(gm, a.flags.value("panel", json()), "tailorRemoveNotch"))];
            const int k = a.has("index") ? int(a.get<int64_t>("index")) : -1;
            const int n = int(p.outline.size());
            const bool had = std::erase(p.notches, k) > 0;
            if (had && p.twin >= 0 && size_t(p.twin) < gm.panels.size() && n > 0)
                std::erase(gm.panels[size_t(p.twin)].notches, (n - k % n) % n);
            writeGarment(g, op, gm);
            return had;
        },
        .examples = {"tailorRemoveNotch('Shirt', panel='Sleeve', index=7)"},
    });

    ctx.registerCommand({
        .name = "tailorMirrorPanel",
        .doc = "Add a mirror image of a panel, placed mirrored across the middle of the garment. Linked, editing either "
               "panel's outline changes both. Returns the new panel's name.",
        .positionalDoc = "garment",
        .flags = {{"panel", "p", T::String, json(), "Panel by name or index."},
                  {"link", "lk", T::Bool, true, "Keep the two outlines mirrored as either is edited."},
                  {"planeX", "px", T::Float, json(), "The mirror plane's x, cm; the middle of the garment's other panels when left out."}},
        .run = [](const Args& a) -> json {
            Graph& g = Graph::current();
            Node& op = garmentArg(g, a, "tailorMirrorPanel");
            Garment gm = readGarment(op);
            const int pi = panelIndex(gm, a.flags.value("panel", json()), "tailorMirrorPanel");
            float plane = 0.f;
            if (a.has("planeX"))
                plane = float(a.get<double>("planeX") / kCmPerM);
            else
            {
                float lo = 1e30f, hi = -1e30f;
                for (size_t k = 0; k < gm.panels.size(); ++k)
                    if (int(k) != pi)
                        for (const Vec2& v : gm.panels[k].outline)
                        {
                            const float x = placePatternPoint(gm.panels[k], v)[0];
                            lo = std::min(lo, x), hi = std::max(hi, x);
                        }
                plane = hi >= lo ? 0.5f * (lo + hi) : 0.f;
            }
            const int k = mirrorPanel(gm, pi, plane, a.get<bool>("link"));
            writeGarment(g, op, gm);
            return gm.panels[size_t(k)].name;
        },
        .examples = {"tailorMirrorPanel('Jacket', panel='FrontLeft')"},
    });

    ctx.registerCommand({
        .name = "tailorUnlinkPanel",
        .doc = "Stop a panel and its mirrored twin following each other's edits. Returns whether it had a twin.",
        .positionalDoc = "garment",
        .flags = {{"panel", "p", T::String, json(), "Panel by name or index."}},
        .run = [](const Args& a) -> json {
            Graph& g = Graph::current();
            Node& op = garmentArg(g, a, "tailorUnlinkPanel");
            Garment gm = readGarment(op);
            Panel& p = gm.panels[size_t(panelIndex(gm, a.flags.value("panel", json()), "tailorUnlinkPanel"))];
            const bool had = p.twin >= 0;
            if (had && size_t(p.twin) < gm.panels.size())
                gm.panels[size_t(p.twin)].twin = -1;
            p.twin = -1;
            writeGarment(g, op, gm);
            return had;
        },
        .examples = {"tailorUnlinkPanel('Jacket', panel='FrontLeft')"},
    });

    ctx.registerCommand({
        .name = "tailorAutoSew",
        .doc = "Sew the unsewn edges that meet: outline edges (corner to corner) whose lengths match within 15% and that face "
               "each other within maxGap as placed, closest first; with a collider, also edges that both open upwards over the "
               "body (shoulders, a hood's crown). Hems, necklines and armholes stay open. Returns the seams as {a, b, reversed} "
               "in tailorSew's panel:from:to form, each with its index when applied.",
        .positionalDoc = "garment",
        .flags = {{"maxGap", "mg", T::Float, 8.0, "Furthest apart two edges may be placed, on average, cm.", {}, {}, 0.1, 100.0},
                  {"apply", "ap", T::Bool, true, "Sew them; off only lists them."}},
        .run = [](const Args& a) -> json {
            Graph& g = Graph::current();
            Node& op = garmentArg(g, a, "tailorAutoSew");
            Garment gm = readGarment(op);
            // The body lets edges arching over it (shoulders) sew as well as edges facing each other.
            Sdf body;
            if (const std::optional<PlugRef> src = g.source({op.id(), plugIndex(op, "colliderMesh")}))
                if (const Node* collider = g.node(src->node))
                {
                    const PlugValue bv = g.value({collider->id(), MeshNode::kOutMesh});
                    const PlugValue bw = g.value({collider->id(), Transform::kWorldMatrix});
                    if (const MeshPtr* bm = std::get_if<MeshPtr>(&bv); bm && *bm && std::get_if<glm::dmat4>(&bw))
                    {
                        std::vector<Vec3> pts;
                        std::vector<std::array<int, 3>> tris;
                        colliderTriangles(**bm, std::get<glm::dmat4>(bw), pts, tris);
                        body = sdfFromMesh(pts, tris, 0.01f, 0.2f);
                    }
                }
            const std::vector<Seam> found = suggestSeams(gm, float(a.get<double>("maxGap") / kCmPerM), body.nx > 0 ? &body : nullptr);
            auto text = [&](const std::vector<SeamSide>& sides) {
                std::string out;
                for (const SeamSide& s : sides)
                    out += (out.empty() ? "" : ",") + gm.panels[size_t(s.panel)].name + ":" + std::to_string(s.from) + ":" + std::to_string(s.to);
                return out;
            };
            json out = json::array();
            for (const Seam& s : found)
            {
                json e = {{"a", text(s.a)}, {"b", text(s.b)}, {"reversed", s.reversed}};
                if (a.get<bool>("apply"))
                {
                    e["seam"] = int64_t(gm.seams.size());
                    gm.seams.push_back(s);
                }
                out.push_back(std::move(e));
            }
            if (a.get<bool>("apply") && !found.empty())
                writeGarment(g, op, gm);
            return out;
        },
        .examples = {"tailorAutoSew('Garment')", "tailorAutoSew('Garment', maxGap=12, apply=False)"},
    });

    ctx.registerCommand({
        .name = "tailorPin",
        .doc = "Pin the cloth near points on a garment's drape where it hangs now, or unpin it (remove=True). Pins hold in "
               "every drape, live or not, follow pattern edits and ride the body as it moves. Returns the garment's pin count.",
        .positionalDoc = "garment",
        .flags = {{"points", "pts", T::List, json(), "Brush points [[x, y, z], ...], world cm."},
                  {"radius", "r", T::Float, 3.0, "Brush radius, cm.", {}, {}, 0.1, 100.0},
                  {"remove", "rm", T::Bool, false, "Unpin instead."}},
        .run = [](const Args& a) -> json {
            Graph& g = Graph::current();
            Node& op = garmentArg(g, a, "tailorPin");
            const std::vector<glm::dvec3> pts = brushPoints(g, op, a, "tailorPin");
            const double r = a.get<double>("radius") / kCmPerM;
            Garment gm = readGarment(op);
            auto near = [&](const glm::dvec3& q) {
                return std::any_of(pts.begin(), pts.end(), [&](const glm::dvec3& p) { return glm::length(p - q) < r; });
            };
            if (a.get<bool>("remove"))
                std::erase_if(gm.pins, [&](const GarmentPin& p) { return near(glm::dvec3(p.target[0], p.target[1], p.target[2])); });
            else
            {
                const MeshPtr mesh = drapedMesh(g, op, "tailorPin");
                const geo::Mesh& m = *mesh;
                const std::span<const int32_t> panel = m.faceAttrs().get<int32_t>("panel");
                const std::span<const int32_t> side = m.faceAttrs().get<int32_t>("side");
                const std::span<const glm::vec2> fabric = m.cornerAttrs().get<glm::vec2>(geo::attr::uvName(1));
                std::vector<char> done(m.pointCapacity(), 0);
                for (geo::Index f = 0; f < m.faceCapacity() && !panel.empty() && !fabric.empty(); ++f)
                {
                    if (!m.faceValid(f) || (!side.empty() && side[f] != int32_t(GarmentSurface::Outer)))
                        continue;
                    for (geo::Index c : m.faceCorners(f))
                    {
                        const geo::Index p = m.cornerPoint(c);
                        const glm::dvec3 q = m.position(p) / kCmPerM;
                        if (done[p] || !near(q))
                            continue;
                        done[p] = 1;
                        gm.pins.push_back({panel[f], {fabric[c].x, fabric[c].y}, {float(q.x), float(q.y), float(q.z)}});
                    }
                }
                attachToCollider(g, op, gm);
            }
            writeGarment(g, op, gm);
            return int64_t(gm.pins.size());
        },
        .examples = {"tailorPin('Cloak', points=[[0, 150, -12]], radius=4)", "tailorPin('Cloak', points=[[0, 150, -12]], remove=True)"},
    });

    ctx.registerCommand({
        .name = "tailorFold",
        .doc = "Press a fold along a stroke on a garment's drape: the stroke becomes a fold line on each panel it crosses, "
               "held at the angle. Returns the number of fold lines added.",
        .positionalDoc = "garment",
        .flags = {{"points", "pts", T::List, json(), "The stroke [[x, y, z], ...], world cm, on or near the cloth."},
                  {"angle", "a", T::Float, 90.0, "Fold angle, degrees; negative folds the other way.", {}, {}, -179.0, 179.0},
                  {"reach", "rc", T::Float, 5.0, "How far a stroke point may lie off the cloth, cm.", {}, {}, 0.1, 100.0}},
        .run = [](const Args& a) -> json {
            Graph& g = Graph::current();
            Node& op = garmentArg(g, a, "tailorFold");
            const std::vector<glm::dvec3> pts = brushPoints(g, op, a, "tailorFold");
            const MeshPtr mesh = drapedMesh(g, op, "tailorFold");
            Garment gm = readGarment(op);
            const float angle = float(a.get<double>("angle") * std::numbers::pi / 180.0);
            int added = 0;
            std::vector<Vec2> run;
            int runPanel = -1;
            auto flush = [&] {
                if (run.size() >= 2 && runPanel >= 0 && size_t(runPanel) < gm.panels.size())
                {
                    Panel& p = gm.panels[size_t(runPanel)];
                    p.lineAngles.resize(p.internalLines.size(), std::nanf(""));
                    p.internalLines.push_back(run);
                    p.lineAngles.push_back(angle);
                    ++added;
                }
                run.clear();
            };
            for (const glm::dvec3& q : pts)
            {
                const auto hit = patternPointAt(*mesh, q, a.get<double>("reach") / kCmPerM);
                if (!hit || hit->first != runPanel)
                {
                    flush();
                    runPanel = hit ? hit->first : -1;
                }
                if (hit)
                    run.push_back(hit->second);
            }
            flush();
            if (added == 0)
                throw CommandError("tailorFold: the stroke doesn't run along any panel");
            writeGarment(g, op, gm);
            return int64_t(added);
        },
        .examples = {"tailorFold('Tunic', points=[[-10, 120, 14], [10, 120, 14]], angle=120)"},
    });

    ctx.registerCommand({
        .name = "tailorShrink",
        .doc = "Shrink (scale below 1) or stretch (above 1) the cloth's rest shape around points on a garment's drape, "
               "fading out to the radius: gathers, puckers and eased fullness. Returns the garment's shrink spot count.",
        .positionalDoc = "garment",
        .flags = {{"points", "pts", T::List, json(), "Brush points [[x, y, z], ...], world cm."},
                  {"radius", "r", T::Float, 5.0, "Brush radius, cm.", {}, {}, 0.1, 100.0},
                  {"scale", "s", T::Float, 0.9, "Rest scale at the middle of each dab.", {}, {}, 0.3, 2.0}},
        .run = [](const Args& a) -> json {
            Graph& g = Graph::current();
            Node& op = garmentArg(g, a, "tailorShrink");
            const std::vector<glm::dvec3> pts = brushPoints(g, op, a, "tailorShrink");
            const MeshPtr mesh = drapedMesh(g, op, "tailorShrink");
            Garment gm = readGarment(op);
            const float r = float(a.get<double>("radius") / kCmPerM), scale = float(a.get<double>("scale"));
            size_t total = 0;
            for (const glm::dvec3& q : pts)
                if (const auto hit = patternPointAt(*mesh, q, r); hit && size_t(hit->first) < gm.panels.size())
                {
                    auto& spots = gm.panels[size_t(hit->first)].shrinkSpots;
                    // Dabbing the same place again compounds the spot there instead of stacking another.
                    const auto same = std::find_if(spots.begin(), spots.end(), [&](const ShrinkSpot& s) {
                        return std::hypot(s.at[0] - hit->second[0], s.at[1] - hit->second[1]) < 0.3f * r && std::abs(s.radius - r) < 0.2f * r;
                    });
                    if (same != spots.end())
                        same->scale = std::clamp(same->scale * scale, 0.3f, 2.f);
                    else
                        spots.push_back({hit->second, r, scale});
                }
            for (const Panel& p : gm.panels)
                total += p.shrinkSpots.size();
            writeGarment(g, op, gm);
            return int64_t(total);
        },
        .examples = {"tailorShrink('Tunic', points=[[0, 100, 15]], radius=6, scale=0.85)"},
    });

    ctx.registerCommand({
        .name = "tailorRelax",
        .doc = "Steam a live garment around points: the cloth there smooths out and comes to rest, then settles again. "
               "Returns whether the garment is live.",
        .positionalDoc = "garment",
        .flags = {{"points", "pts", T::List, json(), "Brush points [[x, y, z], ...], world cm."},
                  {"radius", "r", T::Float, 6.0, "Brush radius, cm.", {}, {}, 0.1, 100.0}},
        .undoable = false,
        .run = [](const Args& a) -> json {
            Graph& g = Graph::current();
            Node& op = garmentArg(g, a, "tailorRelax");
            const std::vector<glm::dvec3> pts = brushPoints(g, op, a, "tailorRelax");
            const auto live = liveDrape(op.id());
            if (!live)
                return false;
            for (const glm::dvec3& q : pts)
                live->relax({float(q.x), float(q.y), float(q.z)}, float(a.get<double>("radius") / kCmPerM));
            return true;
        },
        .examples = {"tailorRelax('Cloak', points=[[0, 120, -15]])"},
    });

    ctx.registerCommand({
        .name = "tailorClearBrush",
        .doc = "Remove what the cloth brushes painted on a garment: pins, folds, shrink or all. Returns how many were removed.",
        .positionalDoc = "garment",
        .flags = {{"kind", "k", T::String, "all", "What to clear.", {"pins", "folds", "shrink", "all"}}},
        .run = [](const Args& a) -> json {
            Graph& g = Graph::current();
            Node& op = garmentArg(g, a, "tailorClearBrush");
            Garment gm = readGarment(op);
            const std::string kind = a.get<std::string>("kind");
            int64_t removed = 0;
            if (kind == "pins" || kind == "all")
            {
                removed += int64_t(gm.pins.size());
                gm.pins.clear();
            }
            for (Panel& p : gm.panels)
            {
                if (kind == "shrink" || kind == "all")
                {
                    removed += int64_t(p.shrinkSpots.size());
                    p.shrinkSpots.clear();
                }
                if (kind == "folds" || kind == "all")
                    for (size_t k = std::min(p.lineAngles.size(), p.internalLines.size()); k-- > 0;)
                        if (std::isfinite(p.lineAngles[k]))
                        {
                            p.internalLines.erase(p.internalLines.begin() + std::ptrdiff_t(k));
                            p.lineAngles.erase(p.lineAngles.begin() + std::ptrdiff_t(k));
                            ++removed;
                        }
            }
            writeGarment(g, op, gm);
            return removed;
        },
        .examples = {"tailorClearBrush('Cloak', kind='pins')"},
    });

    ctx.registerCommand({
        .name = "tailorSendToUnreal",
        .doc = "Ship a garment to Unreal with Chaos Cloth. as=clothAsset builds a Chaos Cloth Asset from the sewing pattern "
               "itself (panels as sim patterns, seams welded, the drape as its rest shape), ready for the Cloth Editor and a "
               "Chaos Cloth Component on your character. as=skeletalMesh bakes it (skinned like its collider, which must be "
               "skinned), imports it and adds clothing data to the mesh. Both paint max distance from how far the cloth hangs "
               "off the body and carry the fabric's mass, stiffness, damping and friction. as=skinnedMesh bakes and imports "
               "it the same way with no cloth at all: the drape holds its shape and follows the skin. Returns what was made.",
        .positionalDoc = "garment",
        .flags = {{"contentPath", "cp", T::String, json(), "The skeletal mesh asset, like /Game/Characters/Hero/SK_Hero_Cloak; /Game/Tailor/SK_<garment mesh> when left out."},
                  {"skeleton", "sk", T::String, json(), "Skeleton asset to reuse, like /Game/Characters/Hero/SK_Hero_Skeleton."},
                  {"retopo", "rt", T::Int, int64_t(0), "Quad remesh the game mesh to about this many faces first; 0 keeps the cloth triangles.",
                   {}, {}, 0.0, 200000.0},
                  {"maxInfluences", "mi", T::Int, int64_t(4), "Skin influences per point.", {}, {}, 1.0, 16.0},
                  {"save", "sv", T::Bool, true, "Save the Unreal assets afterwards."},
                  {"as", "", T::String, "clothAsset", "clothAsset, skeletalMesh with clothing, or skinnedMesh with no cloth.",
                   {"clothAsset", "skeletalMesh", "skinnedMesh"}},
                  {"skeletalMesh", "skm", T::String, json(), "Cloth asset: the character's skeletal mesh in Unreal it is made for."},
                  {"material", "mat", T::String, json(), "Cloth asset: the render material, like /Game/Cloth/M_Wool; a debug material when left out."}},
        .run = [](const Args& a) -> json {
            Graph& g = Graph::current();
            Node& op = garmentArg(g, a, "tailorSendToUnreal");
            std::shared_ptr<link::RpcClient> client = link::Link::get().client();
            if (!client)
                throw CommandError("tailorSendToUnreal: not connected to Unreal; run unrealConnect first");
            std::string path = a.has("contentPath") ? a.get<std::string>("contentPath") : std::string();
            if (path.empty())
            {
                const std::vector<PlugRef> out = g.destinations({op.id(), plugIndex(op, "outMesh")});
                const Node* fed = out.empty() ? nullptr : g.node(out.front().node);
                path = "/Game/Tailor/SK_" + (fed ? fed->name() : op.name());
            }
            const std::string as = a.get<std::string>("as");
            if (as == "clothAsset")
                return sendClothAsset(g, op, *client, path, a);
            const bool withCloth = as == "skeletalMesh";
            std::string name = path.substr(path.find_last_of('/') + 1);
            if (!Graph::validName(name))
                name = "Garment_baked";

            const json baked = ops::execute("tailorBake", {{"args", {op.path()}},
                                                           {"name", name},
                                                           {"retopo", a.get<int64_t>("retopo")},
                                                           {"clothMask", withCloth},
                                                           {"maxInfluences", a.get<int64_t>("maxInfluences")}});
            if (!baked.value("skinned", false))
                throw CommandError("tailorSendToUnreal: " + op.name() + "'s collider isn't skinned, so the garment can't go to Unreal as a skeletal mesh");
            Node* mesh = g.find(baked.value("mesh", std::string()));
            if (!mesh)
                throw CommandError("tailorSendToUnreal: the bake left no mesh");

            const std::filesystem::path file = link::cacheDir() / "tailor" / (name + ".fbx");
            std::filesystem::create_directories(file.parent_path());
            ops::execute("exportFbx", {{"args", {file.string(), mesh->path()}}, {"kind", "skeletal"}, {"animation", false},
                                       {"maxInfluences", a.get<int64_t>("maxInfluences")}});
            json mapping = {{"contentPath", path}, {"kind", "skeletal"}};
            if (a.has("skeleton"))
                mapping["skeleton"] = a.get<std::string>("skeleton");
            if (a.get<bool>("save"))
                mapping["save"] = true;
            const link::RpcResult imported = client->call("importAsset", link::importParams(file, mapping), link::importCallOptions());
            if (!imported.ok)
                throw CommandError("tailorSendToUnreal: importing into Unreal failed: " + imported.error);
            std::string asset = path;
            if (const json made = imported.result.value("assets", json::array()); made.is_array() && !made.empty() && made[0].is_string())
                asset = made[0].get<std::string>();
            if (!withCloth)
                return {{"mesh", mesh->path()}, {"asset", asset}};

            // Max distance per point in Unreal's space; Unreal matches its own cloth vertices to the nearest.
            g.evaluate();
            json samples = json::array();
            const PlugValue mv = g.value({mesh->id(), MeshNode::kOutMesh});
            const PlugValue mw = g.value({mesh->id(), Transform::kWorldMatrix});
            if (const MeshPtr* m = std::get_if<MeshPtr>(&mv); m && *m && std::get_if<glm::dmat4>(&mw))
            {
                const glm::dmat4 world = std::get<glm::dmat4>(mw);
                const std::span<const float> dist = (*m)->pointAttrs().get<float>("clothMaxDistance");
                for (geo::Index p = 0; p < (*m)->pointCapacity() && p < dist.size(); ++p)
                {
                    const math::Vector u = math::toUnreal(math::Vector(glm::dvec3(world * glm::dvec4((*m)->position(p), 1.0))));
                    samples.push_back({u.x, u.y, u.z, dist[p]});
                }
            }
            const Garment gm = readGarment(op);
            const Fabric& f = gm.fabrics.empty() ? *findFabric("Cotton") : gm.fabrics.front();
            const json config = chaosConfig(f);
            const link::RpcResult cloth = client->call("applyClothing",
                                                       {{"mesh", asset}, {"name", name + "_Cloth"}, {"maxDistance", samples}, {"config", config},
                                                        {"save", a.get<bool>("save")}},
                                                       link::importCallOptions());
            if (!cloth.ok)
                throw CommandError("tailorSendToUnreal: the mesh is in Unreal at " + asset + " but its clothing failed: " + cloth.error);
            return {{"mesh", mesh->path()}, {"asset", asset}, {"clothing", cloth.result}};
        },
        .examples = {"tailorSendToUnreal('Cloak', contentPath='/Game/Characters/Hero/SK_Hero_Cloak', skeleton='/Game/Characters/Hero/SK_Hero_Skeleton')",
                     "tailorSendToUnreal('Jacket', as='skinnedMesh', skeleton='/Game/Characters/Hero/SK_Hero_Skeleton')"},
    });

    ctx.registerCommand({
        .name = "tailorAddTrim",
        .doc = "Put copies of a trim on a garment's drape at points, or spaced along edges, each standing on the cloth with "
               "its +Z out of it and riding the cloth rigidly from then on: your mesh (eyes, teeth, spines, plates), or a "
               "built-in shape (button, grommet, rivet, toggle, buckle, zipTooth, slider). Along edges, each copy's +Y "
               "follows the edge. The copies make one new mesh. Returns [mesh, binding].",
        .positionalDoc = "garment",
        .flags = {nodeFlag({"mesh", "m", T::String, json(), "Your trim, modelled at the origin with +Z facing out of the cloth, cm."}, "mesh"),
                  {"shape", "sh", T::String, "button", "A built-in trim, when mesh is left out.", trimShapes()},
                  {"points", "pts", T::List, json(), "Where, [[x, y, z], ...] world cm, on or near the cloth."},
                  {"along", "al", T::String, json(), "Edges to space copies along instead of points, as tailorSew sides, e.g. \"Hood L:0:1\"."},
                  {"count", "c", T::Int, int64_t(0), "Copies along the edges; 0 spaces them by spacing.", {}, {}, 0.0, 500.0},
                  {"spacing", "sg", T::Float, 3.0, "Between copies along the edges, cm.", {}, {}, 0.2, 100.0},
                  {"inset", "in", T::Float, 0.5, "Copies in from the edge, cm.", {}, {}, 0.05, 20.0},
                  {"scale", "sc", T::Float, 1.0, "Size of each copy.", {}, {}, 0.01, 100.0},
                  {"name", "n", T::String, "Trims", "Name of the new mesh."},
                  {"spin", "sp", T::Float, 0.0, "Turn each copy about its +Z, degrees.", {}, {}, -360.0, 360.0}},
        .run = [](const Args& a) -> json {
            Graph& g = Graph::current();
            Node& op = garmentArg(g, a, "tailorAddTrim");
            geo::Mesh shape;
            const geo::Mesh* trim = nullptr;
            MeshPtr held;
            if (a.has("mesh"))
            {
                Node* node = g.find(a.get<std::string>("mesh"));
                if (!node || !node->type().isA("mesh"))
                    throw CommandError("tailorAddTrim: no mesh named " + a.get<std::string>("mesh"));
                const PlugValue tv = g.value({node->id(), MeshNode::kOutMesh});
                const MeshPtr* tm = std::get_if<MeshPtr>(&tv);
                if (!tm || !*tm || (*tm)->empty())
                    throw CommandError("tailorAddTrim: " + node->name() + " has no geometry");
                held = *tm;
                trim = held.get();
            }
            else
            {
                shape = trimShape(a.get<std::string>("shape"));
                trim = &shape;
            }
            const MeshPtr drape = drapedMesh(g, op, "tailorAddTrim");
            std::vector<TrimFrame> frames;
            std::vector<glm::dvec3> along;
            if (a.has("along"))
            {
                const Garment gm = readGarment(op);
                const std::vector<SeamSide> sides = parseSides(gm, a.get<std::string>("along"), "tailorAddTrim");
                const float length = seamSideLength(gm, sides);
                const int count = a.get<int64_t>("count") > 0 ? int(a.get<int64_t>("count"))
                                                              : std::max(1, int(length / float(a.get<double>("spacing") / kCmPerM)));
                const float inset = float(a.get<double>("inset") / kCmPerM);
                for (int k = 0; k < count; ++k)
                {
                    const float s = (float(k) + 0.5f) / float(count) * length;
                    const auto [panel, at] = pointAlongSides(gm, sides, s, inset);
                    const auto [panel2, ahead] = pointAlongSides(gm, sides, std::min(s + 0.01f, length), inset);
                    const auto f = frameAtPattern(*drape, panel, at);
                    if (!f)
                        continue;
                    frames.push_back(*f);
                    const auto t = frameAtPattern(*drape, panel2, ahead);
                    along.push_back(t ? t->at - f->at : glm::dvec3(0.0, 1.0, 0.0));
                }
                if (frames.empty())
                    throw CommandError("tailorAddTrim: those edges aren't on the drape");
            }
            else
                for (const glm::dvec3& p : brushPoints(g, op, a, "tailorAddTrim"))
                    frames.push_back(frameNear(*drape, p * kCmPerM));
            return placeTrims(g, op, *trim, frames, drape, a.get<std::string>("name"), a.get<double>("spin"), along, a.get<double>("scale"));
        },
        .examples = {"tailorAddTrim('Coat', shape='button', points=[[0, 130, 16], [0, 118, 17], [0, 106, 17]])",
                     "tailorAddTrim('Coat', mesh='SM_Buckle', points=[[0, 100, 16]])",
                     "tailorAddTrim('Onesie', mesh='Tooth', along='Hood L:0:1', count=2, spin=180)"},
    });

    ctx.registerCommand({
        .name = "tailorAddStitching",
        .doc = "Topstitch a garment: thread sewn along its seams (every seam, or the ones given) or along any edges, laid out on "
               "the flat pattern so rows keep their spacing however the cloth drapes, and latched to the cloth so it bends with "
               "it. Real geometry with plies and UVs along the thread, as the stitch command makes along curves. Returns "
               "[mesh, binding, stitches].",
        .positionalDoc = "garment",
        .flags = {{"preset", "p", T::String, json(), "Starting settings (flags given as well win).", curve::stitchPresetNames()},
                  {"seams", "s", T::List, json(), "Seam indices to stitch; default every seam."},
                  {"seamSide", "ss", T::String, "both", "Which side of each seam: both, a or b.", {"both", "a", "b"}},
                  {"along", "al", T::String, json(), "Edges to stitch instead of seams, as tailorSew sides, e.g. \"Hood L:0:1\" (hems, pockets)."},
                  {"inset", "in", T::Float, 0.4, "First row in from the edge, cm.", {}, {}, -5.0, 20.0},
                  {"pattern", "pt", T::String, json(), "running, zigzag, cross or blanket.", {"running", "zigzag", "cross", "blanket"}},
                  {"stitchLength", "sl", T::Float, json(), "Each stitch's length, cm.", {}, {}, 0.01, 10.0},
                  {"stitchGap", "sg", T::Float, json(), "Between stitches, cm.", {}, {}, 0.0, 10.0},
                  {"rotation", "ro", T::Float, json(), "Running: each stitch's slant, degrees.", {}, {}, -90.0, 90.0},
                  {"stitchWidth", "sw", T::Float, json(), "Zigzag, cross and blanket: reach across the line, cm.", {}, {}, 0.0, 10.0},
                  {"rows", "r", T::Int, json(), "Parallel rows, going inward.", {}, {}, 1.0, 16.0},
                  {"rowSpacing", "rs", T::Float, json(), "Between rows, cm.", {}, {}, 0.0, 10.0},
                  {"diameter", "d", T::Float, json(), "Thread thickness, cm.", {}, {}, 0.001, 2.0},
                  {"flatten", "fl", T::Float, json(), "0 round, toward 1 pressed flat.", {}, {}, 0.0, 0.95},
                  {"plies", "pl", T::Int, json(), "Strands twisted into the thread.", {}, {}, 1.0, 3.0},
                  {"twist", "tw", T::Float, json(), "Ply turns per cm.", {}, {}, 0.0, 20.0},
                  {"restHeight", "rh", T::Float, json(), "Thread's underside off the cloth, cm; negative sinks it in.", {}, {}, -1.0, 1.0},
                  {"insetDepth", "id", T::Float, json(), "How deep the thread dives into its holes, cm.", {}, {}, 0.0, 2.0},
                  {"jitter", "j", T::Float, json(), "Hand-sewn wobble; 0 is machine-even.", {}, {}, 0.0, 1.0},
                  {"seed", "sd", T::Int, json(), "The wobble's seed."},
                  {"name", "n", T::String, "Stitching", "Name of the new mesh."}},
        .run = [](const Args& a) -> json {
            Graph& g = Graph::current();
            Node& op = garmentArg(g, a, "tailorAddStitching");
            curve::StitchOptions o;
            if (a.has("preset"))
            {
                const std::optional<curve::StitchOptions> p = curve::stitchPreset(a.get<std::string>("preset"));
                if (!p)
                    throw CommandError("tailorAddStitching: unknown preset " + a.get<std::string>("preset"));
                o = *p;
            }
            if (a.has("pattern"))
            {
                const std::string p = a.get<std::string>("pattern");
                o.pattern = p == "zigzag" ? curve::StitchPattern::Zigzag
                            : p == "cross" ? curve::StitchPattern::Cross
                            : p == "blanket" ? curve::StitchPattern::Blanket
                                             : curve::StitchPattern::Running;
            }
            const auto num = [&](const char* flag, double& to) {
                if (a.has(flag))
                    to = a.get<double>(flag);
            };
            const auto whole = [&](const char* flag, int& to) {
                if (a.has(flag))
                    to = int(a.get<int64_t>(flag));
            };
            num("stitchLength", o.length);
            num("stitchGap", o.gap);
            num("rotation", o.rotation);
            num("stitchWidth", o.width);
            whole("rows", o.rows);
            num("rowSpacing", o.rowSpacing);
            num("diameter", o.diameter);
            num("flatten", o.flatten);
            whole("plies", o.plies);
            num("twist", o.twist);
            num("restHeight", o.restHeight);
            num("insetDepth", o.inset);
            num("jitter", o.jitter);
            if (a.has("seed"))
                o.seed = uint32_t(a.get<int64_t>("seed"));

            const Garment gm = readGarment(op);
            std::vector<std::vector<SeamSide>> chains;
            if (a.has("along"))
                chains.push_back(parseSides(gm, a.get<std::string>("along"), "tailorAddStitching"));
            else
            {
                std::vector<int> which;
                if (a.has("seams"))
                    for (const json& s : a.flags["seams"])
                        which.push_back(s.get<int>());
                else
                    for (int k = 0; k < int(gm.seams.size()); ++k)
                        which.push_back(k);
                const std::string side = a.get<std::string>("seamSide");
                for (int k : which)
                {
                    if (k < 0 || k >= int(gm.seams.size()))
                        throw CommandError("tailorAddStitching: no seam " + std::to_string(k) + " (the garment has " + std::to_string(gm.seams.size()) + ")");
                    if (side != "b" && !gm.seams[size_t(k)].a.empty())
                        chains.push_back(gm.seams[size_t(k)].a);
                    if (side != "a" && !gm.seams[size_t(k)].b.empty())
                        chains.push_back(gm.seams[size_t(k)].b);
                }
            }
            if (chains.empty())
                throw CommandError("tailorAddStitching: the garment has no seams; sew some or give along");
            return stitchAlong(g, op, gm, chains, drapedMesh(g, op, "tailorAddStitching"), o, a.get<double>("inset"), a.get<std::string>("name"));
        },
        .examples = {"tailorAddStitching('Jacket', preset='denim')", "tailorAddStitching('Onesie', seams=[0, 3], seamSide='a', rows=2)",
                     "tailorAddStitching('Onesie', along='Hood L:2:5', pattern='blanket', inset=0)"},
    });

    ctx.registerCommand({
        .name = "tailorAddPocket",
        .doc = "Add a patch pocket on a panel, tacked round all but its mouth, with an optional flap over the mouth. "
               "Returns the pocket panel's name.",
        .positionalDoc = "garment",
        .flags = {{"panel", "p", T::String, json(), "The panel it sits on, by name or index."},
                  {"position", "pos", T::List, json(), "Its lower left corner, [x, y] cm in the panel's pattern."},
                  {"width", "w", T::Float, 14.0, "cm.", {}, {}, 1.0, 100.0},
                  {"height", "ht", T::Float, 16.0, "cm.", {}, {}, 1.0, 100.0},
                  {"flap", "fl", T::Bool, false, "A flap over the mouth."}},
        .run = [](const Args& a) -> json {
            Graph& g = Graph::current();
            Node& op = garmentArg(g, a, "tailorAddPocket");
            Garment gm = readGarment(op);
            const int host = panelIndex(gm, a.flags.value("panel", json()), "tailorAddPocket");
            const std::vector<Vec2> at = pointsArg(json::array({a.flags.value("position", json())}), "tailorAddPocket");
            const int k = addPocket(gm, host, at.front(), float(a.get<double>("width") / kCmPerM), float(a.get<double>("height") / kCmPerM),
                                    a.get<bool>("flap"));
            if (k < 0)
                throw CommandError("tailorAddPocket: couldn't place the pocket");
            writeGarment(g, op, gm);
            return gm.panels[size_t(k)].name;
        },
        .examples = {"tailorAddPocket('Shirt', panel='Front', position=[5, 30], flap=True)"},
    });

    ctx.registerCommand({
        .name = "tailorAddBeltLoops",
        .doc = "Add belt loops across a panel at a height, each tacked at both ends and standing off the cloth for a belt "
               "to pass under. Returns the loop panels' names.",
        .positionalDoc = "garment",
        .flags = {{"panel", "p", T::String, json(), "The panel, by name or index."},
                  {"height", "ht", T::Float, json(), "Where the loops' lower ends sit, cm up the panel's pattern."},
                  {"count", "c", T::Int, int64_t(3), "How many.", {}, {}, 1.0, 20.0},
                  {"width", "w", T::Float, 1.2, "Loop width, cm.", {}, {}, 0.3, 10.0},
                  {"length", "l", T::Float, 5.0, "Span between the tacked ends, cm.", {}, {}, 1.0, 30.0}},
        .run = [](const Args& a) -> json {
            Graph& g = Graph::current();
            Node& op = garmentArg(g, a, "tailorAddBeltLoops");
            Garment gm = readGarment(op);
            const int host = panelIndex(gm, a.flags.value("panel", json()), "tailorAddBeltLoops");
            if (!a.has("height"))
                throw CommandError("tailorAddBeltLoops: give height, cm up the panel");
            const std::vector<int> made = addBeltLoops(gm, host, float(a.get<double>("height") / kCmPerM), int(a.get<int64_t>("count")),
                                                       float(a.get<double>("width") / kCmPerM), float(a.get<double>("length") / kCmPerM));
            writeGarment(g, op, gm);
            json names = json::array();
            for (int k : made)
                names.push_back(gm.panels[size_t(k)].name);
            return names;
        },
        .examples = {"tailorAddBeltLoops('Trousers', panel='Front L', height=68, count=2)"},
    });

    ctx.registerCommand({
        .name = "tailorAddCollar",
        .doc = "Add a collar sewn to the neckline: the garment's open edges round its top, opening upwards and joined at the "
               "shoulders, unless given. A band stands up; a shirt collar folds its fall down over the stand. Returns the "
               "collar panel's name.",
        .positionalDoc = "garment",
        .flags = {{"height", "ht", T::Float, 7.0, "cm.", {}, {}, 1.0, 40.0},
                  {"style", "st", T::String, "shirt", "band or shirt.", {"band", "shirt"}},
                  {"neckline", "nl", T::String, json(), "The neckline as tailorSew sides, e.g. \"Front:24:52,Back:46:24\"."}},
        .run = [](const Args& a) -> json {
            Graph& g = Graph::current();
            Node& op = garmentArg(g, a, "tailorAddCollar");
            Garment gm = readGarment(op);
            const std::vector<SeamSide> neck =
                a.has("neckline") ? parseSides(gm, a.get<std::string>("neckline"), "tailorAddCollar") : findNeckline(gm);
            if (neck.empty())
                throw CommandError("tailorAddCollar: found no neckline; give neckline as tailorSew sides");
            const int k = addCollar(gm, neck, float(a.get<double>("height") / kCmPerM), a.get<std::string>("style") == "shirt");
            if (k < 0)
                throw CommandError("tailorAddCollar: couldn't make the collar");
            writeGarment(g, op, gm);
            return gm.panels[size_t(k)].name;
        },
        .examples = {"tailorAddCollar('Shirt')", "tailorAddCollar('Tunic', style='band', height=4)"},
    });

    ctx.registerCommand({
        .name = "tailorAddDrawstring",
        .doc = "Thread a drawstring along edges (a hood's face, a waist): a cord held along them that draws them in by "
               "gather, tails hanging past both ends, optional toggles on the tails. Returns the cord panel's name, and the "
               "toggles' [mesh, binding] when added.",
        .positionalDoc = "garment",
        .flags = {{"along", "al", T::String, json(), "The channel as tailorSew sides, e.g. \"Hood L:5:9,Hood R:2:6\"."},
                  {"gather", "ga", T::Float, 0.8, "How much of the channel's length the cord leaves it, 0.2 to 1.", {}, {}, 0.2, 1.0},
                  {"tail", "tl", T::Float, 15.0, "Tail length past each end, cm.", {}, {}, 0.0, 100.0},
                  {"width", "w", T::Float, 0.6, "Cord width, cm.", {}, {}, 0.2, 5.0},
                  {"toggles", "tg", T::Bool, true, "Toggles on the tails' ends."}},
        .run = [](const Args& a) -> json {
            Graph& g = Graph::current();
            Node& op = garmentArg(g, a, "tailorAddDrawstring");
            Garment gm = readGarment(op);
            if (!a.has("along"))
                throw CommandError("tailorAddDrawstring: give along, the channel as tailorSew sides");
            const float tail = float(a.get<double>("tail") / kCmPerM), width = float(a.get<double>("width") / kCmPerM);
            const int cord = addDrawstring(gm, parseSides(gm, a.get<std::string>("along"), "tailorAddDrawstring"), tail,
                                           float(a.get<double>("gather")), width);
            if (cord < 0)
                throw CommandError("tailorAddDrawstring: couldn't thread the cord");
            writeGarment(g, op, gm);
            json out = {{"cord", gm.panels[size_t(cord)].name}};
            if (a.get<bool>("toggles") && tail > 0.f)
            {
                const float l = gm.panels[size_t(cord)].outline[1][0];
                out["toggles"] = trimsAtPattern(g, op, "toggle", {{cord, {0.01f, 0.5f * width}}, {cord, {l - 0.01f, 0.5f * width}}},
                                                gm.panels[size_t(cord)].name + "_toggles", "tailorAddDrawstring");
            }
            return out;
        },
        .examples = {"tailorAddDrawstring('Cloak', along='Hood L:5:9,Hood R:2:6', gather=0.7)"},
    });

    ctx.registerCommand({
        .name = "tailorAddLacing",
        .doc = "Lace two edges together (a corset back, a laced neckline): eyelets inset along both, a lace zigzagging "
               "through them with tails at the top, optional grommets at the eyelets. Returns the lace panel's name, and the "
               "grommets' [mesh, binding] when added.",
        .positionalDoc = "garment",
        .flags = {{"sideA", "a", T::String, json(), "One edge as tailorSew sides."},
                  {"sideB", "b", T::String, json(), "The other edge."},
                  {"pairs", "pr", T::Int, int64_t(5), "Eyelets on each edge.", {}, {}, 1.0, 50.0},
                  {"inset", "in", T::Float, 1.5, "Eyelets in from the edge, cm.", {}, {}, 0.2, 10.0},
                  {"tail", "tl", T::Float, 12.0, "Tail length, cm.", {}, {}, 0.0, 100.0},
                  {"width", "w", T::Float, 0.5, "Lace width, cm.", {}, {}, 0.2, 5.0},
                  {"grommets", "gr", T::Bool, true, "Grommets at the eyelets."}},
        .run = [](const Args& a) -> json {
            Graph& g = Graph::current();
            Node& op = garmentArg(g, a, "tailorAddLacing");
            Garment gm = readGarment(op);
            if (!a.has("sideA") || !a.has("sideB"))
                throw CommandError("tailorAddLacing: give sideA and sideB");
            std::vector<std::pair<int, Vec2>> eyelets;
            const int lace = addLacing(gm, parseSides(gm, a.get<std::string>("sideA"), "tailorAddLacing"),
                                       parseSides(gm, a.get<std::string>("sideB"), "tailorAddLacing"), int(a.get<int64_t>("pairs")),
                                       float(a.get<double>("inset") / kCmPerM), float(a.get<double>("tail") / kCmPerM),
                                       float(a.get<double>("width") / kCmPerM), &eyelets);
            if (lace < 0)
                throw CommandError("tailorAddLacing: couldn't lace the edges");
            writeGarment(g, op, gm);
            json out = {{"lace", gm.panels[size_t(lace)].name}};
            if (a.get<bool>("grommets"))
                out["grommets"] = trimsAtPattern(g, op, "grommet", eyelets, gm.panels[size_t(lace)].name + "_grommets", "tailorAddLacing");
            return out;
        },
        .examples = {"tailorAddLacing('Bodice', sideA='Back L:1:2', sideB='Back R:3:0', pairs=8)"},
    });

    ctx.registerCommand({
        .name = "tailorAddZip",
        .doc = "Put a zip in a seam: teeth along both sides and a slider where it stands, the seam unsewn past the slider. "
               "open runs from 0 (closed) to 1 (fully open), from side A's end back towards its start. Returns the teeth and "
               "slider as [mesh, binding] each.",
        .positionalDoc = "garment",
        .flags = {{"seam", "s", T::Int, json(), "The seam, as tailorSew returned it."},
                  {"open", "op", T::Float, 0.0, "How far open, 0 to 1.", {}, {}, 0.0, 1.0},
                  {"spacing", "sp", T::Float, 0.6, "Between teeth along each side, cm.", {}, {}, 0.2, 5.0}},
        .run = [](const Args& a) -> json {
            Graph& g = Graph::current();
            Node& op = garmentArg(g, a, "tailorAddZip");
            Garment gm = readGarment(op);
            const int seam = a.has("seam") ? int(a.get<int64_t>("seam")) : -1;
            if (seam < 0 || size_t(seam) >= gm.seams.size() || gm.seams[size_t(seam)].a.size() != 1 || gm.seams[size_t(seam)].b.size() != 1)
                throw CommandError("tailorAddZip: give seam, a seam with one run on each side");
            const Seam whole = gm.seams[size_t(seam)];
            const float open = float(a.get<double>("open"));
            // Teeth along the whole of both runs, a little in from each edge, alternating sides.
            const float spacing = float(a.get<double>("spacing") / kCmPerM), inset = 0.003f;
            std::vector<std::pair<int, Vec2>> teeth, towards;
            auto runPoint = [&](const SeamSide& s, float f, float in) {
                const auto& o = gm.panels[size_t(s.panel)].outline;
                const int n = int(o.size());
                float total = 0.f;
                for (int v = s.from; v != s.to; v = (v + 1) % n)
                    total += std::hypot(o[size_t((v + 1) % n)][0] - o[size_t(v)][0], o[size_t((v + 1) % n)][1] - o[size_t(v)][1]);
                float want = f * total, walked = 0.f;
                for (int v = s.from; v != s.to; v = (v + 1) % n)
                {
                    const Vec2 &p = o[size_t(v)], &q = o[size_t((v + 1) % n)];
                    const float l = std::hypot(q[0] - p[0], q[1] - p[1]);
                    if (walked + l >= want && l > 0.f)
                    {
                        const float t = (want - walked) / l;
                        // Inward is left of travel round a counter-clockwise outline.
                        return Vec2{p[0] + (q[0] - p[0]) * t - (q[1] - p[1]) / l * in, p[1] + (q[1] - p[1]) * t + (q[0] - p[0]) / l * in};
                    }
                    walked += l;
                }
                return o[size_t(s.to)];
            };
            const float length = seamSideLength(gm, whole.a);
            const int count = std::max(2, int(length / spacing));
            for (int k = 0; k < count; ++k)
            {
                const float f = (float(k) + 0.5f) / float(count);
                const SeamSide& side = k % 2 == 0 ? whole.a.front() : whole.b.front();
                const float fs = k % 2 == 0 || !whole.reversed ? f : 1.f - f;
                teeth.push_back({side.panel, runPoint(side, fs, inset)});
                towards.push_back({side.panel, runPoint(side, std::min(fs + 0.01f, 1.f), inset)});
            }
            const float closed = 1.f - open;
            if (!openSeam(gm, seam, closed))
                throw CommandError("tailorAddZip: couldn't open the seam");
            writeGarment(g, op, gm);
            json out = {{"teeth", trimsAtPattern(g, op, "zipTooth", teeth, "ZipTeeth", "tailorAddZip", towards)}};
            const SeamSide& sa = whole.a.front();
            const float at = std::clamp(closed, 0.02f, 0.98f);
            out["slider"] = trimsAtPattern(g, op, "slider", {{sa.panel, runPoint(sa, at, 0.f)}}, "ZipSlider", "tailorAddZip",
                                           {{sa.panel, runPoint(sa, std::min(at + 0.05f, 1.f), 0.f)}});
            return out;
        },
        .examples = {"tailorAddZip('Jacket', seam=4, open=0.3)"},
    });

    ctx.registerCommand({
        .name = "tailorDamage",
        .doc = "Wear a garment: ragged holes or tears in the cloth at points on its drape, cut into the pattern so the "
               "cloth drapes around them. Returns how many were cut.",
        .positionalDoc = "garment",
        .flags = {{"points", "pts", T::List, json(), "Where, [[x, y, z], ...] world cm, on or near the cloth."},
                  {"radius", "r", T::Float, 3.0, "Hole radius or half the tear's length, cm.", {}, {}, 0.3, 100.0},
                  {"kind", "k", T::String, "hole", "hole or tear.", {"hole", "tear"}},
                  {"seed", "sd", T::Int, int64_t(1), "Varies the ragged edge."}},
        .run = [](const Args& a) -> json {
            Graph& g = Graph::current();
            Node& op = garmentArg(g, a, "tailorDamage");
            const std::vector<glm::dvec3> pts = brushPoints(g, op, a, "tailorDamage");
            const MeshPtr drape = drapedMesh(g, op, "tailorDamage");
            Garment gm = readGarment(op);
            const float r = float(a.get<double>("radius") / kCmPerM);
            const bool tear = a.get<std::string>("kind") == "tear";
            uint32_t state = uint32_t(a.get<int64_t>("seed")) * 2654435761u + 1u;
            auto rnd = [&] {
                state = state * 1664525u + 1013904223u;
                return float(state >> 8) / float(1u << 24);
            };
            int64_t cut = 0;
            for (const glm::dvec3& q : pts)
            {
                const auto hit = patternPointAt(*drape, q, 2.0 * r);
                if (!hit || size_t(hit->first) >= gm.panels.size())
                    continue;
                Panel& p = gm.panels[size_t(hit->first)];
                const Vec2 c = hit->second;
                std::vector<Vec2> loop;
                const int n = tear ? 16 : 14;
                const float turn = rnd() * 6.2831853f;
                for (int k = 0; k < n; ++k)
                {
                    const float t = 6.2831853f * float(k) / float(n);
                    // A tear is a long ragged slit; a hole is a ragged round.
                    const float rx = tear ? r : r * (0.75f + 0.45f * rnd()), ry = tear ? r * (0.08f + 0.12f * rnd()) : rx;
                    const float x = rx * std::cos(t), y = ry * std::sin(t);
                    loop.push_back({c[0] + x * std::cos(turn) - y * std::sin(turn), c[1] + x * std::sin(turn) + y * std::cos(turn)});
                }
                // Inside the panel by a cloth edge, or the cut would open the outline.
                bool inside = true;
                for (const Vec2& v : loop)
                {
                    bool in = false;
                    const auto& o = p.outline;
                    for (size_t i = 0, j = o.size() - 1; i < o.size(); j = i++)
                        if ((o[i][1] > v[1]) != (o[j][1] > v[1]) && v[0] < (o[j][0] - o[i][0]) * (v[1] - o[i][1]) / (o[j][1] - o[i][1]) + o[i][0])
                            in = !in;
                    inside = inside && in;
                }
                if (!inside)
                    continue;
                p.holes.push_back(std::move(loop));
                ++cut;
            }
            if (cut == 0)
                throw CommandError("tailorDamage: no point fell inside a panel with room for the cut");
            writeGarment(g, op, gm);
            return cut;
        },
        .examples = {"tailorDamage('Cloak', points=[[5, 90, -20]], radius=4)", "tailorDamage('Cloak', points=[[-10, 60, -22]], kind='tear', radius=8)"},
    });

    ctx.registerCommand({
        .name = "tailorStrap",
        .doc = "Add a strap to a garment along a path (a belt, a sash, a bandolier): a strip of cloth the path's length, "
               "its middle pinned along the path so it drapes over the body there. Give points or a curve. Returns the strap "
               "panel's name.",
        .positionalDoc = "garment",
        .flags = {{"points", "pts", T::List, json(), "The path, [[x, y, z], ...] world cm."},
                  nodeFlag({"curve", "cv", T::String, json(), "A curve to follow instead of points (its points, world space)."}, "curve"),
                  {"width", "w", T::Float, 4.0, "Strap width, cm.", {}, {}, 0.5, 100.0},
                  {"name", "n", T::String, "Strap", "Panel name; made unique."},
                  {"fabric", "fab", T::String, "Leather", "Fabric preset of the strap.", presetChoices()},
                  {"closed", "cl", T::Bool, false, "The path closes on itself (a belt round the waist)."}},
        .run = [](const Args& a) -> json {
            Graph& g = Graph::current();
            Node& op = garmentArg(g, a, "tailorStrap");
            std::vector<glm::dvec3> path;
            const glm::dmat4 toLocal = glm::inverse(garmentWorld(g, op.id()));
            if (a.has("curve"))
            {
                const json data = ops::execute("curveData", {{"args", {a.get<std::string>("curve")}}, {"worldSpace", true}});
                const json splines = data.is_object() ? data.value("splines", json::array()) : data;
                if (splines.is_array() && !splines.empty())
                {
                    // The evaluated curve at even spacing, a quarter strap width apart, not its control points.
                    const curve::Spline s = curve::splineFromJson(splines[0]);
                    const double len = curve::length(s);
                    const int n = std::clamp(int(std::ceil(len / (0.25 * a.get<double>("width")))), 2, 2000);
                    for (int k = 0; k <= n; ++k)
                        path.push_back(glm::dvec3(toLocal * glm::dvec4(curve::pointAt(s, curve::paramAtLength(s, len * k / n)), 1.0)) / kCmPerM);
                }
            }
            else
                path = brushPoints(g, op, a, "tailorStrap");
            if (a.get<bool>("closed") && path.size() >= 3)
                path.push_back(path.front());
            if (path.size() < 2)
                throw CommandError("tailorStrap: give points or a curve with at least 2 points");
            std::vector<double> at{0.0};
            for (size_t i = 1; i < path.size(); ++i)
                at.push_back(at.back() + glm::length(path[i] - path[i - 1]));
            const float length = float(at.back()), w = float(a.get<double>("width") / kCmPerM);
            if (length < 2.f * w)
                throw CommandError("tailorStrap: the path is shorter than twice the strap's width");

            Garment gm = readGarment(op);
            Panel p;
            const std::string base = a.get<std::string>("name");
            p.name = base;
            for (int k = 2; std::any_of(gm.panels.begin(), gm.panels.end(), [&](const Panel& o) { return o.name == p.name; }); ++k)
                p.name = base + std::to_string(k);
            p.outline = {{0.f, 0.f}, {length, 0.f}, {length, w}, {0.f, w}};
            p.particleDistance = std::min(gm.panels.empty() ? 0.02f : gm.panels.front().particleDistance, 0.5f * w);
            // Placed flat along the path's chord, at its start; the pins carry it onto the path as it drapes.
            const glm::dvec3 chord = glm::normalize(path.back() - path.front() + glm::dvec3(1e-9, 0.0, 0.0));
            p.origin = {float(path.front().x), float(path.front().y - 0.5 * w), float(path.front().z)};
            p.right = {float(chord.x), float(chord.y), float(chord.z)};
            p.up = {0.f, 1.f, 0.f};
            if (std::abs(chord.y) > 0.9)
                p.up = {1.f, 0.f, 0.f};
            const Fabric* f = findFabric(a.get<std::string>("fabric"));
            int fabric = 0;
            if (f)
            {
                const auto same = std::find_if(gm.fabrics.begin(), gm.fabrics.end(), [&](const Fabric& o) { return o.name == f->name; });
                fabric = int(same - gm.fabrics.begin());
                if (same == gm.fabrics.end())
                    gm.fabrics.push_back(*f);
            }
            p.fabric = fabric;
            const int index = int(gm.panels.size());
            gm.panels.push_back(std::move(p));
            // Pins down the strap's middle line, about one per strap width.
            const int count = std::max(2, int(std::ceil(length / w)));
            for (int k = 0; k <= count; ++k)
            {
                const double s = double(length) * k / count;
                size_t i = 1;
                while (i + 1 < at.size() && at[i] < s)
                    ++i;
                const double t = at[i] > at[i - 1] ? (s - at[i - 1]) / (at[i] - at[i - 1]) : 0.0;
                const glm::dvec3 q = glm::mix(path[i - 1], path[i], std::clamp(t, 0.0, 1.0));
                gm.pins.push_back({index, {float(s), 0.5f * w}, {float(q.x), float(q.y), float(q.z)}});
            }
            attachToCollider(g, op, gm);
            writeGarment(g, op, gm);
            return gm.panels[size_t(index)].name;
        },
        .examples = {"tailorStrap('Tunic', curve='BeltPath', closed=True, width=5)", "tailorStrap('Tunic', points=[[-15, 140, 14], [15, 95, 15]], width=6, name='Sash')"},
    });

    ctx.registerCommand({
        .name = "tailorBakeWrinkleMaps",
        .doc = "Bake a fabric's wrinkle normal maps: a square of it squeezed along U, then along V, simulated until it folds. "
               "They tile, and pair with the 'wrinkle' colour set Tailor's drapes and Lila's tension deformer write "
               "(compression along U in red, V in green). With send, they go to Unreal with MF_LilaTensionWrinkles, a material "
               "function for your own materials (and M_LilaTensionWrinkles, a material built on it, with material=True). "
               "Returns {files, unreal}.",
        .positionalDoc = "garment",
        .flags = {{"fabric", "fab", T::String, json(), "A fabric preset; the garment's first fabric when left out.", presetChoices()},
                  {"size", "s", T::Int, int64_t(512), "Map width and height, texels.", {}, {}, 32.0, 4096.0},
                  {"tile", "tl", T::Float, 20.0, "The square of cloth one map covers, cm.", {}, {}, 2.0, 200.0},
                  {"compress", "c", T::Float, 0.25, "How far the square is squeezed, 0 to 0.6.", {}, {}, 0.05, 0.6},
                  {"folder", "fo", T::String, "textures", "Where the maps go; relative paths are beside the scene file."},
                  {"send", "sd", T::Bool, false, "Send them to Unreal and build MF_LilaTensionWrinkles there."},
                  {"unrealFolder", "uf", T::String, "/Game/Lila/Wrinkles", "Unreal folder for the maps and the function."},
                  {"material", "mat", T::Bool, false, "Also build M_LilaTensionWrinkles on the function."}},
        .undoable = false,
        .run = [](const Args& a) -> json {
            Graph& g = Graph::current();
            const Fabric* fabric = nullptr;
            Fabric own;
            if (a.has("fabric"))
                fabric = &fabricArg(a.get<std::string>("fabric"), "tailorBakeWrinkleMaps");
            else
            {
                Node& op = garmentArg(g, a, "tailorBakeWrinkleMaps");
                const Garment gm = readGarment(op);
                own = gm.fabrics.empty() ? Fabric{} : gm.fabrics.front();
                fabric = &own;
            }
            const int size = int(a.get<int64_t>("size"));
            const float tile = float(a.get<double>("tile") / kCmPerM);
            const std::filesystem::path folder = paths::resolvePath(a.get<std::string>("folder"));
            std::filesystem::create_directories(folder);
            json files = json::array();
            for (int axis = 0; axis < 2; ++axis)
            {
                const std::vector<float> h = bakeWrinkleHeight(*fabric, axis, float(a.get<double>("compress")), size, tile);
                const std::vector<float> rgba = heightToNormals(h, size, tile);
                const std::filesystem::path file = folder / ("T_LilaWrinkle_" + fabric->name + (axis == 0 ? "_U.png" : "_V.png"));
                image::writePng(file, size, size, rgba, 16);
                files.push_back(file.generic_string());
            }
            json out = {{"files", files}};
            if (!a.get<bool>("send"))
                return out;
            std::shared_ptr<link::RpcClient> client = link::Link::get().client();
            if (!client)
                throw CommandError("tailorBakeWrinkleMaps: not connected to Unreal; run unrealConnect first");
            const std::string ufolder = a.get<std::string>("unrealFolder");
            json textures = json::array();
            for (const json& f : files)
            {
                const std::string name = std::filesystem::path(f.get<std::string>()).stem().string();
                textures.push_back({{"name", name}, {"map", "normal"}, {"srgb", false}, {"compression", "normalmap"},
                                    {"files", json::array({{{"file", f}, {"udim", 1001}}})}});
            }
            const link::RpcResult imported = client->call("importTextures", {{"folder", ufolder}, {"textures", textures}, {"save", true}},
                                                          link::importCallOptions());
            if (!imported.ok)
                throw CommandError("tailorBakeWrinkleMaps: importing the maps failed: " + imported.error);
            const std::string u = ufolder + "/" + std::filesystem::path(files[0].get<std::string>()).stem().string();
            const std::string v = ufolder + "/" + std::filesystem::path(files[1].get<std::string>()).stem().string();
            const link::RpcResult made = client->call("wrinkleFunction",
                                                      {{"folder", ufolder}, {"wrinkleU", u}, {"wrinkleV", v}, {"material", a.get<bool>("material")}, {"save", true}},
                                                      link::importCallOptions());
            if (!made.ok)
                throw CommandError("tailorBakeWrinkleMaps: building MF_LilaTensionWrinkles failed: " + made.error);
            out["unreal"] = made.result;
            return out;
        },
        .examples = {"tailorBakeWrinkleMaps(fabric='Wool', size=1024)", "tailorBakeWrinkleMaps('Coat', send=True)"},
    });

    ctx.registerCommand({
        .name = "tailorLive",
        .doc = "Keep a garment simulating in real time (on) or stop it where it got to (off). Live garments answer pattern "
               "edits as they happen and can be pulled with the Grab Cloth tool. Returns whether it is live.",
        .positionalDoc = "garment",
        .flags = {{"on", "o", T::Bool, true, "Live or not."}},
        .run = [](const Args& a) -> json {
            Graph& g = Graph::current();
            Node& op = garmentArg(g, a, "tailorLive");
            g.setValue({op.id(), plugIndex(op, "live")}, a.get<bool>("on"));
            return a.get<bool>("on");
        },
        .examples = {"tailorLive('Garment')", "tailorLive('Garment', on=False)"},
    });

    ctx.registerCommand({
        .name = "tailorDrawPanel",
        .doc = "Make a panel from a closed outline drawn on the body: the points (world, cm) are unrolled around the body's "
               "vertical axis into a flat pattern piece that wraps back onto the body at a clearance.\n\n"
               "Adds to the named or selected garment; with none, starts a garment on the selected mesh. Returns [garment, "
               "panel names].",
        .positionalDoc = "garment",
        .flags = {{"points", "pt", T::List, json(), "The outline, [[x, y, z], ...] in world cm, at least 3."},
                  {"name", "n", T::String, "Panel", "Panel name; made unique."},
                  {"mirror", "m", T::Bool, false, "Also add the mirror image across the body's centre."},
                  {"clearance", "cl", T::Float, 1.5, "How far the panel starts outside the body, cm.", {}, {}, 0.0, 50.0},
                  {"layer", "l", T::Int, int64_t(0), "Layer: higher layers sit outside lower ones."},
                  nodeFlag({"collider", "c", T::String, json(), "The body, when a new garment is started; the selected mesh otherwise."}, "mesh"),
                  {"fabric", "fab", T::String, "Cotton", "Fabric preset of a new garment.", presetChoices()},
                  {"particleDistance", "pd", T::Float, 2.0, "Cloth edge length of a new garment, cm.", {}, {}, 0.1, 50.0},
                  {"simulate", "sim", T::Bool, true, "Drape a new garment."},
                  {"frames", "fr", T::Int, int64_t(90), "Solver frames for a new garment.", {}, {}, 0.0, 10000.0},
                  {"wait", "w", T::Bool, false, "Drape before returning instead of in the background."},
                  {"flatten", "fl", T::Bool, true, "Flatten the panel as the body curves under it (lengths true on the body); off unrolls it round the body's axis."},
                  {"sew", "sw", T::Bool, true, "Sew the new panels to edges of the garment's panels that meet them."}},
        .run = [](const Args& a) -> json {
            Graph& g = Graph::current();
            std::vector<glm::dvec3> pts;
            const json list = a.flags.value("points", json());
            if (list.is_array())
                for (const json& p : list)
                    if (p.is_array() && p.size() >= 3)
                        pts.push_back(glm::dvec3(p[0].get<double>(), p[1].get<double>(), p[2].get<double>()) / kCmPerM);
            if (pts.size() < 3)
                throw CommandError("tailorDrawPanel: give at least 3 points as [[x, y, z], ...] in cm");

            Node* op = nullptr;
            try
            {
                op = &garmentArg(g, a, "tailorDrawPanel");
            }
            catch (const CommandError&)
            {
                op = nullptr;
            }
            Node* collider = nullptr;
            if (op)
            {
                const std::optional<PlugRef> src = g.source({op->id(), plugIndex(*op, "colliderMesh")});
                collider = src ? g.node(src->node) : nullptr;
            }
            else
                collider = colliderArg(g, a);

            if (collider && a.get<bool>("flatten"))
            {
                // The outline lies on the body: points drawn off it go to the nearest point of its surface.
                g.evaluate();
                const PlugValue bv = g.value({collider->id(), MeshNode::kOutMesh});
                const PlugValue bw = g.value({collider->id(), Transform::kWorldMatrix});
                if (const MeshPtr* bm = std::get_if<MeshPtr>(&bv); bm && *bm && std::get_if<glm::dmat4>(&bw))
                {
                    std::vector<Vec3> bp;
                    std::vector<std::array<int, 3>> bt;
                    colliderTriangles(**bm, std::get<glm::dmat4>(bw), bp, bt);
                    auto at = [&](int v) { return glm::dvec3(bp[size_t(v)][0], bp[size_t(v)][1], bp[size_t(v)][2]); };
                    for (glm::dvec3& p : pts)
                    {
                        double best = 1e30;
                        glm::dvec3 foot = p;
                        for (const auto& t : bt)
                        {
                            const glm::dvec3 o = at(t[0]), e1 = at(t[1]) - o, e2 = at(t[2]) - o, rel = p - o;
                            const double d11 = glm::dot(e1, e1), d12 = glm::dot(e1, e2), d22 = glm::dot(e2, e2), den = d11 * d22 - d12 * d12;
                            if (den <= 1e-20)
                                continue;
                            double v = (d22 * glm::dot(rel, e1) - d12 * glm::dot(rel, e2)) / den, w = (d11 * glm::dot(rel, e2) - d12 * glm::dot(rel, e1)) / den;
                            v = std::clamp(v, 0.0, 1.0);
                            w = std::clamp(w, 0.0, 1.0 - v);
                            const glm::dvec3 q = o + e1 * v + e2 * w;
                            if (const double d = glm::length(q - p); d < best)
                                best = d, foot = q;
                        }
                        p = foot;
                    }
                }
            }

            // The body's vertical axis: its feet centre, else the outline's own centre.
            glm::dvec2 axis(0.0);
            if (collider)
            {
                Vec3 feet{0.f, 0.f, 0.f};
                measureCollider(g, *collider, feet);
                axis = {feet[0], feet[2]};
            }
            else
            {
                for (const glm::dvec3& p : pts)
                    axis += glm::dvec2(p.x, p.z) / double(pts.size());
            }

            std::vector<double> theta(pts.size());
            double radius = 0.0;
            for (size_t i = 0; i < pts.size(); ++i)
            {
                theta[i] = std::atan2(pts[i].x - axis.x, pts[i].z - axis.y);
                // Keep the angle continuous along the outline so a panel crossing the back doesn't jump a full turn.
                if (i > 0)
                    while (theta[i] - theta[i - 1] > std::numbers::pi)
                        theta[i] -= 2.0 * std::numbers::pi;
                if (i > 0)
                    while (theta[i] - theta[i - 1] < -std::numbers::pi)
                        theta[i] += 2.0 * std::numbers::pi;
                radius += glm::length(glm::dvec2(pts[i].x, pts[i].z) - axis) / double(pts.size());
            }
            radius += a.get<double>("clearance") / kCmPerM;
            double centre = 0.0;
            for (double t : theta)
                centre += t / double(theta.size());

            double placeRadius = radius;
            std::vector<Vec2> shape(pts.size());
            for (size_t i = 0; i < pts.size(); ++i)
                shape[i] = {float((theta[i] - centre) * radius), float(pts[i].y)};
            if (a.get<bool>("flatten") && collider)
            {
                g.evaluate();
                const PlugValue bv = g.value({collider->id(), MeshNode::kOutMesh});
                const PlugValue bw = g.value({collider->id(), Transform::kWorldMatrix});
                if (const MeshPtr* bm = std::get_if<MeshPtr>(&bv); bm && *bm && std::get_if<glm::dmat4>(&bw))
                {
                    std::vector<Vec3> bodyPts;
                    std::vector<std::array<int, 3>> bodyTris;
                    colliderTriangles(**bm, std::get<glm::dmat4>(bw), bodyPts, bodyTris);
                    const double c0 = centre;
                    auto unroll = [&](const glm::dvec3& q) {
                        double t = std::atan2(q.x - axis.x, q.z - axis.y);
                        while (t - c0 > std::numbers::pi)
                            t -= 2.0 * std::numbers::pi;
                        while (t - c0 < -std::numbers::pi)
                            t += 2.0 * std::numbers::pi;
                        return Vec2{float((t - c0) * radius), float(q.y)};
                    };
                    std::vector<Vec2> flat = flattenOnBody(bodyPts, bodyTris, pts, shape, unroll);
                    if (flat.size() == shape.size())
                    {
                        // True lengths run shorter than the unroll at the clearance radius; place on the radius they fit.
                        double fx = 0.0, ux = 0.0, num = 0.0, den = 0.0;
                        for (size_t i = 0; i < flat.size(); ++i)
                            fx += flat[i][0] / double(flat.size()), ux += shape[i][0] / double(flat.size());
                        for (size_t i = 0; i < flat.size(); ++i)
                        {
                            num += (flat[i][0] - fx) * (shape[i][0] - ux);
                            den += (shape[i][0] - ux) * (shape[i][0] - ux);
                        }
                        if (den > 1e-12 && num > 0.0)
                            placeRadius = std::min(radius, radius * num / den + a.get<double>("clearance") / kCmPerM);
                        shape = std::move(flat);
                    }
                    else
                        log::warn("tailorDrawPanel: couldn't cut the body out under the outline; unrolled it instead");
                }
            }
            const size_t firstNew = op ? readGarment(*op).panels.size() : 0;
            Garment gm = op ? readGarment(*op) : Garment{};
            auto makePanel = [&](const std::string& base, double side) {
                Panel p;
                p.name = base;
                for (int k = 2; std::any_of(gm.panels.begin(), gm.panels.end(), [&](const Panel& o) { return o.name == p.name; }); ++k)
                    p.name = base + " " + std::to_string(k);
                for (const Vec2& q : shape)
                    p.outline.push_back({float(side) * q[0], q[1]});
                double area = 0.0;
                for (size_t i = 0; i < p.outline.size(); ++i)
                {
                    const Vec2 &u = p.outline[i], &v = p.outline[(i + 1) % p.outline.size()];
                    area += double(u[0]) * v[1] - double(v[0]) * u[1];
                }
                if (area < 0.0)
                    std::reverse(p.outline.begin(), p.outline.end());
                // Pattern x runs around the body at this radius, y is height; see Panel's placement convention.
                const double c = centre;
                p.origin = {float(axis.x + placeRadius * std::sin(c)), 0.f, float(axis.y + placeRadius * std::cos(c))};
                p.right = {float(std::cos(c)), 0.f, float(-std::sin(c))};
                p.up = {0.f, 1.f, 0.f};
                p.wrapRadius = float(placeRadius);
                p.layer = int(a.get<int64_t>("layer"));
                gm.panels.push_back(std::move(p));
                return gm.panels.back().name;
            };
            json names = json::array();
            const std::string base = a.get<std::string>("name");
            names.push_back(makePanel(a.get<bool>("mirror") ? base + " L" : base, 1.0));
            if (a.get<bool>("mirror"))
            {
                // Mirror across x = axis: the angle flips sign.
                centre = -centre;
                names.push_back(makePanel(base + " R", -1.0));
            }
            if (a.get<bool>("sew"))
                for (Seam& s : suggestSeams(gm, float(2.0 * a.get<double>("clearance") / kCmPerM + 0.03)))
                    if (size_t(s.a.front().panel) >= firstNew || size_t(s.b.front().panel) >= firstNew)
                        gm.seams.push_back(std::move(s));
            if (op)
            {
                writeGarment(g, *op, gm);
                return json::array({op->path(), names});
            }
            const json made = createGarment(gm, std::string(), a, "tailorDrawPanel", "Garment");
            return json::array({made[1], names});
        },
        .examples = {"tailorDrawPanel(points=[[-15, 140, 12], [15, 140, 12], [15, 100, 14], [-15, 100, 14]], name='Bib', collider='Mannequin')"},
    });

    ctx.registerCommand({
        .name = "tailorBindRenderMesh",
        .doc = "Latch a render mesh to a garment's cloth, like APEX's latch to nearest: every render point follows the "
               "nearest cloth triangle with its offset kept, so a thick, sculpted or retopologised mesh moves with the drape, "
               "live drapes included. Both meshes are taken as they are now; keep their transforms the same (freeze them). "
               "Returns the tailorBinding node.",
        .positionalDoc = "garment",
        .flags = {nodeFlag({"mesh", "m", T::String, json(), "The render mesh to latch."}, "mesh")},
        .run = [](const Args& a) -> json {
            Graph& g = Graph::current();
            Node& op = garmentArg(g, a, "tailorBindRenderMesh");
            Node* render = a.has("mesh") ? g.find(a.get<std::string>("mesh")) : nullptr;
            if (!render || !render->type().isA("mesh"))
                throw CommandError("tailorBindRenderMesh: give mesh, the render mesh to latch");
            {
                DrapeNow now;
                g.evaluate();
            }
            const PlugValue sim = g.value({op.id(), plugIndex(op, "outMesh")});
            const PlugValue rest = g.value({render->id(), MeshNode::kOutMesh});
            if (!std::get_if<MeshPtr>(&sim) || !std::get<MeshPtr>(sim) || !std::get_if<MeshPtr>(&rest) || !std::get<MeshPtr>(rest))
                throw CommandError("tailorBindRenderMesh: the garment or the render mesh has no geometry");
            Node* bind = g.createNode(*types::find(kBindingType), {}, kNoNode, true);
            g.setValue({bind->id(), plugIndex(*bind, "restMesh")}, std::get<MeshPtr>(rest));
            g.setValue({bind->id(), plugIndex(*bind, "bindSim")}, std::get<MeshPtr>(sim));
            g.connect({op.id(), plugIndex(op, "outMesh")}, {bind->id(), plugIndex(*bind, "simMesh")});
            g.connect({bind->id(), plugIndex(*bind, "outMesh")}, {render->id(), MeshNode::kInMesh});
            return bind->path();
        },
        .examples = {"tailorBindRenderMesh('Cloak', mesh='SM_Cloak_Thick')"},
    });

    ctx.registerCommand({
        .name = "tailorCheck",
        .doc = "Check a garment's pattern the way a pattern maker would: sleeve cap ease, dropped-shoulder sleeves on an "
               "undropped shoulder, seams that don't match, ease round the chest against the collider, panels too small for the "
               "cloth, crossed outlines, grain. Each finding comes with a tip, shows in the Pattern Editor and as a diagnostic "
               "on the garment mesh. Returns the findings.",
        .positionalDoc = "garment",
        .run = [](const Args& a) -> json {
            Graph& g = Graph::current();
            return checkGarment(g, garmentArg(g, a, "tailorCheck"));
        },
        .examples = {"tailorCheck('Tunic')"},
    });

    ctx.registerCommand({
        .name = "tailorExportAnimation",
        .doc = "Export an animated garment's simulation for a game or a cutscene: simulates the frames first, then writes an "
               "Alembic file (an Unreal Geometry Cache) or vertex animation textures. Returns what the exporter returns.",
        .positionalDoc = "garment",
        .flags = {{"format", "fmt", T::String, "alembic", "alembic or vat.", {"alembic", "vat"}},
                  {"start", "st", T::Int, json(), "First frame; the garment's startFrame when left out."},
                  {"end", "e", T::Int, json(), "Last frame, inclusive."},
                  {"path", "p", T::String, json(), "Alembic: the .abc file. VAT: the output folder."}},
        .run = [](const Args& a) -> json {
            Graph& g = Graph::current();
            Node& op = garmentArg(g, a, "tailorExportAnimation");
            if (!op.as<bool>(plugIndex(op, "animate")))
                throw CommandError("tailorExportAnimation: " + op.name() + " isn't animated; turn on its animate attribute");
            const std::vector<PlugRef> fed = g.destinations({op.id(), plugIndex(op, "outMesh")});
            const Node* mesh = fed.empty() ? nullptr : g.node(fed.front().node);
            if (!mesh)
                throw CommandError("tailorExportAnimation: the garment feeds no mesh");
            json call = {{"args", {mesh->path()}}};
            const int64_t start = a.has("start") ? a.get<int64_t>("start") : op.as<int64_t>(plugIndex(op, "startFrame"));
            call["start"] = double(start);
            if (!a.has("end"))
                throw CommandError("tailorExportAnimation: give end, the last frame");
            call["end"] = double(a.get<int64_t>("end"));
            // Every exported frame must be in the cache: the garment shows cached frames as the exporter steps time.
            ops::execute("simulate", {{"args", {op.path()}}, {"frame", a.get<int64_t>("end")}});
            const bool vat = a.get<std::string>("format") == "vat";
            if (a.has("path"))
                call[vat ? "folder" : "path"] = a.get<std::string>("path");
            return ops::execute(vat ? "exportVertexAnimation" : "exportAlembic", call);
        },
        .examples = {"tailorExportAnimation('Cloak', end=120, path='anim/cloak.abc')", "tailorExportAnimation('Cloak', format='vat')"},
    });

    ctx.registerCommand({
        .name = "tailorRefit",
        .doc = "Move a garment onto another body: grades the pattern by the bodies' measurements (widths by chest, lengths by "
               "height), keeps each set-in sleeve cap's ease to its armhole, stands it where the new body stands and drapes it "
               "there. Returns the scale factors used.",
        .positionalDoc = "garment",
        .flags = {nodeFlag({"collider", "c", T::String, json(), "The new body mesh."}, "mesh"),
                  {"grade", "gr", T::Bool, true, "Scale the pattern to the new body; off only moves it."}},
        .run = [](const Args& a) -> json {
            Graph& g = Graph::current();
            Node& op = garmentArg(g, a, "tailorRefit");
            Node* target = a.has("collider") ? g.find(a.get<std::string>("collider")) : nullptr;
            if (!target || !target->type().isA("mesh"))
                throw CommandError("tailorRefit: give collider, the new body mesh");
            Body from;
            Vec3 fromFeet{0.f, 0.f, 0.f};
            if (const std::optional<PlugRef> src = g.source({op.id(), plugIndex(op, "colliderMesh")}))
                if (Node* old = g.node(src->node))
                    from = measureCollider(g, *old, fromFeet);
            Vec3 toFeet{0.f, 0.f, 0.f};
            const Body to = measureCollider(g, *target, toFeet);
            const bool grade = a.get<bool>("grade");
            const float sx = grade ? to.chest / std::max(from.chest, 0.01f) : 1.f;
            const float sy = grade ? to.torso() / std::max(from.torso(), 0.01f) : 1.f;
            Garment gm = readGarment(op);
            gradePattern(gm, sx, sy);
            for (Panel& p : gm.panels)
                p.origin = {toFeet[0] + (p.origin[0] - fromFeet[0]) * sx, toFeet[1] + (p.origin[1] - fromFeet[1]) * sy,
                            toFeet[2] + (p.origin[2] - fromFeet[2]) * sx};
            // Pins go where the grading takes them, then hold onto the new body.
            for (GarmentPin& p : gm.pins)
            {
                p.target = {toFeet[0] + (p.target[0] - fromFeet[0]) * sx, toFeet[1] + (p.target[1] - fromFeet[1]) * sy,
                            toFeet[2] + (p.target[2] - fromFeet[2]) * sx};
                p.bodyTri = -1;
            }
            g.connect({target->id(), MeshNode::kOutMesh}, {op.id(), plugIndex(op, "colliderMesh")}, true);
            g.connect({target->id(), Transform::kWorldMatrix}, {op.id(), plugIndex(op, "colliderMatrix")}, true);
            attachToCollider(g, op, gm);
            writeGarment(g, op, gm);
            return {{"width", sx}, {"length", sy}};
        },
        .examples = {"tailorRefit('Tunic', collider='Hero')"},
    });

    ctx.registerCommand({
        .name = "tailorFreeze",
        .doc = "Freeze panels so they collide without moving, or set them back to active, deactivated or strengthened.\n\n"
               "Returns the panels changed.",
        .positionalDoc = "garment",
        .flags = {{"panel", "p", T::List, json(), "Panels by name or index; all when left out."},
                  {"state", "s", T::String, "frozen", "What the panels become.", {"active", "frozen", "deactivated", "strengthened"}}},
        .run = [](const Args& a) -> json {
            PanelState state;
            if (!parsePanelState(a.get<std::string>("state"), &state))
                throw CommandError("tailorFreeze: unknown state " + a.get<std::string>("state"));
            Graph& g = Graph::current();
            Node& op = garmentArg(g, a, "tailorFreeze");
            Garment gm = readGarment(op);
            json changed = json::array();
            for (int p : panelsArg(gm, a, "panel", "tailorFreeze"))
            {
                gm.panels[size_t(p)].state = state;
                changed.push_back(gm.panels[size_t(p)].name);
            }
            writeGarment(g, op, gm);
            return changed;
        },
        .examples = {"tailorFreeze('Tunic')", "tailorFreeze('Cloak', panel=['Hood'], state='active')"},
    });
}
} // namespace tailor
