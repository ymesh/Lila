#pragma once

#include "core/Export.h"

#include "scene/Node.h"

namespace lila::scene
{
class Graph;

enum class RotateOrder : int64_t
{
    XYZ,
    YZX,
    ZXY,
    XZY,
    YXZ,
    ZYX,
};

/** Rotation matrix from Euler degrees; XYZ means X is applied first. */
LILA_SCENE_API glm::dmat4 eulerToMatrix(const glm::dvec3& degrees, RotateOrder order);
LILA_SCENE_API glm::dvec3 matrixToEuler(const glm::dmat3& rotation, RotateOrder order);
/** Euler angles of rotation in order, wrapped and flipped to lie closest to ref, so sampled rotations stay continuous. */
LILA_SCENE_API glm::dvec3 eulerNear(const glm::dmat3& rotation, RotateOrder order, const glm::dvec3& ref);

/** How new rotate keys are stored: three Euler curves, or one rotation curve with slerp or smooth interpolation. */
enum class RotationInterpolation : int64_t
{
    Euler,
    Slerp,
    Smooth,
};
/** The transform's rotationInterpolation attr, with def as its default (joints default to Smooth). */
LILA_SCENE_API AttrDef rotationInterpolationAttr(int64_t def);

/** Pref general /display/inheritVisibility (default on): hiding a node hides everything under it, as in Maya. */
inline constexpr const char* kInheritVisibilityPref = "/display/inheritVisibility";
LILA_SCENE_API bool inheritVisibility();

/**
 * Whether the viewport shows a node: its own visibility, and every DAG ancestor's too when inherit is on. Picking,
 * framing, snapping and counts all go by this rather than the node's own flag.
 */
LILA_SCENE_API bool effectivelyVisible(const Graph& g, NodeId id, bool inherit);
inline bool effectivelyVisible(const Graph& g, NodeId id) { return effectivelyVisible(g, id, inheritVisibility()); }

class LILA_SCENE_API Transform : public Node
{
public:
    enum Attr
    {
        kTranslate,
        kRotate,
        kScale,
        kRotateOrder,
        kRotatePivot,
        kScalePivot,
        kRotatePivotTranslate,
        kScalePivotTranslate,
        kVisibility,
        kParentMatrix,
        kMatrix,
        kWorldMatrix,
        kPivotOrient,
        kDisplayLayer,
        kRotateAxis,
        kRotationInterpolation,
        kSpace,
        kSpaceFrames,
        kLinkId,
        kOffsetParentMatrix,
        kCount
    };

    void compute(const EvalContext& ctx) override;

    /** Rotation applied between rotate and the parent: a joint's jointOrient, identity otherwise. */
    virtual glm::dmat4 preRotation() const { return glm::dmat4(1.0); }

    /** The R term of the local matrix: rotate in rotateOrder. Joints blend their IK solution in here. */
    virtual glm::dmat4 rotation() const;

    /** rotateAxis as a matrix (XYZ order), applied before rotate as Maya does. */
    glm::dmat4 rotateAxisMatrix() const;

    /**
     * Maya's pivot model, column vectors:
     * T * Rpt * Rp * Pre * R * Ra * Rp^-1 * Spt * Sp * S * Sp^-1  (Pre = preRotation(), Ra = rotateAxis)
     * The pivot-translate terms let a pivot move without moving the object.
     */
    static glm::dmat4 localMatrix(const Node& n);
    /** localMatrix with `rotation` standing in for the R term (rotate, or a joint's IK blend). */
    static glm::dmat4 localMatrix(const Node& n, const glm::dmat4& rotation);
    /** The terms either side of R, so localMatrix(n, R) == before * R * after. */
    static void splitLocal(const Node& n, glm::dmat4& before, glm::dmat4& after);

    /** Sets translate/rotate/scale (undoably) so the local matrix becomes m, keeping pivots. Shear is dropped. */
    static void setLocalMatrix(Graph& g, NodeId id, const glm::dmat4& m);

    static NodeType makeType();
};

class LILA_SCENE_API Locator : public Transform
{
public:
    enum Attr
    {
        kLocalScale = Transform::kCount,
        kDrawSize,
    };
    static NodeType makeType(const NodeType& base);
    /** drawSize hidden, for types built on locator that draw themselves (cameras, lights). */
    static AttrDef hiddenDrawSize();
};
} // namespace lila::scene
