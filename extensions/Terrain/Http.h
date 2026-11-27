#pragma once

#include <cstdint>
#include <filesystem>
#include <functional>
#include <string>
#include <vector>

namespace terrain
{
/** A transfer's bytes so far and its size (0 when the server didn't say); false stops the transfer. */
using HttpProgress = std::function<bool(uint64_t received, uint64_t total)>;

/**
 * Sends the progress of every transfer on this thread to fn while it lives. Without one, each transfer opens its own
 * progress scope ("Downloading", host, bytes and rate, cancellable when it is the only operation running).
 */
class HttpProgressHook
{
public:
    explicit HttpProgressHook(HttpProgress fn);
    ~HttpProgressHook();
    HttpProgressHook(const HttpProgressHook&) = delete;
    HttpProgressHook& operator=(const HttpProgressHook&) = delete;

private:
    HttpProgress mFn;
    HttpProgress* mPrev = nullptr;
};

/** Reports bytes to this thread's hook; false when it asks to stop. True when no hook is installed. Stand-in downloaders call it too. */
bool httpReportProgress(uint64_t received, uint64_t total);

/** "1.2 MB", "340 KB": transfer sizes for progress lines. */
std::string httpSize(uint64_t bytes);

/** User agent for every request: names Lila and where to reach its maintainers, as Nominatim's policy asks. */
const char* httpUserAgent();

/**
 * GET url into dest (Qt Network, redirects followed, 120 s without progress times out). Returns the HTTP status, or 0
 * with why set when the server was never reached or the transfer was stopped. Blocks, running a local event loop.
 * With LILA_FETCH_MIRROR set (such as http://cache.studio:8080), https://host/path is fetched from <mirror>/host/path.
 */
int httpGet(const std::string& url, const std::filesystem::path& dest, std::string* why);

/** GET url as text; status as httpGet. */
int httpGetText(const std::string& url, std::string* body, std::string* why);

/** GET count bytes of url from offset (an HTTP Range request); returns 206 or 200 when out has them, as httpGet otherwise. */
int httpGetRange(const std::string& url, uint64_t offset, uint64_t count, std::vector<uint8_t>* out, std::string* why);

/** Destroys the application object httpGet made when the host had none; called while the extension unloads, before its DLL goes. */
void httpShutdown();
} // namespace terrain
