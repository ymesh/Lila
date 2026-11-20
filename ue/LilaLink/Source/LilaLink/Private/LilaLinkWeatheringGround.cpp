#include "LilaLinkWeatheringGround.h"

#include "LilaLinkLog.h"
#include "LilaLinkMaterialFunction.h"
#include "LilaLinkMethods.h"

#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "MaterialEditingLibrary.h"
#include "Materials/Material.h"
#include "Materials/MaterialExpressionCustom.h"
#include "Materials/MaterialExpressionMaterialFunctionCall.h"
#include "Materials/MaterialExpressionVertexColor.h"
#include "Materials/MaterialFunction.h"
#include "ProfilingDebugging/CpuProfilerTrace.h"

namespace
{
constexpr int32 GroundVersion = 1;
const TCHAR* GroundVersionTag = TEXT("Lila Ground Tint v");

int32 GroundVersionOf(const UMaterialFunction* Function)
{
	for (UMaterialExpression* E : Function->GetExpressions())
	{
		if (const UMaterialExpressionCustom* C = Cast<UMaterialExpressionCustom>(E); C && C->Description.StartsWith(GroundVersionTag))
		{
			return FCString::Atoi(*C->Description.RightChop(FCString::Strlen(GroundVersionTag)));
		}
	}
	return 0;
}

UMaterialExpressionCustom* GroundCustom(LilaLink::FFunctionGraph& G, const FString& Description, const FString& Code, ECustomMaterialOutputType Type,
	std::initializer_list<TPair<const TCHAR*, TPair<UMaterialExpression*, const TCHAR*>>> Wires, int32 X = -400)
{
	auto* E = G.New<UMaterialExpressionCustom>(X);
	E->Description = FString::Printf(TEXT("%s%d %s"), GroundVersionTag, GroundVersion, *Description);
	E->Code = Code;
	E->OutputType = Type;
	E->Inputs.Reset();
	for (const auto& W : Wires)
	{
		E->Inputs.AddDefaulted_GetRef().InputName = W.Key;
	}
	for (const auto& W : Wires)
	{
		UMaterialEditingLibrary::ConnectMaterialExpressions(W.Value.Key, W.Value.Value, E, W.Key);
	}
	return E;
}

TPair<UMaterialExpression*, const TCHAR*> GroundPin(UMaterialExpression* E, const TCHAR* Output = TEXT("")) { return {E, Output}; }

/**
 * MF_LilaGroundTint: vertex colour R (tint) darkens and shifts the ground toward a grime colour; G (wet) darkens it further
 * and drops roughness toward a wet value. Bands > 1 steps both into hard bands for a toon look.
 */
void BuildGroundTint(UMaterialFunction* Function)
{
	LilaLink::FFunctionGraph G(Function, TEXT("Lila Ground Tint: darkens a ground round walls and scattered pieces and wets its dips, from the "
											  "tint (R) and wetness (G) Lila's Ground Tint modifier wrote into vertex colour. Drop it between your "
											  "ground's colour and roughness and the material outputs."));
	UMaterialExpression* Base = G.VectorInput(TEXT("BaseColor"), FLinearColor(0.3f, 0.28f, 0.25f), TEXT("Ground"), TEXT("The ground's own colour."));
	UMaterialExpression* BaseRough = G.ScalarInput(TEXT("BaseRoughness"), 0.9f, TEXT("Ground"), TEXT("The ground's own roughness."));
	UMaterialExpression* TintColor = G.VectorInput(TEXT("TintColor"), FLinearColor(0.08f, 0.07f, 0.05f), TEXT("Tint"), TEXT("Grime colour at full tint."));
	UMaterialExpression* TintStrength = G.ScalarInput(TEXT("TintStrength"), 1.0f, TEXT("Tint"), TEXT("Scales the tint."));
	UMaterialExpression* WetDarken = G.ScalarInput(TEXT("WetDarken"), 0.45f, TEXT("Wet"), TEXT("How much darker fully wet ground gets, 0 to 1."));
	UMaterialExpression* WetRough = G.ScalarInput(TEXT("WetRoughness"), 0.08f, TEXT("Wet"), TEXT("Roughness of fully wet ground."));
	UMaterialExpression* Bands = G.ScalarInput(TEXT("Bands"), 0.0f, TEXT("Style"), TEXT("0 smooth; 2 or more steps tint and wetness into hard bands (toon)."));
	auto* VC = G.New<UMaterialExpressionVertexColor>(-1700);

	UMaterialExpression* Masks = GroundCustom(G, TEXT("masks"),
		TEXT("float t = saturate(VC.r * Strength);\n"
			 "float w = saturate(VC.g);\n"
			 "if (Bands > 1.5) { t = ceil(t * Bands - 1e-3) / Bands; w = w > 0.5 ? 1.0 : 0.0; }\n"
			 "return float2(t, w);"),
		CMOT_Float2, {{TEXT("VC"), GroundPin(VC)}, {TEXT("Strength"), GroundPin(TintStrength)}, {TEXT("Bands"), GroundPin(Bands)}}, -1100);
	UMaterialExpression* Color = GroundCustom(G, TEXT("colour"), TEXT("float3 c = lerp(A, Tint, M.x);\nreturn c * (1.0 - M.y * Darken);"), CMOT_Float3,
		{{TEXT("A"), GroundPin(Base)}, {TEXT("Tint"), GroundPin(TintColor)}, {TEXT("M"), GroundPin(Masks)}, {TEXT("Darken"), GroundPin(WetDarken)}}, -150);
	UMaterialExpression* Rough = GroundCustom(G, TEXT("roughness"), TEXT("return lerp(A, Wet, M.y);"), CMOT_Float1,
		{{TEXT("A"), GroundPin(BaseRough)}, {TEXT("Wet"), GroundPin(WetRough)}, {TEXT("M"), GroundPin(Masks)}}, -150);
	UMaterialExpression* Tint = GroundCustom(G, TEXT("tint"), TEXT("return M.x;"), CMOT_Float1, {{TEXT("M"), GroundPin(Masks)}}, -150);
	UMaterialExpression* Wet = GroundCustom(G, TEXT("wet"), TEXT("return M.y;"), CMOT_Float1, {{TEXT("M"), GroundPin(Masks)}}, -150);
	G.Output(Color, TEXT("BaseColor"), TEXT("BaseColor tinted and darkened where wet."));
	G.Output(Rough, TEXT("Roughness"), TEXT("Roughness, glossier where wet."));
	G.Output(Tint, TEXT("Tint"), TEXT("Tint, 0 to 1, for your own blends."));
	G.Output(Wet, TEXT("Wet"), TEXT("Wetness, 0 to 1, for your own blends (puddle normals, ripples)."));
	G.Finish();
}

/** groundTintMaterials {regenerate?}: builds MF_LilaGroundTint and M_LilaGroundTint when missing or older; returns {function, material, created, warnings}. */
TSharedPtr<FJsonValue> GroundTintMaterials(const TSharedPtr<FJsonObject>& Params, FString& OutError)
{
	TRACE_CPUPROFILER_EVENT_SCOPE(LilaLinkWeatheringGround::GroundTintMaterials);

	bool bRegenerate = false;
	Params->TryGetBoolField(TEXT("regenerate"), bRegenerate);
	TArray<FString> Created, Warnings;
	FString Error;
	const LilaLink::FMaterialPair Pair = LilaLink::FindOrCreateMaterialPair(LilaLink::MastersFolderPath(), TEXT("MF_LilaGroundTint"), TEXT("M_LilaGroundTint"), Error);
	if (!Pair.Material)
	{
		OutError = Error;
		return nullptr;
	}
	if (Pair.bCreated || bRegenerate || GroundVersionOf(Pair.Function) < GroundVersion)
	{
		BuildGroundTint(Pair.Function);
		UMaterialExpressionMaterialFunctionCall* Call = LilaLink::ResetToFunctionCall(Pair.Material, Pair.Function);
		if (!LilaLink::ConnectFunctionOutputs(Call, {{TEXT("BaseColor"), MP_BaseColor}, {TEXT("Roughness"), MP_Roughness}}))
		{
			Warnings.Add(TEXT("Could not wire MF_LilaGroundTint into M_LilaGroundTint"));
		}
		Pair.Material->PostEditChange();
		Pair.Material->MarkPackageDirty();
		Created.Add(Pair.Function->GetPathName());
		Created.Add(Pair.Material->GetPathName());
	}
	TArray<FString> SaveErrors;
	LilaLink::SaveAssetPaths(Created, SaveErrors);
	Warnings.Append(SaveErrors);
	TSharedRef<FJsonObject> Out = MakeShared<FJsonObject>();
	Out->SetStringField(TEXT("function"), Pair.Function->GetPathName());
	Out->SetStringField(TEXT("material"), Pair.Material->GetPathName());
	TArray<TSharedPtr<FJsonValue>> CreatedJson, WarningsJson;
	for (const FString& S : Created)
	{
		CreatedJson.Add(MakeShared<FJsonValueString>(S));
	}
	for (const FString& S : Warnings)
	{
		WarningsJson.Add(MakeShared<FJsonValueString>(S));
	}
	Out->SetArrayField(TEXT("created"), CreatedJson);
	Out->SetArrayField(TEXT("warnings"), WarningsJson);
	return MakeShared<FJsonValueObject>(Out);
}
} // namespace

void LilaLink::AddWeatheringGroundMethods(TMap<FString, FLilaLinkMethod>& Methods)
{
	Methods.Add(TEXT("groundTintMaterials"), LilaLink::Sync(&GroundTintMaterials));
}
