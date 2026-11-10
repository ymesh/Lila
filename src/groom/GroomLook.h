#pragma once

#include "core/Export.h"
#include "scene/Graph.h"

#include <map>
#include <string>
#include <vector>

/** A groom's look nodes (the strands' Clump, Noise, Curl... after Interpolate) and their live preview (GroomLook.cpp). */
namespace lila::groom
{
/** The look node types the groom offers, in menu order. */
LILA_SCENE_API std::vector<std::string> groomLookTypes();

struct PreviewResult
{
    /** The preview ran on the GPU look engine. */
    bool gpu = false;
    /** What kept it on the CPU (no device, a node the GPU can't run...), empty on the GPU. */
    std::string why;
};

/**
 * Shows the groom with `params` on look node `node` without setting them (a slider's drag): the look chain from the first
 * changed node runs on the GPU from the strands as last grown when it can, else the graph runs on the CPU with them.
 * Previews of other nodes made before stay. Evaluates the groom. GroomLab.cpp.
 */
LILA_SCENE_API PreviewResult previewLook(scene::Graph& g, scene::NodeId groom, const std::string& node, const std::map<std::string, double>& params);
/** Ends the preview: the groom evaluates its own graph again. */
LILA_SCENE_API void endPreviewLook(scene::Graph& g, scene::NodeId groom);

/** Registers groomLook, groomPreview and groomEndPreview. */
void registerLookCommands();
} // namespace lila::groom
