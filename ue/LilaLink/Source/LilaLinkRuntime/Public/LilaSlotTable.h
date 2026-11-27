#pragma once

#include "CoreMinimal.h"
#include "Engine/DataAsset.h"

#include "LilaSlotTable.generated.h"

class AActor;
class UWorld;

/** One Lila slot as sent: its stable id, type, owner, what fills it and where. */
USTRUCT(BlueprintType)
struct LILALINKRUNTIME_API FLilaSlotRow
{
	GENERATED_BODY()

	/** Stable across regenerating in Lila: owner id plus a local path ("settlement:Town/street/1a2b/lamp/3c"). */
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Lila")
	FString Id;

	/** Dotted type, most general first ("door.shop"). */
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Lila")
	FString Type;

	/** The building, lot, street or arrangement it belongs to. */
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Lila")
	FString Owner;

	/** The placement group holding it (its actors carry the tag LilaGroup:<Group>). */
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Lila")
	FString Group;

	/** The actor class spawned for it, when its payload names one. */
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Lila")
	FString ActorClass;

	/** The static mesh it was given, when its payload names one. */
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Lila")
	FString Mesh;

	/** The Lila library item it shows, when it has one. */
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Lila")
	FString Item;

	/** Gameplay data from Lila (locked, shopType, missionId), values as text. */
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Lila")
	TMap<FString, FString> Data;

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Lila")
	FTransform Transform;
};

/**
 * Every slot LilaLink placed, rewritten on each send, so game code can find "door 1234" and what it carries without
 * walking the level. Slots resolving to actors also carry the tags LilaSlot:<id>, LilaSlotType:<type> and
 * LilaSlotData:<key>=<value>; instanced ones are in their holder's ULilaInstanceData (columns slotType and slot.<key>).
 */
UCLASS(BlueprintType)
class LILALINKRUNTIME_API ULilaSlotTable : public UDataAsset
{
	GENERATED_BODY()

public:
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Lila")
	TArray<FLilaSlotRow> Rows;

	/** The row with the id; false when there is none. */
	UFUNCTION(BlueprintPure, Category = "Lila")
	bool FindSlot(const FString& Id, FLilaSlotRow& OutRow) const;

	/** Rows whose type is Type or under it ("door" finds "door.shop"). */
	UFUNCTION(BlueprintPure, Category = "Lila")
	TArray<FLilaSlotRow> FindByType(const FString& Type) const;

	/** Rows of an owner (and the owners under it). */
	UFUNCTION(BlueprintPure, Category = "Lila")
	TArray<FLilaSlotRow> FindByOwner(const FString& Owner) const;

	/** The actor placed for a slot (tagged LilaSlot:<id>), or null for an instanced slot or one not in the world. */
	UFUNCTION(BlueprintCallable, Category = "Lila", meta = (WorldContext = "WorldContext"))
	static AActor* FindSlotActor(const UObject* WorldContext, const FString& Id);

	/** Rebuilds the id lookup after Rows change. */
	void RebuildIndex();

	virtual void PostLoad() override;

private:
	TMap<FString, int32> Index;
};
