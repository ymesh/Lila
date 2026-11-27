#include "LilaLinkPlacements.h"

#include "LilaInstanceData.h"
#include "LilaSlotTable.h"
#include "LilaLinkAssets.h"
#include "LilaLinkComponents.h"
#include "LilaLinkLog.h"

#include "AssetRegistry/AssetRegistryModule.h"
#include "AssetRegistry/IAssetRegistry.h"
#include "Components/DirectionalLightComponent.h"
#include "Components/HierarchicalInstancedStaticMeshComponent.h"
#include "Components/InstancedStaticMeshComponent.h"
#include "Components/LocalLightComponent.h"
#include "Components/PointLightComponent.h"
#include "Components/RectLightComponent.h"
#include "Components/SpotLightComponent.h"
#include "Components/StaticMeshComponent.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Editor.h"
#include "EditorLevelUtils.h"
#include "Engine/Brush.h"
#include "Engine/CollisionProfile.h"
#include "Engine/DirectionalLight.h"
#include "Engine/LevelStreamingDynamic.h"
#include "Engine/PointLight.h"
#include "Engine/RectLight.h"
#include "Engine/Scene.h"
#include "Engine/SpotLight.h"
#include "Engine/StaticMesh.h"
#include "Engine/StaticMeshActor.h"
#include "Engine/World.h"
#include "EngineUtils.h"
#include "UObject/UObjectIterator.h"
#include "UObject/Linker.h"
#include "Engine/Level.h"
#include "ExternalPackageHelper.h"
#include "FileHelpers.h"
#include "GameFramework/WorldSettings.h"
#include "HAL/FileManager.h"
#include "DataLayer/DataLayerEditorSubsystem.h"
#include "LevelInstance/LevelInstanceActor.h"
#include "WorldPartition/DataLayer/DataLayerAsset.h"
#include "WorldPartition/DataLayer/DataLayerInstance.h"
#include "LevelInstance/LevelInstanceInterface.h"
#include "LevelInstance/LevelInstanceSubsystem.h"
#include "LevelInstance/LevelInstanceTypes.h"
#include "Misc/PackageName.h"
#include "ObjectTools.h"
#include "PackedLevelActor/PackedLevelActor.h"
#include "PackedLevelActor/PackedLevelActorBuilder.h"
#include "PhysicalMaterials/PhysicalMaterial.h"
#include "PhysicsEngine/BodySetup.h"
#include "ProfilingDebugging/CpuProfilerTrace.h"
#include "ScopedTransaction.h"
#include "WorldPartition/HLOD/CustomHLODActor.h"
#include "WorldPartition/HLOD/HLODLayer.h"
#include "WorldPartition/WorldPartition.h"

#define LOCTEXT_NAMESPACE "LilaLinkPlacements"

namespace
{
const TCHAR* GroupPrefix = TEXT("LilaGroup:");
const TCHAR* NodePrefix = TEXT("LilaNode:");
const FName InstancedTag(TEXT("LilaInstanced"));
const FName LightTag(TEXT("LilaLight"));
const TCHAR* AssemblyPrefix = TEXT("LilaAssembly:");
const TCHAR* HashPrefix = TEXT("LilaHash:");

TSharedPtr<FJsonValue> Obj(const TSharedRef<FJsonObject>& O) { return MakeShared<FJsonValueObject>(O); }

UWorld* EditorWorld()
{
	return GEditor ? GEditor->GetEditorWorldContext().World() : nullptr;
}

FName GroupTag(const FString& Group) { return FName(*(FString(GroupPrefix) + Group)); }
FName NodeTag(const FString& Id) { return FName(*(FString(NodePrefix) + Id)); }

/** The value after a tag prefix on the actor, or empty. */
FString TagValue(const AActor* Actor, const TCHAR* Prefix)
{
	for (const FName& Tag : Actor->Tags)
	{
		FString S = Tag.ToString();
		if (S.StartsWith(Prefix))
		{
			return S.RightChop(FCString::Strlen(Prefix));
		}
	}
	return FString();
}

/** A node id as Lila sends it (a number), or a string id. */
FString IdOf(const TSharedPtr<FJsonObject>& P)
{
	TSharedPtr<FJsonValue> V = P->TryGetField(TEXT("id"));
	if (!V.IsValid())
	{
		return FString();
	}
	if (V->Type == EJson::Number)
	{
		return FString::Printf(TEXT("%lld"), static_cast<long long>(V->AsNumber()));
	}
	return V->AsString();
}

FVector VectorField(const TSharedPtr<FJsonObject>& P, const TCHAR* Field, const FVector& Fallback)
{
	const TArray<TSharedPtr<FJsonValue>>* Values = nullptr;
	if (!P->TryGetArrayField(Field, Values) || Values->Num() != 3)
	{
		return Fallback;
	}
	return FVector((*Values)[0]->AsNumber(), (*Values)[1]->AsNumber(), (*Values)[2]->AsNumber());
}

/** The placement's custom primitive data: floats a material reads per actor or instance (index 0 is a stable random). */
TArray<float> CustomDataOf(const TSharedPtr<FJsonObject>& P)
{
	TArray<float> Out;
	const TArray<TSharedPtr<FJsonValue>>* Values = nullptr;
	if (P->TryGetArrayField(TEXT("customData"), Values))
	{
		for (const TSharedPtr<FJsonValue>& V : *Values)
		{
			Out.Add(static_cast<float>(V->AsNumber()));
		}
	}
	return Out;
}

/** The placement's Lighting Channels 0, 1 and 2, when Lila sent them. */
TOptional<FLightingChannels> ChannelsOf(const TSharedPtr<FJsonObject>& P)
{
	const TArray<TSharedPtr<FJsonValue>>* Values = nullptr;
	if (!P->TryGetArrayField(TEXT("lightingChannels"), Values) || Values->Num() != 3)
	{
		return {};
	}
	FLightingChannels Channels;
	Channels.bChannel0 = (*Values)[0]->AsBool();
	Channels.bChannel1 = (*Values)[1]->AsBool();
	Channels.bChannel2 = (*Values)[2]->AsBool();
	return Channels;
}

/** Collision "none" turns the placed components' collision off; anything else blocks like a placed static mesh. */
void SetCollides(UPrimitiveComponent* Component, bool bCollide)
{
	if (bCollide)
	{
		Component->SetCollisionProfileName(UCollisionProfile::BlockAll_ProfileName);
	}
	else
	{
		Component->SetCollisionProfileName(UCollisionProfile::NoCollision_ProfileName);
	}
}

bool SameChannels(const FLightingChannels& A, const FLightingChannels& B)
{
	return A.bChannel0 == B.bChannel0 && A.bChannel1 == B.bChannel1 && A.bChannel2 == B.bChannel2;
}

/** location (cm), rotation (quat x y z w) and scale, already in Unreal's axes. */
FTransform TransformOf(const TSharedPtr<FJsonObject>& P)
{
	FQuat Rotation = FQuat::Identity;
	const TArray<TSharedPtr<FJsonValue>>* Q = nullptr;
	if (P->TryGetArrayField(TEXT("rotation"), Q) && Q->Num() == 4)
	{
		Rotation = FQuat((*Q)[0]->AsNumber(), (*Q)[1]->AsNumber(), (*Q)[2]->AsNumber(), (*Q)[3]->AsNumber());
		Rotation.Normalize();
	}
	return FTransform(Rotation, VectorField(P, TEXT("location"), FVector::ZeroVector), VectorField(P, TEXT("scale"), FVector::OneVector));
}

/** An actor of a loaded Level Instance's level: part of that instance, edited only by rebuilding it (makeLevelInstance). */
bool InsideLevelInstance(UWorld* World, const AActor* Actor)
{
	const ULevelInstanceSubsystem* Instances = World->GetSubsystem<ULevelInstanceSubsystem>();
	return Instances && Actor->GetLevel() != World->PersistentLevel && Instances->GetOwningLevelInstance(Actor->GetLevel());
}

/** The level's actors with Tag, leaving out those inside Level Instances. */
TArray<AActor*> TaggedActors(UWorld* World, const FName& Tag)
{
	TArray<AActor*> Out;
	for (TActorIterator<AActor> It(World); It; ++It)
	{
		if (It->Tags.Contains(Tag) && !InsideLevelInstance(World, *It))
		{
			Out.Add(*It);
		}
	}
	return Out;
}

/** What one removal took away: an instanced holder counts its instances, an actor counts one. */
int32 Weight(AActor* Actor)
{
	if (Actor->Tags.Contains(InstancedTag))
	{
		if (const UInstancedStaticMeshComponent* C = Actor->FindComponentByClass<UInstancedStaticMeshComponent>())
		{
			return C->GetInstanceCount();
		}
	}
	return 1;
}

int32 DestroyActors(UWorld* World, const TArray<AActor*>& Actors)
{
	int32 Removed = 0;
	for (AActor* Actor : Actors)
	{
		Removed += Weight(Actor);
		World->EditorDestroyActor(Actor, true);
	}
	return Removed;
}

TSharedPtr<FJsonValue> StringArray(const TArray<FString>& Strings)
{
	TArray<TSharedPtr<FJsonValue>> Values;
	for (const FString& S : Strings)
	{
		Values.Add(MakeShared<FJsonValueString>(S));
	}
	return MakeShared<FJsonValueArray>(Values);
}

/**
 * Deletes the level files (with their external actors and objects) of removed Level Instances that no instance left in
 * World or in another level shows; OutKept says which stayed and why. Returns the packages deleted.
 */
TArray<FString> DeleteUnusedAssemblyLevels(UWorld* World, const TArray<FString>& Levels, TArray<FString>& OutKept, TArray<FString>& OutErrors)
{
	TRACE_CPUPROFILER_EVENT_SCOPE(LilaLinkPlacements::DeleteUnusedAssemblyLevels);

	TArray<FString> Unused;
	for (const FString& Level : Levels)
	{
		bool bInUse = Level.IsEmpty() || !FPackageName::DoesPackageExist(Level);
		for (TActorIterator<ALevelInstance> It(World); It && !bInUse; ++It)
		{
			bInUse = IsValid(*It) && It->GetWorldAsset().GetLongPackageName() == Level;
		}
		// Another level's Level Instance of the same interior (sends to several levels share its level) keeps it too.
		if (const FString By = bInUse ? FString() : LilaLink::UsedOutside(World, Level); !By.IsEmpty())
		{
			bInUse = true;
			OutKept.Add(FString::Printf(TEXT("%s: %s uses it"), *Level, *By));
		}
		if (!bInUse)
		{
			Unused.Add(Level);
		}
	}
	if (Unused.IsEmpty())
	{
		return {};
	}
	// The level and its external packages are known unused: nothing to load or scan for references, so the files go
	// directly once no package in memory (the unloaded instance's /Temp copy included) still reads them.
	auto Folders = [](const FString& Level) {
		TArray<FString> Out = ULevel::GetExternalActorsPaths(Level);
		Out.Add(FExternalPackageHelper::GetExternalObjectsPath(Level));
		return Out;
	};
	TArray<UPackage*> InMemory;
	for (TObjectIterator<UPackage> It; It; ++It)
	{
		const FString Name = It->GetName(), From = It->GetLoadedPath().GetPackageName();
		for (const FString& Level : Unused)
		{
			const bool bOwn = Name == Level || From == Level ||
							  Folders(Level).ContainsByPredicate([&Name](const FString& F) { return Name.StartsWith(F + TEXT("/")); });
			if (bOwn)
			{
				InMemory.Add(*It);
				break;
			}
		}
	}
	for (UPackage* Package : InMemory)
	{
		ResetLoaders(Package);
	}
	TArray<FString> Deleted;
	for (const FString& Level : Unused)
	{
		FString File;
		const bool bFile = FPackageName::DoesPackageExist(Level, &File);
		if (bFile && !IFileManager::Get().Delete(*File, /*RequireExists=*/false, /*EvenReadOnly=*/true))
		{
			OutErrors.Add(FString::Printf(TEXT("%s could not be deleted (in use); delete it in the Content Browser"), *Level));
			continue;
		}
		IAssetRegistry::GetChecked().ScanModifiedAssetFiles({File});
		for (const FString& Folder : Folders(Level))
		{
			FString Dir;
			if (FPackageName::TryConvertLongPackageNameToFilename(Folder, Dir) && IFileManager::Get().DirectoryExists(*Dir))
			{
				if (!IFileManager::Get().DeleteDirectory(*Dir, /*RequireExists=*/false, /*Tree=*/true))
				{
					OutErrors.Add(FString::Printf(TEXT("%s: some of %s could not be deleted"), *Level, *Folder));
				}
				IAssetRegistry::GetChecked().ScanPathsSynchronous({Folder}, /*bForceRescan=*/true);
			}
		}
		Deleted.Add(Level);
	}
	return Deleted;
}

FString ShortGroupName(const FString& Group)
{
	int32 Bar = INDEX_NONE;
	return Group.FindLastChar(TEXT('|'), Bar) ? Group.RightChop(Bar + 1) : Group;
}

/** Where placed actors' attribute components go: data assets beside the mesh, warnings and assets made back to Lila. */
struct FComponentSink
{
	FString Folder;
	TArray<FString> Warnings;
	TArray<FString> Assets;
};

void ApplyPlacementComponents(AActor* Actor, const TSharedPtr<FJsonObject>& Placement, FComponentSink& Sink)
{
	TArray<LilaLink::FLilaComponent> Components;
	if (LilaLink::ParseComponents(Placement, Components))
	{
		LilaLink::ApplyComponentsToActor(Actor, Components, Sink.Folder, Sink.Warnings, Sink.Assets);
	}
}

/** The physical material for a Lila body material, made beside the meshes the first time: PM_<material>. */
UPhysicalMaterial* PhysicalMaterialFor(const TSharedPtr<FJsonObject>& Physics, const FString& Folder)
{
	TRACE_CPUPROFILER_EVENT_SCOPE(LilaLinkPlacements::PhysicalMaterialFor);

	FString Material;
	if (!Physics->TryGetStringField(TEXT("material"), Material) || Material.IsEmpty())
	{
		return nullptr;
	}
	const FString Name = TEXT("PM_") + Material;
	const FString PackageName = Folder / TEXT("PhysicalMaterials") / Name;
	UPhysicalMaterial* PM = LoadObject<UPhysicalMaterial>(nullptr, *(PackageName + TEXT(".") + Name), nullptr, LOAD_NoWarn | LOAD_Quiet);
	if (!PM)
	{
		UPackage* Package = CreatePackage(*PackageName);
		PM = NewObject<UPhysicalMaterial>(Package, *Name, RF_Public | RF_Standalone | RF_Transactional);
		FAssetRegistryModule::AssetCreated(PM);
	}
	double Friction = PM->Friction, Restitution = PM->Restitution, Density = PM->Density * 1000.0;
	Physics->TryGetNumberField(TEXT("friction"), Friction);
	Physics->TryGetNumberField(TEXT("restitution"), Restitution);
	Physics->TryGetNumberField(TEXT("density"), Density);
	PM->Modify();
	PM->Friction = static_cast<float>(Friction);
	PM->Restitution = static_cast<float>(Restitution);
	// Lila's density is kg/m3, Unreal's g/cm3.
	PM->Density = static_cast<float>(Density / 1000.0);
	PM->MarkPackageDirty();
	return PM;
}

/**
 * A placement's body settings on its component: the physical material, and for an actor of its own whether it
 * simulates and its mass. Instances share one component, which can't simulate per instance.
 */
void ApplyPhysics(UPrimitiveComponent* Component, const TSharedPtr<FJsonObject>& Placement, const FString& Folder, bool bInstanced, const FString& Label,
	FComponentSink& Sink)
{
	TRACE_CPUPROFILER_EVENT_SCOPE(LilaLinkPlacements::ApplyPhysics);

	const TSharedPtr<FJsonObject>* Physics = nullptr;
	if (!Placement->TryGetObjectField(TEXT("physics"), Physics))
	{
		return;
	}
	if (UPhysicalMaterial* PM = PhysicalMaterialFor(*Physics, Folder))
	{
		Component->SetPhysMaterialOverride(PM);
	}
	bool bSimulate = false;
	(*Physics)->TryGetBoolField(TEXT("simulate"), bSimulate);
	if (bInstanced)
	{
		if (bSimulate)
		{
			Sink.Warnings.Add(FString::Printf(TEXT("%s: instanced members can't simulate physics; send it as actors"), *Label));
		}
		return;
	}
	if (bSimulate)
	{
		Component->SetMobility(EComponentMobility::Movable);
	}
	Component->SetSimulatePhysics(bSimulate);
	double Mass = 0.0;
	if ((*Physics)->TryGetNumberField(TEXT("mass"), Mass) && Mass > 0.0)
	{
		Component->SetMassOverrideInKg(NAME_None, static_cast<float>(Mass), true);
	}
}

/** The mesh collides with its UCX_ hulls, or with its own triangles when the body is a static triangle mesh. */
void ApplyMeshCollision(UStaticMesh* Mesh, const TArray<TSharedPtr<FJsonValue>>& Placements)
{
	TRACE_CPUPROFILER_EVENT_SCOPE(LilaLinkPlacements::ApplyMeshCollision);

	const TSharedPtr<FJsonObject>* First = nullptr;
	const TSharedPtr<FJsonObject>* Physics = nullptr;
	FString Collision;
	if (Placements.IsEmpty() || !Placements[0].IsValid() || !Placements[0]->TryGetObject(First) || !(*First)->TryGetObjectField(TEXT("physics"), Physics) ||
		!(*Physics)->TryGetStringField(TEXT("collision"), Collision))
	{
		return;
	}
	UBodySetup* Body = Mesh->GetBodySetup();
	if (!Body)
	{
		return;
	}
	const ECollisionTraceFlag Flag = Collision == TEXT("complexAsSimple") ? CTF_UseComplexAsSimple : CTF_UseDefault;
	if (Body->CollisionTraceFlag != Flag)
	{
		Body->Modify();
		Body->CollisionTraceFlag = Flag;
		Body->InvalidatePhysicsData();
		Body->CreatePhysicsMeshes();
		Mesh->MarkPackageDirty();
	}
}

/** Replaces the actor's Lila slot tags (LilaSlot:, LilaSlotType:, LilaSlotData:) with the placement's tags. */
void ApplySlotTags(AActor* Actor, const TSharedPtr<FJsonObject>& P)
{
	Actor->Tags.RemoveAll([](const FName& Tag) { return Tag.ToString().StartsWith(TEXT("LilaSlot")); });
	const TArray<TSharedPtr<FJsonValue>>* Tags = nullptr;
	if (!P->TryGetArrayField(TEXT("tags"), Tags))
	{
		return;
	}
	for (const TSharedPtr<FJsonValue>& T : *Tags)
	{
		FString Tag;
		if (T.IsValid() && T->TryGetString(Tag) && !Tag.IsEmpty())
		{
			Actor->Tags.AddUnique(FName(*Tag));
		}
	}
}

/**
 * An actor of Class per placement (a slot's Blueprint: an interactable door, a lamp with a light), matched to the
 * existing ones by node id; an actor of another class under the same id is replaced.
 */
void PlaceClassActors(UWorld* World, const FString& Group, const FString& Label, UClass* Class, const TArray<TSharedPtr<FJsonValue>>& Placements,
	TArray<AActor*> Existing, int32& Spawned, int32& Updated, int32& Removed, FComponentSink& Sink)
{
	TRACE_CPUPROFILER_EVENT_SCOPE(LilaLinkPlacements::PlaceClassActors);

	TMap<FString, AActor*> ByNode;
	for (AActor* A : Existing)
	{
		if (A->GetClass() != Class || A->Tags.Contains(InstancedTag))
		{
			World->EditorDestroyActor(A, true);
			++Removed;
			continue;
		}
		ByNode.Add(TagValue(A, NodePrefix), A);
	}
	const FName Folder(*(TEXT("Lila/") + ShortGroupName(Label)));
	TSet<FString> Sent;
	for (const TSharedPtr<FJsonValue>& V : Placements)
	{
		const TSharedPtr<FJsonObject>* P = nullptr;
		if (!V.IsValid() || !V->TryGetObject(P))
		{
			continue;
		}
		const FString Id = IdOf(*P);
		Sent.Add(Id);
		const FTransform Transform = TransformOf(*P);
		AActor*& Actor = ByNode.FindOrAdd(Id);
		if (!Actor)
		{
			FActorSpawnParameters Params;
			Params.ObjectFlags |= RF_Transactional;
			Actor = World->SpawnActor<AActor>(Class, Transform, Params);
			if (!Actor)
			{
				Sink.Warnings.Add(FString::Printf(TEXT("%s: could not spawn %s"), *Id, *Class->GetName()));
				continue;
			}
			Actor->Tags.Add(GroupTag(Group));
			Actor->Tags.Add(NodeTag(Id));
			++Spawned;
		}
		else
		{
			Actor->Modify();
			Actor->SetActorTransform(Transform);
			++Updated;
		}
		Actor->SetFolderPath(Folder);
		FString Name;
		if ((*P)->TryGetStringField(TEXT("name"), Name))
		{
			Actor->SetActorLabel(Name);
		}
		ApplySlotTags(Actor, *P);
		ApplyPlacementComponents(Actor, *P, Sink);
	}
	for (const TPair<FString, AActor*>& Pair : ByNode)
	{
		if (Pair.Value && !Sent.Contains(Pair.Key))
		{
			World->EditorDestroyActor(Pair.Value, true);
			++Removed;
		}
	}
}

/** An actor per placement, matched to the existing ones by node id. */
void PlaceActors(UWorld* World, const FString& Group, const FString& Label, UStaticMesh* Mesh, bool bCollide, const TArray<TSharedPtr<FJsonValue>>& Placements,
	TArray<AActor*> Existing, int32& Spawned, int32& Updated, int32& Removed, FComponentSink& Sink)
{
	TRACE_CPUPROFILER_EVENT_SCOPE(LilaLinkPlacements::PlaceActors);

	TArray<AActor*> Holders = Existing.FilterByPredicate([](const AActor* A) { return A->Tags.Contains(InstancedTag); });
	Removed += DestroyActors(World, Holders);
	TMap<FString, AStaticMeshActor*> ByNode;
	for (AActor* A : Existing)
	{
		if (AStaticMeshActor* S = Cast<AStaticMeshActor>(A); S && !A->Tags.Contains(InstancedTag))
		{
			ByNode.Add(TagValue(S, NodePrefix), S);
		}
	}
	const FName Folder(*(TEXT("Lila/") + ShortGroupName(Label)));
	TSet<FString> Sent;
	for (const TSharedPtr<FJsonValue>& V : Placements)
	{
		const TSharedPtr<FJsonObject>* P = nullptr;
		if (!V.IsValid() || !V->TryGetObject(P))
		{
			continue;
		}
		const FString Id = IdOf(*P);
		Sent.Add(Id);
		const FTransform Transform = TransformOf(*P);
		AStaticMeshActor*& Actor = ByNode.FindOrAdd(Id);
		if (!Actor)
		{
			FActorSpawnParameters Params;
			Params.ObjectFlags |= RF_Transactional;
			Actor = World->SpawnActor<AStaticMeshActor>(AStaticMeshActor::StaticClass(), Transform, Params);
			if (!Actor)
			{
				continue;
			}
			Actor->Tags.Add(GroupTag(Group));
			Actor->Tags.Add(NodeTag(Id));
			Actor->SetFolderPath(Folder);
			++Spawned;
		}
		else
		{
			Actor->Modify();
			Actor->SetActorTransform(Transform);
			Actor->SetFolderPath(Folder);
			++Updated;
		}
		FString Name;
		if ((*P)->TryGetStringField(TEXT("name"), Name))
		{
			Actor->SetActorLabel(Name);
		}
		Actor->GetStaticMeshComponent()->Modify();
		Actor->GetStaticMeshComponent()->SetStaticMesh(Mesh);
		SetCollides(Actor->GetStaticMeshComponent(), bCollide);
		const TArray<float> Custom = CustomDataOf(*P);
		// The defaults are what the level saves; SetCustomPrimitiveDataFloat only writes the transient runtime copy.
		for (int32 K = 0; K < Custom.Num(); ++K)
		{
			Actor->GetStaticMeshComponent()->SetDefaultCustomPrimitiveDataFloat(K, Custom[K]);
		}
		if (const TOptional<FLightingChannels> Channels = ChannelsOf(*P))
		{
			Actor->GetStaticMeshComponent()->SetLightingChannels(Channels->bChannel0, Channels->bChannel1, Channels->bChannel2);
		}
		ApplySlotTags(Actor, *P);
		ApplyPlacementComponents(Actor, *P, Sink);
		ApplyPhysics(Actor->GetStaticMeshComponent(), *P, Sink.Folder, false, Label, Sink);
	}
	for (const TPair<FString, AStaticMeshActor*>& Pair : ByNode)
	{
		if (Pair.Value && !Sent.Contains(Pair.Key))
		{
			World->EditorDestroyActor(Pair.Value, true);
			++Removed;
		}
	}
}

/** Instances share their component's Lighting Channels: the first placement's, with a warning when others differ. */
void ApplyInstancedChannels(UInstancedStaticMeshComponent* Component, const TSharedPtr<FJsonObject>& First, const TArray<TSharedPtr<FJsonValue>>& Placements,
	const FString& Label, FComponentSink& Sink)
{
	const TOptional<FLightingChannels> Channels = ChannelsOf(First);
	if (!Channels)
	{
		return;
	}
	Component->SetLightingChannels(Channels->bChannel0, Channels->bChannel1, Channels->bChannel2);
	for (const TSharedPtr<FJsonValue>& V : Placements)
	{
		const TSharedPtr<FJsonObject>* P = nullptr;
		if (V.IsValid() && V->TryGetObject(P))
		{
			const TOptional<FLightingChannels> Other = ChannelsOf(*P);
			if (Other && !SameChannels(*Other, *Channels))
			{
				Sink.Warnings.Add(FString::Printf(TEXT("%s: instances share one component, so all take the first member's lighting channels"), *Label));
				return;
			}
		}
	}
}

/**
 * The holder's ULilaInstanceData: every instance's key and component values by index, and the custom data slot names
 * from the group's customData [{name, slot, width}].
 */
void WriteInstanceData(AActor* Holder, const TSharedPtr<FJsonObject>& Params, const TArray<TSharedPtr<FJsonValue>>& Placements)
{
	TRACE_CPUPROFILER_EVENT_SCOPE(LilaLinkPlacements::WriteInstanceData);

	ULilaInstanceData* Data = ULilaInstanceData::FindOn(Holder);
	if (!Data)
	{
		Data = NewObject<ULilaInstanceData>(Holder, ULilaInstanceData::StaticClass(), TEXT("LilaInstanceData"), RF_Transactional);
		Holder->AddInstanceComponent(Data);
		Data->RegisterComponent();
	}
	Data->Modify();
	Data->InstanceIds.Reset(Placements.Num());
	Data->Columns.Reset();
	Data->CustomDataNames.Reset();
	const int32 Count = Placements.Num();
	for (int32 Index = 0; Index < Count; ++Index)
	{
		const TSharedPtr<FJsonObject>* P = nullptr;
		if (!Placements[Index].IsValid() || !Placements[Index]->TryGetObject(P))
		{
			Data->InstanceIds.Add(FString());
			continue;
		}
		Data->InstanceIds.Add(IdOf(*P));
		// Slots: their type and gameplay data, so game code reads an instanced door's data like an actor's tags.
		FString SlotType;
		if ((*P)->TryGetStringField(TEXT("slotType"), SlotType))
		{
			FLilaInstanceColumn& Column = Data->Columns.FindOrAdd(TEXT("slotType"));
			Column.Values.SetNum(Count);
			Column.Values[Index] = SlotType;
		}
		const TSharedPtr<FJsonObject>* SlotData = nullptr;
		if ((*P)->TryGetObjectField(TEXT("slotData"), SlotData))
		{
			for (const TPair<FString, TSharedPtr<FJsonValue>>& Value : (*SlotData)->Values)
			{
				FLilaInstanceColumn& Column = Data->Columns.FindOrAdd(FName(*(TEXT("slot.") + Value.Key)));
				Column.Values.SetNum(Count);
				Column.Values[Index] = LilaLink::ComponentValueText(Value.Value);
			}
		}
		TArray<LilaLink::FLilaComponent> Components;
		LilaLink::ParseComponents(*P, Components);
		for (const LilaLink::FLilaComponent& C : Components)
		{
			if (C.Mode == TEXT("none"))
			{
				continue;
			}
			for (const TPair<FString, TSharedPtr<FJsonValue>>& Attr : C.Attrs->Values)
			{
				FLilaInstanceColumn& Column = Data->Columns.FindOrAdd(FName(*(C.Name.IsEmpty() ? Attr.Key : C.Name + TEXT(".") + Attr.Key)));
				Column.Values.SetNum(Count);
				Column.Values[Index] = LilaLink::ComponentValueText(Attr.Value);
			}
		}
	}
	const TArray<TSharedPtr<FJsonValue>>* Slots = nullptr;
	if (Params->TryGetArrayField(TEXT("customData"), Slots))
	{
		for (const TSharedPtr<FJsonValue>& V : *Slots)
		{
			const TSharedPtr<FJsonObject>* Slot = nullptr;
			if (!V.IsValid() || !V->TryGetObject(Slot))
			{
				continue;
			}
			FString Name;
			(*Slot)->TryGetStringField(TEXT("name"), Name);
			int32 Width = 1;
			(*Slot)->TryGetNumberField(TEXT("width"), Width);
			for (int32 K = 0; K < Width; ++K)
			{
				Data->CustomDataNames.Add(FName(*(Width > 1 ? FString::Printf(TEXT("%s.%d"), *Name, K) : Name)));
			}
		}
	}
}

/**
 * One holder actor whose (H)ISM component carries every placement as an instance, rebuilt in order each time. Instances
 * can't hold components: the holder takes the components every instance shares (the group's "components"), and each
 * instance's own values go in its ULilaInstanceData.
 */
void PlaceInstanced(UWorld* World, const TSharedPtr<FJsonObject>& Params, const FString& Group, const FString& Label, UStaticMesh* Mesh, bool bHierarchical,
	bool bCollide, const TArray<TSharedPtr<FJsonValue>>& Placements, TArray<AActor*> Existing, int32& Spawned, int32& Updated, int32& Removed,
	FComponentSink& Sink)
{
	TRACE_CPUPROFILER_EVENT_SCOPE(LilaLinkPlacements::PlaceInstanced);

	AActor* Holder = nullptr;
	TArray<AActor*> Others;
	for (AActor* A : Existing)
	{
		if (!Holder && A->Tags.Contains(InstancedTag))
		{
			Holder = A;
		}
		else
		{
			Others.Add(A);
		}
	}
	Removed += DestroyActors(World, Others);
	if (!Holder)
	{
		FActorSpawnParameters SpawnParams;
		SpawnParams.ObjectFlags |= RF_Transactional;
		Holder = World->SpawnActor<AActor>(AActor::StaticClass(), FTransform::Identity, SpawnParams);
		if (!Holder)
		{
			return;
		}
		Holder->Tags.Add(GroupTag(Group));
		Holder->Tags.Add(InstancedTag);
		Holder->SetFolderPath(FName(TEXT("Lila")));
	}
	Holder->Modify();
	Holder->SetActorLabel(TEXT("Lila_") + ShortGroupName(Label));

	UInstancedStaticMeshComponent* Component = Holder->FindComponentByClass<UInstancedStaticMeshComponent>();
	const bool bRightKind = Component && Component->IsA<UHierarchicalInstancedStaticMeshComponent>() == bHierarchical;
	const int32 Before = Component && bRightKind ? Component->GetInstanceCount() : 0;
	if (Component && !bRightKind)
	{
		Removed += Component->GetInstanceCount();
		Holder->RemoveInstanceComponent(Component);
		Component->DestroyComponent();
		Component = nullptr;
	}
	if (!Component)
	{
		UClass* Class = bHierarchical ? UHierarchicalInstancedStaticMeshComponent::StaticClass() : UInstancedStaticMeshComponent::StaticClass();
		Component = NewObject<UInstancedStaticMeshComponent>(Holder, Class, MakeUniqueObjectName(Holder, Class, TEXT("LilaInstances")), RF_Transactional);
		Holder->SetRootComponent(Component);
		Holder->AddInstanceComponent(Component);
		Component->RegisterComponent();
	}
	Component->Modify();
	Component->SetStaticMesh(Mesh);
	SetCollides(Component, bCollide);
	Component->ClearInstances();
	// One bulk add and one custom data write: per-instance calls rebuild state each time and crawl at thousands of instances.
	TArray<FTransform> Transforms;
	TArray<TArray<float>> Custom;
	Transforms.Reserve(Placements.Num());
	Custom.Reserve(Placements.Num());
	int32 CustomFloats = 0;
	for (const TSharedPtr<FJsonValue>& V : Placements)
	{
		const TSharedPtr<FJsonObject>* P = nullptr;
		if (V.IsValid() && V->TryGetObject(P))
		{
			Transforms.Add(TransformOf(*P));
			CustomFloats = FMath::Max(CustomFloats, Custom.Add_GetRef(CustomDataOf(*P)).Num());
		}
	}
	const int32 Count = Transforms.Num();
	Component->SetNumCustomDataFloats(CustomFloats);
	Component->AddInstances(Transforms, false);
	if (CustomFloats > 0 && Count > 0)
	{
		// Every instance gets as many floats as the placement with the most, the missing ones 0.
		TArray<float> Flat;
		Flat.SetNumZeroed(Count * CustomFloats);
		for (int32 I = 0; I < Count; ++I)
		{
			FMemory::Memcpy(&Flat[I * CustomFloats], Custom[I].GetData(), Custom[I].Num() * sizeof(float));
		}
		Component->SetCustomData(0, Count - 1, Flat, true);
	}
	Updated += FMath::Min(Before, Count);
	Spawned += FMath::Max(0, Count - Before);
	Removed += FMath::Max(0, Before - Count);
	ApplyPlacementComponents(Holder, Params, Sink);
	WriteInstanceData(Holder, Params, Placements);
	const TSharedPtr<FJsonObject>* First = nullptr;
	if (!Placements.IsEmpty() && Placements[0].IsValid() && Placements[0]->TryGetObject(First))
	{
		ApplyInstancedChannels(Component, *First, Placements, Label, Sink);
		ApplyPhysics(Component, *First, Sink.Folder, true, Label, Sink);
	}
}

/** placeInstances with an actorClass: an actor of that class per placement, no mesh. */
/** Tags the group's actors with the signature Lila sent (params.hash), which placementHashes reads back. */
void StampHash(UWorld* World, const FString& Group, const TSharedPtr<FJsonObject>& Params)
{
	FString Hash;
	if (!Params->TryGetStringField(TEXT("hash"), Hash) || Hash.IsEmpty())
	{
		return;
	}
	const FName Tag(*(FString(HashPrefix) + Hash));
	for (AActor* Actor : TaggedActors(World, GroupTag(Group)))
	{
		if (Actor->Tags.Contains(Tag))
		{
			continue;
		}
		Actor->Tags.RemoveAll([](const FName& T) { return T.ToString().StartsWith(HashPrefix); });
		Actor->Tags.Add(Tag);
	}
}

/**
 * placementHashes {}: per Lila group in the open level, the signature its last placeInstances carried and how many
 * actors carry the group: {groups: {group: {hash, actors}}}. Lila skips groups whose signature and count still match.
 */
TSharedPtr<FJsonValue> PlacementHashes(const TSharedPtr<FJsonObject>& Params, FString& OutError)
{
	TRACE_CPUPROFILER_EVENT_SCOPE(LilaLinkPlacements::PlacementHashes);

	UWorld* World = EditorWorld();
	if (!World)
	{
		OutError = TEXT("placementHashes: no level is open");
		return nullptr;
	}
	TMap<FString, TPair<FString, int32>> ByGroup;
	for (TActorIterator<AActor> It(World); It; ++It)
	{
		const FString Group = TagValue(*It, GroupPrefix);
		if (Group.IsEmpty())
		{
			continue;
		}
		TPair<FString, int32>& Entry = ByGroup.FindOrAdd(Group);
		FString Hash = TagValue(*It, HashPrefix);
		// A mesh that no longer loads (an import never saved) needs the group sent again.
		if (const UStaticMeshComponent* Mesh = It->FindComponentByClass<UStaticMeshComponent>(); Mesh && !Mesh->GetStaticMesh())
		{
			Hash.Reset();
		}
		// One actor without the signature (placed by an older send) makes the group's signature unknown.
		Entry.Key = Entry.Value == 0 || Entry.Key == Hash ? Hash : FString();
		++Entry.Value;
	}
	TSharedRef<FJsonObject> Groups = MakeShared<FJsonObject>();
	for (const TPair<FString, TPair<FString, int32>>& G : ByGroup)
	{
		TSharedRef<FJsonObject> E = MakeShared<FJsonObject>();
		E->SetStringField(TEXT("hash"), G.Value.Key);
		E->SetNumberField(TEXT("actors"), G.Value.Value);
		Groups->SetObjectField(G.Key, E);
	}
	TSharedRef<FJsonObject> Out = MakeShared<FJsonObject>();
	Out->SetObjectField(TEXT("groups"), Groups);
	return Obj(Out);
}

/**
 * The layer a Custom HLOD Actor layer links to: Path as it is, or the level's default HLOD layer for "default". Only a
 * layer the level's runtime partition generates HLODs for gives the custom actors a cell to show for; a Custom HLOD Actor
 * layer never does.
 */
UHLODLayer* LinkedHlodLayer(UWorld* World, const FString& Path, TArray<FString>& OutWarnings)
{
	if (Path == TEXT("default"))
	{
		UWorldPartition* Partition = World->GetWorldPartition();
		UHLODLayer* Default = Partition ? Partition->GetDefaultHLODLayer() : nullptr;
		if (!Default)
		{
			OutWarnings.Add(TEXT("the level has no default HLOD layer (World Settings, World Partition) to link to: these show whenever their source cell is unloaded"));
		}
		return Default;
	}
	const FString Object = Path.Contains(TEXT(".")) ? Path : Path + TEXT(".") + FPackageName::GetShortName(Path);
	UHLODLayer* Layer = LoadObject<UHLODLayer>(nullptr, *Object, nullptr, LOAD_NoWarn | LOAD_Quiet);
	if (!Layer)
	{
		OutWarnings.Add(TEXT("no HLOD layer ") + Path + TEXT(" to link to"));
	}
	else if (Layer->GetLayerType() == EHLODLayerType::CustomHLODActor)
	{
		OutWarnings.Add(Path + TEXT(" is a Custom HLOD Actor layer, which World Partition gives no cells: link to a layer it builds HLODs for"));
	}
	return Layer;
}

/**
 * The Custom HLOD Actor layer at Path, made when missing (and turned into one when it is another type). Linked names the
 * layer whose unloading shows this one's actors (LinkedHlodLayer: "default" for the level's own, so far proxies show once
 * its HLODs unload); empty leaves it unset, and they show once their source cell unloads.
 */
UHLODLayer* CustomHlodLayer(UWorld* World, const FString& Path, const FString& Linked, TArray<FString>& OutWarnings)
{
	TRACE_CPUPROFILER_EVENT_SCOPE(LilaLinkPlacements::CustomHlodLayer);

	const FString Package = Path.Contains(TEXT(".")) ? FPackageName::ObjectPathToPackageName(Path) : Path;
	if (!FPackageName::IsValidLongPackageName(Package))
	{
		OutWarnings.Add(FString::Printf(TEXT("%s is not a content path like /Game/Lila/HLOD/HL_Buildings"), *Path));
		return nullptr;
	}
	const FString Name = FPackageName::GetShortName(Package);
	UHLODLayer* Layer = LoadObject<UHLODLayer>(nullptr, *(Package + TEXT(".") + Name), nullptr, LOAD_NoWarn | LOAD_Quiet);
	bool bDirty = false;
	if (!Layer)
	{
		Layer = NewObject<UHLODLayer>(CreatePackage(*Package), *Name, RF_Public | RF_Standalone | RF_Transactional);
		FAssetRegistryModule::AssetCreated(Layer);
		bDirty = true;
	}
	if (Layer->GetLayerType() != EHLODLayerType::CustomHLODActor)
	{
		Layer->Modify();
		Layer->SetLayerType(EHLODLayerType::CustomHLODActor);
		bDirty = true;
	}
	if (!Linked.IsEmpty())
	{
		UHLODLayer* LinkedLayer = LinkedHlodLayer(World, Linked, OutWarnings);
		// LinkedLayer has no setter; it is an editor-only property of the layer.
		if (FObjectProperty* Prop = FindFProperty<FObjectProperty>(UHLODLayer::StaticClass(), TEXT("LinkedLayer")); Prop && LinkedLayer)
		{
			if (Prop->GetObjectPropertyValue_InContainer(Layer) != LinkedLayer)
			{
				Layer->Modify();
				Prop->SetObjectPropertyValue_InContainer(Layer, LinkedLayer);
				bDirty = true;
			}
		}
	}
	if (bDirty)
	{
		Layer->MarkPackageDirty();
		UEditorLoadingAndSavingUtils::SavePackages({Layer->GetPackage()}, true);
	}
	return Layer;
}

/**
 * A Custom HLOD actor per placement showing Mesh while its World Partition cell is unloaded (a building block's HLOD
 * proxy), in a Custom HLOD Actor layer; matched to the existing ones by node id.
 */
void PlaceCustomHlods(UWorld* World, const FString& Group, const FString& Label, UStaticMesh* Mesh, UHLODLayer* Layer,
	const TArray<TSharedPtr<FJsonValue>>& Placements, TArray<AActor*> Existing, int32& Spawned, int32& Updated, int32& Removed)
{
	TRACE_CPUPROFILER_EVENT_SCOPE(LilaLinkPlacements::PlaceCustomHlods);

	TMap<FString, AWorldPartitionCustomHLOD*> ByNode;
	for (AActor* A : Existing)
	{
		if (AWorldPartitionCustomHLOD* H = Cast<AWorldPartitionCustomHLOD>(A))
		{
			ByNode.Add(TagValue(H, NodePrefix), H);
		}
		else
		{
			World->EditorDestroyActor(A, true);
			++Removed;
		}
	}
	const FName Folder(*(TEXT("Lila/HLOD/") + ShortGroupName(Label)));
	TSet<FString> Sent;
	for (const TSharedPtr<FJsonValue>& V : Placements)
	{
		const TSharedPtr<FJsonObject>* P = nullptr;
		if (!V.IsValid() || !V->TryGetObject(P))
		{
			continue;
		}
		const FString Id = IdOf(*P);
		Sent.Add(Id);
		const FTransform Transform = TransformOf(*P);
		AWorldPartitionCustomHLOD*& Actor = ByNode.FindOrAdd(Id);
		if (!Actor)
		{
			FActorSpawnParameters Params;
			Params.ObjectFlags |= RF_Transactional;
			Actor = World->SpawnActor<AWorldPartitionCustomHLOD>(AWorldPartitionCustomHLOD::StaticClass(), Transform, Params);
			if (!Actor)
			{
				continue;
			}
			Actor->Tags.Add(GroupTag(Group));
			Actor->Tags.Add(NodeTag(Id));
			++Spawned;
		}
		else
		{
			Actor->Modify();
			Actor->SetActorTransform(Transform);
			++Updated;
		}
		Actor->SetFolderPath(Folder);
		FString Name;
		if ((*P)->TryGetStringField(TEXT("name"), Name))
		{
			Actor->SetActorLabel(Name);
		}
		if (UStaticMeshComponent* Component = Actor->FindComponentByClass<UStaticMeshComponent>())
		{
			Component->Modify();
			Component->SetStaticMesh(Mesh);
			Component->SetCollisionEnabled(ECollisionEnabled::NoCollision);
		}
		Actor->SetHLODLayer(Layer);
	}
	for (const TPair<FString, AWorldPartitionCustomHLOD*>& Pair : ByNode)
	{
		if (Pair.Value && !Sent.Contains(Pair.Key))
		{
			World->EditorDestroyActor(Pair.Value, true);
			++Removed;
		}
	}
}

TSharedPtr<FJsonValue> PlaceSlotClass(const TSharedPtr<FJsonObject>& Params, const FString& Group, const FString& ClassPath,
	const TArray<TSharedPtr<FJsonValue>>& Placements, FString& OutError)
{
	UClass* Class = StaticLoadClass(AActor::StaticClass(), nullptr, *ClassPath, nullptr, LOAD_NoWarn | LOAD_Quiet);
	if (!Class && !ClassPath.EndsWith(TEXT("_C")))
	{
		Class = StaticLoadClass(AActor::StaticClass(), nullptr, *(ClassPath + TEXT("_C")), nullptr, LOAD_NoWarn | LOAD_Quiet);
	}
	if (!Class)
	{
		OutError = FString::Printf(TEXT("placeInstances: no actor class at %s"), *ClassPath);
		return nullptr;
	}
	UWorld* World = EditorWorld();
	if (!World)
	{
		OutError = TEXT("placeInstances: no level is open");
		return nullptr;
	}
	FString Label = Group;
	Params->TryGetStringField(TEXT("label"), Label);
	FScopedTransaction Transaction(LOCTEXT("PlaceSlotActors", "Lila: Place Slot Actors"));
	int32 Spawned = 0, Updated = 0, Removed = 0;
	FComponentSink Sink;
	PlaceClassActors(World, Group, Label, Class, Placements, TaggedActors(World, GroupTag(Group)), Spawned, Updated, Removed, Sink);
	StampHash(World, Group, Params);
	for (const FString& W : Sink.Warnings)
	{
		UE_LOG(LogLilaLink, Warning, TEXT("placeInstances %s: %s"), *Label, *W);
	}
	if (ULevel* Level = World->GetCurrentLevel())
	{
		Level->MarkPackageDirty();
	}
	UE_LOG(LogLilaLink, Log, TEXT("placeInstances %s: %d spawned, %d updated, %d removed (%s)"), *Label, Spawned, Updated, Removed, *Class->GetName());
	TSharedRef<FJsonObject> Out = MakeShared<FJsonObject>();
	Out->SetNumberField(TEXT("spawned"), Spawned);
	Out->SetNumberField(TEXT("updated"), Updated);
	Out->SetNumberField(TEXT("removed"), Removed);
	Out->SetStringField(TEXT("mode"), TEXT("actors"));
	Out->SetStringField(TEXT("actorClass"), Class->GetPathName());
	TArray<TSharedPtr<FJsonValue>> Warnings;
	for (const FString& W : Sink.Warnings)
	{
		Warnings.Add(MakeShared<FJsonValueString>(W));
	}
	Out->SetArrayField(TEXT("warnings"), Warnings);
	return MakeShared<FJsonValueObject>(Out);
}

/**
 * writeSlotTable {path, rows: [{id, type, owner, group, actorClass, mesh, item, data, location, rotation, scale}], sources?}
 * writes the rows into the ULilaSlotTable data asset at path (made when missing), replacing the rows whose group starts
 * with one of sources (every row when sources is missing). Returns {path, rows}.
 */
TSharedPtr<FJsonValue> WriteSlotTable(const TSharedPtr<FJsonObject>& Params, FString& OutError)
{
	TRACE_CPUPROFILER_EVENT_SCOPE(LilaLinkPlacements::WriteSlotTable);

	FString Path;
	const TArray<TSharedPtr<FJsonValue>>* Rows = nullptr;
	if (!Params->TryGetStringField(TEXT("path"), Path) || !Params->TryGetArrayField(TEXT("rows"), Rows))
	{
		OutError = TEXT("writeSlotTable: give path and rows");
		return nullptr;
	}
	if (!FPackageName::IsValidLongPackageName(Path))
	{
		OutError = FString::Printf(TEXT("writeSlotTable: %s is not a content path like /Game/Lila/Slots/DA_LilaSlotTable"), *Path);
		return nullptr;
	}
	const FString ObjectPath = Path + TEXT(".") + FPackageName::GetShortName(Path);
	// Found in memory first: a load mid-send flushes async loading, which can catch World Partition registering actors.
	ULilaSlotTable* Table = FindObject<ULilaSlotTable>(nullptr, *ObjectPath);
	if (!Table && FPackageName::DoesPackageExist(Path))
	{
		Table = LoadObject<ULilaSlotTable>(nullptr, *ObjectPath, nullptr, LOAD_NoWarn | LOAD_Quiet);
	}
	if (!Table)
	{
		UPackage* Package = CreatePackage(*Path);
		Table = NewObject<ULilaSlotTable>(Package, *FPackageName::GetShortName(Path), RF_Public | RF_Standalone | RF_Transactional);
		FAssetRegistryModule::AssetCreated(Table);
	}
	Table->Modify();
	// sources: the group prefixes this send covers; their rows are replaced and every other row stays. Without it the
	// table is rewritten whole.
	const TArray<TSharedPtr<FJsonValue>>* Sources = nullptr;
	if (Params->TryGetArrayField(TEXT("sources"), Sources))
	{
		TArray<FString> Prefixes;
		for (const TSharedPtr<FJsonValue>& S : *Sources)
		{
			FString Prefix;
			if (S.IsValid() && S->TryGetString(Prefix) && !Prefix.IsEmpty())
			{
				Prefixes.Add(Prefix);
			}
		}
		Table->Rows.RemoveAll([&Prefixes](const FLilaSlotRow& Row) {
			return Prefixes.ContainsByPredicate([&Row](const FString& P) { return Row.Group.StartsWith(P, ESearchCase::CaseSensitive); });
		});
	}
	else
	{
		Table->Rows.Reset(Rows->Num());
	}
	for (const TSharedPtr<FJsonValue>& V : *Rows)
	{
		const TSharedPtr<FJsonObject>* R = nullptr;
		if (!V.IsValid() || !V->TryGetObject(R))
		{
			continue;
		}
		FLilaSlotRow& Row = Table->Rows.AddDefaulted_GetRef();
		(*R)->TryGetStringField(TEXT("id"), Row.Id);
		(*R)->TryGetStringField(TEXT("type"), Row.Type);
		(*R)->TryGetStringField(TEXT("owner"), Row.Owner);
		(*R)->TryGetStringField(TEXT("group"), Row.Group);
		(*R)->TryGetStringField(TEXT("actorClass"), Row.ActorClass);
		(*R)->TryGetStringField(TEXT("mesh"), Row.Mesh);
		(*R)->TryGetStringField(TEXT("item"), Row.Item);
		const TSharedPtr<FJsonObject>* Data = nullptr;
		if ((*R)->TryGetObjectField(TEXT("data"), Data))
		{
			for (const TPair<FString, TSharedPtr<FJsonValue>>& Value : (*Data)->Values)
			{
				Row.Data.Add(Value.Key, LilaLink::ComponentValueText(Value.Value));
			}
		}
		Row.Transform = TransformOf(*R);
	}
	Table->RebuildIndex();
	Table->MarkPackageDirty();
	UEditorLoadingAndSavingUtils::SavePackages({Table->GetPackage()}, true);
	UE_LOG(LogLilaLink, Log, TEXT("writeSlotTable %s: %d rows"), *Path, Table->Rows.Num());
	TSharedRef<FJsonObject> Out = MakeShared<FJsonObject>();
	Out->SetStringField(TEXT("path"), ObjectPath);
	Out->SetNumberField(TEXT("rows"), Table->Rows.Num());
	return MakeShared<FJsonValueObject>(Out);
}

TSharedPtr<FJsonValue> PlaceInstances(const TSharedPtr<FJsonObject>& Params, FString& OutError)
{
	TRACE_CPUPROFILER_EVENT_SCOPE(LilaLinkPlacements::PlaceInstances);

	FString Group, MeshPath, Mode = TEXT("actors"), ActorClass;
	const TArray<TSharedPtr<FJsonValue>>* Placements = nullptr;
	Params->TryGetStringField(TEXT("actorClass"), ActorClass);
	Params->TryGetStringField(TEXT("mesh"), MeshPath);
	if (!Params->TryGetStringField(TEXT("group"), Group) || (MeshPath.IsEmpty() && ActorClass.IsEmpty()) ||
		!Params->TryGetArrayField(TEXT("placements"), Placements))
	{
		OutError = TEXT("placeInstances: give group, mesh (or actorClass) and placements");
		return nullptr;
	}
	if (!ActorClass.IsEmpty())
	{
		return PlaceSlotClass(Params, Group, ActorClass, *Placements, OutError);
	}
	Params->TryGetStringField(TEXT("mode"), Mode);
	FString Collision;
	Params->TryGetStringField(TEXT("collision"), Collision);
	const bool bCollide = Collision != TEXT("none");
	FString Label = Group;
	Params->TryGetStringField(TEXT("label"), Label);
	if (Mode != TEXT("actors") && Mode != TEXT("ism") && Mode != TEXT("hism") && Mode != TEXT("customHlod"))
	{
		OutError = FString::Printf(TEXT("placeInstances: mode is actors, ism, hism or customHlod, not '%s'"), *Mode);
		return nullptr;
	}
	const FString ObjectPath = MeshPath.Contains(TEXT(".")) ? MeshPath : MeshPath + TEXT(".") + FPackageName::GetShortName(MeshPath);
	UStaticMesh* Mesh = LoadObject<UStaticMesh>(nullptr, *ObjectPath);
	if (!Mesh)
	{
		OutError = FString::Printf(TEXT("placeInstances: no static mesh at %s"), *ObjectPath);
		return nullptr;
	}
	UWorld* World = EditorWorld();
	if (!World)
	{
		OutError = TEXT("placeInstances: no level is open");
		return nullptr;
	}

	FScopedTransaction Transaction(LOCTEXT("PlaceInstances", "Lila: Place Instances"));
	int32 Spawned = 0, Updated = 0, Removed = 0;
	FComponentSink Sink;
	Sink.Folder = FPackageName::GetLongPackagePath(Mesh->GetPackage()->GetName());
	TArray<AActor*> Existing = TaggedActors(World, GroupTag(Group));
	// A Lila send names the collision, which its import already gave the mesh; other senders leave it to the body.
	if (Collision.IsEmpty())
	{
		ApplyMeshCollision(Mesh, *Placements);
	}
	FString HlodPath;
	Params->TryGetStringField(TEXT("hlodLayer"), HlodPath);
	if (Mode == TEXT("customHlod"))
	{
		if (!World->IsPartitionedWorld())
		{
			OutError = TEXT("placeInstances: custom HLOD actors need a World Partition level; this level is not one");
			return nullptr;
		}
		FString Linked;
		Params->TryGetStringField(TEXT("linkedHlodLayer"), Linked);
		UHLODLayer* Layer = CustomHlodLayer(World, HlodPath.IsEmpty() ? TEXT("/Game/Lila/HLOD/HL_LilaCustom") : HlodPath, Linked, Sink.Warnings);
		PlaceCustomHlods(World, Group, Label, Mesh, Layer, *Placements, Existing, Spawned, Updated, Removed);
		HlodPath.Reset();
	}
	else if (Mode == TEXT("actors"))
	{
		PlaceActors(World, Group, Label, Mesh, bCollide, *Placements, Existing, Spawned, Updated, Removed, Sink);
	}
	else
	{
		PlaceInstanced(World, Params, Group, Label, Mesh, Mode == TEXT("hism"), bCollide, *Placements, Existing, Spawned, Updated, Removed, Sink);
	}
	StampHash(World, Group, Params);
	if (HlodPath == TEXT("none"))
	{
		// Left out of HLOD generation: something else (Custom HLOD actors) stands in for them.
		for (AActor* Actor : TaggedActors(World, GroupTag(Group)))
		{
			if (Actor->bEnableAutoLODGeneration)
			{
				Actor->Modify();
				Actor->bEnableAutoLODGeneration = false;
			}
		}
	}
	else if (!HlodPath.IsEmpty())
	{
		const FString HlodObject = HlodPath.Contains(TEXT(".")) ? HlodPath : HlodPath + TEXT(".") + FPackageName::GetShortName(HlodPath);
		if (UHLODLayer* Layer = LoadObject<UHLODLayer>(nullptr, *HlodObject, nullptr, LOAD_NoWarn | LOAD_Quiet))
		{
			for (AActor* Actor : TaggedActors(World, GroupTag(Group)))
			{
				Actor->Modify();
				Actor->SetHLODLayer(Layer);
			}
		}
		else
		{
			Sink.Warnings.Add(TEXT("no HLOD layer ") + HlodPath + TEXT("; the actors keep the default"));
		}
	}
	for (const FString& W : Sink.Warnings)
	{
		UE_LOG(LogLilaLink, Warning, TEXT("placeInstances %s: %s"), *Label, *W);
	}
	if (ULevel* Level = World->GetCurrentLevel())
	{
		Level->MarkPackageDirty();
	}
	UE_LOG(LogLilaLink, Log, TEXT("placeInstances %s: %d spawned, %d updated, %d removed (%s)"), *Label, Spawned, Updated, Removed, *Mode);

	TSharedRef<FJsonObject> Out = MakeShared<FJsonObject>();
	Out->SetNumberField(TEXT("spawned"), Spawned);
	Out->SetNumberField(TEXT("updated"), Updated);
	Out->SetNumberField(TEXT("removed"), Removed);
	Out->SetStringField(TEXT("mode"), Mode);
	TArray<TSharedPtr<FJsonValue>> Warnings, Assets;
	for (const FString& W : Sink.Warnings)
	{
		Warnings.Add(MakeShared<FJsonValueString>(W));
	}
	for (const FString& A : Sink.Assets)
	{
		Assets.Add(MakeShared<FJsonValueString>(A));
	}
	Out->SetArrayField(TEXT("warnings"), Warnings);
	Out->SetArrayField(TEXT("assets"), Assets);
	return Obj(Out);
}

double NumberField(const TSharedPtr<FJsonObject>& P, const TCHAR* Field, double Fallback)
{
	double V = Fallback;
	P->TryGetNumberField(Field, V);
	return V;
}

bool BoolField(const TSharedPtr<FJsonObject>& P, const TCHAR* Field, bool Fallback)
{
	bool V = Fallback;
	P->TryGetBoolField(Field, V);
	return V;
}

UClass* LightClassFor(const FString& Kind)
{
	if (Kind == TEXT("spot"))
	{
		return ASpotLight::StaticClass();
	}
	if (Kind == TEXT("directional"))
	{
		return ADirectionalLight::StaticClass();
	}
	if (Kind == TEXT("area") || Kind == TEXT("disc"))
	{
		return ARectLight::StaticClass();
	}
	return APointLight::StaticClass();
}

/** Lila's light settings on the component: colour, temperature, intensity in its units, falloff, cone, size, shadows. */
void ApplyLightSettings(ULightComponent* Light, const TSharedPtr<FJsonObject>& P, const FString& Label, TArray<FString>& Warnings)
{
	TRACE_CPUPROFILER_EVENT_SCOPE(LilaLinkPlacements::ApplyLightSettings);

	FString Kind, Units = TEXT("unitless");
	P->TryGetStringField(TEXT("kind"), Kind);
	P->TryGetStringField(TEXT("units"), Units);
	const FVector Color = VectorField(P, TEXT("color"), FVector::OneVector);
	Light->SetLightColor(FLinearColor(Color.X, Color.Y, Color.Z), false);
	Light->SetUseTemperature(BoolField(P, TEXT("useTemperature"), false));
	Light->SetTemperature(NumberField(P, TEXT("temperature"), 6500.0));
	Light->SetCastShadows(BoolField(P, TEXT("castShadows"), true));
	double Intensity = NumberField(P, TEXT("intensity"), 1.0);
	if (ULocalLightComponent* Local = Cast<ULocalLightComponent>(Light))
	{
		ELightUnits LightUnits = ELightUnits::Unitless;
		if (Units == TEXT("candela"))
		{
			LightUnits = ELightUnits::Candelas;
		}
		else if (Units == TEXT("lumens"))
		{
			LightUnits = ELightUnits::Lumens;
		}
		else if (Units == TEXT("nits"))
		{
			LightUnits = ELightUnits::Nits;
		}
		else if (Units == TEXT("watts"))
		{
			// 683 lm/W, the peak luminous efficacy other apps convert radiant watts with.
			LightUnits = ELightUnits::Lumens;
			Intensity *= 683.0;
		}
		else if (Units == TEXT("lux"))
		{
			Warnings.Add(FString::Printf(TEXT("%s: lux has no meaning on a local light; sent unitless"), *Label));
		}
		Local->SetIntensityUnits(LightUnits);
		if (const double Range = NumberField(P, TEXT("range"), 0.0); Range > 0.0)
		{
			Local->SetAttenuationRadius(Range);
		}
	}
	Light->SetIntensity(Intensity);
	if (UPointLightComponent* Point = Cast<UPointLightComponent>(Light))
	{
		Point->SetSourceRadius(NumberField(P, TEXT("sourceRadius"), 0.0));
	}
	if (USpotLightComponent* Spot = Cast<USpotLightComponent>(Light))
	{
		Spot->SetInnerConeAngle(NumberField(P, TEXT("innerConeAngle"), 0.0));
		Spot->SetOuterConeAngle(NumberField(P, TEXT("outerConeAngle"), 45.0));
	}
	if (URectLightComponent* Rect = Cast<URectLightComponent>(Light))
	{
		const double Width = NumberField(P, TEXT("width"), 100.0);
		Rect->SetSourceWidth(Width);
		Rect->SetSourceHeight(Kind == TEXT("disc") ? Width : NumberField(P, TEXT("height"), 100.0));
		if (Kind == TEXT("disc"))
		{
			Warnings.Add(FString::Printf(TEXT("%s: Unreal has no disc light; sent as a square rect light"), *Label));
		}
	}
	if (UDirectionalLightComponent* Directional = Cast<UDirectionalLightComponent>(Light))
	{
		Directional->SetLightSourceAngle(NumberField(P, TEXT("sourceRadius"), 0.0));
		bool bSun = false;
		P->TryGetBoolField(TEXT("atmosphereSun"), bSun);
		Directional->SetAtmosphereSunLight(bSun);
	}
	if (const TOptional<FLightingChannels> Channels = ChannelsOf(P))
	{
		Light->SetLightingChannels(Channels->bChannel0, Channels->bChannel1, Channels->bChannel2);
	}
}

TSharedPtr<FJsonValue> PlaceLights(const TSharedPtr<FJsonObject>& Params, FString& OutError)
{
	TRACE_CPUPROFILER_EVENT_SCOPE(LilaLinkPlacements::PlaceLights);

	const TArray<TSharedPtr<FJsonValue>>* Lights = nullptr;
	if (!Params->TryGetArrayField(TEXT("lights"), Lights))
	{
		OutError = TEXT("placeLights: give lights");
		return nullptr;
	}
	UWorld* World = EditorWorld();
	if (!World)
	{
		OutError = TEXT("placeLights: no level is open");
		return nullptr;
	}

	FScopedTransaction Transaction(LOCTEXT("PlaceLights", "Lila: Place Lights"));
	TMap<FString, ALight*> ByNode;
	for (AActor* A : TaggedActors(World, LightTag))
	{
		if (ALight* L = Cast<ALight>(A))
		{
			ByNode.Add(TagValue(L, NodePrefix), L);
		}
	}
	int32 Spawned = 0, Updated = 0;
	TArray<FString> Warnings;
	for (const TSharedPtr<FJsonValue>& V : *Lights)
	{
		const TSharedPtr<FJsonObject>* P = nullptr;
		if (!V.IsValid() || !V->TryGetObject(P))
		{
			continue;
		}
		const FString Id = IdOf(*P);
		FString Kind, Name = Id;
		(*P)->TryGetStringField(TEXT("kind"), Kind);
		(*P)->TryGetStringField(TEXT("name"), Name);
		UClass* Class = LightClassFor(Kind);
		// The actor's X axis is where the light shines; Z is Lila's up, along a rect light's height.
		const FVector Forward = VectorField(*P, TEXT("forward"), FVector::ForwardVector);
		const FVector Up = VectorField(*P, TEXT("up"), FVector::UpVector);
		const FTransform Transform(FRotationMatrix::MakeFromXZ(Forward, Up).ToQuat(), VectorField(*P, TEXT("location"), FVector::ZeroVector));
		ALight*& Actor = ByNode.FindOrAdd(Id);
		if (Actor && !Actor->IsA(Class))
		{
			World->EditorDestroyActor(Actor, true);
			Actor = nullptr;
		}
		if (!Actor)
		{
			FActorSpawnParameters SpawnParams;
			SpawnParams.ObjectFlags |= RF_Transactional;
			Actor = Cast<ALight>(World->SpawnActor(Class, &Transform, SpawnParams));
			if (!Actor)
			{
				continue;
			}
			Actor->Tags.Add(LightTag);
			Actor->Tags.Add(NodeTag(Id));
			++Spawned;
		}
		else
		{
			Actor->Modify();
			++Updated;
		}
		// Spawning composes the rotation with the class default's (spot and directional lights point down), so it is set again.
		Actor->SetActorLocationAndRotation(Transform.GetLocation(), Transform.GetRotation());
		Actor->SetFolderPath(FName(TEXT("Lila/Lights")));
		Actor->SetActorLabel(Name);
		if (ULightComponent* Light = Actor->GetLightComponent())
		{
			Light->Modify();
			ApplyLightSettings(Light, *P, Name, Warnings);
		}
	}
	for (const FString& W : Warnings)
	{
		UE_LOG(LogLilaLink, Warning, TEXT("placeLights: %s"), *W);
	}
	if (ULevel* Level = World->GetCurrentLevel())
	{
		Level->MarkPackageDirty();
	}
	UE_LOG(LogLilaLink, Log, TEXT("placeLights: %d spawned, %d updated"), Spawned, Updated);

	TSharedRef<FJsonObject> Out = MakeShared<FJsonObject>();
	Out->SetNumberField(TEXT("spawned"), Spawned);
	Out->SetNumberField(TEXT("updated"), Updated);
	TArray<TSharedPtr<FJsonValue>> WarningValues;
	for (const FString& W : Warnings)
	{
		WarningValues.Add(MakeShared<FJsonValueString>(W));
	}
	Out->SetArrayField(TEXT("warnings"), WarningValues);
	return Obj(Out);
}

TSharedPtr<FJsonValue> RemovePlacements(const TSharedPtr<FJsonObject>& Params, FString& OutError)
{
	TRACE_CPUPROFILER_EVENT_SCOPE(LilaLinkPlacements::RemovePlacements);

	UWorld* World = EditorWorld();
	if (!World)
	{
		OutError = TEXT("removePlacements: no level is open");
		return nullptr;
	}
	TSet<FName> Tags;
	const TArray<TSharedPtr<FJsonValue>>* Values = nullptr;
	if (Params->TryGetArrayField(TEXT("groups"), Values))
	{
		for (const TSharedPtr<FJsonValue>& V : *Values)
		{
			Tags.Add(GroupTag(V->AsString()));
		}
	}
	if (Params->TryGetArrayField(TEXT("ids"), Values))
	{
		for (const TSharedPtr<FJsonValue>& V : *Values)
		{
			Tags.Add(NodeTag(V->Type == EJson::Number ? FString::Printf(TEXT("%lld"), static_cast<long long>(V->AsNumber())) : V->AsString()));
		}
	}
	// Groups whose key starts with a prefix go too, unless kept: a source's geometry nodes meshes it no longer instances.
	TArray<FString> Prefixes;
	TSet<FString> Keep;
	if (Params->TryGetArrayField(TEXT("prefixes"), Values))
	{
		for (const TSharedPtr<FJsonValue>& V : *Values)
		{
			Prefixes.Add(V->AsString());
		}
	}
	if (Params->TryGetArrayField(TEXT("keep"), Values))
	{
		for (const TSharedPtr<FJsonValue>& V : *Values)
		{
			Keep.Add(V->AsString());
		}
	}
	if (Tags.IsEmpty() && Prefixes.IsEmpty())
	{
		OutError = TEXT("removePlacements: give groups, ids or prefixes");
		return nullptr;
	}
	FScopedTransaction Transaction(LOCTEXT("RemovePlacements", "Lila: Remove Placements"));
	TArray<AActor*> Doomed;
	for (TActorIterator<AActor> It(World); It; ++It)
	{
		if (InsideLevelInstance(World, *It))
		{
			continue;
		}
		const FString Group = Prefixes.IsEmpty() ? FString() : TagValue(*It, GroupPrefix);
		if (!Group.IsEmpty() && !Keep.Contains(Group) && Prefixes.ContainsByPredicate([&Group](const FString& P) { return Group.StartsWith(P); }))
		{
			Doomed.Add(*It);
			continue;
		}
		for (const FName& Tag : It->Tags)
		{
			if (Tags.Contains(Tag))
			{
				Doomed.Add(*It);
				break;
			}
		}
	}
	// levelInstances: a Lila Level Instance whose every group is being removed goes too (its contents are never edited in place).
	bool bLevelInstances = false;
	Params->TryGetBoolField(TEXT("levelInstances"), bLevelInstances);
	int32 LevelInstancesRemoved = 0;
	TArray<const UDataLayerAsset*> InteriorLayers;
	TArray<FString> InteriorLevels;
	TArray<const ULevel*> InteriorContents;
	if (bLevelInstances && !Tags.IsEmpty())
	{
		if (ULevelInstanceSubsystem* Instances = World->GetSubsystem<ULevelInstanceSubsystem>())
		{
			for (TActorIterator<ALevelInstance> It(World); It; ++It)
			{
				const ULevel* Inner = TagValue(*It, AssemblyPrefix).IsEmpty() ? nullptr : Instances->GetLevelInstanceLevel(*It);
				if (!Inner)
				{
					continue;
				}
				bool bAny = false, bAll = true;
				for (const AActor* Actor : Inner->Actors)
				{
					const FString Group = Actor ? TagValue(Actor, GroupPrefix) : FString();
					if (Group.IsEmpty())
					{
						continue;
					}
					bAny = true;
					if (!Tags.Contains(GroupTag(Group)))
					{
						bAll = false;
						break;
					}
				}
				if (bAny && bAll)
				{
					Doomed.Add(*It);
					++LevelInstancesRemoved;
					InteriorLevels.AddUnique(It->GetWorldAsset().GetLongPackageName());
					InteriorContents.Add(Inner);
					for (const UDataLayerAsset* Asset : It->GetDataLayerAssets())
					{
						if (Asset && Asset->GetName().StartsWith(TEXT("DL_Interior_")))
						{
							InteriorLayers.AddUnique(Asset);
						}
					}
				}
			}
		}
	}
	// deleteMeshes: the static meshes the removed actors showed go too, when they lie under meshFolder and nothing left in the level shows them.
	bool bDeleteMeshes = false;
	Params->TryGetBoolField(TEXT("deleteMeshes"), bDeleteMeshes);
	FString MeshFolder;
	Params->TryGetStringField(TEXT("meshFolder"), MeshFolder);
	if (bDeleteMeshes && MeshFolder.IsEmpty())
	{
		OutError = TEXT("removePlacements: deleteMeshes needs meshFolder, the folder the meshes may be deleted from");
		return nullptr;
	}
	TSet<UStaticMesh*> Meshes;
	if (bDeleteMeshes)
	{
		const FString Prefix = MeshFolder.EndsWith(TEXT("/")) ? MeshFolder : MeshFolder + TEXT("/");
		TArray<AActor*> Showing = Doomed;
		for (const ULevel* Inner : InteriorContents)
		{
			for (AActor* A : Inner->Actors)
			{
				if (A)
				{
					Showing.Add(A);
				}
			}
		}
		for (AActor* Actor : Showing)
		{
			TArray<UStaticMeshComponent*> Components;
			Actor->GetComponents(Components);
			for (UStaticMeshComponent* C : Components)
			{
				UStaticMesh* Mesh = C ? C->GetStaticMesh() : nullptr;
				if (Mesh && Mesh->GetPathName().StartsWith(Prefix))
				{
					Meshes.Add(Mesh);
				}
			}
		}
	}
	TSharedRef<FJsonObject> Out = MakeShared<FJsonObject>();
	Out->SetNumberField(TEXT("removed"), DestroyActors(World, Doomed));
	// The removed Level Instances unload now rather than on the next tick, so their level files are free to delete
	// below; the subsystem skips its update while a transaction records, and this one is only removals.
	if (ULevelInstanceSubsystem* Instances = InteriorLevels.IsEmpty() ? nullptr : World->GetSubsystem<ULevelInstanceSubsystem>())
	{
		TGuardValue<ITransaction*> NoUndo(GUndo, nullptr);
		Instances->OnUpdateStreamingState();
	}
	if (bLevelInstances)
	{
		// An interior's own data layer and its level go with its last actor.
		TArray<FString> Errors, Kept;
		Out->SetNumberField(TEXT("levelInstances"), LevelInstancesRemoved);
		Out->SetNumberField(TEXT("dataLayers"), LilaLink::DeleteUnusedDataLayers(World, InteriorLayers, Errors));
		Out->SetField(TEXT("levelsDeleted"), StringArray(DeleteUnusedAssemblyLevels(World, InteriorLevels, Kept, Errors)));
		if (!Kept.IsEmpty())
		{
			Out->SetField(TEXT("levelsKept"), StringArray(Kept));
		}
		if (!Errors.IsEmpty())
		{
			Out->SetField(TEXT("errors"), StringArray(Errors));
		}
	}
	if (bDeleteMeshes)
	{
		for (TActorIterator<AActor> It(World); It && !Meshes.IsEmpty(); ++It)
		{
			TArray<UStaticMeshComponent*> Components;
			It->GetComponents(Components);
			for (UStaticMeshComponent* C : Components)
			{
				if (C && C->GetStaticMesh())
				{
					Meshes.Remove(C->GetStaticMesh());
				}
			}
		}
		TArray<UObject*> Assets;
		TArray<TSharedPtr<FJsonValue>> Deleted;
		for (UStaticMesh* Mesh : Meshes)
		{
			// Another level's actors showing it (sends to several levels share meshes) keep it.
			if (!LilaLink::UsedOutside(World, Mesh->GetOutermost()->GetName()).IsEmpty())
			{
				continue;
			}
			Assets.Add(Mesh);
			Deleted.Add(MakeShared<FJsonValueString>(Mesh->GetOutermost()->GetName()));
		}
		Out->SetNumberField(TEXT("meshesDeleted"), Assets.IsEmpty() ? 0 : LilaLink::ForceDeleteAssets(Assets));
		Out->SetArrayField(TEXT("meshes"), Deleted);
	}
	return Obj(Out);
}

/** A package name not taken yet: the path itself, else with _2, _3, ... after it. */
FString FreePackageName(const FString& Path)
{
	FString Candidate = Path;
	for (int32 K = 2; FPackageName::DoesPackageExist(Candidate) || FindPackage(nullptr, *Candidate); ++K)
	{
		Candidate = FString::Printf(TEXT("%s_%d"), *Path, K);
	}
	return Candidate;
}

/** Where a Level Instance of these actors stands: the centre of their bounds, at their lowest point. */
FVector AssemblyPivot(const TArray<AActor*>& Actors)
{
	FBox Bounds(ForceInit);
	for (const AActor* Actor : Actors)
	{
		Bounds += Actor->GetComponentsBoundingBox(true);
	}
	if (!Bounds.IsValid)
	{
		return Actors[0]->GetActorLocation();
	}
	const FVector Center = Bounds.GetCenter();
	return FVector(Center.X, Center.Y, Bounds.Min.Z);
}

/** A new level saved as PackageName and streamed into World, made without the Save As dialog. */
ULevel* CreateAssemblyLevel(UWorld* World, const FString& PackageName)
{
	EditorLevelUtils::FCreateNewStreamingLevelForWorldParams Create(ULevelStreamingDynamic::StaticClass(),
		FPackageName::LongPackageNameToFilename(PackageName, FPackageName::GetMapPackageExtension()));
	Create.bUseSaveAs = false;
	Create.bCreateWorldPartition = World->IsPartitionedWorld();
	Create.bUseExternalActors = World->IsPartitionedWorld();
	Create.bEnableWorldPartitionStreaming = false;
	// Saved before the brush is placed in the world, so it keeps no world transform; a Level Instance needs no HLOD layers.
	Create.PreSaveLevelCallback = [](ULevel* New)
	{
		if (ABrush* Brush = New->IsUsingExternalActors() ? New->GetDefaultBrush() : nullptr)
		{
			Brush->GetRootComponent()->SetRelativeTransform(FTransform::Identity);
		}
		if (UWorldPartition* Partition = New->GetWorldPartition())
		{
			Partition->SetDefaultHLODLayer(nullptr);
		}
	};
	ULevelStreaming* Streaming = UEditorLevelUtils::CreateNewStreamingLevelForWorld(*World, Create);
	return Streaming ? Streaming->GetLoadedLevel() : nullptr;
}

/** The assembly's existing level streamed into World, emptied of what LilaLink put in it last time; their external packages go in OutStale. */
ULevel* ReopenAssemblyLevel(UWorld* World, const FString& PackageName, TArray<FString>& OutStale)
{
	ULevelStreaming* Streaming = UEditorLevelUtils::AddLevelToWorld(World, *PackageName, ULevelStreamingDynamic::StaticClass());
	ULevel* Level = Streaming ? Streaming->GetLoadedLevel() : nullptr;
	if (!Level)
	{
		return nullptr;
	}
	TArray<AActor*> Stale;
	for (AActor* Actor : Level->Actors)
	{
		if (Actor && !TagValue(Actor, GroupPrefix).IsEmpty())
		{
			Stale.Add(Actor);
		}
	}
	for (AActor* Actor : Stale)
	{
		if (UPackage* External = Actor->GetExternalPackage())
		{
			OutStale.Add(External->GetName());
		}
		World->EditorDestroyActor(Actor, false);
	}
	return Level;
}

/**
 * Moves the actors into the level saved as PackageName, placed relative to Pivot, and takes that level out of the world again.
 * An existing level of that name is reused, so a resend keeps its file. ULevelInstanceSubsystem::CreateLevelInstanceFrom
 * always asks for the file name in a modal dialog, so the level is made here.
 */
bool SaveActorsAsLevel(UWorld* World, const TArray<AActor*>& Actors, const FString& PackageName, const FVector& Pivot, FString& OutError)
{
	TRACE_CPUPROFILER_EVENT_SCOPE(LilaLinkPlacements::SaveActorsAsLevel);

	TArray<UPackage*> Packages;
	TArray<FString> Stale;
	ULevel* Level = FPackageName::DoesPackageExist(PackageName) ? ReopenAssemblyLevel(World, PackageName, Stale) : CreateAssemblyLevel(World, PackageName);
	if (!Level)
	{
		OutError = FString::Printf(TEXT("makeLevelInstance: Unreal could not make %s"), *PackageName);
		return false;
	}
	// Moving copies the actors, and the copies' labels can come back numbered; each keeps its Lila node tag.
	TMap<FString, FString> Labels;
	for (const AActor* Actor : Actors)
	{
		Labels.Add(TagValue(Actor, NodePrefix), Actor->GetActorLabel());
	}
	TArray<AActor*> Moved;
	UEditorLevelUtils::MoveActorsToLevel(Actors, Level, /*bWarnAboutReferences=*/false, /*bWarnAboutRenaming=*/false, /*bMoveAllOrFail=*/false, &Moved);
	Packages.Add(Level->GetPackage());
	for (AActor* Actor : Moved)
	{
		if (!Actor)
		{
			continue;
		}
		if (const FString* Label = Labels.Find(TagValue(Actor, NodePrefix)); Label && Actor->GetActorLabel() != *Label)
		{
			Actor->SetActorLabel(*Label);
		}
		if (!Actor->GetAttachParentActor())
		{
			Actor->SetFolderPath_Recursively(NAME_None);
			Actor->SetActorLocation(Actor->GetActorLocation() - Pivot);
		}
		if (UPackage* External = Actor->GetExternalPackage())
		{
			Packages.Add(External);
		}
	}
	const bool bSaved = UEditorLoadingAndSavingUtils::SavePackages(Packages, /*bOnlyDirty=*/false);
	// Saving skips the emptied packages of the actors removed from a reused level, so their files are deleted here.
	for (const FString& Name : Stale)
	{
		FString File;
		if (FPackageName::DoesPackageExist(Name, &File) && IFileManager::Get().Delete(*File, false, true))
		{
			IAssetRegistry::GetChecked().ScanModifiedAssetFiles({File});
		}
	}
	World->SetCurrentLevel(World->PersistentLevel);
	UEditorLevelUtils::RemoveLevelFromWorld(Level, /*bClearSelection=*/true, /*bResetTransBuffer=*/false);
	if (!bSaved)
	{
		OutError = FString::Printf(TEXT("makeLevelInstance: Unreal could not save %s"), *PackageName);
	}
	return bSaved;
}

TSharedPtr<FJsonValue> MakeLevelInstance(const TSharedPtr<FJsonObject>& Params, FString& OutError)
{
	TRACE_CPUPROFILER_EVENT_SCOPE(LilaLinkPlacements::MakeLevelInstance);

	FString Key, Name, Kind = TEXT("levelInstance"), Level;
	const TArray<TSharedPtr<FJsonValue>>* Groups = nullptr;
	if (!Params->TryGetStringField(TEXT("key"), Key) || !Params->TryGetStringField(TEXT("level"), Level) || !Params->TryGetArrayField(TEXT("groups"), Groups))
	{
		OutError = TEXT("makeLevelInstance: give key, level and groups");
		return nullptr;
	}
	Params->TryGetStringField(TEXT("name"), Name);
	Params->TryGetStringField(TEXT("kind"), Kind);
	UWorld* World = EditorWorld();
	if (!World)
	{
		OutError = TEXT("makeLevelInstance: no level is open");
		return nullptr;
	}
	// Only actors loose in the level: a resend that left every group unchanged placed none, and the instance stands as it is.
	TArray<AActor*> Actors;
	for (const TSharedPtr<FJsonValue>& G : *Groups)
	{
		Actors.Append(TaggedActors(World, GroupTag(G->AsString())));
	}
	const FName Tag(*(FString(AssemblyPrefix) + Key));
	TArray<AActor*> Previous = TaggedActors(World, Tag);
	if (Actors.IsEmpty() && !Previous.IsEmpty())
	{
		TSharedRef<FJsonObject> Same = MakeShared<FJsonObject>();
		Same->SetStringField(TEXT("actor"), Previous[0]->GetActorLabel());
		if (const ILevelInstanceInterface* Instance = Cast<ILevelInstanceInterface>(Previous[0]))
		{
			Same->SetStringField(TEXT("level"), Instance->GetWorldAsset().GetLongPackageName());
		}
		Same->SetStringField(TEXT("kind"), Kind);
		Same->SetBoolField(TEXT("unchanged"), true);
		return Obj(Same);
	}
	if (Actors.IsEmpty())
	{
		OutError = TEXT("makeLevelInstance: none of the groups' actors are in the level");
		return nullptr;
	}

	// A resend refills the level the assembly was sent as before, so its file and name stay.
	FString PackageName;
	for (AActor* Old : Previous)
	{
		if (const ILevelInstanceInterface* OldInstance = Cast<ILevelInstanceInterface>(Old); OldInstance && PackageName.IsEmpty())
		{
			PackageName = OldInstance->GetWorldAsset().GetLongPackageName();
		}
	}
	{
		FScopedTransaction Transaction(LOCTEXT("ReplaceLevelInstance", "Lila: Replace Level Instance"));
		for (AActor* Old : Previous)
		{
			World->EditorDestroyActor(Old, true);
		}
	}
	if (PackageName.IsEmpty())
	{
		// The level a removed instance left behind is reused rather than numbered past.
		bool bInUse = false;
		for (TActorIterator<ALevelInstance> It(World); It && !bInUse; ++It)
		{
			bInUse = It->GetWorldAsset().GetLongPackageName() == Level;
		}
		PackageName = bInUse ? FreePackageName(Level) : Level;
	}
	const FVector Pivot = AssemblyPivot(Actors);
	if (!SaveActorsAsLevel(World, Actors, PackageName, Pivot, OutError))
	{
		return nullptr;
	}
	const FString AssetName = FPackageName::GetShortName(PackageName);
	const TSoftObjectPtr<UWorld> WorldAsset(FSoftObjectPath(PackageName + TEXT(".") + AssetName));

	FScopedTransaction Transaction(LOCTEXT("MakeLevelInstance", "Lila: Make Level Instance"));
	FActorSpawnParameters Spawn;
	Spawn.OverrideLevel = World->PersistentLevel;
	AActor* Actor = nullptr;
	if (Kind == TEXT("packedLevelActor"))
	{
		const FString Blueprint = FString::Format(TEXT("{0}/{1}{2}.{1}{2}"), {FPackageName::GetLongPackagePath(PackageName), FPackedLevelActorBuilder::GetPackedBPPrefix(), AssetName});
		const TSoftObjectPtr<UBlueprint> BlueprintAsset{FSoftObjectPath(Blueprint)};
		UBlueprint* Packed = BlueprintAsset.LoadSynchronous();
		if (!Packed)
		{
			Packed = FPackedLevelActorBuilder::CreatePackedLevelActorBlueprint(BlueprintAsset, WorldAsset, true);
		}
		// A new Blueprint is empty and a reused one holds the last send: packing the level's meshes into it is a second step.
		if (Packed && !FPackedLevelActorBuilder::CreateDefaultBuilder()->CreateOrUpdateBlueprint(WorldAsset, BlueprintAsset, false, false))
		{
			Packed = nullptr;
		}
		if (Packed && Packed->GeneratedClass)
		{
			Actor = World->SpawnActor<APackedLevelActor>(Packed->GeneratedClass, FTransform(Pivot), Spawn);
		}
	}
	else
	{
		Actor = World->SpawnActor<ALevelInstance>(ALevelInstance::StaticClass(), FTransform(Pivot), Spawn);
	}
	ILevelInstanceInterface* Instance = Cast<ILevelInstanceInterface>(Actor);
	if (!Instance)
	{
		OutError = FString::Printf(TEXT("makeLevelInstance: Unreal could not place %s"), *PackageName);
		return nullptr;
	}
	Instance->SetWorldAsset(WorldAsset);
	// load=false: the caller tags the actor and puts it on its data layer first (buildingInteriors), then loads it; an
	// actor modified while its level's actors register trips World Partition.
	bool bLoad = true;
	Params->TryGetBoolField(TEXT("load"), bLoad);
	if (bLoad)
	{
		Instance->LoadLevelInstance();
	}
	Actor->Tags.Add(Tag);
	Actor->SetActorLabel(Name.IsEmpty() ? AssetName : Name);
	Actor->SetFolderPath(FName(TEXT("Lila/Assemblies")));
	UE_LOG(LogLilaLink, Log, TEXT("makeLevelInstance %s: %d actors into %s (%s)"), *Name, Actors.Num(), *PackageName, *Kind);

	TSharedRef<FJsonObject> Out = MakeShared<FJsonObject>();
	Out->SetStringField(TEXT("actor"), Actor->GetActorLabel());
	Out->SetStringField(TEXT("level"), PackageName);
	Out->SetStringField(TEXT("kind"), Kind);
	Out->SetNumberField(TEXT("actors"), Actors.Num());
	Out->SetNumberField(TEXT("replaced"), Previous.Num());
	return Obj(Out);
}

TSharedPtr<FJsonValue> ListPlacements(const TSharedPtr<FJsonObject>& Params, FString& OutError)
{
	TRACE_CPUPROFILER_EVENT_SCOPE(LilaLinkPlacements::ListPlacements);

	UWorld* World = EditorWorld();
	if (!World)
	{
		OutError = TEXT("listPlacements: no level is open");
		return nullptr;
	}
	TSet<FString> Only;
	const TArray<TSharedPtr<FJsonValue>>* Groups = nullptr;
	if (Params->TryGetArrayField(TEXT("groups"), Groups))
	{
		for (const TSharedPtr<FJsonValue>& V : *Groups)
		{
			Only.Add(V->AsString());
		}
	}
	TArray<TSharedPtr<FJsonValue>> Out;
	for (TActorIterator<AActor> It(World); It; ++It)
	{
		const FString Group = TagValue(*It, GroupPrefix);
		if (Group.IsEmpty() || (Groups && !Only.Contains(Group)))
		{
			continue;
		}
		TSharedRef<FJsonObject> Entry = MakeShared<FJsonObject>();
		Entry->SetStringField(TEXT("actor"), It->GetActorLabel());
		Entry->SetStringField(TEXT("group"), Group);
		if (const UStaticMeshComponent* C = It->FindComponentByClass<UStaticMeshComponent>(); C && C->GetStaticMesh())
		{
			Entry->SetStringField(TEXT("mesh"), C->GetStaticMesh()->GetPathName());
			Entry->SetStringField(TEXT("collision"), C->GetCollisionProfileName().ToString());
		}
		const FVector Location = It->GetActorLocation();
		TArray<TSharedPtr<FJsonValue>> At;
		At.Add(MakeShared<FJsonValueNumber>(Location.X));
		At.Add(MakeShared<FJsonValueNumber>(Location.Y));
		At.Add(MakeShared<FJsonValueNumber>(Location.Z));
		Entry->SetArrayField(TEXT("location"), At);
		if (It->Tags.Contains(InstancedTag))
		{
			Entry->SetNumberField(TEXT("instances"), Weight(*It));
		}
		else
		{
			Entry->SetStringField(TEXT("node"), TagValue(*It, NodePrefix));
		}
		Out.Add(Obj(Entry));
	}
	return MakeShared<FJsonValueArray>(Out);
}
} // namespace

FString LilaLink::UsedOutside(UWorld* World, const FString& Package)
{
	TRACE_CPUPROFILER_EVENT_SCOPE(LilaLinkPlacements::UsedOutside);

	TArray<FString> Own = ULevel::GetExternalActorsPaths(Package);
	Own.Add(FExternalPackageHelper::GetExternalObjectsPath(Package));
	TArray<FName> Referencers;
	IAssetRegistry::GetChecked().GetReferencers(FName(*Package), Referencers);
	for (const FName& Ref : Referencers)
	{
		const FString Name = Ref.ToString();
		if (Own.ContainsByPredicate([&Name](const FString& P) { return Name.StartsWith(P + TEXT("/")); }))
		{
			continue;
		}
		// This world's own packages are judged by what is in memory: actors just removed still list it until saved.
		const UPackage* Loaded = FindPackage(nullptr, *Name);
		const AActor* Actor = Loaded ? AActor::FindActorInPackage(const_cast<UPackage*>(Loaded)) : nullptr;
		if (Actor && (!IsValid(Actor) || Actor->GetWorld() == World))
		{
			continue;
		}
		if (World && Loaded == World->GetPackage())
		{
			continue;
		}
		return Name;
	}
	return FString();
}

int32 LilaLink::DeleteUnusedDataLayers(UWorld* World, const TArray<const UDataLayerAsset*>& Assets, TArray<FString>& OutErrors)
{
	TRACE_CPUPROFILER_EVENT_SCOPE(LilaLinkPlacements::DeleteUnusedDataLayers);

	UDataLayerEditorSubsystem* Layers = UDataLayerEditorSubsystem::Get();
	if (!Layers)
	{
		return 0;
	}
	int32 Removed = 0;
	TArray<UObject*> Files;
	for (const UDataLayerAsset* Asset : Assets)
	{
		bool bUsed = !Asset;
		for (TActorIterator<AActor> It(World); It && !bUsed; ++It)
		{
			bUsed = IsValid(*It) && It->GetDataLayerAssets().Contains(Asset);
		}
		if (bUsed)
		{
			continue;
		}
		if (UDataLayerInstance* Instance = Layers->GetDataLayerInstance(Asset))
		{
			Layers->DeleteDataLayer(Instance);
			++Removed;
		}
		// Another level's data layer of the same asset (sends to several levels share it) keeps the asset.
		if (LilaLink::UsedOutside(World, Asset->GetPackage()->GetName()).IsEmpty())
		{
			Files.Add(const_cast<UDataLayerAsset*>(Asset));
		}
	}
	if (!Files.IsEmpty() && LilaLink::ForceDeleteAssets(Files) < Files.Num())
	{
		OutErrors.Add(TEXT("some data layer assets could not be deleted; delete them in the Content Browser"));
	}
	return Removed;
}


void LilaLink::AddPlacementMethods(TMap<FString, FLilaLinkMethod>& Methods)
{
	Methods.Add(TEXT("placeInstances"), LilaLink::Sync(&PlaceInstances));
	Methods.Add(TEXT("placeLights"), LilaLink::Sync(&PlaceLights));
	Methods.Add(TEXT("removePlacements"), LilaLink::Sync(&RemovePlacements));
	Methods.Add(TEXT("listPlacements"), LilaLink::Sync(&ListPlacements));
	Methods.Add(TEXT("makeLevelInstance"), LilaLink::Sync(&MakeLevelInstance));
	Methods.Add(TEXT("writeSlotTable"), LilaLink::Sync(&WriteSlotTable));
	Methods.Add(TEXT("placementHashes"), LilaLink::Sync(&PlacementHashes));
}

#undef LOCTEXT_NAMESPACE
