#pragma once

#include "Osm.h"

#include <cstdint>
#include <filesystem>
#include <map>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

/**
 * OpenStreetMap data kept whole: every node with its tags (points of interest too), every way as node references and
 * every relation, so a remix can move all of it and write it back for any reader (Terrain, Settlement, Buildings). All
 * positions live on nodes, so warping the nodes warps every feature kind, including kinds added later.
 */
namespace openworld
{
using terrain::LatLon;
using OsmTags = std::map<std::string, std::string, std::less<>>;

struct OsmDoc
{
    struct Node
    {
        int64_t id = 0;
        LatLon at;
        OsmTags tags;
    };
    struct Way
    {
        int64_t id = 0;
        std::vector<int64_t> nodes;
        OsmTags tags;
    };
    struct Member
    {
        /** 'n' node, 'w' way, 'r' relation. */
        char type = 'w';
        int64_t ref = 0;
        std::string role;
    };
    struct Relation
    {
        int64_t id = 0;
        std::vector<Member> members;
        OsmTags tags;
    };

    std::vector<Node> nodes;
    std::vector<Way> ways;
    std::vector<Relation> relations;

    /** Node index by id; rebuilt by index(). */
    std::unordered_map<int64_t, size_t> nodeAt;
    void index();
    const Node* node(int64_t id) const;
    bool empty() const { return nodes.empty() && ways.empty(); }
};

/** .osm XML, Overpass .json (out body or out geom) and .osm.pbf, tagged nodes included. Throws std::runtime_error. */
OsmDoc readOsmDoc(const std::filesystem::path& path, std::vector<std::string>* notes = nullptr);
OsmDoc parseOsmDocJson(std::string_view text);
OsmDoc parseOsmDocXml(std::string_view text);

/** Overpass JSON ("out body": nodes, then ways by node id, then relations), which every OSM reader in Lila takes. */
std::string writeOsmDocJson(const OsmDoc& d);

/** Ways with their positions, for Terrain's feature and land cover extraction. */
terrain::OsmData toOsmData(const OsmDoc& d);

std::string_view tagOf(const OsmTags& t, std::string_view key);

/** What a way is to the remix: a road (any highway line), a river (waterway line), coastline, an area, or other. */
enum class WayKind : uint8_t
{
    Other,
    Road,
    River,
    Coastline,
    Area,
};
WayKind wayKind(const OsmDoc::Way& w);
} // namespace openworld
