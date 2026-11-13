// Settlement: street furniture along roads from per-profile sets, at junctions by their control, and on squares.

#include "Streetscape.h"

#include <lila/Profile.h>

#include <algorithm>
#include <cctype>
#include <cmath>
#include <stdexcept>

namespace settlement
{
namespace
{
inline Vec2 planXZ(const glm::dvec3& p) { return {p.x, p.z}; }
inline Vec2 kerbSide(Vec2 d) { return {-d.y, d.x}; }

uint64_t furnitureMix(uint64_t h, uint64_t v) { return hash64(h ^ (v + 0x9E3779B97F4A7C15ull + (h << 6) + (h >> 2))); }

uint64_t textKey(std::string_view s)
{
    uint64_t h = 0xCBF29CE484222325ull;
    for (char c : s)
        h = (h ^ uint8_t(c)) * 0x100000001B3ull;
    return h;
}

uint64_t pointKey(Vec2 p) { return furnitureMix(uint64_t(std::llround(p.x / 10.0)), uint64_t(std::llround(p.y / 10.0))); }

double headingOf(Vec2 facing) { return glm::degrees(std::atan2(facing.x, facing.y)); }

FurnitureRule rule(const char* kind, double spacing, const char* where, const char* sides, double minWidth = 0.0)
{
    return {kind, spacing, where, sides, minWidth};
}

constexpr double kCurb = 15.0;
} // namespace

std::vector<FurnitureSet> furnitureSets(bool medieval)
{
    if (medieval)
        return {
            {"highway", {}},
            {"avenue", {rule("lantern", 4000.0, "edge", "alternate")}},
            {"street", {rule("lantern", 4000.0, "edge", "alternate")}},
            {"parkedStreet", {rule("lantern", 4000.0, "edge", "alternate")}},
            {"lane", {rule("lantern", 5000.0, "edge", "right")}},
            {"alley", {rule("lantern", 6000.0, "edge", "right")}},
            {"serviceRoad", {}},
            {"track", {}},
            {"footpath", {}},
            {"stairs", {rule("lantern", 3000.0, "edge", "right")}},
        };
    return {
        {"highway", {rule("lamp", 4000.0, "median", "right", 100.0)}},
        {"avenue",
         {rule("lamp", 3000.0, "kerb", "alternate"), rule("tree", 1000.0, "kerb", "both", 250.0), rule("bench", 6000.0, "back", "both", 250.0),
          rule("bin", 5000.0, "back", "alternate", 200.0), rule("hydrant", 9000.0, "kerb", "right")}},
        {"street", {rule("lamp", 3500.0, "kerb", "alternate"), rule("tree", 1200.0, "kerb", "both", 250.0), rule("hydrant", 9000.0, "kerb", "right")}},
        {"parkedStreet",
         {rule("lamp", 3500.0, "kerb", "alternate"), rule("tree", 1200.0, "kerb", "both", 180.0), rule("hydrant", 9000.0, "kerb", "right")}},
        {"lane", {rule("lamp", 4000.0, "kerb", "right")}},
        {"alley", {rule("lamp", 5000.0, "edge", "right")}},
        {"serviceRoad", {rule("lamp", 5000.0, "verge", "right")}},
        {"track", {}},
        {"footpath", {rule("lamp", 3000.0, "edge", "right"), rule("bench", 6000.0, "edge", "left")}},
        {"stairs", {rule("lamp", 3000.0, "edge", "right")}},
    };
}

json toJson(const FurnitureSet& s)
{
    json rules = json::array();
    for (const FurnitureRule& r : s.rules)
        rules.push_back({{"kind", r.kind}, {"spacing", r.spacing}, {"where", r.where}, {"sides", r.sides}, {"minWidth", r.minWidth}});
    return rules;
}

std::vector<FurnitureSet> applyFurnitureJson(std::vector<FurnitureSet> base, const json& j)
{
    if (j.is_null())
        return base;
    if (!j.is_object())
        throw std::runtime_error("furniture sets are {profile: [rule, ...]}");
    for (const auto& [profile, rules] : j.items())
    {
        if (!rules.is_array())
            throw std::runtime_error("furniture set " + profile + ": a list of rules");
        FurnitureSet set{profile, {}};
        for (const json& r : rules)
        {
            if (!r.is_object() || !r.contains("kind"))
                throw std::runtime_error("furniture set " + profile + ": each rule is an object with a kind");
            FurnitureRule x;
            x.kind = r["kind"].get<std::string>();
            x.spacing = std::max(100.0, r.value("spacing", x.spacing));
            x.where = r.value("where", x.where);
            x.sides = r.value("sides", x.sides);
            x.minWidth = std::max(0.0, r.value("minWidth", 0.0));
            if (x.where != "kerb" && x.where != "back" && x.where != "verge" && x.where != "median" && x.where != "edge")
                throw std::runtime_error("furniture set " + profile + ": where is kerb, back, verge, median or edge");
            if (x.sides != "both" && x.sides != "alternate" && x.sides != "right" && x.sides != "left")
                throw std::runtime_error("furniture set " + profile + ": sides is both, alternate, right or left");
            set.rules.push_back(std::move(x));
        }
        auto it = std::find_if(base.begin(), base.end(), [&](const FurnitureSet& s) { return s.profile == profile; });
        if (it != base.end())
            *it = std::move(set);
        else
            base.push_back(std::move(set));
    }
    return base;
}

std::vector<FurnitureSet> furnitureSetsFor(const Layout& l, const json& edits)
{
    const json furniture = edits.is_object() ? edits.value("furniture", json::object()) : json::object();
    return applyFurnitureJson(furnitureSets(medievalStyle(l)), furniture.is_object() ? furniture.value("sets", json()) : json());
}

std::string furnitureAsset(const json& edits, const std::string& kind)
{
    if (edits.is_object())
        if (const json f = edits.value("furniture", json::object()); f.is_object())
            if (const json assets = f.value("assets", json::object()); assets.is_object() && assets.contains(kind) && assets[kind].is_string())
                return assets[kind].get<std::string>();
    std::string name = kind;
    if (!name.empty())
        name[0] = char(std::toupper(static_cast<unsigned char>(name[0])));
    return "/Game/Settlement/Furniture/SM_" + name;
}

void addFurniturePlacements(Layout& l, const Ground& ground, const json& edits)
{
    LILA_ZONE();

    const std::vector<FurnitureSet> sets = furnitureSetsFor(l, edits);
    l.placements.clear();
    for (const FurniturePlacement& f : placeFurniture(l, roadNetwork(l, ground), ground, sets))
    {
        const double half = glm::radians(f.heading) * 0.5;
        Placement p;
        p.id = std::to_string(f.id);
        p.asset = furnitureAsset(edits, f.kind);
        p.position = f.p;
        p.rotation = {0.0, std::sin(half), 0.0, std::cos(half)};
        p.tags = {"settlement", "furniture", f.kind, f.profile};
        l.placements.push_back(std::move(p));
    }
}

std::vector<FurniturePlacement> placeFurniture(const Layout& l, const RoadNetwork& net, const Ground& ground, const std::vector<FurnitureSet>& sets)
{
    LILA_ZONE();

    std::vector<FurniturePlacement> out;
    const bool medieval = medievalStyle(l);
    auto groundAt = [&](Vec2 q, double up) { return glm::dvec3(q.x, ground.surfaceHeight(q) + up, q.y); };

    // Along roads: each rule's slots spread evenly over the road, counted from its lower-coordinate end so the slots
    // keep their ids whichever way the road was walked.
    for (size_t k = 0; k < net.lines.size(); ++k)
    {
        const StreetLine& line = net.lines[k];
        if (line.surface || line.pts.size() < 2)
            continue;
        const RoadProfile& prof = l.roads[size_t(line.profile)];
        auto set = std::find_if(sets.begin(), sets.end(), [&](const FurnitureSet& s) { return s.profile == prof.name; });
        if (set == sets.end() || set->rules.empty())
            continue;
        const RoadSection sec = roadSection(l, line);
        std::vector<glm::dvec3> pts = line.pts;
        const Vec2 f = planXZ(pts.front()), b = planXZ(pts.back());
        if (std::pair(std::llround(b.x), std::llround(b.y)) < std::pair(std::llround(f.x), std::llround(f.y)))
            std::reverse(pts.begin(), pts.end());
        std::vector<double> at = {0.0};
        for (size_t i = 0; i + 1 < pts.size(); ++i)
            at.push_back(at.back() + glm::length(planXZ(pts[i + 1]) - planXZ(pts[i])));
        const double length = at.back();
        auto sample = [&](double s, Vec2* p, Vec2* d) {
            size_t i = 0;
            while (i + 2 < at.size() && at[i + 1] < s)
                ++i;
            const double seg = std::max(at[i + 1] - at[i], 1e-9);
            *d = (planXZ(pts[i + 1]) - planXZ(pts[i])) / seg;
            *p = planXZ(pts[i]) + *d * (s - at[i]);
        };
        const uint64_t lineKey = furnitureMix(line.street, pointKey(planXZ(pts.front())));
        for (size_t r = 0; r < set->rules.size(); ++r)
        {
            const FurnitureRule& rule = set->rules[r];
            double offset = 0.0, up = 0.0;
            bool fits = true;
            if (rule.where == "median")
            {
                fits = sec.median > 0.0 && sec.median >= rule.minWidth;
                up = kCurb;
            }
            else if (rule.where == "verge")
            {
                fits = sec.verge >= rule.minWidth;
                offset = sec.verge > 0.0 ? sec.carriage + sec.sidewalk + 0.5 * sec.verge : sec.half() - 40.0;
                up = sec.sidewalk > 0.0 ? kCurb : 0.0;
            }
            else if ((rule.where == "kerb" || rule.where == "back") && sec.sidewalk > 0.0 && sec.carriage > 0.0)
            {
                fits = sec.sidewalk >= rule.minWidth;
                offset = rule.where == "kerb" ? sec.carriage + std::min(50.0, 0.5 * sec.sidewalk) : sec.carriage + std::max(0.5 * sec.sidewalk, sec.sidewalk - 50.0);
                up = kCurb;
            }
            else
            {
                // At the road's edge: kerbless streets, footpaths, and kerb rules on roads without a sidewalk.
                fits = rule.minWidth <= 0.0 || rule.where == "edge";
                offset = std::max(0.0, sec.half() - 40.0);
            }
            if (!fits || length < 0.5 * rule.spacing)
                continue;
            const int n = std::max(1, int(length / rule.spacing));
            const uint64_t ruleKey = furnitureMix(lineKey, textKey(rule.kind) + r);
            for (int slot = 0; slot < n; ++slot)
            {
                Vec2 p, d;
                sample((slot + 0.5) * length / n, &p, &d);
                std::vector<int> sides;
                if (rule.where == "median" || offset <= 0.0)
                    sides = {0};
                else if (rule.sides == "both")
                    sides = {1, -1};
                else if (rule.sides == "alternate")
                    sides = {slot % 2 == 0 ? 1 : -1};
                else
                    sides = {rule.sides == "right" ? 1 : -1};
                for (int side : sides)
                {
                    FurniturePlacement x;
                    x.id = furnitureMix(furnitureMix(ruleKey, uint64_t(slot)), uint64_t(side + 2)) & kStreetscapeIdMask;
                    x.kind = rule.kind;
                    const Vec2 q = p + kerbSide(d) * (offset * side);
                    x.p = groundAt(q, up);
                    x.heading = headingOf(side == 0 ? d : kerbSide(d) * double(-side));
                    x.street = line.street;
                    x.profile = prof.name;
                    out.push_back(std::move(x));
                }
            }
        }
    }

    // Junctions: signals and signs on each vehicle approach by the control, a name sign on one corner.
    for (size_t ji = 0; ji < net.junctions.size(); ++ji)
    {
        const Junction& j = net.junctions[ji];
        const uint64_t jKey = pointKey(planXZ(j.p));
        StreetClass top = StreetClass::Path;
        int named = -1;
        for (size_t e = 0; e < j.ends.size(); ++e)
            if (j.ends[e].vehicle && net.lines[size_t(j.ends[e].line)].cls < top)
            {
                top = net.lines[size_t(j.ends[e].line)].cls;
                named = int(e);
            }
        for (size_t e = 0; e < j.ends.size(); ++e)
        {
            const RoadEnd& end = j.ends[e];
            const StreetLine& line = net.lines[size_t(end.line)];
            const RoadSection sec = roadSection(l, line);
            Vec2 d = planXZ(end.out);
            d = glm::length(d) > 1e-9 ? d / glm::length(d) : Vec2(1.0, 0.0);
            // The incoming traffic's right-hand kerb, just back from the stop line.
            const double off = (sec.carriage > 0.0 ? sec.carriage : sec.half()) + 60.0;
            const Vec2 right = planXZ(end.p) - kerbSide(d) * off;
            auto add = [&](const char* kind, Vec2 q, Vec2 facing) {
                FurniturePlacement x;
                // A street running on through the junction comes in from both sides: its approach's own point tells them apart.
                x.id = furnitureMix(furnitureMix(furnitureMix(jKey, textKey(kind)), line.street), pointKey(planXZ(end.p))) & kStreetscapeIdMask;
                x.kind = kind;
                x.p = groundAt(q, sec.sidewalk > 0.0 && !sec.kerbless ? kCurb : 0.0);
                x.heading = headingOf(facing);
                x.junction = int(ji);
                x.profile = "junction";
                out.push_back(std::move(x));
            };
            if (int(e) == named)
                add(medieval ? "namePlaque" : "streetSign", planXZ(end.p) + kerbSide(d) * off, -kerbSide(d));
            if (!end.vehicle || medieval)
                continue;
            if (j.control == Control::Signal)
                add("signal", right, d);
            else if (j.control == Control::Stop)
                add("stopSign", right + d * 100.0, d);
            else if (j.control == Control::Yield && line.cls > top)
                add("yieldSign", right + d * 100.0, d);
        }
    }

    // Squares: a well and market stalls round a medieval square with a few carts; benches, trees and bollards on a
    // modern one.
    for (const Block& b : l.blocks)
    {
        if (b.use != Use::Plaza || b.outline.size() < 3)
            continue;
        const Vec2 c = centroid(b.outline);
        const uint64_t bKey = pointKey(c);
        int counter = 0;
        auto add = [&](const char* kind, Vec2 q, Vec2 facing, double up = 0.0) {
            FurniturePlacement x;
            x.id = furnitureMix(furnitureMix(bKey, textKey(kind)), uint64_t(counter++)) & kStreetscapeIdMask;
            x.kind = kind;
            x.p = groundAt(q, up);
            x.heading = headingOf(facing);
            x.block = b.id;
            x.profile = "plaza";
            out.push_back(std::move(x));
        };
        // The square's own frame: along its longest side.
        Vec2 u(1.0, 0.0);
        double longest = 0.0;
        for (size_t k = 0; k < b.outline.size(); ++k)
        {
            const Vec2 e = b.outline.pts[(k + 1) % b.outline.size()] - b.outline.pts[k];
            if (glm::length(e) > longest)
            {
                longest = glm::length(e);
                u = e / longest;
            }
        }
        const Vec2 v = kerbSide(u);
        double lo[2] = {1e300, 1e300}, hi[2] = {-1e300, -1e300};
        for (const Vec2& q : b.outline.pts)
            for (int a = 0; a < 2; ++a)
            {
                const double t = glm::dot(q - c, a == 0 ? u : v);
                lo[a] = std::min(lo[a], t);
                hi[a] = std::max(hi[a], t);
            }
        const bool wellFits = contains(b.outline.pts, c) && boundaryDistance(b.outline.pts, c) > 400.0;
        if (medieval)
        {
            if (wellFits)
                add("well", c, u);
            int slot = 0;
            for (double y = lo[1] + 400.0; y <= hi[1] - 400.0; y += 500.0)
                for (double x = lo[0] + 400.0; x <= hi[0] - 400.0; x += 600.0)
                {
                    const Vec2 q = c + u * x + v * y;
                    const double edge = boundaryDistance(b.outline.pts, q);
                    if (!contains(b.outline.pts, q) || edge < 400.0 || edge > 1400.0 || glm::length(q - c) < 600.0 || slot >= 48)
                        continue;
                    // Stalls face into the square; one slot in seven holds a cart instead.
                    add(slot % 7 == 3 ? "cart" : "marketStall", q, glm::length(c - q) > 1e-6 ? (c - q) / glm::length(c - q) : u);
                    ++slot;
                }
            continue;
        }
        for (double y = lo[1] + 600.0; y <= hi[1] - 600.0; y += 1200.0)
            for (double x = lo[0] + 600.0; x <= hi[0] - 600.0; x += 1200.0)
            {
                const Vec2 q = c + u * x + v * y;
                if (contains(b.outline.pts, q) && boundaryDistance(b.outline.pts, q) >= 500.0 && glm::length(q - c) > 300.0)
                    add("tree", q, u);
            }
        const size_t n = b.outline.size();
        for (size_t k = 0; k < n; ++k)
        {
            const Vec2 a = b.outline.pts[k], e = b.outline.pts[(k + 1) % n];
            const double len = glm::length(e - a);
            if (len < 1.0)
                continue;
            const Vec2 d = (e - a) / len, inward = kerbSide(d); // counter-clockwise outline: the inside is a quarter turn on
            for (double s = 400.0; s + 400.0 <= len; s += 800.0)
                add("bench", a + d * s + inward * 300.0, -inward);
            if (b.outline.tag[k] >= 0 && b.outline.tag[k] <= int(StreetClass::Collector))
                for (double s = 125.0; s + 125.0 <= len; s += 250.0)
                    add("bollard", a + d * s + inward * 50.0, -inward);
        }
    }
    return out;
}
} // namespace settlement
