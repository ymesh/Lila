#include "LilaLinkWaterRoutes.h"

#include "LilaLinkLog.h"
#include "LilaWaterRoute.h"

#include "Components/SplineComponent.h"
#include "Editor.h"
#include "Engine/World.h"
#include "EngineUtils.h"
#include "Misc/Crc.h"
#include "ProfilingDebugging/CpuProfilerTrace.h"
#include "ScopedTransaction.h"

namespace
{
/** On every actor this file makes, so traffic and AI setups can find them by tag. */
const FName RouteTag(TEXT("LilaWaterRoute"));

FName RoutesTerrainTag(const FString& Id)
{
	return FName(*FString::Printf(TEXT("LilaLink.Routes:%08X"), FCrc::StrCrc32(*Id)));
}

FName LaneTag(const FString& Lane)
{
	return FName(*(TEXT("LilaLink.Lane:") + Lane.Right(900)));
}

FString LaneOf(const AActor* Actor)
{
	const FString Prefix = TEXT("LilaLink.Lane:");
	for (const FName& Tag : Actor->Tags)
	{
		const FString S = Tag.ToString();
		if (S.StartsWith(Prefix, ESearchCase::IgnoreCase))
		{
			return S.RightChop(Prefix.Len());
		}
	}
	return FString();
}

UWorld* RoutesEditorWorld()
{
	return GEditor ? GEditor->GetEditorWorldContext().World() : nullptr;
}

bool ReadRouteVector(const TSharedPtr<FJsonObject>& Object, const TCHAR* Field, FVector& Out)
{
	const TArray<TSharedPtr<FJsonValue>>* Values = nullptr;
	if (!Object->TryGetArrayField(Field, Values) || Values->Num() != 3)
	{
		return false;
	}
	Out = FVector((*Values)[0]->AsNumber(), (*Values)[1]->AsNumber(), (*Values)[2]->AsNumber());
	return !Out.ContainsNaN();
}

float RouteNumber(const TSharedPtr<FJsonObject>& Object, const TCHAR* Field, double Fallback = 0.0)
{
	double V = Fallback;
	Object->TryGetNumberField(Field, V);
	return float(V);
}

/** Profile width per craft from laneProfiles: [{name, lanes: [{width}]}]. */
TMap<FString, float> ProfileWidths(const TSharedPtr<FJsonObject>& Params)
{
	TMap<FString, float> Out;
	const TArray<TSharedPtr<FJsonValue>>* Profiles = nullptr;
	if (!Params->TryGetArrayField(TEXT("laneProfiles"), Profiles))
	{
		return Out;
	}
	for (const TSharedPtr<FJsonValue>& Value : *Profiles)
	{
		const TSharedPtr<FJsonObject>* Profile = nullptr;
		const TArray<TSharedPtr<FJsonValue>>* Lanes = nullptr;
		FString Name;
		if (Value.IsValid() && Value->TryGetObject(Profile) && (*Profile)->TryGetStringField(TEXT("name"), Name) && (*Profile)->TryGetArrayField(TEXT("lanes"), Lanes) &&
			!Lanes->IsEmpty() && (*Lanes)[0]->Type == EJson::Object)
		{
			Out.Add(Name, RouteNumber((*Lanes)[0]->AsObject(), TEXT("width")));
		}
	}
	return Out;
}

TSharedPtr<FJsonValue> SetWaterRoutes(const TSharedPtr<FJsonObject>& Params, FString& OutError)
{
	TRACE_CPUPROFILER_EVENT_SCOPE(LilaLinkWaterRoutes::SetWaterRoutes);

	FString Id;
	if (!Params->TryGetStringField(TEXT("id"), Id) || Id.IsEmpty())
	{
		OutError = TEXT("setWaterRoutes: give id");
		return nullptr;
	}
	UWorld* World = RoutesEditorWorld();
	if (!World)
	{
		OutError = TEXT("setWaterRoutes: no editor world");
		return nullptr;
	}
	FString Label = TEXT("Terrain");
	Params->TryGetStringField(TEXT("label"), Label);
	FString Folder = TEXT("Lila/Routes/") + Label;
	Params->TryGetStringField(TEXT("folder"), Folder);
	const FName Terrain = RoutesTerrainTag(Id);
	const TMap<FString, float> Widths = ProfileWidths(Params);

	TMap<FString, AActor*> Existing;
	for (TActorIterator<AActor> It(World); It; ++It)
	{
		if (It->ActorHasTag(Terrain))
		{
			Existing.Add(LaneOf(*It), *It);
		}
	}

	const TArray<TSharedPtr<FJsonValue>>* Lanes = nullptr;
	if (!Params->TryGetArrayField(TEXT("lanes"), Lanes))
	{
		OutError = TEXT("setWaterRoutes: give lanes");
		return nullptr;
	}

	FScopedTransaction Transaction(NSLOCTEXT("LilaLink", "SetWaterRoutes", "Lila: Update Water Routes"));
	TSet<FString> Sent;
	int32 Created = 0, Updated = 0;
	for (const TSharedPtr<FJsonValue>& Value : *Lanes)
	{
		const TSharedPtr<FJsonObject>* LanePtr = nullptr;
		if (!Value.IsValid() || !Value->TryGetObject(LanePtr))
		{
			OutError = TEXT("setWaterRoutes: a lane is not an object");
			Transaction.Cancel();
			return nullptr;
		}
		const TSharedPtr<FJsonObject>& Lane = *LanePtr;
		FString LaneId, Kind, Craft, From, To;
		Lane->TryGetStringField(TEXT("id"), LaneId);
		Lane->TryGetStringField(TEXT("kind"), Kind);
		Lane->TryGetStringField(TEXT("craft"), Craft);
		Lane->TryGetStringField(TEXT("from"), From);
		Lane->TryGetStringField(TEXT("to"), To);
		bool bAir = false, bMain = false;
		Lane->TryGetBoolField(TEXT("air"), bAir);
		Lane->TryGetBoolField(TEXT("main"), bMain);
		const TArray<TSharedPtr<FJsonValue>>* Points = nullptr;
		if (LaneId.IsEmpty() || Sent.Contains(LaneId) || !Lane->TryGetArrayField(TEXT("points"), Points) || Points->Num() < 2)
		{
			OutError = FString::Printf(TEXT("setWaterRoutes: lane %s needs its own id and at least two points"), *LaneId);
			Transaction.Cancel();
			return nullptr;
		}
		Sent.Add(LaneId);

		FVector Location = FVector::ZeroVector;
		if ((*Points)[0]->Type == EJson::Object)
		{
			ReadRouteVector((*Points)[0]->AsObject(), TEXT("position"), Location);
		}
		AActor** Found = Existing.Find(LaneId);
		AActor* Actor = Found ? *Found : nullptr;
		if (Actor && !Cast<USplineComponent>(Actor->GetRootComponent()))
		{
			World->EditorDestroyActor(Actor, true);
			Actor = nullptr;
		}
		if (!Actor)
		{
			FActorSpawnParameters Spawn;
			Spawn.ObjectFlags |= RF_Transactional;
			Actor = World->SpawnActor<AActor>(AActor::StaticClass(), FTransform(Location), Spawn);
			if (!Actor)
			{
				OutError = TEXT("setWaterRoutes: could not spawn an actor");
				Transaction.Cancel();
				return nullptr;
			}
			USplineComponent* Root = NewObject<USplineComponent>(Actor, TEXT("LilaRouteSpline"), RF_Transactional);
			Root->SetMobility(EComponentMobility::Static);
			Actor->SetRootComponent(Root);
			Actor->AddInstanceComponent(Root);
			Root->RegisterComponent();
			Actor->Tags.Add(RouteTag);
			Actor->Tags.Add(Terrain);
			Actor->Tags.Add(LaneTag(LaneId));
			++Created;
		}
		else
		{
			Actor->Modify();
			++Updated;
		}
		Actor->SetActorLocation(Location);
		Actor->SetActorLabel(Label + TEXT("_") + LaneId);
		Actor->SetFolderPath(FName(*Folder));
		Actor->Tags.RemoveAll([](const FName& Tag) { return Tag.ToString().StartsWith(TEXT("LilaRoute."), ESearchCase::IgnoreCase); });
		Actor->Tags.Add(FName(*(TEXT("LilaRoute.") + Kind)));
		Actor->Tags.Add(FName(*(TEXT("LilaRoute.") + Craft)));

		ULilaWaterRoute* Route = ULilaWaterRoute::FindOn(Actor);
		if (Route)
		{
			Route->Modify();
		}
		else
		{
			Route = NewObject<ULilaWaterRoute>(Actor, TEXT("LilaWaterRoute"), RF_Transactional);
			Actor->AddInstanceComponent(Route);
			Route->RegisterComponent();
		}
		Route->LaneId = LaneId;
		Route->Kind = FName(*Kind);
		Route->Craft = FName(*Craft);
		Route->bAir = bAir;
		Route->bMainNetwork = bMain;
		Route->FromNode = From;
		Route->ToNode = To;
		const float* Width = Widths.Find(Craft);
		Route->ProfileWidth = Width ? *Width : 0.0f;

		USplineComponent* Spline = Cast<USplineComponent>(Actor->GetRootComponent());
		Spline->Modify();
		Spline->ClearSplinePoints(false);
		Route->Points.Reset();
		for (const TSharedPtr<FJsonValue>& PointValue : *Points)
		{
			const TSharedPtr<FJsonObject>* Point = nullptr;
			FVector Position;
			if (!PointValue.IsValid() || !PointValue->TryGetObject(Point) || !ReadRouteVector(*Point, TEXT("position"), Position))
			{
				OutError = FString::Printf(TEXT("setWaterRoutes: a point of %s has no position"), *LaneId);
				Transaction.Cancel();
				return nullptr;
			}
			Spline->AddSplinePoint(Position - Location, ESplineCoordinateSpace::Local, false);
			FLilaRoutePoint& P = Route->Points.AddDefaulted_GetRef();
			P.Width = RouteNumber(*Point, TEXT("width"));
			P.Depth = RouteNumber(*Point, TEXT("depth"));
			P.Clearance = RouteNumber(*Point, TEXT("clearance"));
		}
		Spline->UpdateSpline();
		Actor->MarkPackageDirty();
	}

	int32 Removed = 0;
	for (const auto& [Key, Actor] : Existing)
	{
		if (!Sent.Contains(Key) && IsValid(Actor))
		{
			Removed += World->EditorDestroyActor(Actor, true) ? 1 : 0;
		}
	}
	UE_LOG(LogLilaLink, Log, TEXT("setWaterRoutes: %s (%d created, %d updated, %d removed)"), *Label, Created, Updated, Removed);
	TSharedRef<FJsonObject> Out = MakeShared<FJsonObject>();
	Out->SetNumberField(TEXT("actors"), Sent.Num());
	Out->SetNumberField(TEXT("created"), Created);
	Out->SetNumberField(TEXT("updated"), Updated);
	Out->SetNumberField(TEXT("removed"), Removed);
	return MakeShared<FJsonValueObject>(Out);
}

TSharedPtr<FJsonValue> RemoveWaterRoutes(const TSharedPtr<FJsonObject>& Params, FString& OutError)
{
	TRACE_CPUPROFILER_EVENT_SCOPE(LilaLinkWaterRoutes::RemoveWaterRoutes);

	FString Id;
	if (!Params->TryGetStringField(TEXT("id"), Id) || Id.IsEmpty())
	{
		OutError = TEXT("removeWaterRoutes: give id");
		return nullptr;
	}
	UWorld* World = RoutesEditorWorld();
	if (!World)
	{
		OutError = TEXT("removeWaterRoutes: no editor world");
		return nullptr;
	}
	FScopedTransaction Transaction(NSLOCTEXT("LilaLink", "RemoveWaterRoutes", "Lila: Remove Water Routes"));
	const FName Terrain = RoutesTerrainTag(Id);
	TArray<AActor*> Ours;
	for (TActorIterator<AActor> It(World); It; ++It)
	{
		if (It->ActorHasTag(Terrain))
		{
			Ours.Add(*It);
		}
	}
	int32 Removed = 0;
	for (AActor* Actor : Ours)
	{
		Removed += World->EditorDestroyActor(Actor, true) ? 1 : 0;
	}
	TSharedRef<FJsonObject> Out = MakeShared<FJsonObject>();
	Out->SetNumberField(TEXT("removed"), Removed);
	return MakeShared<FJsonValueObject>(Out);
}
} // namespace

void LilaLink::AddWaterRouteMethods(TMap<FString, FLilaLinkMethod>& Methods)
{
	Methods.Add(TEXT("setWaterRoutes"), LilaLink::Sync(&SetWaterRoutes));
	Methods.Add(TEXT("removeWaterRoutes"), LilaLink::Sync(&RemoveWaterRoutes));
}
