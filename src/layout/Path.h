#pragma once

#include "core/Export.h"

#include "layout/Layout.h"
#include "layout/Surface.h"

#include <limits>
#include <span>
#include <vector>

/** Polylines on the surface: how runs evaluate, and the polyline maths the edits and fits share. */
namespace lila::layout
{
/** A run (or stroke) as points on the surface, with their normals and the distance along it to each. */
struct LILA_SCENE_API Path
{
    std::vector<glm::dvec3> points;
    std::vector<glm::dvec3> normals;
    std::vector<double> lengths;

    double length() const { return lengths.empty() ? 0.0 : lengths.back(); }
    size_t size() const { return points.size(); }
    bool empty() const { return points.size() < 2; }
    /** The point and normal at a distance along it. */
    glm::dvec3 pointAt(double s) const;
    glm::dvec3 normalAt(double s) const;
    glm::dvec3 tangentAt(double s) const;
    /** Fills lengths from points. */
    void measure();
};

/** The closest spot on a polyline to a point. */
struct PathNearest
{
    double distance = std::numeric_limits<double>::max();
    /** Distance along the polyline. */
    double s = 0.0;
    size_t segment = 0;
    glm::dvec3 point{0.0};
};

LILA_SCENE_API PathNearest nearestOnPath(std::span<const glm::dvec3> points, std::span<const double> lengths, const glm::dvec3& p);
inline PathNearest nearestOnPath(const Path& path, const glm::dvec3& p) { return nearestOnPath(path.points, path.lengths, p); }

LILA_SCENE_API double polylineLength(std::span<const glm::dvec3> points);
/** Evenly spaced points along a polyline, ends kept. */
LILA_SCENE_API std::vector<glm::dvec3> resample(std::span<const glm::dvec3> points, double step);
/** Indices of the points Douglas-Peucker keeps at tolerance eps, ends included. */
LILA_SCENE_API std::vector<size_t> simplify(std::span<const glm::dvec3> points, double eps);
/** Largest distance from the interior points to the chord between the ends. */
LILA_SCENE_API double chordDeviation(std::span<const glm::dvec3> points);

/**
 * A smooth curve through the given points (centripetal Catmull-Rom), laid on the surface: sampled every `spacing`,
 * each sample moved to the closest surface point, and long gaps between samples split at projected midpoints so it
 * never cuts through the mesh.
 */
LILA_SCENE_API Path surfaceCurve(const Surface& surface, std::span<const glm::dvec3> through, double spacing, bool closed = false);

/** Where a run's ends and controls are now, from a to b. */
LILA_SCENE_API std::vector<glm::dvec3> runThrough(const PanelLayout& layout, const Run& run, const Surface& surface);
/** A run laid on the surface. */
LILA_SCENE_API Path runPath(const PanelLayout& layout, const Run& run, const Surface& surface, double spacing);

/** Sample spacing for a layout's runs on a surface: fine enough for the narrowest line, bounded by the mesh size. */
LILA_SCENE_API double defaultSpacing(const PanelLayout& layout, const Surface& surface);
} // namespace lila::layout
