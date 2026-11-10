#include <doctest/doctest.h>

#include "TestUtil.h"

#include "geo/Mesh.h"
#include "geo/Primitives.h"
#include "core/PrefStore.h"
#include "ops/Command.h"
#include "scene/Components.h"
#include "scene/Graph.h"
#include "scene/MeshNodes.h"
#include "scene/OrientPivot.h"
#include "scene/Rig.h"
#include "scene/Transform.h"
#include "scene/TransformOps.h"
#include "ui/Manipulator.h"
#include "ui/Tools.h"
#include "ui/ViewInfo.h"

#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>

#include <cmath>
#include <vector>

using namespace lila;
using json = nlohmann::json;
using scene::ComponentMode;
using namespace testutil;
namespace op = scene::orientpivot;

namespace
{
scene::Node& node(const std::string& name) { return *scene::Graph::current().find(name); }

void checkRotation(const glm::dmat3& f)
{
    for (int i = 0; i < 3; ++i)
        CHECK(glm::length(f[i]) == doctest::Approx(1.0));
    CHECK(std::abs(glm::dot(f[0], f[1])) < 1e-9);
    CHECK(glm::determinant(f) == doctest::Approx(1.0));
}

std::vector<op::Pick> picks(const std::vector<geo::Index>& ids)
{
    std::vector<op::Pick> out;
    for (size_t i = 0; i < ids.size(); ++i)
        out.push_back({ids[i], i});
    return out;
}

/** Edges of an open cylinder whose two ends both sit at height y. */
std::vector<geo::Index> ringEdges(const geo::Mesh& m, double y)
{
    std::vector<geo::Index> out;
    for (geo::Index e = 0; e < m.edgeCapacity(); ++e)
        if (m.edgeValid(e) && std::abs(m.position(m.edgePoint(e, 0)).y - y) < 1e-6 && std::abs(m.position(m.edgePoint(e, 1)).y - y) < 1e-6)
            out.push_back(e);
    return out;
}
} // namespace

TEST_CASE("orientPivot: aim frames are right handed with the aim and up on the chosen axes")
{
    glm::dmat3 f = op::aimFrame({1, 0, 0}, {{0, 1, 0}}, op::parseAxis("x"), op::parseAxis("y"));
    CHECK(near(f[0], {1, 0, 0}));
    CHECK(near(f[1], {0, 1, 0}));
    CHECK(near(f[2], {0, 0, 1}));

    // Up only leans toward the reference: it is made square to the aim.
    f = op::aimFrame({0, 0, 2}, {{0, 1, 1}}, op::parseAxis("-y"), op::parseAxis("z"));
    checkRotation(f);
    CHECK(near(f[1], {0, 0, -1}));
    CHECK(near(f[2], {0, 1, 0}));

    for (const char* aim : {"x", "-x", "y", "-y", "z", "-z"})
        for (const char* up : {"x", "-x", "y", "-y", "z", "-z"})
        {
            if (op::parseAxis(aim).index == op::parseAxis(up).index)
            {
                CHECK_THROWS_AS(op::aimFrame({1, 2, 3}, {{0, 1, 0}}, op::parseAxis(aim), op::parseAxis(up)), std::invalid_argument);
                continue;
            }
            glm::dmat3 r = op::aimFrame({1, 2, 3}, {{0, 1, 0}}, op::parseAxis(aim), op::parseAxis(up));
            checkRotation(r);
            op::Axis a = op::parseAxis(aim), u = op::parseAxis(up);
            CHECK(near(r[a.index] * a.sign, glm::normalize(glm::dvec3(1, 2, 3))));
            CHECK(glm::dot(r[u.index] * u.sign, glm::dvec3(0, 1, 0)) > 0.0);
        }

    // An up along the aim is skipped for the next candidate, and with none left the least aligned world axis is used.
    f = op::aimFrame({0, 1, 0}, {{0, 2, 0}, {0, 0, 1}}, op::parseAxis("x"), op::parseAxis("y"));
    CHECK(near(f[1], {0, 0, 1}));
    f = op::aimFrame({0, 1, 0}, {{0, 2, 0}}, op::parseAxis("x"), op::parseAxis("y"));
    checkRotation(f);
    CHECK(near(f[0], {0, 1, 0}));
    CHECK_THROWS_AS(op::parseAxis("w"), std::invalid_argument);
}

TEST_CASE("orientPivot: single pieces aim along themselves")
{
    // A 10 x 2 rectangle facing +Y: the face aims along its normal with up along its long side.
    geo::Mesh m;
    m.addPoint({0, 0, 0});
    m.addPoint({0, 0, 2});
    m.addPoint({10, 0, 2});
    m.addPoint({10, 0, 0});
    geo::Index f = m.addFace({0, 1, 2, 3});
    REQUIRE(f != geo::kInvalid);
    std::vector<op::Piece> ps = op::pieces(m, ComponentMode::Face, picks({f}), false);
    REQUIRE(ps.size() == 1);
    op::Aim a = op::solve(ps);
    CHECK(near(a.aim, {0, 1, 0}));
    CHECK(near(ps[0].centre, {5, 0, 1}));
    CHECK(std::abs(glm::dot(a.ups.front(), glm::dvec3(1, 0, 0))) == doctest::Approx(1.0));

    // An edge aims from its first point to its second, up from the face beside it.
    geo::Index e = m.findEdge(0, 3);
    ps = op::pieces(m, ComponentMode::Edge, picks({e}), false);
    REQUIRE(ps.size() == 1);
    a = op::solve(ps);
    glm::dvec3 along = glm::normalize(m.position(m.edgePoint(e, 1)) - m.position(m.edgePoint(e, 0)));
    CHECK(near(a.aim, along));
    CHECK(near(a.ups.front(), {0, 1, 0}));

    // A vertex aims along its normal.
    ps = op::pieces(m, ComponentMode::Vertex, picks({2}), false);
    REQUIRE(ps.size() == 1);
    CHECK(near(op::solve(ps).aim, {0, 1, 0}));

    // An open chain of edges runs from the end nearer the first pick to the other end.
    ps = op::pieces(m, ComponentMode::Edge, picks({m.findEdge(2, 3), m.findEdge(1, 2), m.findEdge(0, 1)}), false);
    REQUIRE(ps.size() == 1);
    a = op::solve(ps);
    CHECK(near(a.aim, glm::normalize(m.position(0) - m.position(3))));
}

TEST_CASE("orientPivot: two picks aim first to second; connected picks group into pieces")
{
    geo::Mesh m = geo::makePlane({.width = 4.0, .depth = 4.0, .subdivX = 4, .subdivZ = 4});
    auto pointAt = [&](double x, double z) {
        for (geo::Index p = 0; p < m.pointCapacity(); ++p)
            if (m.pointValid(p) && near(m.position(p), {x, 0, z}))
                return p;
        return geo::kInvalid;
    };
    // Two neighbouring verts would be one piece, but two picks are always two.
    geo::Index a = pointAt(1, 0), b = pointAt(0, 0);
    std::vector<op::Piece> ps = op::pieces(m, ComponentMode::Vertex, picks({a, b}), true);
    REQUIRE(ps.size() == 2);
    CHECK(near(op::solve(ps).aim, {-1, 0, 0}));
    CHECK(op::pieces(m, ComponentMode::Vertex, picks({a, b}), false).size() == 1);

    // Two separate face pairs: the aim runs between their centres in pick order.
    auto faceAt = [&](double x, double z) {
        for (geo::Index f = 0; f < m.faceCapacity(); ++f)
            if (m.faceValid(f) && near(m.faceCentroid(f), {x, 0, z}))
                return f;
        return geo::kInvalid;
    };
    ps = op::pieces(m, ComponentMode::Face, picks({faceAt(1.5, 1.5), faceAt(-1.5, -1.5), faceAt(1.5, 0.5), faceAt(-1.5, -0.5)}), false);
    REQUIRE(ps.size() == 2);
    CHECK(near(ps[0].centre, {1.5, 0, 1}));
    CHECK(near(ps[1].centre, {-1.5, 0, -1}));
    CHECK(near(op::solve(ps).aim, glm::normalize(glm::dvec3(-3, 0, -2))));
}

TEST_CASE("orientPivot: an open cylinder's two edge loops aim along its axis; one loop through its plane")
{
    geo::Mesh m = geo::makeCylinder({.radius = 50.0, .height = 100.0, .subdivAxis = 12, .subdivHeight = 1, .caps = false});
    std::vector<geo::Index> top = ringEdges(m, 50.0), bottom = ringEdges(m, -50.0);
    REQUIRE(top.size() == 12);
    REQUIRE(bottom.size() == 12);
    std::vector<geo::Index> both = bottom;
    both.insert(both.end(), top.begin(), top.end());
    std::vector<op::Piece> ps = op::pieces(m, ComponentMode::Edge, picks(both), false);
    REQUIRE(ps.size() == 2);
    CHECK(near(ps[0].centre, {0, -50, 0}));
    CHECK(near(ps[1].centre, {0, 50, 0}));
    op::Aim a = op::solve(ps);
    CHECK(near(a.aim, {0, 1, 0}));
    glm::dmat3 f = op::aimFrame(a.aim, a.ups, op::parseAxis("y"), op::parseAxis("x"));
    checkRotation(f);
    CHECK(near(f[1], {0, 1, 0}));

    // Picking the top loop first turns the aim around.
    std::vector<geo::Index> reversed = top;
    reversed.insert(reversed.end(), bottom.begin(), bottom.end());
    CHECK(near(op::solve(op::pieces(m, ComponentMode::Edge, picks(reversed), false)).aim, {0, -1, 0}));

    // A tube's ring has no surface side, so it faces its largest positive axis, with up toward its first point.
    ps = op::pieces(m, ComponentMode::Edge, picks(top), false);
    REQUIRE(ps.size() == 1);
    a = op::solve(ps);
    CHECK(near(a.aim, {0, 1, 0}));
    glm::dvec3 start = m.position(m.edgePoint(top.front(), 0));
    CHECK(near(a.ups.front(), glm::normalize(start - glm::dvec3(0, 50, 0))));
}

TEST_CASE("orientPivot command: pick order, pivot mode, undo")
{
    run("newScene");
    std::string plane = run("polyPlane", {{"width", 4}, {"depth", 4}, {"subdivisionsX", 4}, {"subdivisionsZ", 4}})[0];
    run("move", {{"args", {plane}}, {"by", {10, 0, 0}}});
    geo::Mesh m = meshOf(plane);
    auto pointAt = [&](double x, double z) {
        for (geo::Index p = 0; p < m.pointCapacity(); ++p)
            if (m.pointValid(p) && near(m.position(p), {x, 0, z}))
                return p;
        return geo::kInvalid;
    };
    geo::Index a = pointAt(2, 2), b = pointAt(-2, -2);
    REQUIRE(a < b);
    // b first: the selection is stored sorted, the pick order is kept beside it.
    run("selectComponents", {{"args", {plane}}, {"vertices", {b}}});
    run("selectComponents", {{"args", {plane}}, {"vertices", {a}}, {"add", true}});
    auto order = scene::components::ordered(ComponentMode::Vertex);
    REQUIRE(order.size() == 2);
    CHECK(order[0].second == b);
    CHECK(order[1].second == a);

    const std::vector<glm::dvec3> before = worldPoints(plane);
    const glm::dmat4 worldBefore = worldOf(plane);
    json r = run("orientPivot", {{"upReference", "world"}, {"mode", "pivot"}});
    scene::Graph& g = scene::Graph::current();
    scene::NodeId id = node(plane).id();
    glm::dmat3 pf = scene::xformops::pivotFrame(g, id);
    CHECK(near(pf[0], glm::normalize(glm::dvec3(1, 0, 1))));
    CHECK(near(pf[1], {0, 1, 0}));
    CHECK(near(scene::xformops::worldPivot(g, id), {8, 0, -2}));
    CHECK(near(vec(r["x"]), pf[0]));
    // Pivot mode moves nothing.
    const glm::dmat4 worldAfter = worldOf(plane);
    for (int c = 0; c < 4; ++c)
        CHECK(near(glm::dvec3(worldAfter[c]), glm::dvec3(worldBefore[c])));
    CHECK(worldPoints(plane) == before);

    run("orientPivot", {{"aim", "z"}, {"up", "y"}, {"position", "center"}, {"upReference", "world"}, {"mode", "pivot"}});
    pf = scene::xformops::pivotFrame(g, id);
    CHECK(near(pf[2], glm::normalize(glm::dvec3(1, 0, 1))));
    CHECK(near(scene::xformops::worldPivot(g, id), {10, 0, 0}));

    run("undo");
    CHECK(near(scene::xformops::pivotFrame(g, id)[0], glm::normalize(glm::dvec3(1, 0, 1))));
    run("undo");
    CHECK(near(node(plane).as<glm::dvec3>(scene::Transform::kPivotOrient), {0, 0, 0}));

    CHECK_THROWS_AS(run("orientPivot", {{"aim", "y"}, {"up", "y"}}), CommandError);
    run("selectComponents", {{"args", {plane}}, {"vertices", json::array()}});
    CHECK_THROWS_AS(run("orientPivot"), CommandError);
}

TEST_CASE("orientPivot command: transform mode puts back the rotation a freeze removed")
{
    run("newScene");
    std::string cyl = run("polyCylinder", {{"radius", 50.0}, {"height", 100.0}, {"caps", false}, {"subdivisionsAxis", 12}, {"subdivisionsHeight", 2}})[0];
    geo::Mesh local = meshOf(cyl);
    // The first picked point decides up, so remember the one on +X at the bottom.
    geo::Index startPoint = geo::kInvalid;
    std::vector<geo::Index> bottom, top;
    for (geo::Index p = 0; p < local.pointCapacity(); ++p)
    {
        if (!local.pointValid(p))
            continue;
        const glm::dvec3 pos = local.position(p);
        if (near(pos, {50, -50, 0}))
            startPoint = p;
        else if (std::abs(pos.y + 50.0) < 1e-6)
            bottom.push_back(p);
        else if (std::abs(pos.y - 50.0) < 1e-6)
            top.push_back(p);
    }
    REQUIRE(startPoint != geo::kInvalid);
    REQUIRE(bottom.size() == 11);
    REQUIRE(top.size() == 12);

    run("rotate", {{"args", {cyl}}, {"by", {30, 45, 60}}});
    run("move", {{"args", {cyl}}, {"by", {5, 6, 7}}});
    const glm::dmat4 placed = worldOf(cyl);
    run("freezeTransforms", {{"args", {cyl}}});
    CHECK(near(node(cyl).as<glm::dvec3>(scene::Transform::kRotate), {0, 0, 0}));
    const std::vector<glm::dvec3> before = worldPoints(cyl);

    json firstPick = json::array({startPoint});
    json restBottom = json::array(), topIds = json::array();
    for (geo::Index p : bottom)
        restBottom.push_back(p);
    for (geo::Index p : top)
        topIds.push_back(p);
    run("selectComponents", {{"args", {cyl}}, {"vertices", firstPick}});
    run("selectComponents", {{"args", {cyl}}, {"vertices", restBottom}, {"add", true}});
    run("selectComponents", {{"args", {cyl}}, {"vertices", topIds}, {"add", true}});
    run("orientPivot", {{"aim", "y"}, {"up", "x"}, {"position", "center"}, {"mode", "transform"}});

    // The object is back where it was before the freeze, and the mesh has not moved in the world.
    const glm::dmat4 recovered = worldOf(cyl);
    for (int c = 0; c < 4; ++c)
        CHECK(near(glm::dvec3(recovered[c]), glm::dvec3(placed[c]), 1e-6));
    const glm::dmat3 channels(scene::eulerToMatrix(node(cyl).as<glm::dvec3>(scene::Transform::kRotate), scene::RotateOrder::XYZ));
    for (int c = 0; c < 3; ++c)
        CHECK(near(channels[c], glm::dvec3(placed[c]), 1e-6));
    const std::vector<glm::dvec3> after = worldPoints(cyl);
    REQUIRE(after.size() == before.size());
    for (size_t i = 0; i < after.size(); ++i)
        CHECK(near(after[i], before[i], 1e-6));
    geo::Mesh unfrozen = meshOf(cyl);
    CHECK(near(unfrozen.position(startPoint), {50, -50, 0}, 1e-6));

    run("undo");
    CHECK(near(node(cyl).as<glm::dvec3>(scene::Transform::kRotate), {0, 0, 0}));
    CHECK(near(meshOf(cyl).position(startPoint), glm::dvec3(placed * glm::dvec4(local.position(startPoint), 1.0)), 1e-6));
}

TEST_CASE("pivot-edit rotate on a joint turns it once, keeps its child, and undoes")
{
    using scene::Joint;
    run("newScene");
    run("joint", {{"name", "root"}, {"position", {0, -20, 0}}, {"parent", ""}});
    const std::string mid = run("joint", {{"name", "mid"}, {"position", {0, 0, 0}}, {"parent", "root"}}).get<std::string>();
    run("joint", {{"name", "tip"}, {"position", {20, 0, 0}}, {"parent", mid}});
    const glm::dvec3 tipAt = glm::dvec3(worldOf("tip")[3]);

    ui::tools::setCurrent(ui::Tool::Rotate);
    ui::tools::setPivotEdit(true);
    // Looking down -Z, 1 px per cm, the world origin at pixel (50, 50).
    ui::ViewInfo view;
    view.proj = glm::ortho(-50.0, 50.0, -50.0, 50.0, -100.0, 100.0);
    view.width = view.height = 100.0;
    view.ortho = true;
    view.eye = {0.0, 0.0, 50.0};

    // The same drag in one step and in many lands on the same jointOrient.
    auto dragRing = [&](int steps) {
        run("select", {{"args", {mid}}});
        ui::Manipulator m;
        m.setView(view);
        // The first ring handle out along the diagonal.
        double r = 6.0;
        while (r < 49.0 && !(m.hover({50 + r, 50 + r}), m.hovering()))
            r += 0.5;
        REQUIRE(r < 49.0);
        REQUIRE(m.press({50 + r, 50 + r}, Qt::LeftButton, Qt::NoModifier));
        for (int i = 1; i <= steps; ++i)
            m.drag({50 + r - 12.0 * i / steps, 50 + r + 12.0 * i / steps}, Qt::NoModifier);
        m.release();
        return node(mid).as<glm::dvec3>(Joint::kJointOrient);
    };
    const glm::dvec3 once = dragRing(1);
    CHECK(glm::length(once) > 1.0);
    CHECK(near(glm::dvec3(worldOf("tip")[3]), tipAt));
    CHECK(near(node(mid).as<glm::dvec3>(scene::Transform::kRotate), {0, 0, 0}));
    run("undo");
    CHECK(near(node(mid).as<glm::dvec3>(Joint::kJointOrient), {0, 0, 0}));

    const glm::dvec3 many = dragRing(8);
    CHECK(near(many, once, 1e-6));
    run("undo");
    CHECK(near(node(mid).as<glm::dvec3>(Joint::kJointOrient), {0, 0, 0}));
    CHECK(near(glm::dvec3(worldOf("tip")[3]), tipAt));
    ui::tools::setPivotEdit(false);
    ui::tools::setCurrent(ui::Tool::Move);
}

TEST_CASE("gimbal rotate: a ring drag turns one rotate channel, and rings coincide at gimbal lock")
{
    run("newScene");
    run("spaceLocator", {{"name", "A"}});
    run("rotate", {{"args", {"A"}}, {"to", {20, 30, 0}}});
    PrefStore::get().set("general", "/manipulator/rotateSpace", "gimbal");
    ui::tools::setCurrent(ui::Tool::Rotate);
    ui::ViewInfo view;
    view.proj = glm::ortho(-50.0, 50.0, -50.0, 50.0, -100.0, 100.0);
    view.width = view.height = 100.0;
    view.ortho = true;
    view.eye = {0.0, 0.0, 50.0};

    run("select", {{"args", {"A"}}});
    ui::Manipulator m;
    m.setView(view);
    double r = 6.0;
    while (r < 49.0 && !(m.hover({50 + r, 50 + r}), m.hovering()))
        r += 0.5;
    REQUIRE(r < 49.0);
    REQUIRE(m.press({50 + r, 50 + r}, Qt::LeftButton, Qt::NoModifier));
    m.drag({50 + r - 12.0, 50 + r + 12.0}, Qt::NoModifier);
    m.release();
    const glm::dvec3 after = node("A").as<glm::dvec3>(scene::Transform::kRotate);
    int changed = 0;
    for (int c = 0; c < 3; ++c)
        changed += std::abs(after[c] - glm::dvec3(20, 30, 0)[c]) > 1e-6;
    CHECK(changed == 1);
    run("undo");
    CHECK(near(node("A").as<glm::dvec3>(scene::Transform::kRotate), {20, 30, 0}));

    PrefStore::get().set("general", "/manipulator/rotateSpace", "world");
    ui::tools::setCurrent(ui::Tool::Move);
}
