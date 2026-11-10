#include <doctest/doctest.h>

#include "TestUtil.h"

#include "ops/Command.h"
#include "scene/Graph.h"

#include <filesystem>

using namespace lila;
using json = nlohmann::json;
using namespace testutil;

namespace
{
std::string make(const char* type, const char* name, const char* parent = nullptr)
{
    json call = {{"args", {type}}, {"name", name}, {"skipSelect", true}};
    if (parent)
        call["parent"] = parent;
    return run("createNode", call).get<std::string>();
}

/** Two "Arm"s, under |Body|L and |Body|R. */
void twoArms()
{
    run("newScene");
    make("transform", "Body");
    make("transform", "L", "Body");
    make("transform", "R", "Body");
    make("transform", "Arm", "L");
    make("transform", "Arm", "R");
}

std::string errorOf(const char* name, json call)
{
    try
    {
        run(name, call);
    }
    catch (const CommandError& e)
    {
        return e.what();
    }
    return {};
}

struct UniqueNames
{
    explicit UniqueNames(bool on) { run("namingConvention", {{"uniqueNames", on}}); }
    ~UniqueNames() { run("namingConvention", {{"uniqueNames", false}}); }
};
} // namespace

TEST_CASE("names only need to be unique among siblings")
{
    run("newScene");
    make("transform", "L");
    make("transform", "R");
    CHECK(make("transform", "Arm", "L") == "Arm");
    CHECK(make("transform", "Arm", "R") == "R|Arm");
    CHECK(run("ls", {{"args", {"Arm"}}}) == json({"L|Arm", "R|Arm"}));
    // A real sibling clash still gets a suffix.
    CHECK(make("transform", "Arm", "L") == "Arm1");
    // Default names stay unique across the scene.
    CHECK(make("transform", "", "L") != make("transform", "", "R"));
}

TEST_CASE("an ambiguous name is an error that lists the paths")
{
    twoArms();
    std::string err = errorOf("getAttr", {{"args", {"Arm.tx"}}});
    CHECK(err.find("ambiguous") != std::string::npos);
    CHECK(err.find("|Body|L|Arm") != std::string::npos);
    CHECK(err.find("|Body|R|Arm") != std::string::npos);
    CHECK_THROWS_AS(run("select", {{"args", {"Arm"}}}), CommandError);
    CHECK(run("objExists", {{"args", {"Arm"}}}).get<bool>());
    CHECK_FALSE(run("objExists", {{"args", {"Foot"}}}).get<bool>());
    CHECK_FALSE(run("objExists", {{"args", {"R|L|Arm"}}}).get<bool>());
}

TEST_CASE("partial and full paths resolve")
{
    twoArms();
    run("setAttr", {{"args", {"L|Arm.tx", 3}}});
    CHECK(run("getAttr", {{"args", {"|Body|L|Arm.tx"}}}).get<double>() == doctest::Approx(3));
    CHECK(run("getAttr", {{"args", {"Body|R|Arm.tx"}}}).get<double>() == doctest::Approx(0));
    CHECK_FALSE(run("objExists", {{"args", {"|L|Arm"}}}).get<bool>()); // absolute must start at a root

    // The shortest unique suffix grows as needed.
    make("transform", "Other");
    make("transform", "L", "Other");
    make("transform", "Arm", "|Other|L");
    scene::Graph& g = scene::Graph::current();
    CHECK(g.path(g.find("|Body|L|Arm")->id()) == "Body|L|Arm");
    CHECK(g.path(g.find("|Body|R|Arm")->id()) == "R|Arm");
    CHECK(g.find("|Other|L|Arm")->fullPath() == "|Other|L|Arm");
    CHECK(g.find("Other|L|Arm")->name() == "Arm");
    CHECK_THROWS_AS(g.find("L|Arm"), CommandError);
    CHECK(g.findAll("L|Arm").size() == 2);
}

TEST_CASE("command results re-resolve to the same node")
{
    twoArms();
    scene::Graph& g = scene::Graph::current();
    for (const json& p : run("ls"))
        CHECK(g.findAll(p.get<std::string>()).size() == 1);
    run("select", {{"args", {"L|Arm", "R|Arm"}}});
    json sel = run("ls", {{"selection", true}});
    REQUIRE(sel.size() == 2);
    CHECK(g.find(sel[0].get<std::string>())->fullPath() == "|Body|L|Arm");
    CHECK(g.find(sel[1].get<std::string>())->fullPath() == "|Body|R|Arm");
    CHECK(g.plugName(g.plug("|Body|R|Arm.tx")) == "R|Arm.translateX");
    CHECK(run("rename", {{"args", {"R|Arm", "Arm"}}}) == "R|Arm");
    CHECK(run("rename", {{"args", {"R|Arm", "Wing"}}}) == "Wing");
}

TEST_CASE("duplicate, group and parent leave other nodes' names alone")
{
    twoArms();
    json dup = run("duplicate", {{"args", {"L"}}});
    REQUIRE(dup.size() == 1);
    scene::Graph& g = scene::Graph::current();
    scene::Node* copy = g.find(dup[0].get<std::string>());
    REQUIRE(copy);
    REQUIRE(g.children(copy->id()).size() == 1);
    CHECK(g.node(g.children(copy->id())[0])->name() == "Arm");

    std::string grp = run("group", {{"args", {"L|Arm"}}, {"name", "G"}}).get<std::string>();
    CHECK(grp == "G");
    CHECK(g.find("G|Arm"));

    run("parent", {{"args", {"R|Arm"}}, {"world", true}});
    CHECK(g.find("|Arm"));
    CHECK(g.find("|Arm")->name() == "Arm");

    // Parenting into a real clash renames, and undo puts the name back.
    run("parent", {{"args", {"|Arm", "G"}}});
    CHECK(g.find("|Body|L|G|Arm1"));
    run("undo");
    CHECK(g.find("|Arm"));
    CHECK_FALSE(g.find("|Body|L|G|Arm1"));
}

TEST_CASE("scene files keep duplicate short names apart")
{
    for (const char* file : {"paths.lila", "paths.lilaa"})
    {
        CAPTURE(file);
        twoArms();
        run("spaceLocator", {{"name", "Arm"}}); // a third, at the root
        run("setAttr", {{"args", {"L|Arm.tx", 4}}});
        run("connectAttr", {{"args", {"L|Arm.tx", "R|Arm.ty"}}});
        run("select", {{"args", {"R|Arm"}}});
        run("saveScene", {{"args", {tempPath("paths", file)}}});

        run("newScene");
        run("openScene", {{"args", {tempPath("paths", file)}}});
        scene::Graph& g = scene::Graph::current();
        CHECK(g.findAll("Arm").size() == 3);
        CHECK(run("getAttr", {{"args", {"|Body|L|Arm.tx"}}}).get<double>() == doctest::Approx(4));
        CHECK(run("getAttr", {{"args", {"|Body|R|Arm.ty"}}}).get<double>() == doctest::Approx(4));
        CHECK(g.find("|Arm")->type().name == "locator");
        REQUIRE(g.selection().size() == 1);
        CHECK(g.fullPath(g.selection()[0]) == "|Body|R|Arm");
    }
}

TEST_CASE("a reference with duplicate short names keeps its edits")
{
    twoArms();
    std::string asset = tempPath("paths", "arms.lilaa");
    run("saveScene", {{"args", {asset}}});

    run("newScene");
    run("createReference", {{"args", {asset}}, {"namespace", "hero"}});
    run("setAttr", {{"args", {"hero:R|hero:Arm.tx", 2}}});
    run("saveScene", {{"args", {tempPath("paths", "shot.lilaa")}}});
    run("newScene");
    run("openScene", {{"args", {tempPath("paths", "shot.lilaa")}}});
    CHECK(run("getAttr", {{"args", {"hero:R|hero:Arm.tx"}}}).get<double>() == doctest::Approx(2));
    CHECK(run("getAttr", {{"args", {"hero:L|hero:Arm.tx"}}}).get<double>() == doctest::Approx(0));

    run("importReference", {{"args", {"hero"}}});
    CHECK(scene::Graph::current().findAll("hero:Arm").size() == 2);
}

TEST_CASE("the unique names pref restores scene-wide renaming")
{
    UniqueNames on(true);
    run("newScene");
    make("transform", "L");
    make("transform", "R");
    CHECK(make("transform", "Arm", "L") == "Arm");
    CHECK(make("transform", "Arm", "R") == "Arm1");
    CHECK(run("rename", {{"args", {"Arm1", "Arm"}}}) == "Arm1");
}
