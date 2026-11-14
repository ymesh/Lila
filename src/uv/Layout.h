#pragma once

#include "core/Export.h"

#include "uv/Chart.h"

#include <glm/glm.hpp>

#include <span>
#include <vector>

namespace lila::uv
{
enum class OrientMode : uint8_t
{
    BoundingBox, // smallest bounding rectangle, long side along U
    Principal,   // principal axis of the uv area along U
    LongestEdge, // longest boundary edge snapped to the nearest axis
    Grid,        // polygon edges, weighted by length, turned onto the axes: a quad grid runs along U and V
    Flow,        // the surface's flow (curvature and creases) along U and V
    Upright,     // edge grid when there is one, else flow when it costs little room, else bounding box
};

/**
 * The turn that puts the chart's polygon edges, weighted by length, closest onto the axes, and how much they agree on
 * it: 1 for a perfect grid, near 0 for edges in every direction.
 */
LILA_SCENE_API double gridAngle(const Chart& chart, double* agreement = nullptr);

/** Rotates the chart about its uv centroid. */
LILA_SCENE_API void orientChart(Chart& chart, OrientMode mode = OrientMode::BoundingBox);

/** Rotation (radians) that orients the points' minimum-area rectangle to the axes, long side along U. */
LILA_SCENE_API double minAreaRectAngle(std::span<const glm::dvec2> points, glm::dvec2* size = nullptr);

/** Counter-clockwise convex hull (Andrew's monotone chain). */
LILA_SCENE_API std::vector<glm::dvec2> convexHull(std::vector<glm::dvec2> points);

/** Rotates by angle (radians) and scales about a pivot. */
LILA_SCENE_API void transformChart(Chart& chart, const glm::dvec2& pivot, double angle, const glm::dvec2& scale = glm::dvec2(1.0),
                    const glm::dvec2& offset = glm::dvec2(0.0));

enum class StraightenResult : uint8_t
{
    Failed,
    Grid,      // every face a quad in a regular grid: rows and columns made straight
    Rectangle, // boundary laid on a rectangle through its four sharpest corners, interior relaxed
};

/**
 * RizomUV-style rectify. Quad grids (strips, panels, cylinders cut once) become exact grids with column
 * widths and row heights from their 3D edge lengths; other disk islands are fitted to a rectangle. The
 * result keeps the island's previous uv centroid and area when it had valid uvs.
 */
LILA_SCENE_API StraightenResult straightenChart(Chart& chart);

/** Moves and scales the chart so its centroid and area match the reference ones (when positive). */
LILA_SCENE_API void fitChart(Chart& chart, const glm::dvec2& centroid, double area);
} // namespace lila::uv
