#include "LilaLinkAssets.h"

#include "LilaLinkLog.h"

#include "AssetRegistry/IAssetRegistry.h"
#include "HAL/FileManager.h"
#include "Misc/PackageName.h"
#include "ObjectTools.h"
#include "ProfilingDebugging/CpuProfilerTrace.h"
#include "UObject/GarbageCollection.h"
#include "UObject/Package.h"
#include "UObject/UObjectHash.h"

namespace
{
/** Whether the loader still pins an object of Package for objects it may yet import. */
bool PinnedByLoader(const UPackage* Package)
{
	bool bPinned = Package->HasAnyInternalFlags(EInternalObjectFlags::LoaderImport);
	ForEachObjectWithPackage(Package, [&bPinned](UObject* Object)
	{
		bPinned = Object->HasAnyInternalFlags(EInternalObjectFlags::LoaderImport);
		return !bPinned;
	});
	return bPinned;
}
} // namespace

int32 LilaLink::ForceDeleteAssets(const TArray<UObject*>& Objects, TArray<FString>* OutFailed)
{
	TRACE_CPUPROFILER_EVENT_SCOPE(LilaLinkAssets::ForceDeleteAssets);

	TArray<UObject*> Assets;
	TArray<FString> Packages;
	bool bPinned = false;
	for (UObject* Object : Objects)
	{
		if (IsValid(Object))
		{
			Assets.AddUnique(Object);
			Packages.AddUnique(Object->GetPackage()->GetName());
			bPinned = bPinned || PinnedByLoader(Object->GetPackage());
		}
	}
	if (Assets.IsEmpty())
	{
		return 0;
	}
	if (bPinned)
	{
		// Starting a GC runs the loader's deferred cleanup, which unpins them; the assets are standalone, so they stay.
		CollectGarbage(GARBAGE_COLLECTION_KEEPFLAGS);
	}
	ObjectTools::ForceDeleteObjects(Assets, /*ShowConfirmation=*/false);

	int32 Deleted = 0;
	for (const FString& Package : Packages)
	{
		FString File;
		if (!FPackageName::DoesPackageExist(Package, &File))
		{
			++Deleted;
		}
		else if (!FindPackage(nullptr, *Package) && IFileManager::Get().Delete(*File, /*RequireExists=*/false, /*EvenReadOnly=*/true))
		{
			UE_LOG(LogLilaLink, Warning, TEXT("ForceDeleteAssets: %s was unloaded but its file stayed; deleted the file directly"), *Package);
			IAssetRegistry::GetChecked().ScanModifiedAssetFiles({File});
			++Deleted;
		}
		else if (OutFailed)
		{
			OutFailed->Add(Package);
		}
	}
	return Deleted;
}
