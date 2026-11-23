#include <doctest/doctest.h>

#include "TestUtil.h"
#include "bake/Backend.h"
#include "bake/Bake.h"
#include "bake/CpuBackend.h"
#include "bake/BakeInputs.h"
#include "bake/BakeNodes.h"
#include "bake/Maps.h"
#include "bake/Naming.h"
#include "bake/Targets.h"
#include "core/ImageIO.h"
#include "core/Workspace.h"
#include "geo/Mesh.h"
#include "geo/Primitives.h"
#include "ops/Command.h"
#include "ops/Undo.h"
#include "scene/Graph.h"
#include "scene/Materials.h"
#include "scene/MeshNodes.h"
#include "scene/Modifiers.h"

#include <glm/gtc/constants.hpp>

#include <cmath>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <set>

using namespace lila;
using namespace baketest;
using json = nlohmann::json;
namespace fs = std::filesystem;

TEST_CASE("naming tokens expand, stay file safe and take UDIMs")
{
    bake::NameTokens t;
    t.project = "Proj";
    t.group = "rock_bake";
    t.mesh = "env:rock_low";
    t.material = "M_Rock";
    t.map = "normal";
    t.ext = "png";
    t.udim = 1002;
    t.width = t.height = 2048;
    CHECK(bake::expandName("{mesh}_{map}", t) == "env_rock_low_normal.png");
    CHECK(bake::expandName("T_{material}_{map}_{res}.{ext}", t) == "T_M_Rock_normal_2048.png");
    CHECK(bake::expandName("{project}/{group}_{map}.{udim}", t) == "Proj/rock_bake_normal.1002.png");
    CHECK(bake::expandName("{material}_{map}", t, true) == "M_Rock_normal.1002.png");
    CHECK(bake::expandName("{material}_{map}.{ext}", t, true) == "M_Rock_normal.1002.png");
    CHECK(bake::expandName("{material}_{udim}", t, true) == "M_Rock_1002.png");
    CHECK(bake::expandName("T_{[!M_]material}_{map}", t) == "T_Rock_normal.png");
    CHECK(bake::expandName("T_{[!M_][!_low]mesh}_{map}", t) == "T_env_rock_normal.png");
    CHECK(bake::expandName("{[!X_]material}", t) == "M_Rock.png");
    CHECK(bake::hasToken("{[!M_]material}", "material"));
    CHECK(bake::unknownTokens("{[!M_]material}_{nope}") == std::vector<std::string>{"nope"});
    t.height = 1024;
    CHECK(bake::expandName("{map}_{res}", t) == "normal_2048x1024.png");
    CHECK(bake::expandName("{map}_{nope}", t) == "normal_{nope}.png");
    CHECK(bake::unknownTokens("{map}_{nope}_{udim}") == std::vector<std::string>{"nope"});
    CHECK(bake::fileSafe("a:b|c d") == "a_b_c_d");
}

TEST_CASE("auto grouping pairs _low/_high, _lo/_hi and _LP/_HP names")
{
    std::vector<std::string> unmatched;
    auto groups = bake::matchAutoGroups({"rock_low", "rock_high", "rock_high1", "rock_high_2", "Crate_LP", "Crate_HP", "pipe_lo", "pipe_hi",
                                         "lonely_low", "tree", "ns:door_low", "ns:door_high"},
                                        &unmatched);
    REQUIRE(groups.size() == 4);
    CHECK(groups[0].base == "rock");
    CHECK(groups[0].lows == std::vector<std::string>{"rock_low"});
    CHECK(groups[0].highs == std::vector<std::string>{"rock_high", "rock_high1", "rock_high_2"});
    CHECK(groups[1].base == "Crate");
    CHECK(groups[2].base == "pipe");
    CHECK(groups[3].base == "ns:door");
    CHECK(std::set<std::string>(unmatched.begin(), unmatched.end()) == std::set<std::string>{"lonely_low", "tree"});
}

TEST_CASE("a kept sculpt's _high pairs with the mesh named its stem")
{
    std::vector<std::string> unmatched;
    auto child = bake::matchAutoGroups({"|grp|Tree", "|grp|Tree|Tree_high"}, &unmatched);
    REQUIRE(child.size() == 1);
    CHECK(child[0].lows == std::vector<std::string>{"|grp|Tree"});
    auto groups = bake::matchAutoGroups({"Rock", "Rock_high", "crate_low", "crate_low_high"}, &unmatched);
    REQUIRE(groups.size() == 2);
    CHECK(groups[0].base == "Rock");
    CHECK(groups[0].lows == std::vector<std::string>{"Rock"});
    CHECK(groups[1].base == "crate_low");
    CHECK(groups[1].lows == std::vector<std::string>{"crate_low"});
    CHECK(groups[1].highs == std::vector<std::string>{"crate_low_high"});
    CHECK(unmatched.empty());
}

TEST_CASE("image files round trip: png 8/16, exr half, tga")
{
    const fs::path dir = tempDir("lila_bake_io");
    image::Image img{3, 2, {}};
    for (int i = 0; i < 6; ++i)
        img.rgba.insert(img.rgba.end(), {float(i) / 5.0f, 0.25f, 1.0f - float(i) / 5.0f, 1.0f});
    for (auto [file, bits, tol] : {std::tuple{"a.png", 8, 0.5f / 255.0f}, {"b.png", 16, 0.5f / 65535.0f}, {"c.exr", 16, 1e-3f}, {"d.tga", 8, 0.5f / 255.0f}})
    {
        image::write(dir / file, img, bits);
        std::string err;
        image::Image back = image::read(dir / file, &err);
        INFO(file << " " << err);
        REQUIRE(back.width == 3);
        REQUIRE(back.height == 2);
        for (size_t i = 0; i < img.rgba.size(); ++i)
            CHECK(std::abs(back.rgba[i] - img.rgba[i]) <= tol);
    }
}

TEST_CASE("tga is run-length encoded by default and reads back exactly; uncompressed on request")
{
    const fs::path dir = tempDir("lila_bake_tga_rle");
    // Rows mix long runs (past the 128-texel packet limit), raw spans and runs of two.
    const int W = 300, H = 5;
    image::Image img{W, H, std::vector<float>(size_t(W) * H * 4)};
    for (int y = 0; y < H; ++y)
        for (int x = 0; x < W; ++x)
        {
            float* p = &img.rgba[(size_t(y) * W + size_t(x)) * 4];
            const int v = x < 200 ? y * 40 : x < 260 ? (x * 7 + y) % 256 : ((x / 2) * 31) % 256;
            p[0] = float(v) / 255.0f;
            p[1] = float(255 - v) / 255.0f;
            p[2] = float(y * 50) / 255.0f;
            p[3] = x % 3 == 0 && x >= 260 ? 0.0f : 1.0f;
        }
    image::write(dir / "rle.tga", img, image::WriteOptions{});
    image::write(dir / "raw.tga", img, image::WriteOptions{.compress = false});
    CHECK(fs::file_size(dir / "raw.tga") == 18u + size_t(W) * H * 4);
    CHECK(fs::file_size(dir / "rle.tga") < fs::file_size(dir / "raw.tga") / 2);
    for (const char* file : {"rle.tga", "raw.tga"})
    {
        std::string err;
        const image::Image back = image::read(dir / file, &err);
        INFO(file << " " << err);
        REQUIRE(back.width == W);
        REQUIRE(back.height == H);
        for (size_t i = 0; i < img.rgba.size(); ++i)
            REQUIRE(std::abs(back.rgba[i] - img.rgba[i]) <= 0.5f / 255.0f);
    }
}

TEST_CASE("dilation fills padding from the nearest island and never changes covered texels")
{
    const int W = 32, H = 4;
    std::vector<glm::vec4> v(W * H, glm::vec4(0.0f));
    std::vector<uint8_t> cov(W * H, 0);
    const glm::vec4 A(1, 0, 0, 1), B(0, 0, 1, 1);
    for (int y = 0; y < H; ++y)
        for (int x = 0; x < W; ++x)
        {
            if (x < 10)
                v[y * W + x] = A, cov[y * W + x] = 1;
            if (x >= 20)
                v[y * W + x] = B, cov[y * W + x] = 1;
        }
    const std::vector<uint8_t> before = cov;
    bake::dilateCpu(v, cov, W, H, 16);
    CHECK(cov == before);
    for (int y = 0; y < H; ++y)
        for (int x = 0; x < W; ++x)
        {
            INFO(x << "," << y);
            CHECK(v[y * W + x] == (x < 15 ? A : B));
        }

    std::vector<glm::vec4> w(W * H, glm::vec4(0.0f));
    std::vector<uint8_t> c2(W * H, 0);
    w[0] = A;
    c2[0] = 1;
    bake::dilateCpu(w, c2, W, H, 2);
    CHECK(w[2] == A);
    CHECK(w[3] == glm::vec4(0.0f));
}

TEST_CASE("plane bump: height and tangent normals match the analytic surface")
{
    newScene();
    const fs::path dir = tempDir("lila_bake_bump");
    const double A = 3.0, L = 100.0, k = glm::two_pi<double>() / L;
    auto f = [&](double x, double z) { return A * std::cos(k * x) + A * std::cos(k * z); };
    const std::string low = addMesh("bump_low", plane(1));
    const std::string high = addMesh("bump_high", displaced(200, f));
    const std::string p = project(dir, 64);
    ops::execute("bakeGroup", {{"args", {"bump"}}, {"project", p}, {"lows", {low}}, {"highs", {high}}, {"frontal", 10.0}, {"rear", 10.0}});
    ops::execute("setBakeMap", {{"args", {"normal"}}, {"project", p}});
    const json r = ops::execute("bake", {{"project", p}, {"maps", {"height", "normal"}}});
    CHECK(r["stats"]["misses"] == 0);
    CHECK(r["stats"]["samples"] == 64 * 64);

    const json& hOut = outputFor(r, "height");
    const double range = hOut["meta"]["heightRange"].get<double>();
    CHECK(range == doctest::Approx(2.0 * A).epsilon(0.03));
    const image::Image h = readImage(hOut);
    const image::Image n = readImage(outputFor(r, "normal"));
    double worstH = 0.0, worstN = 0.0;
    for (int y = 0; y < 64; ++y)
        for (int x = 0; x < 64; ++x)
        {
            const double u = (x + 0.5) / 64.0, v = 1.0 - (y + 0.5) / 64.0;
            const double wx = u * 100.0 - 50.0, wz = 50.0 - v * 100.0;
            const double decoded = (texel(h, x, y).r - 0.5) * 2.0 * range;
            worstH = std::max(worstH, std::abs(decoded - f(wx, wz)));
            // n = (-fx, 1, -fz); tangent +x, bitangent +v = -z, DirectX stores green along -bitangent.
            const double fx = -A * k * std::sin(k * wx), fz = -A * k * std::sin(k * wz);
            const glm::vec3 world = glm::normalize(glm::vec3(float(-fx), 1.0f, float(-fz)));
            const glm::vec3 expected(world.x, world.z, world.y);
            const glm::vec3 got = glm::vec3(texel(n, x, y)) * 2.0f - 1.0f;
            worstN = std::max(worstN, double(angleDeg(got, expected)));
        }
    CHECK(worstH < 0.05);
    CHECK(worstN < 0.5);

    // OpenGL flips green only.
    ops::execute("setBakeMap", {{"args", {"normal"}}, {"project", p}, {"options", {{"greenUp", true}}}});
    const json r2 = ops::execute("bake", {{"project", p}, {"maps", {"normal"}}});
    const image::Image gl = readImage(outputFor(r2, "normal"));
    for (int y = 0; y < 64; y += 7)
        for (int x = 0; x < 64; x += 5)
        {
            CHECK(texel(gl, x, y).r == doctest::Approx(texel(n, x, y).r).epsilon(1e-3));
            CHECK(texel(gl, x, y).g == doctest::Approx(1.0f - texel(n, x, y).g).epsilon(2e-3));
        }
}

TEST_CASE("sphere over an icosphere: radial normals and positions")
{
    newScene();
    const fs::path dir = tempDir("lila_bake_sphere");
    geo::SphereOptions so;
    so.subdivAxis = 32;
    so.subdivHeight = 32;
    const std::string low = addMesh("ball_low", geo::makeSphere(so));
    geo::IcoSphereOptions io;
    io.subdivisions = 6;
    const std::string high = addMesh("ball_high", geo::makeIcoSphere(io));
    const std::string p = project(dir, 64);
    ops::execute("bakeGroup", {{"args", {"ball"}}, {"project", p}, {"lows", {low}}, {"highs", {high}}, {"frontal", 3.0}, {"rear", 3.0}});
    const json r = ops::execute("bake", {{"project", p}, {"maps", {"objectNormal", "position", "normal"}}});
    const json& stats = r["stats"];
    CHECK(stats["misses"].get<double>() < 0.001 * stats["samples"].get<double>());
    const json& posOut = outputFor(r, "position");
    const glm::vec3 lo(posOut["meta"]["boundsMin"][0].get<float>(), posOut["meta"]["boundsMin"][1].get<float>(), posOut["meta"]["boundsMin"][2].get<float>());
    const glm::vec3 hi(posOut["meta"]["boundsMax"][0].get<float>(), posOut["meta"]["boundsMax"][1].get<float>(), posOut["meta"]["boundsMax"][2].get<float>());
    const image::Image pos = readImage(posOut);
    const image::Image obj = readImage(outputFor(r, "objectNormal"));
    const image::Image tan = readImage(outputFor(r, "normal"));
    double worst = 0.0, sumTan = 0.0;
    int count = 0;
    for (int y = 2; y < 62; ++y)
        for (int x = 0; x < 64; ++x)
        {
            const glm::vec3 pp = lo + glm::vec3(texel(pos, x, y)) * (hi - lo);
            if (glm::length(pp) < 1.0f)
                continue;
            const glm::vec3 n = glm::vec3(texel(obj, x, y)) * 2.0f - 1.0f;
            worst = std::max(worst, double(angleDeg(n, pp)));
            sumTan += angleDeg(glm::vec3(texel(tan, x, y)) * 2.0f - 1.0f, glm::vec3(0, 0, 1));
            ++count;
        }
    REQUIRE(count > 1000);
    CHECK(worst < 1.5);
    // The low's own smooth normals are radial too, so the tangent map is nearly flat.
    CHECK(sumTan / count < 1.0);
}

TEST_CASE("material ID maps hold exact colours with no averaging")
{
    newScene();
    const fs::path dir = tempDir("lila_bake_id");
    geo::Mesh hi = plane(16);
    std::vector<uint32_t> left, right;
    for (geo::Index f = 0; f < hi.faceCapacity(); ++f)
        (hi.faceCentroid(f).x < 0.0 ? left : right).push_back(f);
    scene::materials::assign(hi, "matA", left);
    scene::materials::assign(hi, "matB", right);
    for (geo::Index p = 0; p < hi.pointCapacity(); ++p)
        hi.setPosition(p, hi.position(p) + glm::dvec3(0.0, 1.0, 0.0));
    const std::string low = addMesh("tile_low", plane(1));
    const std::string high = addMesh("tile_high", std::move(hi));
    const std::string p = project(dir, 33, 16, "png");
    ops::execute("bakeGroup", {{"args", {"tile"}}, {"project", p}, {"lows", {low}}, {"highs", {high}}, {"frontal", 3.0}, {"rear", 3.0}});
    const json r = ops::execute("bake", {{"project", p}, {"maps", {"id"}}});
    const json& out = outputFor(r, "id");
    const glm::vec3 a = bake::idColour("matA"), b = bake::idColour("matB");
    CHECK(out["meta"]["legend"]["matA"] == json{std::lround(a.r * 255), std::lround(a.g * 255), std::lround(a.b * 255)});
    const image::Image img = readImage(out);
    int na = 0, nb = 0;
    for (int y = 0; y < img.height; ++y)
        for (int x = 0; x < img.width; ++x)
        {
            const glm::vec3 c(texel(img, x, y));
            const bool isA = glm::all(glm::lessThan(glm::abs(c - a), glm::vec3(1e-6f)));
            const bool isB = glm::all(glm::lessThan(glm::abs(c - b), glm::vec3(1e-6f)));
            INFO(x << "," << y << " " << c.r << " " << c.g << " " << c.b);
            CHECK((isA || isB));
            na += isA;
            nb += isB;
            // 33 texels: column 16 straddles the material border and must still hold one exact colour.
            if (x < 16)
                CHECK(isA);
            if (x > 16)
                CHECK(isB);
        }
    CHECK(na > 0);
    CHECK(nb > 0);

    // A palette option overrides a colour exactly.
    ops::execute("setBakeMap", {{"args", {"id"}}, {"project", p}, {"options", {{"palette", {{"matB", "#ff8000"}}}}}});
    const image::Image img2 = readImage(outputFor(ops::execute("bake", {{"project", p}, {"maps", {"id"}}}), "id"));
    const glm::vec3 c(texel(img2, 30, 10));
    CHECK(c.r == doctest::Approx(1.0f));
    CHECK(c.g == doctest::Approx(128.0f / 255.0f));
    CHECK(c.b == doctest::Approx(0.0f));
}

TEST_CASE("masks maps pack a 0/1 mask per material into channels")
{
    newScene();
    const fs::path dir = tempDir("lila_bake_packed_masks");
    geo::Mesh hi = plane(16);
    std::vector<uint32_t> left, right;
    for (geo::Index f = 0; f < hi.faceCapacity(); ++f)
        (hi.faceCentroid(f).x < 0.0 ? left : right).push_back(f);
    scene::materials::assign(hi, "matB", left);
    scene::materials::assign(hi, "matA", right);
    for (geo::Index p = 0; p < hi.pointCapacity(); ++p)
        hi.setPosition(p, hi.position(p) + glm::dvec3(0.0, 1.0, 0.0));
    const std::string low = addMesh("tile_low", plane(1));
    const std::string high = addMesh("tile_high", std::move(hi));
    const std::string p = project(dir, 33, 16, "png");
    ops::execute("bakeGroup", {{"args", {"tile"}}, {"project", p}, {"lows", {low}}, {"highs", {high}}, {"frontal", 3.0}, {"rear", 3.0}});
    ops::execute("setBakeMap", {{"args", {"masks"}}, {"project", p}, {"enabled", true}});
    const json r = ops::execute("bake", {{"project", p}, {"maps", {"masks"}}});
    const json& out = outputFor(r, "masks");
    CHECK(out["meta"]["legend"]["R"] == "matA");
    CHECK(out["meta"]["legend"]["G"] == "matB");
    CHECK(out["meta"]["mapsNeeded"] == 1);
    const image::Image img = readImage(out);
    const glm::vec4 l = texel(img, 4, 8), rt = texel(img, 28, 8);
    CHECK(l.r == doctest::Approx(0.0f));
    CHECK(l.g == doctest::Approx(1.0f));
    CHECK(rt.r == doctest::Approx(1.0f));
    CHECK(rt.g == doctest::Approx(0.0f));
    CHECK(rt.b == doctest::Approx(0.0f));
}

TEST_CASE("UV island and coverage masks")
{
    newScene();
    const fs::path dir = tempDir("lila_bake_masks");
    geo::Mesh a = plane(1, {0.0f, 0.0f}, {0.4f, 1.0f});
    geo::Mesh b = plane(1, {0.6f, 0.0f}, {1.0f, 1.0f});
    for (geo::Index pt = 0; pt < b.pointCapacity(); ++pt)
        b.setPosition(pt, b.position(pt) + glm::dvec3(200.0, 0.0, 0.0));
    a.append(b);
    const std::string low = addMesh("two_low", std::move(a));
    const std::string p = project(dir, 20, 4, "png");
    ops::execute("bakeGroup", {{"args", {"two"}}, {"project", p}, {"lows", {low}}, {"self", true}});
    const json r = ops::execute("bake", {{"project", p}, {"maps", {"uvIslands", "alpha"}}});
    const image::Image isl = readImage(outputFor(r, "uvIslands"));
    const image::Image alpha = readImage(outputFor(r, "alpha"));
    const glm::vec3 c0 = bake::paletteColour(0), c1 = bake::paletteColour(1);
    CHECK(glm::vec3(texel(isl, 2, 5)) == c0);
    CHECK(glm::vec3(texel(isl, 17, 5)) == c1);
    // Padding 4: the gap's middle texels come from the nearer island, exactly.
    CHECK(glm::vec3(texel(isl, 8, 5)) == c0);
    CHECK(glm::vec3(texel(isl, 11, 5)) == c1);
    CHECK(texel(alpha, 2, 5).r == doctest::Approx(1.0f));
    CHECK(texel(alpha, 10, 5).r == doctest::Approx(0.0f));
}

TEST_CASE("groups are isolated: overlapping groups trace only their own highs and can share one texture")
{
    newScene();
    const fs::path dir = tempDir("lila_bake_isolate");
    auto lift = [](geo::Mesh m, double y) {
        for (geo::Index p = 0; p < m.pointCapacity(); ++p)
            m.setPosition(p, m.position(p) + glm::dvec3(0.0, y, 0.0));
        return m;
    };
    // Both lows sit in the same place; group 2's high lies under group 1's, so without isolation group 2 would hit group 1.
    const std::string low1 = addMesh("a_low", plane(1, {0.0f, 0.0f}, {0.5f, 1.0f}));
    const std::string low2 = addMesh("b_low", plane(1, {0.5f, 0.0f}, {1.0f, 1.0f}));
    const std::string high1 = addMesh("a_high", lift(plane(4), 1.0));
    const std::string high2 = addMesh("b_high", lift(plane(4), 0.5));
    const std::string p = project(dir, 32, 1, "exr", "{project}_{map}");
    for (auto [grp, low, high] : {std::tuple{"a", low1, high1}, {"b", low2, high2}})
        ops::execute("bakeGroup", {{"args", {grp}}, {"project", p}, {"lows", {low}}, {"highs", {high}}, {"frontal", 3.0}, {"rear", 3.0}});
    ops::execute("setBakeMap", {{"args", {"ao"}}, {"project", p}, {"options", {{"rays", 16}, {"distance", 50.0}}}});
    const json r = ops::execute("bake", {{"project", p}, {"maps", {"height", "ao"}}});
    REQUIRE(r["outputs"].size() == 2);
    const json& hOut = outputFor(r, "height");
    CHECK(hOut["groups"].size() == 2);
    const double range = hOut["meta"]["heightRange"].get<double>();
    const image::Image h = readImage(hOut);
    for (int y = 0; y < 32; y += 3)
    {
        CHECK((texel(h, 5, y).r - 0.5) * 2.0 * range == doctest::Approx(1.0).epsilon(0.01));
        CHECK((texel(h, 26, y).r - 0.5) * 2.0 * range == doctest::Approx(0.5).epsilon(0.01));
    }
    const image::Image ao = readImage(outputFor(r, "ao"));
    CHECK(texel(ao, 26, 16).r == doctest::Approx(1.0f));

    // With every group as occluders, group 1's high shadows group 2's.
    ops::execute("setBakeMap", {{"args", {"ao"}}, {"project", p}, {"options", {{"scope", "all"}}}});
    const image::Image aoAll = readImage(outputFor(ops::execute("bake", {{"project", p}, {"maps", {"ao"}}}), "ao"));
    CHECK(texel(aoAll, 26, 16).r < 0.2f);
    CHECK(texel(aoAll, 5, 16).r == doctest::Approx(1.0f));
}

TEST_CASE("bake-time smooth modifiers shape the high without touching the scene")
{
    newScene();
    const fs::path dir = tempDir("lila_bake_smooth");
    const std::string low = addMesh("box_low", geo::makeCube());
    scene::Graph& g = graph();
    const scene::NodeId id = g.find(low)->id();
    const std::string p = project(dir, 32);
    ops::execute("bakeGroup", {{"args", {"box"}}, {"project", p}, {"lows", {low}}, {"self", true}, {"preset", "Smooth x2"}, {"rear", 40.0}});
    const json info = ops::execute("bakeStatus", {{"project", p}});
    REQUIRE(info["groups"].size() == 1);
    CHECK(info["groups"][0]["highs"][0]["self"] == true);
    CHECK(info["groups"][0]["highs"][0]["modifiers"][0]["kind"] == "subdivision");

    const size_t nodes = g.nodes().size();
    const std::string undoLabel = UndoStack::get().undoLabel();
    const json r = ops::execute("bake", {{"project", p}, {"maps", {"height"}}});
    CHECK(r["stats"]["groups"][0]["highTriangles"] == 6 * 16 * 2);
    CHECK(r["stats"]["groups"][0]["lowTriangles"] == 12);
    // The smoothed cube lies inside the low, deepest at the corners.
    CHECK(outputFor(r, "height")["meta"]["heightRange"].get<double>() > 5.0);
    CHECK(g.nodes().size() == nodes);
    CHECK(scene::modifiers::stack(g, id).empty());
    CHECK(UndoStack::get().undoLabel() == undoLabel);
    const scene::MeshPtr base = std::get<scene::MeshPtr>(g.storedValue({id, scene::MeshNode::kInMesh, -1}));
    CHECK(base->numFaces() == 6);

    // Auto Bevel + Smooth runs a modifier that computes through its node rather than an apply function.
    ops::execute("bakeGroup", {{"args", {"box"}}, {"preset", "Auto Bevel + Smooth"}});
    const json r2 = ops::execute("bake", {{"project", p}, {"maps", {"height"}}});
    // Lint findings about the cube itself don't concern the modifier.
    for (const json& w : r2["warnings"])
        CHECK_MESSAGE(w.get<std::string>().starts_with("lint "), w.get<std::string>());
    CHECK(r2["stats"]["groups"][0]["highTriangles"].get<int>() > 6 * 16 * 2);
    CHECK(g.nodes().size() == nodes);
    CHECK(scene::modifiers::stack(g, id).empty());
}

TEST_CASE("bake projects round trip through .lila and .lilaa and edits undo")
{
    for (const char* ext : {".lila", ".lilaa"})
    {
        newScene();
        const fs::path dir = tempDir("lila_bake_roundtrip");
        const std::string low = addMesh("crate_low", geo::makeCube());
        const std::string high = addMesh("crate_high", geo::makeCube());
        const std::string p = project(dir, 512, 4, "png", "T_{mesh}_{map}");
        ops::execute("bakeGroup", {{"args", {"crate"}},  {"project", p},       {"lows", {low}},     {"highs", {high}},
                                   {"preset", "Auto Bevel + Smooth"}, {"frontal", 2.5}, {"aoScope", "self"}});
        ops::execute("setBakeMap", {{"args", {"normal"}}, {"project", p}, {"suffix", "N"}, {"options", {{"greenUp", true}}}});
        const json before = ops::execute("bakeStatus", {{"project", p}});
        const fs::path file = dir / (std::string("scene") + ext);
        ops::execute("saveScene", {{"args", {file.generic_string()}}});
        newScene();
        CHECK(bake::projects(graph()).empty());
        ops::execute("openScene", {{"args", {file.generic_string()}}, {"force", true}});
        const json after = ops::execute("bakeStatus", {{"project", p}});
        CHECK(after["settings"] == before["settings"]);
        CHECK(after["groups"] == before["groups"]);
        CHECK(after["groups"][0]["highs"][0]["modifiers"].size() == 2);
    }

    newScene();
    const std::string low = addMesh("u_low", geo::makeCube());
    ops::execute("createBakeProject", {{"name", "P"}});
    ops::execute("bakeGroup", {{"args", {"g"}}, {"lows", {low}}, {"frontal", 1.0}});
    ops::execute("bakeGroup", {{"args", {"g"}}, {"frontal", 7.0}});
    CHECK(bake::groupSettings(*graph().find("g")).frontal == 7.0);
    UndoStack::get().undo();
    CHECK(bake::groupSettings(*graph().find("g")).frontal == 1.0);
    UndoStack::get().redo();
    CHECK(bake::groupSettings(*graph().find("g")).frontal == 7.0);
}

TEST_CASE("target profiles resolve built in, user and workspace files and drive bakeForTarget")
{
    newScene();
    const auto painter = bake::findTarget("substancePainter");
    REQUIRE(painter);
    CHECK(painter->output["name"] == "{material}_{map}");
    std::vector<std::string> skipped;
    const bake::ProjectSettings s = bake::applyTarget(bake::ProjectSettings::defaults(), *painter, false, &skipped);
    CHECK(s.output.bits == 16);
    CHECK(s.target == "substancePainter");
    REQUIRE(!s.maps.empty());
    CHECK(s.maps[0].map == "normal");
    CHECK(s.maps[0].options["greenUp"] == true);
    std::set<std::string> ids;
    for (const bake::MapSettings& m : s.maps)
        ids.insert(m.map);
    CHECK(ids == std::set<std::string>{"normal", "objectNormal", "ao", "curvature", "position", "thickness", "height", "cavity"});
    CHECK(skipped.empty());
    CHECK_FALSE(s.autoAssign);

    // Unreal needs only the normal and ORM; the rest are optional. It wires them into the lows' materials after baking.
    const auto unreal = bake::findTarget("unreal");
    REQUIRE(unreal);
    bake::ProjectSettings assigned = bake::ProjectSettings::defaults();
    assigned.autoAssign = true;
    CHECK_FALSE(bake::applyTarget(assigned, *painter, false, nullptr).autoAssign);
    const bake::ProjectSettings u = bake::applyTarget(bake::ProjectSettings::defaults(), *unreal, false, nullptr);
    std::set<std::string> uids;
    for (const bake::MapSettings& m : u.maps)
        uids.insert(m.map);
    CHECK(uids == std::set<std::string>{"normal", "orm"});
    CHECK(u.autoAssign);
    CHECK_FALSE(u.rebakeOnSave);
    std::set<std::string> optional;
    for (const bake::TargetMap& m : unreal->maps)
        if (m.optional)
            optional.insert(m.settings.map);
    CHECK(optional == std::set<std::string>{"height", "ao", "id", "baseColor", "emissive"});
    CHECK(bake::TargetProfile::fromJson(unreal->toJson()).pipeline == json{{"autoAssign", true}});
    CHECK_THROWS(bake::TargetProfile::fromJson({{"name", "x"}, {"maps", json::array()}, {"pipeline", {{"autoAssign", "yes"}}}}));

    // A workspace target of the same name replaces the built-in one.
    const fs::path ws = tempDir("lila_bake_workspace");
    fs::create_directories(ws / ".lila" / "bake" / "targets");
    {
        std::ofstream f(ws / ".lila" / "bake" / "targets" / "studio.json");
        f << R"({"name": "substancePainter", "label": "Studio Painter", "output": {"name": "SP_{mesh}_{map}"}, "maps": [{"map": "height", "suffix": "h"}]})";
    }
    Workspace::get().open(ws);
    const auto studio = bake::findTarget("substancePainter");
    REQUIRE(studio);
    CHECK(studio->label == "Studio Painter");
    Workspace::get().close();
    CHECK(bake::findTarget("substancePainter")->label == "Smart Material Inputs");

    // bakeForTarget: auto-groups, applies Painter's maps and names, reports what it skipped.
    const fs::path dir = tempDir("lila_bake_painter");
    geo::Mesh lowMesh = plane(1);
    scene::materials::assign(lowMesh, "M_Panel", {});
    const std::string low = addMesh("panel_low", std::move(lowMesh));
    addMesh("panel_high", displaced(16, [](double x, double) { return 0.02 * x; }));
    ops::execute("createBakeProject", {{"name", "SP"}, {"folder", dir.generic_string()}});
    ops::execute("setBakeOutput", {{"project", "SP"}, {"resolution", 16}, {"supersampling", 1}});
    const json r = ops::execute("bakeForTarget", {{"project", "SP"}, {"target", "substancePainter"}});
    CHECK(r["target"] == "substancePainter");
    CHECK(r["autoGroup"]["groups"].size() == 1);
    std::set<std::string> files;
    for (const json& o : r["outputs"])
    {
        files.insert(fs::path(o["file"].get<std::string>()).filename().string());
        CHECK(o["bits"] == 16);
        CHECK(fs::exists(o["file"].get<std::string>()));
    }
    CHECK(files == std::set<std::string>{"M_Panel_normal.png", "M_Panel_normalobj.png", "M_Panel_ao.png", "M_Panel_curvature.png",
                                         "M_Panel_position.png", "M_Panel_thickness.png", "M_Panel_height.png", "M_Panel_cavity.png"});
    CHECK(r["skipped"].empty());
    CHECK(fs::exists(r["manifest"].get<std::string>()));

    const json dry = ops::execute("bake", {{"project", "SP"}, {"dryRun", true}});
    CHECK(dry["dryRun"] == true);
    CHECK(dry["outputs"].size() == 8);
}

TEST_CASE("Auto frontal and rear are measured against the highs")
{
    newScene();
    const fs::path dir = tempDir("lila_bake_autodistance");
    addMesh("crate_low", geo::makeCube());
    geo::SphereOptions so;
    so.radius = 75.0;
    addMesh("crate_high", geo::makeSphere(so));
    ops::execute("createBakeProject", {{"name", "SP"}, {"folder", dir.generic_string()}});
    ops::execute("setBakeOutput", {{"project", "SP"}, {"resolution", 64}, {"supersampling", 4}});
    const json r = ops::execute("bakeForTarget", {{"project", "SP"}, {"target", "substancePainter"}});
    REQUIRE(r["autoGroup"]["groups"].size() == 1);
    const json& st = r["stats"];
    REQUIRE(st["samples"].get<double>() > 0.0);
    CHECK(st["misses"].get<double>() / st["samples"].get<double>() < 0.002);
    const json& grp = st["groups"][0];
    CHECK(grp["measured"] == true);
    CHECK(grp["frontalAuto"] == true);
    // The sphere sits 25 cm outside the face centres and up to 11.6 cm inside the corners (the very tips fall in the percentile's tail).
    CHECK(grp["frontal"].get<double>() >= 25.0);
    CHECK(grp["frontal"].get<double>() < 35.0);
    CHECK(grp["rear"].get<double>() >= 8.0);
    CHECK(grp["rear"].get<double>() < 20.0);

    // The auto distances cover the high, so lint has nothing to say about range or containment.
    for (const json& issue : r["lint"]["issues"])
    {
        CHECK(issue["check"] != "highRange");
        CHECK(issue["check"] != "cageContainment");
    }
    const json lint = ops::execute("bakeLint", {{"project", "SP"}});
    CHECK(lint["distances"][0]["measured"] == true);
    CHECK(lint["distances"][0]["frontal"].get<double>() == doctest::Approx(grp["frontal"].get<double>()));

    // No material: the texture set is the low's name.
    std::set<std::string> files;
    for (const json& o : r["outputs"])
        files.insert(fs::path(o["file"].get<std::string>()).filename().string());
    CHECK(files.contains("crate_low_normal.png"));
}

namespace
{
/**
 * Projection as the CPU backend, but secondary rays from a tracer built on first use under a lock, as the GPU backend
 * builds its CPU fallback. The build is parallel, so a band waiting on it must not pick up another band that then takes
 * the same lock.
 */
class LazyCpuBackend final : public bake::Backend, public bake::Tracer
{
public:
    std::string name() const override { return "lazyCpu"; }
    int bandRows(int, int) const override { return 1; }
    void prepare(const bake::BakeInputs& inputs) override
    {
        mInputs = &inputs;
        mProjection.prepare(inputs);
        std::scoped_lock lock(mMutex);
        mCpu.reset();
    }
    void traceProjection(size_t group, std::span<const bake::TexelSample> samples, std::span<bake::HitRecord> hits) const override
    {
        mProjection.traceProjection(group, samples, hits);
    }
    const bake::Tracer* tracer() const override { return this; }
    bool occluded(size_t group, bake::OccluderScope scope, uint32_t high, uint32_t low, const glm::vec3& origin, const glm::vec3& dir,
                  float tMax) const override
    {
        return cpu().occluded(group, scope, high, low, origin, dir, tMax);
    }
    bool closestHit(size_t group, bake::OccluderScope scope, uint32_t high, uint32_t low, bake::RayKind kind, const glm::vec3& origin,
                    const glm::vec3& dir, float tMax, bake::TraceHit& hit) const override
    {
        return cpu().closestHit(group, scope, high, low, kind, origin, dir, tMax, hit);
    }

private:
    const bake::CpuBackend& cpu() const
    {
        std::scoped_lock lock(mMutex);
        if (!mCpu)
        {
            auto c = std::make_unique<bake::CpuBackend>();
            c->prepare(*mInputs);
            mCpu = std::move(c);
        }
        return *mCpu;
    }

    const bake::BakeInputs* mInputs = nullptr;
    bake::CpuBackend mProjection;
    mutable std::mutex mMutex;
    mutable std::unique_ptr<bake::CpuBackend> mCpu;
};
} // namespace

TEST_CASE("band by band: a lazily built tracer never runs a band inside another band's wait")
{
    bake::registerBackend("lazyCpu", [] { return std::make_unique<LazyCpuBackend>(); });
    newScene();
    const fs::path dir = tempDir("lila_bake_lazy");
    // One-row bands, most of them empty: idle workers steal the tracer build's tasks, so the band building it waits and
    // could pick up other bands.
    std::vector<std::string> lows;
    for (int i = 0; i < 4; ++i)
    {
        geo::Mesh m = displaced(24, [i](double x, double z) { return 4.0 * std::sin(0.1 * x + i) * std::cos(0.13 * z); });
        for (glm::vec2& uv : m.cornerAttrs().get<glm::vec2>("uv0"))
            uv = {0.25f * float(i) + 0.05f * uv.x, 0.2f * uv.y};
        lows.push_back(addMesh("strip" + std::to_string(i), std::move(m), glm::dvec3(120.0 * i, 0.0, 0.0)));
    }
    std::vector<std::string> highs;
    for (int i = 0; i < 32; ++i)
        highs.push_back(addMesh("rock" + std::to_string(i), displaced(140, [i](double x, double z) { return std::sin(0.2 * x * (i + 1)) + std::cos(0.3 * z); }),
                                glm::dvec3(120.0 * (i % 4), -30.0 - 5.0 * (i / 4), 0.0)));
    const std::string p = project(dir, 512, 1, "exr", "{mesh}_{map}");
    ops::execute("bakeGroup", {{"args", {"strips"}}, {"project", p}, {"lows", lows}, {"highs", highs}, {"self", true}, {"frontal", 2.0}, {"rear", 2.0}});
    for (const char* map : {"thickness", "ao"})
        ops::execute("setBakeMap", {{"args", {map}}, {"project", p}, {"options", {{"rays", 2}}}});
    const std::vector<std::string> maps = {"height", "thickness", "ao"};
    json lazy;
    for (int run = 0; run < 4; ++run)
    {
        INFO("run " << run);
        lazy = ops::execute("bake", {{"project", p}, {"maps", maps}, {"backend", "lazyCpu"}});
        REQUIRE(lazy["outputs"].size() == lows.size() * maps.size());
    }
    std::vector<image::Image> lazyImages;
    for (const json& o : lazy["outputs"])
        lazyImages.push_back(readImage(o));
    const json cpu = ops::execute("bake", {{"project", p}, {"maps", maps}, {"backend", "cpu"}});
    REQUIRE(cpu["outputs"].size() == lazy["outputs"].size());
    CHECK(lazy["stats"]["samples"] == cpu["stats"]["samples"]);
    for (size_t i = 0; i < lazyImages.size(); ++i)
        CHECK(lazyImages[i].rgba == readImage(cpu["outputs"][i]).rgba);
    bake::unregisterBackend("lazyCpu");
}
