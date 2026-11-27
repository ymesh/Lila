// The Site Kit geometry node: a site's layout on the ground it stands on. Envelopes become greybox massing, every other
// part an instance published as a typed slot, so the site stays live as its spec or the terrain under it changes.

#include "SitesNode.h"

#include <lila/Extension.h>
#include <lila/GeometryNodes.h>
#include <lila/Graph.h>
#include <lila/Mesh.h>
#include <lila/Profile.h>
#include <lila/Slots.h>

#include <glm/gtc/matrix_transform.hpp>

#include <algorithm>
#include <cmath>
#include <map>
#include <mutex>
#include <numbers>

namespace openworld::sites
{
namespace
{
using lila::geo::Index;

std::mutex& evalLock()
{
    static std::mutex m;
    return m;
}

std::map<uint64_t, Evaluated>& evaluated()
{
    static std::map<uint64_t, Evaluated> m;
    return m;
}

// Ground

/**
 * Heights over a mesh seen from above: its triangles bucketed on a grid, the highest hit wins. The mesh is shared, not
 * copied (a terrain's is millions of triangles); a ground that is only moved (no turn or scale) is read through its
 * offset, anything else is read from a world-space copy.
 */
class GroundGrid
{
public:
    GroundGrid(std::shared_ptr<const lila::geo::Mesh> m, const glm::dmat4& xf)
    {
        const bool moveOnly = glm::length(glm::dvec3(xf[0]) - glm::dvec3(1, 0, 0)) < 1e-9 && glm::length(glm::dvec3(xf[1]) - glm::dvec3(0, 1, 0)) < 1e-9 &&
                              glm::length(glm::dvec3(xf[2]) - glm::dvec3(0, 0, 1)) < 1e-9;
        if (moveOnly)
        {
            mMesh = std::move(m);
            mOffset = glm::dvec3(xf[3]);
        }
        else
        {
            lila::geo::Mesh copy = *m;
            for (Index v = 0; v < copy.pointCapacity(); ++v)
                if (copy.pointValid(v))
                    copy.setPosition(v, glm::dvec3(xf * glm::dvec4(copy.position(v), 1.0)));
            mMesh = std::make_shared<const lila::geo::Mesh>(std::move(copy));
        }
        const lila::geo::Mesh& mesh = *mMesh;
        for (Index f = 0; f < mesh.faceCapacity(); ++f)
        {
            if (!mesh.faceValid(f))
                continue;
            const std::vector<Index> fp = mesh.facePoints(f);
            for (size_t k = 1; k + 1 < fp.size(); ++k)
                mTri.insert(mTri.end(), {uint32_t(fp[0]), uint32_t(fp[k]), uint32_t(fp[k + 1])});
        }
        if (mTri.empty())
            return;
        mLo = glm::dvec2(std::numeric_limits<double>::infinity());
        glm::dvec2 hi(-std::numeric_limits<double>::infinity());
        for (uint32_t i : mTri)
        {
            const glm::dvec3 p = at(i);
            mLo = glm::min(mLo, glm::dvec2(p.x, p.z));
            hi = glm::max(hi, glm::dvec2(p.x, p.z));
        }
        const size_t tris = mTri.size() / 3;
        const glm::dvec2 ext = glm::max(hi - mLo, glm::dvec2(1.0));
        mCell = std::max(1.0, std::sqrt(ext.x * ext.y / std::max<size_t>(1, tris / 2)));
        mNx = std::max(1, int(ext.x / mCell) + 1);
        mNz = std::max(1, int(ext.y / mCell) + 1);
        // Two passes, count then fill one flat array, so a terrain's millions of buckets are not a vector each.
        mStart.assign(size_t(mNx) * size_t(mNz) + 1, 0);
        auto cells = [&](uint32_t t, auto&& visit) {
            const glm::dvec3 a = at(mTri[t * 3]), b = at(mTri[t * 3 + 1]), c = at(mTri[t * 3 + 2]);
            const int x0 = cellX(std::min({a.x, b.x, c.x})), x1 = cellX(std::max({a.x, b.x, c.x}));
            const int z0 = cellZ(std::min({a.z, b.z, c.z})), z1 = cellZ(std::max({a.z, b.z, c.z}));
            for (int z = z0; z <= z1; ++z)
                for (int x = x0; x <= x1; ++x)
                    visit(size_t(z) * size_t(mNx) + size_t(x));
        };
        for (uint32_t t = 0; t < tris; ++t)
            cells(t, [&](size_t cell) { ++mStart[cell + 1]; });
        for (size_t k = 1; k < mStart.size(); ++k)
            mStart[k] += mStart[k - 1];
        mItems.resize(mStart.back());
        std::vector<uint32_t> fill(mStart.begin(), mStart.end() - 1);
        for (uint32_t t = 0; t < tris; ++t)
            cells(t, [&](size_t cell) { mItems[fill[cell]++] = t; });
    }

    bool empty() const { return mTri.empty(); }

    bool height(double x, double z, double* y) const
    {
        if (mTri.empty())
            return false;
        const int cx = int(std::floor((x - mLo.x) / mCell)), cz = int(std::floor((z - mLo.y) / mCell));
        if (cx < 0 || cz < 0 || cx >= mNx || cz >= mNz)
            return false;
        bool hit = false;
        const size_t cell = size_t(cz) * size_t(mNx) + size_t(cx);
        for (uint32_t k = mStart[cell]; k < mStart[cell + 1]; ++k)
        {
            const uint32_t t = mItems[k];
            const glm::dvec3 a = at(mTri[t * 3]), b = at(mTri[t * 3 + 1]), c = at(mTri[t * 3 + 2]);
            const double d = (b.z - c.z) * (a.x - c.x) + (c.x - b.x) * (a.z - c.z);
            if (std::abs(d) < 1e-12)
                continue;
            const double u = ((b.z - c.z) * (x - c.x) + (c.x - b.x) * (z - c.z)) / d;
            const double v = ((c.z - a.z) * (x - c.x) + (a.x - c.x) * (z - c.z)) / d;
            const double w = 1.0 - u - v;
            if (u < -1e-9 || v < -1e-9 || w < -1e-9)
                continue;
            const double h = u * a.y + v * b.y + w * c.y;
            if (!hit || h > *y)
                *y = h;
            hit = true;
        }
        return hit;
    }

private:
    glm::dvec3 at(uint32_t i) const { return mMesh->position(Index(i)) + mOffset; }
    int cellX(double x) const { return std::clamp(int(std::floor((x - mLo.x) / mCell)), 0, mNx - 1); }
    int cellZ(double z) const { return std::clamp(int(std::floor((z - mLo.y) / mCell)), 0, mNz - 1); }

    std::shared_ptr<const lila::geo::Mesh> mMesh;
    glm::dvec3 mOffset{0.0};
    std::vector<uint32_t> mTri;
    glm::dvec2 mLo{0.0};
    double mCell = 1.0;
    int mNx = 1, mNz = 1;
    std::vector<uint32_t> mStart, mItems;
};

std::mutex gGroundLock;
std::map<uint64_t, std::shared_ptr<const GroundGrid>> gGrounds;

/**
 * The grid for a ground mesh and its placement, rebuilt only when either changes. Keyed by a sample of the points
 * (a terrain has millions): the counts, every 97th position and the placement.
 */
std::shared_ptr<const GroundGrid> groundFor(const std::shared_ptr<const lila::geo::Mesh>& m, const glm::dmat4& xf)
{
    uint64_t h = 1469598103934665603ull ^ uint64_t(m->numFaces()) ^ (uint64_t(m->numPoints()) << 32);
    auto mix = [&](double v) {
        uint64_t bits;
        std::memcpy(&bits, &v, sizeof bits);
        h = (h ^ bits) * 1099511628211ull;
    };
    const auto pts = m->positions();
    for (size_t i = 0; i < pts.size(); i += 97)
        mix(pts[i].x), mix(pts[i].y), mix(pts[i].z);
    if (!pts.empty())
        mix(pts.back().y);
    for (int c = 0; c < 4; ++c)
        for (int r = 0; r < 4; ++r)
            mix(xf[c][r]);
    std::lock_guard l(gGroundLock);
    if (auto it = gGrounds.find(h); it != gGrounds.end())
        return it->second;
    // Two: the ground before and after an edit. A changed terrain's older grids are not asked for again.
    if (gGrounds.size() >= 2)
        gGrounds.clear();
    auto g = std::make_shared<const GroundGrid>(m, xf);
    gGrounds[h] = g;
    return g;
}

// Meshes

struct Soup
{
    std::vector<glm::dvec3> p;
    std::vector<Index> sizes, idx;

    Index add(const glm::dvec3& q)
    {
        p.push_back(q);
        return Index(p.size() - 1);
    }

    /** A face, turned to face want. */
    void face(std::vector<Index> f, const glm::dvec3& want)
    {
        glm::dvec3 n(0.0);
        for (size_t i = 0; i < f.size(); ++i)
        {
            const glm::dvec3 &a = p[f[i]], &b = p[f[(i + 1) % f.size()]];
            n += glm::dvec3((a.y - b.y) * (a.z + b.z), (a.z - b.z) * (a.x + b.x), (a.x - b.x) * (a.y + b.y));
        }
        if (glm::dot(n, want) < 0.0)
            std::reverse(f.begin(), f.end());
        sizes.push_back(Index(f.size()));
        idx.insert(idx.end(), f.begin(), f.end());
    }

    void box(const glm::dvec3& lo, const glm::dvec3& hi)
    {
        const glm::dvec3 c = (lo + hi) * 0.5;
        Index v[8];
        for (int k = 0; k < 8; ++k)
            v[k] = add({k & 1 ? hi.x : lo.x, k & 2 ? hi.y : lo.y, k & 4 ? hi.z : lo.z});
        const int quads[6][4] = {{0, 2, 3, 1}, {4, 5, 7, 6}, {0, 4, 6, 2}, {1, 3, 7, 5}, {0, 1, 5, 4}, {2, 6, 7, 3}};
        for (const auto& q : quads)
        {
            const glm::dvec3 m = (p[v[q[0]]] + p[v[q[1]]] + p[v[q[2]]] + p[v[q[3]]]) * 0.25;
            face({v[q[0]], v[q[1]], v[q[2]], v[q[3]]}, m - c);
        }
    }

    /** A prism over a ring (x, z) from y0 to y1, capped; returns the top ring's point indices. */
    std::vector<Index> prism(const std::vector<glm::dvec2>& ring, double y0, double y1, bool topCap = true)
    {
        glm::dvec2 c(0.0);
        for (const glm::dvec2& q : ring)
            c += q;
        c /= double(std::max<size_t>(1, ring.size()));
        std::vector<Index> bot, top;
        for (const glm::dvec2& q : ring)
            bot.push_back(add({q.x, y0, q.y}));
        for (const glm::dvec2& q : ring)
            top.push_back(add({q.x, y1, q.y}));
        face(bot, {0.0, -1.0, 0.0});
        if (topCap)
            face(top, {0.0, 1.0, 0.0});
        for (size_t i = 0; i < ring.size(); ++i)
        {
            const size_t j = (i + 1) % ring.size();
            const glm::dvec2 m = (ring[i] + ring[j]) * 0.5 - c;
            face({bot[i], bot[j], top[j], top[i]}, {m.x, 0.0, m.y});
        }
        return top;
    }

    void cylinder(double r, double y0, double y1, int n = 12, glm::dvec2 at = glm::dvec2(0.0))
    {
        std::vector<glm::dvec2> ring;
        for (int k = 0; k < n; ++k)
        {
            const double a = 2.0 * std::numbers::pi * k / n;
            ring.push_back(at + glm::dvec2(std::cos(a), std::sin(a)) * r);
        }
        prism(ring, y0, y1);
    }

    lila::geo::Mesh mesh() const { return lila::geo::Mesh::fromPolygons(p, sizes, idx); }
};

/** A roof over a top ring at y: gable or hip along the long side of four corners, a dome, else a pyramid. */
void roof(Soup& s, const std::vector<glm::dvec2>& ring, const std::vector<Index>& top, double y, const std::string& kind, double rh)
{
    glm::dvec2 c(0.0);
    for (const glm::dvec2& q : ring)
        c += q;
    c /= double(ring.size());
    const glm::dvec3 c3(c.x, y, c.y);
    auto want = [&](const std::vector<Index>& f) {
        glm::dvec3 m(0.0);
        for (Index i : f)
            m += s.p[i];
        m /= double(f.size());
        return glm::dvec3(m.x - c3.x, 0.0, m.z - c3.z) + glm::dvec3(0.0, 1.0, 0.0) * 50.0;
    };
    if (kind == "flat" || rh <= 0.0)
    {
        s.face(top, {0.0, 1.0, 0.0});
        return;
    }
    if (kind == "dome")
    {
        constexpr int rings = 4;
        std::vector<Index> prev = top;
        for (int k = 1; k < rings; ++k)
        {
            const double a = 0.5 * std::numbers::pi * k / rings, f = std::cos(a), h = std::sin(a) * rh;
            std::vector<Index> next;
            for (const glm::dvec2& q : ring)
                next.push_back(s.add({c.x + (q.x - c.x) * f, y + h, c.y + (q.y - c.y) * f}));
            for (size_t i = 0; i < ring.size(); ++i)
            {
                const size_t j = (i + 1) % ring.size();
                const std::vector<Index> quad = {prev[i], prev[j], next[j], next[i]};
                s.face(quad, want(quad));
            }
            prev = std::move(next);
        }
        const Index apex = s.add({c.x, y + rh, c.y});
        for (size_t i = 0; i < ring.size(); ++i)
        {
            const std::vector<Index> tri = {prev[i], prev[(i + 1) % ring.size()], apex};
            s.face(tri, want(tri));
        }
        return;
    }
    if (ring.size() == 4)
    {
        // Ridge along the longer pair of sides.
        const bool along01 = glm::length(ring[1] - ring[0]) >= glm::length(ring[2] - ring[1]);
        const int a0 = along01 ? 0 : 1, a1 = (a0 + 1) % 4, b0 = (a0 + 2) % 4, b1 = (a0 + 3) % 4;
        glm::dvec2 ra = (ring[b1] + ring[a0]) * 0.5, rb = (ring[a1] + ring[b0]) * 0.5;
        if (kind != "gable")
        {
            const double inset = std::min(glm::length(ring[b0] - ring[a1]) * 0.5, glm::length(rb - ra) * 0.45);
            const glm::dvec2 d = glm::normalize(rb - ra);
            ra += d * inset;
            rb -= d * inset;
        }
        const Index A = s.add({ra.x, y + rh, ra.y}), B = s.add({rb.x, y + rh, rb.y});
        const std::vector<std::vector<Index>> faces = {{top[a0], top[a1], B, A}, {top[b0], top[b1], A, B}, {top[a1], top[b0], B}, {top[b1], top[a0], A}};
        for (const auto& f : faces)
            s.face(f, want(f));
        return;
    }
    const Index apex = s.add({c.x, y + rh, c.y});
    for (size_t i = 0; i < ring.size(); ++i)
    {
        const std::vector<Index> tri = {top[i], top[(i + 1) % ring.size()], apex};
        s.face(tri, want(tri));
    }
}

/** The placeholder a site kit slot type draws with: shaped for the kits' own big parts, else W0's placeholder for the type. */
lila::gn::GeometryPtr placeholderFor(const std::string& type)
{
    static std::mutex lock;
    static std::map<std::string, lila::gn::GeometryPtr> cache;
    {
        std::lock_guard l(lock);
        if (auto it = cache.find(type); it != cache.end())
            return it->second;
    }
    Soup s;
    auto is = [&](const char* t) { return type == t || type.starts_with(std::string(t) + "."); };
    if (is("crane.sts"))
    {
        for (double x : {-1200.0, 1200.0})
            for (double z : {-1300.0, 1300.0})
                s.box({x - 90, 0, z - 90}, {x + 90, 5200, z + 90});
        s.box({-1300, 5200, -4500}, {1300, 5600, 9000});
        s.box({-700, 5600, -4300}, {700, 6600, -2300});
        s.box({-1300, 600, -1400}, {1300, 900, 1400});
    }
    else if (is("crane.rtg"))
    {
        for (double x : {-550.0, 550.0})
        {
            for (double z : {-1250.0, 1250.0})
                s.box({x - 60, 0, z - 60}, {x + 60, 2000, z + 60});
            s.box({x - 80, 1900, -1300}, {x + 80, 2100, 1300});
        }
        s.box({-600, 1950, -200}, {600, 2150, 200});
    }
    else if (is("light.mast"))
    {
        s.cylinder(25, 0, 3000, 8);
        s.box({-120, 2950, -60}, {120, 3080, 60});
    }
    else if (is("greenhouse.frame"))
    {
        const std::vector<glm::dvec2> ring = {{-480, -1250}, {480, -1250}, {480, 1250}, {-480, 1250}};
        const std::vector<Index> top = s.prism(ring, 0, 330, false);
        roof(s, ring, top, 330, "gable", 120);
    }
    else if (is("shadehouse.frame"))
    {
        for (double x : {-470.0, 470.0})
            for (double z : {-1240.0, 1240.0})
                s.box({x - 8, 0, z - 8}, {x + 8, 400, z + 8});
        s.box({-480, 395, -1250}, {480, 400, 1250});
    }
    else if (is("windsock"))
    {
        s.cylinder(8, 0, 600, 6);
        s.box({0, 540, -25}, {160, 590, 25});
    }
    else if (is("ship.cargo"))
    {
        s.box({-1600, -600, -12500}, {1600, 800, 12500});
        s.box({-1300, 800, -12000}, {1300, 1600, 8000});
        s.box({-1200, 800, -12300}, {1200, 2400, -10000});
    }
    else if (is("ship.cruise"))
    {
        s.box({-1800, -700, -14500}, {1800, 900, 14500});
        s.box({-1550, 900, -12500}, {1550, 3600, 11000});
        s.box({-600, 3600, -2000}, {600, 4300, 1000});
    }
    else if (is("pool.swimming"))
        s.box({-500, -150, -250}, {500, -10, 250});
    else if (is("helipad"))
        s.cylinder(1000, 0, 12, 24);
    else if (is("drum.fuel"))
        s.cylinder(30, 0, 90, 10);
    else if (is("lighthouse.lantern"))
    {
        s.cylinder(150, 0, 300, 8);
        s.cylinder(170, 300, 340, 8);
    }
    else if (is("reefer.rack"))
    {
        for (double x : {-640.0, 640.0})
            s.box({x - 15, 0, -15}, {x + 15, 800, 15});
        s.box({-650, 760, -150}, {650, 800, 150});
    }
    else if (is("dock.pier") || is("dock.floating"))
    {
        const lila::slots::SlotType* t = lila::slots::resolveType(type);
        const glm::dvec3 f = t ? t->footprint : glm::dvec3(500, 100, 300);
        s.box({-f.x * 0.5, 0, -f.z * 0.5}, {f.x * 0.5, f.y, f.z * 0.5});
        if (is("dock.pier"))
            for (double x : {-f.x * 0.3, f.x * 0.3})
                for (double z : {-f.z * 0.4, f.z * 0.4})
                    s.box({x - 15, -600, z - 15}, {x + 15, 0, z + 15});
    }
    lila::gn::GeometryPtr g;
    if (!s.p.empty())
        g = lila::gn::meshGeometry(s.mesh());
    else
    {
        const lila::slots::SlotType* t = lila::slots::resolveType(type);
        auto m = lila::slots::placeholderMesh(t ? t->placeholder : std::string("box"), t ? t->footprint : glm::dvec3(100.0));
        lila::gn::Geometry geo;
        geo.mesh = m;
        g = lila::gn::makeGeometry(std::move(geo));
    }
    std::lock_guard l(lock);
    cache[type] = g;
    return g;
}

lila::gn::NodeDecl siteDecl()
{
    using Socket = lila::gn::SocketType;
    return {
        .type = "SiteKit",
        .label = "Site Kit",
        .category = "Open World",
        .doc = "A site kit laid out on the ground: a cargo port, cruise terminal, cemetery, airstrip, farm, village, compound, private island "
               "or lighthouse from its spec (siteCreate writes it). Buildings come out as greybox massing of their envelopes (siteEnvelopes "
               "lists them for a building generator); every other part is an instance published as a typed slot with a stable id under "
               "the site's owner id, so slot rules and overrides swap them. Ground is the terrain the parts stand on.",
        .sockets = {.inputs = {{.id = "spec", .type = Socket::String, .defaultValue = "{}", .doc = "The site spec, JSON.", .editor = "multiline"},
                               {.id = "ground", .type = Socket::Geometry, .doc = "The terrain, world space; none stands the site at height 0."},
                               {.id = "massing", .type = Socket::Bool, .defaultValue = true, .doc = "Draw the buildings' envelopes as greybox massing."}},
                    .outputs = {{.id = "geometry", .type = Socket::Geometry}}},
        .exec =
            [](lila::gn::ExecContext& c) {
                LILA_ZONE();
                Spec spec;
                std::shared_ptr<const Layout> L;
                try
                {
                    spec = specFromJson(json::parse(c.s("spec")));
                    L = layoutFor(spec);
                }
                catch (const std::exception& e)
                {
                    throw lila::gn::ExecError(c.node.name + ": " + e.what());
                }

                std::shared_ptr<const GroundGrid> grid;
                if (lila::gn::GeometryPtr g = c.geometry("ground"))
                {
                    // Object Info hands the terrain as one shared instance (no copy), or as a mesh in world space.
                    const auto& inst = g->instances;
                    if (inst && inst->size() == 1 && !inst->references.empty() && inst->references[0] && inst->references[0]->mesh &&
                        !inst->references[0]->mesh->empty())
                        grid = groundFor(inst->references[0]->mesh, inst->transforms[0]);
                    else if (g->mesh && !g->mesh->empty())
                        grid = groundFor(g->mesh, glm::dmat4(1.0));
                }
                auto groundAt = [&](P2 w) {
                    double y = 0.0;
                    if (grid && grid->height(w.x, w.y, &y))
                        return y;
                    return 0.0;
                };

                Evaluated ev;
                ev.spec = spec;
                ev.layout = L;
                ev.hasGround = grid && !grid->empty();
                ev.originHeight = groundAt(spec.at);
                const double h = spec.heading * std::numbers::pi / 180.0;

                // Slots as instances, one reference per type.
                auto inst = std::make_shared<lila::gn::Instances>();
                std::map<std::string, int> refOf;
                std::vector<lila::slots::Slot> slots;
                slots.reserve(L->slots.size());
                const std::string owner = ownerOf(spec.id);
                const std::vector<std::string> regions = {"site:" + spec.id, "siteKind:" + spec.kind};
                for (const SlotOut& s : L->slots)
                {
                    const P2 w = toWorld(spec, s.at);
                    const double base = s.level == Level::Water ? spec.waterLevel : s.level == Level::Site ? ev.originHeight : groundAt(w);
                    glm::dmat4 m = glm::translate(glm::dmat4(1.0), glm::dvec3(w.x, base + s.y, w.y));
                    m = glm::rotate(m, h + s.heading, glm::dvec3(0.0, 1.0, 0.0));
                    m = glm::scale(m, s.scale);
                    auto [it, fresh] = refOf.try_emplace(s.type, int(inst->references.size()));
                    if (fresh)
                        inst->references.push_back(placeholderFor(s.type));
                    inst->add(m, it->second);
                    lila::slots::Slot sl;
                    sl.id = lila::slots::makeId(owner, s.path);
                    sl.type = s.type;
                    sl.owner = owner;
                    sl.regions = regions;
                    sl.tags = s.tags;
                    sl.tags.push_back("site");
                    sl.tags.push_back(spec.kind);
                    sl.payload.data = s.data;
                    slots.push_back(std::move(sl));
                }
                inst->attrs.resize(inst->size());
                lila::gn::Geometry ig;
                ig.instances = std::move(inst);
                lila::gn::GeometryPtr instances = lila::slots::applySlots(lila::gn::makeGeometry(std::move(ig)), lila::slots::NodeId(c.env.owner), std::move(slots));

                // Envelopes as greybox massing on the lowest ground under each.
                Soup massing;
                for (const EnvelopeOut& e : L->envelopes)
                {
                    std::vector<glm::dvec2> ring;
                    double lo = std::numeric_limits<double>::infinity(), hi = -lo;
                    for (P2 q : e.footprint)
                    {
                        ring.push_back(toWorld(spec, q));
                        const double y = groundAt(ring.back());
                        lo = std::min(lo, y);
                        hi = std::max(hi, y);
                    }
                    const double yc = groundAt(toWorld(spec, centroidOf(e.footprint)));
                    lo = std::min(lo, yc);
                    hi = std::max(hi, yc);
                    ev.envelopeGround.push_back({lo, hi});
                    if (!c.b("massing") || ring.size() < 3)
                        continue;
                    const std::vector<Index> top = massing.prism(ring, lo, hi + e.height, false);
                    roof(massing, ring, top, hi + e.height, e.roof, e.roofHeight);
                }
                std::vector<lila::gn::GeometryPtr> parts = {instances};
                if (!massing.p.empty())
                    parts.push_back(lila::gn::meshGeometry(massing.mesh()));
                if (c.env.owner)
                {
                    std::lock_guard l(evalLock());
                    evaluated()[c.env.owner] = std::move(ev);
                }
                c.out("geometry", lila::gn::makeGeometry(lila::gn::join(parts)));
            },
    };
}
} // namespace

bool evaluatedSite(uint64_t modifier, Evaluated* out)
{
    std::lock_guard l(evalLock());
    const auto it = evaluated().find(modifier);
    if (it == evaluated().end())
        return false;
    *out = it->second;
    return true;
}

void forgetSite(uint64_t modifier)
{
    std::lock_guard l(evalLock());
    evaluated().erase(modifier);
}

std::shared_ptr<const Layout> layoutFor(const Spec& s)
{
    static std::mutex lock;
    static std::map<std::string, std::shared_ptr<const Layout>> cache;
    const std::string key = s.toJson().dump() + "#" + std::to_string(avoidRevision());
    {
        std::lock_guard l(lock);
        if (auto it = cache.find(key); it != cache.end())
            return it->second;
    }
    auto L = std::make_shared<const Layout>(build(s));
    std::lock_guard l(lock);
    if (cache.size() > 32)
        cache.clear();
    cache[key] = L;
    return L;
}

json envelopeJson(const Evaluated& ev, size_t i)
{
    const EnvelopeOut& e = ev.layout->envelopes[i];
    const auto [lo, hi] = i < ev.envelopeGround.size() ? ev.envelopeGround[i] : std::pair<double, double>{0.0, 0.0};
    json fp = json::array(), doors = json::array();
    for (P2 q : e.footprint)
        fp.push_back(pt(toWorld(ev.spec, q)));
    for (P2 q : e.entrances)
        doors.push_back(pt(toWorld(ev.spec, q)));
    const std::string id = lila::slots::makeId(ownerOf(ev.spec.id), e.path);
    json out = {{"id", id},
                {"key", lila::slots::hex(lila::slots::idHash(id))},
                {"site", ev.spec.id},
                {"siteKind", ev.spec.kind},
                {"footprint", fp},
                {"base", lo},
                {"ground", lo},
                {"top", hi + e.height},
                {"height", e.height},
                {"floors", e.floors},
                {"use", e.use},
                {"roof", e.roof},
                {"roofHeight", e.roofHeight},
                {"front", pt(dirToWorld(ev.spec, e.front))},
                {"entrances", doors},
                {"program", e.program},
                {"style", e.style},
                {"styleFrom", "siteKit"},
                {"name", e.name},
                {"route", "buildings"}};
    for (const auto& [k, v] : e.extra.items())
        if (!out.contains(k))
            out[k] = v;
    return out;
}

void forgetGrounds()
{
    std::lock_guard l(gGroundLock);
    gGrounds.clear();
}

void forgetGoneSites()
{
    const lila::scene::Graph& g = lila::scene::Graph::current();
    std::lock_guard l(evalLock());
    std::erase_if(evaluated(), [&](const auto& e) { return !g.node(lila::scene::NodeId(e.first)); });
}

bool registerSiteNode(lila::ext::Context& ctx) { return lila::gn::registerDecl(ctx, siteDecl()) != nullptr; }
} // namespace openworld::sites
