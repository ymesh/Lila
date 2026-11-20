#include "LilaLinkIvy.h"

#include "LilaLinkLog.h"
#include "LilaLinkMaterialFunction.h"
#include "LilaLinkMaterials.h"

#include "AssetRegistry/AssetRegistryModule.h"
#include "AssetRegistry/IAssetRegistry.h"
#include "AssetToolsModule.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Engine/StaticMesh.h"
#include "Engine/Texture.h"
#include "Factories/MaterialInstanceConstantFactoryNew.h"
#include "IAssetTools.h"
#include "MaterialEditingLibrary.h"
#include "Materials/Material.h"
#include "Materials/MaterialExpressionAppendVector.h"
#include "Materials/MaterialExpressionComponentMask.h"
#include "Materials/MaterialExpressionConstant.h"
#include "Materials/MaterialExpressionCustom.h"
#include "Materials/MaterialExpressionDivide.h"
#include "Materials/MaterialExpressionFmod.h"
#include "Materials/MaterialExpressionMaterialFunctionCall.h"
#include "Materials/MaterialExpressionMultiply.h"
#include "Materials/MaterialExpressionScalarParameter.h"
#include "Materials/MaterialExpressionTextureCoordinate.h"
#include "Materials/MaterialExpressionTextureSampleParameter2D.h"
#include "Materials/MaterialExpressionTime.h"
#include "Materials/MaterialExpressionVectorParameter.h"
#include "Materials/MaterialExpressionVertexColor.h"
#include "Materials/MaterialExpressionWorldPosition.h"
#include "Materials/MaterialFunction.h"
#include "Materials/MaterialInstanceConstant.h"
#include "ProfilingDebugging/CpuProfilerTrace.h"
#include "UObject/Package.h"

namespace
{
constexpr int32 WindVersion = 1;
const TCHAR* WindFunctionName = TEXT("MF_LilaFoliageWind");
const TCHAR* IvyMasterName = TEXT("M_LilaIvy");
const TCHAR* WindVersionTag = TEXT("Lila Foliage Wind v");

TSharedPtr<FJsonValue> IvyObj(const TSharedRef<FJsonObject>& O) { return MakeShared<FJsonValueObject>(O); }
TSharedPtr<FJsonValue> IvyStr(const FString& S) { return MakeShared<FJsonValueString>(S); }

int32 WindVersionOf(const UMaterialFunction* Function)
{
	for (UMaterialExpression* E : Function->GetExpressions())
	{
		if (const UMaterialExpressionCustom* C = Cast<UMaterialExpressionCustom>(E); C && C->Description.StartsWith(WindVersionTag))
		{
			return FCString::Atoi(*C->Description.RightChop(FCString::Strlen(WindVersionTag)));
		}
	}
	return 0;
}

// Sway along the wind, a slower gust over it, and a quick flutter on leaves; R scales all of it so stems on the wall stay put.
const TCHAR* WindCode = TEXT(R"(
float sway = saturate(VColor.r) * Strength;
float phase = VColor.g * 6.2831853 + dot(WorldTile.xy, float2(0.37, 0.23));
float t = Time * Speed;
float3 dir = normalize(float3(Direction.xy, 0.0) + float3(1e-4, 0.0, 0.0));
float main = sin(t + phase) * 0.6 + sin(t * 0.37 + phase * 0.5) * 0.4;
float flutter = sin(t * 3.7 + phase * 7.0) * Flutter * VColor.b;
return dir * (main * Distance * sway) + float3(0.0, 0.0, flutter * Distance * 0.35 * sway);
)");

/** MF_LilaFoliageWind: World Position Offset from the wind colours. */
void BuildWindFunction(UMaterialFunction* Function)
{
	TRACE_CPUPROFILER_EVENT_SCOPE(LilaLinkIvy::BuildWindFunction);

	LilaLink::FFunctionGraph G(Function, TEXT("Lila Foliage Wind: sways foliage by its vertex colour (R how far a vertex sways, G its phase, "
											   "B 1 on leaves, which also flutter), as Lila writes it on ivy and other growth. Connect "
											   "WorldPositionOffset to the material's World Position Offset."));
	TMap<FString, UMaterialExpression*> In;
	In.Add(TEXT("Strength"), G.ScalarInput(TEXT("WindStrength"), 1.0f, TEXT("Wind"), TEXT("Overall sway, 0 for none.")));
	In.Add(TEXT("Speed"), G.ScalarInput(TEXT("WindSpeed"), 1.5f, TEXT("Wind"), TEXT("How fast it sways.")));
	In.Add(TEXT("Distance"), G.ScalarInput(TEXT("WindDistance"), 4.0f, TEXT("Wind"), TEXT("Farthest a fully swaying vertex moves, cm.")));
	In.Add(TEXT("Flutter"), G.ScalarInput(TEXT("LeafFlutter"), 0.5f, TEXT("Wind"), TEXT("Quick leaf flutter on top of the sway.")));
	In.Add(TEXT("Direction"), G.VectorInput(TEXT("WindDirection"), FLinearColor(1.0f, 0.3f, 0.0f), TEXT("Wind"), TEXT("Wind direction in X and Y.")));
	{
		UMaterialExpression* Color = G.Make(UMaterialExpressionVertexColor::StaticClass(), -1300);
		auto* Rgba = G.New<UMaterialExpressionAppendVector>(-1200);
		Rgba->A.Connect(0, Color);
		Rgba->B.Connect(4, Color);
		In.Add(TEXT("VColor"), G.Input(TEXT("WindColor"), FunctionInput_Vector4, Rgba, TEXT("Sway (R), phase (G), leaf (B); the vertex colour when unconnected.")));
	}
	In.Add(TEXT("Time"), G.Make(UMaterialExpressionTime::StaticClass(), -1200));
	{
		// World position is large-world (double) and a Custom node takes floats: divide and wrap it with nodes first.
		UMaterialExpression* World = G.Make(UMaterialExpressionWorldPosition::StaticClass(), -1600);
		auto* Divide = G.New<UMaterialExpressionDivide>(-1400);
		UMaterialEditingLibrary::ConnectMaterialExpressions(World, TEXT(""), Divide, TEXT("A"));
		auto* Scale = G.New<UMaterialExpressionConstant>(-1500);
		Scale->R = 100.0f;
		UMaterialEditingLibrary::ConnectMaterialExpressions(Scale, TEXT(""), Divide, TEXT("B"));
		auto* Wrap = G.New<UMaterialExpressionFmod>(-1300);
		UMaterialEditingLibrary::ConnectMaterialExpressions(Divide, TEXT(""), Wrap, TEXT("A"));
		auto* Period = G.New<UMaterialExpressionConstant>(-1400);
		Period->R = 256.0f;
		UMaterialEditingLibrary::ConnectMaterialExpressions(Period, TEXT(""), Wrap, TEXT("B"));
		In.Add(TEXT("WorldTile"), Wrap);
	}

	auto* Custom = G.New<UMaterialExpressionCustom>(-400);
	Custom->Description = FString::Printf(TEXT("%s%d"), WindVersionTag, WindVersion);
	Custom->Code = WindCode;
	Custom->OutputType = CMOT_Float3;
	Custom->Inputs.Reset();
	const TCHAR* Names[] = {TEXT("VColor"), TEXT("Time"), TEXT("WorldTile"), TEXT("Strength"), TEXT("Speed"), TEXT("Distance"), TEXT("Flutter"), TEXT("Direction")};
	for (const TCHAR* Name : Names)
	{
		Custom->Inputs.AddDefaulted_GetRef().InputName = Name;
	}
	for (const TCHAR* Name : Names)
	{
		UMaterialEditingLibrary::ConnectMaterialExpressions(In[Name], TEXT(""), Custom, Name);
	}
	G.Output(Custom, TEXT("WorldPositionOffset"), TEXT("Connect to World Position Offset."));
	G.Finish();
}

/** M_LilaIvy: masked two-sided foliage with Tint times BaseColorMap, its alpha as the mask, swayed by MF_LilaFoliageWind. */
void BuildIvyMaster(UMaterial* Material, UMaterialFunction* Function, TArray<FString>& OutWarnings)
{
	TRACE_CPUPROFILER_EVENT_SCOPE(LilaLinkIvy::BuildIvyMaster);

	UMaterialExpressionMaterialFunctionCall* Call = LilaLink::ResetToFunctionCall(Material, Function);
	if (!LilaLink::ConnectFunctionOutputs(Call, {{TEXT("WorldPositionOffset"), MP_WorldPositionOffset}}))
	{
		OutWarnings.Add(FString::Printf(TEXT("Could not wire the outputs of %s into %s"), *Function->GetName(), *Material->GetName()));
	}
	auto Param = [&](const TCHAR* Name, float Value, int32 Y) {
		auto* P = Cast<UMaterialExpressionScalarParameter>(UMaterialEditingLibrary::CreateMaterialExpression(Material, UMaterialExpressionScalarParameter::StaticClass(), -700, Y));
		P->ParameterName = Name;
		P->DefaultValue = Value;
		P->Group = TEXT("Surface");
		return P;
	};
	auto Color = [&](const TCHAR* Name, const FLinearColor& Value, int32 Y) {
		auto* P = Cast<UMaterialExpressionVectorParameter>(UMaterialEditingLibrary::CreateMaterialExpression(Material, UMaterialExpressionVectorParameter::StaticClass(), -900, Y));
		P->ParameterName = Name;
		P->DefaultValue = Value;
		P->Group = TEXT("Surface");
		return P;
	};
	auto* Map = Cast<UMaterialExpressionTextureSampleParameter2D>(
		UMaterialEditingLibrary::CreateMaterialExpression(Material, UMaterialExpressionTextureSampleParameter2D::StaticClass(), -900, 200));
	Map->ParameterName = TEXT("BaseColorMap");
	Map->Texture = LoadObject<UTexture>(nullptr, TEXT("/Engine/EngineResources/WhiteSquareTexture.WhiteSquareTexture"));
	Map->Group = TEXT("Surface");
	UMaterialExpressionVectorParameter* Tint = Color(TEXT("Tint"), FLinearColor(0.12f, 0.26f, 0.05f), 0);
	auto* Multiply = Cast<UMaterialExpressionMultiply>(UMaterialEditingLibrary::CreateMaterialExpression(Material, UMaterialExpressionMultiply::StaticClass(), -500, 100));
	UMaterialEditingLibrary::ConnectMaterialExpressions(Tint, TEXT(""), Multiply, TEXT("A"));
	UMaterialEditingLibrary::ConnectMaterialExpressions(Map, TEXT("RGB"), Multiply, TEXT("B"));
	UMaterialEditingLibrary::ConnectMaterialProperty(Multiply, TEXT(""), MP_BaseColor);
	UMaterialEditingLibrary::ConnectMaterialProperty(Map, TEXT("A"), MP_OpacityMask);
	UMaterialEditingLibrary::ConnectMaterialProperty(Param(TEXT("Roughness"), 0.6f, 400), TEXT(""), MP_Roughness);
	UMaterialEditingLibrary::ConnectMaterialProperty(Color(TEXT("SubsurfaceColor"), FLinearColor(0.2f, 0.35f, 0.05f), 500), TEXT(""), MP_SubsurfaceColor);
	Material->SetShadingModel(MSM_TwoSidedFoliage);
	Material->BlendMode = BLEND_Masked;
	Material->TwoSided = true;
	Material->SetMaterialUsage(MATUSAGE_StaticLighting);
	Material->SetMaterialUsage(MATUSAGE_InstancedStaticMeshes);
	Material->SetMaterialUsage(MATUSAGE_Nanite);
	Material->PostEditChange();
	Material->MarkPackageDirty();
	UMaterialEditingLibrary::RecompileMaterial(Material);
}

UMaterial* IvyMaster(bool bRegenerate, bool& bOutBuilt, TArray<FString>& OutCreated, TArray<FString>& OutWarnings)
{
	TRACE_CPUPROFILER_EVENT_SCOPE(LilaLinkIvy::IvyMaster);

	FString Error;
	const LilaLink::FMaterialPair Pair = LilaLink::FindOrCreateMaterialPair(LilaLink::MastersFolderPath(), WindFunctionName, IvyMasterName, Error);
	if (!Pair.Material)
	{
		OutWarnings.Add(Error);
		return nullptr;
	}
	bOutBuilt = Pair.bCreated || bRegenerate;
	if (bOutBuilt)
	{
		BuildWindFunction(Pair.Function);
		BuildIvyMaster(Pair.Material, Pair.Function, OutWarnings);
		OutCreated.AddUnique(Pair.Function->GetPathName());
		OutCreated.AddUnique(Pair.Material->GetPathName());
	}
	else if (const int32 Version = WindVersionOf(Pair.Function); Version < WindVersion)
	{
		OutWarnings.Add(FString::Printf(TEXT("%s is version %d, older than this plugin's %d; it is kept as edited (regenerate rebuilds it)"),
			WindFunctionName, Version, WindVersion));
	}
	return Pair.Material;
}

void SetIvyParameters(UMaterialInstanceConstant* Instance, const TSharedPtr<FJsonObject>& In, const FString& Folder, TArray<FString>& OutCreated,
	TArray<FString>& OutWarnings)
{
	const TSharedPtr<FJsonObject>* Scalars = nullptr;
	if (In->TryGetObjectField(TEXT("scalars"), Scalars))
	{
		for (const auto& [Name, Value] : (*Scalars)->Values)
		{
			UMaterialEditingLibrary::SetMaterialInstanceScalarParameterValue(Instance, FName(*Name), float(Value->AsNumber()));
		}
	}
	const TSharedPtr<FJsonObject>* Vectors = nullptr;
	if (In->TryGetObjectField(TEXT("vectors"), Vectors))
	{
		for (const auto& [Name, Value] : (*Vectors)->Values)
		{
			const TArray<TSharedPtr<FJsonValue>>& C = Value->AsArray();
			if (C.Num() >= 3)
			{
				UMaterialEditingLibrary::SetMaterialInstanceVectorParameterValue(
					Instance, FName(*Name), FLinearColor(float(C[0]->AsNumber()), float(C[1]->AsNumber()), float(C[2]->AsNumber())));
			}
		}
	}
	const TSharedPtr<FJsonObject>* Files = nullptr;
	if (In->TryGetObjectField(TEXT("files"), Files))
	{
		for (const auto& [Name, Value] : (*Files)->Values)
		{
			const TSharedPtr<FJsonObject> F = Value->AsObject();
			if (F.IsValid())
			{
				if (UTexture* Texture = LilaLink::ImportTextureFile(F->GetStringField(TEXT("file")), Folder, true, false, false, OutCreated, OutWarnings))
				{
					UMaterialEditingLibrary::SetMaterialInstanceTextureParameterValue(Instance, FName(*Name), Texture);
				}
			}
		}
	}
}

TSharedPtr<FJsonValue> IvyMaterials(const TSharedPtr<FJsonObject>& Params, FString& OutError)
{
	TRACE_CPUPROFILER_EVENT_SCOPE(LilaLinkIvy::IvyMaterials);

	FString Folder;
	const TArray<TSharedPtr<FJsonValue>>* Instances = nullptr;
	if (!Params->TryGetStringField(TEXT("folder"), Folder) || !Params->TryGetArrayField(TEXT("instances"), Instances))
	{
		OutError = TEXT("ivyMaterials: give folder and instances");
		return nullptr;
	}
	TArray<FString> Created, Warnings;
	bool bRegenerate = false, bBuilt = false;
	Params->TryGetBoolField(TEXT("regenerate"), bRegenerate);
	UMaterialInterface* Parent = nullptr;
	FString ParentPath;
	if (Params->TryGetStringField(TEXT("parent"), ParentPath) && !ParentPath.IsEmpty())
	{
		Parent = Cast<UMaterialInterface>(FSoftObjectPath(ParentPath).TryLoad());
		if (!Parent)
		{
			OutError = FString::Printf(TEXT("ivyMaterials: no material %s"), *ParentPath);
			return nullptr;
		}
	}
	else
	{
		Parent = IvyMaster(bRegenerate, bBuilt, Created, Warnings);
		if (!Parent)
		{
			OutError = FString::Printf(TEXT("ivyMaterials: %s"), *FString::Join(Warnings, TEXT("; ")));
			return nullptr;
		}
	}

	TArray<UStaticMesh*> Meshes;
	const TArray<TSharedPtr<FJsonValue>>* MeshPaths = nullptr;
	if (Params->TryGetArrayField(TEXT("meshes"), MeshPaths))
	{
		for (const TSharedPtr<FJsonValue>& V : *MeshPaths)
		{
			if (UStaticMesh* Mesh = Cast<UStaticMesh>(FSoftObjectPath(V->AsString()).TryLoad()))
			{
				Meshes.AddUnique(Mesh);
			}
			else
			{
				Warnings.Add(FString::Printf(TEXT("no static mesh %s"), *V->AsString()));
			}
		}
	}

	TArray<TSharedPtr<FJsonValue>> Made, Assigned;
	TSet<UStaticMesh*> Changed;
	for (const TSharedPtr<FJsonValue>& V : *Instances)
	{
		const TSharedPtr<FJsonObject> In = V->AsObject();
		FString Name, Slot;
		if (!In.IsValid() || !In->TryGetStringField(TEXT("name"), Name) || !In->TryGetStringField(TEXT("slot"), Slot))
		{
			Warnings.Add(TEXT("an instance without name or slot was skipped"));
			continue;
		}
		FAssetData Data = IAssetRegistry::GetChecked().GetAssetByObjectPath(FSoftObjectPath(Folder / Name + TEXT(".") + Name));
		UMaterialInstanceConstant* Instance = Data.IsValid() ? Cast<UMaterialInstanceConstant>(Data.GetAsset()) : nullptr;
		if (!Instance)
		{
			UMaterialInstanceConstantFactoryNew* Factory = NewObject<UMaterialInstanceConstantFactoryNew>(GetTransientPackage());
			Factory->InitialParent = Parent;
			Instance = Cast<UMaterialInstanceConstant>(
				FAssetToolsModule::GetModule().Get().CreateAsset(Name, Folder, UMaterialInstanceConstant::StaticClass(), Factory));
			if (!Instance)
			{
				Warnings.Add(FString::Printf(TEXT("Could not create %s in %s"), *Name, *Folder));
				continue;
			}
			Created.AddUnique(Instance->GetPathName());
		}
		Instance->Modify();
		if (Instance->Parent != Parent)
		{
			UMaterialEditingLibrary::SetMaterialInstanceParent(Instance, Parent);
		}
		SetIvyParameters(Instance, In, Folder, Created, Warnings);
		UMaterialEditingLibrary::UpdateMaterialInstance(Instance);
		Instance->MarkPackageDirty();
		Made.Add(IvyStr(Instance->GetPathName()));

		const FName SlotName(*Slot);
		for (UStaticMesh* Mesh : Meshes)
		{
			for (FStaticMaterial& M : Mesh->GetStaticMaterials())
			{
				if (M.MaterialSlotName != SlotName || M.MaterialInterface == Instance)
				{
					continue;
				}
				if (!Changed.Contains(Mesh))
				{
					Mesh->Modify();
					Changed.Add(Mesh);
				}
				M.MaterialInterface = Instance;
				TSharedRef<FJsonObject> A = MakeShared<FJsonObject>();
				A->SetStringField(TEXT("mesh"), Mesh->GetPathName());
				A->SetStringField(TEXT("slot"), Slot);
				A->SetStringField(TEXT("material"), Instance->GetPathName());
				Assigned.Add(IvyObj(A));
			}
		}
	}
	for (UStaticMesh* Mesh : Changed)
	{
		Mesh->PostEditChange();
		Mesh->MarkPackageDirty();
	}

	TSharedRef<FJsonObject> Out = MakeShared<FJsonObject>();
	Out->SetStringField(TEXT("master"), Parent->GetPathName());
	Out->SetBoolField(TEXT("built"), bBuilt);
	Out->SetArrayField(TEXT("instances"), Made);
	Out->SetArrayField(TEXT("assigned"), Assigned);
	TArray<TSharedPtr<FJsonValue>> CreatedJson, WarningsJson;
	for (const FString& C : Created)
	{
		CreatedJson.Add(IvyStr(C));
	}
	for (const FString& W : Warnings)
	{
		WarningsJson.Add(IvyStr(W));
		UE_LOG(LogLilaLink, Warning, TEXT("ivyMaterials: %s"), *W);
	}
	Out->SetArrayField(TEXT("created"), CreatedJson);
	Out->SetArrayField(TEXT("warnings"), WarningsJson);
	return IvyObj(Out);
}
} // namespace

void LilaLink::AddIvyMethods(TMap<FString, FLilaLinkMethod>& Methods)
{
	Methods.Add(TEXT("ivyMaterials"), LilaLink::Sync(&IvyMaterials));
}
