#include "LilaLinkCrowd.h"

#include "LilaLinkLog.h"
#include "LilaLinkMethods.h"

#include "Animation/AnimSequence.h"
#include "Animation/SkeletalMeshActor.h"
#include "AssetRegistry/AssetRegistryModule.h"
#include "Components/SkeletalMeshComponent.h"
#include "Editor.h"
#include "EngineUtils.h"
#include "FileHelpers.h"
#include "LevelSequence.h"
#include "Misc/PackageName.h"
#include "MovieScene.h"
#include "ProfilingDebugging/CpuProfilerTrace.h"
#include "Tracks/MovieSceneSkeletalAnimationTrack.h"
#include "UObject/Package.h"

namespace
{
FString ToObjectPath(const FString& Path)
{
	return Path.Contains(TEXT(".")) ? Path : Path + TEXT(".") + FPackageName::GetShortName(Path);
}

/** The Level Sequence at Path, made when missing, emptied of the bindings an earlier send left. */
ULevelSequence* SequenceAt(const FString& Path, FString& OutError)
{
	TRACE_CPUPROFILER_EVENT_SCOPE(LilaLinkCrowd::SequenceAt);

	if (ULevelSequence* Existing = LoadObject<ULevelSequence>(nullptr, *ToObjectPath(Path), nullptr, LOAD_NoWarn | LOAD_Quiet))
	{
		UMovieScene* Scene = Existing->GetMovieScene();
		for (int32 Index = Scene->GetPossessableCount() - 1; Index >= 0; --Index)
		{
			const FGuid Guid = Scene->GetPossessable(Index).GetGuid();
			Existing->UnbindPossessableObjects(Guid);
			Scene->RemovePossessable(Guid);
		}
		return Existing;
	}
	if (!FPackageName::IsValidLongPackageName(Path))
	{
		OutError = FString::Printf(TEXT("createCrowdSequence: %s is not a content path like /Game/Crowds/LS_Crowd"), *Path);
		return nullptr;
	}
	UPackage* Package = CreatePackage(*Path);
	ULevelSequence* Sequence = NewObject<ULevelSequence>(Package, *FPackageName::GetShortName(Path), RF_Public | RF_Standalone | RF_Transactional);
	Sequence->Initialize();
	FAssetRegistryModule::AssetCreated(Sequence);
	Package->MarkPackageDirty();
	return Sequence;
}

TSharedPtr<FJsonValue> CreateCrowdSequence(const TSharedPtr<FJsonObject>& Params, FString& OutError)
{
	TRACE_CPUPROFILER_EVENT_SCOPE(LilaLinkCrowd::CreateCrowdSequence);

	FString Path;
	if (!Params->TryGetStringField(TEXT("path"), Path) || Path.IsEmpty())
	{
		OutError = TEXT("createCrowdSequence: give path");
		return nullptr;
	}
	const TArray<TSharedPtr<FJsonValue>>* Agents = nullptr;
	if (!Params->TryGetArrayField(TEXT("agents"), Agents) || Agents->IsEmpty())
	{
		OutError = TEXT("createCrowdSequence: give agents");
		return nullptr;
	}
	double Fps = 30.0, Frames = 0.0;
	Params->TryGetNumberField(TEXT("fps"), Fps);
	Params->TryGetNumberField(TEXT("frames"), Frames);
	UWorld* World = GEditor ? GEditor->GetEditorWorldContext().World() : nullptr;
	if (!World)
	{
		OutError = TEXT("createCrowdSequence: no editor level is open");
		return nullptr;
	}
	ULevelSequence* Sequence = SequenceAt(Path, OutError);
	if (!Sequence)
	{
		return nullptr;
	}
	UMovieScene* Scene = Sequence->GetMovieScene();
	const FFrameRate Rate(FMath::Max(1, FMath::RoundToInt(Fps)), 1);
	Scene->SetDisplayRate(Rate);
	const FFrameNumber End = FFrameRate::TransformTime(FFrameTime(FFrameNumber(static_cast<int32>(FMath::RoundToInt(Frames)))), Rate, Scene->GetTickResolution()).FloorToFrame();
	Scene->SetPlaybackRange(TRange<FFrameNumber>(FFrameNumber(0), FMath::Max(End, FFrameNumber(1))));

	// Actors from an earlier send of this sequence go first.
	const FName Folder(*(TEXT("Crowds/") + FPackageName::GetShortName(Path)));
	TArray<AActor*> Old;
	for (TActorIterator<ASkeletalMeshActor> It(World); It; ++It)
	{
		if (It->GetFolderPath() == Folder)
		{
			Old.Add(*It);
		}
	}
	for (AActor* Actor : Old)
	{
		World->EditorDestroyActor(Actor, true);
	}

	TArray<FString> Warnings;
	int32 Made = 0;
	for (const TSharedPtr<FJsonValue>& Value : *Agents)
	{
		const TSharedPtr<FJsonObject>* Agent = nullptr;
		if (!Value.IsValid() || !Value->TryGetObject(Agent))
		{
			continue;
		}
		FString Name, MeshPath, AnimPath;
		(*Agent)->TryGetStringField(TEXT("name"), Name);
		(*Agent)->TryGetStringField(TEXT("mesh"), MeshPath);
		(*Agent)->TryGetStringField(TEXT("animation"), AnimPath);
		USkeletalMesh* Mesh = LoadObject<USkeletalMesh>(nullptr, *ToObjectPath(MeshPath));
		UAnimSequence* Anim = LoadObject<UAnimSequence>(nullptr, *ToObjectPath(AnimPath));
		if (!Mesh || !Anim)
		{
			Warnings.Add(FString::Printf(TEXT("%s: no %s at %s"), *Name, Mesh ? TEXT("animation") : TEXT("skeletal mesh"), Mesh ? *AnimPath : *MeshPath));
			continue;
		}
		FActorSpawnParameters Spawn;
		Spawn.ObjectFlags = RF_Transactional;
		ASkeletalMeshActor* Actor = World->SpawnActor<ASkeletalMeshActor>(FVector::ZeroVector, FRotator::ZeroRotator, Spawn);
		if (!Actor)
		{
			Warnings.Add(FString::Printf(TEXT("%s: the actor could not be spawned"), *Name));
			continue;
		}
		Actor->SetActorLabel(Name);
		Actor->SetFolderPath(Folder);
		Actor->GetSkeletalMeshComponent()->SetSkeletalMeshAsset(Mesh);
		const FGuid Guid = Scene->AddPossessable(Actor->GetActorLabel(), Actor->GetClass());
		Sequence->BindPossessableObject(Guid, *Actor, World);
		UMovieSceneSkeletalAnimationTrack* Track = Scene->AddTrack<UMovieSceneSkeletalAnimationTrack>(Guid);
		Track->AddNewAnimation(FFrameNumber(0), Anim);
		++Made;
	}
	Sequence->MarkPackageDirty();
	UEditorLoadingAndSavingUtils::SavePackages({Sequence->GetPackage()}, true);

	TSharedPtr<FJsonObject> Out = MakeShared<FJsonObject>();
	Out->SetStringField(TEXT("sequence"), Path);
	Out->SetNumberField(TEXT("actors"), Made);
	TArray<TSharedPtr<FJsonValue>> W;
	for (const FString& S : Warnings)
	{
		W.Add(MakeShared<FJsonValueString>(S));
	}
	Out->SetArrayField(TEXT("warnings"), W);
	if (Made == 0)
	{
		OutError = TEXT("createCrowdSequence: no agent could be placed") + (Warnings.IsEmpty() ? FString() : TEXT(": ") + Warnings[0]);
		return nullptr;
	}
	return MakeShared<FJsonValueObject>(Out);
}
} // namespace

void LilaLink::AddCrowdMethods(TMap<FString, FLilaLinkMethod>& Methods)
{
	Methods.Add(TEXT("createCrowdSequence"), LilaLink::Sync(&CreateCrowdSequence));
}
