#pragma once

#include "ComputeFramework/ComputeDataProvider.h"
#include "CoreMinimal.h"
#include "OptimusComputeDataInterface.h"
#include "PrimitiveComponentId.h"
#include "RenderGraphResources.h"

#include "LilaBendBonesDataInterface.generated.h"

class FSceneInterface;
class USkinnedMeshComponent;
class ULilaBendBonesData;

/**
 * Lila Bend Bones in a Deformer Graph: each vertex bent along its chains' curves as Lila's bend bones deformer bends it,
 * from the ULilaBendBonesData that LilaLink's unrealBendBones puts on the skeletal mesh. Pins: Bent Position (component
 * space) and Bent Rotation (the turn from rest to posed at the vertex, for its normal and tangent).
 */
UCLASS(Category = ComputeFramework)
class LILALINKDEFORMER_API ULilaBendBonesDataInterface : public UOptimusComputeDataInterface
{
	GENERATED_BODY()

public:
	//~ Begin UOptimusComputeDataInterface Interface
	FString GetDisplayName() const override;
	TArray<FOptimusCDIPinDefinition> GetPinDefinitions() const override;
	TSubclassOf<UActorComponent> GetRequiredComponentClass() const override;
	//~ End UOptimusComputeDataInterface Interface

	//~ Begin UComputeDataInterface Interface
	TCHAR const* GetClassName() const override { return TEXT("LilaBendBones"); }
	bool CanSupportUnifiedDispatch() const override { return true; }
	void GetSupportedInputs(TArray<FShaderFunctionDefinition>& OutFunctions) const override;
	void GetShaderParameters(TCHAR const* UID, FShaderParametersMetadataBuilder& InOutBuilder, FShaderParametersMetadataAllocations& InOutAllocations) const override;
	void GetShaderHash(FString& InOutKey) const override;
	void GetHLSL(FString& OutHLSL, FString const& InDataInterfaceName) const override;
	UComputeDataProvider* CreateDataProvider(TObjectPtr<UObject> InBinding, uint64 InInputMask, uint64 InOutputMask) const override;
	//~ End UComputeDataInterface Interface
};

/** One LOD's binding, uploaded once. */
struct FLilaBendBonesGpuLod
{
	int32 VertexCount = 0;
	TArray<FVector4f> Entries;
	TRefCountPtr<FRDGPooledBuffer> Pooled;
};

/** The binding uploaded once per mesh revision, shared by the provider and its render proxies. */
struct FLilaBendBonesGpuBinding
{
	int32 Revision = -1;
	TArray<FLilaBendBonesGpuLod> Lods;
};

UCLASS(BlueprintType, EditInlineNew, Category = ComputeFramework)
class LILALINKDEFORMER_API ULilaBendBonesDataProvider : public UComputeDataProvider
{
	GENERATED_BODY()

public:
	TWeakObjectPtr<USkinnedMeshComponent> SkinnedMesh;

	//~ Begin UComputeDataProvider Interface
	FComputeDataProviderRenderProxy* GetRenderProxy() override;
	//~ End UComputeDataProvider Interface

	/** The uploaded binding is a GPU buffer: it is let go on the render thread, before the renderer shuts down. */
	void BeginDestroy() override;

private:
	TSharedPtr<FLilaBendBonesGpuBinding, ESPMode::ThreadSafe> Binding;
	/** Each chain's bone indices in the mesh, resolved for this mesh and revision. */
	TArray<TArray<int32>> BoneIndices;
	/** Each chain's CVs' driver bone indices (INDEX_NONE: the CV rides its joint). */
	TArray<TArray<int32>> CvDrivers;
	TWeakObjectPtr<const ULilaBendBonesData> ResolvedFor;
	int32 ResolvedRevision = -1;
};
