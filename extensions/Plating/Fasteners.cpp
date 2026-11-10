#include "Fasteners.h"

#include <lila/MeshOps.h>
#include <lila/Profile.h>

#include <algorithm>
#include <cmath>
#include <unordered_map>

namespace plating
{
using namespace lila;
using geo::Index;

namespace
{
glm::dvec3 sideOf(const glm::dvec3& normal, const glm::dvec3& tangent)
{
    const glm::dvec3 s = glm::cross(normal, tangent);
    const double len = glm::length(s);
    return len > 1e-12 ? s / len : glm::dvec3(0.0);
}

bool inSet(const layout::Resolved& r, const layout::ResolvedRun& run, const std::string& set)
{
    if (set.empty())
        return true;
    const layout::LineSet* s = r.layout().set(run.set);
    return s && s->name == set;
}

/** Spots a fastener would land in a gap or on another line. */
bool blocked(const layout::Resolved& r, const glm::dvec3& p, double clearance, int ownRun)
{
    for (const layout::LineSample& l : r.linesNear(p, clearance))
        if (l.run != ownRun)
            return true;
    return false;
}

void scaleY(geo::Mesh& m, double s, double lift = 0.0)
{
    for (Index p = 0; p < m.pointCapacity(); ++p)
        if (m.pointValid(p))
        {
            glm::dvec3 q = m.position(p);
            q.y = q.y * s + lift;
            m.setPosition(p, q);
        }
}
} // namespace

std::vector<FastenerSpot> fastenerSpots(const layout::Resolved& r, const FastenerOptions& o)
{
    std::vector<FastenerSpot> out;
    const double spacing = std::max(o.spacing, o.size * 0.5);
    const double clearance = std::max(o.size * 0.5, o.inset * 0.6);
    if (o.placement != Placement::Corners)
    {
        for (size_t ri = 0; ri < r.runs().size(); ++ri)
        {
            const layout::ResolvedRun& run = r.runs()[ri];
            if (!inSet(r, run, o.set) || run.path.empty())
                continue;
            const double len = run.path.length();
            const int count = std::max(1, int(std::floor(len / spacing)));
            const double first = (len - (count - 1) * spacing) * 0.5;
            for (int k = 0; k < count; ++k)
            {
                const double s = std::clamp(first + k * spacing, 0.0, len);
                const glm::dvec3 p = run.path.pointAt(s), n = run.path.normalAt(s), t = run.path.tangentAt(s);
                if (o.placement == Placement::OnLines)
                {
                    if (!blocked(r, p, clearance, int(ri)))
                        out.push_back({p, n, t});
                    continue;
                }
                const glm::dvec3 side = sideOf(n, t);
                for (double sgn : {1.0, -1.0})
                {
                    const glm::dvec3 q = p + side * (sgn * o.inset);
                    if (!blocked(r, q, o.inset * 0.9, int(ri)) && r.lineAt(q, o.inset * 0.9).run < 0)
                        out.push_back({q, n, t});
                }
            }
        }
        return out;
    }

    // Corners: between each pair of neighbouring lines leaving a junction, along their bisector.
    for (const layout::ResolvedJunction& j : r.junctions())
    {
        if (j.degree < 2)
            continue;
        struct Leg
        {
            glm::dvec3 dir;
            glm::dvec3 normal;
            bool ours;
        };
        std::vector<Leg> legs;
        for (const layout::ResolvedRun& run : r.runs())
        {
            if (run.path.empty())
                continue;
            const bool ours = inSet(r, run, o.set);
            if (run.a == j.id)
                legs.push_back({run.path.tangentAt(0.0), run.path.normals.front(), ours});
            if (run.b == j.id)
                legs.push_back({-run.path.tangentAt(run.path.length()), run.path.normals.back(), ours});
        }
        if (legs.size() < 2 || std::none_of(legs.begin(), legs.end(), [](const Leg& l) { return l.ours; }))
            continue;
        const glm::dvec3 n = glm::normalize(legs.front().normal);
        glm::dvec3 u = legs.front().dir - n * glm::dot(legs.front().dir, n);
        if (glm::length(u) < 1e-9)
            continue;
        u = glm::normalize(u);
        const glm::dvec3 v = glm::cross(n, u);
        std::vector<double> angles;
        for (const Leg& l : legs)
            angles.push_back(std::atan2(glm::dot(l.dir, v), glm::dot(l.dir, u)));
        std::sort(angles.begin(), angles.end());
        for (size_t k = 0; k < angles.size(); ++k)
        {
            const double a0 = angles[k], a1 = k + 1 < angles.size() ? angles[k + 1] : angles[0] + 2.0 * 3.14159265358979;
            const double span = a1 - a0;
            // A line passing straight through leaves no corner.
            if (span > 3.14159265358979 * 0.94)
                continue;
            const double mid = (a0 + a1) * 0.5;
            const glm::dvec3 bis = u * std::cos(mid) + v * std::sin(mid);
            const double reach = o.inset / std::max(0.2, std::sin(span * 0.5));
            const glm::dvec3 q = j.position + bis * reach;
            if (r.lineAt(q, o.inset * 0.9).run < 0)
                out.push_back({q, n, bis});
        }
    }
    return out;
}

geo::Mesh fastenerMesh(const FastenerOptions& o)
{
    const double r = std::max(0.01, o.size * 0.5), h = std::max(0.005, o.height);
    switch (o.kind)
    {
    case FastenerKind::Rivet:
    {
        geo::Mesh m = geo::makeSphere({.radius = r, .subdivAxis = 12, .subdivHeight = 8, .fraction = 0.5});
        scaleY(m, h / r);
        return m;
    }
    case FastenerKind::HexBolt:
        return geo::makeCylinder({.radius = r, .height = h, .subdivAxis = 6, .baseAtOrigin = true});
    case FastenerKind::Screw:
    {
        geo::Mesh m = geo::makeSphere({.radius = r, .subdivAxis = 16, .subdivHeight = 8, .fraction = 0.35});
        // The kept cap starts above the equator: drop it to the base, then flatten it to the height.
        const double base = m.bounds().min.y;
        scaleY(m, 1.0, -base);
        scaleY(m, h / std::max(1e-6, m.bounds().max.y));
        return m;
    }
    case FastenerKind::Washer:
    {
        geo::Mesh m = geo::makeTorus({.radius = r * 0.75, .sectionRadius = r * 0.25, .subdivAxis = 16, .subdivHeight = 6});
        scaleY(m, h / (r * 0.5), h * 0.5);
        return m;
    }
    }
    return {};
}

std::vector<glm::dmat4> fastenerPlacements(const layout::Resolved& r, const geo::Mesh& surface, const FastenerOptions& o)
{
    std::vector<glm::dmat4> out;
    const std::vector<FastenerSpot> spots = fastenerSpots(r, o);
    if (spots.empty())
        return out;
    const geo::MeshBvh bvh(surface);
    const double reach = std::max({o.height * 20.0, o.size * 10.0, 10.0});
    for (const FastenerSpot& s : spots)
    {
        const glm::dvec3 n = glm::length(s.normal) > 1e-12 ? glm::normalize(s.normal) : glm::dvec3(0, 1, 0);
        glm::dvec3 seat = s.position;
        geo::BvhHit hit;
        // Dropped from above along the normal onto whatever is there: the plate top, the grooved surface.
        if (bvh.raycast(s.position + n * reach, -n, hit, reach * 2.0, false))
            seat = hit.point;
        glm::dvec3 x = s.tangent - n * glm::dot(s.tangent, n);
        x = glm::length(x) > 1e-9 ? glm::normalize(x) : glm::normalize(glm::cross(n, std::abs(n.y) < 0.9 ? glm::dvec3(0, 1, 0) : glm::dvec3(1, 0, 0)));
        const glm::dvec3 z = glm::cross(x, n);
        const glm::dvec3 base = seat - n * (o.sink + std::max(0.0, o.recess));
        out.push_back(glm::dmat4(glm::dvec4(x, 0.0), glm::dvec4(n, 0.0), glm::dvec4(z, 0.0), glm::dvec4(base, 1.0)));
    }
    return out;
}

namespace
{
/** Presses a round recess under each placement into the surface's points. */
size_t pressRecesses(geo::Mesh& m, const std::vector<glm::dmat4>& at, const FastenerOptions& o)
{
    const double radius = std::max(0.01, o.size * 0.5) * 1.2, depth = o.recess;
    const double cell = radius * 2.0;
    std::unordered_map<uint64_t, std::vector<size_t>> grid;
    auto key = [&](const glm::dvec3& p) {
        const glm::ivec3 c(glm::floor(p / cell));
        return (uint64_t(uint32_t(c.x + (1 << 20))) & 0x1fffff) | ((uint64_t(uint32_t(c.y + (1 << 20))) & 0x1fffff) << 21) |
               ((uint64_t(uint32_t(c.z + (1 << 20))) & 0x1fffff) << 42);
    };
    for (size_t k = 0; k < at.size(); ++k)
        grid[key(glm::dvec3(at[k][3]))].push_back(k);
    size_t moved = 0;
    for (Index p = 0; p < m.pointCapacity(); ++p)
    {
        if (!m.pointValid(p))
            continue;
        const glm::dvec3 q = m.position(p);
        const glm::ivec3 c(glm::floor(q / cell));
        double press = 0.0;
        glm::dvec3 dir(0.0);
        for (int dx = -1; dx <= 1; ++dx)
            for (int dy = -1; dy <= 1; ++dy)
                for (int dz = -1; dz <= 1; ++dz)
                {
                    const glm::dvec3 probe = (glm::dvec3(c + glm::ivec3(dx, dy, dz)) + 0.5) * cell;
                    auto it = grid.find(key(probe));
                    if (it == grid.end())
                        continue;
                    for (size_t k : it->second)
                    {
                        const glm::dvec3 n(at[k][1]), floor(at[k][3]);
                        // The recess's top is the surface where the head would sit before being sunk.
                        const glm::dvec3 top = floor + n * (o.sink + depth);
                        const glm::dvec3 d = q - top;
                        const double up = glm::dot(d, n);
                        if (std::abs(up) > depth + radius * 0.5)
                            continue;
                        const double across = glm::length(d - n * up) / radius;
                        if (across >= 1.0)
                            continue;
                        const double t = across < 0.8 ? 1.0 : 1.0 - (across - 0.8) / 0.2;
                        const double amount = depth * t * t * (3.0 - 2.0 * t);
                        if (amount > press)
                        {
                            press = amount;
                            dir = n;
                        }
                    }
                }
        if (press > 0.0)
        {
            m.setPosition(p, q - dir * press);
            ++moved;
        }
    }
    return moved;
}
} // namespace

FastenersResult makeFasteners(const layout::Resolved& r, const geo::Mesh& surface, const FastenerOptions& o)
{
    LILA_ZONE();
    FastenersResult out;
    const std::vector<glm::dmat4> at = fastenerPlacements(r, surface, o);
    if (at.empty())
        return out;
    const geo::Mesh shape = fastenerMesh(o);
    for (const glm::dmat4& frame : at)
    {
        geo::Mesh one = shape;
        for (Index p = 0; p < one.pointCapacity(); ++p)
            if (one.pointValid(p))
                one.setPosition(p, glm::dvec3(frame * glm::dvec4(one.position(p), 1.0)));
        out.mesh.append(one);
        ++out.count;
    }
    geo::setHardEdgesByAngle(out.mesh, 40.0);
    if (o.recess > 0.0)
    {
        out.surface = surface;
        const size_t moved = pressRecesses(out.surface, at, o);
        if (moved < at.size() * 3)
            out.warnings.push_back("Recesses need a mesh dense enough to show them: add sculpt levels or subdivide under the fasteners");
    }
    return out;
}
} // namespace plating
