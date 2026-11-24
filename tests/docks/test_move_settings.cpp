#include <doctest/doctest.h>

#include "core/PrefStore.h"
#include "ui/Actions.h"
#include "ui/Controls.h"
#include "ui/ModelingToolkit.h"
#include "ui/Tools.h"

#include <QApplication>
#include <QComboBox>
#include <QDoubleSpinBox>

#include <chrono>
#include <thread>
#include <vector>

using namespace lila;

namespace
{
void ensureToolActions()
{
    if (!ui::actions::find("tool.pivotEditOn"))
        ui::tools::registerActions();
}
} // namespace

TEST_CASE("number fields don't hold a panel wide for their largest value")
{
    std::vector<QDoubleSpinBox*> spins;
    QWidget* v = ui::controls::vectorField(spins, -1e6, 1e6, 3, {}, nullptr);
    CHECK(v->minimumSizeHint().width() < 200);
    delete v;
}

TEST_CASE("the pivot key toggles on a tap and edits only while held")
{
    ensureToolActions();
    ui::tools::setPivotEdit(false);
    ui::actions::run("tool.pivotEditOn");
    ui::actions::run("tool.pivotEditOff");
    CHECK(ui::tools::pivotEdit());
    ui::actions::run("tool.pivotEditOn");
    ui::actions::run("tool.pivotEditOff");
    CHECK_FALSE(ui::tools::pivotEdit());

    ui::actions::run("tool.pivotEditOn");
    CHECK(ui::tools::pivotEdit());
    std::this_thread::sleep_for(std::chrono::milliseconds(300));
    ui::actions::run("tool.pivotEditOff");
    CHECK_FALSE(ui::tools::pivotEdit());
}

TEST_CASE("the toolkit's Move section sets the move axis and the Custom angles")
{
    ensureToolActions();
    PrefStore& prefs = PrefStore::get();
    prefs.set("general", "/manipulator/moveSpace", "world");
    ui::ModelingToolkit toolkit;
    toolkit.show();
    QApplication::processEvents();

    QComboBox* axis = nullptr;
    for (QComboBox* c : toolkit.findChildren<QComboBox*>())
        if (c->findData("custom") >= 0)
            axis = c;
    REQUIRE(axis);
    const int custom = axis->findData("custom");
    axis->setCurrentIndex(custom);
    Q_EMIT axis->activated(custom);
    CHECK(prefs.value<std::string>("general", "/manipulator/moveSpace", "") == "custom");

    std::vector<QDoubleSpinBox*> angles;
    for (QDoubleSpinBox* s : toolkit.findChildren<QDoubleSpinBox*>())
        if (s->suffix().contains(QChar(0x00B0)))
            angles.push_back(s);
    REQUIRE(angles.size() == 3);
    angles[1]->setValue(45.0);
    const nlohmann::json stored = prefs.value("general", "/manipulator/customAxis", nlohmann::json());
    REQUIRE(stored.is_array());
    CHECK(stored[1].get<double>() == doctest::Approx(45.0));
    prefs.set("general", "/manipulator/moveSpace", "world");
    prefs.set("general", "/manipulator/customAxis", nlohmann::json::array({0.0, 0.0, 0.0}));
}
