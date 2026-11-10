#pragma once

#include "core/Export.h"

#include "geo/Mesh.h"

#include <glm/glm.hpp>

#include <array>
#include <memory>
#include <span>
#include <vector>

/**
 * Soft Net: a network of curves bound to a surface shapes it. Each curve sample is a blend of three surface points (a
 * barycentric soft constraint on the mesh as it is, never cut); moving the curves moves the surface by a smooth
 * displacement that meets the samples and fades with distance.
 *
 * Every line between two knots is a cubic Bezier: its ends are the knots and its inner control points are the knots'
 * tangent handles. A sample at parameter t on a line moves by the Bezier of the control points' moves, so the whole
 * deformation is linear in those moves (the net's degrees of freedom) and is precomputed per point as an influence
 * table, evaluated each frame as a sparse product.
 */
namespace lila::deform::softnet
{
/** A point on a surface: a blend of a triangle's three points. */
struct SurfacePoint
{
    std::array<uint32_t, 3> points{};
    glm::dvec3 bary{0.0};
};

/** Where a surface point sits on points. */
inline glm::dvec3 at(const SurfacePoint& s, std::span<const glm::dvec3> points)
{
    return points[s.points[0]] * s.bary.x + points[s.points[1]] * s.bary.y + points[s.points[2]] * s.bary.z;
}

/**
 * A curve sample. A knot's own sample has run -1 and moves with knot. A line sample has its run and its Bezier
 * parameter t.
 */
struct Sample
{
    SurfacePoint at;
    int run = -1;
    int knot = -1;
    double t = 0.0;
};

/** A line between two knots: its ends' knots and their tangent handles (indices into the net's tangents). */
struct Run
{
    std::array<int, 2> knot{-1, -1};
    std::array<int, 2> tangent{-1, -1};
};

/** The net's degrees of freedom: one per knot (its position), then one per tangent handle. */
inline size_t dofCount(size_t knots, size_t tangents) { return knots + tangents; }

/** The cubic Bernstein weights at t. */
LILA_SCENE_API std::array<double, 4> bernstein(double t);

/** One point's share of each degree of freedom it feels. */
struct Influence
{
    uint32_t dof = 0;
    float weight = 0.0f;
};

/**
 * The precomputed deformation for one mesh: per point, the degrees of freedom that move it and by how much. The
 * displacement minimises the bi-Laplacian (over the mesh's edges), a screening term that makes it fade over reach,
 * and the samples' misses weighted by stiffness.
 */
struct LILA_SCENE_API InfluenceTable
{
    std::vector<uint32_t> offsets; // per point, into entries; size points + 1
    std::vector<Influence> entries;
    size_t dofs = 0;

    size_t points() const { return offsets.empty() ? 0 : offsets.size() - 1; }
    /** The displacement of every point for the degrees of freedom's moves (one per dof). */
    void apply(std::span<const glm::dvec3> moves, std::span<glm::dvec3> points) const;
};

/**
 * Builds the influence table over topology's points and edges. edgeLength is the mean edge length at bind, which reach
 * is measured against; entries below cutoff times their dof's largest are dropped. Empty when there is nothing to solve.
 */
LILA_SCENE_API InfluenceTable buildInfluence(const geo::Mesh& topology, std::span<const Sample> samples, std::span<const Run> runs, size_t knots,
                                             size_t tangents, double edgeLength, double reach, double stiffness, double cutoff = 1e-6);

/** Each sample's delta for the degrees of freedom's moves: what the influence table is fitted to. */
LILA_SCENE_API std::vector<glm::dvec3> sampleDeltas(std::span<const Sample> samples, std::span<const Run> runs, size_t knots,
                                                    std::span<const glm::dvec3> moves);

/**
 * What orients a knot: its own surface point, a point a short way along each line through it, those lines' directions
 * in its rest frame, and for knots off junctions the junctions whose turn it takes (inverse-distance weights).
 */
struct KnotRig
{
    SurfacePoint at;
    std::vector<SurfacePoint> along;
    std::vector<glm::dvec3> restDirections;
    std::vector<std::pair<int, double>> blend;
    glm::dmat3 rest{1.0};
    /** Where the knot sat at bind, in the body's object space. */
    glm::dvec3 origin{0.0};
    int degree = 0;
};

/**
 * The knots' frames on points: origin on the surface, z the surface normal, x turned to fit the lines through it.
 * Junctions (3 or more lines) take the best-fit turn of their lines about the normal after carrying the rest frame
 * onto it; other knots take the inverse-distance blend of the junctions' turns, or their own lines' fit when the net
 * has no junction.
 */
LILA_SCENE_API std::vector<glm::dmat4> knotFrames(std::span<const KnotRig> knots, std::span<const glm::dvec3> points);

/** The rest frame (z the normal at the surface point, x along the first line) of a knot rig bound on points. */
LILA_SCENE_API glm::dmat3 restFrame(const KnotRig& k, std::span<const glm::dvec3> points);
/** The surface normal under a surface point: its triangle's. */
LILA_SCENE_API glm::dvec3 normalAt(const SurfacePoint& s, std::span<const glm::dvec3> points);
} // namespace lila::deform::softnet
