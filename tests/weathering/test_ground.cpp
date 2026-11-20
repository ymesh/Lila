#include "TestUtil.h"

#include "geo/ColorSets.h"

#include <set>

using namespace weatheringtest;

namespace
{
std::string ground(int subdivisions = 20)
{
    return run("polyPlane", {{"name", "Ground"}, {"width", 1000.0}, {"depth", 1000.0}, {"subdivisionsX", subdivisions}, {"subdivisionsZ", subdivisions}})[0]
        .get<std::string>();
}

/** The tint at the ground point nearest (x, z). */
float tintAt(const geo::Mesh& m, double x, double z, const char* attr = "weatheringTint")
{
    const std::span<const float> t = m.pointAttrs().get<float>(attr);
    REQUIRE(!t.empty());
    geo::Index best = 0;
    double bestD = 1e300;
    for (geo::Index p = 0; p < m.pointCapacity(); ++p)
        if (m.pointValid(p))
        {
            const glm::dvec3 q = m.position(p);
            const double d = (q.x - x) * (q.x - x) + (q.z - z) * (q.z - z);
            if (d < bestD)
                bestD = d, best = p;
        }
    return t[best];
}
} // namespace

TEST_CASE("weathering: ground dressing scatters, colours and tints the ground near walls")
{
    fresh();
    const std::string gr = ground();
    cube("Wall", {0, 0, 0}, {300, 250, 40});
    const json r = run("groundDressing", {{"args", {gr}}, {"preset", "Rubble Ground"}});
    CHECK(r["scatter"]["count"].get<int>() > 0);
    CHECK(!r["material"].get<std::string>().empty());
    CHECK(!r["group"].get<std::string>().empty());
    REQUIRE(!r["tint"]["modifier"].get<std::string>().empty());
    const geo::Mesh m = meshOf(gr);
    CHECK(geo::colors::has(m, "weathering"));
    CHECK(tintAt(m, 0, 50) > 0.3f);
    CHECK(tintAt(m, 450, 450) < 0.05f);
}

TEST_CASE("weathering: ground tint follows a wall that moves")
{
    fresh();
    const std::string gr = ground();
    cube("Wall", {0, 0, 0}, {200, 250, 40});
    run("groundTint", {{"args", {gr}}, {"sources", {"Wall"}}, {"reach", 80.0}, {"wetness", 0.0}});
    CHECK(tintAt(meshOf(gr), 0, 50) > 0.3f);
    CHECK(tintAt(meshOf(gr), 300, 300) < 0.05f);
    run("move", {{"args", {"Wall"}}, {"by", {300, 0, 300}}});
    CHECK(tintAt(meshOf(gr), 300, 350) > 0.3f);
    CHECK(tintAt(meshOf(gr), 0, 50) < 0.05f);
}

TEST_CASE("weathering: ground tint wets dips and steps into bands for toon")
{
    fresh();
    // A 20 x 20 grid over 1000 cm with a 25 cm dip in the middle.
    json points = json::array(), faces = json::array();
    for (int z = 0; z <= 20; ++z)
        for (int x = 0; x <= 20; ++x)
        {
            const double px = -500.0 + 50.0 * x, pz = -500.0 + 50.0 * z;
            points.push_back({px, std::abs(px) < 60.0 && std::abs(pz) < 60.0 ? -25.0 : 0.0, pz});
        }
    for (int z = 0; z < 20; ++z)
        for (int x = 0; x < 20; ++x)
            faces.push_back({z * 21 + x, (z + 1) * 21 + x, (z + 1) * 21 + x + 1, z * 21 + x + 1});
    const json made = run("createMesh", {{"points", points}, {"faces", faces}, {"name", "Dipped"}});
    const std::string gr = made.is_string() ? made.get<std::string>() : made[0].get<std::string>();
    cube("Rock", {300, 0, 300}, {60, 60, 60});
    run("groundTint", {{"args", {gr}}, {"sources", {"Rock"}}, {"wetness", 1.0}, {"wetDepth", 8.0}, {"wetRadius", 200.0}});
    const geo::Mesh m = meshOf(gr);
    CHECK(tintAt(m, 0, 0, "weatheringWet") > 0.5f);
    CHECK(tintAt(m, -450, -450, "weatheringWet") < 0.1f);

    fresh();
    const std::string g2 = ground(20);
    cube("Rock", {0, 0, 0}, {100, 60, 100});
    run("groundTint", {{"args", {g2}}, {"sources", {"Rock"}}, {"reach", 300.0}, {"wetness", 0.0}, {"style", "toon"}});
    std::set<int> levels;
    for (float t : meshOf(g2).pointAttrs().get<float>("weatheringTint"))
        levels.insert(int(std::lround(t * 1000.0f)));
    CHECK(levels.size() <= 4);
    CHECK(levels.size() >= 2);
}

TEST_CASE("weathering: ground presets in both styles run through Stagehand")
{
    fresh();
    const std::string gr = ground(10);
    std::set<std::string> names;
    for (const json& p : run("arrangementPresets"))
        names.insert(p["name"].get<std::string>());
    for (const char* n : {"Rubble Ground", "Asphalt Crack", "Dirt and Litter", "Grass Patches", "Moss Floor"})
    {
        CHECK(names.contains(n));
        CHECK(names.contains(std::string(n) + " Toon"));
    }
    const json r = run("arrangePreset", {{"args", {"Grass Patches Toon"}}, {"slots", {{"ground", gr}}}});
    CHECK(!r["group"].get<std::string>().empty());
    const json step = r["steps"][0]["result"];
    CHECK(step["scatter"]["count"].get<int>() > 0);
    for (const auto& [id, count] : step["scatter"]["perItem"].items())
        CHECK(id.find("Toon") != std::string::npos);
}

TEST_CASE("weathering: a source that reads the ground is taken as it stands, and said so")
{
    fresh();
    const std::string gr = ground();
    cube("Wall", {0, 0, 0}, {200, 250, 40});
    run("grungeMask", {{"args", {"Wall"}}});
    const json r = run("groundTint", {{"args", {gr}}, {"sources", {"Wall"}}, {"reach", 80.0}, {"wetness", 0.0}});
    CHECK(r["static"].size() == 1);
    CHECK(r.contains("note"));
    CHECK(tintAt(meshOf(gr), 0, 50) > 0.3f);
}
