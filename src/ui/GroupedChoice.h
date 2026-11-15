#pragma once

#include "core/Export.h"

#include <QComboBox>
#include <QStringList>

namespace lila::ui
{
/**
 * A dropdown for a long list whose items fall into groups: closed it is an ordinary combo box, open it is a search
 * field over two columns, the groups and the open group's items. Typing lists the matches from every group.
 * Items are added like any combo's; each one's group goes in with setGroups.
 */
class LILA_UI_API GroupedChoice : public QComboBox
{
    Q_OBJECT
public:
    explicit GroupedChoice(QWidget* parent = nullptr);

    /** Each item's group, by index. Groups list in the order they first appear. */
    void setGroups(const QStringList& groups);
    const QStringList& groups() const { return mGroups; }

    void showPopup() override;
    void hidePopup() override;

private:
    QStringList mGroups;
};
} // namespace lila::ui
