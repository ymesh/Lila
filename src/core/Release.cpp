#include "core/Release.h"

#include "core/Profile.h"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <deque>
#include <mutex>
#include <thread>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

namespace lila::release
{
namespace
{
struct State
{
    std::mutex mutex;
    std::condition_variable wake, idle;
    std::deque<std::unique_ptr<Piece>> queue;
    size_t busy = 0;
    std::atomic<int> holds{0};
    std::atomic<uint64_t> finished{0};
    /** steady_clock ticks when the last Hold ended. */
    std::atomic<int64_t> heldUntil{0};
    bool started = false;
};

/** Never destroyed: pieces may still be queued when statics are torn down at exit. */
State& state()
{
    static State* s = new State;
    return *s;
}

/**
 * Below normal while viewports draw, so freeing never takes a core from a frame; normal once none has for a moment
 * (headless, a long command, a build), so a freed world comes back as fast as the heap allows.
 */
void keepOutOfFrames(State& s)
{
    constexpr auto kQuiet = std::chrono::milliseconds(250);
    const bool drawing = s.holds.load(std::memory_order_acquire) > 0 ||
                         std::chrono::steady_clock::now().time_since_epoch().count() - s.heldUntil.load(std::memory_order_acquire) <
                             std::chrono::duration_cast<std::chrono::steady_clock::duration>(kQuiet).count();
    static thread_local int was = -1;
    if (int(drawing) == was)
        return;
    was = int(drawing);
#ifdef _WIN32
    SetThreadPriority(GetCurrentThread(), drawing ? THREAD_PRIORITY_BELOW_NORMAL : THREAD_PRIORITY_NORMAL);
#endif
}

void run()
{
    LILA_THREAD_NAME("lila releaser");
    State& s = state();
    for (;;)
    {
        std::unique_ptr<Piece> p;
        {
            std::unique_lock lock(s.mutex);
            s.wake.wait(lock, [&] { return !s.queue.empty(); });
            p = std::move(s.queue.front());
            s.queue.pop_front();
            ++s.busy;
        }
        for (bool more = true; more;)
        {
            // Woken the moment the last Hold ends, so the gaps between frames are used, however short.
            for (int h; (h = s.holds.load(std::memory_order_acquire)) > 0;)
                s.holds.wait(h, std::memory_order_acquire);
            keepOutOfFrames(s);
            LILA_ZONE_N("release.step");
            more = p->step();
        }
        p.reset();
        s.finished.fetch_add(1, std::memory_order_release);
        std::scoped_lock lock(s.mutex);
        --s.busy;
        if (s.queue.empty() && s.busy == 0)
            s.idle.notify_all();
    }
}
} // namespace

void enqueue(std::unique_ptr<Piece> piece)
{
    if (!piece)
        return;
    State& s = state();
    {
        std::scoped_lock lock(s.mutex);
        s.queue.push_back(std::move(piece));
        if (!s.started)
        {
            s.started = true;
            std::thread(run).detach();
        }
    }
    s.wake.notify_one();
}

Hold::Hold() { state().holds.fetch_add(1, std::memory_order_acq_rel); }
Hold::~Hold()
{
    State& s = state();
    s.heldUntil.store(std::chrono::steady_clock::now().time_since_epoch().count(), std::memory_order_release);
    if (s.holds.fetch_sub(1, std::memory_order_acq_rel) == 1)
        s.holds.notify_all();
}

void drain()
{
    State& s = state();
    std::unique_lock lock(s.mutex);
    s.idle.wait(lock, [&] { return s.queue.empty() && s.busy == 0; });
}

uint64_t finished() { return state().finished.load(std::memory_order_acquire); }

size_t pending()
{
    State& s = state();
    std::scoped_lock lock(s.mutex);
    return s.queue.size() + s.busy;
}
} // namespace lila::release
