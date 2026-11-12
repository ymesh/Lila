// S1: the Seam Points and Distribute Along Seam geometry nodes, seamScatter and scatterProximity.

#include "Scatter.h"
#include "Seamwork.h"

#include <lila/Command.h>
#include <lila/Extension.h>
#include <lila/GeometryNodes.h>
#include <lila/Mesh.h>
#include <lila/Profile.h>
#include <lila/Transform.h>

#include <glm/gtc/matrix_transform.hpp>

#include <algorithm>
#include <cmath>
#include <set>

using namespace lila;
using namespace lila::scene;

namespace seamwork
{
using T = ArgType;

namespace
{
std::string protoName(const std::string& asset)
{
    std::string s = asset.substr(asset.find_last_of('/') + 1);
    for (char& c : s)
        if (!std::isalnum(static_cast<unsigned char>(c)))
            c = '_';
    return s + "Proto";
}

/**
 * A Stagehand library asset as one hidden mesh under parent: placed once as a copy at the origin, every mesh of it baked
 * into one with its placed transform, and the placement deleted again.
 */
NodeId libraryPrototype(Graph& g, const std::string& asset, NodeId parent, const std::string& cmd)
{
    const json placed = ops::execute("place", {{"args", {asset}}, {"at", {0.0, 0.0, 0.0}}, {"snap", false}, {"mode", "copy"}});
    const json first = placed.is_array() && !placed.empty() ? placed[0] : placed;
    const NodeId root = g.find(first.value("node", "")) ? g.find(first.value("node", ""))->id() : kNoNode;
    g.evaluate();
    geo::Mesh merged;
    for (const json& path : first.value("meshes", json::array()))
    {
        Node* n = g.find(path.get<std::string>());
        const MeshPtr m = n ? n->as<MeshPtr>(MeshNode::kOutMesh) : nullptr;
        if (!m || m->numFaces() == 0)
            continue;
        geo::Mesh part = *m;
        const glm::dmat4 xf = n->as<glm::dmat4>(Transform::kWorldMatrix);
        for (glm::dvec3& p : part.positions())
            p = glm::dvec3(xf * glm::dvec4(p, 1.0));
        const glm::mat3 nm = glm::mat3(glm::transpose(glm::inverse(glm::dmat3(xf))));
        for (glm::vec3& nrm : part.cornerAttrs().get<glm::vec3>(geo::attr::kNormal))
            nrm = glm::normalize(nm * nrm);
        if (glm::determinant(glm::dmat3(xf)) < 0.0)
            for (geo::Index f = 0; f < part.faceCapacity(); ++f)
                if (part.faceValid(f))
                    part.flipFace(f);
        merged.append(part);
    }
    if (root != kNoNode)
        ops::execute("delete", {{"args", {g.node(root)->path()}}});
    if (merged.numFaces() == 0)
        throw CommandError(cmd + ": library asset '" + asset + "' placed no mesh");
    Node* m = g.createNode(*types::find("mesh"), protoName(asset), parent);
    g.setValue({m->id(), MeshNode::kInMesh, -1}, MeshPtr(std::make_shared<geo::Mesh>(std::move(merged))));
    g.setValue({m->id(), m->type().attrIndex("visibility"), -1}, false);
    return m->id();
}
} // namespace

bool raySurface(const SeamData& d, const glm::dvec3& from, const glm::dvec3& dir, glm::dvec3& hit, glm::dvec3& normal, double range)
{
    double best = range;
    bool found = false;
    for (const Object& o : d.objects)
    {
        const glm::dmat4 inv = glm::inverse(o.world);
        geo::BvhHit h;
        if (o.bvh->raycast(glm::dvec3(inv * glm::dvec4(from, 1.0)), glm::dmat3(inv) * dir, h, best))
        {
            best = h.t;
            hit = glm::dvec3(o.world * glm::dvec4(h.point, 1.0));
            normal = glm::normalize(glm::transpose(glm::inverse(glm::dmat3(o.world))) * o.mesh->faceNormal(h.face));
            found = true;
        }
    }
    if (d.ground)
    {
        const Heightfield& f = *d.ground->field;
        const glm::dmat4 inv = glm::inverse(d.ground->world);
        const glm::dvec3 o(inv * glm::dvec4(from, 1.0)), dl = glm::dmat3(inv) * dir;
        const double step = f.cell * 0.25;
        double prev = 0.0;
        for (double t = step; t <= best; t += step)
        {
            const glm::dvec3 q = o + dl * t;
            if (!f.contains(q.x, q.z) || q.y > f.heightAt(q.x, q.z))
            {
                prev = t;
                continue;
            }
            // Bisect between the last point above and this one below.
            double lo = prev, hi = t;
            for (int k = 0; k < 30; ++k)
            {
                const double m = 0.5 * (lo + hi);
                const glm::dvec3 qm = o + dl * m;
                (qm.y > f.heightAt(qm.x, qm.z) ? lo : hi) = m;
            }
            const glm::dvec3 ql = o + dl * hi;
            hit = glm::dvec3(d.ground->world * glm::dvec4(ql.x, f.heightAt(ql.x, ql.z), ql.z, 1.0));
            normal = glm::normalize(glm::transpose(glm::inverse(glm::dmat3(d.ground->world))) * f.normalAt(ql.x, ql.z));
            found = true;
            break;
        }
    }
    return found;
}

namespace
{
std::vector<SeamPoint> pointsFromMesh(const geo::Mesh& m)
{
    std::vector<SeamPoint> out(m.numPoints());
    const auto& a = m.pointAttrs();
    auto f = [&](const char* name) { return a.get<float>(name); };
    auto v = [&](const char* name) { return a.get<glm::vec3>(name); };
    const auto angle = f("angle"), depth = f("depth"), gap = f("gap"), upf = f("upFacing"), exp = f("exposure"), layer = f("layer"), kind = f("kind");
    const auto oa = f("objectA"), ob = f("objectB");
    const auto na = v("normalA"), nb = v("normalB"), tg = v("tangent");
    geo::Index i = 0;
    for (geo::Index p = 0; p < m.pointCapacity(); ++p)
    {
        if (!m.pointValid(p))
            continue;
        SeamPoint& s = out[i++];
        s.position = m.position(p);
        s.curve = p;
        s.along = double(p);
        if (!angle.empty())
            s.angle = angle[p];
        if (!depth.empty())
            s.depth = depth[p];
        if (!gap.empty())
            s.gap = gap[p];
        if (!upf.empty())
            s.upFacing = upf[p];
        if (!exp.empty())
            s.exposure = exp[p];
        if (!layer.empty())
            s.layer = int(layer[p]);
        if (!kind.empty())
            s.kind = Kind(std::clamp(int(kind[p]), 0, 2));
        if (!oa.empty())
            s.a = int(oa[p]);
        if (!ob.empty())
            s.b = int(ob[p]);
        if (!na.empty())
            s.normalA = glm::dvec3(na[p]);
        if (!nb.empty())
            s.normalB = glm::dvec3(nb[p]);
        if (!tg.empty())
            s.tangent = glm::dvec3(tg[p]);
    }
    return out;
}

int kindMask(const std::string& text)
{
    int mask = 0;
    for (Kind k : {Kind::Ground, Kind::Mesh, Kind::Crease})
        if (text.find(kindName(k)) != std::string::npos)
            mask |= 1 << int(k);
    return mask == 0 ? 7 : mask;
}

gn::SocketDecl sock(std::string id, gn::SocketType t, json def, std::string doc, std::optional<double> lo = {}, std::optional<double> hi = {})
{
    gn::SocketDecl s{.id = std::move(id), .type = t, .defaultValue = std::move(def), .doc = std::move(doc)};
    s.min = lo;
    s.max = hi;
    return s;
}

using S = gn::SocketType;

gn::NodeDecl seamPointsDecl()
{
    return {
        .type = "SeamPoints",
        .label = "Seam Points",
        .category = "Point",
        .doc = "Keeps the contact points (from Object Info on a seam node's points object) that suit what will grow or gather there: by "
               "kind, wedge angle, exposure to the sky and depth, thinned to a density. Their facts stay on as point attributes.",
        .sockets = {.inputs = {{.id = "points", .type = S::Geometry},
                               sock("kinds", S::String, "ground mesh crease", "Kinds kept: ground, mesh, crease."),
                               sock("angleMin", S::Float, 0.0, "Narrowest wedge kept, degrees.", 0.0, 180.0),
                               sock("angleMax", S::Float, 180.0, "Widest wedge kept, degrees.", 0.0, 180.0),
                               sock("exposureMin", S::Float, 0.0, "Least sky visibility kept.", 0.0, 1.0),
                               sock("exposureMax", S::Float, 1.0, "Most sky visibility kept.", 0.0, 1.0),
                               sock("depthMax", S::Float, 0.0, "Deepest join kept, cm; 0 keeps all.", 0.0),
                               sock("density", S::Float, 1.0, "Share of the points kept.", 0.0, 1.0),
                               sock("seed", S::Int, 0, "Which points the density keeps.")},
                    .outputs = {{.id = "points", .type = S::Geometry}}},
        .exec =
            [](gn::ExecContext& c) {
                LILA_ZONE();

                gn::GeometryPtr g = c.geometry("points");
                const geo::Mesh* src = g->points ? g->points.get() : g->mesh.get();
                if (!src)
                {
                    c.out("points", gn::makeGeometry({}));
                    return;
                }
                ScatterOptions o;
                o.kinds = kindMask(c.s("kinds"));
                o.angleMin = c.f("angleMin");
                o.angleMax = c.f("angleMax");
                o.exposureMin = c.f("exposureMin");
                o.exposureMax = c.f("exposureMax");
                o.depthMax = c.f("depthMax");
                const double density = std::clamp(c.f("density"), 0.0, 1.0);
                const uint32_t seed = uint32_t(c.i("seed"));
                geo::Mesh out = gn::pointsOnly(*src);
                const std::vector<SeamPoint> pts = pointsFromMesh(*src);
                std::vector<geo::Index> drop;
                geo::Index i = 0;
                for (geo::Index p = 0; p < out.pointCapacity(); ++p)
                {
                    if (!out.pointValid(p))
                        continue;
                    const SeamPoint& s = pts[i++];
                    const uint32_t h = (p * 2654435761u) ^ (seed * 40503u + 0x9E37u);
                    const bool thin = double((h >> 8) & 0xFFFF) / 65535.0 >= density;
                    if (!accepts(s, o) || thin)
                        drop.push_back(p);
                }
                for (geo::Index p : drop)
                    out.removePoint(p);
                out.compact();
                c.out("points", gn::pointGeometry(std::move(out)));
            },
    };
}

gn::NodeDecl distributeDecl()
{
    return {
        .type = "DistributeAlongSeam",
        .label = "Distribute Along Seam",
        .category = "Instances",
        .doc = "Instances along contact points: wedged into the crease along the bisector of both surfaces and sunk by part of their "
               "height, in clumps that thin out away from the join.",
        .props = {{.id = "side", .options = sideNames(), .defaultValue = "ground", .doc = "Spread onto the ground, up the wall, or both."}},
        .sockets = {.inputs = {{.id = "points", .type = S::Geometry},
                               {.id = "instance", .type = S::Geometry},
                               sock("clump", S::Int, 1, "Instances per point.", 1.0, 64.0),
                               sock("clumpRadius", S::Float, 10.0, "Clump size, cm.", 0.0),
                               sock("spread", S::Float, 0.0, "How far instances spread from the join, cm, thinning out.", 0.0),
                               sock("wedge", S::Bool, true, "Lean into the crease."),
                               sock("sink", S::Float, 0.1, "Share of each instance's height pushed below the surface.", 0.0, 1.0),
                               sock("scaleMin", S::Float, 0.8, "Smallest scale.", 0.0),
                               sock("scaleMax", S::Float, 1.2, "Largest scale.", 0.0),
                               sock("tilt", S::Float, 10.0, "Random tilt, degrees.", 0.0, 90.0),
                               sock("seed", S::Int, 0, "Variation seed.")},
                    .outputs = {{.id = "instances", .type = S::Geometry}}},
        .exec =
            [](gn::ExecContext& c) {
                LILA_ZONE();

                gn::GeometryPtr g = c.geometry("points");
                gn::GeometryPtr inst = c.geometry("instance");
                const geo::Mesh* src = g->points ? g->points.get() : g->mesh.get();
                if (!src || !inst || !inst->mesh || inst->mesh->numPoints() == 0)
                {
                    c.out("instances", gn::makeGeometry({}));
                    return;
                }
                const geo::Bounds b = inst->mesh->bounds();
                AssetShape shape{std::max(1e-3, b.max.y - b.min.y), std::max(1e-3, 0.5 * std::max(b.max.x - b.min.x, b.max.z - b.min.z))};
                ScatterOptions o;
                o.clump = int(c.i("clump"));
                o.clumpRadius = c.f("clumpRadius");
                o.spread = c.f("spread");
                o.wedge = c.b("wedge");
                o.sink = c.f("sink");
                o.scaleMin = c.f("scaleMin");
                o.scaleMax = c.f("scaleMax");
                o.tilt = c.f("tilt");
                o.seed = uint32_t(c.i("seed"));
                const std::string side = c.prop("side");
                for (size_t k = 0; k < sideNames().size(); ++k)
                    if (sideNames()[k] == side)
                        o.side = Side(k);
                const auto placed = distributePoints(pointsFromMesh(*src), {shape}, o);
                // The instance's bounds' bottom centre goes on each placement.
                const glm::dmat4 toBase = glm::translate(glm::dmat4(1.0), -glm::dvec3(b.center().x, b.min.y, b.center().z));
                gn::Instances out;
                out.references.push_back(inst);
                for (const Placement& p : placed)
                    out.add(p.world * toBase, 0);
                gn::Geometry geo;
                geo.instances = std::make_shared<const gn::Instances>(std::move(out));
                c.out("instances", gn::makeGeometry(std::move(geo)));
            },
    };
}

void setWorld(Graph& g, NodeId id, const glm::dmat4& world)
{
    const glm::dmat4 parent = g.node(id)->as<glm::dmat4>(Transform::kParentMatrix);
    Transform::setLocalMatrix(g, id, glm::inverse(parent) * world);
}

void tagOutput(Graph& g, NodeId id, const char* attr, const PlugValue& v, PlugType type)
{
    Node* n = g.node(id);
    int i = n->type().attrIndex(attr);
    if (i < 0)
        i = g.addAttr(id, {.name = attr, .type = type, .defaultValue = v, .flags = AttrInput | AttrStorable | AttrUser | AttrHidden, .doc = "Seamwork."});
    g.setValue({id, i, -1}, v);
}

std::vector<ProximityRule> parseRules(Graph& g, const json& rules, const std::string& cmd)
{
    std::vector<ProximityRule> out;
    if (!rules.is_array())
        return out;
    for (const json& r : rules)
    {
        ProximityRule rule;
        const std::string kind = r.value("kind", "repel");
        const auto it = std::find(ruleNames().begin(), ruleNames().end(), kind);
        if (it == ruleNames().end())
            throw CommandError(cmd + ": rule kind '" + kind + "' is not one of repel, shrink, tilt, cullInside, cullOutside");
        rule.kind = RuleKind(it - ruleNames().begin());
        const std::string target = r.value("target", "");
        Node* n = g.find(target);
        if (!n || !n->type().isA("mesh"))
            throw CommandError(cmd + ": rule target '" + target + "' is not a mesh");
        g.evaluate();
        rule.target.name = n->path();
        rule.target.mesh = n->as<MeshPtr>(MeshNode::kOutMesh);
        if (!rule.target.mesh)
            throw CommandError(cmd + ": rule target '" + target + "' has no mesh");
        rule.target.bvh = bvhOf(rule.target.mesh);
        rule.target.world = n->as<glm::dmat4>(Transform::kWorldMatrix);
        rule.radius = r.value("radius", 100.0);
        rule.strength = r.value("strength", rule.kind == RuleKind::Tilt ? 30.0 : rule.kind == RuleKind::Shrink ? 0.3 : 1.0);
        out.push_back(std::move(rule));
    }
    return out;
}

const char* kRulesDoc = "Proximity rules, applied in order: [{'kind': 'repel'|'shrink'|'tilt'|'cullInside'|'cullOutside', 'target': mesh, "
                        "'radius': cm, 'strength': repel share, smallest scale or tilt degrees}].";
} // namespace

bool registerScatterNodes(ext::Context& ctx)
{
    return gn::registerDecl(ctx, seamPointsDecl()) != nullptr && gn::registerDecl(ctx, distributeDecl()) != nullptr;
}

void registerScatterCommands(ext::Context& ctx)
{
    std::vector<std::string> presetNames;
    std::string presetDocs;
    for (const Preset& p : presets())
    {
        presetNames.push_back(p.name);
        presetDocs += "\n- " + p.name + ": " + p.doc;
    }
    ctx.registerCommand({
        .name = "seamScatter",
        .doc = "Spawn things along a seam node's contacts: rubble, gravel, grass, moss, roots, snow or leaves from a preset, or your own "
               "meshes and library assets.\n\nEach is an instance of a hidden prototype, wedged into the crease and sunk a little, in "
               "clumps that thin out away from the join. settle=True lets Stagehand settle them; check runs dressCheck after. Flags "
               "left out take the preset's values. Presets:" + presetDocs,
        .positionalDoc = "seam node (or its curves or points object)",
        .flags = {{"preset", "p", T::String, "rubble", "Settings and built-in asset.", presetNames},
                  {"items", "it", T::List, nullptr, "Meshes or Stagehand library asset ids to scatter instead of the preset's asset."},
                  {"density", "d", T::Float, nullptr, "Clumps per metre of contact.", {}, {}, 0.0},
                  {"clump", "cl", T::Int, nullptr, "Things per clump.", {}, {}, 1.0, 64.0},
                  {"clumpRadius", "cr", T::Float, nullptr, "Clump size, cm.", {}, {}, 0.0},
                  {"spread", "sp", T::Float, nullptr, "How far from the contact things spread, cm.", {}, {}, 0.0},
                  {"side", "si", T::String, nullptr, "Spread onto the ground, up the wall, or both.", sideNames()},
                  {"wedge", "w", T::Bool, nullptr, "Lean into the crease."},
                  {"sink", "sk", T::Float, nullptr, "Share of each thing's height below the surface.", {}, {}, 0.0, 1.0},
                  {"scaleMin", "smn", T::Float, nullptr, "Smallest scale.", {}, {}, 0.0},
                  {"scaleMax", "smx", T::Float, nullptr, "Largest scale.", {}, {}, 0.0},
                  {"tilt", "ti", T::Float, nullptr, "Random tilt, degrees.", {}, {}, 0.0, 90.0},
                  {"kinds", "k", T::List, nullptr, "Contact kinds taken: ground, mesh, crease."},
                  {"angleMin", "amn", T::Float, nullptr, "Narrowest wedge taken, degrees."},
                  {"angleMax", "amx", T::Float, nullptr, "Widest wedge taken, degrees."},
                  {"exposureMin", "emn", T::Float, nullptr, "Least sky visibility taken, 0 to 1."},
                  {"exposureMax", "emx", T::Float, nullptr, "Most sky visibility taken, 0 to 1."},
                  {"depthMax", "dmx", T::Float, nullptr, "Deepest contact taken, cm; 0 takes all."},
                  {"seed", "s", T::Int, int64_t(0), "Variation seed."},
                  {"rules", "r", T::List, nullptr, kRulesDoc},
                  {"settle", "st", T::Bool, false, "Settle them with Stagehand physics."},
                  {"check", "ck", T::Bool, true, "Run dressCheck on them after settling."},
                  {"name", "n", T::String, nullptr, "Name of the group they go under."}},
        .run = [](const Args& a) -> json {
            LILA_ZONE();

            Graph& g = Graph::current();
            const std::string cmd = "seamScatter";
            const auto names = a.objects();
            if (names.size() != 1)
                throw CommandError(cmd + ": give one seam node");
            NodeId sid = kNoNode;
            const auto d = seamDataNamed(g, names[0], cmd, &sid);
            const std::string presetName = a.get<std::string>("preset", "rubble");
            const Preset* pr = preset(presetName);
            if (!pr)
                throw CommandError(cmd + ": no preset '" + presetName + "'");
            ScatterOptions o = pr->options;
            o.up = d->up;
            o.seed = uint32_t(a.get<int64_t>("seed", 0));
            auto num = [&](const char* f, double& v) {
                if (a.has(f))
                    v = a.get<double>(f, v);
            };
            num("density", o.density);
            num("clumpRadius", o.clumpRadius);
            num("spread", o.spread);
            num("sink", o.sink);
            num("scaleMin", o.scaleMin);
            num("scaleMax", o.scaleMax);
            num("tilt", o.tilt);
            num("angleMin", o.angleMin);
            num("angleMax", o.angleMax);
            num("exposureMin", o.exposureMin);
            num("exposureMax", o.exposureMax);
            num("depthMax", o.depthMax);
            if (a.has("clump"))
                o.clump = int(a.get<int64_t>("clump", 1));
            if (a.has("wedge"))
                o.wedge = a.get<bool>("wedge", true);
            if (a.has("side"))
            {
                const auto it = std::find(sideNames().begin(), sideNames().end(), a.get<std::string>("side", "ground"));
                o.side = it == sideNames().end() ? Side::Ground : Side(it - sideNames().begin());
            }
            if (a.has("kinds"))
            {
                std::string all;
                for (const json& k : a.flags["kinds"])
                    all += k.get<std::string>() + " ";
                o.kinds = kindMask(all);
            }
            const std::vector<ProximityRule> rules = parseRules(g, a.flags.value("rules", json()), cmd);

            const std::string groupName = a.get<std::string>("name", g.node(sid)->name() + "_" + presetName);
            Node* group = g.createNode(*types::find("transform"), groupName);
            tagOutput(g, group->id(), "seamScatter", true, PlugType::Bool);
            tagOutput(g, group->id(), "seamSource", g.node(sid)->path(), PlugType::String);

            // Prototypes, hidden, with the matrix that puts each one's bounds' bottom centre at its pivot.
            struct Proto
            {
                NodeId node;
                glm::dmat4 toBase;
            };
            std::vector<Proto> protos;
            std::vector<AssetShape> shapes;
            auto addProto = [&](NodeId id) {
                g.evaluate();
                const MeshPtr m = g.node(id)->as<MeshPtr>(MeshNode::kOutMesh);
                if (!m || m->numPoints() == 0)
                    throw CommandError(cmd + ": " + g.node(id)->path() + " has no mesh");
                const geo::Bounds b = m->bounds();
                protos.push_back({id, glm::translate(glm::dmat4(1.0), -glm::dvec3(b.center().x, b.min.y, b.center().z))});
                shapes.push_back({std::max(1e-3, b.max.y - b.min.y), std::max(1e-3, 0.5 * std::max(b.max.x - b.min.x, b.max.z - b.min.z))});
            };
            json used = json::array();
            if (a.has("items"))
            {
                for (const json& item : a.flags["items"])
                {
                    const std::string name = item.get<std::string>();
                    if (Node* n = g.find(name); n && n->type().isA("mesh"))
                    {
                        addProto(n->id());
                        used.push_back(n->path());
                        continue;
                    }
                    if (!ops::find("place"))
                        throw CommandError(cmd + ": '" + name + "' is not a mesh, and library assets need Stagehand");
                    const NodeId proto = libraryPrototype(g, name, group->id(), cmd);
                    tagOutput(g, proto, "seamworkOutput", true, PlugType::Bool);
                    addProto(proto);
                    used.push_back(name);
                }
            }
            else
                for (int k = 0; k < pr->variants; ++k)
                {
                    Node* m = g.createNode(*types::find("mesh"), presetName + "Asset" + std::to_string(k + 1), group->id());
                    g.setValue({m->id(), MeshNode::kInMesh, -1}, MeshPtr(std::make_shared<geo::Mesh>(pr->asset(k, o.seed))));
                    g.setValue({m->id(), m->type().attrIndex("visibility"), -1}, false);
                    tagOutput(g, m->id(), "seamworkOutput", true, PlugType::Bool);
                    addProto(m->id());
                }

            const auto placed = distribute(*d->result, shapes, o, [&](const glm::dvec3& from, const glm::dvec3& dir, glm::dvec3& hit, glm::dvec3& n) {
                return raySurface(*d, from, dir, hit, n, 200.0);
            });
            json made = json::array();
            size_t culled = 0;
            double farthest = 0.0;
            const SegmentIndex seams(*d->result);
            for (size_t i = 0; i < placed.size(); ++i)
            {
                const Placement& p = placed[i];
                RuleOutcome r = applyRules(p.world, rules, d->up);
                if (r.culled)
                {
                    ++culled;
                    continue;
                }
                Node* m = g.createNode(*types::find("mesh"), presetName + "_1", group->id());
                instancing::connectInstance(g, protos[size_t(p.asset)].node, m->id());
                // The group sits at the origin, so local is world.
                Transform::setLocalMatrix(g, m->id(), r.world * protos[size_t(p.asset)].toBase);
                tagOutput(g, m->id(), "seamworkOutput", true, PlugType::Bool);
                made.push_back(m->path());
                farthest = std::max(farthest, seams.distance(glm::dvec3(r.world[3]), 1e9));
            }
            json out = {{"group", group->path()}, {"instances", made.size()}, {"culled", culled}, {"preset", presetName},
                        {"assets", used}, {"farthestFromContact", farthest}};
            if (a.get<bool>("settle", false) && !made.empty())
            {
                if (!ops::find("settle"))
                    out["settle"] = "skipped: Stagehand is not loaded";
                else
                {
                    for (const json& m : made)
                        ops::execute("physicsBody", {{"args", {m}}, {"role", "dynamic"}});
                    const json s = ops::execute("settle", {{"args", made}});
                    out["settle"] = {{"atRest", s.value("atRest", false)}};
                }
            }
            if (a.get<bool>("check", true) && a.get<bool>("settle", false) && ops::find("dressCheck") && !made.empty())
            {
                const json c = ops::execute("dressCheck", {{"args", made}, {"ground", false}});
                out["dressCheck"] = {{"ok", c["ok"]}, {"issues", c["issues"]}};
            }
            return out;
        },
        .examples = {"seamScatter('contacts', preset='rubble', density=4, settle=True)",
                     "seamScatter('contacts', items=['Rock_A', 'Rock_B'], spread=60, rules=[{'kind': 'repel', 'target': 'Door', 'radius': 150}])"},
    });

    ctx.registerCommand({
        .name = "scatterProximity",
        .doc = "Apply proximity rules to any scattered instances: repel, shrink or tilt them near chosen objects, or remove those inside "
               "or outside a closed volume. Works on any meshes (Stagehand scatter, seamScatter, hand-placed). Returns what changed.",
        .positionalDoc = "instances, or groups holding them (default the selection)",
        .flags = {{"rules", "r", T::List, json::array(), kRulesDoc}},
        .run = [](const Args& a) -> json {
            Graph& g = Graph::current();
            const std::string cmd = "scatterProximity";
            const std::vector<ProximityRule> rules = parseRules(g, a.flags["rules"], cmd);
            if (rules.empty())
                throw CommandError(cmd + ": give rules");
            std::vector<NodeId> nodes;
            for (const std::string& name : a.objects())
            {
                Node* n = g.find(name);
                if (!n)
                    throw CommandError(cmd + ": no node named '" + name + "'");
                if (n->type().isA("mesh"))
                    nodes.push_back(n->id());
                else
                    for (NodeId c : g.descendants(n->id()))
                        if (Node* m = g.node(c); m && m->type().isA("mesh") && effectivelyVisible(g, c, true))
                            nodes.push_back(c);
            }
            g.evaluate();
            const glm::dvec3 up = sceneUp();
            size_t moved = 0;
            json removed = json::array();
            for (NodeId id : nodes)
            {
                const glm::dmat4 w = g.node(id)->as<glm::dmat4>(Transform::kWorldMatrix);
                const RuleOutcome r = applyRules(w, rules, up);
                if (r.culled)
                {
                    removed.push_back(g.node(id)->path());
                    g.deleteNode(id);
                    continue;
                }
                if (r.world != w)
                {
                    setWorld(g, id, r.world);
                    ++moved;
                }
            }
            return {{"changed", moved}, {"removed", removed}, {"kept", nodes.size() - removed.size()}};
        },
        .examples = {"scatterProximity('contacts_rubble', rules=[{'kind': 'cullInside', 'target': 'Road'}, {'kind': 'shrink', 'target': 'Door', 'radius': 200}])"},
    });
}
} // namespace seamwork
