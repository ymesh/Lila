// buildingCheck: modules intersecting, doors not reaching the ground, buildings overlapping their neighbours, slots
// floating off their building, real names on signs, and the interiors of enterable buildings (reachability, door
// openings, stairs and doors against the character profile).

#include "BuildingsScene.h"

#include <lila/Command.h>
#include <lila/Extension.h>
#include <lila/GeometryNodes.h>
#include <lila/Graph.h>
#include <lila/Names.h>
#include <lila/Node.h>
#include <lila/Profile.h>
#include <lila/Slots.h>

#include <lila/Deformer.h>

#include <array>
#include <cmath>

using namespace lila;
using lila::Args;
using lila::CommandError;
using T = lila::ArgType;

namespace buildings
{
namespace
{
enum class Attach
{
    Wall,
    Ground,
    Roof
};

Attach attachOf(const Module& m)
{
    if (m.kind == "tank" || m.kind == "vent" || m.kind == "antenna" || m.kind == "rebar" || (m.kind == "ac" && m.variant == 1))
        return Attach::Roof;
    if (m.kind == "perimeter" || m.kind == "gate" || m.kind == "pool" || m.kind == "column" || m.kind == "portico" || m.path.ends_with("/portico/floor"))
        return Attach::Ground;
    return Attach::Wall;
}

/** The module's box in its frame: x across, y up, z out of the wall (into it for openings set in reveals). */
void localBox(const Module& m, Vec3& lo, Vec3& hi)
{
    const double hw = m.size.x * 0.5;
    lo = {-hw, 0.0, 0.0};
    hi = {hw, m.size.y, m.size.z};
    if (m.kind == "window" || m.kind == "door" || m.kind == "rollup" || m.kind == "shopfront")
    {
        lo.z = -m.size.z * 0.5;
        hi.z = m.size.z * 0.5;
    }
    else if (attachOf(m) != Attach::Wall)
    {
        lo.z = -m.size.z * 0.5;
        hi.z = m.size.z * 0.5;
    }
    if (m.kind == "arcade")
    {
        lo = {-m.size.x, 0.0, -45.0};
        hi = {0.0, m.size.y, 0.0};
    }
}

struct Obb
{
    Vec3 c, ax[3];
    double e[3];
};

Obb obbOf(const Module& m, double shrink)
{
    Vec3 lo, hi;
    localBox(m, lo, hi);
    Obb o;
    const Vec3 mid = 0.5 * (lo + hi);
    o.c = m.origin + m.x * mid.x + m.y * mid.y + m.z * mid.z;
    o.ax[0] = m.x;
    o.ax[1] = m.y;
    o.ax[2] = m.z;
    for (int k = 0; k < 3; ++k)
        o.e[k] = std::max(0.0, 0.5 * (hi[k] - lo[k]) - shrink);
    return o;
}

/** Separating axis test between two oriented boxes. */
bool overlaps(const Obb& a, const Obb& b)
{
    std::array<Vec3, 15> axes;
    int n = 0;
    for (int i = 0; i < 3; ++i)
        axes[size_t(n++)] = a.ax[i];
    for (int i = 0; i < 3; ++i)
        axes[size_t(n++)] = b.ax[i];
    for (int i = 0; i < 3; ++i)
        for (int j = 0; j < 3; ++j)
        {
            const Vec3 c = glm::cross(a.ax[i], b.ax[j]);
            if (glm::length(c) > 1e-6)
                axes[size_t(n++)] = glm::normalize(c);
        }
    const Vec3 d = b.c - a.c;
    for (int k = 0; k < n; ++k)
    {
        const Vec3& L = axes[size_t(k)];
        double ra = 0.0, rb = 0.0;
        for (int i = 0; i < 3; ++i)
        {
            ra += a.e[i] * std::abs(glm::dot(a.ax[i], L));
            rb += b.e[i] * std::abs(glm::dot(b.ax[i], L));
        }
        if (std::abs(glm::dot(d, L)) > ra + rb)
            return false;
    }
    return true;
}

/** Pairs that sit together by design: a door in its shopfront, steps under a door, a shutter or grille over its opening. */
bool together(const Module& a, const Module& b)
{
    const std::string ba = a.path.substr(0, a.path.find_last_of('/')), bb = b.path.substr(0, b.path.find_last_of('/'));
    if (ba == bb)
        return true;
    auto is = [](const Module& m, const char* k) { return m.kind == k; };
    if ((is(a, "column") && is(b, "portico")) || (is(b, "column") && is(a, "portico")))
        return true;
    // Arcades round a corner share the corner pier.
    if (a.type == "arcade.bay" && b.type == "arcade.bay")
        return true;
    if ((is(a, "steps") || is(b, "steps")) && (a.path.find("/portico") != std::string::npos || b.path.find("/portico") != std::string::npos))
        return true;
    return false;
}

void addIssue(json& issues, json& counts, const std::string& kind, const std::string& building, const std::string& what, Vec3 at, const std::string& slot = {})
{
    counts[kind] = counts.value(kind, 0) + 1;
    if (issues.size() >= 500)
        return;
    json i = {{"kind", kind}, {"building", building}, {"what", what}, {"at", {at.x, at.y, at.z}}};
    if (!slot.empty())
        i["slot"] = slot;
    issues.push_back(std::move(i));
}
} // namespace

json checkBuildings(const std::vector<uint64_t>& modifiers, double tol, const json& profile)
{
    LILA_ZONE();
    scene::Graph& g = scene::Graph::current();
    g.evaluate();
    json issues = json::array(), counts = json::object();
    for (const char* k : {"modulesIntersecting", "doorNotReachingGround", "overlappingNeighbours", "slotFloating", "realName"})
        counts[k] = 0;
    struct Entry
    {
        const Spec* spec;
        const Result* r;
        Vec2 lo, hi;
    };
    std::vector<Entry> all;
    std::vector<std::shared_ptr<const Published>> keep;
    std::vector<names::Text> texts;
    std::vector<std::string> enterable;
    size_t buildings = 0;
    for (const auto& p : allPublished())
    {
        scene::Node* mod = g.node(scene::NodeId(p->modifier));
        if (!mod)
            continue;
        if (!modifiers.empty() && std::find(modifiers.begin(), modifiers.end(), p->modifier) == modifiers.end())
            continue;
        keep.push_back(p);
        for (size_t i = 0; i < p->results.size(); ++i)
        {
            const Spec& s = p->specs[i];
            const Result& r = *p->results[i];
            ++buildings;
            if (r.enterable)
                enterable.push_back(s.id);
            Vec2 lo(1e300), hi(-1e300);
            for (const Vec2& q : s.footprint)
            {
                lo = glm::min(lo, q);
                hi = glm::max(hi, q);
            }
            all.push_back({&s, &r, lo, hi});

            // Modules intersecting (instanced ones; the shell's own pieces are built to meet).
            std::vector<size_t> inst;
            for (size_t k = 0; k < r.modules.size(); ++k)
                if (!r.modules[k].shell && r.modules[k].kind != "perimeter" && r.modules[k].kind != "gate")
                    inst.push_back(k);
            std::vector<Obb> boxes;
            for (size_t k : inst)
                boxes.push_back(obbOf(r.modules[k], tol));
            for (size_t x = 0; x < inst.size(); ++x)
                for (size_t y = x + 1; y < inst.size(); ++y)
                {
                    const Module& a = r.modules[inst[x]];
                    const Module& b = r.modules[inst[y]];
                    if (glm::length(a.origin - b.origin) > glm::length(a.size) + glm::length(b.size))
                        continue;
                    if (together(a, b) || !overlaps(boxes[x], boxes[y]))
                        continue;
                    addIssue(issues, counts, "modulesIntersecting", s.id, a.type + " " + a.path + " meets " + b.type + " " + b.path, a.origin,
                             slots::makeId(s.id, a.path));
                }

            // Doors that don't reach the ground, slots off their building.
            double topY = -1e300, bottomTop = 1e300;
            for (const Facade& f : r.facades)
            {
                topY = std::max(topY, f.top);
                bottomTop = std::min(bottomTop, f.top);
            }
            for (const Module& m : r.modules)
            {
                if (m.shell)
                    continue;
                const std::string slot = slots::makeId(s.id, m.path);
                if ((m.kind == "door" || m.kind == "rollup") && m.type != "door.balcony" && m.path.find("/l0/") != std::string::npos)
                {
                    const Vec3 front = m.origin + m.z * 80.0;
                    const double ground = s.groundAt({front.x, front.z});
                    const double gap = m.origin.y - ground;
                    const std::string bay = m.path.substr(0, m.path.find_last_of('/'));
                    bool steps = false;
                    for (const Module& o : r.modules)
                        steps = steps || (o.kind == "steps" && o.path.rfind(bay, 0) == 0);
                    if (gap > 45.0 + tol && !steps)
                        addIssue(issues, counts, "doorNotReachingGround", s.id, "door sill " + std::to_string(int(gap)) + " cm above the ground, no steps", m.origin, slot);
                    else if (gap < -30.0 - tol)
                        addIssue(issues, counts, "doorNotReachingGround", s.id, "door sill " + std::to_string(int(-gap)) + " cm below the ground", m.origin, slot);
                }
                const Attach at = attachOf(m);
                bool floating = false;
                if (at == Attach::Wall)
                {
                    double best = 1e300;
                    for (const Facade& f : r.facades)
                    {
                        const Vec3 a3{f.a.x, 0.0, f.a.y}, n3{f.normal.x, 0.0, f.normal.y};
                        const Vec2 ab = f.b - f.a;
                        const double L = glm::length(ab);
                        const double sAlong = glm::dot(Vec2(m.origin.x, m.origin.z) - f.a, ab / std::max(L, 1e-9));
                        if (sAlong < -60.0 || sAlong > L + 60.0 || m.origin.y < f.bottom - 60.0 || m.origin.y > f.top + 60.0)
                            continue;
                        best = std::min(best, std::abs(glm::dot(m.origin - a3, n3)));
                    }
                    floating = best > 500.0 + tol;
                }
                else if (at == Attach::Roof)
                    floating = !inside(s.footprint, {m.origin.x, m.origin.z}) || m.origin.y > topY + 5.0 + tol || m.origin.y < bottomTop - 5.0 - tol;
                else
                {
                    const double ground = s.groundAt({m.origin.x, m.origin.z});
                    floating = std::abs(m.origin.y - ground) > 120.0 + tol && m.kind != "column" && m.kind != "portico";
                }
                if (floating)
                    addIssue(issues, counts, "slotFloating", s.id, m.type + " " + m.path + " is off its building", m.origin, slot);
                if (m.data.is_object() && m.data.contains("text") && m.data["text"].is_string() && !m.data["text"].get<std::string>().empty())
                    texts.push_back({slot, m.data["text"].get<std::string>()});
            }
            if (!s.name.empty())
                texts.push_back({s.id + ".name", s.name});
        }
    }

    // Overlapping neighbours: footprints sharing more than a sliver (party walls only touch).
    {
        LILA_ZONE_N("buildings.checkOverlap");
        std::vector<size_t> order(all.size());
        for (size_t k = 0; k < order.size(); ++k)
            order[k] = k;
        std::sort(order.begin(), order.end(), [&](size_t a, size_t b) { return all[a].lo.x < all[b].lo.x; });
        for (size_t x = 0; x < order.size(); ++x)
        {
            const Entry& a = all[order[x]];
            for (size_t y = x + 1; y < order.size(); ++y)
            {
                const Entry& b = all[order[y]];
                if (b.lo.x > a.hi.x)
                    break;
                if (b.lo.y > a.hi.y || b.hi.y < a.lo.y || a.spec->id == b.spec->id)
                    continue;
                const double area = overlapArea(a.spec->footprint, b.spec->footprint);
                const double smaller = std::min(std::abs(signedArea(a.spec->footprint)), std::abs(signedArea(b.spec->footprint)));
                if (area > std::max(10000.0, 0.02 * smaller))
                {
                    const Vec2 c = centroid(a.spec->footprint);
                    addIssue(issues, counts, "overlappingNeighbours", a.spec->id, "overlaps " + b.spec->id + " by " + std::to_string(int(area / 10000.0)) + " m2",
                             {c.x, a.r->wallTop, c.y});
                }
            }
        }
    }

    // Real names on signs and buildings.
    {
        const names::RealNames real = names::sceneRealNames(g);
        for (const names::Leak& l : names::findRealNames(texts, real, true))
            addIssue(issues, counts, "realName", l.where.substr(0, l.where.find('/', 9)), "'" + l.text + "' matches the real name '" + l.real + "' (" + l.source + ")",
                     Vec3(0.0), l.where);
    }
    // Interiors.
    std::sort(enterable.begin(), enterable.end());
    enterable.erase(std::unique(enterable.begin(), enterable.end()), enterable.end());
    if (!enterable.empty())
    {
        CharacterProfile prof = characterProfile();
        if (profile.is_object())
        {
            json j = prof.toJson();
            for (auto& [k, v] : profile.items())
                j[k] = v;
            prof = CharacterProfile::fromJson(j);
        }
        const json in = interiorIssues(enterable, prof);
        for (const json& i : in["issues"])
            issues.push_back(i);
        for (auto& [k, v] : in["counts"].items())
            counts[k] = v;
    }
    size_t total = 0;
    for (auto& [k, v] : counts.items())
        total += v.get<size_t>();
    return {{"ok", total == 0}, {"buildings", buildings}, {"interiors", enterable.size()}, {"counts", counts}, {"issues", issues}, {"texts", texts.size()}};
}

void registerCheck(ext::Context& ctx)
{
    ctx.registerCommand({
        .name = "buildingCheck",
        .doc = "Check buildings: modules intersecting (instanced parts meeting where they shouldn't), doors not reaching the ground "
               "(sill above it without steps, or buried), buildings overlapping their neighbours (party walls only touch), slots "
               "floating off their building, real names on signs or buildings (against imported real names and the brand "
               "blocklist), and for enterable buildings: rooms the entry can't reach, doors whose wall the room meshes close, stairs "
               "steeper or lower than the character profile, doors narrower or lower than the character, no exterior door into a "
               "room, and enterable buildings with no interior. height=, radius=, maxStep=, maxSlope= override the scene's profile "
               "(buildingCharacterProfile) for this check. Returns ok, counts by kind and issues (kind, building, what, at, slot).",
        .positionalDoc = "building meshes (default every one)",
        .flags = {{"tolerance", "tl", T::Float, 1.0, "cm of slack on every test.", {}, {}, 0.0},
                  {"height", "h", T::Float, nullptr, "Character height, cm.", {}, {}, 30.0, 1000.0},
                  {"radius", "r", T::Float, nullptr, "Character radius, cm.", {}, {}, 5.0, 300.0},
                  {"maxStep", "ms", T::Float, nullptr, "Highest step the character walks up, cm.", {}, {}, 1.0, 200.0},
                  {"maxSlope", "sl", T::Float, nullptr, "Steepest slope it walks, degrees.", {}, {}, 5.0, 85.0}},
        .undoable = false,
        .run = [](const Args& a) -> json {
            std::vector<uint64_t> mods;
            scene::Graph& g = scene::Graph::current();
            for (const json& p : a.positional)
            {
                if (!p.is_string())
                    continue;
                scene::Node* n = g.find(p.get<std::string>());
                if (!n)
                    throw CommandError("buildingCheck: no node " + p.get<std::string>());
                if (lila::gn::isGnModifier(*n))
                    mods.push_back(uint64_t(n->id()));
                for (scene::NodeId m : scene::modifiers::stack(g, n->id()))
                    mods.push_back(uint64_t(m));
            }
            json prof = json::object();
            for (const char* k : {"height", "radius", "maxStep", "maxSlope"})
                if (a.has(k))
                    prof[k] = a.get<double>(k);
            return checkBuildings(mods, a.get<double>("tolerance", 1.0), prof.empty() ? json() : prof);
        },
        .examples = {"buildingCheck()", "buildingCheck('Town_Buildings_0_0')['counts']"},
    });
}
} // namespace buildings
