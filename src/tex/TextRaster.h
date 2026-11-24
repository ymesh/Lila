#pragma once

#include "core/Export.h"
#include "core/ImageIO.h"

#include "tex/Types.h"

#include <map>
#include <memory>
#include <string>
#include <string_view>

/**
 * The Text node's words as a coverage image: the planner names the image by the node's values ("text:" and their JSON),
 * and both evaluators draw it from that name, so the CPU and the GPU sample the same texels.
 */
namespace lila::tex
{
inline constexpr std::string_view kTextSourcePrefix = "text:";

/** Rasters wider or taller than this are drawn at this size and sampled up. */
inline constexpr int kMaxTextRaster = 4096;

/** The image name a Text node's resolved values make for a plan of width x height. */
LILA_SCENE_API std::string textSourceName(const std::map<std::string, json>& values, int width, int height);

/**
 * Draws a text source: coverage in every channel, rows top first, anti-aliased (exact horizontally, 8 rows of samples
 * per texel vertically). The last few drawn are kept, so the same name returns the same image without drawing again. An
 * image with no texels and *error set on a malformed name.
 */
LILA_SCENE_API std::shared_ptr<const image::Image> textSourceImage(std::string_view name, std::string* error = nullptr);
} // namespace lila::tex
