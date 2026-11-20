#pragma once

#include "core/Export.h"

#include <nlohmann/json.hpp>

#include <QWidget>

#include <functional>
#include <string>
#include <vector>

class QComboBox;
class QLabel;
class QLineEdit;
class QListWidget;
class QPlainTextEdit;
class QPushButton;
class QStackedWidget;
class QTreeWidget;
class QTreeWidgetItem;

namespace lila::ui
{
/** One level of a marking menu drawn the way it pops up, with empty slots outlined. */
class LILA_UI_API RadialPreview : public QWidget
{
    Q_OBJECT

public:
    explicit RadialPreview(QWidget* parent = nullptr);

    /** selected: 0-7 compass slot, kOverflow + i overflow row, -1 none. */
    void setLevel(const nlohmann::json& level, int selected, QString crumb);
    QSize sizeHint() const override;

    static constexpr int kOverflow = 100;

Q_SIGNALS:
    void picked(int slot);

protected:
    void paintEvent(QPaintEvent* e) override;
    void mousePressEvent(QMouseEvent* e) override;

private:
    struct Box
    {
        int slot;
        QRectF rect;
        QString label;
        bool empty;
        bool submenu;
    };
    std::vector<Box> layoutBoxes() const;

    nlohmann::json mLevel;
    int mSelected = -1;
    QString mCrumb;
};

/**
 * Edits "markingMenus/<name>" prefs: the eight compass slots, overflow rows and submenus, with each item's label
 * and target (an action, Python, or a submenu). Saves the difference from the built-in menu, so untouched slots
 * keep tracking the defaults.
 */
class LILA_UI_API MarkingMenuEditor : public QWidget
{
    Q_OBJECT

public:
    explicit MarkingMenuEditor(QWidget* parent = nullptr);
    ~MarkingMenuEditor() override;

    /** Opens a menu by name; exposed for screenshots. */
    void openMenu(const std::string& name);
    /** Selects an item by JSON pointer into the menu ("/N", "/S/children/E", "/overflow/0"). */
    void selectPointer(const std::string& ptr);
    /** Opens a menu and selects ptr in it (empty keeps the default), asking first when unsaved changes would be lost. */
    void showMenu(const std::string& name, const std::string& ptr);

private:
    enum class Kind
    {
        Action,
        Python,
        Submenu,
        Other
    };

    void refreshMenuList();
    void rebuildTree();
    void addLevel(QTreeWidgetItem* parent, const nlohmann::json& level, const std::string& base);
    void showSelection();
    void refreshPreview();
    void setDirty(bool dirty);
    void save();

    nlohmann::json* item();
    nlohmann::json& ensureItem();
    std::string levelPointer() const;
    int selectedSlot() const;
    void editItem(const std::function<void(nlohmann::json&)>& fn);
    void removeSelected();
    void moveOverflow(int delta);
    void addOverflow(const nlohmann::json& item);

    nlohmann::json mDoc = nlohmann::json::object();
    std::string mName;
    std::string mSel;
    bool mDirty = false;
    bool mLoading = false;
    int mPrefListener = 0;

    QListWidget* mMenus = nullptr;
    QPushButton* mSave = nullptr;
    QPushButton* mRevert = nullptr;
    QPushButton* mResetDefault = nullptr;
    RadialPreview* mPreview = nullptr;
    QTreeWidget* mTree = nullptr;

    QLabel* mSlotLabel = nullptr;
    QLineEdit* mLabel = nullptr;
    QComboBox* mKind = nullptr;
    QStackedWidget* mPages = nullptr;
    QLineEdit* mActionSearch = nullptr;
    QListWidget* mActionList = nullptr;
    QPlainTextEdit* mPython = nullptr;
    QPlainTextEdit* mOther = nullptr;
    QLabel* mOtherError = nullptr;
    QPushButton* mToPython = nullptr;
    QPushButton* mRemove = nullptr;
    QPushButton* mUp = nullptr;
    QPushButton* mDown = nullptr;
};
} // namespace lila::ui
