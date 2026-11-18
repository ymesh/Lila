#include <doctest/doctest.h>

#include "ops/Command.h"
#include "ops/Undo.h"
#include "core/MainQueue.h"
#include "core/PrefStore.h"
#include "geo/Primitives.h"
#include "retopo/RetopoCommands.h"
#include "retopo/TopologyCommands.h"
#include "scene/Graph.h"
#include "scene/MeshNodes.h"
#include "scene/Node.h"
#include "scene/SceneCommands.h"
#include "ui/RetopoDrawTool.h"
#include "ui/TopologyTool.h"
#include "ui/TopologyToolkit.h"
#include "ui/ViewportTool.h"

#include <QKeyEvent>
#include <QListWidget>
#include <QMouseEvent>
#include <QWheelEvent>

#include <glm/gtc/matrix_transform.hpp>

#include <cmath>

using namespace lila;
using json = nlohmann::json;

namespace
{
void setup()
{
    static bool done = false;
    if (!done)
    {
        mainq::init();
        ops::registerBuiltins();
        scene::types::registerBuiltins();
        scene::registerCommands();
        retopo::registerCommands();
        done = true;
    }
    // Pencil strokes land exactly where the synthetic drags put them unless a test turns these on.
    PrefStore::get().set("general", "/topology/pencil/lazy", 0.0);
    PrefStore::get().set("general", "/topology/pencil/smoothing", 0.0);
    ui::viewportTools::set(nullptr);
    scene::Graph::current().clear();
    UndoStack::get().clear();
    PrefStore::get().set("general", "/topology/edgeLength", 10.0);
    PrefStore::get().set("general", "/topology/brush", 5.0);
}

scene::Node* meshNode(const std::string& name, geo::Mesh mesh)
{
    scene::Graph& g = scene::Graph::current();
    scene::Node* n = g.createNode(*scene::types::find("mesh"), name, scene::kNoNode, false);
    g.setValue({n->id(), scene::MeshNode::kInMesh, -1}, scene::MeshPtr(std::make_shared<geo::Mesh>(std::move(mesh))));
    return n;
}

/** Looking down -Y at the XZ plane, 1 px per cm, world origin at pixel (100, 100), +X right and +Z down the screen. */
ui::ViewInfo topView()
{
    ui::ViewInfo v;
    v.eye = {0.0, 200.0, 0.0};
    v.view = glm::lookAt(v.eye, glm::dvec3(0.0), glm::dvec3(0.0, 0.0, -1.0));
    v.proj = glm::ortho(-100.0, 100.0, -100.0, 100.0, 1.0, 400.0);
    v.width = v.height = 200.0;
    v.ortho = true;
    v.camera = "top";
    return v;
}

/** Looking down -Z, 1 px per cm, world origin at pixel (100, 100), +Y up the screen. */
ui::ViewInfo frontView()
{
    ui::ViewInfo v;
    v.eye = {0.0, 0.0, 200.0};
    v.view = glm::lookAt(v.eye, glm::dvec3(0.0), glm::dvec3(0.0, 1.0, 0.0));
    v.proj = glm::ortho(-100.0, 100.0, -100.0, 100.0, 1.0, 400.0);
    v.width = v.height = 200.0;
    v.ortho = true;
    v.camera = "front";
    return v;
}

QPointF px(double x, double z) { return QPointF(100.0 + x, 100.0 + z); }

ui::TopologyTool* tool() { return ui::activeTopologyTool(); }

void begin(ui::TopoMode mode)
{
    scene::Node* ref = scene::Graph::current().find("ref");
    REQUIRE(ref);
    ui::viewportTools::set(std::make_unique<ui::TopologyTool>(ref->id(), "topo", mode));
    REQUIRE(tool());
}

/** A left drag through the points, optionally with modifiers, as the viewport delivers it. */
void drag(const std::vector<QPointF>& pts, const ui::ViewInfo& view, Qt::KeyboardModifiers mods = Qt::NoModifier)
{
    REQUIRE(pts.size() >= 1);
    QMouseEvent press(QEvent::MouseButtonPress, pts.front(), pts.front(), Qt::LeftButton, Qt::LeftButton, mods);
    tool()->mousePress(&press, view);
    for (size_t i = 1; i < pts.size(); ++i)
    {
        QMouseEvent move(QEvent::MouseMove, pts[i], pts[i], Qt::NoButton, Qt::LeftButton, mods);
        tool()->mouseMove(&move, view);
    }
    QMouseEvent release(QEvent::MouseButtonRelease, pts.back(), pts.back(), Qt::LeftButton, Qt::NoButton, mods);
    tool()->mouseRelease(&release, view);
}

/** Points every 2 px along straight segments through the corners. */
std::vector<QPointF> path(const std::vector<QPointF>& corners)
{
    std::vector<QPointF> out{corners.front()};
    for (size_t i = 1; i < corners.size(); ++i)
    {
        QPointF a = corners[i - 1], b = corners[i];
        int steps = std::max(1, int(std::hypot(b.x() - a.x(), b.y() - a.y()) / 2.0));
        for (int k = 1; k <= steps; ++k)
            out.push_back(a + (b - a) * (double(k) / steps));
    }
    return out;
}

void toolWheel(QPointF at, int notches, const ui::ViewInfo& view, Qt::KeyboardModifiers mods = Qt::NoModifier)
{
    QWheelEvent e(at, at, QPoint(), QPoint(0, 120 * notches), Qt::NoButton, mods, Qt::NoScrollPhase, false);
    tool()->wheel(&e, view);
}

void toolKey(int k)
{
    QKeyEvent e(QEvent::KeyPress, k, Qt::NoModifier);
    tool()->keyPress(&e, topView());
}

const geo::Mesh* topoMesh()
{
    scene::Graph& g = scene::Graph::current();
    g.evaluate();
    scene::Node* n = g.find("topo");
    return n ? n->as<scene::MeshPtr>(scene::MeshNode::kOutMesh).get() : nullptr;
}

std::vector<retopo::ConstructRecord> records()
{
    scene::Node* n = scene::Graph::current().find("topo");
    return n ? retopo::retopoConstructs(scene::Graph::current(), *n) : std::vector<retopo::ConstructRecord>{};
}

geo::Mesh plane()
{
    geo::PlaneOptions o;
    o.width = 160.0;
    o.depth = 160.0;
    o.subdivX = 40;
    o.subdivZ = 40;
    return geo::makePlane(o);
}

bool allQuads(const geo::Mesh& m)
{
    for (geo::Index f = 0; f < m.faceCapacity(); ++f)
        if (m.faceValid(f) && m.faceSize(f) != 4)
            return false;
    return true;
}
} // namespace

TEST_CASE("topology tool: patch, wheel and pull on a closed shape")
{
    setup();
    meshNode("ref", plane());
    begin(ui::TopoMode::Patch);
    ui::ViewInfo view = topView();
    drag(path({px(-40, -30), px(40, -30), px(40, 30), px(-40, 30), px(-40, -28)}), view);
    const geo::Mesh* m = topoMesh();
    REQUIRE(m);
    CHECK(m->numFaces() > 4);
    CHECK(allQuads(*m));
    std::vector<retopo::ConstructRecord> recs = records();
    REQUIRE(recs.size() == 1);
    CHECK(recs[0].kind == retopo::ConstructKind::Patch);
    CHECK(tool()->activeConstruct() == recs[0].id);
    size_t faces = m->numFaces();

    toolWheel(px(0, -29), 1, view, Qt::ControlModifier);
    CHECK(topoMesh()->numFaces() != faces);
    CHECK(records().size() == 1);
    CHECK(records()[0].live);

    drag(path({px(0, 0), px(20, 10)}), view);
    CHECK(records()[0].spec.contains("pull"));
    CHECK(allQuads(*topoMesh()));

    toolKey(Qt::Key_Return);
    CHECK(records().empty());
    CHECK(topoMesh()->numFaces() > 4);
    ui::viewportTools::set(nullptr);
}

TEST_CASE("topology tool: strips stay editable, snap to neighbours, gaps fill, cap closes a border")
{
    setup();
    meshNode("ref", plane());
    ui::ViewInfo view = topView();
    begin(ui::TopoMode::Strip);
    drag(path({px(-60, -60), px(0, -60)}), view);
    drag(path({px(-60, -30), px(0, -30)}), view);
    // Adding one strip leaves the other live, so its knots still show and drag.
    REQUIRE(records().size() == 2);
    CHECK(records()[0].live);
    CHECK(records()[1].live);

    // Knots dragged next to the other strip: its border joins the other's.
    const size_t pointsBefore = topoMesh()->numPoints();
    for (double x : {-60.0, -40.0, -20.0, 0.0})
        drag({px(x, -30), px(x, -49)}, view);
    CHECK(topoMesh()->numPoints() < pointsBefore);

    // Shift-click in the gap between two retopo vertices pairs fills it.
    ui::viewportTools::set(nullptr);
    setup();
    meshNode("ref", plane());
    begin(ui::TopoMode::Strip);
    drag(path({px(-40, -60), px(-20, -60)}), view);
    drag(path({px(0, -60), px(20, -60)}), view);
    const size_t faces = topoMesh()->numFaces();
    drag({px(-10, -60)}, view, Qt::ShiftModifier);
    CHECK(topoMesh()->numFaces() == faces + 1);

    // Cap fills a closed border.
    ui::viewportTools::set(nullptr);
    setup();
    meshNode("ref", plane());
    begin(ui::TopoMode::Strip);
    drag(path({px(-40, -40), px(40, -40), px(40, 40), px(-40, 40), px(-40, -38)}), view);
    const size_t ring = topoMesh()->numFaces();
    tool()->setMode(ui::TopoMode::Cap);
    drag({px(-35, 0)}, view);
    CHECK(topoMesh()->numFaces() > ring);
    ui::viewportTools::set(nullptr);
}

TEST_CASE("topology tool: strips stay mirrored, the wheel over a strip changes its loops")
{
    setup();
    meshNode("ref", plane());
    ui::ViewInfo view = topView();
    begin(ui::TopoMode::Pencil);
    drag(path({px(60, 60), px(70, 70)}), view);
    ops::execute("setSymmetry", {{"args", {"topo"}}, {"axis", "x"}});
    tool()->setMode(ui::TopoMode::Strip);
    auto report = [&](const char* when) {
        const geo::Mesh* m = topoMesh();
        int unpaired = 0, onPlane = 0, borderOnPlane = 0;
        for (geo::Index p = 0; p < m->pointCapacity(); ++p)
        {
            if (!m->pointValid(p)) continue;
            glm::dvec3 q = m->position(p);
            if (std::abs(q.x) < 1e-6) { ++onPlane; borderOnPlane += m->isBoundaryPoint(p); continue; }
            bool found = false;
            for (geo::Index r = 0; r < m->pointCapacity() && !found; ++r)
                found = m->pointValid(r) && glm::length(m->position(r) - glm::dvec3(-q.x, q.y, q.z)) < 1e-4;
            unpaired += !found;
        }
        INFO(when << ": on plane " << onPlane << ", border on plane " << borderOnPlane);
        CHECK(unpaired == 0);
        CHECK(records().back().live);
    };
    drag(path({px(-30, 0), px(50, 0)}), view);
    report("across");
    // Right over a knot: Ctrl-wheel still means loops; Shift-wheel there is the knot's width.
    const nlohmann::json spec = records().back().spec;
    const std::vector<int> counts = records().back().counts;
    const nlohmann::json knot = spec["knots"][1];
    const QPointF knotPx = px(knot[0].get<double>(), -knot[2].get<double>());
    {
        // The plain wheel is the camera's, never the tool's.
        QWheelEvent plain(knotPx, knotPx, QPoint(), QPoint(0, 120), Qt::NoButton, Qt::NoModifier, Qt::NoScrollPhase, false);
        CHECK_FALSE(tool()->wheel(&plain, view));
    }
    toolWheel(knotPx, 1, view, Qt::ControlModifier);
    CHECK(records().back().counts.at(0) == counts.at(0) + 1);
    CHECK_FALSE(records().back().spec.contains("scales"));
    toolWheel(knotPx, 1, view, Qt::ShiftModifier);
    CHECK(records().back().spec.contains("scales"));
    report("wheel");
    toolWheel(px(20, 0), 1, view, Qt::ControlModifier | Qt::AltModifier);
    report("ctrl wheel");
    drag(path({px(-60, 40), px(-2, 40)}), view);
    report("ends near middle");
    drag(path({px(4, -40), px(4, 40)}), view);
    report("along middle");
    ui::viewportTools::set(nullptr);
}

TEST_CASE("topology tool: a cap follows the end it closes")
{
    setup();
    geo::CylinderOptions o;
    o.radius = 20.0;
    o.height = 120.0;
    o.subdivAxis = 48;
    o.subdivHeight = 12;
    o.caps = true;
    meshNode("ref", geo::makeCylinder(o));
    begin(ui::TopoMode::Cylinder);
    ui::ViewInfo view = frontView();
    // Two rings below the top end (y = 60), screen y running with world y here: a tube whose open top the cap closes.
    drag({QPointF(60, 140), QPointF(140, 140)}, view);
    drag({QPointF(60, 70), QPointF(140, 70)}, view);
    REQUIRE(topoMesh());
    double ringTop = -1e9;
    for (geo::Index p = 0; p < topoMesh()->pointCapacity(); ++p)
        if (topoMesh()->pointValid(p))
            ringTop = std::max(ringTop, topoMesh()->position(p).y);
    const size_t faces = topoMesh()->numFaces();
    tool()->setMode(ui::TopoMode::Cap);
    drag({QPointF(100, 100.0 + ringTop)}, view);
    REQUIRE(topoMesh()->numFaces() > faces);
    double top = -1e9;
    for (geo::Index p = 0; p < topoMesh()->pointCapacity(); ++p)
        if (topoMesh()->pointValid(p))
            top = std::max(top, topoMesh()->position(p).y);
    // Carried out along the tube onto its end, not left flat across the ring.
    CHECK(top == doctest::Approx(60.0).epsilon(0.02));
    ui::viewportTools::set(nullptr);
}

TEST_CASE("topology tool: strip, strokes, bridge and sweep")
{
    setup();
    meshNode("ref", plane());
    ui::ViewInfo view = topView();

    begin(ui::TopoMode::Strip);
    drag(path({px(-60, -60), px(0, -60), px(0, 0)}), view);
    std::vector<retopo::ConstructRecord> recs = records();
    REQUIRE(recs.size() == 1);
    CHECK(recs[0].kind == retopo::ConstructKind::Strip);
    REQUIRE(topoMesh());
    CHECK(topoMesh()->numFaces() > 3);
    CHECK(allQuads(*topoMesh()));

    tool()->setMode(ui::TopoMode::Stroke);
    drag(path({px(20, 20), px(70, 20)}), view);
    recs = records();
    REQUIRE(recs.size() == 2);
    CHECK(recs[1].kind == retopo::ConstructKind::Stroke);
    size_t before = topoMesh()->numFaces();
    drag(path({px(20, 50), px(70, 50)}), view);
    recs = records();
    REQUIRE(recs.size() == 2);
    CHECK(recs[1].kind == retopo::ConstructKind::Bridge);
    CHECK(topoMesh()->numFaces() > before);

    // From the bridge's far corner, outward: a sweep of the border beside it.
    before = topoMesh()->numFaces();
    drag(path({px(70, 50), px(70, 75)}), view);
    recs = records();
    REQUIRE(recs.size() == 3);
    CHECK(recs[2].kind == retopo::ConstructKind::Sweep);
    CHECK(topoMesh()->numFaces() > before);
    for (geo::Index e = 0; e < topoMesh()->edgeCapacity(); ++e)
        if (topoMesh()->edgeValid(e))
            CHECK(topoMesh()->edgeFaceCount(e) <= 2);
    ui::viewportTools::set(nullptr);
}

TEST_CASE("topology tool: cylinder rings, pencil guides, auto region and handles")
{
    setup();
    SUBCASE("cylinder")
    {
        geo::CylinderOptions o;
        o.radius = 20.0;
        o.height = 120.0;
        o.subdivAxis = 48;
        o.subdivHeight = 12;
        o.caps = false;
        meshNode("ref", geo::makeCylinder(o));
        begin(ui::TopoMode::Cylinder);
        ui::ViewInfo view = frontView();
        // Lines across the upright tube at two heights (screen y down is world -Y).
        drag({QPointF(60, 140), QPointF(140, 140)}, view);
        std::vector<retopo::ConstructRecord> recs = records();
        REQUIRE(recs.size() == 1);
        CHECK(recs[0].kind == retopo::ConstructKind::Cylinder);
        drag({QPointF(60, 60), QPointF(140, 60)}, view);
        REQUIRE(topoMesh());
        CHECK(topoMesh()->numFaces() >= 16);
        CHECK(allQuads(*topoMesh()));
        size_t faces = topoMesh()->numFaces();
        // Across the middle of the cylinder: a ring inserted, not a new cylinder.
        drag({QPointF(60, 100), QPointF(140, 100)}, view);
        CHECK(records().size() == 1);
        CHECK(records()[0].spec["rings"].size() == 3);
        toolWheel(QPointF(100, 80), 1, view, Qt::ControlModifier);
        CHECK(records()[0].spec["points"].get<int>() == 9);
        CHECK(topoMesh()->numFaces() != faces);
    }
    SUBCASE("pencil")
    {
        meshNode("ref", plane());
        begin(ui::TopoMode::Pencil);
        ui::ViewInfo view = topView();
        drag(path({px(-50, 0), px(50, 0)}), view);
        drag(path({px(0, -50), px(0, 50)}), view);
        scene::Node* topo = scene::Graph::current().find("topo");
        REQUIRE(topo);
        CHECK(retopo::retopoGuides(*topo).size() == 2);
        drag(path({px(-10, -40), px(10, -40)}), view, Qt::ControlModifier);
        CHECK(retopo::retopoGuides(*topo).size() == 1);
        // Starting on a guide's end carries it on in its colour; ending back at its start closes it.
        std::vector<retopo::Guide> before = retopo::retopoGuides(*topo);
        drag(path({px(50, 0), px(50, 40)}), view);
        std::vector<retopo::Guide> after = retopo::retopoGuides(*topo);
        REQUIRE(after.size() == 1);
        CHECK(after[0].points.size() > before[0].points.size());
        CHECK(after[0].color == before[0].color);
        CHECK_FALSE(after[0].closed);
        drag(path({px(50, 40), px(-50, 40), px(-50, 0)}), view);
        after = retopo::retopoGuides(*topo);
        REQUIRE(after.size() == 1);
        CHECK(after[0].closed);
    }
    SUBCASE("lazy pencil trails the cursor, smoothing keeps the ends")
    {
        meshNode("ref", plane());
        PrefStore::get().set("general", "/topology/pencil/lazy", 10.0);
        PrefStore::get().set("general", "/topology/pencil/smoothing", 1.0);
        begin(ui::TopoMode::Pencil);
        ui::ViewInfo view = topView();
        drag(path({px(-50, 0), px(0, 20), px(50, 0)}), view);
        scene::Node* topo = scene::Graph::current().find("topo");
        REQUIRE(topo);
        std::vector<retopo::Guide> g = retopo::retopoGuides(*topo);
        REQUIRE(g.size() == 1);
        CHECK(g[0].points.front().x == doctest::Approx(-50.0).epsilon(0.02));
        CHECK(g[0].points.back().x < 45.0);
        CHECK(g[0].points.back().x > 30.0);
    }
    SUBCASE("pencil with symmetry meets the middle and continues from the mirror")
    {
        meshNode("ref", plane());
        begin(ui::TopoMode::Pencil);
        ui::ViewInfo view = topView();
        drag(path({px(30, 0), px(30, 30)}), view);
        scene::Node* topo = scene::Graph::current().find("topo");
        REQUIRE(topo);
        ops::execute("setSymmetry", {{"args", {"topo"}}, {"axis", "x"}});
        // Drawn from the mirror image's end: the guide on the +X side carries on.
        drag(path({px(-30, 30), px(-30, 60)}), view);
        std::vector<retopo::Guide> g = retopo::retopoGuides(*topo);
        REQUIRE(g.size() == 1);
        CHECK(g[0].points.back().x == doctest::Approx(30.0).epsilon(0.05));
        CHECK(g[0].points.back().z == doctest::Approx(-60.0).epsilon(0.05));
        // Across the middle: cut where it meets the plane.
        drag(path({px(40, -40), px(-20, -40)}), view);
        g = retopo::retopoGuides(*topo);
        REQUIRE(g.size() == 2);
        for (const glm::dvec3& p : g[1].points)
            CHECK(p.x >= -1e-6);
        CHECK(g[1].points.back().x == doctest::Approx(0.0));
        // Ending a few pixels short of the middle lands on it.
        drag(path({px(40, -70), px(4, -70)}), view);
        g = retopo::retopoGuides(*topo);
        REQUIRE(g.size() == 3);
        CHECK(g[2].points.back().x == doctest::Approx(0.0));
    }
    SUBCASE("auto region and handles")
    {
        meshNode("ref", plane());
        begin(ui::TopoMode::Region);
        ui::ViewInfo view = topView();
        drag(path({px(-30, 0), px(30, 0)}), view);
        CHECK(tool()->regionFaceCount() > 20);
        toolKey(Qt::Key_Return);
        std::vector<retopo::ConstructRecord> recs = records();
        REQUIRE(recs.size() == 1);
        CHECK(recs[0].kind == retopo::ConstructKind::Region);
        REQUIRE(topoMesh());
        CHECK(topoMesh()->numFaces() > 4);
        CHECK(tool()->regionFaceCount() == 0);
        const int id = recs[0].id;

        // Faces sets the density; one far past the limit is refused instead of running for minutes.
        CHECK(tool()->editConstruct(id, [](nlohmann::json& spec) { spec["targetFaces"] = 60; }));
        CHECK(records()[0].counts.at(0) == doctest::Approx(60).epsilon(0.5));
        const size_t faces = topoMesh()->numFaces();
        CHECK_FALSE(tool()->editConstruct(id, [](nlohmann::json& spec) {
            spec.erase("targetFaces");
            spec["edgeLength"] = 0.001;
        }));
        CHECK(topoMesh()->numFaces() == faces);

        // The region undoes like any other edit.
        ops::execute("undo", nlohmann::json::object());
        CHECK(records()[0].spec.value("targetFaces", 0) == 0);
        ops::execute("undo", nlohmann::json::object());
        CHECK((!topoMesh() || topoMesh()->numFaces() == 0));
        ops::execute("redo", nlohmann::json::object());
        ops::execute("redo", nlohmann::json::object());
        REQUIRE(topoMesh());
        CHECK(topoMesh()->numFaces() == faces);

        tool()->setMode(ui::TopoMode::Handles);
        drag(path({px(-20, 0), px(20, 0)}), view);
        geo::Mesh before = *topoMesh();
        drag(path({px(-20, 0), px(-20, 15)}), view);
        const geo::Mesh* after = topoMesh();
        double moved = 0.0;
        for (geo::Index p = 0; p < before.pointCapacity() && p < after->pointCapacity(); ++p)
            if (before.pointValid(p) && after->pointValid(p))
                moved = std::max(moved, glm::length(after->position(p) - before.position(p)));
        CHECK(moved > 3.0);
    }
    ui::viewportTools::set(nullptr);
}

TEST_CASE("topology toolkit lists the constructs and follows the active one")
{
    setup();
    meshNode("ref", plane());
    begin(ui::TopoMode::Strip);
    drag(path({px(-60, -60), px(0, -60)}), topView());
    ui::TopologyToolkit kit;
    kit.show();
    auto* list = kit.findChild<QListWidget*>();
    REQUIRE(list);
    CHECK(list->count() == 1);
    ui::viewportTools::set(nullptr);
}

TEST_CASE("retopo draw: drag moves an edge or a face, Ctrl middle-drag moves a loop, a click still places a dot")
{
    setup();
    meshNode("ref", plane());
    scene::Graph& g = scene::Graph::current();
    // A 2x2 grid of 40 cm quads centred on the origin.
    auto P = [](double x, double z) { return json::array({x, 0.0, z}); };
    for (double x : {-40.0, 0.0})
        for (double z : {-40.0, 0.0})
            ops::execute("retopoDrawAddFace", {{"mesh", "retopo"}, {"reference", "ref"}, {"create", true}, {"weld", 0.5},
                                               {"points", {P(x, z), P(x, z + 40), P(x + 40, z + 40), P(x + 40, z)}}});
    auto mesh = [&]() -> const geo::Mesh& {
        g.evaluate();
        return *g.find("retopo")->as<scene::MeshPtr>(scene::MeshNode::kOutMesh);
    };
    REQUIRE(mesh().numFaces() == 4);
    REQUIRE(mesh().numPoints() == 9);
    auto at = [&](double x, double z) {
        const geo::Mesh& m = mesh();
        for (geo::Index p = 0; p < m.pointCapacity(); ++p)
            if (m.pointValid(p) && glm::length(m.position(p) - glm::dvec3(x, 0.0, z)) < 1e-6)
                return true;
        return false;
    };
    ui::viewportTools::set(std::make_unique<ui::RetopoDrawTool>(g.find("ref")->id(), "retopo"));
    auto* draw = ui::activeRetopoDraw();
    REQUIRE(draw);
    const ui::ViewInfo view = topView();
    // World +Z is up the screen in this view.
    auto spx = [](double x, double z) { return px(x, -z); };
    auto dragWith = [&](Qt::MouseButton button, QPointF from, QPointF to, Qt::KeyboardModifiers mods) {
        QMouseEvent press(QEvent::MouseButtonPress, from, from, button, button, mods);
        draw->mousePress(&press, view);
        for (int k = 1; k <= 5; ++k)
        {
            QPointF p = from + (to - from) * (k / 5.0);
            QMouseEvent move(QEvent::MouseMove, p, p, Qt::NoButton, button, mods);
            draw->mouseMove(&move, view);
        }
        QMouseEvent release(QEvent::MouseButtonRelease, to, to, button, Qt::NoButton, mods);
        draw->mouseRelease(&release, view);
    };

    // The right border's upper edge, grabbed at its middle.
    dragWith(Qt::LeftButton, spx(40, -20), spx(46, -20), Qt::NoModifier);
    CHECK(at(46, -40));
    CHECK(at(46, 0));
    CHECK(at(40, 40));

    // The lower left face, grabbed inside.
    dragWith(Qt::MiddleButton, spx(-20, 20), spx(-20, 26), Qt::NoModifier);
    CHECK(at(-40, 6));
    CHECK(at(-40, 46));
    CHECK(at(0, 46));
    CHECK(at(40, 40));

    // The middle column, through its upper edge.
    dragWith(Qt::MiddleButton, spx(0, -20), spx(-8, -20), Qt::ControlModifier);
    CHECK(at(-8, -40));
    CHECK(at(-8, 6));
    CHECK(at(-8, 46));
    CHECK(mesh().numPoints() == 9);

    // A click inside a face places a dot and moves nothing.
    dragWith(Qt::LeftButton, spx(20, 20), spx(20, 20), Qt::NoModifier);
    CHECK(mesh().numPoints() == 9);
    CHECK(at(40, 40));
    CHECK(retopo::retopoDrawDots(*g.find("retopo")).size() == 1);
    ui::viewportTools::set(nullptr);
}

namespace
{
/** Looking down -Z at (x, y), cmPerPx to the pixel, that point at pixel (100, 100). */
ui::ViewInfo zoomedFrontView(double x, double y, double cmPerPx)
{
    ui::ViewInfo v;
    v.eye = {x, y, 200.0};
    v.view = glm::lookAt(v.eye, glm::dvec3(x, y, 0.0), glm::dvec3(0.0, 1.0, 0.0));
    const double half = 100.0 * cmPerPx;
    // Reversed-Z as the viewport's cameras are, so pixel rays start in front of the sphere.
    v.proj = glm::ortho(-half, half, -half, half, 400.0, 1.0);
    v.width = v.height = 200.0;
    v.ortho = true;
    v.camera = "front";
    return v;
}

/** A quad on the front of a 50 cm sphere reference, with Retopo Draw running on it. */
ui::RetopoDrawTool* drawOnSphere()
{
    geo::SphereOptions o;
    o.subdivAxis = 48;
    o.subdivHeight = 48;
    meshNode("ref", geo::makeSphere(o));
    auto S = [](double x, double y) {
        const glm::dvec3 p = glm::normalize(glm::dvec3(x, y, 1.0)) * 50.0;
        return json::array({p.x, p.y, p.z});
    };
    ops::execute("retopoDrawAddFace", {{"mesh", "retopo"}, {"reference", "ref"}, {"create", true},
                                       {"points", {S(-0.4, -0.4), S(0.4, -0.4), S(0.4, 0.4), S(-0.4, 0.4)}}});
    ui::viewportTools::set(std::make_unique<ui::RetopoDrawTool>(scene::Graph::current().find("ref")->id(), "retopo"));
    REQUIRE(ui::activeRetopoDraw());
    return ui::activeRetopoDraw();
}

const geo::Mesh& retopoMesh()
{
    scene::Graph& g = scene::Graph::current();
    g.evaluate();
    return *g.find("retopo")->as<scene::MeshPtr>(scene::MeshNode::kOutMesh);
}

/** The retopo point nearest to p. */
geo::Index nearestPoint(const glm::dvec3& p)
{
    const geo::Mesh& m = retopoMesh();
    geo::Index best = geo::kInvalid;
    for (geo::Index q = 0; q < m.pointCapacity(); ++q)
        if (m.pointValid(q) && (best == geo::kInvalid || glm::length(m.position(q) - p) < glm::length(m.position(best) - p)))
            best = q;
    return best;
}

void drawDrag(Qt::MouseButton button, QPointF from, QPointF to, const ui::ViewInfo& view, Qt::KeyboardModifiers mods = Qt::NoModifier)
{
    ui::RetopoDrawTool* draw = ui::activeRetopoDraw();
    QMouseEvent press(QEvent::MouseButtonPress, from, from, button, button, mods);
    draw->mousePress(&press, view);
    for (int k = 1; k <= 5; ++k)
    {
        QPointF p = from + (to - from) * (k / 5.0);
        QMouseEvent move(QEvent::MouseMove, p, p, Qt::NoButton, button, mods);
        draw->mouseMove(&move, view);
    }
    QMouseEvent release(QEvent::MouseButtonRelease, to, to, button, Qt::NoButton, mods);
    draw->mouseRelease(&release, view);
}
} // namespace

TEST_CASE("retopo draw: an inserted loop's points can be grabbed close up")
{
    setup();
    drawOnSphere();
    // Ctrl click on the middle of the top edge, from far enough to see the whole quad.
    const glm::dvec3 corner = glm::normalize(glm::dvec3(0.4, 0.4, 1.0)) * 50.0;
    const ui::ViewInfo far = zoomedFrontView(0.0, 0.0, 1.0);
    drawDrag(Qt::LeftButton, QPointF(100.0, 100.0 + corner.y), QPointF(100.0, 100.0 + corner.y), far, Qt::ControlModifier);
    REQUIRE(retopoMesh().numPoints() == 6);

    const glm::dvec3 top = glm::normalize(glm::dvec3(0.0, 0.4, 1.0)) * 50.0;
    const geo::Index p = nearestPoint(top);
    const glm::dvec3 was = retopoMesh().position(p);
    CHECK(std::abs(was.x) < 0.5);
    // 0.05 cm to the pixel: the occlusion slack is a fraction of a millimetre here.
    const ui::ViewInfo close = zoomedFrontView(was.x, was.y, 0.05);
    drawDrag(Qt::MiddleButton, QPointF(100, 100), QPointF(140, 100), close);
    CHECK(retopoMesh().position(p).x == doctest::Approx(was.x + 2.0).epsilon(0.05));
    CHECK(retopoMesh().position(p).z > 40.0);
    ui::viewportTools::set(nullptr);
}

TEST_CASE("retopo draw: X-Ray Reference lets a point under the reference be grabbed")
{
    setup();
    drawOnSphere();
    // A corner pushed 5 cm under the surface.
    const glm::dvec3 corner = glm::normalize(glm::dvec3(0.4, 0.4, 1.0)) * 50.0;
    const geo::Index p = nearestPoint(corner);
    const glm::dvec3 under = corner * 0.9;
    ops::execute("retopoDrawMove", {{"mesh", "retopo"}, {"points", {p}}, {"positions", {json::array({under.x, under.y, under.z})}}});
    const ui::ViewInfo close = zoomedFrontView(under.x, under.y, 0.05);

    ui::setXrayReference(false);
    drawDrag(Qt::MiddleButton, QPointF(100, 100), QPointF(140, 100), close);
    CHECK(glm::length(retopoMesh().position(p) - under) < 1e-9);

    ui::setXrayReference(true);
    drawDrag(Qt::MiddleButton, QPointF(100, 100), QPointF(140, 100), close);
    CHECK(retopoMesh().position(p).x == doctest::Approx(under.x + 2.0).epsilon(0.05));
    CHECK(retopoMesh().position(p).z > 40.0);
    ui::setXrayReference(false);
    ui::viewportTools::set(nullptr);
}
