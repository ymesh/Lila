#include "TexTest.h"

#include "tex/Evaluate.h"
#include "tex/Layers.h"

#include <glm/gtc/matrix_transform.hpp>

#include <cmath>
#include <numbers>

using namespace textest;

namespace
{
constexpr int kRes = 32;

/**
 * A mirrored unwrap: texels at u and 1 - u hold the same surface point, as the two sides of a uv seam do. The surface is
 * flat (z = 0) for |x| < 10 and bends down past it, so follow has something to carry over.
 */
glm::dvec3 surfacePoint(double u, double v)
{
    const double x = (u < 0.5 ? u : 1.0 - u) * 100.0 - 25.0;
    const double y = v * 100.0 - 50.0;
    return {x, y, std::abs(x) > 10.0 ? -(std::abs(x) - 10.0) : 0.0};
}

glm::dvec3 surfaceNormal(double u)
{
    const double x = (u < 0.5 ? u : 1.0 - u) * 100.0 - 25.0;
    if (std::abs(x) <= 10.0)
        return {0.0, 0.0, 1.0};
    return glm::normalize(glm::dvec3(x > 0 ? 1.0 : -1.0, 0.0, 1.0));
}

/** The normal as the projection reads it back from the map's floats, so ties between axes fall the same way. */
glm::dvec3 storedNormal(double u)
{
    const glm::vec3 stored = glm::vec3(surfaceNormal(u)) * 0.5f + 0.5f;
    return glm::dvec3(stored * 2.0f - 1.0f);
}

/** The box the position map is normalised to, as a bake's bounds are. */
const glm::dvec3 kBoundsLo(-30.0, -60.0, -20.0), kBoundsHi(30.0, 60.0, 10.0);

/** The mesh maps as a bake stores them: positions 0-1 across the bounds, normals n * 0.5 + 0.5. */
std::map<std::string, image::Image> meshMaps()
{
    image::Image pos{kRes, kRes, std::vector<float>(size_t(kRes) * kRes * 4)}, nrm = pos;
    for (int y = 0; y < kRes; ++y)
        for (int x = 0; x < kRes; ++x)
        {
            const double u = (x + 0.5) / kRes, v = 1.0 - (y + 0.5) / kRes;
            const glm::dvec3 p = (surfacePoint(u, v) - kBoundsLo) / (kBoundsHi - kBoundsLo), n = surfaceNormal(u) * 0.5 + 0.5;
            float* a = &pos.rgba[(size_t(y) * kRes + x) * 4];
            float* b = &nrm.rgba[(size_t(y) * kRes + x) * 4];
            a[0] = float(p.x), a[1] = float(p.y), a[2] = float(p.z), a[3] = 1.0f;
            b[0] = float(n.x), b[1] = float(n.y), b[2] = float(n.z), b[3] = 1.0f;
        }
    return {{"position", pos}, {"worldNormal", nrm}};
}

/** The coordinate layerProjection reads at, written out again on the CPU. */
glm::dvec2 reference(const json& p, double u, double v)
{
    const glm::dvec3 deg(p.value("rotationX", 0.0), p.value("rotationY", 0.0), p.value("rotationZ", 0.0));
    const glm::dvec3 r = glm::radians(deg);
    const glm::dmat3 rot = glm::dmat3(glm::rotate(glm::dmat4(1.0), r.z, {0, 0, 1}) * glm::rotate(glm::dmat4(1.0), r.y, {0, 1, 0}) *
                                      glm::rotate(glm::dmat4(1.0), r.x, {1, 0, 0}));
    const glm::dvec3 c(p.value("positionX", 0.0), p.value("positionY", 0.0), p.value("positionZ", 0.0));
    const glm::dvec3 size(p.value("sizeX", 100.0), p.value("sizeY", 100.0), p.value("sizeZ", 100.0));
    const glm::dvec3 f = glm::transpose(rot) * (surfacePoint(u, v) - c), fn = glm::transpose(rot) * storedNormal(u);
    const glm::dvec3 l = f / size;
    const std::string mode = p.value("mode", "uv");
    if (mode == "spherical")
        return {std::atan2(l.y, l.x) / (2 * std::numbers::pi) + 0.5, std::acos(std::clamp(l.z / glm::length(l), -1.0, 1.0)) / std::numbers::pi};
    if (mode == "cylindrical")
        return {std::atan2(l.y, l.x) / (2 * std::numbers::pi) + 0.5, l.z + 0.5};
    if (mode == "triplanar")
    {
        const glm::dvec3 a = glm::abs(fn);
        if (a.x >= a.y && a.x >= a.z)
            return glm::dvec2(l.y, l.z) + 0.5;
        if (a.y >= a.z)
            return glm::dvec2(l.x, l.z) + 0.5;
        return glm::dvec2(l.x, l.y) + 0.5;
    }
    glm::dvec2 co = glm::dvec2(l.x, l.y) + 0.5;
    const glm::dvec2 side = glm::dvec2(fn.x, fn.y) / std::max(std::abs(fn.x) + std::abs(fn.y), 1e-6);
    co += side * std::max(-f.z, 0.0) * (1.0 - std::abs(fn.z)) * p.value("follow", 0.0) / glm::dvec2(size.x, size.y);
    if (mode == "warp")
    {
        const glm::dvec2 w00(p.value("warp00X", 0.0), p.value("warp00Y", 0.0)), w10(p.value("warp10X", 0.0), p.value("warp10Y", 0.0));
        const glm::dvec2 w01(p.value("warp01X", 0.0), p.value("warp01Y", 0.0)), w11(p.value("warp11X", 0.0), p.value("warp11Y", 0.0));
        co += glm::mix(glm::mix(w00, w10, co.x), glm::mix(w01, w11, co.x), co.y);
    }
    return co;
}

/**
 * The same surface baked after the mesh moved by world: the maps hold world positions and normals, normalised to the
 * moved surface's bounds, and the node gets that bake's world-to-object matrix as the layer stack gives it.
 */
image::Image movedCoords(const json& projection, const glm::dmat4& world)
{
    glm::dvec3 lo(1e30), hi(-1e30);
    for (int y = 0; y < kRes; ++y)
        for (int x = 0; x < kRes; ++x)
        {
            const glm::dvec3 p = glm::dvec3(world * glm::dvec4(surfacePoint((x + 0.5) / kRes, 1.0 - (y + 0.5) / kRes), 1.0));
            lo = glm::min(lo, p);
            hi = glm::max(hi, p);
        }
    lo -= 1.0;
    hi += 1.0;
    const glm::dmat3 normalMatrix = glm::transpose(glm::inverse(glm::dmat3(world)));
    image::Image pos{kRes, kRes, std::vector<float>(size_t(kRes) * kRes * 4)}, nrm = pos;
    for (int y = 0; y < kRes; ++y)
        for (int x = 0; x < kRes; ++x)
        {
            const double u = (x + 0.5) / kRes, v = 1.0 - (y + 0.5) / kRes;
            const glm::dvec3 p = (glm::dvec3(world * glm::dvec4(surfacePoint(u, v), 1.0)) - lo) / (hi - lo);
            const glm::dvec3 n = glm::normalize(normalMatrix * surfaceNormal(u)) * 0.5 + 0.5;
            float* a = &pos.rgba[(size_t(y) * kRes + x) * 4];
            float* b = &nrm.rgba[(size_t(y) * kRes + x) * 4];
            a[0] = float(p.x), a[1] = float(p.y), a[2] = float(p.z), a[3] = 1.0f;
            b[0] = float(n.x), b[1] = float(n.y), b[2] = float(n.z), b[3] = 1.0f;
        }
    Graph g = Graph::makeMaterial("S");
    json values = projection;
    values["boundsMin"] = {lo.x, lo.y, lo.z};
    values["boundsMax"] = {hi.x, hi.y, hi.z};
    const glm::dmat4 inv = glm::inverse(world);
    const char* keys[] = {"objectX", "objectY", "objectZ", "objectW"};
    for (int c = 0; c < 4; ++c)
        values[keys[c]] = {inv[c].x, inv[c].y, inv[c].z};
    add(g, "P", "layerProjection", values);
    tex::EvalOptions o;
    o.externals = {{"position", pos}, {"worldNormal", nrm}};
    Plan p = plan(g, "P", "coord", kRes, PortType::Vector);
    REQUIRE_MESSAGE(p.error.empty(), p.error);
    return tex::evaluate(p, o, "").outputs.at("out");
}

image::Image coords(const json& projection)
{
    Graph g = Graph::makeMaterial("S");
    json values = projection;
    values["boundsMin"] = {kBoundsLo.x, kBoundsLo.y, kBoundsLo.z};
    values["boundsMax"] = {kBoundsHi.x, kBoundsHi.y, kBoundsHi.z};
    add(g, "P", "layerProjection", values);
    tex::EvalOptions o;
    o.externals = meshMaps();
    Plan p = plan(g, "P", "coord", kRes, PortType::Vector);
    REQUIRE_MESSAGE(p.error.empty(), p.error);
    return tex::evaluate(p, o, "").outputs.at("out");
}
} // namespace

TEST_CASE("tex projection: each mode's coordinate matches the CPU reference")
{
    const json cases[] = {
        {{"mode", "planar"}, {"sizeX", 50.0}, {"sizeY", 50.0}},
        {{"mode", "planar"}, {"rotationZ", 30.0}, {"positionX", 5.0}, {"sizeX", 40.0}},
        {{"mode", "planar"}, {"follow", 1.0}, {"sizeX", 50.0}, {"sizeY", 50.0}},
        {{"mode", "triplanar"}, {"sizeX", 60.0}, {"sizeY", 60.0}, {"sizeZ", 60.0}},
        {{"mode", "spherical"}, {"positionZ", -20.0}},
        {{"mode", "cylindrical"}, {"rotationX", 90.0}},
        {{"mode", "warp"}, {"sizeX", 50.0}, {"sizeY", 50.0}, {"warp10X", 0.2}, {"warp11Y", -0.1}},
    };
    for (const json& c : cases)
    {
        INFO(c.dump());
        const image::Image img = coords(c);
        double worst = 0.0;
        for (int y = 0; y < kRes; ++y)
            for (int x = 0; x < kRes; ++x)
            {
                const glm::dvec2 want = reference(c, (x + 0.5) / kRes, 1.0 - (y + 0.5) / kRes);
                const float* got = texel(img, x, y);
                // Angles wrap at the seam of spherical and cylindrical: compare on the circle.
                double du = std::abs(got[0] - want.x);
                du = std::min(du, std::abs(1.0 - du));
                worst = std::max({worst, du, std::abs(got[1] - want.y)});
            }
        CHECK(worst < 2e-3);
    }
}

TEST_CASE("tex projection: the coordinate is continuous across a uv seam, with follow too")
{
    for (double follow : {0.0, 1.0})
    {
        const image::Image img = coords({{"mode", "planar"}, {"sizeX", 50.0}, {"sizeY", 50.0}, {"follow", follow}});
        // Texels x and kRes - 1 - x are the two sides of the mirrored unwrap: one surface point each pair.
        for (int y = 0; y < kRes; ++y)
            for (int x = 0; x < kRes / 2; ++x)
            {
                const float *a = texel(img, x, y), *b = texel(img, kRes - 1 - x, y);
                CHECK(a[0] == doctest::Approx(b[0]).epsilon(1e-4));
                CHECK(a[1] == doctest::Approx(b[1]).epsilon(1e-4));
            }
    }
    // Follow carries the coordinate over the bend: it keeps growing past the edge instead of standing still.
    const image::Image flat = coords({{"mode", "planar"}, {"sizeX", 50.0}, {"sizeY", 50.0}});
    const image::Image wrapped = coords({{"mode", "planar"}, {"sizeX", 50.0}, {"sizeY", 50.0}, {"follow", 1.0}});
    const int edge = int(0.05 * kRes);
    CHECK(texel(wrapped, edge, kRes / 2)[0] < texel(flat, edge, kRes / 2)[0]);
}

TEST_CASE("tex projection: a fill image and a projected procedural layer compile through layerProjection, and survive a round trip")
{
    tex::Layer l;
    l.id = "L1";
    l.kind = tex::LayerKind::Fill;
    l.material = {{"baseColor", "textures/decal.png"}};
    l.projection = {{"mode", "planar"}, {"sizeX", 20.0}};
    const tex::Layer back = tex::Layer::fromJson(l.toJson());
    CHECK(back.projection == l.projection);
    CHECK(tex::hasProjection(back.projection));
    CHECK_FALSE(tex::hasProjection(json{{"mode", "uv"}}));

    tex::LayerStack st;
    st.layers.push_back(l);
    tex::SetSettings s = tex::SetSettings::makeDefault();
    const tex::StackCompile c = tex::compileStack(st, s, Graph::makeMaterial("S"));
    const auto sub = c.graph.subgroups.find("L1");
    REQUIRE(sub != c.graph.subgroups.end());
    const Graph group = Graph::fromJson(sub->second);
    const GNode* proj = group.node("Proj_baseColor");
    REQUIRE(proj);
    CHECK(proj->type == "layerProjection");
    CHECK(proj->values.value("sizeX", 0.0) == doctest::Approx(20.0));
}

TEST_CASE("tex projection: a projection follows the mesh it is placed on when the mesh moves, turns and scales")
{
    const glm::dmat4 world = glm::translate(glm::dmat4(1.0), {40.0, -15.0, 70.0}) * glm::rotate(glm::dmat4(1.0), glm::radians(35.0), glm::normalize(glm::dvec3(1, 2, 0.5))) *
                             glm::scale(glm::dmat4(1.0), {1.2, 0.8, 1.5});
    const json cases[] = {
        {{"mode", "planar"}, {"rotationZ", 30.0}, {"positionX", 5.0}, {"sizeX", 40.0}},
        {{"mode", "planar"}, {"follow", 1.0}, {"sizeX", 50.0}, {"sizeY", 50.0}},
        {{"mode", "spherical"}, {"positionZ", -20.0}},
        {{"mode", "cylindrical"}, {"rotationX", 90.0}},
    };
    for (const json& c : cases)
    {
        INFO(c.dump());
        const image::Image still = coords(c), moved = movedCoords(c, world);
        double worst = 0.0;
        for (int y = 0; y < kRes; ++y)
            for (int x = 0; x < kRes; ++x)
            {
                const float *a = texel(still, x, y), *b = texel(moved, x, y);
                double du = std::abs(a[0] - b[0]);
                du = std::min(du, std::abs(1.0 - du));
                worst = std::max({worst, du, double(std::abs(a[1] - b[1]))});
            }
        CHECK(worst < 2e-3);
    }
}

TEST_CASE("tex projection: a planar fill placed before the mesh moves lands in the same place on it after a rebake")
{
    namespace fs = std::filesystem;
    run("newScene");
    const std::string plane = run("polyPlane", {{"width", 100.0}, {"depth", 100.0}})[0];
    run("select", {{"args", {plane}}});
    run("createMaterial", {{"args", {"M_Panel"}}});
    run("assignMaterial", {{"args", {"M_Panel"}}});
    run("texCreateSet", {{"args", {"Panel"}}, {"resolution", 32}, {"materials", {"M_Panel"}}});
    const fs::path dir = tempDir("projection_follow");
    run("createBakeProject", {{"name", "PB"}, {"folder", (dir / "bake").generic_string()}});
    // Float positions, so the comparison measures the projection rather than the map's quantisation.
    run("setBakeOutput", {{"project", "PB"}, {"resolution", 32}, {"supersampling", 1}, {"format", "exr"}, {"bits", 32}});
    run("bakeGroup", {{"args", {"G"}}, {"project", "PB"}, {"lows", {plane}}, {"self", true}});
    run("texSetSettings", {{"args", {"Panel", {{"bakeProject", "PB"}}}}});
    auto bake = [] { run("bake", {{"project", "PB"}, {"maps", {"position", "objectNormal"}}}); };
    bake();

    // A picture whose colour is its own coordinate, projected straight down onto the middle of the plane.
    std::vector<float> px(16 * 16 * 4);
    for (int y = 0; y < 16; ++y)
        for (int x = 0; x < 16; ++x)
        {
            float* p = &px[(size_t(y) * 16 + x) * 4];
            p[0] = float(x) / 15.0f, p[1] = float(y) / 15.0f, p[2] = 0.0f, p[3] = 1.0f;
        }
    image::writeExr(dir / "coord.exr", 16, 16, px, 32);
    const json projection = {{"mode", "planar"}, {"rotationX", -90.0}, {"positionX", 10.0}, {"sizeX", 60.0}, {"sizeY", 60.0}, {"sizeZ", 60.0}};
    run("texAddLayer", {{"args", {"Panel", "fill"}}, {"material", {{"baseColor", (dir / "coord.exr").generic_string()}}}, {"projection", projection}});
    auto baseColor = [&](const std::string& name) {
        const fs::path out = dir / name;
        run("texEvaluate", {{"args", {"Panel"}}, {"folder", out.generic_string()}, {"channels", {"baseColor"}}, {"format", "exr"}});
        return image::read(out / "Panel_baseColor.exr");
    };
    const image::Image before = baseColor("before");
    REQUIRE(before.width == 32);
    double spread = 0.0;
    for (size_t i = 0; i < before.rgba.size(); i += 4)
        spread = std::max(spread, double(before.rgba[i]));
    CHECK(spread > 0.5);

    run("setAttr", {{"args", {plane + ".translate", 35.0, 12.0, -50.0}}});
    run("setAttr", {{"args", {plane + ".rotate", 25.0, 60.0, -10.0}}});
    bake();
    const image::Image after = baseColor("after");
    REQUIRE(after.width == 32);
    double worst = 0.0;
    for (size_t i = 0; i < before.rgba.size(); ++i)
        if (i % 4 != 3)
            worst = std::max(worst, std::abs(double(before.rgba[i]) - double(after.rgba[i])));
    MESSAGE("largest change on the mesh after the move: " << worst);
    CHECK(worst < 0.01);
}
