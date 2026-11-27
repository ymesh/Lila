// Settlement: eras and districts. A settlement (or a district of it) runs a sequence of eras, each a profile and a
// generator over its own region: an organic core inside walls, an avenue cut through it, the walls replaced by a ring
// boulevard with a grid beyond, then highways and suburbs. Every street keeps the era it was laid in. Also the hooks
// build calls around its pattern.

#include "Generators.h"
#include "Parcels.h"

#include <lila/Profile.h>

#include <algorithm>
#include <cmath>
#include <limits>
#include <map>
#include <stdexcept>

namespace settlement
{
namespace
{
constexpr int kOldEra = (1 << 20) / 2; // inside gen::withEra, edges at or past this were there before

/** A named profile as build scales the settlement's: the options' width and lot scales applied. */
Profile scaledProfile(const Profile& base, const std::string& name, const json& overrides, const Options& o)
{
    if (name.empty())
        return gen::resolveProfile(base, "", overrides);
    Profile p = gen::resolveProfile(base, name, overrides);
    for (double* w : {&p.widthArterial, &p.widthCollector, &p.widthLocal, &p.widthAlley, &p.widthPath})
        *w *= std::max(o.widthScale, 0.05);
    for (double* w : {&p.lotWidth, &p.lotDepth, &p.buildDepth})
        *w *= std::max(o.lotScale, 0.05);
    p.minLot *= std::max(o.lotScale * o.lotScale, 0.0025);
    return p;
}

GenLayout& genOf(Layout& l)
{
    if (!l.gen)
        l.gen = std::make_shared<GenLayout>();
    return *l.gen;
}

/** The layout's outline, centre and radius, put back when it goes out of scope. */
struct RegionScope
{
    Layout& l;
    Polygon outline;
    Vec2 center;
    double radius;
    explicit RegionScope(Layout& x) : l(x), outline(x.outline), center(x.center), radius(x.radius) {}
    ~RegionScope()
    {
        l.outline = outline;
        l.center = center;
        l.radius = radius;
    }
};

/** A road profile by name in the layout's list, added when missing. */
int roadProfileIndex(Layout& l, const char* name)
{
    for (size_t k = 0; k < l.roads.size(); ++k)
        if (l.roads[k].name == name)
            return int(k);
    RoadProfile r;
    builtinRoadProfile(name, &r);
    l.roads.push_back(r);
    return int(l.roads.size()) - 1;
}

/** Removes the new streets (laid in this era) that lie inside the inner circle: the era runs outside it. */
void dropInside(Layout& l, double inner)
{
    if (inner <= 0.0)
        return;
    StreetGraph& g = l.graph;
    for (size_t e = 0; e < g.edges.size(); ++e)
    {
        const StreetEdge& x = g.edges[e];
        if (!g.isStreet(int(e)) || x.era >= kOldEra || x.path >= 0)
            continue;
        const Vec2 a = g.nodes[size_t(x.a)].p, b = g.nodes[size_t(x.b)].p;
        if (glm::length((a + b) * 0.5 - l.center) < inner)
            g.removeEdge(int(e));
    }
}

/** New streets cut at the inner circle are joined to the older network just inside it. */
void joinInward(Layout& l, const Ground& g, const Profile& p, double inner)
{
    StreetGraph& graph = l.graph;
    std::vector<int> ends;
    for (size_t n = 0; n < graph.nodes.size(); ++n)
    {
        if (graph.degree(int(n)) != 1)
            continue;
        const double d = glm::length(graph.nodes[n].p - l.center);
        if (std::abs(d - inner) > p.segment)
            continue;
        for (int e : graph.incident(int(n)))
            if (graph.isStreet(e) && graph.edges[size_t(e)].era < kOldEra && graph.edges[size_t(e)].cls <= StreetClass::Collector)
                ends.push_back(int(n));
    }
    for (int n : ends)
    {
        const Vec2 at = graph.nodes[size_t(n)].p;
        int best = -1;
        double bestD = 2.5 * p.segment;
        for (size_t m = 0; m < graph.nodes.size(); ++m)
        {
            if (graph.degree(int(m)) == 0 || int(m) == n)
                continue;
            bool old = false;
            for (int e : graph.incident(int(m)))
                old = old || (graph.isStreet(e) && graph.edges[size_t(e)].era >= kOldEra);
            if (!old)
                continue;
            const double d = glm::length(graph.nodes[m].p - at);
            if (d < bestD && glm::length(graph.nodes[m].p - l.center) < glm::length(at - l.center))
            {
                bestD = d;
                best = int(m);
            }
        }
        if (best >= 0)
        {
            const Vec2 line[] = {at, graph.nodes[size_t(best)].p};
            gen::lay(l, g, p, line, StreetClass::Collector, 0);
        }
    }
}

/** A straight avenue through the region: older streets running along it come down, and the corridor is recorded. */
void cutAxis(Layout& l, const Ground& g, const Profile& p, const AxisSpec& axis, int index, int count)
{
    GenLayout& gen = genOf(l);
    Vec2 a, b;
    if (axis.pts.size() == 2)
    {
        a = axis.pts[0];
        b = axis.pts[1];
    }
    else
    {
        const double t = (axis.angle + 180.0 * index / std::max(count, 1)) * kPi / 180.0;
        const Vec2 d(std::cos(t), std::sin(t));
        a = l.center - d * (1.5 * l.radius);
        b = l.center + d * (1.5 * l.radius);
    }
    const double width = p.widthArterial * 1.3;
    const double clear = axis.clearance > 0.0 ? axis.clearance : 0.5 * width + 0.3 * p.spacing;
    const Vec2 u = glm::normalize(b - a);
    const double keep = p.plaza ? 2.0 * std::max(1.25 * p.lotDepth, 2.0 * p.widthCollector) : 0.0;
    StreetGraph& graph = l.graph;
    for (size_t e = 0; e < graph.edges.size(); ++e)
    {
        const StreetEdge& x = graph.edges[e];
        if (!graph.isStreet(int(e)) || x.era < kOldEra || x.path >= 0 || x.cls == StreetClass::Arterial)
            continue;
        const Vec2 pa = graph.nodes[size_t(x.a)].p, pb = graph.nodes[size_t(x.b)].p;
        if (glm::length(pa - l.center) < keep && glm::length(pb - l.center) < keep)
            continue;
        const Vec2 seg[] = {a, b};
        const double da = gen::lineDistance(seg, pa), db = gen::lineDistance(seg, pb), dm = gen::lineDistance(seg, (pa + pb) * 0.5);
        const Vec2 d = pb - pa;
        const bool along = glm::length(d) > 1e-9 && std::abs(glm::dot(glm::normalize(d), u)) > 0.85;
        if ((da < clear && db < clear) || (dm < clear && along))
        {
            graph.removeEdge(int(e));
            ++gen.demolishedStreets;
        }
    }
    for (const std::vector<Vec2>& run : gen::clip(std::vector<Vec2>{a, b}, l.outline, l.center, 0.0, 0.0))
    {
        const Vec2 r0 = run.front(), r1 = run.back(), n = leftNormal(u) * clear;
        gen.cleared.push_back(makePolygon({r0 - n, r1 - n, r1 + n, r0 + n}));
    }
    // The axes run to the market square's edge, so the square stays whole.
    const double half = p.plaza ? std::max(1.25 * p.lotDepth, 2.0 * p.widthCollector) * std::sqrt(2.0) : 0.0;
    const double t = glm::dot(l.center - a, u), len = glm::length(b - a);
    if (half > 0.0 && t > half && t < len - half)
    {
        const Vec2 first[] = {a, a + u * (t - half)}, second[] = {a + u * (t + half), b};
        gen::lay(l, g, p, first, StreetClass::Arterial, 0, 0.0, 0.0, width);
        gen::lay(l, g, p, second, StreetClass::Arterial, 0, 0.0, 0.0, width);
        return;
    }
    const Vec2 line[] = {a, b};
    gen::lay(l, g, p, line, StreetClass::Arterial, 0, 0.0, 0.0, width);
}

void demolishWalls(Layout& l, int era)
{
    GenLayout& gen = genOf(l);
    StreetGraph& graph = l.graph;
    for (WallLine& w : gen.walls)
    {
        if (w.demolished >= 0 || w.era >= era)
            continue;
        w.demolished = era;
        for (size_t e = 0; e < graph.edges.size(); ++e)
        {
            const StreetEdge& x = graph.edges[e];
            if (!x.alive || x.cls != StreetClass::Boundary)
                continue;
            const Vec2 m = (graph.nodes[size_t(x.a)].p + graph.nodes[size_t(x.b)].p) * 0.5;
            if (gen::lineDistance(w.pts, m, nullptr, true) < graph.snap() + 1.0)
                graph.removeEdge(int(e));
        }
    }
    for (size_t e = 0; e < graph.edges.size(); ++e)
        if (graph.edges[e].alive && graph.edges[e].moat)
            graph.removeEdge(int(e));
    std::erase_if(l.water, [](const WaterLine& w) { return w.kind == "moat"; });
}

void runEras(Layout& l, const Ground& g, const Options& o, std::span<const EraSpec> eras, int firstEra);

/** One generator over the current region, inside an era (new edges are the ones below kOldEra). */
void runPattern(Layout& l, const Ground& g, const Options& o, Pattern pattern, double inner, double outer)
{
    const Profile& p = o.profile;
    switch (pattern)
    {
    case Pattern::Organic:
    case Pattern::Linear: growOrganic(l, g, o, outer); break;
    case Pattern::Grid: layGrid(l, g, o, inner); break;
    case Pattern::Radial: layRadial(l, g, o); break;
    case Pattern::OrganicGrid:
    {
        const double core = std::clamp(p.coreRadius, 0.05, 0.95) * (outer > 0.0 ? outer : l.radius);
        growOrganic(l, g, o, core);
        gen::lay(l, g, p, gen::circle(l.center, core, int(2.0 * kPi * core / std::max(p.segment, 100.0))), StreetClass::Arterial, 0);
        layGrid(l, g, o, std::max(core, inner));
        break;
    }
    case Pattern::None: break;
    default: layPattern(l, g, o, pattern, outer, inner); break;
    }
    if (inner > 0.0 && pattern != Pattern::Grid && pattern != Pattern::OrganicGrid)
    {
        dropInside(l, inner);
        joinInward(l, g, p, inner);
    }
}

void runEra(Layout& l, const Ground& g, const Options& o, const EraSpec& era, int index)
{
    LILA_ZONE();

    GenLayout& gen = genOf(l);
    const Profile& p = o.profile;
    const double full = l.radius;
    RegionScope scope(l);
    bool ringed = false;
    if (era.demolishWalls)
    {
        std::vector<std::vector<Vec2>> lines;
        for (const WallLine& w : gen.walls)
            if (w.demolished < 0 && w.era < index)
                lines.push_back(w.pts);
        demolishWalls(l, index);
        if (era.ring)
            for (std::vector<Vec2> line : lines)
            {
                line.push_back(line.front());
                gen::lay(l, g, p, line, StreetClass::Arterial, 0, 0.0, 0.0, p.widthArterial * 1.2);
                ringed = true;
            }
        if (lines.empty() && era.ring)
            gen.warnings.push_back("era " + std::to_string(index) + ": no walls to demolish, so the ring boulevard runs at the era's inner edge");
    }
    if (era.ring && !ringed && era.region.inner > 0.0)
        gen::lay(l, g, p, gen::circle(l.center, era.region.inner * full, 96), StreetClass::Arterial, 0, 0.0, 0.0, p.widthArterial * 1.2);
    if (!era.region.polygon.empty())
    {
        Polygon poly = makePolygon(era.region.polygon);
        if (!poly.pts.empty())
        {
            l.outline = poly;
            l.center = centroid(poly);
            l.radius = 0.0;
            for (const Vec2& q : poly.pts)
                l.radius = std::max(l.radius, glm::length(q - l.center));
        }
    }
    else if (era.region.outer < 1.0)
    {
        const std::vector<Vec2> ring = gen::circle(l.center, era.region.outer * full, 96);
        Polygon clipped = gen::intersectConvex(l.outline, std::span<const Vec2>(ring.data(), ring.size() - 1));
        if (!clipped.pts.empty())
            l.outline = clipped;
        l.radius = era.region.outer * full;
    }
    if (era.walls)
    {
        const double r = era.walls->radius > 0.0 ? era.walls->radius * full : 0.95 * l.radius;
        layWalls(l, g, p, *era.walls, index, r);
    }
    const Pattern pattern = era.pattern ? *era.pattern : p.pattern;
    const double inner = era.region.polygon.empty() ? era.region.inner * full : 0.0;
    const double outer = era.region.polygon.empty() && era.region.outer < 1.0 ? era.region.outer * full : 0.0;
    runPattern(l, g, o, pattern, inner, outer);
    {
        std::vector<int> made;
        for (size_t e = 0; e < l.graph.edges.size(); ++e)
            if (l.graph.isStreet(int(e)) && l.graph.edges[e].era < kOldEra)
                made.push_back(int(e));
        gen::closeEnds(l, g, p, made, std::max(p.spacing, p.segment));
    }
    for (size_t k = 0; k < era.axes.size(); ++k)
        for (int i = 0; i < (era.axes[k].pts.empty() ? era.axes[k].count : 1); ++i)
            cutAxis(l, g, p, era.axes[k], i, era.axes[k].count);
    if (era.highwayRing > 0.0)
    {
        l.outline = scope.outline;
        const int prof = roadProfileIndex(l, "highway");
        const double width = l.roads[size_t(prof)].width();
        std::vector<int> made = gen::lay(l, g, p, gen::circle(scope.center, era.highwayRing * full, 128), StreetClass::Arterial, 0, 0.0, 0.0, width);
        for (int s = 0; s < era.highwaySpurs; ++s)
        {
            const double t = 2.0 * kPi * (s + 0.5) / era.highwaySpurs;
            const Vec2 d(std::cos(t), std::sin(t));
            const Vec2 line[] = {scope.center + d * (era.highwayRing * full), scope.center + d * (2.0 * full)};
            for (int e : gen::lay(l, g, p, line, StreetClass::Arterial, 0, 0.0, 0.0, width))
                made.push_back(e);
        }
        for (int e : made)
            if (e < int(l.graph.edges.size()))
                l.graph.edges[size_t(e)].profile = prof;
    }
}

void runEras(Layout& l, const Ground& g, const Options& o, std::span<const EraSpec> eras, int firstEra)
{
    GenLayout& gen = genOf(l);
    for (size_t k = 0; k < eras.size(); ++k)
    {
        const EraSpec& era = eras[k];
        const int index = firstEra + int(k);
        Options eo = o;
        eo.profile = scaledProfile(o.profile, era.profile, era.overrides, o);
        eo.seed = o.seed + uint32_t(7919 * (index + 1));
        eo.growth = std::clamp(era.growth * o.growth, 0.0, 1.0);
        if (int(gen.eras.size()) <= index)
            gen.eras.resize(size_t(index) + 1);
        if (gen.eras[size_t(index)].name.empty())
            gen.eras[size_t(index)] = {era.name.empty() ? "era" + std::to_string(index) : era.name, eo.profile};
        gen::withEra(l, index, [&] { runEra(l, g, eo, era, index); });
    }
}

/** The era whose streets bound most of the polygon's sides (by length), latest on a tie. */
int eraAround(const Layout& l, const Polygon& poly)
{
    std::map<int, double> weight;
    double widest = 0.0;
    for (size_t e = 0; e < l.graph.edges.size(); ++e)
        if (l.graph.isStreet(int(e)))
            widest = std::max(widest, edgeWidth(l, int(e)));
    const double reach = 0.6 * widest + l.graph.snap() + 100.0;
    for (size_t k = 0; k < poly.size(); ++k)
    {
        const Vec2 a = poly.pts[k], b = poly.pts[(k + 1) % poly.size()];
        Vec2 at{0.0};
        const int e = l.graph.nearestEdge((a + b) * 0.5, reach, &at);
        if (e >= 0 && l.graph.isStreet(e))
            weight[l.graph.edges[size_t(e)].era] += glm::length(b - a);
    }
    int best = 0;
    double most = -1.0;
    for (const auto& [era, w] : weight)
        if (w >= most)
        {
            most = w;
            best = era;
        }
    return best;
}
} // namespace

void prepareGenerators(Layout& l, const Ground& g, const Options& o)
{
    LILA_ZONE();

    GenLayout& gen = genOf(l);
    gen.spec = std::make_shared<const GenSpec>(parseGenerators(o.generators));
    const GenSpec& spec = *gen.spec;
    if (spec.eras.empty())
        gen.eras.push_back({o.profile.name, o.profile});
    if (!spec.osm.empty())
        layImported(l, spec.osm, 0);
    if (spec.walls && spec.eras.empty())
    {
        const double r = (spec.walls->radius > 0.0 ? spec.walls->radius : std::clamp(o.profile.coreRadius, 0.05, 0.95)) * l.radius;
        layWalls(l, g, o.profile, *spec.walls, 0, r);
    }
}

bool layEras(Layout& l, const Ground& g, const Options& o)
{
    if (!l.gen || !l.gen->spec || l.gen->spec->eras.empty())
        return false;
    LILA_ZONE();
    const std::shared_ptr<const GenSpec> spec = l.gen->spec;
    runEras(l, g, o, spec->eras, 0);
    return true;
}

void layPattern(Layout& l, const Ground& g, const Options& o, Pattern p, double limit, double inner)
{
    LILA_ZONE();
    const std::shared_ptr<const GenSpec> spec = l.gen ? l.gen->spec : nullptr;
    static const GenSpec kEmpty;
    const GenSpec& s = spec ? *spec : kEmpty;
    switch (p)
    {
    case Pattern::Tensor: layTensor(l, g, o, s.tensor ? *s.tensor : TensorSpec{}, limit); break;
    case Pattern::Contour: layContour(l, g, o, s.contour, limit); break;
    case Pattern::Osm:
        if (s.osm.empty())
            throw std::runtime_error("pattern osm needs imported streets (settlementCreate osm= or osmFile=)");
        break;
    case Pattern::Example: layExample(l, g, o, s.example, limit); break;
    default: break;
    }
    (void)inner;
}

void afterPattern(Layout& l, const Ground& g, const Options& o)
{
    if (!l.gen || !l.gen->spec)
        return;
    LILA_ZONE();
    GenLayout& gen = *l.gen;
    const std::shared_ptr<const GenSpec> spec = gen.spec;
    const bool outlineConvex = [&] {
        const size_t n = l.outline.size();
        for (size_t k = 0; k < n; ++k)
            if (cross(l.outline.pts[(k + 1) % n] - l.outline.pts[k], l.outline.pts[(k + 2) % n] - l.outline.pts[(k + 1) % n]) < -1e-6)
                return false;
        return true;
    }();
    for (size_t i = 0; i < spec->districts.size(); ++i)
    {
        const DistrictSpec& d = spec->districts[i];
        gen.districts.push_back(d.name.empty() ? "district" + std::to_string(i) : d.name);
        Polygon poly = makePolygon(d.polygon);
        if (outlineConvex)
            poly = gen::intersectConvex(poly, l.outline.pts);
        else
            for (const Vec2& q : poly.pts)
                if (!contains(l.outline.pts, q))
                {
                    gen.warnings.push_back("district " + gen.districts.back() + " reaches past the settlement's outline; it is laid as given");
                    break;
                }
        if (poly.pts.empty())
        {
            gen.warnings.push_back("district " + gen.districts.back() + " lies outside the settlement; skipped");
            continue;
        }
        // The district's own streets replace what the settlement grew there; main roads through it stay.
        StreetGraph& graph = l.graph;
        for (size_t e = 0; e < graph.edges.size(); ++e)
        {
            const StreetEdge& x = graph.edges[e];
            if (!graph.isStreet(int(e)) || x.path >= 0 || x.cls == StreetClass::Arterial || x.layer != 0)
                continue;
            if (contains(poly.pts, (graph.nodes[size_t(x.a)].p + graph.nodes[size_t(x.b)].p) * 0.5))
                graph.removeEdge(int(e));
        }
        RegionScope scope(l);
        l.outline = poly;
        l.center = centroid(poly);
        l.radius = 0.0;
        for (const Vec2& q : poly.pts)
            l.radius = std::max(l.radius, glm::length(q - l.center));
        Options dop = o;
        dop.profile = scaledProfile(o.profile, d.profile, d.overrides, o);
        gen.districtOutlines.push_back(poly);
        gen.districtProfiles.push_back(dop.profile);
        dop.seed = d.seed ? d.seed : o.seed + uint32_t(104729 * (i + 1));
        dop.growth = std::clamp(d.growth * o.growth, 0.0, 1.0);
        if (!d.eras.empty())
        {
            runEras(l, g, dop, d.eras, 0);
            gen.districtEras.push_back(int(d.eras.size()) - 1);
            continue;
        }
        const int era = d.era >= 0 ? d.era : std::max(0, int(gen.eras.size()) - 1);
        gen.districtEras.push_back(era);
        if (int(gen.eras.size()) <= era)
            gen.eras.resize(size_t(era) + 1, {"era" + std::to_string(era), dop.profile});
        const Pattern pattern = d.pattern ? *d.pattern : dop.profile.pattern;
        gen::withEra(l, era, [&] { runPattern(l, g, dop, pattern, 0.0, 0.0); });
    }
    if (spec->levels)
        layLevels(l, g, o, *spec->levels);
}

void afterStreets(Layout& l, const Ground& g, const Options&)
{
    if (!l.gen)
        return;
    LILA_ZONE();
    GenLayout& gen = *l.gen;
    // Lanes cut through a district's large blocks belong to the district's era.
    for (size_t d = 0; d < gen.districtOutlines.size() && d < gen.districtEras.size(); ++d)
        for (StreetEdge& x : l.graph.edges)
            if (x.alive && x.cls > StreetClass::Arterial && x.cls < StreetClass::Boundary && x.path < 0 && x.era < gen.districtEras[d] &&
                contains(gen.districtOutlines[d].pts, (l.graph.nodes[size_t(x.a)].p + l.graph.nodes[size_t(x.b)].p) * 0.5))
                x.era = gen.districtEras[d];
    if (!gen.walls.empty())
        finishWalls(l, g);
}

void afterLots(Layout& l, const Site& site, const Ground& g, const Options& o)
{
    if (!l.gen)
        return;
    LILA_ZONE();
    GenLayout& gen = *l.gen;
    const bool eras = gen.eras.size() > 1;
    if (!eras && gen.districtOutlines.empty())
    {
        if (gen.spec && gen.spec->levels)
            finishLevels(l, g, o, *gen.spec->levels);
        return;
    }
    if (eras)
    {
        for (Block& b : l.blocks)
            b.era = eraAround(l, b.outline);
        for (Lot& lot : l.lots)
            if (lot.block >= 0 && lot.block < int(l.blocks.size()))
                lot.era = l.blocks[size_t(lot.block)].era;
    }
    // Envelopes take the style of their district, else of their era: floors, heights, depth and roof of that profile.
    std::vector<Envelope> kept;
    bool restyled = false;
    for (Envelope& env : l.envelopes)
    {
        const Lot& lot = l.lots[size_t(env.lot)];
        const int era = std::clamp(lot.era, 0, int(gen.eras.size()) - 1);
        const Profile* style = era > 0 ? &gen.eras[size_t(era)].profile : nullptr;
        const Vec2 c = centroid(lot.outline);
        for (size_t d = 0; d < gen.districtOutlines.size(); ++d)
            if (contains(gen.districtOutlines[d].pts, c))
                style = &gen.districtProfiles[d];
        // An imported building keeps the heights and roof its tags gave it.
        if (!style || style->name == o.profile.name || !lot.osmId.empty())
        {
            env.era = era;
            kept.push_back(std::move(env));
            continue;
        }
        Options eo = o;
        eo.profile = *style;
        Envelope styled = parcelEnvelope(lot, l.blocks[size_t(lot.block)], l, g, eo, site);
        if (styled.footprint.pts.empty())
            continue;
        styled.era = era;
        kept.push_back(std::move(styled));
        restyled = true;
    }
    l.envelopes = std::move(kept);
    if (restyled)
        matchPartyWalls(l.envelopes);
    if (gen.spec && gen.spec->levels)
        finishLevels(l, g, o, *gen.spec->levels);
}
} // namespace settlement
