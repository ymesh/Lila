#include "slots/Slots.h"

#include "core/ExtensionContext.h"
#include "core/Log.h"
#include "core/Profile.h"
#include "core/Release.h"
#include "ops/Undo.h"
#include "scene/Graph.h"
#include "scene/Node.h"
#include "scene/SceneData.h"

#include <algorithm>
#include <atomic>
#include <cstdio>
#include <map>
#include <mutex>
#include <optional>
#include <set>

namespace lila::slots
{
namespace
{
constexpr const char* kRulesKey = "slots.rules";
constexpr const char* kOverridesKey = "slots.overrides";
constexpr const char* kPinsKey = "slots.pins";

json vecJson(const glm::dvec3& v) { return json::array({v.x, v.y, v.z}); }

glm::dvec3 vecOf(const json& j, glm::dvec3 fallback)
{
    if (!j.is_array() || j.size() != 3)
        return fallback;
    return {j[0].get<double>(), j[1].get<double>(), j[2].get<double>()};
}

json matJson(const glm::dmat4& m)
{
    json a = json::array();
    for (int c = 0; c < 4; ++c)
        for (int r = 0; r < 4; ++r)
            a.push_back(m[c][r]);
    return a;
}

glm::dmat4 matOf(const json& j)
{
    glm::dmat4 m(1.0);
    if (!j.is_array() || j.size() != 16)
        return m;
    for (int c = 0; c < 4; ++c)
        for (int r = 0; r < 4; ++r)
            m[c][r] = j[size_t(c * 4 + r)].get<double>();
    return m;
}

std::vector<std::string> strings(const json& j)
{
    std::vector<std::string> out;
    if (j.is_array())
        for (const json& v : j)
            if (v.is_string())
                out.push_back(v.get<std::string>());
    return out;
}

std::string str(const json& j, const char* key)
{
    auto it = j.find(key);
    return it != j.end() && it->is_string() ? it->get<std::string>() : std::string();
}

bool startsWithPath(std::string_view s, std::string_view prefix)
{
    if (prefix.empty())
        return true;
    if (!s.starts_with(prefix))
        return false;
    return s.size() == prefix.size() || s[prefix.size()] == '/' || prefix.back() == ':' || prefix.back() == '/';
}

std::atomic<uint64_t> gRevision{1};

std::mutex& stateMutex()
{
    static std::mutex m;
    return m;
}
StatePtr& currentState()
{
    static StatePtr s = std::make_shared<State>();
    return s;
}

/** Refreshes on undo (before the data swap runs backwards) or on redo (after it runs forwards). */
struct RefreshAction final : UndoAction
{
    bool onUndo;
    explicit RefreshAction(bool u) : onUndo(u) {}
    void undo() override
    {
        if (onUndo)
            refresh();
    }
    void redo() override
    {
        if (!onUndo)
            refresh();
    }
    size_t bytes() const override { return sizeof(*this); }
};

void commit(const char* key, const json& value)
{
    UndoStack& u = UndoStack::get();
    u.openChunk("Slots");
    u.record(std::make_unique<RefreshAction>(true));
    scene::data::set(key, value.empty() ? json() : value);
    u.record(std::make_unique<RefreshAction>(false));
    u.closeChunk();
    refresh();
}

json rulesJson(const std::vector<Rule>& rules)
{
    json a = json::array();
    for (const Rule& r : rules)
        a.push_back(r.toJson());
    return a;
}

json overridesJson(const std::map<std::string, Override>& m)
{
    json o = json::object();
    for (const auto& [id, ov] : m)
        o[id] = ov.toJson();
    return o;
}

json pinsJson(const std::map<std::string, Pin>& m)
{
    json o = json::object();
    for (const auto& [k, p] : m)
        o[k] = p.toJson();
    return o;
}

struct Published
{
    std::vector<Slot> slots;
};
std::mutex& tableMutex()
{
    static std::mutex m;
    return m;
}
std::map<NodeId, Published>& table()
{
    static std::map<NodeId, Published> t;
    return t;
}
} // namespace

const char* attachName(Attach a)
{
    switch (a)
    {
    case Attach::Ground: return "ground";
    case Attach::Wall: return "wall";
    case Attach::Ceiling: return "ceiling";
    case Attach::Roof: return "roof";
    case Attach::Free: return "free";
    }
    return "ground";
}

std::optional<Attach> parseAttach(std::string_view name)
{
    for (Attach a : {Attach::Ground, Attach::Wall, Attach::Ceiling, Attach::Roof, Attach::Free})
        if (name == attachName(a))
            return a;
    return std::nullopt;
}

const char* levelName(Level l)
{
    switch (l)
    {
    case Level::Global: return "global";
    case Level::Region: return "region";
    case Level::Owner: return "owner";
    }
    return "global";
}

std::optional<Level> parseLevel(std::string_view name)
{
    for (Level l : {Level::Global, Level::Region, Level::Owner})
        if (name == levelName(l))
            return l;
    return std::nullopt;
}

json SlotType::toJson() const
{
    return {{"id", id}, {"label", label}, {"tags", tags}, {"footprint", vecJson(footprint)}, {"attach", attachName(attach)},
            {"placeholder", placeholder}, {"doc", doc}};
}

Registry<SlotType>& types()
{
    static Registry<SlotType> r;
    return r;
}

void registerType(SlotType t)
{
    std::string id = t.id;
    types().put(std::move(id), std::move(t));
}

bool registerType(ext::Context& ctx, SlotType t)
{
    if (types().contains(t.id))
    {
        log::warn("Slots: type '{}' is already registered", t.id);
        return false;
    }
    std::string id = t.id;
    types().add(std::move(id), std::move(t), ctx.owner());
    return true;
}

const SlotType* resolveType(std::string_view id)
{
    std::string s(id);
    while (!s.empty())
    {
        if (const SlotType* t = types().find(s))
            return t;
        const size_t dot = s.rfind('.');
        if (dot == std::string::npos)
            break;
        s.resize(dot);
    }
    return nullptr;
}

bool typeMatches(std::string_view pattern, std::string_view type)
{
    if (pattern.empty() || pattern == "*")
        return true;
    size_t start = 0;
    while (start <= pattern.size())
    {
        size_t comma = pattern.find(',', start);
        if (comma == std::string_view::npos)
            comma = pattern.size();
        std::string_view p = pattern.substr(start, comma - start);
        while (!p.empty() && p.front() == ' ')
            p.remove_prefix(1);
        while (!p.empty() && p.back() == ' ')
            p.remove_suffix(1);
        if (p == "*")
            return true;
        if (p.ends_with(".*"))
        {
            std::string_view base = p.substr(0, p.size() - 2);
            if (type.size() > base.size() + 1 && type.starts_with(base) && type[base.size()] == '.')
                return true;
        }
        else if (!p.empty() && type.starts_with(p) && (type.size() == p.size() || type[p.size()] == '.'))
            return true;
        start = comma + 1;
    }
    return false;
}

// Payload

bool Payload::empty() const
{
    return item.empty() && pool.empty() && poolTag.empty() && mesh.empty() && placeholder.empty() && unrealMesh.empty() &&
           unrealClass.empty() && !hidden && (data.is_null() || data.empty());
}

json Payload::toJson() const
{
    json j = json::object();
    if (!item.empty())
        j["item"] = item;
    if (!pool.empty())
        j["pool"] = pool;
    if (!poolTag.empty())
        j["poolTag"] = poolTag;
    if (!mesh.empty())
        j["mesh"] = mesh;
    if (!placeholder.empty())
        j["placeholder"] = placeholder;
    if (!unrealMesh.empty())
        j["unrealMesh"] = unrealMesh;
    if (!unrealClass.empty())
        j["unrealClass"] = unrealClass;
    if (hidden)
        j["hidden"] = *hidden;
    if (data.is_object() && !data.empty())
        j["data"] = data;
    return j;
}

Payload Payload::fromJson(const json& j)
{
    Payload p;
    if (!j.is_object())
        return p;
    p.item = str(j, "item");
    p.pool = strings(j.value("pool", json()));
    p.poolTag = str(j, "poolTag");
    p.mesh = str(j, "mesh");
    p.placeholder = str(j, "placeholder");
    p.unrealMesh = str(j, "unrealMesh");
    p.unrealClass = str(j, "unrealClass");
    if (auto it = j.find("hidden"); it != j.end() && it->is_boolean())
        p.hidden = it->get<bool>();
    if (auto it = j.find("data"); it != j.end() && it->is_object())
        p.data = *it;
    return p;
}

void Payload::overlay(const Payload& over)
{
    // A visual source replaces every other one, so a rule naming an item wins over a pool set earlier.
    const bool visual = !over.item.empty() || !over.pool.empty() || !over.poolTag.empty() || !over.mesh.empty() || !over.placeholder.empty();
    if (visual)
    {
        item = over.item;
        pool = over.pool;
        poolTag = over.poolTag;
        mesh = over.mesh;
        placeholder = over.placeholder;
    }
    if (!over.unrealMesh.empty())
        unrealMesh = over.unrealMesh;
    if (!over.unrealClass.empty())
        unrealClass = over.unrealClass == "-" ? std::string() : over.unrealClass;
    if (over.hidden)
        hidden = over.hidden;
    if (over.data.is_object())
    {
        if (!data.is_object())
            data = json::object();
        for (auto it = over.data.begin(); it != over.data.end(); ++it)
            if (it->is_null())
                data.erase(it.key());
            else
                data[it.key()] = *it;
    }
}

std::string Payload::describe() const
{
    std::string s;
    auto add = [&](const std::string& part) { s += (s.empty() ? "" : ", ") + part; };
    if (hidden && *hidden)
        add("hidden");
    if (!item.empty())
        add("item " + item);
    if (!pool.empty())
        add("pool of " + std::to_string(pool.size()));
    if (!poolTag.empty())
        add("pool tag " + poolTag);
    if (!mesh.empty())
        add("mesh " + mesh);
    if (!placeholder.empty())
        add("placeholder " + placeholder);
    if (!unrealMesh.empty())
        add("UE mesh " + unrealMesh);
    if (!unrealClass.empty())
        add("class " + unrealClass);
    if (data.is_object() && !data.empty())
        add(std::to_string(data.size()) + " data");
    return s.empty() ? "default" : s;
}

// Slots

json Slot::toJson() const
{
    json j = {{"id", id}, {"type", type}, {"matrix", matJson(transform)}, {"owner", owner}};
    if (!regions.empty())
        j["regions"] = regions;
    if (!tags.empty())
        j["tags"] = tags;
    if (!payload.empty())
        j["payload"] = payload.toJson();
    return j;
}

Slot Slot::fromJson(const json& j)
{
    Slot s;
    s.id = str(j, "id");
    s.type = str(j, "type");
    s.transform = matOf(j.value("matrix", json()));
    s.owner = str(j, "owner");
    s.regions = strings(j.value("regions", json()));
    s.tags = strings(j.value("tags", json()));
    s.payload = Payload::fromJson(j.value("payload", json()));
    return s;
}

std::string makeId(std::string_view owner, std::string_view path)
{
    auto trim = [](std::string_view s) {
        while (!s.empty() && s.front() == '/')
            s.remove_prefix(1);
        while (!s.empty() && s.back() == '/')
            s.remove_suffix(1);
        return s;
    };
    owner = trim(owner);
    path = trim(path);
    if (owner.empty())
        return std::string(path);
    if (path.empty())
        return std::string(owner);
    return std::string(owner) + "/" + std::string(path);
}

std::string pathOf(std::initializer_list<std::string_view> parts)
{
    std::string out;
    for (std::string_view p : parts)
    {
        if (p.empty())
            continue;
        if (!out.empty())
            out += '/';
        out += p;
    }
    return out;
}

std::string hex(uint64_t key)
{
    char buf[17];
    std::snprintf(buf, sizeof buf, "%llx", static_cast<unsigned long long>(key));
    return buf;
}

uint64_t idHash(std::string_view id)
{
    uint64_t h = 14695981039346656037ull;
    for (char c : id)
    {
        h ^= uint8_t(c);
        h *= 1099511628211ull;
    }
    return h;
}

// Rules

bool Rule::covers(const Slot& s) const
{
    if (!enabled || !typeMatches(type, s.type))
        return false;
    for (const std::string& t : tags)
        if (std::find(s.tags.begin(), s.tags.end(), t) == s.tags.end())
            return false;
    switch (level)
    {
    case Level::Global: return true;
    case Level::Region:
        return std::any_of(s.regions.begin(), s.regions.end(), [&](const std::string& r) { return startsWithPath(r, region); });
    case Level::Owner: return startsWithPath(s.owner, owner) || startsWithPath(s.id, owner);
    }
    return false;
}

json Rule::toJson() const
{
    json j = {{"id", id}, {"level", levelName(level)}, {"type", type}, {"payload", payload.toJson()}, {"enabled", enabled}};
    if (!name.empty())
        j["name"] = name;
    if (!region.empty())
        j["region"] = region;
    if (!owner.empty())
        j["owner"] = owner;
    if (!tags.empty())
        j["tags"] = tags;
    return j;
}

Rule Rule::fromJson(const json& j)
{
    Rule r;
    r.id = str(j, "id");
    r.name = str(j, "name");
    r.level = parseLevel(str(j, "level")).value_or(Level::Global);
    r.type = j.contains("type") ? str(j, "type") : std::string("*");
    r.region = str(j, "region");
    r.owner = str(j, "owner");
    r.tags = strings(j.value("tags", json()));
    r.payload = Payload::fromJson(j.value("payload", json()));
    r.enabled = j.value("enabled", true);
    return r;
}

json Override::toJson() const
{
    json j = {{"payload", payload.toJson()}, {"type", type}, {"at", vecJson(at)}};
    if (!owner.empty())
        j["owner"] = owner;
    return j;
}

Override Override::fromJson(const json& j)
{
    Override o;
    o.payload = Payload::fromJson(j.value("payload", json()));
    o.type = str(j, "type");
    o.at = vecOf(j.value("at", json()), glm::dvec3(0.0));
    o.owner = str(j, "owner");
    return o;
}

json Pin::toJson() const
{
    json a = json::array();
    for (const PinnedSlot& p : slots)
        a.push_back(p.slot.toJson());
    json out = {{"slots", a}};
    if (!slots.empty() && slots.front().swappable)
        out["swappable"] = true;
    return out;
}

Pin Pin::fromJson(const json& j)
{
    Pin p;
    const bool swappable = j.value("swappable", false);
    if (auto it = j.find("slots"); it != j.end() && it->is_array())
        for (const json& s : *it)
            p.slots.push_back({Slot::fromJson(s), swappable});
    return p;
}

// State

StatePtr state()
{
    std::lock_guard lock(stateMutex());
    return currentState();
}

uint64_t revision() { return gRevision.load(); }

namespace detail
{
/** Touches the publishers (all, or only these) and every slotSet so results and draws pick up a change. Defined in SlotSet.cpp. */
void touchPublishers(const std::set<NodeId>* only);
void notifyListeners();
} // namespace detail

namespace
{
/** Keys whose entry differs between two maps (added, removed or changed). */
template <class M>
std::set<std::string> changedKeys(const M& a, const M& b)
{
    std::set<std::string> out;
    for (const auto& [k, v] : a)
        if (auto it = b.find(k); it == b.end() || it->second.toJson() != v.toJson())
            out.insert(k);
    for (const auto& [k, v] : b)
        if (!a.contains(k))
            out.insert(k);
    return out;
}

/**
 * The publishers a change from one state to the next reaches: those holding a slot whose override or pin changed. None
 * (rerun them all) when the rules changed or too much did to look up.
 */
std::optional<std::set<NodeId>> affectedPublishers(const State& before, const State& after)
{
    if (rulesJson(before.rules) != rulesJson(after.rules))
        return std::nullopt;
    const std::set<std::string> overrides = changedKeys(before.overrides, after.overrides);
    const std::set<std::string> pins = changedKeys(before.pins, after.pins);
    if (overrides.size() + pins.size() > 256)
        return std::nullopt;
    std::set<NodeId> out;
    if (overrides.empty() && pins.empty())
        return out;
    std::vector<std::string> owners;
    std::set<std::string> ids = overrides;
    for (const std::string& k : pins)
        if (k.starts_with("owner:"))
            owners.push_back(k.substr(6));
        else
            ids.insert(k);
    std::lock_guard lock(tableMutex());
    for (const auto& [node, pub] : table())
        for (const Slot& s : pub.slots)
            if (ids.contains(s.id) ||
                std::any_of(owners.begin(), owners.end(), [&](const std::string& o) { return startsWithPath(s.owner, o) || startsWithPath(s.id, o); }))
            {
                out.insert(node);
                break;
            }
    return out;
}
} // namespace

void refresh()
{
    LILA_ZONE();
    const StatePtr before = state();
    auto next = std::make_shared<State>();
    const json rules = scene::data::get(kRulesKey, json::array());
    if (rules.is_array())
        for (const json& r : rules)
            next->rules.push_back(Rule::fromJson(r));
    const json overrides = scene::data::get(kOverridesKey, json::object());
    if (overrides.is_object())
        for (auto it = overrides.begin(); it != overrides.end(); ++it)
            next->overrides[it.key()] = Override::fromJson(*it);
    const json pins = scene::data::get(kPinsKey, json::object());
    if (pins.is_object())
        for (auto it = pins.begin(); it != pins.end(); ++it)
        {
            Pin p = Pin::fromJson(*it);
            p.key = it.key();
            next->pins[it.key()] = std::move(p);
        }
    next->revision = ++gRevision;
    const std::optional<std::set<NodeId>> only = affectedPublishers(*before, *next);
    {
        std::lock_guard lock(stateMutex());
        currentState() = std::move(next);
    }
    detail::touchPublishers(only ? &*only : nullptr);
    detail::notifyListeners();
}

std::string addRule(Rule r, int index)
{
    StatePtr st = state();
    std::vector<Rule> rules = st->rules;
    if (r.id.empty())
    {
        int n = 1;
        auto taken = [&](const std::string& id) { return std::any_of(rules.begin(), rules.end(), [&](const Rule& x) { return x.id == id; }); };
        while (taken("r" + std::to_string(n)))
            ++n;
        r.id = "r" + std::to_string(n);
    }
    auto it = std::find_if(rules.begin(), rules.end(), [&](const Rule& x) { return x.id == r.id; });
    if (it != rules.end())
        *it = r;
    else if (index >= 0 && size_t(index) < rules.size())
        rules.insert(rules.begin() + index, r);
    else
        rules.push_back(r);
    commit(kRulesKey, rulesJson(rules));
    return r.id;
}

bool removeRule(std::string_view id)
{
    std::vector<Rule> rules = state()->rules;
    const size_t before = rules.size();
    std::erase_if(rules, [&](const Rule& r) { return r.id == id; });
    if (rules.size() == before)
        return false;
    commit(kRulesKey, rulesJson(rules));
    return true;
}

bool moveRule(std::string_view id, int index)
{
    std::vector<Rule> rules = state()->rules;
    auto it = std::find_if(rules.begin(), rules.end(), [&](const Rule& r) { return r.id == id; });
    if (it == rules.end())
        return false;
    Rule r = *it;
    rules.erase(it);
    index = std::clamp(index, 0, int(rules.size()));
    rules.insert(rules.begin() + index, std::move(r));
    commit(kRulesKey, rulesJson(rules));
    return true;
}

void setRules(std::vector<Rule> rules) { commit(kRulesKey, rulesJson(rules)); }

void setOverride(const std::string& id, const Payload& payload, bool clear)
{
    std::map<std::string, Override> m = state()->overrides;
    Override& o = m[id];
    if (clear)
        o.payload = Payload{};
    o.payload.overlay(payload);
    if (std::optional<Slot> s = findSlot(scene::Graph::current(), id))
    {
        o.type = s->type;
        o.at = glm::dvec3(s->transform[3]);
        o.owner = s->owner;
    }
    commit(kOverridesKey, overridesJson(m));
}

bool clearOverride(const std::string& id)
{
    std::map<std::string, Override> m = state()->overrides;
    if (!m.erase(id))
        return false;
    commit(kOverridesKey, overridesJson(m));
    return true;
}

bool reassignOverride(const std::string& from, const std::string& to)
{
    std::map<std::string, Override> m = state()->overrides;
    auto it = m.find(from);
    if (it == m.end() || from == to)
        return false;
    Override o = it->second;
    m.erase(it);
    if (std::optional<Slot> s = findSlot(scene::Graph::current(), to))
    {
        o.type = s->type;
        o.at = glm::dvec3(s->transform[3]);
        o.owner = s->owner;
    }
    Override& dst = m[to];
    dst.type = o.type;
    dst.at = o.at;
    dst.owner = o.owner;
    dst.payload.overlay(o.payload);
    commit(kOverridesKey, overridesJson(m));
    return true;
}

bool pin(scene::Graph& g, const std::string& key, bool swappable)
{
    std::map<std::string, Pin> m = state()->pins;
    Pin p;
    p.key = key;
    const bool ownerPin = key.starts_with("owner:");
    const std::string owner = ownerPin ? key.substr(6) : std::string();
    auto covered = [&](const Slot& s) { return ownerPin ? startsWithPath(s.owner, owner) || startsWithPath(s.id, owner) : s.id == key; };
    for (Slot& s : collect(g, covered))
    {
        if (!swappable)
            s.payload = resolve(s).payload;
        p.slots.push_back({std::move(s), swappable});
    }
    if (p.slots.empty())
        return false;
    m[key] = std::move(p);
    commit(kPinsKey, pinsJson(m));
    return true;
}

bool unpin(const std::string& key)
{
    std::map<std::string, Pin> m = state()->pins;
    if (!m.erase(key))
        return false;
    commit(kPinsKey, pinsJson(m));
    return true;
}

namespace
{
/** The pin snapshot covering a slot id, or null. */
const PinnedSlot* pinnedSlot(const State& st, std::string_view id, std::string_view owner)
{
    if (auto it = st.pins.find(std::string(id)); it != st.pins.end() && !it->second.slots.empty())
        return &it->second.slots.front();
    for (const auto& [key, p] : st.pins)
    {
        if (!key.starts_with("owner:"))
            continue;
        const std::string_view o = std::string_view(key).substr(6);
        if (!(startsWithPath(owner, o) || startsWithPath(id, o)))
            continue;
        for (const PinnedSlot& s : p.slots)
            if (s.slot.id == id)
                return &s;
    }
    return nullptr;
}

/** An owner pin whose owner covers the slot, so slots it doesn't list are new under a frozen owner. */
const Pin* ownerPinCovering(const State& st, std::string_view id, std::string_view owner)
{
    for (const auto& [key, p] : st.pins)
        if (key.starts_with("owner:"))
        {
            const std::string_view o = std::string_view(key).substr(6);
            if (startsWithPath(owner, o) || startsWithPath(id, o))
                return &p;
        }
    return nullptr;
}
} // namespace

bool isPinned(std::string_view key)
{
    StatePtr st = state();
    if (st->pins.contains(std::string(key)))
        return true;
    const std::string_view k = key.starts_with("owner:") ? key.substr(6) : key;
    for (const auto& [pk, p] : st->pins)
        if (pk.starts_with("owner:") && startsWithPath(k, std::string_view(pk).substr(6)))
            return true;
    return false;
}

// Resolving

Registry<TagResolver>& tagResolvers()
{
    static Registry<TagResolver> r;
    return r;
}

namespace
{
std::mutex& tagMutex()
{
    static std::mutex m;
    return m;
}
/** Pool tags resolved on the main thread, read by workers. */
std::map<std::string, std::vector<std::string>>& tagCache()
{
    static std::map<std::string, std::vector<std::string>> c;
    return c;
}
} // namespace

namespace detail
{
std::vector<std::string> poolForTag(const std::string& tags, bool mainThread)
{
    {
        std::lock_guard lock(tagMutex());
        if (auto it = tagCache().find(tags); it != tagCache().end())
            return it->second;
    }
    if (!mainThread)
        return {};
    std::vector<std::string> out;
    for (TagResolver* r : tagResolvers().allInOrder())
    {
        out = (*r)(tags);
        if (!out.empty())
            break;
    }
    std::lock_guard lock(tagMutex());
    tagCache()[tags] = out;
    return out;
}

void clearTagCache()
{
    std::lock_guard lock(tagMutex());
    tagCache().clear();
}

/** resolve, with pool tags looked up through the resolvers when on the main thread. */
Resolved resolveImpl(const Slot& s, const State& st, bool mainThread)
{
    Resolved r;
    r.payload = s.payload;
    r.sources.push_back("default");
    const PinnedSlot* p = pinnedSlot(st, s.id, s.owner);
    if (p)
    {
        r.payload = p->slot.payload;
        r.sources = {"pin"};
        r.pinned = true;
    }
    if (!p || p->swappable)
    {
        for (Level level : {Level::Global, Level::Region, Level::Owner})
            for (const Rule& rule : st.rules)
                if (rule.level == level && rule.covers(s))
                {
                    r.payload.overlay(rule.payload);
                    r.sources.push_back("rule:" + rule.id);
                }
        if (auto it = st.overrides.find(s.id); it != st.overrides.end())
        {
            r.payload.overlay(it->second.payload);
            r.sources.push_back("override");
        }
    }
    std::vector<std::string> pool = r.payload.pool;
    if (pool.empty() && !r.payload.poolTag.empty())
        pool = poolForTag(r.payload.poolTag, mainThread);
    if (!pool.empty() && r.payload.item.empty())
    {
        r.poolPick = pool[size_t(idHash(s.id) % pool.size())];
        r.payload.item = r.poolPick;
    }
    return r;
}
} // namespace detail

Resolved resolve(const Slot& s, const State& st) { return detail::resolveImpl(s, st, false); }

Resolved resolve(const Slot& s)
{
    StatePtr st = state();
    return detail::resolveImpl(s, *st, true);
}

// Publishing

void publish(NodeId publisher, std::vector<Slot> slots)
{
    {
        std::lock_guard lock(tableMutex());
        table()[publisher].slots = std::move(slots);
    }
    ++gRevision;
}

void unpublish(NodeId publisher)
{
    std::lock_guard lock(tableMutex());
    table().erase(publisher);
}

void unpublishGone(const scene::Graph& g)
{
    LILA_ZONE();
    std::vector<Published> gone;
    {
        std::lock_guard lock(tableMutex());
        for (auto it = table().begin(); it != table().end();)
            if (!g.node(it->first))
            {
                gone.push_back(std::move(it->second));
                it = table().erase(it);
            }
            else
                ++it;
    }
    // A world's slots run to millions of small allocations.
    release::laterEach(std::move(gone));
}

std::vector<Slot> publishedSlots(NodeId publisher)
{
    std::lock_guard lock(tableMutex());
    auto it = table().find(publisher);
    return it == table().end() ? std::vector<Slot>{} : it->second.slots;
}

namespace
{
std::map<int, std::function<void()>>& listeners()
{
    static std::map<int, std::function<void()>> l;
    return l;
}
} // namespace

int addListener(std::function<void()> fn)
{
    static int next = 1;
    listeners()[next] = std::move(fn);
    return next++;
}

void removeListener(int id) { listeners().erase(id); }

namespace detail
{
void notifyListeners()
{
    const auto copy = listeners();
    for (const auto& [id, fn] : copy)
        fn();
}
} // namespace detail

namespace detail
{
std::vector<NodeId> publishedNodes()
{
    std::lock_guard lock(tableMutex());
    std::vector<NodeId> out;
    for (const auto& [n, p] : table())
        out.push_back(n);
    return out;
}

void forEachPublishedPayload(const std::function<void(const Payload&)>& fn)
{
    std::lock_guard lock(tableMutex());
    for (const auto& [n, p] : table())
        for (const Slot& s : p.slots)
            fn(s.payload);
}

std::vector<Slot> publishedBy(NodeId n)
{
    std::lock_guard lock(tableMutex());
    auto it = table().find(n);
    return it == table().end() ? std::vector<Slot>{} : it->second.slots;
}

/** Applies pins to one publisher's slots: pinned frames win, slots new under a pinned owner go, pinned ones it dropped stay. */
std::vector<Slot> withPins(std::vector<Slot> slots, const State& st, std::vector<std::string>* conflicts)
{
    if (st.pins.empty())
        return slots;
    std::vector<Slot> out;
    out.reserve(slots.size());
    for (Slot& s : slots)
    {
        if (const PinnedSlot* p = pinnedSlot(st, s.id, s.owner))
        {
            const bool moved = glm::length(glm::dvec3(p->slot.transform[3]) - glm::dvec3(s.transform[3])) > 0.01 || p->slot.type != s.type;
            if (moved && conflicts)
                conflicts->push_back(s.id);
            Slot keep = p->slot;
            keep.payload = s.payload;
            out.push_back(std::move(keep));
            continue;
        }
        if (ownerPinCovering(st, s.id, s.owner))
        {
            if (conflicts)
                conflicts->push_back(s.id);
            continue;
        }
        out.push_back(std::move(s));
    }
    return out;
}

int pinOf(const State& st, const Slot& s, Slot* out)
{
    if (st.pins.empty())
        return 0;
    if (const PinnedSlot* p = pinnedSlot(st, s.id, s.owner))
    {
        if (out)
        {
            *out = p->slot;
            out->payload = s.payload;
        }
        return 1;
    }
    return ownerPinCovering(st, s.id, s.owner) ? 2 : 0;
}

bool ruledOrOverridden(const State& st, const Slot& s)
{
    if (st.overrides.contains(s.id))
        return true;
    return std::any_of(st.rules.begin(), st.rules.end(), [&](const Rule& r) { return r.covers(s); });
}

/** Slots (by id) of every pin, for the ones no publisher makes any more. */
std::vector<const PinnedSlot*> allPinned(const State& st)
{
    std::vector<const PinnedSlot*> out;
    for (const auto& [k, p] : st.pins)
        for (const PinnedSlot& s : p.slots)
            out.push_back(&s);
    return out;
}
} // namespace detail
} // namespace lila::slots
