#include <doctest/doctest.h>

#include "core/ExtensionContext.h"
#include "extensions/ExtensionManager.h"
#include "ui/ExtensionManagerDialog.h"
#include "ui/ExtensionScope.h"

#include <QApplication>
#include <QCheckBox>
#include <QTableWidget>
#include <QTimer>

using namespace lila;

namespace
{
int rowOf(QTableWidget* table, const QString& id)
{
    for (int r = 0; r < table->rowCount(); ++r)
        if (table->item(r, 0) && table->item(r, 0)->data(Qt::UserRole).toString() == id)
            return r;
    return -1;
}

QCheckBox* checkAt(QTableWidget* table, int row, int column)
{
    QWidget* cell = table->cellWidget(row, column);
    return cell ? cell->findChild<QCheckBox*>() : nullptr;
}
} // namespace

TEST_CASE("extension manager lists packages with version, vendor, SDK and manifest errors")
{
    extensions::ExtensionManager::get().addSearchPath(LILA_TEST_PACKAGES_DIR);
    auto* dialog = new ui::ExtensionManagerDialog;
    auto* table = dialog->findChild<QTableWidget*>();
    REQUIRE(table);

    const int base = rowOf(table, "pybase");
    REQUIRE(base >= 0);
    CHECK(table->item(base, 0)->text() == "Python Base");
    CHECK(table->item(base, 1)->text() == "1.2.0");
    CHECK(table->item(base, 2)->text() == "Lila Tests");
    CHECK(table->item(base, 3)->text() == "1.0");
    REQUIRE(checkAt(table, base, 4));
    CHECK_FALSE(checkAt(table, base, 4)->isChecked());
    CHECK(checkAt(table, base, 4)->isEnabled());

    // A broken manifest says why, and can't be loaded or set to auto load.
    const int broken = rowOf(table, "broken");
    REQUIRE(broken >= 0);
    CHECK(table->item(broken, 6)->toolTip().contains("'version' is missing"));
    CHECK_FALSE(checkAt(table, broken, 4)->isEnabled());
    CHECK_FALSE(checkAt(table, broken, 5)->isEnabled());

    // A load that Lila refuses shows its reason after the attempt, and the box stays off.
    const int future = rowOf(table, "future");
    REQUIRE(future >= 0);
    CHECK_THROWS_AS(extensions::ExtensionManager::get().load("future"), extensions::ExtensionError);
    dialog->refresh();
    CHECK(table->item(future, 6)->toolTip().contains("SDK 2.0"));
    CHECK_FALSE(checkAt(table, future, 4)->isChecked());
    delete dialog;
}

TEST_CASE("an extension's scope drops the deferred calls still queued when it unloads")
{
    int ran = 0;
    {
        ext::Context ctx("scopeTest");
        QObject* scope = ui::scope(ctx);
        REQUIRE(scope);
        CHECK(ui::scope(ctx) == scope);
        CHECK(ui::scope("scopeTest") == scope);
        QTimer::singleShot(0, ui::scope("scopeTest"), [&ran] { ++ran; });
        ctx.unloadAll();
        CHECK(ui::scope("scopeTest") == nullptr);
    }
    QApplication::processEvents();
    CHECK(ran == 0);
}
