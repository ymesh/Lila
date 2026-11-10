#pragma once

#include "Kismet/BlueprintFunctionLibrary.h"
#include "Units/RigUnit.h"

class USkeletalMesh;

#include "RigUnit_LilaStrings.generated.h"

/**
 * Lila Strings in Control Rig: pose a bone the way Lila's Strings modes do, through the character's rule web (its
 * contacts, limbs, lines of action and stiffness, from Lila's stringsRuleWeb). The pose comes from the moment: nothing is
 * kept between evaluations but the bones' transforms. LilaLink's unrealStringsRig builds rigs with these.
 */
USTRUCT(meta = (Abstract, Category = "Lila|Strings", NodeColor = "0.42 0.2 0.62"))
struct LILALINKRIG_API FRigUnit_LilaStringsBase : public FRigUnitMutable
{
	GENERATED_BODY()

	/** The rule web as JSON, as stringsRuleWeb writes it. */
	UPROPERTY(meta = (Input))
	FString RuleWeb;

	/** The bone being posed. */
	UPROPERTY(meta = (Input))
	FName Bone;

	/** A control whose global transform is where the bone goes; none uses Target. */
	UPROPERTY(meta = (Input))
	FName Control;

	/** Where the bone goes, global, when no Control is given. */
	UPROPERTY(meta = (Input))
	FTransform Target = FTransform::Identity;

	/** The rule web's contacts hold where they were (feet stay planted). */
	UPROPERTY(meta = (Input))
	bool bHoldContacts = true;

	/** 0 leaves the pose, 1 poses all the way. */
	UPROPERTY(meta = (Input, ClampMin = "0.0", ClampMax = "1.0"))
	float Alpha = 1.0f;

	/** Largest distance left between a held contact and where it was, cm. */
	UPROPERTY(meta = (Output))
	float Error = 0.0f;
};

/** Forward: the bone turns to the target's rotation and its children follow. */
USTRUCT(meta = (DisplayName = "Lila Strings Forward", Keywords = "Strings,FK,Ephemeral,Lila"))
struct LILALINKRIG_API FRigUnit_LilaStringsForward : public FRigUnit_LilaStringsBase
{
	GENERATED_BODY()

	RIGVM_METHOD()
	virtual void Execute() override;
};

/** Backward: the bone turns to the target's rotation, the body swinging around it while the contacts hold. */
USTRUCT(meta = (DisplayName = "Lila Strings Backward", Keywords = "Strings,Reverse,Ephemeral,Lila"))
struct LILALINKRIG_API FRigUnit_LilaStringsBackward : public FRigUnit_LilaStringsBase
{
	GENERATED_BODY()

	RIGVM_METHOD()
	virtual void Execute() override;
};

/** IK: the bone's limb bends to put it at the target; on the hips, the body moves and the contacts hold. */
USTRUCT(meta = (DisplayName = "Lila Strings IK", Keywords = "Strings,IK,Ephemeral,Lila"))
struct LILALINKRIG_API FRigUnit_LilaStringsIK : public FRigUnit_LilaStringsBase
{
	GENERATED_BODY()

	RIGVM_METHOD()
	virtual void Execute() override;

	/** Bones above this one that bend; 0 its limb from the rule web, or up to the next branch. */
	UPROPERTY(meta = (Input, ClampMin = "0"))
	int32 ChainLength = 0;

	/** The bone also takes the target's rotation. */
	UPROPERTY(meta = (Input))
	bool bRotate = true;
};

/** Full Body: the body moves to put the bone at the target when its limb can't reach, stiff bones resisting, the contacts holding. */
USTRUCT(meta = (DisplayName = "Lila Strings Full Body", Keywords = "Strings,FBIK,Full Body,Ephemeral,Lila"))
struct LILALINKRIG_API FRigUnit_LilaStringsFullBody : public FRigUnit_LilaStringsBase
{
	GENERATED_BODY()

	RIGVM_METHOD()
	virtual void Execute() override;

	/** The bone also takes the target's rotation. */
	UPROPERTY(meta = (Input))
	bool bRotate = true;
};

UCLASS()
class LILALINKRIG_API ULilaStringsLibrary : public UBlueprintFunctionLibrary
{
	GENERATED_BODY()

public:
	/**
	 * Runs a Strings unit's solve on the mesh's reference pose, without a rig: Mode is forward, backward, ik or fullBody,
	 * Target the bone's goal (component space), the rest as the units take them. Returns every bone's component space
	 * transform after the pose, with Error the largest hold error left (-1 when it couldn't run). For comparing with Lila.
	 */
	UFUNCTION(BlueprintCallable, Category = "Lila|Strings")
	static TMap<FName, FTransform> SolveOnMesh(USkeletalMesh* Mesh, const FString& RuleWeb, const FString& Mode, FName Bone, const FTransform& Target, bool bRotate,
											  bool bHoldContacts, int32 ChainLength, float& Error);

	/**
	 * Runs a Control Rig class once with Control at Target (global) and returns every bone's global transform after its
	 * Forwards Solve, to check a rig unrealStringsRig built against Lila. Empty when the class isn't a Control Rig.
	 */
	UFUNCTION(BlueprintCallable, Category = "Lila|Strings")
	static TMap<FName, FTransform> DragRig(UClass* RigClass, FName Control, const FTransform& Target);
};
