// Settlement: block types, lot methods and the edits that override them, and cutting blocks into lots that all face a
// street.

#include "Parcels.h"

#include <lila/Profile.h>
#include <lila/Skeleton.h>

#include <algorithm>
#include <cmath>
#include <limits>
#include <stdexcept>

namespace settlement
{
namespace
{
constexpr const char* kBlockTypes[] = {"perimeter", "courtyard", "open", "row", "superblock", "park", "plaza", "market", "churchyard", "field"};
constexpr const char* kLotMethods[] = {"frontage", "obb", "skeleton", "burgage", "voronoi", "strips", "whole"};
constexpr const char* kRoutes[] = {"outline", "massing", "library", "masonry", "unreal"};
constexpr const char* kEras[] = {"medieval", "early", "industrial", "modern", "future"};

template <class E, size_t N> bool parseName(const char* const (&names)[N], std::string_view name, E* out)
{
    for (size_t k = 0; k < N; ++k)
        if (name == names[k])
        {
            *out = E(k);
            return true;
        }
    return false;
}

template <size_t N> std::string nameList(const char* const (&names)[N])
{
    std::string s;
    for (const char* n : names)
        s += (s.empty() ? "" : ", ") + std::string(n);
    return s;
}

/** A stable stream of numbers for one block. */
struct BlockRandom
{
    uint64_t state;
    double next()
    {
        state = hash64(state);
        return double(state >> 11) * (1.0 / 9007199254740992.0);
    }
    double range(double a, double b) { return a + (b - a) * next(); }
};

double sideLength(const Polygon& p, size_t k) { return glm::length(p.pts[(k + 1) % p.size()] - p.pts[k]); }

bool facesStreet(const Polygon& p)
{
    for (size_t k = 0; k < p.size(); ++k)
        if (p.tag[k] >= 0 && sideLength(p, k) >= 100.0)
            return true;
    return false;
}

bool usable(const Polygon& p) { return p.size() >= 3 && area(p) > 1.0; }

StreetClass edgeClass(const Layout& l, int tag) { return tag >= 0 ? l.graph.edges[size_t(tag)].cls : StreetClass::Boundary; }

/** The street side lots are laid along: the longest, or (byClass) the one on the most important street. -1 when none. */
int frontSide(const Polygon& p, const Layout& l, bool byClass)
{
    int best = -1;
    double score = 0.0;
    for (size_t k = 0; k < p.size(); ++k)
    {
        if (p.tag[k] < 0)
            continue;
        double s = sideLength(p, k);
        if (byClass)
            s *= 1.0 + 0.6 * (int(StreetClass::Alley) - int(std::min(edgeClass(l, p.tag[k]), StreetClass::Alley)));
        if (s > score)
        {
            score = s;
            best = int(k);
        }
    }
    return best;
}

/** Extents of the polygon along u and v from o. */
void extents(const Polygon& p, Vec2 o, Vec2 u, Vec2 v, double& u0, double& u1, double& v0, double& v1)
{
    u0 = v0 = std::numeric_limits<double>::max();
    u1 = v1 = -u0;
    for (const Vec2& q : p.pts)
    {
        u0 = std::min(u0, glm::dot(q - o, u)), u1 = std::max(u1, glm::dot(q - o, u));
        v0 = std::min(v0, glm::dot(q - o, v)), v1 = std::max(v1, glm::dot(q - o, v));
    }
}

/**
 * True when a half-plane cut left one piece. Cutting a concave polygon can leave several, joined by edges running along
 * the cut line outside the polygon; such a piece has more than one edge on the line.
 */
bool onePiece(const Polygon& piece, Vec2 at, Vec2 n)
{
    int onLine = 0;
    for (size_t k = 0; k < piece.size(); ++k)
        if (std::abs(glm::dot(piece.pts[k] - at, n)) < 1.0 && std::abs(glm::dot(piece.pts[(k + 1) % piece.size()] - at, n)) < 1.0)
            ++onLine;
    return onLine <= 1;
}

/** Both halves of p either side of the line, each still facing a street. */
bool splitFacing(const Polygon& p, Vec2 at, Vec2 n, std::vector<Polygon>& todo)
{
    Polygon a = clipHalfPlane(p, at, n), b = clipHalfPlane(p, at, -n);
    if (!usable(a) || !usable(b) || !facesStreet(a) || !facesStreet(b) || !onePiece(a, at, n) || !onePiece(b, at, n))
        return false;
    todo.push_back(std::move(a));
    todo.push_back(std::move(b));
    return true;
}

/** Lots along a street side: across it every width, back to back where deeper than about two depths. */
std::vector<Polygon> frontageCut(const Polygon& start, const Layout& l, double width, double depth, bool byClass, double minArea,
                                 BlockRandom& rng)
{
    std::vector<Polygon> out, todo = {start};
    for (int guard = 0; guard < 8192 && !todo.empty(); ++guard)
    {
        Polygon poly = std::move(todo.back());
        todo.pop_back();
        const int front = frontSide(poly, l, byClass);
        bool split = false;
        if (front >= 0 && area(poly) > 2.0 * minArea)
        {
            const Vec2 o = poly.pts[size_t(front)];
            const Vec2 u = glm::normalize(poly.pts[(size_t(front) + 1) % poly.size()] - o), v = leftNormal(u);
            double u0, u1, v0, v1;
            extents(poly, o, u, v, u0, u1, v0, v1);
            const double w = u1 - u0, d = v1 - v0;
            if (d > 2.2 * depth)
                split = splitFacing(poly, o + v * (v0 + d * rng.range(0.45, 0.55)), v, todo);
            if (!split && w > 1.6 * width)
            {
                const int n = std::max(2, int(std::lround(w / width)));
                split = splitFacing(poly, o + u * (u0 + w * (double(n / 2) / n) + rng.range(-0.08, 0.08) * width), u, todo);
            }
        }
        if (!split)
            out.push_back(std::move(poly));
    }
    return out;
}

} // namespace

void boxOf(const Polygon& p, Vec2& c, Vec2& u, Vec2& v, double& hu, double& hv)
{
    double best = std::numeric_limits<double>::max();
    for (size_t k = 0; k < p.size(); ++k)
    {
        const Vec2 e = p.pts[(k + 1) % p.size()] - p.pts[k];
        if (glm::length(e) < 1e-6)
            continue;
        const Vec2 a = glm::normalize(e), b = leftNormal(a);
        double u0, u1, v0, v1;
        extents(p, p.pts[0], a, b, u0, u1, v0, v1);
        if ((u1 - u0) * (v1 - v0) < best)
        {
            best = (u1 - u0) * (v1 - v0);
            u = a, v = b;
            c = p.pts[0] + a * (0.5 * (u0 + u1)) + b * (0.5 * (v0 + v1));
            hu = 0.5 * (u1 - u0), hv = 0.5 * (v1 - v0);
        }
    }
    if (hu < hv)
    {
        std::swap(u, v);
        std::swap(hu, hv);
        v = leftNormal(u);
    }
}

namespace
{

/** Recursive splits across the long side of each piece's box until pieces are near the target area. */
std::vector<Polygon> obbCut(const Polygon& start, double target, double minWidth, BlockRandom& rng)
{
    std::vector<Polygon> out, todo = {start};
    for (int guard = 0; guard < 8192 && !todo.empty(); ++guard)
    {
        Polygon poly = std::move(todo.back());
        todo.pop_back();
        bool split = false;
        if (area(poly) > 1.8 * target)
        {
            Vec2 c{0.0}, u{1.0, 0.0}, v{0.0, 1.0};
            double hu = 0.0, hv = 0.0;
            boxOf(poly, c, u, v, hu, hv);
            const std::pair<Vec2, double> axes[] = {{u, hu}, {v, hv}};
            for (const auto& [axis, half] : axes)
            {
                if (half < minWidth)
                    continue;
                const Vec2 at = c + axis * (rng.range(-0.1, 0.1) * half);
                Polygon a = clipHalfPlane(poly, at, axis), b = clipHalfPlane(poly, at, -axis);
                if (usable(a) && usable(b) && facesStreet(a) && facesStreet(b) && area(a) > 0.3 * target && area(b) > 0.3 * target &&
                    onePiece(a, at, axis) && onePiece(b, at, axis))
                {
                    todo.push_back(std::move(a));
                    todo.push_back(std::move(b));
                    split = true;
                    break;
                }
            }
        }
        if (!split)
            out.push_back(std::move(poly));
    }
    return out;
}

/** The ring of skeleton nodes as a polygon: side 0 is the street side (tag), the rest untagged. Repeated points dropped. */
Polygon skeletonFace(const lila::curve::StraightSkeleton& sk, const std::vector<int>& ring, int tag)
{
    Polygon p;
    for (size_t i = 0; i < ring.size(); ++i)
    {
        const Vec2 q = sk.nodes[size_t(ring[i])];
        const int t = i == 0 ? tag : -1;
        // A zero-length side goes; the side after it starts where it did.
        if (!p.pts.empty() && glm::length(q - p.pts.back()) < 1.0)
            p.tag.back() = std::max(p.tag.back(), t);
        else
        {
            p.pts.push_back(q);
            p.tag.push_back(t);
        }
    }
    if (p.pts.size() > 1 && glm::length(p.pts.back() - p.pts.front()) < 1.0)
    {
        const int t = p.tag.back();
        p.pts.pop_back();
        p.tag.pop_back();
        p.tag.back() = std::max(p.tag.back(), t);
    }
    if (p.pts.size() < 3)
        return {};
    if (signedArea(p.pts) < 0.0)
    {
        std::reverse(p.pts.begin(), p.pts.end());
        std::reverse(p.tag.begin(), p.tag.end());
        std::rotate(p.tag.begin(), p.tag.begin() + 1, p.tag.end());
    }
    return p;
}

/**
 * Strips off each street side along the weighted straight skeleton (sides without a street stand still, so their ground
 * goes to the strips beside them), out to depth, cut across every width. What lies past depth is the courtyard.
 */
bool skeletonCut(const Polygon& block, double width, double depth, BlockRandom& rng, std::vector<Polygon>& out,
                 std::vector<Polygon>* courtyards, std::string& why)
{
    std::vector<double> weights(block.size());
    bool any = false;
    for (size_t k = 0; k < block.size(); ++k)
    {
        weights[k] = block.tag[k] >= 0 ? 1.0 : 0.0;
        any = any || block.tag[k] >= 0;
    }
    if (!any)
    {
        why = "no street side";
        return false;
    }
    lila::curve::SkeletonOptions so;
    so.maxTime = depth;
    so.weights = {weights};
    const lila::curve::StraightSkeleton sk = lila::curve::straightSkeleton({block.pts}, so);
    if (!sk.ok())
    {
        why = sk.error;
        return false;
    }
    std::vector<Polygon> pieces;
    double covered = 0.0;
    for (size_t k = 0; k < block.size() && k < sk.faces.size(); ++k)
    {
        if (block.tag[k] < 0 || sk.faces[k].size() < 3)
            continue;
        Polygon strip = skeletonFace(sk, sk.faces[k], block.tag[k]);
        if (!usable(strip))
            continue;
        covered += area(strip);
        const Vec2 a = block.pts[k], u = glm::normalize(block.pts[(k + 1) % block.size()] - a), v = leftNormal(u);
        double u0, u1, v0, v1;
        extents(strip, a, u, v, u0, u1, v0, v1);
        const int n = std::max(1, int(std::lround((u1 - u0) / width)));
        Polygon rest = strip;
        for (int i = 1; i < n; ++i)
        {
            const Vec2 at = a + u * (u0 + (u1 - u0) * (double(i) + rng.range(-0.1, 0.1)) / n);
            Polygon left = clipHalfPlane(rest, at, -u), right = clipHalfPlane(rest, at, u);
            if (usable(left) && usable(right) && facesStreet(left) && facesStreet(right) && onePiece(left, at, u) && onePiece(right, at, u))
            {
                pieces.push_back(std::move(left));
                rest = std::move(right);
            }
        }
        pieces.push_back(std::move(rest));
    }
    double inside = 0.0;
    std::vector<Polygon> yards;
    for (const std::vector<int>& loop : sk.front)
    {
        Polygon y;
        for (int n : loop)
            y.pts.push_back(sk.nodes[size_t(n)]);
        y = makePolygon(std::move(y.pts));
        if (usable(y))
        {
            inside += area(y);
            yards.push_back(std::move(y));
        }
    }
    // A skeleton that lost ground (a face it could not trace) would leave gaps between lots.
    if (std::abs(covered + inside - area(block)) > 0.02 * area(block))
    {
        why = "its strips cover " + std::to_string(int(std::lround(100.0 * (covered + inside) / area(block)))) + "% of the block";
        return false;
    }
    out = std::move(pieces);
    if (courtyards)
        *courtyards = std::move(yards);
    return true;
}

/** Cells around seeds set back from each street side; seeds whose cell reaches no street are dropped, so neighbours take it. */
std::vector<Polygon> voronoiCut(const Polygon& block, double width, double depth, BlockRandom& rng)
{
    std::vector<Vec2> seeds;
    for (size_t k = 0; k < block.size(); ++k)
    {
        const double len = sideLength(block, k);
        if (block.tag[k] < 0 || len < 0.5 * width)
            continue;
        const Vec2 a = block.pts[k], d = (block.pts[(k + 1) % block.size()] - a) / len;
        const int n = std::max(1, int(std::lround(len / width)));
        for (int i = 0; i < n; ++i)
        {
            const double t = (i + 0.5 + rng.range(-0.25, 0.25)) / n;
            const Vec2 q = a + d * (t * len) + leftNormal(d) * (depth * rng.range(0.3, 0.6));
            if (contains(block.pts, q))
                seeds.push_back(q);
        }
    }
    if (seeds.size() < 2)
        return {block};
    std::vector<Polygon> cells;
    for (int pass = 0; pass < 8; ++pass)
    {
        cells.assign(seeds.size(), Polygon{});
        for (size_t i = 0; i < seeds.size(); ++i)
        {
            Polygon c = block;
            for (size_t j = 0; j < seeds.size() && usable(c); ++j)
                if (j != i && glm::length(seeds[j] - seeds[i]) > 1e-6)
                    c = clipHalfPlane(c, (seeds[i] + seeds[j]) * 0.5, glm::normalize(seeds[i] - seeds[j]));
            cells[i] = std::move(c);
        }
        std::vector<Vec2> kept;
        for (size_t i = 0; i < seeds.size(); ++i)
            if (usable(cells[i]) && facesStreet(cells[i]))
                kept.push_back(seeds[i]);
        if (kept.size() == seeds.size() || kept.size() < 2)
            break;
        seeds = std::move(kept);
    }
    std::vector<Polygon> out;
    for (Polygon& c : cells)
        if (usable(c))
            out.push_back(std::move(c));
    return out;
}

double boundaryShared(const Polygon& a, const Polygon& b)
{
    double shared = 0.0;
    for (size_t i = 0; i < a.size(); ++i)
    {
        const Vec2 p = a.pts[i], q = a.pts[(i + 1) % a.size()];
        const double len = glm::length(q - p);
        if (len < 1e-6)
            continue;
        const Vec2 d = (q - p) / len;
        for (size_t j = 0; j < b.size(); ++j)
        {
            const Vec2 r = b.pts[j], s = b.pts[(j + 1) % b.size()];
            if (std::abs(cross(d, r - p)) > 2.0 || std::abs(cross(d, s - p)) > 2.0)
                continue;
            const double t0 = std::clamp(glm::dot(r - p, d), 0.0, len), t1 = std::clamp(glm::dot(s - p, d), 0.0, len);
            shared += std::abs(t1 - t0);
        }
    }
    return shared;
}

/** Lots that reach no street, or are too small to build on, join the neighbour they share the most boundary with. */
void joinLandlocked(std::vector<Polygon>& lots, double minArea, std::vector<std::string>* warnings, uint64_t blockKey)
{
    int left = 0;
    std::vector<char> tried(lots.size(), 0);
    for (size_t i = 0; i < lots.size();)
    {
        const bool landlocked = !facesStreet(lots[i]);
        if ((!landlocked && area(lots[i]) >= minArea) || tried[i])
        {
            ++i;
            continue;
        }
        int best = -1;
        double shared = 0.0;
        for (size_t j = 0; j < lots.size(); ++j)
            if (j != i)
                if (const double s = boundaryShared(lots[i], lots[j]); s > shared)
                {
                    shared = s;
                    best = int(j);
                }
        Polygon joined = best >= 0 ? joinPolygons(lots[size_t(best)], lots[i]) : Polygon{};
        if (joined.pts.empty())
        {
            left += landlocked ? 1 : 0;
            tried[i] = 1;
            ++i;
            continue;
        }
        lots[size_t(best)] = std::move(joined);
        tried[size_t(best)] = 0;
        lots.erase(lots.begin() + long(i));
        tried.erase(tried.begin() + long(i));
        i = 0;
    }
    if (left > 0 && warnings)
        warnings->push_back("block " + std::to_string(blockKey) + ": " + std::to_string(left) +
                            " lot(s) reach no street and could not join a neighbour; left as open ground");
}
} // namespace

const char* blockTypeName(BlockType t) { return size_t(t) < std::size(kBlockTypes) ? kBlockTypes[size_t(t)] : "perimeter"; }
bool parseBlockType(std::string_view name, BlockType* out) { return parseName(kBlockTypes, name, out); }
const char* lotMethodName(LotMethod m) { return size_t(m) < std::size(kLotMethods) ? kLotMethods[size_t(m)] : "frontage"; }
bool parseLotMethod(std::string_view name, LotMethod* out) { return parseName(kLotMethods, name, out); }
const char* routeName(BuildingRoute r) { return size_t(r) < std::size(kRoutes) ? kRoutes[size_t(r)] : "outline"; }
bool parseRoute(std::string_view name, BuildingRoute* out) { return parseName(kRoutes, name, out); }

bool parseUse(std::string_view name, Use* out)
{
    for (int u = 0; u <= int(Use::Cemetery); ++u)
        if (name == useName(Use(u)))
        {
            *out = Use(u);
            return true;
        }
    return false;
}

std::string eraOf(const Profile& p)
{
    if (!p.era.empty())
        return p.era;
    const std::pair<const char*, const char*> named[] = {{"hamlet", "medieval"},       {"streetVillage", "medieval"}, {"medievalTown", "medieval"},
                                                         {"europeanCity", "early"},    {"usTown", "industrial"},      {"manhattan", "modern"},
                                                         {"modernCity", "modern"},     {"scifiMegacity", "future"}};
    for (const auto& [n, e] : named)
        if (p.name == n)
            return e;
    switch (p.pattern)
    {
    case Pattern::Organic:
    case Pattern::Linear: return "medieval";
    case Pattern::OrganicGrid: return "early";
    case Pattern::Radial: return "future";
    case Pattern::Grid:
    case Pattern::None: return "modern";
    }
    return "modern";
}

bool preModern(std::string_view era) { return era == "medieval" || era == "early"; }

void applyParcelEdits(Site& site, const json& edits)
{
    auto idOf = [](const json& j, const char* key, const char* what) {
        if (!j.is_object() || !j.contains(key) || !j[key].is_number_integer())
            throw std::runtime_error(std::string(what) + " are objects with an integer " + key + " id");
        return j[key].get<uint64_t>();
    };
    for (const json& b : edits.value("blockOverrides", json::array()))
    {
        BlockOverride o;
        o.block = idOf(b, "block", "block overrides");
        if (b.contains("type"))
        {
            if (!b["type"].is_string() || !parseBlockType(b["type"].get<std::string>(), &o.type))
                throw std::runtime_error("block override " + std::to_string(o.block) + ": unknown type (" + nameList(kBlockTypes) + ")");
            o.hasType = true;
        }
        if (b.contains("method"))
        {
            if (!b["method"].is_string() || !parseLotMethod(b["method"].get<std::string>(), &o.method))
                throw std::runtime_error("block override " + std::to_string(o.block) + ": unknown method (" + nameList(kLotMethods) + ")");
            o.hasMethod = true;
        }
        site.blockOverrides.push_back(o);
    }
    for (const json& j : edits.value("lotOverrides", json::array()))
    {
        LotOverride o;
        o.lot = idOf(j, "lot", "lot overrides");
        if (j.contains("use"))
        {
            Use u = Use::Residential;
            if (!j["use"].is_string() || !parseUse(j["use"].get<std::string>(), &u))
                throw std::runtime_error("lot override " + std::to_string(o.lot) + ": unknown use");
            o.use = uint8_t(u);
            o.hasUse = true;
        }
        o.landmark = j.value("landmark", std::string());
        site.lotOverrides.push_back(std::move(o));
    }
    for (const json& j : edits.value("buildingRoutes", json::array()))
    {
        if (!j.is_object())
            throw std::runtime_error("building routes are objects");
        RouteRegion r;
        if (const std::string name = j.value("route", std::string("outline")); !parseRoute(name, &r.route))
            throw std::runtime_error("unknown building route '" + name + "' (" + nameList(kRoutes) + ")");
        r.name = j.value("name", std::string());
        for (const json& p : j.value("polygon", json::array()))
        {
            if (!p.is_array() || p.size() < 2)
                throw std::runtime_error("building route polygons are lists of [x, z] points");
            r.polygon.push_back({p[0].get<double>(), p[p.size() == 3 ? 2 : 1].get<double>()});
        }
        if (!r.polygon.empty() && r.polygon.size() < 3)
            throw std::runtime_error("a building route polygon needs three or more points");
        site.buildingRoutes.push_back(std::move(r));
    }
    if (const json picks = edits.value("libraryPicks", json::object()); picks.is_object())
        for (const auto& [key, v] : picks.items())
        {
            if (!v.is_object() || !v.contains("asset"))
                throw std::runtime_error("libraryPicks are {lot id: {asset, unreal, assetHeading}}");
            site.libraryPicks[std::stoull(key)] = {v["asset"].get<std::string>(), v.value("unreal", std::string()), v.value("assetHeading", 0.0)};
        }
    if (const json f = edits.value("foundations", json::object()); f.is_object())
        for (const auto& [key, v] : f.items())
        {
            if (!v.is_object() || !v.contains("min") || !v.contains("max"))
                throw std::runtime_error("foundations are {lot id: {min, max}}");
            site.foundations[std::stoull(key)] = {v["min"].get<double>(), v["max"].get<double>()};
        }
}

void validateParcelProfile(const Profile& p)
{
    if (!p.era.empty() && std::ranges::find_if(kEras, [&](const char* e) { return p.era == e; }) == std::end(kEras))
        throw std::runtime_error("unknown era '" + p.era + "' (" + nameList(kEras) + ")");
    BlockType t;
    if (p.blockType != "auto" && !parseBlockType(p.blockType, &t))
        throw std::runtime_error("unknown blockType '" + p.blockType + "' (auto, " + nameList(kBlockTypes) + ")");
    LotMethod m;
    if (p.lotMethod != "auto" && !parseLotMethod(p.lotMethod, &m))
        throw std::runtime_error("unknown lotMethod '" + p.lotMethod + "' (auto, " + nameList(kLotMethods) + ")");
}

std::vector<Polygon> cutBlock(const Block& b, const Layout& l, const Options& o, std::vector<Polygon>* courtyards,
                              std::vector<std::string>* warnings)
{
    LILA_ZONE();

    const Profile& p = o.profile;
    BlockRandom rng{hash64(hash64(o.seed) ^ b.key)};
    const double minArea = p.minLot * kM2;
    const double width = std::max(p.lotWidth, 100.0), depth = std::max(p.lotDepth, 100.0);
    std::vector<Polygon> lots;
    auto fellBack = [&](const std::string& why) {
        if (warnings)
            warnings->push_back("block " + std::to_string(b.key) + ": " + lotMethodName(b.method) + " lots failed (" + why +
                                "); cut along its frontage instead");
        lots = frontageCut(b.outline, l, width, depth, false, minArea, rng);
    };
    switch (b.method)
    {
    case LotMethod::Whole: lots = {b.outline}; break;
    case LotMethod::Frontage: lots = frontageCut(b.outline, l, width, depth, false, minArea, rng); break;
    case LotMethod::Burgage: lots = frontageCut(b.outline, l, 0.6 * width, 1.8 * depth, true, minArea, rng); break;
    case LotMethod::Strips: lots = frontageCut(b.outline, l, std::max(2.5 * width, 1500.0), 8.0 * depth, false, minArea, rng); break;
    case LotMethod::Obb:
    {
        const double scale = b.type == BlockType::Superblock ? 4.0 : 1.0;
        lots = obbCut(b.outline, scale * width * depth, 0.5 * width, rng);
        break;
    }
    case LotMethod::Voronoi: lots = voronoiCut(b.outline, width, depth, rng); break;
    case LotMethod::Skeleton:
    {
        // Courtyard blocks keep their middle open; perimeter blocks are filled to the middle.
        const double reach = b.type == BlockType::Courtyard ? depth : std::numeric_limits<double>::infinity();
        std::string why;
        if (!skeletonCut(b.outline, width, reach, rng, lots, courtyards, why))
            fellBack(why);
        break;
    }
    }
    std::erase_if(lots, [](const Polygon& x) { return !usable(x); });
    if (b.method != LotMethod::Whole)
        joinLandlocked(lots, minArea, warnings, b.key);
    // A lot reaching past its block would overlap the next block's: it is left out, and said so.
    const size_t before = lots.size();
    std::erase_if(lots, [&](const Polygon& x) {
        return std::ranges::any_of(x.pts, [&](Vec2 q) { return !contains(b.outline.pts, q) && boundaryDistance(b.outline.pts, q) > 10.0; });
    });
    if (lots.size() < before && warnings)
        warnings->push_back("block " + std::to_string(b.key) + ": " + std::to_string(before - lots.size()) + " " + lotMethodName(b.method) +
                            " lot(s) reached past the block and were left open");
    return lots;
}

Polygon joinPolygons(const Polygon& a, const Polygon& b, double tol)
{
    struct Side
    {
        Vec2 p, q;
        int tag;
    };
    auto sidesOf = [](const Polygon& x) {
        std::vector<Side> s;
        for (size_t k = 0; k < x.size(); ++k)
            s.push_back({x.pts[k], x.pts[(k + 1) % x.size()], x.tag[k]});
        return s;
    };
    // Sides split where the other polygon's corners lie on them, so shared stretches match corner to corner.
    auto splitAt = [tol](const std::vector<Side>& in, const std::vector<Vec2>& pts) {
        std::vector<Side> out;
        for (const Side& s : in)
        {
            const Vec2 d = s.q - s.p;
            const double len2 = glm::dot(d, d);
            std::vector<std::pair<double, Vec2>> cuts;
            if (len2 > 1e-9)
                for (const Vec2& q : pts)
                {
                    const double t = glm::dot(q - s.p, d) / len2;
                    if (t > 1e-6 && t < 1.0 - 1e-6 && glm::length(s.p + d * t - q) < tol && glm::length(q - s.p) > tol &&
                        glm::length(q - s.q) > tol)
                        cuts.push_back({t, q});
                }
            std::sort(cuts.begin(), cuts.end(), [](const auto& x, const auto& y) { return x.first < y.first; });
            Vec2 from = s.p;
            for (const auto& [t, q] : cuts)
            {
                out.push_back({from, q, s.tag});
                from = q;
            }
            out.push_back({from, s.q, s.tag});
        }
        return out;
    };
    std::vector<Side> sa = splitAt(sidesOf(a), b.pts), sb = splitAt(sidesOf(b), a.pts);
    auto near = [tol](Vec2 x, Vec2 y) { return glm::length(x - y) < tol; };
    std::vector<char> deadA(sa.size(), 0), deadB(sb.size(), 0);
    double shared = 0.0;
    for (size_t i = 0; i < sa.size(); ++i)
        for (size_t j = 0; j < sb.size(); ++j)
            if (!deadB[j] && near(sa[i].p, sb[j].q) && near(sa[i].q, sb[j].p))
            {
                deadA[i] = deadB[j] = 1;
                shared += glm::length(sa[i].q - sa[i].p);
                break;
            }
    if (shared < tol)
        return {};
    std::vector<Side> rest;
    for (size_t i = 0; i < sa.size(); ++i)
        if (!deadA[i])
            rest.push_back(sa[i]);
    for (size_t j = 0; j < sb.size(); ++j)
        if (!deadB[j])
            rest.push_back(sb[j]);
    if (rest.size() < 3)
        return {};
    std::vector<char> used(rest.size(), 0);
    Polygon out;
    size_t cur = 0;
    used[0] = 1;
    for (size_t step = 0; step < rest.size(); ++step)
    {
        out.pts.push_back(rest[cur].p);
        out.tag.push_back(rest[cur].tag);
        size_t next = rest.size();
        for (size_t k = 0; k < rest.size(); ++k)
            if (!used[k] && near(rest[k].p, rest[cur].q))
            {
                next = k;
                break;
            }
        if (next == rest.size())
            break;
        used[next] = 1;
        cur = next;
    }
    if (std::ranges::count(used, 0) > 0 || !near(rest[cur].q, rest[0].p))
        return {};
    // Corners left in the middle of a straight run by the join go, when both sides agree on the street.
    for (bool changed = true; changed && out.pts.size() > 3;)
    {
        changed = false;
        for (size_t k = 0; k < out.pts.size() && out.pts.size() > 3; ++k)
        {
            const size_t n = out.pts.size(), prev = (k + n - 1) % n;
            const Vec2 a0 = out.pts[prev], a1 = out.pts[k], a2 = out.pts[(k + 1) % n];
            const double len = glm::length(a2 - a0);
            if (out.tag[prev] == out.tag[k] && len > 1e-6 && std::abs(cross(a2 - a0, a1 - a0)) / len < 0.5)
            {
                out.pts.erase(out.pts.begin() + long(k));
                out.tag.erase(out.tag.begin() + long(k));
                changed = true;
                break;
            }
        }
    }
    if (signedArea(out.pts) <= 0.0 || std::abs(area(out) - area(a) - area(b)) > 0.01 * (area(a) + area(b)) || selfIntersects(out.pts))
        return {};
    return out;
}
} // namespace settlement
