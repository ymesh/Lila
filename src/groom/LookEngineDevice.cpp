// Adapted from GroomLab's look_engine_gl.cpp (github.com/thekesslereffect/groomlab, Apache-2.0,
// see src/groom/groomlab/LICENSE and NOTICE).
#include "groom/LookEngineDevice.h"

#include <groomlab/look/look_engine_internal.h>

#include <algorithm>
#include <cstring>
#include <functional>
#include <map>
#include <stdexcept>
#include <vector>

namespace lila::groom
{
namespace lookDevice
{
using groomlab::gpu::GpuApi;
using groomlab::gpu::kConstantSlotBytes;
using groomlab::gpu::LookEngine;
using groomlab::gpu::LookTargets;

/** Device buffer ids: each engine of a device (siblings share it) has its own range; the constants and zero slots past them. */
constexpr int kIdsPerEngine = 1024;
constexpr int kConstantsBuffer = kIdsPerEngine - 1;

const char* kKernelEntries[] = {
#define GROOMLAB_KERNEL(Name) #Name "CS",
#include "groomlab/look/look_kernels.inc"
#undef GROOMLAB_KERNEL
};

/** The bindings every kernel's layout holds: look.slang's set 0 without the point texture. */
const LookSlot kSlots[] = {{0, true}, {1}, {2}, {3}, {4}, {5}, {6}, {7}, {8}, {9}, {10}, {12}, {13}};

size_t rounded(size_t bytes)
{
    return std::max<size_t>((bytes + 15) & ~size_t(15), 16);
}

/** The device and what the engines on it share: their next id range, and the kernels made on it. */
struct Shared
{
    std::shared_ptr<LookDevice> device;
    std::string shader;
    int nextEngine = 0;
};

class DeviceLookEngine final : public LookEngine
{
public:
    explicit DeviceLookEngine(std::shared_ptr<Shared> shared) : mShared(std::move(shared)), mBase(mShared->nextEngine++ * kIdsPerEngine) {}

    GpuApi api() const override { return GpuApi::Vulkan; }
    void* device() const override { return mShared->device.get(); }
    void dropTargets() override {}
    void finish() override { flush(); }

    std::size_t videoBytes() const override
    {
        size_t n = mConstantsBytes;
        for (const auto& [id, bytes] : mSizes)
            n += bytes;
        return n;
    }

protected:
    std::unique_ptr<LookEngine> sibling() override { return std::make_unique<DeviceLookEngine>(mShared); }

    void borrow(int id, LookEngine& from, int fromId) override
    {
        auto& other = dynamic_cast<DeviceLookEngine&>(from);
        if (!other.has(fromId))
            throw std::runtime_error("GPU look: a sibling's buffer to borrow is missing");
        release(id);
        mBorrowed[id] = other.deviceId(fromId);
    }

    void write(int id, const void* data, std::size_t bytes, bool) override
    {
        make(id, bytes);
        upload(id, 0, data, bytes);
    }

    void writeRange(int id, std::size_t offset, const void* data, std::size_t bytes) override
    {
        if (!bytes || !mSizes.count(id))
            return;
        upload(id, offset, data, bytes);
    }

    void allocate(int id, std::size_t bytes) override
    {
        const auto it = mSizes.find(id);
        if (it != mSizes.end() && it->second == rounded(bytes))
            return;
        make(id, bytes);
    }

    bool has(int id) const override { return mSizes.count(id) != 0 || mBorrowed.count(id) != 0; }

    void release(int id) override
    {
        mBorrowed.erase(id);
        mSizes.erase(id);
    }

    bool bindTargets(const LookTargets& targets, bool frames, std::size_t bytes, std::size_t) override
    {
        if (targets.pointTexture)
            throw std::runtime_error("GPU look: no point texture on this device");
        for (int i = 0; i < 4; ++i)
            if (i == 0 || frames)
                allocate(kOut0 + i, bytes);
            else
                release(kOut0 + i);
        return false;
    }

    void copyToTargets(bool, std::size_t) override {}

    void copy(int from, int to, std::size_t bytes, std::size_t fromOffset, std::size_t toOffset) override
    {
        if (!bytes)
            return;
        const int a = deviceId(from), b = deviceId(to);
        mOps.push_back([=](LookRecorder& r) {
            r.copy(a, b, bytes, fromOffset, toOffset);
            r.barrier();
        });
    }

    void setConstants(const void* slots, std::size_t count, std::size_t slotBytes) override
    {
        // A slot of kConstantSlotBytes per dispatch, bound by range: 256 is every device's uniform offset alignment or a
        // multiple of it.
        auto bytes = std::make_shared<std::vector<std::uint8_t>>(std::max<size_t>(count, 1) * kConstantSlotBytes, 0);
        for (std::size_t i = 0; i < count; ++i)
            std::memcpy(bytes->data() + i * kConstantSlotBytes, static_cast<const char*>(slots) + i * slotBytes, slotBytes);
        const int id = mBase + kConstantsBuffer;
        const size_t size = rounded(bytes->size());
        const bool grow = size > mConstantsBytes;
        mConstantsBytes = std::max(mConstantsBytes, size);
        const size_t made = mConstantsBytes;
        mOps.push_back([=](LookRecorder& r) {
            if (grow)
                r.buffer(id, made);
            r.upload(id, bytes->data(), bytes->size(), 0);
            r.barrier();
        });
    }

    void dispatch(Kernel kernel, std::size_t slot, const Binding& b, std::size_t groupsX, std::size_t groupsY) override
    {
        if (b.texture)
            throw std::runtime_error("GPU look: no point texture on this device");
        if (!groupsX || !groupsY)
            return;
        std::vector<LookBind> binds;
        binds.push_back({0, mBase + kConstantsBuffer, slot * kConstantSlotBytes, sizeof(groomlab::gpu::StageConstants)});
        auto bind = [&](uint32_t binding, int id) { binds.push_back({binding, id >= 0 && has(id) ? deviceId(id) : -1, 0, 0}); };
        bind(1, b.offsets >= 0 ? b.offsets : static_cast<int>(kOffsets));
        bind(2, kStrandData);
        bind(3, b.in);
        bind(4, b.out[0]);
        bind(5, b.out[1]);
        bind(6, b.out[2]);
        bind(7, kRamps);
        bind(8, kFields);
        bind(9, b.aux[0]);
        bind(10, b.aux[1]);
        bind(12, b.aux[2]);
        bind(13, b.out[0] == kPointStrands ? -1 : static_cast<int>(kPointStrands));
        const int k = int(kernel);
        const uint32_t gx = uint32_t(groupsX), gy = uint32_t(groupsY);
        mOps.push_back([this, k, binds = std::move(binds), gx, gy](LookRecorder& r) {
            useKernel(r, k);
            r.dispatch(k, binds.data(), binds.size(), gx, gy);
            r.barrier();
        });
    }

    void beginRun() override {}
    void stamp(std::size_t) override {}
    void endRun(std::size_t) override { flush(); }
    bool stampTicks(bool, std::vector<std::uint64_t>*, double*) override { return false; }

    std::vector<std::uint8_t> readBack(int buffer, std::size_t bytes) override
    {
        std::vector<std::uint8_t> out(bytes);
        if (!bytes)
            return out;
        const int id = deviceId(buffer);
        mOps.push_back([=](LookRecorder& r) { r.readback(id, bytes); });
        flush();
        if (!mShared->device->read(id, out.data(), bytes, 0))
            throw std::runtime_error("GPU look: reading a buffer back failed");
        return out;
    }

private:
    int deviceId(int id) const
    {
        if (const auto it = mBorrowed.find(id); it != mBorrowed.end())
            return it->second;
        if (id < 0 || id >= kConstantsBuffer)
            throw std::runtime_error("GPU look: buffer number out of range");
        return mBase + id;
    }

    void make(int id, size_t bytes)
    {
        mBorrowed.erase(id);
        const size_t size = rounded(bytes);
        mSizes[id] = size;
        const int did = mBase + id;
        mOps.push_back([=](LookRecorder& r) { r.buffer(did, size); });
    }

    void upload(int id, size_t offset, const void* data, size_t bytes)
    {
        auto copy = std::make_shared<std::vector<std::uint8_t>>(static_cast<const std::uint8_t*>(data), static_cast<const std::uint8_t*>(data) + bytes);
        const int did = deviceId(id);
        mOps.push_back([=](LookRecorder& r) {
            r.upload(did, copy->data(), copy->size(), offset);
            r.barrier();
        });
    }

    void useKernel(LookRecorder& r, int k) { r.kernel(k, mShared->shader.c_str(), kKernelEntries[k], kSlots, std::size(kSlots)); }

    void flush()
    {
        if (mOps.empty())
            return;
        std::vector<std::function<void(LookRecorder&)>> ops;
        ops.swap(mOps);
        std::string err;
        if (!mShared->device->run(
                [&](LookRecorder& r) {
                    for (auto& op : ops)
                        op(r);
                },
                &err))
            throw std::runtime_error("GPU look: " + err);
    }

    std::shared_ptr<Shared> mShared;
    int mBase = 0;
    std::map<int, size_t> mSizes;
    std::map<int, int> mBorrowed;
    size_t mConstantsBytes = 0;
    std::vector<std::function<void(LookRecorder&)>> mOps;
};
} // namespace lookDevice

std::unique_ptr<groomlab::gpu::LookEngine> makeDeviceLookEngine(std::shared_ptr<LookDevice> device, std::string shader)
{
    if (!device)
        throw std::runtime_error("GPU look: no device");
    auto shared = std::make_shared<lookDevice::Shared>();
    shared->device = std::move(device);
    shared->shader = std::move(shader);
    return std::make_unique<lookDevice::DeviceLookEngine>(std::move(shared));
}
} // namespace lila::groom
