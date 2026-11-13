// Ruin and age: a ragged break through any set of stones, survivors, a support pass so nothing floats, tipped top
// stones, fallen stones heaped (or settled with Jolt) at the foot, slabs broken with Fracture; then wet, moss and age
// per stone. realizePlan runs the whole pipeline the masonry nodes share.

#include "MasonryRuin.h"

#include <lila/Boolean.h>
#include <lila/Fracture.h>
#include <lila/Log.h>
#include <lila/Profile.h>

#include "RigidScene.h"

#include <algorithm>
#include <cmath>
#include <map>
#include <numeric>
#include <stdexcept>
#include <unordered_map>

namespace masonry
{
namespace
{
double smooth(double t) { return t * t * (3.0 - 2.0 * t); }

/** Value noise in [0, 1] on a unit lattice, seeded. */
double valueNoise(const glm::dvec3& p, uint64_t seed)
{
    const glm::dvec3 f = glm::floor(p);
    const glm::dvec3 t = p - f;
    auto at = [&](int dx, int dy, int dz) {
        return unitRandom(mixKey(seed, uint64_t(int64_t(f.x) + dx), uint64_t(int64_t(f.y) + dy), uint64_t(int64_t(f.z) + dz), 77));
    };
    const double sx = smooth(t.x), sy = smooth(t.y), sz = smooth(t.z);
    auto lerp = [](double a, double b, double s) { return a + (b - a) * s; };
    const double x00 = lerp(at(0, 0, 0), at(1, 0, 0), sx), x10 = lerp(at(0, 1, 0), at(1, 1, 0), sx);
    const double x01 = lerp(at(0, 0, 1), at(1, 0, 1), sx), x11 = lerp(at(0, 1, 1), at(1, 1, 1), sx);
    return lerp(lerp(x00, x10, sy), lerp(x01, x11, sy), sz);
}

double fbm(const glm::dvec3& p, uint64_t seed)
{
    double sum = 0.0, amp = 0.5, norm = 0.0;
    glm::dvec3 q = p;
    for (int o = 0; o < 3; ++o)
    {
        sum += valueNoise(q, seed + uint64_t(o) * 1013) * amp;
        norm += amp;
        amp *= 0.5;
        q *= 2.03;
    }
    return sum / norm;
}

double volumeOf(const Stone& s) { return s.size.x * s.size.y * s.size.z * (s.shape == Shape::Wedge ? (1.0 + s.taper) * 0.5 : 1.0); }

/** Ground and top of the structure per square ground cell, from the stones' bounds. */
struct Columns
{
    double cell = 100.0;
    std::unordered_map<uint64_t, std::pair<double, double>> at; // cell -> (lowest bottom, highest top)
    double ground = 0.0, top = 0.0;

    static uint64_t key(int64_t x, int64_t z) { return StoneGrid::pack(x, 0, z); }
    glm::i64vec2 cellOf(const glm::dvec3& p) const { return {int64_t(std::floor(p.x / cell)), int64_t(std::floor(p.z / cell))}; }

    /** Tops from every stone; the ground from grounded ones only when groundedOnly, so a dome or arch is not ground. */
    void build(std::span<const Stone> stones, double size, bool groundedOnly = false)
    {
        cell = std::max(size, 1.0);
        at.clear();
        ground = 1e300;
        top = -1e300;
        for (const Stone& s : stones)
        {
            const glm::dvec3 e = halfExtentAabb(s);
            const auto c = cellOf(s.center);
            const double bottom = groundedOnly && !s.grounded ? 1e300 : s.center.y - e.y;
            auto [it, added] = at.emplace(key(c.x, c.y), std::pair<double, double>{bottom, s.center.y + e.y});
            it->second.first = std::min(it->second.first, bottom);
            it->second.second = std::max(it->second.second, s.center.y + e.y);
            ground = std::min(ground, bottom);
            top = std::max(top, s.center.y + e.y);
        }
        if (ground >= 1e299)
            ground = 0.0;
        // Cells with nothing on the ground stand on the lowest ground there is.
        for (auto& [k, v] : at)
            if (v.first >= 1e299)
                v.first = ground;
        if (stones.empty())
            ground = top = 0.0;
    }
    /** The nearest cell's (ground, top) within a few cells, else the whole structure's. */
    std::pair<double, double> near(const glm::dvec3& p) const
    {
        const auto c = cellOf(p);
        for (int r = 0; r <= 3; ++r)
        {
            double best = 1e300;
            std::pair<double, double> found{ground, top};
            for (int64_t dx = -r; dx <= r; ++dx)
                for (int64_t dz = -r; dz <= r; ++dz)
                {
                    if (std::max(std::abs(dx), std::abs(dz)) != r)
                        continue;
                    auto it = at.find(key(c.x + dx, c.y + dz));
                    if (it == at.end())
                        continue;
                    const double d = double(dx * dx + dz * dz);
                    if (d < best)
                    {
                        best = d;
                        found = it->second;
                    }
                }
            if (best < 1e300)
                return found;
        }
        return {ground, top};
    }
};

/** Typical stone size: the median of each stone's middle dimension. */
double typicalSize(std::span<const Stone> stones)
{
    if (stones.empty())
        return 10.0;
    std::vector<double> mids;
    mids.reserve(stones.size());
    for (const Stone& s : stones)
    {
        glm::dvec3 d = s.size;
        std::sort(&d.x, &d.x + 3);
        mids.push_back(d.y);
    }
    std::nth_element(mids.begin(), mids.begin() + std::ptrdiff_t(mids.size() / 2), mids.end());
    return std::max(mids[mids.size() / 2], 0.5);
}

double gridCell(std::span<const Stone> stones)
{
    double big = 1.0;
    for (const Stone& s : stones)
        big = std::max(big, std::max({s.size.x, s.size.y, s.size.z}));
    return std::max(typicalSize(stones) * 4.0, std::min(big, typicalSize(stones) * 16.0));
}

/** Whether a rests on (or leans against) b: touching within pad, and b's centre lower, or a is a slab piece. */
bool touching(const Stone& a, const Stone& b, double pad) { return overlaps(a, b, -2.0 * pad); }

/**
 * Whether s rests on t: touching within pad, or within a course when t is an arch stone, since the triangles
 * between an arch's extrados and the level courses over it are spandrel fill in the core.
 */
bool restsOn(const Stone& s, const Stone& t, double pad)
{
    const bool arch = t.role == Role::Voussoir || t.role == Role::Keystone;
    return touching(s, t, arch ? std::max(pad, 1.1 * s.size.y) : pad);
}

/** Height map of a heap of stones on the ground. */
struct Heap
{
    double cell = 5.0;
    std::unordered_map<uint64_t, double> h;
    const Columns* ground = nullptr;

    double at(int64_t x, int64_t z) const
    {
        auto it = h.find(StoneGrid::pack(x, 0, z));
        if (it != h.end())
            return it->second;
        return ground->near(glm::dvec3((x + 0.5) * cell, 0.0, (z + 0.5) * cell)).first;
    }
    template <class Fn> void forCells(const glm::dvec3& centre, const glm::dvec3& e, Fn&& fn) const
    {
        const int64_t x0 = int64_t(std::floor((centre.x - e.x) / cell)), x1 = int64_t(std::floor((centre.x + e.x) / cell));
        const int64_t z0 = int64_t(std::floor((centre.z - e.z) / cell)), z1 = int64_t(std::floor((centre.z + e.z) / cell));
        for (int64_t x = x0; x <= x1; ++x)
            for (int64_t z = z0; z <= z1; ++z)
                fn(x, z);
    }
    /**
     * Cells under s's own outline seen from above, with s moved to at, rather than under its box's bounds: a stone
     * turned across the cells must not raise the heap over the corners it doesn't cover. At least the cell under at.
     */
    template <class Fn> void forFootprint(const Stone& s, const glm::dvec3& at, Fn&& fn) const
    {
        int up = 0;
        for (int i = 1; i < 3; ++i)
            if (std::abs(s.frame[i].y) > std::abs(s.frame[up].y))
                up = i;
        glm::dvec3 axis[2];
        double half[2];
        for (int i = 0, n = 0; i < 3; ++i)
            if (i != up)
            {
                const glm::dvec3 a(s.frame[i].x, 0.0, s.frame[i].z);
                axis[n] = glm::length(a) > 1e-9 ? glm::normalize(a) : glm::dvec3(n == 0, 0.0, n == 1);
                half[n++] = s.size[i] * 0.5;
            }
        // A cell counts when it overlaps the outline at all: its half size reaches out along each axis.
        for (int n = 0; n < 2; ++n)
            half[n] += cell * 0.5 * (std::abs(axis[n].x) + std::abs(axis[n].z));
        bool any = false;
        forCells(at, halfExtentAabb(s), [&](int64_t x, int64_t z) {
            const glm::dvec3 d((x + 0.5) * cell - at.x, 0.0, (z + 0.5) * cell - at.z);
            if (std::abs(glm::dot(d, axis[0])) <= half[0] && std::abs(glm::dot(d, axis[1])) <= half[1])
            {
                any = true;
                fn(x, z);
            }
        });
        if (!any)
            fn(int64_t(std::floor(at.x / cell)), int64_t(std::floor(at.z / cell)));
    }
    double highestUnder(const Stone& s, const glm::dvec3& at) const
    {
        double top = -1e300;
        forFootprint(s, at, [&](int64_t x, int64_t z) { top = std::max(top, this->at(x, z)); });
        return top;
    }
    void raiseUnder(const Stone& s, double top)
    {
        forFootprint(s, s.center, [&](int64_t x, int64_t z) { h[StoneGrid::pack(x, 0, z)] = std::max(at(x, z), top); });
    }
};

/** Turns a stone to lie on its flattest side with a random heading and a small lean. */
void layDown(Stone& s, double yaw, double leanX, double leanZ)
{
    int a = 0;
    if (s.size[1] <= s.size[a])
        a = 1;
    if (s.size[2] < s.size[a])
        a = 2;
    if (s.shape == Shape::Drum && s.size.y < s.size.x)
        a = 1;
    const glm::dvec3 heading(std::cos(yaw), 0.0, std::sin(yaw));
    glm::dmat3 f;
    f[a] = kUp;
    f[(a + 1) % 3] = heading;
    f[(a + 2) % 3] = glm::cross(f[a], f[(a + 1) % 3]);
    const glm::dmat3 lean = glm::dmat3(glm::dvec3(1, 0, 0), glm::dvec3(0, std::cos(leanX), std::sin(leanX)), glm::dvec3(0, -std::sin(leanX), std::cos(leanX))) *
                            glm::dmat3(glm::dvec3(std::cos(leanZ), std::sin(leanZ), 0), glm::dvec3(-std::sin(leanZ), std::cos(leanZ), 0), glm::dvec3(0, 0, 1));
    s.frame = lean * f;
}

void settleRubble(std::vector<Stone>& stones, const std::vector<size_t>& rubble, double ground, RuinStats& stats)
{
    LILA_ZONE_N("Masonry settle rubble");
    if (rubble.empty() || rubble.size() > 6000)
        return;
    namespace ph = lila::physics;
    ph::initJolt();
    ph::Settings set;
    set.ground = true;
    set.groundHeight = ground;
    set.recordContacts = false;
    ph::RigidScene scene(set);
    glm::dvec3 lo(1e300), hi(-1e300);
    for (size_t k : rubble)
    {
        const glm::dvec3 e = halfExtentAabb(stones[k]);
        lo = glm::min(lo, stones[k].center - e);
        hi = glm::max(hi, stones[k].center + e);
    }
    std::vector<char> isRubble(stones.size(), 0);
    for (size_t k : rubble)
        isRubble[k] = 1;
    // The standing stones near the heaps hold them up.
    for (size_t k = 0; k < stones.size(); ++k)
    {
        if (isRubble[k] || stones[k].role == Role::Rubble)
            continue;
        const glm::dvec3 e = halfExtentAabb(stones[k]);
        if (glm::any(glm::lessThan(stones[k].center + e, lo)) || glm::any(glm::greaterThan(stones[k].center - e, hi)))
            continue;
        ph::BodyDesc b;
        b.role = ph::Role::Static;
        b.shape.kind = ph::Shape::Kind::Box;
        b.shape.halfExtents = glm::max(stones[k].size * 0.5, glm::dvec3(0.5));
        b.shape.rounding = std::min(0.5, b.shape.halfExtents.x * 0.2);
        b.position = stones[k].center;
        b.rotation = glm::quat_cast(stones[k].frame);
        scene.add(b);
    }
    std::vector<int> body;
    for (size_t k : rubble)
    {
        ph::BodyDesc b;
        b.shape.kind = ph::Shape::Kind::Box;
        b.shape.halfExtents = glm::max(stones[k].size * 0.5, glm::dvec3(0.5));
        b.shape.rounding = std::min({0.5, b.shape.halfExtents.x * 0.2, b.shape.halfExtents.y * 0.2, b.shape.halfExtents.z * 0.2});
        b.position = stones[k].center + kUp * std::max(1.0, stones[k].size.y * 0.1);
        b.rotation = glm::quat_cast(stones[k].frame);
        b.density = 2400.0;
        b.friction = 0.8;
        b.bounce = 0.0;
        b.damping = 0.2;
        b.user = int64_t(k);
        body.push_back(scene.add(b));
    }
    scene.settle(4.0);
    for (size_t i = 0; i < rubble.size(); ++i)
    {
        const ph::Pose pose = scene.pose(body[i]);
        Stone& s = stones[rubble[i]];
        s.center = pose.position;
        s.frame = glm::mat3_cast(pose.rotation);
    }
    stats.settled = true;
}

/** Closed prism over a polygon outline (Y ignored) between y0 and y1, faces outward. */
geo::Mesh slabMesh(const std::vector<glm::dvec3>& outline, double y0, double y1)
{
    const size_t n = outline.size();
    std::vector<glm::dvec3> pts;
    for (const glm::dvec3& p : outline)
        pts.push_back({p.x, y0, p.z});
    for (const glm::dvec3& p : outline)
        pts.push_back({p.x, y1, p.z});
    std::vector<geo::Index> sizes, idx;
    // Outlines are anticlockwise seen from above (positive area in x, z), so the top in order faces down.
    sizes.push_back(geo::Index(n));
    for (size_t k = 0; k < n; ++k)
        idx.push_back(geo::Index(k));
    sizes.push_back(geo::Index(n));
    for (size_t k = n; k-- > 0;)
        idx.push_back(geo::Index(n + k));
    for (size_t k = 0; k < n; ++k)
    {
        const geo::Index a = geo::Index(k), b = geo::Index((k + 1) % n);
        sizes.push_back(4);
        idx.insert(idx.end(), {a, geo::Index(a + n), geo::Index(b + n), b});
    }
    geo::Mesh m = geo::Mesh::fromPolygons(pts, sizes, idx);
    double vol = 0.0;
    for (geo::Index f = 0; f < m.faceCapacity(); ++f)
        if (m.faceValid(f))
        {
            const std::vector<geo::Index> fp = m.facePoints(f);
            for (size_t k = 1; k + 1 < fp.size(); ++k)
                vol += glm::dot(m.position(fp[0]), glm::cross(m.position(fp[k]), m.position(fp[k + 1])));
        }
    if (vol < 0.0)
    {
        for (size_t f = 0, o = 0; f < sizes.size(); o += sizes[f], ++f)
            std::reverse(idx.begin() + std::ptrdiff_t(o), idx.begin() + std::ptrdiff_t(o + sizes[f]));
        m = geo::Mesh::fromPolygons(pts, sizes, idx);
    }
    return m;
}

double turnXz(const glm::dvec3& a, const glm::dvec3& b, const glm::dvec3& c) { return (b.x - a.x) * (c.z - a.z) - (b.z - a.z) * (c.x - a.x); }

/** A polygon seen from above cut into convex pieces: itself when convex, else triangles by ear clipping. */
std::vector<std::vector<glm::dvec3>> convexPieces(std::vector<glm::dvec3> poly)
{
    double area = 0.0;
    for (size_t i = 0; i < poly.size(); ++i)
        area += turnXz(glm::dvec3(0.0), poly[i], poly[(i + 1) % poly.size()]);
    if (area < 0.0)
        std::reverse(poly.begin(), poly.end());
    bool convex = true;
    for (size_t i = 0; i < poly.size() && convex; ++i)
        convex = turnXz(poly[i], poly[(i + 1) % poly.size()], poly[(i + 2) % poly.size()]) >= -1e-9;
    if (convex || poly.size() < 4)
        return {poly};
    std::vector<std::vector<glm::dvec3>> out;
    while (poly.size() > 3)
    {
        const size_t n = poly.size();
        size_t ear = n;
        for (size_t i = 0; i < n && ear == n; ++i)
        {
            const glm::dvec3 &a = poly[(i + n - 1) % n], &b = poly[i], &c = poly[(i + 1) % n];
            if (turnXz(a, b, c) <= 1e-9)
                continue;
            bool empty = true;
            for (size_t k = 0; k < n && empty; ++k)
            {
                const glm::dvec3& q = poly[k];
                if (k == i || k == (i + 1) % n || k == (i + n - 1) % n)
                    continue;
                empty = !(turnXz(a, b, q) > 0.0 && turnXz(b, c, q) > 0.0 && turnXz(c, a, q) > 0.0);
            }
            if (empty)
                ear = i;
        }
        if (ear == n)
            ear = 0;
        out.push_back({poly[(ear + n - 1) % n], poly[ear], poly[(ear + 1) % n]});
        poly.erase(poly.begin() + std::ptrdiff_t(ear));
    }
    out.push_back(poly);
    return out;
}

/** Convex prisms covering a slab piece lying in place, one per convex part of each of its top faces: its collision. */
std::vector<geo::Mesh> slabHulls(const Stone& s, const geo::Mesh& piece)
{
    std::vector<geo::Mesh> out;
    const geo::Bounds bb = piece.bounds();
    for (geo::Index f = 0; f < piece.faceCapacity(); ++f)
    {
        if (!piece.faceValid(f) || piece.faceNormal(f).y < 0.5)
            continue;
        std::vector<glm::dvec3> poly;
        for (geo::Index q : piece.facePoints(f))
            poly.push_back(s.center + piece.position(q));
        if (poly.size() < 3)
            continue;
        for (const std::vector<glm::dvec3>& part : convexPieces(std::move(poly)))
            out.push_back(slabMesh(part, s.center.y + bb.min.y, s.center.y + bb.max.y));
    }
    return out;
}

bool inside(const std::vector<glm::dvec3>& poly, double x, double z)
{
    bool in = false;
    for (size_t i = 0, j = poly.size() - 1; i < poly.size(); j = i++)
        if ((poly[i].z > z) != (poly[j].z > z) && x < (poly[j].x - poly[i].x) * (z - poly[i].z) / (poly[j].z - poly[i].z) + poly[i].x)
            in = !in;
    return in;
}
} // namespace

/**
 * Every standing stone as a rigid body (grounded ones fixed) left to settle under gravity: those that move or turn more
 * than a little cannot stand. Returns how many fell; they are taken out of keep.
 */
int structuralPass(std::vector<Stone>& stones, std::vector<char>& keep, double ground, double pad)
{
    LILA_ZONE_N("Masonry structural pass");
    namespace ph = lila::physics;
    std::vector<size_t> bodies;
    size_t dynamic = 0;
    for (size_t k = 0; k < stones.size(); ++k)
        if (keep[k])
        {
            bodies.push_back(k);
            dynamic += stones[k].grounded ? 0 : 1;
        }
    if (dynamic == 0 || dynamic > 8000)
        return 0;
    ph::initJolt();
    ph::Settings set;
    set.ground = true;
    set.groundHeight = ground;
    set.recordContacts = false;
    ph::RigidScene scene(set);
    std::vector<int> id(bodies.size());
    for (size_t i = 0; i < bodies.size(); ++i)
    {
        const Stone& s = stones[bodies[i]];
        auto pts = std::make_shared<std::vector<glm::dvec3>>();
        const glm::dvec3 h = s.size * 0.5;
        for (int c = 0; c < 8; ++c)
        {
            glm::dvec3 q((c & 1) ? h.x : -h.x, (c & 2) ? h.y : -h.y, (c & 4) ? h.z : -h.z);
            if (s.shape == Shape::Wedge && !(c & 2))
                q.x *= double(s.taper);
            pts->push_back(q);
        }
        ph::BodyDesc b;
        b.shape.kind = ph::Shape::Kind::Hull;
        b.shape.points = pts;
        b.shape.rounding = std::min(0.2, std::min({h.x, h.y, h.z}) * 0.1);
        b.position = s.center;
        b.rotation = glm::quat_cast(s.frame);
        b.role = s.grounded ? ph::Role::Static : ph::Role::Dynamic;
        b.density = 2200.0;
        b.friction = 0.8;
        b.bounce = 0.0;
        b.damping = 0.1;
        id[i] = scene.add(b);
    }
    scene.settle(3.0);
    int fell = 0;
    for (size_t i = 0; i < bodies.size(); ++i)
    {
        Stone& s = stones[bodies[i]];
        if (s.grounded)
            continue;
        const ph::Pose pose = scene.pose(id[i]);
        const double moved = glm::length(pose.position - s.center);
        const glm::dquat was = glm::quat_cast(s.frame);
        const double turned = 2.0 * std::acos(std::clamp(std::abs(glm::dot(was, pose.rotation)), 0.0, 1.0));
        const double minSize = std::min({s.size.x, s.size.y, s.size.z});
        if (moved > std::max(2.0 + pad, 0.25 * minSize) || turned > glm::radians(8.0))
        {
            keep[bodies[i]] = 0;
            ++fell;
        }
    }
    return fell;
}

std::vector<RuinStroke> strokesFromJson(const nlohmann::json& list)
{
    std::vector<RuinStroke> out;
    if (list.is_null() || (list.is_string() && list.get<std::string>().empty()))
        return out;
    const nlohmann::json parsed = list.is_string() ? nlohmann::json::parse(list.get<std::string>(), nullptr, false) : list;
    if (!parsed.is_array())
        throw std::runtime_error("ruin strokes are a JSON list of {points, radius, strength}");
    for (const nlohmann::json& s : parsed)
    {
        if (!s.is_object() || !s.contains("points") || !s["points"].is_array())
            throw std::runtime_error("a ruin stroke is {points: [[x, y, z], ...], radius, strength}");
        RuinStroke r;
        for (const nlohmann::json& q : s["points"])
            if (q.is_array() && q.size() == 3)
                r.points.emplace_back(q[0].get<double>(), q[1].get<double>(), q[2].get<double>());
        r.radius = s.value("radius", 50.0);
        r.strength = std::clamp(s.value("strength", 1.0), -1.0, 1.0);
        out.push_back(std::move(r));
    }
    return out;
}

nlohmann::json RuinStats::toJson() const
{
    return {{"removed", removed},       {"survivors", survivors},       {"loose", loose},     {"unsupported", unsupported},
            {"rubble", rubble},         {"slabPieces", slabPieces},     {"fallenPieces", fallenPieces}, {"painted", painted},
            {"unstable", unstable},     {"left", left},                 {"settled", settled}};
}

RuinStats ruinStones(std::vector<Stone>& stones, const RuinParams& p, double pitch, std::span<const char> eligible, std::vector<int>* keptOut)
{
    LILA_ZONE_N("Masonry ruin");
    RuinStats st;
    if (stones.empty() || !p.active())
        return st;
    const size_t n = stones.size();
    auto may = [&](size_t k) { return eligible.empty() || (k < eligible.size() && eligible[k]); };
    const double unit = pitch > 0.0 ? pitch : typicalSize(stones);
    // The ground under the structure, from what stands on it: floors and roofs are not ground.
    std::vector<Stone> footing;
    for (const Stone& s : stones)
        if (s.role != Role::Slab)
            footing.push_back(s);
    Columns cols;
    cols.build(footing.empty() ? std::span<const Stone>(stones) : std::span<const Stone>(footing), unit * 4.0);
    bool anyGrounded = false;
    for (const Stone& s : stones)
        anyGrounded = anyGrounded || s.grounded;
    if (!anyGrounded)
        for (Stone& s : stones)
            s.grounded = s.center.y - halfExtentAabb(s).y <= cols.near(s.center).first + unit * 0.5;
    footing.clear();
    for (const Stone& s : stones)
        if (s.role != Role::Slab)
            footing.push_back(s);
    cols.build(footing.empty() ? std::span<const Stone>(stones) : std::span<const Stone>(footing), unit * 4.0, true);

    // Where each stone stands relative to the break: its height up the structure against a ragged, sloping line.
    glm::dvec3 centre(0.0);
    double total = 0.0;
    for (size_t k = 0; k < n; ++k)
        if (may(k))
        {
            centre += stones[k].center * volumeOf(stones[k]);
            total += volumeOf(stones[k]);
        }
    centre /= std::max(total, 1e-12);
    double extent = 1.0;
    for (const Stone& s : stones)
        extent = std::max(extent, glm::length(glm::dvec2(s.center.x - centre.x, s.center.z - centre.z)));
    const double azimuth = glm::radians(p.collapse);
    const glm::dvec3 towards(std::cos(azimuth), 0.0, -std::sin(azimuth));
    const double featur = p.scale > 0.0 ? p.scale : unit * 10.0;
    std::vector<glm::dvec3> gapAt;
    for (int g = 0; g < p.gaps; ++g)
        gapAt.push_back(stones[size_t(unitRandom(mixKey(p.seed, 401, g)) * double(n)) % n].center);
    const double gapWidth = std::max(unit * 3.0, extent * 0.12);

    std::vector<double> rel(n), off(n), gapCap(n, 1e300);
    for (size_t k = 0; k < n; ++k)
    {
        const Stone& s = stones[k];
        const auto [g, t] = cols.near(s.center);
        rel[k] = (s.center.y - g) / std::max(t - g, unit);
        const double along = glm::dot(glm::dvec3(s.center.x - centre.x, 0.0, s.center.z - centre.z), towards) / extent;
        const double noise = fbm(glm::dvec3(s.center.x, s.center.y * 0.5, s.center.z) / featur, p.seed) - 0.5;
        const double edge = (unitRandom(mixKey(p.seed, 402, k)) - 0.5) * 0.08;
        off[k] = -std::clamp(p.slope, 0.0, 1.0) * 0.6 * along + std::clamp(p.roughness, 0.0, 1.0) * 0.9 * noise + edge;
        for (const glm::dvec3& ga : gapAt)
        {
            const double d = glm::length(glm::dvec2(s.center.x - ga.x, s.center.z - ga.z)) / gapWidth;
            gapCap[k] = std::min(gapCap[k], 0.03 + d * d * 0.8 + edge);
        }
    }
    auto keptAt = [&](double base, size_t k) { return !may(k) || rel[k] <= std::min(base + off[k], gapCap[k]); };
    // The level whose break leaves the asked share standing.
    double lo = -2.0, hi = 3.0;
    if (p.left >= 1.0 - 1e-9)
        lo = hi = 1e6;
    else
        for (int it = 0; it < 40; ++it)
        {
            const double mid = (lo + hi) * 0.5;
            double kept = 0.0;
            for (size_t k = 0; k < n; ++k)
                if (may(k) && keptAt(mid, k))
                    kept += volumeOf(stones[k]);
            if (kept / total < std::clamp(p.left, 0.0, 1.0))
                lo = mid;
            else
                hi = mid;
        }
    const double base = (lo + hi) * 0.5;
    std::vector<char> keep(n);
    for (size_t k = 0; k < n; ++k)
    {
        keep[k] = keptAt(base, k);
        // A few stones just above the break hang on.
        if (!keep[k] && rel[k] - std::min(base + off[k], gapCap[k]) < 0.12 && unitRandom(mixKey(p.seed, 403, k)) < p.survivors)
        {
            keep[k] = 2;
            ++st.survivors;
        }
    }

    // Brush strokes: stones near a stroke fall (or, for a negative stroke, stay), by strength and distance.
    for (const RuinStroke& stroke : p.strokes)
    {
        if (stroke.points.empty() || stroke.radius <= 0.0)
            continue;
        for (size_t k = 0; k < n; ++k)
        {
            double d = 1e300;
            const glm::dvec3& c = stones[k].center;
            for (size_t i = 0; i < stroke.points.size(); ++i)
            {
                const glm::dvec3 a = stroke.points[i], b = stroke.points[std::min(i + 1, stroke.points.size() - 1)];
                const glm::dvec3 ab = b - a;
                const double t = glm::dot(ab, ab) > 1e-12 ? std::clamp(glm::dot(c - a, ab) / glm::dot(ab, ab), 0.0, 1.0) : 0.0;
                d = std::min(d, glm::length(c - (a + ab * t)));
            }
            const double w = stroke.strength * std::clamp(1.0 - d / stroke.radius, 0.0, 1.0);
            if (w > 0.25 + 0.4 * unitRandom(mixKey(p.seed, 420, k)))
            {
                st.painted += keep[k] ? 1 : 0;
                keep[k] = 0;
            }
            else if (w < -0.25)
                keep[k] = std::max<char>(keep[k], 1);
        }
    }

    // Support: from the ground up, a stone stays if it touches a lower stone that stays. Slab pieces may also hang off a neighbour piece.
    StoneGrid grid;
    grid.cell = gridCell(stones);
    grid.build(stones, 0.0);
    const double pad = std::max(0.5, unit * 0.12);
    auto supportPass = [&] {
        std::vector<size_t> order(n);
        std::iota(order.begin(), order.end(), size_t(0));
        std::stable_sort(order.begin(), order.end(), [&](size_t a, size_t b) {
            return stones[a].center.y - halfExtentAabb(stones[a]).y < stones[b].center.y - halfExtentAabb(stones[b]).y;
        });
        std::vector<char> supported(n, 0);
        for (int pass = 0; pass < 3; ++pass)
            for (size_t k : order)
            {
                if (!keep[k] || supported[k])
                    continue;
                const Stone& s = stones[k];
                if (s.grounded)
                {
                    supported[k] = 1;
                    continue;
                }
                for (int o : grid.around(s, std::max(pad, 1.1 * s.size.y)))
                {
                    const size_t m = size_t(o);
                    if (m == k || !keep[m] || !supported[m])
                        continue;
                    const Stone& t = stones[m];
                    const bool lower = t.center.y < s.center.y - 0.1 * std::min(s.size.y, t.size.y);
                    const bool slabHold = s.role == Role::Slab && (t.role != Role::Slab || pass < 2);
                    if ((lower && restsOn(s, t, pad)) || (slabHold && touching(s, t, pad)))
                    {
                        supported[k] = 1;
                        break;
                    }
                }
            }
        for (size_t k = 0; k < n; ++k)
            if (keep[k] && !supported[k])
            {
                keep[k] = 0;
                ++st.unsupported;
            }
    };
    supportPass();

    // Loose stones: tops of what is left, tipped about their bottom edge outward or slid out a little.
    StoneGrid keptGrid;
    keptGrid.cell = grid.cell;
    std::vector<Stone> keptOnly;
    std::vector<size_t> keptIndex;
    for (size_t k = 0; k < n; ++k)
        if (keep[k])
        {
            keptOnly.push_back(stones[k]);
            keptIndex.push_back(k);
        }
    keptGrid.build(keptOnly, 0.0);
    std::vector<char> topExposed(n, 0);
    for (size_t i = 0; i < keptOnly.size(); ++i)
    {
        const Stone& s = keptOnly[i];
        bool covered = false;
        for (int o : keptGrid.around(s, pad))
            if (size_t(o) != i && keptOnly[size_t(o)].center.y > s.center.y + 0.25 * s.size.y && touching(s, keptOnly[size_t(o)], pad))
            {
                covered = true;
                break;
            }
        topExposed[keptIndex[i]] = !covered;
    }
    auto clearOf = [&](const Stone& c, size_t k) {
        for (int o : grid.around(c, std::max({pad, c.size.x, c.size.y, c.size.z})))
            if (size_t(o) != k && keep[size_t(o)] && overlaps(c, stones[size_t(o)], 0.0))
                return false;
        return true;
    };
    for (size_t k = 0; k < n; ++k)
    {
        const Stone& s = stones[k];
        if (!keep[k] || !topExposed[k] || s.t1 < s.t0 || s.grounded || !may(k))
            continue;
        if (unitRandom(mixKey(p.seed, 404, k)) >= p.loose)
            continue;
        const double sideSign = s.face == Face::Through ? (unitRandom(mixKey(p.seed, 405, k)) < 0.5 ? 1.0 : -1.0) : 1.0;
        const bool tipped = unitRandom(mixKey(p.seed, 406, k)) < 0.7;
        const double angle = glm::radians(std::clamp(p.tip, 0.0, 89.0)) * (0.25 + 0.75 * unitRandom(mixKey(p.seed, 407, k)));
        const double slide = s.size.z * (0.1 + 0.2 * unitRandom(mixKey(p.seed, 408, k)));
        // One that would meet a neighbour tips or slides less, or stays put.
        for (double scale : {1.0, 0.5, 0.25})
        {
            Stone c = s;
            if (tipped)
            {
                const glm::dvec3 pivotLocal(0.0, -c.size.y * 0.5, -sideSign * c.size.z * 0.5);
                const glm::dvec3 pivot = c.center + c.frame * pivotLocal;
                const double a = -sideSign * angle * scale;
                const glm::dmat3 rx(glm::dvec3(1, 0, 0), glm::dvec3(0, std::cos(a), std::sin(a)), glm::dvec3(0, -std::sin(a), std::cos(a)));
                c.frame = c.frame * rx;
                c.center = pivot + c.frame * (-pivotLocal);
            }
            else
                c.center -= c.frame[2] * (sideSign * slide * scale);
            if (!clearOf(c, k))
                continue;
            c.loose = true;
            stones[k] = c;
            ++st.loose;
            break;
        }
    }
    // A tipped or slid stone may no longer hold what it held.
    grid.build(stones, 0.0);
    supportPass();
    if (p.structural)
    {
        st.unstable = structuralPass(stones, keep, cols.ground, pad);
        supportPass();
    }

    // Damage: kept stones near what fell, by distance.
    {
        std::vector<Stone> fallen;
        for (size_t k = 0; k < n; ++k)
            if (!keep[k])
                fallen.push_back(stones[k]);
        StoneGrid fg;
        fg.cell = grid.cell;
        fg.build(fallen, 0.0);
        const double reach = unit * 2.5;
        for (size_t k = 0; k < n; ++k)
        {
            if (!keep[k] || fallen.empty())
                continue;
            double nearest = 1e300;
            for (int o : fg.around(stones[k], reach))
                nearest = std::min(nearest, glm::length(fallen[size_t(o)].center - stones[k].center));
            stones[k].damage = float(std::clamp(1.0 - nearest / (reach * 2.0), 0.0, 1.0));
            if (stones[k].loose)
                stones[k].damage = std::max(stones[k].damage, 0.8f);
        }
    }

    // Rubble: a share of the fallen land in heaps at the foot, lowest first, thrown out of their face and toward the collapse.
    Heap heap, standing;
    heap.cell = standing.cell = std::max(unit * 0.5, 0.5);
    heap.ground = standing.ground = &cols;
    // Where standing stones start above the ground, per cell: rubble lands under a floor or roof but never in a wall.
    std::unordered_map<uint64_t, double> standingFrom;
    for (size_t k = 0; k < n; ++k)
        if (keep[k])
        {
            const glm::dvec3 e = halfExtentAabb(stones[k]);
            standing.forCells(stones[k].center, e * 0.9, [&](int64_t x, int64_t z) {
                auto [it, added] = standingFrom.emplace(StoneGrid::pack(x, 0, z), stones[k].center.y - e.y);
                it->second = std::min(it->second, stones[k].center.y - e.y);
            });
        }
    auto blocked = [&](const glm::dvec3& c, const glm::dvec3& e, double bottom, double height) {
        bool hit = false;
        standing.forCells(c, e, [&](int64_t x, int64_t z) {
            auto it = standingFrom.find(StoneGrid::pack(x, 0, z));
            hit = hit || (it != standingFrom.end() && it->second < bottom + height);
        });
        return hit;
    };
    // Down the heap while it is steeper than loose stone lies, so heaps spread out and fill hollows instead of stacking.
    auto slideDownHeap = [&](Stone& s, const glm::dvec3& e, double run, uint64_t key) {
        constexpr double kRepose = 0.7; // tan of about 35 degrees
        for (int slide = 0; slide < 64; ++slide)
        {
            double low = heap.highestUnder(s, s.center) - run * kRepose;
            glm::dvec3 next = s.center;
            const double turn = unitRandom(mixKey(key, uint64_t(slide)));
            for (int d = 0; d < 8; ++d)
            {
                const double a = (d + turn) * kPi * 0.25;
                const glm::dvec3 q = s.center + glm::dvec3(std::cos(a), 0.0, std::sin(a)) * run;
                const double y = heap.highestUnder(s, q);
                if (y < low && !blocked(q, e, y, 2.0 * e.y))
                {
                    low = y;
                    next = q;
                }
            }
            if (next == s.center)
                return;
            s.center = next;
        }
    };
    std::vector<size_t> fallenOrder;
    for (size_t k = 0; k < n; ++k)
        if (!keep[k])
        {
            ++st.removed;
            if (stones[k].role == Role::Slab)
                ++st.fallenPieces;
            if (unitRandom(mixKey(p.seed, 409, k)) < p.rubble)
                fallenOrder.push_back(k);
        }
    std::stable_sort(fallenOrder.begin(), fallenOrder.end(), [&](size_t a, size_t b) { return stones[a].center.y < stones[b].center.y; });
    std::vector<size_t> rubbleIndex;
    for (size_t k : fallenOrder)
    {
        Stone s = stones[k];
        const auto [g, t] = cols.near(s.center);
        const double fall = std::max(0.0, s.center.y - g);
        glm::dvec3 out = s.t1 >= s.t0 ? -s.frame[2] : glm::dvec3(0.0);
        if (s.face == Face::Through || s.t1 < s.t0)
        {
            const double a = unitRandom(mixKey(p.seed, 410, k)) * 6.283185307179586;
            out = glm::dvec3(std::cos(a), 0.0, std::sin(a));
        }
        out.y = 0.0;
        if (glm::length(out) > 1e-9)
            out = glm::normalize(out);
        const double throwOut = (0.15 + 0.6 * unitRandom(mixKey(p.seed, 411, k))) * fall * 0.5 + s.size.z;
        const double toward = std::clamp(p.slope, 0.0, 1.0) * unitRandom(mixKey(p.seed, 412, k)) * fall * 0.4;
        glm::dvec3 land = s.center + out * throwOut + towards * toward;
        const double yaw = unitRandom(mixKey(p.seed, 413, k)) * 6.283185307179586;
        const double lx = (unitRandom(mixKey(p.seed, 414, k)) - 0.5) * 0.2, lz = (unitRandom(mixKey(p.seed, 415, k)) - 0.5) * 0.2;
        if (s.role != Role::Slab)
            layDown(s, yaw, lx, lz);
        s.center = land;
        const glm::dvec3 e = halfExtentAabb(s);
        // Off the standing walls: further out until the ground there is clear.
        const glm::dvec3 away = glm::length(out) > 1e-9 ? out : towards;
        // The lowest of a few spots around where it lands, so heaps spread instead of stacking into towers.
        {
            glm::dvec3 best = s.center;
            double bestY = heap.highestUnder(s, s.center);
            const double reach = std::max({e.x, e.z, unit}) * 1.5;
            for (int c = 0; c < 6; ++c)
            {
                const double a = unitRandom(mixKey(p.seed, 416, k, c)) * 6.283185307179586;
                const glm::dvec3 at = s.center + glm::dvec3(std::cos(a), 0.0, std::sin(a)) * reach * (0.5 + 0.5 * unitRandom(mixKey(p.seed, 417, k, c)));
                const double y = heap.highestUnder(s, at);
                if (y < bestY - 1e-9)
                {
                    bestY = y;
                    best = at;
                }
            }
            s.center = best;
        }
        int step = 0;
        for (; step < 12 && blocked(s.center, e * 0.8, heap.highestUnder(s, s.center), 2.0 * e.y); ++step)
            s.center += away * std::max({e.x, e.z, unit});
        // Nowhere clear to land: it is gone with the rest.
        if (step == 12)
            continue;
        slideDownHeap(s, e * 0.8, 2.0 * std::max({e.x, e.z, unit * 0.25}), mixKey(p.seed, 418, k));
        s.center.y = heap.highestUnder(s, s.center) + e.y;
        // A heap filling an enclosed floor stops well below the walls around it; the rest is gone.
        if (const auto [lg, lt] = cols.near(s.center); s.center.y - e.y > lg + 0.4 * std::max(lt - lg, unit))
            continue;
        heap.raiseUnder(s, s.center.y + e.y);
        s.role = s.role == Role::Slab ? Role::Slab : Role::Rubble;
        s.t1 = -1.0;
        s.t0 = 0.0;
        s.loose = true;
        s.grounded = true;
        s.damage = 1.0f;
        stones[k] = s;
        keep[k] = 3;
        rubbleIndex.push_back(k);
        ++st.rubble;
    }
    if (p.settle)
        settleRubble(stones, rubbleIndex, cols.ground, st);

    std::vector<Stone> out;
    out.reserve(n);
    double left = 0.0;
    for (size_t k = 0; k < n; ++k)
        if (keep[k])
        {
            if (keep[k] != 3 && may(k))
                left += volumeOf(stones[k]);
            out.push_back(stones[k]);
            if (keptOut)
                keptOut->push_back(int(k));
        }
    st.left = left / std::max(total, 1e-12);
    stones = std::move(out);
    return st;
}

void ageStones(std::vector<Stone>& stones, const AgeParams& p)
{
    LILA_ZONE_N("Masonry age");
    if (stones.empty())
        return;
    const double unit = typicalSize(stones);
    Columns cols;
    cols.build(stones, unit * 4.0, std::any_of(stones.begin(), stones.end(), [](const Stone& s) { return s.grounded; }));
    const double wetHeight = p.wetHeight > 0.0 ? p.wetHeight : std::max(unit, (cols.top - cols.ground) * 0.2);
    StoneGrid grid;
    grid.cell = gridCell(stones);
    grid.build(stones, 0.0);
    const double pad = std::max(0.5, unit * 0.12);
    const double age = std::clamp(p.age, 0.0, 1.0), moss = std::clamp(p.moss, 0.0, 1.0);
    for (size_t k = 0; k < stones.size(); ++k)
    {
        Stone& s = stones[k];
        bool covered = false;
        for (int o : grid.around(s, pad))
            if (size_t(o) != k && stones[size_t(o)].center.y > s.center.y + 0.25 * s.size.y && touching(s, stones[size_t(o)], pad))
            {
                covered = true;
                break;
            }
        const double top = covered ? 0.0 : 1.0;
        const double ground = cols.near(s.center).first;
        const double bottom = s.center.y - halfExtentAabb(s).y;
        const double damp = std::clamp(1.0 - (bottom - ground) / wetHeight, 0.0, 1.0);
        const double patch = fbm(s.center / (unit * 6.0), p.seed + 9);
        const double face = s.role == Role::Rubble ? 1.0 : (s.face == Face::Inner ? 0.5 : 0.8);
        s.wet = float(std::clamp(0.75 * damp + 0.35 * top * (0.4 + 0.6 * patch), 0.0, 1.0));
        s.age = float(std::clamp(age * (0.65 + 0.35 * unitRandom(mixKey(p.seed, 501, k))) + 0.25 * age * s.damage, 0.0, 1.0));
        s.moss = float(std::clamp(moss * age * (0.6 * s.wet + 0.5 * top * face) * (0.3 + 1.0 * patch), 0.0, 1.0));
    }
}

int countFloating(std::span<const Stone> stones, double tolerance, std::vector<int>* which)
{
    StoneGrid grid;
    grid.cell = gridCell(stones);
    grid.build(stones, 0.0);
    int count = 0;
    for (size_t k = 0; k < stones.size(); ++k)
    {
        const Stone& s = stones[k];
        if (s.grounded)
            continue;
        bool held = false;
        for (int o : grid.around(s, std::max(tolerance, 1.1 * s.size.y)))
        {
            const Stone& t = stones[size_t(o)];
            if (size_t(o) == k)
                continue;
            const bool lower = t.center.y < s.center.y - 0.1 * std::min(s.size.y, t.size.y);
            if ((lower && restsOn(s, t, tolerance)) || (s.role == Role::Slab && touching(s, t, tolerance)))
            {
                held = true;
                break;
            }
        }
        count += held ? 0 : 1;
        if (!held && which)
            which->push_back(int(k));
    }
    return count;
}

int countStacked(std::span<const Stone> stones, double height, std::vector<int>* which, double* highest)
{
    std::vector<Stone> standing;
    for (const Stone& s : stones)
        if (!isRubble(s))
            standing.push_back(s);
    if (standing.empty())
        standing.assign(stones.begin(), stones.end());
    Columns cols;
    cols.build(standing, typicalSize(standing) * 4.0, std::any_of(standing.begin(), standing.end(), [](const Stone& s) { return s.grounded; }));
    int count = 0;
    for (size_t k = 0; k < stones.size(); ++k)
    {
        const Stone& s = stones[k];
        if (!isRubble(s))
            continue;
        const double above = s.center.y - halfExtentAabb(s).y - cols.near(s.center).first;
        if (highest)
            *highest = std::max(*highest, above);
        if (above <= height)
            continue;
        ++count;
        if (which)
            which->push_back(int(k));
    }
    return count;
}

namespace
{
/**
 * The slab with the walls it reaches into cut away, a little more than their thickness so the faces' wander and
 * protrusion stay clear: a floor fits inside its walls, down to the slab's underside where it lies at their foot.
 * Walls it only sits on (a roof) are left whole.
 */
geo::Mesh fitInsideWalls(const geo::Mesh& slab, const SlabPlan& sl, const Plan& plan)
{
    geo::Bounds sb = slab.bounds();
    std::vector<geo::Mesh> hulls;
    for (const WallPlan& wp : plan.walls)
    {
        WallParams p = wp.params;
        const double clear = std::max(0.0, p.protrude) + p.stone.joint * 0.5;
        p.thickness = std::max(p.thickness, 1.0) + 2.0 * clear;
        for (geo::Mesh& h : collisionHulls(wp.footprint, p, 1))
        {
            const geo::Bounds hb = h.bounds();
            if (hb.min.y >= sl.y1 + p.stone.joint || hb.max.y <= sl.y0 + p.stone.joint || hb.min.x >= sb.max.x || hb.max.x <= sb.min.x ||
                hb.min.z >= sb.max.z || hb.max.z <= sb.min.z)
                continue;
            for (glm::dvec3& q : h.positions())
                if (q.y <= hb.min.y + 1e-6)
                    q.y = std::min(q.y, sb.min.y - 1.0);
            hulls.push_back(std::move(h));
        }
    }
    if (hulls.empty())
        return slab;
    std::vector<lila::scene::BooleanOperand> ops{{&slab, glm::dmat4(1.0)}};
    for (const geo::Mesh& h : hulls)
        ops.push_back({&h, glm::dmat4(1.0)});
    lila::scene::BooleanOptions bo;
    bo.op = lila::scene::BooleanOp::Difference;
    bo.cleanup = false;
    bo.splitNgons = false;
    const lila::scene::BooleanResult r = lila::scene::meshBoolean(ops, glm::dmat4(1.0), bo);
    if (!r.ok())
    {
        lila::log::warn("Masonry: a floor could not be fitted inside its walls ({}), so it is laid whole", r.error);
        return slab;
    }
    return r.mesh;
}
} // namespace

Built realizePlan(const Plan& plan, const RuinParams& ruin, const AgeParams& age, bool lods, RuinStats* statsOut)
{
    LILA_ZONE_N("Masonry realize");
    Built b;
    b.regions = plan.regions;
    for (size_t w = 0; w < plan.walls.size(); ++w)
    {
        const WallPlan& wp = plan.walls[w];
        Layout l = layoutWall(wp.footprint, wp.params, int(w));
        b.courses = std::max(b.courses, l.courses);
        b.pitch = std::max(b.pitch, l.pitch);
        b.trimmed += l.trimmed;
        b.dropped += l.dropped;
        for (Stone& s : l.stones)
        {
            const int opening = -1 - s.region;
            s.region = s.region < 0 && size_t(opening) < wp.openingRegions.size() ? wp.openingRegions[size_t(opening)] : wp.region;
        }
        b.stones.insert(b.stones.end(), l.stones.begin(), l.stones.end());
    }
    // Without walls there are no courses: the nominal course height scales the check's tolerances instead.
    if (b.pitch <= 0.0)
        b.pitch = plan.pitch;
    {
        // Paving is laid out to its outline, so cut it back where it runs into the walls.
        std::vector<Stone> paving;
        for (const Stone& s : plan.elements)
            if (s.role == Role::Paving)
                paving.push_back(s);
        if (!paving.empty() && !b.stones.empty())
            b.dropped += fitAgainst(paving, b.stones, plan.look.wall.stone.joint);
        for (Stone s : plan.elements)
            if (s.role != Role::Paving)
            {
                s.wall = -1;
                b.stones.push_back(s);
            }
        for (Stone& s : paving)
        {
            s.wall = -1;
            b.stones.push_back(s);
        }
    }
    for (size_t k = 0; k < plan.slabs.size(); ++k)
    {
        const SlabPlan& sl = plan.slabs[k];
        if (sl.outline.size() < 3)
            continue;
        const geo::Mesh slab = fitInsideWalls(slabMesh(sl.outline, sl.y0, sl.y1), sl, plan);
        if (slab.numFaces() == 0)
            continue;
        std::vector<geo::Mesh> parts;
        if (ruin.active())
        {
            double area = 0.0;
            glm::dvec3 lo(1e300), hi(-1e300);
            for (size_t i = 0; i < sl.outline.size(); ++i)
            {
                const glm::dvec3& a = sl.outline[i];
                const glm::dvec3& c = sl.outline[(i + 1) % sl.outline.size()];
                area += 0.5 * (a.x * c.z - c.x * a.z);
                lo = glm::min(lo, a);
                hi = glm::max(hi, a);
            }
            const double cell = std::max(plan.pitch * 6.0, std::sqrt(std::abs(area)) / 7.0);
            const int count = std::clamp(int(std::abs(area) / (cell * cell)), 4, 64);
            std::vector<glm::dvec3> seeds;
            for (int i = 0; seeds.size() < size_t(count) && i < count * 50; ++i)
            {
                const double x = lo.x + (hi.x - lo.x) * unitRandom(mixKey(ruin.seed, 601, k, i, 1));
                const double z = lo.z + (hi.z - lo.z) * unitRandom(mixKey(ruin.seed, 601, k, i, 2));
                if (inside(sl.outline, x, z))
                    seeds.push_back({x, (sl.y0 + sl.y1) * 0.5, z});
            }
            lila::scene::fracture::FractureOptions fo;
            fo.seed = ruin.seed + k;
            const auto fr = lila::scene::fracture::voronoiFracture(slab, seeds, fo);
            if (fr.ok())
                for (const auto& piece : fr.pieces)
                    parts.push_back(piece.mesh);
        }
        if (parts.empty())
            parts.push_back(slab);
        for (geo::Mesh& m : parts)
        {
            const geo::Bounds bb = m.bounds();
            const glm::dvec3 c = (bb.min + bb.max) * 0.5;
            for (glm::dvec3& q : m.positions())
                q -= c;
            Stone s;
            s.center = c;
            s.size = glm::max(bb.max - bb.min, glm::dvec3(0.1));
            s.role = Role::Slab;
            s.face = Face::Through;
            s.region = sl.region;
            s.wall = -1;
            s.mesh = int32_t(b.pieces.size());
            s.random = float(unitRandom(mixKey(ruin.seed, 602, b.pieces.size())));
            b.pieces.push_back(std::move(m));
            b.stones.push_back(s);
        }
    }

    // Loose elements (columns, stairs, vaults) and floors stand on the ground where they reach down to the walls' ground around them.
    {
        std::vector<Stone> walls;
        for (const Stone& s : b.stones)
            if (s.wall >= 0)
                walls.push_back(s);
        Columns ground;
        ground.build(walls.empty() ? std::span<const Stone>(b.stones) : std::span<const Stone>(walls), std::max(plan.pitch, 1.0) * 4.0);
        for (Stone& s : b.stones)
            if (s.wall < 0 && !s.grounded)
                s.grounded = s.center.y - halfExtentAabb(s).y <= ground.near(s.center).first + std::max(plan.pitch, 1.0) * 0.5;
    }

    RuinStats stats;
    if (ruin.active())
    {
        for (const Stone& s : b.stones)
            stats.slabPieces += s.role == Role::Slab ? 1 : 0;
        const int pieces = stats.slabPieces;
        std::vector<char> eligible;
        if (!ruin.regions.empty())
        {
            std::vector<char> regionOk(b.regions.size(), 0);
            for (size_t r = 0; r < b.regions.size(); ++r)
                for (const std::string& want : ruin.regions)
                    regionOk[r] = regionOk[r] || b.regions[r] == want || b.regions[r].starts_with(want + "/");
            for (const Stone& s : b.stones)
                eligible.push_back(s.region >= 0 && size_t(s.region) < regionOk.size() ? regionOk[size_t(s.region)] : 0);
        }
        stats = ruinStones(b.stones, ruin, plan.pitch, eligible);
        stats.slabPieces = pieces;
    }
    ageStones(b.stones, age);

    for (size_t w = 0; w < plan.walls.size(); ++w)
        for (geo::Mesh& cell : coreCells(plan.walls[w].footprint, plan.walls[w].params, b.stones, int(w)))
        {
            b.core.append(cell);
            b.coreHulls.push_back(std::move(cell));
        }
    for (const geo::Mesh& cell : plan.elementCore)
    {
        b.core.append(cell);
        b.coreHulls.push_back(cell);
    }
    for (const Stone& s : b.stones)
        if (s.role == Role::Slab && !s.loose && s.mesh >= 0 && size_t(s.mesh) < b.pieces.size())
            for (geo::Mesh& h : slabHulls(s, b.pieces[size_t(s.mesh)]))
                b.coreHulls.push_back(std::move(h));
    if (b.core.numFaces() > 0)
        lila::scene::materials::assign(b.core, kMortarSlot, {});
    // Full thickness cells for the low poly shells, following what stands.
    for (size_t w = 0; w < plan.walls.size(); ++w)
    {
        WallParams full = plan.walls[w].params;
        full.recess = 0.0;
        const double grid = std::max(200.0, full.stone.length * 4.0);
        for (geo::Mesh& cell : coreCells(plan.walls[w].footprint, full, b.stones, int(w), grid))
            b.shellCells.push_back(std::move(cell));
    }

    BuildOptions look = plan.look;
    look.lods = lods;
    makeMeshes(b, look);
    // Stones off the courses take their height from where they stand in the whole structure.
    double lo = 1e300, hi = -1e300;
    for (const Stone& s : b.stones)
    {
        lo = std::min(lo, s.center.y);
        hi = std::max(hi, s.center.y);
    }
    for (size_t k = 0; k < b.stones.size(); ++k)
        if (b.stones[k].wall < 0 || b.stones[k].role == Role::Rubble)
            b.heightFraction[k] = hi > lo ? float((b.stones[k].center.y - lo) / (hi - lo)) : 0.0f;
    if (age.plaster > 0.0)
        b.dressing.append(plasterMesh(b.stones, b.heightFraction, age, plan.look.wall.stone.joint));
    if (age.ivy > 0.0)
        b.dressing.append(ivyMesh(b.stones, age));
    if (statsOut)
        *statsOut = stats;
    return b;
}
} // namespace masonry
