#pragma once

#include <glm/glm.hpp>
#include <nlohmann/json.hpp>

#include <lila/Names.h>

#include <cstdint>
#include <memory>
#include <limits>
#include <string>
#include <string_view>
#include <vector>

/**
 * Site kits: preset places for an open-world map (a cargo port, a cruise terminal, a mausoleum cemetery, a hidden
 * airstrip, an industrial farm, a mountain village, a walled compound, a private island, a lighthouse), each laid out
 * from a spec by a pure function so the same spec always gives the same parts with the same ids.
 *
 * A layout is in the site's own frame: x along its main axis (quay, runway, farm road), z across it (inland from a
 * quay), y up, cm. It holds
 *   - slots: every non-building part (containers, cranes, crop rows, graves, walls, docks) with a stable path;
 *   - envelopes: every building as a Settlement-style envelope with a style tag, built by the Buildings generator
 *     when it is there and drawn as greybox massing until then;
 *   - ground: the Terrain calls that grade the site (pads, quays, roads, canals, the airstrip);
 *   - parts: areas siteCheck judges (land, water, slope limits).
 */
namespace openworld::sites
{
using json = nlohmann::json;
/** A point on the ground plane: x, z. */
using P2 = glm::dvec2;
using Ring = std::vector<P2>;

/** A kit setting: a typed flag of siteCreate, used only by the kinds that list it. */
struct Param
{
    std::string name;
    enum class Type : uint8_t
    {
        Int,
        Float,
        Bool,
        String,
    } type = Type::Float;
    json def;
    std::string doc;
    double min = std::numeric_limits<double>::quiet_NaN();
    double max = std::numeric_limits<double>::quiet_NaN();
    std::vector<std::string> choices;
    std::string label;
};

struct Kind
{
    std::string id;    // port, cruise, cemetery, airstrip, farm, village, compound, island, lighthouse
    std::string label; // Cargo Port
    std::string icon;
    std::string group; // Coast, Inland, Compounds: how the palette groups them
    std::string doc;
    std::vector<std::string> keywords;
    std::vector<std::string> params; // names in params(), in option-box order
    std::string nodeName;            // scene node base name: CargoPort
};

/** Every kit setting, each kind's subset named by Kind::params. */
const std::vector<Param>& params();
const Param* findParam(std::string_view name);
const std::vector<Kind>& kinds();
const Kind* findKind(std::string_view id);
/** "port, cruise, ..." for messages. */
std::string kindList();

/** What siteCreate builds and the site's node keeps: the kind, the stable id, the fictional name and the settings. */
struct Spec
{
    std::string kind;
    std::string id;
    std::string name;
    std::string culture = "mexican";
    int64_t seed = 0;
    /** Sea or lake level the site's water parts sit at (world up), cm. */
    double waterLevel = 0.0;
    /** Where the site's origin stands on the ground plane (world x, z) and how it is turned (degrees about up). */
    P2 at{0.0};
    double heading = 0.0;
    /** Every setting of the kind, defaults filled in. */
    json params = json::object();

    double f(const char* key) const;
    int i(const char* key) const;
    bool b(const char* key) const;
    std::string s(const char* key) const;
    json toJson() const;
};

/** Reads a spec, filling defaults and checking every setting; throws std::runtime_error naming what is wrong. */
Spec specFromJson(const json& j);
/** Checks one setting against the kind: the value in its type, or an error naming the kinds that take it. */
json checkParam(const Kind& k, const std::string& name, const json& value);

/** Where a slot's height comes from. */
enum class Level : uint8_t
{
    Ground, // y above the ground under it
    Water,  // y above the site's water level
    Site,   // y above the ground at the site's origin (decks, roofs of graded pads)
};

struct SlotOut
{
    std::string path; // stable, under the site's owner id
    std::string type;
    P2 at{0.0};
    double y = 0.0;
    Level level = Level::Ground;
    double heading = 0.0; // radians about +Y; 0 faces +Z
    glm::dvec3 scale{1.0};
    std::vector<std::string> tags;
    json data = json::object();
};

/** A building as Settlement envelopes describe one, in the site's frame. */
struct EnvelopeOut
{
    std::string path;
    Ring footprint; // counter-clockwise in (X, Z)
    int floors = 1;
    double height = 300.0; // walls, from the ground floor to the eaves
    std::string roof = "flat"; // flat, gable, hip, mansard, dome
    double roofHeight = 0.0;
    P2 front{0.0, -1.0};
    std::vector<P2> entrances;
    std::string use = "commercial"; // Settlement's use names
    std::string program;
    std::string style; // colonial, seafront, resortTower, modernCommercial, mansion, civic, warehouse, packingPlant, portShed,
                       // adobe, ruralShack, church, mausoleum, hangar, lighthouse, terminal
    std::string name;
    json extra = json::object();
};

/** The style tags an envelope can carry. */
const std::vector<std::string>& styles();

/** A Terrain call, its points in the site's frame. Point flags: footprint, path (lists), a, b, at (points). */
struct GroundOp
{
    std::string id; // under the site's id
    std::string command;
    json flags = json::object();
};

/** An area siteCheck judges. */
struct Part
{
    std::string id;
    std::string kind; // yard, quay, berth, pier, field, plot, runway, building, plaza, dock, island
    Ring ring;
    enum class Needs : uint8_t
    {
        Land,
        Water,
        Any,
    } needs = Needs::Land;
    /** Steepest ground it takes ungraded, degrees; 0 when a stamp grades it. */
    double maxSlope = 0.0;
    /** Deepest a pad may cut and fill on average over its area, cm; 0 for no limit. */
    double maxRelief = 0.0;
    /** Water: shallowest depth it needs, cm. */
    double minDepth = 0.0;
    /** Ground op that grades it, when one does. */
    std::string groundOp;
};

struct Layout
{
    std::vector<SlotOut> slots;
    std::vector<EnvelopeOut> envelopes;
    std::vector<GroundOp> ground;
    std::vector<Part> parts;
    /** Outline of the whole site. */
    Ring boundary;
    /** Fictional names by role: site, plus terminal, customs, warehouse:1, livery:2, family:12, street:3 ... */
    json names = json::object();
    /** Container liveries: [{id, name, slug, colour}]. */
    json liveries = json::array();
    json stats = json::object();
    std::vector<std::string> notes;
};

/** Lays a site out; deterministic in the spec. Throws std::runtime_error for settings that leave no room. */
Layout build(const Spec& s);

/** The site frame to world: a local (x, z) to world (x, z). */
P2 toWorld(const Spec& s, P2 local);
/** A local direction to world. */
P2 dirToWorld(const Spec& s, P2 local);

/** The fictional name a site of this kind, culture and id gets when none is given. */
std::string siteName(const std::string& kind, const std::string& culture, const std::string& id, uint64_t salt = 0);

/** The slot types site kits register (owner OpenWorld). */
struct TypeDef
{
    std::string id, label;
    glm::dvec3 footprint;
    std::string attach; // ground, wall, free
    std::string placeholder;
    std::vector<std::string> tags;
    std::string doc;
};
const std::vector<TypeDef>& slotTypes();

/**
 * Real names imported into the scene (as hashes), which kit names steer clear of: layouts are built without the scene,
 * so the site commands set these before they lay a site out.
 */
void setAvoidNames(lila::names::NameHashes names);
std::shared_ptr<const lila::names::NameHashes> avoidNames();
uint64_t avoidRevision();

/** The owner id of a site's slots and the prefix of its terrain ids: "site:<id>". */
std::string ownerOf(const std::string& siteId);

// Geometry helpers shared by the kits, the node and the check.
Ring rect(double x0, double z0, double x1, double z1);
Ring orientedRect(P2 centre, double halfW, double halfD, double heading);
Ring circle(P2 centre, double r, int n);
double ringArea(const Ring& r);
bool insideRing(const Ring& r, P2 p);
bool ringsOverlap(const Ring& a, const Ring& b);
P2 centroidOf(const Ring& r);
/** [[x, z], ...] and [x, z]. */
json pts(const Ring& r);
json pt(P2 p);
} // namespace openworld::sites
