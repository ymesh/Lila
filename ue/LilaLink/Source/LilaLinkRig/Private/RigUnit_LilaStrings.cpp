#include "RigUnit_LilaStrings.h"

#include "LilaStringsSolver.h"

#include "ControlRig.h"
#include "Dom/JsonObject.h"
#include "Engine/SkeletalMesh.h"
#include "Misc/ScopeLock.h"
#include "ProfilingDebugging/CpuProfilerTrace.h"
#include "Rigs/RigHierarchy.h"
#include "Rigs/RigHierarchyController.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"
#include "Units/Execution/RigUnit_BeginExecution.h"
#include "Units/RigUnitContext.h"

#include UE_INLINE_GENERATED_CPP_BY_NAME(RigUnit_LilaStrings)

namespace
{
using namespace LilaStrings;

/** The rule web by bone name, as Lila's stringsRuleWeb writes it: the bones as Strings solves them, parents first. */
struct FRuleWeb
{
	FString Source;
	FName Hips;
	TArray<FName> Bones;
	TArray<FName> Parents;
	TArray<bool> Solve;
	TArray<double> Stiffness;
	TArray<FLimit> Limits;
	TArray<FName> Contacts;
	TArray<TArray<FName>> Chains;
	double LengthScale = 0.0;
	FString Error;
};

TArray<FName> ReadNames(const TArray<TSharedPtr<FJsonValue>>& Values)
{
	TArray<FName> Out;
	for (const TSharedPtr<FJsonValue>& V : Values)
	{
		Out.Add(FName(*V->AsString()));
	}
	return Out;
}

FVector ReadVector3(const TSharedPtr<FJsonObject>& Obj, const TCHAR* Field, const FVector& Fallback)
{
	const TArray<TSharedPtr<FJsonValue>>* V = nullptr;
	return Obj->TryGetArrayField(Field, V) && V->Num() == 3 ? FVector((*V)[0]->AsNumber(), (*V)[1]->AsNumber(), (*V)[2]->AsNumber()) : Fallback;
}

/** A bone's limit as stringsRuleWeb writes it, already in Unreal's local-rotation space. */
FLimit ReadLimit(const TSharedPtr<FJsonObject>& Obj)
{
	FLimit L;
	const FString Kind = Obj->GetStringField(TEXT("kind"));
	L.Kind = Kind == TEXT("hinge") ? FLimit::EKind::Hinge : Kind == TEXT("cone") ? FLimit::EKind::Cone : FLimit::EKind::None;
	L.Axis = ReadVector3(Obj, TEXT("axis"), FVector::ZAxisVector);
	L.BoneDir = ReadVector3(Obj, TEXT("bone"), FVector::XAxisVector);
	Obj->TryGetNumberField(TEXT("min"), L.Min);
	Obj->TryGetNumberField(TEXT("max"), L.Max);
	Obj->TryGetNumberField(TEXT("cone"), L.Cone);
	const TArray<TSharedPtr<FJsonValue>>* Post = nullptr;
	if (Obj->TryGetArrayField(TEXT("post"), Post) && Post->Num() == 4)
	{
		L.Post = FQuat((*Post)[0]->AsNumber(), (*Post)[1]->AsNumber(), (*Post)[2]->AsNumber(), (*Post)[3]->AsNumber()).GetNormalized();
	}
	return L;
}

TSharedPtr<FRuleWeb> Parse(const FString& Text)
{
	TRACE_CPUPROFILER_EVENT_SCOPE(LilaStrings::Parse);

	TSharedPtr<FRuleWeb> Web = MakeShared<FRuleWeb>();
	Web->Source = Text;
	TSharedPtr<FJsonObject> Root;
	if (!FJsonSerializer::Deserialize(TJsonReaderFactory<>::Create(Text), Root) || !Root.IsValid())
	{
		Web->Error = TEXT("the rule web isn't JSON; make it with Lila's stringsRuleWeb");
		return Web;
	}
	Web->Hips = FName(*Root->GetStringField(TEXT("hips")));
	const TSharedPtr<FJsonObject>* Stiff = nullptr;
	Root->TryGetObjectField(TEXT("stiffness"), Stiff);
	const TSharedPtr<FJsonObject>* LimitsOf = nullptr;
	Root->TryGetObjectField(TEXT("limits"), LimitsOf);
	const TArray<TSharedPtr<FJsonValue>>* Values = nullptr;
	if (Root->TryGetArrayField(TEXT("bones"), Values))
	{
		for (const TSharedPtr<FJsonValue>& B : *Values)
		{
			const TSharedPtr<FJsonObject> Obj = B->AsObject();
			const FString Name = Obj->GetStringField(TEXT("name"));
			const FString ParentName = Obj->GetStringField(TEXT("parent"));
			bool bSolve = true;
			Obj->TryGetBoolField(TEXT("solve"), bSolve);
			double Stiffness = 0.0;
			if (Stiff)
			{
				(*Stiff)->TryGetNumberField(Name, Stiffness);
			}
			Web->Bones.Add(FName(*Name));
			Web->Parents.Add(ParentName.IsEmpty() ? NAME_None : FName(*ParentName));
			Web->Solve.Add(bSolve);
			Web->Stiffness.Add(Stiffness);
			const TSharedPtr<FJsonObject>* Limit = nullptr;
			Web->Limits.Add(LimitsOf && (*LimitsOf)->TryGetObjectField(Name, Limit) ? ReadLimit(*Limit) : FLimit());
		}
	}
	if (Root->TryGetArrayField(TEXT("contacts"), Values))
	{
		Web->Contacts = ReadNames(*Values);
	}
	if (Root->TryGetArrayField(TEXT("chains"), Values))
	{
		for (const TSharedPtr<FJsonValue>& C : *Values)
		{
			Web->Chains.Add(ReadNames(C->AsArray()));
		}
	}
	Root->TryGetNumberField(TEXT("lengthScale"), Web->LengthScale);
	return Web;
}

/** Parsed once per distinct web, shared by every unit and instance using it. */
TSharedPtr<FRuleWeb> Cached(const FString& Text)
{
	static FCriticalSection Lock;
	static TMap<uint32, TSharedPtr<FRuleWeb>> Cache;
	const uint32 Key = GetTypeHash(Text);
	FScopeLock Guard(&Lock);
	if (TSharedPtr<FRuleWeb>* Found = Cache.Find(Key); Found && (*Found)->Source == Text)
	{
		return *Found;
	}
	TSharedPtr<FRuleWeb> Web = Parse(Text);
	Cache.Add(Key, Web);
	return Web;
}

FRigElementKey BoneKey(const FName Name) { return FRigElementKey(Name, ERigElementType::Bone); }

/** The skeleton as the web's problem, from the hierarchy's current pose; false with Missing set when a bone isn't there. */
bool ProblemOf(URigHierarchy* H, const FRuleWeb& W, FProblem& Out, FRules& Rules, FName& Missing)
{
	TRACE_CPUPROFILER_EVENT_SCOPE(LilaStrings::ProblemOf);

	TMap<FName, int32> Index;
	FVector Lo(TNumericLimits<double>::Max()), Hi(-TNumericLimits<double>::Max());
	for (int32 I = 0; I < W.Bones.Num(); ++I)
	{
		const FRigElementKey Key = BoneKey(W.Bones[I]);
		if (!H->Contains(Key))
		{
			Missing = W.Bones[I];
			return false;
		}
		const FTransform Local = H->GetLocalTransform(Key);
		const FTransform Global = H->GetGlobalTransform(Key);
		FBone& B = Out.Bones.AddDefaulted_GetRef();
		const int32* Parent = W.Parents[I].IsNone() ? nullptr : Index.Find(W.Parents[I]);
		B.Parent = Parent ? *Parent : INDEX_NONE;
		if (B.Parent == INDEX_NONE)
		{
			// Global is Local then the parent's global, so the parent outside the solve is Local^-1 then Global.
			B.ParentWorld = Local.ToMatrixWithScale().Inverse() * Global.ToMatrixWithScale();
		}
		B.Before = FTranslationMatrix(Local.GetTranslation());
		B.After = FScaleMatrix(Local.GetScale3D());
		B.Rotation = B.Reference = Local.GetRotation().GetNormalized();
		B.bSolve = W.Solve[I];
		B.Stiffness = W.Stiffness[I];
		B.Limit = W.Limits[I];
		Index.Add(W.Bones[I], I);
		Lo = Lo.ComponentMin(Global.GetLocation());
		Hi = Hi.ComponentMax(Global.GetLocation());
	}
	Out.LengthScale = W.LengthScale > 0.0 ? W.LengthScale : FMath::Max((Hi - Lo).Size() * 0.2, 1.0);
	for (const TArray<FName>& Names : W.Chains)
	{
		TArray<int32> Chain;
		for (const FName& N : Names)
		{
			if (const int32* I = Index.Find(N))
			{
				Chain.Add(*I);
			}
		}
		if (Chain.Num() >= 2)
		{
			Rules.Chains.Add(MoveTemp(Chain));
		}
	}
	return true;
}

/** Writes the pose into the hierarchy, parents first, as each bone's local transform. */
void ApplyPose(URigHierarchy* H, const FRuleWeb& W, const FProblem& P, const FPose& S)
{
	TRACE_CPUPROFILER_EVENT_SCOPE(LilaStrings::ApplyPose);

	for (int32 I = 0; I < P.Bones.Num(); ++I)
	{
		const FBone& B = P.Bones[I];
		const FVector Lengthen = (S.Stretch[I] - 1.0) * B.Before.GetOrigin();
		const FMatrix Local = Mul(Mul(Mul(FTranslationMatrix(S.Offset[I] + Lengthen), B.Before), FQuatRotationMatrix(S.Rotation[I])), B.After);
		H->SetLocalTransform(BoneKey(W.Bones[I]), FTransform(Local), false, true);
	}
}

/** Every bone of the web as it is, to blend toward by Alpha after the solve. */
struct FPoseBlend
{
	TArray<TPair<FName, FTransform>> Before;

	void Capture(URigHierarchy* H, const FRuleWeb& W)
	{
		for (const FName& Bone : W.Bones)
		{
			Before.Emplace(Bone, H->GetLocalTransform(BoneKey(Bone)));
		}
	}

	void Apply(URigHierarchy* H, const float Alpha) const
	{
		if (Alpha >= 1.0f)
		{
			return;
		}
		for (const TPair<FName, FTransform>& B : Before)
		{
			FTransform Blended;
			Blended.Blend(B.Value, H->GetLocalTransform(BoneKey(B.Key)), FMath::Clamp(Alpha, 0.0f, 1.0f));
			H->SetLocalTransform(BoneKey(B.Key), Blended, false, true);
		}
	}
};

/** What a unit asks for, whichever mode. */
struct FAsk
{
	const FString& RuleWeb;
	FName Bone;
	FName Control;
	const FTransform& Target;
	bool bHoldContacts = true;
	float Alpha = 1.0f;
	bool bRotate = true;
	int32 ChainLength = 0;
};

/** Poses Ask.Bone in Mode as Lila's stringsPose does, from the hierarchy's pose. False (with a warning) when it can't. */
bool Run(URigHierarchy* H, const EMode Mode, const FAsk& Ask, float& OutError, const TFunctionRef<void(const FString&)> Warn)
{
	TRACE_CPUPROFILER_EVENT_SCOPE(LilaStrings::Run);

	if (!H || Ask.Alpha <= 0.0f)
	{
		return false;
	}
	const TSharedPtr<FRuleWeb> Web = Cached(Ask.RuleWeb);
	const FRuleWeb& W = *Web;
	if (!W.Error.IsEmpty())
	{
		Warn(W.Error);
		return false;
	}
	FProblem P;
	FRules Rules;
	FName Missing;
	if (!ProblemOf(H, W, P, Rules, Missing))
	{
		Warn(FString::Printf(TEXT("No bone named %s, which the rule web poses"), *Missing.ToString()));
		return false;
	}
	const int32 Grabbed = W.Bones.IndexOfByKey(Ask.Bone);
	if (Grabbed == INDEX_NONE)
	{
		Warn(FString::Printf(TEXT("%s isn't a bone the rule web poses"), *Ask.Bone.ToString()));
		return false;
	}
	const FRigElementKey ControlKey(Ask.Control, ERigElementType::Control);
	const FTransform Goal = !Ask.Control.IsNone() && H->Contains(ControlKey) ? H->GetGlobalTransform(ControlKey) : Ask.Target;
	const FTransform Now = H->GetGlobalTransform(BoneKey(Ask.Bone));

	FRequest R;
	R.Mode = Mode;
	R.Grabbed = Grabbed;
	R.Hips = W.Bones.IndexOfByKey(W.Hips);
	R.ChainLength = Ask.ChainLength;
	R.Rules = &Rules;
	if (const FVector Move = Goal.GetLocation() - Now.GetLocation(); !Move.IsNearlyZero(1e-6))
	{
		R.Move = Move;
	}
	// Forward and Backward always take the target's rotation; IK and Full Body when asked to.
	const FQuat Turn = (Goal.GetRotation() * Now.GetRotation().Inverse()).GetNormalized();
	if ((Ask.bRotate || Mode == EMode::Forward || Mode == EMode::Backward) && !Turn.Equals(FQuat::Identity, 1e-9))
	{
		R.Turn = Turn;
	}
	if (Ask.bHoldContacts)
	{
		const TArray<FMatrix> Start = Worlds(P, StartPose(P));
		for (const FName& C : W.Contacts)
		{
			const int32 Bone = W.Bones.IndexOfByKey(C);
			if (Bone != INDEX_NONE && Bone != Grabbed && !R.Holds.ContainsByPredicate([&](const FHold& Hold) { return Hold.Bone == Bone; }))
			{
				R.Holds.Add({Bone, Start[Bone], true});
			}
		}
	}
	// Full Body turning with nothing held is a plain turn: no solve has anything to keep.
	if (Mode == EMode::FullBody && !R.Move && R.Holds.IsEmpty())
	{
		R.Mode = EMode::Forward;
	}
	FPoseBlend Blend;
	Blend.Capture(H, W);
	FResult Res = SolveRequest(P, R);
	if (!Res.bHandled)
	{
		// A bone above the hips has no body to swing or limb to bend: it turns and moves the plain way.
		R.Mode = EMode::Forward;
		Res = SolveRequest(P, R);
	}
	ApplyPose(H, W, P, Res.Pose);
	Blend.Apply(H, Ask.Alpha);
	OutError = float(Res.Error);
	return true;
}
} // namespace

FRigUnit_LilaStringsForward_Execute()
{
	TRACE_CPUPROFILER_EVENT_SCOPE(RigUnit_LilaStringsForward::Execute);

	Run(ExecuteContext.Hierarchy, EMode::Forward, {RuleWeb, Bone, Control, Target, bHoldContacts, Alpha, true, 0}, Error,
		[&](const FString& Message) { UE_CONTROLRIG_RIGUNIT_REPORT_WARNING(TEXT("%s"), *Message); });
}

FRigUnit_LilaStringsBackward_Execute()
{
	TRACE_CPUPROFILER_EVENT_SCOPE(RigUnit_LilaStringsBackward::Execute);

	Run(ExecuteContext.Hierarchy, EMode::Backward, {RuleWeb, Bone, Control, Target, bHoldContacts, Alpha, true, 0}, Error,
		[&](const FString& Message) { UE_CONTROLRIG_RIGUNIT_REPORT_WARNING(TEXT("%s"), *Message); });
}

FRigUnit_LilaStringsIK_Execute()
{
	TRACE_CPUPROFILER_EVENT_SCOPE(RigUnit_LilaStringsIK::Execute);

	Run(ExecuteContext.Hierarchy, EMode::Ik, {RuleWeb, Bone, Control, Target, bHoldContacts, Alpha, bRotate, ChainLength}, Error,
		[&](const FString& Message) { UE_CONTROLRIG_RIGUNIT_REPORT_WARNING(TEXT("%s"), *Message); });
}

FRigUnit_LilaStringsFullBody_Execute()
{
	TRACE_CPUPROFILER_EVENT_SCOPE(RigUnit_LilaStringsFullBody::Execute);

	Run(ExecuteContext.Hierarchy, EMode::FullBody, {RuleWeb, Bone, Control, Target, bHoldContacts, Alpha, bRotate, 0}, Error,
		[&](const FString& Message) { UE_CONTROLRIG_RIGUNIT_REPORT_WARNING(TEXT("%s"), *Message); });
}

TMap<FName, FTransform> ULilaStringsLibrary::SolveOnMesh(USkeletalMesh* Mesh, const FString& RuleWeb, const FString& Mode, const FName Bone, const FTransform& Target,
														 const bool bRotate, const bool bHoldContacts, const int32 ChainLength, float& Error)
{
	TRACE_CPUPROFILER_EVENT_SCOPE(LilaStringsLibrary::SolveOnMesh);

	TMap<FName, FTransform> Out;
	Error = -1.0f;
	if (!Mesh)
	{
		return Out;
	}
	const TMap<FString, EMode> Modes = {{TEXT("forward"), EMode::Forward}, {TEXT("backward"), EMode::Backward}, {TEXT("ik"), EMode::Ik}, {TEXT("fullBody"), EMode::FullBody}};
	const EMode* Found = Modes.Find(Mode);
	if (!Found)
	{
		UE_LOG(LogTemp, Warning, TEXT("Lila Strings: no mode %s (forward, backward, ik, fullBody)"), *Mode);
		return Out;
	}
	URigHierarchy* H = NewObject<URigHierarchy>(GetTransientPackage());
	H->GetController(true)->ImportBones(Mesh->GetRefSkeleton(), NAME_None, true, true, false, false);
	if (Run(H, *Found, {RuleWeb, Bone, NAME_None, Target, bHoldContacts, 1.0f, bRotate, ChainLength}, Error,
			[](const FString& Message) { UE_LOG(LogTemp, Warning, TEXT("Lila Strings: %s"), *Message); }))
	{
		H->ForEach<FRigBoneElement>([&](FRigBoneElement* E) {
			Out.Add(E->GetFName(), H->GetGlobalTransform(E->GetKey()));
			return true;
		});
	}
	return Out;
}

TMap<FName, FTransform> ULilaStringsLibrary::DragRig(UClass* RigClass, const FName Control, const FTransform& Target)
{
	TRACE_CPUPROFILER_EVENT_SCOPE(LilaStringsLibrary::DragRig);

	TMap<FName, FTransform> Out;
	if (!RigClass || !RigClass->IsChildOf(UControlRig::StaticClass()))
	{
		return Out;
	}
	UControlRig* Rig = NewObject<UControlRig>(GetTransientPackage(), RigClass);
	Rig->Initialize(true);
	Rig->Execute(FRigUnit_BeginExecution::EventName);
	URigHierarchy* H = Rig->GetHierarchy();
	if (!H)
	{
		return Out;
	}
	H->SetGlobalTransform(FRigElementKey(Control, ERigElementType::Control), Target, false, true);
	Rig->Execute(FRigUnit_BeginExecution::EventName);
	H->ForEach<FRigBoneElement>([&](FRigBoneElement* E) {
		Out.Add(E->GetFName(), H->GetGlobalTransform(E->GetKey()));
		return true;
	});
	return Out;
}
