#include "TestUtil.h"

#include "geo/ObjIO.h"
#include "scene/Skin.h"

#include <cmath>
#include <cstdlib>
#include <filesystem>

using namespace rigtest;
using namespace lila::scene;

namespace
{
constexpr int kAround = 24, kRows = 20;
constexpr double kOuter = 12.0, kInner = 10.0, kHalf = 50.0;

/** A closed thick tube: outside, lining, and the hems joining them; a coat sleeve in miniature. */
std::string thickTube()
{
    run("newScene");
    json pts = json::array(), faces = json::array();
    for (int layer = 0; layer < 2; ++layer)
        for (int r = 0; r <= kRows; ++r)
            for (int k = 0; k < kAround; ++k)
            {
                const double a = 2.0 * 3.14159265358979323846 * k / kAround, rad = layer == 0 ? kOuter : kInner;
                pts.push_back({rad * std::cos(a), -kHalf + 2.0 * kHalf * r / kRows, rad * std::sin(a)});
            }
    auto at = [](int layer, int r, int k) { return layer * (kRows + 1) * kAround + r * kAround + (k % kAround); };
    for (int r = 0; r < kRows; ++r)
        for (int k = 0; k < kAround; ++k)
        {
            faces.push_back({at(0, r, k), at(0, r + 1, k), at(0, r + 1, k + 1), at(0, r, k + 1)});
            faces.push_back({at(1, r, k), at(1, r, k + 1), at(1, r + 1, k + 1), at(1, r + 1, k)});
        }
    for (int k = 0; k < kAround; ++k)
    {
        faces.push_back({at(0, 0, k), at(0, 0, k + 1), at(1, 0, k + 1), at(1, 0, k)});
        faces.push_back({at(0, kRows, k), at(1, kRows, k), at(1, kRows, k + 1), at(0, kRows, k + 1)});
    }
    std::string mesh = run("createMesh", {{"points", pts}, {"faces", faces}, {"name", "Sleeve"}});
    run("select", {{"args", json::array()}});
    run("joint", {{"position", v3(0, -60, 0)}, {"name", "bone_a"}});
    run("joint", {{"position", v3(0, 0, 0)}, {"name", "bone_b"}});
    run("joint", {{"position", v3(0, 60, 0)}, {"name", "bone_c"}});
    run("skinCluster", {{"args", {"bone_a", mesh}}});
    return mesh;
}

int outerPoint(int r, int k) { return r * kAround + k; }
int innerPoint(int r, int k) { return (kRows + 1) * kAround + r * kAround + k; }
} // namespace

TEST_CASE("a weight proxy is one sheet over a thick mesh, with loops at the joints, beside it")
{
    std::string mesh = thickTube();
    std::string proxy = run("createWeightProxy", {{"args", {mesh}}, {"loopsPerBone", 3}});
    CHECK(proxy == "Sleeve_weightProxy");
    const auto& roots = graph().roots();
    const auto it = std::find(roots.begin(), roots.end(), id(mesh));
    REQUIRE(it != roots.end());
    REQUIRE(it + 1 != roots.end());
    CHECK(*(it + 1) == id(proxy));

    json info = run("weightProxyInfo", {{"args", {mesh}}});
    CHECK(info["proxy"] == proxy);
    CHECK(info["meshes"] == json::array({mesh}));
    CHECK(run("skinInfo", {{"args", {proxy}}})["influences"].size() == 3);

    // Only the outside: no point near the lining.
    MeshPtr m = outMesh(proxy);
    if (std::getenv("LILA_PROXY_DUMP"))
        geo::writeObjFile((std::filesystem::temp_directory_path() / "proxy.obj").string(), *m);
    int nearJoint = 0, total = 0;
    for (geo::Index p = 0; p < m->pointCapacity(); ++p)
    {
        if (!m->pointValid(p))
            continue;
        ++total;
        const glm::dvec3 q = m->position(p);
        CHECK(std::hypot(q.x, q.z) > kInner + 0.75);
        nearJoint += std::abs(q.y) < 1.0;
    }
    REQUIRE(total > 20);
    // A loop lands on bone_b's joint, and more sit evenly along each bone (those at +-45 are too near the hems to fit).
    CHECK(nearJoint >= 5);
    for (double y : {-30.0, -15.0, 15.0, 30.0})
    {
        CAPTURE(y);
        int near = 0;
        for (geo::Index p = 0; p < m->pointCapacity(); ++p)
            near += m->pointValid(p) && std::abs(m->position(p).y - y) < 1.5;
        CHECK(near >= 4);
    }
}

TEST_CASE("pushing a proxy gives a thick mesh's outside and lining the same weights; pull goes back")
{
    std::string mesh = thickTube();
    std::string proxy = run("createWeightProxy", {{"args", {mesh}}});
    run("floodSkinWeights", {{"args", {proxy}}, {"influence", "bone_c"}, {"value", 1.0}, {"strict", false}});
    json r = run("pushProxyWeights", {{"args", {proxy}}});
    CHECK(r[mesh].get<int>() == 2 * (kRows + 1) * kAround);
    for (int row : {2, 10, 18})
        for (int k : {0, 7, 15})
        {
            json outer = run("skinPercent", {{"args", {mesh}}, {"vertices", {outerPoint(row, k)}}})[0]["weights"];
            json inner = run("skinPercent", {{"args", {mesh}}, {"vertices", {innerPoint(row, k)}}})[0]["weights"];
            CHECK(outer.value("bone_c", 0.0) == doctest::Approx(1.0).epsilon(1e-3));
            for (auto& [j, w] : outer.items())
                CHECK(inner.value(j, 0.0) == doctest::Approx(w.get<double>()).epsilon(1e-3));
        }
    run("undo");
    CHECK(run("skinPercent", {{"args", {mesh}}, {"vertices", {outerPoint(2, 0)}}})[0]["weights"].value("bone_c", 0.0) < 0.5);

    // Pull: the proxy takes the render mesh's weights back (its bottom is bone_a's).
    run("pullProxyWeights", {{"args", {mesh}}});
    MeshPtr m = outMesh(proxy);
    json all = run("skinPercent", {{"args", {proxy}}});
    for (geo::Index p = 0; p < m->pointCapacity(); ++p)
        if (m->pointValid(p) && m->position(p).y < -40.0)
            CHECK(all[p]["weights"].value("bone_a", 0.0) > 0.5);
}

TEST_CASE("a weight proxy keeps its render meshes through save and load")
{
    std::string mesh = thickTube();
    std::string proxy = run("createWeightProxy", {{"args", {mesh}}});
    auto path = (std::filesystem::temp_directory_path() / "lila_rig_tests" / "proxy.lila").string();
    run("saveScene", {{"args", {path}}});
    run("newScene");
    run("openScene", {{"args", {path}}});
    CHECK(run("weightProxyInfo", {{"args", {proxy}}})["meshes"] == json::array({mesh}));
    CHECK(run("pushProxyWeights", {{"args", {mesh}}}).contains(mesh));
}

TEST_CASE("an unskinned mesh needs joints for its proxy, and is bound on push")
{
    run("newScene");
    std::string mesh = run("polyCylinder", {{"name", "Tube"}, {"radius", 10}, {"height", 100}, {"subdivisionsHeight", 10}, {"caps", true}})[0];
    run("select", {{"args", json::array()}});
    run("joint", {{"position", v3(0, -50, 0)}, {"name", "j1"}});
    run("joint", {{"position", v3(0, 50, 0)}, {"name", "j2"}});
    CHECK_THROWS(run("createWeightProxy", {{"args", {mesh}}}));
    std::string proxy = run("createWeightProxy", {{"args", {mesh}}, {"joints", {"j1", "j2"}}});
    CHECK(run("weightProxyInfo", {{"args", {proxy}}})["meshes"] == json::array({mesh}));
    run("pushProxyWeights", {{"args", {proxy}}});
    CHECK(run("skinInfo", {{"args", {mesh}}})["unweighted"] == 0);
}

TEST_CASE("a proxy over a single-sided mesh")
{
    run("newScene");
    std::string mesh = run("polyCylinder", {{"name", "Coat"}, {"radius", 12}, {"height", 100}, {"subdivisionsHeight", 20}, {"caps", false}})[0];
    run("select", {{"args", json::array()}});
    run("joint", {{"position", v3(0, -60, 0)}, {"name", "bone_a"}});
    run("joint", {{"position", v3(0, 0, 0)}, {"name", "bone_b"}});
    run("joint", {{"position", v3(0, 60, 0)}, {"name", "bone_c"}});
    run("skinCluster", {{"args", {"bone_a", mesh}}});
    std::string proxy = run("createWeightProxy", {{"args", {mesh}}, {"edgesAround", 10}});
    CHECK(run("weightProxyInfo", {{"args", {proxy}}})["meshes"] == json::array({mesh}));
    // At least edgesAround edges round the limb, so the bone's own loop spacing doesn't leave it a few edges wide.
    MeshPtr m = outMesh(proxy);
    // Faces cut by a plane across the limb: one per edge around it.
    int ring = 0;
    for (geo::Index f = 0; f < m->faceCapacity(); ++f)
    {
        if (!m->faceValid(f))
            continue;
        double lo = 1e300, hi = -1e300;
        for (geo::Index p : m->facePoints(f))
        {
            lo = std::min(lo, m->position(p).y - 20.0);
            hi = std::max(hi, m->position(p).y - 20.0);
        }
        ring += lo < 0.0 && hi >= 0.0;
    }
    CHECK(ring >= 10);
}
