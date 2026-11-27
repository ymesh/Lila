// Process memory for worldBuild's per-step budget (kept out of the unity build: it includes windows.h).

#include "WorldScene.h"

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <psapi.h>
#endif

namespace openworld::world
{
namespace
{
#ifdef _WIN32
PROCESS_MEMORY_COUNTERS counters()
{
    PROCESS_MEMORY_COUNTERS c{};
    c.cb = sizeof(c);
    K32GetProcessMemoryInfo(GetCurrentProcess(), &c, sizeof(c));
    return c;
}
#endif
} // namespace

double memoryMB()
{
#ifdef _WIN32
    return double(counters().WorkingSetSize) / (1024.0 * 1024.0);
#else
    return 0.0;
#endif
}

double peakMemoryMB()
{
#ifdef _WIN32
    return double(counters().PeakWorkingSetSize) / (1024.0 * 1024.0);
#else
    return 0.0;
#endif
}
} // namespace openworld::world
