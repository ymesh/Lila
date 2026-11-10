#include "ui/GroomGpu.h"

#include "render/GpuCompute.h"

#include <vector>

namespace lila::ui
{
namespace
{
class Recording final : public groom::LookRecorder
{
public:
    explicit Recording(gpu::Recorder& r) : mR(r) {}
    void buffer(int id, size_t bytes) override { mR.buffer(id, bytes); }
    void upload(int id, const void* data, size_t bytes, size_t offset) override { mR.upload(id, data, bytes, offset); }
    void copy(int from, int to, size_t bytes, size_t fromOffset, size_t toOffset) override { mR.copy(from, to, bytes, fromOffset, toOffset); }
    void readback(int id, size_t bytes) override { mR.readback(id, bytes); }
    void kernel(int id, const char* shader, const char* entry, const groom::LookSlot* slots, size_t count) override
    {
        std::vector<gpu::Slot> s(count);
        for (size_t i = 0; i < count; ++i)
            s[i] = {slots[i].binding, slots[i].uniform ? gpu::SlotKind::Uniform : gpu::SlotKind::Storage};
        mR.boundKernel(id, shader, entry, s);
    }
    void dispatch(int kernel, const groom::LookBind* binds, size_t count, uint32_t groupsX, uint32_t groupsY) override
    {
        std::vector<gpu::Bind> b(count);
        for (size_t i = 0; i < count; ++i)
            b[i] = {binds[i].binding, binds[i].buffer, binds[i].offset, binds[i].bytes};
        mR.dispatchBound(kernel, b, groupsX, groupsY);
    }
    void barrier() override { mR.barrier(); }

private:
    gpu::Recorder& mR;
};

class GpuLookDevice final : public groom::LookDevice
{
public:
    bool run(const std::function<void(groom::LookRecorder&)>& record, std::string* err) override
    {
        return mSession.run(
            [&](gpu::Recorder& r) {
                Recording rec(r);
                record(rec);
            },
            err);
    }
    bool read(int id, void* out, size_t bytes, size_t offset) const override { return mSession.read(id, out, bytes, offset); }
    double lastMs() const override { return mSession.lastMs(); }

private:
    gpu::Session mSession;
};
} // namespace

std::unique_ptr<groom::LookDevice> makeGpuLookDevice()
{
    if (!gpu::available())
        return nullptr;
    return std::make_unique<GpuLookDevice>();
}
} // namespace lila::ui
