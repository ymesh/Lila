#pragma once

#include "Animation/AnimNodeBase.h"
#include "CoreMinimal.h"
#include "Engine/AssetUserData.h"

#include "LilaMorphPaths.generated.h"

/** One exported morph shape's weight, sampled against its target's weight. */
USTRUCT()
struct LILALINKRUNTIME_API FLilaMorphShapeCurve
{
	GENERATED_BODY()

	UPROPERTY(VisibleAnywhere, Category = "Lila")
	FName Shape;

	/** The shape's weight at each of the path's Weights. */
	UPROPERTY(VisibleAnywhere, Category = "Lila")
	TArray<float> Values;
};

/** A Lila morph target whose curve drives several shapes: in-betweens, a Smooth, Arc or Pivot path, or a response curve. */
USTRUCT()
struct LILALINKRUNTIME_API FLilaMorphPath
{
	GENERATED_BODY()

	/** The curve named after the target, read before any shape writes it. */
	UPROPERTY(VisibleAnywhere, Category = "Lila")
	FName Curve;

	/** Sorted target weights the shapes' values are sampled at; between them values blend straight, past the ends they extend. */
	UPROPERTY(VisibleAnywhere, Category = "Lila")
	TArray<float> Weights;

	UPROPERTY(VisibleAnywhere, Category = "Lila")
	TArray<FLilaMorphShapeCurve> Shapes;

	float Sample(const TArray<float>& Values, float Weight) const;
};

/** Lila's morph paths for a skeletal mesh, kept on the mesh. Written by Lila's unrealMorphPaths; read by the Lila Morph Paths anim node. */
UCLASS()
class LILALINKRUNTIME_API ULilaMorphPathsData : public UAssetUserData
{
	GENERATED_BODY()

public:
	UPROPERTY(VisibleAnywhere, Category = "Lila")
	TArray<FLilaMorphPath> Paths;

	/** Sets every path's shape curves from its target curve. */
	void Apply(FBlendedCurve& Curve) const;
};

/**
 * Turns each Lila morph target's weight curve into the curves of the shapes it exports, as Lila evaluates them. Uses
 * the skeletal mesh's Lila morph paths unless Paths is set.
 */
USTRUCT(BlueprintInternalUseOnly)
struct LILALINKRUNTIME_API FAnimNode_LilaMorphPaths : public FAnimNode_Base
{
	GENERATED_BODY()

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Links")
	FPoseLink Source;

	/** Overrides the paths kept on the skeletal mesh. */
	UPROPERTY(EditAnywhere, Category = "Settings")
	TObjectPtr<ULilaMorphPathsData> Paths;

	virtual void Initialize_AnyThread(const FAnimationInitializeContext& Context) override;
	virtual void CacheBones_AnyThread(const FAnimationCacheBonesContext& Context) override;
	virtual void Update_AnyThread(const FAnimationUpdateContext& Context) override;
	virtual void Evaluate_AnyThread(FPoseContext& Output) override;
	virtual void GatherDebugData(FNodeDebugData& DebugData) override;
	virtual bool NeedsOnInitializeAnimInstance() const override { return true; }
	virtual void OnInitializeAnimInstance(const FAnimInstanceProxy* InProxy, const UAnimInstance* InAnimInstance) override;

private:
	UPROPERTY(Transient)
	TObjectPtr<ULilaMorphPathsData> Resolved;
};
