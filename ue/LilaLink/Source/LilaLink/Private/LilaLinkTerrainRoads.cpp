#include "LilaLinkTerrainRoads.h"

#include "LilaLinkLog.h"
#include "LilaLinkSplines.h"

#include "Editor.h"
#include "Engine/StaticMesh.h"
#include "Engine/World.h"
#include "EngineUtils.h"
#include "Landscape.h"
#include "LandscapeEditLayer.h"
#include "LandscapeInfo.h"
#include "LandscapeSplineActor.h"
#include "LandscapeSplineControlPoint.h"
#include "LandscapeSplineSegment.h"
#include "LandscapeSplinesComponent.h"
#include "Misc/Crc.h"
#include "Misc/PackageName.h"
#include "ProfilingDebugging/CpuProfilerTrace.h"
#include "ScopedTransaction.h"
#include "WorldPartition/WorldPartition.h"

namespace
{
const FName RoadLinkTag(TEXT("LilaLink"));

struct FLilaRoadPoint
{
	FVector Position = FVector::ZeroVector;
	FVector ArriveTangent = FVector::ZeroVector;
	FVector LeaveTangent = FVector::ZeroVector;
	FVector Up = FVector::UpVector;
};

struct FLilaRoadSpan
{
	double From = 0.0;
	double To = 0.0;
};

struct FLilaRoad
{
	FString Id;
	double HalfWidth = 300.0;
	double SideFalloff = 500.0;
	double EndFalloff = 500.0;
	bool bRaise = false;
	bool bLower = false;
	FName Layer;
	TArray<FLilaRoadPoint> Points;
	FString MeshAsset;
	FString MeshAxis;
	bool bScaleToWidth = true;
	TArray<FLilaRoadSpan> Spans;
	/** A roundabout's ring: a last segment joins the last point back to the first. */
	bool bClosed = false;
};

/** The spline actor Lila's roads for a terrain live on in a partitioned world. */
FName RoadActorTag(const FString& Id)
{
	return FName(*(TEXT("LilaLink.TerrainRoads:") + Id.Right(900)));
}

/** The tag setLandscape gives the landscape it makes for a terrain. */
FName RoadLandscapeTag(const FString& Id)
{
	return FName(*(TEXT("LilaLink.Landscape:") + Id.Right(900)));
}

/** Names of the control points and segments made for a terrain's roads, so a re-send finds and replaces only those. */
FString RoadPrefix(const FString& Id)
{
	return FString::Printf(TEXT("LilaRoad_%08X_"), FCrc::StrCrc32(*Id));
}

bool RoadReadVector(const TSharedPtr<FJsonObject>& Object, const TCHAR* Field, FVector& Out)
{
	const TArray<TSharedPtr<FJsonValue>>* Values = nullptr;
	if (!Object->TryGetArrayField(Field, Values) || Values->Num() != 3)
	{
		return false;
	}
	Out = FVector((*Values)[0]->AsNumber(), (*Values)[1]->AsNumber(), (*Values)[2]->AsNumber());
	return !Out.ContainsNaN();
}

bool ParseRoads(const TSharedPtr<FJsonObject>& Params, TArray<FLilaRoad>& Out, FString& OutError)
{
	const TArray<TSharedPtr<FJsonValue>>* Roads = nullptr;
	if (!Params->TryGetArrayField(TEXT("roads"), Roads))
	{
		OutError = TEXT("setTerrainRoads: give roads");
		return false;
	}
	for (const TSharedPtr<FJsonValue>& Value : *Roads)
	{
		const TSharedPtr<FJsonObject>* Object = nullptr;
		const TArray<TSharedPtr<FJsonValue>>* Points = nullptr;
		if (!Value.IsValid() || !Value->TryGetObject(Object) || !(*Object)->TryGetArrayField(TEXT("points"), Points) || Points->Num() < 2)
		{
			OutError = TEXT("setTerrainRoads: each road needs at least two points");
			return false;
		}
		FLilaRoad& Road = Out.AddDefaulted_GetRef();
		(*Object)->TryGetStringField(TEXT("id"), Road.Id);
		(*Object)->TryGetNumberField(TEXT("halfWidth"), Road.HalfWidth);
		(*Object)->TryGetNumberField(TEXT("sideFalloff"), Road.SideFalloff);
		(*Object)->TryGetNumberField(TEXT("endFalloff"), Road.EndFalloff);
		(*Object)->TryGetBoolField(TEXT("raiseTerrain"), Road.bRaise);
		(*Object)->TryGetBoolField(TEXT("lowerTerrain"), Road.bLower);
		(*Object)->TryGetBoolField(TEXT("closed"), Road.bClosed);
		FString Layer;
		if ((*Object)->TryGetStringField(TEXT("layer"), Layer) && !Layer.IsEmpty())
		{
			Road.Layer = FName(*Layer);
		}
		const TSharedPtr<FJsonObject>* Mesh = nullptr;
		if ((*Object)->TryGetObjectField(TEXT("mesh"), Mesh))
		{
			(*Mesh)->TryGetStringField(TEXT("asset"), Road.MeshAsset);
			(*Mesh)->TryGetStringField(TEXT("forwardAxis"), Road.MeshAxis);
			(*Mesh)->TryGetBoolField(TEXT("scaleToWidth"), Road.bScaleToWidth);
		}
		const TArray<TSharedPtr<FJsonValue>>* Spans = nullptr;
		if ((*Object)->TryGetArrayField(TEXT("spans"), Spans))
		{
			for (const TSharedPtr<FJsonValue>& SpanValue : *Spans)
			{
				const TSharedPtr<FJsonObject>* Span = nullptr;
				if (SpanValue.IsValid() && SpanValue->TryGetObject(Span))
				{
					FLilaRoadSpan& S = Road.Spans.AddDefaulted_GetRef();
					(*Span)->TryGetNumberField(TEXT("from"), S.From);
					(*Span)->TryGetNumberField(TEXT("to"), S.To);
				}
			}
		}
		for (const TSharedPtr<FJsonValue>& PointValue : *Points)
		{
			const TSharedPtr<FJsonObject>* PointObject = nullptr;
			FLilaRoadPoint& Point = Road.Points.AddDefaulted_GetRef();
			if (!PointValue.IsValid() || !PointValue->TryGetObject(PointObject) || !RoadReadVector(*PointObject, TEXT("position"), Point.Position))
			{
				OutError = TEXT("setTerrainRoads: a point has no position");
				return false;
			}
			RoadReadVector(*PointObject, TEXT("arriveTangent"), Point.ArriveTangent);
			RoadReadVector(*PointObject, TEXT("leaveTangent"), Point.LeaveTangent);
			RoadReadVector(*PointObject, TEXT("up"), Point.Up);
		}
	}
	return true;
}

UWorld* RoadEditorWorld()
{
	return GEditor ? GEditor->GetEditorWorldContext().World() : nullptr;
}

AActor* FindRoadActor(UWorld* World, FName Tag)
{
	for (TActorIterator<AActor> It(World); It; ++It)
	{
		if (It->ActorHasTag(Tag))
		{
			return *It;
		}
	}
	return nullptr;
}

/** The landscape setLandscape made for the terrain, else the one labelled Label, else the only one. */
ALandscape* FindRoadLandscape(UWorld* World, const FString& Id, const FString& Label)
{
	const FName Tag = RoadLandscapeTag(Id);
	ALandscape* Labelled = nullptr;
	ALandscape* First = nullptr;
	int32 Count = 0;
	for (TActorIterator<ALandscape> It(World); It; ++It)
	{
		if (It->ActorHasTag(Tag))
		{
			return *It;
		}
		if (!Label.IsEmpty() && (It->GetActorLabel() == Label || It->GetName() == Label))
		{
			Labelled = *It;
		}
		First = First ? First : *It;
		++Count;
	}
	return Labelled ? Labelled : (Count == 1 ? First : nullptr);
}

ESplineMeshAxis::Type RoadMeshAxis(const FString& Name)
{
	return Name.Equals(TEXT("y"), ESearchCase::IgnoreCase) ? ESplineMeshAxis::Y
		: Name.Equals(TEXT("z"), ESearchCase::IgnoreCase) ? ESplineMeshAxis::Z
		: ESplineMeshAxis::X;
}

/** Deletes the control points and segments a previous send made for these roads. */
void ClearRoads(ULandscapeSplinesComponent* Splines, const FString& Prefix)
{
	TRACE_CPUPROFILER_EVENT_SCOPE(LilaLinkTerrainRoads::ClearRoads);

	Splines->Modify();
	const TArray<TObjectPtr<ULandscapeSplineSegment>> Segments = Splines->GetSegments();
	for (ULandscapeSplineSegment* Segment : Segments)
	{
		if (!Segment || !Segment->GetName().StartsWith(Prefix))
		{
			continue;
		}
		Segment->Modify();
		Segment->DeleteSplinePoints();
		for (int32 End = 0; End < 2; ++End)
		{
			if (ULandscapeSplineControlPoint* Point = Segment->Connections[End].ControlPoint)
			{
				Point->Modify();
				Point->ConnectedSegments.Remove(FLandscapeSplineConnection(Segment, End));
			}
		}
		Splines->GetSegments().Remove(Segment);
	}
	const TArray<TObjectPtr<ULandscapeSplineControlPoint>> Points = Splines->GetControlPoints();
	for (ULandscapeSplineControlPoint* Point : Points)
	{
		if (Point && Point->GetName().StartsWith(Prefix) && Point->ConnectedSegments.IsEmpty())
		{
			Point->Modify();
			Point->DeleteSplinePoints();
			Splines->GetControlPoints().Remove(Point);
		}
	}
}

/** The splines component the roads go on: a tagged landscape spline actor in a partitioned world, else the landscape's own. */
ULandscapeSplinesComponent* RoadSplines(UWorld* World, ALandscape* Landscape, const FString& Id, const FVector& Location, FString& OwnerPath, FString& OutError)
{
	if (World->GetWorldPartition())
	{
		const FName Tag = RoadActorTag(Id);
		ALandscapeSplineActor* SplineActor = Cast<ALandscapeSplineActor>(FindRoadActor(World, Tag));
		if (!SplineActor)
		{
			ULandscapeInfo* Info = Landscape->GetLandscapeInfo();
			SplineActor = Info ? Info->CreateSplineActor(Location) : nullptr;
			if (!SplineActor)
			{
				OutError = TEXT("setTerrainRoads: could not make a landscape spline actor");
				return nullptr;
			}
			SplineActor->Tags.Add(Tag);
			SplineActor->Tags.Add(RoadLinkTag);
			SplineActor->SetActorLabel(TEXT("LilaRoads"));
		}
		SplineActor->Modify();
		OwnerPath = SplineActor->GetPathName();
		return SplineActor->GetSplinesComponent();
	}
	Landscape->Modify();
	if (!Landscape->GetSplinesComponent())
	{
		Landscape->CreateSplineComponent();
	}
	OwnerPath = Landscape->GetPathName();
	return Landscape->GetSplinesComponent();
}

TSharedPtr<FJsonValue> SetTerrainRoads(const TSharedPtr<FJsonObject>& Params, FString& OutError)
{
	TRACE_CPUPROFILER_EVENT_SCOPE(LilaLinkTerrainRoads::SetTerrainRoads);

	FString Id, LandscapeLabel;
	if (!Params->TryGetStringField(TEXT("id"), Id) || Id.IsEmpty())
	{
		OutError = TEXT("setTerrainRoads: give id");
		return nullptr;
	}
	Params->TryGetStringField(TEXT("landscape"), LandscapeLabel);
	UWorld* World = RoadEditorWorld();
	if (!World)
	{
		OutError = TEXT("setTerrainRoads: no editor world");
		return nullptr;
	}
	TArray<FLilaRoad> Roads;
	if (!ParseRoads(Params, Roads, OutError))
	{
		return nullptr;
	}
	FVector Location = FVector::ZeroVector;
	RoadReadVector(Params, TEXT("location"), Location);
	ALandscape* Landscape = FindRoadLandscape(World, Id, LandscapeLabel);
	if (!Landscape)
	{
		OutError = FString::Printf(TEXT("setTerrainRoads: no landscape for this terrain%s; send the terrain first"),
			LandscapeLabel.IsEmpty() ? TEXT("") : *FString::Printf(TEXT(" (looked for %s)"), *LandscapeLabel));
		return nullptr;
	}

	FScopedTransaction Transaction(NSLOCTEXT("LilaLink", "SetTerrainRoads", "Lila: Update Terrain Roads"));
	const FString Prefix = RoadPrefix(Id);
	int32 RegionsLoaded = 0;
	if (World->GetWorldPartition())
	{
		// Only loaded landscape takes a redraw: load what the earlier and the new roads cover, before the Splines layer is
		// added, so every region gets that layer too.
		LilaLink::LoadSplineActors(World);
		const ALandscapeSplineActor* Earlier = Cast<ALandscapeSplineActor>(FindRoadActor(World, RoadActorTag(Id)));
		FBox Covered = Earlier ? LilaLink::SplineBounds(Earlier->GetSplinesComponent(), Prefix) : FBox(ForceInit);
		for (const FLilaRoad& Road : Roads)
		{
			for (const FLilaRoadPoint& In : Road.Points)
			{
				Covered += FBox::BuildAABB(Location + In.Position, FVector(Road.HalfWidth + Road.SideFalloff + Road.EndFalloff));
			}
		}
		RegionsLoaded = LilaLink::LoadLandscapeUnder(World, Covered);
	}
	FString OwnerPath;
	ULandscapeSplinesComponent* Component = RoadSplines(World, Landscape, Id, Location, OwnerPath, OutError);
	if (!Component)
	{
		if (OutError.IsEmpty())
		{
			OutError = TEXT("setTerrainRoads: the landscape has no splines component");
		}
		Transaction.Cancel();
		return nullptr;
	}
	ClearRoads(Component, Prefix);

	TArray<FString> Warnings;
	const FTransform ToComponent = Component->GetComponentTransform();
	const double WidthScale = FMath::Max(KINDA_SMALL_NUMBER, ToComponent.GetScale3D().X);
	int32 PointCount = 0, SegmentCount = 0;
	bool bDeforms = false;
	for (int32 RoadIndex = 0; RoadIndex < Roads.Num(); ++RoadIndex)
	{
		const FLilaRoad& Road = Roads[RoadIndex];
		UStaticMesh* Mesh = nullptr;
		if (!Road.MeshAsset.IsEmpty())
		{
			const FString ObjectPath = Road.MeshAsset.Contains(TEXT(".")) ? Road.MeshAsset : Road.MeshAsset + TEXT(".") + FPackageName::GetShortName(Road.MeshAsset);
			Mesh = LoadObject<UStaticMesh>(nullptr, *ObjectPath);
			if (!Mesh)
			{
				Warnings.Add(FString::Printf(TEXT("%s: no static mesh at %s; laid without segment meshes"), *Road.Id, *Road.MeshAsset));
			}
		}
		TArray<ULandscapeSplineControlPoint*> Points;
		TArray<double> Along;
		double Distance = 0.0;
		for (int32 Index = 0; Index < Road.Points.Num(); ++Index)
		{
			const FLilaRoadPoint& In = Road.Points[Index];
			if (Index > 0)
			{
				Distance += FVector::Dist(In.Position, Road.Points[Index - 1].Position);
			}
			Along.Add(Distance);
			const FName Name(*FString::Printf(TEXT("%sR%d_P%d"), *Prefix, RoadIndex, Index));
			ULandscapeSplineControlPoint* Point = NewObject<ULandscapeSplineControlPoint>(Component, Name, RF_Transactional);
			Component->GetControlPoints().Add(Point);
			Point->Location = ToComponent.InverseTransformPosition(Location + In.Position);
			const FVector Forward = ToComponent.InverseTransformVectorNoScale(In.LeaveTangent.IsNearlyZero() ? In.ArriveTangent : In.LeaveTangent);
			const FVector Up = ToComponent.InverseTransformVectorNoScale(In.Up);
			Point->Rotation = FRotationMatrix::MakeFromXZ(Forward.GetSafeNormal(), Up).Rotator();
			Point->Width = static_cast<float>(Road.HalfWidth / WidthScale);
			Point->SideFalloff = static_cast<float>(Road.SideFalloff / WidthScale);
			Point->EndFalloff = static_cast<float>(Road.EndFalloff / WidthScale);
			Point->LayerName = Road.Layer;
			Point->bRaiseTerrain = Road.bRaise;
			Point->bLowerTerrain = Road.bLower;
			Points.Add(Point);
		}
		const int32 SegmentsInRoad = Road.bClosed && Points.Num() > 2 ? Points.Num() : Points.Num() - 1;
		for (int32 Index = 0; Index < SegmentsInRoad; ++Index)
		{
			const int32 Next = (Index + 1) % Points.Num();
			const FName Name(*FString::Printf(TEXT("%sR%d_S%d"), *Prefix, RoadIndex, Index));
			ULandscapeSplineSegment* Segment = NewObject<ULandscapeSplineSegment>(Component, Name, RF_Transactional);
			Component->GetSegments().Add(Segment);
			Segment->Connections[0].ControlPoint = Points[Index];
			Segment->Connections[1].ControlPoint = Points[Next];
			Segment->Connections[0].TangentLen = static_cast<float>(ToComponent.InverseTransformVector(Road.Points[Index].LeaveTangent).Size());
			Segment->Connections[1].TangentLen = -static_cast<float>(ToComponent.InverseTransformVector(Road.Points[Next].ArriveTangent).Size());
			// Bridges and tunnels leave the landscape alone under or over them.
			const double Middle = Next > Index ? 0.5 * (Along[Index] + Along[Next]) : Along[Index];
			bool bInSpan = false;
			for (const FLilaRoadSpan& Span : Road.Spans)
			{
				bInSpan |= Middle >= Span.From && Middle <= Span.To;
			}
			Segment->LayerName = bInSpan ? NAME_None : Road.Layer;
			Segment->bRaiseTerrain = Road.bRaise && !bInSpan;
			Segment->bLowerTerrain = Road.bLower && !bInSpan;
			bDeforms |= Segment->bRaiseTerrain || Segment->bLowerTerrain || Segment->LayerName != NAME_None;
			if (Mesh)
			{
				FLandscapeSplineMeshEntry Entry;
				Entry.Mesh = Mesh;
				Entry.ForwardAxis = RoadMeshAxis(Road.MeshAxis);
				Entry.bScaleToWidth = Road.bScaleToWidth;
				Segment->SplineMeshes.Add(Entry);
			}
			Points[Index]->ConnectedSegments.Add(FLandscapeSplineConnection(Segment, 0));
			Points[Next]->ConnectedSegments.Add(FLandscapeSplineConnection(Segment, 1));
			++SegmentCount;
		}
		for (ULandscapeSplineControlPoint* Point : Points)
		{
			Point->UpdateSplinePoints();
		}
		PointCount += Points.Num();
	}
	Component->MarkRenderStateDirty();
	Component->MarkPackageDirty();
	// Also when nothing deforms: a re-send that turns it off takes the earlier shape and paint out of the landscape.
	TArray<FString> LayersRemoved;
	LilaLink::RedrawSplineLayer(Landscape, bDeforms, Warnings, &LayersRemoved);

	TArray<TSharedPtr<FJsonValue>> WarningValues, RemovedValues;
	for (const FString& W : Warnings)
	{
		WarningValues.Add(MakeShared<FJsonValueString>(W));
	}
	for (const FString& L : LayersRemoved)
	{
		RemovedValues.Add(MakeShared<FJsonValueString>(L));
	}
	TSharedRef<FJsonObject> Out = MakeShared<FJsonObject>();
	Out->SetStringField(TEXT("actor"), OwnerPath);
	Out->SetStringField(TEXT("landscape"), Landscape->GetActorLabel());
	Out->SetNumberField(TEXT("roads"), Roads.Num());
	Out->SetNumberField(TEXT("controlPoints"), PointCount);
	Out->SetNumberField(TEXT("segments"), SegmentCount);
	Out->SetNumberField(TEXT("regionsLoaded"), RegionsLoaded);
	Out->SetArrayField(TEXT("paintLayersRemoved"), RemovedValues);
	Out->SetArrayField(TEXT("warnings"), WarningValues);
	UE_LOG(LogLilaLink, Log, TEXT("setTerrainRoads: %d road(s) on %s"), Roads.Num(), *Landscape->GetActorLabel());
	return MakeShared<FJsonValueObject>(Out);
}

TSharedPtr<FJsonValue> RemoveTerrainRoads(const TSharedPtr<FJsonObject>& Params, FString& OutError)
{
	TRACE_CPUPROFILER_EVENT_SCOPE(LilaLinkTerrainRoads::RemoveTerrainRoads);

	FString Id;
	if (!Params->TryGetStringField(TEXT("id"), Id) || Id.IsEmpty())
	{
		OutError = TEXT("removeTerrainRoads: give id");
		return nullptr;
	}
	UWorld* World = RoadEditorWorld();
	if (!World)
	{
		OutError = TEXT("removeTerrainRoads: no editor world");
		return nullptr;
	}
	FScopedTransaction Transaction(NSLOCTEXT("LilaLink", "RemoveTerrainRoads", "Lila: Remove Terrain Roads"));
	bool bRemoved = false;
	int32 RegionsLoaded = 0;
	TArray<ALandscape*> Reshaped;
	LilaLink::LoadSplineActors(World);
	if (AActor* Tagged = FindRoadActor(World, RoadActorTag(Id)))
	{
		if (const ALandscapeSplineActor* SplineActor = Cast<ALandscapeSplineActor>(Tagged))
		{
			// The redraw that takes the roads out reaches loaded landscape only.
			RegionsLoaded = LilaLink::LoadLandscapeUnder(World, LilaLink::SplineBounds(SplineActor->GetSplinesComponent(), FString()));
			const ULandscapeInfo* Info = SplineActor->GetLandscapeInfo();
			if (ALandscape* Landscape = Info ? Info->LandscapeActor.Get() : nullptr)
			{
				Reshaped.Add(Landscape);
			}
		}
		bRemoved = World->EditorDestroyActor(Tagged, true);
	}
	const FString Prefix = RoadPrefix(Id);
	for (TActorIterator<ALandscape> It(World); It; ++It)
	{
		if (ULandscapeSplinesComponent* Splines = It->GetSplinesComponent())
		{
			const int32 Before = Splines->GetControlPoints().Num();
			ClearRoads(Splines, Prefix);
			if (Splines->GetControlPoints().Num() != Before)
			{
				bRemoved = true;
				Splines->MarkRenderStateDirty();
				Reshaped.AddUnique(*It);
			}
		}
	}
	TArray<FString> Warnings, LayersRemoved;
	for (ALandscape* Landscape : Reshaped)
	{
		LilaLink::RedrawSplineLayer(Landscape, /*bAddLayer=*/false, Warnings, &LayersRemoved);
	}
	TArray<TSharedPtr<FJsonValue>> WarningValues, RemovedValues;
	for (const FString& W : Warnings)
	{
		WarningValues.Add(MakeShared<FJsonValueString>(W));
	}
	for (const FString& L : LayersRemoved)
	{
		RemovedValues.Add(MakeShared<FJsonValueString>(L));
	}
	TSharedRef<FJsonObject> Out = MakeShared<FJsonObject>();
	Out->SetBoolField(TEXT("removed"), bRemoved);
	Out->SetNumberField(TEXT("regionsLoaded"), RegionsLoaded);
	Out->SetArrayField(TEXT("paintLayersRemoved"), RemovedValues);
	Out->SetArrayField(TEXT("warnings"), WarningValues);
	return MakeShared<FJsonValueObject>(Out);
}
} // namespace

void LilaLink::AddTerrainRoadMethods(TMap<FString, FLilaLinkMethod>& Methods)
{
	Methods.Add(TEXT("setTerrainRoads"), LilaLink::Sync(&SetTerrainRoads));
	Methods.Add(TEXT("removeTerrainRoads"), LilaLink::Sync(&RemoveTerrainRoads));
}
