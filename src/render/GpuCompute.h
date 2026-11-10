#pragma once

#include "core/Export.h"

#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <span>
#include <string>

/**
 * GPU compute for extensions, without Vulkan types. Buffers are reached from Slang through device addresses (pass them
 * in a buffer or the push constants and declare pointers), kernels are entry points of shaders compiled next to the
 * executable (lila_add_shaders), and each Session::run records one submission on the render thread and waits for it.
 *
 *     gpu::Session s;
 *     s.run([&](gpu::Recorder& r) {
 *         r.kernel(0, "my_shader.spv", "mainCS", sizeof(Push));
 *         Push push{r.buffer(0, bytes), n};
 *         r.upload(0, data, bytes);
 *         r.dispatch(0, &push, (n + 63) / 64);
 *         r.barrier();
 *         r.readback(0, bytes);
 *     });
 *     s.read(0, out, bytes);
 */
namespace lila::gpu
{
/** True while the renderer runs; nothing here works without it (batch runs have none). */
LILA_UI_API bool available();
/** True when kernels may use double (shaderFloat64); false when the renderer isn't running. */
LILA_UI_API bool supportsFloat64();

enum class BufferKind : uint8_t
{
    Storage,
    Indirect, // also holds dispatchIndirect arguments: three uint group counts at a 4-byte aligned offset
};

class Recorder;

/** A resource a bound kernel reads through a descriptor (a shader written for D3D or OpenGL style bindings, set 0). */
enum class SlotKind : uint8_t
{
    Uniform,
    Storage,
};

struct Slot
{
    uint32_t binding = 0;
    SlotKind kind = SlotKind::Storage;
};

/** What a bound dispatch puts at a binding: a buffer (whole, or bytes from offset), or -1 for a small zeroed one. */
struct Bind
{
    uint32_t binding = 0;
    int buffer = -1;
    size_t offset = 0;
    /** 0: to the end of the buffer. */
    size_t bytes = 0;
};

/**
 * Owns buffers and kernels across runs, named by small non-negative ints the caller picks. Freed when the session is
 * destroyed or the renderer stops (then valid() turns false and buffers must be made again). Thread safe; runs of one
 * session are serialised.
 */
class LILA_UI_API Session
{
public:
    Session();
    ~Session();
    Session(const Session&) = delete;
    Session& operator=(const Session&) = delete;

    /**
     * Calls record on the render thread, submits what it recorded and waits for the GPU. Never call it from the render
     * thread. False with err when the renderer isn't running, record threw, or the GPU failed.
     */
    bool run(const std::function<void(Recorder&)>& record, std::string* err = nullptr);
    /** False until a run made buffers, and again after the renderer stopped. */
    bool valid() const;
    /** Copies what Recorder::readback captured of buffer id in the last run. False when there is no such capture. */
    bool read(int id, void* out, size_t bytes, size_t offset = 0) const;
    /** GPU time of the last run, ms. */
    double lastMs() const;

    struct Impl;

private:
    std::shared_ptr<Impl> impl_;
};

/** Valid only inside Session::run, on the render thread. Work runs in the order recorded; barrier() orders it. */
class LILA_UI_API Recorder
{
public:
    /** Buffer id with at least bytes, kept (with its contents) when it already is that kind and size, else made anew
     *  with undefined contents. Returns its device address. */
    uint64_t buffer(int id, size_t bytes, BufferKind kind = BufferKind::Storage);
    /** 0 when there is no buffer id. */
    uint64_t address(int id) const;
    size_t size(int id) const;
    /** Copies host data into the buffer when the work recorded so far has run. */
    void upload(int id, const void* data, size_t bytes, size_t offset = 0);
    /** Sets every 4-byte word to value. */
    void fill(int id, uint32_t value);
    void copy(int from, int to, size_t bytes, size_t fromOffset = 0, size_t toOffset = 0);
    /** After the run, Session::read returns the first bytes of buffer id as they were at this point. */
    void readback(int id, size_t bytes);

    /** Kernel id: entry of a compiled shader, taking pushBytes of push constants (at most 128). shader is a file name in
     *  the shaders folder next to lila.exe, or an absolute path (an SDK extension's own folder, from ext::Context::root).
     *  Loaded once per session. Throws std::runtime_error when the file or entry point is missing. */
    void kernel(int id, const char* shader, const char* entry, uint32_t pushBytes);
    void dispatch(int kernel, const void* push, uint32_t groupsX, uint32_t groupsY = 1, uint32_t groupsZ = 1);
    void dispatchIndirect(int kernel, const void* push, int argsBuffer, size_t offset);
    /** Kernel id reading its buffers through descriptors at slots (set 0), no push constants. Loaded once per session. */
    void boundKernel(int id, const char* shader, const char* entry, std::span<const Slot> slots);
    /** Dispatches a bound kernel with its slots filled from binds; a slot left out gets a small zeroed buffer. */
    void dispatchBound(int kernel, std::span<const Bind> binds, uint32_t groupsX, uint32_t groupsY = 1, uint32_t groupsZ = 1);
    /** The smallest step a uniform buffer offset may take on this device. */
    size_t uniformAlignment() const;
    /** Every write recorded before is visible to all work recorded after, indirect arguments included. */
    void barrier();
    /** The largest group count of one dispatch dimension. */
    static constexpr uint32_t maxGroups = 65535;

private:
    friend class Session;
    explicit Recorder(void* state) : state_(state) {}
    void* state_;
};
} // namespace lila::gpu
