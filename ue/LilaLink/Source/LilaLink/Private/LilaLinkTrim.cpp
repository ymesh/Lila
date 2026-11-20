#include "LilaLinkTrim.h"

#include "LilaLinkHeightNormal.h"
#include "LilaLinkLog.h"
#include "LilaLinkMaterialFunction.h"
#include "LilaLinkMaterials.h"

#include "AssetRegistry/AssetRegistryModule.h"
#include "AssetRegistry/IAssetRegistry.h"
#include "AssetToolsModule.h"
#include "Components/DecalComponent.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Editor.h"
#include "Engine/DecalActor.h"
#include "Engine/StaticMesh.h"
#include "Engine/World.h"
#include "EngineUtils.h"
#include "Engine/Texture.h"
#include "Engine/Texture2D.h"
#include "Factories/MaterialFactoryNew.h"
#include "Factories/MaterialInstanceConstantFactoryNew.h"
#include "IAssetTools.h"
#include "MaterialEditingLibrary.h"
#include "Materials/Material.h"
#include "Materials/MaterialExpressionConstant.h"
#include "Materials/MaterialExpressionCustom.h"
#include "Materials/MaterialExpressionScalarParameter.h"
#include "Materials/MaterialExpressionStaticSwitchParameter.h"
#include "Materials/MaterialExpressionTextureCoordinate.h"
#include "Materials/MaterialExpressionTextureObjectParameter.h"
#include "Materials/MaterialExpressionTextureProperty.h"
#include "Materials/MaterialExpressionVectorParameter.h"
#include "Materials/MaterialExpressionVertexColor.h"
#include "Materials/MaterialExpressionMaterialFunctionCall.h"
#include "Materials/MaterialFunction.h"
#include "Materials/MaterialInstanceConstant.h"
#include "ProfilingDebugging/CpuProfilerTrace.h"
#include "ScopedTransaction.h"
#include "UObject/Package.h"

namespace
{
/** Raised when the graph changes; a master older than this is kept (artists own it) and reported, until regenerate. */
constexpr int32 MasterVersion = 3;
const TCHAR* MasterName = TEXT("M_LilaTrim");
const TCHAR* FunctionName = TEXT("MF_LilaTrim");
const TCHAR* VersionTag = TEXT("Lila Trim v");

TSharedPtr<FJsonValue> Obj(const TSharedRef<FJsonObject>& O) { return MakeShared<FJsonValueObject>(O); }
TSharedPtr<FJsonValue> Str(const FString& S) { return MakeShared<FJsonValueString>(S); }

template <class T> T* AssetAt(const FString& Folder, const FString& Name)
{
	FAssetData Data = IAssetRegistry::GetChecked().GetAssetByObjectPath(FSoftObjectPath(Folder / Name + TEXT(".") + Name));
	return Data.IsValid() ? Cast<T>(Data.GetAsset()) : nullptr;
}

/** Edge and cavity masks from the baked curvature (0.5 flat, above convex) and cavity maps, shared by every output. */
const TCHAR* MasksCode = TEXT(R"(
float curv = Texture2DSample(SheetCurvatureMap, SheetCurvatureMapSampler, SheetUV).r;
float cavity = Texture2DSample(SheetCavityMap, SheetCavityMapSampler, SheetUV).r;
float edge = saturate((curv - 0.5 - EdgeThreshold) / max(EdgeSoftness, 1e-3)) * EdgeWear;
float dirt = saturate(saturate((0.5 - curv) * 2.0) + (1.0 - cavity)) * CavityStrength;
float blend = UseVertexBlend > 0.5 ? saturate(VColor.r * VertexBlend) : 0.0;
float2 detailUV = SheetUV * float2(SheetWidthCm, SheetHeightCm) / max(DetailScaleCm, 1e-3);
)");

const TCHAR* ColorCode = TEXT(R"(
float3 col = Tint.rgb;
if (UseDetail > 0.5)
{
    float3 d = Texture2DSample(DetailBaseColorMap, DetailBaseColorMapSampler, detailUV).rgb;
    col *= lerp(float3(1, 1, 1), d * 2.0, DetailStrength);
}
col = lerp(col, SecondaryTint.rgb, blend);
col = lerp(col, EdgeTint.rgb, edge);
return lerp(col, col * DirtColor.rgb, dirt);
)");

const TCHAR* RoughnessCode = TEXT(R"(
float r = BaseRoughness;
if (UseDetail > 0.5)
    r += (Texture2DSample(DetailORMMap, DetailORMMapSampler, detailUV).g - 0.5) * DetailStrength;
r = lerp(r, SecondaryRoughness, blend);
r = lerp(r, EdgeRoughness, edge);
return saturate(r + dirt * DirtRoughness);
)");

const TCHAR* MetallicCode = TEXT(R"(
return saturate(lerp(lerp(Metallic, 0.0, blend), EdgeMetallic, edge));
)");

const TCHAR* OcclusionCode = TEXT(R"(
float ao = Texture2DSample(SheetAOMap, SheetAOMapSampler, SheetUV).r;
return lerp(1.0, ao, AOStrength);
)");

/**
 * The sheet's baked normal (BC5 keeps only xy), laid as detail on the height's own normal with UseHeightNormals, under the
 * tiling detail normal, RNM blended.
 */
FString NormalCode()
{
	return FString(TEXT(R"(
float2 xy = Texture2DSample(SheetNormalMap, SheetNormalMapSampler, SheetUV).xy * 2.0 - 1.0;
float3 nrm = float3(xy, sqrt(saturate(1.0 - dot(xy, xy))));
if (UseHeightNormals > 0.5)
{
)")) + LilaLink::HeightNormalHlsl(TEXT("SheetHeightMap"), TEXT("SheetUV"), TEXT("HeightTexel"),
			   TEXT("HeightDepthCm / max(float2(SheetWidthCm, SheetHeightCm), 1e-3)"), TEXT("nrm")) +
		   TEXT(R"(
    nrm = heightNormal;
}
if (UseDetail > 0.5)
{
    float2 dxy = (Texture2DSample(DetailNormalMap, DetailNormalMapSampler, detailUV).xy * 2.0 - 1.0) * DetailNormalStrength * (1.0 - edge);
    float3 d = float3(dxy, sqrt(saturate(1.0 - dot(dxy, dxy))));
    float3 n1 = nrm + float3(0, 0, 1);
    float3 n2 = d * float3(-1, -1, 1);
    nrm = n1 * dot(n1, n2) / n1.z - n2;
}
return normalize(nrm);
)");
}

/** A 4x4 linear texture of one grey level and compression: defaults whose sampler type matches the parameter's. */
UTexture* NeutralTexture(const TCHAR* Name, uint8 Level, TextureCompressionSettings Compression, TArray<FString>& OutCreated)
{
	TRACE_CPUPROFILER_EVENT_SCOPE(LilaLinkTrim::NeutralTexture);

	const FString Folder = LilaLink::MastersFolderPath();
	if (UTexture* Existing = AssetAt<UTexture>(Folder, Name))
	{
		return Existing;
	}
	UPackage* Package = CreatePackage(*(Folder / Name));
	UTexture2D* Texture = NewObject<UTexture2D>(Package, Name, RF_Public | RF_Standalone | RF_Transactional);
	TArray<uint8> Pixels;
	Pixels.Init(Level, 4 * 4 * 4);
	for (int32 K = 3; K < Pixels.Num(); K += 4)
	{
		Pixels[K] = 255;
	}
	Texture->Source.Init(4, 4, 1, 1, TSF_BGRA8, Pixels.GetData());
	Texture->SRGB = false;
	Texture->CompressionSettings = Compression;
	Texture->PostEditChange();
	FAssetRegistryModule::AssetCreated(Texture);
	Package->MarkPackageDirty();
	OutCreated.AddUnique(Texture->GetPathName());
	return Texture;
}

/** The version a master's function was built at, from its Custom nodes' description; 0 when none says. */
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

/**
 * MF_LilaTrim: the sheet's baked maps through UVs (uv0 unless connected) turned into paint, outputs BaseColor, Roughness,
 * Metallic, AmbientOcclusion and tangent-space Normal. Every parameter input defaults to the parameter of its name; the
 * UseDetail, UseVertexBlend, UseParallax and UsePaintedMaps static switches stay parameters inside the function.
 */
void BuildFunction(UMaterialFunction* Function, TArray<FString>& OutCreated)
{
	TRACE_CPUPROFILER_EVENT_SCOPE(LilaLinkTrim::BuildFunction);

	LilaLink::FFunctionGraph G(Function, TEXT("Lila Trim: a trim sheet's baked maps turned into worn edges, cavity dirt, a tiling detail layer and a vertex blend."));
	TMap<FString, UMaterialExpression*> Inputs;
	auto Scalar = [&](const TCHAR* Param, float Value, const TCHAR* Group, const TCHAR* Description) {
		Inputs.Add(Param, G.ScalarInput(Param, Value, Group, Description));
	};
	auto Vector = [&](const TCHAR* Param, const FLinearColor& Value, const TCHAR* Group, const TCHAR* Description) {
		Inputs.Add(Param, G.VectorInput(Param, Value, Group, Description));
	};
	auto Texture = [&](const TCHAR* Param, UTexture* Default, EMaterialSamplerType Type, const TCHAR* Group, const TCHAR* Description) {
		Inputs.Add(Param, G.TextureInput(Param, Default, Type, Group, Description));
	};
	// A static switch between 1 and 0: an instance that leaves it off folds the branch it gates out of the shader.
	auto Switch = [&](const TCHAR* Param, bool bDefault, const TCHAR* Group) {
		auto* S = G.New<UMaterialExpressionStaticSwitchParameter>(-1200);
		S->ParameterName = Param;
		S->DefaultValue = bDefault;
		S->Group = Group;
		auto* On = G.New<UMaterialExpressionConstant>(-1400);
		On->R = 1.0f;
		auto* Off = G.New<UMaterialExpressionConstant>(-1400);
		Off->R = 0.0f;
		UMaterialEditingLibrary::ConnectMaterialExpressions(On, TEXT(""), S, TEXT("True"));
		UMaterialEditingLibrary::ConnectMaterialExpressions(Off, TEXT(""), S, TEXT("False"));
		Inputs.Add(Param, S);
		return S;
	};

	UTexture* Flat = LoadObject<UTexture>(nullptr, TEXT("/Engine/EngineMaterials/FlatNormal.FlatNormal"));
	UTexture* Grey = NeutralTexture(TEXT("T_LilaTrimNeutral"), 128, TC_Masks, OutCreated);
	UTexture* WhiteMask = NeutralTexture(TEXT("T_LilaTrimWhite"), 255, TC_Masks, OutCreated);
	UTexture* Height = NeutralTexture(TEXT("T_LilaTrimHeight"), 128, TC_Grayscale, OutCreated);
	Texture(TEXT("SheetNormalMap"), Flat, SAMPLERTYPE_Normal, TEXT("Sheet"), TEXT("The sheet's baked normal."));
	Texture(TEXT("SheetAOMap"), WhiteMask, SAMPLERTYPE_Masks, TEXT("Sheet"), TEXT("The sheet's baked ambient occlusion."));
	Texture(TEXT("SheetCurvatureMap"), Grey, SAMPLERTYPE_Masks, TEXT("Sheet"), TEXT("The sheet's baked curvature, 0.5 flat."));
	Texture(TEXT("SheetCavityMap"), WhiteMask, SAMPLERTYPE_Masks, TEXT("Sheet"), TEXT("The sheet's baked cavity."));
	Texture(TEXT("SheetHeightMap"), Height, SAMPLERTYPE_LinearGrayscale, TEXT("Sheet"), TEXT("The sheet's baked height, for parallax."));
	Texture(TEXT("DetailBaseColorMap"), Grey, SAMPLERTYPE_Color, TEXT("Detail"), TEXT("Tiling detail colour, 0.5 neutral."));
	Texture(TEXT("DetailNormalMap"), Flat, SAMPLERTYPE_Normal, TEXT("Detail"), TEXT("Tiling detail normal."));
	Texture(TEXT("DetailORMMap"), Grey, SAMPLERTYPE_Masks, TEXT("Detail"), TEXT("Tiling detail ORM; roughness in G, 0.5 neutral."));
	Texture(TEXT("SheetBaseColorMap"), Grey, SAMPLERTYPE_Color, TEXT("Painted"), TEXT("The sheet's painted base colour, with UsePaintedMaps."));
	Texture(TEXT("SheetORMMap"), Grey, SAMPLERTYPE_Masks, TEXT("Painted"), TEXT("The sheet's painted ORM, with UsePaintedMaps."));

	Scalar(TEXT("SheetWidthCm"), 400.0f, TEXT("Sheet"), TEXT("The sheet's width in the world, cm."));
	Scalar(TEXT("SheetHeightCm"), 400.0f, TEXT("Sheet"), TEXT("The sheet's height in the world, cm."));
	Scalar(TEXT("AOStrength"), 1.0f, TEXT("Sheet"), TEXT("How much of the baked AO shows."));
	Vector(TEXT("Tint"), FLinearColor(0.35f, 0.37f, 0.4f), TEXT("Paint"), TEXT("Paint colour."));
	Scalar(TEXT("BaseRoughness"), 0.55f, TEXT("Paint"), TEXT("Paint roughness."));
	Scalar(TEXT("Metallic"), 0.0f, TEXT("Paint"), TEXT("Paint metallic."));
	Vector(TEXT("EdgeTint"), FLinearColor(0.62f, 0.6f, 0.58f), TEXT("Edges"), TEXT("Colour of worn edges."));
	Scalar(TEXT("EdgeWear"), 1.0f, TEXT("Edges"), TEXT("Amount of edge wear."));
	Scalar(TEXT("EdgeThreshold"), 0.05f, TEXT("Edges"), TEXT("Curvature above flat where wear starts."));
	Scalar(TEXT("EdgeSoftness"), 0.15f, TEXT("Edges"), TEXT("Fade of the wear."));
	Scalar(TEXT("EdgeRoughness"), 0.3f, TEXT("Edges"), TEXT("Roughness of worn edges."));
	Scalar(TEXT("EdgeMetallic"), 1.0f, TEXT("Edges"), TEXT("Metallic of worn edges."));
	Vector(TEXT("DirtColor"), FLinearColor(0.45f, 0.4f, 0.33f), TEXT("Dirt"), TEXT("Multiplied in where dirt gathers."));
	Scalar(TEXT("CavityStrength"), 0.5f, TEXT("Dirt"), TEXT("Dirt amount in cavities."));
	Scalar(TEXT("DirtRoughness"), 0.2f, TEXT("Dirt"), TEXT("Roughness added by dirt."));
	Scalar(TEXT("DetailScaleCm"), 100.0f, TEXT("Detail"), TEXT("World size of one detail tile, cm."));
	Scalar(TEXT("DetailStrength"), 0.5f, TEXT("Detail"), TEXT("How much of the detail colour and roughness shows."));
	Scalar(TEXT("DetailNormalStrength"), 1.0f, TEXT("Detail"), TEXT("Scales the detail normal."));
	Vector(TEXT("SecondaryTint"), FLinearColor(0.3f, 0.25f, 0.2f), TEXT("Vertex Blend"), TEXT("Colour blended in by vertex colour red."));
	Scalar(TEXT("SecondaryRoughness"), 0.85f, TEXT("Vertex Blend"), TEXT("Roughness blended in by vertex colour red."));
	Scalar(TEXT("VertexBlend"), 1.0f, TEXT("Vertex Blend"), TEXT("Scales vertex colour red."));
	Scalar(TEXT("ParallaxHeight"), 0.02f, TEXT("Parallax"), TEXT("Parallax depth in UV units."));
	Scalar(TEXT("HeightDepthCm"), 5.0f, TEXT("Height Normal"), TEXT("cm from the sheet height's 0 to its 1, as Lila baked it."));
	Switch(TEXT("UseHeightNormals"), false, TEXT("Height Normal"));
	{
		auto* Texel = G.New<UMaterialExpressionTextureProperty>(-1200);
		Texel->Property = TMTM_TexelSize;
		Texel->TextureObject.Connect(0, Inputs[TEXT("SheetHeightMap")]);
		Inputs.Add(TEXT("HeightTexel"), Texel);
	}
	Switch(TEXT("UseDetail"), false, TEXT("Detail"));
	Switch(TEXT("UseVertexBlend"), false, TEXT("Vertex Blend"));
	Inputs.Add(TEXT("VColor"), G.Make(UMaterialExpressionVertexColor::StaticClass(), -1400));

	// The sheet uv: uv0, or offset along the tangent-space view by the baked height when parallax is on (Unreal's bump offset).
	{
		auto* UV0 = G.New<UMaterialExpressionTextureCoordinate>(-1700);
		UV0->CoordinateIndex = 0;
		UMaterialExpression* Uvs = G.Input(TEXT("UVs"), FunctionInput_Vector2, UV0, TEXT("The sheet UVs; UV channel 0 when unconnected."));
		auto* Offset = G.New<UMaterialExpressionCustom>(-1500);
		Offset->Description = TEXT("Sheet parallax");
		Offset->Code = TEXT("float h = Texture2DSample(SheetHeightMap, SheetHeightMapSampler, UV).r;\n"
							"float3 v = mul(Parameters.TangentToWorld, Parameters.CameraVector);\n"
							"return UV + v.xy * (h - 0.5) * ParallaxHeight;");
		Offset->OutputType = CMOT_Float2;
		Offset->Inputs.Reset();
		for (const TCHAR* Name : {TEXT("SheetHeightMap"), TEXT("UV"), TEXT("ParallaxHeight")})
		{
			Offset->Inputs.AddDefaulted_GetRef().InputName = Name;
		}
		UMaterialEditingLibrary::ConnectMaterialExpressions(Inputs[TEXT("SheetHeightMap")], TEXT(""), Offset, TEXT("SheetHeightMap"));
		UMaterialEditingLibrary::ConnectMaterialExpressions(Uvs, TEXT(""), Offset, TEXT("UV"));
		UMaterialEditingLibrary::ConnectMaterialExpressions(Inputs[TEXT("ParallaxHeight")], TEXT(""), Offset, TEXT("ParallaxHeight"));
		auto* Pick = G.New<UMaterialExpressionStaticSwitchParameter>(-1300);
		Pick->ParameterName = TEXT("UseParallax");
		Pick->DefaultValue = false;
		Pick->Group = TEXT("Parallax");
		UMaterialEditingLibrary::ConnectMaterialExpressions(Offset, TEXT(""), Pick, TEXT("True"));
		UMaterialEditingLibrary::ConnectMaterialExpressions(Uvs, TEXT(""), Pick, TEXT("False"));
		Inputs.Add(TEXT("SheetUV"), Pick);
	}

	const TArray<const TCHAR*> MaskInputs = {TEXT("SheetUV"), TEXT("SheetCurvatureMap"), TEXT("SheetCavityMap"), TEXT("EdgeThreshold"),
		TEXT("EdgeSoftness"), TEXT("EdgeWear"), TEXT("CavityStrength"), TEXT("UseVertexBlend"), TEXT("VColor"), TEXT("VertexBlend"),
		TEXT("SheetWidthCm"), TEXT("SheetHeightCm"), TEXT("DetailScaleCm")};
	auto With = [&](std::initializer_list<const TCHAR*> Extra) {
		TArray<const TCHAR*> All = MaskInputs;
		All.Append(Extra.begin(), int32(Extra.size()));
		return All;
	};
	auto CustomOf = [&](const FString& Description, const FString& Code, ECustomMaterialOutputType Type, const TArray<const TCHAR*>& Names) {
		auto* E = G.New<UMaterialExpressionCustom>(-400);
		E->Description = Description;
		E->Code = Code;
		E->OutputType = Type;
		E->Inputs.Reset();
		for (const TCHAR* Name : Names)
		{
			E->Inputs.AddDefaulted_GetRef().InputName = Name;
		}
		for (const TCHAR* Name : Names)
		{
			if (UMaterialExpression** From = Inputs.Find(Name))
			{
				UMaterialEditingLibrary::ConnectMaterialExpressions(*From, TEXT(""), E, Name);
			}
		}
		return E;
	};

	const FString Version = FString::Printf(TEXT("%s%d"), VersionTag, MasterVersion);
	auto* Color = CustomOf(Version + TEXT(" colour"), FString(MasksCode) + ColorCode, CMOT_Float3,
		With({TEXT("Tint"), TEXT("UseDetail"), TEXT("DetailBaseColorMap"), TEXT("DetailStrength"), TEXT("SecondaryTint"), TEXT("EdgeTint"), TEXT("DirtColor")}));
	auto* Roughness = CustomOf(Version + TEXT(" roughness"), FString(MasksCode) + RoughnessCode, CMOT_Float1,
		With({TEXT("BaseRoughness"), TEXT("UseDetail"), TEXT("DetailORMMap"), TEXT("DetailStrength"), TEXT("SecondaryRoughness"), TEXT("EdgeRoughness"),
			TEXT("DirtRoughness")}));
	auto* Metallic = CustomOf(Version + TEXT(" metallic"), FString(MasksCode) + MetallicCode, CMOT_Float1,
		With({TEXT("Metallic"), TEXT("EdgeMetallic")}));
	auto* Occlusion = CustomOf(Version + TEXT(" occlusion"), OcclusionCode, CMOT_Float1, {TEXT("SheetUV"), TEXT("SheetAOMap"), TEXT("AOStrength")});
	auto* Normal = CustomOf(Version + TEXT(" normal"), FString(MasksCode) + NormalCode(), CMOT_Float3,
		With({TEXT("SheetNormalMap"), TEXT("UseDetail"), TEXT("DetailNormalMap"), TEXT("DetailNormalStrength"), TEXT("UseHeightNormals"),
			TEXT("SheetHeightMap"), TEXT("HeightTexel"), TEXT("HeightDepthCm")}));

	// UsePaintedMaps: the sheet's texture set from Lila (base colour and ORM) stands in for the procedural paint.
	auto Painted = [&](UMaterialExpression* Procedural, const TCHAR* Map, const TCHAR* Code, ECustomMaterialOutputType Type) -> UMaterialExpression* {
		auto* Sample = CustomOf(Version + TEXT(" painted"), Code, Type, {TEXT("SheetUV"), Map});
		auto* Pick = G.New<UMaterialExpressionStaticSwitchParameter>(-200);
		Pick->ParameterName = TEXT("UsePaintedMaps");
		Pick->DefaultValue = false;
		Pick->Group = TEXT("Painted");
		UMaterialEditingLibrary::ConnectMaterialExpressions(Sample, TEXT(""), Pick, TEXT("True"));
		UMaterialEditingLibrary::ConnectMaterialExpressions(Procedural, TEXT(""), Pick, TEXT("False"));
		return Pick;
	};
	G.Output(Painted(Color, TEXT("SheetBaseColorMap"), TEXT("return Texture2DSample(SheetBaseColorMap, SheetBaseColorMapSampler, SheetUV).rgb;"), CMOT_Float3),
		TEXT("BaseColor"), TEXT("Base colour."));
	G.Output(Painted(Roughness, TEXT("SheetORMMap"), TEXT("return Texture2DSample(SheetORMMap, SheetORMMapSampler, SheetUV).g;"), CMOT_Float1),
		TEXT("Roughness"), TEXT("Roughness."));
	G.Output(Painted(Metallic, TEXT("SheetORMMap"), TEXT("return Texture2DSample(SheetORMMap, SheetORMMapSampler, SheetUV).b;"), CMOT_Float1),
		TEXT("Metallic"), TEXT("Metallic."));
	G.Output(Painted(Occlusion, TEXT("SheetORMMap"), TEXT("return Texture2DSample(SheetORMMap, SheetORMMapSampler, SheetUV).r;"), CMOT_Float1),
		TEXT("AmbientOcclusion"), TEXT("Ambient occlusion."));
	G.Output(Normal, TEXT("Normal"), TEXT("Tangent-space normal."));
	G.Finish();
}

/** M_LilaTrim: MF_LilaTrim's five outputs on the material's pins. */
void BuildMaster(UMaterial* Material, UMaterialFunction* Function, TArray<FString>& OutWarnings)
{
	TRACE_CPUPROFILER_EVENT_SCOPE(LilaLinkTrim::BuildMaster);

	UMaterialExpressionMaterialFunctionCall* Call = LilaLink::ResetToFunctionCall(Material, Function);
	if (!LilaLink::ConnectFunctionOutputs(Call, {{TEXT("BaseColor"), MP_BaseColor}, {TEXT("Roughness"), MP_Roughness}, {TEXT("Metallic"), MP_Metallic},
			{TEXT("AmbientOcclusion"), MP_AmbientOcclusion}, {TEXT("Normal"), MP_Normal}}))
	{
		OutWarnings.Add(FString::Printf(TEXT("Could not wire the outputs of %s into %s"), *Function->GetName(), *Material->GetName()));
	}
	Material->SetMaterialUsage(MATUSAGE_InstancedStaticMeshes);
	Material->SetMaterialUsage(MATUSAGE_Nanite);
	Material->PostEditChange();
	Material->MarkPackageDirty();
}

TArray<FString> Strings(const TSharedPtr<FJsonObject>& O, const TCHAR* Field)
{
	TArray<FString> Out;
	const TArray<TSharedPtr<FJsonValue>>* Values = nullptr;
	if (O->TryGetArrayField(Field, Values))
	{
		for (const TSharedPtr<FJsonValue>& V : *Values)
		{
			Out.Add(V->AsString());
		}
	}
	return Out;
}

void SetParameters(UMaterialInstanceConstant* Instance, const TSharedPtr<FJsonObject>& In, TArray<FString>& OutWarnings)
{
	TRACE_CPUPROFILER_EVENT_SCOPE(LilaLinkTrim::SetParameters);

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
	const TSharedPtr<FJsonObject>* Switches = nullptr;
	if (In->TryGetObjectField(TEXT("switches"), Switches))
	{
		for (const auto& [Name, Value] : (*Switches)->Values)
		{
			UMaterialEditingLibrary::SetMaterialInstanceStaticSwitchParameterValue(Instance, FName(*Name), Value->AsBool());
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

TSharedPtr<FJsonValue> TrimMaterials(const TSharedPtr<FJsonObject>& Params, FString& OutError)
{
	TRACE_CPUPROFILER_EVENT_SCOPE(LilaLinkTrim::TrimMaterials);

	FString Folder;
	const TArray<TSharedPtr<FJsonValue>>* Instances = nullptr;
	if (!Params->TryGetStringField(TEXT("folder"), Folder) || !Params->TryGetArrayField(TEXT("instances"), Instances))
	{
		OutError = TEXT("trimMaterials: give folder and instances");
		return nullptr;
	}
	TArray<FString> Created, Warnings;
	bool bRegenerate = false;
	Params->TryGetBoolField(TEXT("regenerate"), bRegenerate);
	bool bBuilt = false;
	int32 Version = 0;
	UMaterial* Master = LilaLink::TrimMaster(bRegenerate, bBuilt, Version, Created, Warnings);
	if (!Master)
	{
		OutError = FString::Printf(TEXT("trimMaterials: %s"), *FString::Join(Warnings, TEXT("; ")));
		return nullptr;
	}

	TArray<TSharedPtr<FJsonValue>> Made, Assigned;
	// Each palette's instance goes on the mesh slots named after its Lila material.
	TMap<FName, UMaterialInstanceConstant*> BySlot;
	for (const TSharedPtr<FJsonValue>& V : *Instances)
	{
		const TSharedPtr<FJsonObject> In = V->AsObject();
		FString Name;
		if (!In.IsValid() || !In->TryGetStringField(TEXT("name"), Name))
		{
			Warnings.Add(TEXT("an instance without a name was skipped"));
			continue;
		}
		UMaterialInstanceConstant* Instance = AssetAt<UMaterialInstanceConstant>(Folder, Name);
		if (!Instance)
		{
			UMaterialInstanceConstantFactoryNew* Factory = NewObject<UMaterialInstanceConstantFactoryNew>(GetTransientPackage());
			Factory->InitialParent = Master;
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
		if (Instance->Parent != Master)
		{
			UMaterialEditingLibrary::SetMaterialInstanceParent(Instance, Master);
		}
		SetParameters(Instance, In, Warnings);
		UMaterialEditingLibrary::UpdateMaterialInstance(Instance);
		Instance->MarkPackageDirty();
		Made.Add(Str(Instance->GetPathName()));
		FString Slot;
		if (In->TryGetStringField(TEXT("slot"), Slot) && !Slot.IsEmpty())
		{
			BySlot.Add(FName(*Slot), Instance);
		}
	}

	for (const FString& Path : Strings(Params, TEXT("meshes")))
	{
		if (BySlot.IsEmpty())
		{
			break;
		}
		UStaticMesh* Mesh = Cast<UStaticMesh>(FSoftObjectPath(Path).TryLoad());
		if (!Mesh)
		{
			Warnings.Add(FString::Printf(TEXT("no static mesh %s"), *Path));
			continue;
		}
		bool bChanged = false;
		for (FStaticMaterial& M : Mesh->GetStaticMaterials())
		{
			UMaterialInstanceConstant** Instance = BySlot.Find(M.MaterialSlotName);
			if (!Instance)
			{
				continue;
			}
			TSharedRef<FJsonObject> A = MakeShared<FJsonObject>();
			A->SetStringField(TEXT("mesh"), Mesh->GetPathName());
			A->SetStringField(TEXT("slot"), M.MaterialSlotName.ToString());
			A->SetStringField(TEXT("material"), (*Instance)->GetPathName());
			A->SetBoolField(TEXT("changed"), M.MaterialInterface != *Instance);
			Assigned.Add(Obj(A));
			if (M.MaterialInterface == *Instance)
			{
				continue;
			}
			if (!bChanged)
			{
				Mesh->Modify();
				bChanged = true;
			}
			M.MaterialInterface = *Instance;
		}
		if (bChanged)
		{
			Mesh->PostEditChange();
			Mesh->MarkPackageDirty();
		}
	}

	TSharedRef<FJsonObject> Out = MakeShared<FJsonObject>();
	Out->SetStringField(TEXT("master"), Master->GetPathName());
	Out->SetNumberField(TEXT("masterVersion"), Version);
	Out->SetBoolField(TEXT("built"), bBuilt);
	Out->SetArrayField(TEXT("instances"), Made);
	Out->SetArrayField(TEXT("assigned"), Assigned);
	TArray<TSharedPtr<FJsonValue>> CreatedJson, WarningsJson;
	for (const FString& C : Created)
	{
		CreatedJson.Add(Str(C));
	}
	for (const FString& W : Warnings)
	{
		WarningsJson.Add(Str(W));
		UE_LOG(LogLilaLink, Warning, TEXT("trimMaterials: %s"), *W);
	}
	Out->SetArrayField(TEXT("created"), CreatedJson);
	Out->SetArrayField(TEXT("warnings"), WarningsJson);
	return Obj(Out);
}
constexpr int32 DecalVersion = 1;
const TCHAR* DecalMasterName = TEXT("M_LilaDecal");
const TCHAR* DecalFunctionName = TEXT("MF_LilaDecal");
const TCHAR* DecalVersionTag = TEXT("Lila Decal v");
const FName DecalTag(TEXT("LilaDecal"));
const TCHAR* DecalNodePrefix = TEXT("LilaNode:");

/**
 * MF_LilaDecal: a unique decal's baked maps (base colour, tangent-space normal, opacity mask) through the decal's uvs, out
 * as BaseColor, Normal, Roughness, Metallic and Opacity. FlipU mirrors the uvs for projectors that read them reversed.
 */
void BuildDecalFunction(UMaterialFunction* Function, TArray<FString>& OutCreated)
{
	TRACE_CPUPROFILER_EVENT_SCOPE(LilaLinkTrim::BuildDecalFunction);

	LilaLink::FFunctionGraph G(Function, TEXT("Lila Decal: a unique decal baked or painted in Lila, its normal and colour masked by its opacity."));
	TMap<FString, UMaterialExpression*> Inputs;
	UTexture* Flat = LoadObject<UTexture>(nullptr, TEXT("/Engine/EngineMaterials/FlatNormal.FlatNormal"));
	UTexture* Grey = NeutralTexture(TEXT("T_LilaTrimNeutral"), 128, TC_Masks, OutCreated);
	UTexture* White = NeutralTexture(TEXT("T_LilaTrimWhite"), 255, TC_Masks, OutCreated);
	Inputs.Add(TEXT("DecalBaseColorMap"), G.TextureInput(TEXT("DecalBaseColorMap"), Grey, SAMPLERTYPE_Color, TEXT("Decal"), TEXT("Base colour.")));
	Inputs.Add(TEXT("DecalNormalMap"), G.TextureInput(TEXT("DecalNormalMap"), Flat, SAMPLERTYPE_Normal, TEXT("Decal"), TEXT("Tangent-space normal.")));
	Inputs.Add(TEXT("DecalOpacityMap"), G.TextureInput(TEXT("DecalOpacityMap"), White, SAMPLERTYPE_Masks, TEXT("Decal"), TEXT("Opacity mask in red.")));
	Inputs.Add(TEXT("Tint"), G.VectorInput(TEXT("Tint"), FLinearColor(1.0f, 1.0f, 1.0f), TEXT("Decal"), TEXT("Multiplies the base colour.")));
	Inputs.Add(TEXT("Roughness"), G.ScalarInput(TEXT("Roughness"), 0.7f, TEXT("Decal"), TEXT("Roughness.")));
	Inputs.Add(TEXT("Metallic"), G.ScalarInput(TEXT("Metallic"), 0.0f, TEXT("Decal"), TEXT("Metallic.")));
	Inputs.Add(TEXT("OpacityStrength"), G.ScalarInput(TEXT("OpacityStrength"), 1.0f, TEXT("Decal"), TEXT("Scales the opacity mask.")));
	Inputs.Add(TEXT("NormalStrength"), G.ScalarInput(TEXT("NormalStrength"), 1.0f, TEXT("Decal"), TEXT("Scales the normal's slope.")));
	Inputs.Add(TEXT("FlipU"), G.ScalarInput(TEXT("FlipU"), 0.0f, TEXT("Decal"), TEXT("1 mirrors the decal's U.")));
	auto* UV0 = G.New<UMaterialExpressionTextureCoordinate>(-1700);
	UV0->CoordinateIndex = 0;
	Inputs.Add(TEXT("UV"), G.Input(TEXT("UVs"), FunctionInput_Vector2, UV0, TEXT("The decal uvs; UV channel 0 when unconnected.")));

	const FString Version = FString::Printf(TEXT("%s%d"), DecalVersionTag, DecalVersion);
	const FString Uv = TEXT("float2 duv = float2(FlipU > 0.5 ? 1.0 - UV.x : UV.x, UV.y);\n");
	auto CustomOf = [&](const FString& Description, const FString& Code, ECustomMaterialOutputType Type, std::initializer_list<const TCHAR*> Names) {
		auto* E = G.New<UMaterialExpressionCustom>(-400);
		E->Description = Version + TEXT(" ") + Description;
		E->Code = Uv + Code;
		E->OutputType = Type;
		E->Inputs.Reset();
		for (const TCHAR* Name : Names)
		{
			E->Inputs.AddDefaulted_GetRef().InputName = Name;
		}
		for (const TCHAR* Name : Names)
		{
			UMaterialEditingLibrary::ConnectMaterialExpressions(Inputs[Name], TEXT(""), E, Name);
		}
		return E;
	};
	G.Output(CustomOf(TEXT("colour"), TEXT("return Texture2DSample(DecalBaseColorMap, DecalBaseColorMapSampler, duv).rgb * Tint;"), CMOT_Float3,
				 {TEXT("UV"), TEXT("FlipU"), TEXT("DecalBaseColorMap"), TEXT("Tint")}),
		TEXT("BaseColor"), TEXT("Base colour."));
	G.Output(CustomOf(TEXT("normal"),
				 TEXT("float2 xy = (Texture2DSample(DecalNormalMap, DecalNormalMapSampler, duv).xy * 2.0 - 1.0) * NormalStrength;\n"
					  "if (FlipU > 0.5) xy.x = -xy.x;\n"
					  "return normalize(float3(xy, sqrt(saturate(1.0 - dot(xy, xy)))));"),
				 CMOT_Float3, {TEXT("UV"), TEXT("FlipU"), TEXT("DecalNormalMap"), TEXT("NormalStrength")}),
		TEXT("Normal"), TEXT("Tangent-space normal."));
	G.Output(CustomOf(TEXT("roughness"), TEXT("return Roughness;"), CMOT_Float1, {TEXT("UV"), TEXT("FlipU"), TEXT("Roughness")}), TEXT("Roughness"),
		TEXT("Roughness."));
	G.Output(CustomOf(TEXT("metallic"), TEXT("return Metallic;"), CMOT_Float1, {TEXT("UV"), TEXT("FlipU"), TEXT("Metallic")}), TEXT("Metallic"),
		TEXT("Metallic."));
	G.Output(CustomOf(TEXT("opacity"), TEXT("return saturate(Texture2DSample(DecalOpacityMap, DecalOpacityMapSampler, duv).r * OpacityStrength);"),
				 CMOT_Float1, {TEXT("UV"), TEXT("FlipU"), TEXT("DecalOpacityMap"), TEXT("OpacityStrength")}),
		TEXT("Opacity"), TEXT("Opacity mask."));
	G.Finish();
}

/** M_LilaDecal: MF_LilaDecal on a deferred decal material's pins. */
void BuildDecalMaster(UMaterial* Material, UMaterialFunction* Function, TArray<FString>& OutWarnings)
{
	TRACE_CPUPROFILER_EVENT_SCOPE(LilaLinkTrim::BuildDecalMaster);

	Material->MaterialDomain = MD_DeferredDecal;
	Material->BlendMode = BLEND_Translucent;
	UMaterialExpressionMaterialFunctionCall* Call = LilaLink::ResetToFunctionCall(Material, Function);
	if (!LilaLink::ConnectFunctionOutputs(Call, {{TEXT("BaseColor"), MP_BaseColor}, {TEXT("Normal"), MP_Normal}, {TEXT("Roughness"), MP_Roughness},
			{TEXT("Metallic"), MP_Metallic}, {TEXT("Opacity"), MP_Opacity}}))
	{
		OutWarnings.Add(FString::Printf(TEXT("Could not wire the outputs of %s into %s"), *Function->GetName(), *Material->GetName()));
	}
	Material->PostEditChange();
	Material->MarkPackageDirty();
}

UMaterial* DecalMaster(bool bRegenerate, bool& bOutBuilt, TArray<FString>& OutCreated, TArray<FString>& OutWarnings)
{
	TRACE_CPUPROFILER_EVENT_SCOPE(LilaLinkTrim::DecalMaster);

	FString Error;
	const LilaLink::FMaterialPair Pair = LilaLink::FindOrCreateMaterialPair(LilaLink::MastersFolderPath(), DecalFunctionName, DecalMasterName, Error);
	if (!Pair.Material)
	{
		OutWarnings.Add(Error);
		return nullptr;
	}
	bOutBuilt = Pair.bCreated || bRegenerate;
	if (bOutBuilt)
	{
		BuildDecalFunction(Pair.Function, OutCreated);
		BuildDecalMaster(Pair.Material, Pair.Function, OutWarnings);
		OutCreated.AddUnique(Pair.Function->GetPathName());
		OutCreated.AddUnique(Pair.Material->GetPathName());
	}
	for (UMaterialExpression* E : Pair.Function->GetExpressions())
	{
		if (const UMaterialExpressionCustom* C = Cast<UMaterialExpressionCustom>(E); C && C->Description.StartsWith(DecalVersionTag))
		{
			if (FCString::Atoi(*C->Description.RightChop(FCString::Strlen(DecalVersionTag))) < DecalVersion)
			{
				OutWarnings.Add(FString::Printf(TEXT("%s is older than this plugin's; it is kept as edited (regenerate rebuilds it)"), DecalMasterName));
			}
			break;
		}
	}
	return Pair.Material;
}

FVector DecalVector(const TSharedPtr<FJsonObject>& P, const TCHAR* Field, const FVector& Fallback)
{
	const TArray<TSharedPtr<FJsonValue>>* V = nullptr;
	if (!P->TryGetArrayField(Field, V) || V->Num() < 3)
	{
		return Fallback;
	}
	return FVector((*V)[0]->AsNumber(), (*V)[1]->AsNumber(), (*V)[2]->AsNumber());
}

/**
 * trimDecals: the decal master and one instance per decal, then a decal actor per placement in the open level, keyed by
 * the Lila node so a resend moves it instead of adding another.
 */
TSharedPtr<FJsonValue> TrimDecals(const TSharedPtr<FJsonObject>& Params, FString& OutError)
{
	TRACE_CPUPROFILER_EVENT_SCOPE(LilaLinkTrim::TrimDecals);

	FString Folder;
	const TArray<TSharedPtr<FJsonValue>>* Decals = nullptr;
	if (!Params->TryGetStringField(TEXT("folder"), Folder) || !Params->TryGetArrayField(TEXT("decals"), Decals))
	{
		OutError = TEXT("trimDecals: give folder and decals");
		return nullptr;
	}
	UWorld* World = GEditor ? GEditor->GetEditorWorldContext().World() : nullptr;
	if (!World)
	{
		OutError = TEXT("trimDecals: no level is open");
		return nullptr;
	}
	TArray<FString> Created, Warnings;
	bool bRegenerate = false, bBuilt = false;
	Params->TryGetBoolField(TEXT("regenerate"), bRegenerate);
	UMaterial* Master = DecalMaster(bRegenerate, bBuilt, Created, Warnings);
	if (!Master)
	{
		OutError = FString::Printf(TEXT("trimDecals: %s"), *FString::Join(Warnings, TEXT("; ")));
		return nullptr;
	}

	FScopedTransaction Transaction(NSLOCTEXT("LilaLinkTrim", "PlaceDecals", "Lila: Place Decals"));
	TMap<FString, ADecalActor*> ByNode;
	for (TActorIterator<ADecalActor> It(World); It; ++It)
	{
		if (!It->Tags.Contains(DecalTag))
		{
			continue;
		}
		for (const FName& Tag : It->Tags)
		{
			if (FString S = Tag.ToString(); S.StartsWith(DecalNodePrefix))
			{
				ByNode.Add(S.RightChop(FCString::Strlen(DecalNodePrefix)), *It);
			}
		}
	}
	// Each instance is made or updated once per call, however many decals and meshes use it.
	TMap<FString, UMaterialInstanceConstant*> Instances;
	auto InstanceOf = [&](const TSharedPtr<FJsonObject>& Owner) -> UMaterialInstanceConstant* {
		const TSharedPtr<FJsonObject>* InstanceJson = nullptr;
		FString InstanceName;
		if (!Owner->TryGetObjectField(TEXT("instance"), InstanceJson) || !(*InstanceJson)->TryGetStringField(TEXT("name"), InstanceName))
		{
			return nullptr;
		}
		if (UMaterialInstanceConstant** Done = Instances.Find(InstanceName))
		{
			return *Done;
		}
		UMaterialInstanceConstant* Instance = AssetAt<UMaterialInstanceConstant>(Folder, InstanceName);
		if (!Instance)
		{
			UMaterialInstanceConstantFactoryNew* Factory = NewObject<UMaterialInstanceConstantFactoryNew>(GetTransientPackage());
			Factory->InitialParent = Master;
			Instance = Cast<UMaterialInstanceConstant>(
				FAssetToolsModule::GetModule().Get().CreateAsset(InstanceName, Folder, UMaterialInstanceConstant::StaticClass(), Factory));
			if (Instance)
			{
				Created.AddUnique(Instance->GetPathName());
			}
		}
		if (Instance)
		{
			Instance->Modify();
			if (Instance->Parent != Master)
			{
				UMaterialEditingLibrary::SetMaterialInstanceParent(Instance, Master);
			}
			SetParameters(Instance, *InstanceJson, Warnings);
			UMaterialEditingLibrary::UpdateMaterialInstance(Instance);
			Instance->MarkPackageDirty();
		}
		else
		{
			Warnings.Add(FString::Printf(TEXT("Could not create %s in %s"), *InstanceName, *Folder));
		}
		Instances.Add(InstanceName, Instance);
		return Instance;
	};

	TArray<TSharedPtr<FJsonValue>> Placed;
	int32 Spawned = 0, Updated = 0;
	for (const TSharedPtr<FJsonValue>& V : *Decals)
	{
		const TSharedPtr<FJsonObject> D = V.IsValid() ? V->AsObject() : nullptr;
		FString Id, Name;
		if (!D.IsValid() || !D->TryGetStringField(TEXT("id"), Id))
		{
			Warnings.Add(TEXT("a decal without an id was skipped"));
			continue;
		}
		D->TryGetStringField(TEXT("name"), Name);
		UMaterialInstanceConstant* Instance = InstanceOf(D);

		// The actor's X axis projects into the surface; Z is the decal's up.
		const FVector Forward = DecalVector(D, TEXT("forward"), -FVector::UpVector);
		const FVector Up = DecalVector(D, TEXT("up"), FVector::ForwardVector);
		const FVector Size = DecalVector(D, TEXT("size"), FVector(10.0, 100.0, 100.0));
		const FTransform Transform(FRotationMatrix::MakeFromXZ(Forward, Up).ToQuat(), DecalVector(D, TEXT("location"), FVector::ZeroVector));
		ADecalActor*& Actor = ByNode.FindOrAdd(Id);
		if (!Actor)
		{
			FActorSpawnParameters SpawnParams;
			SpawnParams.ObjectFlags |= RF_Transactional;
			Actor = World->SpawnActor<ADecalActor>(ADecalActor::StaticClass(), Transform, SpawnParams);
			if (!Actor)
			{
				Warnings.Add(FString::Printf(TEXT("Could not place decal %s"), *Name));
				continue;
			}
			Actor->Tags.Add(DecalTag);
			Actor->Tags.Add(FName(*(FString(DecalNodePrefix) + Id)));
			++Spawned;
		}
		else
		{
			Actor->Modify();
			++Updated;
		}
		// Spawning composes the transform with the decal component's default pitch of -90, so it is set again here.
		Actor->SetActorTransform(Transform);
		Actor->SetFolderPath(FName(TEXT("Lila/Decals")));
		if (!Name.IsEmpty())
		{
			Actor->SetActorLabel(Name);
		}
		if (UDecalComponent* Decal = Actor->GetDecal())
		{
			Decal->Modify();
			// DecalSize is half the box: depth along the projection, then width and height.
			Decal->DecalSize = Size * 0.5;
			double Sort = 0.0;
			if (D->TryGetNumberField(TEXT("sortOrder"), Sort))
			{
				Decal->SortOrder = int32(Sort);
			}
			if (Instance)
			{
				Decal->SetDecalMaterial(Instance);
			}
			Decal->MarkRenderStateDirty();
		}
		TSharedRef<FJsonObject> P = MakeShared<FJsonObject>();
		P->SetStringField(TEXT("id"), Id);
		P->SetStringField(TEXT("actor"), Actor->GetPathName());
		P->SetStringField(TEXT("material"), Instance ? Instance->GetPathName() : FString());
		Placed.Add(Obj(P));
	}

	// Mesh decals: the decal instance on the named slot of static meshes, which then render as mesh decals.
	TArray<TSharedPtr<FJsonValue>> Assigned;
	const TArray<TSharedPtr<FJsonValue>>* MeshDecals = nullptr;
	if (Params->TryGetArrayField(TEXT("meshes"), MeshDecals))
	{
		for (const TSharedPtr<FJsonValue>& V : *MeshDecals)
		{
			const TSharedPtr<FJsonObject> M = V.IsValid() ? V->AsObject() : nullptr;
			FString Path, Slot;
			if (!M.IsValid() || !M->TryGetStringField(TEXT("mesh"), Path) || !M->TryGetStringField(TEXT("slot"), Slot))
			{
				Warnings.Add(TEXT("a mesh decal without mesh and slot was skipped"));
				continue;
			}
			UStaticMesh* Mesh = Cast<UStaticMesh>(FSoftObjectPath(Path).TryLoad());
			if (!Mesh)
			{
				Warnings.Add(FString::Printf(TEXT("no static mesh %s"), *Path));
				continue;
			}
			UMaterialInstanceConstant* Instance = InstanceOf(M);
			if (!Instance)
			{
				continue;
			}
			bool bChanged = false;
			for (FStaticMaterial& Material : Mesh->GetStaticMaterials())
			{
				if (Material.MaterialSlotName != FName(*Slot))
				{
					continue;
				}
				TSharedRef<FJsonObject> A = MakeShared<FJsonObject>();
				A->SetStringField(TEXT("mesh"), Mesh->GetPathName());
				A->SetStringField(TEXT("slot"), Slot);
				A->SetStringField(TEXT("material"), Instance->GetPathName());
				A->SetBoolField(TEXT("changed"), Material.MaterialInterface != Instance);
				Assigned.Add(Obj(A));
				if (Material.MaterialInterface == Instance)
				{
					continue;
				}
				if (!bChanged)
				{
					Mesh->Modify();
					bChanged = true;
				}
				Material.MaterialInterface = Instance;
			}
			if (bChanged)
			{
				Mesh->PostEditChange();
				Mesh->MarkPackageDirty();
			}
		}
	}
	if (ULevel* Level = World->GetCurrentLevel())
	{
		Level->MarkPackageDirty();
	}

	TSharedRef<FJsonObject> Out = MakeShared<FJsonObject>();
	Out->SetStringField(TEXT("master"), Master->GetPathName());
	Out->SetBoolField(TEXT("built"), bBuilt);
	Out->SetNumberField(TEXT("spawned"), Spawned);
	Out->SetNumberField(TEXT("updated"), Updated);
	Out->SetArrayField(TEXT("decals"), Placed);
	Out->SetArrayField(TEXT("assigned"), Assigned);
	TArray<TSharedPtr<FJsonValue>> CreatedJson, WarningsJson;
	for (const FString& C : Created)
	{
		CreatedJson.Add(Str(C));
	}
	for (const FString& W : Warnings)
	{
		WarningsJson.Add(Str(W));
		UE_LOG(LogLilaLink, Warning, TEXT("trimDecals: %s"), *W);
	}
	Out->SetArrayField(TEXT("created"), CreatedJson);
	Out->SetArrayField(TEXT("warnings"), WarningsJson);
	return Obj(Out);
}
} // namespace

UMaterial* LilaLink::TrimMaster(bool bRegenerate, bool& bOutBuilt, int32& OutVersion, TArray<FString>& OutCreated, TArray<FString>& OutWarnings)
{
	TRACE_CPUPROFILER_EVENT_SCOPE(LilaLinkTrim::TrimMaster);

	const FString Folder = LilaLink::MastersFolderPath();
	FString Error;
	const LilaLink::FMaterialPair Pair = LilaLink::FindOrCreateMaterialPair(Folder, FunctionName, MasterName, Error);
	if (!Pair.Material)
	{
		OutWarnings.Add(Error);
		return nullptr;
	}
	bOutBuilt = Pair.bCreated || bRegenerate;
	if (bOutBuilt)
	{
		BuildFunction(Pair.Function, OutCreated);
		BuildMaster(Pair.Material, Pair.Function, OutWarnings);
		OutCreated.AddUnique(Pair.Function->GetPathName());
		OutCreated.AddUnique(Pair.Material->GetPathName());
	}
	OutVersion = VersionOf(Pair.Function);
	if (OutVersion < MasterVersion)
	{
		OutWarnings.Add(FString::Printf(TEXT("%s is version %d, older than this plugin's %d; it is kept as edited (regenerate rebuilds it)"),
			MasterName, OutVersion, MasterVersion));
	}
	return Pair.Material;
}

void LilaLink::AddTrimMethods(TMap<FString, FLilaLinkMethod>& Methods)
{
	Methods.Add(TEXT("trimMaterials"), LilaLink::Sync(&TrimMaterials));
	Methods.Add(TEXT("trimDecals"), LilaLink::Sync(&TrimDecals));
}
