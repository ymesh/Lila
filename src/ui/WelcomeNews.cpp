#include "ui/WelcomeNews.h"

#include "core/Log.h"
#include "core/Paths.h"
#include "core/PrefStore.h"
#include "core/Version.h"
#include "ui/Updater.h"

#include <QApplication>
#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QPointer>
#include <QUrl>

#include <algorithm>
#include <memory>

namespace lila::ui::news
{
using json = nlohmann::json;

namespace
{
std::string feedUrl()
{
    if (const char* v = std::getenv("LILA_NEWS_URL"); v && *v)
        return v;
    return kFeedUrl;
}

bool enabled()
{
    return PrefStore::get().value<bool>("general", kEnabledPref, true);
}

QDir cacheDir()
{
    QDir dir(QString::fromStdWString((paths::userConfigDir() / "news").wstring()));
    dir.mkpath(".");
    return dir;
}

QNetworkAccessManager& nam()
{
    static QPointer<QNetworkAccessManager> nam;
    if (!nam)
        nam = new QNetworkAccessManager(qApp);
    return *nam;
}

QNetworkReply* get(const QString& url)
{
    QNetworkRequest req{QUrl(url)};
    req.setTransferTimeout(10000);
    req.setAttribute(QNetworkRequest::CacheLoadControlAttribute, QNetworkRequest::AlwaysNetwork);
    return nam().get(req);
}

QString cachePath(const Item& item)
{
    const QString url = QString::fromStdString(item.image);
    const QString hash = QCryptographicHash::hash(url.toUtf8(), QCryptographicHash::Sha1).toHex().left(16);
    const QString ext = QFileInfo(QUrl(url).path()).suffix();
    return cacheDir().filePath(ext.isEmpty() ? hash : hash + "." + ext);
}

std::vector<Item> readCache()
{
    QFile f(cacheDir().filePath("news.json"));
    if (!f.open(QIODevice::ReadOnly))
        return {};
    return parse(json::parse(f.readAll().toStdString(), nullptr, false), feedUrl(), version::number());
}
} // namespace

std::vector<Item> parse(const json& feed, std::string_view url, std::string_view version, size_t limit)
{
    std::vector<Item> out;
    const json& items = feed.is_object() ? feed.value("items", json::array()) : json::array();
    if (!items.is_array())
        return out;
    const QUrl base(QString::fromUtf8(url.data(), qsizetype(url.size())));
    for (const json& j : items)
    {
        if (!j.is_object() || !j.value("title", json()).is_string())
            continue;
        const std::string minVersion = j.value("minVersion", ""), maxVersion = j.value("maxVersion", "");
        if ((!minVersion.empty() && updater::compareVersions(version, minVersion) < 0) ||
            (!maxVersion.empty() && updater::compareVersions(version, maxVersion) > 0))
            continue;
        Item item;
        item.id = j.value("id", "");
        item.type = j.value("type", "news");
        item.date = j.value("date", "");
        item.title = j.value("title", "");
        item.body = j.value("body", "");
        item.author = j.value("author", "");
        item.authorUrl = j.value("authorUrl", "");
        item.until = j.value("until", "");
        item.url = j.value("url", "");
        if (const std::string image = j.value("image", ""); !image.empty())
            item.image = base.resolved(QUrl(QString::fromStdString(image))).toString().toStdString();
        if (item.id.empty())
            item.id = item.date + " " + item.title;
        out.push_back(std::move(item));
    }
    // ISO dates sort as text; stable so same-day items keep the feed's order.
    std::stable_sort(out.begin(), out.end(), [](const Item& a, const Item& b) { return a.date > b.date; });
    size_t news = 0, showcase = 0;
    std::erase_if(out, [&](const Item& item) { return ++(isShowcase(item) ? showcase : news) > limit; });
    return out;
}

bool isShowcase(const Item& item)
{
    return item.type == "spotlight";
}

bool running(const Item& item, std::string_view today)
{
    return item.until.empty() || std::string_view(item.until) >= today;
}

size_t firstUnseen(const std::vector<Item>& items, const std::vector<std::string>& seen)
{
    for (size_t i = 0; i < items.size(); ++i)
        if (std::ranges::find(seen, items[i].id) == seen.end())
            return i;
    return 0;
}

int pickShowcase(const std::vector<Item>& showcase, const json& views)
{
    int best = -1;
    int64_t fewest = 0;
    for (size_t i = 0; i < showcase.size(); ++i)
    {
        const json& v = views.is_object() ? views.value(showcase[i].id, json(0)) : json(0);
        const int64_t n = v.is_number_integer() ? v.get<int64_t>() : 0;
        // Ties keep the earlier, newer entry.
        if (best < 0 || n < fewest)
        {
            best = int(i);
            fewest = n;
        }
    }
    return best;
}

namespace
{
std::string_view trimmed(std::string_view s)
{
    while (!s.empty() && (s.front() == ' ' || s.front() == '\t'))
        s.remove_prefix(1);
    while (!s.empty() && (s.back() == ' ' || s.back() == '\t' || s.back() == '\r'))
        s.remove_suffix(1);
    return s;
}

/** Markdown inline marks dropped: [text](url) keeps its text, and *, _ and ` go. */
std::string plainInline(std::string_view s)
{
    std::string out;
    for (size_t i = 0; i < s.size(); ++i)
    {
        const char c = s[i];
        if (c == '[')
        {
            const size_t close = s.find("](", i);
            const size_t end = close == std::string_view::npos ? close : s.find(')', close);
            if (end != std::string_view::npos)
            {
                out += plainInline(s.substr(i + 1, close - i - 1));
                i = end;
                continue;
            }
        }
        if (c == '*' || c == '`' || (c == '_' && (i == 0 || i + 1 == s.size() || s[i - 1] == ' ' || s[i + 1] == ' ')))
            continue;
        out += c;
    }
    return out;
}
} // namespace

Release latestRelease(std::string_view markdown)
{
    Release r;
    bool inside = false;
    while (!markdown.empty())
    {
        const size_t nl = markdown.find('\n');
        const std::string_view line = trimmed(markdown.substr(0, nl));
        markdown.remove_prefix(nl == std::string_view::npos ? markdown.size() : nl + 1);
        if (line.starts_with("## "))
        {
            if (inside)
                break;
            inside = true;
            const std::string_view heading = trimmed(line.substr(3));
            const size_t dash = heading.find(" - ");
            r.version = std::string(trimmed(heading.substr(0, dash)));
            if (dash != std::string_view::npos)
                r.date = std::string(trimmed(heading.substr(dash + 3)));
        }
        else if (inside && (line.starts_with("- ") || line.starts_with("* ")))
            r.changes.push_back(plainInline(trimmed(line.substr(2))));
    }
    return r;
}

std::vector<std::string> seenIds()
{
    std::vector<std::string> out;
    const json seen = PrefStore::get().value("general", kSeenPref, json::array());
    for (const json& id : seen.is_array() ? seen : json::array())
        if (id.is_string())
            out.push_back(id.get<std::string>());
    return out;
}

void markSeen(const std::string& id)
{
    std::vector<std::string> seen = seenIds();
    if (std::ranges::find(seen, id) != seen.end())
        return;
    seen.push_back(id);
    // Older ids have long left the feed.
    if (seen.size() > 200)
        seen.erase(seen.begin(), seen.end() - 200);
    PrefStore::get().set("general", kSeenPref, json(seen));
}

void countView(const std::string& id)
{
    json views = PrefStore::get().value("general", kShowcaseViewsPref, json::object());
    if (!views.is_object())
        views = json::object();
    const json n = views.value(id, json(0));
    views[id] = (n.is_number_integer() ? n.get<int64_t>() : 0) + 1;
    PrefStore::get().set("general", kShowcaseViewsPref, views);
}

std::vector<Item> cached()
{
    return enabled() ? readCache() : std::vector<Item>{};
}

QString imageFile(const Item& item)
{
    if (item.image.empty())
        return {};
    QString path = cachePath(item);
    return QFileInfo::exists(path) ? path : QString();
}

void refresh(std::function<void(const std::vector<Item>&)> done)
{
    if (!enabled())
        return;
    QNetworkReply* reply = get(QString::fromStdString(feedUrl()));
    QObject::connect(reply, &QNetworkReply::finished, qApp, [reply, done = std::move(done)] {
        reply->deleteLater();
        if (reply->error() != QNetworkReply::NoError)
        {
            log::info("news: {}", reply->errorString().toStdString());
            return;
        }
        const QByteArray body = reply->readAll();
        if (json::parse(body.toStdString(), nullptr, false).is_discarded())
            return;
        if (QFile f(cacheDir().filePath("news.json")); f.open(QIODevice::WriteOnly))
            f.write(body);

        std::vector<Item> items = readCache();
        auto pending = std::make_shared<int>(1);
        auto finish = [pending, items, done] {
            if (--*pending == 0 && done)
                done(items);
        };
        for (const Item& item : items)
        {
            if (item.image.empty() || !imageFile(item).isEmpty())
                continue;
            ++*pending;
            QNetworkReply* image = get(QString::fromStdString(item.image));
            QObject::connect(image, &QNetworkReply::finished, qApp, [image, path = cachePath(item), finish] {
                image->deleteLater();
                if (image->error() == QNetworkReply::NoError)
                    if (QFile f(path); f.open(QIODevice::WriteOnly))
                        f.write(image->readAll());
                finish();
            });
        }
        finish();
    });
}
} // namespace lila::ui::news
