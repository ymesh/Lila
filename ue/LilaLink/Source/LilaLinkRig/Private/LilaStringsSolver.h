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

enum class EMode
{
	Forward,
	Backward,
	Ik,
	FullBody,
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
};

FPose StartPose(const FProblem& P);
TArray<FMatrix> Worlds(const FProblem& P, const FPose& S);
bool IsUnder(const FProblem& P, int32 Bone, int32 Ancestor);
TArray<int32> LimbOf(const FProblem& P, int32 Bone, int32 Hips, int32 Stop = INDEX_NONE, int32 Length = 0, const FRules* Rules = nullptr);
FResult SolveRequest(const FProblem& P, const FRequest& R);
} // namespace LilaStrings
