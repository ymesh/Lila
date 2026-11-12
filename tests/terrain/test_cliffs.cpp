#include "Cliffs.h"
#include "TestUtil.h"
#include "Volume.h"

#include "geo/Heightfield.h"

#include <doctest/doctest.h>

#include <cmath>
#include <filesystem>
#include <set>

using namespace lila;
using namespace terraintest;
using namespace terrain::cliffs;
using json = nlohmann::json;

namespace
{
double smoothstepC(double a, double b, double x)
{
    const double t = std::clamp((x - a) / (b - a), 0.0, 1.0);
    return t * t * (3.0 - 2.0 * t);
}

/** Flat ground at 0 for x < -300, a 70 degree face rising to a plateau 3000 cm high by x = 800; 100 cm cells. */
geo::Heightfield escarpment(int res = 129)
{
    geo::Heightfield h = geo::makeHeightfieldCovering({0.0, 0.0}, {12800.0, 12800.0}, res);
    for (int z = 0; z < h.nz; ++z)
        for (int x = 0; x < h.nx; ++x)
            h.heights[h.index(x, z)] = float(3000.0 * smoothstepC(-300.0, 800.0, h.samplePos(x, z).x));
    return h;
}

TriMesh plate(double size, int n)
{
    TriMesh m;
    m.addVertexAttr("w", 1);
    for (int j = 0; j <= n; ++j)
        for (int i = 0; i <= n; ++i)
        {
            const size_t v = m.addVertex({-size * 0.5 + size * i / n, 0.0, -size * 0.5 + size * j / n});
            m.vattr(v)[0] = float(i);
        }
    for (int j = 0; j < n; ++j)
        for (int i = 0; i < n; ++i)
        {
            const uint32_t a = uint32_t(j * (n + 1) + i), b = a + 1, c = a + uint32_t(n + 1) + 1, d = a + uint32_t(n + 1);
            m.t.push_back({a, c, b});
            m.t.push_back({a, d, c});
        }
    m.addCornerAttr("u0", 2);
    return m;
}
} // namespace

TEST_CASE("cliffs: slope regions are found, named, faced downhill and sized")
{
    const geo::Heightfield h = escarpment();
    RegionParams p;
    p.slope = 50.0;
    p.minHeight = 1000.0;
    size_t dropped = 0;
    const std::vector<Region> r = findRegions(h, p, &dropped);
    REQUIRE(r.size() == 1);
    CHECK(r[0].name == "cliff_1");
    CHECK(r[0].facing.x < -0.99);
    CHECK(r[0].facingDegrees == doctest::Approx(270.0).epsilon(0.01));
    CHECK(r[0].high - r[0].low > 1500.0);
    CHECK(r[0].maxSlope > 70.0);
    CHECK(!r[0].rings.empty());
    // Every steep sample is inside the region's rings.
    const std::vector<uint8_t> inside = ringSamples(h, r[0].rings, 0);
    const std::vector<float> slope = slopeDegrees(h);
    for (size_t i = 0; i < h.size(); ++i)
        if (slope[i] >= 50.0f)
            CHECK(inside[i] == 1);
    p.minHeight = 5000.0;
    CHECK(findRegions(h, p, &dropped).empty());
    CHECK(dropped == 1);
}

TEST_CASE("cliffs: a proud cliff never bulges past the terrain, its base is below the ground everywhere and the ground steps back behind it")
{
    const geo::Heightfield h = escarpment();
    RegionParams rp;
    const std::vector<Region> r = findRegions(h, rp);
    REQUIRE(!r.empty());
    MeshParams p;
    p.shape.facing = r[0].facing;
    p.shape.depth = 120.0;
    p.detail = 2.0;
    const CliffMesh c = buildFromRegion(h, r[0].rings, p);
    INFO(c.stats.warnings.size());
    REQUIRE(c.mesh.t.size() > 1000);
    CHECK(c.stats.layers > 4);
    CHECK(c.stats.maxOut <= p.shape.depth + 1e-6);
    CHECK(c.stats.baseClearance < 0.0);
    CHECK(c.stats.hidden < 0.1);
    // Masks: colour on every vertex, strata in 0..1, some ledges.
    const int color = c.mesh.vertexIndex("color"), strata = c.mesh.vertexIndex("strata"), ledge = c.mesh.vertexIndex("ledge");
    REQUIRE(color >= 0);
    REQUIRE(strata >= 0);
    double ledges = 0.0;
    for (size_t v = 0; v < c.mesh.p.size(); ++v)
    {
        CHECK(c.mesh.vattr(v)[strata] >= 0.0f);
        CHECK(c.mesh.vattr(v)[strata] <= 1.0f);
        ledges += c.mesh.vattr(v)[ledge];
    }
    CHECK(ledges > 0.0);
    // Every bottom-edge vertex is below the ground left after the cliff's edit.
    geo::Heightfield after = h;
    GroundEdit e;
    e.rings = r[0].rings;
    e.depth = p.shape.depth;
    const GroundResult gr = applyGroundEdits(after, {e}, {});
    CHECK(gr.recessed > 0);
    CHECK(after.findMask("cliffTalus") != nullptr);
    const std::vector<uint8_t> border = borderVertices(c.mesh);
    const int hf = c.mesh.vertexIndex("heightFraction");
    size_t bottoms = 0;
    for (size_t v = 0; v < c.mesh.p.size(); ++v)
        if (border[v] && c.mesh.vattr(v)[hf] < 0.5)
        {
            ++bottoms;
            CHECK(c.mesh.p[v].y < after.heightAt(c.mesh.p[v].x, c.mesh.p[v].z));
        }
    CHECK(bottoms > 10);
    // The shape stays behind where the terrain was: no vertex stands out past the original surface along x.
    for (size_t v = 0; v < c.mesh.p.size(); ++v)
    {
        const glm::dvec3 q = c.mesh.p[v];
        CHECK(q.y <= h.heightAt(q.x, q.z) + 1.0);
    }
}

TEST_CASE("cliffs: an inset cliff fills its holes without a crack (the split's own seam check)")
{
    const geo::Heightfield h = escarpment();
    const std::vector<Region> r = findRegions(h, RegionParams{});
    REQUIRE(!r.empty());
    MeshParams p;
    p.mode = Mode::Inset;
    p.shape.facing = r[0].facing;
    p.detail = 2.0;
    const CliffMesh c = buildFromRegion(h, r[0].rings, p);
    geo::Heightfield after = h;
    GroundEdit e;
    e.mode = Mode::Inset;
    e.rings = r[0].rings;
    const GroundResult gr = applyGroundEdits(after, {e}, {});
    CHECK(gr.holes > 0);
    CHECK(gr.recessed == 0);
    std::vector<std::shared_ptr<const geo::Mesh>> meshes{std::make_shared<const geo::Mesh>(toMesh(c.mesh))};
    const terrain::SeamReport s = terrain::measureSeams(after, meshes);
    INFO("worst " << s.worst.x << " " << s.worst.y << " " << s.worst.z);
    CHECK(s.length > 0.0);
    CHECK(s.uncovered == 0.0);
    CHECK(s.maxGap < 0.5);
    const terrain::FootprintReport fp = terrain::measureFootprint(after, meshes, 2.0 * h.cell);
    CHECK(fp.uncoveredInHole == 0);
}

TEST_CASE("cliffs: the slope-aware projection keeps flat and vertical texel density within 1.5, top projection alone does not")
{
    const glm::dvec3 a(0, 0, 0), b(100, 0, 0), c(0, 0, 100);
    const glm::dvec3 va(0, 0, 0), vb(0, 100, 0), vc(0, 0, 100);
    const glm::dvec3 sa(0, 0, 0), sb(100, 0, 0), sc(0, 100, 30);
    for (bool biplanar : {false, true})
    {
        const double flat = texelDensity(a, c, b, 400.0, kSlopeStart, kSlopeBlend, biplanar);
        const double wall = texelDensity(va, vb, vc, 400.0, kSlopeStart, kSlopeBlend, biplanar);
        const double steep = texelDensity(sa, sb, sc, 400.0, kSlopeStart, kSlopeBlend, biplanar);
        CHECK(flat > 0.0);
        CHECK(std::max(flat, wall) / std::min(flat, wall) < 1.5);
        CHECK(std::max(flat, steep) / std::min(flat, steep) < 1.5);
        // Below the threshold the top projection alone: flat ground pays one sample.
        const glm::dvec3 w = projectionWeights({0.0, 1.0, 0.1}, kSlopeStart, kSlopeBlend, biplanar);
        CHECK(w.y == doctest::Approx(1.0));
    }
    const double wallTop = texelDensity(va, vb, vc, 400.0, 90.0, 0.001, false);
    CHECK(wallTop < 0.01 / 400.0);
}

TEST_CASE("cliffs: a one-cell landscape step keeps its texel density, the lip included")
{
    // A 1500 cm step across one 100 cm cell, shaded with per-vertex normals from central differences as a landscape is.
    const int n = 8;
    const double cell = 100.0;
    auto height = [&](int x) { return x >= n / 2 ? 1500.0 : 0.0; };
    auto vertexNormal = [&](int x) {
        const double dh = (height(std::min(x + 1, n - 1)) - height(std::max(x - 1, 0))) / (2.0 * cell);
        return glm::normalize(glm::dvec3(-dh, 1.0, 0.0));
    };
    double lo = 1e300, hi = 0.0;
    bool leaning = false;
    for (int x = 0; x + 1 < n; ++x)
    {
        const glm::dvec3 p0(x * cell, height(x), 0.0), p1((x + 1) * cell, height(x + 1), 0.0);
        const glm::dvec3 q0 = p0 + glm::dvec3(0, 0, cell), q1 = p1 + glm::dvec3(0, 0, cell);
        const glm::dvec3 shading = glm::normalize(vertexNormal(x) + vertexNormal(x + 1));
        for (const auto& [a, b, c] : {std::array{p0, q0, p1}, std::array{p1, q0, q1}})
        {
            const glm::dvec3 face = glm::normalize(glm::cross(b - a, c - a));
            leaning = leaning || glm::degrees(std::acos(std::clamp(std::abs(glm::dot(face, shading)), 0.0, 1.0))) > kProjectionFace;
            const double d = texelDensity(a, b, c, 400.0, kSlopeStart, kSlopeBlend, false, &shading);
            lo = std::min(lo, d);
            hi = std::max(hi, d);
        }
    }
    CHECK(leaning);
    CHECK(hi / lo < 1.5);
    // Gently curved ground keeps the smooth shading normal.
    const glm::dvec3 smooth = glm::normalize(glm::dvec3(0.3, 1.0, 0.0)), facet = glm::normalize(glm::dvec3(0.35, 1.0, 0.0));
    CHECK(glm::length(projectionNormal(smooth, facet) - smooth) < 1e-9);
}

TEST_CASE("cliffs: tiles cut on the World Partition grid weld exactly and stay inside their cells")
{
    const geo::Heightfield h = escarpment(65);
    const std::vector<Region> r = findRegions(h, RegionParams{});
    REQUIRE(!r.empty());
    MeshParams p;
    p.shape.facing = r[0].facing;
    p.detail = 1.0;
    const CliffMesh c = buildFromRegion(h, r[0].rings, p);
    TileParams tp;
    tp.cell = 1600.0;
    tp.lods = 2;
    const std::vector<Tile> tiles = cutTiles(c.mesh, tp);
    REQUIRE(tiles.size() > 3);
    std::map<std::pair<int, int>, const Tile*> byCell;
    for (const Tile& t : tiles)
    {
        byCell[t.cell] = &t;
        const double x0 = t.cell.first * tp.cell, z0 = t.cell.second * tp.cell;
        CHECK(t.bounds.min.x >= x0 - 1e-6);
        CHECK(t.bounds.max.x <= x0 + tp.cell + 1e-6);
        CHECK(t.bounds.min.z >= z0 - 1e-6);
        CHECK(t.bounds.max.z <= z0 + tp.cell + 1e-6);
        CHECK(t.lods.size() == 2);
        CHECK(!t.collision.empty());
        CHECK(t.hash != 0);
    }
    // Neighbours along z share the vertices on their common line, bit for bit, at LOD0 and at the reduced level.
    size_t shared = 0;
    for (const auto& [cell, t] : byCell)
    {
        auto it = byCell.find({cell.first, cell.second + 1});
        if (it == byCell.end())
            continue;
        const double line = (cell.second + 1) * tp.cell;
        for (size_t lod = 0; lod < 2; ++lod)
        {
            auto onLine = [&](const Tile& tile) {
                std::set<std::tuple<long long, long long, long long>> pts;
                auto key = [](double v) { return (long long)std::llround(v * 1e4); };
                const geo::Mesh& m = tile.lods[lod];
                for (geo::Index v = 0; v < m.pointCapacity(); ++v)
                    if (m.pointValid(v))
                    {
                        const glm::dvec3 q = m.position(v) + tile.centre;
                        if (std::abs(q.z - line) < 1e-6)
                            pts.insert({key(q.x), key(q.y), key(q.z)});
                    }
                return pts;
            };
            const auto a = onLine(*t), b = onLine(*it->second);
            CHECK(a.size() == b.size());
            size_t same = 0;
            for (const auto& q : a)
                same += b.count(q);
            CHECK(same == a.size());
            shared += same;
        }
    }
    CHECK(shared > 0);
}

TEST_CASE("cliffs: slicing splits shared edges once and keeps corner attributes")
{
    TriMesh m = plate(1000.0, 4);
    const size_t before = m.t.size();
    sliceOnGrid(m, {0.0, 0.0}, 300.0);
    CHECK(m.t.size() > before);
    CHECK(m.ca.size() == m.t.size() * 3 * size_t(m.cornerStride));
    const auto cells = splitCells(m, {0.0, 0.0}, 300.0);
    CHECK(cells.size() == 16);
    // Watertight: the only open edges are the plate's own outline, so nothing tore at the cuts.
    const std::vector<uint8_t> border = borderVertices(m);
    for (size_t v = 0; v < m.p.size(); ++v)
        if (border[v])
            CHECK((std::abs(std::abs(m.p[v].x) - 500.0) < 1e-6 || std::abs(std::abs(m.p[v].z) - 500.0) < 1e-6));
}

TEST_CASE("cliffs: tiles keep the source's colour set and material slots, so contact skirts tile with their blend")
{
    geo::Mesh src = toMesh(plate(1000.0, 4));
    auto col = geo::colors::add(src, "Col", glm::vec4(1.0f));
    for (geo::Index c = 0; c < src.cornerCapacity(); ++c)
        if (src.cornerValid(c))
            col[c] = glm::vec4(0.25f, 0.5f, 0.75f, float(src.position(src.cornerPoint(c)).x > 0.0 ? 1.0 : 0.0));
    src.materialNames() = {"LilaSkirtBlend"};
    TileParams tp;
    tp.cell = 300.0;
    tp.lods = 2;
    const std::vector<Tile> tiles = cutTiles(fromMesh(src), tp);
    REQUIRE(tiles.size() > 1);
    for (const Tile& t : tiles)
        for (const geo::Mesh& lod : t.lods)
        {
            CHECK(lod.materialNames() == std::vector<std::string>{"LilaSkirtBlend"});
            REQUIRE(geo::colors::sets(lod) == std::vector<std::string>{"Col"});
            const auto v = geo::colors::values(lod, "Col");
            for (geo::Index c = 0; c < lod.cornerCapacity(); ++c)
                if (lod.cornerValid(c))
                {
                    CHECK(v[c].g == doctest::Approx(0.5f));
                    CHECK(v[c].a >= 0.0f);
                    CHECK(v[c].a <= 1.0f);
                }
        }
}

TEST_CASE("cliffs: tiered scatter seeds per cell, so an edit only reshuffles the cells it touches")
{
    TriMesh m = plate(8000.0, 40);
    m.addVertexAttr("scatter", 1, 1.0f);
    std::vector<Tier> tiers = {{"rocks", 1000.0, 0.5, "scatter", 0.5, 1.0, 1.0, {"rock"}}};
    const auto a = scatterTiers(m, tiers, 7, {0.0, 0.0});
    CHECK(a.size() > 100);
    CHECK(scatterTiers(m, tiers, 7, {0.0, 0.0}).size() == a.size());
    // Lift the vertices inside one cell (x and z in [1000, 2000)).
    for (glm::dvec3& q : m.p)
        if (q.x > 1000.0 && q.x < 2000.0 && q.z > 1000.0 && q.z < 2000.0)
            q.y += 50.0;
    const auto b = scatterTiers(m, tiers, 7, {0.0, 0.0});
    std::map<std::pair<int, int>, std::vector<glm::dvec3>> ca, cb;
    for (const auto& s : a)
        ca[s.cell].push_back(s.position);
    for (const auto& s : b)
        cb[s.cell].push_back(s.position);
    size_t changed = 0;
    for (const auto& [cell, pts] : ca)
        if (cb[cell] != pts)
            ++changed;
    CHECK(changed >= 1);
    CHECK(changed <= 9);
    CHECK(changed < ca.size() / 4);
}

TEST_CASE("cliffs: kit pieces fit along a cliff, none floating, gaps filled")
{
    const geo::Heightfield h = escarpment();
    const std::vector<Region> r = findRegions(h, RegionParams{});
    REQUIRE(!r.empty());
    std::vector<KitPiece> kit;
    for (double w : {800.0, 500.0, 250.0})
    {
        KitPiece k;
        k.name = "rock" + std::to_string(int(w));
        k.size = {w, 1000.0, 400.0};
        k.lo = {-w * 0.5, 0.0, -200.0};
        for (int i = 0; i <= 4; ++i)
            for (int j = 0; j <= 4; ++j)
                k.samples.push_back({-w * 0.5 + w * i / 4.0, 1000.0 * j / 4.0, 200.0});
        kit.push_back(k);
    }
    KitParams p;
    const KitResult res = fitKit(h, r[0], kit, p);
    CHECK(res.placed.size() > 10);
    CHECK(res.coverage > 0.6);
    for (const KitPlacement& k : res.placed)
    {
        const glm::dvec3 base = glm::dvec3(k.m * glm::dvec4(0.0, 0.0, 0.0, 1.0));
        CHECK(base.y <= h.heightAt(base.x, base.z) + p.floatTolerance + 1.0);
    }
}

TEST_CASE("cliffs: a physics depth check rejects kit pieces buried past maxPenetration of their depth")
{
    const geo::Heightfield h = escarpment();
    const std::vector<Region> r = findRegions(h, RegionParams{});
    REQUIRE(!r.empty());
    KitPiece k;
    k.name = "rock";
    k.size = {500.0, 1000.0, 400.0};
    k.lo = {-250.0, 0.0, -200.0};
    for (int i = 0; i <= 4; ++i)
        for (int j = 0; j <= 4; ++j)
            k.samples.push_back({-250.0 + 500.0 * i / 4.0, 1000.0 * j / 4.0, 200.0});
    KitParams p;
    size_t asked = 0;
    // Every piece reads as buried 300 cm deep: past 0.6 of a 400 cm deep piece at scale 1.2 or more, clear below.
    p.depthOf = [&asked](std::span<const glm::dvec3> hull, const glm::dvec3& out) {
        ++asked;
        CHECK(hull.size() == 25);
        CHECK(std::abs(out.y) < 1e-9);
        return 300.0;
    };
    p.maxPenetration = 0.6;
    p.scaleJitter = 0.0;
    const KitResult deep = fitKit(h, r[0], {k}, p);
    CHECK(asked > 0);
    p.maxPenetration = 0.01;
    const KitResult strict = fitKit(h, r[0], {k}, p);
    CHECK(strict.placed.empty());
    CHECK(strict.rejectedPenetrating > 0);
    p.maxPenetration = 10.0;
    const KitResult loose = fitKit(h, r[0], {k}, p);
    CHECK(loose.rejectedPenetrating == 0);
    CHECK(loose.placed.size() >= deep.placed.size());
}

TEST_CASE("cliffs: an extruded wall rises from the ground to the stamped plateau, layered")
{
    geo::Heightfield h = geo::makeHeightfieldCovering({0.0, 0.0}, {12800.0, 12800.0}, 129);
    WallStamp w;
    w.line = {{-4000.0, 0.0}, {4000.0, 0.0}};
    w.height = 1500.0;
    applyGroundEdits(h, {}, {w});
    // Behind (left of +X as seen from above is -Z... the stamp's left) is raised, the other side stays.
    const double behind = h.heightAt(0.0, -500.0), front = h.heightAt(0.0, 500.0);
    CHECK(std::max(behind, front) == doctest::Approx(1500.0).epsilon(0.01));
    CHECK(std::min(behind, front) == doctest::Approx(0.0).epsilon(0.01));
    ExtrudeParams p;
    p.height = 1500.0;
    p.profile = Profile::Battered;
    p.cell = h.cell;
    p.detail = 2.0;
    const bool flip = behind < front;
    const CliffMesh c = extrudeWall(w.line, &h, p, flip);
    REQUIRE(c.mesh.t.size() > 100);
    CHECK(c.stats.baseClearance < 0.0);
    const geo::Bounds b = c.mesh.bounds();
    CHECK(b.max.y > 1300.0);
    CHECK(b.max.y < 1600.0);
    CHECK(c.stats.layers > 3);
    Profile pr;
    CHECK(parseProfile("stepped", &pr));
    ExtrudeParams st;
    st.profile = Profile::Stepped;
    st.steps = 3;
    CHECK(profilePoints(st).size() > 13);
}

TEST_CASE("cliffs: terrainCliffs, terrainCliffMesh, terrainCheck and terrainSendCliffs work end to end on a terrain")
{
    loadTerrain();
    json t = run("terrainCreate", {{"size", {20000.0, 20000.0}}, {"resolution", 129}, {"amplitude", 6000.0}, {"featureSize", 6000.0}, {"seed", 3},
                                   {"name", "Crag"}});
    json c = run("terrainCliffs", {{"args", {t["mesh"]}}, {"slope", 40.0}, {"minHeight", 300.0}, {"minArea", 10.0}});
    INFO(c.dump().substr(0, 2000));
    REQUIRE(c["count"].get<int>() > 0);
    CHECK(c["regions"][0]["name"] == "cliff_1");
    CHECK(c["regions"][0]["reference"] == "region:cliff_1");
    // The stored region is a region: reference for every terrain command.
    json s = run("terrainSample", {{"args", {t["mesh"]}}, {"points", {"region:cliff_1"}}});
    CHECK(s.size() == 1);

    json m = run("terrainCliffMesh", {{"args", {t["mesh"]}}, {"region", "cliff_1"}, {"detail", 1.0}});
    INFO(m.dump().substr(0, 2000));
    REQUIRE(m["cliffs"].size() == 1);
    CHECK(m["cliffs"][0]["stats"]["triangles"].get<int>() > 100);
    CHECK(m["cliffs"][0]["stats"]["baseClearance"].get<double>() < 0.0);
    CHECK(m["ground"]["recessed"].get<int>() > 0);
    json info = run("terrainInfo", {{"args", {t["mesh"]}}});
    CHECK(info.dump().find("cliffTalus") != std::string::npos);

    json cover = run("terrainGroundCover", {{"args", {t["mesh"]}}, {"rules", {"scree", json{{"preset", "hangingRoots"}, {"density", 0.2}}}}});
    INFO(cover.dump().substr(0, 1000));
    REQUIRE(cover["rules"].size() == 2);
    CHECK(cover["rules"][0]["placed"].get<int>() > 0);
    CHECK(cover["missing"].empty());

    json check = run("terrainCheck", {{"args", {t["mesh"]}}});
    for (const json& i : check["issues"])
    {
        INFO(i.dump());
        CHECK(i["type"] != "floatingCliff");
    }
    json over = run("terrainCheck", {{"args", {t["mesh"]}}, {"triangleBudget", 10.0}});
    bool budget = false;
    for (const json& i : over["issues"])
        budget = budget || i["type"] == "tileOverBudget";
    CHECK(budget);

    json send = run("terrainSendCliffs", {{"args", {t["mesh"]}}, {"dryRun", true}, {"cellSize", 3200.0}});
    CHECK(send["tileCount"].get<int>() > 0);
    CHECK(send["tiles"][0]["lods"].get<int>() == 3);

    json inset = run("terrainCliffMesh", {{"args", {t["mesh"]}}, {"region", "cliff_1"}, {"detail", 1.0}, {"mode", "inset"}});
    CHECK(inset["ground"]["holes"].get<int>() > 0);
    json check2 = run("terrainCheck", {{"args", {t["mesh"]}}});
    for (const json& i : check2["issues"])
    {
        INFO(i.dump());
        CHECK(i["type"] != "cliffCrack");
    }
    json list = run("terrainCliffs", {{"args", {t["mesh"]}}, {"list", true}});
    CHECK(list["cliffs"].size() == 1);
    CHECK(list["regions"].contains("cliff_1"));
}

TEST_CASE("cliffs: lowPoly makes a reduced cliff with normal and AO maps baked from the full one")
{
    loadTerrain();
    run("newScene");
    json t = run("terrainCreate", {{"size", {20000.0, 20000.0}}, {"resolution", 129}, {"amplitude", 6000.0}, {"featureSize", 6000.0}, {"seed", 3},
                                   {"name", "LowCrag"}});
    json c = run("terrainCliffs", {{"args", {t["mesh"]}}, {"slope", 40.0}, {"minHeight", 300.0}, {"minArea", 10.0}});
    REQUIRE(c["count"].get<int>() > 0);
    // An unsaved scene bakes into the working folder.
    const std::filesystem::path before = std::filesystem::current_path(), dir = std::filesystem::temp_directory_path() / "lila_cliff_lowpoly";
    std::filesystem::create_directories(dir);
    std::filesystem::current_path(dir);
    json m = run("terrainCliffMesh", {{"args", {t["mesh"]}}, {"region", "cliff_1"}, {"detail", 1.0}, {"lowPoly", true}, {"lowPolyRatio", 0.1},
                                      {"bakeResolution", 1024}});
    std::filesystem::current_path(before);
    REQUIRE(m["cliffs"].size() == 1);
    const json& low = m["cliffs"][0]["lowPoly"];
    REQUIRE(low.is_object());
    CHECK_FALSE(low.contains("warning"));
    const int high = m["cliffs"][0]["stats"]["triangles"].get<int>();
    CHECK(low["triangles"].get<int>() > 0);
    CHECK(low["triangles"].get<int>() < high / 5);
    REQUIRE(low.contains("bake"));
    // At 1024 the lint sees uv-collapsed triangles a squashed layout would leave.
    CHECK(low["bake"]["lint"]["issues"].empty());
    // Headless there's no GPU backend, and the result says so.
    CHECK(low.value("backendNote", std::string()).find("CPU") != std::string::npos);
    std::set<std::string> maps;
    for (const json& o : low["bake"]["outputs"])
        if (std::filesystem::exists(o["file"].get<std::string>()))
            maps.insert(o["map"].get<std::string>());
    CHECK(maps == std::set<std::string>{"ao", "normal"});
}

TEST_CASE("cliffs: a terrain spec's cliffs entry finds and meshes the cliffs")
{
    loadTerrain();
    json spec = {{"name", "SpecCrag"},
                 {"size", {20000.0, 20000.0}},
                 {"resolution", 129},
                 {"source", {{"type", "noise"}, {"amplitude", 6000.0}, {"featureSize", 6000.0}}},
                 {"seed", 3},
                 {"cliffs", {{"slope", 40.0}, {"minHeight", 300.0}, {"minArea", 10.0}, {"mesh", {{"region", "cliff_1"}, {"detail", 1.0}}}}},
                 {"check", false}};
    json r = run("terrainSpec", {{"spec", spec}});
    INFO(r.dump().substr(0, 3000));
    CHECK(r["ok"] == true);
    bool meshed = false;
    for (const json& s : r["steps"])
        meshed = meshed || s.value("step", std::string()) == "cliffs.mesh";
    CHECK(meshed);
}

TEST_CASE("cliffs: terrainCliffExtrude raises a live wall and stamps the plateau behind it")
{
    loadTerrain();
    json t = run("terrainCreate", {{"size", {12800.0, 12800.0}}, {"resolution", 129}, {"flat", true}, {"name", "Ledge"}});
    const json cv = run("curve", {{"points", {{-5000.0, 0.0, 0.0}, {5000.0, 0.0, 0.0}}}, {"type", "poly"}, {"name", "LedgeEdge"}});
    json e = run("terrainCliffExtrude", {{"args", {t["mesh"]}}, {"curve", cv}, {"height", 1200.0}, {"profile", "battered"}});
    INFO(e.dump().substr(0, 1500));
    CHECK(e["ground"]["stamped"].get<int>() > 0);
    const double a = heightAt(t, 0.0, -600.0), b = heightAt(t, 0.0, 600.0);
    CHECK(std::max(a, b) == doctest::Approx(1200.0).epsilon(0.02));
    CHECK(std::min(a, b) == doctest::Approx(0.0).epsilon(0.02));
    CHECK(e["wall"]["walls"][0]["triangles"].get<int>() > 100);
    // Live: moving the curve moves the plateau.
    run("move", {{"args", {cv}}, {"by", {0.0, 0.0, 2000.0}}});
    const double a2 = heightAt(t, 0.0, 2000.0 - 600.0), b2 = heightAt(t, 0.0, 2000.0 + 600.0);
    CHECK(std::max(a2, b2) == doctest::Approx(1200.0).epsilon(0.02));
    CHECK(std::min(a2, b2) == doctest::Approx(0.0).epsilon(0.02));
}
