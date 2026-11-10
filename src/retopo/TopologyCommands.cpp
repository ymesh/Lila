#include "retopo/TopologyCommands.h"

#include "ops/Command.h"
#include "ops/Undo.h"
#include "core/Log.h"
#include "core/Profile.h"
#include "core/Progress.h"
#include "retopo/CommandArgs.h"
#include "retopo/LiveRelax.h"
#include "retopo/RetopoCommands.h"
#include "scene/Components.h"
#include "scene/MeshNodes.h"
#include "scene/Modifiers.h"

#include <glm/gtc/matrix_inverse.hpp>

#include <algorithm>
#include <cmath>
#include <functional>
#include <map>

namespace lila::retopo
{
namespace
{
using scene::Graph;
using scene::MeshNode;
using scene::MeshPtr;
using scene::Node;
using scene::PlugRef;
using scene::Transform;

Node& target(const Args& a, const char* cmd) { return retopoTargetNode(a, cmd, a.get<bool>("create")); }

std::optional<geo::SymmetryPlane> symmetryPlane(const Node& n)
{
    int64_t axis = n.as<int64_t>(MeshNode::kSymmetry);
    if (axis <= 0 || axis > 3)
        return std::nullopt;
    return geo::axisPlane(int(axis - 1), scene::symmetryOffset(n, int(axis - 1)));
}

/** The reference surface in the retopo mesh's object space, with its Bvh; the last one is kept for the next call. */
struct Surface
{
    MeshPtr source;
    glm::dmat4 toLocal{1.0};
    geo::Mesh mesh;
    geo::MeshBvh bvh;
};

const Surface* surfaceFor(const Node& retopo)
{
    static Surface cache;
    scene::NodeId ref = retopoDrawReference(Graph::current(), retopo.id());
    Node* r = Graph::current().node(ref);
    if (!r)
        return nullptr;
    Graph::current().evaluate();
    MeshPtr m = r->as<MeshPtr>(MeshNode::kOutMesh);
    if (!m || m->numFaces() == 0)
        return nullptr;
    glm::dmat4 toLocal = glm::inverse(worldOf(retopo)) * worldOf(*r);
    if (cache.source != m || cache.toLocal != toLocal)
    {
        LILA_ZONE_N("retopo surface cache");
        cache.source = m;
        cache.toLocal = toLocal;
        cache.mesh = *m;
        cache.mesh.compact();
        for (glm::dvec3& p : cache.mesh.positions())
            p = glm::dvec3(toLocal * glm::dvec4(p, 1.0));
        cache.bvh.build(cache.mesh);
    }
    return &cache;
}

json vec(const glm::dvec3& p) { return json::array({p.x, p.y, p.z}); }

glm::dvec3 toVec(const json& j, const char* what)
{
    if (!j.is_array() || j.size() != 3 || !j[0].is_number() || !j[1].is_number() || !j[2].is_number())
        throw std::runtime_error(std::string(what) + " must be [x, y, z]");
    return {j[0].get<double>(), j[1].get<double>(), j[2].get<double>()};
}

std::vector<glm::dvec3> toVecs(const json& j, const char* what)
{
    std::vector<glm::dvec3> out;
    if (j.is_null())
        return out;
    if (!j.is_array())
        throw std::runtime_error(std::string(what) + " must be a list of [x, y, z]");
    for (const json& v : j)
        out.push_back(toVec(v, what));
    return out;
}

template <class T> std::vector<T> toList(const json& j)
{
    std::vector<T> out;
    if (j.is_array())
        for (const json& v : j)
            out.push_back(v.get<T>());
    return out;
}

/** Applies fn to every point (and dir to every direction, len to every length) in a spec, whatever its kind. */
void mapSpec(json& spec, ConstructKind kind, const std::function<glm::dvec3(const glm::dvec3&)>& point,
             const std::function<glm::dvec3(const glm::dvec3&)>& dir, double lengthScale)
{
    auto points = [&](json& list) {
        if (list.is_array())
            for (json& v : list)
                if (v.is_array() && v.size() == 3)
                    v = vec(point(toVec(v, "point")));
    };
    auto one = [&](json& v) {
        if (v.is_array() && v.size() == 3)
            v = vec(point(toVec(v, "point")));
    };
    auto length = [&](const char* key) {
        if (spec.contains(key) && spec[key].is_number())
            spec[key] = spec[key].get<double>() * lengthScale;
    };
    switch (kind)
    {
    case ConstructKind::Patch:
        if (spec.contains("sides"))
            for (json& side : spec["sides"])
                points(side["points"]);
        if (spec.contains("pull"))
            one(spec["pull"]);
        if (spec.contains("lift"))
            spec["lift"] = vec(dir(toVec(spec["lift"], "lift")));
        break;
    case ConstructKind::Strip:
        points(spec["knots"]);
        if (spec.contains("startRow"))
            points(spec["startRow"]);
        if (spec.contains("endRow"))
            points(spec["endRow"]);
        length("width");
        break;
    case ConstructKind::Stroke: points(spec["path"]); break;
    case ConstructKind::Bridge:
        points(spec["a"]);
        points(spec["b"]);
        break;
    case ConstructKind::Sweep:
        points(spec["border"]);
        points(spec["path"]);
        break;
    case ConstructKind::Cylinder:
        if (spec.contains("rings"))
            for (json& ring : spec["rings"])
            {
                one(ring["origin"]);
                one(ring["pick"]);
                if (ring.contains("normal"))
                    ring["normal"] = vec(dir(toVec(ring["normal"], "normal")));
            }
        break;
    case ConstructKind::Region:
        points(spec["border"]);
        if (spec.contains("guides"))
            for (json& g : spec["guides"])
                points(g);
        length("edgeLength");
        break;
    }
}

/** A spec carried from one space to another by the affine matrix m. */
json transformSpec(json spec, ConstructKind kind, const glm::dmat4& m)
{
    glm::dmat3 lin(m);
    glm::dmat3 normalMat = glm::transpose(glm::inverse(lin));
    double scale = std::cbrt(std::abs(glm::determinant(lin)));
    mapSpec(
        spec, kind, [&](const glm::dvec3& p) { return glm::dvec3(m * glm::dvec4(p, 1.0)); },
        [&](const glm::dvec3& d) {
            glm::dvec3 n = normalMat * d;
            double l = glm::length(n);
            return l > 0.0 ? n / l : d;
        },
        scale);
    return spec;
}

/** The constructs stored on a retopo mesh: {"next": id, "items": [{id, kind, spec (object space), hash}]}. */
json loadRecords(const Node& n)
{
    const std::string& text = n.as<std::string>(MeshNode::kRetopoConstructs);
    json j = text.empty() ? json::object() : json::parse(text, nullptr, false);
    if (!j.is_object())
        j = json::object();
    if (!j.contains("items") || !j["items"].is_array())
        j["items"] = json::array();
    if (!j.contains("next") || !j["next"].is_number_integer())
        j["next"] = 1;
    return j;
}

void storeRecords(Graph& g, const Node& n, const json& records)
{
    std::string text = records["items"].empty() ? std::string() : records.dump();
    if (n.as<std::string>(MeshNode::kRetopoConstructs) != text)
        g.setValue({n.id(), MeshNode::kRetopoConstructs, -1}, text);
}

json* findRecord(json& records, int id)
{
    for (json& r : records["items"])
        if (r.value("id", 0) == id)
            return &r;
    return nullptr;
}

void dropRecord(json& records, int id)
{
    json& items = records["items"];
    for (size_t i = 0; i < items.size(); ++i)
        if (items[i].value("id", 0) == id)
        {
            items.erase(items.begin() + std::ptrdiff_t(i));
            return;
        }
}

std::string hashText(uint64_t h) { return std::to_string(h); }

LiveRelaxOptions settleOptions(const Args& a, const Node& n)
{
    LiveRelaxOptions o;
    o.evenSize = std::clamp(a.get<double>("evenSize", 0.5), 0.0, 1.0);
    o.squareness = std::clamp(a.get<double>("squareness", 0.7), 0.0, 1.0);
    o.pinBoundary = a.get<bool>("pinBoundary", true);
    o.symmetry = symmetryPlane(n);
    return o;
}

const char* kKindChoicesDoc = "patch, strip, stroke, bridge, sweep, cylinder or region";
} // namespace

ConstructSpec parseConstructSpec(ConstructKind kind, const json& s)
{
    if (!s.is_object())
        throw std::runtime_error("spec must be a dict");
    switch (kind)
    {
    case ConstructKind::Patch:
    {
        PatchSpec p;
        for (const json& side : s.value("sides", json::array()))
        {
            PatchSide ps;
            ps.points = toVecs(side.value("points", json::array()), "sides[].points");
            ps.fixed = side.value("fixed", false);
            ps.count = side.value("count", 0);
            p.sides.push_back(std::move(ps));
        }
        p.padding = s.value("padding", -1);
        p.variant = s.value("variant", 0);
        if (s.contains("pull") && !s["pull"].is_null())
            p.pull = toVec(s["pull"], "pull");
        if (s.contains("lift") && !s["lift"].is_null())
            p.lift = toVec(s["lift"], "lift");
        return p;
    }
    case ConstructKind::Strip:
    {
        StripSpec p;
        p.knots = toVecs(s.value("knots", json::array()), "knots");
        p.scales = toList<double>(s.value("scales", json::array()));
        p.twists = toList<double>(s.value("twists", json::array()));
        for (const json& c : s.value("corners", json::array()))
            p.corners.push_back(c.is_boolean() ? uint8_t(c.get<bool>()) : uint8_t(c.get<int>() != 0));
        p.width = s.value("width", 1.0);
        p.divisions = s.value("divisions", 0);
        p.rows = s.value("rows", 1);
        p.closed = s.value("closed", false);
        auto row = [&](const char* key) -> std::optional<std::array<glm::dvec3, 2>> {
            if (!s.contains(key) || s[key].is_null())
                return std::nullopt;
            std::vector<glm::dvec3> pts = toVecs(s[key], key);
            if (pts.size() != 2)
                throw std::runtime_error(std::string(key) + " must be two points");
            return std::array<glm::dvec3, 2>{pts[0], pts[1]};
        };
        p.startRow = row("startRow");
        p.endRow = row("endRow");
        return p;
    }
    case ConstructKind::Stroke:
    {
        StrokeSpec p;
        p.path = toVecs(s.value("path", json::array()), "path");
        p.divisions = s.value("divisions", 8);
        return p;
    }
    case ConstructKind::Bridge:
    {
        BridgeSpec p;
        p.a = toVecs(s.value("a", json::array()), "a");
        p.b = toVecs(s.value("b", json::array()), "b");
        p.aFixed = s.value("aFixed", false);
        p.bFixed = s.value("bFixed", false);
        p.divisions = s.value("divisions", 0);
        p.rows = s.value("rows", 0);
        return p;
    }
    case ConstructKind::Sweep:
    {
        SweepSpec p;
        p.border = toVecs(s.value("border", json::array()), "border");
        p.borderClosed = s.value("borderClosed", false);
        p.anchor = s.value("anchor", 0);
        p.span = s.value("span", 1);
        p.path = toVecs(s.value("path", json::array()), "path");
        p.rows = s.value("rows", 0);
        return p;
    }
    case ConstructKind::Cylinder:
    {
        CylinderSpec p;
        for (const json& r : s.value("rings", json::array()))
            p.rings.push_back({toVec(r.at("origin"), "rings[].origin"), toVec(r.at("normal"), "rings[].normal"), toVec(r.at("pick"), "rings[].pick")});
        p.points = s.value("points", 8);
        p.rotate = s.value("rotate", 0.0);
        p.twist = s.value("twist", 0.0);
        p.rowDensity = s.value("rowDensity", 1.0);
        p.capStart = s.value("capStart", false);
        p.capEnd = s.value("capEnd", false);
        return p;
    }
    case ConstructKind::Region:
    {
        RegionSpec p;
        p.faces = toList<Index>(s.value("faces", json::array()));
        p.referenceFaces = s.value("referenceFaces", Index(0));
        p.edgeLength = s.value("edgeLength", 1.0);
        p.targetFaces = s.value("targetFaces", 0);
        for (const json& g : s.value("guides", json::array()))
            p.guides.push_back(toVecs(g, "guides"));
        p.border = toVecs(s.value("border", json::array()), "border");
        return p;
    }
    }
    throw std::runtime_error("unknown construct kind");
}

glm::vec3 guidePaletteColor(int n)
{
    static const glm::vec3 kPalette[] = {{1.0f, 0.55f, 0.1f}, {0.2f, 0.75f, 1.0f}, {0.55f, 0.95f, 0.3f}, {1.0f, 0.35f, 0.6f},
                                         {0.95f, 0.9f, 0.25f}, {0.65f, 0.5f, 1.0f}, {0.2f, 0.95f, 0.8f}, {1.0f, 0.3f, 0.25f}};
    return kPalette[((n % 8) + 8) % 8];
}

std::vector<Guide> retopoGuides(const Node& retopo)
{
    std::vector<Guide> out;
    const std::string& text = retopo.as<std::string>(MeshNode::kRetopoGuides);
    if (text.empty())
        return out;
    json list = json::parse(text, nullptr, false);
    if (!list.is_array())
        return out;
    glm::dmat4 world = worldOf(retopo);
    for (const json& g : list)
    {
        if (!g.is_object())
            continue;
        Guide guide;
        try
        {
            for (const glm::dvec3& p : toVecs(g.value("points", json::array()), "points"))
                guide.points.push_back(glm::dvec3(world * glm::dvec4(p, 1.0)));
            if (json c = g.value("color", json()); c.is_array() && c.size() == 3)
                guide.color = glm::vec3(c[0].get<float>(), c[1].get<float>(), c[2].get<float>());
        }
        catch (const std::exception&)
        {
            continue;
        }
        guide.closed = g.value("closed", false);
        out.push_back(std::move(guide));
    }
    return out;
}

std::vector<ConstructRecord> retopoConstructs(const Graph& g, const Node& retopo)
{
    std::vector<ConstructRecord> out;
    json records = loadRecords(retopo);
    MeshPtr base;
    if (auto v = g.storedValue(scene::modifiers::editPlug(const_cast<Graph&>(g), retopo.id())); std::holds_alternative<MeshPtr>(v))
        base = std::get<MeshPtr>(v);
    glm::dmat4 world = worldOf(retopo);
    for (const json& r : records["items"])
    {
        ConstructRecord rec;
        rec.id = r.value("id", 0);
        if (!constructKindFromName(r.value("kind", std::string()), rec.kind))
            continue;
        rec.spec = transformSpec(r.value("spec", json::object()), rec.kind, world);
        if (base)
            rec.live = hashText(constructHash(*base, rec.id)) == r.value("hash", std::string());
        rec.counts = toList<int>(r.value("counts", json::array()));
        out.push_back(std::move(rec));
    }
    return out;
}

int constructOfFace(const geo::Mesh& mesh, Index face)
{
    std::span<const int32_t> tags = mesh.faceAttrs().get<int32_t>(kConstructAttr);
    return face < tags.size() ? tags[face] : 0;
}

void registerTopologyCommands()
{
    using T = ArgType;
    const FlagDef meshFlag = nodeFlag({"mesh", "", T::String, nullptr, "Retopo mesh."}, "mesh");
    const FlagDef referenceFlag = nodeFlag({"reference", "", T::String, nullptr, "Reference mesh, connected so the topology tools resume on this pair later."}, "mesh");
    const FlagDef createFlag{"create", "", T::Bool, false, "Create the mesh if it doesn't exist (taking the reference's symmetry)."};

    ops::registerCommand({
        .name = "retopoSetGuides",
        .doc = "Topology: replace the retopo mesh's guides, the planning lines the Pencil draws. Guides never export; quad "
               "remeshing a region follows the ones inside it. Returns the mesh name.",
        .flags = {meshFlag,
                  {"guides", "", T::List, json::array(), "Each a dict: points (world [x, y, z] list), color ([r, g, b] sRGB 0-1), closed."},
                  referenceFlag,
                  createFlag},
        .run = [](const Args& a) -> json {
            Graph& g = Graph::current();
            Node& n = target(a, "retopoSetGuides");
            glm::dmat4 toLocal = glm::inverse(worldOf(n));
            json out = json::array();
            for (const json& guide : a.flags["guides"])
            {
                json local = json::array();
                try
                {
                    for (const glm::dvec3& p : toVecs(guide.value("points", json::array()), "points"))
                        local.push_back(vec(glm::dvec3(toLocal * glm::dvec4(p, 1.0))));
                }
                catch (const std::exception& e)
                {
                    throw CommandError(std::string("retopoSetGuides: ") + e.what());
                }
                if (local.size() < 2)
                    continue;
                json item = {{"points", local}, {"closed", guide.value("closed", false)}};
                if (guide.contains("color"))
                    item["color"] = guide["color"];
                out.push_back(item);
            }
            std::string text = out.empty() ? std::string() : out.dump();
            if (n.as<std::string>(MeshNode::kRetopoGuides) != text)
                g.setValue({n.id(), MeshNode::kRetopoGuides, -1}, text);
            return n.path();
        },
    });

    ops::registerCommand({
        .name = "retopoConstruct",
        .doc = "Topology: make a live construct (patch, strip, stroke, bridge, sweep, cylinder or auto region) on the retopo "
               "mesh, or change one by id: its faces are regenerated from the new spec and settled on the reference. A "
               "construct whose faces were edited by hand since is frozen instead (left as plain geometry). Returns "
               "{mesh, id, faces, variants, sideCounts, frozen}.",
        .flags = {meshFlag,
                  {"kind", "", T::String, "patch", std::string("What to make: ") + kKindChoicesDoc + ".",
                   {"patch", "strip", "stroke", "bridge", "sweep", "cylinder", "region"}},
                  {"spec", "", T::Any, json::object(), "The construct's inputs and settings, world space; see the Topology docs per kind."},
                  {"id", "", T::Int, 0, "The construct to change; 0 makes a new one.", {}, {}, 0.0},
                  {"edgeLength", "", T::Float, 0.0, "Edge length for counts left on auto, cm; 0 takes a fortieth of the reference's size.", {}, {}, 0.0},
                  {"settle", "", T::Int, 40, "Live relax steps on the new points; 0 leaves them as generated.", {}, {}, 0.0, 1000.0},
                  {"weld", "", T::Float, 0.001, "Reuse existing points closer than this, cm.", {}, {}, 0.0},
                  {"timeout", "", T::Float, 10.0, "Seconds an auto region may take; 0 is unlimited.", {}, {}, 0.0},
                  referenceFlag,
                  createFlag},
        .run = [](const Args& a) -> json {
            LILA_ZONE_N("retopoConstruct");
            ConstructKind kind;
            if (!constructKindFromName(a.get<std::string>("kind", "patch"), kind))
                throw CommandError(std::string("retopoConstruct: kind must be ") + kKindChoicesDoc);
            Graph& g = Graph::current();
            Node& n = target(a, "retopoConstruct");
            const Surface* surface = surfaceFor(n);
            if (!surface && kind != ConstructKind::Stroke)
                throw CommandError("retopoConstruct: " + n.path() + " has no reference surface; give reference=");
            PlugRef base = scene::modifiers::editPlug(g, n.id());
            geo::Mesh m = baseMesh(base);
            json records = loadRecords(n);
            int id = a.get<int>("id");
            json* existing = id > 0 ? findRecord(records, id) : nullptr;
            if (id > 0 && !existing)
                throw CommandError("retopoConstruct: no construct " + std::to_string(id) + " on " + n.path());
            if (existing)
            {
                ConstructKind was;
                constructKindFromName(existing->value("kind", std::string()), was);
                if (was != kind)
                    throw CommandError("retopoConstruct: construct " + std::to_string(id) + " is a " + constructKindName(was));
                if (existing->value("hash", std::string()) != hashText(constructHash(m, id)))
                {
                    untagConstruct(m, id);
                    dropRecord(records, id);
                    g.setValue(base, MeshPtr(std::make_shared<geo::Mesh>(std::move(m))));
                    storeRecords(g, n, records);
                    log::warn("retopoConstruct: construct {} was edited by hand, so it is frozen as plain faces", id);
                    return {{"mesh", n.path()}, {"id", id}, {"frozen", true}, {"faces", 0}};
                }
                removeConstructFaces(m, id);
            }
            else
            {
                id = records["next"].get<int>();
                records["next"] = id + 1;
            }

            glm::dmat4 toLocal = glm::inverse(worldOf(n));
            json localSpec = transformSpec(a.flags["spec"], kind, toLocal);
            ConstructSpec spec;
            try
            {
                spec = parseConstructSpec(kind, localSpec);
            }
            catch (const std::exception& e)
            {
                throw CommandError(std::string("retopoConstruct: ") + e.what());
            }
            GenContext ctx;
            if (surface)
            {
                ctx.surfaceMesh = &surface->mesh;
                ctx.surface = &surface->bvh;
                geo::Bounds b = surface->mesh.bounds();
                ctx.edgeLength = glm::length(b.max - b.min) / 40.0;
            }
            if (double e = a.get<double>("edgeLength"); e > 0.0)
                ctx.edgeLength = e * std::cbrt(std::abs(glm::determinant(glm::dmat3(toLocal))));
            ctx.timeout = std::max(0.0, a.get<double>("timeout"));
            GenResult gen;
            if (kind == ConstructKind::Region)
            {
                progress::Scope scope("Auto retopo region", 1.0, progress::Cancellable);
                progress::runOnWorker([&] { gen = generate(spec, ctx); });
            }
            else
                gen = generate(spec, ctx);
            if (!gen.ok())
                throw CommandError(std::string("retopoConstruct ") + constructKindName(kind) + ": " + gen.error);

            double weld = std::max(0.0, a.get<double>("weld")) * std::cbrt(std::abs(glm::determinant(glm::dmat3(toLocal))));
            std::optional<geo::SymmetryPlane> sym = symmetryPlane(n);
            // Points within about a third of an edge of the symmetry plane or of an open border join it, so pieces meet.
            StitchResult st = stitchConstruct(m, gen, id, weld, sym, ctx.edgeLength * 0.35);
            if (!gen.faces.empty() && st.faces.empty())
                throw CommandError(std::string("retopoConstruct ") + constructKindName(kind) +
                                   ": every face would overlap existing faces or make a non-manifold edge");
            if (int steps = std::clamp(a.get<int>("settle"), 0, 1000); steps > 0 && surface && !st.movable.empty())
            {
                std::vector<uint8_t> active(m.pointCapacity(), 0);
                for (Index p : st.movable)
                    active[p] = 1;
                LiveRelaxOptions o = settleOptions(a, n);
                o.targetLength = ctx.edgeLength;
                o.evenSize = 0.0;
                settleMesh(m, &surface->bvh, o, steps, active);
            }
            m.removeAllLoose();
            m.compact();
            if (kind == ConstructKind::Patch && localSpec.contains("sides"))
                for (size_t i = 0; i < gen.sideCounts.size() && i < localSpec["sides"].size(); ++i)
                    if (!localSpec["sides"][i].value("fixed", false))
                        localSpec["sides"][i]["count"] = gen.sideCounts[i];
            uint64_t hash = constructHash(m, id);
            json rec = {{"id", id}, {"kind", constructKindName(kind)}, {"spec", localSpec}, {"hash", hashText(hash)}, {"counts", gen.sideCounts}};
            if (json* r = findRecord(records, id))
                *r = rec;
            else
                records["items"].push_back(rec);
            size_t faces = constructFaces(m, id).size();
            g.setValue(base, MeshPtr(std::make_shared<geo::Mesh>(std::move(m))));
            storeRecords(g, n, records);
            scene::components::clear();
            json out = {{"mesh", n.path()}, {"id", id}, {"faces", faces}, {"frozen", false}, {"variants", gen.variants}};
            if (!gen.sideCounts.empty())
                out["sideCounts"] = gen.sideCounts;
            return out;
        },
    });

    ops::registerCommand({
        .name = "retopoConstructDelete",
        .doc = "Topology: delete constructs by id, with the faces they made (points other faces share stay).",
        .flags = {meshFlag, {"ids", "", T::List, json::array(), "Construct ids."}},
        .run = [](const Args& a) -> json {
            Graph& g = Graph::current();
            Node& n = meshArg(a.get<std::string>("mesh"), "retopoConstructDelete");
            PlugRef base = scene::modifiers::editPlug(g, n.id());
            geo::Mesh m = baseMesh(base);
            json records = loadRecords(n);
            for (int id : toList<int>(a.flags["ids"]))
            {
                removeConstructFaces(m, id);
                dropRecord(records, id);
            }
            m.compact();
            g.setValue(base, MeshPtr(std::make_shared<geo::Mesh>(std::move(m))));
            storeRecords(g, n, records);
            scene::components::clear();
            return nullptr;
        },
    });

    ops::registerCommand({
        .name = "retopoConstructFreeze",
        .doc = "Topology: freeze constructs, leaving their faces as plain geometry that no setting regenerates. No ids "
               "freezes them all.",
        .flags = {meshFlag, {"ids", "", T::List, json::array(), "Construct ids; empty for all."}},
        .run = [](const Args& a) -> json {
            Graph& g = Graph::current();
            Node& n = meshArg(a.get<std::string>("mesh"), "retopoConstructFreeze");
            PlugRef base = scene::modifiers::editPlug(g, n.id());
            geo::Mesh m = baseMesh(base);
            json records = loadRecords(n);
            std::vector<int> ids = toList<int>(a.flags["ids"]);
            if (ids.empty())
                for (const json& r : records["items"])
                    ids.push_back(r.value("id", 0));
            for (int id : ids)
            {
                untagConstruct(m, id);
                dropRecord(records, id);
            }
            g.setValue(base, MeshPtr(std::make_shared<geo::Mesh>(std::move(m))));
            storeRecords(g, n, records);
            return int(ids.size());
        },
    });

    ops::registerCommand({
        .name = "retopoSettle",
        .doc = "Topology: live relax the retopo mesh to rest on its reference: faces even out toward squares of a common "
               "size, points stay on the surface, borders stay put, symmetry is kept exactly. points= limits it to those "
               "base mesh point ids.",
        .flags = {meshFlag,
                  {"points", "", T::List, nullptr, "Point ids to move; all when not given."},
                  {"steps", "", T::Int, 100, "Relax steps, 1 to 5000.", {}, {}, 1.0, 5000.0},
                  {"evenSize", "", T::Float, 0.5, "0 keeps each face's size, 1 pulls every face to the same size.", {}, {}, 0.0, 1.0},
                  {"squareness", "", T::Float, 0.7, "0 only evens spacing, 1 pulls faces fully square.", {}, {}, 0.0, 1.0},
                  {"pinBoundary", "", T::Bool, true, "Keep border points fixed; off lets them slide along the border."}},
        .run = [](const Args& a) -> json {
            Graph& g = Graph::current();
            Node& n = meshArg(a.get<std::string>("mesh"), "retopoSettle");
            const Surface* surface = surfaceFor(n);
            PlugRef base = scene::modifiers::editPlug(g, n.id());
            geo::Mesh m = baseMesh(base);
            std::vector<uint8_t> active;
            if (a.has("points"))
            {
                active.assign(m.pointCapacity(), 0);
                for (Index p : toList<Index>(a.flags["points"]))
                    if (p < active.size())
                        active[p] = 1;
            }
            settleMesh(m, surface ? &surface->bvh : nullptr, settleOptions(a, n), std::clamp(a.get<int>("steps"), 1, 5000), active);
            g.setValue(base, MeshPtr(std::make_shared<geo::Mesh>(std::move(m))));
            return nullptr;
        },
    });
    ops::setAdjustable("retopoSettle");
}
} // namespace lila::retopo
