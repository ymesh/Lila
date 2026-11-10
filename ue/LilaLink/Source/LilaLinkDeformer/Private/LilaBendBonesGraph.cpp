#include "LilaBendBonesData.h"
#include "LilaBendBonesDataInterface.h"
#include "LilaGraphPins.h"

#include "ProfilingDebugging/CpuProfilerTrace.h"

#if WITH_EDITOR
#include "Animation/MeshDeformerGeometryReadback.h"
#include "AssetRegistry/AssetRegistryModule.h"
#include "Components/SkinnedMeshComponent.h"
#include "Misc/FileHelper.h"
#include "OptimusComponentSource.h"
#include "OptimusDeformer.h"
#include "OptimusDataDomain.h"
#include "OptimusDiagnostic.h"
#include "OptimusExecutionDomain.h"
#include "OptimusNode.h"
#include "OptimusNodeGraph.h"
#include "OptimusNodePin.h"
#include "OptimusShaderText.h"
#include "UObject/Package.h"
#include "UObject/SavePackage.h"
#endif

namespace
{
// The kernel: each vertex where Lila Bend Bones puts it, its tangents turned with it.
const TCHAR* const BendKernel = TEXT(R"(if (Index >= ReadNumThreads().x) return;
const float4 Turn = ReadBentRotation(Index);
const float4 TX = ReadRestTangentX(Index);
const float4 TZ = ReadRestTangentZ(Index);
const float3 TT = 2.0 * cross(Turn.xyz, TX.xyz);
const float3 ZT = 2.0 * cross(Turn.xyz, TZ.xyz);
WritePosition(Index, ReadBentPosition(Index));
WriteTangentX(Index, float4(TX.xyz + Turn.w * TT + cross(Turn.xyz, TT), TX.w));
WriteTangentZ(Index, float4(TZ.xyz + Turn.w * ZT + cross(Turn.xyz, ZT), TZ.w));
)");

} // namespace

UObject* ULilaBendBonesLibrary::MakeBendBonesDeformer(const FString& AssetPath, FString& OutError)
{
	TRACE_CPUPROFILER_EVENT_SCOPE(LilaBendBonesLibrary::MakeBendBonesDeformer);

#if WITH_EDITOR
	if (UOptimusDeformer* Existing = LoadObject<UOptimusDeformer>(nullptr, *AssetPath, nullptr, LOAD_NoWarn | LOAD_Quiet))
	{
		return Existing;
	}
	const FString PackageName = FPackageName::ObjectPathToPackageName(AssetPath);
	UPackage* Package = CreatePackage(*PackageName);
	UOptimusDeformer* Deformer = NewObject<UOptimusDeformer>(Package, FName(*FPackageName::GetShortName(PackageName)), RF_Public | RF_Standalone | RF_Transactional);
	// The editor's factory gives a new deformer its primary binding; nodes with no binding of their own use it.
	Deformer->AddComponentBinding(nullptr, UOptimusComponentSourceBinding::GetPrimaryBindingName());
	UOptimusNodeGraph* Graph = Deformer->GetUpdateGraph();
	UClass* SkinnedMeshClass = FindObject<UClass>(nullptr, TEXT("/Script/OptimusCore.OptimusSkinnedMeshDataInterface"));
	UClass* WriteClass = FindObject<UClass>(nullptr, TEXT("/Script/OptimusCore.OptimusSkinnedMeshWriteDataInterface"));
	UClass* KernelClass = FindObject<UClass>(nullptr, TEXT("/Script/OptimusCore.OptimusNode_CustomComputeKernel"));
	if (!Graph || !SkinnedMeshClass || !WriteClass || !KernelClass)
	{
		OutError = TEXT("this engine's Deformer Graph lacks the skinned mesh interfaces or the custom kernel node");
		return nullptr;
	}
	// Off the origin: a node placed where it already is counts as not placed, and is dropped.
	UOptimusNode* Mesh = Graph->AddDataInterfaceNode(SkinnedMeshClass, FVector2D(0, -50));
	UOptimusNode* Bend = Graph->AddDataInterfaceNode(ULilaBendBonesDataInterface::StaticClass(), FVector2D(0, 300));
	UOptimusNode* Write = Graph->AddDataInterfaceNode(WriteClass, FVector2D(900, 100));
	UOptimusNode* Kernel = Graph->AddNode(KernelClass, FVector2D(450, 100));
	if (!Mesh || !Bend || !Write || !Kernel)
	{
		OutError = FString::Printf(TEXT("couldn't add the graph's nodes (mesh %d, bend bones %d, write %d, kernel %d) to %s"), Mesh != nullptr, Bend != nullptr,
								   Write != nullptr, Kernel != nullptr, *Graph->GetCollectionPath());
		return nullptr;
	}
	const bool bPins = LilaGraphPins::AddKernelPin(Graph, Kernel, LilaGraphPins::PinNamed(Bend, TEXT("BentPosition")), EOptimusNodePinDirection::Input, "BentPosition", OutError) &&
					   LilaGraphPins::AddKernelPin(Graph, Kernel, LilaGraphPins::PinNamed(Bend, TEXT("BentRotation")), EOptimusNodePinDirection::Input, "BentRotation", OutError) &&
					   LilaGraphPins::AddKernelPin(Graph, Kernel, LilaGraphPins::PinNamed(Mesh, TEXT("TangentX")), EOptimusNodePinDirection::Input, "RestTangentX", OutError) &&
					   LilaGraphPins::AddKernelPin(Graph, Kernel, LilaGraphPins::PinNamed(Mesh, TEXT("TangentZ")), EOptimusNodePinDirection::Input, "RestTangentZ", OutError) &&
					   LilaGraphPins::AddKernelPin(Graph, Kernel, LilaGraphPins::PinNamed(Write, TEXT("Position")), EOptimusNodePinDirection::Output, "Position", OutError) &&
					   LilaGraphPins::AddKernelPin(Graph, Kernel, LilaGraphPins::PinNamed(Write, TEXT("TangentX")), EOptimusNodePinDirection::Output, "TangentX", OutError) &&
					   LilaGraphPins::AddKernelPin(Graph, Kernel, LilaGraphPins::PinNamed(Write, TEXT("TangentZ")), EOptimusNodePinDirection::Output, "TangentZ", OutError);
	if (!bPins)
	{
		return nullptr;
	}
	FStructProperty* Source = CastField<FStructProperty>(KernelClass->FindPropertyByName(TEXT("ShaderSource")));
	if (!Source || Source->Struct != FOptimusShaderText::StaticStruct())
	{
		OutError = TEXT("the custom kernel node has no ShaderSource");
		return nullptr;
	}
	Kernel->PreEditChange(Source);
	Source->ContainerPtrToValuePtr<FOptimusShaderText>(Kernel)->ShaderText = BendKernel;
	FPropertyChangedEvent Changed(Source);
	Kernel->PostEditChangeProperty(Changed);
	FStructProperty* Domain = CastField<FStructProperty>(KernelClass->FindPropertyByName(TEXT("ExecutionDomain")));
	if (!Domain || Domain->Struct != FOptimusExecutionDomain::StaticStruct())
	{
		OutError = TEXT("the custom kernel node has no ExecutionDomain");
		return nullptr;
	}
	Kernel->PreEditChange(Domain);
	*Domain->ContainerPtrToValuePtr<FOptimusExecutionDomain>(Kernel) = FOptimusExecutionDomain(Optimus::DomainName::Vertex);
	FPropertyChangedEvent DomainChanged(Domain);
	Kernel->PostEditChangeProperty(DomainChanged);
	TArray<FString> Messages;
	const FDelegateHandle Listen = Deformer->GetCompileMessageDelegate().AddLambda([&Messages](const FOptimusCompilerDiagnostic& D) {
		if (D.Level == EOptimusDiagnosticLevel::Error)
		{
			Messages.Add(D.Message.ToString());
		}
	});
	const bool bCompiled = Deformer->Compile();
	Deformer->GetCompileMessageDelegate().Remove(Listen);
	if (!bCompiled || !Messages.IsEmpty())
	{
		OutError = TEXT("the graph didn't compile: ") + FString::Join(Messages, TEXT("; "));
	}
	FAssetRegistryModule::AssetCreated(Deformer);
	Package->MarkPackageDirty();
	FSavePackageArgs Args;
	Args.TopLevelFlags = RF_Public | RF_Standalone;
	UPackage::SavePackage(Package, Deformer, *FPackageName::LongPackageNameToFilename(PackageName, FPackageName::GetAssetPackageExtension()), Args);
	return Deformer;
#else
	OutError = TEXT("the deformer is made in the editor");
	return nullptr;
#endif
}

bool ULilaBendBonesLibrary::WriteDeformedPositions(USkinnedMeshComponent* Component, const FString& File)
{
#if WITH_EDITORONLY_DATA
	if (!Component)
	{
		return false;
	}
	TUniquePtr<FMeshDeformerGeometryReadbackRequest> Request = MakeUnique<FMeshDeformerGeometryReadbackRequest>();
	Request->VertexDataArraysCallback_AnyThread = [File](const FMeshDeformerGeometryReadbackVertexDataArrays& Data) {
		FString Out = FString::Printf(TEXT("{\"lod\": %d, \"positions\": ["), Data.LODIndex);
		for (int32 I = 0; I < Data.Positions.Num(); ++I)
		{
			Out += FString::Printf(TEXT("%s[%.5f, %.5f, %.5f]"), I ? TEXT(", ") : TEXT(""), Data.Positions[I].X, Data.Positions[I].Y, Data.Positions[I].Z);
		}
		Out += TEXT("]}");
		FFileHelper::SaveStringToFile(Out, *File);
	};
	return Component->RequestReadbackRenderGeometry(MoveTemp(Request));
#else
	return false;
#endif
}
