#include "TestUtil.h"

#include "MaskCompute.h"
#include "geo/ColorSets.h"
#include "geo/Primitives.h"
#include "render/RenderMesh.h"
#include "scene/Modifiers.h"
#include "scene/Transform.h"
#include "tex/TextureSet.h"
#include "ui/MeshCache.h"
#include "ui/ViewportHooks.h"

#include <glm/gtc/matrix_transform.hpp>

#include <chrono>
#include <cmath>
#include <map>

using namespace weatheringtest;

namespace
{
/** A mask's corner values with each corner's point position, in the mesh's own space. */
struct Sampled
{
    glm::dvec3 p;
    glm::dvec3 n; // the corner's face normal
    float v;
};

std::vector<Sampled> sampled(const geo::Mesh& m, const char* attr)
{
    std::vector<Sampled> out;
    const auto v = m.cornerAttrs().get<float>(attr);
    REQUIRE(!v.empty());
    for (geo::Index c = 0; c < m.cornerCapacity(); ++c)
        if (m.cornerValid(c))
            out.push_back({m.position(m.cornerPoint(c)), m.faceNormal(m.cornerFace(c)), v[c]});
    return out;
}

json masks(const std::string& mesh, json flags = json::object())
{
    flags["args"] = json::array({mesh});
    return run("grungeMask", flags);
}

std::string subCube(const char* name, const glm::dvec3& at, const glm::dvec3& size, int sx, int sy, int sz)
{
    return run("polyCube", {{"name", name},
                            {"position", json::array({at.x, at.y, at.z})},
                            {"width", size.x},
                            {"height", size.y},
                            {"depth", size.z},
                            {"subdivisionsX", sx},
                            {"subdivisionsY", sy},
                            {"subdivisionsZ", sz},
                            {"baseAtOrigin", true},
                            {"generator", false}})[0]
        .get<std::string>();
}

/** A 300 x 300 x 20 cm wall (front at z = 20) on y = 0 with a window from x 100 to 200, y 150 to 250, on a 50 cm grid. */
std::string windowWall()
{
    std::vector<glm::dvec3> pts;
    std::map<std::tuple<int, int, int>, int> ids;
    auto pt = [&](double x, double y, double z) {
        const auto key = std::make_tuple(int(std::lround(x)), int(std::lround(y)), int(std::lround(z)));
        if (auto it = ids.find(key); it != ids.end())
            return it->second;
        pts.push_back({x, y, z});
        return ids[key] = int(pts.size()) - 1;
    };
    json faces = json::array();
    auto quad = [&](glm::dvec3 a, glm::dvec3 b, glm::dvec3 c, glm::dvec3 d, glm::dvec3 outward) {
        std::vector<int> f = {pt(a.x, a.y, a.z), pt(b.x, b.y, b.z), pt(c.x, c.y, c.z), pt(d.x, d.y, d.z)};
        if (glm::dot(glm::cross(b - a, c - a), outward) < 0.0)
            std::reverse(f.begin(), f.end());
        faces.push_back(f);
    };
    auto hole = [](double x, double y) { return x >= 100 && x < 200 && y >= 150 && y < 250; };
    for (double x = 0; x < 300; x += 50)
        for (double y = 0; y < 300; y += 50)
            if (!hole(x, y))
            {
                quad({x, y, 20}, {x + 50, y, 20}, {x + 50, y + 50, 20}, {x, y + 50, 20}, {0, 0, 1});
                quad({x, y, 0}, {x + 50, y, 0}, {x + 50, y + 50, 0}, {x, y + 50, 0}, {0, 0, -1});
            }
    for (double s = 0; s < 300; s += 50)
    {
        quad({0, s, 0}, {0, s + 50, 0}, {0, s + 50, 20}, {0, s, 20}, {-1, 0, 0});
        quad({300, s, 0}, {300, s + 50, 0}, {300, s + 50, 20}, {300, s, 20}, {1, 0, 0});
        quad({s, 0, 0}, {s + 50, 0, 0}, {s + 50, 0, 20}, {s, 0, 20}, {0, -1, 0});
        quad({s, 300, 0}, {s + 50, 300, 0}, {s + 50, 300, 20}, {s, 300, 20}, {0, 1, 0});
    }
    for (double s = 100; s < 200; s += 50)
    {
        quad({s, 150, 0}, {s + 50, 150, 0}, {s + 50, 150, 20}, {s, 150, 20}, {0, 1, 0});  // sill
        quad({s, 250, 0}, {s + 50, 250, 0}, {s + 50, 250, 20}, {s, 250, 20}, {0, -1, 0}); // lintel
    }
    for (double s = 150; s < 250; s += 50)
    {
        quad({100, s, 0}, {100, s + 50, 0}, {100, s + 50, 20}, {100, s, 20}, {1, 0, 0});
        quad({200, s, 0}, {200, s + 50, 0}, {200, s + 50, 20}, {200, s, 20}, {-1, 0, 0});
    }
    json points = json::array();
    for (const glm::dvec3& p : pts)
        points.push_back({p.x, p.y, p.z});
    const json made = run("createMesh", {{"points", points}, {"faces", faces}, {"name", "Wall"}});
    return made.is_array() ? made[0].get<std::string>() : made.get<std::string>();
}

bool near(double a, double b) { return std::abs(a - b) < 1e-6; }
} // namespace

TEST_CASE("grunge edge wear covers a cube's edges only")
{
    fresh();
    const std::string c = subCube("Box", {0, 0, 0}, {100, 100, 100}, 4, 4, 4);
    masks(c, {{"neighbours", false}});
    const geo::Mesh m = meshOf(c);
    int onEdge = 0, inside = 0;
    for (const Sampled& s : sampled(m, "grungeEdge"))
    {
        int extremes = 0;
        for (int k = 0; k < 3; ++k)
        {
            const double lo = k == 1 ? 0.0 : -50.0, hi = k == 1 ? 100.0 : 50.0;
            extremes += near(s.p[k], lo) || near(s.p[k], hi) ? 1 : 0;
        }
        if (extremes >= 2)
        {
            // 25 cm faces are past twice the 4 cm reach, so the corner value is off; the per-pixel edge distances carry it.
            CHECK(s.v == doctest::Approx(0.0f));
            ++onEdge;
        }
        else
        {
            CHECK(s.v == doctest::Approx(0.0f));
            ++inside;
        }
    }
    CHECK(onEdge > 0);
    CHECK(inside > 0);
}

TEST_CASE("grunge edge wear holds per pixel on an 8-vertex block")
{
    fresh();
    // Off the ground: an edge resting on something is a contact line, not a worn edge (see the 4-corner wall test).
    const std::string c = subCube("Block", {0, 0, 0}, {200, 200, 200}, 1, 1, 1);
    masks(c, {{"neighbours", false}, {"groundPlane", false}});
    const geo::Mesh m = meshOf(c);
    REQUIRE(m.numPoints() == 8);
    const auto d = m.cornerAttrs().get<glm::vec4>(render::kGrungeEdgeDistanceAttr);
    REQUIRE(!d.empty());
    auto nearest = [](const glm::vec4& v) { return std::min(std::min(v.x, v.y), std::min(v.z, v.w)); };
    const double edgeWidth = 4.0;
    for (geo::Index f = 0; f < m.faceCapacity(); ++f)
    {
        if (!m.faceValid(f))
            continue;
        const std::vector<geo::Index> corners = m.faceCorners(f);
        REQUIRE(corners.size() == 4);
        // What the rasterizer interpolates: the face centre is the corners' mean, an edge midpoint its two corners' mean.
        glm::vec4 centre(0.0f);
        for (geo::Index k : corners)
            centre += d[k] * 0.25f;
        CHECK(nearest(centre) == doctest::Approx(100.0).epsilon(0.001));
        CHECK(std::clamp(1.0 - nearest(centre) / edgeWidth, 0.0, 1.0) == doctest::Approx(0.0));
        for (size_t k = 0; k < 4; ++k)
        {
            const glm::vec4 mid = (d[corners[k]] + d[corners[(k + 1) % 4]]) * 0.5f;
            CHECK(nearest(mid) == doctest::Approx(0.0).epsilon(0.001));
        }
    }
    // The render copy carries them as halves after the masks.
    const auto rm = ui::renderMeshFor(std::make_shared<geo::Mesh>(m));
    REQUIRE(rm);
    REQUIRE(rm->grunge.size() == rm->vertices.size() * size_t(render::kGrungeWords));
    for (size_t v = 0; v < rm->vertices.size(); ++v)
    {
        const glm::vec2 a = glm::unpackHalf2x16(rm->grunge[v * render::kGrungeWords + 3]);
        const glm::vec2 b = glm::unpackHalf2x16(rm->grunge[v * render::kGrungeWords + 4]);
        CHECK(std::min(std::min(a.x, a.y), std::min(b.x, b.y)) == doctest::Approx(0.0).epsilon(0.001));
        CHECK(std::max(std::max(a.x, a.y), std::max(b.x, b.y)) == doctest::Approx(200.0).epsilon(0.001));
    }
}

TEST_CASE("grunge streaks run below a window opening only")
{
    fresh();
    const std::string wall = windowWall();
    masks(wall, {{"neighbours", false}, {"streakLength", 150.0}});
    const geo::Mesh m = meshOf(wall);
    int under = 0;
    for (const Sampled& s : sampled(m, "grungeStreak"))
    {
        if (!near(s.n.z, 1.0) || !near(s.p.z, 20.0))
            continue;
        const bool below = s.p.x >= 100 && s.p.x <= 200 && s.p.y < 150 && s.p.y > 0;
        INFO("point " << s.p.x << " " << s.p.y);
        if (s.p.x >= 100 && s.p.x <= 200 && near(s.p.y, 150.0))
            continue; // on the sill's edge, where the streak starts
        if (below)
        {
            CHECK(s.v > 0.2f);
            ++under;
        }
        else
            CHECK(s.v == doctest::Approx(0.0f));
    }
    CHECK(under > 0);
    // The sill's height: right under it the streak is strongest.
    const json info = run("grungeInfo", {{"args", {wall}}});
    CHECK(info["masks"]["streak"]["max"].get<double>() > 0.6);
}

TEST_CASE("grunge fade sits near the ground only")
{
    fresh();
    const std::string c = subCube("Tower", {0, 0, 0}, {100, 400, 100}, 1, 8, 1);
    masks(c, {{"neighbours", false}, {"fadeHeight", 60.0}});
    const geo::Mesh m = meshOf(c);
    // Per pixel the fade is 1 - height / 60 from the height field, which must be the corner's height above the ground.
    const auto h = m.cornerAttrs().get<float>(render::kGrungeHeightAttr);
    REQUIRE(!h.empty());
    int low = 0;
    for (geo::Index k = 0; k < m.cornerCapacity(); ++k)
    {
        if (!m.cornerValid(k))
            continue;
        const glm::dvec3 n = m.faceNormal(m.cornerFace(k));
        const double y = m.position(m.cornerPoint(k)).y;
        if (std::abs(n.y) > 0.5)
        {
            CHECK(h[k] >= 5000.0f);
            continue;
        }
        CHECK(h[k] == doctest::Approx(y).epsilon(0.01));
        low += near(y, 0.0) ? 1 : 0;
    }
    CHECK(low > 0);
}

TEST_CASE("grunge cavity gathers where a wall meets the floor it stands on")
{
    fresh();
    const std::string floor = subCube("Floor", {0, 0, 0}, {600, 20, 600}, 1, 1, 1);
    const std::string wall = subCube("Pillar", {0, 20, 0}, {100, 200, 100}, 2, 8, 2);
    masks(wall, {{"groundPlane", false}, {"cavityWidth", 10.0}});
    masks(floor, {{"groundPlane", false}});
    const geo::Mesh m = meshOf(wall);
    int foot = 0;
    for (const Sampled& s : sampled(m, "grungeCavity"))
    {
        if (std::abs(s.n.y) > 0.5)
            continue;
        if (near(s.p.y, 0.0))
        {
            // 50 cm faces are past twice the 10 cm reach: the per-pixel contact lines carry it.
            CHECK(s.v == doctest::Approx(0.0f));
            ++foot;
        }
        else if (s.p.y > 20.0)
            CHECK(s.v == doctest::Approx(0.0f));
    }
    CHECK(foot > 0);
    CHECK(run("grungeInfo", {{"args", {wall}}})["neighbours"].get<int>() >= 1);
}

TEST_CASE("grunge masks are deterministic for a seed")
{
    auto build = [](int64_t seed) {
        fresh();
        const std::string floor = subCube("Floor", {0, 0, 0}, {400, 20, 400}, 1, 1, 1);
        const std::string c = subCube("Box", {0, 20, 0}, {100, 150, 100}, 3, 3, 3);
        masks(c, {{"seed", seed}});
        masks(floor, {{"seed", seed}});
        const geo::Mesh m = meshOf(c);
        std::vector<float> all;
        for (const char* a : render::kGrungeAttrs)
            for (const Sampled& s : sampled(m, a))
                all.push_back(s.v);
        return all;
    };
    const std::vector<float> a = build(3), b = build(3);
    CHECK(a == b);
}

TEST_CASE("grunge masks follow their host and recompute only for geometry")
{
    fresh();
    const std::string c = subCube("Box", {0, 0, 0}, {100, 100, 100}, 2, 2, 2);
    masks(c, {{"neighbours", false}});
    CHECK(run("grungeInfo", {{"args", {c}}})["masks"]["fade"]["max"].get<double>() > 0.9);

    // Lifted off the ground its sides no longer fade.
    run("move", {{"args", {c}}, {"by", json::array({0, 300, 0})}});
    CHECK(run("grungeInfo", {{"args", {c}}})["masks"]["fade"]["max"].get<double>() == doctest::Approx(0.0));

    // A look change keeps the computed mesh: same pointer, no new geometric pass.
    g().evaluate();
    scene::Node* mesh = g().find(c);
    const scene::MeshPtr before = mesh->as<scene::MeshPtr>(scene::MeshNode::kOutMesh);
    const int64_t passes = run("grungeInfo", {{"args", {c}}})["passes"].get<int64_t>();
    masks(c, {{"look", {{"edgeLevel", 0.2}, {"edgeBands", 3}}}});
    g().evaluate();
    CHECK(run("grungeInfo", {{"args", {c}}})["passes"].get<int64_t>() == passes);
    CHECK(mesh->as<scene::MeshPtr>(scene::MeshNode::kOutMesh).get() == before.get());
}

TEST_CASE("grunge masks update when the host mesh is edited")
{
    fresh();
    const std::string c = subCube("Box", {0, 0, 0}, {100, 100, 100}, 1, 4, 1);
    masks(c, {{"neighbours", false}, {"fadeHeight", 60.0}});
    const double before = run("grungeInfo", {{"args", {c}}})["masks"]["fade"]["mean"].get<double>();
    // A direct edit of the base mesh: the box stretched ten times taller.
    const scene::PlugRef base = scene::modifiers::editPlug(g(), g().find(c)->id());
    REQUIRE(base.valid());
    auto taller = std::make_shared<geo::Mesh>(*std::get<scene::MeshPtr>(g().storedValue(base)));
    for (glm::dvec3& p : taller->positions())
        p.y *= 10.0;
    g().setValue(base, scene::MeshPtr(taller));
    const double after = run("grungeInfo", {{"args", {c}}})["masks"]["fade"]["mean"].get<double>();
    // A taller box has a smaller share of its sides near the ground.
    CHECK(after < before * 0.75);
}

TEST_CASE("grunge reaches the viewport: masks per render vertex and a look on the materials")
{
    fresh();
    const std::string c = subCube("Box", {0, 0, 0}, {100, 100, 100}, 1, 1, 1);
    masks(c, {{"neighbours", false}, {"style", "toon"}});
    const geo::Mesh m = meshOf(c);
    const auto rm = ui::renderMeshFor(std::make_shared<geo::Mesh>(m));
    REQUIRE(rm);
    CHECK(rm->grunge.size() == rm->vertices.size() * size_t(render::kGrungeWords));
    std::vector<render::Material> mats(1);
    ui::viewportHooks::decorateMaterials(g().find(c)->id(), m, mats);
    REQUIRE(mats[0].grunge);
    CHECK(mats[0].grunge->layers[0].bands == 2);
    CHECK(mats[0].grunge->layers[0].enabled);
    const auto block = render::packGrunge(*mats[0].grunge, 0x1234567890ull);
    CHECK(block[0] == 0x34567890u);
    CHECK(block[2] == 0x3fu);
}

TEST_CASE("grunge bakes to generator layers and comes back from them")
{
    static const bool texReady = [] {
        tex::registerTypes();
        tex::registerCommands();
        return true;
    }();
    CHECK(texReady);
    fresh();
    const std::string a = subCube("Crate", {0, 0, 0}, {100, 100, 100}, 1, 1, 1);
    masks(a, {{"neighbours", false}, {"masks", {"edge", "cavity", "dust"}}, {"look", {{"edgeLevel", 0.37}, {"edgeBands", 3}}}});
    const json baked = run("grungeBake", {{"args", {a}}});
    const json r = baked[g().find(a)->path()];
    CHECK(r["layers"].size() == 3);
    CHECK(r["skipped"].dump().find("edgeBands") != std::string::npos);
    const std::string set = r["set"].get<std::string>();
    const json stack = run("texLayerStack", {{"args", {set}}});
    int generators = 0;
    for (const json& layer : stack["layers"])
        for (const json& mask : layer.value("masks", json::array()))
            if (mask.value("type", "") == "genEdgeWear")
            {
                CHECK(mask["values"]["level"].get<double>() == doctest::Approx(0.37));
                ++generators;
            }
    CHECK(generators == 1);
    // The live masks are off once baked.
    CHECK(run("grungeInfo", {{"args", {a}}})["masks"]["edge"]["max"].get<double>() == doctest::Approx(0.0));

    const std::string b = subCube("Crate2", {300, 0, 0}, {100, 100, 100}, 1, 1, 1);
    const json back = run("grungeFromLayers", {{"args", {set, b}}});
    CHECK(back[g().find(b)->path()].contains("edge"));
    CHECK(back[g().find(b)->path()].contains("dust"));
    scene::Node* mod = g().find(run("grungeInfo", {{"args", {b}}})["modifier"].get<std::string>());
    CHECK(mod->as<double>(mod->type().attrIndex("edgeLevel")) == doctest::Approx(0.37));
    CHECK_FALSE(mod->as<bool>(mod->type().attrIndex("aoEnabled")));
}

TEST_CASE("grunge sends its look as MF_LilaGrunge instance parameters and writes Unreal channels")
{
    fresh();
    const std::string c = subCube("Box", {0, 0, 0}, {100, 100, 100}, 1, 1, 1);
    masks(c, {{"neighbours", false}, {"look", {{"cavityLevel", 0.6}}}});
    const json sent = run("grungeSend", {{"args", {c}}, {"dryRun", true}});
    REQUIRE(sent["instances"].size() == 1);
    const json& i = sent["instances"][0];
    CHECK(i["name"] == "MI_Grunge_Box");
    CHECK(i["layout"] == "color");
    CHECK(i["scalars"]["CavityLevel"].get<double>() == doctest::Approx(0.6));
    CHECK(i["vectors"].contains("StreaksColor"));
    const geo::Mesh m = meshOf(c);
    CHECK(geo::colors::has(m, "Grunge"));
    CHECK(m.cornerAttrs().find("uv3") != nullptr);
    CHECK(m.cornerAttrs().find("uv5") != nullptr);
    CHECK(i["channels"]["edgeDistance"] == "uv4-uv5");
    CHECK(i["scalars"].contains("EdgeWidth"));
}

TEST_CASE("grunge attribute pass on a 500 block level")
{
    // 500 boxes on a 25 x 20 grid, 2 cm apart, each seeing its neighbours: the pass a level runs when it is first aged.
    std::vector<geo::Mesh> boxes;
    std::vector<glm::dmat4> worlds;
    const geo::Mesh box = geo::makeCube({100, 120, 100, 1, 1, 1, true});
    for (int i = 0; i < 500; ++i)
    {
        boxes.push_back(box);
        worlds.push_back(glm::translate(glm::dmat4(1.0), glm::dvec3((i % 25) * 102.0, 0.0, (i / 25) * 102.0)));
    }
    weathering::masks::Settings s;
    const auto t0 = std::chrono::steady_clock::now();
    size_t samples = 0;
    for (int i = 0; i < 500; ++i)
    {
        std::vector<weathering::masks::Neighbour> near;
        for (int j = 0; j < 500; ++j)
            if (j != i && std::abs(worlds[j][3].x - worlds[i][3].x) < 210.0 && std::abs(worlds[j][3].z - worlds[i][3].z) < 210.0)
                near.push_back({&boxes[size_t(j)], worlds[size_t(j)]});
        samples += weathering::masks::compute(boxes[size_t(i)], worlds[size_t(i)], near, s).samples;
    }
    const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
    MESSAGE("grunge attribute pass, 500 blocks with neighbours: " << ms << " ms total, " << ms / 500.0 << " ms per block, " << samples << " samples");
    CHECK(samples == 500 * 24);
    // One block's edit recomputes that block alone: well under a frame.
    CHECK(ms / 500.0 < 16.0);
}

TEST_CASE("grunge cavity, occlusion and fade hold per pixel on a 4-corner wall")
{
    fresh();
    const std::string c = subCube("Wall", {0, 0, 0}, {600, 300, 30}, 1, 1, 1);
    masks(c, {{"neighbours", false}, {"fadeHeight", 60.0}, {"cavityWidth", 10.0}});
    const geo::Mesh m = meshOf(c);
    REQUIRE(m.numPoints() == 8);
    const auto contact = m.cornerAttrs().get<glm::vec4>(render::kGrungeContactDistanceAttr);
    const auto height = m.cornerAttrs().get<float>(render::kGrungeHeightAttr);
    const auto edge = m.cornerAttrs().get<glm::vec4>(render::kGrungeEdgeDistanceAttr);
    REQUIRE(!contact.empty());
    REQUIRE(!height.empty());
    auto nearest = [](const glm::vec4& v) { return std::min(std::min(v.x, v.y), std::min(v.z, v.w)); };
    int sides = 0;
    for (geo::Index f = 0; f < m.faceCapacity(); ++f)
    {
        if (!m.faceValid(f) || std::abs(m.faceNormal(f).y) > 0.5)
            continue;
        ++sides;
        const std::vector<geo::Index> corners = m.faceCorners(f);
        REQUIRE(corners.size() == 4);
        // What the rasterizer interpolates: linear fields, so the face centre is the corners' mean.
        glm::vec4 cc(0.0f), ec(0.0f);
        float hc = 0.0f;
        for (geo::Index k : corners)
        {
            cc += contact[k] * 0.25f;
            ec += edge[k] * 0.25f;
            hc += height[k] * 0.25f;
        }
        CHECK(hc == doctest::Approx(150.0).epsilon(0.01));
        CHECK(nearest(cc) == doctest::Approx(150.0).epsilon(0.01));
        // Fade and dirt at the centre of the wall: none.
        CHECK(std::clamp(1.0 - hc / 60.0, 0.0, 1.0) == doctest::Approx(0.0));
        CHECK(std::clamp(1.0 - nearest(cc) / 10.0, 0.0, 1.0) == doctest::Approx(0.0));
        for (size_t k = 0; k < 4; ++k)
        {
            const geo::Index a = corners[k], b = corners[(k + 1) % 4];
            const glm::dvec3 pa = m.position(m.cornerPoint(a)), pb = m.position(m.cornerPoint(b));
            const float hm = (height[a] + height[b]) * 0.5f;
            const glm::vec4 cm = (contact[a] + contact[b]) * 0.5f, em = (edge[a] + edge[b]) * 0.5f;
            if (pa.y < 1.0 && pb.y < 1.0)
            {
                // The foot: full fade and dirt, and no edge wear where it rests on the ground.
                CHECK(hm == doctest::Approx(0.0).epsilon(0.01));
                CHECK(nearest(cm) == doctest::Approx(0.0).epsilon(0.01));
                CHECK(nearest(em) > 10.0f);
            }
            else if (pa.y > 299.0 && pb.y > 299.0)
            {
                CHECK(hm == doctest::Approx(300.0).epsilon(0.01));
                CHECK(nearest(cm) > 100.0f);
                CHECK(nearest(em) == doctest::Approx(0.0).epsilon(0.01));
            }
        }
    }
    CHECK(sides == 4);
    // The render copy carries the fields after the edge distances.
    const auto rm = ui::renderMeshFor(std::make_shared<geo::Mesh>(m));
    REQUIRE(rm);
    REQUIRE(rm->grunge.size() == rm->vertices.size() * size_t(render::kGrungeWords));
}

TEST_CASE("grunge mask data never shows as viewport colour")
{
    fresh();
    const std::string c = subCube("Painted", {0, 0, 0}, {100, 100, 100}, 1, 1, 1);
    masks(c, {{"neighbours", false}, {"unreal", "color"}});
    const geo::Mesh m = meshOf(c);
    CHECK(!geo::colors::sets(m).empty());
    CHECK(geo::colors::displayed(m).empty());
}

TEST_CASE("grunge lines skip concave faces they would cut across")
{
    fresh();
    // One L-shaped face standing on the ground: its reflex corner's edges must not carry lines across the face.
    const json pts = json::array({{0, 0, 0}, {300, 0, 0}, {300, 100, 0}, {100, 100, 0}, {100, 300, 0}, {0, 300, 0}});
    const json made = run("createMesh", {{"points", pts}, {"faces", json::array({json::array({0, 1, 2, 3, 4, 5})})}, {"name", "ElWall"}});
    const std::string c = made.is_string() ? made.get<std::string>() : made[0].get<std::string>();
    masks(c, {{"neighbours", false}});
    const geo::Mesh m = meshOf(c);
    for (const char* attr : {render::kGrungeContactDistanceAttr, render::kGrungeEdgeDistanceAttr})
    {
        const auto d = m.cornerAttrs().get<glm::vec4>(attr);
        REQUIRE(!d.empty());
        for (geo::Index k = 0; k < m.cornerCapacity(); ++k)
            if (m.cornerValid(k))
                for (int i = 0; i < 4; ++i)
                    CHECK(d[k][i] > -0.5f);
    }
}
