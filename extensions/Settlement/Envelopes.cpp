// Settlement: building envelopes on lots (setbacks by block type, tiers, roofs, entrances, corners, foundations), party
// walls matched between neighbours, greybox massing, and the JSON consumers read.

#include "Parcels.h"

#include <lila/Curves.h>
#include <lila/MeshOps.h>
#include <lila/Profile.h>
#include <lila/Skeleton.h>

#include <algorithm>
#include <cmath>
#include <map>
#include <numeric>

namespace settlement
{
namespace
{
json xzOf(Vec2 p) { return json::array({p.x, p.y}); }

json ringOf(const Polygon& p)
{
    json a = json::array();
    for (const Vec2& q : p.pts)
        a.push_back(xzOf(q));
    return a;
}

/** Extent of the polygon across the direction d (perpendicular to it) and along it. */
void spans(const Polygon& p, Vec2 d, double& across, double& along)
{
    const Vec2 n = leftNormal(d);
    double a0 = 1e300, a1 = -1e300, b0 = 1e300, b1 = -1e300;
    for (const Vec2& q : p.pts)
    {
        a0 = std::min(a0, glm::dot(q, n)), a1 = std::max(a1, glm::dot(q, n));
        b0 = std::min(b0, glm::dot(q, d)), b1 = std::max(b1, glm::dot(q, d));
    }
    across = a1 - a0;
    along = b1 - b0;
}

Polygon insetBy(const Polygon& p, double d)
{
    const std::vector<double> ds(p.size(), d);
    return inset(p, ds);
}

/**
 * One counter-clockwise outline with the courtyards bridged in (each hole joined to the outline by a cut to its nearest
 * visible corner), for ear clipping a face with holes. Holes come out clockwise. Empty when a hole can't be bridged.
 */
std::vector<Vec2> bridgedOutline(const Polygon& outer, const std::vector<const Polygon*>& holes)
{
    std::vector<Vec2> ring = outer.pts;
    if (signedArea(ring) < 0.0)
        std::reverse(ring.begin(), ring.end());
    std::vector<std::vector<Vec2>> hs;
    for (const Polygon* h : holes)
    {
        std::vector<Vec2> r = h->pts;
        if (signedArea(r) > 0.0)
            std::reverse(r.begin(), r.end());
        hs.push_back(std::move(r));
    }
    // Rightmost holes first, so each cut runs to the outline or to an already bridged hole.
    std::sort(hs.begin(), hs.end(), [](const auto& a, const auto& b) {
        double ma = -1e300, mb = -1e300;
        for (const Vec2& q : a)
            ma = std::max(ma, q.x);
        for (const Vec2& q : b)
            mb = std::max(mb, q.x);
        return ma > mb;
    });
    auto crosses = [](Vec2 a, Vec2 b, Vec2 c, Vec2 d) {
        const double d1 = cross(b - a, c - a), d2 = cross(b - a, d - a), d3 = cross(d - c, a - c), d4 = cross(d - c, b - c);
        return ((d1 > 1e-9 && d2 < -1e-9) || (d1 < -1e-9 && d2 > 1e-9)) && ((d3 > 1e-9 && d4 < -1e-9) || (d3 < -1e-9 && d4 > 1e-9));
    };
    for (size_t k = 0; k < hs.size(); ++k)
    {
        const std::vector<Vec2>& h = hs[k];
        size_t m = 0;
        for (size_t i = 1; i < h.size(); ++i)
            if (h[i].x > h[m].x)
                m = i;
        size_t best = SIZE_MAX;
        double bestD = 1e300;
        for (size_t i = 0; i < ring.size(); ++i)
        {
            const double d = glm::length(ring[i] - h[m]);
            if (d >= bestD)
                continue;
            bool clear = true;
            auto test = [&](const std::vector<Vec2>& r) {
                for (size_t j = 0; j < r.size() && clear; ++j)
                    clear = !crosses(h[m], ring[i], r[j], r[(j + 1) % r.size()]);
            };
            test(ring);
            for (size_t j = k; j < hs.size() && clear; ++j)
                test(hs[j]);
            if (clear)
                best = i, bestD = d;
        }
        if (best == SIZE_MAX)
            return {};
        std::vector<Vec2> merged(ring.begin(), ring.begin() + std::ptrdiff_t(best) + 1);
        for (size_t i = 0; i <= h.size(); ++i)
            merged.push_back(h[(m + i) % h.size()]);
        merged.push_back(ring[best]);
        merged.insert(merged.end(), ring.begin() + std::ptrdiff_t(best) + 1, ring.end());
        ring = std::move(merged);
    }
    return ring;
}

/** Ground under the footprint: its corners and a grid inside it no coarser than the ground's cells. */
void groundUnder(const Polygon& fp, const Ground& g, double& lo, double& hi)
{
    lo = std::numeric_limits<double>::max();
    hi = -lo;
    auto take = [&](Vec2 q) {
        const double h = g.height(q);
        lo = std::min(lo, h);
        hi = std::max(hi, h);
    };
    for (const Vec2& q : fp.pts)
        take(q);
    take(centroid(fp));
    if (g.isPlane())
        return;
    Vec2 x0(1e300), x1(-1e300);
    for (const Vec2& q : fp.pts)
        x0 = glm::min(x0, q), x1 = glm::max(x1, q);
    const double step = std::clamp(std::min(g.cellSize(), std::max(x1.x - x0.x, x1.y - x0.y) / 6.0), 50.0, 2000.0);
    for (double z = x0.y; z <= x1.y + 1e-6; z += step)
        for (double x = x0.x; x <= x1.x + 1e-6; x += step)
            if (contains(fp.pts, {x, z}))
                take({x, z});
    for (size_t k = 0; k < fp.size(); ++k)
        take((fp.pts[k] + fp.pts[(k + 1) % fp.size()]) * 0.5);
}

/** The corner between two street sides cut off square to its bisector. */
Polygon chamfer(const Polygon& fp, double size)
{
    Polygon out = fp;
    for (size_t k = 0; k < out.size(); ++k)
    {
        const size_t n = out.size(), prev = (k + n - 1) % n;
        if (out.tag[prev] != 0 || out.tag[k] != 0)
            continue;
        const Vec2 a = out.pts[k];
        const Vec2 e1 = out.pts[prev] - a, e2 = out.pts[(k + 1) % n] - a;
        if (glm::length(e1) < 3.0 * size || glm::length(e2) < 3.0 * size)
            continue;
        const Vec2 u1 = glm::normalize(e1), u2 = glm::normalize(e2);
        if (glm::dot(u1, u2) > 0.5 || cross(u2, u1) > 0.0)
            continue;
        const Vec2 b = glm::normalize(u1 + u2);
        Polygon cut = clipHalfPlane(out, a + b * size, b, 0);
        if (cut.size() >= 3)
            return cut;
    }
    return out;
}

const char* sideName2(int tag) { return tag == 0 ? "street" : tag == 1 ? "party" : "open"; }

/** cm: the widest footprint a pitched roof spans. */
constexpr double kWidestPitched = 4000.0;
} // namespace

Envelope parcelEnvelope(const Lot& lot, const Block& block, const Layout& l, const Ground& g, const Options& o, const Site& site)
{
    LILA_ZONE();
    const Profile& p = o.profile; // an era or district restyle passes its own profile here
    const std::string era = eraOf(p);
    const bool pm = preModern(era);
    const bool landmark = !lot.landmark.empty();
    double front = p.frontSetback, side = 0.0, depth = p.buildDepth;
    switch (block.type)
    {
    case BlockType::Open:
        front = std::max(p.frontSetback, 300.0);
        side = std::max(p.sideSetback, 200.0);
        break;
    case BlockType::Superblock:
        front = std::max(p.frontSetback, 800.0);
        side = std::max(p.sideSetback, 800.0);
        depth = 1.5 * p.buildDepth;
        break;
    case BlockType::Courtyard: depth = std::min(p.buildDepth, p.lotDepth); break;
    default: break;
    }
    if (landmark && (lot.use == Use::Religious || lot.use == Use::Civic))
    {
        front = std::max(front, 300.0);
        side = std::max(side, 300.0);
        depth = 1.6 * p.buildDepth;
    }
    // Everything but the generators: they hold the imported streets and buildings as JSON, megabytes for a city.
    Options local;
    local.seed = o.seed;
    local.growth = o.growth;
    local.heightScale = o.heightScale;
    local.widthScale = o.widthScale;
    local.lotScale = o.lotScale;
    local.blocks = o.blocks;
    local.buildings = o.buildings;
    local.walk = o.walk;
    local.profile = p;
    local.profile.frontSetback = front;
    local.profile.sideSetback = side;
    local.profile.buildDepth = depth;
    Envelope env = envelopeFor(lot, l, g, local);
    if (env.footprint.pts.empty())
        return env;
    env.key = lot.key;
    env.frontSetback = front;
    env.sideSetback = side;
    env.landmark = lot.landmark;
    env.eraName = era;
    env.wealth = lot.wealth;
    env.route = lot.route;
    for (int& t : env.footprint.tag)
        t = t == 0 ? 0 : 2;

    const Vec2 c0 = centroid(env.footprint);
    const double dist = std::clamp(glm::length(c0 - l.center) / std::max(l.radius, 1.0), 0.0, 1.0);
    if (lot.corner)
        env.corner = era == "early" ? (landmark ? "tower" : "chamfer") : "wrap";
    if (env.corner == "chamfer")
    {
        double across = 0.0, along = 0.0;
        spans(env.footprint, glm::normalize(Vec2(-env.front.y, env.front.x)), across, along);
        env.footprint = chamfer(env.footprint, std::clamp(0.15 * std::min(across, along), 100.0, 400.0));
    }

    int floors = env.floors;
    double storey = p.floorHeight;
    // A piece of a larger town ranks its streets only against each other, so its busiest would rise like the town's
    // busiest: its heights take average access instead.
    const double access = p.townRadius > 0.0 ? 0.5 : lot.access;
    if (!pm && lot.use != Use::Religious)
        floors = std::max(1, int(std::lround(floors * (0.7 + 0.6 * access))));
    switch (lot.use)
    {
    case Use::Religious: floors = std::max(floors, 2), storey *= 1.6; break;
    case Use::Industrial: floors = std::clamp(floors, 1, 3), storey *= 1.4; break;
    case Use::Port: floors = std::clamp(floors, 2, 4); break;
    case Use::Market: floors = std::clamp(floors, 1, 2), storey *= 1.3; break;
    default: break;
    }
    env.floors = floors;
    env.height = floors * storey;

    double lo = 0.0, hi = 0.0;
    groundUnder(env.footprint, g, lo, hi);
    env.foundationSource = "ground";
    if (auto it = site.foundations.find(lot.key); it != site.foundations.end())
    {
        lo = std::min(lo, it->second.min);
        hi = std::max(hi, it->second.max);
        env.foundationSource = "terrainRegionStats";
    }
    env.ground = lo;
    env.base = lo - p.foundation;
    env.foundationDepth = hi - env.base;
    env.top = hi + env.height;

    // Tall buildings step back as they rise (the setback towers of zoning codes).
    Polygon tierFp = env.footprint;
    double bottom = env.base;
    if (!pm && floors > 10)
    {
        double across = 0.0, along = 0.0;
        spans(tierFp, glm::normalize(Vec2(-env.front.y, env.front.x)), across, along);
        const double step = std::clamp(0.1 * std::min(across, along), 200.0, 600.0);
        const double tops[] = {hi + std::max(6, int(std::lround(0.4 * floors))) * storey, hi + std::lround(0.75 * floors) * storey};
        for (double t : tops)
        {
            Polygon next = insetBy(tierFp, step);
            if (t >= env.top || next.size() < 3 || area(next) < 0.3 * area(tierFp))
                break;
            env.tiers.push_back({tierFp, bottom, t});
            bottom = t;
            tierFp = std::move(next);
        }
    }
    env.tiers.push_back({tierFp, bottom, env.top});

    const bool tall = env.tiers.size() > 1;
    // Otherwise the style's own roof stands: an era or district profile names the roof its buildings take.
    if (tall || (lot.use == Use::Industrial && !pm))
        env.roof = "flat";
    else if (lot.use == Use::Religious)
        env.roof = era == "early" && landmark ? "dome" : "gable";
    else if (lot.use == Use::Civic && landmark)
        env.roof = era == "early" ? "dome" : "hip";
    else if (env.roof != "gable" && env.roof != "hip" && env.roof != "flat" && env.roof != "mansard" && env.roof != "dome")
        env.roof = "gable";
    const double pitch = (era == "medieval" ? 50.0 : era == "early" ? 40.0 : era == "industrial" ? 35.0 : 30.0) + (lot.use == Use::Religious ? 5.0 : 0.0);
    const Vec2 along = glm::normalize(Vec2(-env.front.y, env.front.x));
    Vec2 bc{0.0}, bu{1.0, 0.0}, bv{0.0, 1.0};
    double hu = 0.0, hv = 0.0;
    boxOf(tierFp, bc, bu, bv, hu, hv);
    if (landmark || block.type == BlockType::Open || block.type == BlockType::Superblock)
        env.ridge = bu;
    else if (era == "medieval")
        env.ridge = env.front; // gable end to the street
    else
        env.ridge = along;
    double across = 0.0, ridgeLen = 0.0;
    spans(tierFp, env.ridge, across, ridgeLen);
    // A pitched roof over a footprint wider than a house would stand as a hill: wide buildings have flat roofs.
    if ((env.roof == "gable" || env.roof == "hip" || env.roof == "mansard") && across > kWidestPitched)
        env.roof = "flat";
    if (env.roof == "gable" || env.roof == "hip")
        env.roofHeight = 0.5 * across * std::tan(pitch * kPi / 180.0);
    else if (env.roof == "mansard")
        env.roofHeight = 0.9 * p.floorHeight + 0.25 * across * std::tan(20.0 * kPi / 180.0);
    else if (env.roof == "dome")
        env.roofHeight = std::min(hu, hv);

    std::vector<std::pair<double, Vec2>> doors;
    for (size_t k = 0; k < env.footprint.size(); ++k)
        if (env.footprint.tag[k] == 0)
        {
            const Vec2 a = env.footprint.pts[k], b = env.footprint.pts[(k + 1) % env.footprint.size()];
            if (glm::length(b - a) >= 100.0)
                doors.push_back({glm::length(b - a), (a + b) * 0.5});
        }
    if (doors.empty())
    {
        // A street side shorter than a door after setbacks: the door goes on the side that faces the street most.
        double best = -2.0;
        for (size_t k = 0; k < env.footprint.size(); ++k)
        {
            const Vec2 a = env.footprint.pts[k], b = env.footprint.pts[(k + 1) % env.footprint.size()];
            if (glm::length(b - a) < 1e-6)
                continue;
            const Vec2 out(glm::normalize(b - a).y, -glm::normalize(b - a).x);
            if (const double f = glm::dot(out, env.front); f > best)
            {
                best = f;
                doors = {{glm::length(b - a), (a + b) * 0.5}};
            }
        }
    }
    std::stable_sort(doors.begin(), doors.end(), [](const auto& x, const auto& y) { return x.first > y.first; });
    for (const auto& [len, at] : doors)
        env.entrances.push_back(at);

    env.groundUse = lot.use;
    if (lot.use == Use::Market)
        env.groundUse = Use::Commercial;
    else if (lot.use == Use::Residential && lot.access >= 0.6 && dist < p.commercial && lot.frontClass <= StreetClass::Collector)
        env.groundUse = Use::Commercial;
    env.partyWith.assign(env.footprint.size(), -1);
    return env;
}

void frontageFit(const Envelope& env, double& width, double& depth, Vec2& centre)
{
    const Vec2 along(-env.front.y, env.front.x);
    double a0 = 1e300, a1 = -1e300, d0 = 1e300, d1 = -1e300;
    for (const Vec2& q : env.footprint.pts)
    {
        a0 = std::min(a0, glm::dot(q, along)), a1 = std::max(a1, glm::dot(q, along));
        d0 = std::min(d0, glm::dot(q, env.front)), d1 = std::max(d1, glm::dot(q, env.front));
    }
    width = a1 - a0;
    depth = d1 - d0;
    centre = along * (0.5 * (a0 + a1)) + env.front * (0.5 * (d0 + d1));
}

Placement libraryPlacement(const Envelope& env, const LibraryPick& pick)
{
    double w = 0.0, d = 0.0;
    Vec2 c{0.0};
    frontageFit(env, w, d, c);
    const double turn = (std::atan2(env.front.x, env.front.y) * 180.0 / kPi - pick.assetHeading) * kPi / 180.0;
    Placement p;
    p.id = "building/" + std::to_string(env.key);
    p.asset = pick.unreal.empty() ? pick.asset : pick.unreal;
    p.position = {c.x, env.ground, c.y};
    p.rotation = {0.0, std::sin(0.5 * turn), 0.0, std::cos(0.5 * turn)};
    p.tags = {"building", useName(env.use)};
    if (!env.eraName.empty())
        p.tags.push_back(env.eraName);
    if (!env.landmark.empty())
        p.tags.push_back(env.landmark);
    if (pick.unreal.empty())
        p.tags.push_back("noEngineAsset");
    return p;
}

void matchPartyWalls(std::vector<Envelope>& envs)
{
    LILA_ZONE();

    constexpr double kCell = 5000.0;
    std::map<std::pair<int64_t, int64_t>, std::vector<size_t>> cells;
    std::vector<std::pair<Vec2, Vec2>> boxes(envs.size());
    std::vector<std::vector<double>> shared(envs.size()); // per side, the longest stretch shared so far
    for (size_t i = 0; i < envs.size(); ++i)
    {
        Envelope& e = envs[i];
        e.partyWith.assign(e.footprint.size(), -1);
        for (int& t : e.footprint.tag)
            t = t == 0 ? 0 : 2;
        shared[i].assign(e.footprint.size(), 0.0);
        Vec2 lo(1e300), hi(-1e300);
        for (const Vec2& q : e.footprint.pts)
            lo = glm::min(lo, q), hi = glm::max(hi, q);
        boxes[i] = {lo - Vec2(5.0), hi + Vec2(5.0)};
        for (int64_t x = int64_t(std::floor(boxes[i].first.x / kCell)); x <= int64_t(std::floor(boxes[i].second.x / kCell)); ++x)
            for (int64_t z = int64_t(std::floor(boxes[i].first.y / kCell)); z <= int64_t(std::floor(boxes[i].second.y / kCell)); ++z)
                cells[{x, z}].push_back(i);
    }
    std::vector<std::pair<size_t, size_t>> pairs;
    for (auto& [key, ids] : cells)
        for (size_t a = 0; a < ids.size(); ++a)
            for (size_t b = a + 1; b < ids.size(); ++b)
                pairs.push_back({std::min(ids[a], ids[b]), std::max(ids[a], ids[b])});
    std::sort(pairs.begin(), pairs.end());
    pairs.erase(std::unique(pairs.begin(), pairs.end()), pairs.end());
    for (const auto& [i, j] : pairs)
    {
        if (boxes[i].second.x < boxes[j].first.x || boxes[j].second.x < boxes[i].first.x || boxes[i].second.y < boxes[j].first.y ||
            boxes[j].second.y < boxes[i].first.y)
            continue;
        Polygon& A = envs[i].footprint;
        Polygon& B = envs[j].footprint;
        for (size_t a = 0; a < A.size(); ++a)
        {
            if (A.tag[a] == 0)
                continue;
            const Vec2 pa = A.pts[a], qa = A.pts[(a + 1) % A.size()];
            const double la = glm::length(qa - pa);
            if (la < 1.0)
                continue;
            const Vec2 ua = (qa - pa) / la;
            for (size_t b = 0; b < B.size(); ++b)
            {
                if (B.tag[b] == 0)
                    continue;
                const Vec2 pb = B.pts[b], qb = B.pts[(b + 1) % B.size()];
                const double lb = glm::length(qb - pb);
                if (lb < 1.0 || glm::dot(ua, (qb - pb) / lb) > -0.995 || std::abs(cross(ua, pb - pa)) > 5.0 || std::abs(cross(ua, qb - pa)) > 5.0)
                    continue;
                const double t0 = glm::dot(pb - pa, ua), t1 = glm::dot(qb - pa, ua);
                const double overlap = std::min(la, std::max(t0, t1)) - std::max(0.0, std::min(t0, t1));
                if (overlap < 100.0)
                    continue;
                A.tag[a] = 1;
                B.tag[b] = 1;
                if (overlap > shared[i][a])
                {
                    shared[i][a] = overlap;
                    envs[i].partyWith[a] = envs[j].lot;
                }
                if (overlap > shared[j][b])
                {
                    shared[j][b] = overlap;
                    envs[j].partyWith[b] = envs[i].lot;
                }
            }
        }
    }
}

bool appendMassing(const Envelope& env, const Layout& l, geo::Mesh& into, const std::function<void(int part)>& tagFace)
{
    auto face = [&](const std::vector<geo::Index>& ids, int part) {
        if (into.addFace(std::span<const geo::Index>(ids)) != geo::kInvalid)
            tagFace(part);
    };
    auto ring = [&](const Polygon& fp, double y) {
        std::vector<geo::Index> ids;
        for (const Vec2& q : fp.pts)
            ids.push_back(into.addPoint({q.x, y, q.y}));
        return ids;
    };
    auto cap = [&](const Polygon& fp, double y, int part) {
        std::vector<geo::Index> top = ring(fp, y);
        std::reverse(top.begin(), top.end());
        face(top, part);
    };
    /** A horizontal face with courtyards cut out, as triangles; up faces up, else down. False when it can't be cut. */
    auto holedFace = [&](const Polygon& fp, const std::vector<const Polygon*>& holes, double y, bool up, int part) {
        const std::vector<Vec2> outline = bridgedOutline(fp, holes);
        if (outline.size() < 3)
            return false;
        std::vector<geo::Index> tris;
        geo::earClip(std::span<const glm::dvec2>(outline), tris);
        if (tris.size() != 3 * (outline.size() - 2))
            return false;
        std::vector<geo::Index> ids;
        for (const Vec2& q : outline)
            ids.push_back(into.addPoint({q.x, y, q.y}));
        for (size_t t = 0; t + 2 < tris.size(); t += 3)
        {
            const geo::Index a = ids[tris[t]], b = ids[tris[t + 1]], c = ids[tris[t + 2]];
            face(up ? std::vector<geo::Index>{a, c, b} : std::vector<geo::Index>{a, b, c}, part);
        }
        return true;
    };
    // Courtyards cut through the tiers whose footprint holds them.
    auto holesIn = [&](const Polygon& fp) {
        std::vector<const Polygon*> out;
        for (const Polygon& h : env.holes)
        {
            bool in = h.size() >= 3;
            for (const Vec2& q : h.pts)
                in = in && contains(fp.pts, q);
            if (in)
                out.push_back(&h);
        }
        return out;
    };
    /** Sides between two rings with the same corner count, lo below hi. */
    auto loft = [&](const Polygon& a, double ya, const Polygon& b, double yb, int part) {
        const std::vector<geo::Index> lo = ring(a, ya), hi = ring(b, yb);
        for (size_t k = 0; k < lo.size(); ++k)
        {
            const size_t j = (k + 1) % lo.size();
            face({lo[k], hi[k], hi[j], lo[j]}, part);
        }
    };
    const std::vector<Envelope::Tier>& tiers = env.tiers;
    bool built = true;
    std::vector<std::vector<const Polygon*>> tierHoles;
    for (size_t t = 0; t < tiers.size(); ++t)
    {
        const Envelope::Tier& tier = tiers[t];
        tierHoles.push_back(holesIn(tier.footprint));
        loft(tier.footprint, tier.bottom, tier.footprint, tier.top, 0);
        for (const Polygon* h : tierHoles.back())
        {
            // Courtyard walls face into the courtyard: the hole's ring runs the other way round.
            Polygon inward = *h;
            if ((signedArea(inward.pts) > 0.0) == (signedArea(tier.footprint.pts) > 0.0))
            {
                std::reverse(inward.pts.begin(), inward.pts.end());
                std::reverse(inward.tag.begin(), inward.tag.end());
            }
            loft(inward, tier.bottom, inward, tier.top, 0);
        }
        if (tierHoles.back().empty() || !holedFace(tier.footprint, tierHoles.back(), tier.bottom, false, 2))
        {
            face(ring(tier.footprint, tier.bottom), 2);
            built = built && tierHoles.back().empty();
        }
    }
    if (tiers.empty())
        return true;
    auto skeletonRoof = [&](const Polygon& fp, const std::vector<const Polygon*>& holes, double at, double pitch, bool gables, Vec2 ridge) {
        lila::curve::Splines loops;
        auto loop = [&](const Polygon& p) {
            lila::curve::Spline s;
            s.kind = lila::curve::Kind::Poly;
            s.closed = true;
            for (const Vec2& q : p.pts)
                s.points.push_back({q.x, at, q.y});
            loops.push_back(std::move(s));
        };
        loop(fp);
        for (const Polygon* h : holes)
            loop(*h);
        lila::curve::RoofOptions ro;
        ro.pitch = pitch;
        ro.normal = {0.0, 1.0, 0.0};
        if (gables)
            for (size_t k = 0; k < fp.size(); ++k)
            {
                const Vec2 d = fp.pts[(k + 1) % fp.size()] - fp.pts[k];
                if (glm::length(d) > 1e-6 && std::abs(glm::dot(glm::normalize(d), ridge)) < 0.35)
                    ro.gables.push_back(int(k));
            }
        const lila::curve::RoofResult r = lila::curve::roof(loops, ro);
        if (r.mesh.numFaces() == 0)
            return false;
        std::vector<geo::Index> remap(r.mesh.pointCapacity(), geo::kInvalid);
        for (geo::Index pt = 0; pt < r.mesh.pointCapacity(); ++pt)
            if (r.mesh.pointValid(pt))
                remap[pt] = into.addPoint(r.mesh.position(pt));
        for (geo::Index f = 0; f < r.mesh.faceCapacity(); ++f)
            if (r.mesh.faceValid(f))
            {
                std::vector<geo::Index> ids;
                for (geo::Index pt : r.mesh.facePoints(f))
                    ids.push_back(remap[pt]);
                face(ids, 1);
            }
        return true;
    };
    /** A flat top, courtyards cut out; false when they couldn't be and a solid cap stands in. */
    auto flatTop = [&](const Polygon& top, const std::vector<const Polygon*>& holes, double y) {
        if (!holes.empty() && holedFace(top, holes, y, true, 1))
            return true;
        cap(top, y, 1);
        return holes.empty();
    };
    /** The named roof on a footprint at wall top y; false when a flat cap stood in for it. */
    auto drawRoof = [&](const Polygon& top, const std::vector<const Polygon*>& holes, double y, const std::string& shape, double roofHeight,
                        Vec2 ridge) {
        double across = 0.0, along = 0.0;
        spans(top, ridge, across, along);
        const double pitch = across > 1e-6 ? std::atan(2.0 * roofHeight / across) * 180.0 / kPi : 30.0;
        if (shape == "gable" || shape == "hip")
        {
            if (skeletonRoof(top, holes, y, pitch, shape == "gable", ridge))
                return true;
            flatTop(top, holes, y);
            return false;
        }
        if ((shape == "mansard" || shape == "dome") && !holes.empty())
        {
            // Insets don't carry courtyards: a flat top with them stands in, reported.
            flatTop(top, holes, y);
            return false;
        }
        if (shape == "mansard")
        {
            const Polygon upper = insetBy(top, std::clamp(0.2 * across, 60.0, 250.0));
            const double y1 = y + 0.9 * l.profile.floorHeight;
            if (upper.size() != top.size())
            {
                cap(top, y, 1);
                return false;
            }
            loft(top, y, upper, y1, 1);
            if (skeletonRoof(upper, {}, y1, 20.0, false, ridge))
                return true;
            cap(upper, y1, 1);
            return false;
        }
        if (shape == "dome")
        {
            const double r = std::max(roofHeight, 1.0);
            Polygon prev = top;
            double prevY = y;
            for (int k = 1; k <= 6; ++k)
            {
                const double a = k * kPi / 12.0;
                const Polygon next = insetBy(top, r * (1.0 - std::cos(a)));
                if (next.size() != prev.size() || area(next) < 1.0)
                {
                    cap(prev, prevY, 1);
                    return true;
                }
                loft(prev, prevY, next, y + r * std::sin(a), 1);
                prev = next;
                prevY = y + r * std::sin(a);
            }
            cap(prev, prevY, 1);
            return true;
        }
        return flatTop(top, holes, y);
    };
    // Tiers with a roof of their own (imported building:parts) draw it; the others are capped, the last with the envelope's roof.
    for (size_t t = 0; t < tiers.size(); ++t)
    {
        const Envelope::Tier& tier = tiers[t];
        const bool last = t + 1 == tiers.size();
        if (!tier.roof.empty())
        {
            Vec2 c{0.0}, u{1.0, 0.0}, v{0.0, 1.0};
            double hu = 0.0, hv = 0.0;
            boxOf(tier.footprint, c, u, v, hu, hv);
            built = drawRoof(tier.footprint, tierHoles[t], tier.top, tier.roof, tier.roofHeight, u) && built;
        }
        else if (last)
            built = drawRoof(tier.footprint, tierHoles[t], tier.top, env.roof, env.roofHeight, env.ridge) && built;
        else
            built = flatTop(tier.footprint, tierHoles[t], tier.top) && built;
    }
    return built;
}

json blockJson(const Block& b)
{
    json yards = json::array();
    for (const Polygon& y : b.courtyards)
        yards.push_back(ringOf(y));
    return {{"blockId", b.key},
            {"type", blockTypeName(b.type)},
            {"method", lotMethodName(b.method)},
            {"use", useName(b.use)},
            {"areaM2", b.area},
            {"water", b.water},
            {"center", xzOf(centroid(b.outline))},
            {"courtyards", yards},
            {"reasons", b.reasons}};
}

json businessesJson(const std::vector<Business>& list)
{
    json out = json::array();
    for (const Business& b : list)
        out.push_back({{"name", b.name}, {"program", b.program}, {"osmId", b.osmId}, {"at", xzOf(b.at)}});
    return out;
}

json lotExtras(const Lot& lot)
{
    static const char* kWealth[] = {"low", "middle", "high"};
    json j = {{"lotId", lot.key},
              {"wealth", kWealth[std::clamp(lot.wealth, 0, 2)]},
              {"access", lot.access},
              {"landmark", lot.landmark},
              {"reasons", lot.reasons},
              {"route", routeName(lot.route)},
              {"name", lot.name},
              {"program", lot.program}};
    if (!lot.osmId.empty())
        j["osmId"] = lot.osmId;
    if (!lot.businesses.empty())
        j["businesses"] = businessesJson(lot.businesses);
    return j;
}

json envelopeExtras(const Envelope& env)
{
    json sides = json::array(), tiers = json::array(), doors = json::array();
    for (int t : env.footprint.tag)
        sides.push_back(sideName2(t));
    for (const Envelope::Tier& t : env.tiers)
    {
        tiers.push_back({{"footprint", ringOf(t.footprint)}, {"bottom", t.bottom}, {"top", t.top}});
        if (!t.roof.empty())
            tiers.back().update({{"roof", t.roof}, {"roofHeight", t.roofHeight}});
    }
    for (const Vec2& d : env.entrances)
        doors.push_back(xzOf(d));
    json j = {{"lotId", env.key},
            {"sides", sides},
            {"partyWith", env.partyWith},
            {"frontSetback", env.frontSetback},
            {"sideSetback", env.sideSetback},
            {"tiers", tiers},
            {"ridge", xzOf(env.ridge)},
            {"roofHeight", env.roofHeight},
            {"entrances", doors},
            {"groundUse", useName(env.groundUse)},
            {"corner", env.corner},
            {"foundationDepth", env.foundationDepth},
            {"foundationSource", env.foundationSource},
            {"landmark", env.landmark},
            {"eraName", env.eraName},
            {"wealth", env.wealth},
            {"route", routeName(env.route)},
            {"name", env.name},
            {"program", env.program},
            {"style", env.style},
            {"styleFrom", env.styleFrom},
            {"businesses", businessesJson(env.businesses)}};
    if (!env.osmId.empty())
    {
        json holes = json::array();
        for (const Polygon& h : env.holes)
            holes.push_back(ringOf(h));
        j.update({{"osmId", env.osmId},
                  {"heightSource", env.heightSource},
                  {"roofSource", env.roofSource},
                  {"roofShape", env.roofShape},
                  {"colour", env.colour},
                  {"roofColour", env.roofColour},
                  {"material", env.material},
                  {"roofMaterial", env.roofMaterial},
                  {"holes", holes}});
    }
    return j;
}

json parcelStats(const Layout& l)
{
    json types = json::object(), methods = json::object(), uses = json::object(), wealth = json::object(), roofs = json::object();
    for (const Block& b : l.blocks)
    {
        types[blockTypeName(b.type)] = types.value(blockTypeName(b.type), 0) + 1;
        methods[lotMethodName(b.method)] = methods.value(lotMethodName(b.method), 0) + 1;
    }
    std::vector<double> areas;
    json landmarks = json::array();
    int landlocked = 0;
    for (const Lot& lot : l.lots)
    {
        uses[useName(lot.use)] = uses.value(useName(lot.use), 0) + 1;
        if (lot.use == Use::Residential)
        {
            const char* w = lot.wealth <= 0 ? "low" : lot.wealth == 1 ? "middle" : "high";
            wealth[w] = wealth.value(w, 0) + 1;
        }
        if (lot.frontages == 0)
            ++landlocked;
        if (!lot.landmark.empty())
            landmarks.push_back({{"name", lot.landmark}, {"lot", lot.id}, {"lotId", lot.key}, {"use", useName(lot.use)},
                                 {"center", xzOf(centroid(lot.outline))}, {"reasons", lot.reasons}});
        else if (lot.use != Use::Open && lot.use != Use::Farm)
            areas.push_back(lot.area);
    }
    int party = 0;
    for (const Envelope& env : l.envelopes)
    {
        roofs[env.roof] = roofs.value(env.roof, 0) + 1;
        party += int(std::ranges::count(env.footprint.tag, 1));
    }
    std::sort(areas.begin(), areas.end());
    auto q = [&](double f) { return areas.empty() ? 0.0 : areas[std::min(areas.size() - 1, size_t(f * double(areas.size())))]; };
    const double mean = areas.empty() ? 0.0 : std::accumulate(areas.begin(), areas.end(), 0.0) / double(areas.size());
    return {{"blockTypes", types},
            {"lotMethods", methods},
            {"lotUses", uses},
            {"residentialWealth", wealth},
            {"lotAreaM2", {{"count", areas.size()}, {"mean", mean}, {"p10", q(0.1)}, {"p50", q(0.5)}, {"p90", q(0.9)}, {"max", areas.empty() ? 0.0 : areas.back()}}},
            {"landlockedLots", landlocked},
            {"landmarks", landmarks},
            {"roofs", roofs},
            {"partyWallSides", party},
            {"warnings", l.parcelWarnings}};
}
void envelopeGround(const Polygon& footprint, const Ground& g, double& lo, double& hi) { groundUnder(footprint, g, lo, hi); }
} // namespace settlement
