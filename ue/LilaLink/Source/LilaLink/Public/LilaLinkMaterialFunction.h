#pragma once

#include "CoreMinimal.h"
#include "Engine/EngineTypes.h"
#include "Materials/MaterialExpressionFunctionInput.h"
#include "SceneTypes.h"

class UMaterial;
class UMaterialExpression;
class UMaterialExpressionMaterialFunctionCall;
class UMaterialFunction;
class UMaterialFunctionInterface;
class UTexture;

namespace LilaLink
{
/** A generated material function and the material built on it. */
struct FMaterialPair
{
	UMaterialFunction* Function = nullptr;
	UMaterial* Material = nullptr;
	/** Either was just created, so both need building. */
	bool bCreated = false;
};

/**
 * Folder/FunctionName and Folder/MaterialName, each created when missing; both null, with OutError, when either can't be
 * made. The caller builds both when bCreated or when asked to rebuild, never one alone.
 */
LILALINK_API FMaterialPair FindOrCreateMaterialPair(const FString& Folder, const FString& FunctionName, const FString& MaterialName, FString& OutError);

/** Folder/Name, created when missing (bOutCreated set); null when it can't be made. */
LILALINK_API UMaterialFunction* FindOrCreateMaterialFunction(const FString& Folder, const FString& Name, bool& bOutCreated);
LILALINK_API UMaterial* FindOrCreateMaterial(const FString& Folder, const FString& Name, bool& bOutCreated);

/** Where the generated master materials and the MF_Lila functions live. */
LILALINK_API const TCHAR* MastersFolderPath();

/**
 * MF_LilaTerrainLayer, built or upgraded when needed: adds one weight-blended layer (tinted top-projected texture and
 * roughness, times Weight) to what the calls before it summed. Null with a warning when it can't be made.
 */
LILALINK_API UMaterialFunction* TerrainLayerFunction(TArray<FString>& OutWarnings);

/**
 * A material function's graph, cleared and rebuilt down columns. Its parameter inputs default to a parameter of the same
 * name, so a material that calls the function and leaves a pin empty still has that parameter for its instances to set,
 * and Lila's instances drive a user's own material the same way they drive the generated one.
 */
struct LILALINK_API FFunctionGraph
{
	FFunctionGraph(UMaterialFunction* InFunction, const TCHAR* Description);

	UMaterialFunction* Function = nullptr;
	int32 Row = 0;
	int32 InputPriority = 0;
	int32 OutputPriority = 0;

	UMaterialExpression* Make(TSubclassOf<UMaterialExpression> Class, int32 X = -600);
	template <class T> T* New(int32 X = -600) { return Cast<T>(Make(T::StaticClass(), X)); }

	/** An input whose value is Default (any expression in the function) when the caller leaves it empty. */
	UMaterialExpressionFunctionInput* Input(const TCHAR* Name, EFunctionInputType Type, UMaterialExpression* Default, const TCHAR* Description,
		int32 DefaultOutput = 0);
	UMaterialExpressionFunctionInput* ScalarInput(const TCHAR* Name, float Value, const TCHAR* Group, const TCHAR* Description);
	/** Vector3 from a vector parameter's RGB. */
	UMaterialExpressionFunctionInput* VectorInput(const TCHAR* Name, const FLinearColor& Value, const TCHAR* Group, const TCHAR* Description);
	UMaterialExpressionFunctionInput* TextureInput(const TCHAR* Name, UTexture* Value, EMaterialSamplerType Type, const TCHAR* Group,
		const TCHAR* Description);
	void Output(UMaterialExpression* From, const TCHAR* Name, const TCHAR* Description, int32 FromOutput = 0);
	/**
	 * Full on SM5/SM6, Mobile on ES3.1 (mobile forward and mobile deferred), so one function carries its cheap mobile
	 * path. Mobile null uses Full everywhere.
	 */
	UMaterialExpression* FeatureLevelSwitch(UMaterialExpression* Full, UMaterialExpression* Mobile, int32 FullOutput = 0,
		int32 MobileOutput = 0);

	/** Marks the function changed and recompiles the materials that call it. */
	void Finish();
};

/**
 * Deletes every expression in the graph. UMaterialEditingLibrary::DeleteAllMaterialExpressions removes while iterating the
 * same array, so it leaves every other expression behind.
 */
LILALINK_API void DeleteAllExpressions(UMaterial* Material);
LILALINK_API void DeleteAllExpressions(UMaterialFunction* Function);

/** Clears Material's graph and places a call of Function in it. */
LILALINK_API UMaterialExpressionMaterialFunctionCall* ResetToFunctionCall(UMaterial* Material, UMaterialFunctionInterface* Function);

/** Wires the call's named outputs to the material's properties; false when any output is missing. */
LILALINK_API bool ConnectFunctionOutputs(UMaterialExpressionMaterialFunctionCall* Call, std::initializer_list<TPair<const TCHAR*, EMaterialProperty>> Outputs);
} // namespace LilaLink
