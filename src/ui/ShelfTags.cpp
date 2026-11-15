#include "ui/ShelfTags.h"

#include "ui/FlowLayout.h"
#include "ui/Icons.h"
#include "ui/Theme.h"
#include "ui/UiCommands.h"
#include "ui/UiCommon.h"

#include <QInputDialog>
#include <QMenu>
#include <QToolButton>

namespace lila::ui
{
ShelfTagFilter::ShelfTagFilter(QWidget* parent) : QObject(parent)
{
    mButton = toolButton(parent, "filter", "Tags: show only items carrying every checked tag");
    mButton->setObjectName("shelfTags");
    mButton->setCheckable(true);
    mMenu = new QMenu(mButton);
    mButton->setMenu(mMenu);
    mButton->setPopupMode(QToolButton::InstantPopup);
    QObject::connect(mMenu, &QMenu::aboutToShow, this, [this] { fillMenu(); });
    mChips = new QWidget(parent);
    mChips->setObjectName("shelfTagChips");
    auto* flow = new FlowLayout(2);
    flow->setContentsMargins(0, 0, 0, 0);
    mChips->setLayout(flow);
    mChips->hide();
}

QString ShelfTagFilter::label(const QString& tag)
{
    return tag.isEmpty() ? tag : tag.left(1).toUpper() + tag.mid(1);
}

void ShelfTagFilter::setAvailable(const QMap<QString, int>& counts)
{
    mCounts = counts;
    const qsizetype before = mPicked.size();
    mPicked.removeIf([&](const QString& t) { return !counts.contains(t); });
    mButton->setEnabled(!counts.isEmpty() || !mPicked.isEmpty());
    if (mPicked.size() != before)
        rebuildChips();
}

bool ShelfTagFilter::matches(const QStringList& tags) const
{
    for (const QString& t : mPicked)
        if (!tags.contains(t))
            return false;
    return true;
}

void ShelfTagFilter::toggle(const QString& tag)
{
    if (!mPicked.removeOne(tag))
        mPicked << tag;
    rebuildChips();
    if (onChanged)
        onChanged();
}

void ShelfTagFilter::fillMenu()
{
    mMenu->clear();
    if (mCounts.isEmpty())
    {
        mMenu->addAction("No tags here")->setEnabled(false);
        return;
    }
    for (auto it = mCounts.cbegin(); it != mCounts.cend(); ++it)
    {
        const QString tag = it.key();
        QAction* a = mMenu->addAction(QString("%1 (%2)").arg(label(tag)).arg(it.value()));
        a->setCheckable(true);
        a->setChecked(mPicked.contains(tag));
        QObject::connect(a, &QAction::triggered, this, [this, tag] { toggle(tag); });
    }
    if (!mPicked.isEmpty())
    {
        mMenu->addSeparator();
        QObject::connect(mMenu->addAction("Clear Tags"), &QAction::triggered, this, [this] {
            mPicked.clear();
            rebuildChips();
            if (onChanged)
                onChanged();
        });
    }
}

void ShelfTagFilter::rebuildChips()
{
    QLayout* flow = mChips->layout();
    while (QLayoutItem* item = flow->takeAt(0))
    {
        delete item->widget();
        delete item;
    }
    for (const QString& tag : mPicked)
    {
        auto* chip = new QToolButton(mChips);
        chip->setText(label(tag));
        chip->setIcon(icon("x"));
        chip->setToolButtonStyle(Qt::ToolButtonTextBesideIcon);
        chip->setLayoutDirection(Qt::RightToLeft);
        chip->setAutoRaise(false);
        chip->setStyleSheet(themedStyleSheet("QToolButton { background: #4a4a4a; border: none; border-radius: 9px; padding: 1px 4px 1px 8px; }"
                                             "QToolButton:hover { background: #5a5a5a; }"));
        chip->setToolTip("Stop filtering by " + label(tag));
        QObject::connect(chip, &QToolButton::clicked, this, [this, tag] { toggle(tag); });
        flow->addWidget(chip);
    }
    mChips->setVisible(!mPicked.isEmpty());
    mButton->setChecked(!mPicked.isEmpty());
}

void addSmartTagMenu(QMenu* menu, const QString& smartMaterial, const QStringList& tags, const QStringList& known, std::function<void()> changed)
{
    QMenu* sub = menu->addMenu(icon("tags"), "Tags");
    QStringList all = known;
    for (const QString& t : tags)
        if (!all.contains(t))
            all << t;
    all.sort();
    const std::string name = smartMaterial.toStdString();
    for (const QString& t : all)
    {
        QAction* a = sub->addAction(ShelfTagFilter::label(t));
        a->setCheckable(true);
        a->setChecked(tags.contains(t));
        QObject::connect(a, &QAction::triggered, menu, [name, t, changed](bool on) {
            runUi("texSetSmartMaterialTags", {{"args", {name}}, {on ? "add" : "remove", {t.toStdString()}}});
            if (changed)
                changed();
        });
    }
    if (!all.isEmpty())
        sub->addSeparator();
    QObject::connect(sub->addAction("New Tag..."), &QAction::triggered, menu, [name, changed, parent = menu->parentWidget()] {
        bool ok = false;
        const QString t = QInputDialog::getText(parent, "New Tag", "Tag", QLineEdit::Normal, {}, &ok).trimmed().toLower();
        if (!ok || t.isEmpty())
            return;
        runUi("texSetSmartMaterialTags", {{"args", {name}}, {"add", {t.toStdString()}}});
        if (changed)
            changed();
    });
}
} // namespace lila::ui
