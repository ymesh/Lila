#include "layout/Layout.h"

#include <algorithm>
#include <cmath>

namespace lila::layout
{
namespace
{
constexpr std::array<const char*, 5> kProfileNames = {"v", "u", "square", "step", "chamfer"};
constexpr std::array<const char*, 5> kOutputNames = {"geometry", "sculpt", "texture", "decal", "none"};
constexpr std::array<const char*, 3> kEndNames = {"round", "square", "fade"};

template <size_t N> std::optional<size_t> indexOf(const std::array<const char*, N>& names, std::string_view s)
{
    for (size_t k = 0; k < N; ++k)
        if (s == names[k])
            return k;
    return std::nullopt;
}

json vec(const glm::dvec3& v) { return {v.x, v.y, v.z}; }

glm::dvec3 vecFrom(const json& j, glm::dvec3 def = glm::dvec3(0.0))
{
    if (!j.is_array() || j.size() != 3)
        return def;
    return {j[0].get<double>(), j[1].get<double>(), j[2].get<double>()};
}

json anchorJson(const Anchor& a)
{
    json j = {{"pos", vec(a.pos)}};
    if (a.face != kInvalid)
    {
        j["face"] = a.face;
        j["points"] = {a.points[0], a.points[1], a.points[2]};
        j["bary"] = vec(a.bary);
    }
    return j;
}

Anchor anchorFrom(const json& j)
{
    Anchor a;
    a.pos = vecFrom(j.value("pos", json()));
    if (j.contains("face"))
    {
        a.face = j["face"].get<Index>();
        const json& p = j.value("points", json::array());
        for (size_t k = 0; k < 3 && k < p.size(); ++k)
            a.points[k] = p[k].get<Index>();
        a.bary = vecFrom(j.value("bary", json()));
    }
    return a;
}
} // namespace

const char* profileName(Profile p) { return kProfileNames[size_t(p)]; }
std::optional<Profile> profileFromName(std::string_view s)
{
    if (auto k = indexOf(kProfileNames, s))
        return Profile(*k);
    return std::nullopt;
}
const char* outputName(Output o) { return kOutputNames[size_t(o)]; }
std::optional<Output> outputFromName(std::string_view s)
{
    if (auto k = indexOf(kOutputNames, s))
        return Output(*k);
    return std::nullopt;
}
const char* endStyleName(EndStyle e) { return kEndNames[size_t(e)]; }
std::optional<EndStyle> endStyleFromName(std::string_view s)
{
    if (auto k = indexOf(kEndNames, s))
        return EndStyle(*k);
    return std::nullopt;
}

double profileDepth(Profile p, double x)
{
    x = std::abs(x);
    if (x >= 1.0)
        return 0.0;
    switch (p)
    {
    case Profile::V: return 1.0 - x;
    case Profile::U: return std::sqrt(std::max(0.0, 1.0 - x * x));
    // Walls over the outer tenth, so the floor keeps most of the width.
    case Profile::Square: return x <= 0.9 ? 1.0 : (1.0 - x) / 0.1;
    // Full depth on the inner half, half depth on the outer.
    case Profile::Step: return x <= 0.45 ? 1.0 : x <= 0.55 ? 1.0 - (x - 0.45) / 0.1 * 0.5 : x <= 0.9 ? 0.5 : 0.5 * (1.0 - x) / 0.1;
    // A flat floor with 45 degree sides over the outer half.
    case Profile::Chamfer: return x <= 0.5 ? 1.0 : (1.0 - x) / 0.5;
    }
    return 0.0;
}

std::vector<double> profileBreaks(Profile p)
{
    switch (p)
    {
    case Profile::V: return {0.0, 1.0};
    case Profile::U: return {0.0, 0.38, 0.71, 0.92, 1.0};
    case Profile::Square: return {0.9, 1.0};
    case Profile::Step: return {0.45, 0.55, 0.9, 1.0};
    case Profile::Chamfer: return {0.5, 1.0};
    }
    return {1.0};
}

const Junction* PanelLayout::junction(uint32_t id) const
{
    auto it = std::find_if(junctions.begin(), junctions.end(), [id](const Junction& j) { return j.id == id; });
    return it == junctions.end() ? nullptr : &*it;
}
Junction* PanelLayout::junction(uint32_t id) { return const_cast<Junction*>(std::as_const(*this).junction(id)); }

const Run* PanelLayout::run(uint32_t id) const
{
    auto it = std::find_if(runs.begin(), runs.end(), [id](const Run& r) { return r.id == id; });
    return it == runs.end() ? nullptr : &*it;
}
Run* PanelLayout::run(uint32_t id) { return const_cast<Run*>(std::as_const(*this).run(id)); }

const LineSet* PanelLayout::set(uint32_t id) const
{
    auto it = std::find_if(sets.begin(), sets.end(), [id](const LineSet& s) { return s.id == id; });
    return it == sets.end() ? nullptr : &*it;
}
LineSet* PanelLayout::set(uint32_t id) { return const_cast<LineSet*>(std::as_const(*this).set(id)); }

const LineSet& PanelLayout::setOf(const Run& r) const
{
    static const LineSet kFallback{.id = 0, .name = "Lines"};
    if (const LineSet* s = set(r.set))
        return *s;
    return sets.empty() ? kFallback : sets.front();
}

std::vector<uint32_t> PanelLayout::runsAt(uint32_t j) const
{
    std::vector<uint32_t> out;
    for (const Run& r : runs)
        if (r.a == j || r.b == j)
            out.push_back(r.id);
    return out;
}

int PanelLayout::degree(uint32_t j) const
{
    int n = 0;
    for (const Run& r : runs)
        n += int(r.a == j) + int(r.b == j);
    return n;
}

void PanelLayout::removeUnusedJunctions()
{
    std::erase_if(junctions, [&](const Junction& j) {
        return std::none_of(runs.begin(), runs.end(), [&](const Run& r) { return r.a == j.id || r.b == j.id; });
    });
}

PanelLayout PanelLayout::withDefaultSets()
{
    PanelLayout l;
    l.sets.push_back({.id = l.newId(), .name = "Major", .width = 0.4, .depth = 0.2, .profile = Profile::V, .output = Output::Geometry});
    l.sets.push_back({.id = l.newId(), .name = "Minor", .width = 0.2, .depth = 0.08, .profile = Profile::V, .output = Output::Sculpt});
    l.activeSet = l.sets.front().id;
    return l;
}

json PanelLayout::toJson() const
{
    json js = json::array();
    for (const Junction& j : junctions)
    {
        json o = {{"id", j.id}, {"at", anchorJson(j.at)}};
        if (j.corner)
            o["corner"] = true;
        if (j.end != EndStyle::Round)
            o["end"] = endStyleName(j.end);
        js.push_back(std::move(o));
    }
    json rs = json::array();
    for (const Run& r : runs)
    {
        json o = {{"id", r.id}, {"a", r.a}, {"b", r.b}, {"set", r.set}};
        json c = json::array();
        for (const Anchor& a : r.controls)
            c.push_back(anchorJson(a));
        o["controls"] = std::move(c);
        if (r.width)
            o["width"] = *r.width;
        if (r.depth)
            o["depth"] = *r.depth;
        if (r.profile)
            o["profile"] = profileName(*r.profile);
        if (r.loose)
            o["loose"] = true;
        rs.push_back(std::move(o));
    }
    json ss = json::array();
    for (const LineSet& s : sets)
        ss.push_back({{"id", s.id},
                      {"name", s.name},
                      {"width", s.width},
                      {"depth", s.depth},
                      {"profile", profileName(s.profile)},
                      {"output", outputName(s.output)}});
    json ps = json::array();
    for (const PanelOverride& p : panels)
        ps.push_back({{"seed", anchorJson(p.seed)}, {"values", p.values}});
    json out = {{"junctions", js}, {"runs", rs}, {"sets", ss}, {"panels", ps}, {"activeSet", activeSet}, {"nextId", nextId}};
    if (mirrorAxis >= 0)
    {
        out["mirrorAxis"] = mirrorAxis;
        out["mirrorSide"] = mirrorSide;
    }
    return out;
}

PanelLayout PanelLayout::fromJson(const json& j)
{
    PanelLayout l;
    for (const json& o : j.value("junctions", json::array()))
    {
        Junction jn;
        jn.id = o.value("id", 0u);
        jn.at = anchorFrom(o.value("at", json::object()));
        jn.corner = o.value("corner", false);
        jn.end = endStyleFromName(o.value("end", "round")).value_or(EndStyle::Round);
        l.junctions.push_back(jn);
    }
    for (const json& o : j.value("runs", json::array()))
    {
        Run r;
        r.id = o.value("id", 0u);
        r.a = o.value("a", 0u);
        r.b = o.value("b", 0u);
        r.set = o.value("set", 0u);
        for (const json& c : o.value("controls", json::array()))
            r.controls.push_back(anchorFrom(c));
        if (o.contains("width"))
            r.width = o["width"].get<double>();
        if (o.contains("depth"))
            r.depth = o["depth"].get<double>();
        if (o.contains("profile"))
            r.profile = profileFromName(o["profile"].get<std::string>());
        r.loose = o.value("loose", false);
        l.runs.push_back(std::move(r));
    }
    for (const json& o : j.value("sets", json::array()))
    {
        LineSet s;
        s.id = o.value("id", 0u);
        s.name = o.value("name", "Lines");
        s.width = o.value("width", 0.4);
        s.depth = o.value("depth", 0.2);
        s.profile = profileFromName(o.value("profile", "v")).value_or(Profile::V);
        s.output = outputFromName(o.value("output", "geometry")).value_or(Output::Geometry);
        l.sets.push_back(std::move(s));
    }
    for (const json& o : j.value("panels", json::array()))
        l.panels.push_back({anchorFrom(o.value("seed", json::object())), o.value("values", json::object())});
    l.activeSet = j.value("activeSet", l.sets.empty() ? 0u : l.sets.front().id);
    l.mirrorAxis = std::clamp(j.value("mirrorAxis", -1), -1, 2);
    l.mirrorSide = j.value("mirrorSide", 1) < 0 ? -1 : 1;
    uint32_t maxId = 0;
    for (const Junction& jn : l.junctions)
        maxId = std::max(maxId, jn.id);
    for (const Run& r : l.runs)
        maxId = std::max(maxId, r.id);
    for (const LineSet& s : l.sets)
        maxId = std::max(maxId, s.id);
    l.nextId = std::max(j.value("nextId", 1u), maxId + 1);
    return l;
}

json LayoutData::toJson() const
{
    json j = layout.toJson();
    j["kind"] = "panelLayout";
    return j;
}

size_t LayoutData::bytes() const
{
    size_t n = sizeof(*this) + layout.junctions.size() * sizeof(Junction) + layout.runs.size() * sizeof(Run);
    for (const Run& r : layout.runs)
        n += r.controls.size() * sizeof(Anchor);
    return n;
}

scene::DataPtr LayoutData::fromJson(const json& j) { return std::make_shared<LayoutData>(PanelLayout::fromJson(j)); }

scene::DataPtr makeLayoutData(PanelLayout l) { return std::make_shared<LayoutData>(std::move(l)); }

const PanelLayout* layoutOf(const scene::DataPtr& d)
{
    if (!d || std::string_view(d->kind()) != "panelLayout")
        return nullptr;
    return &static_cast<const LayoutData&>(*d).layout;
}
} // namespace lila::layout
