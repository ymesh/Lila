#pragma once

#include "CoreMinimal.h"

/**
 * Lila's full-body IK (src/scene/FullBodyIk.cpp) and Strings solve (src/scene/Strings.cpp), ported so the Strings Rig
 * Units pose a skeleton as Lila does. Matrices compose as Lila's (column vectors): Mul(A, B) is Lila's A * B.
 */
namespace LilaStrings
{
/**
 * How far a bone may turn, as Lila's ik::JointLimit (src/scene/IkSolver.h) carried into Unreal: R is S * Post, and the
 * limit is on S, a hinge about Axis within [Min, Max] or a cone of half-angle Cone about where BoneDir points.
 */
struct FLimit
{
	enum class EKind
	{
		None,
		Hinge,
		Cone,
	};
	EKind Kind = EKind::None;
	FVector Axis = FVector::ZAxisVector;
	double Min = -UE_DOUBLE_PI, Max = UE_DOUBLE_PI;
	FVector BoneDir = FVector::XAxisVector;
	double Cone = UE_DOUBLE_PI;
	FQuat Post = FQuat::Identity;
};

/** R moved to the nearest rotation the limit allows (Lila's ik::clampRotate on S). */
FQuat ClampRotation(const FLimit& Limit, const FQuat& R);

/** One joint of the solve. Bones are ordered parents first; a bone's local is Before * R * After. */
struct FBone
{
	int32 Parent = INDEX_NONE;
	/** World of a root's parent outside the solve. */
	FMatrix ParentWorld = FMatrix::Identity;
	FMatrix Before = FMatrix::Identity;
	FMatrix After = FMatrix::Identity;
	FQuat Rotation = FQuat::Identity;
	/** The R the solve pulls toward. */
	FQuat Reference = FQuat::Identity;
	bool bSolve = true;
	double Stiffness = 0.0;
	double Pull = 1.0;
	/** The solve may also move the bone, by an offset in its parent's space. */
	bool bTranslate = false;
	FVector Offset = FVector::ZeroVector;
	double MaxStretch = 1.0;
	FLimit Limit;
};

struct FEffector
{
	int32 Bone = INDEX_NONE;
	TOptional<FVector> Position;
	TOptional<FQuat> Rotation;
	double Weight = 1.0;
	bool bSoft = false;
};

struct FProblem
{
	TArray<FBone> Bones;
	TArray<FEffector> Effectors;
	double LengthScale = 50.0;
	double Pull = 0.1;
	int32 Iterations = 30;
	int32 FinishIterations = 30;
	double Tolerance = 1e-4;
};

struct FSolution
{
	TArray<FQuat> Rotation;
	TArray<FVector> Offset;
	TArray<double> Stretch;
	double Error = 0.0;
	int32 Iterations = 0;
};

/** Lila's A * B for column-vector matrices. */
inline FMatrix Mul(const FMatrix& A, const FMatrix& B) { return B * A; }

TArray<FMatrix> Forward(const FProblem& P, const TArray<FQuat>& Rotation, const TArray<FVector>& Offset, const TArray<double>& Stretch);
FSolution Solve(const FProblem& P);

/** Lila's strings::Mode: what a drag does to the grabbed bone and what follows it. */
enum class EMode
{
	/** FK, keeping the holds below the bone outside its own limb and the isolated bones' world rotations. */
	Forward,
	Backward,
	Ik,
	FullBody,
	/** The rule web's mode for the bone; without one, moves are IK and turns Forward. */
	Default,
	/** The bone alone: its children keep their world rotation (and on a move their place). */
	Free,
	/** Plain FK: everything below follows, holds and isolation ignored. */
	ForwardAll,
	/** Backward with nothing held: the body swings rigidly about the anchor. */
	BackwardAll,
	/** Both sides give: a turn swings below and above by half each about the bone; a move carries the body half way. */
	Bidirectional,
	/** Bidirectional rigid, nothing held. */
	All,
};

/** A bone that has to end at a world matrix. */
struct FHold
{
	int32 Bone = INDEX_NONE;
	FMatrix World = FMatrix::Identity;
	bool bRotation = true;
};

/** The rule web's limbs by bone index, top to end. */
struct FRules
{
	TArray<TArray<int32>> Chains;
	/** Default mode: the mode each listed bone poses in. */
	TArray<TPair<int32, EMode>> Defaults;
};

struct FRequest
{
	EMode Mode = EMode::Forward;
	int32 Grabbed = INDEX_NONE;
	/** World rotation about the grabbed bone's origin. */
	TOptional<FQuat> Turn;
	/** World offset of the grabbed bone. */
	TOptional<FVector> Move;
	TArray<FHold> Holds;
	int32 Hips = INDEX_NONE;
	int32 ChainLength = 0;
	double MaxStretch = 1.0;
	const FRules* Rules = nullptr;
	/** Bones that keep their world rotation while the drag carries them (Isolate Rotation); the grabbed bone never does. */
	TArray<int32> Isolate;
};

struct FPose
{
	TArray<FQuat> Rotation;
	TArray<FVector> Offset;
	TArray<double> Stretch;
};

struct FResult
{
	FPose Pose;
	double Error = 0.0;
	int32 Anchor = INDEX_NONE;
	/** False when the request can't move the grabbed bone (above the hips in Backward or IK). */
	bool bHandled = true;
	/** The mode the solve ran in: Default's pick, else the request's. */
	EMode Resolved = EMode::Forward;
};

FPose StartPose(const FProblem& P);
TArray<FMatrix> Worlds(const FProblem& P, const FPose& S);
bool IsUnder(const FProblem& P, int32 Bone, int32 Ancestor);
TArray<int32> LimbOf(const FProblem& P, int32 Bone, int32 Hips, int32 Stop = INDEX_NONE, int32 Length = 0, const FRules* Rules = nullptr);
/** The mode Default poses R's grabbed bone in: its rule, else IK for a move of a bone with a limb or the hips, else Forward. */
EMode ResolveDefault(const FProblem& P, const FRequest& R);
FResult SolveRequest(const FProblem& P, const FRequest& R);
/**
 * Brings each hold's bone to its world by bending its limb (LimbOf) from pose S, leaving every other bone as it is (Lila's
 * strings::holdLimbs). Soft gives positions the limbs lean toward without having to meet. Returns the largest hold error left.
 */
/** One joint of a locked IK chain as its handle holds it (Unreal space). */
struct FChainSlot
{
	/** The rest local the handle solves from. */
	FVector Rest = FVector::ZeroVector;
	FQuat RestRotation = FQuat::Identity;
	/** The joint's orient (Lila's frame) and rotate axis (Lila's post): its local rotation is Orient * rotate * Axis. */
	FQuat Orient = FQuat::Identity;
	FQuat Axis = FQuat::Identity;
	/** The limit on rotate, its Post the rotate axis; None when the joint has none. */
	FLimit Limit;
	double Stiffness = 0.0;
	/** The whole local rotation a preferred angle starts the solve from. */
	TOptional<FQuat> Preferred;
};

/** A locked IK chain as Lila's ikHandle solves it, start to end. */
struct FChainIk
{
	enum class ESolver
	{
		TwoBone,
		Fabrik,
		Spline,
	};
	TArray<int32> Bones;
	TArray<FChainSlot> Slots;
	ESolver Solver = ESolver::TwoBone;
	/** Where the bend points, as an offset from the start bone. */
	TOptional<FVector> Pole;
	/** Pole angle and spline roll, radians, already signed for this space. */
	double Twist = 0.0;
	double Roll = 0.0;
	double Softness = 0.0;
	/** Above 1 the chain lengthens, up to this multiple, toward a target out of reach. */
	double MaxStretch = 1.0;
	/** Spline: the curve the chain lies along, and whether the bones scale to span it. */
	TArray<FVector> Curve;
	bool bFitCurve = false;
	/** Lila's world up and side axes in this space: the bend a chain falls back to without a pole or bend. */
	FVector Up = FVector::ZAxisVector;
	FVector Side = FVector::XAxisVector;
	/** How far the end turns to the controller's rotation (Lila's align node weight). */
	double AlignWeight = 0.0;
};

/**
 * Poses the chain's bones as Lila's ikHandle does, from its rest locals toward Target (the end's goal world):
 * the solver, preferred angles, softness, stretch, limits and stiffness, then the end turned to Target by AlignWeight.
 */
void SolveChain(const FProblem& P, FPose& S, const FChainIk& C, const FMatrix& Target);

double HoldLimbs(const FProblem& P, FPose& S, const TArray<FHold>& Holds, int32 Hips, double MaxStretch, const TArray<TPair<int32, FVector>>& Soft = {},
				 const FRules* Rules = nullptr);
} // namespace LilaStrings
