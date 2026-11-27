#include "LilaLinkMeshTerrain.h"

#include "LilaLinkMaterialFunction.h"
#include "LilaLinkMethodRegistry.h"

#include "Dom/JsonObject.h"
#include "DynamicMesh/DynamicMesh3.h"
#include "DynamicMesh/DynamicMeshAttributeSet.h"
#include "Editor.h"
#include "Engine/Texture.h"
#include "Engine/World.h"
#include "EngineUtils.h"
#include "MaterialEditingLibrary.h"
#include "Materials/Material.h"
#include "Materials/MaterialExpressionConstant.h"
#include "Materials/MaterialExpressionConstant3Vector.h"
#include "Materials/MaterialExpressionFunctionInput.h"
#include "Materials/MaterialExpressionMaterialFunctionCall.h"
#include "Materials/MaterialExpressionScalarParameter.h"
#include "Materials/MaterialExpressionTextureObjectParameter.h"
#include "Materials/MaterialExpressionVectorParameter.h"
#include "Materials/MaterialFunction.h"
#include "Materials/MaterialInterface.h"
#include "MeshPartition.h"
#include "MeshPartitionChannel.h"
#include "MeshPartitionDefinition.h"
#include "MeshPartitionMaterialExpressionUtils.h"
#include "Misc/FileHelper.h"
#include "MeshPartitionEditorComponent.h"
#include "MeshPartitionEditorSubsystem.h"
#include "MeshPartitionHeightmapImporter.h"
#include "MeshPartitionMeshData.h"
#include "MeshPartitionPreviewSection.h"
#include "MeshPartitionModifierComponent.h"
#include "Components/SplineComponent.h"
#include "Misc/PackageName.h"
#include "Modifiers/MeshPartitionMeshProvider.h"
// The spline modifier's inline helper calls an experimental USplineComponent API.
PRAGMA_DISABLE_EXPERIMENTAL_WARNINGS
#include "Modifiers/MeshPartitionSplineModifier.h"
PRAGMA_ENABLE_EXPERIMENTAL_WARNINGS
#include "ProfilingDebugging/CpuProfilerTrace.h"
#include "ScopedTransaction.h"

namespace
{
using UE::MeshPartition::AMeshPartition;
using UE::MeshPartition::APreviewSection;
using UE::MeshPartition::UMeshPartitionDefinition;
using UE::MeshPartition::UMeshPartitionEditorComponent;
using UE::MeshPartition::UMeshProviderModifier;
using UE::MeshPartition::UModifierComponent;
using UE::MeshPartition::USplineModifier;

const TCHAR* SectionTagPrefix = TEXT("LilaLink.MeshTerrainSection:");

FName TagFor(const FString& Id)
{
	return FName(*(TEXT("LilaLink.MeshTerrain:") + Id.Right(900)));
}

UWorld* EditorWorld()
{
	return GEditor ? GEditor->GetEditorWorldContext().World() : nullptr;
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

bool ReadPoint(const TSharedPtr<FJsonObject>& Object, const TCHAR* Field, FInt32Point& Out)
{
	const TArray<TSharedPtr<FJsonValue>>* Values = nullptr;
	if (!Object->TryGetArrayField(Field, Values) || Values->Num() != 2)
	{
		return false;
	}
	Out = FInt32Point(static_cast<int32>((*Values)[0]->AsNumber()), static_cast<int32>((*Values)[1]->AsNumber()));
	return true;
}

AMeshPartition* FindTagged(UWorld* World, FName Tag)
{
	for (TActorIterator<AMeshPartition> It(World); It; ++It)
	{
		if (It->ActorHasTag(Tag))
		{
			return *It;
		}
	}
	return nullptr;
}

/** The section key the importer's label carries ("Section_X3-Y1" gives "X3-Y1"), or the actor's section tag. */
FString SectionKey(const AActor* Actor)
{
	for (const FName& Tag : Actor->Tags)
	{
		const FString S = Tag.ToString();
		if (S.StartsWith(SectionTagPrefix))
		{
			return S.RightChop(FCString::Strlen(SectionTagPrefix));
		}
	}
	FString Label = Actor->GetActorLabel();
	return Label.RemoveFromStart(TEXT("Section_")) ? Label : FString();
}

/** Base modifier actors Lila made for this id, by section key. */
TMap<FString, AActor*> TaggedSections(UWorld* World, FName Tag)
{
	TRACE_CPUPROFILER_EVENT_SCOPE(LilaLinkMeshTerrain::TaggedSections);

	TMap<FString, AActor*> Out;
	for (TActorIterator<AActor> It(World); It; ++It)
	{
		if (It->ActorHasTag(Tag) && !It->IsA<AMeshPartition>() && It->FindComponentByClass<UMeshProviderModifier>())
		{
			Out.Add(SectionKey(*It), *It);
		}
	}
	return Out;
}

void NotifyMoved(AMeshPartition* Mesh, const TArray<FBox>& Bounds)
{
	if (UMeshPartitionEditorComponent* Editor = Mesh ? Cast<UMeshPartitionEditorComponent>(Mesh->GetMeshPartitionComponent()) : nullptr)
	{
		Editor->OnBoundsChanged(Bounds, UE::MeshPartition::EChangeType::StateChange);
	}
}

/** A paint layer as a Mesh Partition channel: its name and one weight byte per heightmap mesh vertex. */
struct FLilaChannel
{
	FName Name;
	TArray<uint8> Weights;
};

bool ReadChannels(const TSharedPtr<FJsonObject>& Params, FInt32Point Resolution, TArray<FLilaChannel>& Out, FString& OutError)
{
	const TArray<TSharedPtr<FJsonValue>>* Values = nullptr;
	if (!Params->TryGetArrayField(TEXT("channels"), Values))
	{
		return true;
	}
	const int64 Expected = int64(Resolution.X + 1) * int64(Resolution.Y + 1);
	for (const TSharedPtr<FJsonValue>& Value : *Values)
	{
		const TSharedPtr<FJsonObject>* Object = nullptr;
		FString Name, File;
		if (!Value.IsValid() || !Value->TryGetObject(Object) || !(*Object)->TryGetStringField(TEXT("name"), Name) || !(*Object)->TryGetStringField(TEXT("file"), File))
		{
			OutError = TEXT("setMeshTerrain: each channel needs name and file");
			return false;
		}
		FLilaChannel& C = Out.AddDefaulted_GetRef();
		C.Name = FName(*Name);
		if (!FFileHelper::LoadFileToArray(C.Weights, *File) || C.Weights.Num() != Expected)
		{
			OutError = FString::Printf(TEXT("setMeshTerrain: channel %s: %s is not %lld bytes"), *Name, *File, Expected);
			return false;
		}
	}
	return true;
}

/** The terrain's own definition; the engine's default one is shared, so a terrain still on it gets its own copy first. */
UMeshPartitionDefinition* OwnDefinition(AMeshPartition* Mesh)
{
	UMeshPartitionDefinition* Definition = Mesh->GetMeshPartitionDefinition();
	if (!Definition || Definition == UMeshPartitionDefinition::GetDefaultMegaMeshDefinition())
	{
		const UMeshPartitionDefinition* Source = Definition ? Definition : UMeshPartitionDefinition::GetDefaultMegaMeshDefinition();
		Definition = DuplicateObject<UMeshPartitionDefinition>(Source, Mesh, MakeUniqueObjectName(Mesh, UMeshPartitionDefinition::StaticClass(), TEXT("LilaDefinition")));
		Mesh->Modify();
		Mesh->SetMeshPartitionDefinition(Definition);
	}
	return Definition;
}

/**
 * Makes Material the one the terrain's sections draw with. Mesh Partition builds every section with its definition's
 * material and ignores the providers' own slots.
 */
void SetDefinitionMaterial(AMeshPartition* Mesh, UMaterialInterface* Material, TArray<FString>& Warnings)
{
	TRACE_CPUPROFILER_EVENT_SCOPE(LilaLinkMeshTerrain::SetDefinitionMaterial);

	if (!Mesh || !Material)
	{
		return;
	}
	UMeshPartitionDefinition* Definition = OwnDefinition(Mesh);
	// No setter either; the details panel writes it the same way.
	FObjectProperty* Property = FindFProperty<FObjectProperty>(UMeshPartitionDefinition::StaticClass(), TEXT("Material"));
	if (!Property)
	{
		Warnings.Add(TEXT("this engine's Mesh Partition definition has no Material, so the sections keep the default material"));
		return;
	}
	Definition->Modify();
	Property->SetObjectPropertyValue_InContainer(Definition, Material);
	FPropertyChangedEvent Changed(Property);
	Definition->PostEditChangeProperty(Changed);
}

/** Declares the channels on the terrain's definition, so they are rasterized for materials to sample. */
void DeclareChannels(AMeshPartition* Mesh, const TArray<FLilaChannel>& Channels, TArray<FString>& Warnings)
{
	if (!Mesh || Channels.IsEmpty())
	{
		return;
	}
	UMeshPartitionDefinition* Definition = OwnDefinition(Mesh);
	// The channel list has no setter; it is reached through reflection like the details panel does.
	FStructProperty* MapProperty = FindFProperty<FStructProperty>(UMeshPartitionDefinition::StaticClass(), TEXT("ChannelMap"));
	FArrayProperty* DescsProperty = MapProperty ? FindFProperty<FArrayProperty>(MapProperty->Struct, TEXT("ChannelDescs")) : nullptr;
	if (!DescsProperty)
	{
		Warnings.Add(TEXT("this engine's Mesh Partition definition has no ChannelMap.ChannelDescs, so the channels were written to the mesh but not declared"));
		return;
	}
	Definition->Modify();
	void* Map = MapProperty->ContainerPtrToValuePtr<void>(Definition);
	TArray<UE::MeshPartition::FChannelDesc>& Descs = *DescsProperty->ContainerPtrToValuePtr<TArray<UE::MeshPartition::FChannelDesc>>(Map);
	for (const FLilaChannel& C : Channels)
	{
		if (!Descs.ContainsByPredicate([&](const UE::MeshPartition::FChannelDesc& D) { return D.Name == C.Name; }))
		{
			if (Descs.Num() >= UE::MeshPartition::FChannelPacking::MaxNumberPackedChannels)
			{
				Warnings.Add(FString::Printf(TEXT("channel %s left out: the definition already has %d channels"), *C.Name.ToString(), Descs.Num()));
				continue;
			}
			Descs.AddDefaulted_GetRef().Name = C.Name;
		}
	}
	FPropertyChangedEvent Changed(MapProperty);
	Definition->PostEditChangeProperty(Changed);
}

/** Writes the channels into a section's base mesh as weight layers, sampled at each vertex's place in the heightmap mesh. */
void WriteChannels(AActor* Section, const TArray<FLilaChannel>& Channels, FInt32Point Resolution, const FVector& MeshSize)
{
	UMeshProviderModifier* Provider = Section ? Section->FindComponentByClass<UMeshProviderModifier>() : nullptr;
	if (!Provider || !Provider->GetMesh() || Channels.IsEmpty())
	{
		return;
	}
	UE::Geometry::FDynamicMesh3 Mesh(*Provider->GetMesh());
	Mesh.EnableAttributes();
	Mesh.Attributes()->SetNumWeightLayers(Channels.Num());
	const FTransform ToImport = Section->GetActorTransform();
	const int32 Stride = Resolution.X + 1;
	for (int32 Index = 0; Index < Channels.Num(); ++Index)
	{
		UE::Geometry::FDynamicMeshWeightAttribute* Layer = Mesh.Attributes()->GetWeightLayer(Index);
		Layer->SetName(Channels[Index].Name);
		const TArray<uint8>& W = Channels[Index].Weights;
		for (const int32 Vid : Mesh.VertexIndicesItr())
		{
			// The importer's mesh is centred on the origin: vertex k of Resolution.X sits at (k / X - 0.5) * MeshSize.X.
			const FVector P = ToImport.TransformPosition(Mesh.GetVertex(Vid));
			const double K = FMath::Clamp((P.X / MeshSize.X + 0.5) * Resolution.X, 0.0, double(Resolution.X));
			const double L = FMath::Clamp((P.Y / MeshSize.Y + 0.5) * Resolution.Y, 0.0, double(Resolution.Y));
			const int32 K0 = FMath::Min(int32(K), Resolution.X - 1), L0 = FMath::Min(int32(L), Resolution.Y - 1);
			const double FK = K - K0, FL = L - L0;
			auto At = [&](int32 X, int32 Y) { return double(W[Y * Stride + X]) / 255.0; };
			const float Value = float(FMath::Lerp(FMath::Lerp(At(K0, L0), At(K0 + 1, L0), FK), FMath::Lerp(At(K0, L0 + 1), At(K0 + 1, L0 + 1), FK), FL));
			Layer->SetValue(Vid, &Value);
		}
	}
	Provider->SetMesh(MoveTemp(Mesh), false);
}

const TCHAR* ChannelFunctionName = TEXT("MF_LilaMeshTerrainChannel");
const TCHAR* ChannelVersionTag = TEXT("Lila Mesh Terrain Channel v");
constexpr int32 ChannelFunctionVersion = 2;
const TCHAR* MeshTerrainFolder = TEXT("/Game/LilaLink/MeshTerrain");

/** A material layer of Lila's terrain sent as a channel: the channel's name and the layer's look. */
struct FLilaMaterialLayer
{
	FName Channel;
	FLinearColor Color = FLinearColor(0.5f, 0.5f, 0.5f);
	float Roughness = 0.85f;
	float TileSize = 400.0f;
};

void ReadMaterialLayers(const TSharedPtr<FJsonObject>& Params, TArray<FLilaMaterialLayer>& Out)
{
	const TArray<TSharedPtr<FJsonValue>>* Values = nullptr;
	if (!Params->TryGetArrayField(TEXT("materialLayers"), Values))
	{
		return;
	}
	for (const TSharedPtr<FJsonValue>& Value : *Values)
	{
		const TSharedPtr<FJsonObject>* Object = nullptr;
		FString Name;
		if (!Value.IsValid() || !Value->TryGetObject(Object) || !(*Object)->TryGetStringField(TEXT("name"), Name))
		{
			continue;
		}
		FLilaMaterialLayer& L = Out.AddDefaulted_GetRef();
		L.Channel = FName(*Name);
		FVector Color;
		if (ReadVector(*Object, TEXT("color"), Color))
		{
			L.Color = FLinearColor(float(Color.X), float(Color.Y), float(Color.Z));
		}
		double Number = 0.0;
		if ((*Object)->TryGetNumberField(TEXT("roughness"), Number))
		{
			L.Roughness = float(Number);
		}
		if ((*Object)->TryGetNumberField(TEXT("tileSize"), Number) && Number > 0.0)
		{
			L.TileSize = float(Number);
		}
	}
}

int32 ChannelFunctionVersionOf(const UMaterialFunction* Function)
{
	const FString& D = Function->Description;
	return D.StartsWith(ChannelVersionTag) ? FCString::Atoi(*D.RightChop(FCString::Strlen(ChannelVersionTag))) : 0;
}

/**
 * MF_LilaMeshTerrainChannel: a Mesh Partition channel's weight (0 to 1) at the pixel, by the channel's index in the
 * terrain's definition. setMeshTerrain reports each channel's index. The index must be a constant: Mesh Partition looks the
 * channel up in the section's channel table when the shader compiles, and a parameter there reads nothing.
 */
void BuildChannelFunction(UMaterialFunction* Function)
{
	TRACE_CPUPROFILER_EVENT_SCOPE(LilaLinkMeshTerrain::BuildChannelFunction);

	const FString Description = FString::Printf(TEXT("%s%d: the weight of a Mesh Partition channel at the pixel, by the channel's index in the definition."),
		ChannelVersionTag, ChannelFunctionVersion);
	LilaLink::FFunctionGraph G(Function, *Description);
	UMaterialExpressionFunctionInput* Index = G.ScalarInput(TEXT("ChannelIndex"), 0.0f, TEXT("Channel"), TEXT("The channel's index in the Mesh Partition definition. A Constant, never a parameter."));
	auto* Resource = G.New<UE::MeshPartition::UMaterialExpressionMeshPartitionResource>(-800);
	auto* Sample = G.New<UE::MeshPartition::UMaterialExpressionMeshPartitionChannelSampleIndex>(-400);
	Sample->ChannelTextureInput.Connect(0, Resource);
	Sample->ChannelIndex.Connect(0, Index);
	G.Output(Sample, TEXT("Weight"), TEXT("The channel's weight at the pixel, 0 to 1."));
	G.Finish();
}

UMaterialFunction* ChannelFunction(TArray<FString>& Warnings)
{
	bool bCreated = false;
	UMaterialFunction* Function = LilaLink::FindOrCreateMaterialFunction(LilaLink::MastersFolderPath(), ChannelFunctionName, bCreated);
	if (!Function)
	{
		Warnings.Add(FString::Printf(TEXT("could not create %s in %s"), ChannelFunctionName, LilaLink::MastersFolderPath()));
		return nullptr;
	}
	if (bCreated || ChannelFunctionVersionOf(Function) < ChannelFunctionVersion)
	{
		BuildChannelFunction(Function);
	}
	return Function;
}

/**
 * M_<Label>_MeshTerrain: per material layer, MF_LilaMeshTerrainChannel reads the layer's channel and feeds a chain of
 * MF_LilaTerrainLayer calls: the channel index a constant, the layer's tint, roughness, texture and tile size parameters
 * (defaults from Lila). Rebuilt on every send so the defaults follow Lila's rules.
 */
UMaterial* MeshTerrainMaterial(const FString& Label, const AMeshPartition* Mesh, const TArray<FLilaMaterialLayer>& Layers, TArray<FString>& OutAssets,
	TArray<FString>& Warnings)
{
	TRACE_CPUPROFILER_EVENT_SCOPE(LilaLinkMeshTerrain::MeshTerrainMaterial);

	const UMeshPartitionDefinition* Definition = Mesh ? Mesh->GetMeshPartitionDefinition() : nullptr;
	UMaterialFunction* Channel = ChannelFunction(Warnings);
	UMaterialFunction* Layer = LilaLink::TerrainLayerFunction(Warnings);
	if (!Definition || !Channel || !Layer)
	{
		return nullptr;
	}
	const FString Folder = FString(MeshTerrainFolder) / Label;
	const FString Name = TEXT("M_") + Label + TEXT("_MeshTerrain");
	bool bCreated = false;
	UMaterial* Material = LilaLink::FindOrCreateMaterial(Folder, Name, bCreated);
	if (!Material)
	{
		Warnings.Add(FString::Printf(TEXT("could not create %s in %s"), *Name, *Folder));
		return nullptr;
	}
	Material->Modify();
	LilaLink::DeleteAllExpressions(Material);
	int32 Row = 0;
	auto Make = [&](TSubclassOf<UMaterialExpression> Class, int32 X) { return UMaterialEditingLibrary::CreateMaterialExpression(Material, Class, X, (Row++) * 140); };
	auto* Zero = Cast<UMaterialExpressionConstant3Vector>(Make(UMaterialExpressionConstant3Vector::StaticClass(), -1500));
	Zero->Constant = FLinearColor(0.0f, 0.0f, 0.0f);
	auto* ZeroRough = Cast<UMaterialExpressionConstant>(Make(UMaterialExpressionConstant::StaticClass(), -1500));
	ZeroRough->R = 0.0f;
	UMaterialExpression* Color = Zero;
	UMaterialExpression* Rough = ZeroRough;
	FString ColorOut, RoughOut;
	UTexture* White = LoadObject<UTexture>(nullptr, TEXT("/Engine/EngineResources/WhiteSquareTexture.WhiteSquareTexture"));
	int32 Placed = 0;
	for (const FLilaMaterialLayer& L : Layers)
	{
		const int32 ChannelIndex = Definition->GetChannelMap().FindChannel(L.Channel);
		if (ChannelIndex == INDEX_NONE)
		{
			Warnings.Add(FString::Printf(TEXT("material layer %s left out of %s: the Mesh Terrain has no such channel"), *L.Channel.ToString(), *Name));
			continue;
		}
		const FString LayerName = L.Channel.ToString();
		auto Param = [&](TSubclassOf<UMaterialExpression> Class, const FString& Suffix) {
			auto* P = Cast<UMaterialExpressionParameter>(Make(Class, -1200));
			P->ParameterName = *(LayerName + Suffix);
			P->Group = *LayerName;
			return P;
		};
		auto* Index = Cast<UMaterialExpressionConstant>(Make(UMaterialExpressionConstant::StaticClass(), -1200));
		Index->R = float(ChannelIndex);
		auto* Tint = Cast<UMaterialExpressionVectorParameter>(Param(UMaterialExpressionVectorParameter::StaticClass(), TEXT("Tint")));
		Tint->DefaultValue = L.Color;
		auto* LayerRough = Cast<UMaterialExpressionScalarParameter>(Param(UMaterialExpressionScalarParameter::StaticClass(), TEXT("Roughness")));
		LayerRough->DefaultValue = L.Roughness;
		auto* Tile = Cast<UMaterialExpressionScalarParameter>(Param(UMaterialExpressionScalarParameter::StaticClass(), TEXT("TileSize")));
		Tile->DefaultValue = L.TileSize;
		auto* Texture = Cast<UMaterialExpressionTextureObjectParameter>(Make(UMaterialExpressionTextureObjectParameter::StaticClass(), -1200));
		Texture->ParameterName = *(LayerName + TEXT("Texture"));
		Texture->Group = *LayerName;
		Texture->Texture = White;

		auto* Weight = Cast<UMaterialExpressionMaterialFunctionCall>(Make(UMaterialExpressionMaterialFunctionCall::StaticClass(), -800));
		Weight->SetMaterialFunction(Channel);
		UMaterialEditingLibrary::ConnectMaterialExpressions(Index, TEXT(""), Weight, TEXT("ChannelIndex"));
		auto* Call = Cast<UMaterialExpressionMaterialFunctionCall>(Make(UMaterialExpressionMaterialFunctionCall::StaticClass(), -400 + Placed * 300));
		Call->SetMaterialFunction(Layer);
		UMaterialEditingLibrary::ConnectMaterialExpressions(Color, ColorOut, Call, TEXT("Accum"));
		UMaterialEditingLibrary::ConnectMaterialExpressions(Rough, RoughOut, Call, TEXT("AccumRoughness"));
		UMaterialEditingLibrary::ConnectMaterialExpressions(Weight, TEXT("Weight"), Call, TEXT("Weight"));
		UMaterialEditingLibrary::ConnectMaterialExpressions(Tint, TEXT(""), Call, TEXT("LayerTint"));
		UMaterialEditingLibrary::ConnectMaterialExpressions(LayerRough, TEXT(""), Call, TEXT("LayerRoughness"));
		UMaterialEditingLibrary::ConnectMaterialExpressions(Texture, TEXT(""), Call, TEXT("LayerTexture"));
		UMaterialEditingLibrary::ConnectMaterialExpressions(Tile, TEXT(""), Call, TEXT("LayerTileSize"));
		Color = Rough = Call;
		ColorOut = TEXT("BaseColor");
		RoughOut = TEXT("Roughness");
		++Placed;
	}
	if (Placed == 0)
	{
		return nullptr;
	}
	UMaterialEditingLibrary::ConnectMaterialProperty(Color, ColorOut, MP_BaseColor);
	UMaterialEditingLibrary::ConnectMaterialProperty(Rough, RoughOut, MP_Roughness);
	Material->PostEditChange();
	Material->MarkPackageDirty();
	UMaterialEditingLibrary::RecompileMaterial(Material);
	OutAssets.AddUnique(Material->GetPathName());
	OutAssets.AddUnique(Channel->GetPathName());
	OutAssets.AddUnique(Layer->GetPathName());
	return Material;
}

TSharedPtr<FJsonValue> SetMeshTerrain(const TSharedPtr<FJsonObject>& Params, FString& OutError)
{
	TRACE_CPUPROFILER_EVENT_SCOPE(LilaLinkMeshTerrain::SetMeshTerrain);

	FString Id, Heightmap, MaterialPath;
	if (!Params->TryGetStringField(TEXT("id"), Id) || Id.IsEmpty() || !Params->TryGetStringField(TEXT("heightmap"), Heightmap))
	{
		OutError = TEXT("setMeshTerrain: give id and heightmap");
		return nullptr;
	}
	FInt32Point MeshResolution, Sections;
	FVector MeshSize, Location = FVector::ZeroVector, PitchYawRoll = FVector::ZeroVector;
	if (!ReadPoint(Params, TEXT("meshResolution"), MeshResolution) || !ReadPoint(Params, TEXT("sections"), Sections) ||
		!ReadVector(Params, TEXT("meshSize"), MeshSize))
	{
		OutError = TEXT("setMeshTerrain: give meshResolution [x, y], sections [x, y] and meshSize [x, y, z]");
		return nullptr;
	}
	ReadVector(Params, TEXT("location"), Location);
	ReadVector(Params, TEXT("rotation"), PitchYawRoll);
	FString Label = FPackageName::GetShortName(Id);
	Params->TryGetStringField(TEXT("label"), Label);
	Params->TryGetStringField(TEXT("material"), MaterialPath);
	bool bSaveAndUnload = false;
	Params->TryGetBoolField(TEXT("saveAndUnload"), bSaveAndUnload);
	TArray<FLilaChannel> Channels;
	if (!ReadChannels(Params, MeshResolution, Channels, OutError))
	{
		return nullptr;
	}
	TArray<FLilaMaterialLayer> MaterialLayers;
	ReadMaterialLayers(Params, MaterialLayers);
	UWorld* World = EditorWorld();
	if (!World)
	{
		OutError = TEXT("setMeshTerrain: no editor world");
		return nullptr;
	}

	const FName Tag = TagFor(Id);
	AMeshPartition* Old = FindTagged(World, Tag);
	if (Old && bSaveAndUnload)
	{
		OutError = TEXT("setMeshTerrain: saveAndUnload can't update a sent terrain whose sections are unloaded; removeMeshTerrain it first");
		return nullptr;
	}
	TArray<FString> Warnings;
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

	FScopedTransaction Transaction(NSLOCTEXT("LilaLink", "SetMeshTerrain", "Lila: Update Mesh Terrain"));
	TSet<AActor*> Before;
	for (TActorIterator<AActor> It(World); It; ++It)
	{
		Before.Add(*It);
	}
	UE::MeshPartition::FHeightmapImportParams Import;
	Import.World = World;
	Import.HeightmapFilename = Heightmap;
	Import.MeshResolution = MeshResolution;
	Import.MeshSize = MeshSize;
	Import.SectionsResolution = Sections;
	Import.bSaveAndUnload = bSaveAndUnload;
	Import.LocationVolumesResolution = FInt32Point(0);
	UE::MeshPartition::FHeightmapImporter Importer(Import);
	if (!Importer.Import(nullptr))
	{
		Transaction.Cancel();
		OutError = TEXT("setMeshTerrain: ") + Importer.GetErrorText().ToString();
		return nullptr;
	}
	AMeshPartition* Made = Importer.GetMegaMesh();
	TArray<AActor*> NewSections;
	for (TActorIterator<AActor> It(World); It; ++It)
	{
		if (!Before.Contains(*It) && *It != Made && It->FindComponentByClass<UMeshProviderModifier>())
		{
			NewSections.Add(*It);
		}
	}

	for (AActor* Section : NewSections)
	{
		WriteChannels(Section, Channels, MeshResolution, MeshSize);
	}

	const FTransform Place(FRotator(PitchYawRoll.X, PitchYawRoll.Y, PitchYawRoll.Z), Location);
	TArray<FBox> Moved;
	if (bSaveAndUnload)
	{
		Warnings.Add(TEXT("saveAndUnload: the sections stay where the importer put them, centred on the origin from Z 0; move the terrain in Lila to the origin or send without it"));
	}
	else
	{
		for (AActor* Section : NewSections)
		{
			for (const FBox& B : Section->FindComponentByClass<UMeshProviderModifier>()->ComputeBounds())
			{
				Moved.Add(B);
			}
			Section->SetActorTransform(Section->GetActorTransform() * Place);
			for (const FBox& B : Section->FindComponentByClass<UMeshProviderModifier>()->ComputeBounds())
			{
				Moved.Add(B);
			}
		}
	}

	bool bCreated = true;
	AMeshPartition* Result = Made;
	TMap<FString, AActor*> OldSections = Old ? TaggedSections(World, Tag) : TMap<FString, AActor*>();
	TSet<FString> NewKeys;
	for (AActor* Section : NewSections)
	{
		NewKeys.Add(SectionKey(Section));
	}
	bool bSameSections = Old && OldSections.Num() == NewSections.Num();
	for (const FString& Key : NewKeys)
	{
		bSameSections &= OldSections.Contains(Key);
	}

	if (bSameSections)
	{
		// The same sections: new meshes into the old base modifiers, so everything sitting on that Mesh Partition stays.
		TArray<FBox> Changed;
		for (AActor* Section : NewSections)
		{
			AActor* Target = OldSections[SectionKey(Section)];
			UMeshProviderModifier* From = Section->FindComponentByClass<UMeshProviderModifier>();
			UMeshProviderModifier* To = Target->FindComponentByClass<UMeshProviderModifier>();
			Changed.Append(To->ComputeBounds());
			Target->Modify();
			Target->SetActorTransform(Section->GetActorTransform());
			UE::Geometry::FDynamicMesh3 Copy(*From->GetMesh());
			To->SetMesh(MoveTemp(Copy), true);
			Changed.Append(To->ComputeBounds());
			if (Material)
			{
				To->SetMaterial(0, Material);
			}
		}
		for (AActor* Section : NewSections)
		{
			World->EditorDestroyActor(Section, true);
		}
		World->EditorDestroyActor(Made, true);
		Result = Old;
		bCreated = false;
		NotifyMoved(Old, Changed);
	}
	else
	{
		if (Old)
		{
			// New sections: the user's modifiers move to the new Mesh Partition, Lila's old base goes.
			for (TActorIterator<AActor> It(World); It; ++It)
			{
				if (It->ActorHasTag(Tag))
				{
					continue;
				}
				TArray<UModifierComponent*> Modifiers;
				It->GetComponents(Modifiers);
				for (UModifierComponent* M : Modifiers)
				{
					if (M->GetAffectedMeshPartition() == Old)
					{
						M->SetAffectedMeshPartition(Made);
					}
				}
			}
			for (const TPair<FString, AActor*>& S : OldSections)
			{
				World->EditorDestroyActor(S.Value, true);
			}
			World->EditorDestroyActor(Old, true);
			Warnings.Add(TEXT("the sections changed, so the Mesh Partition actor was replaced; your modifiers on it moved to the new one"));
		}
		Made->Tags.AddUnique(Tag);
		Made->SetActorLabel(Label);
		for (AActor* Section : NewSections)
		{
			Section->Tags.AddUnique(Tag);
			Section->Tags.AddUnique(FName(*(FString(SectionTagPrefix) + SectionKey(Section))));
			if (Material)
			{
				Section->FindComponentByClass<UMeshProviderModifier>()->SetMaterial(0, Material);
			}
		}
		NotifyMoved(Made, Moved);
	}

	DeclareChannels(Result, Channels, Warnings);

	TArray<FString> Assets;
	UMaterial* Generated = nullptr;
	if (!Material && !MaterialLayers.IsEmpty())
	{
		Generated = MeshTerrainMaterial(Label, Result, MaterialLayers, Assets, Warnings);
		if (Generated)
		{
			for (const TPair<FString, AActor*>& S : TaggedSections(World, Tag))
			{
				S.Value->FindComponentByClass<UMeshProviderModifier>()->SetMaterial(0, Generated);
			}
		}
	}
	SetDefinitionMaterial(Result, Material ? Material : Generated, Warnings);

	TSharedRef<FJsonObject> Out = MakeShared<FJsonObject>();
	Out->SetStringField(TEXT("actor"), Result ? Result->GetActorLabel() : FString());
	TArray<TSharedPtr<FJsonValue>> ChannelValues;
	const UMeshPartitionDefinition* Definition = Result ? Result->GetMeshPartitionDefinition() : nullptr;
	for (const FLilaChannel& C : Channels)
	{
		ChannelValues.Add(MakeShared<FJsonValueString>(C.Name.ToString()));
	}
	Out->SetArrayField(TEXT("channels"), ChannelValues);
	// The index MF_LilaMeshTerrainChannel takes for each channel.
	TSharedRef<FJsonObject> Indices = MakeShared<FJsonObject>();
	for (const FLilaChannel& C : Channels)
	{
		Indices->SetNumberField(C.Name.ToString(), Definition ? Definition->GetChannelMap().FindChannel(C.Name) : INDEX_NONE);
	}
	Out->SetObjectField(TEXT("channelIndices"), Indices);
	Out->SetStringField(TEXT("material"), Material ? Material->GetPathName() : Generated ? Generated->GetPathName() : FString());
	TArray<TSharedPtr<FJsonValue>> AssetValues;
	for (const FString& A : Assets)
	{
		AssetValues.Add(MakeShared<FJsonValueString>(A));
	}
	Out->SetArrayField(TEXT("assets"), AssetValues);
	Out->SetBoolField(TEXT("created"), bCreated);
	Out->SetNumberField(TEXT("sections"), NewSections.Num());
	TArray<TSharedPtr<FJsonValue>> WarningValues;
	for (const FString& W : Warnings)
	{
		WarningValues.Add(MakeShared<FJsonValueString>(W));
	}
	Out->SetArrayField(TEXT("warnings"), WarningValues);
	return MakeShared<FJsonValueObject>(Out);
}

FName RoadsTagFor(const FString& Id)
{
	return FName(*(TEXT("LilaLink.MeshTerrainRoads:") + Id.Right(900)));
}

template <typename T>
T* PrivateMember(UObject* Object, const TCHAR* Name)
{
	FProperty* Property = FindFProperty<FProperty>(Object->GetClass(), Name);
	return Property ? Property->ContainerPtrToValuePtr<T>(Object) : nullptr;
}

/**
 * Roads on a Lila Mesh Terrain as Mesh Partition spline modifiers, one actor per road: the spline through the road's
 * points, a plateau of its half width and its falloff beyond, writing the road's channel and, when raise or lower is
 * on, pulling the surface to the road. Sending again replaces them.
 */
TSharedPtr<FJsonValue> SetMeshTerrainRoads(const TSharedPtr<FJsonObject>& Params, FString& OutError)
{
	TRACE_CPUPROFILER_EVENT_SCOPE(LilaLinkMeshTerrain::SetMeshTerrainRoads);

	FString Id;
	const TArray<TSharedPtr<FJsonValue>>* Roads = nullptr;
	if (!Params->TryGetStringField(TEXT("id"), Id) || !Params->TryGetArrayField(TEXT("roads"), Roads))
	{
		OutError = TEXT("setMeshTerrainRoads: give id and roads");
		return nullptr;
	}
	FVector Location = FVector::ZeroVector;
	ReadVector(Params, TEXT("location"), Location);
	UWorld* World = EditorWorld();
	AMeshPartition* Mesh = World ? FindTagged(World, TagFor(Id)) : nullptr;
	if (!Mesh)
	{
		OutError = TEXT("setMeshTerrainRoads: no Mesh Terrain from this terrain; send it with terrainSendToUnreal(kind='meshTerrain') first");
		return nullptr;
	}
	FScopedTransaction Transaction(NSLOCTEXT("LilaLink", "SetMeshTerrainRoads", "Lila: Update Mesh Terrain Roads"));
	const FName Tag = RoadsTagFor(Id);
	TArray<AActor*> Doomed;
	for (TActorIterator<AActor> It(World); It; ++It)
	{
		if (It->ActorHasTag(Tag))
		{
			Doomed.Add(*It);
		}
	}
	for (AActor* A : Doomed)
	{
		World->EditorDestroyActor(A, true);
	}
	const TArray<FName> Channels = Mesh->GetMeshPartitionDefinition() ? Mesh->GetMeshPartitionDefinition()->GetChannelNames() : TArray<FName>();
	UMeshPartitionEditorComponent* Editor = Cast<UMeshPartitionEditorComponent>(Mesh->GetMeshPartitionComponent());
	TArray<FString> Warnings;
	int32 Made = 0;
	for (const TSharedPtr<FJsonValue>& Value : *Roads)
	{
		const TSharedPtr<FJsonObject>* Road = nullptr;
		const TArray<TSharedPtr<FJsonValue>>* Points = nullptr;
		if (!Value.IsValid() || !Value->TryGetObject(Road) || !(*Road)->TryGetArrayField(TEXT("points"), Points) || Points->Num() < 2)
		{
			OutError = TEXT("setMeshTerrainRoads: each road needs at least two points");
			return nullptr;
		}
		FString RoadId, Layer;
		(*Road)->TryGetStringField(TEXT("id"), RoadId);
		(*Road)->TryGetStringField(TEXT("layer"), Layer);
		double HalfWidth = 300.0, Falloff = 500.0;
		bool bRaise = false, bLower = false, bClosed = false;
		(*Road)->TryGetNumberField(TEXT("halfWidth"), HalfWidth);
		(*Road)->TryGetNumberField(TEXT("sideFalloff"), Falloff);
		(*Road)->TryGetBoolField(TEXT("raiseTerrain"), bRaise);
		(*Road)->TryGetBoolField(TEXT("lowerTerrain"), bLower);
		(*Road)->TryGetBoolField(TEXT("closed"), bClosed);

		AActor* Actor = World->SpawnActor<AActor>(AActor::StaticClass(), FTransform::Identity);
		Actor->SetActorLabel(TEXT("Road_") + RoadId);
		Actor->Tags.Add(Tag);
		USplineComponent* Spline = NewObject<USplineComponent>(Actor, TEXT("Spline"), RF_Transactional);
		Actor->AddInstanceComponent(Spline);
		Actor->SetRootComponent(Spline);
		Spline->RegisterComponent();
		Spline->ClearSplinePoints(false);
		for (const TSharedPtr<FJsonValue>& PointValue : *Points)
		{
			const TSharedPtr<FJsonObject>* Point = nullptr;
			FVector Position, Arrive = FVector::ZeroVector, Leave = FVector::ZeroVector;
			if (!PointValue->TryGetObject(Point) || !ReadVector(*Point, TEXT("position"), Position))
			{
				OutError = TEXT("setMeshTerrainRoads: a point has no position");
				return nullptr;
			}
			Spline->AddSplinePoint(Location + Position, ESplineCoordinateSpace::World, false);
			const int32 Index = Spline->GetNumberOfSplinePoints() - 1;
			if (ReadVector(*Point, TEXT("arriveTangent"), Arrive) && ReadVector(*Point, TEXT("leaveTangent"), Leave))
			{
				Spline->SetTangentsAtSplinePoint(Index, Arrive, Leave, ESplineCoordinateSpace::World, false);
			}
		}
		Spline->SetClosedLoop(bClosed, false);
		Spline->UpdateSpline();

		USplineModifier* Modifier = NewObject<USplineModifier>(Actor, TEXT("RoadModifier"), RF_Transactional);
		Modifier->SetPriority(1.0);
		Modifier->SetAffectedMeshPartition(Mesh);
		Actor->AddInstanceComponent(Modifier);
		Modifier->AttachToComponent(Spline, FAttachmentTransformRules::KeepWorldTransform);
		Modifier->SetUseSplineScaleForFalloff(false);
		Modifier->SetFalloffDistance(float(FMath::Max(Falloff, 0.1)));
		Modifier->SetMeshClosedInterior(false);
		if (float* Plateau = PrivateMember<float>(Modifier, TEXT("PlateauDistance")))
		{
			*Plateau = float(HalfWidth);
		}
		using UE::MeshPartition::ESplineModifierWriteMode;
		ESplineModifierWriteMode Mode = ESplineModifierWriteMode(0);
		if (bRaise || bLower)
		{
			Mode |= ESplineModifierWriteMode::Positions;
			if (UE::MeshPartition::ESplineModifierBlendMode* Blend = PrivateMember<UE::MeshPartition::ESplineModifierBlendMode>(Modifier, TEXT("BlendMode")))
			{
				// Max moves only the surface below the road up to it, Min only the surface above it down.
				*Blend = bRaise && bLower ? UE::MeshPartition::ESplineModifierBlendMode::Normal
						 : bRaise		  ? UE::MeshPartition::ESplineModifierBlendMode::Max
										  : UE::MeshPartition::ESplineModifierBlendMode::Min;
			}
		}
		if (!Layer.IsEmpty())
		{
			if (Channels.Contains(FName(*Layer)))
			{
				Mode |= ESplineModifierWriteMode::Weights;
				if (TArray<UE::MeshPartition::FSplineModifierWeightEntry>* Entries = PrivateMember<TArray<UE::MeshPartition::FSplineModifierWeightEntry>>(Modifier, TEXT("WeightChannels")))
				{
					UE::MeshPartition::FSplineModifierWeightEntry& Entry = Entries->AddDefaulted_GetRef();
					Entry.WeightChannelName = UE::MeshPartition::FChannelName(FName(*Layer));
					Entry.Value = 1.0;
				}
			}
			else
			{
				Warnings.Add(FString::Printf(TEXT("%s: the Mesh Terrain has no channel %s, so the road paints none (send the mask as a layer)"), *RoadId, *Layer));
			}
		}
		if (uint8* WriteMode = PrivateMember<uint8>(Modifier, TEXT("WriteMode")))
		{
			*WriteMode = uint8(Mode);
		}
		Modifier->SetSplineComponent(Spline, true);
		Modifier->RegisterComponent();
		Modifier->UpdateSplineData();
		if (Editor)
		{
			Editor->OnModifierAssigned();
		}
		++Made;
	}
	TSharedRef<FJsonObject> Out = MakeShared<FJsonObject>();
	Out->SetNumberField(TEXT("modifiers"), Made);
	TArray<TSharedPtr<FJsonValue>> WarningValues;
	for (const FString& W : Warnings)
	{
		WarningValues.Add(MakeShared<FJsonValueString>(W));
	}
	Out->SetArrayField(TEXT("warnings"), WarningValues);
	return MakeShared<FJsonValueObject>(Out);
}

/** At each world [x, y], the base mesh vertex nearest it across the sent sections: its world position and channel weights. */
/** A built preview section: its mesh in world space and which channels hold weight anywhere in it. */
struct FBuiltSection
{
	TSharedPtr<const UE::MeshPartition::FMeshData> Mesh;
	FTransform ToWorld;
	TArray<uint8> ChannelTable;
	FBox Bounds;
};

/**
 * Samples the preview sections Mesh Partition built from the inputs and every modifier: positions per point, and per
 * section the channel indices (as in MF_LilaMeshTerrainChannel) that hold weight, so what a spline modifier painted
 * shows. The built meshes keep no weights of their own; they go to the channel texture. False with OutError while none
 * are built yet.
 */
bool SampleBuilt(AMeshPartition* Terrain, const TArray<FVector2D>& Points, TSharedRef<FJsonObject> Out, FString& OutError)
{
	TRACE_CPUPROFILER_EVENT_SCOPE(LilaLinkMeshTerrain::SampleBuilt);

	TArray<FBuiltSection> Built;
	for (TActorIterator<APreviewSection> It(Terrain->GetWorld()); It; ++It)
	{
		if (It->GetParent() == Terrain)
		{
			if (TSharedPtr<const UE::MeshPartition::FMeshData> Mesh = It->GetPreviewMesh())
			{
				Built.Add({Mesh, It->GetActorTransform(), It->GetChannelTable(), It->GetPreviewMeshBounds()});
			}
		}
	}
	const UE::MeshPartition::UMeshPartitionEditorSubsystem* Subsystem = UE::MeshPartition::UMeshPartitionEditorSubsystem::Get();
	const uint32 Building = Subsystem ? Subsystem->GetTotalPreviewSectionBuildNumber() : 0;
	if (Built.IsEmpty() || Building > 0)
	{
		OutError = FString::Printf(TEXT("sampleMeshTerrain: the preview sections aren't built yet (%d built, %u building); try again shortly"), Built.Num(), Building);
		return false;
	}
	TArray<TSharedPtr<FJsonValue>> Sections, Samples;
	for (const FBuiltSection& Section : Built)
	{
		TArray<TSharedPtr<FJsonValue>> Active;
		for (int32 Channel = 0; Channel < Section.ChannelTable.Num(); ++Channel)
		{
			// The table holds each channel's texture slice; 255 (-1 stored in a byte) marks one left all zero.
			if (Section.ChannelTable[Channel] != 255)
			{
				Active.Add(MakeShared<FJsonValueNumber>(Channel));
			}
		}
		TSharedRef<FJsonObject> S = MakeShared<FJsonObject>();
		S->SetArrayField(TEXT("activeChannels"), Active);
		S->SetArrayField(TEXT("min"), {MakeShared<FJsonValueNumber>(Section.Bounds.Min.X), MakeShared<FJsonValueNumber>(Section.Bounds.Min.Y)});
		S->SetArrayField(TEXT("max"), {MakeShared<FJsonValueNumber>(Section.Bounds.Max.X), MakeShared<FJsonValueNumber>(Section.Bounds.Max.Y)});
		Sections.Add(MakeShared<FJsonValueObject>(S));
	}
	for (const FVector2D& At : Points)
	{
		double Best = TNumericLimits<double>::Max();
		FVector BestPosition = FVector::ZeroVector;
		for (const FBuiltSection& Section : Built)
		{
			for (const int32 Vid : Section.Mesh->VertexIndicesItr())
			{
				const FVector P = Section.ToWorld.TransformPosition(Section.Mesh->GetVertex(Vid));
				const double D = FVector2D::DistSquared(FVector2D(P), At);
				if (D < Best)
				{
					Best = D;
					BestPosition = P;
				}
			}
		}
		TSharedRef<FJsonObject> Sample = MakeShared<FJsonObject>();
		Sample->SetArrayField(TEXT("position"), {MakeShared<FJsonValueNumber>(BestPosition.X), MakeShared<FJsonValueNumber>(BestPosition.Y),
													MakeShared<FJsonValueNumber>(BestPosition.Z)});
		Samples.Add(MakeShared<FJsonValueObject>(Sample));
	}
	Out->SetArrayField(TEXT("sections"), Sections);
	Out->SetArrayField(TEXT("samples"), Samples);
	return true;
}

TSharedPtr<FJsonValue> SampleMeshTerrain(const TSharedPtr<FJsonObject>& Params, FString& OutError)
{
	TRACE_CPUPROFILER_EVENT_SCOPE(LilaLinkMeshTerrain::SampleMeshTerrain);

	FString Id;
	const TArray<TSharedPtr<FJsonValue>>* Points = nullptr;
	if (!Params->TryGetStringField(TEXT("id"), Id) || !Params->TryGetArrayField(TEXT("points"), Points))
	{
		OutError = TEXT("sampleMeshTerrain: give id and points [[x, y], ...]");
		return nullptr;
	}
	UWorld* World = EditorWorld();
	const TMap<FString, AActor*> Sections = World ? TaggedSections(World, TagFor(Id)) : TMap<FString, AActor*>();
	if (Sections.IsEmpty())
	{
		OutError = TEXT("sampleMeshTerrain: no Mesh Terrain from this terrain");
		return nullptr;
	}
	TArray<TSharedPtr<FJsonValue>> Out;
	bool bBuilt = false;
	if (Params->TryGetBoolField(TEXT("built"), bBuilt) && bBuilt)
	{
		AMeshPartition* Terrain = FindTagged(World, TagFor(Id));
		TArray<FVector2D> At;
		for (const TSharedPtr<FJsonValue>& Value : *Points)
		{
			const TArray<TSharedPtr<FJsonValue>>& XY = Value->AsArray();
			if (XY.Num() < 2)
			{
				OutError = TEXT("sampleMeshTerrain: each point is [x, y]");
				return nullptr;
			}
			At.Emplace(XY[0]->AsNumber(), XY[1]->AsNumber());
		}
		TSharedRef<FJsonObject> Result = MakeShared<FJsonObject>();
		if (!Terrain)
		{
			OutError = TEXT("sampleMeshTerrain: no Mesh Terrain from this terrain");
			return nullptr;
		}
		if (!SampleBuilt(Terrain, At, Result, OutError))
		{
			return nullptr;
		}
		return MakeShared<FJsonValueObject>(Result);
	}
	for (const TSharedPtr<FJsonValue>& Value : *Points)
	{
		const TArray<TSharedPtr<FJsonValue>>& XY = Value->AsArray();
		if (XY.Num() < 2)
		{
			OutError = TEXT("sampleMeshTerrain: each point is [x, y]");
			return nullptr;
		}
		const FVector2D At(XY[0]->AsNumber(), XY[1]->AsNumber());
		double Best = TNumericLimits<double>::Max();
		const UE::Geometry::FDynamicMesh3* BestMesh = nullptr;
		int32 BestVertex = INDEX_NONE;
		FVector BestPosition = FVector::ZeroVector;
		for (const TPair<FString, AActor*>& S : Sections)
		{
			UMeshProviderModifier* Provider = S.Value->FindComponentByClass<UMeshProviderModifier>();
			const UE::Geometry::FDynamicMesh3* Mesh = Provider ? Provider->GetMesh() : nullptr;
			if (!Mesh)
			{
				continue;
			}
			const FTransform ToWorld = S.Value->GetActorTransform();
			for (const int32 Vid : Mesh->VertexIndicesItr())
			{
				const FVector P = ToWorld.TransformPosition(Mesh->GetVertex(Vid));
				const double D = FVector2D::DistSquared(FVector2D(P), At);
				if (D < Best)
				{
					Best = D;
					BestMesh = Mesh;
					BestVertex = Vid;
					BestPosition = P;
				}
			}
		}
		TSharedRef<FJsonObject> Sample = MakeShared<FJsonObject>();
		Sample->SetArrayField(TEXT("position"), {MakeShared<FJsonValueNumber>(BestPosition.X), MakeShared<FJsonValueNumber>(BestPosition.Y),
													MakeShared<FJsonValueNumber>(BestPosition.Z)});
		TSharedRef<FJsonObject> Weights = MakeShared<FJsonObject>();
		if (BestMesh && BestMesh->HasAttributes())
		{
			for (int32 Layer = 0; Layer < BestMesh->Attributes()->NumWeightLayers(); ++Layer)
			{
				const UE::Geometry::FDynamicMeshWeightAttribute* W = BestMesh->Attributes()->GetWeightLayer(Layer);
				float V = 0.0f;
				W->GetValue(BestVertex, &V);
				Weights->SetNumberField(W->GetName().ToString(), V);
			}
		}
		Sample->SetObjectField(TEXT("weights"), Weights);
		Out.Add(MakeShared<FJsonValueObject>(Sample));
	}
	return MakeShared<FJsonValueArray>(Out);
}

TSharedPtr<FJsonValue> RemoveMeshTerrain(const TSharedPtr<FJsonObject>& Params, FString& OutError)
{
	TRACE_CPUPROFILER_EVENT_SCOPE(LilaLinkMeshTerrain::RemoveMeshTerrain);

	FString Id;
	if (!Params->TryGetStringField(TEXT("id"), Id) || Id.IsEmpty())
	{
		OutError = TEXT("removeMeshTerrain: give id");
		return nullptr;
	}
	UWorld* World = EditorWorld();
	if (!World)
	{
		OutError = TEXT("removeMeshTerrain: no editor world");
		return nullptr;
	}
	FScopedTransaction Transaction(NSLOCTEXT("LilaLink", "RemoveMeshTerrain", "Lila: Remove Mesh Terrain"));
	const FName Tag = TagFor(Id);
	TArray<AActor*> Doomed;
	for (TActorIterator<AActor> It(World); It; ++It)
	{
		if (It->ActorHasTag(Tag) || It->ActorHasTag(RoadsTagFor(Id)))
		{
			Doomed.Add(*It);
		}
	}
	for (AActor* A : Doomed)
	{
		World->EditorDestroyActor(A, true);
	}
	TSharedRef<FJsonObject> Out = MakeShared<FJsonObject>();
	Out->SetBoolField(TEXT("removed"), Doomed.Num() > 0);
	return MakeShared<FJsonValueObject>(Out);
}
} // namespace

void LilaLinkMeshTerrain::RegisterMethods()
{
	LilaLink::RegisterMethod(TEXT("setMeshTerrain"), &SetMeshTerrain, Feature);
	LilaLink::RegisterMethod(TEXT("removeMeshTerrain"), &RemoveMeshTerrain, Feature);
	LilaLink::RegisterMethod(TEXT("sampleMeshTerrain"), &SampleMeshTerrain, Feature);
	LilaLink::RegisterMethod(TEXT("setMeshTerrainRoads"), &SetMeshTerrainRoads, TEXT("meshTerrainRoads"));
}
