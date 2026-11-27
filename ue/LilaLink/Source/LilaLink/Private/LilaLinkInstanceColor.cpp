#include "LilaLinkInstanceColor.h"

#include "LilaLinkLog.h"
#include "LilaLinkMaterialFunction.h"
#include "LilaLinkMethods.h"

#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Engine/StaticMesh.h"
#include "MaterialEditingLibrary.h"
#include "Materials/Material.h"
#include "Materials/MaterialExpressionAppendVector.h"
#include "Materials/MaterialExpressionCustom.h"
#include "Materials/MaterialExpressionMaterialFunctionCall.h"
#include "Materials/MaterialExpressionPerInstanceCustomData.h"
#include "Materials/MaterialExpressionVectorParameter.h"
#include "Materials/MaterialFunction.h"
#include "ProfilingDebugging/CpuProfilerTrace.h"

namespace
{
/** Raised when the graph changes; an older function is rebuilt on the next call. */
constexpr int32 Version = 2;
const TCHAR* VersionTag = TEXT("Lila Instance Color v");
const TCHAR* FunctionName = TEXT("MF_LilaInstanceColor");
const TCHAR* MaterialName = TEXT("M_LilaInstanceColor");

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
 * MF_LilaInstanceColor: the colour each instance carries from Lila (container liveries, building palettes, any slot or
 * geometry nodes instance with a colour) over your base colour: replaced where the amount is 1, or multiplied in with
 * InstanceColorMultiply 1 (keeps a texture's detail). SecondTone 1 takes the instance's second tone instead (the
 * material Lila marks Second Tone: trim and frames of two-tone pieces). Drop it between your base colour and the output.
 */
void BuildFunction(UMaterialFunction* Function)
{
	LilaLink::FFunctionGraph G(Function, TEXT("Lila Instance Color: the colour Lila sends per instance (custom data 0-3: linear RGB, amount; "
											  "the second tone in 4-7) over your base colour."));
	UMaterialExpression* Base = G.VectorInput(TEXT("BaseColor"), FLinearColor(0.5f, 0.5f, 0.5f), TEXT("Surface"), TEXT("Your base colour."));
	UMaterialExpression* Multiply = G.ScalarInput(TEXT("InstanceColorMultiply"), 0.0f, TEXT("Instance Color"),
		TEXT("0 replaces the base colour with the instance colour, 1 multiplies it in (for textured bases)."));
	UMaterialExpression* Strength = G.ScalarInput(TEXT("InstanceColorStrength"), 1.0f, TEXT("Instance Color"), TEXT("Scales the amount Lila sent."));
	UMaterialExpression* SecondTone = G.ScalarInput(TEXT("SecondTone"), 0.0f, TEXT("Instance Color"),
		TEXT("1 takes the second tone (custom data 4-7), for the second part of a two-tone piece (Lila's Second Tone material)."));
	UMaterialExpression* Data = LilaLink::InstanceToneData(G, -1200, SecondTone);
	auto* Mix = G.New<UMaterialExpressionCustom>(-400);
	Mix->Description = FString::Printf(TEXT("%s%d mix"), VersionTag, Version);
	Mix->Code = TEXT("float a = saturate(C.a * S);\n"
					 "float3 c = lerp(C.rgb, Base * C.rgb, saturate(M));\n"
					 "return lerp(Base, c, a);");
	Mix->OutputType = CMOT_Float3;
	Mix->Inputs.Reset();
	for (const TCHAR* Name : {TEXT("Base"), TEXT("C"), TEXT("M"), TEXT("S")})
	{
		Mix->Inputs.AddDefaulted_GetRef().InputName = FName(Name);
	}
	UMaterialEditingLibrary::ConnectMaterialExpressions(Base, FString(), Mix, TEXT("Base"));
	UMaterialEditingLibrary::ConnectMaterialExpressions(Data, FString(), Mix, TEXT("C"));
	UMaterialEditingLibrary::ConnectMaterialExpressions(Multiply, FString(), Mix, TEXT("M"));
	UMaterialEditingLibrary::ConnectMaterialExpressions(Strength, FString(), Mix, TEXT("S"));
	G.Output(Mix, TEXT("BaseColor"), TEXT("The base colour with the instance colour over it."));
	G.Output(Data, TEXT("InstanceColor"), TEXT("The instance colour (or second tone) as sent: RGB linear, A the amount (0 where none was sent)."));
	G.Finish();
}

TSharedPtr<FJsonValue> InstanceColorMaterials(const TSharedPtr<FJsonObject>& Params, FString& OutError)
{
	TRACE_CPUPROFILER_EVENT_SCOPE(LilaLinkInstanceColor::InstanceColorMaterials);

	bool bRegenerate = false;
	Params->TryGetBoolField(TEXT("regenerate"), bRegenerate);
	TArray<FString> Created, Warnings;
	FString Error;
	const LilaLink::FMaterialPair Pair = LilaLink::FindOrCreateMaterialPair(LilaLink::MastersFolderPath(), FunctionName, MaterialName, Error);
	if (!Pair.Material)
	{
		OutError = TEXT("instanceColorMaterials: ") + Error;
		return nullptr;
	}
	if (Pair.bCreated || bRegenerate || VersionOf(Pair.Function) < Version)
	{
		BuildFunction(Pair.Function);
		UMaterialExpressionMaterialFunctionCall* Call = LilaLink::ResetToFunctionCall(Pair.Material, Pair.Function);
		if (!LilaLink::ConnectFunctionOutputs(Call, {{TEXT("BaseColor"), MP_BaseColor}}))
		{
			Warnings.Add(FString::Printf(TEXT("Could not wire %s into %s"), FunctionName, MaterialName));
		}
		Pair.Material->SetUsageByFlag(MATUSAGE_Nanite, true);
		Pair.Material->SetUsageByFlag(MATUSAGE_InstancedStaticMeshes, true);
		Pair.Material->PostEditChange();
		Pair.Material->MarkPackageDirty();
		Created.AddUnique(Pair.Function->GetPathName());
		Created.AddUnique(Pair.Material->GetPathName());
	}
	int32 Assigned = 0;
	const TArray<TSharedPtr<FJsonValue>>* Assets = nullptr;
	if (Params->TryGetArrayField(TEXT("assets"), Assets))
	{
		for (const TSharedPtr<FJsonValue>& V : *Assets)
		{
			UStaticMesh* Mesh = LoadObject<UStaticMesh>(nullptr, *V->AsString(), nullptr, LOAD_NoWarn | LOAD_Quiet);
			if (!Mesh)
			{
				Warnings.Add(FString::Printf(TEXT("No static mesh %s to give %s"), *V->AsString(), MaterialName));
				continue;
			}
			bool bChanged = false;
			for (FStaticMaterial& Slot : Mesh->GetStaticMaterials())
			{
				if (Slot.MaterialInterface != Pair.Material)
				{
					Slot.MaterialInterface = Pair.Material;
					bChanged = true;
				}
			}
			if (bChanged)
			{
				Mesh->Modify();
				Mesh->PostEditChange();
				Mesh->MarkPackageDirty();
				Created.AddUnique(Mesh->GetPathName());
			}
			++Assigned;
		}
	}
	TArray<FString> SaveErrors;
	LilaLink::SaveAssetPaths(Created, SaveErrors);
	Warnings.Append(SaveErrors);
	TSharedRef<FJsonObject> Out = MakeShared<FJsonObject>();
	Out->SetStringField(TEXT("function"), Pair.Function->GetPathName());
	Out->SetStringField(TEXT("material"), Pair.Material->GetPathName());
	Out->SetNumberField(TEXT("assigned"), Assigned);
	TArray<TSharedPtr<FJsonValue>> C, W;
	for (const FString& P : Created)
	{
		C.Add(Str(P));
	}
	for (const FString& P : Warnings)
	{
		W.Add(Str(P));
		UE_LOG(LogLilaLink, Warning, TEXT("instanceColorMaterials: %s"), *P);
	}
	Out->SetArrayField(TEXT("created"), C);
	Out->SetArrayField(TEXT("warnings"), W);
	return MakeShared<FJsonValueObject>(Out);
}
} // namespace

UMaterialExpression* LilaLink::InstanceColorData(FFunctionGraph& G, int32 X, bool bSecondTone)
{
	const int32 First = bSecondTone ? 4 : 0;
	// Actors carry the colour as custom primitive data, which the parameter reads; instances read their own custom data and
	// fall back to it outside an instanced component.
	auto* Primitive = G.New<UMaterialExpressionVectorParameter>(X - 400);
	Primitive->ParameterName = bSecondTone ? TEXT("LilaInstanceColor2") : TEXT("LilaInstanceColor");
	Primitive->DefaultValue = FLinearColor(0.0f, 0.0f, 0.0f, 0.0f);
	Primitive->bUseCustomPrimitiveData = true;
	Primitive->PrimitiveDataIndex = First;
	auto* Rgb = G.New<UMaterialExpressionPerInstanceCustomData3Vector>(X - 200);
	Rgb->DataIndex = First;
	Rgb->DefaultValue.Connect(0, Primitive);
	auto* Amount = G.New<UMaterialExpressionPerInstanceCustomData>(X - 200);
	Amount->DataIndex = First + 3;
	Amount->DefaultValue.Connect(4, Primitive);
	auto* Rgba = G.New<UMaterialExpressionAppendVector>(X);
	Rgba->A.Connect(0, Rgb);
	Rgba->B.Connect(0, Amount);
	return Rgba;
}

UMaterialExpression* LilaLink::InstanceToneData(FFunctionGraph& G, int32 X, UMaterialExpression* SecondTone)
{
	UMaterialExpression* First = InstanceColorData(G, X - 200);
	if (!SecondTone)
	{
		return First;
	}
	UMaterialExpression* Second = InstanceColorData(G, X - 200, true);
	auto* Pick = G.New<UMaterialExpressionCustom>(X);
	Pick->Description = TEXT("Lila instance tone");
	Pick->Code = TEXT("return T > 0.5 ? C2 : C1;");
	Pick->OutputType = CMOT_Float4;
	Pick->Inputs.Reset();
	for (const TCHAR* Name : {TEXT("C1"), TEXT("C2"), TEXT("T")})
	{
		Pick->Inputs.AddDefaulted_GetRef().InputName = FName(Name);
	}
	UMaterialEditingLibrary::ConnectMaterialExpressions(First, FString(), Pick, TEXT("C1"));
	UMaterialEditingLibrary::ConnectMaterialExpressions(Second, FString(), Pick, TEXT("C2"));
	UMaterialEditingLibrary::ConnectMaterialExpressions(SecondTone, FString(), Pick, TEXT("T"));
	return Pick;
}

void LilaLink::AddInstanceColorMethods(TMap<FString, FLilaLinkMethod>& Methods)
{
	Methods.Add(TEXT("instanceColorMaterials"), LilaLink::Sync(&InstanceColorMaterials));
}
