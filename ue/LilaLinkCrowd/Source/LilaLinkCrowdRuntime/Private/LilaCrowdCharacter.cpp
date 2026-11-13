#include "LilaCrowdCharacter.h"

#include "Animation/AnimationAsset.h"
#include "Components/SkeletalMeshComponent.h"
#include "Engine/SkeletalMesh.h"
#include "ProfilingDebugging/CpuProfilerTrace.h"

#include UE_INLINE_GENERATED_CPP_BY_NAME(LilaCrowdCharacter)

ALilaCrowdCharacter::ALilaCrowdCharacter()
{
	RootComponent = CreateDefaultSubobject<USceneComponent>(TEXT("Root"));
	Body = CreateDefaultSubobject<USkeletalMeshComponent>(TEXT("Body"));
	Body->SetupAttachment(RootComponent);
	Body->SetCollisionEnabled(ECollisionEnabled::NoCollision);
	Body->SetGenerateOverlapEvents(false);
	Body->VisibilityBasedAnimTickOption = EVisibilityBasedAnimTickOption::OnlyTickPoseWhenRendered;
	PrimaryActorTick.bCanEverTick = false;
}

void ALilaCrowdCharacter::OnConstruction(const FTransform& Transform)
{
	Super::OnConstruction(Transform);
	Apply();
}

void ALilaCrowdCharacter::BeginPlay()
{
	TRACE_CPUPROFILER_EVENT_SCOPE(LilaCrowdCharacter::BeginPlay);

	Super::BeginPlay();
	Apply();
	if (Walk)
	{
		Body->PlayAnimation(Walk, true);
	}
}

void ALilaCrowdCharacter::Apply()
{
	if (Mesh && Body->GetSkeletalMeshAsset() != Mesh)
	{
		Body->SetSkeletalMeshAsset(Mesh);
	}
	Body->SetRelativeRotation(FRotator(0.0, Yaw, 0.0));
}
