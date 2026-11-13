#include "LilaLinkMethods.h"

#include "LilaLinkComponents.h"
#include "LilaLinkLog.h"
#include "LilaLinkMaterials.h"
#include "LilaLinkSettings.h"

#include "AbcImportSettings.h"
#include "AssetImportTask.h"
#include "AssetRegistry/IAssetRegistry.h"
#include "AssetToolsModule.h"
#include "EditorReimportHandler.h"
#include "Factories/FbxAnimSequenceImportData.h"
#include "Factories/FbxFactory.h"
#include "Factories/FbxImportUI.h"
#include "Factories/FbxSkeletalMeshImportData.h"
#include "Factories/FbxStaticMeshImportData.h"
#include "IAssetTools.h"
#include "InterchangeGenericAnimationPipeline.h"
#include "InterchangeGenericAssetsPipeline.h"
#include "InterchangeGenericAssetsPipelineSharedSettings.h"
#include "InterchangeGenericMaterialPipeline.h"
#include "InterchangeGenericMeshPipeline.h"
#include "InterchangeGenericTexturePipeline.h"
#include "InterchangeAssetImportData.h"
#include "InterchangeManager.h"
#include "InterchangeMeshDefinitions.h"
#include "InterchangeResult.h"
#include "InterchangeResultsContainer.h"
#include "Animation/AnimCurveMetadata.h"
#include "Animation/AnimSequence.h"
#include "Animation/Skeleton.h"
#include "Engine/SkeletalMesh.h"
#include "Engine/SkeletalMeshSocket.h"
#include "Engine/StaticMesh.h"
#include "Containers/Ticker.h"
#include "Editor.h"
#include "HAL/PlatformTime.h"
#include "Misc/PackageName.h"
#include "Misc/Paths.h"
#include "PhysicsEngine/BodySetup.h"
#include "ProfilingDebugging/CpuProfilerTrace.h"
#include "RenderUtils.h"
#include "StaticMeshEditorSubsystem.h"
#include "StaticMeshEditorSubsystemHelpers.h"
#include "UObject/StrongObjectPtr.h"

namespace
{
enum class EKind : uint8
{
	Static,
	Skeletal,
	Animation,
	GeometryCache,
};

struct FImportRequest
{
	FString File;
	FString PackagePath; // /Game/Props
	FString AssetName;   // SM_Crate
	EKind Kind = EKind::Static;
	FString Skeleton;
	/** Skeletal and animation: yaw turning the file's +X forward to the requested facing; unset leaves the pipeline's. */
	TOptional<double> FacingYaw;
	FString LodGroup;
	/** Screen size per LOD; empty leaves them automatic. */
	TArray<float> LodScreenSizes;
	bool bImportMaterials = true;
	bool bImportTextures = true;
	bool bCombineMeshes = true;
	bool bAutoCollision = false;
	/** Static: none, box, convex, complexAsSimple or ucx, applied after the import; empty leaves autoCollision in charge. */
	FString Collision;
	/** Static: complex collision from this LOD (a simplified one for non-Nanite tiles); -1 leaves LOD0. */
	int32 LodForCollision = -1;
	bool bGenerateLightmapUVs = false;
	bool bComputeNormals = false;
	bool bNanite = false;
	/** nanite was given, so an existing asset is set to it after a reimport too. */
	bool bNaniteGiven = false;
	bool bImportAnimations = false;
	/** Reimport keeps the asset's stored settings unless this is set. */
	bool bReapplyOptions = false;
	bool bReplace = false;
	bool bSave = false;
	/** Curves the skeleton marks as material curves, so a material parameter of the same name follows them. */
	TArray<FName> MaterialCurves;
	LilaLink::FMaterialRequest Materials;
	/** Static: a prop's socket, made on SocketMesh on SocketBone at SocketLocation and SocketRotation, so the prop attaches where Lila had it. */
	FString SocketMesh;
	FName SocketName;
	FName SocketBone;
	FVector SocketLocation = FVector::ZeroVector;
	FRotator SocketRotation = FRotator::ZeroRotator;
	/** The object's attribute components; unset when Lila sent none, so the asset's are left as they are. */
	TOptional<TArray<LilaLink::FLilaComponent>> Components;
};

struct FImportOutcome
{
	TArray<FString> Assets;
	TArray<FString> Warnings;
	TArray<FString> Errors;
	FString Method;
	TArray<LilaLink::FSlotResult> Slots;
};

bool ParseRequest(const TSharedPtr<FJsonObject>& Params, FImportRequest& Out, FString& OutError)
{
	TRACE_CPUPROFILER_EVENT_SCOPE(LilaLinkImport::ParseRequest);

	if (!Params->TryGetStringField(TEXT("file"), Out.File) || Out.File.IsEmpty())
	{
		OutError = TEXT("importAsset: give file");
		return false;
	}
	Out.File = FPaths::ConvertRelativePathToFull(Out.File);
	if (!FPaths::FileExists(Out.File))
	{
		OutError = FString::Printf(TEXT("importAsset: no file at %s"), *Out.File);
		return false;
	}

	FString ContentPath;
	if (!Params->TryGetStringField(TEXT("contentPath"), ContentPath) || ContentPath.IsEmpty())
	{
		OutError = TEXT("importAsset: give contentPath, like /Game/Props/SM_Crate");
		return false;
	}
	ContentPath = FPackageName::ObjectPathToPackageName(ContentPath);
	FText Reason;
	if (!FPackageName::IsValidLongPackageName(ContentPath, false, &Reason))
	{
		OutError = FString::Printf(TEXT("importAsset: bad contentPath %s: %s"), *ContentPath, *Reason.ToString());
		return false;
	}
	Out.PackagePath = FPackageName::GetLongPackagePath(ContentPath);
	Out.AssetName = FPackageName::GetShortName(ContentPath);

	FString Kind = TEXT("static");
	Params->TryGetStringField(TEXT("kind"), Kind);
	if (Kind == TEXT("static"))
	{
		Out.Kind = EKind::Static;
	}
	else if (Kind == TEXT("skeletal"))
	{
		Out.Kind = EKind::Skeletal;
	}
	else if (Kind == TEXT("animation"))
	{
		Out.Kind = EKind::Animation;
	}
	else if (Kind == TEXT("geometryCache"))
	{
		Out.Kind = EKind::GeometryCache;
	}
	else
	{
		OutError = FString::Printf(TEXT("importAsset: kind must be static, skeletal, animation or geometryCache, not '%s'"), *Kind);
		return false;
	}

	const TSharedPtr<FJsonObject>* OptionsPtr = nullptr;
	TSharedPtr<FJsonObject> Options = Params->TryGetObjectField(TEXT("options"), OptionsPtr) ? *OptionsPtr : MakeShared<FJsonObject>();
	Options->TryGetStringField(TEXT("skeleton"), Out.Skeleton);
	Options->TryGetStringField(TEXT("lodGroup"), Out.LodGroup);
	FString Facing;
	if (Out.Kind != EKind::Static && Options->TryGetStringField(TEXT("facing"), Facing))
	{
		if (Facing != TEXT("x") && Facing != TEXT("y"))
		{
			OutError = FString::Printf(TEXT("importAsset: facing must be x or y, not '%s'"), *Facing);
			return false;
		}
		Out.FacingYaw = Facing == TEXT("y") ? 90.0 : 0.0;
	}
	const TArray<TSharedPtr<FJsonValue>>* Sizes = nullptr;
	if (Options->TryGetArrayField(TEXT("lodScreenSizes"), Sizes))
	{
		for (const TSharedPtr<FJsonValue>& Size : *Sizes)
		{
			Out.LodScreenSizes.Add(Size.IsValid() ? float(Size->AsNumber()) : 0.0f);
		}
	}
	Options->TryGetBoolField(TEXT("importMaterials"), Out.bImportMaterials);
	Options->TryGetBoolField(TEXT("importTextures"), Out.bImportTextures);
	Options->TryGetBoolField(TEXT("combineMeshes"), Out.bCombineMeshes);
	Options->TryGetBoolField(TEXT("autoCollision"), Out.bAutoCollision);
	Options->TryGetStringField(TEXT("collision"), Out.Collision);
	if (!Out.Collision.IsEmpty() && Out.Collision != TEXT("none") && Out.Collision != TEXT("box") && Out.Collision != TEXT("convex") &&
		Out.Collision != TEXT("complexAsSimple") && Out.Collision != TEXT("ucx"))
	{
		OutError = FString::Printf(TEXT("importAsset: collision is none, box, convex, complexAsSimple or ucx, not '%s'"), *Out.Collision);
		return false;
	}
	Options->TryGetNumberField(TEXT("lodForCollision"), Out.LodForCollision);
	Options->TryGetBoolField(TEXT("generateLightmapUVs"), Out.bGenerateLightmapUVs);
	Out.bNaniteGiven = Options->TryGetBoolField(TEXT("nanite"), Out.bNanite);
	Options->TryGetBoolField(TEXT("importAnimations"), Out.bImportAnimations);
	Options->TryGetBoolField(TEXT("reapplyOptions"), Out.bReapplyOptions);
	Options->TryGetBoolField(TEXT("save"), Out.bSave);
	const TArray<TSharedPtr<FJsonValue>>* Curves = nullptr;
	if (Options->TryGetArrayField(TEXT("materialCurves"), Curves))
	{
		for (const TSharedPtr<FJsonValue>& Curve : *Curves)
		{
			FString Name;
			if (Curve.IsValid() && Curve->TryGetString(Name) && !Name.IsEmpty())
			{
				Out.MaterialCurves.AddUnique(FName(*Name));
			}
		}
	}

	const TSharedPtr<FJsonObject>* Socket = nullptr;
	if (Out.Kind == EKind::Static && Options->TryGetObjectField(TEXT("socket"), Socket) && Socket->IsValid())
	{
		FString Name, Bone;
		(*Socket)->TryGetStringField(TEXT("skeletalMesh"), Out.SocketMesh);
		(*Socket)->TryGetStringField(TEXT("name"), Name);
		(*Socket)->TryGetStringField(TEXT("bone"), Bone);
		Out.SocketName = FName(*Name);
		Out.SocketBone = FName(*(Bone.IsEmpty() ? Name : Bone));
		const TSharedPtr<FJsonObject>* Offset = nullptr;
		if ((*Socket)->TryGetObjectField(TEXT("offset"), Offset) && Offset->IsValid())
		{
			auto VectorField = [&](const TCHAR* Field, FVector& V) {
				const TArray<TSharedPtr<FJsonValue>>* A = nullptr;
				if ((*Offset)->TryGetArrayField(Field, A) && A->Num() == 3)
				{
					V = FVector((*A)[0]->AsNumber(), (*A)[1]->AsNumber(), (*A)[2]->AsNumber());
					return true;
				}
				return false;
			};
			FVector X = FVector::ForwardVector, Y = FVector::RightVector;
			VectorField(TEXT("location"), Out.SocketLocation);
			if (VectorField(TEXT("xAxis"), X) && VectorField(TEXT("yAxis"), Y))
			{
				Out.SocketRotation = FRotationMatrix::MakeFromXY(X, Y).Rotator();
			}
		}
	}

	FString Normals = TEXT("import");
	Options->TryGetStringField(TEXT("normals"), Normals);
	Out.bComputeNormals = Normals == TEXT("compute");

	FString Existing = TEXT("reimport");
	Options->TryGetStringField(TEXT("existing"), Existing);
	Out.bReplace = Existing == TEXT("replace");

	TArray<LilaLink::FLilaComponent> Components;
	if (LilaLink::ParseComponents(Params, Components))
	{
		Out.Components = MoveTemp(Components);
	}

	if (!LilaLink::ParseMaterialRequest(Params, Options, Out.PackagePath, Out.Materials, OutError))
	{
		return false;
	}

	if (Out.Kind == EKind::Animation && Out.Skeleton.IsEmpty())
	{
		OutError = TEXT("importAsset: animation imports need options.skeleton");
		return false;
	}
	return true;
}

FString ObjectPathOf(const FImportRequest& R)
{
	return R.PackagePath / R.AssetName + TEXT(".") + R.AssetName;
}

UObject* FindExisting(const FImportRequest& R)
{
	FAssetData Data = IAssetRegistry::GetChecked().GetAssetByObjectPath(FSoftObjectPath(ObjectPathOf(R)));
	return Data.IsValid() ? Data.GetAsset() : nullptr;
}

/** Starts from the project's FBX pipeline asset so project-wide tweaks carry over, then applies the request. */
UInterchangeGenericAssetsPipeline* MakePipeline(const FImportRequest& R)
{
	TRACE_CPUPROFILER_EVENT_SCOPE(LilaLinkImport::MakePipeline);

	UInterchangeGenericAssetsPipeline* Pipeline = nullptr;
	for (const TCHAR* Path : {TEXT("/Interchange/Pipelines/DefaultFBXOBJAssetsPipeline.DefaultFBXOBJAssetsPipeline"), TEXT("/Interchange/Pipelines/DefaultAssetsPipeline.DefaultAssetsPipeline")})
	{
		if (UInterchangeGenericAssetsPipeline* Template = LoadObject<UInterchangeGenericAssetsPipeline>(nullptr, Path, nullptr, LOAD_NoWarn | LOAD_Quiet))
		{
			Pipeline = DuplicateObject<UInterchangeGenericAssetsPipeline>(Template, GetTransientPackage());
			break;
		}
	}
	if (!Pipeline)
	{
		Pipeline = NewObject<UInterchangeGenericAssetsPipeline>(GetTransientPackage());
	}

	Pipeline->bUseSourceNameForAsset = false;
	Pipeline->AssetName = R.AssetName;
	Pipeline->bSceneNameSubFolder = false;
	Pipeline->bAssetTypeSubFolders = false;
	if (R.FacingYaw.IsSet())
	{
		Pipeline->ImportOffsetRotation = FRotator(0.0, R.FacingYaw.GetValue(), 0.0);
	}

	UInterchangeGenericCommonMeshesProperties* Common = Pipeline->CommonMeshesProperties;
	Common->ForceAllMeshAsType = R.Kind == EKind::Static ? EInterchangeForceMeshType::IFMT_StaticMesh : EInterchangeForceMeshType::IFMT_SkeletalMesh;
	Common->bRecomputeNormals = R.bComputeNormals;
	Common->bImportLods = true;

	UInterchangeGenericCommonSkeletalMeshesAndAnimationsProperties* SkelCommon = Pipeline->CommonSkeletalMeshesAndAnimationsProperties;
	SkelCommon->bImportOnlyAnimations = R.Kind == EKind::Animation;
	if (!R.Skeleton.IsEmpty())
	{
		SkelCommon->Skeleton = TSoftObjectPtr<USkeleton>(FSoftObjectPath(R.Skeleton));
	}

	UInterchangeGenericMeshPipeline* Mesh = Pipeline->MeshPipeline;
	Mesh->bImportStaticMeshes = R.Kind == EKind::Static;
	Mesh->bImportSkeletalMeshes = R.Kind == EKind::Skeletal;
	Mesh->CombineStaticMeshesBehavior = R.bCombineMeshes ? EInterchangeCombineStaticMeshesBehavior::All : EInterchangeCombineStaticMeshesBehavior::DoNotCombine;
	Mesh->bGenerateLightmapUVs = R.bGenerateLightmapUVs;
	Mesh->bBuildNanite = R.bNanite;
	Mesh->LodGroup = R.LodGroup.IsEmpty() ? NAME_None : FName(*R.LodGroup);
	Mesh->bAutoComputeLODScreenSizes = R.LodScreenSizes.IsEmpty();
	Mesh->LODScreenSizes = R.LodScreenSizes;
	// UCX_ and friends still import; this only stops the fallback hull when there are none.
	Mesh->bCollision = true;
	Mesh->bImportCollisionAccordingToMeshName = true;
	if (!R.bAutoCollision || !R.Collision.IsEmpty())
	{
		Mesh->Collision = EInterchangeMeshCollision::None;
	}

	Pipeline->AnimationPipeline->bImportAnimations = R.Kind == EKind::Animation || R.bImportAnimations;

	// Managed materials are matched and made after the import, by slot name.
	Pipeline->MaterialPipeline->bImportMaterials = R.bImportMaterials && !R.Materials.bManaged;
	if (Pipeline->MaterialPipeline->TexturePipeline)
	{
		Pipeline->MaterialPipeline->TexturePipeline->bImportTextures = R.bImportTextures && !R.Materials.bManaged;
	}
	return Pipeline;
}

UInterchangeAssetImportData* InterchangeDataOf(UObject* Asset)
{
	if (UStaticMesh* Mesh = Cast<UStaticMesh>(Asset))
	{
		return Cast<UInterchangeAssetImportData>(Mesh->GetAssetImportData());
	}
	if (USkeletalMesh* Mesh = Cast<USkeletalMesh>(Asset))
	{
		return Cast<UInterchangeAssetImportData>(Mesh->GetAssetImportData());
	}
	if (UAnimSequence* Anim = Cast<UAnimSequence>(Asset))
	{
		return Cast<UInterchangeAssetImportData>(Anim->AssetImportData);
	}
	return nullptr;
}

/**
 * The asset's stored pipelines for a reimport: its settings kept, except material import is off for managed materials
 * and the requested facing replaces the stored one.
 */
TArray<UObject*> StoredPipelinesForReimport(UObject* Existing, const FImportRequest& R)
{
	TRACE_CPUPROFILER_EVENT_SCOPE(LilaLinkImport::StoredPipelinesForReimport);

	TArray<UObject*> Out;
	UInterchangeAssetImportData* Data = InterchangeDataOf(Existing);
	if (!Data)
	{
		return Out;
	}
	for (UObject* Stored : Data->GetPipelines())
	{
		if (!Stored)
		{
			continue;
		}
		UObject* Copy = DuplicateObject<UObject>(Stored, GetTransientPackage());
		if (UInterchangeGenericAssetsPipeline* Generic = Cast<UInterchangeGenericAssetsPipeline>(Copy))
		{
			if (R.Materials.bManaged)
			{
				Generic->MaterialPipeline->bImportMaterials = false;
				if (Generic->MaterialPipeline->TexturePipeline)
				{
					Generic->MaterialPipeline->TexturePipeline->bImportTextures = false;
				}
			}
			if (R.FacingYaw.IsSet())
			{
				Generic->ImportOffsetRotation = FRotator(0.0, R.FacingYaw.GetValue(), 0.0);
			}
		}
		Out.Add(Copy);
	}
	return Out;
}

void CollectResults(const UE::Interchange::FAssetImportResultRef& Result, FImportOutcome& Out)
{
	TRACE_CPUPROFILER_EVENT_SCOPE(LilaLinkImport::CollectResults);

	for (UObject* Object : Result->GetImportedObjects())
	{
		if (Object)
		{
			Out.Assets.AddUnique(Object->GetPathName());
		}
	}
	if (UInterchangeResultsContainer* Results = Result->GetResults())
	{
		for (UInterchangeResult* Item : Results->GetResults())
		{
			if (!Item)
			{
				continue;
			}
			if (Item->GetResultType() == EInterchangeResultType::Warning)
			{
				Out.Warnings.Add(Item->GetText().ToString());
			}
			else if (Item->GetResultType() == EInterchangeResultType::Error)
			{
				Out.Errors.Add(Item->GetText().ToString());
			}
		}
	}
}

/** An Interchange import in flight, polled from the core ticker until it is done. */
struct FInterchangeJob
{
	FImportRequest Request;
	FImportOutcome Outcome;
	TSharedPtr<FLilaLinkCall, ESPMode::ThreadSafe> Call;
	TSharedPtr<UE::Interchange::FImportResult, ESPMode::ThreadSafe> Result;
	TStrongObjectPtr<UInterchangeGenericAssetsPipeline> Pipeline;
	TArray<TStrongObjectPtr<UObject>> StoredPipelines;
	double NextProgressSeconds = 0.0;
};

void FinishImport(const FImportRequest& Request, FImportOutcome& Outcome, const FLilaLinkCallRef& Call);

/**
 * Starts the import without waiting for it: the synchronous Interchange API pumps game-thread tasks while it waits,
 * which must not happen from inside one. False when Interchange cannot read the file.
 */
bool StartInterchangeImport(const FImportRequest& R, UObject* Existing, const FLilaLinkCallRef& Call)
{
	TRACE_CPUPROFILER_EVENT_SCOPE(LilaLinkImport::StartInterchangeImport);

	UInterchangeManager& Manager = UInterchangeManager::GetInterchangeManager();
	UE::Interchange::FScopedSourceData Source(R.File);
	if (!Manager.CanTranslateSourceData(Source.GetSourceData()))
	{
		UE_LOG(LogLilaLink, Log, TEXT("Interchange cannot read %s; using the FBX factory"), *R.File);
		return false;
	}

	TSharedRef<FInterchangeJob> Job = MakeShared<FInterchangeJob>();
	Job->Request = R;
	Job->Call = Call;

	FImportAssetParameters Params;
	Params.bIsAutomated = true;
	Params.bReplaceExisting = true;
	Params.DestinationName = R.AssetName;

	if (!Existing || R.bReapplyOptions)
	{
		Job->Pipeline.Reset(MakePipeline(R));
		Params.OverridePipelines.Add(FSoftObjectPath(Job->Pipeline.Get()));
	}
	else if (R.Materials.bManaged || R.FacingYaw.IsSet())
	{
		for (UObject* Stored : StoredPipelinesForReimport(Existing, R))
		{
			Job->StoredPipelines.Emplace(Stored);
			Params.OverridePipelines.Add(FSoftObjectPath(Stored));
		}
	}

	FString ContentPath = R.PackagePath;
	if (Existing)
	{
		Params.ReimportAsset = Existing;
		ContentPath.Reset();
		Job->Outcome.Method = TEXT("interchange-reimport");
	}
	else
	{
		Job->Outcome.Method = TEXT("interchange");
	}

	UE_LOG(LogLilaLink, Log, TEXT("%s: starting %s of %s into %s"), *Call->Describe(), *Job->Outcome.Method, *R.File, *ObjectPathOf(R));
	Job->Result = Manager.ImportAssetAsync(ContentPath, Source.GetSourceData(), Params);
	Call->Progress(TEXT("importing"));
	Job->NextProgressSeconds = FPlatformTime::Seconds() + 1.0;

	FTSTicker::GetCoreTicker().AddTicker(TEXT("LilaLinkImport"), 0.05f, [Job](float)
	{
		TRACE_CPUPROFILER_EVENT_SCOPE(LilaLinkImport::PollInterchange);

		if (Job->Result->GetStatus() != UE::Interchange::FImportResult::EStatus::Done)
		{
			const double Now = FPlatformTime::Seconds();
			if (Now >= Job->NextProgressSeconds)
			{
				Job->NextProgressSeconds = Now + 1.0;
				Job->Call->Progress(TEXT("importing"));
			}
			return true;
		}
		CollectResults(Job->Result.ToSharedRef(), Job->Outcome);
		if (Job->Outcome.Assets.IsEmpty() && Job->Outcome.Errors.IsEmpty())
		{
			Job->Outcome.Errors.Add(TEXT("Interchange imported nothing; see LogInterchange in the editor log"));
		}
		FinishImport(Job->Request, Job->Outcome, Job->Call.ToSharedRef());
		return false;
	});
	return true;
}

UFbxImportUI* MakeFbxOptions(const FImportRequest& R)
{
	TRACE_CPUPROFILER_EVENT_SCOPE(LilaLinkImport::MakeFbxOptions);

	UFbxImportUI* Options = NewObject<UFbxImportUI>(GetTransientPackage());
	Options->bAutomatedImportShouldDetectType = false;
	Options->bImportMesh = R.Kind != EKind::Animation;
	Options->bImportAsSkeletal = R.Kind != EKind::Static;
	Options->MeshTypeToImport = R.Kind == EKind::Static ? FBXIT_StaticMesh : (R.Kind == EKind::Skeletal ? FBXIT_SkeletalMesh : FBXIT_Animation);
	Options->OriginalImportType = Options->MeshTypeToImport;
	Options->bImportAnimations = R.Kind == EKind::Animation || R.bImportAnimations;
	Options->bImportMaterials = R.bImportMaterials && !R.Materials.bManaged;
	Options->bImportTextures = R.bImportTextures && !R.Materials.bManaged;
	Options->bCreatePhysicsAsset = R.Kind == EKind::Skeletal;
	if (!R.Skeleton.IsEmpty())
	{
		Options->Skeleton = LoadObject<USkeleton>(nullptr, *R.Skeleton);
	}

	const EFBXNormalImportMethod Normals = R.bComputeNormals ? FBXNIM_ComputeNormals : FBXNIM_ImportNormals;
	UFbxStaticMeshImportData* Static = Options->StaticMeshImportData;
	Static->bCombineMeshes = R.bCombineMeshes;
	Static->bAutoGenerateCollision = R.bAutoCollision && R.Collision.IsEmpty();
	Static->bGenerateLightmapUVs = R.bGenerateLightmapUVs;
	Static->bBuildNanite = R.bNanite;
	Static->NormalImportMethod = Normals;
	Static->bImportMeshLODs = true;
	if (!R.LodGroup.IsEmpty())
	{
		Static->StaticMeshLODGroup = FName(*R.LodGroup);
	}
	Options->SkeletalMeshImportData->NormalImportMethod = Normals;
	Options->SkeletalMeshImportData->bImportMeshLODs = true;
	Options->bAutoComputeLodDistances = R.LodScreenSizes.IsEmpty();
	float* Distances[] = {&Options->LodDistance0, &Options->LodDistance1, &Options->LodDistance2, &Options->LodDistance3,
	                      &Options->LodDistance4, &Options->LodDistance5, &Options->LodDistance6, &Options->LodDistance7};
	for (int32 Lod = 0; Lod < R.LodScreenSizes.Num() && Lod < UE_ARRAY_COUNT(Distances); ++Lod)
	{
		*Distances[Lod] = R.LodScreenSizes[Lod];
	}
	return Options;
}

void ImportWithFbxFactory(const FImportRequest& R, FImportOutcome& Out)
{
	TRACE_CPUPROFILER_EVENT_SCOPE(LilaLinkImport::ImportWithFbxFactory);

	Out.Method = TEXT("fbx-factory");
	TStrongObjectPtr<UAssetImportTask> Task(NewObject<UAssetImportTask>(GetTransientPackage()));
	Task->Filename = R.File;
	Task->DestinationPath = R.PackagePath;
	Task->DestinationName = R.AssetName;
	Task->bReplaceExisting = true;
	Task->bReplaceExistingSettings = true;
	Task->bAutomated = true;
	Task->bSave = false;
	// An explicit factory keeps AssetTools off the Interchange route.
	Task->Factory = NewObject<UFbxFactory>(GetTransientPackage());
	Task->Options = MakeFbxOptions(R);

	FAssetToolsModule::GetModule().Get().ImportAssetTasks({Task.Get()});
	for (const FString& Path : Task->ImportedObjectPaths)
	{
		Out.Assets.AddUnique(Path);
	}
	if (Out.Assets.IsEmpty())
	{
		Out.Errors.Add(TEXT("FBX import produced no assets; see the editor log"));
	}
}

/**
 * An .abc as a Geometry Cache through the Alembic importer, every frame of the file. Over an existing cache the factory
 * reimports in place, so actors and sequencer tracks using it keep working.
 */
void ImportGeometryCache(const FImportRequest& R, FImportOutcome& Out)
{
	TRACE_CPUPROFILER_EVENT_SCOPE(LilaLinkImport::ImportGeometryCache);

	Out.Method = TEXT("alembic");
	// Found by name so the module does not link the importer; it is a plugin a project can turn off.
	UClass* FactoryClass = FindObject<UClass>(nullptr, TEXT("/Script/AlembicImporter.AlembicImportFactory"));
	if (!FactoryClass)
	{
		Out.Errors.Add(TEXT("Geometry caches need the Alembic Importer plugin; enable it in this project"));
		return;
	}
	UAbcImportSettings* Settings = NewObject<UAbcImportSettings>(GetTransientPackage());
	Settings->ImportType = EAlembicImportType::GeometryCache;
	Settings->SamplingSettings.SamplingType = EAlembicSamplingType::PerFrame;
	// Lila writes real times, so a range from frame 32 starts 32 frames in; skipping the empty start keeps frame 0 the first sample.
	Settings->SamplingSettings.bSkipEmpty = true;
	Settings->SamplingSettings.FrameStart = 0;
	Settings->SamplingSettings.FrameEnd = 0;
	Settings->NormalGenerationSettings.bRecomputeNormals = R.bComputeNormals;
	Settings->MaterialSettings.bFindMaterials = true;
	Settings->MaterialSettings.bCreateMaterials = false;
	Settings->GeometryCacheSettings.bFlattenTracks = true;
	// ML Deformer training maps cache vertices to the skeletal mesh's by these.
	Settings->GeometryCacheSettings.bStoreImportedVertexNumbers = true;

	TStrongObjectPtr<UAssetImportTask> Task(NewObject<UAssetImportTask>(GetTransientPackage()));
	Task->Filename = R.File;
	Task->DestinationPath = R.PackagePath;
	Task->DestinationName = R.AssetName;
	Task->bReplaceExisting = true;
	Task->bReplaceExistingSettings = true;
	Task->bAutomated = true;
	Task->bSave = false;
	Task->Factory = NewObject<UFactory>(GetTransientPackage(), FactoryClass);
	Task->Options = Settings;

	FAssetToolsModule::GetModule().Get().ImportAssetTasks({Task.Get()});
	for (const FString& Path : Task->ImportedObjectPaths)
	{
		Out.Assets.AddUnique(Path);
	}
	if (Out.Assets.IsEmpty())
	{
		if (UObject* Existing = FindExisting(R))
		{
			Out.Assets.Add(Existing->GetPathName());
		}
		else
		{
			Out.Errors.Add(TEXT("Alembic import produced no Geometry Cache; see the AlembicImporter messages in the editor log"));
		}
	}
}

void ReimportWithManager(const FImportRequest& R, UObject* Existing, FImportOutcome& Out)
{
	TRACE_CPUPROFILER_EVENT_SCOPE(LilaLinkImport::ReimportWithManager);

	Out.Method = TEXT("reimport");
	FReimportManager* Manager = FReimportManager::Instance();
	if (!Manager->CanReimport(Existing))
	{
		Out.Errors.Add(FString::Printf(TEXT("%s has no import source to reimport from; pass options.existing=\"replace\""), *Existing->GetPathName()));
		return;
	}
	Manager->UpdateReimportPaths(Existing, {R.File});
	if (Manager->Reimport(Existing, false, false, R.File, nullptr, INDEX_NONE, false, true))
	{
		Out.Assets.Add(Existing->GetPathName());
	}
	else
	{
		Out.Errors.Add(FString::Printf(TEXT("Reimport of %s failed; see the editor log"), *Existing->GetPathName()));
	}
}

TSharedPtr<FJsonValue> ToJson(const FImportOutcome& Out)
{
	auto Strings = [](const TArray<FString>& In)
	{
		TArray<TSharedPtr<FJsonValue>> Values;
		for (const FString& S : In)
		{
			Values.Add(MakeShared<FJsonValueString>(S));
		}
		return Values;
	};
	TSharedRef<FJsonObject> Json = MakeShared<FJsonObject>();
	Json->SetArrayField(TEXT("assets"), Strings(Out.Assets));
	Json->SetArrayField(TEXT("warnings"), Strings(Out.Warnings));
	Json->SetArrayField(TEXT("errors"), Strings(Out.Errors));
	Json->SetStringField(TEXT("method"), Out.Method);
	TArray<TSharedPtr<FJsonValue>> Slots;
	for (const LilaLink::FSlotResult& S : Out.Slots)
	{
		TSharedRef<FJsonObject> Slot = MakeShared<FJsonObject>();
		Slot->SetStringField(TEXT("asset"), S.Asset);
		Slot->SetStringField(TEXT("slot"), S.Slot.ToString());
		Slot->SetStringField(TEXT("material"), S.Material);
		Slot->SetStringField(TEXT("how"), S.How);
		Slots.Add(MakeShared<FJsonValueObject>(Slot));
	}
	Json->SetArrayField(TEXT("materialSlots"), Slots);
	return MakeShared<FJsonValueObject>(Json);
}

/**
 * Sets the requested LOD screen sizes on the imported meshes. The import pipelines only apply theirs to new assets, so a
 * reimport needs this to pick up changed sizes. Rebuilds only meshes whose sizes changed.
 */
void ApplyLodScreenSizes(const FImportRequest& Request, FImportOutcome& Outcome)
{
	TRACE_CPUPROFILER_EVENT_SCOPE(LilaLinkImport::ApplyLodScreenSizes);

	const TArray<float>& Sizes = Request.LodScreenSizes;
	for (const FString& Path : Outcome.Assets)
	{
		UObject* Asset = FindObject<UObject>(nullptr, *Path);
		if (UStaticMesh* Mesh = Cast<UStaticMesh>(Asset))
		{
			bool bChanged = Mesh->GetAutoComputeLODScreenSize();
			for (int32 Lod = 0; Lod < Mesh->GetNumSourceModels() && Lod < Sizes.Num(); ++Lod)
			{
				bChanged |= !FMath::IsNearlyEqual(Mesh->GetSourceModel(Lod).ScreenSize.Default, Sizes[Lod]);
			}
			if (!bChanged)
			{
				continue;
			}
			Mesh->Modify();
			Mesh->SetAutoComputeLODScreenSize(false);
			for (int32 Lod = 0; Lod < Mesh->GetNumSourceModels() && Lod < Sizes.Num(); ++Lod)
			{
				Mesh->GetSourceModel(Lod).ScreenSize = Sizes[Lod];
			}
			Mesh->PostEditChange();
		}
		else if (USkeletalMesh* Skel = Cast<USkeletalMesh>(Asset))
		{
			bool bChanged = false;
			for (int32 Lod = 0; Lod < Skel->GetLODNum() && Lod < Sizes.Num(); ++Lod)
			{
				bChanged |= !FMath::IsNearlyEqual(Skel->GetLODInfo(Lod)->ScreenSize.Default, Sizes[Lod]);
			}
			if (!bChanged)
			{
				continue;
			}
			Skel->Modify();
			for (int32 Lod = 0; Lod < Skel->GetLODNum() && Lod < Sizes.Num(); ++Lod)
			{
				Skel->GetLODInfo(Lod)->ScreenSize = Sizes[Lod];
			}
			Skel->PostEditChange();
		}
	}
}

/** Flags the requested curves as material curves on the skeletons of the imported meshes and animations. */
void ApplyMaterialCurves(const FImportRequest& Request, FImportOutcome& Outcome)
{
	TRACE_CPUPROFILER_EVENT_SCOPE(LilaLinkImport::ApplyMaterialCurves);

	TSet<USkeleton*> Skeletons;
	for (const FString& Path : Outcome.Assets)
	{
		UObject* Asset = FindObject<UObject>(nullptr, *Path);
		if (USkeleton* Skeleton = Cast<USkeleton>(Asset))
		{
			Skeletons.Add(Skeleton);
		}
		else if (USkeletalMesh* Mesh = Cast<USkeletalMesh>(Asset))
		{
			Skeletons.Add(Mesh->GetSkeleton());
		}
		else if (UAnimSequence* Anim = Cast<UAnimSequence>(Asset))
		{
			Skeletons.Add(Anim->GetSkeleton());
		}
	}
	if (!Request.Skeleton.IsEmpty())
	{
		Skeletons.Add(LoadObject<USkeleton>(nullptr, *Request.Skeleton));
	}
	Skeletons.Remove(nullptr);
	for (USkeleton* Skeleton : Skeletons)
	{
		bool bChanged = false;
		for (const FName& Curve : Request.MaterialCurves)
		{
			const FCurveMetaData* Meta = Skeleton->GetCurveMetaData(Curve);
			if (!Meta || !Meta->Type.bMaterial)
			{
				if (!bChanged)
				{
					Skeleton->Modify();
				}
				Skeleton->AccumulateCurveMetaData(Curve, true, Meta && Meta->Type.bMorphtarget);
				bChanged = true;
			}
		}
		if (bChanged)
		{
			Outcome.Assets.AddUnique(Skeleton->GetPathName());
		}
	}
}

/**
 * The socket a prop attaches to, on its character's skeletal mesh: on the bone Lila baked where the prop sits with no
 * offset, or on an existing bone with the offset the prop is held at, so attaching the prop's actor to it places the
 * prop as in Lila.
 */
void ApplySocket(const FImportRequest& Request, FImportOutcome& Outcome)
{
	TRACE_CPUPROFILER_EVENT_SCOPE(LilaLinkImport::ApplySocket);

	// A mesh imported earlier in the same sync is in memory but not saved yet, so find it by its full object path first.
	FString ObjectPath = Request.SocketMesh;
	if (!ObjectPath.Contains(TEXT(".")))
	{
		ObjectPath += TEXT(".") + FPackageName::GetShortName(ObjectPath);
	}
	USkeletalMesh* Mesh = FindObject<USkeletalMesh>(nullptr, *ObjectPath);
	if (!Mesh)
	{
		Mesh = LoadObject<USkeletalMesh>(nullptr, *ObjectPath);
	}
	if (!Mesh)
	{
		Outcome.Warnings.Add(FString::Printf(TEXT("No skeletal mesh %s for socket %s yet; send the character, then this prop again"),
			*Request.SocketMesh, *Request.SocketName.ToString()));
		return;
	}
	if (Mesh->GetRefSkeleton().FindBoneIndex(Request.SocketBone) == INDEX_NONE)
	{
		Outcome.Warnings.Add(FString::Printf(TEXT("%s has no bone %s for socket %s"), *Mesh->GetPathName(), *Request.SocketBone.ToString(),
			*Request.SocketName.ToString()));
		return;
	}
	USkeletalMeshSocket* Socket = Mesh->FindSocket(Request.SocketName);
	if (Socket && Socket->BoneName == Request.SocketBone && Socket->RelativeLocation.Equals(Request.SocketLocation, 1e-3)
		&& Socket->RelativeRotation.Equals(Request.SocketRotation, 1e-3) && Socket->RelativeScale.Equals(FVector::OneVector))
	{
		return;
	}
	if (!Socket)
	{
		Mesh->Modify();
		Socket = NewObject<USkeletalMeshSocket>(Mesh);
		Socket->SocketName = Request.SocketName;
		Mesh->AddSocket(Socket);
	}
	Socket->Modify();
	Socket->BoneName = Request.SocketBone;
	Socket->RelativeLocation = Request.SocketLocation;
	Socket->RelativeRotation = Request.SocketRotation;
	Socket->RelativeScale = FVector::OneVector;
	Socket->GetOuter()->MarkPackageDirty();
	Outcome.Assets.AddUnique(Socket->GetOuter()->GetPathName());
}

/** The asset the import was asked for: the one at its content path, else the first mesh or animation it made. */
UObject* RequestedAsset(const FImportRequest& Request, const FImportOutcome& Outcome)
{
	if (UObject* Asset = FindObject<UObject>(nullptr, *ObjectPathOf(Request)))
	{
		return Asset;
	}
	for (const FString& Path : Outcome.Assets)
	{
		UObject* Asset = FindObject<UObject>(nullptr, *Path);
		if (Cast<UStaticMesh>(Asset) || Cast<USkeletalMesh>(Asset) || Cast<UAnimSequence>(Asset))
		{
			return Asset;
		}
	}
	return nullptr;
}

void ApplyComponents(const FImportRequest& Request, FImportOutcome& Outcome)
{
	TRACE_CPUPROFILER_EVENT_SCOPE(LilaLinkImport::ApplyComponents);

	UObject* Asset = RequestedAsset(Request, Outcome);
	if (!Asset)
	{
		if (!Request.Components->IsEmpty())
		{
			Outcome.Warnings.Add(TEXT("No imported asset to hold the attribute components"));
		}
		return;
	}
	TArray<FString> Created;
	LilaLink::ApplyComponentsToAsset(Asset, Request.Components.GetValue(), Request.PackagePath, Outcome.Warnings, Created);
	Asset->MarkPackageDirty();
	for (const FString& C : Created)
	{
		Outcome.Assets.AddUnique(C);
	}
}

/** The requested collision on the imported static mesh, replacing whatever the import gave it (ucx keeps the file's). */
/**
 * Nanite and the collision LOD set on the asset itself with reapplyOptions: a reimport over an existing static mesh keeps
 * the asset's own settings whatever the pipeline asks, so the requested ones are written here and the mesh rebuilt when
 * they changed.
 */
void ApplyStaticSettings(const FImportRequest& Request, FImportOutcome& Outcome)
{
	TRACE_CPUPROFILER_EVENT_SCOPE(LilaLinkImport::ApplyStaticSettings);

	UStaticMesh* Mesh = Cast<UStaticMesh>(RequestedAsset(Request, Outcome));
	if (!Mesh)
	{
		return;
	}
	bool bChanged = false;
	if (Request.bNaniteGiven && Mesh->NaniteSettings.bEnabled != Request.bNanite)
	{
		Mesh->Modify();
		Mesh->NaniteSettings.bEnabled = Request.bNanite;
		bChanged = true;
	}
	if (Request.LodForCollision >= 0)
	{
		const int32 Lod = FMath::Clamp(Request.LodForCollision, 0, FMath::Max(0, Mesh->GetNumSourceModels() - 1));
		if (Mesh->LODForCollision != Lod)
		{
			Mesh->Modify();
			Mesh->LODForCollision = Lod;
			bChanged = true;
		}
	}
	if (!bChanged)
	{
		return;
	}
	Mesh->Build(false);
	Mesh->PostEditChange();
	if (UBodySetup* Body = Mesh->GetBodySetup())
	{
		Body->Modify();
		Body->InvalidatePhysicsData();
		Body->CreatePhysicsMeshes();
	}
	Mesh->MarkPackageDirty();
}

void ApplyCollision(const FImportRequest& Request, FImportOutcome& Outcome)
{
	TRACE_CPUPROFILER_EVENT_SCOPE(LilaLinkImport::ApplyCollision);

	UStaticMesh* Mesh = Cast<UStaticMesh>(RequestedAsset(Request, Outcome));
	// The subsystem's class isn't exported, so its StaticClass can't be linked: look it up by path instead.
	UClass* SubsystemClass = FindObject<UClass>(nullptr, TEXT("/Script/StaticMeshEditor.StaticMeshEditorSubsystem"));
	UStaticMeshEditorSubsystem* Meshes =
		GEditor && SubsystemClass ? static_cast<UStaticMeshEditorSubsystem*>(GEditor->GetEditorSubsystemBase(SubsystemClass)) : nullptr;
	if (!Mesh || !Meshes)
	{
		Outcome.Warnings.Add(FString::Printf(TEXT("Collision %s not applied: no static mesh to apply it to"), *Request.Collision));
		return;
	}
	Mesh->Modify();
	// GetSimpleCollisionCount leaves out convex hulls, which is what UCX_ meshes become.
	const int32 Shapes = FMath::Max(0, Meshes->GetSimpleCollisionCount(Mesh)) + FMath::Max(0, Meshes->GetConvexCollisionCount(Mesh));
	if (Request.Collision == TEXT("ucx"))
	{
		if (Shapes == 0)
		{
			Outcome.Warnings.Add(TEXT("collision ucx: the file has no UCX_ meshes, so the asset has no simple collision"));
		}
	}
	else
	{
		if (Shapes > 0)
		{
			Meshes->RemoveCollisionsWithNotification(Mesh, false);
		}
		if (Request.Collision == TEXT("box"))
		{
			Meshes->AddSimpleCollisionsWithNotification(Mesh, EScriptCollisionShapeType::Box, false);
		}
		else if (Request.Collision == TEXT("convex"))
		{
			// The editor's Auto Convex Collision defaults: up to 4 hulls of 16 vertices from 100k voxels.
			Meshes->SetConvexDecompositionCollisionsWithNotification(Mesh, 4, 16, 100000, false);
		}
	}
	if (!Mesh->GetBodySetup())
	{
		Mesh->CreateBodySetup();
	}
	if (UBodySetup* Body = Mesh->GetBodySetup())
	{
		Body->Modify();
		Body->CollisionTraceFlag = Request.Collision == TEXT("complexAsSimple") ? CTF_UseComplexAsSimple : CTF_UseDefault;
		Body->InvalidatePhysicsData();
		Body->CreatePhysicsMeshes();
	}
	Mesh->PostEditChange();
	Mesh->MarkPackageDirty();
}

/** Runs the import again once Mesh has finished building, polling on the core ticker; gives up after five minutes. */
void WaitForMeshBuild(const UStaticMesh* Mesh, const TSharedPtr<FJsonObject>& Params, const FLilaLinkCallRef& Call)
{
	TRACE_CPUPROFILER_EVENT_SCOPE(LilaLinkImport::WaitForMeshBuild);

	UE_LOG(LogLilaLink, Log, TEXT("%s: waiting for %s to finish building"), *Call->Describe(), *Mesh->GetName());
	Call->Progress(TEXT("waiting for build"));
	const double Deadline = FPlatformTime::Seconds() + 300.0;
	TWeakObjectPtr<const UStaticMesh> Weak(Mesh);
	FTSTicker::GetCoreTicker().AddTicker(TEXT("LilaLinkWaitForBuild"), 1.0f, [Weak, Params, Call, Deadline](float)
	{
		if (Weak.IsValid() && Weak->IsCompiling())
		{
			if (FPlatformTime::Seconds() < Deadline)
			{
				Call->Progress(TEXT("waiting for build"));
				return true;
			}
			Call->Reply(nullptr, FString::Printf(TEXT("%s was still building after five minutes"), *Weak->GetName()));
			return false;
		}
		LilaLink::ImportAsset(Params, Call);
		return false;
	});
}

void FinishImport(const FImportRequest& Request, FImportOutcome& Outcome, const FLilaLinkCallRef& Call)
{
	TRACE_CPUPROFILER_EVENT_SCOPE(LilaLinkImport::FinishImport);

	if (!Request.SocketMesh.IsEmpty() && !Request.SocketName.IsNone() && Outcome.Errors.IsEmpty())
	{
		ApplySocket(Request, Outcome);
	}

	if (!Request.LodScreenSizes.IsEmpty() && !Outcome.Assets.IsEmpty())
	{
		ApplyLodScreenSizes(Request, Outcome);
	}

	if (Request.Kind == EKind::Static && Request.bReapplyOptions && (Request.bNaniteGiven || Request.LodForCollision >= 0) && Outcome.Errors.IsEmpty())
	{
		ApplyStaticSettings(Request, Outcome);
	}

	if (Request.Kind == EKind::Static && !Request.Collision.IsEmpty() && Outcome.Errors.IsEmpty())
	{
		Call->Progress(TEXT("collision"));
		ApplyCollision(Request, Outcome);
	}

	if (!Request.MaterialCurves.IsEmpty())
	{
		ApplyMaterialCurves(Request, Outcome);
	}

	if (!NaniteAssembliesSupported())
	{
		for (const FString& Path : Outcome.Assets)
		{
			const UStaticMesh* Mesh = LoadObject<UStaticMesh>(nullptr, *Path);
			if (Mesh && Mesh->IsNaniteAssembly())
			{
				Outcome.Errors.Add(FString::Printf(TEXT("%s is a Nanite Assembly, which this project does not build: set r.Nanite.AllowAssemblies=1 under [/Script/Engine.RendererSettings] in DefaultEngine.ini (or enable Nanite Foliage) and restart the editor"), *Mesh->GetName()));
			}
		}
	}

	if (Request.Materials.bManaged && !Outcome.Assets.IsEmpty())
	{
		Call->Progress(TEXT("assigning materials"));
		TArray<FString> Created;
		LilaLink::ApplyMaterials(Request.Materials, Outcome.Assets, Created, Outcome.Warnings, Outcome.Slots);
		for (const FString& C : Created)
		{
			Outcome.Assets.AddUnique(C);
		}
	}

	if (Request.Components.IsSet() && Outcome.Errors.IsEmpty())
	{
		ApplyComponents(Request, Outcome);
	}

	for (const FString& W : Outcome.Warnings)
	{
		UE_LOG(LogLilaLink, Warning, TEXT("%s: %s"), *Request.AssetName, *W);
	}
	for (const FString& E : Outcome.Errors)
	{
		UE_LOG(LogLilaLink, Error, TEXT("%s: %s"), *Request.AssetName, *E);
	}
	UE_LOG(LogLilaLink, Log, TEXT("Imported %s -> %s via %s (%d assets)"), *Request.File, *ObjectPathOf(Request), *Outcome.Method, Outcome.Assets.Num());

	if (!Outcome.Assets.IsEmpty() && (Request.bSave || GetDefault<ULilaLinkSettings>()->bAutoSaveAfterImport))
	{
		Call->Progress(TEXT("saving"));
		LilaLink::SaveAssetPaths(Outcome.Assets, Outcome.Errors);
	}
	Call->Reply(ToJson(Outcome));
}
} // namespace

void LilaLink::ImportAsset(const TSharedPtr<FJsonObject>& Params, const FLilaLinkCallRef& Call)
{
	TRACE_CPUPROFILER_EVENT_SCOPE(LilaLink::ImportAsset);

	FImportRequest Request;
	FString Error;
	if (!ParseRequest(Params, Request, Error))
	{
		Call->Reply(nullptr, Error);
		return;
	}

	// Reimporting a mesh that is still building asserts in the engine; a Nanite Assembly rebuilds whenever its parts change.
	if (const UStaticMesh* Building = Cast<UStaticMesh>(Request.bReplace ? nullptr : FindExisting(Request)); Building && Building->IsCompiling())
	{
		WaitForMeshBuild(Building, Params, Call);
		return;
	}

	if (Request.Kind == EKind::GeometryCache)
	{
		Call->Progress(TEXT("importing"));
		FImportOutcome Outcome;
		ImportGeometryCache(Request, Outcome);
		FinishImport(Request, Outcome, Call);
		return;
	}

	UObject* Existing = Request.bReplace ? nullptr : FindExisting(Request);
	if (Existing && Request.Materials.bManaged)
	{
		LilaLink::SnapshotMaterials(Existing, Request.Materials);
	}
	const bool bInterchangeOwnsExisting = [&]
	{
		TArray<FString> Files;
		return Existing && UInterchangeManager::GetInterchangeManager().CanReimport(Existing, Files);
	}();

	FImportOutcome Outcome;
	if (Existing && !bInterchangeOwnsExisting)
	{
		ReimportWithManager(Request, Existing, Outcome);
	}
	else if (UInterchangeManager::IsInterchangeImportEnabled() && StartInterchangeImport(Request, Existing, Call))
	{
		return;
	}
	else if (FPaths::GetExtension(Request.File).StartsWith(TEXT("usd"), ESearchCase::IgnoreCase))
	{
		Outcome.Errors.Add(FString::Printf(TEXT("%s is USD, which this project cannot import: enable the Interchange OpenUSD plugin"), *FPaths::GetCleanFilename(Request.File)));
	}
	else
	{
		ImportWithFbxFactory(Request, Outcome);
	}
	FinishImport(Request, Outcome, Call);
}
