#include <doctest/doctest.h>

#include "TestUtil.h"

#include "core/ImageIO.h"
#include "core/MainQueue.h"
#include "core/Paths.h"
#include "core/PrefStore.h"
#include "gn/Gn.h"
#include "ops/Command.h"
#include "scene/Anim.h"
#include "scene/Graph.h"
#include "scene/Node.h"
#include "scene/SceneCommands.h"
#include "tex/GraphFiles.h"
#include "tex/TextureSet.h"
#include "ui/Actions.h"
#include "ui/Hotkeys.h"
#include "ui/NodeEditor.h"
#include "ui/NodePicker.h"
#include "ui/TexParams.h"
#include "ui/TexPreview.h"
#include "ui/TextureGraphUi.h"

#include <QApplication>
#include <QComboBox>
#include <QDoubleSpinBox>
#include <QGraphicsItem>
#include <QGraphicsView>
#include <QKeyEvent>
#include <QLayout>
#include <QLineEdit>
#include <QMainWindow>
#include <QMouseEvent>
#include <QPlainTextEdit>
#include <QThread>
#include <QToolButton>

#include <chrono>
#include <filesystem>
#include <format>
#include <map>

using namespace lila;
using json = nlohmann::json;
using namespace dockstest;

namespace
{
/** Pumps until done() or the timeout; the CPU evaluator compiles each new graph structure first. */
template <typename F>
bool pumpUntil(F done, int seconds = 60)
{
    auto end = std::chrono::steady_clock::now() + std::chrono::seconds(seconds);
    while (std::chrono::steady_clock::now() < end)
    {
        pump();
        if (done())
            return true;
        QThread::msleep(20);
    }
    return false;
}

void pressWithText(QWidget* w, Qt::Key key, Qt::KeyboardModifiers mods = Qt::NoModifier, const QString& text = {})
{
    QKeyEvent down(QEvent::KeyPress, key, mods, text);
    QApplication::sendEvent(w, &down);
    QKeyEvent up(QEvent::KeyRelease, key, mods, text);
    QApplication::sendEvent(w, &up);
    pump();
}

void viewMouse(QGraphicsView* view, QEvent::Type type, QPointF scenePos, Qt::MouseButton button, Qt::KeyboardModifiers mods = Qt::NoModifier)
{
    QPoint local = view->mapFromScene(scenePos);
    QPoint global = view->viewport()->mapToGlobal(local);
    Qt::MouseButtons buttons = type == QEvent::MouseButtonRelease ? Qt::NoButton : Qt::MouseButtons(button);
    QMouseEvent e(type, QPointF(local), QPointF(global), button, buttons, mods);
    QApplication::sendEvent(view->viewport(), &e);
}

tex::Graph rock() { return tex::readGraph(*tex::findSet(scene::Graph::current(), "Rock")); }

QGraphicsItem* nodeItem(QGraphicsView* view, const std::string& node)
{
    const tex::GNode* n = rock().node(node);
    REQUIRE(n);
    for (QGraphicsItem* i : view->scene()->items())
        if ((i->flags() & QGraphicsItem::ItemIsSelectable) && QLineF(i->pos(), QPointF(n->position.x, n->position.y)).length() < 0.5)
            return i;
    return nullptr;
}
} // namespace

TEST_CASE("texture graph editor adds, links and edits nodes through tex commands, and the 2D view shows them")
{
    registerTexGraphUi();
    run("newScene");
    run("texCreateSet", {{"args", {"Rock"}}, {"resolution", 64}});

    std::vector<std::string> executed;
    int listener = ops::addExecuteListener([&](const ExecuteEvent& e) {
        if (e.done)
            executed.push_back(std::string(e.name));
    });

    QMainWindow win;
    auto* editor = new ui::NodeEditor(&win, ui::kTextureGraphBackend);
    win.setCentralWidget(editor);
    win.resize(1000, 700);
    win.show();
    win.activateWindow();
    editor->setGroup("Rock");
    pump();
    REQUIRE(editor->backend().id() == ui::kTextureGraphBackend);
    auto* view = editor->findChild<QGraphicsView*>();
    REQUIRE(view);
    view->setFocus();
    pump();

    // Tab opens the add-node search (tex nodes only); typing and Enter adds the node under the cursor.
    pressWithText(view, Qt::Key_Tab);
    QWidget* popup = QApplication::activePopupWidget();
    REQUIRE(popup);
    auto* search = popup->findChild<QLineEdit*>();
    REQUIRE(search);
    search->setText("perlin");
    pump();
    pressWithText(search, Qt::Key_Return);
    pump();
    REQUIRE(rock().nodes.size() == 2);
    std::string perlin;
    for (const tex::GNode& n : rock().nodes)
        if (n.type == "perlin")
            perlin = n.name;
    REQUIRE(!perlin.empty());
    CHECK(std::find(executed.begin(), executed.end(), "texAddNode") != executed.end());

    // Move the noise left of the output so the wire runs left to right, then drag a wire from its output to roughness.
    run("texMoveNodes", {{"args", {"Rock"}}, {"positions", {{perlin, {-400, 0}}, {"Output", {0, 0}}}}});
    pump();
    editor->frameAll();
    pump();
    QGraphicsItem* noise = nodeItem(view, perlin);
    REQUIRE(noise);
    // Header 24, a 96 px thumbnail with 8 of margin, then one 20 px row per socket: outputs first.
    const QPointF out = QPointF(-400 + 170, 24 + 96 + 8 + 10);
    int roughness = -1;
    json info = run("texGraphInfo", {{"args", {"Rock"}}});
    for (const json& n : info["nodes"])
        if (n["name"] == "Output")
            for (size_t i = 0; i < n["inputs"].size(); ++i)
                if (n["inputs"][i]["id"] == "roughness")
                    roughness = int(i);
    REQUIRE(roughness >= 0);
    const QPointF in = QPointF(0, 24 + 20.0 * roughness + 10);
    viewMouse(view, QEvent::MouseButtonPress, out, Qt::LeftButton);
    viewMouse(view, QEvent::MouseMove, (out + in) * 0.5, Qt::LeftButton);
    viewMouse(view, QEvent::MouseMove, in, Qt::LeftButton);
    viewMouse(view, QEvent::MouseButtonRelease, in, Qt::LeftButton);
    pump();
    tex::Graph g = rock();
    REQUIRE(g.links.size() == 1);
    CHECK(g.links[0].fromNode == perlin);
    CHECK(g.links[0].toNode == "Output");
    CHECK(g.links[0].toSocket == "roughness");

    // Selecting the node shows its parameters; editing one runs texSetValue.
    view->scene()->clearSelection();
    nodeItem(view, perlin)->setSelected(true);
    pump();
    QDoubleSpinBox* persistence = nullptr;
    for (QDoubleSpinBox* s : editor->findChildren<QDoubleSpinBox*>())
        if (s->toolTip().startsWith("persistence"))
            persistence = s;
    REQUIRE(persistence);
    executed.clear();
    persistence->setValue(0.25);
    pump();
    CHECK(std::find(executed.begin(), executed.end(), "texSetValue") != executed.end());
    CHECK(rock().node(perlin)->values.value("persistence", 0.0) == doctest::Approx(0.25));
    run("undo");
    pump();
    CHECK_FALSE(rock().node(perlin)->values.contains("persistence"));

    // The node's thumbnail renders on a worker.
    CHECK(pumpUntil([&] { return ui::texPreview::get({"Rock", perlin, {}, {}, 128}).image != nullptr; }));

    // Ctrl+Shift+click previews the node in the 2D View.
    ui::Texture2DView view2d;
    view2d.resize(400, 400);
    view2d.show();
    viewMouse(view, QEvent::MouseButtonPress, nodeItem(view, perlin)->pos() + QPointF(60, 10), Qt::LeftButton, Qt::ControlModifier | Qt::ShiftModifier);
    viewMouse(view, QEvent::MouseButtonRelease, nodeItem(view, perlin)->pos() + QPointF(60, 10), Qt::LeftButton, Qt::ControlModifier | Qt::ShiftModifier);
    pump();
    CHECK(ui::texture2d::current().node == perlin);
    view2d.setResolution(128);
    REQUIRE(pumpUntil([&] { return !view2d.displayed().isNull(); }));
    CHECK(view2d.displayed().width() == 128);
    CHECK(view2d.status().contains(QString::fromStdString(perlin)));
    CHECK(view2d.inspect(5, 5).startsWith("5, 5"));
    CHECK(view2d.inspect(500, 5).isEmpty());
    // Noise isn't flat.
    QImage img = view2d.displayed();
    bool varied = false;
    for (int x = 1; x < img.width() && !varied; ++x)
        varied = img.pixel(x, 7) != img.pixel(0, 7);
    CHECK(varied);
    view2d.setSolo(1);
    CHECK(!view2d.displayed().isNull());

    // A value edit re-renders the view.
    QImage before = view2d.displayed();
    run("texSetValue", {{"args", {"Rock", perlin + ".seed", 7}}});
    CHECK(pumpUntil([&] { return view2d.displayed() != before && !view2d.status().contains("rendering"); }));

    ops::removeExecuteListener(listener);
}

TEST_CASE("texture graph backend follows texture sets and their meshes")
{
    registerTexGraphUi();
    run("newScene");
    run("texCreateSet", {{"args", {"Wood"}}, {"materials", {"Planks"}}});
    run("texCreateSet", {{"args", {"Stone"}}});
    scene::Graph& g = scene::Graph::current();
    CHECK(ui::textureSetForSlot(g, "Planks") == "Wood");
    // A set is picked by materials, never matched by name.
    CHECK(ui::textureSetForSlot(g, "Stone").empty());
    CHECK(ui::textureSetForSlot(g, "Other").empty());

    QMainWindow win;
    auto* editor = new ui::NodeEditor(&win, ui::kTextureGraphBackend);
    win.setCentralWidget(editor);
    win.show();
    pump();
    run("select", {{"args", {"Stone"}}});
    pump();
    CHECK(editor->group() == "Stone");
    run("select", {{"args", {"Wood"}}});
    pump();
    CHECK(editor->group() == "Wood");
    auto* groups = editor->findChild<QComboBox*>();
    REQUIRE(groups);
}

TEST_CASE("node previews hide per node with the graph, and for every node from the view toggle")
{
    registerTexGraphUi();
    run("newScene");
    run("texCreateSet", {{"args", {"Rock"}}, {"resolution", 64}});
    const std::string perlin = run("texAddNode", {{"args", {"Rock", "perlin"}}, {"position", {-400, 0}}}).get<std::string>();
    QMainWindow win;
    auto* editor = new ui::NodeEditor(&win, ui::kTextureGraphBackend);
    win.setCentralWidget(editor);
    win.resize(1000, 700);
    win.show();
    win.activateWindow();
    editor->setGroup("Rock");
    pump();
    auto* view = editor->findChild<QGraphicsView*>();
    REQUIRE(view);
    view->setFocus();
    pump();
    REQUIRE(editor->previews());
    const double shown = nodeItem(view, perlin)->boundingRect().height();

    // The per-node flag is kept with the graph and undoes.
    view->scene()->clearSelection();
    nodeItem(view, perlin)->setSelected(true);
    ui::actions::run(json("nodeEditor.toggleNodePreview"));
    pump();
    CHECK(rock().node(perlin)->hidePreview);
    CHECK(nodeItem(view, perlin)->boundingRect().height() == doctest::Approx(shown - 96 - 8));
    CHECK(editor->nodeFlagOn("hidePreview"));
    run("undo");
    pump();
    CHECK_FALSE(rock().node(perlin)->hidePreview);
    CHECK(nodeItem(view, perlin)->boundingRect().height() == doctest::Approx(shown));

    // The view toggle hides every preview and is remembered as a pref.
    ui::actions::run(json("nodeEditor.togglePreviews"));
    pump();
    CHECK_FALSE(editor->previews());
    CHECK_FALSE(PrefStore::get().value<bool>("general", "/nodeEditor/previews", true));
    CHECK(nodeItem(view, perlin)->boundingRect().height() == doctest::Approx(shown - 96 - 8));
    ui::actions::run(json("nodeEditor.togglePreviews"));
    pump();
    CHECK(editor->previews());
    CHECK(nodeItem(view, perlin)->boundingRect().height() == doctest::Approx(shown));
}

TEST_CASE("fresnel's camera field takes a picked camera's pose, and the gradient editor types a stop's position")
{
    registerTexGraphUi();
    run("newScene");
    const std::string set = run("texCreateSet", {{"args", {"FresnelPanel"}}, {"resolution", 64}}).get<std::string>();
    const std::string fres = run("texAddNode", {{"args", {set, "fresnel"}}}).get<std::string>();
    const std::string cam = run("createNode", {{"args", {"camera"}}, {"name", "rimCam"}}).get<std::string>();
    run("setAttr", {{"args", {cam + ".translate", 0.0, 0.0, 500.0}}});
    json node;
    const json info = run("texGraphInfo", {{"args", {set}}});
    for (const json& n : info["nodes"])
        if (n.value("name", "") == fres)
            node = n;
    REQUIRE(node.is_object());

    std::map<std::string, json> sets;
    QWidget host;
    QWidget* panel = ui::texParams::nodePanel(&host, node, [&](const std::string& id, const json& v) { sets[id] = v; });
    REQUIRE(panel);
    QToolButton* activeView = nullptr;
    for (QToolButton* b : panel->findChildren<QToolButton*>())
        if (b->text() == "Active View")
            activeView = b;
    CHECK(activeView);
    const QList<ui::NodePicker*> pickers = panel->findChildren<ui::NodePicker*>();
    REQUIRE(pickers.size() == 1);

    Q_EMIT pickers.front()->edited(json(cam));
    CHECK(sets["camera"] == json(cam));
    REQUIRE(sets.contains("viewDirection"));
    // A new camera looks down -Z from +Z, so the view runs toward the origin.
    CHECK(sets["viewDirection"][2].get<double>() == doctest::Approx(-1.0));
    REQUIRE(sets.contains("cameraPosition"));
    CHECK(sets["cameraPosition"][2].get<double>() == doctest::Approx(500.0));

    json gradient;
    QWidget* editor = ui::texParams::gradientEditor(&host, json::array({json::array({0.0, "#000000"}), json::array({1.0, "#ffffff"})}),
                                                    [&](const json& v) { gradient = v; });
    editor->resize(300, 80);
    QWidget* bar = editor->layout()->itemAt(0)->widget();
    REQUIRE(bar);
    // A click on a stop's handle under the bar selects it; the Position field then edits that stop.
    const QPointF handle(bar->width() - 8.0, 26.0);
    QMouseEvent press(QEvent::MouseButtonPress, handle, bar->mapToGlobal(handle), Qt::LeftButton, Qt::LeftButton, Qt::NoModifier);
    QApplication::sendEvent(bar, &press);
    QMouseEvent release(QEvent::MouseButtonRelease, handle, bar->mapToGlobal(handle), Qt::LeftButton, Qt::NoButton, Qt::NoModifier);
    QApplication::sendEvent(bar, &release);
    QDoubleSpinBox* position = nullptr;
    for (QDoubleSpinBox* s : editor->findChildren<QDoubleSpinBox*>())
        if (s->prefix().startsWith("Position"))
            position = s;
    REQUIRE(position);
    position->setValue(0.25);
    REQUIRE(!gradient.is_null());
    json stops = ui::texParams::gradientStops(gradient);
    bool moved = false;
    for (const json& s : stops)
        moved = moved || s[0].get<double>() == doctest::Approx(0.25);
    CHECK(moved);
}

TEST_CASE("texture parameter editors read every value form the evaluator takes")
{
    json stops = ui::texParams::gradientStops(json::array({json::array({1.0, "#ffffff"}), json::array({0.0, 0.5})}));
    REQUIRE(stops.size() == 2);
    CHECK(stops[0][0].get<double>() == 0.0);
    CHECK(stops[0][1][0].get<double>() == doctest::Approx(0.5));
    CHECK(ui::texParams::gradientStops(json{{"stops", json::array()}}).size() == 2);

    json pts = ui::texParams::curvePoints(json{{"points", json::array({json::array({1.0, 0.0}), json::array({0.0, 1.0})})}});
    REQUIRE(pts.size() == 2);
    CHECK(pts[0][0].get<double>() == 0.0);
    CHECK(ui::texParams::evalCurve(pts, false, 0.25) == doctest::Approx(0.75));
    CHECK(ui::texParams::evalCurve(pts, true, 0.5) == doctest::Approx(0.5));
    CHECK(ui::texParams::evalCurve(pts, true, -1.0) == doctest::Approx(1.0));
}

TEST_CASE("texture graph nodes rename in place and the graph takes notes")
{
    registerTexGraphUi();
    run("newScene");
    run("texCreateSet", {{"args", {"Rock"}}, {"resolution", 64}});
    const std::string perlin = run("texAddNode", {{"args", {"Rock", "perlin"}}, {"position", {-400, 0}}}).get<std::string>();
    QMainWindow win;
    auto* editor = new ui::NodeEditor(&win, ui::kTextureGraphBackend);
    win.setCentralWidget(editor);
    win.resize(1000, 700);
    win.show();
    win.activateWindow();
    editor->setGroup("Rock");
    pump();
    auto* view = editor->findChild<QGraphicsView*>();
    REQUIRE(view);
    view->setFocus();
    pump();

    view->scene()->clearSelection();
    nodeItem(view, perlin)->setSelected(true);
    pressWithText(view, Qt::Key_F2);
    auto* edit = qobject_cast<QLineEdit*>(editor->inPlaceEditor());
    REQUIRE(edit);
    edit->setText("Rust Breakup");
    pressWithText(edit, Qt::Key_Return);
    CHECK(rock().node(perlin)->label == "Rust Breakup");
    run("undo");
    pump();
    CHECK(rock().node(perlin)->label.empty());

    ui::actions::run(json("nodeEditor.addNote"));
    pump();
    auto* text = qobject_cast<QPlainTextEdit*>(editor->inPlaceEditor());
    REQUIRE(text);
    text->setPlainText("Breakup only drives roughness.");
    pressWithText(text, Qt::Key_Return, Qt::ControlModifier);
    REQUIRE(rock().frames.size() == 1);
    CHECK(rock().frames[0].text == "Breakup only drives roughness.");
    run("undo");
    pump();
    CHECK(rock().frames.empty());
}

TEST_CASE("texture graph nodes group into a node group that opens, edits its inputs, and ungroups")
{
    registerTexGraphUi();
    run("newScene");
    run("texCreateSet", {{"args", {"Rock"}}, {"resolution", 64}});
    run("texAddNode", {{"args", {"Rock", "math"}}, {"name", "m0"}, {"position", {-500, 0}}});
    run("texAddNode", {{"args", {"Rock", "math"}}, {"name", "m1"}, {"position", {-250, 0}}});
    run("texConnect", {{"args", {"Rock", "m0.out", "m1.a"}}});
    run("texConnect", {{"args", {"Rock", "m1.out", "Output.roughness"}}});
    QMainWindow win;
    auto* editor = new ui::NodeEditor(&win, ui::kTextureGraphBackend);
    win.setCentralWidget(editor);
    win.resize(1000, 700);
    win.show();
    editor->setGroup("Rock");
    pump();
    auto* view = editor->findChild<QGraphicsView*>();
    REQUIRE(view);
    auto selectAt = [&](QPointF at) {
        view->scene()->clearSelection();
        for (QGraphicsItem* i : view->scene()->items())
            if ((i->flags() & QGraphicsItem::ItemIsSelectable) && QLineF(i->pos(), at).length() < 0.5)
                i->setSelected(true);
        pump();
    };
    auto has = [](const tex::Graph& g, const char* from, const char* to) {
        return std::any_of(g.links.begin(), g.links.end(), [&](const tex::GLink& l) { return l.fromNode + "." + l.fromSocket == from && l.toNode + "." + l.toSocket == to; });
    };

    selectAt(QPointF(-250, 0));
    ui::actions::run(json("nodeEditor.groupNodes"));
    pump();
    tex::Graph g = rock();
    REQUIRE(g.subgroups.contains("NodeGroup"));
    REQUIRE(g.node("NodeGroup"));
    CHECK(g.node("NodeGroup")->type == "Group");
    CHECK_FALSE(g.node("m1"));
    CHECK(has(g, "m0.out", "NodeGroup.a"));
    CHECK(has(g, "NodeGroup.out", "Output.roughness"));
    CHECK_FALSE(run("texGraphInfo", {{"args", {"Rock"}}}).contains("error"));

    selectAt(QPointF(-250, 0));
    ui::actions::run(json("nodeEditor.openGroup"));
    pump();
    CHECK(editor->group() == "Rock/NodeGroup");
    json inner = run("texGraphInfo", {{"args", {"Rock/NodeGroup"}}});
    CHECK(inner["nodes"].size() == 3);
    run("texRenameInput", {{"args", {"Rock/NodeGroup", "a", "amount"}}});
    CHECK(has(rock(), "m0.out", "NodeGroup.amount"));
    run("texAddInput", {{"args", {"Rock/NodeGroup", "bias", "float"}}});
    CHECK(tex::Graph::fromJson(rock().subgroups.at("NodeGroup")).inputs.size() == 2);
    ui::actions::run(json("nodeEditor.parentGroup"));
    pump();
    CHECK(editor->group() == "Rock");

    selectAt(QPointF(-250, 0));
    ui::actions::run(json("nodeEditor.ungroup"));
    pump();
    g = rock();
    CHECK_FALSE(g.node("NodeGroup"));
    CHECK(g.subgroups.empty());
    CHECK(has(g, "m0.out", "m1.a"));
    CHECK(has(g, "m1.out", "Output.roughness"));
    run("undo");
    pump();
    CHECK(rock().node("NodeGroup"));
}

TEST_CASE("a node group's inputs rename on Group Input by double-click, and its outputs rename and move")
{
    registerTexGraphUi();
    run("newScene");
    run("texCreateSet", {{"args", {"Rock"}}, {"resolution", 64}});
    run("texAddNode", {{"args", {"Rock", "math"}}, {"name", "m0"}, {"position", {-500, 0}}});
    run("texAddNode", {{"args", {"Rock", "separateRgb"}}, {"name", "s"}, {"position", {-250, 0}}});
    run("texConnect", {{"args", {"Rock", "m0.out", "s.input"}}});
    run("texConnect", {{"args", {"Rock", "s.r", "Output.roughness"}}});
    run("texConnect", {{"args", {"Rock", "s.g", "Output.metallic"}}});
    run("texGroupNodes", {{"args", {"Rock", "s"}}});
    auto inner = [] { return tex::Graph::fromJson(rock().subgroups.at("NodeGroup")); };
    REQUIRE(inner().outputs.size() == 2);
    const std::string first = inner().outputs[0].name, second = inner().outputs[1].name;

    run("texRenameInput", {{"args", {"Rock/NodeGroup", first, "rough"}}, {"output", true}});
    CHECK(inner().outputs[0].name == "rough");
    const auto linked = [](const char* from, const char* to) {
        const tex::Graph g = rock();
        return std::any_of(g.links.begin(), g.links.end(), [&](const tex::GLink& l) { return l.fromNode + "." + l.fromSocket == from && l.toNode + "." + l.toSocket == to; });
    };
    CHECK(linked("NodeGroup.rough", "Output.roughness"));
    run("texMoveInput", {{"args", {"Rock/NodeGroup", "rough", 1}}, {"output", true}});
    CHECK(inner().outputs[0].name == second);
    CHECK(inner().outputs[1].name == "rough");

    QMainWindow win;
    auto* editor = new ui::NodeEditor(&win, ui::kTextureGraphBackend);
    win.setCentralWidget(editor);
    win.resize(1000, 700);
    win.show();
    editor->setGroup("Rock/NodeGroup");
    pump();
    auto* view = editor->findChild<QGraphicsView*>();
    REQUIRE(view);
    const tex::GNode* gi = nullptr;
    const tex::Graph g = inner();
    for (const tex::GNode& n : g.nodes)
        if (n.type == "GroupInput")
            gi = &n;
    REQUIRE(gi);
    REQUIRE(g.inputs.size() == 1);
    const std::string input = g.inputs[0].name;

    // Down the Group Input node until a double-click opens its input's name rather than the node's title.
    QLineEdit* edit = nullptr;
    for (int dy = 4; dy < 120 && !edit; dy += 4)
    {
        viewMouse(view, QEvent::MouseButtonDblClick, QPointF(gi->position.x + 60.0, gi->position.y + dy), Qt::LeftButton);
        pump();
        if (auto* e = qobject_cast<QLineEdit*>(editor->inPlaceEditor()))
        {
            if (e->text() != "Group Input")
                edit = e;
            else
                pressWithText(e, Qt::Key_Escape);
        }
    }
    REQUIRE(edit);
    edit->setText("Strength");
    pressWithText(edit, Qt::Key_Return);
    CHECK(inner().inputs[0].name == "strength");
    CHECK(linked("m0.out", "NodeGroup.strength"));
}

TEST_CASE("texture previews show an Image Sequence at the scene's current frame")
{
    registerTexGraphUi();
    const std::filesystem::path dir = std::filesystem::temp_directory_path() / "lila_docks_preview_sequence";
    std::filesystem::create_directories(dir);
    for (int i = 0; i < 3; ++i)
    {
        const float v = 0.1f + 0.2f * float(i);
        image::write(dir / std::format("heat_{:04}.exr", i + 1), image::Image{2, 2, std::vector<float>{v, v, v, 1, v, v, v, 1, v, v, v, 1, v, v, v, 1}}, 32);
    }
    run("newScene");
    run("texCreateSet", {{"args", {"Fx"}}, {"resolution", 8}});
    run("texAddNode", {{"args", {"Fx", "imageSequence"}}, {"name", "Seq"}});
    run("texSetValue", {{"args", {"Fx", "Seq.file", paths::toUtf8(dir / "heat_0001.exr")}}});
    run("texConnect", {{"args", {"Fx", "Seq.out", "Output.baseColor"}}});
    CHECK(ui::texPreview::animated("Fx"));

    auto shown = [](double frame) {
        run("currentTime", {{"args", {scene::anim::secondsOf(frame)}}});
        const ui::texPreview::Result r = ui::texPreview::render({"Fx", "Seq", {}, {}, 8});
        REQUIRE(r.image);
        return r.image->rgba[0];
    };
    CHECK(shown(1.0) == doctest::Approx(0.1).epsilon(0.01));
    CHECK(shown(3.0) == doctest::Approx(0.5).epsilon(0.01));
    CHECK(shown(2.0) == doctest::Approx(0.3).epsilon(0.01));
}
