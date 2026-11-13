#include <doctest/doctest.h>

#include "TestUtil.h"

#include "ops/Command.h"
#include "scene/Graph.h"
#include "ui/OutlinerLogic.h"

using namespace lila;
using json = nlohmann::json;
using namespace testutil;
namespace outliner = lila::ui::outliner;

TEST_CASE("node type icons fall back through the base types")
{
    using scene::types::find;
    using scene::types::iconFor;
    const std::string fallback = "fallback";
    CHECK(iconFor(*find("transform"), fallback) == "box-multiple");
    CHECK(iconFor(*find("mesh"), fallback) == "cube");
    CHECK(iconFor(*find("joint"), fallback) == "bone");
    CHECK(iconFor(*find("locator"), fallback) == "crosshair");
    CHECK(iconFor(*find("imagePlane"), fallback) == "photo");
    CHECK(iconFor(*find("distanceMeasure"), fallback) == "ruler-measure");
    // Constraint kinds set none of their own and inherit the constraint base's.
    CHECK(iconFor(*find("parentConstraint"), fallback) == "link");

    scene::NodeType bare;
    bare.name = "iconTestBare";
    CHECK(iconFor(bare, fallback) == "fallback");
    scene::NodeType derived;
    derived.name = "iconTestDerived";
    derived.base = find("mesh");
    CHECK(iconFor(derived, fallback) == "cube");
    derived.icon = "sphere";
    CHECK(iconFor(derived, fallback) == "sphere");
}

TEST_CASE("outliner row icons come from the node's type, whatever its name or parent")
{
    run("newScene");
    run("createNode", {{"args", {"mesh"}}, {"name", "body"}});
    run("createNode", {{"args", {"mesh"}}, {"name", "rock_high"}});
    scene::Graph& g = scene::Graph::current();
    g.createNode(*scene::types::find("mesh"), "body_high", g.find("body")->id(), false);
    run("spaceLocator", {{"name", "mark"}});
    CHECK(outliner::iconName(*g.find("body")) == "cube");
    CHECK(outliner::iconName(*g.find("body_high")) == "cube");
    CHECK(outliner::iconName(*g.find("rock_high")) == "cube");
    CHECK(outliner::iconName(*g.find("mark")) == "crosshair");
}

TEST_CASE("outliner context menu targets")
{
    std::vector<scene::NodeId> sel = {3, 5, 7};
    CHECK(outliner::menuTargets(5, sel) == sel);
    CHECK(outliner::menuTargets(9, sel) == std::vector<scene::NodeId>{9});
    CHECK(outliner::menuTargets(scene::kNoNode, sel) == sel);
    CHECK(outliner::menuTargets(4, {}) == std::vector<scene::NodeId>{4});
    CHECK(outliner::menuTargets(scene::kNoNode, {}).empty());
}

TEST_CASE("outliner hidden rows include hidden ancestors")
{
    run("newScene");
    run("group", {{"empty", true}, {"name", "grp"}});
    run("spaceLocator", {{"name", "loc"}});
    run("parent", {{"args", {"loc", "grp"}}});
    scene::Graph& g = scene::Graph::current();
    CHECK_FALSE(outliner::hiddenInHierarchy(g, idOf("loc")));
    run("toggleVisibility", {{"args", {"grp"}}});
    CHECK(outliner::hiddenInHierarchy(g, idOf("grp")));
    CHECK(outliner::hiddenInHierarchy(g, idOf("loc")));
    CHECK(outliner::visibleSelf(g, idOf("loc")));
    CHECK(outliner::ancestors(g, idOf("loc")) == std::vector<scene::NodeId>{idOf("grp")});
    run("undo");
    CHECK_FALSE(outliner::hiddenInHierarchy(g, idOf("loc")));
}

TEST_CASE("ungroup moves children up and deletes the group")
{
    run("newScene");
    run("group", {{"empty", true}, {"name", "outer"}});
    run("group", {{"empty", true}, {"name", "inner"}});
    run("spaceLocator", {{"name", "a"}});
    run("parent", {{"args", {"inner", "outer"}}});
    run("parent", {{"args", {"a", "inner"}}});
    run("setAttr", {{"args", {"inner.translate", {1.0, 2.0, 3.0}}}});
    scene::Graph& g = scene::Graph::current();

    json kids = run("ungroup", {{"args", {"inner"}}});
    CHECK(kids == json::array({"a"}));
    CHECK(g.find("inner") == nullptr);
    CHECK(g.parent(idOf("a")) == idOf("outer"));
    json world = run("xform", {{"args", {"a"}}, {"query", true}, {"worldSpace", true}, {"translation", true}});
    CHECK(world[0].get<double>() == doctest::Approx(1.0));
    CHECK(world[1].get<double>() == doctest::Approx(2.0));
    CHECK(world[2].get<double>() == doctest::Approx(3.0));
    CHECK_THROWS(run("ungroup", {{"args", {"a"}}}));

    run("undo");
    REQUIRE(g.find("inner") != nullptr);
    CHECK(g.parent(idOf("a")) == idOf("inner"));
}

namespace
{
std::vector<std::string> namesUnder(const char* parent)
{
    scene::Graph& g = scene::Graph::current();
    std::vector<std::string> out;
    for (scene::NodeId id : parent ? g.children(idOf(parent)) : g.roots())
        out.push_back(g.node(id)->name());
    return out;
}

using Names = std::vector<std::string>;
} // namespace

TEST_CASE("parent index reorders and inserts among siblings")
{
    run("newScene");
    for (const char* n : {"a", "b", "c", "d"})
        run("spaceLocator", {{"name", n}});
    run("group", {{"empty", true}, {"name", "grp"}});
    run("spaceLocator", {{"name", "x"}});
    run("spaceLocator", {{"name", "y"}});
    run("parent", {{"args", {"x", "y", "grp"}}});
    CHECK(namesUnder(nullptr) == Names{"a", "b", "c", "d", "grp"});
    CHECK(namesUnder("grp") == Names{"x", "y"});

    // Reorder at world, which plain world=True can't do.
    run("parent", {{"args", {"d"}}, {"world", true}, {"index", 0}});
    CHECK(namesUnder(nullptr) == Names{"d", "a", "b", "c", "grp"});
    run("parent", {{"args", {"d"}}, {"world", true}, {"index", 99}});
    CHECK(namesUnder(nullptr) == Names{"a", "b", "c", "grp", "d"});

    // Several objects keep their given order, and the index skips them.
    run("parent", {{"args", {"c", "a"}}, {"world", true}, {"index", 1}});
    CHECK(namesUnder(nullptr) == Names{"b", "c", "a", "grp", "d"});
    run("undo");
    CHECK(namesUnder(nullptr) == Names{"a", "b", "c", "grp", "d"});
    run("redo");
    CHECK(namesUnder(nullptr) == Names{"b", "c", "a", "grp", "d"});

    // Reorder under a parent, and reparent into a position, both ways.
    run("parent", {{"args", {"y", "grp"}}, {"index", 0}});
    CHECK(namesUnder("grp") == Names{"y", "x"});
    run("parent", {{"args", {"b", "grp"}}, {"index", 1}});
    CHECK(namesUnder("grp") == Names{"y", "b", "x"});
    CHECK(namesUnder(nullptr) == Names{"c", "a", "grp", "d"});
    run("parent", {{"args", {"b"}}, {"world", true}, {"index", 0}});
    CHECK(namesUnder(nullptr) == Names{"b", "c", "a", "grp", "d"});
    CHECK(namesUnder("grp") == Names{"y", "x"});
    run("undo");
    CHECK(namesUnder("grp") == Names{"y", "b", "x"});
    CHECK(namesUnder(nullptr) == Names{"c", "a", "grp", "d"});

    // Without an index nothing moves when the parent is unchanged.
    run("parent", {{"args", {"x", "grp"}}});
    CHECK(namesUnder("grp") == Names{"y", "b", "x"});
    CHECK_THROWS(run("parent", {{"args", {"x", "grp"}}, {"index", -1}}));

    CHECK(ops::toPython("parent", {{"args", {"b"}}, {"world", true}, {"index", 0}}).find("index=0") != std::string::npos);
}

TEST_CASE("outliner drop index and selection ancestors")
{
    std::vector<scene::NodeId> sib = {1, 2, 3, 4};
    CHECK(outliner::dropIndex(sib, {}, 3) == 2);
    CHECK(outliner::dropIndex(sib, {1}, 3) == 1);
    CHECK(outliner::dropIndex(sib, {3}, 3) == 2);
    CHECK(outliner::dropIndex(sib, {2}, scene::kNoNode) == 3);
    CHECK(outliner::dropIndex({}, {7}, scene::kNoNode) == 0);

    run("newScene");
    run("group", {{"empty", true}, {"name", "outer"}});
    run("group", {{"empty", true}, {"name", "inner"}});
    run("spaceLocator", {{"name", "leaf"}});
    run("spaceLocator", {{"name", "loose"}});
    run("parent", {{"args", {"inner", "outer"}}});
    run("parent", {{"args", {"leaf", "inner"}}});
    scene::Graph& g = scene::Graph::current();
    CHECK(outliner::selectionAncestors(g, {idOf("leaf")}) == std::set<scene::NodeId>{idOf("inner"), idOf("outer")});
    CHECK(outliner::selectionAncestors(g, {idOf("leaf"), idOf("inner")}) == std::set<scene::NodeId>{idOf("outer")});
    CHECK(outliner::selectionAncestors(g, {idOf("loose")}).empty());
}
