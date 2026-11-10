#pragma once

#include "core/Export.h"

#include <glm/glm.hpp>

#include <span>
#include <vector>

/** Cleaning a hand-drawn stroke into the few points a run needs. */
namespace lila::layout
{
struct FitOptions
{
    /** The hand's error: points this close count as the same place. A few screen pixels in world units. */
    double tolerance = 0.1;
    /** 0 keeps the drawn wobble, 1 fits the fewest points. */
    double cleanup = 0.5;
    /** Turns sharper than this, in degrees, become corners. */
    double cornerAngle = 50.0;
    /** Every piece between corners becomes straight. */
    bool straight = false;
    /** The last piece becomes straight (the stroke ended with a hold). */
    bool straightenLast = false;
};

/** One stretch between corners: its points from start to end; only the ends when straight. */
struct FitPiece
{
    std::vector<glm::dvec3> points;
};

struct FitResult
{
    std::vector<FitPiece> pieces;
    /** The stroke ended where it began: the last piece ends on the first piece's start. */
    bool closed = false;
    /** With closed: the start is a corner rather than a point the line passes through. */
    bool closedCorner = false;
};

/** Cleans a stroke (points on the surface, in drawing order) into corner-separated pieces. */
LILA_SCENE_API FitResult fitStroke(std::span<const glm::dvec3> stroke, const FitOptions& opts = {});

/** The points a loose stretch keeps at a cleanup amount: what Loosen's refit and overdraw use. */
LILA_SCENE_API std::vector<glm::dvec3> fitPoints(std::span<const glm::dvec3> points, double tolerance, double cleanup);
} // namespace lila::layout
