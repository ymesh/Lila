// Stagehand at level scale: terrain colliders, convex parts, reach, settle caps, instances to objects, the starter
// library and what goes to Unreal.

#include "TestUtil.h"
#include "extensions/ExtensionManager.h"
#include "ops/Command.h"
#include "scene/Graph.h"

#include <doctest/doctest.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <random>
#include <string>
#include <vector>

using namespace lila;
using namespace stagehandtest;
using json = nlohmann::json;

namespace
{
void load()
{
    if (!ops::registry().find("settle"))
        REQUIRE(extensions::ExtensionManager::get().load(LILA_TEST_STAGEHAND_PACKAGE) == "Stagehand");
}

void loadTerrain()
{
    load();
    if (!ops::registry().find("terrainCreate"))
        REQUIRE(extensions::ExtensionManager::get().load(LILA_TEST_TERRAIN_PACKAGE) == "Terrain");
}

glm::dvec3 worldPos(const std::string& node)
{
    const json t = run("xform", {{"args", {node}}, {"query", true}, {"worldSpace", true}, {"translation", true}});
    return {t[0].get<double>(), t[1].get<double>(), t[2].get<double>()};
}

void moveTo(const std::string& node, const glm::dvec3& at)
{
    run("xform", {{"args", {node}}, {"worldSpace", true}, {"translation", json::array({at.x, at.y, at.z})}});
}

/** An arch 200 wide, 240 tall and 60 deep from three closed boxes: two legs 40 wide and a lintel, its opening 120 wide and 200 high. */
std::string arch(const char* name)
{
    json points = json::array(), faces = json::array();
    auto box = [&](glm::dvec3 lo, glm::dvec3 hi) {
        const int base = int(points.size());
        for (int k = 0; k < 8; ++k)
            points.push_back({k & 1 ? hi.x : lo.x, k & 2 ? hi.y : lo.y, k & 4 ? hi.z : lo.z});
        for (auto q : {std::array{0, 2, 3, 1}, std::array{4, 5, 7, 6}, std::array{0, 1, 5, 4}, std::array{2, 6, 7, 3}, std::array{0, 4, 6, 2},
                       std::array{1, 3, 7, 5}})
            faces.push_back({base + q[0], base + q[1], base + q[2], base + q[3]});
    };
    box({-100, 0, -30}, {-60, 200, 30});
    box({60, 0, -30}, {100, 200, 30});
    box({-100, 200, -30}, {100, 240, 30});
    return run("createMesh", {{"points", points}, {"faces", faces}, {"name", name}}).get<std::string>();
}
} // namespace

TEST_CASE("stagehand: an arch collides as convex parts and lands around a post instead of on it")
{
    load();
    run("newScene");
    const std::string post = cube("Post", {0, 0, 0}, {40, 150, 40});
    const std::string a = arch("Arch");
    moveTo(a, {0, 40, 0});
    run("physicsBody", {{"args", {a}}, {"material", "stone"}, {"role", "dynamic"}});
    const json info = run("dressInfo", {{"args", {a}}});
    CHECK(info[0]["shape"] == "compound");
    CHECK(info[0]["parts"].get<int64_t>() >= 3);
    const json r = run("settle", {{"args", {a}}});
    CHECK(r["atRest"].get<bool>());
    CHECK(worldPos(a).y < 5.0);
    CHECK(worldPos(post).y == doctest::Approx(0.0));

    // As one hull it would sit on the post.
    run("undo");
    run("physicsBody", {{"args", {a}}, {"shape", "hull"}});
    moveTo(a, {0, 160, 0});
    run("settle", {{"args", {a}}});
    CHECK(worldPos(a).y > 140.0);
}

TEST_CASE("stagehand: settle kilometres out takes only the colliders near each mover, and reports a cap")
{
    load();
    run("newScene");
    const glm::dvec3 west(-250000, 0, 120000), east(250000, 0, 120000);
    const std::string t1 = cube("TableWest", west, {120, 75, 80});
    const std::string t2 = cube("TableEast", east, {120, 75, 80});
    cube("Between", {0, 0, 120000}, {500, 500, 500});
    const std::string c1 = cube("CrateWest", west + glm::dvec3(0, 140, 0), {30, 30, 30});
    const std::string c2 = cube("CrateEast", east + glm::dvec3(10, 160, 0), {30, 30, 30});
    const json r = run("settle", {{"args", {c1, c2}}});
    CHECK(r["atRest"].get<bool>());
    CHECK_FALSE(r["capped"].get<bool>());
    CHECK(r["colliders"].get<int64_t>() == 2);
    CHECK(worldPos(c1).y == doctest::Approx(75.0).epsilon(0.01));
    CHECK(worldPos(c2).y == doctest::Approx(75.0).epsilon(0.01));
    CHECK(std::abs(worldPos(c1).x - west.x) < 1.0);
    CHECK(r["deepestPenetration"].get<double>() < 0.5);
    CHECK(r["movers"][0]["restsOn"][0] == t1);
    CHECK(r["movers"][1]["restsOn"][0] == t2);

    const std::string high = cube("High", west + glm::dvec3(300, 20000, 0), {30, 30, 30});
    const json capped = run("settle", {{"args", {high}}, {"time", 0.25}});
    CHECK(capped["capped"].get<bool>());
    CHECK_FALSE(capped["atRest"].get<bool>());
}

TEST_CASE("stagehand: a terrain is a heightfield collider and things settle onto it far from the origin")
{
    loadTerrain();
    run("newScene");
    const json t = run("terrainCreate", {{"flat", true}, {"size", json::array({400000, 400000})}, {"resolution", 257},
                                         {"center", json::array({300000, 1500, -200000})}, {"name", "Plain"}});
    const std::string terrain = t["mesh"].get<std::string>();
    run("physicsBody", {{"args", {terrain}}, {"role", "static"}});
    CHECK(run("dressInfo", {{"args", {terrain}}})[0]["shape"] == "heightfield");
    const std::string stone = cube("Stone", {310000, 2500, -190000}, {200, 120, 150});
    run("physicsBody", {{"args", {stone}}, {"material", "stone"}});
    const json r = run("settle", {{"args", {stone}}});
    CHECK(r["atRest"].get<bool>());
    CHECK(worldPos(stone).y == doctest::Approx(1500.0).epsilon(0.001));
    CHECK(r["movers"][0]["restsOn"][0] == terrain);
}

namespace
{
/** A cube broken by a Fracture geometry-nodes modifier into about `pieces` instanced pieces. */
std::string fracturedCube(const char* name, const glm::dvec3& at, int pieces)
{
    const std::string c = cube(name, at, {200, 200, 200});
    const std::string group = std::string(name) + "Break";
    run("gnBuildGroup", {{"args", {json{{"name", group},
                                        {"nodes", json::array({{{"name", "F"}, {"type", "Fracture"}, {"values", {{"pieces", pieces}, {"roughness", 0.0}}}}})},
                                        {"links", json::array({"GroupInput.geometry -> F.mesh", "F.pieces -> GroupOutput.geometry"})}}}}});
    run("gnApply", {{"args", {c, group}}});
    return c;
}

std::filesystem::path scratchFile(const char* name)
{
    const auto dir = std::filesystem::temp_directory_path() / "lila_stagehand_level";
    std::filesystem::create_directories(dir);
    return dir / name;
}
} // namespace

TEST_CASE("stagehand: fracture pieces become objects that settle, undo, and survive save and reopen")
{
    load();
    run("newScene");
    const glm::dvec3 site(1000000, 0, -400000);
    const std::string wall = fracturedCube("Wall", site, 12);
    const json r = run("instancesToObjects", {{"args", {wall}}, {"material", "stone"}});
    const size_t count = r["count"].get<size_t>();
    CHECK(count >= 6);
    CHECK(r["objects"].size() == count);
    CHECK(r["meshes"].get<size_t>() == count); // every piece is its own mesh
    CHECK(run("getAttr", {{"args", {wall + ".visibility"}}}).get<bool>() == false);
    // Each piece sits where its instance was: together they still fill the cube.
    glm::dvec3 lo(1e300), hi(-1e300);
    for (const json& o : r["objects"])
    {
        const json info = run("dressInfo", {{"args", {o}}})[0];
        CHECK(info["role"] == "dynamic");
        CHECK(info["material"] == "stone");
        for (int k = 0; k < 3; ++k)
        {
            lo[k] = std::min(lo[k], info["boundsMin"][k].get<double>());
            hi[k] = std::max(hi[k], info["boundsMax"][k].get<double>());
        }
    }
    CHECK(lo.x == doctest::Approx(site.x - 100.0).epsilon(1e-4));
    CHECK(hi.y == doctest::Approx(200.0).epsilon(0.01));

    // Lifted and settled, the pieces fall in a heap on the ground.
    run("xform", {{"args", {r["group"]}}, {"translation", json::array({0, 300, 0})}});
    const std::vector<std::string> objs = r["objects"].get<std::vector<std::string>>();
    const json s = run("settle", {{"args", objs}});
    CHECK(s["atRest"].get<bool>());
    CHECK(s["deepestPenetration"].get<double>() < 1.0);
    for (const json& m : s["movers"])
        CHECK(m["position"][1].get<double>() < 300.0);

    const auto path = scratchFile("fracture_objects.lila");
    const glm::dvec3 before = worldPos(objs[0]);
    run("saveScene", {{"args", {path.string()}}});
    run("openScene", {{"args", {path.string()}}});
    CHECK(glm::length(worldPos(objs[0]) - before) < 1e-6);
    CHECK(run("dressInfo", {{"args", {objs[0]}}})[0]["material"] == "stone");
    CHECK(run("getAttr", {{"args", {wall + ".visibility"}}}).get<bool>() == false);

    run("newScene");
    const std::string w2 = fracturedCube("Wall", site, 12);
    run("instancesToObjects", {{"args", {w2}}});
    run("undo");
    CHECK(run("getAttr", {{"args", {w2 + ".visibility"}}}).get<bool>() == true);
    CHECK_FALSE(run("objExists", {{"args", {"Wall_Objects"}}}).get<bool>());
}

TEST_CASE("stagehand: scattered copies of one mesh become scene instances of it")
{
    load();
    run("newScene");
    const std::string ground = cube("Lot", {0, 0, 0}, {1000, 10, 1000});
    const std::string rock = cube("Rock", {0, 0, 3000}, {20, 15, 25});
    run("gnBuildGroup",
        {{"args", {json{{"name", "Strew"},
                        {"nodes", json::array({{{"name", "Info"}, {"type", "ObjectInfo"}, {"props", {{"object", rock}, {"transformSpace", "original"}}}},
                                               {{"name", "Pts"}, {"type", "DistributePointsOnFaces"}, {"values", {{"density", 0.2}, {"seed", 3}}}},
                                               {{"name", "Inst"}, {"type", "InstanceOnPoints"}}})},
                        {"links", json::array({"GroupInput.geometry -> Pts.mesh", "Pts.points -> Inst.points", "Info.geometry -> Inst.instance",
                                               "Inst.instances -> GroupOutput.geometry"})}}}}});
    run("gnApply", {{"args", {ground, "Strew"}}});
    const json r = run("instancesToObjects", {{"args", {ground}}, {"name", "Strewn"}});
    CHECK(r["meshes"].get<int64_t>() == 1);
    CHECK(r["count"].get<int64_t>() >= 10);
    CHECK(run("selectInstances", {{"args", {r["objects"][0]}}}).size() == r["objects"].size());

    const json u = run("instancesToObjects", {{"args", {ground}}, {"mode", "unique"}, {"hide", false}});
    CHECK(u["count"] == r["count"]);
    CHECK(run("selectInstances", {{"args", {u["objects"][0]}}}).size() == 1);
}

TEST_CASE("library: the starter props are built on first use, found by words and tags, and placed with their physics")
{
    load();
    run("newScene");
    const json built = run("libraryStarter");
    const std::vector<std::string> ids = built["assets"].get<std::vector<std::string>>();
    CHECK(ids.size() >= 30);
    for (const char* id : {"Industrial/Crate", "Industrial/Barrel", "Industrial/Pallet", "Industrial/Beam", "Industrial/IBeam", "Industrial/PipeSegment",
                           "Industrial/Panel", "Industrial/CableSpool", "Industrial/ScaffoldFrame", "Debris/StoneChunkS1", "Debris/StoneChunkXL3",
                           "Debris/RubblePile", "Debris/BrokenSlab", "Debris/Rebar", "Facility/Locker", "Facility/Console", "Facility/SupplyCrate",
                           "Facility/CableTray", "Road/Delineator", "Road/StreetLight", "Road/EdgeLight", "Road/CatenaryMast"})
        CHECK(std::find(ids.begin(), ids.end(), id) != ids.end());
    CHECK(std::filesystem::exists(std::filesystem::path(built["dir"].get<std::string>()) / "Industrial" / "Barrel" / "asset.json"));
    // Built once: asking again finds them current.
    CHECK_FALSE(run("libraryStarter")["built"].get<bool>());

    const json rebar = run("librarySearch", {{"args", {"rebar"}}, {"tier", "starter"}});
    REQUIRE(rebar.size() >= 2);
    CHECK(rebar[0]["collection"] == "Debris");
    // Small debris by tag and size: only the S and M stone chunks.
    const json small = run("librarySearch", {{"tags", json::array({"debris", "stone"})}, {"maxSize", 50.0}, {"tier", "starter"}});
    CHECK(small.size() == 6);
    for (const json& a : small)
        CHECK(a["physics"]["material"] == "stone");
    const json hangar = run("librarySearch", {{"args", {"starfighter"}}, {"tier", "starter"}});
    CHECK(hangar.size() >= 2);

    const std::string floor = cube("Floor", {0, 0, 0}, {2000, 10, 2000});
    const json crates = run("place", {{"args", {"Industrial/Crate"}}, {"on", floor}, {"count", 3}, {"spacing", json::array({100, 0, 0})}});
    REQUIRE(crates.size() == 3);
    const std::string crateMesh = crates[0]["meshes"][0].get<std::string>();
    const json info = run("dressInfo", {{"args", {crateMesh}}})[0];
    CHECK(info["role"] == "dynamic");
    CHECK(info["material"] == "wood");
    CHECK(info["shape"] == "box");
    CHECK(info["boundsMax"][1].get<double>() - info["boundsMin"][1].get<double>() == doctest::Approx(80.0).epsilon(0.01));
    CHECK(info["boundsMin"][1].get<double>() == doctest::Approx(10.0).epsilon(0.01));

    // An I-beam and a scaffold frame collide as their parts.
    const json beam = run("place", {{"args", {"Industrial/IBeam"}}, {"at", json::array({0, 300, 400})}, {"snap", false}});
    const json beamInfo = run("dressInfo", {{"args", {beam[0]["meshes"][0]}}})[0];
    CHECK(beamInfo["shape"] == "compound");
    CHECK(beamInfo["parts"].get<int64_t>() >= 3);
    CHECK(beamInfo["material"] == "metal");
    const json r = run("settle", {{"args", {beam[0]["meshes"][0]}}});
    CHECK(r["atRest"].get<bool>());
    CHECK(r["movers"][0]["restsOn"][0] == floor);

    const auto path = std::filesystem::temp_directory_path() / "lila_stagehand_level" / "starter_props.lila";
    std::filesystem::create_directories(path.parent_path());
    run("saveScene", {{"args", {path.string()}}});
    run("openScene", {{"args", {path.string()}}});
    CHECK(run("dressInfo", {{"args", {crateMesh}}})[0]["material"] == "wood");
    CHECK(run("dressInfo", {{"args", {beam[0]["meshes"][0]}}})[0]["shape"] == "compound");
}

TEST_CASE("unreal: body shapes export as UCX_ collision beside their meshes, only for meshes with bodies")
{
    load();
    run("newScene");
    const std::string a = arch("Gate");
    run("physicsBody", {{"args", {a}}, {"material", "stone"}, {"role", "dynamic"}});
    const std::string plain = cube("Plain", {500, 0, 0}, {50, 50, 50});
    const auto path = scratchFile("gate_collision.fbx");
    run("exportFbx", {{"args", {path.string(), a, plain}}, {"collision", true}, {"kind", "static"}});
    run("newScene");
    run("importFbx", {{"args", {path.string()}}});
    CHECK(run("objExists", {{"args", {"UCX_Gate_01"}}}).get<bool>());
    CHECK(run("objExists", {{"args", {"UCX_Gate_03"}}}).get<bool>());
    CHECK_FALSE(run("objExists", {{"args", {"UCX_Plain_01"}}}).get<bool>());

    // Without collision=True the file holds only the meshes.
    run("newScene");
    const std::string b = arch("Gate");
    run("physicsBody", {{"args", {b}}, {"material", "stone"}});
    const auto bare = scratchFile("gate_bare.fbx");
    run("exportFbx", {{"args", {bare.string(), b}}, {"kind", "static"}});
    run("newScene");
    run("importFbx", {{"args", {bare.string()}}});
    CHECK_FALSE(run("objExists", {{"args", {"UCX_Gate_01"}}}).get<bool>());
}

TEST_CASE("unreal: placements carry body settings, and simulated groups stay actors")
{
    load();
    run("newScene");
    const std::string crate = cube("Crate", {0, 0, 0}, {100, 100, 100});
    run("physicsBody", {{"args", {crate}}, {"material", "wood"}, {"role", "dynamic"}});
    run("duplicate", {{"args", {crate}}, {"count", 4}, {"mode", "instance"}, {"translate", json::array({150, 0, 0})}});
    const std::string floor = cube("Floor", {0, -10, 0}, {2000, 10, 2000});
    run("physicsBody", {{"args", {floor}}, {"role", "static"}});
    const json plan = run("unrealPlaceInstances", {{"args", {crate, floor}}, {"dryRun", true}, {"threshold", 2}});
    json crates, floors;
    for (const json& g : plan["groups"])
        (g["source"].get<std::string>().find("Crate") != std::string::npos ? crates : floors) = g;
    REQUIRE(crates.is_object());
    CHECK(crates["placements"].size() == 5);
    CHECK(crates["mode"] == "actors");
    const json p = crates["placements"][0]["physics"];
    CHECK(p["simulate"].get<bool>());
    CHECK(p["material"] == "wood");
    CHECK(p["collision"] == "simple");
    CHECK(p["mass"].get<double>() == doctest::Approx(600.0).epsilon(0.02)); // 1 m3 of wood
    REQUIRE(floors.is_object());
    const json f = floors["placements"][0]["physics"];
    CHECK_FALSE(f["simulate"].get<bool>());
    CHECK(f["collision"] == "complexAsSimple");
    CHECK_FALSE(f.contains("mass"));
}

TEST_CASE("unreal: an assembly goes as one Level Instance and keeps its settings through save and reopen")
{
    load();
    run("newScene");
    const std::string l = cube("PostLeft", {-100, 0, 0}, {30, 200, 30});
    const std::string r = cube("PostRight", {100, 0, 0}, {30, 200, 30});
    const std::string top = cube("Lintel", {0, 200, 0}, {260, 30, 30});
    const std::string group = run("assemblyCreate", {{"args", {l, r, top}}, {"name", "Gate"}, {"kind", "packedLevelActor"}}).get<std::string>();
    const json dry = run("unrealPlaceAssembly", {{"args", {group}}, {"dryRun", true}, {"folder", "/Game/Kits"}});
    CHECK(dry["levelInstance"]["kind"] == "packedLevelActor");
    CHECK(dry["levelInstance"]["level"] == "/Game/Kits/LI_Gate");
    CHECK(dry["levelInstance"]["groups"].size() == 3);
    CHECK(dry["plan"]["groups"].size() == 3);
    const std::string key = dry["levelInstance"]["key"].get<std::string>();
    CHECK(key.size() == 16);
    // Anything inside the assembly names it too.
    CHECK(run("unrealPlaceAssembly", {{"args", {l}}, {"dryRun", true}})["levelInstance"]["key"] == key);
    CHECK_THROWS(run("unrealPlaceAssembly", {{"args", {cube("Loose", {900, 0, 0}, {10, 10, 10})}}, {"dryRun", true}}));

    const auto path = scratchFile("assembly.lila");
    run("saveScene", {{"args", {path.string()}}});
    run("openScene", {{"args", {path.string()}}});
    const json again = run("unrealPlaceAssembly", {{"args", {"Gate"}}, {"dryRun", true}});
    CHECK(again["levelInstance"]["key"] == key);
    CHECK(again["levelInstance"]["kind"] == "packedLevelActor");

    run("newScene");
    const std::string c = cube("Crate", {0, 0, 0}, {50, 50, 50});
    run("assemblyCreate", {{"args", {c}}, {"name", "Pile"}});
    run("undo");
    CHECK_FALSE(run("objExists", {{"args", {"Pile"}}}).get<bool>());
}

TEST_CASE("library: an assembly saves its props by id, nests, and places as a whole that survives save and reopen")
{
    load();
    run("newScene");
    const auto lib = std::filesystem::temp_directory_path() / ("lila_stagehand_asm_" + std::to_string(std::random_device{}()));
    std::filesystem::create_directories(lib);
    run("libraryPaths", {{"add", lib.generic_string()}});
    const std::string post = cube("Post", {0, 0, 0}, {30, 200, 30});
    run("physicsBody", {{"args", {post}}, {"material", "stone"}, {"role", "static"}});
    run("libraryAdd", {{"args", {post}}, {"tier", lib.generic_string()}, {"collection", "Kit"}, {"thumbnail", false}});
    const std::string beam = cube("Lintel", {0, 0, 0}, {260, 30, 30});
    run("libraryAdd", {{"args", {beam}}, {"tier", lib.generic_string()}, {"collection", "Kit"}, {"thumbnail", false}});
    run("newScene");

    // A gate: two posts and a lintel from the library, and a step that is only in this scene.
    const json left = run("place", {{"args", {"Kit/Post"}}, {"at", json::array({-115, 0, 0})}, {"snap", false}});
    const json right = run("place", {{"args", {"Kit/Post"}}, {"at", json::array({115, 0, 0})}, {"snap", false}});
    const json lintel = run("place", {{"args", {"Kit/Lintel"}}, {"at", json::array({0, 200, 0})}, {"snap", false}});
    const std::string step = cube("Step", {0, 0, 40}, {200, 10, 40});
    const std::string gate = run("assemblyCreate", {{"args", {left[0]["node"], right[0]["node"], lintel[0]["node"], step}}, {"name", "Gate"},
                                                    {"kind", "packedLevelActor"}})
                                 .get<std::string>();
    const json saved = run("libraryAdd", {{"args", {gate}}, {"tier", lib.generic_string()}, {"collection", "Kits"}, {"thumbnail", false}});
    REQUIRE(saved["assembly"].is_object());
    CHECK(saved["assembly"]["members"].size() == 3);
    CHECK(saved["assembly"]["kind"] == "packedLevelActor");
    CHECK(saved["model"].get<std::string>().find("Gate.glb") != std::string::npos);
    const json found = run("librarySearch", {{"args", {"gate"}}, {"collection", "Kits"}, {"tier", "studio"}});
    REQUIRE(found.size() >= 1);
    CHECK(found[0]["id"] == "Kits/Gate");
    CHECK(found[0]["assembly"]["members"].size() == 3);

    // Two gates side by side make a court: an assembly of assemblies.
    run("newScene");
    const json g1 = run("place", {{"args", {"Kits/Gate"}}, {"at", json::array({-500, 0, 0})}, {"snap", false}});
    const json g2 = run("place", {{"args", {"Kits/Gate"}}, {"at", json::array({500, 0, 0})}, {"snap", false}});
    CHECK(g1[0]["meshes"].size() == 4);
    const std::string court = run("assemblyCreate", {{"args", {g1[0]["node"], g2[0]["node"]}}, {"name", "Court"}}).get<std::string>();
    const json courtAsset = run("libraryAdd", {{"args", {court}}, {"tier", lib.generic_string()}, {"collection", "Kits"}, {"thumbnail", false}});
    CHECK(courtAsset["assembly"]["members"].size() == 2);
    CHECK(courtAsset["assembly"]["members"][0]["asset"] == "Kits/Gate");
    const bool courtHasModel = courtAsset.contains("model") && !courtAsset["model"].get<std::string>().empty();
    CHECK_FALSE(courtHasModel);

    run("newScene");
    const json placed = run("place", {{"args", {"Kits/Court"}}, {"at", json::array({300000, 0, 0})}, {"snap", false}});
    REQUIRE(placed.size() == 1);
    CHECK(placed[0]["meshes"].size() == 8);
    const std::string root = placed[0]["node"].get<std::string>();
    // The two gates keep their 10 m spacing and stand on the point.
    glm::dvec3 lo(1e300), hi(-1e300);
    for (const json& m : placed[0]["meshes"])
    {
        const json info = run("dressInfo", {{"args", {m}}})[0];
        for (int k = 0; k < 3; ++k)
        {
            lo[k] = std::min(lo[k], info["boundsMin"][k].get<double>());
            hi[k] = std::max(hi[k], info["boundsMax"][k].get<double>());
        }
    }
    CHECK(hi.x - lo.x == doctest::Approx(1000.0 + 260.0).epsilon(0.001));
    CHECK(lo.y == doctest::Approx(0.0));
    CHECK((lo.x + hi.x) * 0.5 == doctest::Approx(300000.0));
    int stonePosts = 0;
    for (const json& m : placed[0]["meshes"])
        if (m.get<std::string>().find("Post") != std::string::npos)
            stonePosts += run("dressInfo", {{"args", {m}}})[0]["material"] == "stone" ? 1 : 0;
    CHECK(stonePosts == 4);
    CHECK(run("unrealPlaceAssembly", {{"args", {root}}, {"dryRun", true}})["levelInstance"]["kind"] == "levelInstance");

    const auto path = scratchFile("court.lila");
    run("saveScene", {{"args", {path.string()}}});
    run("openScene", {{"args", {path.string()}}});
    CHECK(run("unrealPlaceAssembly", {{"args", {root}}, {"dryRun", true}})["plan"]["groups"].size() >= 2);

    std::error_code ec;
    std::filesystem::remove_all(lib, ec);
}

TEST_CASE("library: starter thumbnails are drawn without leaving anything in the scene or on the undo stack")
{
    load();
    run("newScene");
    run("libraryStarter", {{"thumbnails", false}});
    // Without a renderer it says so and draws nothing.
    if (!ops::registry().find("captureView"))
        CHECK(run("libraryStarter").contains("thumbnailNote"));

    // A stand-in renderer that writes a file and checks it sees the one prop it is asked to draw.
    int calls = 0;
    ops::registerCommand({.name = "captureView",
                          .flags = {{"path", "", ArgType::String, nullptr, ""},
                                    {"view", "", ArgType::String, nullptr, ""},
                                    {"width", "", ArgType::Int, nullptr, ""},
                                    {"height", "", ArgType::Int, nullptr, ""},
                                    {"display", "", ArgType::String, nullptr, ""},
                                    {"isolate", "", ArgType::Bool, nullptr, ""},
                                    {"frameSelection", "", ArgType::Bool, nullptr, ""},
                                    {"grid", "", ArgType::Bool, nullptr, ""},
                                    {"label", "", ArgType::Bool, nullptr, ""},
                                    {"showSelection", "", ArgType::Bool, nullptr, ""}},
                          .undoable = false,
                          .run = [&calls](const Args& a) -> json {
                              CHECK(scene::Graph::current().selection().size() == 1);
                              std::ofstream(a.get<std::string>("path"), std::ios::binary) << "png";
                              ++calls;
                              return a.get<std::string>("path");
                          }});
    const std::string keep = cube("Keep", {0, 0, 0}, {10, 10, 10});
    const size_t nodes = scene::Graph::current().nodes().size();
    const json r = run("libraryStarter", {{"rebuild", true}});
    ops::unregisterCommand("captureView");
    CHECK(r["thumbnails"].get<int64_t>() == int64_t(r["assets"].size()));
    CHECK(r["missing"].get<int64_t>() == 0);
    CHECK(calls == int(r["assets"].size()));
    CHECK(scene::Graph::current().nodes().size() == nodes);
    CHECK(std::filesystem::exists(std::filesystem::path(r["dir"].get<std::string>()) / "Facility" / "Locker" / "thumb.png"));
    CHECK_FALSE(run("librarySearch", {{"args", {"Locker"}}, {"tier", "starter"}})[0]["thumbnail"].get<std::string>().empty());
    // The last undo step is still the cube.
    run("undo");
    CHECK_FALSE(run("objExists", {{"args", {keep}}}).get<bool>());

    // A script that empties the scene while a thumbnail is drawn (drawing pumps events) leaves nothing to roll back.
    ops::registerCommand({.name = "captureView",
                          .flags = {{"path", "", ArgType::String, nullptr, ""},
                                    {"view", "", ArgType::String, nullptr, ""},
                                    {"width", "", ArgType::Int, nullptr, ""},
                                    {"height", "", ArgType::Int, nullptr, ""},
                                    {"display", "", ArgType::String, nullptr, ""},
                                    {"isolate", "", ArgType::Bool, nullptr, ""},
                                    {"frameSelection", "", ArgType::Bool, nullptr, ""},
                                    {"grid", "", ArgType::Bool, nullptr, ""},
                                    {"label", "", ArgType::Bool, nullptr, ""},
                                    {"showSelection", "", ArgType::Bool, nullptr, ""}},
                          .undoable = false,
                          .run = [](const Args& a) -> json {
                              ops::execute("newScene", json::object());
                              std::ofstream(a.get<std::string>("path"), std::ios::binary) << "png";
                              return a.get<std::string>("path");
                          }});
    CHECK_NOTHROW(run("libraryStarter", {{"rebuild", true}}));
    ops::unregisterCommand("captureView");
    CHECK(scene::Graph::current().nodes().size() <= nodes);
}

TEST_CASE("library: a saved prop comes back with one body and its snap points, and no empty transforms for them")
{
    load();
    run("newScene");
    const auto lib = std::filesystem::temp_directory_path() / ("lila_stagehand_tags_" + std::to_string(std::random_device{}()));
    std::filesystem::create_directories(lib);
    run("libraryPaths", {{"add", lib.generic_string()}});
    const std::string crate = cube("Crate", {0, 0, 0}, {60, 60, 60});
    run("physicsBody", {{"args", {crate}}, {"material", "wood"}, {"role", "dynamic"}});
    run("addSnapPoint", {{"args", {crate}}, {"type", "stack"}, {"position", json::array({30, 30, 0})}, {"normal", json::array({1, 0, 0})}, {"name", "side"}});
    run("libraryAdd", {{"args", {crate}}, {"tier", lib.generic_string()}, {"collection", "Tags"}, {"thumbnail", false}});
    // The source keeps its body and snap point.
    CHECK(run("dressInfo", {{"args", {crate}}})[0]["material"] == "wood");
    CHECK(run("snapPoints", {{"args", {crate}}}).size() == 1);

    run("newScene");
    const json placed = run("place", {{"args", {"Tags/Crate"}}, {"at", json::array({0, 0, 0})}, {"snap", false}});
    scene::Graph& g = scene::Graph::current();
    const scene::Node* root = g.find(placed[0]["node"].get<std::string>());
    REQUIRE(root);
    int bodies = 0, snaps = 0, empties = 0;
    for (scene::NodeId d : g.descendants(root->id()))
    {
        const std::string type = g.node(d)->type().name;
        bodies += type == "stagehandBody";
        snaps += type == "stagehandSnap";
        empties += type == "transform";
    }
    CHECK(bodies == 1);
    CHECK(snaps == 1);
    CHECK(empties == 0);
    std::error_code ec;
    std::filesystem::remove_all(lib, ec);
}

TEST_CASE("stagehand: terrain roads stand the starter library's road props, turned along the road")
{
    loadTerrain();
    run("newScene");
    json t = run("terrainCreate", {{"size", {40000.0, 40000.0}}, {"resolution", 129}, {"flat", true}, {"name", "Propped"}});
    const std::string mesh = t["mesh"];
    run("terrainAddRoad", {{"args", {mesh}}, {"id", "lit"}, {"path", {{-15000.0, 0.0}, {15000.0, 0.0}}}, {"settings", {{"poleSpacing", 3000.0}, {"markerSpacing", 5000.0}}}});
    json built = run("terrainRoadBuild", {{"args", {mesh}}});
    REQUIRE(built.size() == 1);
    int lights = 0, markers = 0;
    for (const json& p : built[0]["props"])
    {
        const std::string node = p.get<std::string>();
        lights += node.find("StreetLight") != std::string::npos;
        markers += node.find("Delineator") != std::string::npos;
    }
    CHECK(lights >= 9);
    CHECK(markers >= 10);

    json boxes = run("terrainRoadBuild", {{"args", {mesh}}, {"props", "boxes"}});
    for (const json& p : boxes[0]["props"])
    {
        const std::string node = p.get<std::string>();
        CHECK(node.find("StreetLight") == std::string::npos);
        CHECK(node.find("Delineator") == std::string::npos);
    }
    run("newScene");
}

TEST_CASE("arrange: debrisProps strews chips sized from the pieces beside them, at rest, in one undo step")
{
    load();
    run("newScene");
    run("libraryStarter", {{"thumbnails", false}});
    const std::string big = cube("BigPiece", {0, 0, 0}, {100, 100, 100});
    const std::string small = cube("SmallPiece", {300, 0, 0}, {25, 25, 25});
    run("group", {{"args", {big, small}}, {"name", "Broken"}});
    const json r = run("debrisProps", {{"args", {"Broken"}}, {"amount", 6.0}});
    REQUIRE(r["nodes"].is_array());
    const size_t chips = r["nodes"].size();
    CHECK(chips >= 4);
    CHECK(r["atRest"].get<int64_t>() == int64_t(chips));
    int nearBig = 0, nearSmall = 0;
    json chipMeshes = json::array();
    double biggest = 0.0;
    for (const json& n : r["nodes"])
    {
        json info;
        for (const json& d : run("listRelatives", {{"args", {n}}, {"allDescendents", true}}))
            if (run("nodeType", {{"args", {d}}}) == "mesh")
                info = run("dressInfo", {{"args", {d}}})[0];
        REQUIRE(info.is_object());
        chipMeshes.push_back(info["node"]);
        const glm::dvec3 lo(info["boundsMin"][0].get<double>(), info["boundsMin"][1].get<double>(), info["boundsMin"][2].get<double>());
        const glm::dvec3 hi(info["boundsMax"][0].get<double>(), info["boundsMax"][1].get<double>(), info["boundsMax"][2].get<double>());
        biggest = std::max(biggest, std::max({hi.x - lo.x, hi.y - lo.y, hi.z - lo.z}));
        const glm::dvec3 c = (lo + hi) * 0.5;
        (glm::length(glm::dvec2(c.x, c.z)) < glm::length(glm::dvec2(c.x - 300.0, c.z)) ? nearBig : nearSmall) += 1;
    }
    // Resting on the ground or the pieces: nothing floats or sinks in.
    CHECK(run("dressCheck", {{"args", chipMeshes}})["ok"].get<bool>());
    // Chips are a fraction of their piece, and the big piece sheds more of them.
    CHECK(biggest < 50.0);
    CHECK(nearBig > nearSmall);
    run("undo");
    CHECK_FALSE(run("objExists", {{"args", {r["group"]}}}).get<bool>());
    CHECK_THROWS(run("debrisProps", {{"args", {"NoSuchWall"}}}));
}

TEST_CASE("presets: a built-in preset dresses a floor in one undo step; a saved one runs; unset slots are named")
{
    load();
    run("newScene");
    run("libraryStarter", {{"thumbnails", false}});
    const json all = run("arrangementPresets");
    std::vector<std::string> names;
    for (const json& p : all)
        names.push_back(p["name"].get<std::string>());
    for (const char* want : {"Storeroom", "Rubble Corner", "Library Shelf", "Messy Desk"})
        CHECK(std::ranges::find(names, want) != names.end());

    const std::string floor = cube("Floor", {0, -10, 0}, {800, 10, 800});
    const json r = run("arrangePreset", {{"args", {"Storeroom"}}, {"slots", {{"floor", floor}}}});
    REQUIRE(r["group"].is_string());
    CHECK(r["steps"].size() == 2);
    CHECK(r["steps"][0]["result"]["count"].get<int64_t>() > 0);
    CHECK(r["steps"][1]["result"]["count"].get<int64_t>() == 6);
    const std::string group = r["group"].get<std::string>();
    CHECK(run("listRelatives", {{"args", {group}}}).size() == 2);
    run("undo");
    CHECK_FALSE(run("objExists", {{"args", {group}}}).get<bool>());

    // The optional broken slot left out skips the chips.
    const json rubble = run("arrangePreset", {{"args", {"rubble corner"}}, {"slots", {{"floor", floor}}}, {"seed", 3}});
    CHECK(rubble["steps"][2]["skipped"].get<bool>());

    try
    {
        run("arrangePreset", {{"args", {"Library Shelf"}}, {"slots", {{"boards", {floor}}}}});
        FAIL("no books given");
    }
    catch (const std::exception& e)
    {
        CHECK(std::string(e.what()).find("books") != std::string::npos);
    }
    CHECK_THROWS(run("arrangePreset", {{"args", {"No Such Preset"}}}));

    const json spec = {{"description", "A row of crates."},
                       {"slots", {{"floor", {{"kind", "mesh"}}}}},
                       {"steps", json::array({{{"command", "stackProps"},
                                               {"flags", {{"items", json::array({"Industrial/Crate"})}, {"at", "$floor.top"}, {"count", 3}, {"realize", true}}}}})}};
    const std::string file = run("arrangementSave", {{"args", {"Crate Tower"}}, {"spec", spec}, {"overwrite", true}}).get<std::string>();
    CHECK(std::filesystem::exists(file));
    const json tower = run("arrangePreset", {{"args", {"Crate Tower"}}, {"slots", {{"floor", floor}}}});
    CHECK(tower["steps"][0]["result"]["count"].get<int64_t>() == 3);
    CHECK_THROWS(run("arrangementSave", {{"args", {"Bad"}}, {"spec", {{"steps", json::array({{{"command", "noSuchCommand"}}})}}}}));
    std::error_code ec;
    std::filesystem::remove(file, ec);
}
