#include <doctest/doctest.h>

#include "core/Release.h"
#include "geo/Mesh.h"
#include "geo/Primitives.h"
#include "ops/Command.h"
#include "ui/PerfTrace.h"

using namespace lila;

TEST_CASE("performance traces: the dialog records up to two minutes, commands up to three hours")
{
    using ui::perfTrace::recordingSeconds;
    CHECK(recordingSeconds(10, false) == 10);
    CHECK(recordingSeconds(600, false) == 120);
    CHECK(recordingSeconds(0, false) == 1);
    CHECK(recordingSeconds(600, true) == 600);
    CHECK(recordingSeconds(3 * 3600, true) == 3 * 3600);
    CHECK(recordingSeconds(3 * 3600 + 1, true) == 3 * 3600);
    CHECK(recordingSeconds(-5, true) == 1);

    ops::registerBuiltins();
    if (!ops::find("perfTraceRecord"))
        ui::perfTrace::registerAll(nullptr);
    const CommandDef* record = ops::find("perfTraceRecord");
    REQUIRE(record);
    REQUIRE(!record->flags.empty());
    CHECK(record->flags[0].name == "seconds");
    REQUIRE(record->flags[0].max);
    CHECK(*record->flags[0].max == doctest::Approx(3 * 3600));
}

TEST_CASE("memory report: heaps, live types and who holds an object")
{
    ops::registerBuiltins();
    if (!ops::find("memoryReport"))
        ui::perfTrace::registerAll(nullptr);
    const json plain = ops::execute("memoryReport", json::object());
    CHECK(plain["commitMB"].get<double>() > 0.0);
    CHECK(plain.contains("releasePending"));

    // Held by a global the census and the trace can name.
    static std::vector<std::shared_ptr<const geo::Mesh>> held;
    for (int i = 0; i < 5; ++i)
        held.push_back(std::make_shared<const geo::Mesh>(geo::makeCube()));
    const json r = ops::execute("memoryReport", {{"heaps", true}, {"drain", true}, {"types", 400}, {"holders", "lila::geo::Mesh const >"}, {"samples", 4}, {"depth", 2}});
    CHECK(r["busyMB"].get<double>() > 0.0);
    CHECK(r["busyBlocks"].get<double>() > 0.0);
    bool meshes = false;
    for (const json& t : r["types"])
        meshes = meshes || (t["type"].get<std::string>().find("lila::geo::Mesh const >") != std::string::npos && t["count"].get<int>() >= 5);
    CHECK(meshes);
    REQUIRE(r["holders"]["objects"].get<int>() >= 1);
    REQUIRE(!r["holders"]["levels"].empty());
    CHECK(!r["holders"]["levels"][0].empty());
    held.clear();
}
