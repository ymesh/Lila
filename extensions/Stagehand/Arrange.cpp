#include "Arrange.h"

#include <lila/Profile.h>

#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtc/quaternion.hpp>

#include <algorithm>
#include <cmath>
#include <map>
#include <random>
#include <unordered_map>

namespace stagehand::arrange
{
namespace
{
using lila::geo::Index;
using lila::geo::Mesh;
namespace physics = lila::physics;

constexpr double kPi = 3.14159265358979323846;

struct Rng
{
    std::mt19937 engine;
    explicit Rng(uint32_t seed) : engine(seed * 2654435761u + 1013904223u) {}
    double u() { return std::uniform_real_distribution<double>(0.0, 1.0)(engine); }
    double range(double a, double b) { return a + (b - a) * u(); }
    double signedUnit() { return range(-1.0, 1.0); }
    int pick(size_t n) { return n <= 1 ? 0 : int(std::uniform_int_distribution<size_t>(0, n - 1)(engine)); }
};

/** Arc-length lookup on a polyline. */
struct Polyline
{
    std::vector<glm::dvec3> pts;
    std::vector<double> at; // arc length at each point

    explicit Polyline(std::span<const glm::dvec3> line) : pts(line.begin(), line.end())
    {
        at.push_back(0.0);
        for (size_t k = 1; k < pts.size(); ++k)
            at.push_back(at.back() + glm::length(pts[k] - pts[k - 1]));
    }
    double length() const { return at.empty() ? 0.0 : at.back(); }

    /** Point and horizontal tangent at arc length s. */
    void sample(double s, glm::dvec3& p, glm::dvec3& t) const
    {
        size_t k = size_t(std::upper_bound(at.begin(), at.end(), s) - at.begin());
        k = std::clamp<size_t>(k, 1, pts.size() - 1);
        const double seg = std::max(at[k] - at[k - 1], 1e-12);
        const double f = std::clamp((s - at[k - 1]) / seg, 0.0, 1.0);
        p = glm::mix(pts[k - 1], pts[k], f);
        t = pts[k] - pts[k - 1];
        t.y = 0.0;
        t = glm::length(t) > 1e-12 ? glm::normalize(t) : glm::dvec3(1, 0, 0);
    }
};

/** Frame at a point: X along t, Y up, Z = X x Y. */
glm::dmat4 frameAt(const glm::dvec3& p, const glm::dvec3& t)
{
    const glm::dvec3 y(0, 1, 0);
    const glm::dvec3 z = glm::normalize(glm::cross(t, y));
    glm::dmat4 m(1.0);
    m[0] = glm::dvec4(t, 0);
    m[1] = glm::dvec4(y, 0);
    m[2] = glm::dvec4(z, 0);
    m[3] = glm::dvec4(p, 1);
    return m;
}

/** Bounds of an item's box after rotation r and uniform scale, about its centre. */
void turnedBounds(const Item& it, const glm::dmat3& r, double scale, glm::dvec3& lo, glm::dvec3& hi)
{
    const glm::dvec3 h = it.size() * 0.5 * scale;
    lo = glm::dvec3(1e300);
    hi = glm::dvec3(-1e300);
    for (int k = 0; k < 8; ++k)
    {
        const glm::dvec3 c((k & 1) ? h.x : -h.x, (k & 2) ? h.y : -h.y, (k & 4) ? h.z : -h.z);
        const glm::dvec3 q = r * c;
        lo = glm::min(lo, q);
        hi = glm::max(hi, q);
    }
}

/** frame * T(offset) * R * S * T(-centre): the item turned and scaled about its centre, then moved by offset in the frame. */
glm::dmat4 compose(const glm::dmat4& frame, const glm::dvec3& offset, const glm::dmat3& r, double scale, const Item& it)
{
    const glm::dvec3 c = (it.lo + it.hi) * 0.5;
    return frame * glm::translate(glm::dmat4(1.0), offset) * glm::dmat4(r) * glm::scale(glm::dmat4(1.0), glm::dvec3(scale)) *
           glm::translate(glm::dmat4(1.0), -c);
}

glm::dmat3 rotZ(double deg) { return glm::dmat3(glm::rotate(glm::dmat4(1.0), glm::radians(deg), glm::dvec3(0, 0, 1))); }
glm::dmat3 rotY(double deg) { return glm::dmat3(glm::rotate(glm::dmat4(1.0), glm::radians(deg), glm::dvec3(0, 1, 0))); }

/** Rotation taking +Y to n. */
glm::dmat3 alignUp(const glm::dvec3& n)
{
    const glm::dvec3 up(0, 1, 0), to = glm::normalize(n);
    const double d = glm::dot(up, to);
    if (d > 1.0 - 1e-9)
        return glm::dmat3(1.0);
    if (d < -1.0 + 1e-9)
        return glm::dmat3(glm::rotate(glm::dmat4(1.0), kPi, glm::dvec3(1, 0, 0)));
    return glm::mat3_cast(glm::normalize(glm::dquat(1.0 + d, glm::cross(up, to))));
}

glm::dmat3 randomTurn(Rng& rng)
{
    glm::dquat q(rng.signedUnit(), rng.signedUnit(), rng.signedUnit(), rng.signedUnit());
    if (glm::length(q) < 1e-6)
        q = glm::dquat(1, 0, 0, 0);
    return glm::mat3_cast(glm::normalize(q));
}
} // namespace

Item itemOf(const Mesh& m)
{
    Item it;
    it.lo = glm::dvec3(1e300);
    it.hi = glm::dvec3(-1e300);
    for (Index p = 0; p < m.pointCapacity(); ++p)
        if (m.pointValid(p))
        {
            it.lo = glm::min(it.lo, m.position(p));
            it.hi = glm::max(it.hi, m.position(p));
        }
    if (it.lo.x > it.hi.x)
        it = Item{};
    return it;
}

std::vector<Placed> row(std::span<const glm::dvec3> line, std::span<const Item> items, const RowOptions& o)
{
    LILA_ZONE();

    std::vector<Placed> out;
    if (line.size() < 2 || items.empty())
        return out;
    const Polyline pl(line);
    const double start = std::max(0.0, o.inset);
    const double end = std::min(pl.length() * std::clamp(o.fill, 0.0, 1.0), pl.length() - o.inset);
    Rng rng(o.seed);
    double s = start;
    bool lastLeaned = false;
    while (s < end)
    {
        const int k = rng.pick(items.size());
        const Item& it = items[size_t(k)];
        const double scale = rng.range(o.scaleMin, o.scaleMax);
        const float random = float(rng.u());

        if (rng.u() < o.lyingChance)
        {
            // A few lying flat, one on another: each turned so its thickness stands up and its height runs along the line.
            // The widest of them sets the room the stack takes.
            const int n = 1 + rng.pick(size_t(std::max(1, o.lyingMax)));
            const glm::dmat3 lie = rotZ(-90.0);
            struct Lying
            {
                int item;
                double scale;
                glm::dmat3 r;
                glm::dvec3 lo, hi;
            };
            std::vector<Lying> pile;
            double width = 0.0;
            for (int j = 0; j < n; ++j)
            {
                Lying l{j == 0 ? k : rng.pick(items.size()), j == 0 ? scale : rng.range(o.scaleMin, o.scaleMax), rotY(rng.signedUnit() * 6.0) * lie, {}, {}};
                turnedBounds(items[size_t(l.item)], l.r, l.scale, l.lo, l.hi);
                width = std::max(width, l.hi.x - l.lo.x);
                pile.push_back(l);
            }
            if (s + width > end)
                break;
            glm::dvec3 p, t;
            pl.sample(s + width * 0.5, p, t);
            double y = 0.0;
            for (const Lying& l : pile)
            {
                out.push_back({compose(frameAt(p, t), {0.0, y - l.lo.y, rng.signedUnit() * o.depthJitter}, l.r, l.scale, items[size_t(l.item)]), l.item,
                               Pose::Lying, float(rng.u())});
                y += l.hi.y - l.lo.y;
            }
            s += width + o.gap;
            lastLeaned = false;
            continue;
        }

        // A lean tips the top back onto the item before, about the bottom corner on the far side.
        const bool lean = !out.empty() && !lastLeaned && rng.u() < o.leanChance;
        const glm::dmat3 r = lean ? rotZ(rng.range(0.4, 1.0) * o.leanAngle) : glm::dmat3(1.0);
        glm::dvec3 lo, hi;
        turnedBounds(it, r, scale, lo, hi);
        const double width = hi.x - lo.x;
        if (s + width > end)
            break;
        glm::dvec3 p, t;
        pl.sample(s + width * 0.5, p, t);
        const glm::dvec3 offset(-(lo.x + hi.x) * 0.5, -lo.y, rng.signedUnit() * o.depthJitter);
        out.push_back({compose(frameAt(p, t), offset, r, scale, it), k, lean ? Pose::Leaning : Pose::Upright, random});
        lastLeaned = lean;
        // A leaning item overlaps the one it leans on at the bottom; the next one starts after its foot.
        s += width + std::max(0.0, o.gap * (1.0 + std::clamp(o.gapJitter, 0.0, 1.0) * rng.signedUnit()));
    }
    return out;
}

std::vector<Placed> stack(const glm::dvec3& base, std::span<const Item> items, const StackOptions& o)
{
    std::vector<Placed> out;
    if (items.empty())
        return out;
    Rng rng(o.seed);
    double y = 0.0;
    for (int n = 0; n < o.count; ++n)
    {
        const int k = rng.pick(items.size());
        const Item& it = items[size_t(k)];
        const double scale = rng.range(o.scaleMin, o.scaleMax);
        const glm::dmat3 r = rotY(rng.signedUnit() * o.turn);
        glm::dvec3 lo, hi;
        turnedBounds(it, r, scale, lo, hi);
        const glm::dvec3 offset(rng.signedUnit() * o.offset, y - lo.y, rng.signedUnit() * o.offset);
        out.push_back({compose(glm::translate(glm::dmat4(1.0), base), offset, r, scale, it), k, Pose::Lying, float(rng.u())});
        y += hi.y - lo.y;
    }
    return out;
}

std::vector<Placed> pile(const glm::dvec3& center, std::span<const Item> items, const PileOptions& o)
{
    LILA_ZONE();

    std::vector<Placed> out;
    if (items.empty() || o.count <= 0)
        return out;
    Rng rng(o.seed);
    // Spaced by the largest item's bounding sphere, any turn of anything fits its cell.
    double sphere = 0.0;
    for (const Item& it : items)
        sphere = std::max(sphere, glm::length(it.size()) * 0.5 * o.scaleMax);
    const double step = 2.0 * sphere + o.gap;
    std::vector<glm::dvec2> cells;
    const int reach = int(std::ceil(o.radius / step));
    for (int i = -reach; i <= reach; ++i)
        for (int j = -reach; j <= reach; ++j)
        {
            // Every other row shifted half a cell, so layers sit a little more like a heap.
            const glm::dvec2 c((i + (j % 2 ? 0.5 : 0.0)) * step, j * step * 0.866);
            if (glm::length(c) + sphere <= o.radius || (i == 0 && j == 0))
                cells.push_back(c);
        }
    int layer = 0;
    while (int(out.size()) < o.count)
    {
        std::vector<glm::dvec2> order = cells;
        std::shuffle(order.begin(), order.end(), rng.engine);
        for (const glm::dvec2& c : order)
        {
            if (int(out.size()) >= o.count)
                break;
            const int k = rng.pick(items.size());
            const double scale = rng.range(o.scaleMin, o.scaleMax);
            const glm::dvec3 at = center + glm::dvec3(c.x + rng.signedUnit() * o.gap * 0.4, sphere + layer * step, c.y + rng.signedUnit() * o.gap * 0.4);
            out.push_back({compose(glm::translate(glm::dmat4(1.0), at), glm::dvec3(0.0), randomTurn(rng), scale, items[size_t(k)]), k, Pose::Lying,
                           float(rng.u())});
        }
        ++layer;
    }
    return out;
}

std::vector<Placed> scatter(const Mesh& m, std::span<const Item> items, const ScatterOptions& o)
{
    LILA_ZONE();

    std::vector<Placed> out;
    if (items.empty())
        return out;
    struct Tri
    {
        glm::dvec3 a, b, c, n;
        Index pa, pb, pc;
        double area;
    };
    std::vector<Tri> tris;
    double total = 0.0;
    const std::span<const float> mask = o.mask.empty() ? std::span<const float>() : m.pointAttrs().get<float>(o.mask);
    const double cosMax = std::cos(glm::radians(std::clamp(o.slopeMax, 0.0, 180.0)));
    for (Index f = 0; f < m.faceCapacity(); ++f)
    {
        if (!m.faceValid(f))
            continue;
        const std::vector<Index> p = m.facePoints(f);
        for (size_t k = 1; k + 1 < p.size(); ++k)
        {
            Tri t{m.position(p[0]), m.position(p[k]), m.position(p[k + 1]), {}, p[0], p[k], p[k + 1], 0.0};
            const glm::dvec3 cr = glm::cross(t.b - t.a, t.c - t.a);
            t.area = glm::length(cr) * 0.5;
            if (t.area < 1e-12)
                continue;
            t.n = cr / (2.0 * t.area);
            if (t.n.y < cosMax)
                continue;
            tris.push_back(t);
            total += t.area;
        }
    }
    if (tris.empty())
        return out;
    std::vector<double> cdf;
    double run = 0.0;
    for (const Tri& t : tris)
        cdf.push_back(run += t.area);

    const int target = std::min(o.maxCount, int(std::ceil(o.density * total / 10000.0)));
    Rng rng(o.seed);
    // Accepted footprints on a grid of the largest footprint, so each test looks at a few neighbours only.
    double maxR = 1.0;
    for (const Item& it : items)
        maxR = std::max(maxR, std::max(it.size().x, it.size().z) * 0.5 * o.scaleMax * o.spacing);
    const double cell = 2.0 * maxR;
    std::unordered_map<int64_t, std::vector<std::pair<glm::dvec3, double>>> grid;
    auto key = [&](int64_t x, int64_t z) { return (x << 32) ^ (z & 0xffffffff); };

    for (int attempt = 0; attempt < target * 30 && int(out.size()) < target; ++attempt)
    {
        const double pick = rng.u() * total;
        const Tri& t = tris[size_t(std::lower_bound(cdf.begin(), cdf.end(), pick) - cdf.begin()) % tris.size()];
        double u = rng.u(), v = rng.u();
        if (u + v > 1.0)
            u = 1.0 - u, v = 1.0 - v;
        const glm::dvec3 p = t.a + (t.b - t.a) * u + (t.c - t.a) * v;
        if (!mask.empty())
        {
            const double w = mask[t.pa] * (1.0 - u - v) + mask[t.pb] * u + mask[t.pc] * v;
            if (rng.u() > std::clamp(w, 0.0, 1.0))
                continue;
        }
        const int k = rng.pick(items.size());
        const Item& it = items[size_t(k)];
        const double scale = rng.range(o.scaleMin, o.scaleMax);
        const double r = std::max(it.size().x, it.size().z) * 0.5 * scale * o.spacing;
        // Not inside or under anything in the way: a box over the footprint reaching above the surface there.
        bool blocked = false;
        for (const auto& [lo, hi] : o.avoid)
            if (p.x + r > lo.x && p.x - r < hi.x && p.z + r > lo.z && p.z - r < hi.z && hi.y > p.y + 0.5)
            {
                blocked = true;
                break;
            }
        if (blocked)
            continue;
        const int64_t cx = int64_t(std::floor(p.x / cell)), cz = int64_t(std::floor(p.z / cell));
        bool clear = true;
        for (int64_t dx = -1; dx <= 1 && clear; ++dx)
            for (int64_t dz = -1; dz <= 1 && clear; ++dz)
                if (auto g = grid.find(key(cx + dx, cz + dz)); g != grid.end())
                    for (const auto& [q, rq] : g->second)
                        if (glm::length(q - p) < r + rq)
                        {
                            clear = false;
                            break;
                        }
        if (!clear)
            continue;
        grid[key(cx, cz)].push_back({p, r});

        const glm::dmat3 upright(1.0), along = alignUp(t.n);
        const glm::dmat3 tilt = o.align <= 0.0 ? upright
                                                : glm::mat3_cast(glm::slerp(glm::dquat(1, 0, 0, 0), glm::quat_cast(along), std::clamp(o.align, 0.0, 1.0)));
        const glm::dmat3 rot = tilt * rotY(rng.signedUnit() * o.turn);
        // Bottom centre on the point.
        const glm::dvec3 c = (it.lo + it.hi) * 0.5;
        const glm::dvec3 bottom(c.x, it.lo.y, c.z);
        const glm::dmat4 xf = glm::translate(glm::dmat4(1.0), p) * glm::dmat4(rot) * glm::scale(glm::dmat4(1.0), glm::dvec3(scale)) *
                              glm::translate(glm::dmat4(1.0), -bottom);
        out.push_back({xf, k, Pose::Upright, float(rng.u())});
    }
    return out;
}

std::vector<Placed> path(std::span<const glm::dvec3> line, std::span<const Item> items, const PathOptions& o)
{
    LILA_ZONE();

    std::vector<Placed> out;
    if (line.size() < 2 || items.empty() || o.spacing <= 0.0)
        return out;
    const Polyline pl(line);
    Rng rng(o.seed);
    for (double s = 0.0; s <= pl.length() + 1e-9;)
    {
        glm::dvec3 p, t;
        pl.sample(s, p, t);
        const int k = rng.pick(items.size());
        const Item& it = items[size_t(k)];
        const double scale = rng.range(o.scaleMin, o.scaleMax);
        const double side = o.offset + rng.signedUnit() * o.spread;
        const double turn = o.along ? rng.signedUnit() * o.turn : rng.range(-180.0, 180.0);
        // Bottom centre on the line, then out to the side.
        const glm::dvec3 c = (it.lo + it.hi) * 0.5;
        const glm::dvec3 bottom(c.x, it.lo.y, c.z);
        const glm::dmat4 xf = frameAt(p, t) * glm::translate(glm::dmat4(1.0), glm::dvec3(0.0, 0.0, side)) * glm::dmat4(rotY(turn)) *
                              glm::scale(glm::dmat4(1.0), glm::dvec3(scale)) * glm::translate(glm::dmat4(1.0), -bottom);
        out.push_back({xf, k, Pose::Upright, float(rng.u())});
        s += o.spacing * (1.0 + rng.signedUnit() * std::clamp(o.spacingJitter, 0.0, 0.95));
    }
    return out;
}

std::vector<Placed> debris(std::span<const std::pair<glm::dvec3, glm::dvec3>> pieces, std::span<const Item> items, const DebrisOptions& o)
{
    LILA_ZONE();

    std::vector<Placed> out;
    if (pieces.empty() || items.empty() || o.amount <= 0.0)
        return out;
    std::vector<double> sizes;
    for (const auto& [lo, hi] : pieces)
        sizes.push_back(std::cbrt(std::max((hi.x - lo.x) * (hi.y - lo.y) * (hi.z - lo.z), 1e-6)));
    std::vector<double> sorted = sizes;
    std::nth_element(sorted.begin(), sorted.begin() + sorted.size() / 2, sorted.end());
    const double median = std::max(sorted[sorted.size() / 2], 1e-6);
    Rng rng(o.seed);
    for (size_t p = 0; p < pieces.size(); ++p)
    {
        const auto& [lo, hi] = pieces[p];
        const double want = o.amount * sizes[p] / median;
        int count = int(want);
        if (rng.u() < want - double(count))
            ++count;
        const glm::dvec3 centre = (lo + hi) * 0.5;
        const double radius = 0.5 * std::max(hi.x - lo.x, hi.z - lo.z);
        const double chip = std::clamp(o.size * sizes[p], o.minSize, o.maxSize);
        // Each chip starts a layer above the last, so none start inside another.
        double height = lo.y + chip;
        for (int c = 0; c < count; ++c)
        {
            const int k = rng.pick(items.size());
            const Item& it = items[size_t(k)];
            const double target = chip * rng.range(0.6, 1.4);
            const double scale = target / std::max({it.size().x, it.size().y, it.size().z, 1e-6});
            const double angle = rng.range(0.0, 2.0 * kPi);
            const double dist = radius + (0.15 + o.spread * std::sqrt(rng.u())) * std::max(radius, chip);
            const glm::dvec3 at(centre.x + std::cos(angle) * dist, height, centre.z + std::sin(angle) * dist);
            const glm::dquat q(rng.signedUnit(), rng.signedUnit(), rng.signedUnit(), rng.signedUnit());
            const glm::dquat turn = glm::length(q) > 1e-6 ? glm::normalize(q) : glm::dquat(1, 0, 0, 0);
            const glm::dvec3 c0 = (it.lo + it.hi) * 0.5;
            out.push_back({glm::translate(glm::dmat4(1.0), at) * glm::mat4_cast(turn) * glm::scale(glm::dmat4(1.0), glm::dvec3(scale)) *
                               glm::translate(glm::dmat4(1.0), -c0),
                           k, Pose::Lying, float(rng.u())});
            height += target * 1.8;
        }
    }
    return out;
}

Settled settle(std::span<const glm::dmat4> transforms, std::span<const int> items, std::span<const Mesh* const> itemMeshes,
               std::span<const Mesh* const> colliders, const SettleOptions& o)
{
    LILA_ZONE();

    physics::Settings st;
    st.gravity = o.gravity;
    st.ground = o.ground;
    st.groundHeight = o.groundHeight;
    st.collisionSteps = 2;
    physics::RigidScene s(st);

    // Shapes per (item, scale), shared by every placement of that size.
    std::map<std::pair<int, int64_t>, physics::Shape> shapes;
    std::vector<glm::dvec3> scales(transforms.size());
    for (size_t i = 0; i < transforms.size(); ++i)
    {
        const glm::dmat3 r3(transforms[i]);
        glm::dvec3 sc(glm::length(r3[0]), glm::length(r3[1]), glm::length(r3[2]));
        if (glm::determinant(r3) < 0.0)
            sc.x = -sc.x;
        scales[i] = sc;
        glm::dmat3 rot = r3;
        for (int c = 0; c < 3; ++c)
            rot[c] /= std::abs(sc[c]) > 1e-12 ? sc[c] : 1.0;

        const int item = i < items.size() ? items[i] : 0;
        const Mesh* mesh = item >= 0 && size_t(item) < itemMeshes.size() ? itemMeshes[size_t(item)] : nullptr;
        const int64_t scaleKey = int64_t(std::llround(sc.x * 1e4)) * 73856093 ^ int64_t(std::llround(sc.y * 1e4)) * 19349663 ^
                                 int64_t(std::llround(sc.z * 1e4)) * 83492791;
        auto [it, fresh] = shapes.try_emplace({item, scaleKey});
        if (fresh)
        {
            physics::Shape& sh = it->second;
            sh.rounding = o.rounding;
            auto pts = std::make_shared<std::vector<glm::dvec3>>();
            if (mesh)
                for (Index p = 0; p < mesh->pointCapacity(); ++p)
                    if (mesh->pointValid(p))
                        pts->push_back(mesh->position(p) * sc);
            // A box when the item nearly fills its bounds, as books and crates do; else its hull.
            const Item bounds = mesh ? itemOf(*mesh) : Item{};
            const glm::dvec3 size = bounds.size() * glm::abs(sc);
            double volume = 0.0;
            if (mesh)
                for (Index f = 0; f < mesh->faceCapacity(); ++f)
                {
                    if (!mesh->faceValid(f))
                        continue;
                    const std::vector<Index> fp = mesh->facePoints(f);
                    for (size_t k = 1; k + 1 < fp.size(); ++k)
                        volume += glm::dot(mesh->position(fp[0]) * sc, glm::cross(mesh->position(fp[k]) * sc, mesh->position(fp[k + 1]) * sc)) / 6.0;
                }
            if (std::abs(volume) >= 0.9 * size.x * size.y * size.z && size.x * size.y * size.z > 0.0)
            {
                sh.kind = physics::Shape::Kind::Box;
                sh.center = (bounds.lo + bounds.hi) * 0.5 * sc;
                sh.halfExtents = size * 0.5;
            }
            else
            {
                sh.kind = physics::Shape::Kind::Hull;
                sh.points = std::move(pts);
            }
        }
        physics::BodyDesc b;
        b.shape = it->second;
        b.position = glm::dvec3(transforms[i][3]);
        b.rotation = glm::normalize(glm::quat_cast(rot));
        b.density = o.density;
        b.friction = o.friction;
        b.bounce = o.bounce;
        s.add(b);
    }
    const size_t movers = transforms.size();
    for (const Mesh* c : colliders)
    {
        if (!c)
            continue;
        auto tris = std::make_shared<std::vector<glm::dvec3>>();
        for (Index f = 0; f < c->faceCapacity(); ++f)
        {
            if (!c->faceValid(f))
                continue;
            const std::vector<Index> p = c->facePoints(f);
            for (size_t k = 1; k + 1 < p.size(); ++k)
                for (Index v : {p[0], p[k], p[k + 1]})
                    tris->push_back(c->position(v));
        }
        physics::BodyDesc b;
        b.role = physics::Role::Static;
        b.shape.kind = physics::Shape::Kind::Mesh;
        b.shape.points = std::move(tris);
        b.friction = o.friction;
        s.add(b);
    }

    Settled out;
    out.seconds = s.settle(o.seconds);
    out.transforms.resize(movers);
    out.atRest.resize(movers);
    out.impact.assign(movers, 0.0f);
    out.masses.resize(movers);
    for (size_t i = 0; i < movers; ++i)
    {
        const physics::Pose p = s.pose(int(i));
        glm::dmat4 m = glm::mat4_cast(p.rotation);
        m[0] *= scales[i].x;
        m[1] *= scales[i].y;
        m[2] *= scales[i].z;
        m[3] = glm::dvec4(p.position, 1.0);
        out.transforms[i] = m;
        out.atRest[i] = s.asleep(int(i)) ? 1 : 0;
        out.masses[i] = s.mass(int(i));
    }
    for (physics::Contact c : s.contacts())
    {
        for (int* b : {&c.a, &c.b})
            if (*b >= int(movers))
                *b = -2;
        if (c.a >= 0)
            out.impact[size_t(c.a)] = std::max(out.impact[size_t(c.a)], float(c.peakImpulse));
        if (c.b >= 0)
            out.impact[size_t(c.b)] = std::max(out.impact[size_t(c.b)], float(c.peakImpulse));
        out.contacts.push_back(c);
    }
    return out;
}
} // namespace stagehand::arrange
