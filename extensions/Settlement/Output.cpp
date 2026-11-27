// Settlement: measures and checks for agents, the layout as JSON, and meshes for the viewport and export.

#include "Generators.h"
#include "Parcels.h"
#include "Streetscape.h"

#include <lila/Profile.h>

#include <tbb/parallel_for.h>

#include <algorithm>
#include <array>
#include <format>
#include <cmath>
#include <functional>
#include <limits>
#include <map>
#include <numeric>
#include <tuple>
#include <unordered_map>

namespace settlement
{
namespace
{
json xz(Vec2 p) { return json::array({p.x, p.y}); }

json ring(const Polygon& p)
{
    json a = json::array();
    for (const Vec2& q : p.pts)
        a.push_back(xz(q));
    return a;
}

const char* sideName(int tag)
{
    switch (tag)
    {
    case 0: return "street";
    case 1: return "party";
    default: return "open";
    }
}

int findRoot(std::vector<int>& parent, int x)
{
    while (parent[size_t(x)] != x)
        x = parent[size_t(x)] = parent[size_t(parent[size_t(x)])];
    return x;
}

double median(std::vector<double> v)
{
    if (v.empty())
        return 0.0;
    std::sort(v.begin(), v.end());
    return v[v.size() / 2];
}

/** Writes per-face values once every face exists: adding layers or faces moves earlier spans. */
void setFaceInts(geo::Mesh& m, const std::vector<std::pair<const char*, std::vector<int32_t>>>& layers)
{
    for (const auto& [name, values] : layers)
        m.faceAttrs().add(name, geo::AttrType::Int);
    for (const auto& [name, values] : layers)
    {
        std::span<int32_t> out = m.faceAttrs().get<int32_t>(name);
        std::copy(values.begin(), values.begin() + std::min(values.size(), out.size()), out.begin());
    }
}

enum Surface : int32_t
{
    SurfAsphalt,
    SurfSidewalk,
    SurfDirt,
    SurfPaving,
    SurfConcrete,
    SurfGrass,
    SurfYard,
    SurfField,
    SurfCemetery,
};

constexpr SurfaceMaterial kSurfaces[] = {
    {"Settlement_Asphalt", {0.075f, 0.075f, 0.08f}, 0.9f}, {"Settlement_Sidewalk", {0.36f, 0.34f, 0.31f}, 0.85f},
    {"Settlement_Dirt", {0.26f, 0.19f, 0.12f}, 0.95f},     {"Settlement_Paving", {0.4f, 0.33f, 0.26f}, 0.85f},
    {"Settlement_Concrete", {0.3f, 0.29f, 0.27f}, 0.85f},  {"Settlement_Grass", {0.075f, 0.13f, 0.04f}, 0.95f},
    {"Settlement_Yard", {0.25f, 0.22f, 0.18f}, 0.9f},      {"Settlement_Field", {0.13f, 0.14f, 0.06f}, 0.95f},
    {"Settlement_Cemetery", {0.2f, 0.19f, 0.15f}, 0.9f},
};

double townDistance(const Layout& l, Vec2 q) { return std::clamp(glm::length(q - l.center) / std::max(l.radius, 1.0), 0.0, 1.0); }

/**
 * A street's carriageway paving: paths paved in town and earth beyond, medieval streets cobbled or earth, alleys concrete,
 * outer residential streets more often earth the further out they lie, the rest asphalt. Stable per street.
 */
int32_t streetSurface(const Layout& l, const StreetEdge& x, Vec2 mid, bool medieval)
{
    const double dist = townDistance(l, mid);
    if (x.cls == StreetClass::Path)
        return dist < 0.5 ? SurfPaving : SurfDirt;
    if (medieval)
        return x.cls <= StreetClass::Collector ? SurfPaving : SurfDirt;
    if (x.cls == StreetClass::Alley)
        return SurfConcrete;
    if (x.cls == StreetClass::Local && unitRandom(hash64(x.street ^ 0x51DE5EEDull)) < 0.5 * std::clamp((dist - 0.7) / 0.3, 0.0, 1.0))
        return SurfDirt;
    return SurfAsphalt;
}

/** Corners turning less than this get a curb return; wider ones, and bends, are joined round the junction's centre. */
constexpr double kFilletGap = 160.0 * kPi / 180.0;
/** Corners up to this have curb lines that meet; past it they are taken as the outside of a bend. */
constexpr double kArcGap = 174.0 * kPi / 180.0;
/** cm: about how far apart the points of a curb return and a sidewalk corner are. */
constexpr double kCornerStep = 150.0;

/** Width of each sidewalk on a street of this width: its road profile's share, none on alleys, paths and kerbless streets. */
double walkWidth(const Layout& l, int e, double width, bool medieval)
{
    if (medieval || l.graph.edges[size_t(e)].cls >= StreetClass::Alley)
        return 0.0;
    const RoadProfile& rp = edgeProfile(l, e);
    const double full = rp.width();
    return full > 0.0 ? width * std::clamp((rp.sidewalk + rp.verge) / full, 0.0, 0.3) : 0.0;
}

/** Convex hull, counter-clockwise. */
std::vector<Vec2> hull(std::vector<Vec2> pts)
{
    std::sort(pts.begin(), pts.end(), [](Vec2 a, Vec2 b) { return a.x != b.x ? a.x < b.x : a.y < b.y; });
    if (pts.size() < 3)
        return pts;
    std::vector<Vec2> h(2 * pts.size());
    size_t k = 0;
    for (size_t i = 0; i < pts.size(); ++i)
    {
        while (k >= 2 && cross(h[k - 1] - h[k - 2], pts[i] - h[k - 2]) <= 0.0)
            --k;
        h[k++] = pts[i];
    }
    for (size_t i = pts.size() - 1, t = k + 1; i-- > 0;)
    {
        while (k >= t && cross(h[k - 1] - h[k - 2], pts[i] - h[k - 2]) <= 0.0)
            --k;
        h[k++] = pts[i];
    }
    h.resize(k - 1);
    return h;
}

/** Per-face values a Polys carries until it is appended: street, class, path, material (or block, material). */
using FaceTags = std::array<int32_t, 4>;

/** Polygons made apart from a mesh, so they can be made in parallel and appended in order. */
struct Polys
{
    std::vector<glm::dvec3> pts;
    std::vector<uint32_t> idx, sizes;
    std::vector<FaceTags> tags;

    uint32_t point(const glm::dvec3& p)
    {
        pts.push_back(p);
        return uint32_t(pts.size() - 1);
    }
    void face(std::initializer_list<uint32_t> ids, const FaceTags& t) { face(std::vector<uint32_t>(ids), t); }
    /** Quads and triangles go in as they are; larger polygons (clipped lots, junction hulls) as ear-clipped triangles. */
    void face(const std::vector<uint32_t>& ids, const FaceTags& t)
    {
        if (ids.size() <= 4)
        {
            raw(ids, t);
            return;
        }
        // Wound clockwise in XZ (normal up): an ear turns the same way as the whole.
        std::vector<uint32_t> ring = ids;
        auto turn = [&](uint32_t a, uint32_t b, uint32_t c) {
            const glm::dvec3 &p = pts[a], &q = pts[b], &r = pts[c];
            return (q.x - p.x) * (r.z - p.z) - (q.z - p.z) * (r.x - p.x);
        };
        double whole = 0.0;
        for (size_t k = 0; k < ring.size(); ++k)
            whole += pts[ring[k]].x * pts[ring[(k + 1) % ring.size()]].z - pts[ring[(k + 1) % ring.size()]].x * pts[ring[k]].z;
        const double sign = whole < 0.0 ? -1.0 : 1.0;
        size_t guard = ring.size() * ring.size();
        while (ring.size() > 3 && guard-- > 0)
        {
            bool cut = false;
            for (size_t k = 0; k < ring.size() && !cut; ++k)
            {
                const uint32_t a = ring[(k + ring.size() - 1) % ring.size()], b = ring[k], c = ring[(k + 1) % ring.size()];
                if (turn(a, b, c) * sign <= 0.0)
                    continue;
                bool inside = false;
                for (uint32_t q : ring)
                    if (q != a && q != b && q != c && turn(a, b, q) * sign > 0.0 && turn(b, c, q) * sign > 0.0 && turn(c, a, q) * sign > 0.0)
                    {
                        inside = true;
                        break;
                    }
                if (inside)
                    continue;
                raw({a, b, c}, t);
                ring.erase(ring.begin() + long(k));
                cut = true;
            }
            if (!cut)
                break;
        }
        // What is left (three corners, or a sliver no ear could be cut from) goes in as a fan.
        for (size_t k = 1; k + 1 < ring.size(); ++k)
            raw({ring[0], ring[k], ring[k + 1]}, t);
    }
    void raw(const std::vector<uint32_t>& ids, const FaceTags& t)
    {
        idx.insert(idx.end(), ids.begin(), ids.end());
        sizes.push_back(uint32_t(ids.size()));
        tags.push_back(t);
    }
    /** Adds the points and faces to the mesh, calling tag once per face in order. */
    void appendTo(geo::Mesh& m, const std::function<void(const FaceTags&)>& tag) const
    {
        if (sizes.empty())
            return;
        std::vector<geo::Index> ids(pts.size());
        for (size_t k = 0; k < pts.size(); ++k)
            ids[k] = m.addPoint(pts[k]);
        std::vector<geo::Index> f;
        for (size_t k = 0, off = 0; k < sizes.size(); off += sizes[k], ++k)
        {
            f.clear();
            for (uint32_t i = 0; i < sizes[k]; ++i)
                f.push_back(ids[idx[off + i]]);
            m.addFace(f);
            tag(tags[k]);
        }
    }
};

/** A simple polygon as triangles, counter-clockwise, by ear clipping; what no ear can be cut from goes in as a fan. */
std::vector<std::array<Vec2, 3>> earClip(std::vector<Vec2> ring)
{
    std::vector<std::array<Vec2, 3>> out;
    if (ring.size() < 3)
        return out;
    if (signedArea(ring) < 0.0)
        std::reverse(ring.begin(), ring.end());
    auto turn = [](Vec2 a, Vec2 b, Vec2 c) { return cross(b - a, c - a); };
    for (size_t guard = ring.size() * ring.size(); ring.size() > 3 && guard > 0; --guard)
    {
        bool cut = false;
        const size_t n = ring.size();
        for (size_t k = 0; k < n && !cut; ++k)
        {
            const Vec2 a = ring[(k + n - 1) % n], b = ring[k], c = ring[(k + 1) % n];
            const double t = turn(a, b, c);
            // A point that turns neither way (a run back along a cut, a straight side) is dropped without a triangle.
            if (std::abs(t) <= 1e-9 * (glm::dot(b - a, b - a) + glm::dot(c - b, c - b) + 1.0))
            {
                ring.erase(ring.begin() + long(k));
                cut = true;
                break;
            }
            if (t < 0.0)
                continue;
            bool inside = false;
            for (size_t i = 0; i < n && !inside; ++i)
            {
                const Vec2 q = ring[i];
                if (i == k || i == (k + 1) % n || i == (k + n - 1) % n || q == a || q == b || q == c)
                    continue;
                inside = turn(a, b, q) > 0.0 && turn(b, c, q) > 0.0 && turn(c, a, q) > 0.0;
            }
            if (inside)
                continue;
            out.push_back({a, b, c});
            ring.erase(ring.begin() + long(k));
            cut = true;
        }
        if (!cut)
            break;
    }
    for (size_t k = 1; k + 1 < ring.size(); ++k)
        if (turn(ring[0], ring[k], ring[k + 1]) > 0.0)
            out.push_back({ring[0], ring[k], ring[k + 1]});
    return out;
}

/**
 * Adds the polygon as faces cut on a grid, every corner draped by at, so a lot or block follows the ground under it
 * however large it is. The polygon is triangulated first and each triangle cut, so every piece is convex. On a lattice
 * ground the cuts are its own lines and diagonals: every piece then lies flat on one triangle of the surface drawn and
 * cannot dip under it; on other ground they are a grid of cell.
 */
void addDraped(Polys& m, const Polygon& p, double cell, const Ground::Lattice* grid, const std::function<glm::dvec3(Vec2)>& at,
               const FaceTags& tags)
{
    if (p.size() < 3)
        return;
    auto add = [&](const Polygon& piece) {
        if (piece.size() < 3 || area(piece) < 1.0)
            return;
        std::vector<uint32_t> ids;
        // Counter-clockwise in XZ faces down; the face is wound the other way so its normal points up.
        for (auto it = piece.pts.rbegin(); it != piece.pts.rend(); ++it)
            ids.push_back(m.point(at(*it)));
        m.face(ids, tags);
    };
    const Vec2 origin = grid ? grid->origin : Vec2(0.0), step = grid ? grid->cell : Vec2(cell);
    for (const std::array<Vec2, 3>& tri : earClip(p.pts))
    {
        Polygon t;
        t.pts.assign(tri.begin(), tri.end());
        t.tag.assign(3, -1);
        const Vec2 lo = glm::min(tri[0], glm::min(tri[1], tri[2])), hi = glm::max(tri[0], glm::max(tri[1], tri[2]));
        const int i0 = int(std::floor((lo.x - origin.x) / step.x)), i1 = int(std::ceil((hi.x - origin.x) / step.x));
        const int j0 = int(std::floor((lo.y - origin.y) / step.y)), j1 = int(std::ceil((hi.y - origin.y) / step.y));
        for (int i = i0; i < i1; ++i)
        {
            const double x0 = origin.x + i * step.x, x1 = x0 + step.x;
            Polygon col = x0 > lo.x ? clipHalfPlane(t, {x0, 0.0}, {1.0, 0.0}) : t;
            if (x1 < hi.x && col.size() >= 3)
                col = clipHalfPlane(col, {x1, 0.0}, {-1.0, 0.0});
            if (col.size() < 3)
                continue;
            for (int j = j0; j < j1; ++j)
            {
                const double z0 = origin.y + j * step.y, z1 = z0 + step.y;
                Polygon piece = z0 > lo.y ? clipHalfPlane(col, {0.0, z0}, {0.0, 1.0}) : col;
                if (z1 < hi.y && piece.size() >= 3)
                    piece = clipHalfPlane(piece, {0.0, z1}, {0.0, -1.0});
                if (piece.size() < 3)
                    continue;
                if (!grid)
                {
                    add(piece);
                    continue;
                }
                const Vec2 from = grid->mainDiagonal ? Vec2(x0, z0) : Vec2(x1, z0);
                const Vec2 n = grid->mainDiagonal ? Vec2(-step.y, step.x) : Vec2(-step.y, -step.x);
                add(clipHalfPlane(piece, from, n));
                add(clipHalfPlane(piece, from, -n));
            }
        }
    }
}

/** The ground a lot shows where no building stands, by its use; -1 when the block's own ground is enough. */
int32_t lotSurface(Use u)
{
    switch (u)
    {
    case Use::Park: return SurfGrass;
    case Use::Plaza:
    case Use::Market:
    case Use::Civic: return SurfPaving;
    case Use::Cemetery: return SurfCemetery;
    case Use::Open: return SurfDirt;
    case Use::Farm: return SurfField;
    case Use::Industrial:
    case Use::Port: return SurfConcrete;
    default: return -1;
    }
}
/** A block some of whose sides are not streets (the settlement's edge, water): open land lies past its lots. */
bool openBlock(const Block& b)
{
    return std::ranges::any_of(b.outline.tag, [](int t) { return t < 0; });
}

/**
 * The town ground of an area in an open block: what lies within reach of a street's edge or 2 m of a building, traced
 * on a grid of cell; not past a dead end, where the street runs out into open land. The area is taken two cells wider so its traced edge stays under the sidewalks it meets.
 */
std::vector<Polygon> nearStreets(const Polygon& area, const Layout& l, std::span<const int> degree, double reach,
                                 std::span<const Polygon* const> buildings, double cell)
{
    if (area.size() < 3)
        return {};
    Vec2 lo(std::numeric_limits<double>::max()), hi(-std::numeric_limits<double>::max());
    for (const Vec2& q : area.pts)
        lo = glm::min(lo, q), hi = glm::max(hi, q);
    Mask inside = Mask::around(lo, hi, cell, cell), near = inside;
    inside.fillPolygon(area.pts);
    inside.dilate(2.0 * cell);
    const StreetGraph& g = l.graph;
    for (size_t e = 0; e < g.edges.size(); ++e)
    {
        if (!g.isStreet(int(e)))
            continue;
        const Vec2 a = g.nodes[size_t(g.edges[e].a)].p, b = g.nodes[size_t(g.edges[e].b)].p;
        const double r = 0.5 * edgeWidth(l, int(e)) + reach, len = glm::length(b - a);
        if (len < 1e-6 || glm::min(a, b).x - r > hi.x || glm::max(a, b).x + r < lo.x || glm::min(a, b).y - r > hi.y || glm::max(a, b).y + r < lo.y)
            continue;
        const Vec2 d = (b - a) / len, n(-d.y, d.x);
        const std::array<Vec2, 4> band{a + n * r, a - n * r, b - n * r, b + n * r};
        near.fillPolygon(band);
        for (const int nd : {g.edges[e].a, g.edges[e].b})
        {
            const Vec2 c = g.nodes[size_t(nd)].p;
            const double rc = degree[size_t(nd)] > 1 ? r : 0.5 * edgeWidth(l, int(e));
            std::array<Vec2, 12> disk;
            for (size_t k = 0; k < disk.size(); ++k)
                disk[k] = c + Vec2(std::cos(k * kPi / 6.0), std::sin(k * kPi / 6.0)) * rc;
            near.fillPolygon(disk);
        }
    }
    for (const Polygon* fp : buildings)
        if (fp && fp->size() >= 3)
            if (const Polygon around = grow(*fp, 200.0); around.size() >= 3)
                near.fillPolygon(around.pts);
    for (size_t k = 0; k < near.on.size(); ++k)
        near.on[k] = near.on[k] && inside.on[k];
    std::vector<Polygon> out;
    for (const Polygon& p : near.outlines(1.5 * cell))
        if (Polygon q = makePolygon(p.pts); q.size() >= 3)
            out.push_back(std::move(q));
    return out;
}
} // namespace

std::span<const SurfaceMaterial> surfaceMaterials() { return kSurfaces; }

json metrics(const Layout& l)
{
    LILA_ZONE();

    const StreetGraph& g = l.graph;
    double bins[36] = {};
    double total = 0.0;
    for (size_t e = 0; e < g.edges.size(); ++e)
    {
        if (!g.isStreet(int(e)))
            continue;
        const Vec2 d = g.nodes[size_t(g.edges[e].b)].p - g.nodes[size_t(g.edges[e].a)].p;
        const double len = glm::length(d);
        double bearing = std::atan2(d.y, d.x) * 180.0 / kPi;
        for (double b : {bearing, bearing + 180.0})
        {
            const double w = std::fmod(b + 360.0, 360.0);
            bins[std::min(35, int(w / 10.0))] += len;
        }
        total += len;
    }
    // Boeing 2019: entropy over 36 bearings, from ln 4 (a perfect grid) to ln 36 (no order).
    double entropy = 0.0;
    for (double b : bins)
        if (b > 0.0)
        {
            const double p = b / (2.0 * total);
            entropy -= p * std::log(p);
        }
    const double hMax = std::log(36.0), hGrid = std::log(4.0);
    const double order = total > 0.0 ? std::clamp(1.0 - std::pow((entropy - hGrid) / (hMax - hGrid), 2.0), 0.0, 1.0) : 0.0;

    int counts[6] = {};
    int nodes = 0;
    for (size_t n = 0; n < g.nodes.size(); ++n)
    {
        const int d = g.degree(int(n));
        if (d == 0)
            continue;
        ++nodes;
        ++counts[std::min(d, 5)];
    }
    const double km2 = area(l.outline) / kM2 / 1.0e6;
    const int intersections = counts[3] + counts[4] + counts[5];
    std::vector<double> blockAreas;
    for (const Block& b : l.blocks)
        blockAreas.push_back(b.area);
    return {{"streetLengthKm", total / 1.0e5},
            {"streetDensityKmPerKm2", km2 > 0.0 ? total / 1.0e5 / km2 : 0.0},
            {"orientationEntropy", entropy},
            {"orientationOrder", order},
            {"nodes", nodes},
            {"intersections", intersections},
            {"intersectionsPerKm2", km2 > 0.0 ? intersections / km2 : 0.0},
            {"deadEndRatio", nodes > 0 ? double(counts[1]) / nodes : 0.0},
            {"degree", {{"1", counts[1]}, {"2", counts[2]}, {"3", counts[3]}, {"4", counts[4]}, {"5+", counts[5]}}},
            {"meanDegree", nodes > 0 ? double(counts[1] + 2 * counts[2] + 3 * counts[3] + 4 * counts[4] + 5 * counts[5]) / nodes : 0.0},
            {"blocks", int64_t(l.blocks.size())},
            {"blockAreaMedianM2", median(blockAreas)},
            {"blockAreaMeanM2", blockAreas.empty() ? 0.0 : std::accumulate(blockAreas.begin(), blockAreas.end(), 0.0) / double(blockAreas.size())}};
}

json checks(const Layout& l, const Ground& ground, const Options& o)
{
    LILA_ZONE();

    const StreetGraph& g = l.graph;
    const Profile& p = l.profile;
    std::vector<int> parent(g.nodes.size());
    std::iota(parent.begin(), parent.end(), 0);
    int steep = 0, crossings = 0;
    json crossingAt = json::array();
    for (size_t e = 0; e < g.edges.size(); ++e)
    {
        if (!g.isStreet(int(e)))
            continue;
        const StreetEdge& x = g.edges[e];
        parent[size_t(findRoot(parent, x.a))] = findRoot(parent, x.b);
        const Vec2 a = g.nodes[size_t(x.a)].p, b = g.nodes[size_t(x.b)].p;
        if (ground.gradeAlong(a, b) > p.maxGrade * 1.5 + 1e-9)
            ++steep;
        Vec2 hit{0.0};
        if (g.firstCrossing(a, b, &hit, nullptr, x.a, x.b) >= 0)
        {
            if (++crossings <= 8)
                crossingAt.push_back(xz(hit));
        }
    }
    std::map<int, int> roots;
    for (size_t n = 0; n < g.nodes.size(); ++n)
        if (g.degree(int(n)) > 0)
            ++roots[findRoot(parent, int(n))];

    int sharp = 0;
    for (size_t n = 0; n < g.nodes.size(); ++n)
    {
        std::vector<double> angles;
        for (int e : g.incident(int(n)))
        {
            if (!g.isStreet(e))
                continue;
            const StreetEdge& x = g.edges[size_t(e)];
            const Vec2 d = g.nodes[size_t(x.a == int(n) ? x.b : x.a)].p - g.nodes[n].p;
            angles.push_back(std::atan2(d.y, d.x));
        }
        if (angles.size() < 2)
            continue;
        std::sort(angles.begin(), angles.end());
        for (size_t k = 0; k < angles.size(); ++k)
        {
            double gap = (k + 1 < angles.size() ? angles[k + 1] : angles[0] + 2.0 * kPi) - angles[k];
            if (gap * 180.0 / kPi < p.minAngle * 0.5)
            {
                ++sharp;
                break;
            }
        }
    }
    int landlocked = 0, emptyEnvelopes = 0;
    for (const Lot& lot : l.lots)
        if (lot.frontages == 0 && lot.use != Use::Plaza)
            ++landlocked;
    for (const Envelope& env : l.envelopes)
        if (env.floors <= 0 || env.footprint.pts.size() < 3)
            ++emptyEnvelopes;
    // Anything inside an exclusion is a fault: streets through it, lots or buildings on it.
    auto inside = [&](Vec2 q) {
        for (const Polygon& x : l.exclusions)
            if (contains(x.pts, q) && boundaryDistance(x.pts, q) > 1.0)
                return true;
        return false;
    };
    int inExclusions = 0;
    for (size_t e = 0; e < g.edges.size(); ++e)
        if (g.isStreet(int(e)) && inside((g.nodes[size_t(g.edges[e].a)].p + g.nodes[size_t(g.edges[e].b)].p) * 0.5))
            ++inExclusions;
    for (const Lot& lot : l.lots)
        if (std::ranges::any_of(lot.outline.pts, inside))
            ++inExclusions;
    json failed = json::array();
    for (const PathResult& r : l.pathResults)
        if (!r.ok)
            failed.push_back({{"id", r.id}, {"error", r.error}});
    const int components = int(roots.size());
    return {{"components", components},
            {"crossings", crossings},
            {"crossingAt", crossingAt},
            {"steepStreets", steep},
            {"sharpJunctions", sharp},
            {"landlockedLots", landlocked},
            {"badEnvelopes", emptyEnvelopes},
            {"inExclusions", inExclusions},
            {"failedPaths", failed},
            {"staleOverrides", l.staleOverrides},
            {"warnings", l.streetWarnings},
            {"ok", components <= 1 && crossings == 0 && emptyEnvelopes == 0 && inExclusions == 0 && failed.empty() &&
                       (!o.blocks || !l.lots.empty())}};
}

json stats(const Layout& l)
{
    const StreetGraph& g = l.graph;
    json byClass = json::object();
    int streets = 0;
    for (size_t e = 0; e < g.edges.size(); ++e)
        if (g.isStreet(int(e)))
        {
            ++streets;
            const char* c = className(g.edges[e].cls);
            byClass[c] = byClass.value(c, 0.0) + g.length(int(e)) / 100.0;
        }
    json uses = json::object();
    for (const Lot& lot : l.lots)
        uses[useName(lot.use)] = uses.value(useName(lot.use), 0) + 1;
    int minF = 0, maxF = 0;
    double sumF = 0.0;
    for (const Envelope& env : l.envelopes)
    {
        minF = minF == 0 ? env.floors : std::min(minF, env.floors);
        maxF = std::max(maxF, env.floors);
        sumF += env.floors;
    }
    return {{"profile", l.profile.name},
            {"pattern", patternName(l.profile.pattern)},
            {"areaM2", area(l.outline) / kM2},
            {"radiusM", l.radius / 100.0},
            {"center", xz(l.center)},
            {"streetSegments", streets},
            {"streetMetresByClass", byClass},
            {"blocks", int64_t(l.blocks.size())},
            {"lots", int64_t(l.lots.size())},
            {"lotUses", uses},
            {"buildings", int64_t(l.envelopes.size())},
            {"floors", {{"min", minF}, {"max", maxF}, {"mean", l.envelopes.empty() ? 0.0 : sumF / double(l.envelopes.size())}}},
            {"refusedStreets", l.refused},
            {"bridges", int64_t(l.bridges.size())},
            {"exclusions", int64_t(l.exclusions.size())},
            {"surfacePaths", int64_t(l.surfacePaths.size())},
            {"paths", pathResults(l)}};
}

const char* useName(Use u)
{
    switch (u)
    {
    case Use::Residential: return "residential";
    case Use::Commercial: return "commercial";
    case Use::Civic: return "civic";
    case Use::Plaza: return "plaza";
    case Use::Open: return "open";
    case Use::Market: return "market";
    case Use::Religious: return "religious";
    case Use::Industrial: return "industrial";
    case Use::Port: return "port";
    case Use::Farm: return "farm";
    case Use::Park: return "park";
    case Use::Cemetery: return "cemetery";
    }
    return "open";
}

json toJson(const Layout& l)
{
    LILA_ZONE();

    const StreetGraph& g = l.graph;
    // Nodes renumbered over the ones streets use, so the file holds no dead entries.
    std::vector<int> remap(g.nodes.size(), -1);
    json nodes = json::array();
    json streets = json::array();
    for (size_t e = 0; e < g.edges.size(); ++e)
    {
        if (!g.isStreet(int(e)))
            continue;
        const StreetEdge& x = g.edges[e];
        for (int n : {x.a, x.b})
            if (remap[size_t(n)] < 0)
            {
                remap[size_t(n)] = int(nodes.size());
                nodes.push_back(xz(g.nodes[size_t(n)].p));
            }
        streets.push_back({{"a", remap[size_t(x.a)]},
                           {"b", remap[size_t(x.b)]},
                           {"class", className(x.cls)},
                           {"width", edgeWidth(l, int(e))},
                           {"profile", edgeProfile(l, int(e)).name},
                           {"path", x.path},
                           {"street", x.street},
                           {"era", x.era},
                           {"name", streetName(l, x.street)}});
    }
    json blocks = json::array();
    for (const Block& b : l.blocks)
    {
        blocks.push_back({{"id", b.id}, {"use", useName(b.use)}, {"areaM2", b.area}, {"outline", ring(b.outline)}});
        blocks.back().update(blockJson(b));
    }
    json lots = json::array();
    for (const Lot& lot : l.lots)
    {
        json sides = json::array();
        for (int t : lot.outline.tag)
            sides.push_back(t >= 0 ? json(className(StreetClass(t))) : json());
        lots.push_back({{"id", lot.id},
                        {"block", lot.block},
                        {"use", useName(lot.use)},
                        {"areaM2", lot.area},
                        {"frontages", lot.frontages},
                        {"corner", lot.corner},
                        {"frontClass", className(lot.frontClass)},
                        {"outline", ring(lot.outline)},
                        {"sideStreets", sides},
                        {"random", lot.random}});
        lots.back().update(lotExtras(lot));
    }
    json envelopes = json::array();
    for (const Envelope& env : l.envelopes)
    {
        json sides = json::array();
        for (int t : env.footprint.tag)
            sides.push_back(sideName(t));
        envelopes.push_back({{"lot", env.lot},
                             {"use", useName(env.use)},
                             {"footprint", ring(env.footprint)},
                             {"sides", sides},
                             {"floors", env.floors},
                             {"floorHeight", l.profile.floorHeight},
                             {"height", env.height},
                             {"base", env.base},
                             {"ground", env.ground},
                             {"top", env.top},
                             {"roof", env.roof},
                             {"front", xz(env.front)},
                             {"random", env.random}});
        envelopes.back().update(envelopeExtras(env));
    }
    json water = json::array();
    for (const WaterLine& w : l.water)
    {
        json pts = json::array();
        for (const Vec2& q : w.pts)
            pts.push_back(xz(q));
        water.push_back({{"width", w.width}, {"points", pts}});
    }
    json bridges = json::array();
    for (const Vec2& b : l.bridges)
        bridges.push_back(xz(b));
    json exclusions = json::array();
    for (size_t k = 0; k < l.exclusions.size(); ++k)
        exclusions.push_back({{"id", k < l.exclusionIds.size() ? l.exclusionIds[k] : -1}, {"outline", ring(l.exclusions[k])}});
    json surface = json::array();
    for (const SurfacePath& s : l.surfacePaths)
    {
        json pts = json::array();
        for (const glm::dvec3& q : s.pts)
            pts.push_back(json::array({q.x, q.y, q.z}));
        surface.push_back({{"path", s.path}, {"class", className(s.cls)}, {"profile", l.roads[size_t(s.profile)].name}, {"width", s.width}, {"points", pts}});
    }
    json roads = json::array();
    for (const RoadProfile& r : l.roads)
        roads.push_back(toJson(r));
    json districts = json::array();
    for (const District& d : l.districts)
        districts.push_back({{"id", d.id}, {"name", d.name}, {"areaM2", d.area}, {"center", xz(d.center)}, {"outline", ring(d.outline)}});
    json out = {{"format", "lila.settlement"},
            {"version", 1},
            {"units", "cm"},
            {"up", "y"},
            {"note", "2D points are [x, z]; heights are world Y."},
            {"profile", toJson(l.profile)},
            {"center", xz(l.center)},
            {"outline", ring(l.outline)},
            {"nodes", nodes},
            {"streets", streets},
            {"blocks", blocks},
            {"lots", lots},
            {"envelopes", envelopes},
            {"water", water},
            {"bridges", bridges},
            {"exclusions", exclusions},
            {"surfacePaths", surface},
            {"roadProfiles", roads},
            {"culture", l.culture},
            {"districts", districts}};
    if (hasOsmBuildings(l))
        out["osmSites"] = osmSitesJson(l);
    return out;
}

MeshParts toMeshes(const Layout& l, const Ground& ground, const OutputOptions& o)
{
    LILA_ZONE();

    MeshParts out;
    const StreetGraph& g = l.graph;
    // Draped no coarser than the ground's own grid, on its exact surface, so ribbons follow it at any scale.
    const double seg = std::max(std::min(o.segment, ground.isPlane() ? o.segment : std::max(ground.cellSize(), 50.0)), 10.0);
    auto at = [&](Vec2 p, double lift) { return glm::dvec3(p.x, ground.surfaceHeight(p) + lift, p.y); };

    // Each street is a carriageway between two sidewalks a curb above it, cut back where it meets other streets. Every
    // junction and bend gets a carriageway patch with rounded curb returns and a sidewalk corner round each return. All of
    // it is draped on the same ground at the same lifts, so the pieces meet edge to edge; faces are wound so their normals
    // face up (clockwise in XZ seen as x, z), curbs face the road and the sidewalks' outer skirts face away from it.
    // Draping is the cost (a raycast per corner), so each edge, junction, block and lot is made apart and appended in order.
    const bool medieval = medievalStyle(l);
    // The road stands a few cm clear of the ground: between draped points a curved terrain can rise over a flat face.
    const double skirtLift = o.lift * 0.25, roadLift = o.lift + kRoadRise, walkLift = roadLift + kCurbHeight;
    auto strip = [&](Polys& m, Vec2 a, Vec2 b, double left, double right, double lift, const FaceTags& tags) {
        const double len = glm::length(b - a);
        const Vec2 d = (b - a) / len, n(-d.y, d.x);
        const int steps = std::max(1, int(std::ceil(len / seg)));
        uint32_t prevL = 0, prevR = 0;
        for (int s = 0; s <= steps; ++s)
        {
            const Vec2 c = a + (b - a) * (double(s) / steps);
            const uint32_t L = m.point(at(c + n * left, lift)), R = m.point(at(c + n * right, lift));
            if (s > 0)
                m.face({prevL, L, R, prevR}, tags);
            prevL = L;
            prevR = R;
        }
    };
    struct NodeEnd
    {
        int edge = -1;
        int side = 0; // 0: the edge starts here, 1: it ends here
        Vec2 dir{0.0};
        double inner = 0.0, half = 0.0;
        double left = 0.0, right = 0.0; // how far out the sidewalk reaches on each side: inner where it has none
        int32_t surface = SurfAsphalt;
        StreetClass cls = StreetClass::Path;
    };
    // Per street, whether each side (0 left of its direction, 1 right) has a sidewalk.
    std::vector<std::array<bool, 2>> walkSides(g.edges.size(), {true, true});
    std::vector<std::vector<NodeEnd>> ends(g.nodes.size());
    auto makeEnds = [&] {
        for (std::vector<NodeEnd>& es : ends)
            es.clear();
        for (size_t e = 0; e < g.edges.size(); ++e)
        {
            const StreetEdge& x = g.edges[e];
            if (!g.isStreet(int(e)))
                continue;
            const Vec2 a = g.nodes[size_t(x.a)].p, b = g.nodes[size_t(x.b)].p;
            const double len = glm::length(b - a);
            if (len < 1e-6)
                continue;
            const double half = 0.5 * edgeWidth(l, int(e)), inner = half - walkWidth(l, int(e), 2.0 * half, medieval);
            const double out0 = walkSides[e][0] ? half : inner, out1 = walkSides[e][1] ? half : inner;
            const int32_t surface = streetSurface(l, x, (a + b) * 0.5, medieval);
            ends[size_t(x.a)].push_back({int(e), 0, (b - a) / len, inner, half, out0, out1, surface, x.cls});
            ends[size_t(x.b)].push_back({int(e), 1, (a - b) / len, inner, half, out1, out0, surface, x.cls});
        }
        for (std::vector<NodeEnd>& es : ends)
            std::sort(es.begin(), es.end(), [](const NodeEnd& p, const NodeEnd& q) { return std::atan2(p.dir.y, p.dir.x) < std::atan2(q.dir.y, q.dir.x); });
    };
    // A straight run of one width needs no patch: its strips meet end to end.
    auto plain = [](const std::vector<NodeEnd>& es) {
        return es.size() < 2 || (es.size() == 2 && glm::dot(es[0].dir, -es[1].dir) > std::cos(glm::radians(2.0)) &&
                                 std::abs(es[0].half - es[1].half) < 1.0 && std::abs(es[0].left - es[1].right) < 1.0 &&
                                 std::abs(es[0].right - es[1].left) < 1.0);
    };

    /** The corner from end a to the next end b counter-clockwise: where a's left curb and sidewalk lines meet b's right ones. */
    struct Corner
    {
        double gap = 0.0, radius = 0.0;
        bool lines = false, walks = false;
        Vec2 curb{0.0}, outer{0.0};
    };
    auto corner = [&](Vec2 c, const NodeEnd& a, const NodeEnd& b, double gap) {
        Corner k;
        k.gap = gap;
        k.walks = a.left - a.inner > 1.0 || b.right - b.inner > 1.0;
        k.lines = gap < kArcGap && cross(a.dir, b.dir) > 1e-3;
        if (k.lines)
        {
            const Vec2 na(-a.dir.y, a.dir.x), nb(-b.dir.y, b.dir.x);
            auto meet = [](Vec2 p, Vec2 u, Vec2 q, Vec2 v) { return p + u * (cross(q - p, v) / cross(u, v)); };
            k.curb = meet(c + na * a.inner, a.dir, c - nb * b.inner, b.dir);
            k.outer = meet(c + na * a.left, a.dir, c - nb * b.right, b.dir);
        }
        k.radius = k.lines && gap < kFilletGap && k.walks ? std::clamp(0.5 * std::min(a.inner, b.inner), 100.0, 600.0) : 0.0;
        return k;
    };
    auto gapAfter = [](const std::vector<NodeEnd>& es, size_t k) {
        const Vec2 a = es[k].dir, b = es[(k + 1) % es.size()].dir;
        double gap = std::atan2(b.y, b.x) - std::atan2(a.y, a.x);
        while (gap <= 0.0)
            gap += 2.0 * kPi;
        return gap;
    };
    // A street that ends within this of the settlement's outline runs on out of it as a dirt track; one ending inside
    // the town ends at a turning head.
    constexpr double kEdgeReach = 3000.0;
    auto runsOut = [&](Vec2 p) { return l.outline.size() >= 3 && (!contains(l.outline.pts, p) || boundaryDistance(l.outline.pts, p) < kEdgeReach); };
    /** cm: a turning head's carriageway radius on a street of this carriageway half-width. */
    auto headRadius = [](double inner) { return std::min(std::max(inner + 250.0, 1.6 * inner), inner + 1200.0); };
    // How far each street's strips stop short of its ends: past its curb returns and sidewalk corners.
    std::vector<std::array<double, 2>> trims(g.edges.size(), {0.0, 0.0});
    auto makeTrims = [&] {
        std::fill(trims.begin(), trims.end(), std::array<double, 2>{0.0, 0.0});
        tbb::parallel_for(tbb::blocked_range<size_t>(0, ends.size()), [&](const tbb::blocked_range<size_t>& range) {
            for (size_t nd = range.begin(); nd < range.end(); ++nd)
            {
                const std::vector<NodeEnd>& es = ends[nd];
                if (es.size() == 1 && !runsOut(g.nodes[nd].p))
                {
                    const double r = headRadius(es[0].inner);
                    trims[size_t(es[0].edge)][size_t(es[0].side)] = std::sqrt(r * r - es[0].inner * es[0].inner);
                    continue;
                }
                if (plain(es))
                    continue;
                const Vec2 c = g.nodes[nd].p;
                std::vector<double> trim(es.size(), 0.0);
                for (size_t k = 0; k < es.size(); ++k)
                {
                    const size_t k1 = (k + 1) % es.size();
                    const Corner cr = corner(c, es[k], es[k1], gapAfter(es, k));
                    if (!cr.lines)
                        continue;
                    const double tangent = cr.radius > 0.0 ? cr.radius / std::tan(0.5 * cr.gap) : 0.0;
                    trim[k] = std::max({trim[k], glm::dot(cr.curb - c, es[k].dir) + tangent, glm::dot(cr.outer - c, es[k].dir)});
                    trim[k1] = std::max({trim[k1], glm::dot(cr.curb - c, es[k1].dir) + tangent, glm::dot(cr.outer - c, es[k1].dir)});
                }
                for (size_t k = 0; k < es.size(); ++k)
                    trims[size_t(es[k].edge)][size_t(es[k].side)] = std::clamp(trim[k], 0.0, 3.0 * es[k].half + 600.0);
            }
        });
        // A street too short for both ends' trims gives each its share.
        for (size_t e = 0; e < g.edges.size(); ++e)
        {
            const StreetEdge& x = g.edges[e];
            const double len = glm::length(g.nodes[size_t(x.b)].p - g.nodes[size_t(x.a)].p), sum = trims[e][0] + trims[e][1];
            if (sum > len && sum > 0.0)
                trims[e] = {trims[e][0] * len / sum, trims[e][1] * len / sum};
        }
    };

    /** n + 1 points along a polyline, evenly by length. */
    auto resample = [](const std::vector<Vec2>& line, int n) {
        std::vector<double> at(line.size(), 0.0);
        for (size_t k = 1; k < line.size(); ++k)
            at[k] = at[k - 1] + glm::length(line[k] - line[k - 1]);
        std::vector<Vec2> out;
        size_t piece = 0;
        for (int i = 0; i <= n; ++i)
        {
            const double t = at.back() * i / n;
            while (piece + 2 < line.size() && at[piece + 1] < t)
                ++piece;
            const double span = at[piece + 1] - at[piece];
            out.push_back(span > 1e-9 ? glm::mix(line[piece], line[piece + 1], std::clamp((t - at[piece]) / span, 0.0, 1.0)) : line[piece]);
        }
        return out;
    };
    /** n + 1 points turning counter-clockwise round c from p to q (straight across when q is not ahead of p). */
    auto around = [](Vec2 c, Vec2 p, Vec2 q, int n) {
        const double a0 = std::atan2(p.y - c.y, p.x - c.x), r0 = glm::length(p - c), r1 = glm::length(q - c);
        double turn = std::atan2(q.y - c.y, q.x - c.x) - a0;
        while (turn > kPi)
            turn -= 2.0 * kPi;
        while (turn <= -kPi)
            turn += 2.0 * kPi;
        std::vector<Vec2> out;
        for (int i = 0; i <= n; ++i)
        {
            const double t = double(i) / n;
            out.push_back(turn > 1e-6 ? c + Vec2(std::cos(a0 + turn * t), std::sin(a0 + turn * t)) * glm::mix(r0, r1, t) : glm::mix(p, q, t));
        }
        return out;
    };
    /** A junction's plan: the carriageway outline, and per corner its curb and the sidewalk's outer edge, point for point. */
    struct NodeShape
    {
        std::vector<Vec2> outline;
        std::vector<size_t> startOf, curbStart;
        std::vector<std::vector<Vec2>> curbs, outers;
        std::vector<bool> walks;
    };
    auto shape = [&](size_t nd) {
        NodeShape s;
        const std::vector<NodeEnd>& es = ends[nd];
        const Vec2 c = g.nodes[nd].p;
        const size_t count = es.size();
        // Each street's mouth: its curb points right and left of its direction, and its sidewalks' outer edges.
        std::vector<Vec2> R(count), L(count), oR(count), oL(count);
        std::vector<double> trim(count);
        for (size_t k = 0; k < count; ++k)
        {
            const NodeEnd& x = es[k];
            trim[k] = trims[size_t(x.edge)][size_t(x.side)];
            const Vec2 n(-x.dir.y, x.dir.x), mouth = c + x.dir * trim[k];
            R[k] = mouth - n * x.inner;
            L[k] = mouth + n * x.inner;
            oR[k] = mouth - n * x.right;
            oL[k] = mouth + n * x.left;
        }
        s.curbs.resize(count);
        s.outers.resize(count);
        s.walks.resize(count);
        for (size_t k = 0; k < count; ++k)
        {
            const size_t k1 = (k + 1) % count;
            const Corner cr = corner(c, es[k], es[k1], gapAfter(es, k));
            s.walks[k] = cr.walks;
            if (!cr.lines)
            {
                const int n = std::clamp(int(std::ceil(glm::length(oR[k1] - oL[k]) / kCornerStep)), 1, 16);
                s.curbs[k] = around(c, L[k], R[k1], n);
                s.outers[k] = around(c, oL[k], oR[k1], n);
                continue;
            }
            std::vector<Vec2> line{L[k]};
            const double pk = glm::dot(cr.curb - c, es[k].dir), pk1 = glm::dot(cr.curb - c, es[k1].dir);
            const double tangent = cr.radius > 0.0 ? std::min({cr.radius / std::tan(0.5 * cr.gap), trim[k] - pk, trim[k1] - pk1}) : 0.0;
            if (tangent > 1.0)
            {
                const double r = tangent * std::tan(0.5 * cr.gap);
                const Vec2 ti = cr.curb + es[k].dir * tangent, tj = cr.curb + es[k1].dir * tangent;
                const Vec2 centre = cr.curb + glm::normalize(es[k].dir + es[k1].dir) * (r / std::sin(0.5 * cr.gap));
                const double a0 = std::atan2(ti.y - centre.y, ti.x - centre.x);
                double turn = std::atan2(tj.y - centre.y, tj.x - centre.x) - a0;
                while (turn > kPi)
                    turn -= 2.0 * kPi;
                while (turn <= -kPi)
                    turn += 2.0 * kPi;
                const int n = std::clamp(int(std::ceil(std::abs(turn) * r / kCornerStep)), 2, 12);
                for (int i = 0; i <= n; ++i)
                    line.push_back(centre + Vec2(std::cos(a0 + turn * i / n), std::sin(a0 + turn * i / n)) * r);
            }
            else if (pk < trim[k] && pk1 < trim[k1])
                line.push_back(cr.curb);
            line.push_back(R[k1]);
            double length = 0.0;
            for (size_t i = 1; i < line.size(); ++i)
                length += glm::length(line[i] - line[i - 1]);
            const int n = 2 * std::clamp(int(std::ceil(length / (2.0 * kCornerStep))), 1, 8);
            s.curbs[k] = resample(line, n);
            // The outer corner pairs with the middle of the curb, so the sidewalk keeps its width round the turn.
            if (glm::dot(cr.outer - c, es[k].dir) <= trim[k] + 1.0 && glm::dot(cr.outer - c, es[k1].dir) <= trim[k1] + 1.0)
            {
                s.outers[k] = resample({oL[k], cr.outer}, n / 2);
                const std::vector<Vec2> back = resample({cr.outer, oR[k1]}, n / 2);
                s.outers[k].insert(s.outers[k].end(), back.begin() + 1, back.end());
            }
            else
                s.outers[k] = resample({oL[k], oR[k1]}, n);
        }
        s.startOf.resize(count);
        s.curbStart.resize(count);
        for (size_t k = 0; k < count; ++k)
        {
            s.startOf[k] = s.outline.size();
            s.outline.push_back(R[k]);
            // The mouth takes the points the street's strip is cut across at, so the two share every edge.
            const int across = std::clamp(int(std::ceil(2.0 * es[k].inner / seg)), 1, 8);
            for (int j = across - 1; j >= 1; --j)
                s.outline.push_back(glm::mix(L[k], R[k], double(j) / across));
            s.curbStart[k] = s.outline.size();
            s.outline.insert(s.outline.end(), s.curbs[k].begin(), s.curbs[k].end() - 1);
        }
        return s;
    };

    // Where the carriageways lie: each street's strip and each junction's outline, binned for point tests.
    struct Area
    {
        std::vector<Vec2> ring;
        Vec2 lo{0.0}, hi{0.0};
    };
    std::vector<Area> areas;
    std::unordered_map<int64_t, std::vector<uint32_t>> areaBins;
    constexpr double kAreaCell = 2000.0;
    auto areaKey = [](int x, int z) { return (int64_t(x) << 32) ^ int64_t(uint32_t(z)); };
    auto makeAreas = [&](const std::vector<NodeShape>& shapes) {
        areas.clear();
        areaBins.clear();
        for (size_t e = 0; e < g.edges.size(); ++e)
        {
            const StreetEdge& x = g.edges[e];
            if (!g.isStreet(int(e)))
                continue;
            Vec2 a = g.nodes[size_t(x.a)].p, b = g.nodes[size_t(x.b)].p;
            const double len = glm::length(b - a);
            if (len < 1e-6 || len - trims[e][0] - trims[e][1] < 1.0)
                continue;
            const Vec2 d = (b - a) / len, n(-d.y, d.x);
            a += d * trims[e][0];
            b -= d * trims[e][1];
            const double half = 0.5 * edgeWidth(l, int(e)), inner = half - walkWidth(l, int(e), 2.0 * half, medieval);
            areas.push_back({{a + n * inner, b + n * inner, b - n * inner, a - n * inner}});
        }
        for (const NodeShape& s : shapes)
            if (s.outline.size() >= 3)
                areas.push_back({s.outline});
        for (size_t nd = 0; nd < ends.size(); ++nd)
            if (ends[nd].size() == 1 && !runsOut(g.nodes[nd].p))
            {
                Area head;
                const double r = headRadius(ends[nd][0].inner);
                for (int k = 0; k < 24; ++k)
                    head.ring.push_back(g.nodes[nd].p + Vec2(std::cos(k * kPi / 12.0), std::sin(k * kPi / 12.0)) * r);
                areas.push_back(std::move(head));
            }
        for (size_t i = 0; i < areas.size(); ++i)
        {
            Area& ar = areas[i];
            ar.lo = Vec2(std::numeric_limits<double>::max());
            ar.hi = Vec2(-std::numeric_limits<double>::max());
            for (const Vec2& p : ar.ring)
                ar.lo = glm::min(ar.lo, p), ar.hi = glm::max(ar.hi, p);
            for (int x = int(std::floor(ar.lo.x / kAreaCell)); x <= int(std::floor(ar.hi.x / kAreaCell)); ++x)
                for (int z = int(std::floor(ar.lo.y / kAreaCell)); z <= int(std::floor(ar.hi.y / kAreaCell)); ++z)
                    areaBins[areaKey(x, z)].push_back(uint32_t(i));
        }
    };
    auto onRoad = [&](Vec2 p) {
        const auto it = areaBins.find(areaKey(int(std::floor(p.x / kAreaCell)), int(std::floor(p.y / kAreaCell))));
        if (it == areaBins.end())
            return false;
        for (uint32_t i : it->second)
        {
            const Area& ar = areas[i];
            if (p.x < ar.lo.x || p.x > ar.hi.x || p.y < ar.lo.y || p.y > ar.hi.y)
                continue;
            bool in = false;
            for (size_t a = 0, b = ar.ring.size() - 1; a < ar.ring.size(); b = a++)
                if ((ar.ring[a].y > p.y) != (ar.ring[b].y > p.y) &&
                    p.x < (ar.ring[b].x - ar.ring[a].x) * (p.y - ar.ring[a].y) / (ar.ring[b].y - ar.ring[a].y) + ar.ring[a].x)
                    in = !in;
            if (in)
                return true;
        }
        return false;
    };
    /** Whether a piece of sidewalk reaches into a carriageway: its centre or a point halfway to any corner lies on one. */
    auto pieceOnRoad = [&](std::initializer_list<Vec2> corners) {
        Vec2 c(0.0);
        for (const Vec2& p : corners)
            c += p / double(corners.size());
        if (onRoad(c))
            return true;
        for (const Vec2& p : corners)
            if (onRoad(glm::mix(c, p, 0.5)))
                return true;
        return false;
    };
    std::vector<NodeShape> shapes(g.nodes.size());
    auto makeShapes = [&] {
        tbb::parallel_for(tbb::blocked_range<size_t>(0, ends.size()), [&](const tbb::blocked_range<size_t>& range) {
            for (size_t nd = range.begin(); nd < range.end(); ++nd)
                shapes[nd] = plain(ends[nd]) ? NodeShape{} : shape(nd);
        });
        makeAreas(shapes);
    };
    makeEnds();
    makeTrims();
    makeShapes();
    // A side whose sidewalk would lie on another carriageway (a divided road's inner sides, a sliver between streets) has none.
    bool dropped = false;
    for (size_t e = 0; e < g.edges.size(); ++e)
    {
        const StreetEdge& x = g.edges[e];
        if (!g.isStreet(int(e)))
            continue;
        Vec2 a = g.nodes[size_t(x.a)].p, b = g.nodes[size_t(x.b)].p;
        const double len = glm::length(b - a);
        const double half = 0.5 * edgeWidth(l, int(e)), inner = half - walkWidth(l, int(e), 2.0 * half, medieval);
        if (len < 1e-6 || half - inner < 1e-6 || len - trims[e][0] - trims[e][1] < 1.0)
            continue;
        const Vec2 d = (b - a) / len, n(-d.y, d.x);
        a += d * trims[e][0];
        b -= d * trims[e][1];
        const int samples = std::max(3, int(glm::length(b - a) / 200.0));
        for (int s = 0; s < 2; ++s)
        {
            int hit = 0;
            for (int i = 0; i < samples; ++i)
            {
                const Vec2 c = glm::mix(a, b, (i + 0.5) / samples);
                for (double off : {0.5 * (inner + half), half - 0.1 * (half - inner)})
                    hit += onRoad(c + n * (s == 0 ? off : -off)) ? 1 : 0;
            }
            if (hit > samples / 2)
            {
                walkSides[e][size_t(s)] = false;
                dropped = true;
            }
        }
    }
    if (dropped)
    {
        makeEnds();
        makeTrims();
        makeShapes();
    }

    /** Adds a face wound so its normal points along want (up when want is zero); slivers are dropped. */
    auto orient = [](Polys& m, std::vector<uint32_t> ids, Vec2 want, const FaceTags& t) {
        glm::dvec3 n(0.0);
        for (size_t k = 0; k < ids.size(); ++k)
        {
            const glm::dvec3 &a = m.pts[ids[k]], &b = m.pts[ids[(k + 1) % ids.size()]];
            n += glm::dvec3((a.y - b.y) * (a.z + b.z), (a.z - b.z) * (a.x + b.x), (a.x - b.x) * (a.y + b.y));
        }
        if (glm::length(n) < 1e-3)
            return;
        if ((want == Vec2(0.0) ? n.y : n.x * want.x + n.z * want.y) < 0.0)
            std::reverse(ids.begin(), ids.end());
        m.raw(ids, t);
    };
    std::vector<Polys> edgePolys(g.edges.size());
    tbb::parallel_for(tbb::blocked_range<size_t>(0, g.edges.size()), [&](const tbb::blocked_range<size_t>& range) {
        for (size_t e = range.begin(); e < range.end(); ++e)
        {
            const StreetEdge& x = g.edges[e];
            const bool water = x.alive && x.cls == StreetClass::Water;
            if (!g.isStreet(int(e)) && !water)
                continue;
            Vec2 a = g.nodes[size_t(x.a)].p, b = g.nodes[size_t(x.b)].p;
            const double len = glm::length(b - a);
            if (len < 1e-6)
                continue;
            const double half = 0.5 * edgeWidth(l, int(e));
            if (water)
            {
                strip(edgePolys[e], a, b, half, -half, o.lift * 0.5, {int32_t(e), 0, 0, 0});
                continue;
            }
            if (len - trims[e][0] - trims[e][1] < 1.0)
                continue;
            const Vec2 d = (b - a) / len, n(-d.y, d.x);
            a += d * trims[e][0];
            b -= d * trims[e][1];
            const double inner = half - walkWidth(l, int(e), 2.0 * half, medieval);
            const bool walk = half - inner > 1e-6;
            const FaceTags road{int32_t(e), int32_t(x.cls), int32_t(x.path), streetSurface(l, x, (a + b) * 0.5, medieval)};
            const FaceTags side{road[0], road[1], road[2], SurfSidewalk}, curb{road[0], road[1], road[2], SurfConcrete};
            Polys& m = edgePolys[e];
            const int steps = std::max(1, int(std::ceil(glm::length(b - a) / seg)));
            // Per station: the curb lines at road level, then each side's curb and outer edge at sidewalk level and its foot.
            // The carriageway is cut across too, so a wide one follows the ground under its middle.
            const int across = std::clamp(int(std::ceil(2.0 * inner / seg)), 1, 8);
            std::array<uint32_t, 8> prev{}, cur{};
            std::vector<uint32_t> prevRoad(size_t(across) + 1), curRoad(size_t(across) + 1);
            auto pt = [&](Vec2 p, double h) { return m.point(glm::dvec3(p.x, h, p.y)); };
            Vec2 prevC = a;
            for (int s = 0; s <= steps; ++s)
            {
                const Vec2 c = a + (b - a) * (double(s) / steps);
                const double hl = ground.surfaceHeight(c + n * inner), hr = ground.surfaceHeight(c - n * inner);
                cur[0] = curRoad.front() = pt(c + n * inner, hl + roadLift);
                cur[1] = curRoad.back() = pt(c - n * inner, hr + roadLift);
                for (int j = 1; j < across; ++j)
                    curRoad[size_t(j)] = m.point(at(c + n * (inner * (1.0 - 2.0 * j / across)), roadLift));
                if (walk)
                {
                    const double ol = ground.surfaceHeight(c + n * half), orr = ground.surfaceHeight(c - n * half);
                    cur[2] = pt(c + n * inner, hl + walkLift);
                    cur[3] = pt(c + n * half, ol + walkLift);
                    cur[4] = pt(c + n * half, ol + skirtLift);
                    cur[5] = pt(c - n * inner, hr + walkLift);
                    cur[6] = pt(c - n * half, orr + walkLift);
                    cur[7] = pt(c - n * half, orr + skirtLift);
                }
                if (s > 0)
                {
                    for (size_t j = 0; j < size_t(across); ++j)
                        m.face({prevRoad[j], curRoad[j], curRoad[j + 1], prevRoad[j + 1]}, road);
                    if (walk && walkSides[e][0] && !pieceOnRoad({prevC + n * inner, c + n * inner, c + n * half, prevC + n * half}))
                    {
                        m.face({prev[3], cur[3], cur[2], prev[2]}, side);
                        orient(m, {prev[0], cur[0], cur[2], prev[2]}, -n, curb);
                        orient(m, {prev[3], cur[3], cur[4], prev[4]}, n, side);
                    }
                    if (walk && walkSides[e][1] && !pieceOnRoad({prevC - n * inner, c - n * inner, c - n * half, prevC - n * half}))
                    {
                        m.face({prev[5], cur[5], cur[6], prev[6]}, side);
                        orient(m, {prev[1], cur[1], cur[5], prev[5]}, n, curb);
                        orient(m, {prev[6], cur[6], cur[7], prev[7]}, -n, side);
                    }
                }
                prev = cur;
                prevC = c;
                std::swap(prevRoad, curRoad);
            }
        }
    });
    /** A fan from c over the outline (counter-clockwise or not), cut in rings no wider than the drape step. */
    auto fan = [&](Polys& m, Vec2 c, const std::vector<Vec2>& outline, const std::vector<uint32_t>& outlineIds, double lift, const FaceTags& t) {
        double reach = 0.0;
        for (const Vec2& p : outline)
            reach = std::max(reach, glm::length(p - c));
        const int rings = std::clamp(int(std::ceil(reach / seg)), 1, 8);
        const uint32_t centre = m.point(at(c, lift));
        std::vector<std::vector<uint32_t>> spokes(outline.size());
        for (size_t i = 0; i < outline.size(); ++i)
        {
            spokes[i].push_back(centre);
            for (int r = 1; r < rings; ++r)
                spokes[i].push_back(m.point(at(glm::mix(c, outline[i], double(r) / rings), lift)));
            spokes[i].push_back(outlineIds[i]);
        }
        for (size_t i = 0; i < outline.size(); ++i)
        {
            const std::vector<uint32_t>&p = spokes[i], &q = spokes[(i + 1) % outline.size()];
            orient(m, {p[0], p[1], q[1]}, Vec2(0.0), t);
            for (size_t r = 1; r < size_t(rings); ++r)
                orient(m, {p[r], p[r + 1], q[r + 1], q[r]}, Vec2(0.0), t);
        }
    };
    /**
     * A street's open end: a dirt track running on out of the settlement, its sidewalks closed across their ends; or, inside
     * the town, a turning head with its sidewalk round it.
     */
    auto deadEnd = [&](Polys& m, size_t nd) {
        const NodeEnd& x = ends[nd][0];
        const Vec2 c = g.nodes[nd].p, n(-x.dir.y, x.dir.x);
        const FaceTags tag{int32_t(x.edge), int32_t(x.cls), -1, x.surface};
        const FaceTags side{tag[0], tag[1], -1, SurfSidewalk}, curb{tag[0], tag[1], -1, SurfConcrete}, dirt{tag[0], tag[1], -1, SurfDirt};
        const int across = std::clamp(int(std::ceil(2.0 * x.inner / seg)), 1, 8);
        auto lifted = [&](Vec2 p, double lift) { return m.point(glm::dvec3(p.x, ground.surfaceHeight(p) + lift, p.y)); };
        if (runsOut(c))
        {
            for (const double s : {1.0, -1.0})
            {
                const double reach = s > 0.0 ? x.left : x.right;
                if (reach - x.inner < 1.0)
                    continue;
                const Vec2 pi = c + n * (s * x.inner), po = c + n * (s * reach);
                orient(m, {lifted(pi, roadLift), lifted(pi, walkLift), lifted(po, walkLift), lifted(po, skirtLift)}, -x.dir, curb);
            }
            // The track narrows from the carriageway to a lane and comes down from the road to the ground.
            const double length = 2000.0, endHalf = std::max(150.0, 0.5 * x.inner);
            const int steps = std::max(2, int(std::ceil(length / seg)));
            std::vector<uint32_t> prev, cur;
            for (int s = 0; s <= steps; ++s)
            {
                const double u = double(s) / steps, half = glm::mix(x.inner, endHalf, u), lift = glm::mix(roadLift, o.lift, u);
                const Vec2 mid = c - x.dir * (length * u);
                cur.clear();
                for (int j = 0; j <= across; ++j)
                {
                    const Vec2 p = mid + n * (half * (1.0 - 2.0 * j / across));
                    cur.push_back(j == 0 || j == across ? lifted(p, lift) : m.point(at(p, lift)));
                }
                cur.push_back(lifted(mid + n * half, skirtLift));
                cur.push_back(lifted(mid - n * half, skirtLift));
                if (s > 0)
                {
                    for (size_t j = 0; j < size_t(across); ++j)
                        orient(m, {prev[j], cur[j], cur[j + 1], prev[j + 1]}, Vec2(0.0), dirt);
                    orient(m, {prev[0], cur[0], cur[size_t(across) + 1], prev[size_t(across) + 1]}, n, dirt);
                    orient(m, {prev[size_t(across)], cur[size_t(across)], cur[size_t(across) + 2], prev[size_t(across) + 2]}, -n, dirt);
                }
                std::swap(prev, cur);
            }
            return;
        }
        // The head: a disc round the node meeting the street's strip where its curb lines cross the circle.
        const double r = headRadius(x.inner), t = trims[size_t(x.edge)][size_t(x.side)];
        const Vec2 mouth = c + x.dir * t, L = mouth + n * x.inner, R = mouth - n * x.inner;
        const double a0 = std::atan2(L.y - c.y, L.x - c.x);
        double turn = std::atan2(R.y - c.y, R.x - c.x) - a0;
        while (turn <= 0.0)
            turn += 2.0 * kPi;
        const int count = std::clamp(int(std::ceil(turn * r / kCornerStep)), 6, 32);
        std::vector<Vec2> arc, outline;
        for (int i = 0; i <= count; ++i)
            arc.push_back(i == 0 ? L : i == count ? R : c + Vec2(std::cos(a0 + turn * i / count), std::sin(a0 + turn * i / count)) * r);
        outline = arc;
        for (int j = across - 1; j >= 1; --j)
            outline.push_back(glm::mix(L, R, double(j) / across));
        std::vector<uint32_t> ids;
        for (size_t i = 0; i < outline.size(); ++i)
            ids.push_back(i == 0 || i == size_t(count) ? lifted(outline[i], roadLift) : m.point(at(outline[i], roadLift)));
        fan(m, c, outline, ids, roadLift, tag);
        const double walk = std::max(x.left, x.right) - x.inner;
        if (walk < 1.0)
            return;
        for (int i = 0; i < count; ++i)
        {
            const Vec2 in0 = arc[size_t(i)], in1 = arc[size_t(i) + 1];
            auto outer = [&](int k) {
                return k == 0 ? mouth + n * x.left : k == count ? mouth - n * x.right : c + glm::normalize(arc[size_t(k)] - c) * (r + walk);
            };
            const Vec2 out0 = outer(i), out1 = outer(i + 1);
            if (pieceOnRoad({in0, out0, out1, in1}))
                continue;
            const uint32_t w0 = lifted(in0, walkLift), w1 = lifted(in1, walkLift), o0 = lifted(out0, walkLift), o1 = lifted(out1, walkLift);
            orient(m, {w0, o0, o1, w1}, Vec2(0.0), side);
            orient(m, {lifted(in0, roadLift), lifted(in1, roadLift), w1, w0}, c - 0.5 * (in0 + in1), curb);
            orient(m, {o0, o1, lifted(out1, skirtLift), lifted(out0, skirtLift)}, 0.5 * (out0 + out1) - c, side);
        }
    };
    std::vector<Polys> nodePolys(g.nodes.size());
    tbb::parallel_for(tbb::blocked_range<size_t>(0, ends.size()), [&](const tbb::blocked_range<size_t>& range) {
        for (size_t nd = range.begin(); nd < range.end(); ++nd)
        {
            const std::vector<NodeEnd>& es = ends[nd];
            if (es.size() == 1)
            {
                deadEnd(nodePolys[nd], nd);
                continue;
            }
            if (plain(es))
                continue;
            const NodeShape& s = shapes[nd];
            const Vec2 c = g.nodes[nd].p;
            const NodeEnd* top = &es[0];
            for (const NodeEnd& x : es)
                if (x.cls < top->cls)
                    top = &x;
            const FaceTags road{-1, int32_t(top->cls), -1, top->surface}, side{-1, int32_t(top->cls), -1, SurfSidewalk},
                curb{-1, int32_t(top->cls), -1, SurfConcrete};
            const size_t count = es.size();
            Polys& m = nodePolys[nd];
            // The carriageway: a fan from the centre over its outline, cut in rings so it follows the ground.
            std::vector<uint32_t> outlineIds;
            double reach = 0.0;
            for (const Vec2& p : s.outline)
            {
                outlineIds.push_back(m.point(at(p, roadLift)));
                reach = std::max(reach, glm::length(p - c));
            }
            std::vector<std::vector<uint32_t>> curbIds(count);
            for (size_t k = 0; k < count; ++k)
            {
                for (size_t i = 0; i + 1 < s.curbs[k].size(); ++i)
                    curbIds[k].push_back(outlineIds[s.curbStart[k] + i]);
                curbIds[k].push_back(outlineIds[s.startOf[(k + 1) % count]]);
            }
            const int rings = std::clamp(int(std::ceil(reach / seg)), 1, 8);
            const uint32_t centre = m.point(at(c, roadLift));
            std::vector<std::vector<uint32_t>> spokes(s.outline.size());
            for (size_t i = 0; i < s.outline.size(); ++i)
            {
                spokes[i].push_back(centre);
                for (int r = 1; r < rings; ++r)
                    spokes[i].push_back(m.point(at(glm::mix(c, s.outline[i], double(r) / rings), roadLift)));
                spokes[i].push_back(outlineIds[i]);
            }
            for (size_t i = 0; i < s.outline.size(); ++i)
            {
                const std::vector<uint32_t>&p = spokes[i], &q = spokes[(i + 1) % s.outline.size()];
                orient(m, {p[0], p[1], q[1]}, Vec2(0.0), road);
                for (size_t r = 1; r < size_t(rings); ++r)
                    orient(m, {p[r], p[r + 1], q[r + 1], q[r]}, Vec2(0.0), road);
            }
            // Sidewalk corners: the paving between curb and outer edge, the curb face and the outer skirt; a piece that
            // would reach into a carriageway (a sharp corner's point, an island narrower than its sidewalks) is left out.
            for (size_t k = 0; k < count; ++k)
            {
                if (!s.walks[k])
                    continue;
                const std::vector<Vec2>&in = s.curbs[k], &out = s.outers[k];
                std::vector<uint32_t> walkIn, walkOut, foot;
                for (size_t i = 0; i < in.size(); ++i)
                {
                    walkIn.push_back(m.point(at(in[i], walkLift)));
                    const double h = ground.surfaceHeight(out[i]);
                    walkOut.push_back(m.point(glm::dvec3(out[i].x, h + walkLift, out[i].y)));
                    foot.push_back(m.point(glm::dvec3(out[i].x, h + skirtLift, out[i].y)));
                }
                for (size_t i = 0; i + 1 < in.size(); ++i)
                {
                    if (pieceOnRoad({in[i], out[i], out[i + 1], in[i + 1]}))
                        continue;
                    orient(m, {walkIn[i], walkOut[i], walkOut[i + 1], walkIn[i + 1]}, Vec2(0.0), side);
                    const Vec2 along = in[i + 1] - in[i], ahead = out[i + 1] - out[i];
                    orient(m, {curbIds[k][i], curbIds[k][i + 1], walkIn[i + 1], walkIn[i]}, Vec2(-along.y, along.x), curb);
                    orient(m, {walkOut[i], walkOut[i + 1], foot[i + 1], foot[i]}, Vec2(ahead.y, -ahead.x), side);
                }
            }
        }
    });
    // Islands: faces of the street graph too small for a block (between a divided road's carriageways, inside a cluster of
    // short streets) are raised to the sidewalks out to the curb lines, so no ground shows in the road; planted where they
    // are 3 m or more across.
    const std::vector<Face> faces = planarFaces(g);
    // Just under the sidewalks, so an island's own sidewalk ring draws over its fill.
    const double islandLift = walkLift - 1.5;
    std::vector<Polys> islandPolys(faces.size());
    tbb::parallel_for(tbb::blocked_range<size_t>(0, faces.size()), [&](const tbb::blocked_range<size_t>& range) {
        for (size_t k = range.begin(); k < range.end(); ++k)
        {
            const Polygon& f = faces[k].outline;
            std::vector<double> full(f.size()), curbs(f.size());
            bool streets = true;
            for (size_t i = 0; i < f.size() && streets; ++i)
            {
                const int e = f.tag[i];
                streets = g.isStreet(e);
                full[i] = streets ? 0.5 * edgeWidth(l, e) : 0.0;
                curbs[i] = full[i] - (streets ? walkWidth(l, e, 2.0 * full[i], medieval) : 0.0);
            }
            if (!streets)
                continue;
            if (const Polygon block = inset(f, full); block.size() >= 3 && area(block) >= l.profile.minLot * kM2)
                continue;
            const Polygon island = inset(f, curbs);
            if (island.size() < 3 || area(island) < 0.5 * kM2)
                continue;
            const std::vector<double> margin(island.size(), 150.0);
            const Polygon core = inset(island, margin);
            const FaceTags top{-1, int32_t(StreetClass::Local), -1, core.size() >= 3 && area(core) > kM2 ? SurfGrass : SurfSidewalk};
            const FaceTags curb{-1, int32_t(StreetClass::Local), -1, SurfConcrete};
            Polys& m = islandPolys[k];
            addDraped(m, island, std::max(2.0 * seg, 400.0), ground.lattice(), [&](Vec2 q) { return at(q, islandLift); }, top);
            for (size_t i = 0; i < island.size(); ++i)
            {
                const Vec2 a = island.pts[i], b = island.pts[(i + 1) % island.size()];
                const Vec2 d = b - a;
                const int steps = std::max(1, int(std::ceil(glm::length(d) / seg)));
                for (int s = 0; s < steps; ++s)
                {
                    const Vec2 p = a + d * (double(s) / steps), q = a + d * (double(s + 1) / steps);
                    const uint32_t p0 = m.point(at(p, roadLift)), q0 = m.point(at(q, roadLift)), p1 = m.point(at(p, islandLift)), q1 = m.point(at(q, islandLift));
                    orient(m, {p0, q0, q1, p1}, Vec2(d.y, -d.x), curb);
                }
            }
        }
    });
    std::vector<int32_t> streetId, streetClass, streetPath, streetMat, waterId;
    for (size_t e = 0; e < edgePolys.size(); ++e)
    {
        const bool water = g.edges[e].alive && g.edges[e].cls == StreetClass::Water;
        edgePolys[e].appendTo(water ? out.water : out.streets, [&](const FaceTags& t) {
            if (water)
            {
                waterId.push_back(t[0]);
                return;
            }
            streetId.push_back(t[0]);
            streetClass.push_back(t[1]);
            streetPath.push_back(t[2]);
            streetMat.push_back(t[3]);
        });
    }
    edgePolys = {};
    for (const Polys& p : nodePolys)
        p.appendTo(out.streets, [&](const FaceTags& t) {
            streetId.push_back(t[0]);
            streetClass.push_back(t[1]);
            streetPath.push_back(t[2]);
            streetMat.push_back(t[3]);
        });
    nodePolys = {};
    for (const Polys& p : islandPolys)
        p.appendTo(out.streets, [&](const FaceTags& t) {
            streetId.push_back(t[0]);
            streetClass.push_back(t[1]);
            streetPath.push_back(t[2]);
            streetMat.push_back(t[3]);
        });
    islandPolys = {};
    // Surface paths: ribbons on the walkable surface, level with each point, -1 for the street id.
    for (const SurfacePath& s : l.surfacePaths)
    {
        const double half = 0.5 * s.width;
        geo::Index prevL = 0, prevR = 0;
        for (size_t k = 0; k < s.pts.size(); ++k)
        {
            const glm::dvec3 prev = s.pts[k > 0 ? k - 1 : k], next = s.pts[k + 1 < s.pts.size() ? k + 1 : k];
            glm::dvec2 d(next.x - prev.x, next.z - prev.z);
            d = glm::length(d) > 1e-9 ? d / glm::length(d) : glm::dvec2(1.0, 0.0);
            const glm::dvec3 side(-d.y * half, 0.0, d.x * half), up(0.0, o.lift, 0.0);
            const geo::Index L = out.streets.addPoint(s.pts[k] + side + up);
            const geo::Index R = out.streets.addPoint(s.pts[k] - side + up);
            if (k > 0)
            {
                out.streets.addFace({prevL, L, R, prevR});
                streetId.push_back(-1);
                streetClass.push_back(int32_t(s.cls));
                streetPath.push_back(int32_t(s.path));
                streetMat.push_back(SurfPaving);
            }
            prevL = L;
            prevR = R;
        }
    }
    setFaceInts(out.streets, {{"street", streetId}, {"class", streetClass}, {"path", streetPath}, {"material", streetMat}});
    setFaceInts(out.water, {{"water", waterId}});
    for (const SurfaceMaterial& s : kSurfaces)
        out.streets.materialNames().push_back(s.name);

    // Ground: each block's yards, park or square, then the lots whose use shows its own ground (vacant lots, parks,
    // plazas, works yards, fields, graveyards), a little above it.
    if (o.ground)
    {
        const Ground::Lattice* grid = ground.lattice();
        const double cell = ground.isPlane() ? std::max(4.0 * seg, 1200.0) : std::max(2.0 * seg, 400.0);
        auto lay = [&](Polys& m, const Polygon& p, double lift, int32_t block, int32_t mat) {
            addDraped(m, p, cell, grid, [&](Vec2 q) { return at(q, lift); }, {block, mat, 0, 0});
        };
        auto blockSurface = [](const Block& b) {
            return b.use == Use::Park ? SurfGrass : b.use == Use::Plaza ? SurfPaving : b.use == Use::Farm ? SurfField : SurfYard;
        };
        // An open block shows town ground only within reach of its streets and buildings: past them is open land.
        std::vector<std::vector<const Polygon*>> buildingsIn(l.blocks.size());
        for (const Envelope& env : l.envelopes)
            if (env.lot >= 0 && size_t(env.lot) < l.lots.size() && size_t(l.lots[size_t(env.lot)].block) < l.blocks.size())
                buildingsIn[size_t(l.lots[size_t(env.lot)].block)].push_back(&env.footprint);
        const double reach = std::max(l.profile.lotDepth, 500.0), traceCell = 50.0;
        std::vector<int> degree(g.nodes.size(), 0);
        for (size_t e = 0; e < g.edges.size(); ++e)
            if (g.isStreet(int(e)))
                ++degree[size_t(g.edges[e].a)], ++degree[size_t(g.edges[e].b)];
        std::vector<Polys> blockPolys(l.blocks.size()), lotPolys(l.lots.size());
        tbb::parallel_for(tbb::blocked_range<size_t>(0, l.blocks.size()), [&](const tbb::blocked_range<size_t>& range) {
            for (size_t k = range.begin(); k < range.end(); ++k)
            {
                const Block& b = l.blocks[k];
                if (!openBlock(b))
                    lay(blockPolys[k], b.outline, o.lift, b.id, blockSurface(b));
                else
                    for (const Polygon& p : nearStreets(b.outline, l, degree, reach, buildingsIn[k], traceCell))
                        lay(blockPolys[k], p, o.lift, b.id, blockSurface(b));
            }
        });
        tbb::parallel_for(tbb::blocked_range<size_t>(0, l.lots.size()), [&](const tbb::blocked_range<size_t>& range) {
            for (size_t k = range.begin(); k < range.end(); ++k)
            {
                const Lot& lot = l.lots[k];
                const int32_t own = lotSurface(lot.use);
                if (own < 0 || size_t(lot.block) >= l.blocks.size())
                    continue;
                const Block& b = l.blocks[size_t(lot.block)];
                if (!openBlock(b))
                {
                    if (lotSurface(b.use) != own)
                        lay(lotPolys[k], lot.outline, o.lift * 1.5, lot.block, own);
                    continue;
                }
                for (const Polygon& p : nearStreets(lot.outline, l, degree, reach, buildingsIn[size_t(lot.block)], traceCell))
                    lay(lotPolys[k], p, o.lift * 1.5, lot.block, own);
            }
        });
        std::vector<int32_t> groundBlock, groundMat;
        auto tags = [&](const FaceTags& t) {
            groundBlock.push_back(t[0]);
            groundMat.push_back(t[1]);
        };
        for (const Polys& p : blockPolys)
            p.appendTo(out.ground, tags);
        for (const Polys& p : lotPolys)
            p.appendTo(out.ground, tags);
        setFaceInts(out.ground, {{"block", groundBlock}, {"material", groundMat}});
        for (const SurfaceMaterial& s : kSurfaces)
            out.ground.materialNames().push_back(s.name);
    }

    // Lots: outlines as loose edges draped on the ground.
    std::vector<int32_t> edgeLot;
    for (const Lot& lot : o.lots ? std::span<const Lot>(l.lots) : std::span<const Lot>())
    {
        const size_t n = lot.outline.size();
        std::vector<geo::Index> ids;
        for (size_t k = 0; k < n; ++k)
        {
            const Vec2 a = lot.outline.pts[k], b = lot.outline.pts[(k + 1) % n];
            const int steps = std::max(1, int(std::ceil(glm::length(b - a) / seg)));
            for (int s = 0; s < steps; ++s)
                ids.push_back(out.lots.addPoint(at(a + (b - a) * (double(s) / steps), o.lift * 1.5)));
        }
        for (size_t k = 0; k < ids.size(); ++k)
        {
            out.lots.addEdge(ids[k], ids[(k + 1) % ids.size()]);
            edgeLot.push_back(lot.id);
        }
    }
    out.lots.edgeAttrs().add("lot", geo::AttrType::Int);
    {
        std::span<int32_t> v = out.lots.edgeAttrs().get<int32_t>("lot");
        std::copy(edgeLot.begin(), edgeLot.begin() + std::min(edgeLot.size(), v.size()), v.begin());
    }

    // Envelopes: closed prisms from the foundation to the top, walls facing out, roof up, floor down.
    std::vector<int32_t> envLot, envUse, envFloors, envPart;
    std::vector<int> flatRoofs;
    for (const Envelope& env : o.envelopes ? std::span<const Envelope>(l.envelopes) : std::span<const Envelope>())
    {
        // The Buildings extension's live buildings stand there instead.
        if (env.route == BuildingRoute::Buildings)
            continue;
        if (env.route == BuildingRoute::Massing)
        {
            if (!appendMassing(env, l, out.envelopes, [&](int part) {
                    envLot.push_back(env.lot);
                    envUse.push_back(int32_t(env.use));
                    envFloors.push_back(env.floors);
                    envPart.push_back(part);
                }))
                flatRoofs.push_back(env.lot);
            continue;
        }
        const size_t n = env.footprint.size();
        std::vector<geo::Index> lo, hi;
        for (const Vec2& q : env.footprint.pts)
        {
            lo.push_back(out.envelopes.addPoint({q.x, env.base, q.y}));
            hi.push_back(out.envelopes.addPoint({q.x, env.top, q.y}));
        }
        auto tagFace = [&](int part) {
            envLot.push_back(env.lot);
            envUse.push_back(int32_t(env.use));
            envFloors.push_back(env.floors);
            envPart.push_back(part);
        };
        for (size_t k = 0; k < n; ++k)
        {
            const size_t j = (k + 1) % n;
            out.envelopes.addFace({lo[k], hi[k], hi[j], lo[j]});
            tagFace(0);
        }
        std::vector<geo::Index> top(hi.rbegin(), hi.rend());
        out.envelopes.addFace(top);
        tagFace(1);
        out.envelopes.addFace(lo);
        tagFace(2);
    }
    setFaceInts(out.envelopes, {{"lot", envLot}, {"use", envUse}, {"floors", envFloors}, {"part", envPart}});
    if (!flatRoofs.empty())
    {
        std::string lots;
        for (size_t k = 0; k < flatRoofs.size() && k < 20; ++k)
            lots += (k ? ", " : "") + std::to_string(flatRoofs[k]);
        out.warnings.push_back(std::format("{} massing roofs are flat caps: the straight skeleton found no roof on their footprints (lots {}{})", flatRoofs.size(), lots, flatRoofs.size() > 20 ? ", ..." : ""));
    }
    return out;
}

std::vector<StreetLine> streetLines(const Layout& l, const Ground& ground)
{
    const StreetGraph& g = l.graph;
    std::vector<char> used(g.edges.size(), 0);
    std::vector<StreetLine> lines;
    auto same = [&](int a, int b) {
        const StreetEdge &x = g.edges[size_t(a)], &y = g.edges[size_t(b)];
        return x.cls == y.cls && x.profile == y.profile && x.path == y.path && edgeWidth(l, a) == edgeWidth(l, b);
    };
    auto through = [&](int node, int e) {
        // The edge that carries a street straight on through a plain node: the same kind, the only other one.
        if (g.degree(node) != 2)
            return -1;
        for (int x : g.incident(node))
            if (x != e && g.isStreet(x) && same(x, e))
                return x;
        return -1;
    };
    auto walk = [&](int start, int e) {
        StreetLine line;
        const StreetEdge& first = g.edges[size_t(e)];
        line.cls = first.cls;
        line.profile = first.profile >= 0 ? first.profile : int(std::min(first.cls, StreetClass::Path));
        line.width = edgeWidth(l, e);
        line.path = first.path;
        line.street = first.street;
        line.startNode = start;
        int node = start;
        line.pts.push_back({g.nodes[size_t(node)].p.x, ground.surfaceHeight(g.nodes[size_t(node)].p), g.nodes[size_t(node)].p.y});
        while (e >= 0 && !used[size_t(e)])
        {
            used[size_t(e)] = 1;
            const StreetEdge& x = g.edges[size_t(e)];
            node = x.a == node ? x.b : x.a;
            const Vec2 p = g.nodes[size_t(node)].p;
            line.pts.push_back({p.x, ground.surfaceHeight(p), p.y});
            e = through(node, e);
        }
        line.endNode = node;
        if (line.pts.size() >= 2)
            lines.push_back(std::move(line));
    };
    for (size_t n = 0; n < g.nodes.size(); ++n)
        for (int e : g.incident(int(n)))
            if (g.isStreet(e) && !used[size_t(e)] && through(int(n), e) < 0)
                walk(int(n), e);
    for (size_t e = 0; e < g.edges.size(); ++e)
        if (g.isStreet(int(e)) && !used[e])
            walk(g.edges[e].a, int(e));
    for (const SurfacePath& s : l.surfacePaths)
    {
        StreetLine line;
        line.pts = s.pts;
        line.cls = s.cls;
        line.profile = s.profile;
        line.width = s.width;
        line.path = s.path;
        line.surface = true;
        lines.push_back(std::move(line));
    }
    return lines;
}

json pathResults(const Layout& l)
{
    json out = json::array();
    for (const PathResult& r : l.pathResults)
    {
        json j = {{"id", r.id}, {"ok", r.ok}, {"lengthM", r.length / 100.0}, {"surfaceLengthM", r.surfaceLength / 100.0}};
        if (!r.ok)
            j["error"] = r.error;
        out.push_back(j);
    }
    return out;
}

const char* controlName(Control c)
{
    switch (c)
    {
    case Control::None: return "none";
    case Control::Yield: return "yield";
    case Control::Stop: return "stop";
    case Control::Signal: return "signal";
    }
    return "none";
}

namespace
{
/**
 * How a junction is controlled, its signal phases and crosswalks: lights where an arterial meets another vehicle road or
 * two collectors cross, stop lines where four or more meet, yield otherwise. Opposite approaches share a phase.
 */
void controlJunction(Junction& j, const RoadNetwork& net)
{
    int vehicles = 0, arterials = 0, collectors = 0;
    for (const RoadEnd& e : j.ends)
    {
        if (!e.vehicle)
            continue;
        ++vehicles;
        const StreetClass c = net.lines[size_t(e.line)].cls;
        arterials += c == StreetClass::Arterial;
        collectors += c == StreetClass::Collector;
    }
    if (vehicles < 2)
        j.control = Control::None;
    else if (vehicles >= 3 && (arterials >= 1 && arterials + collectors >= 2))
        j.control = Control::Signal;
    else if (vehicles >= 4 || collectors >= 2)
        j.control = Control::Stop;
    else
        j.control = Control::Yield;
    if (j.control == Control::Signal)
    {
        // Approaches pair with the one most nearly opposite, the straightest pairs first, so a road that runs on
        // through the junction is never paired across a side road.
        std::vector<std::tuple<double, int, int>> pairs;
        for (size_t a = 0; a < j.ends.size(); ++a)
            for (size_t b = a + 1; b < j.ends.size(); ++b)
                if (j.ends[a].vehicle && j.ends[b].vehicle)
                    if (const double d = -glm::dot(j.ends[a].out, j.ends[b].out); d > 0.7)
                        pairs.emplace_back(-d, int(a), int(b));
        std::sort(pairs.begin(), pairs.end());
        std::vector<int> partner(j.ends.size(), -1);
        for (const auto& [d, a, b] : pairs)
            if (partner[size_t(a)] < 0 && partner[size_t(b)] < 0)
            {
                partner[size_t(a)] = b;
                partner[size_t(b)] = a;
            }
        for (size_t a = 0; a < j.ends.size(); ++a)
        {
            if (!j.ends[a].vehicle || (partner[a] >= 0 && partner[a] < int(a)))
                continue;
            std::vector<int> phase = {int(a)};
            if (partner[a] >= 0)
                phase.push_back(partner[a]);
            j.phases.push_back(std::move(phase));
        }
    }
    if (j.control == Control::Signal || j.control == Control::Stop)
        for (size_t k = 0; k < j.ends.size(); ++k)
        {
            const RoadEnd& e = j.ends[k];
            if (!e.vehicle)
                continue;
            const double half = 0.5 * net.lines[size_t(e.line)].width;
            const glm::dvec3 side = glm::normalize(glm::dvec3(-e.out.z, 0.0, e.out.x)) * half;
            j.crosswalks.push_back({int(k), e.p - side, e.p + side});
        }
}

/** Parking spaces along both kerbs of roads whose profile has a parking lane. */
void parkAlong(RoadNetwork& net, const Layout& l)
{
    for (size_t k = 0; k < net.lines.size(); ++k)
    {
        const StreetLine& s = net.lines[k];
        const RoadProfile& p = l.roads[size_t(s.profile)];
        if (p.parking <= 0.0 || s.surface)
            continue;
        const double offset = 0.5 * (p.lanes * p.laneWidth + p.median) + 0.5 * p.parking;
        const double bay = 600.0;
        for (size_t i = 0; i + 1 < s.pts.size(); ++i)
        {
            const glm::dvec3 a = s.pts[i], b = s.pts[i + 1];
            const double len = glm::length(b - a);
            if (len < bay)
                continue;
            const glm::dvec3 dir = (b - a) / len;
            const glm::dvec3 side = glm::normalize(glm::dvec3(-dir.z, 0.0, dir.x));
            const int n = int(len / bay);
            const double start = 0.5 * (len - n * bay);
            for (int m = 0; m < n; ++m)
                for (double sign : {-1.0, 1.0})
                    net.parking.push_back({a + dir * (start + (m + 0.5) * bay) + side * (sign * offset), dir * sign, bay, p.parking, int(k)});
        }
    }
}
} // namespace

RoadNetwork roadNetwork(const Layout& l, const Ground& ground)
{
    LILA_ZONE();

    RoadNetwork net;
    net.lines = streetLines(l, ground);
    std::map<int, size_t> at; // graph node -> junction
    for (size_t k = 0; k < net.lines.size(); ++k)
    {
        const StreetLine& line = net.lines[k];
        for (const int node : {line.startNode, line.endNode})
            if (node >= 0 && l.graph.degree(node) >= 3 && !at.contains(node))
            {
                at[node] = net.junctions.size();
                Junction j;
                const Vec2 p = l.graph.nodes[size_t(node)].p;
                j.p = {p.x, ground.surfaceHeight(p), p.y};
                // Wide enough for the widest road meeting there, and never past half of any road's length.
                for (int e : l.graph.incident(node))
                    if (l.graph.isStreet(e))
                        j.radius = std::max(j.radius, 0.5 * edgeWidth(l, e) + 100.0);
                net.junctions.push_back(j);
            }
    }
    for (size_t k = 0; k < net.lines.size(); ++k)
    {
        StreetLine& line = net.lines[k];
        for (int end = 0; end < 2; ++end)
        {
            const int node = end == 0 ? line.startNode : line.endNode;
            auto it = at.find(node);
            if (it == at.end())
                continue;
            Junction& j = net.junctions[it->second];
            double length = 0.0;
            for (size_t i = 0; i + 1 < line.pts.size(); ++i)
                length += glm::length(line.pts[i + 1] - line.pts[i]);
            const double cut = std::min(j.radius, 0.45 * length);
            // Walk in from this end by cut, dropping the points passed.
            std::vector<glm::dvec3>& pts = line.pts;
            if (end == 1)
                std::reverse(pts.begin(), pts.end());
            double left = cut;
            while (pts.size() >= 2 && glm::length(pts[1] - pts[0]) <= left)
            {
                left -= glm::length(pts[1] - pts[0]);
                pts.erase(pts.begin());
            }
            if (pts.size() >= 2)
                pts[0] += glm::normalize(pts[1] - pts[0]) * left;
            const glm::dvec3 out = pts.size() >= 2 ? glm::normalize(pts[1] - pts[0]) : glm::dvec3(1, 0, 0);
            j.ends.push_back({int(k), end == 0, pts[0], out, l.roads[size_t(line.profile)].lanes > 0 && line.cls != StreetClass::Path});
            if (end == 1)
                std::reverse(pts.begin(), pts.end());
        }
    }
    for (Junction& j : net.junctions)
        controlJunction(j, net);
    finishSignals(net, l);
    parkAlong(net, l);
    return net;
}
} // namespace settlement
