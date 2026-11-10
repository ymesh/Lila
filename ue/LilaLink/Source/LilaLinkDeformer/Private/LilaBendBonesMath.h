#pragma once

#include "CoreMinimal.h"

/**
 * Lila's bend bones curve math (Lila src/deform/BendBones.cpp), the parts Unreal runs on the CPU: the curve through a
 * chain with its rotation-minimising frames and twist, and where a point sits on it. The deformation itself runs in the
 * Deformer Graph kernel (LilaBendBonesDataInterface's HLSL).
 */
namespace LilaBendBones
{
/** A chain's joints in order, component space, and each bone's CVs (the curve passes through them after its joint). */
struct FChain
{
	TArray<FVector> Position;
	TArray<FQuat> Rotation;
	TArray<TArray<FVector>> Cvs;
};

/** The chain as a sampled curve: points, frames (X tangent, Y normal, Z binormal) and arc length per sample. */
struct FCurve
{
	TArray<FVector> Point;
	TArray<FQuat> Frame;
	TArray<double> Length;
	TArray<int32> Knot; // the sample at each joint
};

/** Where a point sits on a chain: its segment, the arc-length fraction along it, its offset in the frame there. */
struct FPlace
{
	int32 Segment = 0;
	double Fraction = 0.0;
	FVector Offset = FVector::ZeroVector;
	double Distance = 0.0;
	/** Arc length along the curve, and the fraction of its segment's strand (at its offset) it covers. */
	double ArcLength = 0.0;
	double Strand = 0.0;
	FQuat Frame = FQuat::Identity;
};

/** The curve through the chain and its bone CVs (centripetal Catmull-Rom, SamplesPerSpan per span), frames carried without twisting, then each joint's twist relative to RestNormals (joint space) ramped between. Empty RestNormals: no twist correction. */
FCurve Build(const FChain& Chain, const TArray<FVector>& RestNormals, int32 SamplesPerSpan);

/** Puts each (bone, position) CV on its bone of Chain, in order of how far along the bone it sits. */
void PlaceCvs(FChain& Chain, const TArray<TPair<int32, FVector>>& Cvs);

/** Each joint's normal at rest, in its own space. */
TArray<FVector> RestNormals(const FChain& Chain, const FCurve& Rest);

/** The nearest place on the curve to P. */
FPlace Locate(const FCurve& Rest, const FVector& P);
}
