#pragma once

#include <QMap>
#include <QStringList>
#include <QWidget>

#include <functional>

class QMenu;
class QToolButton;

namespace lila::ui
{
/**
 * Tag filtering for a library shelf: a Tags button whose menu lists the tags present (with counts) and the picked ones
 * as removable chips on their own row, hidden while none are picked. An item shows when it carries every picked tag.
 */
class ShelfTagFilter final : public QObject
{
public:
    explicit ShelfTagFilter(QWidget* parent);

    /** The Tags button, for the search row. */
    QToolButton* button() const { return mButton; }
    /** The chips row, under the search row. */
    QWidget* chips() const { return mChips; }

    /** The tags the shown items carry, with how many carry each; picked tags no longer present are dropped. */
    void setAvailable(const QMap<QString, int>& counts);
    bool matches(const QStringList& tags) const;
    bool active() const { return !mPicked.isEmpty(); }

    std::function<void()> onChanged;

    /** "metal" -> "Metal". */
    static QString label(const QString& tag);

private:
    void toggle(const QString& tag);
    void rebuildChips();
    void fillMenu();

    QToolButton* mButton = nullptr;
    QMenu* mMenu = nullptr;
    QWidget* mChips = nullptr;
    QMap<QString, int> mCounts;
    QStringList mPicked;
};

/**
 * A Tags submenu for one smart material: every tag in use as a check, plus New Tag. Changes go through
 * texSetSmartMaterialTags; changed runs after each.
 */
void addSmartTagMenu(QMenu* menu, const QString& smartMaterial, const QStringList& tags, const QStringList& known, std::function<void()> changed);
} // namespace lila::ui
