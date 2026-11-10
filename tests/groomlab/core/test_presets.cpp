// SPDX-License-Identifier: Apache-2.0
// Modified for Lila (2026).
// Graph presets (presets.h): the shipped files are found and described, fitted to a scalp size
// as the Maya host's presets.py did (which now fits through this), their graphs load, and a
// preset made from a graph reads back. PresetLooks: what every shipped preset makes on a
// reference groom, against recorded digests (tests/data/golden/PresetLooks.json).

#ifdef _MSC_VER
#define _CRT_SECURE_NO_WARNINGS  // std::getenv, read-only
#endif

#include <gtest/gtest.h>

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <algorithm>
#include <cmath>
#include <set>
#include <sstream>

#include <nlohmann/json.hpp>

#include "groomlab/graph_edit.h"
#include "groomlab/graph_io.h"
#include "groomlab/groom_file.h"
#include "groom/GroomEvaluator.h"
#include "groomlab/presets.h"
#include "groomlab/random.h"
#include "test_helpers.h"

using lila::groom::GroomEvaluator;

namespace groomlab {
namespace {

using json = nlohmann::json;

const std::string kShipped = GROOMLAB_SHIPPED_PRESETS;

// The first param of any operator with a given unit: (type, param).
std::pair<std::string, std::string> paramWithUnit(const std::string& unit) {
    for (const json& op : json::parse(operatorSpecsJson())) {
        for (const json& p : op.value("params", json::array())) {
            if (p.value("unit", std::string()) == unit) return {op.value("type", std::string()), p.value("name", std::string())};
        }
    }
    return {};
}

// An operator's param spec (operatorSpecsJson), or null.
const json* operatorParamSpecForTest(const std::string& type, const std::string& param) {
    static const json specs = json::parse(operatorSpecsJson());
    for (const json& op : specs) {
        if (op.value("type", std::string()) != type) continue;
        for (const json& p : op["params"]) {
            if (p.value("name", std::string()) == param) return &p;
        }
    }
    return nullptr;
}

const json& nodeNamed(const json& graph, const std::string& name) {
    for (const json& n : graph["nodes"]) {
        if (n.value("name", std::string()) == name) return n;
    }
    throw std::runtime_error("no node " + name);
}

const json& nodeOfType(const json& graph, const std::string& type) {
    for (const json& n : graph["nodes"]) {
        if (n.value("type", std::string()) == type) return n;
    }
    throw std::runtime_error("no node of type " + type);
}

}  // namespace

TEST(Presets, ShippedPresetsAreFoundDescribedAndLoad) {
    const std::vector<std::string> dirs = presetSearchDirs({}, kShipped);
    ASSERT_FALSE(dirs.empty());
    EXPECT_EQ(dirs.back(), kShipped);
    const std::vector<PresetInfo> presets = listPresets(dirs);
    ASSERT_GT(presets.size(), 10u);
    bool basics = false;
    for (const PresetInfo& p : presets) {
        EXPECT_FALSE(p.title.empty()) << p.name;
        EXPECT_FALSE(p.description.empty()) << p.name << ": every shipped preset is described for the menu";
        EXPECT_TRUE(std::find(presetCategories().begin(), presetCategories().end(), p.category) != presetCategories().end())
            << p.name << " in category '" << p.category << "'";
        EXPECT_EQ(presetPath(p.name, dirs), p.path);
        // Its graph loads as a groom stores it (the preset's own fields gone, the backdrops kept).
        std::ifstream in(std::filesystem::u8path(p.path));
        std::stringstream text;
        text << in.rdbuf();
        const std::string graphText = presetGraphJson(text.str(), 20.0);
        EXPECT_NO_THROW(loadGraphJson(graphText)) << p.name;
        const json graph = json::parse(graphText);
        for (const std::string& field : presetFields()) {
            if (field != "backdrops") EXPECT_FALSE(graph.contains(field)) << p.name << " keeps " << field;
        }
        if (p.name == "hair_basics") {
            basics = true;
            EXPECT_EQ(p.title, "Hair basics");
            EXPECT_EQ(p.category, "Hair");
        }
    }
    EXPECT_TRUE(basics);
    EXPECT_EQ(presetPath("no_such_preset", dirs), "");
    EXPECT_EQ(presetTitle("wet_hair"), "Wet hair");
    EXPECT_EQ(presetNameError("mine"), "");
    EXPECT_NE(presetNameError("bad/name"), "");
    EXPECT_NE(presetNameError(""), "");
}

TEST(Presets, AnEarlierDirectoryHidesALaterOne) {
    const std::filesystem::path dir = std::filesystem::temp_directory_path() / "groomlab_presets_test";
    std::filesystem::remove_all(dir);
    std::filesystem::create_directories(dir);
    {
        std::ofstream out(dir / "hair_basics.json");
        out << R"({"version": 1, "about": {"title": "Mine", "category": "Hair"}, "nodes": [], "connections": []})";
        std::ofstream broken(dir / "broken.json");
        broken << "not json";
    }
    const std::vector<PresetInfo> presets = listPresets({dir.u8string(), kShipped});
    const auto basics = std::find_if(presets.begin(), presets.end(), [](const PresetInfo& p) { return p.name == "hair_basics"; });
    ASSERT_NE(basics, presets.end());
    EXPECT_EQ(basics->title, "Mine") << "the user's hides the shipped one";
    const auto broken = std::find_if(presets.begin(), presets.end(), [](const PresetInfo& p) { return p.name == "broken"; });
    ASSERT_NE(broken, presets.end());
    EXPECT_EQ(broken->description.rfind("Cannot be read", 0), 0u);
    EXPECT_EQ(broken->title, "Broken");
    std::filesystem::remove_all(dir);
}

TEST(Presets, FittingScalesByUnitMapsIncluded) {
    const auto [op, param] = paramWithUnit("length");
    ASSERT_FALSE(op.empty());
    json graph = {{"size", 1.0}, {"nodes", json::array({{{"name", "n"}, {"type", op}, {"params", {{param, {{"texture", "a.png"}}}}}}})}};
    json fitted = json::parse(fitPresetJson(graph.dump(), 2.0));
    EXPECT_FALSE(fitted.contains("size"));
    // A map without a range: its default max (1) scales too.
    EXPECT_EQ(fitted["nodes"][0]["params"][param], json({{"texture", "a.png"}, {"max", 2.0}}));
    graph["nodes"][0]["params"][param] = {{"texture", "a.png"}, {"min", 0.25}, {"max", 1.5}};
    fitted = json::parse(fitPresetJson(graph.dump(), 2.0));
    EXPECT_EQ(fitted["nodes"][0]["params"][param], json({{"texture", "a.png"}, {"min", 0.5}, {"max", 3.0}}));
    // A number scales; the same size, or none, leaves it; "size" goes either way.
    graph["nodes"][0]["params"][param] = 3.0;
    EXPECT_DOUBLE_EQ(json::parse(fitPresetJson(graph.dump(), 4.0))["nodes"][0]["params"][param].get<double>(), 12.0);
    EXPECT_DOUBLE_EQ(json::parse(fitPresetJson(graph.dump(), 1.0))["nodes"][0]["params"][param].get<double>(), 3.0);
    const json same = json::parse(fitPresetJson(graph.dump(), std::nullopt));
    EXPECT_DOUBLE_EQ(same["nodes"][0]["params"][param].get<double>(), 3.0);
    EXPECT_FALSE(same.contains("size"));
    // A density scales by the inverse square, a per-length rate by the inverse.
    const auto [dop, dparam] = paramWithUnit("density");
    ASSERT_FALSE(dop.empty());
    json dense = {{"size", 1.0}, {"nodes", json::array({{{"name", "d"}, {"type", dop}, {"params", {{dparam, 100.0}}}}})}};
    EXPECT_DOUBLE_EQ(json::parse(fitPresetJson(dense.dump(), 10.0))["nodes"][0]["params"][dparam].get<double>(), 1.0);
    const auto [rop, rparam] = paramWithUnit("perLength");
    ASSERT_FALSE(rop.empty());
    json rate = {{"size", 1.0}, {"nodes", json::array({{{"name", "r"}, {"type", rop}, {"params", {{rparam, 2.0}}}}})}};
    EXPECT_DOUBLE_EQ(json::parse(fitPresetJson(rate.dump(), 4.0))["nodes"][0]["params"][rparam].get<double>(), 0.5);
    EXPECT_THROW(fitPresetJson("[1, 2]", 1.0), std::runtime_error);
}

TEST(Presets, FittingReachesTheFeatherNodesAndKeyedControls) {
    // A feather design's nodes (its Propagate, Noise and Width) have their lengths scaled.
    std::ifstream in(std::filesystem::u8path(presetPath("feathers_contour", {kShipped})));
    std::stringstream text;
    text << in.rdbuf();
    const json design = json::parse(text.str());
    ASSERT_DOUBLE_EQ(design["size"].get<double>(), 2.0);
    const json fitted = json::parse(fitPresetJson(text.str(), 20.0));
    const json& propagate = nodeNamed(fitted, "contourFeather/propagate");
    const json& was = nodeNamed(design, "contourFeather/propagate");
    EXPECT_NEAR(propagate["params"]["length"].get<double>(), was["params"]["length"].get<double>() * 10.0, 1e-9);
    EXPECT_NEAR(propagate["params"]["shaftWidth"].get<double>(), was["params"]["shaftWidth"].get<double>() * 10.0, 1e-9);
    EXPECT_EQ(propagate["params"]["count"], was["params"]["count"]) << "no unit";
    EXPECT_EQ(propagate["params"]["angle"], was["params"]["angle"]);
    EXPECT_NEAR(nodeNamed(fitted, "contourFeather/width")["params"]["width"].get<double>(),
                nodeNamed(design, "contourFeather/width")["params"]["width"].get<double>() * 10.0, 1e-9);
    EXPECT_NEAR(nodeNamed(fitted, "contourFeather/ruffle")["params"]["amplitude"].get<double>(),
                nodeNamed(design, "contourFeather/ruffle")["params"]["amplitude"].get<double>() * 10.0, 1e-9);
    EXPECT_NEAR(nodeOfType(fitted, "Scatter")["params"]["density"].get<double>(), 14.0 / 100.0, 1e-9);

    // A keyed length's control value scales like the length; a keyed count's does not.
    const auto [op, param] = paramWithUnit("length");
    json keyed = {{"size", 1.0},
                  {"controls", {{"ctl_len", 2.0}, {"ctl_other", 5.0}}},
                  {"nodes", json::array({{{"name", "n"}, {"type", op}, {"params", {{param, 1.0}}}, {"paramControls", {{param, "len"}}}}})}};
    const json controls = json::parse(presetControlsJson(keyed.dump(), 3.0));
    EXPECT_DOUBLE_EQ(controls["ctl_len"].get<double>(), 6.0);
    EXPECT_DOUBLE_EQ(controls["ctl_other"].get<double>(), 5.0);
    EXPECT_EQ(presetControlsJson(R"({"nodes": []})", 3.0), "{}");
    // The graph takes the control values it holds itself: a Control node's value, a keyed param.
    json graph = {{"nodes", json::array({{{"name", "c"}, {"type", "Control"}, {"params", {{"name", "amount"}, {"value", 0.1}}}},
                                         {{"name", "n"}, {"type", op}, {"params", {{param, 1.0}}}, {"paramControls", {{param, "len"}}}}})}};
    const json applied = json::parse(applyPresetControlsToGraph(graph.dump(), R"({"ctl_amount": 0.75, "ctl_len": 6.0})"));
    EXPECT_DOUBLE_EQ(nodeNamed(applied, "c")["params"]["value"].get<double>(), 0.75);
    EXPECT_DOUBLE_EQ(nodeNamed(applied, "n")["params"][param].get<double>(), 6.0);
}

TEST(Presets, FittingScalesVectorsConditionalUnitsAndExpressionVariables) {
    // Positions, moves and sizes (vec3 of length) scale on every component, set or at defaults.
    json graph = {{"size", 1.0},
                  {"nodes", json::array({{{"name", "t"}, {"type", "Transform"},
                                          {"params", {{"translate", {1.0, -2.0, 0.5}}, {"rotate", {10.0, 20.0, 30.0}}}}},
                                         {{"name", "g"}, {"type", "Group"}, {"params", {{"by", "box"}, {"center", {0.0, 1.0, 2.0}}}}}})}};
    json fitted = json::parse(fitPresetJson(graph.dump(), 3.0));
    EXPECT_EQ(nodeNamed(fitted, "t")["params"]["translate"], json({3.0, -6.0, 1.5}));
    EXPECT_EQ(nodeNamed(fitted, "t")["params"]["rotate"], json({10.0, 20.0, 30.0})) << "angles stay";
    EXPECT_EQ(nodeNamed(fitted, "g")["params"]["center"], json({0.0, 3.0, 6.0}));
    EXPECT_EQ(nodeNamed(fitted, "g")["params"]["size"], json({3.0, 3.0, 3.0})) << "the default box, fitted";
    EXPECT_FALSE(nodeNamed(fitted, "t")["params"].contains("pivot")) << "a default of zero stays zero: left out";
    // A vector taking an expression in its place stays.
    graph["nodes"][0]["params"]["translate"] = {{"expression", "N * 2"}};
    fitted = json::parse(fitPresetJson(graph.dump(), 3.0));
    EXPECT_EQ(nodeNamed(fitted, "t")["params"]["translate"], json({{"expression", "N * 2"}}));

    // Noise's frequency is per scene unit in world space only: there it scales, per strand length not.
    const json* frequency = operatorParamSpecForTest("Noise", "frequency");
    ASSERT_TRUE(frequency);
    EXPECT_EQ((*frequency)["unit"], "perLength");
    EXPECT_EQ((*frequency)["unitWhen"], json({{"param", "space"}, {"in", {"world"}}}));
    json noise = {{"size", 1.0},
                  {"nodes", json::array({{{"name", "w"}, {"type", "Noise"}, {"params", {{"space", "world"}, {"frequency", 4.0}}}},
                                         {{"name", "s"}, {"type", "Noise"}, {"params", {{"frequency", 4.0}}}}})}};
    fitted = json::parse(fitPresetJson(noise.dump(), 2.0));
    EXPECT_DOUBLE_EQ(nodeNamed(fitted, "w")["params"]["frequency"].get<double>(), 2.0);
    EXPECT_DOUBLE_EQ(nodeNamed(fitted, "s")["params"]["frequency"].get<double>(), 4.0) << "strand space: per strand length";
    EXPECT_DOUBLE_EQ(nodeNamed(fitted, "s")["params"]["amplitude"].get<double>(), 0.4) << "the default amplitude, fitted";
    // The default space (strand) leaves the default frequency as it is on a new node too.
    const json added = json::parse(fitNewNodeParamsJson("Noise", "", kDefaultsScalpSize * 10.0));
    EXPECT_FALSE(added.contains("frequency"));
    EXPECT_DOUBLE_EQ(added["amplitude"].get<double>(), 2.0);
    const json addedWorld = json::parse(fitNewNodeParamsJson("Noise", R"({"space": "world"})", kDefaultsScalpSize * 10.0));
    EXPECT_DOUBLE_EQ(addedWorld["frequency"].get<double>(), 0.3);

    // A field plugged into a param is a factor of the param's own number (field.h): its range stays.
    json masked = {{"size", 1.0},
                   {"nodes", json::array({{{"name", "m"}, {"type", "Map"}, {"params", {{"name", "mask"}, {"min", 0.2}, {"max", 0.9}}}},
                                          {{"name", "d"}, {"type", "Displace"}, {"params", {{"distance", 0.5}}}, {"paramInputs", {"distance"}}}})},
                   {"connections", json::array({{{"from", "m"}, {"to", "d"}, {"input", "distance"}}})}};
    fitted = json::parse(fitPresetJson(masked.dump(), 4.0));
    EXPECT_DOUBLE_EQ(nodeNamed(fitted, "m")["params"]["max"].get<double>(), 0.9);
    EXPECT_DOUBLE_EQ(nodeNamed(fitted, "d")["params"]["distance"].get<double>(), 2.0);

    // An Expression's variables that say their unit fit by it (a keyable one's control value too).
    const json variables = {{"radius", {{"value", 2.0}, {"unit", "length"}}},
                            {"amount", 0.5},
                            {"center", {0.0, 1.0, 0.0}},
                            {"reach", {{"value", 1.0}, {"unit", "length"}, {"control", "reach"}}}};
    json expression = {{"size", 1.0},
                       {"controls", {{"ctl_reach", 1.5}}},
                       {"nodes", json::array({{{"name", "e"}, {"type", "Expression"},
                                               {"params", {{"expression", "$radius"}, {"variables", variables.dump()}}}}})}};
    fitted = json::parse(fitPresetJson(expression.dump(), 10.0));
    const json fittedVariables = json::parse(nodeNamed(fitted, "e")["params"]["variables"].get<std::string>());
    EXPECT_DOUBLE_EQ(fittedVariables["radius"]["value"].get<double>(), 20.0);
    EXPECT_DOUBLE_EQ(fittedVariables["amount"].get<double>(), 0.5);
    EXPECT_EQ(fittedVariables["center"], json({0.0, 1.0, 0.0})) << "no unit said";
    EXPECT_DOUBLE_EQ(json::parse(presetControlsJson(expression.dump(), 10.0))["ctl_reach"].get<double>(), 15.0);
}

TEST(Presets, NewNodesAndTheDefaultGraphFitTheScalp) {
    // A node added on a groom takes its defaults fitted from the 20-unit head they are authored at.
    const json scatter = json::parse(fitNewNodeParamsJson("Scatter", "", 200.0));  // a 2 m creature, in cm
    EXPECT_DOUBLE_EQ(scatter["density"].get<double>(), 0.8);
    EXPECT_DOUBLE_EQ(scatter["length"].get<double>(), 10.0);
    EXPECT_FALSE(scatter.contains("seed")) << "no unit: left to its default";
    const json given = json::parse(fitNewNodeParamsJson("Scatter", R"({"length": 3})", 200.0));
    EXPECT_DOUBLE_EQ(given["length"].get<double>(), 3.0) << "what is given stays";
    EXPECT_EQ(fitNewNodeParamsJson("Scatter", "", kDefaultsScalpSize), "{}");
    EXPECT_EQ(fitNewNodeParamsJson("Scatter", "", std::nullopt), "{}");
    GraphEdit edit(defaultGraphJson());
    const std::string made = edit.addNode("Width", "", "", std::nullopt, std::nullopt, 2.0);
    EXPECT_NEAR(json::parse(edit.toJson())["nodes"].back()["params"]["width"].get<double>(), 0.0007, 1e-12) << made;

    // The default graph a new groom stores: its nodes' defaults fitted the same way.
    const json small = json::parse(defaultGraphJsonFor(2.0));
    EXPECT_NEAR(nodeOfType(small, "Scatter")["params"]["density"].get<double>(), 8000.0, 1e-6);
    EXPECT_NEAR(nodeOfType(small, "Scatter")["params"]["length"].get<double>(), 0.1, 1e-9);
    EXPECT_NEAR(nodeOfType(small, "Noise")["params"]["amplitude"].get<double>(), 0.02, 1e-9);
    EXPECT_NEAR(nodeOfType(small, "Width")["params"]["width"].get<double>(), 0.0007, 1e-12);
    EXPECT_DOUBLE_EQ(nodeOfType(small, "Length")["params"]["randomness"].get<double>(), 0.2) << "its own params as they are";
    EXPECT_EQ(json::parse(defaultGraphJsonFor(std::nullopt)), json::parse(defaultGraphJson()));
    EXPECT_EQ(json::parse(defaultGraphJsonFor(kDefaultsScalpSize)), json::parse(defaultGraphJson()));
}

TEST(Presets, RequiredMapsAndMakingOne) {
    std::ifstream in(std::filesystem::u8path(presetPath("painted_maps", {kShipped})));
    std::stringstream text;
    text << in.rdbuf();
    const std::vector<PresetMap> maps = presetRequiredMaps(text.str());
    ASSERT_EQ(maps.size(), 1u);
    EXPECT_EQ(maps[0].name, "groomMasks");
    EXPECT_EQ(maps[0].kind, "vertices");
    EXPECT_EQ(maps[0].color.size(), 3u);
    EXPECT_TRUE(presetRequiredMaps(R"({"nodes": []})").empty());

    // The guide groups a preset reads: the hand-placed flight feathers' (made empty, drawn into).
    std::ifstream handPlaced(std::filesystem::u8path(presetPath("hand_placed_feathers", {kShipped})));
    std::stringstream handText;
    handText << handPlaced.rdbuf();
    const std::vector<PresetGroup> groups = presetRequiredGroups(handText.str());
    ASSERT_EQ(groups.size(), 1u);
    EXPECT_EQ(groups[0].name, "feathers");
    EXPECT_TRUE(groups[0].draw);
    EXPECT_TRUE(presetRequiredMaps(handText.str()).empty());
    EXPECT_TRUE(presetRequiredGroups(text.str()).empty());
    const std::vector<PresetGroup> some =
        presetRequiredGroups(R"({"requires": {"groups": [{"name": "tail"}, {"name": ""}, {"name": "tail", "draw": true}, 3]}, "nodes": []})");
    ASSERT_EQ(some.size(), 1u) << "named once, nameless entries skipped";
    EXPECT_EQ(some[0].name, "tail");
    EXPECT_FALSE(some[0].draw);

    // A preset made from the default graph: titled, sized, the layout kept, and it loads again.
    json graph = json::parse(defaultGraphJson());
    graph["backdrops"] = json::array({{{"title", "All"}, {"x", 0}, {"y", 0}, {"w", 100}, {"h", 100}}});
    const std::string made = makePresetJson(graph.dump(), 20.0, "Mine", "  what it is ", "Hair", R"({"ctl_a": 1})");
    const json preset = json::parse(made);
    EXPECT_EQ(made.find("\"version\""), made.find_first_of('"')) << "the version comes first";
    EXPECT_DOUBLE_EQ(preset["size"].get<double>(), 20.0);
    EXPECT_EQ(preset["about"]["title"], "Mine");
    EXPECT_EQ(preset["about"]["description"], "what it is");
    EXPECT_EQ(preset["about"]["category"], "Hair");
    EXPECT_EQ(preset["backdrops"].size(), 1u);
    EXPECT_EQ(preset["controls"]["ctl_a"], 1);
    const json back = json::parse(presetGraphJson(made, 20.0));
    EXPECT_EQ(back["nodes"].size(), graph["nodes"].size());
    EXPECT_EQ(back["backdrops"].size(), 1u);
    EXPECT_FALSE(back.contains("about"));
    EXPECT_NO_THROW(loadGraphJson(back.dump()));
    // No about, no size: neither is written.
    const json bare = json::parse(makePresetJson(defaultGraphJson(), std::nullopt, "", "", ""));
    EXPECT_FALSE(bare.contains("about"));
    EXPECT_FALSE(bare.contains("size"));

    // The scalp size a preset records: the largest extent of the mesh.
    const auto plane = test::makePlane(2, 3.0f);
    ASSERT_TRUE(presetScalpSize(*plane));
    EXPECT_FLOAT_EQ(static_cast<float>(*presetScalpSize(*plane)), 3.0f);
}

std::string readPreset(const std::string& name) {
    std::ifstream in(std::filesystem::u8path(presetPath(name, {kShipped})));
    std::stringstream text;
    text << in.rdbuf();
    return text.str();
}



TEST(Presets, SearchDirsFollowTheEnvironment) {
#ifdef _WIN32
    _putenv_s("GROOMLAB_PRESET_PATH", "C:/a;C:/b");
    _putenv_s("GROOMLAB_USER_PRESETS", "C:/mine");
#else
    setenv("GROOMLAB_PRESET_PATH", "/a:/b", 1);
    setenv("GROOMLAB_USER_PRESETS", "/mine", 1);
#endif
    const std::vector<std::string> dirs = presetSearchDirs(presetUserDir(), "shipped");
    ASSERT_EQ(dirs.size(), 4u);
#ifdef _WIN32
    EXPECT_EQ(dirs[0], "C:/a");
    EXPECT_EQ(dirs[1], "C:/b");
    EXPECT_EQ(dirs[2], "C:/mine");
    _putenv_s("GROOMLAB_PRESET_PATH", "");
    _putenv_s("GROOMLAB_USER_PRESETS", "");
#else
    EXPECT_EQ(dirs[0], "/a");
    EXPECT_EQ(dirs[2], "/mine");
    unsetenv("GROOMLAB_PRESET_PATH");
    unsetenv("GROOMLAB_USER_PRESETS");
#endif
    EXPECT_EQ(dirs[3], "shipped");
    // Without the variables: the user's Maya folder (when a home is known), then the shipped one.
    const std::vector<std::string> plain = presetSearchDirs(presetUserDir(), "shipped");
    EXPECT_EQ(plain.back(), "shipped");
    if (plain.size() == 2) EXPECT_NE(plain[0].find("groomlab"), std::string::npos);
}

// ---------------------------------------------------------------------------
// Preset looks: every shipped preset evaluated on a reference groom (the strokes fixture's scalp
// and guides, the preset fitted to its size, the maps it requires painted) at a low density,
// against digests recorded in tests/data/golden/PresetLooks.json, so a change that alters what a
// preset makes (a default, an operator's behaviour, the preset itself) is flagged. Re-record an intended change with
// `python tools/dev/record_preset_looks.py` (GROOMLAB_UPDATE_GOLDEN=1 on this test) and review the
// diff.

namespace {

const std::filesystem::path kPresetLooks = std::filesystem::path(GROOMLAB_TEST_DATA) / "golden" / "PresetLooks.json";
constexpr float kPresetLookDensity = 0.05f;

bool updatingGoldens() {
    const char* v = std::getenv("GROOMLAB_UPDATE_GOLDEN");
    return v && *v && std::string(v) != "0";
}

// What a preset made, in a form that survives float noise: counts and IDs exactly, the points'
// sums and bounds, the widths' sum and the channels' names (and each channel's sum).
json presetLook(const StrandSet& s) {
    std::uint64_t ids = hash64(s.strandCount());
    for (const StrandId id : s.ids()) ids = hashCombine(ids, id);
    double sum[3] = {0, 0, 0};
    float lo[3] = {1e30f, 1e30f, 1e30f}, hi[3] = {-1e30f, -1e30f, -1e30f};
    std::size_t points = 0;
    for (std::size_t i = 0; i < s.strandCount(); ++i) {
        const Vec3f* p = s.strandPoints(i);
        for (std::size_t k = 0; k < s.strandPointCount(i); ++k, ++points) {
            const float xyz[3] = {p[k].x, p[k].y, p[k].z};
            for (int a = 0; a < 3; ++a) {
                sum[a] += xyz[a];
                lo[a] = std::min(lo[a], xyz[a]);
                hi[a] = std::max(hi[a], xyz[a]);
            }
        }
    }
    double widths = 0.0;
    for (const float w : s.widths()) widths += w;
    json channels = json::object();
    for (const std::string& name : s.strandChannelNames()) {
        double total = 0.0;
        for (const float v : *s.strandChannel(name)) total += v;
        channels[name] = total;
    }
    return {{"strands", s.strandCount()},
            {"points", points},
            {"ids", std::to_string(ids)},
            {"sum", {sum[0], sum[1], sum[2]}},
            {"lo", {lo[0], lo[1], lo[2]}},
            {"hi", {hi[0], hi[1], hi[2]}},
            {"widths", widths},
            {"channels", channels}};
}

// Equal within a relative tolerance (float sums in another order, a parallel pass).
void expectNear(const json& got, const json& want, const std::string& what) {
    if (got.is_number() && want.is_number()) {
        const double g = got.get<double>(), w = want.get<double>();
        EXPECT_NEAR(g, w, 1e-4 * std::max(1.0, std::abs(w))) << what;
    } else if (got.is_array() && want.is_array() && got.size() == want.size()) {
        for (std::size_t i = 0; i < got.size(); ++i) expectNear(got[i], want[i], what + "[" + std::to_string(i) + "]");
    } else if (got.is_object() && want.is_object()) {
        EXPECT_EQ(got.size(), want.size()) << what << ": " << got.dump() << " vs " << want.dump();
        for (const auto& [key, value] : want.items()) {
            if (got.contains(key)) expectNear(got.at(key), value, what + "." + key);
            else ADD_FAILURE() << what << ": no " << key;
        }
    } else {
        EXPECT_EQ(got, want) << what;
    }
}

}  // namespace

TEST(PresetLooks, EveryShippedPresetMakesWhatItMadeBefore) {
    const GroomFile reference = readGroomFile(std::string(GROOMLAB_TEST_DATA) + "/strokes/strokes.groom");
    ASSERT_TRUE(reference.data.scalp);
    const std::optional<double> size = presetScalpSize(*reference.data.scalp);
    json looks = json::object();
    for (const PresetInfo& p : listPresets({kShipped})) {
        std::ifstream in(std::filesystem::u8path(p.path));
        std::stringstream text;
        text << in.rdbuf();
        GroomData data = reference.data;
        data.graphJson = presetGraphJson(text.str(), size);
        test::paintRequiredMaps(text.str(), data);  // the maps a host makes on applying it, painted
        try {
            GroomEvaluator groom(data);
            groom.setPathResolver(searchPathResolver({reference.directory, kShipped}));
            const StrandsPtr made = groom.evaluate(kPresetLookDensity, 0.0);
            looks[p.name] = made ? presetLook(*made) : json{{"error", "nothing made"}};
        } catch (const std::exception& e) {
            looks[p.name] = {{"error", e.what()}};
        }
    }
    ASSERT_GT(looks.size(), 10u);
    if (updatingGoldens()) std::ofstream(kPresetLooks, std::ios::binary) << looks.dump(1) << "\n";
    std::ifstream in(kPresetLooks, std::ios::binary);
    ASSERT_TRUE(in) << "no " << kPresetLooks.generic_string() << " (run python tools/dev/record_preset_looks.py)";
    const json recorded = json::parse(in);
    for (const auto& [name, look] : looks.items()) {
        if (!recorded.contains(name)) {
            ADD_FAILURE() << "preset " << name << " has no recorded look (run python tools/dev/record_preset_looks.py)";
            continue;
        }
        expectNear(look, recorded.at(name), name);
    }
    for (const auto& [name, look] : recorded.items()) {
        EXPECT_TRUE(looks.contains(name)) << "preset " << name << " is recorded but no longer shipped: re-record";
    }
}

// ---- A preset's look as one Compound shows the look's key params ("exposed") -------------------

namespace {

// An operator's param spec (operatorSpecsJson), or null.
const json* operatorParamSpec(const std::string& type, const std::string& param) {
    static const json specs = json::parse(operatorSpecsJson());
    for (const json& op : specs) {
        if (op.value("type", std::string()) != type) continue;
        for (const json& p : op["params"]) {
            if (p.value("name", std::string()) == param) return &p;
        }
    }
    return nullptr;
}

json& mutableNode(json& graph, const std::string& name) {
    for (json& n : graph["nodes"]) {
        if (n.value("name", std::string()) == name) return n;
    }
    throw std::runtime_error("no node " + name);
}

// A value of the param's kind other than `value`, within its limits: a number at 0.7 of it, a
// vector's components too, a switch flipped, a choice the next one.
json editedValue(const json& spec, const json& value) {
    const std::string kind = spec.value("kind", std::string());
    if (kind == "bool") return !value.get<bool>();
    if (kind == "vec3") {
        json out = json::array();
        for (const json& c : value) out.push_back(c.get<double>() * 0.7 + 0.05);
        return out;
    }
    if (kind == "string") {
        for (const json& choice : spec.value("choices", json::array())) {
            if (choice != value) return choice;
        }
        return value;
    }
    double v = value.get<double>() != 0.0 ? value.get<double>() * 0.7 : spec.value("softMax", spec.value("max", 1.0)) * 0.1;
    if (spec.contains("min")) v = std::max(v, spec["min"].get<double>());
    if (spec.contains("max")) v = std::min(v, spec["max"].get<double>());
    if (kind == "int") {
        std::int64_t i = std::llround(v);
        if (i == value.get<std::int64_t>()) i += 1;
        return i;
    }
    return v;
}

// The look the reference groom makes from a graph (presetLook), or the error it gives.
json lookOf(const GroomFile& reference, const std::string& graphJson) {
    GroomData data = reference.data;
    data.graphJson = graphJson;
    try {
        GroomEvaluator groom(data);
        groom.setPathResolver(searchPathResolver({reference.directory, kShipped}));
        const StrandsPtr made = groom.evaluate(kPresetLookDensity, 0.0);
        return made ? presetLook(*made) : json{{"error", "nothing made"}};
    } catch (const std::exception& e) {
        return {{"error", e.what()}};
    }
}

}  // namespace




}  // namespace groomlab
