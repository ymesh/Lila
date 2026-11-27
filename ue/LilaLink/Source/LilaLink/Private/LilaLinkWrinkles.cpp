#include "LilaLinkCloth.h"
#include "LilaLinkMaterialFunction.h"

#include "AssetRegistry/AssetRegistryModule.h"
#include "AssetRegistry/IAssetRegistry.h"
#include "AssetToolsModule.h"
#include "Dom/JsonObject.h"
#include "Engine/Texture2D.h"
#include "Factories/MaterialFactoryNew.h"
#include "Factories/MaterialFunctionFactoryNew.h"
#include "FileHelpers.h"
#include "IAssetTools.h"
#include "MaterialEditingLibrary.h"
#include "Materials/Material.h"
#include "Materials/MaterialExpressionAdd.h"
#include "Materials/MaterialExpressionClamp.h"
#include "Materials/MaterialExpressionComponentMask.h"
#include "Materials/MaterialExpressionConstant.h"
#include "Materials/MaterialExpressionConstant2Vector.h"
#include "Materials/MaterialExpressionConstant3Vector.h"
#include "Materials/MaterialExpressionFunctionInput.h"
#include "Materials/MaterialExpressionFunctionOutput.h"
#include "Materials/MaterialExpressionMaterialFunctionCall.h"
#include "Materials/MaterialExpressionMultiply.h"
#include "Materials/MaterialExpressionNormalize.h"
#include "Materials/MaterialExpressionScalarParameter.h"
#include "Materials/MaterialExpressionSubtract.h"
#include "Materials/MaterialExpressionTextureCoordinate.h"
#include "Materials/MaterialExpressionTextureObject.h"
#include "Materials/MaterialExpressionTextureSample.h"
#include "Materials/MaterialExpressionVectorParameter.h"
#include "Materials/MaterialExpressionVertexColor.h"
#include "Materials/MaterialFunction.h"
#include "UObject/Package.h"

namespace
{
template <class T> T* FindAt(const FString& Folder, const FString& Name)
{
	const FAssetData Data = IAssetRegistry::GetChecked().GetAssetByObjectPath(FSoftObjectPath(Folder / Name + TEXT(".") + Name));
	return Data.IsValid() ? Cast<T>(Data.GetAsset()) : nullptr;
}

/** Expressions in a material function or a material, stacked in a column. */
struct FGraph
{
	UMaterialFunction* Function = nullptr;
	UMaterial* Material = nullptr;
	int32 Row = 0;

	template <class T> T* New(int32 X = -500)
	{
		const int32 Y = (Row++) * 110;
		UMaterialExpression* E = Function ? UMaterialEditingLibrary::CreateMaterialExpressionInFunction(Function, T::StaticClass(), X, Y)
		                                  : UMaterialEditingLibrary::CreateMaterialExpression(Material, T::StaticClass(), X, Y);
		return Cast<T>(E);
	}

	template <class T> T* Binary(UMaterialExpression* A, UMaterialExpression* B)
	{
		T* E = New<T>();
		E->A.Connect(0, A);
		E->B.Connect(0, B);
		return E;
	}

	UMaterialExpression* Vec3(const FLinearColor& V)
	{
		auto* E = New<UMaterialExpressionConstant3Vector>(-900);
		E->Constant = V;
		return E;
	}

	UMaterialExpressionFunctionInput* Input(const TCHAR* Name, EFunctionInputType Type, int32 Priority, const TCHAR* Description,
	                                        UMaterialExpression* Preview = nullptr)
	{
		auto* E = New<UMaterialExpressionFunctionInput>(-1200);
		E->InputName = Name;
		E->InputType = Type;
		E->SortPriority = Priority;
		E->Description = Description;
		if (Preview)
		{
			E->Preview.Connect(0, Preview);
			E->bUsePreviewValueAsDefault = true;
		}
		return E;
	}

	void Output(UMaterialExpression* In, const TCHAR* Name, int32 Priority, const TCHAR* Description)
	{
		auto* E = New<UMaterialExpressionFunctionOutput>(400);
		E->OutputName = Name;
		E->SortPriority = Priority;
		E->Description = Description;
		E->A.Connect(0, In);
	}
};

/**
 * MF_LilaTensionWrinkles: inputs Normal (the surface's own, tangent space; flat by default), Mask (compression along U in
 * R and V in G, 0 to 1: Lila's "wrinkle" colour set, a Deformer Graph attribute or a curve-driven parameter), WrinkleU and
 * WrinkleV (normal maps of folds from squeezing along U and V), UVs, Tiling and Strength. Each wrinkle map fades in with its
 * compression; the result is blended onto Normal by reoriented normal mapping. Outputs Normal and Folds (how much fold
 * there is, 0 to 1, for darkening or roughening the folds).
 */
void BuildFunction(UMaterialFunction* Function, UTexture* WrinkleU, UTexture* WrinkleV)
{
	LilaLink::DeleteAllExpressions(Function);
	Function->Description = TEXT("Lila tension wrinkles: fold normals faded in by compression along U and V, blended onto your normal.");
	Function->bExposeToLibrary = true;
	FGraph G;
	G.Function = Function;

	UMaterialExpression* Flat = G.Vec3(FLinearColor(0.f, 0.f, 1.f));
	auto* Normal = G.Input(TEXT("Normal"), FunctionInput_Vector3, 0, TEXT("The surface's normal, tangent space. Flat when unconnected."), Flat);
	auto* ZeroMask = G.New<UMaterialExpressionConstant2Vector>(-900);
	auto* Mask = G.Input(TEXT("Mask"), FunctionInput_Vector2, 1,
	                     TEXT("Compression along U (R) and V (G), 0 to 1: a vertex colour set, a Deformer Graph attribute or parameters."), ZeroMask);
	auto* TexU = G.New<UMaterialExpressionTextureObject>(-900);
	TexU->Texture = WrinkleU;
	TexU->SamplerType = SAMPLERTYPE_Normal;
	auto* InU = G.Input(TEXT("WrinkleU"), FunctionInput_Texture2D, 2, TEXT("Folds from squeezing along U (a normal map)."), TexU);
	auto* TexV = G.New<UMaterialExpressionTextureObject>(-900);
	TexV->Texture = WrinkleV;
	TexV->SamplerType = SAMPLERTYPE_Normal;
	auto* InV = G.Input(TEXT("WrinkleV"), FunctionInput_Texture2D, 3, TEXT("Folds from squeezing along V (a normal map)."), TexV);
	auto* Coord = G.New<UMaterialExpressionTextureCoordinate>(-900);
	auto* Uvs = G.Input(TEXT("UVs"), FunctionInput_Vector2, 4, TEXT("Where the wrinkle maps tile; UV0 when unconnected."), Coord);
	auto* One = G.New<UMaterialExpressionConstant>(-900);
	One->R = 1.f;
	auto* Tiling = G.Input(TEXT("Tiling"), FunctionInput_Scalar, 5, TEXT("Wrinkle tiles per UV unit."), One);
	auto* Strength = G.Input(TEXT("Strength"), FunctionInput_Scalar, 6, TEXT("Scales the folds."), One);

	UMaterialExpression* Uv = G.Binary<UMaterialExpressionMultiply>(Uvs, Tiling);
	auto Sample = [&](UMaterialExpression* Tex) {
		auto* S = G.New<UMaterialExpressionTextureSample>(-300);
		S->TextureObject.Connect(0, Tex);
		S->Coordinates.Connect(0, Uv);
		S->SamplerType = SAMPLERTYPE_Normal;
		return S;
	};
	auto Channel = [&](UMaterialExpression* In, bool bR) {
		auto* M = G.New<UMaterialExpressionComponentMask>();
		M->Input.Connect(0, In);
		M->R = bR;
		M->G = !bR;
		return M;
	};
	auto Rgb = [&](UMaterialExpression* In) {
		auto* M = G.New<UMaterialExpressionComponentMask>();
		M->Input.Connect(0, In);
		M->R = M->G = M->B = true;
		return M;
	};
	UMaterialExpression* AmountU = G.Binary<UMaterialExpressionMultiply>(Channel(Mask, true), Strength);
	UMaterialExpression* AmountV = G.Binary<UMaterialExpressionMultiply>(Channel(Mask, false), Strength);
	// detail = flat + (U - flat) amountU + (V - flat) amountV
	UMaterialExpression* DeltaU = G.Binary<UMaterialExpressionMultiply>(G.Binary<UMaterialExpressionSubtract>(Rgb(Sample(InU)), Flat), AmountU);
	UMaterialExpression* DeltaV = G.Binary<UMaterialExpressionMultiply>(G.Binary<UMaterialExpressionSubtract>(Rgb(Sample(InV)), Flat), AmountV);
	auto* Detail = G.New<UMaterialExpressionNormalize>();
	Detail->VectorInput.Connect(0, G.Binary<UMaterialExpressionAdd>(G.Binary<UMaterialExpressionAdd>(Flat, DeltaU), DeltaV));

	// Unreal's own angle-corrected blend puts the folds onto the normal.
	auto* Blend = G.New<UMaterialExpressionMaterialFunctionCall>(-100);
	Blend->SetMaterialFunction(LoadObject<UMaterialFunctionInterface>(
		nullptr, TEXT("/Engine/Functions/Engine_MaterialFunctions02/Utility/BlendAngleCorrectedNormals.BlendAngleCorrectedNormals")));
	for (FFunctionExpressionInput& In : Blend->FunctionInputs)
		if (In.ExpressionInput && In.ExpressionInput->InputName == TEXT("BaseNormal"))
			In.Input.Connect(0, Normal);
		else if (In.ExpressionInput && In.ExpressionInput->InputName == TEXT("AdditionalNormal"))
			In.Input.Connect(0, Detail);
	G.Output(Blend, TEXT("Normal"), 0, TEXT("Your normal with the folds blended on, tangent space."));

	auto* Folds = G.New<UMaterialExpressionClamp>();
	Folds->Input.Connect(0, G.Binary<UMaterialExpressionAdd>(AmountU, AmountV));
	G.Output(Folds, TEXT("Folds"), 1, TEXT("How much fold there is, 0 to 1: darken or roughen with it."));
	Function->PostEditChange();
	Function->MarkPackageDirty();
}

/** M_LilaTensionWrinkles: the function on a plain surface, its mask from the mesh's vertex colours (Lila's "wrinkle" set). */
void BuildMaterial(UMaterial* Material, UMaterialFunction* Function)
{
	LilaLink::DeleteAllExpressions(Material);
	FGraph G;
	G.Material = Material;
	auto* Colours = G.New<UMaterialExpressionVertexColor>(-900);
	auto* Rg = G.New<UMaterialExpressionComponentMask>(-700);
	Rg->Input.Connect(0, Colours);
	Rg->R = Rg->G = true;
	auto* Call = G.New<UMaterialExpressionMaterialFunctionCall>(-400);
	Call->SetMaterialFunction(Function);
	for (FFunctionExpressionInput& In : Call->FunctionInputs)
		if (In.ExpressionInput && In.ExpressionInput->InputName == TEXT("Mask"))
			In.Input.Connect(0, Rg);
	UMaterialEditingLibrary::ConnectMaterialProperty(Call, TEXT("Normal"), MP_Normal);
	auto* Base = G.New<UMaterialExpressionVectorParameter>(-900);
	Base->ParameterName = TEXT("BaseColor");
	Base->DefaultValue = FLinearColor(0.5f, 0.5f, 0.5f);
	UMaterialEditingLibrary::ConnectMaterialProperty(Base, TEXT(""), MP_BaseColor);
	auto* Rough = G.New<UMaterialExpressionScalarParameter>(-900);
	Rough->ParameterName = TEXT("Roughness");
	Rough->DefaultValue = 0.8f;
	UMaterialEditingLibrary::ConnectMaterialProperty(Rough, TEXT(""), MP_Roughness);
	Material->SetMaterialUsage(MATUSAGE_SkeletalMesh);
	Material->SetMaterialUsage(MATUSAGE_GeometryCache);
	Material->PostEditChange();
	Material->MarkPackageDirty();
	UMaterialEditingLibrary::RecompileMaterial(Material);
}

/**
 * wrinkleFunction {folder, wrinkleU, wrinkleV (texture asset paths), material?, save?}: builds MF_LilaTensionWrinkles in
 * folder with those maps as its defaults, and with material M_LilaTensionWrinkles on top of it. Rebuilds both when they
 * exist. Replies {function, material?}.
 */
void WrinkleFunction(const TSharedPtr<FJsonObject>& Params, const FLilaLinkCallRef& Call)
{
	const FString Folder = Params->HasField(TEXT("folder")) ? Params->GetStringField(TEXT("folder")) : FString(TEXT("/Game/Lila/Wrinkles"));
	UTexture* WrinkleU = LoadObject<UTexture>(nullptr, *Params->GetStringField(TEXT("wrinkleU")));
	UTexture* WrinkleV = LoadObject<UTexture>(nullptr, *Params->GetStringField(TEXT("wrinkleV")));
	if (!WrinkleU || !WrinkleV)
	{
		Call->Reply(nullptr, TEXT("wrinkleFunction: wrinkleU and wrinkleV must be texture assets"));
		return;
	}
	IAssetTools& Tools = FAssetToolsModule::GetModule().Get();
	UMaterialFunction* Function = FindAt<UMaterialFunction>(Folder, TEXT("MF_LilaTensionWrinkles"));
	if (!Function)
		Function = Cast<UMaterialFunction>(Tools.CreateAsset(TEXT("MF_LilaTensionWrinkles"), Folder, UMaterialFunction::StaticClass(),
		                                                     NewObject<UMaterialFunctionFactoryNew>(GetTransientPackage())));
	if (!Function)
	{
		Call->Reply(nullptr, TEXT("wrinkleFunction: could not create MF_LilaTensionWrinkles in ") + Folder);
		return;
	}
	BuildFunction(Function, WrinkleU, WrinkleV);
	TArray<UPackage*> Packages{Function->GetPackage()};
	TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
	Result->SetStringField(TEXT("function"), Function->GetPathName());
	bool bMaterial = false;
	if (Params->TryGetBoolField(TEXT("material"), bMaterial) && bMaterial)
	{
		UMaterial* Material = FindAt<UMaterial>(Folder, TEXT("M_LilaTensionWrinkles"));
		if (!Material)
			Material = Cast<UMaterial>(Tools.CreateAsset(TEXT("M_LilaTensionWrinkles"), Folder, UMaterial::StaticClass(),
			                                            NewObject<UMaterialFactoryNew>(GetTransientPackage())));
		if (Material)
		{
			BuildMaterial(Material, Function);
			Packages.Add(Material->GetPackage());
			Result->SetStringField(TEXT("material"), Material->GetPathName());
		}
	}
	bool bSave = false;
	if (Params->TryGetBoolField(TEXT("save"), bSave) && bSave)
		UEditorLoadingAndSavingUtils::SavePackages(Packages, true);
	Call->Reply(MakeShared<FJsonValueObject>(Result));
}
} // namespace

void LilaLink::AddWrinkleMethods(TMap<FString, FLilaLinkMethod>& Methods)
{
	Methods.Add(TEXT("wrinkleFunction"), &WrinkleFunction);
}
