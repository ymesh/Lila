#include "TexTest.h"

#include "core/ImageIO.h"
#include "geo/Attribute.h"
#include "geo/Mesh.h"
#include "tex/PatchFill.h"

#include <cmath>

using namespace textest;

namespace
{
constexpr int kSize = 256;

/**
 * Two quads sharing the edge x = 0 (100 units across, facing +Y) as two uv islands: the left one's seam side at u = 0.45,
 * the right one mirrored in u and v with its seam side at u = 0.95.
 */
glm::vec2 islandUv(int face, double x, double z)
{
    const double t = (z + 50.0) / 100.0;
    if (face == 0)
        return {float(0.05 + (x + 50.0) / 50.0 * 0.4), float(0.9 - t * 0.8)};
    return {float(0.95 - x / 50.0 * 0.4), float(0.1 + t * 0.8)};
}

/** The surface point (x, z) a uv lands on through an island's mapping, extended past its edges. */
glm::dvec2 islandPoint(int face, glm::vec2 uv)
{
    if (face == 0)
        return {(uv.x - 0.05) / 0.4 * 50.0 - 50.0, (0.9 - uv.y) / 0.8 * 100.0 - 50.0};
    return {(0.95 - uv.x) / 0.4 * 50.0, (uv.y - 0.1) / 0.8 * 100.0 - 50.0};
}

geo::Mesh seamPanel()
{
    const std::vector<glm::dvec3> pts = {{-50, 0, -50}, {0, 0, -50}, {50, 0, -50}, {-50, 0, 50}, {0, 0, 50}, {50, 0, 50}};
    geo::Mesh m = geo::Mesh::fromPolygons(pts, std::vector<geo::Index>{4, 4}, std::vector<geo::Index>{0, 3, 4, 1, 1, 4, 5, 2});
    auto uv = m.cornerAttrs().getOrAdd<glm::vec2>(geo::attr::uvName(0));
    for (geo::Index f = 0; f < m.faceCapacity(); ++f)
        if (m.faceValid(f))
            for (geo::Index c : m.faceCorners(f))
            {
                const glm::dvec3 p = m.position(m.cornerPoint(c));
                uv[c] = islandUv(int(f), p.x, p.z);
            }
    return m;
}

/** A pattern laid on the surface by position, so it runs on unbroken across the seam. */
glm::vec3 surfacePattern(glm::dvec2 p)
{
    // Every channel repeats every 12 units both ways.
    const bool odd = (int(std::floor(p.x / 6.0)) + int(std::floor(p.y / 6.0))) % 2 != 0;
    const double ramp = (p.x + p.y) / 12.0;
    return {odd ? 0.9f : 0.1f, float(ramp - std::floor(ramp)), float(0.5 + 0.5 * std::sin(p.x * 2.0 * 3.14159265 / 12.0))};
}

/** Which island a texel centre lies on (-1 for none) and its surface point. */
int texelIsland(int x, int y, glm::dvec2* at)
{
    const glm::vec2 uv((x + 0.5f) / kSize, 1.0f - (y + 0.5f) / kSize);
    for (int face = 0; face < 2; ++face)
    {
        const glm::dvec2 p = islandPoint(face, uv);
        if (p.y > -50.0 && p.y < 50.0 && (face == 0 ? p.x > -50.0 && p.x < 0.0 : p.x > 0.0 && p.x < 50.0))
        {
            *at = p;
            return face;
        }
    }
    return -1;
}

struct Case
{
    image::Image truth, damaged;
    std::vector<uint8_t> hole;
};

/** The pattern on both islands, with a hole across the seam: 9 units each side of it, 40 along it. */
Case makeCase()
{
    Case c;
    c.truth.width = c.truth.height = kSize;
    c.truth.rgba.assign(size_t(kSize) * kSize * 4, 0.0f);
    c.hole.assign(size_t(kSize) * kSize, 0);
    for (int y = 0; y < kSize; ++y)
        for (int x = 0; x < kSize; ++x)
        {
            float* px = &c.truth.rgba[(size_t(y) * kSize + x) * 4];
            glm::dvec2 p;
            if (texelIsland(x, y, &p) >= 0)
            {
                const glm::vec3 v = surfacePattern(p);
                px[0] = v.x;
                px[1] = v.y;
                px[2] = v.z;
                px[3] = 1.0f;
            }
            // Each side's hole runs a little past its seam edge, so the island's edge texels are in it.
            const glm::vec2 uv((x + 0.5f) / kSize, 1.0f - (y + 0.5f) / kSize);
            const glm::dvec2 l = islandPoint(0, uv), r = islandPoint(1, uv);
            if ((l.x > -9.0 && l.x < 1.5 && std::abs(l.y) < 20.0) || (r.x > -1.5 && r.x < 9.0 && std::abs(r.y) < 20.0))
                c.hole[size_t(y) * kSize + x] = 1;
        }
    c.damaged = c.truth;
    for (size_t i = 0; i < c.hole.size(); ++i)
        if (c.hole[i])
            for (int k = 0; k < 4; ++k)
                c.damaged.rgba[i * 4 + size_t(k)] = 0.0f;
    return c;
}

/** Mean absolute error over the hole's island texels on one side (face), RGB. */
double sideError(const Case& c, const image::Image& filled, int face)
{
    double sum = 0.0;
    int n = 0;
    for (int y = 0; y < kSize; ++y)
        for (int x = 0; x < kSize; ++x)
        {
            glm::dvec2 p;
            const size_t i = size_t(y) * kSize + x;
            if (!c.hole[i] || texelIsland(x, y, &p) != face || std::abs(p.x) < 0.5)
                continue;
            for (int k = 0; k < 3; ++k, ++n)
                sum += std::abs(filled.rgba[i * 4 + size_t(k)] - c.truth.rgba[i * 4 + size_t(k)]);
        }
    return n ? sum / n : 1.0;
}
} // namespace

TEST_CASE("patch fill across uv seams: a hole on a seam fills from both sides, closer than island by island")
{
    const Case c = makeCase();
    const geo::Mesh mesh = seamPanel();
    tex::PatchFillOptions o;
    o.patch = 7;
    o.seed = 3;
    const image::Image seams = tex::patchFillUv({c.damaged}, c.hole, {}, &mesh, true, o).front();
    const image::Image isles = tex::patchFillUv({c.damaged}, c.hole, {}, &mesh, false, o).front();

    const double seamLeft = sideError(c, seams, 0), seamRight = sideError(c, seams, 1);
    const double isleLeft = sideError(c, isles, 0), isleRight = sideError(c, isles, 1);
    MESSAGE("across seams " << seamLeft << " / " << seamRight << ", island by island " << isleLeft << " / " << isleRight);
    CHECK(seamLeft < 0.08);
    CHECK(seamRight < 0.08);
    CHECK(seamLeft + seamRight < 0.5 * (isleLeft + isleRight));

    // Nothing outside the hole changes.
    for (size_t i = 0; i < c.hole.size(); ++i)
        if (!c.hole[i])
            for (int k = 0; k < 4; ++k)
                REQUIRE(seams.rgba[i * 4 + size_t(k)] == c.damaged.rgba[i * 4 + size_t(k)]);
}
