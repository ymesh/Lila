#pragma once

#include "Projection.h"
#include "Terrain.h"

#include <cstdint>
#include <filesystem>
#include <functional>
#include <limits>
#include <map>
#include <string>
#include <string_view>
#include <vector>

namespace terrain
{
using OsmTags = std::map<std::string, std::string, std::less<>>;

/** A point of a way: its node id (or a key made from its position when the data gave positions only) and where it is. */
struct OsmPoint
{
    int64_t key = 0;
    LatLon at;
};

/** OpenStreetMap data as read: ways resolved to positions, relations listing their member ways. */
struct OsmData
{
    struct Way
    {
        int64_t id = 0;
        std::vector<OsmPoint> points;
        OsmTags tags;
    };
    struct Member
    {
        /** 'n' node, 'w' way, 'r' relation. */
        char type = 'w';
        int64_t ref = 0;
        std::string role;
        /** Positions given inline (Overpass "out geom"), used when the way itself isn't in the data. */
        std::vector<OsmPoint> points;
    };
    struct Relation
    {
        int64_t id = 0;
        std::vector<Member> members;
        OsmTags tags;
    };
    /** A node with tags of its own (a shop, a place, a peak); untagged nodes are only positions of ways. */
    struct Node
    {
        int64_t id = 0;
        LatLon at;
        OsmTags tags;
    };
    std::vector<Way> ways;
    std::vector<Relation> relations;
    std::vector<Node> tagged;
    size_t nodes = 0;
    /** Ways that named nodes the data doesn't have (cut off at the extract's edge); their known part is kept. */
    size_t missingNodes = 0;
};

/** .osm XML. Throws std::runtime_error. */
OsmData parseOsmXml(std::string_view text);
/** Overpass API JSON ("out body" with nodes, or "out geom"). */
OsmData parseOverpassJson(std::string_view text);
/** .osm.pbf (zlib or uncompressed blobs). */
OsmData parseOsmPbf(std::string_view bytes);
/** By extension: .osm/.xml, .json, .pbf. */
OsmData readOsm(const std::filesystem::path& path);

enum class OsmFeatureKind : uint8_t
{
    Road,
    Path,
    River,
    Stream,
    Lake,
    Coastline,
    Pier,
    Breakwater,
    Quay,
    Dam,
};

const std::vector<std::string>& osmFeatureKindNames();

/** A line or closed outline for a terrain feature, in degrees. */
struct OsmFeature
{
    OsmFeatureKind kind = OsmFeatureKind::Road;
    /** The way or relation id. */
    int64_t id = 0;
    /** The real name: only for the hidden source attribute and the real-name check, never for output. */
    std::string name;
    /** The OSM class: the highway or waterway value, "lake", "reservoir", "coastline", "pier", "breakwater", "quay", "dam". */
    std::string cls;
    /** Full width, cm: the width tag, lanes x 3.5 m, or a default for the class. */
    double width = 0.0;
    bool closed = false;
    std::vector<LatLon> points;
};

/** Land cover from OSM tags: rings of one polygon, outer and inner (holes), filled even-odd. */
struct OsmArea
{
    /** forest, grassland, farmland, scrub, rock, sand, urban, wetland, water or glacier. */
    std::string cls;
    int64_t id = 0;
    std::vector<std::vector<LatLon>> rings;
};

const std::vector<std::string>& landcoverClasses();

/**
 * A building or building:part footprint: one outer ring and its holes (courtyards), in degrees. A multipolygon with several
 * outers gives one OsmBuilding per outer, numbered by piece. Heights in metres as tagged, NaN when absent; levels -1.
 */
struct OsmBuilding
{
    int64_t id = 0;
    char type = 'w'; // 'w' way or 'r' relation
    int piece = 0;
    bool part = false; // building:part, a volume of the building whose outline holds it
    std::vector<LatLon> outer;
    std::vector<std::vector<LatLon>> holes;
    std::string building; // the building (or building:part) value: yes, house, apartments, church, ...
    double height = std::numeric_limits<double>::quiet_NaN(), minHeight = std::numeric_limits<double>::quiet_NaN();
    double roofHeight = std::numeric_limits<double>::quiet_NaN();
    int levels = -1, minLevel = -1, roofLevels = -1;
    std::string roofShape, colour, roofColour, material, roofMaterial, startDate;
    /** What it is for (osmProgram), from its use tags or its building value; empty for a plain building. */
    std::string program;
    /** names::Kind name of what it is: business, school, religious, ... (building when it is only a building). */
    std::string kind;
    /** Real names (name, brand, operator, official, alternative and language names): for the hidden source only. */
    std::vector<std::string> realNames;
    /** The brand or operator, normalised: branches of one chain share their fictional name. */
    std::string chain;
};

/**
 * A point of interest or a named area: amenity, shop, tourism, office, leisure, landuse, aeroway, man_made, harbour,
 * places (towns, neighbourhoods, islands), beaches, bays and peaks. A node has only at; an area has rings (outer and inner,
 * even-odd, like OsmArea) and at its centre.
 */
struct OsmPlace
{
    int64_t id = 0;
    char type = 'n';
    std::string key, value; // the tag that made it a place, such as amenity=restaurant
    std::string program;    // osmProgram: taqueria, pharmacy, cemetery, port, town, ...
    std::string kind;       // names::Kind name
    LatLon at;
    std::vector<std::vector<LatLon>> rings;
    std::vector<std::string> realNames;
    std::string chain;
};

struct OsmExtract
{
    std::vector<OsmFeature> features;
    std::vector<OsmArea> areas;
    std::vector<OsmBuilding> buildings;
    std::vector<OsmPlace> places;
};

/**
 * Roads, paths, rivers, streams, lakes, coastline, piers, breakwaters, quays and dams as features, land cover as areas,
 * buildings and building parts, and places. Multipolygons (with holes) are assembled.
 */
OsmExtract extractOsm(const OsmData& d);

/**
 * Every position an extract holds (features, areas, buildings and places), for a warp applied to all of it before import
 * (World Remix): f may move each one.
 */
void forEachPoint(OsmExtract& x, const std::function<void(LatLon&)>& f);

/** What tags say a thing is: a program (taqueria, pharmacy, school, cemetery, port, ...) and a names::Kind name; empty for none. */
std::pair<std::string, std::string> osmProgram(const OsmTags& t);

/** A feature's line on the heightfield (cm, XZ), cut where it leaves the heightfield; each run is its own line. */
std::vector<std::vector<glm::dvec2>> clipToHeightfield(const std::vector<glm::dvec2>& line, const Heightfield& h, bool closed);

/** Fills masks landcover_<class> (1 inside) for every class present. Returns the covered fraction per class. */
std::map<std::string, double> rasterizeLandcover(Heightfield& h, const std::vector<OsmArea>& areas, const Georef& g);

/** The ODbL attribution a game or film using the data must show. */
const char* osmAttribution();
} // namespace terrain
