#include "link/RpcClient.h"

#include "core/Log.h"
#include "core/MainQueue.h"
#include "core/Profile.h"
#include "core/Progress.h"

#include <algorithm>
#include <condition_variable>
#include <deque>
#include <memory>
#include <vector>

namespace lila::link
{
namespace
{
using Clock = std::chrono::steady_clock;

double secondsSince(Clock::time_point t)
{
    return std::chrono::duration<double>(Clock::now() - t).count();
}
} // namespace

RpcClient::~RpcClient()
{
    disconnect();
}

bool RpcClient::connect(const std::string& host, uint16_t port, int timeoutMs, std::string* err)
{
    LILA_ZONE();

    disconnect();
    TcpSocket s = TcpSocket::connect(host, port, timeoutMs, err);
    if (!s.valid())
        return false;
    mSocket = std::move(s);
    mPort = port;
    mStopping = false;
    mConnected = true;
    mReader = std::thread([this] { readLoop(); });
    return true;
}

void RpcClient::disconnect()
{
    LILA_ZONE();

    mStopping = true;
    // shutdown() rather than close(): the reader may be inside recv on this handle.
    mSocket.shutdown();
    if (mReader.joinable())
        mReader.join();
    {
        std::scoped_lock lock(mSendMutex);
        mSocket.close();
    }
    mConnected = false;
    failAll(RpcResult::kDisconnected, "disconnected");
}

void RpcClient::onDisconnected(std::function<void()> fn)
{
    std::scoped_lock lock(mPendingMutex);
    mOnDisconnected = std::move(fn);
}

void RpcClient::callAsync(const std::string& method, const json& params, int timeoutMs, Callback done)
{
    callAsync(method, params, CallOptions{.timeoutMs = timeoutMs}, std::move(done));
}

void RpcClient::callAsync(const std::string& method, const json& params, CallOptions opts, Callback done)
{
    LILA_ZONE();

    if (!mConnected)
    {
        done({.ok = false, .code = RpcResult::kDisconnected, .error = "not connected"});
        return;
    }

    int64_t id;
    {
        std::scoped_lock lock(mPendingMutex);
        id = mNextId++;
        auto now = Clock::now();
        mPending[id] = {.method = method,
                        .done = std::move(done),
                        .onProgress = std::move(opts.onProgress),
                        .sent = now,
                        .deadline = now + std::chrono::milliseconds(opts.timeoutMs),
                        .lastHeard = now,
                        .idle = std::chrono::milliseconds(std::max(0, opts.idleTimeoutMs))};
    }

    json req = {{"jsonrpc", "2.0"}, {"id", id}, {"method", method}, {"params", params.is_null() ? json::object() : params}};
    std::string line = req.dump(-1, ' ', false, json::error_handler_t::replace) + "\n";
    log::debug("Unreal link: -> {} #{} ({} bytes)", method, id, line.size());
    bool sent;
    {
        std::scoped_lock lock(mSendMutex);
        sent = mSocket.sendAll(line);
    }
    // The reader clears mConnected before failing everything, so a request added after that is caught here.
    if (!sent || !mConnected)
    {
        Callback cb;
        {
            std::scoped_lock lock(mPendingMutex);
            auto it = mPending.find(id);
            if (it == mPending.end())
                return;
            cb = std::move(it->second.done);
            mPending.erase(it);
        }
        cb({.ok = false, .code = RpcResult::kDisconnected, .error = "send failed"});
    }
}

void RpcClient::callOnMain(const std::string& method, const json& params, int timeoutMs, Callback done)
{
    callOnMain(method, params, CallOptions{.timeoutMs = timeoutMs}, std::move(done));
}

void RpcClient::callOnMain(const std::string& method, const json& params, CallOptions opts, Callback done)
{
    if (opts.onProgress)
        opts.onProgress = [fn = std::move(opts.onProgress)](const json& p) { mainq::post([fn, p] { fn(p); }); };
    callAsync(method, params, std::move(opts), [done = std::move(done)](RpcResult r) {
        mainq::post([done, r = std::move(r)]() mutable { done(std::move(r)); });
    });
}

RpcResult RpcClient::call(const std::string& method, const json& params, int timeoutMs)
{
    return call(method, params, CallOptions{.timeoutMs = timeoutMs});
}

RpcResult RpcClient::call(const std::string& method, const json& params, const CallOptions& opts)
{
    LILA_ZONE();

    // Progress is handed over to this thread, so onProgress may touch state the caller owns.
    struct Waiter
    {
        std::mutex m;
        std::condition_variable cv;
        bool done = false;
        RpcResult result;
        std::deque<json> progress;
    };
    auto w = std::make_shared<Waiter>();
    CallOptions async{.timeoutMs = opts.timeoutMs, .idleTimeoutMs = opts.idleTimeoutMs};
    if (opts.onProgress)
    {
        async.onProgress = [w](const json& p) {
            std::scoped_lock lock(w->m);
            w->progress.push_back(p);
            w->cv.notify_all();
        };
    }
    callAsync(method, params, std::move(async), [w](RpcResult r) {
        std::scoped_lock lock(w->m);
        w->result = std::move(r);
        w->done = true;
        w->cv.notify_all();
    });
    std::unique_lock lock(w->m);
    for (;;)
    {
        w->cv.wait(lock, [&] { return w->done || !w->progress.empty(); });
        while (!w->progress.empty())
        {
            json p = std::move(w->progress.front());
            w->progress.pop_front();
            lock.unlock();
            opts.onProgress(p);
            lock.lock();
        }
        if (w->done)
            return std::move(w->result);
    }
}

std::vector<RpcResult> RpcClient::callMany(const std::vector<Request>& requests, size_t window)
{
    LILA_ZONE();

    RpcPipeline p(*this, window);
    for (const Request& r : requests)
        p.send(r.method, r.params, r.timeoutMs);
    return p.finish();
}

void RpcClient::readLoop()
{
    LILA_THREAD_NAME("Unreal link");

    std::string buffer;
    char chunk[16 * 1024];
    while (!mStopping)
    {
        int n = mSocket.recv(chunk, sizeof(chunk), 50);
        if (n < 0)
            break;
        if (n > 0)
        {
            buffer.append(chunk, static_cast<size_t>(n));
            size_t start = 0;
            for (size_t nl; (nl = buffer.find('\n', start)) != std::string::npos; start = nl + 1)
            {
                size_t end = nl;
                if (end > start && buffer[end - 1] == '\r')
                    --end;
                if (end > start)
                    handleLine(buffer.substr(start, end - start));
            }
            buffer.erase(0, start);
        }
        expire();
    }

    bool dropped = !mStopping;
    mConnected = false;
    if (dropped)
        log::debug("Unreal link: port {} closed by the editor", mPort);
    failAll(RpcResult::kDisconnected, "the Unreal editor closed the connection");
    if (dropped)
    {
        std::function<void()> fn;
        {
            std::scoped_lock lock(mPendingMutex);
            fn = mOnDisconnected;
        }
        if (fn)
            fn();
    }
}

void RpcClient::handleLine(const std::string& line)
{
    LILA_ZONE();

    json msg = json::parse(line, nullptr, false);
    if (msg.is_discarded() || !msg.is_object())
        return;
    if (msg.contains("method") && msg["method"] == "progress" && msg.contains("params") && msg["params"].is_object())
        return handleProgress(msg["params"]);
    if (!msg.contains("id") || !msg["id"].is_number_integer())
        return; // other notifications and junk; nothing is waiting on them

    int64_t id = msg["id"].get<int64_t>();
    Callback cb;
    std::string method;
    double took = 0.0;
    {
        std::scoped_lock lock(mPendingMutex);
        auto it = mPending.find(id);
        if (it == mPending.end())
        {
            log::debug("Unreal link: late reply to #{} ignored", id);
            return;
        }
        cb = std::move(it->second.done);
        method = it->second.method;
        took = secondsSince(it->second.sent);
        mPending.erase(it);
    }

    RpcResult r;
    if (msg.contains("error") && msg["error"].is_object())
    {
        r.code = msg["error"].value("code", 0);
        r.error = msg["error"].value("message", std::string("error"));
    }
    else
    {
        r.ok = true;
        r.result = msg.value("result", json());
    }
    log::debug("Unreal link: <- {} #{} {} in {:.2f} s", method, id, r.ok ? "ok" : "failed", took);
    cb(std::move(r));
}

void RpcClient::handleProgress(const json& params)
{
    if (!params.contains("id") || !params["id"].is_number_integer())
        return;
    std::function<void(const json&)> fn;
    {
        std::scoped_lock lock(mPendingMutex);
        auto it = mPending.find(params["id"].get<int64_t>());
        if (it == mPending.end())
            return;
        it->second.lastHeard = Clock::now();
        fn = it->second.onProgress;
    }
    if (fn)
        fn(params);
}

void RpcClient::expire()
{
    auto now = Clock::now();
    std::vector<std::pair<Callback, std::string>> late;
    {
        std::scoped_lock lock(mPendingMutex);
        for (auto it = mPending.begin(); it != mPending.end();)
        {
            const Pending& p = it->second;
            std::string why;
            if (p.deadline <= now)
            {
                why = std::format("{} timed out after {}", p.method, progress::formatElapsed(secondsSince(p.sent)));
            }
            else if (p.idle.count() > 0 && now - p.lastHeard >= p.idle)
            {
                why = std::format("Unreal stopped responding to {} ({} without a reply or progress); the editor is "
                                  "busy, waiting in a dialog, or hung. Check LogLilaLink in its log",
                                  p.method, progress::formatElapsed(secondsSince(p.lastHeard)));
            }
            if (why.empty())
            {
                ++it;
                continue;
            }
            log::debug("Unreal link: #{} {}", it->first, why);
            late.emplace_back(std::move(it->second.done), std::move(why));
            it = mPending.erase(it);
        }
    }
    for (auto& [cb, why] : late)
        cb({.ok = false, .code = RpcResult::kTimeout, .error = why});
}

void RpcClient::failAll(int code, const std::string& message)
{
    std::map<int64_t, Pending> pending;
    {
        std::scoped_lock lock(mPendingMutex);
        pending.swap(mPending);
    }
    for (auto& [id, p] : pending)
        p.done({.ok = false, .code = code, .error = message});
}
struct RpcPipeline::State
{
    std::mutex m;
    std::condition_variable cv;
    std::vector<RpcResult> results;
    size_t inFlight = 0, done = 0;
};

RpcPipeline::RpcPipeline(RpcClient& client, size_t window) : mClient(client), mWindow(std::max<size_t>(1, window)), mState(std::make_shared<State>()) {}

RpcPipeline::~RpcPipeline()
{
    // Replies still on their way write into the shared state, which outlives this.
    std::unique_lock lock(mState->m);
    mState->cv.wait(lock, [&] { return mState->inFlight == 0; });
}

size_t RpcPipeline::send(const std::string& method, const json& params, int timeoutMs)
{
    return send(method, params, CallOptions{.timeoutMs = timeoutMs});
}

size_t RpcPipeline::send(const std::string& method, const json& params, CallOptions opts)
{
    size_t i;
    {
        std::unique_lock lock(mState->m);
        mState->cv.wait(lock, [&] { return mState->inFlight < mWindow; });
        ++mState->inFlight;
        i = mState->results.size();
        mState->results.emplace_back();
    }
    mClient.callAsync(method, params, std::move(opts), [st = mState, i](RpcResult r) {
        std::scoped_lock lock(st->m);
        st->results[i] = std::move(r);
        --st->inFlight;
        ++st->done;
        st->cv.notify_all();
    });
    return i;
}

std::vector<RpcResult> RpcPipeline::finish()
{
    LILA_ZONE();

    std::unique_lock lock(mState->m);
    mState->cv.wait(lock, [&] { return mState->inFlight == 0; });
    std::vector<RpcResult> out = std::move(mState->results);
    mState->results.clear();
    mState->done = 0;
    return out;
}

} // namespace lila::link
