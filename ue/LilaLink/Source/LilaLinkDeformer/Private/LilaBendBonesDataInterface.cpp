#include "LilaBendBonesDataInterface.h"

#include "LilaBendBonesData.h"
#include "LilaBendBonesMath.h"

#include "Components/SkinnedMeshComponent.h"
#include "ComputeFramework/ShaderParamTypeDefinition.h"
#include "Engine/SkeletalMesh.h"
#include "Engine/SkinnedAsset.h"
#include "OptimusDataDomain.h"
#include "ProfilingDebugging/CpuProfilerTrace.h"
#include "RenderGraphBuilder.h"
#include "RenderingThread.h"
#include "RenderGraphUtils.h"
#include "Rendering/SkeletalMeshLODRenderData.h"
#include "Rendering/SkeletalMeshRenderData.h"
#include "ShaderParameterMetadataBuilder.h"
#include "SkeletalMeshDeformerHelpers.h"
#include "SkeletalRenderPublic.h"

#include UE_INLINE_GENERATED_CPP_BY_NAME(LilaBendBonesDataInterface)

namespace
{
BEGIN_SHADER_PARAMETER_STRUCT(FLilaBendBonesParameters, )
	SHADER_PARAMETER(uint32, NumVertices)
	SHADER_PARAMETER(float, Volume)
	SHADER_PARAMETER(float, EvenBend)
	SHADER_PARAMETER_RDG_BUFFER_SRV(StructuredBuffer<float4>, Entries)
	SHADER_PARAMETER_RDG_BUFFER_SRV(StructuredBuffer<float4>, CurvePoint)
	SHADER_PARAMETER_RDG_BUFFER_SRV(StructuredBuffer<float4>, CurveFrame)
	SHADER_PARAMETER_RDG_BUFFER_SRV(StructuredBuffer<uint4>, ChainInfo)
	SHADER_PARAMETER_RDG_BUFFER_SRV(StructuredBuffer<float4>, Knot)
END_SHADER_PARAMETER_STRUCT()

// Lila src/deform/BendBones.cpp deformPoint, per vertex over its (up to two) chains. Each chain's posed curve comes as
// samples: CurvePoint (xyz, arc length), CurveFrame (quaternion: X tangent, Y normal, Z binormal). ChainInfo per chain:
// first sample, sample count, first knot. Knot per joint: (sample index as uint bits, rest arc length there).
const TCHAR* const KernelHlsl = TEXT(R"(
uint {DataInterfaceName}_NumVertices;
float {DataInterfaceName}_Volume;
float {DataInterfaceName}_EvenBend;
StructuredBuffer<float4> {DataInterfaceName}_Entries;
StructuredBuffer<float4> {DataInterfaceName}_CurvePoint;
StructuredBuffer<float4> {DataInterfaceName}_CurveFrame;
StructuredBuffer<uint4> {DataInterfaceName}_ChainInfo;
StructuredBuffer<float4> {DataInterfaceName}_Knot;

float3 Rotate_{DataInterfaceName}(float4 Q, float3 V)
{
	const float3 T = 2.0 * cross(Q.xyz, V);
	return V + Q.w * T + cross(Q.xyz, T);
}

float4 QuatMul_{DataInterfaceName}(float4 A, float4 B)
{
	return float4(A.w * B.xyz + B.w * A.xyz + cross(A.xyz, B.xyz), A.w * B.w - dot(A.xyz, B.xyz));
}

void SampleAt_{DataInterfaceName}(uint Start, uint Count, float S, out uint I, out float F)
{
	uint Lo = Start, Hi = Start + Count - 2;
	while (Lo < Hi)
	{
		const uint Mid = (Lo + Hi + 1) / 2;
		if ({DataInterfaceName}_CurvePoint[Mid].w <= S) Lo = Mid; else Hi = Mid - 1;
	}
	I = Lo;
	const float Span = {DataInterfaceName}_CurvePoint[I + 1].w - {DataInterfaceName}_CurvePoint[I].w;
	F = Span > 1e-6 ? saturate((S - {DataInterfaceName}_CurvePoint[I].w) / Span) : 0.0;
}

float3 PointAt_{DataInterfaceName}(uint Start, uint Count, float S)
{
	uint I; float F;
	SampleAt_{DataInterfaceName}(Start, Count, S, I, F);
	return lerp({DataInterfaceName}_CurvePoint[I].xyz, {DataInterfaceName}_CurvePoint[I + 1].xyz, F);
}

float4 FrameAt_{DataInterfaceName}(uint Start, uint Count, float S)
{
	uint I; float F;
	SampleAt_{DataInterfaceName}(Start, Count, S, I, F);
	float4 A = {DataInterfaceName}_CurveFrame[I], B = {DataInterfaceName}_CurveFrame[I + 1];
	if (dot(A, B) < 0.0) B = -B;
	return normalize(lerp(A, B, F));
}

float3 StrandPoint_{DataInterfaceName}(uint I, float2 Across)
{
	return {DataInterfaceName}_CurvePoint[I].xyz + Rotate_{DataInterfaceName}({DataInterfaceName}_CurveFrame[I], float3(0, Across.x, Across.y));
}

float StrandLength_{DataInterfaceName}(uint From, uint To, float2 Across, float Fraction)
{
	float Total = 0.0;
	float3 Prev = StrandPoint_{DataInterfaceName}(From, Across);
	for (uint I = From + 1; I <= To; ++I)
	{
		const float3 Q = StrandPoint_{DataInterfaceName}(I, Across);
		Total += length(Q - Prev);
		Prev = Q;
	}
	const float Want = saturate(Fraction) * Total;
	float Run = 0.0;
	Prev = StrandPoint_{DataInterfaceName}(From, Across);
	for (uint J = From + 1; J <= To; ++J)
	{
		const float3 Q = StrandPoint_{DataInterfaceName}(J, Across);
		const float D = length(Q - Prev);
		if (Run + D >= Want)
		{
			return lerp({DataInterfaceName}_CurvePoint[J - 1].w, {DataInterfaceName}_CurvePoint[J].w, D > 1e-6 ? (Want - Run) / D : 0.0);
		}
		Run += D;
		Prev = Q;
	}
	return {DataInterfaceName}_CurvePoint[To].w;
}

// One entry: where the vertex goes on its chain, and the turn from its rest frame to the posed frame there.
void Bend_{DataInterfaceName}(uint E, out float3 Position, out float4 Turn)
{
	const float4 A = {DataInterfaceName}_Entries[E];
	const float4 B = {DataInterfaceName}_Entries[E + 1];
	const float4 RestFrame = {DataInterfaceName}_Entries[E + 2];
	const uint4 Chain = {DataInterfaceName}_ChainInfo[asuint(A.x)];
	const uint Segment = asuint(A.y);
	const uint Start = Chain.x, Count = Chain.y;
	const float4 KnotA = {DataInterfaceName}_Knot[Chain.z + Segment], KnotB = {DataInterfaceName}_Knot[Chain.z + Segment + 1];
	const uint From = Start + asuint(KnotA.x), To = Start + asuint(KnotB.x);
	const float Pa = {DataInterfaceName}_CurvePoint[From].w, Pb = {DataInterfaceName}_CurvePoint[To].w;
	const float RestSpan = KnotB.y - KnotA.y;
	const float Stretch = RestSpan > 1e-6 ? (Pb - Pa) / RestSpan : 1.0;
	const float Thin = lerp(1.0, rsqrt(max(Stretch, 1e-3)), saturate({DataInterfaceName}_Volume));
	const float3 Offset = B.xyz;
	const float2 Across = Offset.yz * Thin;

	float S = Pa + A.w * (Pb - Pa);
	if ({DataInterfaceName}_EvenBend > 0.0 && length(Across) > 1e-6)
	{
		S = lerp(S, StrandLength_{DataInterfaceName}(From, To, Across, B.w), saturate({DataInterfaceName}_EvenBend));
	}
	const float4 F = FrameAt_{DataInterfaceName}(Start, Count, S);
	const float3 Tangent = Rotate_{DataInterfaceName}(F, float3(1, 0, 0));
	float3 D = Rotate_{DataInterfaceName}(F, float3(0, Across.x, Across.y));
	// Held short of the bend's centre: an offset reaching past it would fold the inside through itself.
	const float H = max(1e-3, ({DataInterfaceName}_CurvePoint[Start + Count - 1].w - {DataInterfaceName}_CurvePoint[Start].w) / float(Count - 1));
	const float3 Ahead = Rotate_{DataInterfaceName}(FrameAt_{DataInterfaceName}(Start, Count, S + H), float3(1, 0, 0));
	const float3 Behind = Rotate_{DataInterfaceName}(FrameAt_{DataInterfaceName}(Start, Count, S - H), float3(1, 0, 0));
	const float3 BendRate = (Ahead - Behind) / (2.0 * H);
	const float Curvature = length(BendRate);
	if (Curvature > 1e-6)
	{
		const float3 ToCentre = BendRate / Curvature;
		const float Reach = dot(D, ToCentre), Limit = 0.85 / Curvature;
		if (Reach > Limit) D -= (Reach - Limit) * ToCentre;
	}
	Position = PointAt_{DataInterfaceName}(Start, Count, S) + Tangent * Offset.x + D;
	Turn = QuatMul_{DataInterfaceName}(F, float4(-RestFrame.xyz, RestFrame.w));
}

uint ReadNumVertices_{DataInterfaceName}()
{
	return {DataInterfaceName}_NumVertices;
}

void BendVertex_{DataInterfaceName}(uint VertexIndex, out float3 Position, out float4 Turn)
{
	Position = 0;
	Turn = float4(0, 0, 0, 0);
	float Total = 0.0;
	for (uint K = 0; K < 2; ++K)
	{
		const uint E = (VertexIndex * 2 + K) * 3;
		const float4 A = {DataInterfaceName}_Entries[E];
		if (asuint(A.x) == 0xffffffff || A.z <= 0.0) continue;
		float3 P; float4 Q;
		Bend_{DataInterfaceName}(E, P, Q);
		if (dot(Turn, Q) < 0.0) Q = -Q;
		Position += P * A.z;
		Turn += Q * A.z;
		Total += A.z;
	}
	Position /= max(Total, 1e-6);
	Turn = Total > 0.0 ? normalize(Turn) : float4(0, 0, 0, 1);
}

float3 ReadBentPosition_{DataInterfaceName}(uint VertexIndex)
{
	float3 P; float4 Q;
	BendVertex_{DataInterfaceName}(VertexIndex, P, Q);
	return P;
}

float4 ReadBentRotation_{DataInterfaceName}(uint VertexIndex)
{
	float3 P; float4 Q;
	BendVertex_{DataInterfaceName}(VertexIndex, P, Q);
	return Q;
}
)");

class FLilaBendBonesProxy final : public FComputeDataProviderRenderProxy
{
public:
	using FParameters = FLilaBendBonesParameters;

	FSceneInterface* SceneInterface = nullptr;
	FPrimitiveComponentId ComponentId;
	/** The LOD being deformed, chosen in IsValid from the one the mesh renders. */
	mutable int32 Lod = 0;
	float Volume = 1.0f;
	float EvenBend = 1.0f;
	TSharedPtr<FLilaBendBonesGpuBinding, ESPMode::ThreadSafe> Binding;
	TArray<FVector4f> CurvePoint, CurveFrame, Knot;
	TArray<FUintVector4> ChainInfo;

	FRDGBufferSRVRef EntriesSRV = nullptr, PointSRV = nullptr, FrameSRV = nullptr, ChainSRV = nullptr, KnotSRV = nullptr;

	bool IsValid(FValidationData const& InValidationData) const override
	{
		if (InValidationData.ParameterStructSize != sizeof(FParameters) || !Binding || ChainInfo.Num() == 0)
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
		return Binding->Lods.IsValidIndex(Lod) && RenderLods.IsValidIndex(Lod) && int32(RenderLods[Lod].GetNumVertices()) == Binding->Lods[Lod].VertexCount;
	}

	void AllocateResources(FRDGBuilder& GraphBuilder, FAllocationData const& InAllocationData) override
	{
		TRACE_CPUPROFILER_EVENT_SCOPE(LilaBendBonesProxy::AllocateResources);

		FLilaBendBonesGpuLod& Bound = Binding->Lods[Lod];
		FRDGBufferRef Entries;
		if (Bound.Pooled.IsValid())
		{
			Entries = GraphBuilder.RegisterExternalBuffer(Bound.Pooled);
		}
		else
		{
			Entries = CreateStructuredBuffer(GraphBuilder, TEXT("LilaBendBones.Entries"), Bound.Entries);
			Bound.Pooled = GraphBuilder.ConvertToExternalBuffer(Entries);
		}
		EntriesSRV = GraphBuilder.CreateSRV(Entries);
		PointSRV = GraphBuilder.CreateSRV(CreateStructuredBuffer(GraphBuilder, TEXT("LilaBendBones.CurvePoint"), CurvePoint));
		FrameSRV = GraphBuilder.CreateSRV(CreateStructuredBuffer(GraphBuilder, TEXT("LilaBendBones.CurveFrame"), CurveFrame));
		ChainSRV = GraphBuilder.CreateSRV(CreateStructuredBuffer(GraphBuilder, TEXT("LilaBendBones.ChainInfo"), ChainInfo));
		KnotSRV = GraphBuilder.CreateSRV(CreateStructuredBuffer(GraphBuilder, TEXT("LilaBendBones.Knot"), Knot));
	}

	void GatherDispatchData(FDispatchData const& InDispatchData) override
	{
		const TStridedView<FParameters> ParameterArray = MakeStridedParameterView<FParameters>(InDispatchData);
		for (int32 I = 0; I < ParameterArray.Num(); ++I)
		{
			FParameters& P = ParameterArray[I];
			P.NumVertices = uint32(Binding->Lods[Lod].VertexCount);
			P.Volume = Volume;
			P.EvenBend = EvenBend;
			P.Entries = EntriesSRV;
			P.CurvePoint = PointSRV;
			P.CurveFrame = FrameSRV;
			P.ChainInfo = ChainSRV;
			P.Knot = KnotSRV;
		}
	}
};
} // namespace

FString ULilaBendBonesDataInterface::GetDisplayName() const
{
	return TEXT("Lila Bend Bones");
}

TArray<FOptimusCDIPinDefinition> ULilaBendBonesDataInterface::GetPinDefinitions() const
{
	TArray<FOptimusCDIPinDefinition> Defs;
	// The bent pins change every frame with the bones; the vertex count is fixed.
	Defs.Add({"NumVertices", "ReadNumVertices", false});
	Defs.Add({"BentPosition", "ReadBentPosition", Optimus::DomainName::Vertex, "ReadNumVertices"});
	Defs.Add({"BentRotation", "ReadBentRotation", Optimus::DomainName::Vertex, "ReadNumVertices"});
	return Defs;
}

TSubclassOf<UActorComponent> ULilaBendBonesDataInterface::GetRequiredComponentClass() const
{
	return USkinnedMeshComponent::StaticClass();
}

void ULilaBendBonesDataInterface::GetSupportedInputs(TArray<FShaderFunctionDefinition>& OutFunctions) const
{
	OutFunctions.AddDefaulted_GetRef().SetName(TEXT("ReadNumVertices")).AddReturnType(EShaderFundamentalType::Uint);
	OutFunctions.AddDefaulted_GetRef().SetName(TEXT("ReadBentPosition")).AddReturnType(EShaderFundamentalType::Float, 3).AddParam(EShaderFundamentalType::Uint);
	OutFunctions.AddDefaulted_GetRef().SetName(TEXT("ReadBentRotation")).AddReturnType(EShaderFundamentalType::Float, 4).AddParam(EShaderFundamentalType::Uint);
}

void ULilaBendBonesDataInterface::GetShaderParameters(TCHAR const* UID, FShaderParametersMetadataBuilder& InOutBuilder, FShaderParametersMetadataAllocations& InOutAllocations) const
{
	InOutBuilder.AddNestedStruct<FLilaBendBonesParameters>(UID);
}

void ULilaBendBonesDataInterface::GetShaderHash(FString& InOutKey) const
{
	InOutKey += FString::Printf(TEXT("LilaBendBones%08x"), GetTypeHash(FString(KernelHlsl)));
}

void ULilaBendBonesDataInterface::GetHLSL(FString& OutHLSL, FString const& InDataInterfaceName) const
{
	const TMap<FString, FStringFormatArg> Args = {{TEXT("DataInterfaceName"), InDataInterfaceName}};
	OutHLSL += FString::Format(KernelHlsl, Args);
}

UComputeDataProvider* ULilaBendBonesDataInterface::CreateDataProvider(TObjectPtr<UObject> InBinding, uint64 InInputMask, uint64 InOutputMask) const
{
	ULilaBendBonesDataProvider* Provider = NewObject<ULilaBendBonesDataProvider>();
	Provider->SkinnedMesh = Cast<USkinnedMeshComponent>(InBinding);
	return Provider;
}

void ULilaBendBonesDataProvider::BeginDestroy()
{
	if (Binding)
	{
		ENQUEUE_RENDER_COMMAND(LilaBendBonesRelease)([Released = MoveTemp(Binding)](FRHICommandListImmediate&) mutable { Released.Reset(); });
	}
	Super::BeginDestroy();
}

FComputeDataProviderRenderProxy* ULilaBendBonesDataProvider::GetRenderProxy()
{
	TRACE_CPUPROFILER_EVENT_SCOPE(LilaBendBonesDataProvider::GetRenderProxy);

	FLilaBendBonesProxy* Proxy = new FLilaBendBonesProxy();
	USkinnedMeshComponent* Component = SkinnedMesh.Get();
	const USkeletalMesh* Mesh = Component ? Cast<USkeletalMesh>(Component->GetSkinnedAsset()) : nullptr;
	const ULilaBendBonesData* Data = Mesh ? const_cast<USkeletalMesh*>(Mesh)->GetAssetUserData<ULilaBendBonesData>() : nullptr;
	if (!Component || !Component->IsRegistered() || !Data || Data->Chains.Num() == 0)
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
		BoneIndices.Reset();
		CvDrivers.Reset();
		const FReferenceSkeleton& Ref = Pose->GetSkinnedAsset() ? Pose->GetSkinnedAsset()->GetRefSkeleton() : Mesh->GetRefSkeleton();
		for (const FLilaBendBonesChain& Chain : Data->Chains)
		{
			TArray<int32>& Out = BoneIndices.AddDefaulted_GetRef();
			for (const FName& Bone : Chain.Bones)
			{
				Out.Add(Ref.FindBoneIndex(Bone));
			}
			TArray<int32>& Drivers = CvDrivers.AddDefaulted_GetRef();
			for (const FLilaBendBonesCv& Cv : Chain.Cvs)
			{
				Drivers.Add(Cv.Driver.IsNone() ? INDEX_NONE : Ref.FindBoneIndex(Cv.Driver));
			}
		}
		Binding = MakeShared<FLilaBendBonesGpuBinding, ESPMode::ThreadSafe>();
		Binding->Revision = Data->Revision;
		for (const FLilaBendBonesLod& L : Data->Lods)
		{
			Binding->Lods.Add({L.VertexCount, L.Entries, nullptr});
		}
	}

	Proxy->SceneInterface = Component->GetScene();
	Proxy->ComponentId = Component->GetPrimitiveSceneId();
	Proxy->Volume = Data->Volume;
	Proxy->EvenBend = Data->EvenBend;
	Proxy->Binding = Binding;
	for (int32 C = 0; C < Data->Chains.Num(); ++C)
	{
		LilaBendBones::FChain Chain;
		for (const int32 Bone : BoneIndices[C])
		{
			if (!Space.IsValidIndex(Bone))
			{
				Proxy->ChainInfo.Reset();
				return Proxy;
			}
			Chain.Position.Add(Space[Bone].GetLocation());
			Chain.Rotation.Add(Space[Bone].GetRotation());
		}
		TArray<TPair<int32, FVector>> Cvs;
		const TArray<FLilaBendBonesCv>& ChainCvs = Data->Chains[C].Cvs;
		for (int32 V = 0; V < ChainCvs.Num(); ++V)
		{
			const int32 Driver = CvDrivers[C].IsValidIndex(V) ? CvDrivers[C][V] : INDEX_NONE;
			const int32 Joint = BoneIndices[C].IsValidIndex(ChainCvs[V].Bone) ? BoneIndices[C][ChainCvs[V].Bone] : INDEX_NONE;
			if (Space.IsValidIndex(Driver))
			{
				Cvs.Emplace(ChainCvs[V].Bone, Space[Driver].GetLocation());
			}
			else if (Space.IsValidIndex(Joint))
			{
				Cvs.Emplace(ChainCvs[V].Bone, Space[Joint].TransformPosition(ChainCvs[V].RestLocal));
			}
		}
		LilaBendBones::PlaceCvs(Chain, Cvs);
		const LilaBendBones::FCurve Curve = LilaBendBones::Build(Chain, Data->Chains[C].RestNormals, Data->SamplesPerSpan);
		Proxy->ChainInfo.Add(FUintVector4(uint32(Proxy->CurvePoint.Num()), uint32(Curve.Point.Num()), uint32(Proxy->Knot.Num()), 0));
		for (int32 I = 0; I < Curve.Point.Num(); ++I)
		{
			Proxy->CurvePoint.Add(FVector4f(FVector3f(Curve.Point[I]), float(Curve.Length[I])));
			const FQuat& Q = Curve.Frame[I];
			Proxy->CurveFrame.Add(FVector4f(float(Q.X), float(Q.Y), float(Q.Z), float(Q.W)));
		}
		for (int32 K = 0; K < Curve.Knot.Num(); ++K)
		{
			const double RestLength = Data->Chains[C].RestKnotLength.IsValidIndex(K) ? Data->Chains[C].RestKnotLength[K] : Curve.Length[Curve.Knot[K]];
			Proxy->Knot.Add(FVector4f(FMath::AsFloat(uint32(Curve.Knot[K])), float(RestLength), 0.0f, 0.0f));
		}
	}
	return Proxy;
}
