#include <doctest/doctest.h>

#include "FakeServer.h"
#include "TestUtil.h"

#include "core/MainQueue.h"
#include "link/UnrealLink.h"
#include "ops/Command.h"
#include "rigmath/ExpressionVm.h"
#include "rigmath/MathNodes.h"
#include "scene/Graph.h"
#include "geo/Mesh.h"
#include "scene/Modifiers.h"
#include "scene/Morphs.h"
#include "scene/SceneCommands.h"

using namespace lila;
using namespace lila::link;
using namespace linktest;
using json = nlohmann::json;

namespace exprRigTests
{
void setupExpressionRigTest()
{
    static bool once = [] {
        mainq::init();
        ops::registerBuiltins();
        scene::types::registerBuiltins();
        scene::registerCommands();
        rigmath::registerTypes();
        rigmath::registerCommands();
        link::registerCommands();
        return true;
    }();
    (void)once;
}

json rigCmd(const char* name, json call = json::object()) { return ops::execute(name, call); }

void nudgePoint0(const std::string& mesh, const glm::dvec3& by)
{
    scene::Graph& g = scene::Graph::current();
    const scene::PlugRef plug = scene::modifiers::editPlug(g, g.find(mesh)->id());
    auto m = std::make_shared<geo::Mesh>(*std::get<scene::MeshPtr>(g.value(plug)));
    m->setPosition(0, m->position(0) + by);
    g.setValue(plug, scene::MeshPtr(m));
}

void buildCharacter()
{
    rigCmd("newScene");
    rigCmd("group", {{"empty", true}, {"name", "Char"}});
    rigCmd("joint", {{"position", {0, 0, 0}}, {"name", "root"}, {"parent", "Char"}});
    rigCmd("joint", {{"position", {0, 50, 0}}, {"name", "jaw"}, {"parent", "root"}});
    rigCmd("spaceLocator", {{"name", "ctrl"}});
    rigCmd("parent", {{"args", {"ctrl", "Char"}}});
    rigCmd("addAttr", {{"args", {"ctrl"}}, {"longName", "open"}, {"keyable", true}, {"minValue", 0.0}, {"maxValue", 1.0}});
    rigCmd("spaceLocator", {{"name", "fx"}});
    rigCmd("parent", {{"args", {"fx", "Char"}}});
    rigCmd("addAttr", {{"args", {"fx"}}, {"longName", "glow"}, {"keyable", true}});
    rigCmd("spaceLocator", {{"name", "away"}});
    rigCmd("expression", {{"args", {"fx.glow"}}, {"string", "jaw.rx * 2"}});
    rigCmd("expression", {{"args", {"jaw.rx"}}, {"string", "ctrl.open * 30 + time"}});
    rigCmd("expression", {{"args", {"root.ry"}}, {"string", "ctrl.open * 10"}});
    rigCmd("expression", {{"args", {"jaw.rz"}}, {"string", "fabs(ctrl.open)"}});
    rigCmd("expression", {{"args", {"away.tx"}}, {"string", "ctrl.open"}});
}

FakeServer::Handler rigEditor(json result)
{
    return [result](const json& req) -> std::optional<json> {
        const std::string m = req["method"];
        if (m == "hello")
            return FakeServer::reply(req, {{"project", "Chars"}, {"projectDir", "C:/Projects/Chars/"}, {"pluginVersion", "0.6.0"}, {"features", {"progress", "expressionRig"}}});
        if (m == "runPython")
            return FakeServer::reply(req, {{"ok", true}, {"result", ""}, {"log", {{{"type", "Info"}, {"text", "LILA_RESULT:" + result.dump()}}}}});
        if (m == "expressionPostProcess")
            return FakeServer::reply(req, {{"animBlueprint", "/Game/Chars/ABP_Char_Expressions"}, {"warnings", json::array()}});
        return json{{"jsonrpc", "2.0"}, {"id", req["id"]}, {"error", {{"code", -32601}, {"message", "Unknown method"}}}};
    };
}
} // namespace exprRigTests
using namespace exprRigTests;

TEST_CASE("unrealExpressionRig binds bones, curves and controls and orders expressions by what they read")
{
    setupExpressionRigTest();
    buildCharacter();
    json r = rigCmd("unrealExpressionRig", {{"args", {"Char"}}, {"skeletalMesh", "/Game/Chars/SK_Char"}, {"dryRun", true}});
    CHECK(r["rig"] == "/Game/Chars/CR_Char_Expressions");
    // glow reads jaw.rx, so jaw's expression runs first.
    const json& plugs = r["expressions"];
    REQUIRE(plugs.size() == 3);
    auto at = [&](const char* p) { return std::find(plugs.begin(), plugs.end(), json(p)) - plugs.begin(); };
    CHECK(at("jaw.rotateX") < at("fx.glow"));
    CHECK(at("root.rotateY") < 3);
    REQUIRE(r["skipped"].size() == 1);
    CHECK(r["skipped"][0]["plug"] == "jaw.rotateZ");
    CHECK(r["skipped"][0]["why"] == "runs as Python");
    REQUIRE(r["controls"].size() == 1);
    CHECK(r["controls"][0]["name"] == "ctrl_open");
    CHECK(r["controls"][0]["max"] == 1.0);
    CHECK(r["curves"] == json{"fx_glow"});

    const json& exprs = r["payload"]["expressions"];
    for (const json& e : exprs)
    {
        const json& b = e["bindings"];
        CHECK(b["basis"].size() == 9);
        if (e["plug"] == "jaw.rotateX")
        {
            CHECK(b["target"]["kind"] == "bone");
            CHECK(b["target"]["name"] == "jaw");
            CHECK(b["target"]["channel"] == 3);
            CHECK_FALSE(b["target"].contains("bindLocal"));
            CHECK(b["inputs"][0]["kind"] == "control");
            CHECK(e["program"].get<std::string>().find(" t") != std::string::npos);
        }
        if (e["plug"] == "fx.glow")
        {
            CHECK(b["target"] == json{{"kind", "curve"}, {"name", "fx_glow"}});
            CHECK(b["inputs"][0]["kind"] == "bone");
        }
        if (e["plug"] == "root.rotateY")
        {
            // A root bone carries its bind pose so LilaLink can solve the frame the import put it in.
            CHECK(b["target"]["bindLocal"].size() == 9);
            CHECK(b["target"]["bindTranslation"].size() == 3);
        }
    }
}

TEST_CASE("unrealExpressionRig turns a mapped morph target's weight curve into its shapes' curves")
{
    setupExpressionRigTest();
    rigCmd("newScene");
    rigCmd("group", {{"empty", true}, {"name", "Char"}});
    rigCmd("joint", {{"position", {0, 0, 0}}, {"name", "root"}, {"parent", "Char"}});
    const std::string face = rigCmd("polyCube", {{"name", "Face"}})[0];
    const std::string lid = rigCmd("polyCube", {{"name", "Lid"}})[0];
    const std::string half = rigCmd("polyCube", {{"name", "LidHalf"}})[0];
    nudgePoint0(lid, {10, 10, 0});
    nudgePoint0(half, {2.9289, 7.0711, 0});
    rigCmd("parent", {{"args", {face, "Char"}}});
    rigCmd("morph", {{"args", {lid, face}}});
    const std::string t = rigCmd("morphTargets", {{"args", {face}}})[0]["node"];
    rigCmd("addInBetween", {{"args", {t, half}}, {"weight", 0.5}});
    rigCmd("morphPath", {{"args", {t}}, {"interpolation", "arc"}, {"response", "sineEaseInOut"}});

    json r = rigCmd("unrealExpressionRig", {{"args", {"Char"}}, {"skeletalMesh", "/Game/Chars/SK_Char"}, {"dryRun", true}});
    const json shapes = rigCmd("morphPath", {{"args", {t}}})["shapes"];
    REQUIRE(shapes.size() >= 2);
    CHECK(r["morphShapes"].size() == shapes.size());
    // The shape named like the target reads and rewrites its curve, so it runs after the others.
    CHECK(r["morphShapes"].back() == "Lid");

    scene::Graph& g = scene::Graph::current();
    const scene::NodeId target = g.find(t)->id();
    for (const json& e : r["payload"]["expressions"])
    {
        const json& b = e["bindings"];
        CHECK(b["inputs"][0] == json{{"kind", "curve"}, {"name", "Lid"}});
        lila_expr::Program p;
        std::string why;
        REQUIRE_MESSAGE(lila_expr::parse(e["program"].get<std::string>(), p, &why), why);
        const size_t k = std::find(shapes.begin(), shapes.end(), b["target"]["name"]) - shapes.begin();
        REQUIRE(k < shapes.size());
        for (double w : {0.0, 0.2, 0.5, 0.8, 1.0})
        {
            const double in[1] = {w};
            CHECK(lila_expr::evaluate(p, in, 0.0) == doctest::Approx(scene::morphs::shapeWeights(g, target, w)[k]).epsilon(0.01));
        }
    }
}

TEST_CASE("unrealMorphPaths sends each mapped target's shape tables for the anim node")
{
    setupExpressionRigTest();
    rigCmd("newScene");
    rigCmd("group", {{"empty", true}, {"name", "Char"}});
    rigCmd("joint", {{"position", {0, 0, 0}}, {"name", "root"}, {"parent", "Char"}});
    const std::string face = rigCmd("polyCube", {{"name", "Face"}})[0];
    const std::string brow = rigCmd("polyCube", {{"name", "Brow"}})[0];
    const std::string plain = rigCmd("polyCube", {{"name", "Plain"}})[0];
    nudgePoint0(brow, {0, 10, 0});
    nudgePoint0(plain, {0, 0, 10});
    rigCmd("parent", {{"args", {face, "Char"}}});
    rigCmd("morph", {{"args", {brow, plain, face}}});
    const std::string t = rigCmd("morphTargets", {{"args", {face}}})[0]["node"];

    // Plain targets need no node.
    CHECK_THROWS(rigCmd("unrealMorphPaths", {{"args", {"Char"}}, {"skeletalMesh", "/Game/Chars/SK_Char"}, {"dryRun", true}}));

    rigCmd("morphPath", {{"args", {t}}, {"response", "quadraticEaseIn"}});
    json r = rigCmd("unrealMorphPaths", {{"args", {"Char"}}, {"skeletalMesh", "/Game/Chars/SK_Char"}, {"dryRun", true}});
    REQUIRE(r["paths"] == 1);
    const json& path = r["payload"]["paths"][0];
    CHECK(path["curve"] == "Brow");
    REQUIRE(path["shapes"].size() == 1);
    CHECK(path["shapes"][0]["name"] == "Brow");
    const json& w = path["weights"];
    const json& v = path["shapes"][0]["values"];
    REQUIRE(w.size() == v.size());
    for (size_t i = 0; i < w.size(); ++i)
        CHECK(v[i].get<double>() == doctest::Approx(w[i].get<double>() * w[i].get<double>()));

    FakeServer server([](const json& req) -> std::optional<json> {
        const std::string m = req["method"];
        if (m == "hello")
            return FakeServer::reply(req, {{"project", "Chars"}, {"projectDir", "C:/Projects/Chars/"}, {"pluginVersion", "0.6.0"}, {"features", {"morphPaths"}}});
        if (m == "morphPaths")
        {
            CHECK(req["params"]["skeletalMesh"] == "/Game/Chars/SK_Char");
            CHECK(req["params"]["paths"].size() == 1);
            return FakeServer::reply(req, {{"paths", 1}, {"animBlueprint", "/Game/Chars/ABP_Char_MorphPaths"}, {"warnings", json::array()}});
        }
        return json{{"jsonrpc", "2.0"}, {"id", req["id"]}, {"error", {{"code", -32601}, {"message", "Unknown method"}}}};
    });
    Link::ConnectOptions o;
    o.port = server.port();
    Link::get().connect(o);
    r = rigCmd("unrealMorphPaths", {{"args", {"Char"}}, {"skeletalMesh", "/Game/Chars/SK_Char"}});
    CHECK(r["animBlueprint"] == "/Game/Chars/ABP_Char_MorphPaths");
    Link::get().disconnect();
}

TEST_CASE("unrealExpressionRig builds the rig over the link and wires the post-process Anim Blueprint")
{
    setupExpressionRigTest();
    buildCharacter();
    FakeServer server(rigEditor({{"rig", "/Game/Chars/CR_Char_Expressions"}, {"nodes", {"LilaExpr_jaw_rotatex"}}, {"errors", json::array()}}));
    Link::ConnectOptions o;
    o.port = server.port();
    Link::get().connect(o);
    json r = rigCmd("unrealExpressionRig", {{"args", {"Char"}}, {"skeletalMesh", "/Game/Chars/SK_Char"}});
    CHECK(r["animBlueprint"] == "/Game/Chars/ABP_Char_Expressions");
    CHECK(r["unreal"]["nodes"][0] == "LilaExpr_jaw_rotatex");
    json python, post;
    for (const json& req : server.requests())
    {
        if (req["method"] == "runPython")
            python = req["params"];
        if (req["method"] == "expressionPostProcess")
            post = req["params"];
    }
    const std::string code = python["code"];
    CHECK(code.find("lila_link_expressions.build_and_print(") != std::string::npos);
    CHECK(post["rig"] == "/Game/Chars/CR_Char_Expressions");
    CHECK(post["skeletalMesh"] == "/Game/Chars/SK_Char");
    Link::get().disconnect();
}

TEST_CASE("unrealExpressionRig fails with Unreal's error")
{
    setupExpressionRigTest();
    buildCharacter();
    FakeServer server(rigEditor({{"errors", {"no skeletal mesh at /Game/Chars/SK_Char"}}}));
    Link::ConnectOptions o;
    o.port = server.port();
    Link::get().connect(o);
    CHECK_THROWS_WITH(rigCmd("unrealExpressionRig", {{"args", {"Char"}}, {"skeletalMesh", "/Game/Chars/SK_Char"}}),
                      doctest::Contains("no skeletal mesh"));
    Link::get().disconnect();
}
