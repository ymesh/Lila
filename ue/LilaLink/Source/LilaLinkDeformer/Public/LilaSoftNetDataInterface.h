#pragma once

#include "ComputeFramework/ComputeDataProvider.h"
#include "CoreMinimal.h"
#include "OptimusComputeDataInterface.h"
#include "PrimitiveComponentId.h"
#include "RenderGraphResources.h"

#include "LilaSoftNetDataInterface.generated.h"

class USkinnedMeshComponent;
class ULilaSoftNetData;

/**
 * Lila Soft Net in a Deformer Graph: each vertex's offset from the net, the sum of its precomputed influence entries
 * times the moves of the net's knots and tangent handles (read from their bones each frame). Pin: Net Offset
 * (component space), added to the skinned position.
 */
UCLASS(Category = ComputeFramework)
class LILALINKDEFORMER_API ULilaSoftNetDataInterface : public UOptimusComputeDataInterface
{
	GENERATED_BODY()

public:
	//~ Begin UOptimusComputeDataInterface Interface
	FString GetDisplayName() const override;
	TArray<FOptimusCDIPinDefinition> GetPinDefinitions() const override;
	TSubclassOf<UActorComponent> GetRequiredComponentClass() const override;
	//~ End UOptimusComputeDataInterface Interface

	//~ Begin UComputeDataInterface Interface
	TCHAR const* GetClassName() const override { return TEXT("LilaSoftNet"); }
	bool CanSupportUnifiedDispatch() const override { return true; }
	void GetSupportedInputs(TArray<FShaderFunctionDefinition>& OutFunctions) const override;
	void GetShaderParameters(TCHAR const* UID, FShaderParametersMetadataBuilder& InOutBuilder, FShaderParametersMetadataAllocations& InOutAllocations) const override;
	void GetShaderHash(FString& InOutKey) const override;
	void GetHLSL(FString& OutHLSL, FString const& InDataInterfaceName) const override;
	UComputeDataProvider* CreateDataProvider(TObjectPtr<UObject> InBinding, uint64 InInputMask, uint64 InOutputMask) const override;
	//~ End UComputeDataInterface Interface
};

/** One LOD's binding, uploaded once. */
struct FLilaSoftNetGpuLod
{
	int32 VertexCount = 0;
	TArray<uint32> Offsets;
	TArray<FVector2f> Entries;
	TRefCountPtr<FRDGPooledBuffer> PooledOffsets;
	TRefCountPtr<FRDGPooledBuffer> PooledEntries;
};

/** The binding uploaded once per mesh revision, shared by the provider and its render proxies. */
struct FLilaSoftNetGpuBinding
{
	int32 Revision = -1;
	TArray<FLilaSoftNetGpuLod> Lods;
};

UCLASS(BlueprintType, EditInlineNew, Category = ComputeFramework)
class LILALINKDEFORMER_API ULilaSoftNetDataProvider : public UComputeDataProvider
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
	TSharedPtr<FLilaSoftNetGpuBinding, ESPMode::ThreadSafe> Binding;
	/** Bone indices in the mesh: per knot (frame, control), per tangent (bone, rest). */
	TArray<FIntPoint> KnotBones;
	TArray<FIntPoint> TangentBones;
	TWeakObjectPtr<const ULilaSoftNetData> ResolvedFor;
	int32 ResolvedRevision = -1;
};
