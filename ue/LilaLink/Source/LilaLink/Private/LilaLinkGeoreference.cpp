#include "LilaLinkGeoreference.h"

#include "LilaLinkLog.h"
#include "AssetImportTask.h"
#include "AssetToolsModule.h"
#include "IAssetTools.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Editor.h"
#include "Engine/World.h"
#include "EngineUtils.h"
#include "ProfilingDebugging/CpuProfilerTrace.h"
#include "ScopedTransaction.h"
#include "UObject/Script.h"

#define LOCTEXT_NAMESPACE "LilaLinkGeoreference"

namespace LilaLink
{
namespace
{
// Found by name: both plugins are optional, so LilaLink builds and loads without either.
const TCHAR* GeoReferencingSystemClass = TEXT("/Script/GeoReferencing.GeoReferencingSystem");
const TCHAR* CesiumGeoreferenceClass = TEXT("/Script/CesiumRuntime.CesiumGeoreference");

UWorld* GeoEditorWorld()
{
	return GEditor ? GEditor->GetEditorWorldContext().World() : nullptr;
}

/** Sets a property from its text form; false when the class has no such property or the text doesn't parse. */
bool SetProperty(UObject* Object, const TCHAR* Name, const FString& Value)
{
	FProperty* Property = Object->GetClass()->FindPropertyByName(FName(Name));
	return Property && Property->ImportText_InContainer(*Value, Object, Object, PPF_None) != nullptr;
}

/** The level's actor of the class, or a new one. */
AActor* FindOrSpawn(UWorld* World, UClass* Class, bool& bSpawned)
{
	TRACE_CPUPROFILER_EVENT_SCOPE(LilaLinkGeoreference::FindOrSpawn);

	bSpawned = false;
	for (TActorIterator<AActor> It(World, Class); It; ++It)
	{
		return *It;
	}
	bSpawned = true;
	return World->SpawnActor(Class);
}

/** Calls a UFUNCTION taking one FVector. */
bool CallWithVector(UObject* Object, const TCHAR* Function, const FVector& Value)
{
	UFunction* Fn = Object->FindFunction(FName(Function));
	if (!Fn || Fn->ParmsSize != sizeof(FVector))
	{
		return false;
	}
	FVector Params = Value;
	// Actors skip ProcessEvent in an editor world without the guard.
	FEditorScriptExecutionGuard ScriptGuard;
	Object->ProcessEvent(Fn, &Params);
	return true;
}

TSharedPtr<FJsonValue> SetGeoreference(const TSharedPtr<FJsonObject>& Params, FString& OutError)
{
	TRACE_CPUPROFILER_EVENT_SCOPE(LilaLinkGeoreference::SetGeoreference);

	FString Crs;
	const TArray<TSharedPtr<FJsonValue>>* Origin = nullptr;
	const TSharedPtr<FJsonObject>* Wgs84 = nullptr;
	if (!Params->TryGetStringField(TEXT("crs"), Crs) || !Params->TryGetArrayField(TEXT("origin"), Origin) || Origin->Num() != 3 ||
		!Params->TryGetObjectField(TEXT("wgs84"), Wgs84))
	{
		OutError = TEXT("setGeoreference: give crs, origin [easting, northing, height] and wgs84 {latitude, longitude, height}");
		return nullptr;
	}
	UWorld* World = GeoEditorWorld();
	if (!World)
	{
		OutError = TEXT("setGeoreference: no editor world");
		return nullptr;
	}
	FString Authority;
	Params->TryGetStringField(TEXT("authority"), Authority);
	const FString ProjectedCrs = Authority.IsEmpty() ? Crs : Authority;
	const double Easting = (*Origin)[0]->AsNumber(), Northing = (*Origin)[1]->AsNumber(), Up = (*Origin)[2]->AsNumber();
	const double Latitude = (*Wgs84)->GetNumberField(TEXT("latitude"));
	const double Longitude = (*Wgs84)->GetNumberField(TEXT("longitude"));
	const double Height = (*Wgs84)->GetNumberField(TEXT("height"));

	const FScopedTransaction Transaction(LOCTEXT("SetGeoreference", "Lila Georeference"));
	TArray<FString> Done, Missing;
	bool bGeoSystem = false, bCesium = false;

	UClass* CesiumClass = FindObject<UClass>(nullptr, CesiumGeoreferenceClass);
	if (UClass* Class = FindObject<UClass>(nullptr, GeoReferencingSystemClass))
	{
		bool bSpawned = false;
		if (AActor* Actor = FindOrSpawn(World, Class, bSpawned))
		{
			Actor->Modify();
			// Alone, a flat planet on the projected grid, which Lila's scene is laid out on. Beside Cesium, which lays the
			// level on the globe's tangent plane at the origin, a round planet with that origin so both agree; Lila then
			// sends positions in that frame.
			bGeoSystem = SetProperty(Actor, TEXT("PlanetShape"), CesiumClass ? TEXT("RoundPlanet") : TEXT("FlatPlanet"));
			bGeoSystem &= SetProperty(Actor, TEXT("ProjectedCRS"), ProjectedCrs);
			SetProperty(Actor, TEXT("GeographicCRS"), TEXT("EPSG:4326"));
			SetProperty(Actor, TEXT("bOriginAtPlanetCenter"), TEXT("False"));
			SetProperty(Actor, TEXT("bOriginLocationInProjectedCRS"), CesiumClass ? TEXT("False") : TEXT("True"));
			SetProperty(Actor, TEXT("OriginProjectedCoordinatesEasting"), FString::SanitizeFloat(Easting));
			SetProperty(Actor, TEXT("OriginProjectedCoordinatesNorthing"), FString::SanitizeFloat(Northing));
			SetProperty(Actor, TEXT("OriginProjectedCoordinatesUp"), FString::SanitizeFloat(Up));
			SetProperty(Actor, TEXT("OriginLatitude"), FString::SanitizeFloat(Latitude));
			SetProperty(Actor, TEXT("OriginLongitude"), FString::SanitizeFloat(Longitude));
			SetProperty(Actor, TEXT("OriginAltitude"), FString::SanitizeFloat(Height));
			Actor->PostEditChange();
			if (UFunction* Apply = Actor->FindFunction(TEXT("ApplySettings")))
			{
				FEditorScriptExecutionGuard ScriptGuard;
				Actor->ProcessEvent(Apply, nullptr);
			}
			Done.Add(FString::Printf(TEXT("GeoReferencingSystem%s set to %s"), bSpawned ? TEXT(" (new)") : TEXT(""), *ProjectedCrs));
		}
	}
	else
	{
		Missing.Add(TEXT("GeoReferencing"));
	}

	if (UClass* Class = CesiumClass)
	{
		bool bSpawned = false;
		if (AActor* Actor = FindOrSpawn(World, Class, bSpawned))
		{
			Actor->Modify();
			SetProperty(Actor, TEXT("OriginPlacement"), TEXT("CartographicOrigin"));
			bCesium = CallWithVector(Actor, TEXT("SetOriginLongitudeLatitudeHeight"), FVector(Longitude, Latitude, Height));
			if (!bCesium)
			{
				bCesium = SetProperty(Actor, TEXT("OriginLongitude"), FString::SanitizeFloat(Longitude)) &&
						  SetProperty(Actor, TEXT("OriginLatitude"), FString::SanitizeFloat(Latitude)) &&
						  SetProperty(Actor, TEXT("OriginHeight"), FString::SanitizeFloat(Height));
			}
			Actor->PostEditChange();
			Done.Add(FString::Printf(TEXT("CesiumGeoreference%s at %.7f, %.7f, %.2f m"), bSpawned ? TEXT(" (new)") : TEXT(""), Latitude, Longitude, Height));
		}
	}
	else
	{
		Missing.Add(TEXT("Cesium for Unreal"));
	}

	FString Message = Done.IsEmpty() ? TEXT("Neither GeoReferencing nor Cesium for Unreal is enabled: the level's origin is the scene's origin, with no place on the globe")
									 : FString::Join(Done, TEXT("; "));
	if (!Done.IsEmpty() && !Missing.IsEmpty())
	{
		Message += FString::Printf(TEXT(" (%s not enabled)"), *FString::Join(Missing, TEXT(", ")));
	}
	UE_LOG(LogLilaLink, Log, TEXT("setGeoreference: %s"), *Message);

	TSharedPtr<FJsonObject> Out = MakeShared<FJsonObject>();
	Out->SetBoolField(TEXT("geoReferencingSystem"), bGeoSystem);
	Out->SetBoolField(TEXT("cesium"), bCesium);
	Out->SetStringField(TEXT("message"), Message);
	return MakeShared<FJsonValueObject>(Out);
}
/** Imports a .las through the LiDAR Point Cloud plugin and places an actor showing it at its own coordinates. */
TSharedPtr<FJsonValue> ImportPointCloud(const TSharedPtr<FJsonObject>& Params, FString& OutError)
{
	TRACE_CPUPROFILER_EVENT_SCOPE(LilaLinkGeoreference::ImportPointCloud);

	FString File, Folder = TEXT("/Game/Lila/PointClouds"), Name, Label;
	if (!Params->TryGetStringField(TEXT("file"), File))
	{
		OutError = TEXT("importPointCloud: give file");
		return nullptr;
	}
	Params->TryGetStringField(TEXT("folder"), Folder);
	Params->TryGetStringField(TEXT("name"), Name);
	Label = Name;
	Params->TryGetStringField(TEXT("label"), Label);
	UClass* ActorClass = FindObject<UClass>(nullptr, TEXT("/Script/LidarPointCloudRuntime.LidarPointCloudActor"));
	if (!ActorClass)
	{
		OutError = TEXT("importPointCloud: the LiDAR Point Cloud plugin is not enabled in this project");
		return nullptr;
	}
	UWorld* World = GeoEditorWorld();
	if (!World)
	{
		OutError = TEXT("importPointCloud: no editor world");
		return nullptr;
	}
	// The import runs on a worker by default and fills in the cloud's offsets when it ends: run it here instead, so the
	// offsets exist when this returns.
	UObject* Settings = nullptr;
	FBoolProperty* Async = nullptr;
	bool bWasAsync = false;
	if (UClass* SettingsClass = FindObject<UClass>(nullptr, TEXT("/Script/LidarPointCloudRuntime.LidarPointCloudSettings")))
	{
		Settings = SettingsClass->GetDefaultObject();
		Async = CastField<FBoolProperty>(SettingsClass->FindPropertyByName(TEXT("bUseAsyncImport")));
		if (Async)
		{
			bWasAsync = Async->GetPropertyValue_InContainer(Settings);
			Async->SetPropertyValue_InContainer(Settings, false);
		}
	}
	UAssetImportTask* Task = NewObject<UAssetImportTask>();
	Task->Filename = File;
	Task->DestinationPath = Folder;
	Task->DestinationName = Name;
	Task->bAutomated = true;
	Task->bReplaceExisting = true;
	Task->bSave = false;
	FAssetToolsModule::GetModule().Get().ImportAssetTasks({Task});
	if (Async)
	{
		Async->SetPropertyValue_InContainer(Settings, bWasAsync);
	}
	UObject* Cloud = nullptr;
	for (UObject* Object : Task->GetObjects())
	{
		if (Object && Object->GetClass()->GetName() == TEXT("LidarPointCloud"))
		{
			Cloud = Object;
		}
	}
	if (!Cloud)
	{
		OutError = TEXT("importPointCloud: Unreal made no point cloud from ") + File + TEXT("; see the editor log");
		return nullptr;
	}
	// The plugin centres the points for precision; the file's coordinates are the level's, so draw them there.
	if (UFunction* Restore = Cloud->FindFunction(TEXT("RestoreOriginalCoordinates")))
	{
		Cloud->ProcessEvent(Restore, nullptr);
	}
	const FScopedTransaction Transaction(LOCTEXT("ImportPointCloud", "Lila Point Cloud"));
	AActor* Actor = nullptr;
	for (TActorIterator<AActor> It(World, ActorClass); It; ++It)
	{
		if (It->GetActorLabel() == Label)
		{
			Actor = *It;
		}
	}
	if (!Actor)
	{
		Actor = World->SpawnActor(ActorClass);
		Actor->SetActorLabel(Label);
	}
	Actor->Modify();
	bool bSet = false;
	if (FObjectProperty* Prop = CastField<FObjectProperty>(ActorClass->FindPropertyByName(TEXT("PointCloudComponent"))))
	{
		if (UObject* Component = Prop->GetObjectPropertyValue_InContainer(Actor))
		{
			if (UFunction* Set = Component->FindFunction(TEXT("SetPointCloud")))
			{
				struct
				{
					UObject* InPointCloud;
				} Args{Cloud};
				Component->ProcessEvent(Set, &Args);
				bSet = true;
			}
		}
	}
	Actor->SetActorLocation(FVector::ZeroVector);
	Actor->PostEditChange();
	TSharedPtr<FJsonObject> Out = MakeShared<FJsonObject>();
	Out->SetStringField(TEXT("asset"), Cloud->GetPathName());
	Out->SetStringField(TEXT("actor"), Actor->GetActorLabel());
	Out->SetBoolField(TEXT("assigned"), bSet);
	return MakeShared<FJsonValueObject>(Out);
}
} // namespace

void AddGeoreferenceMethods(TMap<FString, FLilaLinkMethod>& Methods)
{
	Methods.Add(TEXT("setGeoreference"), LilaLink::Sync(&SetGeoreference));
	Methods.Add(TEXT("importPointCloud"), LilaLink::Sync(&ImportPointCloud));
}
} // namespace LilaLink

#undef LOCTEXT_NAMESPACE
