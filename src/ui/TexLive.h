#pragma once

#include "core/Export.h"

#include "render/GpuImage.h"
#include "render/RenderMesh.h"

#include <nlohmann/json.hpp>

#include <optional>
#include <string>

namespace lila::tex
{
struct Graph;
}

/**
 * Texture sets in the viewport: a set's channels evaluate on the GPU into images its material samples (by the set's
 * material name, else the set's own), so editing the graph updates the viewport live. Evaluation starts when a draw
 * first asks for the material's maps, then reruns after every edit: a small preview first (prefs /texture/previewLevels,
 * 256 and 1024 by default), then the tiles' full size. Turn it off with the general pref /texture/liveViewport. Idle
 * evaluations give their GPU memory back, previews at once and full sizes after /texture/keepEvaluatorSeconds (30).
 */
namespace lila::ui::texLive
{
/** What a texture set gives its material. Graphs make OpenGL normals, so normal maps are always green-up. */
struct Maps
{
    render::TexturePtr baseColor; // sRGB (or linear float), alpha is opacity
    render::TexturePtr normal;
    render::TexturePtr orm;       // R occlusion, G roughness, B metallic
    render::TexturePtr emissive;
    bool occlusion = false, roughness = false, metallic = false;
    /** A level has written the images; before that they are black, so draws keep the material's own maps. */
    bool ready = false;
};

/**
 * The live maps of the texture set bound to a material, starting its evaluation the first time; nullopt when no set binds
 * to it (or the renderer is not running). Main thread.
 */
LILA_UI_API std::optional<Maps> mapsFor(const std::string& material);

/** A set's channel image for a UDIM tile (its format from the channel's bits), null when the set isn't live. Main thread. */
LILA_UI_API render::GpuImageRef channelImage(const std::string& set, const std::string& channel, int udim = 1001);

/** Listens to the scene and mesh map rebakes. Call once at startup, after the renderer starts. */
LILA_UI_API void install();
/** Stops the plan worker and drops every live set. Before the renderer stops. */
LILA_UI_API void shutdown();

/**
 * Compiles, in the background and only while no real edit waits, the shaders the live set named set would need if its
 * graph became graph: a tool calls this before an edit it is about to make (a paint layer added, a channel wired), so
 * the edit plans without compiling. Nothing for a set that isn't live. Main thread.
 */
LILA_UI_API void warm(const std::string& set, const tex::Graph& graph);

/** Whether every live set has finished evaluating its latest edit. Main thread. */
LILA_UI_API bool idle();
/** Runs the main queue until idle or the timeout; false on timeout. Tests and batch scripts. */
LILA_UI_API bool waitIdle(double timeoutMs = 30000.0);
/** Per live set: material, tiles, levels with their sizes and pass statistics, the last error. */
LILA_UI_API nlohmann::json status();
} // namespace lila::ui::texLive
