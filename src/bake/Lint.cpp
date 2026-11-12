#include "bake/Lint.h"

#include "bake/Backend.h"
#include "bake/BakeCommon.h"
#include "bake/BakeInputs.h"
#include "bake/BakeNodes.h"
#include "bake/Cage.h"
#include "bake/Naming.h"
#include "core/Profile.h"
#include "geo/Bvh.h"
#include "geo/Mesh.h"
#include "geo/Triangulate.h"
#include "scene/Graph.h"
#include "scene/MeshNodes.h"
#include "scene/Modifiers.h"

#include <tbb/parallel_for.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <iterator>
#include <map>
#include <set>
#include <tuple>

namespace lila::bake
{
namespace
{
using detail::exportedMesh;
using detail::shortName;

/** Component ids an issue lists for selecting them; the count it reports has no cap. */
constexpr size_t kMaxComponents = 100000;
/** Under a texel of UV area a triangle's winding and tangent sign don't show in the bake. */
constexpr double kMinTexels = 1.0;
/** Texels an island must have wound against the rest before it counts as folded: smaller folds vanish under padding. */
constexpr double kFoldTexels = 16.0;
/** A collapsed triangle smaller than this at its island's density is too small to see. */
constexpr double kVisibleTexels = 16.0;
/** UV area, as a fraction of what its island's density would give it, under which a triangle counts as collapsed. */
constexpr double kCollapsedFraction = 0.02;

bool named(const LintOptions& o, const char* check) { return std::ranges::find(o.checks, check) != o.checks.end(); }
bool wanted(const LintOptions& o, const char* check) { return o.checks.empty() || named(o, check); }

double uvArea(const glm::vec2* uv) { return 0.5 * (double(uv[1].x - uv[0].x) * (uv[2].y - uv[0].y) - double(uv[2].x - uv[0].x) * (uv[1].y - uv[0].y)); }
double area3(const glm::vec3* p) { return 0.5 * double(glm::length(glm::cross(p[1] - p[0], p[2] - p[0]))); }

void hardEdges(const GroupInputs& gi, const LowInput& low, LintReport& r)
{
    const MeshSoup& s = low.soup;
    if (s.uvs.empty() || !s.triangleCount())
        return;
    const double q = std::max(1e-6, double(glm::length(s.boundsMax - s.boundsMin)) * 1e-6);
    auto key = [&](const glm::vec3& p) { return std::array<int64_t, 3>{int64_t(std::llround(p.x / q)), int64_t(std::llround(p.y / q)), int64_t(std::llround(p.z / q))}; };
    // Each triangle side as its undirected edge key and its corners (a, b), sorted so the sides on one edge are adjacent.
    struct Side
    {
        std::array<int64_t, 3> ka, kb;
        size_t a, b;
    };
    std::vector<Side> sides;
    sides.reserve(s.triangleCount() * 3);
    for (size_t t = 0; t < s.triangleCount(); ++t)
        for (size_t k = 0; k < 3; ++k)
        {
            size_t a = t * 3 + k, b = t * 3 + (k + 1) % 3;
            auto ka = key(s.positions[a]), kb = key(s.positions[b]);
            if (kb < ka)
            {
                std::swap(ka, kb);
                std::swap(a, b);
            }
            sides.push_back({ka, kb, a, b});
        }
    std::ranges::sort(sides, [](const Side& x, const Side& y) { return std::tie(x.ka, x.kb, x.a) < std::tie(y.ka, y.kb, y.a); });
    const float cosHard = std::cos(glm::radians(1.0f));
    auto split = [&](size_t c0, size_t c1) {
        const glm::vec3 n0 = glm::normalize(s.normals[c0]), n1 = glm::normalize(s.normals[c1]);
        return glm::dot(n0, n1) < cosHard;
    };
    auto seam = [&](size_t c0, size_t c1) { return glm::any(glm::greaterThan(glm::abs(s.uvs[c0] - s.uvs[c1]), glm::vec2(1e-5f))); };
    size_t count = 0;
    json where = json::array();
    json edgePoints = json::array();
    for (size_t i = 0; i < sides.size();)
    {
        size_t j = i + 1;
        while (j < sides.size() && sides[j].ka == sides[i].ka && sides[j].kb == sides[i].kb)
            ++j;
        const size_t first = i;
        const bool manifold = j - i == 2;
        i = j;
        if (!manifold)
            continue;
        const size_t a0 = sides[first].a, b0 = sides[first].b, a1 = sides[first + 1].a, b1 = sides[first + 1].b;
        if (!(split(a0, a1) || split(b0, b1)) || seam(a0, a1) || seam(b0, b1))
            continue;
        ++count;
        if (a0 < low.points.size() && b0 < low.points.size() && edgePoints.size() < kMaxComponents)
            edgePoints.push_back({low.points[a0], low.points[b0]});
        if (where.size() < 8)
        {
            const glm::vec3 m = (s.positions[a0] + s.positions[b0]) * 0.5f;
            where.push_back({m.x, m.y, m.z});
        }
    }
    if (count)
        r.issues.push_back({"hardEdges", "warning", gi.name, s.name,
                            std::to_string(count) + " hard edge(s) are not UV seams; the normal map can't hide their shading split. Split the UVs there, or soften them with Surface > Soften Normals",
                            {{"count", count}, {"positions", where}, {"edgePoints", edgePoints}}});
}

/** Texels per cm2 each island of a low gets; an island with no UV area takes the low's overall density. */
std::vector<double> islandDensities(const LowInput& low, double texels)
{
    const MeshSoup& s = low.soup;
    std::vector<double> uvA(low.islandCount, 0.0), worldA(low.islandCount, 0.0);
    double uvAll = 0.0, worldAll = 0.0;
    for (size_t t = 0; t < s.triangleCount(); ++t)
    {
        const double u = std::abs(uvArea(&s.uvs[t * 3])), w = area3(&s.positions[t * 3]);
        uvAll += u;
        worldAll += w;
        if (t < low.islands.size() && low.islands[t] < low.islandCount)
        {
            uvA[low.islands[t]] += u;
            worldA[low.islands[t]] += w;
        }
    }
    const double overall = worldAll > 0.0 ? uvAll * texels / worldAll : 0.0;
    std::vector<double> out(low.islandCount, overall);
    for (uint32_t i = 0; i < low.islandCount; ++i)
        if (worldA[i] > 0.0 && uvA[i] * texels >= kMinTexels)
            out[i] = uvA[i] * texels / worldA[i];
    return out;
}

void uvMissing(const GroupInputs& gi, const LowInput& low, const ProjectSettings& ps, LintReport& r)
{
    const MeshSoup& s = low.soup;
    if (s.uvs.empty())
    {
        r.issues.push_back({"uvMissing", "warning", gi.name, s.name, "has no uv0; it bakes nothing", json::object()});
        return;
    }
    const double texels = double(ps.output.width) * ps.output.height;
    double uvAll = 0.0;
    for (size_t t = 0; t < s.triangleCount(); ++t)
        uvAll += std::abs(uvArea(&s.uvs[t * 3]));
    if (uvAll * texels < kMinTexels)
    {
        r.issues.push_back({"uvMissing", "warning", gi.name, s.name, "its uv0 has no area; it bakes nothing", json::object()});
        return;
    }
    const std::vector<double> density = islandDensities(low, texels);
    size_t collapsed = 0;
    std::set<uint32_t> faces;
    for (size_t t = 0; t < s.triangleCount(); ++t)
    {
        const uint32_t island = t < low.islands.size() ? low.islands[t] : 0u;
        const double expected = area3(&s.positions[t * 3]) * (island < density.size() ? density[island] : 0.0);
        // Only a triangle that would cover enough texels to show, squeezed to a sliver of them, smears in the bake.
        if (expected >= kVisibleTexels && std::abs(uvArea(&s.uvs[t * 3])) * texels < expected * kCollapsedFraction)
        {
            ++collapsed;
            if (t < low.faces.size() && faces.size() < kMaxComponents)
                faces.insert(low.faces[t]);
        }
    }
    if (collapsed)
        r.issues.push_back({"uvMissing", "warning", gi.name, s.name,
                            std::to_string(collapsed) + " triangle(s) are collapsed in UV; their texels smear into a line",
                            {{"count", collapsed}, {"faces", faces}}});
}

/** Faces (by face id) whose triangles of a texel or more are wound both ways in UV: the triangulation folds them. */
std::set<uint32_t> foldedFaces(const LowInput& low, double texels)
{
    std::map<uint32_t, std::array<bool, 2>> wound;
    for (size_t t = 0; t < low.soup.triangleCount() && t < low.faces.size(); ++t)
        if (const double a = uvArea(&low.soup.uvs[t * 3]); std::abs(a) * texels >= kMinTexels)
            wound[low.faces[t]][a < 0.0] = true;
    std::set<uint32_t> out;
    for (const auto& [f, w] : wound)
        if (w[0] && w[1])
            out.insert(f);
    return out;
}

void mirroredTangents(const GroupInputs& gi, const LowInput& low, const ProjectSettings& ps, LintReport& r)
{
    const MeshSoup& s = low.soup;
    if (s.uvs.empty() || low.tangents.size() != s.positions.size())
        return;
    const double texels = double(ps.output.width) * ps.output.height;
    const std::set<uint32_t> splitFolds = foldedFaces(low, texels);
    auto inSplitFold = [&](size_t t) { return t < low.faces.size() && splitFolds.contains(low.faces[t]); };
    size_t mismatched = 0, mismatchedInSplits = 0, mirrored = 0;
    std::set<uint32_t> faces;
    // Per island: texels wound each way, to find islands folding over themselves, with and without the split-folded faces.
    std::vector<std::array<double, 2>> winding(low.islandCount), windingOutsideSplits(low.islandCount);
    for (size_t t = 0; t < s.triangleCount(); ++t)
    {
        const double a = uvArea(&s.uvs[t * 3]);
        // Slivers under a texel have no reliable winding and MikkTSpace takes their tangents from neighbours.
        if (std::abs(a) * texels < kMinTexels)
            continue;
        const bool flipped = a < 0.0;
        mirrored += flipped;
        if (t < low.islands.size() && low.islands[t] < winding.size())
        {
            winding[low.islands[t]][flipped] += std::abs(a) * texels;
            if (!inSplitFold(t))
                windingOutsideSplits[low.islands[t]][flipped] += std::abs(a) * texels;
        }
        // A small triangle's sign disagrees with a neighbouring sliver's through their shared corner; it doesn't show.
        if (std::abs(a) * texels < kVisibleTexels)
            continue;
        for (int k = 0; k < 3; ++k)
            if ((low.tangents[t * 3 + size_t(k)].w < 0.0f) != flipped)
            {
                ++mismatched;
                mismatchedInSplits += inSplitFold(t);
                if (t < low.faces.size() && faces.size() < kMaxComponents)
                    faces.insert(low.faces[t]);
                break;
            }
    }
    size_t folded = 0, foldedOutsideSplits = 0;
    for (size_t i = 0; i < winding.size(); ++i)
    {
        folded += std::min(winding[i][0], winding[i][1]) >= kFoldTexels;
        foldedOutsideSplits += std::min(windingOutsideSplits[i][0], windingOutsideSplits[i][1]) >= kFoldTexels;
    }
    // Every problem inside faces whose triangulation folds them: a concave UV face cut across its inside corner.
    if ((mismatched || folded) && mismatchedInSplits == mismatched && !foldedOutsideSplits)
    {
        r.issues.push_back({"mirroredTangents", "warning", gi.name, s.name,
                            std::to_string(splitFolds.size()) +
                                " face(s) fold in UV only where the export triangulation splits them (a concave UV face cut across its inside corner): "
                                "move that UV corner out or split the face",
                            {{"count", mismatched}, {"splitFaces", splitFolds.size()}, {"faces", splitFolds}}});
        return;
    }
    if (mismatched)
        r.issues.push_back({"mirroredTangents", "warning", gi.name, s.name,
                            std::to_string(mismatched) + " triangle(s) have a tangent sign that disagrees with their UV winding; mirrored UVs will decode inverted",
                            {{"count", mismatched}, {"mirroredTriangles", mirrored}, {"faces", faces}}});
    if (folded)
        r.issues.push_back({"mirroredTangents", "warning", gi.name, s.name,
                            std::to_string(folded) + " UV island(s) fold over themselves (triangles wound both ways); their texels bake twice",
                            {{"islands", folded}}});
}

/** One rasterised UV layout per texture file and UDIM, owner per cell: (low index, island) + 1. */
void uvOverlap(const BakeInputs& in, const ProjectSettings& ps, LintReport& r)
{
    LILA_ZONE();

    const int R = std::clamp(std::min(ps.output.width, ps.output.height), 64, 1024);
    struct Owner
    {
        size_t group, low;
    };
    std::vector<Owner> lows;
    std::map<std::pair<std::string, int>, std::vector<uint64_t>> grids;
    // Cells two owners claim, with the pair; only those whose neighbours are contested too count, so islands packed edge
    // to edge or overlapping by less than a cell, which padding covers, aren't reported.
    std::map<std::pair<std::string, int>, std::vector<uint8_t>> contested;
    std::vector<std::tuple<std::vector<uint8_t>*, size_t, std::pair<uint64_t, uint64_t>>> claims;
    for (size_t g = 0; g < in.groups.size(); ++g)
        for (size_t l = 0; l < in.groups[g].lows.size(); ++l)
        {
            const LowInput& low = in.groups[g].lows[l];
            const MeshSoup& s = low.soup;
            if (s.uvs.empty())
                continue;
            const uint64_t lowIndex = lows.size();
            lows.push_back({g, l});
            const std::vector<glm::ivec2> tiles = triangleTiles(low);
            // The texture a triangle writes depends only on its material and UDIM tile.
            std::map<std::tuple<uint32_t, int, int>, std::pair<std::vector<uint64_t>*, std::vector<uint8_t>*>> targets;
            for (size_t t = 0; t < s.triangleCount(); ++t)
            {
                const glm::vec2* uv = &s.uvs[t * 3];
                const double a = uvArea(uv);
                if (std::abs(a) < 1e-14)
                    continue;
                const int tu = tiles[t].x, tv = tiles[t].y;
                const uint32_t material = t < s.materials.size() ? s.materials[t] : UINT32_MAX;
                auto [target, added] = targets.try_emplace({material, tu, tv});
                if (added)
                {
                    NameTokens tok;
                    tok.project = in.project;
                    tok.group = in.groups[g].name;
                    tok.mesh = shortName(s.name);
                    tok.material = material < s.materialNames.size() ? s.materialNames[material] : std::string();
                    tok.map = "*";
                    tok.ext = "x";
                    tok.udim = udimOf(tu, tv);
                    const std::string file = expandName(ps.output.name, tok, true);
                    std::vector<uint64_t>& grid = grids[{file, tok.udim}];
                    std::vector<uint8_t>& clash = contested[{file, tok.udim}];
                    if (grid.empty())
                    {
                        grid.assign(size_t(R) * size_t(R), 0);
                        clash.assign(size_t(R) * size_t(R), 0);
                    }
                    target->second = {&grid, &clash};
                }
                std::vector<uint64_t>& grid = *target->second.first;
                std::vector<uint8_t>& clash = *target->second.second;
                const uint64_t me = ((lowIndex << 32) | (t < low.islands.size() ? low.islands[t] : 0u)) + 1;
                glm::dvec2 p[3];
                for (int k = 0; k < 3; ++k)
                    p[k] = {(double(uv[k].x) - tu) * R, (double(uv[k].y) - tv) * R};
                const int x0 = std::clamp(int(std::floor(std::min({p[0].x, p[1].x, p[2].x}))), 0, R), x1 = std::clamp(int(std::ceil(std::max({p[0].x, p[1].x, p[2].x}))), 0, R);
                const int y0 = std::clamp(int(std::floor(std::min({p[0].y, p[1].y, p[2].y}))), 0, R), y1 = std::clamp(int(std::ceil(std::max({p[0].y, p[1].y, p[2].y}))), 0, R);
                auto edge = [](const glm::dvec2& a, const glm::dvec2& b, const glm::dvec2& q) { return (b.x - a.x) * (q.y - a.y) - (b.y - a.y) * (q.x - a.x); };
                const double sign = a > 0.0 ? 1.0 : -1.0;
                for (int y = y0; y < y1; ++y)
                    for (int x = x0; x < x1; ++x)
                    {
                        const glm::dvec2 q(x + 0.5, y + 0.5);
                        if (sign * edge(p[0], p[1], q) < 0.0 || sign * edge(p[1], p[2], q) < 0.0 || sign * edge(p[2], p[0], q) < 0.0)
                            continue;
                        const size_t at = size_t(y) * size_t(R) + size_t(x);
                        uint64_t& cell = grid[at];
                        if (cell == 0)
                            cell = me;
                        else if (cell != me)
                        {
                            clash[at] = 1;
                            claims.emplace_back(&clash, at, std::pair{std::min(cell, me), std::max(cell, me)});
                        }
                    }
            }
        }
    std::map<std::pair<uint64_t, uint64_t>, size_t> conflicts;
    for (const auto& [clash, at, pair] : claims)
    {
        const size_t x = at % size_t(R), y = at / size_t(R);
        const std::vector<uint8_t>& c = *clash;
        if (x > 0 && x + 1 < size_t(R) && y > 0 && y + 1 < size_t(R) && c[at - 1] && c[at + 1] && c[at - size_t(R)] && c[at + size_t(R)])
            ++conflicts[pair];
    }
    // Per pair of lows: overlapping cells and island pairs.
    std::map<std::pair<uint64_t, uint64_t>, std::pair<size_t, size_t>> perLows;
    std::map<std::pair<uint64_t, uint64_t>, json> islandPairs;
    for (const auto& [pair, cells] : conflicts)
    {
        const std::pair<uint64_t, uint64_t> lowPair{(pair.first - 1) >> 32, (pair.second - 1) >> 32};
        auto& v = perLows[lowPair];
        v.first += cells;
        ++v.second;
        json& ip = islandPairs[lowPair];
        if (ip.size() < 64)
            ip.push_back({(pair.first - 1) & 0xffffffffu, (pair.second - 1) & 0xffffffffu, cells});
    }
    for (const auto& [pair, v] : perLows)
    {
        const Owner a = lows[pair.first], b = lows[pair.second];
        const GroupInputs& ga = in.groups[a.group];
        const GroupInputs& gb = in.groups[b.group];
        const std::string& na = ga.lows[a.low].soup.name;
        const std::string& nb = gb.lows[b.low].soup.name;
        const double pct = 100.0 * double(v.first) / (double(R) * R);
        char buf[32];
        std::snprintf(buf, sizeof buf, "%.2f%%", pct);
        std::string msg;
        if (pair.first == pair.second)
            msg = std::to_string(v.second) + " pair(s) of its UV islands overlap (" + buf + " of the tile); stacked islands bake into the same texels";
        else if (a.group != b.group)
            msg = "UVs overlap " + nb + " of group " + gb.name + " in a texture both write (" + buf + " of the tile); the later group overwrites those texels";
        else
            msg = "UVs overlap " + nb + " (" + buf + " of the tile) in the texture they share";
        r.issues.push_back({"uvOverlap", "warning", ga.name, na, msg,
                            {{"cells", v.first}, {"islandPairs", v.second}, {"islands", islandPairs[pair]}, {"other", nb}, {"otherGroup", gb.name}, {"resolution", R}}});
    }
}

void texelDensity(const BakeInputs& in, const ProjectSettings& ps, const LintOptions& o, LintReport& r)
{
    struct Island
    {
        size_t group, low;
        uint32_t island;
        double density;
    };
    std::vector<Island> islands;
    const double texels = double(ps.output.width) * ps.output.height;
    for (size_t g = 0; g < in.groups.size(); ++g)
        for (size_t l = 0; l < in.groups[g].lows.size(); ++l)
        {
            const LowInput& low = in.groups[g].lows[l];
            const MeshSoup& s = low.soup;
            if (s.uvs.empty() || !low.islandCount)
                continue;
            std::vector<double> uvA(low.islandCount, 0.0), worldA(low.islandCount, 0.0);
            for (size_t t = 0; t < s.triangleCount() && t < low.islands.size(); ++t)
            {
                uvA[low.islands[t]] += std::abs(uvArea(&s.uvs[t * 3]));
                worldA[low.islands[t]] += area3(&s.positions[t * 3]);
            }
            for (uint32_t i = 0; i < low.islandCount; ++i)
                if (worldA[i] > 1e-8 && uvA[i] * texels >= kVisibleTexels)
                    islands.push_back({g, l, i, std::sqrt(uvA[i] * texels / worldA[i])});
        }
    // Against each mesh's own median: meshes deliberately get different densities (text, small trim, big glass), but
    // islands of one mesh far off the rest are usually a scaling slip.
    std::map<std::pair<size_t, size_t>, std::vector<double>> perMesh;
    for (const Island& i : islands)
        perMesh[{i.group, i.low}].push_back(i.density);
    std::map<std::pair<size_t, size_t>, std::pair<std::vector<double>, double>> outliers;
    for (auto& [key, d] : perMesh)
    {
        if (d.size() < 2)
            continue;
        std::vector<double> sorted = d;
        std::ranges::nth_element(sorted, sorted.begin() + std::ptrdiff_t(sorted.size() / 2));
        const double median = sorted[sorted.size() / 2];
        if (median <= 0.0)
            continue;
        for (double v : d)
            if (v > median * o.densityTolerance || v < median / o.densityTolerance)
            {
                outliers[key].first.push_back(v);
                outliers[key].second = median;
            }
    }
    for (const auto& [key, entry] : outliers)
    {
        const auto& [ds, median] = entry;
        const auto [lo, hi] = std::ranges::minmax(ds);
        char buf[160];
        std::snprintf(buf, sizeof buf, "%zu UV island(s) at %.3g to %.3g texels/cm against the rest of the mesh at %.3g", ds.size(), lo, hi, median);
        const GroupInputs& gi = in.groups[key.first];
        r.issues.push_back({"texelDensity", "warning", gi.name, gi.lows[key.second].soup.name, buf,
                            {{"islands", ds.size()}, {"min", lo}, {"max", hi}, {"median", median}}});
    }
}

/** Rays like the bake's from evenly spread low triangles; reports lows that find no high, or miss often. */
void highRange(const BakeInputs& in, const Backend& backend, const LintOptions& o, LintReport& r)
{
    LILA_ZONE();

    for (size_t g = 0; g < in.groups.size(); ++g)
    {
        const GroupInputs& gi = in.groups[g];
        if (gi.highs.empty())
            continue;
        for (const LowInput& low : gi.lows)
        {
            const MeshSoup& s = low.soup;
            const size_t n = s.triangleCount();
            if (!n)
                continue;
            const size_t step = std::max<size_t>(1, n / size_t(std::max(1, o.raySamples)));
            const bool explicitCage = low.cagePositions.size() == s.positions.size();
            std::vector<TexelSample> samples;
            for (size_t t = 0; t < n; t += step)
            {
                const size_t c = t * 3;
                const glm::vec3 P = (s.positions[c] + s.positions[c + 1] + s.positions[c + 2]) / 3.0f;
                glm::vec3 origin, dir;
                float length;
                const glm::vec3 C = explicitCage ? (low.cagePositions[c] + low.cagePositions[c + 1] + low.cagePositions[c + 2]) / 3.0f : glm::vec3(0.0f);
                if (explicitCage && glm::length(P - C) > 1e-6f)
                {
                    origin = C;
                    dir = glm::normalize(P - C);
                    length = glm::length(P - C) + gi.rear;
                }
                else
                {
                    glm::vec3 cn = low.cageNormals[c] + low.cageNormals[c + 1] + low.cageNormals[c + 2];
                    cn = glm::length(cn) > 0.0f ? glm::normalize(cn) : glm::vec3(0.0f, 0.0f, 1.0f);
                    const float scale = (low.cageScale[c] + low.cageScale[c + 1] + low.cageScale[c + 2]) / 3.0f;
                    const float push = gi.frontal * float(gi.settings.cageOffset) * scale;
                    origin = P + cn * push;
                    dir = -cn;
                    length = push + gi.rear;
                }
                TexelSample smp{};
                const glm::vec3 N = s.normals[c] + s.normals[c + 1] + s.normals[c + 2];
                for (int k = 0; k < 3; ++k)
                {
                    smp.position[k] = P[k];
                    smp.normal[k] = N[k];
                    smp.origin[k] = origin[k];
                    smp.direction[k] = dir[k];
                    smp.tangent[k] = low.tangents.size() > c ? low.tangents[c][k] : 0.0f;
                }
                smp.tangent[3] = 1.0f;
                smp.rayLength = length;
                smp.lowTriangle = uint32_t(t);
                smp.low = uint32_t(&low - gi.lows.data());
                samples.push_back(smp);
            }
            std::vector<HitRecord> hits(samples.size());
            backend.traceProjection(g, samples, hits);
            backend.endBand(samples);
            size_t missed = 0;
            for (const HitRecord& h : hits)
                if (h.high == kNoHit || !(h.flags & kHitValid) || (h.flags & kHitProbed))
                    ++missed;
            const double frac = samples.empty() ? 0.0 : double(missed) / double(samples.size());
            if (missed == samples.size())
                r.issues.push_back({"highRange", "warning", gi.name, s.name,
                                    "no high within range: every ray missed (frontal " + std::to_string(gi.frontal) + " cm, rear " + std::to_string(gi.rear) +
                                        " cm); move the high onto the low or raise the distances",
                                    {{"rays", samples.size()}, {"missed", missed}}});
            else if (frac > 0.25)
            {
                char buf[96];
                std::snprintf(buf, sizeof buf, "%.0f%% of test rays find no high; raise frontal/rear or paint bakeOffset there", frac * 100.0);
                r.issues.push_back({"highRange", "warning", gi.name, s.name, buf, {{"rays", samples.size()}, {"missed", missed}}});
            }
        }
    }
}

/** High points beyond the cage (outside the push) or deeper than rear, by their closest point on the group's lows. */
void cageContainment(const BakeInputs& in, const LintOptions& o, LintReport& r)
{
    LILA_ZONE();

    for (const GroupInputs& gi : in.groups)
    {
        if (gi.highs.empty() || gi.lows.empty())
            continue;
        // The lows as one unwelded mesh, so closest-point face ids are (low, triangle) in order.
        std::vector<glm::dvec3> pts;
        std::vector<geo::Index> sizes, idx;
        std::vector<std::pair<size_t, size_t>> origin;
        for (size_t l = 0; l < gi.lows.size(); ++l)
        {
            const MeshSoup& s = gi.lows[l].soup;
            for (size_t t = 0; t < s.triangleCount(); ++t)
            {
                if (area3(&s.positions[t * 3]) <= 1e-12)
                    continue;
                for (int k = 0; k < 3; ++k)
                {
                    idx.push_back(geo::Index(pts.size()));
                    pts.push_back(glm::dvec3(s.positions[t * 3 + size_t(k)]));
                }
                sizes.push_back(3);
                origin.push_back({l, t});
            }
        }
        const geo::Mesh lowMesh = geo::Mesh::fromPolygons(pts, sizes, idx);
        if (lowMesh.numFaces() != origin.size())
            continue;
        const geo::MeshBvh bvh(lowMesh);
        size_t total = 0;
        for (const HighInput& h : gi.highs)
            total += h.soup.positions.size();
        if (!total)
            continue;
        const size_t step = std::max<size_t>(1, total / size_t(std::max(1, o.pointSamples)));
        std::vector<glm::vec3> sample;
        for (const HighInput& h : gi.highs)
            for (size_t i = 0; i < h.soup.positions.size(); i += step)
                sample.push_back(h.soup.positions[i]);
        std::vector<float> outside(sample.size(), 0.0f), inside(sample.size(), 0.0f);
        tbb::parallel_for(size_t(0), sample.size(), [&](size_t i) {
            geo::BvhClosest c;
            if (!bvh.closestPoint(glm::dvec3(sample[i]), c) || c.face >= origin.size())
                return;
            const auto [l, t] = origin[c.face];
            const LowInput& low = gi.lows[l];
            const size_t c0 = t * 3;
            const glm::vec3 w(c.bary);
            glm::vec3 n = low.cageNormals[c0] * w.x + low.cageNormals[c0 + 1] * w.y + low.cageNormals[c0 + 2] * w.z;
            n = glm::length(n) > 0.0f ? glm::normalize(n) : glm::vec3(0.0f, 0.0f, 1.0f);
            float push;
            if (low.cagePositions.size() == low.soup.positions.size())
            {
                const glm::vec3 C = low.cagePositions[c0] * w.x + low.cagePositions[c0 + 1] * w.y + low.cagePositions[c0 + 2] * w.z;
                const glm::vec3 P = low.soup.positions[c0] * w.x + low.soup.positions[c0 + 1] * w.y + low.soup.positions[c0 + 2] * w.z;
                push = glm::length(C - P);
            }
            else
                push = gi.frontal * float(gi.settings.cageOffset) * (low.cageScale[c0] * w.x + low.cageScale[c0 + 1] * w.y + low.cageScale[c0 + 2] * w.z);
            const float d = glm::dot(sample[i] - glm::vec3(c.point), n);
            const float tol = 1e-3f * std::max(1.0f, push);
            if (d > push + tol)
                outside[i] = d - push;
            else if (-d > gi.rear + tol)
                inside[i] = -d - gi.rear;
        });
        size_t out = 0, in2 = 0;
        float maxOut = 0.0f, maxIn = 0.0f;
        for (size_t i = 0; i < sample.size(); ++i)
        {
            if (outside[i] > 0.0f)
            {
                ++out;
                maxOut = std::max(maxOut, outside[i]);
            }
            if (inside[i] > 0.0f)
            {
                ++in2;
                maxIn = std::max(maxIn, inside[i]);
            }
        }
        // Measured Auto distances leave the outermost tenth of a percent of samples out by design.
        const size_t tolerated = gi.measured ? sample.size() / 100 : 0;
        if (gi.measured && gi.settings.frontal <= 0.0 && out <= tolerated)
            out = 0;
        if (gi.measured && gi.settings.rear <= 0.0 && in2 <= tolerated)
            in2 = 0;
        char buf[200];
        if (out)
        {
            std::snprintf(buf, sizeof buf, "%zu of %zu sampled high points lie outside the cage, up to %.3g cm beyond it; raise frontal or paint bakeOffset",
                          out, sample.size(), double(maxOut));
            r.issues.push_back({"cageContainment", "warning", gi.name, gi.highs.front().source, buf,
                                {{"outside", out}, {"sampled", sample.size()}, {"maxBeyond", maxOut}, {"suggestedFrontal", gi.frontal + maxOut}}});
        }
        if (in2)
        {
            std::snprintf(buf, sizeof buf, "%zu of %zu sampled high points lie deeper than rear (%.3g cm), up to %.3g cm; raise rear", in2, sample.size(),
                          double(gi.rear), double(maxIn));
            r.issues.push_back({"cageContainment", "warning", gi.name, gi.highs.front().source, buf,
                                {{"inside", in2}, {"sampled", sample.size()}, {"maxBeyond", maxIn}, {"suggestedRear", gi.rear + maxIn}}});
        }
    }
}

scene::Node* meshNamed(scene::Graph& g, const std::string& name)
{
    try
    {
        scene::Node* n = g.find(name);
        return n && n->type().isA("mesh") ? n : nullptr;
    }
    catch (const std::exception&)
    {
        return nullptr;
    }
}

void triangulation(scene::Graph& g, const GroupInputs& gi, LintReport& r)
{
    for (const std::string& name : gi.settings.lows)
    {
        scene::Node* n = meshNamed(g, name);
        if (!n)
            continue;
        scene::MeshPtr m = exportedMesh(g, n->id());
        if (!m)
            continue;
        const geo::Triangulation tris = geo::triangulate(*m);
        size_t folded = 0;
        double worst = 0.0;
        json faces = json::array();
        for (geo::Index f = 0; f < m->faceCapacity(); ++f)
        {
            if (!m->faceValid(f) || m->faceCorners(f).size() <= 3 || f >= tris.faceFirst.size() || tris.faceFirst[f] == geo::kInvalid)
                continue;
            glm::dvec3 first(0.0);
            double fold = 0.0;
            for (geo::Index t = tris.faceFirst[f]; t < tris.triangleCount() && tris.faces[t] == f; ++t)
            {
                const glm::dvec3 a = m->position(m->cornerPoint(tris.corners[size_t(t) * 3]));
                const glm::dvec3 b = m->position(m->cornerPoint(tris.corners[size_t(t) * 3 + 1]));
                const glm::dvec3 c = m->position(m->cornerPoint(tris.corners[size_t(t) * 3 + 2]));
                glm::dvec3 nrm = glm::cross(b - a, c - a);
                if (glm::length(nrm) < 1e-18)
                    continue;
                nrm = glm::normalize(nrm);
                if (first == glm::dvec3(0.0))
                    first = nrm;
                else
                    fold = std::max(fold, glm::degrees(std::acos(std::clamp(glm::dot(first, nrm), -1.0, 1.0))));
            }
            if (fold > 2.0)
            {
                ++folded;
                worst = std::max(worst, fold);
                if (faces.size() < kMaxComponents)
                    faces.push_back(f);
            }
        }
        if (folded)
        {
            char buf[220];
            std::snprintf(buf, sizeof buf,
                          "%zu non-planar face(s) (folding up to %.1f deg) bake on Lila's triangle split; export triangulated (the game exporter's default) "
                          "or triangulate them so the target shades the same triangles",
                          folded, worst);
            r.issues.push_back({"triangulation", "warning", gi.name, n->path(), buf, {{"faces", folded}, {"maxFold", worst}, {"faceIds", faces}}});
        }
    }
}

void cageTopology(scene::Graph& g, const GroupInputs& gi, LintReport& r)
{
    const GroupSettings& s = gi.settings;
    if (s.cage.empty())
        return;
    scene::Node* cage = meshNamed(g, s.cage);
    if (!cage)
    {
        r.issues.push_back({"cageTopology", "warning", gi.name, s.cage, "the cage is not a mesh in the scene; the auto cage is used", json::object()});
        return;
    }
    if (s.lows.size() != 1)
    {
        r.issues.push_back({"cageTopology", "warning", gi.name, cage->path(),
                            "an editable cage needs a group with exactly one low; the auto cage is used", {{"lows", s.lows.size()}}});
        return;
    }
    scene::Node* low = meshNamed(g, s.lows.front());
    scene::MeshPtr lm = low ? exportedMesh(g, low->id()) : nullptr;
    scene::MeshPtr cm = exportedMesh(g, cage->id());
    if (!lm || !cm)
        return;
    if (const std::string why = cageMismatch(*lm, *cm); !why.empty())
        r.issues.push_back({"cageTopology", "warning", gi.name, cage->path(), "doesn't match " + low->path() + ": " + why + "; the auto cage is used",
                            json::object()});
}
} // namespace

json LintIssue::toJson() const
{
    return {{"check", check}, {"severity", severity}, {"group", group}, {"mesh", mesh}, {"message", message}, {"data", data}};
}

size_t LintReport::warnings() const
{
    return size_t(std::ranges::count_if(issues, [](const LintIssue& i) { return i.severity == "warning"; }));
}

json LintReport::toJson() const
{
    json list = json::array();
    for (const LintIssue& i : issues)
        list.push_back(i.toJson());
    return {{"issues", list}, {"warnings", warnings()}, {"checks", lintChecks()}};
}

std::vector<std::string> LintReport::lines() const
{
    std::vector<std::string> out;
    for (const LintIssue& i : issues)
        if (i.severity == "warning")
            out.push_back("lint " + i.check + ": " + (i.mesh.empty() ? i.group : i.mesh) + ": " + i.message);
    return out;
}

const std::vector<std::string>& lintChecks()
{
    static const std::vector<std::string> checks = {"hardEdges",  "uvOverlap", "uvMissing",       "texelDensity", "mirroredTangents",
                                                    "highRange", "cageContainment", "triangulation", "cageTopology"};
    return checks;
}

LintReport lintInputs(const BakeInputs& in, const ProjectSettings& ps, const Backend* backend, const LintOptions& o)
{
    LILA_ZONE();

    LintReport r;
    std::vector<std::pair<const GroupInputs*, const LowInput*>> lows;
    for (const GroupInputs& gi : in.groups)
        for (const LowInput& low : gi.lows)
            lows.emplace_back(&gi, &low);
    std::vector<LintReport> perLow(lows.size());
    tbb::parallel_for(size_t(0), lows.size(), [&](size_t i) {
        const auto [gi, low] = lows[i];
        if (wanted(o, "uvMissing"))
            uvMissing(*gi, *low, ps, perLow[i]);
        if (wanted(o, "hardEdges"))
            hardEdges(*gi, *low, perLow[i]);
        if (wanted(o, "mirroredTangents"))
            mirroredTangents(*gi, *low, ps, perLow[i]);
    });
    for (LintReport& l : perLow)
        std::ranges::move(l.issues, std::back_inserter(r.issues));
    if (wanted(o, "uvOverlap"))
        uvOverlap(in, ps, r);
    if (wanted(o, "texelDensity"))
        texelDensity(in, ps, o, r);
    if (backend && wanted(o, "highRange"))
        highRange(in, *backend, o, r);
    if (wanted(o, "cageContainment"))
        cageContainment(in, o, r);
    return r;
}

void lintScene(scene::Graph& g, const BakeInputs& in, LintReport& r, const LintOptions& o)
{
    for (const GroupInputs& gi : in.groups)
    {
        // Lila's FBX and Unreal exports triangulate exactly as the bake does, so this only matters when asked for.
        if (named(o, "triangulation"))
            triangulation(g, gi, r);
        if (wanted(o, "cageTopology"))
            cageTopology(g, gi, r);
    }
}

namespace
{
/** The mesh's export-enabled modifiers. */
std::vector<std::string> exportModifiers(scene::Graph& g, const scene::Node& n)
{
    std::vector<std::string> names;
    for (scene::NodeId id : scene::modifiers::stack(g, n.id()))
        if (const scene::ModifierNode* m = scene::modifiers::asModifier(g.node(id)); m && scene::modifiers::exportEnabled(*m))
            names.push_back(m->name());
    return names;
}

/** The mesh's last export modifier that sets normals, when it is a Weighted Normals keeping the hard edges split: its name. */
std::string hardEdgeKeeper(scene::Graph& g, const scene::Node& n)
{
    const std::vector<scene::NodeId> stack = scene::modifiers::stack(g, n.id());
    for (auto it = stack.rbegin(); it != stack.rend(); ++it)
        if (const scene::ModifierNode* m = scene::modifiers::asModifier(g.node(*it));
            m && scene::modifiers::exportEnabled(*m) && m->type().isA("weightedNormalsModifier"))
            return m->as<bool>(m->type().attrIndex("keepHardEdges")) ? m->name() : std::string();
    return {};
}

/** Whether base, linted alone, raises an issue of the same check and kind (a triangle count or an island count) as i. */
bool baseHasIssue(const LintIssue& i, const LowInput& base, const ProjectSettings& ps)
{
    GroupInputs gi;
    gi.name = i.group;
    LintReport r;
    if (i.check == "uvMissing")
        uvMissing(gi, base, ps, r);
    else if (i.check == "hardEdges")
        hardEdges(gi, base, r);
    else
        mirroredTangents(gi, base, ps, r);
    return std::ranges::any_of(r.issues, [&](const LintIssue& b) { return b.check == i.check && b.data.contains("islands") == i.data.contains("islands"); });
}

/**
 * A per-mesh issue the base mesh doesn't have comes from the mesh's modifiers: they move the surface under UVs they only
 * blend, or split its normals. Says so, and for UV problems that Relax or Unwrap (which solve on the exported mesh) fix
 * it, and that Unwrap Exported Mesh always does.
 */
void explainModifierIssues(scene::Graph& g, LintReport& r, const ProjectSettings& ps)
{
    for (LintIssue& i : r.issues)
    {
        const bool uvs = (i.check == "uvMissing" && i.data.contains("count")) || i.check == "mirroredTangents";
        if (!uvs && i.check != "hardEdges")
            continue;
        scene::Node* n = g.find(i.mesh);
        if (!n || !n->type().isA("mesh"))
            continue;
        if (i.check == "hardEdges")
            if (const std::string keeper = hardEdgeKeeper(g, *n); !keeper.empty())
            {
                i.message = std::to_string(i.data["count"].get<size_t>()) + " hard edge(s) are not UV seams: " + keeper +
                            " (Weighted Normals) keeps the mesh's hard edges split. Turn off its Keep Hard Edges, or split the UVs there";
                i.data["keptBy"] = keeper;
                continue;
            }
        const std::vector<std::string> mods = exportModifiers(g, *n);
        const scene::MeshPtr cage = mods.empty() ? nullptr : scene::modifiers::cage(g, n->id());
        if (!cage || baseHasIssue(i, lowFromMesh(*cage, glm::dmat4(1.0), i.mesh), ps))
            continue;
        std::string names;
        for (const std::string& m : mods)
            names += (names.empty() ? "" : ", ") + m;
        const auto count = [&](const char* key) { return std::to_string(i.data[key].get<size_t>()); };
        const std::string what = i.check == "hardEdges"           ? count("count") + " hard edge(s) off the UV seams"
                                 : i.data.contains("islands")     ? count("islands") + " UV island(s) folding over themselves"
                                 : i.check == "uvMissing"         ? count("count") + " triangle(s) collapsed in UV"
                                                                  : count("count") + " triangle(s) wound against their tangent sign";
        i.data["fromModifiers"] = mods;
        if (i.check == "hardEdges")
        {
            i.message = what + " on the exported mesh only: its modifiers (" + names + ") split the normals there; the base mesh is fine. "
                        "Split the UVs along those edges or change the modifiers' angle";
            continue;
        }
        i.message = what + " on the exported mesh only: its modifiers (" + names +
                    ") move the surface under UVs they keep; the base mesh's UVs are fine. Relax or Unwrap the mesh to solve its UVs on the "
                    "exported mesh, or UV > Unwrap Exported Mesh to apply the modifiers and unwrap the result";
        i.data["fix"] = "uvUnwrapExported";
    }
}

} // namespace

LintReport lint(scene::Graph& g, const BakeInputs& in, const ProjectSettings& ps, const Backend* backend, const LintOptions& o)
{
    LintReport r = lintInputs(in, ps, backend, o);
    lintScene(g, in, r, o);
    explainModifierIssues(g, r, ps);
    return r;
}
} // namespace lila::bake
