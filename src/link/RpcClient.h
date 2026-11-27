#pragma once

#include "core/Export.h"

#include "link/Socket.h"

#include <nlohmann/json.hpp>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <functional>
#include <map>
#include <mutex>
#include <string>
#include <thread>

namespace lila::link
{
using json = nlohmann::json;

struct RpcResult
{
    bool ok = false;
    json result;
    int code = 0; // JSON-RPC error code; kTimeout / kDisconnected for local failures
    std::string error;

    static constexpr int kTimeout = -1;
    static constexpr int kDisconnected = -2;
};

struct CallOptions
{
    /** Fails the call this long after it was sent, however much progress came in. */
    int timeoutMs = 10000;
    /** Fails the call when neither its reply nor a progress notification arrived for this long. 0 is off. */
    int idleTimeoutMs = 0;
    /** Gets the params of each {"method":"progress","params":{"id":<this call>,...}} notification. */
    std::function<void(const json& progress)> onProgress;
};

/**
 * Newline-delimited JSON-RPC 2.0 client. A reader thread matches replies to requests by id, feeds progress notifications
 * to their call, and fails requests whose timeout passes. Thread-safe.
 */
class LILA_SCENE_API RpcClient
{
public:
    using Callback = std::function<void(RpcResult)>;

    RpcClient() = default;
    ~RpcClient();
    RpcClient(const RpcClient&) = delete;
    RpcClient& operator=(const RpcClient&) = delete;

    bool connect(const std::string& host, uint16_t port, int timeoutMs, std::string* err = nullptr);
    void disconnect();
    bool connected() const { return mConnected; }
    uint16_t port() const { return mPort; }

    /** Blocks the calling thread until the reply or a timeout. onProgress runs on this thread. Don't call on the reader thread. */
    RpcResult call(const std::string& method, const json& params, const CallOptions& opts);
    RpcResult call(const std::string& method, const json& params = json::object(), int timeoutMs = 10000);

    /** `done` and onProgress run on the reader thread (`done` on the calling thread for an immediate failure). */
    void callAsync(const std::string& method, const json& params, CallOptions opts, Callback done);
    void callAsync(const std::string& method, const json& params, int timeoutMs, Callback done);

    struct Request
    {
        std::string method;
        json params;
        int timeoutMs = 10000;
    };
    /**
     * Sends the requests with up to `window` of them awaiting replies at once, so Unreal answers several per frame rather
     * than one per round trip, and blocks until every one replied. Unreal starts them in the order given; the replies come
     * back in that order. Don't call on the reader thread.
     */
    std::vector<RpcResult> callMany(const std::vector<Request>& requests, size_t window = 32);

    /** Same, with `done` and onProgress posted to the main thread through mainq. */
    void callOnMain(const std::string& method, const json& params, CallOptions opts, Callback done);
    void callOnMain(const std::string& method, const json& params, int timeoutMs, Callback done);

    /** Fires (on the reader thread) when the connection drops. */
    void onDisconnected(std::function<void()> fn);

private:
    struct Pending
    {
        std::string method;
        Callback done;
        std::function<void(const json&)> onProgress;
        std::chrono::steady_clock::time_point sent;
        std::chrono::steady_clock::time_point deadline;
        std::chrono::steady_clock::time_point lastHeard;
        std::chrono::milliseconds idle{0};
    };

    void readLoop();
    void handleLine(const std::string& line);
    void handleProgress(const json& params);
    void failAll(int code, const std::string& message);
    void expire();

    TcpSocket mSocket;
    std::mutex mSendMutex;
    std::thread mReader;
    std::atomic<bool> mConnected{false};
    std::atomic<bool> mStopping{false};
    uint16_t mPort = 0;

    std::mutex mPendingMutex;
    std::map<int64_t, Pending> mPending;
    int64_t mNextId = 1;
    std::function<void()> mOnDisconnected;
};
/**
 * Calls sent without waiting for each reply, up to `window` awaiting at once, so Unreal answers several per frame rather
 * than one per round trip. Unreal starts them in the order sent. finish() blocks until every one replied and returns the
 * replies in send order. Use from one thread, never the reader thread.
 */
class LILA_SCENE_API RpcPipeline
{
public:
    RpcPipeline(RpcClient& client, size_t window = 32);
    ~RpcPipeline();
    RpcPipeline(const RpcPipeline&) = delete;
    RpcPipeline& operator=(const RpcPipeline&) = delete;

    /** Sends the call (waiting first while `window` are awaiting); returns its index in finish()'s replies. */
    size_t send(const std::string& method, const json& params, int timeoutMs = 10000);
    size_t send(const std::string& method, const json& params, CallOptions opts);
    std::vector<RpcResult> finish();

private:
    struct State;
    RpcClient& mClient;
    size_t mWindow;
    std::shared_ptr<State> mState;
};

} // namespace lila::link
