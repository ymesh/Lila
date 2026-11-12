// The seam node: objects and a terrain in, joins out (curves, points, per-object distance fields), live in the graph.

#include "Seamwork.h"

#include <lila/Command.h>
#include <lila/Curves.h>
#include <lila/Extension.h>
#include <lila/GeometryNodes.h>
#include <lila/Deformer.h>
#include <lila/Log.h>
#include <lila/Profile.h>

#include <tbb/parallel_for.h>

#include <algorithm>
#include <cmath>
#include <mutex>
#include <sstream>
#include <unordered_map>

using namespace lila;
using namespace lila::scene;

namespace seamwork
{
namespace
{
AttrDef param(std::string name, PlugType type, PlugValue def, std::string doc, std::string category, std::optional<double> lo = {},
              std::optional<double> hi = {})
{
    return {.name = std::move(name), .type = type, .defaultValue = std::move(def), .flags = AttrInput | AttrStorable | AttrKeyable,
            .doc = std::move(doc), .minValue = lo, .maxValue = hi, .category = std::move(category)};
}

AttrDef hiddenArray(std::string name, PlugType type, PlugValue def, bool storable, std::string doc)
{
    return {.name = std::move(name), .type = type, .defaultValue = std::move(def),
            .flags = AttrInput | AttrHidden | AttrArray | (storable ? AttrStorable : 0u), .doc = std::move(doc)};
}

int at(const Node& n, std::string_view name) { return n.type().attrIndex(name); }
double fv(const Node& n, std::string_view name) { return n.as<double>(at(n, name)); }
int64_t iv(const Node& n, std::string_view name) { return n.as<int64_t>(at(n, name)); }
bool bv(const Node& n, std::string_view name) { return n.as<bool>(at(n, name)); }
std::string sv(const Node& n, std::string_view name) { return n.as<std::string>(at(n, name)); }

uint64_t mix(uint64_t h, uint64_t v)
{
    h ^= v + 0x9E3779B97F4A7C15ull + (h << 6) + (h >> 2);
    return h;
}

uint64_t mixDouble(uint64_t h, double v) { return mix(h, uint64_t(int64_t(std::llround(v * 1000.0)))); }

uint64_t revisionOf(const Result& r, const Band& b, double range)
{
    uint64_t h = 1469598103934665603ull;
    for (const Curve& c : r.curves)
    {
        h = mix(h, uint64_t(c.kind) | uint64_t(uint32_t(c.a)) << 8 | uint64_t(uint32_t(c.b)) << 40);
        for (const glm::dvec3& p : c.points)
            h = mixDouble(mixDouble(mixDouble(h, p.x), p.y), p.z);
    }
    h = mixDouble(mixDouble(mixDouble(h, b.width), b.noise), b.noiseScale);
    h = mix(mix(mix(h, uint64_t(b.falloff)), b.seed), std::hash<std::string>()(b.curve));
    return mixDouble(h, range);
}

/** The ignorePairs attr: "a b; c d" by object name. */
std::vector<std::pair<int, int>> parsePairs(const std::string& text, const std::vector<std::string>& names)
{
    std::vector<std::pair<int, int>> out;
    std::stringstream all(text);
    std::string item;
    auto find = [&](const std::string& s) {
        for (size_t i = 0; i < names.size(); ++i)
            if (names[i] == s)
                return int(i);
        return -1;
    };
    while (std::getline(all, item, ';'))
    {
        std::stringstream one(item);
        std::string a, b;
        if (one >> a >> b)
            if (int ia = find(a), ib = find(b); ia >= 0 && ib >= 0)
                out.emplace_back(ia, ib);
    }
    return out;
}

curve::Splines curvesOut(const Result& r)
{
    curve::Splines out;
    out.reserve(r.curves.size());
    for (const Curve& c : r.curves)
    {
        curve::Spline s;
        s.kind = curve::Kind::Poly;
        s.closed = c.closed && c.points.size() > 2;
        s.points = c.points;
        auto& pa = s.pointAttrs;
        pa["angle"] = c.angle;
        pa["depth"] = c.depth;
        pa["gap"] = c.gap;
        pa["upFacing"] = c.upFacing;
        pa["exposure"] = c.exposure;
        std::vector<double>& layer = pa["layer"];
        for (int l : c.layer)
            layer.push_back(double(l));
        for (int k = 0; k < 3; ++k)
        {
            std::vector<double>& na = pa[std::string("normalA") + "XYZ"[k]];
            std::vector<double>& nb = pa[std::string("normalB") + "XYZ"[k]];
            for (size_t i = 0; i < c.points.size(); ++i)
            {
                na.push_back(c.normalA[i][k]);
                nb.push_back(c.normalB[i][k]);
            }
        }
        s.attrs["kind"] = double(int(c.kind));
        s.attrs["a"] = double(c.a);
        s.attrs["b"] = double(c.b);
        s.attrs["near"] = c.nearContact ? 1.0 : 0.0;
        out.push_back(std::move(s));
    }
    return out;
}

MeshPtr pointsOut(const std::vector<SeamPoint>& pts)
{
    auto m = std::make_shared<geo::Mesh>();
    for (const SeamPoint& p : pts)
        m->addPoint(p.position);
    auto& attrs = m->pointAttrs();
    auto put = [&](const char* name, auto get) {
        auto v = attrs.getOrAdd<float>(name);
        for (size_t i = 0; i < pts.size(); ++i)
            v[i] = float(get(pts[i]));
    };
    put("angle", [](const SeamPoint& p) { return p.angle; });
    put("depth", [](const SeamPoint& p) { return p.depth; });
    put("gap", [](const SeamPoint& p) { return p.gap; });
    put("upFacing", [](const SeamPoint& p) { return p.upFacing; });
    put("exposure", [](const SeamPoint& p) { return p.exposure; });
    put("layer", [](const SeamPoint& p) { return double(p.layer); });
    put("kind", [](const SeamPoint& p) { return double(int(p.kind)); });
    put("objectA", [](const SeamPoint& p) { return double(p.a); });
    put("objectB", [](const SeamPoint& p) { return double(p.b); });
    auto vec = [&](const char* name, auto get) {
        auto v = attrs.getOrAdd<glm::vec3>(name);
        for (size_t i = 0; i < pts.size(); ++i)
            v[i] = glm::vec3(get(pts[i]));
    };
    vec("normalA", [](const SeamPoint& p) { return p.normalA; });
    vec("normalB", [](const SeamPoint& p) { return p.normalB; });
    vec("tangent", [](const SeamPoint& p) { return p.tangent; });
    return m;
}

/**
 * The full-resolution heightfield behind the terrain mesh: the last geometry-node modifier of its stack, when that
 * heightfield covers what the mesh shows. Past about 4M samples the mesh is a stepped preview of it.
 */
std::shared_ptr<const Heightfield> stackHeightfield(const Node& seam, int terrainAttr, const Mesh& shown)
{
    Graph* g = const_cast<Graph*>(seam.graph());
    if (!g)
        return nullptr;
    const auto src = g->source({seam.id(), terrainAttr, -1});
    if (!src)
        return nullptr;
    const std::vector<NodeId> stack = scene::modifiers::stack(*g, src->node);
    for (auto it = stack.rbegin(); it != stack.rend(); ++it)
    {
        const Node* m = g->node(*it);
        if (!m || !gn::isGnModifier(*m))
            continue;
        auto h = gn::modifierHeightfield(*it);
        if (!h || h->empty())
            return nullptr;
        const geo::Bounds b = shown.bounds();
        const glm::dvec2 lo = h->origin, hi = h->origin + h->extent();
        const double tol = h->cell * 0.01;
        if (std::abs(b.min.x - lo.x) > tol || std::abs(b.min.z - lo.y) > tol || std::abs(b.max.x - hi.x) > tol || std::abs(b.max.z - hi.y) > tol)
            return nullptr;
        return h;
    }
    return nullptr;
}

class SeamNode final : public Node
{
public:
    void compute(const EvalContext&) override
    {
        LILA_ZONE();

        const int warn = at(*this, "warning");
        try
        {
            run();
            setOutput(warn, mWarning);
        }
        catch (const std::exception& e)
        {
            setOutput(at(*this, "outCurves"), DataPtr(curve::makeCurveData({})));
            setOutput(at(*this, "outPoints"), MeshPtr());
            setOutput(at(*this, "outSeams"), DataPtr());
            setOutput(warn, std::string(e.what()));
        }
        if (const std::string& w = as<std::string>(warn); !w.empty() && w != mLogged)
            log::warn("{}: {}", name(), w);
        mLogged = as<std::string>(warn);
    }

private:
    void run()
    {
        mWarning.clear();
        auto data = std::make_shared<SeamData>();
        std::vector<std::string> names;
        const int objAttr = at(*this, "objects"), matAttr = at(*this, "matrices"), nameAttr = at(*this, "names");
        const int flagAttr = at(*this, "flags"), groupAttr = at(*this, "groups");
        for (const auto& [index, value] : elements(objAttr))
        {
            const MeshPtr* mesh = std::get_if<MeshPtr>(&value);
            if (!mesh || !*mesh || (*mesh)->numFaces() == 0)
                continue;
            Object o;
            o.mesh = *mesh;
            o.bvh = bvhOf(*mesh);
            if (const PlugValue* m = element(matAttr, index))
                o.world = std::get<glm::dmat4>(*m);
            if (const PlugValue* s = element(nameAttr, index))
                o.name = std::get<std::string>(*s);
            if (o.name.empty())
                o.name = "object" + std::to_string(index);
            const int64_t flags = elementOr<int64_t>(flagAttr, index);
            o.ground = (flags & kFlagGround) != 0;
            o.ignore = (flags & kFlagIgnore) != 0;
            o.group = int(elementOr<int64_t>(groupAttr, index));
            names.push_back(o.name);
            data->objects.push_back(std::move(o));
        }

        const MeshPtr terrain = as<MeshPtr>(at(*this, "terrain"));
        const std::string terrainName = sv(*this, "terrainName");
        if (terrain && terrain->numPoints() > 0)
        {
            std::string why;
            auto hf = stackHeightfield(*this, at(*this, "terrain"), *terrain);
            if (!hf)
                hf = heightfieldFromMesh(*terrain, &why);
            if (hf)
            {
                auto g = std::make_shared<Ground>();
                g->field = std::move(hf);
                g->world = as<glm::dmat4>(at(*this, "terrainMatrix"));
                g->name = terrainName.empty() ? "terrain" : terrainName;
                data->ground = std::move(g);
                data->groundMode = "heightfield";
            }
            else
            {
                Object o;
                o.mesh = terrain;
                o.bvh = bvhOf(terrain);
                o.world = as<glm::dmat4>(at(*this, "terrainMatrix"));
                o.name = terrainName.empty() ? "terrain" : terrainName;
                o.ground = true;
                names.push_back(o.name);
                data->objects.push_back(std::move(o));
                data->groundMode = "mesh";
                mWarning = "terrain read as a mesh (" + why + ")";
            }
        }

        Options o;
        o.ground = bv(*this, "findGround");
        o.mesh = bv(*this, "findMesh");
        o.nearContact = bv(*this, "nearContact");
        o.gap = fv(*this, "gap");
        o.crease = bv(*this, "crease");
        o.creaseRange = fv(*this, "creaseRange");
        o.creaseThreshold = fv(*this, "creaseThreshold");
        o.minLength = fv(*this, "minLength");
        o.exposureRays = int(iv(*this, "exposureRays"));
        o.exposureRange = fv(*this, "exposureRange");
        o.up = as<glm::dvec3>(at(*this, "up"));
        o.ignorePairs = parsePairs(sv(*this, "ignorePairs"), names);
        data->up = o.up;

        auto result = std::make_shared<Result>(findSeams(data->objects, data->ground.get(), o));
        data->band = bandOf(*this);
        data->fieldRange = std::max(1.0, fv(*this, "fieldRange"));
        data->distances.resize(data->objects.size());
        for (size_t i = 0; i < data->objects.size(); ++i)
            if (!data->objects[i].ground && !data->objects[i].ignore)
                data->distances[i] = distanceField(data->objects[i], int(i), *result, data->fieldRange);
        data->points = samplePoints(*result, std::max(0.1, fv(*this, "spacing")));
        data->revision = revisionOf(*result, data->band, data->fieldRange);
        data->result = result;

        json pairs = json::array();
        for (const PairStats& p : result->pairs())
            pairs.push_back({{"a", objectName(*data, p.a)}, {"b", objectName(*data, p.b)}, {"kind", kindName(p.kind)}, {"curves", p.curves},
                             {"length", p.length}});
        size_t nearCount = 0;
        for (const Curve& c : result->curves)
            nearCount += c.nearContact ? 1 : 0;
        data->stats = {{"curves", result->curves.size()},
                       {"length", result->totalLength()},
                       {"nearContacts", nearCount},
                       {"points", data->points.size()},
                       {"pairs", pairs},
                       {"objects", names},
                       {"terrain", data->groundMode},
                       {"crossings", result->crossings},
                       {"layers", result->layers}};
        if (o.crease)
            data->stats["creaseBackend"] = result->creaseBackend;
        for (const std::string& w : result->warnings)
            mWarning += (mWarning.empty() ? "" : "; ") + w;

        setOutput(at(*this, "outCurves"), DataPtr(curve::makeCurveData(curvesOut(*result))));
        setOutput(at(*this, "outPoints"), pointsOut(data->points));
        setOutput(at(*this, "stats"), data->stats.dump());
        setOutput(at(*this, "outSeams"), DataPtr(std::move(data)));
    }

    std::string mWarning;
    std::string mLogged;
};

std::vector<AttrDef> seamAttrs()
{
    using P = PlugType;
    std::vector<AttrDef> a = {
        hiddenArray("objects", P::Mesh, MeshPtr(), false, "Each object's evaluated mesh."),
        hiddenArray("matrices", P::Matrix, glm::dmat4(1.0), false, "Each object's world matrix."),
        hiddenArray("names", P::String, std::string(), true, "Each object's name, for stats and the fields."),
        hiddenArray("groups", P::Int, int64_t(0), true, "Objects sharing a non-zero group never join each other."),
        hiddenArray("flags", P::Int, int64_t(0), true, "Per object: 1 ground, 2 ignored."),
        {.name = "terrain", .type = P::Mesh, .defaultValue = MeshPtr(), .flags = AttrInput | AttrHidden, .doc = "The terrain's evaluated mesh."},
        {.name = "terrainMatrix", .type = P::Matrix, .defaultValue = glm::dmat4(1.0), .flags = AttrInput | AttrHidden, .doc = "The terrain's world matrix."},
        {.name = "terrainName", .type = P::String, .defaultValue = std::string(), .flags = AttrInput | AttrStorable | AttrHidden, .doc = "The terrain mesh's name."},
        {.name = "up", .type = P::Vec3, .defaultValue = glm::dvec3(0, 1, 0), .flags = AttrInput | AttrStorable | AttrHidden, .doc = "Scene up."},
        param("findGround", P::Bool, true, "Find joins with the ground.", "Find"),
        param("findMesh", P::Bool, true, "Find joins between objects.", "Find"),
        param("nearContact", P::Bool, true, "Also join surfaces within gap of each other that don't cross.", "Find"),
        param("gap", P::Float, 2.0, "Largest gap that still counts as a contact, cm.", "Find", 0.0),
        param("crease", P::Bool, false, "Find tight creases from short-range occlusion.", "Find"),
        param("creaseRange", P::Float, 20.0, "How far crease rays look, cm.", "Find", 0.1),
        param("creaseThreshold", P::Float, 0.4, "Occlusion from which a point is in a crease, 0 to 1.", "Find", 0.0, 1.0),
        param("minLength", P::Float, 0.5, "Shorter joins are dropped, cm.", "Find", 0.0),
        {.name = "ignorePairs", .type = P::String, .defaultValue = std::string(), .flags = AttrInput | AttrStorable,
         .doc = "Object pairs never joined: \"wall rock; crate barrel\".", .category = "Find"},
        param("exposureRays", P::Int, int64_t(16), "Rays for each point's sky visibility; 0 skips it.", "Facts", 0.0, 256.0),
        param("exposureRange", P::Float, 2000.0, "How far something blocks the sky, cm.", "Facts", 1.0),
        param("width", P::Float, 30.0, "Band width either side of a join, cm.", "Band", 0.0),
        {.name = "falloff", .type = P::Enum, .defaultValue = int64_t(1), .flags = AttrInput | AttrStorable | AttrKeyable, .enumNames = falloffNames(),
         .doc = "How the band fades: linear, smooth, sharp, root or custom.", .category = "Band"},
        {.name = "falloffCurve", .type = P::String, .defaultValue = std::string("0:1 0.5:0.6 1:0"), .flags = AttrInput | AttrStorable,
         .doc = "Custom falloff \"distance:weight ...\", distance 0 at the join to 1 at the band's edge.", .category = "Band"},
        param("noise", P::Float, 0.0, "Width varies by up to this fraction with world noise.", "Band", 0.0, 4.0),
        param("noiseScale", P::Float, 150.0, "Size of the width noise, cm.", "Band", 1.0),
        param("seed", P::Int, int64_t(0), "Noise seed.", "Band"),
        param("fieldRange", P::Float, 200.0, "Distance fields stop here, cm.", "Band", 1.0),
        param("spacing", P::Float, 25.0, "Points along joins every this many cm.", "Points", 0.1),
        param("live", P::Bool, true, "Keep fields and the terrain mask up to date as objects move.", "Live"),
        param("terrainMask", P::Bool, true, "Write the contact mask into the terrain.", "Live"),
        {.name = "outCurves", .type = P::Data, .defaultValue = DataPtr(), .flags = AttrOutput | AttrHidden, .doc = "Join curves, world space."},
        {.name = "outPoints", .type = P::Mesh, .defaultValue = MeshPtr(), .flags = AttrOutput | AttrHidden, .doc = "Points along the joins."},
        {.name = "outSeams", .type = P::Data, .defaultValue = DataPtr(), .flags = AttrOutput | AttrHidden, .doc = "Everything the node found."},
        {.name = "stats", .type = P::String, .defaultValue = std::string(), .flags = AttrOutput, .doc = "What was found, as JSON."},
        {.name = "warning", .type = P::String, .defaultValue = std::string(), .flags = AttrOutput, .doc = "Problems with the last run."},
    };
    return a;
}

struct CacheEntry
{
    std::weak_ptr<const Mesh> mesh;
    std::shared_ptr<const MeshBvh> bvh;
};
std::mutex gBvhMutex;
std::unordered_map<const Mesh*, CacheEntry> gBvhCache;
} // namespace

size_t SeamData::bytes() const
{
    size_t b = sizeof(*this);
    if (result)
        for (const Curve& c : result->curves)
            b += c.points.size() * 160;
    for (const auto& d : distances)
        b += d.size() * sizeof(float);
    return b + points.size() * sizeof(SeamPoint);
}

const SeamData* seamDataOf(const PlugValue& v)
{
    const DataPtr* d = std::get_if<DataPtr>(&v);
    if (!d || !*d || std::string((*d)->kind()) != "seamwork")
        return nullptr;
    return static_cast<const SeamData*>(d->get());
}

const SeamData* seamDataOf(const Node& seamNode)
{
    const int a = seamNode.type().attrIndex("outSeams");
    return a < 0 ? nullptr : seamDataOf(seamNode.value(a));
}

NodeId seamNodeOf(Graph& g, NodeId id)
{
    Node* n = g.node(id);
    if (!n)
        return kNoNode;
    if (n->type().name == kSeamType)
        return id;
    // A curve or points mesh fed by a seam node, or a strip/skirt node or its mesh.
    for (int attr = 0; attr < int(n->type().attrs.size()); ++attr)
        if (auto src = g.source({id, attr, -1}))
        {
            Node* s = g.node(src->node);
            if (!s)
                continue;
            if (s->type().name == kSeamType)
                return s->id();
            if (s->type().name == kStripType || s->type().name == kSkirtType)
                return seamNodeOf(g, s->id());
        }
    return kNoNode;
}

std::shared_ptr<const SeamData> seamDataNamed(Graph& g, const std::string& name, const std::string& cmd, NodeId* idOut)
{
    Node* n = g.find(name);
    if (!n)
        throw CommandError(cmd + ": no node named '" + name + "'");
    const NodeId id = seamNodeOf(g, n->id());
    if (id == kNoNode)
        throw CommandError(cmd + ": " + name + " is not a seam node or fed by one (make one with seamFind)");
    if (idOut)
        *idOut = id;
    const PlugValue v = g.value({id, g.node(id)->type().attrIndex("outSeams"), -1});
    const DataPtr* d = std::get_if<DataPtr>(&v);
    if (!d || !seamDataOf(v))
    {
        const std::string w = g.node(id)->as<std::string>(g.node(id)->type().attrIndex("warning"));
        throw CommandError(cmd + ": " + g.node(id)->path() + " has no result" + (w.empty() ? "" : ": " + w));
    }
    return std::static_pointer_cast<const SeamData>(*d);
}

std::string objectName(const SeamData& d, int index)
{
    if (index == kTerrain)
        return d.ground ? d.ground->name : "terrain";
    if (index == kNone || index < 0 || size_t(index) >= d.objects.size())
        return "";
    return d.objects[size_t(index)].name;
}

Band bandOf(const Node& n)
{
    Band b;
    b.width = n.as<double>(n.type().attrIndex("width"));
    b.falloff = Falloff(std::clamp<int64_t>(n.as<int64_t>(n.type().attrIndex("falloff")), 0, int64_t(falloffNames().size()) - 1));
    b.curve = n.as<std::string>(n.type().attrIndex("falloffCurve"));
    b.noise = n.as<double>(n.type().attrIndex("noise"));
    b.noiseScale = n.as<double>(n.type().attrIndex("noiseScale"));
    b.seed = uint32_t(n.as<int64_t>(n.type().attrIndex("seed")));
    return b;
}

std::shared_ptr<const Heightfield> heightfieldFromMesh(const Mesh& m, std::string* why)
{
    auto fail = [why](std::string s) -> std::shared_ptr<const Heightfield> {
        if (why)
            *why = std::move(s);
        return nullptr;
    };
    const size_t n = m.numPoints();
    if (n < 4 || m.pointCapacity() != n)
        return fail("too few points");
    const auto pos = m.positions();
    const double tol = 1e-4;
    size_t cx = 1;
    while (cx < n && std::abs(pos[cx].z - pos[0].z) < tol)
        ++cx;
    if (cx < 2 || n % cx != 0)
        return fail("not a regular grid");
    const size_t cz = n / cx;
    if (cz < 2)
        return fail("not a regular grid");
    const double dx = pos[1].x - pos[0].x, dz = pos[cx].z - pos[0].z;
    if (dx <= tol || std::abs(dx - dz) > tol * std::max(1.0, dx))
        return fail("cells are not square");
    for (size_t j = 0; j < cz; ++j)
        for (size_t i = 0; i < cx; ++i)
        {
            const glm::dvec3& p = pos[j * cx + i];
            if (std::abs(p.x - (pos[0].x + double(i) * dx)) > tol * std::max(1.0, dx) * 10.0 ||
                std::abs(p.z - (pos[0].z + double(j) * dz)) > tol * std::max(1.0, dx) * 10.0)
                return fail("samples are not evenly spaced (a stepped preview of a large terrain)");
        }
    auto h = std::make_shared<Heightfield>();
    h->nx = int(cx);
    h->nz = int(cz);
    h->cell = dx;
    h->origin = {pos[0].x, pos[0].z};
    h->heights.resize(n);
    for (size_t k = 0; k < n; ++k)
        h->heights[k] = float(pos[k].y);
    const auto& attrs = m.pointAttrs();
    for (size_t l = 0; l < attrs.layerCount(); ++l)
    {
        const geo::Attribute& a = attrs.layer(l);
        if (a.type() != geo::AttrType::Float || a.name().rfind("__", 0) == 0)
            continue;
        const auto v = a.values<float>();
        if (v.size() == n)
            h->masks[a.name()] = std::vector<float>(v.begin(), v.end());
    }
    return h;
}

std::shared_ptr<const MeshBvh> bvhOf(const std::shared_ptr<const Mesh>& mesh)
{
    if (!mesh)
        return nullptr;
    {
        std::lock_guard lock(gBvhMutex);
        if (auto it = gBvhCache.find(mesh.get()); it != gBvhCache.end())
        {
            if (auto alive = it->second.mesh.lock(); alive && alive == mesh)
                return it->second.bvh;
            gBvhCache.erase(it);
        }
    }
    auto bvh = std::make_shared<MeshBvh>(*mesh);
    std::lock_guard lock(gBvhMutex);
    if (gBvhCache.size() > 4096)
        std::erase_if(gBvhCache, [](const auto& e) { return e.second.mesh.expired(); });
    gBvhCache[mesh.get()] = {mesh, bvh};
    return bvh;
}

bool registerNodes(ext::Context& ctx)
{
    NodeType t;
    t.name = kSeamType;
    t.icon = "layers-intersect";
    t.doc = "Where objects meet the ground and each other: contact curves with facts at every point (angle, depth, gap, exposure, "
            "ground layer), points along them, distance fields for the meshes and a contact mask for the terrain. Live: it follows "
            "the objects as they move.";
    t.ownAttrs = seamAttrs();
    t.create = [] { return std::make_unique<SeamNode>(); };
    return types::registerType(ctx, std::move(t)) != nullptr && registerFieldNodes(ctx);
}
} // namespace seamwork
