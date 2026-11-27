#pragma once

#include "CoreMinimal.h"

class UObject;
class UWorld;

/** Project config LilaLink and its feature plugins write: backed up before a change, shared entries reference-counted. */
namespace LilaLink
{
/**
 * Saves Settings to its project ini (TryUpdateDefaultConfigFile); when that changes the file, the file as it was is kept
 * in Saved/LilaLink/ConfigBackups first (a .absent note when it did not exist). Returns the backup's path, empty when the
 * file did not change.
 */
LILALINK_API FString UpdateProjectIni(UObject* Settings);

/**
 * Records in Config/LilaLink/LaneProfileUsers.json that User (what a send made, such as "settlement:<id>") in World's
 * level uses the ZoneGraph lane profiles Profiles, replacing what it used before; empty Profiles drops it. Users whose
 * level no longer exists are dropped too. Returns the profiles the record held that no user lists any more, for the
 * caller to take out of the ZoneGraph settings; profiles the record never held are never returned.
 */
LILALINK_API TArray<FName> SetLaneProfileUser(const UWorld* World, const FString& User, const TArray<FName>& Profiles);
} // namespace LilaLink
