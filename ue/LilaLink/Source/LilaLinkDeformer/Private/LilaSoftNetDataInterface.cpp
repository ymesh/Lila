#include "LilaSoftNetDataInterface.h"

#include "LilaSoftNetData.h"

#include "Components/SkinnedMeshComponent.h"
#include "ComputeFramework/ShaderParamTypeDefinition.h"
#include "Engine/SkeletalMesh.h"
#include "Engine/SkinnedAsset.h"
#include "OptimusDataDomain.h"
#include "ProfilingDebugging/CpuProfilerTrace.h"
#include "RenderGraphBuilder.h"
#include "RenderGraphUtils.h"
#include "RenderingThread.h"
#include "Rendering/SkeletalMeshLODRenderData.h"
#include "Rendering/SkeletalMeshRenderData.h"
#include "ShaderParameterMetadataBuilder.h"
#include "SkeletalMeshDeformerHelpers.h"
#include "SkeletalRenderPublic.h"

#include UE_INLINE_GENERATED_CPP_BY_NAME(LilaSoftNetDataInterface)

namespace
{
BEGIN_SHADER_PARAMETER_STRUCT(FLilaSoftNetParameters, )
	SHADER_PARAMETER(uint32, NumVertices)
	SHADER_PARAMETER_RDG_BUFFER_SRV(StructuredBuffer<uint>, Offsets)
	SHADER_PARAMETER_RDG_BUFFER_SRV(StructuredBuffer<float2>, Entries)
	SHADER_PARAMETER_RDG_BUFFER_SRV(StructuredBuffer<float4>, Moves)
END_SHADER_PARAMETER_STRUCT()

// Lila src/deform/SoftNet.cpp InfluenceTable::apply: a vertex's offset is the sum over its entries of weight times its
// degree of freedom's move (knots first, then tangent handles).
const TCHAR* const SoftNetHlsl = TEXT(R"(
uint {DataInterfaceName}_NumVertices;
StructuredBuffer<uint> {DataInterfaceName}_Offsets;
StructuredBuffer<float2> {DataInterfaceName}_Entries;
StructuredBuffer<float4> {DataInterfaceName}_Moves;

uint ReadNumVertices_{DataInterfaceName}()
{
	return {DataInterfaceName}_NumVertices;
}

float3 ReadNetOffset_{DataInterfaceName}(uint VertexIndex)
{
	float3 D = 0;
	const uint End = {DataInterfaceName}_Offsets[VertexIndex + 1];
	for (uint E = {DataInterfaceName}_Offsets[VertexIndex]; E < End; ++E)
	{
		const float2 Entry = {DataInterfaceName}_Entries[E];
		D += Entry.y * {DataInterfaceName}_Moves[asuint(Entry.x)].xyz;
	}
	return D;
}
)");

class FLilaSoftNetProxy final : public FComputeDataProviderRenderProxy
{
public:
	using FParameters = FLilaSoftNetParameters;

	FSceneInterface* SceneInterface = nullptr;
	FPrimitiveComponentId ComponentId;
	/** The LOD being deformed, chosen in IsValid from the one the mesh renders. */
	mutable int32 Lod = 0;
	TSharedPtr<FLilaSoftNetGpuBinding, ESPMode::ThreadSafe> Binding;
	TArray<FVector4f> Moves;

	FRDGBufferSRVRef OffsetsSRV = nullptr, EntriesSRV = nullptr, MovesSRV = nullptr;

	bool IsValid(FValidationData const& InValidationData) const override
	{
		if (InValidationData.ParameterStructSize != sizeof(FParameters) || !Binding || Moves.Num() == 0)
		{
			return false;
		}
		FSkeletalMeshObject* MeshObject = FSkeletalMeshDeformerHelpers::GetSkeletalMeshObject(SceneInterface, ComponentId);
		if (!MeshObject)
		{
			return false;
		}
		Lod = MeshObject->GetLOD();
		const TIndirectArray<FSkeletalMeshLODRenderData>& RenderLods = MeshObject->GetSkeletalMeshRenderData().LODRenderData;
		return Binding->Lods.IsValidIndex(Lod) && RenderLods.IsValidIndex(Lod) && int32(RenderLods[Lod].GetNumVertices()) == Binding->Lods[Lod].VertexCount &&
			   Binding->Lods[Lod].Entries.Num() > 0;
	}

	void AllocateResources(FRDGBuilder& GraphBuilder, FAllocationData const& InAllocationData) override
	{
		TRACE_CPUPROFILER_EVENT_SCOPE(LilaSoftNetProxy::AllocateResources);

		FLilaSoftNetGpuLod& Bound = Binding->Lods[Lod];
		FRDGBufferRef Offsets, Entries;
		if (Bound.PooledOffsets.IsValid() && Bound.PooledEntries.IsValid())
		{
			Offsets = GraphBuilder.RegisterExternalBuffer(Bound.PooledOffsets);
			Entries = GraphBuilder.RegisterExternalBuffer(Bound.PooledEntries);
		}
		else
		{
			Offsets = CreateStructuredBuffer(GraphBuilder, TEXT("LilaSoftNet.Offsets"), Bound.Offsets);
			Entries = CreateStructuredBuffer(GraphBuilder, TEXT("LilaSoftNet.Entries"), Bound.Entries);
			Bound.PooledOffsets = GraphBuilder.ConvertToExternalBuffer(Offsets);
			Bound.PooledEntries = GraphBuilder.ConvertToExternalBuffer(Entries);
		}
		OffsetsSRV = GraphBuilder.CreateSRV(Offsets);
		EntriesSRV = GraphBuilder.CreateSRV(Entries);
		MovesSRV = GraphBuilder.CreateSRV(CreateStructuredBuffer(GraphBuilder, TEXT("LilaSoftNet.Moves"), Moves));
	}

	void GatherDispatchData(FDispatchData const& InDispatchData) override
	{
		const TStridedView<FParameters> ParameterArray = MakeStridedParameterView<FParameters>(InDispatchData);
		for (int32 I = 0; I < ParameterArray.Num(); ++I)
		{
			FParameters& P = ParameterArray[I];
			P.NumVertices = uint32(Binding->Lods[Lod].VertexCount);
			P.Offsets = OffsetsSRV;
			P.Entries = EntriesSRV;
			P.Moves = MovesSRV;
		}
	}
};
} // namespace

FString ULilaSoftNetDataInterface::GetDisplayName() const
{
	return TEXT("Lila Soft Net");
}

TArray<FOptimusCDIPinDefinition> ULilaSoftNetDataInterface::GetPinDefinitions() const
{
	TArray<FOptimusCDIPinDefinition> Defs;
	Defs.Add({"NumVertices", "ReadNumVertices", false});
	Defs.Add({"NetOffset", "ReadNetOffset", Optimus::DomainName::Vertex, "ReadNumVertices"});
	return Defs;
}

TSubclassOf<UActorComponent> ULilaSoftNetDataInterface::GetRequiredComponentClass() const
{
	return USkinnedMeshComponent::StaticClass();
}

void ULilaSoftNetDataInterface::GetSupportedInputs(TArray<FShaderFunctionDefinition>& OutFunctions) const
{
	OutFunctions.AddDefaulted_GetRef().SetName(TEXT("ReadNumVertices")).AddReturnType(EShaderFundamentalType::Uint);
	OutFunctions.AddDefaulted_GetRef().SetName(TEXT("ReadNetOffset")).AddReturnType(EShaderFundamentalType::Float, 3).AddParam(EShaderFundamentalType::Uint);
}

void ULilaSoftNetDataInterface::GetShaderParameters(TCHAR const* UID, FShaderParametersMetadataBuilder& InOutBuilder, FShaderParametersMetadataAllocations& InOutAllocations) const
{
	InOutBuilder.AddNestedStruct<FLilaSoftNetParameters>(UID);
}

void ULilaSoftNetDataInterface::GetShaderHash(FString& InOutKey) const
{
	InOutKey += FString::Printf(TEXT("LilaSoftNet%08x"), GetTypeHash(FString(SoftNetHlsl)));
}

void ULilaSoftNetDataInterface::GetHLSL(FString& OutHLSL, FString const& InDataInterfaceName) const
{
	const TMap<FString, FStringFormatArg> Args = {{TEXT("DataInterfaceName"), InDataInterfaceName}};
	OutHLSL += FString::Format(SoftNetHlsl, Args);
}

UComputeDataProvider* ULilaSoftNetDataInterface::CreateDataProvider(TObjectPtr<UObject> InBinding, uint64 InInputMask, uint64 InOutputMask) const
{
	ULilaSoftNetDataProvider* Provider = NewObject<ULilaSoftNetDataProvider>();
	Provider->SkinnedMesh = Cast<USkinnedMeshComponent>(InBinding);
	return Provider;
}

void ULilaSoftNetDataProvider::BeginDestroy()
{
	if (Binding)
	{
		ENQUEUE_RENDER_COMMAND(LilaSoftNetRelease)([Released = MoveTemp(Binding)](FRHICommandListImmediate&) mutable { Released.Reset(); });
	}
	Super::BeginDestroy();
}

FComputeDataProviderRenderProxy* ULilaSoftNetDataProvider::GetRenderProxy()
{
	TRACE_CPUPROFILER_EVENT_SCOPE(LilaSoftNetDataProvider::GetRenderProxy);

	FLilaSoftNetProxy* Proxy = new FLilaSoftNetProxy();
	USkinnedMeshComponent* Component = SkinnedMesh.Get();
	const USkeletalMesh* Mesh = Component ? Cast<USkeletalMesh>(Component->GetSkinnedAsset()) : nullptr;
	const ULilaSoftNetData* Data = Mesh ? const_cast<USkeletalMesh*>(Mesh)->GetAssetUserData<ULilaSoftNetData>() : nullptr;
	if (!Component || !Component->IsRegistered() || !Data || Data->Knots.Num() == 0)
	{
		return Proxy;
	}
	// A leader pose component's bones drive followers.
	const USkinnedMeshComponent* Pose = Component->LeaderPoseComponent.IsValid() ? Component->LeaderPoseComponent.Get() : Component;
	const TArray<FTransform>& Space = Pose->GetComponentSpaceTransforms();

	if (ResolvedFor.Get() != Data || ResolvedRevision != Data->Revision)
	{
		ResolvedFor = Data;
		ResolvedRevision = Data->Revision;
		const FReferenceSkeleton& Ref = Pose->GetSkinnedAsset() ? Pose->GetSkinnedAsset()->GetRefSkeleton() : Mesh->GetRefSkeleton();
		KnotBones.Reset();
		TangentBones.Reset();
		for (const FLilaSoftNetKnot& K : Data->Knots)
		{
			KnotBones.Emplace(Ref.FindBoneIndex(K.Frame), Ref.FindBoneIndex(K.Control));
		}
		for (const FLilaSoftNetTangent& T : Data->Tangents)
		{
			TangentBones.Emplace(Ref.FindBoneIndex(T.Bone), Ref.FindBoneIndex(T.Rest));
		}
		Binding = MakeShared<FLilaSoftNetGpuBinding, ESPMode::ThreadSafe>();
		Binding->Revision = Data->Revision;
		for (const FLilaSoftNetLod& L : Data->Lods)
		{
			Binding->Lods.Add({L.VertexCount, L.Offsets, L.Entries, nullptr, nullptr});
		}
	}

	// The moves, as Lila's dofMoves makes them: a knot's control bone from its frame bone, a handle's bone from its rest bone.
	auto Delta = [&Space](const FIntPoint& Bones) {
		return Space.IsValidIndex(Bones.X) && Space.IsValidIndex(Bones.Y) ? FVector3f(Space[Bones.X].GetLocation() - Space[Bones.Y].GetLocation()) : FVector3f::ZeroVector;
	};
	Proxy->Moves.Reserve(KnotBones.Num() + TangentBones.Num());
	for (const FIntPoint& K : KnotBones)
	{
		Proxy->Moves.Add(FVector4f(Delta(FIntPoint(K.Y, K.X)), 0.0f));
	}
	for (const FIntPoint& T : TangentBones)
	{
		Proxy->Moves.Add(FVector4f(Delta(T), 0.0f));
	}
	Proxy->SceneInterface = Component->GetScene();
	Proxy->ComponentId = Component->GetPrimitiveSceneId();
	Proxy->Binding = Binding;
	return Proxy;
}
