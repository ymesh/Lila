// Live USD stages: a usdStage node hosting a composed stage, with Lila's edits written as overs to its edit layer.

#include "UsdTestUtil.h"

#include "io/FileTranslators.h"
#include "io/SceneFile.h"
#include "scene/Anim.h"
#include "scene/MeshCache.h"
#include "scene/Modifiers.h"
#include "scene/Skin.h"
#include "ops/Undo.h"

#include <chrono>
#include <format>
#include <thread>

using namespace lila;
using namespace lila::scene;
using namespace lila::usdtest;

namespace
{
fs::path workDir(const std::string& name)
{
    const fs::path dir = tempDir() / "stage" / name;
    std::error_code ec;
    fs::remove_all(dir, ec);
    fs::create_directories(dir);
    return dir;
}

fs::path copyFixture(const fs::path& dir, const char* name)
{
    fs::copy_file(fixture(name), dir / name, fs::copy_options::overwrite_existing);
    return dir / name;
}

void writeFile(const fs::path& p, const std::string& text)
{
    // A later write has to look newer on disk, however fast the test runs.
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
    std::ofstream f(p, std::ios::binary | std::ios::trunc);
    f << text;
}

std::string replaced(std::string text, const std::string& from, const std::string& to)
{
    const size_t at = text.find(from);
    REQUIRE_MESSAGE(at != std::string::npos, from);
    return text.replace(at, from.size(), to);
}

json openStage(const fs::path& p, json flags = json::object())
{
    flags["args"] = {p.generic_string()};
    flags["report"] = true;
    return run("usdStageOpen", flags);
}

json info(const std::string& stage) { return run("usdStageInfo", {{"args", {stage}}, {"edits", true}}); }
std::string edits(const std::string& stage) { return info(stage)["edits"].get<std::string>(); }

void saveAs(const fs::path& p) { run("saveScene", {{"args", {p.generic_string()}}}); }

void reopen(const fs::path& p)
{
    run("newScene");
    run("openScene", {{"args", {p.generic_string()}}, {"force", true}});
}

/** The composed value of a prim property, as usdStagePrim shows it; empty when it has none. */
std::string prop(const std::string& object, const std::string& name)
{
    const json r = run("usdStagePrim", {{"args", {object}}});
    for (const json& p : r["properties"])
        if (p["name"] == name)
        {
            if (p.contains("targets"))
                return p["targets"].dump();
            return p["value"].is_string() ? p["value"].get<std::string>() : std::string();
        }
    return {};
}

void setTranslate(const std::string& name, const glm::dvec3& v) { graph().setValue({node(name).id(), Transform::kTranslate, -1}, v); }
glm::dvec3 translateOf(const std::string& name) { return node(name).as<glm::dvec3>(Transform::kTranslate); }

bool warned(const json& list, const std::string& part)
{
    for (const json& w : list)
        if (has(w.get<std::string>(), part))
            return true;
    return false;
}
} // namespace

TEST_CASE("usd stage: opens live under a usdStage node, and saving without edits writes nothing")
{
    UpAxisScope axis(math::UpAxis::Z);
    fresh();
    const fs::path dir = workDir("open");
    const fs::path src = copyFixture(dir, "live.usda");
    const std::string before = readAll(src);
    json r = openStage(src);
    INFO(r.dump());
    const std::string stage = r["stage"];
    CHECK(node(stage).type().name == "usdStage");
    CHECK(graph().find("live|World|Box|Geo"));
    CHECK(graph().find("World|Lamp"));
    CHECK(r["prims"].get<int>() >= 6);
    CHECK(near(translateOf("Box"), {10, 0, 0}, 1e-9));
    CHECK(prop("Lamp", "studio:assetId") == "\"LA_001\"");
    const json i = info(stage);
    CHECK(i["editLayer"]["mode"] == "embedded");
    CHECK(i["upAxis"] == "Z");

    saveAs(dir / "scene.lila");
    CHECK(!has(edits(stage), "over"));
    CHECK(!has(edits(stage), "def"));
    CHECK(readAll(src) == before);

    // File > Import of a USD file makes a live stage too.
    fresh();
    io::translators::run(io::translators::Op::Import, src);
    bool found = false;
    for (NodeId id : graph().nodes())
        found = found || graph().node(id)->type().name == "usdStage";
    CHECK(found);
}

TEST_CASE("usd stage: time-sampled points play through a mesh cache, and saving without edits writes nothing")
{
    UpAxisScope axis(math::UpAxis::Z);
    fresh();
    const fs::path dir = workDir("pointcache");
    const fs::path src = copyFixture(dir, "pointcache.usda");
    const std::string before = readAll(src);
    json r = openStage(src);
    INFO(r.dump());
    const std::string stage = r["stage"];
    const NodeId cache = meshCache::find(graph(), node("Flag").id());
    REQUIRE(cache != kNoNode);
    graph().setTime(0.5);
    MeshPtr m = outMesh("Flag");
    REQUIRE(m);
    double top = 0.0;
    for (const glm::dvec3& p : m->positions())
        top = std::max(top, p.z);
    CHECK(top == doctest::Approx(4.0));
    graph().setTime(0.0);

    saveAs(dir / "scene.lila");
    CHECK(!has(edits(stage), "over"));
    CHECK(readAll(src) == before);
    reopen(dir / "scene.lila");
    CHECK(meshCache::find(graph(), node("Flag").id()) != kNoNode);
    CHECK(meshCache::samples(graph(), meshCache::find(graph(), node("Flag").id()))->sampleCount() == 3);
}

TEST_CASE("usd stage: transforms and their animation are written as overs, survive a reopen and go out in a shot")
{
    UpAxisScope axis(math::UpAxis::Z);
    fresh();
    const fs::path dir = workDir("xform");
    const fs::path src = copyFixture(dir, "live.usda");
    const std::string before = readAll(src);
    const std::string stage = openStage(src)["stage"];

    setTranslate("Box", {30, 0, 5});
    anim::setKey(graph(), {node("Chair").id(), Transform::kTranslate, 0}, 0.0, 0.0);
    anim::setKey(graph(), {node("Chair").id(), Transform::kTranslate, 0}, 1.0, 50.0);
    saveAs(dir / "scene.lila");
    const std::string text = edits(stage);
    INFO(text);
    CHECK(has(text, "over \"Box\""));
    CHECK(has(text, "(30, 0, 5)"));
    CHECK(has(text, "xformOp:translate.timeSamples"));
    CHECK(!has(text, "over \"Lamp\""));
    CHECK(prop("Box", "xformOp:translate") == "(30, 0, 5)");
    CHECK(readAll(src) == before);

    reopen(dir / "scene.lila");
    CHECK(near(translateOf("Box"), {30, 0, 5}, 1e-9));
    CHECK(prop("Box", "xformOp:translate") == "(30, 0, 5)");
    graph().setTime(1.0);
    graph().evaluate();
    CHECK(node("Chair").as<glm::dvec3>(Transform::kTranslate).x == doctest::Approx(50.0));
    graph().setTime(0.0);

    // A shot layers the edits over the source; opened as a stage of its own it shows them.
    const fs::path shot = dir / "shot.usda";
    run("usdStageWriteShot", {{"args", {shot.generic_string(), stage}}});
    const std::string shotText = readAll(shot);
    CHECK(has(shotText, "subLayers = [\n        @./live.usda@"));
    CHECK(has(shotText, "upAxis = \"Z\""));
    fresh();
    openStage(shot);
    CHECK(near(translateOf("Box"), {30, 0, 5}, 1e-9));
    CHECK(near(translateOf("Lamp"), {0, 20, 0}, 1e-9));
    CHECK(readAll(src) == before);
}

TEST_CASE("usd stage: an added layer composes under the edits, takes outside changes, goes out in a shot and comes out again")
{
    UpAxisScope axis(math::UpAxis::Z);
    fresh();
    const fs::path dir = workDir("layers");
    const fs::path src = copyFixture(dir, "live.usda");
    const std::string before = readAll(src);
    const std::string stage = openStage(src)["stage"];
    const fs::path layer = dir / "lighting.usda";

    const json added = run("usdStageAddLayer", {{"args", {layer.generic_string(), stage}}});
    REQUIRE(added.size() == 1);
    CHECK(fs::exists(layer));
    CHECK(has(readAll(layer), "upAxis = \"Z\""));
    CHECK(info(stage)["addedLayers"] == added);

    // Another tool writes into it: its opinion beats the source.
    writeFile(layer, "#usda 1.0\n(\n    metersPerUnit = 0.01\n    upAxis = \"Z\"\n)\n\nover \"World\"\n{\n    over \"Box\"\n    {\n"
                     "        double3 xformOp:translate = (40, 0, 0)\n    }\n}\n");
    run("usdStageUpdate");
    CHECK(near(translateOf("Box"), {40, 0, 0}, 1e-9));
    // Lila's edits still win over it.
    setTranslate("Box", {50, 0, 0});
    saveAs(dir / "scene.lila");
    CHECK(prop("Box", "xformOp:translate") == "(50, 0, 0)");
    const json opinions = run("usdStageOpinions", {{"args", {"Box"}}});
    INFO(opinions.dump());
    CHECK(opinions["specs"][0]["specifier"] == "over");
    CHECK(opinions["specs"].back()["specifier"] == "def");
    for (const json& p : opinions["properties"])
        if (p["name"] == "xformOp:translate")
        {
            const json& stack = p["opinions"];
            REQUIRE(stack.size() == 3);
            CHECK(stack[0]["role"] == "edits");
            CHECK(stack[0]["wins"] == true);
            CHECK(stack[0]["arc"] == "local");
            CHECK(stack[1]["role"] == "added");
            CHECK(stack[1]["value"] == "(40, 0, 0)");
            CHECK(!stack[1].contains("wins"));
            CHECK(fs::path(stack[2]["layer"].get<std::string>()).filename() == "live.usda");
            CHECK(!p.contains("losesTo"));
        }

    const fs::path shot = dir / "shot.usda";
    run("usdStageWriteShot", {{"args", {shot.generic_string(), stage}}});
    CHECK(has(readAll(shot), "@./lighting.usda@,\n        @./live.usda@"));

    reopen(dir / "scene.lila");
    CHECK(info(stage)["addedLayers"].size() == 1);
    setTranslate("Box", {10, 0, 0});
    run("usdStageRemoveLayer", {{"args", {layer.generic_string(), stage}}});
    CHECK(info(stage)["addedLayers"].empty());
    CHECK(fs::exists(layer));
    run("undo");
    CHECK(info(stage)["addedLayers"].size() == 1);
    CHECK(readAll(src) == before);
}

TEST_CASE("usd stage: hiding and deleting objects, and an undone delete brings the prim back")
{
    UpAxisScope axis(math::UpAxis::Z);
    fresh();
    const fs::path dir = workDir("delete");
    const fs::path src = copyFixture(dir, "live.usda");
    const std::string stage = openStage(src)["stage"];

    graph().setValue({node("Lamp").id(), Transform::kVisibility, -1}, false);
    run("delete", {{"args", {"Chair"}}});
    saveAs(dir / "scene.lila");
    std::string text = edits(stage);
    INFO(text);
    CHECK(has(text, "token visibility = \"invisible\""));
    CHECK(has(text, "active = false"));
    CHECK(prop("Lamp", "visibility") == "\"invisible\"");

    run("undo");
    REQUIRE(graph().find("Chair"));
    saveAs(dir / "scene.lila");
    text = edits(stage);
    CHECK(!has(text, "active = false"));
    CHECK(prop("Chair", "xformOp:translate") == "(0, -20, 0)");

    // The stage node itself deleted and brought back keeps its stage.
    run("delete", {{"args", {stage}}});
    saveAs(dir / "scene.lila");
    run("undo");
    REQUIRE(graph().find(stage));
    CHECK(prop("Lamp", "visibility") == "\"invisible\"");

    run("delete", {{"args", {"Chair"}}});
    graph().setValue({node("Lamp").id(), Transform::kVisibility, -1}, true);
    saveAs(dir / "scene.lila");
    CHECK(prop("Lamp", "visibility") == "\"inherited\"");
    reopen(dir / "scene.lila");
    CHECK_FALSE(graph().find("Chair"));
    CHECK(node("Lamp").as<bool>(Transform::kVisibility));
}

TEST_CASE("usd stage: a mesh edit rewrites the mesh, blocks stale primvars and replaces the subsets")
{
    UpAxisScope axis(math::UpAxis::Z);
    fresh();
    const fs::path dir = workDir("mesh");
    const fs::path src = copyFixture(dir, "live.usda");
    const std::string stage = openStage(src)["stage"];

    geo::Mesh m = geo::makeSphere({.radius = 3, .subdivAxis = 8, .subdivHeight = 4});
    const size_t points = m.numPoints();
    graph().setValue({node("Geo").id(), MeshNode::kInMesh, -1}, MeshPtr(std::make_shared<geo::Mesh>(std::move(m))));
    saveAs(dir / "scene.lila");
    const std::string text = edits(stage);
    INFO(text);
    CHECK(has(text, "point3f[] points = ["));
    CHECK(has(text, "string primvars:tag = None"));
    CHECK(has(text, "over \"Top\""));
    CHECK(has(text, "over \"Bottom\""));
    CHECK(prop("Geo", "primvars:tag").empty());

    reopen(dir / "scene.lila");
    CHECK(outMesh("Geo")->numPoints() == points);
    run("usdStageReload", {{"args", {stage}}});
    CHECK(outMesh("Geo")->numPoints() == points);
}

TEST_CASE("usd stage: material edits go over their material prims, new materials are written, assignments rebind")
{
    UpAxisScope axis(math::UpAxis::Z);
    fresh();
    const fs::path dir = workDir("materials");
    const fs::path src = copyFixture(dir, "live.usda");
    const std::string stage = openStage(src)["stage"];
    REQUIRE(materials::find(graph(), "Red"));

    graph().setValue({materials::find(graph(), "Red")->id(), material::kBaseColor, -1}, glm::dvec3(0, 0, 1));
    saveAs(dir / "scene.lila");
    std::string text = edits(stage);
    INFO(text);
    CHECK(has(text, "def Material \"Red\""));
    CHECK(has(text, "(0, 0, 1)"));
    CHECK(!has(text, "\"Green\""));

    // A Lila material assigned to the whole mesh is written beside the stage's own and bound.
    Node* blue = materials::create(graph(), "Blue");
    graph().setValue({blue->id(), material::kBaseColor, -1}, glm::dvec3(0.1, 0.2, 0.9));
    geo::Mesh m = *outMesh("Geo");
    m.faceAttrs().remove(geo::attr::kMaterial);
    m.materialNames().clear();
    materials::assign(m, "Blue", {});
    graph().setValue({node("Geo").id(), MeshNode::kInMesh, -1}, MeshPtr(std::make_shared<geo::Mesh>(std::move(m))));
    saveAs(dir / "scene.lila");
    text = edits(stage);
    CHECK(has(text, "def Scope \"LilaMaterials\""));
    CHECK(has(text, "def Material \"Blue\""));
    CHECK(prop("Geo", "material:binding") == "[\"/World/LilaMaterials/Blue\"]");

    reopen(dir / "scene.lila");
    run("usdStageReload", {{"args", {stage}}});
    CHECK(materials::find(graph(), "Blue"));
    CHECK(near(materials::find(graph(), "Red")->as<glm::dvec3>(material::kBaseColor), {0, 0, 1}, 1e-6));
}

TEST_CASE("usd stage: new objects under the stage become prims, renames keep the link")
{
    UpAxisScope axis(math::UpAxis::Z);
    fresh();
    const fs::path dir = workDir("new");
    const fs::path src = copyFixture(dir, "live.usda");
    const std::string stage = openStage(src)["stage"];

    Node* added = addMesh("Pebble", geo::makeCube({}), node("World").id());
    graph().setValue({added->id(), Transform::kTranslate, -1}, glm::dvec3(1, 2, 3));
    graph().rename(node("Lamp").id(), "Light");
    setTranslate("Light", {0, 25, 0});
    saveAs(dir / "scene.lila");
    const json i = info(stage);
    const std::string text = i["edits"];
    INFO(text);
    CHECK(has(text, "def Mesh \"Pebble\""));
    CHECK(prop("Pebble", "xformOp:translate") == "(1, 2, 3)");
    CHECK(has(text, "over \"Lamp\""));
    CHECK(prop("Light", "xformOp:translate") == "(0, 25, 0)");
    CHECK(i["warnings"].empty());

    // A new object deleted leaves nothing behind; undone, it is written again.
    run("delete", {{"args", {"Pebble"}}});
    saveAs(dir / "scene.lila");
    CHECK(!has(edits(stage), "\"Pebble\""));
    run("undo");
    saveAs(dir / "scene.lila");
    CHECK(prop("Pebble", "xformOp:translate") == "(1, 2, 3)");

    reopen(dir / "scene.lila");
    CHECK(prop("Pebble", "xformOp:translate") == "(1, 2, 3)");
    CHECK(run("usdStagePrim", {{"args", {"Light"}}})["prim"] == "/World/Lamp");
    run("usdStageReload", {{"args", {stage}}});
    CHECK(near(translateOf("Pebble"), {1, 2, 3}, 1e-6));
}

TEST_CASE("usd stage: variants and payloads switch live and are kept")
{
    UpAxisScope axis(math::UpAxis::Z);
    fresh();
    const fs::path dir = workDir("variants");
    const fs::path src = copyFixture(dir, "composition.usda");
    copyFixture(dir, "part.usda");
    copyFixture(dir, "sub.usda");
    const std::string before = readAll(src);
    const std::string stage = openStage(src, {{"loadPayloads", false}})["stage"];
    CHECK(graph().find("Chair|Seat"));
    CHECK_FALSE(graph().find("Payloaded|Box"));
    CHECK(graph().find("Referenced|Box"));

    run("usdStageVariant", {{"args", {stage}}, {"set", "style"}, {"variant", "metal"}});
    CHECK(graph().find("Chair|Frame"));
    CHECK_FALSE(graph().find("Chair|Seat"));
    run("usdStageLoad", {{"args", {stage}}, {"prim", "/Root/Payloaded"}});
    CHECK(graph().find("Payloaded|Box"));
    json i = info(stage);
    INFO(i.dump());
    REQUIRE(i["variants"].size() == 1);
    CHECK(i["variants"][0]["selection"] == "metal");
    REQUIRE(i["payloads"].size() == 1);
    CHECK(i["payloads"][0]["loaded"] == true);

    saveAs(dir / "scene.lila");
    reopen(dir / "scene.lila");
    CHECK(graph().find("Chair|Frame"));
    CHECK(graph().find("Payloaded|Box"));

    // Undoing a switch brings the old variant's objects back and takes the selection out of the edit layer.
    run("usdStageVariant", {{"args", {stage}}, {"set", "style"}, {"variant", "wood"}});
    CHECK(graph().find("Chair|Seat"));
    run("undo");
    CHECK(graph().find("Chair|Frame"));
    CHECK_FALSE(graph().find("Chair|Seat"));
    CHECK(info(stage)["variants"][0]["selection"] == "metal");
    CHECK(readAll(src) == before);
}

TEST_CASE("usd stage: the edit layer as a .usda file, and as one of the stage's sublayers")
{
    UpAxisScope axis(math::UpAxis::Z);
    fresh();
    const fs::path dir = workDir("editlayer");
    const fs::path src = copyFixture(dir, "composition.usda");
    copyFixture(dir, "part.usda");
    const fs::path sub = copyFixture(dir, "sub.usda");
    const std::string before = readAll(src);
    const fs::path layer = dir / "edits.usda";
    const std::string stage = openStage(src, {{"editLayer", layer.generic_string()}})["stage"];
    CHECK(info(stage)["editLayer"]["mode"] == "file");
    setTranslate("Referenced", {100, 7, 0});
    saveAs(dir / "scene.lila");
    CHECK(has(readAll(layer), "(100, 7, 0)"));
    CHECK(node(stage).as<std::string>(node(stage).type().attrIndex("editLayerData")).empty());

    // Another tool writing the shared edit layer updates the object it touches.
    writeFile(layer, replaced(readAll(layer), "(100, 7, 0)", "(100, 8, 0)"));
    run("usdStageUpdate");
    CHECK(near(translateOf("Referenced"), {100, 8, 0}, 1e-9));
    saveAs(dir / "scene.lila");

    // Pointing the edit layer at a sublayer moves the edits into it; that file is then edited directly. The source's
    // own opinions are stronger than its sublayers', so the Referenced move loses to it, and says so.
    run("setAttr", {{"args", {stage + ".editLayer", sub.generic_string()}}});
    const json moved = info(stage);
    CHECK(moved["editLayer"]["mode"] == "sublayer");
    CHECK(warned(moved["warnings"], "/Root/Referenced: a stronger layer"));
    CHECK(near(translateOf("Referenced"), {100, 0, 0}, 1e-9));
    const json referenced = run("usdStageOpinions", {{"args", {"Referenced", "xformOp:translate"}}})["properties"][0];
    INFO(referenced.dump());
    CHECK(fs::path(referenced["losesTo"].get<std::string>()).filename() == "composition.usda");
    const json& stack = referenced["opinions"];
    REQUIRE(stack.size() >= 2);
    CHECK(stack[0]["wins"] == true);
    CHECK(stack[1]["role"] == "edits");
    CHECK(stack[1]["value"] == "(100, 8, 0)");
    const json listed481 = run("usdStagePrim", {{"args", {"Referenced"}}});
    for (const json& p : listed481["properties"])
        if (p["name"] == "xformOp:translate")
            CHECK(p.contains("losesTo"));
    setTranslate("FromSublayer", {0, 0, 9});
    saveAs(dir / "scene.lila");
    const std::string subText = readAll(sub);
    INFO(subText);
    CHECK(has(subText, "(0, 0, 9)"));
    CHECK(has(subText, "(100, 8, 0)"));
    CHECK(readAll(src) == before);

    reopen(dir / "scene.lila");
    CHECK(near(translateOf("FromSublayer"), {0, 0, 9}, 1e-9));
    CHECK(near(translateOf("Referenced"), {100, 0, 0}, 1e-9));
}

TEST_CASE("usd stage: changes from other tools update only the prims they touch, reloads keep edits, reopening reloads changed files")
{
    UpAxisScope axis(math::UpAxis::Z);
    fresh();
    const fs::path dir = workDir("notices");
    const fs::path src = copyFixture(dir, "live.usda");
    const std::string stage = openStage(src)["stage"];
    const NodeId box = node("Box").id(), lamp = node("Lamp").id(), geo = node("Geo").id();
    setTranslate("Box", {40, 0, 0});

    // Another tool rewrites the source: the Lamp moves, and Lila's unsaved Box edit is kept.
    std::string text = readAll(src);
    writeFile(src, replaced(text, "(0, 20, 0)", "(0, 33, 0)"));
    const json updated = run("usdStageUpdate");
    CHECK(updated.get<int>() >= 1);
    CHECK(near(translateOf("Lamp"), {0, 33, 0}, 1e-9));
    CHECK(near(translateOf("Box"), {40, 0, 0}, 1e-9));
    CHECK(node("Lamp").id() == lamp);
    CHECK(node("Box").id() == box);
    CHECK(node("Geo").id() == geo);
    CHECK(has(edits(stage), "(40, 0, 0)"));

    // A property set through the stage writes an over, and the object follows the notice.
    run("usdStageSetProperty", {{"args", {"Lamp", "xformOp:translate", "(5, 6, 7)"}}});
    CHECK(near(translateOf("Lamp"), {5, 6, 7}, 1e-9));
    run("usdStageSetProperty", {{"args", {"Lamp", "studio:assetId", "LA_002"}}});
    CHECK(prop("Lamp", "studio:assetId") == "\"LA_002\"");
    run("usdStageSetProperty", {{"args", {"Lamp", "studio:rating", 4}}, {"type", "int"}});
    CHECK(prop("Lamp", "studio:rating") == "4");
    CHECK(node("Lamp").id() == lamp);

    saveAs(dir / "scene.lila");
    run("newScene");
    text = readAll(src);
    writeFile(src, replaced(text, "(10, 0, 0)", "(11, 0, 0)"));
    writeFile(src, replaced(readAll(src), "-20, 0)", "-21, 0)"));
    run("openScene", {{"args", {(dir / "scene.lila").generic_string()}}, {"force", true}});
    CHECK(near(translateOf("Chair"), {0, -21, 0}, 1e-9));
    CHECK(near(translateOf("Box"), {40, 0, 0}, 1e-9));
    CHECK(near(translateOf("Lamp"), {5, 6, 7}, 1e-9));
    CHECK(prop("Lamp", "studio:rating") == "4");

    // Reload reads the files again: unchanged files leave the objects alone.
    const NodeId chair = node("Chair").id();
    run("usdStageReload", {{"args", {stage}}});
    CHECK(node("Chair").id() == chair);
    CHECK(near(translateOf("Box"), {40, 0, 0}, 1e-9));
}

TEST_CASE("usd stage: extract turns an object into a plain Lila object and deactivates its prim")
{
    UpAxisScope axis(math::UpAxis::Z);
    fresh();
    const fs::path dir = workDir("extract");
    const fs::path src = copyFixture(dir, "live.usda");
    const std::string stage = openStage(src)["stage"];
    const json out = run("usdStageExtract", {{"args", {"Box"}}});
    REQUIRE(out.size() == 1);
    CHECK(graph().parent(node("Box").id()) == kNoNode);
    CHECK(near(glm::dvec3(world("Box")[3]), {10, 0, 0}, 1e-9));
    CHECK(graph().find("Box|Geo"));
    saveAs(dir / "scene.lila");
    const std::string text = edits(stage);
    INFO(text);
    CHECK(has(text, "over \"Box\" (\n        active = false"));
    CHECK(!has(text, "def Mesh"));
    reopen(dir / "scene.lila");
    CHECK(graph().parent(node("Box").id()) == kNoNode);
    CHECK_FALSE(graph().find(stage + "|World|Box"));
}

namespace
{
fs::path writeRig(const fs::path& dir);

double rotateZ(const std::string& joint) { return node(joint).as<glm::dvec3>(Transform::kRotate).z; }

/** The stage as another tool sees it: its edits written as a shot over the source and opened anew, in a new scene. */
void openShot(const std::string& stage, const fs::path& dir)
{
    const fs::path shot = dir / "shot.usda";
    run("usdStageWriteShot", {{"args", {shot.generic_string(), stage}}});
    fresh();
    openStage(shot);
}
} // namespace

TEST_CASE("usd stage: an object moved to another parent moves its prim, and moving it back puts it where it was")
{
    UpAxisScope axis(math::UpAxis::Z);
    fresh();
    const fs::path dir = workDir("reparent");
    const fs::path src = copyFixture(dir, "live.usda");
    const std::string stage = openStage(src)["stage"];
    const glm::dmat4 box = world("Box");
    run("parent", {{"args", {"Box", "Lamp"}}});
    setTranslate("Chair", {0, -30, 0});
    saveAs(dir / "scene.lila");
    json i = info(stage);
    std::string text = i["edits"];
    INFO(text);
    CHECK(has(text, "prepend references = </World/Box>"));
    CHECK(has(text, "over \"Box\" (\n        active = false"));
    CHECK(run("usdStagePrim", {{"args", {"Box"}}})["prim"] == "/World/Lamp/Box");
    CHECK(run("usdStagePrim", {{"args", {"Geo"}}})["prim"] == "/World/Lamp/Box/Geo");
    CHECK(!warned(i["warnings"], "move isn't written"));

    // Seen afresh, it sits under its new parent where it was, still bound to the stage's materials.
    openShot(stage, dir);
    REQUIRE(graph().find("Lamp|Box|Geo"));
    CHECK_FALSE(graph().find("World|Box"));
    CHECK(near(world("Box"), box, 1e-9));
    bool red = false;
    for (const auto& [slot, faces] : materials::slotUsage(*outMesh("Geo")))
        red = red || slot == "Red";
    CHECK(red);

    // Edits after the move land on the moved prim, and survive a reopen.
    reopen(dir / "scene.lila");
    setTranslate("Box", {1, 2, 3});
    saveAs(dir / "scene.lila");
    reopen(dir / "scene.lila");
    CHECK(near(translateOf("Box"), {1, 2, 3}, 1e-9));
    CHECK(prop("Box", "xformOp:translate") == "(1, 2, 3)");

    // Moved back, its edits go onto the original prim, which comes back.
    run("parent", {{"args", {"Box", "World"}}});
    saveAs(dir / "scene.lila");
    text = edits(stage);
    CHECK(!has(text, "references"));
    CHECK(!has(text, "active = false"));
    CHECK(run("usdStagePrim", {{"args", {"Box"}}})["prim"] == "/World/Box");
    openShot(stage, dir);
    REQUIRE(graph().find("World|Box|Geo"));
    CHECK_FALSE(graph().find("Lamp|Box"));
    CHECK(near(glm::dvec3(world("Box")[3]), {1, 22, 3}, 1e-9));
    reopen(dir / "scene.lila");

    // An object only the edit layer has moves as it is; undone, it moves back.
    addMesh("Pebble", geo::makeCube({}), node("World").id());
    saveAs(dir / "scene.lila");
    run("parent", {{"args", {"Pebble", "Chair"}}});
    saveAs(dir / "scene.lila");
    CHECK(!has(edits(stage), "</World/Pebble>"));
    CHECK(run("usdStagePrim", {{"args", {"Pebble"}}})["prim"] == "/World/Chair/Pebble");
    run("undo");
    saveAs(dir / "scene.lila");
    CHECK(run("usdStagePrim", {{"args", {"Pebble"}}})["prim"] == "/World/Pebble");

    // Out of the stage, its prim is deactivated; back in, it returns.
    run("parent", {{"args", {"Chair"}}, {"world", true}});
    saveAs(dir / "scene.lila");
    CHECK(has(edits(stage), "over \"Chair\" (\n        active = false"));
    run("undo");
    saveAs(dir / "scene.lila");
    CHECK(!has(edits(stage), "\"Chair\" (\n        active = false"));
    CHECK(prop("Chair", "xformOp:translate") == "(0, -30, 0)");
}

TEST_CASE("usd stage: joint edits are written over the skeleton and its animation, keyed or still, and new joints join it")
{
    UpAxisScope axis(math::UpAxis::Z);
    fresh();
    const fs::path dir = workDir("joints");
    const fs::path src = writeRig(dir);
    const std::string before = readAll(src);
    fresh();
    const std::string stage = openStage(src)["stage"];
    REQUIRE(graph().find("bone_b"));
    run("setAttr", {{"args", {"bone_b.rotate", 0, 0, 30}}});
    saveAs(dir / "scene.lila");
    json i = info(stage);
    std::string text = i["edits"];
    INFO(text);
    CHECK(has(text, "def SkelAnimation"));
    CHECK(!has(text, "rotations.timeSamples"));
    CHECK(!warned(i["warnings"], "joint"));
    openShot(stage, dir);
    REQUIRE(graph().find("bone_b"));
    CHECK(rotateZ("bone_b") == doctest::Approx(30.0));

    // Undone after a reopen, the pose before it comes back.
    reopen(dir / "scene.lila");
    run("setAttr", {{"args", {"bone_b.rotate", 0, 0, 60}}});
    saveAs(dir / "scene.lila");
    run("undo");
    saveAs(dir / "scene.lila");
    openShot(stage, dir);
    CHECK(rotateZ("bone_b") == doctest::Approx(30.0));
    reopen(dir / "scene.lila");

    // Keys go on the animation, sampled over the playback range.
    run("playbackOptions", {{"minTime", 0.0}, {"maxTime", 1.0}, {"animationStartTime", 0.0}, {"animationEndTime", 1.0}});
    anim::setKey(graph(), {node("bone_a").id(), Transform::kRotate, 2}, 0.0, 0.0);
    anim::setKey(graph(), {node("bone_a").id(), Transform::kRotate, 2}, 1.0, 45.0);
    saveAs(dir / "scene.lila");
    CHECK(has(edits(stage), "rotations.timeSamples"));
    openShot(stage, dir);
    graph().setTime(0.0);
    graph().evaluate();
    CHECK(rotateZ("bone_a") == doctest::Approx(0.0).epsilon(1e-3));
    graph().setTime(1.0);
    graph().evaluate();
    CHECK(rotateZ("bone_a") == doctest::Approx(45.0).epsilon(1e-3));
    CHECK(rotateZ("bone_b") == doctest::Approx(30.0).epsilon(1e-3));
    graph().setTime(0.0);
    reopen(dir / "scene.lila");

    // A new joint joins the skeleton after its others, so the skin's indices still hold.
    run("select", {{"args", {"bone_b"}}});
    run("joint", {{"position", json::array({0, 90, 0})}, {"name", "bone_c"}});
    REQUIRE(graph().find("bone_b|bone_c"));
    saveAs(dir / "scene.lila");
    i = info(stage);
    text = i["edits"];
    CHECK(has(text, "\"bone_a/bone_b/bone_c\""));
    CHECK(!warned(i["warnings"], "new joints"));
    openShot(stage, dir);
    CHECK(graph().find("bone_b|bone_c"));
    CHECK(skin::find(graph(), node("Tube").id()));
    CHECK(readAll(src) == before);
}

TEST_CASE("usd stage: skin weight edits are written in the stage's joint order, and undo puts them back")
{
    UpAxisScope axis(math::UpAxis::Z);
    fresh();
    const fs::path dir = workDir("weights");
    const fs::path src = writeRig(dir);
    fresh();
    const std::string stage = openStage(src)["stage"];
    auto weightOf = [](uint32_t point, const std::string& joint) {
        const json r = run("skinPercent", {{"args", {"Tube"}}, {"vertices", json::array({point})}});
        for (const auto& [name, w] : r[0]["weights"].items())
            if (name.ends_with(joint))
                return w.get<double>();
        return 0.0;
    };
    const double was = weightOf(0, "bone_b"), other = weightOf(2, "bone_b");
    REQUIRE(was < 0.99);
    run("skinPercent", {{"args", {"Tube"}}, {"vertices", json::array({0, 1})}, {"transformValue", {{"bone_b", 1.0}}}});
    saveAs(dir / "scene.lila");
    json i = info(stage);
    std::string text = i["edits"];
    INFO(text);
    CHECK(has(text, "primvars:skel:jointWeights"));
    CHECK(!warned(i["warnings"], "skin"));
    openShot(stage, dir);
    CHECK(weightOf(0, "bone_b") == doctest::Approx(1.0));
    CHECK(weightOf(1, "bone_b") == doctest::Approx(1.0));
    CHECK(weightOf(2, "bone_b") == doctest::Approx(other).epsilon(1e-4));

    // Undone, the stage's weights come back.
    reopen(dir / "scene.lila");
    run("skinPercent", {{"args", {"Tube"}}, {"vertices", json::array({0})}, {"transformValue", {{"bone_a", 1.0}}}});
    saveAs(dir / "scene.lila");
    run("undo");
    saveAs(dir / "scene.lila");
    openShot(stage, dir);
    CHECK(weightOf(0, "bone_b") == doctest::Approx(1.0));
}

TEST_CASE("usd stage: blend shape edits and their weights are written, new targets join, and removed ones leave")
{
    UpAxisScope axis(math::UpAxis::Z);
    fresh();
    const fs::path dir = workDir("shapes");
    auto shape = [](const std::string& name, double dz) {
        geo::Mesh m = geo::makeCube({});
        for (geo::Index p = 0; p < m.pointCapacity(); ++p)
            if (m.pointValid(p))
                m.setPosition(p, m.position(p) + glm::dvec3(0, 0, dz));
        addMesh(name, std::move(m));
    };
    shape("Face", 0.0);
    shape("Smile", 2.0);
    run("morph", {{"args", {"Smile", "Face"}}});
    run("delete", {{"args", {"Smile"}}});
    const fs::path src = dir / "face.usda";
    run("exportUsd", {{"args", {src.generic_string()}}, {"selectionOnly", false}});
    fresh();
    const std::string stage = openStage(src)["stage"];
    auto targets = [] { return run("morphTargets", {{"args", {"Face"}}}); };
    REQUIRE(targets().size() == 1);
    run("setAttr", {{"args", {targets()[0]["node"].get<std::string>() + ".weight", 0.75}}});
    saveAs(dir / "scene.lila");
    json i = info(stage);
    std::string text = i["edits"];
    INFO(text);
    CHECK(has(text, "blendShapeWeights"));
    CHECK(has(text, "0.75"));
    CHECK(!warned(i["warnings"], "blend shape"));
    openShot(stage, dir);
    REQUIRE(targets().size() == 1);
    CHECK(targets()[0]["weight"].get<double>() == doctest::Approx(0.75));

    // Another target: a BlendShape of its own, named on the mesh.
    reopen(dir / "scene.lila");
    shape("Frown", -3.0);
    run("addMorphTarget", {{"args", {"Face", "Frown"}}});
    run("delete", {{"args", {"Frown"}}});
    saveAs(dir / "scene.lila");
    text = edits(stage);
    CHECK(has(text, "def BlendShape \"Frown\""));
    CHECK(has(text, "skel:blendShapes = [\"Smile\", \"Frown\"]"));
    openShot(stage, dir);
    REQUIRE(targets().size() == 2);
    CHECK(targets()[1]["name"] == "Frown");

    // Taken out again, the mesh names only the first.
    reopen(dir / "scene.lila");
    REQUIRE(targets().size() == 2);
    run("removeMorphTarget", {{"args", {targets()[1]["node"].get<std::string>()}}});
    saveAs(dir / "scene.lila");
    openShot(stage, dir);
    CHECK(targets().size() == 1);
}

TEST_CASE("usd stage: a skeleton made in Lila is written with its SkelRoot, and meshes skinned to it bind to it")
{
    UpAxisScope axis(math::UpAxis::Z);
    fresh();
    const fs::path dir = workDir("newSkeleton");
    const fs::path src = copyFixture(dir, "live.usda");
    const std::string stage = openStage(src)["stage"];
    run("select", {{"args", json::array()}});
    run("joint", {{"position", json::array({10, 0, -1})}, {"name", "hip"}});
    run("joint", {{"position", json::array({10, 0, 1})}, {"name", "knee"}});
    run("parent", {{"args", {"hip", "World"}}});
    run("skinCluster", {{"args", {"hip", "Geo"}}});
    saveAs(dir / "scene.lila");
    json i = info(stage);
    std::string text = i["edits"];
    INFO(text);
    CHECK(has(text, "def Skeleton \"Skeleton\""));
    CHECK(has(text, "SkelRoot"));
    CHECK(has(text, "primvars:skel:jointWeights"));
    CHECK(!warned(i["warnings"], "aren't written"));
    CHECK(!warned(i["warnings"], "isn't written"));

    // A joint added to it later joins it, as for any skeleton in the stage.
    run("select", {{"args", {"knee"}}});
    run("joint", {{"position", json::array({10, 0, 3})}, {"name", "ankle"}});
    saveAs(dir / "scene.lila");
    CHECK(has(edits(stage), "\"hip/knee/ankle\""));

    openShot(stage, dir);
    REQUIRE(graph().find("World|hip|knee|ankle"));
    const NodeId sk = skin::find(graph(), node("Geo").id());
    REQUIRE(sk);
    CHECK(skin::influences(graph(), sk).size() == 2);
}

TEST_CASE("usd stage: an object moved under a new object gets that object written first")
{
    UpAxisScope axis(math::UpAxis::Z);
    fresh();
    const fs::path dir = workDir("newParent");
    const fs::path src = copyFixture(dir, "live.usda");
    const std::string stage = openStage(src)["stage"];
    const glm::dmat4 chair = world("Chair");
    Node* rack = graph().createNode(*types::find("transform"), "Rack", node("World").id(), false);
    graph().setValue({rack->id(), Transform::kTranslate, -1}, glm::dvec3(0, 5, 0));
    run("parent", {{"args", {"Chair", "Rack"}}});
    saveAs(dir / "scene.lila");
    json i = info(stage);
    std::string text = i["edits"];
    INFO(text);
    CHECK(has(text, "def Xform \"Rack\""));
    CHECK(run("usdStagePrim", {{"args", {"Chair"}}})["prim"] == "/World/Rack/Chair");
    CHECK(run("usdStagePrim", {{"args", {"Rack"}}})["prim"] == "/World/Rack");
    CHECK(!warned(i["warnings"], "isn't written"));
    openShot(stage, dir);
    REQUIRE(graph().find("World|Rack|Chair"));
    CHECK(near(world("Chair"), chair, 1e-9));
}

TEST_CASE("usd stage: a deleted joint leaves its skeleton, and undone it goes back where it stood")
{
    UpAxisScope axis(math::UpAxis::Z);
    fresh();
    const fs::path dir = workDir("jointDelete");
    const fs::path src = writeRig(dir);
    fresh();
    const std::string stage = openStage(src)["stage"];
    auto weightOf = [](uint32_t point, const std::string& joint) {
        const json r = run("skinPercent", {{"args", {"Tube"}}, {"vertices", json::array({point})}});
        for (const auto& [name, w] : r[0]["weights"].items())
            if (name.ends_with(joint))
                return w.get<double>();
        return 0.0;
    };
    const double was = weightOf(0, "bone_b");
    run("delete", {{"args", {"bone_b"}}});
    saveAs(dir / "scene.lila");
    json i = info(stage);
    std::string text = i["edits"];
    INFO(text);
    CHECK(has(text, "uniform token[] joints = [\"bone_a\"]"));
    CHECK(!warned(i["warnings"], "isn't written"));

    run("undo");
    REQUIRE(graph().find("bone_a|bone_b"));
    saveAs(dir / "scene.lila");
    text = edits(stage);
    CHECK(has(text, "uniform token[] joints = [\"bone_a\", \"bone_a/bone_b\"]"));
    openShot(stage, dir);
    REQUIRE(graph().find("bone_a|bone_b"));
    CHECK(skin::influences(graph(), skin::find(graph(), node("Tube").id())).size() == 2);
    CHECK(weightOf(0, "bone_b") == doctest::Approx(was).epsilon(1e-4));
}

TEST_CASE("usd stage: a point instance taken out of its instancer goes back once it holds only its prototype again")
{
    UpAxisScope axis(math::UpAxis::Z);
    fresh();
    const fs::path dir = workDir("mergeBack");
    const fs::path src = copyFixture(dir, "instancing.usda");
    const std::string stage = openStage(src)["stage"];
    const NodeId scatter = node("Scatter").id();
    const glm::dmat4 before = world("Rock_1");
    addMesh("Moss", geo::makeCube({}), node("Rock_1").id());
    saveAs(dir / "scene.lila");
    REQUIRE(has(edits(stage), "def Xform \"Rock_1\""));
    CHECK(graph().parent(node("Rock_1").id()) != scatter);

    // The added object deleted: back on the instancer, shown there, nothing left in the edit layer.
    run("delete", {{"args", {"Moss"}}});
    saveAs(dir / "scene.lila");
    std::string text = edits(stage);
    INFO(text);
    CHECK(!has(text, "Rock_1"));
    CHECK(!has(text, "invisibleIds"));
    CHECK(graph().parent(node("Rock_1").id()) == scatter);
    CHECK(near(world("Rock_1"), before, 1e-9));
    saveAs(dir / "scene.lila");
    CHECK(!has(edits(stage), "positions"));
    run("usdStageUpdate");
    run("usdStageReload", {{"args", {stage}}});
    REQUIRE(graph().find("Scatter|Rock_1|Rock|Body"));
    CHECK(near(world("Rock_1"), before, 1e-6));

    // Through undo: the delete undone takes it out again, and undone once more puts it back.
    run("undo");
    REQUIRE(graph().find("Rock_1|Moss"));
    saveAs(dir / "scene.lila");
    CHECK(has(edits(stage), "def Xform \"Rock_1\""));
    run("delete", {{"args", {"Moss"}}});
    saveAs(dir / "scene.lila");
    CHECK(!has(edits(stage), "Rock_1"));
    run("undo");
    saveAs(dir / "scene.lila");
    CHECK(has(edits(stage), "def Mesh \"Moss\""));

    // Across a reopen.
    reopen(dir / "scene.lila");
    REQUIRE(graph().find("Rock_1|Moss"));
    run("delete", {{"args", {"Moss"}}});
    saveAs(dir / "scene.lila");
    text = edits(stage);
    CHECK(!has(text, "Rock_1"));
    CHECK(graph().parent(node("Rock_1").id()) == node("Scatter").id());
    run("usdStageReload", {{"args", {stage}}});
    CHECK(graph().find("Scatter|Rock_1|Rock|Body"));
    CHECK(near(world("Rock_1"), before, 1e-6));

    // A copy edited since stays out.
    addMesh("Moss", geo::makeCube({}), node("Rock_1").id());
    saveAs(dir / "scene.lila");
    setTranslate("Rock_1|Rock|Body", {0, 0, 1});
    run("delete", {{"args", {"Moss"}}});
    saveAs(dir / "scene.lila");
    CHECK(has(edits(stage), "def Xform \"Rock_1\""));
}

namespace
{
const anim::Curve* curveOn(const std::string& name, int attr, int component, anim::Curve& out)
{
    auto src = graph().source({node(name).id(), attr, component});
    if (!src || !anim::isCurve(*graph().node(src->node)))
        return nullptr;
    out = anim::curveOf(*graph().node(src->node));
    return &out;
}

json stats(const std::string& stage) { return info(stage)["stats"]; }

size_t countType(const char* type)
{
    size_t n = 0;
    for (NodeId id : graph().nodes())
        n += graph().node(id)->type().name == type;
    return n;
}

/** A stage of a Lila rig: a tube skinned to two joints. */
fs::path writeRig(const fs::path& dir)
{
    run("polyCylinder", {{"name", "Tube"}, {"radius", 10}, {"height", 100}, {"subdivisionsHeight", 4}, {"caps", false}});
    run("select", {{"args", json::array()}});
    run("joint", {{"position", json::array({0, -50, 0})}, {"name", "bone_a"}});
    run("joint", {{"position", json::array({0, 50, 0})}, {"name", "bone_b"}});
    run("skinCluster", {{"args", {"bone_a", "Tube"}}});
    const fs::path src = dir / "rig.usda";
    run("exportUsd", {{"args", {src.generic_string()}}, {"selectionOnly", false}});
    return src;
}
} // namespace

TEST_CASE("usd stage: a written animated edit comes back with its own keys, and dense samples come back thinned")
{
    UpAxisScope axis(math::UpAxis::Z);
    fresh();
    const fs::path dir = workDir("keys");
    const fs::path src = copyFixture(dir, "live.usda");
    const std::string stage = openStage(src)["stage"];

    const PlugRef tx{node("Chair").id(), Transform::kTranslate, 0};
    anim::setKey(graph(), tx, 0.0, 0.0);
    anim::setKey(graph(), tx, 0.5, 10.0);
    anim::setKey(graph(), tx, 1.0, 50.0);
    graph().setTime(0.25);
    graph().evaluate();
    const double quarter = node("Chair").as<glm::dvec3>(Transform::kTranslate).x;
    graph().setTime(0.0);
    saveAs(dir / "scene.lila");
    CHECK(has(edits(stage), "xformOp:translate.timeSamples"));

    // Anything that builds the Chair again from the stage: a property set on its prim.
    run("usdStageSetProperty", {{"args", {"Chair", "studio:note", "keyed"}}, {"type", "string"}});
    anim::Curve c;
    REQUIRE(curveOn("Chair", Transform::kTranslate, 0, c));
    CHECK(c.keys.size() == 3);
    graph().setTime(0.25);
    graph().evaluate();
    CHECK(node("Chair").as<glm::dvec3>(Transform::kTranslate).x == doctest::Approx(quarter).epsilon(1e-6));
    graph().setTime(0.0);
    CHECK_FALSE(graph().source({node("Chair").id(), Transform::kTranslate, 1}));

    // Samples written elsewhere, a linear move sampled on every frame: two keys.
    std::string text = "#usda 1.0\n(\n    startTimeCode = 0\n    endTimeCode = 24\n    timeCodesPerSecond = 24\n    upAxis = \"Z\"\n)\n"
                       "def Xform \"Mover\"\n{\n    double3 xformOp:translate.timeSamples = {\n";
    for (int f = 0; f <= 24; ++f)
        text += std::format("        {}: ({}, 0, 0),\n", f, f * 2);
    text += "    }\n    uniform token[] xformOpOrder = [\"xformOp:translate\"]\n}\n";
    writeFile(dir / "dense.usda", text);
    fresh();
    openStage(dir / "dense.usda");
    REQUIRE(curveOn("Mover", Transform::kTranslate, 0, c));
    CHECK(c.keys.size() == 2);
    CHECK(c.evaluate(0.5) == doctest::Approx(24.0));
}

TEST_CASE("usd stage: a one-object edit on a large stage exports only that object")
{
    UpAxisScope axis(math::UpAxis::Z);
    fresh();
    const fs::path dir = workDir("large");
    std::string text = "#usda 1.0\n(\n    defaultPrim = \"World\"\n    metersPerUnit = 0.01\n    upAxis = \"Z\"\n)\ndef Xform \"World\"\n{\n";
    for (int i = 0; i < 3000; ++i)
        text += std::format("    def Xform \"Item{}\"\n    {{\n        double3 xformOp:translate = ({}, 0, 0)\n        uniform token[] xformOpOrder = "
                            "[\"xformOp:translate\"]\n    }}\n",
                            i, i);
    text += "}\n";
    writeFile(dir / "large.usda", text);
    const std::string stage = openStage(dir / "large.usda")["stage"];
    const json before = stats(stage);
    CHECK(before["lastExported"].get<int>() >= 3000);

    saveAs(dir / "scene.lila");
    CHECK(stats(stage)["lastExported"] == 0);

    setTranslate("Item1234", {5, 6, 7});
    saveAs(dir / "scene.lila");
    json after = stats(stage);
    INFO(after.dump());
    CHECK(after["lastExported"] == 1);
    CHECK(after["fullDiffs"] == before["fullDiffs"]);
    CHECK(after["partialDiffs"].get<int>() == before["partialDiffs"].get<int>() + 1);
    CHECK(prop("Item1234", "xformOp:translate") == "(5, 6, 7)");
    CHECK(!has(edits(stage), "over \"Item1233\""));

    // A batch can change anything, so the next comparison exports the whole stage.
    {
        Graph::Batch batch(graph());
        setTranslate("Item7", {1, 1, 1});
    }
    saveAs(dir / "scene.lila");
    after = stats(stage);
    CHECK(after["lastExported"].get<int>() >= 3000);
    CHECK(prop("Item7", "xformOp:translate") == "(1, 1, 1)");

    // A key on a curve, which lives outside the stage, reaches the object it drives.
    anim::setKey(graph(), {node("Item42").id(), Transform::kTranslate, 2}, 0.0, 3.0);
    saveAs(dir / "scene.lila");
    CHECK(stats(stage)["lastExported"] == 1);
    CHECK(prop("Item42", "xformOp:translate") == "(42, 0, 3)");
}

TEST_CASE("usd stage: a rebuild keeps a mesh's modifiers, applies them to a new shape, and never to their own output")
{
    UpAxisScope axis(math::UpAxis::Z);
    fresh();
    const fs::path dir = workDir("modifiers");
    const fs::path src = copyFixture(dir, "live.usda");
    const std::string stage = openStage(src)["stage"];
    const NodeId geo = node("Geo").id();
    run("addModifier", {{"args", {"Geo"}}, {"kind", "bevel"}, {"params", {{"segments", 1}}}});
    const size_t beveled = outMesh("Geo")->numPoints();
    REQUIRE(beveled > 8);
    saveAs(dir / "scene.lila");

    // The stage now holds the beveled result; built again, Geo keeps its stack and doesn't bevel it twice.
    run("usdStageSetProperty", {{"args", {"Geo", "studio:note", "beveled"}}, {"type", "string"}});
    CHECK(node("Geo").id() == geo);
    REQUIRE(modifiers::stack(graph(), geo).size() == 1);
    CHECK(outMesh("Geo")->numPoints() == beveled);

    // Another shape written to the stage goes under the modifier.
    run("usdStageSetProperty", {{"args", {"Geo", "points", "[(-2, -2, -2), (-2, -2, 2), (-2, 2, -2), (-2, 2, 2), (2, -2, -2), (2, -2, 2), (2, 2, -2), (2, 2, 2)]"}}});
    run("usdStageSetProperty", {{"args", {"Geo", "faceVertexCounts", "[4, 4, 4, 4, 4, 4]"}}});
    run("usdStageSetProperty", {{"args", {"Geo", "faceVertexIndices", "[0, 1, 3, 2, 2, 3, 7, 6, 6, 7, 5, 4, 4, 5, 1, 0, 2, 6, 4, 0, 7, 3, 1, 5]"}}});
    CHECK(node("Geo").id() == geo);
    REQUIRE(modifiers::stack(graph(), geo).size() == 1);
    CHECK(modifiers::cage(graph(), geo)->numPoints() == 8);
    CHECK(outMesh("Geo")->numPoints() == beveled);
    double far = 0.0;
    for (geo::Index p = 0; p < outMesh("Geo")->pointCapacity(); ++p)
        if (outMesh("Geo")->pointValid(p))
            far = std::max(far, std::abs(outMesh("Geo")->position(p).x));
    CHECK(far == doctest::Approx(2.0));
}

TEST_CASE("usd stage: a rebuild keeps a skinned mesh's skin and leaves no skin nodes behind")
{
    UpAxisScope axis(math::UpAxis::Z);
    fresh();
    const fs::path dir = workDir("skinned");
    const fs::path src = writeRig(dir);
    fresh();
    const std::string stage = openStage(src)["stage"];
    const NodeId tube = node("Tube").id();
    const NodeId skin = skin::find(graph(), tube);
    REQUIRE(skin);
    graph().setValue({skin, SkinClusterNode::kSkinningMethod, -1}, int64_t(SkinClusterNode::kDualQuaternion));
    const size_t influences = skin::influences(graph(), skin).size();

    // The mesh alone built again.
    run("usdStageSetProperty", {{"args", {"Tube", "studio:note", "rigged"}}, {"type", "string"}});
    CHECK(node("Tube").id() == tube);
    CHECK(skin::find(graph(), tube) == skin);
    CHECK(node("Tube").type().name == "mesh");
    CHECK(graph().node(skin)->as<int64_t>(SkinClusterNode::kSkinningMethod) == SkinClusterNode::kDualQuaternion);

    // The skeleton built again from a change in the file: new joints, the same skin, bound to them.
    std::string text = readAll(src);
    // Into the Skeleton's body, past the braces of its metadata.
    const size_t at = text.find("\n    {\n", text.find("def Skeleton"));
    REQUIRE(at != std::string::npos);
    text.insert(at + 6, "\n        custom int studio:revision = 2\n");
    writeFile(src, text);
    run("usdStageUpdate");
    REQUIRE(graph().find("bone_b"));
    CHECK(skin::find(graph(), node("Tube").id()) == skin);
    CHECK(graph().node(skin)->as<int64_t>(SkinClusterNode::kSkinningMethod) == SkinClusterNode::kDualQuaternion);
    const std::vector<skin::Influence> now = skin::influences(graph(), skin);
    CHECK(now.size() == influences);
    for (const skin::Influence& inf : now)
        CHECK(inf.joint != kNoNode);
    CHECK(countType("skinCluster") == 1);
    CHECK(countType("skinInfluence") == influences);
    CHECK(!warned(info(stage)["warnings"], "unskinned"));

    // Still driven by the joints.
    const glm::dvec3 tipBefore = world("bone_b")[3];
    const scene::MeshPtr rest = outMesh("Tube");
    graph().setValue({node("bone_a").id(), Transform::kRotate, -1}, glm::dvec3(0, 0, 30));
    CHECK(glm::length(glm::dvec3(world("bone_b")[3]) - tipBefore) > 1.0);
    CHECK(outMesh("Tube") != rest);
}

TEST_CASE("usd stage: point instances move on their instancer, copies edit their prototype, deleted ones hide")
{
    UpAxisScope axis(math::UpAxis::Z);
    fresh();
    const fs::path dir = workDir("instancer");
    const fs::path src = copyFixture(dir, "instancing.usda");
    const std::string stage = openStage(src)["stage"];
    REQUIRE(graph().find("Rock_1"));
    REQUIRE(graph().find("Pebble_2|Pebble"));
    REQUIRE(graph().find("Pebble_0|Pebble"));

    setTranslate("Rock_1", {15, 0, 0});
    graph().setValue({node("Pebble_2|Pebble").id(), Transform::kTranslate, -1}, glm::dvec3(0, 0, 4));
    saveAs(dir / "scene.lila");
    std::string text = edits(stage);
    INFO(text);
    CHECK(has(text, "point3f[] positions = [(0, 0, 0), (15, 0, 0), (20, 0, 0), (30, 0, 0)]"));
    CHECK(has(text, "over \"Pebble\""));
    CHECK(has(text, "(0, 0, 4)"));

    // The other copies of the prototype follow once the stage updates.
    run("usdStageUpdate");
    REQUIRE(graph().find("Pebble_0|Pebble"));
    CHECK(near(translateOf("Pebble_0|Pebble"), {0, 0, 4}, 1e-9));
    CHECK(near(translateOf("Rock_1"), {15, 0, 0}, 1e-6));

    run("delete", {{"args", {"Pebble_0"}}});
    saveAs(dir / "scene.lila");
    text = edits(stage);
    CHECK(has(text, "int64[] invisibleIds = [3, 0]"));
}

TEST_CASE("usd stage: edits inside an instance open it, and objects added under instances and joints are written")
{
    UpAxisScope axis(math::UpAxis::Z);
    fresh();
    const fs::path dir = workDir("instances");
    const fs::path src = copyFixture(dir, "instancing.usda");
    const std::string stage = openStage(src)["stage"];
    REQUIRE(graph().find("InstA|Tri"));
    graph().setValue({node("InstA|Tri").id(), Transform::kTranslate, -1}, glm::dvec3(1, 2, 3));
    addMesh("Extra", geo::makeCube({}), node("InstB").id());
    saveAs(dir / "scene.lila");
    json i = info(stage);
    std::string text = i["edits"];
    INFO(text);
    CHECK(has(text, "over \"InstA\" (\n        instanceable = false"));
    CHECK(has(text, "over \"InstB\" (\n        instanceable = false"));
    CHECK(has(text, "(1, 2, 3)"));
    CHECK(has(text, "def Mesh \"Extra\""));
    CHECK(warned(i["warnings"], "made non-instanceable"));
    CHECK(prop("InstA|Tri", "xformOp:translate") == "(1, 2, 3)");
    run("usdStageReload", {{"args", {stage}}});
    CHECK(near(translateOf("InstA|Tri"), {1, 2, 3}, 1e-9));
    CHECK(graph().find("InstB|Extra"));

    // Under a joint: written where the skeleton sits, following the joint.
    fresh();
    const fs::path rig = writeRig(dir);
    fresh();
    const std::string rigStage = openStage(rig)["stage"];
    addMesh("Hat", geo::makeCube({}), node("bone_b").id());
    saveAs(dir / "rig.lila");
    i = info(rigStage);
    INFO(i.dump());
    CHECK(has(i["edits"].get<std::string>(), "def Mesh \"Hat\""));
    CHECK(!warned(i["warnings"], "aren't written"));
    CHECK(!prop("Hat", "points").empty());
}

TEST_CASE("usd stage: edits inside LOD levels are written into their variants, and a shot keeps the payload load state")
{
    UpAxisScope axis(math::UpAxis::Z);
    fresh();
    const fs::path dir = workDir("lods");
    const std::string mesh = "                def Mesh \"Mesh\"\n                {{\n                    int[] faceVertexCounts = [3]\n"
                             "                    int[] faceVertexIndices = [0, 1, 2]\n                    point3f[] points = [(0, 0, 0), ({}, 0, 0), (0, 1, 0)]\n"
                             "                }}\n";
    const auto lodMesh = [&](int pointX) { return std::vformat(mesh, std::make_format_args(pointX)); };
    const std::string text = "#usda 1.0\n(\n    defaultPrim = \"World\"\n    metersPerUnit = 0.01\n    upAxis = \"Z\"\n)\ndef Xform \"World\"\n{\n"
                             "    def Xform \"Chair\" (\n        variants = {\n            string LOD = \"LOD0\"\n        }\n        prepend variantSets = \"LOD\"\n    )\n"
                             "    {\n        variantSet \"LOD\" = {\n            \"LOD0\" {\n" +
                             lodMesh(1) + "            }\n            \"LOD1\" {\n" + lodMesh(2) +
                             "            }\n        }\n    }\n}\n";
    writeFile(dir / "lods.usda", text);
    const std::string stage = openStage(dir / "lods.usda")["stage"];
    REQUIRE(node("Chair").type().name == "lodGroup");
    const std::vector<NodeId> levels = graph().children(node("Chair").id());
    REQUIRE(levels.size() == 2);
    const std::string lod1 = graph().path(levels[1]);
    CHECK(run("usdStagePrim", {{"args", {lod1}}})["prim"] == "/World/Chair/Mesh");
    graph().setValue({levels[1], Transform::kTranslate, -1}, glm::dvec3(0, 0, 7));
    saveAs(dir / "scene.lila");
    const std::string e = edits(stage);
    INFO(e);
    CHECK(has(e, "\"LOD1\" {"));
    CHECK(has(e, "(0, 0, 7)"));
    CHECK(!has(e, "\"LOD0\" {"));
    CHECK(prop(lod1, "xformOp:translate") == "(0, 0, 7)");
    CHECK(prop(graph().path(levels[0]), "xformOp:translate").empty());

    reopen(dir / "scene.lila");
    run("usdStageReload", {{"args", {stage}}});
    const std::vector<NodeId> again = graph().children(node("Chair").id());
    REQUIRE(again.size() == 2);
    CHECK(near(graph().node(again[1])->as<glm::dvec3>(Transform::kTranslate), {0, 0, 7}, 1e-9));
    CHECK(near(graph().node(again[0])->as<glm::dvec3>(Transform::kTranslate), {0, 0, 0}, 1e-9));

    // Payload load state goes into a shot, and opening the shot honours it.
    fresh();
    const fs::path comp = copyFixture(dir, "composition.usda");
    copyFixture(dir, "part.usda");
    copyFixture(dir, "sub.usda");
    const std::string cstage = openStage(comp, {{"loadPayloads", false}})["stage"];
    CHECK_FALSE(graph().find("Payloaded|Box"));
    const fs::path shot = dir / "shot.usda";
    run("usdStageWriteShot", {{"args", {shot.generic_string(), cstage}}});
    const std::string shotText = readAll(shot);
    INFO(shotText);
    CHECK(has(shotText, "customLayerData"));
    CHECK(has(shotText, "loadRules"));
    fresh();
    openStage(shot);
    CHECK(graph().find("Referenced|Box"));
    CHECK_FALSE(graph().find("Payloaded|Box"));
}

namespace
{
double farX(NodeId mesh)
{
    const scene::MeshPtr m = outMesh(graph().path(mesh));
    double far = 0.0;
    for (geo::Index p = 0; p < m->pointCapacity(); ++p)
        if (m->pointValid(p))
            far = std::max(far, m->position(p).x);
    return far;
}
} // namespace

TEST_CASE("usd stage: variant sets inside LOD levels switch per level")
{
    UpAxisScope axis(math::UpAxis::Z);
    fresh();
    const fs::path dir = workDir("lodVariants");
    const std::string mesh = "                def Mesh \"Mesh\" (\n                    variants = {\n                        string look = \"a\"\n                    }\n"
                             "                    prepend variantSets = \"look\"\n                )\n                {\n"
                             "                    int[] faceVertexCounts = [3]\n                    int[] faceVertexIndices = [0, 1, 2]\n"
                             "                    variantSet \"look\" = {\n"
                             "                        \"a\" {\n                            point3f[] points = [(0, 0, 0), (1, 0, 0), (0, 1, 0)]\n                        }\n"
                             "                        \"b\" {\n                            point3f[] points = [(0, 0, 0), (5, 0, 0), (0, 1, 0)]\n                        }\n"
                             "                    }\n                }\n";
    const std::string text = "#usda 1.0\n(\n    defaultPrim = \"World\"\n    metersPerUnit = 0.01\n    upAxis = \"Z\"\n)\ndef Xform \"World\"\n{\n"
                             "    def Xform \"Chair\" (\n        variants = {\n            string LOD = \"LOD0\"\n        }\n        prepend variantSets = \"LOD\"\n    )\n"
                             "    {\n        variantSet \"LOD\" = {\n            \"LOD0\" {\n" +
                             mesh + "            }\n            \"LOD1\" {\n" + mesh + "            }\n        }\n    }\n}\n";
    writeFile(dir / "lods.usda", text);
    const std::string stage = openStage(dir / "lods.usda")["stage"];
    std::vector<NodeId> levels = graph().children(node("Chair").id());
    REQUIRE(levels.size() == 2);
    CHECK(farX(levels[0]) == doctest::Approx(1.0));
    CHECK(farX(levels[1]) == doctest::Approx(1.0));

    bool listed = false;
    const json variants = info(stage)["variants"];
    INFO(variants.dump());
    for (const json& v : variants)
        listed = listed || (v["prim"] == "/World/Chair{LOD=LOD1}Mesh" && v["set"] == "look" && v.value("level", "") == "LOD1");
    CHECK(listed);

    // One level's mesh switched: written inside that level, built again in Lila.
    run("usdStageVariant", {{"args", {stage}}, {"prim", "/World/Chair{LOD=LOD1}Mesh"}, {"set", "look"}, {"variant", "b"}});
    const std::string e = edits(stage);
    INFO(e);
    CHECK(has(e, "\"LOD1\" {"));
    CHECK(has(e, "string look = \"b\""));
    CHECK(!has(e, "\"LOD0\" {"));
    levels = graph().children(node("Chair").id());
    REQUIRE(levels.size() == 2);
    CHECK(farX(levels[0]) == doctest::Approx(1.0));
    CHECK(farX(levels[1]) == doctest::Approx(5.0));
    CHECK(has(prop(graph().path(levels[1]), "points"), "(5, 0, 0)"));

    // By set name alone, every level.
    run("usdStageVariant", {{"args", {stage}}, {"set", "look"}, {"variant", "b"}});
    levels = graph().children(node("Chair").id());
    REQUIRE(levels.size() == 2);
    CHECK(farX(levels[0]) == doctest::Approx(5.0));
    CHECK(farX(levels[1]) == doctest::Approx(5.0));

    saveAs(dir / "scene.lila");
    reopen(dir / "scene.lila");
    run("usdStageReload", {{"args", {stage}}});
    levels = graph().children(node("Chair").id());
    REQUIRE(levels.size() == 2);
    CHECK(farX(levels[0]) == doctest::Approx(5.0));
    CHECK(farX(levels[1]) == doctest::Approx(5.0));
}

TEST_CASE("usd stage: an object added under a point instance takes the instance out of its instancer")
{
    UpAxisScope axis(math::UpAxis::Z);
    fresh();
    const fs::path dir = workDir("extract");
    const fs::path src = copyFixture(dir, "instancing.usda");
    const std::string stage = openStage(src)["stage"];
    REQUIRE(graph().find("Rock_1|Rock|Body"));
    const glm::dmat4 before = world("Rock_1");
    addMesh("Moss", geo::makeCube({}), node("Rock_1").id());
    saveAs(dir / "scene.lila");
    const json i = info(stage);
    const std::string text = i["edits"];
    INFO(text);
    CHECK(has(text, "def Xform \"Rock_1\""));
    CHECK(has(text, "prepend references = </World/Scatter/Prototypes/Rock>"));
    CHECK(has(text, "def Mesh \"Moss\""));
    CHECK(has(text, "int64[] invisibleIds = [3, 1]"));
    CHECK(warned(i["warnings"], "taken out of"));
    CHECK(!warned(i["warnings"], "aren't written"));
    CHECK(near(world("Rock_1"), before, 1e-6));
    CHECK(!prop("Moss", "points").empty());
    CHECK(has(prop("Rock_1|Rock|Body", "points"), "(1, 1, 0)"));

    run("usdStageReload", {{"args", {stage}}});
    REQUIRE(graph().find("Rock_1|Rock|Body"));
    REQUIRE(graph().find("Rock_1|Moss"));
    CHECK(near(world("Rock_1"), before, 1e-6));
    CHECK(graph().find("Pebble_2|Pebble"));
}

TEST_CASE("usd stage: part of a point instance deleted leaves its prototype, an emptied one hides, and undo brings both back")
{
    UpAxisScope axis(math::UpAxis::Z);
    fresh();
    const fs::path dir = workDir("copyDelete");
    const fs::path src = copyFixture(dir, "instancing.usda");
    const std::string stage = openStage(src)["stage"];
    run("delete", {{"args", {"Rock_1|Rock|Body"}}});
    run("delete", {{"args", {"Pebble_2|Pebble"}}});
    saveAs(dir / "scene.lila");
    const json i = info(stage);
    std::string text = i["edits"];
    INFO(text);
    CHECK(has(text, "over \"Body\" (\n                    active = false"));
    CHECK(has(text, "int64[] invisibleIds = [3, 2]"));
    CHECK(warned(i["warnings"], "removed from its prototype"));
    CHECK(!warned(i["warnings"], "isn't written"));

    run("undo");
    run("undo");
    REQUIRE(graph().find("Rock_1|Rock|Body"));
    REQUIRE(graph().find("Pebble_2|Pebble"));
    saveAs(dir / "scene.lila");
    text = edits(stage);
    CHECK(!has(text, "active = false"));
    CHECK(!has(text, "[3, 2]"));
    run("usdStageUpdate");
    CHECK(graph().find("Rock_1|Rock|Body"));
}

TEST_CASE("usd stage: an instance whose edits are all undone is instanced again")
{
    UpAxisScope axis(math::UpAxis::Z);
    fresh();
    const fs::path dir = workDir("reinstance");
    const fs::path src = copyFixture(dir, "instancing.usda");
    const std::string stage = openStage(src)["stage"];
    run("setAttr", {{"args", {"InstA|Tri.translate", 1, 2, 3}}});
    saveAs(dir / "scene.lila");
    CHECK(has(edits(stage), "instanceable = false"));
    run("undo");
    saveAs(dir / "scene.lila");
    std::string text = edits(stage);
    INFO(text);
    CHECK(!has(text, "InstA"));
    CHECK(prop("InstA|Tri", "xformOp:translate").empty());

    // A deleted object inside one, brought back.
    run("delete", {{"args", {"InstB|Tri"}}});
    saveAs(dir / "scene.lila");
    CHECK(has(edits(stage), "instanceable = false"));
    run("undo");
    saveAs(dir / "scene.lila");
    CHECK(!has(edits(stage), "InstB"));

    // Across a reopen, set back by hand.
    run("setAttr", {{"args", {"InstA|Tri.translate", 4, 0, 0}}});
    saveAs(dir / "scene.lila");
    reopen(dir / "scene.lila");
    CHECK(has(edits(stage), "instanceable = false"));
    run("setAttr", {{"args", {"InstA|Tri.translate", 0, 0, 0}}});
    saveAs(dir / "scene.lila");
    CHECK(!has(edits(stage), "InstA"));

    // Still open while anything inside differs.
    run("setAttr", {{"args", {"InstA|Tri.translate", 1, 0, 0}}});
    addMesh("Extra", geo::makeCube({}), node("InstA").id());
    saveAs(dir / "scene.lila");
    run("setAttr", {{"args", {"InstA|Tri.translate", 0, 0, 0}}});
    saveAs(dir / "scene.lila");
    CHECK(has(edits(stage), "instanceable = false"));
}

TEST_CASE("usd stage: materials go out as MaterialX when the stage asks or already uses it")
{
    UpAxisScope axis(math::UpAxis::Z);
    fresh();
    const fs::path dir = workDir("materialx");
    std::string stage = openStage(copyFixture(dir, "live.usda"), {{"materialX", "openPBR"}})["stage"];
    Node* blue = materials::create(graph(), "Blue");
    graph().setValue({blue->id(), material::kBaseColor, -1}, glm::dvec3(0.1, 0.2, 0.9));
    geo::Mesh m = *outMesh("Geo");
    m.faceAttrs().remove(geo::attr::kMaterial);
    m.materialNames().clear();
    materials::assign(m, "Blue", {});
    graph().setValue({node("Geo").id(), MeshNode::kInMesh, -1}, MeshPtr(std::make_shared<geo::Mesh>(std::move(m))));
    saveAs(dir / "scene.lila");
    std::string text = edits(stage);
    INFO(text);
    CHECK(has(text, "def Material \"Blue\""));
    CHECK(has(text, "outputs:mtlx:surface.connect"));
    CHECK(has(text, "ND_open_pbr_surface_surfaceshader"));
    CHECK(has(text, "UsdPreviewSurface"));

    // A stage of MaterialX materials: nothing written until an edit, which goes out as MaterialX again.
    fresh();
    fs::copy(fixture("tex"), dir / "tex", fs::copy_options::recursive | fs::copy_options::overwrite_existing);
    copyFixture(dir, "mtlx_library.mtlx");
    stage = openStage(copyFixture(dir, "mtlx_materials.usda"))["stage"];
    saveAs(dir / "scene2.lila");
    CHECK(!has(edits(stage), "def"));
    CHECK(!has(edits(stage), "over"));
    Node* chrome = materials::find(graph(), "Chrome");
    REQUIRE(chrome);
    graph().setValue({chrome->id(), material::kBaseColor, -1}, glm::dvec3(0.7, 0.7, 0.75));
    saveAs(dir / "scene2.lila");
    text = edits(stage);
    CHECK(has(text, "ND_open_pbr_surface_surfaceshader"));
    CHECK(has(text, "(0.7, 0.7, 0.75)"));
    run("usdStageReload", {{"args", {stage}}});
    REQUIRE(materials::find(graph(), "Chrome"));
    CHECK(near(materials::find(graph(), "Chrome")->as<glm::dvec3>(material::kBaseColor), {0.7, 0.7, 0.75}, 1e-6));
}
