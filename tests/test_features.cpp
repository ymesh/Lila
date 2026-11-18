#include <doctest/doctest.h>

#include "core/Features.h"
#include "core/Paths.h"
#include "extensions/ExtensionManager.h"
#include "ui/Actions.h"
#include "ui/FeatureStatus.h"
#include "ui/Menus.h"

#include <fstream>

using namespace lila;
using json = nlohmann::json;

namespace
{
/** A small roadmap in place of the shipped one, put back however the test ends. */
struct Roadmap
{
    Roadmap()
    {
        json roadmap = {{"sections",
                         {{{"id", "alpha"},
                           {"cards",
                            {{{"id", "done"}, {"title", "Done"}, {"status", "ready"}},
                             {{"id", "wip"}, {"title", "Work"}, {"status", "dev"}},
                             {{"id", "wip-part"}, {"title", "Part"}, {"status", "dev"}},
                             {{"id", "held"}, {"title", "Held"}, {"status", "dev"}, {"note", "Please don't try it yet."}},
                             {{"id", "later"}, {"title", "Later"}, {"status", "planned"}}}}}}}};
        json map = {{"done", {{"actions", {"ft.done."}}}},
                    {"wip", {{"extensions", {"FtWip"}}, {"menus", {"FtMenu"}}, {"actions", {"ft.wip."}}}},
                    {"wip-part", {{"actions", {"ft.wip.part"}}}},
                    {"held", {{"extensions", {"FtHeld"}}, {"menus", {"FtMenu/Held"}}}}};
        features::set(roadmap, map);
    }
    ~Roadmap() { features::reset(); }
};
} // namespace

TEST_CASE("features: a card's status, with a note meaning not ready for use")
{
    Roadmap r;
    CHECK(features::find("done")->status == features::Status::Ready);
    CHECK(features::find("wip")->status == features::Status::InDevelopment);
    CHECK(features::find("held")->status == features::Status::NotReady);
    CHECK(features::find("later")->status == features::Status::Planned);
    CHECK(features::find("missing") == nullptr);
    CHECK(std::string(features::label(features::Status::InDevelopment)) == "In development");
    CHECK(std::string(features::label(features::Status::NotReady)) == "Not ready for use");
    CHECK(std::string(features::label(features::Status::Ready)).empty());
}

TEST_CASE("features: actions match by longest prefix, then by owning extension; menus by the deepest mapped path")
{
    Roadmap r;
    CHECK(features::ofAction("ft.wip.tool")->id == "wip");
    CHECK(features::ofAction("ft.wip.partTool")->id == "wip-part");
    CHECK(features::ofAction("other.tool", "FtHeld")->id == "held");
    CHECK(features::ofAction("other.tool", "lila") == nullptr);
    CHECK(features::ofMenu("FtMenu")->id == "wip");
    CHECK(features::ofMenu("FtMenu/Sub")->id == "wip");
    CHECK(features::ofMenu("FtMenu/Held/Deep")->id == "held");
    CHECK(features::ofMenu("FtMenuOther") == nullptr);
    CHECK(features::extensionStatus("FtHeld") == features::Status::NotReady);
    CHECK(features::extensionStatus("Unmapped") == features::Status::Ready);
}

TEST_CASE("features: an extension that is not ready for use does not load by default")
{
    Roadmap r;
    extensions::ExtensionManager& pm = extensions::ExtensionManager::get();
    extensions::ExtensionInfo info;
    info.manifest = ext::Manifest{};
    info.manifest->enabledByDefault = true;
    info.name = "FtWip";
    CHECK(pm.autoloads(info));
    info.name = "FtHeld";
    CHECK_FALSE(pm.autoloads(info));
}

TEST_CASE("features: tooltips end with the status, and a menu row takes its menu's when it has none of its own")
{
    Roadmap r;
    using namespace lila::ui;
    actions::add({"ft.wip.tool", "Tool", "Test", [] {}, nullptr, "Does it."});
    actions::add({"ft.done.tool", "Done Tool", "Test", [] {}, nullptr, "Does it."});
    actions::add({"ft.plain", "Plain", "Test", [] {}, nullptr, "Does it."});
    menus::addMenu({.id = "FtMenu", .title = "Ft"});
    menus::addActions("FtMenu", {"ft.plain", "ft.done.tool"});

    CHECK(actions::tooltip("ft.wip.tool") == "Tool\nDoes it.\nIn development");
    CHECK(actions::tooltip("ft.done.tool") == "Done Tool\nDoes it.");
    // Listed in a mapped menu.
    CHECK(actions::tooltip("ft.plain") == "Plain\nDoes it.\nIn development");
    CHECK(featureStatus::rowStatus("ft.plain", "FtMenu") == features::Status::InDevelopment);
    CHECK(featureStatus::rowStatus("ft.done.tool", "FtMenu") == features::Status::Ready);
    CHECK(featureStatus::rowStatus("ft.plain", "Elsewhere") == features::Status::Ready);

    menus::removeMenu("FtMenu");
    for (const char* id : {"ft.wip.tool", "ft.done.tool", "ft.plain"})
        actions::remove(id);
}

TEST_CASE("features: every card the shipped map names is on the shipped roadmap")
{
    features::reset();
    REQUIRE_FALSE(features::all().empty());
    std::ifstream in(paths::resourcesDir() / "roadmap" / "features.json");
    REQUIRE(in);
    const json map = json::parse(in);
    for (auto it = map.begin(); it != map.end(); ++it)
        CHECK_MESSAGE(features::find(it.key()), it.key());
}
