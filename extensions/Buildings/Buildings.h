#pragma once

#include <glm/glm.hpp>
#include <nlohmann/json.hpp>

#include <cstdint>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace lila::geo
{
class Mesh;
}

/**
 * Buildings: a procedural building generator. A building's spec (footprint, sides, tiers, floors, roof, entrances,
 * program, businesses, style) runs through a grammar, footprint -> tiers -> floors -> facades -> bays -> modules, and
 * comes out as a shell (walls, bands, plinths, roofs as one soup) plus modules (windows, doors, shopfronts, signs,
 * balconies, AC units, water tanks...). Every module is a slot (lila/Slots.h) with a stable id
 * "building:<key>/t<tier>/f<facade>/l<floor>/b<bay>/<kind>", so swap rules and overrides survive regeneration. Shell
 * modules (wall bays, cornices, pilasters, parapets, roofs) are merged into the shell until a rule or override gives
 * them a visual, then the instance draws instead.
 *
 * Everything here is pure: no scene, any thread. Units cm, Y up, footprints in (x, z).
 */
namespace buildings
{
using json = nlohmann::json;
using Vec2 = glm::dvec2;
using Vec3 = glm::dvec3;
using Ring = std::vector<Vec2>;

// Geometry ------------------------------------------------------------------------------------------------------------

/** Signed area in (x, z); positive is counter-clockwise in those coordinates. */
double signedArea(const Ring& r);
/** The ring counter-clockwise (positive area), duplicate and collinear points removed. */
Ring cleanRing(Ring r, bool ccw = true);
Vec2 centroid(const Ring& r);
bool inside(const Ring& r, Vec2 p);
/** Each edge moved out (d > 0) or in along its normal, corners mitred (clamped). */
Ring offsetRing(const Ring& r, double d);
/** Outward normal of edge a -> b of a counter-clockwise ring. */
inline Vec2 outward(Vec2 a, Vec2 b)
{
    const Vec2 d = b - a;
    const double l = glm::length(d);
    return l > 1e-9 ? Vec2(d.y, -d.x) / l : Vec2(0.0, -1.0);
}
/** Area of the overlap of two simple rings (Sutherland-Hodgman against the convex parts of b's triangles), cm2. */
double overlapArea(const Ring& a, const Ring& b);
/** Triangles of a polygon with holes (holes bridged into the outline, then ear clipped), as index triples into pts. */
void triangulateWithHoles(const Ring& outer, const std::vector<Ring>& holes, std::vector<Vec2>& pts, std::vector<uint32_t>& tris);

// Soup ----------------------------------------------------------------------------------------------------------------

/** Material slots every building mesh uses; palette colours are carried in the face colour and the material name. */
enum Mat : int32_t
{
    MatRender,
    MatTrim,
    MatBase,
    MatRoof,
    MatRooftop,
    MatGlass,
    MatFrame,
    MatDoor,
    MatMetal,
    MatSign,
    MatSignFace,
    MatAwning,
    MatConcrete,
    MatStone,
    MatWater,
    MatWood,
    MatDark,
    MatGlassClear, // glazing of enterable buildings, seen through into the rooms
    MatCount
};
const char* matName(int m);

/** Trims of the LilaBuilding sheet the shell's uv0 is laid on (strips, top to bottom); -1 for faces on their own material. */
enum TrimId : int32_t
{
    TrimNone = -1,
    TrimRender,
    TrimPlinth,
    TrimCornice,
    TrimBand,
    TrimCoping,
    TrimPilaster,
    TrimRoofTile,
    TrimSheet,
    TrimCount
};
struct TrimStrip
{
    const char* name;
    int px;          // strip height on the 2048 sheet
    double worldH;   // cm the strip's height stands for
    const char* style; // Trim Sheets profile style
};
const TrimStrip& trimStrip(int t);
/** uv v range (0 bottom of the sheet, 1 top) of a strip. */
void trimRange(int t, double& v0, double& v1);
/** Sheet width in cm (U wraps every this many cm). */
inline constexpr double kSheetWorld = 800.0;

/** Polygons with per-corner uv and per-face material, trim, colour and owner (the shell module index, -1 none). */
struct Soup
{
    std::vector<Vec3> pts;
    std::vector<glm::vec2> uvs;
    std::vector<uint32_t> sizes;
    std::vector<int32_t> mat;
    std::vector<int32_t> trim;
    std::vector<int32_t> owner;
    std::vector<int32_t> part;   // what the face is (Part)
    std::vector<int32_t> bay;    // index into the building's bays, -1 none
    std::vector<glm::vec4> colour; // linear palette tint, a = dirt

    size_t faces() const { return sizes.size(); }
    void clear();
    void reserve(size_t faces);
    /** Gives back what reserve and growth left unused: a city holds tens of thousands of these. */
    void shrink();
    /** Appends one polygon (counter-clockwise seen from its front). */
    void poly(const Vec3* p, const glm::vec2* uv, size_t n, int32_t m, int32_t trim, int32_t owner, int32_t part, int32_t bay, glm::vec4 colour);
    void append(const Soup& o, int32_t ownerOffset = 0, int32_t bayOffset = 0);
};

/** What a shell face is, for checks, the paint tool and Unreal. */
enum Part : int32_t
{
    PartWall,
    PartReveal,
    PartPlinth,
    PartBand,
    PartCornice,
    PartPilaster,
    PartParapet,
    PartRoof,
    PartRooftop,
    PartSoffit,
    PartPartyWall,
    PartGable,
    PartDome,
    PartStep,
    PartOther
};

/** Soup as a compact mesh: uv0, "material" with names, face ints "part", "bldBay", "bldOwner", corner "color". */
lila::geo::Mesh toMesh(const Soup& s, const std::vector<std::string>& materialNames);

// Styles --------------------------------------------------------------------------------------------------------------

struct Palette
{
    glm::vec3 render{0.8f}, trim{0.9f}, base{0.4f}, roof{0.5f}, frame{0.2f}, door{0.3f}, awning{0.6f};
    float dirt = 0.2f;
    /** Palette entry indices the colours came from (-1: given), so the Lila material names follow them. */
    int renderIndex = 0, trimIndex = 0, roofIndex = 0;
    json toJson() const;
};

/** A style: the grammar's sliders. Read from JSON (styles/*.json); unknown keys are kept in raw. */
struct Style
{
    std::string id, label, description, source;
    std::vector<std::string> tags;
    /** Shipped style ids this user or workspace style stands in for: buildings naming one of them get this style. */
    std::vector<std::string> replaces;
    // Floors
    int minFloors = 1, maxFloors = 40;
    double groundHeight = 420.0, floorHeight = 320.0, plinth = 30.0;
    // Bays
    double bayWidth = 320.0, minBay = 200.0, cornerWidth = 40.0;
    /**
     * Bay rhythm: which bay columns of a facade are blank wall. `rhythm` lays letters from both ends of the facade inward
     * ("w" opening, "b" blank; "wbw" gives window, pier, window... mirrored); without one, street and courtyard columns are
     * blank by chance `blankBays` and side walls open by chance `sideOpenings` (halved within `sparseWithin` of a
     * neighbour). Walls within `blankWithin` of a neighbour have no openings.
     */
    std::string rhythm;
    double blankBays = 0.12, sideOpenings = 0.5, blankWithin = 300.0, sparseWithin = 800.0;
    double windowW = 0.45, windowH = 0.55, windowSill = 0.28; // of the bay width / floor height
    double doorW = 0.45, doorH = 0.78;
    double shopW = 0.82, shopH = 0.78;
    double reveal = 20.0; // opening depth into the wall
    // Ground floor
    std::string groundStreet = "window"; // what a residential street bay is on the ground floor: window, door, wall
    double arcade = 0.0;    // chance of portales on street facades (1: always)
    double arcadeDepth = 300.0;
    bool shopfronts = true; // businesses get shopfronts
    double shutters = 0.0;  // roll-up shutters over shopfronts
    double awnings = 0.0;
    std::string doorKind = "door.house";
    // Upper floors
    double balconies = 0.0;
    std::string balconyKind = "iron"; // iron, concrete, glass, stack (every bay, every floor)
    double grilles = 0.0;
    double acUnits = 0.0;
    std::string windowKind = "window.residential";
    // Articulation
    bool pilasters = false;
    std::string cornice = "light"; // none, light, heavy, vigas
    bool bands = false;
    std::string corner = "none"; // none, quoin, pilaster
    bool drainpipes = false;
    // Roof
    std::string roof = "flat"; // flat, hip, gable, dome, sheet, envelope (whatever the envelope says)
    std::string roofMaterial = "tile"; // tile, sheet, concrete
    double parapet = 80.0, overhang = 30.0, pitch = 25.0;
    double waterTanks = 0.0, clutter = 0.0;
    // Massing
    int podium = 0;          // floors of a full-footprint podium under an inset tower
    double towerInset = 0.0; // cm the tower steps in
    int stepEvery = 0;       // stepped tower: inset every this many floors
    double stepInset = 0.0;
    double wallThickness = 30.0;
    // Extras
    int columns = 0;          // portico columns at the main entrance
    int domes = 0;            // domes on the roof
    double perimeterWall = 0.0; // height of a wall round the lot, 0 none
    bool gate = false, pool = false;
    int bellTowers = 0;
    double rebar = 0.0;       // chance of rebar left standing on the roof
    std::string signKind = "sign.storefront";
    bool glassFacade = false; // windows fill the bay (curtain wall look)
    bool enterable = false;   // W4 default
    // Palette
    std::vector<glm::vec3> renders, trims, roofs, bases;
    glm::vec2 dirt{0.05f, 0.3f};
    /**
     * Two-tone modules, by module kind: the palette entries (render, trim, roof, base, frame, door, awning, or none) its
     * main parts and its second-tone parts (its trim and frames: an arcade's columns against its beams, a window's frame)
     * take. An empty entry keeps the default (moduleTones()).
     */
    std::map<std::string, std::array<std::string, 2>> moduleTones;
    json raw;

    static Style fromJson(const json& j, std::string* error = nullptr);
    json toJson() const;
};

/**
 * The two colours a module instance carries (linear rgb, alpha 1, or zero for none): its main parts' and its second
 * tone's. By default the main parts take render (wall-like blocks, domes), roof (roofs) or trim (the rest), and the second
 * tone the same, except on framed pieces (windows, doors, shopfronts, balconies) whose frames keep their own colour.
 */
std::array<glm::vec4, 2> moduleTones(const Style& st, const Palette& p, const std::string& kind);

/** A style id from a Settlement style guess (passed through) or else the program and use ("police" -> policeStation). */
std::string styleFor(std::string_view settlementStyle, std::string_view program, std::string_view use);
/** The shipped style ids, in menu order. */
const std::vector<std::string>& shippedStyleIds();

glm::vec3 hexColour(std::string_view hex, glm::vec3 fallback);
std::string colourHex(glm::vec3 linear);

// Specs ---------------------------------------------------------------------------------------------------------------

struct Business
{
    std::string name, program;
    Vec2 at{0.0};
    bool hasAt = false;
};

/** One building's input, from a Settlement envelope, a curve, an OSM building through its envelope or a mesh. */
struct Spec
{
    std::string id; // stable: "building:<key hex>"
    uint64_t key = 0;
    std::string source; // settlement, curve, mesh
    Ring footprint;     // counter-clockwise
    std::vector<int> sides; // per edge: 0 street, 1 party, 2 open
    /** Per edge: cm from the edge to the nearest other building in front of it; empty when not measured. */
    std::vector<double> clearance;
    std::vector<Ring> holes;
    struct Tier
    {
        Ring footprint;
        double bottom = 0.0, top = 0.0;
    };
    std::vector<Tier> tiers;
    double ground = 0.0;     // lowest ground under the footprint
    double highGround = 0.0; // highest
    std::vector<double> cornerGround; // per footprint point, when sampled
    double top = 0.0;        // top of the walls plus roof (envelope top)
    double roofHeight = 0.0;
    int floors = 1;
    std::string roof = "flat";
    Vec2 front{0.0, 1.0}, ridge{1.0, 0.0};
    std::vector<Vec2> entrances;
    std::string program, use, groundUse, style, styleFrom, name, corner = "none", landmark, era;
    std::vector<Business> businesses;
    std::string colour, roofColour, material, roofMaterial, osmId;
    int wealth = 1;
    float random = 0.0f;
    std::vector<std::string> regions;
    double setbackFront = 0.0, setbackSide = 0.0;

    json toJson() const;
    static Spec fromJson(const json& j);
    /** From Settlement's published envelope (settlementEnvelopes). */
    static Spec fromEnvelope(const json& env, const std::string& settlement);
    double groundAt(Vec2 p) const;
    /** Lowest and highest ground under the building: its sampled corners, else ground and highGround. */
    std::pair<double, double> groundRange() const;
    /** Whether the ground floor steps with the ground: sampled corners more than 60 cm apart. */
    bool sloped() const;
    /** A stepped ground floor's level over a point: the ground there plus the plinth, in 30 cm steps, between the lowest
     *  ground (plus 5 cm) and the level of the ground floor (`floor0`, the highest ground plus the plinth). */
    double steppedLevel(Vec2 p, double plinth, double floor0) const;
};

/** Whether the building has shops on its ground floor: businesses, or a commercial program or use. */
bool isBusiness(const Spec& s);

/** Fills each spec's clearance: per footprint edge, the distance out from it to the nearest other footprint (cm, capped at `reach`). */
void measureClearance(std::vector<Spec>& specs, double reach = 2000.0);

/** A building's edits, kept by its id and applied over the spec and style on every regeneration. */
struct Edits
{
    std::string style;
    std::optional<int> floors, seed;
    std::optional<double> bayWidth, floorHeight;
    std::optional<std::string> roof;
    std::optional<bool> enterable;
    json palette = json::object(); // render, trim, roof: "#rrggbb"
    /** Bay paint by "t0/f2/l0/b3": window, door, shopfront, wall, arcade. */
    std::map<std::string, std::string> bays;
    /** Toggles by "t0/f2/l1/b3": balcony, awning, ac, grille, shutter -> on/off. */
    std::map<std::string, std::map<std::string, bool>> toggles;
    /** Bay counts by facade "t0/f2". */
    std::map<std::string, int> facadeBays;
    /** The interior's layout edits (W4, Interior.h InteriorEdits): program, room kinds, splits, merges, moves, doors. */
    json interior = json::object();

    bool empty() const;
    json toJson() const;
    static Edits fromJson(const json& j);
};

// Generation ----------------------------------------------------------------------------------------------------------

/** A placed module: a slot with a stable path, frame and size. Shell modules are drawn in the shell by default. */
struct Module
{
    std::string type;   // slot type: window.residential, door.shop, sign.storefront, wall.bay, cornice...
    std::string path;   // "t0/f2/l1/b3/window"
    std::string kind;   // placeholder kind (Modules.cpp)
    Vec3 origin{0.0};   // bottom centre of the module's front, cm
    Vec3 x{1, 0, 0}, y{0, 1, 0}, z{0, 0, 1}; // frame: +Z out of the wall
    Vec3 size{100.0};   // w, h, d
    int variant = 0;
    bool shell = false; // geometry in the shell soup (owner index = this module)
    json data;          // sign text, business, program...
    glm::dmat4 frame() const;
};

/** A bay of a facade on one floor: where the paint tool and W4 find it. */
struct Bay
{
    std::string path; // "t0/f2/l0/b3"
    std::string kind; // window, door, shopfront, wall, arcade
    Vec3 corners[4];  // bottom left, bottom right, top right, top left (seen from outside)
    Vec3 normal{0.0};
    int tier = 0, facade = 0, floor = 0, bay = 0;
    bool balcony = false;
    /** The opening in the shell, bottom left and size along the facade (cm), zero size for none. */
    double openS = 0.0, openT = 0.0, openW = 0.0, openH = 0.0;
};

struct Facade
{
    std::string path; // "t0/f2"
    int tier = 0, edge = 0;
    bool courtyard = false;
    int side = 2; // 0 street, 1 party, 2 open
    Vec2 a{0.0}, b{0.0}, normal{0.0};
    double bottom = 0.0, top = 0.0;
    int bays = 0;
};

/** W4: a floor of the building: its outline, height and level. */
struct FloorPlate
{
    int tier = 0, floor = 0;
    double y = 0.0, height = 0.0;
    Ring outline;
    std::vector<Ring> holes;
    /** The tier's outer ring (the facades' plane) and wall thickness; tower caps (bell, corner towers) have no rooms. */
    Ring ring;
    double wall = 30.0;
    bool tower = false;
};

/** W4: an opening on a facade: what it is and where (bay path, world rectangle). */
struct Opening
{
    std::string bay, kind, slot;
    Vec3 origin{0.0}, normal{0.0};
    double width = 0.0, height = 0.0;
};

struct Result
{
    std::string id, style;
    Soup shell;
    std::vector<Module> modules;
    std::vector<Facade> facades;
    std::vector<Bay> bays;
    std::vector<FloorPlate> plates;
    std::vector<Opening> openings;
    Vec2 core{0.0}; // stair and lift core, plan position
    double coreSize = 0.0;
    Palette palette;
    bool enterable = false;
    int floors = 0;
    double wallTop = 0.0;
    Vec3 lo{0.0}, hi{0.0};
    std::vector<std::string> warnings;
    /** Massing proxies: hlod (tiers and roofs, no openings) and far (flat-roof blocks). */
    Soup hlod, far;
};

struct Options
{
    bool proxies = true;
    bool clutter = true;
};

/** Runs the grammar. Throws nothing; problems go in warnings. */
std::shared_ptr<const Result> generate(const Spec& spec, const Style& style, const Edits& edits, const Options& o = {});

/** A generated building as bytes for the building cache (ResultCache.cpp). */
std::string encodeResult(const Result& r);
/** The building encodeResult wrote; null when the bytes are not a whole Result of this format. */
std::shared_ptr<const Result> decodeResult(std::string_view bytes);

/** The size a module's placeholder is built at: width and height down to 5 cm steps (never past the module's own size,
 *  so a frame never reaches into its reveals), depth to the centimetre. Placeholders of one size are shared. */
Vec3 placeholderSize(const Vec3& size);
/** The placeholder mesh soup for a module kind at a size (front +Z, bottom centre at the origin), with uv0. */
const Soup& placeholderSoup(const std::string& kind, const Vec3& size, int variant);
/** Placeholder kinds, for the docs and the check. */
const std::vector<std::string>& placeholderKinds();

/** Slot types Buildings registers (id, label, footprint, attach, placeholder, tags, doc), JSON rows. */
const json& slotTypeRows();

/** FNV-1a 64. */
uint64_t fnv(std::string_view s, uint64_t h = 1469598103934665603ull);
std::string hex64(uint64_t v);
/** A deterministic unit random from a key and a salt. */
double unitRandom(uint64_t key, uint64_t salt);
} // namespace buildings
