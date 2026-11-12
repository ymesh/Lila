#include "core/Progress.h"

#include "core/MainQueue.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <format>
#include <mutex>
#include <thread>

namespace lila::progress
{
using Clock = std::chrono::steady_clock;

struct Node : std::enable_shared_from_this<Node>
{
    uint64_t id = 0;
    std::string title;
    std::string status; // guarded by gMutex
    std::atomic<double> done{0.0};
    std::atomic<double> total{0.0};
    double span = 1.0;
    uint32_t flags = None;
    bool main = false;
    Node* parent = nullptr;
    Node* root = nullptr;
    std::vector<Node*> children; // guarded by gMutex
    std::atomic<bool> cancelled{false}; // read on the root
    std::atomic<bool> immediate{false}; // read on the root
    Clock::time_point start = Clock::now();
};

namespace
{
std::mutex gMutex;
std::vector<std::shared_ptr<Node>> gRoots;
std::atomic<uint64_t> gNextId{1};
thread_local Node* tCurrent = nullptr;
thread_local uint64_t tCancelledClosed = 0;

std::atomic<bool> gListening{false};
std::function<void()> gPump;
int gPumpInterval = 30;
Clock::time_point gLastPump;
bool gInPump = false;

std::mutex gReporterMutex;
std::function<void(const std::vector<Snapshot>&)> gReporter;
int gReporterInterval = 100;
std::atomic<int64_t> gLastReport{0};

int64_t nowMs()
{
    return std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now().time_since_epoch()).count();
}

void updateListening()
{
    bool reporter;
    {
        std::scoped_lock lock(gReporterMutex);
        reporter = bool(gReporter);
    }
    gListening.store(bool(gPump) || reporter, std::memory_order_relaxed);
}

/** Caller holds gMutex. */
double fractionOf(const Node& n)
{
    const double t = n.total.load(std::memory_order_relaxed);
    if (t <= 0.0)
        return 0.0;
    double sub = 0.0;
    for (const Node* c : n.children)
        sub += fractionOf(*c) * c->span;
    return std::clamp((n.done.load(std::memory_order_relaxed) + sub) / t, 0.0, 1.0);
}

/** Caller holds gMutex. Follows the newest child down to the deepest open scope. */
std::string statusOf(const Node& root)
{
    std::string status = root.status, title;
    for (const Node* n = root.children.empty() ? nullptr : root.children.back(); n;
         n = n->children.empty() ? nullptr : n->children.back())
    {
        if (!n->status.empty())
            status = n->status;
        else if (!n->title.empty())
            status = n->title;
    }
    return status;
}

Snapshot snapshotOf(const Node& r)
{
    Snapshot s;
    s.id = r.id;
    s.title = r.title;
    s.status = statusOf(r);
    s.determinate = r.total.load(std::memory_order_relaxed) > 0.0;
    s.fraction = fractionOf(r);
    s.silent = r.flags & Silent;
    s.cancellable = r.flags & Cancellable;
    s.cancelled = r.cancelled.load(std::memory_order_relaxed);
    s.immediate = r.immediate.load(std::memory_order_relaxed);
    s.elapsed = std::chrono::duration<double>(Clock::now() - r.start).count();
    s.mainThread = r.main;
    return s;
}

void maybePump(bool force)
{
    if (!gPump || gInPump || !mainq::isMainThread())
        return;
    const Clock::time_point now = Clock::now();
    if (!force && now - gLastPump < std::chrono::milliseconds(gPumpInterval))
        return;
    gLastPump = now;
    gInPump = true;
    try
    {
        gPump();
    }
    catch (...)
    {
    }
    gInPump = false;
}

void maybeReport(bool force)
{
    const int64_t now = nowMs();
    int64_t last = gLastReport.load(std::memory_order_relaxed);
    if (!force && now - last < gReporterInterval)
        return;
    if (!gLastReport.compare_exchange_strong(last, now))
        return; // another thread is reporting this tick
    std::unique_lock lock(gReporterMutex, std::try_to_lock);
    if (!lock || !gReporter)
        return;
    gReporter(snapshot());
}

void notify(bool force = false)
{
    if (!gListening.load(std::memory_order_relaxed))
        return;
    maybePump(force);
    maybeReport(force);
}
} // namespace

Scope::Scope(std::string title, double total, uint32_t flags, double span)
{
    mNode = std::make_shared<Node>();
    Node& n = *mNode;
    n.id = gNextId.fetch_add(1);
    n.title = std::move(title);
    n.total = total;
    n.flags = flags;
    n.span = span;
    n.main = mainq::isMainThread();
    {
        std::scoped_lock lock(gMutex);
        if (Node* p = tCurrent)
        {
            n.parent = p;
            n.root = p->root;
            p->children.push_back(&n);
        }
        else
        {
            n.root = &n;
            gRoots.push_back(mNode);
        }
    }
    mPrevCurrent = tCurrent;
    tCurrent = &n;
    notify(n.parent == nullptr);
}

Scope::~Scope()
{
    Node& n = *mNode;
    const bool root = n.parent == nullptr;
    {
        std::scoped_lock lock(gMutex);
        if (n.parent)
            std::erase(n.parent->children, &n);
        else
            std::erase(gRoots, mNode);
    }
    tCurrent = mPrevCurrent;
    if (root && n.cancelled.load(std::memory_order_relaxed))
        ++tCancelledClosed;
    if (n.parent)
        n.parent->done.fetch_add(n.span, std::memory_order_relaxed);
    if (!gListening.load(std::memory_order_relaxed))
        return;
    if (root)
    {
        maybePump(true);
        maybeReport(true);
    }
    else
        notify();
}

void Scope::step(double n)
{
    mNode->done.fetch_add(n, std::memory_order_relaxed);
    notify();
}

void Scope::set(double done)
{
    mNode->done.store(done, std::memory_order_relaxed);
    notify();
}

void Scope::setTotal(double total)
{
    mNode->total.store(total, std::memory_order_relaxed);
    notify();
}

void Scope::setStatus(std::string text)
{
    {
        std::scoped_lock lock(gMutex);
        mNode->status = std::move(text);
    }
    notify();
}

void Scope::setCancellable(bool on)
{
    {
        std::scoped_lock lock(gMutex);
        mNode->flags = on ? (mNode->flags | Cancellable) : (mNode->flags & ~uint32_t(Cancellable));
    }
    notify();
}

void Scope::showNow()
{
    if (mNode->root->immediate.exchange(true, std::memory_order_relaxed))
        return;
    notify(true);
}

bool Scope::cancelled() const
{
    return mNode->root->cancelled.load(std::memory_order_relaxed);
}

void Scope::checkCancel() const
{
    if (cancelled())
        throw Cancelled();
}

double Scope::done() const
{
    return mNode->done.load(std::memory_order_relaxed);
}

double Scope::total() const
{
    return mNode->total.load(std::memory_order_relaxed);
}

std::shared_ptr<Node> current()
{
    return tCurrent ? tCurrent->shared_from_this() : nullptr;
}

Adopt::Adopt(std::shared_ptr<Node> parent) : mParent(std::move(parent))
{
    mPrev = tCurrent;
    tCurrent = mParent.get();
}

Adopt::~Adopt()
{
    tCurrent = mPrev;
}

std::vector<Snapshot> snapshot()
{
    std::scoped_lock lock(gMutex);
    std::vector<Snapshot> out;
    out.reserve(gRoots.size());
    for (const auto& r : gRoots)
        out.push_back(snapshotOf(*r));
    return out;
}

double fraction(const Scope& root)
{
    std::scoped_lock lock(gMutex);
    return fractionOf(*root.mNode);
}

bool cancelRequested()
{
    return tCurrent && tCurrent->root->cancelled.load(std::memory_order_relaxed);
}

void checkCancel()
{
    if (cancelRequested())
        throw Cancelled();
}

uint64_t cancelledCount()
{
    return tCancelledClosed;
}

void cancel(uint64_t id)
{
    std::scoped_lock lock(gMutex);
    for (const auto& r : gRoots)
        if (r->id == id)
            r->cancelled = true;
}

bool modal()
{
    std::scoped_lock lock(gMutex);
    return std::any_of(gRoots.begin(), gRoots.end(), [](const auto& r) { return r->main && !(r->flags & Silent); });
}

void setPump(std::function<void()> pump, int intervalMs)
{
    gPump = std::move(pump);
    gPumpInterval = intervalMs;
    updateListening();
}

void setReporter(std::function<void(const std::vector<Snapshot>&)> reporter, int intervalMs)
{
    {
        std::scoped_lock lock(gReporterMutex);
        gReporter = std::move(reporter);
        gReporterInterval = intervalMs;
    }
    gLastReport = 0;
    updateListening();
}

void pumpNow()
{
    maybePump(true);
}

void runOnWorker(const std::function<void()>& fn)
{
    if (!gPump || !mainq::isMainThread())
    {
        fn();
        return;
    }
    std::mutex m;
    std::condition_variable cv;
    bool finished = false;
    std::exception_ptr error;
    std::thread worker([&, parent = current()] {
        {
            Adopt adopt(parent);
            try
            {
                fn();
            }
            catch (...)
            {
                error = std::current_exception();
            }
        }
        std::scoped_lock lock(m);
        finished = true;
        cv.notify_all();
    });
    {
        std::unique_lock lock(m);
        while (!finished)
        {
            cv.wait_for(lock, std::chrono::milliseconds(gPumpInterval));
            if (finished)
                break;
            lock.unlock();
            maybePump(true);
            lock.lock();
        }
    }
    worker.join();
    if (error)
        std::rethrow_exception(error);
}

std::string formatElapsed(double seconds)
{
    if (seconds < 60.0)
        return std::format("{:.1f} s", seconds);
    const int s = int(seconds);
    return std::format("{} min {:02} s", s / 60, s % 60);
}

std::string consoleLine(const std::vector<Snapshot>& roots, size_t width)
{
    if (roots.empty())
        return {};
    auto it = std::find_if(roots.begin(), roots.end(), [](const Snapshot& s) { return !s.silent; });
    const Snapshot& s = it != roots.end() ? *it : roots.front();
    std::string line = s.title;
    if (s.determinate)
        line += std::format("  {:3d}%", int(std::floor(s.fraction * 100.0)));
    if (!s.status.empty())
        line += "  " + s.status;
    if (line.size() > width)
        line = line.substr(0, width - 3) + "...";
    return line;
}
} // namespace lila::progress
