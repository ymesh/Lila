// Settlement: example-based streets. Imported streets (OpenStreetMap through terrainImportOsm, or any polylines with
// classes) go into the street graph as they are; or their numbers (orientation histogram, junction degrees, block
// size, segment length) steer a new layout on another site towards the same character.

#include "Generators.h"

#include <lila/Profile.h>

#include <algorithm>
#include <cmath>
#include <numeric>

namespace settlement
{
namespace
{
double medianOf(std::vector<double> v)
{
    if (v.empty())
        return 0.0;
    std::sort(v.begin(), v.end());
    return v.size() % 2 ? v[v.size() / 2] : 0.5 * (v[v.size() / 2 - 1] + v[v.size() / 2]);
}

/** The dominant grid direction (degrees, 0 to 90) and how strongly the streets keep to it (0 to 1). */
void gridAxis(const StreetStats& s, double* angle, double* order)
{
    double c = 0.0, sn = 0.0, total = 0.0;
    if (std::isfinite(s.axis))
    {
        for (int b = 0; b < 36; ++b)
        {
            const double t = (b * 10.0 + 5.0) * kPi / 180.0;
            c += s.bins[b] * std::cos(4.0 * t);
            sn += s.bins[b] * std::sin(4.0 * t);
            total += s.bins[b];
        }
        *angle = s.axis;
        *order = total > 0.0 ? std::sqrt(c * c + sn * sn) / total : 0.0;
        return;
    }
    for (int b = 0; b < 36; ++b)
    {
        const double t = (b * 10.0 + 5.0) * kPi / 180.0;
        c += s.bins[b] * std::cos(4.0 * t);
        sn += s.bins[b] * std::sin(4.0 * t);
        total += s.bins[b];
    }
    *angle = total > 0.0 ? std::atan2(sn, c) / 4.0 * 180.0 / kPi : 0.0;
    *order = total > 0.0 ? std::sqrt(c * c + sn * sn) / total : 0.0;
}

/** Node degrees among the nodes the given edges touch. */
void degreeShares(const StreetGraph& g, std::span<const int> edges, double* deadEnds, double* fourWay, std::vector<int>* nodes)
{
    std::vector<char> seen(g.nodes.size(), 0);
    int counts[6] = {};
    for (int e : edges)
        if (e < int(g.edges.size()) && g.isStreet(e))
            for (int n : {g.edges[size_t(e)].a, g.edges[size_t(e)].b})
                if (!seen[size_t(n)])
                {
                    seen[size_t(n)] = 1;
                    if (nodes)
                        nodes->push_back(n);
                }
    int total = 0;
    for (size_t n = 0; n < seen.size(); ++n)
        if (seen[n])
            if (const int d = g.degree(int(n)); d > 0)
            {
                ++counts[std::min(d, 5)];
                ++total;
            }
    *deadEnds = total ? double(counts[1]) / total : 0.0;
    const int junctions = counts[3] + counts[4] + counts[5];
    *fourWay = junctions ? double(counts[4] + counts[5]) / junctions : 0.0;
}
} // namespace

void layImported(Layout& l, std::span<const ImportedStreet> streets, int era)
{
    LILA_ZONE();

    if (!l.gen)
        l.gen = std::make_shared<GenLayout>();
    int laid = 0;
    for (const ImportedStreet& s : streets)
    {
        bool any = false;
        for (const std::vector<Vec2>& run : gen::clip(s.pts, l.outline, l.center, 0.0, 0.0, l.exclusions))
        {
            l.graph.addPolyline(run, s.cls, era);
            if (!s.name.empty() || !s.osmId.empty())
                l.gen->importedRuns.push_back({run, s.cls, s.name, s.osmId});
            any = true;
        }
        laid += any ? 1 : 0;
    }
    l.gen->imported += laid;
    if (laid < int(streets.size()))
        l.gen->warnings.push_back("osm: " + std::to_string(int(streets.size()) - laid) + " of " + std::to_string(streets.size()) +
                                  " imported streets lie outside the settlement's outline and were left out");
}

StreetStats streetStats(const StreetGraph& g, std::span<const Block> blocks)
{
    LILA_ZONE();
    StreetStats s;
    double total = 0.0, c4 = 0.0, s4 = 0.0;
    int segments = 0;
    for (size_t e = 0; e < g.edges.size(); ++e)
    {
        if (!g.isStreet(int(e)))
            continue;
        const Vec2 d = g.nodes[size_t(g.edges[e].b)].p - g.nodes[size_t(g.edges[e].a)].p;
        const double len = glm::length(d);
        const double bearing = std::atan2(d.y, d.x) * 180.0 / kPi;
        c4 += len * std::cos(4.0 * bearing * kPi / 180.0);
        s4 += len * std::sin(4.0 * bearing * kPi / 180.0);
        for (double b : {bearing, bearing + 180.0})
            s.bins[std::min(35, int(std::fmod(b + 720.0, 360.0) / 10.0))] += len;
        total += len;
    }
    int counts[6] = {}, nodes = 0, ends = 0;
    for (size_t n = 0; n < g.nodes.size(); ++n)
        if (const int d = g.degree(int(n)); d > 0)
        {
            ++counts[std::min(d, 5)];
            ++nodes;
            if (d != 2)
                ends += d;
        }
    for (int d = 1; d <= 5; ++d)
        s.fraction[d] = nodes ? double(counts[d]) / nodes : 0.0;
    segments = std::max(1, ends / 2);
    s.meanSegment = total / segments;
    std::vector<double> areas;
    if (!blocks.empty())
        for (const Block& b : blocks)
            areas.push_back(b.area);
    else
        for (const Face& f : planarFaces(g))
            areas.push_back(area(f.outline) / kM2);
    s.blockMedian = medianOf(std::move(areas));
    if (total > 0.0)
        s.axis = std::fmod(std::atan2(s4, c4) / 4.0 * 180.0 / kPi + 90.0, 90.0);
    s.valid = total > 0.0;
    return s;
}

StreetStats statsFromStreets(std::span<const ImportedStreet> streets)
{
    StreetGraph g(150.0, 2000.0);
    for (const ImportedStreet& s : streets)
        if (s.cls < StreetClass::Path)
            g.addPolyline(s.pts, s.cls);
    return streetStats(g);
}

json toJson(const StreetStats& s)
{
    json bins = json::array();
    for (double b : s.bins)
        bins.push_back(b);
    return {{"orientation", bins},
            {"degree", {{"1", s.fraction[1]}, {"2", s.fraction[2]}, {"3", s.fraction[3]}, {"4", s.fraction[4]}, {"5+", s.fraction[5]}}},
            {"blockAreaMedianM2", s.blockMedian},
            {"meanSegment", s.meanSegment},
            {"axis", std::isfinite(s.axis) ? json(s.axis) : json()}};
}

StreetStats statsFromJson(const json& j)
{
    StreetStats s;
    if (!j.is_object())
        throw std::runtime_error("generators: example: takes {orientation, degree, blockAreaMedianM2, meanSegment} or {streets}");
    for (auto& [k, v] : j.items())
        if (k != "orientation" && k != "degree" && k != "blockAreaMedianM2" && k != "meanSegment" && k != "axis")
            throw std::runtime_error("generators: example: unknown field '" + k + "' (orientation, degree, blockAreaMedianM2, meanSegment, axis, streets)");
    if (j.contains("orientation"))
    {
        const json& o = j["orientation"];
        if (!o.is_array() || (o.size() != 36 && o.size() != 18))
            throw std::runtime_error("generators: example: orientation takes 36 (or 18) bins of street length by bearing");
        const size_t n = o.size() == 18 ? 18 : 36;
        for (size_t b = 0; b < 36; ++b)
            s.bins[b] = o[b % n].is_number() ? std::max(0.0, o[b % n].get<double>()) : 0.0;
    }
    if (j.contains("degree") && j["degree"].is_object())
        for (int d = 1; d <= 5; ++d)
        {
            const std::string key = d == 5 ? "5+" : std::to_string(d);
            s.fraction[d] = j["degree"].value(key, 0.0);
        }
    s.blockMedian = j.value("blockAreaMedianM2", 0.0);
    s.meanSegment = j.value("meanSegment", 0.0);
    if (j.contains("axis") && j["axis"].is_number())
        s.axis = j["axis"].get<double>();
    s.valid = std::accumulate(std::begin(s.bins), std::end(s.bins), 0.0) > 0.0;
    return s;
}

void layExample(Layout& l, const Ground& g, const Options& o, const StreetStats& stats, double limit)
{
    LILA_ZONE();

    if (!stats.valid)
        throw std::runtime_error("pattern example needs an example (settlementCreate example= or osm streets)");
    double angle = 0.0, order = 0.0;
    gridAxis(stats, &angle, &order);
    Options eo = o;
    Profile& p = eo.profile;
    const double side = stats.blockMedian > 0.0 ? std::sqrt(stats.blockMedian * kM2) : 0.5 * (p.gridWidth + p.gridDepth);
    p.gridAngle = angle;
    p.gridWidth = p.gridDepth = std::clamp(side, 2000.0, 60000.0);
    p.maxBlock = std::max(p.maxBlock, stats.blockMedian * 2.5);
    if (stats.meanSegment > 0.0)
        p.segment = std::clamp(stats.meanSegment, 800.0, 20000.0);

    const size_t before = l.graph.edges.size();
    // Ordered networks are traced along their grid; unordered ones grow, bending as much as the example's streets do.
    if (order > 0.45)
    {
        TensorSpec t;
        TensorBasis grid;
        grid.angle = angle;
        t.fields.push_back(grid);
        TensorBasis edge;
        edge.kind = "boundary";
        edge.follow = "outline";
        edge.weight = 0.3;
        edge.decay = 0.1 * l.radius;
        t.fields.push_back(edge);
        t.minorSpacing = p.gridWidth + p.widthLocal;
        t.majorSpacing = t.minorSpacing * std::max(2, p.arterialEvery);
        layTensor(l, g, eo, t, limit);
    }
    else
    {
        p.turn = 10.0 + 30.0 * (1.0 - order);
        p.spacing = std::clamp(0.6 * side, 1000.0, 20000.0);
        growOrganic(l, g, eo, limit);
    }

    // Steer the junctions towards the example's: dead ends and four-way crossings.
    std::vector<int> made;
    for (size_t e = before; e < l.graph.edges.size(); ++e)
        made.push_back(int(e));
    const double wantDead = stats.fraction[1];
    const double junctions = stats.fraction[3] + stats.fraction[4] + stats.fraction[5];
    const double wantFour = junctions > 0.0 ? (stats.fraction[4] + stats.fraction[5]) / junctions : -1.0;
    gen::Rng rng(o.seed, 12);
    for (int round = 0; round < 400; ++round)
    {
        double dead = 0.0, four = 0.0;
        std::vector<int> nodes;
        degreeShares(l.graph, made, &dead, &four, &nodes);
        if (nodes.empty())
            break;
        const int n = nodes[size_t(rng.next() * double(nodes.size())) % nodes.size()];
        const std::vector<int> inc = l.graph.incident(n);
        if (wantFour >= 0.0 && four > wantFour + 0.05 && l.graph.degree(n) >= 4)
        {
            // A crossing becomes a T: drop one arm whose far end stays a junction.
            for (int e : inc)
            {
                const StreetEdge& x = l.graph.edges[size_t(e)];
                if (!l.graph.isStreet(e) || x.cls == StreetClass::Arterial || x.path >= 0)
                    continue;
                if (l.graph.degree(x.a == n ? x.b : x.a) >= 3)
                {
                    l.graph.removeEdge(e);
                    break;
                }
            }
        }
        else if (dead < wantDead - 0.03 && l.graph.degree(n) >= 3)
        {
            // A cul-de-sac: one link cut back to a stub.
            for (int e : inc)
            {
                const StreetEdge x = l.graph.edges[size_t(e)];
                if (!l.graph.isStreet(e) || x.cls == StreetClass::Arterial || x.path >= 0)
                    continue;
                const int far = x.a == n ? x.b : x.a;
                if (l.graph.degree(far) < 3)
                    continue;
                const Vec2 a = l.graph.nodes[size_t(n)].p, b = l.graph.nodes[size_t(far)].p;
                l.graph.removeEdge(e);
                for (int m : l.graph.addStreet(a, a + (b - a) * 0.45, x))
                    made.push_back(m);
                break;
            }
        }
        else if ((wantFour < 0.0 || std::abs(four - wantFour) <= 0.05) && std::abs(dead - wantDead) <= 0.03)
            break;
    }
}
} // namespace settlement
