#include "LilaLinkTextures.h"

#include "LilaLinkLog.h"
#include "LilaLinkMaterials.h"
#include "LilaLinkMethods.h"
#include "LilaLinkSettings.h"

#include "AssetRegistry/AssetRegistryModule.h"
#include "AssetRegistry/IAssetRegistry.h"
#include "AssetToolsModule.h"
#include "EditorFramework/AssetImportData.h"
#include "Engine/SkeletalMesh.h"
#include "Engine/StaticMesh.h"
#include "Engine/Texture2D.h"
#include "Factories/MaterialInstanceConstantFactoryNew.h"
#include "IAssetTools.h"
#include "ImageCore.h"
#include "ImageCoreUtils.h"
#include "ImageUtils.h"
#include "MaterialEditingLibrary.h"
#include "Materials/MaterialInstanceConstant.h"
#include "Materials/MaterialInterface.h"
#include "Misc/PackageName.h"
#include "Misc/Paths.h"
#include "ProfilingDebugging/CpuProfilerTrace.h"
#include "UObject/Package.h"

namespace
{
struct FTextureFile
{
	FString File;
	int32 Udim = 1001;
};

struct FTextureRequest
{
	FString Name;
	FString Map;
	TArray<FTextureFile> Files;
	bool bSRGB = false;
	FString Compression = TEXT("default");
	FString LodGroup = TEXT("World");
	bool bFlipGreen = false;
	bool bVirtual = false;
};

struct FInstanceRequest
{
	FString Name;
	FName Slot;
	TMap<FName, FString> Parameters;
	TMap<FName, float> Scalars;
	/** Lila's material values for a Painterly or Toon parent's parameters. */
	TSharedPtr<FJsonObject> Stylised;
	TArray<FString> Meshes;
};

template <class T> T* FindAssetAt(const FString& PackagePath, const FString& Name)
{
	const FString ObjectPath = PackagePath / Name + TEXT(".") + Name;
	FAssetData Data = IAssetRegistry::GetChecked().GetAssetByObjectPath(FSoftObjectPath(ObjectPath));
	return Data.IsValid() ? Cast<T>(Data.GetAsset()) : nullptr;
}

TSharedPtr<FJsonValue> Strings(const TArray<FString>& In)
{
	TArray<TSharedPtr<FJsonValue>> Values;
	for (const FString& S : In)
	{
		Values.Add(MakeShared<FJsonValueString>(S));
	}
	return MakeShared<FJsonValueArray>(Values);
}

TextureCompressionSettings ParseCompression(const FString& Name)
{
	if (Name == TEXT("normalmap"))
	{
		return TC_Normalmap;
	}
	if (Name == TEXT("masks"))
	{
		return TC_Masks;
	}
	if (Name == TEXT("grayscale"))
	{
		return TC_Grayscale;
	}
	if (Name == TEXT("hdr"))
	{
		return TC_HDR;
	}
	if (Name == TEXT("uncompressed"))
	{
		return TC_VectorDisplacementmap;
	}
	if (Name == TEXT("bc7"))
	{
		return TC_BC7;
	}
	return TC_Default;
}

TextureGroup ParseLodGroup(const FString& Name)
{
	const int64 Value = StaticEnum<TextureGroup>()->GetValueByNameString(TEXT("TEXTUREGROUP_") + Name);
	return Value == INDEX_NONE ? TEXTUREGROUP_World : TextureGroup(Value);
}

bool ParseTextures(const TSharedPtr<FJsonObject>& Params, TArray<FTextureRequest>& Out, FString& OutError)
{
	const TArray<TSharedPtr<FJsonValue>>* Textures = nullptr;
	if (!Params->TryGetArrayField(TEXT("textures"), Textures) || Textures->IsEmpty())
	{
		OutError = TEXT("importTextures: give textures");
		return false;
	}
	for (const TSharedPtr<FJsonValue>& V : *Textures)
	{
		const TSharedPtr<FJsonObject>* T = nullptr;
		if (!V.IsValid() || !V->TryGetObject(T))
		{
			continue;
		}
		FTextureRequest R;
		(*T)->TryGetStringField(TEXT("name"), R.Name);
		(*T)->TryGetStringField(TEXT("map"), R.Map);
		(*T)->TryGetBoolField(TEXT("srgb"), R.bSRGB);
		(*T)->TryGetStringField(TEXT("compression"), R.Compression);
		(*T)->TryGetStringField(TEXT("lodGroup"), R.LodGroup);
		(*T)->TryGetBoolField(TEXT("flipGreen"), R.bFlipGreen);
		(*T)->TryGetBoolField(TEXT("virtualTexture"), R.bVirtual);
		const TArray<TSharedPtr<FJsonValue>>* Files = nullptr;
		if ((*T)->TryGetArrayField(TEXT("files"), Files))
		{
			for (const TSharedPtr<FJsonValue>& F : *Files)
			{
				const TSharedPtr<FJsonObject>* FO = nullptr;
				if (F.IsValid() && F->TryGetObject(FO))
				{
					FTextureFile File;
					(*FO)->TryGetStringField(TEXT("file"), File.File);
					(*FO)->TryGetNumberField(TEXT("udim"), File.Udim);
					if (!File.File.IsEmpty())
					{
						R.Files.Add(File);
					}
				}
			}
		}
		if (R.Name.IsEmpty() || R.Files.IsEmpty())
		{
			OutError = TEXT("importTextures: every texture needs a name and files");
			return false;
		}
		Out.Add(MoveTemp(R));
	}
	return true;
}

void ParseInstances(const TSharedPtr<FJsonObject>& Params, TArray<FInstanceRequest>& Out)
{
	const TArray<TSharedPtr<FJsonValue>>* Materials = nullptr;
	if (!Params->TryGetArrayField(TEXT("materials"), Materials))
	{
		return;
	}
	for (const TSharedPtr<FJsonValue>& V : *Materials)
	{
		const TSharedPtr<FJsonObject>* M = nullptr;
		if (!V.IsValid() || !V->TryGetObject(M))
		{
			continue;
		}
		FInstanceRequest R;
		(*M)->TryGetStringField(TEXT("name"), R.Name);
		FString Slot;
		(*M)->TryGetStringField(TEXT("slot"), Slot);
		R.Slot = FName(*Slot);
		const TSharedPtr<FJsonObject>* P = nullptr;
		if ((*M)->TryGetObjectField(TEXT("parameters"), P))
		{
			for (const auto& [Key, Value] : (*P)->Values)
			{
				FString Texture;
				if (Value.IsValid() && Value->TryGetString(Texture))
				{
					R.Parameters.Add(FName(*Key), Texture);
				}
			}
		}
		const TSharedPtr<FJsonObject>* Scalars = nullptr;
		if ((*M)->TryGetObjectField(TEXT("scalars"), Scalars))
		{
			for (const auto& [Key, Value] : (*Scalars)->Values)
			{
				double Number = 0.0;
				if (Value.IsValid() && Value->TryGetNumber(Number))
				{
					R.Scalars.Add(FName(*Key), float(Number));
				}
			}
		}
		const TSharedPtr<FJsonObject>* Stylised = nullptr;
		if ((*M)->TryGetObjectField(TEXT("stylised"), Stylised))
		{
			R.Stylised = *Stylised;
		}
		const TArray<TSharedPtr<FJsonValue>>* Meshes = nullptr;
		if ((*M)->TryGetArrayField(TEXT("meshes"), Meshes))
		{
			for (const TSharedPtr<FJsonValue>& Mesh : *Meshes)
			{
				FString Path;
				if (Mesh.IsValid() && Mesh->TryGetString(Path) && !Path.IsEmpty())
				{
					R.Meshes.Add(Path);
				}
			}
		}
		if (!R.Name.IsEmpty())
		{
			Out.Add(MoveTemp(R));
		}
	}
}

/** The texture's source from its files: one image, or one block per UDIM tile converted to the first tile's format. */
bool SetSource(UTexture2D* Texture, const FTextureRequest& R, TArray<FString>& OutErrors)
{
	TRACE_CPUPROFILER_EVENT_SCOPE(LilaLinkTextures::SetSource);

	TArray<FImage> Images;
	Images.SetNum(R.Files.Num());
	for (int32 I = 0; I < R.Files.Num(); ++I)
	{
		if (!FImageUtils::LoadImage(*R.Files[I].File, Images[I]))
		{
			OutErrors.Add(FString::Printf(TEXT("Could not read %s"), *R.Files[I].File));
			return false;
		}
	}
	if (Images.Num() == 1 && R.Files[0].Udim == 1001)
	{
		Texture->Source.Init(Images[0]);
		return true;
	}

	const FImage& First = Images[0];
	TArray<FTextureSourceBlock> Blocks;
	TArray<const uint8*> Data;
	for (int32 I = 0; I < Images.Num(); ++I)
	{
		FImage& Image = Images[I];
		if (Image.SizeX != First.SizeX || Image.SizeY != First.SizeY)
		{
			OutErrors.Add(FString::Printf(TEXT("%s: UDIM tiles must share one size (%s is %dx%d, not %dx%d)"), *R.Name, *R.Files[I].File, Image.SizeX,
				Image.SizeY, First.SizeX, First.SizeY));
			return false;
		}
		if (Image.Format != First.Format)
		{
			Image.ChangeFormat(First.Format, First.GammaSpace);
		}
		FTextureSourceBlock Block;
		Block.BlockX = (R.Files[I].Udim - 1001) % 10;
		Block.BlockY = (R.Files[I].Udim - 1001) / 10;
		Block.SizeX = Image.SizeX;
		Block.SizeY = Image.SizeY;
		Block.NumSlices = 1;
		Block.NumMips = 1;
		Blocks.Add(Block);
		Data.Add(Image.RawData.GetData());
	}
	const ETextureSourceFormat Format = FImageCoreUtils::ConvertToTextureSourceFormat(First.Format);
	Texture->Source.InitBlocked(&Format, Blocks.GetData(), 1, Blocks.Num(), Data.GetData());
	return true;
}

/** Created, or updated in place so every material using it keeps its reference. */
UTexture2D* ImportTexture(const FTextureRequest& R, const FString& Folder, TArray<FString>& OutAssets, TArray<FString>& OutWarnings,
	TArray<FString>& OutErrors)
{
	TRACE_CPUPROFILER_EVENT_SCOPE(LilaLinkTextures::ImportTexture);

	UTexture2D* Texture = FindAssetAt<UTexture2D>(Folder, R.Name);
	const bool bCreated = Texture == nullptr;
	if (bCreated)
	{
		UPackage* Package = CreatePackage(*(Folder / R.Name));
		Texture = NewObject<UTexture2D>(Package, *R.Name, RF_Public | RF_Standalone | RF_Transactional);
	}
	Texture->Modify();
	Texture->PreEditChange(nullptr);
	if (!SetSource(Texture, R, OutErrors))
	{
		Texture->PostEditChange();
		return bCreated ? nullptr : Texture;
	}
	Texture->SRGB = R.bSRGB;
	Texture->CompressionSettings = ParseCompression(R.Compression);
	LilaLink::KeepGrayscale16(Texture);
	Texture->bFlipGreenChannel = R.bFlipGreen;
	Texture->LODGroup = ParseLodGroup(R.LodGroup);
	Texture->VirtualTextureStreaming = R.bVirtual || R.Files.Num() > 1;
	if (!Texture->AssetImportData)
	{
		Texture->AssetImportData = NewObject<UAssetImportData>(Texture, TEXT("AssetImportData"));
	}
	Texture->AssetImportData->Update(R.Files[0].File);
	Texture->PostEditChange();
	LilaLink::WarnIfCapped(Texture, OutWarnings);
	Texture->MarkPackageDirty();
	if (bCreated)
	{
		FAssetRegistryModule::AssetCreated(Texture);
	}
	OutAssets.AddUnique(Texture->GetPathName());
	return Texture;
}

UMaterialInstanceConstant* MakeInstance(const FString& Folder, const FString& Name, UMaterialInterface* Parent, TArray<FString>& OutErrors)
{
	TRACE_CPUPROFILER_EVENT_SCOPE(LilaLinkTextures::MakeInstance);

	UMaterialInstanceConstant* Instance = FindAssetAt<UMaterialInstanceConstant>(Folder, Name);
	if (!Instance)
	{
		UMaterialInstanceConstantFactoryNew* Factory = NewObject<UMaterialInstanceConstantFactoryNew>(GetTransientPackage());
		Factory->InitialParent = Parent;
		Instance = Cast<UMaterialInstanceConstant>(FAssetToolsModule::GetModule().Get().CreateAsset(Name, Folder, UMaterialInstanceConstant::StaticClass(), Factory));
	}
	if (!Instance)
	{
		OutErrors.Add(FString::Printf(TEXT("Could not create material instance %s in %s"), *Name, *Folder));
		return nullptr;
	}
	if (Instance->Parent != Parent)
	{
		UMaterialEditingLibrary::SetMaterialInstanceParent(Instance, Parent);
	}
	return Instance;
}

/** Sets the instance on the slot named Slot of a static or skeletal mesh. False when the mesh has no such slot. */
bool AssignToMesh(const FString& MeshPath, FName Slot, UMaterialInterface* Material, TArray<FString>& OutAssets, TArray<FString>& OutWarnings)
{
	UObject* Asset = FSoftObjectPath(MeshPath.Contains(TEXT(".")) ? MeshPath : MeshPath + TEXT(".") + FPackageName::GetShortName(MeshPath)).TryLoad();
	if (UStaticMesh* Mesh = Cast<UStaticMesh>(Asset))
	{
		const TArray<FStaticMaterial>& Materials = Mesh->GetStaticMaterials();
		for (int32 I = 0; I < Materials.Num(); ++I)
		{
			if (Materials[I].MaterialSlotName == Slot)
			{
				if (Materials[I].MaterialInterface != Material)
				{
					Mesh->Modify();
					Mesh->SetMaterial(I, Material);
					Mesh->MarkPackageDirty();
					OutAssets.AddUnique(Mesh->GetPathName());
				}
				return true;
			}
		}
	}
	else if (USkeletalMesh* Skel = Cast<USkeletalMesh>(Asset))
	{
		TArray<FSkeletalMaterial>& Materials = Skel->GetMaterials();
		for (FSkeletalMaterial& M : Materials)
		{
			if (M.MaterialSlotName == Slot)
			{
				if (M.MaterialInterface != Material)
				{
					Skel->Modify();
					M.MaterialInterface = Material;
					Skel->PostEditChange();
					Skel->MarkPackageDirty();
					OutAssets.AddUnique(Skel->GetPathName());
				}
				return true;
			}
		}
	}
	else
	{
		OutWarnings.Add(FString::Printf(TEXT("%s is not a static or skeletal mesh in this project; its slot %s was left as is"), *MeshPath, *Slot.ToString()));
		return false;
	}
	OutWarnings.Add(FString::Printf(TEXT("%s has no material slot %s"), *MeshPath, *Slot.ToString()));
	return false;
}

void ImportTextures(const TSharedPtr<FJsonObject>& Params, const FLilaLinkCallRef& Call)
{
	TRACE_CPUPROFILER_EVENT_SCOPE(LilaLinkTextures::ImportTextures);

	TArray<FTextureRequest> Textures;
	FString Error;
	if (!ParseTextures(Params, Textures, Error))
	{
		Call->Reply(nullptr, Error);
		return;
	}
	FString Folder = TEXT("/Game/Textures");
	Params->TryGetStringField(TEXT("folder"), Folder);
	FString Mode = TEXT("none");
	Params->TryGetStringField(TEXT("materialMode"), Mode);
	FString MaterialFolder = Folder;
	Params->TryGetStringField(TEXT("materialFolder"), MaterialFolder);
	bool bSave = false;
	bool bRebuildMaterial = false;
	const TSharedPtr<FJsonObject>* Options = nullptr;
	if (Params->TryGetObjectField(TEXT("options"), Options))
	{
		(*Options)->TryGetBoolField(TEXT("save"), bSave);
		(*Options)->TryGetBoolField(TEXT("rebuildMaterial"), bRebuildMaterial);
	}

	TArray<FString> Assets, Warnings, Errors;
	TMap<FString, UTexture2D*> ByName;
	for (int32 I = 0; I < Textures.Num(); ++I)
	{
		Call->Progress(FString::Printf(TEXT("importing %s (%d of %d)"), *Textures[I].Name, I + 1, Textures.Num()));
		if (UTexture2D* Texture = ImportTexture(Textures[I], Folder, Assets, Warnings, Errors))
		{
			ByName.Add(Textures[I].Name, Texture);
		}
	}

	if (Mode == TEXT("instance"))
	{
		FString ParentPath;
		Params->TryGetStringField(TEXT("materialParent"), ParentPath);
		// "lila:painterly" and "lila:toon" are LilaLink's own stylised masters, built on first use or with rebuildMaterial.
		UMaterialInterface* Parent = ParentPath.IsEmpty() ? nullptr
			: ParentPath.StartsWith(TEXT("lila:")) ? LilaLink::StylisedParent(ParentPath.RightChop(5), bRebuildMaterial, Assets, Warnings)
			: Cast<UMaterialInterface>(FSoftObjectPath(ParentPath.Contains(TEXT(".")) ? ParentPath : ParentPath + TEXT(".") + FPackageName::GetShortName(ParentPath)).TryLoad());
		TArray<FInstanceRequest> Instances;
		ParseInstances(Params, Instances);
		if (!Parent)
		{
			Errors.Add(FString::Printf(TEXT("No parent material at '%s'; no instances were made"), *ParentPath));
		}
		for (const FInstanceRequest& R : Instances)
		{
			if (!Parent)
			{
				break;
			}
			Call->Progress(FString::Printf(TEXT("updating %s"), *R.Name));
			UMaterialInstanceConstant* Instance = MakeInstance(MaterialFolder, R.Name, Parent, Errors);
			if (!Instance)
			{
				continue;
			}
			if (R.Stylised.IsValid())
			{
				LilaLink::ApplyStylised(Instance, R.Stylised, Assets, Warnings);
			}
			for (const auto& [Param, Value] : R.Scalars)
			{
				UMaterialEditingLibrary::SetMaterialInstanceScalarParameterValue(Instance, Param, Value);
			}
			TArray<FMaterialParameterInfo> Infos;
			TArray<FGuid> Ids;
			Instance->GetAllParameterInfoOfType(EMaterialParameterType::Texture, Infos, Ids);
			TSet<FName> Known;
			for (const FMaterialParameterInfo& Info : Infos)
			{
				Known.Add(Info.Name);
			}
			for (const auto& [Param, TextureName] : R.Parameters)
			{
				UTexture2D* const* Texture = ByName.Find(TextureName);
				if (!Texture)
				{
					continue;
				}
				if (!Known.Contains(Param))
				{
					Warnings.Add(FString::Printf(TEXT("%s has no texture parameter %s; %s was not set"), *Parent->GetPathName(), *Param.ToString(), *TextureName));
					continue;
				}
				UMaterialEditingLibrary::SetMaterialInstanceTextureParameterValue(Instance, Param, *Texture);
			}
			UMaterialEditingLibrary::UpdateMaterialInstance(Instance);
			Instance->MarkPackageDirty();
			Assets.AddUnique(Instance->GetPathName());
			for (const FString& Mesh : R.Meshes)
			{
				AssignToMesh(Mesh, R.Slot, Instance, Assets, Warnings);
			}
		}
	}

	for (const FString& E : Errors)
	{
		UE_LOG(LogLilaLink, Error, TEXT("importTextures: %s"), *E);
	}
	UE_LOG(LogLilaLink, Log, TEXT("importTextures %s: %d texture(s), %d asset(s) changed"), *Folder, ByName.Num(), Assets.Num());
	if (!Assets.IsEmpty() && (bSave || GetDefault<ULilaLinkSettings>()->bAutoSaveAfterImport))
	{
		Call->Progress(TEXT("saving"));
		LilaLink::SaveAssetPaths(Assets, Errors);
	}

	TSharedRef<FJsonObject> Out = MakeShared<FJsonObject>();
	Out->SetField(TEXT("assets"), Strings(Assets));
	Out->SetField(TEXT("warnings"), Strings(Warnings));
	Out->SetField(TEXT("errors"), Strings(Errors));
	Out->SetStringField(TEXT("method"), TEXT("textures"));
	Call->Reply(MakeShared<FJsonValueObject>(Out));
}
} // namespace

void LilaLink::AddTextureMethods(TMap<FString, FLilaLinkMethod>& Methods)
{
	Methods.Add(TEXT("importTextures"), &ImportTextures);
}
