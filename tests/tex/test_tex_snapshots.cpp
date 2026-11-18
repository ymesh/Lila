#include "TexTest.h"

#include "scene/Graph.h"
#include "tex/Snapshots.h"
#include "tex/TextureSet.h"

#include <cstring>

using namespace textest;
namespace snap = lila::tex::snapshots;

namespace snaptest
{
void layered()
{
    run("newScene");
    snap::clear();
    run("texCreateSet", {{"args", {"Crate"}}, {"resolution", 96}});
    run("texAddLayer", {{"args", {"Crate", "fill"}}, {"name", "Base"}, {"material", {{"baseColor", "#808080"}, {"roughness", 0.6}}}});
}

image::Image evaluated(const std::string& channel)
{
    scene::Node* set = findSet(scene::Graph::current(), "Crate");
    REQUIRE(set);
    const std::vector<OutputRequest> requests = channelRequests(readGraph(*set), readSettings(*set));
    return evaluateOutputs(*set, requests, 1001, 96, 96, "cpu").at(channel);
}
} // namespace snaptest

using namespace snaptest;

TEST_CASE("texture set snapshots restore bitwise after the set changes, and share unchanged pages")
{
    layered();
    const image::Image before = evaluated("baseColor");
    json first = run("texSnapshot", {{"args", {"Crate"}}, {"name", "grey"}, {"resolution", 96}, {"backend", "cpu"}});
    CHECK(first["name"] == "grey");
    CHECK(first["newPages"].get<int>() > 0);

    // The same set again adds no pages.
    json again = run("texSnapshot", {{"args", {"Crate"}}, {"name", "grey again"}, {"resolution", 96}, {"backend", "cpu"}});
    CHECK(again["newPages"] == 0);

    run("texAddLayer", {{"args", {"Crate", "fill"}}, {"name", "Red"}, {"material", {{"baseColor", "#FF0000"}}}});
    CHECK(evaluated("baseColor").rgba != before.rgba);

    const std::shared_ptr<const snap::Snapshot> s = snap::find("Crate", "grey");
    REQUIRE(s);
    const image::Image restored = snap::image(*s, "baseColor");
    REQUIRE(restored.width == before.width);
    REQUIRE(restored.height == before.height);
    CHECK(std::memcmp(restored.rgba.data(), before.rgba.data(), before.rgba.size() * sizeof(float)) == 0);

    json listed = run("texSnapshots", {{"args", {"Crate"}}});
    CHECK(listed["snapshots"].size() == 2);
    CHECK(run("texDeleteSnapshot", {{"args", {"Crate", "grey again"}}}) == true);
    CHECK(run("texSnapshots", {{"args", {"Crate"}}})["snapshots"].size() == 1);
    CHECK(snap::image(*s, "nothing").width == 0);
}
