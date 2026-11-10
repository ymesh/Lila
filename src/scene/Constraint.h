#pragma once

#include "core/Export.h"

#include "scene/Node.h"

namespace lila::scene
{
/**
 * parentConstraint, pointConstraint, orientConstraint and aimConstraint. Each is a DG node fed by any number of target
 * worldMatrix connections, the grouped array targets[n].matrix with targets[n].weight (keyable) and targets[n].offset
 * beside each, and drives the constrained node's translate and/or rotate through its constraintTranslate/constraintRotate
 * outputs.
 *
 * What the constraint needs to know about the constrained node (rotate order, jointOrient, pivots, rest values for
 * skipped axes) is copied onto it when it is made: reading those live would make the node depend on what it drives.
 * The constrained node's parent worldMatrix is connected, so constraints work under moving parents.
 */
class LILA_SCENE_API ConstraintNode : public Node
{
public:
    enum class Kind
    {
        Parent,
        Point,
        Orient,
        Aim,
    };

    enum Attr
    {
        kParentMatrix,
        kRotateOrder,
        kJointOrient,
        kRotatePivot,
        kRotatePivotTranslate,
        kRestTranslate,
        kRestRotate,
        kSkipTranslateX,
        kSkipTranslateY,
        kSkipTranslateZ,
        kSkipRotateX,
        kSkipRotateY,
        kSkipRotateZ,
        kOffset,
        kTargetMatrix, // targets[n].matrix
        kTargetWeight, // targets[n].weight
        kTargetOffset, // targets[n].offset: parent constraint, the object relative to the target
        kConstraintTranslate,
        kConstraintRotate,
        kPivotOffsets,
        // aimConstraint only
        kAimVector,
        kUpVector,
        kWorldUpType,
        kWorldUpVector,
        kWorldUpMatrix,
    };

    enum WorldUp : int64_t
    {
        kUpScene,
        kUpObject,
        kUpObjectRotation,
        kUpVectorOnly,
        kUpNone,
    };

    void compute(const EvalContext& ctx) override;

    Kind kind = Kind::Parent;
};
} // namespace lila::scene
