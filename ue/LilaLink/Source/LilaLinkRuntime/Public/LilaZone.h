#pragma once

#include "CoreMinimal.h"
#include "Engine/DataAsset.h"
#include "GameFramework/Volume.h"

#include "LilaZone.generated.h"

/** One ring of a zone's outline in Unreal XY, cm. */
USTRUCT(BlueprintType)
struct LILALINKRUNTIME_API FLilaZoneRing
{
	GENERATED_BODY()

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Lila")
	TArray<FVector2D> Points;
};

/**
 * A zone's painted mask: a value 0 to 255 per cell. Cell (u, v) of a world point P is
 * ((P - Origin) | AxisU) / |AxisU|^2 and the same for V, rows along V.
 */
USTRUCT(BlueprintType)
struct LILALINKRUNTIME_API FLilaZoneMask
{
	GENERATED_BODY()

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Lila")
	FVector Origin = FVector::ZeroVector;

	/** One cell along the mask's columns, world space. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Lila")
	FVector AxisU = FVector::ZeroVector;

	/** One cell along the mask's rows, world space. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Lila")
	FVector AxisV = FVector::ZeroVector;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Lila")
	FIntPoint Size = FIntPoint::ZeroValue;

	/** Size.X * Size.Y values, row by row. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Lila")
	TArray<uint8> Values;

	/** 0 to 1 at a world point; 0 outside. */
	float At(const FVector& Location) const;
	bool IsEmpty() const { return Values.IsEmpty(); }
};

/** One zone of a Lila zone layer. */
USTRUCT(BlueprintType)
struct LILALINKRUNTIME_API FLilaZoneInfo
{
	GENERATED_BODY()

	/** Stable id within the layer. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Lila")
	FName Id;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Lila")
	FString Name;

	/** Layer priority * 1000 + the zone's priority within the layer; higher wins. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Lila")
	int32 Priority = 0;

	/** Attributes, with the layer's defaults filled in, as text (numbers, true/false, lists as JSON). */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Lila")
	TMap<FName, FString> Attributes;

	/** The same attributes as one JSON object. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Lila")
	FString AttributesJson;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Lila")
	TArray<FLilaZoneRing> Outline;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Lila")
	TArray<FLilaZoneRing> Holes;

	/** World Z range the zone counts in. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Lila")
	float Bottom = 0.0f;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Lila")
	float Top = 0.0f;

	/** Painted zones: the mask; the outline is its traced edge. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Lila")
	FLilaZoneMask Mask;

	/** 1 inside the outline, else the mask's value, 0 outside the Z range. */
	float WeightAt(const FVector& Location) const;
};

/** A Lila zone layer: districts, jurisdiction, territory, spawn sets, densities, wanted response, interiors, missions or a user type. */
USTRUCT(BlueprintType)
struct LILALINKRUNTIME_API FLilaZoneLayer
{
	GENERATED_BODY()

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Lila")
	FName Id;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Lila")
	FName Type;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Lila")
	FString Label;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Lila")
	int32 Priority = 0;

	/** Painted weight scales the zone's density attribute. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Lila")
	bool bDensity = false;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Lila")
	bool bExclusive = false;

	/** The World Partition data layer its volumes are in. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Lila")
	FString DataLayer;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Lila")
	FLinearColor Color = FLinearColor::Gray;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Lila")
	TArray<FLilaZoneInfo> Zones;
};

/** What a layer says at a point. */
USTRUCT(BlueprintType)
struct LILALINKRUNTIME_API FLilaZoneHit
{
	GENERATED_BODY()

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Lila")
	FName Layer;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Lila")
	FName LayerType;

	/** None when no zone of the layer holds the point. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Lila")
	FName Zone;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Lila")
	FString ZoneName;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Lila")
	TMap<FName, FString> Attributes;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Lila")
	float Weight = 0.0f;

	/** Density layers: the zone's density attribute times the weight. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Lila")
	float Density = 0.0f;
};

/**
 * Every zone layer of a Lila zone set, for runtime lookups without the volumes: which district, whose territory, which
 * spawn set, how dense the traffic is at a point. LilaLink rewrites it on every zone send.
 */
UCLASS(BlueprintType)
class LILALINKRUNTIME_API ULilaZoneData : public UDataAsset
{
	GENERATED_BODY()

public:
	/** The zone set's name in Lila. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Lila")
	FString SetName;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Lila")
	TArray<FLilaZoneLayer> Layers;

	/** The layer's winning zone at a location: highest priority, then the later zone. False when none holds it. */
	UFUNCTION(BlueprintPure, Category = "Lila")
	bool QueryLayer(FName Layer, const FVector& Location, FLilaZoneHit& Hit) const;

	/** Every layer's answer at a location, one hit per layer (Zone is None where no zone holds it). */
	UFUNCTION(BlueprintPure, Category = "Lila")
	TArray<FLilaZoneHit> Query(const FVector& Location) const;
};

/**
 * A Lila zone as a volume: the brush is the zone's outline extruded from below the ground to above it, and the zone's
 * layer, name, priority and attributes ride on it, so overlap events and volume queries read gameplay data straight off
 * it. LilaLink rewrites it on every zone send.
 */
UCLASS(BlueprintType, ClassGroup = (Lila))
class LILALINKRUNTIME_API ALilaZoneVolume : public AVolume
{
	GENERATED_BODY()

public:
	ALilaZoneVolume(const FObjectInitializer& ObjectInitializer);

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Lila")
	FName ZoneId;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Lila")
	FName LayerId;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Lila")
	FName LayerType;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Lila")
	FString ZoneName;

	/** Layer priority * 1000 + the zone's priority; higher wins where volumes overlap. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Lila")
	int32 Priority = 0;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Lila")
	TMap<FName, FString> Attributes;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Lila")
	FString AttributesJson;

	/** The zone data asset this volume came with, for the zone's mask and the other layers. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Lila")
	TSoftObjectPtr<ULilaZoneData> ZoneData;

	/** An attribute as text; Fallback when the zone has none by that name. */
	UFUNCTION(BlueprintPure, Category = "Lila")
	FString GetAttribute(FName Key, const FString& Fallback) const;
};
