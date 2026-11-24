#include <doctest/doctest.h>

#include "ops/Command.h"
#include "ops/CommandOptions.h"
#include "ops/OptionPresets.h"
#include "scene/Plug.h"
#include "ui/Actions.h"
#include "ui/ChoiceTips.h"
#include "ui/Controls.h"
#include "ui/FlagFields.h"
#include "ui/OptionBox.h"

#include <QApplication>
#include <QComboBox>
#include <QDialog>
#include <QDoubleSpinBox>
#include <QLabel>
#include <QMenu>
#include <QMouseEvent>
#include <QPushButton>
#include <QStyleFactory>
#include <QToolTip>

#include <cstdlib>

using namespace lila;

namespace
{
int probeRuns = 0;

QDialog* optionsDialog()
{
    for (QWidget* w : QApplication::topLevelWidgets())
        if (auto* d = qobject_cast<QDialog*>(w); d && d->isVisible() && d->windowTitle() == "Box Probe Options")
            return d;
    return nullptr;
}

void clickMenu(QMenu& menu, QPoint at, Qt::KeyboardModifiers mods)
{
    QMouseEvent press(QEvent::MouseButtonPress, at, menu.mapToGlobal(at), Qt::LeftButton, Qt::LeftButton, mods);
    QApplication::sendEvent(&menu, &press);
    QMouseEvent release(QEvent::MouseButtonRelease, at, menu.mapToGlobal(at), Qt::LeftButton, Qt::NoButton, mods);
    QApplication::sendEvent(&menu, &release);
}
} // namespace

TEST_CASE("menu rows of commands with flags get an option box that opens the generated dialog")
{
    QApplication::setStyle(QStyleFactory::create("Fusion"));
    if (!ops::find("boxProbe"))
        ops::registerCommand({.name = "boxProbe",
                               .doc = "Probe.",
                               .flags = {{"segments", "", ArgType::Int, 2, "How many."}, {"mode", "", ArgType::String, "a", "a | b"}},
                               .undoable = false,
                               .run = [](const Args&) -> json {
                                   ++probeRuns;
                                   return nullptr;
                               }});
    ui::actions::add({.id = "test.boxProbe", .label = "Box Probe", .category = "Mesh", .doc = "Runs the probe.", .command = "boxProbe"});
    ui::actions::add({.id = "test.noBox", .label = "No Box", .category = "Mesh", .run = [] {}, .doc = "Plain action."});
    CHECK(ui::optionBox::available("test.boxProbe"));
    CHECK_FALSE(ui::optionBox::available("test.noBox"));

    QMenu menu;
    for (const char* id : {"test.noBox", "test.boxProbe"})
        menu.addAction(QString::fromStdString(ui::actions::find(id)->label))->setData(QString(id));
    ui::optionBox::decorateMenu(&menu);
    menu.popup(QPoint(100, 100));
    QApplication::processEvents();
    QAction* row = menu.actions()[1];
    CHECK(row->text().contains('\t'));
    CHECK_FALSE(menu.actions()[0]->text().contains('\t'));

    if (const char* shot = std::getenv("LILA_OPTIONBOX_SHOT"))
        menu.grab().save(QString::fromLocal8Bit(shot));

    ops::options::reset("boxProbe");
    probeRuns = 0;
    CHECK(ui::actions::run(json("test.boxProbe")));
    CHECK(probeRuns == 1);

    // Shift+click opens the options instead of running.
    QRect r = menu.actionGeometry(row);
    clickMenu(menu, QPoint(r.left() + 20, r.center().y()), Qt::ShiftModifier);
    QApplication::processEvents();
    QDialog* d = optionsDialog();
    REQUIRE(d);
    CHECK(probeRuns == 1);
    if (const char* shot = std::getenv("LILA_OPTIONBOX_DIALOG_SHOT"))
        d->grab().save(QString::fromLocal8Bit(shot));
    d->close();
    QApplication::processEvents();

    // So does a click on the square at the row's right end.
    menu.popup(QPoint(100, 100));
    QApplication::processEvents();
    r = menu.actionGeometry(row);
    clickMenu(menu, QPoint(r.right() - 22, r.center().y()), Qt::NoModifier);
    QApplication::processEvents();
    CHECK(optionsDialog());
    CHECK(probeRuns == 1);
    if (QDialog* again = optionsDialog())
        again->close();
    QApplication::processEvents();
}

TEST_CASE("Apply in an action's options runs the action itself, and panel buttons open options on Shift")
{
    if (!ops::find("boxProbe"))
        ops::registerCommand({.name = "boxProbe",
                               .doc = "Probe.",
                               .flags = {{"segments", "", ArgType::Int, 2, "How many."}, {"mode", "", ArgType::String, "a", "a | b"}},
                               .undoable = false,
                               .run = [](const Args&) -> json {
                                   ++probeRuns;
                                   return nullptr;
                               }});
    int lambdaRuns = 0;
    ui::actions::add({.id = "test.boxGather",
                      .label = "Box Gather",
                      .category = "Mesh",
                      .run = [&lambdaRuns] { ++lambdaRuns; },
                      .doc = "Gathers its own arguments.",
                      .command = "boxProbe"});
    CHECK(ui::optionBox::available("test.boxGather"));
    ui::optionBox::open("test.boxGather");
    QApplication::processEvents();
    QDialog* d = nullptr;
    for (QWidget* w : QApplication::topLevelWidgets())
        if (auto* dlg = qobject_cast<QDialog*>(w); dlg && dlg->isVisible() && dlg->windowTitle() == "Box Gather Options")
            d = dlg;
    REQUIRE(d);
    QPushButton* apply = nullptr;
    for (QPushButton* b : d->findChildren<QPushButton*>())
        if (b->text() == "Apply")
            apply = b;
    REQUIRE(apply);
    apply->click();
    CHECK(lambdaRuns == 1);
    d->close();
    QApplication::processEvents();
    ui::actions::remove("test.boxGather");

    // Without Shift a panel button runs; select has no settings worth a dialog either way.
    CHECK_FALSE(ui::optionBox::shiftOpens("boxProbe", json::object()));
    CHECK_FALSE(ui::optionBox::openCommand("Select", "select", json::object()));
    CHECK(ui::optionBox::openCommand("Box Probe", "boxProbe", json::object()));
    QApplication::processEvents();
    for (QWidget* w : QApplication::topLevelWidgets())
        if (auto* dlg = qobject_cast<QDialog*>(w); dlg && dlg->isVisible())
            dlg->close();
    QApplication::processEvents();
}

TEST_CASE("choice fields give each option its own tip, and the closed combo the current one's")
{
    if (!ops::find("tipProbe"))
        ops::registerCommand({.name = "tipProbe",
                               .doc = "Probe.",
                               .flags = {{"mode", "", ArgType::String, "slerp", "slerp: constant speed. smooth: cubic.", {"slerp", "smooth"}}},
                               .undoable = false,
                               .run = [](const Args&) -> json { return nullptr; }});
    std::vector<ops::options::Field> fields = ops::options::fields("tipProbe");
    REQUIRE(fields.size() == 1);
    QWidget parent;
    ui::FlagFieldWidgets w = ui::makeFlagField(fields[0], "slerp", [](const json&) {}, &parent);
    auto* combo = qobject_cast<QComboBox*>(w.field);
    REQUIRE(combo);
    CHECK(combo->itemData(0, Qt::ToolTipRole).toString() == "Constant speed.");
    CHECK(combo->itemData(1, Qt::ToolTipRole).toString() == "Cubic.");
    CHECK(combo->toolTip() == "Constant speed.");
    combo->setCurrentIndex(1);
    CHECK(combo->toolTip() == "Cubic.");
    w.set("slerp");
    CHECK(combo->toolTip() == "Constant speed.");
}

TEST_CASE("a number's scrub label shows the flag's doc, with the scrub and reset hints set apart under it")
{
    if (!ops::find("scrubTipProbe"))
        ops::registerCommand({.name = "scrubTipProbe",
                               .doc = "Probe.",
                               .flags = {{"jointPoles", "", ArgType::Float, 30.0, "Keeps poles out of joints."}},
                               .undoable = false,
                               .run = [](const Args&) -> json { return nullptr; }});
    std::vector<ops::options::Field> fields = ops::options::fields("scrubTipProbe");
    REQUIRE(fields.size() == 1);
    QWidget parent;
    parent.show();
    ui::FlagFieldWidgets w = ui::makeFlagField(fields[0], 30.0, [](const json&) {}, &parent);
    REQUIRE(w.label);
    CHECK(w.label->toolTip() == "jointPoles: Keeps poles out of joints.");
    QHelpEvent help(QEvent::ToolTip, QPoint(1, 1), w.label->mapToGlobal(QPoint(1, 1)));
    QApplication::sendEvent(w.label, &help);
    QString shown = QToolTip::text();
    CHECK(shown.indexOf("jointPoles: Keeps poles out of joints.") >= 0);
    int italic = shown.indexOf("font-style:italic");
    CHECK(italic > shown.indexOf("jointPoles"));
    CHECK(shown.indexOf("Drag sideways to scrub") > italic);
    CHECK(shown.indexOf("Middle-click to reset to default (30") > shown.indexOf("Drag sideways to scrub"));
    QToolTip::hideText();
}

TEST_CASE("a flag's soft range is what its slider covers; typing still reaches the hard range")
{
    FlagDef flag{"subdivisions", "", ArgType::Int, 3, "Segments.", {}, {}, 1.0, 1000.0};
    flag.softMax = 100.0;
    if (!ops::find("softRangeProbe"))
        ops::registerCommand({.name = "softRangeProbe",
                               .doc = "Probe.",
                               .flags = {flag},
                               .undoable = false,
                               .run = [](const Args&) -> json { return nullptr; }});
    std::vector<ops::options::Field> fields = ops::options::fields("softRangeProbe");
    REQUIRE(fields.size() == 1);
    CHECK(fields[0].softMax == 100.0);
    QWidget parent;
    ui::FlagFieldWidgets w = ui::makeFlagField(fields[0], 3, [](const json&) {}, &parent);
    auto* slider = dynamic_cast<ui::controls::SliderField*>(w.field);
    if (!slider && w.field)
        slider = dynamic_cast<ui::controls::SliderField*>(w.field->findChild<QDoubleSpinBox*>());
    REQUIRE(slider);
    CHECK(slider->maximum() == 1000.0);
    CHECK(slider->fillMin() == 1.0);
    CHECK(slider->fillMax() == 100.0);
}

TEST_CASE("an enum attr's choice docs: its own per-name docs first, else what its doc says")
{
    scene::AttrDef def{.name = "mode", .type = scene::PlugType::Enum, .defaultValue = int64_t(0), .enumNames = {"euler", "slerp", "smooth"},
                       .enumDocs = {"", "Arc."}, .doc = "euler (three curves), slerp or smooth (cubic)."};
    CHECK(ui::choiceTips::forAttr(def) == std::vector<std::string>{"Three curves.", "Arc.", "Cubic."});
}

TEST_CASE("presets head the box in one menu with the saved ones, and the menu shows the pick until a field changes")
{
    static json lastCall;
    if (!ops::find("presetProbe"))
        ops::registerCommand({.name = "presetProbe",
                               .doc = "Probe.",
                               .flags = {{"count", "", ArgType::Int, 1, "How many."}, {"scale", "", ArgType::Float, 1.0, "How big."}},
                               .undoable = false,
                               .run = [](const Args& a) -> json {
                                   lastCall = a.flags;
                                   return nullptr;
                               }});
    ops::options::Override o;
    for (int i = 0; i < 4; ++i)
        o.fills.push_back({"Fill " + std::to_string(i), "Sets count to " + std::to_string(10 + i) + ".", [i] { return json{{"count", 10 + i}}; }});
    ops::options::setOverride("presetProbe", o);
    ops::options::reset("presetProbe");
    namespace pr = ops::options::presets;
    for (const pr::Preset& p : pr::list("presetProbe"))
        pr::remove(p);
    REQUIRE(pr::save("presetProbe", "Mine", {{"count", 42}, {"scale", 3.0}}));

    REQUIRE(ui::optionBox::openCommand("Preset Probe", "presetProbe", json::object()));
    QApplication::processEvents();
    QDialog* d = nullptr;
    for (QWidget* w : QApplication::topLevelWidgets())
        if (auto* dlg = qobject_cast<QDialog*>(w); dlg && dlg->isVisible() && dlg->windowTitle() == "Preset Probe Options")
            d = dlg;
    REQUIRE(d);
    auto* menu = d->findChild<QComboBox*>("optionPresets");
    REQUIRE(menu);
    // Above every field.
    for (QLabel* l : d->findChildren<QLabel*>())
        if (l->text() == "Count")
            CHECK(l->mapTo(d, QPoint()).y() > menu->mapTo(d, QPoint()).y());
    CHECK(menu->currentIndex() == -1);
    CHECK(menu->count() == 6); // 4 fills, a separator, Mine
    CHECK(menu->itemData(2, Qt::ToolTipRole).toString() == "Sets count to 12.");
    CHECK(menu->itemText(5) == "Mine");
    CHECK(menu->itemData(5, Qt::ToolTipRole).toString() == "Your preset");
    menu->setCurrentIndex(2);
    Q_EMIT menu->activated(2);
    QApplication::processEvents();
    CHECK(menu->currentIndex() == 2);
    auto apply = [&] {
        for (QPushButton* b : d->findChildren<QPushButton*>())
            if (b->text() == "Apply")
                b->click();
    };
    apply();
    CHECK(lastCall.value("count", 0) == 12);
    menu->setCurrentIndex(5);
    Q_EMIT menu->activated(5);
    QApplication::processEvents();
    apply();
    CHECK(lastCall.value("count", 0) == 42);
    CHECK(lastCall.value("scale", 0.0) == doctest::Approx(3.0));
    CHECK(menu->currentText() == "Mine");
    d->close();
    QApplication::processEvents();
    for (const pr::Preset& p : pr::list("presetProbe"))
        pr::remove(p);
    ops::options::reset("presetProbe");
}
