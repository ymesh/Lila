#include "LilaLinkSettlementTraffic.h"

#include "AssetRegistry/AssetRegistryModule.h"
#include "Editor.h"
#include "Engine/DataAsset.h"
#include "Engine/World.h"
#include "EngineUtils.h"
#include "Misc/PackageName.h"
#include "ProfilingDebugging/CpuProfilerTrace.h"
#include "UObject/Package.h"
#include "UObject/UnrealType.h"
#include "ZoneGraphTypes.h"

namespace
{
UClass* TrafficClass(const TCHAR* Name)
{
	return FindObject<UClass>(nullptr, *FString::Printf(TEXT("/Script/MassTraffic.%s"), Name));
}

template <class T> T* PropertyValue(UObject* Object, const TCHAR* Name)
{
	if (!Object)
	{
		return nullptr;
	}
	const FProperty* Property = Object->GetClass()->FindPropertyByName(Name);
	return Property ? Property->ContainerPtrToValuePtr<T>(Object) : nullptr;
}

/** The struct member of that name in an array element, or null. */
template <class T> T* MemberOf(const FStructProperty* Inner, void* Element, const TCHAR* Name)
{
	const FProperty* Member = Inner->Struct->FindPropertyByName(Name);
	return Member ? Member->ContainerPtrToValuePtr<T>(Element) : nullptr;
}

bool IsEmpty(const FZoneGraphTagFilter& F)
{
	return F.AnyTags == FZoneGraphTagMask::None && F.AllTags == FZoneGraphTagMask::None && F.NotTags == FZoneGraphTagMask::None;
}

UObject* FindOrCreateAsset(UClass* Class, const FString& AssetPath, bool& bCreated)
{
	bCreated = false;
	const FString ObjectPath = AssetPath + TEXT(".") + FPackageName::GetShortName(AssetPath);
	if (UObject* Existing = StaticLoadObject(Class, nullptr, *ObjectPath, nullptr, LOAD_NoWarn | LOAD_Quiet))
	{
		return Existing;
	}
	if (!FPackageName::IsValidLongPackageName(AssetPath))
	{
		return nullptr;
	}
	UPackage* Package = CreatePackage(*AssetPath);
	UObject* Asset = NewObject<UObject>(Package, Class, *FPackageName::GetShortName(AssetPath), RF_Public | RF_Standalone | RF_Transactional);
	FAssetRegistryModule::AssetCreated(Asset);
	bCreated = true;
	return Asset;
}

/** Sum of a lane profile's widths, from the request's profiles. */
double ProfileWidth(const TSharedPtr<FJsonObject>& Params, const FString& Profile)
{
	const TSharedPtr<FJsonObject>* Profiles = nullptr;
	const TSharedPtr<FJsonObject>* Desc = nullptr;
	const TArray<TSharedPtr<FJsonValue>>* Lanes = nullptr;
	double Width = 0.0;
	if (Params->TryGetObjectField(TEXT("profiles"), Profiles) && (*Profiles)->TryGetObjectField(Profile, Desc) && (*Desc)->TryGetArrayField(TEXT("lanes"), Lanes))
	{
		for (const TSharedPtr<FJsonValue>& Lane : *Lanes)
		{
			if (const TSharedPtr<FJsonObject> L = Lane->AsObject())
			{
				Width += L->GetNumberField(TEXT("width"));
			}
		}
	}
	return Width;
}

FVector ReadVector(const TSharedPtr<FJsonObject>& Object, const TCHAR* Field)
{
	const TArray<TSharedPtr<FJsonValue>>* Xyz = nullptr;
	if (!Object->TryGetArrayField(Field, Xyz) || Xyz->Num() != 3)
	{
		return FVector::ZeroVector;
	}
	return FVector((*Xyz)[0]->AsNumber(), (*Xyz)[1]->AsNumber(), (*Xyz)[2]->AsNumber());
}

const TArray<TSharedPtr<FJsonValue>>& Entries(const TSharedPtr<FJsonObject>& Object, const TCHAR* Field)
{
	static const TArray<TSharedPtr<FJsonValue>> Empty;
	const TArray<TSharedPtr<FJsonValue>>* Values = nullptr;
	return Object->TryGetArrayField(Field, Values) ? *Values : Empty;
}

/**
 * A traffic light per approach of each signalised junction: on the kerb at the stop line, facing the traffic, controlling
 * the side whose middle is the road's centre where it meets the junction (MassTraffic matches lights to sides by that point).
 */
int32 WriteLights(UObject* Asset, UObject* Types, const TSharedPtr<FJsonObject>& Params)
{
	TRACE_CPUPROFILER_EVENT_SCOPE(LilaLinkSettlementTraffic::WriteLights);

	const FArrayProperty* Array = CastField<FArrayProperty>(Asset->GetClass()->FindPropertyByName(TEXT("TrafficLights")));
	const FStructProperty* Inner = Array ? CastField<FStructProperty>(Array->Inner) : nullptr;
	if (!Inner)
	{
		return INDEX_NONE;
	}
	Asset->Modify();
	FScriptArrayHelper Lights(Array, Array->ContainerPtrToValuePtr<void>(Asset));
	Lights.EmptyValues();
	for (const TSharedPtr<FJsonValue>& Value : Entries(Params, TEXT("junctions")))
	{
		const TSharedPtr<FJsonObject> Junction = Value->AsObject();
		FString Control;
		if (!Junction.IsValid() || !Junction->TryGetStringField(TEXT("control"), Control) || Control != TEXT("signal"))
		{
			continue;
		}
		for (const TSharedPtr<FJsonValue>& EndValue : Entries(Junction, TEXT("points")))
		{
			const TSharedPtr<FJsonObject> End = EndValue->AsObject();
			bool bVehicle = true;
			if (!End.IsValid() || (End->TryGetBoolField(TEXT("vehicle"), bVehicle) && !bVehicle))
			{
				continue;
			}
			const FVector Position = ReadVector(End, TEXT("position"));
			const FVector Forward = ReadVector(End, TEXT("forward")).GetSafeNormal2D();
			const FVector Right(-Forward.Y, Forward.X, 0.0);
			void* Light = Lights.GetRawPtr(Lights.AddValue());
			*MemberOf<FVector>(Inner, Light, TEXT("Position")) = Position + Right * (0.5 * ProfileWidth(Params, End->GetStringField(TEXT("profile"))));
			*MemberOf<float>(Inner, Light, TEXT("ZRotation")) = static_cast<float>((-Forward).Rotation().Yaw);
			*MemberOf<FVector>(Inner, Light, TEXT("ControlledIntersectionSideMidpoint")) = Position;
			*MemberOf<int16>(Inner, Light, TEXT("TrafficLightTypeIndex")) = INDEX_NONE;
		}
	}
	if (int32* Count = PropertyValue<int32>(Asset, TEXT("NumTrafficLights")))
	{
		*Count = Lights.Num();
	}
	if (Types)
	{
		*PropertyValue<TObjectPtr<UObject>>(Asset, TEXT("TrafficLightTypesData")) = Types;
	}
	Asset->MarkPackageDirty();
	return Lights.Num();
}

/** A light type for every lane count, so each approach finds one; the user gives it meshes to see the lights. */
void SeedLightTypes(UObject* Types)
{
	const FArrayProperty* Array = CastField<FArrayProperty>(Types->GetClass()->FindPropertyByName(TEXT("TrafficLightTypes")));
	const FStructProperty* Inner = Array ? CastField<FStructProperty>(Array->Inner) : nullptr;
	if (!Inner)
	{
		return;
	}
	FScriptArrayHelper TypesArray(Array, Array->ContainerPtrToValuePtr<void>(Types));
	if (TypesArray.Num() > 0)
	{
		return;
	}
	void* Type = TypesArray.GetRawPtr(TypesArray.AddValue());
	*MemberOf<FName>(Inner, Type, TEXT("Name")) = FName(TEXT("Lila"));
	*MemberOf<int32>(Inner, Type, TEXT("NumLanes")) = 0;
	Types->MarkPackageDirty();
}

int32 WriteParking(UObject* Asset, const TSharedPtr<FJsonObject>& Params, const FName Type)
{
	TRACE_CPUPROFILER_EVENT_SCOPE(LilaLinkSettlementTraffic::WriteParking);

	const FArrayProperty* Array = CastField<FArrayProperty>(Asset->GetClass()->FindPropertyByName(TEXT("TypedParkingSpaces")));
	const FStructProperty* Inner = Array ? CastField<FStructProperty>(Array->Inner) : nullptr;
	if (!Inner)
	{
		return INDEX_NONE;
	}
	Asset->Modify();
	FScriptArrayHelper Typed(Array, Array->ContainerPtrToValuePtr<void>(Asset));
	Typed.EmptyValues();
	void* Entry = Typed.GetRawPtr(Typed.AddValue());
	*MemberOf<FName>(Inner, Entry, TEXT("Name")) = Type;
	TArray<FTransform>& Spaces = *MemberOf<TArray<FTransform>>(Inner, Entry, TEXT("ParkingSpaces"));
	for (const TSharedPtr<FJsonValue>& Value : Entries(Params, TEXT("parking")))
	{
		const TSharedPtr<FJsonObject> Spot = Value->AsObject();
		if (!Spot.IsValid())
		{
			continue;
		}
		const FVector Forward = ReadVector(Spot, TEXT("forward")).GetSafeNormal2D();
		Spaces.Emplace(Forward.IsNearlyZero() ? FQuat::Identity : Forward.ToOrientationQuat(), ReadVector(Spot, TEXT("position")));
	}
	*MemberOf<int32>(Inner, Entry, TEXT("NumParkingSpaces")) = Spaces.Num();
	if (int32* Count = PropertyValue<int32>(Asset, TEXT("NumParkingSpaces")))
	{
		*Count = Spaces.Num();
	}
	Asset->MarkPackageDirty();
	return Spaces.Num();
}

/**
 * Points the level's Mass spawners at the data: an intersection generator's light data, a parked vehicle generator's
 * parking spaces. Generators already pointing at other data are left alone. Returns how many generators were set.
 */
int32 WireSpawners(UWorld* World, UObject* Lights, UObject* Types, UObject* Parking)
{
	TRACE_CPUPROFILER_EVENT_SCOPE(LilaLinkSettlementTraffic::WireSpawners);

	UClass* IntersectionGenerator = TrafficClass(TEXT("MassTrafficIntersectionSpawnDataGenerator"));
	UClass* ParkedGenerator = TrafficClass(TEXT("MassTrafficParkedVehicleSpawnDataGenerator"));
	auto SetIfFree = [](UObject* Generator, const TCHAR* Name, UObject* Value) {
		TObjectPtr<UObject>* Slot = PropertyValue<TObjectPtr<UObject>>(Generator, Name);
		if (!Slot || !Value || (*Slot && *Slot != Value))
		{
			return false;
		}
		*Slot = Value;
		return true;
	};
	int32 Wired = 0;
	for (TActorIterator<AActor> It(World); It; ++It)
	{
		const FArrayProperty* Array = CastField<FArrayProperty>(It->GetClass()->FindPropertyByName(TEXT("SpawnDataGenerators")));
		const FStructProperty* Inner = Array ? CastField<FStructProperty>(Array->Inner) : nullptr;
		const FObjectPropertyBase* InstanceProperty = Inner ? CastField<FObjectPropertyBase>(Inner->Struct->FindPropertyByName(TEXT("GeneratorInstance"))) : nullptr;
		if (!InstanceProperty)
		{
			continue;
		}
		FScriptArrayHelper Generators(Array, Array->ContainerPtrToValuePtr<void>(*It));
		for (int32 Index = 0; Index < Generators.Num(); ++Index)
		{
			UObject* Generator = InstanceProperty->GetObjectPropertyValue_InContainer(Generators.GetRawPtr(Index));
			if (!Generator)
			{
				continue;
			}
			Generator->Modify();
			bool bSet = false;
			if (IntersectionGenerator && Generator->IsA(IntersectionGenerator))
			{
				bSet |= SetIfFree(Generator, TEXT("TrafficLightInstanceData"), Lights);
				bSet |= SetIfFree(Generator, TEXT("TrafficLightTypesData"), Types);
			}
			else if (ParkedGenerator && Generator->IsA(ParkedGenerator))
			{
				bSet |= SetIfFree(Generator, TEXT("ParkingSpaces"), Parking);
			}
			Wired += bSet ? 1 : 0;
		}
	}
	return Wired;
}
} // namespace

bool LilaLinkSettlementTraffic::Present()
{
	return TrafficClass(TEXT("MassTrafficSettings")) != nullptr;
}

TSharedPtr<FJsonObject> LilaLinkSettlementTraffic::ConfigureSettings(TFunctionRef<FZoneGraphTagMask(const TCHAR*)> MaskOf, TMap<FString, FZoneGraphTagMask>& Extra)
{
	TRACE_CPUPROFILER_EVENT_SCOPE(LilaLinkSettlementTraffic::ConfigureSettings);

	UClass* Class = TrafficClass(TEXT("MassTrafficSettings"));
	UObject* Settings = Class ? Class->GetDefaultObject() : nullptr;
	if (!Settings)
	{
		return nullptr;
	}
	auto Filter = [&](const TCHAR* Any, const TCHAR* All, const TCHAR* Not) {
		FZoneGraphTagFilter F;
		F.AnyTags = Any ? MaskOf(Any) : FZoneGraphTagMask::None;
		F.AllTags = All ? MaskOf(All) : FZoneGraphTagMask::None;
		F.NotTags = Not ? MaskOf(Not) : FZoneGraphTagMask::None;
		return F;
	};
	// Polygon lanes carry their shape's tags, so a sidewalk crossing a junction is Pedestrian and Intersection and Vehicle.
	struct FLane
	{
		const TCHAR* Property;
		const TCHAR* Kind; // the Lila tag that filter selects, or null for one Lila never needs to add to
		FZoneGraphTagFilter Default;
	};
	const FLane Lanes[] = {
		{TEXT("TrafficLaneFilter"), TEXT("Vehicle"), Filter(TEXT("Vehicle"), nullptr, TEXT("Pedestrian"))},
		{TEXT("IntersectionLaneFilter"), TEXT("Intersection"), Filter(nullptr, TEXT("Intersection,Vehicle"), TEXT("Pedestrian"))},
		{TEXT("CrosswalkLaneFilter"), TEXT("Crossing"), Filter(nullptr, TEXT("Crossing"), nullptr)},
		{TEXT("TrunkLaneFilter"), TEXT("Trunk"), Filter(TEXT("Trunk"), nullptr, nullptr)},
		{TEXT("LaneChangingLaneFilter"), nullptr, Filter(TEXT("Vehicle"), nullptr, TEXT("Intersection,Pedestrian"))},
	};
	TSharedPtr<FJsonObject> Out = MakeShared<FJsonObject>();
	TArray<TSharedPtr<FJsonValue>> Configured, Kept;
	for (const FLane& Lane : Lanes)
	{
		FZoneGraphTagFilter* Value = PropertyValue<FZoneGraphTagFilter>(Settings, Lane.Property);
		if (!Value)
		{
			continue;
		}
		if (IsEmpty(*Value))
		{
			Settings->Modify();
			*Value = Lane.Default;
			Configured.Add(MakeShared<FJsonValueString>(Lane.Property));
			continue;
		}
		Kept.Add(MakeShared<FJsonValueString>(Lane.Property));
		// The project's own tags for that kind of lane go on Lila's lanes of the kind too.
		if (Lane.Kind)
		{
			FZoneGraphTagMask Mask = Value->AnyTags;
			Mask.Add(Value->AllTags);
			Extra.FindOrAdd(Lane.Kind).Add(Mask);
		}
	}
	if (!Configured.IsEmpty())
	{
		Settings->TryUpdateDefaultConfigFile();
	}
	Out->SetArrayField(TEXT("configured"), Configured);
	Out->SetArrayField(TEXT("kept"), Kept);
	return Out;
}

TSharedPtr<FJsonObject> LilaLinkSettlementTraffic::WriteData(UWorld* World, const TSharedPtr<FJsonObject>& Params, const FString& Folder, const FString& Name, TArray<FString>& Warnings)
{
	TRACE_CPUPROFILER_EVENT_SCOPE(LilaLinkSettlementTraffic::WriteData);

	TSharedPtr<FJsonObject> Out = MakeShared<FJsonObject>();
	UClass* LightsClass = TrafficClass(TEXT("MassTrafficLightInstancesDataAsset"));
	UClass* TypesClass = TrafficClass(TEXT("MassTrafficLightTypesDataAsset"));
	UClass* ParkingClass = TrafficClass(TEXT("MassTrafficParkingSpacesDataAsset"));
	if (!LightsClass || !TypesClass || !ParkingClass)
	{
		Warnings.Add(TEXT("MassTraffic has no light or parking data asset classes; its intersections run as stop signs"));
		return Out;
	}
	bool bCreated = false;
	UObject* Types = FindOrCreateAsset(TypesClass, Folder + TEXT("/DA_") + Name + TEXT("_TrafficLightTypes"), bCreated);
	if (Types && bCreated)
	{
		SeedLightTypes(Types);
	}
	UObject* Lights = FindOrCreateAsset(LightsClass, Folder + TEXT("/DA_") + Name + TEXT("_TrafficLights"), bCreated);
	UObject* Parking = FindOrCreateAsset(ParkingClass, Folder + TEXT("/DA_") + Name + TEXT("_ParkingSpaces"), bCreated);
	if (!Types || !Lights || !Parking)
	{
		Warnings.Add(FString::Printf(TEXT("could not make MassTraffic data assets in %s"), *Folder));
		return Out;
	}
	FString ParkingType = TEXT("Car");
	Params->TryGetStringField(TEXT("parkingType"), ParkingType);
	Out->SetNumberField(TEXT("trafficLights"), WriteLights(Lights, Types, Params));
	Out->SetNumberField(TEXT("parkingSpaces"), WriteParking(Parking, Params, FName(*ParkingType)));
	Out->SetStringField(TEXT("trafficLightData"), Lights->GetPathName());
	Out->SetStringField(TEXT("trafficLightTypes"), Types->GetPathName());
	Out->SetStringField(TEXT("parkingData"), Parking->GetPathName());
	Out->SetNumberField(TEXT("spawnersWired"), World ? WireSpawners(World, Lights, Types, Parking) : 0);
	return Out;
}
