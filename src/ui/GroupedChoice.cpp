#include "ui/GroupedChoice.h"
#include "ui/Theme.h"

#include <QFrame>
#include <QGuiApplication>
#include <QHBoxLayout>
#include <QKeyEvent>
#include <QLineEdit>
#include <QListWidget>
#include <QPointer>
#include <QScreen>
#include <QVBoxLayout>

namespace lila::ui
{
namespace
{
constexpr int kMaxRows = 14;

class GroupedPopup : public QFrame
{
public:
    explicit GroupedPopup(GroupedChoice* combo) : QFrame(combo, Qt::Popup), mCombo(combo)
    {
        setObjectName("groupedChoicePopup");
        setAttribute(Qt::WA_DeleteOnClose);
        setFrameShape(QFrame::StyledPanel);

        auto* lay = new QVBoxLayout(this);
        lay->setContentsMargins(6, 6, 6, 6);
        lay->setSpacing(4);
        mSearch = new QLineEdit(this);
        mSearch->setPlaceholderText("Search");
        mSearch->setClearButtonEnabled(true);
        lay->addWidget(mSearch);

        auto* columns = new QHBoxLayout;
        columns->setSpacing(4);
        mGroups = new QListWidget(this);
        mItems = new QListWidget(this);
        for (QListWidget* list : {mGroups, mItems})
        {
            list->setFocusPolicy(Qt::NoFocus);
            list->setUniformItemSizes(true);
            list->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
            list->setFrameShape(QFrame::NoFrame);
        }
        mGroups->setObjectName("groupedChoiceGroups");
        mItems->setObjectName("groupedChoiceItems");
        columns->addWidget(mGroups);
        columns->addWidget(mItems, 1);
        lay->addLayout(columns);

        int widestGroup = 0, widestItem = 0, mostItems = 0;
        const QFontMetrics fm(font());
        QMap<QString, int> counts;
        for (int i = 0; i < combo->count(); ++i)
        {
            const QString group = i < combo->groups().size() ? combo->groups()[i] : QString();
            if (!counts.contains(group))
                mGroups->addItem(group);
            mostItems = std::max(mostItems, ++counts[group]);
            widestGroup = std::max(widestGroup, fm.horizontalAdvance(group));
            widestItem = std::max(widestItem, fm.horizontalAdvance(combo->itemText(i)));
        }
        const int row = std::max(mGroups->sizeHintForRow(0), fm.height() + 2);
        const int rows = std::clamp(std::max(mostItems, mGroups->count()), 6, kMaxRows);
        mGroups->setFixedWidth(widestGroup + 28);
        mItems->setMinimumWidth(widestItem + 36);
        for (QListWidget* list : {mGroups, mItems})
            list->setFixedHeight(rows * row + 4);

        const int current = combo->currentIndex();
        showGroup(current >= 0 && current < combo->groups().size() ? combo->groups()[current] : QString());

        mSearch->installEventFilter(this);
        connect(mSearch, &QLineEdit::textChanged, this, [this](const QString& text) {
            if (text.trimmed().isEmpty())
                showGroup(mGroup);
            else
                showMatches(text.trimmed());
        });
        connect(mGroups, &QListWidget::itemClicked, this, [this](QListWidgetItem* item) {
            const QString group = item->text();
            QSignalBlocker b(mSearch);
            mSearch->clear();
            showGroup(group);
        });
        connect(mItems, &QListWidget::itemClicked, this, [this](QListWidgetItem* item) { pick(item); });
        // A search lists every group's matches; the group column shows which one the current match is in.
        connect(mItems, &QListWidget::currentItemChanged, this, [this](QListWidgetItem* item) {
            if (!item || mSearch->text().trimmed().isEmpty() || !(item->flags() & Qt::ItemIsEnabled))
                return;
            const int index = item->data(Qt::UserRole).toInt();
            const QString group = index < mCombo->groups().size() ? mCombo->groups()[index] : QString();
            for (int i = 0; i < mGroups->count(); ++i)
                if (mGroups->item(i)->text() == group)
                    mGroups->setCurrentRow(i);
        });
    }

protected:
    bool eventFilter(QObject* o, QEvent* e) override
    {
        if (o == mSearch && e->type() == QEvent::KeyPress)
        {
            auto* key = static_cast<QKeyEvent*>(e);
            switch (key->key())
            {
            case Qt::Key_Down:
            case Qt::Key_Up:
                step(mItems, key->key() == Qt::Key_Down ? 1 : -1);
                return true;
            case Qt::Key_Tab:
            case Qt::Key_Backtab:
                stepGroup(key->key() == Qt::Key_Tab ? 1 : -1);
                return true;
            case Qt::Key_Left:
            case Qt::Key_Right:
                // The arrows move the caret while there is text to move it in.
                if (!mSearch->text().isEmpty())
                    break;
                stepGroup(key->key() == Qt::Key_Right ? 1 : -1);
                return true;
            case Qt::Key_Return:
            case Qt::Key_Enter:
                pick(mItems->currentItem() ? mItems->currentItem() : mItems->item(0));
                return true;
            case Qt::Key_Escape:
                close();
                return true;
            default:
                break;
            }
        }
        return QFrame::eventFilter(o, e);
    }

private:
    static void step(QListWidget* list, int by)
    {
        if (list->count() == 0)
            return;
        const int at = list->currentRow() < 0 ? (by > 0 ? 0 : list->count() - 1) : (list->currentRow() + by + list->count()) % list->count();
        list->setCurrentRow(at);
    }

    void stepGroup(int by)
    {
        QSignalBlocker b(mSearch);
        mSearch->clear();
        step(mGroups, by);
        if (QListWidgetItem* item = mGroups->currentItem())
            showGroup(item->text());
    }

    void addItem(int index)
    {
        auto* item = new QListWidgetItem(mCombo->itemText(index), mItems);
        item->setData(Qt::UserRole, index);
        item->setToolTip(mCombo->itemData(index, Qt::ToolTipRole).toString());
        if (index == mCombo->currentIndex())
            mItems->setCurrentItem(item);
    }

    void showGroup(const QString& group)
    {
        mGroup = group;
        for (int i = 0; i < mGroups->count(); ++i)
            if (mGroups->item(i)->text() == group)
                mGroups->setCurrentRow(i);
        mItems->clear();
        for (int i = 0; i < mCombo->count(); ++i)
            if ((i < mCombo->groups().size() ? mCombo->groups()[i] : QString()) == group)
                addItem(i);
    }

    void showMatches(const QString& text)
    {
        mGroups->setCurrentRow(-1);
        {
            QSignalBlocker b(mItems);
            mItems->clear();
        }
        // Labels starting with the text first, then those holding it, then a match on the group's name.
        for (int pass = 0; pass < 3; ++pass)
            for (int i = 0; i < mCombo->count(); ++i)
            {
                const QString label = mCombo->itemText(i);
                const QString group = i < mCombo->groups().size() ? mCombo->groups()[i] : QString();
                const int rank = label.startsWith(text, Qt::CaseInsensitive) ? 0
                                 : label.contains(text, Qt::CaseInsensitive) || mCombo->itemData(i).toString().contains(text, Qt::CaseInsensitive) ? 1
                                 : group.contains(text, Qt::CaseInsensitive) ? 2
                                                                             : 3;
                if (rank == pass)
                    addItem(i);
            }
        if (mItems->count() == 0)
        {
            auto* none = new QListWidgetItem("No matches", mItems);
            none->setFlags(Qt::NoItemFlags);
        }
        else
            mItems->setCurrentRow(0);
    }

    void pick(QListWidgetItem* item)
    {
        if (!item || !(item->flags() & Qt::ItemIsEnabled))
            return;
        const int index = item->data(Qt::UserRole).toInt();
        const QPointer<GroupedChoice> combo = mCombo;
        close();
        if (combo && index >= 0 && index < combo->count())
        {
            combo->setCurrentIndex(index);
            Q_EMIT combo->activated(index);
        }
    }

    GroupedChoice* mCombo;
    QLineEdit* mSearch = nullptr;
    QListWidget* mGroups = nullptr;
    QListWidget* mItems = nullptr;
    QString mGroup;

    friend class lila::ui::GroupedChoice;
};
} // namespace

GroupedChoice::GroupedChoice(QWidget* parent) : QComboBox(parent)
{
    // No focus from the wheel: scrolling the box past it would otherwise step through the list.
    setFocusPolicy(Qt::StrongFocus);
}

void GroupedChoice::setGroups(const QStringList& groups)
{
    mGroups = groups;
}

void GroupedChoice::showPopup()
{
    auto* pop = new GroupedPopup(this);
    pop->adjustSize();
    QPoint at = mapToGlobal(QPoint(0, height() + 2));
    if (const QScreen* screen = QGuiApplication::screenAt(at))
    {
        const QRect area = screen->availableGeometry();
        if (at.y() + pop->height() > area.bottom())
            at.setY(mapToGlobal(QPoint(0, 0)).y() - pop->height() - 2);
        at.setX(std::clamp(at.x(), area.left(), std::max(area.left(), area.right() - pop->width())));
    }
    pop->move(at);
    pop->show();
    pop->mSearch->setFocus();
}

void GroupedChoice::hidePopup()
{
    if (auto* pop = findChild<QFrame*>("groupedChoicePopup", Qt::FindDirectChildrenOnly))
        pop->close();
    QComboBox::hidePopup();
}
} // namespace lila::ui
