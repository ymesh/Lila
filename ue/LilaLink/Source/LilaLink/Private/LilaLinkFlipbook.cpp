#include "LilaLinkFlipbook.h"

#include "LilaLinkLog.h"
#include "LilaLinkMaterialFunction.h"
#include "LilaLinkMethods.h"

#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "MaterialEditingLibrary.h"
#include "Materials/Material.h"
#include "Materials/MaterialExpressionComponentMask.h"
#include "Materials/MaterialExpressionCustom.h"
#include "Materials/MaterialExpressionMaterialFunctionCall.h"
#include "Materials/MaterialExpressionTextureCoordinate.h"
#include "Materials/MaterialExpressionTime.h"
#include "Materials/MaterialFunction.h"
#include "ProfilingDebugging/CpuProfilerTrace.h"

namespace
{
constexpr int32 Version = 1;
const TCHAR* VersionTag = TEXT("Lila Flipbook v");
const TCHAR* FunctionName = TEXT("MF_LilaFlipbook");

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

/**
 * MF_LilaFlipbook: plays a sheet Lila's texExportFlipbook wrote (frames left to right, then top to bottom), with the
 * settings an Image Sequence node has: frame rate, an offset in frames, how many frames play, what happens after the end
 * and a crossfade between neighbouring frames. Frame overrides time, for driving the playhead from a parameter or a curve.
 */
void BuildFlipbook(UMaterialFunction* Function)
{
	LilaLink::FFunctionGraph G(Function, TEXT("Lila Flipbook: plays a flipbook sheet exported from Lila (texExportFlipbook), frames left to "
											  "right then top to bottom. Set Columns, Rows and FPS to what the export reported."));
	UTexture* Black = LoadObject<UTexture>(nullptr, TEXT("/Engine/EngineResources/Black.Black"));
	UMaterialExpression* Sheet = G.TextureInput(TEXT("Flipbook"), Black, SAMPLERTYPE_Color, TEXT("Flipbook"), TEXT("The sheet Lila exported."));
	UMaterialExpression* Columns = G.ScalarInput(TEXT("Columns"), 4.0f, TEXT("Flipbook"), TEXT("Frames per row of the sheet."));
	UMaterialExpression* Rows = G.ScalarInput(TEXT("Rows"), 4.0f, TEXT("Flipbook"), TEXT("Rows of the sheet."));
	UMaterialExpression* Frames = G.ScalarInput(TEXT("Frames"), 0.0f, TEXT("Flipbook"),
		TEXT("Frames that play; 0 plays every cell (Columns x Rows). Lower it when the last row isn't full."));
	UMaterialExpression* Fps = G.ScalarInput(TEXT("FPS"), 30.0f, TEXT("Flipbook"), TEXT("Frames per second; negative plays backwards."));
	UMaterialExpression* Offset = G.ScalarInput(TEXT("FrameOffset"), 0.0f, TEXT("Flipbook"), TEXT("Frames into the flipbook at time 0."));
	UMaterialExpression* After = G.ScalarInput(TEXT("AfterEnd"), 0.0f, TEXT("Flipbook"), TEXT("0 loops, 1 holds the end frame, 2 plays back and forth."));
	UMaterialExpression* Blend = G.ScalarInput(TEXT("Blend"), 0.0f, TEXT("Flipbook"),
		TEXT("1 crossfades into the next frame, for flipbooks played slower than they were made; 0 holds each frame."));
	UMaterialExpression* Frame = G.ScalarInput(TEXT("Frame"), -1.0f, TEXT("Flipbook"),
		TEXT("The frame to show, fractions included, instead of time: a parameter or curve drives the playhead. Below 0 plays over time."));
	auto* UV0 = G.New<UMaterialExpressionTextureCoordinate>(-1600);
	UMaterialExpression* Uvs = G.Input(TEXT("UVs"), FunctionInput_Vector2, UV0, TEXT("UVs of one frame, 0 to 1; UV channel 0 when unconnected."));
	UMaterialExpression* Time = G.Make(UMaterialExpressionTime::StaticClass(), -1600);

	// Gradients come from the frame's own UVs, so the jump between cells never picks a blurry mip along cell borders.
	const TCHAR* Code = TEXT(R"(
float fbCount = Frames > 0.5 ? Frames : Columns * Rows;
fbCount = max(fbCount, 1.0);
float fbPos = Frame >= 0.0 ? Frame : Time * Fps + Offset;
float fbA = floor(fbPos);
float fbB = fbA + 1.0;
float fbMix = Blend > 0.5 ? fbPos - fbA : 0.0;
if (After > 1.5)
{
    float fbPeriod = max(2.0 * (fbCount - 1.0), 1.0);
    fbA = fmod(fmod(fbA, fbPeriod) + fbPeriod, fbPeriod);
    fbB = fmod(fmod(fbB, fbPeriod) + fbPeriod, fbPeriod);
    fbA = fbA < fbCount ? fbA : fbPeriod - fbA;
    fbB = fbB < fbCount ? fbB : fbPeriod - fbB;
}
else if (After > 0.5)
{
    fbA = clamp(fbA, 0.0, fbCount - 1.0);
    fbB = clamp(fbB, 0.0, fbCount - 1.0);
}
else
{
    fbA = fmod(fmod(fbA, fbCount) + fbCount, fbCount);
    fbB = fmod(fmod(fbB, fbCount) + fbCount, fbCount);
}
float2 fbCell = 1.0 / max(float2(Columns, Rows), 1.0);
float2 fbUv = frac(UV);
float2 fbDx = ddx(UV) * fbCell;
float2 fbDy = ddy(UV) * fbCell;
float2 fbUvA = (float2(fmod(fbA, Columns), floor(fbA / Columns)) + fbUv) * fbCell;
float2 fbUvB = (float2(fmod(fbB, Columns), floor(fbB / Columns)) + fbUv) * fbCell;
float4 fbColA = Texture2DSampleGrad(Sheet, SheetSampler, fbUvA, fbDx, fbDy);
float4 fbColB = Texture2DSampleGrad(Sheet, SheetSampler, fbUvB, fbDx, fbDy);
return lerp(fbColA, fbColB, fbMix);
)");
	auto* Custom = G.New<UMaterialExpressionCustom>(-400);
	Custom->Description = FString::Printf(TEXT("%s%d"), VersionTag, Version);
	Custom->Code = Code;
	Custom->OutputType = CMOT_Float4;
	Custom->Inputs.Reset();
	const TPair<const TCHAR*, UMaterialExpression*> Wires[] = {
		{TEXT("Sheet"), Sheet}, {TEXT("UV"), Uvs}, {TEXT("Time"), Time}, {TEXT("Columns"), Columns}, {TEXT("Rows"), Rows}, {TEXT("Frames"), Frames},
		{TEXT("Fps"), Fps}, {TEXT("Offset"), Offset}, {TEXT("After"), After}, {TEXT("Blend"), Blend}, {TEXT("Frame"), Frame}};
	for (const TPair<const TCHAR*, UMaterialExpression*>& W : Wires)
	{
		Custom->Inputs.AddDefaulted_GetRef().InputName = W.Key;
	}
	for (const TPair<const TCHAR*, UMaterialExpression*>& W : Wires)
	{
		UMaterialEditingLibrary::ConnectMaterialExpressions(W.Value, TEXT(""), Custom, W.Key);
	}
	auto* Rgb = G.New<UMaterialExpressionComponentMask>(-150);
	Rgb->R = Rgb->G = Rgb->B = true;
	Rgb->A = false;
	Rgb->Input.Connect(0, Custom);
	auto* Alpha = G.New<UMaterialExpressionComponentMask>(-150);
	Alpha->R = Alpha->G = Alpha->B = false;
	Alpha->A = true;
	Alpha->Input.Connect(0, Custom);
	G.Output(Rgb, TEXT("Color"), TEXT("The frame's colour."));
	G.Output(Alpha, TEXT("Alpha"), TEXT("The frame's alpha: to Opacity or Opacity Mask."));
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
	S.Add({TEXT("flipbook"), TEXT("M_LilaFlipbook"), [](UMaterial* M, UMaterialFunction* F, TArray<FString>& W) {
			   M->BlendMode = BLEND_Opaque;
			   Wire(M, F, {{TEXT("Color"), MP_BaseColor}}, W);
		   }});
	S.Add({TEXT("flipbookTranslucent"), TEXT("M_LilaFlipbookTranslucent"), [](UMaterial* M, UMaterialFunction* F, TArray<FString>& W) {
			   M->BlendMode = BLEND_Translucent;
			   M->SetShadingModel(MSM_Unlit);
			   M->TwoSided = true;
			   Wire(M, F, {{TEXT("Color"), MP_EmissiveColor}, {TEXT("Alpha"), MP_Opacity}}, W);
		   }});
	return S;
}

/** flipbookMaterials {which?: [keys], regenerate?}: builds what is missing or older, returns {function, materials, created, warnings}. */
TSharedPtr<FJsonValue> FlipbookMaterials(const TSharedPtr<FJsonObject>& Params, FString& OutError)
{
	TRACE_CPUPROFILER_EVENT_SCOPE(LilaLinkFlipbook::FlipbookMaterials);

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
				BuildFlipbook(Pair.Function);
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
		UE_LOG(LogLilaLink, Warning, TEXT("flipbookMaterials: %s"), *P);
	}
	Out->SetArrayField(TEXT("created"), C);
	Out->SetArrayField(TEXT("warnings"), W);
	return Obj(Out);
}
} // namespace

void LilaLink::AddFlipbookMethods(TMap<FString, FLilaLinkMethod>& Methods)
{
	Methods.Add(TEXT("flipbookMaterials"), LilaLink::Sync(&FlipbookMaterials));
}
