#include "core/MainQueue.h"

#include "core/CrashHandler.h"
#include "core/Profile.h"

#include <mutex>
#include <vector>

namespace lila::mainq
{
namespace
{
std::thread::id gMainId;
std::mutex gMutex;
struct Task
{
    std::function<void()> fn;
    const char* label = nullptr;
};
std::vector<Task> gQueue;
std::function<void()> gWake;
int gHolds = 0; // main thread only
} // namespace

void init()
{
    gMainId = std::this_thread::get_id();
}

bool isMainThread()
{
    return std::this_thread::get_id() == gMainId;
}

void post(std::function<void()> fn)
{
    post(nullptr, std::move(fn));
}

void post(const char* label, std::function<void()> fn)
{
    std::function<void()> wake;
    {
        std::scoped_lock lock(gMutex);
        gQueue.push_back({std::move(fn), label});
        wake = gWake;
    }
    if (wake)
        wake();
}

void drain()
{
    LILA_ZONE();

    if (gHolds > 0)
        return;
    std::vector<Task> work;
    {
        std::scoped_lock lock(gMutex);
        work.swap(gQueue);
    }
    for (Task& t : work)
    {
        crash::setContext("context", "mainTask", t.label ? t.label : "unnamed");
        t.fn();
    }
    if (!work.empty())
        crash::setContext("context", "mainTask", "");
}

Hold::Hold()
{
    ++gHolds;
}

Hold::~Hold()
{
    if (--gHolds > 0)
        return;
    std::function<void()> wake;
    {
        std::scoped_lock lock(gMutex);
        if (!gQueue.empty())
            wake = gWake;
    }
    if (wake)
        wake();
}

void setWakeup(std::function<void()> wake)
{
    std::scoped_lock lock(gMutex);
    gWake = std::move(wake);
}
} // namespace lila::mainq
