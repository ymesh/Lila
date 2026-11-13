#pragma once

#include "GameFramework/Actor.h"

#include "LilaCrowdCharacter.generated.h"

class UAnimationAsset;
class USkeletalMesh;
class USkeletalMeshComponent;

/**
 * A Lila crowd character as Mass draws it near the camera: the character's skeletal mesh playing its walk on a loop,
 * turned so it faces the way its agent walks. LilaLink's setMassCrowd makes a Blueprint of it per character.
 */
UCLASS(Blueprintable)
class LILALINKCROWDRUNTIME_API ALilaCrowdCharacter : public AActor
{
	GENERATED_BODY()

public:
	ALilaCrowdCharacter();

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Crowd")
	TObjectPtr<USkeletalMesh> Mesh;

	/** Played on a loop, in place. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Crowd")
	TObjectPtr<UAnimationAsset> Walk;

	/** Turn of the mesh about up, degrees, so the character faces the actor's forward. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Crowd")
	float Yaw = 0.0f;

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Crowd")
	TObjectPtr<USkeletalMeshComponent> Body;

	virtual void OnConstruction(const FTransform& Transform) override;
	virtual void BeginPlay() override;

private:
	void Apply();
};
