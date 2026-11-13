#include "RigUnit_LilaStrings.h"

#include "LilaStringsSolver.h"

#include "ControlRig.h"
#include "ControlRigObjectBinding.h"
#include "Components/PoseableMeshComponent.h"
#include "Components/SkinnedMeshComponent.h"
#include "EngineUtils.h"
#include "Dom/JsonObject.h"
#include "Engine/SkeletalMesh.h"
#include "Misc/ScopeLock.h"
#include "MovieScene.h"
#include "Sections/MovieSceneParameterSection.h"
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
	TArray<FName> Isolate;
	TArray<TPair<FName, EMode>> Modes;
	/** Lila's pinned joints: they hold where they are, whatever the contacts do. */
	TArray<FName> Pins;
	/** IK chains: the end with whether the chain is locked (a locked end holds as a pin does), and how to solve it. */
	struct FLock
	{
		FName Bone;
		bool bLocked = false;
		/** The chain's bones, start to end; empty when one isn't a bone of the skeleton (the end is then only held). */
		TArray<FName> Chain;
		/** How the handle solves it; its Bones are filled per evaluation. */
		FChainIk Ik;
	};
	TArray<FLock> Locks;
	FVector Up = FVector::ZAxisVector;
	FVector Side = FVector::XAxisVector;
	/** Glue (two-way) and attach (all but the last follow the last), each switchable by a channel when the rig has one. */
	struct FRelation
	{
		TArray<FName> Members;
		bool bAttach = false;
		FName Control;
		FName Channel;
	};
	TArray<FRelation> Relations;
	/**
	 * Glue and attach members outside the skeleton: props and other characters' joints, each a frame in this character's
	 * space and the rig's control for it. A relation names one as "@" and its name.
	 */
	struct FExternal
	{
		FName Name;
		FMatrix Frame = FMatrix::Identity;
		FName Control;
	};
	TArray<FExternal> Externals;
	/** The rig's control for a bone, where its Pin and Lock channels live. */
	TMap<FName, FName> Controls;
	double LengthScale = 0.0;
	FString Error;
};

/** Lila's mode names (strings::modeName) to the solver's modes. */
TOptional<EMode> ModeNamed(const FString& Name)
{
	static const TPair<const TCHAR*, EMode> Names[] = {
		{TEXT("default"), EMode::Default},
		{TEXT("free"), EMode::Free},
		{TEXT("forward"), EMode::Forward},
		{TEXT("forwardAll"), EMode::ForwardAll},
		{TEXT("backward"), EMode::Backward},
		{TEXT("backwardAll"), EMode::BackwardAll},
		{TEXT("bidirectional"), EMode::Bidirectional},
		{TEXT("all"), EMode::All},
		{TEXT("ik"), EMode::Ik},
		{TEXT("fullBody"), EMode::FullBody},
	};
	for (const TPair<const TCHAR*, EMode>& N : Names)
	{
		if (Name.Equals(N.Key, ESearchCase::IgnoreCase))
		{
			return N.Value;
		}
	}
	return {};
}

EMode ModeOf(const ELilaStringsMode Mode)
{
	switch (Mode)
	{
	case ELilaStringsMode::Default: return EMode::Default;
	case ELilaStringsMode::Free: return EMode::Free;
	case ELilaStringsMode::Forward: return EMode::Forward;
	case ELilaStringsMode::ForwardAll: return EMode::ForwardAll;
	case ELilaStringsMode::Backward: return EMode::Backward;
	case ELilaStringsMode::BackwardAll: return EMode::BackwardAll;
	case ELilaStringsMode::Bidirectional: return EMode::Bidirectional;
	case ELilaStringsMode::All: return EMode::All;
	case ELilaStringsMode::IK: return EMode::Ik;
	case ELilaStringsMode::FullBody: return EMode::FullBody;
	}
	return EMode::FullBody;
}

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

/** A quaternion [x, y, z, w], identity when the field is missing. */
FQuat ReadQuat(const TSharedPtr<FJsonObject>& Obj, const TCHAR* Field)
{
	const TArray<TSharedPtr<FJsonValue>>* V = nullptr;
	return Obj->TryGetArrayField(Field, V) && V->Num() == 4 ? FQuat((*V)[0]->AsNumber(), (*V)[1]->AsNumber(), (*V)[2]->AsNumber(), (*V)[3]->AsNumber()).GetNormalized()
																 : FQuat::Identity;
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
	if (Root->TryGetArrayField(TEXT("isolate"), Values))
	{
		Web->Isolate = ReadNames(*Values);
	}
	const TSharedPtr<FJsonObject>* ModesOf = nullptr;
	if (Root->TryGetObjectField(TEXT("modes"), ModesOf))
	{
		for (const TPair<FString, TSharedPtr<FJsonValue>>& M : (*ModesOf)->Values)
		{
			if (const TOptional<EMode> Mode = ModeNamed(M.Value->AsString()))
			{
				Web->Modes.Emplace(FName(*M.Key), *Mode);
			}
		}
	}
	if (Root->TryGetArrayField(TEXT("pins"), Values))
	{
		Web->Pins = ReadNames(*Values);
	}
	if (Root->TryGetArrayField(TEXT("locks"), Values))
	{
		for (const TSharedPtr<FJsonValue>& L : *Values)
		{
			const TSharedPtr<FJsonObject> Obj = L->AsObject();
			FRuleWeb::FLock& Lock = Web->Locks.AddDefaulted_GetRef();
			Lock.Bone = FName(*Obj->GetStringField(TEXT("bone")));
			Obj->TryGetBoolField(TEXT("locked"), Lock.bLocked);
			const TArray<TSharedPtr<FJsonValue>>* Chain = nullptr;
			if (Obj->TryGetArrayField(TEXT("chain"), Chain))
			{
				Lock.Chain = ReadNames(*Chain);
				FChainIk& C = Lock.Ik;
				const FString Solver = Obj->GetStringField(TEXT("solver"));
				C.Solver = Solver == TEXT("twoBone") ? FChainIk::ESolver::TwoBone : Solver == TEXT("spline") ? FChainIk::ESolver::Spline : FChainIk::ESolver::Fabrik;
				if (Obj->HasField(TEXT("pole")))
				{
					C.Pole = ReadVector3(Obj, TEXT("pole"), FVector::ZeroVector);
				}
				Obj->TryGetNumberField(TEXT("twist"), C.Twist);
				Obj->TryGetNumberField(TEXT("roll"), C.Roll);
				Obj->TryGetNumberField(TEXT("softness"), C.Softness);
				Obj->TryGetNumberField(TEXT("maxStretch"), C.MaxStretch);
				Obj->TryGetBoolField(TEXT("fitCurve"), C.bFitCurve);
				Obj->TryGetNumberField(TEXT("alignWeight"), C.AlignWeight);
				const TArray<TSharedPtr<FJsonValue>>* Curve = nullptr;
				if (Obj->TryGetArrayField(TEXT("curve"), Curve))
				{
					for (const TSharedPtr<FJsonValue>& P : *Curve)
					{
						const TArray<TSharedPtr<FJsonValue>>& V = P->AsArray();
						C.Curve.Emplace(V[0]->AsNumber(), V[1]->AsNumber(), V[2]->AsNumber());
					}
				}
				const TArray<TSharedPtr<FJsonValue>>* Slots = nullptr;
				if (Obj->TryGetArrayField(TEXT("slots"), Slots))
				{
					for (const TSharedPtr<FJsonValue>& SV : *Slots)
					{
						const TSharedPtr<FJsonObject> So = SV->AsObject();
						FChainSlot& Slot = C.Slots.AddDefaulted_GetRef();
						Slot.Rest = ReadVector3(So, TEXT("rest"), FVector::ZeroVector);
						Slot.RestRotation = ReadQuat(So, TEXT("restRotation"));
						Slot.Orient = ReadQuat(So, TEXT("orient"));
						Slot.Axis = ReadQuat(So, TEXT("axis"));
						if (So->HasField(TEXT("preferred")))
						{
							Slot.Preferred = ReadQuat(So, TEXT("preferred"));
						}
						const TSharedPtr<FJsonObject>* Limit = nullptr;
						if (So->TryGetObjectField(TEXT("limit"), Limit))
						{
							FLimit& Lim = Slot.Limit;
							const FString Kind = (*Limit)->GetStringField(TEXT("kind"));
							Lim.Kind = Kind == TEXT("hinge") ? FLimit::EKind::Hinge : Kind == TEXT("cone") ? FLimit::EKind::Cone : FLimit::EKind::None;
							Lim.Axis = ReadVector3(*Limit, TEXT("axis"), FVector::ZAxisVector);
							Lim.BoneDir = ReadVector3(*Limit, TEXT("bone"), FVector::XAxisVector);
							(*Limit)->TryGetNumberField(TEXT("min"), Lim.Min);
							(*Limit)->TryGetNumberField(TEXT("max"), Lim.Max);
							(*Limit)->TryGetNumberField(TEXT("cone"), Lim.Cone);
							(*Limit)->TryGetNumberField(TEXT("stiffness"), Slot.Stiffness);
							Lim.Post = Slot.Axis;
						}
					}
				}
			}
		}
	}
	if (Root->TryGetArrayField(TEXT("glue"), Values))
	{
		for (const TSharedPtr<FJsonValue>& G : *Values)
		{
			const TSharedPtr<FJsonObject> Obj = G->AsObject();
			FRuleWeb::FRelation& R = Web->Relations.AddDefaulted_GetRef();
			const TArray<TSharedPtr<FJsonValue>>* Members = nullptr;
			if (Obj->TryGetArrayField(TEXT("members"), Members))
			{
				for (const TSharedPtr<FJsonValue>& M : *Members)
				{
					const TSharedPtr<FJsonObject>* Ext = nullptr;
					if (!M->TryGetObject(Ext))
					{
						R.Members.Add(FName(*M->AsString()));
						continue;
					}
					const FName Name(*(*Ext)->GetStringField(TEXT("name")));
					if (!Web->Externals.ContainsByPredicate([&](const FRuleWeb::FExternal& E) { return E.Name == Name; }))
					{
						FRuleWeb::FExternal& E = Web->Externals.AddDefaulted_GetRef();
						E.Name = Name;
						E.Frame = FQuatRotationTranslationMatrix(ReadQuat(*Ext, TEXT("rotation")), ReadVector3(*Ext, TEXT("location"), FVector::ZeroVector));
						FString Control;
						if ((*Ext)->TryGetStringField(TEXT("control"), Control))
						{
							E.Control = FName(*Control);
						}
					}
					R.Members.Add(FName(*(TEXT("@") + Name.ToString())));
				}
			}
			Obj->TryGetBoolField(TEXT("attach"), R.bAttach);
			const TSharedPtr<FJsonObject>* Channel = nullptr;
			if (Obj->TryGetObjectField(TEXT("channel"), Channel))
			{
				R.Control = FName(*(*Channel)->GetStringField(TEXT("control")));
				R.Channel = FName(*(*Channel)->GetStringField(TEXT("name")));
			}
		}
	}
	const TSharedPtr<FJsonObject>* Basis = nullptr;
	if (Root->TryGetObjectField(TEXT("basis"), Basis))
	{
		Web->Up = ReadVector3(*Basis, TEXT("up"), Web->Up);
		Web->Side = ReadVector3(*Basis, TEXT("side"), Web->Side);
	}
	const TSharedPtr<FJsonObject>* ControlsOf = nullptr;
	if (Root->TryGetObjectField(TEXT("controls"), ControlsOf))
	{
		for (const auto& C : (*ControlsOf)->Values)
		{
			Web->Controls.Add(FName(*FString(C.Key)), FName(*C.Value->AsString()));
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
	for (const TPair<FName, EMode>& M : W.Modes)
	{
		if (const int32* I = Index.Find(M.Key))
		{
			Rules.Defaults.Emplace(*I, M.Value);
		}
	}
	return true;
}

/** Sets a bone's local transform even when it moved less than the hierarchy's 1e-4 change test, as a limb held at a limit can. */
void SetLocalForced(URigHierarchy* H, const FRigElementKey& Key, const FTransform& Local)
{
	if (FRigTransformElement* E = H->Find<FRigTransformElement>(Key))
	{
		H->SetTransform(E, Local, ERigTransformType::CurrentLocal, true, false, true);
	}
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
		SetLocalForced(H, BoneKey(W.Bones[I]), FTransform(Local));
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
			SetLocalForced(H, BoneKey(B.Key), Blended);
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
	bool bIsolate = false;
	double MaxStretch = 1.0;
	/** What a bone the mode can't move (above the hips in Backward or IK) poses in instead. */
	EMode Fallback = EMode::Forward;
};

/** The control's animation channel named Name, when it has one. */
const FRigControlElement* ChannelOf(const URigHierarchy* H, const FRigElementKey& Control, const FName Name)
{
	const FRigControlElement* Element = H->Find<FRigControlElement>(Control);
	if (!Element)
	{
		return nullptr;
	}
	for (const FRigBaseElement* Child : H->GetChildren(Element))
	{
		if (const FRigControlElement* Channel = Cast<FRigControlElement>(Child); Channel && Channel->IsAnimationChannel() && Channel->GetDisplayName().IsEqual(Name))
		{
			return Channel;
		}
	}
	return nullptr;
}

/** Mode and Isolate Rotation from the control's channels where it has them, else as the unit's pins give them. */
void ModeFromChannels(const URigHierarchy* H, const FRigElementKey& Control, EMode& Mode, bool& bIsolate)
{
	TRACE_CPUPROFILER_EVENT_SCOPE(LilaStrings::ModeFromChannels);

	if (const FRigControlElement* M = ChannelOf(H, Control, TEXT("Mode")))
	{
		const int32 Index = H->GetControlValue(M->GetKey()).Get<int32>();
		const UEnum* Enum = StaticEnum<ELilaStringsMode>();
		// NumEnums counts the generated _MAX entry.
		if (Index >= 0 && Index < Enum->NumEnums() - 1)
		{
			Mode = ModeOf(ELilaStringsMode(Enum->GetValueByIndex(Index)));
		}
	}
	if (const FRigControlElement* I = ChannelOf(H, Control, TEXT("Isolate Rotation")))
	{
		bIsolate = H->GetControlValue(I->GetKey()).Get<bool>();
	}
}

/** The bool channel Name on Control when the rig has it, else Default. */
bool SwitchOr(const URigHierarchy* H, const FName Control, const FName Name, const bool bDefault)
{
	if (Control.IsNone())
	{
		return bDefault;
	}
	const FRigControlElement* Channel = ChannelOf(H, FRigElementKey(Control, ERigElementType::Control), Name);
	return Channel ? H->GetControlValue(Channel->GetKey()).Get<bool>() : bDefault;
}

/** Pins, then the contacts when held, then locked chain ends, as Lila's holdsFor orders them; channels override the web. */
TArray<int32> HeldBones(const URigHierarchy* H, const FRuleWeb& W, const bool bContacts)
{
	TRACE_CPUPROFILER_EVENT_SCOPE(LilaStrings::HeldBones);

	TArray<FName> Names;
	auto ControlOf = [&](const FName Bone) {
		const FName* C = W.Controls.Find(Bone);
		return C ? *C : NAME_None;
	};
	for (const FName& P : W.Pins)
	{
		if (SwitchOr(H, ControlOf(P), TEXT("Pin"), true))
		{
			Names.Add(P);
		}
	}
	for (const TPair<FName, FName>& C : W.Controls)
	{
		if (!W.Pins.Contains(C.Key) && SwitchOr(H, C.Value, TEXT("Pin"), false))
		{
			Names.Add(C.Key);
		}
	}
	if (bContacts)
	{
		Names.Append(W.Contacts);
	}
	for (const FRuleWeb::FLock& L : W.Locks)
	{
		if (SwitchOr(H, ControlOf(L.Bone), TEXT("Lock"), L.bLocked))
		{
			Names.Add(L.Bone);
		}
	}
	TArray<int32> Out;
	for (const FName& N : Names)
	{
		if (const int32 B = W.Bones.IndexOfByKey(N); B != INDEX_NONE)
		{
			Out.AddUnique(B);
		}
	}
	return Out;
}

/** A general 4x4 inverse computed as glm::inverse does, term for term, so glued followers land on the same bits as in Lila. */
FMatrix InverseAsLila(const FMatrix& In)
{
	const auto& m = In.M;
	const double C00 = m[2][2] * m[3][3] - m[3][2] * m[2][3], C02 = m[1][2] * m[3][3] - m[3][2] * m[1][3], C03 = m[1][2] * m[2][3] - m[2][2] * m[1][3];
	const double C04 = m[2][1] * m[3][3] - m[3][1] * m[2][3], C06 = m[1][1] * m[3][3] - m[3][1] * m[1][3], C07 = m[1][1] * m[2][3] - m[2][1] * m[1][3];
	const double C08 = m[2][1] * m[3][2] - m[3][1] * m[2][2], C10 = m[1][1] * m[3][2] - m[3][1] * m[1][2], C11 = m[1][1] * m[2][2] - m[2][1] * m[1][2];
	const double C12 = m[2][0] * m[3][3] - m[3][0] * m[2][3], C14 = m[1][0] * m[3][3] - m[3][0] * m[1][3], C15 = m[1][0] * m[2][3] - m[2][0] * m[1][3];
	const double C16 = m[2][0] * m[3][2] - m[3][0] * m[2][2], C18 = m[1][0] * m[3][2] - m[3][0] * m[1][2], C19 = m[1][0] * m[2][2] - m[2][0] * m[1][2];
	const double C20 = m[2][0] * m[3][1] - m[3][0] * m[2][1], C22 = m[1][0] * m[3][1] - m[3][0] * m[1][1], C23 = m[1][0] * m[2][1] - m[2][0] * m[1][1];
	const double F0[4] = {C00, C00, C02, C03}, F1[4] = {C04, C04, C06, C07}, F2[4] = {C08, C08, C10, C11};
	const double F3[4] = {C12, C12, C14, C15}, F4[4] = {C16, C16, C18, C19}, F5[4] = {C20, C20, C22, C23};
	const double V0[4] = {m[1][0], m[0][0], m[0][0], m[0][0]}, V1[4] = {m[1][1], m[0][1], m[0][1], m[0][1]};
	const double V2[4] = {m[1][2], m[0][2], m[0][2], m[0][2]}, V3[4] = {m[1][3], m[0][3], m[0][3], m[0][3]};
	const double SignA[4] = {1.0, -1.0, 1.0, -1.0}, SignB[4] = {-1.0, 1.0, -1.0, 1.0};
	FMatrix Out;
	for (int32 I = 0; I < 4; ++I)
	{
		Out.M[0][I] = (V1[I] * F0[I] - V2[I] * F1[I] + V3[I] * F2[I]) * SignA[I];
		Out.M[1][I] = (V0[I] * F0[I] - V2[I] * F3[I] + V3[I] * F4[I]) * SignB[I];
		Out.M[2][I] = (V0[I] * F1[I] - V1[I] * F3[I] + V3[I] * F5[I]) * SignA[I];
		Out.M[3][I] = (V0[I] * F2[I] - V1[I] * F4[I] + V2[I] * F5[I]) * SignB[I];
	}
	const double D0 = m[0][0] * Out.M[0][0], D1 = m[0][1] * Out.M[1][0], D2 = m[0][2] * Out.M[2][0], D3 = m[0][3] * Out.M[3][0];
	const double OneOverDet = 1.0 / ((D0 + D1) + (D2 + D3));
	for (int32 C = 0; C < 4; ++C)
	{
		for (int32 R = 0; R < 4; ++R)
		{
			Out.M[C][R] *= OneOverDet;
		}
	}
	return Out;
}

bool SameMatrix(const FMatrix& A, const FMatrix& B)
{
	for (int32 R = 0; R < 4; ++R)
	{
		if (FVector4(A.M[R][0] - B.M[R][0], A.M[R][1] - B.M[R][1], A.M[R][2] - B.M[R][2], A.M[R][3] - B.M[R][3]).Size() > 1e-9)
		{
			return false;
		}
	}
	return true;
}

/**
 * Where glue and attach put their members after a solve took the bones from W0 to W1 (Lila's followersOf): an attach's
 * followers ride its last member; a glue's ride the grabbed bone when it is one of them, else the first that moved.
 */
TArray<FHold> Followers(const URigHierarchy* H, const FRuleWeb& W, const TArray<FMatrix>& W0, const TArray<FMatrix>& W1, const int32 Grabbed)
{
	TRACE_CPUPROFILER_EVENT_SCOPE(LilaStrings::Followers);

	TArray<FHold> Out;
	for (const FRuleWeb::FRelation& R : W.Relations)
	{
		if (!R.Channel.IsNone() && !SwitchOr(H, R.Control, R.Channel, true))
		{
			continue;
		}
		// Bones by index, externals after them.
		TArray<int32> Members;
		for (const FName& M : R.Members)
		{
			if (const FString Text = M.ToString(); Text.StartsWith(TEXT("@")))
			{
				const FName Name(*Text.RightChop(1));
				if (const int32 E = W.Externals.IndexOfByPredicate([&](const FRuleWeb::FExternal& X) { return X.Name == Name; }); E != INDEX_NONE)
				{
					Members.Add(W.Bones.Num() + E);
				}
			}
			else if (const int32 B = W.Bones.IndexOfByKey(M); B != INDEX_NONE)
			{
				Members.Add(B);
			}
		}
		if (Members.Num() < 2)
		{
			continue;
		}
		int32 Leader = INDEX_NONE;
		if (R.bAttach)
		{
			Leader = Members.Last();
		}
		else if (Members.Contains(Grabbed))
		{
			Leader = Grabbed;
		}
		else
		{
			for (const int32 M : Members)
			{
				if (!SameMatrix(W0[M], W1[M]))
				{
					Leader = M;
					break;
				}
			}
		}
		if (Leader == INDEX_NONE || SameMatrix(W0[Leader], W1[Leader]))
		{
			continue;
		}
		const FMatrix Delta = Mul(W1[Leader], InverseAsLila(W0[Leader]));
		for (const int32 M : Members)
		{
			if (M != Leader && !Out.ContainsByPredicate([&](const FHold& F) { return F.Bone == M; }))
			{
				Out.Add({M, Mul(Delta, W0[M]), true});
			}
		}
	}
	return Out;
}

/** The rule web's locked IK chains (Lock channel where the rig has one), each with its controller: where its end is in Raw. */
TArray<TPair<FChainIk, FMatrix>> LockedChains(const URigHierarchy* H, const FRuleWeb& W, const TArray<FMatrix>& Raw)
{
	TRACE_CPUPROFILER_EVENT_SCOPE(LilaStrings::LockedChains);

	TArray<TPair<FChainIk, FMatrix>> Out;
	for (const FRuleWeb::FLock& L : W.Locks)
	{
		const FName* LockControl = W.Controls.Find(L.Bone);
		if (L.Chain.Num() < 2 || !SwitchOr(H, LockControl ? *LockControl : NAME_None, TEXT("Lock"), L.bLocked))
		{
			continue;
		}
		FChainIk C = L.Ik;
		for (const FName& N : L.Chain)
		{
			C.Bones.Add(W.Bones.IndexOfByKey(N));
		}
		if (C.Bones.Contains(INDEX_NONE))
		{
			continue;
		}
		C.Up = W.Up;
		C.Side = W.Side;
		// The controller: the end's control in a rig, else where the end is.
		const FName* EndControl = W.Controls.Find(L.Bone);
		const FRigElementKey EndKey(EndControl ? *EndControl : NAME_None, ERigElementType::Control);
		const bool bDragged = EndControl && H->Contains(EndKey) && !FRigComputedTransform::Equals(H->GetGlobalTransform(EndKey), H->GetGlobalTransform(EndKey, true), 1e-9f);
		// Resting, the control sits where the bone rests; that rest, in double precision, rather than the control's single.
		const FMatrix Controller = bDragged											? H->GetGlobalTransform(EndKey).ToMatrixWithScale()
								   : EndControl && H->Contains(EndKey)				? H->GetGlobalTransform(BoneKey(L.Bone), true).ToMatrixWithScale()
																					: Raw[C.Bones.Last()];
		Out.Emplace(MoveTemp(C), Controller);
	}
	return Out;
}

/** Makes S the problem's own pose: offsets and stretches folded into each bone, its rotation the reference. */
void FoldPose(FProblem& P, const FPose& S)
{
	for (int32 I = 0; I < P.Bones.Num(); ++I)
	{
		FBone& B = P.Bones[I];
		B.Before = Mul(FTranslationMatrix(S.Offset[I] + (S.Stretch[I] - 1.0) * B.Before.GetOrigin()), B.Before);
		B.Rotation = B.Reference = S.Rotation[I];
	}
}

/** Where each external member is: its control (as it started, or as it is now) where the rig has one, else its frame. */
TArray<FMatrix> ExternalWorlds(const URigHierarchy* H, const FRuleWeb& W, const bool bInitial)
{
	TArray<FMatrix> Out;
	for (const FRuleWeb::FExternal& E : W.Externals)
	{
		const FRigElementKey Key(E.Control, ERigElementType::Control);
		Out.Add(!E.Control.IsNone() && H->Contains(Key) ? H->GetGlobalTransform(Key, bInitial).ToMatrixWithScale() : E.Frame);
	}
	return Out;
}

/**
 * External controls a bone's unit moved this evaluation: their own units, which run after, leave them be rather than
 * drag the bones a second time.
 */
TSet<FName>& PlacedBy(const URigHierarchy* H, FCriticalSection*& OutLock)
{
	static FCriticalSection Lock;
	static TMap<const URigHierarchy*, TSet<FName>> Placed;
	OutLock = &Lock;
	return Placed.FindOrAdd(H);
}

/** Moves the external followers' controls where the glue puts them. */
void PlaceExternals(URigHierarchy* H, const FRuleWeb& W, const TArray<FHold>& Followers)
{
	for (const FHold& F : Followers)
	{
		if (const int32 E = F.Bone - W.Bones.Num(); W.Externals.IsValidIndex(E))
		{
			const FRigElementKey Key(W.Externals[E].Control, ERigElementType::Control);
			if (!W.Externals[E].Control.IsNone() && H->Contains(Key))
			{
				H->SetGlobalTransform(Key, FTransform(F.World), false, false);
				FCriticalSection* Lock = nullptr;
				TSet<FName>& Placed = PlacedBy(H, Lock);
				FScopeLock Guard(Lock);
				Placed.Add(W.Externals[E].Control);
			}
		}
	}
}

/**
 * A prop or another character's joint dragged by its control: the glued and attached bones of this character bend their
 * limbs to keep their place on it, its pins, contacts and locks holding (Lila's follow).
 */
bool RunFollow(URigHierarchy* H, const FRuleWeb& W, FProblem& P, const FRules& Rules, const int32 External, const bool bHoldContacts, float& OutError)
{
	TRACE_CPUPROFILER_EVENT_SCOPE(LilaStrings::RunFollow);

	{
		FCriticalSection* Lock = nullptr;
		TSet<FName>& Placed = PlacedBy(H, Lock);
		FScopeLock Guard(Lock);
		if (Placed.Remove(W.Externals[External].Control) > 0)
		{
			OutError = 0.0f;
			return true;
		}
	}
	// Lila's follow solves on the skeleton as it is, without the full-body stiffness a drag uses.
	for (FBone& B : P.Bones)
	{
		B.Stiffness = 0.0;
	}

	const TArray<FMatrix> Start = Worlds(P, StartPose(P));
	TArray<FMatrix> W0 = Start, W1 = Start;
	TArray<FMatrix> Now = ExternalWorlds(H, W, false);
	TArray<FMatrix> Before = Now;
	Before[External] = ExternalWorlds(H, W, true)[External];
	W0.Append(Before);
	W1.Append(Now);
	TArray<FHold> Holds;
	for (const int32 Bone : HeldBones(H, W, bHoldContacts))
	{
		Holds.Add({Bone, Start[Bone], true});
	}
	const TArray<FHold> Moved = Followers(H, W, W0, W1, W.Bones.Num() + External);
	bool bBones = false;
	for (const FHold& F : Moved)
	{
		if (F.Bone < W.Bones.Num())
		{
			Holds.RemoveAll([&](const FHold& Hold) { return Hold.Bone == F.Bone; });
			Holds.Add(F);
			bBones = true;
		}
	}
	PlaceExternals(H, W, Moved);
	if (bBones)
	{
		FPose S = StartPose(P);
		const int32 Hips = W.Bones.IndexOfByKey(W.Hips);
		OutError = float(HoldLimbs(P, S, Holds, Hips, 1.0, {}, &Rules));
		ApplyPose(H, W, P, S);
	}
	return true;
}

/** Poses Ask.Bone in Mode as Lila's stringsPose does, from the hierarchy's pose. False (with a warning) when it can't. */
bool Run(URigHierarchy* H, EMode Mode, const FAsk& Ask, float& OutError, const TFunctionRef<void(const FString&)> Warn)
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
	if (const int32 External = W.Externals.IndexOfByPredicate([&](const FRuleWeb::FExternal& E) { return !Ask.Control.IsNone() && E.Control == Ask.Control; });
		Grabbed == INDEX_NONE && External != INDEX_NONE)
	{
		const FRigElementKey Key(Ask.Control, ERigElementType::Control);
		if (!H->Contains(Key) || FRigComputedTransform::Equals(H->GetGlobalTransform(Key), H->GetGlobalTransform(Key, true), 1e-9f))
		{
			OutError = 0.0f;
			return true;
		}
		return RunFollow(H, W, P, Rules, External, Ask.bHoldContacts, OutError);
	}
	if (Grabbed == INDEX_NONE)
	{
		Warn(FString::Printf(TEXT("%s isn't a bone the rule web poses"), *Ask.Bone.ToString()));
		return false;
	}
	const FRigElementKey ControlKey(Ask.Control, ERigElementType::Control);
	const bool bControl = !Ask.Control.IsNone() && H->Contains(ControlKey);
	bool bIsolate = Ask.bIsolate;
	if (bControl)
	{
		ModeFromChannels(H, ControlKey, Mode, bIsolate);
	}
	const FTransform Goal = bControl ? H->GetGlobalTransform(ControlKey) : Ask.Target;
	// Locked IK chains hold their joints as Lila's handle does whatever else poses them, toward where their ends are
	// before it: the pose every interaction starts from, as Lila evaluates it.
	const TArray<FMatrix> Raw = Worlds(P, StartPose(P));
	TArray<TPair<FChainIk, FMatrix>> Chains = LockedChains(H, W, Raw);
	if (!Chains.IsEmpty())
	{
		const bool bIdle = bControl && FRigComputedTransform::Equals(Goal, H->GetGlobalTransform(ControlKey, true), 1e-9f);
		FPose S0 = StartPose(P);
		for (const TPair<FChainIk, FMatrix>& C : Chains)
		{
			// An end turned part way to its controller blends from its FK rotation, which a second pass would blend
			// again: only the unit that poses applies it, so it lands once.
			FChainIk Once = C.Key;
			if (bIdle && Once.AlignWeight < 1.0)
			{
				Once.AlignWeight = 0.0;
			}
			SolveChain(P, S0, Once, C.Value);
		}
		FoldPose(P, S0);
	}
	// A control left where it started asks for nothing: posing toward it would undo what the dragged controls' units did.
	if (bControl && FRigComputedTransform::Equals(Goal, H->GetGlobalTransform(ControlKey, true), 1e-9f))
	{
		if (!Chains.IsEmpty())
		{
			ApplyPose(H, W, P, StartPose(P));
		}
		OutError = 0.0f;
		return true;
	}
	const FTransform Now = Chains.IsEmpty() ? H->GetGlobalTransform(BoneKey(Ask.Bone)) : FTransform(Worlds(P, StartPose(P))[Grabbed]);

	FRequest R;
	R.Mode = Mode;
	R.Grabbed = Grabbed;
	R.Hips = W.Bones.IndexOfByKey(W.Hips);
	R.ChainLength = Ask.ChainLength;
	R.MaxStretch = FMath::Max(1.0, Ask.MaxStretch);
	R.Rules = &Rules;
	if (bIsolate)
	{
		for (const FName& Name : W.Isolate)
		{
			if (const int32 B = W.Bones.IndexOfByKey(Name); B != INDEX_NONE)
			{
				R.Isolate.Add(B);
			}
		}
	}
	// Control values are stored in single precision: a control dragged only one way sits a few 1e-6 cm and 1e-7 rad off its
	// bone the other way, which must not read as a move or turn (a turn pins the rotation; a move in Forward replaces the turn).
	const double MoveTolerance = bControl ? 1e-4 : 1e-6;
	const double TurnTolerance = bControl ? 1e-6 : 1e-9;
	if (const FVector Move = Goal.GetLocation() - Now.GetLocation(); !Move.IsNearlyZero(MoveTolerance))
	{
		R.Move = Move;
	}
	// IK and Full Body take the target's rotation when asked to; the other modes always do.
	const FQuat Turn = (Goal.GetRotation() * Now.GetRotation().Inverse()).GetNormalized();
	if ((Ask.bRotate || (Mode != EMode::Ik && Mode != EMode::FullBody)) && !Turn.Equals(FQuat::Identity, TurnTolerance))
	{
		R.Turn = Turn;
	}
	const TArray<FMatrix> Start = Worlds(P, StartPose(P));
	for (const int32 Bone : HeldBones(H, W, Ask.bHoldContacts))
	{
		if (Bone != Grabbed)
		{
			R.Holds.Add({Bone, Start[Bone], true});
		}
	}
	// Full Body turning with nothing held is a plain turn: no solve has anything to keep.
	if (Mode == EMode::FullBody && !R.Move && R.Holds.IsEmpty())
	{
		R.Mode = EMode::ForwardAll;
	}
	FPoseBlend Blend;
	Blend.Capture(H, W);
	FResult Res = SolveRequest(P, R);
	if (!Res.bHandled)
	{
		// A bone above the hips has no body to swing or limb to bend.
		R.Mode = Ask.Fallback;
		Res = SolveRequest(P, R);
	}
	// Glue and attach: what follows the grabbed bone bends its limb there, the holds staying.
	if (!W.Relations.IsEmpty())
	{
		TArray<FHold> Glued = R.Holds;
		bool bFollowed = false;
		const TArray<FMatrix> Externals = ExternalWorlds(H, W, false);
		TArray<FMatrix> W0 = Start, W1 = Worlds(P, Res.Pose);
		W0.Append(Externals);
		W1.Append(Externals);
		const TArray<FHold> Moved = Followers(H, W, W0, W1, Grabbed);
		PlaceExternals(H, W, Moved);
		for (const FHold& F : Moved)
		{
			if (F.Bone != Grabbed && F.Bone < W.Bones.Num())
			{
				Glued.RemoveAll([&](const FHold& Hold) { return Hold.Bone == F.Bone; });
				Glued.Add(F);
				bFollowed = true;
			}
		}
		if (bFollowed)
		{
			Res.Error = FMath::Max(Res.Error, HoldLimbs(P, Res.Pose, Glued, R.Hips, R.MaxStretch, {}, &Rules));
		}
	}
	// Locked IK chains then solve as Lila's ikHandle does, toward their controllers.
	for (const TPair<FChainIk, FMatrix>& C : Chains)
	{
		SolveChain(P, Res.Pose, C.Key, C.Value);
	}
	ApplyPose(H, W, P, Res.Pose);
	Blend.Apply(H, Ask.Alpha);
	OutError = float(Res.Error);
	return true;
}
} // namespace

FRigUnit_LilaStringsSource_Execute()
{
	TRACE_CPUPROFILER_EVENT_SCOPE(RigUnit_LilaStringsSource::Execute);

	bFound = false;
	URigHierarchy* H = ExecuteContext.Hierarchy;
	const USceneComponent* Owner = ExecuteContext.GetOwningComponent();
	UWorld* World = Owner ? Owner->GetWorld() : nullptr;
	const FRigElementKey Key(Control, ERigElementType::Control);
	if (!H || !World || SourceTag.IsNone() || !H->Contains(Key))
	{
		return;
	}
	// Found once per world and tag.
	static TMap<TPair<TWeakObjectPtr<UWorld>, FName>, TWeakObjectPtr<AActor>> Found;
	TWeakObjectPtr<AActor>& Cached = Found.FindOrAdd({World, SourceTag});
	if (!Cached.IsValid())
	{
		for (TActorIterator<AActor> It(World); It; ++It)
		{
			if (It->ActorHasTag(SourceTag))
			{
				Cached = *It;
				break;
			}
		}
	}
	const AActor* Source = Cached.Get();
	if (!Source)
	{
		return;
	}
	FTransform SourceWorld = Source->GetActorTransform();
	if (!Bone.IsNone())
	{
		TInlineComponentArray<USkinnedMeshComponent*> Meshes(Source);
		const USkinnedMeshComponent* Mesh = nullptr;
		for (const USkinnedMeshComponent* M : Meshes)
		{
			Mesh = M->DoesSocketExist(Bone) ? M : Mesh;
		}
		if (!Mesh)
		{
			return;
		}
		SourceWorld = Mesh->GetSocketTransform(Bone, RTS_World);
	}
	H->SetGlobalTransform(Key, (Offset * SourceWorld).GetRelativeTransform(Owner->GetComponentTransform()), false, false);
	bFound = true;
}

FRigUnit_LilaStrings_Execute()
{
	TRACE_CPUPROFILER_EVENT_SCOPE(RigUnit_LilaStrings::Execute);

	FAsk Ask{RuleWeb, Bone, Control, Target, bHoldContacts, Alpha, bRotate, ChainLength};
	Ask.bIsolate = bIsolateRotation;
	Ask.MaxStretch = MaxStretch;
	Run(ExecuteContext.Hierarchy, ModeOf(Mode), Ask, Error, [&](const FString& Message) { UE_CONTROLRIG_RIGUNIT_REPORT_WARNING(TEXT("%s"), *Message); });
}

namespace
{
/** The deprecated units pose as they always did: plain FK where their mode can't move the bone, no isolation, no stretch. */
FAsk LegacyAsk(FAsk Ask)
{
	Ask.Fallback = EMode::ForwardAll;
	return Ask;
}

FRigUnit_LilaStrings Upgraded(const FRigUnit_LilaStringsBase& U, const ELilaStringsMode Mode, const bool bRotate, const int32 ChainLength)
{
	FRigUnit_LilaStrings New;
	New.RuleWeb = U.RuleWeb;
	New.Bone = U.Bone;
	New.Control = U.Control;
	New.Target = U.Target;
	New.bHoldContacts = U.bHoldContacts;
	New.Alpha = U.Alpha;
	New.Mode = Mode;
	New.bRotate = bRotate;
	New.ChainLength = ChainLength;
	return New;
}
} // namespace

FRigUnit_LilaStringsForward_Execute()
{
	TRACE_CPUPROFILER_EVENT_SCOPE(RigUnit_LilaStringsForward::Execute);

	Run(ExecuteContext.Hierarchy, EMode::ForwardAll, LegacyAsk({RuleWeb, Bone, Control, Target, bHoldContacts, Alpha, true, 0}), Error,
		[&](const FString& Message) { UE_CONTROLRIG_RIGUNIT_REPORT_WARNING(TEXT("%s"), *Message); });
}

FRigVMStructUpgradeInfo FRigUnit_LilaStringsForward::GetUpgradeInfo() const
{
	return FRigVMStructUpgradeInfo(*this, Upgraded(*this, ELilaStringsMode::ForwardAll, true, 0));
}

FRigUnit_LilaStringsBackward_Execute()
{
	TRACE_CPUPROFILER_EVENT_SCOPE(RigUnit_LilaStringsBackward::Execute);

	Run(ExecuteContext.Hierarchy, EMode::Backward, LegacyAsk({RuleWeb, Bone, Control, Target, bHoldContacts, Alpha, true, 0}), Error,
		[&](const FString& Message) { UE_CONTROLRIG_RIGUNIT_REPORT_WARNING(TEXT("%s"), *Message); });
}

FRigVMStructUpgradeInfo FRigUnit_LilaStringsBackward::GetUpgradeInfo() const
{
	return FRigVMStructUpgradeInfo(*this, Upgraded(*this, ELilaStringsMode::Backward, true, 0));
}

FRigUnit_LilaStringsIK_Execute()
{
	TRACE_CPUPROFILER_EVENT_SCOPE(RigUnit_LilaStringsIK::Execute);

	Run(ExecuteContext.Hierarchy, EMode::Ik, LegacyAsk({RuleWeb, Bone, Control, Target, bHoldContacts, Alpha, bRotate, ChainLength}), Error,
		[&](const FString& Message) { UE_CONTROLRIG_RIGUNIT_REPORT_WARNING(TEXT("%s"), *Message); });
}

FRigVMStructUpgradeInfo FRigUnit_LilaStringsIK::GetUpgradeInfo() const
{
	return FRigVMStructUpgradeInfo(*this, Upgraded(*this, ELilaStringsMode::IK, bRotate, ChainLength));
}

FRigUnit_LilaStringsFullBody_Execute()
{
	TRACE_CPUPROFILER_EVENT_SCOPE(RigUnit_LilaStringsFullBody::Execute);

	Run(ExecuteContext.Hierarchy, EMode::FullBody, LegacyAsk({RuleWeb, Bone, Control, Target, bHoldContacts, Alpha, bRotate, 0}), Error,
		[&](const FString& Message) { UE_CONTROLRIG_RIGUNIT_REPORT_WARNING(TEXT("%s"), *Message); });
}

FRigVMStructUpgradeInfo FRigUnit_LilaStringsFullBody::GetUpgradeInfo() const
{
	return FRigVMStructUpgradeInfo(*this, Upgraded(*this, ELilaStringsMode::FullBody, bRotate, 0));
}

TMap<FName, FTransform> ULilaStringsLibrary::SolveOnMesh(USkeletalMesh* Mesh, const FString& RuleWeb, const FString& Mode, const FName Bone, const FTransform& Target,
														 const bool bRotate, const bool bHoldContacts, const int32 ChainLength, const FVector TurnVector, float& Error,
														 const bool bIsolateRotation)
{
	TRACE_CPUPROFILER_EVENT_SCOPE(LilaStringsLibrary::SolveOnMesh);

	TMap<FName, FTransform> Out;
	Error = -1.0f;
	if (!Mesh)
	{
		return Out;
	}
	const TOptional<EMode> Found = ModeNamed(Mode);
	if (!Found)
	{
		UE_LOG(LogTemp, Warning, TEXT("Lila Strings: no mode %s (default, free, forward, forwardAll, backward, backwardAll, bidirectional, all, ik, fullBody)"), *Mode);
		return Out;
	}
	URigHierarchy* H = NewObject<URigHierarchy>(GetTransientPackage());
	H->GetController(true)->ImportBones(Mesh->GetRefSkeleton(), NAME_None, true, true, false, false);
	FTransform Goal = Target;
	if (const double Angle = TurnVector.Size(); Angle > 0.0)
	{
		Goal.SetRotation(FQuat(TurnVector / Angle, Angle) * H->GetGlobalTransform(FRigElementKey(Bone, ERigElementType::Bone)).GetRotation());
	}
	FAsk Ask{RuleWeb, Bone, NAME_None, Goal, bHoldContacts, 1.0f, bRotate, ChainLength};
	Ask.bIsolate = bIsolateRotation;
	if (Run(H, *Found, Ask, Error,
			[](const FString& Message) { UE_LOG(LogTemp, Warning, TEXT("Lila Strings: %s"), *Message); }))
	{
		H->ForEach<FRigBoneElement>([&](FRigBoneElement* E) {
			Out.Add(E->GetFName(), H->GetGlobalTransform(E->GetKey()));
			return true;
		});
	}
	return Out;
}

bool ULilaStringsLibrary::PlaceControlAtBone(URigHierarchy* Hierarchy, const FName Control, const FName Bone)
{
	const FRigElementKey ControlKey(Control, ERigElementType::Control);
	if (!Hierarchy || !Hierarchy->Contains(ControlKey) || !Hierarchy->Contains(BoneKey(Bone)))
	{
		return false;
	}
	const FTransform Rest = Hierarchy->GetGlobalTransform(BoneKey(Bone), true);
	Hierarchy->SetGlobalTransform(ControlKey, Rest, true, false);
	Hierarchy->SetGlobalTransform(ControlKey, Rest, false, false);
	return true;
}

namespace
{
/** The section's channel for a bool control, from its parameter curves (a protected property, reached by reflection). */
FMovieSceneBoolChannel* BoolChannelOf(UMovieSceneSection* Section, const FName Control)
{
	const FProperty* Curves = Section ? Section->GetClass()->FindPropertyByName(TEXT("BoolParameterNamesAndCurves")) : nullptr;
	if (!Curves)
	{
		return nullptr;
	}
	for (FBoolParameterNameAndCurve& C : *Curves->ContainerPtrToValuePtr<TArray<FBoolParameterNameAndCurve>>(Section))
	{
		if (C.ParameterName == Control)
		{
			return &C.ParameterCurve;
		}
	}
	return nullptr;
}

FFrameNumber TickOf(const UMovieSceneSection* Section, const int32 Frame)
{
	const UMovieScene* Scene = Section->GetTypedOuter<UMovieScene>();
	return FFrameRate::TransformTime(FFrameTime(Frame), Scene->GetDisplayRate(), Scene->GetTickResolution()).RoundToFrame();
}
} // namespace

bool ULilaStringsLibrary::KeyRigBools(UMovieSceneSection* Section, const FName Control, const TArray<int32>& Frames, const TArray<bool>& Values)
{
	FMovieSceneBoolChannel* Channel = BoolChannelOf(Section, Control);
	if (!Channel || Frames.Num() != Values.Num())
	{
		return false;
	}
	Section->Modify();
	TMovieSceneChannelData<bool> Data = Channel->GetData();
	Data.Reset();
	for (int32 I = 0; I < Frames.Num(); ++I)
	{
		Data.UpdateOrAddKey(TickOf(Section, Frames[I]), Values[I]);
	}
	return true;
}

TArray<bool> ULilaStringsLibrary::RigBools(UMovieSceneSection* Section, const FName Control, const TArray<int32>& Frames)
{
	TArray<bool> Out;
	if (FMovieSceneBoolChannel* Channel = BoolChannelOf(Section, Control))
	{
		for (const int32 F : Frames)
		{
			bool Value = false;
			Channel->Evaluate(FFrameTime(TickOf(Section, F)), Value);
			Out.Add(Value);
		}
	}
	return Out;
}

bool ULilaStringsLibrary::PlaceControl(URigHierarchy* Hierarchy, const FName Control, const FVector Location, const FQuat Rotation)
{
	const FRigElementKey Key(Control, ERigElementType::Control);
	if (!Hierarchy || !Hierarchy->Contains(Key))
	{
		return false;
	}
	const FTransform T(Rotation.GetNormalized(), Location);
	Hierarchy->SetGlobalTransform(Key, T, true, false);
	Hierarchy->SetGlobalTransform(Key, T, false, false);
	return true;
}

bool ULilaStringsLibrary::SetModeEnum(URigHierarchy* Hierarchy, const FRigElementKey Channel)
{
	const FRigControlElement* Control = Hierarchy ? Hierarchy->Find<FRigControlElement>(Channel) : nullptr;
	if (!Control)
	{
		return false;
	}
	FRigControlSettings Settings = Control->Settings;
	Settings.ControlType = ERigControlType::Integer;
	Settings.ControlEnum = StaticEnum<ELilaStringsMode>();
	Hierarchy->SetControlSettings(Channel, Settings, false, true);
	return true;
}

TMap<FName, FTransform> ULilaStringsLibrary::DragRig(UClass* RigClass, const FName Control, const FTransform& Target, const FVector TurnVector)
{
	return DragRigSwitched(RigClass, Control, Target, TurnVector, {});
}

bool ULilaStringsLibrary::PoseMesh(UPoseableMeshComponent* Mesh, const TMap<FName, FTransform>& ComponentSpace)
{
	const USkinnedAsset* Asset = Mesh ? Mesh->GetSkinnedAsset() : nullptr;
	if (!Asset)
	{
		return false;
	}
	const FReferenceSkeleton& Ref = Asset->GetRefSkeleton();
	for (int32 I = 0; I < Ref.GetNum(); ++I)
	{
		if (const FTransform* T = ComponentSpace.Find(Ref.GetBoneName(I)))
		{
			Mesh->SetBoneTransformByName(Ref.GetBoneName(I), *T, EBoneSpaces::ComponentSpace);
		}
	}
	Mesh->RefreshBoneTransforms();
	return true;
}

TMap<FName, FTransform> ULilaStringsLibrary::DragRigOn(UClass* RigClass, USceneComponent* Owner, const FName Control, const FTransform& Target,
													  const FVector TurnVector)
{
	TRACE_CPUPROFILER_EVENT_SCOPE(LilaStringsLibrary::DragRigOn);

	TMap<FName, FTransform> Out;
	if (!RigClass || !RigClass->IsChildOf(UControlRig::StaticClass()) || !Owner)
	{
		return Out;
	}
	UControlRig* Rig = NewObject<UControlRig>(GetTransientPackage(), RigClass);
	Rig->SetObjectBinding(MakeShared<FControlRigObjectBinding>());
	Rig->GetObjectBinding()->BindToObject(Owner);
	// As the animation graph does for a rig on a mesh: what the rig's nodes find as the component they run on.
	Rig->GetDataSourceRegistry()->RegisterDataSource(UControlRig::OwnerComponent, Owner);
	Rig->Initialize(true);
	Rig->Execute(FRigUnit_BeginExecution::EventName);
	URigHierarchy* H = Rig->GetHierarchy();
	const FRigElementKey Key(Control, ERigElementType::Control);
	if (H && !Control.IsNone() && H->Contains(Key))
	{
		FTransform Goal = Target;
		if (const double Angle = TurnVector.Size(); Angle > 0.0)
		{
			Goal.SetRotation(FQuat(TurnVector / Angle, Angle) * H->GetGlobalTransform(Key).GetRotation());
		}
		H->SetGlobalTransform(Key, Goal, false, true);
	}
	// Each evaluation starts from the input pose, as a frame of animation does: the bones back at rest.
	if (H)
	{
		H->ResetPoseToInitial(ERigElementType::Bone);
	}
	Rig->Execute(FRigUnit_BeginExecution::EventName);
	if (H)
	{
		H->ForEach<FRigBoneElement>([&](FRigBoneElement* E) {
			Out.Add(E->GetFName(), H->GetGlobalTransform(E->GetKey()));
			return true;
		});
		H->ForEach<FRigControlElement>([&](FRigControlElement* E) {
			if (E->GetFName().ToString().StartsWith(TEXT("LS_x_")))
			{
				Out.Add(E->GetFName(), H->GetGlobalTransform(E->GetKey()));
			}
			return true;
		});
	}
	return Out;
}

TMap<FName, FTransform> ULilaStringsLibrary::DragRigSwitched(UClass* RigClass, const FName Control, const FTransform& Target, const FVector TurnVector,
															 const TMap<FString, bool>& Switches)
{
	TRACE_CPUPROFILER_EVENT_SCOPE(LilaStringsLibrary::DragRigSwitched);

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
	for (const TPair<FString, bool>& S : Switches)
	{
		FString Owner, Name;
		if (S.Key.Split(TEXT("/"), &Owner, &Name))
		{
			if (const FRigControlElement* Channel = ChannelOf(H, FRigElementKey(FName(*Owner), ERigElementType::Control), FName(*Name)))
			{
				H->SetControlValue(Channel->GetKey(), FRigControlValue::Make<bool>(S.Value), ERigControlValueType::Current);
			}
			else
			{
				UE_LOG(LogTemp, Warning, TEXT("Lila Strings: no channel %s"), *S.Key);
			}
		}
	}
	const FRigElementKey Key(Control, ERigElementType::Control);
	FTransform Goal = Target;
	if (const double Angle = TurnVector.Size(); Angle > 0.0)
	{
		Goal.SetRotation(FQuat(TurnVector / Angle, Angle) * H->GetGlobalTransform(Key).GetRotation());
	}
	H->SetGlobalTransform(Key, Goal, false, true);
	Rig->Execute(FRigUnit_BeginExecution::EventName);
	H->ForEach<FRigBoneElement>([&](FRigBoneElement* E) {
		Out.Add(E->GetFName(), H->GetGlobalTransform(E->GetKey()));
		return true;
	});
	// The glued props' and other characters' controls too, under their own names.
	H->ForEach<FRigControlElement>([&](FRigControlElement* E) {
		if (E->GetFName().ToString().StartsWith(TEXT("LS_x_")))
		{
			Out.Add(E->GetFName(), H->GetGlobalTransform(E->GetKey()));
		}
		return true;
	});
	return Out;
}
