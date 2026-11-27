#pragma once

// What the cliff nodes and commands share: the Terrain Cliffs node's entries, its published input, and the checks
// terrainCheck runs over them.

#include "Cliffs.h"

#include <nlohmann/json.hpp>

#include <memory>
#include <string>

namespace terrain::cliffs
{
inline constexpr const char* kCliffsType = "TerrainCliffs";
inline constexpr const char* kCliffsNode = "Cliffs";

/** The ground the Terrain Cliffs node of a modifier saw on its last evaluation, before any cliff edit. */
void publishGround(uint64_t modifier, std::shared_ptr<const Heightfield> h);
/** Forgets the grounds and results of modifiers no longer in the scene. */
void forgetGoneCliffs();
std::shared_ptr<const Heightfield> publishedGround(uint64_t modifier);

/** What a cliff node of a modifier reported on its last evaluation; null before one. */
nlohmann::json cliffNodeResult(uint64_t owner, const std::string& node);

/** The Terrain Cliffs, Cliff Wall, Cliff Scatter and Cliff Kit nodes. */
bool registerCliffNodes(lila::ext::Context& ctx);
/** terrainCliffs, terrainCliffMesh, terrainCliffExtrude, terrainCliffKit, terrainCliffScatter, terrainSendCliffs and their UI. */
void registerCliffCommands(lila::ext::Context& ctx);
void registerCliffUi(lila::ext::Context& ctx);

/**
 * terrainCheck's cliff issues for a terrain: cliff pieces floating above the ground (floatingCliff), cracks where a cliff
 * meets the terrain (cliffCrack), and World Partition tiles over a triangle budget (tileOverBudget). world places it.
 */
nlohmann::json cliffIssues(const std::string& terrain, double tolerance, double triangleBudget, double cellSize);

/** terrainSendToUnreal cliffs=True: terrainSendCliffs with the send's own flags; null when the terrain has no cliffs. */
nlohmann::json sendCliffsWith(const std::string& terrain, const nlohmann::json& flags);
} // namespace terrain::cliffs
