#include "render/GpuCompute.h"

#include "core/Paths.h"
#include "core/Profile.h"
#include "render/Compute.h"
#include "render/Renderer.h"
#include "render/VulkanContext.h"

#include <algorithm>
#include <atomic>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <map>
#include <mutex>
#include <stdexcept>
#include <thread>
#include <vector>

namespace lila::gpu
{
namespace
{
namespace rc = render::compute;
using render::Renderer;
using render::VulkanContext;

constexpr VkShaderStageFlags kCompute = VK_SHADER_STAGE_COMPUTE_BIT;

std::atomic<std::thread::id> gRenderThread;
std::once_flag gStopHook;
std::mutex gLiveMutex;

std::vector<std::weak_ptr<Session::Impl>>& liveSessions()
{
    static std::vector<std::weak_ptr<Session::Impl>> live;
    return live;
}
} // namespace

bool available() { return Renderer::get().running(); }

bool supportsFloat64()
{
    const VulkanContext* vk = available() ? Renderer::get().context() : nullptr;
    return vk && vk->shaderFloat64;
}

struct Session::Impl
{
    struct Buf
    {
        rc::Buffer b;
        BufferKind kind = BufferKind::Storage;
    };
    struct Kern
    {
        VkPipeline pipe = VK_NULL_HANDLE;
        VkPipelineLayout layout = VK_NULL_HANDLE;
        uint32_t push = 0;
        /** Bound kernels: their slots, in binding order. */
        std::vector<Slot> slots;
    };
    /** A bound kernel's set layout and pipeline layout, shared by every kernel of the same slots. */
    struct BoundLayout
    {
        VkDescriptorSetLayout set = VK_NULL_HANDLE;
        VkPipelineLayout layout = VK_NULL_HANDLE;
    };
    struct Code
    {
        std::vector<uint32_t> words;
        uint64_t hash = 0;
    };

    mutable std::mutex mutex;
    std::map<int, Buf> bufs;
    std::map<int, rc::Buffer> readbacks;
    std::map<int, Kern> kernels;
    std::map<uint32_t, VkPipelineLayout> layouts;
    std::map<std::string, Code> code;
    std::map<std::string, BoundLayout> boundLayouts;
    /** Descriptor sets of the run being recorded: reset at each run. */
    std::vector<VkDescriptorPool> pools;
    size_t poolUsed = 0;
    uint32_t setsInPool = 0;
    rc::Buffer zero;
    rc::Submitter submit;
    bool alive = false;
    double ms = 0.0;

    /** Render thread, under mutex. Pipelines belong to the shared cache, which the renderer clears itself. */
    void release(VulkanContext& vk)
    {
        for (auto& [id, b] : bufs)
            rc::destroy(vk, b.b);
        for (auto& [id, b] : readbacks)
            rc::destroy(vk, b);
        for (auto& [bytes, l] : layouts)
            rc::destroy(vk, l);
        for (auto& [key, l] : boundLayouts)
        {
            rc::destroy(vk, l.layout);
            vkDestroyDescriptorSetLayout(vk.device, l.set, nullptr);
        }
        for (VkDescriptorPool p : pools)
            vkDestroyDescriptorPool(vk.device, p, nullptr);
        if (zero.buffer)
            rc::destroy(vk, zero);
        boundLayouts.clear();
        pools.clear();
        poolUsed = 0;
        setsInPool = 0;
        bufs.clear();
        readbacks.clear();
        layouts.clear();
        kernels.clear();
        if (submit.pool)
            submit.destroy(vk);
        alive = false;
    }
};

namespace
{
struct RecState
{
    Session::Impl& s;
    VulkanContext& vk;
    VkCommandBuffer cmd;
    std::vector<rc::Buffer>& staging;
};

RecState& st(void* p) { return *static_cast<RecState*>(p); }

const Session::Impl::Buf& need(const RecState& r, int id)
{
    const auto it = r.s.bufs.find(id);
    if (it == r.s.bufs.end())
        throw std::runtime_error("gpu: no buffer " + std::to_string(id));
    return it->second;
}

const Session::Impl::Kern& needKernel(const RecState& r, int id)
{
    const auto it = r.s.kernels.find(id);
    if (it == r.s.kernels.end())
        throw std::runtime_error("gpu: no kernel " + std::to_string(id));
    return it->second;
}
} // namespace

Session::Session() : impl_(std::make_shared<Impl>())
{
    std::call_once(gStopHook, [] {
        Renderer::get().addStopHook([](VulkanContext& vk) {
            std::scoped_lock live(gLiveMutex);
            auto& l = liveSessions();
            for (auto& w : l)
                if (auto s = w.lock())
                {
                    std::scoped_lock lock(s->mutex);
                    s->release(vk);
                }
            l.erase(std::remove_if(l.begin(), l.end(), [](auto& w) { return w.expired(); }), l.end());
        });
    });
    std::scoped_lock live(gLiveMutex);
    liveSessions().push_back(impl_);
}

Session::~Session()
{
    bool held;
    {
        std::scoped_lock lock(impl_->mutex);
        held = impl_->alive || impl_->submit.pool;
    }
    // Without a running renderer the stop hook already released everything.
    if (held && Renderer::get().running() && std::this_thread::get_id() != gRenderThread.load())
    {
        auto impl = impl_;
        try
        {
            Renderer::get()
                .runTask([impl](VulkanContext& vk) {
                    std::scoped_lock lock(impl->mutex);
                    impl->release(vk);
                    return true;
                })
                .get();
        }
        catch (const std::exception&)
        {
        }
    }
}

bool Session::run(const std::function<void(Recorder&)>& record, std::string* err)
{
    LILA_ZONE();
    Renderer& r = Renderer::get();
    const char* problem = !r.running()                                          ? "gpu: the renderer is not running"
                          : std::this_thread::get_id() == gRenderThread.load() ? "gpu: Session::run cannot wait on the render thread"
                                                                                : nullptr;
    if (problem)
    {
        if (err)
            *err = problem;
        return false;
    }
    auto impl = impl_;
    try
    {
        r.runTask([impl, &record](VulkanContext& vk) {
             gRenderThread = std::this_thread::get_id();
             std::scoped_lock lock(impl->mutex);
             if (!impl->submit.pool)
                 impl->submit.init(vk);
             // The last run's descriptor sets are done with: its wait returned.
             for (VkDescriptorPool p : impl->pools)
                 vkResetDescriptorPool(vk.device, p, 0);
             impl->poolUsed = 0;
             impl->setsInPool = 0;
             std::vector<rc::Buffer> staging;
             try
             {
                 impl->ms = impl->submit.run(vk, [&](VkCommandBuffer cmd) {
                     RecState state{*impl, vk, cmd, staging};
                     Recorder rec(&state);
                     record(rec);
                 });
             }
             catch (...)
             {
                 for (rc::Buffer& b : staging)
                     rc::destroy(vk, b);
                 throw;
             }
             for (rc::Buffer& b : staging)
                 rc::destroy(vk, b);
             impl->alive = true;
             return true;
         })
            .get();
    }
    catch (const std::exception& e)
    {
        if (err)
            *err = e.what();
        return false;
    }
    return true;
}

bool Session::valid() const
{
    std::scoped_lock lock(impl_->mutex);
    return impl_->alive;
}

bool Session::read(int id, void* out, size_t bytes, size_t offset) const
{
    std::scoped_lock lock(impl_->mutex);
    const auto it = impl_->readbacks.find(id);
    if (it == impl_->readbacks.end() || !it->second.mapped || offset + bytes > it->second.size)
        return false;
    VulkanContext* vk = Renderer::get().context();
    if (vk)
        vmaInvalidateAllocation(vk->allocator, it->second.alloc, 0, VK_WHOLE_SIZE);
    std::memcpy(out, static_cast<const char*>(it->second.mapped) + offset, bytes);
    return true;
}

double Session::lastMs() const
{
    std::scoped_lock lock(impl_->mutex);
    return impl_->ms;
}

uint64_t Recorder::buffer(int id, size_t bytes, BufferKind kind)
{
    RecState& r = st(state_);
    bytes = std::max<size_t>((bytes + 3) & ~size_t(3), 16);
    auto it = r.s.bufs.find(id);
    if (it != r.s.bufs.end() && it->second.kind == kind && it->second.b.size == bytes)
        return it->second.b.address;
    if (it != r.s.bufs.end())
        // Earlier work in this submission may still use the old buffer; free it with the staging after the wait.
        r.staging.push_back(it->second.b);
    Session::Impl::Buf b;
    b.kind = kind;
    b.b = rc::makeBuffer(r.vk, bytes,
                         VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT | (kind == BufferKind::Indirect ? VK_BUFFER_USAGE_INDIRECT_BUFFER_BIT : 0),
                         rc::Host::None);
    r.s.bufs[id] = b;
    return b.b.address;
}

uint64_t Recorder::address(int id) const
{
    const RecState& r = st(state_);
    const auto it = r.s.bufs.find(id);
    return it == r.s.bufs.end() ? 0 : it->second.b.address;
}

size_t Recorder::size(int id) const
{
    const RecState& r = st(state_);
    const auto it = r.s.bufs.find(id);
    return it == r.s.bufs.end() ? 0 : size_t(it->second.b.size);
}

void Recorder::upload(int id, const void* data, size_t bytes, size_t offset)
{
    RecState& r = st(state_);
    const auto& b = need(r, id);
    if (!data || !bytes)
        return;
    if (offset + bytes > b.b.size)
        throw std::runtime_error("gpu: upload past the end of buffer " + std::to_string(id));
    r.staging.push_back(rc::hostBuffer(r.vk, data, bytes, VK_BUFFER_USAGE_TRANSFER_SRC_BIT));
    const VkBufferCopy region{0, offset, bytes};
    vkCmdCopyBuffer(r.cmd, r.staging.back().buffer, b.b.buffer, 1, &region);
}

void Recorder::fill(int id, uint32_t value)
{
    RecState& r = st(state_);
    vkCmdFillBuffer(r.cmd, need(r, id).b.buffer, 0, VK_WHOLE_SIZE, value);
}

void Recorder::copy(int from, int to, size_t bytes, size_t fromOffset, size_t toOffset)
{
    RecState& r = st(state_);
    const auto &a = need(r, from), &b = need(r, to);
    if (fromOffset + bytes > a.b.size || toOffset + bytes > b.b.size)
        throw std::runtime_error("gpu: copy past the end of a buffer");
    const VkBufferCopy region{fromOffset, toOffset, bytes};
    vkCmdCopyBuffer(r.cmd, a.b.buffer, b.b.buffer, 1, &region);
}

void Recorder::readback(int id, size_t bytes)
{
    RecState& r = st(state_);
    const auto& b = need(r, id);
    bytes = std::min<size_t>(std::max<size_t>(bytes, 4), b.b.size);
    rc::Buffer& rb = r.s.readbacks[id];
    if (rb.size < bytes)
    {
        if (rb.buffer)
            r.staging.push_back(rb);
        rb = rc::makeBuffer(r.vk, bytes, 0, rc::Host::Read);
    }
    rc::memoryBarrier(r.cmd);
    const VkBufferCopy region{0, 0, bytes};
    vkCmdCopyBuffer(r.cmd, b.b.buffer, rb.buffer, 1, &region);
}

void Recorder::kernel(int id, const char* shader, const char* entry, uint32_t pushBytes)
{
    RecState& r = st(state_);
    if (r.s.kernels.count(id))
        return;
    if (pushBytes > 128)
        throw std::runtime_error("gpu: push constants are limited to 128 bytes");
    Session::Impl::Code& code = r.s.code[shader];
    if (code.words.empty())
    {
        std::string err;
        const std::filesystem::path path = paths::fromUtf8(shader);
        if (path.is_absolute())
        {
            std::ifstream in(path, std::ios::binary | std::ios::ate);
            if (in)
            {
                code.words.resize(size_t(in.tellg()) / 4);
                in.seekg(0);
                in.read(reinterpret_cast<char*>(code.words.data()), std::streamsize(code.words.size() * 4));
            }
            else
                err = std::string("gpu: missing shader ") + shader;
        }
        else
            code.words = rc::loadSpirv(shader, &err);
        if (code.words.empty())
            throw std::runtime_error(err.empty() ? std::string("gpu: cannot load ") + shader : err);
        code.hash = rc::spirvHash(code.words);
    }
    VkPipelineLayout& layout = r.s.layouts[pushBytes];
    if (!layout)
        layout = rc::pipelineLayout(r.vk, pushBytes, kCompute);
    const uint64_t key = rc::layoutKey(pushBytes, kCompute, "lila.gpu.push");
    r.s.kernels[id] = {rc::sharedPipelines().get(r.vk, layout, key, code.words, code.hash, entry), layout, pushBytes};
}

void Recorder::dispatch(int kernel, const void* push, uint32_t groupsX, uint32_t groupsY, uint32_t groupsZ)
{
    RecState& r = st(state_);
    const auto& k = needKernel(r, kernel);
    if (!groupsX || !groupsY || !groupsZ)
        return;
    vkCmdBindPipeline(r.cmd, VK_PIPELINE_BIND_POINT_COMPUTE, k.pipe);
    if (k.push && push)
        vkCmdPushConstants(r.cmd, k.layout, kCompute, 0, k.push, push);
    vkCmdDispatch(r.cmd, groupsX, groupsY, groupsZ);
}

void Recorder::dispatchIndirect(int kernel, const void* push, int argsBuffer, size_t offset)
{
    RecState& r = st(state_);
    const auto& k = needKernel(r, kernel);
    const auto& args = need(r, argsBuffer);
    if (args.kind != BufferKind::Indirect)
        throw std::runtime_error("gpu: dispatchIndirect needs a BufferKind::Indirect buffer");
    vkCmdBindPipeline(r.cmd, VK_PIPELINE_BIND_POINT_COMPUTE, k.pipe);
    if (k.push && push)
        vkCmdPushConstants(r.cmd, k.layout, kCompute, 0, k.push, push);
    vkCmdDispatchIndirect(r.cmd, args.b.buffer, offset);
}

void Recorder::barrier() { rc::memoryBarrier(st(state_).cmd); }

namespace
{
constexpr uint32_t kSetsPerPool = 1024;
constexpr size_t kZeroBytes = 4096;

/** The words of a shader file: absolute, or in the shaders folder. */
const Session::Impl::Code& shaderCode(RecState& r, const char* shader)
{
    Session::Impl::Code& code = r.s.code[shader];
    if (!code.words.empty())
        return code;
    std::string err;
    const std::filesystem::path path = paths::fromUtf8(shader);
    if (path.is_absolute())
    {
        std::ifstream in(path, std::ios::binary | std::ios::ate);
        if (in)
        {
            code.words.resize(size_t(in.tellg()) / 4);
            in.seekg(0);
            in.read(reinterpret_cast<char*>(code.words.data()), std::streamsize(code.words.size() * 4));
        }
        else
            err = std::string("gpu: missing shader ") + shader;
    }
    else
        code.words = rc::loadSpirv(shader, &err);
    if (code.words.empty())
        throw std::runtime_error(err.empty() ? std::string("gpu: cannot load ") + shader : err);
    code.hash = rc::spirvHash(code.words);
    return code;
}

VkDescriptorType descriptorType(SlotKind k)
{
    return k == SlotKind::Uniform ? VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER : VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
}

VkDescriptorSet allocateSet(RecState& r, VkDescriptorSetLayout layout)
{
    Session::Impl& s = r.s;
    if (s.pools.empty() || s.setsInPool >= kSetsPerPool)
    {
        if (!s.pools.empty())
            ++s.poolUsed;
        s.setsInPool = 0;
        if (s.poolUsed >= s.pools.size())
        {
            const VkDescriptorPoolSize sizes[] = {{VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, kSetsPerPool * 2},
                                                  {VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, kSetsPerPool * 16}};
            VkDescriptorPoolCreateInfo pci{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
            pci.maxSets = kSetsPerPool;
            pci.poolSizeCount = 2;
            pci.pPoolSizes = sizes;
            VkDescriptorPool pool = VK_NULL_HANDLE;
            if (vkCreateDescriptorPool(r.vk.device, &pci, nullptr, &pool) != VK_SUCCESS)
                throw std::runtime_error("gpu: cannot create a descriptor pool");
            s.pools.push_back(pool);
            s.poolUsed = s.pools.size() - 1;
        }
    }
    VkDescriptorSetAllocateInfo ai{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
    ai.descriptorPool = s.pools[s.poolUsed];
    ai.descriptorSetCount = 1;
    ai.pSetLayouts = &layout;
    VkDescriptorSet set = VK_NULL_HANDLE;
    if (vkAllocateDescriptorSets(r.vk.device, &ai, &set) != VK_SUCCESS)
        throw std::runtime_error("gpu: cannot allocate a descriptor set");
    ++s.setsInPool;
    return set;
}
} // namespace

void Recorder::boundKernel(int id, const char* shader, const char* entry, std::span<const Slot> slots)
{
    RecState& r = st(state_);
    if (r.s.kernels.count(id))
        return;
    std::vector<Slot> sorted(slots.begin(), slots.end());
    std::sort(sorted.begin(), sorted.end(), [](const Slot& a, const Slot& b) { return a.binding < b.binding; });
    std::string key = "lila.gpu.bound";
    for (const Slot& s : sorted)
        key += (s.kind == SlotKind::Uniform ? ":u" : ":s") + std::to_string(s.binding);
    Session::Impl::BoundLayout& bl = r.s.boundLayouts[key];
    if (!bl.set)
    {
        std::vector<VkDescriptorSetLayoutBinding> bindings;
        for (const Slot& s : sorted)
            bindings.push_back({s.binding, descriptorType(s.kind), 1, kCompute, nullptr});
        VkDescriptorSetLayoutCreateInfo ci{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
        ci.bindingCount = uint32_t(bindings.size());
        ci.pBindings = bindings.data();
        if (vkCreateDescriptorSetLayout(r.vk.device, &ci, nullptr, &bl.set) != VK_SUCCESS)
            throw std::runtime_error("gpu: cannot create a descriptor set layout");
        bl.layout = rc::pipelineLayout(r.vk, 0, kCompute, std::span<const VkDescriptorSetLayout>(&bl.set, 1));
    }
    const Session::Impl::Code& code = shaderCode(r, shader);
    const uint64_t layoutKey = rc::layoutKey(0, kCompute, key);
    r.s.kernels[id] = {rc::sharedPipelines().get(r.vk, bl.layout, layoutKey, code.words, code.hash, entry), bl.layout, 0, sorted};
}

void Recorder::dispatchBound(int kernel, std::span<const Bind> binds, uint32_t groupsX, uint32_t groupsY, uint32_t groupsZ)
{
    RecState& r = st(state_);
    const auto& k = needKernel(r, kernel);
    if (!groupsX || !groupsY || !groupsZ)
        return;
    if (!r.s.zero.buffer)
    {
        r.s.zero = rc::makeBuffer(r.vk, kZeroBytes, VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT, rc::Host::None);
        vkCmdFillBuffer(r.cmd, r.s.zero.buffer, 0, VK_WHOLE_SIZE, 0);
        rc::memoryBarrier(r.cmd);
    }
    std::string key = "lila.gpu.bound";
    for (const Slot& s : k.slots)
        key += (s.kind == SlotKind::Uniform ? ":u" : ":s") + std::to_string(s.binding);
    const VkDescriptorSet set = allocateSet(r, r.s.boundLayouts.at(key).set);
    std::vector<VkDescriptorBufferInfo> infos(k.slots.size());
    std::vector<VkWriteDescriptorSet> writes(k.slots.size());
    for (size_t i = 0; i < k.slots.size(); ++i)
    {
        const Slot& slot = k.slots[i];
        VkDescriptorBufferInfo& info = infos[i];
        info = {r.s.zero.buffer, 0, VK_WHOLE_SIZE};
        for (const Bind& b : binds)
            if (b.binding == slot.binding && b.buffer >= 0)
            {
                const auto it = r.s.bufs.find(b.buffer);
                if (it == r.s.bufs.end())
                    break;
                info = {it->second.b.buffer, b.offset, b.bytes ? b.bytes : VK_WHOLE_SIZE};
                break;
            }
        if (slot.kind == SlotKind::Uniform && info.buffer == r.s.zero.buffer)
            info.range = kZeroBytes;
        VkWriteDescriptorSet& w = writes[i];
        w = {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
        w.dstSet = set;
        w.dstBinding = slot.binding;
        w.descriptorCount = 1;
        w.descriptorType = descriptorType(slot.kind);
        w.pBufferInfo = &info;
    }
    vkUpdateDescriptorSets(r.vk.device, uint32_t(writes.size()), writes.data(), 0, nullptr);
    vkCmdBindPipeline(r.cmd, VK_PIPELINE_BIND_POINT_COMPUTE, k.pipe);
    vkCmdBindDescriptorSets(r.cmd, VK_PIPELINE_BIND_POINT_COMPUTE, k.layout, 0, 1, &set, 0, nullptr);
    vkCmdDispatch(r.cmd, groupsX, groupsY, groupsZ);
}

size_t Recorder::uniformAlignment() const
{
    return size_t(st(state_).vk.properties.limits.minUniformBufferOffsetAlignment);
}
} // namespace lila::gpu
