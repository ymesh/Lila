#include <doctest/doctest.h>

#include "ops/Command.h"
#include "core/PrefStore.h"
#include "core/Workspace.h"
#include "ui/Layouts.h"

#include <algorithm>
#include <filesystem>
#include <fstream>

using namespace lila;
namespace layouts = lila::ui::layouts;
namespace fs = std::filesystem;

namespace
{
const layouts::Entry* entry(const std::vector<layouts::Entry>& all, const std::string& name)
{
    for (const layouts::Entry& e : all)
        if (e.name == name)
            return &e;
    return nullptr;
}

/** The layout's origin, or nothing when it isn't listed, so a missing layout fails a check instead of crashing. */
std::optional<layouts::Origin> originOf(const std::string& name)
{
    if (std::optional<layouts::Entry> e = layouts::find(name))
        return e->origin;
    return std::nullopt;
}

/** The saved layout's "state", or empty when there is no such layout. */
std::string stateOf(const std::string& name)
{
    std::optional<json> l = layouts::read(name);
    return l ? l->value("state", "") : std::string();
}

bool isPreset(const std::string& name)
{
    std::optional<json> l = layouts::read(name);
    return l && l->contains("preset");
}

void ensureLayoutCommands()
{
    if (!ops::find("applyLayout"))
        layouts::registerCommands();
}
} // namespace

TEST_CASE("layout lists merge built-in, user and workspace")
{
    auto all = layouts::merge({"Modeling", "Rigging"}, {"zeta", "Alpha", "Rigging"}, {"beta", "zeta"});
    REQUIRE(all.size() == 5);
    CHECK(all[0].name == "Modeling");
    CHECK(all[0].origin == layouts::Origin::BuiltIn);
    CHECK(all[1].name == "Rigging");
    CHECK(all[1].origin == layouts::Origin::User);
    CHECK(all[1].shadowsBuiltIn);
    CHECK(all[2].name == "Alpha");
    CHECK(all[3].name == "beta");
    CHECK(all[3].origin == layouts::Origin::Workspace);
    CHECK(all[4].name == "zeta");
    CHECK(all[4].origin == layouts::Origin::Workspace);
    CHECK_FALSE(all[4].shadowsBuiltIn);
}

TEST_CASE("layout names")
{
    CHECK(layouts::invalidReason("UV Editing").empty());
    CHECK_FALSE(layouts::invalidReason("").empty());
    CHECK_FALSE(layouts::invalidReason("main").empty());
    CHECK_FALSE(layouts::invalidReason("a/b").empty());
    CHECK_FALSE(layouts::invalidReason("what?").empty());
    CHECK_FALSE(layouts::invalidReason(" padded").empty());
}

TEST_CASE("layouts save to user and workspace and read back")
{
    PrefStore& p = PrefStore::get();
    p.start();
    fs::path userDir = p.userDir() / "layouts";
    fs::remove_all(userDir);
    fs::path ws = fs::temp_directory_path() / "lila_tests_layout_ws";
    fs::remove_all(ws);
    fs::create_directories(ws / ".lila");

    // The window's own autosave is never listed.
    p.set("layouts/main", "/state", "xyz");
    p.flush();

    json mine = {{"version", 1}, {"state", "AAAA"}, {"viewport", {{"quad", true}, {"camera", "top"}}}};
    layouts::write("Mine", mine, layouts::Origin::User);
    layouts::write("Model", {{"version", 1}, {"state", "BBBB"}}, layouts::Origin::User);
    CHECK(fs::exists(userDir / "Mine.json"));
    CHECK_THROWS_AS(layouts::write("Team", mine, layouts::Origin::Workspace), CommandError);

    Workspace::get().open(ws);
    layouts::write("Team", {{"version", 1}, {"state", "CCCC"}}, layouts::Origin::Workspace);
    layouts::write("Mine", {{"version", 1}, {"state", "DDDD"}}, layouts::Origin::Workspace);
    CHECK(fs::exists(ws / ".lila" / "layouts" / "Team.json"));

    auto all = layouts::list();
    CHECK(entry(all, "main") == nullptr);
    REQUIRE(entry(all, "Model"));
    CHECK(entry(all, "Model")->origin == layouts::Origin::User);
    CHECK(entry(all, "Model")->shadowsBuiltIn);
    REQUIRE(entry(all, "Team"));
    CHECK(entry(all, "Team")->origin == layouts::Origin::Workspace);
    REQUIRE(entry(all, "Mine"));
    CHECK(entry(all, "Mine")->origin == layouts::Origin::Workspace);
    REQUIRE(entry(all, "UV"));
    CHECK(entry(all, "UV")->origin == layouts::Origin::BuiltIn);

    CHECK(stateOf("Mine") == "DDDD");
    CHECK(stateOf("Model") == "BBBB");
    CHECK(isPreset("UV"));
    CHECK_FALSE(layouts::read("Nope"));

    // Deleting the workspace copy uncovers the user one; deleting that uncovers the built-in.
    CHECK(layouts::remove("Mine"));
    CHECK_FALSE(fs::exists(ws / ".lila" / "layouts" / "Mine.json"));
    CHECK(originOf("Mine") == layouts::Origin::User);
    CHECK(stateOf("Mine") == "AAAA");
    CHECK(layouts::remove("Model"));
    CHECK(originOf("Model") == layouts::Origin::BuiltIn);
    CHECK(isPreset("Model"));
    CHECK_FALSE(layouts::remove("Model"));

    layouts::setCurrent("Team");
    layouts::rename("Team", "Crew");
    CHECK_FALSE(layouts::find("Team"));
    CHECK(originOf("Crew") == layouts::Origin::Workspace);
    CHECK(stateOf("Crew") == "CCCC");
    CHECK(layouts::current() == "Crew");
    CHECK_THROWS_AS(layouts::rename("Crew", "Mine"), CommandError);
    CHECK_THROWS_AS(layouts::rename("Rig", "Mine2"), CommandError);

    Workspace::get().close();
    CHECK_FALSE(layouts::find("Crew"));
    CHECK(layouts::find("Mine"));

    // A file dropped in by hand is listed and readable.
    std::ofstream(userDir / "Dropped.json") << R"({"version": 1, "state": "EEEE"})";
    CHECK(stateOf("Dropped") == "EEEE");

    layouts::setCurrent("");
    fs::remove_all(userDir);
    fs::remove_all(ws);
}

TEST_CASE("layout commands need the UI for apply and save")
{
    ensureLayoutCommands();
    layouts::setGuiHooks({});
    CHECK_THROWS_AS(ops::execute("applyLayout", {{"name", "Model"}}), CommandError);
    CHECK_THROWS_AS(ops::execute("saveLayout", {{"args", {"X"}}}), CommandError);
    json listed = ops::execute("listLayouts", json::object());
    REQUIRE(listed.is_array());
    CHECK(listed.size() >= layouts::builtInNames().size());
    CHECK(listed[0]["origin"] == "builtin");

    std::string applied;
    json appliedLayout;
    layouts::setGuiHooks({
        .apply = [&](const std::string& name, const json& l) { applied = name; appliedLayout = l; },
        .capture = [] { return json{{"version", 1}, {"state", "FFFF"}}; },
    });
    ops::execute("saveLayout", {{"args", {"Captured"}}});
    CHECK(stateOf("Captured") == "FFFF");
    CHECK(layouts::current() == "Captured");
    ops::execute("applyLayout", {{"args", {"UV"}}});
    CHECK(applied == "UV");
    CHECK(appliedLayout.contains("preset"));
    CHECK(layouts::current() == "UV");
    CHECK_THROWS_AS(ops::execute("applyLayout", {{"name", "Missing"}}), CommandError);
    ops::execute("renameLayout", {{"name", "Captured"}, {"to", "Kept"}});
    ops::execute("deleteLayout", {{"name", "Kept"}});
    CHECK_FALSE(layouts::find("Kept"));
    CHECK_THROWS_AS(ops::execute("deleteLayout", {{"name", "Model"}}), CommandError);

    layouts::setGuiHooks({});
    layouts::setCurrent("");
}

TEST_CASE("built-in layouts keep the outliner and inspector open")
{
    for (const std::string& name : layouts::builtInNames())
    {
        INFO(name);
        std::optional<json> l = layouts::builtIn(name);
        REQUIRE(l);
        json open = l->value(json::json_pointer("/preset/open"), json::array());
        for (const char* dock : {"outliner", "channelBox", "attributeEditor", "displayLayers"})
        {
            CAPTURE(dock);
            CHECK(std::find(open.begin(), open.end(), json(dock)) != open.end());
        }
    }
}

TEST_CASE("the Model layout shows the modeling toolkit tab")
{
    std::optional<json> l = layouts::builtIn("Model");
    REQUIRE(l);
    json open = l->value(json::json_pointer("/preset/open"), json::array());
    CHECK(std::find(open.begin(), open.end(), json("modelingToolkit")) != open.end());
    CHECK(std::find(open.begin(), open.end(), json("modifierStack")) != open.end());
    json raise = l->value(json::json_pointer("/preset/raise"), json::array());
    CHECK(std::find(raise.begin(), raise.end(), json("modelingToolkit")) != raise.end());
}
