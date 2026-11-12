#include "scene/DeformEval.h"

#include "core/PrefStore.h"
#include "core/Profile.h"
#include "scene/Deformers.h"

#include <tbb/parallel_for.h>

#include <atomic>
#include <bit>
#include <cstring>
#include <list>
#include <mutex>
#include <unordered_map>

namespace lila::scene::deformEval
{
namespace
{
std::mutex gBackendMutex;
std::shared_ptr<Backend> gBackend;
thread_local int gCpuOnly = 0;

struct Counters
{
    std::atomic<uint64_t> computes{0}, cacheHits{0}, gpuRuns{0}, gpuDeclined{0}, bindHits{0}, bindMisses{0};
};
Counters& counters()
{
    static Counters c;
    return c;
}

constexpr uint64_t kPrime = 0x9e3779b97f4a7c15ull;

uint64_t mix64(uint64_t h)
{
    h ^= h >> 33;
    h *= 0xff51afd7ed558ccdull;
    h ^= h >> 33;
    h *= 0xc4ceb3f99c2bc11dull;
    h ^= h >> 33;
    return h;
}

/** Order-dependent hash of n words produced by word(i), in parallel blocks combined in block order. */
template <class F> uint64_t hashWords(size_t n, uint64_t seed, F&& word)
{
    constexpr size_t kBlock = 1 << 16;
    const size_t blocks = (n + kBlock - 1) / kBlock;
    std::vector<uint64_t> partial(blocks);
    tbb::parallel_for(size_t(0), blocks, [&](size_t b) {
        uint64_t h = mix64(b + 1);
        for (size_t i = b * kBlock, e = std::min(n, i + kBlock); i < e; ++i)
            h = (h ^ mix64(word(i))) * kPrime;
        partial[b] = h;
    });
    uint64_t h = mix64(seed ^ n);
    for (uint64_t p : partial)
        h = hashCombine(h, p);
    return h;
}

struct BindEntry
{
    std::shared_ptr<const void> value;
    size_t bytes = 0;
    std::list<uint64_t>::iterator lru;
};

struct BindCache
{
    std::mutex mutex;
    std::unordered_map<uint64_t, BindEntry> entries;
    std::list<uint64_t> lru; // most recent first
    size_t bytes = 0;
    static constexpr size_t kBudget = size_t(2) << 30;
};
BindCache& bindCache()
{
    static BindCache* c = new BindCache;
    return *c;
}

struct Identity
{
    std::weak_ptr<const void> object;
    uint64_t key = 0;
};
std::mutex gIdentityMutex;
std::unordered_map<const void*, Identity> gIdentities;
uint64_t gNextIdentity = 1;
} // namespace

void Program::push(float v) { params.push_back(std::bit_cast<uint32_t>(v)); }
void Program::push(uint32_t v) { params.push_back(v); }
void Program::push(const glm::dvec3& v)
{
    for (int k = 0; k < 3; ++k)
        push(float(v[k]));
}
void Program::push(const glm::dmat3& m)
{
    for (int c = 0; c < 3; ++c)
        push(m[c]);
}
void Program::push(const glm::dmat4& m)
{
    for (int c = 0; c < 4; ++c)
        for (int r = 0; r < 4; ++r)
            push(float(m[c][r]));
}
void Program::push(std::span<const glm::dvec3> v)
{
    params.reserve(params.size() + v.size() * 3);
    for (const glm::dvec3& p : v)
        push(p);
}

void setBackend(std::shared_ptr<Backend> backend)
{
    std::scoped_lock lock(gBackendMutex);
    gBackend = std::move(backend);
}

bool hasBackend()
{
    std::scoped_lock lock(gBackendMutex);
    return gBackend != nullptr;
}

CpuOnly::CpuOnly() { ++gCpuOnly; }
CpuOnly::~CpuOnly() { --gCpuOnly; }

bool useGpu(size_t points)
{
    if (gCpuOnly > 0 || !hasBackend())
        return false;
    const PrefStore& prefs = PrefStore::get();
    return prefs.value<bool>("general", "/deformers/gpu", true) && double(points) >= prefs.value<double>("general", "/deformers/gpuMinPoints", 20000.0);
}

bool tryGpu(const DeformerNode& d, const geo::Mesh& in, std::vector<glm::dvec3>& points)
{
    if (!useGpu(points.size()))
        return false;
    LILA_ZONE();

    Program p;
    if (!d.gpuProgram(in, p))
        return false;
    std::shared_ptr<Backend> b;
    {
        std::scoped_lock lock(gBackendMutex);
        b = gBackend;
    }
    if (b && b->run(p, points))
    {
        ++counters().gpuRuns;
        return true;
    }
    ++counters().gpuDeclined;
    return false;
}

Stats stats()
{
    Counters& c = counters();
    return {c.computes.load(), c.cacheHits.load(), c.gpuRuns.load(), c.gpuDeclined.load(), c.bindHits.load(), c.bindMisses.load()};
}

void resetStats()
{
    Counters& c = counters();
    c.computes = 0;
    c.cacheHits = 0;
    c.gpuRuns = 0;
    c.gpuDeclined = 0;
    c.bindHits = 0;
    c.bindMisses = 0;
}

void countCompute(bool cacheHit) { ++(cacheHit ? counters().cacheHits : counters().computes); }

// ------------------------------------------------------------------------------------------------ hashing and caches

uint64_t hashCombine(uint64_t a, uint64_t b) { return mix64(a * kPrime + b + 0x632be59bd9b4e019ull); }

uint64_t hashBytes(const void* data, size_t bytes, uint64_t seed)
{
    LILA_ZONE();

    const auto* p = static_cast<const unsigned char*>(data);
    const size_t words = bytes / 8;
    uint64_t h = hashWords(words, seed ^ bytes, [&](size_t i) {
        uint64_t w;
        std::memcpy(&w, p + i * 8, 8);
        return w;
    });
    uint64_t tail = 0;
    if (bytes > words * 8)
        std::memcpy(&tail, p + words * 8, bytes - words * 8);
    return hashCombine(h, tail);
}

uint64_t topologyHash(const geo::Mesh& m)
{
    LILA_ZONE();

    using geo::Index;
#ifdef _WIN32
    uint64_t h = hashWords(m.pointCapacity(), 1, [&](size_t p) { return m.pointValid(Index(p)) ? uint64_t(m.pointEdge(Index(p))) : ~0ull; });
    h = hashCombine(h, hashWords(m.edgeCapacity(), 2, [&](size_t i) {
                        const Index e = Index(i);
                        if (!m.edgeValid(e))
                            return ~0ull;
                        const Index a = m.edgePoint(e, 0), b = m.edgePoint(e, 1);
                        return mix64((uint64_t(a) << 32 | b) ^ mix64(uint64_t(m.edgeNextAround(e, a)) << 32 | m.edgeNextAround(e, b)));
                    }));
    h = hashCombine(h, hashWords(m.cornerCapacity(), 3, [&](size_t i) {
                        const Index c = Index(i);
                        return m.cornerValid(c) ? uint64_t(m.cornerPoint(c)) << 32 | m.cornerNext(c) : ~0ull;
                    }));
#else
    // uint64_t is unsigned long here, not unsigned long long, so ~0ull would deduce a second lambda return type.
    uint64_t h = hashWords(m.pointCapacity(), 1, [&](size_t p) { return m.pointValid(Index(p)) ? uint64_t(m.pointEdge(Index(p))) : ~uint64_t(0); });
    h = hashCombine(h, hashWords(m.edgeCapacity(), 2, [&](size_t i) {
                        const Index e = Index(i);
                        if (!m.edgeValid(e))
                            return ~uint64_t(0);
                        const Index a = m.edgePoint(e, 0), b = m.edgePoint(e, 1);
                        return mix64((uint64_t(a) << 32 | b) ^ mix64(uint64_t(m.edgeNextAround(e, a)) << 32 | m.edgeNextAround(e, b)));
                    }));
    h = hashCombine(h, hashWords(m.cornerCapacity(), 3, [&](size_t i) {
                        const Index c = Index(i);
                        return m.cornerValid(c) ? uint64_t(m.cornerPoint(c)) << 32 | m.cornerNext(c) : ~uint64_t(0);
                    }));
#endif
    return h;
}

uint64_t identityKey(const std::shared_ptr<const void>& object)
{
    if (!object)
        return 0;
    std::scoped_lock lock(gIdentityMutex);
    Identity& id = gIdentities[object.get()];
    if (id.key == 0 || id.object.expired() || id.object.lock() != object)
    {
        id.object = object;
        id.key = mix64(gNextIdentity++ ^ 0x5bd1e995ull);
    }
    // Forget dead objects now and then so the map stays as small as the live set.
    if (gIdentities.size() > 4096)
        std::erase_if(gIdentities, [](const auto& e) { return e.second.object.expired(); });
    return id.key;
}

std::shared_ptr<const void> cachedBind(uint64_t key, const std::function<std::pair<std::shared_ptr<const void>, size_t>()>& make)
{
    BindCache& c = bindCache();
    {
        std::scoped_lock lock(c.mutex);
        if (auto it = c.entries.find(key); it != c.entries.end())
        {
            c.lru.splice(c.lru.begin(), c.lru, it->second.lru);
            ++counters().bindHits;
            return it->second.value;
        }
    }
    ++counters().bindMisses;
    auto [value, bytes] = make();
    if (!value)
        return value;
    std::scoped_lock lock(c.mutex);
    if (auto it = c.entries.find(key); it != c.entries.end())
        return it->second.value;
    c.lru.push_front(key);
    c.entries[key] = {value, bytes, c.lru.begin()};
    c.bytes += bytes;
    while (c.bytes > BindCache::kBudget && c.lru.size() > 1)
    {
        auto last = c.entries.find(c.lru.back());
        c.bytes -= last->second.bytes;
        c.entries.erase(last);
        c.lru.pop_back();
    }
    return value;
}

void clearBindCache()
{
    BindCache& c = bindCache();
    std::scoped_lock lock(c.mutex);
    c.entries.clear();
    c.lru.clear();
    c.bytes = 0;
}
} // namespace lila::scene::deformEval
