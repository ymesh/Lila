#pragma once

#include "Buildings.h"

#include <array>
#include <set>

/**
 * Interiors (W4): rooms generated from a building's own grammar result. Each floor plate is laid on a 20 cm grid in the
 * building's frame (aligned with its longest wall), a stair core is fitted through the floors, and the plate is split into
 * the rooms of the building's program (shop, house, apartment, office, hotel, warehouse, police, mansion, bar, garage,
 * church, or a generic fallback) by area, with corridors and units on apartment and office floors. Doors join the rooms
 * along a cheapest spanning tree from the entry (ground floor) or the core (upper floors). Out come a soup per room (floor,
 * ceiling, the room's side of every wall with the exterior openings cut through, skirting, sills), one for each core
 * (stairs, landings, lift shaft), and slots: interior doors, ceiling lamps, furnishings from room presets, the lift car.
 *
 * Ids are stable while the layout holds: rooms "l<floor>/<name>" (names from the program's template: "sales",
 * "bedroom2", "u3-living"), doors "l<floor>/door/<nameA>+<nameB>", parts as room ids plus "core0". Pure, any thread.
 */
namespace buildings
{
/** The character the navigation checks hold interiors to: Unreal's default character by default. */
struct CharacterProfile
{
    double height = 180.0;  // capsule height, cm
    double radius = 35.0;   // capsule radius, cm
    double maxStep = 45.0;  // highest step it walks up, cm
    double maxSlope = 44.0; // steepest walkable slope, degrees
    json toJson() const;
    static CharacterProfile fromJson(const json& j);
};

/** The user's layout edits of one building's interior, kept in its edits under "interior". */
struct InteriorEdits
{
    std::string program;
    std::map<std::string, std::string> kinds; // room id -> kind
    struct Split
    {
        std::string axis = "auto"; // auto (across the longer side), u, v
        double at = 0.5;
        std::string kind;
    };
    std::map<std::string, Split> splits;                   // room id -> split; the new room is "<id>-split"
    std::vector<std::pair<std::string, std::string>> merges; // {into, from}
    struct Move
    {
        std::string room, toward;
        double by = 0.0; // cm the wall between moves into `toward` (negative: into `room`)
    };
    std::vector<Move> moves;
    struct DoorAdd
    {
        std::string a, b;
        std::optional<Vec2> at;
        double width = 0.0;
    };
    std::vector<DoorAdd> addDoors;
    std::set<std::string> removeDoors;
    bool furnish = true;

    bool empty() const;
    json toJson() const;
    static InteriorEdits fromJson(const json& j);
};

struct Room
{
    std::string id, kind, unit;
    int tier = 0, floor = 0;
    double y = 0.0, height = 0.0, area = 0.0; // area m2
    Vec2 label{0.0};                           // a point inside, world xz
    std::vector<std::array<Vec2, 4>> rects;     // the room's floor as rectangles, world xz
    std::vector<double> rectY;                  // each rectangle's floor level (a stepped ground floor has terraces); y is the lowest
    bool entry = false, reachable = false, user = false;
    std::vector<std::string> doors;
};

struct InteriorDoor
{
    std::string id, a, b; // room ids; "core0@l2" for a core on a floor
    std::string slot;     // the slot path ("l0/door/backroom+sales")
    Vec3 origin{0.0}, normal{0.0}; // bottom centre on the wall's middle plane; normal from a to b
    double width = 0.0, height = 0.0, thickness = 0.0;
    int floor = 0;
    bool user = false;
    bool open = false; // an opening with no leaf (no slot): a compact stair's landing face
    bool up = false;   // the foot of a straight flight, on a floor the flight below also arrives at
};

struct Stair
{
    std::string id, core; // "core0/l2", "core0"
    std::string kind = "u"; // u (two flights, landings in the core), dogleg (landings in the rooms), straight (one flight a floor)
    int floor = 0;
    double y0 = 0.0, y1 = 0.0;
    int risers = 0;
    double riser = 0.0, going = 0.0, slope = 0.0, width = 0.0, headroom = 0.0;
    bool straight = false;
};

/** The cells of one floor: what the room tool picks and what reachability walks. */
struct FloorGrid
{
    int tier = 0, floor = 0;
    double y = 0.0, height = 0.0;
    Vec2 origin{0.0}, ax{1.0, 0.0}, az{0.0, 1.0};
    double u0 = 0.0, v0 = 0.0, cell = 20.0;
    int nx = 0, nz = 0;
    std::vector<int> room; // room index, -1 none, -2 core
    std::vector<double> level; // per cell floor level on a stepped ground floor; empty when the floor is flat at y
    std::string prefix;    // "l2", or "t1/l2" when two plates share a floor
};

struct InteriorPart
{
    std::string id; // room id, "core0", or "l2/ceiling" (a floor's ceilings)
    int floor = -1;
    Soup soup;
};

struct Interior
{
    std::string building, program, programFrom;
    bool generic = false;
    std::vector<Room> rooms;
    std::vector<InteriorDoor> doors;
    std::vector<Stair> stairs;
    std::vector<InteriorPart> parts;
    std::vector<Module> slots;          // paths under the interior's owner
    std::vector<FloorGrid> grids;
    std::vector<std::string> entries;   // exterior door module paths ("t0/f1/l0/b2/door") that open into rooms
    std::vector<std::string> warnings, notes;
    /** Edits naming rooms or doors that are not there any more. */
    std::vector<std::string> orphanEdits;
    /** The room under a point (index into rooms), or -1; core cells give -2. */
    int roomAt(Vec3 p) const;
    /** The floor level under a point (a stepped ground floor's terrace), or nothing off every floor. */
    std::optional<double> floorAt(Vec3 p) const;
    const Room* room(const std::string& id) const;
};

/** Material slots interiors use. */
enum IntMat : int32_t
{
    IntWall,
    IntCeiling,
    IntFloorWood,
    IntFloorTile,
    IntFloorConcrete,
    IntFloorStone,
    IntWallTile,
    IntTrim,
    IntStair,
    IntShaft,
    IntCount
};
const char* intMatName(int m);
glm::vec3 intMatColour(int m);

const std::vector<std::string>& interiorPrograms();
/** Room kinds a room can be given, in menu order. */
const std::vector<std::string>& roomKinds();
/** The interior program of a building: forced, else from its program, use and style. */
std::string interiorProgramFor(const Spec& s, const Style& st, const std::string& forced, std::string* from = nullptr);
/** What a fake interior shows behind a window: the room kind a floor of the program mostly has. */
std::string fakeRoomKind(const std::string& program, int floor);

/** Furnishing slot types (furnishing.bed, furnishing.counter...) and their footprints, cm. */
const std::map<std::string, Vec3>& furnishingTypes();

/** Room presets (furnishing per room kind): the shipped ones with the user's merged over them by kind. */
const json& shippedRoomPresets();
void setUserRoomPresets(const json& presets);
json roomPresets();

/** Furnishing and a ceiling lamp for a floor area: rectangles in world xz at height y, ceiling at ceilingY, doors kept clear. */
std::vector<Module> furnishArea(const std::string& kind, const std::vector<std::array<Vec2, 4>>& rects, double y, double ceilingY,
                                const std::vector<Vec3>& keepClear, const std::string& pathPrefix, uint64_t seed);

std::shared_ptr<const Interior> generateInterior(const Spec& spec, const Result& r, const Style& st, const json& edits, const CharacterProfile& profile);

/** Issues of an interior: unreachable rooms, stairs too steep or low for the profile, doors narrower or lower than it. */
json checkInterior(const Interior& in, const CharacterProfile& p);
} // namespace buildings
