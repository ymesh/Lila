#include "LilaLinkMorphPaths.h"

#include "LilaLinkMethods.h"

#include "AnimGraphNode_LilaMorphPaths.h"
#include "AnimGraphNode_LinkedInputPose.h"
#include "AnimGraphNode_Root.h"
#include "Animation/AnimBlueprint.h"
#include "Animation/AnimInstance.h"
#include "Animation/Skeleton.h"
#include "AssetToolsModule.h"
#include "EdGraph/EdGraph.h"
#include "EdGraphSchema_K2.h"
#include "Engine/SkeletalMesh.h"
#include "Factories/AnimBlueprintFactory.h"
#include "IAssetTools.h"
#include "Kismet2/KismetEditorUtilities.h"
#include "LilaMorphPaths.h"
#include "Misc/PackageName.h"
#include "ProfilingDebugging/CpuProfilerTrace.h"

namespace
{
using namespace LilaLink;

UEdGraphPin* MorphPosePin(UEdGraphNode* Node, EEdGraphPinDirection Direction)
{
	for (UEdGraphPin* Pin : Node->Pins)
	{
		if (Pin->Direction == Direction && Pin->PinType.PinCategory == UEdGraphSchema_K2::PC_Struct &&
			Pin->PinType.PinSubCategoryObject == FPoseLink::StaticStruct())
		{
			return Pin;
		}
	}
	return nullptr;
}

UEdGraph* MorphAnimGraph(UAnimBlueprint* Blueprint)
{
	for (UEdGraph* Graph : Blueprint->FunctionGraphs)
	{
		if (Graph && Graph->GetFName() == UEdGraphSchema_K2::GN_AnimGraph)
		{
			return Graph;
		}
	}
	return nullptr;
}

bool ReadPaths(const TSharedPtr<FJsonObject>& Params, TArray<FLilaMorphPath>& Out, FString& OutError)
{
	TRACE_CPUPROFILER_EVENT_SCOPE(LilaLinkMorphPaths::ReadPaths);

	const TArray<TSharedPtr<FJsonValue>>* List = nullptr;
	if (!Params->TryGetArrayField(TEXT("paths"), List))
	{
		OutError = TEXT("morphPaths: no paths");
		return false;
	}
	for (const TSharedPtr<FJsonValue>& Value : *List)
	{
		const TSharedPtr<FJsonObject> P = Value->AsObject();
		if (!P)
		{
			continue;
		}
		FLilaMorphPath Path;
		Path.Curve = FName(*P->GetStringField(TEXT("curve")));
		for (const TSharedPtr<FJsonValue>& W : P->GetArrayField(TEXT("weights")))
		{
			Path.Weights.Add(float(W->AsNumber()));
		}
		for (const TSharedPtr<FJsonValue>& S : P->GetArrayField(TEXT("shapes")))
		{
			const TSharedPtr<FJsonObject> SO = S->AsObject();
			FLilaMorphShapeCurve Shape;
			Shape.Shape = FName(*SO->GetStringField(TEXT("name")));
			for (const TSharedPtr<FJsonValue>& V : SO->GetArrayField(TEXT("values")))
			{
				Shape.Values.Add(float(V->AsNumber()));
			}
			if (Shape.Values.Num() != Path.Weights.Num())
			{
				OutError = FString::Printf(TEXT("morphPaths: %s has %d values for %d weights"), *Shape.Shape.ToString(), Shape.Values.Num(), Path.Weights.Num());
				return false;
			}
			Path.Shapes.Add(MoveTemp(Shape));
		}
		Out.Add(MoveTemp(Path));
	}
	return true;
}

/** A post-process Anim Blueprint running Input Pose -> Lila Morph Paths -> Output Pose; empty with OutError on failure. */
FString MakePostProcess(USkeletalMesh* Mesh, const FString& MeshPath, FString& OutError)
{
	TRACE_CPUPROFILER_EVENT_SCOPE(LilaLinkMorphPaths::MakePostProcess);

	FString Name = FPackageName::GetShortName(MeshPath);
	Name.RemoveFromStart(TEXT("SK_"));
	const FString AbpPath = FPackageName::GetLongPackagePath(MeshPath) / TEXT("ABP_") + Name + TEXT("_MorphPaths");
	UAnimBlueprint* Abp = LoadObject<UAnimBlueprint>(nullptr, *AbpPath);
	if (!Abp)
	{
		UAnimBlueprintFactory* Factory = NewObject<UAnimBlueprintFactory>();
		Factory->TargetSkeleton = Mesh->GetSkeleton();
		Factory->PreviewSkeletalMesh = Mesh;
		Factory->ParentClass = UAnimInstance::StaticClass();
		Factory->BlueprintType = BPTYPE_Normal;
		IAssetTools& Tools = FModuleManager::LoadModuleChecked<FAssetToolsModule>("AssetTools").Get();
		Abp = Cast<UAnimBlueprint>(
			Tools.CreateAsset(FPackageName::GetShortName(AbpPath), FPackageName::GetLongPackagePath(AbpPath), UAnimBlueprint::StaticClass(), Factory));
		if (!Abp)
		{
			OutError = FString::Printf(TEXT("morphPaths: couldn't create %s"), *AbpPath);
			return FString();
		}
	}
	UEdGraph* Graph = MorphAnimGraph(Abp);
	if (!Graph)
	{
		OutError = FString::Printf(TEXT("morphPaths: %s has no AnimGraph"), *AbpPath);
		return FString();
	}
	UAnimGraphNode_Root* Root = nullptr;
	UAnimGraphNode_LilaMorphPaths* PathsNode = nullptr;
	UAnimGraphNode_LinkedInputPose* Input = nullptr;
	for (UEdGraphNode* Node : Graph->Nodes)
	{
		Root = Root ? Root : Cast<UAnimGraphNode_Root>(Node);
		PathsNode = PathsNode ? PathsNode : Cast<UAnimGraphNode_LilaMorphPaths>(Node);
		Input = Input ? Input : Cast<UAnimGraphNode_LinkedInputPose>(Node);
	}
	if (!Root)
	{
		OutError = FString::Printf(TEXT("morphPaths: %s has no Output Pose"), *AbpPath);
		return FString();
	}
	if (!Input)
	{
		FGraphNodeCreator<UAnimGraphNode_LinkedInputPose> Creator(*Graph);
		Input = Creator.CreateNode();
		Input->NodePosX = Root->NodePosX - 600;
		Input->NodePosY = Root->NodePosY;
		Creator.Finalize();
	}
	if (!PathsNode)
	{
		FGraphNodeCreator<UAnimGraphNode_LilaMorphPaths> Creator(*Graph);
		PathsNode = Creator.CreateNode();
		PathsNode->NodePosX = Root->NodePosX - 300;
		PathsNode->NodePosY = Root->NodePosY;
		Creator.Finalize();
	}
	UEdGraphPin* InputOut = MorphPosePin(Input, EGPD_Output);
	UEdGraphPin* PathsIn = MorphPosePin(PathsNode, EGPD_Input);
	UEdGraphPin* PathsOut = MorphPosePin(PathsNode, EGPD_Output);
	UEdGraphPin* RootIn = MorphPosePin(Root, EGPD_Input);
	if (!InputOut || !PathsIn || !PathsOut || !RootIn)
	{
		OutError = TEXT("morphPaths: an anim graph node has no pose pin");
		return FString();
	}
	PathsIn->BreakAllPinLinks();
	RootIn->BreakAllPinLinks();
	InputOut->MakeLinkTo(PathsIn);
	PathsOut->MakeLinkTo(RootIn);
	FKismetEditorUtilities::CompileBlueprint(Abp);
	if (Abp->Status == BS_Error)
	{
		OutError = FString::Printf(TEXT("morphPaths: %s doesn't compile"), *AbpPath);
		return FString();
	}
	Mesh->SetPostProcessAnimBlueprint(TSubclassOf<UAnimInstance>(Abp->GeneratedClass.Get()));
	return AbpPath;
}

bool HasMorphPathsNode(const UClass* AnimClass)
{
	const UAnimBlueprint* Abp = AnimClass ? Cast<UAnimBlueprint>(AnimClass->ClassGeneratedBy) : nullptr;
	if (!Abp)
	{
		return false;
	}
	for (const UEdGraph* Graph : Abp->FunctionGraphs)
	{
		for (const UEdGraphNode* Node : Graph ? Graph->Nodes : TArray<TObjectPtr<UEdGraphNode>>())
		{
			if (Cast<UAnimGraphNode_LilaMorphPaths>(Node))
			{
				return true;
			}
		}
	}
	return false;
}

TSharedPtr<FJsonValue> MorphPaths(const TSharedPtr<FJsonObject>& Params, FString& OutError)
{
	TRACE_CPUPROFILER_EVENT_SCOPE(LilaLinkMorphPaths::MorphPaths);

	const FString MeshPath = Params->GetStringField(TEXT("skeletalMesh"));
	USkeletalMesh* Mesh = LoadObject<USkeletalMesh>(nullptr, *MeshPath);
	if (!Mesh || !Mesh->GetSkeleton())
	{
		OutError = FString::Printf(TEXT("morphPaths: no skeletal mesh at %s"), *MeshPath);
		return nullptr;
	}
	TArray<FLilaMorphPath> Paths;
	if (!ReadPaths(Params, Paths, OutError))
	{
		return nullptr;
	}

	Mesh->Modify();
	ULilaMorphPathsData* Data = Mesh->GetAssetUserData<ULilaMorphPathsData>();
	if (!Data)
	{
		Data = NewObject<ULilaMorphPathsData>(Mesh, NAME_None, RF_Public | RF_Transactional);
		Mesh->AddAssetUserData(Data);
	}
	Data->Modify();
	Data->Paths = MoveTemp(Paths);

	// Shape curves drive morph targets only when the skeleton marks them so.
	USkeleton* Skeleton = Mesh->GetSkeleton();
	Skeleton->Modify();
	for (const FLilaMorphPath& Path : Data->Paths)
	{
		Skeleton->AccumulateCurveMetaData(Path.Curve, false, false);
		for (const FLilaMorphShapeCurve& Shape : Path.Shapes)
		{
			Skeleton->AccumulateCurveMetaData(Shape.Shape, false, true);
		}
	}

	TSharedRef<FJsonObject> Out = MakeShared<FJsonObject>();
	TArray<TSharedPtr<FJsonValue>> Warnings;
	TArray<FString> Saved = {MeshPath, Skeleton->GetPathName()};
	bool bPostProcess = true;
	Params->TryGetBoolField(TEXT("postProcess"), bPostProcess);
	const UClass* Existing = Mesh->GetPostProcessAnimBlueprint().Get();
	if (bPostProcess && !Existing)
	{
		const FString AbpPath = MakePostProcess(Mesh, MeshPath, OutError);
		if (AbpPath.IsEmpty())
		{
			return nullptr;
		}
		Out->SetStringField(TEXT("animBlueprint"), AbpPath);
		Saved.Add(AbpPath);
	}
	else if (Existing && !HasMorphPathsNode(Existing))
	{
		const FString ExistingPath = FPackageName::ObjectPathToPackageName(Existing->GetPathName());
		Warnings.Add(MakeShared<FJsonValueString>(
			ExistingPath.EndsWith(TEXT("_Expressions"))
				? FString::Printf(TEXT("%s runs Lila's expression rig, which drives the morph shapes already; use one or the other"), *ExistingPath)
				: FString::Printf(TEXT("%s already has the post-process Anim Blueprint %s: add a Lila Morph Paths node to it"), *MeshPath, *ExistingPath)));
	}
	Mesh->PostEditChange();

	TArray<FString> Errors;
	LilaLink::SaveAssetPaths(Saved, Errors);
	for (const FString& E : Errors)
	{
		Warnings.Add(MakeShared<FJsonValueString>(E));
	}
	Out->SetNumberField(TEXT("paths"), Data->Paths.Num());
	Out->SetArrayField(TEXT("warnings"), Warnings);
	return MakeShared<FJsonValueObject>(Out);
}
} // namespace

void LilaLink::AddMorphPathMethods(TMap<FString, FLilaLinkMethod>& Methods)
{
	Methods.Add(TEXT("morphPaths"), LilaLink::Sync(&MorphPaths));
}
