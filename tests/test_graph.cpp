#include <doctest/doctest.h>

#include <atomic>

#include "TestUtil.h"

#include "ops/Command.h"
#include "ops/Undo.h"
#include "scene/Graph.h"
#include "scene/Naming.h"
#include "scene/Rig.h"
#include "scene/Transform.h"
#include "scene/TransformOps.h"

using namespace lila;
using namespace lila::scene;
using namespace testutil;

TEST_CASE("names are unique and undo restores them")
{
    run("newScene");
    CHECK(run("spaceLocator", {{"name", "loc"}}) == "loc");
    CHECK(run("spaceLocator", {{"name", "loc"}}) == "loc1");
    CHECK(run("spaceLocator", {{"name", "loc1"}}) == "loc2");
    run("undo");
    CHECK_FALSE(run("objExists", {{"args", {"loc2"}}}).get<bool>());
    run("redo");
    CHECK(run("objExists", {{"args", {"loc2"}}}).get<bool>());
}

TEST_CASE("hierarchy keeps world position when parenting")
{
    run("newScene");
    run("spaceLocator", {{"name", "a"}});
    run("setAttr", {{"args", {"a.translate", json::array({10, 0, 0})}}});
    run("spaceLocator", {{"name", "b"}});
    run("setAttr", {{"args", {"b.translate", json::array({0, 5, 0})}}});
    run("parent", {{"args", {"b", "a"}}});

    glm::dvec3 world = vec(run("xform", {{"args", {"b"}}, {"q", true}, {"ws", true}, {"t", true}}));
    CHECK(world.x == doctest::Approx(0));
    CHECK(world.y == doctest::Approx(5));
    glm::dvec3 local = vec(run("getAttr", {{"args", {"b.translate"}}}));
    CHECK(local.x == doctest::Approx(-10));

    run("setAttr", {{"args", {"a.tx", 20}}});
    world = vec(run("xform", {{"args", {"b"}}, {"q", true}, {"ws", true}, {"t", true}}));
    CHECK(world.x == doctest::Approx(10));

    run("undo");
    world = vec(run("xform", {{"args", {"b"}}, {"q", true}, {"ws", true}, {"t", true}}));
    CHECK(world.x == doctest::Approx(0));
}

TEST_CASE("connections drive values and refuse cycles")
{
    run("newScene");
    run("spaceLocator", {{"name", "src"}});
    run("spaceLocator", {{"name", "dst"}});
    run("connectAttr", {{"args", {"src.translate", "dst.rotate"}}});
    run("setAttr", {{"args", {"src.translate", json::array({1, 2, 3})}}});
    CHECK(vec(run("getAttr", {{"args", {"dst.rotate"}}})) == glm::dvec3(1, 2, 3));
    CHECK_THROWS_AS(run("setAttr", {{"args", {"dst.rotate", json::array({0, 0, 0})}}}), CommandError);
    CHECK_THROWS_AS(run("connectAttr", {{"args", {"dst.rotate", "src.scale"}}}), CommandError);
}

TEST_CASE("undoing a connection puts back the value the plug had before it")
{
    run("newScene");
    run("spaceLocator", {{"name", "src"}});
    run("spaceLocator", {{"name", "dst"}});
    run("setAttr", {{"args", {"dst.translate", json::array({4, 5, 6})}}});
    run("setAttr", {{"args", {"src.translate", json::array({1, 2, 3})}}});
    run("connectAttr", {{"args", {"src.translate", "dst.translate"}}});
    CHECK(vec(run("getAttr", {{"args", {"dst.translate"}}})) == glm::dvec3(1, 2, 3));
    run("undo");
    CHECK(vec(run("getAttr", {{"args", {"dst.translate"}}})) == glm::dvec3(4, 5, 6));
    run("connectAttr", {{"args", {"src.ty", "dst.tx"}}});
    CHECK(vec(run("getAttr", {{"args", {"dst.translate"}}})) == glm::dvec3(2, 5, 6));
    run("undo");
    CHECK(vec(run("getAttr", {{"args", {"dst.translate"}}})) == glm::dvec3(4, 5, 6));
}

TEST_CASE("pivots don't move the object")
{
    run("newScene");
    run("spaceLocator", {{"name", "p"}});
    run("setAttr", {{"args", {"p.rotatePivot", json::array({5, 0, 0})}}});
    run("setAttr", {{"args", {"p.rotate", json::array({0, 0, 90})}}});
    glm::dvec3 world = vec(run("xform", {{"args", {"p"}}, {"q", true}, {"ws", true}, {"t", true}}));
    // Rotating 90 about Z around (5,0,0) swings the origin to (5,-5,0).
    CHECK(world.x == doctest::Approx(5));
    CHECK(world.y == doctest::Approx(-5));
}

TEST_CASE("euler round trip for every rotate order")
{
    glm::dvec3 r(30, -40, 70);
    for (int o = 0; o < 6; ++o)
    {
        glm::dmat4 m = eulerToMatrix(r, RotateOrder(o));
        glm::dvec3 back = matrixToEuler(glm::dmat3(m), RotateOrder(o));
        glm::dmat4 m2 = eulerToMatrix(back, RotateOrder(o));
        for (int c = 0; c < 3; ++c)
            for (int k = 0; k < 3; ++k)
                CHECK(m[c][k] == doctest::Approx(m2[c][k]).epsilon(1e-9));
    }
}

TEST_CASE("parallel evaluation of a wide graph")
{
    run("newScene");
    run("spaceLocator", {{"name", "root"}});
    for (int i = 0; i < 200; ++i)
    {
        std::string n = run("createNode", {{"args", {"locator"}}, {"name", "c"}, {"parent", "root"}, {"skipSelect", true}}).get<std::string>();
        run("setAttr", {{"args", {n + ".tx", i}}});
    }
    run("setAttr", {{"args", {"root.ty", 3}}});
    glm::dvec3 w = vec(run("xform", {{"args", {"c199"}}, {"q", true}, {"ws", true}, {"t", true}}));
    CHECK(w.x == doctest::Approx(199));
    CHECK(w.y == doctest::Approx(3));
}

TEST_CASE("naming conventions")
{
    using namespace lila::scene;
    CHECK(applyCase("myCube", CaseStyle::Pascal) == "MyCube");
    CHECK(applyCase("my_cube", CaseStyle::Pascal) == "MyCube");
    CHECK(applyCase("MyCube", CaseStyle::Snake) == "my_cube");
    CHECK(applyCase("LeftArmIK", CaseStyle::Snake) == "left_arm_ik");
    CHECK(applyCase("HTTPServer", CaseStyle::Pascal) == "HttpServer");
    const Underscores anywhere{.allow = true, .suffixesOnly = false};
    CHECK(applyCase("upper arm_l", CaseStyle::Pascal, anywhere) == "UpperArm_L");
    CHECK(applyCase("tail_", CaseStyle::Pascal, anywhere) == "Tail_");
    CHECK(applyCase("left_arm_2", CaseStyle::Camel, anywhere) == "left_Arm_2");
    const Underscores sides{.allow = true, .suffixes = {"L", "B", "LF"}};
    CHECK(applyCase("dog_leg_b", CaseStyle::Pascal, sides) == "DogLeg_B");
    CHECK(applyCase("wheel_lf", CaseStyle::Pascal, sides) == "Wheel_LF");
    CHECK(applyCase("arm_l_2", CaseStyle::Pascal, sides) == "Arm_L_2");
    CHECK(applyCase("tail_", CaseStyle::Pascal, sides) == "Tail_");
    CHECK(applyCase("upper_arm", CaseStyle::Camel, sides) == "upperArm");
    CHECK(applyCase("l_arm", CaseStyle::Pascal, sides) == "LArm");

    const NodeType& loc = *types::find("locator");
    NamingPolicy kept = NamingPolicy::fromJson({{"enabled", true}});
    CHECK(kept.conform(loc, "ns:my_cube_12") == "ns:MyCube_12");
    CHECK(kept.conform(loc, "dog_leg_b") == "DogLeg_B");
    CHECK(kept.conform(loc, "UCX_arm_l") == "UCX_Arm_L");
    CHECK(kept.conform(loc, "skm_hero_body") == "SKM_HeroBody");
    NamingPolicy sided = NamingPolicy::fromJson({{"enabled", true}, {"prefixes", "SM, L"}, {"rules", {{"*", "camelCase"}}}});
    CHECK(sided.conform(loc, "sm_l_upper_arm_b") == "SM_L_UpperArm_B");
    CHECK(sided.conform(loc, "upper_arm") == "upperArm");
    NamingPolicy p = NamingPolicy::fromJson({{"enabled", true}, {"allowUnderscores", {{"joint", false}, {"*", false}}}});
    CHECK(p.conform(loc, "myCube") == "MyCube");
    CHECK(p.conform(loc, "ns:my_cube12") == "ns:MyCube12");
    CHECK(p.conform(loc, "UCX_my_cube") == "UCX_MyCube");

    Graph& g = Graph::current();
    run("newScene");
    g.setNamingPolicy(p);
    CHECK(run("spaceLocator", {{"name", "my_target"}}) == "MyTarget");
    CHECK(run("spaceLocator") == "Locator");
    g.setNamingPolicy({});
    CHECK(run("spaceLocator") == "Locator1");
    g.setNamingPolicy(p);
    p.mode = NamingPolicy::Mode::Block;
    g.setNamingPolicy(p);
    CHECK_THROWS_AS(run("spaceLocator", {{"name", "bad_name"}}), CommandError);
    g.setNamingPolicy({});
}

#include "io/SceneFile.h"

TEST_CASE("scene save/load round trip")
{
    run("newScene");
    run("spaceLocator", {{"name", "A"}});
    run("setAttr", {{"args", {"A.translate", json::array({1, 2, 3})}}});
    run("spaceLocator", {{"name", "B"}});
    run("parent", {{"args", {"B", "A"}}, {"relative", true}});
    run("setAttr", {{"args", {"B.rotateOrder", "zxy"}}});
    run("connectAttr", {{"args", {"A.translate", "B.scale"}}});
    json doc = io::serialize();

    run("newScene");
    io::deserialize(doc);
    CHECK(run("listRelatives", {{"args", {"A"}}}) == json::array({"B"}));
    CHECK(vec(run("getAttr", {{"args", {"B.scale"}}})) == glm::dvec3(1, 2, 3));
    CHECK(run("getAttr", {{"args", {"B.rotateOrder"}}}) == 2);
    CHECK(ops::execute("undoInfo", json::object())["undo"].get<std::string>().empty());
    CHECK(io::serialize() == doc);
}

TEST_CASE("parent chain follows selection order")
{
    run("newScene");
    for (const char* n : {"L1", "L2", "L3"})
        run("spaceLocator", {{"name", n}});
    run("select", {{"args", {"L3", "L2", "L1"}}});
    run("parent", {{"chain", true}});
    CHECK(run("listRelatives", {{"args", {"L3"}}, {"parent", true}}) == json::array({"L2"}));
    CHECK(run("listRelatives", {{"args", {"L2"}}, {"parent", true}}) == json::array({"L1"}));
    run("undo");
    CHECK(run("listRelatives", {{"args", {"L3"}}, {"parent", true}}) == json::array());
}

TEST_CASE("move rotate scale commands")
{
    run("newScene");
    run("spaceLocator", {{"name", "A"}});
    run("spaceLocator", {{"name", "B"}});
    run("parent", {{"args", {"B", "A"}}});
    run("move", {{"args", {"A", "B"}}, {"by", {0, 5, 0}}});
    // B is under A, so it only moves with its parent.
    CHECK(run("xform", {{"args", {"B"}}, {"query", true}, {"worldSpace", true}, {"translation", true}})[1].get<double>() == doctest::Approx(5));
    run("rotate", {{"args", {"A"}}, {"by", {0, 90, 0}}});
    CHECK(run("getAttr", {{"args", {"A.ry"}}}).get<double>() == doctest::Approx(90));
    run("scale", {{"args", {"A"}}, {"by", {2, 2, 2}}});
    CHECK(run("getAttr", {{"args", {"A.sx"}}}).get<double>() == doctest::Approx(2));
    run("move", {{"args", {"A"}}, {"to", {10, 0, 0}}});
    auto p = run("pivot", {{"args", {"A"}}});
    CHECK(p[0].get<double>() == doctest::Approx(10));
}

TEST_CASE("pivot edits keep the object in place")
{
    using namespace lila::scene;
    run("newScene");
    run("spaceLocator", {{"name", "P"}});
    run("xform", {{"args", {"P"}}, {"translation", {3, 4, 5}}, {"rotation", {20, 30, 40}}, {"scale", {1, 2, 3}}});
    Graph& g = Graph::current();
    g.evaluate();
    glm::dmat4 before = g.find("P")->as<glm::dmat4>(Transform::kWorldMatrix);
    run("setPivot", {{"args", {"P"}}, {"position", {10, -2, 7}}});
    g.evaluate();
    glm::dmat4 after = g.find("P")->as<glm::dmat4>(Transform::kWorldMatrix);
    for (int c = 0; c < 4; ++c)
        for (int r = 0; r < 4; ++r)
            CHECK(after[c][r] == doctest::Approx(before[c][r]).epsilon(1e-9));
    auto piv = run("pivot", {{"args", {"P"}}});
    CHECK(piv[0].get<double>() == doctest::Approx(10));
    CHECK(piv[1].get<double>() == doctest::Approx(-2));
    CHECK(piv[2].get<double>() == doctest::Approx(7));

    // Rotating now turns about the new pivot, which stays put.
    run("rotate", {{"args", {"P"}}, {"by", {0, 45, 0}}});
    piv = run("pivot", {{"args", {"P"}}});
    CHECK(piv[0].get<double>() == doctest::Approx(10));
    CHECK(piv[2].get<double>() == doctest::Approx(7));

    run("resetPivot", {{"args", {"P"}}});
    g.evaluate();
    glm::dmat4 reset = g.find("P")->as<glm::dmat4>(Transform::kWorldMatrix);
    run("undo");
    g.evaluate();
    glm::dmat4 undone = g.find("P")->as<glm::dmat4>(Transform::kWorldMatrix);
    for (int c = 0; c < 4; ++c)
        for (int r = 0; r < 4; ++r)
            CHECK(reset[c][r] == doctest::Approx(undone[c][r]).epsilon(1e-9));
}

TEST_CASE("setPivot on a joint moves the joint and leaves its children in place")
{
    using namespace lila::scene;
    run("newScene");
    std::string root = run("joint", {{"name", "root"}, {"position", {0, 10, 0}}, {"parent", ""}}).get<std::string>();
    std::string mid = run("joint", {{"name", "mid"}, {"position", {0, 30, 0}}, {"parent", root}}).get<std::string>();
    std::string tip = run("joint", {{"name", "tip"}, {"position", {0, 50, 0}}, {"parent", mid}}).get<std::string>();
    run("setPivot", {{"args", {mid}}, {"position", {5, 25, 0}}});
    Graph& g = Graph::current();
    g.evaluate();
    auto at = [&](const std::string& name) { return xformops::worldPivot(g, g.find(name)->id()); };
    CHECK(glm::length(at(mid) - glm::dvec3(5, 25, 0)) == doctest::Approx(0.0));
    CHECK(glm::length(at(tip) - glm::dvec3(0, 50, 0)) == doctest::Approx(0.0));
    CHECK(glm::length(at(root) - glm::dvec3(0, 10, 0)) == doctest::Approx(0.0));
    CHECK(glm::length(g.find(mid)->as<glm::dvec3>(Transform::kRotatePivot)) == doctest::Approx(0.0));

    // orient= turns the joint through jointOrient; the tip stays put and rotate is untouched.
    run("setPivot", {{"args", {mid}}, {"orient", {0, 0, 30}}});
    g.evaluate();
    CHECK(glm::length(at(tip) - glm::dvec3(0, 50, 0)) == doctest::Approx(0.0).epsilon(1e-9));
    CHECK(g.find(mid)->as<glm::dvec3>(Joint::kJointOrient).z == doctest::Approx(30.0));
    CHECK(glm::length(g.find(mid)->as<glm::dvec3>(Transform::kRotate)) == doctest::Approx(0.0));
    CHECK(glm::length(g.find(mid)->as<glm::dvec3>(Transform::kPivotOrient)) == doctest::Approx(0.0));

    // A manipulator turn: the joint's world rotation becomes the turn applied to the old one.
    const NodeId midId = g.find(mid)->id();
    const glm::dmat3 before(g.find(mid)->as<glm::dmat4>(Transform::kWorldMatrix));
    const glm::dmat3 turn(eulerToMatrix(glm::dvec3(20, 0, 0), RotateOrder::XYZ));
    rig::setJointOrient(g, midId, rig::turnedJointOrient(g, midId, turn));
    g.evaluate();
    const glm::dmat3 after(g.find(mid)->as<glm::dmat4>(Transform::kWorldMatrix));
    const glm::dmat3 expected = turn * before;
    for (int c = 0; c < 3; ++c)
        CHECK(glm::length(after[c] - expected[c]) == doctest::Approx(0.0).epsilon(1e-9));
    CHECK(glm::length(at(tip) - glm::dvec3(0, 50, 0)) == doctest::Approx(0.0).epsilon(1e-9));
}

TEST_CASE("matchPivot copies position, orientation or both")
{
    using namespace lila::scene;
    run("newScene");
    run("spaceLocator", {{"name", "A"}});
    run("spaceLocator", {{"name", "S"}});
    run("xform", {{"args", {"A"}}, {"translation", {1, 2, 3}}, {"rotation", {10, 0, 0}}, {"scale", {1, 2, 1}}});
    run("xform", {{"args", {"S"}}, {"translation", {-4, 0, 6}}, {"rotation", {0, 50, 20}}});
    run("setPivot", {{"args", {"S"}}, {"position", {9, 9, 9}}, {"orient", {0, 0, 30}}});
    Graph& g = Graph::current();
    g.evaluate();
    const glm::dmat4 before = g.find("A")->as<glm::dmat4>(Transform::kWorldMatrix);
    const glm::dvec3 aPivot = xformops::worldPivot(g, g.find("A")->id());
    const glm::dmat3 aAxes = xformops::pivotFrame(g, g.find("A")->id());
    const glm::dmat3 sAxes = xformops::pivotFrame(g, g.find("S")->id());
    auto near = [](const glm::dmat3& x, const glm::dmat3& y) {
        for (int c = 0; c < 3; ++c)
            for (int r = 0; r < 3; ++r)
                if (std::abs(x[c][r] - y[c][r]) > 1e-6)
                    return false;
        return true;
    };

    run("matchPivot", {{"args", {"S", "A"}}, {"orient", false}});
    g.evaluate();
    CHECK(glm::length(xformops::worldPivot(g, g.find("A")->id()) - glm::dvec3(9)) < 1e-6);
    CHECK(near(xformops::pivotFrame(g, g.find("A")->id()), aAxes));

    run("undo");
    run("matchPivot", {{"args", {"S", "A"}}, {"position", false}});
    g.evaluate();
    CHECK(glm::length(xformops::worldPivot(g, g.find("A")->id()) - aPivot) < 1e-6);
    CHECK(near(xformops::pivotFrame(g, g.find("A")->id()), sAxes));

    run("undo");
    run("matchPivot", {{"args", {"S", "A"}}});
    g.evaluate();
    CHECK(glm::length(xformops::worldPivot(g, g.find("A")->id()) - glm::dvec3(9)) < 1e-6);
    CHECK(near(xformops::pivotFrame(g, g.find("A")->id()), sAxes));
    const glm::dmat4 after = g.find("A")->as<glm::dmat4>(Transform::kWorldMatrix);
    for (int c = 0; c < 4; ++c)
        for (int r = 0; r < 4; ++r)
            CHECK(after[c][r] == doctest::Approx(before[c][r]).epsilon(1e-9));
}

TEST_CASE("zeroPivotRotation lines pivot axes up with the world or the object")
{
    using namespace lila::scene;
    run("newScene");
    run("spaceLocator", {{"name", "A"}});
    run("xform", {{"args", {"A"}}, {"translation", {1, 2, 3}}, {"rotation", {10, 40, 0}}});
    run("setPivot", {{"args", {"A"}}, {"position", {5, 5, 5}}, {"orient", {0, 0, 30}}});
    Graph& g = Graph::current();
    const NodeId id = g.find("A")->id();
    const glm::dmat4 before = g.find("A")->as<glm::dmat4>(Transform::kWorldMatrix);

    run("zeroPivotRotation", {{"args", {"A"}}});
    const glm::dmat3 world = xformops::pivotFrame(g, id);
    for (int c = 0; c < 3; ++c)
        CHECK(glm::length(world[c] - glm::dmat3(1.0)[c]) < 1e-6);
    CHECK(glm::length(xformops::worldPivot(g, id) - glm::dvec3(5)) < 1e-6);
    g.evaluate();
    const glm::dmat4 after = g.find("A")->as<glm::dmat4>(Transform::kWorldMatrix);
    for (int c = 0; c < 4; ++c)
        CHECK(glm::length(after[c] - before[c]) < 1e-9);

    run("zeroPivotRotation", {{"args", {"A"}}, {"space", "object"}});
    CHECK(run("getAttr", {{"args", {"A.pivotOrient"}}})[2].get<double>() == doctest::Approx(0));
}

TEST_CASE("zeroPivot zeroes the chosen world axes and keeps the object")
{
    using namespace lila::scene;
    run("newScene");
    run("spaceLocator", {{"name", "A"}});
    run("xform", {{"args", {"A"}}, {"translation", {1, 2, 3}}, {"rotation", {10, 40, 0}}});
    run("setPivot", {{"args", {"A"}}, {"position", {4, 5, 6}}});
    Graph& g = Graph::current();
    const NodeId id = g.find("A")->id();
    g.evaluate();
    const glm::dmat4 before = g.find("A")->as<glm::dmat4>(Transform::kWorldMatrix);

    run("zeroPivot", {{"args", {"A"}}, {"axes", "y"}});
    CHECK(glm::length(xformops::worldPivot(g, id) - glm::dvec3(4, 0, 6)) < 1e-6);
    run("zeroPivot", {{"args", {"A"}}, {"axes", "xz"}});
    CHECK(glm::length(xformops::worldPivot(g, id)) < 1e-6);
    g.evaluate();
    const glm::dmat4 after = g.find("A")->as<glm::dmat4>(Transform::kWorldMatrix);
    for (int c = 0; c < 4; ++c)
        CHECK(glm::length(after[c] - before[c]) < 1e-9);
    CHECK_THROWS(run("zeroPivot", {{"args", {"A"}}, {"axes", "w"}}));
}

TEST_CASE("closest euler avoids flips")
{
    using namespace lila::scene;
    glm::dvec3 e(170, 10, -175);
    glm::dmat3 m(eulerToMatrix(e, RotateOrder::XYZ));
    glm::dvec3 out = xformops::closestEuler(m, RotateOrder::XYZ, glm::dvec3(530, 10, 185));
    CHECK(out.x == doctest::Approx(530));
    CHECK(out.z == doctest::Approx(185));
    for (int o = 0; o < 6; ++o)
    {
        glm::dvec3 hint(100, 120, -200);
        glm::dmat3 r(eulerToMatrix(glm::dvec3(25, 160, -70), RotateOrder(o)));
        glm::dvec3 got = xformops::closestEuler(r, RotateOrder(o), hint);
        glm::dmat3 back(eulerToMatrix(got, RotateOrder(o)));
        for (int c = 0; c < 3; ++c)
            CHECK(glm::length(back[c] - r[c]) < 1e-9);
    }
}

TEST_CASE("evaluateUpstream computes only what the given nodes depend on")
{
    run("newScene");
    Graph& g = Graph::current();
    run("spaceLocator", {{"name", "parent"}});
    run("spaceLocator", {{"name", "child"}});
    run("parent", {{"args", {"child", "parent"}}});
    run("spaceLocator", {{"name", "other"}});
    g.evaluate();

    run("setAttr", {{"args", {"parent.translate", json::array({0, 5, 0})}}});
    run("setAttr", {{"args", {"other.translate", json::array({0, 7, 0})}}});
    const uint64_t before = g.find("other")->evaluatedPass();
    g.evaluateUpstream({g.find("child")->id()});
    CHECK(glm::dvec3(g.find("child")->as<glm::dmat4>(Transform::kWorldMatrix)[3]).y == doctest::Approx(5.0));
    CHECK(g.find("other")->evaluatedPass() == before);

    // The rest computes on the next full evaluation.
    g.evaluate();
    CHECK(g.find("other")->evaluatedPass() > before);
    CHECK(glm::dvec3(g.find("other")->as<glm::dmat4>(Transform::kWorldMatrix)[3]).y == doctest::Approx(7.0));
}

TEST_CASE("values stay inside an attr's hard limits")
{
    run("newScene");
    run("spaceLocator", {{"name", "loc"}});
    run("addAttr", {{"args", {"loc"}}, {"longName", "count"}, {"attributeType", "long"}, {"minValue", 1}, {"maxValue", 32}});
    run("addAttr", {{"args", {"loc"}}, {"longName", "amount"}, {"attributeType", "double"}, {"minValue", 0.0}});
    run("setAttr", {{"args", {"loc.count", -4}}});
    CHECK(run("getAttr", {{"args", {"loc.count"}}}).get<int64_t>() == 1);
    run("setAttr", {{"args", {"loc.count", 99}}});
    CHECK(run("getAttr", {{"args", {"loc.count"}}}).get<int64_t>() == 32);
    run("setAttr", {{"args", {"loc.amount", -0.5}}});
    CHECK(run("getAttr", {{"args", {"loc.amount"}}}).get<double>() == 0.0);
    run("setAttr", {{"args", {"loc.amount", 7.5}}});
    CHECK(run("getAttr", {{"args", {"loc.amount"}}}).get<double>() == 7.5);
}

TEST_CASE("command flags are clamped to their bounds")
{
    ops::registerCommand({
        .name = "boundsTestEcho",
        .doc = "Returns its flags.",
        .flags = {{"count", "c", ArgType::Int, 1, "Loops.", {}, {}, 1.0, 32.0},
                  {"amount", "a", ArgType::Float, 0.5, "Fraction.", {}, {}, 0.0, 1.0}},
        .undoable = false,
        .run = [](const Args& a) -> json { return a.flags; },
    });
    json r = run("boundsTestEcho", {{"count", -3}, {"amount", 4.0}});
    CHECK(r["count"].get<int64_t>() == 1);
    CHECK(r["amount"].get<double>() == 1.0);
    r = run("boundsTestEcho", {{"count", 40}, {"amount", 0.25}});
    CHECK(r["count"].get<int64_t>() == 32);
    CHECK(r["amount"].get<double>() == 0.25);
    ops::unregisterCommand("boundsTestEcho");
}

TEST_CASE("connected values are held inside the destination's limits")
{
    run("newScene");
    run("spaceLocator", {{"name", "src"}});
    run("spaceLocator", {{"name", "dst"}});
    run("addAttr", {{"args", {"dst"}}, {"longName", "count"}, {"attributeType", "long"}, {"minValue", 1}, {"maxValue", 32}});
    run("connectAttr", {{"args", {"src.tx", "dst.count"}}});
    run("setAttr", {{"args", {"src.tx", -5.0}}});
    CHECK(run("getAttr", {{"args", {"dst.count"}}}).get<int64_t>() == 1);
    run("setAttr", {{"args", {"src.tx", 50.0}}});
    CHECK(run("getAttr", {{"args", {"dst.count"}}}).get<int64_t>() == 32);
    run("setAttr", {{"args", {"src.tx", 7.0}}});
    CHECK(run("getAttr", {{"args", {"dst.count"}}}).get<int64_t>() == 7);
}

namespace
{
/** Outputs the time it computed at and counts its computes. */
class TimeCounterNode final : public Node
{
public:
    static inline std::atomic<int> computes{0};
    void compute(const EvalContext& ctx) override
    {
        ++computes;
        setOutput(0, ctx.time);
    }
};
} // namespace

TEST_CASE("cached playback restores kept times and drops them on an edit")
{
    if (!types::find("testTimeCounter"))
        types::registerType({.name = "testTimeCounter",
                             .timeDependent = true,
                             .ownAttrs = {AttrDef{.name = "out", .shortName = "o", .flags = AttrOutput}},
                             .create = [] { return std::make_unique<TimeCounterNode>(); }});
    run("newScene");
    Graph& g = Graph::current();
    run("createNode", {{"args", {"testTimeCounter"}}, {"name", "counter"}});
    run("spaceLocator", {{"name", "loc"}});
    run("connectAttr", {{"args", {"counter.out", "loc.tx"}}});
    const Node& loc = *g.find("loc");
    auto x = [&] { return glm::dvec3(loc.as<glm::dmat4>(Transform::kWorldMatrix)[3]).x; };

    run("cachedPlayback", {{"state", true}});
    for (double t : {1.0, 2.0})
    {
        g.setTime(t);
        g.evaluate();
    }
    CHECK(g.playbackCacheTimes() == std::vector<double>{1.0, 2.0});

    const int before = TimeCounterNode::computes;
    g.setTime(1.0);
    g.evaluate();
    CHECK(x() == doctest::Approx(1.0));
    g.setTime(2.0);
    g.evaluate();
    CHECK(x() == doctest::Approx(2.0));
    CHECK(TimeCounterNode::computes == before);

    run("setAttr", {{"args", {"loc.ty", 3}}});
    CHECK(g.playbackCacheTimes().empty());
    g.setTime(1.0);
    g.evaluate();
    CHECK(TimeCounterNode::computes == before + 1);
    CHECK(x() == doctest::Approx(1.0));
    run("cachedPlayback", {{"state", false}});
}

namespace
{
/** Outputs -1 until told its data has arrived, as a simulation's output does before its state reaches the frame. */
class LateDataNode final : public Node
{
public:
    static inline std::atomic<bool> ready{false};
    void compute(const EvalContext& ctx) override { setOutput(0, ready ? ctx.time : -1.0); }
    PlaybackExtra playbackExtra(const EvalContext&) const override { return {.keep = ready.load()}; }
};
} // namespace

TEST_CASE("cached playback never keeps a placeholder result")
{
    if (!types::find("testLateData"))
        types::registerType({.name = "testLateData",
                             .timeDependent = true,
                             .ownAttrs = {AttrDef{.name = "out", .shortName = "o", .flags = AttrOutput}},
                             .create = [] { return std::make_unique<LateDataNode>(); }});
    run("newScene");
    Graph& g = Graph::current();
    run("createNode", {{"args", {"testLateData"}}, {"name", "late"}});
    run("spaceLocator", {{"name", "follower"}});
    run("connectAttr", {{"args", {"late.out", "follower.tx"}}});
    Node& late = *g.find("late");
    const Node& follower = *g.find("follower");
    auto x = [&] { return glm::dvec3(follower.as<glm::dmat4>(Transform::kWorldMatrix)[3]).x; };
    run("cachedPlayback", {{"state", true}});

    LateDataNode::ready = false;
    g.setTime(3.0);
    g.evaluate();
    CHECK(late.as<double>(0) == doctest::Approx(-1.0));
    CHECK(g.playbackCacheTimes().empty());

    LateDataNode::ready = true;
    g.touch(late.id());
    g.evaluate();
    CHECK(late.as<double>(0) == doctest::Approx(3.0));
    CHECK(x() == doctest::Approx(3.0));
    CHECK(g.playbackCacheTimes() == std::vector<double>{3.0});

    g.setTime(4.0);
    g.evaluate();
    g.setTime(3.0);
    g.evaluate();
    CHECK(late.as<double>(0) == doctest::Approx(3.0));
    CHECK(x() == doctest::Approx(3.0));
    run("cachedPlayback", {{"state", false}});
}
