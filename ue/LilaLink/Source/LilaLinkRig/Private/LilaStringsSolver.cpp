#include "LilaStringsSolver.h"

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

FQuat TurnBetween(const FVector& From, const FVector& To)
{
	const FVector A = From.GetSafeNormal(), B = To.GetSafeNormal();
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
	return FQuat(Axis.GetSafeNormal(), FMath::Acos(FMath::Clamp(C, -1.0, 1.0)));
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
	const FVector Dir = (Target - A).GetSafeNormal();
	const double D = FMath::Clamp((Target - A).Size(), FMath::Abs(L1 - L2) + 1e-6, L1 + L2 - 1e-6);
	FVector Bend = (B - A) - FVector::DotProduct(B - A, Dir) * Dir;
	if (Bend.Size() < 1e-6 * L1)
	{
		const FVector Reach = (C - A).GetSafeNormal();
		Bend = (B - A) - FVector::DotProduct(B - A, Reach) * Reach;
		Bend -= FVector::DotProduct(Bend, Dir) * Dir;
		if (Bend.Size() < 1e-9)
		{
			return;
		}
	}
	Bend = Bend.GetSafeNormal();
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

/** Brings each hold's bone to its world by bending its limb from pose S. Returns the largest hold error left. */
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

FResult SolveRequest(const FProblem& P, const FRequest& R)
{
	TRACE_CPUPROFILER_EVENT_SCOPE(LilaStrings::SolveRequest);

	check(P.Bones.IsValidIndex(R.Grabbed));
	if (R.Mode == EMode::FullBody)
	{
		return FullBody(P, R);
	}
	FResult Out;
	Out.Pose = StartPose(P);
	FPose& S = Out.Pose;
	const int32 G = R.Grabbed;
	const TArray<FMatrix> Start = Worlds(P, S);
	const bool bBelowHips = R.Hips >= 0 && IsUnder(P, G, R.Hips) && G != R.Hips;

	if (R.Mode == EMode::Forward)
	{
		if (R.Turn)
		{
			ForwardTurn(P, S, G, *R.Turn);
		}
		if (R.Move)
		{
			Place(P, S, Start, G, Shifted(Start[G], *R.Move), true);
		}
		return Out;
	}

	if (R.Mode == EMode::Backward)
	{
		if (R.Hips >= 0 && !IsUnder(P, G, R.Hips))
		{
			Out.bHandled = false;
			return Out;
		}
		TArray<FHold> Others = R.Holds;
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
			Soft.Emplace(G, Start[G].GetOrigin() + *R.Move);
		}
		Out.Error = HoldLimbs(P, S, Others, R.Hips, R.MaxStretch, Soft, R.Rules);
		return Out;
	}

	// IK.
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
} // namespace LilaStrings
