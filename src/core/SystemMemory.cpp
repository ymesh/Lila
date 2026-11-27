#include "core/SystemMemory.h"

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <dbghelp.h>
#include <psapi.h>

#include <algorithm>
#include <bit>
#include <cstdlib>
#include <new>
#include <unordered_map>
#else
#include <unistd.h>
#endif

namespace lila
{
size_t physicalMemoryBytes()
{
#ifdef _WIN32
    MEMORYSTATUSEX mem{sizeof(mem)};
    return GlobalMemoryStatusEx(&mem) ? size_t(mem.ullTotalPhys) : 0;
#else
    const long pages = sysconf(_SC_PHYS_PAGES), size = sysconf(_SC_PAGE_SIZE);
    return pages > 0 && size > 0 ? size_t(pages) * size_t(size) : 0;
#endif
}

ProcessMemory processMemory()
{
    ProcessMemory out;
#ifdef _WIN32
    PROCESS_MEMORY_COUNTERS_EX c{};
    c.cb = sizeof c;
    if (K32GetProcessMemoryInfo(GetCurrentProcess(), reinterpret_cast<PROCESS_MEMORY_COUNTERS*>(&c), sizeof c))
    {
        out.commit = c.PrivateUsage;
        out.workingSet = c.WorkingSetSize;
    }
#endif
    return out;
}

HeapUse heapUse()
{
    HeapUse out;
#ifdef _WIN32
    std::vector<HANDLE> heaps(64);
    DWORD n = GetProcessHeaps(DWORD(heaps.size()), heaps.data());
    if (n > heaps.size())
    {
        heaps.resize(n);
        n = GetProcessHeaps(n, heaps.data());
    }
    heaps.resize(std::min<size_t>(n, heaps.size()));
    for (HANDLE h : heaps)
    {
        if (!HeapLock(h))
            continue;
        ++out.heaps;
        PROCESS_HEAP_ENTRY e{};
        while (HeapWalk(h, &e))
        {
            if (e.wFlags & PROCESS_HEAP_REGION)
                out.committed += e.Region.dwCommittedSize;
            else if (e.wFlags & PROCESS_HEAP_ENTRY_BUSY)
            {
                out.busy += e.cbData;
                ++out.busyBlocks;
                const size_t bucket = e.cbData < 128 ? 0 : std::min<size_t>(std::bit_width(e.cbData) - 7, out.busyBySize.size() - 1);
                out.busyBySize[bucket] += e.cbData;
            }
            else if (!(e.wFlags & PROCESS_HEAP_UNCOMMITTED_RANGE))
            {
                out.free += e.cbData;
                ++out.freeBlocks;
            }
        }
        HeapUnlock(h);
    }
#endif
    return out;
}
#ifdef _WIN32
namespace
{
/** The loaded modules' image ranges, sorted. */
std::vector<std::pair<uintptr_t, uintptr_t>> moduleImages()
{
    std::vector<std::pair<uintptr_t, uintptr_t>> images;
    std::vector<HMODULE> mods(2048);
    DWORD needed = 0;
    if (!K32EnumProcessModules(GetCurrentProcess(), mods.data(), DWORD(mods.size() * sizeof(HMODULE)), &needed))
        return images;
    mods.resize(std::min<size_t>(needed / sizeof(HMODULE), mods.size()));
    for (HMODULE m : mods)
    {
        MODULEINFO info{};
        if (K32GetModuleInformation(GetCurrentProcess(), m, &info, sizeof info))
            images.push_back({uintptr_t(info.lpBaseOfDll), uintptr_t(info.lpBaseOfDll) + info.SizeOfImage});
    }
    std::sort(images.begin(), images.end());
    return images;
}

template <class Ranges> bool inRanges(const Ranges& ranges, uintptr_t a)
{
    auto it = std::upper_bound(ranges.begin(), ranges.end(), std::make_pair(a, UINTPTR_MAX));
    return it != ranges.begin() && a < std::prev(it)->second;
}

/**
 * A heap of the walk's own: what a walk collects is allocated here, since allocating on a heap being walked breaks
 * the walk. Left out of the heaps walked.
 */
struct ScratchHeap
{
    HANDLE heap = HeapCreate(0, 0, 0);
    ~ScratchHeap() { HeapDestroy(heap); }
};
HANDLE gScratch = nullptr;

template <class T> struct ScratchAlloc
{
    using value_type = T;
    ScratchAlloc() = default;
    template <class U> ScratchAlloc(const ScratchAlloc<U>&) {}
    T* allocate(size_t n)
    {
        if (void* p = HeapAlloc(gScratch, 0, n * sizeof(T)))
            return static_cast<T*>(p);
        throw std::bad_alloc();
    }
    void deallocate(T* p, size_t) { HeapFree(gScratch, 0, p); }
    template <class U> bool operator==(const ScratchAlloc<U>&) const { return true; }
};
template <class T> using ScratchVector = std::vector<T, ScratchAlloc<T>>;
template <class K, class V> using ScratchMap = std::unordered_map<K, V, std::hash<K>, std::equal_to<K>, ScratchAlloc<std::pair<const K, V>>>;

/** module!symbol+offset for an address in a loaded module, as far as the symbols tell. */
template <class Symbols> std::string codeAddress(Symbols& symbols, uintptr_t addr)
{
    std::string module = "?";
    HMODULE m = nullptr;
    char file[MAX_PATH] = {};
    if (GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT, reinterpret_cast<LPCSTR>(addr), &m) &&
        GetModuleFileNameA(m, file, MAX_PATH))
    {
        module = file;
        module = module.substr(module.find_last_of("\\/") + 1);
    }
    uint64_t d = 0;
    const std::string name = symbols.name(addr, &d);
    return module + "!" + (name.empty() ? "?" : name) + "+" + std::to_string(d);
}

/** The first word in [lo, hi) that points into one of the sorted target ranges; 0 when none or the memory went away. */
uintptr_t firstPointerInto(uintptr_t lo, uintptr_t hi, const std::pair<uintptr_t, uintptr_t>* targets, size_t count)
{
    __try
    {
        for (uintptr_t a = lo; a + sizeof(uintptr_t) <= hi; a += sizeof(uintptr_t))
        {
            const uintptr_t w = *reinterpret_cast<const uintptr_t*>(a);
            if (w < targets[0].first || w >= targets[count - 1].second)
                continue;
            size_t l = 0, h = count;
            while (l < h)
            {
                const size_t m = (l + h) / 2;
                if (targets[m].first <= w)
                    l = m + 1;
                else
                    h = m;
            }
            if (l > 0 && w < targets[l - 1].second)
                return a;
        }
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
    }
    return 0;
}

std::vector<HANDLE> processHeaps()
{
    std::vector<HANDLE> heaps(64);
    DWORD n = GetProcessHeaps(DWORD(heaps.size()), heaps.data());
    if (n > heaps.size())
    {
        heaps.resize(n);
        n = GetProcessHeaps(n, heaps.data());
    }
    heaps.resize(std::min<size_t>(n, heaps.size()));
    std::erase(heaps, gScratch);
    return heaps;
}

/** Calls fn(data, size) for every busy block of every heap, each heap locked while it is walked. */
template <class F> void forEachBusyBlock(const std::vector<HANDLE>& heaps, bool lock, F&& fn)
{
    for (HANDLE h : heaps)
    {
        if (lock && !HeapLock(h))
            continue;
        PROCESS_HEAP_ENTRY e{};
        while (HeapWalk(h, &e))
            if ((e.wFlags & PROCESS_HEAP_ENTRY_BUSY) && e.cbData >= sizeof(uintptr_t))
                fn(static_cast<const uintptr_t*>(e.lpData), size_t(e.cbData));
        if (lock)
            HeapUnlock(h);
    }
}

/** dbghelp on a handle of its own, so it never meets the crash handler's. */
class Symbols
{
public:
    Symbols()
    {
        if (!DuplicateHandle(GetCurrentProcess(), GetCurrentProcess(), GetCurrentProcess(), &mProcess, 0, FALSE, DUPLICATE_SAME_ACCESS))
            return;
        SymSetOptions(SYMOPT_UNDNAME | SYMOPT_DEFERRED_LOADS);
        mOk = SymInitialize(mProcess, nullptr, TRUE);
    }
    ~Symbols()
    {
        if (mOk)
            SymCleanup(mProcess);
        if (mProcess)
            CloseHandle(mProcess);
    }
    /** The symbol at or before addr; empty when none. */
    std::string name(uintptr_t addr, uint64_t* displacement = nullptr)
    {
        if (!mOk)
            return {};
        alignas(SYMBOL_INFO) char buffer[sizeof(SYMBOL_INFO) + 1024];
        auto* sym = reinterpret_cast<SYMBOL_INFO*>(buffer);
        *sym = SYMBOL_INFO{};
        sym->SizeOfStruct = sizeof(SYMBOL_INFO);
        sym->MaxNameLen = 1024;
        DWORD64 d = 0;
        if (!SymFromAddr(mProcess, addr, &d, sym))
            return {};
        if (displacement)
            *displacement = d;
        return std::string(sym->Name, sym->NameLen);
    }
    /** The class whose vtable is at addr; empty when addr is not exactly a vtable. */
    std::string vtable(uintptr_t addr)
    {
        auto [it, added] = mVtables.try_emplace(addr);
        if (added)
        {
            uint64_t d = 0;
            std::string n = name(addr, &d);
            if (d == 0 && n.find("vftable") != std::string::npos)
                it->second = std::move(n);
        }
        return it->second;
    }

private:
    HANDLE mProcess = nullptr;
    bool mOk = false;
    std::unordered_map<uintptr_t, std::string> mVtables;
};
} // namespace
#endif

std::vector<HeapType> heapTypes(size_t top)
{
    std::vector<HeapType> out;
#ifdef _WIN32
    const auto images = moduleImages();
    ScratchHeap scratch;
    gScratch = scratch.heap;
    ScratchMap<uintptr_t, std::pair<size_t, size_t>> byFirst;
    forEachBusyBlock(processHeaps(), true, [&](const uintptr_t* data, size_t size) {
        if (inRanges(images, data[0]))
        {
            auto& c = byFirst[data[0]];
            ++c.first;
            c.second += size;
        }
    });
    Symbols symbols;
    std::unordered_map<std::string, size_t> index;
    for (const auto& [addr, c] : byFirst)
    {
        std::string name = symbols.vtable(addr);
        if (name.empty())
            continue;
        auto [it, added] = index.emplace(std::move(name), out.size());
        if (added)
            out.push_back({it->first, 0, 0});
        out[it->second].count += c.first;
        out[it->second].bytes += c.second;
    }
    std::sort(out.begin(), out.end(), [](const HeapType& a, const HeapType& b) { return a.count > b.count; });
    if (out.size() > top)
        out.resize(top);
#else
    (void)top;
#endif
    return out;
}

HeapHolders heapHolders(const std::string& type, size_t samples, size_t depth)
{
    HeapHolders out;
#ifdef _WIN32
    const auto images = moduleImages();
    ScratchHeap scratch;
    gScratch = scratch.heap;
    const std::vector<HANDLE> heaps = processHeaps();
    Symbols symbols;
    // Which vtables name the type, resolved before the heaps are locked (dbghelp allocates).
    ScratchMap<uintptr_t, char> isType;
    forEachBusyBlock(heaps, true, [&](const uintptr_t* data, size_t) {
        if (inRanges(images, data[0]))
            isType.try_emplace(data[0], 0);
    });
    for (auto& [addr, hit] : isType)
        hit = symbols.vtable(addr).find(type) != std::string::npos ? 1 : 0;
    // "size>=N" follows big blocks (arrays) instead of objects of a class.
    size_t minSize = 0;
    if (type.starts_with("size>="))
        minSize = size_t(std::strtoull(type.c_str() + 6, nullptr, 10));
    auto matches = [&](uintptr_t first, size_t size) {
        if (minSize)
            return size >= minSize;
        auto it = isType.find(first);
        return it != isType.end() && it->second != 0;
    };

    // Writable data in the modules, where static containers keep their roots.
    std::vector<std::pair<uintptr_t, uintptr_t>> globals;
    for (const auto& [lo, hi] : images)
        for (uintptr_t a = lo; a < hi;)
        {
            MEMORY_BASIC_INFORMATION mbi{};
            if (!VirtualQuery(reinterpret_cast<const void*>(a), &mbi, sizeof mbi))
                break;
            const uintptr_t end = std::min(hi, uintptr_t(mbi.BaseAddress) + mbi.RegionSize);
            if (mbi.State == MEM_COMMIT && (mbi.Protect & (PAGE_READWRITE | PAGE_WRITECOPY)) && !(mbi.Protect & PAGE_GUARD))
                globals.push_back({a, end});
            a = end;
        }

    struct Found
    {
        uintptr_t at = 0, block = 0, first = 0;
        size_t size = 0;
        bool global = false, other = false;
    };
    std::vector<ScratchVector<Found>> levels;
    levels.reserve(depth);
    for (HANDLE h : heaps)
        HeapLock(h);
    {
        // The heaps' own ranges, so the scan of other memory (stacks, Python's and TBB's pools) skips them.
        ScratchVector<std::pair<uintptr_t, uintptr_t>> heapRanges;
        size_t matching = 0;
        std::vector<HANDLE> walked = heaps;
        walked.push_back(gScratch);
        for (HANDLE h : walked)
        {
            PROCESS_HEAP_ENTRY e{};
            while (HeapWalk(h, &e))
                if (e.wFlags & PROCESS_HEAP_REGION)
                    heapRanges.push_back({uintptr_t(e.lpData), uintptr_t(e.Region.lpLastBlock)});
                else if ((e.wFlags & PROCESS_HEAP_ENTRY_BUSY) && e.cbData >= sizeof(uintptr_t))
                {
                    if (e.cbData >= (size_t(1) << 19))
                        heapRanges.push_back({uintptr_t(e.lpData), uintptr_t(e.lpData) + e.cbData});
                    const uintptr_t first = *static_cast<const uintptr_t*>(e.lpData);
                    if (h != gScratch && matches(first, e.cbData))
                        ++matching;
                }
        }
        std::sort(heapRanges.begin(), heapRanges.end());
        ScratchVector<std::pair<uintptr_t, uintptr_t>> others;
        for (uintptr_t a = 0x10000;;)
        {
            MEMORY_BASIC_INFORMATION mbi{};
            if (!VirtualQuery(reinterpret_cast<const void*>(a), &mbi, sizeof mbi))
                break;
            const uintptr_t end = uintptr_t(mbi.BaseAddress) + mbi.RegionSize;
            if (mbi.State == MEM_COMMIT && mbi.Type == MEM_PRIVATE && (mbi.Protect & (PAGE_READWRITE | PAGE_WRITECOPY)) &&
                !(mbi.Protect & PAGE_GUARD) && mbi.RegionSize <= (size_t(256) << 20) && !inRanges(heapRanges, uintptr_t(mbi.BaseAddress)))
                others.push_back({uintptr_t(mbi.BaseAddress), end});
            if (end <= a)
                break;
            a = end;
        }
        // Samples spread over all the matching objects, not the first few the walk meets.
        const size_t stride = std::max<size_t>(1, matching / std::max<size_t>(samples, 1));
        ScratchVector<std::pair<uintptr_t, uintptr_t>> targets;
        size_t seen = 0;
        forEachBusyBlock(heaps, false, [&](const uintptr_t* data, size_t size) {
            if (matches(data[0], size) && seen++ % stride == 0 && targets.size() < samples)
                targets.push_back({uintptr_t(data), uintptr_t(data) + size});
        });
        out.objects = targets.size();
        ScratchMap<uintptr_t, char> visited;
        for (const auto& t : targets)
            visited.emplace(t.first, 1);
        for (size_t level = 0; level < depth && !targets.empty(); ++level)
        {
            std::sort(targets.begin(), targets.end());
            const uintptr_t lo = targets.front().first, hi = targets.back().second;
            ScratchVector<Found> found;
            // Arrays are held by their first element (a little past the block's start when over-aligned); any word inside a
            // block of hundreds of megabytes would match by chance.
            const bool nearStart = minSize > 0 && level == 0;
            auto hit = [&](uintptr_t w) {
                if (w < lo || w >= hi)
                    return false;
                if (nearStart)
                {
                    auto it = std::upper_bound(targets.begin(), targets.end(), std::make_pair(w, UINTPTR_MAX));
                    return it != targets.begin() && w - std::prev(it)->first < 256;
                }
                return inRanges(targets, w);
            };
            forEachBusyBlock(heaps, false, [&](const uintptr_t* data, size_t size) {
                const uintptr_t block = uintptr_t(data);
                if (inRanges(targets, block))
                    return;
                for (size_t i = 0; i < size / sizeof(uintptr_t); ++i)
                    if (hit(data[i]))
                    {
                        found.push_back({block + i * sizeof(uintptr_t), block, data[0], size, false});
                        return;
                    }
            });
            for (const auto& [glo, ghi] : globals)
                for (uintptr_t a = glo; a + sizeof(uintptr_t) <= ghi; a += sizeof(uintptr_t))
                    if (hit(*reinterpret_cast<const uintptr_t*>(a)))
                        found.push_back({a, 0, 0, 0, true});
            // Other threads' stacks change under the scan, so each region is read guarded.
            for (const auto& [olo, ohi] : others)
                if (const uintptr_t a = firstPointerInto(olo, ohi, targets.data(), targets.size()))
                    found.push_back({a, olo, 0, size_t(ohi - olo), false, true});
            targets.clear();
            for (const Found& f : found)
                if (!f.global && !f.other && !inRanges(images, f.first) && targets.size() < 4096 && visited.emplace(f.block, 1).second)
                    targets.push_back({f.block, f.block + f.size});
            levels.push_back(std::move(found));
        }
    }
    for (HANDLE h : heaps)
        HeapUnlock(h);

    for (const ScratchVector<Found>& found : levels)
    {
        std::unordered_map<std::string, size_t> index;
        std::vector<HeapHolders::Holder> holders;
        for (const Found& f : found)
        {
            std::string what;
            if (f.global)
            {
                uint64_t d = 0;
                what = "global " + symbols.name(f.at, &d);
                if (what == "global ")
                    what += "?";
                what += " +" + std::to_string(d);
            }
            else if (f.other)
                what = "memory outside the heaps (a stack, Python, TBB) in a region of " + std::to_string(f.size / 1024) + " KB";
            else if (inRanges(images, f.first))
            {
                what = symbols.vtable(f.first);
                if (what.empty())
                    what = "block starting with " + codeAddress(symbols, f.first);
                what += " (+" + std::to_string(f.at - f.block) + ")";
            }
            else
                what = "block of " + std::to_string(f.size) + " bytes";
            auto [it, added] = index.emplace(what, holders.size());
            if (added)
                holders.push_back({what, 0});
            ++holders[it->second].count;
        }
        std::sort(holders.begin(), holders.end(), [](const auto& a, const auto& b) { return a.count > b.count; });
        if (holders.size() > 40)
            holders.resize(40);
        out.levels.push_back(std::move(holders));
    }
#else
    (void)type;
    (void)samples;
    (void)depth;
#endif
    return out;
}
} // namespace lila
