#include "LilaLinkSettlement.h"

#include "LilaLinkMethodRegistry.h"
#include "LilaLinkSettlementTraffic.h"

#include "AssetRegistry/AssetRegistryModule.h"
#include "Components/SplineComponent.h"
#include "Curve/GeneralPolygon2.h"
#include "Data/PCGPointArrayData.h"
#include "Data/PCGPolygon2DData.h"
#include "Data/PCGSplineData.h"
#include "Dom/JsonObject.h"
#include "Editor.h"
#include "Engine/World.h"
#include "EngineUtils.h"
#include "Metadata/PCGMetadata.h"
#include "PCGParamData.h"
#include "UObject/UObjectIterator.h"
#include "Metadata/PCGMetadataAttributeTpl.h"
#include "PCGDataAsset.h"
#include "Polygon2.h"
#include "ProfilingDebugging/CpuProfilerTrace.h"
#include "ScopedTransaction.h"
#include "Misc/PackageName.h"
#include "UObject/Package.h"
#include "UObject/UnrealType.h"
#include "ZoneGraphDelegates.h"
#include "ZoneGraphSettings.h"
#include "ZoneGraphTypes.h"
#include "ZoneShapeComponent.h"

DEFINE_LOG_CATEGORY_STATIC(LogLilaLinkSettlement, Log, All);

namespace
{
const FName LilaTag(TEXT("LilaLink"));

FName ZoneTagFor(const FString& Id)
{
	return FName(*(TEXT("LilaLink.Settlement:") + Id.Right(900)));
}

UWorld* EditorWorld()
{
	return GEditor ? GEditor->GetEditorWorldContext().World() : nullptr;
}

FVector ReadPoint(const TSharedPtr<FJsonValue>& Value)
{
	const TArray<TSharedPtr<FJsonValue>>* Xyz = nullptr;
	if (!Value.IsValid() || !Value->TryGetArray(Xyz) || Xyz->Num() != 3)
	{
		return FVector::ZeroVector;
	}
	return FVector((*Xyz)[0]->AsNumber(), (*Xyz)[1]->AsNumber(), (*Xyz)[2]->AsNumber());
}

TArray<FVector> ReadPoints(const TSharedPtr<FJsonObject>& Object, const TCHAR* Field)
{
	TArray<FVector> Out;
	const TArray<TSharedPtr<FJsonValue>>* Values = nullptr;
	if (Object->TryGetArrayField(Field, Values))
	{
		for (const TSharedPtr<FJsonValue>& V : *Values)
		{
			Out.Add(ReadPoint(V));
		}
	}
	return Out;
}

const TArray<TSharedPtr<FJsonValue>>& ArrayOf(const TSharedPtr<FJsonObject>& Object, const TCHAR* Field)
{
	static const TArray<TSharedPtr<FJsonValue>> Empty;
	const TArray<TSharedPtr<FJsonValue>>* Values = nullptr;
	return Object->TryGetArrayField(Field, Values) ? *Values : Empty;
}

/** Writes every number, bool and string field of Object (but the skipped ones) as an attribute of the data's own domain. */
void SetDataAttributes(UPCGData* Data, const TSharedPtr<FJsonObject>& Object, const TSet<FString>& Skip)
{
	UPCGMetadata* Metadata = Data ? Data->MutableMetadata() : nullptr;
	if (!Metadata)
	{
		return;
	}
	for (const TPair<FString, TSharedPtr<FJsonValue>>& Field : Object->Values)
	{
		if (Skip.Contains(Field.Key) || !Field.Value.IsValid())
		{
			continue;
		}
		const FPCGAttributeIdentifier Name(FName(*Field.Key), PCGMetadataDomainID::Data);
		switch (Field.Value->Type)
		{
		case EJson::Number:
			Metadata->FindOrCreateAttribute<double>(Name, Field.Value->AsNumber(), false, false);
			break;
		case EJson::Boolean:
			Metadata->FindOrCreateAttribute<bool>(Name, Field.Value->AsBool(), false, false);
			break;
		case EJson::String:
			Metadata->FindOrCreateAttribute<FString>(Name, Field.Value->AsString(), false, false);
			break;
		default:
			break;
		}
	}
}

/** A polygon on the XY plane at the height of its first point, the rest of its height kept by the data's transform. */
UPCGPolygon2DData* MakePolygon(UObject* Outer, const TArray<FVector>& Points)
{
	TRACE_CPUPROFILER_EVENT_SCOPE(LilaLinkSettlement::MakePolygon);

	if (Points.Num() < 3)
	{
		return nullptr;
	}
	UE::Geometry::FPolygon2d Outer2;
	for (const FVector& P : Points)
	{
		Outer2.AppendVertex(FVector2d(P.X, P.Y));
	}
	UPCGPolygon2DData* Data = NewObject<UPCGPolygon2DData>(Outer);
	Data->SetPolygon(UE::Geometry::FGeneralPolygon2d(Outer2));
	Data->SetTransform(FTransform(FVector(0.0, 0.0, Points[0].Z)), true);
	return Data;
}

void AddTagged(UPCGDataAsset* Asset, UPCGData* Data, const TCHAR* Pin, const TCHAR* Tag)
{
	if (!Data)
	{
		return;
	}
	FPCGTaggedData& Tagged = Asset->Data.TaggedData.AddDefaulted_GetRef();
	Tagged.Data = Data;
	Tagged.Pin = FName(Pin);
	Tagged.Tags.Add(Tag);
}

/** One point per envelope: at the footprint's centre on the ground, facing the street, bounds the footprint and its height. */
UPCGPointArrayData* MakeEnvelopePoints(UObject* Outer, const TArray<TSharedPtr<FJsonValue>>& Envelopes)
{
	TRACE_CPUPROFILER_EVENT_SCOPE(LilaLinkSettlement::MakeEnvelopePoints);

	UPCGPointArrayData* Points = NewObject<UPCGPointArrayData>(Outer);
	Points->SetNumPoints(Envelopes.Num());
	UPCGMetadata* Metadata = Points->MutableMetadata();
	FPCGMetadataAttribute<int64>* Lot = Metadata->CreateAttribute<int64>(TEXT("Lot"), 0, false, false);
	FPCGMetadataAttribute<double>* MaxHeight = Metadata->CreateAttribute<double>(TEXT("MaxHeight"), 0.0, true, false);
	FPCGMetadataAttribute<double>* FloorHeight = Metadata->CreateAttribute<double>(TEXT("FloorHeight"), 0.0, true, false);
	FPCGMetadataAttribute<double>* Base = Metadata->CreateAttribute<double>(TEXT("Base"), 0.0, true, false);
	FPCGMetadataAttribute<int32>* Floors = Metadata->CreateAttribute<int32>(TEXT("Floors"), 0, false, false);
	FPCGMetadataAttribute<int32>* Era = Metadata->CreateAttribute<int32>(TEXT("Era"), 0, false, false);
	FPCGMetadataAttribute<FString>* Use = Metadata->CreateAttribute<FString>(TEXT("Use"), FString(), false, false);
	FPCGMetadataAttribute<FString>* Roof = Metadata->CreateAttribute<FString>(TEXT("Roof"), FString(), false, false);
	FPCGMetadataAttribute<FString>* Style = Metadata->CreateAttribute<FString>(TEXT("Style"), FString(), false, false);

	auto Transforms = Points->GetTransformValueRange();
	auto BoundsMin = Points->GetBoundsMinValueRange();
	auto BoundsMax = Points->GetBoundsMaxValueRange();
	auto Seeds = Points->GetSeedValueRange();
	auto Entries = Points->GetMetadataEntryValueRange();
	for (int32 Index = 0; Index < Envelopes.Num(); ++Index)
	{
		const TSharedPtr<FJsonObject> E = Envelopes[Index]->AsObject();
		if (!E.IsValid())
		{
			continue;
		}
		const FVector Center = ReadPoint(E->TryGetField(TEXT("center")));
		FVector Front = ReadPoint(E->TryGetField(TEXT("front")));
		Front.Z = 0.0;
		const FQuat Rotation = Front.IsNearlyZero() ? FQuat::Identity : FRotationMatrix::MakeFromX(Front.GetSafeNormal()).ToQuat();
		const FTransform Transform(Rotation, Center);
		FBox Local(ForceInit);
		for (const FVector& P : ReadPoints(E, TEXT("footprint")))
		{
			Local += Transform.InverseTransformPosition(P);
		}
		const double Height = E->GetNumberField(TEXT("maxHeight"));
		Transforms[Index] = Transform;
		BoundsMin[Index] = FVector(Local.Min.X, Local.Min.Y, 0.0);
		BoundsMax[Index] = FVector(Local.Max.X, Local.Max.Y, Height);
		Seeds[Index] = static_cast<int32>(E->GetNumberField(TEXT("seed")));
		const int64 Key = Metadata->AddEntry();
		Entries[Index] = Key;
		Lot->SetValue(Key, static_cast<int64>(E->GetNumberField(TEXT("lot"))));
		MaxHeight->SetValue(Key, Height);
		FloorHeight->SetValue(Key, E->GetNumberField(TEXT("floorHeight")));
		Base->SetValue(Key, E->GetNumberField(TEXT("base")));
		Floors->SetValue(Key, static_cast<int32>(E->GetNumberField(TEXT("floors"))));
		Era->SetValue(Key, static_cast<int32>(E->GetNumberField(TEXT("era"))));
		Use->SetValue(Key, E->GetStringField(TEXT("use")));
		Roof->SetValue(Key, E->GetStringField(TEXT("roof")));
		Style->SetValue(Key, E->GetStringField(TEXT("style")));
	}
	return Points;
}

/**
 * A point per entry at its position field, turned to face its forward field when it has one, bounds from length and
 * width (or radius) when given; every other number and string field an attribute.
 */
UPCGPointArrayData* MakeMarkerPoints(UObject* Outer, const TArray<TSharedPtr<FJsonValue>>& Entries, const TCHAR* PositionField, const TCHAR* ForwardField)
{
	TRACE_CPUPROFILER_EVENT_SCOPE(LilaLinkSettlement::MakeMarkerPoints);

	if (Entries.IsEmpty())
	{
		return nullptr;
	}
	UPCGPointArrayData* Points = NewObject<UPCGPointArrayData>(Outer);
	Points->SetNumPoints(Entries.Num());
	UPCGMetadata* Metadata = Points->MutableMetadata();
	auto Transforms = Points->GetTransformValueRange();
	auto BoundsMin = Points->GetBoundsMinValueRange();
	auto BoundsMax = Points->GetBoundsMaxValueRange();
	auto Keys = Points->GetMetadataEntryValueRange();
	for (int32 Index = 0; Index < Entries.Num(); ++Index)
	{
		const TSharedPtr<FJsonObject> E = Entries[Index]->AsObject();
		if (!E.IsValid())
		{
			continue;
		}
		FVector Forward = ForwardField ? ReadPoint(E->TryGetField(ForwardField)) : FVector::ZeroVector;
		Forward.Z = 0.0;
		const FQuat Rotation = Forward.IsNearlyZero() ? FQuat::Identity : FRotationMatrix::MakeFromX(Forward.GetSafeNormal()).ToQuat();
		Transforms[Index] = FTransform(Rotation, ReadPoint(E->TryGetField(PositionField)));
		double Length = 100.0, Width = 100.0;
		if (E->TryGetNumberField(TEXT("radius"), Length))
		{
			Width = Length;
		}
		E->TryGetNumberField(TEXT("length"), Length);
		E->TryGetNumberField(TEXT("width"), Width);
		BoundsMin[Index] = FVector(-0.5 * Length, -0.5 * Width, 0.0);
		BoundsMax[Index] = FVector(0.5 * Length, 0.5 * Width, 200.0);
		const int64 Key = Metadata->AddEntry();
		Keys[Index] = Key;
		for (const TPair<FString, TSharedPtr<FJsonValue>>& Field : E->Values)
		{
			if (!Field.Value.IsValid() || Field.Key == PositionField || (ForwardField && Field.Key == ForwardField))
			{
				continue;
			}
			const FName Name(*Field.Key);
			if (Field.Value->Type == EJson::Number)
			{
				Metadata->FindOrCreateAttribute<double>(Name, 0.0, true, false)->SetValue(Key, Field.Value->AsNumber());
			}
			else if (Field.Value->Type == EJson::String)
			{
				Metadata->FindOrCreateAttribute<FString>(Name, FString(), false, false)->SetValue(Key, Field.Value->AsString());
			}
		}
	}
	return Points;
}

UPCGSplineData* MakeRoad(UObject* Outer, const TArray<FVector>& Points)
{
	if (Points.Num() < 2)
	{
		return nullptr;
	}
	TArray<FSplinePoint> SplinePoints;
	for (int32 Index = 0; Index < Points.Num(); ++Index)
	{
		SplinePoints.Emplace(static_cast<float>(Index), Points[Index], ESplinePointType::Curve);
	}
	UPCGSplineData* Data = NewObject<UPCGSplineData>(Outer);
	Data->Initialize(SplinePoints, false, FTransform::Identity);
	return Data;
}

UPCGDataAsset* FindOrCreateDataAsset(const FString& AssetPath, FString& OutError)
{
	const FString ObjectPath = AssetPath + TEXT(".") + FPackageName::GetShortName(AssetPath);
	if (UPCGDataAsset* Existing = LoadObject<UPCGDataAsset>(nullptr, *ObjectPath, nullptr, LOAD_NoWarn | LOAD_Quiet))
	{
		return Existing;
	}
	if (!FPackageName::IsValidLongPackageName(AssetPath))
	{
		OutError = FString::Printf(TEXT("setSettlementData: %s is not a content path"), *AssetPath);
		return nullptr;
	}
	UPackage* Package = CreatePackage(*AssetPath);
	UPCGDataAsset* Asset = NewObject<UPCGDataAsset>(Package, *FPackageName::GetShortName(AssetPath), RF_Public | RF_Standalone | RF_Transactional);
	FAssetRegistryModule::AssetCreated(Asset);
	return Asset;
}

TSharedPtr<FJsonValue> SetSettlementData(const TSharedPtr<FJsonObject>& Params, FString& OutError)
{
	TRACE_CPUPROFILER_EVENT_SCOPE(LilaLinkSettlement::SetSettlementData);

	FString AssetPath, Label;
	if (!Params->TryGetStringField(TEXT("asset"), AssetPath) || AssetPath.IsEmpty())
	{
		OutError = TEXT("setSettlementData: give asset");
		return nullptr;
	}
	Params->TryGetStringField(TEXT("label"), Label);
	UPCGDataAsset* Asset = FindOrCreateDataAsset(AssetPath, OutError);
	if (!Asset)
	{
		return nullptr;
	}
	FScopedTransaction Transaction(NSLOCTEXT("LilaLink", "SetSettlementData", "Lila: Update Settlement Data"));
	Asset->Modify();
	Asset->Data.TaggedData.Reset();
	Asset->Name = Label;
#if WITH_EDITORONLY_DATA
	Asset->Description = FText::FromString(TEXT("Lots, building envelopes and roads from a Lila settlement."));
#endif

	const TArray<TSharedPtr<FJsonValue>>& Lots = ArrayOf(Params, TEXT("lots"));
	for (const TSharedPtr<FJsonValue>& Value : Lots)
	{
		const TSharedPtr<FJsonObject> Lot = Value->AsObject();
		UPCGPolygon2DData* Data = Lot.IsValid() ? MakePolygon(Asset, ReadPoints(Lot, TEXT("outline"))) : nullptr;
		if (Data)
		{
			SetDataAttributes(Data, Lot, {TEXT("outline")});
			AddTagged(Asset, Data, TEXT("Lots"), TEXT("Lot"));
		}
	}
	const TArray<TSharedPtr<FJsonValue>>& Envelopes = ArrayOf(Params, TEXT("envelopes"));
	for (const TSharedPtr<FJsonValue>& Value : Envelopes)
	{
		const TSharedPtr<FJsonObject> Envelope = Value->AsObject();
		UPCGPolygon2DData* Data = Envelope.IsValid() ? MakePolygon(Asset, ReadPoints(Envelope, TEXT("footprint"))) : nullptr;
		if (Data)
		{
			SetDataAttributes(Data, Envelope, {TEXT("footprint"), TEXT("sides"), TEXT("center"), TEXT("front")});
			// City Sample's building assignment reads the height as MaxHeight on the data.
			if (UPCGMetadata* Metadata = Data->MutableMetadata())
			{
				Metadata->FindOrCreateAttribute<double>(FPCGAttributeIdentifier(TEXT("MaxHeight"), PCGMetadataDomainID::Data),
					Envelope->GetNumberField(TEXT("maxHeight")), false, false);
			}
			AddTagged(Asset, Data, TEXT("Envelopes"), TEXT("Envelope"));
		}
	}
	AddTagged(Asset, MakeEnvelopePoints(Asset, Envelopes), TEXT("EnvelopePoints"), TEXT("Envelope"));
	const TArray<TSharedPtr<FJsonValue>>& Intersections = ArrayOf(Params, TEXT("intersections"));
	AddTagged(Asset, MakeMarkerPoints(Asset, Intersections, TEXT("center"), nullptr), TEXT("Intersections"), TEXT("Intersection"));
	const TArray<TSharedPtr<FJsonValue>>& Parking = ArrayOf(Params, TEXT("parking"));
	AddTagged(Asset, MakeMarkerPoints(Asset, Parking, TEXT("position"), TEXT("forward")), TEXT("ParkingSpots"), TEXT("Parking"));
	const TArray<TSharedPtr<FJsonValue>>& Roads = ArrayOf(Params, TEXT("roads"));
	for (const TSharedPtr<FJsonValue>& Value : Roads)
	{
		const TSharedPtr<FJsonObject> Road = Value->AsObject();
		UPCGSplineData* Data = Road.IsValid() ? MakeRoad(Asset, ReadPoints(Road, TEXT("points"))) : nullptr;
		if (Data)
		{
			SetDataAttributes(Data, Road, {TEXT("points")});
			AddTagged(Asset, Data, TEXT("Roads"), TEXT("Road"));
		}
	}
	Asset->MarkPackageDirty();
	UE_LOG(LogLilaLinkSettlement, Log, TEXT("setSettlementData: %s (%d lots, %d envelopes, %d roads)"), *AssetPath, Lots.Num(), Envelopes.Num(), Roads.Num());

	TSharedRef<FJsonObject> Out = MakeShared<FJsonObject>();
	Out->SetStringField(TEXT("asset"), Asset->GetPathName());
	Out->SetNumberField(TEXT("lots"), Lots.Num());
	Out->SetNumberField(TEXT("envelopes"), Envelopes.Num());
	Out->SetNumberField(TEXT("roads"), Roads.Num());
	Out->SetNumberField(TEXT("intersections"), Intersections.Num());
	Out->SetNumberField(TEXT("parking"), Parking.Num());
	return MakeShared<FJsonValueObject>(Out);
}

// ---------------------------------------------------------------------------------------------------------------------
// ZoneGraph

/** The settings' tag table and lane profiles. They are protected config properties, so they are reached by reflection. */
struct FZoneSettingsAccess
{
	UZoneGraphSettings* Settings = nullptr;
	TArray<FZoneLaneProfile>* Profiles = nullptr;
	FZoneGraphTagInfo* Tags = nullptr;
	int32 TagCount = 0;

	bool Init()
	{
		Settings = GetMutableDefault<UZoneGraphSettings>();
		if (!Settings)
		{
			return false;
		}
		if (const FArrayProperty* P = FindFProperty<FArrayProperty>(UZoneGraphSettings::StaticClass(), TEXT("LaneProfiles")))
		{
			Profiles = P->ContainerPtrToValuePtr<TArray<FZoneLaneProfile>>(Settings);
		}
		if (const FStructProperty* P = FindFProperty<FStructProperty>(UZoneGraphSettings::StaticClass(), TEXT("Tags")))
		{
			Tags = P->ContainerPtrToValuePtr<FZoneGraphTagInfo>(Settings);
			TagCount = P->ArrayDim;
		}
		return Profiles && Tags;
	}

	/** The tag of that name, made in the first free slot when the project has none. */
	FZoneGraphTag Tag(const FString& Name, TArray<FString>& Warnings)
	{
		const FName Key(*Name);
		for (int32 Index = 0; Index < TagCount; ++Index)
		{
			if (Tags[Index].Name == Key)
			{
				return Tags[Index].Tag;
			}
		}
		for (int32 Index = 0; Index < TagCount; ++Index)
		{
			if (Tags[Index].Name.IsNone())
			{
				Tags[Index].Name = Key;
				Tags[Index].Tag = FZoneGraphTag(static_cast<uint8>(Index));
				Tags[Index].Color = FColor::MakeRandomSeededColor(static_cast<int32>(GetTypeHash(Name)));
				return Tags[Index].Tag;
			}
		}
		Warnings.Add(FString::Printf(TEXT("no free ZoneGraph tag slot for %s"), *Name));
		return FZoneGraphTag::None;
	}

	/** Tags another system expects on lanes of each kind (Vehicle, Intersection, Crossing, Trunk). */
	TMap<FString, FZoneGraphTagMask> Extra;

	FZoneGraphTagMask Mask(const TArray<TSharedPtr<FJsonValue>>& Names, TArray<FString>& Warnings)
	{
		FZoneGraphTagMask Out = FZoneGraphTagMask::None;
		for (const TSharedPtr<FJsonValue>& N : Names)
		{
			Out.Add(Tag(N->AsString(), Warnings));
			if (const FZoneGraphTagMask* More = Extra.Find(N->AsString()))
			{
				Out.Add(*More);
			}
		}
		return Out;
	}

	/** Fills MassTraffic's empty lane filters with Lila's tags and picks up the ones the project set; null without MassTraffic. */
	TSharedPtr<FJsonObject> ReadMassTraffic(TArray<FString>& Warnings)
	{
		return LilaLinkSettlementTraffic::ConfigureSettings(
			[&](const TCHAR* Names) {
				TArray<FString> Parts;
				FString(Names).ParseIntoArray(Parts, TEXT(","));
				FZoneGraphTagMask Mask = FZoneGraphTagMask::None;
				for (const FString& Part : Parts)
				{
					Mask.Add(Tag(Part, Warnings));
				}
				return Mask;
			},
			Extra);
	}
};

EZoneLaneDirection DirectionFrom(const FString& Name)
{
	return Name == TEXT("backward") ? EZoneLaneDirection::Backward : Name == TEXT("none") ? EZoneLaneDirection::None : EZoneLaneDirection::Forward;
}

TSharedPtr<FJsonValue> SetZoneGraph(const TSharedPtr<FJsonObject>& Params, FString& OutError)
{
	TRACE_CPUPROFILER_EVENT_SCOPE(LilaLinkSettlement::SetZoneGraph);

	FString Id, Label = TEXT("Settlement_Lanes");
	if (!Params->TryGetStringField(TEXT("id"), Id) || Id.IsEmpty())
	{
		OutError = TEXT("setZoneGraph: give id");
		return nullptr;
	}
	Params->TryGetStringField(TEXT("label"), Label);
	UWorld* World = EditorWorld();
	if (!World)
	{
		OutError = TEXT("setZoneGraph: no editor world");
		return nullptr;
	}
	FZoneSettingsAccess Access;
	if (!Access.Init())
	{
		OutError = TEXT("setZoneGraph: this engine's ZoneGraph settings have no LaneProfiles or Tags");
		return nullptr;
	}
	TArray<FString> Warnings;
	const TSharedPtr<FJsonObject> MassTraffic = Access.ReadMassTraffic(Warnings);
	FScopedTransaction Transaction(NSLOCTEXT("LilaLink", "SetZoneGraph", "Lila: Update Settlement Lanes"));

	// Lane profiles, matched by name so a resend keeps their ids and the shapes already referring to them.
	Access.Settings->Modify();
	TMap<FString, FZoneLaneProfileRef> ProfileRefs;
	const TSharedPtr<FJsonObject>* Profiles = nullptr;
	if (Params->TryGetObjectField(TEXT("profiles"), Profiles))
	{
		for (const TPair<FString, TSharedPtr<FJsonValue>>& Entry : (*Profiles)->Values)
		{
			const TSharedPtr<FJsonObject> Desc = Entry.Value->AsObject();
			if (!Desc.IsValid())
			{
				continue;
			}
			const FName Name(*(TEXT("Lila_") + Entry.Key));
			FZoneLaneProfile* Profile = Access.Profiles->FindByPredicate([&Name](const FZoneLaneProfile& P) { return P.Name == Name; });
			if (!Profile)
			{
				Profile = &Access.Profiles->AddDefaulted_GetRef();
				Profile->Name = Name;
			}
			Profile->Lanes.Reset();
			for (const TSharedPtr<FJsonValue>& LaneValue : ArrayOf(Desc, TEXT("lanes")))
			{
				const TSharedPtr<FJsonObject> Lane = LaneValue->AsObject();
				if (!Lane.IsValid())
				{
					continue;
				}
				FZoneLaneDesc& Out = Profile->Lanes.AddDefaulted_GetRef();
				Out.Width = static_cast<float>(Lane->GetNumberField(TEXT("width")));
				Out.Direction = DirectionFrom(Lane->GetStringField(TEXT("direction")));
				Out.Tags = Access.Mask(ArrayOf(Lane, TEXT("tags")), Warnings);
			}
			ProfileRefs.Add(Entry.Key, FZoneLaneProfileRef(*Profile));
		}
	}
	Access.Settings->TryUpdateDefaultConfigFile();
	UE::ZoneGraphDelegates::OnZoneGraphTagsChanged.Broadcast();

	const FName Tag = ZoneTagFor(Id);
	AActor* Actor = nullptr;
	for (TActorIterator<AActor> It(World); It; ++It)
	{
		if (It->ActorHasTag(Tag))
		{
			Actor = *It;
			break;
		}
	}
	if (Actor)
	{
		// The shapes are rebuilt from scratch on every send.
		Actor->Modify();
		TArray<UZoneShapeComponent*> Old;
		Actor->GetComponents(Old);
		for (UZoneShapeComponent* Component : Old)
		{
			Component->Modify();
			Actor->RemoveInstanceComponent(Component);
			Component->DestroyComponent();
		}
	}
	else
	{
		FActorSpawnParameters Spawn;
		Spawn.ObjectFlags |= RF_Transactional;
		Actor = World->SpawnActor<AActor>(AActor::StaticClass(), FTransform::Identity, Spawn);
		if (!Actor)
		{
			Transaction.Cancel();
			OutError = TEXT("setZoneGraph: could not spawn the lane actor");
			return nullptr;
		}
		Actor->Tags.Add(Tag);
		Actor->Tags.Add(LilaTag);
		USceneComponent* Root = NewObject<USceneComponent>(Actor, TEXT("Root"), RF_Transactional);
		Actor->SetRootComponent(Root);
		Actor->AddInstanceComponent(Root);
		Root->RegisterComponent();
	}
	Actor->SetActorLabel(Label);
	Actor->SetFolderPath(FName(TEXT("Lila/Settlement")));

	int32 Shapes = 0;
	auto NewShape = [&](FZoneShapeType Type) {
		const FName Name = MakeUniqueObjectName(Actor, UZoneShapeComponent::StaticClass(), TEXT("LilaZoneShape"));
		UZoneShapeComponent* Shape = NewObject<UZoneShapeComponent>(Actor, Name, RF_Transactional);
		Shape->ComponentTags.Add(LilaTag);
		Shape->SetupAttachment(Actor->GetRootComponent());
		Shape->SetShapeType(Type);
		Actor->AddInstanceComponent(Shape);
		++Shapes;
		return Shape;
	};
	auto RefOf = [&](const FString& Name) {
		const FZoneLaneProfileRef* Ref = ProfileRefs.Find(Name);
		if (!Ref)
		{
			Warnings.Add(FString::Printf(TEXT("no lane profile %s"), *Name));
		}
		return Ref ? *Ref : FZoneLaneProfileRef();
	};

	for (const TSharedPtr<FJsonValue>& RoadValue : ArrayOf(Params, TEXT("roads")))
	{
		const TSharedPtr<FJsonObject> Road = RoadValue->AsObject();
		const TArray<FVector> Points = Road.IsValid() ? ReadPoints(Road, TEXT("points")) : TArray<FVector>();
		if (Points.Num() < 2)
		{
			continue;
		}
		UZoneShapeComponent* Shape = NewShape(FZoneShapeType::Spline);
		Shape->SetCommonLaneProfile(RefOf(Road->GetStringField(TEXT("profile"))));
		Shape->SetTags(Access.Mask(ArrayOf(Road, TEXT("tags")), Warnings));
		TArray<FZoneShapePoint>& ShapePoints = Shape->GetMutablePoints();
		ShapePoints.Reset();
		for (const FVector& P : Points)
		{
			FZoneShapePoint& Point = ShapePoints.Emplace_GetRef(P);
			Point.Type = FZoneShapePointType::AutoBezier;
		}
		for (int32 Index = 0; Index < ShapePoints.Num(); ++Index)
		{
			Shape->UpdatePointRotationAndTangent(Index);
		}
		Shape->RegisterComponent();
		Shape->UpdateShape();
	}

	for (const TSharedPtr<FJsonValue>& JunctionValue : ArrayOf(Params, TEXT("junctions")))
	{
		const TSharedPtr<FJsonObject> Junction = JunctionValue->AsObject();
		if (!Junction.IsValid())
		{
			continue;
		}
		const FVector Center = ReadPoint(Junction->TryGetField(TEXT("center")));
		struct FEnd
		{
			FVector Position;
			FVector Forward;
			FString Profile;
			double Angle;
			bool bReverse = false;
		};
		TArray<FEnd> Ends;
		for (const TSharedPtr<FJsonValue>& EndValue : ArrayOf(Junction, TEXT("points")))
		{
			const TSharedPtr<FJsonObject> End = EndValue->AsObject();
			if (!End.IsValid())
			{
				continue;
			}
			FEnd& E = Ends.AddDefaulted_GetRef();
			E.Position = ReadPoint(End->TryGetField(TEXT("position")));
			E.Forward = ReadPoint(End->TryGetField(TEXT("forward"))).GetSafeNormal();
			E.Profile = End->GetStringField(TEXT("profile"));
			E.Angle = FMath::Atan2(E.Position.Y - Center.Y, E.Position.X - Center.X);
			End->TryGetBoolField(TEXT("reverse"), E.bReverse);
		}
		if (Ends.Num() < 2)
		{
			continue;
		}
		// Round the junction in order, so the polygon's sides never cross.
		Ends.Sort([](const FEnd& A, const FEnd& B) { return A.Angle < B.Angle; });
		UZoneShapeComponent* Shape = NewShape(FZoneShapeType::Polygon);
		Shape->SetTags(Access.Mask(ArrayOf(Junction, TEXT("tags")), Warnings));
		TArray<FZoneShapePoint>& ShapePoints = Shape->GetMutablePoints();
		ShapePoints.Reset();
		for (const FEnd& E : Ends)
		{
			FZoneShapePoint& Point = ShapePoints.Emplace_GetRef(E.Position);
			Point.Type = FZoneShapePointType::LaneProfile;
			Point.SetRotationFromForwardAndUp(E.Forward, FVector::UpVector);
			const int32 Profile = Shape->AddUniquePerPointLaneProfile(RefOf(E.Profile));
			Point.LaneProfile = Profile >= 0 && Profile < 255 ? static_cast<uint8>(Profile) : FZoneShapePoint::InheritLaneProfile;
			Point.bReverseLaneProfile = E.bReverse;
		}
		Shape->RegisterComponent();
		Shape->UpdateShape();
	}
	Actor->MarkPackageDirty();
	UE::ZoneGraphDelegates::OnZoneGraphRequestRebuild.Broadcast();
	if (MassTraffic.IsValid())
	{
		FString Folder = TEXT("/Game/Lila/Settlement");
		Params->TryGetStringField(TEXT("folder"), Folder);
		FString Name = Label;
		for (TCHAR& C : Name)
		{
			C = FChar::IsAlnum(C) ? C : TEXT('_');
		}
		const TSharedPtr<FJsonObject> Data = LilaLinkSettlementTraffic::WriteData(World, Params, Folder, Name, Warnings);
		for (const auto& Field : Data->Values)
		{
			MassTraffic->SetField(FString(Field.Key), Field.Value);
		}
	}
	UE_LOG(LogLilaLinkSettlement, Log, TEXT("setZoneGraph: %s (%d shapes, %d profiles)"), *Label, Shapes, ProfileRefs.Num());

	TSharedRef<FJsonObject> Out = MakeShared<FJsonObject>();
	Out->SetStringField(TEXT("actor"), Actor->GetPathName());
	Out->SetNumberField(TEXT("shapes"), Shapes);
	Out->SetNumberField(TEXT("profiles"), ProfileRefs.Num());
	if (MassTraffic.IsValid())
	{
		Out->SetObjectField(TEXT("massTraffic"), MassTraffic);
	}
	else
	{
		Out->SetBoolField(TEXT("massTraffic"), false);
	}
	TArray<TSharedPtr<FJsonValue>> WarningValues;
	for (const FString& W : Warnings)
	{
		WarningValues.Add(MakeShared<FJsonValueString>(W));
	}
	Out->SetArrayField(TEXT("warnings"), WarningValues);
	return MakeShared<FJsonValueObject>(Out);
}

TSharedPtr<FJsonValue> RemoveSettlement(const TSharedPtr<FJsonObject>& Params, FString& OutError)
{
	TRACE_CPUPROFILER_EVENT_SCOPE(LilaLinkSettlement::RemoveSettlement);

	FString Id;
	if (!Params->TryGetStringField(TEXT("id"), Id) || Id.IsEmpty())
	{
		OutError = TEXT("removeSettlement: give id");
		return nullptr;
	}
	UWorld* World = EditorWorld();
	if (!World)
	{
		OutError = TEXT("removeSettlement: no editor world");
		return nullptr;
	}
	FScopedTransaction Transaction(NSLOCTEXT("LilaLink", "RemoveSettlement", "Lila: Remove Settlement Lanes"));
	bool bRemoved = false;
	const FName Tag = ZoneTagFor(Id);
	TArray<AActor*> Found;
	for (TActorIterator<AActor> It(World); It; ++It)
	{
		if (It->ActorHasTag(Tag))
		{
			Found.Add(*It);
		}
	}
	for (AActor* Actor : Found)
	{
		bRemoved |= World->EditorDestroyActor(Actor, true);
	}
	if (bRemoved)
	{
		UE::ZoneGraphDelegates::OnZoneGraphRequestRebuild.Broadcast();
	}
	TSharedRef<FJsonObject> Out = MakeShared<FJsonObject>();
	Out->SetBoolField(TEXT("removed"), bRemoved);
	return MakeShared<FJsonValueObject>(Out);
}
} // namespace

namespace
{
// ---------------------------------------------------------------------------------------------------------------------
// Road cross-sections as shape grammar modules

TSharedPtr<FJsonValue> SetRoadGrammar(const TSharedPtr<FJsonObject>& Params, FString& OutError)
{
	TRACE_CPUPROFILER_EVENT_SCOPE(LilaLinkSettlement::SetRoadGrammar);

	FString AssetPath, Label, Grammar;
	if (!Params->TryGetStringField(TEXT("asset"), AssetPath) || AssetPath.IsEmpty())
	{
		OutError = TEXT("setRoadGrammar: give asset");
		return nullptr;
	}
	Params->TryGetStringField(TEXT("label"), Label);
	Params->TryGetStringField(TEXT("grammar"), Grammar);
	UPCGDataAsset* Asset = FindOrCreateDataAsset(AssetPath, OutError);
	if (!Asset)
	{
		return nullptr;
	}
	FScopedTransaction Transaction(NSLOCTEXT("LilaLink", "SetRoadGrammar", "Lila: Update Road Grammar"));
	Asset->Modify();
	Asset->Data.TaggedData.Reset();
	Asset->Name = Label;

	// One entry per distinct symbol, as Subdivide Segment's ModulesInfo pin takes them.
	UPCGParamData* Modules = NewObject<UPCGParamData>(Asset);
	UPCGMetadata* Metadata = Modules->MutableMetadata();
	FPCGMetadataAttribute<FName>* Symbol = Metadata->CreateAttribute<FName>(TEXT("Symbol"), NAME_None, false, false);
	FPCGMetadataAttribute<double>* Size = Metadata->CreateAttribute<double>(TEXT("Size"), 100.0, false, false);
	FPCGMetadataAttribute<bool>* Scalable = Metadata->CreateAttribute<bool>(TEXT("Scalable"), false, false, false);
	FPCGMetadataAttribute<FVector4>* Color = Metadata->CreateAttribute<FVector4>(TEXT("DebugColor"), FVector4::One(), false, false);
	TSet<FString> Seen;
	for (const TSharedPtr<FJsonValue>& Value : ArrayOf(Params, TEXT("modules")))
	{
		const TSharedPtr<FJsonObject> Module = Value->AsObject();
		FString Name;
		if (!Module.IsValid() || !Module->TryGetStringField(TEXT("symbol"), Name) || Seen.Contains(Name))
		{
			continue;
		}
		Seen.Add(Name);
		const int64 Key = Metadata->AddEntry();
		Symbol->SetValue(Key, FName(*Name));
		Size->SetValue(Key, Module->GetNumberField(TEXT("size")));
		bool bScalable = false;
		Module->TryGetBoolField(TEXT("scalable"), bScalable);
		Scalable->SetValue(Key, bScalable);
		const FLinearColor Debug = FLinearColor::MakeRandomSeededColor(static_cast<int32>(GetTypeHash(Name)));
		Color->SetValue(Key, FVector4(Debug.R, Debug.G, Debug.B, 1.0));
	}
	AddTagged(Asset, Modules, TEXT("ModulesInfo"), TEXT("RoadModules"));
	UPCGParamData* GrammarData = NewObject<UPCGParamData>(Asset);
	const int64 GrammarKey = GrammarData->MutableMetadata()->AddEntry();
	GrammarData->MutableMetadata()->CreateAttribute<FString>(TEXT("Grammar"), FString(), false, false)->SetValue(GrammarKey, Grammar);
	AddTagged(Asset, GrammarData, TEXT("Grammar"), TEXT("RoadGrammar"));
	Asset->MarkPackageDirty();

	TSharedRef<FJsonObject> Out = MakeShared<FJsonObject>();
	Out->SetStringField(TEXT("asset"), Asset->GetPathName());
	Out->SetNumberField(TEXT("modules"), Seen.Num());
	return MakeShared<FJsonValueObject>(Out);
}

/** A numeric value of any float or integer property, or false. */
bool ReadNumber(const FProperty* Prop, const void* Container, double& Out)
{
	if (const FNumericProperty* Numeric = CastField<FNumericProperty>(Prop))
	{
		const void* Value = Numeric->ContainerPtrToValuePtr<void>(Container);
		Out = Numeric->IsFloatingPoint() ? Numeric->GetFloatingPointPropertyValue(Value) : static_cast<double>(Numeric->GetSignedIntPropertyValue(Value));
		return true;
	}
	return false;
}

bool ReadText(const FProperty* Prop, const void* Container, FString& Out)
{
	if (const FNameProperty* Name = CastField<FNameProperty>(Prop))
	{
		Out = Name->GetPropertyValue_InContainer(Container).ToString();
		return true;
	}
	if (const FStrProperty* Str = CastField<FStrProperty>(Prop))
	{
		Out = Str->GetPropertyValue_InContainer(Container);
		return true;
	}
	return false;
}

/**
 * Slots from any object: every array of structs whose elements have a name-like field and a size-like field, searched
 * a few levels down; and the first string property named like a grammar.
 */
void ReadSlots(const UStruct* Type, const void* Container, int32 Depth, TArray<TSharedPtr<FJsonValue>>& Modules, FString& Grammar)
{
	if (Depth > 3)
	{
		return;
	}
	for (TFieldIterator<FProperty> It(Type); It; ++It)
	{
		const FString PropName = It->GetName();
		FString Text;
		if (Grammar.IsEmpty() && PropName.Contains(TEXT("Grammar")) && ReadText(*It, Container, Text))
		{
			Grammar = Text;
			continue;
		}
		if (const FArrayProperty* Array = CastField<FArrayProperty>(*It))
		{
			const FStructProperty* Inner = CastField<FStructProperty>(Array->Inner);
			if (!Inner)
			{
				continue;
			}
			FScriptArrayHelper Helper(Array, Array->ContainerPtrToValuePtr<void>(Container));
			for (int32 Index = 0; Index < Helper.Num(); ++Index)
			{
				const void* Element = Helper.GetRawPtr(Index);
				FString Symbol;
				double Size = -1.0;
				for (TFieldIterator<FProperty> Field(Inner->Struct); Field; ++Field)
				{
					const FString FieldName = Field->GetName();
					FString S;
					double D = 0.0;
					if (Symbol.IsEmpty() && (FieldName.Contains(TEXT("Symbol")) || FieldName == TEXT("Name") || FieldName.Contains(TEXT("Slot")) || FieldName == TEXT("Id")) &&
						ReadText(*Field, Element, S))
					{
						Symbol = S;
					}
					else if (Size < 0.0 && (FieldName.Contains(TEXT("Size")) || FieldName.Contains(TEXT("Width"))) && ReadNumber(*Field, Element, D))
					{
						Size = D;
					}
				}
				if (!Symbol.IsEmpty() && Size >= 0.0)
				{
					TSharedRef<FJsonObject> Module = MakeShared<FJsonObject>();
					Module->SetStringField(TEXT("symbol"), Symbol);
					Module->SetNumberField(TEXT("size"), Size);
					Modules.Add(MakeShared<FJsonValueObject>(Module));
				}
				else
				{
					ReadSlots(Inner->Struct, Element, Depth + 1, Modules, Grammar);
				}
			}
		}
		else if (const FStructProperty* Struct = CastField<FStructProperty>(*It))
		{
			ReadSlots(Struct->Struct, Struct->ContainerPtrToValuePtr<void>(Container), Depth + 1, Modules, Grammar);
		}
	}
}

TSharedPtr<FJsonValue> ReadRoadGrammar(const TSharedPtr<FJsonObject>& Params, FString& OutError)
{
	TRACE_CPUPROFILER_EVENT_SCOPE(LilaLinkSettlement::ReadRoadGrammar);

	FString AssetPath;
	if (!Params->TryGetStringField(TEXT("asset"), AssetPath) || AssetPath.IsEmpty())
	{
		OutError = TEXT("readRoadGrammar: give asset");
		return nullptr;
	}
	const FString ObjectPath = AssetPath.Contains(TEXT(".")) ? AssetPath : AssetPath + TEXT(".") + FPackageName::GetShortName(AssetPath);
	UObject* Object = LoadObject<UObject>(nullptr, *ObjectPath);
	if (!Object)
	{
		OutError = FString::Printf(TEXT("readRoadGrammar: no asset at %s"), *AssetPath);
		return nullptr;
	}
	TArray<TSharedPtr<FJsonValue>> Modules;
	FString Grammar;
	if (const UPCGDataAsset* Data = Cast<UPCGDataAsset>(Object))
	{
		for (const FPCGTaggedData& Tagged : Data->Data.TaggedData)
		{
			const UPCGParamData* Param = Cast<UPCGParamData>(Tagged.Data);
			const UPCGMetadata* Metadata = Param ? Param->ConstMetadata() : nullptr;
			if (!Metadata)
			{
				continue;
			}
			const FPCGMetadataAttributeBase* SymbolBase = Metadata->GetConstAttribute(TEXT("Symbol"));
			const FPCGMetadataAttributeBase* SizeBase = Metadata->GetConstAttribute(TEXT("Size"));
			const FPCGMetadataAttributeBase* GrammarBase = Metadata->GetConstAttribute(TEXT("Grammar"));
			if (GrammarBase && GrammarBase->GetTypeId() == static_cast<int16>(EPCGMetadataTypes::String) && Metadata->GetLocalItemCount() > 0)
			{
				Grammar = static_cast<const FPCGMetadataAttribute<FString>*>(GrammarBase)->GetValueFromItemKey(0);
			}
			if (!SymbolBase || !SizeBase)
			{
				continue;
			}
			for (int64 Key = 0; Key < Metadata->GetLocalItemCount(); ++Key)
			{
				FString Symbol;
				if (SymbolBase->GetTypeId() == static_cast<int16>(EPCGMetadataTypes::Name))
				{
					Symbol = static_cast<const FPCGMetadataAttribute<FName>*>(SymbolBase)->GetValueFromItemKey(Key).ToString();
				}
				else if (SymbolBase->GetTypeId() == static_cast<int16>(EPCGMetadataTypes::String))
				{
					Symbol = static_cast<const FPCGMetadataAttribute<FString>*>(SymbolBase)->GetValueFromItemKey(Key);
				}
				double Size = 0.0;
				switch (static_cast<EPCGMetadataTypes>(SizeBase->GetTypeId()))
				{
				case EPCGMetadataTypes::Double: Size = static_cast<const FPCGMetadataAttribute<double>*>(SizeBase)->GetValueFromItemKey(Key); break;
				case EPCGMetadataTypes::Float: Size = static_cast<const FPCGMetadataAttribute<float>*>(SizeBase)->GetValueFromItemKey(Key); break;
				case EPCGMetadataTypes::Integer32: Size = static_cast<const FPCGMetadataAttribute<int32>*>(SizeBase)->GetValueFromItemKey(Key); break;
				case EPCGMetadataTypes::Integer64: Size = static_cast<double>(static_cast<const FPCGMetadataAttribute<int64>*>(SizeBase)->GetValueFromItemKey(Key)); break;
				default: break;
				}
				if (!Symbol.IsEmpty())
				{
					TSharedRef<FJsonObject> Module = MakeShared<FJsonObject>();
					Module->SetStringField(TEXT("symbol"), Symbol);
					Module->SetNumberField(TEXT("size"), Size);
					Modules.Add(MakeShared<FJsonValueObject>(Module));
				}
			}
		}
	}
	else
	{
		ReadSlots(Object->GetClass(), Object, 0, Modules, Grammar);
	}
	TSharedRef<FJsonObject> Out = MakeShared<FJsonObject>();
	Out->SetStringField(TEXT("asset"), Object->GetPathName());
	Out->SetStringField(TEXT("class"), Object->GetClass()->GetName());
	Out->SetArrayField(TEXT("modules"), Modules);
	Out->SetStringField(TEXT("grammar"), Grammar);
	return MakeShared<FJsonValueObject>(Out);
}
} // namespace

TSharedPtr<FJsonValue> LilaLinkSettlement::WriteSettlementData(const TSharedPtr<FJsonObject>& Params, FString& OutError)
{
	return SetSettlementData(Params, OutError);
}

void LilaLinkSettlement::RegisterMethods()
{
	LilaLink::RegisterMethod(TEXT("setSettlementData"), &SetSettlementData, Feature);
	LilaLink::RegisterMethod(TEXT("setZoneGraph"), &SetZoneGraph, Feature);
	LilaLink::RegisterMethod(TEXT("removeSettlement"), &RemoveSettlement, Feature);
	LilaLink::RegisterMethod(TEXT("setRoadGrammar"), &SetRoadGrammar, Feature);
	LilaLink::RegisterMethod(TEXT("readRoadGrammar"), &ReadRoadGrammar, Feature);
}
