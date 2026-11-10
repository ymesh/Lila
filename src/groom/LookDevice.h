#pragma once

#include "core/Export.h"

#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>

/**
 * A GPU for the groom's look engine without the renderer's types: the renderer (LilaUI) provides one over its compute
 * sessions, the groom code (LilaScene) records into it. Mirrors gpu::Session and gpu::Recorder for kernels that read their
 * buffers through descriptors (look.slang's set 0).
 */
namespace lila::groom
{
struct LookSlot
{
    uint32_t binding = 0;
    bool uniform = false;
};

struct LookBind
{
    uint32_t binding = 0;
    /** -1: a small zeroed buffer. */
    int buffer = -1;
    size_t offset = 0;
    /** 0: to the end. */
    size_t bytes = 0;
};

class LookRecorder
{
public:
    virtual ~LookRecorder() = default;
    /** Buffer id at least bytes long: kept with its contents when already that size, else made anew. */
    virtual void buffer(int id, size_t bytes) = 0;
    virtual void upload(int id, const void* data, size_t bytes, size_t offset) = 0;
    virtual void copy(int from, int to, size_t bytes, size_t fromOffset, size_t toOffset) = 0;
    virtual void readback(int id, size_t bytes) = 0;
    virtual void kernel(int id, const char* shader, const char* entry, const LookSlot* slots, size_t count) = 0;
    virtual void dispatch(int kernel, const LookBind* binds, size_t count, uint32_t groupsX, uint32_t groupsY) = 0;
    /** Every write recorded before is visible to all work recorded after. */
    virtual void barrier() = 0;
};

class LookDevice
{
public:
    virtual ~LookDevice() = default;
    /** Records with record, submits and waits. False with err when there is no GPU or it failed. */
    virtual bool run(const std::function<void(LookRecorder&)>& record, std::string* err) = 0;
    /** What readback captured of buffer id in the last run. */
    virtual bool read(int id, void* out, size_t bytes, size_t offset) const = 0;
    virtual double lastMs() const = 0;
};

using LookDeviceFactory = std::function<std::unique_ptr<LookDevice>()>;

/** Set by the renderer when it starts; null when there is no GPU (batch runs). */
LILA_SCENE_API void setLookDeviceFactory(LookDeviceFactory factory);
/** A new device, or null without a GPU. */
LILA_SCENE_API std::unique_ptr<LookDevice> makeLookDevice();
} // namespace lila::groom
