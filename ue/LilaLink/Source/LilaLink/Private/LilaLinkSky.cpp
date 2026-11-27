#include "LilaLinkSky.h"

#include "LilaLinkLog.h"
#include "LilaLinkMaterialFunction.h"
#include "LilaLinkMethods.h"

#include "AssetImportTask.h"
#include "AssetToolsModule.h"
#include "Components/ExponentialHeightFogComponent.h"
#include "Components/HeterogeneousVolumeComponent.h"
#include "Components/SkyAtmosphereComponent.h"
#include "Components/SkyLightComponent.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Editor.h"
#include "Engine/ExponentialHeightFog.h"
#include "Engine/SkyLight.h"
#include "Engine/World.h"
#include "EngineUtils.h"
#include "Factories/MaterialInstanceConstantFactoryNew.h"
#include "IAssetTools.h"
#include "MaterialEditingLibrary.h"
#include "Materials/Material.h"
#include "Materials/MaterialExpressionComponentMask.h"
#include "Materials/MaterialExpressionCustom.h"
#include "Materials/MaterialExpressionMaterialFunctionCall.h"
#include "Materials/MaterialExpressionScalarParameter.h"
#include "Materials/MaterialExpressionSparseVolumeTextureSample.h"
#include "Materials/MaterialExpressionVectorParameter.h"
#include "Materials/MaterialFunction.h"
#include "Materials/MaterialInstanceConstant.h"
#include "ProfilingDebugging/CpuProfilerTrace.h"
#include "ScopedTransaction.h"
#include "SparseVolumeTexture/SparseVolumeTexture.h"

#define LOCTEXT_NAMESPACE "LilaLinkSky"

namespace LilaLink
{
namespace
{
constexpr int32 Version = 2;
const TCHAR* VersionTag = TEXT("Lila Volume v");
const TCHAR* FunctionName = TEXT("MF_LilaVolume");
const TCHAR* MaterialName = TEXT("M_LilaVolume");

UWorld* SkyWorld()
{
	return GEditor ? GEditor->GetEditorWorldContext().World() : nullptr;
}

double Num(const TSharedPtr<FJsonObject>& O, const TCHAR* Key, double Fallback)
{
	double V = Fallback;
	O->TryGetNumberField(Key, V);
	return V;
}

FLinearColor Color(const TSharedPtr<FJsonObject>& O, const TCHAR* Key, const FLinearColor& Fallback)
{
	const TArray<TSharedPtr<FJsonValue>>* A = nullptr;
	if (!O->TryGetArrayField(Key, A) || A->Num() < 3)
	{
		return Fallback;
	}
	return FLinearColor(float((*A)[0]->AsNumber()), float((*A)[1]->AsNumber()), float((*A)[2]->AsNumber()));
}

FVector Vec(const TSharedPtr<FJsonObject>& O, const TCHAR* Key)
{
	const TArray<TSharedPtr<FJsonValue>>* A = nullptr;
	if (!O->TryGetArrayField(Key, A) || A->Num() < 3)
	{
		return FVector::ZeroVector;
	}
	return FVector((*A)[0]->AsNumber(), (*A)[1]->AsNumber(), (*A)[2]->AsNumber());
}

/** The level's first actor of the class, or a new one labelled Label. */
template <class T> T* FindOrSpawn(UWorld* World, const TCHAR* Label)
{
	for (TActorIterator<T> It(World); It; ++It)
	{
		return *It;
	}
	T* Actor = World->SpawnActor<T>();
	if (Actor)
	{
		Actor->SetActorLabel(Label);
	}
	return Actor;
}

void ApplyAtmosphere(USkyAtmosphereComponent* C, const TSharedPtr<FJsonObject>& A)
{
	FString Mode;
	A->TryGetStringField(TEXT("transformMode"), Mode);
	C->TransformMode = Mode == TEXT("PlanetCenterAtComponentTransform") ? ESkyAtmosphereTransformMode::PlanetCenterAtComponentTransform
					   : Mode == TEXT("PlanetTopAtComponentTransform")  ? ESkyAtmosphereTransformMode::PlanetTopAtComponentTransform
																		 : ESkyAtmosphereTransformMode::PlanetTopAtAbsoluteWorldOrigin;
	C->SetWorldLocation(Vec(A, TEXT("location")));
	C->SetBottomRadius(float(Num(A, TEXT("bottomRadius"), 6360.0)));
	C->SetGroundAlbedo(Color(A, TEXT("groundAlbedo"), FLinearColor(0.4f, 0.4f, 0.4f)).ToFColor(true));
	C->SetAtmosphereHeight(float(Num(A, TEXT("atmosphereHeight"), 60.0)));
	C->SetMultiScatteringFactor(float(Num(A, TEXT("multiScatteringFactor"), 1.0)));
	C->SetRayleighScatteringScale(float(Num(A, TEXT("rayleighScatteringScale"), 0.0331)));
	C->SetRayleighScattering(Color(A, TEXT("rayleighScattering"), FLinearColor(0.175287f, 0.409607f, 1.0f)));
	C->SetRayleighExponentialDistribution(float(Num(A, TEXT("rayleighExponentialDistribution"), 8.0)));
	C->SetMieScatteringScale(float(Num(A, TEXT("mieScatteringScale"), 0.003996)));
	C->SetMieScattering(Color(A, TEXT("mieScattering"), FLinearColor::White));
	C->SetMieAbsorptionScale(float(Num(A, TEXT("mieAbsorptionScale"), 0.000444)));
	C->SetMieAbsorption(Color(A, TEXT("mieAbsorption"), FLinearColor::White));
	C->SetMieAnisotropy(float(Num(A, TEXT("mieAnisotropy"), 0.8)));
	C->SetMieExponentialDistribution(float(Num(A, TEXT("mieExponentialDistribution"), 1.2)));
	C->SetOtherAbsorptionScale(float(Num(A, TEXT("otherAbsorptionScale"), 0.001881)));
	C->SetOtherAbsorption(Color(A, TEXT("otherAbsorption"), FLinearColor(0.345561f, 1.0f, 0.045188f)));
	C->OtherTentDistribution.TipAltitude = float(Num(A, TEXT("otherTentTipAltitude"), 25.0));
	C->OtherTentDistribution.TipValue = float(Num(A, TEXT("otherTentTipValue"), 1.0));
	C->OtherTentDistribution.Width = float(Num(A, TEXT("otherTentWidth"), 15.0));
	C->SetSkyLuminanceFactor(Color(A, TEXT("skyLuminanceFactor"), FLinearColor::White));
	C->SetAerialPespectiveViewDistanceScale(float(Num(A, TEXT("aerialPerspectiveViewDistanceScale"), 1.0)));
	C->SetAerialPerspectiveStartDepth(float(Num(A, TEXT("aerialPerspectiveStartDepth"), 0.1)));
	C->SetHeightFogContribution(float(Num(A, TEXT("heightFogContribution"), 1.0)));
	C->SetTransmittanceMinLightElevationAngle(float(Num(A, TEXT("transmittanceMinLightElevationAngle"), -90.0)));
	C->MarkRenderStateDirty();
}

void ApplyFog(AExponentialHeightFog* Actor, UExponentialHeightFogComponent* C, const TSharedPtr<FJsonObject>& F)
{
	FVector Location = Actor->GetActorLocation();
	Location.Z = Num(F, TEXT("height"), 0.0);
	Actor->SetActorLocation(Location);
	C->SetFogDensity(float(Num(F, TEXT("fogDensity"), 0.02)));
	C->SetFogHeightFalloff(float(Num(F, TEXT("fogHeightFalloff"), 0.2)));
	C->SetFogInscatteringColor(Color(F, TEXT("inscatteringColor"), FLinearColor(0.45f, 0.5f, 0.6f)));
	C->SetStartDistance(float(Num(F, TEXT("startDistance"), 0.0)));
	C->SetDirectionalInscatteringExponent(float(Num(F, TEXT("directionalInscatteringExponent"), 4.0)));
	const float Directional = float(Num(F, TEXT("directionalInscatteringIntensity"), 0.0));
	C->SetDirectionalInscatteringColor(FLinearColor(Directional, Directional, Directional));
	bool bVolumetric = true;
	F->TryGetBoolField(TEXT("volumetricFog"), bVolumetric);
	C->SetVolumetricFog(bVolumetric);
}

TSharedPtr<FJsonValue> SetSky(const TSharedPtr<FJsonObject>& Params, FString& OutError)
{
	TRACE_CPUPROFILER_EVENT_SCOPE(LilaLinkSky::SetSky);

	UWorld* World = SkyWorld();
	if (!World)
	{
		OutError = TEXT("setSky: no level is open");
		return nullptr;
	}
	const FScopedTransaction Transaction(LOCTEXT("SetSky", "Lila Sky"));
	TSharedRef<FJsonObject> Out = MakeShared<FJsonObject>();
	const TSharedPtr<FJsonObject>* Atmosphere = nullptr;
	if (Params->TryGetObjectField(TEXT("atmosphere"), Atmosphere))
	{
		ASkyAtmosphere* Sky = FindOrSpawn<ASkyAtmosphere>(World, TEXT("LilaSkyAtmosphere"));
		if (Sky && Sky->GetComponent())
		{
			Sky->Modify();
			Sky->GetComponent()->Modify();
			ApplyAtmosphere(Sky->GetComponent(), *Atmosphere);
			Out->SetStringField(TEXT("skyAtmosphere"), Sky->GetActorLabel());
		}
		// The sky lights the level through a sky light capturing it every frame, as Lila's viewport lights with the baked sky.
		ASkyLight* Light = FindOrSpawn<ASkyLight>(World, TEXT("LilaSkyLight"));
		if (Light && Light->GetLightComponent())
		{
			USkyLightComponent* C = Light->GetLightComponent();
			Light->Modify();
			C->Modify();
			C->SourceType = SLS_CapturedScene;
			C->bRealTimeCapture = true;
			C->SetMobility(EComponentMobility::Movable);
			C->MarkRenderStateDirty();
			Out->SetStringField(TEXT("skyLight"), Light->GetActorLabel());
		}
	}
	const TSharedPtr<FJsonObject>* Fog = nullptr;
	if (Params->TryGetObjectField(TEXT("heightFog"), Fog))
	{
		AExponentialHeightFog* Actor = FindOrSpawn<AExponentialHeightFog>(World, TEXT("LilaHeightFog"));
		if (Actor && Actor->GetComponent())
		{
			Actor->Modify();
			Actor->GetComponent()->Modify();
			ApplyFog(Actor, Actor->GetComponent(), *Fog);
			Out->SetStringField(TEXT("heightFog"), Actor->GetActorLabel());
		}
	}
	return MakeShared<FJsonValueObject>(Out);
}

int32 VersionOf(const UMaterialFunction* Function)
{
	for (UMaterialExpression* E : Function->GetExpressions())
	{
		if (const UMaterialExpressionCustom* C = Cast<UMaterialExpressionCustom>(E); C && C->Description.StartsWith(VersionTag))
		{
			return FCString::Atoi(*C->Description.RightChop(FCString::Strlen(VersionTag)));
		}
	}
	return 0;
}

UMaterialExpressionCustom* CustomNode(LilaLink::FFunctionGraph& G, const TCHAR* Code, TArray<TPair<const TCHAR*, UMaterialExpression*>> Wires, int32 X)
{
	auto* Custom = G.New<UMaterialExpressionCustom>(X);
	Custom->Code = Code;
	Custom->OutputType = CMOT_Float3;
	Custom->Inputs.Reset();
	for (const TPair<const TCHAR*, UMaterialExpression*>& W : Wires)
	{
		Custom->Inputs.AddDefaulted_GetRef().InputName = W.Key;
	}
	for (const TPair<const TCHAR*, UMaterialExpression*>& W : Wires)
	{
		UMaterialEditingLibrary::ConnectMaterialExpressions(W.Value, TEXT(""), Custom, W.Key);
	}
	return Custom;
}

/**
 * MF_LilaVolume: Lila's volume material in Unreal's Volume domain terms. Density (a grid's value, 1 for a uniform fog) times
 * DensityScale (Lila's volumeDensity per cm) scales scattering and absorption into extinction; albedo is the scattering share.
 */
void BuildVolumeFunction(UMaterialFunction* Function)
{
	LilaLink::FFunctionGraph G(Function, TEXT("Lila Volume: a volume material from Lila (density, scatter, absorption and emission colours) as the "
											  "Albedo, Extinction and Emissive a Volume-domain material takes, for Heterogeneous Volumes and volumetric fog."));
	UMaterialExpression* Density = G.ScalarInput(TEXT("Density"), 1.0f, TEXT("Volume"), TEXT("The grid's value here: a Sparse Volume Texture sample, or 1."));
	UMaterialExpression* Scale = G.ScalarInput(TEXT("DensityScale"), 0.01f, TEXT("Volume"), TEXT("Extinction per cm at a value of 1 (Lila's volumeDensity per metre / 100)."));
	UMaterialExpression* Scatter = G.VectorInput(TEXT("ScatterColor"), FLinearColor(0.8f, 0.8f, 0.8f), TEXT("Volume"), TEXT("Light scattered, per unit of density."));
	UMaterialExpression* Absorb = G.VectorInput(TEXT("AbsorptionColor"), FLinearColor::Black, TEXT("Volume"), TEXT("Light absorbed, per unit of density."));
	UMaterialExpression* Emission = G.VectorInput(TEXT("EmissionColor"), FLinearColor::Black, TEXT("Volume"), TEXT("Light given off, per unit of density."));
	UMaterialExpressionCustom* Albedo = CustomNode(G, TEXT("return S / max(S + A, 1e-5);"), {{TEXT("S"), Scatter}, {TEXT("A"), Absorb}}, -400);
	Albedo->Description = FString::Printf(TEXT("%s%d"), VersionTag, Version);
	UMaterialExpressionCustom* Extinction = CustomNode(G, TEXT("return max(D, 0.0) * K * (S + A);"),
		{{TEXT("D"), Density}, {TEXT("K"), Scale}, {TEXT("S"), Scatter}, {TEXT("A"), Absorb}}, -400);
	UMaterialExpressionCustom* Emissive = CustomNode(G, TEXT("return max(D, 0.0) * K * E;"), {{TEXT("D"), Density}, {TEXT("K"), Scale}, {TEXT("E"), Emission}}, -400);
	G.Output(Albedo, TEXT("Albedo"), TEXT("To Base Color."));
	G.Output(Extinction, TEXT("Extinction"), TEXT("To Subsurface Color (a Volume-domain material's extinction)."));
	G.Output(Emissive, TEXT("Emissive"), TEXT("To Emissive Color."));
	G.Finish();
}

/**
 * M_LilaVolume: Volume domain, a Sparse Volume Texture parameter's first attribute as the density, the colours as
 * parameters. The parameter needs a texture to compile; Default is the first one Lila sent.
 */
void BuildVolumeMaterial(UMaterial* Material, UMaterialFunction* Function, USparseVolumeTexture* Default, TArray<FString>& OutWarnings)
{
	Material->MaterialDomain = MD_Volume;
	Material->SetUsageByFlag(MATUSAGE_HeterogeneousVolumes, true);
	Material->BlendMode = BLEND_Additive;
	Material->SetShadingModel(MSM_Unlit);
	UMaterialExpressionMaterialFunctionCall* Call = LilaLink::ResetToFunctionCall(Material, Function);
	if (!LilaLink::ConnectFunctionOutputs(Call, {{TEXT("Albedo"), MP_BaseColor}, {TEXT("Extinction"), MP_SubsurfaceColor}, {TEXT("Emissive"), MP_EmissiveColor}}))
	{
		OutWarnings.Add(TEXT("Could not wire MF_LilaVolume's outputs into M_LilaVolume"));
	}
	auto* Grid = Cast<UMaterialExpressionSparseVolumeTextureSampleParameter>(
		UMaterialEditingLibrary::CreateMaterialExpression(Material, UMaterialExpressionSparseVolumeTextureSampleParameter::StaticClass(), -900, -200));
	Grid->ParameterName = TEXT("Density");
	Grid->SparseVolumeTexture = Default;
	auto* R = Cast<UMaterialExpressionComponentMask>(UMaterialEditingLibrary::CreateMaterialExpression(Material, UMaterialExpressionComponentMask::StaticClass(), -600, -200));
	R->R = true;
	R->G = R->B = R->A = false;
	R->Input.Connect(0, Grid);
	UMaterialEditingLibrary::ConnectMaterialExpressions(R, TEXT(""), Call, TEXT("Density"));
	auto Scalar = [&](const TCHAR* Name, float Value, int32 Y) {
		auto* P = Cast<UMaterialExpressionScalarParameter>(UMaterialEditingLibrary::CreateMaterialExpression(Material, UMaterialExpressionScalarParameter::StaticClass(), -600, Y));
		P->ParameterName = Name;
		P->DefaultValue = Value;
		UMaterialEditingLibrary::ConnectMaterialExpressions(P, TEXT(""), Call, Name);
	};
	auto Vector = [&](const TCHAR* Name, const FLinearColor& Value, int32 Y) {
		auto* P = Cast<UMaterialExpressionVectorParameter>(UMaterialEditingLibrary::CreateMaterialExpression(Material, UMaterialExpressionVectorParameter::StaticClass(), -600, Y));
		P->ParameterName = Name;
		P->DefaultValue = Value;
		UMaterialEditingLibrary::ConnectMaterialExpressions(P, TEXT(""), Call, Name);
	};
	Scalar(TEXT("DensityScale"), 0.01f, -50);
	Vector(TEXT("ScatterColor"), FLinearColor(0.8f, 0.8f, 0.8f), 50);
	Vector(TEXT("AbsorptionColor"), FLinearColor::Black, 200);
	Vector(TEXT("EmissionColor"), FLinearColor::Black, 350);
	Material->PostEditChange();
	Material->MarkPackageDirty();
}

/** MF_LilaVolume and M_LilaVolume, built when missing or older; Default becomes the master's texture when it has none. */
LilaLink::FMaterialPair VolumeMaterials(bool bRegenerate, USparseVolumeTexture* Default, TArray<FString>& OutCreated, TArray<FString>& OutWarnings)
{
	FString Error;
	LilaLink::FMaterialPair Pair = LilaLink::FindOrCreateMaterialPair(LilaLink::MastersFolderPath(), FunctionName, MaterialName, Error);
	if (!Pair.Material)
	{
		OutWarnings.Add(Error);
		return Pair;
	}
	USparseVolumeTexture* Current = nullptr;
	for (UMaterialExpression* E : Pair.Material->GetExpressions())
	{
		if (const auto* P = Cast<UMaterialExpressionSparseVolumeTextureSampleParameter>(E))
		{
			Current = P->SparseVolumeTexture;
		}
	}
	if (Pair.bCreated || bRegenerate || VersionOf(Pair.Function) < Version || (!Current && Default))
	{
		BuildVolumeFunction(Pair.Function);
		BuildVolumeMaterial(Pair.Material, Pair.Function, Current ? Current : Default, OutWarnings);
		OutCreated.AddUnique(Pair.Function->GetPathName());
		OutCreated.AddUnique(Pair.Material->GetPathName());
	}
	return Pair;
}

TSharedPtr<FJsonValue> Strings(const TArray<FString>& List)
{
	TArray<TSharedPtr<FJsonValue>> Out;
	for (const FString& S : List)
	{
		Out.Add(MakeShared<FJsonValueString>(S));
	}
	return MakeShared<FJsonValueArray>(Out);
}

TSharedPtr<FJsonValue> VolumeMaterialsMethod(const TSharedPtr<FJsonObject>& Params, FString& OutError)
{
	TRACE_CPUPROFILER_EVENT_SCOPE(LilaLinkSky::VolumeMaterials);

	bool bRegenerate = false;
	Params->TryGetBoolField(TEXT("regenerate"), bRegenerate);
	TArray<FString> Created, Warnings;
	const LilaLink::FMaterialPair Pair = VolumeMaterials(bRegenerate, nullptr, Created, Warnings);
	TArray<FString> SaveErrors;
	LilaLink::SaveAssetPaths(Created, SaveErrors);
	Warnings.Append(SaveErrors);
	TSharedRef<FJsonObject> Out = MakeShared<FJsonObject>();
	Out->SetStringField(TEXT("function"), Pair.Function ? Pair.Function->GetPathName() : FString());
	Out->SetStringField(TEXT("material"), Pair.Material ? Pair.Material->GetPathName() : FString());
	Out->SetField(TEXT("created"), Strings(Created));
	Out->SetField(TEXT("warnings"), Strings(Warnings));
	return MakeShared<FJsonValueObject>(Out);
}

TSharedPtr<FJsonValue> PlaceVolume(const TSharedPtr<FJsonObject>& Params, FString& OutError)
{
	TRACE_CPUPROFILER_EVENT_SCOPE(LilaLinkSky::PlaceVolume);

	FString File, Folder, Name, Label;
	if (!Params->TryGetStringField(TEXT("file"), File) || !Params->TryGetStringField(TEXT("name"), Name))
	{
		OutError = TEXT("placeVolume: give file and name");
		return nullptr;
	}
	Params->TryGetStringField(TEXT("folder"), Folder);
	if (Folder.IsEmpty())
	{
		Folder = TEXT("/Game/Lila/Volumes");
	}
	Label = Name;
	Params->TryGetStringField(TEXT("label"), Label);
	UWorld* World = SkyWorld();
	if (!World)
	{
		OutError = TEXT("placeVolume: no level is open");
		return nullptr;
	}
	TArray<FString> Created, Warnings;
	// Automated, the OpenVDB importer takes the file's first grid with its default settings and shows no dialog.
	UAssetImportTask* Task = NewObject<UAssetImportTask>();
	Task->Filename = File;
	Task->DestinationPath = Folder;
	Task->DestinationName = TEXT("SVT_") + Name;
	Task->bAutomated = true;
	Task->bReplaceExisting = true;
	Task->bSave = false;
	FAssetToolsModule::GetModule().Get().ImportAssetTasks({Task});
	USparseVolumeTexture* Texture = nullptr;
	for (UObject* Object : Task->GetObjects())
	{
		if (USparseVolumeTexture* T = Cast<USparseVolumeTexture>(Object))
		{
			Texture = T;
		}
	}
	if (!Texture)
	{
		OutError = TEXT("placeVolume: Unreal made no Sparse Volume Texture from ") + File + TEXT("; see the editor log");
		return nullptr;
	}
	Created.AddUnique(Texture->GetPathName());
	const LilaLink::FMaterialPair Pair = VolumeMaterials(false, Texture, Created, Warnings);
	if (!Pair.Material)
	{
		OutError = TEXT("placeVolume: no M_LilaVolume: ") + FString::Join(Warnings, TEXT("; "));
		return nullptr;
	}

	const FString InstanceName = TEXT("MI_") + Name;
	UMaterialInstanceConstant* Instance = LoadObject<UMaterialInstanceConstant>(nullptr, *(Folder + TEXT("/") + InstanceName + TEXT(".") + InstanceName), nullptr, LOAD_NoWarn | LOAD_Quiet);
	if (!Instance)
	{
		UMaterialInstanceConstantFactoryNew* Factory = NewObject<UMaterialInstanceConstantFactoryNew>(GetTransientPackage());
		Factory->InitialParent = Pair.Material;
		Instance = Cast<UMaterialInstanceConstant>(FAssetToolsModule::GetModule().Get().CreateAsset(InstanceName, Folder, UMaterialInstanceConstant::StaticClass(), Factory));
	}
	if (!Instance)
	{
		OutError = TEXT("placeVolume: could not create ") + InstanceName;
		return nullptr;
	}
	Instance->SetSparseVolumeTextureParameterValueEditorOnly(FMaterialParameterInfo(TEXT("Density")), Texture);
	Instance->SetScalarParameterValueEditorOnly(FMaterialParameterInfo(TEXT("DensityScale")), float(Num(Params, TEXT("densityScale"), 0.01)));
	Instance->SetVectorParameterValueEditorOnly(FMaterialParameterInfo(TEXT("ScatterColor")), Color(Params, TEXT("scatterColor"), FLinearColor(0.8f, 0.8f, 0.8f)));
	Instance->SetVectorParameterValueEditorOnly(FMaterialParameterInfo(TEXT("AbsorptionColor")), Color(Params, TEXT("absorptionColor"), FLinearColor::Black));
	Instance->SetVectorParameterValueEditorOnly(FMaterialParameterInfo(TEXT("EmissionColor")), Color(Params, TEXT("emissionColor"), FLinearColor::Black));
	Instance->PostEditChange();
	Instance->MarkPackageDirty();
	Created.AddUnique(Instance->GetPathName());

	const FScopedTransaction Transaction(LOCTEXT("PlaceVolume", "Lila Volume"));
	AHeterogeneousVolume* Actor = nullptr;
	for (TActorIterator<AHeterogeneousVolume> It(World); It; ++It)
	{
		if (It->GetActorLabel() == Label)
		{
			Actor = *It;
		}
	}
	if (!Actor)
	{
		Actor = World->SpawnActor<AHeterogeneousVolume>();
		Actor->SetActorLabel(Label);
	}
	Actor->Modify();
	// The voxel axes, mirrored as Lila to Unreal is, make the transform: one unit a voxel, centred on the centre voxel.
	const TArray<TSharedPtr<FJsonValue>>* Axes = nullptr;
	FMatrix M = FMatrix::Identity;
	if (Params->TryGetArrayField(TEXT("axes"), Axes) && Axes->Num() == 3)
	{
		for (int32 I = 0; I < 3; ++I)
		{
			const TArray<TSharedPtr<FJsonValue>>& A = (*Axes)[I]->AsArray();
			M.SetAxis(I, FVector(A[0]->AsNumber(), A[1]->AsNumber(), A[2]->AsNumber()));
		}
	}
	M.SetOrigin(Vec(Params, TEXT("centre")));
	Actor->SetActorTransform(FTransform(M));
	if (UHeterogeneousVolumeComponent* C = Actor->FindComponentByClass<UHeterogeneousVolumeComponent>())
	{
		C->Modify();
		const TArray<TSharedPtr<FJsonValue>>* Res = nullptr;
		if (Params->TryGetArrayField(TEXT("resolution"), Res) && Res->Num() == 3)
		{
			C->SetVolumeResolution(FIntVector(int32((*Res)[0]->AsNumber()), int32((*Res)[1]->AsNumber()), int32((*Res)[2]->AsNumber())));
		}
		C->bPivotAtCentroid = true;
		C->SetMaterial(0, Instance);
		C->MarkRenderStateDirty();
	}
	else
	{
		Warnings.Add(TEXT("The HeterogeneousVolume has no volume component; its material is not set"));
	}
	TArray<FString> SaveErrors;
	LilaLink::SaveAssetPaths(Created, SaveErrors);
	Warnings.Append(SaveErrors);
	for (const FString& W : Warnings)
	{
		UE_LOG(LogLilaLink, Warning, TEXT("placeVolume: %s"), *W);
	}
	TSharedRef<FJsonObject> Out = MakeShared<FJsonObject>();
	Out->SetStringField(TEXT("texture"), Texture->GetPathName());
	Out->SetStringField(TEXT("material"), Instance->GetPathName());
	Out->SetStringField(TEXT("actor"), Actor->GetActorLabel());
	Out->SetField(TEXT("warnings"), Strings(Warnings));
	return MakeShared<FJsonValueObject>(Out);
}
} // namespace

void AddSkyMethods(TMap<FString, FLilaLinkMethod>& Methods)
{
	Methods.Add(TEXT("setSky"), LilaLink::Sync(&SetSky));
	Methods.Add(TEXT("placeVolume"), LilaLink::Sync(&PlaceVolume));
	Methods.Add(TEXT("volumeMaterials"), LilaLink::Sync(&VolumeMaterialsMethod));
}
} // namespace LilaLink

#undef LOCTEXT_NAMESPACE
