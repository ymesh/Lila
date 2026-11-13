#pragma once

#include "Components/ActorComponent.h"
#include "CoreMinimal.h"

#include "LilaCrowdSignals.generated.h"

/** A crossing of a Lila crowd: the ends of its walk, and the signal phase it opens in (-1 when it is always open). */
USTRUCT(BlueprintType)
struct LILALINKCROWDRUNTIME_API FLilaCrowdCrossing
{
	GENERATED_BODY()

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Crossing")
	FVector A = FVector::ZeroVector;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Crossing")
	FVector B = FVector::ZeroVector;

	/** The junction it belongs to; crossings of one junction share its cycle. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Crossing")
	FName Junction;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Crossing")
	int32 Phase = -1;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Crossing")
	int32 Phases = 0;
};

/**
 * Opens and closes a Lila crowd's crossings on its junctions' signal cycles while the game runs: each phase lasts
 * PhaseSeconds, and a crossing is open in its phase except for the last ClearSeconds, so people only start across on a
 * walk signal. Crowd agents don't pick a closed crossing; one who reaches it stands and tries again. Unsignalled
 * crossings stay open. LilaLink's setMassCrowd puts one on the crowd's lanes actor.
 */
UCLASS(ClassGroup = (Lila), meta = (BlueprintSpawnableComponent))
class LILALINKCROWDRUNTIME_API ULilaCrowdSignals : public UActorComponent
{
	GENERATED_BODY()

public:
	ULilaCrowdSignals();

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Signals")
	TArray<FLilaCrowdCrossing> Crossings;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Signals", meta = (ClampMin = "1.0", ForceUnits = "s"))
	float PhaseSeconds = 20.0f;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Signals", meta = (ClampMin = "0.0", ForceUnits = "s"))
	float ClearSeconds = 4.0f;

	/**
	 * How long before a crossing's walk ends agents stop choosing it: they pick their next lane up to a move ahead, so
	 * one who chose it at the last moment still reaches the kerb in time to start across on the walk.
	 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Signals", meta = (ClampMin = "0.0", ForceUnits = "s"))
	float CommitSeconds = 0.0f;

	/** Whether the crossing at Index shows walk at a time (seconds since play began); Lead seconds earlier with Lead. */
	bool IsOpenAt(int32 Index, double Seconds, double Lead = 0.0) const;

	/** Per crossing: open to choosing and showing walk now, and how many people started across on walk and off it. */
	struct FState
	{
		/** Agents may choose it (its walk, less the commit lead). */
		bool bOpen = true;
		/** It shows walk. */
		bool bWalk = true;
		int32 EnteredOpen = 0;
		int32 EnteredClosed = 0;
		int32 Lanes = 0;
	};
	const TArray<FState>& States() const { return Current; }

	virtual void TickComponent(float DeltaTime, ELevelTick TickType, FActorComponentTickFunction* ThisTickFunction) override;

private:
	/** Each crossing's lanes (one each way), found once the ZoneGraph is there. */
	TArray<TArray<int32>> LaneIndex;
	TArray<TArray<int32>> LastCount;
	int32 DataIndex = INDEX_NONE;
	int32 DataGeneration = 0;
	TArray<FState> Current;
	bool Resolve();
};
