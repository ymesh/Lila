#include "LilaLinkMaterialFunction.h"

#include "AssetRegistry/IAssetRegistry.h"
#include "AssetToolsModule.h"
#include "Factories/MaterialFactoryNew.h"
#include "Factories/MaterialFunctionFactoryNew.h"
#include "IAssetTools.h"
#include "MaterialEditingLibrary.h"
#include "Materials/Material.h"
#include "Materials/MaterialExpressionFeatureLevelSwitch.h"
#include "Materials/MaterialExpressionFunctionOutput.h"
#include "Materials/MaterialExpressionMaterialFunctionCall.h"
#include "Materials/MaterialExpressionScalarParameter.h"
#include "Materials/MaterialExpressionTextureObjectParameter.h"
#include "Materials/MaterialExpressionVectorParameter.h"
#include "Materials/MaterialFunction.h"
#include "ProfilingDebugging/CpuProfilerTrace.h"
#include "UObject/Package.h"

namespace
{
template <class T> T* FindOrCreate(const FString& Folder, const FString& Name, UFactory* Factory, bool& bOutCreated)
{
	const FAssetData Data = IAssetRegistry::GetChecked().GetAssetByObjectPath(FSoftObjectPath(Folder / Name + TEXT(".") + Name));
	if (T* Existing = Data.IsValid() ? Cast<T>(Data.GetAsset()) : nullptr)
	{
		return Existing;
	}
	bOutCreated = true;
	return Cast<T>(FAssetToolsModule::GetModule().Get().CreateAsset(Name, Folder, T::StaticClass(), Factory));
}
} // namespace

LilaLink::FMaterialPair LilaLink::FindOrCreateMaterialPair(const FString& Folder, const FString& FunctionName, const FString& MaterialName,
	FString& OutError)
{
	TRACE_CPUPROFILER_EVENT_SCOPE(LilaLinkMaterialFunction::FindOrCreateMaterialPair);

	FMaterialPair Pair;
	Pair.Function = FindOrCreate<UMaterialFunction>(Folder, FunctionName, NewObject<UMaterialFunctionFactoryNew>(GetTransientPackage()), Pair.bCreated);
	Pair.Material = FindOrCreate<UMaterial>(Folder, MaterialName, NewObject<UMaterialFactoryNew>(GetTransientPackage()), Pair.bCreated);
	if (!Pair.Function || !Pair.Material)
	{
		OutError = FString::Printf(TEXT("Could not create %s and %s in %s"), *FunctionName, *MaterialName, *Folder);
		Pair = {};
	}
	return Pair;
}

UMaterialFunction* LilaLink::FindOrCreateMaterialFunction(const FString& Folder, const FString& Name, bool& bOutCreated)
{
	return FindOrCreate<UMaterialFunction>(Folder, Name, NewObject<UMaterialFunctionFactoryNew>(GetTransientPackage()), bOutCreated);
}

UMaterial* LilaLink::FindOrCreateMaterial(const FString& Folder, const FString& Name, bool& bOutCreated)
{
	return FindOrCreate<UMaterial>(Folder, Name, NewObject<UMaterialFactoryNew>(GetTransientPackage()), bOutCreated);
}

void LilaLink::DeleteAllExpressions(UMaterial* Material)
{
	const TArray<TObjectPtr<UMaterialExpression>> Expressions(Material->GetExpressions());
	for (UMaterialExpression* Expression : Expressions)
	{
		UMaterialEditingLibrary::DeleteMaterialExpression(Material, Expression);
	}
}

void LilaLink::DeleteAllExpressions(UMaterialFunction* Function)
{
	const TArray<TObjectPtr<UMaterialExpression>> Expressions(Function->GetExpressions());
	for (UMaterialExpression* Expression : Expressions)
	{
		UMaterialEditingLibrary::DeleteMaterialExpressionInFunction(Function, Expression);
	}
}

LilaLink::FFunctionGraph::FFunctionGraph(UMaterialFunction* InFunction, const TCHAR* Description)
	: Function(InFunction)
{
	Function->Modify();
	LilaLink::DeleteAllExpressions(Function);
	Function->Description = Description;
	Function->bExposeToLibrary = true;
}

UMaterialExpression* LilaLink::FFunctionGraph::Make(TSubclassOf<UMaterialExpression> Class, int32 X)
{
	return UMaterialEditingLibrary::CreateMaterialExpressionInFunction(Function, Class, X, (Row++) * 120);
}

UMaterialExpressionFunctionInput* LilaLink::FFunctionGraph::Input(const TCHAR* Name, EFunctionInputType Type, UMaterialExpression* Default,
	const TCHAR* Description, int32 DefaultOutput)
{
	auto* E = New<UMaterialExpressionFunctionInput>(-1100);
	E->InputName = Name;
	E->InputType = Type;
	E->SortPriority = InputPriority++;
	E->Description = Description;
	if (Default)
	{
		E->Preview.Connect(DefaultOutput, Default);
		E->bUsePreviewValueAsDefault = true;
	}
	return E;
}

UMaterialExpressionFunctionInput* LilaLink::FFunctionGraph::ScalarInput(const TCHAR* Name, float Value, const TCHAR* Group, const TCHAR* Description)
{
	auto* P = New<UMaterialExpressionScalarParameter>(-1500);
	P->ParameterName = Name;
	P->DefaultValue = Value;
	P->Group = Group;
	return Input(Name, FunctionInput_Scalar, P, Description);
}

UMaterialExpressionFunctionInput* LilaLink::FFunctionGraph::VectorInput(const TCHAR* Name, const FLinearColor& Value, const TCHAR* Group,
	const TCHAR* Description)
{
	auto* P = New<UMaterialExpressionVectorParameter>(-1500);
	P->ParameterName = Name;
	P->DefaultValue = Value;
	P->Group = Group;
	return Input(Name, FunctionInput_Vector3, P, Description);
}

UMaterialExpressionFunctionInput* LilaLink::FFunctionGraph::TextureInput(const TCHAR* Name, UTexture* Value, EMaterialSamplerType Type,
	const TCHAR* Group, const TCHAR* Description)
{
	auto* P = New<UMaterialExpressionTextureObjectParameter>(-1500);
	P->ParameterName = Name;
	P->Texture = Value;
	P->SamplerType = Type;
	P->Group = Group;
	return Input(Name, FunctionInput_Texture2D, P, Description);
}

void LilaLink::FFunctionGraph::Output(UMaterialExpression* From, const TCHAR* Name, const TCHAR* Description, int32 FromOutput)
{
	auto* E = New<UMaterialExpressionFunctionOutput>(400);
	E->OutputName = Name;
	E->SortPriority = OutputPriority++;
	E->Description = Description;
	E->A.Connect(FromOutput, From);
}

UMaterialExpression* LilaLink::FFunctionGraph::FeatureLevelSwitch(UMaterialExpression* Full, UMaterialExpression* Mobile,
	int32 FullOutput, int32 MobileOutput)
{
	auto* E = New<UMaterialExpressionFeatureLevelSwitch>(200);
	E->Default.Connect(FullOutput, Full);
	E->Inputs[ERHIFeatureLevel::SM5].Connect(FullOutput, Full);
	E->Inputs[ERHIFeatureLevel::SM6].Connect(FullOutput, Full);
	E->Inputs[ERHIFeatureLevel::ES3_1].Connect(Mobile ? MobileOutput : FullOutput, Mobile ? Mobile : Full);
	return E;
}

void LilaLink::FFunctionGraph::Finish()
{
	TRACE_CPUPROFILER_EVENT_SCOPE(LilaLinkMaterialFunction::Finish);

	Function->PostEditChange();
	Function->MarkPackageDirty();
	UMaterialEditingLibrary::UpdateMaterialFunction(Function);
}

UMaterialExpressionMaterialFunctionCall* LilaLink::ResetToFunctionCall(UMaterial* Material, UMaterialFunctionInterface* Function)
{
	Material->Modify();
	LilaLink::DeleteAllExpressions(Material);
	auto* Call = Cast<UMaterialExpressionMaterialFunctionCall>(
		UMaterialEditingLibrary::CreateMaterialExpression(Material, UMaterialExpressionMaterialFunctionCall::StaticClass(), -400, 0));
	Call->SetMaterialFunction(Function);
	return Call;
}

bool LilaLink::ConnectFunctionOutputs(UMaterialExpressionMaterialFunctionCall* Call, std::initializer_list<TPair<const TCHAR*, EMaterialProperty>> Outputs)
{
	bool bAll = true;
	for (const TPair<const TCHAR*, EMaterialProperty>& Out : Outputs)
	{
		bAll &= UMaterialEditingLibrary::ConnectMaterialProperty(Call, Out.Key, Out.Value);
	}
	return bAll;
}
