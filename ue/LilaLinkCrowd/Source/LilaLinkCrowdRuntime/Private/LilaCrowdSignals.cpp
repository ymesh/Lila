#include "LilaCrowdSignals.h"

#include "MassCrowdSettings.h"
#include "MassCrowdSubsystem.h"
#include "Modules/ModuleManager.h"
#include "ProfilingDebugging/CpuProfilerTrace.h"
#include "ZoneGraphSubsystem.h"

#include UE_INLINE_GENERATED_CPP_BY_NAME(LilaCrowdSignals)

IMPLEMENT_MODULE(FDefaultModuleImpl, LilaLinkCrowdRuntime)

ULilaCrowdSignals::ULilaCrowdSignals()
{
	PrimaryComponentTick.bCanEverTick = true;
	PrimaryComponentTick.bStartWithTickEnabled = true;
}

bool ULilaCrowdSignals::IsOpenAt(const int32 Index, const double Seconds, const double Lead) const
{
	if (!Crossings.IsValidIndex(Index))
	{
		return false;
	}
	const FLilaCrowdCrossing& C = Crossings[Index];
	if (C.Phase < 0 || C.Phases <= 0)
	{
		return true;
	}
	const double Cycle = double(PhaseSeconds) * C.Phases;
	const double In = FMath::Fmod(Seconds, Cycle);
	const double Start = double(PhaseSeconds) * C.Phase;
	return In >= Start && In < Start + FMath::Max(0.0, double(PhaseSeconds) - double(ClearSeconds) - Lead);
}

bool ULilaCrowdSignals::Resolve()
{
	TRACE_CPUPROFILER_EVENT_SCOPE(LilaCrowdSignals::Resolve);

	const UZoneGraphSubsystem* Graph = GetWorld() ? GetWorld()->GetSubsystem<UZoneGraphSubsystem>() : nullptr;
	if (!Graph)
	{
		return false;
	}
	const FZoneGraphTag CrossingTag = GetDefault<UMassCrowdSettings>()->CrossingTag;
	for (const FRegisteredZoneGraphData& Data : Graph->GetRegisteredZoneGraphData())
	{
		if (!Data.bInUse || !Data.ZoneGraphData)
		{
			continue;
		}
		const FZoneGraphStorage& Storage = Data.ZoneGraphData->GetStorage();
		TArray<TArray<int32>> Found;
		Found.SetNum(Crossings.Num());
		int32 Matched = 0;
		for (int32 L = 0; L < Storage.Lanes.Num(); ++L)
		{
			const FZoneLaneData& Lane = Storage.Lanes[L];
			if (!Lane.Tags.Contains(CrossingTag) || Lane.PointsEnd - Lane.PointsBegin < 2)
			{
				continue;
			}
			const FVector Mid = 0.5 * (Storage.LanePoints[Lane.PointsBegin] + Storage.LanePoints[Lane.PointsEnd - 1]);
			for (int32 C = 0; C < Crossings.Num(); ++C)
			{
				const FVector CrossMid = 0.5 * (Crossings[C].A + Crossings[C].B);
				if (FVector::Dist2D(Mid, CrossMid) < 200.0)
				{
					Matched += Found[C].IsEmpty() ? 1 : 0;
					Found[C].Add(L);
					break;
				}
			}
		}
		if (Matched > 0)
		{
			LaneIndex = MoveTemp(Found);
			DataIndex = Storage.DataHandle.Index;
			DataGeneration = Storage.DataHandle.Generation;
			LastCount.SetNum(Crossings.Num());
			Current.SetNum(Crossings.Num());
			for (int32 C = 0; C < Crossings.Num(); ++C)
			{
				LastCount[C].Init(0, LaneIndex[C].Num());
				Current[C].Lanes = LaneIndex[C].Num();
				Current[C].bOpen = !IsOpenAt(C, 0.0);
			}
			return true;
		}
	}
	return false;
}

void ULilaCrowdSignals::TickComponent(const float DeltaTime, const ELevelTick TickType, FActorComponentTickFunction* ThisTickFunction)
{
	TRACE_CPUPROFILER_EVENT_SCOPE(LilaCrowdSignals::TickComponent);

	Super::TickComponent(DeltaTime, TickType, ThisTickFunction);
	UWorld* World = GetWorld();
	if (!World || !World->IsGameWorld())
	{
		return;
	}
	UMassCrowdSubsystem* Crowd = World->GetSubsystem<UMassCrowdSubsystem>();
	if (!Crowd || (DataIndex == INDEX_NONE && !Resolve()))
	{
		return;
	}
	FZoneGraphDataHandle Data;
	Data.Index = uint16(DataIndex);
	Data.Generation = uint16(DataGeneration);
	const double Seconds = World->GetTimeSeconds();
	for (int32 C = 0; C < LaneIndex.Num(); ++C)
	{
		FState& S = Current[C];
		const bool bOpen = IsOpenAt(C, Seconds, CommitSeconds);
		const bool bWalk = IsOpenAt(C, Seconds);
		for (int32 K = 0; K < LaneIndex[C].Num(); ++K)
		{
			FZoneGraphLaneHandle Lane;
			Lane.Index = LaneIndex[C][K];
			Lane.DataHandle = Data;
			if (bOpen != S.bOpen)
			{
				Crowd->SetLaneState(Lane, bOpen ? ECrowdLaneState::Opened : ECrowdLaneState::Closed);
			}
			// Entries counted from the people on the lane: a rise is someone starting across.
			if (const FCrowdTrackingLaneData* Tracking = Crowd->GetCrowdTrackingLaneData(Lane))
			{
				if (const int32 Rise = Tracking->NumEntitiesOnLane - LastCount[C][K]; Rise > 0)
				{
					(S.bWalk ? S.EnteredOpen : S.EnteredClosed) += Rise;
				}
				LastCount[C][K] = Tracking->NumEntitiesOnLane;
			}
		}
		S.bOpen = bOpen;
		S.bWalk = bWalk;
	}
}
