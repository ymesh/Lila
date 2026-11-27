#pragma once

#include "core/Export.h"

#include <array>
#include <cstddef>
#include <string>
#include <vector>

namespace lila
{
/** The machine's physical memory in bytes; 0 when it can't be read. */
LILA_FOUNDATION_API size_t physicalMemoryBytes();

/** This process's private commit and working set in bytes. */
struct ProcessMemory
{
    size_t commit = 0, workingSet = 0;
};
LILA_FOUNDATION_API ProcessMemory processMemory();

/**
 * What the process heaps hold, from walking them: allocated (busy) against committed but free. committed counts the heap
 * segments only; blocks too big for a segment are allocated on their own and count in busy alone.
 */
struct HeapUse
{
    size_t heaps = 0, committed = 0, busy = 0, free = 0, busyBlocks = 0, freeBlocks = 0;
    /** Busy bytes by block size: bucket i holds blocks of 2^(i+6) bytes up to the next (the first under 128, the last 4 GB and over). */
    std::array<size_t, 27> busyBySize{};
};
/** Walks every process heap with each one locked in turn: seconds on a heap of tens of gigabytes. */
LILA_FOUNDATION_API HeapUse heapUse();

/** Live heap objects of one polymorphic type (a class with virtual functions, or a make_shared control block). */
struct HeapType
{
    std::string name;
    size_t count = 0, bytes = 0;
};
/**
 * Counts heap blocks that start with a vtable, by the vtable's class (from the symbols), most numerous first. Objects
 * held by shared_ptr from make_shared show as their control block (std::_Ref_count_obj2<T>). bytes is the blocks' own
 * size, not what the objects own. Walks the heaps like heapUse and loads symbols: slow, for diagnosing leaks.
 */
LILA_FOUNDATION_API std::vector<HeapType> heapTypes(size_t top);

/** Who holds live objects of a type: one level of references, each holder described by type, size or global. */
struct HeapHolders
{
    struct Holder
    {
        std::string what;
        size_t count = 0;
    };
    /** Per level: level 0 holds the objects themselves, level 1 holds untyped holders of level 0, and so on. */
    std::vector<std::vector<Holder>> levels;
    size_t objects = 0;
};
/**
 * Follows references back from up to `samples` live objects whose vtable's class contains `type` (as heapTypes names it),
 * `depth` levels (type "size>=N" follows blocks of N bytes or more instead, to find who owns big arrays): each heap
 * block or global that points into a target is described by its class (when it starts with a
 * vtable), the static variable it is (when it lies in a module), or its size; untyped blocks become the next level's
 * targets. Holds every heap lock while it scans, so the whole process stops for tens of seconds on a big scene.
 */
LILA_FOUNDATION_API HeapHolders heapHolders(const std::string& type, size_t samples, size_t depth);
} // namespace lila
