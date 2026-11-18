#pragma once

#include "core/Export.h"

#include "geo/Bvh.h"
#include "geo/Knife.h"
#include "geo/Symmetry.h"
#include "scene/Plug.h"
#include "ui/StepHistory.h"
#include "ui/ViewportTool.h"

#include <map>
#include <memory>
#include <optional>
#include <vector>

class QObject;

namespace lila::ui
{
/**
 * Knife, after Maya's Multi-Cut and Blender's Knife: click points on a mesh to draw a cut across its surface, with a live
 * preview of the cut and the vertices it will make.
 *
 *   click                add a cut point: snaps to vertices and edges near the cursor, else a point inside the face
 *   drag                 the new point follows the cursor (still snapping) until the button comes up
 *   Shift                edge points snap to steps of the snap step (10% by default, so midpoints too)
 *   Ctrl+click an edge   insert an edge loop across its ring there
 *   Ctrl+Alt+click an edge     delete it, joining its two faces (highlighted while hovering)
 *   click the first point   close the cut and finish it
 *   Enter or RMB         finish the cut; the tool stays for the next one
 *   Backspace or Z       remove the last point (Ctrl+Z too: whatever undo is bound to); scene undo waits until the tool is left
 *   Shift+Z              put the removed point back (whatever redo is bound to)
 *   A                    toggle the angle constraint: the line from the last point snaps to 45 degree steps on screen
 *   X                    toggle cut through: hidden faces under the line are cut too
 *   Esc                  drop the points, or leave the tool when there are none
 *
 * Every cut is one polyKnife call, so it undoes and echoes like any other command, and follows the mesh's symmetry.
 */
class LILA_UI_API KnifeTool : public ViewportTool
{
public:
    KnifeTool();
    ~KnifeTool() override;
    const char* name() const override { return "Knife"; }

    void activate() override;
    void deactivate() override;
    bool mousePress(QMouseEvent* e, const ViewInfo& view) override;
    bool mouseMove(QMouseEvent* e, const ViewInfo& view) override;
    bool mouseRelease(QMouseEvent* e, const ViewInfo& view) override;
    bool keyPress(QKeyEvent* e, const ViewInfo& view) override;
    bool hotkey(const QKeyEvent& e, std::string_view action) override;
    void draw(render::FrameState& s, OverlayPainter& p, const ViewInfo& view) override;
    std::optional<QCursor> cursor() const override;

    /** Keys the tool takes before the hotkeys see them; true when used. */
    bool key(int key, Qt::KeyboardModifiers mods);

private:
    struct Target
    {
        scene::MeshPtr mesh; // the cage: the ids the command takes
        glm::dmat4 world{1.0};
        geo::MeshBvh bvh; // world space
        std::optional<geo::SymmetryMap> symmetry;
    };

    /** A cut point and where it sits in the world. */
    struct Spot
    {
        geo::KnifePoint point;
        glm::dvec3 world{0.0};
    };

    void refresh();
    Target* target(scene::NodeId id);
    /** edgeOnly: the nearest edge even beside a vertex, never snapped onto its ends. */
    std::optional<std::pair<scene::NodeId, Spot>> pick(QPointF px, const ViewInfo& view, bool snapStep, bool edgeOnly = false);
    QPointF constrained(QPointF px, const ViewInfo& view, size_t from) const;
    bool visible(const Target& t, const glm::dvec3& world, const ViewInfo& view) const;
    geo::KnifeOptions options(const Target& t, const ViewInfo& view) const;
    void finish(bool closed);
    void insertLoop(const Spot& spot);
    /** loop: the whole edge loop through the edge. */
    void deleteEdge(const Spot& spot, bool loop);
    /** Ctrl+Alt with no cut under way: a click deletes the hovered edge. */
    bool deleting() const;
    void cancel();
    void stepBack();
    void stepForward();

    std::map<scene::NodeId, Target> mTargets;
    scene::NodeId mNode = scene::kNoNode; // the mesh being cut, fixed by the first point
    std::vector<Spot> mPoints;
    /** Points undo took off, with the mesh they were on. */
    StepHistory<std::pair<scene::NodeId, Spot>> mHistory;
    std::optional<std::pair<scene::NodeId, Spot>> mHover;
    bool mHoverFirst = false; // the cursor is on the first point, so a click closes the cut
    bool mDragging = false; // the left button is down on the point just placed, which follows the cursor
    QPointF mCursor;
    std::string mCursorView;
    ViewInfo mView;
    bool mAngle = false;
    bool mThrough = false;

    std::unique_ptr<QObject> mFilter;
};

/** Registers "mesh.knife". */
LILA_UI_API void registerKnifeActions();
} // namespace lila::ui
