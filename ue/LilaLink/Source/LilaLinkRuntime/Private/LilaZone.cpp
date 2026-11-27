#include "LilaZone.h"

#include "Components/BrushComponent.h"
#include "ProfilingDebugging/CpuProfilerTrace.h"

namespace
{
bool InsideRing(const TArray<FVector2D>& Ring, const FVector2D& P)
{
	bool bIn = false;
	const int32 N = Ring.Num();
	for (int32 I = 0, J = N - 1; I < N; J = I++)
	{
		const FVector2D& A = Ring[I];
		const FVector2D& B = Ring[J];
		if ((A.Y > P.Y) != (B.Y > P.Y) && P.X < (B.X - A.X) * (P.Y - A.Y) / (B.Y - A.Y) + A.X)
		{
			bIn = !bIn;
		}
	}
	return bIn;
}

/** Winners first: priority, then the later zone, as Lila orders them. */
TArray<int32> WinOrder(const FLilaZoneLayer& Layer)
{
	TArray<int32> Order;
	for (int32 I = 0; I < Layer.Zones.Num(); ++I)
	{
		Order.Add(I);
	}
	Order.StableSort([&Layer](int32 A, int32 B) {
		if (Layer.Zones[A].Priority != Layer.Zones[B].Priority)
		{
			return Layer.Zones[A].Priority > Layer.Zones[B].Priority;
		}
		return A > B;
	});
	return Order;
}

float NumberOr(const TMap<FName, FString>& Attributes, const TCHAR* Key, float Fallback)
{
	const FString* Value = Attributes.Find(FName(Key));
	return Value && Value->IsNumeric() ? FCString::Atof(**Value) : Fallback;
}
} // namespace

float FLilaZoneMask::At(const FVector& Location) const
{
	const double U2 = AxisU.SizeSquared(), V2 = AxisV.SizeSquared();
	if (Values.IsEmpty() || U2 <= 0.0 || V2 <= 0.0)
	{
		return 0.0f;
	}
	const FVector D = Location - Origin;
	const int32 U = FMath::FloorToInt32((D | AxisU) / U2);
	const int32 V = FMath::FloorToInt32((D | AxisV) / V2);
	if (U < 0 || V < 0 || U >= Size.X || V >= Size.Y)
	{
		return 0.0f;
	}
	return Values[V * Size.X + U] / 255.0f;
}

float FLilaZoneInfo::WeightAt(const FVector& Location) const
{
	if (Top > Bottom && (Location.Z < Bottom || Location.Z > Top))
	{
		return 0.0f;
	}
	const FVector2D P(Location.X, Location.Y);
	if (Mask.IsEmpty())
	{
		for (const FLilaZoneRing& Ring : Outline)
		{
			if (InsideRing(Ring.Points, P))
			{
				for (const FLilaZoneRing& Hole : Holes)
				{
					if (InsideRing(Hole.Points, P))
					{
						return 0.0f;
					}
				}
				return 1.0f;
			}
		}
		return 0.0f;
	}
	return Mask.At(Location);
}

bool ULilaZoneData::QueryLayer(FName LayerId, const FVector& Location, FLilaZoneHit& Hit) const
{
	TRACE_CPUPROFILER_EVENT_SCOPE(ULilaZoneData::QueryLayer);
	for (const FLilaZoneLayer& Layer : Layers)
	{
		if (Layer.Id != LayerId)
		{
			continue;
		}
		Hit = FLilaZoneHit();
		Hit.Layer = Layer.Id;
		Hit.LayerType = Layer.Type;
		for (int32 Index : WinOrder(Layer))
		{
			const FLilaZoneInfo& Zone = Layer.Zones[Index];
			const float W = Zone.WeightAt(Location);
			if (Layer.bDensity ? W > 0.0f : W >= 0.5f)
			{
				Hit.Zone = Zone.Id;
				Hit.ZoneName = Zone.Name;
				Hit.Attributes = Zone.Attributes;
				Hit.Weight = W;
				Hit.Density = Layer.bDensity ? NumberOr(Zone.Attributes, TEXT("density"), 1.0f) * W : 0.0f;
				return true;
			}
		}
		return false;
	}
	return false;
}

TArray<FLilaZoneHit> ULilaZoneData::Query(const FVector& Location) const
{
	TRACE_CPUPROFILER_EVENT_SCOPE(ULilaZoneData::Query);
	TArray<FLilaZoneHit> Out;
	for (const FLilaZoneLayer& Layer : Layers)
	{
		FLilaZoneHit Hit;
		QueryLayer(Layer.Id, Location, Hit);
		Hit.Layer = Layer.Id;
		Hit.LayerType = Layer.Type;
		Out.Add(Hit);
	}
	return Out;
}

ALilaZoneVolume::ALilaZoneVolume(const FObjectInitializer& ObjectInitializer) : Super(ObjectInitializer)
{
	// Overlaps only: zones never block anything.
	GetBrushComponent()->SetCollisionProfileName(TEXT("OverlapAll"));
	GetBrushComponent()->SetGenerateOverlapEvents(true);
	bColored = true;
	BrushColor = FColor(255, 160, 60, 255);
}

FString ALilaZoneVolume::GetAttribute(FName Key, const FString& Fallback) const
{
	const FString* Value = Attributes.Find(Key);
	return Value ? *Value : Fallback;
}
