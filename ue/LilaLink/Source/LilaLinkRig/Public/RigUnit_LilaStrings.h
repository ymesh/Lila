#pragma once

#include "Kismet/BlueprintFunctionLibrary.h"
#include "Units/RigUnit.h"

class URigHierarchy;
class UMovieSceneSection;
class UPoseableMeshComponent;
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

	/** The rule web's contacts hold where they were (feet stay planted). Pins and locks hold either way. */
	UPROPERTY(meta = (Input))
	bool bHoldContacts = true;

	/** 0 leaves the pose, 1 poses all the way. */
	UPROPERTY(meta = (Input, ClampMin = "0.0", ClampMax = "1.0"))
	float Alpha = 1.0f;

	/** Largest distance left between a held contact and where it was, cm. */
	UPROPERTY(meta = (Output))
	float Error = 0.0f;
};

/** A Strings mode, in the order Lila lists them. */
UENUM(BlueprintType)
enum class ELilaStringsMode : uint8
{
	/** The rule web's mode for the bone; without one, moves are IK and turns Forward. */
	Default,
	/** The bone alone: its children keep their world rotation, and on a move their place. */
	Free,
	/** FK, keeping held contacts below the bone outside its own limb (feet stay planted while the hips turn). */
	Forward,
	/** Plain FK: everything below follows, contacts and isolation ignored. */
	ForwardAll UMETA(DisplayName = "Forward All"),
	/** The body swings around the bone while the contacts hold. */
	Backward,
	/** The body swings rigidly around the bone; nothing else holds. */
	BackwardAll UMETA(DisplayName = "Backward All"),
	/** Both sides give: a turn swings the parts below and the body above by half each; a move carries the body half way. */
	Bidirectional,
	/** Bidirectional rigid, nothing held. */
	All,
	/** The bone's limb bends to put it at the target; on the hips, the body moves and the contacts hold. */
	IK UMETA(DisplayName = "IK"),
	/** The body moves to put the bone at the target when its limb can't reach, stiff bones resisting, the contacts holding. */
	FullBody UMETA(DisplayName = "Full Body"),
};

/**
 * Lila Strings: poses the bone in any Strings mode. When Control has animation channels named Mode (an integer, the
 * mode's index) or Isolate Rotation (a bool), they override Mode and bIsolateRotation, so an animator picks and keys the
 * mode per control. The rule web's pins and locked IK chain ends hold as contacts do (always, not only with
 * bHoldContacts), and its glue and attach sets follow the bone; the rig's Pin, Lock and Glue/Attach channels, where the
 * web names them, switch each on and off.
 */
USTRUCT(meta = (DisplayName = "Lila Strings", Keywords = "Strings,FK,IK,FBIK,Full Body,Backward,Bidirectional,Free,Ephemeral,Lila"))
struct LILALINKRIG_API FRigUnit_LilaStrings : public FRigUnit_LilaStringsBase
{
	GENERATED_BODY()

	RIGVM_METHOD()
	virtual void Execute() override;

	UPROPERTY(meta = (Input))
	ELilaStringsMode Mode = ELilaStringsMode::FullBody;

	/** The rule web's isolated bones (head, hands) keep their world rotation in Default, Forward, Backward and Bidirectional. */
	UPROPERTY(meta = (Input))
	bool bIsolateRotation = false;

	/** IK and Full Body: the bone also takes the target's rotation. The other modes always do. */
	UPROPERTY(meta = (Input))
	bool bRotate = true;

	/** IK: bones above this one that bend; 0 its limb from the rule web, or up to the next branch. */
	UPROPERTY(meta = (Input, ClampMin = "0"))
	int32 ChainLength = 0;

	/** Above 1 lets limb bones lengthen, up to this multiple, when turning alone can't meet the holds. */
	UPROPERTY(meta = (Input, ClampMin = "1.0"))
	float MaxStretch = 1.0f;
};

/**
 * Puts a control where an actor in the world is, or a bone of its skinned mesh: how a prop's actor or another character
 * leads what a rule web glues or attaches to them. The control lands at Offset in the source's frame, in the space of the
 * component the rig runs on; nothing happens while the rig runs on no component or no actor has SourceTag.
 */
USTRUCT(meta = (DisplayName = "Lila Strings Source", Category = "Lila|Strings", NodeColor = "0.42 0.2 0.62", Keywords = "Strings,Glue,Attach,Prop,Follow,Lila"))
struct LILALINKRIG_API FRigUnit_LilaStringsSource : public FRigUnitMutable
{
	GENERATED_BODY()

	RIGVM_METHOD()
	virtual void Execute() override;

	UPROPERTY(meta = (Input))
	FName Control;

	/** The tag on the actor that leads. */
	UPROPERTY(meta = (Input))
	FName SourceTag;

	/** A bone (or socket) of the actor's skinned mesh; none follows the actor itself. */
	UPROPERTY(meta = (Input))
	FName Bone;

	UPROPERTY(meta = (Input))
	FTransform Offset = FTransform::Identity;

	UPROPERTY(meta = (Output))
	bool bFound = false;
};

/** Plain FK, as Lila's Forward All: the bone turns to the target's rotation and its children follow. */
USTRUCT(meta = (DisplayName = "Lila Strings Forward", Keywords = "Strings,FK,Ephemeral,Lila", Deprecated = "5.8"))
struct LILALINKRIG_API FRigUnit_LilaStringsForward : public FRigUnit_LilaStringsBase
{
	GENERATED_BODY()

	RIGVM_METHOD()
	virtual void Execute() override;

	RIGVM_METHOD()
	virtual FRigVMStructUpgradeInfo GetUpgradeInfo() const override;
};

/** Backward: the bone turns to the target's rotation, the body swinging around it while the contacts hold. */
USTRUCT(meta = (DisplayName = "Lila Strings Backward", Keywords = "Strings,Reverse,Ephemeral,Lila", Deprecated = "5.8"))
struct LILALINKRIG_API FRigUnit_LilaStringsBackward : public FRigUnit_LilaStringsBase
{
	GENERATED_BODY()

	RIGVM_METHOD()
	virtual void Execute() override;

	RIGVM_METHOD()
	virtual FRigVMStructUpgradeInfo GetUpgradeInfo() const override;
};

/** IK: the bone's limb bends to put it at the target; on the hips, the body moves and the contacts hold. */
USTRUCT(meta = (DisplayName = "Lila Strings IK", Keywords = "Strings,IK,Ephemeral,Lila", Deprecated = "5.8"))
struct LILALINKRIG_API FRigUnit_LilaStringsIK : public FRigUnit_LilaStringsBase
{
	GENERATED_BODY()

	RIGVM_METHOD()
	virtual void Execute() override;

	RIGVM_METHOD()
	virtual FRigVMStructUpgradeInfo GetUpgradeInfo() const override;

	/** Bones above this one that bend; 0 its limb from the rule web, or up to the next branch. */
	UPROPERTY(meta = (Input, ClampMin = "0"))
	int32 ChainLength = 0;

	/** The bone also takes the target's rotation. */
	UPROPERTY(meta = (Input))
	bool bRotate = true;
};

/** Full Body: the body moves to put the bone at the target when its limb can't reach, stiff bones resisting, the contacts holding. */
USTRUCT(meta = (DisplayName = "Lila Strings Full Body", Keywords = "Strings,FBIK,Full Body,Ephemeral,Lila", Deprecated = "5.8"))
struct LILALINKRIG_API FRigUnit_LilaStringsFullBody : public FRigUnit_LilaStringsBase
{
	GENERATED_BODY()

	RIGVM_METHOD()
	virtual void Execute() override;

	RIGVM_METHOD()
	virtual FRigVMStructUpgradeInfo GetUpgradeInfo() const override;

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
	 * Runs a Strings unit's solve on the mesh's reference pose, without a rig: Mode is a Lila mode name (default, free,
	 * forward, forwardAll, backward, backwardAll, bidirectional, all, ik, fullBody), Target the bone's goal (component
	 * space), the rest as the Lila Strings unit takes them. TurnVector, when not zero, is the turn
	 * (axis times radians, component space) the bone's goal rotation takes from its current one, in place of Target's
	 * rotation: script quaternions are single precision. Returns every bone's component space transform after the pose,
	 * with Error the largest hold error left (-1 when it couldn't run). For comparing with Lila.
	 */
	UFUNCTION(BlueprintCallable, Category = "Lila|Strings")
	static TMap<FName, FTransform> SolveOnMesh(USkeletalMesh* Mesh, const FString& RuleWeb, const FString& Mode, FName Bone, const FTransform& Target, bool bRotate,
											  bool bHoldContacts, int32 ChainLength, FVector TurnVector, float& Error,
											  bool bIsolateRotation = false);

	/**
	 * Runs a Control Rig class once with Control at Target (global) and returns every bone's global transform after its
	 * Forwards Solve, to check a rig unrealStringsRig built against Lila. TurnVector, when not zero, turns the control
	 * from where it starts (axis times radians) in place of Target's rotation, as SolveOnMesh. Empty when the class isn't
	 * a Control Rig.
	 */
	/** Puts the control (initial and current) exactly where the bone rests, in double precision; script transforms are single. */
	UFUNCTION(BlueprintCallable, Category = "Lila|Strings")
	static bool PlaceControlAtBone(URigHierarchy* Hierarchy, FName Control, FName Bone);

	/** Replaces a bool control's keys (a Lock channel) on a Control Rig track's section: Frames in the sequence's display rate. */
	UFUNCTION(BlueprintCallable, Category = "Lila|Strings")
	static bool KeyRigBools(UMovieSceneSection* Section, FName Control, const TArray<int32>& Frames, const TArray<bool>& Values);

	/** A bool control's keyed value at each frame (display rate) of a Control Rig track's section. */
	UFUNCTION(BlueprintCallable, Category = "Lila|Strings")
	static TArray<bool> RigBools(UMovieSceneSection* Section, FName Control, const TArray<int32>& Frames);

	/** Puts the control (initial and current) at Location and Rotation, global, in double precision. */
	UFUNCTION(BlueprintCallable, Category = "Lila|Strings")
	static bool PlaceControl(URigHierarchy* Hierarchy, FName Control, FVector Location, FQuat Rotation);

	/** Makes an integer control (a Mode channel) a dropdown of the Strings modes; script can't set ControlEnum itself. */
	UFUNCTION(BlueprintCallable, Category = "Lila|Strings")
	static bool SetModeEnum(URigHierarchy* Hierarchy, FRigElementKey Channel);

	UFUNCTION(BlueprintCallable, Category = "Lila|Strings")
	static TMap<FName, FTransform> DragRig(UClass* RigClass, FName Control, const FTransform& Target, FVector TurnVector);

	/**
	 * DragRig with bool channels set first: each key is "Control/Channel" (LS_hand_l/Pin), its value the channel's. The result
	 * also has the glued props' and other characters' controls (LS_x_...).
	 */
	/** Poses a poseable mesh to bones' component-space transforms (as DragRig returns them), parents first, and refreshes it. */
	UFUNCTION(BlueprintCallable, Category = "Lila|Strings")
	static bool PoseMesh(UPoseableMeshComponent* Mesh, const TMap<FName, FTransform>& ComponentSpace);

	/** DragRig with the rig running on Owner (a component in a world), so its Source nodes find their actors; no Control just evaluates. */
	UFUNCTION(BlueprintCallable, Category = "Lila|Strings")
	static TMap<FName, FTransform> DragRigOn(UClass* RigClass, USceneComponent* Owner, FName Control, const FTransform& Target, FVector TurnVector);

	UFUNCTION(BlueprintCallable, Category = "Lila|Strings")
	static TMap<FName, FTransform> DragRigSwitched(UClass* RigClass, FName Control, const FTransform& Target, FVector TurnVector,
												   const TMap<FString, bool>& Switches);
};
