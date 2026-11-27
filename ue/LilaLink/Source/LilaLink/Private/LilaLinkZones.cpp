#include "LilaLinkZones.h"

#include "LilaLinkLog.h"
#include "LilaLinkAssets.h"
#include "LilaLinkPlacements.h"
#include "LilaZone.h"

#include "AssetRegistry/AssetRegistryModule.h"
#include "AssetRegistry/IAssetRegistry.h"
#include "AssetToolsModule.h"
#include "BSPOps.h"
#include "Components/BrushComponent.h"
#include "DataLayer/DataLayerEditorSubsystem.h"
#include "Dom/JsonObject.h"
#include "Editor.h"
#include "Engine/Polys.h"
#include "Engine/World.h"
#include "EngineUtils.h"
#include "Misc/Base64.h"
#include "Misc/Crc.h"
#include "Misc/PackageName.h"
#include "Misc/Paths.h"
#include "Model.h"
#include "ObjectTools.h"
#include "ProfilingDebugging/CpuProfilerTrace.h"
#include "ScopedTransaction.h"
#include "Serialization/JsonSerializer.h"
#include "Serialization/JsonWriter.h"
#include "UObject/Package.h"
#include "WorldPartition/DataLayer/DataLayerAsset.h"
#include "WorldPartition/DataLayer/DataLayerInstance.h"

namespace
{
/** On every volume this file makes. */
const FName ZoneTag(TEXT("LilaZone"));

FName SetTag(const FString& Id)
{
	return FName(*FString::Printf(TEXT("LilaLink.Zones:%08X"), FCrc::StrCrc32(*Id)));
}

FString KeyOf(const FString& Layer, const FString& Zone)
{
	return Layer + TEXT("/") + Zone;
}

FName KeyTag(const FString& Key)
{
	return FName(*(TEXT("LilaLink.Zone:") + Key.Right(900)));
}

FString KeyFromTags(const AActor* Actor)
{
	const FString Prefix = TEXT("LilaLink.Zone:");
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

/** The level an external actor package belongs to (/Game/__ExternalActors__/Maps/A/1/AB/X: /Game/Maps/A), else Package. */
FString LevelOfPackage(const FString& Package)
{
	const FString Marker = TEXT("/__ExternalActors__");
	const int32 At = Package.Find(Marker);
	if (At == INDEX_NONE)
	{
		return Package;
	}
	FString Level = Package.Left(At) + Package.Mid(At + Marker.Len());
	for (int32 Up = 0; Up < 3; ++Up)
	{
		Level = FPaths::GetPath(Level);
	}
	return Level;
}

UWorld* ZonesEditorWorld()
{
	return GEditor ? GEditor->GetEditorWorldContext().World() : nullptr;
}

bool ReadVector(const TSharedPtr<FJsonValue>& Value, FVector& Out)
{
	const TArray<TSharedPtr<FJsonValue>>* A = nullptr;
	if (!Value.IsValid() || !Value->TryGetArray(A) || A->Num() != 3)
	{
		return false;
	}
	Out = FVector((*A)[0]->AsNumber(), (*A)[1]->AsNumber(), (*A)[2]->AsNumber());
	return !Out.ContainsNaN();
}

/** [[x, y, z], ...] rings into XY rings. */
TArray<FLilaZoneRing> ReadRings(const TSharedPtr<FJsonObject>& Zone, const TCHAR* Field)
{
	TArray<FLilaZoneRing> Out;
	const TArray<TSharedPtr<FJsonValue>>* Rings = nullptr;
	if (!Zone->TryGetArrayField(Field, Rings))
	{
		return Out;
	}
	for (const TSharedPtr<FJsonValue>& RingValue : *Rings)
	{
		const TArray<TSharedPtr<FJsonValue>>* Points = nullptr;
		if (!RingValue.IsValid() || !RingValue->TryGetArray(Points))
		{
			continue;
		}
		FLilaZoneRing& Ring = Out.AddDefaulted_GetRef();
		for (const TSharedPtr<FJsonValue>& P : *Points)
		{
			FVector V;
			if (ReadVector(P, V))
			{
				Ring.Points.Add(FVector2D(V.X, V.Y));
			}
		}
	}
	return Out;
}

/** Attributes as text per key (strings as they are, the rest as JSON) and as one JSON object. */
void ReadAttributes(const TSharedPtr<FJsonObject>& Zone, TMap<FName, FString>& Out, FString& OutJson)
{
	Out.Reset();
	OutJson = TEXT("{}");
	const TSharedPtr<FJsonObject>* Attrs = nullptr;
	if (!Zone->TryGetObjectField(TEXT("attrs"), Attrs))
	{
		return;
	}
	for (const auto& [Key, Value] : (*Attrs)->Values)
	{
		FString Text;
		if (Value->Type == EJson::String)
		{
			Text = Value->AsString();
		}
		else
		{
			const TSharedRef<TJsonWriter<TCHAR, TCondensedJsonPrintPolicy<TCHAR>>> Writer = TJsonWriterFactory<TCHAR, TCondensedJsonPrintPolicy<TCHAR>>::Create(&Text);
			FJsonSerializer::Serialize(Value, TEXT(""), Writer);
			Text.TrimStartAndEndInline();
		}
		Out.Add(FName(*Key), Text);
	}
	const TSharedRef<TJsonWriter<TCHAR, TCondensedJsonPrintPolicy<TCHAR>>> Writer = TJsonWriterFactory<TCHAR, TCondensedJsonPrintPolicy<TCHAR>>::Create(&OutJson);
	FJsonSerializer::Serialize(Attrs->ToSharedRef(), Writer);
}

FLilaZoneMask ReadMask(const TSharedPtr<FJsonObject>& Zone)
{
	FLilaZoneMask Mask;
	const TSharedPtr<FJsonObject>* M = nullptr;
	if (!Zone->TryGetObjectField(TEXT("mask"), M))
	{
		return Mask;
	}
	ReadVector((*M)->TryGetField(TEXT("origin")), Mask.Origin);
	ReadVector((*M)->TryGetField(TEXT("axisU")), Mask.AxisU);
	ReadVector((*M)->TryGetField(TEXT("axisV")), Mask.AxisV);
	const TArray<TSharedPtr<FJsonValue>>* Size = nullptr;
	if ((*M)->TryGetArrayField(TEXT("size"), Size) && Size->Num() == 2)
	{
		Mask.Size = FIntPoint(int32((*Size)[0]->AsNumber()), int32((*Size)[1]->AsNumber()));
	}
	FString Values;
	if ((*M)->TryGetStringField(TEXT("values"), Values) && FBase64::Decode(Values, Mask.Values) && Mask.Values.Num() == Mask.Size.X * Mask.Size.Y)
	{
		return Mask;
	}
	return FLilaZoneMask();
}

/** A face of the brush, wound so its normal points away from Centre. */
void AddFace(UModel* Brush, ABrush* Owner, const TArray<FVector>& Verts, const FVector& Centre)
{
	FPoly Poly;
	Poly.Init();
	for (const FVector& V : Verts)
	{
		Poly.Vertices.Add(FVector3f(V));
	}
	Poly.Base = Poly.Vertices[0];
	if (Poly.Finalize(Owner, 1) != 0)
	{
		return;
	}
	FVector3f Mid = FVector3f::ZeroVector;
	for (const FVector3f& V : Poly.Vertices)
	{
		Mid += V;
	}
	Mid /= float(Poly.Vertices.Num());
	if ((FVector3f(Centre) - Mid | Poly.Normal) > 0.0f)
	{
		Poly.Reverse();
		Poly.Finalize(Owner, 1);
	}
	Brush->Polys->Element.Add(Poly);
}

/** The volume's brush: each convex piece extruded from Bottom to Top, in the actor's local space. */
void BuildBrush(ALilaZoneVolume* Volume, const TArray<TArray<FVector2D>>& Pieces, float Bottom, float Top, const FVector& Location)
{
	TRACE_CPUPROFILER_EVENT_SCOPE(LilaLinkZones::BuildBrush);
	Volume->PreEditChange(nullptr);
	UModel* Brush = Volume->Brush;
	if (!Brush)
	{
		Brush = NewObject<UModel>(Volume, NAME_None, RF_Transactional);
		Brush->Initialize(nullptr, true);
		Volume->Brush = Brush;
	}
	Brush->Modify();
	if (!Brush->Polys)
	{
		Brush->Polys = NewObject<UPolys>(Brush, NAME_None, RF_Transactional);
	}
	Brush->Polys->Element.Empty();
	for (const TArray<FVector2D>& Piece : Pieces)
	{
		if (Piece.Num() < 3)
		{
			continue;
		}
		FVector Centre = FVector::ZeroVector;
		for (const FVector2D& P : Piece)
		{
			Centre += FVector(P.X, P.Y, (Bottom + Top) * 0.5f);
		}
		Centre = Centre / double(Piece.Num()) - Location;
		TArray<FVector> Low, High;
		for (const FVector2D& P : Piece)
		{
			Low.Add(FVector(P.X, P.Y, Bottom) - Location);
			High.Add(FVector(P.X, P.Y, Top) - Location);
		}
		AddFace(Brush, Volume, Low, Centre);
		AddFace(Brush, Volume, High, Centre);
		for (int32 I = 0; I < Piece.Num(); ++I)
		{
			const int32 J = (I + 1) % Piece.Num();
			AddFace(Brush, Volume, {Low[I], Low[J], High[J], High[I]}, Centre);
		}
	}
	Brush->Linked = false;
	Brush->BuildBound();
	Volume->GetBrushComponent()->Brush = Brush;
	FBSPOps::csgPrepMovingBrush(Volume);
	Volume->GetBrushComponent()->RequestUpdateBrushCollision();
	Volume->PostEditChange();
}

ULilaZoneData* ZoneAsset(const FString& Folder, const FString& Label)
{
	const FString AssetName = ObjectTools::SanitizeObjectName(TEXT("ZD_") + Label);
	const FString PackageName = Folder / AssetName;
	const FString ObjectPath = PackageName + TEXT(".") + AssetName;
	UObject* Existing = FindObject<UObject>(nullptr, *ObjectPath);
	if (!Existing && FPackageName::DoesPackageExist(PackageName))
	{
		Existing = LoadObject<UObject>(nullptr, *ObjectPath, nullptr, LOAD_NoWarn | LOAD_Quiet);
	}
	if (Existing && !Existing->IsA<ULilaZoneData>())
	{
		return nullptr;
	}
	ULilaZoneData* Asset = Cast<ULilaZoneData>(Existing);
	if (!Asset)
	{
		UPackage* Package = CreatePackage(*PackageName);
		Asset = NewObject<ULilaZoneData>(Package, FName(*AssetName), RF_Public | RF_Standalone | RF_Transactional);
		FAssetRegistryModule::AssetCreated(Asset);
	}
	Asset->Modify();
	return Asset;
}

/** The data layer instance for a zone layer, its asset made under Folder when missing; null with Why outside World Partition. */
UDataLayerInstance* ZoneDataLayer(UWorld* World, const FString& Folder, const FString& Name, FString& Why)
{
	if (!World->IsPartitionedWorld())
	{
		Why = TEXT("skipped: the level is not a World Partition level");
		return nullptr;
	}
	UDataLayerEditorSubsystem* Layers = UDataLayerEditorSubsystem::Get();
	if (!Layers)
	{
		Why = TEXT("skipped: no data layer editor subsystem");
		return nullptr;
	}
	const FString AssetName = ObjectTools::SanitizeObjectName(TEXT("DL_") + Name);
	const FString PackageName = Folder / TEXT("DataLayers") / AssetName;
	const FString ObjectPath = PackageName + TEXT(".") + AssetName;
	UDataLayerAsset* Asset = FindObject<UDataLayerAsset>(nullptr, *ObjectPath);
	if (!Asset && FPackageName::DoesPackageExist(PackageName))
	{
		Asset = LoadObject<UDataLayerAsset>(nullptr, *ObjectPath, nullptr, LOAD_NoWarn | LOAD_Quiet);
	}
	if (!Asset)
	{
		UPackage* Package = CreatePackage(*PackageName);
		Asset = NewObject<UDataLayerAsset>(Package, FName(*AssetName), RF_Public | RF_Standalone | RF_Transactional);
		FAssetRegistryModule::AssetCreated(Asset);
		Asset->MarkPackageDirty();
	}
	if (UDataLayerInstance* Existing = Layers->GetDataLayerInstance(Asset))
	{
		return Existing;
	}
	FDataLayerCreationParameters Params;
	Params.DataLayerAsset = Asset;
	Params.WorldDataLayers = World->GetWorldDataLayers();
	UDataLayerInstance* Instance = Layers->CreateDataLayerInstance(Params);
	if (!Instance)
	{
		Why = TEXT("failed: could not make data layer ") + AssetName;
	}
	return Instance;
}

/** The data layers made under Folder/DataLayers (Lila's own) that these volumes are on. */
TArray<const UDataLayerAsset*> LilaDataLayersOf(const TArray<AActor*>& Actors, const FString& Folder)
{
	const FString Prefix = Folder / TEXT("DataLayers/");
	TArray<const UDataLayerAsset*> Out;
	for (const AActor* Actor : Actors)
	{
		for (const UDataLayerAsset* Asset : Actor->GetDataLayerAssets())
		{
			if (Asset && Asset->GetPathName().StartsWith(Prefix))
			{
				Out.AddUnique(Asset);
			}
		}
	}
	return Out;
}

TSharedPtr<FJsonValue> SetZones(const TSharedPtr<FJsonObject>& Params, FString& OutError)
{
	TRACE_CPUPROFILER_EVENT_SCOPE(LilaLinkZones::SetZones);

	FString Id;
	if (!Params->TryGetStringField(TEXT("id"), Id) || Id.IsEmpty())
	{
		OutError = TEXT("setZones: give id");
		return nullptr;
	}
	UWorld* World = ZonesEditorWorld();
	if (!World)
	{
		OutError = TEXT("setZones: no editor world");
		return nullptr;
	}
	const TArray<TSharedPtr<FJsonValue>>* LayersJson = nullptr;
	if (!Params->TryGetArrayField(TEXT("layers"), LayersJson))
	{
		OutError = TEXT("setZones: give layers");
		return nullptr;
	}
	FString Label = TEXT("Zones");
	Params->TryGetStringField(TEXT("label"), Label);
	FString Folder = TEXT("Lila/Zones/") + Label;
	Params->TryGetStringField(TEXT("folder"), Folder);
	FString AssetFolder = TEXT("/Game/Lila/Zones");
	Params->TryGetStringField(TEXT("asset"), AssetFolder);
	bool bDataLayers = true;
	Params->TryGetBoolField(TEXT("dataLayers"), bDataLayers);
	const FName Set = SetTag(Id);

	TMap<FString, ALilaZoneVolume*> Existing;
	for (TActorIterator<ALilaZoneVolume> It(World); It; ++It)
	{
		if (It->ActorHasTag(Set))
		{
			Existing.Add(KeyFromTags(*It), *It);
		}
	}

	FScopedTransaction Transaction(NSLOCTEXT("LilaLink", "SetZones", "Lila: Update Zones"));
	ULilaZoneData* Asset = ZoneAsset(AssetFolder, Label);
	if (!Asset)
	{
		OutError = FString::Printf(TEXT("setZones: %s/ZD_%s exists and is not a Lila zone data asset"), *AssetFolder, *Label);
		Transaction.Cancel();
		return nullptr;
	}
	Asset->SetName = Label;
	Asset->Layers.Reset();

	TSet<FString> Sent;
	int32 Created = 0, Updated = 0, Unchanged = 0;
	TMap<FString, TArray<AActor*>> ByDataLayer;
	for (const TSharedPtr<FJsonValue>& LayerValue : *LayersJson)
	{
		const TSharedPtr<FJsonObject>* LayerPtr = nullptr;
		if (!LayerValue.IsValid() || !LayerValue->TryGetObject(LayerPtr))
		{
			continue;
		}
		const TSharedPtr<FJsonObject>& LayerJson = *LayerPtr;
		FLilaZoneLayer& Layer = Asset->Layers.AddDefaulted_GetRef();
		FString LayerId, Type, DataLayer;
		LayerJson->TryGetStringField(TEXT("id"), LayerId);
		LayerJson->TryGetStringField(TEXT("type"), Type);
		LayerJson->TryGetStringField(TEXT("label"), Layer.Label);
		LayerJson->TryGetStringField(TEXT("dataLayer"), DataLayer);
		LayerJson->TryGetNumberField(TEXT("priority"), Layer.Priority);
		LayerJson->TryGetBoolField(TEXT("density"), Layer.bDensity);
		LayerJson->TryGetBoolField(TEXT("exclusive"), Layer.bExclusive);
		Layer.Id = FName(*LayerId);
		Layer.Type = FName(*Type);
		Layer.DataLayer = DataLayer.IsEmpty() ? Layer.Label : DataLayer;
		const TArray<TSharedPtr<FJsonValue>>* Color = nullptr;
		if (LayerJson->TryGetArrayField(TEXT("color"), Color) && Color->Num() >= 3)
		{
			Layer.Color = FLinearColor(float((*Color)[0]->AsNumber()), float((*Color)[1]->AsNumber()), float((*Color)[2]->AsNumber()));
		}

		const TArray<TSharedPtr<FJsonValue>>* ZonesJson = nullptr;
		if (!LayerJson->TryGetArrayField(TEXT("zones"), ZonesJson))
		{
			continue;
		}
		for (const TSharedPtr<FJsonValue>& ZoneValue : *ZonesJson)
		{
			const TSharedPtr<FJsonObject>* ZonePtr = nullptr;
			if (!ZoneValue.IsValid() || !ZoneValue->TryGetObject(ZonePtr))
			{
				continue;
			}
			const TSharedPtr<FJsonObject>& ZoneJson = *ZonePtr;
			FLilaZoneInfo& Zone = Layer.Zones.AddDefaulted_GetRef();
			FString ZoneId;
			ZoneJson->TryGetStringField(TEXT("id"), ZoneId);
			ZoneJson->TryGetStringField(TEXT("name"), Zone.Name);
			ZoneJson->TryGetNumberField(TEXT("priority"), Zone.Priority);
			double Bottom = 0.0, Top = 0.0;
			ZoneJson->TryGetNumberField(TEXT("bottom"), Bottom);
			ZoneJson->TryGetNumberField(TEXT("top"), Top);
			Zone.Id = FName(*ZoneId);
			Zone.Bottom = float(FMath::Min(Bottom, Top));
			Zone.Top = float(FMath::Max(Bottom, Top));
			ReadAttributes(ZoneJson, Zone.Attributes, Zone.AttributesJson);
			Zone.Outline = ReadRings(ZoneJson, TEXT("outline"));
			Zone.Holes = ReadRings(ZoneJson, TEXT("holes"));
			Zone.Mask = ReadMask(ZoneJson);
			TArray<TArray<FVector2D>> Pieces;
			for (const FLilaZoneRing& Piece : ReadRings(ZoneJson, TEXT("pieces")))
			{
				Pieces.Add(Piece.Points);
			}

			const FString Key = KeyOf(LayerId, ZoneId);
			if (ZoneId.IsEmpty() || Sent.Contains(Key) || Pieces.IsEmpty())
			{
				continue;
			}
			Sent.Add(Key);
			FBox2D Bounds(ForceInit);
			for (const TArray<FVector2D>& Piece : Pieces)
			{
				for (const FVector2D& P : Piece)
				{
					Bounds += P;
				}
			}
			const FVector Location(Bounds.GetCenter().X, Bounds.GetCenter().Y, Zone.Bottom);

			ALilaZoneVolume** Found = Existing.Find(Key);
			ALilaZoneVolume* Volume = Found ? *Found : nullptr;
			// What this zone's volume is made from: a volume already carrying it keeps its brush.
			FString ZoneText;
			{
				TSharedRef<TJsonWriter<TCHAR, TCondensedJsonPrintPolicy<TCHAR>>> Writer = TJsonWriterFactory<TCHAR, TCondensedJsonPrintPolicy<TCHAR>>::Create(&ZoneText);
				FJsonSerializer::Serialize(ZoneJson.ToSharedRef(), Writer);
			}
			ZoneText += Label + Folder + Layer.Label + Type + Layer.DataLayer + Layer.Color.ToString() + Asset->GetPathName();
			const FName HashTag(*FString::Printf(TEXT("LilaHash:%08x%08x"), FCrc::StrCrc32(*ZoneText), GetTypeHash(ZoneText)));
			if (Volume && Volume->Tags.Contains(HashTag))
			{
				++Unchanged;
				ByDataLayer.FindOrAdd(Layer.DataLayer).Add(Volume);
				continue;
			}
			if (!Volume)
			{
				FActorSpawnParameters Spawn;
				Spawn.ObjectFlags |= RF_Transactional;
				Volume = World->SpawnActor<ALilaZoneVolume>(ALilaZoneVolume::StaticClass(), FTransform(Location), Spawn);
				if (!Volume)
				{
					OutError = TEXT("setZones: could not spawn a zone volume");
					Transaction.Cancel();
					return nullptr;
				}
				Volume->Tags.Add(ZoneTag);
				Volume->Tags.Add(Set);
				Volume->Tags.Add(KeyTag(Key));
				++Created;
			}
			else
			{
				Volume->Modify();
				++Updated;
			}
			Volume->SetActorLocation(Location);
			Volume->SetActorLabel(Label + TEXT("_") + LayerId + TEXT("_") + ZoneId);
			Volume->SetFolderPath(FName(*(Folder / Layer.Label)));
			Volume->Tags.RemoveAll([](const FName& Tag) { return Tag.ToString().StartsWith(TEXT("LilaZone."), ESearchCase::IgnoreCase); });
			Volume->Tags.Add(FName(*(TEXT("LilaZone.") + Type)));
			Volume->Tags.Add(FName(*(TEXT("LilaZone.") + LayerId)));
			Volume->ZoneId = Zone.Id;
			Volume->LayerId = Layer.Id;
			Volume->LayerType = Layer.Type;
			Volume->ZoneName = Zone.Name;
			Volume->Priority = Zone.Priority;
			Volume->Attributes = Zone.Attributes;
			Volume->AttributesJson = Zone.AttributesJson;
			Volume->ZoneData = Asset;
			Volume->BrushColor = Layer.Color.ToFColor(true);
			BuildBrush(Volume, Pieces, Zone.Bottom, Zone.Top, Location);
			Volume->Tags.RemoveAll([](const FName& Tag) { return Tag.ToString().StartsWith(TEXT("LilaHash:")); });
			Volume->Tags.Add(HashTag);
			Volume->MarkPackageDirty();
			ByDataLayer.FindOrAdd(Layer.DataLayer).Add(Volume);
		}
	}
	Asset->MarkPackageDirty();

	int32 Removed = 0;
	TArray<AActor*> Gone;
	for (const auto& [Key, Volume] : Existing)
	{
		if (!Sent.Contains(Key) && IsValid(Volume))
		{
			Gone.Add(Volume);
		}
	}
	const TArray<const UDataLayerAsset*> GoneLayers = LilaDataLayersOf(Gone, AssetFolder);
	for (AActor* Volume : Gone)
	{
		Removed += World->EditorDestroyActor(Volume, true) ? 1 : 0;
	}
	// A layer whose last zone went takes its data layer with it.
	TArray<FString> Errors;
	const int32 LayersRemoved = GoneLayers.IsEmpty() ? 0 : LilaLink::DeleteUnusedDataLayers(World, GoneLayers, Errors);

	TSharedRef<FJsonObject> DataLayers = MakeShared<FJsonObject>();
	if (bDataLayers)
	{
		UDataLayerEditorSubsystem* Subsystem = UDataLayerEditorSubsystem::Get();
		for (const auto& [Name, Actors] : ByDataLayer)
		{
			FString Why;
			UDataLayerInstance* Instance = ZoneDataLayer(World, AssetFolder, Name, Why);
			if (Instance && Subsystem)
			{
				Subsystem->AddActorsToDataLayer(Actors, Instance);
				DataLayers->SetStringField(Name, Instance->GetDataLayerShortName());
			}
			else
			{
				DataLayers->SetStringField(Name, Why);
			}
		}
	}

	UE_LOG(LogLilaLink, Log, TEXT("setZones: %s (%d created, %d updated, %d unchanged, %d removed)"), *Label, Created, Updated, Unchanged, Removed);
	TSharedRef<FJsonObject> Out = MakeShared<FJsonObject>();
	Out->SetNumberField(TEXT("volumes"), Sent.Num());
	Out->SetNumberField(TEXT("created"), Created);
	Out->SetNumberField(TEXT("updated"), Updated);
	Out->SetNumberField(TEXT("unchanged"), Unchanged);
	Out->SetNumberField(TEXT("removed"), Removed);
	Out->SetStringField(TEXT("asset"), Asset->GetPathName());
	Out->SetObjectField(TEXT("dataLayers"), DataLayers);
	Out->SetNumberField(TEXT("dataLayersRemoved"), LayersRemoved);
	if (!Errors.IsEmpty())
	{
		Out->SetStringField(TEXT("warning"), Errors[0]);
	}
	return MakeShared<FJsonValueObject>(Out);
}

TSharedPtr<FJsonValue> RemoveZones(const TSharedPtr<FJsonObject>& Params, FString& OutError)
{
	TRACE_CPUPROFILER_EVENT_SCOPE(LilaLinkZones::RemoveZones);

	FString Id;
	if (!Params->TryGetStringField(TEXT("id"), Id) || Id.IsEmpty())
	{
		OutError = TEXT("removeZones: give id");
		return nullptr;
	}
	UWorld* World = ZonesEditorWorld();
	if (!World)
	{
		OutError = TEXT("removeZones: no editor world");
		return nullptr;
	}
	FString Label, AssetFolder = TEXT("/Game/Lila/Zones");
	Params->TryGetStringField(TEXT("label"), Label);
	Params->TryGetStringField(TEXT("asset"), AssetFolder);
	FScopedTransaction Transaction(NSLOCTEXT("LilaLink", "RemoveZones", "Lila: Remove Zones"));
	const FName Set = SetTag(Id);
	TArray<AActor*> Ours;
	for (TActorIterator<ALilaZoneVolume> It(World); It; ++It)
	{
		if (It->ActorHasTag(Set))
		{
			Ours.Add(*It);
		}
	}
	const TArray<const UDataLayerAsset*> Layers = LilaDataLayersOf(Ours, AssetFolder);
	int32 Removed = 0;
	for (AActor* Actor : Ours)
	{
		Removed += World->EditorDestroyActor(Actor, true) ? 1 : 0;
	}
	TArray<FString> Errors;
	const int32 LayersRemoved = Layers.IsEmpty() ? 0 : LilaLink::DeleteUnusedDataLayers(World, Layers, Errors);
	// The set's zone data asset, which game code queries, goes with its volumes, unless another level's volumes name it:
	// sends of the set to several levels share it.
	bool bAssetDeleted = false;
	if (!Label.IsEmpty())
	{
		const FString AssetName = ObjectTools::SanitizeObjectName(TEXT("ZD_") + Label);
		const FString PackageName = AssetFolder / AssetName;
		if (FPackageName::DoesPackageExist(PackageName) || FindPackage(nullptr, *PackageName))
		{
			if (const FString User = LilaLink::UsedOutside(World, PackageName); !User.IsEmpty())
			{
				Errors.Add(TEXT("zone data kept: another level uses it (") + LevelOfPackage(User) + TEXT(")"));
			}
			else if (UObject* Asset = LoadObject<UObject>(nullptr, *(PackageName + TEXT(".") + AssetName), nullptr, LOAD_NoWarn | LOAD_Quiet))
			{
				bAssetDeleted = LilaLink::ForceDeleteAssets({Asset}) > 0;
				if (!bAssetDeleted)
				{
					Errors.Add(PackageName + TEXT(" could not be deleted"));
				}
			}
		}
	}
	UE_LOG(LogLilaLink, Log, TEXT("removeZones: %d volumes, %d data layers, zone data %s"), Removed, LayersRemoved, bAssetDeleted ? TEXT("deleted") : TEXT("kept"));
	TSharedRef<FJsonObject> Out = MakeShared<FJsonObject>();
	Out->SetNumberField(TEXT("removed"), Removed);
	Out->SetNumberField(TEXT("dataLayers"), LayersRemoved);
	Out->SetBoolField(TEXT("zoneData"), bAssetDeleted);
	if (!Errors.IsEmpty())
	{
		Out->SetStringField(TEXT("warning"), FString::Join(Errors, TEXT("; ")));
	}
	return MakeShared<FJsonValueObject>(Out);
}
} // namespace

void LilaLink::AddZoneMethods(TMap<FString, FLilaLinkMethod>& Methods)
{
	Methods.Add(TEXT("setZones"), LilaLink::Sync(&SetZones));
	Methods.Add(TEXT("removeZones"), LilaLink::Sync(&RemoveZones));
}
