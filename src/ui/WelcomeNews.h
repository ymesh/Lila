#pragma once

#include "core/Export.h"

#include <nlohmann/json.hpp>

#include <QString>

#include <functional>
#include <string>
#include <string_view>
#include <vector>

/** The news feed on the welcome dialog: announcements, releases and community spotlights from the Lila site. */
namespace lila::ui::news
{
inline constexpr const char* kEnabledPref = "/startup/news";
inline constexpr const char* kFeedUrl = "https://lila-3d.github.io/news.json";
/** Ids of the news items the user has been shown. */
inline constexpr const char* kSeenPref = "/startup/newsSeen";
/** How many times each showcase entry has been shown, by id. */
inline constexpr const char* kShowcaseViewsPref = "/startup/showcaseViews";

struct Item
{
    std::string id;
    /** news, release, spotlight or tutorial. A spotlight is a showcase entry: the art of someone using Lila. */
    std::string type;
    std::string date;
    std::string title;
    std::string body;
    /** Who made the work, for a spotlight or tutorial. */
    std::string author;
    /** The author's page, for a spotlight. */
    std::string authorUrl;
    /** ISO date a spotlight runs until, inclusive; empty runs until it leaves the feed. */
    std::string until;
    /** Absolute: a relative path in the feed resolves against the feed's URL. */
    std::string image;
    std::string url;
};

/**
 * What to show from a feed: newest first, at most limit news items and limit showcase entries, without items whose
 * minVersion or maxVersion excludes version. An item without an id gets its date and title as one.
 */
LILA_UI_API std::vector<Item> parse(const nlohmann::json& feed, std::string_view feedUrl, std::string_view version, size_t limit = 8);

/** A showcase entry (type spotlight): shown in its own card and rotated, not in date order with the news. */
LILA_UI_API bool isShowcase(const Item& item);
/** Whether an item still runs on today, an ISO date. */
LILA_UI_API bool running(const Item& item, std::string_view today);
/** The newest item whose id is not in seen, so a busy week of news can't hide what came before; 0 when all are seen. */
LILA_UI_API size_t firstUnseen(const std::vector<Item>& items, const std::vector<std::string>& seen);
/** The showcase entry shown fewest times (views maps id to count), newest on a tie; -1 when there are none. */
LILA_UI_API int pickShowcase(const std::vector<Item>& showcase, const nlohmann::json& views);

/** One release from the changelog page: its "## <version> - <date>" heading and its bullets as plain text. */
struct Release
{
    std::string version;
    std::string date;
    std::vector<std::string> changes;
};

/** The first release section of changelog markdown, with links, emphasis and code marks stripped; empty when there is none. */
LILA_UI_API Release latestRelease(std::string_view markdown);

/** The ids in the seen pref. */
LILA_UI_API std::vector<std::string> seenIds();
LILA_UI_API void markSeen(const std::string& id);
/** One more showing of a showcase entry. */
LILA_UI_API void countView(const std::string& id);

/** The feed as last fetched, or nothing when it never has been or the pref is off. */
LILA_UI_API std::vector<Item> cached();

/** The downloaded image of an item, or empty until it is. */
LILA_UI_API QString imageFile(const Item& item);

/** Fetches the feed and any images not yet downloaded, then calls done with the items on the main thread. Off with the pref. */
LILA_UI_API void refresh(std::function<void(const std::vector<Item>&)> done);
} // namespace lila::ui::news
