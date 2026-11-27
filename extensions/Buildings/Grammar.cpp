// The building grammar: footprint -> tiers -> floors -> facades -> bays -> modules, then roofs, roof clutter, extras,
// floor plates and proxies. One call per building, pure, any thread.

#include "Buildings.h"
#include "Interior.h"

#include "RooftopLayout.h"

#include <lila/Curves.h>
#include <lila/Profile.h>

#include "curve/Roof.h"

#include <algorithm>
#include <tuple>
#include <cmath>
#include <numbers>

namespace buildings
{
namespace
{
constexpr double kPi = std::numbers::pi;
const Vec3 kUp{0.0, 1.0, 0.0};
/** cm: the widest footprint a pitched roof spans. */
constexpr double kWidestPitched = 4000.0;

/** The ring's narrowest width: over each side's direction, the farthest any corner stands from that side. */
double ringWidth(const Ring& r)
{
    double best = std::numeric_limits<double>::max();
    for (size_t e = 0; e < r.size(); ++e)
    {
        const Vec2 a = r[e], d = r[(e + 1) % r.size()] - a;
        const double len = glm::length(d);
        if (len < 1.0)
            continue;
        const Vec2 n(-d.y / len, d.x / len);
        double far = 0.0;
        for (const Vec2& q : r)
            far = std::max(far, std::abs(glm::dot(q - a, n)));
        best = std::min(best, far);
    }
    return best == std::numeric_limits<double>::max() ? 0.0 : best;
}

struct TierPlan
{
    Ring ring;
    std::vector<int> sides;
    std::vector<std::string> edgeIds; // facade id per edge ("f3", "f3x" for a chamfer)
    std::vector<Ring> holes;
    double bottom = 0.0, top = 0.0; // walls, world Y
    struct Floor
    {
        double y0, y1;
        int index; // building floor number
    };
    std::vector<Floor> floors;
    bool roofed = true; // gets the building's roof (else a terrace or a tower cap)
    std::string cap;    // tower caps: dome, pyramid
    std::vector<double> clear; // per edge: cm to the nearest neighbour in front of it (huge when unknown)
};

struct Ctx
{
    const Spec& spec;
    Style st;
    const Edits& ed;
    Result& r;
    uint64_t key = 0;
    double floor0 = 0.0, groundLo = 0.0, groundHi = 0.0, H0 = 0.0, H = 0.0;
    int floors = 1;
    bool business = false;
    bool slope = false;
    std::string program; // the interior program, for the rooms fake interiors show
    int glazing = 0;     // placeholder variant offset: 10 glazes enterable buildings clear
    glm::vec4 cRender{1.0f}, cTrim{1.0f}, cBase{1.0f}, cRoof{1.0f};
    int bayCounter = 0;

    double rnd(uint64_t salt) const { return unitRandom(key, salt); }
    double rnd(std::string_view s) const { return unitRandom(key, fnv(s)); }

    int addModule(Module m)
    {
        r.modules.push_back(std::move(m));
        return int(r.modules.size()) - 1;
    }
};

// A facade's frame: s along the edge from a, t up (world Y), d into the wall.
struct Frame
{
    Vec3 a{0.0};
    Vec3 u{1, 0, 0};
    Vec3 n{0, 0, 1};
    double uOffset = 0.0;
    Vec3 at(double s, double t, double d = 0.0) const { return Vec3(a.x, 0.0, a.z) + u * s + kUp * t - n * d; }
};

Frame frameOf(Vec2 a, Vec2 b, double uOffset)
{
    Frame f;
    f.a = {a.x, 0.0, a.y};
    const Vec2 d = b - a;
    const double l = std::max(glm::length(d), 1e-9);
    f.u = {d.x / l, 0.0, d.y / l};
    const Vec2 n = outward(a, b);
    f.n = {n.x, 0.0, n.y};
    f.uOffset = uOffset;
    return f;
}

glm::vec2 uvOf(double u, double t, double base, int trim)
{
    if (trim < 0)
        return {float(u / 200.0), float(t / 200.0)};
    double v0, v1;
    trimRange(trim, v0, v1);
    const double h = trimStrip(trim).worldH;
    const double f = std::clamp((t - base) / h, 0.0, 1.0);
    return {float(u / kSheetWorld), float(v0 + f * (v1 - v0))};
}

/** A quad p0, p0+A, p0+A+B, p0+B (normal along A x B). */
void quad(Soup& s, Vec3 p0, Vec3 A, Vec3 B, const glm::vec2 uv[4], int m, int trim, int owner, int part, int bay, glm::vec4 c)
{
    const Vec3 p[4] = {p0, p0 + A, p0 + A + B, p0 + B};
    s.poly(p, uv, 4, m, trim, owner, part, bay, c);
}

/** A rectangle on the facade plane at depth d facing out, uv laid on the trim, cut into courses as tall as the strip. */
void wallRect(Soup& s, const Frame& f, double s0, double s1, double t0, double t1, double d, double base, int m, int trim, int owner, int part,
              int bay, glm::vec4 c)
{
    if (s1 - s0 < 0.5 || t1 - t0 < 0.5)
        return;
    const double h = trim >= 0 ? trimStrip(trim).worldH : 1e9;
    double c0 = t0;
    while (c0 < t1 - 0.5)
    {
        const double courseBase = trim >= 0 ? base + std::floor((c0 - base) / h + 1e-9) * h : base;
        const double c1 = std::min(t1, courseBase + h);
        const glm::vec2 uv[4] = {uvOf(f.uOffset + s0, c0, courseBase, trim), uvOf(f.uOffset + s0, c1, courseBase, trim),
                                 uvOf(f.uOffset + s1, c1, courseBase, trim), uvOf(f.uOffset + s1, c0, courseBase, trim)};
        quad(s, f.at(s0, c0, d), kUp * (c1 - c0), f.u * (s1 - s0), uv, m, trim, owner, part, bay, c);
        c0 = c1;
    }
}

/** A box sticking out of the facade: s0..s1 along, t0..t1 up, from depth 0 out to `out`. Five faces (no back). */
void facadeBox(Soup& s, const Frame& f, double s0, double s1, double t0, double t1, double out, int m, int trim, int owner, int part, glm::vec4 c,
               bool top = true, bool bottom = true)
{
    wallRect(s, f, s0, s1, t0, t1, -out, t0, m, trim, owner, part, -1, c);
    auto uvRect = [&](double a0, double a1, double b0, double b1) {
        const glm::vec2 uv[4] = {uvOf(a0, b0, b0, trim), uvOf(a0, b1, b0, trim), uvOf(a1, b1, b0, trim), uvOf(a1, b0, b0, trim)};
        return std::array<glm::vec2, 4>{uv[0], uv[1], uv[2], uv[3]};
    };
    // Sides: at s0 facing -u, at s1 facing +u. Depth runs from the front (d = -out) back to the wall (d = 0).
    {
        const auto uv = uvRect(0, out, t0, t1);
        quad(s, f.at(s0, t0, 0.0), kUp * (t1 - t0), f.n * out, uv.data(), m, trim, owner, part, -1, c);
        quad(s, f.at(s1, t0, -out), kUp * (t1 - t0), -f.n * out, uv.data(), m, trim, owner, part, -1, c);
    }
    if (top)
    {
        const auto uv = uvRect(f.uOffset + s0, f.uOffset + s1, t1, t1 + out);
        quad(s, f.at(s0, t1, -out), -f.n * out, f.u * (s1 - s0), uv.data(), m, trim, owner, part, -1, c);
    }
    if (bottom)
    {
        const auto uv = uvRect(f.uOffset + s0, f.uOffset + s1, t0, t0 + out);
        quad(s, f.at(s0, t0, 0.0), f.n * out, f.u * (s1 - s0), uv.data(), m, trim, owner, part, -1, c);
    }
}

/** Reveals of an opening: left, right, sill and head, from the facade plane in to depth. */
void reveals(Soup& s, const Frame& f, double os0, double os1, double ot0, double ot1, double d0, double depth, int m, int owner, int bay, glm::vec4 c,
             bool sill = true)
{
    const double w = os1 - os0, h = ot1 - ot0;
    auto uv = [&](double a, double b) {
        const glm::vec2 q[4] = {uvOf(0, 0, 0, TrimRender), uvOf(0, b, 0, TrimRender), uvOf(a, b, 0, TrimRender), uvOf(a, 0, 0, TrimRender)};
        return std::array<glm::vec2, 4>{q[0], q[1], q[2], q[3]};
    };
    const Vec3 in = -f.n * depth;
    {
        const auto q = uv(depth, h);
        quad(s, f.at(os0, ot0, d0), kUp * h, in, q.data(), m, TrimRender, owner, PartReveal, bay, c);
        quad(s, f.at(os1, ot0, d0 + depth), kUp * h, -in, q.data(), m, TrimRender, owner, PartReveal, bay, c);
    }
    {
        const auto q = uv(w, depth);
        if (sill)
            quad(s, f.at(os0, ot0, d0), in, f.u * w, q.data(), m, TrimRender, owner, PartReveal, bay, c);
        quad(s, f.at(os0, ot1, d0 + depth), -in, f.u * w, q.data(), m, TrimRender, owner, PartReveal, bay, c);
    }
}

Module wallModule(const Frame& f, double s, double t, double d, const std::string& type, const std::string& kind, const std::string& path, Vec3 size,
                  int variant = 0)
{
    Module m;
    m.type = type;
    m.kind = kind;
    m.path = path;
    m.origin = f.at(s, t, d);
    m.z = f.n;
    m.y = kUp;
    m.x = glm::cross(m.y, m.z);
    m.size = size;
    m.variant = variant;
    return m;
}

Module groundModule(Vec3 at, Vec2 facing, const std::string& type, const std::string& kind, const std::string& path, Vec3 size, int variant = 0)
{
    Module m;
    m.type = type;
    m.kind = kind;
    m.path = path;
    m.origin = at;
    const double l = glm::length(facing);
    m.z = l > 1e-9 ? Vec3(facing.x / l, 0.0, facing.y / l) : Vec3(0, 0, 1);
    m.y = kUp;
    m.x = glm::cross(m.y, m.z);
    m.size = size;
    m.variant = variant;
    return m;
}

std::string P(const std::string& facade, int floor, int bay) { return facade + "/l" + std::to_string(floor) + "/b" + std::to_string(bay); }

std::string doorTypeFor(const Ctx& c, bool business)
{
    if (business)
        return "door.shop";
    return c.st.doorKind.empty() ? "door.house" : c.st.doorKind;
}

// Tiers ---------------------------------------------------------------------------------------------------------------------

/** Base ring with chamfered corners where two street sides meet (corner = chamfer), keeping each edge's original id. */
void baseRing(const Ctx& c, Ring& ring, std::vector<int>& sides, std::vector<std::string>& ids)
{
    const Ring& fp = c.spec.footprint;
    const size_t n = fp.size();
    const bool chamfer = c.spec.corner == "chamfer";
    for (size_t i = 0; i < n; ++i)
    {
        const int sPrev = c.spec.sides[(i + n - 1) % n], sHere = c.spec.sides[i];
        const Vec2 a = fp[(i + n - 1) % n], b = fp[i], d = fp[(i + 1) % n];
        const double turn = (b.x - a.x) * (d.y - b.y) - (b.y - a.y) * (d.x - b.x);
        const double cut = std::min({200.0, glm::length(b - a) * 0.3, glm::length(d - b) * 0.3});
        if (chamfer && sPrev == 0 && sHere == 0 && turn > 0.0 && cut > 60.0)
        {
            // The corner point is replaced by two: the chamfer edge between them faces the junction.
            ring.push_back(b + glm::normalize(a - b) * cut);
            sides.push_back(0);
            ids.push_back("f" + std::to_string((i + n - 1) % n) + "x");
            ring.push_back(b + glm::normalize(d - b) * cut);
        }
        else
            ring.push_back(b);
        sides.push_back(sHere);
        ids.push_back("f" + std::to_string(i));
    }
    // ids/sides were pushed one per point after it; edge k runs ring[k] -> ring[k+1], so rotate them back by one entry
    // for the corner points that were split.
    std::vector<int> s2(ring.size());
    std::vector<std::string> i2(ring.size());
    // Rebuild directly: walk the ring again pairing each edge with the original edge it lies on.
    for (size_t k = 0; k < ring.size(); ++k)
    {
        const Vec2 p = ring[k], q = ring[(k + 1) % ring.size()];
        const Vec2 mid = 0.5 * (p + q);
        int best = 0;
        double bestD = 1e300;
        for (size_t e = 0; e < n; ++e)
        {
            const Vec2 a = fp[e], b = fp[(e + 1) % n];
            const Vec2 ab = b - a;
            const double t = std::clamp(glm::dot(mid - a, ab) / std::max(glm::dot(ab, ab), 1e-9), 0.0, 1.0);
            const double dd = glm::length(a + ab * t - mid);
            if (dd < bestD)
            {
                bestD = dd;
                best = int(e);
            }
        }
        if (bestD < 1.0)
        {
            s2[k] = c.spec.sides[size_t(best)];
            i2[k] = "f" + std::to_string(best);
        }
        else
        {
            // The chamfer edge: named after the edge before its corner.
            s2[k] = 0;
            int prevEdge = 0;
            double pd = 1e300;
            for (size_t e = 0; e < n; ++e)
            {
                const double dd = glm::length(fp[(e + 1) % n] - mid);
                if (dd < pd)
                {
                    pd = dd;
                    prevEdge = int(e);
                }
            }
            i2[k] = "f" + std::to_string(prevEdge) + "x";
        }
    }
    sides = s2;
    ids = i2;
}

std::vector<TierPlan> planTiers(Ctx& c)
{
    std::vector<TierPlan> tiers;
    const Spec& sp = c.spec;
    Ring ring;
    std::vector<int> sides;
    std::vector<std::string> ids;
    baseRing(c, ring, sides, ids);

    auto floorsBetween = [&](int from, int to) {
        std::vector<TierPlan::Floor> out;
        for (int k = from; k < to; ++k)
        {
            const double y0 = k == 0 ? c.floor0 : c.floor0 + c.H0 + (k - 1) * c.H;
            out.push_back({y0, y0 + (k == 0 ? c.H0 : c.H), k});
        }
        return out;
    };
    const double wallTop = c.floor0 + c.H0 + (c.floors - 1) * c.H;
    c.r.wallTop = wallTop;

    if (sp.tiers.size() >= 2)
    {
        // Imported parts: each tier as given, floors laid at the storey height from its bottom.
        std::vector<Spec::Tier> ts = sp.tiers;
        std::sort(ts.begin(), ts.end(), [](const Spec::Tier& a, const Spec::Tier& b) { return a.bottom < b.bottom; });
        for (size_t k = 0; k < ts.size(); ++k)
        {
            TierPlan t;
            t.ring = cleanRing(ts[k].footprint);
            if (t.ring.size() < 3)
                continue;
            const bool base = k == 0;
            if (base && t.ring.size() == ring.size())
            {
                t.ring = ring;
                t.sides = sides;
                t.edgeIds = ids;
            }
            else
            {
                t.sides.assign(t.ring.size(), 2);
                for (size_t e = 0; e < t.ring.size(); ++e)
                    t.edgeIds.push_back("f" + std::to_string(e));
            }
            t.bottom = base ? c.groundLo - 20.0 : ts[k].bottom;
            // A part standing inside a lower volume rises from that volume's top, not from inside it.
            if (!base)
                for (const TierPlan& below : tiers)
                    if (below.top > t.bottom + 1.0 && inside(below.ring, centroid(t.ring)))
                    {
                        bool all = true;
                        for (const Vec2& q : t.ring)
                            all = all && (inside(below.ring, q) || glm::length(q - below.ring[0]) < 1.0);
                        if (all || overlapArea(t.ring, below.ring) > 0.9 * std::abs(signedArea(t.ring)))
                            t.bottom = std::max(t.bottom, below.top);
                    }
            t.top = std::max(ts[k].top, t.bottom + 250.0);

            if (base)
                t.holes = sp.holes;
            double y = base ? c.floor0 : t.bottom;
            int idx = base ? 0 : 100 * int(k);
            for (int f = 0; y < t.top - 150.0 && f < 200; ++f)
            {
                const double h = (base && f == 0) ? c.H0 : c.H;
                const double y1 = std::min(t.top, y + h);
                t.floors.push_back({y, y1, idx++});
                y = y1;
            }
            t.roofed = k + 1 == ts.size();
            tiers.push_back(std::move(t));
        }
        if (!tiers.empty())
        {
            c.r.wallTop = tiers.back().top;
            return tiers;
        }
    }

    const int podium = c.st.podium;
    if (podium > 0 && c.floors > podium + 2 && c.st.towerInset > 0.0 && !sp.holes.empty())
        c.r.warnings.push_back("a courtyard lot takes no tower over a podium; built as one block round the courtyard");
    else if (podium > 0 && c.floors > podium + 2 && c.st.towerInset > 0.0)
    {
        TierPlan base;
        base.ring = ring;
        base.sides = sides;
        base.edgeIds = ids;
        base.holes = sp.holes;
        base.bottom = c.groundLo - 20.0;
        base.floors = floorsBetween(0, podium);
        base.top = base.floors.back().y1;
        base.roofed = false;
        Ring tower = cleanRing(offsetRing(ring, -c.st.towerInset));
        if (tower.size() >= 3 && std::abs(signedArea(tower)) > 0.25 * std::abs(signedArea(ring)) && signedArea(tower) > 0.0)
        {
            tiers.push_back(std::move(base));
            int from = podium;
            int tierIndex = 1;
            while (from < c.floors)
            {
                int to = c.floors;
                if (c.st.stepEvery > 0)
                    to = std::min(c.floors, from + c.st.stepEvery);
                TierPlan t;
                t.ring = tower;
                t.sides.assign(tower.size(), 2);
                for (size_t e = 0; e < tower.size(); ++e)
                    t.edgeIds.push_back("f" + std::to_string(e));
                t.floors = floorsBetween(from, to);
                t.bottom = t.floors.front().y0;
                t.top = t.floors.back().y1;
                t.roofed = to == c.floors;
                tiers.push_back(std::move(t));
                from = to;
                ++tierIndex;
                if (c.st.stepInset > 0.0 && from < c.floors)
                {
                    Ring next = cleanRing(offsetRing(tower, -c.st.stepInset));
                    if (next.size() >= 3 && signedArea(next) > 0.3 * signedArea(tower))
                        tower = next;
                }
            }
            return tiers;
        }
        c.r.warnings.push_back("the tower inset leaves too little floor; built without a podium");
    }
    TierPlan t;
    t.ring = ring;
    t.sides = sides;
    t.edgeIds = ids;
    t.holes = sp.holes;
    t.bottom = c.groundLo - 20.0;
    t.floors = floorsBetween(0, c.floors);
    t.top = wallTop;
    tiers.push_back(std::move(t));
    return tiers;
}

/** Bell towers at the front corners and corner towers: small square tiers rising past the roof, capped. */
void addTowers(Ctx& c, std::vector<TierPlan>& tiers)
{
    if (tiers.empty())
        return;
    const TierPlan& base = tiers.front();
    const Ring& ring = base.ring;
    const size_t n = ring.size();
    const double top = c.r.wallTop;
    auto tower = [&](Vec2 corner, Vec2 inward, Vec2 along, double side, double rise, const std::string& cap) {
        const Vec2 a = corner, b = corner + along * side, d = corner + inward * side, e = b + inward * side;
        Ring sq = cleanRing({a, b, e, d});
        if (sq.size() < 4)
            return;
        TierPlan t;
        // A centimetre proud of the walls it rises from, so it never shares their plane (or the parapet's).
        t.ring = offsetRing(sq, 1.0);
        t.sides.assign(4, 2);
        const int id = int(tiers.size());
        for (int k = 0; k < 4; ++k)
            t.edgeIds.push_back("f" + std::to_string(k));
        t.bottom = top;
        t.top = top + rise;
        t.floors.push_back({top, top + rise, 1000 + id});
        t.roofed = false;
        t.cap = cap;
        tiers.push_back(std::move(t));
    };
    // Imported parts already give a building its towers.
    if (c.st.bellTowers > 0 && c.spec.tiers.size() < 2)
    {
        // The front facade: the street edge facing most along the front.
        int front = -1;
        double best = -2.0;
        for (size_t e = 0; e < n; ++e)
        {
            const double l = glm::length(ring[(e + 1) % n] - ring[e]);
            const double d = glm::dot(outward(ring[e], ring[(e + 1) % n]), c.spec.front);
            if (l > 600.0 && d > best)
            {
                best = d;
                front = int(e);
            }
        }
        if (front >= 0)
        {
            const Vec2 a = ring[size_t(front)], b = ring[(size_t(front) + 1) % n];
            const Vec2 along = glm::normalize(b - a), in = -outward(a, b);
            const double side = std::clamp(glm::length(b - a) * 0.22, 300.0, 700.0);
            const double rise = std::max(600.0, c.H * 2.5);
            // Held back from a party wall, so a neighbour's tower at the same corner never meets this one face to face.
            const double back0 = base.sides[(size_t(front) + n - 1) % n] == 1 ? 3.0 : 0.0, back1 = base.sides[(size_t(front) + 1) % n] == 1 ? 3.0 : 0.0;
            tower(a + along * back0, in, along, side, rise, "dome");
            if (c.st.bellTowers > 1)
                tower(b - along * (side + back1), in, along, side, rise, "dome");
        }
    }
    if (c.spec.corner == "tower")
    {
        // The convex corner between two street sides nearest the front.
        for (size_t i = 0; i < n; ++i)
        {
            const size_t ip = (i + n - 1) % n;
            if (base.sides[ip] != 0 || base.sides[i] != 0)
                continue;
            const Vec2 prev = ring[ip], p = ring[i], next = ring[(i + 1) % n];
            if ((p.x - prev.x) * (next.y - p.y) - (p.y - prev.y) * (next.x - p.x) <= 0.0)
                continue;
            const double side = std::min({350.0, glm::length(p - prev) * 0.4, glm::length(next - p) * 0.4});
            if (side < 150.0)
                continue;
            const Vec2 along = glm::normalize(next - p), in = glm::normalize(prev - p);
            // Square: from the corner along the next edge and back along the previous one.
            Ring sq = cleanRing({p, p + along * side, p + along * side + in * side, p + in * side});
            if (sq.size() < 4)
                continue;
            TierPlan t;
            t.ring = offsetRing(sq, 1.0);
            t.sides.assign(4, 2);
            for (int k = 0; k < 4; ++k)
                t.edgeIds.push_back("f" + std::to_string(k));
            t.bottom = top;
            t.top = top + c.H;
            t.floors.push_back({top, top + c.H, 2000});
            t.roofed = false;
            t.cap = "dome";
            tiers.push_back(std::move(t));
            break;
        }
    }
}

// Facades -------------------------------------------------------------------------------------------------------------------

struct OpeningRect
{
    double s0 = 0, s1 = 0, t0 = 0, t1 = 0;
    bool any() const { return s1 - s0 > 1.0 && t1 - t0 > 1.0; }
};

/**
 * The bay margin at one end of a ring edge: the corner width, widened where the edge meets the next real wall (edges
 * under 1.5 m skipped, they read as part of the corner) at an acute angle, so a module standing out of either wall stays
 * clear of the other's.
 */
double acuteMargin(const Ring& ring, size_t edge, bool atEnd, double cw, double reach)
{
    const size_t n = ring.size();
    const Vec2 a = ring[edge], b = ring[(edge + 1) % n];
    const Vec2 dir = glm::normalize(b - a);
    Vec2 other(0.0);
    for (size_t step = 1; step < n; ++step)
    {
        const size_t e = atEnd ? (edge + step) % n : (edge + n - step) % n;
        const Vec2 p = ring[e], q = ring[(e + 1) % n];
        if (glm::length(q - p) >= 150.0)
        {
            other = glm::normalize(q - p);
            break;
        }
    }
    if (glm::length(other) < 0.5)
        return cw;
    // The turn from the incoming wall to the outgoing one; a convex corner sharper than a right angle turns past 90 degrees.
    const Vec2 in = atEnd ? dir : other, out = atEnd ? other : dir;
    const double turn = std::atan2(in.x * out.y - in.y * out.x, glm::dot(in, out));
    const double interior = kPi - std::abs(turn);
    if (turn <= 0.0 || interior >= kPi * 0.5)
        return cw;
    return cw + reach * (1.0 / std::tan(interior * 0.5) - 1.0);
}

/** cm: an open side with another building closer than this is built as a party wall. */
constexpr double kTouching = 30.0;
/** cm: party walls stand this far inside their own footprint, and trims stop this far short of a party line, so two
 *  neighbours' faces never lie in one plane. */
constexpr double kPartyGap = 1.5;
/** cm: how far past an opening's module its reveals run to the dark back of a building without rooms. */
constexpr double kOpeningBack = 12.0;

/** Each edge's clearance from the spec (a tier edge lying on a footprint edge takes that edge's), and open sides with a
 *  neighbour against them made party walls. */
void measureTier(const Ctx& c, TierPlan& tier)
{
    const Ring& fp = c.spec.footprint;
    const size_t n = tier.ring.size();
    tier.clear.assign(n, 1e9);
    if (c.spec.clearance.size() != fp.size())
        return;
    for (size_t e = 0; e < n; ++e)
    {
        const Vec2 a = tier.ring[e], b = tier.ring[(e + 1) % n];
        for (size_t k = 0; k < fp.size(); ++k)
        {
            const Vec2 p = fp[k], q = fp[(k + 1) % fp.size()];
            const Vec2 d = q - p;
            const double L2 = glm::dot(d, d);
            if (L2 < 1.0)
                continue;
            auto off = [&](Vec2 x) {
                const double t = glm::dot(x - p, d) / L2;
                return t < -0.01 || t > 1.01 ? 1e9 : glm::length(p + d * t - x);
            };
            if (off(a) < 5.0 && off(b) < 5.0)
            {
                tier.clear[e] = c.spec.clearance[k];
                break;
            }
        }
        if (e < tier.sides.size() && tier.sides[e] == 2 && tier.clear[e] < kTouching)
            tier.sides[e] = 1;
    }
}

/** The point at a ring's corner `v` offset `d` out from both edges meeting there (in when d < 0), mitred. */
Vec2 mitrePoint(const Ring& ring, size_t v, double d)
{
    const size_t n = ring.size();
    const Vec2 p = ring[(v + n - 1) % n], b = ring[v], q = ring[(v + 1) % n];
    const Vec2 n0 = outward(p, b), n1 = outward(b, q);
    Vec2 m = n0 + n1;
    const double l = glm::length(m);
    if (l < 1e-9)
        return b + n0 * d;
    m /= l;
    return b + m * (d / std::max(0.25, glm::dot(m, n0)));
}

/** How a trim running along a ring edge ends at a corner: mitred into the next edge's, or capped `gap` short of it. */
struct BandEnd
{
    bool mitre = false;
    double gap = 0.0;
};

/**
 * A box running along ring edge `e` out of its wall, from depth `in` (negative: into the wall) out to `out`, t0..t1 up.
 * Ends that meet the next edge's band are mitred into it, so the two never overlap; other ends are capped.
 */
void ringBand(Soup& s, const Ring& ring, size_t e, BandEnd start, BandEnd end, double t0, double t1, double in, double out, int m, int trim,
              int owner, int part, glm::vec4 c, bool top = true, bool bottom = true, bool back = false)
{
    const size_t n = ring.size();
    const Vec2 a = ring[e], b = ring[(e + 1) % n];
    const double L = glm::length(b - a);
    if (L < 1.0 || t1 - t0 < 0.5)
        return;
    const Vec2 u = (b - a) / L, nn = outward(a, b);
    auto at = [&](bool atEnd, double d) {
        const BandEnd& x = atEnd ? end : start;
        if (x.mitre)
            return mitrePoint(ring, atEnd ? (e + 1) % n : e, d);
        return (atEnd ? b - u * x.gap : a + u * x.gap) + nn * d;
    };
    const Vec2 A0 = at(false, in), B0 = at(true, in), A1 = at(false, out), B1 = at(true, out);
    auto P = [](Vec2 p, double y) { return Vec3(p.x, y, p.y); };
    const double len = glm::length(B1 - A1), depth = out - in;
    {
        const glm::vec2 uv[4] = {uvOf(0, t0, t0, trim), uvOf(0, t1, t0, trim), uvOf(len, t1, t0, trim), uvOf(len, t0, t0, trim)};
        const Vec3 p[4] = {P(A1, t0), P(A1, t1), P(B1, t1), P(B1, t0)};
        s.poly(p, uv, 4, m, trim, owner, part, -1, c);
    }
    const glm::vec2 flat[4] = {uvOf(0, 0, 0, trim), uvOf(L, 0, 0, trim), uvOf(L, depth, 0, trim), uvOf(0, depth, 0, trim)};
    if (top)
    {
        const Vec3 p[4] = {P(A0, t1), P(B0, t1), P(B1, t1), P(A1, t1)};
        s.poly(p, flat, 4, m, trim, owner, part, -1, c);
    }
    if (bottom)
    {
        const Vec3 p[4] = {P(A0, t0), P(A1, t0), P(B1, t0), P(B0, t0)};
        s.poly(p, flat, 4, m, trim, owner, part, -1, c);
    }
    const glm::vec2 cap[4] = {uvOf(0, t0, t0, trim), uvOf(0, t1, t0, trim), uvOf(depth, t1, t0, trim), uvOf(depth, t0, t0, trim)};
    if (!start.mitre)
    {
        const Vec3 p[4] = {P(A0, t0), P(A0, t1), P(A1, t1), P(A1, t0)};
        s.poly(p, cap, 4, m, trim, owner, part, -1, c);
    }
    if (!end.mitre)
    {
        const Vec3 p[4] = {P(B0, t0), P(B1, t0), P(B1, t1), P(B0, t1)};
        s.poly(p, cap, 4, m, trim, owner, part, -1, c);
    }
    if (back)
    {
        const double lb = glm::length(B0 - A0);
        const glm::vec2 uv[4] = {uvOf(lb, t0, t0, trim), uvOf(lb, t1, t0, trim), uvOf(0, t1, t0, trim), uvOf(0, t0, t0, trim)};
        const Vec3 p[4] = {P(B0, t0), P(B0, t1), P(A0, t1), P(A0, t0)};
        s.poly(p, uv, 4, m, trim, owner, part, -1, c);
    }
}

/** Ends of a facade trim on edge e of a ring whose sides are given (empty: every edge open): mitred where the
 *  neighbouring edge carries the same trim, held short of a party wall. */
std::pair<BandEnd, BandEnd> bandEnds(const Ring& ring, const std::vector<int>& sides, size_t e)
{
    const size_t n = ring.size();
    auto carries = [&](size_t k) { return (sides.empty() || k >= sides.size() || sides[k] != 1) && glm::length(ring[(k + 1) % n] - ring[k]) >= 20.0; };
    auto end = [&](size_t k) { return carries(k) ? BandEnd{true, 0.0} : BandEnd{false, (k < sides.size() && sides[k] == 1) ? kPartyGap : 0.0}; };
    return {end((e + n - 1) % n), end((e + 1) % n)};
}

/**
 * Which bay columns of a facade are blank wall, the same on every floor. Street and courtyard facades follow the style's
 * rhythm, else lose a few columns by chance (never a door's, never more than a third); side walls open fewer columns, fewer
 * still near a neighbour, and none when one stands within the style's blankWithin.
 */
std::vector<char> blankColumns(const Ctx& c, const TierPlan& tier, size_t edge, const std::string& fpath, int nb, int side, bool courtyard, bool tower,
                               const std::vector<int>& doorBays)
{
    std::vector<char> blank(size_t(std::max(nb, 0)), 0);
    if (nb <= 0 || tower)
        return blank;
    const Style& st = c.st;
    const bool street = side == 0 && !courtyard;
    const double clear = courtyard || edge >= tier.clear.size() ? 1e9 : tier.clear[edge];
    if (!street && !courtyard && clear < st.blankWithin)
    {
        std::fill(blank.begin(), blank.end(), char(1));
        return blank;
    }
    const auto isDoor = [&](int k) { return std::find(doorBays.begin(), doorBays.end(), k) != doorBays.end(); };
    // Curtain walls glaze every bay they face the street or open ground with.
    const double blankChance = st.glassFacade ? 0.0 : st.blankBays;
    const double sideChance = (st.glassFacade ? 1.0 : st.sideOpenings) * (clear < st.sparseWithin ? 0.5 : 1.0);
    for (int k = 0; k < nb; ++k)
    {
        // Mirrored about the middle, so a facade's piers pair up.
        const int sym = std::min(k, nb - 1 - k);
        if (!st.rhythm.empty() && (street || courtyard))
            blank[size_t(k)] = st.rhythm[size_t(sym) % st.rhythm.size()] == 'b';
        else if (street || courtyard)
            blank[size_t(k)] = nb >= 3 && c.rnd(fpath + "/col" + std::to_string(sym)) < blankChance;
        else
            blank[size_t(k)] = c.rnd(fpath + "/col" + std::to_string(sym)) >= sideChance;
        if (isDoor(k))
            blank[size_t(k)] = 0;
    }
    if (street || courtyard)
    {
        // At most a third of the columns blank, giving back the middle ones first.
        int n = int(std::count(blank.begin(), blank.end(), char(1)));
        for (int d = 0; d <= nb / 2 && n > nb / 3; ++d)
            for (int k : {nb / 2 - d, (nb - 1) / 2 + d})
                if (k >= 0 && k < nb && blank[size_t(k)] && n > nb / 3)
                {
                    blank[size_t(k)] = 0;
                    --n;
                }
    }
    else if (std::all_of(blank.begin(), blank.end(), [](char b) { return b != 0; }) && st.sideOpenings > 0.0 && clear >= st.sparseWithin)
        blank[size_t(nb / 2)] = 0;
    return blank;
}

void emitFacade(Ctx& c, const TierPlan& tier, int tierIndex, size_t edge, bool courtyard, int holeIndex, const Ring& ring, int side,
                const std::string& edgeId)
{
    const Style& st = c.st;
    Result& r = c.r;
    const size_t n = ring.size();
    const Vec2 a = ring[edge], b = ring[(edge + 1) % n];
    const double L = glm::length(b - a);
    if (L < 20.0)
        return;
    const std::string fpath = "t" + std::to_string(tierIndex) + "/" + (courtyard ? "h" + std::to_string(holeIndex) : std::string()) + edgeId;
    const Frame f = frameOf(a, b, double(fnv(fpath) % 8) * 100.0);
    Facade fac;
    fac.path = fpath;
    fac.tier = tierIndex;
    fac.edge = int(edge);
    fac.courtyard = courtyard;
    fac.side = side;
    fac.a = a;
    fac.b = b;
    fac.normal = outward(a, b);
    fac.bottom = tier.bottom;
    fac.top = tier.top;
    Soup& sh = r.shell;
    const bool party = side == 1;
    const bool street = side == 0 && !courtyard;
    const bool baseTier = tier.floors.empty() ? false : tier.floors.front().index == 0;
    const bool tower = !tier.cap.empty();

    // Party walls: blank, one shell module per facade.
    if (party)
    {
        const int owner = c.addModule(wallModule(f, L * 0.5, tier.bottom, 0.0, "wall.party", "wall", fpath + "/wall", {L, tier.top - tier.bottom, st.wallThickness}));
        r.modules[size_t(owner)].shell = true;
        wallRect(sh, f, 0.0, L, tier.bottom, tier.top, kPartyGap, tier.bottom, MatRender, TrimRender, owner, PartPartyWall, -1, c.cRender);
        r.facades.push_back(fac);
        return;
    }
    static const std::vector<int> kOpenRing;
    const std::vector<int>& ringSides = courtyard ? kOpenRing : tier.sides;
    const auto [bandStart, bandEnd] = bandEnds(ring, ringSides, edge);

    // Bays, kept back from the facade's ends by the corner width, and further at an acute corner.
    const double cw = std::min(st.cornerWidth, L * 0.15);
    // The deepest a module stands out of or into its wall: balconies and awnings, or windows at the back of an arcade.
    const double reach = 150.0 + (st.arcade > 0.0 ? std::min(st.arcadeDepth, 450.0) : 0.0);
    const double m0 = courtyard ? cw : acuteMargin(ring, edge, false, cw, reach), m1 = courtyard ? cw : acuteMargin(ring, edge, true, cw, reach);
    const double usable = std::max(0.0, L - m0 - m1);
    const double bayW = c.ed.bayWidth ? std::max(80.0, *c.ed.bayWidth) : st.bayWidth;
    int nb = 0;
    if (auto it = c.ed.facadeBays.find(fpath); it != c.ed.facadeBays.end())
        nb = std::clamp(it->second, 0, 200);
    else if (usable >= st.minBay * 0.8)
    {
        nb = std::max(1, int(std::lround(usable / bayW)));
        while (nb > 1 && usable / nb < st.minBay)
            --nb;
    }
    if (tower && usable > 50.0)
        nb = std::max(1, std::min(nb, 2));
    if (usable < 1.0)
        nb = 0;
    fac.bays = nb;
    const double bw = nb > 0 ? usable / nb : usable;

    // Arcade (portales): street facades of the base tier, decided per facade.
    const bool arcade = street && baseTier && c.floors >= 2 && nb >= 2 &&
                        (st.arcade >= 1.0 || (st.arcade > 0.0 && c.rnd(fpath + "/arcade") < st.arcade));
    const double arcadeD = arcade ? std::min(st.arcadeDepth, 450.0) : 0.0;

    // Entrances: the bays the envelope's entrances fall on (main first), else the middle bay of the front street facade.
    std::vector<int> doorBays;
    if (street && baseTier && nb > 0)
        for (const Vec2& e : c.spec.entrances)
        {
            const Vec2 ab = b - a;
            const double t = glm::dot(e - a, ab) / std::max(glm::dot(ab, ab), 1e-9);
            const Vec2 q = a + ab * std::clamp(t, 0.0, 1.0);
            if (glm::length(q - e) > 150.0 || t < -0.02 || t > 1.02)
                continue;
            const int k = std::clamp(int((t * L - m0) / bw), 0, nb - 1);
            if (std::find(doorBays.begin(), doorBays.end(), k) == doorBays.end())
                doorBays.push_back(k);
        }

    const std::vector<char> blank = blankColumns(c, tier, edge, fpath, nb, side, courtyard, tower, doorBays);

    const int plinthOwner = [&] {
        if (!baseTier || tower)
            return -1;
        Module m = wallModule(f, L * 0.5, tier.bottom, 0.0, "plinth", "plinth", fpath + "/plinth", {L, c.floor0 - tier.bottom, 6.0});
        m.shell = true;
        return c.addModule(std::move(m));
    }();

    // Upright trims end inside the cornice's top course, and quoin blocks skip the string courses, so their tops never
    // meet another face in a plane.
    const bool hasCornice = st.cornice != "none" && !courtyard && tier.roofed && !tower;
    const double stripTop = hasCornice && st.cornice != "vigas" ? tier.top - 2.0 : tier.top;
    // A tower's corner strips start just inside the cornice it stands on.
    const double stripBottom = tower ? tier.bottom - 1.0 : tier.bottom;
    auto onBand = [&](double y0, double y1) {
        if (!st.bands || tower)
            return false;
        for (const TierPlan::Floor& fl : tier.floors)
            if (fl.index > 0 && fl.y0 > tier.bottom + 1.0 && y0 < fl.y0 + 13.0 && y1 > fl.y0 - 13.0)
                return true;
        return false;
    };

    // Corner strips: quoins or pilasters at the facade's start corner when it is convex.
    if (st.corner != "none" && !courtyard)
    {
        const Vec2 prev = ring[(edge + n - 1) % n];
        const bool convex = (a.x - prev.x) * (b.y - a.y) - (a.y - prev.y) * (b.x - a.x) > 0.0;
        if (convex)
        {
            Module m = wallModule(f, cw * 0.5, tier.bottom, 0.0, "corner", "corner", fpath + "/corner", {cw, tier.top - tier.bottom, 6.0},
                                  st.corner == "quoin" ? 1 : 0);
            m.shell = true;
            const int owner = c.addModule(std::move(m));
            if (st.corner == "quoin")
            {
                // Alternating blocks up the corner.
                for (double y = stripBottom; y < stripTop - 1.0; y += 60.0)
                {
                    const bool wide = int((y - stripBottom) / 60.0) % 2 == 0;
                    const double y1 = std::min(stripTop, y + 58.0);
                    if (!onBand(y + 2.0, y1) && y1 - y > 10.0)
                        facadeBox(sh, f, 0.0, wide ? cw * 1.4 : cw, y + 2.0, y1, 5.0, MatTrim, TrimPilaster, owner, PartPilaster, c.cTrim);
                }
            }
            else
                facadeBox(sh, f, 0.0, cw, stripBottom, stripTop, 7.0, MatTrim, TrimPilaster, owner, PartPilaster, c.cTrim);
        }
    }

    // Pilasters at bay boundaries, full tier height (over the portales, which have their own columns).
    double pilasterFrom = std::max(tier.bottom, c.floor0);
    if (arcade && !tier.floors.empty())
        pilasterFrom = tier.floors.front().y1;
    if (st.pilasters && nb > 1 && !tower)
        for (int k = 1; k < nb; ++k)
        {
            const double s = m0 + k * bw;
            Module m = wallModule(f, s, tier.bottom, 0.0, "pilaster", "pilaster", fpath + "/p" + std::to_string(k), {36.0, tier.top - tier.bottom, 8.0});
            m.shell = true;
            const int owner = c.addModule(std::move(m));
            facadeBox(sh, f, s - 18.0, s + 18.0, pilasterFrom, stripTop, 8.0, MatTrim, TrimPilaster, owner, PartPilaster, c.cTrim);
        }

    // Corner margins of the wall (between the facade ends and the first and last bay), per floor.
    const int marginOwner = [&] {
        Module m = wallModule(f, L * 0.5, tier.bottom, 0.0, "wall.bay", "wall", fpath + "/margin", {L, tier.top - tier.bottom, st.wallThickness});
        m.shell = true;
        return c.addModule(std::move(m));
    }();
    const double wallBottom = baseTier ? c.floor0 : tier.bottom;
    if (nb == 0)
        wallRect(sh, f, 0.0, L, wallBottom, tier.top, 0.0, wallBottom, MatRender, TrimRender, marginOwner, PartWall, -1, c.cRender);
    else
    {
        wallRect(sh, f, 0.0, m0, wallBottom, tier.top, 0.0, wallBottom, MatRender, TrimRender, marginOwner, PartWall, -1, c.cRender);
        wallRect(sh, f, L - m1, L, wallBottom, tier.top, 0.0, wallBottom, MatRender, TrimRender, marginOwner, PartWall, -1, c.cRender);
    }
    // Plinth band below the ground floor (base tier), stepping with the ground on slopes.
    if (plinthOwner >= 0)
    {
        if (nb == 0)
            wallRect(sh, f, 0.0, L, tier.bottom, c.floor0, 0.0, tier.bottom, MatBase, TrimPlinth, plinthOwner, PartPlinth, -1, c.cBase);
        else
        {
            wallRect(sh, f, 0.0, m0, tier.bottom, c.floor0, 0.0, tier.bottom, MatBase, TrimPlinth, plinthOwner, PartPlinth, -1, c.cBase);
            wallRect(sh, f, L - m1, L, tier.bottom, c.floor0, 0.0, tier.bottom, MatBase, TrimPlinth, plinthOwner, PartPlinth, -1, c.cBase);
        }
    }

    // Business: shopfronts on the street facades' ground floor; one sign per facade spanning them.
    const bool shops = c.business && st.shopfronts && street && baseTier;
    int signFirst = -1, signLast = -1;
    double signTop = 0.0, signBottom = 0.0;

    for (const TierPlan::Floor& fl : tier.floors)
    {
        const bool ground = fl.index == 0;
        const double H = fl.y1 - fl.y0;
        for (int k = 0; k < nb; ++k)
        {
            const double s0 = m0 + k * bw, s1 = s0 + bw, sc = 0.5 * (s0 + s1);
            const std::string bpath = P(fpath, fl.index, k);
            // Ground floor level of this bay: stepped with the ground on slopes.
            double y0 = fl.y0;
            if (ground && c.slope)
            {
                const Vec3 mid = f.at(sc, 0.0);
                y0 = c.spec.steppedLevel({mid.x, mid.z}, st.plinth, fl.y0);
            }
            const double y1 = fl.y1;
            const double bh = y1 - y0;

            // What the bay is.
            std::string kind = "window";
            if (ground && street)
            {
                if (arcade)
                    kind = "arcade";
                else if (std::find(doorBays.begin(), doorBays.end(), k) != doorBays.end())
                    kind = shops ? "shopdoor" : "door";
                else if (shops)
                    kind = "shopfront";
                else
                    kind = st.groundStreet;
            }
            else if (ground && courtyard && k == nb / 2)
                kind = "door";
            if (tower)
                kind = "window";
            if (blank[size_t(k)] && kind == "window")
                kind = "wall";
            if (auto it = c.ed.bays.find(bpath); it != c.ed.bays.end())
                kind = it->second;
            if (kind == "arcade" && !(ground && street))
                kind = "window";
            const bool doorInArcade =
                arcade && (std::find(doorBays.begin(), doorBays.end(), k) != doorBays.end() || (doorBays.empty() && k == nb / 2));

            auto toggle = [&](const char* what, bool def) {
                if (auto it = c.ed.toggles.find(bpath); it != c.ed.toggles.end())
                    if (auto jt = it->second.find(what); jt != it->second.end())
                        return jt->second;
                return def;
            };

            const double d0 = kind == "arcade" ? arcadeD : 0.0; // the wall plane of this bay
            const int bayIndex = int(r.bays.size());
            Bay bay;
            bay.path = bpath;
            bay.kind = kind;
            bay.tier = tierIndex;
            bay.floor = fl.index;
            bay.bay = k;
            bay.normal = f.n;
            bay.corners[0] = f.at(s0, y0, 0.0);
            bay.corners[1] = f.at(s1, y0, 0.0);
            bay.corners[2] = f.at(s1, y1, 0.0);
            bay.corners[3] = f.at(s0, y1, 0.0);

            const int wallOwner = [&] {
                Module m = wallModule(f, sc, y0, d0, "wall.bay", "wall", bpath + "/wall", {bw, bh, st.wallThickness});
                m.shell = true;
                return c.addModule(std::move(m));
            }();

            // Balconies on upper street (and resort open) facades: stacked per bay, or per bay and floor.
            const bool balconyFacade = (street || (st.balconyKind == "stack" && !courtyard)) && !ground && !tower;
            bool balcony = false;
            if (balconyFacade && kind == "window")
            {
                const double roll = st.balconyKind == "stack" ? c.rnd(fpath + "/b" + std::to_string(k) + "/balcony")
                                                               : c.rnd(bpath + "/balcony");
                balcony = roll < st.balconies;
            }
            balcony = toggle("balcony", balcony) && !ground && kind != "wall";
            bay.balcony = balcony;

            // The opening.
            OpeningRect o;
            std::string openType, openKind = "window";
            int openVariant = 0;
            if (kind == "window")
            {
                if (balcony)
                {
                    const double w = std::clamp(bw * 0.5, 90.0, 180.0);
                    o = {sc - w * 0.5, sc + w * 0.5, y0, y0 + std::min(bh - 30.0, 240.0)};
                    openType = "door.balcony";
                    openKind = "door";
                    openVariant = 1;
                }
                else if (st.glassFacade)
                {
                    o = {s0 + 10.0, s1 - 10.0, y0 + 25.0, y1 - 30.0};
                    openType = "window.glazing";
                    openVariant = 2;
                }
                else
                {
                    const double w = std::clamp(bw * st.windowW, 50.0, bw - 30.0);
                    const double h = std::clamp(H * st.windowH, 60.0, bh - 40.0);
                    const double sill = y0 + std::clamp(H * st.windowSill, 20.0, bh - h - 15.0);
                    o = {sc - w * 0.5, sc + w * 0.5, sill, sill + h};
                    openType = tower ? "window.bell" : st.windowKind;
                    openVariant = tower ? 3 : 0;
                }
            }
            else if (kind == "door" || kind == "shopdoor" || (kind == "arcade" && doorInArcade))
            {
                const double w = std::clamp(bw * st.doorW, 90.0, std::min(bw - 30.0, 200.0));
                const double h = std::clamp(H * st.doorH, 200.0, bh - 25.0);
                o = {sc - w * 0.5, sc + w * 0.5, y0, y0 + h};
                openType = doorTypeFor(c, kind == "shopdoor" || shops);
                openKind = "door";
                if (kind == "shopdoor")
                {
                    const double sw = std::clamp(bw * st.shopW, 120.0, bw - 30.0);
                    // Room for the sign over the shopfront.
                    const double shh = std::max(180.0, std::min(std::clamp(H * st.shopH, 200.0, bh - 25.0), bh - 85.0));
                    o = {sc - sw * 0.5, sc + sw * 0.5, y0, y0 + shh};
                }
            }
            else if (kind == "shopfront" || (kind == "arcade" && shops))
            {
                const double w = std::clamp(bw * st.shopW, 120.0, bw - 30.0);
                const double h = std::max(180.0, std::min(std::clamp(H * st.shopH, 200.0, bh - 25.0), bh - 85.0));
                o = {sc - w * 0.5, sc + w * 0.5, y0, y0 + h};
                openType = "shopfront";
                openKind = "shopfront";
            }
            else if (kind == "garage")
            {
                const double w = std::clamp(bw * 0.85, 200.0, bw - 30.0);
                const double h = std::clamp(H * 0.85, 220.0, std::min(bh - 25.0, 600.0));
                o = {sc - w * 0.5, sc + w * 0.5, y0, y0 + h};
                openType = "door.garage";
                openKind = "rollup";
            }
            else if (kind == "arcade")
            {
                const double w = std::clamp(bw * st.windowW, 50.0, bw - 30.0);
                const double h = std::clamp(H * st.windowH, 60.0, bh - 40.0);
                const double sill = y0 + std::clamp(H * st.windowSill, 20.0, bh - h - 15.0);
                o = {sc - w * 0.5, sc + w * 0.5, sill, sill + h};
                openType = st.windowKind;
            }
            // kind "wall": no opening.

            // Shell: the wall around the opening, the reveals, and the plinth under it.
            if (o.any())
            {
                wallRect(sh, f, s0, o.s0, y0, y1, d0, fl.y0, MatRender, TrimRender, wallOwner, PartWall, bayIndex, c.cRender);
                wallRect(sh, f, o.s1, s1, y0, y1, d0, fl.y0, MatRender, TrimRender, wallOwner, PartWall, bayIndex, c.cRender);
                wallRect(sh, f, o.s0, o.s1, y0, o.t0, d0, fl.y0, MatRender, TrimRender, wallOwner, PartWall, bayIndex, c.cRender);
                wallRect(sh, f, o.s0, o.s1, o.t1, y1, d0, fl.y0, MatRender, TrimRender, wallOwner, PartWall, bayIndex, c.cRender);
                // Without rooms behind, the reveals run on past the module to a dark back, so an opening never shows the hollow shell.
                const double depth = r.enterable ? st.reveal : st.reveal + kOpeningBack;
                reveals(sh, f, o.s0, o.s1, o.t0, o.t1, d0, depth, MatRender, wallOwner, bayIndex, c.cRender, o.t0 > y0 + 1.0);
                if (!r.enterable)
                    wallRect(sh, f, o.s0, o.s1, o.t0, o.t1, d0 + depth, o.t0, MatDark, TrimNone, wallOwner, PartReveal, bayIndex, glm::vec4(0.03f, 0.03f, 0.035f, 0.0f));
                bay.openS = o.s0 - s0;
                bay.openT = o.t0 - y0;
                bay.openW = o.s1 - o.s0;
                bay.openH = o.t1 - o.t0;
            }
            else
                wallRect(sh, f, s0, s1, y0, y1, d0, fl.y0, MatRender, TrimRender, wallOwner, PartWall, bayIndex, c.cRender);
            if (ground && plinthOwner >= 0)
                wallRect(sh, f, s0, s1, tier.bottom, y0, 0.0, tier.bottom, MatBase, TrimPlinth, plinthOwner, PartPlinth, bayIndex, c.cBase);

            // The arcade: column and arch at the facade line, the soffit over the walk, side returns at the ends.
            if (kind == "arcade")
            {
                // Under the lowest a soffit sits, so the beam's top and a soffit never share a plane.
                Module m = wallModule(f, s0, y0, -2.0, "arcade.bay", "arcade", bpath + "/arcade", {bw, y1 - y0 - 2.0, arcadeD}, 0);
                m.data = {{"span", bw}};
                c.addModule(std::move(m));
                const glm::vec2 uv[4] = {uvOf(0, 0, 0, -1), uvOf(0, arcadeD, 0, -1), uvOf(bw, arcadeD, 0, -1), uvOf(bw, 0, 0, -1)};
                // The soffit faces down: along first, then into the wall. Neighbouring facades' soffits overlap at a corner
                // both arcades turn, so each edge's sits half a centimetre from the next one's.
                const double soffitY = y1 - 0.5 * double(edge + 1 == n && n % 2 == 1 ? 2 : edge % 2);
                quad(sh, f.at(s0, soffitY, 0.0), f.u * bw, -f.n * arcadeD, uv, MatRender, TrimNone, wallOwner, PartSoffit, bayIndex, c.cTrim);
                if (k == nb - 1)
                    m = wallModule(f, s1, y0, -2.0, "arcade.bay", "column", bpath + "/arcadeEnd", {40.0, y1 - y0 - 7.0, 40.0});
                if (k == nb - 1)
                    c.addModule(std::move(m));
                if (k == 0)
                {
                    const glm::vec2 q[4] = {uvOf(0, y0, y0, TrimRender), uvOf(0, y1, y0, TrimRender), uvOf(arcadeD, y1, y0, TrimRender),
                                            uvOf(arcadeD, y0, y0, TrimRender)};
                    quad(sh, f.at(s0, y0, 0.0), kUp * (y1 - y0), -f.n * arcadeD, q, MatRender, TrimRender, wallOwner, PartWall, bayIndex, c.cRender);
                }
                if (k == nb - 1)
                {
                    const glm::vec2 q[4] = {uvOf(0, y0, y0, TrimRender), uvOf(0, y1, y0, TrimRender), uvOf(arcadeD, y1, y0, TrimRender),
                                            uvOf(arcadeD, y0, y0, TrimRender)};
                    quad(sh, f.at(s1, y0, arcadeD), kUp * (y1 - y0), f.n * arcadeD, q, MatRender, TrimRender, wallOwner, PartWall, bayIndex, c.cRender);
                }
            }

            // Modules in the opening.
            const double inset = d0 + st.reveal;
            // A stoop when a ground-floor door's sill stands well above the ground in front of it.
            auto stoop = [&](double oc, double ow) {
                if (!ground)
                    return;
                const Vec3 front = f.at(oc, 0.0, -60.0);
                const double local = c.spec.groundAt({front.x, front.z});
                if (o.t0 - local <= 35.0)
                    return;
                Module s = wallModule(f, oc, local, -1.0, "steps", "steps", bpath + "/steps", {ow + 40.0, o.t0 - local, std::min(140.0, (o.t0 - local) * 1.6)});
                s.data = {{"rise", o.t0 - local}};
                c.addModule(std::move(s));
            };
            if (o.any())
            {
                const double ow = o.s1 - o.s0, oh = o.t1 - o.t0, oc = 0.5 * (o.s0 + o.s1);
                // Modules sit a centimetre inside their reveals, never in the reveals' planes.
                const double mw = ow - 2.0, mb = o.t0 + (o.t0 > y0 + 1.0 ? 1.0 : 0.0), mh = o.t1 - 1.0 - mb;
                Opening op;
                op.bay = bpath;
                op.normal = f.n;
                op.width = ow;
                op.height = oh;
                op.origin = f.at(oc, o.t0, d0);
                if (openKind == "shopfront" || kind == "shopdoor")
                {
                    Module m = wallModule(f, oc, mb, inset, "shopfront", "shopfront", bpath + "/shopfront", {mw, mh, 15.0}, c.glazing);
                    if (!r.enterable)
                        m.data = {{"interior", "card"}, {"room", fakeRoomKind(c.program, 0)}};
                    c.addModule(std::move(m));
                    if (kind == "shopdoor" || (kind == "arcade" && doorInArcade))
                    {
                        const double dw = std::min(120.0, ow * 0.4);
                        // In front of the shopfront's frame and clear of its end bar, so no face of one lies in a face of the other.
                        Module d = wallModule(f, o.s1 - dw * 0.5 - 20.0, o.t0, inset - 9.5, "door.shop", "door", bpath + "/door",
                                              {dw, std::min(oh - 10.0, 250.0), 8.0}, 2 + c.glazing);
                        if (r.enterable && !courtyard)
                            d.data = {{"entry", true}, {"interior", c.spec.id}};
                        c.addModule(std::move(d));
                        stoop(oc, ow);
                    }
                    op.kind = kind == "shopdoor" ? "shopdoor" : "shopfront";
                    op.slot = "shopfront";
                    if (signFirst < 0)
                        signFirst = k;
                    signLast = k;
                    signBottom = std::max(signBottom, o.t1 + 8.0);
                    signTop = y1 - 15.0;
                    // Awnings and roll-up shutters over shopfronts; the sign goes above the awnings.
                    if (toggle("awning", c.rnd(bpath + "/awning") < st.awnings) && !arcade && y1 - o.t1 >= 125.0)
                    {
                        c.addModule(wallModule(f, oc, o.t1 + 5.0, -2.0, "awning", "awning", bpath + "/awning", {ow + 30.0, 50.0, 110.0}));
                        signBottom = std::max(signBottom, o.t1 + 60.0);
                    }
                    if (toggle("shutter", c.rnd(bpath + "/shutter") < st.shutters))
                    {
                        c.addModule(wallModule(f, oc, o.t0, d0 - 3.0, "shutter.rollup", "shutter", bpath + "/shutter", {ow + 10.0, oh + 25.0, 22.0},
                                               int(c.rnd(bpath + "/shutterDown") * 3.0)));
                        signBottom = std::max(signBottom, o.t1 + 30.0);
                    }
                }
                else if (openKind == "door" || openKind == "rollup")
                {
                    Module m = wallModule(f, oc, mb, inset - 4.0, openType, openKind == "rollup" ? "rollup" : "door", bpath + "/door", {mw, mh, 8.0},
                                          openVariant + (openKind == "rollup" ? 0 : c.glazing));
                    if (r.enterable && ground && !courtyard)
                        m.data = {{"entry", true}, {"interior", c.spec.id}};
                    c.addModule(std::move(m));
                    op.kind = openKind == "rollup" ? "garage" : "door";
                    op.slot = openType;
                    stoop(oc, ow);
                }
                else
                {
                    Module m = wallModule(f, oc, mb, inset, openType, "window", bpath + "/window", {mw, mh, 12.0}, openVariant + c.glazing);
                    // Windows of buildings without rooms show a fake one (Weathering's interior card in Unreal).
                    if (!r.enterable && !tower)
                        m.data = {{"interior", "card"}, {"room", fakeRoomKind(c.program, fl.index)}};
                    c.addModule(std::move(m));
                    op.kind = "window";
                    op.slot = openType;
                    if (toggle("grille", ground ? c.rnd(bpath + "/grille") < st.grilles : c.rnd(bpath + "/grille") < st.grilles * 0.35))
                        c.addModule(wallModule(f, oc, o.t0 - 5.0, d0 - 4.0, "grille.window", "grille", bpath + "/grille", {ow + 12.0, oh + 10.0, 6.0},
                                               ground ? 0 : 1));
                    if (toggle("ac", !ground && c.rnd(bpath + "/ac") < st.acUnits))
                    {
                        // Under the sill, else beside the window; never above it, where the next floor's balcony may be.
                        std::optional<Vec2> at;
                        if (o.t0 - 70.0 > fl.y0 + 10.0)
                            at = Vec2(oc + std::min(ow * 0.25, 30.0), o.t0 - 70.0);
                        else if (s1 - o.s1 > 105.0)
                            at = Vec2(o.s1 + 55.0, o.t0 + std::max(0.0, (oh - 55.0) * 0.5));
                        // Clear of the pilasters at the bay's edges.
                        if (at && st.pilasters && nb > 1)
                        {
                            const double lo = s0 + (k > 0 ? 22.0 : 0.0) + 40.0, hi = s1 - (k < nb - 1 ? 22.0 : 0.0) - 40.0;
                            if (lo > hi)
                                at.reset();
                            else
                                at->x = std::clamp(at->x, lo, hi);
                        }
                        if (at)
                            c.addModule(wallModule(f, at->x, at->y, -1.0, "ac.unit", "ac", bpath + "/ac", {80.0, 55.0, 30.0}));
                    }
                    if (toggle("shutter", false))
                        c.addModule(wallModule(f, oc, o.t0, d0 - 3.0, "shutter.rollup", "shutter", bpath + "/shutter", {ow + 10.0, oh + 25.0, 22.0}));
                }
                if (balcony)
                {
                    const std::string bk = st.balconyKind == "stack" ? "concrete" : st.balconyKind;
                    const int v = bk == "iron" ? 0 : bk == "concrete" ? 1 : 2;
                    Module m = wallModule(f, oc, y0, -1.0, "balcony." + bk, "balcony", bpath + "/balcony",
                                          {std::min(bw - 10.0, ow + 80.0), 105.0, bk == "iron" ? 70.0 : 120.0}, v);
                    c.addModule(std::move(m));
                }
                r.openings.push_back(std::move(op));
            }
            r.bays.push_back(std::move(bay));
        }

        // String course between floors.
        if (st.bands && fl.index > 0 && !tower && fl.y0 > tier.bottom + 1.0)
        {
            Module m = wallModule(f, L * 0.5, fl.y0 - 12.0, 0.0, "band", "band", fpath + "/l" + std::to_string(fl.index) + "/band", {L, 24.0, 6.0});
            m.shell = true;
            const int owner = c.addModule(std::move(m));
            ringBand(sh, ring, edge, bandStart, bandEnd, fl.y0 - 12.0, fl.y0 + 12.0, 0.0, 6.0, MatTrim, TrimBand, owner, PartBand, c.cTrim);
        }
    }

    // Sign over the shopfronts.
    if (signFirst >= 0 && signTop - signBottom > 25.0)
    {
        const double s0 = m0 + signFirst * bw + 15.0, s1 = m0 + (signLast + 1) * bw - 15.0;
        const double h = std::min(signTop - signBottom, 110.0);
        // Under portales the sign goes on the shop wall behind the columns.
        Module m = wallModule(f, 0.5 * (s0 + s1), signBottom, arcade ? arcadeD - 1.0 : -1.0, st.signKind, "sign", fpath + "/l0/sign", {s1 - s0, h, 8.0});
        // The business named on the sign: the one on this facade, else the first; never a real name (W1 names are fictional).
        std::string text = c.spec.name;
        std::string program = c.spec.program;
        const Vec2 mid = 0.5 * (a + b);
        double best = 1e300;
        for (const Business& bz : c.spec.businesses)
        {
            const double d = bz.hasAt ? glm::length(bz.at - mid) : 1e200;
            if (d < best || text.empty())
            {
                best = d;
                text = bz.name;
                program = bz.program;
            }
        }
        m.data = {{"text", text}, {"program", program}, {"face", "front"}};
        c.addModule(std::move(m));
    }

    // Cornice at the wall top (not under a pitched roof's eaves).
    if (hasCornice)
    {
        const bool heavy = st.cornice == "heavy";
        const double ch = heavy ? 45.0 : 22.0, out = heavy ? 30.0 : 12.0;
        Module m = wallModule(f, L * 0.5, tier.top - ch, 0.0, "cornice", "cornice", fpath + "/cornice", {L, ch, out});
        m.shell = true;
        const int owner = c.addModule(std::move(m));
        if (st.cornice == "vigas")
        {
            for (double s = 40.0; s < L - 20.0; s += 70.0)
                facadeBox(sh, f, s - 9.0, s + 9.0, tier.top - 40.0, tier.top - 22.0, 45.0, MatWood, TrimNone, owner, PartCornice, c.cTrim);
        }
        else
        {
            ringBand(sh, ring, edge, bandStart, bandEnd, tier.top - ch, tier.top - ch * 0.45, 0.0, out * 0.5, MatTrim, TrimCornice, owner, PartCornice, c.cTrim,
                     false);
            ringBand(sh, ring, edge, bandStart, bandEnd, tier.top - ch * 0.45, tier.top, 0.0, out, MatTrim, TrimCornice, owner, PartCornice, c.cTrim);
        }
    }

    // A drainpipe at one end of street facades.
    if (st.drainpipes && street && baseTier && L > 400.0 && !arcade)
        c.addModule(wallModule(f, L - std::max(cw * 0.5, 8.0), c.groundLo, -7.5, "drainpipe", "drainpipe", fpath + "/drainpipe", {12.0, stripTop - 3.0 - c.groundLo, 12.0}));

    r.facades.push_back(fac);
}

// Roofs ---------------------------------------------------------------------------------------------------------------------

void flatCap(Ctx& c, const Ring& outer, const std::vector<Ring>& holes, double y, int mat, int part, int owner, glm::vec4 col)
{
    std::vector<Vec2> pts;
    std::vector<uint32_t> tris;
    triangulateWithHoles(outer, holes, pts, tris);
    for (size_t t = 0; t + 2 < tris.size(); t += 3)
    {
        // Counter-clockwise in (x, z) faces down in Y-up space, so flip to face up.
        const Vec2 q[3] = {pts[tris[t]], pts[tris[t + 2]], pts[tris[t + 1]]};
        const Vec3 p[3] = {{q[0].x, y, q[0].y}, {q[1].x, y, q[1].y}, {q[2].x, y, q[2].y}};
        const glm::vec2 uv[3] = {{float(q[0].x / 200.0), float(q[0].y / 200.0)}, {float(q[1].x / 200.0), float(q[1].y / 200.0)},
                                 {float(q[2].x / 200.0), float(q[2].y / 200.0)}};
        c.r.shell.poly(p, uv, 3, mat, TrimNone, owner, part, -1, col);
    }
}

/** A parapet round a roof, mitred at its corners: none along party walls, where it stops short with an end cap. */
void parapet(Ctx& c, const Ring& ring, const std::vector<int>& sides, double y, double h, int owner)
{
    const double t = 25.0;
    for (size_t e = 0; e < ring.size(); ++e)
    {
        if (e < sides.size() && sides[e] == 1)
            continue;
        const auto [start, end] = bandEnds(ring, sides, e);
        // The wall carried up, then a coping a little wider than it on top.
        ringBand(c.r.shell, ring, e, start, end, y, y + h - 5.0, -t, 0.0, MatRender, TrimRender, owner, PartParapet, c.cRender, false, false, true);
        ringBand(c.r.shell, ring, e, start, end, y + h - 5.0, y + h, -t - 2.0, 3.0, MatTrim, TrimCoping, owner, PartParapet, c.cTrim, true, true, true);
    }
}

void dome(Ctx& c, Vec2 centre, double radius, double y, int owner, double drum)
{
    Soup& s = c.r.shell;
    const int seg = 12, rings = 5;
    // Drum.
    for (int k = 0; k < seg; ++k)
    {
        const double a0 = 2.0 * kPi * k / seg, a1 = 2.0 * kPi * (k + 1) / seg;
        const Vec3 p0{centre.x + radius * std::cos(a0), y, centre.y + radius * std::sin(a0)};
        const Vec3 p1{centre.x + radius * std::cos(a1), y, centre.y + radius * std::sin(a1)};
        const Vec3 p[4] = {p0, p0 + kUp * drum, p1 + kUp * drum, p1};
        const glm::vec2 uv[4] = {uvOf(k * 50.0, y, y, TrimRender), uvOf(k * 50.0, y + drum, y, TrimRender), uvOf((k + 1) * 50.0, y + drum, y, TrimRender),
                                 uvOf((k + 1) * 50.0, y, y, TrimRender)};
        // Winding: p0 -> up -> p1 runs clockwise seen from outside for increasing angle in (x, z), which faces out.
        const Vec3 q[4] = {p[3], p[2], p[1], p[0]};
        const glm::vec2 qu[4] = {uv[3], uv[2], uv[1], uv[0]};
        const Vec3 nrm = glm::cross(q[1] - q[0], q[2] - q[0]);
        if (glm::dot(nrm, Vec3(p0.x - centre.x, 0.0, p0.z - centre.y)) < 0.0)
            s.poly(p, uv, 4, MatRender, TrimRender, owner, PartDome, -1, c.cRender);
        else
            s.poly(q, qu, 4, MatRender, TrimRender, owner, PartDome, -1, c.cRender);
    }
    const double base = y + drum;
    for (int ri = 0; ri < rings; ++ri)
    {
        const double b0 = kPi * 0.5 * ri / rings, b1 = kPi * 0.5 * (ri + 1) / rings;
        for (int k = 0; k < seg; ++k)
        {
            const double a0 = 2.0 * kPi * k / seg, a1 = 2.0 * kPi * (k + 1) / seg;
            auto at = [&](double a, double b) {
                return Vec3(centre.x + radius * std::cos(b) * std::cos(a), base + radius * std::sin(b), centre.y + radius * std::cos(b) * std::sin(a));
            };
            Vec3 p[4] = {at(a0, b0), at(a1, b0), at(a1, b1), at(a0, b1)};
            const glm::vec2 uv[4] = {{float(k / 6.0), float(ri / 2.5)}, {float((k + 1) / 6.0), float(ri / 2.5)}, {float((k + 1) / 6.0), float((ri + 1) / 2.5)},
                                     {float(k / 6.0), float((ri + 1) / 2.5)}};
            const Vec3 mid = 0.25 * (p[0] + p[1] + p[2] + p[3]);
            const Vec3 nrm = glm::cross(p[1] - p[0], p[3] - p[0]);
            if (glm::dot(nrm, mid - Vec3(centre.x, base, centre.y)) < 0.0)
            {
                std::swap(p[1], p[3]);
                const glm::vec2 u2[4] = {uv[0], uv[3], uv[2], uv[1]};
                s.poly(p, u2, ri + 1 == rings ? 4 : 4, MatRoof, TrimNone, owner, PartDome, -1, c.cRoof);
            }
            else
                s.poly(p, uv, 4, MatRoof, TrimNone, owner, PartDome, -1, c.cRoof);
        }
    }
}

void pyramidCap(Ctx& c, const Ring& ring, double y, double rise, int owner)
{
    const Vec2 ctr = centroid(ring);
    const Vec3 apex{ctr.x, y + rise, ctr.y};
    for (size_t e = 0; e < ring.size(); ++e)
    {
        const Vec2 a = ring[e], b = ring[(e + 1) % ring.size()];
        // Outward: a -> apex -> b would face in for a counter-clockwise (x, z) ring, so go b -> apex -> a... checked by normal.
        Vec3 p[3] = {{a.x, y, a.y}, {b.x, y, b.y}, apex};
        const Vec3 nrm = glm::cross(p[1] - p[0], p[2] - p[0]);
        const Vec2 mid = 0.5 * (a + b) - ctr;
        if (nrm.x * mid.x + nrm.z * mid.y < 0.0)
            std::swap(p[0], p[1]);
        const glm::vec2 uv[3] = {{0.0f, 0.0f}, {1.0f, 0.0f}, {0.5f, 1.0f}};
        c.r.shell.poly(p, uv, 3, MatRoof, TrimNone, owner, PartRoof, -1, c.cRoof);
    }
}

/** The ring with each edge moved in by its own distance (negative: out), corners where the moved edges meet. */
Ring edgesMovedIn(const Ring& ring, const std::vector<double>& in)
{
    const size_t n = ring.size();
    Ring out(n);
    for (size_t v = 0; v < n; ++v)
    {
        const size_t p = (v + n - 1) % n;
        const Vec2 np = outward(ring[p], ring[v]), nn = outward(ring[v], ring[(v + 1) % n]);
        const double dp = in[p], dn = in[v];
        const double det = np.x * nn.y - np.y * nn.x;
        if (std::abs(det) < 1e-6)
            out[v] = ring[v] - np * std::max(dp, dn);
        else
            out[v] = ring[v] + Vec2((-dp * nn.y + dn * np.y) / det, (-dn * np.x + dp * nn.x) / det);
    }
    return out;
}

void pitchedRoof(Ctx& c, const TierPlan& tier, const std::string& kind, int owner)
{
    // Party edges end in a gable at the party wall: pulled in by the overhang the roof adds, so the eaves never reach over
    // the neighbour's roof.
    std::vector<double> in(tier.ring.size(), 0.0);
    bool anyParty = false;
    for (size_t e = 0; e < in.size() && e < tier.sides.size(); ++e)
        if (tier.sides[e] == 1)
        {
            in[e] = c.st.overhang + kPartyGap + 1.0;
            anyParty = true;
        }
    const Ring ring = anyParty ? edgesMovedIn(tier.ring, in) : tier.ring;
    const double y = tier.top;
    lila::curve::Splines curves;
    {
        lila::curve::Spline s;
        s.kind = lila::curve::Kind::Poly;
        s.closed = true;
        for (const Vec2& q : ring)
            s.points.push_back({q.x, y, q.y});
        curves.push_back(std::move(s));
    }
    for (const Ring& h : tier.holes)
    {
        lila::curve::Spline s;
        s.kind = lila::curve::Kind::Poly;
        s.closed = true;
        for (const Vec2& q : cleanRing(h, false))
            s.points.push_back({q.x, y, q.y});
        curves.push_back(std::move(s));
    }
    // The span across the ridge sets the pitch from the envelope's roof height, else the style's.
    double across = 0.0;
    {
        const Vec2 rd = glm::length(c.spec.ridge) > 1e-9 ? glm::normalize(c.spec.ridge) : Vec2(1.0, 0.0);
        const Vec2 nd{-rd.y, rd.x};
        double lo = 1e300, hi = -1e300;
        for (const Vec2& q : ring)
        {
            lo = std::min(lo, glm::dot(q, nd));
            hi = std::max(hi, glm::dot(q, nd));
        }
        across = hi - lo;
    }
    lila::curve::RoofOptions ro;
    ro.normal = {0.0, 1.0, 0.0};
    ro.overhang = c.st.overhang;
    double pitch = c.st.pitch;
    if (kind == "sheet")
        pitch = std::min(pitch, 14.0);
    else if (c.spec.roofHeight > 1.0 && across > 1.0 && c.st.roof == "envelope")
        pitch = std::clamp(std::atan(2.0 * c.spec.roofHeight / across) * 180.0 / kPi, 8.0, 60.0);
    if (kind == "mansard")
        pitch = 60.0;
    ro.pitch = pitch;
    ro.uvScale = 200.0;
    if (kind == "gable" || kind == "sheet")
    {
        const Vec2 rd = glm::length(c.spec.ridge) > 1e-9 ? glm::normalize(c.spec.ridge) : Vec2(1.0, 0.0);
        for (size_t k = 0; k < ring.size(); ++k)
        {
            const Vec2 d = ring[(k + 1) % ring.size()] - ring[k];
            if (glm::length(d) > 1e-6 && std::abs(glm::dot(glm::normalize(d), rd)) < 0.35)
                ro.gables.push_back(int(k));
        }
        if (ro.gables.empty())
            ro.gableEnds = true;
    }
    for (size_t e = 0; e < tier.sides.size() && e < ring.size(); ++e)
        if (tier.sides[e] == 1 && std::find(ro.gables.begin(), ro.gables.end(), int(e)) == ro.gables.end())
            ro.gables.push_back(int(e));
    lila::curve::RoofResult rr;
    try
    {
        rr = lila::curve::roof(curves, ro);
    }
    catch (const std::exception& e)
    {
        c.r.warnings.push_back(std::string("roof: ") + e.what());
    }
    c.r.warnings.insert(c.r.warnings.end(), rr.warnings.begin(), rr.warnings.end());
    if (rr.mesh.numFaces() == 0)
    {
        c.r.warnings.push_back("the " + kind + " roof could not be built on this footprint; a flat roof stands in");
        flatCap(c, tier.ring, tier.holes, y, MatRooftop, PartRooftop, owner, c.cRoof);
        parapet(c, tier.ring, tier.sides, y, std::max(40.0, c.st.parapet), owner);
        return;
    }
    const auto* partAttr = rr.mesh.faceAttrs().find("part");
    const auto parts = partAttr ? partAttr->values<int32_t>() : std::span<const int32_t>{};
    const auto* uvAttr = rr.mesh.cornerAttrs().find(lila::geo::attr::uvName(0));
    const auto uvs = uvAttr ? uvAttr->values<glm::vec2>() : std::span<const glm::vec2>{};
    const int roofMat = (c.st.roofMaterial == "sheet" || kind == "sheet" || c.spec.roofMaterial == "metal") ? MatMetal : MatRoof;
    std::vector<Vec3> pts;
    std::vector<glm::vec2> uv;
    for (lila::geo::Index fi = 0; fi < rr.mesh.faceCapacity(); ++fi)
    {
        if (!rr.mesh.faceValid(fi))
            continue;
        pts.clear();
        uv.clear();
        const int part = fi < parts.size() ? parts[fi] : 0;
        for (lila::geo::Index cr : rr.mesh.faceCorners(fi))
        {
            const Vec3 p = rr.mesh.position(rr.mesh.cornerPoint(cr));
            pts.push_back(p);
            if (part == 1)
                uv.push_back(uvOf((p.x + p.z), p.y, y, TrimRender));
            else if (part == 3)
                uv.push_back(uvOf(p.x + p.z, p.y, y - 25.0, TrimBand));
            else
                uv.push_back(cr < uvs.size() ? uvs[cr] : glm::vec2(float(p.x / 200.0), float(p.z / 200.0)));
        }
        const int mat = part == 1 ? MatRender : part == 2 ? MatTrim : part == 3 ? MatTrim : roofMat;
        const int trim = part == 1 ? TrimRender : part == 3 ? TrimBand : TrimNone;
        const int pt = part == 1 ? PartGable : part == 2 ? PartSoffit : part == 3 ? PartCornice : PartRoof;
        const glm::vec4 col = part == 1 ? c.cRender : part == 0 ? c.cRoof : c.cTrim;
        c.r.shell.poly(pts.data(), uv.data(), pts.size(), mat, trim, owner, pt, -1, col);
    }
}

/** Water tanks, AC units, vents, antennas and rebar on a flat top, laid out with Weathering's rooftop rules. */
void roofClutter(Ctx& c, const TierPlan& tier, const std::string& tierPath)
{
    const Style& st = c.st;
    if (st.waterTanks <= 0.0 && st.clutter <= 0.0 && st.rebar <= 0.0)
        return;
    LILA_ZONE();
    // The top as a mesh for the rooftop layout.
    std::vector<Vec2> pts;
    std::vector<uint32_t> tris;
    triangulateWithHoles(tier.ring, tier.holes, pts, tris);
    if (tris.empty())
        return;
    lila::geo::Mesh host;
    std::vector<lila::geo::Index> ids;
    for (const Vec2& p : pts)
        ids.push_back(host.addPoint({p.x, tier.top, p.y}));
    for (size_t t = 0; t + 2 < tris.size(); t += 3)
        host.addFace({ids[tris[t]], ids[tris[t + 2]], ids[tris[t + 1]]});
    weathering::rooftop::Options o;
    o.edgeMargin = std::max(60.0, st.parapet > 0.0 ? 60.0 : 80.0);
    o.railings = 0.0;
    o.towers = 0.0;
    o.cell = 50.0;
    o.minArea = 4.0;
    o.seed = uint32_t(c.key & 0xffffffffu);
    weathering::rooftop::Result res;
    try
    {
        res = weathering::rooftop::layout(host, {}, {}, {}, {}, o);
    }
    catch (const std::exception&)
    {
        return;
    }
    const auto* freeAttr = res.surface.pointAttrs().find(weathering::rooftop::kFreeAttr);
    if (!freeAttr)
        return;
    const auto freeV = freeAttr->values<float>();
    std::vector<Vec3> spots;
    for (lila::geo::Index p = 0; p < res.surface.pointCapacity(); ++p)
        if (res.surface.pointValid(p) && p < freeV.size() && freeV[p] > 0.99f)
            spots.push_back(res.surface.position(p));
    if (spots.empty())
        return;
    // Deterministic picks, kept apart.
    std::vector<Vec3> taken;
    auto pick = [&](uint64_t salt, double clear) -> std::optional<Vec3> {
        for (int attempt = 0; attempt < 12; ++attempt)
        {
            const Vec3 p = spots[size_t(c.rnd(salt * 31 + uint64_t(attempt)) * double(spots.size())) % spots.size()];
            bool ok = true;
            for (const Vec3& q : taken)
                ok = ok && glm::length(Vec2(p.x - q.x, p.z - q.z)) > clear;
            if (ok)
            {
                taken.push_back(p);
                return p;
            }
        }
        return std::nullopt;
    };
    const Vec2 face = c.spec.front;
    if (c.rnd(tierPath + "/tanks") < st.waterTanks)
    {
        const int count = 1 + int(c.rnd(tierPath + "/tankCount") * 2.2);
        for (int k = 0; k < count; ++k)
            if (auto p = pick(100 + uint64_t(k), 160.0))
                c.addModule(groundModule(*p, face, "tank.water", "tank", tierPath + "/roof/tank" + std::to_string(k), {110.0, 130.0, 110.0}));
    }
    const double area = std::abs(signedArea(tier.ring)) / 10000.0;
    const int clutter = int(std::min(8.0, area * st.clutter * 0.04 + c.rnd(tierPath + "/clutterN") * st.clutter * 2.0));
    for (int k = 0; k < clutter; ++k)
        if (auto p = pick(200 + uint64_t(k), 120.0))
        {
            const double roll = c.rnd(tierPath + "/clutter" + std::to_string(k));
            if (roll < 0.5)
                c.addModule(groundModule(*p, face, "ac.unit", "ac", tierPath + "/roof/ac" + std::to_string(k), {90.0, 70.0, 60.0}, 1));
            else if (roll < 0.8)
                c.addModule(groundModule(*p, face, "roof.clutter.vent", "vent", tierPath + "/roof/vent" + std::to_string(k), {50.0, 60.0, 50.0}));
            else
                c.addModule(groundModule(*p, face, "roof.clutter.antenna", "antenna", tierPath + "/roof/antenna" + std::to_string(k), {30.0, 300.0, 30.0}));
        }
    if (c.rnd(tierPath + "/rebar") < st.rebar)
        for (int k = 0; k < 4; ++k)
        {
            // Rebar stands at the corners of the top, where the next floor's columns would rise.
            const Vec2 q = tier.ring[size_t(k) % tier.ring.size()];
            const Vec2 in = glm::normalize(centroid(tier.ring) - q) * 30.0;
            c.addModule(groundModule({q.x + in.x, tier.top, q.y + in.y}, face, "rebar", "rebar", tierPath + "/roof/rebar" + std::to_string(k), {20.0, 90.0, 20.0}));
        }
}

void roofs(Ctx& c, std::vector<TierPlan>& tiers)
{
    for (size_t t = 0; t < tiers.size(); ++t)
    {
        TierPlan& tier = tiers[t];
        const std::string tp = "t" + std::to_string(t);
        if (!tier.cap.empty())
        {
            Module m;
            m.type = tier.cap == "dome" ? "dome" : "roof.hip";
            m.kind = "roof";
            m.path = tp + "/roof";
            m.origin = {centroid(tier.ring).x, tier.top, centroid(tier.ring).y};
            m.size = {200.0, 200.0, 200.0};
            m.shell = true;
            const int owner = c.addModule(std::move(m));
            flatCap(c, tier.ring, {}, tier.top, MatRooftop, PartRooftop, owner, c.cRoof);
            double minSide = 1e300;
            for (size_t e = 0; e < tier.ring.size(); ++e)
                minSide = std::min(minSide, glm::length(tier.ring[(e + 1) % tier.ring.size()] - tier.ring[e]));
            if (tier.cap == "dome")
                dome(c, centroid(tier.ring), minSide * 0.42, tier.top, owner, 40.0);
            else
                pyramidCap(c, tier.ring, tier.top, minSide * 0.8, owner);
            continue;
        }
        std::string kind = "flat";
        if (tier.roofed)
        {
            if (c.ed.roof)
                kind = *c.ed.roof;
            else if (c.st.roof == "envelope")
                kind = c.spec.roof.empty() ? "flat" : c.spec.roof;
            else
                kind = c.st.roof;
        }
        if (kind == "skillion" || kind == "shed")
            kind = "flat";
        // A style's pitched roof over a footprint wider than a house would stand as a hill: wide tiers take a flat one.
        if (!c.ed.roof && kind != "flat" && kind != "dome" && ringWidth(tier.ring) > kWidestPitched)
            kind = "flat";
        const bool flat = kind == "flat" || kind == "dome";
        Module m;
        m.type = flat ? "roof.flat" : kind == "sheet" ? "roof.sheet" : (c.st.roofMaterial == "tile" ? "roof.tiled" : "roof." + kind);
        if (kind == "hip" || kind == "gable")
            m.type = c.st.roofMaterial == "sheet" ? "roof.sheet" : "roof." + kind;
        m.kind = "roof";
        m.path = tp + "/roof";
        const Vec2 ctr = centroid(tier.ring);
        m.origin = {ctr.x, tier.top, ctr.y};
        m.size = {100.0, 100.0, 100.0};
        m.shell = true;
        m.data = {{"roof", kind}};
        const int owner = c.addModule(std::move(m));
        // A lower tier's top is a terrace round the tier above it.
        std::vector<Ring> holes = tier.holes;
        if (!tier.roofed)
            for (size_t u = t + 1; u < tiers.size(); ++u)
                if (std::abs(tiers[u].bottom - tier.top) < 30.0 && tiers[u].cap.empty())
                {
                    holes.push_back(tiers[u].ring);
                    break;
                }
        if (flat)
        {
            flatCap(c, tier.ring, holes, tier.top, MatRooftop, PartRooftop, owner, c.cRoof);
            const double ph = tier.roofed ? c.st.parapet : std::max(c.st.parapet, 100.0);
            if (ph > 1.0)
            {
                parapet(c, tier.ring, tier.sides, tier.top, ph, owner);
                for (const Ring& h : tier.holes)
                    parapet(c, cleanRing(h, false), {}, tier.top, ph, owner);
            }
            if (tier.roofed)
            {
                roofClutter(c, tier, tp);
                if (kind == "dome" || c.st.domes > 0)
                {
                    double minSide = 1e300;
                    for (size_t e = 0; e < tier.ring.size(); ++e)
                        minSide = std::min(minSide, glm::length(tier.ring[(e + 1) % tier.ring.size()] - tier.ring[e]));
                    const int domes = std::max(1, c.st.domes);
                    for (int d = 0; d < domes; ++d)
                    {
                        Module dm;
                        dm.type = "dome";
                        dm.kind = "dome";
                        dm.path = tp + "/dome" + std::to_string(d);
                        const double r = std::clamp(minSide * (domes > 1 ? 0.18 : 0.28), 120.0, 900.0);
                        Vec2 at = ctr;
                        if (domes > 1)
                        {
                            const Vec2 fwd = glm::length(c.spec.front) > 1e-9 ? glm::normalize(c.spec.front) : Vec2(0, 1);
                            at = ctr + fwd * (minSide * 0.25 * (d == 0 ? 1.0 : -1.0));
                            if (!inside(tier.ring, at))
                                at = ctr;
                        }
                        dm.origin = {at.x, tier.top, at.y};
                        dm.size = {2 * r, r + 60.0, 2 * r};
                        dm.shell = true;
                        const int downer = c.addModule(std::move(dm));
                        dome(c, at, r, tier.top, downer, 60.0);
                    }
                }
            }
        }
        else
            pitchedRoof(c, tier, kind, owner);
    }
}

// Extras --------------------------------------------------------------------------------------------------------------------

/** The front street facade's edge of the base ring, the one facing most along the front. */
int frontEdge(const Ring& ring, const std::vector<int>& sides, Vec2 front)
{
    int best = -1;
    double score = -1e300;
    for (size_t e = 0; e < ring.size(); ++e)
    {
        const double d = glm::dot(outward(ring[e], ring[(e + 1) % ring.size()]), front) + (sides[e] == 0 ? 1.0 : 0.0);
        if (d > score)
        {
            score = d;
            best = int(e);
        }
    }
    return best;
}

void extras(Ctx& c, const TierPlan& base)
{
    const Style& st = c.st;
    const Ring& ring = base.ring;
    const int fe = frontEdge(ring, base.sides, c.spec.front);
    if (fe < 0)
        return;
    const Vec2 a = ring[size_t(fe)], b = ring[(size_t(fe) + 1) % ring.size()];
    const Vec2 nrm = outward(a, b), along = glm::normalize(b - a), mid = 0.5 * (a + b);
    const std::string fp = "t0/" + base.edgeIds[size_t(fe)];

    // Portico: columns in front of the main door with a slab over them.
    if (st.columns > 0)
    {
        const int n = std::clamp(st.columns, 2, 8);
        const double span = std::min(glm::length(b - a) * 0.6, 250.0 * (n - 1));
        const double depth = 280.0;
        const double h = std::min(c.r.wallTop - c.floor0, c.H0 + (c.floors > 1 ? c.H : 0.0));
        for (int k = 0; k < n; ++k)
        {
            const Vec2 q = mid + along * (-span * 0.5 + span * k / (n - 1)) + nrm * depth;
            c.addModule(groundModule({q.x, c.floor0, q.y}, nrm, "column", "column", fp + "/portico/c" + std::to_string(k), {50.0, h, 50.0}));
        }
        // The slab runs from just inside the wall face out past the columns, clear of the roof behind the wall.
        const double slabMid = (depth + 30.0) * 0.5 - 2.0;
        Module slab = groundModule({mid.x + nrm.x * slabMid, c.floor0 + h, mid.y + nrm.y * slabMid}, nrm, "portico", "portico", fp + "/portico",
                                   {span + 120.0, 70.0, depth + 30.0});
        c.addModule(std::move(slab));
        // The porch floor stands on the ground in front of the door, not the building's lowest corner.
        const Vec2 porch = mid + nrm * depth * 0.5;
        const double porchGround = std::min(c.spec.groundAt(porch), c.floor0 - 15.0);
        c.addModule(groundModule({porch.x, porchGround, porch.y}, nrm, "steps", "steps", fp + "/portico/floor",
                                 {span + 160.0, c.floor0 - porchGround, depth + 80.0}));
    }

    // Perimeter wall round the lot, with a gate on the front, and a pool behind the house.
    if (st.perimeterWall > 0.0)
    {
        // Clear of a portico's columns; never past half the gap to a neighbour, and short of a party line.
        const double off = std::max(std::clamp(std::max(c.spec.setbackFront, c.spec.setbackSide), 250.0, 900.0), st.columns > 0 ? 560.0 : 0.0);
        std::vector<double> in(ring.size(), -off);
        for (size_t e = 0; e < ring.size(); ++e)
        {
            const double clear = e < base.clear.size() ? base.clear[e] : 1e9;
            if (base.sides[e] == 1)
                in[e] = kPartyGap;
            else if (clear < 1e8)
                in[e] = -std::max(0.0, std::min(off, clear * 0.5 - 20.0));
        }
        const Ring wall = edgesMovedIn(ring, in);
        const size_t n = wall.size();
        std::vector<char> drawn(n, 0);
        for (size_t e = 0; e < n; ++e)
            drawn[e] = base.sides[e] != 1 && in[e] <= -30.0 && glm::length(wall[(e + 1) % n] - wall[e]) >= 50.0;
        // One mitred band round the lot, cut for the gate; each side a shell module rules can swap.
        auto band = [&](size_t e, BandEnd s0, BandEnd s1, const std::string& id) {
            const Vec2 p = wall[e], q = wall[(e + 1) % n];
            const Vec2 m2 = 0.5 * (p + q);
            const double g = c.spec.groundAt(m2);
            const double low = std::min({g, c.spec.groundAt(p), c.spec.groundAt(q)}) - 20.0, top = g + st.perimeterWall;
            Module m = groundModule({m2.x, g, m2.y}, outward(p, q), "wall.perimeter", "perimeter", id, {glm::length(q - p), st.perimeterWall, 30.0});
            m.shell = true;
            const int owner = c.addModule(std::move(m));
            ringBand(c.r.shell, wall, e, s0, s1, low, top - 8.0, -15.0, 15.0, MatRender, TrimRender, owner, PartWall, c.cRender, false, false, true);
            ringBand(c.r.shell, wall, e, s0, s1, top - 8.0, top, -18.0, 18.0, MatTrim, TrimCoping, owner, PartParapet, c.cTrim, true, true, true);
        };
        for (size_t e = 0; e < n; ++e)
        {
            if (!drawn[e])
                continue;
            const Vec2 p = wall[e], q = wall[(e + 1) % n];
            const double L = glm::length(q - p);
            const BandEnd start{drawn[(e + n - 1) % n] != 0, 0.0}, end{drawn[(e + 1) % n] != 0, 0.0};
            const std::string id = "site/w" + std::to_string(e);
            if (st.gate && int(e) == fe && L > 600.0)
            {
                const double gw = 420.0;
                // The walls run into the gate's posts.
                const double cut = L * 0.5 + gw * 0.5 + 15.0;
                band(e, start, {false, cut}, id + "a");
                band(e, {false, cut}, end, id + "b");
                const Vec2 gq = p + (q - p) * 0.5;
                Module g = groundModule({gq.x, c.spec.groundAt(gq), gq.y}, outward(p, q), "gate", "gate", "site/gate", {gw, st.perimeterWall + 40.0, 20.0});
                c.addModule(std::move(g));
            }
            else
                band(e, start, end, id);
        }
        if (st.pool && off >= 400.0)
        {
            const Vec2 ctr = centroid(ring);
            double depth = 0.0;
            for (const Vec2& q : ring)
                depth = std::max(depth, glm::dot(q - ctr, -c.spec.front));
            const Vec2 at = ctr - glm::normalize(c.spec.front) * (depth + std::min(off * 0.5, 350.0));
            c.addModule(groundModule({at.x, c.spec.groundAt(at), at.y}, c.spec.front, "pool", "pool", "site/pool", {700.0, 0.0, std::min(off * 0.8, 400.0)}));
        }
    }
}

// Proxies -------------------------------------------------------------------------------------------------------------------

void proxies(Ctx& c, const std::vector<TierPlan>& tiers)
{
    Result& r = c.r;
    for (const TierPlan& t : tiers)
    {
        for (int pass = 0; pass < 2; ++pass)
        {
            Soup& s = pass == 0 ? r.hlod : r.far;
            const double bottom = pass == 1 ? c.groundLo - 20.0 : t.bottom;
            const double top = t.top;
            const size_t n = t.ring.size();
            for (size_t e = 0; e < n; ++e)
            {
                const Vec2 a = t.ring[e], b = t.ring[(e + 1) % n];
                const double L = glm::length(b - a);
                if (L < 1.0)
                    continue;
                const Frame f = frameOf(a, b, 0.0);
                const glm::vec2 uv[4] = {uvOf(0, bottom, bottom, -1), uvOf(0, top, bottom, -1), uvOf(L, top, bottom, -1), uvOf(L, bottom, bottom, -1)};
                quad(s, f.at(0.0, bottom), kUp * (top - bottom), f.u * L, uv, MatRender, TrimNone, -1, PartWall, -1, c.cRender);
            }
            std::vector<Vec2> pts;
            std::vector<uint32_t> tris;
            triangulateWithHoles(t.ring, pass == 0 ? t.holes : std::vector<Ring>{}, pts, tris);
            for (size_t k = 0; k + 2 < tris.size(); k += 3)
            {
                const Vec2 q[3] = {pts[tris[k]], pts[tris[k + 2]], pts[tris[k + 1]]};
                const Vec3 p[3] = {{q[0].x, top, q[0].y}, {q[1].x, top, q[1].y}, {q[2].x, top, q[2].y}};
                s.poly(p, nullptr, 3, MatRooftop, TrimNone, -1, PartRooftop, -1, c.cRoof);
            }
            if (pass == 1)
                break;
        }
    }
    // HLOD keeps pitched roofs as shapes: copy the roof faces of the shell.
    for (size_t f = 0, off = 0; f < r.shell.sizes.size(); off += r.shell.sizes[f], ++f)
    {
        const int part = r.shell.part[f];
        if (part != PartRoof && part != PartGable && part != PartDome)
            continue;
        r.hlod.poly(r.shell.pts.data() + off, r.shell.uvs.data() + off, r.shell.sizes[f], r.shell.mat[f], TrimNone, -1, part, -1, r.shell.colour[f]);
    }
}

Palette paletteFor(const Ctx& c)
{
    const Style& st = c.st;
    Palette p;
    auto pickIndex = [&](size_t n, uint64_t salt) { return n == 0 ? 0 : int(std::min<double>(double(n) - 1.0, c.rnd(salt) * double(n))); };
    p.renderIndex = pickIndex(st.renders.size(), 11);
    p.trimIndex = pickIndex(st.trims.size(), 12);
    p.roofIndex = pickIndex(st.roofs.size(), 13);
    const float jitter = float(0.94 + c.rnd(14) * 0.12);
    p.render = st.renders[size_t(p.renderIndex)] * jitter;
    p.trim = st.trims[size_t(p.trimIndex)];
    p.roof = st.roofs[size_t(p.roofIndex)];
    p.base = st.bases[size_t(pickIndex(st.bases.size(), 15))];
    p.dirt = float(st.dirt.x + (st.dirt.y - st.dirt.x) * c.rnd(16));
    if (!c.spec.colour.empty())
    {
        p.render = hexColour(c.spec.colour, p.render);
        p.renderIndex = -1;
    }
    if (!c.spec.roofColour.empty())
    {
        p.roof = hexColour(c.spec.roofColour, p.roof);
        p.roofIndex = -1;
    }
    if (c.ed.palette.contains("render") && c.ed.palette["render"].is_string())
    {
        p.render = hexColour(c.ed.palette["render"].get<std::string>(), p.render);
        p.renderIndex = -1;
    }
    if (c.ed.palette.contains("trim") && c.ed.palette["trim"].is_string())
    {
        p.trim = hexColour(c.ed.palette["trim"].get<std::string>(), p.trim);
        p.trimIndex = -1;
    }
    if (c.ed.palette.contains("roof") && c.ed.palette["roof"].is_string())
    {
        p.roof = hexColour(c.ed.palette["roof"].get<std::string>(), p.roof);
        p.roofIndex = -1;
    }
    if (c.ed.palette.contains("dirt") && c.ed.palette["dirt"].is_number())
        p.dirt = c.ed.palette["dirt"].get<float>();
    return p;
}
} // namespace

bool isBusiness(const Spec& s)
{
    static const char* kShops[] = {"shop", "retail", "commercial", "restaurant", "bar", "cafe", "pharmacy", "bank", "supermarket",
                                   "fast_food", "pub", "kiosk", "office", "hotel", "market", "marketplace", "cantina", "nightclub"};
    if (!s.businesses.empty())
        return true;
    for (const char* k : kShops)
        if (s.program == k || s.groundUse == k)
            return true;
    return s.groundUse == "commercial" || s.use == "commercial" || s.use == "mixed";
}

json Palette::toJson() const
{
    return {{"render", colourHex(render)}, {"trim", colourHex(trim)}, {"roof", colourHex(roof)}, {"base", colourHex(base)}, {"dirt", dirt},
            {"renderIndex", renderIndex}, {"trimIndex", trimIndex}, {"roofIndex", roofIndex}};
}

glm::dmat4 Module::frame() const
{
    glm::dmat4 m(1.0);
    m[0] = glm::dvec4(x, 0.0);
    m[1] = glm::dvec4(y, 0.0);
    m[2] = glm::dvec4(z, 0.0);
    m[3] = glm::dvec4(origin, 1.0);
    return m;
}

std::shared_ptr<const Result> generate(const Spec& spec, const Style& style, const Edits& ed, const Options& opts)
{
    LILA_ZONE();
    auto out = std::make_shared<Result>();
    Result& r = *out;
    r.id = spec.id;
    Spec sp = spec;
    if (sp.sides.size() != sp.footprint.size())
        sp.sides.resize(sp.footprint.size(), 0);
    Ctx c{sp, style, ed, r};
    c.key = spec.key ^ (ed.seed ? fnv(std::to_string(*ed.seed)) : 0ull);
    if (ed.floorHeight)
    {
        c.st.floorHeight = std::clamp(*ed.floorHeight, 200.0, 1200.0);
        c.st.groundHeight = std::max(c.st.groundHeight, c.st.floorHeight);
    }
    r.style = style.id;
    if (spec.footprint.size() < 3 || std::abs(signedArea(spec.footprint)) < 1e4)
    {
        r.warnings.push_back("the footprint is smaller than a square metre");
        return out;
    }

    // Ground, floors and heights.
    std::tie(c.groundLo, c.groundHi) = spec.groundRange();
    // Stepping needs to know where the ground is high: only with sampled corners.
    c.slope = spec.sloped();
    c.floor0 = c.groundHi + c.st.plinth;
    const bool given = ed.floors.has_value();
    c.floors = given ? std::clamp(*ed.floors, 1, 120) : std::clamp(spec.floors, c.st.minFloors, c.st.maxFloors);
    c.H0 = c.st.groundHeight;
    c.H = c.st.floorHeight;
    if (!given && c.floors == spec.floors && spec.top > c.floor0 + 200.0 && !ed.floorHeight)
    {
        // Fit the storeys to the envelope's height, within reason, so imported heights hold.
        const bool pitched = spec.roof == "hip" || spec.roof == "gable" || spec.roof == "mansard" || spec.roof == "dome";
        const double wallTop = spec.top - (pitched ? spec.roofHeight : 0.0);
        const double want = wallTop - c.floor0, have = c.H0 + (c.floors - 1) * c.H;
        if (want > 100.0 && have > 1.0)
        {
            const double k = std::clamp(want / have, 0.8, 1.3);
            c.H0 *= k;
            c.H *= k;
        }
    }
    c.business = isBusiness(spec);
    r.enterable = ed.enterable.value_or(c.st.enterable);
    c.glazing = r.enterable ? 10 : 0;
    c.program = interiorProgramFor(spec, style, ed.interior.is_object() ? ed.interior.value("program", std::string()) : std::string());
    r.floors = c.floors;
    r.palette = paletteFor(c);
    c.cRender = glm::vec4(r.palette.render, r.palette.dirt);
    c.cTrim = glm::vec4(r.palette.trim, r.palette.dirt);
    c.cBase = glm::vec4(r.palette.base, r.palette.dirt);
    c.cRoof = glm::vec4(r.palette.roof, r.palette.dirt);
    r.shell.reserve(512);

    std::vector<TierPlan> tiers;
    {
        LILA_ZONE_N("buildings.tiers");
        tiers = planTiers(c);
        for (TierPlan& tier : tiers)
            measureTier(c, tier);
        addTowers(c, tiers);
    }
    if (tiers.empty())
    {
        r.warnings.push_back("no tier could be laid on the footprint");
        return out;
    }
    {
        LILA_ZONE_N("buildings.facades");
        for (size_t t = 0; t < tiers.size(); ++t)
            if (!tiers[t].cap.empty())
                measureTier(c, tiers[t]);
        for (size_t t = 0; t < tiers.size(); ++t)
        {
            const TierPlan& tier = tiers[t];
            for (size_t e = 0; e < tier.ring.size(); ++e)
                emitFacade(c, tier, int(t), e, false, -1, tier.ring, tier.sides[e], tier.edgeIds[e]);
            for (size_t h = 0; h < tier.holes.size(); ++h)
            {
                const Ring hole = cleanRing(tier.holes[h], false);
                for (size_t e = 0; e < hole.size(); ++e)
                    emitFacade(c, tier, int(t), e, true, int(h), hole, 2, "f" + std::to_string(e));
            }
        }
    }
    {
        LILA_ZONE_N("buildings.roofs");
        roofs(c, tiers);
    }
    {
        LILA_ZONE_N("buildings.extras");
        extras(c, tiers.front());
    }

    // Floor plates and the core (W4).
    for (size_t t = 0; t < tiers.size(); ++t)
        for (const TierPlan::Floor& fl : tiers[t].floors)
        {
            FloorPlate p;
            p.tier = int(t);
            p.floor = fl.index;
            p.y = fl.y0;
            p.height = fl.y1 - fl.y0;
            p.outline = cleanRing(offsetRing(tiers[t].ring, -c.st.wallThickness));
            p.holes = tiers[t].holes;
            p.ring = tiers[t].ring;
            p.wall = c.st.wallThickness;
            p.tower = !tiers[t].cap.empty();
            r.plates.push_back(std::move(p));
        }
    {
        const Ring& base = tiers.front().ring;
        Vec2 core = centroid(base);
        if (!inside(base, core))
        {
            const Ring in = cleanRing(offsetRing(base, -200.0));
            core = in.size() >= 3 ? centroid(in) : base.front();
        }
        r.core = core;
        r.coreSize = c.floors > 1 ? 450.0 : 0.0;
    }

    if (opts.proxies)
    {
        LILA_ZONE_N("buildings.proxies");
        proxies(c, tiers);
    }

    // Bounds.
    r.lo = Vec3(1e300);
    r.hi = Vec3(-1e300);
    for (const Vec3& p : r.shell.pts)
    {
        r.lo = glm::min(r.lo, p);
        r.hi = glm::max(r.hi, p);
    }
    for (Soup* s : {&r.shell, &r.hlod, &r.far})
        s->shrink();
    r.modules.shrink_to_fit();
    r.bays.shrink_to_fit();
    r.facades.shrink_to_fit();
    r.plates.shrink_to_fit();
    r.openings.shrink_to_fit();
    return out;
}
} // namespace buildings
