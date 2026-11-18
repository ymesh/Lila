#pragma once

#include "core/Export.h"
#include "ui/GraphBackend.h"

#include <QWidget>

#include <nlohmann/json.hpp>

#include <array>
#include <functional>
#include <set>
#include <string>
#include <vector>

class QComboBox;
class QScrollArea;
class QSplitter;
class QTimer;
class QToolButton;

namespace lila::scene
{
class Node;
}

namespace lila::ui
{
class NodeCanvas;
struct WireDrag;

/** Dock object name for the node editor dock, which opens on geometry nodes. */
inline constexpr const char* kNodeEditorDock = "nodeEditor";

/**
 * Node editor: a canvas for one group plus a side panel with the group's exposed inputs and the selected node's
 * values, over whichever GraphBackend it is opened on. Every edit goes through the backend's commands, so it all
 * undoes. Follows the selection the way the backend says: for geometry nodes, picking a mesh with a geometry-nodes
 * modifier opens its group.
 */
class LILA_UI_API NodeEditor : public QWidget
{
public:
    /** backend: a registered graph backend id; an unknown one falls back to geometry nodes. */
    explicit NodeEditor(QWidget* parent = nullptr, const std::string& backend = kGeometryNodesBackend);
    ~NodeEditor() override;

    /** Switches to another registered backend, clearing the group on show. False when id isn't registered. */
    bool setBackend(const std::string& id);
    const GraphBackend& backend() const { return *mBackend; }

    void setGroup(const QString& name);
    QString group() const { return mGroup; }
    /** Shows the group with one node selected and framed. */
    void showNode(const QString& group, const std::string& node);
    void frameAll();
    void frameSelected();

    /** The editor the nodeEditor.* actions act on: the focused one, else the last focused. */
    static NodeEditor* active();
    /** Every open editor. */
    static const std::vector<NodeEditor*>& all();
    /** Hears an editor change the group it shows, from its picker, the selection or setGroup. Returns an id. */
    static int addGroupListener(std::function<void(NodeEditor&)> fn);
    static void removeGroupListener(int id);

    /** Add-node search under the cursor. While a wire is dragged it lists only nodes that connect to it, and links the new one. */
    void addNodeAtCursor();
    void deleteSelected();
    /** Puts the selected nodes on the clipboard as text (CopyNodes), to paste here, in another scene or a post. */
    void copySelected();
    void cutSelected();
    /** Pastes clipboard text at the cursor; text that isn't valid node data reports why and changes nothing. */
    void paste();
    void duplicateSelected();
    void selectAll();
    /** The zip-tie hold key: while down, a drag across wires ties them together. */
    void setZipTieKey(bool down);
    /** Lines the selection up: 0 left, 1 centre, 2 right, 3 top, 4 middle, 5 bottom. */
    void alignSelected(int how);
    void distributeSelected(bool horizontal);
    /** W: the selected nodes follow the cursor until a click places them. */
    void grabMove();
    /** R: the gaps between the selected nodes scale with the cursor until a click places them. */
    void grabSpacing();
    /** E: the zip tie under the cursor turns with it until a click places it. */
    void turnTie();
    /** Nudges the selection so the wires between them run level. */
    void straightenSelected();
    /** Arranges the selection, or the whole graph, in columns by data flow. */
    void autoLayout();
    /** Adds the backend's merge node for the selected nodes' outputs ("add", "subtract", "multiply", "divide"), wired up. */
    void mergeSelected(const std::string& op);
    /** Hides every wire except those on the selected nodes. */
    void setSelectedWiresOnly(bool on);
    bool selectedWiresOnly() const;
    /** The corner overview of the whole graph; remembered as a pref. */
    void setMinimap(bool on);
    bool minimap() const;
    /** Saves the view under slot 1..5 in the group, or brings it back. */
    void storeBookmark(int slot);
    void recallBookmark(int slot);
    /** Search the nodes by name or type and jump to the pick. */
    void findNode();
    /** Selects a node and centres the view on it. */
    void focusNode(const std::string& name);
    /** The canvas's right-click menu of node actions. */
    void showCanvasMenu(QPoint global);
    /** Trades the links and values of the selected node's first two inputs of one type. */
    void swapInputs();
    /** Flips "muted", "collapsed" or "hideUnused" on the selected nodes. */
    void toggleNodeFlag(const char* flag);
    /** A frame around the selected nodes, or an empty one at the cursor. */
    void frameSelection();
    /** Lights the selection's upstream and downstream and dims the rest: hold true for the held key, else the toggle. */
    void setFlowHighlight(bool on, bool hold);
    bool flowHighlight() const;
    /** From a selected portal node, select and centre its Send, or every Receive of its name. */
    void jumpToPortalSource();
    void jumpToPortalReaders();
    /** The links of each zip tie on the canvas, as (fromNode, fromSocket, toNode, toSocket). */
    std::vector<std::vector<std::array<std::string, 4>>> shownTies() const;
    /** Collapses the selected nodes into a new group run by a Group node in their place (GroupNodes). */
    void groupSelected();
    /** Inlines the selected Group node's nodes (Ungroup). */
    void ungroupSelected();
    /** Opens the group the selected Group node runs. */
    void openSelectedGroup();
    /** Shows a group run from the one on show, remembering the way back for the path bar and Parent Group. */
    void enterGroup(const QString& name);
    /** Back to the group that runs this one, with its Group node selected: the way in, else the first group that runs it. */
    void openParentGroup();
    /** The group openParentGroup goes to; empty for a top-level group. */
    QString parentGroup() const;
    /** Writes the group to a file picked in a dialog. */
    void exportGroup();
    /** Opens a name box over the group picker; Enter or leaving it renames the group through the backend, Esc cancels. */
    void startGroupRename();
    /** Reads group files into the scene and shows the last file's main group. */
    void importGroups(const QStringList& files);
    /** Previews a node through the backend: for geometry nodes, in the group's Viewer. */
    void viewNode(const std::string& node);
    /** What the Send button acts on, as the backend picks it. */
    std::vector<std::string> sendTargets() const;
    /** Runs the backend's SetValue for a side-panel field, keeping the panel (and the field being dragged) in place. */
    void setValueFromField(const nlohmann::json& call);

private:
    void openAddMenu(QPointF scenePos, const WireDrag* wire);
    void selectAfterRefresh(const std::vector<std::string>& names);
    void showError(const QString& message);
    /** panel false: only the canvas needs it, so a field being scrubbed in the side panel survives. */
    void scheduleRefresh(bool panel = true);
    void refreshSendButton();
    void refresh(bool panel = true);
    void refreshGroups();
    void rebuildPathBar();
    /** Drops the path past a group that's on it; any other group starts a new path. */
    void retracePath(const QString& name);
    void rebuildSidePanel();
    void followSelection();
    void followOwner(const GraphBackend::Owner& owner);
    /** The followed owner's path when it runs the group on show, else empty. */
    std::string ownerInView() const;
    /** The followed owner's input values by plug, to tell real edits from evaluation writes. */
    nlohmann::json shownValues() const;
    void buildToolbar();
    /** Runs a backend verb from the UI; failures go to the log. */
    nlohmann::json run(std::string_view verb, const nlohmann::json& call) const;
    QWidget* buildInterface(QWidget* panel, const nlohmann::json& info);
    void editInputInfo(const std::string& input, const nlohmann::json& current);
    /** A Send's name field, or a Receive's pick of the Sends in the group. */
    QWidget* portalNameEditor(QWidget* parent, const nlohmann::json& info, const std::string& node, bool receive);
    /** Rows editing an item list a zone, bundle node or Evaluate Closure carries (gnSetItems). */
    QWidget* itemListEditor(QWidget* parent, const nlohmann::json& list);

    GraphBackendPtr mBackend;
    QString mGroup;
    QString mAnnouncedGroup;
    QString mSelectedNode;
    /** The owner the selection led to; Parameters edit its values, which are what the scene uses, not the group's defaults. */
    QString mOwner;
    nlohmann::json mShownValues;
    bool mValueCheck = false;
    /** What the canvas was last built from, so a refresh with an unchanged group stamp can skip the rebuild. */
    std::optional<size_t> mBuiltStamp;
    QString mBuiltGroup;
    QString mBuiltOwner;
    nlohmann::json mBuiltInfo;
    std::set<std::string> mSelectNext; // selected by the next refresh, after a paste or duplicate
    QWidget* mBar = nullptr;
    QComboBox* mGroups = nullptr;
    /** The groups entered on the way to mGroup, outermost first. */
    std::vector<QString> mPath;
    QWidget* mPathBar = nullptr;
    NodeCanvas* mCanvas = nullptr;
    QScrollArea* mSide = nullptr;
    QTimer* mRefresh = nullptr;
    QToolButton* mSendBtn = nullptr;
    int mListener = 0;
    int mExecuteListener = 0;
    int mBackendListener = 0;
    int mPrefsListener = 0;
    bool mRefreshing = false;
    bool mPanelDirty = false;
    /** A refresh came while hidden: it runs when the editor shows. */
    bool mStaleHidden = false;

protected:
    void showEvent(QShowEvent* e) override;

private:
};

/**
 * Registers the geometry-nodes backend, "window.nodeEditor", "gn.newGroup" and the nodeEditor.* editing actions. The
 * window and new-group actions need the show hook to bring the dock up.
 */
LILA_UI_API void registerNodeEditorActions();

/** How the actions show the editor: the main window raises its dock. */
LILA_UI_API void setNodeEditorShowHook(std::function<void()> show);
} // namespace lila::ui
