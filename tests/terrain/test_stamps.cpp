#include "TestUtil.h"
#include "ErosionGpu.h"
#include "Export.h"
#include "Stamps.h"
#include "Terrain.h"

#include "extensions/ExtensionManager.h"
#include "geo/Heightfield.h"
#include "geo/Primitives.h"
#include "gn/NodeLib.h"
#include "curve/CurveNodes.h"
#include "math/MathCore.h"
#include "ops/Command.h"
#include "render/Renderer.h"
#include "scene/Graph.h"

#include <doctest/doctest.h>
#include <glm/gtc/matrix_transform.hpp>

#include <chrono>
#include <cmath>
#include <filesystem>
#include <fstream>

using namespace lila;
using namespace terraintest;
using json = nlohmann::json;
namespace fs = std::filesystem;

namespace
{
/** A tilted, bumpy heightfield, km wide. */
geo::Heightfield slope(int n = 257, double size = 400000.0)
{
    geo::Heightfield h = geo::makeHeightfieldCovering({0.0, 0.0}, {size, size}, n);
    for (int z = 0; z < h.nz; ++z)
        for (int x = 0; x < h.nx; ++x)
        {
            const glm::dvec2 p = h.samplePos(x, z);
            h.heights[h.index(x, z)] = float(0.05 * p.x + 2000.0 * std::sin(p.y / 30000.0));
        }
    return h;
}

std::vector<glm::dvec2> rect(glm::dvec2 c, glm::dvec2 half)
{
    return {c + glm::dvec2(-half.x, -half.y), c + glm::dvec2(half.x, -half.y), c + glm::dvec2(half.x, half.y), c + glm::dvec2(-half.x, half.y)};
}

json square(double cx, double cz, double half)
{
    return json::array({{cx - half, cz - half}, {cx + half, cz - half}, {cx + half, cz + half}, {cx - half, cz + half}});
}

struct UpAxisGuard
{
    explicit UpAxisGuard(math::UpAxis a) { math::setUpAxis(a); }
    ~UpAxisGuard() { math::setUpAxis(math::UpAxis::Y); }
};
} // namespace

TEST_CASE("stamps: a flatten pad levels every sample under its footprint, skirts back to the ground and writes its mask")
{
    geo::Heightfield h = slope();
    const geo::Heightfield before = h;
    terrain::Stamp s;
    s.id = "hall";
    s.falloff = 6000.0;
    s.polygon = rect({20000.0, -10000.0}, {6000.0, 4000.0});
    const terrain::StampResult r = terrain::applyStamp(h, s, terrain::footprintFromPolygon(s.polygon));
    REQUIRE(r.samples > 0);
    CHECK(r.padHeight == doctest::Approx(r.groundMean));
    CHECK(r.cut > 0.0);
    CHECK(r.fill > 0.0);
    // Inside the footprint the bilinear surface is the pad, anywhere.
    for (glm::dvec2 p : {glm::dvec2(14100.0, -6100.0), glm::dvec2(25900.0, -13900.0), glm::dvec2(20000.0, -10000.0)})
    {
        CHECK(h.heightAt(p.x, p.y) == doctest::Approx(r.padHeight).epsilon(1e-6));
        CHECK(h.maskAt("cityFootprint", p.x, p.y) == doctest::Approx(1.0));
    }
    // Past the skirt the ground is untouched.
    CHECK(h.heightAt(20000.0, 10000.0) == doctest::Approx(before.heightAt(20000.0, 10000.0)));
    CHECK(h.maskAt("cityFootprint", 20000.0, 10000.0) == doctest::Approx(0.0));
    // Mid-skirt is between pad and ground.
    const double mid = h.heightAt(29000.0 + 3000.0, -10000.0), g = before.heightAt(32000.0, -10000.0);
    CHECK(mid > std::min(g, r.padHeight) - 1.0);
    CHECK(mid < std::max(g, r.padHeight) + 1.0);
}

TEST_CASE("stamps: a hut smaller than a cell on a km terrain still gets a level pad under it")
{
    geo::Heightfield h = slope(201, 2000000.0); // 20 km, 100 m cells
    terrain::Stamp s;
    s.falloff = 0.0;
    s.edge = terrain::StampEdge::Wall;
    s.polygon = rect({123456.0, -65432.0}, {250.0, 250.0});
    const terrain::StampResult r = terrain::applyStamp(h, s, terrain::footprintFromPolygon(s.polygon));
    REQUIRE(r.samples >= 4);
    for (const glm::dvec2& p : s.polygon)
        CHECK(h.heightAt(p.x, p.y) == doctest::Approx(r.padHeight).epsilon(1e-6));
}

TEST_CASE("stamps: carve only cuts, raise only fills, terraced makes steps, stilts and plinths leave the ground and report")
{
    terrain::Stamp s;
    s.falloff = 0.0;
    s.polygon = rect({0.0, 0.0}, {40000.0, 20000.0});
    {
        geo::Heightfield h = slope();
        s.mode = terrain::StampMode::Carve;
        const terrain::StampResult r = terrain::applyStamp(h, s, terrain::footprintFromPolygon(s.polygon));
        CHECK(r.fill == doctest::Approx(0.0));
        CHECK(r.cut > 0.0);
    }
    {
        geo::Heightfield h = slope();
        s.mode = terrain::StampMode::Raise;
        const terrain::StampResult r = terrain::applyStamp(h, s, terrain::footprintFromPolygon(s.polygon));
        CHECK(r.cut == doctest::Approx(0.0));
        CHECK(r.fill > 0.0);
    }
    {
        geo::Heightfield h = slope();
        s.mode = terrain::StampMode::Terraced;
        s.steps = 4;
        const terrain::StampResult r = terrain::applyStamp(h, s, terrain::footprintFromPolygon(s.polygon));
        REQUIRE(r.levels.size() == 4);
        CHECK(r.levels.front() != doctest::Approx(r.levels.back()));
    }
    {
        geo::Heightfield h = slope();
        const geo::Heightfield before = h;
        s.mode = terrain::StampMode::Stilts;
        s.level = terrain::StampLevel::Max;
        const terrain::StampResult r = terrain::applyStamp(h, s, terrain::footprintFromPolygon(s.polygon));
        CHECK(h.heights == before.heights);
        REQUIRE(r.piles.size() == 4);
        for (const auto& p : r.piles)
            CHECK(p.top >= p.ground - 1e-3);
        s.mode = terrain::StampMode::Plinth;
        const terrain::StampResult q = terrain::applyStamp(h, s, terrain::footprintFromPolygon(s.polygon));
        CHECK(q.plinthDepth == doctest::Approx(q.groundMax - q.groundMin));
    }
}

TEST_CASE("stamps: a sheer region on a slope reports retaining walls; a graded one follows its grade")
{
    geo::Heightfield h = slope();
    terrain::Stamp s;
    s.kind = "region";
    s.edge = terrain::StampEdge::Wall;
    s.polygon = rect({0.0, 0.0}, {60000.0, 30000.0});
    terrain::StampResult r = terrain::applyStamp(h, s, terrain::footprintFromPolygon(s.polygon));
    CHECK_FALSE(r.walls.empty());

    geo::Heightfield g = slope();
    s.edge = terrain::StampEdge::Slope;
    s.grade = 2.0;
    s.direction = {1.0, 0.0};
    r = terrain::applyStamp(g, s, terrain::footprintFromPolygon(s.polygon));
    const double drop = g.heightAt(-20000.0, 0.0) - g.heightAt(20000.0, 0.0);
    CHECK(drop == doctest::Approx(800.0).epsilon(0.02));
}

TEST_CASE("stamps: mesh footprints use their faces from above; closed meshes fall back to their hull outline")
{
    geo::Mesh m = geo::makeCube({1000.0, 500.0, 2000.0});
    const terrain::Footprint f = terrain::footprintFromMesh(m, glm::translate(glm::dmat4(1.0), glm::dvec3(5000.0, 300.0, 0.0)));
    CHECK(f.rings.size() == 6);
    REQUIRE(f.outline.size() == 1);
    CHECK(f.outline[0].size() == 4);
    CHECK(f.lowestY == doctest::Approx(50.0));
}

TEST_CASE("sculpt: raise, lower, flatten and smooth strokes do what they say")
{
    geo::Heightfield h = slope(129, 100000.0);
    const geo::Heightfield before = h;
    terrain::SculptStroke s;
    s.points = {{0.0, 0.0}};
    s.radius = 10000.0;
    s.strength = 500.0;
    terrain::applyStroke(h, s);
    CHECK(h.heightAt(0.0, 0.0) == doctest::Approx(before.heightAt(0.0, 0.0) + 500.0).epsilon(1e-4));
    CHECK(h.heightAt(30000.0, 0.0) == doctest::Approx(before.heightAt(30000.0, 0.0)));

    s.op = terrain::SculptOp::Flatten;
    s.strength = 1.0;
    s.height = 1234.0;
    s.points = {{-20000.0, 20000.0}, {20000.0, 20000.0}};
    s.path = true;
    terrain::applyStroke(h, s);
    CHECK(h.heightAt(0.0, 20000.0) == doctest::Approx(1234.0).epsilon(1e-4));

    geo::Heightfield n = geo::makeHeightfieldCovering({0.0, 0.0}, {100000.0, 100000.0}, 129);
    for (size_t k = 0; k < n.heights.size(); ++k)
        n.heights[k] = (k % 2) ? 100.0f : -100.0f;
    terrain::SculptStroke sm;
    sm.op = terrain::SculptOp::Smooth;
    sm.points = {{0.0, 0.0}};
    sm.radius = 20000.0;
    sm.strength = 1.0;
    terrain::applyStroke(n, sm);
    CHECK(std::abs(n.heightAt(0.0, 0.0)) < 30.0);
}

TEST_CASE("route: a graded route keeps under its max grade with switchbacks, and gives up only when it must")
{
    // A 30% ramp: a straight climb is too steep for 10%.
    geo::Heightfield h = geo::makeHeightfieldCovering({0.0, 0.0}, {100000.0, 100000.0}, 201);
    for (int z = 0; z < h.nz; ++z)
        for (int x = 0; x < h.nx; ++x)
            h.heights[h.index(x, z)] = float(std::clamp(h.samplePos(x, z).y, -15000.0, 15000.0) * 0.3);
    terrain::CostWeights w;
    const terrain::Route r = terrain::route(h, {0.0, -30000.0}, {0.0, 30000.0}, w, 10.0, 512);
    REQUIRE_FALSE(r.points.empty());
    CHECK(r.withinGrade);
    CHECK(r.maxGrade <= 10.5);
    CHECK(r.length > 60000.0 * 1.5);
    const terrain::Route steep = terrain::route(h, {0.0, -30000.0}, {0.0, 30000.0}, w, 0.0, 512);
    CHECK(steep.length < r.length);
}

TEST_CASE("terrain commands: stamps sit after erosion, follow their object live, undo, and survive save and reopen")
{
    loadTerrain();
    run("newScene");
    json t = run("terrainCreate", {{"size", {400000.0, 400000.0}}, {"resolution", 129}, {"amplitude", 20000.0}, {"seed", 4}, {"name", "Land"}});
    json pad = run("terrainStampPad", {{"args", {t["mesh"]}}, {"id", "temple"}, {"footprint", square(10000.0, 20000.0, 8000.0)}, {"falloff", 4000.0}});
    CHECK(pad["applied"] == true);
    CHECK(pad["cutM3"].get<double>() + pad["fillM3"].get<double>() > 0.0);
    const double level = pad["padHeight"].get<double>();
    CHECK(heightAt(t, 10000.0, 20000.0) == doctest::Approx(level).epsilon(1e-6));
    CHECK(heightAt(t, 16000.0, 26000.0) == doctest::Approx(level).epsilon(1e-6));

    // Erosion runs before the stamps, so turning it on leaves the pad level.
    run("terrainErode", {{"args", {t["mesh"]}}, {"iterations", 10}});
    json p2 = run("terrainStamps", {{"args", {t["mesh"]}}})["layers"]["Stamps"];
    REQUIRE(p2.size() == 1);
    const double eroded = p2[0]["padHeight"].get<double>();
    CHECK(heightAt(t, 10000.0, 20000.0) == doctest::Approx(eroded).epsilon(1e-6));
    CHECK(heightAt(t, 16000.0, 26000.0) == doctest::Approx(eroded).epsilon(1e-6));

    // A second stamp, and undo takes only it away.
    run("terrainStampRegion", {{"args", {t["mesh"]}}, {"id", "forum"}, {"footprint", square(-60000.0, -60000.0, 10000.0)}, {"height", 3000.0},
                               {"masks", {"cityFootprint", "paved"}}, {"layer", "City"}});
    CHECK(heightAt(t, -60000.0, -60000.0) == doctest::Approx(3000.0).epsilon(1e-6));
    run("undo");
    CHECK(heightAt(t, -60000.0, -60000.0) != doctest::Approx(3000.0).epsilon(1e-6));
    CHECK(heightAt(t, 10000.0, 20000.0) == doctest::Approx(eroded).epsilon(1e-6));
    run("redo");
    CHECK(heightAt(t, -60000.0, -60000.0) == doctest::Approx(3000.0).epsilon(1e-6));

    // A pad following a mesh moves with it.
    json plane = run("polyPlane", {{"width", 6000.0}, {"depth", 6000.0}, {"name", "Base"}});
    const std::string base = plane.is_array() ? plane[0].get<std::string>() : plane.get<std::string>();
    run("setAttr", {{"args", {base + ".translate", {80000.0, 0.0, -80000.0}}}});
    json live = run("terrainStampPad", {{"args", {t["mesh"]}}, {"id", "house"}, {"object", base}, {"falloff", 2000.0}, {"height", 5000.0}});
    CHECK(live["applied"] == true);
    CHECK(heightAt(t, 80000.0, -80000.0) == doctest::Approx(5000.0).epsilon(1e-6));
    run("setAttr", {{"args", {base + ".translate", {-90000.0, 0.0, 90000.0}}}});
    CHECK(heightAt(t, -90000.0, 90000.0) == doctest::Approx(5000.0).epsilon(1e-6));
    CHECK(heightAt(t, 80000.0, -80000.0) != doctest::Approx(5000.0).epsilon(1e-6));

    // A closed curve works as a footprint too.
    json ring = run("circle", {{"radius", 5000.0}, {"center", {-150000.0, 0.0, -150000.0}}, {"name", "Ring"}});
    json byCurve = run("terrainStampPad", {{"args", {t["mesh"]}}, {"id", "ring"}, {"object", ring}, {"height", 2500.0}, {"falloff", 0.0}});
    CHECK(byCurve["applied"] == true);
    CHECK(heightAt(t, -150000.0, -150000.0) == doctest::Approx(2500.0).epsilon(1e-6));
    CHECK(heightAt(t, -146000.0, -150000.0) == doctest::Approx(2500.0).epsilon(1e-6));

    // Sculpting before erosion and after it.
    run("terrainSculpt", {{"args", {t["mesh"]}}, {"op", "raise"}, {"points", {{-120000.0, 0.0}}}, {"radius", 30000.0}, {"strength", 4000.0}});
    json fin = run("terrainSculpt", {{"args", {t["mesh"]}}, {"op", "flatten"}, {"points", {{120000.0, 120000.0}}}, {"radius", 20000.0}, {"strength", 1.0},
                                     {"height", 777.0}, {"stage", "final"}});
    CHECK(fin["strokes"] == 1);
    CHECK(heightAt(t, 120000.0, 120000.0) == doctest::Approx(777.0).epsilon(1e-4));

    const std::vector<std::pair<double, double>> probes = {{10000.0, 20000.0}, {-60000.0, -60000.0}, {-90000.0, 90000.0}, {-120000.0, 0.0}, {120000.0, 120000.0}};
    std::vector<double> heights;
    for (auto [x, z] : probes)
        heights.push_back(heightAt(t, x, z));
    const fs::path file = fs::temp_directory_path() / "lila_terrain_stamps_test" / "Stamps.lila";
    fs::create_directories(file.parent_path());
    run("saveScene", {{"args", {file.generic_string()}}});
    run("newScene");
    run("openScene", {{"args", {file.generic_string()}}});
    for (size_t k = 0; k < probes.size(); ++k)
        CHECK(heightAt(t, probes[k].first, probes[k].second) == doctest::Approx(heights[k]).epsilon(1e-6));
    json stamps = run("terrainStamps", {{"args", {t["mesh"]}}});
    CHECK(stamps["layers"]["Stamps"].size() == 3);
    CHECK(stamps["layers"]["City"].size() == 1);
    CHECK_FALSE(stamps["layers"]["Stamps"][1].value("object", std::string()).empty());
    CHECK(heightAt(t, -150000.0, -150000.0) == doctest::Approx(2500.0).epsilon(1e-6));

    // The bake is stored relative to the scene, so the folder can move.
    json b = run("terrainBake", {{"args", {t["mesh"]}}});
    CHECK(fs::path(b["stored"].get<std::string>()).is_relative());
    run("saveScene", {{"args", {file.generic_string()}}});
    const fs::path moved = fs::temp_directory_path() / "lila_terrain_stamps_moved";
    std::error_code ec;
    fs::remove_all(moved, ec);
    run("newScene");
    fs::rename(file.parent_path(), moved);
    run("openScene", {{"args", {(moved / "Stamps.lila").generic_string()}}});
    CHECK(heightAt(t, 10000.0, 20000.0) == doctest::Approx(heights[0]).epsilon(1e-5));
    run("newScene");
    fs::remove_all(moved, ec);
}

TEST_CASE("terrain commands: rotated terrains sample, stamp and export in world space")
{
    loadTerrain();
    run("newScene");
    json t = run("terrainCreate", {{"size", {100000.0, 50000.0}}, {"resolution", 65}, {"flat", true}, {"name", "Turned"}});
    run("setAttr", {{"args", {t["mesh"].get<std::string>() + ".rotate", {0.0, 30.0, 0.0}}}});
    run("setAttr", {{"args", {t["mesh"].get<std::string>() + ".translate", {1000.0, 200.0, -3000.0}}}});
    json info = run("terrainInfo", {{"args", {t["mesh"]}}});
    CHECK(info["heightRange"][0].get<double>() == doctest::Approx(200.0));
    // A raise at a world point lands there, not where the unrotated grid would put it.
    run("terrainSculpt", {{"args", {t["mesh"]}}, {"op", "raise"}, {"points", {{20000.0, 5000.0}}}, {"radius", 5000.0}, {"strength", 1000.0}, {"stage", "final"}});
    CHECK(heightAt(t, 20000.0, 5000.0) == doctest::Approx(1200.0).epsilon(1e-3));
    CHECK(heightAt(t, 20000.0, -15000.0) == doctest::Approx(200.0).epsilon(1e-3));
    json pad = run("terrainStampPad", {{"args", {t["mesh"]}}, {"footprint", square(-20000.0, 5000.0, 3000.0)}, {"height", -500.0}, {"falloff", 0.0}});
    CHECK(pad["padHeight"].get<double>() == doctest::Approx(-500.0));
    CHECK(heightAt(t, -20000.0, 5000.0) == doctest::Approx(-500.0).epsilon(1e-6));

    const fs::path dir = fs::temp_directory_path() / "lila_terrain_rotated_export";
    json s = run("terrainExport", {{"args", {t["mesh"]}}, {"folder", dir.generic_string()}});
    // Lila's +Y rotation by 30 degrees turns the landscape's X (Lila +Z) toward Unreal -Y... a yaw of -30.
    CHECK(std::abs(std::abs(s["rotation"][1].get<double>()) - 30.0) < 1e-6);
    CHECK(s["rotation"][0].get<double>() == doctest::Approx(0.0));
    CHECK(s["rotation"][2].get<double>() == doctest::Approx(0.0));
    std::error_code ec;
    fs::remove_all(dir, ec);
}

TEST_CASE("terrain commands: on a Z-up scene the terrain lies on XY with heights up Z")
{
    loadTerrain();
    UpAxisGuard guard(math::UpAxis::Z);
    run("newScene");
    json t = run("terrainCreate", {{"size", {60000.0, 20000.0}}, {"resolution", 65}, {"flat", true}, {"center", {1000.0, 2000.0, 300.0}}, {"name", "ZUp"}});
    CHECK(t["upAxis"] == "z");
    CHECK(t["size"][0].get<double>() == doctest::Approx(20000.0).epsilon(0.05));
    CHECK(t["max"][0].get<double>() - t["min"][0].get<double>() == doctest::Approx(60000.0));
    CHECK(t["max"][1].get<double>() - t["min"][1].get<double>() == doctest::Approx(20000.0).epsilon(0.05));
    CHECK(t["center"][0].get<double>() == doctest::Approx(1000.0));
    CHECK(t["center"][1].get<double>() == doctest::Approx(2000.0));
    CHECK(t["heightRange"][0].get<double>() == doctest::Approx(300.0));
    json s = run("terrainSample", {{"args", {t["mesh"]}}, {"points", {{20000.0, 5000.0}}}});
    CHECK(s[0]["inside"] == true);
    CHECK(s[0]["y"].get<double>() == doctest::Approx(5000.0));
    CHECK(s[0]["position"][2].get<double>() == doctest::Approx(300.0));
    CHECK(s[0]["normal"][2].get<double>() == doctest::Approx(1.0));
    run("terrainStampPad", {{"args", {t["mesh"]}}, {"footprint", square(20000.0, 5000.0, 2000.0)}, {"height", 900.0}, {"falloff", 0.0}});
    CHECK(run("terrainSample", {{"args", {t["mesh"]}}, {"points", {{20000.0, 5000.0}}}})[0]["height"].get<double>() == doctest::Approx(900.0).epsilon(1e-6));
    const fs::path dir = fs::temp_directory_path() / "lila_terrain_zup_export";
    json e = run("terrainExport", {{"args", {t["mesh"]}}, {"folder", dir.generic_string()}});
    // Z-up maps straight onto Unreal's axes: no rotation needed.
    CHECK(e["rotation"][0].get<double>() == doctest::Approx(0.0));
    CHECK(e["rotation"][1].get<double>() == doctest::Approx(0.0));
    CHECK(e["rotation"][2].get<double>() == doctest::Approx(0.0));
    std::error_code ec;
    fs::remove_all(dir, ec);
    run("newScene");
}

TEST_CASE("unreal export: masks become paint layers with weightmaps in the landscape layout, named as asked")
{
    geo::Heightfield h = geo::makeHeightfield(127, 64, 100.0, {0.0, 0.0});
    std::vector<float>& rock = h.mask("rock");
    std::vector<float>& wet = h.mask("cityFootprint");
    for (int z = 0; z < h.nz; ++z)
        for (int x = 0; x < h.nx; ++x)
        {
            rock[h.index(x, z)] = x > 63 ? 1.0f : 0.0f;
            wet[h.index(x, z)] = 0.5f;
        }
    const fs::path dir = fs::temp_directory_path() / "lila_terrain_layers_test";
    terrain::UnrealOptions o;
    o.layerNames["cityFootprint"] = "Paved Town";
    json s = terrain::exportUnreal(h, glm::dmat4(1.0), dir, "T", o);
    REQUIRE(s["layers"].size() == 2);
    const int w = s["resolution"][0].get<int>(), rows = s["resolution"][1].get<int>();
    for (const json& l : s["layers"])
    {
        CHECK(fs::file_size(l["file"].get<std::string>()) == size_t(w) * size_t(rows));
        if (l["mask"] == "cityFootprint")
            CHECK(l["layer"] == "Paved_Town");
    }
    // Unreal rows run along Lila -X: the first row is Lila's last column, all rock.
    const json rockLayer = s["layers"][0]["mask"] == "rock" ? s["layers"][0] : s["layers"][1];
    std::ifstream f(rockLayer["file"].get<std::string>(), std::ios::binary);
    std::vector<uint8_t> bytes(size_t(w) * size_t(rows));
    f.read(reinterpret_cast<char*>(bytes.data()), std::streamsize(bytes.size()));
    CHECK(bytes[0] == 255);
    CHECK(bytes[size_t(rows - 1) * size_t(w)] == 0);

    terrain::LandscapeSend send;
    send.id = "|Land";
    send.label = "Land";
    json p = terrain::landscapeParams(s, send);
    CHECK(p["makeMaterial"] == true);
    CHECK(p["layers"].size() == 2);
    CHECK(p["layers"][0].contains("weightmap"));
    CHECK(p["rotation"].size() == 3);
    send.material = "/Game/M_Mine";
    CHECK(terrain::landscapeParams(s, send)["makeMaterial"] == false);

    o.masks = {"rock"};
    CHECK(terrain::exportUnreal(h, glm::dmat4(1.0), dir, "T", o)["layers"].size() == 1);
    std::error_code ec;
    fs::remove_all(dir, ec);
}

namespace
{
bool startGpu()
{
    auto& r = render::Renderer::get();
    if (r.running() || r.start())
        return true;
    MESSAGE("no GPU for terrain GPU tests");
    return false;
}

geo::Heightfield ranges(int n)
{
    terrain::NoiseParams np;
    np.featureSize = 60000.0;
    np.amplitude = 20000.0;
    np.seed = 7;
    geo::Heightfield h = geo::makeHeightfieldCovering({0.0, 0.0}, {200000.0, 200000.0}, n);
    for (int z = 0; z < h.nz; ++z)
        for (int x = 0; x < h.nx; ++x)
        {
            const glm::dvec2 p = h.samplePos(x, z);
            h.heights[h.index(x, z)] = float(terrain::noiseHeight(np, p.x, p.y));
        }
    return h;
}
} // namespace

TEST_CASE("gpu erosion: matches the CPU within tolerance, masks included")
{
    if (!startGpu())
        return;
    terrain::ErosionParams p;
    p.iterations = 20;
    p.uplift = 5.0;
    geo::Heightfield cpu = ranges(257), gpu = cpu;
    terrain::erode(cpu, p);
    std::string err;
    REQUIRE_MESSAGE(terrain::erodeGpu(gpu, p, &err), err);
    double worst = 0.0, sum = 0.0;
    const glm::dvec2 r = cpu.range();
    for (size_t k = 0; k < cpu.size(); ++k)
    {
        const double d = std::abs(double(cpu.heights[k]) - double(gpu.heights[k]));
        worst = std::max(worst, d);
        sum += d;
    }
    MESSAGE("gpu vs cpu: worst " << worst << " cm, mean " << sum / double(cpu.size()) << " cm over a " << (r.y - r.x) << " cm range");
    CHECK(sum / double(cpu.size()) < (r.y - r.x) * 1e-3);
    CHECK(worst < (r.y - r.x) * 0.02);
    for (const char* m : {"flow", "wear", "deposit", "lakes"})
        CHECK(gpu.findMask(m) != nullptr);
    // Drainage on near-flat ground hangs on rounding, so the rivers can take other cells; the GPU's masks must still be
    // exactly the drainage of its own heights.
    geo::Heightfield again = gpu;
    terrain::writeFlowMasks(again, terrain::drain(again));
    CHECK(again.mask("flow") == gpu.mask("flow"));
    CHECK(again.mask("lakes") == gpu.mask("lakes"));
}

TEST_CASE("gpu erosion: drainage alone is identical to the CPU's")
{
    if (!startGpu())
        return;
    terrain::ErosionParams p;
    p.iterations = 0;
    geo::Heightfield cpu = ranges(257), gpu = cpu;
    terrain::writeFlowMasks(cpu, terrain::drain(cpu));
    std::string err;
    REQUIRE_MESSAGE(terrain::erodeGpu(gpu, p, &err), err);
    CHECK(gpu.mask("flow") == cpu.mask("flow"));
    CHECK(gpu.mask("lakes") == cpu.mask("lakes"));
}

TEST_CASE("gpu erosion: timing at 4097" * doctest::skip())
{
    if (!startGpu())
        return;
    terrain::ErosionParams p;
    p.iterations = 60;
    geo::Heightfield gpu = ranges(4097);
    const auto t0 = std::chrono::steady_clock::now();
    std::string err;
    REQUIRE_MESSAGE(terrain::erodeGpu(gpu, p, &err), err);
    const double gpuS = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
    MESSAGE("4097x4097, 60 iterations: GPU " << gpuS << " s wall, " << terrain::lastGpuErosionMs() / 1000.0 << " s on the GPU");
    geo::Heightfield cpu = ranges(4097);
    p.iterations = 2;
    const auto t1 = std::chrono::steady_clock::now();
    terrain::erode(cpu, p);
    const double cpuS = std::chrono::duration<double>(std::chrono::steady_clock::now() - t1).count();
    MESSAGE("4097x4097: CPU " << cpuS / 2.0 << " s per iteration (2 run), about " << cpuS / 2.0 * 60.0 << " s for 60");
}

TEST_CASE("stamps: a pad held below the ground round it widens its skirt to keep to its skirt grade")
{
    for (const double grade : {0.0, 0.08})
    {
        // Level ground 6 m above where the pad is held.
        geo::Heightfield h = geo::makeHeightfieldCovering({0.0, 0.0}, {400000.0, 400000.0}, 1025);
        std::fill(h.heights.begin(), h.heights.end(), 1000.0f);
        terrain::Stamp s;
        s.id = "yard";
        s.level = terrain::StampLevel::Value;
        s.height = 400.0;
        s.falloff = 3000.0;
        s.skirtGrade = grade;
        s.polygon = rect({20000.0, -10000.0}, {6000.0, 4000.0});
        const terrain::StampResult r = terrain::applyStamp(h, s, terrain::footprintFromPolygon(s.polygon));
        double steepest = 0.0;
        for (double x = 26000.0; x < 26000.0 + 4.0 * s.falloff; x += h.cell)
            steepest = std::max(steepest, std::abs(h.heightAt(x + h.cell, -10000.0) - h.heightAt(x, -10000.0)) / h.cell);
        CAPTURE(grade);
        CAPTURE(steepest);
        if (grade > 0.0)
        {
            CHECK(r.falloff > s.falloff);
            CHECK(steepest <= grade + 0.01);
        }
        else
            CHECK(steepest > 0.2);
    }
}
