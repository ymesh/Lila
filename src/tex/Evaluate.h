#pragma once

#include "core/Export.h"
#include "core/ImageIO.h"

#include "tex/Plan.h"

#include <filesystem>
#include <functional>
#include <map>
#include <span>
#include <string>
#include <vector>

/**
 * The CPU reference evaluator: runs a Plan's fused passes through Slang's host-callable target (the same source the GPU
 * runs) and its native passes through the C++ implementations registered here. Works without a GPU (lilabatch, tests).
 */
namespace lila::tex
{
struct EvalOptions
{
    /** External maps by name (Plan BufferKind::External). Missing ones read as (u, v, 0, 1) at the plan's size. */
    std::map<std::string, image::Image> externals;
    /** Relative image parameter paths resolve against this (the workspace, else the scene's folder, when empty). */
    std::filesystem::path imageRoot;
    /** Where the plan's texture sits in uv: a UDIM tile's origin, (1, 0) for 1002. */
    float uvOffset[2] = {0.0f, 0.0f};
};

struct EvalResult
{
    /** By output name; rows top first, RGBA floats. Gray outputs are (v, v, v, 1). */
    std::map<std::string, image::Image> outputs;
    std::vector<std::string> warnings;
    double milliseconds = 0.0;
    /** Kernels compiled and found in the cache during this evaluation. */
    size_t compiled = 0;
    size_t cached = 0;
};

/** Throws std::runtime_error when the plan has an error or a pass fails to compile. */
LILA_SCENE_API EvalResult evaluateCpu(const Plan& plan, const EvalOptions& options = {});

/** A buffer a native pass reads or writes. */
struct NativeBuffer
{
    float* rgba = nullptr;
    int width = 0;
    int height = 0;
};

struct NativeCall
{
    const Plan& plan;
    const Pass& pass;
    /** The plan's uniforms with descriptors filled in. */
    std::span<const float> uniforms;
    std::map<std::string, NativeBuffer> inputs;
    NativeBuffer output;

    /** Component c of a parameter's first slot (0 when it has none). */
    float param(const std::string& id, int c = 0) const
    {
        auto it = pass.paramSlots.find(id);
        return it == pass.paramSlots.end() ? 0.0f : uniforms[size_t(it->second) * 4 + size_t(c)];
    }
};

using NativeCpuFn = std::function<void(const NativeCall&)>;

/** The CPU side of a native node type. The GPU evaluator registers its own. */
LILA_SCENE_API void registerNativeCpu(const std::string& type, NativeCpuFn fn);
/** The registered CPU side of a native type; empty when there is none. */
LILA_SCENE_API NativeCpuFn nativeCpu(const std::string& type);

/** An image parameter's file: absolute as given, else under root, the workspace, the scene's folder or Lila's resources. */
LILA_SCENE_API std::filesystem::path resolveImagePath(const std::string& path, const std::filesystem::path& root);

/**
 * An Image node's file as a plan samples it: no larger than the plan (detail past the set's resolution never shows), and
 * when maxTexels is above 0, scaled to hold at most that many texels. That second cut loses detail, so it sets *warning.
 */
LILA_SCENE_API image::Image fitSourceImage(image::Image img, const std::string& name, int planWidth, int planHeight, size_t maxTexels,
                                           std::string* warning);

/** The GPU evaluator the renderer installs: evaluate throws like evaluateCpu; available says whether a GPU can run it. */
struct GpuEvaluator
{
    std::function<EvalResult(const Plan&, const EvalOptions&)> evaluate;
    std::function<bool()> available;
};
LILA_SCENE_API void setGpuEvaluator(GpuEvaluator e);

/**
 * Evaluates on backend "gpu" or "cpu"; empty picks the GPU when one is installed and available, else the CPU. *used
 * receives the backend that ran. Throws std::runtime_error (also when "gpu" is asked for and there is none).
 */
LILA_SCENE_API EvalResult evaluate(const Plan& plan, const EvalOptions& options, const std::string& backend, std::string* used = nullptr);
} // namespace lila::tex
