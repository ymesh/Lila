#include <doctest/doctest.h>

#include "ui/MenuButtons.h"
#include "ui/UvEditor.h"
#include "ui/UvPackPanel.h"

#include <QApplication>
#include <QDoubleSpinBox>
#include <QLabel>
#include <QMenu>
#include <QSpinBox>
#include <QSplitter>
#include <QToolButton>

using namespace lila::ui;

namespace
{
QToolButton* buttonNamed(QWidget& w, const QString& text)
{
    for (QToolButton* b : w.findChildren<QToolButton*>())
        if (b->text() == text)
            return b;
    return nullptr;
}
} // namespace

TEST_CASE("a right-click default runs its menu item without opening the menu")
{
    QToolButton button;
    button.setToolTip("Project");
    auto* menu = new QMenu(&button);
    int ran = 0;
    QAction* first = menu->addAction("Planar (fit)", [&ran] { ++ran; });
    menu->addAction("Box");
    button.setMenu(menu);
    setRightClickAction(&button, first);
    CHECK(rightClickAction(&button) == first);
    CHECK(button.toolTip() == "Project\nRight-click: Planar (fit)");
    Q_EMIT button.customContextMenuRequested({});
    CHECK(ran == 1);

    setRightClickAction(&button, nullptr);
    CHECK(button.toolTip() == "Project");
    Q_EMIT button.customContextMenuRequested({});
    CHECK(ran == 1);
}

TEST_CASE("the UV editor stays narrow whatever its status line and pack options hold")
{
    UvEditor editor;
    editor.resize(900, 700);
    editor.show();
    QToolButton* toolkit = buttonNamed(editor, "Toolkit");
    REQUIRE(toolkit);
    toolkit->setChecked(false);
    CAPTURE(editor.minimumSizeHint().width());
    CHECK(editor.minimumSizeHint().width() <= 400);
    toolkit->setChecked(true);
    const int withToolkit = editor.minimumSizeHint().width();

    QToolButton* options = buttonNamed(editor, "Pack Options");
    REQUIRE(options);
    options->setChecked(true);
    for (QLabel* l : editor.findChildren<QLabel*>())
        if (l->sizePolicy().horizontalPolicy() == QSizePolicy::Ignored)
            l->setText(QString("uvLayout: ") + QString(600, 'x'));
    CHECK(editor.minimumSizeHint().width() <= withToolkit);

    for (const char* name : {"Planar", "Box", "Spherical", "Hose"})
    {
        CAPTURE(name);
        CHECK(buttonNamed(editor, name));
    }
}

TEST_CASE("the UV editor reopens with the last texture size and padding")
{
    const nlohmann::json before = uvPackOptions();
    {
        UvEditor editor;
        auto* size = editor.findChild<QSpinBox*>("textureSize");
        auto* padding = editor.findChild<QDoubleSpinBox*>("padding");
        REQUIRE(size);
        REQUIRE(padding);
        size->setValue(4096);
        padding->setValue(8.0);
    }
    UvEditor editor;
    CHECK(editor.findChild<QSpinBox*>("textureSize")->value() == 4096);
    CHECK(editor.findChild<QDoubleSpinBox*>("padding")->value() == doctest::Approx(8.0));
    setUvPackOptions(before);
}

TEST_CASE("the UV toolkit sits beside a wide editor and below a tall one in two columns")
{
    UvEditor editor;
    editor.resize(1200, 700);
    editor.show();
    QApplication::processEvents();
    QSplitter* split = editor.findChild<QSplitter*>();
    REQUIRE(split);
    CHECK(split->orientation() == Qt::Horizontal);

    editor.resize(1100, 1700);
    QApplication::processEvents();
    CHECK(split->orientation() == Qt::Vertical);
    // Section headers: Unwrap opens the first column, Pack the second.
    auto header = [&](const QString& title) -> QToolButton* {
        for (QToolButton* c : editor.findChildren<QToolButton*>())
            if (c->text() == title && c->property("role").toString() == "sectionHeader")
                return c;
        return nullptr;
    };
    QToolButton* unwrap = header("Unwrap");
    QToolButton* transform = header("Pack");
    REQUIRE(unwrap);
    REQUIRE(transform);
    const QPoint u = unwrap->mapTo(&editor, QPoint());
    const QPoint t = transform->mapTo(&editor, QPoint());
    CHECK(t.x() > u.x());
    CHECK(t.y() == u.y());

    editor.resize(1300, 760);
    QApplication::processEvents();
    CHECK(split->orientation() == Qt::Horizontal);
    CHECK(transform->mapTo(&editor, QPoint()).y() > unwrap->mapTo(&editor, QPoint()).y());
}
