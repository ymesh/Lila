// Settlement: blocks between the streets, their types, lots cut from them, landmarks, land use and envelopes.

#include "Parcels.h"

#include <lila/Profile.h>
#include <lila/Tasks.h>

#include <algorithm>
#include <cmath>
#include <limits>
#include <map>
#include <numeric>
#include <queue>
#include <span>
#include <unordered_map>

namespace settlement
{
namespace
{
constexpr uint64_t kKeyMask = (1ull << 52) - 1; // stable ids stay exact as JSON doubles

uint64_t stableKey(uint64_t h)
{
    const uint64_t k = h & kKeyMask;
    return k ? k : 1;
}

int64_t roundTo(double v, double step) { return int64_t(std::llround(v / step)); }

/** A point strictly inside the face, a hair to the left of its longest side. */
Vec2 faceInsidePoint(const Polygon& f)
{
    const double sign = signedArea(f.pts) >= 0.0 ? 1.0 : -1.0;
    size_t best = 0;
    double len = -1.0;
    for (size_t k = 0; k < f.size(); ++k)
        if (const double d = glm::length(f.pts[(k + 1) % f.size()] - f.pts[k]); d > len)
        {
            len = d;
            best = k;
        }
    const Vec2 a = f.pts[best], b = f.pts[(best + 1) % f.size()];
    const Vec2 d = (b - a) / std::max(len, 1e-12);
    return (a + b) * 0.5 + leftNormal(d) * sign * std::min(1.0, 0.01 * len);
}

double floodMargin(const Profile& p) { return std::max(2000.0, 0.5 * p.lotDepth); }

double normDist(const Layout& l, Vec2 q) { return std::clamp(glm::length(q - l.center) / std::max(l.radius, 1.0), 0.0, 1.0); }

std::string percent(double v) { return std::to_string(int(std::lround(v * 100.0))) + "%"; }
std::string metres(double cm) { return std::to_string(int(std::lround(cm / 100.0))) + " m"; }

/** Blocks: faces of the street graph inside the outline and out of exclusions, pulled in by half of each street's width. */
/** A block for each face that makes one, appended; seen counts the keys already given, so two never share one. */
void addBlocks(Layout& l, std::vector<Face> faces, std::map<uint64_t, int>& seen)
{
    const Profile& p = l.profile;
    for (Face& f : faces)
    {
        if (!l.exclusions.empty())
        {
            const Vec2 q = faceInsidePoint(f.outline);
            if (!contains(l.outline.pts, q) || excluded(l, q))
                continue;
        }
        Polygon src = f.outline;
        std::vector<double> d(src.size());
        std::vector<uint64_t> streets;
        bool water = false;
        for (size_t k = 0; k < src.size(); ++k)
        {
            const int e = src.tag[k];
            d[k] = 0.5 * edgeWidth(l, e);
            water = water || l.graph.edges[size_t(e)].cls == StreetClass::Water;
            if (l.graph.isStreet(e))
                streets.push_back(l.graph.edges[size_t(e)].street);
            else
                src.tag[k] = -1;
        }
        Polygon inner = inset(src, d);
        if (inner.pts.empty() || area(inner) < p.minLot * kM2)
            continue;
        std::sort(streets.begin(), streets.end());
        streets.erase(std::unique(streets.begin(), streets.end()), streets.end());
        const Vec2 c = centroid(inner);
        uint64_t h = hash64(0x5E771E000B10CCull);
        for (uint64_t s : streets)
            h = hash64(h ^ s);
        if (streets.empty())
            h = hash64(h ^ uint64_t(roundTo(c.x, 5000.0) * 73856093) ^ uint64_t(roundTo(c.y, 5000.0) * 19349663));
        // Two faces between the same streets (a loop cut by a footpath): told apart by where they are.
        if (int& n = seen[h]; n++ > 0)
            h = hash64(h ^ uint64_t(roundTo(c.x, 1000.0) * 83492791) ^ uint64_t(roundTo(c.y, 1000.0) * 2654435761u) ^ uint64_t(n));
        Block b;
        b.id = int(l.blocks.size());
        b.key = stableKey(h);
        b.area = area(inner) / kM2;
        b.outline = std::move(inner);
        b.water = water;
        l.blocks.push_back(std::move(b));
    }
}

void makeBlocks(Layout& l)
{
    std::map<uint64_t, int> seen;
    addBlocks(l, planarFaces(l.graph), seen);
}

/** The junction most main roads meet at near the centre, and how many arterials meet there. */
int primaryJunction(const Layout& l, int* arterials)
{
    int best = -1, bestCount = 0;
    double bestDist = 0.0;
    for (size_t n = 0; n < l.graph.nodes.size(); ++n)
    {
        int count = 0;
        for (int e : l.graph.incident(int(n)))
            if (l.graph.isStreet(e) && l.graph.edges[size_t(e)].cls == StreetClass::Arterial)
                ++count;
        const double dist = glm::length(l.graph.nodes[n].p - l.center);
        if (count < 2 || dist > 0.5 * l.radius)
            continue;
        if (count > bestCount || (count == bestCount && dist < bestDist))
        {
            best = int(n);
            bestCount = count;
            bestDist = dist;
        }
    }
    if (arterials)
        *arterials = bestCount;
    return best;
}

int streetSides(const Polygon& p)
{
    int n = 0;
    for (int t : p.tag)
        n += t >= 0 ? 1 : 0;
    return n;
}

/** Types and lot methods: special ground first (no street, the market, flood, slope, fields), then by era and shape. */
void typeBlocks(Layout& l, const Ground& g, const Site& site, const std::string& era, size_t firstFresh = 0)
{
    LILA_ZONE();

    const Profile& p = l.profile;
    const bool pm = preModern(era);
    int market = -1;
    std::string marketWhy;
    if (p.plaza)
    {
        for (const Block& b : l.blocks)
            if (contains(b.outline.pts, l.center) && streetSides(b.outline) > 0)
            {
                market = b.id;
                marketWhy = "at the centre";
            }
        int arterials = 0;
        const int junction = primaryJunction(l, &arterials);
        if (market < 0 && junction >= 0)
        {
            double best = std::numeric_limits<double>::max();
            for (const Block& b : l.blocks)
                if (streetSides(b.outline) > 0)
                    if (const double d = boundaryDistance(b.outline.pts, l.graph.nodes[size_t(junction)].p); d < best)
                    {
                        best = d;
                        market = b.id;
                    }
        }
        if (market >= 0 && junction >= 0 && boundaryDistance(l.blocks[size_t(market)].outline.pts, l.graph.nodes[size_t(junction)].p) < p.widthArterial * 2.0)
            marketWhy += std::string(marketWhy.empty() ? "" : ", ") + "where " + std::to_string(arterials) + " main roads meet";
    }
    const double fieldStart = p.density < 0.5 ? 0.55 : 0.8;
    BlockType profileType = BlockType::Perimeter;
    const bool fixedType = p.blockType != "auto" && parseBlockType(p.blockType, &profileType);
    LotMethod profileMethod = LotMethod::Frontage;
    const bool fixedMethod = p.lotMethod != "auto" && parseLotMethod(p.lotMethod, &profileMethod);

    for (Block& b : std::span(l.blocks).subspan(std::min(firstFresh, l.blocks.size())))
    {
        const Vec2 c = centroid(b.outline);
        const double dist = normDist(l, c);
        Vec2 bc{0.0}, u{1.0, 0.0}, v{0.0, 1.0};
        double hu = 0.0, hv = 0.0;
        boxOf(b.outline, bc, u, v, hu, hv);
        std::vector<Vec2> samples = {c};
        for (size_t k = 0; k < b.outline.size(); k += std::max<size_t>(1, b.outline.size() / 8))
            samples.push_back((b.outline.pts[k] + c) * 0.5);
        double grade = 0.0, flood = 0.0;
        for (const Vec2& q : samples)
        {
            grade += g.grade(q);
            flood += bankDistance(q, l.water) < floodMargin(p) ? 1.0 : 0.0;
        }
        grade /= double(samples.size());
        flood /= double(samples.size());
        bool primary = false;
        for (int t : b.outline.tag)
            primary = primary || (t >= 0 && l.graph.edges[size_t(t)].cls <= StreetClass::Collector);

        bool ordinary = false;
        if (streetSides(b.outline) == 0)
        {
            b.type = BlockType::Park;
            b.reasons.push_back("no street reaches it");
        }
        else if (b.id == market)
        {
            b.type = pm ? BlockType::Market : BlockType::Plaza;
            b.reasons.push_back(marketWhy.empty() ? "the block nearest the main junction" : marketWhy);
        }
        else if (flood > 0.5)
        {
            b.type = BlockType::Park;
            b.reasons.push_back("on flood ground within " + metres(floodMargin(p)) + " of the water");
        }
        else if (grade > steepLimit(p, 2.0))
        {
            b.type = BlockType::Park;
            b.reasons.push_back("slope " + percent(grade) + ", past the style's steepest " + percent(steepLimit(p, 2.0)));
        }
        else if (era == "medieval" && dist > fieldStart && b.area > 3.0 * p.lotWidth * p.lotDepth / kM2)
        {
            b.type = BlockType::Field;
            b.reasons.push_back("outside the town (" + percent(dist) + " of the radius): open fields");
        }
        else
        {
            ordinary = true;
            if (grade > 2.0 * p.maxGrade)
                b.reasons.push_back("terraced: slope " + percent(grade) + ", past twice the style's " + percent(p.maxGrade));
            if (fixedType)
                b.type = profileType;
            else if (era == "medieval")
                b.type = primary || 2.0 * hv >= 1.6 * p.lotDepth ? BlockType::Perimeter : BlockType::Row;
            else if (era == "early")
                b.type = 2.0 * hv > 2.2 * p.lotDepth ? BlockType::Courtyard : BlockType::Perimeter;
            else if (era == "industrial")
                b.type = dist < p.commercial ? BlockType::Row : BlockType::Open;
            else if (era == "modern")
                b.type = b.area > 2.0 * p.maxBlock ? BlockType::Superblock : dist < p.commercial ? BlockType::Perimeter : BlockType::Open;
            else
                b.type = b.area > p.maxBlock ? BlockType::Superblock : BlockType::Open;
        }
        switch (b.type)
        {
        case BlockType::Perimeter:
            b.method = pm ? (primary ? LotMethod::Burgage : p.pattern == Pattern::Organic ? LotMethod::Voronoi : LotMethod::Frontage)
                          : LotMethod::Skeleton;
            break;
        case BlockType::Courtyard: b.method = LotMethod::Skeleton; break;
        case BlockType::Row: b.method = LotMethod::Frontage; break;
        case BlockType::Open:
        case BlockType::Superblock: b.method = LotMethod::Obb; break;
        case BlockType::Field: b.method = LotMethod::Strips; break;
        case BlockType::Park:
        case BlockType::Plaza:
        case BlockType::Market:
        case BlockType::Churchyard: b.method = LotMethod::Whole; break;
        }
        if (ordinary && fixedMethod)
            b.method = profileMethod;
    }
    for (const BlockOverride& o : site.blockOverrides)
    {
        auto it = std::ranges::find_if(l.blocks, [&](const Block& b) { return b.key == o.block; });
        if (it != l.blocks.end() && size_t(it->id) < firstFresh)
            continue;
        if (it == l.blocks.end())
        {
            l.parcelWarnings.push_back("block override " + std::to_string(o.block) + " names a block this layout does not make");
            continue;
        }
        if (o.hasType)
        {
            it->type = o.type;
            it->reasons.push_back("set by hand");
            if (!o.hasMethod)
                it->method = o.type == BlockType::Park || o.type == BlockType::Plaza || o.type == BlockType::Market ||
                                     o.type == BlockType::Churchyard ? LotMethod::Whole
                             : o.type == BlockType::Field                                      ? LotMethod::Strips
                             : o.type == BlockType::Open || o.type == BlockType::Superblock    ? LotMethod::Obb
                             : o.type == BlockType::Courtyard                                  ? LotMethod::Skeleton
                                                                                               : LotMethod::Frontage;
        }
        if (o.hasMethod)
            it->method = o.method;
    }
    for (Block& b : l.blocks)
        b.use = b.type == BlockType::Market || b.type == BlockType::Plaza ? Use::Plaza
                : b.type == BlockType::Park                               ? Use::Park
                : b.type == BlockType::Field                              ? Use::Farm
                                                                          : Use::Residential;
}

/** Street sides, corner, the best street's class and how central the streets it faces are. Tags are still edge ids. */
void finishLot(Lot& lot, const Layout& l)
{
    int best = int(StreetClass::Boundary);
    std::vector<Vec2> fronts;
    lot.frontages = 0;
    lot.corner = false;
    lot.access = 0.0;
    for (size_t k = 0; k < lot.outline.size(); ++k)
    {
        const int e = lot.outline.tag[k];
        const Vec2 d = lot.outline.pts[(k + 1) % lot.outline.size()] - lot.outline.pts[k];
        if (e < 0 || glm::length(d) < 100.0)
            continue;
        ++lot.frontages;
        best = std::min(best, int(l.graph.edges[size_t(e)].cls));
        if (size_t(e) < l.edgeAccess.size())
            lot.access = std::max(lot.access, l.edgeAccess[size_t(e)]);
        const Vec2 u = glm::normalize(d);
        for (const Vec2& f : fronts)
            if (std::abs(glm::dot(f, u)) < 0.5)
                lot.corner = true;
        fronts.push_back(u);
    }
    lot.frontClass = StreetClass(std::min(best, int(StreetClass::Alley)));
}

bool buildable(const Lot& lot, const Profile& p)
{
    if (!lot.landmark.empty() || lot.frontages == 0 || lot.area < p.minLot)
        return false;
    double x0 = 1e300, x1 = -1e300, z0 = 1e300, z1 = -1e300;
    for (const Vec2& q : lot.outline.pts)
        x0 = std::min(x0, q.x), x1 = std::max(x1, q.x), z0 = std::min(z0, q.y), z1 = std::max(z1, q.y);
    return lot.area * kM2 / std::max((x1 - x0) * (z1 - z0), 1e-9) >= 0.3;
}

/** Lots across a street from the polygon (within the widest street and a little). */
bool facing(const Lot& lot, const Polygon& square, double reach)
{
    for (size_t k = 0; k < lot.outline.size(); ++k)
    {
        const Vec2 a = lot.outline.pts[k], b = lot.outline.pts[(k + 1) % lot.outline.size()];
        if (lot.outline.tag[k] >= 0 && segmentDistance(square.pts, a, b) < reach)
            return true;
    }
    return false;
}

void mark(Lot& lot, const std::string& name, Use use, std::vector<std::string> reasons)
{
    lot.landmark = name;
    lot.use = use;
    lot.reasons = std::move(reasons);
}

/** Straight runs of main streets long enough to read as an axis: edges, end nodes, length. */
struct Axis
{
    int a = -1, b = -1;
    double length = 0.0;
    uint64_t street = 0;
};

std::vector<Axis> axes(const Layout& l, double minLength)
{
    const StreetGraph& g = l.graph;
    std::vector<char> used(g.edges.size(), 0);
    std::vector<Axis> out;
    auto main = [&](int e) { return g.isStreet(e) && g.edges[size_t(e)].cls <= StreetClass::Collector; };
    auto other = [&](int e, int n) { return g.edges[size_t(e)].a == n ? g.edges[size_t(e)].b : g.edges[size_t(e)].a; };
    for (size_t e0 = 0; e0 < g.edges.size(); ++e0)
    {
        if (used[e0] || !main(int(e0)))
            continue;
        used[e0] = 1;
        int ends[2] = {g.edges[e0].a, g.edges[e0].b};
        double length = g.length(int(e0));
        for (int side = 0; side < 2; ++side)
        {
            int at = ends[side], from = int(e0);
            for (int guard = 0; guard < 10000; ++guard)
            {
                if (g.degree(at) != 2)
                    break;
                int next = -1;
                for (int e : g.incident(at))
                    if (e != from && g.isStreet(e))
                        next = e;
                if (next < 0 || used[size_t(next)] || !main(next))
                    break;
                const Vec2 d0 = glm::normalize(g.nodes[size_t(at)].p - g.nodes[size_t(other(from, at))].p);
                const Vec2 d1 = glm::normalize(g.nodes[size_t(other(next, at))].p - g.nodes[size_t(at)].p);
                if (glm::dot(d0, d1) < std::cos(10.0 * kPi / 180.0))
                    break;
                used[size_t(next)] = 1;
                length += g.length(next);
                from = next;
                at = other(next, at);
            }
            ends[side] = at;
        }
        const double span = glm::length(g.nodes[size_t(ends[1])].p - g.nodes[size_t(ends[0])].p);
        if (length >= minLength && span > 0.97 * length)
            out.push_back({ends[0], ends[1], length, g.edges[e0].street});
    }
    std::sort(out.begin(), out.end(), [](const Axis& x, const Axis& y) { return x.length > y.length; });
    return out;
}

/** The market square, church, town hall, castle or temple on high ground, axis ends and parks. */
void placeLandmarks(Layout& l, const Ground& g, const std::string& era)
{
    LILA_ZONE();

    const Profile& p = l.profile;
    const bool pm = preModern(era);
    const double reach = std::max(p.widthArterial, p.widthCollector) + 200.0;
    const Block* square = nullptr;
    int parks = 0;
    for (Lot& lot : l.lots)
    {
        const Block& b = l.blocks[size_t(lot.block)];
        if (b.type == BlockType::Market || b.type == BlockType::Plaza)
        {
            mark(lot, "market_square", Use::Plaza, b.reasons);
            square = &b;
        }
        else if (b.type == BlockType::Park)
            mark(lot, "park_" + std::to_string(++parks), Use::Park, b.reasons);
    }

    auto largestFacing = [&](const char* name, Use use, std::vector<std::string> reasons) -> Lot* {
        Lot* best = nullptr;
        for (Lot& lot : l.lots)
            if (square && lot.block != square->id && buildable(lot, p) && lot.area >= 2.0 * p.minLot && facing(lot, square->outline, reach))
                if (!best || lot.area > best->area)
                    best = &lot;
        if (best)
        {
            reasons.push_back("fronts market_square");
            reasons.push_back("the largest lot facing it, " + std::to_string(int(std::lround(best->area))) + " m2");
            mark(*best, name, use, std::move(reasons));
        }
        return best;
    };
    auto mostCentral = [&](const char* name, Use use, double within) -> Lot* {
        Lot* best = nullptr;
        for (Lot& lot : l.lots)
            if (buildable(lot, p) && lot.area >= 2.0 * p.minLot && normDist(l, centroid(lot.outline)) < within)
                if (!best || lot.access > best->access)
                    best = &lot;
        if (best)
            mark(*best, name, use, {"on the most central street within " + percent(within) + " of the radius from the centre",
                                    "street access " + std::to_string(int(std::lround(best->access * 100.0))) + "/100"});
        return best;
    };

    if (era != "modern" && era != "future")
    {
        Lot* church = largestFacing("church", Use::Religious, {});
        if (!church)
            church = mostCentral("church", Use::Religious, 0.3);
        if (church && pm)
        {
            Block& b = l.blocks[size_t(church->block)];
            if (b.area < 6.0 * church->area && b.type != BlockType::Field)
            {
                b.type = BlockType::Churchyard;
                b.use = Use::Religious;
                b.reasons.push_back("holds the church and its graveyard");
                for (Lot& lot : l.lots)
                    if (lot.block == b.id && lot.landmark.empty())
                        lot.use = Use::Cemetery;
            }
        }
    }
    const char* hall = era == "modern" || era == "future" ? "city_hall" : "town_hall";
    if (!largestFacing(hall, Use::Civic, {}))
        mostCentral(hall, Use::Civic, 0.3);

    if (!g.isPlane())
    {
        const double r = std::max(5000.0, 0.15 * l.radius);
        Lot* best = nullptr;
        double bestRise = 0.0;
        for (Lot& lot : l.lots)
        {
            // Steep hills are parks first; a castle may take one.
            const bool park = lot.landmark.rfind("park_", 0) == 0;
            if (!park && (!buildable(lot, p) || lot.area < 2.0 * p.minLot))
                continue;
            Vec2 c = centroid(lot.outline);
            for (const Vec2& q : lot.outline.pts)
                if (g.height(q) > g.height(c))
                    c = q;
            if (normDist(l, c) < 0.15)
                continue;
            double sum = 0.0;
            int n = 0;
            for (int k = 0; k < 12; ++k)
            {
                const Vec2 q = c + Vec2(std::cos(k * kPi / 6.0), std::sin(k * kPi / 6.0)) * r;
                if (contains(l.outline.pts, q))
                {
                    sum += g.height(q);
                    ++n;
                }
            }
            if (n < 6)
                continue;
            if (const double rise = g.height(c) - sum / n; rise > bestRise)
            {
                bestRise = rise;
                best = &lot;
            }
        }
        if (best && bestRise >= 500.0)
        {
            std::vector<std::string> why = {"on high ground " + metres(bestRise) + " above the ground " + metres(r) + " around it",
                                            "the most prominent lot past 15% of the radius"};
            for (const std::string& w : best->reasons)
                why.push_back(w);
            mark(*best, pm ? "castle" : "temple", pm ? Use::Civic : Use::Religious, std::move(why));
        }
    }

    int ends = 0;
    for (const Axis& ax : axes(l, std::max(0.2 * l.radius, 15000.0)))
    {
        for (int side = 0; side < 2 && ends < 6; ++side)
        {
            const int n = side == 0 ? ax.a : ax.b, m = side == 0 ? ax.b : ax.a;
            if (l.graph.degree(n) == 2)
                continue;
            const Vec2 end = l.graph.nodes[size_t(n)].p, d = glm::normalize(end - l.graph.nodes[size_t(m)].p);
            Lot* best = nullptr;
            double bestAlong = std::numeric_limits<double>::max();
            for (Lot& lot : l.lots)
            {
                const Vec2 c = centroid(lot.outline);
                const double along = glm::dot(c - end, d), side2 = std::abs(cross(d, c - end));
                if (!buildable(lot, p) || along <= 0.0 || along > p.widthArterial + 1.5 * p.lotDepth || side2 > 0.75 * p.lotWidth + 500.0)
                    continue;
                if (along < bestAlong)
                {
                    bestAlong = along;
                    best = &lot;
                }
            }
            if (best)
                mark(*best, "axis_end_" + std::to_string(++ends), pm ? Use::Religious : Use::Civic,
                     {"terminates the " + metres(ax.length) + " straight run of street " + std::to_string(ax.street)});
        }
    }
}

/** Where a river carries things: 0 at its upstream end to 1 downstream, by the nearest point (lines run downstream). */
double downstream(Vec2 q, std::span<const WaterLine> water)
{
    double best = std::numeric_limits<double>::max(), at = 0.0;
    for (const WaterLine& w : water)
    {
        double total = 0.0;
        for (size_t k = 0; k + 1 < w.pts.size(); ++k)
            total += glm::length(w.pts[k + 1] - w.pts[k]);
        double run = 0.0;
        for (size_t k = 0; k + 1 < w.pts.size(); ++k)
        {
            const Vec2 a = w.pts[k], d = w.pts[k + 1] - a;
            const double len = glm::length(d);
            const double t = std::clamp(glm::dot(q - a, d) / std::max(len * len, 1e-9), 0.0, 1.0);
            if (const double dist = glm::length(a + d * t - q); dist < best)
            {
                best = dist;
                at = total > 0.0 ? (run + t * len) / total : 0.0;
            }
            run += len;
        }
    }
    return at;
}

/** Uses from the rules: flood and slope, port, industry downwind and downstream, market, commerce by access, residential tiers. */
uint64_t useCellKey(int64_t x, int64_t y) { return (uint64_t(x) * 0x9E3779B97F4A7C15ull) ^ uint64_t(y); }
uint64_t useCellKey(Vec2 p, double size) { return useCellKey(int64_t(std::floor(p.x / size)), int64_t(std::floor(p.y / size))); }

/**
 * Uses and wealth by ranking lots across the town. With only, just those lots are set, against the cuts the last full
 * ranking recorded.
 */
void assignUses(Layout& l, const Ground& g, const std::string& era, const std::vector<char>* only = nullptr)
{
    LILA_ZONE();

    const Profile& p = l.profile;
    const bool pm = preModern(era);
    const Block* square = nullptr;
    for (const Block& b : l.blocks)
        if (b.type == BlockType::Market || b.type == BlockType::Plaza)
            square = &b;
    const double reach = std::max(p.widthArterial, p.widthCollector) + 200.0;
    const double angle = p.windAngle * kPi / 180.0;
    const Vec2 wind(std::cos(angle), std::sin(angle));
    std::vector<char> isOpen(l.lots.size(), 0);
    std::vector<Vec2> centre(l.lots.size());
    tbb::parallel_for(tbb::blocked_range<size_t>(0, l.lots.size()), [&](const tbb::blocked_range<size_t>& range) {
        for (size_t k = range.begin(); k < range.end(); ++k)
            centre[k] = centroid(l.lots[k].outline);
    });
    tbb::parallel_for(tbb::blocked_range<size_t>(0, l.lots.size()), [&](const tbb::blocked_range<size_t>& range) {
        for (size_t k = range.begin(); k < range.end(); ++k)
        {
            Lot& lot = l.lots[k];
            if ((only && !(*only)[k]) || !lot.landmark.empty() || lot.use == Use::Cemetery)
                continue;
            const Block& b = l.blocks[size_t(lot.block)];
            const Vec2 c = centre[size_t(lot.id)];
            const double bank = bankDistance(c, l.water);
            if (b.type == BlockType::Field)
                lot.use = Use::Farm;
            else if (!buildable(lot, p))
                lot.use = Use::Open;
            else if (bank < floodMargin(p) || g.grade(c) > steepLimit(p, 2.0))
                lot.use = Use::Park;
            else if (b.water && bank < 1.5 * p.lotDepth && normDist(l, c) < 0.85 && era != "future")
                lot.use = Use::Port;
            else
            {
                lot.use = Use::Residential;
                isOpen[k] = 1;
            }
        }
    });
    std::vector<int> open;
    for (size_t k = 0; k < l.lots.size(); ++k)
        if (isOpen[k])
            open.push_back(l.lots[k].id);

    const double share = era == "medieval" ? 0.03 : era == "early" ? 0.05 : era == "industrial" ? 0.15 : era == "modern" ? 0.10 : 0.06;
    std::vector<double> scores(open.size(), std::numeric_limits<double>::quiet_NaN());
    tbb::parallel_for(tbb::blocked_range<size_t>(0, open.size()), [&](const tbb::blocked_range<size_t>& range) {
        for (size_t k = range.begin(); k < range.end(); ++k)
        {
            const Lot& lot = l.lots[size_t(open[k])];
            const Vec2 c = centre[size_t(lot.id)];
            if (normDist(l, c) < 0.3)
                continue;
            const Vec2 out = glm::length(c - l.center) > 1e-6 ? glm::normalize(c - l.center) : Vec2(0.0);
            const double bank = bankDistance(c, l.water);
            scores[k] = 0.45 * 0.5 * (1.0 + glm::dot(out, wind)) + 0.25 * (bank < 4.0 * p.lotDepth ? downstream(c, l.water) : 0.0) +
                        0.2 * (bank < 2.0 * p.lotDepth ? 1.0 : 0.0) + 0.1 * lot.access;
        }
    });
    Layout::UseCuts& cuts = l.useCuts;
    if (only)
    {
        for (size_t k = 0; k < open.size(); ++k)
            if (!std::isnan(scores[k]) && scores[k] >= cuts.industry)
                l.lots[size_t(open[k])].use = Use::Industrial;
    }
    else
    {
        std::vector<std::pair<double, int>> industry;
        for (size_t k = 0; k < open.size(); ++k)
            if (!std::isnan(scores[k]))
                industry.push_back({-scores[k], open[k]});
        std::sort(industry.begin(), industry.end());
        const size_t nIndustry = std::min(industry.size(), size_t(std::lround(share * double(open.size()))));
        for (size_t k = 0; k < nIndustry; ++k)
            l.lots[size_t(industry[k].second)].use = Use::Industrial;
        cuts.industry = nIndustry > 0 ? -industry[nIndustry - 1].first : 1e300;

        std::vector<double> access;
        for (int id : open)
            access.push_back(l.lots[size_t(id)].access);
        std::sort(access.begin(), access.end());
        auto quantile = [&](double q) { return access.empty() ? 1.0 : access[std::min(access.size() - 1, size_t(q * double(access.size())))]; };
        cuts.q60 = quantile(0.6), cuts.q92 = quantile(0.92);
    }
    const double q60 = cuts.q60, q92 = cuts.q92;
    for (int id : open)
    {
        Lot& lot = l.lots[size_t(id)];
        if (lot.use != Use::Residential)
            continue;
        const double dist = normDist(l, centre[size_t(lot.id)]);
        if (pm && square && facing(lot, square->outline, reach))
            lot.use = Use::Market;
        else if (pm ? (lot.frontClass <= StreetClass::Collector && dist < p.commercial)
                    : ((lot.access >= q60 && dist < p.commercial) || lot.access >= q92))
            lot.use = Use::Commercial;
    }

    std::vector<Vec2> amenities, works;
    for (const Lot& lot : l.lots)
    {
        if (!lot.landmark.empty() || lot.use == Use::Park)
            amenities.push_back(centre[size_t(lot.id)]);
        if (lot.use == Use::Industrial)
            works.push_back(centre[size_t(lot.id)]);
    }
    // Points binned at the query radius, so a lookup checks the 3x3 cells around it.
    auto grid = [](const std::vector<Vec2>& pts, double r) {
        std::unordered_map<uint64_t, std::vector<Vec2>> g;
        for (Vec2 x : pts)
            g[useCellKey(x, r)].push_back(x);
        return g;
    };
    auto near = [](const std::unordered_map<uint64_t, std::vector<Vec2>>& g, Vec2 q, double r) {
        const int64_t cx = int64_t(std::floor(q.x / r)), cy = int64_t(std::floor(q.y / r));
        for (int64_t dx = -1; dx <= 1; ++dx)
            for (int64_t dy = -1; dy <= 1; ++dy)
                if (auto it = g.find(useCellKey(cx + dx, cy + dy)); it != g.end())
                    for (Vec2 x : it->second)
                        if (glm::length(x - q) < r)
                            return true;
        return false;
    };
    const double amenityReach = 0.12 * l.radius, worksReach = 0.1 * l.radius;
    const auto amenityGrid = grid(amenities, amenityReach), worksGrid = grid(works, worksReach);
    std::vector<std::pair<double, int>> values;
    for (int id : open)
    {
        const Lot& lot = l.lots[size_t(id)];
        if (lot.use != Use::Residential)
            continue;
        const Vec2 c = centre[size_t(lot.id)];
        const double dist = normDist(l, c);
        // Old towns are richest at the centre; industrial and later ones in the suburbs.
        const double place = pm ? 1.0 - dist : 0.4 + 0.6 * dist;
        const double value = 0.45 * lot.access + 0.35 * place + 0.2 * (near(amenityGrid, c, amenityReach) ? 1.0 : 0.0) -
                             0.35 * (near(worksGrid, c, worksReach) ? 1.0 : 0.0);
        values.push_back({value, id});
    }
    if (only)
    {
        for (const auto& [value, id] : values)
            l.lots[size_t(id)].wealth = value < cuts.wealth1 ? 0 : value < cuts.wealth2 ? 1 : 2;
        return;
    }
    std::sort(values.begin(), values.end());
    for (size_t k = 0; k < values.size(); ++k)
        l.lots[size_t(values[k].second)].wealth = int(3 * k / std::max<size_t>(values.size(), 1));
    cuts.wealth1 = values.empty() ? 0.0 : values[(values.size() + 2) / 3 < values.size() ? (values.size() + 2) / 3 : values.size() - 1].first;
    cuts.wealth2 = values.empty() ? 0.0 : values[std::min(values.size() - 1, (2 * values.size() + 2) / 3)].first;
    cuts.set = true;

    if (!pm && open.size() > 50)
    {
        Lot* best = nullptr;
        for (int id : open)
        {
            Lot& lot = l.lots[size_t(id)];
            if (lot.use == Use::Residential && normDist(l, centre[size_t(lot.id)]) > 0.6 && (!best || lot.area > best->area))
                best = &lot;
        }
        if (best)
            mark(*best, "cemetery", Use::Cemetery, {"at the edge of town: the largest lot past 60% of the radius"});
    }
}

void applyLotOverrides(Layout& l, const Site& site)
{
    for (const LotOverride& o : site.lotOverrides)
    {
        auto it = std::ranges::find_if(l.lots, [&](const Lot& lot) { return lot.key == o.lot; });
        if (it == l.lots.end())
        {
            l.parcelWarnings.push_back("lot override " + std::to_string(o.lot) + " names a lot this layout does not make");
            continue;
        }
        if (o.hasUse)
            it->use = Use(o.use);
        if (!o.landmark.empty())
        {
            it->landmark = o.landmark;
            it->reasons = {"placed by hand"};
        }
    }
}

BuildingRoute routeAt(const Site& site, Vec2 q)
{
    BuildingRoute r = BuildingRoute::Outline;
    for (const RouteRegion& region : site.buildingRoutes)
        if (region.polygon.size() < 3 || contains(region.polygon, q))
            r = region.route;
    return r;
}

bool getsEnvelope(Use u)
{
    switch (u)
    {
    case Use::Open:
    case Use::Plaza:
    case Use::Park:
    case Use::Farm:
    case Use::Cemetery: return false;
    default: return true;
    }
}
} // namespace

std::vector<double> streetAccess(const StreetGraph& g)
{
    LILA_ZONE();

    const size_t nNodes = g.nodes.size(), nEdges = g.edges.size();
    std::vector<double> out(nEdges, 0.0);
    std::vector<int> nodes;
    for (size_t n = 0; n < nNodes; ++n)
        if (g.degree(int(n)) > 0)
            nodes.push_back(int(n));
    if (nodes.size() < 2)
        return out;
    const size_t stride = std::max<size_t>(1, nodes.size() / 256);
    std::vector<double> sumDist(nNodes, 0.0), between(nEdges, 0.0);
    std::vector<int> reached(nNodes, 0);
    std::vector<double> dist(nNodes), sigma(nNodes), delta(nNodes);
    std::vector<std::vector<std::pair<int, int>>> preds(nNodes);
    std::vector<int> order;
    for (size_t s = 0; s < nodes.size(); s += stride)
    {
        std::fill(dist.begin(), dist.end(), std::numeric_limits<double>::infinity());
        std::fill(sigma.begin(), sigma.end(), 0.0);
        std::fill(delta.begin(), delta.end(), 0.0);
        for (auto& p : preds)
            p.clear();
        order.clear();
        using Item = std::pair<double, int>;
        std::priority_queue<Item, std::vector<Item>, std::greater<>> queue;
        const int src = nodes[s];
        dist[size_t(src)] = 0.0;
        sigma[size_t(src)] = 1.0;
        queue.push({0.0, src});
        while (!queue.empty())
        {
            const auto [d, n] = queue.top();
            queue.pop();
            if (d > dist[size_t(n)])
                continue;
            order.push_back(n);
            for (int e : g.incident(n))
            {
                if (!g.isStreet(e))
                    continue;
                const int m = g.edges[size_t(e)].a == n ? g.edges[size_t(e)].b : g.edges[size_t(e)].a;
                const double nd = d + g.length(e);
                if (nd < dist[size_t(m)] - 1e-6)
                {
                    dist[size_t(m)] = nd;
                    sigma[size_t(m)] = sigma[size_t(n)];
                    preds[size_t(m)] = {{n, e}};
                    queue.push({nd, m});
                }
                else if (std::abs(nd - dist[size_t(m)]) <= 1e-6)
                {
                    sigma[size_t(m)] += sigma[size_t(n)];
                    preds[size_t(m)].push_back({n, e});
                }
            }
        }
        for (int n : order)
        {
            sumDist[size_t(n)] += dist[size_t(n)];
            ++reached[size_t(n)];
        }
        for (auto it = order.rbegin(); it != order.rend(); ++it)
            for (const auto& [v, e] : preds[size_t(*it)])
            {
                const double c = sigma[size_t(v)] / std::max(sigma[size_t(*it)], 1e-12) * (1.0 + delta[size_t(*it)]);
                between[size_t(e)] += c;
                delta[size_t(v)] += c;
            }
    }
    std::vector<double> close(nNodes, 0.0);
    double maxClose = 0.0, maxBetween = 0.0;
    for (int n : nodes)
    {
        close[size_t(n)] = sumDist[size_t(n)] > 0.0 ? double(reached[size_t(n)]) / sumDist[size_t(n)] : 0.0;
        maxClose = std::max(maxClose, close[size_t(n)]);
    }
    for (size_t e = 0; e < nEdges; ++e)
        if (g.isStreet(int(e)))
            maxBetween = std::max(maxBetween, between[e]);
    for (size_t e = 0; e < nEdges; ++e)
    {
        if (!g.isStreet(int(e)))
            continue;
        const double c = 0.5 * (close[size_t(g.edges[e].a)] + close[size_t(g.edges[e].b)]) / std::max(maxClose, 1e-12);
        const double b = std::sqrt(between[e] / std::max(maxBetween, 1e-12));
        out[e] = std::clamp(0.5 * c + 0.5 * b, 0.0, 1.0);
    }
    return out;
}

/**
 * Lots from index first on that share area with a lot of another block are left out, the smaller of two new ones, and
 * said so in the warnings: where two blocks' insets meet at a sharp junction, their lots can reach into each other.
 * Lots before first are never removed, so their ids stay. Returns how many went.
 */
size_t dropOverlappingLots(Layout& l, size_t first, std::vector<char>* fresh)
{
    if (first >= l.lots.size())
        return 0;
    auto bounds = [](const Polygon& p) {
        Vec2 lo(std::numeric_limits<double>::max()), hi(-std::numeric_limits<double>::max());
        for (const Vec2& q : p.pts)
            lo = glm::min(lo, q), hi = glm::max(hi, q);
        return std::pair{lo, hi};
    };
    Vec2 all0(std::numeric_limits<double>::max()), all1(-std::numeric_limits<double>::max());
    for (size_t k = first; k < l.lots.size(); ++k)
    {
        const auto [lo, hi] = bounds(l.lots[k].outline);
        all0 = glm::min(all0, lo), all1 = glm::max(all1, hi);
    }
    const double cell = std::max(2000.0, l.profile.lotDepth);
    std::unordered_map<int64_t, std::vector<size_t>> bins;
    auto key = [](int x, int y) { return (int64_t(x) << 32) ^ int64_t(uint32_t(y)); };
    for (size_t k = 0; k < l.lots.size(); ++k)
    {
        const auto [lo, hi] = bounds(l.lots[k].outline);
        if (hi.x < all0.x || lo.x > all1.x || hi.y < all0.y || lo.y > all1.y)
            continue;
        for (int y = int(std::floor(lo.y / cell)); y <= int(std::floor(hi.y / cell)); ++y)
            for (int x = int(std::floor(lo.x / cell)); x <= int(std::floor(hi.x / cell)); ++x)
                bins[key(x, y)].push_back(k);
    }
    std::vector<char> drop(l.lots.size(), 0);
    for (const auto& [k, list] : bins)
        for (size_t m = 0; m < list.size(); ++m)
            for (size_t n = m + 1; n < list.size(); ++n)
            {
                size_t a = std::min(list[m], list[n]), b = std::max(list[m], list[n]);
                if (b < first || drop[a] || drop[b] || l.lots[a].block == l.lots[b].block)
                    continue;
                if (l.lots[a].outline.pts.size() < 3 || l.lots[b].outline.pts.size() < 3 || !polygonsShareArea(l.lots[a].outline, l.lots[b].outline, 2.0))
                    continue;
                drop[a >= first && l.lots[a].area < l.lots[b].area ? a : b] = 1;
            }
    size_t gone = 0, w = first;
    for (size_t k = first; k < l.lots.size(); ++k)
    {
        if (drop[k])
        {
            l.parcelWarnings.push_back("lot of block " + std::to_string(l.blocks[size_t(l.lots[k].block)].key) +
                                       " overlapped the next block's lots at a sharp junction and was left open");
            ++gone;
            continue;
        }
        if (w != k)
        {
            l.lots[w] = std::move(l.lots[k]);
            if (fresh)
                (*fresh)[w] = (*fresh)[k];
        }
        l.lots[w].id = int(w);
        ++w;
    }
    l.lots.resize(w);
    if (fresh)
        fresh->resize(w);
    return gone;
}

void planParcels(Layout& l, const Site& site, const Ground& g, const Options& o)
{
    LILA_ZONE();

    const std::string era = eraOf(l.profile);
    l.edgeAccess = streetAccess(l.graph);
    makeBlocks(l);
    typeBlocks(l, g, site, era);

    for (Block& b : l.blocks)
    {
        if (streetSides(b.outline) == 0)
            continue;
        std::vector<Polygon> cut = cutBlock(b, l, o, &b.courtyards, &l.parcelWarnings);
        std::sort(cut.begin(), cut.end(), [](const Polygon& x, const Polygon& y) {
            const Vec2 cx = centroid(x), cy = centroid(y);
            return cx.x != cy.x ? cx.x < cy.x : cx.y < cy.y;
        });
        for (size_t k = 0; k < cut.size(); ++k)
        {
            Lot lot;
            lot.id = int(l.lots.size());
            lot.block = b.id;
            lot.key = stableKey(hash64(b.key * 0x9E3779B97F4A7C15ull + uint64_t(b.method) * 131 + k));
            lot.area = area(cut[k]) / kM2;
            lot.outline = std::move(cut[k]);
            lot.random = float(unitRandom(hash64(lot.key) ^ hash64(o.seed)));
            finishLot(lot, l);
            l.lots.push_back(std::move(lot));
        }
    }

    dropOverlappingLots(l, 0, nullptr);
    placeLandmarks(l, g, era);
    assignUses(l, g, era);
    applyLotOverrides(l, site);
    for (Lot& lot : l.lots)
    {
        lot.route = routeAt(site, centroid(lot.outline));
        for (int& t : lot.outline.tag)
            t = t >= 0 ? int(l.graph.edges[size_t(t)].cls) : -1;
    }

    if (o.buildings)
    {
        for (const Lot& lot : l.lots)
            if (getsEnvelope(lot.use) && lot.frontages > 0)
            {
                Envelope env = parcelEnvelope(lot, l.blocks[size_t(lot.block)], l, g, o, site);
                if (!env.footprint.pts.empty())
                    l.envelopes.push_back(std::move(env));
            }
        matchPartyWalls(l.envelopes);
        for (const Envelope& env : l.envelopes)
            if (env.route == BuildingRoute::Library)
                if (auto it = site.libraryPicks.find(env.key); it != site.libraryPicks.end())
                    l.placements.push_back(libraryPlacement(env, it->second));
    }
}
/**
 * Street access after a local edit without the whole-graph betweenness pass: edges that were there keep theirs, and each new
 * edge takes the mean of the old streets meeting its ends. planParcels measures it exactly.
 */
static void keepStreetAccess(Layout& l)
{
    const size_t before = std::min(l.edgeAccess.size(), l.graph.edges.size());
    l.edgeAccess.resize(l.graph.edges.size(), 0.0);
    for (size_t e = before; e < l.graph.edges.size(); ++e)
    {
        if (!l.graph.isStreet(int(e)))
            continue;
        double sum = 0.0;
        int n = 0;
        for (int end : {l.graph.edges[e].a, l.graph.edges[e].b})
            for (int k : l.graph.incident(end))
                if (size_t(k) < before && l.graph.isStreet(k))
                {
                    sum += l.edgeAccess[size_t(k)];
                    ++n;
                }
        l.edgeAccess[e] = n ? sum / n : 0.0;
    }
}

int replanParcels(Layout& l, const Site& site, const Ground& g, const Options& o, Vec2 lo, Vec2 hi)
{
    LILA_ZONE();

    const std::string era = eraOf(l.profile);
    std::vector<Block> oldBlocks = std::move(l.blocks);
    std::vector<Lot> oldLots = std::move(l.lots);
    std::vector<Envelope> oldEnvelopes = std::move(l.envelopes);
    l.blocks.clear();
    l.lots.clear();
    l.envelopes.clear();
    std::erase_if(l.placements, [](const Placement& p) { return p.id.starts_with("building/"); });
    keepStreetAccess(l);

    // Only the faces round the edges that changed are walked again; every other block stays as it was, lots and all.
    const StreetGraph& graph = l.graph;
    const Vec2 pad(2.0 * graph.snap());
    auto inBox = [&](Vec2 q) { return q.x >= lo.x - pad.x && q.x <= hi.x + pad.x && q.y >= lo.y - pad.y && q.y <= hi.y + pad.y; };
    std::vector<char> touched(graph.edges.size(), 0);
    std::vector<int> seeds;
    for (size_t e = 0; e < graph.edges.size(); ++e)
    {
        const StreetEdge& x = graph.edges[e];
        touched[e] = !x.alive || inBox(graph.nodes[size_t(x.a)].p) || inBox(graph.nodes[size_t(x.b)].p);
        if (touched[e] && x.alive)
            seeds.push_back(int(e));
    }
    std::vector<char> dirty(oldBlocks.size(), 0);
    for (const Block& b : oldBlocks)
    {
        bool d = std::ranges::any_of(b.outline.pts, inBox);
        for (int t : b.outline.tag)
            d = d || (t >= 0 && (size_t(t) >= touched.size() || touched[size_t(t)]));
        dirty[size_t(b.id)] = d;
        if (d)
            for (int t : b.outline.tag)
                if (t >= 0 && size_t(t) < graph.edges.size() && graph.edges[size_t(t)].alive)
                    seeds.push_back(t);
    }
    std::ranges::sort(seeds);
    seeds.erase(std::unique(seeds.begin(), seeds.end()), seeds.end());

    std::vector<std::vector<int>> lotsOfOld(oldBlocks.size());
    for (const Lot& lot : oldLots)
        if (lot.block >= 0 && size_t(lot.block) < lotsOfOld.size())
            lotsOfOld[size_t(lot.block)].push_back(lot.id);
    std::vector<int> envOfOld(oldLots.size(), -1);
    for (size_t k = 0; k < oldEnvelopes.size(); ++k)
        if (oldEnvelopes[k].lot >= 0 && size_t(oldEnvelopes[k].lot) < envOfOld.size())
            envOfOld[size_t(oldEnvelopes[k].lot)] = int(k);

    std::vector<char> fresh;
    std::map<uint64_t, int> seen;
    std::unordered_map<uint64_t, int> cleanByKey;
    l.blocks.reserve(oldBlocks.size() + 16);
    l.lots.reserve(oldLots.size() + 256);
    l.envelopes.reserve(oldEnvelopes.size() + 256);
    fresh.reserve(oldLots.size() + 256);
    for (Block& old : oldBlocks)
    {
        if (dirty[size_t(old.id)])
            continue;
        const int oldId = old.id;
        Block b = std::move(old);
        b.id = int(l.blocks.size());
        cleanByKey[b.key] = b.id;
        for (int id : lotsOfOld[size_t(oldId)])
        {
            Lot lot = std::move(oldLots[size_t(id)]);
            lot.id = int(l.lots.size());
            lot.block = b.id;
            if (const int e = envOfOld[size_t(id)]; e >= 0)
            {
                Envelope env = std::move(oldEnvelopes[size_t(e)]);
                env.lot = lot.id;
                l.envelopes.push_back(std::move(env));
            }
            l.lots.push_back(std::move(lot));
            fresh.push_back(0);
        }
        l.blocks.push_back(std::move(b));
    }
    const size_t firstFresh = l.blocks.size();
    addBlocks(l, planarFaces(graph, &seeds), seen);
    // A face walked again that did not change is the clean block already kept.
    std::erase_if(l.blocks, [&](const Block& b) { return size_t(b.id) >= firstFresh && cleanByKey.contains(b.key); });
    for (size_t k = firstFresh; k < l.blocks.size(); ++k)
    {
        Block& b = l.blocks[k];
        b.id = int(k);
        if (cleanByKey.contains(b.key))
            b.key = stableKey(hash64(b.key ^ uint64_t(roundTo(centroid(b.outline).x, 1000.0) * 83492791)));
    }
    typeBlocks(l, g, site, era, firstFresh);

    int cut = 0;
    const size_t firstFreshLot = l.lots.size();
    for (Block& b : std::span(l.blocks).subspan(firstFresh))
    {
        ++cut;
        if (streetSides(b.outline) == 0)
            continue;
        std::vector<Polygon> pieces = cutBlock(b, l, o, &b.courtyards, &l.parcelWarnings);
        std::sort(pieces.begin(), pieces.end(), [](const Polygon& x, const Polygon& y) {
            const Vec2 cx = centroid(x), cy = centroid(y);
            return cx.x != cy.x ? cx.x < cy.x : cx.y < cy.y;
        });
        for (size_t k = 0; k < pieces.size(); ++k)
        {
            Lot lot;
            lot.id = int(l.lots.size());
            lot.block = b.id;
            lot.key = stableKey(hash64(b.key * 0x9E3779B97F4A7C15ull + uint64_t(b.method) * 131 + k));
            lot.area = area(pieces[k]) / kM2;
            lot.outline = std::move(pieces[k]);
            lot.random = float(unitRandom(hash64(lot.key) ^ hash64(o.seed)));
            finishLot(lot, l);
            l.lots.push_back(std::move(lot));
            fresh.push_back(1);
        }
    }

    dropOverlappingLots(l, firstFreshLot, &fresh);
    // New lots rank against the cuts the whole town's ranking made; a layout made before them is ranked again whole.
    if (l.useCuts.set)
        assignUses(l, g, era, &fresh);
    else
    {
        std::vector<Lot> kept = l.lots;
        for (size_t k = 0; k < l.lots.size(); ++k)
            if (!fresh[k])
                for (int& t : l.lots[k].outline.tag)
                    t = -1;
        assignUses(l, g, era);
        for (size_t k = 0; k < l.lots.size(); ++k)
            if (!fresh[k])
                l.lots[k] = std::move(kept[k]);
    }
    applyLotOverrides(l, site);
    for (size_t k = 0; k < l.lots.size(); ++k)
    {
        if (!fresh[k])
            continue;
        Lot& lot = l.lots[k];
        lot.route = routeAt(site, centroid(lot.outline));
        for (int& t : lot.outline.tag)
            t = t >= 0 ? int(l.graph.edges[size_t(t)].cls) : -1;
    }

    if (o.buildings)
    {
        for (size_t k = 0; k < l.lots.size(); ++k)
            if (fresh[k] && getsEnvelope(l.lots[k].use) && l.lots[k].frontages > 0)
            {
                Envelope env = parcelEnvelope(l.lots[k], l.blocks[size_t(l.lots[k].block)], l, g, o, site);
                if (!env.footprint.pts.empty())
                    l.envelopes.push_back(std::move(env));
            }
        std::sort(l.envelopes.begin(), l.envelopes.end(), [](const Envelope& a, const Envelope& b) { return a.lot < b.lot; });
        // Party walls change only where lots were cut afresh and next to them; the kept ones are matched already.
        const Vec2 margin(2.0 * l.profile.lotDepth);
        const Vec2 near0 = lo - margin, near1 = hi + margin;
        std::vector<size_t> local;
        const double far = std::max(20000.0, 5.0 * l.profile.lotDepth);
        for (size_t k = 0; k < l.envelopes.size(); ++k)
        {
            const std::vector<Vec2>& pts = l.envelopes[k].footprint.pts;
            if (pts.empty() || pts[0].x < near0.x - far || pts[0].x > near1.x + far || pts[0].y < near0.y - far || pts[0].y > near1.y + far)
                continue;
            for (const Vec2& q : pts)
                if (q.x >= near0.x && q.x <= near1.x && q.y >= near0.y && q.y <= near1.y)
                {
                    local.push_back(k);
                    break;
                }
        }
        // Their neighbours join the match so walls shared across the region's edge are found again, but only the local ones are written back.
        auto box = [](const Polygon& p) {
            Vec2 a(1e300), z(-1e300);
            for (const Vec2& q : p.pts)
                a = glm::min(a, q), z = glm::max(z, q);
            return std::pair{a - Vec2(10.0), z + Vec2(10.0)};
        };
        std::vector<std::pair<Vec2, Vec2>> localBoxes;
        std::vector<char> isLocal(l.envelopes.size(), 0);
        for (size_t k : local)
        {
            localBoxes.push_back(box(l.envelopes[k].footprint));
            isLocal[k] = 1;
        }
        Vec2 all0(1e300), all1(-1e300);
        for (const auto& [a, z] : localBoxes)
            all0 = glm::min(all0, a), all1 = glm::max(all1, z);
        std::vector<size_t> context = local;
        for (size_t k = 0; k < l.envelopes.size(); ++k)
        {
            if (isLocal[k] || l.envelopes[k].footprint.pts.empty())
                continue;
            const Vec2 q = l.envelopes[k].footprint.pts.front();
            const double reach = std::max(20000.0, 5.0 * l.profile.lotDepth);
            if (q.x < all0.x - reach || q.x > all1.x + reach || q.y < all0.y - reach || q.y > all1.y + reach)
                continue;
            const auto [a, z] = box(l.envelopes[k].footprint);
            if (std::ranges::any_of(localBoxes, [&](const auto& b) { return a.x <= b.second.x && b.first.x <= z.x && a.y <= b.second.y && b.first.y <= z.y; }))
                context.push_back(k);
        }
        std::vector<Envelope> part;
        for (size_t k : context)
            part.push_back(l.envelopes[k]);
        matchPartyWalls(part);
        for (size_t k = 0; k < local.size(); ++k)
            l.envelopes[local[k]] = std::move(part[k]);
        for (const Envelope& env : l.envelopes)
            if (env.route == BuildingRoute::Library)
                if (auto it = site.libraryPicks.find(env.key); it != site.libraryPicks.end())
                    l.placements.push_back(libraryPlacement(env, it->second));
    }
    return cut;
}
} // namespace settlement
