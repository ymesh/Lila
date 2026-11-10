#include "LilaMorphPaths.h"

#include "Animation/AnimInstance.h"
#include "Animation/AnimInstanceProxy.h"
#include "Components/SkeletalMeshComponent.h"
#include "Engine/SkeletalMesh.h"
#include "ProfilingDebugging/CpuProfilerTrace.h"

#include UE_INLINE_GENERATED_CPP_BY_NAME(LilaMorphPaths)

float FLilaMorphPath::Sample(const TArray<float>& Values, float Weight) const
{
	const int32 Count = FMath::Min(Weights.Num(), Values.Num());
	if (Count == 0)
	{
		return 0.0f;
	}
	if (Count == 1)
	{
		return Values[0];
	}
	int32 Seg = 0;
	while (Seg + 2 < Count && Weight >= Weights[Seg + 1])
	{
		++Seg;
	}
	const float Span = Weights[Seg + 1] - Weights[Seg];
	const float T = FMath::Abs(Span) < UE_SMALL_NUMBER ? 1.0f : (Weight - Weights[Seg]) / Span;
	return Values[Seg] + (Values[Seg + 1] - Values[Seg]) * T;
}

void ULilaMorphPathsData::Apply(FBlendedCurve& Curve) const
{
	TRACE_CPUPROFILER_EVENT_SCOPE(LilaMorphPathsData::Apply);

	for (const FLilaMorphPath& Path : Paths)
	{
		// The shape named like the target writes the curve it reads, so every shape samples the weight first.
		const float Weight = Curve.Get(Path.Curve);
		TArray<float, TInlineAllocator<8>> Out;
		for (const FLilaMorphShapeCurve& Shape : Path.Shapes)
		{
			Out.Add(Path.Sample(Shape.Values, Weight));
		}
		for (int32 Index = 0; Index < Path.Shapes.Num(); ++Index)
		{
			Curve.Set(Path.Shapes[Index].Shape, Out[Index]);
		}
	}
}

void FAnimNode_LilaMorphPaths::OnInitializeAnimInstance(const FAnimInstanceProxy* InProxy, const UAnimInstance* InAnimInstance)
{
	TRACE_CPUPROFILER_EVENT_SCOPE(AnimNode_LilaMorphPaths::OnInitializeAnimInstance);

	Resolved = Paths;
	if (!Resolved && InAnimInstance)
	{
		if (const USkeletalMeshComponent* Component = InAnimInstance->GetSkelMeshComponent())
		{
			if (USkeletalMesh* Mesh = Component->GetSkeletalMeshAsset())
			{
				Resolved = Mesh->GetAssetUserData<ULilaMorphPathsData>();
			}
		}
	}
}

void FAnimNode_LilaMorphPaths::Initialize_AnyThread(const FAnimationInitializeContext& Context)
{
	TRACE_CPUPROFILER_EVENT_SCOPE(AnimNode_LilaMorphPaths::Initialize_AnyThread);

	FAnimNode_Base::Initialize_AnyThread(Context);
	Source.Initialize(Context);
}

void FAnimNode_LilaMorphPaths::CacheBones_AnyThread(const FAnimationCacheBonesContext& Context)
{
	TRACE_CPUPROFILER_EVENT_SCOPE(AnimNode_LilaMorphPaths::CacheBones_AnyThread);

	Source.CacheBones(Context);
}

void FAnimNode_LilaMorphPaths::Update_AnyThread(const FAnimationUpdateContext& Context)
{
	TRACE_CPUPROFILER_EVENT_SCOPE(AnimNode_LilaMorphPaths::Update_AnyThread);

	GetEvaluateGraphExposedInputs().Execute(Context);
	Source.Update(Context);
}

void FAnimNode_LilaMorphPaths::Evaluate_AnyThread(FPoseContext& Output)
{
	TRACE_CPUPROFILER_EVENT_SCOPE(AnimNode_LilaMorphPaths::Evaluate_AnyThread);

	Source.Evaluate(Output);
	if (const ULilaMorphPathsData* Data = Paths ? Paths.Get() : Resolved.Get())
	{
		Data->Apply(Output.Curve);
	}
}

void FAnimNode_LilaMorphPaths::GatherDebugData(FNodeDebugData& DebugData)
{
	const ULilaMorphPathsData* Data = Paths ? Paths.Get() : Resolved.Get();
	DebugData.AddDebugItem(FString::Printf(TEXT("%s (%d paths)"), *DebugData.GetNodeName(this), Data ? Data->Paths.Num() : 0));
	Source.GatherDebugData(DebugData);
}
