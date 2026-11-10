#pragma once

#include "core/Export.h"

#include "scene/IkSolver.h"
#include "scene/Transform.h"

#include <vector>

namespace lila::scene
{
class Graph;

/**
 * An IK handle: a transform whose world position is the chain's target. It solves the chain from startJoint to the
 * end effector with a registered ik::Solver and drives each solved joint's solvedRotate and ikBlend, so the joints'
 * keyable rotate stays FK and ikBlend crossfades between the two.
 *
 * The chain's rest layout (each joint's FK local matrix, jointOrient, rotateAxis, rotate order) is copied into fixed
 * slots when the handle is made, because reading the joints live would make the handle depend on what it drives. The
 * start joint's parent world matrix is connected, so the chain follows its parent. editSkeleton and
 * `ikHandle -refresh` copy the layout again.
 */
class LILA_SCENE_API IkHandle : public Transform
{
public:
    static constexpr int kMaxJoints = 16;

    enum Attr
    {
        kSolver = Transform::kCount,
        kPoleVector,
        kPoleAngle,
        kIkBlend,
        kJointCount,
        kChainParentMatrix,
        kEndEffector,
        kSoftness,
        kStretch,
        kMaxStretch,
        kStretchFactor,
        kInCurve,
        kRoll,
        kSlots, // kMaxJoints * kSlotSize: rest layout, solvedRotate and the joint's IK limits
        kSlotSize = 13,
        kCount = kSlots + kMaxJoints * kSlotSize,
    };

    static int slotLocal(int i) { return kSlots + i * kSlotSize; }
    static int slotOrient(int i) { return kSlots + i * kSlotSize + 1; }
    static int slotAxis(int i) { return kSlots + i * kSlotSize + 2; }
    static int slotOrder(int i) { return kSlots + i * kSlotSize + 3; }
    static int slotRest(int i) { return kSlots + i * kSlotSize + 4; }
    static int slotSolved(int i) { return kSlots + i * kSlotSize + 5; }
    static int slotLimitType(int i) { return kSlots + i * kSlotSize + 6; }
    static int slotLimitAxis(int i) { return kSlots + i * kSlotSize + 7; }
    static int slotLimitMin(int i) { return kSlots + i * kSlotSize + 8; }
    static int slotLimitMax(int i) { return kSlots + i * kSlotSize + 9; }
    static int slotCone(int i) { return kSlots + i * kSlotSize + 10; }
    static int slotStiffness(int i) { return kSlots + i * kSlotSize + 11; }
    static int slotPreferred(int i) { return kSlots + i * kSlotSize + 12; }

    void compute(const EvalContext& ctx) override;
    /** Slot i's joint limit, as copied from the joint by ik::refresh. */
    ik::JointLimit slotLimit(int i) const;
};

namespace ik
{
/** The chain's joints, start first, read back from the handle's solvedRotate connections plus its end effector. */
LILA_SCENE_API std::vector<NodeId> chainOf(Graph& g, NodeId handle);
/** Every ikHandle whose chain includes joint. */
LILA_SCENE_API std::vector<NodeId> handlesOf(Graph& g, NodeId joint);
/** Copies the chain's current FK layout and IK limits into the handle's slots (undoable). The joints' rotate is the rest pose. */
LILA_SCENE_API void refresh(Graph& g, NodeId handle);
/** The joint's IK limit attributes as a limit, its bone toward its first child joint. */
LILA_SCENE_API JointLimit limitOf(Graph& g, NodeId joint);
} // namespace ik

/** ikHandle type and the built-in solvers (ik2BoneSolver, ikFabrikSolver, ikSplineSolver). Idempotent. */
LILA_SCENE_API void registerIkTypes();
/** ikHandle. Idempotent. */
LILA_SCENE_API void registerIkCommands();
} // namespace lila::scene
