#include "LilaLinkPanels.h"

#include "LilaLinkLog.h"
#include "LilaLinkMaterialFunction.h"
#include "LilaLinkMethods.h"

#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "MaterialEditingLibrary.h"
#include "Materials/Material.h"
#include "Materials/MaterialExpressionCustom.h"
#include "Materials/MaterialExpressionMaterialFunctionCall.h"
#include "Materials/MaterialExpressionTextureCoordinate.h"
#include "Materials/MaterialFunction.h"
#include "ProfilingDebugging/CpuProfilerTrace.h"

namespace
{
constexpr int32 Version = 1;
const TCHAR* VersionTag = TEXT("Lila Panel Line v");
const TCHAR* FunctionName = TEXT("MF_LilaPanelLine");

TSharedPtr<FJsonValue> Obj(const TSharedRef<FJsonObject>& O) { return MakeShared<FJsonValueObject>(O); }
TSharedPtr<FJsonValue> Str(const FString& S) { return MakeShared<FJsonValueString>(S); }

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

UMaterialExpressionCustom* Custom(LilaLink::FFunctionGraph& G, const FString& Description, const FString& Code, ECustomMaterialOutputType Type,
	std::initializer_list<TPair<const TCHAR*, UMaterialExpression*>> Wires, int32 X = -400)
{
	auto* E = G.New<UMaterialExpressionCustom>(X);
	E->Description = FString::Printf(TEXT("%s%d %s"), VersionTag, Version, *Description);
	E->Code = Code;
	E->OutputType = Type;
	E->Inputs.Reset();
	for (const auto& W : Wires)
	{
		E->Inputs.AddDefaulted_GetRef().InputName = W.Key;
	}
	for (const auto& W : Wires)
	{
		UMaterialEditingLibrary::ConnectMaterialExpressions(W.Value, TEXT(""), E, W.Key);
	}
	return E;
}

/**
 * MF_LilaPanelLine: a groove across a panel line decal strip from its UVs alone (V runs across the strip, 0 to 1), so it
 * works on any platform with no textures, GBuffer or screen reads: colour darkened toward the groove's floor, a tangent
 * normal tilting into it from both walls, and coverage for masked or translucent strips.
 */
void BuildPanelLine(UMaterialFunction* Function)
{
	LilaLink::FFunctionGraph G(Function, TEXT("Lila Panel Line: the groove of a Lila panel line decal strip, drawn from the strip's UVs: "
											  "darkened colour, a normal into the groove and its coverage. Masked, translucent or in your own "
											  "material over the panels."));
	UMaterialExpression* Tint = G.VectorInput(TEXT("LineColor"), FLinearColor(0.03f, 0.03f, 0.03f), TEXT("Panel Line"),
		TEXT("Colour at the groove's floor."));
	UMaterialExpression* Rough = G.ScalarInput(TEXT("LineRoughness"), 0.7f, TEXT("Panel Line"), TEXT("Roughness in the groove."));
	UMaterialExpression* Width = G.ScalarInput(TEXT("LineWidth"), 0.7f, TEXT("Panel Line"),
		TEXT("How much of the strip's width the groove covers, 0 to 1."));
	UMaterialExpression* Tilt = G.ScalarInput(TEXT("LineNormal"), 0.6f, TEXT("Panel Line"), TEXT("How steeply the groove's walls tilt the normal."));
	UMaterialExpression* Soft = G.ScalarInput(TEXT("LineSoftness"), 0.25f, TEXT("Panel Line"), TEXT("Blur of the groove's edges, 0 sharp to 1 soft."));
	auto* UV0 = G.New<UMaterialExpressionTextureCoordinate>(-1600);
	UMaterialExpression* Uvs = G.Input(TEXT("LineUVs"), FunctionInput_Vector2, UV0, TEXT("The strip's UVs; UV channel 0 when unconnected."));

	// x: 0 at the strip's middle, 1 at the groove's edge.
	const TCHAR* Across = TEXT("float x = abs(UV.y * 2.0 - 1.0) / max(Width, 1e-3);");
	UMaterialExpression* Cover = Custom(G, TEXT("coverage"),
		FString(Across) + TEXT("return saturate((1.0 - x) / max(Soft, 1e-3));"), CMOT_Float1,
		{{TEXT("UV"), Uvs}, {TEXT("Width"), Width}, {TEXT("Soft"), Soft}});
	UMaterialExpression* Color = Custom(G, TEXT("colour"),
		FString(Across) + TEXT("return lerp(Tint * 2.0, Tint, saturate(1.0 - x));"), CMOT_Float3,
		{{TEXT("UV"), Uvs}, {TEXT("Width"), Width}, {TEXT("Tint"), Tint}});
	UMaterialExpression* Normal = Custom(G, TEXT("normal"),
		FString(Across) + TEXT("float side = UV.y < 0.5 ? -1.0 : 1.0; float wall = x < 1.0 ? Tilt * side : 0.0; return normalize(float3(0.0, wall, 1.0));"),
		CMOT_Float3, {{TEXT("UV"), Uvs}, {TEXT("Width"), Width}, {TEXT("Tilt"), Tilt}});
	G.Output(Color, TEXT("BaseColor"), TEXT("Groove colour."));
	G.Output(Rough, TEXT("Roughness"), TEXT("Groove roughness."));
	G.Output(Normal, TEXT("Normal"), TEXT("Tangent-space normal into the groove."));
	G.Output(Cover, TEXT("Opacity"), TEXT("Coverage: to Opacity Mask (masked) or Opacity (translucent)."));
	G.Finish();
}

void Wire(UMaterial* Material, UMaterialFunction* Function, std::initializer_list<TPair<const TCHAR*, EMaterialProperty>> Outputs, TArray<FString>& OutWarnings)
{
	UMaterialExpressionMaterialFunctionCall* Call = LilaLink::ResetToFunctionCall(Material, Function);
	if (!LilaLink::ConnectFunctionOutputs(Call, Outputs))
	{
		OutWarnings.Add(FString::Printf(TEXT("Could not wire the outputs of %s into %s"), *Function->GetName(), *Material->GetName()));
	}
	Material->PostEditChange();
	Material->MarkPackageDirty();
}

struct FSpec
{
	const TCHAR* Key;
	const TCHAR* MaterialName;
	TFunction<void(UMaterial*, UMaterialFunction*, TArray<FString>&)> Master;
};

TArray<FSpec> Specs()
{
	TArray<FSpec> S;
	S.Add({TEXT("panelLine"), TEXT("M_LilaPanelLine"), [](UMaterial* M, UMaterialFunction* F, TArray<FString>& W) {
			   M->BlendMode = BLEND_Masked;
			   M->OpacityMaskClipValue = 0.4f;
			   Wire(M, F, {{TEXT("BaseColor"), MP_BaseColor}, {TEXT("Roughness"), MP_Roughness}, {TEXT("Normal"), MP_Normal}, {TEXT("Opacity"), MP_OpacityMask}}, W);
		   }});
	S.Add({TEXT("panelLineTranslucent"), TEXT("M_LilaPanelLineTranslucent"), [](UMaterial* M, UMaterialFunction* F, TArray<FString>& W) {
			   M->BlendMode = BLEND_Translucent;
			   Wire(M, F, {{TEXT("BaseColor"), MP_BaseColor}, {TEXT("Roughness"), MP_Roughness}, {TEXT("Opacity"), MP_Opacity}}, W);
		   }});
	return S;
}

/** panelMaterials {which?: [keys], regenerate?}: builds what is missing or older, returns {function, materials, created, warnings}. */
TSharedPtr<FJsonValue> PanelMaterials(const TSharedPtr<FJsonObject>& Params, FString& OutError)
{
	TRACE_CPUPROFILER_EVENT_SCOPE(LilaLinkPanels::PanelMaterials);

	bool bRegenerate = false;
	Params->TryGetBoolField(TEXT("regenerate"), bRegenerate);
	TSet<FString> Which;
	const TArray<TSharedPtr<FJsonValue>>* List = nullptr;
	if (Params->TryGetArrayField(TEXT("which"), List))
	{
		for (const TSharedPtr<FJsonValue>& V : *List)
		{
			Which.Add(V->AsString());
		}
	}
	TArray<FString> Created, Warnings;
	bool bBuilt = false;
	FString FunctionPath;
	TSharedRef<FJsonObject> Materials = MakeShared<FJsonObject>();
	for (const FSpec& S : Specs())
	{
		if (!Which.IsEmpty() && !Which.Contains(S.Key))
		{
			continue;
		}
		FString Error;
		const LilaLink::FMaterialPair Pair = LilaLink::FindOrCreateMaterialPair(LilaLink::MastersFolderPath(), FunctionName, S.MaterialName, Error);
		if (!Pair.Material)
		{
			Warnings.Add(Error);
			continue;
		}
		const bool bStale = VersionOf(Pair.Function) < Version;
		if (Pair.bCreated || bRegenerate || bStale)
		{
			if (!bBuilt)
			{
				BuildPanelLine(Pair.Function);
				bBuilt = true;
			}
			S.Master(Pair.Material, Pair.Function, Warnings);
			Created.AddUnique(Pair.Function->GetPathName());
			Created.AddUnique(Pair.Material->GetPathName());
		}
		FunctionPath = Pair.Function->GetPathName();
		Materials->SetStringField(S.Key, Pair.Material->GetPathName());
	}
	TArray<FString> SaveErrors;
	LilaLink::SaveAssetPaths(Created, SaveErrors);
	Warnings.Append(SaveErrors);
	TSharedRef<FJsonObject> Out = MakeShared<FJsonObject>();
	Out->SetStringField(TEXT("function"), FunctionPath);
	Out->SetObjectField(TEXT("materials"), Materials);
	TArray<TSharedPtr<FJsonValue>> C, W;
	for (const FString& P : Created)
	{
		C.Add(Str(P));
	}
	for (const FString& P : Warnings)
	{
		W.Add(Str(P));
		UE_LOG(LogLilaLink, Warning, TEXT("panelMaterials: %s"), *P);
	}
	Out->SetArrayField(TEXT("created"), C);
	Out->SetArrayField(TEXT("warnings"), W);
	return Obj(Out);
}
} // namespace

void LilaLink::AddPanelMethods(TMap<FString, FLilaLinkMethod>& Methods)
{
	Methods.Add(TEXT("panelMaterials"), LilaLink::Sync(&PanelMaterials));
}
