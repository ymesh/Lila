#include "TestUtil.h"

#include "core/PrefStore.h"
#include "scene/ProxySet.h"
#include "scene/Skin.h"
#include "scene/SkinPaint.h"
#include "ui/AnimProxyUi.h"
#include "ui/Camera.h"
#include "ui/SelectionHandlers.h"
#include "ui/ViewportDisplay.h"
#include "ui/ViewportHooks.h"
#include "ui/ViewportTool.h"
#include "ui/WeightPaintTool.h"

#include <QCoreApplication>
#include <QKeyEvent>
#include <QMouseEvent>

using namespace rigtest;
using namespace lila::scene;

namespace
{
lila::ui::ViewInfo frontView()
{
    lila::ui::Camera cam;
    cam.yaw = 0.0;
    cam.pitch = 0.0;
    cam.distance = 300.0;
    lila::ui::ViewInfo v;
    v.view = cam.view();
    v.proj = cam.proj(1.0);
    v.eye = cam.eye();
    v.width = 400;
    v.height = 400;
    return v;
}

double total(const std::string& mesh, const std::string& joint)
{
    double sum = 0.0;
    for (const json& p : run("skinPercent", {{"args", {mesh}}}))
        sum += p["weights"].value(joint, 0.0);
    return sum;
}

bool grayscale(const std::vector<lila::render::LineVertex>& v)
{
    for (const auto& x : v)
        if (x.color.r != x.color.g || x.color.g != x.color.b)
            return false;
    return true;
}

void mouse(lila::ui::ViewportTool* tool, QEvent::Type type, QPointF at, const lila::ui::ViewInfo& view)
{
    Qt::MouseButtons held = type == QEvent::MouseButtonRelease ? Qt::NoButton : Qt::LeftButton;
    QMouseEvent e(type, at, at, Qt::LeftButton, held, Qt::NoModifier);
    if (type == QEvent::MouseButtonPress)
        tool->mousePress(&e, view);
    else if (type == QEvent::MouseMove)
        tool->mouseMove(&e, view);
    else
        tool->mouseRelease(&e, view);
}
} // namespace

TEST_CASE("joints draw in grayscale under Monotone and pick by bone")
{
    lila::PrefStore::get().set("general", lila::ui::viewportDisplay::kMonotone, true);
    skinnedCylinder(20);
    lila::ui::ViewInfo view = frontView();
    lila::render::FrameState s;
    lila::ui::OverlayPainter painter(s.overlayTris, 1.0);
    for (const auto& d : lila::ui::viewportHooks::drawers())
        d(s, painter, view);
    CHECK(s.lines.size() > 100);
    CHECK(grayscale(s.lines));
    lila::PrefStore::get().set("general", lila::ui::viewportDisplay::kMonotone, false);

    // A click on the bone between bone_b and bone_c picks bone_b.
    auto at = view.project({0, 25, 0});
    REQUIRE(at);
    std::string best;
    double px = 1e9;
    for (const auto& p : lila::ui::viewportHooks::pickers())
        for (const auto& h : p(*at, *at, true, view))
            if (h.pixels < px)
            {
                px = h.pixels;
                best = h.name;
            }
    CHECK(best == "bone_b");
}

TEST_CASE("a weight paint stroke paints the current influence as one undo step")
{
    std::string mesh = skinnedCylinder(20);
    run("select", {{"args", {mesh, "bone_c"}}});
    lila::ui::startWeightPaint();
    lila::ui::ViewportTool* tool = lila::ui::viewportTools::active();
    REQUIRE(tool);
    CHECK(std::string(tool->name()) == "weightPaint");

    lila::ui::ViewInfo view = frontView();
    const json weights = run("skinPercent", {{"args", {mesh}}});
    double before = total(mesh, "bone_c");
    // Default brush: replace with 1. Across the middle of the tube three bones share each point, so Strict refuses it.
    lila::PrefStore::get().set("general", skinpaint::kStrictPref, true);
    QPointF mid(200, 200);
    mouse(tool, QEvent::MouseButtonPress, mid, view);
    mouse(tool, QEvent::MouseMove, mid + QPointF(15, 0), view);
    mouse(tool, QEvent::MouseButtonRelease, mid + QPointF(15, 0), view);
    CHECK(run("skinPercent", {{"args", {mesh}}}) == weights);

    // Strict off: it paints and Lila's split is marked automatic.
    run("acceptSkinWeights", {{"args", {mesh}}});
    lila::PrefStore::get().set("general", skinpaint::kStrictPref, false);
    mouse(tool, QEvent::MouseButtonPress, mid, view);
    mouse(tool, QEvent::MouseMove, mid + QPointF(15, 0), view);
    mouse(tool, QEvent::MouseButtonRelease, mid + QPointF(15, 0), view);
    lila::PrefStore::get().set("general", skinpaint::kStrictPref, true);
    double after = total(mesh, "bone_c");
    CHECK(after > before + 1.0);
    CHECK(run("skinInfo", {{"args", {mesh}}})["unnormalized"] == 0);
    CHECK(run("skinInfo", {{"args", {mesh}}})["automatic"].get<int>() > 0);

    lila::render::FrameState s;
    lila::ui::OverlayPainter painter(s.overlayTris, 1.0);
    tool->draw(s, painter, view);
    CHECK(!s.tris3d.empty());
    CHECK(!grayscale(s.tris3d));
    CHECK(!s.points.empty());

    run("undo");
    CHECK(total(mesh, "bone_c") == doctest::Approx(before));
    run("redo");
    CHECK(total(mesh, "bone_c") == doctest::Approx(after));
    lila::ui::viewportTools::set(nullptr);
}

TEST_CASE("Ctrl+Alt+click picks the receiver, and Strict then paints where it refused")
{
    std::string mesh = skinnedCylinder(20);
    run("select", {{"args", {mesh, "bone_c"}}});
    lila::PrefStore::get().set("general", skinpaint::kStrictPref, true);
    lila::ui::startWeightPaint();
    lila::ui::ViewportTool* tool = lila::ui::viewportTools::active();
    REQUIRE(tool);
    lila::ui::ViewInfo view = frontView();
    const json weights = run("skinPercent", {{"args", {mesh}}});
    QPointF mid(200, 200);
    mouse(tool, QEvent::MouseButtonPress, mid, view);
    mouse(tool, QEvent::MouseButtonRelease, mid, view);
    CHECK(run("skinPercent", {{"args", {mesh}}}) == weights);

    // On the surface, the heaviest other joint there (bone_b in the middle) becomes the receiver.
    const auto mods = Qt::ControlModifier | Qt::AltModifier;
    QMouseEvent press(QEvent::MouseButtonPress, mid, mid, Qt::LeftButton, Qt::LeftButton, mods);
    CHECK(tool->mousePress(&press, view));
    QMouseEvent release(QEvent::MouseButtonRelease, mid, mid, Qt::LeftButton, Qt::NoButton, mods);
    tool->mouseRelease(&release, view);
    mouse(tool, QEvent::MouseButtonPress, mid, view);
    mouse(tool, QEvent::MouseButtonRelease, mid, view);
    CHECK(run("skinPercent", {{"args", {mesh}}}) != weights);
    CHECK(run("skinInfo", {{"args", {mesh}}})["unnormalized"] == 0);
    lila::ui::viewportTools::set(nullptr);
}

TEST_CASE("relax and settle strokes keep weights normalized")
{
    std::string mesh = skinnedCylinder(20);
    run("select", {{"args", {mesh, "bone_b"}}});
    run("setAttr", {{"args", {"bone_b.rotate", v3(0, 0, 90)}}});
    lila::ui::startWeightPaint();
    lila::ui::ViewportTool* tool = lila::ui::viewportTools::active();
    REQUIRE(tool);
    lila::ui::ViewInfo view = frontView();
    const json weights = run("skinPercent", {{"args", {mesh}}});
    // Shift relaxes.
    QPointF at(200, 200);
    QMouseEvent press(QEvent::MouseButtonPress, at, at, Qt::LeftButton, Qt::LeftButton, Qt::ShiftModifier);
    tool->mousePress(&press, view);
    QMouseEvent move(QEvent::MouseMove, at + QPointF(10, 0), at + QPointF(10, 0), Qt::LeftButton, Qt::LeftButton, Qt::ShiftModifier);
    tool->mouseMove(&move, view);
    QMouseEvent release(QEvent::MouseButtonRelease, at + QPointF(10, 0), at + QPointF(10, 0), Qt::LeftButton, Qt::NoButton, Qt::ShiftModifier);
    tool->mouseRelease(&release, view);
    CHECK(run("skinPercent", {{"args", {mesh}}}) != weights);
    CHECK(run("skinInfo", {{"args", {mesh}}})["unnormalized"] == 0);
    lila::ui::viewportTools::set(nullptr);
}

TEST_CASE("the split mesh tool previews pieces at the joints and cuts on Enter")
{
    std::string mesh = skinnedCylinder(20);
    run("select", {{"args", {mesh}}});
    REQUIRE(lila::ui::viewportTools::activate("mesh.splitMeshTool"));
    lila::ui::ViewportTool* tool = lila::ui::viewportTools::active();
    REQUIRE(tool);
    lila::ui::ViewInfo view = frontView();
    lila::render::FrameState s;
    lila::ui::OverlayPainter painter(s.overlayTris, 1.0);
    tool->draw(s, painter, view);
    // bone_c is a tip with too little skin of its own for a plane, so the suggestion cuts once, at bone_b.
    CHECK(s.meshes.size() == 2);
    CHECK(lila::ui::viewportHooks::hiddenInView(graph().find(mesh)->id()));

    QKeyEvent enter(QEvent::KeyPress, Qt::Key_Return, Qt::NoModifier);
    CHECK(tool->keyPress(&enter, view));
    lila::ui::viewportTools::set(nullptr);
    CHECK_FALSE(lila::ui::viewportHooks::hiddenInView(graph().find(mesh)->id()));
    CHECK(graph().find(mesh + "_piece2"));
}

TEST_CASE("a click on an anim proxy piece selects its joint")
{
    std::string mesh = skinnedCylinder(20);
    std::string set = run("animProxyFromMesh", {{"args", {mesh}}, {"remesh", "decimate"}});
    json pieces = run("proxySetInfo", {{"args", {set}}})[0]["pieces"];
    REQUIRE(pieces.size() == 2);
    lila::ui::selectionHandlers::Hit hit;
    hit.node = graph().find(pieces[0]["mesh"].get<std::string>())->id();
    hit.name = pieces[0]["mesh"];
    std::vector<std::string> picked = lila::ui::selectionHandlers::redirect({hit});
    REQUIRE(picked.size() == 1);
    CHECK(picked[0] == pieces[0]["joint"].get<std::string>());
}

namespace
{
void dragIn(lila::ui::ViewportTool* tool, QPointF from, QPointF to, const lila::ui::ViewInfo& view)
{
    mouse(tool, QEvent::MouseButtonPress, from, view);
    mouse(tool, QEvent::MouseMove, (from + to) * 0.5, view);
    mouse(tool, QEvent::MouseMove, to, view);
    mouse(tool, QEvent::MouseButtonRelease, to, view);
}

bool pressAction(lila::ui::ViewportTool* tool, const char* action)
{
    QKeyEvent e(QEvent::KeyPress, Qt::Key_Z, Qt::ControlModifier);
    return tool->hotkey(e, action);
}

lila::ui::ViewportTool* startSplitTool(const std::string& mesh, const char* tool = "mesh.splitMeshTool")
{
    run("select", {{"args", {mesh}}});
    REQUIRE(lila::ui::viewportTools::activate(tool));
    return lila::ui::viewportTools::active();
}
} // namespace

TEST_CASE("a split plane picks by its rim as well as its centre")
{
    run("newScene");
    std::string cube = run("polyCube", {{"name", "Block"}, {"width", 100}, {"height", 100}, {"depth", 100}})[0];
    lila::ui::ViewportTool* tool = startSplitTool(cube);
    lila::ui::ViewInfo view = frontView();
    // A horizontal line through the middle: a plane facing up through the cube, on the symmetry plane.
    dragIn(tool, {100, 200}, {300, 200}, view);
    REQUIRE(lila::ui::splitToolPlanes().size() == 1);
    const lila::scene::split::Plane plane = lila::ui::splitToolPlanes()[0];
    CHECK(plane.origin.x == doctest::Approx(0.0).scale(1.0));

    auto centre = view.project(plane.origin);
    REQUIRE(centre);
    // Its rim, seen edge on, runs out to either side of the centre well past the centre handle.
    auto rim = view.project(plane.origin + glm::dvec3(60.0, 0.0, 0.0));
    REQUIRE(rim);
    CHECK(std::abs(rim->x() - centre->x()) > 30.0);
    CHECK(lila::ui::splitToolPlaneAt(*rim + QPointF(0, 6), view) == 0);
    CHECK(lila::ui::splitToolPlaneAt(*centre, view) == 0);
    CHECK(lila::ui::splitToolPlaneAt(*rim + QPointF(0, 40), view) == -1);

    // Dragging the rim moves the plane.
    dragIn(tool, *rim, *rim + QPointF(0, -20), view);
    CHECK(lila::ui::splitToolPlanes()[0].origin.y > plane.origin.y + 1.0);
    lila::ui::viewportTools::set(nullptr);
}

TEST_CASE("undo in the split tool steps back through plane edits before the scene")
{
    run("newScene");
    std::string cube = run("polyCube", {{"name", "Block"}, {"width", 100}, {"height", 100}, {"depth", 100}})[0];
    lila::ui::ViewportTool* tool = startSplitTool(cube);
    lila::ui::ViewInfo view = frontView();
    dragIn(tool, {100, 200}, {300, 200}, view);
    const double y0 = lila::ui::splitToolPlanes()[0].origin.y;
    auto centre = view.project(lila::ui::splitToolPlanes()[0].origin);
    REQUIRE(centre);
    dragIn(tool, *centre, *centre + QPointF(0, -30), view);
    CHECK(lila::ui::splitToolPlanes()[0].origin.y > y0 + 1.0);

    CHECK(pressAction(tool, "edit.undo"));
    CHECK(lila::ui::splitToolPlanes()[0].origin.y == doctest::Approx(y0));
    CHECK(pressAction(tool, "edit.undo"));
    CHECK(lila::ui::splitToolPlanes().empty());
    // Nothing left in the tool: the press goes on to the scene's undo.
    CHECK_FALSE(pressAction(tool, "edit.undo"));
    CHECK(pressAction(tool, "edit.redo"));
    CHECK(lila::ui::splitToolPlanes().size() == 1);
    lila::ui::viewportTools::set(nullptr);
}

TEST_CASE("split planes mirror across the symmetry plane of a mesh that is not symmetrical")
{
    run("newScene");
    run("polyCube", {{"name", "Body"}, {"width", 60}, {"height", 100}, {"depth", 40}});
    run("polyCube", {{"name", "Lump"}, {"width", 20}, {"height", 20}, {"depth", 20}});
    run("setAttr", {{"args", {"Lump.translate", v3(40, 20, 0)}}});
    std::string both = run("polyCombine", {{"args", {"Body", "Lump"}}, {"name", "Uneven"}});
    lila::ui::ViewportTool* tool = startSplitTool(both);
    lila::ui::ViewInfo view = frontView();
    auto left = view.project({-20.0, 0.0, 0.0});
    REQUIRE(left);
    // A vertical line left of centre: a plane facing sideways, with a partner on the right.
    dragIn(tool, *left + QPointF(0, -60), *left + QPointF(0, 60), view);
    std::vector<lila::scene::split::Plane> planes = lila::ui::splitToolPlanes();
    REQUIRE(planes.size() == 2);
    CHECK(planes[1].origin.x == doctest::Approx(-planes[0].origin.x));
    CHECK(planes[1].origin.y == doctest::Approx(planes[0].origin.y));
    CHECK(planes[1].normal.x == doctest::Approx(-planes[0].normal.x));

    // Moving one moves its partner the mirrored way.
    auto c = view.project(planes[0].origin);
    REQUIRE(c);
    dragIn(tool, *c, *c + QPointF(-15, -25), view);
    planes = lila::ui::splitToolPlanes();
    CHECK(planes[1].origin.x == doctest::Approx(-planes[0].origin.x));
    CHECK(planes[1].origin.y == doctest::Approx(planes[0].origin.y));

    // A plane across the middle is its own mirror image and stays on the symmetry plane.
    auto mid = view.project({0.0, -30.0, 0.0});
    REQUIRE(mid);
    dragIn(tool, *mid + QPointF(-80, 0), *mid + QPointF(80, 0), view);
    planes = lila::ui::splitToolPlanes();
    REQUIRE(planes.size() == 3);
    CHECK(planes[2].origin.x == doctest::Approx(0.0).scale(1.0));
    auto m = view.project(planes[2].origin);
    REQUIRE(m);
    dragIn(tool, *m, *m + QPointF(25, 0), view);
    CHECK(lila::ui::splitToolPlanes()[2].origin.x == doctest::Approx(0.0).scale(1.0));
    lila::ui::viewportTools::set(nullptr);
}

TEST_CASE("undoing a made anim proxy brings the tool back with its planes")
{
    std::string mesh = skinnedCylinder(20);
    lila::ui::ViewportTool* tool = startSplitTool(mesh, "rig.animProxyTool");
    lila::ui::ViewInfo view = frontView();
    const std::vector<lila::scene::split::Plane> before = lila::ui::splitToolPlanes();
    REQUIRE_FALSE(before.empty());
    QKeyEvent enter(QEvent::KeyPress, Qt::Key_Return, Qt::NoModifier);
    CHECK(tool->keyPress(&enter, view));
    QCoreApplication::processEvents();
    CHECK_FALSE(lila::ui::viewportTools::active());
    REQUIRE(lila::scene::proxy::setsOfMesh(graph(), graph().find(mesh)->id()).size() == 1);
    const json stored = lila::scene::proxy::planesOf(graph(), lila::scene::proxy::setsOfMesh(graph(), graph().find(mesh)->id())[0]);
    CHECK(stored.size() == before.size());

    run("undo");
    QCoreApplication::processEvents();
    CHECK(lila::scene::proxy::setsOfMesh(graph(), graph().find(mesh)->id()).empty());
    REQUIRE(lila::ui::viewportTools::active());
    const std::vector<lila::scene::split::Plane> back = lila::ui::splitToolPlanes();
    REQUIRE(back.size() == before.size());
    for (size_t i = 0; i < back.size(); ++i)
        CHECK(glm::length(back[i].origin - before[i].origin) < 1e-9);

    run("redo");
    QCoreApplication::processEvents();
    CHECK_FALSE(lila::ui::viewportTools::active());
}
