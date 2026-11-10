#pragma once

#include "groom/LookDevice.h"

#include <groomlab/look/look_engine.h>

#include <memory>
#include <string>

namespace lila::groom
{
/**
 * GroomLab's look engine on a LookDevice (Lila's Vulkan compute): look.slang's kernels compiled to SPIR-V (`shader`, a
 * path or a name in the shaders folder), dispatched with the same bindings as GroomLab's OpenGL engine. Work is recorded
 * and submitted at the end of each run, or when a result is read back. Targets are the engine's own buffers: host vertex
 * buffers and the point texture aren't taken (run throws for a point texture). No GPU timestamps (times() stays invalid).
 */
std::unique_ptr<groomlab::gpu::LookEngine> makeDeviceLookEngine(std::shared_ptr<LookDevice> device, std::string shader = "look.spv");
} // namespace lila::groom
