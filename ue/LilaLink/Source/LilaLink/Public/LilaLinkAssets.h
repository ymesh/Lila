#pragma once

#include "CoreMinimal.h"

class UObject;

/** Asset deletion for LilaLink and its feature plugins. */
namespace LilaLink
{
/**
 * Force-deletes Objects (assets) without a dialog, package files included, and returns how many went.
 *
 * An asset loaded in the same call keeps EInternalObjectFlags::LoaderImport until the loader's deferred package cleanup
 * runs (the next async loading tick or the start of a GC). ObjectTools::ForceDeleteObjects counts that flag as a
 * reference and keeps the package's file, then its own GC unloads the package: the asset would vanish from memory and
 * stay on disk. A GC first releases such assets; a file left anyway is deleted directly, with a warning in the log.
 * OutFailed (optional) gets the package names that are still on disk.
 */
LILALINK_API int32 ForceDeleteAssets(const TArray<UObject*>& Objects, TArray<FString>* OutFailed = nullptr);
} // namespace LilaLink
