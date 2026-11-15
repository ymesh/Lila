#pragma once

#include "core/Export.h"

#include "tex/BrushPresets.h"
#include "tex/PainterFile.h"
#include "tex/SmartMaterial.h"

#include <filesystem>
#include <map>
#include <string>
#include <vector>

/**
 * Substance 3D Painter smart materials (.spsm) as Lila smart materials: groups become folders, fill layers fill or
 * procedural layers, effects masks and filter layers, Painter's mask generators, noises and filters their nearest Lila
 * nodes. Substance graphs Lila has no counterpart for (custom .sbsar materials, painted strokes) can't come across; the
 * report names every one, and every approximation.
 */
namespace lila::tex::painter
{
struct Conversion
{
    /** Smart materials and smart masks. */
    std::vector<SmartMaterial> materials;
    /** Brush presets (.sppr), ids under "Imported/". */
    std::vector<brushes::Preset> brushes;
    /** What came across only approximately, and what was left out, by description, with how often. */
    std::map<std::string, int> approximated;
    std::map<std::string, int> dropped;
    /** Embedded images the materials reference, by the path they reference them at. */
    std::map<std::filesystem::path, image::Image> images;
};

/**
 * The file's presets named after name (several presets: "name", "name 2", ...): smart materials (.spsm), smart masks
 * (.spmsk) or brush presets (.sppr; settings come across, the tip becomes the nearest library alpha). Images go under
 * imageDir. Throws std::runtime_error.
 */
LILA_SCENE_API Conversion convertSpsm(const SpsmFile& file, const std::string& name, const std::filesystem::path& imageDir);

/** Whether a file is one texImportPainterSmartMaterials reads: .spsm, .spmsk or .sppr. */
LILA_SCENE_API bool isPainterFile(const std::filesystem::path& file);

/** texImportPainterSmartMaterials (called by registerCommands). */
LILA_SCENE_API void registerPainterCommands();
} // namespace lila::tex::painter
