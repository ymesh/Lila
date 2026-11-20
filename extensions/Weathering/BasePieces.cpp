// Placeholder pieces for base and ground dressing: rubble, bricks, planks, beams, plants, moss, pebbles and litter, each
// in a realistic and a toon version. Built in code, in cm, standing on y = 0.

#include "Placeholders.h"

#include <lila/Mesh.h>
#include <lila/MeshOps.h>

#include <glm/gtc/matrix_transform.hpp>

#include <algorithm>
#include <cmath>

using namespace lila;

namespace weathering
{
namespace basepieces
{
constexpr double kPi = 3.14159265358979323846;

/** Deterministic noise for piece shapes. */
struct PieceRng
{
    uint32_t state;
    explicit PieceRng(uint32_t seed) : state(seed * 747796405u + 2891336453u) {}
    double u()
    {
        state = state * 747796405u + 2891336453u;
        uint32_t w = ((state >> ((state >> 28u) + 4u)) ^ state) * 277803737u;
        w = (w >> 22u) ^ w;
        return double(w) / 4294967296.0;
    }
    double range(double a, double b) { return a + (b - a) * u(); }
};

/** Moves a mesh so its lowest point is on y = 0 and its centre over the origin. */
geo::Mesh grounded(geo::Mesh m)
{
    glm::dvec3 lo(1e300), hi(-1e300);
    for (geo::Index p = 0; p < m.pointCapacity(); ++p)
        if (m.pointValid(p))
        {
            lo = glm::min(lo, m.position(p));
            hi = glm::max(hi, m.position(p));
        }
    const glm::dvec3 shift(-(lo.x + hi.x) * 0.5, -lo.y, -(lo.z + hi.z) * 0.5);
    for (geo::Index p = 0; p < m.pointCapacity(); ++p)
        if (m.pointValid(p))
            m.setPosition(p, m.position(p) + shift);
    return m;
}

/** An irregular chunk: the hull of points scattered in a box, rough (many facets) or chunky (few, big facets). */
geo::Mesh chunk(const glm::dvec3& size, uint32_t seed, bool toon)
{
    PieceRng rng(seed);
    std::vector<glm::dvec3> pts;
    const int count = toon ? 9 : 48;
    for (int k = 0; k < count; ++k)
    {
        // Toon chunks keep points near the box's corners so the faces stay large and the silhouette clean.
        glm::dvec3 p(rng.range(-0.5, 0.5), rng.range(-0.5, 0.5), rng.range(-0.5, 0.5));
        if (toon)
            p = glm::sign(p) * glm::dvec3(rng.range(0.32, 0.5), rng.range(0.32, 0.5), rng.range(0.32, 0.5));
        pts.push_back(p * size);
    }
    return grounded(geo::convexHullMesh(pts, toon ? 16 : 48));
}

/** A box with optional broken ends: the hull of its corners, one end's corners pulled in at random. */
geo::Mesh brokenBox(const glm::dvec3& size, uint32_t seed, double breakage, int axis)
{
    PieceRng rng(seed);
    std::vector<glm::dvec3> pts;
    for (int k = 0; k < 8; ++k)
    {
        glm::dvec3 c((k & 1) ? 0.5 : -0.5, (k & 2) ? 0.5 : -0.5, (k & 4) ? 0.5 : -0.5);
        if (c[axis] > 0.0 && breakage > 0.0)
            c[axis] -= rng.range(0.0, breakage);
        pts.push_back(c * size);
    }
    // A few extra points on the broken end make it ragged.
    for (int k = 0; k < (breakage > 0.0 ? 6 : 0); ++k)
    {
        glm::dvec3 c(rng.range(-0.5, 0.5), rng.range(-0.5, 0.5), rng.range(-0.5, 0.5));
        c[axis] = 0.5 - rng.range(0.0, breakage);
        pts.push_back(c * size);
    }
    return grounded(geo::convexHullMesh(pts, 32));
}

/** A box with its edges cut off at chamfer (fraction of the smallest side): a softer, toon look. */
geo::Mesh chamferedBox(const glm::dvec3& size, double chamfer)
{
    std::vector<glm::dvec3> pts;
    const double c = std::min({size.x, size.y, size.z}) * chamfer;
    const glm::dvec3 h = size * 0.5;
    for (int k = 0; k < 8; ++k)
    {
        const glm::dvec3 s((k & 1) ? 1.0 : -1.0, (k & 2) ? 1.0 : -1.0, (k & 4) ? 1.0 : -1.0);
        pts.push_back(s * glm::dvec3(h.x - c, h.y, h.z - c));
        pts.push_back(s * glm::dvec3(h.x, h.y - c, h.z - c));
        pts.push_back(s * glm::dvec3(h.x - c, h.y - c, h.z));
    }
    return grounded(geo::convexHullMesh(pts, 64));
}

/** A clump of blades: thin double-sided cards fanning out of the ground. */
geo::Mesh blades(int count, double height, double width, double spread, uint32_t seed, bool toon)
{
    PieceRng rng(seed);
    geo::Mesh m;
    for (int k = 0; k < count; ++k)
    {
        const double angle = 2.0 * kPi * (double(k) + rng.u()) / count;
        const double h = height * rng.range(0.6, 1.0);
        const double lean = rng.range(0.1, 0.45) * spread;
        const glm::dvec3 out(std::cos(angle), 0.0, std::sin(angle));
        const glm::dvec3 side(-out.z, 0.0, out.x);
        const double w = width * (toon ? 1.6 : rng.range(0.7, 1.0));
        // Toon blades are one broad triangle; realistic ones taper over two segments and bend outward.
        const glm::dvec3 base = out * rng.range(0.0, spread * 0.2);
        if (toon)
        {
            const geo::Index a = m.addPoint(base - side * w), b = m.addPoint(base + side * w), t = m.addPoint(base + out * (h * lean) + glm::dvec3(0, h, 0));
            m.addFace({a, b, t});
            m.addFace({b, a, t});
            continue;
        }
        const glm::dvec3 mid = base + out * (h * lean * 0.35) + glm::dvec3(0, h * 0.55, 0);
        const glm::dvec3 tip = base + out * (h * lean) + glm::dvec3(0, h, 0);
        const geo::Index a = m.addPoint(base - side * w), b = m.addPoint(base + side * w);
        const geo::Index c = m.addPoint(mid + side * w * 0.6), d = m.addPoint(mid - side * w * 0.6), t = m.addPoint(tip);
        m.addFace({a, b, c, d});
        m.addFace({d, c, b, a});
        m.addFace({d, c, t});
        m.addFace({t, c, d});
    }
    return grounded(std::move(m));
}

/** A low dome (moss, a toon bush): a squashed sphere cut at its middle. */
geo::Mesh dome(const glm::dvec3& size, uint32_t seed, bool toon)
{
    PieceRng rng(seed);
    std::vector<glm::dvec3> pts;
    const int rings = toon ? 3 : 6, around = toon ? 7 : 14;
    pts.push_back({0.0, 0.5 * size.y, 0.0});
    for (int r = 0; r <= rings; ++r)
    {
        const double phi = 0.5 * kPi * double(r) / rings;
        for (int k = 0; k < around; ++k)
        {
            const double theta = 2.0 * kPi * (double(k) + (toon ? 0.0 : rng.u() * 0.5)) / around;
            const double wobble = toon ? 1.0 : rng.range(0.85, 1.1);
            pts.push_back(glm::dvec3(std::sin(phi) * std::cos(theta) * 0.5 * size.x * wobble, std::cos(phi) * 0.5 * size.y,
                                     std::sin(phi) * std::sin(theta) * 0.5 * size.z * wobble));
        }
    }
    return grounded(geo::convexHullMesh(pts, int(pts.size())));
}

/** A thin bent sheet (a newspaper, a leaf): a card with a lifted corner. */
geo::Mesh sheet(double width, double depth, double lift, uint32_t seed, bool toon)
{
    PieceRng rng(seed);
    geo::Mesh m;
    const int n = toon ? 1 : 3;
    std::vector<geo::Index> ids;
    for (int z = 0; z <= n; ++z)
        for (int x = 0; x <= n; ++x)
        {
            const double u = double(x) / n, v = double(z) / n;
            const double y = lift * u * v + (toon ? 0.0 : rng.range(0.0, lift * 0.2));
            ids.push_back(m.addPoint({(u - 0.5) * width, y, (v - 0.5) * depth}));
        }
    for (int z = 0; z < n; ++z)
        for (int x = 0; x < n; ++x)
        {
            const geo::Index a = ids[size_t(z * (n + 1) + x)], b = ids[size_t(z * (n + 1) + x + 1)];
            const geo::Index c = ids[size_t((z + 1) * (n + 1) + x + 1)], d = ids[size_t((z + 1) * (n + 1) + x)];
            m.addFace({a, d, c, b});
            m.addFace({b, c, d, a});
        }
    return grounded(std::move(m));
}

geo::Mesh can(bool toon)
{
    geo::CylinderOptions o;
    o.radius = toon ? 4.5 : 3.3;
    o.height = toon ? 10.0 : 12.0;
    o.subdivAxis = toon ? 8 : 16;
    geo::Mesh m = geo::makeCylinder(o);
    // Lying on its side, as litter does.
    const glm::dmat4 r = glm::rotate(glm::dmat4(1.0), kPi * 0.5, glm::dvec3(0, 0, 1));
    for (geo::Index p = 0; p < m.pointCapacity(); ++p)
        if (m.pointValid(p))
            m.setPosition(p, glm::dvec3(r * glm::dvec4(m.position(p), 1.0)));
    return grounded(std::move(m));
}

void add(const char* collection, std::string name, const char* role, bool toon, std::vector<std::string> tags, std::string description,
         const char* physicsRole, const char* material, const char* shape, std::function<geo::Mesh()> build)
{
    Placeholder p;
    p.collection = std::string("Weathering/") + collection;
    p.name = name + (toon ? "Toon" : "");
    p.role = role;
    p.style = toon ? "toon" : "realistic";
    p.tags = std::move(tags);
    p.description = std::move(description);
    p.physicsRole = physicsRole;
    p.physicsMaterial = material;
    p.physicsShape = shape;
    p.build = std::move(build);
    addPlaceholder(std::move(p));
}
} // namespace basepieces

void registerBasePieces()
{
    using namespace basepieces;
    for (bool toon : {false, true})
    {
        for (uint32_t k = 1; k <= 3; ++k)
        {
            const glm::dvec3 size = toon ? glm::dvec3(18.0 + 6.0 * k, 12.0 + 3.0 * k, 16.0 + 4.0 * k) : glm::dvec3(10.0 + 7.0 * k, 6.0 + 4.0 * k, 9.0 + 6.0 * k);
            add("Rubble", "ConcreteChunk" + std::to_string(k), "rubble", toon, {"concrete", "debris", "stone"}, "Broken concrete.", "dynamic", "stone",
                "hull", [=] { return chunk(size, 100 + k, toon); });
            add("Rubble", "Rock" + std::to_string(k), "rock", toon, {"stone", "debris"}, "A rock.", "dynamic", "stone", "hull",
                [=] { return chunk(size * glm::dvec3(1.2, 0.9, 1.1), 200 + k, toon); });
        }
        for (uint32_t k = 1; k <= 2; ++k)
        {
            add("Bricks", "BrokenBrick" + std::to_string(k), "brick", toon, {"debris", "clay"}, "Half a brick.", "dynamic", "ceramic", "hull", [=] {
                return toon ? chamferedBox({20.0, 8.0, 11.0}, 0.22) : brokenBox({21.5, 6.5, 10.2}, 300 + k, 0.45, 0);
            });
        }
        add("Bricks", "Brick", "brick", toon, {"debris", "clay"}, "A whole brick.", "dynamic", "ceramic", "box",
            [=] { return toon ? chamferedBox({22.0, 8.0, 11.0}, 0.22) : brokenBox({21.5, 6.5, 10.2}, 310, 0.0, 0); });
        for (uint32_t k = 1; k <= 3; ++k)
        {
            const double length = 140.0 + 35.0 * k;
            // Long axis up, so base dressing leans them on walls.
            add("Planks", "Plank" + std::to_string(k), "plank", toon, {"wood", "board", "debris"}, "A loose board.", "dynamic", "wood", "box", [=] {
                return toon ? chamferedBox({18.0, length, 4.0}, 0.3) : brokenBox({15.0, length, 2.5}, 400 + k, k == 2 ? 0.08 : 0.0, 1);
            });
            add("Planks", "CharredPlank" + std::to_string(k), "charred", toon, {"wood", "burned", "debris"}, "A burned board.", "dynamic", "wood",
                "box", [=] { return toon ? chamferedBox({16.0, length * 0.8, 5.0}, 0.35) : brokenBox({14.0, length * 0.8, 3.0}, 420 + k, 0.2, 1); });
        }
        add("Planks", "Beam", "beam", toon, {"wood", "debris", "plank"}, "A heavy beam.", "dynamic", "wood", "box",
            [=] { return toon ? chamferedBox({16.0, 240.0, 16.0}, 0.25) : brokenBox({11.0, 250.0, 11.0}, 450, 0.06, 1); });
        for (uint32_t k = 1; k <= 3; ++k)
        {
            add("Plants", "Weeds" + std::to_string(k), "plant", toon, {"weed", "grass", "foliage"}, "A clump of weeds.", "off", "wood", "box",
                [=] { return blades(toon ? 5 : 14, 30.0 + 12.0 * k, toon ? 3.0 : 1.4, 1.0, 500 + k, toon); });
            add("Plants", "GrassTuft" + std::to_string(k), "grass", toon, {"grass", "foliage", "ground"}, "A tuft of grass.", "off", "wood", "box",
                [=] { return blades(toon ? 4 : 18, 12.0 + 4.0 * k, toon ? 2.2 : 0.6, 0.6, 520 + k, toon); });
            add("Plants", "Moss" + std::to_string(k), "moss", toon, {"moss", "foliage"}, "A moss clump.", "off", "wood", "hull",
                [=] { return dome({20.0 + 8.0 * k, 6.0 + 2.0 * k, 16.0 + 6.0 * k}, 540 + k, toon); });
            add("Plants", "Bush" + std::to_string(k), "bush", toon, {"bush", "foliage", "plant"}, "A small bush.", "off", "wood", "hull",
                [=] { return dome({50.0 + 12.0 * k, 40.0 + 10.0 * k, 45.0 + 12.0 * k}, 560 + k, toon); });
        }
        for (uint32_t k = 1; k <= 3; ++k)
            add("Ground", "Pebble" + std::to_string(k), "pebble", toon, {"stone", "ground"}, "A pebble.", "dynamic", "stone", "hull",
                [=] { return toon ? dome({5.0 + 2.0 * k, 3.0 + k, 4.0 + 2.0 * k}, 600 + k, true) : chunk({3.0 + 2.0 * k, 2.0 + k, 3.0 + 1.5 * k}, 600 + k, false); });
        add("Ground", "Paper", "litter", toon, {"litter", "paper", "ground"}, "A sheet of paper.", "off", "wood", "box",
            [=] { return sheet(toon ? 24.0 : 21.0, toon ? 30.0 : 29.7, toon ? 3.0 : 4.0, 700, toon); });
        add("Ground", "Can", "litter", toon, {"litter", "metal", "ground"}, "A drink can.", "dynamic", "metal", "hull", [=] { return can(toon); });
        add("Ground", "Leaf", "leaf", toon, {"leaf", "litter", "ground"}, "A fallen leaf.", "off", "wood", "box",
            [=] { return sheet(toon ? 9.0 : 6.0, toon ? 12.0 : 8.0, toon ? 1.0 : 1.5, 720, toon); });
    }
}
} // namespace weathering
