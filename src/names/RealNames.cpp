// Real names: normalising text for comparison, the set of real names output must not contain, the shipped brand
// blocklist, NameBook (fictional names that never repeat and never match a real one) and the scene check.

#include "names/Names.h"

#include "core/Log.h"
#include "core/Paths.h"
#include "scene/Graph.h"
#include "scene/Node.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <fstream>
#include <mutex>
#include <set>
#include <stdexcept>

namespace lila::names
{
namespace
{
/** Lower-case ASCII for U+00C0..U+017F; a space where the letter has none. */
constexpr std::string_view kFold = "aaaaaaaceeeeiiiidnooooo ouuuuytsaaaaaaaceeeeiiiidnooooo ouuuuytyaaaaaaccccccccddddeeeeeeeeeegggggggghhhhiiiiiiiiiiiijjkkk"
                                   "llllllllllnnnnnnnnnoooooooorrrrrrssssssssttttttuuuuuuuuuuuuwwyyyzzzzzzs";

/** Words names of every culture use for what a thing is, not which one it is: a name of only these is not distinctive. */
const std::set<std::string, std::less<>>& genericWords()
{
    static const std::set<std::string, std::less<>> w = {
        "a", "al", "and", "av", "ave", "avenida", "avenue", "andador", "auto", "autos", "bar", "barrio", "banco", "bank", "beach",
        "blvd", "boulevard", "bulevar", "building", "cafe", "calle", "callejon", "calzada", "camino", "capilla", "carretera",
        "catedral", "cementerio", "cemetery", "centro", "center", "centre", "church", "clinica", "clinic", "club", "col", "colegio",
        "colonia", "comercial", "court", "de", "del", "deportiva", "drive", "e", "edificio", "el", "elementary", "escuela",
        "estacion", "estadio", "este", "farmacia", "farmacias", "general", "gobierno", "gym", "high", "hospital", "hotel",
        "instituto", "internacional", "international", "isla", "island", "jardin", "la", "lago", "lake", "lane", "las", "laguna",
        "los", "mar", "market", "mercado", "middle", "motel", "municipal", "museo", "museum", "n", "ninos", "no", "norte",
        "north", "nueva", "nuevo", "numero", "of", "oeste", "office", "oficina", "oriente", "palacio", "panteon", "park",
        "parque", "parroquia", "paseo", "pharmacy", "place", "playa", "plaza", "policia", "police", "poniente", "port",
        "preparatoria", "presa", "primaria", "privada", "puerto", "rd", "restaurant", "restaurante", "rio", "river", "road",
        "san", "santa", "santo", "school", "secundaria", "sector", "sendero", "shop", "south", "st", "station", "store",
        "street", "sur", "super", "supermercado", "taller", "templo", "the", "tienda", "torre", "unidad", "universidad",
        "university", "way", "west", "y", "zona", "abarrotes", "taqueria", "tacos", "mariscos", "panaderia", "gasolinera",
        "aeropuerto", "airport", "fraccionamiento", "residencial", "mini", "express", "dam", "reservoir", "bay", "bahia",
        "cerro", "mount", "creek", "arroyo", "tower", "grill", "market", "station", "stop", "parada", "bomberos", "fire",
        "comandancia", "delegacion"};
    return w;
}

std::vector<std::string_view> words(std::string_view s)
{
    std::vector<std::string_view> out;
    size_t i = 0;
    while (i < s.size())
    {
        const size_t j = s.find(' ', i);
        const size_t e = j == std::string_view::npos ? s.size() : j;
        if (e > i)
            out.push_back(s.substr(i, e - i));
        i = e + 1;
    }
    return out;
}

bool isNumber(std::string_view w) { return !w.empty() && std::ranges::all_of(w, [](char c) { return c >= '0' && c <= '9'; }); }

/** Two or more words that say which one it is, or one of four letters or more. */
bool distinctive(std::string_view normalized)
{
    int specific = 0;
    size_t longest = 0;
    for (std::string_view w : words(normalized))
        if (!genericWords().count(w) && !isNumber(w))
        {
            ++specific;
            longest = std::max(longest, w.size());
        }
    return specific >= 2 || (specific == 1 && longest >= 4);
}
} // namespace

const std::vector<std::string>& kindNames()
{
    static const std::vector<std::string> n = {"street", "district", "town", "river", "stream", "lake", "reservoir", "dam", "island",
                                               "beach", "bay", "mountain", "port", "airport", "cemetery", "school", "hospital",
                                               "government", "police", "religious", "park", "stadium", "market", "business",
                                               "building", "landmark"};
    return n;
}

bool parseKind(std::string_view name, Kind* out)
{
    const auto& n = kindNames();
    for (size_t k = 0; k < n.size(); ++k)
        if (n[k] == name)
        {
            *out = Kind(k);
            return true;
        }
    return false;
}

uint64_t hash64(uint64_t x)
{
    x += 0x9E3779B97F4A7C15ull;
    x = (x ^ (x >> 30)) * 0xBF58476D1CE4E5B9ull;
    x = (x ^ (x >> 27)) * 0x94D049BB133111EBull;
    return x ^ (x >> 31);
}

uint64_t textKey(std::string_view text)
{
    uint64_t h = 0xcbf29ce484222325ull;
    for (unsigned char c : text)
        h = (h ^ c) * 0x100000001b3ull;
    return hash64(h);
}

std::string normalize(std::string_view s)
{
    std::string out;
    out.reserve(s.size());
    bool space = true;
    auto put = [&](char c) {
        if (c == ' ')
        {
            if (!space)
                out += ' ';
            space = true;
            return;
        }
        out += c;
        space = false;
    };
    for (size_t i = 0; i < s.size();)
    {
        const unsigned char c = (unsigned char)s[i];
        if (c < 0x80)
        {
            // O'Brien and OBrien are one name.
            if (c != '\'')
                put(std::isalnum(c) ? char(std::tolower(c)) : ' ');
            ++i;
            continue;
        }
        uint32_t cp = 0;
        int len = c >= 0xF0 ? 4 : c >= 0xE0 ? 3 : c >= 0xC0 ? 2 : 1;
        cp = len == 2 ? c & 0x1F : len == 3 ? c & 0x0F : c & 0x07;
        for (int k = 1; k < len && i + size_t(k) < s.size(); ++k)
            cp = (cp << 6) | ((unsigned char)s[i + size_t(k)] & 0x3F);
        i += size_t(len);
        if (cp >= 0xC0 && cp < 0x180)
            put(kFold[cp - 0xC0]);
        else if (cp == 0x2019) // right single quote, as in O’Brien
            ;
        else
            put(' ');
    }
    while (!out.empty() && out.back() == ' ')
        out.pop_back();
    return out;
}

void RealNames::add(std::string_view name, std::string source)
{
    std::string n = normalize(name);
    if (n.empty())
        return;
    if (mExact.count(n))
        return;
    mEntries.push_back({std::string(name), std::move(source)});
    const size_t index = mEntries.size() - 1;
    if (distinctive(n))
    {
        mLongest = std::max(mLongest, words(n).size());
        mContained.emplace(n, index);
    }
    mExact.emplace(std::move(n), index);
}

void RealNames::merge(const RealNames& other)
{
    for (const Entry& e : other.mEntries)
        add(e.name, e.source);
}

const RealNames::Entry* RealNames::find(std::string_view text) const
{
    if (mEntries.empty())
        return nullptr;
    const std::string n = normalize(text);
    if (n.empty())
        return nullptr;
    if (auto it = mExact.find(n); it != mExact.end())
        return &mEntries[it->second];
    const std::vector<std::string_view> w = words(n);
    std::string run;
    for (size_t a = 0; a < w.size(); ++a)
    {
        run.clear();
        for (size_t b = a; b < w.size() && b - a < mLongest; ++b)
        {
            if (b > a)
                run += ' ';
            run += w[b];
            if (auto it = mContained.find(run); it != mContained.end())
                return &mEntries[it->second];
        }
    }
    return nullptr;
}

namespace
{
uint64_t hash52(std::string_view normalized) { return textKey(normalized) & ((1ull << 52) - 1); }
} // namespace

NameHashes RealNames::hashes() const
{
    NameHashes h;
    for (const auto& [n, i] : mExact)
        h.exact.insert(hash52(n));
    for (const auto& [n, i] : mContained)
        h.contained.insert(hash52(n));
    h.longest = mLongest;
    return h;
}

bool NameHashes::matches(std::string_view text) const
{
    if (exact.empty())
        return false;
    const std::string n = normalize(text);
    if (n.empty())
        return false;
    if (exact.count(hash52(n)))
        return true;
    const std::vector<std::string_view> w = words(n);
    std::string run;
    for (size_t a = 0; a < w.size(); ++a)
    {
        run.clear();
        for (size_t b = a; b < w.size() && b - a < longest; ++b)
        {
            if (b > a)
                run += ' ';
            run += w[b];
            if (contained.count(hash52(run)))
                return true;
        }
    }
    return false;
}

std::string NameHashes::toJson() const
{
    std::vector<uint64_t> e(exact.begin(), exact.end()), c(contained.begin(), contained.end());
    std::sort(e.begin(), e.end());
    std::sort(c.begin(), c.end());
    return nlohmann::json{{"exact", e}, {"contained", c}, {"longest", longest}}.dump();
}

NameHashes NameHashes::fromJson(std::string_view text)
{
    NameHashes h;
    if (text.empty())
        return h;
    const nlohmann::json j = nlohmann::json::parse(text, nullptr, false);
    if (j.is_null())
        return h;
    if (!j.is_object() || !j.contains("exact") || !j["exact"].is_array())
        throw std::runtime_error("name hashes take {exact, contained, longest}");
    for (const nlohmann::json& v : j["exact"])
        if (v.is_number_unsigned() || v.is_number_integer())
            h.exact.insert(v.get<uint64_t>());
    for (const nlohmann::json& v : j.value("contained", nlohmann::json::array()))
        if (v.is_number_unsigned() || v.is_number_integer())
            h.contained.insert(v.get<uint64_t>());
    h.longest = std::max<size_t>(1, j.value("longest", size_t(1)));
    return h;
}

std::string brandBlocklistPath(bool* found)
{
    const std::filesystem::path p = paths::resourcesDir() / "names" / "brands.txt";
    if (found)
        *found = std::filesystem::exists(p);
    return paths::toUtf8(p);
}

const RealNames& brandBlocklist()
{
    static const RealNames list = [] {
        RealNames r;
        bool found = false;
        const std::string path = brandBlocklistPath(&found);
        if (!found)
        {
            log::warn("names: no brand blocklist at {}; the real-name check compares imported names only", path);
            return r;
        }
        std::ifstream f(paths::fromUtf8(path), std::ios::binary);
        std::string line;
        while (std::getline(f, line))
        {
            if (!line.empty() && line.back() == '\r')
                line.pop_back();
            // A brand named only by words that say what it is ("Farmacia", "Super") is no name to keep out.
            if (line.empty() || line[0] == '#')
                continue;
            const std::string n = normalize(line);
            if (std::ranges::all_of(words(n), [](std::string_view w) { return genericWords().count(w) > 0; }))
                continue;
            r.add(line, "blocklist");
        }
        return r;
    }();
    return list;
}

NameBook::NameBook(std::string culture, std::shared_ptr<const RealNames> avoid) : mCulture(std::move(culture)), mAvoid(std::move(avoid))
{
    requireCulture(mCulture);
}

bool NameBook::real(std::string_view text) const { return (mAvoid && mAvoid->find(text)) || brandBlocklist().find(text); }

namespace
{
/** A byte index near the middle of word that starts a UTF-8 character. */
size_t middleOf(std::string_view word)
{
    size_t i = word.size() / 2;
    while (i > 0 && (static_cast<unsigned char>(word[i]) & 0xC0) == 0x80)
        --i;
    return i;
}

bool distinctiveWord(std::string_view word)
{
    const std::string n = normalize(word);
    return n.size() >= 4 && n.find(' ') == std::string::npos && !genericWords().count(n) && !isNumber(n);
}
} // namespace

std::string coinName(std::string_view a, std::string_view b)
{
    std::vector<std::string_view> donors;
    for (std::string_view w : words(b))
        if (distinctiveWord(w))
            donors.push_back(w);
    if (donors.empty())
        return std::string(a);
    std::string out;
    size_t used = 0;
    for (std::string_view w : words(a))
    {
        if (!out.empty())
            out += ' ';
        const std::string own = normalize(w);
        std::string_view d;
        for (size_t k = 0; k < donors.size() && d.empty() && distinctiveWord(w); ++k, ++used)
            if (normalize(donors[used % donors.size()]) != own)
                d = donors[used % donors.size()];
        if (d.empty())
        {
            out += w;
            continue;
        }
        out += w.substr(0, std::max<size_t>(1, middleOf(w)));
        out += d.substr(middleOf(d));
    }
    return out;
}

template <class F>
std::string NameBook::unique(uint64_t bucket, uint64_t key, F&& make)
{
    const uint64_t id = hash64(bucket * 0x9E3779B97F4A7C15ull ^ key);
    if (auto it = mGiven.find(id); it != mGiven.end())
        return it->second;
    std::unordered_set<std::string>& taken = mTaken[bucket];
    constexpr int kAttempts = 96;
    std::string name;
    auto take = [&](std::string s) {
        if (!taken.count(s) && !real(s))
            name = std::move(s);
    };
    for (int attempt = 0; attempt < kAttempts && name.empty(); ++attempt)
        take(make(attempt));
    // Real data can name things with every word of the culture's lists, or a pattern can have one fixed word a real place
    // shares; then the words are spliced with others of the culture into ones of its sound.
    auto coin = [&](int attempt) {
        const uint64_t k = hash64(key + uint64_t(attempt));
        return coinName(make(attempt), make((attempt * 7 + 13) % kAttempts) + " " + streetName(mCulture, 2, k, 0) + " " + districtName(mCulture, k, 0));
    };
    bool coined = false;
    for (int attempt = 0; attempt < kAttempts && name.empty(); ++attempt)
    {
        take(coin(attempt));
        coined = !name.empty();
    }
    // Numbered after any of the attempts whose own name is not real, so a real first attempt can't loop forever. The search
    // resumes at the bucket's last number, so it stays linear however many names one bucket hands out.
    int& from = mNumberFrom[bucket];
    for (int n = std::max(2, from); name.empty(); ++n)
    {
        const std::string number = " " + std::to_string(n);
        for (int attempt = 0; attempt < kAttempts && name.empty(); ++attempt)
            take(make(attempt) + number);
        for (int attempt = 0; attempt < kAttempts && name.empty(); ++attempt)
        {
            take(coin(attempt) + number);
            coined = !name.empty();
        }
        if (!name.empty())
            from = n;
        else if (n > from + 64)
            throw std::runtime_error("no fictional name left that matches no real name");
    }
    if (coined)
        ++mCoined;
    taken.insert(name);
    mGiven.emplace(id, name);
    return name;
}

std::string NameBook::place(Kind kind, std::string_view program, uint64_t key)
{
    if (kind == Kind::District)
        return district(key);
    if (kind == Kind::Street)
        return street(2, key);
    const std::string p(program);
    // Taken names are shared by every program of a kind; what was given is remembered per program.
    return unique(uint64_t(kind) + 100, key ^ textKey(p), [&](int attempt) { return placeName(mCulture, kind, p, key, attempt); });
}

std::string NameBook::street(int cls, uint64_t key)
{
    return unique(uint64_t(Kind::Street) + 100, key, [&](int attempt) { return streetName(mCulture, cls, key, attempt); });
}

std::string NameBook::district(uint64_t key)
{
    return unique(uint64_t(Kind::District) + 100, key, [&](int attempt) { return districtName(mCulture, key, attempt); });
}

namespace
{
const std::string* stringAttr(const scene::Node& n, std::string_view attr)
{
    const int i = n.type().attrIndex(attr);
    if (i < 0)
        return nullptr;
    return std::get_if<std::string>(&n.value(i));
}

void collectStrings(const nlohmann::json& j, const std::string& where, std::vector<Text>& out, int depth = 0)
{
    if (depth > 8)
        return;
    if (j.is_object())
    {
        for (const auto& [k, v] : j.items())
            if (v.is_string() && (k == "name" || k == "text" || k == "label" || k == "displayName" || k == "brand" || k == "operator"))
                out.push_back({where, v.get<std::string>()});
            else if (v.is_structured())
                collectStrings(v, where, out, depth + 1);
    }
    else if (j.is_array())
        for (const nlohmann::json& v : j)
            collectStrings(v, where, out, depth + 1);
}

/** Attributes whose JSON carries names an import or generator wrote. */
bool nameBearing(std::string_view attr)
{
    return attr == "osmFeatures" || attr == "osmBuildings" || attr == "osmPlaces" || attr == "signText" || attr == "labels" ||
           attr == "displayName" || attr == "fictionalNames" || attr == "generators" || attr == "edits";
}
} // namespace

RealNames sceneRealNames(const scene::Graph& g)
{
    RealNames r;
    for (scene::NodeId id : g.nodes())
    {
        const scene::Node* n = g.node(id);
        if (!n)
            continue;
        const std::string* s = stringAttr(*n, kSourceAttr);
        if (!s || s->empty())
            continue;
        const nlohmann::json j = nlohmann::json::parse(*s, nullptr, false);
        if (!j.is_object())
            continue;
        const std::string where = n->path();
        for (const auto& [k, v] : j.items())
        {
            if (v.is_string())
                r.add(v.get<std::string>(), where + "." + k);
            else if (v.is_array())
                for (const nlohmann::json& x : v)
                    if (x.is_string())
                        r.add(x.get<std::string>(), where + "." + k);
        }
    }
    return r;
}

std::vector<Leak> findRealNames(std::span<const Text> texts, const RealNames& real, bool blocklist)
{
    std::vector<Leak> out;
    for (const Text& t : texts)
    {
        if (t.text.empty())
            continue;
        const RealNames::Entry* e = real.find(t.text);
        if (!e && blocklist)
            e = brandBlocklist().find(t.text);
        if (e)
            out.push_back({t.where, t.text, e->name, e->source});
    }
    return out;
}

std::vector<Leak> findRealNames(const scene::Graph& g, std::span<const Text> extra, bool sceneTexts)
{
    const RealNames real = sceneRealNames(g);
    std::vector<Leak> out = findRealNames(extra, real, true);
    if (!sceneTexts)
        return out;
    std::vector<Text> named, nodeNames;
    for (scene::NodeId id : g.nodes())
    {
        const scene::Node* n = g.node(id);
        if (!n)
            continue;
        nodeNames.push_back({n->path(), n->name()});
        const auto& attrs = n->type().attrs;
        for (size_t i = 0; i < attrs.size(); ++i)
        {
            if (attrs[i].type != scene::PlugType::String || !nameBearing(attrs[i].name))
                continue;
            const std::string* s = std::get_if<std::string>(&n->value(int(i)));
            if (!s || s->empty())
                continue;
            const nlohmann::json j = nlohmann::json::parse(*s, nullptr, false);
            if (j.is_discarded())
                named.push_back({n->path() + "." + attrs[i].name, *s});
            else
                collectStrings(j, n->path() + "." + attrs[i].name, named);
        }
    }
    for (Leak& l : findRealNames(named, real, true))
        out.push_back(std::move(l));
    // Any node's own name is held to the imported names only: the blocklist's common words would flag ordinary names.
    for (Leak& l : findRealNames(nodeNames, real, false))
        out.push_back(std::move(l));
    return out;
}
} // namespace lila::names
