#pragma once

#include "CoreMinimal.h"
#include "Dom/JsonObject.h"
#include "ZoneGraphTypes.h"

class UWorld;

/**
 * MassTraffic (a City Sample project plugin) reached by reflection, so LilaLinkSettlement never depends on it: its lane
 * filters, light and parking data, and the level's Mass spawners that read them.
 */
namespace LilaLinkSettlementTraffic
{
bool Present();

/**
 * Fills MassTraffic's empty lane filters with Lila's tags (MaskOf takes comma-separated tag names) and saves them to the
 * project config. For filters the project already set, Extra gets their tags under the Lila tag of that kind of lane
 * (Vehicle, Intersection, Crossing, Trunk), so Lila's lanes carry them too. Null when the project has no MassTraffic.
 */
TSharedPtr<FJsonObject> ConfigureSettings(TFunctionRef<FZoneGraphTagMask(const TCHAR*)> MaskOf, TMap<FString, FZoneGraphTagMask>& Extra);

/**
 * Writes Folder/DA_<Name>_TrafficLights (a light per approach of each signalised junction), DA_<Name>_TrafficLightTypes
 * (made once with a mesh-less type) and DA_<Name>_ParkingSpaces (Params.parking under Params.parkingType, default Car),
 * then points the level's MassTraffic intersection and parked vehicle generators that have no data of their own at them.
 */
TSharedPtr<FJsonObject> WriteData(UWorld* World, const TSharedPtr<FJsonObject>& Params, const FString& Folder, const FString& Name, TArray<FString>& Warnings);
} // namespace LilaLinkSettlementTraffic
