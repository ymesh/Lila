#include "LilaLinkMethods.h"

#include "LilaComponentExporter.h"
#include "LilaLinkLog.h"
#include "LilaLinkExpressions.h"
#include "LilaLinkMorphPaths.h"
#include "LilaLinkMLDeformer.h"
#include "LilaLinkGeometryCollection.h"
#include "LilaLinkMasonry.h"
#include "LilaLinkGeoreference.h"
#include "LilaLinkPlacements.h"
#include "LilaLinkRender.h"
#include "LilaLinkPanels.h"
#include "LilaLinkGroom.h"
#include "LilaLinkSeams.h"
#include "LilaLinkCrowd.h"
#include "LilaLinkSequence.h"
#include "LilaLinkImposter.h"
#include "LilaLinkLandscape.h"
#include "LilaLinkLandscapeRegions.h"
#include "LilaLinkTerrainVolume.h"
#include "LilaLinkTerrainMaterial.h"
#include "LilaLinkMethodRegistry.h"
#include "LilaLinkSplines.h"
#include "LilaLinkTerrainWater.h"
#include "LilaLinkTerrainRoads.h"
#include "LilaLinkTextures.h"
#include "LilaLinkTrim.h"
#include "LilaLinkVat.h"
#include "LilaLinkCloth.h"

#include "Animation/Skeleton.h"
#include "AssetRegistry/IAssetRegistry.h"
#include "ContentBrowserModule.h"
#include "Engine/SkeletalMesh.h"
#include "Engine/StaticMesh.h"
#include "Materials/MaterialInterface.h"
#include "PhysicsEngine/BodySetup.h"
#include "FileHelpers.h"
#include "IContentBrowserSingleton.h"
#include "IPythonScriptPlugin.h"
#include "Interfaces/IPluginManager.h"
#include "Misc/App.h"
#include "Misc/EngineVersion.h"
#include "Misc/PackageName.h"
#include "Misc/Paths.h"
#include "Modules/ModuleManager.h"
#include "ProfilingDebugging/CpuProfilerTrace.h"
#include "PythonScriptTypes.h"
#include "Rendering/SkeletalMeshRenderData.h"
#include "UObject/Package.h"

namespace
{
TSharedPtr<FJsonValue> Str(const FString& S) { return MakeShared<FJsonValueString>(S); }
TSharedPtr<FJsonValue> Num(double N) { return MakeShared<FJsonValueNumber>(N); }
TSharedPtr<FJsonValue> Obj(const TSharedRef<FJsonObject>& O) { return MakeShared<FJsonValueObject>(O); }

TArray<FString> StringArray(const TSharedPtr<FJsonObject>& Params, const TCHAR* Field)
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
	else
	{
		FString Single;
		if (Params->TryGetStringField(Field, Single))
		{
			Out.Add(Single);
		}
	}
	return Out;
}

/** "/Game/Props/SM_Crate" or "/Game/Props/SM_Crate.SM_Crate" to the object path. */
FString ToObjectPath(const FString& Path)
{
	if (Path.Contains(TEXT(".")))
	{
		return Path;
	}
	return Path + TEXT(".") + FPackageName::GetShortName(Path);
}

FAssetData FindAsset(const FString& Path)
{
	return IAssetRegistry::GetChecked().GetAssetByObjectPath(FSoftObjectPath(ToObjectPath(Path)));
}

TSharedPtr<FJsonValue> Hello(const TSharedPtr<FJsonObject>&, FString&)
{
	TRACE_CPUPROFILER_EVENT_SCOPE(LilaLinkMethods::Hello);

	const FEngineVersion& Version = FEngineVersion::Current();
	TSharedRef<FJsonObject> Engine = MakeShared<FJsonObject>();
	Engine->SetNumberField(TEXT("major"), Version.GetMajor());
	Engine->SetNumberField(TEXT("minor"), Version.GetMinor());
	Engine->SetNumberField(TEXT("patch"), Version.GetPatch());
	Engine->SetStringField(TEXT("string"), Version.ToString(EVersionComponent::Patch));

	FString PluginVersion;
	if (TSharedPtr<IPlugin> Plugin = IPluginManager::Get().FindPlugin(TEXT("LilaLink")))
	{
		PluginVersion = Plugin->GetDescriptor().VersionName;
	}

	TSharedRef<FJsonObject> Out = MakeShared<FJsonObject>();
	Out->SetObjectField(TEXT("engine"), Engine);
	Out->SetStringField(TEXT("project"), FApp::GetProjectName());
	Out->SetStringField(TEXT("projectDir"), FPaths::ConvertRelativePathToFull(FPaths::ProjectDir()));
	Out->SetStringField(TEXT("contentDir"), FPaths::ConvertRelativePathToFull(FPaths::ProjectContentDir()));
	Out->SetStringField(TEXT("pluginVersion"), PluginVersion);
	Out->SetStringField(TEXT("axisProfile"), Version.GetMajor() >= 6 ? TEXT("ue6") : TEXT("ue5"));
	Out->SetNumberField(TEXT("pid"), FPlatformProcess::GetCurrentProcessId());
	// "progress": long methods send progress notifications, so Lila can tell a slow call from a hung editor.
	Out->SetArrayField(TEXT("features"), {Str(TEXT("progress")), Str(TEXT("materials")), Str(TEXT("meshSequence")), Str(TEXT("geometryCache")),
		Str(TEXT("vertexAnimation")), Str(TEXT("textures")), Str(TEXT("materialInstances")),
		Str(TEXT("stylisedInstances")), Str(TEXT("mlDeformer")), Str(TEXT("splines")), Str(TEXT("placements")), Str(TEXT("landscape")), Str(TEXT("landscapeRegions")), Str(TEXT("terrainWater")), Str(TEXT("landscapeVolume")),
		Str(TEXT("masonryMaterials")), Str(TEXT("trimMaterials")), Str(TEXT("imposters")), Str(TEXT("components")), Str(TEXT("lights")), Str(TEXT("lightingChannels")),
		Str(TEXT("geometryCollection")), Str(TEXT("trimDecals")), Str(TEXT("physics")), Str(TEXT("levelInstances")),
		Str(TEXT("collision")), Str(TEXT("instanceData")), Str(TEXT("expressionRig")), Str(TEXT("morphPaths")), Str(TEXT("terrainRoads")), Str(TEXT("terrainMaterial")), Str(TEXT("render")), Str(TEXT("seams")), Str(TEXT("panelLines")), Str(TEXT("cliffMaterial")), Str(TEXT("cliffTiles")), Str(TEXT("groom")), Str(TEXT("georeference")), Str(TEXT("pointClouds"))});
	// Features of the optional plugins that registered methods, such as LilaLinkSettlement's "settlement".
	TArray<TSharedPtr<FJsonValue>> Features = Out->GetArrayField(TEXT("features"));
	for (const FString& Feature : LilaLink::ExternalFeatures())
	{
		Features.Add(Str(Feature));
	}
	Out->SetArrayField(TEXT("features"), Features);
	TArray<TSharedPtr<FJsonValue>> Exporters;
	for (const FString& Name : LilaLink::ComponentExporterNames())
	{
		Exporters.Add(Str(Name));
	}
	Out->SetArrayField(TEXT("componentExporters"), Exporters);
	Out->SetBoolField(TEXT("python"), IPythonScriptPlugin::Get() != nullptr && IPythonScriptPlugin::Get()->IsPythonAvailable());
	return Obj(Out);
}

TSharedPtr<FJsonValue> Ping(const TSharedPtr<FJsonObject>&, FString&)
{
	return Str(TEXT("pong"));
}

TSharedPtr<FJsonValue> SaveAssets(const TSharedPtr<FJsonObject>& Params, FString& OutError)
{
	TRACE_CPUPROFILER_EVENT_SCOPE(LilaLinkMethods::SaveAssets);

	TArray<FString> Paths = StringArray(Params, TEXT("paths"));
	bool bLevel = false, bDirty = false;
	Params->TryGetBoolField(TEXT("level"), bLevel);
	Params->TryGetBoolField(TEXT("dirty"), bDirty);
	if (Paths.IsEmpty() && !bLevel && !bDirty)
	{
		OutError = TEXT("saveAssets: give paths, level or dirty");
		return nullptr;
	}
	TArray<FString> Errors;
	if (!Paths.IsEmpty())
	{
		LilaLink::SaveAssetPaths(Paths, Errors);
	}
	if (bDirty)
	{
		if (!UEditorLoadingAndSavingUtils::SaveDirtyPackages(true, true))
		{
			Errors.Add(TEXT("Saving the unsaved packages failed; see the editor log"));
		}
	}
	else if (bLevel && !UEditorLoadingAndSavingUtils::SaveCurrentLevel())
	{
		Errors.Add(TEXT("Saving the level failed; it may never have been saved (use Save As in the editor first)"));
	}
	TSharedRef<FJsonObject> Out = MakeShared<FJsonObject>();
	TArray<TSharedPtr<FJsonValue>> ErrorValues;
	for (const FString& E : Errors)
	{
		ErrorValues.Add(Str(E));
	}
	Out->SetBoolField(TEXT("saved"), Errors.IsEmpty());
	Out->SetArrayField(TEXT("errors"), ErrorValues);
	return Obj(Out);
}

TSharedPtr<FJsonValue> ListAssets(const TSharedPtr<FJsonObject>& Params, FString& OutError)
{
	TRACE_CPUPROFILER_EVENT_SCOPE(LilaLinkMethods::ListAssets);

	FString Path = TEXT("/Game");
	Params->TryGetStringField(TEXT("path"), Path);
	bool bRecursive = true;
	Params->TryGetBoolField(TEXT("recursive"), bRecursive);

	FARFilter Filter;
	Filter.PackagePaths.Add(FName(*Path));
	Filter.bRecursivePaths = bRecursive;
	for (const FString& ClassName : StringArray(Params, TEXT("class")))
	{
		FTopLevelAssetPath ClassPath = ClassName.StartsWith(TEXT("/")) ? FTopLevelAssetPath(ClassName) : UClass::TryConvertShortTypeNameToPathName<UClass>(ClassName);
		if (ClassPath.IsNull())
		{
			OutError = FString::Printf(TEXT("listAssets: unknown class '%s'"), *ClassName);
			return nullptr;
		}
		Filter.ClassPaths.Add(ClassPath);
	}
	Filter.bRecursiveClasses = true;

	TArray<FAssetData> Assets;
	IAssetRegistry::GetChecked().GetAssets(Filter, Assets);

	TArray<TSharedPtr<FJsonValue>> Out;
	for (const FAssetData& Asset : Assets)
	{
		TSharedRef<FJsonObject> Entry = MakeShared<FJsonObject>();
		Entry->SetStringField(TEXT("path"), Asset.GetObjectPathString());
		Entry->SetStringField(TEXT("class"), Asset.AssetClassPath.GetAssetName().ToString());
		Out.Add(Obj(Entry));
	}
	return MakeShared<FJsonValueArray>(Out);
}

TSharedPtr<FJsonValue> AssetInfo(const TSharedPtr<FJsonObject>& Params, FString& OutError)
{
	TRACE_CPUPROFILER_EVENT_SCOPE(LilaLinkMethods::AssetInfo);

	FString Path;
	if (!Params->TryGetStringField(TEXT("path"), Path))
	{
		OutError = TEXT("assetInfo: give path");
		return nullptr;
	}
	FAssetData Data = FindAsset(Path);
	if (!Data.IsValid())
	{
		OutError = FString::Printf(TEXT("No asset at %s"), *Path);
		return nullptr;
	}

	TSharedRef<FJsonObject> Out = MakeShared<FJsonObject>();
	Out->SetStringField(TEXT("path"), Data.GetObjectPathString());
	Out->SetStringField(TEXT("class"), Data.AssetClassPath.GetAssetName().ToString());

	UObject* Asset = Data.GetAsset();
	TArray<TSharedPtr<FJsonValue>> Slots;
	auto AddSlot = [&Slots](FName Slot, const UMaterialInterface* Material) {
		TSharedRef<FJsonObject> Entry = MakeShared<FJsonObject>();
		Entry->SetStringField(TEXT("slot"), Slot.ToString());
		Entry->SetStringField(TEXT("material"), Material ? Material->GetPathName() : FString());
		Slots.Add(Obj(Entry));
	};
	if (UStaticMesh* Mesh = Cast<UStaticMesh>(Asset))
	{
		for (const FStaticMaterial& M : Mesh->GetStaticMaterials())
		{
			AddSlot(M.MaterialSlotName, M.MaterialInterface);
		}
	}
	else if (USkeletalMesh* Skel = Cast<USkeletalMesh>(Asset))
	{
		for (const FSkeletalMaterial& M : Skel->GetMaterials())
		{
			AddSlot(M.MaterialSlotName, M.MaterialInterface);
		}
	}
	if (!Slots.IsEmpty())
	{
		Out->SetArrayField(TEXT("materialSlots"), Slots);
	}
	if (UStaticMesh* Mesh = Cast<UStaticMesh>(Asset))
	{
		Out->SetNumberField(TEXT("lods"), Mesh->GetNumLODs());
		Out->SetNumberField(TEXT("vertices"), Mesh->GetNumLODs() > 0 ? Mesh->GetNumVertices(0) : 0);
		Out->SetNumberField(TEXT("triangles"), Mesh->GetNumLODs() > 0 ? Mesh->GetNumTriangles(0) : 0);
		Out->SetNumberField(TEXT("materials"), Mesh->GetStaticMaterials().Num());
		Out->SetBoolField(TEXT("nanite"), Mesh->IsNaniteEnabled());
		TSharedRef<FJsonObject> Collision = MakeShared<FJsonObject>();
		const UBodySetup* Body = Mesh->GetBodySetup();
		Collision->SetNumberField(TEXT("simple"), Body ? Body->AggGeom.GetElementCount() : 0);
		Collision->SetNumberField(TEXT("convex"), Body ? Body->AggGeom.ConvexElems.Num() : 0);
		Collision->SetBoolField(TEXT("complexAsSimple"), Body && Body->CollisionTraceFlag == CTF_UseComplexAsSimple);
		Out->SetObjectField(TEXT("collision"), Collision);
	}
	else if (USkeletalMesh* Skel = Cast<USkeletalMesh>(Asset))
	{
		Out->SetNumberField(TEXT("lods"), Skel->GetLODNum());
		Out->SetNumberField(TEXT("bones"), Skel->GetRefSkeleton().GetNum());
		Out->SetNumberField(TEXT("materials"), Skel->GetMaterials().Num());
		if (Skel->GetSkeleton())
		{
			Out->SetStringField(TEXT("skeleton"), Skel->GetSkeleton()->GetPathName());
		}
		FSkeletalMeshRenderData* Render = Skel->GetResourceForRendering();
		if (Render && Render->LODRenderData.Num() > 0)
		{
			Out->SetNumberField(TEXT("vertices"), Render->LODRenderData[0].GetNumVertices());
			Out->SetNumberField(TEXT("triangles"), Render->LODRenderData[0].GetTotalFaces());
		}
	}
	return Obj(Out);
}

TSharedPtr<FJsonValue> FocusAsset(const TSharedPtr<FJsonObject>& Params, FString& OutError)
{
	TRACE_CPUPROFILER_EVENT_SCOPE(LilaLinkMethods::FocusAsset);

	TArray<FAssetData> Assets;
	for (const FString& Path : StringArray(Params, TEXT("path")))
	{
		FAssetData Data = FindAsset(Path);
		if (Data.IsValid())
		{
			Assets.Add(Data);
		}
	}
	for (const FString& Path : StringArray(Params, TEXT("paths")))
	{
		FAssetData Data = FindAsset(Path);
		if (Data.IsValid())
		{
			Assets.Add(Data);
		}
	}
	if (Assets.IsEmpty())
	{
		OutError = TEXT("focusAsset: no such asset");
		return nullptr;
	}
	FContentBrowserModule& ContentBrowser = FModuleManager::LoadModuleChecked<FContentBrowserModule>(TEXT("ContentBrowser"));
	ContentBrowser.Get().SyncBrowserToAssets(Assets);
	return MakeShared<FJsonValueBoolean>(true);
}

TSharedPtr<FJsonValue> RunPython(const TSharedPtr<FJsonObject>& Params, FString& OutError)
{
	TRACE_CPUPROFILER_EVENT_SCOPE(LilaLinkMethods::RunPython);

	IPythonScriptPlugin* Python = IPythonScriptPlugin::Get();
	if (!Python || !Python->IsPythonAvailable())
	{
		OutError = TEXT("runPython: the Python Editor Script Plugin is not enabled in this project");
		return nullptr;
	}
	FString Code;
	if (!Params->TryGetStringField(TEXT("code"), Code))
	{
		OutError = TEXT("runPython: give code");
		return nullptr;
	}

	FPythonCommandEx Command;
	Command.Command = Code;
	Command.ExecutionMode = EPythonCommandExecutionMode::ExecuteFile;
	FString Mode;
	if (Params->TryGetStringField(TEXT("mode"), Mode) && Mode == TEXT("evaluate"))
	{
		Command.ExecutionMode = EPythonCommandExecutionMode::EvaluateStatement;
	}
	const bool bOk = Python->ExecPythonCommandEx(Command);

	TSharedRef<FJsonObject> Out = MakeShared<FJsonObject>();
	Out->SetBoolField(TEXT("ok"), bOk);
	Out->SetStringField(TEXT("result"), Command.CommandResult);
	TArray<TSharedPtr<FJsonValue>> Log;
	for (const FPythonLogOutputEntry& Entry : Command.LogOutput)
	{
		TSharedRef<FJsonObject> Line = MakeShared<FJsonObject>();
		Line->SetStringField(TEXT("type"), LexToString(Entry.Type));
		Line->SetStringField(TEXT("text"), Entry.Output);
		Log.Add(Obj(Line));
	}
	Out->SetArrayField(TEXT("log"), Log);
	return Obj(Out);
}

/** Frees what long batches of imports left behind; senders call it every few tiles. */
TSharedPtr<FJsonValue> CollectGarbageNow(const TSharedPtr<FJsonObject>&, FString&)
{
	TRACE_CPUPROFILER_EVENT_SCOPE(LilaLinkMethods::CollectGarbage);
	CollectGarbage(GARBAGE_COLLECTION_KEEPFLAGS, true);
	return MakeShared<FJsonValueBoolean>(true);
}
} // namespace

bool LilaLink::SaveAssetPaths(const TArray<FString>& Paths, TArray<FString>& OutErrors)
{
	TRACE_CPUPROFILER_EVENT_SCOPE(LilaLink::SaveAssetPaths);

	TArray<UPackage*> Packages;
	for (const FString& Path : Paths)
	{
		FString PackageName = FPackageName::ObjectPathToPackageName(Path);
		UPackage* Package = FindPackage(nullptr, *PackageName);
		if (!Package)
		{
			Package = LoadPackage(nullptr, *PackageName, LOAD_None);
		}
		if (Package)
		{
			Packages.AddUnique(Package);
		}
		else
		{
			OutErrors.Add(FString::Printf(TEXT("No package %s"), *PackageName));
		}
	}
	if (!Packages.IsEmpty() && !UEditorLoadingAndSavingUtils::SavePackages(Packages, false))
	{
		OutErrors.Add(TEXT("Saving failed; see the editor log"));
	}
	return OutErrors.IsEmpty();
}

TMap<FString, FLilaLinkMethod> LilaLink::MakeMethods()
{
	TMap<FString, FLilaLinkMethod> Methods;
	Methods.Add(TEXT("hello"), LilaLink::Sync(&Hello));
	Methods.Add(TEXT("ping"), LilaLink::Sync(&Ping));
	Methods.Add(TEXT("importAsset"), &LilaLink::ImportAsset);
	Methods.Add(TEXT("saveAssets"), LilaLink::Sync(&SaveAssets));
	Methods.Add(TEXT("listAssets"), LilaLink::Sync(&ListAssets));
	Methods.Add(TEXT("assetInfo"), LilaLink::Sync(&AssetInfo));
	Methods.Add(TEXT("focusAsset"), LilaLink::Sync(&FocusAsset));
	Methods.Add(TEXT("runPython"), LilaLink::Sync(&RunPython));
	Methods.Add(TEXT("collectGarbage"), LilaLink::Sync(&CollectGarbageNow));
	LilaLink::AddSequenceMethods(Methods);
	LilaLink::AddCrowdMethods(Methods);
	LilaLink::AddVatMethods(Methods);
	LilaLink::AddClothMethods(Methods);
	LilaLink::AddClothAssetMethods(Methods);
	LilaLink::AddWrinkleMethods(Methods);
	LilaLink::AddImposterMethods(Methods);
	LilaLink::AddTextureMethods(Methods);
	LilaLink::AddMLDeformerMethods(Methods);
	LilaLink::AddExpressionMethods(Methods);
	LilaLink::AddMorphPathMethods(Methods);
	LilaLink::AddSplineMethods(Methods);
	LilaLink::AddLandscapeMethods(Methods);
	LilaLink::AddLandscapeRegionMethods(Methods);
	LilaLink::AddTerrainWaterMethods(Methods);
	LilaLink::AddTerrainRoadMethods(Methods);
	LilaLink::AddTerrainVolumeMethods(Methods);
	LilaLink::AddTerrainMaterialMethods(Methods);
	LilaLink::AddPlacementMethods(Methods);
	LilaLink::AddGeoreferenceMethods(Methods);
	LilaLink::AddMasonryMethods(Methods);
	LilaLink::AddTrimMethods(Methods);
	LilaLink::AddGeometryCollectionMethods(Methods);
	LilaLink::AddRenderMethods(Methods);
	LilaLink::AddSeamMethods(Methods);
	LilaLink::AddPanelMethods(Methods);
	LilaLink::AddGroomMethods(Methods);
	return Methods;
}
