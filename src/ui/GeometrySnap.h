#pragma once

#include "core/Export.h"

#include "scene/Plug.h"
#include "ui/ViewInfo.h"

#include <glm/glm.hpp>

#include <QPointF>

#include <cstdint>
#include <optional>
#include <vector>

namespace lila::scene
{
class Graph;
}

/**
 * V and C snapping: the world point nearest the cursor on screen among vertices and edges of meshes and curves, and
 * the centers of locators and joints.
 */
namespace lila::ui::geometrySnap
{
/** Keeps whichever candidate projects closest to the cursor, within a pixel radius. */
class LILA_UI_API Nearest
{
public:
    Nearest(QPointF px, const ViewInfo& view, double radiusPx) : mPx(px), mView(view), mBest(radiusPx) {}

    void point(const glm::dvec3& world);
    /** The point of the segment nearest the cursor on screen. */
    void segment(const glm::dvec3& a, const glm::dvec3& b);

    const std::optional<glm::dvec3>& hit() const { return mHit; }

private:
    QPointF mPx;
    const ViewInfo& mView;
    double mBest;
    std::optional<glm::dvec3> mHit;
};

/**
 * Offers a curve node's targets in world space: its points and NURBS edit points (SnapVertex) and its evaluated shape
 * (SnapEdge). Nothing for a node that isn't a visible curve shown by the active view's isolation. Components in moving
 * (curve component ids) are left out, and so are the shape and edit points of any spline holding one, as it is reshaping.
 */
LILA_UI_API void addCurve(Nearest& n, scene::Graph& g, scene::NodeId id, unsigned snaps, const std::vector<uint32_t>& moving = {});

/** Whether V snapping offers the node's world position: a visible locator or joint, shown by the active view's isolation. */
LILA_UI_API bool isPointObject(scene::Graph& g, scene::NodeId id);

/** Offers a locator's or joint's center (SnapVertex). Returns whether the node is one, offered or not. */
LILA_UI_API bool addPointObject(Nearest& n, scene::Graph& g, scene::NodeId id, unsigned snaps);
} // namespace lila::ui::geometrySnap
