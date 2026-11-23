#include "LilaLinkGrunge.h"

#include "LilaLinkLog.h"
#include "LilaLinkMaterialFunction.h"
#include "LilaLinkMethods.h"

#include "AssetToolsModule.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Factories/MaterialInstanceConstantFactoryNew.h"
#include "IAssetTools.h"
#include "MaterialEditingLibrary.h"
#include "Materials/Material.h"
#include "Materials/MaterialExpressionCustom.h"
#include "Materials/MaterialExpressionMaterialFunctionCall.h"
#include "Materials/MaterialExpressionTextureCoordinate.h"
#include "Materials/MaterialExpressionVertexColor.h"
#include "Materials/MaterialExpressionWorldPosition.h"
#include "Materials/MaterialFunction.h"
#include "Materials/MaterialInstanceConstant.h"
#include "ProfilingDebugging/CpuProfilerTrace.h"

namespace
{
constexpr int32 Version = 3;
const TCHAR* VersionTag = TEXT("Lila Grunge v");
const TCHAR* InstanceFolder = TEXT("/Game/LilaLink/Grunge");

/** Mask names as the parameters spell them, in Lila's order: edge, cavity, occlusion, dust, streaks, fade. */
const TCHAR* const MaskNames[] = {TEXT("Edge"), TEXT("Cavity"), TEXT("Occlusion"), TEXT("Dust"), TEXT("Streaks"), TEXT("Fade")};
constexpr int32 MaskCount = 6;

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

using FWire = TPair<FString, TPair<UMaterialExpression*, FString>>;

/** A Custom node running Code over named inputs; every one carries the version tag that VersionOf reads. */
UMaterialExpressionCustom* Custom(LilaLink::FFunctionGraph& G, const FString& Description, const FString& Code, ECustomMaterialOutputType Type,
	const TArray<FWire>& Wires, int32 X = -400)
{
	auto* E = G.New<UMaterialExpressionCustom>(X);
	E->Description = FString::Printf(TEXT("%s%d %s"), VersionTag, Version, *Description);
	E->Code = Code;
	E->OutputType = Type;
	E->Inputs.Reset();
	for (const FWire& W : Wires)
	{
		E->Inputs.AddDefaulted_GetRef().InputName = FName(*W.Key);
	}
	for (const FWire& W : Wires)
	{
		UMaterialEditingLibrary::ConnectMaterialExpressions(W.Value.Key, W.Value.Value, E, W.Key);
	}
	return E;
}

FWire Wire(const FString& Name, UMaterialExpression* E, const FString& Output = FString()) { return MakeTuple(Name, MakeTuple(E, Output)); }

/** Value noise over world space, two octaves; the same as Lila's viewport, so a seed breaks up the same way in kind. */
const TCHAR* NoiseCode = TEXT(
	"float3 p = W / max(Scale, 0.01) + Seed;\n"
	"float n = 0.0;\n"
	"[unroll] for (int o = 0; o < 2; ++o) {\n"
	"  float3 q = o == 0 ? p : p * 2.03 + 17.0;\n"
	"  float3 i = floor(q); float3 f = frac(q); f = f * f * (3.0 - 2.0 * f);\n"
	"  float v = 0.0;\n"
	"  [unroll] for (int k = 0; k < 8; ++k) {\n"
	"    float3 c = float3(k & 1, (k >> 1) & 1, (k >> 2) & 1);\n"
	"    float h = frac(sin(dot(i + c, float3(127.1, 311.7, 74.7))) * 43758.5453);\n"
	"    float3 w = lerp(1.0 - f, f, c);\n"
	"    v += h * w.x * w.y * w.z;\n"
	"  }\n"
	"  n += v * (o == 0 ? 0.65 : 0.35);\n"
	"}\n");

/**
 * MF_LilaGrunge: Lila's six grunge masks (edge wear, cavity dirt, occlusion, dust, streaks, ground fade) from vertex colour
 * and a UV set, each remapped as Lila's look does (level, softness, world-noise breakup, bands, hard cut, outline) and laid
 * over a base colour and roughness as a two-stop ramp. Breakup is world-space noise, or a triplanar texture, so no UVs.
 */
void BuildGrunge(UMaterialFunction* Function)
{
	LilaLink::FFunctionGraph G(Function, TEXT("Lila Grunge: edge wear, cavity dirt, occlusion, dust, streaks and ground fade from the masks Lila "
											  "wrote into vertex colour and UV sets, styled per mask (soft to banded toon) and laid over your "
											  "material. Breakup is world-space noise or a triplanar texture: no UVs needed."));
	UMaterialExpression* Base = G.VectorInput(TEXT("BaseColor"), FLinearColor(0.5f, 0.5f, 0.5f), TEXT("Surface"), TEXT("The surface's own colour."));
	UMaterialExpression* BaseRough = G.ScalarInput(TEXT("BaseRoughness"), 0.6f, TEXT("Surface"), TEXT("The surface's own roughness."));
	UMaterialExpression* OutlineColor = G.VectorInput(TEXT("GrungeOutlineColor"), FLinearColor(0.03f, 0.03f, 0.03f), TEXT("Grunge"), TEXT("Colour of every mask's outline."));
	UTexture* White = LoadObject<UTexture>(nullptr, TEXT("/Engine/EngineResources/WhiteSquareTexture.WhiteSquareTexture"));
	UMaterialExpression* NoiseTex = G.TextureInput(TEXT("GrungeNoiseTexture"), White, SAMPLERTYPE_LinearColor, TEXT("Grunge"),
		TEXT("Breakup texture, projected triplanar in world space; used when GrungeUseTexture is 1."));
	UMaterialExpression* UseTex = G.ScalarInput(TEXT("GrungeUseTexture"), 0.0f, TEXT("Grunge"), TEXT("1 breaks masks up with GrungeNoiseTexture instead of noise."));
	UMaterialExpression* TexScale = G.ScalarInput(TEXT("GrungeTextureScale"), 100.0f, TEXT("Grunge"), TEXT("World size of one texture repeat, cm."));

	auto* VC = G.New<UMaterialExpressionVertexColor>(-1900);
	UMaterialExpression* VCDefault = Custom(G, TEXT("vertex colour"), TEXT("return float4(VC, VA);"), CMOT_Float4,
		{Wire(TEXT("VC"), VC), Wire(TEXT("VA"), VC, TEXT("A"))}, -1700);
	UMaterialExpression* MasksA = G.Input(TEXT("GrungeMasksA"), FunctionInput_Vector4, VCDefault,
		TEXT("Edge, cavity, occlusion, streaks: vertex colour RGBA when unconnected."));
	auto* UV3 = G.New<UMaterialExpressionTextureCoordinate>(-1900);
	UV3->CoordinateIndex = 3;
	UMaterialExpression* MasksB = G.Input(TEXT("GrungeMasksB"), FunctionInput_Vector2, UV3, TEXT("Dust, fade: UV channel 3 when unconnected."));
	auto* UV4 = G.New<UMaterialExpressionTextureCoordinate>(-1900);
	UV4->CoordinateIndex = 4;
	auto* UV5 = G.New<UMaterialExpressionTextureCoordinate>(-1900);
	UV5->CoordinateIndex = 5;
	UMaterialExpression* DistDefault = Custom(G, TEXT("edge distances"), TEXT("return float4(U0, U1);"), CMOT_Float4,
		{Wire(TEXT("U0"), UV4), Wire(TEXT("U1"), UV5)}, -1700);
	UMaterialExpression* EdgeDist = G.Input(TEXT("GrungeEdgeDistance"), FunctionInput_Vector4, DistDefault,
		TEXT("Distances, cm, to the face's sharp edge lines: UV channels 4 and 5 when unconnected. Their min is the distance to the nearest "
			 "sharp edge, so edge wear holds on blocks with no vertices between their edges."));
	UMaterialExpression* EdgeWidth = G.ScalarInput(TEXT("EdgeWidth"), 4.0f, TEXT("Edge"), TEXT("How far wear runs from a sharp edge, cm."));
	auto* UV6 = G.New<UMaterialExpressionTextureCoordinate>(-1900);
	UV6->CoordinateIndex = 6;
	auto* UV7 = G.New<UMaterialExpressionTextureCoordinate>(-1900);
	UV7->CoordinateIndex = 7;
	UMaterialExpression* FieldsDefault = Custom(G, TEXT("fields"), TEXT("return float4(U0, U1);"), CMOT_Float4,
		{Wire(TEXT("U0"), UV6), Wire(TEXT("U1"), UV7)}, -1700);
	UMaterialExpression* Fields = G.Input(TEXT("GrungeFields"), FunctionInput_Vector4, FieldsDefault,
		TEXT("Height above the ground, distance to the face's contact line, drop below a streak source and position along it, cm: UV "
			 "channels 6 and 7 when unconnected. Exact per pixel, so dirt, fade and streaks hold on blocks with four corners a face."));
	UMaterialExpression* CavityWidth = G.ScalarInput(TEXT("CavityWidth"), 10.0f, TEXT("Cavity"), TEXT("How far dirt runs from a contact line, cm."));
	UMaterialExpression* AoWidth = G.ScalarInput(TEXT("AoWidth"), 40.0f, TEXT("Occlusion"), TEXT("How far occlusion runs from a contact line, cm."));
	UMaterialExpression* FadeHeight = G.ScalarInput(TEXT("FadeHeight"), 60.0f, TEXT("Fade"), TEXT("How high the ground fade reaches, cm."));
	UMaterialExpression* StreakLength = G.ScalarInput(TEXT("StreakLength"), 150.0f, TEXT("Streaks"), TEXT("How far streaks run down, cm."));
	auto* World = G.New<UMaterialExpressionWorldPosition>(-1900);
	UMaterialExpression* Normal = Custom(G, TEXT("normal"), TEXT("return Parameters.TangentToWorld[2];"), CMOT_Float3, {}, -1700);

	TArray<UMaterialExpression*> Styled;
	TArray<FWire> Composite = {Wire(TEXT("Base"), Base), Wire(TEXT("Rough"), BaseRough), Wire(TEXT("OutlineColor"), OutlineColor)};
	const float Levels[MaskCount] = {0.45f, 0.35f, 0.35f, 0.55f, 0.35f, 0.4f};
	const float Softness[MaskCount] = {0.3f, 0.4f, 0.6f, 0.5f, 0.35f, 0.6f};
	const float Breakup[MaskCount] = {0.6f, 0.6f, 0.0f, 0.5f, 0.8f, 0.5f};
	const float Scale[MaskCount] = {6.0f, 20.0f, 30.0f, 30.0f, 4.0f, 25.0f};
	const float Strength[MaskCount] = {0.45f, 0.6f, 0.35f, 0.2f, 0.35f, 0.45f};
	const float Multiply[MaskCount] = {0.0f, 1.0f, 1.0f, 0.0f, 1.0f, 1.0f};
	const float Rough[MaskCount] = {0.45f, 0.9f, -1.0f, 0.95f, 0.75f, 0.9f};
	const FLinearColor Colors[MaskCount] = {{0.62f, 0.6f, 0.56f}, {0.4f, 0.36f, 0.31f}, {0.6f, 0.6f, 0.6f}, {0.72f, 0.69f, 0.63f}, {0.55f, 0.52f, 0.47f}, {0.55f, 0.5f, 0.41f}};
	const FLinearColor Lows[MaskCount] = {{0.45f, 0.43f, 0.4f}, {0.65f, 0.62f, 0.58f}, {0.8f, 0.8f, 0.8f}, {0.62f, 0.59f, 0.54f}, {0.75f, 0.72f, 0.67f}, {0.75f, 0.71f, 0.63f}};
	// Edge wear from the per-pixel distance to the nearest sharp edge where the face has one, else the vertex value.
	const TCHAR* Raw[MaskCount] = {TEXT("min(min(D.x, D.y), min(D.z, D.w)) < 5000.0 ? saturate(1.0 - max(min(min(D.x, D.y), min(D.z, D.w)), 0.0) / max(EdgeWidth, 1e-3)) : A.x"),
		TEXT("max(F.y < 5000.0 ? saturate(1.0 - max(F.y, 0.0) / max(CavityWidth, 1e-3)) : 0.0, A.y)"),
		TEXT("max(F.y < 5000.0 ? saturate(1.0 - max(F.y, 0.0) / max(AoWidth, 1e-3)) : 0.0, A.z)"), TEXT("B.x"),
		TEXT("max(A.w, (F.z < 5000.0 && F.z >= 0.0 && F.w >= 0.0 && F.w <= 1.0) ? saturate(1.0 - F.z / max(StreakLength, 1e-3)) : 0.0)"),
		TEXT("max(F.x < 5000.0 ? saturate(1.0 - max(F.x, 0.0) / max(FadeHeight, 1e-3)) : 0.0, B.y)")};
	for (int32 K = 0; K < MaskCount; ++K)
	{
		const FString M = MaskNames[K];
		const FString Group = M;
		auto S = [&](const TCHAR* Suffix, float Value, const TCHAR* Doc) { return G.ScalarInput(*(M + Suffix), Value, *Group, Doc); };
		UMaterialExpression* On = S(TEXT("Enabled"), 1.0f, TEXT("1 shows this mask."));
		UMaterialExpression* Level = S(TEXT("Level"), Levels[K], TEXT("Where the mask starts, 0 to 1."));
		UMaterialExpression* Soft = S(TEXT("Softness"), Softness[K], TEXT("Blend width at the level; near 0 is a hard edge."));
		UMaterialExpression* Brk = S(TEXT("Breakup"), Breakup[K], TEXT("How much noise breaks the edge up."));
		UMaterialExpression* Sc = S(TEXT("NoiseScale"), Scale[K], TEXT("Size of the breakup noise, cm."));
		UMaterialExpression* Bands = S(TEXT("Bands"), 0.0f, TEXT("Posterize into this many bands; 0 is smooth."));
		UMaterialExpression* Cut = S(TEXT("Cut"), 0.0f, TEXT("Hard cut: below it nothing, above it full; 0 is off."));
		UMaterialExpression* Outline = S(TEXT("Outline"), 0.0f, TEXT("Outline along the mask's edge, in mask units; 0 is off."));
		UMaterialExpression* Str = S(TEXT("Strength"), Strength[K], TEXT("How strongly the mask lays its colour over the surface."));
		UMaterialExpression* Seed = S(TEXT("Seed"), 0.0f, TEXT("Moves the noise; Lila sets it from the mask's seed."));
		UMaterialExpression* R = S(TEXT("Roughness"), Rough[K], TEXT("Roughness under the mask; below 0 leaves the surface's own."));
		UMaterialExpression* Mul = S(TEXT("Multiply"), Multiply[K], TEXT("1 darkens the surface by the colour, keeping its texture; 0 lays the colour over it."));
		UMaterialExpression* Color = G.VectorInput(*(M + TEXT("Color")), Colors[K], *Group, TEXT("Colour where the mask is full."));
		UMaterialExpression* Low = G.VectorInput(*(M + TEXT("Low")), Lows[K], *Group, TEXT("Colour where the mask just starts."));
		// Streaks stretch their breakup down the wall (Unreal is Z-up), so it reads as drips.
		const FString Code = FString(K == 4 ? TEXT("float3 Wt = W;\nW.z *= 0.1;\n") : TEXT("float3 Wt = W;\n")) + NoiseCode +
			TEXT("if (UseTex > 0.5) {\n"
				 "  float3 bw = pow(abs(N), 4.0); bw /= max(dot(bw, 1.0), 1e-4);\n"
				 "  float3 uvw = Wt / max(TexScale, 0.01);\n"
				 "  n = Texture2DSample(Tex, TexSampler, uvw.yz).r * bw.x + Texture2DSample(Tex, TexSampler, uvw.xz).r * bw.y\n"
				 "    + Texture2DSample(Tex, TexSampler, uvw.xy).r * bw.z;\n"
				 "}\n") +
			FString::Printf(TEXT("float raw = %s;\n"), Raw[K]) +
			TEXT("float v = raw + (n - 0.5) * Breakup * saturate(1.5 - length(fwidth(Wt / max(Scale, 0.01))));\n"
				 "float s = max(max(Softness, 1e-4) * 0.5, fwidth(v));\n"
				 "float m = smoothstep(Level - s, Level + s, v);\n"
				 "if (Bands >= 2.0) m = min(floor(m * Bands), Bands - 1.0) / (Bands - 1.0);\n"
				 "if (Cut > 0.0) m = m >= Cut ? 1.0 : 0.0;\n"
				 "float line = 0.0;\n"
				 "if (Outline > 0.0) line = 1.0 - saturate(abs(v - Level) / max(Outline, max(fwidth(v), 1e-5)));\n"
				 "return On > 0.5 ? float2(m, line) : float2(0.0, 0.0);");
		UMaterialExpression* Mask = Custom(G, M.ToLower(), Code, CMOT_Float2,
			{Wire(TEXT("A"), MasksA), Wire(TEXT("B"), MasksB), Wire(TEXT("D"), EdgeDist), Wire(TEXT("EdgeWidth"), EdgeWidth), Wire(TEXT("F"), Fields),
				Wire(TEXT("CavityWidth"), CavityWidth), Wire(TEXT("AoWidth"), AoWidth), Wire(TEXT("FadeHeight"), FadeHeight), Wire(TEXT("StreakLength"), StreakLength),
				Wire(TEXT("W"), World), Wire(TEXT("N"), Normal), Wire(TEXT("Scale"), Sc),
				Wire(TEXT("Seed"), Seed),
				Wire(TEXT("Tex"), NoiseTex), Wire(TEXT("UseTex"), UseTex), Wire(TEXT("TexScale"), TexScale), Wire(TEXT("Breakup"), Brk),
				Wire(TEXT("Softness"), Soft), Wire(TEXT("Level"), Level), Wire(TEXT("Bands"), Bands), Wire(TEXT("Cut"), Cut),
				Wire(TEXT("Outline"), Outline), Wire(TEXT("On"), On)},
			-800);
		Styled.Add(Mask);
		Composite.Add(Wire(FString::Printf(TEXT("M%d"), K), Mask));
		Composite.Add(Wire(FString::Printf(TEXT("C%d"), K), Color));
		Composite.Add(Wire(FString::Printf(TEXT("L%d"), K), Low));
		Composite.Add(Wire(FString::Printf(TEXT("S%d"), K), Str));
		Composite.Add(Wire(FString::Printf(TEXT("R%d"), K), R));
		Composite.Add(Wire(FString::Printf(TEXT("X%d"), K), Mul));
	}
	FString Layer = TEXT("float3 c = Base; float rt = 0.0; float rw = 0.0;\n");
	for (int32 K = 0; K < MaskCount; ++K)
	{
		Layer += FString::Printf(TEXT("{ float3 col = lerp(L%d, C%d, M%d.x); float w = M%d.x * S%d;\n"
									  "  if (M%d.y > 0.0) { col = lerp(col, OutlineColor, M%d.y); w = max(w, M%d.y * S%d); }\n"
									  "  c = lerp(c, X%d > 0.5 ? c * col : col, w);\n"
									  "  if (R%d >= 0.0 && w > 0.0) { rt = rw > 0.0 ? lerp(rt, R%d, w) : R%d; rw = max(rw, w); } }\n"),
			K, K, K, K, K, K, K, K, K, K, K, K, K);
	}
	Layer += TEXT("return float4(c, lerp(Rough, rt, rw));");
	UMaterialExpression* Result = Custom(G, TEXT("composite"), Layer, CMOT_Float4, Composite, -300);
	UMaterialExpression* Color = Custom(G, TEXT("colour"), TEXT("return X.rgb;"), CMOT_Float3, {Wire(TEXT("X"), Result)}, -150);
	UMaterialExpression* Roughness = Custom(G, TEXT("roughness"), TEXT("return X.a;"), CMOT_Float1, {Wire(TEXT("X"), Result)}, -150);
	G.Output(Color, TEXT("BaseColor"), TEXT("BaseColor with every shown mask laid over it."));
	G.Output(Roughness, TEXT("Roughness"), TEXT("Roughness with the masks' roughness laid over it."));
	for (int32 K = 0; K < MaskCount; ++K)
	{
		UMaterialExpression* Only = Custom(G, FString(MaskNames[K]).ToLower() + TEXT(" out"), TEXT("return X.x;"), CMOT_Float1, {Wire(TEXT("X"), Styled[K])}, -150);
		G.Output(Only, MaskNames[K], *FString::Printf(TEXT("The styled %s mask, 0 to 1, for blending your own layers."), *FString(MaskNames[K]).ToLower()));
	}
	G.Finish();
}

/** The generated material on MF_LilaGrunge; UVBase above 0 feeds the masks and edge distances from five UV sets from that index instead. */
void Master(UMaterial* Material, UMaterialFunction* Function, int32 UVBase, TArray<FString>& OutWarnings)
{
	UMaterialExpressionMaterialFunctionCall* Call = LilaLink::ResetToFunctionCall(Material, Function);
	if (!LilaLink::ConnectFunctionOutputs(Call, {{TEXT("BaseColor"), MP_BaseColor}, {TEXT("Roughness"), MP_Roughness}}))
	{
		OutWarnings.Add(FString::Printf(TEXT("Could not wire the outputs of %s into %s"), *Function->GetName(), *Material->GetName()));
	}
	if (UVBase > 0)
	{
		UMaterialExpressionTextureCoordinate* U[5];
		for (int32 K = 0; K < 5; ++K)
		{
			U[K] = Cast<UMaterialExpressionTextureCoordinate>(
				UMaterialEditingLibrary::CreateMaterialExpression(Material, UMaterialExpressionTextureCoordinate::StaticClass(), -900, K * 150));
			U[K]->CoordinateIndex = UVBase + K;
		}
		auto* Pack = Cast<UMaterialExpressionCustom>(UMaterialEditingLibrary::CreateMaterialExpression(Material, UMaterialExpressionCustom::StaticClass(), -600, 0));
		Pack->Code = TEXT("return float4(U0, U1);");
		Pack->OutputType = CMOT_Float4;
		Pack->Inputs.Reset();
		Pack->Inputs.AddDefaulted_GetRef().InputName = TEXT("U0");
		Pack->Inputs.AddDefaulted_GetRef().InputName = TEXT("U1");
		UMaterialEditingLibrary::ConnectMaterialExpressions(U[0], TEXT(""), Pack, TEXT("U0"));
		UMaterialEditingLibrary::ConnectMaterialExpressions(U[1], TEXT(""), Pack, TEXT("U1"));
		UMaterialEditingLibrary::ConnectMaterialExpressions(Pack, TEXT(""), Call, TEXT("GrungeMasksA"));
		UMaterialEditingLibrary::ConnectMaterialExpressions(U[2], TEXT(""), Call, TEXT("GrungeMasksB"));
		auto* Dist = Cast<UMaterialExpressionCustom>(UMaterialEditingLibrary::CreateMaterialExpression(Material, UMaterialExpressionCustom::StaticClass(), -600, 300));
		Dist->Code = TEXT("return float4(U3, U4);");
		Dist->OutputType = CMOT_Float4;
		Dist->Inputs.Reset();
		Dist->Inputs.AddDefaulted_GetRef().InputName = TEXT("U3");
		Dist->Inputs.AddDefaulted_GetRef().InputName = TEXT("U4");
		UMaterialEditingLibrary::ConnectMaterialExpressions(U[3], TEXT(""), Dist, TEXT("U3"));
		UMaterialEditingLibrary::ConnectMaterialExpressions(U[4], TEXT(""), Dist, TEXT("U4"));
		UMaterialEditingLibrary::ConnectMaterialExpressions(Dist, TEXT(""), Call, TEXT("GrungeEdgeDistance"));
		// The UV layout has no channels left for the per-pixel fields: far values make the masks use their vertex values.
		auto* Far = Cast<UMaterialExpressionCustom>(UMaterialEditingLibrary::CreateMaterialExpression(Material, UMaterialExpressionCustom::StaticClass(), -600, 600));
		Far->Code = TEXT("return float4(10000.0, 10000.0, 10000.0, 0.0);");
		Far->OutputType = CMOT_Float4;
		Far->Inputs.Reset();
		UMaterialEditingLibrary::ConnectMaterialExpressions(Far, TEXT(""), Call, TEXT("GrungeFields"));
	}
	Material->PostEditChange();
	Material->MarkPackageDirty();
}

UMaterialInstanceConstant* InstanceFor(const FString& Name, UMaterialInterface* Parent, TArray<FString>& OutCreated, TArray<FString>& OutWarnings)
{
	const FString Path = FString(InstanceFolder) + TEXT("/") + Name + TEXT(".") + Name;
	UMaterialInstanceConstant* Instance = LoadObject<UMaterialInstanceConstant>(nullptr, *Path, nullptr, LOAD_NoWarn | LOAD_Quiet);
	if (!Instance)
	{
		UMaterialInstanceConstantFactoryNew* Factory = NewObject<UMaterialInstanceConstantFactoryNew>(GetTransientPackage());
		Factory->InitialParent = Parent;
		Instance = Cast<UMaterialInstanceConstant>(
			FAssetToolsModule::GetModule().Get().CreateAsset(Name, InstanceFolder, UMaterialInstanceConstant::StaticClass(), Factory));
	}
	if (!Instance)
	{
		OutWarnings.Add(FString::Printf(TEXT("Could not create material instance %s in %s"), *Name, InstanceFolder));
		return nullptr;
	}
	if (Instance->Parent != Parent)
	{
		UMaterialEditingLibrary::SetMaterialInstanceParent(Instance, Parent);
	}
	OutCreated.AddUnique(Instance->GetPathName());
	return Instance;
}

/**
 * grungeMaterials {regenerate?, instances?: [{name, layout: color|uv, scalars {name: v}, vectors {name: [r, g, b]}}]}:
 * builds what is missing or older, then one instance per entry. Returns {function, materials, instances, created, warnings}.
 */
TSharedPtr<FJsonValue> GrungeMaterials(const TSharedPtr<FJsonObject>& Params, FString& OutError)
{
	TRACE_CPUPROFILER_EVENT_SCOPE(LilaLinkGrunge::GrungeMaterials);

	bool bRegenerate = false;
	Params->TryGetBoolField(TEXT("regenerate"), bRegenerate);
	TArray<FString> Created, Warnings;
	FString Error;
	const LilaLink::FMaterialPair Pair = LilaLink::FindOrCreateMaterialPair(LilaLink::MastersFolderPath(), TEXT("MF_LilaGrunge"), TEXT("M_LilaGrunge"), Error);
	if (!Pair.Material)
	{
		OutError = TEXT("grungeMaterials: ") + Error;
		return nullptr;
	}
	bool bUvCreated = false;
	UMaterial* UvMaterial = LilaLink::FindOrCreateMaterial(LilaLink::MastersFolderPath(), TEXT("M_LilaGrungeUV"), bUvCreated);
	const bool bStale = VersionOf(Pair.Function) < Version;
	if (Pair.bCreated || bRegenerate || bStale)
	{
		BuildGrunge(Pair.Function);
		Created.AddUnique(Pair.Function->GetPathName());
	}
	if (Pair.bCreated || bRegenerate || bStale)
	{
		Master(Pair.Material, Pair.Function, 0, Warnings);
		Created.AddUnique(Pair.Material->GetPathName());
	}
	if (UvMaterial && (bUvCreated || bRegenerate || bStale))
	{
		Master(UvMaterial, Pair.Function, 3, Warnings);
		Created.AddUnique(UvMaterial->GetPathName());
	}

	TSharedRef<FJsonObject> Instances = MakeShared<FJsonObject>();
	const TArray<TSharedPtr<FJsonValue>>* List = nullptr;
	if (Params->TryGetArrayField(TEXT("instances"), List))
	{
		for (const TSharedPtr<FJsonValue>& V : *List)
		{
			const TSharedPtr<FJsonObject> In = V->AsObject();
			if (!In.IsValid())
			{
				continue;
			}
			const FString Name = In->GetStringField(TEXT("name"));
			UMaterialInterface* Parent = In->GetStringField(TEXT("layout")) == TEXT("uv") && UvMaterial ? Cast<UMaterialInterface>(UvMaterial) : Pair.Material;
			UMaterialInstanceConstant* Instance = InstanceFor(Name, Parent, Created, Warnings);
			if (!Instance)
			{
				continue;
			}
			const TSharedPtr<FJsonObject>* Scalars = nullptr;
			if (In->TryGetObjectField(TEXT("scalars"), Scalars))
			{
				for (const auto& [Key, Value] : (*Scalars)->Values)
				{
					UMaterialEditingLibrary::SetMaterialInstanceScalarParameterValue(Instance, FName(*Key), float(Value->AsNumber()));
				}
			}
			const TSharedPtr<FJsonObject>* Vectors = nullptr;
			if (In->TryGetObjectField(TEXT("vectors"), Vectors))
			{
				for (const auto& [Key, Value] : (*Vectors)->Values)
				{
					const TArray<TSharedPtr<FJsonValue>>& C = Value->AsArray();
					if (C.Num() >= 3)
					{
						UMaterialEditingLibrary::SetMaterialInstanceVectorParameterValue(Instance, FName(*Key),
							FLinearColor(float(C[0]->AsNumber()), float(C[1]->AsNumber()), float(C[2]->AsNumber())));
					}
				}
			}
			UMaterialEditingLibrary::UpdateMaterialInstance(Instance);
			Instances->SetStringField(Name, Instance->GetPathName());
		}
	}
	TArray<FString> SaveErrors;
	LilaLink::SaveAssetPaths(Created, SaveErrors);
	Warnings.Append(SaveErrors);
	TSharedRef<FJsonObject> Out = MakeShared<FJsonObject>();
	Out->SetStringField(TEXT("function"), Pair.Function->GetPathName());
	TSharedRef<FJsonObject> Materials = MakeShared<FJsonObject>();
	Materials->SetStringField(TEXT("color"), Pair.Material->GetPathName());
	if (UvMaterial)
	{
		Materials->SetStringField(TEXT("uv"), UvMaterial->GetPathName());
	}
	Out->SetObjectField(TEXT("materials"), Materials);
	Out->SetObjectField(TEXT("instances"), Instances);
	TArray<TSharedPtr<FJsonValue>> C, W;
	for (const FString& P : Created)
	{
		C.Add(Str(P));
	}
	for (const FString& P : Warnings)
	{
		W.Add(Str(P));
		UE_LOG(LogLilaLink, Warning, TEXT("grungeMaterials: %s"), *P);
	}
	Out->SetArrayField(TEXT("created"), C);
	Out->SetArrayField(TEXT("warnings"), W);
	return Obj(Out);
}
} // namespace

void LilaLink::AddGrungeMethods(TMap<FString, FLilaLinkMethod>& Methods)
{
	Methods.Add(TEXT("grungeMaterials"), LilaLink::Sync(&GrungeMaterials));
}
