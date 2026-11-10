#pragma once

#include "core/Export.h"

#include <glm/glm.hpp>

#include <span>
#include <vector>

/**
 * Bend bones: a chain of joints is one smooth curve rather than straight segments, and a mesh bound to it bends along
 * that curve. A chain is one influence, so a limb binds wholly to its chain and only the joins between chains blend.
 *
 * The deformation keeps each strand of the mesh (the line of points at one offset from the curve) evenly spread along its
 * own length: the inside of a bend compresses evenly instead of bunching at the joint, the outside stretches evenly, and
 * offsets are held short of the bend's centre so the inside never folds through itself. Stretching the chain thins the
 * mesh to keep its volume. Our own formulation, measured against a high-subdivision reference in the tests.
 */
namespace lila::deform::bendbones
{
/** A chain's joints in order: where each is and its world rotation. */
struct Chain
{
    std::vector<glm::dvec3> position;
    std::vector<glm::dmat3> rotation;
    /** Bone CVs: extra points the curve passes through, per bone (between joint i and i + 1), in order along it. */
    std::vector<std::vector<glm::dvec3>> cvs;
};

/** The chain as a sampled curve: positions, rotation-minimising frames (tangent first) and arc length per sample. */
struct Curve
{
    std::vector<glm::dvec3> point;
    std::vector<glm::dmat3> frame; // columns: tangent, normal, binormal
    std::vector<double> length;    // arc length from the start
    std::vector<int> knot;         // the sample at each joint
};

/**
 * The curve through the chain's joints (and bone CVs), centripetal Catmull-Rom, samples per span. Frames start from the
 * first joint and are carried along without twisting, then each joint's twist relative to restNormals (one per joint, in
 * its own space, from bind) is ramped in between. Empty restNormals: no twist correction.
 */
LILA_SCENE_API Curve build(const Chain& chain, const std::vector<glm::dvec3>& restNormals = {}, int samplesPerSpan = 16);

/** Each joint's normal at rest, in its own space, so later twists can be measured against it. */
LILA_SCENE_API std::vector<glm::dvec3> restNormals(const Chain& chain, const Curve& rest);

/** Where a point sits on a chain: its joint segment and the arc-length fraction along it, and its offset in the frame. */
struct Place
{
    int segment = 0;
    double fraction = 0.0;
    glm::dvec3 offset{0.0}; // in the curve frame: along the tangent, normal, binormal
    double distance = 0.0;  // from the curve, at bind
};

/** The nearest place on the curve to p. */
LILA_SCENE_API Place locate(const Curve& rest, const glm::dvec3& p);

struct Options
{
    /** 0 the offsets keep their size as the chain stretches, 1 they thin to keep the volume. */
    double volume = 1.0;
    /** 0 points ride the curve by its own arc length, 1 each strand stays evenly spread along its own length. */
    double evenBend = 1.0;
};

/** Where a point placed on the rest curve goes on the posed one. */
LILA_SCENE_API glm::dvec3 deformPoint(const Curve& rest, const Curve& posed, const Place& place, const Options& o);

/** Points bound to chains, each with its weights over them: CSR over the points, chain ids. */
struct Binding
{
    std::vector<uint32_t> offsets{0};
    std::vector<uint32_t> chain;
    std::vector<double> weight;
    std::vector<Place> place; // one per (point, chain) entry
};

/**
 * Binds points to the nearest chains: each point fully to its nearest chain, blending into the next nearest only near the
 * join, over blend times the nearer distance.
 */
LILA_SCENE_API Binding bindPoints(std::span<const Curve> rest, std::span<const glm::dvec3> points, double blend = 0.35);

/** The bound points on the posed chains. */
LILA_SCENE_API void deformPoints(std::span<const Curve> rest, std::span<const Curve> posed, const Binding& b, const Options& o,
                                 std::span<glm::dvec3> points);

/** The curve's frame (tangent, normal, binormal as columns) and point at arc length s, as a matrix. */
LILA_SCENE_API glm::dmat4 matrixAt(const Curve& c, double s);

/** Arc lengths splitting each bone into perBone even segments, plus the chain's end: where baked segment joints go. */
LILA_SCENE_API std::vector<double> segmentLengths(const Curve& c, int perBone);
} // namespace lila::deform::bendbones
