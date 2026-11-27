// worldBuild replace=True leaves exactly one copy of the world: replaced twice in a session, after a save and reopen,
// under naming rules that rename what it makes, and over a build that left no record (found by name and node type),
// while the user's own nodes stay.

#include "World.h"

#include "ops/Command.h"
#include "scene/Graph.h"
#include "scene/Modifiers.h"
#include "scene/Naming.h"
#include "scene/Node.h"
#include "OpenWorldTest.h"

#include <doctest/doctest.h>

#include <filesystem>
#include <map>
#include <set>

using json = nlohmann::json;
namespace fs = std::filesystem;

namespace
{
using owtest::data;
using owtest::failedSteps;
using owtest::run;
using lila::scene::Graph;

json townSpec()
{
    return json::parse(R"({
        "name": "Tiny",
        "culture": "mexican",
        "terrain": {"size": [200000, 200000], "resolution": 65, "seed": 3, "source": {"type": "noise", "amplitude": 1200, "featureSize": 90000}},
        "settlements": [{"name": "Pueblo", "create": {"profile": "usTown", "seed": 2,
                          "outline": [[-30000, -30000], [30000, -30000], [30000, 30000], [-30000, 30000]]},
                         "buildings": {"style": "colonial", "lod": "near"}}],
        "sites": [{"kind": "cemetery", "id": "c1", "at": [-60000, 60000], "params": {"cemeteryWidth": 9000, "cemeteryDepth": 7000}}],
        "zones": {"spec": {"layers": [{"id": "police", "type": "police", "zones": [{"id": "all", "name": "Comandancia Norte",
                    "polygons": [[[-100000, -100000], [100000, -100000], [100000, 100000], [-100000, 100000]]]}]}]}},
        "check": false
    })");
}

/** A remixed coast with an OpenStreetMap town on it: OSM line groups, the remix's own lines and the sea's water mesh. */
json remixSpec()
{
    json s = json::parse(R"({
        "name": "Harbour",
        "culture": "mexican",
        "remix": {"scale": {"horizontal": 0.3333, "vertical": 0.6667},
                  "seams": {"erosion": 0, "detail": 0.0, "roads": false, "rivers": false, "coast": false}},
        "terrain": {"water": {"sea": {"level": 0}}},
        "settlements": [{"name": "Puerto", "osm": "remix", "profile": "usTown", "buildings": {"cell": 6400}}],
        "check": false
    })");
    s["remix"]["sources"] = json::array({{{"id", "coast"}, {"dem", data("coast.asc")}, {"osm", data("coast.json")}}});
    return s;
}

json build(const json& spec, bool replace)
{
    const json r = run("worldBuild", {{"spec", spec}, {"replace", replace}});
    INFO(failedSteps(r));
    REQUIRE(r.value("failed", json()).is_null());
    return r;
}

/** Nodes by type, materials aside (builds share them by name): a second copy of anything shows as a larger count. */
std::map<std::string, int> countsByType()
{
    Graph& g = Graph::current();
    std::map<std::string, int> out;
    for (lila::scene::NodeId id : g.nodes())
        if (const lila::scene::Node* n = g.node(id); n && !n->type().isA("material"))
            ++out[n->type().name];
    return out;
}

/** Names of the top-level nodes: a copy of a world's node shows as a second, numbered name. */
std::set<std::string> topLevel()
{
    Graph& g = Graph::current();
    std::set<std::string> out;
    for (lila::scene::NodeId id : g.nodes())
        if (const lila::scene::Node* n = g.node(id); n && n->type().dag && g.parent(id) == lila::scene::kNoNode)
            out.insert(n->name());
    return out;
}

std::string notesOf(const json& r)
{
    std::string out;
    for (const json& n : r.value("notes", json::array()))
        out += n.get<std::string>() + "\n";
    return out;
}

void sameAs(const std::map<std::string, int>& counts, const std::set<std::string>& names, const char* when)
{
    const std::map<std::string, int> now = countsByType();
    for (const auto& [type, n] : now)
        CHECK_MESSAGE(n == (counts.count(type) ? counts.at(type) : 0), when << ": " << type << " " << n);
    for (const auto& [type, n] : counts)
        CHECK_MESSAGE(now.count(type), when << ": no " << type << " left");
    CHECK_MESSAGE(topLevel() == names, when << ": " << json(topLevel()).dump());
}

/** Jared's workspace rules: PascalCase, underscores only before side suffixes. */
lila::scene::NamingPolicy pascalNaming()
{
    return lila::scene::NamingPolicy::fromJson({{"enabled", true}, {"underscorePrefixes", false}, {"underscoreSuffixes", "L, R, C, M, F, B, T, LF, RF, LB, RB"}});
}
} // namespace

TEST_CASE("world replace: twice in one session, then after a save and reopen, leaves one copy")
{
    owtest::loadPackages(true, true);
    run("newScene");
    build(townSpec(), false);
    const std::map<std::string, int> counts = countsByType();
    const std::set<std::string> names = topLevel();
    REQUIRE(names.count("Pueblo_Buildings"));
    REQUIRE(names.count("Tiny_World"));

    const json r2 = build(townSpec(), true);
    sameAs(counts, names, "first replace");
    CHECK(notesOf(r2).find("replace: removed the earlier build of Tiny") != std::string::npos);
    // Everything the build made is in its record, so nothing is found by name.
    CHECK(notesOf(r2).find("matched by the world's names") == std::string::npos);
    build(townSpec(), true);
    sameAs(counts, names, "second replace");

    const fs::path file = fs::temp_directory_path() / "lila_world_replace" / "tiny.lila";
    fs::create_directories(file.parent_path());
    run("saveScene", {{"args", {file.generic_string()}}});
    run("openScene", {{"args", {file.generic_string()}}, {"force", true}});
    REQUIRE(topLevel() == names);
    const json r4 = build(townSpec(), true);
    sameAs(counts, names, "replace after reopen");
    CHECK(notesOf(r4).find("matched by the world's names") == std::string::npos);
}

TEST_CASE("world replace: naming rules that rename what the build makes still leave one copy")
{
    owtest::loadPackages(true, true);
    run("newScene");
    Graph& g = Graph::current();
    g.setNamingPolicy(pascalNaming());
    build(townSpec(), false);
    const std::map<std::string, int> counts = countsByType();
    const std::set<std::string> names = topLevel();
    INFO(json(names).dump());
    // One Buildings group, seeded and built into the same node; the world node found under its renamed name.
    CHECK(names.count("PuebloBuildings"));
    CHECK_FALSE(names.count("PuebloBuildings1"));
    CHECK(names.count("TinyWorld"));
    const json r2 = build(townSpec(), true);
    sameAs(counts, names, "replace under naming rules");
    CHECK(notesOf(r2).find("matched by the world's names") == std::string::npos);
    g.setNamingPolicy({});
}

TEST_CASE("world replace: a build that left no record is found by the world's names and node types; the user's nodes stay")
{
    owtest::loadPackages(true, true);
    run("newScene");
    Graph& g = Graph::current();
    // The user's own: a cube named like the world, an empty group named like the settlement's Buildings, and a group of theirs.
    run("polyCube", {{"name", "TinyHill"}});
    REQUIRE(g.find("TinyHill"));
    run("group", {{"empty", true}, {"name", "PuebloBuildingsNotes"}});
    run("group", {{"empty", true}, {"name", "Props"}});
    build(townSpec(), false);
    const std::map<std::string, int> counts = countsByType();
    const std::set<std::string> names = topLevel();

    // An older build: the world node renamed by naming rules, no record and no link ids, its Buildings group renamed and
    // a second one beside it, and a cell modifier its deleted mesh left behind.
    const std::string world = g.find("Tiny_World")->path();
    run("setAttr", {{"args", {world + "." + openworld::world::kBuiltAttr, "{}"}}});
    run("rename", {{"args", {world, "TinyWorld"}}});
    for (lila::scene::NodeId id : g.nodes())
        if (lila::scene::Node* n = g.node(id); n && n->type().dag && n->type().attrIndex("linkId") >= 0)
            run("setAttr", {{"args", {n->path() + ".linkId", ""}}});
    run("rename", {{"args", {"Pueblo_Buildings", "PuebloBuildings"}}});
    const lila::scene::NodeId cell = g.children(g.find("PuebloBuildings")->id()).front();
    const std::string cellPath = g.path(cell);
    run("delete", {{"args", {cellPath}}});
    run("group", {{"empty", true}, {"name", "PuebloBuildings1"}});
    run("addAttr", {{"args", {"PuebloBuildings1"}}, {"longName", "buildingsLod"}, {"dataType", "string"}});
    const json r = build(townSpec(), true);
    const std::string notes = notesOf(r);
    INFO(notes);
    CHECK(notes.find("matched by the world's names and node types") != std::string::npos);
    CHECK(notes.find("PuebloBuildings1") != std::string::npos);
    CHECK(notes.find("Buildings modifiers an earlier build left without a mesh held this build's buildings") != std::string::npos);
    CHECK(g.find("TinyHill"));
    CHECK(g.find("PuebloBuildingsNotes"));
    CHECK(g.find("Props"));
    sameAs(counts, names, "replace without a record");
}

TEST_CASE("world replace: a remixed world without a record loses its OSM lines, the remix's lines and its water too")
{
    owtest::loadPackages(true, true);
    run("newScene");
    Graph& g = Graph::current();
    const json r1 = build(remixSpec(), false);
    const std::map<std::string, int> counts = countsByType();
    const std::set<std::string> names = topLevel();
    INFO(json(names).dump());
    REQUIRE(r1["built"].contains("remixFeatures"));
    run("setAttr", {{"args", {g.find("Harbour_World")->path() + "." + openworld::world::kBuiltAttr, "{}"}}});
    for (lila::scene::NodeId id : g.nodes())
        if (lila::scene::Node* n = g.node(id); n && n->type().dag && n->type().attrIndex("linkId") >= 0)
            run("setAttr", {{"args", {n->path() + ".linkId", ""}}});

    const json r2 = build(remixSpec(), true);
    INFO(notesOf(r2));
    CHECK(notesOf(r2).find("matched by the world's names and node types") != std::string::npos);
    sameAs(counts, names, "remix replace without a record");
}
