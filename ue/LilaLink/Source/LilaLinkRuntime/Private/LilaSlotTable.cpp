#include "LilaSlotTable.h"

#include "Engine/Engine.h"
#include "Engine/World.h"
#include "EngineUtils.h"
#include "GameFramework/Actor.h"

namespace
{
bool TypeCovers(const FString& Pattern, const FString& Type)
{
	return Type == Pattern || Type.StartsWith(Pattern + TEXT("."));
}
} // namespace

void ULilaSlotTable::PostLoad()
{
	Super::PostLoad();
	RebuildIndex();
}

void ULilaSlotTable::RebuildIndex()
{
	Index.Reset();
	for (int32 K = 0; K < Rows.Num(); ++K)
	{
		Index.Add(Rows[K].Id, K);
	}
}

bool ULilaSlotTable::FindSlot(const FString& Id, FLilaSlotRow& OutRow) const
{
	if (const int32* K = Index.Find(Id))
	{
		OutRow = Rows[*K];
		return true;
	}
	for (const FLilaSlotRow& Row : Rows)
	{
		if (Row.Id == Id)
		{
			OutRow = Row;
			return true;
		}
	}
	return false;
}

TArray<FLilaSlotRow> ULilaSlotTable::FindByType(const FString& Type) const
{
	return Rows.FilterByPredicate([&Type](const FLilaSlotRow& Row) { return TypeCovers(Type, Row.Type); });
}

TArray<FLilaSlotRow> ULilaSlotTable::FindByOwner(const FString& Owner) const
{
	return Rows.FilterByPredicate([&Owner](const FLilaSlotRow& Row) { return Row.Owner == Owner || Row.Owner.StartsWith(Owner + TEXT("/")); });
}

AActor* ULilaSlotTable::FindSlotActor(const UObject* WorldContext, const FString& Id)
{
	UWorld* World = GEngine ? GEngine->GetWorldFromContextObject(WorldContext, EGetWorldErrorMode::ReturnNull) : nullptr;
	if (!World)
	{
		return nullptr;
	}
	const FName Tag(*(TEXT("LilaSlot:") + Id));
	for (TActorIterator<AActor> It(World); It; ++It)
	{
		if (It->Tags.Contains(Tag))
		{
			return *It;
		}
	}
	return nullptr;
}
