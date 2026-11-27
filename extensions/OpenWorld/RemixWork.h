#pragma once

#include "Remix.h"

#include <map>
#include <tuple>
#include <unordered_map>

/** The state one remix run shares between composing heights, moving features and repairing them. Internal. */
namespace openworld::detail
{
/** How one source meets the map: its edge fade, the seam over the sources under it, where it is present. */
struct SourceEdge
{
    /** Fade toward nothing (the sea floor), cm on the map. */
    double feather = 1500.0;
    /** Blend over the sources under it, cm, and how far that band wanders in (noise amplitude, cm). */
    double seam = 0.0;
    double noise = 0.0;
    /** Box edges that fade: west, north, east, south (local -x, -z, +x, +z). An edge the map ends inside of stays full. */
    bool open[4] = {true, true, true, true};
    /** builtUp: signed distance into the present land on the source's grid, cm on the map (negative outside); else null. */
    std::shared_ptr<const std::vector<float>> inside;
    double vertical = 1.0;
    double erosion = 1.0;
};

/** Where a sample sits in one source's seam over the land under it: weights() fills it for heights to widen the band. */
struct SeamAt
{
    /** Distance in from the source's edge, cm, and how far the band wandered in there; seam 0 when not in a seam. */
    double d = 0.0, wander = 0.0, seam = 0.0;
    /** The weight before the seam, and the weight of the sources under it. */
    double base = 0.0, under = 0.0, wet = 1.0;
};

struct Work
{
    const RemixSpec& spec;
    std::vector<std::shared_ptr<const SourceData>> data;
    std::vector<Polygon> footprints;
    std::vector<double> scales;
    double feather = 1500.0;
    /** Per source; empty until prepareSources, when every source fades on every edge over feather. */
    std::vector<SourceEdge> edges;
    std::shared_ptr<Warp> warp;
    InverseGrid inverse;
    /** The sea floor on the base map (output grid, mask seabed), empty with bathymetry off. */
    Heightfield seaFloor;
    /** Per region: where it ends up on the output map. */
    std::vector<Polygon> targets;
    std::shared_ptr<RemixResult> out;

    explicit Work(const RemixSpec& s) : spec(s) {}

    /** 0 outside a source's footprint, 1 a feather inside it; sea regions fade it out. */
    double weight(size_t s, Vec2 base) const;
    /** Every source's weight at base, in order (each later one's seam reads the ones under it). */
    void weights(Vec2 base, double* into, SeamAt* seams = nullptr) const;
    /** The source's height factor. */
    double vertical(size_t s) const;
    /** 1 on land, falling to 0 across a sea region's seam band inside its outline. */
    double dry(Vec2 base) const;
    /** The source is there and no later source wins the spot. */
    bool owns(size_t s, Vec2 base) const;
    /** Inside a cut, delete or sea region's polygon, so gone from the background. */
    bool punched(Vec2 base) const;
    /** Inside a cut or delete region's polygon: a hole filled from the land round it. */
    bool holed(Vec2 base) const;
    /** On the outline of a sea region (where a feature crossing into it was cut). */
    bool onSeaEdge(Vec2 base) const;
    /** Inside a pasted target later than region above (-1: the background, under every paste). */
    bool covered(Vec2 out, int above) const;
};

/** Working features: nodes on the output map, ways by node id. */
struct Features
{
    struct Node
    {
        int64_t id = 0;
        Vec2 at{0.0};
        OsmTags tags;
    };
    struct Way
    {
        int64_t id = 0;
        std::vector<int64_t> nodes;
        OsmTags tags;
        WayKind kind = WayKind::Other;
        bool added = false;
    };
    /** A road or coastline end made by a cut: its node, the way holding it and whether it starts that way. */
    struct End
    {
        int64_t node = 0;
        size_t way = 0;
        bool start = false;
        WayKind kind = WayKind::Road;
        std::string cls;
        bool joined = false;
        std::string how;
    };

    std::vector<Node> nodes;
    std::unordered_map<int64_t, size_t> nodeAt;
    std::vector<Way> ways;
    std::vector<OsmDoc::Relation> relations;
    std::vector<End> ends;
    /** Ends that were one place before a cut (indices into ends). */
    std::vector<std::pair<size_t, size_t>> partners;
    /** Downstream ends of rivers in base coordinates, by way index, for the drained-before test. */
    std::map<size_t, Vec2> riverBaseEnd;
    int64_t nextId = int64_t(1) << 50;

    int64_t addNode(Vec2 at, OsmTags tags = {});
    Node& node(int64_t id) { return nodes[nodeAt.at(id)]; }
    const Node* find(int64_t id) const
    {
        const auto it = nodeAt.find(id);
        return it == nodeAt.end() ? nullptr : &nodes[it->second];
    }
};

/**
 * Fills w.edges from the spec: feathers, seams, open edges against the map [lo, hi] (all open unless the spec gives the
 * map's size) and builtUp presence on the sources' grids. Needs w.data, w.scales and w.feather.
 */
void prepareSources(Work& w, Vec2 lo, Vec2 hi);

void moveFeatures(Work& w, Features& f);
void reconnectRoads(Work& w, Features& f);
void repairRivers(Work& w, Features& f);
void mergeCoast(Work& w, Features& f);
/** Whether each river drains now and did in the sources, into w.out->rivers. */
void assessRivers(Work& w, Features& f);
/** Writes f into w.out: the OSM doc in latitude and longitude, overlay lines, cut ends and river states. */
void finishFeatures(Work& w, Features& f);

/** Pixels-in-the-grid helpers. */
double bilinear(const std::vector<float>& v, const Heightfield& h, Vec2 p);
size_t nearestIndex(const Heightfield& h, Vec2 p);
} // namespace openworld::detail
