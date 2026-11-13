#include <doctest/doctest.h>

#include "TestUtil.h"

#include "curve/CurveNodes.h"
#include "curve/Tessellate.h"
#include "geo/Mesh.h"
#include "math/MathCore.h"
#include "ops/Command.h"
#include "scene/Components.h"
#include "scene/Graph.h"
#include "scene/MeshNodes.h"

#include <filesystem>

using namespace lila;
using json = nlohmann::json;
using D3 = glm::dvec3;
using namespace curvetest;

namespace
{
json localCurveJson(const std::string& name) { return run("curveData", {{"args", {name}}}); }
} // namespace

TEST_CASE("curve commands, save and load")
{
    run("newScene");
    std::string bez = run("bezierCurve", {{"points", pts({D3(0), D3(10, 5, 0), D3(20, 0, 3)})}, {"name", "Bez"}});
    std::string nurbs = run("curve", {{"points", pts({D3(0), D3(10, 0, 0), D3(10, 10, 0), D3(0, 10, 0)})}, {"degree", 2}, {"closed", true}, {"name", "Loop"}});
    std::string circ = run("circle", {{"radius", 7.0}, {"name", "Ring"}});
    std::string ep = run("curveEP", {{"points", pts({D3(0), D3(5, 3, 0), D3(9, 0, 0), D3(14, 4, 1)})}, {"name", "Ep"}});
    json before = {localCurveJson(bez), localCurveJson(nurbs), localCurveJson(circ), localCurveJson(ep)};
    CHECK(before[0][0]["type"] == "bezier");
    CHECK(before[2][0]["length"].get<double>() == doctest::Approx(2 * 3.14159265358979 * 7.0));
    auto dir = std::filesystem::temp_directory_path() / "lila_curve_tests";
    for (const char* ext : {".lila", ".lilaa"})
    {
        std::string path = (dir / (std::string("curves") + ext)).string();
        run("saveScene", {{"args", {path}}});
        run("newScene");
        CHECK(!run("objExists", {{"args", {"Bez"}}}).get<bool>());
        run("openScene", {{"args", {path}}});
        json after = {localCurveJson(bez), localCurveJson(nurbs), localCurveJson(circ), localCurveJson(ep)};
        CHECK(after == before);
    }
}

TEST_CASE("curve edits undo")
{
    run("newScene");
    std::string c = run("curve", {{"type", "poly"}, {"points", pts({D3(0), D3(10, 0, 0), D3(10, 10, 0)})}, {"name", "P"}});
    json start = localCurveJson(c);
    run("reverseCurve", {{"args", {c}}});
    CHECK(localCurveJson(c)[0]["points"][0] == json::array({10.0, 10.0, 0.0}));
    run("undo");
    CHECK(localCurveJson(c) == start);
    run("closeCurve", {{"args", {c}}});
    CHECK(localCurveJson(c)[0]["closed"] == true);
    run("undo");
    CHECK(localCurveJson(c) == start);
    // Moving a point in vertex mode goes through the move command like mesh points.
    run("selectComponents", {{"args", {c}}, {"vertices", json::array({curve::componentId(0, 1, curve::PointRole::Point)})}});
    run("moveComponents", {{"by", {0, 0, 5}}});
    CHECK(localCurveJson(c)[0]["points"][1] == json::array({10.0, 0.0, 5.0}));
    run("undo");
    CHECK(localCurveJson(c) == start);
    run("insertCurvePoint", {{"args", {c}}, {"parameter", 0.5}});
    CHECK(localCurveJson(c)[0]["points"].size() == 4);
    run("undo");
    CHECK(localCurveJson(c) == start);
}

TEST_CASE("sweep, revolve and loft stay live and bake")
{
    run("newScene");
    std::string path = run("curve", {{"type", "poly"}, {"points", pts({D3(0), D3(0, 0, 50), D3(30, 0, 80)})}, {"name", "Path"}});
    json r = run("extrude", {{"args", {path}}, {"sides", 6}, {"radius", 4.0}, {"startCap", "grid"}, {"endCap", "grid"}});
    std::string mesh = r[0];
    scene::MeshPtr m = meshOf(mesh);
    REQUIRE(m);
    CHECK(m->numFaces() == 6 * 2 + 2 * 2);
    // Editing the path updates the sweep.
    run("insertCurvePoint", {{"args", {path}}, {"parameter", 0.5}});
    CHECK(meshOf(mesh)->numFaces() == 6 * 3 + 2 * 2);
    // Moving the path object moves the sweep.
    run("move", {{"args", {path}}, {"by", {100, 0, 0}}});
    CHECK(meshOf(mesh)->bounds().min.x > 90.0);
    // Delete history bakes the mesh and drops the sweep node.
    std::string sweepNode = r[1];
    run("bakeModifierStack", {{"args", {mesh}}});
    CHECK(!run("objExists", {{"args", {sweepNode}}}).get<bool>());
    CHECK(meshOf(mesh)->numFaces() == 6 * 3 + 2 * 2);

    std::string prof = run("curve", {{"type", "poly"}, {"points", pts({D3(10, 0, 0), D3(10, 20, 0), D3(5, 30, 0)})}, {"name", "Prof"}});
    json rv = run("revolve", {{"args", {prof}}, {"sections", 12}});
    CHECK(meshOf(rv[0])->numFaces() == 24);

    std::string a = run("circle", {{"radius", 10.0}});
    std::string b = run("circle", {{"radius", 14.0}, {"center", {0, 20, 0}}});
    std::string c = run("circle", {{"radius", 8.0}, {"center", {0, 40, 0}}});
    json lo = run("loft", {{"args", {a, b, c}}, {"samples", 16}, {"spansBetween", 2}});
    CHECK(meshOf(lo[0])->numFaces() == 16 * 4);
    // Without history the mesh is stored directly.
    json noHist = run("loft", {{"args", {a, b}}, {"samples", 8}, {"constructionHistory", false}});
    CHECK(noHist[1].is_null());
    CHECK(meshOf(noHist[0])->numFaces() == 8);
}

TEST_CASE("curve to curve history")
{
    run("newScene");
    std::string c = run("curve", {{"type", "poly"}, {"points", pts({D3(0), D3(20, 0, 0), D3(20, 20, 0)})}, {"name", "L"}});
    json f = run("filletCurve", {{"args", {c}}, {"radius", 5.0}});
    CHECK(f[0] == c);
    json d = localCurveJson(c);
    CHECK(d[0]["type"] == "nurbs");
    // The original points still drive it: moving the corner moves the fillet.
    run("bakeModifierStack", {{"args", {c}}});
    CHECK(localCurveJson(c) == d);
    run("undo");
    run("undo");
    CHECK(localCurveJson(c)[0]["type"] == "poly");

    json off = run("offsetCurve", {{"args", {c}}, {"distance", 2.0}});
    std::string oc = off[0];
    CHECK(oc != c);
    CHECK(!localCurveJson(oc).empty());
    json rb = run("rebuildCurve", {{"args", {oc}}, {"spans", 5}, {"replaceOriginal", false}});
    CHECK(localCurveJson(rb[0])[0]["spans"].get<int>() >= 5);

    std::string plane = run("polyPlane", {{"width", 200}, {"depth", 200}})[0];
    std::string high = run("curve", {{"type", "poly"}, {"points", pts({D3(-20, 30, 0), D3(20, 30, 10)})}, {"name", "High"}});
    json pr = run("projectCurve", {{"args", {high}}, {"mesh", plane}, {"direction", {0, -1, 0}}});
    const json listed135 = localCurveJson(pr[0]);
    for (const json& p : listed135[0]["points"])
        CHECK(std::abs(p[1].get<double>()) < 1e-6);
    // Live: lifting the plane lifts the projection.
    run("move", {{"args", {plane}}, {"by", {0, 5, 0}}});
    const json listed139 = localCurveJson(pr[0]);
    for (const json& p : listed139[0]["points"])
        CHECK(p[1].get<double>() == doctest::Approx(5.0));

    std::string a2 = run("curve", {{"type", "poly"}, {"points", pts({D3(0), D3(5, 0, 0)})}});
    std::string b2 = run("curve", {{"type", "poly"}, {"points", pts({D3(5, 0, 0), D3(5, 5, 0)})}});
    run("attachCurve", {{"args", {a2, b2}}});
    CHECK(localCurveJson(a2)[0]["points"].size() == 3);
    CHECK(!run("objExists", {{"args", {b2}}}).get<bool>());
    json parts = run("detachCurve", {{"args", {a2}}, {"parameter", 1.0}});
    CHECK(parts.size() == 2);
}

TEST_CASE("revolve axis and sweep up default to the scene up axis")
{
    run("newScene");
    std::string prof = run("curve", {{"type", "poly"}, {"points", pts({D3(10, 0, 0), D3(10, 0, 20)})}, {"name", "Prof"}});
    json y = run("revolve", {{"args", {prof}}});
    CHECK(run("getAttr", {{"args", {y[1].get<std::string>() + ".axis"}}}) == json::array({0.0, 1.0, 0.0}));
    math::setUpAxis(math::UpAxis::Z);
    json z = run("revolve", {{"args", {prof}}});
    json sweep = run("extrude", {{"args", {prof}}});
    math::setUpAxis(math::UpAxis::Y);
    CHECK(run("getAttr", {{"args", {z[1].get<std::string>() + ".axis"}}}) == json::array({0.0, 0.0, 1.0}));
    CHECK(run("getAttr", {{"args", {sweep[1].get<std::string>() + ".upVector"}}}) == json::array({0.0, 0.0, 1.0}));
    // Around z the profile, parallel to it, sweeps a tube 20 tall.
    CHECK(meshOf(z[0])->bounds().size().z == doctest::Approx(20.0));
    CHECK(meshOf(z[0])->bounds().size().x == doctest::Approx(20.0).epsilon(0.01));
}
