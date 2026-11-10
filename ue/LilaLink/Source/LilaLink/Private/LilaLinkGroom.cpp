#include "LilaLinkGroom.h"

#include "LilaLinkLog.h"
#include "LilaLinkMaterialFunction.h"
#include "LilaLinkServer.h"

#include "AssetImportTask.h"
#include "AssetToolsModule.h"
#include "Factories/Factory.h"
#include "Components/SkeletalMeshComponent.h"
#include "Editor.h"
#include "EngineUtils.h"
#include "Engine/SkeletalMesh.h"
#include "GroomAsset.h"
#include "GroomBindingAsset.h"
#include "GroomComponent.h"
#include "HairStrandsCore.h"
#include "GroomAssetRendering.h"
#include "IAssetTools.h"
#include "Materials/Material.h"
#include "Materials/MaterialExpressionComponentMask.h"
#include "Materials/MaterialExpressionHairAttributes.h"
#include "Materials/MaterialExpressionLinearInterpolate.h"
#include "Materials/MaterialExpressionMultiply.h"
#include "Materials/MaterialExpressionOneMinus.h"
#include "Materials/MaterialFunction.h"
#include "Misc/Paths.h"
#include "ProfilingDebugging/CpuProfilerTrace.h"
#include "UObject/Package.h"

namespace
{
using namespace LilaLink;

constexpr int32 HairVersion = 1;
const TCHAR* FunctionName = TEXT("MF_LilaHair");
const TCHAR* MaterialName = TEXT("M_LilaHair");
const TCHAR* VersionTag = TEXT("Lila Hair v");
const FName SlotName(TEXT("LilaHair"));

int32 VersionOf(const UMaterialFunction* Function)
{
	const FString Description = Function->Description;
	const int32 At = Description.Find(VersionTag);
	return At == INDEX_NONE ? 0 : FCString::Atoi(*Description.RightChop(At + FCString::Strlen(VersionTag)));
}

int32 OutputIndex(UMaterialExpression* Expression, const TCHAR* Name)
{
	const TArray<FExpressionOutput>& Outputs = Expression->GetOutputs();
	for (int32 I = 0; I < Outputs.Num(); ++I)
	{
		if (Outputs[I].OutputName == Name)
		{
			return I;
		}
	}
	return 0;
}

/** M_LilaHair on MF_LilaHair: the Hair shading model, used with hair strands and cards. */
UMaterial* HairMaterial(bool bRebuild, TArray<FString>& OutCreated, FString& OutError)
{
	UMaterialFunction* Function = HairFunction(bRebuild, OutCreated);
	if (!Function)
	{
		OutError = TEXT("MF_LilaHair could not be made");
		return nullptr;
	}
	bool bCreated = false;
	UMaterial* Material = FindOrCreateMaterial(MastersFolderPath(), MaterialName, bCreated);
	if (!Material)
	{
		OutError = TEXT("M_LilaHair could not be made");
		return nullptr;
	}
	if (bCreated)
	{
		OutCreated.AddUnique(Material->GetPathName());
	}
	if (!bCreated && !bRebuild)
	{
		return Material;
	}
	Material->SetShadingModel(MSM_Hair);
	Material->SetMaterialUsage(MATUSAGE_HairStrands);
	UMaterialExpressionMaterialFunctionCall* Call = ResetToFunctionCall(Material, Function);
	if (!ConnectFunctionOutputs(Call, {{TEXT("BaseColor"), MP_BaseColor}, {TEXT("Roughness"), MP_Roughness}, {TEXT("Scatter"), MP_Metallic},
										  {TEXT("Specular"), MP_Specular}, {TEXT("Tangent"), MP_Tangent}}))
	{
		OutError = TEXT("MF_LilaHair is missing an output");
		return nullptr;
	}
	Material->PostEditChange();
	Material->MarkPackageDirty();
	return Material;
}

/** The groom factory, found by name: HairStrandsEditor doesn't export it. */
UFactory* GroomFactory()
{
	UClass* Class = FindObject<UClass>(nullptr, TEXT("/Script/HairStrandsEditor.HairStrandsFactory"));
	return Class ? NewObject<UFactory>(GetTransientPackage(), Class) : nullptr;
}

/** Every hair group renders with Material, in the one LilaHair slot. */
void SetGroomMaterial(UGroomAsset* Groom, UMaterialInterface* Material)
{
	TArray<FHairGroupsMaterial> Materials = Groom->GetHairGroupsMaterials();
	FHairGroupsMaterial* Slot = Materials.FindByPredicate([](const FHairGroupsMaterial& M) { return M.SlotName == SlotName; });
	if (!Slot)
	{
		Slot = &Materials.AddDefaulted_GetRef();
		Slot->SlotName = SlotName;
	}
	Slot->Material = Material;
	Groom->SetHairGroupsMaterials(Materials);
	for (FHairGroupsRendering& Rendering : Groom->GetHairGroupsRendering())
	{
		Rendering.MaterialSlotName = SlotName;
	}
	Groom->PostEditChange();
	Groom->MarkPackageDirty();
}

/**
 * The groom bound to a skeletal mesh: Folder/<Name>_Binding made or retargeted and built, so the hair follows the mesh's
 * skinning; with an actor label, a groom component on that actor's first skeletal mesh component, using the binding.
 */
UGroomBindingAsset* BindGroom(UGroomAsset* Groom, const FString& Folder, const FString& Name, const FString& MeshPath, const FString& SourcePath,
	int32 Points, const FString& ActorLabel, TArray<FString>& OutAssets, TArray<FString>& OutWarnings)
{
	USkeletalMesh* Target = LoadObject<USkeletalMesh>(nullptr, *MeshPath);
	if (!Target)
	{
		OutWarnings.Add(TEXT("bindTo: no skeletal mesh ") + MeshPath);
		return nullptr;
	}
	USkeletalMesh* Source = SourcePath.IsEmpty() ? nullptr : LoadObject<USkeletalMesh>(nullptr, *SourcePath);
	const FString BindingPath = Folder / (Name + TEXT("_Binding"));
	UGroomBindingAsset* Binding = LoadObject<UGroomBindingAsset>(nullptr, *(BindingPath + TEXT(".") + Name + TEXT("_Binding")), nullptr, LOAD_NoWarn | LOAD_Quiet);
	if (Binding)
	{
		Binding->SetGroom(Groom);
		Binding->SetSourceSkeletalMesh(Source);
		Binding->SetTargetSkeletalMesh(Target);
		Binding->SetNumInterpolationPoints(Points);
		Binding->MarkPackageDirty();
	}
	else
	{
		Binding = FHairStrandsCore::CreateGroomBindingAsset(EGroomBindingMeshType::SkeletalMesh, BindingPath, nullptr, Groom, Source, Target, Points, -1);
	}
	if (!Binding)
	{
		OutWarnings.Add(TEXT("bindTo: the binding to ") + MeshPath + TEXT(" could not be made"));
		return nullptr;
	}
	Binding->Build();
	OutAssets.Add(Binding->GetPathName());

	if (ActorLabel.IsEmpty() || !GEditor)
	{
		return Binding;
	}
	UWorld* World = GEditor->GetEditorWorldContext().World();
	AActor* Actor = nullptr;
	for (TActorIterator<AActor> It(World); It && !Actor; ++It)
	{
		if (It->GetActorLabel() == ActorLabel)
		{
			Actor = *It;
		}
	}
	USkeletalMeshComponent* Skin = Actor ? Actor->FindComponentByClass<USkeletalMeshComponent>() : nullptr;
	if (!Skin)
	{
		OutWarnings.Add(TEXT("actor: no actor labelled ") + ActorLabel + TEXT(" with a skeletal mesh"));
		return Binding;
	}
	const FName ComponentName(*(Name + TEXT("_Groom")));
	UGroomComponent* Hair = nullptr;
	TArray<UGroomComponent*> Existing;
	Actor->GetComponents(Existing);
	for (UGroomComponent* C : Existing)
	{
		if (C->GetFName() == ComponentName)
		{
			Hair = C;
		}
	}
	Actor->Modify();
	if (!Hair)
	{
		Hair = NewObject<UGroomComponent>(Actor, ComponentName, RF_Transactional);
		Hair->SetupAttachment(Skin);
		Actor->AddInstanceComponent(Hair);
		Hair->RegisterComponent();
	}
	Hair->SetGroomAsset(Groom, Binding);
	return Binding;
}

void ImportGroom(const TSharedPtr<FJsonObject>& Params, const FLilaLinkCallRef& Call)
{
	TRACE_CPUPROFILER_EVENT_SCOPE(LilaLinkGroom::ImportGroom);

	FString File, Folder = TEXT("/Game/Grooms"), Name;
	if (!Params->TryGetStringField(TEXT("file"), File) || !FPaths::FileExists(File))
	{
		Call->Reply(nullptr, TEXT("importGroom: no file ") + File);
		return;
	}
	Params->TryGetStringField(TEXT("folder"), Folder);
	if (!Params->TryGetStringField(TEXT("name"), Name) || Name.IsEmpty())
	{
		Name = FPaths::GetBaseFilename(File);
	}
	bool bMaterial = true, bRebuildMaterial = false;
	Params->TryGetBoolField(TEXT("material"), bMaterial);
	Params->TryGetBoolField(TEXT("rebuildMaterial"), bRebuildMaterial);

	UFactory* Factory = GroomFactory();
	if (!Factory)
	{
		Call->Reply(nullptr, TEXT("importGroom: the HairStrands plugin is not enabled in this project"));
		return;
	}
	UAssetImportTask* Task = NewObject<UAssetImportTask>();
	Task->Filename = File;
	Task->DestinationPath = Folder;
	Task->DestinationName = Name;
	Task->bAutomated = true;
	Task->bReplaceExisting = true;
	Task->bSave = false;
	Task->Factory = Factory;
	Call->Progress(TEXT("importing"));
	FAssetToolsModule::GetModule().Get().ImportAssetTasks({Task});

	TArray<FString> Assets, Created, Warnings;
	UGroomAsset* Groom = nullptr;
	for (UObject* Object : Task->GetObjects())
	{
		if (UGroomAsset* G = Cast<UGroomAsset>(Object))
		{
			Groom = G;
		}
		if (Object)
		{
			Assets.Add(Object->GetPathName());
		}
	}
	if (!Groom)
	{
		Call->Reply(nullptr, TEXT("importGroom: Unreal made no groom from ") + File + TEXT("; see the editor log"));
		return;
	}
	if (bMaterial)
	{
		FString Error;
		if (UMaterial* Material = HairMaterial(bRebuildMaterial, Created, Error))
		{
			SetGroomMaterial(Groom, Material);
		}
		else
		{
			Warnings.Add(Error);
		}
	}
	FString BindTo, BindSource, Actor;
	if (Params->TryGetStringField(TEXT("bindTo"), BindTo) && !BindTo.IsEmpty())
	{
		Params->TryGetStringField(TEXT("bindSource"), BindSource);
		Params->TryGetStringField(TEXT("actor"), Actor);
		int32 Points = 100;
		Params->TryGetNumberField(TEXT("interpolationPoints"), Points);
		Call->Progress(TEXT("binding"));
		BindGroom(Groom, Folder, Name, BindTo, BindSource, Points, Actor, Assets, Warnings);
	}
	UE_LOG(LogLilaLink, Log, TEXT("importGroom %s -> %s"), *File, *Groom->GetPathName());
	auto Strings = [](const TArray<FString>& In) {
		TArray<TSharedPtr<FJsonValue>> Out;
		for (const FString& S : In)
		{
			Out.Add(MakeShared<FJsonValueString>(S));
		}
		return Out;
	};
	TSharedRef<FJsonObject> Out = MakeShared<FJsonObject>();
	Out->SetStringField(TEXT("groom"), Groom->GetPathName());
	Out->SetArrayField(TEXT("assets"), Strings(Assets));
	Out->SetArrayField(TEXT("created"), Strings(Created));
	Out->SetArrayField(TEXT("warnings"), Strings(Warnings));
	Out->SetNumberField(TEXT("groups"), Groom->GetHairGroupsRendering().Num());
	Call->Reply(MakeShared<FJsonValueObject>(Out));
}
} // namespace

UMaterialFunction* LilaLink::HairFunction(bool bRebuild, TArray<FString>& OutCreated)
{
	TRACE_CPUPROFILER_EVENT_SCOPE(LilaLinkGroom::HairFunction);

	bool bCreated = false;
	UMaterialFunction* Function = FindOrCreateMaterialFunction(MastersFolderPath(), FunctionName, bCreated);
	if (!Function)
	{
		return nullptr;
	}
	if (bCreated)
	{
		OutCreated.AddUnique(Function->GetPathName());
	}
	if (!bCreated && !bRebuild && VersionOf(Function) >= HairVersion)
	{
		return Function;
	}

	FFunctionGraph G(Function, *FString::Printf(TEXT("Lila Hair: a groom's strands for the Hair shading model, coloured from the groom's "
												  "groom_color and darker toward the roots. Wire its outputs to a Hair material's Base "
												  "Color, Roughness, Metallic (scatter), Specular and Tangent. %s%d"),
										   VersionTag, HairVersion));
	auto* Hair = G.New<UMaterialExpressionHairAttributes>(-1400);
	UMaterialExpression* Tint = G.VectorInput(TEXT("HairTint"), FLinearColor::White, TEXT("Hair"), TEXT("Multiplies the groom's colour."));
	UMaterialExpression* Darken =
		G.ScalarInput(TEXT("RootDarkening"), 0.35f, TEXT("Hair"), TEXT("How much darker the hair is at its roots, 0 to 1, fading out toward the tips."));
	UMaterialExpression* Roughness = G.ScalarInput(TEXT("HairRoughness"), 0.3f, TEXT("Hair"), TEXT("Strand roughness, 0 to 1."));
	UMaterialExpression* Scatter = G.ScalarInput(TEXT("HairScatter"), 0.5f, TEXT("Hair"), TEXT("Light passing through the hair, 0 to 1."));
	UMaterialExpression* Specular = G.ScalarInput(TEXT("HairSpecular"), 0.5f, TEXT("Hair"), TEXT("Strength of the highlights, 0 to 1."));

	auto* Rgb = G.New<UMaterialExpressionComponentMask>(-1000);
	Rgb->Input.Connect(OutputIndex(Hair, TEXT("BaseColor")), Hair);
	Rgb->R = Rgb->G = Rgb->B = true;
	Rgb->A = false;
	auto* Tinted = G.New<UMaterialExpressionMultiply>(-800);
	Tinted->A.Connect(0, Rgb);
	Tinted->B.Connect(0, Tint);
	auto* Root = G.New<UMaterialExpressionOneMinus>(-800);
	Root->Input.Connect(0, Darken);
	auto* Along = G.New<UMaterialExpressionLinearInterpolate>(-600);
	Along->A.Connect(0, Root);
	Along->ConstB = 1.0f;
	Along->Alpha.Connect(OutputIndex(Hair, TEXT("U")), Hair);
	auto* Color = G.New<UMaterialExpressionMultiply>(-400);
	Color->A.Connect(0, Tinted);
	Color->B.Connect(0, Along);

	G.Output(Color, TEXT("BaseColor"), TEXT("Connect to Base Color."));
	G.Output(Roughness, TEXT("Roughness"), TEXT("Connect to Roughness."));
	G.Output(Scatter, TEXT("Scatter"), TEXT("Connect to Metallic, the Hair model's scatter."));
	G.Output(Specular, TEXT("Specular"), TEXT("Connect to Specular."));
	G.Output(Hair, TEXT("Tangent"), TEXT("Connect to Tangent."), OutputIndex(Hair, TEXT("Tangent")));
	G.Finish();
	return Function;
}

void LilaLink::AddGroomMethods(TMap<FString, FLilaLinkMethod>& Methods)
{
	Methods.Add(TEXT("importGroom"), &ImportGroom);
}
