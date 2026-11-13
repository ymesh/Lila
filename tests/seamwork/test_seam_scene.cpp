#include "TestUtil.h"
#include "extensions/ExtensionManager.h"
#include "geo/ColorSets.h"
#include "ops/Command.h"
#include "scene/Graph.h"
#include "scene/MeshNodes.h"
#include "scene/Transform.h"
#include "scene/Overlap.h"
#include "geo/Heightfield.h"

#include <doctest/doctest.h>

#include <array>
#include <cmath>
#include <filesystem>

using namespace lila;
using namespace testutil;
using json = nlohmann::json;

namespace
{
void load()
{
    if (!ops::registry().find("terrainCreate"))
        REQUIRE(extensions::ExtensionManager::get().load(LILA_TEST_TERRAIN_PACKAGE) == "Terrain");
    if (!ops::registry().find("settle"))
        REQUIRE(extensions::ExtensionManager::get().load(LILA_TEST_STAGEHAND_PACKAGE) == "Stagehand");
    if (!ops::registry().find("seamFind"))
        REQUIRE(extensions::ExtensionManager::get().load(LILA_TEST_SEAMWORK_PACKAGE) == "Seamwork");
}

/** A closed box from lo to hi, faces subdivided n times along each side so fields have points to land on. */
std::string box(const char* name, glm::dvec3 lo, glm::dvec3 hi, int n = 1)
{
    json points = json::array(), faces = json::array();
    std::map<std::array<int, 3>, int> ids;
    auto id = [&](int i, int j, int k) {
        auto [it, fresh] = ids.try_emplace({i, j, k}, int(points.size()));
        if (fresh)
            points.push_back({lo.x + (hi.x - lo.x) * i / n, lo.y + (hi.y - lo.y) * j / n, lo.z + (hi.z - lo.z) * k / n});
        return it->second;
    };
    for (int a = 0; a < n; ++a)
        for (int b = 0; b < n; ++b)
        {
            faces.push_back({id(0, a, b), id(0, a, b + 1), id(0, a + 1, b + 1), id(0, a + 1, b)});
            faces.push_back({id(n, a, b), id(n, a + 1, b), id(n, a + 1, b + 1), id(n, a, b + 1)});
            faces.push_back({id(a, 0, b), id(a + 1, 0, b), id(a + 1, 0, b + 1), id(a, 0, b + 1)});
            faces.push_back({id(a, n, b), id(a, n, b + 1), id(a + 1, n, b + 1), id(a + 1, n, b)});
            faces.push_back({id(a, b, 0), id(a, b + 1, 0), id(a + 1, b + 1, 0), id(a + 1, b, 0)});
            faces.push_back({id(a, b, n), id(a + 1, b, n), id(a + 1, b + 1, n), id(a, b + 1, n)});
        }
    return run("createMesh", {{"points", points}, {"faces", faces}, {"name", name}}).get<std::string>();
}

void moveTo(const std::string& node, const glm::dvec3& at)
{
    run("xform", {{"args", {node}}, {"worldSpace", true}, {"translation", json::array({at.x, at.y, at.z})}});
}

const geo::Mesh& evaluatedMesh(const std::string& node)
{
    g().evaluate();
    return *g().find(node)->as<scene::MeshPtr>(scene::MeshNode::kOutMesh);
}
} // namespace

TEST_CASE("seamFind: a wall on a terrain gives a live join, a field and a terrain mask")
{
    load();
    run("newScene");
    const json t = run("terrainCreate", {{"flat", true}, {"size", json::array({4000, 4000})}, {"resolution", 81}, {"name", "Ground"}});
    const std::string terrain = t["mesh"].get<std::string>();
    const std::string wall = box("Wall", {-200, -20, -10}, {200, 280, 10}, 4);
    const json r = run("seamFind", {{"args", {wall}}, {"terrain", terrain}});
    CHECK(r["curves"].get<int>() == 1);
    CHECK(r["length"].get<double>() == doctest::Approx(840.0).epsilon(0.001));
    CHECK(r["terrain"] == "heightfield");
    REQUIRE(r["pairs"].size() == 1);
    CHECK(r["pairs"][0]["a"] == wall);
    CHECK(r["pairs"][0]["b"] == terrain);
    CHECK(r["fieldModifiers"].size() == 1);
    CHECK_FALSE(r["terrainMaskNode"].is_null());

    // The field: red is 1 at the footprint and 0 past the band.
    const geo::Mesh& m = evaluatedMesh(wall);
    const auto sets = geo::colors::sets(m);
    REQUIRE_FALSE(sets.empty());
    const auto colors = geo::colors::values(m, sets.front());
    float lo = 1.0f, hi = 0.0f;
    for (geo::Index c = 0; c < m.cornerCapacity(); ++c)
        if (m.cornerValid(c))
        {
            lo = std::min(lo, colors[c].r);
            hi = std::max(hi, colors[c].r);
        }
    CHECK(lo == 0.0f);
    CHECK(hi > 0.0f);

    const json info = run("terrainInfo", {{"args", {terrain}}});
    CHECK(info["masks"].contains("contact"));
    // The mask goes to Unreal as its own paint layer.
    const std::string folder = (std::filesystem::temp_directory_path() / "lila_seamwork_contact_layer").string();
    const json exported = run("terrainExport", {{"args", {terrain}}, {"target", "unreal"}, {"folder", folder}});
    bool contactLayer = false;
    for (const json& l : exported["layers"])
        contactLayer = contactLayer || (l["mask"] == "contact" && l["layer"] == "contact");
    CHECK(contactLayer);

    const json si = run("seamInfo", {{"args", {r["seam"]}}, {"points", true}});
    REQUIRE(si["curveList"].size() == 1);
    CHECK(si["curveList"][0]["kind"] == "ground");
    CHECK(si["curveList"][0]["angle"][0].get<double>() == doctest::Approx(90.0).epsilon(0.01));

    // Live: move the wall and the join follows.
    moveTo(wall, {0, 100, 0});
    g().evaluate();
    run("seamUpdate");
    const json moved = run("seamInfo", {{"args", {r["seam"]}}});
    CHECK(moved["curves"].get<int>() == 0);
    moveTo(wall, {0, -100, 0});
    const json back = run("seamInfo", {{"args", {r["seam"]}}});
    CHECK(back["length"].get<double>() == doctest::Approx(840.0).epsilon(0.001));
}

TEST_CASE("seamFind: a terrain past the stepped-preview size is read as its full heightfield")
{
    load();
    run("newScene");
    const json t = run("terrainCreate", {{"flat", true}, {"size", json::array({4000, 4000})}, {"resolution", 2101}, {"name", "Big"}});
    const std::string terrain = t["mesh"].get<std::string>();
    REQUIRE(evaluatedMesh(terrain).numPoints() < size_t(2101) * 2101);
    const std::string wall = box("BigWall", {-200, -20, -10}, {200, 280, 10}, 4);
    const json r = run("seamFind", {{"args", {wall}}, {"terrain", terrain}});
    CHECK(r["terrain"] == "heightfield");
    CHECK(r["length"].get<double>() == doctest::Approx(840.0).epsilon(0.001));
}

TEST_CASE("terrainSpec: a seams key finds and covers contacts once the terrain is built")
{
    load();
    run("newScene");
    const std::string wall = box("SpecWall", {-200, -20, -10}, {200, 280, 10}, 4);
    const json r = run("terrainSpec", {{"spec",
                                        {{"name", "SpecGround"},
                                         {"size", json::array({4000, 4000})},
                                         {"resolution", 81},
                                         {"source", {{"type", "flat"}}},
                                         {"check", false},
                                         {"seams", {{"objects", json::array({wall})}, {"skirt", {{"width", 30}}}, {"decal", true}}}}}});
    INFO(r.dump().substr(0, 3000));
    CHECK(r["ok"] == true);
    json step;
    for (const json& s : r["steps"])
        if (s["step"] == "seams")
            step = s;
    REQUIRE_FALSE(step.is_null());
    CHECK(step["ok"] == true);
    CHECK(step["result"]["curves"].get<int>() == 1);
    CHECK(step["result"].contains("skirt"));
    CHECK(step["result"].contains("decal"));

    // An unknown key is still refused.
    CHECK_THROWS(ops::execute("terrainSpec", {{"spec", {{"name", "SpecGround2"}, {"seamz", true}}}}));
}

TEST_CASE("overlap: Stagehand measures how deep a box sits in a heightfield")
{
    load();
    const geo::Heightfield ground = geo::makeHeightfield(41, 41, 50.0, {-1000.0, -1000.0}, 0.0f);
    std::string label;
    std::unique_ptr<overlap::Session> s = overlap::open(ground, &label);
    REQUIRE(s);
    CHECK(label == "stagehand");
    auto cube = [](double bottom) {
        std::vector<glm::dvec3> p;
        for (int k = 0; k < 8; ++k)
            p.push_back({(k & 1) ? 50.0 : -50.0, bottom + ((k & 2) ? 100.0 : 0.0), (k & 4) ? 50.0 : -50.0});
        return p;
    };
    const glm::dvec3 up(0.0, 1.0, 0.0);
    CHECK(s->depth(cube(-30.0), up) == doctest::Approx(30.0).epsilon(0.05));
    CHECK(s->depth(cube(10.0), up) == doctest::Approx(0.0));
    CHECK(s->depth(cube(-60.0), up) == doctest::Approx(60.0).epsilon(0.05));
    CHECK(s->depth(cube(-150.0), up) == doctest::Approx(150.0).epsilon(0.05));
    // Sideways out of a step: a box pushed 40 cm into a 500 cm wall leaves it after 40 cm.
    geo::Heightfield step = geo::makeHeightfield(81, 41, 25.0, {-1000.0, -500.0}, 0.0f);
    for (int z = 0; z < step.nz; ++z)
        for (int x = 0; x < step.nx; ++x)
            if (step.samplePos(x, z).x >= 0.0)
                step.heights[step.index(x, z)] = 500.0f;
    std::unique_ptr<overlap::Session> wall = overlap::open(step);
    REQUIRE(wall);
    std::vector<glm::dvec3> pushed;
    for (int k = 0; k < 8; ++k)
        pushed.push_back({(k & 1) ? 40.0 : -60.0, 10.0 + ((k & 2) ? 100.0 : 0.0), (k & 4) ? 50.0 : -50.0});
    CHECK(wall->depth(pushed, glm::dvec3(-1.0, 0.0, 0.0)) == doctest::Approx(64.5).epsilon(0.05));
    // Another rise further out doesn't count: the first clear pose is the depth.
    for (int z = 0; z < step.nz; ++z)
        for (int x = 0; x < step.nx; ++x)
            if (step.samplePos(x, z).x <= -400.0)
                step.heights[step.index(x, z)] = 500.0f;
    std::unique_ptr<overlap::Session> gully = overlap::open(step);
    CHECK(gully->depth(pushed, glm::dvec3(-1.0, 0.0, 0.0)) == doctest::Approx(64.5).epsilon(0.05));
}

TEST_CASE("terrainCliffKit: with Stagehand's physics, full-size pieces dress a noise-terrain cliff")
{
    load();
    run("newScene");
    const json t = run("terrainCreate", {{"size", {20000.0, 20000.0}}, {"resolution", 129}, {"amplitude", 6000.0}, {"featureSize", 6000.0}, {"seed", 3}, {"name", "Hills"}});
    run("terrainCliffs", {{"args", {t["mesh"]}}, {"slope", 40.0}, {"minHeight", 300.0}, {"minArea", 10.0}});
    const json big = run("polyCube", {{"name", "RockBig"}, {"width", 600.0}, {"height", 1000.0}, {"depth", 400.0}});
    const json small = run("polyCube", {{"name", "RockSmall"}, {"width", 250.0}, {"height", 300.0}, {"depth", 200.0}});
    const json k = run("terrainCliffKit", {{"args", {t["mesh"]}}, {"region", "cliff_1"}, {"kit", {big[0], small[0]}}});
    INFO(k.dump());
    CHECK(k["penetrationCheck"] == "stagehand");
    CHECK(k["placed"].get<int>() - k["fillers"].get<int>() >= 20);
    CHECK(k["coverage"].get<double>() > 0.95);
    CHECK(k["rejectedPenetrating"].get<int>() > 0);
}

TEST_CASE("seamFind: tags leave objects out and seamCheck reports floating and open joins")
{
    load();
    run("newScene");
    const std::string a = box("BlockA", {0, 0, 0}, {100, 100, 100});
    const std::string b = box("BlockB", {50, 25, 25}, {150, 75, 75});
    const std::string floater = box("Floater", {500, 30, 0}, {550, 80, 50});
    const std::string floor = box("Floor", {-1000, -20, -1000}, {1000, 0, 1000});
    run("seamTag", {{"args", {floor}}, {"ground", true}});
    json r = run("seamFind", {{"args", {a, b, floater, floor}}, {"field", "none"}, {"terrainMask", false}});
    // A-B crossing, A on the floor (near contact at 0 gap is a touch, not counted), B above it.
    bool meshPair = false;
    for (const json& p : r["pairs"])
        meshPair |= p["kind"] == "mesh" && p["a"] == a && p["b"] == b;
    CHECK(meshPair);
    const json check = run("seamCheck", {{"args", {r["seam"]}}});
    bool floating = false, open = false;
    for (const json& i : check["issues"])
    {
        floating |= i["kind"] == "floating" && i["node"] == floater && std::abs(i["gap"].get<double>() - 30.0) < 0.01;
        open |= i["kind"] == "open";
    }
    CHECK(floating);
    CHECK(open);

    run("seamTag", {{"args", {b}}, {"ignore", true}});
    r = run("seamFind", {{"args", {a, b}}, {"field", "none"}, {"name", "second"}});
    CHECK(r["ignored"].size() == 1);
    CHECK(r["curves"].get<int>() == 0);
}

TEST_CASE("seamScatter: rubble along a wall settles clean")
{
    load();
    run("newScene");
    const json t = run("terrainCreate", {{"flat", true}, {"size", json::array({4000, 4000})}, {"resolution", 81}, {"name", "Ground"}});
    const std::string wall = box("Wall", {-200, -20, -10}, {200, 280, 10});
    const json r = run("seamFind", {{"args", {wall}}, {"terrain", t["mesh"]}, {"field", "none"}});
    const json s = run("seamScatter", {{"args", {r["seam"]}}, {"preset", "rubble"}, {"density", 1.0}, {"clump", 2}, {"settle", true}});
    CHECK(s["instances"].get<int>() > 10);
    CHECK(s["farthestFromContact"].get<double>() < 40.0 + 15.0 + 10.0);
    CHECK(s["dressCheck"]["ok"].get<bool>());
    // The scatter covers the contact now.
    const json c = run("seamCheck", {{"args", {r["seam"]}}});
    CHECK(c["ok"].get<bool>());
    // Rules on it: nothing left inside a box over half the wall.
    const std::string zone = box("Zone", {-300, -100, -200}, {0, 100, 200});
    const json p = run("scatterProximity", {{"args", {s["group"]}}, {"rules", {{{"kind", "cullInside"}, {"target", zone}}}}});
    CHECK(p["removed"].size() > 0);
    CHECK(p["kept"].get<int>() > 0);
}

TEST_CASE("seamScatter: a library asset becomes one hidden prototype and leaves no placement behind")
{
    load();
    run("newScene");
    const json t = run("terrainCreate", {{"flat", true}, {"size", json::array({4000, 4000})}, {"resolution", 81}, {"name", "Ground"}});
    const std::string wall = box("Wall", {-200, -20, -10}, {200, 280, 10});
    const json r = run("seamFind", {{"args", {wall}}, {"terrain", t["mesh"]}, {"field", "none"}});
    const json s = run("seamScatter", {{"args", {r["seam"]}}, {"items", json::array({"Industrial/Crate"})}, {"density", 0.5}});
    INFO(s.dump());
    CHECK(s["instances"].get<int>() > 0);
    REQUIRE(s["assets"].size() == 1);
    CHECK(s["assets"][0] == "Industrial/Crate");
    g().evaluate();
    int visibleCrates = 0;
    std::string visible;
    for (const json& n : run("ls", {{"type", "mesh"}}))
    {
        scene::Node* m = g().find(n.get<std::string>());
        if (m && m->name().find("Crate") != std::string::npos && m->as<bool>(m->type().attrIndex("visibility")) &&
            m->path().find(s["group"].get<std::string>()) == std::string::npos &&
            m->fullPath().rfind("|StagehandLibrary", 0) != 0)
            visible += m->fullPath() + " ", ++visibleCrates;
    }
    INFO(visible);
    CHECK(visibleCrates == 0);
}

TEST_CASE("contacts feed terrain layer rules and export to JSON and USD")
{
    load();
    run("newScene");
    const json t = run("terrainCreate", {{"flat", true}, {"size", json::array({4000, 4000})}, {"resolution", 81}, {"name", "Ground"}});
    const std::string terrain = t["mesh"].get<std::string>();
    const std::string wall = box("Wall", {-200, -20, -10}, {200, 280, 10});
    const json r = run("seamFind", {{"args", {wall}}, {"terrain", terrain}, {"terrainWidth", 150.0}});
    const json rules = run("terrainMaterialRules",
                           {{"args", {terrain}}, {"layers", {{{"name", "grass"}}, {{"name", "dirt"}, {"when", {{"contact", json::array({0.3, 1.0})}}}}}}});
    bool dirt = false;
    for (const json& l : rules.value("layers", json::array()))
        if (l.value("name", "") == "dirt")
            dirt = l.value("coverage", 0.0) > 0.0;
    CHECK(dirt);
    CHECK(rules.value("missing", json::array()).empty());

    const auto dir = std::filesystem::temp_directory_path();
    const std::string jsonFile = (dir / "seamwork_contacts.json").string(), usdFile = (dir / "seamwork_contacts.usda").string();
    run("seamExport", {{"args", {r["seam"]}}, {"file", jsonFile}});
    run("seamExport", {{"args", {r["seam"]}}, {"file", usdFile}});
    std::ifstream jf(jsonFile);
    const json j = json::parse(jf);
    CHECK(j["format"] == "lila-contacts");
    REQUIRE(j["curves"].size() == 1);
    CHECK(j["curves"][0]["b"] == terrain);
    CHECK(j["points"].size() == r["points"].get<size_t>());
    std::ifstream uf(usdFile);
    const std::string usd((std::istreambuf_iterator<char>(uf)), std::istreambuf_iterator<char>());
    CHECK(usd.find("def BasisCurves \"curve_0\"") != std::string::npos);
    CHECK(usd.find("primvars:exposure") != std::string::npos);
}

TEST_CASE("seamDecal: a live strip mesh that follows the wall and covers the contact")
{
    load();
    run("newScene");
    const json t = run("terrainCreate", {{"flat", true}, {"size", json::array({4000, 4000})}, {"resolution", 81}, {"name", "Ground"}});
    const std::string wall = box("Wall", {-200, -20, -10}, {200, 280, 10});
    const json r = run("seamFind", {{"args", {wall}}, {"terrain", t["mesh"]}, {"field", "none"}});
    const json s = run("seamDecal", {{"args", {r["seam"]}}, {"side", "both"}});
    CHECK(s["stats"]["strips"].get<int>() == 1);
    CHECK(s["warning"] == "");
    const size_t before = evaluatedMesh(s["mesh"]).numFaces();
    CHECK(before > 0);
    CHECK(run("seamCheck", {{"args", {r["seam"]}}})["ok"].get<bool>());
    moveTo(wall, {300, 0, 0});
    double maxX = -1e9;
    const geo::Mesh& m = evaluatedMesh(s["mesh"]);
    for (geo::Index p = 0; p < m.pointCapacity(); ++p)
        if (m.pointValid(p))
            maxX = std::max(maxX, m.position(p).x);
    CHECK(maxX > 400.0);
}

TEST_CASE("seamSkirt: one live skirt per object, mapped for Unreal, following a move")
{
    load();
    run("newScene");
    const json t = run("terrainCreate", {{"flat", true}, {"size", json::array({4000, 4000})}, {"resolution", 81}, {"name", "Ground"}});
    const std::string wall = box("Wall", {-200, -20, -10}, {200, 280, 10});
    const json r = run("seamFind", {{"args", {wall}}, {"terrain", t["mesh"]}, {"field", "none"}});
    const json s = run("seamSkirt", {{"args", {r["seam"]}}, {"width", 20.0}});
    REQUIRE(s["skirts"].size() == 1);
    const json& one = s["skirts"][0];
    CHECK(one["object"] == wall);
    CHECK(one["stats"]["faces"].get<int>() > 0);
    CHECK(one["mapping"]["lodGroup"] == "SmallProp");
    double before = -1e9;
    for (geo::Index p = 0; p < evaluatedMesh(one["mesh"]).pointCapacity(); ++p)
        before = std::max(before, evaluatedMesh(one["mesh"]).position(p).x);
    moveTo(wall, {500, 0, 0});
    double after = -1e9;
    const geo::Mesh& m = evaluatedMesh(one["mesh"]);
    for (geo::Index p = 0; p < m.pointCapacity(); ++p)
        if (m.pointValid(p))
            after = std::max(after, m.position(p).x);
    CHECK(after == doctest::Approx(before + 500.0).epsilon(0.001));
    CHECK(run("seamCheck", {{"args", {r["seam"]}}})["ok"].get<bool>());
}

TEST_CASE("Seam Points and Distribute Along Seam: instances along the contact points in a geometry-nodes group")
{
    load();
    run("newScene");
    const json t = run("terrainCreate", {{"flat", true}, {"size", json::array({4000, 4000})}, {"resolution", 81}, {"name", "Ground"}});
    const std::string wall = box("Wall", {-200, -20, -10}, {200, 280, 10});
    const json r = run("seamFind", {{"args", {wall}}, {"terrain", t["mesh"]}, {"field", "none"}, {"spacing", 20.0}});
    const std::string user = run("polyCube", {{"width", 10.0}, {"height", 10.0}, {"depth", 10.0}, {"name", "Scatter"}})[0];
    run("gnCreateGroup", {{"args", {"Along"}}});
    const std::string oi = run("gnAddNode", {{"args", {"Along", "ObjectInfo"}}});
    const std::string sp = run("gnAddNode", {{"args", {"Along", "SeamPoints"}}});
    const std::string da = run("gnAddNode", {{"args", {"Along", "DistributeAlongSeam"}}});
    const std::string cube = run("gnAddNode", {{"args", {"Along", "MeshCube"}}});
    const std::string real = run("gnAddNode", {{"args", {"Along", "RealizeInstances"}}});
    run("gnSetValue", {{"args", {"Along", oi + ".object", r["pointsNode"]}}});
    run("gnSetValue", {{"args", {"Along", oi + ".transformSpace", "world"}}});
    run("gnSetValue", {{"args", {"Along", cube + ".size", {4, 4, 4}}}});
    run("gnSetValue", {{"args", {"Along", sp + ".density", 0.5}}});
    run("gnDisconnect", {{"args", {"Along", "GroupOutput.geometry"}}});
    run("gnConnect", {{"args", {"Along", oi + ".geometry", sp + ".points"}}});
    run("gnConnect", {{"args", {"Along", sp + ".points", da + ".points"}}});
    run("gnConnect", {{"args", {"Along", cube + ".mesh", da + ".instance"}}});
    run("gnConnect", {{"args", {"Along", da + ".instances", real + ".geometry"}}});
    run("gnConnect", {{"args", {"Along", real + ".geometry", "GroupOutput.geometry"}}});
    run("gnApply", {{"args", {user, "Along"}}});
    const int cubes = faces(user) / 6;
    // 42 points along 840 cm at 20 cm, about half kept.
    CHECK(cubes > 10);
    CHECK(cubes < 35);
    const geo::Mesh& m = evaluatedMesh(user);
    for (geo::Index p = 0; p < m.pointCapacity(); ++p)
        if (m.pointValid(p))
            CHECK(std::abs(m.position(p).y) < 10.0);
}

TEST_CASE("relocateScene: a seam, its decal strip and fields move with the scene")
{
    load();
    run("newScene");
    const json t = run("terrainCreate", {{"flat", true}, {"size", json::array({4000, 4000})}, {"resolution", 81}, {"name", "Ground"}});
    const std::string terrain = t["mesh"].get<std::string>();
    const std::string wall = box("Wall", {-200, -20, -10}, {200, 280, 10}, 4);
    const json r = run("seamFind", {{"args", {wall}}, {"terrain", terrain}});
    const json strip = run("seamDecal", {{"args", {r["seam"]}}});
    const std::string mesh = strip["mesh"].get<std::string>();
    auto bounds = [&](const std::string& name) {
        const geo::Mesh& m = evaluatedMesh(name);
        const glm::dmat4 w = g().find(name)->as<glm::dmat4>(scene::Transform::kWorldMatrix);
        glm::dvec3 lo(1e300), hi(-1e300);
        for (geo::Index p = 0; p < m.pointCapacity(); ++p)
            if (m.pointValid(p))
            {
                const glm::dvec3 q(w * glm::dvec4(m.position(p), 1.0));
                lo = glm::min(lo, q);
                hi = glm::max(hi, q);
            }
        return std::pair{lo, hi};
    };
    const auto before = bounds(mesh);
    const double length = run("seamInfo", {{"args", {r["seam"]}}})["length"].get<double>();

    json tops = json::array();
    for (scene::NodeId id : g().nodes())
        if (const scene::Node* n = g().node(id); n && n->type().isA("transform") && g().parent(id) == scene::kNoNode)
            tops.push_back(n->path());
    const glm::dvec3 offset(500000.0, 0.0, 300000.0);
    run("relocateScene", {{"offset", json::array({offset.x, offset.y, offset.z})}, {"nodes", tops}});

    // The strip follows once, not twice: built in world space from the moved wall and terrain.
    const auto after = bounds(mesh);
    CHECK(glm::length(after.first - (before.first + offset)) < 1e-3);
    CHECK(glm::length(after.second - (before.second + offset)) < 1e-3);
    CHECK(run("seamInfo", {{"args", {r["seam"]}}})["length"].get<double>() == doctest::Approx(length).epsilon(1e-6));
}
