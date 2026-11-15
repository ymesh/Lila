#include <doctest/doctest.h>

#include "TestUtil.h"
#include "bake/Backend.h"
#include "bake/Bake.h"
#include "bake/BakeInputs.h"
#include "bake/Maps.h"
#include "bake/Targets.h"
#include "core/ImageIO.h"
#include "geo/ColorSets.h"
#include "geo/Mesh.h"
#include "geo/Primitives.h"
#include "ops/Command.h"
#include "ops/Undo.h"
#include "scene/Graph.h"
#include "scene/Materials.h"
#include "scene/MeshNodes.h"

#include <glm/gtc/constants.hpp>

#include <cmath>
#include <filesystem>
#include <map>
#include <set>

using namespace lila;
using namespace baketest;
using json = nlohmann::json;
namespace fs = std::filesystem;

namespace
{
template <class F> geo::Mesh transformed(geo::Mesh m, F f)
{
    for (geo::Index p = 0; p < m.pointCapacity(); ++p)
        m.setPosition(p, f(m.position(p)));
    return m;
}

void group(const std::string& p, const std::string& name, const std::string& low, const std::vector<std::string>& highs, double frontal = 3.0,
           double rear = 3.0)
{
    ops::execute("bakeGroup", {{"args", {name}}, {"project", p}, {"lows", {low}}, {"highs", highs}, {"frontal", frontal}, {"rear", rear}});
}

void setMap(const std::string& p, const std::string& map, const json& options)
{
    ops::execute("setBakeMap", {{"args", {map}}, {"project", p}, {"options", options}});
}

/** World xz of texel (x, y) on the 100 cm plane with uv 0-1. */
glm::dvec2 planeXz(int x, int y, int res)
{
    const double u = (x + 0.5) / res, v = 1.0 - (y + 0.5) / res;
    return {u * 100.0 - 50.0, 50.0 - v * 100.0};
}

double decodeCurvature(const image::Image& img, const json& out, int x, int y)
{
    return (texel(img, x, y).r - 0.5) * 2.0 * out["meta"]["curvatureRange"].get<double>();
}
} // namespace

TEST_CASE("curvature: 0.5 on flat, and the bump's mean curvature from the mesh and from the normals")
{
    newScene();
    const fs::path dir = tempDir("lila_bakemaps_curv");
    const double A = 3.0, k = glm::two_pi<double>() / 100.0;
    auto f = [&](double x, double z) { return A * std::cos(k * x) + A * std::cos(k * z); };
    const std::string low = addMesh("bump_low", plane(1));
    const std::string high = addMesh("bump_high", displaced(200, f));
    const std::string flatHigh = addMesh("flat_high", lifted(plane(8), 1.0));
    const std::string p = project(dir, 64);
    group(p, "bump", low, {high}, 10.0, 10.0);
    setMap(p, "curvature", {{"range", 0.03}});
    setMap(p, "normalToCurvature", {{"range", 0.03}});
    const json r = ops::execute("bake", {{"project", p}, {"maps", {"curvature", "normalToCurvature"}}});
    const json& meshOut = outputFor(r, "curvature");
    const json& nrmOut = outputFor(r, "normalToCurvature");
    CHECK(meshOut["meta"]["source"] == "mesh");
    CHECK(nrmOut["meta"]["source"] == "normals");
    const image::Image mesh = readImage(meshOut), nrm = readImage(nrmOut);
    const double peak = A * k * k;
    double worstMesh = 0.0, worstNrm = 0.0, sumMesh = 0.0;
    int n = 0;
    for (int y = 3; y < 61; ++y)
        for (int x = 3; x < 61; ++x)
        {
            const glm::dvec2 w = planeXz(x, y, 64);
            const double hx = -A * k * std::sin(k * w.x), hz = -A * k * std::sin(k * w.y);
            const double hxx = -A * k * k * std::cos(k * w.x), hzz = -A * k * k * std::cos(k * w.y);
            // Exact mean curvature of a height field (normal up, convex positive), and its small-slope -laplacian / 2.
            const double exact = -((1 + hz * hz) * hxx + (1 + hx * hx) * hzz) / (2.0 * std::pow(1 + hx * hx + hz * hz, 1.5));
            const double lap = -(hxx + hzz) / 2.0;
            const double em = std::abs(decodeCurvature(mesh, meshOut, x, y) - exact);
            worstMesh = std::max(worstMesh, em);
            sumMesh += em;
            worstNrm = std::max(worstNrm, std::abs(decodeCurvature(nrm, nrmOut, x, y) - lap));
            ++n;
        }
    INFO("peak " << peak << " mesh worst " << worstMesh << " normals worst " << worstNrm);
    CHECK(worstMesh < 0.1 * peak);
    CHECK(sumMesh / n < 0.03 * peak);
    CHECK(worstNrm < 0.1 * peak);
    // Convex crests are bright, troughs dark.
    CHECK(texel(mesh, 32, 32).r > 0.65f);
    CHECK(texel(mesh, 0, 0).r < 0.35f);

    // Flat: exactly neutral, even with the auto range.
    ops::execute("bakeGroup", {{"args", {"bump"}}, {"highs", {flatHigh}}});
    setMap(p, "curvature", json::object());
    const json flat = ops::execute("bake", {{"project", p}, {"maps", {"curvature", "normalToCurvature"}}});
    for (const char* map : {"curvature", "normalToCurvature"})
    {
        const image::Image img = readImage(outputFor(flat, map));
        for (int y = 0; y < 64; y += 5)
            for (int x = 0; x < 64; x += 5)
                CHECK(texel(img, x, y).r == doctest::Approx(0.5f).epsilon(1e-3));
    }
}

TEST_CASE("sphere: curvature 1/r, convexity and cavity, thickness 2r normalised")
{
    newScene();
    const fs::path dir = tempDir("lila_bakemaps_sphere");
    geo::SphereOptions so;
    so.subdivAxis = so.subdivHeight = 32;
    const std::string low = addMesh("ball_low", geo::makeSphere(so));
    geo::IcoSphereOptions io;
    io.subdivisions = 5;
    const std::string high = addMesh("ball_high", geo::makeIcoSphere(io));
    const std::string p = project(dir, 32);
    group(p, "ball", low, {high});
    setMap(p, "curvature", {{"range", 0.04}});
    setMap(p, "convexity", {{"range", 0.04}});
    setMap(p, "cavity", {{"range", 0.04}});
    setMap(p, "thickness", {{"spread", 0.0}, {"rays", 4}});
    const json r = ops::execute("bake", {{"project", p}, {"maps", {"curvature", "convexity", "cavity", "thickness"}}});
    const image::Image curv = readImage(outputFor(r, "curvature"));
    const image::Image convex = readImage(outputFor(r, "convexity"));
    const image::Image cavity = readImage(outputFor(r, "cavity"));
    const json& thOut = outputFor(r, "thickness");
    const image::Image th = readImage(thOut);
    // Straight through the centre: the diameter, so the auto range is 2r and every texel is white.
    CHECK(thOut["meta"]["thicknessRange"].get<double>() == doctest::Approx(100.0).epsilon(0.01));
    double sumK = 0.0;
    int n = 0;
    for (int y = 4; y < 28; ++y)
        for (int x = 0; x < 32; ++x)
        {
            sumK += decodeCurvature(curv, outputFor(r, "curvature"), x, y);
            ++n;
            CHECK(texel(convex, x, y).r == doctest::Approx(0.5f).epsilon(0.08));
            CHECK(texel(cavity, x, y).r == doctest::Approx(1.0f));
            CHECK(texel(th, x, y).r == doctest::Approx(1.0f).epsilon(0.01));
        }
    CHECK(sumK / n == doctest::Approx(1.0 / 50.0).epsilon(0.03));

    // A fixed distance normalises against it; the cosine spread averages chords of 2r cos(theta), 4r/3.
    setMap(p, "thickness", {{"spread", 0.0}, {"rays", 4}, {"distance", 200.0}});
    const image::Image half = readImage(outputFor(ops::execute("bake", {{"project", p}, {"maps", {"thickness"}}}), "thickness"));
    CHECK(texel(half, 10, 16).r == doctest::Approx(0.5f).epsilon(0.01));
    setMap(p, "thickness", {{"rays", 256}, {"distance", 200.0}, {"spread", 180.0}});
    const image::Image spread = readImage(outputFor(ops::execute("bake", {{"project", p}, {"maps", {"thickness"}}}), "thickness"));
    double mean = 0.0;
    n = 0;
    for (int y = 4; y < 28; ++y)
        for (int x = 0; x < 32; ++x, ++n)
            mean += texel(spread, x, y).r * 200.0;
    CHECK(mean / n == doctest::Approx(400.0 / 6.0).epsilon(0.03));
}

TEST_CASE("bent normals lean away from a wall and stay put in the open")
{
    newScene();
    const fs::path dir = tempDir("lila_bakemaps_bent");
    const std::string low = addMesh("floor_low", plane(1));
    const std::string high = addMesh("floor_high", lifted(plane(4), 0.5));
    // A huge wall at x = -60 facing +x: it hides almost exactly the -x half of every floor point's hemisphere.
    const std::string wall = addMesh("wall_high", transformed(plane(1, {0, 0}, {1, 1}, 10000.0), [](glm::dvec3 q) {
                                         return glm::dvec3(q.y - 60.0, -q.x, q.z);
                                     }));
    const std::string p = project(dir, 16);
    group(p, "floor", low, {high, wall});
    setMap(p, "bentNormal", {{"rays", 256}});
    const json r = ops::execute("bake", {{"project", p}, {"maps", {"bentNormal"}}});
    const image::Image img = readImage(outputFor(r, "bentNormal"));
    // E[d | d.x > 0] over the cosine hemisphere is (4 / (3 pi), 0, 2 / 3): 32.5 degrees toward +x (the tangent).
    for (int y = 4; y < 12; ++y)
        for (int x = 4; x < 12; ++x)
        {
            const glm::vec3 t = glm::vec3(texel(img, x, y)) * 2.0f - 1.0f;
            INFO(x << "," << y << " " << t.x << " " << t.y << " " << t.z);
            CHECK(t.x > 0.0f);
            CHECK(std::abs(t.y) < 0.08f);
            CHECK(angleDeg(t, glm::vec3(0, 0, 1)) == doctest::Approx(32.5f).epsilon(0.1));
        }

    setMap(p, "bentNormal", {{"rays", 64}, {"scope", "self"}, {"space", "object"}});
    const image::Image open = readImage(outputFor(ops::execute("bake", {{"project", p}, {"maps", {"bentNormal"}}}), "bentNormal"));
    CHECK(angleDeg(glm::vec3(texel(open, 8, 8)) * 2.0f - 1.0f, glm::vec3(0, 1, 0)) < 1.0f);
}

TEST_CASE("rounded edges bevel a hard cube within the radius only")
{
    newScene();
    const fs::path dir = tempDir("lila_bakemaps_rounded");
    const std::string low = addMesh("box_low", geo::makeCube());
    const std::string p = project(dir, 128);
    ops::execute("bakeGroup", {{"args", {"box"}}, {"project", p}, {"lows", {low}}, {"self", true}, {"frontal", 2.0}, {"rear", 2.0}});
    const double radius = 20.0;
    setMap(p, "roundedNormal", {{"radius", radius}, {"rays", 64}, {"space", "object"}});
    const json r = ops::execute("bake", {{"project", p}, {"maps", {"roundedNormal", "position", "alpha"}}});
    const json& posOut = outputFor(r, "position");
    const glm::vec3 lo(posOut["meta"]["boundsMin"][0].get<float>(), posOut["meta"]["boundsMin"][1].get<float>(), posOut["meta"]["boundsMin"][2].get<float>());
    const glm::vec3 hi(posOut["meta"]["boundsMax"][0].get<float>(), posOut["meta"]["boundsMax"][1].get<float>(), posOut["meta"]["boundsMax"][2].get<float>());
    const image::Image pos = readImage(posOut), nrm = readImage(outputFor(r, "roundedNormal")), cover = readImage(outputFor(r, "alpha"));
    int flat = 0, edge = 0;
    for (int y = 0; y < 128; ++y)
        for (int x = 0; x < 128; ++x)
        {
            if (texel(cover, x, y).r < 1.0f)
                continue;
            const glm::vec3 q = lo + glm::vec3(texel(pos, x, y)) * (hi - lo);
            const glm::vec3 aq = glm::abs(q);
            const int axis = aq.x > aq.y ? (aq.x > aq.z ? 0 : 2) : (aq.y > aq.z ? 1 : 2);
            if (aq[axis] < 49.0f)
                continue;
            glm::vec3 face(0.0f);
            face[axis] = q[axis] > 0.0f ? 1.0f : -1.0f;
            float edgeDist = 1e9f;
            for (int a = 0; a < 3; ++a)
                if (a != axis)
                    edgeDist = std::min(edgeDist, 50.0f - aq[a]);
            const float angle = angleDeg(glm::vec3(texel(nrm, x, y)) * 2.0f - 1.0f, face);
            INFO(q.x << " " << q.y << " " << q.z << " angle " << angle << " edge " << edgeDist);
            if (edgeDist > 1.5 * radius)
            {
                CHECK(angle < 2.0f);
                ++flat;
            }
            else if (edgeDist < 0.2 * radius)
            {
                CHECK(angle > 25.0f);
                CHECK(angle < 65.0f);
                ++edge;
            }
        }
    CHECK(flat > 100);
    CHECK(edge > 10);
}

TEST_CASE("vertex colour bakes a named colour set, as its own map or as base colour")
{
    newScene();
    const fs::path dir = tempDir("lila_bakemaps_vcol");
    geo::Mesh hi = lifted(plane(8), 1.0);
    for (const char* set : {"first", "paint"})
    {
        std::span<glm::vec4> c = geo::colors::add(hi, set, glm::vec4(0.0f, 1.0f, 0.0f, 1.0f));
        if (std::string(set) == "paint")
            for (geo::Index cn = 0; cn < hi.cornerCapacity(); ++cn)
                c[cn] = hi.position(hi.cornerPoint(cn)).x < 0.0 ? glm::vec4(1, 0, 0, 1) : glm::vec4(0, 0, 1, 1);
    }
    const std::string low = addMesh("v_low", plane(1));
    const std::string high = addMesh("v_high", std::move(hi));
    const std::string p = project(dir, 16, 1, "png");
    group(p, "v", low, {high});
    setMap(p, "vertexColor", {{"set", "paint"}});
    const image::Image img = readImage(outputFor(ops::execute("bake", {{"project", p}, {"maps", {"vertexColor"}}}), "vertexColor"));
    CHECK(glm::vec3(texel(img, 1, 8)) == glm::vec3(1, 0, 0));
    CHECK(glm::vec3(texel(img, 14, 8)) == glm::vec3(0, 0, 1));
    setMap(p, "vertexColor", {{"set", ""}});
    const image::Image first = readImage(outputFor(ops::execute("bake", {{"project", p}, {"maps", {"vertexColor"}}}), "vertexColor"));
    CHECK(glm::vec3(texel(first, 1, 8)) == glm::vec3(0, 1, 0));

    // Base colour reads the same colours with source vertexColor (sculpt polypaint).
    setMap(p, "baseColor", {{"source", "vertexColor"}, {"set", "paint"}});
    const image::Image base = readImage(outputFor(ops::execute("bake", {{"project", p}, {"maps", {"baseColor"}}}), "baseColor"));
    CHECK(glm::vec3(texel(base, 1, 8)) == glm::vec3(1, 0, 0));
    CHECK(glm::vec3(texel(base, 14, 8)) == glm::vec3(0, 0, 1));
}

TEST_CASE("material transfer: constants, textures, the high's normal map and ORM packing")
{
    newScene();
    const fs::path dir = tempDir("lila_bakemaps_material");
    // A 4 x 4 base colour texture, red on the left half and green on the right.
    image::Image tex{4, 4, {}};
    for (int y = 0; y < 4; ++y)
        for (int x = 0; x < 4; ++x)
            tex.rgba.insert(tex.rgba.end(), {x < 2 ? 1.0f : 0.0f, x < 2 ? 0.0f : 1.0f, 0.0f, 0.5f});
    image::write(dir / "bc.png", tex, 8);
    // A normal map tilted 30 degrees toward +u, DirectX green.
    const glm::vec3 tilt(std::sin(glm::radians(30.0f)), 0.0f, std::cos(glm::radians(30.0f)));
    image::Image nm{2, 2, {}};
    for (int i = 0; i < 4; ++i)
        nm.rgba.insert(nm.rgba.end(), {tilt.x * 0.5f + 0.5f, 0.5f, tilt.z * 0.5f + 0.5f, 1.0f});
    image::write(dir / "n.png", nm, 16);

    ops::execute("createMaterial", {{"args", {"M_Const"}}, {"baseColor", {0.2, 0.4, 0.6}}, {"roughness", 0.7}, {"metallic", 1.0},
                                    {"emissive", {0.5, 0.0, 0.0}}, {"emissiveIntensity", 2.0}});
    ops::execute("createMaterial", {{"args", {"M_Tex"}}, {"baseColorMap", (dir / "bc.png").generic_string()}, {"blendMode", "masked"},
                                    {"opacity", 0.8}, {"roughness", 0.25}, {"normalMap", (dir / "n.png").generic_string()}});
    geo::Mesh constHigh = lifted(plane(4), 1.0);
    scene::materials::assign(constHigh, "M_Const", {});
    geo::Mesh texHigh = lifted(plane(4), 1.0);
    scene::materials::assign(texHigh, "M_Tex", {});
    const std::string lowA = addMesh("a_low", plane(1));
    const std::string lowB = addMesh("b_low", plane(1));
    const std::string hiA = addMesh("a_high", std::move(constHigh));
    const std::string hiB = addMesh("b_high", std::move(texHigh));
    const std::string p = project(dir, 16, 1, "png", "{group}_{map}");
    group(p, "a", lowA, {hiA});
    group(p, "b", lowB, {hiB});
    const json plain = ops::execute("bake", {{"project", p}, {"groups", {"b"}}, {"maps", {"normal"}}});
    CHECK(angleDeg(glm::vec3(texel(readImage(outputFor(plain, "normal")), 8, 8)) * 2.0f - 1.0f, glm::vec3(0, 0, 1)) < 0.5f);
    setMap(p, "normal", {{"highNormalMap", true}});
    const json r = ops::execute("bake", {{"project", p}, {"maps", {"baseColor", "roughness", "metallic", "emissive", "materialOpacity", "orm", "normal"}}});

    auto file = [&](const std::string& map, const std::string& grp) {
        for (const json& o : r["outputs"])
            if (o["map"] == map && o["groups"][0] == grp)
                return readImage(o);
        FAIL("no " << map << " for " << grp);
        return image::Image{};
    };
    const float tol = 1.0f / 255.0f;
    const glm::vec3 bc = glm::vec3(texel(file("baseColor", "a"), 8, 8));
    CHECK(bc.r == doctest::Approx(0.4845f).epsilon(0.01));
    CHECK(bc.g == doctest::Approx(0.6650f).epsilon(0.01));
    CHECK(bc.b == doctest::Approx(0.7975f).epsilon(0.01));
    CHECK(std::abs(texel(file("roughness", "a"), 8, 8).r - 0.7f) <= tol);
    CHECK(std::abs(texel(file("metallic", "a"), 8, 8).r - 1.0f) <= tol);
    CHECK(texel(file("emissive", "a"), 8, 8).r == doctest::Approx(1.0f).epsilon(0.01));
    CHECK(std::abs(texel(file("materialOpacity", "a"), 8, 8).r - 1.0f) <= tol);
    const image::Image bcB = file("baseColor", "b");
    CHECK(glm::vec3(texel(bcB, 3, 8)) == glm::vec3(1, 0, 0));
    CHECK(glm::vec3(texel(bcB, 12, 8)) == glm::vec3(0, 1, 0));
    CHECK(std::abs(texel(file("materialOpacity", "b"), 3, 8).r - 0.4f) <= tol);
    CHECK(std::abs(texel(file("roughness", "b"), 8, 8).r - 0.25f) <= tol);

    // ORM: open plane AO, the material's roughness and metallic; its sources aren't written twice.
    const image::Image orm = file("orm", "a");
    CHECK(std::abs(texel(orm, 8, 8).r - 1.0f) <= tol);
    CHECK(std::abs(texel(orm, 8, 8).g - 0.7f) <= tol);
    CHECK(std::abs(texel(orm, 8, 8).b - 1.0f) <= tol);
    CHECK(fs::path(outputFor(r, "orm")["file"].get<std::string>()).filename().string().find("ORM") != std::string::npos);

    // The high's own normal map: same frame as the low, so the bake reproduces it.
    const glm::vec3 nb = glm::vec3(texel(file("normal", "b"), 8, 8)) * 2.0f - 1.0f;
    CHECK(angleDeg(nb, tilt) < 1.0f);
    const glm::vec3 na = glm::vec3(texel(file("normal", "a"), 8, 8)) * 2.0f - 1.0f;
    CHECK(angleDeg(na, glm::vec3(0, 0, 1)) < 0.5f);

    // A custom pack: only it is written; its sources stay hidden.
    setMap(p, "pack", {{"channels", json::array({json{{"map", "roughness"}}, 0.25, json{{"map", "metallic"}, {"invert", true}}, json{{"map", "baseColor"}, {"channel", 1}}})}});
    const json packed = ops::execute("bake", {{"project", p}, {"groups", {"a"}}, {"maps", {"pack"}}});
    REQUIRE(packed["outputs"].size() == 1);
    const glm::vec4 pk = texel(readImage(packed["outputs"][0]), 8, 8);
    CHECK(std::abs(pk.r - 0.7f) <= tol);
    CHECK(std::abs(pk.g - 0.25f) <= tol);
    CHECK(std::abs(pk.b - 0.0f) <= tol);
    CHECK(pk.a == doctest::Approx(0.665f).epsilon(0.01));
    CHECK(packed["outputs"][0]["meta"]["channels"].size() == 4);

    setMap(p, "pack", {{"channels", json::array({json{{"map", "baseColor"}, {"channel", "luminance"}}, json{{"map", "baseColor"}, {"channel", "max"}}, 0.0, 1.0})}});
    const glm::vec4 lum = texel(readImage(ops::execute("bake", {{"project", p}, {"groups", {"a"}}, {"maps", {"pack"}}})["outputs"][0]), 8, 8);
    CHECK(lum.r == doctest::Approx(0.2126f * bc.r + 0.7152f * bc.g + 0.0722f * bc.b).epsilon(0.01));
    CHECK(lum.g == doctest::Approx(bc.b).epsilon(0.01));

    // Base colour with the emissive mask in alpha.
    const json bce = ops::execute("bake", {{"project", p}, {"groups", {"a"}}, {"maps", {"baseColorEmissive"}}});
    REQUIRE(bce["outputs"].size() == 1);
    CHECK(bce["outputs"][0]["colorSpace"] == "srgb");
    const glm::vec4 be = texel(readImage(bce["outputs"][0]), 8, 8);
    CHECK(glm::vec3(be).r == doctest::Approx(bc.r).epsilon(0.01));
    CHECK(glm::vec3(be).b == doctest::Approx(bc.b).epsilon(0.01));
    CHECK(be.a == doctest::Approx(1.0f).epsilon(0.01));
}

TEST_CASE("UV wireframe skips diagonals; island borders mark seams")
{
    newScene();
    const fs::path dir = tempDir("lila_bakemaps_wire");
    geo::Mesh a = plane(4, {0.0f, 0.0f}, {0.5f, 1.0f});
    geo::Mesh b = plane(1, {0.5f, 0.0f}, {1.0f, 1.0f});
    a.append(transformed(std::move(b), [](glm::dvec3 q) { return q + glm::dvec3(200.0, 0.0, 0.0); }));
    const std::string low = addMesh("w_low", std::move(a));
    const std::string p = project(dir, 64, 4, "png");
    ops::execute("bakeGroup", {{"args", {"w"}}, {"project", p}, {"lows", {low}}, {"self", true}});
    setMap(p, "islandBorder", {{"width", 2.0}});
    const json r = ops::execute("bake", {{"project", p}, {"maps", {"wireframe", "islandBorder"}}});
    const image::Image wire = readImage(outputFor(r, "wireframe"));
    const image::Image border = readImage(outputFor(r, "islandBorder"));
    // Island a: 4 x 4 quads of 8 x 16 texels. Grid lines at x = 0, 8, 16, ...; quad centres (and their diagonals) clear.
    CHECK(texel(wire, 8, 20).r > 0.4f);
    CHECK(texel(wire, 4, 24).r == 0.0f);
    CHECK(texel(wire, 12, 40).r == 0.0f);
    CHECK(texel(wire, 20, 16).r > 0.4f);
    // Island b is one quad: only its outline.
    CHECK(texel(wire, 48, 32).r == 0.0f);
    // Borders: island a's inner grid lines are not seams.
    CHECK(texel(border, 8, 20).r == 0.0f);
    CHECK(texel(border, 0, 20).r == 1.0f);
    CHECK(texel(border, 31, 20).r == 1.0f);
    CHECK(texel(border, 16, 32).r == 0.0f);
    CHECK(texel(border, 48, 32).r == 0.0f);
}

TEST_CASE("error map: misses, back faces, surfaces beyond the rays and degenerate frames")
{
    newScene();
    const fs::path dir = tempDir("lila_bakemaps_error");
    // Left quarter: a good high. Next: a back-facing one. Next: one too deep for the rear distance. Right: nothing.
    auto strip = [](double x0, double x1, double y, bool flip) {
        geo::Mesh m = plane(2);
        return transformed(std::move(m), [&](glm::dvec3 q) {
            glm::dvec3 o((q.x + 50.0) / 100.0 * (x1 - x0) + x0, y, q.z);
            return flip ? glm::dvec3(o.x, y, -q.z) : o;
        });
    };
    geo::Mesh good = strip(-50, -25, 1.0, false);
    geo::Mesh back = strip(-25, 0, 1.0, true);
    geo::Mesh deep = strip(0, 25, -20.0, false);
    const std::string low = addMesh("e_low", plane(1));
    const std::string p = project(dir, 32, 1, "png");
    group(p, "e", low, {addMesh("good_high", std::move(good)), addMesh("back_high", std::move(back)), addMesh("deep_high", std::move(deep))});
    const json r = ops::execute("bake", {{"project", p}, {"maps", {"error"}}});
    const json& out = outputFor(r, "error");
    const image::Image img = readImage(out);
    CHECK(glm::vec3(texel(img, 4, 16)) == glm::vec3(0, 0, 0));
    CHECK(glm::vec3(texel(img, 12, 16)) == glm::vec3(1, 0, 1));
    CHECK(glm::vec3(texel(img, 20, 16)) == glm::vec3(1, 1, 0));
    CHECK(glm::vec3(texel(img, 28, 16)) == glm::vec3(1, 0, 0));
    const json& counts = out["meta"]["texels"];
    CHECK(counts["ok"].get<int>() > 200);
    CHECK(counts["backface"].get<int>() > 200);
    CHECK(counts["beyondDistance"].get<int>() > 200);
    CHECK(counts["miss"].get<int>() > 200);

    // A sample with no tangent frame is flagged before anything else.
    bake::registerBuiltinMaps();
    const bake::MapDef* def = bake::findMap("error");
    REQUIRE(def);
    bake::BakeInputs in;
    bake::GroupInputs g;
    bake::LowInput l;
    l.soup.positions = {{0, 0, 0}, {1, 0, 0}, {0, 0, -1}};
    l.soup.uvs = {{0, 0}, {1, 0}, {0, 1}};
    l.soup.materials = {0};
    g.lows.push_back(l);
    const bake::MapSettings ms{.map = "error"};
    const bake::MapContext ctx{ms, in, g, 0, nullptr, 32, 32, nullptr};
    bake::TexelSample s{};
    s.low = 0;
    s.normal[1] = 1.0f;
    bake::HitRecord h{};
    h.flags = bake::kHitValid;
    glm::vec4 o;
    uint8_t v = 0;
    def->cpu(ctx, std::span(&s, 1), std::span(&h, 1), std::span(&o, 1), std::span(&v, 1));
    CHECK(glm::vec3(o) == glm::vec3(0, 1, 1));
    s.tangent[0] = 1.0f;
    def->cpu(ctx, std::span(&s, 1), std::span(&h, 1), std::span(&o, 1), std::span(&v, 1));
    CHECK(glm::vec3(o) == glm::vec3(0, 0, 0));
}

TEST_CASE("derived maps: gradient, normal to height and edge wear")
{
    newScene();
    const fs::path dir = tempDir("lila_bakemaps_derived");
    const double A = 3.0, k = glm::two_pi<double>() / 100.0;
    auto f = [&](double x, double z) { return A * std::cos(k * x) + A * std::cos(k * z); };
    const std::string low = addMesh("bump_low", plane(1));
    const std::string high = addMesh("bump_high", displaced(200, f));
    const std::string p = project(dir, 64);
    group(p, "bump", low, {high}, 10.0, 10.0);
    setMap(p, "gradient", {{"direction", {1.0, 0.0, 0.0}}});
    setMap(p, "edgeWear", {{"range", 0.012}, {"threshold", 0.5}, {"softness", 0.2}});
    const json r = ops::execute("bake", {{"project", p}, {"maps", {"gradient", "normalToHeight", "edgeWear"}}});

    const image::Image grad = readImage(outputFor(r, "gradient"));
    for (int x = 2; x < 64; x += 6)
        CHECK(texel(grad, x, 20).r == doctest::Approx((x + 0.5) / 64.0).epsilon(0.02));

    const json& hOut = outputFor(r, "normalToHeight");
    const image::Image h = readImage(hOut);
    const double range = hOut["meta"]["heightRange"].get<double>();
    double worst = 0.0;
    for (int y = 0; y < 64; ++y)
        for (int x = 0; x < 64; ++x)
        {
            const glm::dvec2 w = planeXz(x, y, 64);
            worst = std::max(worst, std::abs((texel(h, x, y).r - 0.5) * 2.0 * range - f(w.x, w.y)));
        }
    INFO("worst " << worst);
    CHECK(worst < 0.05 * 4.0 * A);

    // Wear on the crests (curvature A k^2 at the centre), none in the troughs.
    const image::Image wear = readImage(outputFor(r, "edgeWear"));
    CHECK(texel(wear, 32, 32).r > 0.9f);
    CHECK(texel(wear, 0, 32).r < 0.05f);
    CHECK(texel(wear, 16, 16).r < 0.05f);
}

TEST_CASE("Painter and Unreal targets bake every map they name")
{
    for (const char* name : {"substancePainter", "unreal"})
    {
        const auto t = bake::findTarget(name);
        REQUIRE(t);
        std::vector<std::string> skipped;
        bake::applyTarget(bake::ProjectSettings::defaults(), *t, true, &skipped);
        INFO(name);
        CHECK(skipped.empty());
    }
}

TEST_CASE("links: a self high projects only onto its own low; unlinked meshes stop occluding")
{
    newScene();
    const fs::path dir = tempDir("lila_bakemaps_links");
    // A floor and a smaller lid 0.5 cm above it, both baking onto themselves with rays starting 2 cm out.
    const std::string floor = addMesh("floor", plane(1));
    const std::string lid = addMesh("lid", lifted(plane(1, {0.0f, 0.0f}, {1.0f, 1.0f}, 50.0), 0.5));
    const std::string p = project(dir, 16, 1, "exr");
    ops::execute("bakeGroup", {{"args", {"pair"}}, {"project", p}, {"lows", {floor, lid}}, {"self", true}, {"frontal", 2.0}, {"rear", 2.0}});
    setMap(p, "height", {{"range", 1.0}});
    json r = ops::execute("bake", {{"project", p}, {"maps", {"height"}}, {"backend", "cpu"}});
    // The floor's centre sits under the lid; it still finds itself, not the lid.
    for (const json& o : r["outputs"])
        if (o["meshes"][0] == floor)
            CHECK(texel(readImage(o), 8, 8).r == doctest::Approx(0.5f).epsilon(0.01));

    // AO on the floor under a separate 5 cm high lid, then with the lid unlinked from the floor's occlusion.
    newScene();
    const std::string f2 = addMesh("floor", plane(1));
    const std::string roof = addMesh("roof", lifted(plane(1, {0.0f, 0.0f}, {1.0f, 1.0f}, 80.0), 5.0));
    const std::string p2 = project(dir / "ao", 16, 1, "exr");
    ops::execute("bakeGroup", {{"args", {"g"}}, {"project", p2}, {"lows", {f2}}, {"self", true}, {"addHigh", {roof}}, {"frontal", 2.0}, {"rear", 2.0}});
    auto ao = [&] {
        const json out = ops::execute("bake", {{"project", p2}, {"maps", {"ao"}}, {"backend", "cpu"}});
        return texel(readImage(outputFor(out, "ao")), 8, 8).r;
    };
    const float covered = ao();
    CHECK(covered < 0.5f);
    ops::execute("bakeGroup", {{"args", {"g"}}, {"unlinks", json::array({json{{"lows", {f2}}, {"meshes", {roof}}}})}});
    CHECK(ops::execute("bakeGroup", {{"args", {"g"}}})["unlinks"].size() == 1);
    CHECK(ao() == doctest::Approx(1.0f).epsilon(0.01));

    // The same by the lid's material, which follows a rename.
    ops::execute("bakeGroup", {{"args", {"g"}}, {"unlinks", json::array()}});
    ops::execute("createMaterial", {{"args", {"M_Lid"}}});
    ops::execute("assignMaterial", {{"args", {"M_Lid", roof}}});
    CHECK(ao() < 0.5f);
    ops::execute("bakeGroup", {{"args", {"g"}}, {"unlinks", json::array({json{{"materials", {"M_Lid"}}}})}});
    CHECK(ao() == doctest::Approx(1.0f).epsilon(0.01));
    ops::execute("renameMaterial", {{"args", {"M_Lid", "M_Roof"}}});
    CHECK(ops::execute("bakeGroup", {{"args", {"g"}}})["unlinks"][0]["materials"] == json::array({"M_Roof"}));
    CHECK(ao() == doctest::Approx(1.0f).epsilon(0.01));
}

TEST_CASE("AO: a point buried inside another high isn't black")
{
    newScene();
    const fs::path dir = tempDir("lila_bakemaps_buried");
    // The floor's middle runs through a closed 30 cm box, like a strand entering a scalp.
    const std::string floor = addMesh("floor", plane(1));
    const std::string box = addMesh("box", geo::makeCube({.width = 30.0, .height = 30.0, .depth = 30.0}));
    const std::string p = project(dir, 16, 1, "exr");
    ops::execute("bakeGroup", {{"args", {"g"}}, {"project", p}, {"lows", {floor}}, {"self", true}, {"addHigh", {box}}, {"frontal", 2.0}, {"rear", 2.0}});
    setMap(p, "ao", {{"rays", 64}, {"distance", 100.0}});
    const json out = ops::execute("bake", {{"project", p}, {"maps", {"ao"}}, {"backend", "cpu"}});
    const image::Image img = readImage(outputFor(out, "ao"));
    CHECK(texel(img, 8, 8).r > 0.9f);
    // Beside the box the floor still darkens toward it.
    CHECK(texel(img, 5, 8).r < 0.9f);
}

TEST_CASE("projection: a ray tilted off a hard edge that meets a surface facing away is re-aimed along the face normal")
{
    newScene();
    const fs::path dir = tempDir("lila_bakemaps_reaim");
    // Rays near the cube's top rim follow the smooth cage normal, 45 degrees off the face, into a wall that faces away
    // from the top: the far side of a thin part as such a ray sees it.
    const std::string low = addMesh("cube", geo::makeCube({.width = 20.0, .height = 20.0, .depth = 20.0, .subdivX = 8, .subdivY = 8, .subdivZ = 8}));
    geo::Mesh wall = plane(1, {0.0f, 0.0f}, {1.0f, 1.0f}, 8.0);
    const double a = glm::radians(-108.0);
    for (geo::Index q = 0; q < wall.pointCapacity(); ++q)
    {
        const glm::dvec3 v = wall.position(q);
        wall.setPosition(q, glm::dvec3(v.x * std::cos(a) - v.y * std::sin(a) + 8.0, v.x * std::sin(a) + v.y * std::cos(a) + 8.0, v.z * 2.5));
    }
    const std::string high = addMesh("wall", std::move(wall));
    const std::string p = project(dir, 64);
    ops::execute("bakeGroup", {{"args", {"g"}}, {"project", p}, {"lows", {low}}, {"highs", {high}}, {"frontal", 0.5}, {"rear", 10.0}});
    const json r = ops::execute("bake", {{"project", p}, {"maps", {"normal", "alpha"}}, {"backend", "cpu"}});
    CHECK(r["stats"]["reaimed"].get<uint64_t>() > 0);
    const image::Image nrm = readImage(outputFor(r, "normal")), cover = readImage(outputFor(r, "alpha"));
    int inward = 0;
    for (int y = 0; y < 64; ++y)
        for (int x = 0; x < 64; ++x)
            if (texel(cover, x, y).r >= 1.0f && texel(nrm, x, y).b * 2.0f - 1.0f < -0.02f)
                ++inward;
    CHECK(inward == 0);
}

TEST_CASE("projection: where a thin slab's smoothed high pulls back past the rim, samples take the nearest high")
{
    newScene();
    const fs::path dir = tempDir("lila_bakemaps_probe");
    // Smoothing a 1 cm slab onto itself rounds its rim far inside the low's side faces: rays along them pass it by.
    const std::string low = addMesh("slab", geo::makeCube({.width = 40.0, .height = 1.0, .depth = 40.0, .subdivX = 10, .subdivZ = 10}));
    const std::string p = project(dir, 64);
    ops::execute("bakeGroup", {{"args", {"g"}}, {"project", p}, {"lows", {low}}, {"self", true}, {"preset", "Smooth x4"}, {"frontal", 0.5}, {"rear", 20.0}});
    const json r = ops::execute("bake", {{"project", p}, {"maps", {"normal", "alpha"}}, {"backend", "cpu"}});
    const json& st = r["stats"];
    CHECK(st["probed"].get<uint64_t>() > 0);
    CHECK(st["misses"].get<uint64_t>() * 100 < st["samples"].get<uint64_t>());
    const image::Image nrm = readImage(outputFor(r, "normal")), cover = readImage(outputFor(r, "alpha"));
    int inward = 0;
    for (int y = 0; y < 64; ++y)
        for (int x = 0; x < 64; ++x)
            if (texel(cover, x, y).r >= 1.0f && texel(nrm, x, y).b * 2.0f - 1.0f < -0.02f)
                ++inward;
    CHECK(inward == 0);
}

TEST_CASE("AO: a texel whose rays miss the high takes its neighbours' occlusion, not open sky")
{
    newScene();
    const fs::path dir = tempDir("lila_bakemaps_aomiss");
    // The high covers only the floor's middle, under a 5 cm roof; the floor's rim finds no high.
    const std::string floor = addMesh("floor", plane(1));
    const std::string middle = addMesh("middle", plane(1, {0.0f, 0.0f}, {1.0f, 1.0f}, 60.0));
    const std::string roof = addMesh("roof", lifted(plane(1, {0.0f, 0.0f}, {1.0f, 1.0f}, 100.0), 5.0));
    const std::string p = project(dir, 16, 1, "exr");
    ops::execute("bakeGroup", {{"args", {"g"}}, {"project", p}, {"lows", {floor}}, {"highs", {middle, roof}}, {"frontal", 2.0}, {"rear", 2.0}});
    setMap(p, "ao", {{"rays", 64}, {"distance", 100.0}});
    const json out = ops::execute("bake", {{"project", p}, {"maps", {"ao"}}, {"backend", "cpu"}});
    const image::Image img = readImage(outputFor(out, "ao"));
    CHECK(texel(img, 8, 8).r < 0.5f);
    CHECK(texel(img, 1, 8).r < 0.5f);
}

TEST_CASE("a self high ignores its low's normal map, so rebaking doesn't compound the last bake")
{
    newScene();
    const fs::path dir = tempDir("lila_bakemaps_selfnormal");
    const glm::vec3 tilt(std::sin(glm::radians(30.0f)), 0.0f, std::cos(glm::radians(30.0f)));
    image::Image nm{2, 2, {}};
    for (int i = 0; i < 4; ++i)
        nm.rgba.insert(nm.rgba.end(), {tilt.x * 0.5f + 0.5f, 0.5f, tilt.z * 0.5f + 0.5f, 1.0f});
    image::write(dir / "n.png", nm, 16);
    ops::execute("createMaterial", {{"args", {"M_Baked"}}, {"normalMap", (dir / "n.png").generic_string()}});
    geo::Mesh flat = plane(4);
    scene::materials::assign(flat, "M_Baked", {});
    const std::string low = addMesh("low", std::move(flat));
    const std::string p = project(dir, 16, 1, "png", "{group}_{map}");
    ops::execute("bakeGroup", {{"args", {"g"}}, {"project", p}, {"lows", {low}}, {"self", true}, {"frontal", 1.0}, {"rear", 1.0}});
    setMap(p, "normal", {{"highNormalMap", true}});
    const json r = ops::execute("bake", {{"project", p}, {"maps", {"normal"}}, {"backend", "cpu"}});
    const glm::vec3 n = glm::vec3(texel(readImage(outputFor(r, "normal")), 8, 8)) * 2.0f - 1.0f;
    CHECK(angleDeg(n, glm::vec3(0, 0, 1)) < 0.5f);
}

TEST_CASE("material slots of one low with overlapping UVs each fill their own texture")
{
    newScene();
    const fs::path dir = tempDir("lila_bakemaps_matoverlap");
    // Four quads, two per material, every quad's UVs covering the whole tile: each texture set overlaps the other.
    geo::Mesh m = plane(2);
    auto uvs = m.cornerAttrs().get<glm::vec2>("uv0");
    const std::vector<glm::vec2> before(uvs.begin(), uvs.end());
    for (geo::Index c = 0; c < uvs.size(); ++c)
    {
        glm::vec2 centre(0.0f);
        const std::vector<geo::Index> corners = m.faceCorners(m.cornerFace(c));
        for (geo::Index k : corners)
            centre += before[k];
        centre /= float(corners.size());
        uvs[c] = before[c] * 2.0f - glm::floor(centre * 2.0f);
    }
    scene::materials::assign(m, "M_A", {0, 1});
    scene::materials::assign(m, "M_B", {2, 3});
    const std::string low = addMesh("both_low", std::move(m));
    const std::string p = project(dir, 16, 1, "png", "{material}_{map}");
    // No padding, so a texel the other set took stays background black instead of being filled from its neighbours;
    // the ID map's material colours are never black.
    ops::execute("setBakeOutput", {{"project", p}, {"padding", 0}});
    ops::execute("bakeGroup", {{"args", {"g"}}, {"project", p}, {"lows", {low}}, {"self", true}, {"frontal", 1.0}, {"rear", 1.0}});
    const json r = ops::execute("bake", {{"project", p}, {"maps", {"id"}}, {"backend", "cpu"}});
    REQUIRE(r["outputs"].size() == 2);
    for (const json& o : r["outputs"])
    {
        const image::Image img = readImage(o);
        size_t blank = 0;
        for (size_t i = 0; i + 3 < img.rgba.size(); i += 4)
            if (img.rgba[i] + img.rgba[i + 1] + img.rgba[i + 2] < 0.01f)
                ++blank;
        CHECK(blank == 0);
    }
}

TEST_CASE("per-material output: each texture set bakes at its own resolution in one bake")
{
    newScene();
    const fs::path dir = tempDir("lila_bakemaps_permat");
    geo::Mesh a = plane(1);
    scene::materials::assign(a, "M_A", {});
    geo::Mesh b = plane(1);
    scene::materials::assign(b, "M_B", {});
    const std::string lowA = addMesh("a_low", std::move(a));
    const std::string lowB = addMesh("b_low", lifted(std::move(b), 50.0));
    const std::string p = project(dir, 16, 1, "png", "{material}_{map}");
    ops::execute("bakeGroup", {{"args", {"g"}}, {"project", p}, {"lows", {lowA, lowB}}, {"self", true}, {"frontal", 1.0}, {"rear", 1.0}});
    const json out = ops::execute("setBakeOutput", {{"project", p}, {"material", "M_B"}, {"resolution", 32}, {"supersampling", 4}});
    CHECK(out["materials"]["M_B"]["width"] == 32);
    const json r = ops::execute("bake", {{"project", p}, {"maps", {"normal", "ao"}}, {"backend", "cpu"}});
    std::map<std::string, int> width;
    for (const json& o : r["outputs"])
        width[fs::path(o["file"].get<std::string>()).stem().string()] = o["width"].get<int>();
    CHECK(width == std::map<std::string, int>{{"M_A_normal", 16}, {"M_A_ao", 16}, {"M_B_normal", 32}, {"M_B_ao", 32}});
    CHECK(readImage(outputFor(r, "normal")).width == width[fs::path(outputFor(r, "normal")["file"].get<std::string>()).stem().string()]);
    CHECK(fs::exists(fs::path(r["manifest"].get<std::string>())));
    CHECK(r["stats"]["groups"].size() == 1);

    // Clearing the override bakes both at the project's resolution again.
    ops::execute("setBakeOutput", {{"project", p}, {"material", "M_B"}, {"clear", true}});
    const json r2 = ops::execute("bake", {{"project", p}, {"maps", {"normal"}}, {"backend", "cpu"}});
    for (const json& o : r2["outputs"])
        CHECK(o["width"] == 16);
}
