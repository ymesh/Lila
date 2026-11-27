#pragma once

#include "core/Export.h"

#include <cstdint>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

namespace lila::scene
{
class Graph;
}

/**
 * Fictional names for everything a real-world import brings in, and the check that no real name reaches the output.
 *
 * Naming cultures (Settlement's street and district cultures plus mexican) name streets, districts, towns, water,
 * landmarks and businesses. A name is a pure function of culture, kind, program and a stable key, so the same import
 * names the same things the same way every time. Real names an import replaced are kept only in the hidden source
 * attribute (kSourceAttr) on the nodes it made; nothing writes them anywhere else. findRealNames compares every output
 * text in a scene against those and the shipped brand blocklist (OpenStreetMap name-suggestion-index, BSD-3-Clause).
 */
namespace lila::names
{
/** Hidden user string attribute holding the real names an import replaced, as JSON {"<key>": "<real name>"}. Never exported. */
inline constexpr const char* kSourceAttr = "sourceNames";

enum class Kind : uint8_t
{
    Street,
    District,
    Town,
    River,
    Stream,
    Lake,
    Reservoir,
    Dam,
    Island,
    Beach,
    Bay,
    Mountain,
    Port,
    Airport,
    Cemetery,
    School,
    Hospital,
    Government,
    Police,
    Religious,
    Park,
    Stadium,
    Market,
    Business,
    Building,
    Landmark,
};

/** street, district, town, river, stream, lake, reservoir, dam, island, beach, bay, mountain, port, airport, cemetery,
 * school, hospital, government, police, religious, park, stadium, market, business, building, landmark. */
LILA_SCENE_API const std::vector<std::string>& kindNames();
LILA_SCENE_API bool parseKind(std::string_view name, Kind* out);

/** oldEnglish, latin, french, spanish, usGrid, sector and mexican. */
LILA_SCENE_API std::vector<std::string> cultures();
LILA_SCENE_API bool hasCulture(std::string_view culture);
/** Throws std::runtime_error naming the known cultures when culture is not one. */
LILA_SCENE_API void requireCulture(std::string_view culture);
/** A culture's name for menus: Old English, US Grid, Mexican. */
LILA_SCENE_API std::string cultureLabel(std::string_view culture);
/** The business and place vocabulary a culture names with: mexican, english, french, latin or scifi. */
LILA_SCENE_API std::string vocabularyOf(std::string_view culture);
/** The culture for a place on Earth (degrees), and why: mexican for Mexico and Latin America, usGrid, oldEnglish, french, spanish. */
LILA_SCENE_API std::string cultureForLocation(double lat, double lon, std::string* why = nullptr);

/** Business programs placeName knows patterns for (pharmacy, taqueria, carParts, ...). */
LILA_SCENE_API const std::vector<std::string>& programs();

/** A street's name: cls 0 arterial, 1 collector, 2 local, 3 alley, 4 path; attempt picks another when the first is taken. */
LILA_SCENE_API std::string streetName(std::string_view culture, int cls, uint64_t key, int attempt);
LILA_SCENE_API std::string districtName(std::string_view culture, uint64_t key, int attempt);
/** A fictional name for a thing of kind; for businesses and buildings, program says what they are (programs()). */
LILA_SCENE_API std::string placeName(std::string_view culture, Kind kind, std::string_view program, uint64_t key, int attempt);

/** Stable 64-bit mix (splitmix64). */
LILA_SCENE_API uint64_t hash64(uint64_t x);
/** A stable key from text (FNV-1a then hash64). */
LILA_SCENE_API uint64_t textKey(std::string_view text);

/** Text as names are compared: lower case, Latin accents folded, every run of other characters one space, trimmed. */
LILA_SCENE_API std::string normalize(std::string_view text);

/**
 * Real names as one-way hashes of their normalised text, for generators that run away from the scene (a node's compute):
 * they avoid real names without holding them. Matches as RealNames::find does. Hashes are 52 bits, exact as JSON numbers.
 */
struct LILA_SCENE_API NameHashes
{
    std::unordered_set<uint64_t> exact, contained;
    size_t longest = 1;

    bool empty() const { return exact.empty(); }
    bool matches(std::string_view text) const;
    /** {"exact": [...], "contained": [...], "longest": n}. */
    std::string toJson() const;
    /** Throws std::runtime_error for anything but what toJson writes (null or empty gives none). */
    static NameHashes fromJson(std::string_view json);
};

/** Real names output must not contain, matched on normalised whole words. */
class LILA_SCENE_API RealNames
{
public:
    struct Entry
    {
        std::string name;   // as given
        std::string source; // where it came from: a node path and key, or "blocklist"
    };

    void add(std::string_view name, std::string source);
    void merge(const RealNames& other);
    size_t size() const { return mEntries.size(); }
    bool empty() const { return mEntries.empty(); }
    /**
     * The real name text matches: equal to it, or containing it as whole words when the name is distinctive (two or more
     * words, or one word of four letters or more, and not only generic words such as Calle, Farmacia, Street). Null for none.
     */
    const Entry* find(std::string_view text) const;
    NameHashes hashes() const;

private:
    std::vector<Entry> mEntries;
    std::unordered_map<std::string, size_t> mExact;    // normalised -> entry
    std::unordered_map<std::string, size_t> mContained; // distinctive normalised -> entry
    size_t mLongest = 1;                                // words in the longest contained name
};

/** The shipped blocklist (resources/names/brands.txt), loaded once; empty with a warning in the log when the file is missing. */
LILA_SCENE_API const RealNames& brandBlocklist();
/** Where the blocklist was read from, and whether it was found. */
LILA_SCENE_API std::string brandBlocklistPath(bool* found = nullptr);

/**
 * Hands out fictional names for one import: the same request always gives the same name, no name is given to two
 * different keys of the same kind, and no name matches a real name (avoid) or the blocklist.
 */
class LILA_SCENE_API NameBook
{
public:
    explicit NameBook(std::string culture, std::shared_ptr<const RealNames> avoid = nullptr);
    const std::string& culture() const { return mCulture; }
    std::string place(Kind kind, std::string_view program, uint64_t key);
    std::string street(int cls, uint64_t key);
    std::string district(uint64_t key);
    /** True when text matches a real name (avoid or blocklist). */
    bool real(std::string_view text) const;
    /** Names spliced from the culture's words because every listed word matched a real name. */
    size_t coined() const { return mCoined; }

private:
    template <class F>
    std::string unique(uint64_t bucket, uint64_t key, F&& make);

    std::string mCulture;
    std::shared_ptr<const RealNames> mAvoid;
    std::unordered_map<uint64_t, std::string> mGiven;               // (bucket, key) -> name
    std::unordered_map<uint64_t, std::unordered_set<std::string>> mTaken; // bucket -> names
    std::unordered_map<uint64_t, int> mNumberFrom;                   // bucket -> lowest number still worth trying
    size_t mCoined = 0;
};

/** a with each distinctive word spliced onto the second half of one of b's: a name of the same sound that lists name neither. */
LILA_SCENE_API std::string coinName(std::string_view a, std::string_view b);

/** Real names recorded in source attributes on every node of the scene; source is "<node>.<key>". */
LILA_SCENE_API RealNames sceneRealNames(const scene::Graph& g);

struct Leak
{
    std::string where; // node path and attribute, or the label the caller gave
    std::string text;  // the output text
    std::string real;  // the real name it matches
    std::string source; // where that real name came from: an import's source attribute, or "blocklist"
};

/** A piece of output text to check, and where it is. */
struct Text
{
    std::string where;
    std::string text;
};

/**
 * Output text that matches a real name: node names, visible string attributes (including names inside JSON such as
 * osmFeatures), and the extra texts given, against the scene's source attributes and the blocklist.
 */
LILA_SCENE_API std::vector<Leak> findRealNames(const scene::Graph& g, std::span<const Text> extra = {}, bool sceneTexts = true);
/** The same over given texts only, against given real names (and the blocklist when blocklist is true). */
LILA_SCENE_API std::vector<Leak> findRealNames(std::span<const Text> texts, const RealNames& real, bool blocklist = true);

/** namingCultures, fictionalName, realNameCheck. */
LILA_SCENE_API void registerCommands();
} // namespace lila::names
