#pragma once

#include "core/Export.h"
#include "core/Registry.h"
#include "ui/Owner.h"

#include <QWidget>

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

class QScrollArea;
class QTimer;
class QToolButton;

namespace lila::ui
{
/** Dock object name for the Level Toolkit. */
inline constexpr const char* kLevelToolkitDock = "levelToolkit";

/**
 * Level building tools in one panel: terrain, settlements, masonry, set dressing and whatever else registers a section.
 * Lila owns the panel; the sections come from levelToolkit::add, so an extension's tools appear and go with it.
 */
namespace levelToolkit
{
/**
 * The Level menu and its section headings. An extension adds its submenu with menus::addItem(ctx, {.menu = kMenu, ...})
 * and .before the next section's id to land under a heading: before kDressSection for Build, before kAssetsSection for
 * Dress, nothing for Assets.
 */
inline constexpr const char* kMenu = "Level";
inline constexpr const char* kBuildSection = "level.build";
inline constexpr const char* kDressSection = "level.dress";
inline constexpr const char* kAssetsSection = "level.assets";

/**
 * One button in a section. Items in a group show as tiles under the group's title; ungrouped items with a shelf label
 * are the section's daily tools and show as tiles at its top; the rest are labelled buttons, two to a row, and rare
 * ones (or any past six) go in the section's overflow menu.
 */
struct Item
{
    /** A registered action. Items with an empty or unregistered action are skipped. */
    std::string action;
    std::string icon; // Tabler name, for a tile; empty falls back to the action's icon
    std::string text; // short button text; empty for the action's label
    std::string shelf; // label of a button for it on the Level shelf; empty for none
    /** A titled tile group within the section; items naming the same group share a grid under that title. */
    std::string group;
    /** Rare: always in the section's overflow menu instead of a button. */
    bool rare = false;
};

struct Section
{
    std::string id;
    std::string title;
    std::vector<Item> items;
    /** Extra content under the buttons, such as a settings form; parent is the section's box. Empty for none. */
    std::function<QWidget*(QWidget* parent)> create;
    /** The create content goes above the buttons instead of under them. */
    bool createFirst = false;
    /** Lower comes first; equal orders keep registration order. */
    int order = 100;
    /** A docs page ("masonry", "stagehand?id=tune-it") for a docs button on the section's header; empty for none. */
    std::string docs;
};

LILA_UI_API Registry<Section>& registry();
/** Adds or replaces. */
LILA_UI_API void add(Section s, std::string owner = kLilaOwner);
/** Owned by the extension and removed when it unloads. */
LILA_UI_API uint64_t add(ext::Context& ctx, Section s);
LILA_UI_API bool remove(const std::string& id);

/** Keeps the Level shelf in step with the sections' shelf items: the toolkit's toggle, then each section's in order. */
LILA_UI_API void registerShelf();
} // namespace levelToolkit

class LILA_UI_API LevelToolkit : public QWidget
{
    Q_OBJECT

public:
    explicit LevelToolkit(QWidget* parent = nullptr);
    ~LevelToolkit() override;

protected:
    void showEvent(QShowEvent* e) override;

private:
    void rebuild();
    /** A tile for a registered action, or null when it isn't one. */
    QToolButton* tile(const std::string& id, const std::string& iconName, const QString& label, QWidget* parent);
    /** A labelled push button for a registered action, or null when it isn't one. */
    QToolButton* push(const std::string& id, const QString& label, QWidget* parent);
    void refreshButtons();

    QScrollArea* mScroll = nullptr;
    QWidget* mBody = nullptr;
    QTimer* mRebuild = nullptr;
    QTimer* mRefresh = nullptr;
    std::vector<QToolButton*> mButtons;
    int mSectionListener = 0;
    int mActionListener = 0;
};
} // namespace lila::ui
