#include <doctest/doctest.h>

#include "TestUtil.h"

#include "bake/Bake.h"
#include "bake/BakeInputs.h"
#include "bake/BakeNodes.h"
#include "bake/Targets.h"
#include "core/MainQueue.h"
#include "core/PrefStore.h"
#include "geo/Mesh.h"
#include "geo/Primitives.h"
#include "mcp/McpServer.h"
#include "ops/Command.h"
#include "ops/Undo.h"
#include "scene/Graph.h"
#include "scene/MeshNodes.h"
#include "scene/SceneCommands.h"
#include "scene/Transform.h"
#include "ui/Actions.h"
#include "ui/BakerPanel.h"
#include "ui/Theme.h"
#include "ui/Deletion.h"
#include "ui/Menus.h"
#include "ui/Outliner.h"
#include "ui/Panels.h"

#include "scene/Materials.h"

#include <QApplication>
#include <QCheckBox>
#include <QClipboard>
#include <QDir>
#include <QComboBox>
#include <QDoubleSpinBox>
#include <QDropEvent>
#include <QFileInfo>

#include <set>
#include <QElapsedTimer>
#include <QKeyEvent>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QMenu>
#include <QMimeData>
#include <QSpinBox>
#include <QToolButton>
#include <QTreeWidget>

#include <chrono>
#include <filesystem>
#include <future>
#include <memory>

using namespace lila;
using json = nlohmann::json;
using namespace dockstest;
namespace fs = std::filesystem;

namespace
{
std::string addPlane(const std::string& name, double y)
{
    scene::Graph& g = scene::Graph::current();
    scene::Node* n = g.createNode(*scene::types::find("mesh"), name);
    g.setValue({n->id(), scene::MeshNode::kInMesh, -1}, scene::MeshPtr(std::make_shared<geo::Mesh>(geo::makePlane({}))));
    g.setValue({n->id(), scene::Transform::kTranslate, -1}, glm::dvec3(0.0, y, 0.0));
    return n->path();
}

template <class T> T* child(QWidget* w, const char* name)
{
    T* c = w->findChild<T*>(name);
    REQUIRE_MESSAGE(c, name);
    return c;
}

QTreeWidgetItem* findRow(QTreeWidget* t, const QString& text)
{
    for (QTreeWidgetItemIterator it(t); *it; ++it)
        if ((*it)->text(0) == text)
            return *it;
    return nullptr;
}

/** A tools/call through the MCP endpoint, answered on a worker thread while the main thread runs the tool. */
json callTool(const std::string& name, const json& args)
{
    auto f = std::async(std::launch::async, [&] {
        return mcp::Server::get().handle({{"jsonrpc", "2.0"}, {"id", 1}, {"method", "tools/call"}, {"params", {{"name", name}, {"arguments", args}}}});
    });
    while (f.wait_for(std::chrono::milliseconds(5)) != std::future_status::ready)
        pump();
    return f.get()["result"];
}

scene::Node* onlyGroup(const std::string& project)
{
    scene::Graph& g = scene::Graph::current();
    const auto groups = bake::groupsOf(g, g.find(project)->id());
    REQUIRE(groups.size() == 1);
    return g.node(groups.front());
}

void setupBaker()
{
    static bool done = false;
    if (std::exchange(done, true))
        return;
    mainq::init();
    ops::registerBuiltins();
    scene::types::registerBuiltins();
    scene::registerCommands();
    bake::registerCommands();
    ui::actions::registerBuiltins();
    ui::registerBakerPanel();
}
} // namespace

TEST_CASE("baker panel: groups from names, drops, stacks, output preview, bake and thumbnails, all through undoable commands")
{
    setupBaker();
    ops::execute("newScene", json::object());
    UndoStack::get().clear();
    const std::string low = addPlane("crate_low", 0.0);
    const std::string high = addPlane("crate_high", 1.0);
    const std::string extra = addPlane("bolts", 1.5);

    ui::BakerPanel panel;
    panel.resize(420, 900);
    panel.show();
    pump();
    CHECK(panel.project().empty());

    auto* groups = child<QTreeWidget>(&panel, "bakerGroups");
    child<QToolButton>(&panel, "bakerAutoGroup")->click();
    pump();
    REQUIRE_FALSE(panel.project().empty());
    scene::Node* grp = onlyGroup(panel.project());
    CHECK(bake::groupSettings(*grp).lows == std::vector<std::string>{low});
    CHECK(findRow(groups, "crate_low"));
    CHECK(findRow(groups, "crate_high"));
    CHECK(findRow(groups, "Cage"));

    SUBCASE("undo takes the group away and the tree follows")
    {
        REQUIRE(UndoStack::get().undo());
        pump();
        CHECK_FALSE(findRow(groups, "crate_low"));
        REQUIRE(UndoStack::get().redo());
        pump();
        CHECK(findRow(groups, "crate_low"));
    }

    SUBCASE("outliner drops land on the row they hit")
    {
        QTreeWidgetItem* highRow = findRow(groups, "High");
        REQUIRE(highRow);
        auto* mime = new QMimeData;
        mime->setData(ui::kNodeNamesMime, QByteArray::fromStdString(extra));
        const QPoint at = groups->visualItemRect(highRow).center();
        REQUIRE(groups->viewport()->rect().contains(at));
        QDragEnterEvent enter(at, Qt::LinkAction | Qt::MoveAction, mime, Qt::LeftButton, Qt::NoModifier);
        QApplication::sendEvent(groups->viewport(), &enter);
        QDragMoveEvent move(at, Qt::LinkAction | Qt::MoveAction, mime, Qt::LeftButton, Qt::NoModifier);
        QApplication::sendEvent(groups->viewport(), &move);
        CHECK(move.isAccepted());
        QDropEvent drop(QPointF(at), Qt::LinkAction | Qt::MoveAction, mime, Qt::LeftButton, Qt::NoModifier);
        QApplication::sendEvent(groups->viewport(), &drop);
        CHECK(drop.isAccepted());
        pump();
        grp = onlyGroup(panel.project());
        const auto highs = bake::groupSettings(*grp).highs;
        REQUIRE(highs.size() == 2);
        CHECK(highs[1].node == extra);
        CHECK(findRow(groups, "bolts"));
        delete mime;
    }

    SUBCASE("a dropped group brings its meshes, a dropped mesh comes alone")
    {
        const std::string nut = addPlane("nut", 2.0);
        const std::string washer = addPlane("washer", 2.5);
        ops::execute("parent", {{"args", {washer, nut}}});
        ops::execute("group", {{"args", {extra}}, {"name", "Hardware"}});
        pump();
        auto dropOn = [&](const std::string& name) {
            QTreeWidgetItem* lowRow = findRow(groups, "Low");
            REQUIRE(lowRow);
            auto* mime = new QMimeData;
            mime->setData(ui::kNodeNamesMime, QByteArray::fromStdString(name));
            const QPoint at = groups->visualItemRect(lowRow).center();
            QDragEnterEvent enter(at, Qt::LinkAction | Qt::MoveAction, mime, Qt::LeftButton, Qt::NoModifier);
            QApplication::sendEvent(groups->viewport(), &enter);
            QDragMoveEvent move(at, Qt::LinkAction | Qt::MoveAction, mime, Qt::LeftButton, Qt::NoModifier);
            QApplication::sendEvent(groups->viewport(), &move);
            QDropEvent drop(QPointF(at), Qt::LinkAction | Qt::MoveAction, mime, Qt::LeftButton, Qt::NoModifier);
            QApplication::sendEvent(groups->viewport(), &drop);
            pump();
            delete mime;
        };
        auto lows = [&] {
            std::set<std::string> out;
            for (const std::string& l : bake::groupSettings(*onlyGroup(panel.project())).lows)
                out.insert(scene::Graph::current().find(l)->name());
            return out;
        };
        dropOn("nut");
        CHECK(lows() == std::set<std::string>{"crate_low", "nut"});
        dropOn("Hardware");
        CHECK(lows() == std::set<std::string>{"crate_low", "nut", "bolts"});
    }

    SUBCASE("a high's bake-only stack comes from the preset combo")
    {
        QTreeWidgetItem* row = findRow(groups, "crate_high");
        REQUIRE(row);
        groups->setCurrentItem(row);
        pump();
        auto* preset = child<QComboBox>(&panel, "bakerPreset");
        const int i = preset->findData("Smooth x2");
        REQUIRE(i > 0);
        Q_EMIT preset->activated(i);
        pump();
        grp = onlyGroup(panel.project());
        CHECK(bake::groupSettings(*grp).highs[0].modifiers == bake::modifierPreset("Smooth x2"));
        CHECK(findRow(groups, "crate_high")->text(1) == "subdivision 2");
        // One undo step per edit.
        REQUIRE(UndoStack::get().undo());
        pump();
        CHECK(bake::groupSettings(*onlyGroup(panel.project())).highs[0].modifiers.empty());
    }

    SUBCASE("the name template previews live and commits through setBakeOutput; bake fills the thumbnails")
    {
        const fs::path dir = fs::temp_directory_path() / "lila_baker_panel";
        fs::remove_all(dir);
        ops::execute("setBakeOutput", {{"project", panel.project()}, {"folder", dir.generic_string()}, {"resolution", 24}, {"supersampling", 1}});
        ops::execute("bakeGroup", {{"args", {grp->path()}}, {"frontal", 3.0}, {"rear", 3.0}});
        pump();
        CHECK(child<QComboBox>(&panel, "bakerResolution")->currentText() == "24 x 24");
        CHECK(child<QLabel>(&panel, "bakerFolderResolved")->text() == QString::fromStdString(dir.generic_string()));

        auto* autoAssign = child<QCheckBox>(&panel, "bakerAutoAssign");
        CHECK_FALSE(autoAssign->isChecked());
        autoAssign->click();
        pump();
        CHECK(bake::projectSettings(*scene::Graph::current().find(panel.project())).autoAssign);
        autoAssign->click();
        pump();
        CHECK_FALSE(bake::projectSettings(*scene::Graph::current().find(panel.project())).autoAssign);
        child<QCheckBox>(&panel, "bakerRebakeOnSave")->click();
        pump();
        CHECK(bake::projectSettings(*scene::Graph::current().find(panel.project())).rebakeOnSave);
        ops::execute("setBakeProject", {{"project", panel.project()}, {"rebakeOnSave", false}});

        auto* name = child<QLineEdit>(&panel, "bakerName");
        name->setText("T_{mesh}_{map}");
        Q_EMIT name->textEdited(name->text());
        {
            auto* preview = child<QTreeWidget>(&panel, "bakerPreview");
            bool listed = false;
            for (int r = 0; r < preview->topLevelItemCount(); ++r)
                for (int c = 0; c < preview->topLevelItem(r)->childCount(); ++c)
                    listed |= preview->topLevelItem(r)->child(c)->text(0) == "T_crate_low_normal.png";
            CHECK(listed);
        }
        Q_EMIT name->editingFinished();
        pump();
        CHECK(bake::projectSettings(*scene::Graph::current().find(panel.project())).output.name == "T_{mesh}_{map}");

        auto* maps = child<QTreeWidget>(&panel, "bakerMaps");
        QTreeWidgetItem* ao = nullptr;
        for (int r = 0; r < maps->topLevelItemCount(); ++r)
            if (maps->topLevelItem(r)->data(0, Qt::UserRole + 2).toString() == "ao")
                ao = maps->topLevelItem(r);
        REQUIRE(ao);
        ao->setCheckState(0, Qt::Unchecked);
        pump();
        for (const bake::MapSettings& m : bake::projectSettings(*scene::Graph::current().find(panel.project())).maps)
            if (m.map == "ao")
                CHECK_FALSE(m.enabled);

        // The panel bakes off the main thread: thumbnails fill in as the files land, then the result shows.
        child<QToolButton>(&panel, "bakerBake")->click();
        QElapsedTimer t;
        t.start();
        auto summaryRow = [&](const QString& name) -> QTreeWidgetItem* {
            auto* tree = child<QTreeWidget>(&panel, "bakerSummary");
            for (int r = 0; r < tree->topLevelItemCount(); ++r)
                if (tree->topLevelItem(r)->text(0) == name)
                    return tree->topLevelItem(r);
            return nullptr;
        };
        do
            pump();
        while ((bake::bakeRunning() || panel.thumbnailsPending() || !summaryRow("Time")) &&
               t.elapsed() < 20000);
        auto* thumbs = child<QListWidget>(&panel, "bakerThumbs");
        REQUIRE(thumbs->count() >= 3);
        CHECK_FALSE(panel.thumbnailsPending());
        bool sawNormal = false;
        for (int r = 0; r < thumbs->count(); ++r)
            if (thumbs->item(r)->text().startsWith("normal\nT_crate_low_normal"))
                sawNormal = true;
        CHECK(sawNormal);
        CHECK_FALSE(thumbs->item(0)->icon().isNull());
        REQUIRE(summaryRow("Files"));
        CHECK(summaryRow("Files")->text(1).toInt() >= 3);
        REQUIRE(summaryRow("Folder"));
        CHECK(summaryRow("Folder")->text(1) == QString::fromStdString(dir.generic_string()));

        // Assign wires the maps that have a material input into the low's own new material.
        child<QToolButton>(&panel, "bakerAssign")->click();
        pump();
        scene::Node* own = scene::materials::find(scene::Graph::current(), "M_crate_low");
        REQUIRE(own);
        CHECK(!own->as<std::string>(scene::material::kNormalMap).empty());
        CHECK(panel.outputFolder() == QString::fromStdString(dir.generic_string()));

        // The thumbnails' menu acts on the selection: paths and names copy one per line.
        thumbs->clearSelection();
        thumbs->item(0)->setSelected(true);
        thumbs->item(1)->setSelected(true);
        std::unique_ptr<QMenu> menu(panel.thumbnailMenu(&panel));
        auto action = [&](const QString& text) {
            for (QAction* a : menu->actions())
                if (a->text() == text)
                    return a;
            FAIL("no menu action " << text.toStdString());
            return static_cast<QAction*>(nullptr);
        };
        CHECK(action("Open All")->isEnabled());
        CHECK(action("Open Containing Folder")->isEnabled());
        CHECK(action("Open Output Folder")->isEnabled());
        action("Copy Paths")->trigger();
        const QStringList paths = QApplication::clipboard()->text().split('\n');
        REQUIRE(paths.size() == 2);
        CHECK(paths[0] == QDir::toNativeSeparators(thumbs->item(0)->data(Qt::UserRole + 3).toString()));
        action("Copy File Names")->trigger();
        CHECK(QApplication::clipboard()->text().split('\n')[1] == QFileInfo(thumbs->item(1)->data(Qt::UserRole + 3).toString()).fileName());
        thumbs->clearSelection();
        thumbs->setCurrentItem(thumbs->item(0));
        menu.reset(panel.thumbnailMenu(&panel));
        action("Copy File Name")->trigger();
        CHECK(QApplication::clipboard()->text().startsWith("T_crate_low_"));

        const json last = bake::lastResult(scene::Graph::current().find(panel.project())->id());
        const QImage sheet = ui::bakeContactSheet(last, 32, 2);
        CHECK(sheet.width() == 2 * (32 + 4) + 4);
        CHECK_FALSE(ui::bakeThumbnail(fs::path(last["outputs"][0]["file"].get<std::string>()), 16).isNull());
        fs::remove_all(dir);
    }

    SUBCASE("lint findings sit under their group")
    {
        REQUIRE(ops::find("bakeLint"));
        // The high far above the rays' reach: the full check (the toolbar button) reports the low's misses.
        ops::execute("move", {{"args", {high}}, {"to", {0.0, 500.0, 0.0}}});
        child<QToolButton>(&panel, "bakerLint")->click();
        pump();
        QTreeWidgetItem* groupRow = findRow(groups, QString::fromStdString(grp->name()));
        REQUIRE(groupRow);
        // Issues -> one row per check -> its meshes.
        bool found = false;
        for (int c = 0; c < groupRow->childCount(); ++c)
            if (QTreeWidgetItem* issues = groupRow->child(c); issues->data(0, Qt::UserRole).toInt() == 7 && issues->text(0) == "Issues")
                for (int k = 0; k < issues->childCount(); ++k)
                    found |= issues->child(k)->text(0) == "Rays missing the high";
        CHECK(found);
    }
}

TEST_CASE("baker panel: presets stick on group and High rows, typed params edit the stack, Delete removes only tree rows")
{
    setupBaker();
    REQUIRE(scene::types::find("autoBevelModifier"));
    ops::execute("newScene", json::object());
    UndoStack::get().clear();
    const std::string low = addPlane("crate_low", 0.0);
    const std::string high = addPlane("crate_high", 1.0);
    const std::string bolts = addPlane("bolts", 1.5);

    ui::BakerPanel panel;
    panel.resize(420, 900);
    panel.show();
    pump();
    child<QToolButton>(&panel, "bakerAutoGroup")->click();
    pump();
    REQUIRE_FALSE(panel.project().empty());
    const std::string group = onlyGroup(panel.project())->path();
    const QString groupRow = QString::fromStdString(onlyGroup(panel.project())->name());
    ops::execute("bakeGroup", {{"args", {group}}, {"addHigh", {bolts}}});
    pump();

    auto* groups = child<QTreeWidget>(&panel, "bakerGroups");
    auto* preset = child<QComboBox>(&panel, "bakerPreset");
    auto* stack = child<QListWidget>(&panel, "bakerStack");
    auto highs = [&] { return bake::groupSettings(*onlyGroup(panel.project())).highs; };
    auto pick = [&](const char* name) {
        const int i = preset->findData(name);
        REQUIRE(i > 0);
        Q_EMIT preset->activated(i);
        pump();
    };
    auto select = [&](const QString& text) {
        QTreeWidgetItem* it = findRow(groups, text);
        REQUIRE(it);
        groups->setCurrentItem(it);
        pump();
    };

    SUBCASE("the group row applies a preset to every high, keeps showing it, and edits Auto Bevel's params for all")
    {
        select(groupRow);
        pick("Auto Bevel + Smooth");
        const json applied = bake::modifierPreset("Auto Bevel + Smooth");
        REQUIRE(highs().size() == 2);
        for (const bake::HighEntry& h : highs())
            CHECK(h.modifiers == applied);
        CHECK(preset->currentData().toString() == "Auto Bevel + Smooth");
        CHECK_FALSE(stack->isHidden());
        REQUIRE(stack->count() == 2);
        stack->setCurrentRow(0);
        pump();

        auto* offset = child<QDoubleSpinBox>(&panel, "bakerParam_offset");
        auto* segments = child<QDoubleSpinBox>(&panel, "bakerParam_segments");
        child<QDoubleSpinBox>(&panel, "bakerParam_minAngle");
        child<QComboBox>(&panel, "bakerParam_mode");
        CHECK_FALSE(panel.findChild<QWidget*>("bakerParam_enabled"));
        CHECK(offset->value() == doctest::Approx(1.0));

        offset->setValue(2.5);
        pump();
        for (const bake::HighEntry& h : highs())
        {
            CHECK(h.modifiers[0]["params"]["offset"] == 2.5);
            CHECK(h.modifiers[1] == applied[1]);
        }
        CHECK(preset->currentText() == "Custom");
        // The fields survive the refresh their own edit causes.
        CHECK(child<QDoubleSpinBox>(&panel, "bakerParam_offset") == offset);
        CHECK(offset->value() == doctest::Approx(2.5));

        segments->setValue(5);
        pump();
        CHECK(highs()[1].modifiers[0]["params"]["segments"] == 5);

        // One undo step per edit, and the fields follow.
        REQUIRE(UndoStack::get().undo());
        pump();
        CHECK_FALSE(highs()[0].modifiers[0]["params"].contains("segments"));
        CHECK(segments->value() == 2);
        REQUIRE(UndoStack::get().undo());
        pump();
        CHECK(highs()[0].modifiers == applied);
        CHECK(preset->currentData().toString() == "Auto Bevel + Smooth");
    }

    SUBCASE("a High row keeps its preset, matches stacks by meaning, and the group row then reads Mixed")
    {
        select("crate_high");
        pick("Smooth x3");
        CHECK(highs()[0].modifiers == bake::modifierPreset("Smooth x3"));
        CHECK(highs()[1].modifiers.empty());
        REQUIRE(groups->currentItem());
        CHECK(groups->currentItem()->text(0) == "crate_high");
        CHECK(preset->currentData().toString() == "Smooth x3");

        // A bare kind is Smooth x1 (levels defaults to 1); a float level still matches its preset.
        ops::execute("bakeGroup", {{"args", {group}}, {"addHigh", {high}}, {"modifiers", json::array({"subdivision"})}});
        pump();
        CHECK(preset->currentData().toString() == "Smooth x1");
        ops::execute("bakeGroup", {{"args", {group}}, {"addHigh", {high}}, {"modifiers", json::array({json{{"kind", "subdivision"}, {"params", {{"levels", 2.0}}}}})}});
        pump();
        CHECK(preset->currentData().toString() == "Smooth x2");

        stack->setCurrentRow(0);
        pump();
        child<QDoubleSpinBox>(&panel, "bakerParam_levels")->setValue(4);
        pump();
        CHECK(highs()[0].modifiers[0]["params"]["levels"] == 4);
        CHECK(highs()[1].modifiers.empty());
        CHECK(preset->currentData().toString() == "Smooth x4");

        select(groupRow);
        CHECK(preset->currentText() == "Mixed");
        CHECK(stack->isHidden());
        pick("None");
        CHECK(highs()[0].modifiers.empty());
        CHECK(preset->currentData().toString() == "None");
        CHECK_FALSE(stack->isHidden());
    }

    SUBCASE("the inline Save Settings as Preset row saves a target that applies when picked in the target combo")
    {
        ops::execute("setBakeOutput", {{"project", panel.project()}, {"resolution", 256}});
        auto* name = child<QLineEdit>(&panel, "bakerPresetName");
        name->setText("PanelPreset");
        child<QToolButton>(&panel, "bakerPresetSave")->click();
        pump();
        const auto saved = bake::findTarget("PanelPreset");
        REQUIRE(saved);
        CHECK(saved->preset);
        auto* targets = child<QComboBox>(&panel, "bakerTargets");
        const int at = targets->findData("PanelPreset");
        REQUIRE(at >= 0);
        CHECK(targets->currentIndex() == at);

        ops::execute("setBakeOutput", {{"project", panel.project()}, {"resolution", 64}});
        targets->setCurrentIndex(at == 0 ? 1 : 0);
        pump();
        CHECK(bake::projectSettings(*scene::Graph::current().find(panel.project())).output.width == 64);
        targets->setCurrentIndex(targets->findData("PanelPreset"));
        pump();
        CHECK(bake::projectSettings(*scene::Graph::current().find(panel.project())).output.width == 256);
        ops::execute("deleteBakeTarget", {{"args", {"PanelPreset"}}});
    }

    SUBCASE("Delete in the Maps tab removes the selected maps from the project in one undo step")
    {
        auto* maps = child<QTreeWidget>(&panel, "bakerMaps");
        auto mapIds = [&] {
            std::set<std::string> out;
            for (const bake::MapSettings& m : bake::projectSettings(*scene::Graph::current().find(panel.project())).maps)
                out.insert(m.map);
            return out;
        };
        const std::set<std::string> before = mapIds();
        REQUIRE(before.contains("ao"));
        REQUIRE(before.contains("normal"));
        maps->clearSelection();
        for (int r = 0; r < maps->topLevelItemCount(); ++r)
            if (const QString id = maps->topLevelItem(r)->data(0, Qt::UserRole + 2).toString(); id == "ao" || id == "normal")
                maps->topLevelItem(r)->setSelected(true);

        CHECK(ui::deletion::route(maps, groups).target == ui::deletion::Target::Panel);
        ui::deletion::run(maps, groups, [] { FAIL("Delete in the Maps tab reached the scene"); });
        pump();
        std::set<std::string> after = mapIds();
        CHECK(after.size() == before.size() - 2);
        CHECK_FALSE(after.contains("ao"));
        CHECK_FALSE(after.contains("normal"));
        for (const std::string& m : {low, high, bolts})
            CHECK(scene::Graph::current().find(m));
        REQUIRE(UndoStack::get().undo());
        pump();
        CHECK(mapIds() == before);

        CHECK(ui::deletion::route(child<QListWidget>(&panel, "bakerThumbs"), nullptr).target == ui::deletion::Target::Nothing);
        CHECK(ui::deletion::route(child<QListWidget>(&panel, "bakerStack"), nullptr).target == ui::deletion::Target::Panel);
    }

    SUBCASE("Delete in the tree removes the tree's rows from the group, never the scene meshes")
    {
        CHECK(ui::deletion::route(groups, nullptr).target == ui::deletion::Target::Panel);
        CHECK(groups->property("hotkeyContext").toString() == "baker");
        ops::execute("select", {{"args", {low}}});
        select("bolts");
        findRow(groups, "crate_low")->setSelected(true);

        QKeyEvent over(QEvent::ShortcutOverride, Qt::Key_Delete, Qt::NoModifier);
        over.ignore();
        QApplication::sendEvent(groups, &over);
        CHECK(over.isAccepted());
        QKeyEvent del(QEvent::KeyPress, Qt::Key_Delete, Qt::NoModifier);
        QApplication::sendEvent(groups, &del);
        pump();

        const bake::GroupSettings s = bake::groupSettings(*onlyGroup(panel.project()));
        CHECK(s.lows.empty());
        REQUIRE(s.highs.size() == 1);
        CHECK(s.highs[0].node == high);
        scene::Graph& g = scene::Graph::current();
        for (const std::string& m : {low, high, bolts})
            CHECK(g.find(m));
        REQUIRE(g.selection().size() == 1);
        CHECK(g.node(g.selection()[0])->path() == low);

        // A group row takes the group with it, Backspace too, and the meshes stay; one undo brings it back.
        select(groupRow);
        QKeyEvent back(QEvent::KeyPress, Qt::Key_Backspace, Qt::NoModifier);
        QApplication::sendEvent(groups, &back);
        pump();
        const scene::NodeId project = g.find(panel.project())->id();
        CHECK(bake::groupsOf(g, project).empty());
        for (const std::string& m : {low, high, bolts})
            CHECK(g.find(m));
        REQUIRE(UndoStack::get().undo());
        pump();
        CHECK(bake::groupsOf(g, project).size() == 1);
    }
}

TEST_CASE("baker panel: Unlink Material lists the highs' materials, adds one under the group's lows and Remove takes it out")
{
    setupBaker();
    ops::execute("newScene", json::object());
    UndoStack::get().clear();
    addPlane("crate_low", 0.0);
    addPlane("crate_high", 1.0);
    const std::string bolts = addPlane("bolts", 1.5);
    ops::execute("createMaterial", {{"args", {"M_Bolts"}}});
    ops::execute("assignMaterial", {{"args", {"M_Bolts", bolts}}});

    ui::BakerPanel panel;
    panel.resize(420, 900);
    panel.show();
    pump();
    child<QToolButton>(&panel, "bakerAutoGroup")->click();
    pump();
    REQUIRE_FALSE(panel.project().empty());
    const std::string group = onlyGroup(panel.project())->path();
    ops::execute("bakeGroup", {{"args", {group}}, {"addHigh", {bolts}}});
    pump();
    auto* groups = child<QTreeWidget>(&panel, "bakerGroups");
    groups->setCurrentItem(findRow(groups, QString::fromStdString(onlyGroup(panel.project())->name())));
    pump();

    QMenu* menu = child<QToolButton>(&panel, "bakerUnlinkMaterial")->menu();
    REQUIRE(menu);
    Q_EMIT menu->aboutToShow();
    QAction* pickBolts = nullptr;
    for (QAction* a : menu->actions())
        if (a->text() == "M_Bolts")
            pickBolts = a;
    REQUIRE(pickBolts);
    pickBolts->trigger();
    pump();
    auto unlinks = [&] { return bake::groupSettings(*onlyGroup(panel.project())).unlinks; };
    REQUIRE(unlinks().size() == 1);
    CHECK(unlinks()[0].lows.empty());
    CHECK(unlinks()[0].meshes.empty());
    CHECK(unlinks()[0].materials == std::vector<std::string>{"M_Bolts"});

    auto* tree = child<QTreeWidget>(&panel, "bakerUnlinks");
    QTreeWidgetItem* row = findRow(tree, "M_Bolts");
    REQUIRE(row);
    REQUIRE(row->parent());
    CHECK(row->parent()->text(0) == "All lows");
    row->setSelected(true);
    child<QToolButton>(&panel, "bakerUnlinkRemove")->click();
    pump();
    CHECK(unlinks().empty());
}

TEST_CASE("baker panel: an occlusion result under sculpt lighting gets a Lighting row and a button back to environment lighting")
{
    setupBaker();
    ops::execute("newScene", json::object());
    PrefStore& prefs = PrefStore::get();
    const std::string before = prefs.value<std::string>("general", "/viewport/lighting", "environment");

    ui::BakerPanel panel;
    panel.resize(420, 900);
    panel.show();
    pump();
    auto* tree = child<QTreeWidget>(&panel, "bakerSummary");
    // Hidden or not, whichever tab is in front.
    auto* button = child<QToolButton>(&panel, "bakerEnvironmentLighting");
    const json occlusion = {{"outputs", {{{"map", "ao"}}}}, {"stats", json::object()}};
    const json normalOnly = {{"outputs", {{{"map", "normal"}}}}, {"stats", json::object()}};

    prefs.set("general", "/viewport/lighting", std::string("environment"));
    panel.showResult(occlusion);
    pump();
    CHECK_FALSE(findRow(tree, "Lighting"));
    CHECK(button->isHidden());

    // The row follows the lighting, not only the bake.
    prefs.set("general", "/viewport/lighting", std::string("sculpt"));
    pump();
    REQUIRE(findRow(tree, "Lighting"));
    CHECK(findRow(tree, "Lighting")->text(1) == "Sculpt");
    CHECK_FALSE(button->isHidden());

    panel.showResult(normalOnly);
    pump();
    CHECK_FALSE(findRow(tree, "Lighting"));
    CHECK(button->isHidden());

    panel.showResult(occlusion);
    pump();
    REQUIRE(findRow(tree, "Lighting"));
    button->click();
    pump();
    CHECK(prefs.value<std::string>("general", "/viewport/lighting", "") == "environment");
    CHECK_FALSE(findRow(tree, "Lighting"));
    CHECK(button->isHidden());

    prefs.set("general", "/viewport/lighting", before);
}

TEST_CASE("baker panel: a low shown without a modifier its export runs gets a Preview row and Show Export Mesh")
{
    setupBaker();
    ops::execute("newScene", json::object());
    UndoStack::get().clear();
    const std::string low = addPlane("crate_low", 0.0);
    addPlane("crate_high", 1.0);
    ops::execute("addModifier", {{"args", {low}}, {"kind", "subdivision"}});
    const std::string mod = ops::execute("modifierStack", {{"args", {low}}})["modifiers"].back()["name"];
    scene::Graph& g = scene::Graph::current();
    auto enabled = [&] { return std::get<bool>(g.value({g.find(mod)->id(), g.find(mod)->type().attrIndex("enabled"), -1})); };

    ui::BakerPanel panel;
    panel.resize(420, 900);
    panel.show();
    pump();
    child<QToolButton>(&panel, "bakerAutoGroup")->click();
    pump();
    REQUIRE_FALSE(panel.project().empty());
    auto* tree = child<QTreeWidget>(&panel, "bakerSummary");
    // Hidden or not, whichever tab is in front.
    auto* button = child<QToolButton>(&panel, "bakerShowExportMesh");
    const json baked = {{"outputs", {{{"map", "normal"}}}}, {"stats", json::object()}};

    panel.showResult(baked);
    pump();
    CHECK_FALSE(findRow(tree, "Preview"));
    CHECK(button->isHidden());

    // The row follows the stack, not only the bake.
    ops::execute("setAttr", {{"args", {mod + ".enabled", false}}});
    pump();
    REQUIRE(findRow(tree, "Preview"));
    CHECK(findRow(tree, "Preview")->text(1) == "Cage, 1 mesh");
    CHECK_FALSE(button->isHidden());

    // A planned bake has no maps to preview.
    panel.showResult({{"outputs", json::array()}, {"stats", json::object()}, {"dryRun", true}});
    pump();
    CHECK_FALSE(findRow(tree, "Preview"));
    CHECK(button->isHidden());

    panel.showResult(baked);
    pump();
    REQUIRE(findRow(tree, "Preview"));
    button->click();
    pump();
    CHECK(enabled());
    CHECK_FALSE(findRow(tree, "Preview"));
    CHECK(button->isHidden());
    REQUIRE(UndoStack::get().undo());
    pump();
    CHECK_FALSE(enabled());
    CHECK(findRow(tree, "Preview"));
}

TEST_CASE("baker panel: a result whose snapshots overflowed the cache leads with a Cache row in the error colour and Raise Cache")
{
    setupBaker();
    ops::execute("newScene", json::object());
    PrefStore& prefs = PrefStore::get();
    const double before = prefs.value<double>("baker", "/snapshotCacheGb", 0.0);

    ui::BakerPanel panel;
    panel.resize(420, 900);
    panel.show();
    pump();
    auto* tree = child<QTreeWidget>(&panel, "bakerSummary");
    // Hidden or not, whichever tab is in front.
    auto* button = child<QToolButton>(&panel, "bakerRaiseCache");
    const double gb = double(size_t(1) << 30);
    auto result = [&](double dropped) {
        json timings = json::object();
        if (dropped > 0.0)
            timings = {{"snapshotCacheDropped", dropped}, {"snapshotCacheBudget", 3.0 * gb}};
        return json{{"outputs", {{{"map", "normal"}}}}, {"stats", json::object()}, {"timings", timings}, {"seconds", dropped}};
    };

    panel.showResult(result(0.0));
    pump();
    CHECK_FALSE(findRow(tree, "Cache"));
    CHECK(button->isHidden());

    panel.showResult(result(1.0 * gb));
    pump();
    QTreeWidgetItem* row = tree->topLevelItem(0);
    REQUIRE(row);
    CHECK(row->text(0) == "Cache");
    CHECK(row->text(1) == "Full: 1.0 GB of meshes redone every bake");
    CHECK(row->foreground(1).color() == ui::themeColor("statusError"));
    CHECK(row->font(1).bold());
    CHECK_FALSE(button->isHidden());

    // Room for what it held plus what it dropped, and a quarter more.
    button->click();
    pump();
    CHECK(prefs.value<double>("baker", "/snapshotCacheGb", 0.0) == doctest::Approx(5.0));
    CHECK(findRow(tree, "Cache")->text(1) == "Raised to 5 GB, from the next bake");
    CHECK(button->isHidden());

    // More than the machine could spare: the row stays, with nothing to press.
    panel.showResult(result(4096.0 * gb));
    pump();
    REQUIRE(findRow(tree, "Cache"));
    CHECK(button->isHidden());

    prefs.set("baker", "/snapshotCacheGb", before);
}

TEST_CASE("baker actions and menu are registered with docs")
{
    setupBaker();
    for (const char* id : {"window.baker", "bake.forTarget", "bake.bake", "bake.bakeSelected", "bake.cancel", "bake.newProject", "bake.autoGroup",
                           "bake.groupPerMesh", "bake.lint", "bake.removeRows"})
    {
        const ui::Action* a = ui::actions::find(id);
        REQUIRE_MESSAGE(a, id);
        CHECK_FALSE(a->doc.empty());
    }
    CHECK(ui::menus::itemsIn("Bake").size() >= 8);
    CHECK(ui::panels::registry().find(ui::kBakerDock));
}

TEST_CASE("baker MCP tools: bake returns a contact sheet, bake_inspect reads texels, bake_lint reports")
{
    setupBaker();
    ui::registerBakerMcpTools();
    ops::execute("newScene", json::object());
    UndoStack::get().clear();
    addPlane("tile_low", 0.0);
    addPlane("tile_high", 1.0);
    const fs::path dir = fs::temp_directory_path() / "lila_baker_mcp";
    fs::remove_all(dir);
    ops::execute("createBakeProject", {{"name", "P"}, {"folder", dir.generic_string()}});
    ops::execute("setBakeOutput", {{"resolution", 16}, {"supersampling", 1}});
    ops::execute("autoGroupBake", {{"args", {"tile_low", "tile_high"}}});
    ops::execute("bakeGroup", {{"args", {"tile_bake"}}, {"frontal", 3.0}, {"rear", 3.0}});

    const json lint = callTool("bake_lint", json::object());
    REQUIRE_FALSE(lint.value("isError", true));
    CHECK(lint["content"][0]["text"].get<std::string>().find("DRY RUN") != std::string::npos);

    const json baked = callTool("bake", {{"maps", {"normal", "height"}}, {"tile", 48}});
    REQUIRE_FALSE(baked.value("isError", true));
    REQUIRE(baked["content"].size() == 2);
    CHECK(baked["content"][0]["text"].get<std::string>().find("missed (0.00%)") != std::string::npos);
    CHECK(baked["content"][1]["type"] == "image");

    const json inspect = callTool("bake_inspect", {{"map", "normal"}, {"uv", {0.5, 0.5}}, {"image", true}, {"size", 64}});
    REQUIRE_FALSE(inspect.value("isError", true));
    const std::string text = inspect["content"][0]["text"].get<std::string>();
    INFO(text);
    CHECK(text.find("texel [8, 8]") != std::string::npos);
    CHECK(text.find("8-bit [128, 128, 255") != std::string::npos);
    CHECK(inspect["content"].back()["type"] == "image");
    fs::remove_all(dir);
}
