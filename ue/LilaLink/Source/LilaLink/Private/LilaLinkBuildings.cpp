#include "LilaLinkBuildings.h"

#include "LilaLinkInstanceColor.h"
#include "LilaLinkInterior.h"
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
#include "Materials/MaterialExpressionVertexColor.h"
#include "Materials/MaterialExpressionWorldPosition.h"
#include "Materials/MaterialFunction.h"
#include "Materials/MaterialInstanceConstant.h"
#include "Engine/StaticMesh.h"
#include "AssetRegistry/AssetRegistryModule.h"
#include "DataLayer/DataLayerEditorSubsystem.h"
#include "LevelInstance/LevelInstanceInterface.h"
#include "Editor.h"
#include "Engine/World.h"
#include "EngineUtils.h"
#include "Misc/PackageName.h"
#include "ObjectTools.h"
#include "PhysicsEngine/BodySetup.h"
#include "ProfilingDebugging/CpuProfilerTrace.h"
#include "ScopedTransaction.h"
#include "UObject/Package.h"
#include "WorldPartition/DataLayer/DataLayerAsset.h"
#include "WorldPartition/DataLayer/DataLayerInstance.h"

namespace
{
constexpr int32 Version = 5;
const TCHAR* VersionTag = TEXT("Lila Building v");
const TCHAR* InstanceFolder = TEXT("/Game/LilaLink/Buildings");

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
FWire Wire(const FString& Name, UMaterialExpression* E, const FString& Output = FString()) { return MakeTuple(Name, MakeTuple(E, Output)); }

/** A Custom node running Code over named inputs; every one carries the version tag VersionOf reads. */
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

/**
 * MF_LilaBuildingVariation: the building's painted colour (the instance colour Lila sent with shared module meshes, or
 * with SecondTone 1 its second tone for the trim and frame slots of two-tone modules, else vertex colour RGB: one colour
 * per building and part as Lila's palette picked it) multiplied into a grey base such as
 * the LilaBuilding trim sheet's albedo, and dirt (vertex alpha)
 * laid over it, broken up by smooth world-space noise (two octaves at DirtScale). Drop it into your own material between the base colour
 * and the output; the generated M_LilaBuilding does exactly that.
 */
void BuildVariation(UMaterialFunction* Function)
{
	LilaLink::FFunctionGraph G(Function, TEXT("Lila Building Variation: per-building painted colour and dirt from Lila's Buildings, over your "
											  "base colour or the LilaBuilding trim sheet. The colour is the instance colour Lila sent (custom data "
											  "0-3, on shared module meshes; the second tone in 4-7 with SecondTone 1), else the vertex colour (RGB colour); dirt is vertex alpha."));
	UMaterialExpression* Base = G.VectorInput(TEXT("BaseColor"), FLinearColor(0.8f, 0.8f, 0.8f), TEXT("Surface"),
		TEXT("The surface's own colour, grey where the palette should show (a trim sheet's albedo)."));
	UMaterialExpression* BaseRough = G.ScalarInput(TEXT("BaseRoughness"), 0.85f, TEXT("Surface"), TEXT("The surface's own roughness."));
	UMaterialExpression* SecondTone = G.ScalarInput(TEXT("SecondTone"), 0.0f, TEXT("Palette"),
		TEXT("1 paints with the instance's second tone (custom data 4-7): the trim and frames of two-tone modules."));
	UMaterialExpression* SecondToneBase = G.VectorInput(TEXT("SecondToneBase"), FLinearColor(0.85f, 0.85f, 0.82f), TEXT("Palette"),
		TEXT("With SecondTone 1, the colour of instances that carry no second tone (window frames keeping their own colour)."));
	auto* VC = G.New<UMaterialExpressionVertexColor>(-1600);
	// Modules are shared meshes across buildings: their instance colour (custom data 0-3, or the second tone 4-7) is the
	// building's palette.
	UMaterialExpression* Tone = LilaLink::InstanceToneData(G, -1800, SecondTone);
	UMaterialExpression* VCDefault = Custom(G, TEXT("vertex colour"), TEXT("return float4(lerp(VC, I.rgb, saturate(I.a)), VA);"), CMOT_Float4,
		{Wire(TEXT("VC"), VC), Wire(TEXT("VA"), VC, TEXT("A")), Wire(TEXT("I"), Tone)}, -1400);
	UMaterialExpression* Untoned = Custom(G, TEXT("untoned"), TEXT("return T > 0.5 ? 1.0 - saturate(I.a) : 0.0;"), CMOT_Float1,
		{Wire(TEXT("I"), Tone), Wire(TEXT("T"), SecondTone)}, -1400);
	UMaterialExpression* Palette = G.Input(TEXT("BuildingPalette"), FunctionInput_Vector4, VCDefault,
		TEXT("Painted colour (RGB, linear) and dirt (A): the instance colour Lila sent, else vertex colour, when unconnected."));
	UMaterialExpression* Strength = G.ScalarInput(TEXT("PaletteStrength"), 1.0f, TEXT("Palette"), TEXT("0 keeps the base colour, 1 paints it fully."));
	UMaterialExpression* DirtColor = G.VectorInput(TEXT("DirtColor"), FLinearColor(0.12f, 0.1f, 0.08f), TEXT("Dirt"), TEXT("Colour of grime."));
	UMaterialExpression* DirtAmount = G.ScalarInput(TEXT("DirtAmount"), 1.0f, TEXT("Dirt"), TEXT("Scales the dirt Lila wrote per building."));
	UMaterialExpression* DirtScale = G.ScalarInput(TEXT("DirtScale"), 150.0f, TEXT("Dirt"), TEXT("World size of the dirt breakup, cm."));
	auto* World = G.New<UMaterialExpressionWorldPosition>(-1600);
	UMaterialExpression* Result = Custom(G, TEXT("palette and dirt"),
		TEXT("float3 painted = lerp(lerp(Base, Base * P.rgb * 1.25, saturate(S)), Own, U);\n"
			 "float3 q = W / max(Scale, 1.0);\n"
			 "float n = 0.0;\n"
			 "for (int o = 0; o < 2; ++o)\n"
			 "{\n"
			 "    float3 i = floor(q), f = frac(q);\n"
			 "    f = f * f * (3.0 - 2.0 * f);\n"
			 "    float c[8];\n"
			 "    for (int k = 0; k < 8; ++k)\n"
			 "        c[k] = frac(sin(dot(i + float3(k & 1, (k >> 1) & 1, k >> 2), float3(127.1, 311.7, 74.7))) * 43758.5453);\n"
			 "    n += lerp(lerp(lerp(c[0], c[1], f.x), lerp(c[2], c[3], f.x), f.y), lerp(lerp(c[4], c[5], f.x), lerp(c[6], c[7], f.x), f.y), f.z)\n"
			 "         * (o == 0 ? 0.65 : 0.35);\n"
			 "    q = q * 2.7 + 13.0;\n"
			 "}\n"
			 "float d = saturate(P.a * A * (0.55 + 0.45 * n));\n"
			 "return float4(lerp(painted, Dirt, d), d);"),
		CMOT_Float4,
		{Wire(TEXT("Base"), Base), Wire(TEXT("P"), Palette), Wire(TEXT("S"), Strength), Wire(TEXT("Dirt"), DirtColor), Wire(TEXT("A"), DirtAmount),
			Wire(TEXT("Scale"), DirtScale), Wire(TEXT("W"), World), Wire(TEXT("Own"), SecondToneBase), Wire(TEXT("U"), Untoned)});
	UMaterialExpression* Color = Custom(G, TEXT("colour"), TEXT("return R.rgb;"), CMOT_Float3, {Wire(TEXT("R"), Result)}, -200);
	UMaterialExpression* Rough = Custom(G, TEXT("roughness"), TEXT("return saturate(Rough + R.a * 0.1);"), CMOT_Float1,
		{Wire(TEXT("R"), Result), Wire(TEXT("Rough"), BaseRough)}, -200);
	UMaterialExpression* Dirt = Custom(G, TEXT("dirt"), TEXT("return R.a;"), CMOT_Float1, {Wire(TEXT("R"), Result)}, -200);
	G.Output(Color, TEXT("BaseColor"), TEXT("The base colour painted and dirtied."));
	G.Output(Rough, TEXT("Roughness"), TEXT("Roughness, a touch rougher where dirty."));
	G.Output(Dirt, TEXT("Dirt"), TEXT("The dirt mask, 0 to 1, for your own layers."));
	G.Finish();
}

/** MF_LilaBuildingGlass: the clear glazing of enterable buildings, a tinted pane you see the rooms through. */
void BuildGlass(UMaterialFunction* Function)
{
	LilaLink::FFunctionGraph G(Function, TEXT("Lila Building Glass: clear window glass for enterable buildings (tint, opacity, roughness)."));
	UMaterialExpression* Tint = G.VectorInput(TEXT("GlassTint"), FLinearColor(0.55f, 0.62f, 0.66f), TEXT("Glass"), TEXT("Colour of the pane."));
	UMaterialExpression* Opacity = G.ScalarInput(TEXT("GlassOpacity"), 0.18f, TEXT("Glass"), TEXT("0 is invisible, 1 opaque."));
	UMaterialExpression* Rough = G.ScalarInput(TEXT("GlassRoughness"), 0.04f, TEXT("Glass"), TEXT("Roughness of the pane."));
	G.Output(Custom(G, TEXT("tint"), TEXT("return T;"), CMOT_Float3, {Wire(TEXT("T"), Tint)}, -200), TEXT("BaseColor"), TEXT("The pane's colour."));
	G.Output(Custom(G, TEXT("opacity"), TEXT("return saturate(O);"), CMOT_Float1, {Wire(TEXT("O"), Opacity)}, -200), TEXT("Opacity"),
		TEXT("Opacity for a translucent material."));
	G.Output(Custom(G, TEXT("roughness"), TEXT("return saturate(R);"), CMOT_Float1, {Wire(TEXT("R"), Rough)}, -200), TEXT("Roughness"),
		TEXT("The pane's roughness."));
	G.Finish();
}

void Master(UMaterial* Material,UMaterialFunction* Function, TArray<FString>& OutWarnings)
{
	UMaterialExpressionMaterialFunctionCall* Call = LilaLink::ResetToFunctionCall(Material, Function);
	if (!LilaLink::ConnectFunctionOutputs(Call, {{TEXT("BaseColor"), MP_BaseColor}, {TEXT("Roughness"), MP_Roughness}}))
	{
		OutWarnings.Add(FString::Printf(TEXT("Could not wire the outputs of %s into %s"), *Function->GetName(), *Material->GetName()));
	}
	Material->SetUsageByFlag(MATUSAGE_Nanite, true);
	Material->SetUsageByFlag(MATUSAGE_InstancedStaticMeshes, true);
	Material->PostEditChange();
	Material->MarkPackageDirty();
}

/** Slot is Name, or Name numbered by Interchange (Name_1). */
bool IsSlotNamed(const FString& Slot, const FString& Name)
{
	return Slot.Equals(Name, ESearchCase::IgnoreCase) || (Slot.StartsWith(Name, ESearchCase::IgnoreCase) && Slot.Mid(Name.Len()).StartsWith(TEXT("_")));
}

/** A Buildings module slot that takes its building's palette colour: bld_render, bld_trim, bld_roof. */
bool IsModulePaletteSlot(const FString& Slot)
{
	for (const TCHAR* Name : {TEXT("bld_render"), TEXT("bld_trim"), TEXT("bld_roof")})
	{
		if (IsSlotNamed(Slot, Name))
		{
			return true;
		}
	}
	return false;
}

/** A module slot whose Lila material has Second Tone on, with that material's own colour. */
struct FSecondToneSlot
{
	FString Name;
	FLinearColor Color = FLinearColor(0.85f, 0.85f, 0.82f);
	UMaterialInterface* Instance = nullptr;
};

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
 * buildingMaterials {regenerate?, instances?: [{name, scalars {name: v}, vectors {name: [r, g, b]}}], assets?,
 * secondToneSlots?: [{name, color: [r, g, b]}]}: builds MF_LilaBuildingVariation and M_LilaBuilding when missing or
 * older, then one instance per entry. Module slots of the assets take MI_Bld_Modules, the second-tone slots (Lila
 * materials with Second Tone on; bld_trim and bld_frame when not given) MI_Bld_Tone_<slot>, which reads the second tone
 * and shows color where an instance carries none. Returns {function, material, instances, created, warnings}.
 */
TSharedPtr<FJsonValue> BuildingMaterials(const TSharedPtr<FJsonObject>& Params, FString& OutError)
{
	TRACE_CPUPROFILER_EVENT_SCOPE(LilaLinkBuildings::BuildingMaterials);

	bool bRegenerate = false;
	Params->TryGetBoolField(TEXT("regenerate"), bRegenerate);
	TArray<FString> Created, Warnings;
	FString Error;
	const LilaLink::FMaterialPair Pair =
		LilaLink::FindOrCreateMaterialPair(LilaLink::MastersFolderPath(), TEXT("MF_LilaBuildingVariation"), TEXT("M_LilaBuilding"), Error);
	if (!Pair.Material)
	{
		OutError = TEXT("buildingMaterials: ") + Error;
		return nullptr;
	}
	if (Pair.bCreated || bRegenerate || VersionOf(Pair.Function) < Version)
	{
		BuildVariation(Pair.Function);
		Master(Pair.Material, Pair.Function, Warnings);
		Created.AddUnique(Pair.Function->GetPathName());
		Created.AddUnique(Pair.Material->GetPathName());
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
			UMaterialInstanceConstant* Instance = InstanceFor(Name, Pair.Material, Created, Warnings);
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
	// Shell assets: slots named Bld_<style>_<part><k> take their style's instance, Bld_c<hex> (a given colour) the material.
	// Module slots bld_render, bld_trim and bld_roof take MI_Bld_Modules: modules are shared across styles and get their
	// building's palette colour per instance (custom data), which the variation function reads. Second-tone slots take
	// MI_Bld_Tone_<slot>, the same with SecondTone 1 and the slot's own colour for instances without a second tone.
	TArray<FSecondToneSlot> SecondTone;
	SecondTone.Add({TEXT("bld_trim"), FLinearColor(0.85f, 0.83f, 0.78f)});
	SecondTone.Add({TEXT("bld_frame")});
	const TArray<TSharedPtr<FJsonValue>>* SecondToneList = nullptr;
	if (Params->TryGetArrayField(TEXT("secondToneSlots"), SecondToneList))
	{
		SecondTone.Reset();
		for (const TSharedPtr<FJsonValue>& V : *SecondToneList)
		{
			const TSharedPtr<FJsonObject>* In = nullptr;
			if (!V->TryGetObject(In))
			{
				continue;
			}
			FSecondToneSlot& S = SecondTone.AddDefaulted_GetRef();
			S.Name = (*In)->GetStringField(TEXT("name"));
			const TArray<TSharedPtr<FJsonValue>>* C = nullptr;
			if ((*In)->TryGetArrayField(TEXT("color"), C) && C->Num() >= 3)
			{
				S.Color = FLinearColor(float((*C)[0]->AsNumber()), float((*C)[1]->AsNumber()), float((*C)[2]->AsNumber()));
			}
		}
	}
	UMaterialInterface* ModuleInstance = nullptr;
	const TArray<TSharedPtr<FJsonValue>>* Assets = nullptr;
	if (Params->TryGetArrayField(TEXT("assets"), Assets))
	{
		for (const TSharedPtr<FJsonValue>& V : *Assets)
		{
			UStaticMesh* Mesh = LoadObject<UStaticMesh>(nullptr, *V->AsString(), nullptr, LOAD_NoWarn | LOAD_Quiet);
			if (!Mesh)
			{
				Warnings.Add(FString::Printf(TEXT("No static mesh %s to assign building materials to"), *V->AsString()));
				continue;
			}
			bool bChanged = false;
			TArray<FStaticMaterial>& Slots = Mesh->GetStaticMaterials();
			for (int32 K = 0; K < Slots.Num(); ++K)
			{
				const FString Slot = Slots[K].MaterialSlotName.ToString();
				FSecondToneSlot* Second = SecondTone.FindByPredicate([&Slot](const FSecondToneSlot& S) { return IsSlotNamed(Slot, S.Name); });
				if (Second || IsModulePaletteSlot(Slot))
				{
					UMaterialInterface*& Use = Second ? Second->Instance : ModuleInstance;
					if (!Use)
					{
						UMaterialInstanceConstant* MI = InstanceFor(Second ? TEXT("MI_Bld_Tone_") + Second->Name : FString(TEXT("MI_Bld_Modules")), Pair.Material, Created, Warnings);
						if (MI)
						{
							UMaterialEditingLibrary::SetMaterialInstanceScalarParameterValue(MI, TEXT("SecondTone"), Second ? 1.0f : 0.0f);
							if (Second)
							{
								UMaterialEditingLibrary::SetMaterialInstanceVectorParameterValue(MI, TEXT("SecondToneBase"), Second->Color);
							}
							UMaterialEditingLibrary::UpdateMaterialInstance(MI);
						}
						Use = MI;
					}
					if (Use && Slots[K].MaterialInterface != Use)
					{
						Slots[K].MaterialInterface = Use;
						bChanged = true;
					}
					continue;
				}
				if (!Slot.StartsWith(TEXT("Bld_"), ESearchCase::CaseSensitive))
				{
					continue;
				}
				FString Style = Slot.RightChop(4);
				int32 Cut = INDEX_NONE;
				if (Style.FindChar(TCHAR('_'), Cut))
				{
					Style = Style.Left(Cut);
				}
				UMaterialInterface* Use = Pair.Material;
				FString InstancePath;
				if (Instances->TryGetStringField(TEXT("MI_Bld_") + Style, InstancePath))
				{
					if (UMaterialInterface* MI = LoadObject<UMaterialInterface>(nullptr, *InstancePath, nullptr, LOAD_NoWarn | LOAD_Quiet))
					{
						Use = MI;
					}
				}
				if (Slots[K].MaterialInterface != Use)
				{
					Slots[K].MaterialInterface = Use;
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
		}
	}
	TArray<FString> SaveErrors;
	LilaLink::SaveAssetPaths(Created, SaveErrors);
	Warnings.Append(SaveErrors);
	TSharedRef<FJsonObject> Out = MakeShared<FJsonObject>();
	Out->SetStringField(TEXT("function"), Pair.Function->GetPathName());
	Out->SetStringField(TEXT("material"), Pair.Material->GetPathName());
	Out->SetObjectField(TEXT("instances"), Instances);
	TArray<TSharedPtr<FJsonValue>> C, W;
	for (const FString& P : Created)
	{
		C.Add(Str(P));
	}
	for (const FString& P : Warnings)
	{
		W.Add(Str(P));
		UE_LOG(LogLilaLink, Warning, TEXT("buildingMaterials: %s"), *P);
	}
	Out->SetArrayField(TEXT("created"), C);
	Out->SetArrayField(TEXT("warnings"), W);
	return Obj(Out);
}
/** The data layer for one interior, its asset made under Folder when missing; null with Why outside World Partition. */
UDataLayerInstance* InteriorDataLayer(UWorld* World, const FString& Folder, const FString& Name, FString& Why)
{
	if (!World->IsPartitionedWorld())
	{
		Why = TEXT("the level is not a World Partition level: no data layer");
		return nullptr;
	}
	UDataLayerEditorSubsystem* Layers = UDataLayerEditorSubsystem::Get();
	if (!Layers)
	{
		Why = TEXT("no data layer editor subsystem");
		return nullptr;
	}
	const FString AssetName = ObjectTools::SanitizeObjectName(TEXT("DL_") + Name);
	const FString PackageName = Folder / TEXT("DataLayers") / AssetName;
	const FString ObjectPath = PackageName + TEXT(".") + AssetName;
	UDataLayerAsset* Asset = FindObject<UDataLayerAsset>(nullptr, *ObjectPath);
	if (!Asset && FPackageName::DoesPackageExist(PackageName))
	{
		Asset = LoadObject<UDataLayerAsset>(nullptr, *ObjectPath, nullptr, LOAD_NoWarn | LOAD_Quiet);
	}
	if (!Asset)
	{
		UPackage* Package = CreatePackage(*PackageName);
		Asset = NewObject<UDataLayerAsset>(Package, FName(*AssetName), RF_Public | RF_Standalone | RF_Transactional);
		// Runtime, so game code loads an interior when the player comes near its door.
		Asset->SetType(EDataLayerType::Runtime);
		FAssetRegistryModule::AssetCreated(Asset);
		Asset->MarkPackageDirty();
	}
	if (UDataLayerInstance* Existing = Layers->GetDataLayerInstance(Asset))
	{
		return Existing;
	}
	FDataLayerCreationParameters Params;
	Params.DataLayerAsset = Asset;
	Params.WorldDataLayers = World->GetWorldDataLayers();
	UDataLayerInstance* Instance = Layers->CreateDataLayerInstance(Params);
	if (!Instance)
	{
		Why = TEXT("could not make data layer ") + AssetName;
	}
	return Instance;
}

TArray<AActor*> ActorsTagged(UWorld* World, const FName& Tag)
{
	TArray<AActor*> Out;
	for (TActorIterator<AActor> It(World); It; ++It)
	{
		if (It->Tags.Contains(Tag))
		{
			Out.Add(*It);
		}
	}
	return Out;
}

/** Interior meshes collide as their own triangles, so a character walks the floors and stairs as modelled. */
void CollideAsModelled(const TArray<AActor*>& Actors, TArray<FString>& OutCreated)
{
	for (AActor* Actor : Actors)
	{
		TArray<UStaticMeshComponent*> Components;
		Actor->GetComponents(Components);
		for (UStaticMeshComponent* C : Components)
		{
			UStaticMesh* Mesh = C->GetStaticMesh();
			if (!Mesh || !Mesh->GetBodySetup() || Mesh->GetBodySetup()->CollisionTraceFlag == CTF_UseComplexAsSimple)
			{
				continue;
			}
			Mesh->Modify();
			Mesh->GetBodySetup()->CollisionTraceFlag = CTF_UseComplexAsSimple;
			Mesh->GetBodySetup()->CreatePhysicsMeshes();
			Mesh->MarkPackageDirty();
			OutCreated.AddUnique(Mesh->GetPathName());
		}
	}
}

/** The glass of building windows: fake rooms behind the windows of buildings without interiors, clear glass on the others. */
TSharedRef<FJsonObject> AssignGlass(const TArray<TSharedPtr<FJsonValue>>& Assets, TArray<FString>& Created, TArray<FString>& Warnings)
{
	TSharedRef<FJsonObject> Out = MakeShared<FJsonObject>();
	bool bBuilt = false;
	UMaterialInterface* Card = nullptr;
	if (UMaterial* Interior = LilaLink::InteriorMaster(false, bBuilt, Created, Warnings))
	{
		Card = InstanceFor(TEXT("MI_Bld_InteriorCard"), Interior, Created, Warnings);
	}
	FString Error;
	const LilaLink::FMaterialPair GlassPair =
		LilaLink::FindOrCreateMaterialPair(LilaLink::MastersFolderPath(), TEXT("MF_LilaBuildingGlass"), TEXT("M_LilaBuildingGlass"), Error);
	UMaterial* Glass = GlassPair.Material;
	if (!Glass)
	{
		Warnings.Add(TEXT("M_LilaBuildingGlass: ") + Error);
	}
	else if (GlassPair.bCreated || VersionOf(GlassPair.Function) < Version)
	{
		BuildGlass(GlassPair.Function);
		LilaLink::ConnectFunctionOutputs(LilaLink::ResetToFunctionCall(Glass, GlassPair.Function),
			{{TEXT("BaseColor"), MP_BaseColor}, {TEXT("Opacity"), MP_Opacity}, {TEXT("Roughness"), MP_Roughness}});
		Glass->BlendMode = BLEND_Translucent;
		Glass->TranslucencyLightingMode = TLM_SurfacePerPixelLighting;
		Glass->SetUsageByFlag(MATUSAGE_InstancedStaticMeshes, true);
		Glass->PostEditChange();
		Glass->MarkPackageDirty();
		Created.AddUnique(GlassPair.Function->GetPathName());
		Created.AddUnique(Glass->GetPathName());
	}
	UMaterialInterface* Clear = Glass ? InstanceFor(TEXT("MI_Bld_GlassClear"), Glass, Created, Warnings) : nullptr;
	int32 Slots = 0, Meshes = 0, NotNanite = 0;
	for (const TSharedPtr<FJsonValue>& V : Assets)
	{
		UStaticMesh* Mesh = LoadObject<UStaticMesh>(nullptr, *V->AsString(), nullptr, LOAD_NoWarn | LOAD_Quiet);
		if (!Mesh)
		{
			continue;
		}
		// Recorded for undo before anything changes; the package is dirtied only if something does.
		Mesh->Modify(false);
		bool bChanged = false, bTranslucent = false;
		for (FStaticMaterial& S : Mesh->GetStaticMaterials())
		{
			const FString Name = S.MaterialSlotName.ToString();
			UMaterialInterface* Use = Name == TEXT("bld_glass") ? Card : Name == TEXT("bld_glassClear") ? Clear : nullptr;
			bTranslucent |= Use && Use == Clear;
			if (Use && S.MaterialInterface != Use)
			{
				S.MaterialInterface = Use;
				bChanged = true;
				++Slots;
			}
		}
		// Nanite draws translucent sections with the default material, so a mesh with clear glass renders without it.
		if (bTranslucent && Mesh->GetNaniteSettings().bEnabled)
		{
			FMeshNaniteSettings Nanite = Mesh->GetNaniteSettings();
			Nanite.bEnabled = false;
			Mesh->SetNaniteSettings(Nanite);
			bChanged = true;
			++NotNanite;
		}
		if (bChanged)
		{
			Mesh->PostEditChange();
			Mesh->MarkPackageDirty();
			Created.AddUnique(Mesh->GetPathName());
			++Meshes;
		}
	}
	if (!Card)
	{
		Warnings.Add(TEXT("M_LilaInterior is missing: building windows keep their dark glass"));
	}
	Out->SetNumberField(TEXT("assets"), Meshes);
	Out->SetNumberField(TEXT("slots"), Slots);
	Out->SetNumberField(TEXT("naniteOff"), NotNanite);
	return Out;
}

TSharedPtr<FJsonValue> BuildingInteriors(const TSharedPtr<FJsonObject>& Params, FString& OutError)
{
	TRACE_CPUPROFILER_EVENT_SCOPE(LilaLinkBuildings::BuildingInteriors);

	UWorld* World = GEditor ? GEditor->GetEditorWorldContext().World() : nullptr;
	if (!World)
	{
		OutError = TEXT("buildingInteriors: no level is open");
		return nullptr;
	}
	FString Folder = TEXT("/Game/Lila/Buildings");
	Params->TryGetStringField(TEXT("folder"), Folder);
	TArray<FString> Created, Warnings;
	TArray<TSharedPtr<FJsonValue>> Results;
	const TArray<TSharedPtr<FJsonValue>>* Interiors = nullptr;
	FScopedTransaction Transaction(NSLOCTEXT("LilaLink", "BuildingInteriors", "Lila: Building Interiors"));
	if (Params->TryGetArrayField(TEXT("interiors"), Interiors))
	{
		UDataLayerEditorSubsystem* Layers = UDataLayerEditorSubsystem::Get();
		for (const TSharedPtr<FJsonValue>& V : *Interiors)
		{
			const TSharedPtr<FJsonObject> I = V->AsObject();
			if (!I)
			{
				continue;
			}
			FString Building, Name, Mode = TEXT("levelInstance"), Assembly;
			I->TryGetStringField(TEXT("building"), Building);
			I->TryGetStringField(TEXT("name"), Name);
			I->TryGetStringField(TEXT("mode"), Mode);
			I->TryGetStringField(TEXT("assembly"), Assembly);
			TArray<AActor*> Actors;
			if (Mode == TEXT("levelInstance") && !Assembly.IsEmpty())
			{
				Actors = ActorsTagged(World, FName(*(TEXT("LilaAssembly:") + Assembly)));
			}
			const TArray<TSharedPtr<FJsonValue>>* Groups = nullptr;
			if (I->TryGetArrayField(TEXT("groups"), Groups))
			{
				TArray<AActor*> Members;
				for (const TSharedPtr<FJsonValue>& G : *Groups)
				{
					Members.Append(ActorsTagged(World, FName(*(TEXT("LilaGroup:") + G->AsString()))));
				}
				CollideAsModelled(Members, Created);
				if (Mode == TEXT("dataLayer"))
				{
					Actors = Members;
				}
			}
			TSharedRef<FJsonObject> R = MakeShared<FJsonObject>();
			R->SetStringField(TEXT("building"), Building);
			R->SetNumberField(TEXT("actors"), Actors.Num());
			FString Why;
			UDataLayerInstance* Layer = Actors.IsEmpty() ? nullptr : InteriorDataLayer(World, Folder, Name.IsEmpty() ? Building : Name, Why);
			if (Layer && Layers)
			{
				const FName InteriorTag(*(TEXT("LilaInterior:") + Building));
				TArray<AActor*> Changed;
				for (AActor* A : Actors)
				{
					// One already set up is left untouched: modifying a loaded Level Instance's actor can trip World Partition.
					if (A->Tags.Contains(InteriorTag) && !A->bEnableAutoLODGeneration && A->GetDataLayerAssets().Contains(Layer->GetAsset()))
					{
						continue;
					}
					A->Modify();
					A->Tags.AddUnique(InteriorTag);
					// Inside its shell an interior adds nothing to the HLODs but cost.
					A->bEnableAutoLODGeneration = false;
					Changed.Add(A);
				}
				if (!Changed.IsEmpty())
				{
					Layers->AddActorsToDataLayer(Changed, Layer);
				}
				// Level Instances placed with load=false load once they are set up.
				for (AActor* A : Actors)
				{
					if (ILevelInstanceInterface* Instance = Cast<ILevelInstanceInterface>(A); Instance && !Instance->IsLoaded())
					{
						Instance->LoadLevelInstance();
					}
				}
				R->SetStringField(TEXT("dataLayer"), Layer->GetDataLayerShortName());
			}
			else
			{
				R->SetStringField(TEXT("dataLayer"), Actors.IsEmpty() ? FString(TEXT("no actors for this interior")) : Why);
				if (!Actors.IsEmpty())
				{
					Warnings.Add(Building + TEXT(": ") + Why);
				}
				for (AActor* A : Actors)
				{
					if (ILevelInstanceInterface* Instance = Cast<ILevelInstanceInterface>(A); Instance && !Instance->IsLoaded())
					{
						Instance->LoadLevelInstance();
					}
				}
			}
			Results.Add(Obj(R));
		}
	}
	TSharedRef<FJsonObject> Out = MakeShared<FJsonObject>();
	const TArray<TSharedPtr<FJsonValue>>* GlassAssets = nullptr;
	if (Params->TryGetArrayField(TEXT("glassAssets"), GlassAssets))
	{
		Out->SetObjectField(TEXT("glass"), AssignGlass(*GlassAssets, Created, Warnings));
	}
	TArray<FString> SaveErrors;
	LilaLink::SaveAssetPaths(Created, SaveErrors);
	Warnings.Append(SaveErrors);
	TArray<TSharedPtr<FJsonValue>> C, W;
	for (const FString& P : Created)
	{
		C.Add(Str(P));
	}
	for (const FString& P : Warnings)
	{
		W.Add(Str(P));
		UE_LOG(LogLilaLink, Warning, TEXT("buildingInteriors: %s"), *P);
	}
	Out->SetArrayField(TEXT("interiors"), Results);
	Out->SetArrayField(TEXT("created"), C);
	Out->SetArrayField(TEXT("warnings"), W);
	return Obj(Out);
}
} // namespace

void LilaLink::AddBuildingMethods(TMap<FString, FLilaLinkMethod>& Methods)
{
	Methods.Add(TEXT("buildingMaterials"), LilaLink::Sync(&BuildingMaterials));
	Methods.Add(TEXT("buildingInteriors"), LilaLink::Sync(&BuildingInteriors));
}
