#pragma once

#include "core/Export.h"
#include "core/ImageIO.h"
#include "core/PrefStore.h"

#include <cstdint>
#include <filesystem>
#include <map>
#include <string>
#include <vector>

/**
 * Reading Substance 3D Painter's smart material files (.spsm): an HDF5 container holding one or more layer presets
 * (preset.bin, preset_1.bin, ...) and the resources they embed (resources/<hash>.sbsar, resources/<hash>.image).
 *
 * A preset is Painter's own object serialization, decoded here into JSON: every object is {"@": class name, field: value},
 * object arrays are JSON arrays. Both the older layout (dataversion 22) and the current one decode to the same shape.
 */
namespace lila::tex::painter
{
struct SpsmFile
{
    /** Decoded presets, in file order. */
    std::vector<json> presets;
    /** Embedded resources by dataset name ("<hash>.sbsar", "<hash>.image"). */
    std::map<std::string, std::vector<uint8_t>> resources;
};

/** Throws std::runtime_error with what it couldn't read. */
LILA_SCENE_API SpsmFile readSpsm(const std::filesystem::path& file);

/** The preview Painter stores with a smart material (512 px); empty when it has none. Throws std::runtime_error when the file can't be read. */
LILA_SCENE_API image::Image readSpsmPreview(const std::filesystem::path& file);

/** One preset.bin. Throws std::runtime_error. */
LILA_SCENE_API json decodePreset(const std::vector<uint8_t>& bytes);

/** An embedded image resource (LZ4 or raw pixels behind a Qt property header). Throws std::runtime_error. */
LILA_SCENE_API image::Image decodeImage(const std::vector<uint8_t>& bytes);

/** The datasets of an HDF5 file by path ("preset.bin", "resources/x.sbsar"). Contiguous, compact and unfiltered chunked
 * byte datasets only, which is what Painter writes. Throws std::runtime_error. */
LILA_SCENE_API std::map<std::string, std::vector<uint8_t>> readHdf5Datasets(const std::filesystem::path& file);
} // namespace lila::tex::painter
