#include "slots/Slots.h"

#include "core/Log.h"
#include "ops/Command.h"
#include "ops/Undo.h"
#include "scene/Diagnostics.h"
#include "scene/Graph.h"
#include "scene/Node.h"

#include <algorithm>
#include <functional>
#include <format>
#include <map>
#include <set>

namespace lila::slots
{
using scene::Graph;
using scene::Node;

namespace detail
{
std::vector<NodeId> publishedNodes();
std::vector<Slot> publishedBy(NodeId n);
void evaluatePublishers(Graph& g);
void forEachPublishedPayload(const std::function<void(const Payload&)>& fn);
std::vector<Slot> withPins(std::vector<Slot> slots, const State& st, std::vector<std::string>* conflicts);
Resolved resolveImpl(const Slot& s, const State& st, bool mainThread);
std::vector<std::string> poolForTag(const std::string& tags, bool mainThread);
} // namespace detail

namespace
{
constexpr const char* kValidator = "slots";

json vec(const glm::dvec3& v) { return json::array({v.x, v.y, v.z}); }

glm::dvec3 vecArg(const json& j)
{
    if (!j.is_array() || j.size() != 3)
        throw CommandError("expected [x, y, z]");
    return {j[0].get<double>(), j[1].get<double>(), j[2].get<double>()};
}

std::vector<std::string> listArg(const Args& a, const char* flag)
{
    std::vector<std::string> out;
    if (!a.has(flag))
        return out;
    const json& j = a.flags[flag];
    if (j.is_string())
        out.push_back(j.get<std::string>());
    else if (j.is_array())
        for (const json& v : j)
            out.push_back(v.get<std::string>());
    return out;
}

/** The payload fields a command's flags set (item, pool, poolTag, mesh, placeholder, unrealMesh, class, hidden, data). */
Payload payloadArgs(const Args& a)
{
    Payload p;
    p.item = a.get<std::string>("item");
    p.pool = listArg(a, "pool");
    p.poolTag = a.get<std::string>("poolTag");
    p.mesh = a.get<std::string>("mesh");
    p.placeholder = a.get<std::string>("placeholder");
    p.unrealMesh = a.get<std::string>("unrealMesh");
    p.unrealClass = a.get<std::string>("unrealClass");
    if (a.has("hidden"))
        p.hidden = a.get<bool>("hidden");
    if (a.has("data"))
    {
        if (!a.flags["data"].is_object())
            throw CommandError("data is {key: value}");
        p.data = a.flags["data"];
    }
    if (!p.placeholder.empty())
    {
        const auto& shapes = placeholderShapes();
        if (std::find(shapes.begin(), shapes.end(), p.placeholder) == shapes.end())
            throw CommandError("unknown placeholder '" + p.placeholder + "'");
    }
    return p;
}

std::vector<FlagDef> payloadFlags()
{
    using T = ArgType;
    return {
        {"item", "it", T::String, nullptr, "Library item id (Set Dressing library, \"Collection/Asset\")."},
        {"pool", "pl", T::List, nullptr, "Variant pool: item ids, one picked per slot by its id."},
        {"poolTag", "pt", T::String, nullptr, "Variant pool from library tags (\"fictional_signs\", \"a+b\" for both)."},
        {"mesh", "ms", T::String, nullptr, "A scene mesh (or group of meshes) drawn and exported in place of the slot."},
        {"placeholder", "ph", T::String, nullptr, "A shipped stand-in shape sized to the type's footprint.", placeholderShapes()},
        {"unrealMesh", "um", T::String, nullptr, "Unreal static mesh path for instanced placement (\"/Game/Props/SM_Door\")."},
        {"unrealClass", "uc", T::String, nullptr, "Unreal actor class spawned per slot (\"/Game/BP/BP_Door.BP_Door_C\"); \"-\" clears it."},
        {"hidden", "hd", T::Bool, nullptr, "Remove the part (true) or bring it back (false)."},
        {"data", "dt", T::Any, nullptr, "Gameplay key/values merged into the slot's data ({\"locked\": true}); a null value removes a key."},
    };
}

json slotJson(const Slot& s, const State& st, bool full)
{
    const Resolved r = detail::resolveImpl(s, st, true);
    json j = {{"id", s.id}, {"type", s.type}, {"owner", s.owner}, {"position", vec(glm::dvec3(s.transform[3]))},
              {"payload", r.payload.toJson()}, {"summary", r.payload.describe()}, {"sources", r.sources}};
    if (!r.poolPick.empty())
        j["poolPick"] = r.poolPick;
    if (r.pinned)
        j["pinned"] = true;
    if (full)
    {
        j["regions"] = s.regions;
        j["tags"] = s.tags;
        j["default"] = s.payload.toJson();
        j["matrix"] = s.toJson()["matrix"];
    }
    return j;
}

bool regionMatches(const Slot& s, const std::string& region)
{
    return std::any_of(s.regions.begin(), s.regions.end(), [&](const std::string& r) { return r == region || r.starts_with(region); });
}

/** Slots matching slotFind's filters. */
std::vector<Slot> findSlots(Graph& g, const Args& a)
{
    const std::string type = a.get<std::string>("type");
    const std::string region = a.get<std::string>("region");
    const std::vector<std::string> tags = listArg(a, "tag");
    const std::string owner = a.get<std::string>("owner");
    const std::vector<std::string> ids = listArg(a, "id");
    const std::string rule = a.get<std::string>("rule");
    const bool hasNear = a.has("near");
    const glm::dvec3 near = hasNear ? vecArg(a.flags["near"]) : glm::dvec3(0.0);
    const double radius = a.get<double>("radius", 0.0);
    StatePtr st = state();
    std::vector<Slot> out;
    for (Slot& s : collect(g))
    {
        if (!type.empty() && !typeMatches(type, s.type))
            continue;
        if (!region.empty() && !regionMatches(s, region))
            continue;
        if (!owner.empty() && !(s.owner == owner || s.owner.starts_with(owner + "/") || s.id.starts_with(owner + "/")))
            continue;
        if (!ids.empty() && std::find(ids.begin(), ids.end(), s.id) == ids.end())
            continue;
        if (std::any_of(tags.begin(), tags.end(), [&](const std::string& t) { return std::find(s.tags.begin(), s.tags.end(), t) == s.tags.end(); }))
            continue;
        if (hasNear && radius > 0.0 && glm::length(glm::dvec3(s.transform[3]) - near) > radius)
            continue;
        if (!rule.empty())
        {
            const Resolved r = detail::resolveImpl(s, *st, true);
            if (std::find(r.sources.begin(), r.sources.end(), "rule:" + rule) == r.sources.end())
                continue;
        }
        out.push_back(std::move(s));
    }
    if (hasNear)
        std::sort(out.begin(), out.end(), [&](const Slot& x, const Slot& y) {
            return glm::length(glm::dvec3(x.transform[3]) - near) < glm::length(glm::dvec3(y.transform[3]) - near);
        });
    return out;
}

/** Selects the nodes publishing the slots. Returns their paths. */
json selectPublishers(Graph& g, const std::vector<Slot>& slots)
{
    std::vector<NodeId> nodes;
    std::set<NodeId> seen;
    std::set<std::string> ids;
    for (const Slot& s : slots)
        ids.insert(s.id);
    for (const std::string& id : ids)
    {
        const NodeId n = publisherOf(g, id);
        if (n != scene::kNoNode && seen.insert(n).second)
            nodes.push_back(n);
    }
    g.select(nodes);
    json out = json::array();
    for (NodeId n : nodes)
        out.push_back(g.path(n));
    return out;
}

/** The nearest current slot of a type to a point, one of the same owner first when there is any; empty for none. */
std::string nearestOfType(const std::vector<Slot>& all, const std::string& type, const glm::dvec3& at, const std::set<std::string>& exclude,
                          const std::string& owner = {})
{
    if (!owner.empty())
    {
        std::vector<Slot> mine;
        for (const Slot& s : all)
            if (s.owner == owner)
                mine.push_back(s);
        if (std::string best = nearestOfType(mine, type, at, exclude); !best.empty())
            return best;
    }
    std::string best;
    double bestD = 1e300;
    for (const Slot& s : all)
    {
        if (s.type != type || exclude.contains(s.id))
            continue;
        const double d = glm::length(glm::dvec3(s.transform[3]) - at);
        if (d < bestD)
        {
            bestD = d;
            best = s.id;
        }
    }
    return best;
}

std::string pyString(const std::string& s) { return json(s).dump(); }

Rule ruleFromArgs(const Args& a, Rule r)
{
    if (a.has("rule"))
    {
        if (!a.flags["rule"].is_object())
            throw CommandError("slotRule: rule is a rule object ({level, type, payload, ...})");
        Rule j = Rule::fromJson(a.flags["rule"]);
        if (j.id.empty())
            j.id = r.id;
        r = j;
    }
    if (a.has("name"))
        r.name = a.get<std::string>("name");
    if (a.has("level"))
    {
        std::optional<Level> l = parseLevel(a.get<std::string>("level"));
        if (!l)
            throw CommandError("slotRule: level is global, region or owner");
        r.level = *l;
    }
    if (a.has("type"))
        r.type = a.get<std::string>("type");
    if (a.has("region"))
    {
        r.region = a.get<std::string>("region");
        if (!a.has("level"))
            r.level = Level::Region;
    }
    if (a.has("owner"))
    {
        r.owner = a.get<std::string>("owner");
        if (!a.has("level"))
            r.level = Level::Owner;
    }
    if (a.has("tag"))
        r.tags = listArg(a, "tag");
    if (a.has("enabled"))
        r.enabled = a.get<bool>("enabled");
    Payload p = payloadArgs(a);
    if (!p.empty())
    {
        if (a.get<bool>("replacePayload", false))
            r.payload = Payload{};
        r.payload.overlay(p);
    }
    if (r.level == Level::Region && r.region.empty())
        throw CommandError("slotRule: a region rule needs region=");
    if (r.level == Level::Owner && r.owner.empty())
        throw CommandError("slotRule: an owner rule needs owner=");
    return r;
}
} // namespace

json report(Graph& g)
{
    StatePtr st = state();
    json byType = json::object(), byRule = json::object(), byOwner = json::object();
    std::vector<std::string> conflicts;
    std::set<std::string> unregistered;
    json publishers = json::array();
    size_t unowned = 0, total = 0, hidden = 0;
    std::map<std::string, std::string> missingVisuals;
    // Each publisher's slots are counted as they are read, never all held at once: a city publishes millions. Ids are
    // kept as hashes; the few that repeat are named in a second pass.
    std::vector<uint64_t> idHashes;
    auto hashOf = [](std::string_view id) { return std::hash<std::string_view>{}(id); };
    auto look = [&](const Slot& s) {
        ++total;
        byType[s.type] = byType.value(s.type, 0) + 1;
        if (!resolveType(s.type))
            unregistered.insert(s.type);
        if (s.owner.starts_with("unowned/"))
            ++unowned;
        const std::string ownerRoot = s.owner.substr(0, s.owner.find('/'));
        byOwner[ownerRoot] = byOwner.value(ownerRoot, 0) + 1;
        const Resolved r = detail::resolveImpl(s, *st, true);
        for (const std::string& src : r.sources)
            if (src.starts_with("rule:"))
                byRule[src.substr(5)] = byRule.value(src.substr(5), 0) + 1;
        if (r.payload.hidden && *r.payload.hidden)
            ++hidden;
        if (!r.payload.item.empty() || !r.payload.mesh.empty())
        {
            const Visual v = visualFor(g, r, s.type, false);
            if (!v.missing.empty())
                missingVisuals[v.key] = v.missing;
        }
    };
    std::vector<uint64_t> raw;
    auto take = [&](std::vector<Slot> slots, const std::string& node, const char* kind, bool count) {
        size_t n = 0;
        for (const Slot& x : slots)
            raw.push_back(hashOf(x.id));
        for (Slot& x : detail::withPins(std::move(slots), *st, count ? &conflicts : nullptr))
        {
            ++n;
            idHashes.push_back(hashOf(x.id));
            look(x);
        }
        publishers.push_back({{"node", node}, {"kind", kind}, {"slots", n}});
    };
    auto forEachPublisher = [&](const std::function<void(std::vector<Slot>, const std::string&, const char*)>& f) {
        for (NodeId id : g.nodes())
            if (const Node* n = g.node(id); n && n->type().isA(kSetType))
                f(readSet(g, id), g.path(id), "set");
        for (NodeId pub : detail::publishedNodes())
            if (g.node(pub))
                f(detail::publishedBy(pub), g.path(pub), "nodes");
    };
    forEachPublisher([&](std::vector<Slot> v, const std::string& node, const char* kind) { take(std::move(v), node, kind, true); });
    std::sort(idHashes.begin(), idHashes.end());
    idHashes.erase(std::unique(idHashes.begin(), idHashes.end()), idHashes.end());
    auto seen = [&](std::string_view id) { return std::binary_search(idHashes.begin(), idHashes.end(), hashOf(id)); };
    size_t pinnedOrphans = 0;
    std::vector<Slot> orphanPins;
    for (const auto& [k, p] : st->pins)
        for (const PinnedSlot& ps : p.slots)
            if (!seen(ps.slot.id) && std::ranges::none_of(orphanPins, [&](const Slot& o) { return o.id == ps.slot.id; }))
            {
                orphanPins.push_back(ps.slot);
                ++pinnedOrphans;
                look(ps.slot);
            }

    json orphans = json::array();
    std::set<std::string> overridden;
    std::vector<const std::pair<const std::string, Override>*> lost;
    for (const auto& entry : st->overrides)
    {
        if (seen(entry.first) || std::ranges::any_of(orphanPins, [&](const Slot& o) { return o.id == entry.first; }))
            overridden.insert(entry.first);
        else
            lost.push_back(&entry);
    }
    if (!lost.empty())
    {
        // Only the slots an orphaned override could move to are gathered, to name the nearest.
        std::set<std::string> types;
        for (const auto* e : lost)
            types.insert(e->second.type);
        std::vector<Slot> candidates;
        forEachPublisher([&](std::vector<Slot> v, const std::string&, const char*) {
            for (Slot& x : detail::withPins(std::move(v), *st, nullptr))
                if (types.contains(x.type))
                    candidates.push_back(std::move(x));
        });
        for (const Slot& o : orphanPins)
            if (types.contains(o.type))
                candidates.push_back(o);
        for (const auto* e : lost)
        {
            const Override& ov = e->second;
            json o = {{"id", e->first}, {"type", ov.type}, {"at", vec(ov.at)}};
            const std::string near = nearestOfType(candidates, ov.type, ov.at, overridden, ov.owner);
            if (!near.empty())
                o["nearest"] = near;
            orphans.push_back(std::move(o));
        }
    }
    json duplicates = json::array();
    std::sort(raw.begin(), raw.end());
    std::set<uint64_t> repeated;
    for (size_t k = 1; k < raw.size(); ++k)
        if (raw[k] == raw[k - 1])
            repeated.insert(raw[k]);
    if (!repeated.empty())
    {
        std::map<std::string, int> idCount;
        forEachPublisher([&](std::vector<Slot> v, const std::string&, const char*) {
            for (const Slot& x : v)
                if (repeated.contains(hashOf(x.id)))
                    ++idCount[x.id];
        });
        for (const auto& [id, n] : idCount)
            if (n > 1)
                duplicates.push_back(id);
    }
    json rules = json::array();
    for (const Rule& r : st->rules)
        rules.push_back({{"id", r.id}, {"name", r.name}, {"level", levelName(r.level)}, {"type", r.type}, {"enabled", r.enabled},
                         {"slots", byRule.value(r.id, 0)}, {"summary", r.payload.describe()}});
    json missing = json::array();
    for (const auto& [k, why] : missingVisuals)
        missing.push_back({{"key", k}, {"why", why}});
    return {{"total", total},
            {"byType", byType},
            {"byRule", byRule},
            {"byOwner", byOwner},
            {"rules", rules},
            {"overrides", st->overrides.size()},
            {"orphans", orphans},
            {"pins", st->pins.size()},
            {"pinnedOrphans", pinnedOrphans},
            {"conflicts", conflicts},
            {"duplicates", duplicates},
            {"unregisteredTypes", unregistered},
            {"unowned", unowned},
            {"hidden", hidden},
            {"missingVisuals", missing},
            {"publishers", publishers}};
}

namespace
{
void runValidator()
{
    namespace dx = scene::diagnostics;
    const json r = report(Graph::current());
    for (const json& o : r["orphans"])
    {
        dx::Issue i;
        i.severity = dx::Severity::Warning;
        const std::string id = o["id"].get<std::string>();
        i.message = "Slots: override on " + id + " has no slot (its generator no longer makes it)";
        const json at = o["at"];
        i.select = std::format("from lila import ops\nops.slotFind(type={}, near={}, radius=2000, select=True)", pyString(o.value("type", "")), at.dump());
        if (o.contains("nearest"))
            i.fix = std::format("from lila import ops\nops.slotSet(id={}, reassign={})", pyString(id), pyString(o["nearest"].get<std::string>()));
        dx::report(std::move(i));
    }
    for (const json& c : r["conflicts"])
    {
        dx::Issue i;
        i.severity = dx::Severity::Info;
        const std::string id = c.get<std::string>();
        i.message = "Slots: " + id + " is pinned; its generator moved, retyped or added it, and the pin was kept";
        i.select = std::format("from lila import ops\nops.slotFind(id={}, select=True)", pyString(id));
        i.fix = std::format("from lila import ops\nops.slotPin({}, remove=True)", pyString(id));
        dx::report(std::move(i));
    }
    for (const json& d : r["duplicates"])
    {
        dx::Issue i;
        i.severity = dx::Severity::Error;
        i.message = "Slots: id " + d.get<std::string>() + " is made more than once; give the generators distinct owners";
        i.select = std::format("from lila import ops\nops.slotFind(id={}, select=True)", pyString(d.get<std::string>()));
        dx::report(std::move(i));
    }
    if (r["unowned"].get<size_t>() > 0)
    {
        dx::Issue i;
        i.severity = dx::Severity::Warning;
        i.message = std::format("Slots: {} slots come from Slots nodes with no Owner, so their ids change when the scene reopens",
                                r["unowned"].get<size_t>());
        dx::report(std::move(i));
    }
    for (const json& m : r["missingVisuals"])
    {
        dx::Issue i;
        i.severity = dx::Severity::Warning;
        i.message = "Slots: " + m["key"].get<std::string>() + " drawn as its placeholder: " + m["why"].get<std::string>();
        dx::report(std::move(i));
    }
}

/** Library items the scene names: in rules, overrides, pins and the slots' own payloads, pools and pool tags included. */
std::set<std::string> namedItems(Graph& g)
{
    std::set<std::string> out;
    auto take = [&](const Payload& p) {
        if (!p.item.empty())
            out.insert(p.item);
        out.insert(p.pool.begin(), p.pool.end());
        if (!p.poolTag.empty())
            for (const std::string& id : detail::poolForTag(p.poolTag, true))
                out.insert(id);
    };
    const StatePtr st = state();
    for (const Rule& r : st->rules)
        if (r.enabled)
            take(r.payload);
    for (const auto& [id, o] : st->overrides)
        take(o.payload);
    for (const auto& [key, pin] : st->pins)
        for (const PinnedSlot& ps : pin.slots)
            take(ps.slot.payload);
    // Payloads only: placing every slot in the world (a city holds millions) is not needed to name its items.
    for (NodeId id : g.nodes())
        if (const Node* n = g.node(id); n && n->type().isA(kSetType))
            for (const Slot& s : readSet(g, id))
                take(s.payload);
    detail::evaluatePublishers(g);
    detail::forEachPublishedPayload(take);
    return out;
}

/** The named items not loaded in this scene yet (asked of the item provider without loading). */
std::vector<std::string> unloadedItems(Graph& g)
{
    std::vector<std::string> out;
    MeshProvider* p = meshProviders().find("item");
    if (!p)
        return out;
    for (const std::string& id : namedItems(g))
    {
        std::string why;
        try
        {
            if (!(*p)(g, id, false, why))
                out.push_back(id);
        }
        catch (const std::exception&)
        {
            out.push_back(id);
        }
    }
    return out;
}

/** Builds every visual the scene's slots resolve to, loading what it must (inside the calling command's undo step). */
void prewarm(Graph& g)
{
    StatePtr st = state();
    std::set<std::string> done;
    for (const Slot& s : collect(g))
    {
        const Resolved r = detail::resolveImpl(s, *st, true);
        const std::string key = visualKey(r, s.type);
        if (!key.empty() && done.insert(key).second)
            visualFor(g, r, s.type, true);
    }
}
} // namespace

namespace detail
{
bool scheduleItemLoad()
{
    if (!ops::find("slotLoadItems") || unloadedItems(Graph::current()).empty())
        return false;
    try
    {
        const json r = ops::execute("slotLoadItems", json::object());
        log::info("Slots: loaded {} library item(s) the scene's slots name", r["loaded"].size());
        for (const json& m : r["missing"])
            log::warn("Slots: library item {} not loaded: {}", m["item"].get<std::string>(), m["why"].get<std::string>());
    }
    catch (const std::exception& e)
    {
        log::warn("Slots: library items the scene names were not loaded: {}", e.what());
    }
    return true;
}

void registerCommands()
{
    using T = ArgType;
    using lila::FlagDef;

    ops::registerCommand({
        .name = "slotTypes",
        .doc = "The slot types: id, label, tags, footprint (cm, width/height/depth), attach frame and placeholder shape. Types are "
               "dotted, most general first (door.shop is a door); rules naming door cover every door type.",
        .positionalDoc = "type pattern",
        .flags = {{"tag", "tg", T::String, nullptr, "Only types carrying this tag."},
                  {"add", "a", T::Any, nullptr,
                   "Register a type for this session: {id, label, tags, footprint [w, h, d], attach, placeholder, doc}. Extensions "
                   "register theirs in code so they come back on load."}},
        .undoable = false,
        .run = [](const Args& a) -> json {
            if (a.has("add"))
            {
                const json& j = a.flags["add"];
                if (!j.is_object() || !j.contains("id"))
                    throw CommandError("slotTypes: add is {id, label, tags, footprint, attach, placeholder, doc}");
                SlotType t;
                t.id = j["id"].get<std::string>();
                t.label = j.value("label", t.id);
                if (j.contains("tags"))
                    t.tags = j["tags"].get<std::vector<std::string>>();
                if (j.contains("footprint"))
                    t.footprint = vecArg(j["footprint"]);
                if (j.contains("attach"))
                {
                    std::optional<Attach> at = parseAttach(j["attach"].get<std::string>());
                    if (!at)
                        throw CommandError("slotTypes: attach is ground, wall, ceiling, roof or free");
                    t.attach = *at;
                }
                t.placeholder = j.value("placeholder", std::string("box"));
                t.doc = j.value("doc", std::string());
                registerType(std::move(t));
            }
            const std::string pattern = a.positional.empty() ? std::string() : a.positional[0].get<std::string>();
            const std::string tag = a.get<std::string>("tag");
            json out = json::array();
            for (const std::string& id : types().ids())
            {
                const SlotType* t = types().find(id);
                if (!pattern.empty() && !typeMatches(pattern, id))
                    continue;
                if (!tag.empty() && std::find(t->tags.begin(), t->tags.end(), tag) == t->tags.end())
                    continue;
                json j = t->toJson();
                const std::string* owner = types().ownerOf(id);
                j["owner"] = owner && !owner->empty() ? *owner : std::string("lila");
                out.push_back(std::move(j));
            }
            return out;
        },
        .examples = {"slotTypes('door')", "slotTypes(add={'id': 'sign.neon', 'footprint': [200, 60, 10], 'attach': 'wall', 'placeholder': 'sign'})"},
    });

    ops::registerCommand({
        .name = "slotFind",
        .doc = "Find slots by type, region, tag, owner, rule or distance, with each one's resolved payload and what decided it "
               "(default, rule:<id>, override, pin). select=True selects the nodes publishing them.",
        .flags = {{"type", "t", T::String, nullptr, "Type pattern: \"door\" covers every door, \"door.*\" only the subtypes, a comma list any."},
                  {"region", "r", T::String, nullptr, "A region the slot lies in (\"settlement:Town\", \"district:\" for any district)."},
                  {"tag", "tg", T::Any, nullptr, "Tags the slot must all carry (one or a list)."},
                  {"owner", "o", T::String, nullptr, "Owner id or a prefix of it (\"lot/1a2b\")."},
                  {"id", "id", T::Any, nullptr, "Exact slot ids (one or a list)."},
                  {"rule", "ru", T::String, nullptr, "Only slots this rule applies to."},
                  {"near", "n", T::List, nullptr, "[x, y, z]: nearest first (with radius, only within it)."},
                  {"radius", "rad", T::Float, 0.0, "With near: furthest distance, cm; 0 for any.", {}, {}, 0.0},
                  {"limit", "l", T::Int, 500, "Most slots listed, 0 for all; count is always the full number.", {}, {}, 0.0},
                  {"full", "f", T::Bool, false, "Add regions, tags, default payload and matrix."},
                  {"select", "sl", T::Bool, false, "Select the nodes publishing them."}},
        .undoable = true,
        .run = [](const Args& a) -> json {
            Graph& g = Graph::current();
            const std::vector<Slot> found = findSlots(g, a);
            StatePtr st = state();
            json list = json::array();
            const int64_t asked = a.get<int64_t>("limit", 500);
            const size_t limit = asked <= 0 ? found.size() : size_t(asked);
            for (size_t i = 0; i < found.size() && i < limit; ++i)
                list.push_back(slotJson(found[i], *st, a.get<bool>("full", false)));
            json out = {{"count", found.size()}, {"slots", list}};
            if (a.get<bool>("select", false))
                out["selected"] = selectPublishers(g, found);
            return out;
        },
        .examples = {"slotFind(type='sign.storefront', region='district:', limit=10)", "slotFind(near=[0, 0, 0], radius=5000, select=True)"},
    });

    std::vector<FlagDef> ruleFlags = {
        {"id", "id", T::String, nullptr, "The rule (edit, remove, move); add takes it as the new rule's id."},
        {"name", "nm", T::String, nullptr, "A label for the rule."},
        {"level", "lv", T::String, nullptr, "global, region or owner: rules apply in that order, then overrides.", {"global", "region", "owner"}},
        {"type", "t", T::String, nullptr, "Type pattern the rule covers (\"sign.storefront\", \"door\", \"*\")."},
        {"region", "r", T::String, nullptr, "Region rule: a region slots list (\"district:3\", \"settlement:Town\")."},
        {"owner", "o", T::String, nullptr, "Owner rule: an owner id or its prefix (a building, lot, street)."},
        {"tag", "tg", T::List, nullptr, "Slots must carry every tag."},
        {"enabled", "en", T::Bool, nullptr, "Switch the rule on or off."},
        {"index", "ix", T::Int, -1, "Position in the list (add, move); -1 for the end."},
        {"rule", "rl", T::Any, nullptr, "The whole rule as JSON ({level, type, region, owner, tags, payload, enabled})."},
        {"replacePayload", "rp", T::Bool, false, "edit: start the payload over instead of overlaying the given fields."},
    };
    for (FlagDef& f : payloadFlags())
        ruleFlags.push_back(std::move(f));
    ops::registerCommand({
        .name = "slotRule",
        .doc = "Add, edit, remove, move or list swap rules. A rule gives every slot it covers its payload (an item, a variant pool, "
               "a mesh, an Unreal class, data); rules apply global, then region, then owner, each in list order, then per-slot "
               "overrides. Rules are saved in the scene; every change undoes.",
        .positionalDoc = "action: add | edit | remove | move | list",
        .flags = ruleFlags,
        .run = [](const Args& a) -> json {
            const std::string action = a.positional.empty() ? std::string("list") : a.positional[0].get<std::string>();
            Graph& g = Graph::current();
            if (action == "list")
            {
                json rules = json::array();
                for (const Rule& r : state()->rules)
                    rules.push_back(r.toJson());
                return rules;
            }
            const std::string id = a.get<std::string>("id");
            if (action == "add")
            {
                Rule r;
                r.id = id;
                r = ruleFromArgs(a, r);
                if (!id.empty())
                    r.id = id;
                const std::string made = addRule(r, int(a.get<int64_t>("index", -1)));
                prewarm(g);
                return {{"id", made}, {"slots", findSlots(g, Args{.flags = {{"rule", made}}}).size()}};
            }
            const StatePtr st = state();
            auto it = std::find_if(st->rules.begin(), st->rules.end(), [&](const Rule& r) { return r.id == id; });
            if (id.empty() || it == st->rules.end())
                throw CommandError("slotRule: no rule '" + id + "' (slotRule('list') lists them)");
            if (action == "remove")
            {
                removeRule(id);
                prewarm(g);
                return {{"removed", id}};
            }
            if (action == "move")
            {
                moveRule(id, int(a.get<int64_t>("index", -1) < 0 ? int64_t(st->rules.size()) : a.get<int64_t>("index")));
                return {{"moved", id}};
            }
            if (action == "edit")
            {
                Rule r = ruleFromArgs(a, *it);
                r.id = id;
                addRule(r);
                prewarm(g);
                return {{"id", id}, {"slots", findSlots(g, Args{.flags = {{"rule", id}}}).size()}};
            }
            throw CommandError("slotRule: action is add, edit, remove, move or list");
        },
        .examples = {"slotRule('add', type='sign.storefront', poolTag='fictional_signs', name='Fictional signs')",
                     "slotRule('add', level='region', region='district:3', type='door.shop', unrealClass='/Game/BP/BP_ShopDoor.BP_ShopDoor_C')",
                     "slotRule('edit', id='r1', enabled=False)", "slotRule('remove', id='r1')"},
    });

    std::vector<FlagDef> setFlags = {
        {"id", "id", T::Any, nullptr, "Slot ids (one or a list)."},
        {"clear", "cl", T::Bool, false, "Start the override over before applying the given fields."},
        {"remove", "rm", T::Bool, false, "Drop the override: the slot goes back to its rules."},
        {"reassign", "ra", T::String, nullptr, "Move an orphaned override from id to this slot id."},
    };
    for (FlagDef& f : payloadFlags())
        setFlags.push_back(std::move(f));
    ops::registerCommand({
        .name = "slotSet",
        .doc = "Override slots by id: the payload fields given replace what rules gave (item, pool, mesh, Unreal class), data merges "
               "key by key. Overrides are keyed by the stable slot id, so they survive regenerating the generator; one whose slot "
               "disappears is reported as an orphan (Validation panel) and can be reassigned.",
        .flags = setFlags,
        .run = [](const Args& a) -> json {
            Graph& g = Graph::current();
            const std::vector<std::string> ids = listArg(a, "id");
            if (ids.empty())
                throw CommandError("slotSet: id= names the slots");
            if (a.has("reassign"))
            {
                if (ids.size() != 1)
                    throw CommandError("slotSet: reassign takes one id");
                if (!reassignOverride(ids[0], a.get<std::string>("reassign")))
                    throw CommandError("slotSet: no override on " + ids[0]);
                prewarm(g);
                return {{"reassigned", ids[0]}, {"to", a.get<std::string>("reassign")}};
            }
            json out = json::array();
            if (a.get<bool>("remove", false))
            {
                for (const std::string& id : ids)
                    if (clearOverride(id))
                        out.push_back(id);
                prewarm(g);
                return {{"removed", out}};
            }
            const Payload p = payloadArgs(a);
            const bool clear = a.get<bool>("clear", false);
            if (p.empty() && !clear)
                throw CommandError("slotSet: give a payload field (item, pool, poolTag, mesh, placeholder, unrealMesh, unrealClass, hidden, data)");
            UndoStack::get().openChunk("slotSet");
            for (const std::string& id : ids)
                setOverride(id, p, clear);
            UndoStack::get().closeChunk();
            prewarm(g);
            StatePtr st = state();
            for (const std::string& id : ids)
                if (std::optional<Slot> s = findSlot(g, id))
                    out.push_back(slotJson(*s, *st, false));
                else
                    out.push_back({{"id", id}, {"orphan", true}});
            return out;
        },
        .examples = {"slotSet(id='settlement:Town/st1a2b/lamp-3c', item='Street/LampOld')",
                     "slotSet(id=['b/12/f0/door0'], unrealClass='/Game/BP/BP_Door.BP_Door_C', data={'locked': True, 'missionId': 'm04'})",
                     "slotSet(id='old/id', reassign='new/id')"},
    });

    ops::registerCommand({
        .name = "slotPin",
        .doc = "Pin slots, or every slot of an owner (a building, block or arrangement), as they are now: regenerating leaves them "
               "where they are with what they show, and differences are reported as conflicts. With no key, lists the pins.",
        .positionalDoc = "slot id, or owner:<owner id>",
        .flags = {{"owner", "o", T::String, nullptr, "Pin every slot of this owner (same as owner:<id>)."},
                  {"swappable", "sw", T::Bool, false, "Hold off regeneration only: rules and overrides still swap what the slots show."},
                  {"remove", "rm", T::Bool, false, "Unpin."}},
        .run = [](const Args& a) -> json {
            Graph& g = Graph::current();
            std::vector<std::string> keys;
            for (const json& p : a.positional)
                keys.push_back(p.get<std::string>());
            if (a.has("owner"))
                keys.push_back("owner:" + a.get<std::string>("owner"));
            json done = json::array();
            for (const std::string& k : keys)
            {
                const bool ok = a.get<bool>("remove", false) ? unpin(k) : pin(g, k, a.get<bool>("swappable", false));
                if (!ok)
                    throw CommandError("slotPin: " + std::string(a.get<bool>("remove", false) ? "no pin " : "no slot matches ") + k);
                done.push_back(k);
            }
            json pins = json::array();
            for (const auto& [k, p] : state()->pins)
                pins.push_back({{"key", k}, {"slots", p.slots.size()}, {"swappable", !p.slots.empty() && p.slots.front().swappable}});
            return {{"changed", done}, {"pins", pins}};
        },
        .examples = {"slotPin('owner:settlement:Town/lot/1a2b')", "slotPin('b/12/f0/door0', remove=True)", "slotPin()"},
    });

    ops::registerCommand({
        .name = "slotReport",
        .doc = "Counts of slots per type, rule and owner, the rules with how many slots each covers, orphaned overrides (with the "
               "nearest slot of their type to reassign to), pin conflicts, duplicate ids, unregistered types and payloads that fell "
               "back to placeholders. validate=True also lists them in the Validation panel.",
        .flags = {{"validate", "v", T::Bool, true, "Report problems to the Validation panel too."}},
        .undoable = false,
        .run = [](const Args& a) -> json {
            if (a.get<bool>("validate", true))
                scene::diagnostics::validate({kValidator});
            return report(Graph::current());
        },
        .examples = {"r = slotReport()\nr['byType'], r['orphans']"},
    });

    ops::registerCommand({
        .name = "slotPublish",
        .doc = "Write slots into a slotSet node (made under parent when missing), for scripts and generators without nodes of their "
               "own. Each slot: {id | owner + path, type, matrix (16, column-major) | position [x, y, z] + rotateY degrees, regions, "
               "tags, payload}. Pinned slots keep their pinned frame.",
        .positionalDoc = "slotSet name",
        .flags = {{"slots", "s", T::List, nullptr, "The slots, world space."},
                  nodeFlag({"parent", "p", T::String, nullptr, "Node to put a new slotSet under."}, "transform"),
                  {"source", "src", T::String, nullptr, "What wrote them (shown on the node)."}},
        .run = [](const Args& a) -> json {
            Graph& g = Graph::current();
            if (a.positional.empty())
                throw CommandError("slotPublish: name the slotSet");
            NodeId parent = scene::kNoNode;
            if (a.has("parent"))
            {
                const Node* p = g.find(a.get<std::string>("parent"));
                if (!p)
                    throw CommandError("slotPublish: no node " + a.get<std::string>("parent"));
                parent = p->id();
            }
            std::vector<Slot> slots;
            for (const json& j : a.flags.value("slots", json::array()))
            {
                Slot s = Slot::fromJson(j);
                if (s.id.empty())
                    s.id = makeId(s.owner, j.value("path", std::string()));
                if (s.id.empty())
                    throw CommandError("slotPublish: a slot needs id, or owner and path");
                if (s.type.empty())
                    throw CommandError("slotPublish: slot " + s.id + " has no type");
                if (!j.contains("matrix") && j.contains("position"))
                {
                    const glm::dvec3 p = vecArg(j["position"]);
                    const double turn = glm::radians(j.value("rotateY", 0.0));
                    s.transform = glm::dmat4(1.0);
                    s.transform[0] = glm::dvec4(std::cos(turn), 0, -std::sin(turn), 0);
                    s.transform[2] = glm::dvec4(std::sin(turn), 0, std::cos(turn), 0);
                    s.transform[3] = glm::dvec4(p, 1.0);
                }
                slots.push_back(std::move(s));
            }
            json out = writeSet(g, a.positional[0].get<std::string>(), parent, slots, a.get<std::string>("source"));
            prewarm(g);
            return out;
        },
        .examples = {"slotPublish('shopSlots', slots=[{'owner': 'lot/7', 'path': 'f0/door', 'type': 'door.shop', 'position': [0, 0, 500]}])"},
    });

    ops::registerCommand({
        .name = "slotLoadItems",
        .doc = "Load the Set Dressing library items the scene's slots name (rules, overrides, pins, pools, pool tags and the slots' own "
               "payloads) so they draw as themselves rather than placeholders. Runs by itself when a scene opens with named items "
               "not loaded yet. Returns {loaded: [item ids], missing: [{item, why}]} (missing: no such library item, or its model "
               "brought in nothing).",
        .run = [](const Args&) -> json {
            Graph& g = Graph::current();
            json loaded = json::array(), missing = json::array();
            MeshProvider* p = meshProviders().find("item");
            const std::vector<std::string> todo = unloadedItems(g);
            if (!todo.empty() && !p)
                throw CommandError("slotLoadItems: nothing provides library items (load the Set Dressing extension)");
            for (const std::string& id : todo)
            {
                std::string why;
                std::shared_ptr<const geo::Mesh> mesh;
                try
                {
                    mesh = (*p)(g, id, true, why);
                }
                catch (const std::exception& e)
                {
                    why = e.what();
                }
                if (mesh)
                    loaded.push_back(id);
                else
                    missing.push_back({{"item", id}, {"why", why.empty() ? "it gave no mesh" : why}});
            }
            if (!loaded.empty())
            {
                clearVisuals();
                prewarm(g);
                refresh();
            }
            return {{"loaded", loaded}, {"missing", missing}};
        },
        .examples = {"slotLoadItems()"},
    });

    scene::diagnostics::validators().put(kValidator, {.doc = "Slots: orphaned overrides, pin conflicts, duplicate ids, unowned Slots nodes and payloads drawn as placeholders.",
                                                      .run = runValidator});
}
} // namespace detail
} // namespace lila::slots
