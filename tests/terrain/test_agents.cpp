#include "TestUtil.h"

#include "core/ImageIO.h"
#include "core/MainQueue.h"
#include "mcp/McpServer.h"
#include "scene/Graph.h"
#include "scene/MeshNodes.h"
#include "sculpt/SculptSession.h"
#include "render/HeightfieldRenderer.h"
#include "ui/HeightfieldDisplay.h"

#include <cmath>
#include <filesystem>
#include <future>
#include <map>
#include <set>

using namespace terraintest;
namespace fs = std::filesystem;

namespace
{
/** Two round hills 16000 cm apart along X joined by a lower ridge: a saddle between them at the origin. */
json twoHills(const std::string& name)
{
    json t = run("terrainCreate", {{"size", {40000.0, 40000.0}}, {"resolution", 129}, {"flat", true}, {"name", name}});
    for (double x : {-8000.0, 8000.0})
        run("terrainStroke", {{"args", {name}}, {"brush", "raise"}, {"points", {{x, 0.0}}}, {"radius", 8000.0}, {"strength", 3000.0}});
    // A lower ridge joining them, so the ground between is a saddle.
    run("terrainStroke", {{"args", {name}}, {"brush", "raise"}, {"path", {{-8000.0, 0.0}, {8000.0, 0.0}}}, {"radius", 4000.0}, {"strength", 1000.0}});
    return run("terrainInfo", {{"args", {name}}});
}
} // namespace

TEST_CASE("agents: terrainQuery finds the peak, the pass and slopes, and stores regions other commands take by name")
{
    loadTerrain();
    run("newScene");
    const json t = twoHills("QueryHills");
    const double top = t["heightRange"][1].get<double>();

    json peak = run("terrainQuery", {{"args", {"QueryHills"}}, {"named", "highestPeak"}, {"store", "peak"}});
    REQUIRE(peak["points"].size() == 1);
    CHECK(peak["points"][0][1].get<double>() == doctest::Approx(top).epsilon(0.001));
    CHECK(std::abs(std::abs(peak["points"][0][0].get<double>()) - 8000.0) < 1500.0);
    CHECK(peak["mask"] == "region_peak");
    CHECK(run("terrainInfo", {{"args", {"QueryHills"}}})["masks"].contains("region_peak"));

    // A reference resolves to the stored point wherever a command takes one.
    const json s = run("terrainSample", {{"args", {"QueryHills"}}, {"points", {"region:peak"}}});
    CHECK(s[0]["height"].get<double>() == doctest::Approx(top).epsilon(0.001));

    json pass = run("terrainQuery", {{"args", {"QueryHills"}}, {"named", "passes"}, {"store", "pass"}, {"pick", 0}});
    REQUIRE(pass["points"].size() >= 1);
    CHECK(std::abs(pass["points"][0][0].get<double>()) < 1000.0);
    CHECK(std::abs(pass["points"][0][2].get<double>()) < 1000.0);

    // North-facing (downhill toward -Z) slopes lie on the -Z side of the hills.
    json north = run("terrainQuery", {{"args", {"QueryHills"}}, {"named", "northSlopes"}});
    REQUIRE(north["regions"].size() >= 1);
    CHECK(north["regions"][0]["centroid"][1].get<double>() < -1000.0);
    json high = run("terrainQuery", {{"args", {"QueryHills"}}, {"where", {{"height", {top * 0.8, nullptr}}}}});
    REQUIRE(high["regions"].size() == 2);
    CHECK(high["regions"][0]["height"]["min"].get<double>() >= top * 0.8 - 1.0);

    json flat = run("terrainQuery", {{"args", {"QueryHills"}}, {"named", "flatAreas"}, {"minArea", 1000.0}, {"store", "plain"}});
    REQUIRE(flat["regions"].size() >= 1);
    CHECK(flat["regions"][0]["slope"]["max"].get<double>() <= 5.0);
    // A polygon reference: a lake outlined by the stored region's largest part.
    json stats = run("terrainRegionStats", {{"args", {"QueryHills"}}, {"polygon", "region:plain"}});
    CHECK(stats.dump().size() > 10);

    json listed = run("terrainQuery", {{"args", {"QueryHills"}}, {"list", true}});
    CHECK(listed["regions"].contains("peak"));
    CHECK(listed["regions"].contains("pass"));
    run("terrainQuery", {{"args", {"QueryHills"}}, {"remove", "plain"}});
    CHECK_FALSE(run("terrainQuery", {{"args", {"QueryHills"}}, {"list", true}})["regions"].contains("plain"));
    CHECK_THROWS(run("terrainSample", {{"args", {"QueryHills"}}, {"points", {"region:nowhere"}}}));
    run("newScene");
}

TEST_CASE("agents: terrainSpec builds a terrain from one spec and reads it back as a spec that rebuilds it")
{
    loadTerrain();
    run("newScene");
    const json spec = {
        {"name", "SpecHills"},
        {"size", {60000.0, 60000.0}},
        {"resolution", 129},
        {"seed", 7},
        {"source", {{"type", "noise"}, {"amplitude", 8000.0}, {"featureSize", 30000.0}, {"ridged", 0.4}}},
        {"erosion", {{"preset", "hills"}, {"age", 0.4}}},
        {"regions", {{{"name", "low"}, {"named", "lowestPoint"}}, {{"name", "top"}, {"named", "highestPeak"}}}},
        {"roads", {{{"id", "climb"}, {"kind", "track"}, {"route", {{"start", "region:low"}, {"end", "region:top"}, {"maxGrade", 15.0}}}}}},
        {"materials", true},
    };
    json r = run("terrainSpec", {{"spec", spec}});
    INFO(r.dump().substr(0, 2000));
    CHECK(r["ok"] == true);
    std::vector<std::string> steps;
    for (const json& s : r["steps"])
    {
        CHECK(s["ok"] == true);
        steps.push_back(s["step"]);
    }
    CHECK(steps == std::vector<std::string>{"source", "erosion", "regions.low", "regions.top", "roads", "materials", "check"});
    CHECK(r["stats"]["resolution"][0] == 129);
    const std::string rev = r["revision"];

    // Read back while unchanged: the authored spec; rebuilt under another name, the same ground.
    json back = run("terrainSpec", {{"args", {"SpecHills"}}});
    CHECK(back["from"] == "spec");
    json again = back["spec"];
    again["name"] = "SpecHills2";
    json r2 = run("terrainSpec", {{"spec", again}});
    CHECK(r2["revision"] == rev);

    // Edited after the build: read back as its node group, which rebuilds the same ground too.
    run("terrainStroke", {{"args", {"SpecHills"}}, {"brush", "raise"}, {"points", {{0.0, 0.0}}}, {"radius", 8000.0}, {"strength", 1500.0}, {"stage", "final"}});
    const std::string edited = run("terrainInfo", {{"args", {"SpecHills"}}})["revision"];
    CHECK(edited != rev);
    json graph = run("terrainSpec", {{"args", {"SpecHills"}}});
    CHECK(graph["from"] == "graph");
    json g2 = graph["spec"];
    g2["name"] = "SpecHills3";
    json r3 = run("terrainSpec", {{"spec", g2}});
    CHECK(r3["revision"] == edited);

    // Same name again needs replace; unknown keys are refused.
    CHECK_THROWS(run("terrainSpec", {{"spec", spec}}));
    CHECK(run("terrainSpec", {{"spec", spec}, {"replace", true}})["revision"] == rev);
    CHECK_THROWS(run("terrainSpec", {{"spec", {{"name", "X"}, {"mountains", 3}}}}));
    run("newScene");
}

TEST_CASE("agents: every terrainSpec preset builds and passes its steps")
{
    loadTerrain();
    run("newScene");
    const json presets = run("terrainSpec", {{"list", true}});
    REQUIRE(presets.size() == 5);
    for (const json& p : presets)
    {
        const std::string name = p["name"];
        CAPTURE(name);
        CHECK(!p["description"].get<std::string>().empty());
        json r = run("terrainSpec", {{"preset", name}, {"overrides", {{"resolution", 129}}}});
        for (const json& s : r["steps"])
        {
            INFO(s.dump().substr(0, 800));
            CHECK(s["ok"] == true);
        }
        INFO(r["check"].dump());
        CHECK(r["ok"] == true);
    }
    run("newScene");
}

TEST_CASE("agents: terrainWedge builds spec variants in parallel lilabatch processes into a labelled contact sheet")
{
    loadTerrain();
    run("newScene");
    const json spec = {{"name", "WedgeHills"},
                       {"size", {40000.0, 40000.0}},
                       {"resolution", 65},
                       {"source", {{"type", "noise"}, {"amplitude", 6000.0}, {"featureSize", 20000.0}}},
                       {"erosion", {{"preset", "hills"}, {"age", 0.3}}}};
    const std::string folder = (std::filesystem::temp_directory_path() / "lila_terrain_tests_wedge").string();
    json w = run("terrainWedge", {{"spec", spec}, {"vary", {{"seed", {1, 2, 3}}, {"erosion.age", {0.2, 0.5, 0.2}}}}, {"count", 3}, {"tile", 96}, {"folder", folder}});
    INFO(w.dump().substr(0, 3000));
    CHECK(w["mode"] == "parallel");
    REQUIRE(w["variants"].size() == 3);
    std::set<std::string> revisions;
    for (const json& v : w["variants"])
    {
        CHECK(v["ok"] == true);
        CHECK(v["image"].is_string());
        revisions.insert(v["revision"].get<std::string>());
    }
    CHECK(revisions.size() == 3);
    CHECK(w["variants"][1]["vary"]["erosion.age"] == 0.5);
    const lila::image::Image sheet = lila::image::read(w["sheet"].get<std::string>());
    CHECK(sheet.width == 2 * (96 + 6) + 6);
    CHECK(sheet.height > 2 * 96);
    CHECK(std::filesystem::exists(w["json"].get<std::string>()));
    // Nothing is left in the scene, and the same variant built here gives the worker's ground.
    CHECK(run("ls", {{"type", "mesh"}}).size() == 0);
    json one = spec;
    one["seed"] = 2;
    one["erosion"]["age"] = 0.5;
    CHECK(run("terrainSpec", {{"spec", one}})["revision"] == w["variants"][1]["revision"]);

    json seq = run("terrainWedge", {{"spec", spec}, {"count", 2}, {"tile", 64}, {"parallel", false}, {"folder", folder}});
    CHECK(seq["mode"] == "inProcess");
    CHECK(seq["note"] == "parallel=False");
    CHECK(seq["variants"][0]["ok"] == true);
    CHECK(seq["variants"][1]["vary"]["seed"] == 1);
    run("newScene");
}

TEST_CASE("agents: material rules set the heightfield draw's paint layers, one per layer in its colour")
{
    loadTerrain();
    if (!ops::find("terrainDisplay"))
        ui::heightfieldDisplay::registerCommands();
    run("newScene");
    const json t = run("terrainCreate", {{"size", {60000.0, 60000.0}}, {"resolution", 129}, {"erosion", 20}, {"name", "Painted"}});
    json rules = run("terrainMaterialRules", {{"args", {"Painted"}}, {"set", {{"name", "moss"}, {"color", {0.1, 0.4, 0.05}}, {"when", {{"slope", {0.0, 20.0}}}}}}});
    const size_t count = rules["layers"].size();
    REQUIRE(count >= 3);
    REQUIRE(rules["displayLayers"].size() == count);

    json shown = run("terrainDisplayLayers", {{"args", {"Painted"}}});
    REQUIRE(shown["layers"].size() == count);
    for (size_t i = 0; i < count; ++i)
    {
        const std::string name = rules["layers"][i]["name"];
        CHECK(shown["layers"][i]["name"] == name);
        CHECK(shown["layers"][i]["mask"] == "layer_" + name);
        CHECK(shown["layers"][i]["found"] == true);
    }
    CHECK(rules["displayLayers"].back()["color"] == "#59aa3f");

    scene::Graph& g = scene::Graph::current();
    ui::heightfieldDisplay::Settings set;
    std::optional<render::HeightfieldDraw> d = ui::heightfieldDisplay::drawFor(g, g.find("Painted")->id(), set, true);
    REQUIRE(d);
    REQUIRE(d->layers.size() == count);
    CHECK(d->layers.back().color.g == doctest::Approx(0.4f).epsilon(0.02));

    // A rule change reaches the draw again.
    run("terrainMaterialRules", {{"args", {"Painted"}}, {"remove", {"moss"}}});
    CHECK(run("terrainDisplayLayers", {{"args", {"Painted"}}})["layers"].size() == count - 1);
    run("newScene");
}

TEST_CASE("agents: a sculpted overhang becomes a volume piece unless keepOverhangs is off")
{
    loadTerrain();
    if (!ops::find("sculptEnter"))
        sculpt::registerCommands();
    auto sculptShelf = [](const std::string& name, bool keep) {
        run("terrainCreate", {{"size", {40000.0, 40000.0}}, {"resolution", 129}, {"flat", true}, {"name", name}});
        json r = run("terrainSculptRegion", {{"args", {name}}, {"center", {0.0, 0.0}}, {"size", 10000.0}});
        run("sculptCancel");
        scene::Graph& g = scene::Graph::current();
        scene::Node* proxy = g.find(r["mesh"].get<std::string>());
        REQUIRE(proxy);
        g.evaluate();
        geo::Mesh m = *proxy->as<scene::MeshPtr>(scene::MeshNode::kOutMesh);
        // A slab floating 300 to 900 cm over one corner of the patch: rock with air under it.
        const double y0 = 300.0, y1 = 900.0, a0 = 3000.0, a1 = 4500.0;
        const geo::Index base = m.addPoint({a0, y0, a0});
        for (const glm::dvec3& q : {glm::dvec3(a1, y0, a0), glm::dvec3(a1, y0, a1), glm::dvec3(a0, y0, a1), glm::dvec3(a0, y1, a0), glm::dvec3(a1, y1, a0),
                                    glm::dvec3(a1, y1, a1), glm::dvec3(a0, y1, a1)})
            m.addPoint(q);
        m.addFace({base + 0, base + 1, base + 2, base + 3});
        m.addFace({base + 4, base + 7, base + 6, base + 5});
        g.setValue({proxy->id(), scene::MeshNode::kInMesh, -1}, scene::MeshPtr(std::make_shared<geo::Mesh>(std::move(m))));
        return run("terrainSculptRegion", {{"args", {name}}, {"finish", true}, {"keepOverhangs", keep}});
    };
    run("newScene");
    json f = sculptShelf("Shelf", true);
    REQUIRE(f["overhangs"].size() == 1);
    REQUIRE(f.contains("overhangVolume"));
    const json ov = f["overhangVolume"];
    CHECK(ov["cells"].get<int64_t>() > 4);
    const json vol = run("terrainVolume", {{"args", {"Shelf"}}});
    REQUIRE(vol["ops"].size() == 1);
    CHECK(vol["ops"][0]["id"] == ov["id"]);
    REQUIRE(vol["volume"]["pieces"].size() >= 1);
    // The piece covers the slab: over the corner, from its underside to its top.
    bool covers = false;
    for (const json& p : vol["volume"]["pieces"])
    {
        const json b = p["bounds"];
        covers = covers || (b["min"][0].get<double>() <= 3500.0 && b["max"][0].get<double>() >= 4000.0 && b["max"][1].get<double>() >= 850.0);
    }
    CHECK(covers);
    CHECK(run("getAttr", {{"args", {ov["mesh"].get<std::string>() + ".visibility"}}}) == false);

    json off = sculptShelf("ShelfFlat", false);
    CHECK(off["overhangs"].size() == 1);
    CHECK_FALSE(off.contains("overhangVolume"));
    CHECK(run("terrainVolume", {{"args", {"ShelfFlat"}}})["ops"].empty());
    run("newScene");
}

TEST_CASE("agents: terrainCheck flags roads over their grade or over water unbridged, and lakes with no outlet")
{
    loadTerrain();
    run("newScene");
    run("terrainCreate", {{"size", {60000.0, 60000.0}}, {"resolution", 129}, {"flat", true}, {"name", "Checked"}});
    run("terrainAddRiver", {{"args", {"Checked"}}, {"id", "river"}, {"path", {{-30000.0, 0.0}, {30000.0, 0.0}}}, {"width", 800.0}, {"depth", 200.0}});
    json clean = run("terrainCheck", {{"args", {"Checked"}}});
    INFO(clean.dump().substr(0, 1500));
    for (const json& i : clean["issues"])
        CHECK(i["type"] != "riverUphill");

    run("terrainAddRoad", {{"args", {"Checked"}}, {"id", "ford"}, {"path", {{0.0, -20000.0}, {0.0, 20000.0}}}, {"bridgeHeight", 0.0}});
    run("terrainAddRoad", {{"args", {"Checked"}}, {"id", "ramp"}, {"path", {{-20000.0, 15000.0}, {20000.0, 15000.0}}}, {"maxGrade", 6.0},
                           {"keys", {{-20000.0, 0.0, 15000.0}, {20000.0, 8000.0, 15000.0}}}});
    json lake = run("terrainAddLake", {{"args", {"Checked"}}, {"id", "sink"}, {"polygon", {{12000.0, -18000.0}, {18000.0, -18000.0}, {18000.0, -12000.0}, {12000.0, -12000.0}}},
                                       {"level", -300.0}, {"depth", 200.0}});
    INFO(lake["lakes"].dump());

    json c = run("terrainCheck", {{"args", {"Checked"}}});
    INFO(c.dump().substr(0, 3000));
    std::map<std::string, std::string> found;
    for (const json& i : c["issues"])
        found[i["type"].get<std::string>()] = i["about"].get<std::string>();
    CHECK(c["ok"] == false);
    CHECK(found["roadWater"] == "Roads/ford");
    CHECK(found["roadGrade"] == "Roads/ramp");
    CHECK(found["lakeNoOutlet"] == "sink");
    CHECK_FALSE(found.count("riverUphill"));
    run("newScene");
}

TEST_CASE("agents: terrainCheck gives a pad's stream crossing, and a culvert laid along it carries the stream")
{
    loadTerrain();
    run("newScene");
    run("terrainCreate", {{"size", {200000.0, 200000.0}}, {"resolution", 257}, {"seed", 3}, {"name", "Yard"},
                          {"amplitude", 4000.0}, {"featureSize", 60000.0}});
    // A pad across the middle of a stream two others flow into (a reach of the second order or more).
    const json water = run("terrainWaterInfo", {{"args", {"Yard"}}, {"riverArea", 1.0e5}});
    std::map<int, int> inflows;
    for (const json& r : water["rivers"])
        if (r.value("downstream", -1) >= 0)
            ++inflows[r["downstream"].get<int>()];
    json at;
    for (const json& r : water["rivers"])
        if (inflows[r.value("id", -1)] >= 2 && r["points"].size() >= 6 && at.is_null())
            at = r["points"][r["points"].size() / 2]["position"];
    INFO(water["rivers"].size(), " rivers");
    REQUIRE(at.is_array());
    const double x = at[0].get<double>(), z = at[2].get<double>();
    run("terrainStampPad", {{"args", {"Yard"}}, {"id", "yard"}, {"footprint", {{x - 3000.0, z - 3000.0}, {x + 3000.0, z - 3000.0}, {x + 3000.0, z + 3000.0}, {x - 3000.0, z + 3000.0}}},
                            {"falloff", 500.0}});
    json crossing;
    const json first = run("terrainCheck", {{"args", {"Yard"}}, {"riverArea", 1.0e5}});
    for (const json& i : first["issues"])
        if (i["type"] == "blocksFlow" && i["about"] == "Stamps/yard")
            crossing = i["crossing"];
    INFO(first.dump().substr(0, 2000));
    REQUIRE(crossing.is_array());
    REQUIRE(crossing.size() >= 2);
    json path = json::array();
    for (const json& p : crossing)
        path.push_back({p[0], p[2]});
    run("terrainAddFeature", {{"args", {"Yard"}}, {"kind", "culvert"}, {"id", "yardCulvert"}, {"path", path}, {"width", 300.0}});
    int left = 0;
    for (const json& i : run("terrainCheck", {{"args", {"Yard"}}, {"riverArea", 1.0e5}})["issues"])
        left += i["type"] == "blocksFlow" && i["about"] == "Stamps/yard";
    CHECK(left == 0);
    run("newScene");
}

namespace
{
/** A tools/call, resources/read or prompts/get through the MCP endpoint, answered on a worker while this thread runs it. */
json mcpCall(const std::string& method, const json& params)
{
    const json request = {{"jsonrpc", "2.0"}, {"id", 1}, {"method", method}, {"params", params}};
    auto fut = std::async(std::launch::async, [request] { return mcp::Server::get().handle(request); });
    while (fut.wait_for(std::chrono::milliseconds(5)) != std::future_status::ready)
        mainq::drain();
    const json reply = fut.get();
    REQUIRE(reply.contains("result"));
    return reply["result"];
}

json commandOverMcp(const std::string& name, const json& args)
{
    const json r = mcpCall("tools/call", {{"name", "run_command"}, {"arguments", {{"name", name}, {"args", args}}}});
    REQUIRE_FALSE(r.value("isError", false));
    return json::parse(r["content"][0]["text"].get<std::string>());
}

const char* kValleySpec = R"({
  "name": "RiverValley",
  "description": "A river valley between two ranges with a road over the pass and a cave in the cliff.",
  "size": [200000, 200000],
  "resolution": 257,
  "seed": 3,
  "source": {"type": "noise", "amplitude": 4000, "featureSize": 40000, "ridged": 0.3},
  "constraints": [
    {"id": "northRange", "kind": "ridge", "path": [[-100000, 60000], [100000, 60000]], "height": 16000, "width": 70000},
    {"id": "southRange", "kind": "ridge", "path": [[-100000, -60000], [100000, -60000]], "height": 18000, "width": 70000},
    {"id": "notch", "kind": "valley", "path": [[10000, 35000], [10000, 85000]], "height": 9000, "width": 30000},
    {"id": "riverBed", "kind": "river", "path": [[-100000, 2000], [100000, -2000]], "height": 800, "width": 16000}
  ],
  "erosion": {"preset": "natural", "age": 0.4},
  "landforms": [{"process": "cliff", "settings": {"threshold": 28, "step": 1200, "strength": 0.7}}],
  "water": {"rivers": [{"id": "valleyRiver", "path": [[-100000, 2000], [100000, -2000]], "width": 1200, "depth": 200}]},
  "regions": [
    {"name": "pass", "named": "passes", "where": {"z": [35000, 85000], "x": [-15000, 35000]}, "pick": 0},
    {"name": "cliff", "named": "steepSlopes", "where": {"z": [-50000, -20000], "aspect": "south"}, "minArea": 2000, "pick": 0}
  ],
  "roads": [
    {"id": "passUp", "kind": "road", "route": {"start": [10000, 14000], "end": "region:pass", "maxGrade": 10}},
    {"id": "passDown", "kind": "road", "route": {"start": "region:pass", "end": [10000, 98000], "maxGrade": 10}}
  ],
  "volume": [{"op": "cave", "id": "cliffCave", "path": ["region:cliff", [0, -60000]], "radius": 500, "chambers": 1}],
  "materials": true,
  "export": {"target": "unreal"}
})";
} // namespace

TEST_CASE("agents: over MCP, one spec builds a river valley between two ranges with a road over the pass and a cave in the cliff, passes terrainCheck and exports for Unreal")
{
    loadTerrain();
    mcp::registerBuiltins();
    run("newScene");

    // What an agent reads first: the schema resource and the make_terrain prompt.
    const json schema = json::parse(mcpCall("resources/read", {{"uri", "lila://schema/terrain-spec"}})["contents"][0]["text"].get<std::string>());
    CHECK(schema["properties"].contains("roads"));
    CHECK(schema["properties"].contains("regions"));
    const json prompt = mcpCall("prompts/get", {{"name", "make_terrain"}, {"arguments", {{"description", "a river valley between two ranges"}, {"target", "unreal"}}}});
    const std::string promptText = prompt["messages"][0]["content"]["text"];
    CHECK(promptText.find("a river valley between two ranges") != std::string::npos);
    CHECK(promptText.find("lila://schema/terrain-spec") != std::string::npos);

    json spec = json::parse(kValleySpec);
    const fs::path folder = fs::temp_directory_path() / "lila_terrain_tests_valley";
    fs::remove_all(folder);
    spec["export"]["folder"] = folder.string();
    const json r = commandOverMcp("terrainSpec", {{"spec", spec}});
    INFO(r.dump().substr(0, 4000));
    for (const json& s : r["steps"])
        CHECK(s["ok"] == true);
    CHECK(r["check"]["ok"] == true);
    CHECK(r["ok"] == true);

    // The pass is a notch in the north range, lower than the range's crest away from it.
    const json regions = commandOverMcp("terrainQuery", {{"args", {"RiverValley"}}, {"list", true}})["regions"];
    const json pass = regions["pass"]["point"];
    CHECK(pass[2].get<double>() > 35000.0);
    CHECK(pass[2].get<double>() < 85000.0);
    const double crest = commandOverMcp("terrainSample", {{"args", {"RiverValley"}}, {"points", {{-70000.0, 60000.0}}}})[0]["height"];
    CHECK(pass[1].get<double>() < crest - 2000.0);
    // The river runs the valley between the ranges.
    const json water = commandOverMcp("terrainWater", {{"args", {"RiverValley"}}});
    REQUIRE(water["rivers"].size() == 1);
    CHECK(water["rivers"][0]["length"].get<double>() > 150000.0);
    // Both roads reach the pass within their grade.
    const json roads = commandOverMcp("terrainRoads", {{"args", {"RiverValley"}}})["layers"]["Roads"];
    REQUIRE(roads.size() == 2);
    for (const json& road : roads)
    {
        CHECK(road["maxGrade"].get<double>() <= road["design"]["maxGrade"].get<double>() + 0.05);
        const json& line = road["centreline"];
        const json& end = road["id"] == "passUp" ? line.back() : line.front();
        CHECK(std::hypot(end[0].get<double>() - pass[0].get<double>(), end[2].get<double>() - pass[2].get<double>()) < 3000.0);
    }
    // The cave is a volume piece in the south range's cliff.
    const json vol = commandOverMcp("terrainVolume", {{"args", {"RiverValley"}}});
    REQUIRE(vol["ops"].size() == 1);
    CHECK(vol["volume"]["pieces"].size() >= 1);
    CHECK(regions["cliff"]["point"][2].get<double>() < -20000.0);
    // Shipped for Unreal: heights, settings and the cave piece on disk.
    REQUIRE(r["exports"].size() == 1);
    const json files = r["exports"][0]["files"];
    INFO(files.dump());
    CHECK(fs::exists(files["height"].get<std::string>()));
    CHECK(fs::exists(files["settings"].get<std::string>()));
    bool piece = false;
    for (const auto& e : fs::directory_iterator(folder))
        piece = piece || e.path().extension() == ".fbx";
    CHECK(piece);
    run("newScene");
}
