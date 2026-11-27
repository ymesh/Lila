#include <doctest/doctest.h>

#include "geo/Mesh.h"
#include "io/SceneFile.h"
#include "ops/Command.h"
#include "ops/Undo.h"
#include "scene/Diagnostics.h"
#include "scene/Graph.h"
#include "scene/Node.h"
#include "scene/SceneData.h"
#include "slots/Slots.h"

#include <glm/gtc/matrix_transform.hpp>

#include <filesystem>

using namespace lila;
using json = nlohmann::json;

namespace
{
json run(const char* name, json call = json::object()) { return ops::execute(name, call); }

slots::Slot slot(const std::string& owner, const std::string& path, const std::string& type, glm::dvec3 at)
{
    slots::Slot s;
    s.owner = owner;
    s.id = slots::makeId(owner, path);
    s.type = type;
    s.transform = glm::translate(glm::dmat4(1.0), at);
    s.regions = {"settlement:Town", "district:1"};
    return s;
}

/** A street generator: one lamp every 10 m along each street, ids from the street id and the lamp's index. */
std::vector<slots::Slot> street(uint64_t streetId, int lamps, double z)
{
    std::vector<slots::Slot> out;
    for (int k = 0; k < lamps; ++k)
        out.push_back(slot("town/street/" + slots::hex(streetId), "lamp/" + std::to_string(k), "lamp.street", {k * 1000.0, 0, z}));
    return out;
}

void writeTown(const std::vector<slots::Slot>& all)
{
    UndoStack::get().openChunk("write");
    slots::writeSet(scene::Graph::current(), "TownSlots", scene::kNoNode, all, "test");
    UndoStack::get().closeChunk();
}

std::vector<slots::Slot> concat(std::vector<slots::Slot> a, const std::vector<slots::Slot>& b)
{
    a.insert(a.end(), b.begin(), b.end());
    return a;
}

void fresh()
{
    run("newScene");
    slots::refresh();
}
} // namespace

TEST_CASE("slot types resolve through their dotted ancestors and patterns match by segment")
{
    CHECK(slots::types().find("door.shop"));
    CHECK(slots::types().find("container.40ft"));
    CHECK(slots::types().find("tree.palm"));
    CHECK(slots::types().find("sign.storefront"));
    CHECK(slots::types().find("crop.row"));
    REQUIRE(slots::resolveType("door.shop.double"));
    CHECK(slots::resolveType("door.shop.double")->id == "door.shop");
    CHECK(slots::resolveType("sign.traffic.stop")->id == "sign.traffic");
    CHECK_FALSE(slots::resolveType("spaceship"));

    CHECK(slots::typeMatches("*", "door.shop"));
    CHECK(slots::typeMatches("door", "door.shop"));
    CHECK(slots::typeMatches("door", "door"));
    CHECK_FALSE(slots::typeMatches("door", "doormat"));
    CHECK(slots::typeMatches("door.*", "door.shop"));
    CHECK_FALSE(slots::typeMatches("door.*", "door"));
    CHECK(slots::typeMatches("sign.billboard, door", "door.garage"));

    const json listed = run("slotTypes", {{"args", {"door"}}});
    CHECK(listed.size() >= 5);
    run("slotTypes", {{"add", {{"id", "sign.neon"}, {"footprint", {200, 60, 10}}, {"attach", "wall"}, {"placeholder", "sign"}}}});
    CHECK(slots::types().find("sign.neon")->footprint.x == doctest::Approx(200.0));
}

TEST_CASE("slot ids are owner plus path and hash the same on every run")
{
    CHECK(slots::makeId("lot/1a2b", "f0/b3/door") == "lot/1a2b/f0/b3/door");
    CHECK(slots::makeId("/lot/", "/door/") == "lot/door");
    CHECK(slots::pathOf({"f0", "", "b3"}) == "f0/b3");
    CHECK(slots::hex(0x1a2b) == "1a2b");
    // FNV-1a 64: fixed so Unreal custom data and saved tables keep matching.
    CHECK(slots::idHash("") == 0xcbf29ce484222325ull);
    CHECK(slots::idHash("a") == 0xaf63dc4c8601ec8cull);
}

TEST_CASE("payload overlay replaces the visual source as a whole and merges data")
{
    slots::Payload base;
    base.poolTag = "signs";
    base.data = {{"shop", "bakery"}, {"locked", false}};
    slots::Payload over;
    over.item = "Street/Sign";
    over.data = {{"locked", true}, {"shop", nullptr}};
    base.overlay(over);
    CHECK(base.item == "Street/Sign");
    CHECK(base.poolTag.empty());
    CHECK(base.data == json{{"locked", true}});
    CHECK(slots::Payload::fromJson(base.toJson()).toJson() == base.toJson());
}

TEST_CASE("rules apply global, region, owner, then the override, and pools pick by id")
{
    fresh();
    const auto lamps = street(0x51, 4, 0.0);
    writeTown(lamps);
    run("slotRule", {{"args", {"add"}}, {"type", "lamp"}, {"item", "Street/LampA"}, {"name", "all lamps"}});
    run("slotRule", {{"args", {"add"}}, {"level", "owner"}, {"owner", "town/street/51"}, {"type", "lamp.street"}, {"item", "Street/LampB"}});
    run("slotRule", {{"args", {"add"}}, {"level", "region"}, {"region", "district:1"}, {"type", "*"}, {"data", {{"district", 1}}}});
    run("slotSet", {{"id", lamps[2].id}, {"unrealClass", "/Game/BP/BP_Lamp.BP_Lamp_C"}, {"data", {{"broken", true}}}});

    const json found = run("slotFind", {{"type", "lamp"}, {"full", true}});
    REQUIRE(found["count"] == 4);
    for (const json& s : found["slots"])
    {
        CHECK(s["payload"]["item"] == "Street/LampB");
        CHECK(s["payload"]["data"]["district"] == 1);
        // Owner rules come after region rules whatever order they were added in.
        CHECK(s["sources"] == (s["id"] == lamps[2].id ? json{"default", "rule:r1", "rule:r3", "rule:r2", "override"}
                                                          : json{"default", "rule:r1", "rule:r3", "rule:r2"}));
    }
    const std::optional<slots::Slot> third = slots::findSlot(scene::Graph::current(), lamps[2].id);
    REQUIRE(third);
    const slots::Resolved r = slots::resolve(*third);
    CHECK(r.payload.unrealClass == "/Game/BP/BP_Lamp.BP_Lamp_C");
    CHECK(r.payload.data["broken"] == true);

    // A pool picks the same member for a slot every time, and spreads across slots.
    run("slotRule", {{"args", {"edit"}}, {"id", "r2"}, {"replacePayload", true}, {"pool", {"A", "B", "C", "D", "E"}}});
    std::set<std::string> picks;
    for (const slots::Slot& s : lamps)
    {
        const std::string pick = slots::resolve(s).poolPick;
        CHECK(slots::resolve(s).poolPick == pick);
        picks.insert(pick);
    }
    CHECK(picks.size() > 1);

    const json rep = run("slotReport", {{"validate", false}});
    CHECK(rep["total"] == 4);
    CHECK(rep["byType"]["lamp.street"] == 4);
    CHECK(rep["byRule"]["r1"] == 4);
    CHECK(rep["overrides"] == 1);
}

TEST_CASE("overrides survive regeneration, orphans are reported and reassigned")
{
    fresh();
    const auto a = street(0xaa, 3, 0.0);
    const auto b = street(0xbb, 3, 500.0);
    writeTown(concat(a, b));
    run("slotSet", {{"id", a[1].id}, {"item", "Street/Special"}});
    run("slotSet", {{"id", b[2].id}, {"hidden", true}});

    // An unrelated edit (street bb loses its last lamp, a new street appears) regenerates everything: aa's ids hold.
    writeTown(concat(concat(a, street(0xcc, 2, 900.0)), street(0xbb, 2, 500.0)));
    std::optional<slots::Slot> kept = slots::findSlot(scene::Graph::current(), a[1].id);
    REQUIRE(kept);
    CHECK(slots::resolve(*kept).payload.item == "Street/Special");

    const json rep = run("slotReport", {{"validate", true}});
    REQUIRE(rep["orphans"].size() == 1);
    CHECK(rep["orphans"][0]["id"] == b[2].id);
    CHECK(rep["orphans"][0]["nearest"] == b[1].id);
    size_t orphanIssues = 0;
    for (const auto& i : scene::diagnostics::issues())
        if (i.source == "slots" && i.message.find("has no slot") != std::string::npos)
        {
            ++orphanIssues;
            CHECK(i.fix.find("reassign") != std::string::npos);
            CHECK_FALSE(i.select.empty());
        }
    CHECK(orphanIssues == 1);

    run("slotSet", {{"id", b[2].id}, {"reassign", b[1].id}});
    CHECK(run("slotReport", {{"validate", false}})["orphans"].empty());
    CHECK(slots::resolve(*slots::findSlot(scene::Graph::current(), b[1].id)).payload.hidden.value_or(false));
}

TEST_CASE("every rule and override edit undoes and redoes")
{
    fresh();
    writeTown(street(0x1, 2, 0.0));
    run("slotRule", {{"args", {"add"}}, {"type", "lamp"}, {"item", "X"}});
    CHECK(slots::state()->rules.size() == 1);
    run("slotRule", {{"args", {"edit"}}, {"id", "r1"}, {"enabled", false}});
    CHECK_FALSE(slots::state()->rules[0].enabled);
    run("undo");
    CHECK(slots::state()->rules[0].enabled);
    run("undo");
    CHECK(slots::state()->rules.empty());
    run("redo");
    CHECK(slots::state()->rules.size() == 1);

    const std::string id = street(0x1, 2, 0.0)[0].id;
    run("slotSet", {{"id", id}, {"data", {{"locked", true}}}});
    CHECK(slots::state()->overrides.contains(id));
    run("undo");
    CHECK_FALSE(slots::state()->overrides.contains(id));
    run("redo");
    CHECK(slots::state()->overrides.at(id).payload.data["locked"] == true);
    run("slotPin", {{"args", {id}}});
    CHECK(slots::isPinned(id));
    run("undo");
    CHECK_FALSE(slots::isPinned(id));
}

TEST_CASE("rules, overrides, pins and slot sets are saved with the scene")
{
    fresh();
    const auto lamps = street(0x77, 3, 0.0);
    writeTown(lamps);
    run("slotRule", {{"args", {"add"}}, {"type", "lamp"}, {"poolTag", "fictional"}, {"name", "Fictional lamps"}});
    run("slotSet", {{"id", lamps[0].id}, {"unrealClass", "/Game/BP/Lamp"}});
    run("slotPin", {{"args", {lamps[1].id}}});
    const auto file = std::filesystem::temp_directory_path() / "lila_slots_persist.lila";
    io::save(file);
    fresh();
    CHECK(slots::state()->rules.empty());
    // The rules apply as soon as the scene's data is read, before the rest of the load evaluates anything.
    static size_t rulesWhileLoading = 0;
    static bool loadingSeen = false;
    static const bool hooked = [] {
        scene::data::onLoaded([] {
            rulesWhileLoading = slots::state()->rules.size();
            loadingSeen = io::loading();
        });
        return true;
    }();
    CHECK(hooked);
    io::load(file);
    CHECK(rulesWhileLoading == 1);
    CHECK(loadingSeen);
    CHECK_FALSE(io::loading());
    REQUIRE(slots::state()->rules.size() == 1);
    CHECK(slots::state()->rules[0].name == "Fictional lamps");
    CHECK(slots::state()->overrides.at(lamps[0].id).payload.unrealClass == "/Game/BP/Lamp");
    CHECK(slots::isPinned(lamps[1].id));
    CHECK(slots::collect(scene::Graph::current()).size() == 3);
    std::filesystem::remove(file);
}

TEST_CASE("pins keep slots where they were and report what regeneration changed")
{
    fresh();
    const auto lamps = street(0x9, 3, 0.0);
    writeTown(lamps);
    run("slotRule", {{"args", {"add"}}, {"type", "lamp"}, {"item", "Before"}});
    run("slotPin", {{"args", {lamps[0].id}}});
    run("slotPin", {{"owner", "town/street/9"}});
    run("slotRule", {{"args", {"edit"}}, {"id", "r1"}, {"item", "After"}});

    // Regenerate: everything moves by 50 cm and a fourth lamp appears under the pinned owner.
    auto moved = street(0x9, 4, 50.0);
    const json stats = slots::writeSet(scene::Graph::current(), "TownSlots", scene::kNoNode, moved, "test");
    CHECK(stats["conflicts"].size() == 4);
    const std::vector<slots::Slot> all = slots::collect(scene::Graph::current());
    CHECK(all.size() == 3);
    for (const slots::Slot& s : all)
    {
        CHECK(s.transform[3].z == doctest::Approx(0.0));
        CHECK(slots::resolve(s).payload.item == "Before");
    }
    CHECK(run("slotReport", {{"validate", false}})["conflicts"].size() == 4);

    run("slotPin", {{"owner", "town/street/9"}, {"remove", true}});
    run("slotPin", {{"args", {lamps[0].id}}, {"remove", true}});
    for (const slots::Slot& s : slots::collect(scene::Graph::current()))
        CHECK(slots::resolve(s).payload.item == "After");
}

TEST_CASE("swappable pins keep slots where they were but rules and overrides still swap them")
{
    fresh();
    const auto lamps = street(0x7, 2, 0.0);
    writeTown(lamps);
    run("slotPin", {{"owner", "town/street/7"}, {"swappable", true}});
    run("slotRule", {{"args", {"add"}}, {"type", "lamp"}, {"item", "Rule"}});
    run("slotSet", {{"id", lamps[1].id}, {"item", "Mine"}});

    auto moved = street(0x7, 2, 50.0);
    const json stats = slots::writeSet(scene::Graph::current(), "TownSlots", scene::kNoNode, moved, "test");
    CHECK(stats["conflicts"].size() == 2);
    std::map<std::string, std::string> items;
    for (const slots::Slot& s : slots::collect(scene::Graph::current()))
    {
        CHECK(s.transform[3].z == doctest::Approx(0.0));
        items[s.id] = slots::resolve(s).payload.item;
    }
    CHECK(items[lamps[0].id] == "Rule");
    CHECK(items[lamps[1].id] == "Mine");
    CHECK(run("slotPin")["pins"][0]["swappable"] == true);
}

TEST_CASE("visuals fall back to the type's placeholder and say why")
{
    fresh();
    writeTown(street(0x3, 2, 0.0));
    scene::Graph& g = scene::Graph::current();
    const std::vector<slots::DrawBatch> before = slots::drawBatches(g, g.find("TownSlots")->id());
    REQUIRE(before.size() == 1);
    CHECK(before[0].key.starts_with("placeholder:lamp/"));
    CHECK(before[0].transforms.size() == 2);

    run("polyCube", {{"name", "LampMesh"}});
    run("slotRule", {{"args", {"add"}}, {"type", "lamp"}, {"mesh", "LampMesh"}});
    const std::vector<slots::DrawBatch> after = slots::drawBatches(g, g.find("TownSlots")->id());
    REQUIRE(after.size() == 1);
    CHECK(after[0].key == "mesh:LampMesh");
    CHECK(after[0].mesh->numFaces() == 6);

    run("slotRule", {{"args", {"edit"}}, {"id", "r1"}, {"replacePayload", true}, {"item", "No/SuchItem"}});
    const json rep = run("slotReport", {{"validate", false}});
    REQUIRE(rep["missingVisuals"].size() == 1);
    CHECK(rep["missingVisuals"][0]["key"] == "item:No/SuchItem");

    run("slotRule", {{"args", {"edit"}}, {"id", "r1"}, {"replacePayload", true}, {"hidden", true}});
    CHECK(slots::drawBatches(g, g.find("TownSlots")->id()).empty());
}

TEST_CASE("slotPublish writes slots from scripts and slotFind filters them")
{
    fresh();
    run("slotPublish", {{"args", {"Shops"}},
                        {"slots",
                         {{{"owner", "lot/7"}, {"path", "f0/door"}, {"type", "door.shop"}, {"position", {0, 0, 500}}, {"rotateY", 90}, {"tags", {"front"}}},
                          {{"owner", "lot/7"}, {"path", "f0/sign"}, {"type", "sign.storefront"}, {"position", {0, 300, 500}}},
                          {{"owner", "lot/8"}, {"path", "f0/door"}, {"type", "door.house"}, {"position", {2000, 0, 500}}, {"regions", {"district:2"}}}}}});
    CHECK(run("slotFind", {{"type", "door"}})["count"] == 2);
    CHECK(run("slotFind", {{"owner", "lot/7"}})["count"] == 2);
    CHECK(run("slotFind", {{"region", "district:"}})["count"] == 1);
    CHECK(run("slotFind", {{"tag", {"front"}}})["count"] == 1);
    const json near = run("slotFind", {{"near", {1900, 0, 500}}, {"radius", 500}});
    REQUIRE(near["count"] == 1);
    CHECK(near["slots"][0]["id"] == "lot/8/f0/door");
    const json sel = run("slotFind", {{"type", "sign"}, {"select", true}});
    CHECK(sel["selected"] == json{"Shops"});
}

TEST_CASE("a replaced scene's publishers drop what they published")
{
    fresh();
    run("createNode", {{"args", {"transform"}}, {"name", "Kept"}});
    const scene::NodeId kept = scene::Graph::current().find("Kept")->id();
    slots::publish(kept, street(1, 3, 0.0));
    slots::publish(scene::NodeId(kept + 100000), street(2, 4, 0.0));
    CHECK(slots::publishedSlots(kept).size() == 3);
    CHECK(slots::publishedSlots(scene::NodeId(kept + 100000)).size() == 4);
    slots::unpublishGone(scene::Graph::current());
    CHECK(slots::publishedSlots(kept).size() == 3);
    CHECK(slots::publishedSlots(scene::NodeId(kept + 100000)).empty());
    // A new scene deletes no node one by one: its publishers' slots go with the scene.
    run("newScene");
    CHECK(slots::publishedSlots(kept).empty());
}
