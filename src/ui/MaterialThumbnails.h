#pragma once

#include "core/Export.h"

#include <nlohmann/json.hpp>

#include <filesystem>
#include <functional>
#include <string>

/**
 * Material thumbnails: a library material, smart material or texture set rendered offscreen on the preview ball (a stepped
 * sphere on a pedestal whose baked mesh maps put wear and dirt on its edges and crevices), or on a mesh of the scene at
 * the material's real-world scale. Files are cached by graph hash under
 * <config>/cache/materialThumbs, so an unchanged material renders once. Works in batch mode (the renderer starts headless).
 */
namespace lila::ui::materialThumbs
{
struct Options
{
    int size = 256;
    /** A mesh object to render on instead of the preview ball. */
    std::string mesh;
    /** gpu or cpu for the graph; empty picks the GPU. */
    std::string backend;
    /** Render even when the cache has it. */
    bool force = false;
    /** Also copy the image here. */
    std::filesystem::path copyTo;
};

struct Result
{
    std::filesystem::path file;
    bool cached = false;
    std::string key;
};

LILA_UI_API std::filesystem::path cacheDir();
/** The cached file for a library id when it exists (no rendering); empty otherwise. */
LILA_UI_API std::filesystem::path cachedFile(const std::string& material, int size = 256);

/** Gets the result, or an error message with an empty result. */
using Done = std::function<void(const Result& result, const std::string& error)>;

/**
 * material: a library id, a .lilamat path, a texture set name or "smart:<name>" for a smart material or mask. Resolves on
 * the calling (main) thread, then evaluates and renders on a worker; done runs on the main thread through mainq. Never
 * waits for the GPU or the render thread.
 */
LILA_UI_API void renderAsync(const std::string& material, const Options& o, Done done);

/**
 * The blocking form, for commands and batch scripts. On the main thread it keeps Qt's events flowing while it waits.
 * Throws std::runtime_error.
 */
LILA_UI_API Result render(const std::string& material, const Options& o = {});

/** renderMaterialThumbnail. */
LILA_UI_API void registerCommands();
} // namespace lila::ui::materialThumbs
