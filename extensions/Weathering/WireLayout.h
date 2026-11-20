#pragma once

#include "GenCommon.h"

/**
 * Wires: where cables hang from. Objects are read by their world bounds: tall and thin ones are poles (the wire leaves
 * from the top, on an insulator), small ones are wall brackets (the wire leaves from them), anything else is a building
 * whose two corners nearest the run carry a bracket standing off the wall. Anchors are then chained in order along the
 * run, a span between neighbours no further apart than maxSpan.
 */
namespace weathering::wires
{
using gen::Bounds;
using gen::Index;
using gen::Mesh;

enum class Kind
{
    Pole,
    Bracket,
    Corner,
    Point,
};

struct Anchor
{
    glm::dvec3 at{0.0};
    Kind kind = Kind::Point;
    glm::dvec3 out{1, 0, 0}; // horizontal, away from the host (corners and brackets)
    int host = -1;
    double poleBase = 0.0;    // poles placed along a curve: the ground under them
};

struct Options
{
    double cornerHeight = 0.75; // of a building's height
    double standoff = 25.0;     // cm off a building's corner
    double insulator = 0.0;     // cm an insulator adds on top of a pole
    double maxSpan = 3000.0;    // cm
};

/** Anchors on objects, given their world bounds. */
std::vector<Anchor> anchorsOn(std::span<const Bounds> hosts, const Options& o);

/** Anchors along a polyline every `spacing` cm, `height` above it (poles to be placed under them). */
std::vector<Anchor> anchorsAlong(std::span<const glm::dvec3> line, double spacing, double height);

/** Runs of anchor indices in order along the main direction, split where neighbours are further apart than maxSpan. */
std::vector<std::vector<int>> chain(std::span<const Anchor> anchors, double maxSpan, bool keepOrder = false);
} // namespace weathering::wires
