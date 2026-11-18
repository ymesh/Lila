#include <doctest/doctest.h>

#include "TestUtil.h"

#include "core/Events.h"
#include "core/MainQueue.h"
#include "gn/Gn.h"
#include "ops/Command.h"
#include "ops/Undo.h"
#include "scene/Graph.h"
#include "scene/Node.h"
#include "scene/SceneCommands.h"
#pragma push_macro("slots")
#undef slots
#include "tex/GraphFiles.h"
#include "tex/Layers.h"
#include "tex/TextureSet.h"
#pragma pop_macro("slots")
#include "ui/Actions.h"
#include "ui/Hotkeys.h"
#include "ui/MenuContext.h"
#include "ui/NodeEditor.h"
#include "ui/TextureGraphUi.h"
#include "ui/TextureSetPanel.h"
#include "ui/ViewInfo.h"
#include "ui/ViewportTool.h"
#include "ui/Controls.h"
#include "ui/TexturePaintTool.h"
#include "ui/ViewportHooks.h"

#include <glm/gtc/matrix_transform.hpp>

#include <QApplication>
#include <QCheckBox>
#include <QComboBox>
#include <QLineEdit>
#include <QListWidget>
#include <QMimeData>
#include <QMouseEvent>
#include <QMainWindow>
#include <QPushButton>
#include <QRegularExpression>
#include <QDoubleSpinBox>
#include <QSpinBox>
#include <QToolButton>
#include <QTreeWidget>

using namespace lila;
using json = nlohmann::json;
using namespace dockstest;

namespace
{
void pumpWithDeletes()
{
    for (int i = 0; i < 4; ++i)
    {
        mainq::drain();
        QApplication::processEvents();
        QApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
    }
}

json stack() { return run("texLayerStack", {{"args", {"Rock"}}}); }

std::vector<std::string> rootIds()
{
    std::vector<std::string> ids;
    const json s = stack();
    for (const json& l : s["layers"])
        ids.push_back(l["id"].get<std::string>());
    return ids;
}

const json* layerIn(const json& layers, const std::string& id)
{
    for (const json& l : layers)
    {
        if (l["id"] == id)
            return &l;
        if (const json* c = layerIn(l.value("children", json::array()), id))
            return c;
    }
    return nullptr;
}
} // namespace

TEST_CASE("texture set panel builds a layer stack through tex commands, and undo restores it")
{
    registerTexGraphUi();
    run("newScene");
    run("texCreateSet", {{"args", {"Rock"}}, {"resolution", 64}});

    std::vector<std::string> executed;
    int listener = ops::addExecuteListener([&](const ExecuteEvent& e) {
        if (e.done && e.ok)
            executed.push_back(std::string(e.name));
    });
    auto ran = [&](const char* name) { return std::find(executed.begin(), executed.end(), name) != executed.end(); };

    QMainWindow win;
    auto* panel = new ui::TextureSetPanel(&win);
    win.setCentralWidget(panel);
    win.resize(420, 900);
    win.show();
    panel->setTextureSet("Rock");
    pumpWithDeletes();
    CHECK(panel->setPicker()->currentText() == "Rock");

    // Two fill layers: the second goes above the selected first, and the rows show the top first.
    const std::string a = panel->addLayer("fill");
    REQUIRE(a == "L1");
    CHECK(ran("texAddLayer"));
    CHECK(panel->selectedLayer() == a);
    const std::string b = panel->addLayer("fill");
    REQUIRE(!b.empty());
    pumpWithDeletes();
    CHECK(rootIds() == std::vector<std::string>{a, b});
    REQUIRE(panel->tree()->topLevelItemCount() == 2);
    CHECK(panel->tree()->topLevelItem(0)->data(0, Qt::UserRole + 1).toString().toStdString() == b);

    // Reorder: drop the top layer below the other.
    executed.clear();
    panel->moveLayer(b, a, ui::TextureSetPanel::Drop::Below);
    pumpWithDeletes();
    CHECK(ran("texMoveLayer"));
    CHECK(rootIds() == std::vector<std::string>{b, a});
    CHECK(panel->tree()->topLevelItem(0)->data(0, Qt::UserRole + 1).toString().toStdString() == a);

    // Rows carry no channel toggles; the channel list in Properties turns a channel the fill had no value for on.
    CHECK(panel->findChildren<QToolButton*>(QRegularExpression("^channel:")).isEmpty());
    CHECK_FALSE(layerIn(stack()["layers"], a)->at("resolved")["emissive"]["enabled"].get<bool>());
    panel->selectLayer(a);
    pumpWithDeletes();
    auto* emissive = panel->findChild<QCheckBox*>("propChannel:emissive");
    REQUIRE(emissive);
    executed.clear();
    emissive->click();
    pumpWithDeletes();
    CHECK(ran("texSetLayer"));
    CHECK(layerIn(stack()["layers"], a)->at("resolved")["emissive"]["enabled"].get<bool>());

    // The row's opacity sets every channel's (the channel picker shows all channels).
    auto* opacity = panel->findChild<QDoubleSpinBox*>(QString::fromStdString("opacity:" + a));
    REQUIRE(opacity);
    executed.clear();
    opacity->setValue(50);
    pumpWithDeletes();
    CHECK(ran("texSetLayer"));
    json resolved = layerIn(stack()["layers"], a)->at("resolved");
    CHECK(resolved["baseColor"]["opacity"].get<double>() == doctest::Approx(0.5));
    CHECK(resolved["roughness"]["opacity"].get<double>() == doctest::Approx(0.5));
    CHECK(resolved["emissive"]["enabled"].get<bool>());

    // Undo puts the opacity back, and the row follows.
    REQUIRE(UndoStack::get().undo());
    pumpWithDeletes();
    CHECK(layerIn(stack()["layers"], a)->at("resolved")["baseColor"]["opacity"].get<double>() == doctest::Approx(1.0));
    opacity = panel->findChild<QDoubleSpinBox*>(QString::fromStdString("opacity:" + a));
    REQUIRE(opacity);
    CHECK(opacity->value() == doctest::Approx(100.0));

    // Hiding a layer, and a folder taking a dropped layer.
    executed.clear();
    auto* eye = panel->findChild<QToolButton*>(QString::fromStdString("eye:" + a));
    REQUIRE(eye);
    eye->click();
    pumpWithDeletes();
    CHECK_FALSE(layerIn(stack()["layers"], a)->at("enabled").get<bool>());
    const std::string folder = panel->addLayer("folder");
    REQUIRE(!folder.empty());
    panel->moveLayer(a, folder, ui::TextureSetPanel::Drop::Into);
    pumpWithDeletes();
    const json afterMove = stack();
    const json* f = layerIn(afterMove["layers"], folder);
    REQUIRE(f);
    REQUIRE(f->at("children").size() == 1);
    CHECK(f->at("children")[0]["id"] == a);

    // A mask on the selected layer, then duplicate keeps it with fresh ids.
    panel->selectLayer(b);
    pumpWithDeletes();
    const std::string mask = panel->addMask("black");
    REQUIRE(!mask.empty());
    CHECK(ran("texAddMask"));
    CHECK(panel->selectedMask() == mask);
    panel->selectLayer(b);
    const size_t before = rootIds().size();
    panel->duplicateSelected();
    pumpWithDeletes();
    std::vector<std::string> ids = rootIds();
    REQUIRE(ids.size() == before + 1);
    const json afterCopy = stack();
    const json* copy = layerIn(afterCopy["layers"], panel->selectedLayer());
    REQUIRE(copy);
    CHECK(copy->at("id") != b);
    REQUIRE(copy->at("masks").size() == 1);
    CHECK(copy->at("masks")[0]["id"] != mask);

    // Delete the copy.
    executed.clear();
    panel->deleteSelected();
    pumpWithDeletes();
    CHECK(ran("texRemoveLayer"));
    CHECK(rootIds().size() == before);

    ops::removeExecuteListener(listener);
}

TEST_CASE("texture set panel applies smart materials from its shelf and shows the graph-mode banner")
{
    registerTexGraphUi();
    run("newScene");
    run("texCreateSet", {{"args", {"Rock"}}, {"resolution", 64}});

    QMainWindow win;
    auto* panel = new ui::TextureSetPanel(&win);
    win.setCentralWidget(panel);
    win.resize(420, 900);
    win.show();
    panel->setTextureSet("Rock");
    pumpWithDeletes();

    REQUIRE(panel->smartShelf()->count() > 0);
    std::string material;
    for (int i = 0; i < panel->smartShelf()->count(); ++i)
        if (panel->smartShelf()->item(i)->data(Qt::UserRole + 1).toString() == "material")
            material = panel->smartShelf()->item(i)->data(Qt::UserRole).toString().toStdString();
    REQUIRE(!material.empty());

    std::vector<std::string> executed;
    int listener = ops::addExecuteListener([&](const ExecuteEvent& e) {
        if (e.done && e.ok)
            executed.push_back(std::string(e.name));
    });
    panel->applySmartMaterial(material);
    pumpWithDeletes();
    CHECK(std::find(executed.begin(), executed.end(), "texApplySmartMaterial") != executed.end());
    REQUIRE(rootIds().size() == 1);
    CHECK(panel->selectedLayer() == rootIds().front());
    CHECK(panel->tree()->topLevelItemCount() == 1);
    CHECK(panel->properties() != nullptr);
    CHECK(panel->banner()->isHidden());

    // A node added by hand that the stack can't show puts the set in graph mode; Rebuild Layers drops it.
    run("texAddNode", {{"args", {"Rock", "levels"}}});
    pumpWithDeletes();
    CHECK(stack()["mode"] == "graph");
    CHECK_FALSE(panel->banner()->isHidden());
    auto* rebuild = panel->banner()->findChild<QPushButton*>("rebuildLayers");
    REQUIRE(rebuild);
    rebuild->click();
    pumpWithDeletes();
    CHECK(stack()["mode"] == "stack");
    CHECK(panel->banner()->isHidden());

    ops::removeExecuteListener(listener);
}

TEST_CASE("smart materials and masks sit apart on the shelf, and a smart material dropped on a mesh lands in its texture set")
{
    registerTexGraphUi();
    ui::registerTextureSetPanel();
    run("newScene");
    const std::string cube = run("polyCube")[0].get<std::string>();

    QMainWindow win;
    auto* panel = new ui::TextureSetPanel(&win);
    win.setCentralWidget(panel);
    win.resize(420, 900);
    win.show();
    pumpWithDeletes();

    // The shelf shows one kind at a time: materials first.
    QListWidget* shelf = panel->smartShelf();
    REQUIRE(shelf->count() > 0);
    CHECK(shelf->dragEnabled());
    for (int i = 0; i < shelf->count(); ++i)
        CHECK(shelf->item(i)->isHidden() == (shelf->item(i)->data(Qt::UserRole + 1).toString() != "material"));

    // Brushes: preset tiles only, no drags, and the search narrows them.
    panel->showLibrary("brushes");
    pumpWithDeletes();
    int brushes = 0;
    for (int i = 0; i < shelf->count(); ++i)
        if (!shelf->item(i)->isHidden())
        {
            CHECK(shelf->item(i)->data(Qt::UserRole + 1).toString() == "brush");
            ++brushes;
        }
    CHECK(brushes > 1);
    CHECK_FALSE(shelf->dragEnabled());
    auto* search = panel->findChild<QLineEdit*>("shelfSearch");
    REQUIRE(search);
    search->setText("airbrush");
    int found = 0;
    for (int i = 0; i < shelf->count(); ++i)
        found += shelf->item(i)->isHidden() ? 0 : 1;
    CHECK(found >= 1);
    CHECK(found < brushes);
    search->clear();
    // The Brush page lives in the panel's lower pane.
    CHECK(panel->findChild<QWidget*>("texturePaintPage") != nullptr);
    panel->showLibrary("materials");
    pumpWithDeletes();

    // A drop on a mesh with only Default gives it a material and a texture set, with the smart material's layer on top.
    QMimeData mime;
    mime.setData("application/x-lila-smart-material", QByteArray("Rusty Iron\nmaterial"));
    ui::viewportHooks::DropTarget target;
    // The viewport's drag-enter carries no object; the drag has to be taken there to reach the mesh at all.
    CHECK(ui::viewportHooks::handleDrop(mime, target, false));
    CHECK_FALSE(ui::viewportHooks::handleDrop(mime, target, true));
    target.node = scene::Graph::current().find(cube)->id();
    target.face = 0;
    CHECK(ui::viewportHooks::handleDrop(mime, target, false));
    CHECK(ui::viewportHooks::handleDrop(mime, target, true));
    pumpWithDeletes();
    const json sets = run("texSets");
    REQUIRE(sets.size() == 1);
    const std::string set = sets[0].is_string() ? sets[0].get<std::string>() : sets[0].value("name", std::string());
    const json view = run("texLayerStack", {{"args", {set}}});
    REQUIRE(view["layers"].size() == 1);
    CHECK(view["layers"][0]["name"] == "Rusty Iron");
}

TEST_CASE("texture set resolution steps through texture sizes with its up/down buttons")
{
    registerTexGraphUi();
    run("newScene");
    run("texCreateSet", {{"args", {"Rock"}}, {"resolution", 1024}});

    QMainWindow win;
    auto* panel = new ui::TextureSetPanel(&win);
    win.setCentralWidget(panel);
    win.resize(420, 900);
    win.show();
    panel->setTextureSet("Rock");
    pumpWithDeletes();

    auto shown = [&](const char* name) -> QSpinBox* {
        for (QSpinBox* b : panel->findChildren<QSpinBox*>(name))
            if (!b->isHidden())
                return b;
        return nullptr;
    };
    QSpinBox* width = shown("width");
    REQUIRE(width);
    REQUIRE(width->value() == 1024);
    auto* up = width->findChild<QToolButton*>("presetUp");
    REQUIRE(up);
    up->click();
    pumpWithDeletes();
    // Linked by default: both sides double.
    REQUIRE(shown("width"));
    CHECK(shown("width")->value() == 2048);
    CHECK(shown("height")->value() == 2048);

    shown("height")->findChild<QToolButton*>("presetDown")->click();
    pumpWithDeletes();
    CHECK(shown("width")->value() == 1024);
    CHECK(shown("height")->value() == 1024);
}

TEST_CASE("menu context listeners hear a layout change while a tool keeps the menus the same")
{
    QMainWindow win;
    ui::menuContext::install(&win);
    static int heard = 0;
    static bool added = false;
    if (!std::exchange(added, true))
        ui::menuContext::onChanged([] { ++heard; });
    ui::menuContext::setLayoutSet("Texture");
    // Texture Paint running keeps the Texture menus whatever the layout.
    events::send({.type = events::Event::ViewportToolChanged, .name = "texturePaint"});
    pumpWithDeletes();
    REQUIRE(ui::menuContext::current() == "Texture");
    const int before = heard;
    ui::menuContext::setLayoutSet("Model");
    // The menus didn't change, but the layout did: the texturing 2D View has to hear it to hide.
    CHECK(heard == before + 1);
    events::send({.type = events::Event::ViewportToolChanged, .name = ""});
    pumpWithDeletes();
}

TEST_CASE("the Brush page's fill modes are one icon strip that sets what a fill covers")
{
    registerTexGraphUi();
    run("newScene");
    QMainWindow win;
    auto* panel = new ui::TextureSetPanel(&win);
    win.setCentralWidget(panel);
    win.show();
    pumpWithDeletes();
    auto* modes = panel->findChild<ui::controls::Segmented*>("fillBy");
    REQUIRE(modes);
    const QList<QToolButton*> segments = modes->findChildren<QToolButton*>();
    REQUIRE(segments.size() == 6);
    for (QToolButton* b : segments)
    {
        CHECK(b->text().isEmpty());
        CHECK_FALSE(b->toolTip().isEmpty());
    }
    // The payload names its fill mode on fills only.
    auto* fillTool = panel->findChild<QToolButton*>("tool:fill");
    REQUIRE(fillTool);
    fillTool->click();
    segments[2]->click();
    CHECK(ui::texturePaintSettings()["payload"]["fillBy"] == "island");
    segments[1]->click();
    CHECK(ui::texturePaintSettings()["payload"]["fillBy"] == "polygon");
}

namespace
{
/** Looking down -Y at the XZ plane, 1 px per cm, world origin at pixel (100, 100). */
ui::ViewInfo fillTopView()
{
    ui::ViewInfo v;
    v.eye = {0.0, 200.0, 0.0};
    v.view = glm::lookAt(v.eye, glm::dvec3(0.0), glm::dvec3(0.0, 0.0, -1.0));
    // As the viewport builds it: y down the screen, reversed Z (1 is the near plane).
    v.proj = glm::ortho(-100.0, 100.0, 100.0, -100.0, 400.0, 1.0);
    v.width = v.height = 200.0;
    v.ortho = true;
    v.camera = "top";
    return v;
}

void toolDrag(const std::vector<QPointF>& pts, const ui::ViewInfo& view)
{
    ui::ViewportTool* tool = ui::viewportTools::active();
    REQUIRE(tool);
    QMouseEvent hover(QEvent::MouseMove, pts.front(), pts.front(), Qt::NoButton, Qt::NoButton, Qt::NoModifier);
    tool->mouseMove(&hover, view);
    QMouseEvent press(QEvent::MouseButtonPress, pts.front(), pts.front(), Qt::LeftButton, Qt::LeftButton, Qt::NoModifier);
    tool->mousePress(&press, view);
    for (size_t i = 1; i < pts.size(); ++i)
    {
        QMouseEvent move(QEvent::MouseMove, pts[i], pts[i], Qt::NoButton, Qt::LeftButton, Qt::NoModifier);
        tool->mouseMove(&move, view);
    }
    QMouseEvent release(QEvent::MouseButtonRelease, pts.back(), pts.back(), Qt::LeftButton, Qt::NoButton, Qt::NoModifier);
    tool->mouseRelease(&release, view);
}
} // namespace

TEST_CASE("a fill click fills the polygon under it; a drag fills every polygon its box takes in")
{
    registerTexGraphUi();
    run("newScene");
    const std::string plane = run("polyPlane", {{"width", 100.0}, {"depth", 100.0}, {"subdivisionsX", 10}, {"subdivisionsZ", 10}})[0].get<std::string>();
    run("createMaterial", {{"args", {"FloorMat"}}});
    run("assignMaterial", {{"args", {"FloorMat", plane}}});
    run("texCreateSet", {{"args", {"Floor"}}, {"resolution", 64}, {"materials", {"FloorMat"}}});

    QMainWindow win;
    auto* panel = new ui::TextureSetPanel(&win);
    win.setCentralWidget(panel);
    win.show();
    pumpWithDeletes();
    ui::startTexturePaint();
    panel->findChild<QToolButton*>("tool:fill")->click();
    panel->findChild<ui::controls::Segmented*>("fillBy")->findChildren<QToolButton*>()[1]->click(); // polygon

    // The dock suite has no GPU painting: a stand-in takes the strokes, and the test checks what the tool asks for.
    if (!ops::find("texPaintStroke"))
        ops::registerCommand({.name = "texPaintStroke",
                              .doc = "Test stand-in.",
                              .positionalDoc = "set layer",
                              .flags = {{"stroke", "", ArgType::Any, nullptr, "The stroke."}},
                              .undoable = false,
                              .run = [](const Args&) -> json { return nullptr; }});
    std::vector<size_t> filled;
    int listener = ops::addExecuteListener([&](const ExecuteEvent& e) {
        if (e.done && e.ok && e.name == "texPaintStroke")
        {
            const json pay = e.call.value("stroke", json::object()).value("payload", json::object());
            filled.push_back(pay.contains("triangles") ? pay["triangles"].size() : size_t(1));
        }
    });
    const ui::ViewInfo view = fillTopView();
    // A click on one 10 cm quad.
    toolDrag({QPointF(105.0, 105.0)}, view);
    pumpWithDeletes();
    REQUIRE(filled.size() == 1);
    CHECK(filled[0] == 1);
    // A box over 3 x 3 quad centres: 9 quads, 18 triangles.
    toolDrag({QPointF(71.0, 71.0), QPointF(80.0, 80.0), QPointF(99.0, 99.0)}, view);
    pumpWithDeletes();
    REQUIRE(filled.size() == 2);
    CHECK(filled[1] == 18);
    ops::removeExecuteListener(listener);
    ui::viewportTools::set(nullptr);
}
