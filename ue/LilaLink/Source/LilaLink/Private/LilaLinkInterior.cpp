#include "LilaLinkInterior.h"

#include "LilaLinkLog.h"
#include "LilaLinkMaterialFunction.h"
#include "LilaLinkMaterials.h"

#include "AssetRegistry/AssetRegistryModule.h"
#include "AssetRegistry/IAssetRegistry.h"
#include "AssetToolsModule.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Engine/Texture.h"
#include "Factories/MaterialInstanceConstantFactoryNew.h"
#include "IAssetTools.h"
#include "MaterialEditingLibrary.h"
#include "Materials/Material.h"
#include "Materials/MaterialExpressionCameraVectorWS.h"
#include "Materials/MaterialExpressionCustom.h"
#include "Materials/MaterialExpressionMaterialFunctionCall.h"
#include "Materials/MaterialExpressionTextureCoordinate.h"
#include "Materials/MaterialExpressionTransform.h"
#include "Materials/MaterialFunction.h"
#include "Materials/MaterialInstanceConstant.h"
#include "ProfilingDebugging/CpuProfilerTrace.h"
#include "UObject/Package.h"

namespace
{
constexpr int32 InteriorVersion = 1;
const TCHAR* InteriorMasterName = TEXT("M_LilaInterior");
const TCHAR* InteriorFunctionName = TEXT("MF_LilaInterior");
const TCHAR* InteriorVersionTag = TEXT("Lila Interior v");

TSharedPtr<FJsonValue> InteriorObj(const TSharedRef<FJsonObject>& O) { return MakeShared<FJsonValueObject>(O); }
TSharedPtr<FJsonValue> InteriorStr(const FString& S) { return MakeShared<FJsonValueString>(S); }

template <class T> T* InteriorAssetAt(const FString& Folder, const FString& Name)
{
	FAssetData Data = IAssetRegistry::GetChecked().GetAssetByObjectPath(FSoftObjectPath(Folder / Name + TEXT(".") + Name));
	return Data.IsValid() ? Cast<T>(Data.GetAsset()) : nullptr;
}

/**
 * The room's uv for a pixel of the card: a ray from the camera through the card into a box room whose back wall is the
 * middle (1 - Depth) of the image, its side walls, floor and ceiling the image's edges in perspective. View is the tangent
 * vector from the surface to the camera.
 */
const TCHAR* InteriorHlsl = TEXT(R"(
float depth = clamp(Depth, 0.0, 0.95);
float2 cell = frac(UV * max(Tiling, 1e-3));
float depthScale = 1.0 / (1.0 - depth) - 1.0;
float3 v = -normalize(View);
v.z *= -depthScale;
float3 pos = float3(cell * 2.0 - 1.0, -1.0);
float3 inv = 1.0 / (abs(v) < 1e-5 ? sign(v + 1e-6) * 1e-5 : v);
float3 k = abs(inv) - pos * inv;
float kMin = min(min(k.x, k.y), k.z);
pos += kMin * v;
float t = pos.z * 0.5 + 0.5;
float realZ = saturate(t) / max(depthScale, 1e-4) + 1.0;
t = (1.0 - 1.0 / realZ) * (depthScale + 1.0);
float2 room = pos.xy * lerp(1.0, 1.0 - depth, t);
return float3(room * 0.5 + 0.5, 0.0);
)");

/** MF_LilaInterior: see LilaLinkInterior.h. */
void BuildInteriorFunction(UMaterialFunction* Function)
{
	TRACE_CPUPROFILER_EVENT_SCOPE(LilaLinkInterior::BuildInteriorFunction);

	LilaLink::FFunctionGraph G(Function, TEXT("Lila Interior: a room behind a window from one image, with parallax, for flat interior cards."));
	UTexture* Black = LoadObject<UTexture>(nullptr, TEXT("/Engine/EngineResources/Black.Black"));
	UMaterialExpression* Room = G.TextureInput(TEXT("InteriorRoomMap"), Black, SAMPLERTYPE_Color, TEXT("Interior"),
		TEXT("Image of a room in one-point perspective, its back wall in the middle."));
	UMaterialExpression* Depth = G.ScalarInput(TEXT("InteriorDepth"), 0.5f, TEXT("Interior"), TEXT("0 to 0.95: how deep the room reads; 1 minus it is the back wall's share of the image."));
	UMaterialExpression* Tiling = G.ScalarInput(TEXT("InteriorTiling"), 1.0f, TEXT("Interior"), TEXT("Rooms across the card's UVs."));
	UMaterialExpression* Tint = G.VectorInput(TEXT("InteriorTint"), FLinearColor(1.0f, 1.0f, 1.0f), TEXT("Interior"), TEXT("Multiplies the room's colour."));
	UMaterialExpression* Glow = G.ScalarInput(TEXT("InteriorEmissive"), 0.4f, TEXT("Interior"), TEXT("How much the room lights itself, so it reads at night."));
	auto* UV0 = G.New<UMaterialExpressionTextureCoordinate>(-1500);
	UV0->CoordinateIndex = 0;
	UMaterialExpression* Uvs = G.Input(TEXT("UVs"), FunctionInput_Vector2, UV0, TEXT("The card's UVs; UV channel 0 when unconnected."));

	auto* Camera = G.New<UMaterialExpressionCameraVectorWS>(-1200);
	auto* View = G.New<UMaterialExpressionTransform>(-900);
	View->TransformSourceType = TRANSFORMSOURCE_World;
	View->TransformType = TRANSFORM_Tangent;
	View->Input.Connect(0, Camera);

	const FString Version = FString::Printf(TEXT("%s%d"), InteriorVersionTag, InteriorVersion);
	auto* RoomUv = G.New<UMaterialExpressionCustom>(-600);
	RoomUv->Description = Version + TEXT(" room uv");
	RoomUv->Code = InteriorHlsl;
	RoomUv->OutputType = CMOT_Float3;
	RoomUv->Inputs.Reset();
	const TPair<const TCHAR*, UMaterialExpression*> UvWires[] = {{TEXT("UV"), Uvs}, {TEXT("View"), View}, {TEXT("Depth"), Depth}, {TEXT("Tiling"), Tiling}};
	for (const TPair<const TCHAR*, UMaterialExpression*>& W : UvWires)
	{
		RoomUv->Inputs.AddDefaulted_GetRef().InputName = W.Key;
	}
	for (const TPair<const TCHAR*, UMaterialExpression*>& W : UvWires)
	{
		UMaterialEditingLibrary::ConnectMaterialExpressions(W.Value, TEXT(""), RoomUv, W.Key);
	}

	auto* Colour = G.New<UMaterialExpressionCustom>(-300);
	Colour->Description = Version + TEXT(" colour");
	Colour->Code = TEXT("return Texture2DSample(RoomTex, RoomTexSampler, RoomUV.xy).rgb * Tint;");
	Colour->OutputType = CMOT_Float3;
	Colour->Inputs.Reset();
	const TPair<const TCHAR*, UMaterialExpression*> ColourWires[] = {{TEXT("RoomTex"), Room}, {TEXT("RoomUV"), RoomUv}, {TEXT("Tint"), Tint}};
	for (const TPair<const TCHAR*, UMaterialExpression*>& W : ColourWires)
	{
		Colour->Inputs.AddDefaulted_GetRef().InputName = W.Key;
	}
	for (const TPair<const TCHAR*, UMaterialExpression*>& W : ColourWires)
	{
		UMaterialEditingLibrary::ConnectMaterialExpressions(W.Value, TEXT(""), Colour, W.Key);
	}

	auto* Emissive = G.New<UMaterialExpressionCustom>(-300);
	Emissive->Description = Version + TEXT(" emissive");
	Emissive->Code = TEXT("return Colour * Glow;");
	Emissive->OutputType = CMOT_Float3;
	Emissive->Inputs.Reset();
	Emissive->Inputs.AddDefaulted_GetRef().InputName = TEXT("Colour");
	Emissive->Inputs.AddDefaulted_GetRef().InputName = TEXT("Glow");
	UMaterialEditingLibrary::ConnectMaterialExpressions(Colour, TEXT(""), Emissive, TEXT("Colour"));
	UMaterialEditingLibrary::ConnectMaterialExpressions(Glow, TEXT(""), Emissive, TEXT("Glow"));

	auto* Rough = G.New<UMaterialExpressionCustom>(-300);
	Rough->Description = Version + TEXT(" roughness");
	Rough->Code = TEXT("return 1.0;");
	Rough->OutputType = CMOT_Float1;
	Rough->Inputs.Reset();

	G.Output(Colour, TEXT("BaseColor"), TEXT("The room's colour."));
	G.Output(Emissive, TEXT("Emissive"), TEXT("The room lighting itself."));
	G.Output(Rough, TEXT("Roughness"), TEXT("Fully rough: the glass in front carries the reflections."));
	G.Output(RoomUv, TEXT("RoomUV"), TEXT("Where in the image each pixel looks, for your own maps."));
	G.Finish();
}

void BuildInteriorMaster(UMaterial* Material, UMaterialFunction* Function, TArray<FString>& OutWarnings)
{
	TRACE_CPUPROFILER_EVENT_SCOPE(LilaLinkInterior::BuildInteriorMaster);

	UMaterialExpressionMaterialFunctionCall* Call = LilaLink::ResetToFunctionCall(Material, Function);
	if (!LilaLink::ConnectFunctionOutputs(Call, {{TEXT("BaseColor"), MP_BaseColor}, {TEXT("Emissive"), MP_EmissiveColor}, {TEXT("Roughness"), MP_Roughness}}))
	{
		OutWarnings.Add(FString::Printf(TEXT("Could not wire the outputs of %s into %s"), *Function->GetName(), *Material->GetName()));
	}
	Material->PostEditChange();
	Material->MarkPackageDirty();
}

void SetInteriorParameters(UMaterialInstanceConstant* Instance, const TSharedPtr<FJsonObject>& In, TArray<FString>& OutWarnings)
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
	const TSharedPtr<FJsonObject>* Textures = nullptr;
	if (In->TryGetObjectField(TEXT("textures"), Textures))
	{
		for (const auto& [Name, Value] : (*Textures)->Values)
		{
			if (UTexture* Texture = Cast<UTexture>(FSoftObjectPath(Value->AsString()).TryLoad()))
			{
				UMaterialEditingLibrary::SetMaterialInstanceTextureParameterValue(Instance, FName(*Name), Texture);
			}
			else
			{
				OutWarnings.Add(FString::Printf(TEXT("%s: no texture %s for %s"), *Instance->GetName(), *Value->AsString(), *Name));
			}
		}
	}
}

/** weatheringInteriors: see LilaLinkInterior.h. */
TSharedPtr<FJsonValue> WeatheringInteriors(const TSharedPtr<FJsonObject>& Params, FString& OutError)
{
	TRACE_CPUPROFILER_EVENT_SCOPE(LilaLinkInterior::WeatheringInteriors);

	FString Folder;
	const TArray<TSharedPtr<FJsonValue>>* Interiors = nullptr;
	if (!Params->TryGetStringField(TEXT("folder"), Folder) || !Params->TryGetArrayField(TEXT("interiors"), Interiors))
	{
		OutError = TEXT("weatheringInteriors: give folder and interiors");
		return nullptr;
	}
	TArray<FString> Created, Warnings;
	bool bRegenerate = false, bBuilt = false;
	Params->TryGetBoolField(TEXT("regenerate"), bRegenerate);
	UMaterial* Master = LilaLink::InteriorMaster(bRegenerate, bBuilt, Created, Warnings);
	if (!Master)
	{
		OutError = FString::Printf(TEXT("weatheringInteriors: %s"), *FString::Join(Warnings, TEXT("; ")));
		return nullptr;
	}
	TArray<TSharedPtr<FJsonValue>> Instances;
	for (const TSharedPtr<FJsonValue>& V : *Interiors)
	{
		const TSharedPtr<FJsonObject> I = V.IsValid() ? V->AsObject() : nullptr;
		FString Name;
		if (!I.IsValid() || !I->TryGetStringField(TEXT("name"), Name))
		{
			Warnings.Add(TEXT("an interior without a name was skipped"));
			continue;
		}
		UMaterialInstanceConstant* Instance = InteriorAssetAt<UMaterialInstanceConstant>(Folder, Name);
		if (!Instance)
		{
			UMaterialInstanceConstantFactoryNew* Factory = NewObject<UMaterialInstanceConstantFactoryNew>(GetTransientPackage());
			Factory->InitialParent = Master;
			Instance = Cast<UMaterialInstanceConstant>(FAssetToolsModule::GetModule().Get().CreateAsset(Name, Folder, UMaterialInstanceConstant::StaticClass(), Factory));
			if (!Instance)
			{
				Warnings.Add(FString::Printf(TEXT("Could not create %s in %s"), *Name, *Folder));
				continue;
			}
			Created.AddUnique(Instance->GetPathName());
		}
		Instance->Modify();
		if (Instance->Parent != Master)
		{
			UMaterialEditingLibrary::SetMaterialInstanceParent(Instance, Master);
		}
		SetInteriorParameters(Instance, I, Warnings);
		UMaterialEditingLibrary::UpdateMaterialInstance(Instance);
		Instance->MarkPackageDirty();
		Instances.Add(InteriorStr(Instance->GetPathName()));
	}

	TSharedRef<FJsonObject> Out = MakeShared<FJsonObject>();
	Out->SetStringField(TEXT("master"), Master->GetPathName());
	Out->SetBoolField(TEXT("built"), bBuilt);
	Out->SetArrayField(TEXT("instances"), Instances);
	TArray<TSharedPtr<FJsonValue>> CreatedJson, WarningsJson;
	for (const FString& C : Created)
	{
		CreatedJson.Add(InteriorStr(C));
	}
	for (const FString& W : Warnings)
	{
		WarningsJson.Add(InteriorStr(W));
		UE_LOG(LogLilaLink, Warning, TEXT("weatheringInteriors: %s"), *W);
	}
	Out->SetArrayField(TEXT("created"), CreatedJson);
	Out->SetArrayField(TEXT("warnings"), WarningsJson);
	return InteriorObj(Out);
}
} // namespace

UMaterial* LilaLink::InteriorMaster(bool bRegenerate, bool& bOutBuilt, TArray<FString>& OutCreated, TArray<FString>& OutWarnings)
{
	TRACE_CPUPROFILER_EVENT_SCOPE(LilaLinkInterior::InteriorMaster);

	FString Error;
	const FMaterialPair Pair = FindOrCreateMaterialPair(MastersFolderPath(), InteriorFunctionName, InteriorMasterName, Error);
	if (!Pair.Material)
	{
		OutWarnings.Add(Error);
		return nullptr;
	}
	bOutBuilt = Pair.bCreated || bRegenerate;
	if (bOutBuilt)
	{
		BuildInteriorFunction(Pair.Function);
		BuildInteriorMaster(Pair.Material, Pair.Function, OutWarnings);
		OutCreated.AddUnique(Pair.Function->GetPathName());
		OutCreated.AddUnique(Pair.Material->GetPathName());
	}
	for (UMaterialExpression* E : Pair.Function->GetExpressions())
	{
		if (const UMaterialExpressionCustom* C = Cast<UMaterialExpressionCustom>(E); C && C->Description.StartsWith(InteriorVersionTag))
		{
			if (FCString::Atoi(*C->Description.RightChop(FCString::Strlen(InteriorVersionTag))) < InteriorVersion)
			{
				OutWarnings.Add(FString::Printf(TEXT("%s is older than this plugin's; it is kept as edited (regenerate rebuilds it)"), InteriorMasterName));
			}
			break;
		}
	}
	return Pair.Material;
}

void LilaLink::AddInteriorMethods(TMap<FString, FLilaLinkMethod>& Methods)
{
	Methods.Add(TEXT("weatheringInteriors"), LilaLink::Sync(&WeatheringInteriors));
}
