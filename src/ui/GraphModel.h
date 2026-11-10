#pragma once

#include "core/Export.h"
#include "scene/Graph.h"
#include "ui/GraphEditorCommon.h"

#include <QPoint>

#include <cmath>
#include <functional>
#include <map>
#include <set>
#include <span>
#include <string>
#include <vector>

class QMenu;

namespace lila::ui::graphed
{
enum class RotationDisplay : uint8_t
{
    Euler,
    Quaternion,
    Both,
};

/** What a click or box does to the key selection. */
enum class SelectOp : uint8_t
{
    Replace,
    Toggle,
    Add,
    Remove,
};

/** The Graph Editor's modifiers, the same for a click and a box: Shift toggles, Ctrl removes, Ctrl+Shift adds. */
LILA_UI_API SelectOp selectOpFor(bool shift, bool ctrl);

/** A drag in progress on a model's keys: what its previews show and what commit runs. */
struct KeyEdit
{
    enum class Kind : uint8_t
    {
        None,
        Transform, // move, scale or region scale of the selected keys
        Tangent, // one key's tangent handle
        TangentRotate, // every selected key's tangents turned about the key
        Tween,
    };
    Kind kind = Kind::None;
    /** Previews and commits only once the drag has really moved. */
    bool moved = false;

    /** Transform: the time parts shared, the value offset in display units, the value pivots per channel. */
    anim::KeyTransform xform;
    double displayOffset = 0.0;
    std::map<NodeId, double> valuePivots;
    /** Falloff radius in seconds; 0 is off. */
    double falloff = 0.0;

    KeySel tangent;
    int tangentSide = 1; // 1 the out handle, -1 the in handle
    double tangentAngle = std::nan("");
    double tangentWeight = std::nan("");

    /** TangentRotate: the turn in screen degrees, and the view's pixels per value unit over pixels per second. */
    double rotateDelta = 0.0;
    double aspect = 1.0;

    anim::BlendMode tweenMode = anim::BlendMode::Neighbour;
    double tweenAmount = 0.0;
};

/**
 * The Graph Editor's data without its widget: the channels of a set of nodes, their curves, the key selection, and every
 * key and tangent edit, each one undo step. The Graph Editor panel and Puppeteer's Curve Overlay both draw from one.
 */
class LILA_UI_API GraphModel
{
public:
    GraphModel() = default;
    virtual ~GraphModel();
    GraphModel(const GraphModel&) = delete;
    GraphModel& operator=(const GraphModel&) = delete;

    std::vector<Channel> channels;
    /** Keyed by channel id; a rotation channel's holds its derived keys. */
    std::map<NodeId, anim::Curve> curves;
    /** Rotation curves by their own node id, loop applied. */
    std::map<NodeId, anim::RotationCurve> rotations;
    /** Channels whose curve has a loop key. */
    std::set<NodeId> loopCurves;
    std::vector<KeySel> sel;
    NodeId activeCurve = scene::kNoNode;
    KeyEdit edit;

    /**
     * The animated channels of nodes, in attribute order: scalar curves as they are, rotation curves as their Euler X, Y,
     * Z (and quaternion W, X, Y, Z when rotation asks). Each gets a gray level and dash that differ from its neighbours'.
     */
    static std::vector<Channel> gather(scene::Graph& g, const std::vector<NodeId>& nodes, RotationDisplay rotation);
    /** Reads every channel's curve from the scene, dropping selected keys that are gone. */
    void reloadCurves();

    /** What commands call a channel: its curve node, or for a rotation channel its plug. Empty when read only. */
    std::string commandTarget(NodeId id) const;
    const Channel* channelOf(NodeId curve) const;
    bool isSelected(NodeId curve, double t) const;
    std::vector<double> selectedTimes(NodeId id) const;
    /** Curves edits apply to: those with selected keys, else the clicked one, else every shown one. */
    std::vector<NodeId> targetCurves() const;
    /** Curves with selected keys, then the clicked one: those drawn on top. */
    std::vector<NodeId> activeSet() const;
    void dropMissing();
    /** The selection as a keys argument: [{curve, times}], read-only channels left out. */
    nlohmann::json keysArgument() const;
    /** The curves edits apply to as command objects (curve nodes, or plugs for rotation channels). */
    nlohmann::json targetNames() const;
    double defaultFor(const Channel& ch) const;

    /** Applies op with keys: a click passes one key, a box every key inside it. Replace with none clears. */
    void select(std::span<const KeySel> keys, SelectOp op);

    /** Tangent type ("auto", "spline", "linear", "flat", "step", "clamped") on the selected keys, or the target curves. */
    void applyTangent(const char* type);
    /** Unifies (lock) or breaks the selected keys' tangents. */
    void lockTangents(bool lock);
    void setWeighted(bool weighted);
    /** Returns how many keys were deleted. */
    size_t deleteSelectedKeys();

    /** The move or scale in progress for one channel. */
    anim::KeyTransform transformFor(NodeId id) const;
    bool transforming() const { return edit.kind == KeyEdit::Kind::Transform && edit.moved; }
    bool tweening() const { return edit.kind == KeyEdit::Kind::Tween; }
    /** During a key drag the selection still holds the old times; maps a previewed key back. */
    double keyTimeBeforeDrag(NodeId curve, double t) const;
    anim::RotationCurve previewRotation(NodeId realId) const;
    /** The channel's keys with the edit in progress applied. */
    anim::Curve preview(NodeId id) const;
    /** Weight (0..1) the falloff gives an unselected key during a drag; 0 when falloff is off. */
    double falloffWeightAt(NodeId id, double t) const;
    /** The channel's value over time as drawn, with the edit in progress and the curve's modifiers applied. */
    std::function<double(double)> sampler(NodeId id, const anim::Curve& c) const;
    double screenAngleOf(const Channel& ch, double slope) const;
    double slopeOfScreenAngle(const Channel& ch, double degrees) const;

    /** Mirrors the edit in progress into the scene so the viewports follow it. */
    void pushPreviews();
    void dropPreviews();
    /** Commits the edit with one command or undo chunk, then clears it. False when there was nothing to commit. */
    bool commit();
    /** Ends the edit, putting back what its previews showed. */
    void cancel();

    /** Called after the key selection changes through select() or an edit. */
    std::function<void()> onSelectionChanged;

private:
    bool commitTransform();
    bool commitTangent();
    bool commitTangentRotate();
    bool commitTween();
    void selectionChanged();
};

/** Tangent types the menus offer, in toolbar order: their command names. */
inline constexpr const char* kTangentTypes[] = {"auto", "spline", "linear", "flat", "step", "clamped"};
/** The marking menu the Graph Editor and the Curve Overlay open on a right-button drag. */
inline constexpr const char* kTangentMarkingMenu = "graphTangents";

/** Tangent types, Break, Unify, Weighted and Non-Weighted for model: the Graph Editor's context menu and the overlay's. */
LILA_UI_API void fillTangentMenu(QMenu* menu, GraphModel& model);
/**
 * Opens the tangent marking menu at global for model, picked by a flick of button. The graphTangents.* actions it runs
 * act on model until another menu opens or model is destroyed.
 */
LILA_UI_API bool popupTangentMarkingMenu(GraphModel& model, QPoint global, Qt::MouseButton button);
/** The model the tangent actions act on: the last one a tangent menu opened for, else the Graph Editor's. */
LILA_UI_API GraphModel* tangentTarget();
/** The graphTangents.* actions. */
LILA_UI_API void registerTangentActions();
} // namespace lila::ui::graphed
