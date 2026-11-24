#pragma once

#include "core/Export.h"

#include "scene/Plug.h"
#include "ui/ValueReset.h"

#include <QElapsedTimer>
#include <QPoint>
#include <QWidget>

#include <map>
#include <optional>
#include <string>

class QDoubleSpinBox;
class QLabel;
class QLineEdit;
class QListWidget;
class QToolButton;
class QTableWidget;
class QTableWidgetItem;
class QTimer;

namespace lila::ui
{
/**
 * Maya's virtual slider from outside the channel box: a middle drag in a viewport drives the channels highlighted in a
 * visible channel box. begin() is false when none are highlighted, and the viewport then handles the drag itself.
 */
namespace channelSlider
{
LILA_UI_API bool begin(QPoint globalPos);
LILA_UI_API void drag(QPoint globalPos, Qt::KeyboardModifiers mods);
LILA_UI_API void end();
LILA_UI_API bool active();
} // namespace channelSlider

/**
 * Maya's channel box: the keyable channels of the lead selected node, edited on the lead, or with Alt held on every
 * selected node that has the channel. A filter field and chips narrow the rows; a stripe on each name shows what drives
 * it (key, breakdown, driven key, expression, connection, constraint, mute, lock), and a click on it jumps there.
 * Values accept expressions (`+=5`, `*=2`, `=10`). Left drag over channel names selects a range (Ctrl toggles, Shift
 * extends); middle drag anywhere scrubs the highlighted channels (Ctrl slow, Shift fast); the whole drag is one undo step. A middle click on a channel name
 * resets it (and every highlighted channel with it) to its default.
 */
class LILA_UI_API ChannelBox : public QWidget
{
    Q_OBJECT

public:
    explicit ChannelBox(QWidget* parent = nullptr);
    ~ChannelBox() override;

protected:
    bool eventFilter(QObject* obj, QEvent* e) override;

private:
    struct Row
    {
        std::string attr;  // long name, or component long name ("translateX")
        int attrIndex = -1;
        int component = -1;
        scene::PlugType type = scene::PlugType::Float;
        /** An INPUTS row edits this node only; kNoNode rows edit every selected node. */
        scene::NodeId node = scene::kNoNode;
        bool header = false;
        /** Shown instead of the attr's nice name (a morph target's name for its weight). */
        std::string label;
        /** An array's element, whose plug name attr holds ("targets[0].weight"). */
        int element = -1;
    };

    /** The rows the lead node needs: its channels, then INPUTS (modifiers top first, then the generator). */
    std::vector<Row> buildRows(scene::NodeId lead) const;

    void scheduleRefresh();
    void refresh();
    void commitCell(QTableWidgetItem* item);
    /** Applies a value (or expression) to every selected node that has the channel. */
    void apply(const Row& row, const QString& text);
    /** The objects value edits go to: the lead, or every selected one while Alt is held. */
    std::vector<std::string> targets() const;
    /** Every selected object, lead first: menu actions (key, lock, freeze) act on all of them. */
    std::vector<std::string> allTargets() const;

public:
    /** What drives or marks a channel, as bits; also what the filter chips test. */
    enum State : uint32_t
    {
        kKeyed = 1,
        kKeyNow = 2,
        kBreakdown = 4,
        kDrivenKey = 8,
        kExpression = 16,
        kConnected = 32,
        kConstraint = 64,
        kMuted = 128,
        kLocked = 256,
    };
    /** The state of a plug and, when something drives it, that source's node. */
    static uint32_t stateOf(scene::PlugRef plug, scene::NodeId* source = nullptr);

private:
    /** Whether a row passes the filter text and chips. */
    bool passesFilter(const Row& row, scene::NodeId lead) const;
    void refreshStrip();
    /** Shows the lead's size; hidden for objects without geometry of their own. */
    void refreshSize();
    /** Scales every edited object along its own axis so its size there becomes cm. */
    void setSize(int axis, double cm);
    /** After a lead-only edit with several objects selected: "Changed X only. Hold Alt to change all N." */
    void noteEdit(const Row& row);
    /** Opens what drives the row: the Graph Editor for keys, the source node for anything else. */
    void jumpToSource(int row);
    /** A plug dragged onto a channel: connectAttr, or a driven key with Alt held. */
    void dropPlug(const std::string& source, const std::string& target);
    /** Scrub speed, hyperbolic, viewport middle drag, decimals, namespaces and the hint. */
    void showSettings(QWidget* anchor);
    int mDragRow = -1;
    QPoint mDragFrom;
    /** Flips a boolean channel on every selected node, and every other highlighted boolean channel to match. */
    void toggleBool(int row);
    void beginScrub(QPoint pos);
    void scrub(QPoint pos, Qt::KeyboardModifiers mods);
    /** Writes the scrub's current values live, at most once a frame. */
    void applyScrub();
    void endScrub(bool commit);

    /** Restores the channels' defaults on every node they edit, as one undo step. */
    void resetRows(const std::vector<int>& rows);
    /** The middle-clicked row and every highlighted one with it, or just the clicked row when it isn't highlighted. */
    std::vector<int> resetTargets(int clicked) const;

    /** The highlighted channel rows, or none. */
    std::vector<int> highlightedRows() const;
    /** Copies the lead object's highlighted channel values, as "translateX 10" lines and as typed values. */
    void copyValues();
    /** Pastes copied values onto the same channels of every selected object, or one number into every highlighted channel. */
    void pasteValues();

    friend bool channelSlider::begin(QPoint);
    friend void channelSlider::drag(QPoint, Qt::KeyboardModifiers);
    friend void channelSlider::end();

    QLineEdit* mName = nullptr;
    QLineEdit* mFilter = nullptr;
    /** Filter chips by name: 0 off, 1 only channels with it, 2 only channels without it. */
    std::map<std::string, int> mChips;
    QWidget* mChipRow = nullptr;
    QListWidget* mStrip = nullptr;
    QToolButton* mNameMode = nullptr;
    QToolButton* mSync = nullptr;
    QWidget* mHint = nullptr;
    QLabel* mHintText = nullptr;
    int mViewListener = 0;
    QTableWidget* mTable = nullptr;
    QWidget* mSizeRow = nullptr;
    std::vector<QDoubleSpinBox*> mSize;
    std::vector<Row> mRows;
    scene::NodeId mNode = scene::kNoNode;
    int mListener = 0;
    int mToolListener = -1;
    bool mQueued = false;
    bool mUpdating = false;

    struct Scrub
    {
        QPoint start;
        QPoint last;
        double accum = 0.0;
        std::vector<int> rows;
        std::vector<std::pair<scene::PlugRef, scene::PlugValue>> saved;
        bool moved = false;
        bool chunk = false; // the drag's undo chunk is open
    };
    std::optional<Scrub> mScrub;
    valueReset::ClickTracker mMidClick;
    int mMidRow = -1; // the channel a middle press landed on
    QElapsedTimer mClock;
    QTimer* mScrubTimer = nullptr;
    QElapsedTimer mLastScrubApply;
};
} // namespace lila::ui
