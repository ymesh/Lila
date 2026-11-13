#include <doctest/doctest.h>

#include "scene/Node.h"
#include "ui/Environments.h"
#include "ui/ImagePicker.h"

#include <QApplication>
#include <QDeadlineTimer>
#include <QLineEdit>
#include <QListWidget>
#include <QToolButton>

#include <vector>

using namespace lila;
using namespace lila::ui;

namespace
{
QListWidgetItem* itemNamed(QListWidget* list, const QString& text)
{
    for (int i = 0; i < list->count(); ++i)
        if (list->item(i)->text() == text)
            return list->item(i);
    return nullptr;
}

/** The picker's library list once it opens; a single click on the thumbnail waits out the double-click interval first. */
QListWidget* listOnceOpen(ImagePicker& picker)
{
    const QDeadlineTimer deadline(QApplication::doubleClickInterval() + 5000);
    QListWidget* list = picker.findChild<QListWidget*>();
    while (!list && !deadline.hasExpired())
    {
        QApplication::processEvents(QEventLoop::AllEvents, 20);
        list = picker.findChild<QListWidget*>();
    }
    return list;
}
} // namespace

TEST_CASE("image picker: dome texture lists the HDRIs with thumbnails, filters as you type and keeps the current one")
{
    scene::NodeType light;
    light.name = "light";
    scene::NodeType dome;
    dome.name = "myDomeLight";
    dome.base = &light;
    const imagePicker::Library* lib = imagePicker::libraryFor(dome, "texture");
    REQUIRE(lib);
    CHECK(lib->id == "hdri");

    const auto shipped = environments::builtIn();
    REQUIRE(shipped.size() >= 2);
    CHECK_FALSE(imagePicker::thumbnail(shipped.front()).isNull());

    ImagePicker picker(lib, "Texture");
    const std::string name = shipped.front().filename().string();
    picker.setValue(name);
    auto* button = picker.findChild<QToolButton*>("imageThumb");
    auto* path = picker.findChild<QLineEdit*>("imagePath");
    REQUIRE(button);
    REQUIRE(path);
    CHECK_FALSE(button->icon().isNull());
    const QString shownName = path->text();
    CHECK_FALSE(shownName.isEmpty());

    std::vector<QString> chosen;
    QObject::connect(&picker, &ImagePicker::chosen, [&](const QString& v) { chosen.push_back(v); });
    button->click();
    QListWidget* list = listOnceOpen(picker);
    QLineEdit* search = nullptr;
    for (QLineEdit* e : picker.findChildren<QLineEdit*>())
        if (e != path)
            search = e;
    REQUIRE(list);
    REQUIRE(search);
    CHECK(itemNamed(list, "Browse..."));
    CHECK(itemNamed(list, "None"));
    // The current HDRI is highlighted.
    REQUIRE(list->currentItem());
    CHECK(list->currentItem()->text() == shownName);

    // Typing filters to the rows that match.
    const int before = list->count();
    search->setText("zzzz-no-such-image");
    CHECK(list->count() < before);
    CHECK_FALSE(itemNamed(list, shownName));
    search->clear();

    const QString otherName = QString::fromStdString(environments::displayName(shipped.back()));
    QListWidgetItem* other = itemNamed(list, otherName);
    REQUIRE(other);
    Q_EMIT list->itemClicked(other);
    REQUIRE(chosen.size() == 1);
    CHECK(chosen[0].toStdString() == shipped.back().filename().string());

    // Typing a path chooses it; dropping and clearing go through the same signal.
    path->setText("somewhere/typed.png");
    path->setModified(true);
    Q_EMIT path->editingFinished();
    REQUIRE(chosen.size() == 2);
    CHECK(chosen[1] == "somewhere/typed.png");

    picker.setValue("somewhere/missing.hdr");
    CHECK(path->text().contains("missing"));
}
