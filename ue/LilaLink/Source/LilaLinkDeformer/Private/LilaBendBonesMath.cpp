#include "LilaBendBonesMath.h"

#include "Algo/BinarySearch.h"
#include "ProfilingDebugging/CpuProfilerTrace.h"

namespace LilaBendBones
{
namespace
{
FVector Perpendicular(const FVector& V, const FVector& T)
{
	const FVector P = V - FVector::DotProduct(V, T) * T;
	const double L = P.Size();
	if (L > 1e-12)
	{
		return P / L;
	}
	const FVector Any = FMath::Abs(T.X) < 0.9 ? FVector(1, 0, 0) : FVector(0, 1, 0);
	return FVector::CrossProduct(T, FVector::CrossProduct(Any, T)).GetSafeNormal();
}

FVector CatmullRom(const FVector& P0, const FVector& P1, const FVector& P2, const FVector& P3, const double U)
{
	auto Knot = [](double T, const FVector& A, const FVector& B) { return T + FMath::Max(FMath::Sqrt((B - A).Size()), 1e-6); };
	const double T0 = 0.0, T1 = Knot(T0, P0, P1), T2 = Knot(T1, P1, P2), T3 = Knot(T2, P2, P3);
	const double T = T1 + (T2 - T1) * U;
	const FVector A1 = (T1 - T) / (T1 - T0) * P0 + (T - T0) / (T1 - T0) * P1;
	const FVector A2 = (T2 - T) / (T2 - T1) * P1 + (T - T1) / (T2 - T1) * P2;
	const FVector A3 = (T3 - T) / (T3 - T2) * P2 + (T - T2) / (T3 - T2) * P3;
	const FVector B1 = (T2 - T) / (T2 - T0) * A1 + (T - T0) / (T2 - T0) * A2;
	const FVector B2 = (T3 - T) / (T3 - T1) * A2 + (T - T1) / (T3 - T1) * A3;
	return (T2 - T) / (T2 - T1) * B1 + (T - T1) / (T2 - T1) * B2;
}

/** A frame from its tangent and normal: X tangent, Y normal, Z their cross. */
FQuat FrameOf(const FVector& Tangent, const FVector& Normal)
{
	return FQuat(FMatrix(Tangent, Normal, FVector::CrossProduct(Tangent, Normal), FVector::ZeroVector));
}

TPair<int32, double> SampleAt(const FCurve& C, const double S)
{
	int32 I = Algo::UpperBound(C.Length, S) - 1;
	I = FMath::Clamp(I, 0, C.Length.Num() - 2);
	const double Span = C.Length[I + 1] - C.Length[I];
	return {I, Span > 1e-12 ? FMath::Clamp((S - C.Length[I]) / Span, 0.0, 1.0) : 0.0};
}

FVector PointAt(const FCurve& C, const double S)
{
	const auto [I, F] = SampleAt(C, S);
	return FMath::Lerp(C.Point[I], C.Point[I + 1], F);
}

FQuat FrameAt(const FCurve& C, const double S)
{
	const auto [I, F] = SampleAt(C, S);
	return FQuat::Slerp(C.Frame[I], C.Frame[I + 1], F).GetNormalized();
}

/** The fraction of segment K's strand length (the line at Offset from the curve) at arc length S. */
double StrandFraction(const FCurve& C, const int32 K, const FVector2D& Offset, const double S)
{
	const int32 From = C.Knot[K], To = C.Knot[K + 1];
	auto At = [&](int32 I) { return C.Point[I] + C.Frame[I].RotateVector(FVector(0.0, Offset.X, Offset.Y)); };
	double Total = 0.0, UpTo = 0.0;
	FVector Prev = At(From);
	for (int32 I = From + 1; I <= To; ++I)
	{
		const FVector Q = At(I);
		const double D = (Q - Prev).Size();
		if (C.Length[I] <= S)
		{
			UpTo += D;
		}
		else if (C.Length[I - 1] < S)
		{
			UpTo += D * (S - C.Length[I - 1]) / FMath::Max(C.Length[I] - C.Length[I - 1], 1e-12);
		}
		Total += D;
		Prev = Q;
	}
	return Total > 1e-12 ? UpTo / Total : 0.0;
}
} // namespace

FCurve Build(const FChain& Chain, const TArray<FVector>& Normals, const int32 SamplesPerSpan)
{
	TRACE_CPUPROFILER_EVENT_SCOPE(LilaBendBones::Build);

	FCurve C;
	const int32 Joints = Chain.Position.Num();
	if (Joints < 2)
	{
		return C;
	}
	// The control points: each joint, then its bone's CVs.
	TArray<FVector> Control;
	TArray<int32> JointControl;
	for (int32 J = 0; J < Joints; ++J)
	{
		JointControl.Add(Control.Num());
		Control.Add(Chain.Position[J]);
		if (J + 1 < Joints && Chain.Cvs.IsValidIndex(J))
		{
			Control.Append(Chain.Cvs[J]);
		}
	}
	auto At = [&](int32 I) {
		if (I < 0)
		{
			return 2.0 * Control[0] - Control[1];
		}
		if (I >= Control.Num())
		{
			return 2.0 * Control.Last() - Control[Control.Num() - 2];
		}
		return Control[I];
	};
	for (int32 Sp = 0; Sp + 1 < Control.Num(); ++Sp)
	{
		for (int32 K = 0; K < SamplesPerSpan; ++K)
		{
			C.Point.Add(CatmullRom(At(Sp - 1), At(Sp), At(Sp + 1), At(Sp + 2), double(K) / SamplesPerSpan));
		}
	}
	C.Point.Add(Control.Last());
	for (const int32 Jc : JointControl)
	{
		C.Knot.Add(Jc * SamplesPerSpan);
	}

	const int32 N = C.Point.Num();
	C.Length.SetNumZeroed(N);
	for (int32 I = 1; I < N; ++I)
	{
		C.Length[I] = C.Length[I - 1] + (C.Point[I] - C.Point[I - 1]).Size();
	}
	TArray<FVector> Tangent;
	Tangent.SetNum(N);
	for (int32 I = 0; I < N; ++I)
	{
		const FVector D = C.Point[FMath::Min(I + 1, N - 1)] - C.Point[I == 0 ? 0 : I - 1];
		Tangent[I] = D.Size() > 1e-12 ? D.GetSafeNormal() : (I ? Tangent[I - 1] : FVector(1, 0, 0));
	}

	auto JointNormal = [&](int32 J, const FVector& T) {
		if (J < Normals.Num())
		{
			return Perpendicular(Chain.Rotation[J].RotateVector(Normals[J]), T);
		}
		// The joint's axis least along the curve.
		const FVector Axes[3] = {Chain.Rotation[J].GetAxisX(), Chain.Rotation[J].GetAxisY(), Chain.Rotation[J].GetAxisZ()};
		int32 Least = 0;
		for (int32 A = 1; A < 3; ++A)
		{
			if (FMath::Abs(FVector::DotProduct(Axes[A], T)) < FMath::Abs(FVector::DotProduct(Axes[Least], T)))
			{
				Least = A;
			}
		}
		return Perpendicular(Axes[Least], T);
	};

	// Rotation-minimising frames by double reflection, from the first joint's normal.
	TArray<FVector> Normal;
	Normal.SetNum(N);
	Normal[0] = JointNormal(0, Tangent[0]);
	for (int32 I = 0; I + 1 < N; ++I)
	{
		const FVector V1 = C.Point[I + 1] - C.Point[I];
		const double C1 = FVector::DotProduct(V1, V1);
		if (C1 < 1e-24)
		{
			Normal[I + 1] = Perpendicular(Normal[I], Tangent[I + 1]);
			continue;
		}
		const FVector RL = Normal[I] - (2.0 / C1) * FVector::DotProduct(V1, Normal[I]) * V1;
		const FVector TL = Tangent[I] - (2.0 / C1) * FVector::DotProduct(V1, Tangent[I]) * V1;
		const FVector V2 = Tangent[I + 1] - TL;
		const double C2 = FVector::DotProduct(V2, V2);
		Normal[I + 1] = Perpendicular(C2 < 1e-24 ? RL : RL - (2.0 / C2) * FVector::DotProduct(V2, RL) * V2, Tangent[I + 1]);
	}

	// Twist: at each joint, the turn from the carried normal to the joint's own, ramped along the arc in between.
	TArray<double> Twist;
	Twist.SetNumZeroed(Joints);
	for (int32 J = 0; J < Joints && Normals.Num() > 0; ++J)
	{
		const int32 I = C.Knot[J];
		const FVector Want = JointNormal(J, Tangent[I]);
		Twist[J] = FMath::Atan2(FVector::DotProduct(FVector::CrossProduct(Normal[I], Want), Tangent[I]), FVector::DotProduct(Normal[I], Want));
		if (J > 0)
		{
			while (Twist[J] - Twist[J - 1] > UE_DOUBLE_PI)
			{
				Twist[J] -= 2.0 * UE_DOUBLE_PI;
			}
			while (Twist[J] - Twist[J - 1] < -UE_DOUBLE_PI)
			{
				Twist[J] += 2.0 * UE_DOUBLE_PI;
			}
		}
	}
	C.Frame.SetNum(N);
	for (int32 I = 0; I < N; ++I)
	{
		int32 J = 0;
		while (J + 2 < Joints && C.Knot[J + 1] <= I)
		{
			++J;
		}
		const double A = C.Length[C.Knot[J]], B = C.Length[C.Knot[J + 1]];
		const double F = B > A ? FMath::Clamp((C.Length[I] - A) / (B - A), 0.0, 1.0) : 0.0;
		const double Angle = FMath::Lerp(Twist[J], Twist[J + 1], F);
		const FVector NN = FQuat(Tangent[I], Angle).RotateVector(Normal[I]);
		C.Frame[I] = FrameOf(Tangent[I], NN);
	}
	return C;
}

void PlaceCvs(FChain& Chain, const TArray<TPair<int32, FVector>>& Cvs)
{
	Chain.Cvs.SetNum(Chain.Position.Num());
	for (const TPair<int32, FVector>& Cv : Cvs)
	{
		if (Cv.Key >= 0 && Cv.Key + 1 < Chain.Position.Num())
		{
			Chain.Cvs[Cv.Key].Add(Cv.Value);
		}
	}
	for (int32 Bone = 0; Bone + 1 < Chain.Position.Num(); ++Bone)
	{
		const FVector A = Chain.Position[Bone], D = Chain.Position[Bone + 1] - A;
		Chain.Cvs[Bone].Sort([&](const FVector& X, const FVector& Y) { return FVector::DotProduct(X - A, D) < FVector::DotProduct(Y - A, D); });
	}
}

TArray<FVector> RestNormals(const FChain& Chain, const FCurve& Rest)
{
	TArray<FVector> Out;
	for (int32 J = 0; J < Chain.Position.Num(); ++J)
	{
		Out.Add(Chain.Rotation[J].UnrotateVector(Rest.Frame[Rest.Knot[J]].GetAxisY()));
	}
	return Out;
}

FPlace Locate(const FCurve& Rest, const FVector& P)
{
	double Best = TNumericLimits<double>::Max(), S = 0.0;
	for (int32 I = 0; I + 1 < Rest.Point.Num(); ++I)
	{
		const FVector A = Rest.Point[I], D = Rest.Point[I + 1] - A;
		const double Len2 = FVector::DotProduct(D, D);
		const double F = Len2 > 1e-24 ? FMath::Clamp(FVector::DotProduct(P - A, D) / Len2, 0.0, 1.0) : 0.0;
		if (const double Dist = (P - (A + D * F)).Size(); Dist < Best)
		{
			Best = Dist;
			S = FMath::Lerp(Rest.Length[I], Rest.Length[I + 1], F);
		}
	}
	FPlace Out;
	Out.Distance = Best;
	Out.ArcLength = S;
	const int32 Segments = Rest.Knot.Num() - 1;
	while (Out.Segment + 1 < Segments && Rest.Length[Rest.Knot[Out.Segment + 1]] < S)
	{
		++Out.Segment;
	}
	const double A = Rest.Length[Rest.Knot[Out.Segment]], B = Rest.Length[Rest.Knot[Out.Segment + 1]];
	Out.Fraction = B > A ? (S - A) / (B - A) : 0.0;
	Out.Frame = FrameAt(Rest, S);
	Out.Offset = Out.Frame.UnrotateVector(P - PointAt(Rest, S));
	Out.Strand = StrandFraction(Rest, Out.Segment, FVector2D(Out.Offset.Y, Out.Offset.Z), S);
	return Out;
}
} // namespace LilaBendBones
