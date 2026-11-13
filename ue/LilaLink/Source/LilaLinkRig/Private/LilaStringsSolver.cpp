#include "LilaStringsSolver.h"

#include "Algo/BinarySearch.h"

#include "ProfilingDebugging/CpuProfilerTrace.h"

#ifndef EIGEN_MPL2_ONLY
#define EIGEN_MPL2_ONLY
#endif
#if defined(_MSC_VER) && USING_CODE_ANALYSIS
#pragma warning(push)
#pragma warning(disable : 6294)
#endif
PRAGMA_DEFAULT_VISIBILITY_START
THIRD_PARTY_INCLUDES_START
#include <Eigen/Dense>
THIRD_PARTY_INCLUDES_END
PRAGMA_DEFAULT_VISIBILITY_END
#if defined(_MSC_VER) && USING_CODE_ANALYSIS
#pragma warning(pop)
#endif

namespace LilaStrings
{
FQuat ClampRotation(const FLimit& L, const FQuat& R)
{
	// The limit is on S = R * Post^-1, the part of the local rotation Lila's rotate becomes.
	FQuat S = R * L.Post.Inverse();
	if (S.W < 0.0)
	{
		S = FQuat(-S.X, -S.Y, -S.Z, -S.W);
	}
	const FVector V(S.X, S.Y, S.Z);
	switch (L.Kind)
	{
	case FLimit::EKind::Hinge:
	{
		const FVector A = L.Axis.SizeSquared() > 1e-24 ? L.Axis.GetUnsafeNormal() : FVector::ZAxisVector;
		const double Angle = 2.0 * FMath::Atan2(FVector::DotProduct(V, A), S.W);
		return FQuat(A, FMath::Clamp(Angle, FMath::Min(L.Min, L.Max), FMath::Max(L.Min, L.Max))) * L.Post;
	}
	case FLimit::EKind::Cone:
	{
		const FVector B = L.BoneDir.SizeSquared() > 1e-24 ? L.BoneDir.GetUnsafeNormal() : FVector::XAxisVector;
		// S = swing * twist, the twist about the bone: only the swing moves where the bone points.
		const double Along = FVector::DotProduct(V, B);
		FQuat Twist = FQuat::Identity;
		if (FMath::Abs(Along) + FMath::Abs(S.W) > 1e-12)
		{
			Twist = FQuat(B.X * Along, B.Y * Along, B.Z * Along, S.W).GetNormalized();
		}
		FQuat Swing = S * Twist.Inverse();
		if (Swing.W < 0.0)
		{
			Swing = FQuat(-Swing.X, -Swing.Y, -Swing.Z, -Swing.W);
		}
		const double Cone = FMath::Max(L.Cone, 0.0);
		const FVector Axis(Swing.X, Swing.Y, Swing.Z);
		if (2.0 * FMath::Acos(FMath::Clamp(Swing.W, -1.0, 1.0)) <= Cone || Axis.SizeSquared() < 1e-24)
		{
			return R;
		}
		return FQuat(Axis.GetUnsafeNormal(), Cone) * Twist * L.Post;
	}
	case FLimit::EKind::None:
		break;
	}
	return R;
}

namespace
{
/** The largest turn a joint takes in one iteration, radians. */
constexpr double MaxStep = 0.5;

FMatrix Translate(const FVector& V) { return FTranslationMatrix(V); }
FMatrix RotationMatrix(const FQuat& Q) { return FQuatRotationMatrix(Q); }

/** The 3x3 part of M (Lila's dmat3(M)). */
FMatrix Linear(const FMatrix& M)
{
	FMatrix Out = M;
	Out.SetOrigin(FVector::ZeroVector);
	return Out;
}

/** M's columns in Lila's terms (its axes' images) made orthonormal, X first. */
FMatrix Orthonormal(const FMatrix& M)
{
	const FVector MX = M.GetScaledAxis(EAxis::X), MY = M.GetScaledAxis(EAxis::Y), MZ = M.GetScaledAxis(EAxis::Z);
	const FVector X = MX.Size() > 1e-12 ? MX.GetUnsafeNormal() : FVector(1, 0, 0);
	FVector Y = MY - FVector::DotProduct(MY, X) * X;
	Y = Y.Size() > 1e-12 ? Y.GetUnsafeNormal() : FVector(0, 1, 0);
	FVector Z = FVector::CrossProduct(X, Y);
	if (FVector::DotProduct(Z, MZ) < 0.0)
	{
		Z = -Z;
	}
	return FMatrix(X, Y, Z, FVector::ZeroVector);
}

FQuat QuatOf(const FMatrix& M) { return FQuat(Orthonormal(M)).GetNormalized(); }

double Resistance(const double Stiffness)
{
	const double S = FMath::Clamp(Stiffness, 0.0, 0.99);
	return 1.0 + S / (1.0 - S);
}

/** Axis times angle, the short way round. */
FVector RotationVector(FQuat Q)
{
	if (Q.W < 0.0)
	{
		Q = FQuat(-Q.X, -Q.Y, -Q.Z, -Q.W);
	}
	const FVector V(Q.X, Q.Y, Q.Z);
	const double S = V.Size();
	if (S < 1e-12)
	{
		return V * 2.0;
	}
	return V * (2.0 * FMath::Atan2(S, Q.W) / S);
}

FQuat FromRotationVector(const FVector& V)
{
	const double Angle = V.Size();
	return Angle < 1e-12 ? FQuat::Identity : FQuat(V / Angle, Angle);
}

FVector SegmentOf(const FBone& B) { return B.Before.GetOrigin(); }

struct FFrames
{
	TArray<FMatrix> World;
	TArray<FVector> Pivot;
	TArray<FMatrix> Frame;
	TArray<FMatrix> Parent;
};

FFrames FramesOf(const FProblem& P, const TArray<FQuat>& Rotation, const TArray<FVector>& Offset, const TArray<double>& Stretch)
{
	const int32 N = P.Bones.Num();
	FFrames F;
	F.World.SetNum(N);
	F.Pivot.SetNum(N);
	F.Frame.SetNum(N);
	F.Parent.SetNum(N);
	for (int32 I = 0; I < N; ++I)
	{
		const FBone& B = P.Bones[I];
		const FMatrix& Parent = B.Parent < 0 ? B.ParentWorld : F.World[B.Parent];
		const FVector Lengthen = I < Stretch.Num() ? (Stretch[I] - 1.0) * SegmentOf(B) : FVector::ZeroVector;
		const FMatrix Base = Mul(Mul(Parent, Translate(Offset[I] + Lengthen)), B.Before);
		F.World[I] = Mul(Mul(Base, RotationMatrix(Rotation[I])), B.After);
		F.Pivot[I] = Base.GetOrigin();
		F.Frame[I] = Orthonormal(Base);
		F.Parent[I] = Linear(Parent);
	}
	return F;
}

/** How much of the skeleton each bone carries, as a multiple of the average, plus a floor. */
TArray<double> CarriedWeights(const FProblem& P)
{
	const int32 N = P.Bones.Num();
	TArray<double> Carried;
	Carried.Init(0.0, N);
	for (int32 I = N - 1; I >= 0; --I)
	{
		if (const int32 Parent = P.Bones[I].Parent; Parent >= 0)
		{
			Carried[Parent] += Carried[I] + SegmentOf(P.Bones[I]).Size();
		}
	}
	double Sum = 0.0;
	for (double& C : Carried)
	{
		Sum += C += 0.25 * P.LengthScale;
	}
	const double Mean = N ? Sum / double(N) : 1.0;
	for (double& C : Carried)
	{
		C /= Mean;
	}
	return Carried;
}

/** Writes the math matrix [V]x (V x W for any W) at (Row, Col), scaled. */
void PutCross(Eigen::MatrixXd& M, const Eigen::Index Row, const Eigen::Index Col, const FVector& V, const double Scale)
{
	M(Row + 0, Col + 0) = 0.0;
	M(Row + 0, Col + 1) = -V.Z * Scale;
	M(Row + 0, Col + 2) = V.Y * Scale;
	M(Row + 1, Col + 0) = V.Z * Scale;
	M(Row + 1, Col + 1) = 0.0;
	M(Row + 1, Col + 2) = -V.X * Scale;
	M(Row + 2, Col + 0) = -V.Y * Scale;
	M(Row + 2, Col + 1) = V.X * Scale;
	M(Row + 2, Col + 2) = 0.0;
}

void PutDiagonal(Eigen::MatrixXd& M, const Eigen::Index Row, const Eigen::Index Col, const double D)
{
	for (int32 R = 0; R < 3; ++R)
	{
		for (int32 C = 0; C < 3; ++C)
		{
			M(Row + R, Col + C) = R == C ? D : 0.0;
		}
	}
}

FVector InverseLinear(const FMatrix& M, const FVector& V) { return Linear(M).Inverse().TransformVector(V); }

FSolution SolveFrom(const FProblem& P, FSolution Out, const bool bStretching)
{
	TRACE_CPUPROFILER_EVENT_SCOPE(LilaStrings::SolveFrom);

	const int32 N = P.Bones.Num();
	TArray<Eigen::Index> RotCol, MoveCol, StretchCol;
	RotCol.Init(-1, N);
	MoveCol.Init(-1, N);
	StretchCol.Init(-1, N);
	Eigen::Index Cols = 0;
	for (int32 I = 0; I < N; ++I)
	{
		if (P.Bones[I].bSolve)
		{
			RotCol[I] = Cols;
			Cols += 3;
		}
		if (P.Bones[I].bTranslate)
		{
			MoveCol[I] = Cols;
			Cols += 3;
		}
		if (bStretching && P.Bones[I].MaxStretch > 1.0 && SegmentOf(P.Bones[I]).Size() > 1e-9)
		{
			StretchCol[I] = Cols;
			Cols += 1;
		}
	}
	TArray<TArray<int32>> Chains;
	Eigen::Index Rows = 0;
	for (const FEffector& E : P.Effectors)
	{
		TArray<int32>& Chain = Chains.AddDefaulted_GetRef();
		for (int32 B = E.Bone; B >= 0; B = P.Bones[B].Parent)
		{
			Chain.Add(B);
		}
		Rows += (E.Position ? 3 : 0) + (E.Rotation ? 3 : 0);
	}

	auto PinError = [&](const FFrames& F) {
		double Worst = 0.0;
		for (const FEffector& E : P.Effectors)
		{
			if (E.Position && !E.bSoft)
			{
				Worst = FMath::Max(Worst, (*E.Position - F.World[E.Bone].GetOrigin()).Size());
			}
		}
		return Worst;
	};

	FFrames F = FramesOf(P, Out.Rotation, Out.Offset, Out.Stretch);
	Out.Error = PinError(F);
	if (Cols == 0 || Rows == 0)
	{
		return Out;
	}

	const double L = FMath::Max(P.LengthScale, 1e-6);
	const TArray<double> Carried = CarriedWeights(P);
	auto PullOf = [&](int32 I, bool bPulling) {
		const FBone& B = P.Bones[I];
		return bPulling ? P.Pull * B.Pull * Resistance(B.Stiffness) * Carried[I] : 0.0;
	};
	auto TurnAway = [&](const FFrames& Fr, const TArray<FQuat>& Rot, int32 I) -> FVector {
		return Fr.Frame[I].TransformVector(RotationVector(Rot[I] * P.Bones[I].Reference.Inverse()));
	};
	auto MoveAway = [&](const FFrames& Fr, const TArray<FVector>& Off, int32 I) -> FVector { return Fr.Parent[I].TransformVector(Off[I] - P.Bones[I].Offset) / L; };
	auto StretchUnit = [&](int32 I) { return SegmentOf(P.Bones[I]).Size() / L; };
	auto Cost = [&](const FFrames& Fr, const TArray<FQuat>& Rot, const TArray<FVector>& Off, const TArray<double>& Str, bool bPulling) {
		double C = 0.0;
		for (const FEffector& E : P.Effectors)
		{
			if (E.bSoft && !bPulling)
			{
				continue;
			}
			const FMatrix& W = Fr.World[E.Bone];
			if (E.Position)
			{
				C += ((*E.Position - W.GetOrigin()) * (E.Weight / L)).SizeSquared();
			}
			if (E.Rotation)
			{
				C += (RotationVector(*E.Rotation * QuatOf(W).Inverse()) * E.Weight).SizeSquared();
			}
		}
		if (bPulling)
		{
			for (int32 I = 0; I < N; ++I)
			{
				const double Pull = PullOf(I, true);
				if (RotCol[I] >= 0)
				{
					C += Pull * Pull * TurnAway(Fr, Rot, I).SizeSquared();
				}
				if (MoveCol[I] >= 0)
				{
					C += Pull * Pull * MoveAway(Fr, Off, I).SizeSquared();
				}
				if (StretchCol[I] >= 0)
				{
					C += Pull * Pull * FMath::Square((Str[I] - 1.0) * StretchUnit(I));
				}
			}
		}
		return C;
	};

	Eigen::MatrixXd J(Rows, Cols), H(Cols, Cols);
	Eigen::VectorXd R(Rows), G(Cols);
	TArray<FQuat> TrialRot;
	TrialRot.SetNum(N);
	TArray<FVector> TrialOff;
	TrialOff.SetNum(N);
	TArray<double> TrialStr;
	TrialStr.Init(1.0, N);
	double Mu = 1e-3;
	bool bPulling = P.Iterations > 0;
	double Current = Cost(F, Out.Rotation, Out.Offset, Out.Stretch, bPulling);
	const int32 Total = FMath::Max(P.Iterations, 0) + FMath::Max(P.FinishIterations, 0);
	for (int32 It = 0; It < Total; ++It)
	{
		if (bPulling && It >= P.Iterations)
		{
			bPulling = false;
			Mu = 1e-3;
			Current = Cost(F, Out.Rotation, Out.Offset, Out.Stretch, false);
		}
		if (!bPulling && Out.Error <= P.Tolerance)
		{
			break;
		}

		J.setZero();
		R.setZero();
		Eigen::Index Row = 0;
		for (int32 K = 0; K < P.Effectors.Num(); ++K)
		{
			const FEffector& E = P.Effectors[K];
			if (E.bSoft && !bPulling)
			{
				Row += (E.Position ? 3 : 0) + (E.Rotation ? 3 : 0);
				continue;
			}
			const FMatrix& W = F.World[E.Bone];
			if (E.Position)
			{
				const FVector At = W.GetOrigin();
				const FVector Err = (*E.Position - At) * (E.Weight / L);
				R(Row) = Err.X;
				R(Row + 1) = Err.Y;
				R(Row + 2) = Err.Z;
				for (const int32 B : Chains[K])
				{
					if (RotCol[B] >= 0)
					{
						// Turning about W moves At by W x (At - pivot): the column block is -[At - pivot]x.
						PutCross(J, Row, RotCol[B], At - F.Pivot[B], -(E.Weight / L));
					}
					if (MoveCol[B] >= 0)
					{
						PutDiagonal(J, Row, MoveCol[B], E.Weight);
					}
					if (StretchCol[B] >= 0)
					{
						const FVector D = F.Parent[B].TransformVector(SegmentOf(P.Bones[B])) * (E.Weight / L);
						J(Row, StretchCol[B]) = D.X;
						J(Row + 1, StretchCol[B]) = D.Y;
						J(Row + 2, StretchCol[B]) = D.Z;
					}
				}
				Row += 3;
			}
			if (E.Rotation)
			{
				const FVector Err = RotationVector(*E.Rotation * QuatOf(W).Inverse()) * E.Weight;
				R(Row) = Err.X;
				R(Row + 1) = Err.Y;
				R(Row + 2) = Err.Z;
				for (const int32 B : Chains[K])
				{
					if (RotCol[B] >= 0)
					{
						PutDiagonal(J, Row, RotCol[B], E.Weight);
					}
				}
				Row += 3;
			}
		}

		H.noalias() = J.transpose() * J;
		G.noalias() = J.transpose() * R;
		for (int32 I = 0; I < N; ++I)
		{
			const double Pull = PullOf(I, bPulling);
			const double Damp = Mu * Resistance(P.Bones[I].Stiffness) * Carried[I];
			const TPair<Eigen::Index, FVector> Blocks[2] = {
				{RotCol[I], RotCol[I] >= 0 ? TurnAway(F, Out.Rotation, I) : FVector::ZeroVector},
				{MoveCol[I], MoveCol[I] >= 0 ? MoveAway(F, Out.Offset, I) : FVector::ZeroVector},
			};
			for (const TPair<Eigen::Index, FVector>& Block : Blocks)
			{
				if (const Eigen::Index C = Block.Key; C >= 0)
				{
					for (int32 A = 0; A < 3; ++A)
					{
						H(C + A, C + A) += Pull * Pull + Damp;
						G(C + A) -= Pull * Pull * Block.Value[A];
					}
				}
			}
			if (const Eigen::Index C = StretchCol[I]; C >= 0)
			{
				const double K2 = FMath::Square(StretchUnit(I));
				H(C, C) += (Pull * Pull + Damp) * K2;
				G(C) -= Pull * Pull * K2 * (Out.Stretch[I] - 1.0);
				// A stretch held at a bound it is pushed against sits this step out.
				if ((Out.Stretch[I] >= P.Bones[I].MaxStretch - 1e-12 && G(C) > 0.0) || (Out.Stretch[I] <= 1.0 + 1e-12 && G(C) < 0.0))
				{
					H.row(C).setZero();
					H.col(C).setZero();
					H(C, C) = 1.0;
					G(C) = 0.0;
				}
			}
		}

		const Eigen::VectorXd X = H.ldlt().solve(G);
		if (!X.allFinite())
		{
			break;
		}
		for (int32 I = 0; I < N; ++I)
		{
			TrialRot[I] = Out.Rotation[I];
			TrialOff[I] = Out.Offset[I];
			if (RotCol[I] >= 0)
			{
				FVector W(X(RotCol[I]), X(RotCol[I] + 1), X(RotCol[I] + 2));
				if (const double A = W.Size(); A > MaxStep)
				{
					W *= MaxStep / A;
				}
				TrialRot[I] = (FromRotationVector(F.Frame[I].GetTransposed().TransformVector(W)) * Out.Rotation[I]).GetNormalized();
				if (P.Bones[I].Limit.Kind != FLimit::EKind::None)
				{
					TrialRot[I] = ClampRotation(P.Bones[I].Limit, TrialRot[I]).GetNormalized();
				}
			}
			if (MoveCol[I] >= 0)
			{
				TrialOff[I] += InverseLinear(F.Parent[I], FVector(X(MoveCol[I]), X(MoveCol[I] + 1), X(MoveCol[I] + 2)) * L);
			}
			TrialStr[I] = Out.Stretch[I];
			if (StretchCol[I] >= 0)
			{
				TrialStr[I] = FMath::Clamp(Out.Stretch[I] + FMath::Clamp(double(X(StretchCol[I])), -MaxStep, MaxStep), 1.0, P.Bones[I].MaxStretch);
			}
		}
		FFrames Trial = FramesOf(P, TrialRot, TrialOff, TrialStr);
		const double Next = Cost(Trial, TrialRot, TrialOff, TrialStr, bPulling);
		Out.Iterations = It + 1;
		if (Next < Current)
		{
			const bool bSettled = Current - Next < 1e-12 * FMath::Max(Current, 1e-30);
			Swap(Out.Rotation, TrialRot);
			Swap(Out.Offset, TrialOff);
			Swap(Out.Stretch, TrialStr);
			F = MoveTemp(Trial);
			Out.Error = PinError(F);
			Current = Next;
			Mu = FMath::Max(Mu / 3.0, 1e-9);
			if (bSettled && bPulling)
			{
				It = FMath::Max(It, P.Iterations - 1);
			}
		}
		else if ((Mu *= 4.0) > 1e8)
		{
			if (!bPulling)
			{
				break;
			}
			It = FMath::Max(It, P.Iterations - 1);
		}
	}
	return Out;
}

// Strings -----------------------------------------------------------------------------------------------------------

const FMatrix& ParentWorldOf(const FProblem& P, const TArray<FMatrix>& W, const int32 I)
{
	const FBone& B = P.Bones[I];
	return B.Parent < 0 ? B.ParentWorld : W[B.Parent];
}

FVector Lengthening(const FProblem& P, const FPose& S, const int32 I) { return (S.Stretch[I] - 1.0) * SegmentOf(P.Bones[I]); }

/** Sets bone I's rotation (and with bTranslate its offset) so its world becomes Target, its parent where W has it. */
void Place(const FProblem& P, FPose& S, const TArray<FMatrix>& W, const int32 I, const FMatrix& Target, const bool bTranslate)
{
	const FBone& B = P.Bones[I];
	const FMatrix M = Mul(Mul(ParentWorldOf(P, W, I).Inverse(), Target), B.After.Inverse());
	const FMatrix Rot = Mul(Linear(B.Before).Inverse(), Linear(M));
	S.Rotation[I] = QuatOf(Rot);
	if (bTranslate)
	{
		S.Offset[I] = M.GetOrigin() - B.Before.GetOrigin() - Lengthening(P, S, I);
	}
}

FMatrix TurnedAbout(const FMatrix& World, const FQuat& Q)
{
	const FVector C = World.GetOrigin();
	return Mul(Mul(Mul(Translate(C), RotationMatrix(Q)), Translate(-C)), World);
}

FMatrix Shifted(const FMatrix& World, const FVector& By) { return Mul(Translate(By), World); }

double HoldError(const TArray<FMatrix>& W, const TArray<FHold>& Holds)
{
	double E = 0.0;
	for (const FHold& H : Holds)
	{
		E = FMath::Max(E, (W[H.Bone].GetOrigin() - H.World.GetOrigin()).Size());
	}
	return E;
}

TArray<FHold> HoldsUnder(const FProblem& P, const TArray<FHold>& Holds, const int32 Bone)
{
	TArray<FHold> Out;
	for (const FHold& H : Holds)
	{
		if (IsUnder(P, H.Bone, Bone))
		{
			Out.Add(H);
		}
	}
	return Out;
}

void ForwardTurn(const FProblem& P, FPose& S, const int32 G, const FQuat& Q)
{
	const TArray<FMatrix> W = Worlds(P, S);
	Place(P, S, W, G, TurnedAbout(W[G], Q), false);
}

/** V at unit length, as glm::normalize: GetSafeNormal returns zero below a squared length of 1e-8, which loses the tiny
 * turns a nearly straight limb asks for. Zero stays zero. */
FVector Unit(const FVector& V)
{
	const double L = V.Size();
	return L > 0.0 ? V / L : FVector::ZeroVector;
}

FQuat TurnBetween(const FVector& From, const FVector& To)
{
	const FVector A = Unit(From), B = Unit(To);
	const double C = FVector::DotProduct(A, B);
	if (C > 1.0 - 1e-12)
	{
		return FQuat::Identity;
	}
	FVector Axis = FVector::CrossProduct(A, B);
	if (Axis.Size() < 1e-9)
	{
		Axis = FMath::Abs(A.X) < 0.9 ? FVector::CrossProduct(A, FVector(1, 0, 0)) : FVector::CrossProduct(A, FVector(0, 1, 0));
	}
	return FQuat(Unit(Axis), FMath::Acos(FMath::Clamp(C, -1.0, 1.0)));
}

/** Two-bone start for a limb: the elbow placed by the law of cosines in the limb's bend plane. */
void TwoBoneStart(const FProblem& P, FPose& S, const int32 Upper, const int32 Lower, const int32 End, const FVector& Target)
{
	TArray<FMatrix> W = Worlds(P, S);
	const FVector A = W[Upper].GetOrigin(), B = W[Lower].GetOrigin(), C = W[End].GetOrigin();
	const double L1 = (B - A).Size(), L2 = (C - B).Size();
	if (L1 < 1e-9 || L2 < 1e-9 || (Target - A).Size() < 1e-9)
	{
		return;
	}
	const FVector Dir = Unit(Target - A);
	const double D = FMath::Clamp((Target - A).Size(), FMath::Abs(L1 - L2) + 1e-6, L1 + L2 - 1e-6);
	FVector Bend = (B - A) - FVector::DotProduct(B - A, Dir) * Dir;
	if (Bend.Size() < 1e-6 * L1)
	{
		const FVector Reach = Unit(C - A);
		Bend = (B - A) - FVector::DotProduct(B - A, Reach) * Reach;
		Bend -= FVector::DotProduct(Bend, Dir) * Dir;
		if (Bend.Size() < 1e-9)
		{
			return;
		}
	}
	Bend = Unit(Bend);
	const double Angle = FMath::Acos(FMath::Clamp((L1 * L1 + D * D - L2 * L2) / (2.0 * L1 * D), -1.0, 1.0));
	const FVector Elbow = A + L1 * (FMath::Cos(Angle) * Dir + FMath::Sin(Angle) * Bend);
	Place(P, S, W, Upper, TurnedAbout(W[Upper], TurnBetween(B - A, Elbow - A)), false);
	W = Worlds(P, S);
	const FVector B2 = W[Lower].GetOrigin(), C2 = W[End].GetOrigin();
	Place(P, S, W, Lower, TurnedAbout(W[Lower], TurnBetween(C2 - B2, A + D * Dir - B2)), false);
}

FResult FullBody(const FProblem& P, const FRequest& R)
{
	FProblem Q = P;
	const TArray<FMatrix> W = Worlds(P, StartPose(P));
	const FMatrix& G = W[R.Grabbed];
	FEffector E;
	E.Bone = R.Grabbed;
	E.Position = G.GetOrigin() + (R.Move ? *R.Move : FVector::ZeroVector);
	if (R.Turn)
	{
		E.Rotation = QuatOf(TurnedAbout(G, *R.Turn));
	}
	Q.Effectors = {E};
	for (const FHold& H : R.Holds)
	{
		if (H.Bone != R.Grabbed)
		{
			FEffector& Pin = Q.Effectors.AddDefaulted_GetRef();
			Pin.Bone = H.Bone;
			Pin.Position = H.World.GetOrigin();
			if (H.bRotation)
			{
				Pin.Rotation = QuatOf(H.World);
			}
		}
	}
	if (R.Hips >= 0)
	{
		Q.Bones[R.Hips].bTranslate = true;
	}
	if (R.MaxStretch > 1.0)
	{
		for (FBone& B : Q.Bones)
		{
			B.MaxStretch = R.MaxStretch;
		}
	}
	const FSolution Sol = Solve(Q);
	FResult Out;
	Out.Pose.Rotation = Sol.Rotation;
	Out.Pose.Offset = Sol.Offset;
	if (Sol.Stretch.IsEmpty())
	{
		Out.Pose.Stretch.Init(1.0, P.Bones.Num());
	}
	else
	{
		Out.Pose.Stretch = Sol.Stretch;
	}
	Out.Error = Sol.Error;
	return Out;
}

} // namespace

double HoldLimbs(const FProblem& P, FPose& S, const TArray<FHold>& Holds, const int32 Hips, const double MaxStretch,
				 const TArray<TPair<int32, FVector>>& Soft, const FRules* Rules)
{
	TRACE_CPUPROFILER_EVENT_SCOPE(LilaStrings::HoldLimbs);

	if (Holds.IsEmpty())
	{
		return 0.0;
	}
	TArray<bool> Bend;
	Bend.Init(false, P.Bones.Num());
	for (const FHold& H : Holds)
	{
		for (const int32 B : LimbOf(P, H.Bone, Hips, INDEX_NONE, 0, Rules))
		{
			Bend[B] = true;
		}
	}
	if (!Bend.Contains(true))
	{
		return HoldError(Worlds(P, S), Holds);
	}
	for (const FHold& H : Holds)
	{
		if (const TArray<int32> Limb = LimbOf(P, H.Bone, Hips, INDEX_NONE, 0, Rules); Limb.Num() >= 3 && Limb[0] == H.Bone)
		{
			TwoBoneStart(P, S, Limb[2], Limb[1], Limb[0], H.World.GetOrigin());
		}
	}

	// The pose so far becomes the problem's: offsets and stretches folded into each bone, its rotation the reference.
	FProblem Q = P;
	Q.Effectors.Reset();
	for (int32 I = 0; I < Q.Bones.Num(); ++I)
	{
		FBone& B = Q.Bones[I];
		B.Before = Mul(Translate(S.Offset[I] + Lengthening(P, S, I)), B.Before);
		B.Rotation = B.Reference = S.Rotation[I];
		B.bTranslate = false;
		B.Offset = FVector::ZeroVector;
		B.bSolve = Bend[I];
		const bool bLimbTop = B.Parent < 0 || !Bend[B.Parent];
		B.MaxStretch = Bend[I] && !bLimbTop ? FMath::Max(1.0, MaxStretch) : 1.0;
	}
	for (const FHold& H : Holds)
	{
		FEffector& E = Q.Effectors.AddDefaulted_GetRef();
		E.Bone = H.Bone;
		E.Position = H.World.GetOrigin();
		if (H.bRotation)
		{
			E.Rotation = QuatOf(H.World);
		}
	}
	for (const TPair<int32, FVector>& Pt : Soft)
	{
		if (Bend[Pt.Key])
		{
			FEffector& E = Q.Effectors.AddDefaulted_GetRef();
			E.Bone = Pt.Key;
			E.Position = Pt.Value;
			E.bSoft = true;
		}
	}
	const FSolution Sol = Solve(Q);
	for (int32 I = 0; I < Q.Bones.Num(); ++I)
	{
		if (Bend[I])
		{
			S.Rotation[I] = Sol.Rotation[I];
			if (Sol.Stretch.IsValidIndex(I))
			{
				S.Stretch[I] *= Sol.Stretch[I];
			}
		}
	}
	return HoldError(Worlds(P, S), Holds);
}

namespace
{
FMatrix TurnedAround(const FMatrix& World, const FVector& C, const FQuat& Q) { return Mul(Mul(Mul(Translate(C), RotationMatrix(Q)), Translate(-C)), World); }

/** glm::slerp from identity to Q by A, Q already on the short side, so half turns match Lila's to the bit. */
FQuat PartOf(const FQuat& Q, const double A)
{
	const double CosTheta = Q.W;
	if (CosTheta > 1.0 - DBL_EPSILON)
	{
		return FQuat(A * Q.X, A * Q.Y, A * Q.Z, (1.0 - A) + A * Q.W);
	}
	const double Theta = FMath::Acos(CosTheta);
	const double S0 = FMath::Sin((1.0 - A) * Theta), S1 = FMath::Sin(A * Theta), Sin = FMath::Sin(Theta);
	return FQuat(S1 * Q.X / Sin, S1 * Q.Y / Sin, S1 * Q.Z / Sin, (S0 + S1 * Q.W) / Sin);
}

/** Puts each isolated bone (parents first) back to its world rotation from Start, where the pose has carried it now. */
void IsolateRotations(const FProblem& P, FPose& S, const TArray<FMatrix>& Start, const TArray<int32>& Isolate, const int32 Grabbed)
{
	TRACE_CPUPROFILER_EVENT_SCOPE(LilaStrings::IsolateRotations);

	TArray<int32> Bones;
	for (const int32 B : Isolate)
	{
		if (P.Bones.IsValidIndex(B) && B != Grabbed && P.Bones[B].bSolve)
		{
			Bones.Add(B);
		}
	}
	Bones.Sort();
	for (const int32 B : Bones)
	{
		const TArray<FMatrix> W = Worlds(P, S);
		FMatrix Target = Start[B];
		Target.SetOrigin(W[B].GetOrigin());
		Place(P, S, W, B, Target, false);
	}
}

/** Holds below the grabbed bone outside its own limb: what Forward keeps while the bone above them turns. */
TArray<FHold> HeldBelow(const FProblem& P, const FRequest& R)
{
	TArray<FHold> Out;
	for (const FHold& H : R.Holds)
	{
		if (H.Bone == R.Grabbed || !IsUnder(P, H.Bone, R.Grabbed))
		{
			continue;
		}
		if (!LimbOf(P, H.Bone, R.Hips, INDEX_NONE, 0, R.Rules).Contains(R.Grabbed))
		{
			Out.Add(H);
		}
	}
	return Out;
}

/** Free: the bone turns or moves alone, its children keeping their world rotation (and, on a move, their place). */
void FreePose(const FProblem& P, FPose& S, const FRequest& R, const TArray<FMatrix>& Start)
{
	TRACE_CPUPROFILER_EVENT_SCOPE(LilaStrings::FreePose);

	const int32 G = R.Grabbed;
	if (R.Turn)
	{
		ForwardTurn(P, S, G, *R.Turn);
	}
	if (R.Move)
	{
		const TArray<FMatrix> W = Worlds(P, S);
		Place(P, S, W, G, Shifted(W[G], *R.Move), true);
	}
	for (int32 I = 0; I < P.Bones.Num(); ++I)
	{
		if (P.Bones[I].Parent == G && P.Bones[I].bSolve)
		{
			const TArray<FMatrix> W = Worlds(P, S);
			FMatrix Target = Start[I];
			if (!R.Move)
			{
				Target.SetOrigin(W[I].GetOrigin());
			}
			Place(P, S, W, I, Target, R.Move.IsSet());
		}
	}
}

/**
 * Bidirectional and All: a turn takes the parts below the bone half way and the body above half the other way, both
 * about the bone; a move carries the body half way and bends the limb for the rest (All: the whole body all the way).
 */
double BothWays(const FProblem& P, FPose& S, const FRequest& R, const TArray<FMatrix>& Start, const bool bRigid)
{
	TRACE_CPUPROFILER_EVENT_SCOPE(LilaStrings::BothWays);

	const int32 G = R.Grabbed;
	const FVector C = Start[G].GetOrigin();
	TArray<FHold> Outside;
	if (!bRigid)
	{
		for (const FHold& H : R.Holds)
		{
			if (!IsUnder(P, H.Bone, G))
			{
				Outside.Add(H);
			}
		}
	}
	if (R.Turn)
	{
		const FQuat T = *R.Turn;
		const FQuat Half = PartOf(T.W < 0.0 ? FQuat(-T.X, -T.Y, -T.Z, -T.W) : T, 0.5);
		const TArray<FMatrix> W = Worlds(P, S);
		Place(P, S, W, R.Hips, TurnedAround(W[R.Hips], C, Half.Inverse()), true);
		ForwardTurn(P, S, G, T);
	}
	if (R.Move)
	{
		const TArray<FMatrix> W = Worlds(P, S);
		if (bRigid)
		{
			Place(P, S, W, R.Hips, Shifted(W[R.Hips], *R.Move), true);
			return 0.0;
		}
		Place(P, S, W, R.Hips, Shifted(W[R.Hips], *R.Move * 0.5), true);
		TArray<FHold> Holds;
		FHold& Target = Holds.AddDefaulted_GetRef();
		Target.Bone = G;
		Target.World = Shifted(Start[G], *R.Move);
		Holds.Append(Outside);
		return HoldLimbs(P, S, Holds, R.Hips, R.MaxStretch, {}, R.Rules);
	}
	return Outside.IsEmpty() ? 0.0 : HoldLimbs(P, S, Outside, R.Hips, R.MaxStretch, {}, R.Rules);
}
} // namespace

TArray<FMatrix> Forward(const FProblem& P, const TArray<FQuat>& Rotation, const TArray<FVector>& Offset, const TArray<double>& Stretch)
{
	return FramesOf(P, Rotation, Offset, Stretch).World;
}

FSolution Solve(const FProblem& P)
{
	TRACE_CPUPROFILER_EVENT_SCOPE(LilaStrings::Solve);

	const int32 N = P.Bones.Num();
	FSolution Start;
	Start.Rotation.SetNum(N);
	Start.Offset.SetNum(N);
	Start.Stretch.Init(1.0, N);
	for (int32 I = 0; I < N; ++I)
	{
		Start.Rotation[I] = P.Bones[I].Rotation;
		Start.Offset[I] = P.Bones[I].Offset;
	}
	FSolution Out = SolveFrom(P, Start, false);
	// Bones lengthen only for the reach turning could not give.
	const bool bStretchy = P.Bones.ContainsByPredicate([](const FBone& B) { return B.MaxStretch > 1.0; });
	if (bStretchy && Out.Error > P.Tolerance)
	{
		FSolution Longer = SolveFrom(P, Out, true);
		Longer.Iterations += Out.Iterations;
		if (Longer.Error < Out.Error)
		{
			return Longer;
		}
	}
	return Out;
}

FPose StartPose(const FProblem& P)
{
	FPose S;
	for (const FBone& B : P.Bones)
	{
		S.Rotation.Add(B.Rotation);
	}
	S.Offset.Init(FVector::ZeroVector, P.Bones.Num());
	S.Stretch.Init(1.0, P.Bones.Num());
	return S;
}

TArray<FMatrix> Worlds(const FProblem& P, const FPose& S) { return Forward(P, S.Rotation, S.Offset, S.Stretch); }

bool IsUnder(const FProblem& P, const int32 Bone, const int32 Ancestor)
{
	for (int32 B = Bone; B >= 0; B = P.Bones[B].Parent)
	{
		if (B == Ancestor)
		{
			return true;
		}
	}
	return false;
}

TArray<int32> LimbOf(const FProblem& P, const int32 Bone, const int32 Hips, const int32 Stop, const int32 Length, const FRules* Rules)
{
	TArray<int32> Out;
	if (Length > 0)
	{
		for (int32 B = Bone; B >= 0 && B != Hips && B != Stop && Out.Num() < Length; B = P.Bones[B].Parent)
		{
			if (P.Bones[B].bSolve)
			{
				Out.Add(B);
			}
		}
		return Out;
	}
	if (Rules)
	{
		for (const TArray<int32>& Chain : Rules->Chains)
		{
			if (const int32 At = Chain.IndexOfByKey(Bone); At != INDEX_NONE)
			{
				for (int32 K = At; K >= 0 && Chain[K] != Hips && Chain[K] != Stop; --K)
				{
					if (P.Bones[Chain[K]].bSolve)
					{
						Out.Add(Chain[K]);
					}
				}
				return Out;
			}
		}
	}
	const int32 N = P.Bones.Num();
	// A limb continues through a joint with one child carrying two or more joints below it.
	TArray<int32> Height;
	Height.Init(0, N);
	for (int32 I = N - 1; I >= 0; --I)
	{
		if (const int32 Parent = P.Bones[I].Parent; Parent >= 0)
		{
			Height[Parent] = FMath::Max(Height[Parent], Height[I] + 1);
		}
	}
	TArray<int32> Limbs;
	Limbs.Init(0, N);
	for (int32 I = 0; I < N; ++I)
	{
		if (const int32 Parent = P.Bones[I].Parent; Parent >= 0 && Height[I] >= 2)
		{
			++Limbs[Parent];
		}
	}
	for (int32 B = Bone; B >= 0 && B != Hips && B != Stop; B = P.Bones[B].Parent)
	{
		if (B != Bone && Limbs[B] >= 2)
		{
			break;
		}
		if (P.Bones[B].bSolve)
		{
			Out.Add(B);
		}
	}
	return Out;
}

EMode ResolveDefault(const FProblem& P, const FRequest& R)
{
	if (R.Rules)
	{
		for (const TPair<int32, EMode>& D : R.Rules->Defaults)
		{
			if (D.Key == R.Grabbed && D.Value != EMode::Default)
			{
				return D.Value;
			}
		}
	}
	if (R.Move && (R.Grabbed == R.Hips || LimbOf(P, R.Grabbed, R.Hips, INDEX_NONE, 0, R.Rules).Num() >= 2))
	{
		return EMode::Ik;
	}
	return EMode::Forward;
}

FResult SolveRequest(const FProblem& P, const FRequest& R)
{
	TRACE_CPUPROFILER_EVENT_SCOPE(LilaStrings::SolveRequest);

	check(P.Bones.IsValidIndex(R.Grabbed));
	if (R.Mode == EMode::Default)
	{
		FRequest Picked = R;
		Picked.Mode = ResolveDefault(P, R);
		return SolveRequest(P, Picked);
	}
	if (R.Mode == EMode::FullBody)
	{
		FResult Out = FullBody(P, R);
		Out.Resolved = EMode::FullBody;
		return Out;
	}
	FResult Out;
	Out.Resolved = R.Mode;
	Out.Pose = StartPose(P);
	FPose& S = Out.Pose;
	const int32 G = R.Grabbed;
	const TArray<FMatrix> Start = Worlds(P, S);
	const bool bBelowHips = R.Hips >= 0 && IsUnder(P, G, R.Hips) && G != R.Hips;

	if (R.Mode == EMode::Forward || R.Mode == EMode::ForwardAll)
	{
		if (R.Turn)
		{
			ForwardTurn(P, S, G, *R.Turn);
		}
		if (R.Move)
		{
			Place(P, S, Start, G, Shifted(Start[G], *R.Move), true);
		}
		if (R.Mode == EMode::Forward)
		{
			if (const TArray<FHold> Held = HeldBelow(P, R); !Held.IsEmpty())
			{
				Out.Error = HoldLimbs(P, S, Held, R.Hips, R.MaxStretch, {}, R.Rules);
			}
			IsolateRotations(P, S, Start, R.Isolate, G);
		}
		return Out;
	}

	if (R.Mode == EMode::Free)
	{
		FreePose(P, S, R, Start);
		return Out;
	}

	if (R.Mode == EMode::Bidirectional || R.Mode == EMode::All)
	{
		if (!bBelowHips)
		{
			// The hips have nothing above them to give: both ways is Forward there.
			FRequest Fwd = R;
			Fwd.Mode = R.Mode == EMode::All ? EMode::ForwardAll : EMode::Forward;
			FResult F = SolveRequest(P, Fwd);
			F.Resolved = R.Mode;
			return F;
		}
		Out.Error = BothWays(P, S, R, Start, R.Mode == EMode::All);
		if (R.Mode == EMode::Bidirectional)
		{
			IsolateRotations(P, S, Start, R.Isolate, G);
		}
		return Out;
	}

	if (R.Mode == EMode::Backward || R.Mode == EMode::BackwardAll)
	{
		if (R.Hips >= 0 && !IsUnder(P, G, R.Hips))
		{
			Out.bHandled = false;
			return Out;
		}
		const bool bRigid = R.Mode == EMode::BackwardAll;
		TArray<FHold> Others = bRigid ? TArray<FHold>() : R.Holds;
		if (R.Turn)
		{
			ForwardTurn(P, S, G, *R.Turn);
			if (bBelowHips)
			{
				// What stays still: the first hold below the grabbed bone, else the bone itself, so the body hangs from it.
				const TArray<FHold> Below = HoldsUnder(P, R.Holds, G);
				Out.Anchor = Below.IsEmpty() ? G : Below[0].Bone;
				const TArray<FMatrix> Turned = Worlds(P, S);
				const FMatrix Undo = Mul(Start[Out.Anchor], Turned[Out.Anchor].Inverse());
				Place(P, S, Turned, R.Hips, Mul(Undo, Turned[R.Hips]), true);
				Others.RemoveAll([&](const FHold& H) { return H.Bone == Out.Anchor; });
			}
		}
		TArray<TPair<int32, FVector>> Soft;
		if (R.Move && R.Hips >= 0)
		{
			// The body is carried by the grabbed bone; the limbs that hold bend under it.
			const TArray<FMatrix> W = Worlds(P, S);
			Place(P, S, W, R.Hips, Shifted(W[R.Hips], *R.Move), true);
			if (!bRigid)
			{
				Soft.Emplace(G, Start[G].GetOrigin() + *R.Move);
			}
		}
		if (!bRigid)
		{
			Out.Error = HoldLimbs(P, S, Others, R.Hips, R.MaxStretch, Soft, R.Rules);
			IsolateRotations(P, S, Start, R.Isolate, G);
		}
		return Out;
	}

	// IK.
	Out.Resolved = EMode::Ik;
	if (R.Hips >= 0 && !IsUnder(P, G, R.Hips))
	{
		Out.bHandled = false;
		return Out;
	}
	if (R.Turn)
	{
		ForwardTurn(P, S, G, *R.Turn);
		Out.Error = HoldLimbs(P, S, HoldsUnder(P, R.Holds, G), G == R.Hips ? R.Hips : G, R.MaxStretch, {}, R.Rules);
	}
	if (R.Move)
	{
		if (G == R.Hips)
		{
			const TArray<FMatrix> W = Worlds(P, S);
			Place(P, S, W, G, Shifted(W[G], *R.Move), true);
			Out.Error = HoldLimbs(P, S, R.Holds, R.Hips, R.MaxStretch, {}, R.Rules);
		}
		else
		{
			const TArray<FMatrix> W = Worlds(P, S);
			FHold Target;
			Target.Bone = G;
			Target.World = Shifted(W[G], *R.Move);
			TArray<FHold> Holds = {Target};
			for (const FHold& H : R.Holds)
			{
				if (H.Bone != G)
				{
					Holds.Add(H);
				}
			}
			if (R.ChainLength > 0)
			{
				// A set length runs through branches: bend only those bones, the rest held.
				FProblem Q = P;
				TArray<int32> Chain;
				for (int32 B = G; B >= 0 && Chain.Num() < R.ChainLength + 1; B = P.Bones[B].Parent)
				{
					if (P.Bones[B].bSolve)
					{
						Chain.Add(B);
					}
				}
				for (int32 I = 0; I < Q.Bones.Num(); ++I)
				{
					Q.Bones[I].bSolve = Chain.Contains(I);
					Q.Bones[I].bTranslate = false;
					Q.Bones[I].Reference = Q.Bones[I].Rotation = S.Rotation[I];
				}
				FEffector E;
				E.Bone = G;
				E.Position = Target.World.GetOrigin();
				E.Rotation = QuatOf(Target.World);
				Q.Effectors = {E};
				const FSolution Sol = Solve(Q);
				for (const int32 B : Chain)
				{
					S.Rotation[B] = Sol.Rotation[B];
				}
				Out.Error = HoldLimbs(P, S, TArray<FHold>(Holds.GetData() + 1, Holds.Num() - 1), R.Hips, R.MaxStretch, {}, R.Rules);
				Out.Error = FMath::Max(Out.Error, (Worlds(P, S)[G].GetOrigin() - Target.World.GetOrigin()).Size());
			}
			else
			{
				Out.Error = HoldLimbs(P, S, Holds, R.Hips, R.MaxStretch, {}, R.Rules);
			}
		}
	}
	return Out;
}

namespace
{
// Lila's ikHandle (src/scene/Ik.cpp): its solve wrapper and solvers, on positions and world rotations.

FQuat AxisAngle(const FVector& Axis, const double Radians) { return FQuat(Axis.GetUnsafeNormal(), Radians); }

/** Shortest turn taking direction A onto B (both unit). */
FQuat Arc(const FVector& A, const FVector& B)
{
	const double C = FMath::Clamp(FVector::DotProduct(A, B), -1.0, 1.0);
	const FVector Axis = FVector::CrossProduct(A, B);
	if (Axis.Size() < 1e-12)
	{
		if (C > 0.0)
		{
			return FQuat::Identity;
		}
		const FVector Any = FMath::Abs(A.X) < 0.9 ? FVector(1, 0, 0) : FVector(0, 1, 0);
		return AxisAngle(FVector::CrossProduct(A, Any), UE_DOUBLE_PI);
	}
	return AxisAngle(Axis, FMath::Atan2(Axis.Size(), C));
}

FVector Perpendicular(const FVector& V, const FVector& Axis)
{
	const FVector P = V - FVector::DotProduct(V, Axis) * Axis;
	return P.Size() > 1e-9 ? P.GetUnsafeNormal() : FVector::ZeroVector;
}

/** glm::slerp: the short way round, linear when the two are within a hair. */
FQuat SlerpAsLila(const FQuat& X, const FQuat& Y, const double A)
{
	FQuat Z = Y;
	double CosTheta = X.X * Y.X + X.Y * Y.Y + X.Z * Y.Z + X.W * Y.W;
	if (CosTheta < 0.0)
	{
		Z = FQuat(-Y.X, -Y.Y, -Y.Z, -Y.W);
		CosTheta = -CosTheta;
	}
	if (CosTheta > 1.0 - DBL_EPSILON)
	{
		return FQuat(FMath::Lerp(X.X, Z.X, A), FMath::Lerp(X.Y, Z.Y, A), FMath::Lerp(X.Z, Z.Z, A), FMath::Lerp(X.W, Z.W, A));
	}
	const double Angle = std::acos(CosTheta);
	const double Sa = std::sin((1.0 - A) * Angle), Sb = std::sin(A * Angle), Si = std::sin(Angle);
	return FQuat((Sa * X.X + Sb * Z.X) / Si, (Sa * X.Y + Sb * Z.Y) / Si, (Sa * X.Z + Sb * Z.Z) / Si, (Sa * X.W + Sb * Z.W) / Si);
}

FVector BendDirection(const FChainIk& C, const FVector& Root, const FVector& Dir, const FVector& FkBend)
{
	FVector U = C.Pole ? Perpendicular(Root + *C.Pole - Root, Dir) : FVector::ZeroVector;
	if (U.IsZero())
	{
		U = Perpendicular(FkBend, Dir);
	}
	if (U.IsZero())
	{
		U = Perpendicular(FMath::Abs(FVector::DotProduct(Dir, C.Up)) < 0.9 ? C.Up : C.Side, Dir);
	}
	if (C.Twist != 0.0)
	{
		U = AxisAngle(Dir, C.Twist).RotateVector(U);
	}
	return U;
}

/** The turn taking frame (X, Y, X x Y) onto (A, B, A x B). */
FQuat FrameTurn(const FVector& FromX, const FVector& FromY, const FVector& ToX, const FVector& ToY)
{
	const FQuat From(FMatrix(FromX, FromY, FVector::CrossProduct(FromX, FromY), FVector::ZeroVector));
	const FQuat To(FMatrix(ToX, ToY, FVector::CrossProduct(ToX, ToY), FVector::ZeroVector));
	return (To * From.Inverse()).GetNormalized();
}

/** The chain as the solvers see it: joint positions and world rotations, the start's parent rotation, and the target. */
struct FChainState
{
	TArray<FVector> Pos;
	TArray<FQuat> Rot;
	FQuat Root = FQuat::Identity;
	FVector Target = FVector::ZeroVector;
};

bool Limited(const FChainIk& C)
{
	for (int32 I = 0; I + 1 < C.Slots.Num(); ++I)
	{
		if (C.Slots[I].Limit.Kind != FLimit::EKind::None || C.Slots[I].Stiffness > 0.0)
		{
			return true;
		}
	}
	return false;
}

/** The slot's limit applied to a local rotation, after its stiffness eased it back toward Start (Lila's limitLocal). */
FQuat LimitLocal(const FChainSlot& L, const FQuat& Local, const FQuat& Start)
{
	FQuat Q = Local.GetNormalized();
	if (L.Stiffness > 0.0)
	{
		Q = SlerpAsLila(Start.GetNormalized(), Q, 1.0 - FMath::Clamp(L.Stiffness, 0.0, 1.0));
	}
	if (L.Limit.Kind != FLimit::EKind::None)
	{
		Q = L.Orient * ClampRotation(L.Limit, L.Orient.Inverse() * Q);
	}
	return Q.GetNormalized();
}

/** Per joint but the end: the bone's direction in the joint's own frame, its length and its starting local rotation. */
struct FChainBones
{
	TArray<FVector> Dir;
	TArray<double> Len;
	TArray<FQuat> Start;
};

FChainBones BonesOf(const FChainState& K)
{
	FChainBones B;
	FQuat Parent = K.Root;
	for (int32 I = 0; I + 1 < K.Pos.Num(); ++I)
	{
		const FQuat R = K.Rot[I];
		const FVector D = K.Pos[I + 1] - K.Pos[I];
		const double L = D.Size();
		B.Dir.Add(L > 1e-12 ? R.UnrotateVector(D / L) : FVector(1, 0, 0));
		B.Len.Add(L);
		B.Start.Add(Parent.Inverse() * R);
		Parent = R;
	}
	return B;
}

TArray<FQuat> ForwardLimited(const FChainIk& C, const FChainState& K, const FChainBones& B, TArray<FVector>& Pos)
{
	TArray<FQuat> World;
	FQuat Parent = K.Root;
	for (int32 I = 0; I < B.Len.Num(); ++I)
	{
		FQuat R = Parent * B.Start[I];
		if (const FVector Want = Pos[I + 1] - Pos[I]; Want.Size() > 1e-12)
		{
			R = Arc(R.RotateVector(B.Dir[I]), Want.GetUnsafeNormal()) * R;
		}
		const FQuat Local = LimitLocal(C.Slots[I], Parent.Inverse() * R, B.Start[I]);
		World.Add(Parent * Local);
		Pos[I + 1] = Pos[I] + World[I].RotateVector(B.Dir[I]) * B.Len[I];
		Parent = World[I];
	}
	return World;
}

/** Cyclic coordinate descent from a limited pose, end to root, each joint as far as its limit and stiffness allow. */
TArray<FQuat> RefineLimited(const FChainIk& C, const FChainState& K, const FChainBones& B, const TArray<FQuat>& World)
{
	const int32 M = B.Len.Num();
	TArray<FQuat> Local, W;
	Local.SetNum(M);
	W.SetNum(M);
	for (int32 I = 0; I < M; ++I)
	{
		Local[I] = (I == 0 ? K.Root : World[I - 1]).Inverse() * World[I];
	}
	TArray<FVector> Pos;
	Pos.SetNum(M + 1);
	auto Pose = [&] {
		FQuat Parent = K.Root;
		Pos[0] = K.Pos[0];
		for (int32 I = 0; I < M; ++I)
		{
			W[I] = Parent * Local[I];
			Pos[I + 1] = Pos[I] + W[I].RotateVector(B.Dir[I]) * B.Len[I];
			Parent = W[I];
		}
		return (Pos[M] - K.Target).Size();
	};
	double BestError = Pose();
	TArray<FQuat> Best = W;
	for (int32 It = 0; It < 256 && BestError > 1e-9; ++It)
	{
		for (int32 I = M; I-- > 0;)
		{
			Pose();
			const FVector ToEnd = Pos[M] - Pos[I], ToTarget = K.Target - Pos[I];
			if (ToEnd.Size() < 1e-12 || ToTarget.Size() < 1e-12)
			{
				continue;
			}
			FQuat Turn = Arc(ToEnd.GetUnsafeNormal(), ToTarget.GetUnsafeNormal());
			const FChainSlot& L = C.Slots[I];
			if (L.Stiffness > 0.0)
			{
				Turn = SlerpAsLila(FQuat::Identity, Turn, 1.0 - FMath::Clamp(L.Stiffness, 0.0, 1.0));
			}
			FQuat Q = ((I == 0 ? K.Root : W[I - 1]).Inverse() * Turn * W[I]).GetNormalized();
			if (L.Limit.Kind != FLimit::EKind::None)
			{
				Q = L.Orient * ClampRotation(L.Limit, L.Orient.Inverse() * Q);
			}
			Local[I] = Q;
		}
		const double E = Pose();
		if (E >= BestError - 1e-15)
		{
			break;
		}
		BestError = E;
		Best = W;
	}
	return Best;
}

/** Refines a free solution under the limits with FABRIK passes whose forward half honours them; keeps the closest pose. */
TArray<FQuat> SolveLimited(const FChainIk& C, const FChainState& K, const TArray<FQuat>& Free)
{
	const FChainBones B = BonesOf(K);
	const int32 N = K.Pos.Num();
	TArray<FVector> Pos;
	Pos.SetNum(N);
	Pos[0] = K.Pos[0];
	for (int32 I = 0; I + 1 < N; ++I)
	{
		Pos[I + 1] = Pos[I] + Free[I].RotateVector(B.Dir[I]) * B.Len[I];
	}
	TArray<FQuat> Best = ForwardLimited(C, K, B, Pos);
	double BestError = (Pos[N - 1] - K.Target).Size();
	auto SafeDir = [](const FVector& V) { return V.Size() > 1e-12 ? V.GetUnsafeNormal() : FVector(1, 0, 0); };
	for (int32 It = 0, Stalled = 0; It < 256 && BestError > 1e-9 && Stalled < 8; ++It)
	{
		Pos[N - 1] = K.Target;
		for (int32 I = N - 1; I-- > 0;)
		{
			Pos[I] = Pos[I + 1] + SafeDir(Pos[I] - Pos[I + 1]) * B.Len[I];
		}
		Pos[0] = K.Pos[0];
		TArray<FQuat> World = ForwardLimited(C, K, B, Pos);
		if (const double E = (Pos[N - 1] - K.Target).Size(); E < BestError - 1e-12)
		{
			Best = MoveTemp(World);
			BestError = E;
			Stalled = 0;
		}
		else
		{
			++Stalled;
		}
	}
	return BestError > 1e-9 ? RefineLimited(C, K, B, Best) : Best;
}

/** Each joint turned by the shortest arc from where its bone points after its parent turned to where it should point. */
TArray<FQuat> FollowPositions(const FChainState& K, const TArray<FVector>& Pos)
{
	TArray<FQuat> Out;
	FQuat Inherited = FQuat::Identity;
	for (int32 I = 0; I + 1 < K.Pos.Num(); ++I)
	{
		const FVector FkDir = K.Pos[I + 1] - K.Pos[I], NewDir = Pos[I + 1] - Pos[I];
		FQuat Turn = Inherited;
		if (FkDir.Size() > 1e-12 && NewDir.Size() > 1e-12)
		{
			Turn = Arc(Inherited.RotateVector(FkDir.GetUnsafeNormal()).GetUnsafeNormal(), NewDir.GetUnsafeNormal()) * Inherited;
		}
		Out.Add(Turn * K.Rot[I]);
		Inherited = Turn;
	}
	return Out;
}

TArray<FQuat> SolveTwoBone(const FChainIk& C, const FChainState& K)
{
	const FVector P0 = K.Pos[0], P1 = K.Pos[1], P2 = K.Pos[2];
	const double A = (P1 - P0).Size(), B = (P2 - P1).Size();
	const FVector ToTarget = K.Target - P0, FkToEnd = P2 - P0;
	const double D = ToTarget.Size();
	TArray<FQuat> World = {K.Rot[0], K.Rot[1]};
	if (D < 1e-12 || A < 1e-12 || B < 1e-12 || FkToEnd.Size() < 1e-12)
	{
		return World;
	}
	const FVector Dir = ToTarget / D;
	const FVector FkDir = FkToEnd.GetUnsafeNormal();
	const FVector U = BendDirection(C, P0, Dir, Perpendicular(P1 - P0, Dir).IsZero() ? Arc(FkDir, Dir).RotateVector(P1 - P0) : P1 - P0);
	FVector FkU = Perpendicular(P1 - P0, FkDir);
	if (FkU.IsZero())
	{
		FkU = Perpendicular(Arc(Dir, FkDir).RotateVector(U), FkDir);
	}
	const FVector N = FVector::CrossProduct(Dir, U);
	const FQuat Align = FrameTurn(FkDir, FkU, Dir, U);
	const double Reach = FMath::Clamp(D, FMath::Abs(A - B), A + B);
	const double AngleNew = std::acos(FMath::Clamp((A * A + Reach * Reach - B * B) / (2.0 * A * Reach), -1.0, 1.0));
	const FVector Upper = (P1 - P0).GetUnsafeNormal();
	const double AngleFk = std::atan2(FVector::DotProduct(Upper, FkU), FVector::DotProduct(Upper, FkDir));
	const FQuat Turn0 = AxisAngle(N, AngleNew - AngleFk) * Align;
	const FVector Elbow = P0 + Turn0.RotateVector(P1 - P0);
	const FVector Want = P0 + Dir * Reach - Elbow;
	FQuat Turn1 = Turn0;
	if (Want.Size() > 1e-12)
	{
		Turn1 = Arc(Turn0.RotateVector(P2 - P1).GetUnsafeNormal(), Want.GetUnsafeNormal()) * Turn0;
	}
	World = {Turn0 * K.Rot[0], Turn1 * K.Rot[1]};
	return Limited(C) ? SolveLimited(C, K, World) : World;
}

TArray<FQuat> SolveFabrik(const FChainIk& C, const FChainState& K)
{
	const int32 N = K.Pos.Num();
	TArray<FVector> Pos = K.Pos;
	TArray<double> Len;
	double Reach = 0.0;
	for (int32 I = 0; I + 1 < N; ++I)
	{
		Reach += Len.Add_GetRef((Pos[I + 1] - Pos[I]).Size());
	}
	const FVector Root = Pos[0];
	auto SafeDir = [](const FVector& V) { return V.Size() > 1e-12 ? V.GetUnsafeNormal() : FVector(1, 0, 0); };
	if ((K.Target - Root).Size() >= Reach)
	{
		const FVector Dir = SafeDir(K.Target - Root);
		for (int32 I = 0; I + 1 < N; ++I)
		{
			Pos[I + 1] = Pos[I] + Dir * Len[I];
		}
	}
	else
	{
		const FVector Line = SafeDir(K.Target - Root);
		bool bOnLine = true;
		for (int32 I = 1; I + 1 < N && bOnLine; ++I)
		{
			bOnLine = ((Pos[I] - Root) - FVector::DotProduct(Pos[I] - Root, Line) * Line).Size() < 1e-9;
		}
		if (bOnLine && N > 2 && (Pos[N - 1] - Root).Size() > 1e-12)
		{
			const FQuat Aim = Arc((Pos[N - 1] - Root).GetUnsafeNormal(), Line);
			for (int32 I = 1; I < N; ++I)
			{
				Pos[I] = Root + Aim.RotateVector(Pos[I] - Root);
			}
		}
		for (int32 It = 0; It < 256 && (Pos[N - 1] - K.Target).Size() > 1e-9; ++It)
		{
			Pos[N - 1] = K.Target;
			for (int32 I = N - 1; I-- > 0;)
			{
				Pos[I] = Pos[I + 1] + SafeDir(Pos[I] - Pos[I + 1]) * Len[I];
			}
			Pos[0] = Root;
			for (int32 I = 0; I + 1 < N; ++I)
			{
				Pos[I + 1] = Pos[I] + SafeDir(Pos[I + 1] - Pos[I]) * Len[I];
			}
		}
	}
	FVector Axis = Pos[N - 1] - Root;
	if (N > 2 && Axis.Size() > 1e-12 && (C.Pole || C.Twist != 0.0))
	{
		Axis = Axis.GetUnsafeNormal();
		FVector Mid = FVector::ZeroVector;
		for (int32 I = 1; I + 1 < N; ++I)
		{
			Mid += Pos[I] - Root;
		}
		const FVector Bend = Perpendicular(Mid, Axis);
		if (!Bend.IsZero())
		{
			const FVector Want = BendDirection(C, Root, Axis, Bend);
			const FQuat Swing = AxisAngle(Axis, std::atan2(FVector::DotProduct(FVector::CrossProduct(Bend, Want), Axis), FVector::DotProduct(Bend, Want)));
			for (int32 I = 1; I + 1 < N; ++I)
			{
				Pos[I] = Root + Swing.RotateVector(Pos[I] - Root);
			}
		}
	}
	const TArray<FQuat> World = FollowPositions(K, Pos);
	return Limited(C) ? SolveLimited(C, K, World) : World;
}

FVector AlongPolyline(const TArray<FVector>& Pts, const TArray<double>& Cum, const double S)
{
	if (Pts.Num() == 1)
	{
		return Pts[0];
	}
	int32 K = Algo::UpperBound(Cum, S);
	K = FMath::Clamp(K, 1, Pts.Num() - 1);
	const double Seg = Cum[K] - Cum[K - 1];
	const double T = Seg > 1e-12 ? (S - Cum[K - 1]) / Seg : 0.0;
	return Pts[K - 1] + (Pts[K] - Pts[K - 1]) * T;
}

/** The chain laid along the curve from its start (each joint a bone's length from the last), then rolled and twisted. */
TArray<FQuat> SolveSpline(const FChainIk& C, const FChainState& K)
{
	const int32 N = K.Pos.Num();
	if (C.Curve.Num() < 2)
	{
		return SolveFabrik(C, K);
	}
	TArray<double> Cum;
	Cum.Init(0.0, C.Curve.Num());
	for (int32 I = 1; I < C.Curve.Num(); ++I)
	{
		Cum[I] = Cum[I - 1] + (C.Curve[I] - C.Curve[I - 1]).Size();
	}
	TArray<double> Len;
	double Chain = 0.0;
	for (int32 I = 0; I + 1 < N; ++I)
	{
		Chain += Len.Add_GetRef((K.Pos[I + 1] - K.Pos[I]).Size());
	}
	TArray<FVector> Pos;
	Pos.SetNum(N);
	Pos[0] = K.Pos[0];
	if (C.bFitCurve && Chain > 1e-12)
	{
		double S = 0.0;
		for (int32 I = 0; I + 1 < N; ++I)
		{
			Pos[I + 1] = AlongPolyline(C.Curve, Cum, S += Len[I] * Cum.Last() / Chain);
		}
	}
	else
	{
		double S = 0.0;
		const double Step = FMath::Max(Cum.Last() / double(C.Curve.Num() * 4), 1e-6);
		for (int32 I = 0; I + 1 < N; ++I)
		{
			double Lo = S, Hi = S;
			while ((AlongPolyline(C.Curve, Cum, Hi) - Pos[I]).Size() < Len[I] && Hi - S < Len[I] * 4.0 + Cum.Last())
			{
				Lo = Hi;
				Hi += Step;
			}
			for (int32 It = 0; It < 60; ++It)
			{
				const double Mid = 0.5 * (Lo + Hi);
				((AlongPolyline(C.Curve, Cum, Mid) - Pos[I]).Size() < Len[I] ? Lo : Hi) = Mid;
			}
			S = Hi;
			const FVector D = AlongPolyline(C.Curve, Cum, S) - Pos[I];
			Pos[I + 1] = D.Size() > 1e-12 ? Pos[I] + D.GetUnsafeNormal() * Len[I] : Pos[I];
		}
	}
	TArray<FQuat> World = FollowPositions(K, Pos);
	for (int32 I = 0; I + 1 < N; ++I)
	{
		const FVector Bone = Pos[I + 1] - Pos[I];
		const double Share = N > 2 ? double(I) / double(N - 2) : 1.0;
		const double Angle = C.Roll + C.Twist * Share;
		if (FMath::Abs(Angle) > 1e-15 && Bone.Size() > 1e-12)
		{
			World[I] = AxisAngle(Bone, Angle) * World[I];
		}
	}
	return World;
}
} // namespace

void SolveChain(const FProblem& P, FPose& S, const FChainIk& C, const FMatrix& Goal)
{
	TRACE_CPUPROFILER_EVENT_SCOPE(LilaStrings::SolveChain);

	const int32 N = C.Bones.Num();
	if (N < 2 || C.Slots.Num() != N)
	{
		return;
	}
	const TArray<FMatrix> Now = Worlds(P, S);
	const FMatrix ParentWorld = ParentWorldOf(P, Now, C.Bones[0]);
	// The handle solves from its rest locals (a preferred angle replacing the rotation) under the start's parent as it is now.
	TArray<FMatrix> Local;
	for (int32 I = 0; I < N; ++I)
	{
		const FChainSlot& L = C.Slots[I];
		Local.Add(FQuatRotationTranslationMatrix(I + 1 < N && L.Preferred ? *L.Preferred : L.RestRotation, L.Rest));
	}
	TArray<FMatrix> W;
	auto Build = [&] {
		W.Reset();
		FMatrix Parent = ParentWorld;
		for (const FMatrix& M : Local)
		{
			W.Add(Parent = Mul(Parent, M));
		}
	};
	Build();
	FChainState K;
	K.Root = QuatOf(ParentWorld);
	K.Target = Goal.GetOrigin();
	const FVector Root = W[0].GetOrigin();
	double Reach = 0.0;
	for (int32 I = 0; I + 1 < N; ++I)
	{
		Reach += (W[I + 1].GetOrigin() - W[I].GetOrigin()).Size();
	}
	const double D = (K.Target - Root).Size();
	double Soft = FMath::Min(D, Reach);
	if (C.Softness > 0.0 && Reach > 1e-12)
	{
		const double Ds = FMath::Min(C.Softness, Reach), Da = Reach - Ds;
		Soft = D > Da ? Da + Ds * (1.0 - std::exp(-(D - Da) / Ds)) : D;
	}
	double Stretch = 1.0;
	if (C.MaxStretch > 1.0 && Soft > 1e-12 && D > Soft)
	{
		Stretch = FMath::Min(D / Soft, C.MaxStretch);
		for (int32 I = 1; I < N; ++I)
		{
			Local[I].SetOrigin(Local[I].GetOrigin() * Stretch);
		}
		Build();
	}
	if (C.Softness > 0.0 && D > 1e-12)
	{
		K.Target = Root + (K.Target - Root) * (FMath::Min(D, Soft * Stretch) / D);
	}
	for (const FMatrix& M : W)
	{
		K.Pos.Add(M.GetOrigin());
		K.Rot.Add(QuatOf(M));
	}
	const TArray<FQuat> World = C.Solver == FChainIk::ESolver::TwoBone && N == 3 ? SolveTwoBone(C, K)
								: C.Solver == FChainIk::ESolver::Spline		  ? SolveSpline(C, K)
																			  : SolveFabrik(C, K);
	// The joints' lengths follow the stretch; the solution's locals are held to the limits, then posed.
	// Each joint's offset from its parent is its rest one times the stretch, whatever the pose had: the handle sets it.
	for (int32 I = 1; I < N; ++I)
	{
		const int32 B = C.Bones[I];
		S.Stretch[B] = 1.0;
		S.Offset[B] = C.Slots[I].Rest * Stretch - P.Bones[B].Before.GetOrigin();
	}
	FQuat Parent = K.Root;
	for (int32 I = 0; I + 1 < N && I < World.Num(); ++I)
	{
		FQuat Q = (Parent.Inverse() * World[I]).GetNormalized();
		if (const FChainSlot& L = C.Slots[I]; L.Limit.Kind != FLimit::EKind::None)
		{
			Q = (L.Orient * ClampRotation(L.Limit, L.Orient.Inverse() * Q)).GetNormalized();
		}
		const FQuat Solved = Parent * Q;
		FMatrix Target = FQuatRotationMatrix(Solved);
		Place(P, S, Worlds(P, S), C.Bones[I], Target, false);
		Parent = Solved;
	}
	// The end turns toward the controller by its align weight: the rotate between orient and axis blended as Lila's joint does.
	if (C.AlignWeight > 0.0)
	{
		const int32 End = C.Bones.Last();
		const FQuat Fk = S.Rotation[End];
		FPose Aligned = S;
		Place(P, Aligned, Worlds(P, Aligned), End, Goal, false);
		if (C.AlignWeight >= 1.0)
		{
			S.Rotation[End] = Aligned.Rotation[End];
		}
		else
		{
			const FChainSlot& L = C.Slots.Last();
			const FQuat From = L.Orient.Inverse() * Fk * L.Axis.Inverse(), To = L.Orient.Inverse() * Aligned.Rotation[End] * L.Axis.Inverse();
			S.Rotation[End] = (L.Orient * SlerpAsLila(From.GetNormalized(), To.GetNormalized(), C.AlignWeight) * L.Axis).GetNormalized();
		}
	}
}
} // namespace LilaStrings
