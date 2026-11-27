#pragma once

#include "Components/ActorComponent.h"
#include "CoreMinimal.h"

#include "LilaWaterRoute.generated.h"

/** One point of a Lila route lane. Lengths cm, Unreal space. */
USTRUCT(BlueprintType)
struct LILALINKRUNTIME_API FLilaRoutePoint
{
	GENERATED_BODY()

	/** Boats: clear water width at this point. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Lila")
	float Width = 0.0f;

	/** Boats: water depth at this point. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Lila")
	float Depth = 0.0f;

	/** Aircraft: height over the highest ground across the corridor. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Lila")
	float Clearance = 0.0f;
};

/**
 * A lane of a Lila terrain's boat or air route graph on a spline actor, shaped like a ZoneGraph lane: a craft class (its
 * lane profile), a kind, the graph nodes at its ends and per-point clearance data. Feed it to ZoneGraph shapes, AI
 * navigation or a mission system; LilaLink rewrites it on every send.
 */
UCLASS(BlueprintType, ClassGroup = (Lila), meta = (BlueprintSpawnableComponent))
class LILALINKRUNTIME_API ULilaWaterRoute : public UActorComponent
{
	GENERATED_BODY()

public:
	/** The lane's id in Lila, such as coast_1, channel_estuary or air_rancho_approach_a. */
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Lila")
	FString LaneId;

	/** coast, channel, river, harbour, link, approach or departure. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Lila")
	FName Kind;

	/** Craft class: skiff, airboat, goFast, semiSubmersible, cutter, light or twin. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Lila")
	FName Craft;

	/** An air lane rather than a boat lane. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Lila")
	bool bAir = false;

	/** Lane profile width: the craft's narrowest channel, or the air corridor's width. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Lila")
	float ProfileWidth = 0.0f;

	/** Graph node at the lane's start. */
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Lila")
	FString FromNode;

	/** Graph node at the lane's end. */
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Lila")
	FString ToNode;

	/** Part of the main boat network. */
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Lila")
	bool bMainNetwork = false;

	/** Per spline point, by index. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Lila")
	TArray<FLilaRoutePoint> Points;

	/** The actor's Lila route lane, or null. */
	UFUNCTION(BlueprintPure, Category = "Lila")
	static ULilaWaterRoute* FindOn(const AActor* Actor);
};
