#include "slots/SlotsGn.h"

#include "core/Profile.h"
#include "geo/Mesh.h"
#include "gn/InstanceView.h"
#include "gn/NodesCommon.h"
#include "math/Color.h"
#include "scene/Graph.h"

#include <algorithm>
#include <map>
#include <mutex>
#include <set>
#include <span>

namespace lila::slots
{
namespace detail
{
Resolved resolveImpl(const Slot& s, const State& st, bool mainThread);
std::vector<Slot> withPins(std::vector<Slot> slots, const State& st, std::vector<std::string>* conflicts);
/** How pins take one generated slot: 0 as it is, 1 pinned (its pinned slot in out, the generated payload kept), 2 dropped. */
int pinOf(const State& st, const Slot& s, Slot* out);
/** Whether any rule or an override reaches the slot. */
bool ruledOrOverridden(const State& st, const Slot& s);
std::vector<std::string> poolForTag(const std::string& tags, bool mainThread);
} // namespace detail

namespace
{
struct Pending
{
    std::map<std::string, std::pair<Resolved, std::string>> visuals; // key -> (resolved, type)
    std::set<std::string> tags;
    std::set<NodeId> publishers;
};
std::mutex& pendingMutex()
{
    static std::mutex m;
    return m;
}
Pending& pending()
{
    static Pending p;
    return p;
}

std::mutex& geoMutex()
{
    static std::mutex m;
    return m;
}
/**
 * Mesh -> geometry wrapper, so every instance of one visual shares a reference while any uses it. Held weakly: the
 * wrapper holds its mesh, so a strong entry would keep every visual ever placed alive.
 */
std::map<const geo::Mesh*, std::weak_ptr<const gn::Geometry>>& geoCache()
{
    static std::map<const geo::Mesh*, std::weak_ptr<const gn::Geometry>> c;
    return c;
}

gn::GeometryPtr geometryOf(const std::shared_ptr<const geo::Mesh>& m)
{
    std::lock_guard lock(geoMutex());
    auto& c = geoCache();
    if (gn::GeometryPtr g = c[m.get()].lock(); g && g->mesh == m)
        return g;
    if (c.size() > 1024)
        std::erase_if(c, [](const auto& e) { return e.second.expired(); });
    gn::Geometry g;
    g.mesh = m;
    gn::GeometryPtr out = gn::makeGeometry(std::move(g));
    c[m.get()] = out;
    return out;
}

bool setsVisual(const Payload& p)
{
    return !p.item.empty() || !p.mesh.empty() || !p.placeholder.empty() || !p.pool.empty() || !p.poolTag.empty();
}
} // namespace

std::string instancePath(const gn::Instances& in, size_t k, const std::vector<std::string>& pathAttributes)
{
    if (pathAttributes.empty())
        return "i" + std::to_string(k);
    std::string path;
    for (const std::string& a : pathAttributes)
    {
        std::span<const int32_t> v = in.attrs.get<int32_t>(a);
        if (!path.empty())
            path += '/';
        path += a + (k < v.size() ? std::to_string(v[k]) : std::string("?"));
    }
    return path;
}

gn::GeometryPtr applyToInstances(const gn::GeometryPtr& in, NodeId publisher, const InstanceSlots& spec)
{
    if (!in || !in->instances)
        return in;
    const gn::Instances& src = *in->instances;
    const std::string owner = spec.owner.empty() ? "unowned/" + std::to_string(publisher) : spec.owner;
    std::span<const int32_t> typeIdx = spec.typeAttribute.empty() ? std::span<const int32_t>{} : src.attrs.get<int32_t>(spec.typeAttribute);

    std::vector<Slot> slots;
    slots.reserve(src.size());
    const bool given = !spec.slots.empty() && spec.slots.size() == src.size();
    for (size_t k = 0; k < src.size(); ++k)
    {
        if (given)
        {
            Slot s = spec.slots[k];
            s.transform = src.transforms[k];
            slots.push_back(std::move(s));
            continue;
        }
        Slot s;
        std::string type = spec.types.empty() ? std::string("prop") : spec.types.front();
        if (!typeIdx.empty() && k < typeIdx.size() && !spec.types.empty())
            type = spec.types[size_t(std::clamp<int32_t>(typeIdx[k], 0, int32_t(spec.types.size()) - 1))];
        s.type = std::move(type);
        s.id = makeId(owner, instancePath(src, k, spec.pathAttributes));
        s.transform = src.transforms[k];
        s.owner = owner;
        s.regions = spec.regions;
        s.tags = spec.tags;
        s.payload = spec.defaults;
        slots.push_back(std::move(s));
    }
    return applySlots(in, publisher, std::move(slots));
}

std::optional<glm::vec4> dataColor(const json& data, const char* key)
{
    if (!data.is_object())
        return std::nullopt;
    auto it = data.find(key);
    if (it == data.end())
        return std::nullopt;
    if (it->is_string())
    {
        std::string hex = it->get<std::string>();
        if (!hex.empty() && hex.front() == '#')
            hex.erase(0, 1);
        if (hex.size() != 6 || hex.find_first_not_of("0123456789abcdefABCDEF") != std::string::npos)
            return std::nullopt;
        const unsigned long v = std::stoul(hex, nullptr, 16);
        return glm::vec4(math::srgbToLinear(float((v >> 16) & 255) / 255.0f), math::srgbToLinear(float((v >> 8) & 255) / 255.0f),
                         math::srgbToLinear(float(v & 255) / 255.0f), 1.0f);
    }
    if (it->is_array() && (it->size() == 3 || it->size() == 4))
    {
        glm::vec4 c(1.0f);
        for (size_t i = 0; i < it->size(); ++i)
        {
            if (!(*it)[i].is_number())
                return std::nullopt;
            c[int(i)] = (*it)[i].get<float>();
        }
        return c;
    }
    return std::nullopt;
}

gn::GeometryPtr applySlots(const gn::GeometryPtr& in, NodeId publisher, std::vector<Slot> slots)
{
    if (!in || !in->instances)
        return in;
    const gn::Instances& src = *in->instances;
    StatePtr st = state();
    slots.resize(src.size());
    for (size_t k = 0; k < src.size(); ++k)
        slots[k].transform = src.transforms[k];

    auto out = std::make_shared<gn::Instances>();
    out->references = src.references;
    out->attrs.mergeLayout(src.attrs);
    std::map<std::string, int> refOfKey;
    bool missing = false;
    std::vector<int32_t> slotIndex;
    // Payload colours (rules and overrides included) over the generator's own instance colours, which describe only its
    // own references: a swapped visual without a payload colour shows its own.
    std::vector<std::pair<size_t, glm::vec4>> payloadColors, payloadColors2;
    std::vector<size_t> ownColorsDropped;
    auto takeColors = [&](size_t row, const json& data) {
        if (!data.is_object())
            return false;
        bool any = false;
        if (data.contains("color"))
            if (std::optional<glm::vec4> c = dataColor(data))
                payloadColors.push_back({row, *c}), any = true;
        if (data.contains("color2"))
            if (std::optional<glm::vec4> c = dataColor(data, "color2"))
                payloadColors2.push_back({row, *c}), any = true;
        return any;
    };
    out->transforms.reserve(src.size());
    out->reference.reserve(src.size());
    slotIndex.reserve(src.size());
    const bool anyChange = !st->rules.empty() || !st->overrides.empty();
    Slot pinnedSlot;
    for (size_t k = 0; k < slots.size(); ++k)
    {
        // Pins: a pinned instance keeps its frame; one new under a pinned owner goes.
        const int pin = detail::pinOf(*st, slots[k], &pinnedSlot);
        if (pin == 2)
            continue;
        const Slot& s = pin == 1 ? pinnedSlot : slots[k];
        const std::optional<size_t> at = k;
        const bool generated = true;
        // Most slots are as their generator made them: no rule, override or pin reaches them, and their payload draws nothing
        // of its own, so their instance stays as it is without resolving.
        Resolved r;
        if (pin == 0 && !(anyChange && detail::ruledOrOverridden(*st, s)) && s.payload.pool.empty() && s.payload.poolTag.empty() &&
            !setsVisual(s.payload))
        {
            const size_t row = out->size();
            out->add(s.transform, src.reference[k]);
            slotIndex.push_back(int32_t(k));
            takeColors(row, s.payload.data);
            continue;
        }
        r = detail::resolveImpl(s, *st, false);
        if (r.payload.hidden && *r.payload.hidden)
            continue;
        if (!r.payload.poolTag.empty() && r.payload.pool.empty() && r.payload.item.empty() && r.poolPick.empty())
        {
            std::lock_guard lock(pendingMutex());
            pending().tags.insert(r.payload.poolTag);
            pending().publishers.insert(publisher);
            missing = true;
        }
        int ref = generated ? src.reference[*at] : -1;
        if (setsVisual(r.payload) || !generated)
        {
            const std::string key = visualKey(r, s.type);
            auto known = refOfKey.find(key);
            if (known != refOfKey.end())
                ref = known->second;
            else if (std::shared_ptr<const geo::Mesh> m = cachedVisual(key))
            {
                ref = int(out->references.size());
                out->references.push_back(geometryOf(m));
                refOfKey[key] = ref;
            }
            else
            {
                std::lock_guard lock(pendingMutex());
                pending().visuals[key] = {r, s.type};
                pending().publishers.insert(publisher);
                missing = true;
            }
        }
        if (ref < 0)
            continue;
        const size_t row = out->size();
        out->add(s.transform, ref);
        slotIndex.push_back(generated ? int32_t(*at) : -1);
        if (!takeColors(row, r.payload.data) && setsVisual(r.payload))
            ownColorsDropped.push_back(row);
    }
    // Every row's attributes from the instance it came from, in one pass.
    out->attrs.resize(out->size());
    for (size_t row = 0; row < slotIndex.size(); ++row)
        if (slotIndex[row] >= 0)
            out->attrs.copyFrom(src.attrs, row, size_t(slotIndex[row]));
    std::span<int32_t> idx = out->attrs.getOrAdd<int32_t>("slotIndex");
    std::copy(slotIndex.begin(), slotIndex.end(), idx.begin());
    auto writeColors = [&](const char* attr, const std::vector<std::pair<size_t, glm::vec4>>& given, bool second) {
        if (given.empty() && (ownColorsDropped.empty() || !out->attrs.has(attr)))
            return;
        std::vector<glm::vec4> colors(out->size());
        for (size_t k = 0; k < colors.size(); ++k)
            colors[k] = second ? gn::instanceColor2(*out, k) : gn::instanceColor(*out, k);
        for (const auto& [row, c] : given)
            colors[row] = c;
        for (size_t row : ownColorsDropped)
            colors[row] = glm::vec4(0.0f);
        out->attrs.remove(attr);
        std::span<glm::vec4> dst = out->attrs.getOrAdd<glm::vec4>(attr);
        std::copy(colors.begin(), colors.end(), dst.begin());
    };
    writeColors(gn::kInstanceColorAttr, payloadColors, false);
    writeColors(gn::kInstanceColor2Attr, payloadColors2, true);
    (void)missing;
    if (publisher != 0)
        publish(publisher, std::move(slots));
    gn::Geometry g = *in;
    g.instances = std::move(out);
    return gn::makeGeometry(std::move(g));
}

namespace detail
{
/** After an evaluation, on the main thread: builds the visuals and pool tags evaluations asked for, then runs them again. */
void servicePending(scene::Graph& g)
{
    LILA_ZONE();
    Pending work;
    {
        std::lock_guard lock(pendingMutex());
        if (pending().publishers.empty())
            return;
        work = std::move(pending());
        pending() = Pending{};
    }
    for (const std::string& t : work.tags)
        poolForTag(t, true);
    for (const auto& [key, rt] : work.visuals)
        visualFor(g, rt.first, rt.second, false);
    for (NodeId n : work.publishers)
        if (n != 0 && g.node(n))
            g.touch(n);
}

void registerGnNode()
{
    using namespace gn::nodes;
    auto list = [](const std::string& s) {
        std::vector<std::string> out;
        size_t start = 0;
        while (start < s.size())
        {
            size_t comma = s.find(',', start);
            if (comma == std::string::npos)
                comma = s.size();
            std::string part = s.substr(start, comma - start);
            while (!part.empty() && part.front() == ' ')
                part.erase(part.begin());
            while (!part.empty() && part.back() == ' ')
                part.pop_back();
            if (!part.empty())
                out.push_back(part);
            start = comma + 1;
        }
        return out;
    };
    gn::registerDecl({
        .type = "Slots",
        .label = "Slots",
        .category = "Instances",
        .doc = "Makes instances typed slots with stable ids (owner plus a path from int attributes, or the index), so slot rules and "
               "overrides swap what each one shows. Writes the int attribute slotIndex. Type takes a comma list picked per instance "
               "by Type Attribute.",
        .sockets = {.inputs = {geoIn("instances"), in("type", T::String, "prop", "Slot type, or a comma list indexed by Type Attribute."),
                               in("owner", T::String, "", "The owner's stable id; slot ids start with it."),
                               in("typeAttribute", T::String, "", "Int attribute picking each instance's entry in Type."),
                               in("pathAttributes", T::String, "", "Int attributes (comma list) making each slot's path; empty uses the index."),
                               in("regions", T::String, "", "Regions the slots lie in (comma list), for region rules."),
                               in("tags", T::String, "", "Tags every slot carries (comma list).")},
                    .outputs = {geoOut("instances")}},
        .exec =
            [list](gn::ExecContext& c) {
                InstanceSlots spec;
                spec.owner = c.s("owner");
                spec.types = list(c.s("type"));
                spec.typeAttribute = c.s("typeAttribute");
                spec.pathAttributes = list(c.s("pathAttributes"));
                spec.regions = list(c.s("regions"));
                spec.tags = list(c.s("tags"));
                c.out("instances", applyToInstances(c.geometry("instances"), NodeId(c.env.owner), spec));
            },
    });
}
} // namespace detail
} // namespace lila::slots
