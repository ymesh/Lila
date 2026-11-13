#pragma once

#include "SettlementCore.h"

#include <map>
#include <mutex>
#include <span>
#include <string>
#include <string_view>
#include <vector>

/**
 * Settlement on a Terrain: siting rules as weighted terms, main roads routed by the terrain, the pinned plan that only
 * re-plans where the terrain changed, and the checks and explanations agents read. Terrain commands are called by name
 * at runtime (TerrainLink.cpp); nothing here links the Terrain extension.
 */
namespace settlement
{
// ---------------------------------------------------------------------------------------------------------------------
// Siting rules

/** town, market, port, mining, fort, farming, mill. */
std::vector<std::string> sitingKinds();
/** The weights a kind starts from; false for an unknown kind. */
bool sitingPreset(std::string_view kind, SitingRules* out);
json toJson(const SitingRules& r);
/** kind first (resetting the weights to its preset), then any weight given. Throws std::runtime_error for a bad field. */
void applyJson(SitingRules& r, const json& j);

enum class SiteTerm : uint8_t
{
    Flat,
    Water,
    Trade,
    Defense,
    Resource,
    Climate,
    Flood,
    Count,
};
const char* termName(SiteTerm t);

/** What is known about a candidate: each term's value 0 to 1 and why, or nothing where the ground has no data for it. */
struct SiteFacts
{
    Vec2 p{0.0};
    double value[size_t(SiteTerm::Count)] = {};
    bool known[size_t(SiteTerm::Count)] = {};
    std::string why[size_t(SiteTerm::Count)];
    double edge = 1.0; // room to the outline, 0 to 1, multiplies the score
    double gate = 1.0; // 1 on buildable land, less off it
    std::string gateWhy;

    void set(SiteTerm t, double v, std::string reason)
    {
        value[size_t(t)] = v;
        known[size_t(t)] = true;
        why[size_t(t)] = std::move(reason);
    }
};

/**
 * Weighted mean of the known terms, scaled down by flood risk (weight flood), the edge and the gate. reasons.terms has
 * every term: value, weight, contribution and why (unknown terms say so and count for nothing).
 */
SiteScore scoreSite(const SiteFacts& f, const SitingRules& r, double jitter = 0.0);
/** Best first, no two closer than spacing, at most count. */
std::vector<SiteScore> pickSites(std::vector<SiteScore> all, int count, double spacing);
/** Where roads cross water lines. */
std::vector<Vec2> roadBridges(std::span<const std::vector<Vec2>> roads, std::span<const WaterLine> water);

// ---------------------------------------------------------------------------------------------------------------------
// Main roads routed by the terrain

/**
 * Routes the layout asks for, answered by the terrain router (terrainRoute) and kept in the settlement's edits under
 * terrainRoutes. A request with no answer is noted as missing, so the command can route it and build again.
 */
struct RouteBook
{
    struct Entry
    {
        Vec2 a{0.0}, b{0.0};
        double maxGrade = 0.12; // rise over run asked for
        std::vector<Vec2> pts;
        std::string source = "terrain"; // terrain, or own when the terrain's way left the outline or crossed an exclusion
        double gradeFound = 0.0;        // steepest grade along it, rise over run
        bool withinGrade = true;
        std::string note;
    };
    std::vector<Entry> entries;
    double tolerance = 100.0; // cm between asked and booked ends
    bool recording = true;

    /** The booked route from a to b (either way round), or nullptr after noting it as missing. */
    const std::vector<Vec2>* use(Vec2 a, Vec2 b, double maxGrade) const;
    /** Requests that had no route, in the order asked. */
    std::vector<Entry> missing() const;
    void clearMissing() const;

private:
    mutable std::mutex mLock;
    mutable std::vector<Entry> mMissing;
};

json toJson(const RouteBook& b);
std::shared_ptr<RouteBook> routeBookFromJson(const json& j);

/** The steepest grade a street of this class may take, rise over run (arterials and collectors the profile's). */
double classMaxGrade(const Profile& p, StreetClass c);

// ---------------------------------------------------------------------------------------------------------------------
// The pinned plan

/**
 * A layout's street graph, blocks, lots and envelopes as they were planned, kept in the settlement's edits under plan.
 * While every input but the ground is unchanged (key), the settlement shows this plan instead of laying out afresh,
 * so the terrain changing under it moves nothing until it is re-planned, and re-planning changes only dirty regions.
 */
struct PlanSnapshot
{
    uint64_t key = 0;
    std::string revision, terrain;
    std::vector<StreetNode> nodes;
    std::vector<StreetEdge> edges;
    std::vector<Block> blocks;
    std::vector<Lot> lots;
    std::vector<Envelope> envelopes;
    std::vector<Vec2> bridges;
};

/** Hash of every input but the ground: the site, the options and the edits besides their plan and terrain records. */
uint64_t planKey(const Site& site, const Options& o);
json planToJson(const Layout& l, uint64_t key, const std::string& revision, const std::string& terrain);
/** Throws std::runtime_error when j is not a plan. */
std::shared_ptr<PlanSnapshot> planFromJson(const json& j);
/** The layout with the plan's streets, blocks, lots and envelopes in place of its own. */
void applySnapshot(Layout& l, const PlanSnapshot& s);

/** build, then the pinned plan in place when its key still matches (Layout::plan says which). */
Layout buildPlanned(const Site& site, const Ground& ground, const Options& o, const WalkSurface* walk = nullptr);

/** A changed rectangle of ground, from terrainDirtyRegions. */
struct DirtyRect
{
    Vec2 lo{0.0}, hi{0.0};
};

bool touches(const Polygon& p, const DirtyRect& r);

/**
 * before outside the dirty rectangles and fresh inside them: streets of before that touch a rectangle give way to the
 * fresh streets there; blocks of before that touch none stay as they are with their ids, lots and envelopes; fresh
 * blocks touching one (and overlapping no kept block) come in with new ids past the old ones. report gets the counts.
 */
Layout mergeDirty(const Layout& before, const Layout& fresh, std::span<const DirtyRect> dirty, json* report = nullptr);

/** A block's outline, lots and envelopes as one hash: equal hashes, untouched block. */
uint64_t blockHash(const Layout& l, int block);

/**
 * The parts of the edits this file owns: water the settlement builds (water: [{points, width, kind}]), the terrain
 * routes and the pinned plan. Also sets site.editsKey.
 */
void applyTerrainEdits(Site& site, const json& edits);

// ---------------------------------------------------------------------------------------------------------------------
// Checks and explanations

/** A street as its terrain road was built: the steepest grade over all its parts and the road's limit, in percent. */
struct GradedStreet
{
    std::string road; // the terrain road id
    double grade = 0.0;
    double limit = 0.0;
    double length = 0.0;
    std::string why; // the road solver's warnings
};

/** True when a graded street is over its limit, past the road solver's own tolerance. */
inline bool gradedOverLimit(const GradedStreet& s) { return s.limit > 0.0 && s.grade > s.limit * 1.01 + 0.05; }

/** A street edge carried over water: flagged as a bridge, or an arterial meeting water (the generator bridges those). */
bool edgeIsBridge(const Layout& l, int edge);
/** True when the edge crosses or runs in water. */
bool edgeInWater(const Layout& l, int edge);

/**
 * The plan's verification checks: lots without frontage, overlapping lots, streets over their class's grade, streets
 * through water without a bridge, envelopes outside their lot's setbacks, foundations above the ground, the lane graph
 * not strongly connected per mode (vehicles, people), signal phases in conflict. Returns {ok, counts, issues: [{check,
 * severity, id, message, at}]}. A street in graded is judged by the grade its terrain road was built at, as
 * settlementGrade's overGrade is, instead of by the ground between its junctions.
 */
json checkLayout(const Layout& l, const Ground& ground, const Options& o, const std::map<uint64_t, GradedStreet>* graded = nullptr);
/**
 * Issues from the lane graph's own validation, in checkLayout's issue form (check laneGraph), or null when that
 * validation is not built in (checkLayout then warns that lane checks are unavailable).
 */
json streetscapeLaneChecks(const Layout& l, const Ground& ground);

/**
 * Why a part is where it is. id: centre, site, street:<id> (or a bare street id), lot:<id>, block:<id> or
 * landmark:<lot id>. siting is the settlement's stored siting record (its site's terms), or null.
 */
json explain(const Layout& l, const Ground& ground, const json& siting, const std::string& id);
} // namespace settlement
