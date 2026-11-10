#include "TestUtil.h"

#include "bake/BakeNodes.h"
#include "curve/CurveNodes.h"
#include "gn/Gn.h"
#include "ops/Undo.h"
#include "scene/Anim.h"
#include "scene/Relocate.h"
#include "scene/RotationCurve.h"
#include "scene/SceneData.h"
#include "scene/Take.h"
#include "tex/TextureSet.h"

#include <map>
#include <set>

using namespace rigtest;
using namespace lila::scene;

namespace
{
const std::vector<double> kTimes = {0.0, 0.2, 0.4, 0.55, 0.8, 1.1};

/**
 * A leg built far from the origin: a root joint with three joints under it, a cylinder skinned to them, a 2-bone IK
 * handle on hip to ankle, a bend and a soft mod on the cylinder, a prop parent-constrained to the ankle and a locator
 * aim-constrained at a target with a tilted world up. The root's translate and rotate, and the IK handle, are keyed.
 */
void buildRig()
{
    run("newScene");
    run("select", {{"args", json::array()}});
    run("joint", {{"position", v3(5000, 100, -3000)}, {"name", "root"}});
    run("joint", {{"position", v3(5000, 90, -3000)}, {"name", "hip"}});
    run("joint", {{"position", v3(5000, 50, -2995)}, {"name", "knee"}});
    run("joint", {{"position", v3(5000, 10, -3000)}, {"name", "ankle"}});
    run("polyCylinder", {{"name", "Leg"}, {"radius", 6}, {"height", 90}, {"subdivisionsHeight", 12}, {"caps", false}});
    run("setAttr", {{"args", {"Leg.translate", v3(5000, 55, -3000)}}});
    run("skinCluster", {{"args", {"root", "Leg"}}});
    run("select", {{"args", json::array()}});
    run("ikHandle", {{"startJoint", "hip"}, {"endEffector", "ankle"}, {"name", "legIk"}});
    run("nonlinear", {{"args", {"Leg"}}, {"type", "bend"}, {"params", {{"angle", 15.0}}}});
    json soft = run("softMod", {{"args", {"Leg"}}, {"position", v3(5000, 70, -3000)}, {"radius", 25}});
    run("setAttr", {{"args", {soft[0].get<std::string>() + ".translate", v3(5000, 70, -2996)}}});

    run("spaceLocator", {{"name", "Prop"}});
    run("setAttr", {{"args", {"Prop.translate", v3(5010, 12, -3000)}}});
    run("parentConstraint", {{"args", {"ankle", "Prop"}}});
    run("spaceLocator", {{"name", "Target"}});
    run("setAttr", {{"args", {"Target.translate", v3(5100, 60, -3020)}}});
    run("spaceLocator", {{"name", "Look"}});
    run("setAttr", {{"args", {"Look.translate", v3(5040, 60, -3050)}}});
    std::string aim = run("aimConstraint", {{"args", {"Target", "Look"}}}).get<std::string>();
    run("setAttr", {{"args", {aim + ".worldUpVector", v3(0.3, 1, 0.2)}}});

    for (auto [t, x] : std::vector<std::pair<double, double>>{{0.0, 5000}, {0.4, 5030}, {0.8, 5010}})
        run("setKeyframe", {{"args", {"root.tx"}}, {"time", t}, {"value", x}});
    for (auto [t, z] : std::vector<std::pair<double, double>>{{0.0, -3000}, {0.8, -2980}})
        run("setKeyframe", {{"args", {"root.tz"}}, {"time", t}, {"value", z}});
    for (auto [t, r] : std::vector<std::pair<double, double>>{{0.0, 0}, {0.6, 25}})
        run("setKeyframe", {{"args", {"root.ry"}}, {"time", t}, {"value", r}});
    for (auto [t, y] : std::vector<std::pair<double, double>>{{0.0, 10}, {0.4, 30}, {0.8, 12}})
        run("setKeyframe", {{"args", {"legIk.ty"}}, {"time", t}, {"value", y}});
    run("currentTime", {{"args", {0.0}}});
}

struct State
{
    std::map<std::string, glm::dmat4> worlds;
    std::map<std::string, std::vector<glm::dvec3>> points;
};

/** Every transform's world matrix and every mesh's world-space deformed points at each of kTimes. */
std::vector<State> sample()
{
    std::vector<State> out;
    const double now = graph().time();
    for (double t : kTimes)
    {
        graph().setTime(t, false);
        graph().evaluate();
        State s;
        for (NodeId id : graph().nodes())
        {
            const Node* n = graph().node(id);
            if (!n->type().dag)
                continue;
            const glm::dmat4 w = n->as<glm::dmat4>(Transform::kWorldMatrix);
            s.worlds[n->fullPath()] = w;
            if (n->type().isA("mesh"))
                if (const MeshPtr& m = n->as<MeshPtr>(MeshNode::kOutMesh))
                    for (const glm::dvec3& p : m->positions())
                        s.points[n->fullPath()].push_back(glm::dvec3(w * glm::dvec4(p, 1.0)));
        }
        out.push_back(std::move(s));
    }
    graph().setTime(now, false);
    graph().evaluate();
    return out;
}

glm::dmat4 matrixOf(const json& j)
{
    glm::dmat4 m;
    for (int c = 0; c < 4; ++c)
        for (int r = 0; r < 4; ++r)
            m[c][r] = j[size_t(c * 4 + r)].get<double>();
    return m;
}

/**
 * The largest distance between after and m times before (before itself for the names in still), over every transform's
 * axes and origin and every point.
 */
double worstError(const std::vector<State>& before, const std::vector<State>& after, const glm::dmat4& move, const std::set<std::string>& still = {})
{
    double worst = 0.0;
    REQUIRE(before.size() == after.size());
    for (size_t i = 0; i < before.size(); ++i)
    {
        REQUIRE(before[i].worlds.size() == after[i].worlds.size());
        for (const auto& [name, w] : before[i].worlds)
        {
            const glm::dmat4 want = (still.contains(name) ? glm::dmat4(1.0) : move) * w;
            const glm::dmat4& got = after[i].worlds.at(name);
            for (int c = 0; c < 4; ++c)
                worst = std::max(worst, glm::length(glm::dvec3(got[c]) - glm::dvec3(want[c])));
        }
        for (const auto& [name, pts] : before[i].points)
        {
            const auto& got = after[i].points.at(name);
            REQUIRE(got.size() == pts.size());
            const glm::dmat4 m = still.contains(name) ? glm::dmat4(1.0) : move;
            for (size_t p = 0; p < pts.size(); ++p)
                worst = std::max(worst, glm::length(got[p] - glm::dvec3(m * glm::dvec4(pts[p], 1.0))));
        }
    }
    return worst;
}

/** Every stored value of every node, for checking undo puts back exactly what was there. */
std::map<std::string, std::vector<PlugValue>> storedValues()
{
    std::map<std::string, std::vector<PlugValue>> out;
    for (NodeId id : graph().nodes())
    {
        const Node* n = graph().node(id);
        auto& v = out[n->fullPath() + ":" + n->type().name];
        for (int a = 0; a < int(n->type().attrs.size()); ++a)
        {
            // Connected plugs hold whatever was pulled last, which depends on the time evaluated.
            bool connected = graph().source({id, a, -1}).has_value();
            for (int c = 0; c < 3; ++c)
                connected |= graph().source({id, a, c}).has_value();
            if (!(n->type().attrs[size_t(a)].flags & AttrStorable) || connected)
                continue;
            v.push_back(n->value(a));
            for (const auto& [i, e] : n->elements(a))
                if (!graph().source({id, a, -1, i}))
                    v.push_back(e);
        }
    }
    return out;
}

bool sameStates(const std::vector<State>& a, const std::vector<State>& b)
{
    for (size_t i = 0; i < a.size(); ++i)
        if (a[i].worlds != b[i].worlds || a[i].points != b[i].points)
            return false;
    return true;
}

void registerTestTypes()
{
    if (types::find("relocateTestWorldThing"))
        return;
    NodeType world;
    world.name = "relocateTestWorldThing";
    world.ownAttrs = {{.name = "anchor", .type = PlugType::Matrix, .defaultValue = glm::dmat4(1.0), .flags = AttrInput | AttrStorable}};
    world.create = [] { return std::make_unique<Node>(); };
    types::registerType(std::move(world));

    // A handler with a bug: it nudges a locator its node knows nothing about.
    NodeType saboteur;
    saboteur.name = "relocateTestSaboteur";
    saboteur.create = [] { return std::make_unique<Node>(); };
    types::registerType(std::move(saboteur));
    relocate::registerHandler("relocateTestSaboteur", {.plan = [](const relocate::Move& mv, NodeId) -> relocate::Edit {
                                  Graph* g = mv.graph;
                                  NodeId prop = g->find("Target")->id();
                                  return [g, prop] {
                                      glm::dvec3 t = g->node(prop)->as<glm::dvec3>(Transform::kTranslate);
                                      g->setValue({prop, Transform::kTranslate}, t + glm::dvec3(0.001, 0, 0));
                                  };
                              }});
}
} // namespace

TEST_CASE("relocateScene moves a skinned, constrained, IK, deformed and keyed rig by a translation and yaw exactly")
{
    buildRig();
    // Turn the root so it has a heading for rotation='yaw' to take back out.
    run("setAttr", {{"args", {"root.jointOrient", v3(0, 35, 0)}}});
    const auto values = storedValues();
    const std::vector<State> before = sample();
    const size_t steps = UndoStack::get().undoCount();

    json r = run("relocateScene", {{"from", "root"}, {"rotation", "yaw"}});
    INFO(r.dump());
    const glm::dmat4 m = matrixOf(r["matrix"]);
    // The root's pivot lands on the origin and its heading lines up with world X.
    checkNear(glm::dvec3(m * glm::dvec4(5000, 100, -3000, 1)), {0, 0, 0});
    CHECK(std::abs(r["rotation"][1].get<double>()) > 10.0);
    CHECK(r["verified"]["points"].get<size_t>() > 0);
    CHECK(UndoStack::get().undoCount() == steps + 1);
    CHECK(r["handled"]["skinCluster"] == 1);
    CHECK(r["handled"]["skinInfluence"] == 4);
    CHECK(r["handled"]["bendDeformer"] == 1);
    CHECK(r["handled"]["softMod"] == 1);

    const std::vector<State> after = sample();
    CHECK(worstError(before, after, m) < 1e-6);
    // The keys moved rather than being replaced by static values.
    CHECK(anim::curveFor(graph(), {node("root").id(), Transform::kTranslate, 0}) != kNoNode);
    CHECK(anim::curveFor(graph(), {node("legIk").id(), Transform::kTranslate, 1}) != kNoNode);

    run("undo");
    CHECK(sameStates(sample(), before));
    CHECK(storedValues() == values);
    run("redo");
    CHECK(worstError(before, sample(), m) < 1e-6);
}

TEST_CASE("relocateScene by an offset moves keys, locked channels and the display stays put")
{
    buildRig();
    run("setAttr", {{"args", {"Leg.translate", v3(5000, 55, -3000)}}});
    graph().setLocked({node("Target").id(), Transform::kTranslate, -1}, true);
    const std::vector<State> before = sample();
    json r = run("relocateScene", {{"offset", v3(-5000, 0, 3000)}, {"axes", "xz"}});
    INFO(r.dump());
    const glm::dmat4 m = matrixOf(r["matrix"]);
    checkNear(glm::dvec3(m[3]), {-5000, 0, 3000});
    CHECK(worstError(before, sample(), m) < 1e-6);
    CHECK(graph().isLocked({node("Target").id(), Transform::kTranslate, -1}));
    bool lockNoted = false;
    for (const json& n : r["notes"])
        lockNoted |= n.get<std::string>().find("Target.translate") != std::string::npos;
    CHECK(lockNoted);
}

TEST_CASE("relocateScene refuses a node type it doesn't know and changes nothing")
{
    registerTestTypes();
    buildRig();
    run("createNode", {{"args", {"relocateTestWorldThing"}}, {"name", "Thing"}});
    const auto values = storedValues();
    const size_t steps = UndoStack::get().undoCount();
    try
    {
        run("relocateScene", {{"offset", v3(-5000, 0, 3000)}});
        FAIL("relocateScene should have refused");
    }
    catch (const CommandError& e)
    {
        CHECK(std::string(e.what()).find("relocateTestWorldThing") != std::string::npos);
        CHECK(std::string(e.what()).find("Thing") != std::string::npos);
    }
    CHECK(storedValues() == values);
    CHECK(UndoStack::get().undoCount() == steps);
}

TEST_CASE("relocateScene puts everything back when its check after moving fails")
{
    registerTestTypes();
    buildRig();
    run("createNode", {{"args", {"relocateTestSaboteur"}}, {"name", "Saboteur"}});
    const auto values = storedValues();
    const std::vector<State> before = sample();
    const size_t steps = UndoStack::get().undoCount();
    try
    {
        run("relocateScene", {{"offset", v3(-5000, 0, 3000)}});
        FAIL("relocateScene should have failed its check");
    }
    catch (const CommandError& e)
    {
        CHECK(std::string(e.what()).find("put back") != std::string::npos);
        CHECK(std::string(e.what()).find("Target") != std::string::npos);
    }
    CHECK(storedValues() == values);
    CHECK(sameStates(sample(), before));
    CHECK(UndoStack::get().undoCount() == steps);
    run("delete", {{"args", {"Saboteur"}}});
}

TEST_CASE("relocateScene refuses a yaw on separate Euler keys it can't turn exactly")
{
    run("newScene");
    run("polyCube", {{"name", "Crate"}});
    run("setAttr", {{"args", {"Crate.translate", v3(800, 0, 0)}}});
    run("setKeyframe", {{"args", {"Crate.rx"}}, {"time", 0.0}, {"value", 0.0}});
    run("setKeyframe", {{"args", {"Crate.rx"}}, {"time", 1.0}, {"value", 40.0}});
    run("setAttr", {{"args", {"Crate.rotate", v3(0, 30, 0)}}});
    const auto values = storedValues();
    CHECK_THROWS_WITH_AS(run("relocateScene", {{"args", {"Crate"}}, {"from", "Crate"}, {"rotation", "yaw"}}),
                         doctest::Contains("convertRotationCurves"), CommandError);
    CHECK(storedValues() == values);
    // Its outermost axis turns exactly.
    run("setAttr", {{"args", {"Crate.rotateOrder", 2}}});
    const std::vector<State> before = sample();
    json r = run("relocateScene", {{"args", {"Crate"}}, {"from", "Crate"}, {"rotation", "yaw"}});
    CHECK(worstError(before, sample(), matrixOf(r["matrix"])) < 1e-6);
}

TEST_CASE("relocateScene moves only the named hierarchies, and refuses one bound to something left behind")
{
    run("newScene");
    run("select", {{"args", json::array()}});
    run("joint", {{"position", v3(300, 0, 0)}, {"name", "a1"}});
    run("joint", {{"position", v3(300, 20, 0)}, {"name", "a2"}});
    run("group", {{"args", {"a1"}}, {"name", "Hero"}});
    run("polyCube", {{"name", "Rock"}});
    run("setAttr", {{"args", {"Rock.translate", v3(900, 0, 0)}}});
    const glm::dvec3 rock = pos("Rock");
    json r = run("relocateScene", {{"from", "a1"}, {"nodes", {"Hero"}}});
    checkNear(pos("a1"), {0, 0, 0});
    checkNear(pos("a2"), {0, 20, 0});
    checkNear(pos("Rock"), rock);
    run("undo");

    // A mesh outside the hierarchy skinned to its joints would be torn from them.
    run("polyCylinder", {{"name", "Arm"}, {"radius", 2}, {"height", 20}});
    run("setAttr", {{"args", {"Arm.translate", v3(300, 10, 0)}}});
    run("skinCluster", {{"args", {"a1", "Arm"}}});
    const auto values = storedValues();
    CHECK_THROWS_WITH_AS(run("relocateScene", {{"from", "a1"}, {"nodes", {"Hero"}}}), doctest::Contains("include both"), CommandError);
    CHECK(storedValues() == values);
    const std::vector<State> before = sample();
    r = run("relocateScene", {{"from", "a1"}, {"nodes", {"Hero", "Arm"}}});
    CHECK(worstError(before, sample(), matrixOf(r["matrix"]), {"|Rock"}) < 1e-6);
}

TEST_CASE("relocateScene turns the keys of inactive takes, rotation curves and nodes in a world space")
{
    run("newScene");
    run("select", {{"args", json::array()}});
    run("joint", {{"name", "root"}, {"position", v3(2000, 0, 500)}});
    run("joint", {{"name", "chest"}, {"position", v3(2000, 100, 500)}});
    run("joint", {{"name", "hand"}, {"position", v3(2030, 110, 505)}});
    auto keyRoot = [](double x1, double z1) {
        run("setKeyframe", {{"args", {"root.tx", "root.ty", "root.tz"}}, {"time", 0.0}});
        run("setKeyframe", {{"args", {"root.tx"}}, {"time", 1.0}, {"value", x1}});
        run("setKeyframe", {{"args", {"root.ty"}}, {"time", 0.5}, {"value", 8.0}});
        run("setKeyframe", {{"args", {"root.tz"}}, {"time", 1.0}, {"value", z1}});
    };
    keyRoot(2040, 520);
    run("createTake", {{"name", "Walk"}, {"start", 0.0}, {"end", 1.0}});
    run("createTake", {{"name", "Run"}, {"start", 0.0}, {"end", 1.0}});
    run("setActiveTake", {{"args", {"Run"}}});
    run("currentTime", {{"args", {0.0}}});
    keyRoot(2150, 430);
    run("setActiveTake", {{"args", {"Walk"}}});
    run("addSpace", {{"args", {"hand"}}, {"space", {"world"}}});
    run("switchSpace", {{"args", {"hand"}}, {"space", "world"}});

    run("spaceLocator", {{"name", "Crane"}});
    run("setAttr", {{"args", {"Crane.translate", v3(2100, 40, 520)}}});
    run("setAttr", {{"args", {"Crane.rotationInterpolation", 2}}});
    run("setAttr", {{"args", {"Crane.rotate", v3(20, 30, 10)}}});
    run("setKeyframe", {{"args", {"Crane.rotate"}}, {"time", 0.0}});
    run("setAttr", {{"args", {"Crane.rotate", v3(0, 90, 0)}}});
    run("setKeyframe", {{"args", {"Crane.rotate"}}, {"time", 1.0}});
    run("currentTime", {{"args", {0.0}}});
    REQUIRE(anim::isRotationCurve(*graph().node(graph().source({node("Crane").id(), Transform::kRotate, -1})->node)));

    const NodeId runTake = node("Run").id();
    std::vector<State> walkBefore = sample(), runBefore;
    anim::withTake(graph(), runTake, [&] { runBefore = sample(); });
    json r = run("relocateScene", {{"args", {"Crane"}}, {"from", "Crane"}, {"rotation", "full"}});
    INFO(r.dump());
    const glm::dmat4 m = matrixOf(r["matrix"]);
    CHECK(glm::length(glm::dvec3(m * glm::dvec4(2100, 40, 520, 1))) < 1e-9);
    CHECK(r["handled"]["spaceSwitch"] == 1);
    CHECK(worstError(walkBefore, sample(), m) < 1e-6);
    std::vector<State> runAfter;
    anim::withTake(graph(), runTake, [&] { runAfter = sample(); });
    CHECK(worstError(runBefore, runAfter, m) < 1e-6);
}

TEST_CASE("relocateScene dryRun plans without changing anything")
{
    buildRig();
    const auto values = storedValues();
    json r = run("relocateScene", {{"from", "root"}, {"dryRun", true}});
    CHECK(r["dryRun"] == true);
    checkNear(glm::dvec3(matrixOf(r["matrix"])[3]), {-5000, -100, 3000});
    CHECK(storedValues() == values);
}

TEST_CASE("the display origin is scene data, undoable and changes nothing in the scene")
{
    run("newScene");
    run("polyCube", {{"name", "Box"}});
    run("setAttr", {{"args", {"Box.translate", v3(120, 0, -40)}}});
    CHECK(run("displayOrigin").is_null());
    run("setDisplayOrigin", {{"args", {"Box"}}, {"from", "pivot"}});
    checkNear(relocate::displayOrigin(), {120, 0, -40});
    CHECK(data::get("displayOrigin").is_array());
    checkNear(pos("Box"), {120, 0, -40});
    run("setDisplayOrigin", {{"clear", true}});
    CHECK_FALSE(relocate::hasDisplayOrigin());
    run("undo");
    checkNear(relocate::displayOrigin(), {120, 0, -40});
}

TEST_CASE("geometry-nodes modifier types are classified whether or not they read other objects")
{
    for (const char* name : {"gn_relocateLocal", "gn_relocateObjectInfo"})
    {
        NodeType t;
        t.name = name;
        t.ownAttrs = {{.name = "groupDefinition", .type = PlugType::String, .defaultValue = std::string()}};
        if (std::string(name) == "gn_relocateObjectInfo")
            t.ownAttrs.push_back({.name = "obj_Rock_matrix", .type = PlugType::Matrix, .defaultValue = glm::dmat4(1.0)});
        t.create = [] { return std::make_unique<Node>(); };
        if (!types::find(name))
            types::registerType(std::move(t));
    }
    CHECK(relocate::handles("gn_relocateLocal"));
    CHECK(relocate::handles("gn_relocateObjectInfo"));
}

TEST_CASE("every node type is known to relocateScene or deliberately refused")
{
    gn::registerTypes();
    curve::registerTypes();
    tex::registerTypes();
    bake::registerTypes();
    // Types that build world-space geometry from world-space curves, hold an edit in progress, or may hold a world-space
    // matrix of their own (the matrix math nodes): relocateScene refuses scenes holding them rather than guess.
    const std::set<std::string> refused = {"polyEdgeToCurve", "skeletonEdit", "relocateTestWorldThing", "addMatrix", "aimMatrix", "blendMatrix",
                                           "composeMatrix", "decomposeMatrix", "fourByFourMatrix", "holdMatrix", "inverseMatrix", "multMatrix",
                                           "pickMatrix", "transposeMatrix", "wtAddMatrix"};
    std::vector<std::string> unknown;
    for (const NodeType* t : types::all())
        if (!relocate::handles(t->name) && !refused.contains(t->name))
            unknown.push_back(t->name);
    INFO("classify these in Relocate.cpp registerBuiltins: " << json(unknown).dump());
    CHECK(unknown.empty());
}
