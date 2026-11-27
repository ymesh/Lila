// HTTP downloads for terrain sources, on Qt Network like the rest of Lila's downloads.

#include "Http.h"

#include <lila/Log.h>
#include <lila/Tasks.h>

#include <QCoreApplication>
#include <QEventLoop>
#include <QFile>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QTimer>
#include <QUrl>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdlib>
#include <format>
#include <memory>
#include <optional>

namespace terrain
{
namespace httpdetail
{
/** The application object made here when the host had none. */
QCoreApplication* ownedApp = nullptr;

thread_local HttpProgress* tHook = nullptr;

/** url fetched through LILA_FETCH_MIRROR when it is set; said once in the log, so the source is never a surprise. */
std::string mirrored(const std::string& url)
{
    static const std::string mirror = [] {
        const char* m = std::getenv("LILA_FETCH_MIRROR");
        std::string s = m ? m : "";
        while (!s.empty() && s.back() == '/')
            s.pop_back();
        return s;
    }();
    if (mirror.empty())
        return url;
    const size_t scheme = url.find("://");
    if (scheme == std::string::npos)
        return url;
    static std::atomic<bool> said{false};
    if (!said.exchange(true))
        lila::log::info("downloads go through the mirror {} (LILA_FETCH_MIRROR)", mirror);
    return mirror + "/" + url.substr(scheme + 3);
}

/** Progress of one transfer when no hook takes it: its own scope, with the host, bytes and rate as the status. */
class DefaultProgress
{
public:
    explicit DefaultProgress(const QUrl& url)
        : mHost(url.host().toStdString()), mScope("Downloading", 0.0, lila::progress::Cancellable), mStart(std::chrono::steady_clock::now())
    {
        mScope.setStatus(mHost);
    }

    bool update(uint64_t received, uint64_t total)
    {
        if (total > 0 && double(total) != mScope.total())
            mScope.setTotal(double(total));
        mScope.set(double(received));
        const double s = std::chrono::duration<double>(std::chrono::steady_clock::now() - mStart).count();
        std::string line = mHost + "  " + httpSize(received);
        if (total > 0)
            line += " of " + httpSize(total);
        if (s > 0.5)
            line += std::format(", {}/s", httpSize(uint64_t(double(received) / s)));
        mScope.setStatus(std::move(line));
        return !mScope.cancelled();
    }

private:
    std::string mHost;
    lila::progress::Scope mScope;
    std::chrono::steady_clock::time_point mStart;
};

/** Qt Network needs an application object; lilabatch and test runners may have none. */
void ensureApp()
{
    if (QCoreApplication::instance())
        return;
    static int argc = 1;
    static char name[] = "lila";
    static char* argv[] = {name, nullptr};
    ownedApp = new QCoreApplication(argc, argv);
}

/** Runs one GET, handing each chunk of a 200 or 206 answer to sink; false from sink aborts. range is a Range header value. */
int get(const std::string& url, const std::function<bool(const QByteArray&, int status)>& sink, std::string* why, const std::string& range = {})
{
    ensureApp();
    QNetworkAccessManager nam;
    const QUrl qurl(QString::fromStdString(mirrored(url)));
    QNetworkRequest req{qurl};
    req.setAttribute(QNetworkRequest::RedirectPolicyAttribute, QNetworkRequest::NoLessSafeRedirectPolicy);
    req.setHeader(QNetworkRequest::UserAgentHeader, QString::fromUtf8(httpUserAgent()));
    if (!range.empty())
        req.setRawHeader("Range", QByteArray::fromStdString(range));
    QNetworkReply* reply = nam.get(req);
    QEventLoop loop;
    QTimer idle;
    idle.setSingleShot(true);
    idle.setInterval(120000);
    bool timedOut = false, sinkFailed = false, stopped = false;
    QObject::connect(&idle, &QTimer::timeout, &loop, [&] {
        timedOut = true;
        reply->abort();
    });
    std::optional<DefaultProgress> own;
    if (!tHook)
        own.emplace(qurl);
    QObject::connect(reply, &QNetworkReply::downloadProgress, &loop, [&](qint64 received, qint64 total) {
        const uint64_t r = uint64_t(std::max<qint64>(0, received)), t = uint64_t(std::max<qint64>(0, total));
        if (!(own ? own->update(r, t) : httpReportProgress(r, t)) && !stopped)
        {
            stopped = true;
            reply->abort();
        }
    });
    QObject::connect(reply, &QNetworkReply::readyRead, &loop, [&] {
        idle.start();
        const int status = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
        if ((status == 200 || status == 206) && !sink(reply->readAll(), status))
        {
            sinkFailed = true;
            reply->abort();
        }
    });
    QObject::connect(reply, &QNetworkReply::finished, &loop, &QEventLoop::quit);
    idle.start();
    loop.exec();
    const int status = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
    const bool ok = status == 200 || status == 206;
    if (stopped)
    {
        if (why)
            *why = "stopped";
    }
    else if (ok && !sinkFailed && !timedOut)
        sink(reply->readAll(), status);
    else if (!ok && why)
    {
        // Error bodies are short text (OpenTopography explains bad keys and areas there).
        const QByteArray body = reply->readAll().left(400);
        *why = timedOut ? std::string("no answer for 120 s") : sinkFailed ? std::string("can't write the download") : reply->errorString().toStdString();
        if (!body.isEmpty() && !timedOut)
            *why += ": " + body.toStdString();
    }
    reply->deleteLater();
    if (timedOut || sinkFailed || stopped)
        return 0;
    return status;
}
} // namespace httpdetail

HttpProgressHook::HttpProgressHook(HttpProgress fn) : mFn(std::move(fn)), mPrev(httpdetail::tHook) { httpdetail::tHook = &mFn; }

HttpProgressHook::~HttpProgressHook() { httpdetail::tHook = mPrev; }

bool httpReportProgress(uint64_t received, uint64_t total)
{
    return !httpdetail::tHook || !*httpdetail::tHook || (*httpdetail::tHook)(received, total);
}

std::string httpSize(uint64_t bytes)
{
    if (bytes < 1048576)
        return std::format("{} KB", (bytes + 1023) / 1024);
    return std::format("{:.1f} MB", double(bytes) / 1048576.0);
}

const char* httpUserAgent() { return "Lila/1 (3D app terrain import; https://github.com/lila-3d/Lila)"; }

int httpGet(const std::string& url, const std::filesystem::path& dest, std::string* why)
{
    QFile out(QString::fromStdWString(dest.wstring()));
    if (!out.open(QIODevice::WriteOnly | QIODevice::Truncate))
    {
        if (why)
            *why = "can't write " + dest.string();
        return 0;
    }
    const int status = httpdetail::get(url, [&](const QByteArray& chunk, int) { return out.write(chunk) == chunk.size(); }, why);
    out.close();
    return status;
}

int httpGetText(const std::string& url, std::string* body, std::string* why)
{
    std::string text;
    const int status = httpdetail::get(url, [&](const QByteArray& chunk, int) {
        text.append(chunk.constData(), size_t(chunk.size()));
        return true;
    }, why);
    if (body)
        *body = std::move(text);
    return status;
}

int httpGetRange(const std::string& url, uint64_t offset, uint64_t count, std::vector<uint8_t>* out, std::string* why)
{
    out->clear();
    if (count == 0)
        return 206;
    uint64_t at = 0;
    const int status = httpdetail::get(url, [&](const QByteArray& chunk, int st) {
        // A server that ignores Range sends the whole file from the start.
        const uint64_t from = st == 200 ? offset : 0;
        const uint64_t begin = std::max(at, from), end = std::min(at + uint64_t(chunk.size()), from + count);
        if (end > begin)
            out->insert(out->end(), chunk.constData() + (begin - at), chunk.constData() + (end - at));
        at += uint64_t(chunk.size());
        return out->size() < count;
    }, why, "bytes=" + std::to_string(offset) + "-" + std::to_string(offset + count - 1));
    if (status == 0 && out->size() >= count)
        return 206;
    return status;
}

void httpShutdown()
{
    delete httpdetail::ownedApp;
    httpdetail::ownedApp = nullptr;
}
} // namespace terrain
