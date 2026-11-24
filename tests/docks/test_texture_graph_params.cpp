#include <doctest/doctest.h>

#include "TestUtil.h"

#include "scene/Graph.h"
#include "tex/Graph.h"
#pragma push_macro("slots")
#undef slots
#include "tex/Plan.h"
#pragma pop_macro("slots")
#include "tex/TextureSet.h"
#include "ui/NodeEditor.h"
#include "ui/TexGraphPreview.h"

#include <QAction>
#include <QComboBox>
#include <QDoubleSpinBox>
#include <QGraphicsItem>
#include <QGraphicsView>
#include <QLabel>
#include <QMainWindow>
#include <QSpinBox>
#include <QThread>

#include <chrono>

using namespace lila;
using json = nlohmann::json;
using namespace dockstest;

namespace
{
tex::Graph rockGraph() { return tex::readGraph(*tex::findSet(scene::Graph::current(), "Rock")); }

json graphParams(const std::string& graph) { return run("texGraphParams", {{"args", {graph}}}); }

/** A set Rock with a Perlin noise named Noise feeding roughness. */
void makeRock()
{
    registerTexGraphUi();
    run("newScene");
    run("texCreateSet", {{"args", {"Rock"}}, {"resolution", 64}});
    run("texAddNode", {{"args", {"Rock", "perlin"}}, {"name", "Noise"}});
    run("texConnect", {{"args", {"Rock", "Noise.out", "Output.roughness"}}});
}

/** The value a plan gives a node's seed parameter (node is its path through groups, or its last step). */
double plannedSeed(const std::string& node)
{
    const tex::Graph g = rockGraph();
    tex::PlanRequest req;
    req.width = req.height = 16;
    req.outputs = tex::channelRequests(g, tex::readSettings(*tex::findSet(scene::Graph::current(), "Rock")));
    const tex::Plan plan = tex::buildPlan(g, req);
    REQUIRE(plan.error.empty());
    for (const tex::UniformEntry& e : plan.params)
        if (e.id == "seed" && (e.node == node || e.node.ends_with("/" + node)))
            return plan.uniforms[size_t(e.slot) * 4];
    FAIL("no seed for " << node);
    return 0.0;
}

QAction* labelAction(QWidget* root, const QString& text)
{
    for (QWidget* w : root->findChildren<QWidget*>())
        for (QAction* a : w->actions())
            if (a->text() == text)
                return a;
    return nullptr;
}
} // namespace

TEST_CASE("graph parameters drive the set's nodes, and presets store and recall them")
{
    makeRock();
    CHECK(run("texExposeParam", {{"args", {"Rock", "Noise.persistence"}}, {"label", "Grit"}}) == "persistence");
    CHECK_THROWS(run("texExposeParam", {{"args", {"Rock", "Noise.persistence"}}}));
    CHECK_THROWS(run("texExposeParam", {{"args", {"Rock", "Noise.nothing"}}}));
    CHECK_THROWS(run("texExposeParam", {{"args", {"Rock", "Noise.octaves"}}, {"name", "@seed"}}));

    run("texSetGraphParam", {{"args", {"Rock", "persistence", 0.2}}});
    CHECK(rockGraph().node("Noise")->values.value("persistence", 0.0) == doctest::Approx(0.2));
    CHECK_THROWS(run("texSetGraphParam", {{"args", {"Rock", "persistence", "rough"}}}));
    json gp = graphParams("Rock");
    REQUIRE(gp["params"].size() == 1);
    CHECK(gp["params"][0]["label"] == "Grit");
    CHECK(gp["params"][0]["value"].get<double>() == doctest::Approx(0.2));

    // Editing the node itself is the parameter's value too.
    run("texSetValue", {{"args", {"Rock", "Noise.persistence", 0.3}}});
    CHECK(graphParams("Rock")["params"][0]["value"].get<double>() == doctest::Approx(0.3));

    run("texSavePreset", {{"args", {"Rock", "Smooth"}}});
    run("texSetGraphParam", {{"args", {"Rock", "persistence", 0.8}}});
    CHECK(graphParams("Rock")["active"] == "");
    run("texSavePreset", {{"args", {"Rock", "Rough"}}});
    CHECK(graphParams("Rock")["active"] == "Rough");
    CHECK(graphParams("Rock")["presets"] == json::array({"Smooth", "Rough"}));

    run("texApplyPreset", {{"args", {"Rock", "Smooth"}}});
    CHECK(rockGraph().node("Noise")->values.value("persistence", 0.0) == doctest::Approx(0.3));
    CHECK(graphParams("Rock")["active"] == "Smooth");
    run("undo");
    CHECK(rockGraph().node("Noise")->values.value("persistence", 0.0) == doctest::Approx(0.8));
    run("redo");

    CHECK(run("texRenamePreset", {{"args", {"Rock", "Smooth", "Soft"}}}) == "Soft");
    CHECK_THROWS(run("texRenamePreset", {{"args", {"Rock", "Soft", "Rough"}}}));
    run("texDeletePreset", {{"args", {"Rock", "Rough"}}});
    CHECK(graphParams("Rock")["presets"] == json::array({"Soft"}));
    run("undo");
    CHECK(graphParams("Rock")["presets"] == json::array({"Soft", "Rough"}));

    // The presets go with the set's graph.
    CHECK(rockGraph().presets.size() == 2);
    CHECK(tex::Graph::fromJson(rockGraph().toJson()).presets == rockGraph().presets);

    // Removing a parameter leaves the node at its value and drops it from the presets.
    run("texUnexposeParam", {{"args", {"Rock", "persistence"}}});
    CHECK(graphParams("Rock")["params"].empty());
    CHECK(rockGraph().node("Noise")->values.value("persistence", 0.0) == doctest::Approx(0.3));
    CHECK_FALSE(rockGraph().presets[0]["values"].contains("persistence"));
}

TEST_CASE("a node group's graph parameters set its defaults, with presets of their own")
{
    makeRock();
    run("texAddNode", {{"args", {"Rock", "perlin"}}, {"name", "Fine"}});
    run("texConnect", {{"args", {"Rock", "Fine.out", "Output.metallic"}}});
    const json grouped = run("texGroupNodes", {{"args", {"Rock", "Fine"}}, {"name", "Grain"}});
    const std::string group = "Rock/" + grouped["group"].get<std::string>();
    CHECK(run("texExposeParam", {{"args", {group, "Fine.scale"}}}) == "scale");
    run("texSetGraphParam", {{"args", {group, "scale", 9}}});
    const tex::Graph inner = tex::readGraphAt(scene::Graph::current(), group);
    REQUIRE(inner.input("scale"));
    CHECK(inner.input("scale")->defaultValue == 9);
    CHECK(inner.input("scale")->targets == std::vector<std::string>{"Fine.scale"});
    run("texSavePreset", {{"args", {group, "Big"}}});
    run("texSetGraphParam", {{"args", {group, "scale", 2}}});
    run("texApplyPreset", {{"args", {group, "Big"}}});
    CHECK(tex::readGraphAt(scene::Graph::current(), group).input("scale")->defaultValue == 9);
}

TEST_CASE("the Texture Graph side panel edits graph parameters and presets")
{
    makeRock();
    run("texExposeParam", {{"args", {"Rock", "Noise.persistence"}}});
    run("texSavePreset", {{"args", {"Rock", "Base"}}});

    QMainWindow win;
    auto* editor = new ui::NodeEditor(&win, ui::kTextureGraphBackend);
    win.setCentralWidget(editor);
    win.resize(1100, 700);
    win.show();
    editor->setGroup("Rock");
    pump();

    auto* panel = editor->findChild<QWidget*>("graphParams");
    REQUIRE(panel);
    auto* presets = panel->findChild<QComboBox*>("graphPresets");
    REQUIRE(presets);
    CHECK(presets->count() == 1);
    CHECK(presets->currentText() == "Base");
    QDoubleSpinBox* field = nullptr;
    for (QDoubleSpinBox* s : panel->findChildren<QDoubleSpinBox*>())
        if (s->toolTip().contains("Drives Noise.persistence"))
            field = s;
    REQUIRE(field);
    field->setValue(0.65);
    pump();
    CHECK(rockGraph().node("Noise")->values.value("persistence", 0.0) == doctest::Approx(0.65));
    // The values no longer match Base, so the picker says Custom.
    panel = editor->findChild<QWidget*>("graphParams");
    REQUIRE(panel);
    CHECK(panel->findChild<QComboBox*>("graphPresets")->currentIndex() == -1);

    // Picking the preset puts its values back.
    presets = panel->findChild<QComboBox*>("graphPresets");
    presets->setCurrentIndex(0);
    Q_EMIT presets->activated(0);
    pump();
    CHECK(rockGraph().node("Noise")->values.value("persistence", 0.0) == doctest::Approx(0.5));

    // A selected node's parameter labels offer Expose; an exposed one offers Remove.
    auto* view = editor->findChild<QGraphicsView*>();
    REQUIRE(view);
    for (QGraphicsItem* i : view->scene()->items())
        if ((i->flags() & QGraphicsItem::ItemIsSelectable) && QLineF(i->pos(), QPointF(rockGraph().node("Noise")->position.x,
                                                                                       rockGraph().node("Noise")->position.y)).length() < 0.5)
            i->setSelected(true);
    pump();
    QAction* expose = labelAction(editor, "Expose as Graph Parameter");
    REQUIRE(expose);
    CHECK(labelAction(editor, "Remove Graph Parameter"));
    expose->trigger();
    pump();
    CHECK(graphParams("Rock")["params"].size() == 2);
}

TEST_CASE("the graph seed offsets every seed parameter in the set, groups included")
{
    makeRock();
    run("texAddNode", {{"args", {"Rock", "perlin"}}, {"name", "Fine"}});
    run("texConnect", {{"args", {"Rock", "Fine.out", "Output.metallic"}}});
    run("texGroupNodes", {{"args", {"Rock", "Fine"}}, {"name", "Grain"}});
    run("texSetValue", {{"args", {"Rock", "Noise.seed", 3}}});
    CHECK(plannedSeed("Noise") == 3.0);
    CHECK(plannedSeed("Fine") == 0.0);

    CHECK(run("texSetGraphSeed", {{"args", {"Rock", 10}}}) == 10);
    CHECK(rockGraph().seed == 10);
    CHECK(plannedSeed("Noise") == 13.0);
    CHECK(plannedSeed("Fine") == 10.0);
    CHECK(graphParams("Rock")["seed"] == 10);
    CHECK_THROWS(run("texSetGraphSeed", {{"args", {"Rock/Grain", 1}}}));
    CHECK(run("texSetGraphSeed", {{"args", {"Rock"}}, {"random", true}}).get<int>() != 0);
    run("undo");
    CHECK(rockGraph().seed == 10);

    // Presets keep the seed.
    run("texSavePreset", {{"args", {"Rock", "Ten"}}});
    run("texSetGraphSeed", {{"args", {"Rock", 20}}});
    CHECK(graphParams("Rock")["active"] == "");
    run("texApplyPreset", {{"args", {"Rock", "Ten"}}});
    CHECK(rockGraph().seed == 10);
    CHECK(graphParams("Rock")["active"] == "Ten");

    QMainWindow win;
    auto* editor = new ui::NodeEditor(&win, ui::kTextureGraphBackend);
    win.setCentralWidget(editor);
    win.resize(1100, 700);
    win.show();
    editor->setGroup("Rock");
    pump();
    QSpinBox* seed = nullptr;
    for (QSpinBox* s : editor->findChild<QWidget*>("graphParams")->findChildren<QSpinBox*>())
        if (s->toolTip().contains("re-rolls"))
            seed = s;
    REQUIRE(seed);
    CHECK(seed->value() == 10);
    seed->setValue(7);
    pump();
    CHECK(rockGraph().seed == 7);
    CHECK(editor->findChild<QComboBox*>("graphPresets")->currentIndex() == -1);
}

TEST_CASE("the pinned preview stays on the set while its groups open, and holds it while pinned")
{
    makeRock();
    run("texAddNode", {{"args", {"Rock", "perlin"}}, {"name", "Fine"}});
    run("texConnect", {{"args", {"Rock", "Fine.out", "Output.metallic"}}});
    const std::string group = "Rock/" + run("texGroupNodes", {{"args", {"Rock", "Fine"}}, {"name", "Grain"}})["group"].get<std::string>();
    run("texCreateSet", {{"args", {"Moss"}}, {"resolution", 64}});

    QMainWindow win;
    auto* editor = new ui::NodeEditor(&win, ui::kTextureGraphBackend);
    win.setCentralWidget(editor);
    win.resize(1100, 700);
    win.show();
    editor->setGroup("Rock");
    pump();
    auto* preview = dynamic_cast<ui::TexGraphPreview*>(editor->findChild<QWidget*>("texGraphPreview"));
    REQUIRE(preview);
    CHECK(preview->set() == "Rock");
    editor->enterGroup(QString::fromStdString(group));
    pump();
    CHECK(editor->group().toStdString() == group);
    CHECK(preview->set() == "Rock");

    // It renders the set on the preview ball.
    const auto end = std::chrono::steady_clock::now() + std::chrono::seconds(180);
    while (preview->image().isNull() && std::chrono::steady_clock::now() < end)
    {
        pump();
        QThread::msleep(20);
    }
    INFO("status: " << preview->status().toStdString());
    CHECK_FALSE(preview->image().isNull());

    preview->setPinned(true);
    editor->setGroup("Moss");
    pump();
    CHECK(preview->set() == "Rock");
    preview->setPinned(false);
    pump();
    CHECK(preview->set() == "Moss");

    // No mesh uses Moss, so the Mesh shape says so.
    CHECK(preview->mesh().empty());
    preview->setShape(ui::TexGraphPreview::Shape::Mesh);
    pump();
    CHECK(preview->status() == "No mesh uses Moss");

    // Another backend drops the preview.
    editor->setBackend(ui::kGeometryNodesBackend);
    pump();
    CHECK_FALSE(editor->findChild<QWidget*>("texGraphPreview"));
}

TEST_CASE("node timings time each node's passes, by the graph on show")
{
    makeRock();
    run("texAddNode", {{"args", {"Rock", "perlin"}}, {"name", "Fine"}});
    run("texConnect", {{"args", {"Rock", "Fine.out", "Output.metallic"}}});
    const json grouped = run("texGroupNodes", {{"args", {"Rock", "Fine"}}, {"name", "Grain"}});
    const std::string group = "Rock/" + grouped["group"].get<std::string>();

    const json top = run("texProfileGraph", {{"args", {"Rock"}}, {"size", 64}});
    CHECK(top["backend"] == "cpu");
    CHECK(top["size"] == 64);
    CHECK(top["total"].get<double>() > 0.0);
    CHECK(top["nodes"].contains("Noise"));
    CHECK(top["nodes"].contains(grouped["node"].get<std::string>()));
    CHECK_FALSE(top["nodes"].contains("Fine"));
    const json inner = run("texProfileGraph", {{"args", {group}}, {"size", 64}});
    CHECK(inner["nodes"].contains("Fine"));
    CHECK_FALSE(inner["nodes"].contains("Noise"));

    QMainWindow win;
    auto* editor = new ui::NodeEditor(&win, ui::kTextureGraphBackend);
    win.setCentralWidget(editor);
    win.resize(1100, 700);
    win.show();
    editor->setGroup("Rock");
    pump();
    REQUIRE(ui::actions::find("nodeEditor.toggleTimings"));
    editor->setTimings(true);
    CHECK(editor->timings());
    const auto until = [&](auto done) {
        const auto end = std::chrono::steady_clock::now() + std::chrono::seconds(120);
        while (!done() && std::chrono::steady_clock::now() < end)
        {
            pump();
            QThread::msleep(20);
        }
        return done();
    };
    REQUIRE(until([&] { return editor->nodeTimes().contains("Noise"); }));

    // An edit times the graph again; opening the group shows its own nodes.
    editor->enterGroup(QString::fromStdString(group));
    CHECK(until([&] { return editor->nodeTimes().contains("Fine"); }));
    CHECK_FALSE(editor->nodeTimes().contains("Noise"));
    editor->setTimings(false);
    CHECK(editor->nodeTimes().empty());

    // A backend that can't time nodes turns the overlay off.
    editor->setTimings(true);
    editor->setBackend(ui::kGeometryNodesBackend);
    CHECK_FALSE(editor->timings());
}
