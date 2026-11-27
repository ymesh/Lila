// Repairs after a remix: cut roads joined again by Terrain's router (keeping their class), rivers re-solved by flow
// routing until they drain, and cut coastline re-merged along sea level.

#include "RemixWork.h"

#include "Erosion.h"
#include "Stamps.h"

#include <lila/Profile.h>

#include <algorithm>
#include <cmath>
#include <set>

namespace openworld::detail
{
namespace
{
/** Highway classes, most important first; the reconnection keeps the higher of the two ends'. */
int classRank(std::string_view cls)
{
    static const std::vector<std::string_view> order = {"motorway", "trunk", "primary", "secondary", "tertiary", "unclassified", "residential",
                                                        "service", "track", "path", "footway", "cycleway", "bridleway", "steps"};
    std::string_view base = cls;
    if (const size_t link = base.find("_link"); link != std::string_view::npos)
        base = base.substr(0, link);
    for (size_t i = 0; i < order.size(); ++i)
        if (order[i] == base)
            return int(i);
    return int(order.size());
}

/** Steepest grade the router may use for a class, percent. */
double gradeFor(std::string_view cls)
{
    const int r = classRank(cls);
    if (r <= 1)
        return 6.0;
    if (r <= 3)
        return 8.0;
    if (r <= 7)
        return 12.0;
    return 18.0;
}

std::vector<Vec2> simplify(const std::vector<Vec2>& pts, double tol)
{
    if (pts.size() < 3)
        return pts;
    std::vector<char> keep(pts.size(), 0);
    keep.front() = keep.back() = 1;
    std::vector<std::pair<size_t, size_t>> stack{{0, pts.size() - 1}};
    while (!stack.empty())
    {
        const auto [a, b] = stack.back();
        stack.pop_back();
        double worst = -1.0;
        size_t at = a;
        const Vec2 ab = pts[b] - pts[a];
        const double len = glm::length(ab);
        for (size_t i = a + 1; i < b; ++i)
        {
            const Vec2 ap = pts[i] - pts[a];
            const double d = len > 0.0 ? std::abs(ab.x * ap.y - ab.y * ap.x) / len : glm::length(ap);
            if (d > worst)
                worst = d, at = i;
        }
        if (worst > tol)
        {
            keep[at] = 1;
            stack.push_back({a, at});
            stack.push_back({at, b});
        }
    }
    std::vector<Vec2> out;
    for (size_t i = 0; i < pts.size(); ++i)
        if (keep[i])
            out.push_back(pts[i]);
    return out;
}

/** The heightfield's part around a and b, with margin. */
Heightfield cropAround(const Heightfield& h, Vec2 a, Vec2 b, double margin)
{
    const Vec2 lo = glm::min(a, b) - Vec2(margin), hi = glm::max(a, b) + Vec2(margin);
    const int x0 = std::clamp(int(std::floor((lo.x - h.origin.x) / h.cell)), 0, h.nx - 2);
    const int z0 = std::clamp(int(std::floor((lo.y - h.origin.y) / h.cell)), 0, h.nz - 2);
    const int x1 = std::clamp(int(std::ceil((hi.x - h.origin.x) / h.cell)), x0 + 1, h.nx - 1);
    const int z1 = std::clamp(int(std::ceil((hi.y - h.origin.y) / h.cell)), z0 + 1, h.nz - 1);
    Heightfield c;
    c.nx = x1 - x0 + 1;
    c.nz = z1 - z0 + 1;
    c.cell = h.cell;
    c.origin = h.samplePos(x0, z0);
    c.heights.resize(c.size());
    for (int z = 0; z < c.nz; ++z)
        for (int x = 0; x < c.nx; ++x)
            c.heights[c.index(x, z)] = h.at(x + x0, z + z0);
    return c;
}

/** A road from end node a to node b over the terrain, tagged as the way it came from; false when the router finds none. */
bool routeRoad(Work& w, Features& f, int64_t a, int64_t b, const OsmTags& tags)
{
    const Heightfield& h = w.out->after;
    const Vec2 pa = f.node(a).at, pb = f.node(b).at;
    const double dist = glm::length(pb - pa);
    if (dist < 1e-6)
    {
        Features::Way way{f.nextId++, {a, b}, tags, WayKind::Road, true};
        way.tags["remix"] = "reconnect";
        f.ways.push_back(std::move(way));
        return true;
    }
    const Heightfield crop = cropAround(h, pa, pb, std::max(0.3 * dist, 20.0 * h.cell));
    const std::string cls(tagOf(tags, "highway"));
    const terrain::Route r = terrain::route(crop, pa, pb, terrain::CostWeights{}, gradeFor(cls), std::min(512, std::max(crop.nx, crop.nz)));
    if (r.points.empty())
        return false;
    std::vector<Vec2> pts;
    for (const glm::dvec3& p : r.points)
        pts.push_back({p.x, p.z});
    pts = simplify(pts, h.cell * 0.5);
    Features::Way way{f.nextId++, {a}, tags, WayKind::Road, true};
    way.tags["remix"] = "reconnect";
    for (size_t i = 1; i + 1 < pts.size(); ++i)
        way.nodes.push_back(f.addNode(pts[i]));
    way.nodes.push_back(b);
    f.ways.push_back(std::move(way));
    return true;
}

OsmTags higherClass(const OsmTags& a, const OsmTags& b)
{
    return classRank(tagOf(a, "highway")) <= classRank(tagOf(b, "highway")) ? a : b;
}

double reachOf(const RemixSpec& s) { return s.seams.roadReach > 0.0 ? s.seams.roadReach : 4.0 * std::max(s.seams.width, 1.0); }
} // namespace

void reconnectRoads(Work& w, Features& f)
{
    LILA_ZONE();

    auto tagsOf = [&](const Features::End& e) { return f.ways[e.way].tags; };
    // Ends that were one place before the cut, however far apart they are now.
    for (const auto& [ia, ib] : f.partners)
    {
        Features::End& a = f.ends[ia];
        Features::End& b = f.ends[ib];
        if (a.kind != WayKind::Road || b.kind != WayKind::Road || a.joined || b.joined)
            continue;
        if (routeRoad(w, f, a.node, b.node, higherClass(tagsOf(a), tagsOf(b))))
        {
            a.joined = b.joined = true;
            a.how = b.how = "partner";
        }
    }
    // Then the nearest pairs of loose ends, the same class first.
    const double reach = reachOf(w.spec);
    struct Pair
    {
        double score;
        size_t a, b;
    };
    std::vector<Pair> pairs;
    for (size_t i = 0; i < f.ends.size(); ++i)
        for (size_t k = i + 1; k < f.ends.size(); ++k)
        {
            const Features::End& a = f.ends[i];
            const Features::End& b = f.ends[k];
            if (a.kind != WayKind::Road || b.kind != WayKind::Road || a.joined || b.joined || a.way == b.way)
                continue;
            const double d = glm::length(f.node(a.node).at - f.node(b.node).at);
            if (d <= reach)
                pairs.push_back({d * (a.cls == b.cls ? 1.0 : 1.5), i, k});
        }
    std::sort(pairs.begin(), pairs.end(), [](const Pair& x, const Pair& y) { return x.score < y.score; });
    for (const Pair& p : pairs)
    {
        Features::End& a = f.ends[p.a];
        Features::End& b = f.ends[p.b];
        if (a.joined || b.joined)
            continue;
        if (routeRoad(w, f, a.node, b.node, higherClass(tagsOf(a), tagsOf(b))))
        {
            a.joined = b.joined = true;
            a.how = b.how = "near";
        }
    }
    // Last, a loose end joins the nearest road it can reach.
    for (Features::End& e : f.ends)
    {
        if (e.kind != WayKind::Road || e.joined)
            continue;
        const Vec2 at = f.node(e.node).at;
        double best = reach;
        int64_t to = 0;
        for (size_t wi = 0; wi < f.ways.size(); ++wi)
        {
            const Features::Way& way = f.ways[wi];
            if (wi == e.way || way.kind != WayKind::Road)
                continue;
            for (int64_t id : way.nodes)
            {
                const double d = glm::length(f.node(id).at - at);
                if (d < best && tagOf(f.node(id).tags, "remix") != "cut")
                    best = d, to = id;
            }
        }
        if (to && routeRoad(w, f, e.node, to, f.ways[e.way].tags))
        {
            e.joined = true;
            e.how = "near";
        }
    }
}

namespace
{
bool onEdge(const Heightfield& h, size_t i)
{
    const int x = int(i % size_t(h.nx)), z = int(i / size_t(h.nx));
    return x == 0 || z == 0 || x == h.nx - 1 || z == h.nz - 1;
}

/** Water at i reaches the sea or the map's edge downhill: it is at or below sea level, on the edge, or not in a depression. */
bool drainsAt(const Heightfield& h, const terrain::Drainage& d, size_t i, double tol)
{
    return h.heights[i] <= 0.0f || onEdge(h, i) || d.filled[i] - h.heights[i] <= tol;
}

constexpr double kPitTolerance = 50.0;
} // namespace

void repairRivers(Work& w, Features& f)
{
    LILA_ZONE();

    Heightfield& h = w.out->after;
    const terrain::Drainage d = terrain::drain(h);
    // Rivers' samples, so a flow path stops where it meets another river.
    std::vector<int32_t> river(h.size(), -1);
    for (size_t wi = 0; wi < f.ways.size(); ++wi)
    {
        const Features::Way& way = f.ways[wi];
        if (way.kind != WayKind::River)
            continue;
        for (size_t k = 0; k + 1 < way.nodes.size(); ++k)
        {
            const Vec2 a = f.node(way.nodes[k]).at, b = f.node(way.nodes[k + 1]).at;
            const int n = std::max(1, int(glm::length(b - a) / (h.cell * 0.5)));
            for (int s = 0; s <= n; ++s)
                river[nearestIndex(h, a + (b - a) * (double(s) / n))] = int32_t(wi);
        }
    }
    for (size_t wi = 0; wi < f.ways.size(); ++wi)
    {
        if (f.ways[wi].kind != WayKind::River || f.ways[wi].nodes.size() < 2)
            continue;
        const Vec2 end = f.node(f.ways[wi].nodes.back()).at;
        size_t i = nearestIndex(h, end);
        if (drainsAt(h, d, i, kPitTolerance))
            continue;
        // Down the flooded surface's receivers until the sea, the edge or another river; breached so it descends.
        std::vector<size_t> path{i};
        std::set<size_t> seen{i};
        float level = h.heights[i];
        for (int guard = 0; guard < int(h.size()); ++guard)
        {
            const size_t j = d.receiver[i];
            if (j == i || seen.count(j))
                break;
            seen.insert(j);
            path.push_back(j);
            level = std::min(h.heights[j], level - 1.0f);
            h.heights[j] = level;
            i = j;
            if (h.heights[j] <= 0.0f || onEdge(h, j) || (river[j] >= 0 && size_t(river[j]) != wi))
                break;
        }
        std::vector<Vec2> pts;
        for (size_t p : path)
            pts.push_back(h.samplePos(int(p % size_t(h.nx)), int(p / size_t(h.nx))));
        pts = simplify(pts, h.cell * 0.5);
        Features::Way& way = f.ways[wi];
        RemixResult::Line added{{end}, WayKind::River, true};
        for (size_t k = 1; k < pts.size(); ++k)
        {
            way.nodes.push_back(f.addNode(pts[k], {{"remix", "flow"}}));
            added.points.push_back(pts[k]);
        }
        if (added.points.size() > 1)
            w.out->lines.push_back(std::move(added));
    }
}

void assessRivers(Work& w, Features& f)
{
    LILA_ZONE();

    const Heightfield& h = w.out->after;
    const Heightfield& before = w.out->before;
    const terrain::Drainage d = terrain::drain(h);
    const terrain::Drainage db = terrain::drain(before);
    for (size_t wi = 0; wi < f.ways.size(); ++wi)
    {
        const Features::Way& way = f.ways[wi];
        if (way.kind != WayKind::River || way.nodes.size() < 2)
            continue;
        RiverState s;
        s.way = way.id;
        s.name = std::string(tagOf(way.tags, "name"));
        s.end = f.node(way.nodes.back()).at;
        s.drains = drainsAt(h, d, nearestIndex(h, s.end), kPitTolerance);
        if (auto it = f.riverBaseEnd.find(wi); it != f.riverBaseEnd.end())
            s.drainedBefore = drainsAt(before, db, nearestIndex(before, it->second), kPitTolerance);
        for (size_t k = 1; k < way.nodes.size(); ++k)
            if (tagOf(f.node(way.nodes[k]).tags, "remix") == "flow")
                s.extended += glm::length(f.node(way.nodes[k]).at - f.node(way.nodes[k - 1]).at);
        w.out->rivers.push_back(s);
    }
}

void mergeCoast(Work& w, Features& f)
{
    LILA_ZONE();

    const Heightfield& h = w.out->after;
    const double reach = reachOf(w.spec);
    const double snap = std::max(3.0 * h.cell, w.spec.seams.width);
    auto join = [&](Features::End& a, Features::End& b, const char* how) {
        const Vec2 pa = f.node(a.node).at, pb = f.node(b.node).at;
        const double margin = std::max(snap * 2.0, 0.3 * glm::length(pb - pa));
        const std::vector<std::vector<Vec2>> lines = contourLines(h, 0.0, glm::min(pa, pb) - Vec2(margin), glm::max(pa, pb) + Vec2(margin));
        double best = 1e300;
        const std::vector<Vec2>* on = nullptr;
        size_t ia = 0, ib = 0;
        for (const std::vector<Vec2>& l : lines)
        {
            size_t ka = 0, kb = 0;
            double da = 1e300, dbb = 1e300;
            for (size_t k = 0; k < l.size(); ++k)
            {
                const double x = glm::length(l[k] - pa), y = glm::length(l[k] - pb);
                if (x < da)
                    da = x, ka = k;
                if (y < dbb)
                    dbb = y, kb = k;
            }
            if (da <= snap && dbb <= snap && da + dbb < best)
                best = da + dbb, on = &l, ia = ka, ib = kb;
        }
        if (!on)
            return false;
        std::vector<Vec2> path;
        const std::vector<Vec2>& l = *on;
        const bool closed = l.size() > 2 && glm::length(l.front() - l.back()) < 1e-6;
        if (closed)
        {
            const size_t n = l.size() - 1;
            const size_t fwd = (ib + n - ia) % n, back = (ia + n - ib) % n;
            for (size_t k = 0; k <= std::min(fwd, back); ++k)
                path.push_back(l[fwd <= back ? (ia + k) % n : (ia + n - k) % n]);
        }
        else if (ia <= ib)
            path.assign(l.begin() + std::ptrdiff_t(ia), l.begin() + std::ptrdiff_t(ib) + 1);
        else
            for (size_t k = ia + 1; k-- > ib;)
                path.push_back(l[k]);
        path = simplify(path, h.cell * 0.5);
        Features::Way way{f.nextId++, {a.node}, {{"natural", "coastline"}, {"remix", "coast"}}, WayKind::Coastline, true};
        for (const Vec2& p : path)
            way.nodes.push_back(f.addNode(p));
        way.nodes.push_back(b.node);
        f.ways.push_back(std::move(way));
        a.joined = b.joined = true;
        a.how = b.how = how;
        return true;
    };
    for (const auto& [ia, ib] : f.partners)
    {
        Features::End& a = f.ends[ia];
        Features::End& b = f.ends[ib];
        if (a.kind == WayKind::Coastline && b.kind == WayKind::Coastline && !a.joined && !b.joined)
            join(a, b, "partner");
    }
    for (size_t i = 0; i < f.ends.size(); ++i)
    {
        Features::End& a = f.ends[i];
        if (a.kind != WayKind::Coastline || a.joined || a.start)
            continue;
        // Coastline runs with land on the left: an end of one run joins the start of another.
        size_t best = SIZE_MAX;
        double bestD = reach;
        for (size_t k = 0; k < f.ends.size(); ++k)
        {
            const Features::End& b = f.ends[k];
            if (k == i || b.kind != WayKind::Coastline || b.joined || !b.start)
                continue;
            const double dd = glm::length(f.node(a.node).at - f.node(b.node).at);
            if (dd < bestD)
                bestD = dd, best = k;
        }
        if (best != SIZE_MAX)
            join(a, f.ends[best], "near");
    }
}
} // namespace openworld::detail
