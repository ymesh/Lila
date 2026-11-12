#include "gn/Gn.h"
#include "gn/Volume.h"

#include "ops/Command.h"
#include "geo/Mesh.h"
#include "scene/Graph.h"
#include "scene/MeshNodes.h"
#include "scene/Modifiers.h"

#include <cmath>

namespace lila::gn
{
namespace
{
using scene::Graph;
using scene::Node;

/** The gn modifier a target names: the modifier itself, or the top one on a mesh's stack. */
Node* modifierFor(Graph& g, Node& target)
{
    if (isGnModifier(target))
        return &target;
    std::vector<scene::NodeId> stack = scene::modifiers::stack(g, target.id());
    for (auto it = stack.rbegin(); it != stack.rend(); ++it)
        if (Node* m = g.node(*it); m && isGnModifier(*m))
            return m;
    return nullptr;
}

json number(double v) { return std::isfinite(v) ? json(v) : json(nullptr); }

json attrValue(const geo::Attribute& a, size_t i)
{
    switch (a.type())
    {
    case geo::AttrType::Float: return number(a.values<float>()[i]);
    case geo::AttrType::Float2:
    {
        glm::vec2 v = a.values<glm::vec2>()[i];
        return {number(v.x), number(v.y)};
    }
    case geo::AttrType::Float3:
    {
        glm::vec3 v = a.values<glm::vec3>()[i];
        return {number(v.x), number(v.y), number(v.z)};
    }
    case geo::AttrType::Float4:
    {
        glm::vec4 v = a.values<glm::vec4>()[i];
        return {number(v.x), number(v.y), number(v.z), number(v.w)};
    }
    case geo::AttrType::Int: return a.values<int32_t>()[i];
    case geo::AttrType::Bool: return a.values<bool>()[i];
    default: return nullptr;
    }
}

struct Table
{
    size_t rows = 0, start = 0, end = 0;
    json columns = json::array();

    void column(const std::string& name, const char* type, const std::function<json(size_t)>& at)
    {
        json values = json::array();
        for (size_t i = start; i < end; ++i)
            values.push_back(at(i));
        columns.push_back({{"name", name}, {"type", type}, {"values", std::move(values)}});
    }

    void attributes(const geo::AttributeSet& set)
    {
        for (size_t l = 0; l < set.layerCount(); ++l)
        {
            const geo::Attribute& a = set.layer(l);
            if (a.name().starts_with("__") || a.size() < end)
                continue;
            column(a.name(), geo::attrTypeName(a.type()), [&](size_t i) { return attrValue(a, i); });
        }
    }
};

json vec(const glm::dvec3& v) { return {number(v.x), number(v.y), number(v.z)}; }

/** The rows of one component on one domain, start to start + count. */
Table tableOf(const Geometry& g, const std::string& component, const std::string& domain, size_t start, size_t count)
{
    Table t;
    auto window = [&](size_t rows) {
        t.rows = rows;
        t.start = std::min(start, rows);
        t.end = std::min(rows, t.start + count);
    };
    if ((component == "mesh" && g.mesh) || (component == "points" && g.points))
    {
        const geo::Mesh& m = component == "mesh" ? *g.mesh : *g.points;
        geo::Domain d = domain == "edge" ? geo::Domain::Edge : domain == "face" ? geo::Domain::Face : domain == "corner" ? geo::Domain::Corner : geo::Domain::Point;
        if (component == "points")
            d = geo::Domain::Point;
        switch (d)
        {
        case geo::Domain::Point:
            window(m.pointCapacity());
            t.column("position", "Float3", [&](size_t i) { return vec(m.position(geo::Index(i))); });
            break;
        case geo::Domain::Edge:
            window(m.edgeCapacity());
            t.column("vertices", "Int2", [&](size_t i) { return json{m.edgePoint(geo::Index(i), 0), m.edgePoint(geo::Index(i), 1)}; });
            break;
        case geo::Domain::Face:
            window(m.faceCapacity());
            t.column("size", "Int", [&](size_t i) { return m.faceSize(geo::Index(i)); });
            break;
        default:
            window(m.cornerCapacity());
            t.column("vertex", "Int", [&](size_t i) { return m.cornerPoint(geo::Index(i)); });
            break;
        }
        t.attributes(m.attributes(d));
        return t;
    }
    if (component == "curves" && g.curves)
    {
        const CurveSet& c = *g.curves;
        if (domain == "curve")
        {
            window(c.numCurves());
            t.column("points", "Int", [&](size_t i) { return c.curveSize(i); });
            t.column("cyclic", "Bool", [&](size_t i) { return c.cyclic[i] != 0; });
            t.attributes(c.curveAttrs);
        }
        else
        {
            window(c.numPoints());
            t.column("position", "Float3", [&](size_t i) { return vec(c.positions[i]); });
            t.attributes(c.pointAttrs);
        }
        return t;
    }
    if (component == "instances" && g.instances)
    {
        const Instances& in = *g.instances;
        window(in.size());
        t.column("reference", "Int", [&](size_t i) { return in.reference[i]; });
        t.column("position", "Float3", [&](size_t i) { return vec(glm::dvec3(in.transforms[i][3])); });
        t.column("scale", "Float3", [&](size_t i) {
            const glm::dmat4& m = in.transforms[i];
            return vec({glm::length(glm::dvec3(m[0])), glm::length(glm::dvec3(m[1])), glm::length(glm::dvec3(m[2]))});
        });
        t.attributes(in.attrs);
        return t;
    }
    if (component == "volume" && g.volume)
    {
        const Volume& v = *g.volume;
        window(1);
        if (t.end == 0)
            return t;
        double lo = v.values.empty() ? 0.0 : double(v.values[0]), hi = lo, sum = 0.0;
        size_t active = 0;
        for (float x : v.values)
        {
            lo = std::min(lo, double(x));
            hi = std::max(hi, double(x));
            sum += x;
            active += x != v.background;
        }
        t.column("name", "String", [&](size_t) { return v.name; });
        t.column("kind", "String", [&](size_t) { return v.isSdf() ? "signed distance" : v.isVector() ? "vector" : "density"; });
        t.column("resolution", "Int3", [&](size_t) { return json{v.dims.x, v.dims.y, v.dims.z}; });
        t.column("voxelSize", "Float", [&](size_t) { return number(v.voxelSize()); });
        t.column("min", "Float", [&](size_t) { return number(lo); });
        t.column("max", "Float", [&](size_t) { return number(hi); });
        t.column("mean", "Float", [&](size_t) { return number(v.values.empty() ? 0.0 : sum / double(v.values.size())); });
        t.column("active", "Int", [&](size_t) { return active; });
        t.column("background", "Float", [&](size_t) { return number(v.background); });
        return t;
    }
    window(0);
    return t;
}

json componentCounts(const Geometry& g)
{
    json c = json::object();
    if (g.mesh)
        c["mesh"] = {{"point", g.mesh->pointCapacity()}, {"edge", g.mesh->edgeCapacity()}, {"face", g.mesh->faceCapacity()}, {"corner", g.mesh->cornerCapacity()}};
    if (g.points)
        c["points"] = {{"point", g.points->pointCapacity()}};
    if (g.curves)
        c["curves"] = {{"point", g.curves->numPoints()}, {"curve", g.curves->numCurves()}};
    if (g.instances)
        c["instances"] = {{"instance", g.instances->size()}};
    if (g.volume)
        c["volume"] = {{"grid", 1}};
    return c;
}
} // namespace

void registerSpreadsheetCommand()
{
    using Arg = ArgType;
    ops::registerCommand({
        .name = "gnSpreadsheet",
        .doc = "The evaluated geometry of a mesh's top geometry-nodes modifier (or the modifier named) as a table: one row per element of "
               "a component's domain, a column per attribute. Without a geometry-nodes modifier, the mesh as it ends up. Also lists every "
               "component with its element counts.",
        .positionalDoc = "target",
        .flags = {{"component", "", Arg::String, "mesh", "mesh, points, curves, instances or volume."},
                  {"domain", "", Arg::String, "point", "point, edge, face or corner for meshes; point or curve for curves."},
                  {"start", "", Arg::Int, 0, "First row."},
                  {"count", "", Arg::Int, 1000, "Most rows to return."}},
        .undoable = false,
        .run = [](const Args& a) -> json {
            Graph& g = Graph::current();
            if (a.positional.empty() || !a.positional[0].is_string())
                throw CommandError("gnSpreadsheet: target required");
            Node* target = g.find(a.positional[0].get<std::string>());
            if (!target)
                throw CommandError("gnSpreadsheet: no node '" + a.positional[0].get<std::string>() + "'");
            g.evaluate();
            Node* mod = modifierFor(g, *target);
            GeometryPtr geometry = mod ? modifierGeometry(mod->id()) : nullptr;
            if (!geometry)
            {
                scene::MeshPtr mesh = target->type().isA("mesh") ? target->as<scene::MeshPtr>(scene::MeshNode::kOutMesh) : nullptr;
                if (!mesh)
                    throw CommandError("gnSpreadsheet: " + target->name() + " has no geometry to show");
                Geometry only;
                only.mesh = mesh;
                geometry = makeGeometry(std::move(only));
            }
            std::string component = a.get<std::string>("component", "mesh"), domain = a.get<std::string>("domain", "point");
            Table t = tableOf(*geometry, component, domain, size_t(std::max<int64_t>(0, a.get<int64_t>("start", 0))),
                              size_t(std::clamp<int64_t>(a.get<int64_t>("count", 1000), 0, 1000000)));
            return {{"source", mod ? mod->path() : target->path()}, {"components", componentCounts(*geometry)}, {"component", component},
                    {"domain", domain}, {"rows", t.rows}, {"start", t.start}, {"columns", t.columns}};
        },
    });
}
} // namespace lila::gn
