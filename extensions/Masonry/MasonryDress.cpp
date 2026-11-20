// Dressing on the stones: a peeling render coat and climbing ivy, as meshes beside the stones and core.

#include "MasonryRuin.h"

#include <lila/GeometryNodes.h>
#include <lila/Log.h>
#include <lila/Profile.h>

#include <algorithm>
#include <cmath>

namespace masonry
{
namespace
{
double smoothStep(double t) { return t * t * (3.0 - 2.0 * t); }

/** Value noise in [0, 1], seeded. */
double patchNoise(const glm::dvec3& p, uint64_t seed)
{
    const glm::dvec3 f = glm::floor(p), t = p - f;
    auto at = [&](int dx, int dy, int dz) {
        return unitRandom(mixKey(seed, uint64_t(int64_t(f.x) + dx), uint64_t(int64_t(f.y) + dy), uint64_t(int64_t(f.z) + dz), 991));
    };
    const double sx = smoothStep(t.x), sy = smoothStep(t.y), sz = smoothStep(t.z);
    auto mix = [](double a, double b, double s) { return a + (b - a) * s; };
    return mix(mix(mix(at(0, 0, 0), at(1, 0, 0), sx), mix(at(0, 1, 0), at(1, 1, 0), sx), sy),
               mix(mix(at(0, 0, 1), at(1, 0, 1), sx), mix(at(0, 1, 1), at(1, 1, 1), sx), sy), sz);
}

bool onFace(const Stone& s)
{
    return s.t1 >= s.t0 && s.face != Face::Inner && s.role != Role::Rubble && s.role != Role::Slab && s.role != Role::Paving;
}

double typical(std::span<const Stone> stones)
{
    std::vector<double> h;
    for (const Stone& s : stones)
        h.push_back(s.size.y);
    if (h.empty())
        return 10.0;
    std::nth_element(h.begin(), h.begin() + std::ptrdiff_t(h.size() / 2), h.end());
    return std::max(1.0, h[h.size() / 2]);
}
} // namespace

geo::Mesh plasterMesh(std::span<const Stone> stones, std::span<const float> height, const AgeParams& p, double joint)
{
    LILA_ZONE_N("Masonry plaster");
    geo::Mesh out;
    const double unit = typical(stones);
    const double thick = p.plasterThickness > 0.0 ? p.plasterThickness : unit / 3.0;
    const double cover = std::clamp(p.plaster, 0.0, 1.0);
    const double j = std::max(0.0, joint);
    for (size_t k = 0; k < stones.size(); ++k)
    {
        const Stone& s = stones[k];
        if (!onFace(s))
            continue;
        // Peels where it is high up, damaged, exposed to the weather or just weak, in patches.
        const double patches = patchNoise(s.center / (unit * 5.0), p.seed + 31);
        const double peel = 0.55 * patches + 0.25 * (k < height.size() ? double(height[k]) : 0.5) + 0.35 * s.damage +
                            0.1 * unitRandom(mixKey(p.seed, 32, k));
        if (peel > cover * 1.25)
            continue;
        const glm::dvec3 out3 = -s.frame[2];
        const glm::dvec3 centre = s.center + out3 * (s.size.z * 0.5 + thick * 0.5 + 0.05);
        const glm::dvec3 h((s.size.x + j) * 0.5, (s.size.y + j) * 0.5, thick * 0.5);
        std::array<glm::dvec3, 8> c;
        for (int i = 0; i < 8; ++i)
            c[size_t(i)] = centre + s.frame * glm::dvec3((i & 1) ? h.x : -h.x, (i & 2) ? h.y : -h.y, (i & 4) ? h.z : -h.z);
        out.append(hexMesh(c));
    }
    if (out.numFaces() > 0)
    {
        std::span<glm::vec2> uv = out.cornerAttrs().getOrAdd<glm::vec2>(geo::attr::uvName(0));
        const double tile = unit * 15.0;
        for (geo::Index f = 0; f < out.faceCapacity(); ++f)
            if (out.faceValid(f))
            {
                const glm::dvec3 n = glm::abs(out.faceNormal(f));
                const int axis = n.x >= n.y && n.x >= n.z ? 0 : (n.y >= n.z ? 1 : 2);
                for (geo::Index c : out.faceCorners(f))
                {
                    const glm::dvec3 q = out.position(out.cornerPoint(c));
                    uv[c] = axis == 0 ? glm::vec2(float(q.z / tile), float(q.y / tile))
                                      : (axis == 1 ? glm::vec2(float(q.x / tile), float(q.z / tile)) : glm::vec2(float(q.x / tile), float(q.y / tile)));
                }
            }
        lila::scene::materials::assign(out, kPlasterSlot, {});
    }
    return out;
}

geo::Mesh ivyMesh(std::span<const Stone> stones, const AgeParams& p)
{
    namespace gn = lila::gn;
    namespace log = lila::log;
    LILA_ZONE_N("Masonry ivy");
    // Weathering's Ivy geometry node grows it; Masonry lists Weathering as a dependency, so it is there when Masonry loads.
    const gn::NodeDecl* grower = gn::findDecl("IvyGrowth");
    if (!grower || !grower->exec)
    {
        log::warn("Masonry: no ivy grown: it needs the Weathering extension, which is not loaded");
        return {};
    }
    const double unit = typical(stones);
    const double leaf = p.leaf > 0.0 ? p.leaf : unit * 0.66;
    double lo = 1e300, hi = -1e300;
    for (const Stone& s : stones)
    {
        lo = std::min(lo, s.center.y - s.size.y * 0.5);
        hi = std::max(hi, s.center.y + s.size.y * 0.5);
    }
    // The surface: every face stone as a box; seeds at the foot of grounded ones.
    geo::Mesh surface;
    geo::Mesh seeds;
    for (size_t k = 0; k < stones.size(); ++k)
    {
        const Stone& s = stones[k];
        if (!onFace(s))
            continue;
        const glm::dvec3 h = s.size * 0.5;
        geo::Index v[8];
        for (int i = 0; i < 8; ++i)
            v[i] = surface.addPoint(s.center + s.frame * glm::dvec3((i & 1) ? h.x : -h.x, (i & 2) ? h.y : -h.y, (i & 4) ? h.z : -h.z));
        const int faces[6][4] = {{0, 2, 6, 4}, {1, 5, 7, 3}, {0, 4, 5, 1}, {2, 3, 7, 6}, {0, 1, 3, 2}, {4, 6, 7, 5}};
        for (const auto& f : faces)
        {
            const geo::Index face = surface.addFace({v[f[0]], v[f[1]], v[f[2]], v[f[3]]});
            if (face != geo::kInvalid && glm::dot(surface.faceNormal(face), surface.faceCentroid(face) - s.center) < 0.0)
                surface.flipFace(face);
        }
        if (s.grounded && unitRandom(mixKey(p.seed, 41, k)) <= std::clamp(p.ivy, 0.0, 1.0) * 0.35)
            seeds.addPoint(s.center + s.frame * glm::dvec3(0.0, -h.y + 0.5, -h.z));
    }
    if (seeds.numPoints() == 0 || surface.numFaces() == 0)
        return {};
    const double reach = (hi - lo) * std::clamp(p.ivyHeight, 0.0, 1.0) * 1.15;

    gn::Group group;
    gn::GNode node;
    node.name = "masonryIvy";
    node.type = grower->type;
    node.values = {{"seed", int64_t(p.seed % 1000003)}, {"length", std::max(unit, reach)}, {"branchiness", 0.4}, {"climb", 0.85},
                   {"gravity", 0.2},         {"adhesion", std::max(1.0, unit * 0.1)},       {"smoothness", 0.15},
                   {"stemRadius", std::max(0.1, leaf * 0.05)}, {"leafDensity", 100.0 / std::max(1.0, leaf * 0.5)},
                   {"leafSize", leaf},       {"flowerDensity", 0.0}};
    gn::ExecEnv env;
    gn::ExecContext c(group, node, *grower, grower->sockets, env);
    c.inputs.resize(grower->sockets.inputs.size());
    c.outputs.resize(grower->sockets.outputs.size());
    for (size_t k = 0; k < grower->sockets.inputs.size(); ++k)
    {
        const gn::SocketDecl& sd = grower->sockets.inputs[k];
        gn::Value v;
        if (sd.id == "surface")
            v.geometry = gn::meshGeometry(std::move(surface));
        else if (sd.id == "seeds")
            v.geometry = gn::pointGeometry(std::move(seeds));
        else if (sd.type == gn::SocketType::Geometry)
            v.geometry = gn::makeGeometry({});
        else
            v = gn::valueFromJson(sd.type, node.values.contains(sd.id) ? node.values[sd.id] : sd.defaultValue);
        c.inputs[k].push_back(std::move(v));
    }
    for (size_t k = 0; k < grower->sockets.outputs.size(); ++k)
        c.outputs[k] = gn::defaultValue(grower->sockets.outputs[k].type);
    try
    {
        grower->exec(c);
    }
    catch (const std::exception& e)
    {
        log::warn("Masonry: ivy: {}", e.what());
        return {};
    }
    const int out = grower->sockets.outputIndex("geometry");
    if (out < 0 || !c.outputs[size_t(out)].geometry || !c.outputs[size_t(out)].geometry->mesh)
        return {};
    geo::Mesh m = *c.outputs[size_t(out)].geometry->mesh;
    lila::scene::materials::assign(m, kIvySlot, {});
    return m;
}
} // namespace masonry
