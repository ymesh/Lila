#include "LilaLinkSplines.h"

#include "LilaLinkLandscape.h"
#include "LilaLinkLog.h"

#include "Components/SplineComponent.h"
#include "Components/SplineMeshComponent.h"
#include "Editor.h"
#include "Engine/StaticMesh.h"
#include "Engine/World.h"
#include "EngineUtils.h"
#include "ILandscapeSplineInterface.h"
#include "Landscape.h"
#include "LandscapeComponent.h"
#include "LandscapeEdit.h"
#include "LandscapeEditLayer.h"
#include "LandscapeHeightfieldCollisionComponent.h"
#include "LandscapeInfo.h"
#include "LandscapeLayerInfoObject.h"
#include "LandscapeSplineActor.h"
#include "LandscapeSplineControlPoint.h"
#include "LandscapeSplineSegment.h"
#include "LandscapeSplinesComponent.h"
#include "LandscapeStreamingProxy.h"
#include "Materials/Material.h"
#include "Misc/Crc.h"
#include "Misc/PackageName.h"
#include "ProfilingDebugging/CpuProfilerTrace.h"
#include "ScopedTransaction.h"
#include "WorldPartition/LoaderAdapter/LoaderAdapterActorList.h"
#include "WorldPartition/WorldPartition.h"
#include "WorldPartition/WorldPartitionActorDescInstance.h"
#include "WorldPartition/WorldPartitionHelpers.h"

namespace
{
/** Marks the components and actors this file makes, so a re-send replaces only those. */
const FName LilaTag(TEXT("LilaLink"));

struct FLilaSplinePoint
{
	FVector Position = FVector::ZeroVector;
	FVector ArriveTangent = FVector::ZeroVector;
	FVector LeaveTangent = FVector::ZeroVector;
	FVector Up = FVector::UpVector;
	FVector Scale = FVector::OneVector;
	double Roll = 0.0;
};

struct FLilaSpline
{
	bool bClosed = false;
	TArray<FLilaSplinePoint> Points;
	/** Landscape mode: the paint layer it paints, over LayerWidth of its width, and whether it deforms (unset: deformTerrain). */
	FName Layer;
	double LayerWidth = 1.0;
	TOptional<bool> Deform;
};

FName TagFor(const FString& Id)
{
	return FName(*(TEXT("LilaLink.Spline:") + Id.Right(900)));
}

/** Names of the landscape spline objects made for an id, so they can be found again in a shared splines component. */
FString LandscapePrefix(const FString& Id)
{
	return FString::Printf(TEXT("LilaLink_%08X_"), FCrc::StrCrc32(*Id));
}

bool ReadVector(const TSharedPtr<FJsonObject>& Object, const TCHAR* Field, FVector& Out)
{
	const TArray<TSharedPtr<FJsonValue>>* Values = nullptr;
	if (!Object->TryGetArrayField(Field, Values) || Values->Num() != 3)
	{
		return false;
	}
	Out = FVector((*Values)[0]->AsNumber(), (*Values)[1]->AsNumber(), (*Values)[2]->AsNumber());
	return !Out.ContainsNaN();
}

bool ParseSplines(const TSharedPtr<FJsonObject>& Params, TArray<FLilaSpline>& Out, FString& OutError)
{
	const TArray<TSharedPtr<FJsonValue>>* Splines = nullptr;
	if (!Params->TryGetArrayField(TEXT("splines"), Splines) || Splines->IsEmpty())
	{
		OutError = TEXT("setSpline: give splines");
		return false;
	}
	for (const TSharedPtr<FJsonValue>& Value : *Splines)
	{
		const TSharedPtr<FJsonObject>* Object = nullptr;
		const TArray<TSharedPtr<FJsonValue>>* Points = nullptr;
		if (!Value.IsValid() || !Value->TryGetObject(Object) || !(*Object)->TryGetArrayField(TEXT("points"), Points) || Points->Num() < 2)
		{
			OutError = TEXT("setSpline: each spline needs at least two points");
			return false;
		}
		FLilaSpline& Spline = Out.AddDefaulted_GetRef();
		(*Object)->TryGetBoolField(TEXT("closed"), Spline.bClosed);
		FString Layer;
		if ((*Object)->TryGetStringField(TEXT("layer"), Layer) && !Layer.IsEmpty())
		{
			Spline.Layer = FName(*Layer);
		}
		(*Object)->TryGetNumberField(TEXT("layerWidth"), Spline.LayerWidth);
		Spline.LayerWidth = FMath::Clamp(Spline.LayerWidth, 0.01, 1.0);
		bool bSplineDeform = false;
		if ((*Object)->TryGetBoolField(TEXT("deform"), bSplineDeform))
		{
			Spline.Deform = bSplineDeform;
		}
		for (const TSharedPtr<FJsonValue>& PointValue : *Points)
		{
			const TSharedPtr<FJsonObject>* PointObject = nullptr;
			if (!PointValue.IsValid() || !PointValue->TryGetObject(PointObject))
			{
				OutError = TEXT("setSpline: a point is not an object");
				return false;
			}
			FLilaSplinePoint& Point = Spline.Points.AddDefaulted_GetRef();
			if (!ReadVector(*PointObject, TEXT("position"), Point.Position))
			{
				OutError = TEXT("setSpline: a point has no position");
				return false;
			}
			ReadVector(*PointObject, TEXT("arriveTangent"), Point.ArriveTangent);
			ReadVector(*PointObject, TEXT("leaveTangent"), Point.LeaveTangent);
			ReadVector(*PointObject, TEXT("up"), Point.Up);
			ReadVector(*PointObject, TEXT("scale"), Point.Scale);
			(*PointObject)->TryGetNumberField(TEXT("roll"), Point.Roll);
		}
	}
	return true;
}

UWorld* EditorWorld()
{
	return GEditor ? GEditor->GetEditorWorldContext().World() : nullptr;
}

AActor* FindTagged(UWorld* World, FName Tag)
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

ALandscape* FindLandscape(UWorld* World, const FString& Label)
{
	ALandscape* First = nullptr;
	for (TActorIterator<ALandscape> It(World); It; ++It)
	{
		if (Label.IsEmpty() || It->GetActorLabel() == Label || It->GetName() == Label)
		{
			return *It;
		}
		First = First ? First : *It;
	}
	return Label.IsEmpty() ? First : nullptr;
}

/** The ALandscape tag that records a paint layer as one Lila added for its splines. */
const FString PaintLayerTagPrefix(TEXT("LilaLink.PaintLayer:"));

FName PaintLayerTag(FName Layer)
{
	return FName(*(PaintLayerTagPrefix + Layer.ToString()));
}

/** When the landscape draws with Lila's generated M_<label>, rebuilds it to sample the landscape's target layers. */
void RebuildGeneratedMaterial(ALandscape* Landscape, TArray<FString>& Warnings)
{
	TRACE_CPUPROFILER_EVENT_SCOPE(LilaLinkSplines::RebuildGeneratedMaterial);

	const FString Label = Landscape->GetActorLabel();
	const FString Generated = FString::Printf(TEXT("/Game/LilaLink/Landscapes/%s/M_%s"), *Label, *Label);
	UMaterialInterface* Current = Landscape->GetLandscapeMaterial();
	if (!Current || Current->GetOutermost()->GetName() != Generated)
	{
		return;
	}
	TArray<FString> Ignored;
	TArray<FName> Layers;
	Landscape->GetTargetLayers().GetKeys(Layers);
	// The visibility (holes) layer is drawn by the engine, not sampled as paint.
	Layers.RemoveAll([](const FName& L) { return ALandscapeProxy::VisibilityLayer && L == ALandscapeProxy::VisibilityLayer->GetLayerName(); });
	UMaterial* Material = LilaLink::Landscape::GeneratedMaterial(Label, Layers, Ignored, Warnings);
	if (Material && Material != Current)
	{
		Landscape->LandscapeMaterial = Material;
	}
	// Also when it is the same asset rebuilt in place: the landscape's material instances follow its layers.
	FPropertyChangedEvent Changed(FindFProperty<FProperty>(ALandscapeProxy::StaticClass(), GET_MEMBER_NAME_CHECKED(ALandscapeProxy, LandscapeMaterial)));
	Landscape->PostEditChangeProperty(Changed);
}

/** Every paint layer a spline of the landscape (its own splines and every loaded spline actor) paints. */
TSet<FName> PaintedBySplines(ULandscapeInfo* Info)
{
	TRACE_CPUPROFILER_EVENT_SCOPE(LilaLinkSplines::PaintedBySplines);

	TSet<FName> Painted;
	auto Collect = [&Painted](const ULandscapeSplinesComponent* Splines) {
		if (!IsValid(Splines) || !IsValid(Splines->GetOwner()) || Splines->GetOwner()->IsActorBeingDestroyed())
		{
			return;
		}
		for (const ULandscapeSplineControlPoint* Point : Splines->GetControlPoints())
		{
			if (Point && Point->LayerName != NAME_None)
			{
				Painted.Add(Point->LayerName);
			}
		}
		for (const ULandscapeSplineSegment* Segment : Splines->GetSegments())
		{
			if (Segment && Segment->LayerName != NAME_None)
			{
				Painted.Add(Segment->LayerName);
			}
		}
	};
	Info->ForAllSplineActors([&Collect](TScriptInterface<ILandscapeSplineInterface> Owner) {
		if (Owner)
		{
			Collect(Owner->GetSplinesComponent());
		}
	});
	Info->ForEachLandscapeProxy([&Collect](ALandscapeProxy* Proxy) {
		Collect(Proxy->GetSplinesComponent());
		return true;
	});
	return Painted;
}

/** Whether a loaded region holds paint of LayerInfo in an edit layer other than the splines one: paint a user made. */
bool PaintedOutsideSplines(ULandscapeInfo* Info, const ULandscapeLayerInfoObject* LayerInfo, const FGuid& SplinesLayer)
{
	bool bFound = false;
	Info->ForEachLandscapeProxy([&](ALandscapeProxy* Proxy) {
		for (ULandscapeComponent* Component : Proxy->LandscapeComponents)
		{
			if (!Component)
			{
				continue;
			}
			Component->ForEachLayer([&](const FGuid& Guid, FLandscapeLayerComponentData& Data) {
				bFound |= Guid != SplinesLayer && Data.WeightmapData.LayerAllocations.ContainsByPredicate(
					[LayerInfo](const FWeightmapLayerAllocationInfo& A) { return A.LayerInfo == LayerInfo; });
			});
		}
		return !bFound;
	});
	return bFound;
}

/**
 * Takes Lila's paint layers (tagged on the landscape) that no spline paints any more out of the landscape and its generated
 * material. A layer with paint outside the splines edit layer is the user's from then on: kept, its tag dropped, with a warning.
 */
void DropUnusedPaintLayers(ALandscape* Landscape, TArray<FString>& Removed, TArray<ULandscapeLayerInfoObject*>& RemovedInfos, TArray<FString>& Warnings)
{
	TRACE_CPUPROFILER_EVENT_SCOPE(LilaLinkSplines::DropUnusedPaintLayers);

	ULandscapeInfo* Info = Landscape->GetLandscapeInfo();
	TArray<FName> Owned;
	for (const FName& Tag : Landscape->Tags)
	{
		const FString Text = Tag.ToString();
		if (Text.StartsWith(PaintLayerTagPrefix))
		{
			Owned.Add(FName(*Text.RightChop(PaintLayerTagPrefix.Len())));
		}
	}
	if (!Info || Owned.IsEmpty())
	{
		return;
	}
	LilaLink::LoadSplineActors(Landscape->GetWorld());
	const TSet<FName> Painted = PaintedBySplines(Info);
	const ULandscapeEditLayerBase* SplinesLayer = Landscape->FindEditLayerOfTypeConst(ULandscapeEditLayerSplines::StaticClass());
	const FGuid SplinesGuid = SplinesLayer ? SplinesLayer->GetGuid() : FGuid();
	bool bChanged = false;
	for (const FName& Layer : Owned)
	{
		if (Painted.Contains(Layer))
		{
			continue;
		}
		Landscape->Modify();
		Landscape->Tags.Remove(PaintLayerTag(Layer));
		ULandscapeLayerInfoObject* LayerInfo = Info->GetLayerInfoByName(Layer);
		if (LayerInfo && PaintedOutsideSplines(Info, LayerInfo, SplinesGuid))
		{
			Warnings.Add(FString::Printf(TEXT("paint layer %s, added for Lila's roads, has paint of its own, so it stays on the landscape"), *Layer.ToString()));
			continue;
		}
		if (LayerInfo)
		{
			if (SplinesGuid.IsValid())
			{
				FLandscapeEditDataInterface(Info, SplinesGuid).DeleteLayer(LayerInfo);
			}
			FLandscapeEditDataInterface(Info, FGuid()).DeleteLayer(LayerInfo);
			Info->DeleteLayer(LayerInfo, Layer);
			RemovedInfos.Add(LayerInfo);
		}
		else
		{
			Landscape->RemoveTargetLayer(Layer);
			Info->UpdateLayerInfoMap();
		}
		Removed.Add(Layer.ToString());
		bChanged = true;
	}
	if (bChanged)
	{
		RebuildGeneratedMaterial(Landscape, Warnings);
	}
}

/**
 * Rebuilds the collision layer data of loaded regions whose collision still lists a removed layer. The engine keeps a stale
 * trailing entry when a component loses its last allocated layer, which would keep the layer info referenced.
 */
void RefreshCollisionLayers(ULandscapeInfo* Info, const TArray<ULandscapeLayerInfoObject*>& RemovedInfos)
{
	TRACE_CPUPROFILER_EVENT_SCOPE(LilaLinkSplines::RefreshCollisionLayers);

	Info->ForEachLandscapeProxy([&RemovedInfos](ALandscapeProxy* Proxy) {
		for (ULandscapeComponent* Component : Proxy->LandscapeComponents)
		{
			ULandscapeHeightfieldCollisionComponent* Collision = Component ? Component->GetCollisionComponent() : nullptr;
			if (Collision && Collision->ComponentLayerInfos.ContainsByPredicate([&RemovedInfos](const ULandscapeLayerInfoObject* L) { return RemovedInfos.Contains(L); }))
			{
				Collision->Modify();
				Collision->ComponentLayerInfos.Reset();
				Component->UpdateCollisionLayerData();
			}
		}
		return true;
	});
}

/**
 * Whether the landscape can take Layer's paint: a target layer with a layer info, added when missing (LI_<Layer> beside
 * Lila's landscape material, which is rebuilt to sample it when the landscape uses it).
 */
bool EnsurePaintLayer(ALandscape* Landscape, FName Layer, TArray<FString>& Added, TArray<FString>& Warnings)
{
	TRACE_CPUPROFILER_EVENT_SCOPE(LilaLinkSplines::EnsurePaintLayer);

	ULandscapeInfo* Info = Landscape->GetLandscapeInfo();
	if (!Info || !Info->GetLayerInfoByName(Layer))
	{
		const FString Label = Landscape->GetActorLabel();
		TArray<FString> Created;
		ULandscapeLayerInfoObject* LayerInfo = LilaLink::Landscape::LayerInfoFor(Label, Layer, Created);
		if (!LayerInfo)
		{
			Warnings.Add(FString::Printf(TEXT("could not make a layer info for paint layer %s, so the splines paint nothing there"), *Layer.ToString()));
			return false;
		}
		Landscape->Modify();
		if (Landscape->HasTargetLayer(Layer))
		{
			Landscape->UpdateTargetLayer(Layer, FLandscapeTargetLayerSettings(LayerInfo));
		}
		else
		{
			// Only a layer Lila adds outright is Lila's to take away again; a target layer the user named stays theirs.
			Landscape->AddTargetLayer(Layer, FLandscapeTargetLayerSettings(LayerInfo));
			Landscape->Tags.AddUnique(PaintLayerTag(Layer));
		}
		if (Info)
		{
			Info->UpdateLayerInfoMap(Landscape);
		}
		Added.Add(Layer.ToString());
		RebuildGeneratedMaterial(Landscape, Warnings);
	}
	if (!Landscape->RetrieveTargetLayerNamesFromMaterials(/*bIncludeVisibilityLayer=*/false).Contains(Layer))
	{
		Warnings.Add(FString::Printf(TEXT("the landscape's material does not sample paint layer %s: the roads' paint is there but not drawn"), *Layer.ToString()));
	}
	return true;
}

ESplineMeshAxis::Type AxisFrom(const FString& Name)
{
	return Name.Equals(TEXT("y"), ESearchCase::IgnoreCase) ? ESplineMeshAxis::Y
		: Name.Equals(TEXT("z"), ESearchCase::IgnoreCase) ? ESplineMeshAxis::Z
		: ESplineMeshAxis::X;
}

/** Replaces the component's points: positions first, then the tangents they need, then the up vectors that turn on them. */
void FillSpline(USplineComponent* Component, const FLilaSpline& Spline)
{
	TRACE_CPUPROFILER_EVENT_SCOPE(LilaLinkSplines::FillSpline);

	Component->ClearSplinePoints(false);
	for (const FLilaSplinePoint& Point : Spline.Points)
	{
		Component->AddSplinePoint(Point.Position, ESplineCoordinateSpace::Local, false);
	}
	for (int32 Index = 0; Index < Spline.Points.Num(); ++Index)
	{
		const FLilaSplinePoint& Point = Spline.Points[Index];
		Component->SetTangentsAtSplinePoint(Index, Point.ArriveTangent, Point.LeaveTangent, ESplineCoordinateSpace::Local, false);
		Component->SetScaleAtSplinePoint(Index, Point.Scale, false);
	}
	Component->SetClosedLoop(Spline.bClosed, false);
	Component->UpdateSpline();
	for (int32 Index = 0; Index < Spline.Points.Num(); ++Index)
	{
		Component->SetUpVectorAtSplinePoint(Index, Spline.Points[Index].Up, ESplineCoordinateSpace::Local, false);
	}
	Component->UpdateSpline();
}

/** A spline mesh per segment of the spline, attached to it, in its space. Returns how many. */
int32 AddSplineMeshes(AActor* Actor, USplineComponent* Spline, UStaticMesh* Mesh, ESplineMeshAxis::Type Axis)
{
	TRACE_CPUPROFILER_EVENT_SCOPE(LilaLinkSplines::AddSplineMeshes);

	const int32 Points = Spline->GetNumberOfSplinePoints();
	const int32 Segments = Spline->GetNumberOfSplineSegments();
	for (int32 Segment = 0; Segment < Segments; ++Segment)
	{
		const int32 Next = (Segment + 1) % Points;
		const FName Name = MakeUniqueObjectName(Actor, USplineMeshComponent::StaticClass(), *FString::Printf(TEXT("LilaSplineMesh%d"), Segment));
		USplineMeshComponent* Piece = NewObject<USplineMeshComponent>(Actor, Name, RF_Transactional);
		Piece->ComponentTags.Add(LilaTag);
		Piece->SetMobility(Spline->Mobility);
		Piece->SetupAttachment(Spline);
		Piece->SetStaticMesh(Mesh);
		Piece->SetForwardAxis(Axis, false);
		Piece->SetStartAndEnd(Spline->GetLocationAtSplinePoint(Segment, ESplineCoordinateSpace::Local),
			Spline->GetLeaveTangentAtSplinePoint(Segment, ESplineCoordinateSpace::Local),
			Spline->GetLocationAtSplinePoint(Next, ESplineCoordinateSpace::Local),
			Spline->GetArriveTangentAtSplinePoint(Next, ESplineCoordinateSpace::Local), false);
		const FVector StartScale = Spline->GetScaleAtSplinePoint(Segment), EndScale = Spline->GetScaleAtSplinePoint(Next);
		Piece->SetStartScale(FVector2D(StartScale.Y, StartScale.Z), false);
		Piece->SetEndScale(FVector2D(EndScale.Y, EndScale.Z), false);
		Piece->SetStartRollDegrees(Spline->GetRollAtSplinePoint(Segment, ESplineCoordinateSpace::Local), false);
		Piece->SetEndRollDegrees(Spline->GetRollAtSplinePoint(Next, ESplineCoordinateSpace::Local), false);
		Actor->AddInstanceComponent(Piece);
		Piece->RegisterComponent();
		Piece->UpdateMesh();
	}
	return Segments;
}

void RemoveOurComponents(AActor* Actor, const TArray<UActorComponent*>& Components)
{
	for (UActorComponent* Component : Components)
	{
		Component->Modify();
		Actor->RemoveInstanceComponent(Component);
		Component->DestroyComponent();
	}
}

TSharedPtr<FJsonValue> SetActorSplines(UWorld* World, const FString& Id, const FString& Label, const FString& Folder, const FVector& Location,
	const TArray<FLilaSpline>& Splines, UStaticMesh* Mesh, ESplineMeshAxis::Type Axis, FString& OutError)
{
	TRACE_CPUPROFILER_EVENT_SCOPE(LilaLinkSplines::SetActorSplines);

	const FName Tag = TagFor(Id);
	AActor* Actor = FindTagged(World, Tag);
	if (Actor && Actor->IsA<ALandscapeSplineActor>())
	{
		// The curve was a landscape spline before: start over as a plain actor.
		World->EditorDestroyActor(Actor, true);
		Actor = nullptr;
	}
	if (Actor)
	{
		Actor->Modify();
		Actor->SetActorLocation(Location);
	}
	else
	{
		FActorSpawnParameters Spawn;
		Spawn.ObjectFlags |= RF_Transactional;
		Actor = World->SpawnActor<AActor>(AActor::StaticClass(), FTransform(Location), Spawn);
		if (!Actor)
		{
			OutError = TEXT("setSpline: could not spawn the spline actor");
			return nullptr;
		}
		Actor->Tags.Add(Tag);
		Actor->Tags.Add(LilaTag);
	}
	Actor->SetActorLabel(Label);
	Actor->SetFolderPath(FName(*Folder));

	TArray<USplineComponent*> AllSplines;
	Actor->GetComponents(AllSplines);
	TArray<USplineComponent*> Ours;
	for (USplineComponent* Component : AllSplines)
	{
		if (Component->ComponentHasTag(LilaTag))
		{
			Ours.Add(Component);
		}
	}
	TArray<USplineMeshComponent*> AllMeshes;
	Actor->GetComponents(AllMeshes);
	TArray<UActorComponent*> Stale;
	for (USplineMeshComponent* Component : AllMeshes)
	{
		if (Component->ComponentHasTag(LilaTag))
		{
			Stale.Add(Component);
		}
	}
	for (int32 Index = Splines.Num(); Index < Ours.Num(); ++Index)
	{
		// The first one is the root, and there is always at least one spline.
		Stale.Add(Ours[Index]);
	}
	RemoveOurComponents(Actor, Stale);
	Ours.SetNum(FMath::Min(Ours.Num(), Splines.Num()));

	int32 MeshCount = 0;
	for (int32 Index = 0; Index < Splines.Num(); ++Index)
	{
		USplineComponent* Component = Index < Ours.Num() ? Ours[Index] : nullptr;
		if (!Component)
		{
			const FName Name = MakeUniqueObjectName(Actor, USplineComponent::StaticClass(), *FString::Printf(TEXT("LilaSpline%d"), Index));
			Component = NewObject<USplineComponent>(Actor, Name, RF_Transactional);
			Component->ComponentTags.Add(LilaTag);
			if (USceneComponent* Root = Actor->GetRootComponent())
			{
				Component->SetupAttachment(Root);
			}
			else
			{
				Actor->SetRootComponent(Component);
			}
			Actor->AddInstanceComponent(Component);
			Component->RegisterComponent();
		}
		Component->Modify();
		FillSpline(Component, Splines[Index]);
		if (Mesh)
		{
			MeshCount += AddSplineMeshes(Actor, Component, Mesh, Axis);
		}
	}
	if (Actor->GetRootComponent())
	{
		Actor->SetActorLocation(Location);
	}
	Actor->MarkPackageDirty();

	TSharedRef<FJsonObject> Out = MakeShared<FJsonObject>();
	Out->SetStringField(TEXT("actor"), Actor->GetPathName());
	Out->SetNumberField(TEXT("components"), Splines.Num());
	Out->SetNumberField(TEXT("segments"), MeshCount);
	return MakeShared<FJsonValueObject>(Out);
}

/** Deletes the control points and segments made for an id from a splines component (all of them when Prefix is empty). */
void ClearLandscapeSplines(ULandscapeSplinesComponent* Splines, const FString& Prefix)
{
	TRACE_CPUPROFILER_EVENT_SCOPE(LilaLinkSplines::ClearLandscapeSplines);

	Splines->Modify();
	auto Ours = [&Prefix](const UObject* Object) { return Object && (Prefix.IsEmpty() || Object->GetName().StartsWith(Prefix)); };
	const TArray<TObjectPtr<ULandscapeSplineSegment>> Segments = Splines->GetSegments();
	for (ULandscapeSplineSegment* Segment : Segments)
	{
		if (!Ours(Segment))
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
		if (Ours(Point) && Point->ConnectedSegments.IsEmpty())
		{
			Point->Modify();
			Point->DeleteSplinePoints();
			Splines->GetControlPoints().Remove(Point);
		}
	}
}

TSharedPtr<FJsonValue> SetLandscapeSplines(UWorld* World, const FString& Id, const FString& Label, const FString& LandscapeLabel, const FVector& Location,
	const TArray<FLilaSpline>& Splines, UStaticMesh* Mesh, ESplineMeshAxis::Type Axis, double Width, bool bDeform, bool bJoin, TArray<FString>& Warnings,
	FString& OutError)
{
	TRACE_CPUPROFILER_EVENT_SCOPE(LilaLinkSplines::SetLandscapeSplines);

	ALandscape* Landscape = FindLandscape(World, LandscapeLabel);
	if (!Landscape)
	{
		OutError = LandscapeLabel.IsEmpty() ? FString(TEXT("setSpline: the level has no landscape; send it as a plain spline actor instead"))
			: FString::Printf(TEXT("setSpline: no landscape labelled %s"), *LandscapeLabel);
		return nullptr;
	}
	const FName Tag = TagFor(Id);
	LilaLink::LoadSplineActors(World);
	AActor* Tagged = FindTagged(World, Tag);
	ULandscapeSplinesComponent* Component = nullptr;
	FString Prefix = LandscapePrefix(Id);
	FString OwnerPath;
	int32 RegionsLoaded = 0;
	if (World->GetWorldPartition())
	{
		// In a partitioned world each curve gets its own landscape spline actor, found again by its tag.
		ALandscapeSplineActor* SplineActor = Cast<ALandscapeSplineActor>(Tagged);
		// Only loaded landscape takes a redraw: load what the earlier and the new splines cover, before the Splines layer is
		// added, so every region gets that layer too.
		FBox Covered = SplineActor ? LilaLink::SplineBounds(SplineActor->GetSplinesComponent(), FString()) : FBox(ForceInit);
		for (const FLilaSpline& Spline : Splines)
		{
			for (const FLilaSplinePoint& In : Spline.Points)
			{
				// Half width plus the engine's default end falloff, the widest reach a control point paints or shapes.
				Covered += FBox::BuildAABB(Location + In.Position, FVector(Width * FMath::Abs(In.Scale.Y) + 2000.0));
			}
		}
		RegionsLoaded = LilaLink::LoadLandscapeUnder(World, Covered);
		if (Tagged && !SplineActor)
		{
			World->EditorDestroyActor(Tagged, true);
		}
		if (!SplineActor)
		{
			ULandscapeInfo* Info = Landscape->GetLandscapeInfo();
			SplineActor = Info ? Info->CreateSplineActor(Location) : nullptr;
			if (!SplineActor)
			{
				OutError = TEXT("setSpline: could not make a landscape spline actor");
				return nullptr;
			}
			SplineActor->Tags.Add(Tag);
			SplineActor->Tags.Add(LilaTag);
		}
		SplineActor->Modify();
		SplineActor->SetActorLabel(Label);
		Component = SplineActor->GetSplinesComponent();
		OwnerPath = SplineActor->GetPathName();
	}
	else
	{
		if (Tagged)
		{
			World->EditorDestroyActor(Tagged, true);
		}
		Landscape->Modify();
		if (!Landscape->GetSplinesComponent())
		{
			Landscape->CreateSplineComponent();
		}
		Component = Landscape->GetSplinesComponent();
		OwnerPath = Landscape->GetPathName();
	}
	if (!Component)
	{
		OutError = TEXT("setSpline: the landscape has no splines component");
		return nullptr;
	}
	ClearLandscapeSplines(Component, Prefix);

	TArray<FString> LayersAdded, LayersRemoved;
	TSet<FName> Paintable;
	for (const FLilaSpline& Spline : Splines)
	{
		if (Spline.Layer != NAME_None && !Paintable.Contains(Spline.Layer) && EnsurePaintLayer(Landscape, Spline.Layer, LayersAdded, Warnings))
		{
			Paintable.Add(Spline.Layer);
		}
	}

	const FTransform ToComponent = Component->GetComponentTransform();
	int32 SegmentCount = 0;
	bool bShapes = false;
	// With join, splines ending at the same place share one control point: a road network's junctions. Splines painting
	// another layer or deforming differently (shoulders beside a road) keep their own points.
	TMap<TTuple<FIntVector, FName, bool>, ULandscapeSplineControlPoint*> Shared;
	for (int32 SplineIndex = 0; SplineIndex < Splines.Num(); ++SplineIndex)
	{
		const FLilaSpline& Spline = Splines[SplineIndex];
		const bool bSplineDeforms = Spline.Deform.Get(bDeform);
		const FName Layer = Paintable.Contains(Spline.Layer) ? Spline.Layer : NAME_None;
		bShapes |= bSplineDeforms || Layer != NAME_None;
		TArray<ULandscapeSplineControlPoint*> Points;
		for (int32 Index = 0; Index < Spline.Points.Num(); ++Index)
		{
			const FLilaSplinePoint& In = Spline.Points[Index];
			const FVector WorldPosition = Location + In.Position;
			const TTuple<FIntVector, FName, bool> Key(
				FIntVector(FMath::RoundToInt(WorldPosition.X), FMath::RoundToInt(WorldPosition.Y), FMath::RoundToInt(WorldPosition.Z)), Layer, bSplineDeforms);
			if (bJoin)
			{
				if (ULandscapeSplineControlPoint** Found = Shared.Find(Key))
				{
					ULandscapeSplineControlPoint* Point = *Found;
					const float PaintHalf = FMath::Max(Point->Width * Point->LayerWidthRatio, static_cast<float>(Width * In.Scale.Y * Spline.LayerWidth));
					Point->Width = FMath::Max(Point->Width, static_cast<float>(Width * In.Scale.Y));
					Point->LayerWidthRatio = FMath::Clamp(PaintHalf / FMath::Max(Point->Width, 0.001f), 0.01f, 1.0f);
					Points.Add(Point);
					continue;
				}
			}
			const FName Name(*FString::Printf(TEXT("%sP%d_%d"), *Prefix, SplineIndex, Index));
			ULandscapeSplineControlPoint* Point = NewObject<ULandscapeSplineControlPoint>(Component, Name, RF_Transactional);
			Component->GetControlPoints().Add(Point);
			Point->Location = ToComponent.InverseTransformPosition(Location + In.Position);
			const FVector Forward = ToComponent.InverseTransformVectorNoScale(In.LeaveTangent.IsNearlyZero() ? In.ArriveTangent : In.LeaveTangent);
			const FVector Up = ToComponent.InverseTransformVectorNoScale(In.Up);
			Point->Rotation = FRotationMatrix::MakeFromXZ(Forward.GetSafeNormal(), Up).Rotator();
			Point->Width = static_cast<float>(Width * In.Scale.Y);
			Point->LayerWidthRatio = static_cast<float>(Spline.LayerWidth);
			Point->LayerName = Layer;
			Point->bRaiseTerrain = bSplineDeforms;
			Point->bLowerTerrain = bSplineDeforms;
			Points.Add(Point);
			if (bJoin)
			{
				Shared.Add(Key, Point);
			}
		}
		const int32 Segments = Spline.bClosed ? Points.Num() : Points.Num() - 1;
		for (int32 Index = 0; Index < Segments; ++Index)
		{
			const int32 Next = (Index + 1) % Points.Num();
			if (Points[Index] == Points[Next])
			{
				continue;
			}
			const FName Name(*FString::Printf(TEXT("%sS%d_%d"), *Prefix, SplineIndex, Index));
			ULandscapeSplineSegment* Segment = NewObject<ULandscapeSplineSegment>(Component, Name, RF_Transactional);
			Component->GetSegments().Add(Segment);
			Segment->Connections[0].ControlPoint = Points[Index];
			Segment->Connections[1].ControlPoint = Points[Next];
			// The end tangent is the far point's forward times -TangentLen, so a spline running on through it takes a negative length.
			// A shared (joined) point faces the way of the spline that made it; one leaving it the other way takes the opposite sign.
			const FVector Along = Points[Next]->Location - Points[Index]->Location;
			const float StartSign = FVector::DotProduct(Points[Index]->Rotation.Vector(), Along) >= 0.0 ? 1.0f : -1.0f;
			const float EndSign = FVector::DotProduct(Points[Next]->Rotation.Vector(), Along) >= 0.0 ? 1.0f : -1.0f;
			Segment->Connections[0].TangentLen = StartSign * static_cast<float>(ToComponent.InverseTransformVector(Spline.Points[Index].LeaveTangent).Size());
			Segment->Connections[1].TangentLen = -EndSign * static_cast<float>(ToComponent.InverseTransformVector(Spline.Points[Next].ArriveTangent).Size());
			Segment->LayerName = Layer;
			Segment->bRaiseTerrain = bSplineDeforms;
			Segment->bLowerTerrain = bSplineDeforms;
			if (Mesh)
			{
				FLandscapeSplineMeshEntry Entry;
				Entry.Mesh = Mesh;
				Entry.ForwardAxis = Axis;
				Segment->SplineMeshes.Add(Entry);
			}
			Points[Index]->ConnectedSegments.Add(FLandscapeSplineConnection(Segment, 0));
			Points[Next]->ConnectedSegments.Add(FLandscapeSplineConnection(Segment, 1));
			++SegmentCount;
		}
		for (ULandscapeSplineControlPoint* Point : TSet<ULandscapeSplineControlPoint*>(Points))
		{
			Point->UpdateSplinePoints();
		}
	}
	Component->MarkRenderStateDirty();
	Component->MarkPackageDirty();
	// Also when nothing shapes: a re-send that turns it off takes the earlier shape and paint out of the landscape.
	LilaLink::RedrawSplineLayer(Landscape, bShapes, Warnings, &LayersRemoved);
	if (!bShapes)
	{
		Warnings.Add(TEXT("deformTerrain is off and no paint layer is given: the splines do not shape the landscape"));
	}

	TArray<TSharedPtr<FJsonValue>> PaintValues, AddedValues, RemovedValues;
	for (const FName& L : Paintable)
	{
		PaintValues.Add(MakeShared<FJsonValueString>(L.ToString()));
	}
	for (const FString& L : LayersAdded)
	{
		AddedValues.Add(MakeShared<FJsonValueString>(L));
	}
	for (const FString& L : LayersRemoved)
	{
		RemovedValues.Add(MakeShared<FJsonValueString>(L));
	}
	TSharedRef<FJsonObject> Out = MakeShared<FJsonObject>();
	Out->SetStringField(TEXT("actor"), OwnerPath);
	Out->SetNumberField(TEXT("components"), Splines.Num());
	Out->SetNumberField(TEXT("segments"), SegmentCount);
	Out->SetArrayField(TEXT("paintLayers"), PaintValues);
	Out->SetArrayField(TEXT("paintLayersAdded"), AddedValues);
	Out->SetArrayField(TEXT("paintLayersRemoved"), RemovedValues);
	Out->SetNumberField(TEXT("regionsLoaded"), RegionsLoaded);
	return MakeShared<FJsonValueObject>(Out);
}

TSharedPtr<FJsonValue> SetSpline(const TSharedPtr<FJsonObject>& Params, FString& OutError)
{
	TRACE_CPUPROFILER_EVENT_SCOPE(LilaLinkSplines::SetSpline);

	FString Id;
	if (!Params->TryGetStringField(TEXT("id"), Id) || Id.IsEmpty())
	{
		OutError = TEXT("setSpline: give id");
		return nullptr;
	}
	UWorld* World = EditorWorld();
	if (!World)
	{
		OutError = TEXT("setSpline: no editor world");
		return nullptr;
	}
	TArray<FLilaSpline> Splines;
	if (!ParseSplines(Params, Splines, OutError))
	{
		return nullptr;
	}
	FString Label = FPackageName::GetShortName(Id), Mode = TEXT("actor"), Folder = TEXT("Lila"), LandscapeLabel;
	Params->TryGetStringField(TEXT("label"), Label);
	Params->TryGetStringField(TEXT("mode"), Mode);
	Params->TryGetStringField(TEXT("folder"), Folder);
	Params->TryGetStringField(TEXT("landscape"), LandscapeLabel);
	FVector Location = FVector::ZeroVector;
	ReadVector(Params, TEXT("location"), Location);
	double Width = 200.0;
	Params->TryGetNumberField(TEXT("width"), Width);
	bool bDeform = true, bJoin = false;
	Params->TryGetBoolField(TEXT("deformTerrain"), bDeform);
	Params->TryGetBoolField(TEXT("join"), bJoin);

	TArray<FString> Warnings;
	UStaticMesh* Mesh = nullptr;
	ESplineMeshAxis::Type Axis = ESplineMeshAxis::X;
	const TSharedPtr<FJsonObject>* MeshObject = nullptr;
	if (Params->TryGetObjectField(TEXT("mesh"), MeshObject))
	{
		FString Asset, AxisName;
		(*MeshObject)->TryGetStringField(TEXT("asset"), Asset);
		(*MeshObject)->TryGetStringField(TEXT("forwardAxis"), AxisName);
		Axis = AxisFrom(AxisName);
		const FString ObjectPath = Asset.Contains(TEXT(".")) ? Asset : Asset + TEXT(".") + FPackageName::GetShortName(Asset);
		Mesh = Asset.IsEmpty() ? nullptr : LoadObject<UStaticMesh>(nullptr, *ObjectPath);
		if (!Mesh)
		{
			Warnings.Add(FString::Printf(TEXT("no static mesh at %s; sent without spline meshes"), *Asset));
		}
	}

	FScopedTransaction Transaction(NSLOCTEXT("LilaLink", "SetSpline", "Lila: Update Spline"));
	TSharedPtr<FJsonValue> Result = Mode == TEXT("landscape")
		? SetLandscapeSplines(World, Id, Label, LandscapeLabel, Location, Splines, Mesh, Axis, Width, bDeform, bJoin, Warnings, OutError)
		: SetActorSplines(World, Id, Label, Folder, Location, Splines, Mesh, Axis, OutError);
	if (!Result.IsValid())
	{
		Transaction.Cancel();
		return nullptr;
	}
	// Extra actor tags (contact facts from Lila's seams); a re-send replaces the ones it set before.
	const TArray<TSharedPtr<FJsonValue>>* ExtraTags = nullptr;
	if (Mode != TEXT("landscape") && Params->TryGetArrayField(TEXT("tags"), ExtraTags))
	{
		if (AActor* Tagged = FindTagged(World, TagFor(Id)))
		{
			Tagged->Tags.RemoveAll([](const FName& T) { return T.ToString().StartsWith(TEXT("lila.")); });
			for (const TSharedPtr<FJsonValue>& T : *ExtraTags)
			{
				Tagged->Tags.AddUnique(FName(*(TEXT("lila.") + T->AsString())));
			}
		}
	}
	TArray<TSharedPtr<FJsonValue>> WarningValues;
	for (const FString& W : Warnings)
	{
		WarningValues.Add(MakeShared<FJsonValueString>(W));
	}
	Result->AsObject()->SetArrayField(TEXT("warnings"), WarningValues);
	UE_LOG(LogLilaLink, Log, TEXT("setSpline: %s (%s, %d spline(s))"), *Label, *Mode, Splines.Num());
	return Result;
}

TSharedPtr<FJsonValue> RemoveSpline(const TSharedPtr<FJsonObject>& Params, FString& OutError)
{
	TRACE_CPUPROFILER_EVENT_SCOPE(LilaLinkSplines::RemoveSpline);

	FString Id;
	if (!Params->TryGetStringField(TEXT("id"), Id) || Id.IsEmpty())
	{
		OutError = TEXT("removeSpline: give id");
		return nullptr;
	}
	UWorld* World = EditorWorld();
	if (!World)
	{
		OutError = TEXT("removeSpline: no editor world");
		return nullptr;
	}
	FScopedTransaction Transaction(NSLOCTEXT("LilaLink", "RemoveSpline", "Lila: Remove Spline"));
	bool bRemoved = false;
	int32 RegionsLoaded = 0;
	TArray<ALandscape*> Reshaped;
	LilaLink::LoadSplineActors(World);
	if (AActor* Tagged = FindTagged(World, TagFor(Id)))
	{
		const ALandscapeSplineActor* SplineActor = Cast<ALandscapeSplineActor>(Tagged);
		if (SplineActor)
		{
			// The redraw that takes the splines out reaches loaded landscape only.
			RegionsLoaded = LilaLink::LoadLandscapeUnder(World, LilaLink::SplineBounds(SplineActor->GetSplinesComponent(), FString()));
		}
		const ULandscapeInfo* Info = SplineActor ? SplineActor->GetLandscapeInfo() : nullptr;
		if (ALandscape* Landscape = Info ? Info->LandscapeActor.Get() : nullptr)
		{
			Reshaped.Add(Landscape);
		}
		bRemoved = World->EditorDestroyActor(Tagged, true);
	}
	const FString Prefix = LandscapePrefix(Id);
	for (TActorIterator<ALandscape> It(World); It; ++It)
	{
		if (ULandscapeSplinesComponent* Splines = It->GetSplinesComponent())
		{
			const int32 Before = Splines->GetControlPoints().Num();
			ClearLandscapeSplines(Splines, Prefix);
			if (Splines->GetControlPoints().Num() != Before)
			{
				bRemoved = true;
				Reshaped.AddUnique(*It);
			}
			Splines->MarkRenderStateDirty();
		}
	}
	// The removed splines' shape and paint leave the landscape when it redraws its splines layer.
	TArray<FString> Warnings, LayersRemoved;
	for (ALandscape* Landscape : Reshaped)
	{
		LilaLink::RedrawSplineLayer(Landscape, /*bAddLayer=*/false, Warnings, &LayersRemoved);
	}
	TArray<TSharedPtr<FJsonValue>> RemovedValues, WarningValues;
	for (const FString& L : LayersRemoved)
	{
		RemovedValues.Add(MakeShared<FJsonValueString>(L));
	}
	for (const FString& W : Warnings)
	{
		WarningValues.Add(MakeShared<FJsonValueString>(W));
	}
	TSharedRef<FJsonObject> Out = MakeShared<FJsonObject>();
	Out->SetBoolField(TEXT("removed"), bRemoved);
	Out->SetNumberField(TEXT("regionsLoaded"), RegionsLoaded);
	Out->SetArrayField(TEXT("paintLayersRemoved"), RemovedValues);
	Out->SetArrayField(TEXT("warnings"), WarningValues);
	return MakeShared<FJsonValueObject>(Out);
}
} // namespace

void LilaLink::AddSplineMethods(TMap<FString, FLilaLinkMethod>& Methods)
{
	Methods.Add(TEXT("setSpline"), LilaLink::Sync(&SetSpline));
	Methods.Add(TEXT("removeSpline"), LilaLink::Sync(&RemoveSpline));
}

namespace
{
/** Loads actors of a World Partition level by guid; they stay loaded until its World Partition shuts down. */
void LoadActorGuids(UWorld* World, UWorldPartition* Partition, const TArray<FGuid>& Guids)
{
	// Released when its level's World Partition shuts down, before the world goes (as LoadLilaActors' loader).
	static TWeakObjectPtr<UWorldPartition> Owner;
	static TUniquePtr<FLoaderAdapterActorList> Loader;
	if (!Loader || Owner.Get() != Partition)
	{
		Loader = MakeUnique<FLoaderAdapterActorList>(World);
		Owner = Partition;
		TWeakObjectPtr<UWorldPartition> Bound = Partition;
		World->OnWorldPartitionUninitialized().AddLambda([Bound](UWorldPartition* Gone) {
			if (Gone == Bound.Get() && Owner.Get() == Gone)
			{
				Loader.Reset();
				Owner.Reset();
			}
		});
	}
	Loader->AddActors(Guids);
}
} // namespace

int32 LilaLink::LoadLandscapeUnder(UWorld* World, const FBox& Box)
{
	TRACE_CPUPROFILER_EVENT_SCOPE(LilaLinkSplines::LoadLandscapeUnder);

	UWorldPartition* Partition = World ? World->GetWorldPartition() : nullptr;
	if (!Partition || !Box.IsValid)
	{
		return 0;
	}
	const FBox Column(FVector(Box.Min.X, Box.Min.Y, -UE_OLD_WORLD_MAX), FVector(Box.Max.X, Box.Max.Y, UE_OLD_WORLD_MAX));
	TArray<FGuid> Guids;
	FWorldPartitionHelpers::ForEachActorDescInstance(Partition, ALandscapeStreamingProxy::StaticClass(), [&](const FWorldPartitionActorDescInstance* Desc) {
		if (!Desc->IsLoaded() && Desc->GetEditorBounds().Intersect(Column))
		{
			Guids.Add(Desc->GetGuid());
		}
		return true;
	});
	if (Guids.IsEmpty())
	{
		return 0;
	}
	LoadActorGuids(World, Partition, Guids);
	UE_LOG(LogLilaLink, Log, TEXT("Loaded %d landscape region(s) under splines so their redraw reaches them"), Guids.Num());
	return Guids.Num();
}

int32 LilaLink::LoadSplineActors(UWorld* World)
{
	TRACE_CPUPROFILER_EVENT_SCOPE(LilaLinkSplines::LoadSplineActors);

	UWorldPartition* Partition = World ? World->GetWorldPartition() : nullptr;
	if (!Partition)
	{
		return 0;
	}
	TArray<FGuid> Guids;
	FWorldPartitionHelpers::ForEachActorDescInstance(Partition, ALandscapeSplineActor::StaticClass(), [&](const FWorldPartitionActorDescInstance* Desc) {
		if (!Desc->IsLoaded())
		{
			Guids.Add(Desc->GetGuid());
		}
		return true;
	});
	if (!Guids.IsEmpty())
	{
		LoadActorGuids(World, Partition, Guids);
		UE_LOG(LogLilaLink, Log, TEXT("Loaded %d landscape spline actor(s)"), Guids.Num());
	}
	return Guids.Num();
}

FBox LilaLink::SplineBounds(const ULandscapeSplinesComponent* Component, const FString& Prefix)
{
	// Built from the saved point settings, since the computed spline bounds are empty until the points are updated.
	FBox Box(ForceInit);
	if (!Component)
	{
		return Box;
	}
	const FTransform ToWorld = Component->GetComponentTransform();
	for (const ULandscapeSplineControlPoint* Point : Component->GetControlPoints())
	{
		if (Point && (Prefix.IsEmpty() || Point->GetName().StartsWith(Prefix)))
		{
			const double Reach = (Point->Width + Point->SideFalloff + Point->EndFalloff) * ToWorld.GetMaximumAxisScale();
			Box += FBox::BuildAABB(ToWorld.TransformPosition(Point->Location), FVector(Reach));
		}
	}
	return Box;
}

void LilaLink::RedrawSplineLayer(ALandscape* Landscape, bool bAddLayer, TArray<FString>& Warnings, TArray<FString>* LayersRemoved)
{
	TRACE_CPUPROFILER_EVENT_SCOPE(LilaLinkSplines::RedrawSplineLayer);

	TArray<FString> Removed;
	TArray<ULandscapeLayerInfoObject*> RemovedInfos;
	DropUnusedPaintLayers(Landscape, Removed, RemovedInfos, Warnings);
	if (LayersRemoved)
	{
		LayersRemoved->Append(Removed);
	}
	if (bAddLayer && !Landscape->FindEditLayerOfTypeConst(ULandscapeEditLayerSplines::StaticClass()) &&
		Landscape->CreateLayer(FName(TEXT("Splines")), ULandscapeEditLayerSplines::StaticClass()) == INDEX_NONE)
	{
		Warnings.Add(TEXT("could not add a Splines edit layer to the landscape, so the splines neither paint nor deform it"));
	}
	Landscape->RequestSplineLayerUpdate();
	// Redrawn now rather than on a later tick, so the regions it changed can be marked dirty: the editor only records
	// automatic landscape changes as modified, and Save All would leave them out.
	Landscape->ForceUpdateLayersContent();
	if (ULandscapeInfo* Info = Landscape->GetLandscapeInfo())
	{
		if (!RemovedInfos.IsEmpty())
		{
			// Regions that painted a removed layer keep material instances built for it until they are made again.
			Info->UpdateAllComponentMaterialInstances(/*bInInvalidateCombinationMaterials=*/true);
			RefreshCollisionLayers(Info, RemovedInfos);
		}
		Info->MarkModifiedPackagesAsDirty();
	}
}
