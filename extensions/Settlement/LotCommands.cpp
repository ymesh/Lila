// settlementBlocks, settlementLots and settlementEnvelopes read a settlement's blocks, lots and envelopes;
// settlementSubdivide, settlementLandUse and settlementPlaceLandmark change them through the edits by stable id;
// settlementFoundations measures foundations on a Terrain; settlementBuildings picks the building route and builds it
// through Stagehand, Masonry, Seamwork or LilaLink, all called by name.

#include "Parcels.h"
#include "Settlement.h"
#include "SettlementTerrain.h"

#include <lila/Command.h>
#include <lila/Extension.h>
#include <lila/Graph.h>
#include <lila/Profile.h>

#include <algorithm>
#include <cmath>
#include <format>
#include <limits>
#include <set>

using namespace lila;

namespace settlement
{
namespace
{
using LotT = ArgType;

json xzJson(Vec2 p) { return json::array({p.x, p.y}); }

Rebuilt lotSettlementOf(const Args& a, const char* cmd)
{
    const std::vector<std::string> names = a.objects();
    if (names.size() != 1)
        throw CommandError(std::string(cmd) + ": give one settlement");
    return rebuildSettlement(scene::Graph::current(), names[0], cmd);
}

json lotEditsOf(const Rebuilt& r, const char* cmd)
{
    const json text = ops::execute("getAttr", {{"args", {r.modifier + ".edits"}}});
    try
    {
        return parseEdits(text.is_string() ? text.get<std::string>() : std::string());
    }
    catch (const std::exception& e)
    {
        throw CommandError(std::string(cmd) + ": " + e.what());
    }
}

void writeEdits(const Rebuilt& r, const json& edits) { ops::execute("setAttr", {{"args", {r.modifier + ".edits", edits.dump()}}}); }

/** Ids from a flag that takes one id or a list of them. */
std::vector<uint64_t> idList(const Args& a, const char* flag, const char* cmd)
{
    std::vector<uint64_t> out;
    if (!a.has(flag))
        return out;
    const json& v = a.flags[flag];
    for (const json& x : v.is_array() ? v : json::array({v}))
    {
        if (!x.is_number())
            throw CommandError(std::string(cmd) + ": " + flag + " takes ids (numbers)");
        out.push_back(uint64_t(std::llround(x.get<double>())));
    }
    return out;
}

Vec2 pointFlag(const Args& a, const char* flag, const char* cmd)
{
    const json& p = a.flags[flag];
    if (!p.is_array() || (p.size() != 2 && p.size() != 3))
        throw CommandError(std::string(cmd) + ": " + flag + " is [x, z] or [x, y, z]");
    return {p[0].get<double>(), p[p.size() - 1].get<double>()};
}

/** The blocks or lots named by id= or found under at=. */
std::vector<uint64_t> blockTargets(const Args& a, const Layout& l, const char* cmd)
{
    std::vector<uint64_t> ids = idList(a, "block", cmd);
    if (a.has("at"))
    {
        const Vec2 q = pointFlag(a, "at", cmd);
        auto it = std::ranges::find_if(l.blocks, [&](const Block& b) { return contains(b.outline.pts, q); });
        if (it == l.blocks.end())
            throw CommandError(std::string(cmd) + ": no block at [" + std::to_string(q.x) + ", " + std::to_string(q.y) + "]");
        ids.push_back(it->key);
    }
    for (uint64_t id : ids)
        if (std::ranges::none_of(l.blocks, [&](const Block& b) { return b.key == id; }))
            throw CommandError(std::string(cmd) + ": no block " + std::to_string(id) + " (settlementBlocks lists them)");
    if (ids.empty())
        throw CommandError(std::string(cmd) + ": give block= or at=");
    return ids;
}

/** The lots named by lot= and at=. With snapped, an at= point on no lot takes the nearest lot within kSnapReach and
 *  says so in *snapped; otherwise a miss names the nearest lot. */
std::vector<uint64_t> lotTargets(const Args& a, const Layout& l, const char* cmd, json* snapped = nullptr)
{
    constexpr double kSnapReach = 10000.0;
    std::vector<uint64_t> ids = idList(a, "lot", cmd);
    if (a.has("at"))
    {
        const Vec2 q = pointFlag(a, "at", cmd);
        auto it = std::ranges::find_if(l.lots, [&](const Lot& lot) { return contains(lot.outline.pts, q); });
        if (it == l.lots.end())
        {
            const Lot* nearest = nullptr;
            double best = std::numeric_limits<double>::max();
            for (const Lot& lot : l.lots)
                if (const double d = boundaryDistance(lot.outline.pts, q); d < best)
                {
                    best = d;
                    nearest = &lot;
                }
            const std::string where = "no lot at [" + std::to_string(q.x) + ", " + std::to_string(q.y) + "]";
            if (!nearest)
                throw CommandError(std::string(cmd) + ": " + where + ": the settlement has no lots");
            if (!snapped || best > kSnapReach)
                throw CommandError(std::format("{}: {}; the nearest is lotId {} ({:.0f} m away, center [{:.0f}, {:.0f}])", cmd, where, nearest->key,
                                               best / 100.0, centroid(nearest->outline).x, centroid(nearest->outline).y));
            *snapped = {{"from", xzJson(q)}, {"lotId", nearest->key}, {"distanceM", best / 100.0}};
            ids.push_back(nearest->key);
        }
        else
            ids.push_back(it->key);
    }
    for (uint64_t id : ids)
        if (std::ranges::none_of(l.lots, [&](const Lot& lot) { return lot.key == id; }))
            throw CommandError(std::string(cmd) + ": no lot " + std::to_string(id) + " (settlementLots lists them)");
    return ids;
}

json lotSummary(const Layout& l, const Lot& lot)
{
    json j = {{"lot", lot.id},
              {"block", l.blocks[size_t(lot.block)].key},
              {"use", useName(lot.use)},
              {"areaM2", lot.area},
              {"frontages", lot.frontages},
              {"corner", lot.corner},
              {"frontClass", className(lot.frontClass)},
              {"center", xzJson(centroid(lot.outline))}};
    j.update(lotExtras(lot));
    return j;
}

json envelopeSummary(const Envelope& env)
{
    json pts = json::array();
    for (const Vec2& q : env.footprint.pts)
        pts.push_back(xzJson(q));
    json j = {{"lot", env.lot},       {"use", useName(env.use)}, {"footprint", pts},  {"floors", env.floors},
              {"height", env.height}, {"base", env.base},        {"ground", env.ground}, {"top", env.top},
              {"roof", env.roof},     {"front", xzJson(env.front)}};
    j.update(envelopeExtras(env));
    return j;
}

json& entryFor(json& list, const char* key, uint64_t id)
{
    for (json& e : list)
        if (e.value(key, uint64_t(0)) == id)
            return e;
    list.push_back({{key, id}});
    return list.back();
}

/** A command another extension registers, or a CommandError naming it and the extension. */
void lotNeedCommand(const char* name, const char* extension, const char* cmd)
{
    if (!ops::find(name))
        throw CommandError(std::string(cmd) + ": " + name + " is not available; load the " + extension + " extension");
}

std::string objectName(const Envelope& env) { return env.landmark.empty() ? "lot_" + std::to_string(env.lot) : env.landmark; }

/** Library assets fitted to each envelope by footprint, tags and era, placed on it turned to the street. */
json placeLibrary(const std::vector<const Envelope*>& envs, const Args& a, std::vector<std::string>& made, json& picks, const char* cmd)
{
    lotNeedCommand("librarySearch", "Stagehand", cmd);
    lotNeedCommand("place", "Stagehand", cmd);
    json query = {{"limit", 0}};
    query["tags"] = a.has("tags") ? a.flags["tags"] : json::array({"building"});
    if (a.has("collection"))
        query["collection"] = a.flags["collection"];
    const json assets = ops::execute("librarySearch", query);
    const std::string frontAxis = a.get<std::string>("assetFront", "+z");
    const double fit = std::max(0.5, a.get<double>("fit", 1.05));
    const double assetHeading = frontAxis == "+z" ? 0.0 : frontAxis == "-z" ? 180.0 : frontAxis == "+x" ? 90.0 : -90.0;
    const bool sideways = frontAxis == "+x" || frontAxis == "-x";
    json placed = json::array(), unfitted = json::array();
    for (const Envelope* env : envs)
    {
        double w = 0.0, d = 0.0;
        Vec2 c{0.0};
        frontageFit(*env, w, d, c);
        const json* best = nullptr;
        double bestScore = -1.0;
        for (const json& asset : assets)
        {
            const json size = asset.value("size", json::array());
            if (size.size() != 3)
                continue;
            const double aw = size[sideways ? 2 : 0].get<double>(), ad = size[sideways ? 0 : 2].get<double>();
            if (aw <= 0.0 || ad <= 0.0 || aw > w * fit || ad > d * fit)
                continue;
            double score = aw * ad / std::max(w * d, 1.0);
            for (const json& t : asset.value("tags", json::array()))
                if (t.is_string() && (t.get<std::string>() == env->eraName || t.get<std::string>() == useName(env->use)))
                    score += 0.1;
            score += 0.01 * unitRandom(hash64(env->key) ^ hash64(std::hash<std::string>{}(asset.value("id", std::string()))));
            if (score > bestScore)
            {
                bestScore = score;
                best = &asset;
            }
        }
        if (!best)
        {
            unfitted.push_back({{"lot", env->lot}, {"lotId", env->key}, {"width", w}, {"depth", d}});
            continue;
        }
        picks[std::to_string(env->key)] = {{"asset", best->value("id", std::string())}, {"unreal", best->value("unreal", std::string())},
                                           {"assetHeading", assetHeading}};
        const double heading = std::atan2(env->front.x, env->front.y) * 180.0 / kPi;
        const json res = ops::execute("place", {{"args", {best->value("id", std::string())}},
                                                {"at", json::array({c.x, env->ground, c.y})},
                                                {"snap", false},
                                                {"rotate", heading - assetHeading},
                                                {"name", objectName(*env)}});
        for (const json& p : res.is_array() ? res : json::array({res}))
            if (p.is_object() && p.contains("node"))
            {
                made.push_back(p["node"].get<std::string>());
                placed.push_back({{"lot", env->lot}, {"asset", best->value("id", std::string())}, {"node", p["node"]}});
            }
    }
    return {{"placed", placed},
            {"unfitted", unfitted},
            {"candidates", assets.size()},
            {"note", unfitted.empty() ? "" : "no library asset fits these lots' footprints; add assets or give them route massing"}};
}

/** A Masonry building per envelope: walls round the footprint on the ground, a door at the main entrance, windows on the street. */
json buildMasonry(const Rebuilt& r, const std::vector<const Envelope*>& envs, std::vector<std::string>& made, const char* cmd)
{
    lotNeedCommand("masonryBuilding", "Masonry", cmd);
    json built = json::array();
    for (const Envelope* env : envs)
    {
        const Polygon& fp = env->footprint;
        json pts = json::array();
        std::vector<double> at = {0.0};
        for (size_t k = 0; k < fp.size(); ++k)
        {
            pts.push_back(json::array({fp.pts[k].x, r.ground.height(fp.pts[k]), fp.pts[k].y}));
            at.push_back(at.back() + glm::length(fp.pts[(k + 1) % fp.size()] - fp.pts[k]));
        }
        json openings = json::array();
        int windows = 0;
        bool door = false;
        for (size_t k = 0; k < fp.size(); ++k)
        {
            const double len = at[k + 1] - at[k];
            if (fp.tag[k] != 0 || len < 250.0)
                continue;
            const Vec2 mid = (fp.pts[k] + fp.pts[(k + 1) % fp.size()]) * 0.5;
            if (!door && !env->entrances.empty() && glm::length(mid - env->entrances.front()) < 1.0)
            {
                openings.push_back({{"name", "door_1"}, {"kind", "door"}, {"at", at[k] + 0.5 * len}, {"width", 160}, {"height", 280}});
                door = true;
            }
            for (double s = 200.0; s + 200.0 < len; s += 400.0)
                if (std::abs(s - 0.5 * len) > 200.0 || !door)
                    openings.push_back({{"name", "window_" + std::to_string(++windows)}, {"kind", "window"}, {"at", at[k] + s},
                                        {"width", 90}, {"height", 140}, {"sill", 120}});
        }
        const json spec = {{"seed", int64_t(env->key % 100000)},
                           {"bond", "ashlar"},
                           {"stone", {{"length", 60}, {"height", 30}, {"depth", 40}, {"joint", 1}}},
                           {"walls", json::array({{{"name", "walls"},
                                                   {"points", pts},
                                                   {"closed", true},
                                                   {"height", std::min(env->height, 1500.0)},
                                                   {"thickness", 60},
                                                   {"openings", openings}}})}};
        const json res = ops::execute("masonryBuilding", {{"spec", spec}, {"name", objectName(*env)}});
        const std::string mesh = res.value("mesh", std::string());
        if (!mesh.empty())
            made.push_back(mesh);
        built.push_back({{"lot", env->lot}, {"landmark", env->landmark}, {"mesh", mesh}, {"stats", res.value("stats", json::object())}});
    }
    return {{"built", built}};
}

/** The meshes at or under each object: placed library assets are transforms over their meshes. */
json meshesUnder(const std::vector<std::string>& objects)
{
    scene::Graph& g = scene::Graph::current();
    json out = json::array();
    std::vector<scene::NodeId> todo;
    for (const std::string& o : objects)
        if (scene::Node* n = g.find(o))
            todo.push_back(n->id());
    while (!todo.empty())
    {
        const scene::NodeId id = todo.back();
        todo.pop_back();
        scene::Node* n = g.node(id);
        if (!n)
            continue;
        if (n->type().isA("mesh"))
            out.push_back(n->path());
        for (scene::NodeId c : g.children(id))
            todo.push_back(c);
    }
    return out;
}

/** Contact skirts, grime strips or scatter where the buildings meet the ground, through Seamwork. */
json contactsFor(const Rebuilt& r, const std::vector<std::string>& objects, const json& kinds, const char* cmd)
{
    lotNeedCommand("seamFind", "Seamwork", cmd);
    const json meshes = meshesUnder(objects);
    if (meshes.empty())
        throw CommandError(std::string(cmd) + ": the placed buildings have no meshes to give contacts");
    // Only where the buildings meet the ground: joins between them and the streets or each other get no skirt.
    json findArgs = {{"args", meshes}, {"findMesh", false}};
    const std::string ground = r.values.value("ground", std::string());
    if (ground.empty())
        throw CommandError(std::string(cmd) + ": contacts need the ground the buildings stand on: give the settlement a ground mesh or Terrain");
    // A Terrain takes the contact mask; any other ground mesh only acts as the ground.
    if (isTerrainGround(ground))
        findArgs["terrain"] = ground;
    else
    {
        findArgs["ground"] = json::array({ground});
        findArgs["terrainMask"] = false;
    }
    const json found = ops::execute("seamFind", findArgs);
    const std::string seam = found.value("seam", std::string());
    if (seam.empty())
        throw CommandError(std::string(cmd) + ": seamFind made no seam node");
    json out = {{"seam", seam}};
    for (const json& k : kinds)
    {
        const std::string kind = k.is_string() ? k.get<std::string>() : std::string();
        const char* name = kind == "skirt" ? "seamSkirt" : kind == "decal" ? "seamDecal" : kind == "scatter" ? "seamScatter" : nullptr;
        if (!name)
            throw CommandError(std::string(cmd) + ": unknown contact kind '" + kind + "' (skirt, decal, scatter)");
        lotNeedCommand(name, "Seamwork", cmd);
        out[kind] = ops::execute(name, {{"args", {seam}}});
    }
    return out;
}
} // namespace

void registerLotCommands(ext::Context& ctx)
{
    std::vector<std::string> types, methods, routes, uses;
    for (int t = 0; t <= int(BlockType::Field); ++t)
        types.emplace_back(blockTypeName(BlockType(t)));
    for (int m = 0; m <= int(LotMethod::Whole); ++m)
        methods.emplace_back(lotMethodName(LotMethod(m)));
    for (int r = 0; r <= int(BuildingRoute::Unreal); ++r)
        routes.emplace_back(routeName(BuildingRoute(r)));
    for (int u = 0; u <= int(Use::Cemetery); ++u)
        uses.emplace_back(useName(Use(u)));

    ctx.registerCommand({
        .name = "settlementBlocks",
        .doc = "A settlement's blocks: stable id, type (perimeter, courtyard, open, row, superblock, park, plaza, market, "
               "churchyard, field), lot method, area, centre, courtyards and why special blocks are what they are, with counts "
               "of block types, lot methods, uses and the lot area distribution.",
        .positionalDoc = "settlement mesh or its modifier",
        .flags = {{"type", "t", LotT::String, nullptr, "Only blocks of this type.", types},
                  {"limit", "l", LotT::Int, int64_t(0), "At most this many; 0 for all.", {}, {}, 0.0}},
        .undoable = false,
        .run = [](const Args& a) -> json {
            LILA_ZONE();

            const Rebuilt r = lotSettlementOf(a, "settlementBlocks");
            const std::string type = a.get<std::string>("type", "");
            const int64_t limit = a.get<int64_t>("limit", 0);
            json blocks = json::array();
            for (const Block& b : r.layout.blocks)
            {
                if (!type.empty() && type != blockTypeName(b.type))
                    continue;
                json j = blockJson(b);
                j["lots"] = std::ranges::count_if(r.layout.lots, [&](const Lot& lot) { return lot.block == b.id; });
                blocks.push_back(std::move(j));
                if (limit > 0 && int64_t(blocks.size()) >= limit)
                    break;
            }
            return {{"blocks", blocks}, {"stats", parcelStats(r.layout)}};
        },
        .examples = {"settlementBlocks('Settlement', type='courtyard')['blocks'][0]['blockId']"},
    });

    ctx.registerCommand({
        .name = "settlementSubdivide",
        .doc = "Set a block's type or lot method by its stable id (or the block under at=), kept in the settlement's edits so "
               "it holds across re-runs while the block's streets stay. Methods: frontage, obb (recursive oriented-box splits), "
               "skeleton (straight-skeleton strips off each street), burgage (narrow deep plots behind the main street), voronoi "
               "(irregular cells), strips (farm strips), whole (one lot). Every method leaves each lot on a street; lots that "
               "would reach none join a neighbour. clear=True drops the block's override. Returns the blocks as rebuilt.",
        .positionalDoc = "settlement mesh or its modifier",
        .flags = {{"block", "b", LotT::Any, nullptr, "Block id, or a list of them (settlementBlocks lists them)."},
                  {"at", "", LotT::List, nullptr, "Or the block under this point, [x, z]."},
                  {"type", "t", LotT::String, nullptr, "Block type.", types},
                  {"method", "m", LotT::String, nullptr, "Lot method.", methods},
                  {"clear", "cl", LotT::Bool, false, "Drop the override instead."}},
        .run = [](const Args& a) -> json {
            LILA_ZONE();

            const char* cmd = "settlementSubdivide";
            const Rebuilt r = lotSettlementOf(a, cmd);
            const std::vector<uint64_t> ids = blockTargets(a, r.layout, cmd);
            const bool clear = a.get<bool>("clear", false);
            if (!clear && !a.has("type") && !a.has("method"))
                throw CommandError(std::string(cmd) + ": give type=, method= or clear=True");
            BlockType t;
            LotMethod m;
            if (a.has("type") && !parseBlockType(a.get<std::string>("type"), &t))
                throw CommandError(std::string(cmd) + ": unknown type '" + a.get<std::string>("type") + "'");
            if (a.has("method") && !parseLotMethod(a.get<std::string>("method"), &m))
                throw CommandError(std::string(cmd) + ": unknown method '" + a.get<std::string>("method") + "'");
            json edits = lotEditsOf(r, cmd);
            if (!edits.contains("blockOverrides"))
                edits["blockOverrides"] = json::array();
            for (uint64_t id : ids)
            {
                if (clear)
                {
                    json& list = edits["blockOverrides"];
                    for (size_t k = list.size(); k-- > 0;)
                        if (list[k].value("block", uint64_t(0)) == id)
                            list.erase(k);
                    continue;
                }
                json& e = entryFor(edits["blockOverrides"], "block", id);
                for (const char* key : {"type", "method"})
                    if (a.has(key))
                        e[key] = a.get<std::string>(key);
            }
            writeEdits(r, edits);
            const Rebuilt after = rebuildSettlement(scene::Graph::current(), r.modifier, cmd);
            json blocks = json::array();
            for (const Block& b : after.layout.blocks)
                if (std::ranges::find(ids, b.key) != ids.end())
                {
                    json j = blockJson(b);
                    j["lots"] = std::ranges::count_if(after.layout.lots, [&](const Lot& lot) { return lot.block == b.id; });
                    blocks.push_back(std::move(j));
                }
            return {{"blocks", blocks}, {"stats", parcelStats(after.layout)}};
        },
        .examples = {"b = settlementBlocks('Settlement')['blocks'][0]['blockId']\nsettlementSubdivide('Settlement', block=b, type='courtyard', method='skeleton')",
                     "settlementSubdivide('Settlement', at=[1200, -800], method='voronoi')"},
    });

    ctx.registerCommand({
        .name = "settlementLots",
        .doc = "A settlement's lots: stable id, block, use, residential wealth tier, area, street sides, corner, street access "
               "(0 to 1, integration and betweenness of the streets it faces), landmark name and the reasons it is there, "
               "building route. Filter by use, block or landmarks only. stats holds the lot area distribution and use counts.",
        .positionalDoc = "settlement mesh or its modifier",
        .flags = {{"use", "u", LotT::String, nullptr, "Only lots of this use.", uses},
                  {"block", "b", LotT::Any, nullptr, "Only lots in this block (its id)."},
                  {"landmarks", "lm", LotT::Bool, false, "Only landmark lots."},
                  {"limit", "l", LotT::Int, int64_t(500), "At most this many; 0 for all.", {}, {}, 0.0}},
        .undoable = false,
        .run = [](const Args& a) -> json {
            LILA_ZONE();

            const Rebuilt r = lotSettlementOf(a, "settlementLots");
            const std::string use = a.get<std::string>("use", "");
            const std::vector<uint64_t> blocks = idList(a, "block", "settlementLots");
            const bool landmarks = a.get<bool>("landmarks", false);
            const int64_t limit = a.get<int64_t>("limit", 500);
            json lots = json::array();
            for (const Lot& lot : r.layout.lots)
            {
                if ((!use.empty() && use != useName(lot.use)) || (landmarks && lot.landmark.empty()) ||
                    (!blocks.empty() && std::ranges::find(blocks, r.layout.blocks[size_t(lot.block)].key) == blocks.end()))
                    continue;
                lots.push_back(lotSummary(r.layout, lot));
                if (limit > 0 && int64_t(lots.size()) >= limit)
                    break;
            }
            return {{"lots", lots}, {"count", lots.size()}, {"stats", parcelStats(r.layout)}};
        },
        .examples = {"settlementLots('Settlement', landmarks=True)", "settlementLots('Settlement', use='commercial', limit=20)"},
    });

    ctx.registerCommand({
        .name = "settlementLandUse",
        .doc = "Land use: what each use covers and the rules behind it, or (lot= or at= with use=) set a lot's use by its stable "
               "id, kept in the edits. Uses come from rules: street access (integration and betweenness), distance to the "
               "centre and the water, flood ground and slope (parks), the prevailing wind and the river's flow (industry "
               "downwind and downstream), fields outside pre-modern towns, a market facing the square, commerce on the most "
               "central streets and residential wealth tiers by land value. clear=True drops the lot's use override.",
        .positionalDoc = "settlement mesh or its modifier",
        .flags = {{"lot", "lt", LotT::Any, nullptr, "Lot id, or a list of them."},
                  {"at", "", LotT::List, nullptr, "Or the lot under this point, [x, z], or the nearest one (returned as snapped)."},
                  {"use", "u", LotT::String, nullptr, "The use to give them.", uses},
                  {"clear", "cl", LotT::Bool, false, "Drop their use overrides."}},
        .run = [](const Args& a) -> json {
            LILA_ZONE();

            const char* cmd = "settlementLandUse";
            const Rebuilt r = lotSettlementOf(a, cmd);
            json snapped;
            const std::vector<uint64_t> ids = lotTargets(a, r.layout, cmd, &snapped);
            const Layout* l = &r.layout;
            Rebuilt after;
            if (!ids.empty())
            {
                const bool clear = a.get<bool>("clear", false);
                Use u;
                if (!clear && (!a.has("use") || !parseUse(a.get<std::string>("use"), &u)))
                    throw CommandError(std::string(cmd) + ": give use= (one of the uses) or clear=True");
                json edits = lotEditsOf(r, cmd);
                if (!edits.contains("lotOverrides"))
                    edits["lotOverrides"] = json::array();
                for (uint64_t id : ids)
                {
                    json& e = entryFor(edits["lotOverrides"], "lot", id);
                    if (clear)
                        e.erase("use");
                    else
                        e["use"] = a.get<std::string>("use");
                }
                writeEdits(r, edits);
                after = rebuildSettlement(scene::Graph::current(), r.modifier, cmd);
                l = &after.layout;
            }
            json changed = json::array();
            for (const Lot& lot : l->lots)
                if (std::ranges::find(ids, lot.key) != ids.end())
                    changed.push_back(lotSummary(*l, lot));
            std::vector<double> access;
            for (const Lot& lot : l->lots)
                if (lot.frontages > 0)
                    access.push_back(lot.access);
            std::sort(access.begin(), access.end());
            auto q = [&](double f) { return access.empty() ? 0.0 : access[std::min(access.size() - 1, size_t(f * double(access.size())))]; };
            const json stats = parcelStats(*l);
            json out = {{"lots", changed},
                        {"uses", stats["lotUses"]},
                        {"residentialWealth", stats["residentialWealth"]},
                        {"access", {{"p10", q(0.1)}, {"p50", q(0.5)}, {"p90", q(0.9)}}},
                        {"era", eraOf(l->profile)},
                        {"windAngle", l->profile.windAngle},
                        {"landmarks", stats["landmarks"]}};
            if (!snapped.is_null())
                out["snapped"] = snapped;
            return out;
        },
        .examples = {"settlementLandUse('Settlement')['uses']", "settlementLandUse('Settlement', at=[2400, 600], use='industrial')"},
    });

    ctx.registerCommand({
        .name = "settlementPlaceLandmark",
        .doc = "Reserve a lot as a landmark under a name (a footprint to fill from the library or Masonry), by stable id or the "
               "lot under at= (or the nearest within 100 m, returned as snapped), kept in the edits; use= changes its use too. remove=True takes the name off again. Generated "
               "landmarks (market_square, church, town_hall, castle or temple on high ground, axis_end_N, park_N, cemetery) "
               "carry the reasons they are where they are; settlementLots(landmarks=True) lists them all.",
        .positionalDoc = "settlement mesh or its modifier",
        .flags = {{"lot", "lt", LotT::Any, nullptr, "Lot id."},
                  {"at", "", LotT::List, nullptr, "Or the lot under this point, [x, z], or the nearest one."},
                  {"name", "n", LotT::String, nullptr, "Landmark name, such as guild_hall."},
                  {"use", "u", LotT::String, nullptr, "Its use.", uses},
                  {"remove", "rm", LotT::Bool, false, "Take the landmark name off."}},
        .run = [](const Args& a) -> json {
            LILA_ZONE();

            const char* cmd = "settlementPlaceLandmark";
            const Rebuilt r = lotSettlementOf(a, cmd);
            json snapped;
            const std::vector<uint64_t> ids = lotTargets(a, r.layout, cmd, &snapped);
            if (ids.size() != 1)
                throw CommandError(std::string(cmd) + ": give one lot (lot= or at=)");
            const bool remove = a.get<bool>("remove", false);
            if (!remove && a.get<std::string>("name", "").empty())
                throw CommandError(std::string(cmd) + ": give name=");
            Use u;
            if (a.has("use") && !parseUse(a.get<std::string>("use"), &u))
                throw CommandError(std::string(cmd) + ": unknown use '" + a.get<std::string>("use") + "'");
            json edits = lotEditsOf(r, cmd);
            if (!edits.contains("lotOverrides"))
                edits["lotOverrides"] = json::array();
            json& e = entryFor(edits["lotOverrides"], "lot", ids[0]);
            if (remove)
                e.erase("landmark");
            else
                e["landmark"] = a.get<std::string>("name");
            if (a.has("use"))
                e["use"] = a.get<std::string>("use");
            writeEdits(r, edits);
            const Rebuilt after = rebuildSettlement(scene::Graph::current(), r.modifier, cmd);
            for (const Lot& lot : after.layout.lots)
                if (lot.key == ids[0])
                {
                    json out = lotSummary(after.layout, lot);
                    if (!snapped.is_null())
                        out["snapped"] = snapped;
                    return out;
                }
            throw CommandError(std::string(cmd) + ": the lot is gone after the rebuild");
        },
        .examples = {"settlementPlaceLandmark('Settlement', at=[300, 4200], name='guild_hall', use='civic')"},
    });

    ctx.registerCommand({
        .name = "settlementEnvelopes",
        .doc = "Building envelopes in the published envelope schema: footprint and sides (street, party, open) with the lot "
               "across each party wall, setbacks, tiers, floors and heights, roof kind, ridge and height, entrances on the "
               "street, ground-floor use, corner treatment, foundation depth and its source (terrainRegionStats or the "
               "ground sampled under the footprint), landmark, era, wealth and building route. Filter by lot, use or landmarks.",
        .positionalDoc = "settlement mesh or its modifier",
        .flags = {{"lot", "lt", LotT::Any, nullptr, "Only these lot ids."},
                  {"use", "u", LotT::String, nullptr, "Only this use.", uses},
                  {"landmarks", "lm", LotT::Bool, false, "Only landmark envelopes."},
                  {"limit", "l", LotT::Int, int64_t(200), "At most this many; 0 for all.", {}, {}, 0.0}},
        .undoable = false,
        .run = [](const Args& a) -> json {
            LILA_ZONE();

            const Rebuilt r = lotSettlementOf(a, "settlementEnvelopes");
            const std::vector<uint64_t> ids = idList(a, "lot", "settlementEnvelopes");
            const std::string use = a.get<std::string>("use", "");
            const bool landmarks = a.get<bool>("landmarks", false);
            const int64_t limit = a.get<int64_t>("limit", 200);
            json out = json::array(), sources = json::object();
            for (const Envelope& env : r.layout.envelopes)
                sources[env.foundationSource] = sources.value(env.foundationSource, 0) + 1;
            for (const Envelope& env : r.layout.envelopes)
            {
                if ((!ids.empty() && std::ranges::find(ids, env.key) == ids.end()) || (!use.empty() && use != useName(env.use)) ||
                    (landmarks && env.landmark.empty()))
                    continue;
                out.push_back(envelopeSummary(env));
                if (limit > 0 && int64_t(out.size()) >= limit)
                    break;
            }
            const json stats = parcelStats(r.layout);
            json result = {{"envelopes", out},
                           {"count", r.layout.envelopes.size()},
                           {"roofs", stats["roofs"]},
                           {"partyWallSides", stats["partyWallSides"]},
                           {"foundationSources", sources}};
            if (!r.ground.isPlane() && !sources.contains("terrainRegionStats"))
                result["warning"] = "foundations come from the ground sampled under each footprint; on a Terrain, settlementFoundations "
                                    "measures them with terrainRegionStats";
            return result;
        },
        .examples = {"e = settlementEnvelopes('Settlement', landmarks=True)['envelopes'][0]\ne['roof'], e['entrances'], e['foundationDepth']"},
    });

    ctx.registerCommand({
        .name = "settlementFoundations",
        .doc = "Measure each envelope's foundation on a Terrain with terrainRegionStats (the lowest and highest ground under its "
               "footprint) and keep the numbers in the settlement's edits by lot id, so envelopes take their base and "
               "foundation depth from them. terrain= defaults to the settlement's ground. clear=True drops them, going back to "
               "the ground sampled under each footprint.",
        .positionalDoc = "settlement mesh or its modifier",
        .flags = {{"terrain", "t", LotT::String, nullptr, "The Terrain mesh; default the settlement's ground."},
                  {"clear", "cl", LotT::Bool, false, "Drop the measured foundations."}},
        .run = [](const Args& a) -> json {
            LILA_ZONE();

            const char* cmd = "settlementFoundations";
            const Rebuilt r = lotSettlementOf(a, cmd);
            json edits = lotEditsOf(r, cmd);
            if (a.get<bool>("clear", false))
            {
                edits.erase("foundations");
                writeEdits(r, edits);
                return {{"measured", 0}};
            }
            const std::string terrain = a.get<std::string>("terrain", r.values.value("ground", std::string()));
            if (terrain.empty())
                throw CommandError(std::string(cmd) + ": the settlement stands on flat ground; give terrain=");
            lotNeedCommand("terrainRegionStats", "Terrain", cmd);
            json found = json::object();
            double deepest = 0.0;
            for (const Envelope& env : r.layout.envelopes)
            {
                json ring = json::array();
                for (const Vec2& q : env.footprint.pts)
                    ring.push_back(xzJson(q));
                json s;
                try
                {
                    s = ops::execute("terrainRegionStats", {{"args", {terrain}}, {"polygon", ring}, {"fields", json::array({"height"})}});
                }
                catch (const std::exception& e)
                {
                    throw CommandError(std::string(cmd) + ": " + e.what());
                }
                const json h = s.value("fields", json::object()).value("height", json::object());
                if (!h.contains("min") || s.value("samples", 0) == 0)
                    continue;
                const double lo = h["min"].get<double>(), hi = h["max"].get<double>();
                found[std::to_string(env.key)] = {{"min", lo}, {"max", hi}, {"revision", s.value("revision", json())}};
                deepest = std::max(deepest, hi - lo);
            }
            edits["foundations"] = found;
            writeEdits(r, edits);
            return {{"measured", found.size()}, {"envelopes", r.layout.envelopes.size()}, {"terrain", terrain}, {"largestStepCm", deepest}};
        },
        .examples = {"settlementFoundations('Settlement', terrain='Terrain')"},
    });

    ctx.registerCommand({
        .name = "settlementBuildings",
        .doc = "Choose how buildings are made, for the whole settlement or (polygon=) a district, kept in the edits, and build "
               "them. outline: footprint curves and envelope volumes only. massing: greybox blocks with tiers and roofs in the "
               "settlement's buildings output. library: Stagehand library assets fitted to each lot by footprint, era and use "
               "tags, turned to face the street (lots nothing fits are listed). masonry: Masonry stone buildings, landmark lots "
               "only unless landmarksOnly=False. unreal: envelopes go as PCG data with settlementSendToUnreal (send=True sends "
               "now). contacts=True adds Seamwork contact skirts, strips or scatter where placed buildings meet the ground. "
               "Placed objects are grouped under <settlement>_<route> and replaced when the route is built again.",
        .positionalDoc = "settlement mesh or its modifier",
        .flags = {{"route", "r", LotT::String, "outline", "How buildings are made.", routes},
                  {"polygon", "pg", LotT::List, nullptr, "A district, [[x, z], ...]: the route applies to lots whose centre is inside."},
                  {"name", "n", LotT::String, nullptr, "The district's name; the same name replaces its route."},
                  {"build", "bd", LotT::Bool, true, "Place the library or Masonry buildings now."},
                  {"tags", "tg", LotT::List, nullptr, "Library tags every asset must have (default ['building'])."},
                  {"collection", "co", LotT::String, nullptr, "Only assets in this library collection."},
                  {"assetFront", "af", LotT::String, "+z", "The axis library assets face along.", {"+z", "-z", "+x", "-x"}},
                  {"fit", "ft", LotT::Float, 1.05, "How much larger than the footprint an asset may be (1 is exact).", {}, {}, 0.5, 2.0},
                  {"landmarksOnly", "lo", LotT::Bool, true, "Masonry: only landmark lots."},
                  {"limit", "l", LotT::Int, int64_t(12), "Masonry: at most this many buildings.", {}, {}, 1.0},
                  {"contacts", "ct", LotT::Bool, false, "Seamwork contacts at the base of placed buildings."},
                  {"contactKinds", "ck", LotT::List, json::array({"skirt"}), "skirt (seamSkirt), decal (seamDecal), scatter (seamScatter)."},
                  {"send", "sd", LotT::Bool, false, "unreal: send to Unreal now through settlementSendToUnreal."}},
        .run = [](const Args& a) -> json {
            LILA_ZONE();

            const char* cmd = "settlementBuildings";
            const Rebuilt r = lotSettlementOf(a, cmd);
            BuildingRoute route;
            if (!parseRoute(a.get<std::string>("route", "outline"), &route))
                throw CommandError(std::string(cmd) + ": unknown route '" + a.get<std::string>("route") + "' (outline, massing, library, masonry, unreal)");
            const bool build = a.get<bool>("build", true);
            if (build && route == BuildingRoute::Library)
                for (const char* need : {"librarySearch", "place"})
                    lotNeedCommand(need, "Stagehand", cmd);
            if (build && route == BuildingRoute::Masonry)
                lotNeedCommand("masonryBuilding", "Masonry", cmd);
            if (build && a.get<bool>("contacts", false) && (route == BuildingRoute::Library || route == BuildingRoute::Masonry))
            {
                lotNeedCommand("seamFind", "Seamwork", cmd);
                if (r.values.value("ground", std::string()).empty())
                    throw CommandError(std::string(cmd) + ": contacts need the ground the buildings stand on: give the settlement a ground mesh or Terrain");
            }
            json edits = lotEditsOf(r, cmd);
            if (!edits.contains("buildingRoutes"))
                edits["buildingRoutes"] = json::array();
            json region = {{"route", routeName(route)}};
            if (a.has("polygon"))
            {
                json pts = json::array();
                for (const json& p : a.flags["polygon"])
                {
                    if (!p.is_array() || p.size() < 2)
                        throw CommandError(std::string(cmd) + ": polygon is a list of [x, z] points");
                    pts.push_back(json::array({p[0], p[p.size() - 1]}));
                }
                if (pts.size() < 3)
                    throw CommandError(std::string(cmd) + ": polygon needs three or more points");
                region["polygon"] = pts;
            }
            const std::string name = a.get<std::string>("name", "");
            if (!name.empty())
                region["name"] = name;
            json& list = edits["buildingRoutes"];
            bool replaced = false;
            for (json& e : list)
                if ((!a.has("polygon") && !e.contains("polygon")) || (!name.empty() && e.value("name", std::string()) == name))
                {
                    e = region;
                    replaced = true;
                }
            if (!replaced)
            {
                if (a.has("polygon"))
                    list.push_back(region);
                else
                    list.insert(list.begin(), region);
            }
            writeEdits(r, edits);
            const Rebuilt after = rebuildSettlement(scene::Graph::current(), r.modifier, cmd);
            std::vector<const Envelope*> envs;
            for (const Envelope& env : after.layout.envelopes)
                if (env.route == route && (route != BuildingRoute::Masonry || !a.get<bool>("landmarksOnly", true) || !env.landmark.empty()))
                    envs.push_back(&env);
            if (route == BuildingRoute::Masonry && int64_t(envs.size()) > a.get<int64_t>("limit", 12))
                envs.resize(size_t(a.get<int64_t>("limit", 12)));
            json out = {{"route", routeName(route)}, {"envelopes", envs.size()}, {"modifier", after.modifier}};
            std::vector<std::string> made;
            if (build && (route == BuildingRoute::Library || route == BuildingRoute::Masonry))
            {
                const std::string base = after.mesh.empty() ? std::string("Settlement") : after.mesh.substr(after.mesh.find_last_of('|') + 1);
                const std::string groupName = base + "_" + routeName(route) + (name.empty() ? "" : "_" + name);
                if (scene::Graph::current().find(groupName))
                    ops::execute("delete", {{"args", {groupName}}});
                if (route == BuildingRoute::Library)
                {
                    // The picks go in the edits so the layout carries them as engine placements on every rebuild.
                    json picks = json::object();
                    out["library"] = placeLibrary(envs, a, made, picks, cmd);
                    json kept = lotEditsOf(after, cmd);
                    json& all = kept["libraryPicks"];
                    if (!all.is_object())
                        all = json::object();
                    for (const Envelope* env : envs)
                        all.erase(std::to_string(env->key));
                    all.update(picks);
                    writeEdits(after, kept);
                    out["placements"] = picks.size();
                }
                else
                    out["masonry"] = buildMasonry(after, envs, made, cmd);
                // Contacts first: grouping renames the placed objects' paths.
                if (a.get<bool>("contacts", false) && !made.empty())
                    out["contacts"] = contactsFor(after, made, a.flags.value("contactKinds", json::array({"skirt"})), cmd);
                if (!made.empty())
                {
                    json args = json::array();
                    for (const std::string& m : made)
                        args.push_back(m);
                    out["group"] = ops::execute("group", {{"args", args}, {"name", groupName}});
                }
            }
            else if (route == BuildingRoute::Unreal)
            {
                if (a.get<bool>("send", false))
                    out["sent"] = ops::execute("settlementSendToUnreal", {{"args", {after.modifier}}});
                else
                    out["note"] = "the envelopes go to Unreal as PCG data with settlementSendToUnreal";
            }
            else if (a.get<bool>("contacts", false))
                out["note"] = "contacts need placed buildings: the library or masonry route";
            out["objects"] = made;
            return out;
        },
        .examples = {"settlementBuildings('Settlement', route='massing')",
                     "settlementBuildings('Settlement', route='library', tags=['medieval', 'house'], contacts=True)",
                     "settlementBuildings('Settlement', route='masonry', polygon=[[-5000, -5000], [5000, -5000], [5000, 5000], [-5000, 5000]], name='old_town')"},
    });
}
} // namespace settlement
