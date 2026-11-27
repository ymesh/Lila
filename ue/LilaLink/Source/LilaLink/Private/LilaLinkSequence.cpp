#include "LilaLinkSequence.h"

#include "LilaLinkAssets.h"
#include "LilaLinkLog.h"
#include "LilaLinkMethods.h"

#include "AssetRegistry/IAssetRegistry.h"
#include "Engine/StaticMesh.h"
#include "Misc/PackageName.h"
#include "NiagaraEmitter.h"
#include "NiagaraEmitterHandle.h"
#include "NiagaraMeshRendererProperties.h"
#include "NiagaraSystem.h"
#include "ProfilingDebugging/CpuProfilerTrace.h"

namespace
{
FString ToObjectPath(const FString& Path)
{
	return Path.Contains(TEXT(".")) ? Path : Path + TEXT(".") + FPackageName::GetShortName(Path);
}

TArray<FString> Strings(const TSharedPtr<FJsonObject>& Params, const TCHAR* Field)
{
	TArray<FString> Out;
	const TArray<TSharedPtr<FJsonValue>>* Values = nullptr;
	if (Params->TryGetArrayField(Field, Values))
	{
		for (const TSharedPtr<FJsonValue>& V : *Values)
		{
			FString S;
			if (V.IsValid() && V->TryGetString(S))
			{
				Out.Add(S);
			}
		}
	}
	return Out;
}

TSharedPtr<FJsonValue> StringArrayValue(const TArray<FString>& In)
{
	TArray<TSharedPtr<FJsonValue>> Values;
	for (const FString& S : In)
	{
		Values.Add(MakeShared<FJsonValueString>(S));
	}
	return MakeShared<FJsonValueArray>(Values);
}

TSharedPtr<FJsonValue> DeleteAssets(const TSharedPtr<FJsonObject>& Params, FString& OutError)
{
	TRACE_CPUPROFILER_EVENT_SCOPE(LilaLinkSequence::DeleteAssets);

	TArray<FString> Paths = Strings(Params, TEXT("paths"));
	if (Paths.IsEmpty())
	{
		OutError = TEXT("deleteAssets: give paths");
		return nullptr;
	}
	TArray<UObject*> Objects;
	TArray<FString> Missing;
	for (const FString& Path : Paths)
	{
		FAssetData Data = IAssetRegistry::GetChecked().GetAssetByObjectPath(FSoftObjectPath(ToObjectPath(Path)));
		if (UObject* Asset = Data.IsValid() ? Data.GetAsset() : nullptr)
		{
			Objects.Add(Asset);
		}
		else
		{
			Missing.Add(Path);
		}
	}
	// Forced, without the confirmation dialog: references to the frames (a Niagara renderer was updated first) are cleared.
	const int32 Deleted = Objects.IsEmpty() ? 0 : LilaLink::ForceDeleteAssets(Objects);
	UE_LOG(LogLilaLink, Log, TEXT("deleteAssets: deleted %d of %d"), Deleted, Paths.Num());

	TSharedRef<FJsonObject> Out = MakeShared<FJsonObject>();
	Out->SetNumberField(TEXT("deleted"), Deleted);
	Out->SetField(TEXT("missing"), StringArrayValue(Missing));
	return MakeShared<FJsonValueObject>(Out);
}

TSharedPtr<FJsonValue> SetNiagaraMeshes(const TSharedPtr<FJsonObject>& Params, FString& OutError)
{
	TRACE_CPUPROFILER_EVENT_SCOPE(LilaLinkSequence::SetNiagaraMeshes);

	FString SystemPath;
	if (!Params->TryGetStringField(TEXT("system"), SystemPath) || SystemPath.IsEmpty())
	{
		OutError = TEXT("setNiagaraMeshes: give system");
		return nullptr;
	}
	UNiagaraSystem* System = LoadObject<UNiagaraSystem>(nullptr, *ToObjectPath(SystemPath));
	if (!System)
	{
		OutError = FString::Printf(TEXT("setNiagaraMeshes: no Niagara system at %s"), *SystemPath);
		return nullptr;
	}
	TArray<UStaticMesh*> Meshes;
	for (const FString& Path : Strings(Params, TEXT("meshes")))
	{
		UStaticMesh* Mesh = LoadObject<UStaticMesh>(nullptr, *ToObjectPath(Path));
		if (!Mesh)
		{
			OutError = FString::Printf(TEXT("setNiagaraMeshes: no static mesh at %s"), *Path);
			return nullptr;
		}
		Meshes.Add(Mesh);
	}
	if (Meshes.IsEmpty())
	{
		OutError = TEXT("setNiagaraMeshes: give meshes");
		return nullptr;
	}
	FString EmitterName;
	Params->TryGetStringField(TEXT("emitter"), EmitterName);
	int32 RendererIndex = INDEX_NONE;
	Params->TryGetNumberField(TEXT("renderer"), RendererIndex);

	const TArray<FString> Updated = LilaLink::FillNiagaraMeshRenderers(System, Meshes, EmitterName, RendererIndex);
	if (Updated.IsEmpty())
	{
		OutError = FString::Printf(TEXT("setNiagaraMeshes: %s has no mesh renderer%s"), *SystemPath,
			EmitterName.IsEmpty() ? TEXT("") : *FString::Printf(TEXT(" in emitter %s"), *EmitterName));
		return nullptr;
	}
	bool bSave = false;
	Params->TryGetBoolField(TEXT("save"), bSave);
	TArray<FString> Errors;
	if (bSave)
	{
		LilaLink::SaveAssetPaths({System->GetPathName()}, Errors);
	}
	UE_LOG(LogLilaLink, Log, TEXT("setNiagaraMeshes: %d mesh(es) on %d renderer(s) of %s"), Meshes.Num(), Updated.Num(), *SystemPath);

	TSharedRef<FJsonObject> Out = MakeShared<FJsonObject>();
	Out->SetNumberField(TEXT("renderers"), Updated.Num());
	Out->SetField(TEXT("updated"), StringArrayValue(Updated));
	Out->SetField(TEXT("errors"), StringArrayValue(Errors));
	return MakeShared<FJsonValueObject>(Out);
}
} // namespace

TArray<FString> LilaLink::FillNiagaraMeshRenderers(UNiagaraSystem* System, const TArray<UStaticMesh*>& Meshes, const FString& EmitterName, int32 RendererIndex)
{
	TRACE_CPUPROFILER_EVENT_SCOPE(LilaLinkSequence::FillNiagaraMeshRenderers);

	FProperty* MeshesProperty = FindFProperty<FProperty>(UNiagaraMeshRendererProperties::StaticClass(), GET_MEMBER_NAME_CHECKED(UNiagaraMeshRendererProperties, Meshes));
	TArray<FString> Updated;
	for (FNiagaraEmitterHandle& Handle : System->GetEmitterHandles())
	{
		if (!EmitterName.IsEmpty() && Handle.GetName().ToString() != EmitterName)
		{
			continue;
		}
		FVersionedNiagaraEmitterData* Data = Handle.GetEmitterData();
		if (!Data)
		{
			continue;
		}
		int32 MeshRendererIndex = 0;
		for (UNiagaraRendererProperties* Renderer : Data->GetRenderers())
		{
			UNiagaraMeshRendererProperties* MeshRenderer = Cast<UNiagaraMeshRendererProperties>(Renderer);
			if (!MeshRenderer)
			{
				continue;
			}
			if (RendererIndex != INDEX_NONE && MeshRendererIndex++ != RendererIndex)
			{
				continue;
			}
			MeshRenderer->Modify();
			// Entry 0 is the template, so pivot, scale and rotation set up in Unreal survive a resync.
			const FNiagaraMeshRendererMeshProperties Template = MeshRenderer->Meshes.IsEmpty() ? FNiagaraMeshRendererMeshProperties() : MeshRenderer->Meshes[0];
			MeshRenderer->bEnableMeshFlipbook = false;
			MeshRenderer->Meshes.Reset(Meshes.Num());
			for (UStaticMesh* Mesh : Meshes)
			{
				FNiagaraMeshRendererMeshProperties& Entry = MeshRenderer->Meshes.Add_GetRef(Template);
				Entry.Mesh = Mesh;
			}
			FPropertyChangedEvent Event(MeshesProperty, EPropertyChangeType::ValueSet);
			MeshRenderer->PostEditChangeProperty(Event);
			Updated.Add(FString::Printf(TEXT("%s/%s"), *Handle.GetName().ToString(), *MeshRenderer->GetName()));
		}
	}
	if (!Updated.IsEmpty())
	{
		System->RequestCompile(false);
		System->MarkPackageDirty();
	}
	return Updated;
}

void LilaLink::AddSequenceMethods(TMap<FString, FLilaLinkMethod>& Methods)
{
	Methods.Add(TEXT("deleteAssets"), LilaLink::Sync(&DeleteAssets));
	Methods.Add(TEXT("setNiagaraMeshes"), LilaLink::Sync(&SetNiagaraMeshes));
}
