// The arrangement geometry nodes: Row Along Line, Stack Items, Pile Items, Scatter Props, and Settle Instances, the
// physics step every arrangement can end in. Each places copies of the geometries linked into its items socket as
// instances, with item, pose and random attributes for variation downstream.

#include "Arrange.h"
#include "NodeSockets.h"
#include "Stagehand.h"

#include <lila/Extension.h>
#include <lila/GeometryNodes.h>
#include <lila/Profile.h>

#include <cstdlib>
#include <map>

using namespace lila;

namespace stagehand
{
namespace
{
using gn::ExecContext;
using gn::Geometry;
using gn::GeometryPtr;

/** The items linked in, each realized to one mesh, with its bounds. */
struct Items
{
    std::vector<GeometryPtr> refs;
    std::vector<arrange::Item> bounds;
    std::vector<std::shared_ptr<const geo::Mesh>> meshes;
};

Items itemsOf(ExecContext& c)
{
    Items out;
    for (const GeometryPtr& g : c.geometries("items"))
    {
        if (!g)
            continue;
        geo::Mesh m = gn::toOutputMesh(*g);
        if (m.numFaces() == 0)
            continue;
        out.bounds.push_back(arrange::itemOf(m));
        GeometryPtr ref = gn::meshGeometry(std::move(m));
        out.meshes.push_back(ref->mesh);
        out.refs.push_back(std::move(ref));
    }
    if (out.refs.empty())
        throw gn::ExecError(c.node.name + ": link one or more geometries into items");
    return out;
}

/** Polylines in a geometry: its curves, then chains of its mesh's loose edges. */
std::vector<std::vector<glm::dvec3>> linesOf(const Geometry& g)
{
    std::vector<std::vector<glm::dvec3>> out;
    if (g.curves)
    {
        const gn::CurveSet ev = g.curves->evaluated();
        for (size_t k = 0; k < ev.numCurves(); ++k)
        {
            const auto pts = ev.curvePoints(k);
            std::vector<glm::dvec3> line(pts.begin(), pts.end());
            if (ev.cyclic[k] && !line.empty())
                line.push_back(line.front());
            if (line.size() >= 2)
                out.push_back(std::move(line));
        }
    }
    if (g.mesh)
    {
        const geo::Mesh& m = *g.mesh;
        std::map<geo::Index, std::vector<geo::Index>> at;
        for (geo::Index e = 0; e < m.edgeCapacity(); ++e)
            if (m.edgeValid(e) && m.edgeFaceCount(e) == 0)
                for (int k = 0; k < 2; ++k)
                    at[m.edgePoint(e, k)].push_back(e);
        std::map<geo::Index, bool> used;
        auto walk = [&](geo::Index start, geo::Index e) {
            std::vector<glm::dvec3> line{m.position(start)};
            geo::Index p = start;
            while (e != geo::kInvalid && !used[e])
            {
                used[e] = true;
                p = m.edgeOtherPoint(e, p);
                line.push_back(m.position(p));
                e = geo::kInvalid;
                if (const auto& next = at[p]; next.size() == 2)
                    for (geo::Index n : next)
                        if (!used[n])
                            e = n;
            }
            if (line.size() >= 2)
                out.push_back(std::move(line));
        };
        for (const auto& [p, edges] : at)
            if (edges.size() != 2)
                for (geo::Index e : edges)
                    if (!used[e])
                        walk(p, e);
        for (const auto& [p, edges] : at)
            for (geo::Index e : edges)
                if (!used[e])
                    walk(p, e);
    }
    return out;
}

/** Placements as instances of the items, with item, pose and random attributes. */
GeometryPtr instancesOf(const Items& items, const std::vector<arrange::Placed>& placed, const std::vector<double>* wallDistance = nullptr)
{
    auto inst = std::make_shared<gn::Instances>();
    inst->references = items.refs;
    for (const arrange::Placed& p : placed)
        inst->add(p.transform, p.item);
    auto item = inst->attrs.getOrAdd<int32_t>("item");
    auto pose = inst->attrs.getOrAdd<int32_t>("pose");
    auto random = inst->attrs.getOrAdd<float>("random");
    for (size_t k = 0; k < placed.size(); ++k)
    {
        item[k] = placed[k].item;
        pose[k] = int32_t(placed[k].pose);
        random[k] = placed[k].random;
    }
    if (wallDistance)
    {
        auto d = inst->attrs.getOrAdd<float>("wallDistance");
        for (size_t k = 0; k < wallDistance->size() && k < placed.size(); ++k)
            d[k] = float((*wallDistance)[k]);
    }
    Geometry g;
    g.instances = std::move(inst);
    return gn::makeGeometry(std::move(g));
}

void rowExec(ExecContext& c)
{
    LILA_ZONE();

    const Items items = itemsOf(c);
    arrange::RowOptions o;
    o.gap = c.f("gap");
    o.gapJitter = c.f("gapJitter");
    o.scaleMin = c.f("scaleMin");
    o.scaleMax = c.f("scaleMax");
    o.leanChance = c.f("leanChance");
    o.leanAngle = c.f("leanAngle");
    o.lyingChance = c.f("lyingChance");
    o.lyingMax = int(c.i("lyingMax"));
    o.fill = c.f("fill");
    o.inset = c.f("inset");
    o.depthJitter = c.f("depthJitter");
    o.seed = uint32_t(c.i("seed"));
    std::vector<arrange::Placed> all;
    const GeometryPtr line = c.geometry("line");
    if (line)
        for (const std::vector<glm::dvec3>& l : linesOf(*line))
        {
            std::vector<arrange::Placed> part = arrange::row(l, items.bounds, o);
            all.insert(all.end(), part.begin(), part.end());
            ++o.seed;
        }
    c.out("instances", instancesOf(items, all));
}

void stackExec(ExecContext& c)
{
    const Items items = itemsOf(c);
    arrange::StackOptions o;
    o.count = int(std::max<int64_t>(0, c.i("count")));
    o.offset = c.f("offset");
    o.turn = c.f("turn");
    o.scaleMin = c.f("scaleMin");
    o.scaleMax = c.f("scaleMax");
    o.seed = uint32_t(c.i("seed"));
    c.out("instances", instancesOf(items, arrange::stack(c.v("base"), items.bounds, o)));
}

void pileExec(ExecContext& c)
{
    const Items items = itemsOf(c);
    arrange::PileOptions o;
    o.count = int(std::max<int64_t>(0, c.i("count")));
    o.radius = c.f("radius");
    o.gap = c.f("gap");
    o.scaleMin = c.f("scaleMin");
    o.scaleMax = c.f("scaleMax");
    o.seed = uint32_t(c.i("seed"));
    c.out("instances", instancesOf(items, arrange::pile(c.v("center"), items.bounds, o)));
}

void scatterExec(ExecContext& c)
{
    LILA_ZONE();

    const Items items = itemsOf(c);
    arrange::ScatterOptions o;
    o.maxCount = int(std::max<int64_t>(0, c.i("maxCount")));
    o.density = c.f("density");
    o.spacing = c.f("spacing");
    o.slopeMax = c.f("slopeMax");
    o.align = c.f("align");
    o.turn = c.f("turn");
    o.scaleMin = c.f("scaleMin");
    o.scaleMax = c.f("scaleMax");
    o.mask = c.s("mask");
    o.seed = uint32_t(c.i("seed"));
    for (const GeometryPtr& g : c.geometries("avoid"))
        if (g)
        {
            const arrange::Item b = arrange::itemOf(gn::toOutputMesh(*g));
            o.avoid.emplace_back(b.lo, b.hi);
        }
    const GeometryPtr surface = c.geometry("surface");
    std::vector<arrange::Placed> placed;
    if (surface)
        placed = arrange::scatter(gn::toOutputMesh(*surface), items.bounds, o);
    c.out("instances", instancesOf(items, placed));
}

std::vector<double> weightsOf(const std::string& text)
{
    std::vector<double> out;
    std::string cur;
    for (char ch : text + ",")
        if (ch == ',' || ch == ' ')
        {
            if (!cur.empty())
                out.push_back(std::atof(cur.c_str()));
            cur.clear();
        }
        else
            cur += ch;
    return out;
}

void bandExec(ExecContext& c)
{
    LILA_ZONE();

    const Items items = itemsOf(c);
    arrange::BandOptions o;
    o.width = c.f("width");
    o.density = c.f("density");
    o.falloff = c.f("falloff");
    o.contactHeight = c.f("contactHeight");
    o.spacing = c.f("spacing");
    o.scaleNear = c.f("scaleNear");
    o.scaleFar = c.f("scaleFar");
    o.scaleJitter = c.f("scaleJitter");
    o.leanChance = c.f("leanChance");
    o.leanMin = c.f("leanMin");
    o.leanMax = c.f("leanMax");
    o.turn = c.f("turn");
    o.maxCount = int(std::max<int64_t>(0, c.i("maxCount")));
    o.weights = weightsOf(c.s("weights"));
    o.seed = uint32_t(c.i("seed"));
    std::vector<geo::Mesh> hosts, ground, blockers;
    for (const GeometryPtr& g : c.geometries("blockers"))
        if (g)
            blockers.push_back(gn::toOutputMesh(*g));
    for (const GeometryPtr& g : c.geometries("hosts"))
        if (g)
            hosts.push_back(gn::toOutputMesh(*g));
    for (const GeometryPtr& g : c.geometries("ground"))
        if (g)
            ground.push_back(gn::toOutputMesh(*g));
    std::vector<const geo::Mesh*> hp, gp;
    for (const geo::Mesh& m : hosts)
        hp.push_back(&m);
    for (const geo::Mesh& m : ground)
        gp.push_back(&m);
    std::vector<const geo::Mesh*> bp;
    for (const geo::Mesh& m : blockers)
        bp.push_back(&m);
    const arrange::BandResult r = arrange::band(hp, gp, items.bounds, o, bp);
    c.out("instances", instancesOf(items, r.placed, &r.distance));
}

void settleExec(ExecContext& c)
{
    LILA_ZONE();

    const GeometryPtr in = c.geometry("instances");
    if (!in || !in->instances || in->instances->size() == 0)
    {
        c.out("instances", in ? in : gn::makeGeometry({}));
        c.out("contacts", gn::makeGeometry({}));
        return;
    }
    const gn::Instances& src = *in->instances;
    std::vector<geo::Mesh> refMeshes;
    std::vector<const geo::Mesh*> refs;
    for (const GeometryPtr& r : src.references)
        refMeshes.push_back(r ? gn::toOutputMesh(*r) : geo::Mesh());
    for (const geo::Mesh& m : refMeshes)
        refs.push_back(&m);
    std::vector<geo::Mesh> colliderMeshes;
    for (const GeometryPtr& g : c.geometries("colliders"))
        if (g)
            colliderMeshes.push_back(gn::toOutputMesh(*g));
    std::vector<const geo::Mesh*> colliders;
    for (const geo::Mesh& m : colliderMeshes)
        colliders.push_back(&m);

    arrange::SettleOptions o;
    o.seconds = c.f("seconds");
    o.ground = c.b("ground");
    o.groundHeight = c.f("groundHeight");
    o.density = c.f("density");
    o.friction = c.f("friction");
    o.bounce = c.f("bounce");
    const arrange::Settled s = arrange::settle(src.transforms, src.reference, refs, colliders, o);

    auto out = std::make_shared<gn::Instances>(src);
    out->transforms = s.transforms;
    auto rest = out->attrs.getOrAdd<bool>("atRest");
    auto impact = out->attrs.getOrAdd<float>("impact");
    auto mass = out->attrs.getOrAdd<float>("mass");
    for (size_t k = 0; k < out->size(); ++k)
    {
        rest[k] = s.atRest[k] != 0;
        impact[k] = s.impact[k];
        mass[k] = float(s.masses[k]);
    }
    Geometry settled;
    settled.instances = std::move(out);
    c.out("instances", gn::makeGeometry(std::move(settled)));

    // One point per contact, for wear: where, which pair, how hard, how far it slid, whether it still rests there.
    geo::Mesh pts;
    for (const physics::Contact& k : s.contacts)
        pts.addPoint(k.point);
    auto a = pts.pointAttrs().getOrAdd<int32_t>("a");
    auto b = pts.pointAttrs().getOrAdd<int32_t>("b");
    auto impulse = pts.pointAttrs().getOrAdd<float>("impulse");
    auto slide = pts.pointAttrs().getOrAdd<float>("slide");
    auto resting = pts.pointAttrs().getOrAdd<float>("resting");
    auto normal = pts.pointAttrs().getOrAdd<glm::vec3>("contactNormal");
    auto load = pts.pointAttrs().getOrAdd<float>("load");
    // A resting body's weight shared over its resting contacts: what presses on what, for sag.
    std::vector<int> restingCount(s.transforms.size(), 0);
    for (const physics::Contact& k : s.contacts)
        if (k.resting)
            for (int body : {k.a, k.b})
                if (body >= 0)
                    ++restingCount[size_t(body)];
    for (size_t i = 0; i < s.contacts.size(); ++i)
    {
        const physics::Contact& k = s.contacts[i];
        a[i] = k.a;
        b[i] = k.b;
        impulse[i] = float(k.peakImpulse);
        slide[i] = float(k.slide);
        resting[i] = k.resting ? 1.0f : 0.0f;
        normal[i] = glm::vec3(k.normal);
        double w = 0.0;
        if (k.resting)
            for (int body : {k.a, k.b})
                if (body >= 0 && restingCount[size_t(body)] > 0)
                    w = std::max(w, s.masses[size_t(body)] / restingCount[size_t(body)]);
        load[i] = float(w);
    }
    c.out("contacts", gn::pointGeometry(std::move(pts)));
}

std::vector<gn::SocketDecl> scaleSockets(double lo, double hi)
{
    return {in("scaleMin", Socket::Float, lo, "Smallest uniform scale an item gets.", 0.0), in("scaleMax", Socket::Float, hi, "Largest uniform scale an item gets.", 0.0)};
}

const char* kItemsDoc = "The things to place: each geometry linked in is one item (Object Info of a library master, say).";
const char* kAttrsDoc = " Instances carry item (which one), pose (0 upright, 1 leaning, 2 lying) and random (0 to 1).";
} // namespace

bool registerArrangeNodes(ext::Context& ctx)
{
    std::vector<gn::SocketDecl> row = {geoIn("line", "Where the row runs: curves, or a mesh's loose edges; items stand on it, Y up."),
                                       geoIn("items", kItemsDoc, true),
                                       in("gap", Socket::Float, 0.3, "Space between items, cm.", 0.0),
                                       in("gapJitter", Socket::Float, 0.5, "0 to 1: how much the gaps vary.", 0.0, 1.0),
                                       in("leanChance", Socket::Float, 0.08, "0 to 1: chance an item leans back on its neighbour.", 0.0, 1.0),
                                       in("leanAngle", Socket::Float, 14.0, "Most an item leans, degrees.", 0.0, 90.0),
                                       in("lyingChance", Socket::Float, 0.04, "0 to 1: chance a small stack of items lies flat.", 0.0, 1.0),
                                       in("lyingMax", Socket::Int, 4, "Most items in a lying stack.", 1),
                                       in("fill", Socket::Float, 1.0, "0 to 1: how much of the line is used, from its start.", 0.0, 1.0),
                                       in("inset", Socket::Float, 0.0, "Kept clear at both ends, cm.", 0.0),
                                       in("depthJitter", Socket::Float, 0.4, "Back and forth across the line, cm.", 0.0),
                                       in("seed", Socket::Int, 1, "Another seed gives another row.")};
    for (gn::SocketDecl& s : scaleSockets(0.9, 1.1))
        row.push_back(std::move(s));
    std::vector<gn::SocketDecl> stack = {in("base", Socket::Vector, gn::json::array({0, 0, 0}), "Where the stack stands."),
                                         geoIn("items", kItemsDoc, true),
                                         in("count", Socket::Int, 6, "How many.", 0),
                                         in("offset", Socket::Float, 1.0, "Sideways wander per level, cm.", 0.0),
                                         in("turn", Socket::Float, 6.0, "Turn per level, degrees either way.", 0.0, 180.0),
                                         in("seed", Socket::Int, 1, "Another seed gives another stack.")};
    for (gn::SocketDecl& s : scaleSockets(0.95, 1.05))
        stack.push_back(std::move(s));
    std::vector<gn::SocketDecl> pile = {in("center", Socket::Vector, gn::json::array({0, 0, 0}), "Centre of the footprint, at its floor."),
                                        geoIn("items", kItemsDoc, true),
                                        in("count", Socket::Int, 30, "How many.", 0),
                                        in("radius", Socket::Float, 40.0, "Footprint radius, cm.", 0.0),
                                        in("gap", Socket::Float, 1.0, "Space between items as they start, cm.", 0.0),
                                        in("seed", Socket::Int, 1, "Another seed gives another heap.")};
    for (gn::SocketDecl& s : scaleSockets(0.85, 1.15))
        pile.push_back(std::move(s));
    std::vector<gn::SocketDecl> scatter = {geoIn("surface", "The mesh to scatter over."),
                                           geoIn("items", kItemsDoc, true),
                                           in("maxCount", Socket::Int, 200, "At most this many.", 0),
                                           in("density", Socket::Float, 4.0, "Per square metre.", 0.0),
                                           in("spacing", Socket::Float, 1.0, "Footprints kept clear, as a multiple of each item's own.", 0.0),
                                           in("slopeMax", Socket::Float, 35.0, "Steepest surface used, degrees from up.", 0.0, 90.0),
                                           in("align", Socket::Float, 0.0, "0 upright, 1 along the surface.", 0.0, 1.0),
                                           in("turn", Socket::Float, 180.0, "Random turn about up, degrees either way.", 0.0, 180.0),
                                           in("mask", Socket::String, "", "A point attribute (0 to 1) on the surface that thins it out; empty for none."),
                                           geoIn("avoid", "Things nothing goes in or under (world space): each linked geometry's bounds.", true),
                                           in("seed", Socket::Int, 1, "Another seed gives another scatter.")};
    for (gn::SocketDecl& s : scaleSockets(0.8, 1.2))
        scatter.push_back(std::move(s));

    std::vector<gn::SocketDecl> band = {geoIn("hosts", "What the band runs round (world space): walls, buildings, rocks, Blockwork blocks.", true),
                                        geoIn("ground", "The ground (world space); none: a plane at the hosts' lowest point.", true),
                                        geoIn("blockers", "Neighbours (world space) nothing goes on, though no band grows round them.", true),
                                        geoIn("items", kItemsDoc, true),
                                        in("weights", Socket::String, "", "How often each item is picked, in order, comma separated (missing ones are 1)."),
                                        in("width", Socket::Float, 80.0, "How far out from the footprint the band reaches, cm.", 1.0),
                                        in("density", Socket::Float, 6.0, "Per square metre, at the wall.", 0.0),
                                        in("falloff", Socket::Float, 1.5, "0 even across the band; higher keeps it close to the wall.", 0.0, 8.0),
                                        in("contactHeight", Socket::Float, 40.0, "Geometry this close above the ground is the footprint, cm.", 0.0),
                                        in("spacing", Socket::Float, 0.9, "Footprints kept clear, as a multiple of each item's own.", 0.0),
                                        in("scaleNear", Socket::Float, 1.15, "Scale at the wall.", 0.0),
                                        in("scaleFar", Socket::Float, 0.6, "Scale at the band's outer edge.", 0.0),
                                        in("scaleJitter", Socket::Float, 0.2, "Random scale either way, 0 to 1.", 0.0, 0.95),
                                        in("leanChance", Socket::Float, 0.6, "Chance a tall, thin item leans on the wall.", 0.0, 1.0),
                                        in("leanMin", Socket::Float, 8.0, "Least lean, degrees.", 0.0, 80.0),
                                        in("leanMax", Socket::Float, 24.0, "Most lean, degrees.", 0.0, 80.0),
                                        in("turn", Socket::Float, 180.0, "Random turn about up for items that stand, degrees either way.", 0.0, 180.0),
                                        in("maxCount", Socket::Int, 400, "At most this many.", 0),
                                        in("seed", Socket::Int, 1, "Another seed gives another band.")};

    const gn::NodeDecl decls[] = {
        {.type = "BaseDressing",
         .label = "Base Dressing",
         .category = "Stagehand",
         .doc = std::string("Items in a band round where things meet the ground: rubble at a wall's foot, planks leaning on a shed, weeds "
                            "round a rock. Densest and biggest at the wall; tall, thin items lean on it. Any outline works.") +
                kAttrsDoc + " Also wallDistance (cm from the footprint).",
         .sockets = {.inputs = band, .outputs = {geoOut("instances", "The band, as instances.")}},
         .exec = bandExec},
        {.type = "RowAlongLine",
         .label = "Row Along Line",
         .category = "Stagehand",
         .doc = std::string("Packs items along lines side by side, as books on a shelf: their X along the line, their bottoms on it, some "
                            "leaning, a few lying in small stacks.") + kAttrsDoc,
         .sockets = {.inputs = row, .outputs = {geoOut("instances", "The row, as instances.")}},
         .exec = rowExec},
        {.type = "StackItems",
         .label = "Stack Items",
         .category = "Stagehand",
         .doc = std::string("Stacks items one on another from a point: plates, crates, books lying flat.") + kAttrsDoc,
         .sockets = {.inputs = stack, .outputs = {geoOut("instances", "The stack, as instances.")}},
         .exec = stackExec},
        {.type = "PileItems",
         .label = "Pile Items",
         .category = "Stagehand",
         .doc = std::string("Items tumbled in layers above a footprint, nothing touching, ready for Settle Instances to drop them into a heap.") +
                kAttrsDoc,
         .sockets = {.inputs = pile, .outputs = {geoOut("instances", "The items in the air, as instances.")}},
         .exec = pileExec},
        {.type = "ScatterProps",
         .label = "Scatter Props",
         .category = "Stagehand",
         .doc = std::string("Scatters items over a surface, no two closer than their footprints, on slopes gentle enough, thinned by a "
                            "mask.") + kAttrsDoc,
         .sockets = {.inputs = scatter, .outputs = {geoOut("instances", "The scatter, as instances.")}},
         .exec = scatterExec},
        {.type = "SettleInstances",
         .label = "Settle Instances",
         .category = "Stagehand",
         .doc = "Lets instances fall and come to rest against the colliders with physics, deterministically: the same inputs settle "
                "the same way. Adds atRest, impact (hardest hit, kg cm/s) and mass to the instances. Contacts come out as points "
                "with a and b (instance indices; -1 ground, -2 a collider), impulse, slide (cm), resting, contactNormal and load "
                "(kg pressing there), for wear and sag.",
         .sockets = {.inputs = {geoIn("instances", "What falls."),
                                geoIn("colliders", "What it lands on and never moves: meshes, in world space.", true),
                                in("seconds", Socket::Float, 10.0, "Longest it runs; it stops sooner once everything rests.", 0.0),
                                in("ground", Socket::Bool, false, "A floor plane under everything too."),
                                in("groundHeight", Socket::Float, 0.0, "Height of that floor."),
                                in("density", Socket::Float, 600.0, "kg per cubic metre of every item.", 1.0),
                                in("friction", Socket::Float, 0.6, "0 slides, 1 grips.", 0.0),
                                in("bounce", Socket::Float, 0.05, "0 lands dead, 1 bounces back as high.", 0.0, 1.0)},
                     .outputs = {geoOut("instances", "Where they came to rest."), geoOut("contacts", "Where they touched, as points.")}},
         .exec = settleExec},
    };
    for (const gn::NodeDecl& d : decls)
        if (!gn::registerDecl(ctx, d))
            return false;
    return true;
}
} // namespace stagehand
