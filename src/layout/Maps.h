#pragma once

#include "core/Export.h"
#include "core/ImageIO.h"

#include "layout/Resolve.h"

#include <string>
#include <vector>

namespace lila::layout
{
struct MapOptions
{
    int resolution = 2048;
    int uvSet = 0;
    /** Lines whose grooves go into the height map, as output bits; Texture lines by default. */
    uint32_t heightOutputs = outputBit(Output::Texture);
    /** Texels of padding around each UV island. */
    int padding = 4;
};

/** The layout as textures in the mesh's UV space. Row 0 is the top (V = 1). */
struct LayoutMaps
{
    /** Grey: 1 at the surface, darker into the grooves (0 at the deepest groove). */
    image::Image height;
    /** A flat random colour per panel, black on the lines: an ID map for masking panels. */
    image::Image panelId;
    /** White on the lines, fading to black at their edges: for grime and edge wear. */
    image::Image lines;
    std::vector<std::string> warnings;
};

/** The flat colour a panel's key gets in ID maps, 0 to 1 per channel; the same key always gets the same colour. */
LILA_SCENE_API glm::vec3 panelColor(uint64_t key);

/** Bakes the maps; throws std::runtime_error when the mesh has no UVs in the set. */
LILA_SCENE_API LayoutMaps bakeMaps(const Resolved& layout, const MapOptions& opts = {});
} // namespace lila::layout
