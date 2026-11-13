// Live fields: the field modifier writing distance-to-join into a mesh's vertex colour or UV channel, the terrain's Contact
// Mask node, and the idle-time update that keeps both in step with their seam node.

#include "Seamwork.h"

#include <lila/Relocate.h>
#include <lila/Command.h>
#include <lila/Deformer.h>
#include <lila/Events.h>
#include <lila/Extension.h>
#include <lila/GeometryNodes.h>
#include <lila/Log.h>
#include <lila/Mesh.h>
#include <lila/Profile.h>
#include <lila/Tasks.h>
#include <lila/Undo.h>

#include <algorithm>
#include <cmath>
#include <map>
#include <sstream>

using namespace lila;
using namespace lila::scene;

namespace seamwork
{
namespace
{
/** A mesh's distances to its joins, set on its field modifier by applySeam. Kind "seamworkField". */
struct FieldData final : Data
{
    std::vector<float> distance;
    glm::dmat4 world{1.0};
    Band band;
    double range = 200.0;

    const char* kind() const override { return "seamworkField"; }
    json toJson() const override { return {{"points", distance.size()}, {"range", range}}; }
    size_t bytes() const override { return sizeof(*this) + distance.size() * sizeof(float); }
};

const FieldData* fieldOf(const Node& n)
{
    const DataPtr* d = std::get_if<DataPtr>(&n.value(n.type().attrIndex("field")));
    if (!d || !*d || std::string((*d)->kind()) != "seamworkField")
        return nullptr;
    return static_cast<const FieldData*>(d->get());
}

const std::vector<std::string> kTargets = {"color", "uv"};
const std::vector<std::string> kChannels = {"r", "g", "b", "a"};

geo::Mesh applyField(const geo::Mesh& in, const Node& n)
{
    geo::Mesh m = in;
    const FieldData* f = fieldOf(n);
    if (!f || f->distance.empty())
        return m;
    if (f->distance.size() != m.pointCapacity())
        throw std::runtime_error("the field is out of date (the mesh's points changed); move the mesh or run seamUpdate");
    const auto get = [&](const char* name) { return n.value(n.type().attrIndex(name)); };
    Band b = f->band;
    if (const double w = std::get<double>(get("width")); w >= 0.0)
        b.width = w;
    if (const double nz = std::get<double>(get("noise")); nz >= 0.0)
        b.noise = nz;
    const double strength = std::get<double>(get("strength"));
    const int target = int(std::get<int64_t>(get("target")));
    const int channel = int(std::clamp<int64_t>(std::get<int64_t>(get("channel")), 0, 3));
    std::string set = std::get<std::string>(get("colorSet"));
    const std::vector<std::string> sets = geo::colors::sets(m);
    if (set.empty())
        set = sets.empty() ? std::string("Contact") : sets.front();
    const bool override = std::get<bool>(get("paintOverride")) && !(target == 0 && channel == 3) && geo::colors::has(m, set);

    std::vector<float> w(m.pointCapacity(), 0.0f);
    for (geo::Index p = 0; p < m.pointCapacity(); ++p)
        if (m.pointValid(p))
        {
            const glm::dvec3 world(f->world * glm::dvec4(m.position(p), 1.0));
            const double d = std::min<double>(f->distance[p], f->range);
            w[p] = float(std::clamp(b.weight(d, world) * strength, 0.0, 1.0));
        }
    std::vector<float> alpha;
    if (override)
    {
        const auto v = geo::colors::values(m, set);
        alpha.resize(v.size());
        for (size_t c = 0; c < v.size(); ++c)
            alpha[c] = v[c].a;
    }
    if (target == 0)
    {
        auto v = geo::colors::add(m, set, glm::vec4(1.0f));
        for (geo::Index c = 0; c < m.cornerCapacity(); ++c)
            if (m.cornerValid(c))
                v[c][channel] = w[m.cornerPoint(c)] * (override ? alpha[c] : 1.0f);
    }
    else
    {
        const int64_t uvSet = std::get<int64_t>(get("uvSet"));
        auto v = m.cornerAttrs().getOrAdd<glm::vec2>("uv" + std::to_string(uvSet));
        for (geo::Index c = 0; c < m.cornerCapacity(); ++c)
            if (m.cornerValid(c))
            {
                const geo::Index p = m.cornerPoint(c);
                v[c] = glm::vec2(w[p] * (override ? alpha[c] : 1.0f), float(std::min<double>(f->distance[p], f->range) / f->range));
            }
    }
    return m;
}

AttrDef fparam(std::string name, PlugType type, PlugValue def, std::string doc, std::optional<double> lo = {}, std::optional<double> hi = {})
{
    return {.name = std::move(name), .type = type, .defaultValue = std::move(def), .flags = AttrInput | AttrStorable | AttrKeyable,
            .doc = std::move(doc), .minValue = lo, .maxValue = hi};
}

bool registerFieldModifier(ext::Context& ctx)
{
    NodeType t;
    t.name = kFieldModifier;
    t.icon = "gradienter";
    t.doc = "Writes how close each vertex is to where the mesh meets the ground or another object into a vertex colour or UV "
            "channel, for contact materials (MF_LilaContactMask). Its seam node keeps it up to date as things move.";
    AttrDef target = fparam("target", PlugType::Enum, int64_t(0), "color writes a vertex colour channel; uv writes a UV set (U weight, V distance).");
    target.enumNames = kTargets;
    AttrDef channel = fparam("channel", PlugType::Enum, int64_t(0), "Vertex colour channel the weight goes in.");
    channel.enumNames = kChannels;
    t.ownAttrs = {
        {.name = "inMesh", .type = PlugType::Mesh, .defaultValue = MeshPtr(), .flags = AttrInput | AttrHidden},
        fparam("enabled", PlugType::Bool, true, "Turn the modifier off without removing it."),
        {.name = "exportEnabled", .type = PlugType::Bool, .defaultValue = true, .flags = AttrInput | AttrStorable, .doc = "Apply the modifier when exporting."},
        {.name = "field", .type = PlugType::Data, .defaultValue = DataPtr(), .flags = AttrInput | AttrHidden, .doc = "Distances from the seam node."},
        target,
        channel,
        {.name = "colorSet", .type = PlugType::String, .defaultValue = std::string(), .flags = AttrInput | AttrStorable,
         .doc = "Colour set to write; the first set (or a new Contact set) when empty."},
        fparam("uvSet", PlugType::Int, int64_t(2), "UV set to write, uv<n>.", 1.0, 7.0),
        fparam("width", PlugType::Float, -1.0, "Band width for this mesh, cm; below 0 takes the seam node's.", -1.0),
        fparam("strength", PlugType::Float, 1.0, "Scales the weight.", 0.0, 1.0),
        fparam("noise", PlugType::Float, -1.0, "Width noise for this mesh; below 0 takes the seam node's.", -1.0, 4.0),
        fparam("paintOverride", PlugType::Bool, true, "Multiply by the colour set's painted alpha, so painting alpha down hides the blend there."),
        {.name = "outMesh", .type = PlugType::Mesh, .defaultValue = MeshPtr(), .flags = AttrOutput | AttrHidden},
    };
    const int out = int(t.ownAttrs.size()) - 1;
    t.create = [out] {
        auto n = std::make_unique<ModifierNode>();
        n->apply = applyField;
        n->outAttr = out;
        return n;
    };
    if (!types::registerType(ctx, std::move(t)))
        return false;
    // Its weights come from the seam's fields, read against the mesh as it now is.
    lila::scene::relocate::registerNeutral(kFieldModifier);
    return true;
}

using gn::ExecContext;
using gn::SocketDecl;
using Socket = gn::SocketType;

SocketDecl gin(std::string id, Socket t, json def, std::string doc, std::optional<double> lo = {})
{
    SocketDecl s{.id = std::move(id), .type = t, .defaultValue = std::move(def), .doc = std::move(doc)};
    s.min = lo;
    return s;
}

/** "[[[x, y, z], ...], ...]" polylines. */
std::vector<std::pair<glm::dvec3, glm::dvec3>> parseSegments(const std::string& text)
{
    std::vector<std::pair<glm::dvec3, glm::dvec3>> segs;
    const json j = json::parse(text, nullptr, false);
    if (!j.is_array())
        return segs;
    for (const json& line : j)
    {
        if (!line.is_array())
            continue;
        std::vector<glm::dvec3> pts;
        for (const json& p : line)
            if (p.is_array() && p.size() == 3)
                pts.push_back({p[0].get<double>(), p[1].get<double>(), p[2].get<double>()});
        for (size_t k = 0; k + 1 < pts.size(); ++k)
            segs.emplace_back(pts[k], pts[k + 1]);
        if (pts.size() == 1)
            segs.emplace_back(pts[0], pts[0]);
    }
    return segs;
}

gn::NodeDecl contactMaskDecl()
{
    return {
        .type = kContactMaskNode,
        .label = "Contact Mask",
        .category = "Terrain",
        .doc = "A mask on the heightfield that is 1 where objects meet the ground and fades over width: dirt rings around buildings and "
               "rocks for terrain layer rules. Its seam node fills in curves and keeps them up to date.",
        .props = {{.id = "falloff", .options = falloffNames(), .defaultValue = "smooth", .doc = "How the mask fades."}},
        .sockets = {.inputs = {{.id = "heightfield", .type = Socket::Geometry},
                               gin("curves", Socket::String, "[]", "Contact polylines in the heightfield's space, [[[x, y, z], ...], ...]."),
                               gin("width", Socket::Float, 100.0, "How far the mask reaches from a contact, cm.", 0.0),
                               gin("noise", Socket::Float, 0.0, "Width varies by up to this fraction with world noise.", 0.0),
                               gin("noiseScale", Socket::Float, 500.0, "Size of the width noise, cm.", 1.0),
                               gin("seed", Socket::Int, 0, "Noise seed."),
                               gin("mask", Socket::String, "contact", "Name of the mask written."),
                               gin("source", Socket::String, "", "The seam node that fills in curves.")},
                    .outputs = {{.id = "heightfield", .type = Socket::Geometry}}},
        .exec =
            [](ExecContext& c) {
                LILA_ZONE();

                gn::GeometryPtr g = c.geometry("heightfield");
                if (!g->heightfield)
                    throw gn::ExecError(c.node.name + ": the geometry has no heightfield");
                Heightfield h = *g->heightfield;
                Band b;
                b.width = std::max(0.0, c.f("width"));
                b.noise = std::max(0.0, c.f("noise"));
                b.noiseScale = std::max(1.0, c.f("noiseScale"));
                b.seed = uint32_t(c.i("seed"));
                const std::string fall = c.prop("falloff");
                for (size_t k = 0; k < falloffNames().size(); ++k)
                    if (falloffNames()[k] == fall)
                        b.falloff = Falloff(k);
                const std::string name = c.s("mask").empty() ? std::string("contact") : c.s("mask");
                std::vector<float>& mask = h.mask(name);
                std::fill(mask.begin(), mask.end(), 0.0f);
                const SegmentIndex idx(parseSegments(c.s("curves")));
                if (!idx.empty())
                {
                    const double reach = b.width * (1.0 + b.noise) + 1e-6;
                    tbb::parallel_for(0, h.nz, [&](int z) {
                        for (int x = 0; x < h.nx; ++x)
                        {
                            const glm::dvec3 p = h.position(x, z);
                            const double d = idx.distance(p, reach);
                            mask[h.index(x, z)] = d >= reach ? 0.0f : float(b.weight(d, p));
                        }
                    });
                }
                gn::Geometry out = *g;
                out.heightfield = std::make_shared<const Heightfield>(std::move(h));
                c.out("heightfield", gn::makeGeometry(std::move(out)));
            },
    };
}

/** Terrain-space polylines of the joins with the terrain, compact. */
std::string terrainCurvesJson(const SeamData& d)
{
    std::ostringstream s;
    s.setf(std::ios::fixed);
    s.precision(2);
    s << '[';
    bool firstLine = true;
    const glm::dmat4 inv = glm::inverse(d.ground->world);
    for (const Curve& c : d.result->curves)
    {
        if (c.b != kTerrain)
            continue;
        s << (firstLine ? "" : ",") << '[';
        firstLine = false;
        auto put = [&](const glm::dvec3& w, bool first) {
            const glm::dvec3 p(inv * glm::dvec4(w, 1.0));
            s << (first ? "" : ",") << '[' << p.x << ',' << p.y << ',' << p.z << ']';
        };
        for (size_t k = 0; k < c.points.size(); ++k)
            put(c.points[k], k == 0);
        if (c.closed && c.points.size() > 2)
            put(c.points.front(), false);
        s << ']';
    }
    s << ']';
    return s.str();
}

std::map<NodeId, uint64_t> gApplied;
std::map<NodeId, uint64_t> gSeenPass;
} // namespace

bool registerFieldNodes(ext::Context& ctx) { return registerFieldModifier(ctx) && gn::registerDecl(ctx, contactMaskDecl()) != nullptr; }

json applySeam(Graph& g, NodeId seam, bool force)
{
    Node* n = g.node(seam);
    if (!n || n->type().name != kSeamType)
        return json::object();
    const PlugValue v = g.value({seam, n->type().attrIndex("outSeams"), -1});
    const SeamData* d = seamDataOf(v);
    json out = {{"fields", 0}, {"terrainMask", false}};
    if (!d || !d->result)
        return out;
    if (!force && gApplied.contains(seam) && gApplied[seam] == d->revision)
        return out;
    int fields = 0;
    for (size_t i = 0; i < d->objects.size(); ++i)
    {
        if (d->distances[i].empty())
            continue;
        Node* mesh = g.find(d->objects[i].name);
        if (!mesh)
            continue;
        for (NodeId mid : modifiers::stack(g, mesh->id()))
        {
            Node* m = g.node(mid);
            if (!m || m->type().name != kFieldModifier)
                continue;
            auto f = std::make_shared<FieldData>();
            f->distance = d->distances[i];
            f->world = d->objects[i].world;
            f->band = d->band;
            f->range = d->fieldRange;
            g.setValue({mid, m->type().attrIndex("field"), -1}, DataPtr(std::move(f)));
            ++fields;
        }
    }
    out["fields"] = fields;
    const std::string terrainName = n->as<std::string>(n->type().attrIndex("terrainName"));
    if (d->ground && n->as<bool>(n->type().attrIndex("terrainMask")) && !terrainName.empty())
        if (Node* terrain = g.find(terrainName))
        {
            const std::string curves = terrainCurvesJson(*d);
            for (NodeId mid : modifiers::stack(g, terrain->id()))
            {
                Node* mod = g.node(mid);
                if (!mod || !gn::isGnModifier(*mod))
                    continue;
                Node* groupNode = gn::groupOfModifier(g, *mod);
                if (!groupNode)
                    continue;
                gn::Group grp = gn::readGroup(*groupNode);
                bool changed = false;
                for (gn::GNode& gnode : grp.nodes)
                    if (gnode.type == kContactMaskNode && gnode.values.value("source", std::string()) == n->name())
                    {
                        if (gnode.values.value("curves", std::string()) != curves)
                        {
                            gnode.values["curves"] = curves;
                            changed = true;
                        }
                        out["terrainMask"] = true;
                    }
                if (changed)
                    gn::commitGroup(g, *groupNode, std::move(grp));
            }
        }
    gApplied[seam] = d->revision;
    return out;
}

void applyLive(Graph& g, bool force)
{
    for (NodeId id : g.nodes())
    {
        Node* n = g.node(id);
        if (!n || n->type().name != kSeamType || !n->as<bool>(n->type().attrIndex("live")))
            continue;
        applySeam(g, id, force);
    }
}

void registerLiveUpdates(ext::Context& ctx)
{
    ctx.subscribe(events::Event::AppIdle, [](const events::EventData&) {
        if (!tasks::isMainThread())
            return;
        Graph& g = Graph::current();
        static uint64_t structure = 0;
        static std::vector<NodeId> seams;
        if (g.structure() != structure)
        {
            structure = g.structure();
            seams.clear();
            for (NodeId id : g.nodes())
                if (Node* n = g.node(id); n && n->type().name == kSeamType)
                    seams.push_back(id);
            gApplied.clear();
            gSeenPass.clear();
        }
        for (NodeId id : seams)
        {
            Node* n = g.node(id);
            if (!n || !n->as<bool>(n->type().attrIndex("live")))
                continue;
            // Only look once the node computed again; the value pull below would otherwise evaluate every idle.
            if (gSeenPass.contains(id) && gSeenPass[id] == n->evaluatedPass())
                continue;
            gSeenPass[id] = n->evaluatedPass();
            UndoStack& undo = UndoStack::get();
            const bool was = undo.enabled();
            // Fields are derived from the scene, so following a move is not an edit of its own.
            undo.setEnabled(false);
            try
            {
                applySeam(g, id, false);
            }
            catch (const std::exception& e)
            {
                log::warn("{}: live update failed: {}", n->name(), e.what());
            }
            undo.setEnabled(was);
        }
    });
}
} // namespace seamwork
