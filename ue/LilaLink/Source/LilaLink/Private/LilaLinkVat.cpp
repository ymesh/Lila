#include "LilaLinkVat.h"

#include "LilaLinkLog.h"
#include "LilaLinkMaterialFunction.h"
#include "LilaLinkMethods.h"
#include "LilaLinkSettings.h"

#include "AssetImportTask.h"
#include "AssetRegistry/AssetRegistryModule.h"
#include "AssetRegistry/IAssetRegistry.h"
#include "AssetToolsModule.h"
#include "EditorFramework/AssetImportData.h"
#include "Engine/StaticMesh.h"
#include "Engine/Texture2D.h"
#include "Factories/FbxFactory.h"
#include "Factories/FbxImportUI.h"
#include "Factories/FbxStaticMeshImportData.h"
#include "Factories/MaterialFactoryNew.h"
#include "Factories/MaterialFunctionFactoryNew.h"
#include "Factories/MaterialInstanceConstantFactoryNew.h"
#include "IAssetTools.h"
#include "ImageCore.h"
#include "ImageUtils.h"
#include "MaterialEditingLibrary.h"
#include "Materials/Material.h"
#include "Materials/MaterialExpressionAdd.h"
#include "Materials/MaterialExpressionAppendVector.h"
#include "Materials/MaterialExpressionComponentMask.h"
#include "Materials/MaterialExpressionConstant.h"
#include "Materials/MaterialExpressionCrossProduct.h"
#include "Materials/MaterialExpressionFloor.h"
#include "Materials/MaterialExpressionFmod.h"
#include "Materials/MaterialExpressionFrac.h"
#include "Materials/MaterialExpressionFunctionOutput.h"
#include "Materials/MaterialExpressionLinearInterpolate.h"
#include "Materials/MaterialExpressionLocalPosition.h"
#include "Materials/MaterialExpressionMaterialFunctionCall.h"
#include "Materials/MaterialExpressionMax.h"
#include "Materials/MaterialExpressionMin.h"
#include "Materials/MaterialExpressionMultiply.h"
#include "Materials/MaterialExpressionNormalize.h"
#include "Materials/MaterialExpressionScalarParameter.h"
#include "Materials/MaterialExpressionStaticSwitchParameter.h"
#include "Materials/MaterialExpressionSubtract.h"
#include "Materials/MaterialExpressionDivide.h"
#include "Materials/MaterialExpressionTextureCoordinate.h"
#include "Materials/MaterialExpressionTextureSample.h"
#include "Materials/MaterialExpressionTextureSampleParameter2D.h"
#include "Materials/MaterialExpressionTime.h"
#include "Materials/MaterialExpressionTransform.h"
#include "Materials/MaterialExpressionUtils.h"
#include "Materials/MaterialExpressionVectorParameter.h"
#include "Materials/MaterialExpressionVertexInterpolator.h"
#include "Materials/MaterialExpressionVertexNormalWS.h"
#include "Materials/MaterialExpressionVertexTangentWS.h"
#include "Materials/MaterialFunction.h"
#include "Materials/MaterialInstanceConstant.h"
#include "Materials/MaterialParameters.h"
#include "Misc/FileHelper.h"
#include "Misc/PackageName.h"
#include "Misc/Paths.h"
#include "ProfilingDebugging/CpuProfilerTrace.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"
#include "StaticMeshResources.h"
#include "UObject/Package.h"
#include "UObject/StrongObjectPtr.h"

namespace
{
const TCHAR* const DefaultMaterialFolder = TEXT("/Game/LilaLink/VAT");

struct FVatRequest
{
	FString PackagePath; // /Game/FX
	FString AssetName;   // SM_Blob
	FString Fbx;
	FString Positions;
	FString Normals;
	/** Rigid bakes only. */
	FString Rotations;
	FString MaterialParent;
	FString MaterialFolder = DefaultMaterialFolder;
	FString LodGroup;
	bool bRebuildMaterial = false;
	bool bSave = false;
	TSharedPtr<FJsonObject> Meta;
};

struct FVatMeta
{
	int32 FrameCount = 1;
	int32 RowsPerFrame = 1;
	int32 Width = 1;
	int32 Height = 1;
	int32 UvChannel = 1;
	int32 NormalBits = 8;
	double Fps = 30.0;
	bool bAbsolute = false;
	/** HDR material input: 1 for EXR values, 0 for PNG texels normalised by the bounds. */
	bool bHdr = true;
	int32 PositionBits = 16;
	/** Rigid: a transform per piece (position and rotation textures, pivot in two more UV channels) instead of a value per vertex. */
	bool bRigid = false;
	int32 PieceCount = 0;
	int32 PivotChannelXY = 2;
	int32 PivotChannelZ = 3;
	FVector BoundsMin = FVector::ZeroVector;
	FVector BoundsMax = FVector::OneVector;
	FBox MeshBounds = FBox(ForceInit);
};

double NumberOr(const TSharedPtr<FJsonObject>& O, const TCHAR* Field, double Fallback)
{
	double V = Fallback;
	return O.IsValid() && O->TryGetNumberField(Field, V) ? V : Fallback;
}

FVector VectorOr(const TSharedPtr<FJsonObject>& O, const TCHAR* Field, const FVector& Fallback)
{
	const TArray<TSharedPtr<FJsonValue>>* A = nullptr;
	if (!O.IsValid() || !O->TryGetArrayField(Field, A) || A->Num() != 3)
	{
		return Fallback;
	}
	return FVector((*A)[0]->AsNumber(), (*A)[1]->AsNumber(), (*A)[2]->AsNumber());
}

FVatMeta ParseMeta(const TSharedPtr<FJsonObject>& M)
{
	FVatMeta Out;
	Out.FrameCount = FMath::Max(1, int32(NumberOr(M, TEXT("frameCount"), 1)));
	Out.RowsPerFrame = FMath::Max(1, int32(NumberOr(M, TEXT("rowsPerFrame"), 1)));
	Out.Width = FMath::Max(1, int32(NumberOr(M, TEXT("width"), 1)));
	Out.Height = FMath::Max(1, int32(NumberOr(M, TEXT("height"), Out.FrameCount * Out.RowsPerFrame)));
	Out.UvChannel = FMath::Clamp(int32(NumberOr(M, TEXT("uvChannel"), 1)), 0, 7);
	Out.NormalBits = int32(NumberOr(M, TEXT("normalBits"), 8));
	Out.Fps = NumberOr(M, TEXT("fps"), 30.0);
	FString Positions, Format, Mode;
	M->TryGetStringField(TEXT("positions"), Positions);
	M->TryGetStringField(TEXT("format"), Format);
	M->TryGetStringField(TEXT("mode"), Mode);
	Out.bAbsolute = Positions == TEXT("absolute");
	Out.PositionBits = int32(NumberOr(M, TEXT("positionBits"), 16));
	Out.bRigid = Mode == TEXT("rigid");
	if (Out.bRigid)
	{
		Out.PieceCount = int32(NumberOr(M, TEXT("pieceCount"), 0));
		const TArray<TSharedPtr<FJsonValue>>* Pivot = nullptr;
		if (M->TryGetArrayField(TEXT("pivotChannels"), Pivot) && Pivot->Num() == 2)
		{
			Out.PivotChannelXY = FMath::Clamp(int32((*Pivot)[0]->AsNumber()), 0, 7);
			Out.PivotChannelZ = FMath::Clamp(int32((*Pivot)[1]->AsNumber()), 0, 7);
		}
		else
		{
			Out.PivotChannelXY = FMath::Min(Out.UvChannel + 1, 7);
			Out.PivotChannelZ = FMath::Min(Out.UvChannel + 2, 7);
		}
	}
	const TSharedPtr<FJsonObject>* ParamsPtr = nullptr;
	const TSharedPtr<FJsonObject> Params = M->TryGetObjectField(TEXT("materialParams"), ParamsPtr) ? *ParamsPtr : nullptr;
	Out.bHdr = NumberOr(Params, TEXT("HDR"), Format == TEXT("exr") ? 1.0 : 0.0) > 0.5;
	Out.BoundsMin = VectorOr(Params, TEXT("BoundsMin"), FVector::ZeroVector);
	Out.BoundsMax = VectorOr(Params, TEXT("BoundsMax"), FVector::OneVector);
	const TSharedPtr<FJsonObject>* Bounds = nullptr;
	if (M->TryGetObjectField(TEXT("meshBounds"), Bounds))
	{
		Out.MeshBounds = FBox(VectorOr(*Bounds, TEXT("min"), FVector::ZeroVector), VectorOr(*Bounds, TEXT("max"), FVector::ZeroVector));
	}
	return Out;
}

bool RequireFile(const TSharedPtr<FJsonObject>& Params, const TCHAR* Field, bool bRequired, FString& Out, FString& OutError)
{
	if (!Params->TryGetStringField(Field, Out) || Out.IsEmpty())
	{
		if (bRequired)
		{
			OutError = FString::Printf(TEXT("importVertexAnimation: give %s"), Field);
		}
		return !bRequired;
	}
	Out = FPaths::ConvertRelativePathToFull(Out);
	if (!FPaths::FileExists(Out))
	{
		OutError = FString::Printf(TEXT("importVertexAnimation: no file at %s"), *Out);
		return false;
	}
	return true;
}

bool ParseRequest(const TSharedPtr<FJsonObject>& Params, FVatRequest& Out, FString& OutError)
{
	TRACE_CPUPROFILER_EVENT_SCOPE(LilaLinkVat::ParseRequest);

	FString ContentPath;
	if (!Params->TryGetStringField(TEXT("contentPath"), ContentPath) || ContentPath.IsEmpty())
	{
		OutError = TEXT("importVertexAnimation: give contentPath, like /Game/FX/SM_Blob");
		return false;
	}
	ContentPath = FPackageName::ObjectPathToPackageName(ContentPath);
	FText Reason;
	if (!FPackageName::IsValidLongPackageName(ContentPath, false, &Reason))
	{
		OutError = FString::Printf(TEXT("importVertexAnimation: bad contentPath %s: %s"), *ContentPath, *Reason.ToString());
		return false;
	}
	Out.PackagePath = FPackageName::GetLongPackagePath(ContentPath);
	Out.AssetName = FPackageName::GetShortName(ContentPath);

	if (!RequireFile(Params, TEXT("fbx"), true, Out.Fbx, OutError) || !RequireFile(Params, TEXT("positions"), true, Out.Positions, OutError) ||
		!RequireFile(Params, TEXT("normals"), false, Out.Normals, OutError) || !RequireFile(Params, TEXT("rotations"), false, Out.Rotations, OutError))
	{
		return false;
	}

	const TSharedPtr<FJsonObject>* Meta = nullptr;
	if (Params->TryGetObjectField(TEXT("meta"), Meta))
	{
		Out.Meta = *Meta;
	}
	else
	{
		FString MetaFile, Text;
		if (!RequireFile(Params, TEXT("metaFile"), true, MetaFile, OutError))
		{
			return false;
		}
		FFileHelper::LoadFileToString(Text, *MetaFile);
		TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(Text);
		if (!FJsonSerializer::Deserialize(Reader, Out.Meta) || !Out.Meta.IsValid())
		{
			OutError = FString::Printf(TEXT("importVertexAnimation: %s is not JSON"), *MetaFile);
			return false;
		}
	}

	Params->TryGetStringField(TEXT("materialParent"), Out.MaterialParent);
	const TSharedPtr<FJsonObject>* OptionsPtr = nullptr;
	if (Params->TryGetObjectField(TEXT("options"), OptionsPtr))
	{
		const TSharedPtr<FJsonObject>& Options = *OptionsPtr;
		FString Folder;
		if (Options->TryGetStringField(TEXT("materialFolder"), Folder) && !Folder.IsEmpty())
		{
			Out.MaterialFolder = Folder.StartsWith(TEXT("/")) ? Folder : Out.PackagePath / Folder;
		}
		Options->TryGetStringField(TEXT("lodGroup"), Out.LodGroup);
		Options->TryGetBoolField(TEXT("rebuildMaterial"), Out.bRebuildMaterial);
		Options->TryGetBoolField(TEXT("save"), Out.bSave);
	}

	// A rigid bake sent without "rotations" names its rotation texture in the metadata, beside the position texture.
	FString Mode;
	Out.Meta->TryGetStringField(TEXT("mode"), Mode);
	if (Mode != TEXT("rigid"))
	{
		return true;
	}
	const TSharedPtr<FJsonObject>* Files = nullptr;
	FString Listed;
	if (Out.Rotations.IsEmpty() && Out.Meta->TryGetObjectField(TEXT("files"), Files) && (*Files)->TryGetStringField(TEXT("rotations"), Listed) && !Listed.IsEmpty())
	{
		Out.Rotations = FPaths::ConvertRelativePathToFull(FPaths::GetPath(Out.Positions), Listed);
	}
	if (Out.Rotations.IsEmpty() || !FPaths::FileExists(Out.Rotations))
	{
		OutError = FString::Printf(TEXT("importVertexAnimation: the rigid bake has no rotation texture%s%s"), Out.Rotations.IsEmpty() ? TEXT("") : TEXT(" at "), *Out.Rotations);
		return false;
	}
	return true;
}

template <class T> T* FindAssetAt(const FString& PackagePath, const FString& Name)
{
	const FString ObjectPath = PackagePath / Name + TEXT(".") + Name;
	FAssetData Data = IAssetRegistry::GetChecked().GetAssetByObjectPath(FSoftObjectPath(ObjectPath));
	return Data.IsValid() ? Cast<T>(Data.GetAsset()) : nullptr;
}

/** The legacy FBX factory, which reimports over an existing mesh in place and takes the degenerate-triangle setting. */
UStaticMesh* ImportMesh(const FVatRequest& R, TArray<FString>& OutErrors)
{
	TRACE_CPUPROFILER_EVENT_SCOPE(LilaLinkVat::ImportMesh);

	UFbxImportUI* Options = NewObject<UFbxImportUI>(GetTransientPackage());
	Options->bAutomatedImportShouldDetectType = false;
	Options->bImportMesh = true;
	Options->bImportAsSkeletal = false;
	Options->MeshTypeToImport = FBXIT_StaticMesh;
	Options->OriginalImportType = FBXIT_StaticMesh;
	Options->bImportAnimations = false;
	Options->bImportMaterials = false;
	Options->bImportTextures = false;
	UFbxStaticMeshImportData* Static = Options->StaticMeshImportData;
	Static->bCombineMeshes = true;
	Static->bAutoGenerateCollision = false;
	Static->bGenerateLightmapUVs = false;
	Static->bBuildNanite = false;
	// Soup padding is degenerate in the rest pose and opens up when the texture moves it.
	Static->bRemoveDegenerates = false;
	Static->NormalImportMethod = FBXNIM_ImportNormals;
	if (!R.LodGroup.IsEmpty())
	{
		Static->StaticMeshLODGroup = FName(*R.LodGroup);
	}

	TStrongObjectPtr<UAssetImportTask> Task(NewObject<UAssetImportTask>(GetTransientPackage()));
	Task->Filename = R.Fbx;
	Task->DestinationPath = R.PackagePath;
	Task->DestinationName = R.AssetName;
	Task->bReplaceExisting = true;
	Task->bReplaceExistingSettings = true;
	Task->bAutomated = true;
	Task->bSave = false;
	Task->Factory = NewObject<UFbxFactory>(GetTransientPackage());
	Task->Options = Options;
	FAssetToolsModule::GetModule().Get().ImportAssetTasks({Task.Get()});

	for (UObject* Object : Task->GetObjects())
	{
		if (UStaticMesh* Mesh = Cast<UStaticMesh>(Object))
		{
			return Mesh;
		}
	}
	if (UStaticMesh* Existing = FindAssetAt<UStaticMesh>(R.PackagePath, R.AssetName))
	{
		return Existing;
	}
	OutErrors.Add(TEXT("The rest mesh FBX imported no static mesh; see the editor log"));
	return nullptr;
}

/** Build settings the VAT needs, and bounds that hold every frame. Rebuilds the mesh. */
void ConfigureMesh(UStaticMesh* Mesh, const FVatMeta& Meta)
{
	TRACE_CPUPROFILER_EVENT_SCOPE(LilaLinkVat::ConfigureMesh);

	Mesh->Modify();
	for (int32 Lod = 0; Lod < Mesh->GetNumSourceModels(); ++Lod)
	{
		FMeshBuildSettings& Build = Mesh->GetSourceModel(Lod).BuildSettings;
		Build.bRecomputeNormals = false;
		Build.bRecomputeTangents = false;
		Build.bRemoveDegenerates = false;
		// Half precision UVs cannot address thousands of columns.
		Build.bUseFullPrecisionUVs = true;
		Build.bGenerateLightmapUVs = false;
	}
	FMeshNaniteSettings Nanite = Mesh->GetNaniteSettings();
	Nanite.bEnabled = false;
	Mesh->SetNaniteSettings(Nanite);

	if (Meta.MeshBounds.IsValid)
	{
		const FStaticMeshRenderData* Render = Mesh->GetRenderData();
		const FBox Rest = Render ? Render->Bounds.GetBox() : FBox(FVector::ZeroVector, FVector::ZeroVector);
		Mesh->SetPositiveBoundsExtension((Meta.MeshBounds.Max - Rest.Max).ComponentMax(FVector::ZeroVector));
		Mesh->SetNegativeBoundsExtension((Rest.Min - Meta.MeshBounds.Min).ComponentMax(FVector::ZeroVector));
	}
	Mesh->PostEditChange();
	Mesh->MarkPackageDirty();
}

void ApplyDataSettings(UTexture2D* Texture, TextureCompressionSettings Compression)
{
	Texture->SRGB = false;
	Texture->CompressionSettings = Compression;
	Texture->MipGenSettings = TMGS_NoMipmaps;
	Texture->Filter = TF_Nearest;
	Texture->NeverStream = true;
	Texture->LODGroup = TEXTUREGROUP_16BitData;
	Texture->AddressX = TA_Clamp;
	Texture->AddressY = TA_Clamp;
}

/** A data texture from the file, created or updated in place. */
UTexture2D* ImportDataTexture(const FString& File, const FString& PackagePath, const FString& Name, TextureCompressionSettings Compression,
	TArray<FString>& OutCreated, TArray<FString>& OutErrors)
{
	TRACE_CPUPROFILER_EVENT_SCOPE(LilaLinkVat::ImportDataTexture);

	FImage Image;
	if (!FImageUtils::LoadImage(*File, Image))
	{
		OutErrors.Add(FString::Printf(TEXT("Could not read %s"), *File));
		return nullptr;
	}

	UTexture2D* Texture = FindAssetAt<UTexture2D>(PackagePath, Name);
	const bool bCreated = Texture == nullptr;
	if (bCreated)
	{
		UPackage* Package = CreatePackage(*(PackagePath / Name));
		Texture = NewObject<UTexture2D>(Package, *Name, RF_Public | RF_Standalone | RF_Transactional);
	}
	Texture->Modify();
	Texture->PreEditChange(nullptr);
	Texture->Source.Init(Image);
	ApplyDataSettings(Texture, Compression);
	if (!Texture->AssetImportData)
	{
		Texture->AssetImportData = NewObject<UAssetImportData>(Texture, TEXT("AssetImportData"));
	}
	Texture->AssetImportData->Update(File);
	Texture->PostEditChange();
	Texture->MarkPackageDirty();
	if (bCreated)
	{
		FAssetRegistryModule::AssetCreated(Texture);
	}
	OutCreated.AddUnique(Texture->GetPathName());
	return Texture;
}

/** Expressions placed in a material or a material function, laid out in columns. */
struct FVatGraph
{
	UMaterial* Material = nullptr;
	UMaterialFunction* Function = nullptr;
	int32 Row = 0;

	UMaterialExpression* Make(TSubclassOf<UMaterialExpression> Class, int32 X = -600)
	{
		const int32 Y = (Row++) * 120;
		return Function ? UMaterialEditingLibrary::CreateMaterialExpressionInFunction(Function, Class, X, Y)
						: UMaterialEditingLibrary::CreateMaterialExpression(Material, Class, X, Y);
	}

	template <class T> T* New(int32 X = -600) { return Cast<T>(Make(T::StaticClass(), X)); }

	UMaterialExpression* Scalar(const TCHAR* Name, float Value, const TCHAR* Group)
	{
		auto* E = New<UMaterialExpressionScalarParameter>(-1400);
		E->ParameterName = Name;
		E->DefaultValue = Value;
		E->Group = Group;
		return E;
	}

	UMaterialExpression* Vector(const TCHAR* Name, const FLinearColor& Value, const TCHAR* Group)
	{
		auto* E = New<UMaterialExpressionVectorParameter>(-1400);
		E->ParameterName = Name;
		E->DefaultValue = Value;
		E->Group = Group;
		return E;
	}

	/** A two-input math node (A then B); without B its ConstB is used. */
	UMaterialExpression* Binary(TSubclassOf<UMaterialExpression> Class, UMaterialExpression* A, int32 AOut, UMaterialExpression* B, int32 BOut)
	{
		UMaterialExpression* E = Make(Class);
		E->GetInput(0)->Connect(AOut, A);
		if (B)
		{
			E->GetInput(1)->Connect(BOut, B);
		}
		return E;
	}

	UMaterialExpression* Lerp(UMaterialExpression* A, UMaterialExpression* B, UMaterialExpression* Alpha)
	{
		auto* E = New<UMaterialExpressionLinearInterpolate>();
		E->A.Connect(0, A);
		E->B.Connect(0, B);
		E->Alpha.Connect(0, Alpha);
		return E;
	}

	UMaterialExpression* Mask(UMaterialExpression* In, int32 Out, bool bR, bool bG, bool bB, bool bA)
	{
		auto* E = New<UMaterialExpressionComponentMask>();
		E->Input.Connect(Out, In);
		E->R = bR;
		E->G = bG;
		E->B = bB;
		E->A = bA;
		return E;
	}

	UMaterialExpression* Normalize(UMaterialExpression* In)
	{
		auto* E = New<UMaterialExpressionNormalize>();
		E->VectorInput.Connect(0, In);
		return E;
	}

	UMaterialExpression* Transform(UMaterialExpression* In, EMaterialVectorCoordTransformSource From, EMaterialVectorCoordTransform To)
	{
		auto* E = New<UMaterialExpressionTransform>();
		E->TransformSourceType = From;
		E->TransformType = To;
		E->Input.Connect(0, In);
		return E;
	}

	UMaterialExpression* Interpolate(UMaterialExpression* In)
	{
		auto* E = New<UMaterialExpressionVertexInterpolator>();
		E->Input.Connect(0, In);
		return E;
	}

	UMaterialExpression* Sample(const TCHAR* Name, UTexture* Texture, UMaterialExpression* Uv)
	{
		auto* E = New<UMaterialExpressionTextureSampleParameter2D>(-300);
		E->ParameterName = Name;
		E->Group = TEXT("Vertex Animation");
		E->Texture = Texture;
		E->SamplerType = MaterialExpressionUtils::GetSamplerTypeForTexture(Texture);
		E->SamplerSource = SSM_FromTextureAsset;
		// Sampled in the vertex shader, which has no derivatives.
		E->MipValueMode = TMVM_MipLevel;
		E->ConstMipValue = 0;
		E->Coordinates.Connect(0, Uv);
		return E;
	}

	/** V rotated by the unit quaternion Q (xyzw): V + 2 Q.w (Q.xyz x V) + 2 Q.xyz x (Q.xyz x V), as Lila's decoder. */
	UMaterialExpression* Rotate(UMaterialExpression* Q, UMaterialExpression* V)
	{
		UMaterialExpression* U = Mask(Q, 0, true, true, true, false);
		UMaterialExpression* W = Mask(Q, 0, false, false, false, true);
		UMaterialExpression* T = Binary(UMaterialExpressionMultiply::StaticClass(), Binary(UMaterialExpressionCrossProduct::StaticClass(), U, 0, V, 0), 0, nullptr, 0);
		Cast<UMaterialExpressionMultiply>(T)->ConstB = 2.0f;
		UMaterialExpression* Scaled = Binary(UMaterialExpressionMultiply::StaticClass(), T, 0, W, 0);
		UMaterialExpression* Twice = Binary(UMaterialExpressionCrossProduct::StaticClass(), U, 0, T, 0);
		return Binary(UMaterialExpressionAdd::StaticClass(), Binary(UMaterialExpressionAdd::StaticClass(), V, 0, Scaled, 0), 0, Twice, 0);
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
 * MF_LilaVAT_UV<n>: plays a vertex VAT bake. Frame = floor(AutoPlay ? Time * FrameRate * PlayRate (wrapped or clamped by
 * Loop) : Frame); UVs (UV channel n unless connected) move down Frame * RowsPerFrame / TextureHeight rows, the position
 * texel decodes as HDR ? texel : lerp(BoundsMin, BoundsMax, texel) into a local offset (Absolute subtracts the vertex's
 * own position), and the WorldPositionOffset output is that offset in world space. With normals, the Normal output is
 * the normal texel in world space, carried to the pixel shader by a vertex interpolator. Every input but UVs defaults to
 * the parameter of its name.
 */
void BuildVatFunction(UMaterialFunction* Function, int32 UvChannel, UTexture* Positions, UTexture* Normals)
{
	TRACE_CPUPROFILER_EVENT_SCOPE(LilaLinkVat::BuildVatFunction);

	LilaLink::FFunctionGraph In(Function, TEXT("Lila Vertex Animation Texture: plays a baked animation, a position per vertex per frame."));
	FVatGraph G;
	G.Function = Function;
	const TCHAR* Playback = TEXT("Playback");
	const TCHAR* Layout = TEXT("Vertex Animation");
	UMaterialExpression* FrameRate = In.ScalarInput(TEXT("FrameRate"), 30.0f, Playback, TEXT("Frames per second of the bake."));
	UMaterialExpression* PlayRate = In.ScalarInput(TEXT("PlayRate"), 1.0f, Playback, TEXT("Speed multiplier."));
	UMaterialExpression* AutoPlay = In.ScalarInput(TEXT("AutoPlay"), 1.0f, Playback, TEXT("1 plays with time, 0 holds Frame."));
	UMaterialExpression* Loop = In.ScalarInput(TEXT("Loop"), 1.0f, Playback, TEXT("1 wraps at the end, 0 holds the last frame."));
	UMaterialExpression* ManualFrame = In.ScalarInput(TEXT("Frame"), 0.0f, Playback, TEXT("The frame shown when AutoPlay is 0."));
	UMaterialExpression* FrameCount = In.ScalarInput(TEXT("FrameCount"), 1.0f, Layout, TEXT("Frames in the bake."));
	UMaterialExpression* RowsPerFrame = In.ScalarInput(TEXT("RowsPerFrame"), 1.0f, Layout, TEXT("Texture rows each frame takes."));
	UMaterialExpression* TextureHeight = In.ScalarInput(TEXT("TextureHeight"), 1.0f, Layout, TEXT("The data textures' height in texels."));
	UMaterialExpression* Absolute = In.ScalarInput(TEXT("Absolute"), 0.0f, Layout, TEXT("1 when the bake stores positions, 0 for offsets."));
	UMaterialExpression* Hdr = In.ScalarInput(TEXT("HDR"), 1.0f, Layout, TEXT("1 for EXR values, 0 for PNG texels normalised by the bounds."));
	UMaterialExpression* BoundsMin = In.VectorInput(TEXT("BoundsMin"), FLinearColor(0, 0, 0, 0), Layout, TEXT("PNG bakes: the value a texel of 0 stands for."));
	UMaterialExpression* BoundsMax = In.VectorInput(TEXT("BoundsMax"), FLinearColor(1, 1, 1, 0), Layout, TEXT("PNG bakes: the value a texel of 1 stands for."));
	auto TypeOf = [](UTexture* Texture) { return Texture ? MaterialExpressionUtils::GetSamplerTypeForTexture(Texture) : SAMPLERTYPE_LinearColor; };
	UMaterialExpression* PositionTexture = In.TextureInput(TEXT("PositionTexture"), Positions, TypeOf(Positions), Layout, TEXT("The bake's position texture."));
	UMaterialExpression* NormalTexture = Normals
		? In.TextureInput(TEXT("NormalTexture"), Normals, TypeOf(Normals), Layout, TEXT("The bake's normal texture."))
		: nullptr;
	auto* Channel = G.New<UMaterialExpressionTextureCoordinate>(-1500);
	Channel->CoordinateIndex = UvChannel;
	UMaterialExpression* TexCoord = In.Input(TEXT("UVs"), FunctionInput_Vector2, Channel, TEXT("The bake's addressing UVs; the UV channel it was baked to when unconnected."));

	UMaterialExpression* Time = G.Make(UMaterialExpressionTime::StaticClass());
	UMaterialExpression* Played = G.Binary(UMaterialExpressionMultiply::StaticClass(), G.Binary(UMaterialExpressionMultiply::StaticClass(), Time, 0, FrameRate, 0), 0, PlayRate, 0);
	UMaterialExpression* LastFrame = G.Binary(UMaterialExpressionSubtract::StaticClass(), FrameCount, 0, nullptr, 0);
	Cast<UMaterialExpressionSubtract>(LastFrame)->ConstB = 1.0f;
	UMaterialExpression* Wrapped = G.Binary(UMaterialExpressionFmod::StaticClass(), Played, 0, FrameCount, 0);
	UMaterialExpression* Held = G.Binary(UMaterialExpressionMin::StaticClass(), Played, 0, LastFrame, 0);
	auto* Floor = G.New<UMaterialExpressionFloor>();
	Floor->Input.Connect(0, G.Lerp(ManualFrame, G.Lerp(Held, Wrapped, Loop), AutoPlay));
	UMaterialExpression* NotBelow = G.Binary(UMaterialExpressionMax::StaticClass(), Floor, 0, nullptr, 0);
	Cast<UMaterialExpressionMax>(NotBelow)->ConstB = 0.0f;
	UMaterialExpression* Frame = G.Binary(UMaterialExpressionMin::StaticClass(), NotBelow, 0, LastFrame, 0);

	UMaterialExpression* Rows = G.Binary(UMaterialExpressionMultiply::StaticClass(), Frame, 0, RowsPerFrame, 0);
	UMaterialExpression* VOffset = G.Binary(UMaterialExpressionDivide::StaticClass(), Rows, 0, TextureHeight, 0);
	auto* Zero = G.New<UMaterialExpressionConstant>();
	Zero->R = 0.0f;
	UMaterialExpression* Shift = G.Binary(UMaterialExpressionAppendVector::StaticClass(), Zero, 0, VOffset, 0);
	UMaterialExpression* Uv = G.Binary(UMaterialExpressionAdd::StaticClass(), TexCoord, 0, Shift, 0);
	auto Sample = [&](UMaterialExpression* Texture, UTexture* Default) {
		auto* E = G.New<UMaterialExpressionTextureSample>(-300);
		E->TextureObject.Connect(0, Texture);
		E->SamplerType = TypeOf(Default);
		E->SamplerSource = SSM_FromTextureAsset;
		// Sampled in the vertex shader, which has no derivatives.
		E->MipValueMode = TMVM_MipLevel;
		E->ConstMipValue = 0;
		E->Coordinates.Connect(0, Uv);
		return E;
	};

	UMaterialExpression* PosTexel = Sample(PositionTexture, Positions);
	UMaterialExpression* Value = G.Lerp(G.Lerp(BoundsMin, BoundsMax, PosTexel), PosTexel, Hdr);
	auto* Local = G.New<UMaterialExpressionLocalPosition>();
	Local->IncludedOffsets = EPositionIncludedOffsets::ExcludeOffsets;
	UMaterialExpression* RestPart = G.Binary(UMaterialExpressionMultiply::StaticClass(), Local, 0, Absolute, 0);
	UMaterialExpression* Offset = G.Binary(UMaterialExpressionSubtract::StaticClass(), Value, 0, RestPart, 0);
	In.Output(G.Transform(Offset, TRANSFORMSOURCE_Local, TRANSFORM_World), TEXT("WorldPositionOffset"), TEXT("World Position Offset."));

	if (NormalTexture)
	{
		UMaterialExpression* Doubled = G.Binary(UMaterialExpressionMultiply::StaticClass(), Sample(NormalTexture, Normals), 0, nullptr, 0);
		Cast<UMaterialExpressionMultiply>(Doubled)->ConstB = 2.0f;
		UMaterialExpression* Unpacked = G.Binary(UMaterialExpressionSubtract::StaticClass(), Doubled, 0, nullptr, 0);
		Cast<UMaterialExpressionSubtract>(Unpacked)->ConstB = 1.0f;
		In.Output(G.Interpolate(G.Transform(Unpacked, TRANSFORMSOURCE_Local, TRANSFORM_World)), TEXT("Normal"),
			TEXT("The baked normal, world space: connect to Normal with Tangent Space Normal off."));
	}
	In.Finish();
}

/** M_LilaVAT_UV<n>[_NoNormals]: MF_LilaVAT_UV<n>[_NoNormals] with BaseColor, Roughness and Metallic parameters. */
void BuildVatMaterial(UMaterial* Material, UMaterialFunction* Function, bool bNormals, TArray<FString>& OutErrors)
{
	TRACE_CPUPROFILER_EVENT_SCOPE(LilaLinkVat::BuildVatMaterial);

	UMaterialExpressionMaterialFunctionCall* Call = LilaLink::ResetToFunctionCall(Material, Function);
	FVatGraph G;
	G.Material = Material;
	const bool bWired = bNormals ? LilaLink::ConnectFunctionOutputs(Call, {{TEXT("WorldPositionOffset"), MP_WorldPositionOffset}, {TEXT("Normal"), MP_Normal}})
								 : LilaLink::ConnectFunctionOutputs(Call, {{TEXT("WorldPositionOffset"), MP_WorldPositionOffset}});
	if (!bWired)
	{
		OutErrors.Add(FString::Printf(TEXT("Could not wire the outputs of %s into %s"), *Function->GetName(), *Material->GetName()));
	}
	Material->bTangentSpaceNormal = !bNormals;
	UMaterialEditingLibrary::ConnectMaterialProperty(G.Vector(TEXT("BaseColor"), FLinearColor(0.5f, 0.5f, 0.5f), TEXT("Surface")), TEXT(""), MP_BaseColor);
	UMaterialEditingLibrary::ConnectMaterialProperty(G.Scalar(TEXT("Roughness"), 0.55f, TEXT("Surface")), TEXT(""), MP_Roughness);
	UMaterialEditingLibrary::ConnectMaterialProperty(G.Scalar(TEXT("Metallic"), 0.0f, TEXT("Surface")), TEXT(""), MP_Metallic);

	Material->SetMaterialUsage(MATUSAGE_InstancedStaticMeshes);
	Material->SetMaterialUsage(MATUSAGE_NiagaraMeshParticles);
	Material->PostEditChange();
	Material->MarkPackageDirty();
	UMaterialEditingLibrary::RecompileMaterial(Material);
}

const TCHAR* const RigidWpoOutput = TEXT("WorldPositionOffset");
const TCHAR* const RigidNormalOutput = TEXT("Normal");

/**
 * The rigid VAT transform as a material function, so any material (an interior slot's own, say) can play the bake:
 * its parameters are the material's parameters. Frame follows the vertex VAT material: floor(AutoPlay ? Time * FrameRate
 * * PlayRate (wrapped or clamped by Loop) : Frame). The piece's texels sit at the addressing UV moved down
 * Frame * RowsPerFrame / TextureHeight; its pivot is (UV[PivotChannelXY].xy, UV[PivotChannelZ].x). Then
 * q = normalize(HDR ? rot : rot * 2 - 1), centre = (HDR ? pos : lerp(BoundsMin, BoundsMax, pos)) + (Absolute ? 0 : pivot),
 * and the local rest position P moves to rotate(q, P - pivot) + centre (to the pivot, rotate, back out, so rotation
 * never drifts). The Interpolate static switch (off by default) blends towards the next frame: lerp of the centre,
 * normalised lerp of q. Outputs: WorldPositionOffset; Normal and Tangent, the vertex basis rotated by q in world space
 * through vertex interpolators; Rotation, q in the mesh's local space.
 */
void BuildRigidFunction(UMaterialFunction* Function, const FVatMeta& Meta, UTexture* Positions, UTexture* Rotations)
{
	TRACE_CPUPROFILER_EVENT_SCOPE(LilaLinkVat::BuildRigidFunction);

	LilaLink::DeleteAllExpressions(Function);
	Function->Description = TEXT("Lila rigid Vertex Animation Texture: plays a baked rigid-body animation, a transform per piece per frame.");
	Function->bExposeToLibrary = true;
	FVatGraph G;
	G.Function = Function;

	const TCHAR* Playback = TEXT("Playback");
	const TCHAR* Layout = TEXT("Vertex Animation");
	UMaterialExpression* FrameRate = G.Scalar(TEXT("FrameRate"), 30.0f, Playback);
	UMaterialExpression* PlayRate = G.Scalar(TEXT("PlayRate"), 1.0f, Playback);
	UMaterialExpression* AutoPlay = G.Scalar(TEXT("AutoPlay"), 1.0f, Playback);
	UMaterialExpression* Loop = G.Scalar(TEXT("Loop"), 1.0f, Playback);
	UMaterialExpression* ManualFrame = G.Scalar(TEXT("Frame"), 0.0f, Playback);
	UMaterialExpression* FrameCount = G.Scalar(TEXT("FrameCount"), 1.0f, Layout);
	UMaterialExpression* RowsPerFrame = G.Scalar(TEXT("RowsPerFrame"), 1.0f, Layout);
	UMaterialExpression* TextureHeight = G.Scalar(TEXT("TextureHeight"), 1.0f, Layout);
	UMaterialExpression* Absolute = G.Scalar(TEXT("Absolute"), 0.0f, Layout);
	UMaterialExpression* Hdr = G.Scalar(TEXT("HDR"), 1.0f, Layout);
	UMaterialExpression* BoundsMin = G.Vector(TEXT("BoundsMin"), FLinearColor(0, 0, 0, 0), Layout);
	UMaterialExpression* BoundsMax = G.Vector(TEXT("BoundsMax"), FLinearColor(1, 1, 1, 0), Layout);
	auto* InterpolateSwitch = G.New<UMaterialExpressionStaticSwitchParameter>(-1400);
	InterpolateSwitch->ParameterName = TEXT("Interpolate");
	InterpolateSwitch->Group = Playback;
	InterpolateSwitch->DefaultValue = false;

	using Mul = UMaterialExpressionMultiply;
	using Sub = UMaterialExpressionSubtract;
	using Add = UMaterialExpressionAdd;
	UMaterialExpression* Time = G.Make(UMaterialExpressionTime::StaticClass());
	UMaterialExpression* Played = G.Binary(Mul::StaticClass(), G.Binary(Mul::StaticClass(), Time, 0, FrameRate, 0), 0, PlayRate, 0);
	UMaterialExpression* LastFrame = G.Binary(Sub::StaticClass(), FrameCount, 0, nullptr, 0);
	Cast<Sub>(LastFrame)->ConstB = 1.0f;
	UMaterialExpression* Wrapped = G.Binary(UMaterialExpressionFmod::StaticClass(), Played, 0, FrameCount, 0);
	UMaterialExpression* Held = G.Binary(UMaterialExpressionMin::StaticClass(), Played, 0, LastFrame, 0);
	UMaterialExpression* Raw = G.Lerp(ManualFrame, G.Lerp(Held, Wrapped, Loop), AutoPlay);
	auto* Floor = G.New<UMaterialExpressionFloor>();
	Floor->Input.Connect(0, Raw);
	UMaterialExpression* NotBelow = G.Binary(UMaterialExpressionMax::StaticClass(), Floor, 0, nullptr, 0);
	Cast<UMaterialExpressionMax>(NotBelow)->ConstB = 0.0f;
	UMaterialExpression* Frame = G.Binary(UMaterialExpressionMin::StaticClass(), NotBelow, 0, LastFrame, 0);
	auto* Blend = G.New<UMaterialExpressionFrac>();
	Blend->Input.Connect(0, Raw);
	UMaterialExpression* Up = G.Binary(Add::StaticClass(), Frame, 0, nullptr, 0);
	Cast<Add>(Up)->ConstB = 1.0f;
	UMaterialExpression* NextFrame = G.Lerp(G.Binary(UMaterialExpressionMin::StaticClass(), Up, 0, LastFrame, 0),
		G.Binary(UMaterialExpressionFmod::StaticClass(), Up, 0, FrameCount, 0), Loop);

	auto* Address = G.New<UMaterialExpressionTextureCoordinate>();
	Address->CoordinateIndex = Meta.UvChannel;
	auto* Zero = G.New<UMaterialExpressionConstant>();
	Zero->R = 0.0f;
	struct FDecoded
	{
		UMaterialExpression* Q;
		UMaterialExpression* Offset;
	};
	auto Decode = [&](UMaterialExpression* AtFrame) {
		UMaterialExpression* Rows = G.Binary(Mul::StaticClass(), AtFrame, 0, RowsPerFrame, 0);
		UMaterialExpression* VOffset = G.Binary(UMaterialExpressionDivide::StaticClass(), Rows, 0, TextureHeight, 0);
		UMaterialExpression* Shift = G.Binary(UMaterialExpressionAppendVector::StaticClass(), Zero, 0, VOffset, 0);
		UMaterialExpression* Uv = G.Binary(Add::StaticClass(), Address, 0, Shift, 0);
		UMaterialExpression* PosTexel = G.Sample(TEXT("PositionTexture"), Positions, Uv);
		UMaterialExpression* RotTexel = G.Sample(TEXT("RotationTexture"), Rotations, Uv);
		UMaterialExpression* Doubled = G.Binary(Mul::StaticClass(), RotTexel, 5, nullptr, 0);
		Cast<Mul>(Doubled)->ConstB = 2.0f;
		UMaterialExpression* Unpacked = G.Binary(Sub::StaticClass(), Doubled, 0, nullptr, 0);
		Cast<Sub>(Unpacked)->ConstB = 1.0f;
		auto* RotValue = G.New<UMaterialExpressionLinearInterpolate>();
		RotValue->A.Connect(0, Unpacked);
		RotValue->B.Connect(5, RotTexel);
		RotValue->Alpha.Connect(0, Hdr);
		auto* Unnormalised = G.New<UMaterialExpressionLinearInterpolate>();
		Unnormalised->A.Connect(0, BoundsMin);
		Unnormalised->B.Connect(0, BoundsMax);
		Unnormalised->Alpha.Connect(0, PosTexel);
		auto* PosValue = G.New<UMaterialExpressionLinearInterpolate>();
		PosValue->A.Connect(0, Unnormalised);
		PosValue->B.Connect(0, PosTexel);
		PosValue->Alpha.Connect(0, Hdr);
		return FDecoded{G.Normalize(RotValue), PosValue};
	};
	const FDecoded Now = Decode(Frame);
	const FDecoded Next = Decode(NextFrame);
	InterpolateSwitch->A.Connect(0, G.Normalize(G.Lerp(Now.Q, Next.Q, Blend)));
	InterpolateSwitch->B.Connect(0, Now.Q);
	auto* OffsetSwitch = G.New<UMaterialExpressionStaticSwitchParameter>(-1400);
	OffsetSwitch->ParameterName = TEXT("Interpolate");
	OffsetSwitch->Group = Playback;
	OffsetSwitch->DefaultValue = false;
	OffsetSwitch->A.Connect(0, G.Lerp(Now.Offset, Next.Offset, Blend));
	OffsetSwitch->B.Connect(0, Now.Offset);
	UMaterialExpression* Q = InterpolateSwitch;

	auto* PivotXY = G.New<UMaterialExpressionTextureCoordinate>();
	PivotXY->CoordinateIndex = Meta.PivotChannelXY;
	auto* PivotZ = G.New<UMaterialExpressionTextureCoordinate>();
	PivotZ->CoordinateIndex = Meta.PivotChannelZ;
	UMaterialExpression* Pivot = G.Binary(UMaterialExpressionAppendVector::StaticClass(), PivotXY, 0, G.Mask(PivotZ, 0, true, false, false, false), 0);
	auto* RelativePivot = G.New<UMaterialExpressionLinearInterpolate>();
	RelativePivot->A.Connect(0, Pivot);
	RelativePivot->ConstB = 0.0f;
	RelativePivot->Alpha.Connect(0, Absolute);
	UMaterialExpression* Centre = G.Binary(Add::StaticClass(), OffsetSwitch, 0, RelativePivot, 0);

	auto* Local = G.New<UMaterialExpressionLocalPosition>();
	Local->IncludedOffsets = EPositionIncludedOffsets::ExcludeOffsets;
	UMaterialExpression* Arm = G.Binary(Sub::StaticClass(), Local, 0, Pivot, 0);
	UMaterialExpression* Moved = G.Binary(Add::StaticClass(), G.Rotate(Q, Arm), 0, Centre, 0);
	UMaterialExpression* Offset = G.Binary(Sub::StaticClass(), Moved, 0, Local, 0);
	G.Output(G.Transform(Offset, TRANSFORMSOURCE_Local, TRANSFORM_World), RigidWpoOutput, 0, TEXT("World Position Offset."));

	auto RotateBasis = [&](UMaterialExpression* WorldVector) {
		UMaterialExpression* LocalVector = G.Transform(WorldVector, TRANSFORMSOURCE_World, TRANSFORM_Local);
		return G.Interpolate(G.Normalize(G.Transform(G.Rotate(Q, LocalVector), TRANSFORMSOURCE_Local, TRANSFORM_World)));
	};
	G.Output(RotateBasis(G.New<UMaterialExpressionVertexNormalWS>()), RigidNormalOutput, 1,
		TEXT("The vertex normal rotated with its piece, world space: connect to Normal with Tangent Space Normal off."));
	G.Output(RotateBasis(G.New<UMaterialExpressionVertexTangentWS>()), TEXT("Tangent"), 2, TEXT("The vertex tangent rotated with its piece, world space."));
	G.Output(G.Interpolate(Q), TEXT("Rotation"), 3, TEXT("The piece's rotation this frame, a quaternion (xyzw) in the mesh's local space."));

	Function->PostEditChange();
	Function->MarkPackageDirty();
	UMaterialEditingLibrary::UpdateMaterialFunction(Function);
}

/** M_LilaVATRigid_UV<n>: the rigid function's position and world-space normal, plus the vertex VAT material's surface parameters. */
void BuildRigidMaterial(UMaterial* Material, UMaterialFunction* Function, TArray<FString>& OutErrors)
{
	TRACE_CPUPROFILER_EVENT_SCOPE(LilaLinkVat::BuildRigidMaterial);

	UMaterialExpressionMaterialFunctionCall* Call = LilaLink::ResetToFunctionCall(Material, Function);
	FVatGraph G;
	G.Material = Material;
	if (!LilaLink::ConnectFunctionOutputs(Call, {{RigidWpoOutput, MP_WorldPositionOffset}, {RigidNormalOutput, MP_Normal}}))
	{
		OutErrors.Add(FString::Printf(TEXT("Could not wire the outputs of %s into %s"), *Function->GetName(), *Material->GetName()));
	}
	Material->bTangentSpaceNormal = false;
	UMaterialEditingLibrary::ConnectMaterialProperty(G.Vector(TEXT("BaseColor"), FLinearColor(0.5f, 0.5f, 0.5f), TEXT("Surface")), TEXT(""), MP_BaseColor);
	UMaterialEditingLibrary::ConnectMaterialProperty(G.Scalar(TEXT("Roughness"), 0.55f, TEXT("Surface")), TEXT(""), MP_Roughness);
	UMaterialEditingLibrary::ConnectMaterialProperty(G.Scalar(TEXT("Metallic"), 0.0f, TEXT("Surface")), TEXT(""), MP_Metallic);

	Material->SetMaterialUsage(MATUSAGE_InstancedStaticMeshes);
	Material->SetMaterialUsage(MATUSAGE_NiagaraMeshParticles);
	Material->PostEditChange();
	Material->MarkPackageDirty();
	UMaterialEditingLibrary::RecompileMaterial(Material);
}

/**
 * materialParent when given, else the generated material for this bake on its function: M_LilaVAT_UV<n> (or _NoNormals) for
 * a vertex bake, M_LilaVATRigid_<channels> for a rigid one. Both are built when either is missing or rebuildMaterial is set.
 */
UMaterialInterface* ParentMaterial(const FVatRequest& R, const FVatMeta& Meta, UTexture* Positions, UTexture* Normals, UTexture* Rotations,
	TArray<FString>& OutCreated, TArray<FString>& OutErrors)
{
	TRACE_CPUPROFILER_EVENT_SCOPE(LilaLinkVat::ParentMaterial);

	if (!R.MaterialParent.IsEmpty())
	{
		const FString Path = R.MaterialParent.Contains(TEXT(".")) ? R.MaterialParent : R.MaterialParent + TEXT(".") + FPackageName::GetShortName(R.MaterialParent);
		if (UMaterialInterface* Parent = LoadObject<UMaterialInterface>(nullptr, *Path))
		{
			return Parent;
		}
		OutErrors.Add(FString::Printf(TEXT("No material at %s"), *R.MaterialParent));
		return nullptr;
	}
	FString Kind, Suffix;
	if (Meta.bRigid)
	{
		Kind = TEXT("VATRigid_");
		Suffix = Meta.PivotChannelXY == Meta.UvChannel + 1 && Meta.PivotChannelZ == Meta.UvChannel + 2
			? FString::Printf(TEXT("UV%d"), Meta.UvChannel)
			: FString::Printf(TEXT("UV%d_%d_%d"), Meta.UvChannel, Meta.PivotChannelXY, Meta.PivotChannelZ);
	}
	else
	{
		Kind = TEXT("VAT_");
		Suffix = FString::Printf(TEXT("UV%d%s"), Meta.UvChannel, Normals ? TEXT("") : TEXT("_NoNormals"));
	}
	FString Error;
	const LilaLink::FMaterialPair Pair = LilaLink::FindOrCreateMaterialPair(R.MaterialFolder, TEXT("MF_Lila") + Kind + Suffix, TEXT("M_Lila") + Kind + Suffix, Error);
	if (!Pair.Material)
	{
		OutErrors.Add(Error);
		return nullptr;
	}
	if (Pair.bCreated || R.bRebuildMaterial)
	{
		if (Meta.bRigid)
		{
			BuildRigidFunction(Pair.Function, Meta, Positions, Rotations);
			BuildRigidMaterial(Pair.Material, Pair.Function, OutErrors);
		}
		else
		{
			BuildVatFunction(Pair.Function, Meta.UvChannel, Positions, Normals);
			BuildVatMaterial(Pair.Material, Pair.Function, Normals != nullptr, OutErrors);
		}
		OutCreated.AddUnique(Pair.Function->GetPathName());
		OutCreated.AddUnique(Pair.Material->GetPathName());
	}
	return Pair.Material;
}

UMaterialInstanceConstant* MakeInstance(const FVatRequest& R, UMaterialInterface* Parent, TArray<FString>& OutCreated, TArray<FString>& OutErrors)
{
	TRACE_CPUPROFILER_EVENT_SCOPE(LilaLinkVat::MakeInstance);

	const FString Name = TEXT("MI_") + R.AssetName;
	UMaterialInstanceConstant* Instance = FindAssetAt<UMaterialInstanceConstant>(R.PackagePath, Name);
	if (!Instance)
	{
		UMaterialInstanceConstantFactoryNew* Factory = NewObject<UMaterialInstanceConstantFactoryNew>(GetTransientPackage());
		Factory->InitialParent = Parent;
		Instance = Cast<UMaterialInstanceConstant>(FAssetToolsModule::GetModule().Get().CreateAsset(Name, R.PackagePath, UMaterialInstanceConstant::StaticClass(), Factory));
	}
	if (!Instance)
	{
		OutErrors.Add(FString::Printf(TEXT("Could not create material instance %s in %s"), *Name, *R.PackagePath));
		return nullptr;
	}
	if (Instance->Parent != Parent)
	{
		UMaterialEditingLibrary::SetMaterialInstanceParent(Instance, Parent);
	}
	OutCreated.AddUnique(Instance->GetPathName());
	return Instance;
}

void SetInstanceParameters(UMaterialInstanceConstant* Instance, const FVatMeta& Meta, UTexture* Positions, UTexture* Normals, UTexture* Rotations)
{
	TRACE_CPUPROFILER_EVENT_SCOPE(LilaLinkVat::SetInstanceParameters);

	// A user's own parent may have only some of these; the ones it lacks are skipped.
	auto Names = [&](EMaterialParameterType Type) {
		TArray<FMaterialParameterInfo> Infos;
		TArray<FGuid> Ids;
		Instance->GetAllParameterInfoOfType(Type, Infos, Ids);
		TSet<FName> Out;
		for (const FMaterialParameterInfo& Info : Infos)
		{
			Out.Add(Info.Name);
		}
		return Out;
	};
	const TSet<FName> Scalars = Names(EMaterialParameterType::Scalar);
	const TSet<FName> Vectors = Names(EMaterialParameterType::Vector);
	const TSet<FName> Textures = Names(EMaterialParameterType::Texture);
	auto SetScalar = [&](const TCHAR* Name, float Value) {
		if (Scalars.Contains(Name))
		{
			UMaterialEditingLibrary::SetMaterialInstanceScalarParameterValue(Instance, Name, Value);
		}
	};
	auto SetVector = [&](const TCHAR* Name, const FVector& Value) {
		if (Vectors.Contains(Name))
		{
			UMaterialEditingLibrary::SetMaterialInstanceVectorParameterValue(Instance, Name, FLinearColor(FVector3f(Value)));
		}
	};
	auto SetTexture = [&](const TCHAR* Name, UTexture* Value) {
		if (Value && Textures.Contains(Name))
		{
			UMaterialEditingLibrary::SetMaterialInstanceTextureParameterValue(Instance, Name, Value);
		}
	};
	SetTexture(TEXT("PositionTexture"), Positions);
	SetTexture(TEXT("NormalTexture"), Normals);
	SetTexture(TEXT("RotationTexture"), Rotations);
	SetScalar(TEXT("HDR"), Meta.bHdr ? 1.0f : 0.0f);
	SetVector(TEXT("BoundsMin"), Meta.BoundsMin);
	SetVector(TEXT("BoundsMax"), Meta.BoundsMax);
	SetScalar(TEXT("FrameCount"), float(Meta.FrameCount));
	SetScalar(TEXT("RowsPerFrame"), float(Meta.RowsPerFrame));
	SetScalar(TEXT("TextureHeight"), float(Meta.Height));
	SetScalar(TEXT("FrameRate"), float(Meta.Fps));
	SetScalar(TEXT("Absolute"), Meta.bAbsolute ? 1.0f : 0.0f);
	UMaterialEditingLibrary::UpdateMaterialInstance(Instance);
	Instance->MarkPackageDirty();
}

/** Half float EXR stays half, float EXR and 16-bit PNG keep their precision as RGBA32F, 8-bit PNG stays RGBA8. Rigid rotations use the same. */
TextureCompressionSettings PositionCompression(const FVatMeta& Meta)
{
	if (Meta.bHdr)
	{
		return Meta.PositionBits > 16 ? TC_HDR_F32 : TC_HDR;
	}
	return Meta.PositionBits <= 8 ? TC_VectorDisplacementmap : TC_HDR_F32;
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

/** The position and normal textures, the material and its instance, assigned to every slot of Mesh when there is one. */
void BuildVatAssets(const FVatRequest& R, const FVatMeta& Meta, UStaticMesh* Mesh, TArray<FString>& Assets, TArray<FString>& Errors,
	TFunctionRef<void(const TCHAR*)> Progress)
{
	TRACE_CPUPROFILER_EVENT_SCOPE(LilaLinkVat::BuildVatAssets);

	Progress(TEXT("importing the textures"));
	const FString Base = R.AssetName.StartsWith(TEXT("SM_")) ? R.AssetName.RightChop(3) : R.AssetName;
	UTexture2D* Positions = ImportDataTexture(R.Positions, R.PackagePath, TEXT("T_") + Base + TEXT("_VATPos"), PositionCompression(Meta), Assets, Errors);
	UTexture2D* Normals = nullptr;
	if (!R.Normals.IsEmpty())
	{
		Normals = ImportDataTexture(R.Normals, R.PackagePath, TEXT("T_") + Base + TEXT("_VATNrm"), Meta.NormalBits > 8 ? TC_HDR : TC_VectorDisplacementmap, Assets, Errors);
	}
	UTexture2D* Rotations = nullptr;
	if (Meta.bRigid)
	{
		Rotations = ImportDataTexture(R.Rotations, R.PackagePath, TEXT("T_") + Base + TEXT("_VATRot"), PositionCompression(Meta), Assets, Errors);
		if (!Rotations)
		{
			return;
		}
	}

	if (Mesh && Positions)
	{
		Progress(TEXT("building the material"));
		if (UMaterialInterface* Parent = ParentMaterial(R, Meta, Positions, Normals, Rotations, Assets, Errors))
		{
			if (UMaterialInstanceConstant* Instance = MakeInstance(R, Parent, Assets, Errors))
			{
				SetInstanceParameters(Instance, Meta, Positions, Normals, Rotations);
				Mesh->Modify();
				for (int32 Slot = 0; Slot < Mesh->GetStaticMaterials().Num(); ++Slot)
				{
					Mesh->SetMaterial(Slot, Instance);
				}
				Mesh->MarkPackageDirty();
			}
		}
	}
}

void ImportVertexAnimation(const TSharedPtr<FJsonObject>& Params, const FLilaLinkCallRef& Call)
{
	TRACE_CPUPROFILER_EVENT_SCOPE(LilaLinkVat::ImportVertexAnimation);

	FVatRequest R;
	FString Error;
	if (!ParseRequest(Params, R, Error))
	{
		Call->Reply(nullptr, Error);
		return;
	}
	const FVatMeta Meta = ParseMeta(R.Meta);
	TArray<FString> Assets, Warnings, Errors;

	Call->Progress(TEXT("importing the rest mesh"));
	UStaticMesh* Mesh = ImportMesh(R, Errors);
	if (Mesh)
	{
		ConfigureMesh(Mesh, Meta);
		Assets.Add(Mesh->GetPathName());
	}

	BuildVatAssets(R, Meta, Mesh, Assets, Errors, [&Call](const TCHAR* Stage) { Call->Progress(Stage); });

	for (const FString& E : Errors)
	{
		UE_LOG(LogLilaLink, Error, TEXT("%s: %s"), *R.AssetName, *E);
	}
	UE_LOG(LogLilaLink, Log, TEXT("importVertexAnimation %s/%s: %d asset(s), %d frame(s) of %dx%d%s"), *R.PackagePath, *R.AssetName, Assets.Num(), Meta.FrameCount,
		Meta.Width, Meta.Height, Meta.bRigid ? *FString::Printf(TEXT(", rigid, %d piece(s)"), Meta.PieceCount) : TEXT(""));
	if (!Assets.IsEmpty() && (R.bSave || GetDefault<ULilaLinkSettings>()->bAutoSaveAfterImport))
	{
		Call->Progress(TEXT("saving"));
		LilaLink::SaveAssetPaths(Assets, Errors);
	}

	TSharedRef<FJsonObject> Out = MakeShared<FJsonObject>();
	Out->SetField(TEXT("assets"), Strings(Assets));
	Out->SetField(TEXT("warnings"), Strings(Warnings));
	Out->SetField(TEXT("errors"), Strings(Errors));
	Out->SetStringField(TEXT("method"), TEXT("vat"));
	Call->Reply(MakeShared<FJsonValueObject>(Out));
}
} // namespace

void LilaLink::AddVatMethods(TMap<FString, FLilaLinkMethod>& Methods)
{
	Methods.Add(TEXT("importVertexAnimation"), &ImportVertexAnimation);
}

bool LilaLink::SetUpVatOnMesh(UStaticMesh* Mesh, const FString& MetaFile, TArray<FString>& OutAssets, TArray<FString>& OutErrors)
{
	TRACE_CPUPROFILER_EVENT_SCOPE(LilaLinkVat::SetUpVatOnMesh);

	FString Text;
	FFileHelper::LoadFileToString(Text, *MetaFile);
	TSharedPtr<FJsonObject> Json;
	TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(Text);
	if (!FJsonSerializer::Deserialize(Reader, Json) || !Json.IsValid())
	{
		OutErrors.Add(FString::Printf(TEXT("%s is not a VAT metadata file"), *MetaFile));
		return false;
	}

	FVatRequest R;
	R.PackagePath = FPackageName::GetLongPackagePath(Mesh->GetPackage()->GetName());
	R.AssetName = Mesh->GetName();
	R.Meta = Json;

	const FString Folder = FPaths::GetPath(MetaFile);
	const FString Name = FPaths::GetBaseFilename(MetaFile);
	const TSharedPtr<FJsonObject>* Files = nullptr;
	Json->TryGetObjectField(TEXT("files"), Files);
	auto BakeFile = [&](const TCHAR* Key, std::initializer_list<FString> Fallbacks) {
		FString Listed;
		if (Files && (*Files)->TryGetStringField(Key, Listed) && !Listed.IsEmpty())
		{
			return FPaths::ConvertRelativePathToFull(Folder, Listed);
		}
		for (const FString& Fallback : Fallbacks)
		{
			if (FPaths::FileExists(Folder / Fallback))
			{
				return FPaths::ConvertRelativePathToFull(Folder / Fallback);
			}
		}
		return FString();
	};
	R.Positions = BakeFile(TEXT("positions"), {Name + TEXT("_pos.exr"), Name + TEXT("_pos.png")});
	R.Normals = BakeFile(TEXT("normals"), {Name + TEXT("_nrm.png")});
	R.Rotations = BakeFile(TEXT("rotations"), {Name + TEXT("_rot.exr"), Name + TEXT("_rot.png")});
	if (R.Positions.IsEmpty() || !FPaths::FileExists(R.Positions))
	{
		OutErrors.Add(FString::Printf(TEXT("No position texture beside %s"), *MetaFile));
		return false;
	}
	if (!R.Normals.IsEmpty() && !FPaths::FileExists(R.Normals))
	{
		OutErrors.Add(FString::Printf(TEXT("No normal texture at %s; set up without normals"), *R.Normals));
		R.Normals.Reset();
	}

	const FVatMeta Meta = ParseMeta(R.Meta);
	if (Meta.bRigid && (R.Rotations.IsEmpty() || !FPaths::FileExists(R.Rotations)))
	{
		OutErrors.Add(FString::Printf(TEXT("No rotation texture beside %s"), *MetaFile));
		return false;
	}
	const int32 LastChannel = Meta.bRigid ? FMath::Max3(Meta.UvChannel, Meta.PivotChannelXY, Meta.PivotChannelZ) : Meta.UvChannel;
	if (Mesh->GetNumUVChannels(0) <= LastChannel)
	{
		OutErrors.Add(FString::Printf(TEXT("%s has %d UV channel(s); the bake reads UV %d, so it is not the mesh this bake was made from"),
			*Mesh->GetName(), Mesh->GetNumUVChannels(0), LastChannel));
		return false;
	}
	ConfigureMesh(Mesh, Meta);
	OutAssets.AddUnique(Mesh->GetPathName());
	BuildVatAssets(R, Meta, Mesh, OutAssets, OutErrors, [](const TCHAR*) {});
	return OutErrors.IsEmpty();
}

void LilaLink::SetUpVatDataTexture(UTexture2D* Texture, bool bNormals)
{
	TRACE_CPUPROFILER_EVENT_SCOPE(LilaLinkVat::SetUpVatDataTexture);

	TextureCompressionSettings Compression = TC_VectorDisplacementmap;
	switch (Texture->Source.GetFormat())
	{
	case TSF_RGBA16F:
	case TSF_R16F:
		Compression = TC_HDR;
		break;
	case TSF_RGBA16:
	case TSF_G16:
	case TSF_RGBA32F:
	case TSF_R32F:
		Compression = bNormals ? TC_HDR : TC_HDR_F32;
		break;
	default:
		break;
	}
	Texture->Modify();
	Texture->PreEditChange(nullptr);
	ApplyDataSettings(Texture, Compression);
	Texture->PostEditChange();
	Texture->MarkPackageDirty();
}
