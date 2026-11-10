#pragma once

#include "core/Export.h"

#include <glm/glm.hpp>

/**
 * Scalar, vector and matrix utility nodes with Maya's names, attributes and math: multiplyDivide, plusMinusAverage,
 * condition, reverse, clamp, setRange, remapValue, blendColors, blendTwoAttr, blendWeighted, pairBlend, choice,
 * distanceBetween, vectorProduct, unitConversion, multDoubleLinear, addDoubleLinear, and the matrix nodes multMatrix,
 * addMatrix, wtAddMatrix, inverseMatrix, transposeMatrix, holdMatrix, fourByFourMatrix, decomposeMatrix,
 * composeMatrix, blendMatrix, pickMatrix and aimMatrix, and closestPointOnMesh. Rig networks wire them between transform channels the way
 * Maya's utility nodes are, so imported Maya rigs keep working.
 *
 * Angles are degrees, as on transforms. Matrices are Lila's column-vector form: a Maya product A * B (A applied
 * first) is B * A here, which is what multMatrix computes.
 *
 * Also the two constraints Lila's constraint set lacks, as plain nodes: scaleConstraint (weighted target scales
 * times an offset) and poleVectorConstraint (the weighted target position relative to a pivot, for an ikHandle's
 * poleVector).
 */
namespace lila::scene::mathnodes
{
/** Maya's matrix split: m = T * R * Sh * S (column vectors), shear (xy, xz, yz), rotation as Euler degrees in order. */
struct Decomposed
{
    glm::dvec3 translate{0.0};
    glm::dmat3 rotation{1.0};
    glm::dvec3 scale{1.0};
    glm::dvec3 shear{0.0};
};

LILA_SCENE_API Decomposed decompose(const glm::dmat4& m);
LILA_SCENE_API glm::dmat4 compose(const Decomposed& d);

LILA_SCENE_API void registerTypes();
} // namespace lila::scene::mathnodes
