#pragma once

#include <nlohmann/json.hpp>

#include <filesystem>
#include <string>

/**
 * The placeholder edge decal sheets: two .lilatrim decal sheets painted procedurally in Lila, one realistic (chipped
 * paint, worn corners, cracks, dirt, moss) and one hand-painted toon look with flat colours and hard outlines. Each has
 * eight rows of two strips, tagged edge plus chips, worn, crack, dirt or moss, so edgeDecals finds the right strips for
 * convex and concave edges. Written under <user prefs>/weathering-decals on first use.
 */
namespace weathering::edges
{
/** Writes both sheets when missing or out of date (or force). Returns {dir, built, realistic, toon} with the sheet files. */
nlohmann::json writeEdgeSheets(bool force);

/** The placeholder sheet file for a style ("realistic" or "toon"), written first when needed. */
std::filesystem::path edgeSheet(const std::string& style);
} // namespace weathering::edges
