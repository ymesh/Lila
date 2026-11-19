#pragma once

#include "CoreMinimal.h"
#include "Dom/JsonObject.h"
#include "UObject/SoftObjectPath.h"

class UMaterialInstanceConstant;
class UMaterialInterface;
class UTexture;

namespace LilaLink
{
/** Slot name Lila gives faces with no material; it stays on the engine default material. */
inline const FName DefaultSlotName(TEXT("Default"));

/**
 * How the slots of an import get their materials. Managed requests (params.materials present, as Lila's sync sends)
 * match each slot to an Unreal material by name; others leave materials to Interchange.
 */
struct FMaterialRequest
{
	bool bManaged = false;
	/** Create a material for a slot no Unreal material matches. */
	bool bCreate = true;
	bool bImportTextures = true;
	/** On reimport, keep a material assigned in Unreal to a slot that still exists. */
	bool bKeep = true;
	/** Rebuild the generated stylised masters and their functions. */
	bool bRebuildMaterial = false;
	FString AssetFolder;
	/** Searched first, and where created materials and textures go. */
	FString MaterialFolder;
	/** Lila's inputs per material name: baseColor, roughness, baseColorMap (absolute), ... */
	TMap<FName, TSharedPtr<FJsonObject>> Inputs;
	/** Slot name to material on the asset before a reimport. */
	TMap<FName, FSoftObjectPath> Previous;
};

/** What happened to one slot, for the reply. How is kept, found, created, default, ambiguous or none. */
struct FSlotResult
{
	FString Asset;
	FName Slot;
	FString Material;
	FString How;
};

bool ParseMaterialRequest(const TSharedPtr<FJsonObject>& Params, const TSharedPtr<FJsonObject>& Options, const FString& AssetFolder,
	FMaterialRequest& Out, FString& OutError);

/** Records the slot materials of a static or skeletal mesh about to be reimported. */
void SnapshotMaterials(UObject* Asset, FMaterialRequest& Request);

/** Assigns a material to every slot of the imported meshes. Created materials and textures are added to OutCreated. */
void ApplyMaterials(const FMaterialRequest& Request, const TArray<FString>& ImportedAssets, TArray<FString>& OutCreated,
	TArray<FString>& OutWarnings, TArray<FSlotResult>& OutSlots);

/**
 * The material for each slot name, chosen as ApplyMaterials chooses them for a mesh's slots, for assets whose slots are
 * not static or skeletal mesh slots. Asset names the asset in OutSlots.
 */
TArray<UMaterialInterface*> ResolveSlotMaterials(const FMaterialRequest& Request, const FString& Asset, const TArray<FName>& Slots,
	TArray<FString>& OutCreated, TArray<FString>& OutWarnings, TArray<FSlotResult>& OutSlots);

/**
 * The Painterly ("painterly") or Toon ("toon") master material, built on first use or when bRebuild; null with a warning for
 * anything else. Each calls its material function in the masters folder, MF_LilaPainterly (outputs BaseColor and a
 * world-space Normal) or MF_LilaToon (output Color, for Emissive Color on an Unlit material), for users' own materials.
 */
UMaterialInterface* StylisedParent(const FString& Model, bool bRebuild, TArray<FString>& OutCreated, TArray<FString>& OutWarnings);

/**
 * Sets a stylised master's parameters on an instance from Lila's material values (In: shadingModel, baseColor, palette...,
 * strokeMap). Leaves the base colour map to the caller.
 */
void ApplyStylised(UMaterialInstanceConstant* Instance, const TSharedPtr<FJsonObject>& In, TArray<FString>& OutCreated,
	TArray<FString>& OutWarnings);

/**
 * A grayscale texture whose source is 16-bit RGBA keeps only its red channel, as G16: Unreal compresses Grayscale to G8
 * unless the source itself is G16, and a height map loses the precision normals rebuilt from it need.
 */
void KeepGrayscale16(UTexture* Texture);

/**
 * After PostEditChange: a warning when Unreal capped a texture larger than 16384 to 16384, which it does (with only a
 * log line) when the texture isn't virtual and the project has virtual texturing off.
 */
void WarnIfCapped(const UTexture* Texture, TArray<FString>& OutWarnings);

/** Where the generated master materials live. */
LILALINK_API const TCHAR* MastersFolderPath();

/**
 * Imports a texture file into Folder as colour (sRGB) or linear data, or as a normal map, or finds the one already
 * imported from it. Null with a warning when it can't.
 */
UTexture* ImportTextureFile(const FString& File, const FString& Folder, bool bColor, bool bNormal, bool bFlipGreen, TArray<FString>& OutCreated,
	TArray<FString>& OutWarnings);
} // namespace LilaLink
