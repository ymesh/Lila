#pragma once

#include "core/Export.h"

#include "tex/Evaluate.h"
#include "tex/NodeDef.h"

#include <array>
#include <cstdint>
#include <functional>
#include <span>
#include <string>
#include <vector>

/**
 * Native (multi-pass) texture nodes: blur, flood fill, fill to index, distance, dilate, erode, ambient occlusion,
 * normal to height, auto levels and equalize.
 * Each has a C++ implementation the CPU evaluator runs, and a GPU form: Slang kernels in
 * <resources>/texnodes/natives/<file>.slang run in the order nativeSchedule() gives.
 *
 * GPU contract for one native pass. Its frame buffers, by index: the node's inputs in declaration order, then its
 * output, then the schedule's scratch buffers (rgba32f, their own sizes). Every step is one compute dispatch of
 * nativeKernelSource(file, entry), entry "texMain", [numthreads(8, 8, 1)], with the same four bindings as a fused pass
 * (Plan.h):
 *
 *   binding 0  gPass   res = the step's output buffer size (w, h, 1/w, 1/h); uv = NativeStep::constants (kernels take
 *                      the gU slots of the buffers a step reads from here)
 *   binding 1  gU      nativeFrame(): descriptors (asuint offset, width, height, 0) of the inputs and the output, then
 *                      each parameter in declaration order taking paramSlots(type) slots, then the scratch descriptors
 *   binding 2  gArena  every frame buffer, read
 *   binding 3  gOut    the step's output buffer (frame index NativeStep::output), written; a step never reads it
 *
 * dispatched over ceil(threadsX / 8) x ceil(threadsY / 8) groups. Steps run in order with a barrier between them. The
 * GPU result matches the CPU one within float tolerance, except flood fill, whose GPU sweeps are exact for regions they
 * converge on (any region a few row/column turns across, so bricks, tiles and cells) and the distance node, which is a
 * jump flood on the GPU and exact on the CPU. Flood fill data stores texel offsets: exact in rgba16f up to 2048 px.
 */
namespace lila::tex
{
struct NativeStep
{
    std::string entry;
    /** Frame index written. */
    int output = 0;
    int threadsX = 0;
    int threadsY = 0;
    float constants[4] = {0.0f, 0.0f, 0.0f, 0.0f};
};

struct NativeSchedule
{
    /** Kernel file stem under natives/. */
    std::string file;
    /** Scratch buffer sizes, frame indices after the output. */
    std::vector<std::array<int, 2>> scratch;
    std::vector<NativeStep> steps;
};

using NativeParamFn = std::function<float(const std::string& id, int component)>;

/** Whether type has GPU kernels. */
LILA_SCENE_API bool hasNativeKernels(const std::string& type);

/** The steps of a native pass whose output is width x height. Empty steps for an unknown type. */
LILA_SCENE_API NativeSchedule nativeSchedule(const std::string& type, int width, int height, const NativeParamFn& param);

/** The prelude, the kernel file and a texMain calling entry. Empty with *err set when the file is missing. */
LILA_SCENE_API std::string nativeKernelSource(const std::string& file, const std::string& entry, std::string* err = nullptr);

/** gU for a native pass's steps. buffers holds (arena offset, width, height) per frame buffer, in frame order. */
LILA_SCENE_API std::vector<float> nativeFrame(const NodeDef& def, const Pass& pass, std::span<const float> uniforms,
                                              const std::vector<std::array<uint32_t, 3>>& buffers);

/** Runs a native pass's GPU kernels through Slang's CPU target: a reference for the GPU evaluator. Throws on failure. */
LILA_SCENE_API void runNativeKernelsCpu(const NativeCall& call);

/** Adds the C++ implementations to the CPU evaluator's table. */
void addBuiltinNatives(std::map<std::string, NativeCpuFn>& table);
} // namespace lila::tex
