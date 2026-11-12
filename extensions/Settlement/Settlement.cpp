// Settlement: the Settlement geometry node, streets, lots and building envelopes laid out inside an outline on any ground.
// Every tool and command builds a group around this node.

#include "Settlement.h"
#include "SettlementTerrain.h"
#include "Generators.h"

#include <lila/Extension.h>
#include <lila/GeometryNodes.h>
#include <lila/Log.h>
#include <lila/Profile.h>

#include <algorithm>
#include <map>
#include <stdexcept>

using namespace lila;

namespace settlement
{
namespace
{
using gn::ExecContext;
using gn::Geometry;
using gn::GeometryPtr;
using Socket = gn::SocketType;

gn::SocketDecl in(std::string id, Socket t, gn::json def, std::string doc)
{
    return {.id = std::move(id), .type = t, .defaultValue = std::move(def), .doc = std::move(doc)};
}

gn::SocketDecl in(std::string id, Socket t, gn::json def, std::string doc, std::optional<double> lo, std::optional<double> hi = std::nullopt)
{
    gn::SocketDecl s = in(std::move(id), t, std::move(def), std::move(doc));
    s.min = lo;
    s.max = hi;
    return s;
}

gn::SocketDecl geoSocket(std::string id, std::string doc) { return {.id = std::move(id), .type = Socket::Geometry, .doc = std::move(doc)}; }

struct Chain
{
    std::vector<glm::dvec3> pts;
    bool closed = false;
    int kind = 0;       // 0 street or outline, 1 road, 2 water, 3 exclusion: the int edge attributes road, water, exclude
    double width = 0.0; // the float edge attribute width, for water
};

/** Loose edges chained into lines and loops; a chain takes its kind from its first edge. */
std::vector<Chain> chains(const geo::Mesh& m)
{
    std::vector<Chain> out;
    std::map<geo::Index, std::vector<geo::Index>> at;
    std::vector<geo::Index> loose;
    for (geo::Index e = 0; e < m.edgeCapacity(); ++e)
        if (m.edgeValid(e) && m.edgeFaceCount(e) == 0)
        {
            loose.push_back(e);
            for (int k = 0; k < 2; ++k)
                at[m.edgePoint(e, k)].push_back(e);
        }
    std::vector<char> used(m.edgeCapacity(), 0);
    const std::span<const int32_t> road = m.edgeAttrs().get<int32_t>("road");
    const std::span<const int32_t> water = m.edgeAttrs().get<int32_t>("water");
    const std::span<const int32_t> exclude = m.edgeAttrs().get<int32_t>("exclude");
    const std::span<const float> width = m.edgeAttrs().get<float>("width");
    auto walk = [&](geo::Index start, geo::Index e) {
        Chain c;
        if (e < exclude.size() && exclude[e])
            c.kind = 3;
        else if (e < water.size() && water[e])
            c.kind = 2;
        else if (e < road.size() && road[e])
            c.kind = 1;
        if (e < width.size())
            c.width = width[e];
        c.pts.push_back(m.position(start));
        geo::Index p = start;
        while (e != geo::kInvalid && !used[e])
        {
            used[e] = 1;
            p = m.edgeOtherPoint(e, p);
            if (p == start)
            {
                c.closed = true;
                break;
            }
            c.pts.push_back(m.position(p));
            e = geo::kInvalid;
            if (const auto& next = at[p]; next.size() == 2)
                for (geo::Index n : next)
                    if (!used[n])
                        e = n;
        }
        if (c.pts.size() >= 2)
            out.push_back(std::move(c));
    };
    for (const auto& [p, edges] : at)
        if (edges.size() != 2)
            for (geo::Index e : edges)
                if (!used[e])
                    walk(p, e);
    for (geo::Index e : loose)
        if (!used[e])
            walk(m.edgePoint(e, 0), e);
    return out;
}

std::vector<Vec2> flatPoints(const std::vector<glm::dvec3>& pts)
{
    std::vector<Vec2> out;
    for (const glm::dvec3& p : pts)
        out.push_back({p.x, p.z});
    return out;
}

void settlementExec(ExecContext& c)
{
    LILA_ZONE();

    double planeY = 0.0;
    Site site = siteFrom(*c.geometry("site"), &planeY);
    site.size = std::max(100.0, c.f("size"));
    Options o;
    try
    {
        o = optionsFrom([&](const char* id) { return c.f(id); }, [&](const char* id) { return c.i(id); },
                        [&](const char* id) { return c.s(id); }, [&](const char* id) { return c.b(id); });
        applyEdits(site, parseEdits(c.s("edits")));
    }
    catch (const std::exception& e)
    {
        throw gn::ExecError(std::string("Settlement: ") + e.what());
    }
    const Ground ground = groundFrom(*c.geometry("ground"), planeY);
    const WalkSurface walk = walkFrom(*c.geometry("walkable"), o.walk);
    Layout l;
    try
    {
        l = buildPlanned(site, ground, o, &walk);
    }
    catch (const std::runtime_error& e)
    {
        throw gn::ExecError(std::string("Settlement: ") + e.what());
    }
    const Parts parts = toGeometry(l, ground);
    c.out("streets", parts.streets);
    c.out("lots", parts.lots);
    c.out("buildings", parts.buildings);
    c.out("water", parts.water);
    c.out("levels", parts.levels);
    std::vector<GeometryPtr> shown;
    if (c.b("showStreets"))
    {
        shown.push_back(parts.streets);
        shown.push_back(parts.water);
        shown.push_back(parts.levels);
    }
    if (c.b("showLots"))
        shown.push_back(parts.lots);
    if (c.b("showBuildings"))
        shown.push_back(parts.buildings);
    c.out("geometry", gn::makeGeometry(gn::join(shown)));
}

gn::NodeDecl settlementDecl()
{
    const Options o;
    std::string profiles;
    for (const std::string& n : profileNames())
        profiles += (profiles.empty() ? "" : ", ") + n;
    return {
        .type = "Settlement",
        .label = "Settlement",
        .category = "Settlement",
        .doc = "Lays out a settlement inside an outline: arterial roads routed over the ground from the centre to gates and "
               "attractors, streets grown, gridded or ringed by the profile's pattern, blocks between them, lots along the "
               "streets and a building envelope on each lot (the space a building may fill: footprint, floors, height, roof). "
               "Exclusions (closed loops with int edge attribute exclude, or in edits) stay clear of all of it; edits add "
               "streets and routes, routed over the ground or across the walkable mesh. "
               "Streets are ribbons with face attributes street, class (0 arterial, 1 collector, 2 local, 3 alley, 4 path) and "
               "path (the edit id, -1 when generated); lots are "
               "outlines with an edge attribute lot; envelopes are closed volumes with face attributes lot, use (0 residential, "
               "1 commercial, 2 civic), floors and part (0 wall, 1 roof, 2 floor). Curves carry the centrelines, lot outlines "
               "and footprints. Lengths are cm, Y up.",
        .sockets = {.inputs = {geoSocket("site", "Outline (the largest closed curve or loose-edge loop), fixed streets (open lines) "
                                                 "and points (the first is the centre, the rest attractors main roads run to). Loose "
                                                 "edges with int attribute road are main roads arriving from outside; with int "
                                                 "attribute water they are rivers, float attribute width their width; loops with "
                                                 "int attribute exclude are exclusions."),
                               geoSocket("ground", "Mesh the settlement stands on. Empty for flat ground at the site's height."),
                               in("profile", Socket::String, "medievalTown", "Style: " + profiles + "."),
                               in("pattern", Socket::String, "", "Override the profile's pattern: organic, grid, radial, linear, organicGrid, "
                                                              "none, tensor, contour, osm, example."),
                               in("overrides", Socket::String, "", "JSON object of profile fields to change, such as {\"maxFloors\": 6}."),
                               in("size", Socket::Float, 40000.0, "Side of the square used when the site has no outline.", 100.0),
                               in("growth", Socket::Float, o.growth, "0 to 1: how far the settlement has grown.", 0.0, 1.0),
                               in("heightScale", Socket::Float, o.heightScale, "Multiplies every building's floors.", 0.0),
                               in("widthScale", Socket::Float, o.widthScale, "Multiplies every street width.", 0.05),
                               in("lotScale", Socket::Float, o.lotScale, "Multiplies lot frontage, depth and building depth.", 0.05),
                               in("seed", Socket::Int, int64_t(o.seed), "Another seed gives another settlement on the same site."),
                               in("blocks", Socket::Bool, true, "Blocks and lots between the streets. Off for streets and paths only."),
                               in("buildings", Socket::Bool, true, "A building envelope on each lot. Off for empty lots."),
                               in("edits", Socket::String, "", "Streets, routes, exclusions and road profiles as JSON (settlementAddStreet, "
                                                          "settlementRoute and settlementAddExclusion write it)."),
                               geoSocket("walkable", "Mesh routes may walk on: a structure's ramps, terraces and floors."),
                               in("walkSlope", Socket::Float, o.walk.maxSlope, "Steepest walkable face, degrees.", 0.0, 89.0),
                               in("walkStep", Socket::Float, o.walk.step, "Height a walk may step between separate pieces, cm.", 0.0),
                               in("showStreets", Socket::Bool, true, "Include streets in geometry."),
                               in("showLots", Socket::Bool, true, "Include lot outlines in geometry."),
                               in("showBuildings", Socket::Bool, true, "Include building envelopes in geometry."),
                               in("generators", Socket::String, "", "Tensor fields, eras, districts, walls, imported streets and levels as "
                                                               "JSON (settlementCreate's generator flags write it).")},
                    .outputs = {geoSocket("geometry", "What the show inputs select, joined."),
                                geoSocket("streets", "Street ribbons and centreline curves."),
                                geoSocket("lots", "Lot outlines as loose edges and closed curves."),
                                geoSocket("buildings", "Building envelopes as closed meshes, footprints as closed curves."),
                                geoSocket("water", "River ribbons."),
                                geoSocket("levels", "Levels above the ground as curves: deck streets and outlines, elevated roads, ramps, "
                                                    "skybridges and air lanes, with class, layer and kind (0 deck, 1 elevated, 2 ramp, "
                                                    "3 skybridge, 4 air, 5 deck outline).")}},
        .exec = settlementExec,
    };
}

void setCurveInts(gn::CurveSet& cs, const std::vector<std::pair<const char*, std::vector<int32_t>>>& layers)
{
    cs.syncSizes();
    for (const auto& [name, values] : layers)
        cs.curveAttrs.add(name, geo::AttrType::Int);
    for (const auto& [name, values] : layers)
    {
        std::span<int32_t> out = cs.curveAttrs.get<int32_t>(name);
        std::copy(values.begin(), values.begin() + std::min(values.size(), out.size()), out.begin());
    }
}

std::vector<glm::dvec3> draped(const Polygon& p, const Ground& g, double lift)
{
    std::vector<glm::dvec3> out;
    for (const Vec2& q : p.pts)
        out.push_back({q.x, g.height(q) + lift, q.y});
    return out;
}
} // namespace

Site siteFrom(const gn::Geometry& g, double* planeY)
{
    Site site;
    std::vector<Chain> all;
    double ySum = 0.0;
    size_t yCount = 0;
    if (g.curves)
    {
        const gn::CurveSet ev = g.curves->evaluated();
        for (size_t k = 0; k < ev.numCurves(); ++k)
        {
            const auto pts = ev.curvePoints(k);
            all.push_back({std::vector<glm::dvec3>(pts.begin(), pts.end()), ev.cyclic[k] != 0});
        }
    }
    const geo::Mesh* meshes[] = {g.mesh.get(), g.points.get()};
    for (const geo::Mesh* m : meshes)
    {
        if (!m)
            continue;
        for (Chain& c : chains(*m))
            all.push_back(std::move(c));
        for (geo::Index p = 0; p < m->pointCapacity(); ++p)
            if (m->pointValid(p) && m->pointEdge(p) == geo::kInvalid)
            {
                site.attractors.push_back({m->position(p).x, m->position(p).z});
                ySum += m->position(p).y;
                ++yCount;
            }
    }
    site.hasCenter = !site.attractors.empty();
    double bestArea = 0.0;
    int outline = -1;
    for (size_t k = 0; k < all.size(); ++k)
    {
        for (const glm::dvec3& p : all[k].pts)
        {
            ySum += p.y;
            ++yCount;
        }
        if (all[k].kind == 0 && all[k].closed && all[k].pts.size() >= 3)
        {
            const double a = std::abs(signedArea(flatPoints(all[k].pts)));
            if (a > bestArea)
            {
                bestArea = a;
                outline = int(k);
            }
        }
    }
    for (size_t k = 0; k < all.size(); ++k)
    {
        if (int(k) == outline)
            site.outline = flatPoints(all[k].pts);
        else if (all[k].kind == 3)
        {
            Exclusion x;
            x.pts = flatPoints(all[k].pts);
            if (x.pts.size() >= 3)
                site.exclusions.push_back(std::move(x));
        }
        else if (all[k].kind == 2)
            site.water.push_back({flatPoints(all[k].pts), all[k].width > 0.0 ? all[k].width : 2000.0});
        else if (all[k].kind == 1)
            site.roads.push_back(flatPoints(all[k].pts));
        else
        {
            std::vector<Vec2> s = flatPoints(all[k].pts);
            if (all[k].closed)
                s.push_back(s.front());
            site.streets.push_back(std::move(s));
        }
    }
    if (planeY)
        *planeY = yCount ? ySum / double(yCount) : 0.0;
    return site;
}

Ground groundFrom(const gn::Geometry& g, double planeY)
{
    if (g.mesh && g.mesh->numFaces() > 0)
        return Ground::fromMesh(*g.mesh, 200.0);
    return Ground::plane(planeY);
}

WalkSurface walkFrom(const gn::Geometry& g, const WalkOptions& o)
{
    if (g.mesh && g.mesh->numFaces() > 0)
        return WalkSurface::fromMesh(*g.mesh, o);
    return {};
}

json parseEdits(const std::string& text)
{
    if (text.empty())
        return json::object();
    json j = json::parse(text, nullptr, false);
    if (j.is_discarded() || !j.is_object())
        throw std::runtime_error("edits isn't a JSON object");
    return j;
}

Options optionsFrom(const std::function<double(const char*)>& f, const std::function<int64_t(const char*)>& i,
                    const std::function<std::string(const char*)>& s, const std::function<bool(const char*)>& b)
{
    Options o;
    const std::string profile = s("profile");
    if (!builtinProfile(profile, &o.profile))
    {
        std::string names;
        for (const std::string& n : profileNames())
            names += (names.empty() ? "" : ", ") + n;
        throw std::runtime_error("unknown profile '" + profile + "' (" + names + ")");
    }
    const std::string pattern = s("pattern");
    if (!pattern.empty() && !parsePattern(pattern, &o.profile.pattern))
        throw std::runtime_error("unknown pattern '" + pattern + "' (organic, grid, radial, linear, organicGrid, none, tensor, contour, osm, example)");
    const std::string overrides = s("overrides");
    if (!overrides.empty())
    {
        json j = json::parse(overrides, nullptr, false);
        if (j.is_discarded())
            throw std::runtime_error("overrides isn't valid JSON");
        applyJson(o.profile, j);
    }
    o.seed = uint32_t(i("seed"));
    o.growth = std::clamp(f("growth"), 0.0, 1.0);
    o.heightScale = std::max(0.0, f("heightScale"));
    o.widthScale = std::max(0.05, f("widthScale"));
    o.lotScale = std::max(0.05, f("lotScale"));
    o.blocks = b("blocks");
    o.buildings = b("buildings");
    o.walk.maxSlope = std::clamp(f("walkSlope"), 0.0, 89.0);
    o.walk.step = std::max(0.0, f("walkStep"));
    if (const std::string gen = s("generators"); !gen.empty())
    {
        o.generators = json::parse(gen, nullptr, false);
        if (o.generators.is_discarded() || !o.generators.is_object())
            throw std::runtime_error("generators isn't a JSON object");
        parseGenerators(o.generators);
    }
    return o;
}

Parts toGeometry(const Layout& l, const Ground& ground)
{
    LILA_ZONE();

    MeshParts meshes = toMeshes(l, ground);
    for (const std::string& w : meshes.warnings)
        lila::log::warn("Settlement: {}", w);

    gn::CurveSet centre;
    std::vector<int32_t> lineClass, linePath, lineSurface;
    for (const StreetLine& line : streetLines(l, ground))
    {
        centre.addCurve(line.pts, false);
        lineClass.push_back(int32_t(line.cls));
        linePath.push_back(int32_t(line.path));
        lineSurface.push_back(line.surface ? 1 : 0);
    }
    setCurveInts(centre, {{"class", lineClass}, {"path", linePath}, {"surface", lineSurface}});

    gn::CurveSet lotCurves;
    std::vector<int32_t> lotId, lotUse;
    for (const Lot& lot : l.lots)
    {
        lotCurves.addCurve(draped(lot.outline, ground, 3.0), true);
        lotId.push_back(lot.id);
        lotUse.push_back(int32_t(lot.use));
    }
    setCurveInts(lotCurves, {{"lot", lotId}, {"use", lotUse}});

    gn::CurveSet footprints;
    std::vector<int32_t> fpLot, fpUse, fpFloors;
    for (const Envelope& env : l.envelopes)
    {
        std::vector<glm::dvec3> pts;
        for (const Vec2& q : env.footprint.pts)
            pts.push_back({q.x, env.ground, q.y});
        footprints.addCurve(pts, true);
        fpLot.push_back(env.lot);
        fpUse.push_back(int32_t(env.use));
        fpFloors.push_back(env.floors);
    }
    setCurveInts(footprints, {{"lot", fpLot}, {"use", fpUse}, {"floors", fpFloors}});

    gn::CurveSet levels;
    std::vector<int32_t> levelClass, levelLayer, levelKind;
    for (const LevelLine& line : levelLines(l, ground))
    {
        levels.addCurve(line.pts, false);
        levelClass.push_back(int32_t(line.cls));
        levelLayer.push_back(line.layer);
        const char* kinds[] = {"deck", "elevated", "ramp", "skybridge", "air"};
        levelKind.push_back(int32_t(std::ranges::find(kinds, line.kind) - std::begin(kinds)));
    }
    if (l.gen)
        for (size_t k = 0; k < l.gen->levels.size(); ++k)
            if (const Level& lv = l.gen->levels[k]; lv.kind == "deck" && lv.outline.pts.size() >= 3)
            {
                std::vector<glm::dvec3> ring;
                for (const Vec2& q : lv.outline.pts)
                    ring.push_back({q.x, lv.height, q.y});
                levels.addCurve(ring, true);
                levelClass.push_back(int32_t(StreetClass::Boundary));
                levelLayer.push_back(int32_t(k) + 1);
                levelKind.push_back(5);
            }
    setCurveInts(levels, {{"class", levelClass}, {"layer", levelLayer}, {"kind", levelKind}});

    auto pack = [](geo::Mesh m, gn::CurveSet cs) {
        Geometry g;
        if (m.numPoints() > 0)
            g.mesh = std::make_shared<const geo::Mesh>(std::move(m));
        if (cs.numCurves() > 0)
            g.curves = std::make_shared<const gn::CurveSet>(std::move(cs));
        return gn::makeGeometry(std::move(g));
    };
    return {pack(std::move(meshes.streets), std::move(centre)), pack(std::move(meshes.lots), std::move(lotCurves)),
            pack(std::move(meshes.envelopes), std::move(footprints)), pack(std::move(meshes.water), gn::CurveSet()),
            pack(geo::Mesh(), std::move(levels))};
}

bool registerNodes(ext::Context& ctx) { return gn::registerDecl(ctx, settlementDecl()) != nullptr; }
} // namespace settlement

LILA_EXTENSION_EXPORT int lilaExtensionInit(lila::ext::Context& ctx)
{
    if (!settlement::registerNodes(ctx))
        return 1;
    settlement::registerCommands(ctx);
    settlement::registerUi(ctx);
    return 0;
}
