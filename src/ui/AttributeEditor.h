#pragma once

#include "core/Export.h"
#include "core/Registry.h"
#include "ui/Owner.h"

#include "scene/Node.h"
#include "scene/Plug.h"

#include <QWidget>

#include <cstdint>
#include <functional>
#include <map>
#include <set>
#include <string>
#include <utility>
#include <vector>

class QFormLayout;
class QLabel;
class QLineEdit;
class QToolButton;
class QScrollArea;
class QVBoxLayout;

namespace lila::ui
{
namespace attributeEditor
{
/** Builds an extra section for a node (tools, status); return null to skip it. */
using SectionFactory = std::function<QWidget*(scene::NodeId)>;

/** A section added below a type's attributes (Maya's AETemplate callbacks). Registered under "<typeName>/<title>". */
struct Section
{
    std::string typeName;
    std::string title;
    SectionFactory make;
};
LILA_UI_API Registry<Section>& registry();

/** Adds the section to the editor of every node whose type is or derives from typeName. Open editors rebuild. */
LILA_UI_API void addSection(std::string typeName, std::string title, SectionFactory fn, std::string owner = kLilaOwner);
/** Owned by the extension and removed when it unloads. */
LILA_UI_API uint64_t addSection(ext::Context& ctx, std::string typeName, std::string title, SectionFactory fn);
LILA_UI_API bool removeSection(const std::string& typeName, const std::string& title);
/** Builds the added sections that apply to a node, as (title, widget), for other panels that show its tools too. */
LILA_UI_API std::vector<std::pair<std::string, QWidget*>> buildSections(scene::NodeId id);

/**
 * Which of a type's attributes the editor shows for one node, so settings that do not apply to it (a point light's cone)
 * stay out of the way. The editor rebuilds when an attribute named in triggers changes. enabled greys out rows that show
 * but do nothing while their switch is off (symmetry's plane while symmetry is off); it is asked on every value change.
 * A bool attr also switches the attrs its name prefixes (smoothPreview: smoothPreviewLevels) without a filter.
 */
struct AttrFilter
{
    std::string typeName;
    std::vector<std::string> triggers;
    std::function<bool(const scene::Node& node, const std::string& attr)> shows;
    std::function<bool(const scene::Node& node, const std::string& attr)> enabled;
};
/** Holds "lila.light" and "lila.meshSymmetry" from first use. */
LILA_UI_API Registry<AttrFilter>& filters();

/** Scrolls open editors to the issue's attribute or component once they show its object (select it after). */
LILA_UI_API void revealIssue(uint64_t id);
} // namespace attributeEditor

/** Every attribute of the lead selected node, grouped by the type that declares it. Edits go through setAttr. */
class LILA_UI_API AttributeEditor : public QWidget
{
    Q_OBJECT

public:
    explicit AttributeEditor(QWidget* parent = nullptr);
    ~AttributeEditor() override;

    /** Scrolls to the widget showing the issue's attribute or component; false when none does. */
    bool reveal(uint64_t issue);

private:
    void scheduleRefresh();
    void rebuild();
    void updateValues();
    void set(const std::string& attr, const scene::json& value);
    /** Gives each of the attrs (an array, or a grouped array's children) element `element`, holding its default. */
    void addElement(const std::vector<int>& attrs, int element);
    /** Removes the element attr names (attr[3], or group[3].child for the whole group[3]). */
    void removeElement(const std::string& attr);
    /** Runs the command on the shown node, its path as the first argument; errors go to the log. */
    void runOnNode(const char* command, scene::json call);
    /** Deletes the added attributes under one heading: a component. */
    void removeCategory(const std::string& category);
    QWidget* colorSwatch(const scene::AttrDef& def, scene::NodeId id, int index, bool readOnly);
    /**
     * Ends every form's labels on one shared edge: auto (the longest label, capped at 40% of the width) or where the
     * user dragged it, as a fraction of the width in prefs. Labels past it elide.
     */
    void fitLabels();
    /** The label edge dragged to x in the body, live; commit saves it. Double-click on the edge resets it to auto. */
    void dragLabelEdge(int x, bool commit);
    void resetLabelEdge();
    bool eventFilter(QObject* watched, QEvent* event) override;
    /** The node's issues above its sections, each one a link to where it points. */
    QWidget* issueList();
    /** The nodes edits go to: the shown one, or with Alt held every selected one (lead first). */
    std::vector<scene::NodeId> targets() const;
    /** The node the editor shows: the pinned one, a focused related node, else the lead. */
    scene::NodeId shownNode() const;
    /** Selected nodes that have the plug path attr (attr, attr[2], translateX), lead first. */
    std::vector<scene::NodeId> holders(const std::string& attr) const;
    /** True when the selected holders of the plug disagree on its value. */
    bool mixed(const scene::PlugRef& ref) const;
    /** The chips, history, pin and Show button row; the multi-object pill. */
    void rebuildHeader();
    void refreshMultiState();
    void showHint(const QString& text);
    void showViewPopover(QWidget* anchor);
    /** Shows a related node without changing the selection, recording it in the focus history. */
    void focusNode(scene::NodeId id);
    /** A signature of the shared view settings, so only a real change rebuilds. */
    std::string viewSignature() const;
    void openAdd(QWidget* anchor, std::string category = {});
    void openEdit(const std::string& attr, QWidget* anchor);
    /** A component heading's menu: add here, save as a component, Unreal export, remove. */
    void headingMenu(const std::string& category, QWidget* on, QPoint at);
    /** Drops attr (dragged from a row) before another added attr, or under a heading when before is empty. */
    void dropAttr(const std::string& attr, const std::string& before, const std::string& category);
    /** A plug dropped on a row: connectAttr, or a driven key with Alt held. */
    void dropPlug(const std::string& source, const std::string& target);
    /** Moves every attr under a heading in front of another heading's first. */
    void dropHeading(const std::string& category, const std::string& before);
    /** Shows only rows the search matches; sections with none collapse away. */
    void applySearch();
    /** The header's collapse state kept per node type and section in prefs. */
    void rememberCollapse(QWidget* section, const std::string& key);


    QLineEdit* mName = nullptr;
    QLineEdit* mSearch = nullptr;
    QToolButton* mAdd = nullptr;
    QWidget* mChips = nullptr;
    QLabel* mMultiPill = nullptr;
    QWidget* mHint = nullptr;
    QLabel* mHintText = nullptr;
    bool mPinned = false;
    /** A related node shown in place of the lead until the selection changes. */
    scene::NodeId mFocus = scene::kNoNode;
    std::vector<scene::NodeId> mHistory;
    int mHistoryAt = -1;
    bool mNavigating = false;
    std::vector<scene::NodeId> mLastSelection;
    /** Count badges of the last multi-object edits, by attribute, kept until the selection changes. */
    std::map<std::string, int> mBadgeCounts;
    std::map<std::string, QLabel*> mBadges;
    int mViewListener = 0;
    std::string mViewSig;
    /** Arrays (node path + "." + plug) the user asked to show past the element cap. */
    std::set<std::string> mShowAllArrays;
    struct SearchRow
    {
        QFormLayout* form;
        QWidget* label;
        scene::AttrDef def;
        /** What applySearch last set; rows start shown. setRowVisible finds its row by a linear scan, so only changes call it. */
        bool shown = true;
    };
    std::vector<SearchRow> mSearchRows;
    struct SectionEntry
    {
        QWidget* box;
        QString title;
        /** An added-attribute category, which built-in views never hide. */
        bool added;
    };
    /** Every section box with its title, for the search. */
    std::vector<SectionEntry> mSections;
    QLabel* mType = nullptr;
    QScrollArea* mScroll = nullptr;
    QWidget* mBody = nullptr;
    QVBoxLayout* mBodyLayout = nullptr;
    scene::NodeId mNode = scene::kNoNode;
    std::vector<std::function<void()>> mUpdaters;
    int mListener = 0;
    int mSectionListener = 0;
    int mIssueListener = 0;
    /** The shown node had issues at the last rebuild, so a change to them rebuilds. */
    bool mHadIssues = false;
    /** Rows by attribute name and sections by lowercased component, for reveal. */
    std::map<std::string, QWidget*> mAttrWidgets, mComponentWidgets;
    bool mQueued = false;
    bool mRebuild = false;
    /** Attributes of mNode whose change rebuilds the editor (an AttrFilter's triggers). */
    std::set<int> mTriggers;
    /** The label edge in body coordinates, and the fraction a drag in progress holds (negative when none). */
    int mLabelEdge = 0;
    double mDragFraction = -1.0;
};
} // namespace lila::ui
