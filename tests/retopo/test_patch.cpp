#include "geo/Bvh.h"
#include "geo/Primitives.h"
#include "retopo/Patch.h"

#include <doctest/doctest.h>

#include <glm/gtc/constants.hpp>

#include <algorithm>
#include <cmath>
#include <map>
#include <numeric>
#include <set>
#include <string>

using namespace lila;
using geo::Index;

namespace
{
/** Points on a circle in XY, corners evenly spaced and each side's points evenly spaced between them. */
retopo::PatchBoundary circleBoundary(const std::vector<int>& counts, double radius = 10.0)
{
    retopo::PatchBoundary b;
    const double step = glm::two_pi<double>() / double(counts.size());
    for (size_t i = 0; i < counts.size(); ++i)
    {
        b.corners.push_back(int(b.points.size()));
        for (int k = 0; k < counts[i]; ++k)
        {
            const double a = step * (double(i) + double(k) / counts[i]);
            b.points.emplace_back(radius * std::cos(a), radius * std::sin(a), 0.0);
        }
    }
    return b;
}

std::string describe(const std::vector<int>& counts)
{
    std::string s;
    for (int c : counts)
        s += std::to_string(c) + " ";
    return s;
}

/** Checks the fill is an oriented all-quad disk over the boundary, with poles that match the valences. */
void checkPatch(const retopo::PatchBoundary& b, const retopo::PatchResult& r)
{
    REQUIRE(r.ok());
    const Index n = Index(b.points.size());
    const Index count = Index(r.positions.size());
    REQUIRE(count >= n);
    for (Index i = 0; i < n; ++i)
        CHECK(r.positions[i] == b.points[i]);
    CHECK(r.variants >= 1);
    CHECK(r.variant >= 0);
    CHECK(r.variant < r.variants);

    std::map<std::pair<Index, Index>, int> directed;
    std::vector<int> valence(count, 0);
    for (const auto& f : r.faces)
    {
        REQUIRE(f.size() == 4);
        CHECK(std::set<Index>(f.begin(), f.end()).size() == 4);
        for (size_t i = 0; i < 4; ++i)
        {
            REQUIRE(f[i] < count);
            ++valence[f[i]];
            ++directed[{f[i], f[(i + 1) % 4]}];
        }
    }
    for (Index i = 0; i < n; ++i)
    {
        const Index j = (i + 1) % n;
        CHECK(directed[{i, j}] == 1);
        CHECK(directed.count({j, i}) == 0);
    }
    std::set<std::pair<Index, Index>> edges;
    std::vector<int> degree(count, 0);
    for (const auto& [e, c] : directed)
    {
        CHECK(c == 1);
        const bool border = e.first < n && e.second < n && (e.second == (e.first + 1) % n || e.first == (e.second + 1) % n);
        if (!border)
            CHECK(directed.count({e.second, e.first}) == 1);
        if (edges.insert({std::min(e.first, e.second), std::max(e.first, e.second)}).second)
        {
            ++degree[e.first];
            ++degree[e.second];
        }
    }
    for (Index v = 0; v < count; ++v)
        CHECK(valence[v] > 0);
    CHECK(int(count) - int(edges.size()) + int(r.faces.size()) == 1);

    std::vector<Index> poles;
    for (Index v = n; v < count; ++v)
    {
        CHECK(degree[v] == valence[v]);
        if (valence[v] != 4)
            poles.push_back(v);
    }
    CHECK(poles == r.poles);
}

/** True when every quad faces the same way as the planar boundary loop. */
bool facesAgreeWithLoop(const retopo::PatchBoundary& b, const retopo::PatchResult& r)
{
    glm::dvec3 loopNormal(0.0);
    for (size_t i = 0; i < b.points.size(); ++i)
        loopNormal += glm::cross(b.points[i], b.points[(i + 1) % b.points.size()]);
    for (const auto& f : r.faces)
    {
        const glm::dvec3 nf = glm::cross(r.positions[f[2]] - r.positions[f[0]], r.positions[f[3]] - r.positions[f[1]]);
        if (glm::dot(nf, loopNormal) <= 0.0)
            return false;
    }
    return true;
}

glm::dvec3 poleCentre(const retopo::PatchResult& r)
{
    glm::dvec3 c(0.0);
    for (Index p : r.poles)
        c += r.positions[p];
    return r.poles.empty() ? c : c / double(r.poles.size());
}
/** Quads with interior points renumbered in the order a walk from the first boundary edge meets them: equal for equal meshes. */
std::vector<Index> canonicalFaces(const retopo::PatchResult& r, Index n)
{
    std::map<std::pair<Index, Index>, std::pair<size_t, int>> owner;
    for (size_t f = 0; f < r.faces.size(); ++f)
        for (int k = 0; k < 4; ++k)
            owner[{r.faces[f][k], r.faces[f][(k + 1) % 4]}] = {f, k};
    std::vector<Index> label(r.positions.size(), geo::kInvalid);
    for (Index v = 0; v < n; ++v)
        label[v] = v;
    Index next = n;
    std::vector<uint8_t> seen(r.faces.size(), 0);
    std::vector<std::pair<size_t, int>> queue = {owner.at({0, 1})};
    seen[queue[0].first] = 1;
    std::vector<Index> out;
    for (size_t q = 0; q < queue.size(); ++q)
    {
        const auto [f, start] = queue[q];
        const auto& face = r.faces[f];
        for (int k = 0; k < 4; ++k)
        {
            Index& l = label[face[(start + k) % 4]];
            if (l == geo::kInvalid)
                l = next++;
            out.push_back(l);
        }
        for (int k = 0; k < 4; ++k)
            if (auto t = owner.find({face[(start + k + 1) % 4], face[(start + k) % 4]}); t != owner.end() && !seen[t->second.first])
            {
                seen[t->second.first] = 1;
                queue.push_back(t->second);
            }
    }
    return out;
}
} // namespace

TEST_CASE("patch: side counts and fillability")
{
    retopo::PatchBoundary b = circleBoundary({3, 9, 5, 7});
    CHECK(retopo::patchSideCounts(b) == std::vector<int>{3, 9, 5, 7});
    b.corners = {2, 5, 20};
    CHECK(retopo::patchSideCounts(b) == std::vector<int>{3, 15, 6});
    b.corners = {5, 2};
    CHECK(retopo::patchSideCounts(b).empty());

    CHECK(retopo::patchSidesFillable({4, 4, 4, 4}));
    CHECK(retopo::patchSidesFillable({1, 1, 1, 1}));
    CHECK(retopo::patchSidesFillable({2, 2, 10}));
    CHECK_FALSE(retopo::patchSidesFillable({3, 4, 4}));
    CHECK_FALSE(retopo::patchSidesFillable({1, 1}));
    CHECK_FALSE(retopo::patchSidesFillable({4}));
    CHECK_FALSE(retopo::patchSidesFillable({1, 1, 1, 1, 1, 1, 2}));
    CHECK_FALSE(retopo::patchSidesFillable({0, 4, 4, 4}));
}

TEST_CASE("patch: bad input comes back as an error")
{
    retopo::PatchBoundary b;
    b.points = {glm::dvec3(0.0), glm::dvec3(1, 0, 0)};
    b.corners = {0, 1};
    CHECK_FALSE(retopo::fillPatch(b).ok());

    b = circleBoundary({4, 4, 4, 4});
    b.corners = {0, 4, 8, 40};
    CHECK_FALSE(retopo::fillPatch(b).ok());
    b.corners = {0, 8, 4, 12};
    CHECK_FALSE(retopo::fillPatch(b).ok());
    b.corners = {0};
    CHECK_FALSE(retopo::fillPatch(b).ok());
    b.corners = {0, 1, 2, 3, 4, 5, 6};
    CHECK_FALSE(retopo::fillPatch(b).ok());
    b.corners = {-1, 4, 8};
    CHECK_FALSE(retopo::fillPatch(b).ok());

    b = circleBoundary({3, 4, 4});
    const retopo::PatchResult odd = retopo::fillPatch(b);
    CHECK_FALSE(odd.ok());
    CHECK(odd.faces.empty());
}

TEST_CASE("patch: hand-picked boundaries fill cleanly")
{
    const std::vector<std::vector<int>> cases = {
        {4, 4, 4, 4}, {2, 10, 2, 10}, {3, 9, 5, 7}, {1, 3, 1, 3}, {1, 1, 1, 1}, {2, 2, 2, 10}, {6, 4, 2, 4}, {2, 2, 10, 10},
        {4, 4, 4}, {6, 6, 6}, {2, 4, 8}, {2, 2, 10}, {1, 1, 2}, {3, 3, 2}, {12, 3, 5},
        {5, 5}, {1, 5}, {2, 2}, {3, 7}, {10, 2}, {1, 3},
        {2, 2, 2, 2, 2}, {4, 3, 5, 2, 4}, {1, 1, 1, 1, 2}, {9, 2, 3, 2, 2},
        {1, 1, 1, 1, 1, 1}, {3, 3, 3, 3, 3, 3}, {5, 1, 4, 2, 6, 2}, {2, 2, 2, 2, 2, 2},
        {40, 40, 40, 40}, {30, 50, 20, 60}, {25, 25, 30}, {60, 2, 2, 2},
        {120, 80, 100, 64}, {90, 3, 5, 7, 11, 2}, {70, 60, 50, 40, 30, 20}, {150, 150},
    };
    for (const auto& counts : cases)
    {
        CAPTURE(describe(counts));
        const retopo::PatchBoundary b = circleBoundary(counts);
        CHECK(retopo::patchSidesFillable(counts));
        const retopo::PatchResult r = retopo::fillPatch(b);
        checkPatch(b, r);
        CHECK(facesAgreeWithLoop(b, r));
    }
}

TEST_CASE("patch: even patches keep their regular layouts")
{
    retopo::PatchResult grid = retopo::fillPatch(circleBoundary({4, 6, 4, 6}));
    REQUIRE(grid.ok());
    CHECK(grid.poles.empty());
    CHECK(grid.variant == 0);
    CHECK(grid.faces.size() == 24);

    retopo::PatchResult tri = retopo::fillPatch(circleBoundary({4, 4, 4}));
    REQUIRE(tri.ok());
    REQUIRE(tri.poles.size() == 1);
    CHECK(tri.faces.size() == 12);

    retopo::PatchResult penta = retopo::fillPatch(circleBoundary({4, 4, 4, 4, 4}));
    REQUIRE(penta.ok());
    CHECK(penta.poles.size() == 1);

    // Unequal opposite sides need a 3-5 pair.
    retopo::PatchResult pair = retopo::fillPatch(circleBoundary({6, 4, 4, 6}));
    REQUIRE(pair.ok());
    CHECK(pair.poles.size() == 2);
}

TEST_CASE("patch: corners keep a single quad when a pattern fits them")
{
    const std::vector<std::vector<int>> cases = {
        {4, 4, 4, 4}, {2, 10, 2, 10}, {3, 9, 5, 7}, {2, 2, 10, 10}, {30, 50, 20, 60}, {4, 4, 4}, {6, 6, 6}, {25, 25, 30},
        {5, 5}, {3, 7}, {10, 2 + 10}, {2, 2, 2, 2, 2}, {4, 3, 5, 2, 4}, {3, 3, 3, 3, 3, 3},
    };
    for (const auto& counts : cases)
    {
        CAPTURE(describe(counts));
        const retopo::PatchBoundary b = circleBoundary(counts);
        const retopo::PatchResult r = retopo::fillPatch(b);
        checkPatch(b, r);
        std::map<Index, int> uses;
        for (const auto& f : r.faces)
            for (Index v : f)
                ++uses[v];
        for (int c : b.corners)
            CHECK(uses[Index(c)] == 1);
    }
}

TEST_CASE("patch: variants are distinct meshes")
{
    const std::vector<std::vector<int>> cases = {
        {8, 8}, {5, 9}, {8, 8, 8}, {4, 6, 10}, {10, 6, 6, 10}, {6, 8, 6, 8}, {12, 10, 8, 14}, {4, 4, 4, 4, 4}, {3, 5, 4, 6, 2},
        {3, 3, 3, 3, 3, 3}, {6, 2, 4, 4, 2, 6}, {2, 2, 10}, {1, 5},
    };
    for (const auto& counts : cases)
    {
        CAPTURE(describe(counts));
        const retopo::PatchBoundary b = circleBoundary(counts);
        const int variants = retopo::fillPatch(b).variants;
        std::set<std::vector<Index>> meshes;
        for (int v = 0; v < variants; ++v)
        {
            retopo::PatchOptions opts;
            opts.variant = v;
            const retopo::PatchResult r = retopo::fillPatch(b, opts);
            REQUIRE(r.ok());
            CHECK(meshes.insert(canonicalFaces(r, Index(b.points.size()))).second);
        }
    }
}

TEST_CASE("patch: every small boundary, padding and variant fills or errors cleanly")
{
    int filled = 0;
    for (int sides = 2; sides <= 6; ++sides)
    {
        std::vector<int> counts(sides, 1);
        while (true)
        {
            const int total = std::accumulate(counts.begin(), counts.end(), 0);
            const retopo::PatchBoundary b = circleBoundary(counts);
            for (int padding : {-1, 0, 1})
                for (int variant : {0, 1, 5, -3})
                {
                    CAPTURE(describe(counts));
                    CAPTURE(padding);
                    CAPTURE(variant);
                    retopo::PatchOptions opts;
                    opts.padding = padding;
                    opts.variant = variant;
                    const retopo::PatchResult r = retopo::fillPatch(b, opts);
                    if (total < 3)
                    {
                        CHECK_FALSE(r.ok());
                        continue;
                    }
                    CHECK(r.ok() == retopo::patchSidesFillable(counts));
                    if (r.ok())
                    {
                        checkPatch(b, r);
                        ++filled;
                    }
                }
            int i = 0;
            while (i < sides && ++counts[i] > 4)
                counts[i++] = 1;
            if (i == sides)
                break;
        }
    }
    CHECK(filled > 10000);
}

TEST_CASE("patch: padding rings keep the pattern inside")
{
    const retopo::PatchBoundary b = circleBoundary({8, 8, 8});
    retopo::PatchOptions opts;
    opts.padding = 2;
    const retopo::PatchResult padded = retopo::fillPatch(b, opts);
    checkPatch(b, padded);
    opts.padding = 0;
    const retopo::PatchResult bare = retopo::fillPatch(b, opts);
    checkPatch(b, bare);
    CHECK(padded.poles.size() == bare.poles.size());
    CHECK(padded.faces.size() == bare.faces.size());

    // No boundary point touches a pole when two rings separate them.
    std::set<Index> nearBorder;
    for (const auto& f : padded.faces)
        for (Index v : f)
            if (v < b.points.size())
                nearBorder.insert(f.begin(), f.end());
    for (Index p : padded.poles)
        CHECK(nearBorder.count(p) == 0);

    // Far more padding than fits falls back to as much as does.
    opts.padding = 50;
    checkPatch(b, retopo::fillPatch(b, opts));
}

TEST_CASE("patch: variants cycle and wrap")
{
    const retopo::PatchBoundary b = circleBoundary({8, 8, 8});
    const retopo::PatchResult first = retopo::fillPatch(b);
    REQUIRE(first.ok());
    REQUIRE(first.variants > 1);
    retopo::PatchOptions opts;
    opts.variant = 1;
    const retopo::PatchResult second = retopo::fillPatch(b, opts);
    checkPatch(b, second);
    CHECK(second.variant == 1);
    CHECK(second.positions != first.positions);
    opts.variant = first.variants + 1;
    CHECK(retopo::fillPatch(b, opts).variant == 1);
    opts.variant = -1;
    CHECK(retopo::fillPatch(b, opts).variant == first.variants - 1);
}

TEST_CASE("patch: pull picks the variant with poles nearest it and leans toward it")
{
    // Opposite sides off by the same amount both ways: a 3-5 pair that can sit anywhere along the patch.
    const retopo::PatchBoundary b = circleBoundary({10, 6, 6, 10});
    const retopo::PatchResult plain = retopo::fillPatch(b);
    REQUIRE(plain.ok());
    REQUIRE(plain.poles.size() == 2);
    REQUIRE(plain.variants > 4);

    int moved = 0;
    for (const glm::dvec3 target : {glm::dvec3(6, 2, 0), glm::dvec3(-5, 3, 0), glm::dvec3(0, -6, 0), glm::dvec3(2, 6, 0)})
    {
        CAPTURE(target.x);
        CAPTURE(target.y);
        retopo::PatchOptions opts;
        opts.pull = target;
        const retopo::PatchResult pulled = retopo::fillPatch(b, opts);
        checkPatch(b, pulled);
        CHECK(facesAgreeWithLoop(b, pulled));
        CHECK(pulled.poles.size() == plain.poles.size());
        CHECK(glm::length(poleCentre(pulled) - target) < glm::length(poleCentre(plain) - target));
        moved += pulled.variant != plain.variant;
    }
    CHECK(moved >= 3);

    // Large patches judge the variants on a scaled-down copy; the pick still has to land near the pull.
    const retopo::PatchBoundary big = circleBoundary({40, 24, 24, 40});
    const retopo::PatchResult bigPlain = retopo::fillPatch(big);
    REQUIRE(bigPlain.poles.size() == 2);
    for (const glm::dvec3 target : {glm::dvec3(6, -2, 0), glm::dvec3(-4, 5, 0)})
    {
        retopo::PatchOptions opts;
        opts.pull = target;
        const retopo::PatchResult pulled = retopo::fillPatch(big, opts);
        checkPatch(big, pulled);
        CHECK(pulled.variant != bigPlain.variant);
        CHECK(glm::length(poleCentre(pulled) - target) < glm::length(poleCentre(bigPlain) - target) * 0.75);
    }

    // A triangle's single pole has one place, so the pull only leans the interior.
    const retopo::PatchBoundary tri = circleBoundary({8, 8, 8});
    const retopo::PatchResult centred = retopo::fillPatch(tri);
    REQUIRE(centred.poles.size() == 1);
    for (int corner = 0; corner < 3; ++corner)
    {
        CAPTURE(corner);
        const glm::dvec3 target = tri.points[tri.corners[corner]] * 0.6;
        retopo::PatchOptions opts;
        opts.pull = target;
        const retopo::PatchResult pulled = retopo::fillPatch(tri, opts);
        checkPatch(tri, pulled);
        CHECK(facesAgreeWithLoop(tri, pulled));
        CHECK(glm::length(poleCentre(pulled) - target) < glm::length(poleCentre(centred) - target) * 0.75);
    }

    // A grid has nothing to choose between, so the interior just leans toward the pull.
    const retopo::PatchBoundary square = circleBoundary({6, 6, 6, 6});
    const retopo::PatchResult flat = retopo::fillPatch(square);
    REQUIRE(flat.ok());
    retopo::PatchOptions opts;
    opts.pull = glm::dvec3(3.0, 3.0, 0.0);
    const retopo::PatchResult leaned = retopo::fillPatch(square, opts);
    checkPatch(square, leaned);
    CHECK(facesAgreeWithLoop(square, leaned));
    REQUIRE(flat.positions.size() == leaned.positions.size());
    double before = 1e300, after = 1e300;
    for (size_t v = square.points.size(); v < flat.positions.size(); ++v)
    {
        before = std::min(before, glm::length(flat.positions[v] - *opts.pull));
        after = std::min(after, glm::length(leaned.positions[v] - *opts.pull));
    }
    CHECK(after < before);

    // A pull far outside the patch still leaves it unfolded.
    opts.pull = glm::dvec3(500.0, -200.0, 0.0);
    const retopo::PatchResult wild = retopo::fillPatch(b, opts);
    checkPatch(b, wild);
    CHECK(facesAgreeWithLoop(b, wild));
}

TEST_CASE("patch: a surface keeps the interior on it")
{
    geo::SphereOptions so;
    so.radius = 50.0;
    so.subdivAxis = 96;
    so.subdivHeight = 96;
    const geo::Mesh sphere = geo::makeSphere(so);
    const geo::MeshBvh bvh(sphere);

    // A ring of latitude near the top, split into four sides.
    retopo::PatchBoundary b;
    const double polar = 0.7;
    const int perSide = 7;
    for (int i = 0; i < 4 * perSide; ++i)
    {
        if (i % perSide == 0)
            b.corners.push_back(i);
        const double a = glm::two_pi<double>() * i / (4.0 * perSide);
        b.points.emplace_back(so.radius * std::sin(polar) * std::cos(a), so.radius * std::cos(polar),
                              so.radius * std::sin(polar) * std::sin(a));
    }
    for (bool pulled : {false, true})
    {
        CAPTURE(pulled);
        retopo::PatchOptions opts;
        opts.relaxIterations = 15;
        if (pulled)
            opts.pull = glm::dvec3(10.0, 48.0, 0.0);
        const retopo::PatchResult r = retopo::fillPatch(b, opts, &bvh);
        checkPatch(b, r);
        int sign = 0;
        for (size_t v = b.points.size(); v < r.positions.size(); ++v)
            CHECK(std::abs(glm::length(r.positions[v]) - so.radius) < 0.25);
        for (const auto& f : r.faces)
        {
            const glm::dvec3 nf = glm::cross(r.positions[f[2]] - r.positions[f[0]], r.positions[f[3]] - r.positions[f[1]]);
            const glm::dvec3 c = (r.positions[f[0]] + r.positions[f[1]] + r.positions[f[2]] + r.positions[f[3]]) * 0.25;
            const int s = glm::dot(nf, c) > 0.0 ? 1 : -1;
            if (sign == 0)
                sign = s;
            CHECK(s == sign);
        }
    }
}

TEST_CASE("patch: fills are deterministic")
{
    const retopo::PatchBoundary b = circleBoundary({5, 9, 3, 7, 4});
    retopo::PatchOptions opts;
    opts.pull = glm::dvec3(2.0, 1.0, 0.0);
    const retopo::PatchResult a = retopo::fillPatch(b, opts), c = retopo::fillPatch(b, opts);
    REQUIRE(a.ok());
    CHECK(a.positions == c.positions);
    CHECK(a.faces == c.faces);
    CHECK(a.variant == c.variant);
}

TEST_CASE("patch: corners from a loop")
{
    // A 4 x 2 rectangle, points every unit.
    std::vector<glm::dvec3> rect;
    for (int x = 0; x < 4; ++x)
        rect.emplace_back(x, 0, 0);
    for (int y = 0; y < 2; ++y)
        rect.emplace_back(4, y, 0);
    for (int x = 4; x > 0; --x)
        rect.emplace_back(x, 2, 0);
    for (int y = 2; y > 0; --y)
        rect.emplace_back(0, y, 0);
    CHECK(retopo::patchCorners(rect) == std::vector<int>{0, 4, 6, 10});
    CHECK(retopo::patchCorners(rect, 3).size() == 3);
    const std::vector<int> six = retopo::patchCorners(rect, 6);
    REQUIRE(six.size() == 6);
    CHECK(std::is_sorted(six.begin(), six.end()));
    for (int c : {0, 4, 6, 10})
        CHECK(std::find(six.begin(), six.end(), c) != six.end());

    // A smooth circle has no sharp turns, so corners spread evenly.
    std::vector<glm::dvec3> circle;
    for (int i = 0; i < 40; ++i)
        circle.emplace_back(std::cos(i * glm::two_pi<double>() / 40), std::sin(i * glm::two_pi<double>() / 40), 0.0);
    const std::vector<int> spread = retopo::patchCorners(circle, 4);
    REQUIRE(spread.size() == 4);
    for (int i = 0; i < 4; ++i)
        CHECK((spread[(i + 1) % 4] - spread[i] + 40) % 40 == 10);

    CHECK(retopo::patchCorners({glm::dvec3(0.0), glm::dvec3(1, 0, 0)}, 4).size() == 2);
    CHECK(retopo::patchCorners({}, 4).empty());

    // Corners found on a loop fill straight away.
    retopo::PatchBoundary b;
    b.points = rect;
    b.corners = retopo::patchCorners(rect);
    const retopo::PatchResult r = retopo::fillPatch(b);
    checkPatch(b, r);
    CHECK(r.poles.empty());
}

TEST_CASE("patch: a cap lifts onto the dome instead of flattening across the ring")
{
    geo::SphereOptions so;
    so.radius = 10.0;
    so.subdivAxis = 64;
    so.subdivHeight = 64;
    geo::Mesh sphere = geo::makeSphere(so);
    geo::MeshBvh bvh;
    bvh.build(sphere);
    // A ring of 16 points round the sphere at y = 6, its cap bulging along +Y like a fingertip.
    retopo::PatchBoundary b;
    const double y = 6.0, r = std::sqrt(100.0 - y * y);
    for (int i = 0; i < 16; ++i)
    {
        const double a = i / 16.0 * glm::two_pi<double>();
        b.points.push_back({r * std::cos(a), y, r * std::sin(a)});
    }
    b.corners = {0, 4, 8, 12};
    retopo::PatchOptions o;
    o.lift = glm::dvec3(0.0, 1.0, 0.0);
    retopo::PatchResult res = retopo::fillPatch(b, o, &bvh);
    REQUIRE(res.ok());
    double highest = 0.0;
    for (size_t i = b.points.size(); i < res.positions.size(); ++i)
    {
        CHECK(glm::length(res.positions[i]) == doctest::Approx(10.0).epsilon(0.02));
        CHECK(res.positions[i].y > y);
        highest = std::max(highest, res.positions[i].y);
    }
    CHECK(highest > 9.0);
}
