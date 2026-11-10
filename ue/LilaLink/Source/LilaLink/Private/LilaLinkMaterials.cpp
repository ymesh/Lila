#include "LilaLinkMaterials.h"

#include "LilaLinkHeightNormal.h"
#include "LilaLinkLog.h"
#include "LilaLinkMaterialFunction.h"

#include "AssetImportTask.h"
#include "AssetRegistry/IAssetRegistry.h"
#include "AssetToolsModule.h"
#include "Engine/SkeletalMesh.h"
#include "Engine/StaticMesh.h"
#include "Engine/Texture.h"
#include "Engine/Texture2D.h"
#include "Factories/MaterialFactoryNew.h"
#include "Factories/MaterialInstanceConstantFactoryNew.h"
#include "Factories/TextureFactory.h"
#include "IAssetTools.h"
#include "MaterialEditingLibrary.h"
#include "Materials/Material.h"
#include "Materials/MaterialExpressionAdd.h"
#include "Materials/MaterialExpressionCameraVectorWS.h"
#include "Materials/MaterialExpressionCustom.h"
#include "Materials/MaterialExpressionLinearInterpolate.h"
#include "Materials/MaterialExpressionLocalPosition.h"
#include "Materials/MaterialExpressionMaterialFunctionCall.h"
#include "Materials/MaterialExpressionMultiply.h"
#include "Materials/MaterialExpressionScalarParameter.h"
#include "Materials/MaterialExpressionSkyAtmosphereLightDirection.h"
#include "Materials/MaterialExpressionTextureObjectParameter.h"
#include "Materials/MaterialExpressionTextureCoordinate.h"
#include "Materials/MaterialExpressionTextureSampleParameter2D.h"
#include "Materials/MaterialExpressionTime.h"
#include "Materials/MaterialExpressionTransform.h"
#include "Materials/MaterialExpressionVectorParameter.h"
#include "Materials/MaterialExpressionVertexNormalWS.h"
#include "Materials/MaterialFunction.h"
#include "Materials/MaterialInstanceConstant.h"
#include "Materials/MaterialInterface.h"
#include "Misc/PackageName.h"
#include "Misc/Paths.h"
#include "ObjectTools.h"
#include "ProfilingDebugging/CpuProfilerTrace.h"
#include "UObject/StrongObjectPtr.h"

namespace
{
using LilaLink::FMaterialRequest;
using LilaLink::FSlotResult;

enum class ETextureUse : uint8
{
	Color,
	Linear,
	Normal,
	/** Single channel, kept at 16 bits: heights. */
	Height,
};

double Number(const TSharedPtr<FJsonObject>& In, const TCHAR* Field, double Default)
{
	double V = Default;
	return In.IsValid() && In->TryGetNumberField(Field, V) ? V : Default;
}

FLinearColor Color(const TSharedPtr<FJsonObject>& In, const TCHAR* Field, const FLinearColor& Default)
{
	const TArray<TSharedPtr<FJsonValue>>* Values = nullptr;
	if (!In.IsValid() || !In->TryGetArrayField(Field, Values) || Values->Num() < 3)
	{
		return Default;
	}
	return FLinearColor(float((*Values)[0]->AsNumber()), float((*Values)[1]->AsNumber()), float((*Values)[2]->AsNumber()));
}

FString String(const TSharedPtr<FJsonObject>& In, const TCHAR* Field)
{
	FString S;
	return In.IsValid() && In->TryGetStringField(Field, S) ? S : FString();
}

FString AssetNameFor(FName Slot)
{
	return ObjectTools::SanitizeObjectName(Slot.ToString());
}

UMaterialInterface* MaterialAt(const FString& Folder, const FString& Name)
{
	FAssetData Data = IAssetRegistry::GetChecked().GetAssetByObjectPath(FSoftObjectPath(Folder / Name + TEXT(".") + Name));
	return Data.IsValid() && Data.IsInstanceOf(UMaterialInterface::StaticClass()) ? Cast<UMaterialInterface>(Data.GetAsset()) : nullptr;
}

/** Every material and material instance under /Game, by asset name; gathered once per import. */
struct FProjectMaterials
{
	bool bGathered = false;
	TMultiMap<FName, FAssetData> ByName;

	TArray<FAssetData> Find(FName Name)
	{
		TRACE_CPUPROFILER_EVENT_SCOPE(LilaLinkMaterials::FindInProject);

		if (!bGathered)
		{
			bGathered = true;
			FARFilter Filter;
			Filter.PackagePaths.Add(TEXT("/Game"));
			Filter.bRecursivePaths = true;
			Filter.ClassPaths.Add(UMaterialInterface::StaticClass()->GetClassPathName());
			Filter.bRecursiveClasses = true;
			TArray<FAssetData> Assets;
			IAssetRegistry::GetChecked().GetAssets(Filter, Assets);
			for (const FAssetData& A : Assets)
			{
				ByName.Add(A.AssetName, A);
			}
		}
		TArray<FAssetData> Out;
		ByName.MultiFind(Name, Out);
		return Out;
	}
};

UTexture* ImportTexture(const FString& File, const FString& Folder, ETextureUse Use, bool bFlipGreen, TArray<FString>& OutCreated,
	TArray<FString>& OutWarnings)
{
	TRACE_CPUPROFILER_EVENT_SCOPE(LilaLinkMaterials::ImportTexture);

	const FString Name = ObjectTools::SanitizeObjectName(FPaths::GetBaseFilename(File));
	FAssetData Existing = IAssetRegistry::GetChecked().GetAssetByObjectPath(FSoftObjectPath(Folder / Name + TEXT(".") + Name));
	if (Existing.IsValid())
	{
		if (UTexture* Texture = Cast<UTexture>(Existing.GetAsset()))
		{
			// One texture asset per file, so the latest send decides its green convention and sRGB.
			if (Use == ETextureUse::Normal && Texture->bFlipGreenChannel != bFlipGreen)
			{
				Texture->Modify();
				Texture->bFlipGreenChannel = bFlipGreen;
				Texture->PostEditChange();
			}
			if ((Use == ETextureUse::Color || Use == ETextureUse::Linear) && Texture->SRGB != (Use == ETextureUse::Color))
			{
				Texture->Modify();
				Texture->SRGB = Use == ETextureUse::Color;
				Texture->PostEditChange();
			}
			return Texture;
		}
	}
	if (!FPaths::FileExists(File))
	{
		OutWarnings.Add(FString::Printf(TEXT("Texture %s not found; the material is made without it"), *File));
		return nullptr;
	}

	TStrongObjectPtr<UAssetImportTask> Task(NewObject<UAssetImportTask>(GetTransientPackage()));
	Task->Filename = File;
	Task->DestinationPath = Folder;
	Task->DestinationName = Name;
	Task->bReplaceExisting = false;
	Task->bAutomated = true;
	Task->bSave = false;
	// An explicit factory imports synchronously, without going through Interchange.
	Task->Factory = NewObject<UTextureFactory>(GetTransientPackage());
	FAssetToolsModule::GetModule().Get().ImportAssetTasks({Task.Get()});

	UTexture* Texture = nullptr;
	for (UObject* Object : Task->GetObjects())
	{
		Texture = Texture ? Texture : Cast<UTexture>(Object);
	}
	if (!Texture)
	{
		OutWarnings.Add(FString::Printf(TEXT("Could not import texture %s"), *File));
		return nullptr;
	}
	Texture->Modify();
	Texture->SRGB = Use == ETextureUse::Color;
	Texture->CompressionSettings = Use == ETextureUse::Normal ? TC_Normalmap : Use == ETextureUse::Height ? TC_Grayscale : TC_Default;
	LilaLink::KeepGrayscale16(Texture);
	Texture->bFlipGreenChannel = bFlipGreen;
	Texture->PostEditChange();
	OutCreated.AddUnique(Texture->GetPathName());
	return Texture;
}

/**
 * A texture's address modes and filter from Lila's sampling of the map in Field (<Field>WrapU, WrapV, Filter). Unreal
 * has no black border, so black clamps. One texture asset per file, so the latest send decides them.
 */
/** A map's use with its <map>ColorSpace from Lila applied: sRGB reads it as colour, linear as data. Normal maps stay normal. */
ETextureUse UseFor(const TSharedPtr<FJsonObject>& In, const TCHAR* Field, ETextureUse Default)
{
	const FString Space = String(In, *(FString(Field) + TEXT("ColorSpace")));
	if (Default == ETextureUse::Normal || Default == ETextureUse::Height)
	{
		return Default;
	}
	return Space == TEXT("sRGB") ? ETextureUse::Color : Space == TEXT("linear") ? ETextureUse::Linear : Default;
}

void ApplySampling(UTexture* Texture, const TSharedPtr<FJsonObject>& In, const TCHAR* Field)
{
	TRACE_CPUPROFILER_EVENT_SCOPE(LilaLinkMaterials::ApplySampling);

	UTexture2D* Texture2D = Cast<UTexture2D>(Texture);
	if (!Texture2D)
	{
		return;
	}
	auto Address = [&](const TCHAR* Suffix) {
		const FString Wrap = String(In, *(FString(Field) + Suffix));
		return Wrap == TEXT("clamp") || Wrap == TEXT("black") ? TA_Clamp : Wrap == TEXT("mirror") ? TA_Mirror : TA_Wrap;
	};
	const TextureAddress X = Address(TEXT("WrapU")), Y = Address(TEXT("WrapV"));
	const TextureFilter Filter = String(In, *(FString(Field) + TEXT("Filter"))) == TEXT("nearest") ? TF_Nearest : TF_Default;
	if (Texture2D->AddressX == X && Texture2D->AddressY == Y && Texture2D->Filter == Filter)
	{
		return;
	}
	Texture2D->Modify();
	Texture2D->AddressX = X;
	Texture2D->AddressY = Y;
	Texture2D->Filter = Filter;
	Texture2D->PostEditChange();
}

/** Builds a material from Lila's inputs: parameters for the constants, texture parameters for the maps. */
UMaterial* CreateMaterial(const FString& Name, const FMaterialRequest& R, const TSharedPtr<FJsonObject>& In, TArray<FString>& OutCreated,
	TArray<FString>& OutWarnings)
{
	TRACE_CPUPROFILER_EVENT_SCOPE(LilaLinkMaterials::CreateMaterial);

	UMaterialFactoryNew* Factory = NewObject<UMaterialFactoryNew>(GetTransientPackage());
	UMaterial* Material = Cast<UMaterial>(FAssetToolsModule::GetModule().Get().CreateAsset(Name, R.MaterialFolder, UMaterial::StaticClass(), Factory));
	if (!Material)
	{
		OutWarnings.Add(FString::Printf(TEXT("Could not create material %s in %s"), *Name, *R.MaterialFolder));
		return nullptr;
	}
	OutCreated.AddUnique(Material->GetPathName());

	const int32 UvSet = FMath::Clamp(int32(Number(In, TEXT("uvSet"), 0.0)), 0, 7);
	int32 Row = 0;
	auto NextY = [&Row] { return (Row++) * 160; };

	auto Scalar = [&](const TCHAR* Param, float Value) {
		auto* E = Cast<UMaterialExpressionScalarParameter>(UMaterialEditingLibrary::CreateMaterialExpression(Material, UMaterialExpressionScalarParameter::StaticClass(), -400, NextY()));
		E->ParameterName = Param;
		E->DefaultValue = Value;
		return E;
	};
	auto Vector = [&](const TCHAR* Param, const FLinearColor& Value) {
		auto* E = Cast<UMaterialExpressionVectorParameter>(UMaterialEditingLibrary::CreateMaterialExpression(Material, UMaterialExpressionVectorParameter::StaticClass(), -400, NextY()));
		E->ParameterName = Param;
		E->DefaultValue = Value;
		return E;
	};
	auto Map = [&](const TCHAR* Field, const TCHAR* Param, ETextureUse Use, bool bFlipGreen = false) -> UMaterialExpressionTextureSampleParameter2D* {
		const FString File = String(In, Field);
		if (File.IsEmpty() || !R.bImportTextures)
		{
			return nullptr;
		}
		Use = UseFor(In, Field, Use);
		UTexture* Texture = ImportTexture(File, R.MaterialFolder, Use, bFlipGreen, OutCreated, OutWarnings);
		if (!Texture)
		{
			return nullptr;
		}
		ApplySampling(Texture, In, Field);
		auto* E = Cast<UMaterialExpressionTextureSampleParameter2D>(UMaterialEditingLibrary::CreateMaterialExpression(Material, UMaterialExpressionTextureSampleParameter2D::StaticClass(), -400, NextY()));
		E->ParameterName = Param;
		E->Texture = Texture;
		E->SamplerType = Use == ETextureUse::Color ? SAMPLERTYPE_Color : Use == ETextureUse::Normal ? SAMPLERTYPE_Normal : SAMPLERTYPE_LinearColor;
		E->ConstCoordinate = uint8(UvSet);
		return E;
	};
	auto Multiply = [&](UMaterialExpression* A, const TCHAR* AOut, UMaterialExpression* B) {
		UMaterialExpression* E = UMaterialEditingLibrary::CreateMaterialExpression(Material, UMaterialExpressionMultiply::StaticClass(), -200, NextY());
		UMaterialEditingLibrary::ConnectMaterialExpressions(A, AOut, E, TEXT("A"));
		UMaterialEditingLibrary::ConnectMaterialExpressions(B, TEXT(""), E, TEXT("B"));
		return E;
	};

	const FString Blend = String(In, TEXT("blendMode"));
	Material->BlendMode = Blend == TEXT("masked") ? BLEND_Masked : Blend == TEXT("translucent") ? BLEND_Translucent : BLEND_Opaque;
	Material->TwoSided = In.IsValid() && In->HasTypedField<EJson::Boolean>(TEXT("twoSided")) && In->GetBoolField(TEXT("twoSided"));
	Material->OpacityMaskClipValue = float(Number(In, TEXT("opacityMaskClipValue"), 0.3333));

	UMaterialExpressionTextureSampleParameter2D* BaseMap = Map(TEXT("baseColorMap"), TEXT("BaseColorMap"), ETextureUse::Color);
	auto Opacity = [&] {
		if (Material->BlendMode != BLEND_Opaque)
		{
			const EMaterialProperty Target = Material->BlendMode == BLEND_Masked ? MP_OpacityMask : MP_Opacity;
			UMaterialExpression* Value = Scalar(TEXT("Opacity"), float(Number(In, TEXT("opacity"), 1.0)));
			UMaterialEditingLibrary::ConnectMaterialProperty(BaseMap ? Multiply(BaseMap, TEXT("A"), Value) : Value, TEXT(""), Target);
		}
	};

	// Lila's unlit model shows the base colour, plus emissive, with no lighting: Unreal's Unlit through Emissive Color.
	if (String(In, TEXT("shadingModel")) == TEXT("unlit"))
	{
		Material->SetShadingModel(MSM_Unlit);
		UMaterialExpression* Base = BaseMap ? static_cast<UMaterialExpression*>(BaseMap) : Vector(TEXT("BaseColor"), Color(In, TEXT("baseColor"), FLinearColor(0.5f, 0.5f, 0.5f)));
		const TCHAR* BaseOut = BaseMap ? TEXT("RGB") : TEXT("");
		const float Intensity = float(Number(In, TEXT("emissiveIntensity"), 1.0));
		UMaterialExpression* Glow = nullptr;
		const TCHAR* GlowOut = TEXT("");
		if (UMaterialExpression* EmissiveMap = Map(TEXT("emissiveMap"), TEXT("EmissiveMap"), ETextureUse::Color))
		{
			Glow = Multiply(EmissiveMap, TEXT("RGB"), Scalar(TEXT("EmissiveIntensity"), Intensity));
		}
		else if (const FLinearColor Emissive = Color(In, TEXT("emissiveColor"), FLinearColor::Black) * Intensity; !Emissive.Equals(FLinearColor::Black))
		{
			Glow = Vector(TEXT("EmissiveColor"), Emissive);
		}
		if (Glow)
		{
			UMaterialExpression* Sum = UMaterialEditingLibrary::CreateMaterialExpression(Material, UMaterialExpressionAdd::StaticClass(), -200, NextY());
			UMaterialEditingLibrary::ConnectMaterialExpressions(Base, BaseOut, Sum, TEXT("A"));
			UMaterialEditingLibrary::ConnectMaterialExpressions(Glow, GlowOut, Sum, TEXT("B"));
			UMaterialEditingLibrary::ConnectMaterialProperty(Sum, TEXT(""), MP_EmissiveColor);
		}
		else
		{
			UMaterialEditingLibrary::ConnectMaterialProperty(Base, BaseOut, MP_EmissiveColor);
		}
		Opacity();
		Material->PostEditChange();
		Material->MarkPackageDirty();
		return Material;
	}

	if (BaseMap)
	{
		UMaterialEditingLibrary::ConnectMaterialProperty(BaseMap, TEXT("RGB"), MP_BaseColor);
	}
	else
	{
		UMaterialEditingLibrary::ConnectMaterialProperty(Vector(TEXT("BaseColor"), Color(In, TEXT("baseColor"), FLinearColor(0.5f, 0.5f, 0.5f))), TEXT(""), MP_BaseColor);
	}

	// A packed map wins over the separate ones, as in Lila. MRO swaps occlusion and metallic.
	const bool bMro = String(In, TEXT("packedLayout")) == TEXT("mro");
	if (UMaterialExpressionTextureSampleParameter2D* Orm = Map(TEXT("ormMap"), bMro ? TEXT("MROMap") : TEXT("ORMMap"), ETextureUse::Linear))
	{
		UMaterialEditingLibrary::ConnectMaterialProperty(Orm, bMro ? TEXT("B") : TEXT("R"), MP_AmbientOcclusion);
		UMaterialEditingLibrary::ConnectMaterialProperty(Orm, TEXT("G"), MP_Roughness);
		UMaterialEditingLibrary::ConnectMaterialProperty(Orm, bMro ? TEXT("R") : TEXT("B"), MP_Metallic);
	}
	else
	{
		UMaterialExpression* Metallic = Map(TEXT("metallicMap"), TEXT("MetallicMap"), ETextureUse::Linear);
		UMaterialEditingLibrary::ConnectMaterialProperty(Metallic ? Metallic : Scalar(TEXT("Metallic"), float(Number(In, TEXT("metallic"), 0.0))), Metallic ? TEXT("R") : TEXT(""), MP_Metallic);
		UMaterialExpression* Roughness = Map(TEXT("roughnessMap"), TEXT("RoughnessMap"), ETextureUse::Linear);
		UMaterialEditingLibrary::ConnectMaterialProperty(Roughness ? Roughness : Scalar(TEXT("Roughness"), float(Number(In, TEXT("roughness"), 0.55))), Roughness ? TEXT("R") : TEXT(""), MP_Roughness);
		if (UMaterialExpression* Ao = Map(TEXT("ambientOcclusionMap"), TEXT("AmbientOcclusionMap"), ETextureUse::Linear))
		{
			UMaterialEditingLibrary::ConnectMaterialProperty(Ao, TEXT("R"), MP_AmbientOcclusion);
		}
	}
	UMaterialEditingLibrary::ConnectMaterialProperty(Scalar(TEXT("Specular"), float(Number(In, TEXT("specular"), 0.5))), TEXT(""), MP_Specular);

	UMaterialExpression* Normal = Map(TEXT("normalMap"), TEXT("NormalMap"), ETextureUse::Normal, String(In, TEXT("normalFormat")) == TEXT("opengl"));
	const bool bFromHeight = In.IsValid() && In->HasTypedField<EJson::Boolean>(TEXT("normalFromHeight")) && In->GetBoolField(TEXT("normalFromHeight"));
	UTexture* Height = nullptr;
	if (bFromHeight && R.bImportTextures && !String(In, TEXT("heightMap")).IsEmpty())
	{
		Height = ImportTexture(String(In, TEXT("heightMap")), R.MaterialFolder, ETextureUse::Height, false, OutCreated, OutWarnings);
	}
	UMaterialFunction* Rebuild = Height ? LilaLink::HeightNormalFunction(false, OutCreated) : nullptr;
	if (Rebuild)
	{
		// Normals From Height: MF_LilaHeightNormal rebuilds the shape from the height and lays the normal map on it as detail.
		ApplySampling(Height, In, TEXT("heightMap"));
		auto* Call = Cast<UMaterialExpressionMaterialFunctionCall>(
			UMaterialEditingLibrary::CreateMaterialExpression(Material, UMaterialExpressionMaterialFunctionCall::StaticClass(), -200, NextY()));
		Call->SetMaterialFunction(Rebuild);
		auto* HeightParam = Cast<UMaterialExpressionTextureObjectParameter>(
			UMaterialEditingLibrary::CreateMaterialExpression(Material, UMaterialExpressionTextureObjectParameter::StaticClass(), -400, NextY()));
		HeightParam->ParameterName = TEXT("HeightMap");
		HeightParam->Texture = Height;
		HeightParam->SamplerType = SAMPLERTYPE_LinearGrayscale;
		auto* Uv = Cast<UMaterialExpressionTextureCoordinate>(
			UMaterialEditingLibrary::CreateMaterialExpression(Material, UMaterialExpressionTextureCoordinate::StaticClass(), -400, NextY()));
		Uv->CoordinateIndex = UvSet;
		UMaterialEditingLibrary::ConnectMaterialExpressions(HeightParam, TEXT(""), Call, TEXT("HeightMap"));
		UMaterialEditingLibrary::ConnectMaterialExpressions(Uv, TEXT(""), Call, TEXT("UVs"));
		UMaterialEditingLibrary::ConnectMaterialExpressions(Scalar(TEXT("HeightDepth"), float(Number(In, TEXT("heightDepth"), 5.0))), TEXT(""), Call, TEXT("HeightDepth"));
		UMaterialEditingLibrary::ConnectMaterialExpressions(Scalar(TEXT("HeightUVWidth"), float(Number(In, TEXT("heightUvWidth"), 100.0))), TEXT(""), Call,
			TEXT("HeightUVWidth"));
		UMaterialEditingLibrary::ConnectMaterialExpressions(Scalar(TEXT("HeightUVHeight"), float(Number(In, TEXT("heightUvHeight"), 100.0))), TEXT(""), Call,
			TEXT("HeightUVHeight"));
		if (Normal)
		{
			UMaterialEditingLibrary::ConnectMaterialExpressions(Normal, TEXT("RGB"), Call, TEXT("DetailNormal"));
		}
		UMaterialEditingLibrary::ConnectMaterialProperty(Call, TEXT("Normal"), MP_Normal);
	}
	else if (Normal)
	{
		UMaterialEditingLibrary::ConnectMaterialProperty(Normal, TEXT("RGB"), MP_Normal);
	}

	const float Intensity = float(Number(In, TEXT("emissiveIntensity"), 1.0));
	if (UMaterialExpression* EmissiveMap = Map(TEXT("emissiveMap"), TEXT("EmissiveMap"), ETextureUse::Color))
	{
		UMaterialEditingLibrary::ConnectMaterialProperty(Multiply(EmissiveMap, TEXT("RGB"), Scalar(TEXT("EmissiveIntensity"), Intensity)), TEXT(""), MP_EmissiveColor);
	}
	else
	{
		const FLinearColor Emissive = Color(In, TEXT("emissiveColor"), FLinearColor::Black) * Intensity;
		if (!Emissive.Equals(FLinearColor::Black))
		{
			UMaterialEditingLibrary::ConnectMaterialProperty(Vector(TEXT("EmissiveColor"), Emissive), TEXT(""), MP_EmissiveColor);
		}
	}

	Opacity();

	Material->PostEditChange();
	Material->MarkPackageDirty();
	return Material;
}

/** Where the generated stylised master materials live; every Lila material of that model is an instance of one. */
const TCHAR* MastersFolder = TEXT("/Game/LilaLink/Masters");

/** Triplanar stroke sheets bending the object-space normal, RNM-blended (Lila's mesh.slang painterly normal). */
const TCHAR* PainterlyNormalCode = TEXT(R"(
float3 nb = normalize(LocalNormal);
float3 an = abs(nb);
float3 w = pow(an, 4.0);
w /= max(w.x + w.y + w.z, 1e-6);
float scale = 1.0 / max(StrokeScale, 0.01);
float2 boil = 0;
if (Boil > 0.5)
{
    float frame = floor(Time * max(BoilFps, 0.1));
    boil = frac(sin(float2(frame * 12.9898, frame * 78.233)) * 43758.5453);
}
float tilt = saturate(Strength * StrokeNormal);
float3 result = 0;
for (int i = 0; i < 3; i++)
{
    float2 uv = (i == 0 ? LocalPos.zy : (i == 1 ? LocalPos.xz : LocalPos.xy)) * scale;
    float4 a = Texture2DSample(Strokes, StrokesSampler, uv + boil);
    float4 b = Texture2DSample(Strokes, StrokesSampler, uv * Ratio + float2(0.37, 0.71) + boil * 1.7);
    float2 ta = (a.xy * 2 - 1) * tilt;
    float2 tb = (b.xy * 2 - 1) * tilt * 0.6;
    float3 n1 = float3(ta, sqrt(saturate(1 - dot(ta, ta)))) + float3(0, 0, 1);
    float3 n2 = float3(tb, sqrt(saturate(1 - dot(tb, tb)))) * float3(-1, -1, 1);
    float3 t = normalize(n1 * dot(n1, n2) / n1.z - n2);
    float3 s = i == 0 ? float3(nb.zy, an.x) : (i == 1 ? float3(nb.xz, an.y) : float3(nb.xy, an.z));
    n1 = s + float3(0, 0, 1);
    n2 = t * float3(-1, -1, 1);
    float3 r = n1 * dot(n1, n2) / n1.z - n2;
    r.z *= (i == 0 ? nb.x : (i == 1 ? nb.y : nb.z)) < 0 ? -1 : 1;
    float3 o = i == 0 ? r.zyx : (i == 1 ? r.xzy : r.xyz);
    result += o * (i == 0 ? w.x : (i == 1 ? w.y : w.z));
}
return normalize(result);
)");

/** The base colour pushed through the palette by the light and the strokes (Lila's painterlySurface). */
const TCHAR* PainterlyColorCode = TEXT(R"(
float3 nb = normalize(LocalNormal);
float3 w = pow(abs(nb), 4.0);
w /= max(w.x + w.y + w.z, 1e-6);
float scale = 1.0 / max(StrokeScale, 0.01);
float2 boil = 0;
if (Boil > 0.5)
{
    float frame = floor(Time * max(BoilFps, 0.1));
    boil = frac(sin(float2(frame * 12.9898, frame * 78.233)) * 43758.5453);
}
float shade = 0, rnd = 0.5, best = -1;
for (int i = 0; i < 3; i++)
{
    float2 uv = (i == 0 ? LocalPos.zy : (i == 1 ? LocalPos.xz : LocalPos.xy)) * scale;
    float4 a = Texture2DSample(Strokes, StrokesSampler, uv + boil);
    float4 b = Texture2DSample(Strokes, StrokesSampler, uv * Ratio + float2(0.37, 0.71) + boil * 1.7);
    float wi = i == 0 ? w.x : (i == 1 ? w.y : w.z);
    shade += lerp(a.z, b.z, 0.35) * wi;
    if (wi > best)
    {
        best = wi;
        rnd = a.w;
    }
}
float3 l = dot(LightDir, LightDir) > 1e-6 ? normalize(LightDir) : normalize(FallbackLight);
float t = dot(normalize(Normal), l) * 0.5 + 0.5;
t += (shade - 0.5) * 2 * Spread;
float band = lerp(0.5, 0.02, saturate(Contrast));
t = smoothstep(0.5 - band, 0.5 + band, t);
float3 pal = t < 0.5 ? lerp(Shadow, Base, t * 2) : (Count >= 2.5 ? lerp(Base, Light, (t - 0.5) * 1.2) : Base);
if (Count >= 3.5)
    pal = lerp(pal, Accent, smoothstep(0.82, 0.9, rnd) * 0.7);
return lerp(Base, pal, saturate(Strength) * 0.75);
)");

/** Stepped light and tinted shadow, an outer band, a hard highlight and a rim (Lila's toonShade). Unlit: this is the colour. */
const TCHAR* ToonCode = TEXT(R"(
float3 n = normalize(Normal);
float3 v = normalize(CameraVec);
float3 l = dot(LightDir, LightDir) > 1e-6 ? normalize(LightDir) : normalize(FallbackLight);
float nl = dot(n, l);
float soft = max(Softness, 1e-3);
float lit = smoothstep(Threshold - soft, Threshold + soft, nl);
float3 shade = ShadowColor;
if (OuterOffset > 0)
{
    float edge = Threshold - OuterOffset;
    shade = lerp(OuterColor, ShadowColor, smoothstep(edge - soft * 0.25, edge + soft * 0.25, nl));
}
float3 c = Base * lerp(shade, LightTint, lit);
float3 h = normalize(v + l);
float gloss = 1 - Roughness;
float hl = smoothstep(0.5, 0.55, pow(saturate(dot(n, h)), lerp(8.0, 256.0, gloss * gloss)));
c += hl * lit * gloss * Specular * 0.6 * LightTint;
float rimLine = 1 - saturate(RimWidth);
float rim = smoothstep(rimLine, rimLine + 0.06, 1 - saturate(dot(n, v)));
c += RimColor * rim * RimStrength * (0.3 + 0.7 * lit);
return c;
)");

const TCHAR* const WhiteSquarePath = TEXT("/Engine/EngineResources/WhiteSquareTexture.WhiteSquareTexture");

UMaterialExpressionCustom* StylisedCustom(LilaLink::FFunctionGraph& G, const TCHAR* Description, const TCHAR* Code,
	std::initializer_list<TPair<const TCHAR*, UMaterialExpression*>> Inputs)
{
	auto* E = G.New<UMaterialExpressionCustom>(-300);
	E->Description = Description;
	E->Code = Code;
	E->OutputType = CMOT_Float3;
	E->Inputs.Reset();
	for (const TPair<const TCHAR*, UMaterialExpression*>& In : Inputs)
	{
		E->Inputs.AddDefaulted_GetRef().InputName = In.Key;
	}
	for (const TPair<const TCHAR*, UMaterialExpression*>& In : Inputs)
	{
		UMaterialEditingLibrary::ConnectMaterialExpressions(In.Value, TEXT(""), E, In.Key);
	}
	return E;
}

/** The inputs both stylised functions share: BaseColor, Normal, LightDirection and FallbackLightDirection. */
struct FStylisedInputs
{
	UMaterialExpression* Base = nullptr;
	UMaterialExpression* Normal = nullptr;
	UMaterialExpression* Light = nullptr;
	UMaterialExpression* Fallback = nullptr;
};

FStylisedInputs StylisedInputs(LilaLink::FFunctionGraph& G)
{
	// BaseColor's default: the constant, or the map when the instance turns UseBaseColorMap on (Lila's map replaces the constant).
	auto* BaseMap = G.New<UMaterialExpressionTextureSampleParameter2D>(-1900);
	BaseMap->ParameterName = TEXT("BaseColorMap");
	BaseMap->Texture = LoadObject<UTexture>(nullptr, WhiteSquarePath);
	auto* BaseValue = G.New<UMaterialExpressionVectorParameter>(-1900);
	BaseValue->ParameterName = TEXT("BaseColor");
	BaseValue->DefaultValue = FLinearColor(0.5f, 0.5f, 0.5f);
	auto* UseMap = G.New<UMaterialExpressionScalarParameter>(-1900);
	UseMap->ParameterName = TEXT("UseBaseColorMap");
	UseMap->DefaultValue = 0.0f;
	auto* BaseLerp = G.New<UMaterialExpressionLinearInterpolate>(-1500);
	UMaterialEditingLibrary::ConnectMaterialExpressions(BaseValue, TEXT(""), BaseLerp, TEXT("A"));
	UMaterialEditingLibrary::ConnectMaterialExpressions(BaseMap, TEXT("RGB"), BaseLerp, TEXT("B"));
	UMaterialEditingLibrary::ConnectMaterialExpressions(UseMap, TEXT(""), BaseLerp, TEXT("Alpha"));

	auto* Light = G.New<UMaterialExpressionSkyAtmosphereLightDirection>(-1500);
	Light->LightIndex = 0;
	FStylisedInputs Out;
	Out.Base = G.Input(TEXT("BaseColor"), FunctionInput_Vector3, BaseLerp,
		TEXT("The surface colour. Unconnected: BaseColor, or BaseColorMap when UseBaseColorMap is 1."));
	Out.Normal = G.Input(TEXT("Normal"), FunctionInput_Vector3, G.New<UMaterialExpressionVertexNormalWS>(-1500),
		TEXT("World-space normal; the vertex normal when unconnected."));
	Out.Light = G.Input(TEXT("LightDirection"), FunctionInput_Vector3, Light,
		TEXT("World-space direction to the light; the sky atmosphere's sun when unconnected."));
	Out.Fallback = G.VectorInput(TEXT("FallbackLightDirection"), FLinearColor(0.3f, 0.3f, 0.9f), TEXT(""),
		TEXT("Used when LightDirection is zero, as it is with no sky atmosphere light."));
	return Out;
}

/** MF_LilaToon: Lila's toon shade (ToonCode) as one colour output, for Emissive Color on an Unlit material. */
void BuildToonFunction(UMaterialFunction* Function)
{
	TRACE_CPUPROFILER_EVENT_SCOPE(LilaLinkMaterials::BuildToonFunction);

	LilaLink::FFunctionGraph G(Function, TEXT("Lila Toon: stepped light, tinted shadow, an outer band, a hard highlight and a rim."));
	const FStylisedInputs In = StylisedInputs(G);
	auto* Toon = StylisedCustom(G, TEXT("Lila Toon"), ToonCode,
		{{TEXT("Base"), In.Base}, {TEXT("Normal"), In.Normal}, {TEXT("CameraVec"), G.New<UMaterialExpressionCameraVectorWS>(-900)},
			{TEXT("LightDir"), In.Light}, {TEXT("FallbackLight"), In.Fallback},
			{TEXT("ShadowColor"), G.VectorInput(TEXT("ToonShadowColor"), FLinearColor(0.35f, 0.38f, 0.55f), TEXT(""), TEXT("Colour of the shadowed side."))},
			{TEXT("LightTint"), G.VectorInput(TEXT("ToonLightTint"), FLinearColor(1.0f, 0.97f, 0.92f), TEXT(""), TEXT("Colour of the lit side."))},
			{TEXT("OuterColor"), G.VectorInput(TEXT("ToonOuterColor"), FLinearColor(0.2f, 0.2f, 0.35f), TEXT(""), TEXT("Colour of the outer shadow band."))},
			{TEXT("RimColor"), G.VectorInput(TEXT("RimColor"), FLinearColor::White, TEXT(""), TEXT("Colour of the rim light."))},
			{TEXT("Threshold"), G.ScalarInput(TEXT("ToonThreshold"), 0.0f, TEXT(""), TEXT("Where light turns to shadow, as N dot L."))},
			{TEXT("Softness"), G.ScalarInput(TEXT("ToonSoftness"), 0.08f, TEXT(""), TEXT("Width of the light to shadow step."))},
			{TEXT("OuterOffset"), G.ScalarInput(TEXT("ToonOuterOffset"), 0.45f, TEXT(""), TEXT("How far past the threshold the outer band starts; 0 turns it off."))},
			{TEXT("RimWidth"), G.ScalarInput(TEXT("RimWidth"), 0.3f, TEXT(""), TEXT("Width of the rim, 0 to 1."))},
			{TEXT("RimStrength"), G.ScalarInput(TEXT("RimStrength"), 0.4f, TEXT(""), TEXT("Brightness of the rim."))},
			{TEXT("Roughness"), G.ScalarInput(TEXT("Roughness"), 0.5f, TEXT(""), TEXT("Size of the highlight: rougher is larger and dimmer."))},
			{TEXT("Specular"), G.ScalarInput(TEXT("Specular"), 0.5f, TEXT(""), TEXT("Brightness of the highlight."))}});
	G.Output(Toon, TEXT("Color"), TEXT("The shaded colour: connect to Emissive Color on an Unlit material."));
	G.Finish();
}

/**
 * MF_LilaPainterly: the base colour pushed through the palette by the light and the strokes (PainterlyColorCode), and the
 * normal bent by triplanar stroke sheets in object space (PainterlyNormalCode), handed back in world space. Strokes is the
 * StrokeMap default (a linear texture); without one the engine's white square stands in.
 */
void BuildPainterlyFunction(UMaterialFunction* Function, UTexture* Strokes)
{
	TRACE_CPUPROFILER_EVENT_SCOPE(LilaLinkMaterials::BuildPainterlyFunction);

	LilaLink::FFunctionGraph G(Function, TEXT("Lila Painterly: palette shading and stroke-bent normals from a triplanar stroke sheet."));
	const FStylisedInputs In = StylisedInputs(G);
	UMaterialExpression* StrokeMap = G.TextureInput(TEXT("StrokeMap"), Strokes ? Strokes : LoadObject<UTexture>(nullptr, WhiteSquarePath),
		Strokes ? SAMPLERTYPE_LinearColor : SAMPLERTYPE_Color, TEXT(""), TEXT("The stroke sheet: tilt in RG, shade in B, a random per stroke in A."));
	UMaterialExpression* Strength = G.ScalarInput(TEXT("PainterlyStrength"), 1.0f, TEXT(""), TEXT("How much of the palette and strokes show."));
	UMaterialExpression* Scale = G.ScalarInput(TEXT("StrokeScale"), 20.0f, TEXT(""), TEXT("Stroke sheet size in object-space units."));
	UMaterialExpression* Ratio = G.ScalarInput(TEXT("StrokeLayerRatio"), 2.7f, TEXT(""), TEXT("Scale of the second stroke layer against the first."));
	UMaterialExpression* StrokeNormal = G.ScalarInput(TEXT("StrokeNormal"), 0.6f, TEXT(""), TEXT("How far the strokes tilt the normal."));
	UMaterialExpression* Boil = G.ScalarInput(TEXT("Boil"), 0.0f, TEXT(""), TEXT("1 jumps the strokes BoilFps times a second."));
	UMaterialExpression* BoilFps = G.ScalarInput(TEXT("BoilFps"), 10.0f, TEXT(""), TEXT("Stroke jumps per second when Boil is 1."));

	auto* LocalPos = G.New<UMaterialExpressionLocalPosition>(-900);
	auto* ToLocal = G.New<UMaterialExpressionTransform>(-600);
	ToLocal->TransformSourceType = TRANSFORMSOURCE_World;
	ToLocal->TransformType = TRANSFORM_Local;
	ToLocal->Input.Connect(0, In.Normal);
	auto* Time = G.New<UMaterialExpressionTime>(-900);

	auto* Bent = StylisedCustom(G, TEXT("Lila Painterly Normal"), PainterlyNormalCode,
		{{TEXT("Strokes"), StrokeMap}, {TEXT("LocalPos"), LocalPos}, {TEXT("LocalNormal"), ToLocal}, {TEXT("StrokeScale"), Scale}, {TEXT("Ratio"), Ratio},
			{TEXT("Strength"), Strength}, {TEXT("StrokeNormal"), StrokeNormal}, {TEXT("Boil"), Boil}, {TEXT("BoilFps"), BoilFps}, {TEXT("Time"), Time}});
	auto* ToWorld = G.New<UMaterialExpressionTransform>(-100);
	ToWorld->TransformSourceType = TRANSFORMSOURCE_Local;
	ToWorld->TransformType = TRANSFORM_World;
	ToWorld->Input.Connect(0, Bent);

	auto* Color = StylisedCustom(G, TEXT("Lila Painterly Palette"), PainterlyColorCode,
		{{TEXT("Base"), In.Base}, {TEXT("Normal"), In.Normal}, {TEXT("LightDir"), In.Light}, {TEXT("FallbackLight"), In.Fallback},
			{TEXT("Strokes"), StrokeMap}, {TEXT("LocalPos"), LocalPos}, {TEXT("LocalNormal"), ToLocal}, {TEXT("StrokeScale"), Scale},
			{TEXT("Ratio"), Ratio}, {TEXT("Boil"), Boil}, {TEXT("BoilFps"), BoilFps}, {TEXT("Time"), Time},
			{TEXT("Shadow"), G.VectorInput(TEXT("PaletteShadow"), FLinearColor(0.10f, 0.12f, 0.25f), TEXT(""), TEXT("Palette colour of the shadows."))},
			{TEXT("Light"), G.VectorInput(TEXT("PaletteLight"), FLinearColor(0.95f, 0.80f, 0.55f), TEXT(""), TEXT("Palette colour of the lights (3 or more colours)."))},
			{TEXT("Accent"), G.VectorInput(TEXT("PaletteAccent"), FLinearColor(0.60f, 0.30f, 0.35f), TEXT(""), TEXT("Accent dabbed onto some strokes (4 colours)."))},
			{TEXT("Count"), G.ScalarInput(TEXT("PaletteCount"), 3.0f, TEXT(""), TEXT("Palette colours, 2 to 4."))},
			{TEXT("Contrast"), G.ScalarInput(TEXT("PaletteContrast"), 0.5f, TEXT(""), TEXT("Sharpness of the steps between palette colours."))},
			{TEXT("Spread"), G.ScalarInput(TEXT("PaletteSpread"), 0.35f, TEXT(""), TEXT("How far the strokes push the light across the palette."))},
			{TEXT("Strength"), Strength}});
	G.Output(Color, TEXT("BaseColor"), TEXT("The painted base colour."));
	G.Output(ToWorld, TEXT("Normal"), TEXT("The stroke-bent normal, world space: connect to Normal with Tangent Space Normal off."));
	G.Finish();
}

/**
 * The master material of a stylised model on its function, in the masters folder: M_LilaToon is Unlit with MF_LilaToon's
 * colour as emissive, M_LilaPainterly takes MF_LilaPainterly's base colour and world-space normal and adds Metallic,
 * Roughness and Specular parameters. Both are built when either is missing or bRebuild is set. Strokes is the Painterly
 * stroke sheet's default.
 */
UMaterial* StylisedMaster(bool bToon, UTexture* Strokes, bool bRebuild, TArray<FString>& OutCreated, TArray<FString>& OutWarnings)
{
	TRACE_CPUPROFILER_EVENT_SCOPE(LilaLinkMaterials::StylisedMaster);

	FString Error;
	const LilaLink::FMaterialPair Pair = bToon ? LilaLink::FindOrCreateMaterialPair(MastersFolder, TEXT("MF_LilaToon"), TEXT("M_LilaToon"), Error)
											   : LilaLink::FindOrCreateMaterialPair(MastersFolder, TEXT("MF_LilaPainterly"), TEXT("M_LilaPainterly"), Error);
	if (!Pair.Material)
	{
		OutWarnings.Add(Error);
		return nullptr;
	}
	if (!Pair.bCreated && !bRebuild)
	{
		return Pair.Material;
	}
	UMaterial* Material = Pair.Material;
	OutCreated.AddUnique(Pair.Function->GetPathName());
	OutCreated.AddUnique(Material->GetPathName());
	bool bWired = false;
	if (bToon)
	{
		BuildToonFunction(Pair.Function);
		UMaterialExpressionMaterialFunctionCall* Call = LilaLink::ResetToFunctionCall(Material, Pair.Function);
		Material->SetShadingModel(MSM_Unlit);
		bWired = LilaLink::ConnectFunctionOutputs(Call, {{TEXT("Color"), MP_EmissiveColor}});
	}
	else
	{
		BuildPainterlyFunction(Pair.Function, Strokes);
		UMaterialExpressionMaterialFunctionCall* Call = LilaLink::ResetToFunctionCall(Material, Pair.Function);
		Material->bTangentSpaceNormal = false;
		bWired = LilaLink::ConnectFunctionOutputs(Call, {{TEXT("BaseColor"), MP_BaseColor}, {TEXT("Normal"), MP_Normal}});
		int32 Row = 0;
		auto Scalar = [&](const TCHAR* Param, float Value, EMaterialProperty Property) {
			auto* E = Cast<UMaterialExpressionScalarParameter>(
				UMaterialEditingLibrary::CreateMaterialExpression(Material, UMaterialExpressionScalarParameter::StaticClass(), -400, 200 + (Row++) * 120));
			E->ParameterName = Param;
			E->DefaultValue = Value;
			UMaterialEditingLibrary::ConnectMaterialProperty(E, TEXT(""), Property);
		};
		Scalar(TEXT("Metallic"), 0.0f, MP_Metallic);
		Scalar(TEXT("Roughness"), 0.5f, MP_Roughness);
		Scalar(TEXT("Specular"), 0.5f, MP_Specular);
	}
	if (!bWired)
	{
		OutWarnings.Add(FString::Printf(TEXT("Could not wire the outputs of %s into %s"), *Pair.Function->GetName(), *Material->GetName()));
	}
	Material->PostEditChange();
	Material->MarkPackageDirty();
	return Material;
}

/**

 * A stylised master's parameters on an instance from Lila's material values. bImportBaseMap also imports and sets the material's
 * base colour map into TextureFolder (texture sets set their own).
 */
void SetStylisedParameters(UMaterialInstanceConstant* Instance, bool bToon, const TSharedPtr<FJsonObject>& In, UTexture* Strokes, bool bImportBaseMap,
	const FString& TextureFolder, TArray<FString>& OutCreated, TArray<FString>& OutWarnings)
{
	TRACE_CPUPROFILER_EVENT_SCOPE(LilaLinkMaterials::SetStylisedParameters);

	auto SetScalar = [&](const TCHAR* Param, const TCHAR* Field, double Default) {
		UMaterialEditingLibrary::SetMaterialInstanceScalarParameterValue(Instance, Param, float(Number(In, Field, Default)));
	};
	auto SetVector = [&](const TCHAR* Param, const TCHAR* Field, const FLinearColor& Default) {
		UMaterialEditingLibrary::SetMaterialInstanceVectorParameterValue(Instance, Param, Color(In, Field, Default));
	};
	SetVector(TEXT("BaseColor"), TEXT("baseColor"), FLinearColor(0.5f, 0.5f, 0.5f));
	SetScalar(TEXT("Roughness"), TEXT("roughness"), 0.5);
	SetScalar(TEXT("Specular"), TEXT("specular"), 0.5);
	const FString BaseFile = String(In, TEXT("baseColorMap"));
	if (!BaseFile.IsEmpty() && bImportBaseMap)
	{
		if (UseFor(In, TEXT("baseColorMap"), ETextureUse::Color) != ETextureUse::Color)
		{
			OutWarnings.Add(FString::Printf(TEXT("%s is marked linear in Lila, but the stylised material samples its base colour map as sRGB colour"), *BaseFile));
		}
		if (UTexture* Texture = ImportTexture(BaseFile, TextureFolder, ETextureUse::Color, false, OutCreated, OutWarnings))
		{
			ApplySampling(Texture, In, TEXT("baseColorMap"));
			UMaterialEditingLibrary::SetMaterialInstanceTextureParameterValue(Instance, TEXT("BaseColorMap"), Texture);
			UMaterialEditingLibrary::SetMaterialInstanceScalarParameterValue(Instance, TEXT("UseBaseColorMap"), 1.0f);
		}
	}
	if (bToon)
	{
		SetVector(TEXT("ToonShadowColor"), TEXT("toonShadowColor"), FLinearColor(0.35f, 0.38f, 0.55f));
		SetVector(TEXT("ToonLightTint"), TEXT("toonLightTint"), FLinearColor(1.0f, 0.97f, 0.92f));
		SetVector(TEXT("ToonOuterColor"), TEXT("toonOuterColor"), FLinearColor(0.2f, 0.2f, 0.35f));
		SetVector(TEXT("RimColor"), TEXT("rimColor"), FLinearColor::White);
		SetScalar(TEXT("ToonThreshold"), TEXT("toonThreshold"), 0.0);
		SetScalar(TEXT("ToonSoftness"), TEXT("toonSoftness"), 0.08);
		SetScalar(TEXT("ToonOuterOffset"), TEXT("toonOuterOffset"), 0.45);
		SetScalar(TEXT("RimWidth"), TEXT("rimWidth"), 0.3);
		SetScalar(TEXT("RimStrength"), TEXT("rimStrength"), 0.4);
	}
	else
	{
		SetScalar(TEXT("Metallic"), TEXT("metallic"), 0.0);
		SetScalar(TEXT("PainterlyStrength"), TEXT("painterlyStrength"), 1.0);
		SetScalar(TEXT("StrokeNormal"), TEXT("strokeNormal"), 0.6);
		SetScalar(TEXT("StrokeScale"), TEXT("strokeScale"), 20.0);
		SetScalar(TEXT("StrokeLayerRatio"), TEXT("strokeLayerRatio"), 2.7);
		SetScalar(TEXT("PaletteCount"), TEXT("paletteCount"), 3.0);
		SetScalar(TEXT("PaletteContrast"), TEXT("paletteContrast"), 0.5);
		SetScalar(TEXT("PaletteSpread"), TEXT("paletteSpread"), 0.35);
		SetScalar(TEXT("BoilFps"), TEXT("boilFps"), 10.0);
		const bool bBoil = In.IsValid() && In->HasTypedField<EJson::Boolean>(TEXT("boil")) && In->GetBoolField(TEXT("boil"));
		UMaterialEditingLibrary::SetMaterialInstanceScalarParameterValue(Instance, TEXT("Boil"), bBoil ? 1.0f : 0.0f);
		SetVector(TEXT("PaletteShadow"), TEXT("paletteShadow"), FLinearColor(0.10f, 0.12f, 0.25f));
		SetVector(TEXT("PaletteLight"), TEXT("paletteLight"), FLinearColor(0.95f, 0.80f, 0.55f));
		SetVector(TEXT("PaletteAccent"), TEXT("paletteAccent"), FLinearColor(0.60f, 0.30f, 0.35f));
		if (Strokes)
		{
			UMaterialEditingLibrary::SetMaterialInstanceTextureParameterValue(Instance, TEXT("StrokeMap"), Strokes);
		}
	}
}

/** The Painterly stroke sheet a material names, imported next to the masters; null for Toon or without one. */
UTexture* StylisedStrokes(bool bToon, const TSharedPtr<FJsonObject>& In, TArray<FString>& OutCreated, TArray<FString>& OutWarnings)
{
	const FString StrokeFile = String(In, TEXT("strokeMap"));
	return bToon || StrokeFile.IsEmpty() ? nullptr : ImportTexture(StrokeFile, MastersFolder, ETextureUse::Linear, false, OutCreated, OutWarnings);
}

/** Lila's material as an instance of the Painterly or Toon master, or null for the other shading models. */
UMaterialInterface* CreateStylisedInstance(const FString& Name, const FMaterialRequest& R, const TSharedPtr<FJsonObject>& In,
	TArray<FString>& OutCreated, TArray<FString>& OutWarnings)
{
	TRACE_CPUPROFILER_EVENT_SCOPE(LilaLinkMaterials::CreateStylisedInstance);

	const FString Model = String(In, TEXT("shadingModel"));
	const bool bToon = Model == TEXT("toon");
	if (!bToon && Model != TEXT("painterly"))
	{
		return nullptr;
	}
	UTexture* Strokes = R.bImportTextures ? StylisedStrokes(bToon, In, OutCreated, OutWarnings) : nullptr;
	UMaterial* Master = StylisedMaster(bToon, Strokes, R.bRebuildMaterial, OutCreated, OutWarnings);
	if (!Master)
	{
		return nullptr;
	}
	UMaterialInstanceConstantFactoryNew* Factory = NewObject<UMaterialInstanceConstantFactoryNew>(GetTransientPackage());
	Factory->InitialParent = Master;
	auto* Instance = Cast<UMaterialInstanceConstant>(
		FAssetToolsModule::GetModule().Get().CreateAsset(Name, R.MaterialFolder, UMaterialInstanceConstant::StaticClass(), Factory));
	if (!Instance)
	{
		OutWarnings.Add(FString::Printf(TEXT("Could not create material instance %s in %s"), *Name, *R.MaterialFolder));
		return nullptr;
	}
	OutCreated.AddUnique(Instance->GetPathName());
	SetStylisedParameters(Instance, bToon, In, Strokes, R.bImportTextures, R.MaterialFolder, OutCreated, OutWarnings);
	UMaterialEditingLibrary::UpdateMaterialInstance(Instance);
	Instance->MarkPackageDirty();
	return Instance;
}

struct FSlotView
{
	FName Name;
	TObjectPtr<UMaterialInterface>* Material = nullptr;
};

TArray<FSlotView> SlotsOf(UObject* Asset)
{
	TArray<FSlotView> Out;
	if (UStaticMesh* Mesh = Cast<UStaticMesh>(Asset))
	{
		for (FStaticMaterial& M : Mesh->GetStaticMaterials())
		{
			Out.Add({M.MaterialSlotName, &M.MaterialInterface});
		}
	}
	else if (USkeletalMesh* Skel = Cast<USkeletalMesh>(Asset))
	{
		for (FSkeletalMaterial& M : Skel->GetMaterials())
		{
			Out.Add({M.MaterialSlotName, &M.MaterialInterface});
		}
	}
	return Out;
}

/** The material a slot should have, and how it was chosen. */
UMaterialInterface* Resolve(FName Slot, const FMaterialRequest& R, FProjectMaterials& Project, FString& OutHow, TArray<FString>& OutCreated,
	TArray<FString>& OutWarnings)
{
	TRACE_CPUPROFILER_EVENT_SCOPE(LilaLinkMaterials::Resolve);

	const FString Name = AssetNameFor(Slot);
	// A kept material named after the slot is found again by name below, so only real reassignments count.
	if (R.bKeep)
	{
		if (const FSoftObjectPath* Previous = R.Previous.Find(Slot))
		{
			UMaterialInterface* Kept = Cast<UMaterialInterface>(Previous->TryLoad());
			if (Kept && Kept != UMaterial::GetDefaultMaterial(MD_Surface) && Kept->GetName() != Name)
			{
				OutHow = TEXT("kept");
				return Kept;
			}
		}
	}
	if (Slot == LilaLink::DefaultSlotName)
	{
		OutHow = TEXT("default");
		return UMaterial::GetDefaultMaterial(MD_Surface);
	}
	// Slots Lila names after one of its own materials (LilaSkirtBlend, LilaSeamStrip) take that master once it is built.
	if (UMaterialInterface* Master = MaterialAt(MastersFolder, TEXT("M_") + Name))
	{
		OutHow = TEXT("master");
		return Master;
	}
	for (const FString& Folder : {R.MaterialFolder, R.AssetFolder})
	{
		if (UMaterialInterface* Found = MaterialAt(Folder, Name))
		{
			OutHow = TEXT("found");
			return Found;
		}
	}
	TArray<FAssetData> Matches = Project.Find(FName(*Name));
	if (Matches.Num() == 1)
	{
		OutHow = TEXT("found");
		return Cast<UMaterialInterface>(Matches[0].GetAsset());
	}
	if (Matches.Num() > 1)
	{
		TArray<FString> Paths;
		for (const FAssetData& M : Matches)
		{
			Paths.Add(M.PackageName.ToString());
		}
		OutWarnings.Add(FString::Printf(TEXT("Slot %s: %d materials named %s (%s); put the one to use in %s"), *Slot.ToString(), Matches.Num(), *Name,
			*FString::Join(Paths, TEXT(", ")), *R.MaterialFolder));
		OutHow = TEXT("ambiguous");
		return UMaterial::GetDefaultMaterial(MD_Surface);
	}
	if (R.bCreate)
	{
		const TSharedPtr<FJsonObject>* Inputs = R.Inputs.Find(Slot);
		const TSharedPtr<FJsonObject> In = Inputs ? *Inputs : nullptr;
		UMaterialInterface* Created = CreateStylisedInstance(Name, R, In, OutCreated, OutWarnings);
		if (!Created)
		{
			Created = CreateMaterial(Name, R, In, OutCreated, OutWarnings);
		}
		if (Created)
		{
			OutHow = TEXT("created");
			return Created;
		}
	}
	OutHow = TEXT("none");
	return UMaterial::GetDefaultMaterial(MD_Surface);
}
} // namespace

bool LilaLink::ParseMaterialRequest(const TSharedPtr<FJsonObject>& Params, const TSharedPtr<FJsonObject>& Options, const FString& AssetFolder,
	FMaterialRequest& Out, FString& OutError)
{
	TRACE_CPUPROFILER_EVENT_SCOPE(LilaLinkMaterials::ParseMaterialRequest);

	Out.AssetFolder = AssetFolder;
	Options->TryGetBoolField(TEXT("importMaterials"), Out.bCreate);
	Options->TryGetBoolField(TEXT("importTextures"), Out.bImportTextures);
	Options->TryGetBoolField(TEXT("keepMaterials"), Out.bKeep);
	Options->TryGetBoolField(TEXT("rebuildMaterial"), Out.bRebuildMaterial);

	FString Folder;
	Options->TryGetStringField(TEXT("materialFolder"), Folder);
	Folder.TrimStartAndEndInline();
	while (Folder.EndsWith(TEXT("/")))
	{
		Folder.LeftChopInline(1);
	}
	Out.MaterialFolder = Folder.IsEmpty() ? AssetFolder / TEXT("Materials") : Folder.StartsWith(TEXT("/")) ? Folder : AssetFolder / Folder;
	FText Reason;
	if (!FPackageName::IsValidLongPackageName(Out.MaterialFolder / TEXT("M"), false, &Reason))
	{
		OutError = FString::Printf(TEXT("importAsset: bad materialFolder %s: %s"), *Out.MaterialFolder, *Reason.ToString());
		return false;
	}

	const TArray<TSharedPtr<FJsonValue>>* Materials = nullptr;
	Out.bManaged = Params->TryGetArrayField(TEXT("materials"), Materials);
	if (Materials)
	{
		for (const TSharedPtr<FJsonValue>& V : *Materials)
		{
			const TSharedPtr<FJsonObject>* M = nullptr;
			FString Name;
			if (V.IsValid() && V->TryGetObject(M) && (*M)->TryGetStringField(TEXT("name"), Name) && !Name.IsEmpty())
			{
				Out.Inputs.Add(FName(*Name), *M);
			}
		}
	}
	return true;
}

void LilaLink::SnapshotMaterials(UObject* Asset, FMaterialRequest& Request)
{
	for (const FSlotView& S : SlotsOf(Asset))
	{
		if (*S.Material)
		{
			Request.Previous.Add(S.Name, FSoftObjectPath(S.Material->Get()));
		}
	}
}

void LilaLink::ApplyMaterials(const FMaterialRequest& Request, const TArray<FString>& ImportedAssets, TArray<FString>& OutCreated,
	TArray<FString>& OutWarnings, TArray<FSlotResult>& OutSlots)
{
	TRACE_CPUPROFILER_EVENT_SCOPE(LilaLink::ApplyMaterials);

	FProjectMaterials Project;
	for (const FString& Path : ImportedAssets)
	{
		UObject* Asset = FSoftObjectPath(Path).ResolveObject();
		TArray<FSlotView> Slots = SlotsOf(Asset);
		if (Slots.IsEmpty())
		{
			continue;
		}
		bool bChanged = false;
		for (const FSlotView& S : Slots)
		{
			FString How;
			UMaterialInterface* Material = Resolve(S.Name, Request, Project, How, OutCreated, OutWarnings);
			if (S.Material->Get() != Material)
			{
				if (!bChanged)
				{
					Asset->Modify();
					bChanged = true;
				}
				*S.Material = Material;
			}
			OutSlots.Add({Asset->GetPathName(), S.Name, Material ? Material->GetPathName() : FString(), How});
			UE_LOG(LogLilaLink, Log, TEXT("%s slot %s -> %s (%s)"), *Asset->GetName(), *S.Name.ToString(), Material ? *Material->GetPathName() : TEXT("none"), *How);
		}
		if (bChanged)
		{
			Asset->PostEditChange();
			Asset->MarkPackageDirty();
		}
	}
}

TArray<UMaterialInterface*> LilaLink::ResolveSlotMaterials(const FMaterialRequest& Request, const FString& Asset, const TArray<FName>& Slots,
	TArray<FString>& OutCreated, TArray<FString>& OutWarnings, TArray<FSlotResult>& OutSlots)
{
	TRACE_CPUPROFILER_EVENT_SCOPE(LilaLink::ResolveSlotMaterials);

	FProjectMaterials Project;
	TArray<UMaterialInterface*> Out;
	Out.Reserve(Slots.Num());
	for (const FName Slot : Slots)
	{
		FString How;
		UMaterialInterface* Material = Resolve(Slot, Request, Project, How, OutCreated, OutWarnings);
		Out.Add(Material);
		OutSlots.Add({Asset, Slot, Material ? Material->GetPathName() : FString(), How});
		UE_LOG(LogLilaLink, Log, TEXT("%s slot %s -> %s (%s)"), *Asset, *Slot.ToString(), Material ? *Material->GetPathName() : TEXT("none"), *How);
	}
	return Out;
}

UMaterialInterface* LilaLink::StylisedParent(const FString& Model, bool bRebuild, TArray<FString>& OutCreated, TArray<FString>& OutWarnings)
{
	TRACE_CPUPROFILER_EVENT_SCOPE(LilaLinkMaterials::StylisedParent);

	const bool bToon = Model == TEXT("toon");
	if (!bToon && Model != TEXT("painterly"))
	{
		OutWarnings.Add(FString::Printf(TEXT("No stylised master for shading model '%s'"), *Model));
		return nullptr;
	}
	return StylisedMaster(bToon, nullptr, bRebuild, OutCreated, OutWarnings);
}

const TCHAR* LilaLink::MastersFolderPath() { return MastersFolder; }

void LilaLink::KeepGrayscale16(UTexture* Texture)
{
	TRACE_CPUPROFILER_EVENT_SCOPE(LilaLinkMaterials::KeepGrayscale16);

	if (!Texture || Texture->CompressionSettings != TC_Grayscale || Texture->Source.GetFormat() != TSF_RGBA16 || Texture->Source.GetNumBlocks() != 1 ||
		Texture->Source.GetNumLayers() != 1)
	{
		return;
	}
	const int32 Width = Texture->Source.GetSizeX(), Height = Texture->Source.GetSizeY();
	TArray64<uint8> Rgba;
	if (!Texture->Source.GetMipData(Rgba, 0) || Rgba.Num() < int64(Width) * Height * 8)
	{
		return;
	}
	TArray<uint16> Red;
	Red.SetNumUninitialized(Width * Height);
	const uint16* Src = reinterpret_cast<const uint16*>(Rgba.GetData());
	for (int64 I = 0; I < Red.Num(); ++I)
	{
		Red[I] = Src[I * 4];
	}
	Texture->Source.Init(Width, Height, 1, 1, TSF_G16, reinterpret_cast<const uint8*>(Red.GetData()));
}

UTexture* LilaLink::ImportTextureFile(const FString& File, const FString& Folder, bool bColor, bool bNormal, bool bFlipGreen,
	TArray<FString>& OutCreated, TArray<FString>& OutWarnings)
{
	TRACE_CPUPROFILER_EVENT_SCOPE(LilaLinkMaterials::ImportTextureFile);

	const ETextureUse Use = bNormal ? ETextureUse::Normal : bColor ? ETextureUse::Color : ETextureUse::Linear;
	return ImportTexture(File, Folder, Use, bFlipGreen, OutCreated, OutWarnings);
}

void LilaLink::ApplyStylised(UMaterialInstanceConstant* Instance, const TSharedPtr<FJsonObject>& In, TArray<FString>& OutCreated,
	TArray<FString>& OutWarnings)
{
	TRACE_CPUPROFILER_EVENT_SCOPE(LilaLinkMaterials::ApplyStylised);

	const FString Model = String(In, TEXT("shadingModel"));
	const bool bToon = Model == TEXT("toon");
	if (!Instance || (!bToon && Model != TEXT("painterly")))
	{
		return;
	}
	UTexture* Strokes = StylisedStrokes(bToon, In, OutCreated, OutWarnings);
	SetStylisedParameters(Instance, bToon, In, Strokes, false, FString(), OutCreated, OutWarnings);
}
