#include "LilaLinkLandscape.h"

#include "LilaLinkLog.h"
#include "LilaLinkMaterialFunction.h"
#include "LilaLinkMaterials.h"

#include "AssetRegistry/IAssetRegistry.h"
#include "Editor.h"
#include "Engine/Texture.h"
#include "Engine/World.h"
#include "EngineUtils.h"
#include "Landscape.h"
#include "LandscapeEdit.h"
#include "LandscapeEditLayer.h"
#include "LandscapeInfo.h"
#include "LandscapeLayerInfoObject.h"
#include "LandscapeProxy.h"
#include "LandscapeSubsystem.h"
#include "LandscapeUtils.h"
#include "MaterialEditingLibrary.h"
#include "Materials/Material.h"
#include "Materials/MaterialExpressionConstant.h"
#include "Materials/MaterialExpressionConstant3Vector.h"
#include "Materials/MaterialExpressionCustom.h"
#include "Materials/MaterialExpressionDivide.h"
#include "Materials/MaterialExpressionFmod.h"
#include "Materials/MaterialExpressionLandscapeLayerSample.h"
#include "Materials/MaterialExpressionMaterialFunctionCall.h"
#include "Materials/MaterialExpressionScalarParameter.h"
#include "Materials/MaterialExpressionTextureObjectParameter.h"
#include "Materials/MaterialExpressionVectorParameter.h"
#include "Materials/MaterialExpressionWorldPosition.h"
#include "Materials/MaterialFunction.h"
#include "Materials/MaterialInterface.h"
#include "Misc/FileHelper.h"
#include "Misc/PackageName.h"
#include "ProfilingDebugging/CpuProfilerTrace.h"
#include "ScopedTransaction.h"

namespace
{
const FName LilaLayerName(TEXT("Lila"));
const TCHAR* LayerFunctionName = TEXT("MF_LilaLandscapeLayer");
const TCHAR* LayerVersionTag = TEXT("Lila Landscape Layer v");
// The layer texture is read through the shared wrap sampler, so landscapes with more than a dozen layers stay under
// the 16 sampler limit.
constexpr int32 LayerFunctionVersion = 2;
const TCHAR* LandscapeFolder = TEXT("/Game/LilaLink/Landscapes");

/** One paint layer of a send: its name in Unreal and the 8-bit weightmap file in the landscape's vertex layout. */
struct FLayerSend
{
	FName Name;
	FString Mask;
	FString Weightmap;
};

const TCHAR* LayerColorCode = TEXT(R"(
float3 c = Tint.rgb * Texture2DSample(Texture, View.MaterialTextureBilinearWrapedSampler, WorldTile.xy).rgb;
return lerp(Under, c, saturate(Weight));
)");

const TCHAR* LayerRoughnessCode = TEXT(R"(
return lerp(UnderRoughness, Roughness, saturate(Weight));
)");

int32 VersionOf(const UMaterialFunction* Function)
{
	for (UMaterialExpression* E : Function->GetExpressions())
	{
		if (const UMaterialExpressionCustom* C = Cast<UMaterialExpressionCustom>(E); C && C->Description.StartsWith(LayerVersionTag))
		{
			return FCString::Atoi(*C->Description.RightChop(FCString::Strlen(LayerVersionTag)));
		}
	}
	return 0;
}

/**
 * MF_LilaLandscapeLayer: one paint layer over what is under it. Under and UnderRoughness come in, the layer's tinted
 * texture (projected from above, TileSize cm per repeat) and roughness go on top by Weight (a Landscape Layer Sample).
 * Chain one call per layer; the parameter inputs default to parameters of their name.
 */
void BuildLayerFunction(UMaterialFunction* Function)
{
	TRACE_CPUPROFILER_EVENT_SCOPE(LilaLinkLandscape::BuildLayerFunction);

	LilaLink::FFunctionGraph G(Function, TEXT("Lila landscape layer: a tinted, top-projected texture and roughness laid over Under by Weight."));
	auto* UnderDefault = G.New<UMaterialExpressionConstant3Vector>(-1500);
	UnderDefault->Constant = FLinearColor(0.25f, 0.22f, 0.18f);
	auto* RoughDefault = G.New<UMaterialExpressionConstant>(-1500);
	RoughDefault->R = 0.9f;
	auto* WeightDefault = G.New<UMaterialExpressionConstant>(-1500);
	WeightDefault->R = 1.0f;
	TMap<FString, UMaterialExpression*> In;
	In.Add(TEXT("Under"), G.Input(TEXT("Under"), FunctionInput_Vector3, UnderDefault, TEXT("Colour under this layer: the ground, or the previous layer's call.")));
	In.Add(TEXT("UnderRoughness"), G.Input(TEXT("UnderRoughness"), FunctionInput_Scalar, RoughDefault, TEXT("Roughness under this layer.")));
	In.Add(TEXT("Weight"), G.Input(TEXT("Weight"), FunctionInput_Scalar, WeightDefault, TEXT("The layer's weight: a Landscape Layer Sample of its name.")));
	UTexture* White = LoadObject<UTexture>(nullptr, TEXT("/Engine/EngineResources/WhiteSquareTexture.WhiteSquareTexture"));
	In.Add(TEXT("Tint"), G.VectorInput(TEXT("LayerTint"), FLinearColor(0.5f, 0.5f, 0.5f), TEXT("Layer"), TEXT("Layer colour, multiplied with Texture.")));
	In.Add(TEXT("Roughness"), G.ScalarInput(TEXT("LayerRoughness"), 0.8f, TEXT("Layer"), TEXT("Layer roughness.")));
	In.Add(TEXT("Texture"), G.TextureInput(TEXT("LayerTexture"), White, SAMPLERTYPE_Color, TEXT("Layer"), TEXT("Layer colour texture, projected from above.")));
	UMaterialExpressionFunctionInput* TileSize = G.ScalarInput(TEXT("LayerTileSize"), 400.0f, TEXT("Layer"), TEXT("Texture repeat, cm."));
	// World position is large-world (double); divide and wrap it before the Custom node, which takes floats.
	UMaterialExpression* World = G.Make(UMaterialExpressionWorldPosition::StaticClass(), -1600);
	auto* Divide = G.New<UMaterialExpressionDivide>(-1400);
	UMaterialEditingLibrary::ConnectMaterialExpressions(World, TEXT(""), Divide, TEXT("A"));
	UMaterialEditingLibrary::ConnectMaterialExpressions(TileSize, TEXT(""), Divide, TEXT("B"));
	auto* Wrap = G.New<UMaterialExpressionFmod>(-1300);
	UMaterialEditingLibrary::ConnectMaterialExpressions(Divide, TEXT(""), Wrap, TEXT("A"));
	auto* Period = G.New<UMaterialExpressionConstant>(-1400);
	Period->R = 256.0f;
	UMaterialEditingLibrary::ConnectMaterialExpressions(Period, TEXT(""), Wrap, TEXT("B"));
	In.Add(TEXT("WorldTile"), Wrap);

	auto Custom = [&](const FString& Description, const TCHAR* Code, ECustomMaterialOutputType Type, std::initializer_list<const TCHAR*> Names) {
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
			UMaterialEditingLibrary::ConnectMaterialExpressions(In[Name], TEXT(""), E, Name);
		}
		return E;
	};
	const FString Version = FString::Printf(TEXT("%s%d"), LayerVersionTag, LayerFunctionVersion);
	auto* Color = Custom(Version + TEXT(" colour"), LayerColorCode, CMOT_Float3, {TEXT("Under"), TEXT("Weight"), TEXT("Tint"), TEXT("Texture"), TEXT("WorldTile")});
	auto* Rough = Custom(Version + TEXT(" roughness"), LayerRoughnessCode, CMOT_Float1, {TEXT("UnderRoughness"), TEXT("Roughness"), TEXT("Weight")});
	G.Output(Color, TEXT("BaseColor"), TEXT("Under with this layer on top."));
	G.Output(Rough, TEXT("Roughness"), TEXT("Roughness with this layer on top."));
	G.Finish();
}

/** MF_LilaLandscapeLayer in the masters folder, built when new or older than this plugin's version. */
UMaterialFunction* LayerFunction(TArray<FString>& OutWarnings)
{
	TRACE_CPUPROFILER_EVENT_SCOPE(LilaLinkLandscape::LayerFunction);

	bool bCreated = false;
	UMaterialFunction* Function = LilaLink::FindOrCreateMaterialFunction(LilaLink::MastersFolderPath(), LayerFunctionName, bCreated);
	if (!Function)
	{
		OutWarnings.Add(FString::Printf(TEXT("Could not create %s in %s"), LayerFunctionName, LilaLink::MastersFolderPath()));
		return nullptr;
	}
	if (bCreated || VersionOf(Function) < LayerFunctionVersion)
	{
		BuildLayerFunction(Function);
	}
	return Function;
}

/** A distinct, muted colour per layer so a fresh landscape shows where each mask lies. */
FLinearColor LayerColor(int32 Index)
{
	const float Hue = FMath::Fmod(37.0f + 67.0f * float(Index), 360.0f);
	return FLinearColor(Hue, 0.45f, 0.6f).HSVToLinearRGB();
}

/** The layer names a generated material samples, in order, or empty when it isn't one. */
TArray<FName> SampledLayers(const UMaterial* Material)
{
	TArray<FName> Names;
	for (UMaterialExpression* E : Material->GetExpressions())
	{
		if (const UMaterialExpressionLandscapeLayerSample* S = Cast<UMaterialExpressionLandscapeLayerSample>(E))
		{
			Names.Add(S->ParameterName);
		}
	}
	return Names;
}

/**
 * M_<Label>: the landscape's material, a Landscape Layer Sample per paint layer feeding a chain of MF_LilaLandscapeLayer
 * calls over a ground colour, each layer with its own tint, roughness, texture and tile size parameters. Rebuilt only when
 * the layer set changes.
 */
UMaterial* LandscapeMaterial(const FString& Label, const TArray<FLayerSend>& Layers, TArray<FString>& OutCreated, TArray<FString>& OutWarnings)
{
	TRACE_CPUPROFILER_EVENT_SCOPE(LilaLinkLandscape::LandscapeMaterial);

	UMaterialFunction* Function = LayerFunction(OutWarnings);
	if (!Function)
	{
		return nullptr;
	}
	const FString Folder = FString(LandscapeFolder) / Label;
	bool bCreated = false;
	UMaterial* Material = LilaLink::FindOrCreateMaterial(Folder, TEXT("M_") + Label, bCreated);
	if (!Material)
	{
		OutWarnings.Add(FString::Printf(TEXT("Could not create M_%s in %s"), *Label, *Folder));
		return nullptr;
	}
	TArray<FName> Wanted;
	for (const FLayerSend& L : Layers)
	{
		Wanted.Add(L.Name);
	}
	if (!bCreated && SampledLayers(Material) == Wanted)
	{
		return Material;
	}
	Material->Modify();
	LilaLink::DeleteAllExpressions(Material);
	int32 Row = 0;
	auto Make = [&](TSubclassOf<UMaterialExpression> Class, int32 X) {
		return UMaterialEditingLibrary::CreateMaterialExpression(Material, Class, X, (Row++) * 140);
	};
	auto* Ground = Cast<UMaterialExpressionVectorParameter>(Make(UMaterialExpressionVectorParameter::StaticClass(), -1200));
	Ground->ParameterName = TEXT("GroundColor");
	Ground->DefaultValue = FLinearColor(0.25f, 0.22f, 0.18f);
	auto* GroundRough = Cast<UMaterialExpressionScalarParameter>(Make(UMaterialExpressionScalarParameter::StaticClass(), -1200));
	GroundRough->ParameterName = TEXT("GroundRoughness");
	GroundRough->DefaultValue = 0.9f;
	UMaterialExpression* Color = Ground;
	UMaterialExpression* Rough = GroundRough;
	FString ColorOut, RoughOut;
	UTexture* White = LoadObject<UTexture>(nullptr, TEXT("/Engine/EngineResources/WhiteSquareTexture.WhiteSquareTexture"));
	for (int32 Index = 0; Index < Layers.Num(); ++Index)
	{
		const FString Name = Layers[Index].Name.ToString();
		auto* Sample = Cast<UMaterialExpressionLandscapeLayerSample>(Make(UMaterialExpressionLandscapeLayerSample::StaticClass(), -900));
		Sample->ParameterName = Layers[Index].Name;
		Sample->PreviewWeight = 0.0f;
		auto* Tint = Cast<UMaterialExpressionVectorParameter>(Make(UMaterialExpressionVectorParameter::StaticClass(), -900));
		Tint->ParameterName = *(Name + TEXT("Tint"));
		Tint->DefaultValue = LayerColor(Index);
		Tint->Group = *Name;
		auto* LayerRough = Cast<UMaterialExpressionScalarParameter>(Make(UMaterialExpressionScalarParameter::StaticClass(), -900));
		LayerRough->ParameterName = *(Name + TEXT("Roughness"));
		LayerRough->DefaultValue = 0.8f;
		LayerRough->Group = *Name;
		auto* Texture = Cast<UMaterialExpressionTextureObjectParameter>(Make(UMaterialExpressionTextureObjectParameter::StaticClass(), -900));
		Texture->ParameterName = *(Name + TEXT("Texture"));
		Texture->Texture = White;
		Texture->Group = *Name;
		auto* Tile = Cast<UMaterialExpressionScalarParameter>(Make(UMaterialExpressionScalarParameter::StaticClass(), -900));
		Tile->ParameterName = *(Name + TEXT("TileSize"));
		Tile->DefaultValue = 400.0f;
		Tile->Group = *Name;

		auto* Call = Cast<UMaterialExpressionMaterialFunctionCall>(Make(UMaterialExpressionMaterialFunctionCall::StaticClass(), -400 + Index * 300));
		Call->SetMaterialFunction(Function);
		UMaterialEditingLibrary::ConnectMaterialExpressions(Color, ColorOut, Call, TEXT("Under"));
		UMaterialEditingLibrary::ConnectMaterialExpressions(Rough, RoughOut, Call, TEXT("UnderRoughness"));
		UMaterialEditingLibrary::ConnectMaterialExpressions(Sample, TEXT(""), Call, TEXT("Weight"));
		UMaterialEditingLibrary::ConnectMaterialExpressions(Tint, TEXT(""), Call, TEXT("LayerTint"));
		UMaterialEditingLibrary::ConnectMaterialExpressions(LayerRough, TEXT(""), Call, TEXT("LayerRoughness"));
		UMaterialEditingLibrary::ConnectMaterialExpressions(Texture, TEXT(""), Call, TEXT("LayerTexture"));
		UMaterialEditingLibrary::ConnectMaterialExpressions(Tile, TEXT(""), Call, TEXT("LayerTileSize"));
		Color = Call;
		Rough = Call;
		ColorOut = TEXT("BaseColor");
		RoughOut = TEXT("Roughness");
	}
	UMaterialEditingLibrary::ConnectMaterialProperty(Color, ColorOut, MP_BaseColor);
	UMaterialEditingLibrary::ConnectMaterialProperty(Rough, RoughOut, MP_Roughness);
	Material->PostEditChange();
	Material->MarkPackageDirty();
	UMaterialEditingLibrary::RecompileMaterial(Material);
	OutCreated.AddUnique(Material->GetPathName());
	OutCreated.AddUnique(Function->GetPathName());
	return Material;
}

/** LI_<Layer> next to the landscape's material, made when missing, with weight blending off so masks stay independent. */
ULandscapeLayerInfoObject* LayerInfo(const FString& Label, FName Layer, TArray<FString>& OutCreated)
{
	TRACE_CPUPROFILER_EVENT_SCOPE(LilaLinkLandscape::LayerInfo);

	const FString Folder = FString(LandscapeFolder) / Label;
	const FString Name = TEXT("LI_") + Layer.ToString();
	const FAssetData Data = IAssetRegistry::GetChecked().GetAssetByObjectPath(FSoftObjectPath(Folder / Name + TEXT(".") + Name));
	ULandscapeLayerInfoObject* Info = Data.IsValid() ? Cast<ULandscapeLayerInfoObject>(Data.GetAsset()) : nullptr;
	if (!Info)
	{
		Info = UE::Landscape::CreateTargetLayerInfo(Layer, Folder, Name);
		if (Info)
		{
			OutCreated.AddUnique(Info->GetPathName());
		}
	}
	if (Info && Info->GetBlendMethod() != ELandscapeTargetLayerBlendMethod::None)
	{
		Info->SetBlendMethod(ELandscapeTargetLayerBlendMethod::None, /*bInModify=*/true);
	}
	return Info;
}

TArray<FLayerSend> ReadLayers(const TSharedPtr<FJsonObject>& Params)
{
	TArray<FLayerSend> Out;
	const TArray<TSharedPtr<FJsonValue>>* Values = nullptr;
	if (!Params->TryGetArrayField(TEXT("layers"), Values))
	{
		return Out;
	}
	for (const TSharedPtr<FJsonValue>& V : *Values)
	{
		const TSharedPtr<FJsonObject> O = V->AsObject();
		FString Name;
		if (!O.IsValid() || !O->TryGetStringField(TEXT("name"), Name) || Name.IsEmpty())
		{
			continue;
		}
		FLayerSend& L = Out.AddDefaulted_GetRef();
		L.Name = FName(*Name);
		O->TryGetStringField(TEXT("mask"), L.Mask);
		O->TryGetStringField(TEXT("weightmap"), L.Weightmap);
	}
	return Out;
}

FName TagFor(const FString& Id)
{
	return FName(*(TEXT("LilaLink.Landscape:") + Id.Right(900)));
}

UWorld* EditorWorld()
{
	return GEditor ? GEditor->GetEditorWorldContext().World() : nullptr;
}

ALandscape* FindTaggedLandscape(UWorld* World, FName Tag)
{
	for (TActorIterator<ALandscape> It(World); It; ++It)
	{
		if (It->ActorHasTag(Tag))
		{
			return *It;
		}
	}
	return nullptr;
}

bool ReadVector(const TSharedPtr<FJsonObject>& Object, const TCHAR* Field, FVector& Out)
{
	const TArray<TSharedPtr<FJsonValue>>* Values = nullptr;
	if (!Object->TryGetArrayField(Field, Values) || Values->Num() != 3)
	{
		return false;
	}
	Out = FVector((*Values)[0]->AsNumber(), (*Values)[1]->AsNumber(), (*Values)[2]->AsNumber());
	return !Out.ContainsNaN();
}

/** The landscape's layout matches the one asked for: same vertex count and section layout. */
bool SameLayout(ALandscape* Landscape, int32 SizeX, int32 SizeY, int32 QuadsPerSection, int32 SectionsPerComponent)
{
	TRACE_CPUPROFILER_EVENT_SCOPE(LilaLinkLandscape::SameLayout);

	ULandscapeInfo* Info = Landscape->GetLandscapeInfo();
	FIntRect Extent;
	if (!Info || !Info->GetLandscapeExtent(Extent))
	{
		return false;
	}
	return Extent.Width() + 1 == SizeX && Extent.Height() + 1 == SizeY && Landscape->SubsectionSizeQuads == QuadsPerSection &&
		Landscape->NumSubsections == SectionsPerComponent;
}

/** The "Lila" edit layer's guid, made when the landscape has none. */
FGuid LilaLayer(ALandscape* Landscape)
{
	TRACE_CPUPROFILER_EVENT_SCOPE(LilaLinkLandscape::LilaLayer);

	int32 Index = Landscape->GetLayerIndex(LilaLayerName);
	if (Index == INDEX_NONE)
	{
		Index = Landscape->CreateLayer(LilaLayerName);
	}
	const ULandscapeEditLayerBase* Layer = Index == INDEX_NONE ? nullptr : Landscape->GetEditLayerConst(Index);
	return Layer ? Layer->GetGuid() : FGuid();
}

ALandscape* CreateLandscape(UWorld* World, const FString& Label, const FVector& Location, const FRotator& Rotation, const FVector& Scale, int32 SizeX,
	int32 SizeY, int32 QuadsPerSection, int32 SectionsPerComponent)
{
	TRACE_CPUPROFILER_EVENT_SCOPE(LilaLinkLandscape::CreateLandscape);

	// Flat at the encoding's zero: the heights themselves go to the Lila edit layer, which adds to this.
	TArray<uint16> Flat;
	Flat.Init(32768, SizeX * SizeY);
	TMap<FGuid, TArray<uint16>> HeightDataPerLayers;
	HeightDataPerLayers.Add(FGuid(), MoveTemp(Flat));
	TMap<FGuid, TArray<FLandscapeImportLayerInfo>> MaterialLayerDataPerLayers;
	MaterialLayerDataPerLayers.Add(FGuid(), TArray<FLandscapeImportLayerInfo>());

	ALandscape* Landscape = World->SpawnActor<ALandscape>(Location, Rotation);
	if (!Landscape)
	{
		return nullptr;
	}
	Landscape->SetActorRelativeScale3D(Scale);
	Landscape->StaticLightingLOD = FMath::DivideAndRoundUp(FMath::CeilLogTwo((SizeX * SizeY) / (2048 * 2048) + 1), (uint32)2);
	Landscape->Import(FGuid::NewGuid(), 0, 0, SizeX - 1, SizeY - 1, SectionsPerComponent, QuadsPerSection, HeightDataPerLayers, TEXT(""),
		MaterialLayerDataPerLayers, ELandscapeImportAlphamapType::Additive, TArrayView<const FLandscapeLayer>());
	Landscape->SetActorLabel(Label);
	if (ULandscapeInfo* Info = Landscape->GetLandscapeInfo())
	{
		Info->UpdateLayerInfoMap(Landscape);
	}
	return Landscape;
}

TSharedPtr<FJsonValue> SetLandscape(const TSharedPtr<FJsonObject>& Params, FString& OutError)
{
	TRACE_CPUPROFILER_EVENT_SCOPE(LilaLinkLandscape::SetLandscape);

	FString Id, HeightmapPath;
	if (!Params->TryGetStringField(TEXT("id"), Id) || Id.IsEmpty())
	{
		OutError = TEXT("setLandscape: give id");
		return nullptr;
	}
	if (!Params->TryGetStringField(TEXT("heightmap"), HeightmapPath))
	{
		OutError = TEXT("setLandscape: give heightmap");
		return nullptr;
	}
	const TArray<TSharedPtr<FJsonValue>>* Resolution = nullptr;
	if (!Params->TryGetArrayField(TEXT("resolution"), Resolution) || Resolution->Num() != 2)
	{
		OutError = TEXT("setLandscape: give resolution [x, y]");
		return nullptr;
	}
	const int32 SizeX = static_cast<int32>((*Resolution)[0]->AsNumber()), SizeY = static_cast<int32>((*Resolution)[1]->AsNumber());
	int32 QuadsPerSection = 63, SectionsPerComponent = 1;
	Params->TryGetNumberField(TEXT("quadsPerSection"), QuadsPerSection);
	Params->TryGetNumberField(TEXT("sectionsPerComponent"), SectionsPerComponent);
	const int32 QuadsPerComponent = QuadsPerSection * SectionsPerComponent;
	if (SizeX < 2 || SizeY < 2 || QuadsPerComponent <= 0 || (SizeX - 1) % QuadsPerComponent != 0 || (SizeY - 1) % QuadsPerComponent != 0)
	{
		OutError = FString::Printf(TEXT("setLandscape: %dx%d isn't a whole number of %d-quad components"), SizeX, SizeY, QuadsPerComponent);
		return nullptr;
	}

	TArray<uint8> Bytes;
	if (!FFileHelper::LoadFileToArray(Bytes, *HeightmapPath))
	{
		OutError = TEXT("setLandscape: can't read ") + HeightmapPath;
		return nullptr;
	}
	if (Bytes.Num() != SizeX * SizeY * 2)
	{
		OutError = FString::Printf(TEXT("setLandscape: %s holds %d bytes, %dx%d needs %d"), *HeightmapPath, Bytes.Num(), SizeX, SizeY, SizeX * SizeY * 2);
		return nullptr;
	}
	TArray<uint16> Heights;
	Heights.SetNumUninitialized(SizeX * SizeY);
	FMemory::Memcpy(Heights.GetData(), Bytes.GetData(), Bytes.Num());

	UWorld* World = EditorWorld();
	if (!World)
	{
		OutError = TEXT("setLandscape: no editor world");
		return nullptr;
	}
	FString Label = FPackageName::GetShortName(Id), MaterialPath;
	Params->TryGetStringField(TEXT("label"), Label);
	Params->TryGetStringField(TEXT("material"), MaterialPath);
	FVector Location = FVector::ZeroVector, Scale(100.0, 100.0, 100.0), PitchYawRoll = FVector::ZeroVector;
	ReadVector(Params, TEXT("location"), Location);
	ReadVector(Params, TEXT("scale"), Scale);
	ReadVector(Params, TEXT("rotation"), PitchYawRoll);
	const FRotator Rotation(PitchYawRoll.X, PitchYawRoll.Y, PitchYawRoll.Z);
	int32 GridSize = 0;
	Params->TryGetNumberField(TEXT("gridSize"), GridSize);
	bool bMakeMaterial = false;
	Params->TryGetBoolField(TEXT("makeMaterial"), bMakeMaterial);

	// Every weightmap is read and checked before anything in the level changes.
	const TArray<FLayerSend> Layers = ReadLayers(Params);
	TArray<TArray<uint8>> Weights;
	for (const FLayerSend& L : Layers)
	{
		TArray<uint8>& W = Weights.AddDefaulted_GetRef();
		if (!FFileHelper::LoadFileToArray(W, *L.Weightmap) || W.Num() != SizeX * SizeY)
		{
			OutError = FString::Printf(TEXT("setLandscape: layer %s: %s isn't a %dx%d 8-bit weightmap"), *L.Name.ToString(), *L.Weightmap, SizeX, SizeY);
			return nullptr;
		}
	}

	FScopedTransaction Transaction(NSLOCTEXT("LilaLink", "SetLandscape", "Lila: Update Landscape"));
	const FName Tag = TagFor(Id);
	ALandscape* Landscape = FindTaggedLandscape(World, Tag);
	bool bCreated = false;
	if (Landscape && !SameLayout(Landscape, SizeX, SizeY, QuadsPerSection, SectionsPerComponent))
	{
		World->EditorDestroyActor(Landscape, true);
		Landscape = nullptr;
	}
	// Lila's content hash of heights, weightmaps and material: a landscape already carrying it is left as it is.
	FString Hash;
	Params->TryGetStringField(TEXT("hash"), Hash);
	const FName HashTag = Hash.IsEmpty() ? NAME_None : FName(TEXT("LilaHash:") + Hash);
	if (Landscape && !Hash.IsEmpty() && Landscape->Tags.Contains(HashTag))
	{
		Transaction.Cancel();
		TSharedRef<FJsonObject> Same = MakeShared<FJsonObject>();
		Same->SetStringField(TEXT("actor"), Landscape->GetActorLabel());
		Same->SetBoolField(TEXT("created"), false);
		Same->SetBoolField(TEXT("unchanged"), true);
		Same->SetArrayField(TEXT("resolution"), {MakeShared<FJsonValueNumber>(SizeX), MakeShared<FJsonValueNumber>(SizeY)});
		TArray<TSharedPtr<FJsonValue>> Names;
		for (const FLayerSend& L : Layers)
		{
			Names.Add(MakeShared<FJsonValueString>(L.Name.ToString()));
		}
		Same->SetArrayField(TEXT("layers"), Names);
		Same->SetStringField(TEXT("material"), Landscape->LandscapeMaterial ? Landscape->LandscapeMaterial->GetPathName() : FString());
		UE_LOG(LogLilaLink, Log, TEXT("setLandscape: %s unchanged, left as it is"), *Label);
		return MakeShared<FJsonValueObject>(Same);
	}
	if (!Landscape)
	{
		Landscape = CreateLandscape(World, Label, Location, Rotation, Scale, SizeX, SizeY, QuadsPerSection, SectionsPerComponent);
		if (!Landscape)
		{
			Transaction.Cancel();
			OutError = TEXT("setLandscape: couldn't spawn a landscape");
			return nullptr;
		}
		Landscape->Tags.AddUnique(Tag);
		bCreated = true;
	}
	else
	{
		Landscape->Modify();
		Landscape->SetActorLocationAndRotation(Location, Rotation);
		Landscape->SetActorRelativeScale3D(Scale);
	}
	TArray<FString> Created, Warnings;
	UMaterialInterface* Material = nullptr;
	if (!MaterialPath.IsEmpty())
	{
		const FString ObjectPath = MaterialPath.Contains(TEXT(".")) ? MaterialPath : MaterialPath + TEXT(".") + FPackageName::GetShortName(MaterialPath);
		Material = LoadObject<UMaterialInterface>(nullptr, *ObjectPath);
		if (!Material)
		{
			Warnings.Add(TEXT("no material ") + MaterialPath);
		}
	}
	else if (bMakeMaterial && Layers.Num() > 0)
	{
		Material = LandscapeMaterial(Label, Layers, Created, Warnings);
	}
	if (Material && Landscape->LandscapeMaterial != Material)
	{
		Landscape->LandscapeMaterial = Material;
		FPropertyChangedEvent Changed(FindFProperty<FProperty>(ALandscapeProxy::StaticClass(), GET_MEMBER_NAME_CHECKED(ALandscapeProxy, LandscapeMaterial)));
		Landscape->PostEditChangeProperty(Changed);
	}

	// Paint layers: a layer info per layer, registered as a target layer, so the weightmaps have somewhere to go.
	TArray<ULandscapeLayerInfoObject*> Infos;
	for (const FLayerSend& L : Layers)
	{
		ULandscapeLayerInfoObject* LayerInfoObject = LayerInfo(Label, L.Name, Created);
		if (!LayerInfoObject)
		{
			Warnings.Add(TEXT("could not make a layer info for ") + L.Name.ToString());
		}
		else if (!Landscape->HasTargetLayer(L.Name))
		{
			Landscape->AddTargetLayer(L.Name, FLandscapeTargetLayerSettings(LayerInfoObject));
		}
		else
		{
			Landscape->UpdateTargetLayer(L.Name, FLandscapeTargetLayerSettings(LayerInfoObject));
		}
		Infos.Add(LayerInfoObject);
	}
	if (ULandscapeInfo* LayerMapInfo = Landscape->GetLandscapeInfo(); LayerMapInfo && Layers.Num() > 0)
	{
		LayerMapInfo->UpdateLayerInfoMap(Landscape);
	}

	const FGuid LayerGuid = LilaLayer(Landscape);
	ULandscapeInfo* Info = Landscape->GetLandscapeInfo();
	if (!Info || !LayerGuid.IsValid())
	{
		Transaction.Cancel();
		OutError = TEXT("setLandscape: the landscape has no Lila edit layer");
		return nullptr;
	}
	{
		FScopedSetLandscapeEditingLayer Scope(Landscape, LayerGuid, [Landscape] { Landscape->RequestLayersContentUpdateForceAll(); });
		FLandscapeEditDataInterface Edit(Info, LayerGuid);
		Edit.SetHeightData(0, 0, SizeX - 1, SizeY - 1, Heights.GetData(), 0, true);
		for (int32 Index = 0; Index < Layers.Num(); ++Index)
		{
			if (Infos[Index])
			{
				Edit.SetAlphaData(Infos[Index], 0, 0, SizeX - 1, SizeY - 1, Weights[Index].GetData(), 0);
			}
		}
	}
	if (GridSize > 0)
	{
		if (ULandscapeSubsystem* Subsystem = World->GetSubsystem<ULandscapeSubsystem>(); Subsystem && Subsystem->IsGridBased())
		{
			Subsystem->ChangeGridSize(Info, static_cast<uint32>(GridSize));
		}
	}

	Landscape->Tags.RemoveAll([](const FName& T) { return T.ToString().StartsWith(TEXT("LilaHash:")); });
	if (!Hash.IsEmpty())
	{
		Landscape->Tags.Add(HashTag);
	}

	TSharedRef<FJsonObject> Out = MakeShared<FJsonObject>();
	Out->SetStringField(TEXT("actor"), Landscape->GetActorLabel());
	Out->SetBoolField(TEXT("created"), bCreated);
	Out->SetArrayField(TEXT("resolution"), {MakeShared<FJsonValueNumber>(SizeX), MakeShared<FJsonValueNumber>(SizeY)});
	Out->SetStringField(TEXT("editLayer"), LilaLayerName.ToString());
	TArray<TSharedPtr<FJsonValue>> LayerNames, CreatedValues, WarningValues;
	for (int32 Index = 0; Index < Layers.Num(); ++Index)
	{
		if (Infos[Index])
		{
			LayerNames.Add(MakeShared<FJsonValueString>(Layers[Index].Name.ToString()));
		}
	}
	for (const FString& C : Created)
	{
		CreatedValues.Add(MakeShared<FJsonValueString>(C));
	}
	for (const FString& W : Warnings)
	{
		WarningValues.Add(MakeShared<FJsonValueString>(W));
	}
	Out->SetArrayField(TEXT("layers"), LayerNames);
	Out->SetStringField(TEXT("material"), Landscape->LandscapeMaterial ? Landscape->LandscapeMaterial->GetPathName() : FString());
	Out->SetArrayField(TEXT("assets"), CreatedValues);
	Out->SetArrayField(TEXT("warnings"), WarningValues);
	UE_LOG(LogLilaLink, Log, TEXT("setLandscape: %s %dx%d, %d layers (%s)"), *Label, SizeX, SizeY, LayerNames.Num(), bCreated ? TEXT("created") : TEXT("updated"));
	return MakeShared<FJsonValueObject>(Out);
}

TSharedPtr<FJsonValue> RemoveLandscape(const TSharedPtr<FJsonObject>& Params, FString& OutError)
{
	TRACE_CPUPROFILER_EVENT_SCOPE(LilaLinkLandscape::RemoveLandscape);

	FString Id;
	if (!Params->TryGetStringField(TEXT("id"), Id) || Id.IsEmpty())
	{
		OutError = TEXT("removeLandscape: give id");
		return nullptr;
	}
	UWorld* World = EditorWorld();
	if (!World)
	{
		OutError = TEXT("removeLandscape: no editor world");
		return nullptr;
	}
	FScopedTransaction Transaction(NSLOCTEXT("LilaLink", "RemoveLandscape", "Lila: Remove Landscape"));
	bool bRemoved = false;
	if (ALandscape* Landscape = FindTaggedLandscape(World, TagFor(Id)))
	{
		bRemoved = World->EditorDestroyActor(Landscape, true);
	}
	TSharedRef<FJsonObject> Out = MakeShared<FJsonObject>();
	Out->SetBoolField(TEXT("removed"), bRemoved);
	return MakeShared<FJsonValueObject>(Out);
}
} // namespace

void LilaLink::AddLandscapeMethods(TMap<FString, FLilaLinkMethod>& Methods)
{
	Methods.Add(TEXT("setLandscape"), LilaLink::Sync(&SetLandscape));
	Methods.Add(TEXT("removeLandscape"), LilaLink::Sync(&RemoveLandscape));
}

namespace LilaLink::Landscape
{
UWorld* EditorWorld()
{
	return ::EditorWorld();
}

ALandscape* FindLandscape(UWorld* World, const FString& Id)
{
	return FindTaggedLandscape(World, TagFor(Id));
}

FName LandscapeTag(const FString& Id)
{
	return TagFor(Id);
}

bool HasLayout(ALandscape* Landscape, int32 SizeX, int32 SizeY, int32 QuadsPerSection, int32 SectionsPerComponent)
{
	return SameLayout(Landscape, SizeX, SizeY, QuadsPerSection, SectionsPerComponent);
}

ALandscape* CreateFlatLandscape(UWorld* World, const FString& Label, const FVector& Location, const FRotator& Rotation, const FVector& Scale, int32 SizeX,
	int32 SizeY, int32 QuadsPerSection, int32 SectionsPerComponent)
{
	return CreateLandscape(World, Label, Location, Rotation, Scale, SizeX, SizeY, QuadsPerSection, SectionsPerComponent);
}

FGuid LilaEditLayer(ALandscape* Landscape)
{
	return LilaLayer(Landscape);
}

UMaterial* GeneratedMaterial(const FString& Label, const TArray<FName>& Layers, TArray<FString>& OutCreated, TArray<FString>& OutWarnings)
{
	TArray<FLayerSend> Sends;
	for (const FName& Name : Layers)
	{
		Sends.AddDefaulted_GetRef().Name = Name;
	}
	return LandscapeMaterial(Label, Sends, OutCreated, OutWarnings);
}

ULandscapeLayerInfoObject* LayerInfoFor(const FString& Label, FName Layer, TArray<FString>& OutCreated)
{
	return LayerInfo(Label, Layer, OutCreated);
}
} // namespace LilaLink::Landscape
