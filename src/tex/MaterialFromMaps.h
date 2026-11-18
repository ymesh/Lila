#pragma once

#include "core/Export.h"
#include "tex/Graph.h"
#include "tex/TextureSet.h"

#include <filesystem>
#include <optional>
#include <string>
#include <vector>

/** Materials from a set of texture maps: which channel each file is, by the tokens in its name, and a graph wiring them. */
namespace lila::tex::maps
{
/** What a file holds: a channel, or a packed map whose r, g, b each hold one. */
struct MapRole
{
    /** baseColor, roughness, metallic, normal, height, ao, emissive, opacity; or "packed". */
    std::string channel;
    /** packed: the channels in r, g and b (empty where unused). */
    std::vector<std::string> packed;
    /** normal: DirectX's convention (green down), flipped to OpenGL's. */
    bool directX = false;
    /** roughness from a glossiness map: inverted. */
    bool gloss = false;
};

/** The role a file name says, by its tokens (BaseColor, albedo, diff, rough, gloss, nrm, nor_dx, disp, ao, ORM, ARM...). */
LILA_SCENE_API std::optional<MapRole> classify(const std::string& fileName);
/** A physical size in a file name (200cm, 2m, 1.5x1.5m), in cm; 0 when it has none. */
LILA_SCENE_API double sizeFromName(const std::string& fileName);

struct MapMaterial
{
    std::string name;
    std::filesystem::path folder;
    std::vector<std::pair<std::filesystem::path, MapRole>> maps;
    double scaleCm = 0.0;
    std::vector<std::string> warnings;
};

/** Groups map files into materials: files in one folder whose names differ only by the channel token. Unknown files are listed as warnings. */
LILA_SCENE_API std::vector<MapMaterial> group(const std::vector<std::filesystem::path>& files);

/** The material graph: an image node per map into the Material Output, packed maps split, DirectX normals flipped, gloss inverted. */
LILA_SCENE_API Graph buildGraph(const MapMaterial& m);
/** Default settings plus the channels the maps add (opacity). */
LILA_SCENE_API SetSettings settingsFor(const MapMaterial& m);

/** Registers texMaterialFromMaps (called by tex::registerCommands). */
LILA_SCENE_API void registerCommands();
} // namespace lila::tex::maps
