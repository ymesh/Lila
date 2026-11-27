// Interiors: each floor plate laid on a grid in the building's frame, a stair core fitted through the floors, rooms split
// from the plate by the program's template (corridors and units where the program has them), doors along a cheapest
// spanning tree from the entry or the core, then each room's soup and the interior's slots. Pure, any thread.

#include "Interior.h"

#include <lila/Profile.h>

#include <algorithm>
#include <cmath>
#include <deque>
#include <mutex>
#include <numbers>
#include <unordered_map>

namespace buildings
{
// Profile and edits ----------------------------------------------------------------------------------------------------

json CharacterProfile::toJson() const { return {{"height", height}, {"radius", radius}, {"maxStep", maxStep}, {"maxSlope", maxSlope}}; }

CharacterProfile CharacterProfile::fromJson(const json& j)
{
    CharacterProfile p;
    if (!j.is_object())
        return p;
    auto num = [&](const char* k, double& v, double lo, double hi) {
        if (j.contains(k) && j[k].is_number())
            v = std::clamp(j[k].get<double>(), lo, hi);
    };
    num("height", p.height, 30.0, 1000.0);
    num("radius", p.radius, 5.0, 300.0);
    num("maxStep", p.maxStep, 1.0, 200.0);
    num("maxSlope", p.maxSlope, 5.0, 85.0);
    return p;
}

bool InteriorEdits::empty() const
{
    return program.empty() && kinds.empty() && splits.empty() && merges.empty() && moves.empty() && addDoors.empty() && removeDoors.empty() && furnish;
}

json InteriorEdits::toJson() const
{
    json j = json::object();
    if (!program.empty())
        j["program"] = program;
    if (!kinds.empty())
        j["kinds"] = kinds;
    if (!splits.empty())
    {
        json s = json::object();
        for (const auto& [id, sp] : splits)
        {
            s[id] = {{"axis", sp.axis}, {"at", sp.at}};
            if (!sp.kind.empty())
                s[id]["kind"] = sp.kind;
        }
        j["splits"] = s;
    }
    if (!merges.empty())
    {
        json m = json::array();
        for (const auto& [a, b] : merges)
            m.push_back({a, b});
        j["merges"] = m;
    }
    if (!moves.empty())
    {
        json m = json::array();
        for (const Move& mv : moves)
            m.push_back({{"room", mv.room}, {"toward", mv.toward}, {"by", mv.by}});
        j["moves"] = m;
    }
    if (!addDoors.empty())
    {
        json d = json::array();
        for (const DoorAdd& a : addDoors)
        {
            json e = {{"a", a.a}, {"b", a.b}};
            if (a.at)
                e["at"] = {a.at->x, a.at->y};
            if (a.width > 0.0)
                e["width"] = a.width;
            d.push_back(e);
        }
        j["addDoors"] = d;
    }
    if (!removeDoors.empty())
        j["removeDoors"] = removeDoors;
    if (!furnish)
        j["furnish"] = false;
    return j;
}

InteriorEdits InteriorEdits::fromJson(const json& j)
{
    InteriorEdits e;
    if (!j.is_object())
        return e;
    e.program = j.value("program", std::string());
    if (j.contains("kinds") && j["kinds"].is_object())
        for (auto& [k, v] : j["kinds"].items())
            if (v.is_string())
                e.kinds[k] = v.get<std::string>();
    if (j.contains("splits") && j["splits"].is_object())
        for (auto& [k, v] : j["splits"].items())
        {
            Split s;
            if (v.is_object())
            {
                s.axis = v.value("axis", std::string("auto"));
                s.at = std::clamp(v.value("at", 0.5), 0.05, 0.95);
                s.kind = v.value("kind", std::string());
            }
            e.splits[k] = s;
        }
    if (j.contains("merges") && j["merges"].is_object())
        for (auto& [into, from] : j["merges"].items())
            if (from.is_string())
                e.merges.push_back({into, from.get<std::string>()});
    for (const json& m : j.value("merges", json::array()))
        if (m.is_array() && m.size() == 2 && m[0].is_string() && m[1].is_string())
            e.merges.push_back({m[0].get<std::string>(), m[1].get<std::string>()});
    for (const json& m : j.value("moves", json::array()))
        if (m.is_object())
            e.moves.push_back({m.value("room", std::string()), m.value("toward", std::string()), m.value("by", 0.0)});
    for (const json& d : j.value("addDoors", json::array()))
        if (d.is_object())
        {
            DoorAdd a;
            a.a = d.value("a", std::string());
            a.b = d.value("b", std::string());
            if (d.contains("at") && d["at"].is_array() && d["at"].size() >= 2)
                a.at = Vec2(d["at"][0].get<double>(), d["at"][d["at"].size() - 1].get<double>());
            a.width = d.value("width", 0.0);
            e.addDoors.push_back(a);
        }
    for (const json& d : j.value("removeDoors", json::array()))
        if (d.is_string())
            e.removeDoors.insert(d.get<std::string>());
    e.furnish = j.value("furnish", true);
    return e;
}

// Programs, kinds, materials --------------------------------------------------------------------------------------------

const std::vector<std::string>& interiorPrograms()
{
    static const std::vector<std::string> p = {"shop", "house", "apartment", "office", "hotel", "warehouse", "police",
                                               "mansion", "bar", "garage", "church", "generic"};
    return p;
}

const std::vector<std::string>& roomKinds()
{
    static const std::vector<std::string> k = {"living", "kitchen", "dining", "bedroom", "bathroom", "toilet", "hall", "corridor", "lobby",
                                               "sales", "backroom", "storage", "office", "meeting", "openfloor", "workshop", "bar", "nave",
                                               "sacristy", "cell", "interview", "armory", "briefing", "locker", "study", "salon", "utility", "room"};
    return k;
}

namespace
{
std::string lower(std::string s)
{
    for (char& ch : s)
        ch = char(std::tolower(static_cast<unsigned char>(ch)));
    return s;
}

bool circulation(const std::string& k)
{
    return k == "hall" || k == "corridor" || k == "lobby" || k == "living" || k == "sales" || k == "bar" || k == "openfloor" || k == "workshop" ||
           k == "nave" || k == "dining" || k == "salon";
}

bool privateRoom(const std::string& k) { return k == "bedroom" || k == "bathroom" || k == "toilet" || k == "cell"; }

bool publicRoom(const std::string& k) { return k == "sales" || k == "nave" || k == "lobby" || k == "bar" || k == "openfloor" || k == "workshop"; }

int floorMat(const std::string& k)
{
    if (k == "bathroom" || k == "toilet" || k == "kitchen")
        return IntFloorTile;
    if (k == "openfloor" || k == "workshop" || k == "storage" || k == "backroom" || k == "cell" || k == "armory" || k == "locker" || k == "utility")
        return IntFloorConcrete;
    if (k == "nave" || k == "lobby" || k == "sacristy" || k == "hall" || k == "salon")
        return IntFloorStone;
    return IntFloorWood;
}
} // namespace

const char* intMatName(int m)
{
    static const char* k[IntCount] = {"bld_int_wall", "bld_int_ceiling", "bld_int_wood", "bld_int_tile", "bld_int_concrete",
                                      "bld_int_stone", "bld_int_wallTile", "bld_int_trim", "bld_int_stair", "bld_int_shaft"};
    return m >= 0 && m < IntCount ? k[m] : k[0];
}

glm::vec3 intMatColour(int m)
{
    static const glm::vec3 k[IntCount] = {{0.78f, 0.75f, 0.68f}, {0.85f, 0.84f, 0.8f}, {0.42f, 0.27f, 0.15f}, {0.7f, 0.72f, 0.72f},
                                          {0.45f, 0.44f, 0.42f}, {0.62f, 0.58f, 0.52f}, {0.82f, 0.86f, 0.86f}, {0.92f, 0.9f, 0.85f},
                                          {0.55f, 0.52f, 0.48f}, {0.4f, 0.4f, 0.42f}};
    return m >= 0 && m < IntCount ? k[m] : k[0];
}

std::string interiorProgramFor(const Spec& s, const Style& st, const std::string& forced, std::string* from)
{
    auto set = [&](const char* f) {
        if (from)
            *from = f;
    };
    if (!forced.empty() && std::find(interiorPrograms().begin(), interiorPrograms().end(), forced) != interiorPrograms().end())
    {
        set("given");
        return forced;
    }
    const std::string p = lower(s.program), g = lower(s.groundUse), u = lower(s.use), sid = st.id;
    auto is = [&](std::initializer_list<const char*> ks) {
        for (const char* k : ks)
            if (p == k || g == k)
                return true;
        return false;
    };
    // By id or tag, so workspace styles tagged like a shipped one ("mansion", "village") read the same.
    auto styleIs = [&](std::initializer_list<const char*> ks) {
        for (const char* k : ks)
            if (sid == k || std::find(st.tags.begin(), st.tags.end(), k) != st.tags.end())
                return true;
        return false;
    };
    set("program");
    if (sid == "church" || sid == "mausoleum" || is({"church", "chapel", "place_of_worship", "cathedral", "mausoleum", "temple"}))
        return "church";
    if (sid == "policeStation" || is({"police", "police_station"}))
        return "police";
    if (styleIs({"mansion"}) || is({"mansion", "villa"}))
        return "mansion";
    if (is({"bar", "pub", "cantina", "nightclub", "restaurant", "cafe", "fast_food"}))
        return "bar";
    if (is({"garage", "car_repair", "workshop", "car_wash", "fuel"}))
        return "garage";
    if (sid == "warehouse" || sid == "packingPlant" || sid == "portShed" || sid == "hangar" || is({"warehouse", "industrial", "factory", "storage"}) ||
        u == "industrial")
        return "warehouse";
    if (sid == "resortTower" || is({"hotel", "hostel", "motel"}))
        return "hotel";
    if (sid == "civic" || is({"office", "bank", "townhall", "government", "civic", "school", "hospital", "clinic"}) || u == "office" || u == "civic")
        return "office";
    if (isBusiness(s))
        return "shop";
    const bool residential = u == "residential" || is({"residential", "house", "apartments", "apartment", "detached", "terrace"}) ||
                             (p.empty() && u.empty() && styleIs({"residential", "colonial", "adobe", "village", "ruralShack", "seafront"}));
    if (residential)
        return s.floors >= 3 ? "apartment" : "house";
    if (p.empty() && u.empty() && sid == "modernCommercial")
        return "office";
    set("fallback");
    return "generic";
}

std::string fakeRoomKind(const std::string& program, int floor)
{
    const bool g = floor == 0;
    if (program == "shop")
        return g ? "sales" : "living";
    if (program == "house")
        return g ? "living" : "bedroom";
    if (program == "apartment")
        return g ? "lobby" : "living";
    if (program == "office")
        return "office";
    if (program == "hotel")
        return g ? "lobby" : "bedroom";
    if (program == "warehouse")
        return "openfloor";
    if (program == "police")
        return "office";
    if (program == "mansion")
        return g ? "salon" : "bedroom";
    if (program == "bar")
        return g ? "bar" : "living";
    if (program == "garage")
        return "workshop";
    if (program == "church")
        return "nave";
    return "room";
}

// Room presets ----------------------------------------------------------------------------------------------------------

namespace
{
/** Furnishing slot types: id, footprint (w, h, d). Kept here so presets size their pieces like the registered types. */
const std::map<std::string, Vec3>& furnishingSizes()
{
    static const std::map<std::string, Vec3> k = {
        {"furnishing.sofa", {200, 85, 90}},     {"furnishing.armchair", {90, 85, 85}}, {"furnishing.table", {120, 75, 80}},
        {"furnishing.diningTable", {200, 75, 100}}, {"furnishing.chair", {45, 90, 50}}, {"furnishing.shelf", {120, 190, 40}},
        {"furnishing.counter", {240, 92, 62}},  {"furnishing.stove", {60, 92, 62}},   {"furnishing.fridge", {70, 185, 70}},
        {"furnishing.bed", {160, 55, 205}},     {"furnishing.bedSingle", {95, 55, 200}}, {"furnishing.wardrobe", {120, 200, 60}},
        {"furnishing.nightstand", {45, 55, 40}}, {"furnishing.toilet", {40, 78, 68}}, {"furnishing.sink", {60, 88, 46}},
        {"furnishing.shower", {90, 210, 90}},   {"furnishing.bathtub", {170, 60, 75}}, {"furnishing.desk", {150, 75, 75}},
        {"furnishing.cabinet", {90, 120, 45}},  {"furnishing.rack", {270, 400, 110}}, {"furnishing.pallet", {120, 120, 100}},
        {"furnishing.workbench", {200, 92, 75}}, {"furnishing.bartop", {400, 110, 70}}, {"furnishing.stool", {40, 75, 40}},
        {"furnishing.pew", {350, 95, 65}},      {"furnishing.altar", {220, 105, 90}}, {"furnishing.bunk", {85, 60, 200}},
        {"furnishing.locker", {120, 190, 50}},  {"furnishing.bench", {180, 45, 40}},  {"furnishing.reception", {250, 110, 80}},
        {"furnishing.tv", {150, 55, 45}},       {"furnishing.crate", {100, 90, 100}}, {"furnishing.carLift", {320, 20, 520}},
        {"furnishing.register", {60, 100, 50}},
    };
    return k;
}

std::mutex gPresetMutex;
json gUserPresets = json::object();
} // namespace

const std::map<std::string, Vec3>& furnishingTypes() { return furnishingSizes(); }

const json& shippedRoomPresets()
{
    // place: wall (against a wall, facing in), corner, centre, row (rows down the long axis, an aisle in the middle), grid.
    static const json p = json::parse(R"({
 "living": {"pieces": [{"type": "furnishing.sofa", "place": "wall"}, {"type": "furnishing.tv", "place": "wall"}, {"type": "furnishing.table", "place": "centre"},
                       {"type": "furnishing.armchair", "place": "corner"}, {"type": "furnishing.shelf", "place": "wall", "minArea": 14}]},
 "salon": {"pieces": [{"type": "furnishing.sofa", "place": "wall"}, {"type": "furnishing.sofa", "place": "wall"}, {"type": "furnishing.table", "place": "centre"},
                      {"type": "furnishing.armchair", "place": "corner"}, {"type": "furnishing.armchair", "place": "corner"}, {"type": "furnishing.shelf", "place": "wall"}]},
 "kitchen": {"pieces": [{"type": "furnishing.counter", "place": "wall"}, {"type": "furnishing.stove", "place": "wall"}, {"type": "furnishing.fridge", "place": "wall"},
                        {"type": "furnishing.table", "place": "centre", "minArea": 9}]},
 "dining": {"pieces": [{"type": "furnishing.diningTable", "place": "centre"}, {"type": "furnishing.cabinet", "place": "wall"}]},
 "bedroom": {"pieces": [{"type": "furnishing.bed", "place": "wall"}, {"type": "furnishing.wardrobe", "place": "wall"}, {"type": "furnishing.nightstand", "place": "wall"},
                        {"type": "furnishing.desk", "place": "wall", "minArea": 12}]},
 "bathroom": {"pieces": [{"type": "furnishing.toilet", "place": "wall"}, {"type": "furnishing.sink", "place": "wall"}, {"type": "furnishing.shower", "place": "corner"},
                         {"type": "furnishing.bathtub", "place": "wall", "minArea": 7}]},
 "toilet": {"pieces": [{"type": "furnishing.toilet", "place": "wall"}, {"type": "furnishing.sink", "place": "wall"}]},
 "hall": {"pieces": [{"type": "furnishing.cabinet", "place": "wall", "minArea": 6}]},
 "corridor": {"pieces": [], "light": true},
 "lobby": {"pieces": [{"type": "furnishing.reception", "place": "wall"}, {"type": "furnishing.bench", "place": "wall"}, {"type": "furnishing.bench", "place": "wall"}]},
 "sales": {"pieces": [{"type": "furnishing.counter", "place": "wall"}, {"type": "furnishing.register", "place": "wall"}, {"type": "furnishing.shelf", "place": "wall"},
                      {"type": "furnishing.shelf", "place": "wall"}, {"type": "furnishing.shelf", "place": "wall"}, {"type": "furnishing.table", "place": "centre", "minArea": 20}]},
 "backroom": {"pieces": [{"type": "furnishing.shelf", "place": "wall"}, {"type": "furnishing.shelf", "place": "wall"}, {"type": "furnishing.crate", "place": "corner"},
                         {"type": "furnishing.crate", "place": "corner"}], "arrangement": "Storeroom"},
 "storage": {"pieces": [{"type": "furnishing.shelf", "place": "wall"}, {"type": "furnishing.shelf", "place": "wall"}, {"type": "furnishing.crate", "place": "corner"},
                        {"type": "furnishing.pallet", "place": "corner"}], "arrangement": "Storeroom"},
 "utility": {"pieces": [{"type": "furnishing.shelf", "place": "wall"}]},
 "office": {"pieces": [{"type": "furnishing.desk", "place": "wall"}, {"type": "furnishing.chair", "place": "centre"}, {"type": "furnishing.cabinet", "place": "wall"},
                       {"type": "furnishing.desk", "place": "wall", "minArea": 16}]},
 "meeting": {"pieces": [{"type": "furnishing.diningTable", "place": "centre"}, {"type": "furnishing.cabinet", "place": "wall"}]},
 "study": {"pieces": [{"type": "furnishing.desk", "place": "wall"}, {"type": "furnishing.shelf", "place": "wall"}, {"type": "furnishing.armchair", "place": "corner"}]},
 "openfloor": {"pieces": [{"type": "furnishing.rack", "place": "row"}, {"type": "furnishing.pallet", "place": "corner"}, {"type": "furnishing.pallet", "place": "corner"}],
               "arrangement": "Storeroom"},
 "workshop": {"pieces": [{"type": "furnishing.carLift", "place": "centre"}, {"type": "furnishing.workbench", "place": "wall"}, {"type": "furnishing.cabinet", "place": "wall"},
                         {"type": "furnishing.shelf", "place": "wall"}]},
 "bar": {"pieces": [{"type": "furnishing.bartop", "place": "wall"}, {"type": "furnishing.shelf", "place": "wall"}, {"type": "furnishing.table", "place": "grid"}]},
 "nave": {"pieces": [{"type": "furnishing.altar", "place": "wall"}, {"type": "furnishing.pew", "place": "row"}]},
 "sacristy": {"pieces": [{"type": "furnishing.cabinet", "place": "wall"}, {"type": "furnishing.table", "place": "centre"}]},
 "cell": {"pieces": [{"type": "furnishing.bunk", "place": "wall"}, {"type": "furnishing.toilet", "place": "corner"}]},
 "interview": {"pieces": [{"type": "furnishing.table", "place": "centre"}, {"type": "furnishing.chair", "place": "wall"}]},
 "armory": {"pieces": [{"type": "furnishing.locker", "place": "wall"}, {"type": "furnishing.locker", "place": "wall"}, {"type": "furnishing.cabinet", "place": "wall"}]},
 "briefing": {"pieces": [{"type": "furnishing.diningTable", "place": "centre"}, {"type": "furnishing.chair", "place": "row"}]},
 "locker": {"pieces": [{"type": "furnishing.locker", "place": "wall"}, {"type": "furnishing.locker", "place": "wall"}, {"type": "furnishing.bench", "place": "centre"}]},
 "room": {"pieces": [{"type": "furnishing.table", "place": "centre", "minArea": 8}, {"type": "furnishing.shelf", "place": "wall"}]}
})");
    return p;
}

void setUserRoomPresets(const json& presets)
{
    std::lock_guard lock(gPresetMutex);
    gUserPresets = presets.is_object() ? presets : json::object();
}

json roomPresets()
{
    json out = shippedRoomPresets();
    std::lock_guard lock(gPresetMutex);
    for (auto& [k, v] : gUserPresets.items())
        out[k] = v;
    return out;
}

namespace
{
struct LRect
{
    double x0, z0, x1, z1;
    bool overlaps(const LRect& o) const { return x0 < o.x1 - 0.5 && o.x0 < x1 - 0.5 && z0 < o.z1 - 0.5 && o.z0 < z1 - 0.5; }
};
} // namespace

std::vector<Module> furnishArea(const std::string& kind, const std::vector<std::array<Vec2, 4>>& rects, double y, double ceilingY,
                                const std::vector<Vec3>& keepClear, const std::string& pathPrefix, uint64_t seed)
{
    std::vector<Module> out;
    if (rects.empty())
        return out;
    // The largest rectangle carries the furniture.
    size_t best = 0;
    double bestA = -1.0;
    for (size_t k = 0; k < rects.size(); ++k)
    {
        const double a = glm::length(rects[k][1] - rects[k][0]) * glm::length(rects[k][3] - rects[k][0]);
        if (a > bestA)
        {
            bestA = a;
            best = k;
        }
    }
    const auto& R = rects[best];
    const Vec2 o = R[0];
    Vec2 ex = R[1] - R[0], ez = R[3] - R[0];
    const double W = glm::length(ex), D = glm::length(ez);
    if (W < 60.0 || D < 60.0)
        return out;
    ex /= W;
    ez /= D;
    const double margin = 30.0; // the rectangles reach into the walls by up to half their thickness
    const double x0 = margin, z0 = margin, x1 = W - margin, z1 = D - margin;
    const double area = bestA / 1e4;
    auto world = [&](double x, double z) { return o + ex * x + ez * z; };
    const Vec3 up{0.0, 1.0, 0.0};
    auto frame = [&](Module& m, double x, double z, Vec2 facing) {
        const Vec2 w = world(x, z);
        m.origin = {w.x, y, w.y};
        const Vec2 f = facing.x * ex + facing.y * ez;
        m.z = glm::normalize(Vec3(f.x, 0.0, f.y));
        m.y = up;
        m.x = glm::cross(m.y, m.z);
    };
    std::vector<LRect> used;
    for (const Vec3& k : keepClear)
    {
        const Vec2 d = Vec2(k.x, k.z) - o;
        const double kx = glm::dot(d, ex), kz = glm::dot(d, ez);
        if (kx > -80.0 && kx < W + 80.0 && kz > -80.0 && kz < D + 80.0)
            used.push_back({kx - 70.0, kz - 70.0, kx + 70.0, kz + 70.0});
    }
    auto fits = [&](const LRect& r) {
        if (r.x0 < x0 - 0.5 || r.z0 < z0 - 0.5 || r.x1 > x1 + 0.5 || r.z1 > z1 + 0.5)
            return false;
        for (const LRect& u : used)
            if (u.overlaps(r))
                return false;
        return true;
    };
    const json presets = roomPresets();
    const json preset = presets.contains(kind) ? presets[kind] : presets.value("room", json::object());
    std::map<std::string, int> counts;
    auto add = [&](const std::string& type, const Vec3& size, double x, double z, Vec2 facing, const LRect& box) {
        Module m;
        m.type = type;
        m.kind = "furnishing";
        const std::string leaf = type.substr(type.find('.') + 1);
        const int n = counts[leaf]++;
        m.path = pathPrefix + "/" + leaf + (n ? std::to_string(n + 1) : std::string());
        m.size = size;
        frame(m, x, z, facing);
        used.push_back(box);
        out.push_back(std::move(m));
    };
    for (const json& piece : preset.value("pieces", json::array()))
    {
        const std::string type = piece.value("type", std::string());
        auto it = furnishingSizes().find(type);
        if (it == furnishingSizes().end())
            continue;
        if (area < piece.value("minArea", 0.0))
            continue;
        const Vec3 s = it->second;
        const std::string place = piece.value("place", std::string("wall"));
        if (place == "wall")
        {
            // Sides, longest first: front (z0, facing +z), back (z1, facing -z), left (x0, +x), right (x1, -x).
            struct Side
            {
                int id;
                double len;
            };
            std::vector<Side> sides = {{0, W}, {1, W}, {2, D}, {3, D}};
            std::stable_sort(sides.begin(), sides.end(), [](const Side& a, const Side& b) { return a.len > b.len; });
            const int rot = int((seed + counts.size()) % 4);
            std::rotate(sides.begin(), sides.begin() + (rot % 2), sides.end());
            bool done = false;
            for (const Side& sd : sides)
            {
                const double span = sd.id < 2 ? x1 - x0 : z1 - z0;
                for (double t = 0.0; t + s.x <= span + 0.5 && !done; t += 10.0)
                {
                    LRect b{};
                    double cx = 0, cz = 0;
                    Vec2 face{0.0, 1.0};
                    switch (sd.id)
                    {
                    case 0:
                        b = {x0 + t, z0, x0 + t + s.x, z0 + s.z};
                        cx = x0 + t + s.x * 0.5, cz = z0, face = {0, 1};
                        break;
                    case 1:
                        b = {x1 - t - s.x, z1 - s.z, x1 - t, z1};
                        cx = x1 - t - s.x * 0.5, cz = z1, face = {0, -1};
                        break;
                    case 2:
                        b = {x0, z1 - t - s.x, x0 + s.z, z1 - t};
                        cx = x0, cz = z1 - t - s.x * 0.5, face = {1, 0};
                        break;
                    default:
                        b = {x1 - s.z, z0 + t, x1, z0 + t + s.x};
                        cx = x1, cz = z0 + t + s.x * 0.5, face = {-1, 0};
                        break;
                    }
                    if (fits(b))
                    {
                        add(type, s, cx, cz, face, b);
                        done = true;
                    }
                }
                if (done)
                    break;
            }
        }
        else if (place == "corner")
        {
            const double cx[4] = {x0, x1 - s.x, x1 - s.x, x0}, cz[4] = {z0, z0, z1 - s.z, z1 - s.z};
            for (int k = 0; k < 4; ++k)
            {
                const LRect b{cx[k], cz[k], cx[k] + s.x, cz[k] + s.z};
                if (fits(b))
                {
                    add(type, s, cx[k] + s.x * 0.5, cz[k] + (k < 2 ? 0.0 : s.z), k < 2 ? Vec2(0, 1) : Vec2(0, -1), b);
                    break;
                }
            }
        }
        else if (place == "centre")
        {
            const bool alongX = W >= D;
            const double w = alongX ? s.x : s.z, d = alongX ? s.z : s.x;
            const LRect b{W * 0.5 - w * 0.5, D * 0.5 - d * 0.5, W * 0.5 + w * 0.5, D * 0.5 + d * 0.5};
            if (fits(b))
                add(type, s, W * 0.5, D * 0.5 - d * 0.5, alongX ? Vec2(0, 1) : Vec2(1, 0), b);
        }
        else if (place == "row" || place == "grid")
        {
            // Rows along the long side, an aisle down the middle, the far quarter left free (altar, counter).
            const bool alongX = W >= D;
            const double len = alongX ? x1 - x0 : z1 - z0, across = alongX ? z1 - z0 : x1 - x0;
            const double pitch = place == "row" ? s.z + 55.0 : s.z + 110.0;
            const double gap = place == "row" ? 120.0 : 110.0;
            const int perRow = place == "row" ? (across >= 2 * s.x + gap ? 2 : across >= s.x ? 1 : 0) : std::max(0, int((across + 110.0) / (s.x + 110.0)));
            const double usable = place == "row" ? len * 0.72 : len;
            for (double t = 30.0; t + s.z <= usable && perRow > 0 && out.size() < 400; t += pitch)
                for (int k = 0; k < perRow; ++k)
                {
                    double c0 = 0.0;
                    if (place == "row")
                        c0 = perRow == 2 ? (k == 0 ? 0.0 : across - s.x) : (across - s.x) * 0.5;
                    else
                        c0 = (across - (perRow * s.x + (perRow - 1) * 110.0)) * 0.5 + k * (s.x + 110.0);
                    LRect b = alongX ? LRect{x0 + t, z0 + c0, x0 + t + s.z, z0 + c0 + s.x} : LRect{x0 + c0, z0 + t, x0 + c0 + s.x, z0 + t + s.z};
                    if (!fits(b))
                        continue;
                    if (alongX)
                        add(type, s, b.x0, (b.z0 + b.z1) * 0.5, Vec2(-1, 0), b);
                    else
                        add(type, s, (b.x0 + b.x1) * 0.5, b.z0, Vec2(0, -1), b);
                }
        }
    }
    if (preset.value("light", true) && ceilingY > y + 150.0)
    {
        Module m;
        m.type = "lamp.ceiling";
        m.kind = "lamp";
        m.path = pathPrefix + "/lamp";
        m.size = {40, 30, 40};
        const Vec2 c = world(W * 0.5, D * 0.5);
        m.origin = {c.x, ceilingY, c.y};
        m.y = {0.0, -1.0, 0.0};
        m.z = Vec3(ez.x, 0.0, ez.y);
        m.x = glm::cross(m.y, m.z);
        out.push_back(std::move(m));
    }
    return out;
}

// The generator -------------------------------------------------------------------------------------------------------------

namespace
{
constexpr int kNone = -1, kCoreCell = -2;
constexpr double kSlab = 25.0;  // floor build-up between a ceiling and the floor above
constexpr double kWall = 12.0;  // interior partition thickness
constexpr double kDoorRiser = 18.0, kDoorGoing = 28.0; // steps at a door between rooms at different levels
constexpr double kSkirt = 10.0; // skirting height
const Vec3 kIntUp{0.0, 1.0, 0.0};
const glm::vec4 kTint{1.0f, 1.0f, 1.0f, 0.0f};

enum class Role
{
    Single,
    Ground,
    Upper,
    Volume
};

struct Tpl
{
    std::string name, kind;
    double w = 0.0, fixed = 0.0, minArea = 0.0;
    int drop = 0; // dropped when the plate is short, highest first; 0 never
    bool hub = false;
};

struct Layout
{
    std::vector<Tpl> rooms;
    bool corridor = false;
    std::vector<Tpl> specials, unit;
    double unitArea = 0.0;
    std::string unitKind;
};

Tpl T(const char* name, const char* kind, double w, double fixed = 0.0, int drop = 0, bool hub = false, double minArea = 0.0)
{
    return {name, kind, w, fixed, minArea, drop, hub};
}

Layout layoutFor(const std::string& program, Role role, double area)
{
    Layout L;
    const bool ground = role == Role::Ground, single = role == Role::Single, upper = role == Role::Upper;
    auto houseUpper = [&] {
        L.rooms = {T("hall", "hall", 0.14, 0, 0, true), T("bedroom", "bedroom", 0.4), T("bedroom2", "bedroom", 0.28, 0, 3, false, 9),
                   T("bedroom3", "bedroom", 0.18, 0, 4, false, 9), T("bathroom", "bathroom", 0, 5.5, 1)};
    };
    auto flat = [&] {
        L.rooms = {T("living", "living", 0.4, 0, 0, true), T("kitchen", "kitchen", 0.25, 0, 3), T("bedroom", "bedroom", 0.35, 0, 4),
                   T("bathroom", "bathroom", 0, 5, 2)};
    };
    if (program == "shop")
    {
        if (upper)
            flat();
        else
            L.rooms = {T("sales", "sales", 0.7, 0, 0, true), T("backroom", "backroom", 0.3, 0, 2, false, 6), T("toilet", "toilet", 0, 3.5, 1)};
    }
    else if (program == "house")
    {
        if (single)
            L.rooms = {T("living", "living", 0.38, 0, 0, true), T("kitchen", "kitchen", 0.2, 0, 4), T("bedroom", "bedroom", 0.26, 0, 3),
                       T("bedroom2", "bedroom", 0.16, 0, 5, false, 9), T("bathroom", "bathroom", 0, 5, 2)};
        else if (ground)
            L.rooms = {T("living", "living", 0.5, 0, 0, true), T("kitchen", "kitchen", 0.32, 0, 3), T("dining", "dining", 0.18, 0, 4, false, 8),
                       T("toilet", "toilet", 0, 3, 2)};
        else
            houseUpper();
    }
    else if (program == "apartment" || program == "office" || program == "hotel" || program == "generic")
    {
        const bool office = program == "office", hotel = program == "hotel", generic = program == "generic";
        if (ground && hotel)
            L.rooms = {T("lobby", "lobby", 0.45, 0, 0, true), T("bar", "bar", 0.3, 0, 2), T("office", "office", 0.1, 0, 3), T("storage", "storage", 0.15, 0, 4),
                       T("toilet", "toilet", 0, 4, 1)};
        else if (single || area < 60.0)
        {
            if (office)
                L.rooms = {T("lobby", "lobby", 0.3, 0, 0, true), T("office", "office", 0.25), T("office2", "office", 0.25, 0, 3), T("meeting", "meeting", 0.2, 0, 4),
                           T("toilet", "toilet", 0, 4, 2)};
            else if (hotel)
                L.rooms = {T("lobby", "lobby", 0.5, 0, 0, true), T("bedroom", "bedroom", 0.5), T("bathroom", "bathroom", 0, 4.5, 2)};
            else if (generic)
            {
                L.rooms = {T("hall", "hall", 0.2, 0, 0, true)};
                const int n = std::clamp(int(std::lround(area / 20.0)) - 1, 1, 7);
                for (int k = 0; k < n; ++k)
                    L.rooms.push_back(T(k == 0 ? "room" : ("room" + std::to_string(k + 1)).c_str(), "room", 0.8 / n, 0, k == 0 ? 0 : 10 - k));
            }
            else
                flat();
        }
        else
        {
            L.corridor = true;
            if (office)
            {
                L.specials = ground ? std::vector<Tpl>{T("lobby", "lobby", 0, 30), T("toilet", "toilet", 0, 4), T("toilet2", "toilet", 0, 4)}
                                    : std::vector<Tpl>{T("meeting", "meeting", 0, 25), T("toilet", "toilet", 0, 4)};
                L.unit = {T("office", "office", 1.0, 0, 0, true)};
                L.unitArea = ground ? 25.0 : 20.0;
                L.unitKind = "office";
            }
            else if (hotel)
            {
                L.unit = {T("bedroom", "bedroom", 0.8, 0, 0, true), T("bathroom", "bathroom", 0, 4.5)};
                L.unitArea = 28.0;
            }
            else if (generic)
            {
                L.unit = {T("room", "room", 1.0, 0, 0, true)};
                L.unitArea = 20.0;
                L.unitKind = "room";
            }
            else
            {
                if (ground)
                    L.specials = {T("lobby", "lobby", 0, 25), T("storage", "storage", 0, 10)};
                L.unit = {T("living", "living", 0.45, 0, 0, true), T("kitchen", "kitchen", 0.2, 0, 3), T("bedroom", "bedroom", 0.35), T("bathroom", "bathroom", 0, 4.5)};
                L.unitArea = 55.0;
            }
        }
    }
    else if (program == "warehouse")
    {
        if (upper)
            L.rooms = {T("openfloor", "openfloor", 0.88, 0, 0, true), T("office", "office", 0.12, 0, 2)};
        else
            L.rooms = {T("openfloor", "openfloor", 0.9, 0, 0, true), T("office", "office", 0, std::clamp(area * 0.06, 12.0, 30.0), 2),
                       T("toilet", "toilet", 0, 3.5, 1)};
    }
    else if (program == "police")
    {
        if (upper && area >= 120.0)
        {
            L.corridor = true;
            L.specials = {T("briefing", "briefing", 0, 35), T("locker", "locker", 0, 18), T("toilet", "toilet", 0, 4)};
            L.unit = {T("office", "office", 1.0, 0, 0, true)};
            L.unitArea = 18.0;
            L.unitKind = "office";
        }
        else if (upper)
            L.rooms = {T("briefing", "briefing", 0.5, 0, 0, true), T("office", "office", 0.3), T("locker", "locker", 0.2, 0, 2)};
        else
            L.rooms = {T("lobby", "lobby", 0.2, 0, 0, true), T("office", "office", 0.22), T("briefing", "briefing", 0.14, 0, 3), T("interview", "interview", 0, 9, 6),
                       T("armory", "armory", 0, 8, 4), T("cell", "cell", 0, 7, 0), T("cell2", "cell", 0, 7, 7), T("cell3", "cell", 0, 7, 8),
                       T("toilet", "toilet", 0, 4, 2), T("locker", "locker", 0.1, 0, 5)};
    }
    else if (program == "mansion")
    {
        if (single)
            L.rooms = {T("hall", "hall", 0.14, 0, 0, true), T("salon", "salon", 0.26), T("dining", "dining", 0.14, 0, 5), T("kitchen", "kitchen", 0.13, 0, 3),
                       T("bedroom", "bedroom", 0.18), T("bedroom2", "bedroom", 0.1, 0, 6), T("bathroom", "bathroom", 0, 7, 2), T("toilet", "toilet", 0, 3, 7),
                       T("study", "study", 0.08, 0, 8)};
        else if (ground)
            L.rooms = {T("hall", "hall", 0.14, 0, 0, true), T("salon", "salon", 0.3), T("dining", "dining", 0.17, 0, 4), T("kitchen", "kitchen", 0.15, 0, 3),
                       T("study", "study", 0.12, 0, 5), T("toilet", "toilet", 0, 4, 2), T("storage", "storage", 0.06, 0, 6)};
        else
            L.rooms = {T("hall", "hall", 0.14, 0, 0, true), T("bedroom", "bedroom", 0.32), T("bedroom2", "bedroom", 0.2, 0, 4), T("bedroom3", "bedroom", 0.16, 0, 5),
                       T("bathroom", "bathroom", 0, 8, 2), T("bathroom2", "bathroom", 0, 5.5, 6)};
    }
    else if (program == "bar")
    {
        if (upper)
            flat();
        else
            L.rooms = {T("bar", "bar", 0.7, 0, 0, true), T("kitchen", "kitchen", 0.16, 0, 3), T("storage", "storage", 0.08, 0, 4), T("toilet", "toilet", 0, 3.5, 2),
                       T("toilet2", "toilet", 0, 3.5, 5)};
    }
    else if (program == "garage")
    {
        if (upper)
            L.rooms = {T("office", "office", 0.5, 0, 0, true), T("storage", "storage", 0.5, 0, 2)};
        else
            L.rooms = {T("workshop", "workshop", 0.78, 0, 0, true), T("office", "office", 0.12, 0, 3), T("storage", "storage", 0.1, 0, 4), T("toilet", "toilet", 0, 3.5, 2)};
    }
    else if (program == "church")
        L.rooms = {T("nave", "nave", 0.84, 0, 0, true), T("sacristy", "sacristy", 0.16, 0, 2)};
    if (L.rooms.empty() && !L.corridor)
        L.rooms = {T("room", "room", 1.0, 0, 0, true)};
    return L;
}

/** Drops the template's optional rooms, highest drop first, until what is left fits the area (m2). */
std::vector<Tpl> fitToArea(std::vector<Tpl> rooms, double area)
{
    auto need = [&] {
        double n = 0.0;
        for (const Tpl& t : rooms)
            n += t.fixed > 0.0 ? t.fixed : std::max(t.minArea, 5.0);
        return n;
    };
    while (rooms.size() > 1 && need() > area * 0.92)
    {
        auto it = std::max_element(rooms.begin(), rooms.end(), [](const Tpl& a, const Tpl& b) { return a.drop < b.drop; });
        if (it->drop == 0)
            break;
        rooms.erase(it);
    }
    return rooms;
}

std::vector<double> weightsFor(const std::vector<Tpl>& rooms, double area)
{
    std::vector<double> w(rooms.size(), 0.0);
    double fixedSum = 0.0, flexSum = 0.0;
    for (const Tpl& t : rooms)
    {
        if (t.fixed > 0.0)
            fixedSum += t.fixed;
        else
            flexSum += t.w;
    }
    const double fixedShare = area > 0.0 ? std::min(0.6, fixedSum / area) : 0.0;
    const double scale = fixedSum > 0.0 ? fixedShare / (fixedSum / std::max(area, 1e-9)) : 1.0;
    for (size_t k = 0; k < rooms.size(); ++k)
        w[k] = rooms[k].fixed > 0.0 ? rooms[k].fixed / std::max(area, 1e-9) * scale
                                    : (flexSum > 0.0 ? rooms[k].w / flexSum * (1.0 - fixedShare) : 0.0);
    if (flexSum <= 0.0)
    {
        double s = 0.0;
        for (double v : w)
            s += v;
        for (double& v : w)
            v = s > 0.0 ? v / s : 1.0 / double(w.size());
    }
    return w;
}

Ring insetRing(const Ring& r, const std::vector<double>& d)
{
    const size_t n = r.size();
    Ring out(n);
    double dmax = 1.0;
    for (double v : d)
        dmax = std::max(dmax, std::abs(v));
    for (size_t i = 0; i < n; ++i)
    {
        const size_t ip = (i + n - 1) % n;
        const Vec2 a0 = r[ip], a1 = r[i], b1 = r[(i + 1) % n];
        const Vec2 inP = -outward(a0, a1), inC = -outward(a1, b1);
        const Vec2 dirP = glm::normalize(a1 - a0 + Vec2(1e-12)), dirC = glm::normalize(b1 - a1 + Vec2(1e-12));
        const Vec2 pP = a0 + inP * d[ip], pC = a1 + inC * d[i];
        const double denom = dirP.x * dirC.y - dirP.y * dirC.x;
        Vec2 q;
        if (std::abs(denom) < 1e-6)
            q = a1 + inC * d[i];
        else
        {
            const Vec2 w = pC - pP;
            const double t = (w.x * dirC.y - w.y * dirC.x) / denom;
            q = pP + dirP * t;
        }
        if (glm::length(q - a1) > 4.0 * dmax)
        {
            const Vec2 bis = inP + inC;
            q = a1 + (glm::length(bis) > 1e-9 ? glm::normalize(bis) : inC) * d[i];
        }
        out[i] = q;
    }
    return out;
}

/** A polygon in world space whose winding is fixed to face n, uv projected from the dominant axis (1 m tiles). */
void face(Soup& s, std::vector<Vec3> pts, Vec3 n, int mat)
{
    if (pts.size() < 3)
        return;
    Vec3 nn(0.0);
    for (size_t i = 0; i < pts.size(); ++i)
    {
        const Vec3 a = pts[i], b = pts[(i + 1) % pts.size()];
        nn.x += (a.y - b.y) * (a.z + b.z);
        nn.y += (a.z - b.z) * (a.x + b.x);
        nn.z += (a.x - b.x) * (a.y + b.y);
    }
    if (glm::length(nn) < 1e-6)
        return;
    if (glm::dot(nn, n) < 0.0)
        std::reverse(pts.begin(), pts.end());
    std::vector<glm::vec2> uv(pts.size());
    const Vec3 nu = glm::normalize(nn);
    if (std::abs(nu.y) > 0.7)
        for (size_t i = 0; i < pts.size(); ++i)
            uv[i] = {float(pts[i].x / 100.0), float(pts[i].z / 100.0)};
    else
    {
        Vec3 t = glm::cross(kIntUp, nu);
        t = glm::length(t) > 1e-9 ? glm::normalize(t) : Vec3(1, 0, 0);
        for (size_t i = 0; i < pts.size(); ++i)
            uv[i] = {float(glm::dot(pts[i], t) / 100.0), float(pts[i].y / 100.0)};
    }
    s.poly(pts.data(), uv.data(), pts.size(), mat, TrimNone, -1, PartOther, -1, kTint);
}

void quadF(Soup& s, Vec3 a, Vec3 b, Vec3 c, Vec3 d, Vec3 n, int mat) { face(s, {a, b, c, d}, n, mat); }

struct Hole
{
    double s0, s1, t0, t1;
};

/** A wall face over [s0, s1] x [t0, t1] on the plane through P spanned by U (along) and up, facing N, with holes cut out. */
void wallFace(Soup& s, Vec3 P, Vec3 U, Vec3 N, double s0, double s1, double t0, double t1, std::vector<Hole> holes, int mat)
{
    if (s1 - s0 < 0.5 || t1 - t0 < 0.5)
        return;
    std::vector<Hole> hs;
    for (Hole h : holes)
    {
        h.s0 = std::max(h.s0, s0);
        h.s1 = std::min(h.s1, s1);
        h.t0 = std::max(h.t0, t0);
        h.t1 = std::min(h.t1, t1);
        if (h.s1 - h.s0 > 0.5 && h.t1 - h.t0 > 0.5)
            hs.push_back(h);
    }
    std::sort(hs.begin(), hs.end(), [](const Hole& a, const Hole& b) { return a.s0 < b.s0; });
    std::vector<Hole> merged;
    for (const Hole& h : hs)
    {
        if (!merged.empty() && h.s0 < merged.back().s1)
        {
            merged.back().s1 = std::max(merged.back().s1, h.s1);
            merged.back().t0 = std::min(merged.back().t0, h.t0);
            merged.back().t1 = std::max(merged.back().t1, h.t1);
        }
        else
            merged.push_back(h);
    }
    auto rect = [&](double a, double b, double c, double d) {
        if (b - a < 0.2 || d - c < 0.2)
            return;
        quadF(s, P + U * a + kIntUp * c, P + U * b + kIntUp * c, P + U * b + kIntUp * d, P + U * a + kIntUp * d, N, mat);
    };
    double cur = s0;
    for (const Hole& h : merged)
    {
        rect(cur, h.s0, t0, t1);
        rect(h.s0, h.s1, t0, h.t0);
        rect(h.s0, h.s1, h.t1, t1);
        cur = h.s1;
    }
    rect(cur, s1, t0, t1);
}

/** Skirting along a face's foot, broken where a hole reaches the floor. */
void skirting(Soup& s, Vec3 P, Vec3 U, Vec3 N, double s0, double s1, double t0, const std::vector<Hole>& holes)
{
    std::vector<std::pair<double, double>> gaps;
    for (const Hole& h : holes)
        if (h.t0 < t0 + kSkirt)
            gaps.push_back({h.s0, h.s1});
    std::sort(gaps.begin(), gaps.end());
    double cur = s0;
    auto piece = [&](double a, double b) {
        if (b - a < 2.0)
            return;
        const Vec3 o = N * 1.5;
        quadF(s, P + U * a + kIntUp * t0 + o, P + U * b + kIntUp * t0 + o, P + U * b + kIntUp * (t0 + kSkirt) + o, P + U * a + kIntUp * (t0 + kSkirt) + o, N, IntTrim);
        quadF(s, P + U * a + kIntUp * (t0 + kSkirt), P + U * b + kIntUp * (t0 + kSkirt), P + U * b + kIntUp * (t0 + kSkirt) + o, P + U * a + kIntUp * (t0 + kSkirt) + o, kIntUp,
              IntTrim);
    };
    for (const auto& [a, b] : gaps)
    {
        piece(cur, std::min(a, s1));
        cur = std::max(cur, b);
    }
    piece(cur, s1);
}

/** A box from its corner o along three world vectors, outward faces (no bottom when !bottom). */
void boxW(Soup& s, Vec3 o, Vec3 A, Vec3 B, Vec3 C, int mat, bool bottom = true)
{
    const Vec3 c = o + (A + B + C) * 0.5;
    auto f = [&](Vec3 p0, Vec3 e1, Vec3 e2) {
        const Vec3 mid = p0 + (e1 + e2) * 0.5;
        quadF(s, p0, p0 + e1, p0 + e1 + e2, p0 + e2, mid - c, mat);
    };
    f(o, A, C);
    f(o + B, A, C);
    f(o, B, C);
    f(o + A, B, C);
    f(o + C, A, B);
    if (bottom)
        f(o, A, B);
}

struct Grid
{
    Vec2 o{0.0}, ax{1.0, 0.0}, az{0.0, 1.0};
    double c = 20.0, u0 = 0.0, v0 = 0.0;
    int nx = 0, nz = 0;
    Vec2 local(Vec2 p) const
    {
        const Vec2 d = p - o;
        return {glm::dot(d, ax), glm::dot(d, az)};
    }
    Vec2 world(double u, double v) const { return o + ax * u + az * v; }
    Vec3 w3(double u, double v, double y) const
    {
        const Vec2 w = world(u, v);
        return {w.x, y, w.y};
    }
    Vec2 centreL(int i, int j) const { return {u0 + (i + 0.5) * c, v0 + (j + 0.5) * c}; }
    int id(int i, int j) const { return j * nx + i; }
    bool cellOf(Vec2 l, int& i, int& j) const
    {
        i = int(std::floor((l.x - u0) / c));
        j = int(std::floor((l.y - v0) / c));
        return i >= 0 && j >= 0 && i < nx && j < nz;
    }
    Vec3 dirU() const { return {ax.x, 0.0, ax.y}; }
    Vec3 dirV() const { return {az.x, 0.0, az.y}; }
};

struct RoomW
{
    std::string name, kind, unit;
    int plate = 0;
    bool user = false, alive = true, unitShell = false, hub = false;
};

struct PlateW
{
    int index = 0, tier = 0, floor = 0;
    double y = 0.0, h = 0.0;
    std::vector<Ring> loops;          // inner wall faces: the outer one (faces in) then courtyard ones (face out of the hole)
    std::vector<char> loopIsHole;
    std::vector<char> mask;
    std::vector<char> clear; // cells wholly inside the outside walls' inner faces: where a core may stand
    std::vector<int> room;
    std::string prefix;
    bool volume = false;
    int core = -1;
    std::vector<int> openings;        // indices into Result::openings
    int hubCell = -1;
    std::vector<int> entryCells;      // interior cells of the exterior doors, with their opening index
    std::vector<int> entryOpenings;
};

/** U: two flights and a mid landing, a floor landing inside the core. Dog-leg: the same with its floor landings in the rooms.
 *  Straight: one flight a floor, stacked; it starts at the near end (a = 0) and arrives at the far end (a = L). */
enum class CoreKind
{
    U,
    DogLeg,
    Straight
};

const char* coreKindName(CoreKind k) { return k == CoreKind::U ? "u" : k == CoreKind::DogLeg ? "dogleg" : "straight"; }

struct CoreW
{
    std::string id;
    CoreKind kind = CoreKind::U;
    int i0 = 0, j0 = 0, ni = 0, nj = 0;
    Vec2 ol{0.0}, eL{1.0, 0.0}, eW{0.0, 1.0}; // grid-local origin and axes
    double L = 0.0, W = 0.0, coreW = 0.0, Wf = 0.0, Ld = 0.0, Lm = 0.0, run = 0.0; // Ld floor landing, Lm mid landing
    bool lift = false;
    std::vector<int> plates; // served, by floor
};

struct Run
{
    int axis = 0;  // 0: a line u = const (between columns), 1: v = const
    int line = 0;  // grid line index
    int k0 = 0, k1 = 0; // cells along the line, [k0, k1)
    int a = kNone, b = kNone; // entity on the low side, high side
    double e0 = 0.0, e1 = 0.0; // its extent clipped to the outside walls' inner faces, when set
};

struct DoorW
{
    std::string id;
    int plate = 0;
    int a = kNone, b = kNone; // entities (room index or kCoreCell)
    Run run;
    double at = 0.0;   // along the run, grid-local
    double width = 0.0, height = 0.0;
    bool user = false;
    bool open = false; // an opening with no door leaf: a compact stair's landing face
    bool up = false;   // the foot of a straight flight on a floor it also arrives at
};

struct Gen
{
    const Spec& spec;
    const Result& r;
    const Style& st;
    const CharacterProfile& prof;
    InteriorEdits ed;
    Interior& out;
    Grid g;
    std::vector<PlateW> plates;
    std::vector<RoomW> rooms;
    std::vector<CoreW> cores;
    std::vector<DoorW> doors;
    std::vector<int> stamp;
    int stampValue = 0;
    double doorW = 90.0, doorH = 210.0;
    /** Per plate, the floor level of each cell on a stepped ground floor; empty where the floor is flat at the plate's level. */
    std::vector<std::vector<double>> level;

    /** The floor level of a cell (the plate's, or a stepped ground floor's terrace); cells off the grid take the plate's. */
    double levelAt(int plate, int i, int j) const
    {
        if (size_t(plate) >= level.size())
            return plates[size_t(plate)].y;
        const std::vector<double>& l = level[size_t(plate)];
        if (l.empty() || i < 0 || j < 0 || i >= g.nx || j >= g.nz || plates[size_t(plate)].room[size_t(g.id(i, j))] == -2)
            return plates[size_t(plate)].y;
        return l[size_t(g.id(i, j))];
    }
    double levelAt(int plate, int cell) const { return levelAt(plate, cell % g.nx, cell / g.nx); }

    int newStamp()
    {
        if (++stampValue == std::numeric_limits<int>::max())
        {
            std::fill(stamp.begin(), stamp.end(), 0);
            stampValue = 1;
        }
        return stampValue;
    }
    std::string roomId(int k) const { return plates[size_t(rooms[size_t(k)].plate)].prefix + "/" + rooms[size_t(k)].name; }
    std::string entityId(int plate, int e) const
    {
        if (e == kCoreCell)
            return cores.empty() || plates[size_t(plate)].core < 0 ? std::string("core") : cores[size_t(plates[size_t(plate)].core)].id + "@" + plates[size_t(plate)].prefix;
        return e >= 0 ? roomId(e) : std::string("outside");
    }
    int addRoom(const std::string& name, const std::string& kind, int plate, const std::string& unit = {})
    {
        RoomW w;
        w.name = name;
        w.kind = kind;
        w.plate = plate;
        w.unit = unit;
        rooms.push_back(w);
        return int(rooms.size()) - 1;
    }
};

// Grid helpers ----------------------------------------------------------------------------------------------------------------

template <class F>
void forNeighbours(const Grid& g, int c, F f)
{
    const int i = c % g.nx, j = c / g.nx;
    if (i > 0)
        f(c - 1);
    if (i + 1 < g.nx)
        f(c + 1);
    if (j > 0)
        f(c - g.nx);
    if (j + 1 < g.nz)
        f(c + g.nx);
}

/** Connected components of a cell set (4-connected), largest first. */
std::vector<std::vector<int>> components(Gen& x, const std::vector<int>& cells)
{
    const int inSet = x.newStamp();
    for (int c : cells)
        x.stamp[size_t(c)] = inSet;
    const int seen = x.newStamp();
    std::vector<std::vector<int>> comps;
    for (int c : cells)
    {
        if (x.stamp[size_t(c)] != inSet)
            continue;
        std::vector<int> comp;
        std::deque<int> q{c};
        x.stamp[size_t(c)] = seen;
        while (!q.empty())
        {
            const int k = q.front();
            q.pop_front();
            comp.push_back(k);
            forNeighbours(x.g, k, [&](int n) {
                if (x.stamp[size_t(n)] == inSet)
                {
                    x.stamp[size_t(n)] = seen;
                    q.push_back(n);
                }
            });
        }
        comps.push_back(std::move(comp));
    }
    std::sort(comps.begin(), comps.end(), [](const auto& a, const auto& b) { return a.size() > b.size(); });
    return comps;
}

std::vector<int> cellsOf(const PlateW& p, int room)
{
    std::vector<int> out;
    for (size_t c = 0; c < p.room.size(); ++c)
        if (p.room[c] == room)
            out.push_back(int(c));
    return out;
}

/** Which room each neighbour of a cell set belongs to, by shared edge count. */
std::map<int, int> borders(const Gen& x, const PlateW& p, const std::vector<int>& cells, int self)
{
    std::map<int, int> out;
    for (int c : cells)
        forNeighbours(x.g, c, [&](int n) {
            const int r = p.room[size_t(n)];
            if (r >= 0 && r != self && x.rooms[size_t(r)].alive)
                ++out[r];
        });
    return out;
}

int bestNeighbour(const std::map<int, int>& b, const Gen& x, bool preferNotUnitShell = false)
{
    int best = -1, bestN = 0;
    for (const auto& [r, n] : b)
    {
        if (preferNotUnitShell && x.rooms[size_t(r)].unitShell)
            continue;
        if (n > bestN)
        {
            bestN = n;
            best = r;
        }
    }
    return best;
}

/** The cell of a set nearest a grid-local point. */
int nearestCell(const Gen& x, const std::vector<int>& cells, Vec2 l)
{
    int best = cells.empty() ? -1 : cells.front();
    double bd = 1e300;
    for (int c : cells)
    {
        const double d = glm::length(x.g.centreL(c % x.g.nx, c / x.g.nx) - l);
        if (d < bd)
        {
            bd = d;
            best = c;
        }
    }
    return best;
}

// The treemap split -------------------------------------------------------------------------------------------------------------

struct Item
{
    int room;
    double w;
    bool hub;
};

struct Seg
{
    Vec2 p1, p2; // an exterior opening's span on the inner face, grid-local, widened by a margin
};

void treemap(Gen& x, PlateW& p, std::vector<int> cells, std::vector<Item> items, Vec2 hubL, const std::vector<Seg>& segs, int depth = 0)
{
    if (items.empty() || cells.empty())
        return;
    if (items.size() == 1 || depth > 30 || cells.size() < 2)
    {
        for (int c : cells)
            p.room[size_t(c)] = items[0].room;
        return;
    }
    std::stable_sort(items.begin(), items.end(), [](const Item& a, const Item& b) { return a.w > b.w; });
    double total = 0.0;
    for (const Item& it : items)
        total += std::max(it.w, 1e-6);
    size_t k = 1;
    {
        double acc = 0.0, best = 1e300;
        for (size_t s = 1; s < items.size(); ++s)
        {
            acc += std::max(items[s - 1].w, 1e-6);
            const double err = std::abs(acc - total * 0.5);
            if (err < best)
            {
                best = err;
                k = s;
            }
        }
    }
    std::vector<Item> A(items.begin(), items.begin() + std::ptrdiff_t(k)), B(items.begin() + std::ptrdiff_t(k), items.end());
    double wA = 0.0;
    for (const Item& it : A)
        wA += std::max(it.w, 1e-6);
    const double rA = wA / total;
    const bool hubA = std::any_of(A.begin(), A.end(), [](const Item& i) { return i.hub; });
    const bool hubB = std::any_of(B.begin(), B.end(), [](const Item& i) { return i.hub; });

    int imin = x.g.nx, imax = -1, jmin = x.g.nz, jmax = -1;
    for (int c : cells)
    {
        const int i = c % x.g.nx, j = c / x.g.nx;
        imin = std::min(imin, i), imax = std::max(imax, i), jmin = std::min(jmin, j), jmax = std::max(jmax, j);
    }
    int hi = 0, hj = 0;
    x.g.cellOf(hubL, hi, hj);
    const double N = double(cells.size());

    struct Cand
    {
        int axis, t;
        bool aLow;
        double score;
    };
    std::vector<Cand> cands;
    for (int axis = 0; axis < 2; ++axis)
    {
        const int lo = axis == 0 ? imin : jmin, hiE = axis == 0 ? imax : jmax;
        if (hiE - lo < 1)
            continue;
        std::vector<double> count(size_t(hiE - lo + 2), 0.0);
        for (int c : cells)
            count[size_t((axis == 0 ? c % x.g.nx : c / x.g.nx) - lo + 1)] += 1.0;
        for (size_t s = 1; s < count.size(); ++s)
            count[s] += count[s - 1]; // count[t - lo] = cells with index < t
        const int hc = std::clamp(axis == 0 ? hi : hj, lo, hiE);
        const double otherExtent = double(axis == 0 ? jmax - jmin + 1 : imax - imin + 1);
        for (int orient = 0; orient < 2; ++orient)
        {
            const bool aLow = orient == 0;
            const double target = aLow ? rA : 1.0 - rA;
            for (int t = lo + 1; t <= hiE; ++t)
            {
                // The hub stays with the group holding its room.
                if ((hubA && aLow && hc >= t) || (hubA && !aLow && hc < t) || (hubB && aLow && hc < t) || (hubB && !aLow && hc >= t))
                    continue;
                const double frac = count[size_t(t - lo)] / N;
                if (frac <= 0.0 || frac >= 1.0)
                    continue;
                double score = std::abs(frac - target) * 10.0;
                const double e1 = double(t - lo), e2 = double(hiE + 1 - t);
                for (double e : {e1, e2})
                {
                    const double asp = std::max(e, otherExtent) / std::max(1.0, std::min(e, otherExtent));
                    score += 0.25 * std::max(0.0, asp - 2.5);
                    if (e * x.g.c < 110.0)
                        score += 2.0;
                }
                // Cuts through a window or door of the facade put a wall across it.
                const double coord = (axis == 0 ? x.g.u0 : x.g.v0) + t * x.g.c;
                const double oLo = (axis == 0 ? x.g.v0 + jmin * x.g.c : x.g.u0 + imin * x.g.c) - x.g.c * 2.0;
                const double oHi = (axis == 0 ? x.g.v0 + (jmax + 1) * x.g.c : x.g.u0 + (imax + 1) * x.g.c) + x.g.c * 2.0;
                for (const Seg& sg : segs)
                {
                    const double a1 = axis == 0 ? sg.p1.x : sg.p1.y, a2 = axis == 0 ? sg.p2.x : sg.p2.y;
                    const double o1 = axis == 0 ? sg.p1.y : sg.p1.x;
                    if (std::min(a1, a2) < coord && coord < std::max(a1, a2) && o1 > oLo && o1 < oHi)
                        score += 5.0;
                }
                cands.push_back({axis, t, aLow, score});
            }
        }
    }
    if (cands.empty())
    {
        for (int c : cells)
            p.room[size_t(c)] = items[0].room;
        return;
    }
    std::sort(cands.begin(), cands.end(), [](const Cand& a, const Cand& b) { return a.score < b.score; });
    // The best few, judged again by whether either side falls apart.
    Cand best = cands.front();
    double bestScore = 1e300;
    for (size_t q = 0; q < std::min<size_t>(5, cands.size()); ++q)
    {
        const Cand& cd = cands[q];
        std::vector<int> lowS, highS;
        for (int c : cells)
            ((cd.axis == 0 ? c % x.g.nx : c / x.g.nx) < cd.t ? lowS : highS).push_back(c);
        double score = cd.score;
        for (const auto* side : {&lowS, &highS})
        {
            const auto comps = components(x, *side);
            if (comps.size() > 1)
                score += 6.0 * (1.0 - double(comps.front().size()) / double(side->size()));
        }
        if (score < bestScore)
        {
            bestScore = score;
            best = cd;
        }
    }
    std::vector<int> lowS, highS;
    for (int c : cells)
        ((best.axis == 0 ? c % x.g.nx : c / x.g.nx) < best.t ? lowS : highS).push_back(c);
    std::vector<int>& sideA = best.aLow ? lowS : highS;
    std::vector<int>& sideB = best.aLow ? highS : lowS;
    auto hubFor = [&](std::vector<Item>& grp, const std::vector<int>& side) {
        if (std::any_of(grp.begin(), grp.end(), [](const Item& i) { return i.hub; }))
            return hubL;
        // A side without the hub keeps its biggest room next to the cut, where its door to the other side will be.
        grp.front().hub = true;
        const int c = nearestCell(x, side, hubL);
        return c >= 0 ? x.g.centreL(c % x.g.nx, c / x.g.nx) : hubL;
    };
    const Vec2 hA = hubFor(A, sideA), hB = hubFor(B, sideB);
    treemap(x, p, sideA, A, hA, segs, depth + 1);
    treemap(x, p, sideB, B, hB, segs, depth + 1);
}

// Cleanup ---------------------------------------------------------------------------------------------------------------------

void fillUnassigned(Gen& x, PlateW& p)
{
    std::deque<int> q;
    for (size_t c = 0; c < p.room.size(); ++c)
        if (p.mask[c] && p.room[c] >= 0)
            q.push_back(int(c));
    while (!q.empty())
    {
        const int c = q.front();
        q.pop_front();
        forNeighbours(x.g, c, [&](int n) {
            if (p.mask[size_t(n)] && p.room[size_t(n)] == kNone)
            {
                p.room[size_t(n)] = p.room[size_t(c)];
                q.push_back(n);
            }
        });
    }
}

/** Rooms in pieces keep their largest piece; the others go to the neighbour sharing the most wall. Tiny rooms merge away. */
/** Slivers of a room thinner than 60 cm go to the room beside them. */
void thinOut(Gen& x, PlateW& p)
{
    const int kMin = std::max(2, int(std::ceil(60.0 / x.g.c)));
    for (int pass = 0; pass < 4; ++pass)
    {
        bool changed = false;
        for (int j = 0; j < x.g.nz; ++j)
            for (int i = 0; i < x.g.nx; ++i)
            {
                const int c = x.g.id(i, j);
                const int rm = p.room[size_t(c)];
                if (rm < 0)
                    continue;
                for (int axis = 0; axis < 2; ++axis)
                {
                    int lo = axis == 0 ? i : j, hi = lo;
                    auto at = [&](int k) { return axis == 0 ? (k >= 0 && k < x.g.nx ? p.room[size_t(x.g.id(k, j))] : kNone) : (k >= 0 && k < x.g.nz ? p.room[size_t(x.g.id(i, k))] : kNone); };
                    while (at(lo - 1) == rm)
                        --lo;
                    while (at(hi + 1) == rm)
                        ++hi;
                    if (hi - lo + 1 >= kMin)
                        continue;
                    const int a = at(lo - 1), b = at(hi + 1);
                    const int to = b >= 0 ? b : a >= 0 ? a : kNone;
                    if (to < 0)
                        continue;
                    p.room[size_t(c)] = to;
                    changed = true;
                    break;
                }
            }
        if (!changed)
            break;
    }
}

void tidy(Gen& x, PlateW& p, int plateIndex, double minArea)
{
    thinOut(x, p);
    for (int pass = 0; pass < 6; ++pass)
    {
        bool changed = false;
        for (size_t k = 0; k < x.rooms.size(); ++k)
        {
            RoomW& rw = x.rooms[k];
            if (!rw.alive || rw.plate != plateIndex)
                continue;
            const std::vector<int> cells = cellsOf(p, int(k));
            if (cells.empty())
            {
                rw.alive = false;
                continue;
            }
            const auto comps = components(x, cells);
            for (size_t ci = 1; ci < comps.size(); ++ci)
            {
                const int to = bestNeighbour(borders(x, p, comps[ci], int(k)), x);
                if (to >= 0)
                {
                    for (int c : comps[ci])
                        p.room[size_t(c)] = to;
                    changed = true;
                }
            }
            const double area = double(comps.front().size()) * x.g.c * x.g.c / 1e4;
            if (area < minArea && !rw.user)
            {
                const int to = bestNeighbour(borders(x, p, comps.front(), int(k)), x);
                if (to >= 0)
                {
                    for (int c : comps.front())
                        p.room[size_t(c)] = to;
                    rw.alive = false;
                    changed = true;
                }
            }
        }
        if (!changed)
            break;
    }
}

/** Partitions never cross a window or door of the facade: the room most of it opens into takes the cells in front of the rest. */
void clearOpenings(Gen& x, int pi)
{
    PlateW& p = x.plates[size_t(pi)];
    const double wall = x.r.plates[size_t(p.index)].wall;
    for (int o : p.openings)
    {
        const Opening& op = x.r.openings[size_t(o)];
        const Vec2 n = glm::normalize(Vec2(op.normal.x, op.normal.z) + Vec2(1e-12));
        const Vec2 along{-n.y, n.x};
        const Vec2 c = Vec2(op.origin.x, op.origin.z) - n * wall;
        std::map<int, int> votes;
        for (double s = -op.width * 0.5; s <= op.width * 0.5 + 0.1; s += x.g.c * 0.5)
        {
            int i = 0, j = 0;
            if (!x.g.cellOf(x.g.local(c + along * s - n * (x.g.c * 1.5)), i, j))
                continue;
            const int rm = p.room[size_t(x.g.id(i, j))];
            if (rm >= 0)
                ++votes[rm];
        }
        if (votes.size() < 2)
            continue;
        int keep = -1, most = 0;
        for (const auto& [rm, n2] : votes)
            if (n2 > most)
                most = n2, keep = rm;
        const double half = op.width * 0.5 + 15.0, depth = 70.0;
        Vec2 lo(1e300), hi(-1e300);
        for (const Vec2& q : {c + along * half + n * wall, c - along * half + n * wall, c + along * half - n * depth, c - along * half - n * depth})
        {
            const Vec2 l = x.g.local(q);
            lo = glm::min(lo, l);
            hi = glm::max(hi, l);
        }
        int i0 = 0, j0 = 0, i1 = 0, j1 = 0;
        x.g.cellOf(lo, i0, j0);
        x.g.cellOf(hi, i1, j1);
        for (int j = std::max(0, j0); j <= std::min(x.g.nz - 1, j1); ++j)
            for (int i = std::max(0, i0); i <= std::min(x.g.nx - 1, i1); ++i)
            {
                const int cell = x.g.id(i, j);
                const int rm = p.room[size_t(cell)];
                if (rm < 0 || rm == keep || !votes.count(rm))
                    continue;
                const Vec2 l = x.g.centreL(i, j);
                const Vec2 rel = x.g.world(l.x, l.y) - c;
                if (std::abs(glm::dot(rel, along)) <= half && glm::dot(rel, -n) <= depth)
                    p.room[size_t(cell)] = keep;
            }
    }
}

// Layout of one plate -------------------------------------------------------------------------------------------------------------

std::vector<Item> itemsFor(Gen& x, int plate, const std::vector<Tpl>& tpls, double area, const std::string& prefix, const std::string& unit)
{
    const std::vector<double> w = weightsFor(tpls, area);
    std::vector<Item> items;
    for (size_t k = 0; k < tpls.size(); ++k)
    {
        const int r = x.addRoom(prefix + tpls[k].name, tpls[k].kind, plate, unit);
        x.rooms[size_t(r)].hub = tpls[k].hub;
        items.push_back({r, w[k], tpls[k].hub});
    }
    return items;
}

double areaOf(const Gen& x, size_t cells) { return double(cells) * x.g.c * x.g.c / 1e4; }

void layoutCorridor(Gen& x, int pi, const Layout& L, std::vector<int> cells, const std::vector<Seg>& segs)
{
    PlateW& p = x.plates[size_t(pi)];
    const int kc = std::max(3, int(std::ceil(std::max(150.0, 2.0 * x.prof.radius + 60.0) / x.g.c)));
    int imin = x.g.nx, imax = -1, jmin = x.g.nz, jmax = -1;
    for (int c : cells)
    {
        const int i = c % x.g.nx, j = c / x.g.nx;
        imin = std::min(imin, i), imax = std::max(imax, i), jmin = std::min(jmin, j), jmax = std::max(jmax, j);
    }
    const bool alongU = (imax - imin) >= (jmax - jmin);
    int spine = alongU ? (jmin + jmax) / 2 : (imin + imax) / 2;
    if (p.hubCell >= 0)
        spine = alongU ? p.hubCell / x.g.nx : p.hubCell % x.g.nx;
    const int corridor = x.addRoom("corridor", "corridor", pi);
    x.rooms[size_t(corridor)].hub = true;
    std::vector<char> isCorr(p.room.size(), 0), inCells(p.room.size(), 0);
    for (int c : cells)
        inCells[size_t(c)] = 1;
    for (int c : cells)
    {
        const int perp = alongU ? c / x.g.nx : c % x.g.nx;
        if (std::abs(perp - spine) <= kc / 2)
            isCorr[size_t(c)] = 1;
    }
    auto rest = [&] {
        std::vector<int> out;
        for (int c : cells)
            if (!isCorr[size_t(c)])
                out.push_back(c);
        return out;
    };
    // Parts the spine misses get a branch: the shortest path to them, as wide as the corridor.
    for (int iter = 0; iter < 6; ++iter)
    {
        const auto comps = components(x, rest());
        bool carved = false;
        for (const auto& comp : comps)
        {
            bool touches = false;
            for (int c : comp)
                forNeighbours(x.g, c, [&](int n) { touches = touches || isCorr[size_t(n)]; });
            if (touches || areaOf(x, comp.size()) < 4.0)
                continue;
            // BFS from the corridor through the plate to this part.
            const int inPlate = x.newStamp();
            for (int c : cells)
                x.stamp[size_t(c)] = inPlate;
            std::vector<int> from(p.room.size(), -1);
            std::deque<int> q;
            const int visited = x.newStamp();
            for (int c : cells)
                if (isCorr[size_t(c)])
                {
                    q.push_back(c);
                    x.stamp[size_t(c)] = visited;
                }
            std::vector<char> target(p.room.size(), 0);
            for (int c : comp)
                target[size_t(c)] = 1;
            int hit = -1;
            while (!q.empty() && hit < 0)
            {
                const int c = q.front();
                q.pop_front();
                forNeighbours(x.g, c, [&](int n) {
                    if (hit >= 0 || x.stamp[size_t(n)] != inPlate)
                        return;
                    x.stamp[size_t(n)] = visited;
                    from[size_t(n)] = c;
                    if (target[size_t(n)])
                        hit = n;
                    q.push_back(n);
                });
            }
            if (hit < 0)
                continue;
            std::vector<int> path;
            for (int c = hit; c >= 0 && !isCorr[size_t(c)]; c = from[size_t(c)])
                path.push_back(c);
            for (int c : path)
            {
                const int ci = c % x.g.nx, cj = c / x.g.nx;
                for (int dj = -kc / 2; dj <= kc / 2; ++dj)
                    for (int di = -kc / 2; di <= kc / 2; ++di)
                    {
                        const int ni = ci + di, nj = cj + dj;
                        if (ni < 0 || nj < 0 || ni >= x.g.nx || nj >= x.g.nz)
                            continue;
                        const int n = x.g.id(ni, nj);
                        if (inCells[size_t(n)])
                            isCorr[size_t(n)] = 1;
                    }
            }
            carved = true;
            break;
        }
        if (!carved)
            break;
    }
    for (int c : cells)
        if (isCorr[size_t(c)])
            p.room[size_t(c)] = corridor;
    auto comps = components(x, rest());
    // Specials go in the largest part; units fill the parts by area.
    std::vector<Tpl> specials = L.specials;
    int unitCounter = 0;
    std::map<std::string, int> singleCount;
    for (size_t ci = 0; ci < comps.size(); ++ci)
    {
        auto& comp = comps[ci];
        const double A = areaOf(x, comp.size());
        if (A < 3.0)
        {
            for (int c : comp)
                p.room[size_t(c)] = corridor;
            continue;
        }
        std::vector<Tpl> sp;
        double spArea = 0.0;
        if (ci == 0 || A > 40.0)
            while (!specials.empty() && spArea + specials.front().fixed <= A * 0.6)
            {
                spArea += specials.front().fixed;
                sp.push_back(specials.front());
                specials.erase(specials.begin());
            }
        int n = std::max(0, int(std::lround((A - spArea) / L.unitArea)));
        if (n == 0 && sp.empty())
            n = 1;
        std::vector<Item> items;
        double spSum = 0.0;
        for (size_t k = 0; k < sp.size(); ++k)
        {
            const double w = sp[k].fixed / std::max(A, 1e-9);
            spSum += w;
            items.push_back({x.addRoom(sp[k].name, sp[k].kind, pi), w, false});
        }
        const double unitW = n > 0 ? std::max(0.05, 1.0 - spSum) / n : 0.0;
        std::vector<int> unitRooms;
        for (int u = 0; u < n; ++u)
        {
            const int ur = x.addRoom("u" + std::to_string(unitCounter), "room", pi, "u" + std::to_string(unitCounter));
            x.rooms[size_t(ur)].unitShell = true;
            ++unitCounter;
            unitRooms.push_back(ur);
            items.push_back({ur, unitW, false});
        }
        // Everything starts from where the part meets the corridor.
        std::vector<int> edge;
        for (int c : comp)
        {
            bool t = false;
            forNeighbours(x.g, c, [&](int nb) { t = t || p.room[size_t(nb)] == corridor; });
            if (t)
                edge.push_back(c);
        }
        const Vec2 ref = p.hubCell >= 0 ? x.g.centreL(p.hubCell % x.g.nx, p.hubCell / x.g.nx) : x.g.centreL(comp[0] % x.g.nx, comp[0] / x.g.nx);
        const int hc = nearestCell(x, edge.empty() ? comp : edge, ref);
        if (!items.empty())
            items.front().hub = true;
        treemap(x, p, comp, items, x.g.centreL(hc % x.g.nx, hc / x.g.nx), segs);
        // Units that miss the corridor join a neighbour that has it.
        for (int ur : unitRooms)
        {
            const std::vector<int> uc = cellsOf(p, ur);
            if (uc.empty())
            {
                x.rooms[size_t(ur)].alive = false;
                continue;
            }
            const auto b = borders(x, p, uc, ur);
            if (b.count(corridor))
                continue;
            int to = -1, bestN = 0;
            for (const auto& [r2, cnt] : b)
                if (cnt > bestN && x.rooms[size_t(r2)].unitShell && borders(x, p, cellsOf(p, r2), r2).count(corridor))
                    bestN = cnt, to = r2;
            if (to < 0)
                to = corridor;
            for (int c : uc)
                p.room[size_t(c)] = to;
            x.rooms[size_t(ur)].alive = false;
        }
        // Each unit into its rooms, its living room on the corridor.
        for (int ur : unitRooms)
        {
            if (!x.rooms[size_t(ur)].alive)
                continue;
            const std::vector<int> uc = cellsOf(p, ur);
            const std::string uname = x.rooms[size_t(ur)].name;
            x.rooms[size_t(ur)].alive = false;
            const double UA = areaOf(x, uc.size());
            std::vector<int> uedge;
            for (int c : uc)
            {
                bool t = false;
                forNeighbours(x.g, c, [&](int nb) { t = t || p.room[size_t(nb)] == corridor; });
                if (t)
                    uedge.push_back(c);
            }
            Vec2 centre(0.0);
            for (int c : uc)
                centre += x.g.centreL(c % x.g.nx, c / x.g.nx);
            centre /= double(std::max<size_t>(1, uc.size()));
            const int hcell = nearestCell(x, uedge.empty() ? uc : uedge, centre);
            if (L.unit.size() == 1)
            {
                const std::string k = L.unitKind.empty() ? L.unit[0].kind : L.unitKind;
                const int cnt = ++singleCount[k];
                const int r2 = x.addRoom(k + (cnt > 1 ? std::to_string(cnt) : std::string()), L.unit[0].kind, pi);
                for (int c : uc)
                    p.room[size_t(c)] = r2;
                continue;
            }
            const std::vector<Tpl> ut = fitToArea(L.unit, UA);
            std::vector<Item> uitems = itemsFor(x, pi, ut, UA, uname + "-", uname);
            treemap(x, p, uc, uitems, x.g.centreL(hcell % x.g.nx, hcell / x.g.nx), segs);
        }
    }
}

// Cores ------------------------------------------------------------------------------------------------------------------------

struct CoreDims
{
    CoreKind kind;
    double Wf, Ld, Lm, run, L, W, coreW;
    bool lift;
};

/** attempt 0 the normal U, 1 a narrow U, 2 a dog-leg, 3 a straight flight: each smaller than the one before. */
CoreDims coreDims(const Gen& x, double hMax, bool lift, int attempt)
{
    CoreDims d{};
    const double c = x.g.c;
    const bool narrow = attempt >= 1;
    d.kind = attempt <= 1 ? CoreKind::U : attempt == 2 ? CoreKind::DogLeg : CoreKind::Straight;
    d.Wf = narrow ? std::max(80.0, 2.0 * x.prof.radius + 10.0) : std::max(100.0, 2.0 * x.prof.radius + 30.0);
    const double riserMax = std::min(18.0, x.prof.maxStep);
    const double tanMax = std::tan(x.prof.maxSlope * std::numbers::pi / 180.0);
    // Compact stairs take a shorter going, still within the character's slope.
    const double going = std::max(d.kind == CoreKind::U ? 28.0 : 25.0, riserMax / std::max(tanMax, 0.05));
    const int n = std::max(1, int(std::ceil(hMax * (d.kind == CoreKind::Straight ? 1.0 : 0.5) / riserMax)));
    d.run = n * going;
    d.lift = lift && d.kind == CoreKind::U;
    switch (d.kind)
    {
    case CoreKind::U:
        d.Ld = d.Lm = narrow ? std::max(100.0, d.Wf) : std::max(120.0, d.Wf + 10.0);
        d.W = std::ceil((2.0 * d.Wf + 10.0) / c) * c;
        break;
    case CoreKind::DogLeg:
        d.Lm = d.Wf;
        d.W = std::ceil((2.0 * d.Wf + 10.0) / c) * c;
        break;
    case CoreKind::Straight:
        d.W = std::ceil(d.Wf / c) * c;
        break;
    }
    d.L = std::ceil((d.Ld + d.Lm + d.run) / c) * c;
    d.coreW = d.lift ? std::ceil((d.W + 222.0) / c) * c : d.W;
    return d;
}

/** Fits the stair core through the plates: the position (and orientation) inside every plate it serves nearest the preferred point. */
void placeCore(Gen& x, const std::vector<int>& served, Vec2 preferW, bool houseLike, bool lift)
{
    if (served.size() < 2)
        return;
    double hMax = 0.0;
    for (int pi : served)
        hMax = std::max(hMax, x.plates[size_t(pi)].h);
    const size_t n = size_t(x.g.nx * x.g.nz);
    std::vector<char> fit(n, 1);
    for (int pi : served)
        for (size_t c = 0; c < n; ++c)
            fit[c] = char(fit[c] && x.plates[size_t(pi)].clear[c]);
    // Summed-area table over the cells every plate has.
    std::vector<int> sat(size_t((x.g.nx + 1) * (x.g.nz + 1)), 0);
    auto S = [&](int i, int j) -> int& { return sat[size_t(j * (x.g.nx + 1) + i)]; };
    for (int j = 0; j < x.g.nz; ++j)
        for (int i = 0; i < x.g.nx; ++i)
            S(i + 1, j + 1) = S(i, j + 1) + S(i + 1, j) - S(i, j) + (fit[size_t(x.g.id(i, j))] ? 1 : 0);
    auto full = [&](int i0, int j0, int ni, int nj) {
        if (i0 < 0 || j0 < 0 || i0 + ni > x.g.nx || j0 + nj > x.g.nz)
            return false;
        return S(i0 + ni, j0 + nj) - S(i0, j0 + nj) - S(i0 + ni, j0) + S(i0, j0) == ni * nj;
    };
    const Vec2 pref = x.g.local(preferW);
    // The facade's openings on these floors, as the cell just inside each: a core in front of them blocks the view.
    std::vector<std::pair<int, int>> windows;
    for (int pi : served)
        for (int o : x.plates[size_t(pi)].openings)
        {
            const Opening& op = x.r.openings[size_t(o)];
            const Vec2 on = glm::normalize(Vec2(op.normal.x, op.normal.z) + Vec2(1e-12));
            int i = 0, j = 0;
            if (x.g.cellOf(x.g.local(Vec2(op.origin.x, op.origin.z) - on * (x.r.plates[size_t(x.plates[size_t(pi)].index)].wall + x.g.c)), i, j))
                windows.push_back({i, j});
        }
    std::sort(windows.begin(), windows.end());
    windows.erase(std::unique(windows.begin(), windows.end()), windows.end());
    // Every kind with room to step off its landing first; only then compact stairs whose landing is a shallow strip.
    constexpr std::pair<int, bool> kTries[] = {{0, false}, {1, false}, {2, false}, {3, false}, {2, true}, {3, true}};
    for (const auto& [attempt, shallow] : kTries)
    {
        const CoreDims d = coreDims(x, hMax, lift, attempt);
        const int nL = int(std::lround(d.L / x.g.c)), nW = int(std::lround(d.coreW / x.g.c));
        double bestScore = 1e300;
        CoreW best;
        bool found = false;
        for (int dir = 0; dir < 4; ++dir)
        {
            const bool alongU = dir < 2;
            const int ni = alongU ? nL : nW, nj = alongU ? nW : nL;
            for (int j0 = 0; j0 + nj <= x.g.nz; ++j0)
                for (int i0 = 0; i0 + ni <= x.g.nx; ++i0)
                {
                    if (!full(i0, j0, ni, nj))
                        continue;
                    // The landing's open side must face cells of the plate; a straight flight's far end too, where it arrives.
                    // A compact stair lands in the room: two cells clear in front of it, so a character can step off.
                    const int rows = d.kind == CoreKind::U || shallow ? 1 : 2;
                    auto openAt = [&](bool farEnd) {
                        int open = 0, total = 0;
                        const bool low = (dir == 0 || dir == 2) != farEnd;
                        for (int k = 1; k <= rows; ++k)
                            if (alongU)
                            {
                                const int ia = low ? i0 - k : i0 + ni - 1 + k;
                                for (int j = j0; j < j0 + nj; ++j, ++total)
                                    open += ia >= 0 && ia < x.g.nx && fit[size_t(x.g.id(ia, j))] ? 1 : 0;
                            }
                            else
                            {
                                const int ja = low ? j0 - k : j0 + nj - 1 + k;
                                for (int i = i0; i < i0 + ni; ++i, ++total)
                                    open += ja >= 0 && ja < x.g.nz && fit[size_t(x.g.id(i, ja))] ? 1 : 0;
                            }
                        return open * 10 >= total * 7;
                    };
                    if (!openAt(false) || (d.kind == CoreKind::Straight && !openAt(true)))
                        continue;
                    const Vec2 centre{x.g.u0 + (i0 + ni * 0.5) * x.g.c, x.g.v0 + (j0 + nj * 0.5) * x.g.c};
                    double score = glm::length(centre - pref);
                    for (const auto& [wi, wj] : windows)
                        if (wi >= i0 - 2 && wi < i0 + ni + 2 && wj >= j0 - 2 && wj < j0 + nj + 2)
                            score += 500.0;
                    if (houseLike)
                    {
                        // Stairs along a wall: a long side against cells outside the plate.
                        int against = 0;
                        if (alongU)
                            for (int i = i0; i < i0 + ni; ++i)
                                against += (j0 - 1 < 0 || !fit[size_t(x.g.id(i, j0 - 1))]) || (j0 + nj >= x.g.nz || !fit[size_t(x.g.id(i, j0 + nj))]);
                        else
                            for (int j = j0; j < j0 + nj; ++j)
                                against += (i0 - 1 < 0 || !fit[size_t(x.g.id(i0 - 1, j))]) || (i0 + ni >= x.g.nx || !fit[size_t(x.g.id(i0 + ni, j))]);
                        score -= x.g.c * against;
                    }
                    if (score < bestScore)
                    {
                        bestScore = score;
                        found = true;
                        best = CoreW{};
                        best.i0 = i0, best.j0 = j0, best.ni = ni, best.nj = nj;
                        const double U0 = x.g.u0 + i0 * x.g.c, V0 = x.g.v0 + j0 * x.g.c, U1 = U0 + ni * x.g.c, V1 = V0 + nj * x.g.c;
                        switch (dir)
                        {
                        case 0:
                            best.ol = {U0, V0}, best.eL = {1, 0}, best.eW = {0, 1};
                            break;
                        case 1:
                            best.ol = {U1, V0}, best.eL = {-1, 0}, best.eW = {0, 1};
                            break;
                        case 2:
                            best.ol = {U0, V0}, best.eL = {0, 1}, best.eW = {1, 0};
                            break;
                        default:
                            best.ol = {U0, V1}, best.eL = {0, -1}, best.eW = {1, 0};
                            break;
                        }
                    }
                }
        }
        if (!found)
            continue;
        best.kind = d.kind;
        best.L = nL * x.g.c;
        best.coreW = nW * x.g.c;
        best.W = d.W;
        best.Wf = d.Wf;
        best.Ld = d.Ld;
        best.Lm = d.Lm;
        best.run = best.L - d.Ld - d.Lm;
        best.lift = d.lift;
        best.id = "core" + std::to_string(x.cores.size());
        best.plates = served;
        if (attempt == 1)
            x.out.notes.push_back("the stair core is the narrow kind: the floors are tight");
        else if (attempt == 2)
            x.out.notes.push_back("the stair is a dog-leg landing in the rooms: the floors are too tight for a stair core");
        else if (attempt == 3)
            x.out.notes.push_back("the stair is a straight flight on each floor: the floors are too narrow for a dog-leg");
        if (lift && !d.lift)
            x.out.notes.push_back("no lift: the floors only fit a compact stair");
        if (shallow)
            x.out.notes.push_back("the stair's landing is a shallow strip: the floors are too small for room in front of it");
        for (int pi : served)
        {
            x.plates[size_t(pi)].core = int(x.cores.size());
            for (int j = best.j0; j < best.j0 + best.nj; ++j)
                for (int i = best.i0; i < best.i0 + best.ni; ++i)
                    x.plates[size_t(pi)].room[size_t(x.g.id(i, j))] = kCoreCell;
        }
        x.cores.push_back(best);
        return;
    }
    x.out.warnings.push_back("no room for a stair core through the floors: the floors above the ground floor can't be reached");
}

/** Whether a plate is the lowest a core serves. */
bool coreBottom(const Gen& x, const CoreW& k, const PlateW& p)
{
    for (int pi : k.plates)
        if (x.plates[size_t(pi)].floor < p.floor)
            return false;
    return true;
}

/** Whether a plate is the highest a core serves. */
bool coreTop(const Gen& x, const CoreW& k, const PlateW& p)
{
    for (int pi : k.plates)
        if (x.plates[size_t(pi)].floor > p.floor)
            return false;
    return true;
}

bool coreOpen(const CoreW& k) { return k.kind != CoreKind::U; }

/** The cell just outside the middle of a core's landing side (a straight flight arrives at its far end above the lowest floor). */
int coreAccessCell(const Gen& x, const CoreW& k, const PlateW& pl)
{
    const bool far = k.kind == CoreKind::Straight && !coreBottom(x, k, pl);
    const Vec2 p = k.ol + k.eW * (k.coreW * 0.5) + (far ? k.eL * (k.L + x.g.c * 0.5) : -k.eL * (x.g.c * 0.5));
    int i = 0, j = 0;
    return x.g.cellOf(p, i, j) ? x.g.id(i, j) : -1;
}

// Runs and doors --------------------------------------------------------------------------------------------------------------

std::vector<Run> runsOf(const Gen& x, const PlateW& p)
{
    std::vector<Run> runs;
    auto ent = [&](int c) {
        const int r = p.room[size_t(c)];
        return p.mask[size_t(c)] || r == kCoreCell ? r : kNone;
    };
    for (int axis = 0; axis < 2; ++axis)
    {
        const int lines = axis == 0 ? x.g.nx : x.g.nz, along = axis == 0 ? x.g.nz : x.g.nx;
        for (int l = 1; l < lines; ++l)
        {
            Run cur;
            bool open = false;
            for (int k = 0; k <= along; ++k)
            {
                int a = kNone, b = kNone;
                if (k < along)
                {
                    const int ca = axis == 0 ? x.g.id(l - 1, k) : x.g.id(k, l - 1);
                    const int cb = axis == 0 ? x.g.id(l, k) : x.g.id(k, l);
                    a = ent(ca);
                    b = ent(cb);
                }
                const bool wall = k < along && a != b && (a >= 0 || a == kCoreCell) && (b >= 0 || b == kCoreCell);
                if (open && (!wall || a != cur.a || b != cur.b))
                {
                    cur.k1 = k;
                    runs.push_back(cur);
                    open = false;
                }
                if (wall && !open)
                {
                    cur = Run{axis, l, k, k + 1, a, b};
                    open = true;
                }
            }
        }
    }
    return runs;
}

double runLen(const Gen& x, const Run& r) { return r.e1 > r.e0 ? r.e1 - r.e0 : (r.k1 - r.k0) * x.g.c; }

/** Grid-local point on a run's line at `along` (grid-local coordinate along the line). */
Vec2 runPoint(const Gen& x, const Run& r, double along)
{
    return r.axis == 0 ? Vec2(x.g.u0 + r.line * x.g.c, along) : Vec2(along, x.g.v0 + r.line * x.g.c);
}

double runStart(const Gen& x, const Run& r) { return (r.axis == 0 ? x.g.v0 : x.g.u0) + r.k0 * x.g.c; }
double runEnd(const Gen& x, const Run& r) { return (r.axis == 0 ? x.g.v0 : x.g.u0) + r.k1 * x.g.c; }

/** The part of a core run on the core's landing, where its door can go; empty run when none. Compact cores have their landings
 *  outside: their near face, and for a straight flight above the lowest floor the far face it arrives at. */
Run landingPart(const Gen& x, const CoreW& k, const Run& r, const PlateW& p, bool departure = false)
{
    // The landing in grid-local coordinates.
    const bool bottom = coreBottom(x, k, p);
    const double a0 = k.kind == CoreKind::Straight && !bottom && !departure ? k.L : 0.0;
    // A dog-leg's top floor opens only where its last flight arrives: beside it is the drop to the flight below.
    const double b0 = k.kind == CoreKind::DogLeg && coreTop(x, k, p) ? std::floor(k.W * 0.5 / x.g.c) * x.g.c : 0.0;
    const Vec2 a = k.ol + k.eL * a0 + k.eW * b0, b = k.ol + k.eL * (a0 + k.Ld) + k.eW * k.coreW;
    const double lo0 = std::min(a.x, b.x), hi0 = std::max(a.x, b.x), lo1 = std::min(a.y, b.y), hi1 = std::max(a.y, b.y);
    const double line = r.axis == 0 ? x.g.u0 + r.line * x.g.c : x.g.v0 + r.line * x.g.c;
    const double lineLo = r.axis == 0 ? lo0 : lo1, lineHi = r.axis == 0 ? hi0 : hi1;
    if (line < lineLo - 0.5 || line > lineHi + 0.5)
        return Run{r.axis, r.line, 0, 0, r.a, r.b};
    const double alLo = r.axis == 0 ? lo1 : lo0, alHi = r.axis == 0 ? hi1 : hi0;
    const double base = r.axis == 0 ? x.g.v0 : x.g.u0;
    Run out = r;
    out.k0 = std::max(r.k0, int(std::lround((alLo - base) / x.g.c)));
    out.k1 = std::min(r.k1, int(std::lround((alHi - base) / x.g.c)));
    if (out.k1 <= out.k0)
        out.k1 = out.k0;
    return out;
}

/** The entity of a cell as walls see it: its room, the core, or none outside the plate. */
int entAtCell(const Gen& x, const PlateW& p, int i, int j)
{
    if (i < 0 || j < 0 || i >= x.g.nx || j >= x.g.nz)
        return kNone;
    const int c = x.g.id(i, j);
    return p.mask[size_t(c)] || p.room[size_t(c)] == kCoreCell ? p.room[size_t(c)] : kNone;
}

/** Where a line through `from` along d (grid-local) meets the plate's outside wall faces nearest it: the coordinate along d. */
double clipToWall(const Gen& x, const PlateW& p, Vec2 from, Vec2 d, double fallback)
{
    double best = 1e300, bestT = fallback;
    const Vec2 A0 = from - d * (x.g.c * 4.0), A1 = from + d * (x.g.c * 4.0);
    for (const Ring& loop : p.loops)
        for (size_t e = 0; e < loop.size(); ++e)
        {
            const Vec2 B0 = x.g.local(loop[e]), B1 = x.g.local(loop[(e + 1) % loop.size()]);
            const Vec2 r1 = A1 - A0, r2 = B1 - B0;
            const double den = r1.x * r2.y - r1.y * r2.x;
            if (std::abs(den) < 1e-9)
                continue;
            const Vec2 w = B0 - A0;
            const double t = (w.x * r2.y - w.y * r2.x) / den, u = (w.x * r1.y - w.y * r1.x) / den;
            if (t < 0.0 || t > 1.0 || u < 0.0 || u > 1.0)
                continue;
            const Vec2 q = A0 + r1 * t;
            const double dd = glm::length(q - from);
            if (dd < best)
            {
                best = dd;
                bestT = glm::dot(q, d);
            }
        }
    return bestT;
}

/** The entity beyond a run's end on one side. */
int beyondRun(const Gen& x, const PlateW& p, const Run& run, bool end, bool high)
{
    const int k = end ? run.k1 : run.k0 - 1;
    const int l = high ? run.line : run.line - 1;
    return run.axis == 0 ? entAtCell(x, p, l, k) : entAtCell(x, p, k, l);
}

/** A run with its extent clipped to the outside walls' inner faces where it runs into them. */
Run clipped(const Gen& x, const PlateW& p, Run run)
{
    const double line = run.axis == 0 ? x.g.u0 + run.line * x.g.c : x.g.v0 + run.line * x.g.c;
    const Vec2 along = run.axis == 0 ? Vec2(0, 1) : Vec2(1, 0);
    double e0 = runStart(x, run), e1 = runEnd(x, run);
    for (int end = 0; end < 2; ++end)
        if (beyondRun(x, p, run, end == 1, false) == kNone || beyondRun(x, p, run, end == 1, true) == kNone)
        {
            const double at = end == 1 ? e1 : e0;
            const Vec2 from = run.axis == 0 ? Vec2(line, at) : Vec2(at, line);
            const double t = clipToWall(x, p, from, along, at);
            (end == 1 ? e1 : e0) = end == 1 ? std::min(at, t) : std::max(at, t);
        }
    run.e0 = e0;
    run.e1 = std::max(e0, e1);
    return run;
}

double pairCost(const Gen& x, int a, int b)
{
    auto kindOf = [&](int e) { return e == kCoreCell ? std::string("core") : x.rooms[size_t(e)].kind; };
    const std::string ka = kindOf(a), kb = kindOf(b);
    if (ka == "core" || kb == "core")
    {
        const int o = ka == "core" ? b : a;
        if (!x.rooms[size_t(o)].unit.empty())
            return 30.0;
        return circulation(x.rooms[size_t(o)].kind) ? 1.0 : 3.0;
    }
    const std::string ua = x.rooms[size_t(a)].unit, ub = x.rooms[size_t(b)].unit;
    if (!ua.empty() && !ub.empty() && ua != ub)
        return 60.0;
    if (ka == "corridor" || kb == "corridor")
    {
        // A unit opens on the corridor through its hub room only.
        const RoomW& o = x.rooms[size_t(ka == "corridor" ? b : a)];
        return o.unit.empty() || o.hub ? 1.0 : 8.0;
    }
    if (circulation(ka) || circulation(kb))
        return 1.0;
    const bool pa = privateRoom(ka), pb = privateRoom(kb);
    if ((ka == "bathroom" && kb == "bedroom") || (kb == "bathroom" && ka == "bedroom"))
        return 2.0;
    if ((ka == "toilet" && kb == "backroom") || (kb == "toilet" && ka == "backroom"))
        return 1.5;
    if (pa && pb)
        return 6.0;
    if (pa || pb)
        return 4.0;
    return 3.0;
}

std::string leafName(const Gen& x, int e) { return e == kCoreCell ? std::string("core") : x.rooms[size_t(e)].name; }

std::string doorIdFor(const Gen& x, const PlateW& p, int a, int b)
{
    std::string na = leafName(x, a), nb = leafName(x, b);
    if (nb < na)
        std::swap(na, nb);
    return p.prefix + "/door/" + na + "+" + nb;
}

/** The floor levels either side of a door's middle: run.a's side, then run.b's. */
std::pair<double, double> doorLevels(const Gen& x, const DoorW& d)
{
    const int k = int(std::floor((d.at - (d.run.axis == 0 ? x.g.v0 : x.g.u0)) / x.g.c));
    return d.run.axis == 0 ? std::pair{x.levelAt(d.plate, d.run.line - 1, k), x.levelAt(d.plate, d.run.line, k)}
                           : std::pair{x.levelAt(d.plate, k, d.run.line - 1), x.levelAt(d.plate, k, d.run.line)};
}

/** A straight flight between floors in the middle starts at the core's near face: open it to the room in front. */
void addDeparture(Gen& x, int pi)
{
    const PlateW& p = x.plates[size_t(pi)];
    if (p.core < 0)
        return;
    const CoreW& k = x.cores[size_t(p.core)];
    if (k.kind != CoreKind::Straight || coreBottom(x, k, p) || coreTop(x, k, p))
        return;
    Run best;
    double bestLen = 0.0;
    for (const Run& r : runsOf(x, p))
    {
        if ((r.a == kCoreCell) == (r.b == kCoreCell))
            continue;
        Run use = landingPart(x, k, r, p, true);
        if (use.k1 <= use.k0)
            continue;
        use = clipped(x, p, use);
        if (runLen(x, use) > bestLen)
        {
            bestLen = runLen(x, use);
            best = use;
        }
    }
    if (bestLen < 2.0 * x.prof.radius + 10.0)
    {
        x.out.warnings.push_back("the straight flight up from " + p.prefix + " has no room in front of its foot: the floors above can't be reached");
        return;
    }
    DoorW d;
    d.plate = pi;
    d.a = best.a;
    d.b = best.b;
    d.run = best;
    d.at = (best.e0 + best.e1) * 0.5;
    d.width = bestLen - 4.0;
    d.height = x.doorH;
    d.open = d.up = true;
    d.id = doorIdFor(x, p, best.a, best.b) + "/up";
    x.doors.push_back(d);
}

/** Doors of a plate: a cheapest spanning tree from its roots; rooms no door can reach merge into a neighbour. */
void solveDoors(Gen& x, int pi)
{
    PlateW& p = x.plates[size_t(pi)];
    for (int attempt = 0; attempt < 12; ++attempt)
    {
        const std::vector<Run> runs = runsOf(x, p);
        // The best run per pair.
        std::map<std::pair<int, int>, Run> best;
        std::map<std::pair<int, int>, double> shared;
        for (const Run& r : runs)
        {
            const std::pair<int, int> key{std::min(r.a, r.b), std::max(r.a, r.b)};
            shared[key] += runLen(x, r);
            Run use = r;
            const bool toCore = r.a == kCoreCell || r.b == kCoreCell;
            if (toCore)
                use = landingPart(x, x.cores[size_t(p.core)], r, p);
            if (use.k1 <= use.k0)
                continue;
            use = clipped(x, p, use);
            if (runLen(x, use) < (toCore && coreOpen(x.cores[size_t(p.core)]) ? 2.0 * x.prof.radius + 10.0 : x.doorW + 30.0))
                continue;
            auto it = best.find(key);
            if (it == best.end() || runLen(x, use) > runLen(x, it->second))
                best[key] = use;
        }
        // A core whose landing meets no wall long enough opens off the side of its flight instead.
        if (p.core >= 0 && !coreOpen(x.cores[size_t(p.core)]) &&
            std::none_of(best.begin(), best.end(), [](const auto& kv) { return kv.first.first == kCoreCell || kv.first.second == kCoreCell; }))
        {
            for (const Run& r : runs)
            {
                if (r.a != kCoreCell && r.b != kCoreCell)
                    continue;
                const Run use = clipped(x, p, r);
                if (runLen(x, use) < x.doorW + 30.0)
                    continue;
                const std::pair<int, int> key{std::min(r.a, r.b), std::max(r.a, r.b)};
                auto it = best.find(key);
                if (it == best.end() || runLen(x, use) > runLen(x, it->second))
                    best[key] = use;
            }
            if (std::any_of(best.begin(), best.end(), [](const auto& kv) { return kv.first.first == kCoreCell || kv.first.second == kCoreCell; }))
                x.out.notes.push_back("the stair core on " + p.prefix + " opens beside its flight: its landing meets no wall long enough for a door");
        }
        std::vector<int> nodes;
        for (size_t k = 0; k < x.rooms.size(); ++k)
            if (x.rooms[k].alive && x.rooms[k].plate == pi)
                nodes.push_back(int(k));
        const bool hasCore = p.core >= 0;
        std::set<int> roots;
        for (int c : p.entryCells)
            if (p.room[size_t(c)] >= 0)
                roots.insert(p.room[size_t(c)]);
        if (roots.empty())
        {
            if (hasCore && p.floor > 0)
                roots.insert(kCoreCell);
            else if (p.hubCell >= 0 && p.room[size_t(p.hubCell)] >= 0)
                roots.insert(p.room[size_t(p.hubCell)]);
            else if (!nodes.empty())
                roots.insert(nodes.front());
        }
        std::set<int> in = roots;
        std::vector<std::pair<int, int>> tree;
        const size_t want = nodes.size() + (hasCore ? 1 : 0);
        while (in.size() < want)
        {
            double bc = 1e300;
            std::pair<int, int> be{0, 0};
            Run br;
            for (const auto& [key, run] : best)
            {
                const bool ia = in.count(key.first) > 0, ib = in.count(key.second) > 0;
                if (ia == ib)
                    continue;
                const double cost = pairCost(x, key.first, key.second) - runLen(x, run) * 1e-4;
                if (cost < bc)
                {
                    bc = cost;
                    be = key;
                    br = run;
                }
            }
            if (bc >= 1e299)
                break;
            in.insert(be.first);
            in.insert(be.second);
            tree.push_back(be);
        }
        // Rooms left out: no wall long enough for a door. Merge the first into its widest neighbour and try again.
        int stray = kNone;
        for (int k : nodes)
            if (!in.count(k))
            {
                stray = k;
                break;
            }
        if (stray != kNone && attempt < 11)
        {
            const std::vector<int> cells = cellsOf(p, stray);
            const int to = bestNeighbour(borders(x, p, cells, stray), x);
            if (to >= 0)
            {
                x.out.notes.push_back(x.roomId(stray) + " had no wall long enough for a door and joined " + x.roomId(to));
                for (int c : cells)
                    p.room[size_t(c)] = to;
                x.rooms[size_t(stray)].alive = false;
                continue;
            }
        }
        for (const auto& key : tree)
        {
            const Run& run = best[key];
            DoorW d;
            d.plate = pi;
            d.a = run.a;
            d.b = run.b;
            d.run = run;
            const std::string ka = key.first == kCoreCell ? "core" : x.rooms[size_t(key.first)].kind;
            const std::string kb = key.second == kCoreCell ? "core" : x.rooms[size_t(key.second)].kind;
            d.width = (publicRoom(ka) || publicRoom(kb)) && circulation(ka) && circulation(kb) ? 160.0 : x.doorW;
            if (runLen(x, run) < d.width + 30.0)
                d.width = x.doorW;
            d.height = x.doorH;
            d.at = (run.e0 + run.e1) * 0.5;
            d.id = doorIdFor(x, p, key.first, key.second);
            if ((key.first == kCoreCell || key.second == kCoreCell) && coreOpen(x.cores[size_t(p.core)]))
            {
                // Compact stairs land in the room: the core's face is open over its whole width.
                d.open = true;
                d.width = runLen(x, run) - 4.0;
            }
            x.doors.push_back(d);
        }
        addDeparture(x, pi);
        return;
    }
}

// User edits ------------------------------------------------------------------------------------------------------------------

int findRoom(const Gen& x, const std::string& id)
{
    for (size_t k = 0; k < x.rooms.size(); ++k)
        if (x.rooms[k].alive && x.roomId(int(k)) == id)
            return int(k);
    return -1;
}

void applyLayoutEdits(Gen& x)
{
    for (const auto& [id, sp] : x.ed.splits)
    {
        const int r = findRoom(x, id);
        if (r < 0)
        {
            x.out.orphanEdits.push_back("split " + id);
            continue;
        }
        PlateW& p = x.plates[size_t(x.rooms[size_t(r)].plate)];
        std::vector<int> cells = cellsOf(p, r);
        int imin = x.g.nx, imax = -1, jmin = x.g.nz, jmax = -1;
        for (int c : cells)
        {
            const int i = c % x.g.nx, j = c / x.g.nx;
            imin = std::min(imin, i), imax = std::max(imax, i), jmin = std::min(jmin, j), jmax = std::max(jmax, j);
        }
        const int axis = sp.axis == "u" ? 0 : sp.axis == "v" ? 1 : (imax - imin >= jmax - jmin ? 0 : 1);
        std::vector<int> coord;
        for (int c : cells)
            coord.push_back(axis == 0 ? c % x.g.nx : c / x.g.nx);
        std::vector<int> sorted = coord;
        std::sort(sorted.begin(), sorted.end());
        if (sorted.empty())
            continue;
        const int cut = sorted[std::clamp(size_t(sp.at * double(sorted.size())), size_t(0), sorted.size() - 1)];
        const int nr = x.addRoom(x.rooms[size_t(r)].name + "-split", sp.kind.empty() ? "room" : sp.kind, x.rooms[size_t(r)].plate, x.rooms[size_t(r)].unit);
        x.rooms[size_t(nr)].user = true;
        x.rooms[size_t(r)].user = true;
        int moved = 0;
        for (size_t k = 0; k < cells.size(); ++k)
            if (coord[k] >= cut && cut > sorted.front())
            {
                p.room[size_t(cells[k])] = nr;
                ++moved;
            }
        if (moved == 0)
        {
            x.rooms[size_t(nr)].alive = false;
            x.out.warnings.push_back("room " + id + " is too small to split");
        }
    }
    for (const auto& [into, from] : x.ed.merges)
    {
        const int a = findRoom(x, into), b = findRoom(x, from);
        if (a < 0 || b < 0 || a == b || x.rooms[size_t(a)].plate != x.rooms[size_t(b)].plate)
        {
            x.out.orphanEdits.push_back("merge " + from + " into " + into);
            continue;
        }
        PlateW& p = x.plates[size_t(x.rooms[size_t(a)].plate)];
        for (int& v : p.room)
            if (v == b)
                v = a;
        x.rooms[size_t(b)].alive = false;
        x.rooms[size_t(a)].user = true;
    }
    for (const InteriorEdits::Move& mv : x.ed.moves)
    {
        int a = findRoom(x, mv.room), b = findRoom(x, mv.toward);
        if (a < 0 || b < 0 || a == b || x.rooms[size_t(a)].plate != x.rooms[size_t(b)].plate)
        {
            x.out.orphanEdits.push_back("move " + mv.room + " toward " + mv.toward);
            continue;
        }
        if (mv.by < 0.0)
            std::swap(a, b);
        PlateW& p = x.plates[size_t(x.rooms[size_t(a)].plate)];
        const int layers = int(std::lround(std::abs(mv.by) / x.g.c));
        // The shared wall's axis decides which way it moves.
        int vertical = 0, horizontal = 0;
        for (const Run& r : runsOf(x, p))
            if ((r.a == a && r.b == b) || (r.a == b && r.b == a))
                (r.axis == 0 ? vertical : horizontal) += r.k1 - r.k0;
        if (vertical + horizontal == 0)
        {
            x.out.orphanEdits.push_back("move " + mv.room + " toward " + mv.toward + " (they don't share a wall)");
            continue;
        }
        const bool alongU = vertical >= horizontal; // a wall u = const moves along u
        std::vector<int> bCells = cellsOf(p, b);
        for (int layer = 0; layer < layers && bCells.size() > 4; ++layer)
        {
            std::vector<int> grow;
            for (int c : bCells)
            {
                const int i = c % x.g.nx, j = c / x.g.nx;
                const int n1 = alongU ? (i > 0 ? c - 1 : -1) : (j > 0 ? c - x.g.nx : -1);
                const int n2 = alongU ? (i + 1 < x.g.nx ? c + 1 : -1) : (j + 1 < x.g.nz ? c + x.g.nx : -1);
                if ((n1 >= 0 && p.room[size_t(n1)] == a) || (n2 >= 0 && p.room[size_t(n2)] == a))
                    grow.push_back(c);
            }
            if (grow.empty() || grow.size() + 4 >= bCells.size())
                break;
            for (int c : grow)
                p.room[size_t(c)] = a;
            bCells = cellsOf(p, b);
        }
        x.rooms[size_t(a)].user = x.rooms[size_t(b)].user = true;
    }
    for (const auto& [id, kind] : x.ed.kinds)
    {
        const int r = findRoom(x, id);
        if (r < 0)
        {
            x.out.orphanEdits.push_back("kind " + id);
            continue;
        }
        x.rooms[size_t(r)].kind = kind;
        x.rooms[size_t(r)].user = true;
    }
}

/** The run between two entities nearest a point (grid-local), long enough for a door; false when they share none. */
bool runBetween(Gen& x, const PlateW& p, int a, int b, std::optional<Vec2> at, Run& out, double& along, double width)
{
    double bd = 1e300;
    bool found = false;
    for (const Run& r : runsOf(x, p))
    {
        if (!((r.a == a && r.b == b) || (r.a == b && r.b == a)))
            continue;
        Run use = r;
        if (r.a == kCoreCell || r.b == kCoreCell)
            use = landingPart(x, x.cores[size_t(p.core)], r, p);
        if (use.k1 <= use.k0)
            continue;
        use = clipped(x, p, use);
        if (runLen(x, use) < width + 20.0)
            continue;
        const double s0 = use.e0 + width * 0.5 + 10.0, s1 = use.e1 - width * 0.5 - 10.0;
        double t = (s0 + s1) * 0.5;
        double d = -runLen(x, use);
        if (at)
        {
            const double coord = use.axis == 0 ? at->y : at->x;
            t = std::clamp(coord, s0, s1);
            d = glm::length(runPoint(x, use, t) - *at);
        }
        if (d < bd)
        {
            bd = d;
            out = use;
            along = t;
            found = true;
        }
    }
    return found;
}
} // namespace

/**
 * A ground floor on a slope steps as W3 stepped its bays: each cell stands at the stepped level of the ground under it (30
 * cm terraces), and the cells behind a street door or shopfront at its sill, so it opens level; behind a window never above
 * its sill. The core keeps the plate's level; doors between levels get steps, and terraces within a room a riser.
 */
void stepGroundFloor(Gen& x)
{
    x.level.assign(x.plates.size(), {});
    const double lo = x.spec.groundRange().first + 5.0;
    for (size_t pi = 0; pi < x.plates.size(); ++pi)
    {
        const PlateW& p = x.plates[pi];
        if (p.floor != 0)
            continue;
        std::vector<double>& l = x.level[pi];
        l.assign(p.room.size(), p.y);
        for (int j = 0; j < x.g.nz; ++j)
            for (int i = 0; i < x.g.nx; ++i)
                if (p.mask[size_t(x.g.id(i, j))])
                {
                    const Vec2 c = x.g.centreL(i, j);
                    l[size_t(x.g.id(i, j))] = std::clamp(x.spec.steppedLevel(x.g.world(c.x, c.y), x.st.plinth, p.y), lo, p.y);
                }
        // The cells behind each ground-floor opening: as wide as it, a metre and a half deep.
        const double wall = x.r.plates[size_t(p.index)].wall;
        for (int pass = 0; pass < 2; ++pass)
            for (int o : p.openings)
            {
                const Opening& op = x.r.openings[size_t(o)];
                const bool window = op.kind == "window";
                // Windows first, so a door's level wins where the two meet.
                if (op.bay.find("/l0/") == std::string::npos || window != (pass == 0))
                    continue;
                const Vec2 nrm = glm::normalize(Vec2(op.normal.x, op.normal.z) + Vec2(1e-12));
                const Vec2 along{-nrm.y, nrm.x}, at = Vec2(op.origin.x, op.origin.z) - nrm * wall;
                const double y = std::clamp(op.origin.y, lo, p.y);
                for (double s = -op.width * 0.5; s <= op.width * 0.5 + 0.1; s += x.g.c * 0.5)
                    for (double d = x.g.c * 0.5; d <= 150.0; d += x.g.c * 0.5)
                    {
                        int i = 0, j = 0;
                        if (!x.g.cellOf(x.g.local(at + along * s - nrm * d), i, j) || !p.mask[size_t(x.g.id(i, j))])
                            continue;
                        double& cell = l[size_t(x.g.id(i, j))];
                        cell = window ? std::min(cell, y) : y;
                    }
            }
    }
}

// Generation -----------------------------------------------------------------------------------------------------------------

std::shared_ptr<const Interior> generateInterior(const Spec& spec, const Result& r, const Style& st, const json& edits, const CharacterProfile& profile)
{
    LILA_ZONE();
    auto outPtr = std::make_shared<Interior>();
    Interior& out = *outPtr;
    out.building = r.id;
    Gen x{spec, r, st, profile, InteriorEdits::fromJson(edits), out};
    out.program = interiorProgramFor(spec, st, x.ed.program, &out.programFrom);
    if (out.programFrom == "fallback")
    {
        out.generic = true;
        out.warnings.push_back("no interior program for '" + (spec.program.empty() ? spec.use : spec.program) + "' (style " + st.id +
                               "): generic rooms stand in");
    }
    x.doorW = std::max(90.0, 2.0 * profile.radius + 20.0);
    x.doorH = std::max(210.0, profile.height + 20.0);
    if (r.plates.empty())
    {
        out.warnings.push_back("the building has no floor plates");
        return outPtr;
    }

    // The building's frame: along its longest wall.
    {
        const Ring& base = r.plates.front().ring;
        double best = 0.0;
        Vec2 ax{1.0, 0.0};
        for (size_t e = 0; e < base.size(); ++e)
        {
            const Vec2 d = base[(e + 1) % base.size()] - base[e];
            if (glm::length(d) > best)
            {
                best = glm::length(d);
                ax = d / best;
            }
        }
        x.g.ax = ax;
        x.g.az = {-ax.y, ax.x};
        x.g.o = base.empty() ? Vec2(0.0) : base.front();
        double wallMin = 1e9;
        for (const FloorPlate& p : r.plates)
            wallMin = std::min(wallMin, p.wall);
        x.g.c = std::clamp(std::floor(wallMin * 0.66), 12.0, 20.0);
        Vec2 lo(1e300), hi(-1e300);
        for (const FloorPlate& p : r.plates)
            for (const Vec2& q : p.ring)
            {
                const Vec2 l = x.g.local(q);
                lo = glm::min(lo, l);
                hi = glm::max(hi, l);
            }
        x.g.u0 = std::floor(lo.x / x.g.c) * x.g.c - x.g.c;
        x.g.v0 = std::floor(lo.y / x.g.c) * x.g.c - x.g.c;
        x.g.nx = int(std::ceil((hi.x - x.g.u0) / x.g.c)) + 2;
        x.g.nz = int(std::ceil((hi.y - x.g.v0) / x.g.c)) + 2;
        if (size_t(x.g.nx) * size_t(x.g.nz) > 6000000)
        {
            out.warnings.push_back("the building is too large for an interior grid");
            return outPtr;
        }
        x.stamp.assign(size_t(x.g.nx * x.g.nz), 0);
    }

    // Plates: towers skipped, a church or low warehouse as one tall volume.
    const bool volume = out.program == "church" || (out.program == "warehouse" && r.floors <= 2);
    const double arcadeD = std::min(st.arcadeDepth, 450.0);
    std::map<int, int> perFloor;
    for (const FloorPlate& fp : r.plates)
        if (!fp.tower)
            ++perFloor[fp.floor];
    for (size_t pi = 0; pi < r.plates.size(); ++pi)
    {
        const FloorPlate& fp = r.plates[pi];
        if (fp.tower || fp.ring.size() < 3)
            continue;
        if (volume && !(fp.tier == 0 && fp.floor == 0))
            continue;
        PlateW p;
        p.index = int(pi);
        p.tier = fp.tier;
        p.floor = fp.floor;
        p.y = fp.y;
        p.h = fp.height;
        p.volume = volume;
        if (volume)
        {
            double top = fp.y + fp.height;
            for (const FloorPlate& q : r.plates)
                if (q.tier == 0 && !q.tower)
                    top = std::max(top, q.y + q.height);
            p.h = top - fp.y;
        }
        p.prefix = perFloor[fp.floor] > 1 ? "t" + std::to_string(fp.tier) + "/l" + std::to_string(fp.floor) : "l" + std::to_string(fp.floor);
        // Arcade facades of the ground floor stand back by the arcade's depth.
        std::vector<double> inset(fp.ring.size(), fp.wall), half(fp.ring.size(), fp.wall * 0.5);
        if (fp.floor == 0)
            for (size_t e = 0; e < fp.ring.size(); ++e)
            {
                const Vec2 a = fp.ring[e], b = fp.ring[(e + 1) % fp.ring.size()];
                const Vec2 d = glm::normalize(b - a + Vec2(1e-12));
                for (const Bay& bay : r.bays)
                {
                    if (bay.kind != "arcade" || bay.floor != 0 || bay.tier != fp.tier)
                        continue;
                    const Vec2 c0{bay.corners[0].x, bay.corners[0].z};
                    const Vec2 rel = c0 - a;
                    if (std::abs(rel.x * d.y - rel.y * d.x) < 2.0 && glm::dot(rel, d) > -2.0 && glm::dot(rel, d) < glm::length(b - a) + 2.0)
                    {
                        inset[e] += arcadeD;
                        half[e] += arcadeD;
                        break;
                    }
                }
            }
        p.loops.push_back(insetRing(fp.ring, inset));
        p.loopIsHole.push_back(0);
        const Ring mid = insetRing(fp.ring, half);
        std::vector<Ring> midHoles;
        for (const Ring& h : fp.holes)
        {
            const Ring hc = cleanRing(h, true);
            if (hc.size() < 3)
                continue;
            p.loops.push_back(offsetRing(hc, fp.wall));
            p.loopIsHole.push_back(1);
            midHoles.push_back(offsetRing(hc, fp.wall * 0.5));
        }
        const size_t n = size_t(x.g.nx * x.g.nz);
        p.mask.assign(n, 0);
        p.clear.assign(n, 0);
        p.room.assign(n, kNone);
        std::vector<double> deeper = inset;
        for (double& v : deeper)
            v += x.g.c * 0.75;
        const Ring clearRing = insetRing(fp.ring, deeper);
        std::vector<Ring> clearHoles;
        for (const Ring& h : fp.holes)
            if (const Ring hc = cleanRing(h, true); hc.size() >= 3)
                clearHoles.push_back(offsetRing(hc, fp.wall + x.g.c * 0.75));
        Vec2 lo(1e300), hi(-1e300);
        for (const Vec2& q : mid)
        {
            const Vec2 l = x.g.local(q);
            lo = glm::min(lo, l);
            hi = glm::max(hi, l);
        }
        for (int j = 0; j < x.g.nz; ++j)
            for (int i = 0; i < x.g.nx; ++i)
            {
                const Vec2 l = x.g.centreL(i, j);
                if (l.x < lo.x || l.x > hi.x || l.y < lo.y || l.y > hi.y)
                    continue;
                const Vec2 w = x.g.world(l.x, l.y);
                if (!inside(mid, w))
                    continue;
                bool hole = false;
                for (const Ring& h : midHoles)
                    hole = hole || inside(h, w);
                if (hole)
                    continue;
                p.mask[size_t(x.g.id(i, j))] = 1;
                bool clearHole = false;
                for (const Ring& h : clearHoles)
                    clearHole = clearHole || inside(h, w);
                p.clear[size_t(x.g.id(i, j))] = char(!clearHole && inside(clearRing, w));
            }
        // Openings of this plate: same tier and floor (a volume takes every floor of its tier).
        const std::string tierPrefix = "t" + std::to_string(fp.tier) + "/";
        for (size_t o = 0; o < r.openings.size(); ++o)
        {
            const Opening& op = r.openings[o];
            if (op.bay.rfind(tierPrefix, 0) != 0)
                continue;
            const size_t lp = op.bay.find("/l");
            const int fl = lp == std::string::npos ? -1 : std::atoi(op.bay.c_str() + lp + 2);
            if (!volume && fl != fp.floor)
                continue;
            p.openings.push_back(int(o));
            if (fl == 0 && (op.kind == "door" || op.kind == "shopdoor" || op.kind == "garage") && op.bay.find("/h") == std::string::npos)
            {
                const Vec2 nrm = glm::normalize(Vec2(op.normal.x, op.normal.z) + Vec2(1e-12));
                const Vec2 inPt = Vec2(op.origin.x, op.origin.z) - nrm * (fp.wall + x.g.c * 1.5);
                int i = 0, j = 0;
                if (x.g.cellOf(x.g.local(inPt), i, j) && p.mask[size_t(x.g.id(i, j))])
                {
                    p.entryCells.push_back(x.g.id(i, j));
                    p.entryOpenings.push_back(int(o));
                }
            }
        }
        x.plates.push_back(std::move(p));
    }
    if (x.plates.empty())
    {
        out.warnings.push_back("no floor of the building can take rooms");
        return outPtr;
    }

    // The core: through every plate, else through the ones of the tier holding the W3 core point.
    {
        std::vector<int> served;
        std::set<int> floorsSeen;
        for (size_t pi = 0; pi < x.plates.size(); ++pi)
            if (!x.plates[pi].volume)
                served.push_back(int(pi));
        std::sort(served.begin(), served.end(), [&](int a, int b) { return x.plates[size_t(a)].floor < x.plates[size_t(b)].floor; });
        int maxFloor = 0;
        for (int pi : served)
            maxFloor = std::max(maxFloor, x.plates[size_t(pi)].floor);
        if (maxFloor > 0)
        {
            const bool houseLike = out.program == "house" || out.program == "shop" || out.program == "bar" || out.program == "mansion" || out.program == "garage";
            Vec2 pref = r.core;
            if (houseLike && glm::length(spec.front) > 1e-9)
            {
                double depth = 0.0;
                for (const Vec2& q : r.plates.front().ring)
                    depth = std::max(depth, glm::dot(q - r.core, -glm::normalize(spec.front)));
                pref = r.core - glm::normalize(spec.front) * depth * 0.5;
            }
            const bool lift = r.floors >= 4 || ((out.program == "apartment" || out.program == "office" || out.program == "hotel") && r.floors >= 3);
            // One plate per floor in the stack: the first of each floor (by tier) that contains the core point.
            std::vector<int> stack;
            for (int pi : served)
            {
                const int f = x.plates[size_t(pi)].floor;
                bool dup = false;
                for (int q : stack)
                    dup = dup || x.plates[size_t(q)].floor == f;
                if (!dup)
                    stack.push_back(pi);
            }
            placeCore(x, stack, pref, houseLike, lift);
            if (x.cores.empty())
            {
                std::vector<int> sub;
                for (int pi : stack)
                    if (inside(r.plates[size_t(x.plates[size_t(pi)].index)].ring, r.core))
                        sub.push_back(pi);
                if (sub.size() >= 2 && sub.size() < stack.size())
                {
                    out.warnings.pop_back();
                    placeCore(x, sub, pref, houseLike, lift);
                }
            }
        }
    }

    // Hubs: the main entry, else the core's landing, else the middle.
    for (PlateW& p : x.plates)
    {
        if (!p.entryCells.empty())
        {
            size_t best = 0;
            for (size_t k = 0; k < p.entryOpenings.size(); ++k)
                if (r.openings[size_t(p.entryOpenings[k])].kind != "garage")
                {
                    best = k;
                    break;
                }
            p.hubCell = p.entryCells[best];
        }
        else if (p.core >= 0)
            p.hubCell = coreAccessCell(x, x.cores[size_t(p.core)], p);
        if (p.hubCell < 0 || !p.mask[size_t(p.hubCell)] || p.room[size_t(p.hubCell)] == kCoreCell)
        {
            std::vector<int> cells;
            for (size_t c = 0; c < p.mask.size(); ++c)
                if (p.mask[c] && p.room[c] != kCoreCell)
                    cells.push_back(int(c));
            Vec2 centre(0.0);
            for (int c : cells)
                centre += x.g.centreL(c % x.g.nx, c / x.g.nx);
            centre /= double(std::max<size_t>(1, cells.size()));
            p.hubCell = cells.empty() ? -1 : nearestCell(x, cells, centre);
        }
    }

    // Rooms.
    for (size_t pi = 0; pi < x.plates.size(); ++pi)
    {
        PlateW& p = x.plates[pi];
        std::vector<int> cells;
        for (size_t c = 0; c < p.mask.size(); ++c)
            if (p.mask[c] && p.room[c] != kCoreCell)
                cells.push_back(int(c));
        if (cells.empty() || p.hubCell < 0)
            continue;
        std::vector<Seg> segs;
        for (int o : p.openings)
        {
            const Opening& op = r.openings[size_t(o)];
            const Vec2 nrm = glm::normalize(Vec2(op.normal.x, op.normal.z) + Vec2(1e-12));
            const Vec2 along{-nrm.y, nrm.x};
            const Vec2 c = Vec2(op.origin.x, op.origin.z) - nrm * r.plates[size_t(p.index)].wall;
            segs.push_back({x.g.local(c - along * (op.width * 0.5 + 15.0)), x.g.local(c + along * (op.width * 0.5 + 15.0))});
        }
        const double area = areaOf(x, cells.size());
        const Role role = p.volume ? Role::Volume : r.floors == 1 ? Role::Single : p.floor == 0 ? Role::Ground : Role::Upper;
        const Layout L = layoutFor(out.program, role, area);
        const Vec2 hubL = x.g.centreL(p.hubCell % x.g.nx, p.hubCell / x.g.nx);
        const double corrW = std::max(150.0, 2.0 * profile.radius + 60.0);
        int imin = x.g.nx, imax = -1, jmin = x.g.nz, jmax = -1;
        for (int c : cells)
        {
            const int i = c % x.g.nx, j = c / x.g.nx;
            imin = std::min(imin, i), imax = std::max(imax, i), jmin = std::min(jmin, j), jmax = std::max(jmax, j);
        }
        const double shortSide = std::min(imax - imin + 1, jmax - jmin + 1) * x.g.c;
        if (L.corridor && area >= 2.2 * L.unitArea && shortSide >= corrW + 2.0 * 280.0)
            layoutCorridor(x, int(pi), L, cells, segs);
        else
        {
            std::vector<Tpl> tpls = L.corridor ? L.unit : L.rooms;
            if (L.corridor)
                for (const Tpl& s : L.specials)
                    if (s.fixed < area * 0.3)
                        tpls.push_back(s);
            tpls = fitToArea(tpls, area);
            treemap(x, p, cells, itemsFor(x, int(pi), tpls, area, "", ""), hubL, segs);
        }
        fillUnassigned(x, p);
        tidy(x, p, int(pi), 1.5);
        clearOpenings(x, int(pi));
        tidy(x, p, int(pi), 1.5);
    }
    applyLayoutEdits(x);
    for (size_t pi = 0; pi < x.plates.size(); ++pi)
    {
        clearOpenings(x, int(pi));
        tidy(x, x.plates[pi], int(pi), 0.0);
    }
    for (size_t pi = 0; pi < x.plates.size(); ++pi)
        solveDoors(x, int(pi));

    // The user's doors.
    for (const std::string& id : x.ed.removeDoors)
    {
        const auto it = std::find_if(x.doors.begin(), x.doors.end(), [&](const DoorW& d) { return d.id == id; });
        if (it == x.doors.end())
            out.orphanEdits.push_back("remove door " + id);
        else
            x.doors.erase(it);
    }
    for (const InteriorEdits::DoorAdd& add : x.ed.addDoors)
    {
        auto entity = [&](const std::string& id, int& plate) {
            const size_t at = id.find('@');
            if (id.rfind("core", 0) == 0 && at != std::string::npos)
            {
                const std::string pre = id.substr(at + 1);
                for (size_t pi = 0; pi < x.plates.size(); ++pi)
                    if (x.plates[pi].prefix == pre)
                        plate = int(pi);
                return kCoreCell;
            }
            const int k = findRoom(x, id);
            if (k >= 0)
                plate = x.rooms[size_t(k)].plate;
            return k >= 0 ? k : kNone;
        };
        int pa = -1, pb = -1;
        const int ea = entity(add.a, pa), eb = entity(add.b, pb);
        if (ea == kNone || eb == kNone || pa < 0 || pa != pb)
        {
            out.orphanEdits.push_back("door " + add.a + " to " + add.b);
            continue;
        }
        const PlateW& p = x.plates[size_t(pa)];
        const double width = add.width > 0.0 ? add.width : x.doorW;
        Run run;
        double along = 0.0;
        std::optional<Vec2> atL;
        if (add.at)
            atL = x.g.local(*add.at);
        if (!runBetween(x, p, ea, eb, atL, run, along, width))
        {
            out.warnings.push_back("no wall between " + add.a + " and " + add.b + " is long enough for a door");
            continue;
        }
        DoorW d;
        d.plate = pa;
        d.a = run.a;
        d.b = run.b;
        d.run = run;
        d.at = along;
        d.width = width;
        d.height = x.doorH;
        d.user = true;
        d.id = doorIdFor(x, p, ea, eb);
        for (int k = 2; std::any_of(x.doors.begin(), x.doors.end(), [&](const DoorW& o) { return o.id == d.id; }); ++k)
            d.id = doorIdFor(x, p, ea, eb) + "#" + std::to_string(k);
        x.doors.push_back(d);
    }

    // Stepped ground floors: W3 steps the ground-floor bays down a slope, and the floor behind them steps with them.
    if (spec.sloped())
        stepGroundFloor(x);
    else
        x.level.assign(x.plates.size(), {});

    // Final rooms, compacted.
    std::vector<int> remap(x.rooms.size(), -1);
    for (size_t k = 0; k < x.rooms.size(); ++k)
    {
        const RoomW& rw = x.rooms[k];
        if (!rw.alive)
            continue;
        const PlateW& p = x.plates[size_t(rw.plate)];
        const std::vector<int> cells = cellsOf(p, int(k));
        if (cells.empty())
            continue;
        Room room;
        room.id = x.roomId(int(k));
        room.kind = rw.kind;
        room.unit = rw.unit;
        room.tier = p.tier;
        room.floor = p.floor;
        const int pi = rw.plate;
        room.y = p.y;
        for (int c : cells)
            room.y = std::min(room.y, x.levelAt(pi, c));
        room.height = p.y + p.h - room.y;
        room.area = areaOf(x, cells.size());
        room.user = rw.user;
        // Rectangles, greedy.
        std::vector<char> used(p.room.size(), 0);
        double bestA = -1.0;
        for (int c : cells)
        {
            if (used[size_t(c)])
                continue;
            const int i0 = c % x.g.nx, j0 = c / x.g.nx;
            const double here = x.levelAt(pi, i0, j0);
            auto same = [&](int i, int j) {
                const size_t id = size_t(x.g.id(i, j));
                return p.room[id] == int(k) && !used[id] && x.levelAt(pi, i, j) == here;
            };
            int i1 = i0 + 1;
            while (i1 < x.g.nx && same(i1, j0))
                ++i1;
            int j1 = j0 + 1;
            for (; j1 < x.g.nz; ++j1)
            {
                bool rowOk = true;
                for (int i = i0; i < i1 && rowOk; ++i)
                    rowOk = same(i, j1);
                if (!rowOk)
                    break;
            }
            for (int j = j0; j < j1; ++j)
                for (int i = i0; i < i1; ++i)
                    used[size_t(x.g.id(i, j))] = 1;
            const double U0 = x.g.u0 + i0 * x.g.c, V0 = x.g.v0 + j0 * x.g.c, U1 = x.g.u0 + i1 * x.g.c, V1 = x.g.v0 + j1 * x.g.c;
            room.rects.push_back({x.g.world(U0, V0), x.g.world(U1, V0), x.g.world(U1, V1), x.g.world(U0, V1)});
            room.rectY.push_back(here);
            const double a = (U1 - U0) * (V1 - V0);
            if (a > bestA)
            {
                bestA = a;
                room.label = x.g.world((U0 + U1) * 0.5, (V0 + V1) * 0.5);
            }
        }
        for (int c : p.entryCells)
            if (p.room[size_t(c)] == int(k))
                room.entry = true;
        remap[k] = int(out.rooms.size());
        out.rooms.push_back(std::move(room));
    }
    for (const PlateW& p : x.plates)
        for (size_t e = 0; e < p.entryCells.size(); ++e)
            if (p.room[size_t(p.entryCells[e])] >= 0)
            {
                const std::string path = r.openings[size_t(p.entryOpenings[e])].bay + "/door";
                if (std::find(out.entries.begin(), out.entries.end(), path) == out.entries.end())
                    out.entries.push_back(path);
            }
    if (out.entries.empty())
        out.warnings.push_back("no exterior door opens into a room: give the ground floor a door (buildingEdit bays=) to enter");

    // Doors out.
    for (const DoorW& d : x.doors)
    {
        InteriorDoor o;
        o.id = d.id;
        o.slot = d.id;
        o.a = x.entityId(d.plate, d.a);
        o.b = x.entityId(d.plate, d.b);
        const PlateW& p = x.plates[size_t(d.plate)];
        const Vec2 l = runPoint(x, d.run, d.at);
        const Vec2 w = x.g.world(l.x, l.y);
        const auto [la, lb] = doorLevels(x, d);
        o.origin = {w.x, std::max(la, lb), w.y};
        if (const double rise = std::abs(la - lb); rise > 0.5)
        {
            Stair steps;
            steps.id = d.id + "/steps";
            steps.kind = "steps";
            steps.floor = p.floor;
            steps.y0 = std::min(la, lb);
            steps.y1 = std::max(la, lb);
            steps.risers = std::max(1, int(std::ceil(rise / kDoorRiser)));
            steps.riser = rise / steps.risers;
            steps.going = kDoorGoing;
            steps.slope = std::atan2(steps.riser, steps.going) * 180.0 / std::numbers::pi;
            steps.width = d.width;
            steps.headroom = d.height;
            out.stairs.push_back(steps);
        }
        o.normal = d.run.axis == 0 ? x.g.dirU() : x.g.dirV();
        o.width = d.width;
        o.height = d.height;
        o.thickness = kWall;
        o.floor = p.floor;
        o.user = d.user;
        o.open = d.open;
        o.up = d.up;
        for (Room& rm : out.rooms)
            if (rm.id == o.a || rm.id == o.b)
                rm.doors.push_back(o.id);
        out.doors.push_back(o);
    }

    // Reachability: from the entry rooms through doors, up and down the cores' stairs.
    {
        std::map<std::string, std::vector<std::string>> adj;
        for (const InteriorDoor& d : out.doors)
            if (!d.up)
            {
                adj[d.a].push_back(d.b);
                adj[d.b].push_back(d.a);
            }
        for (const CoreW& k : x.cores)
        {
            std::vector<int> ps = k.plates;
            std::sort(ps.begin(), ps.end(), [&](int a, int b) { return x.plates[size_t(a)].floor < x.plates[size_t(b)].floor; });
            for (size_t q = 0; q + 1 < ps.size(); ++q)
            {
                const std::string here = k.id + "@" + x.plates[size_t(ps[q])].prefix;
                const std::string b = k.id + "@" + x.plates[size_t(ps[q + 1])].prefix;
                std::string a = here;
                // A straight flight above the lowest floor starts in the room before its foot, not where the one below arrived.
                if (k.kind == CoreKind::Straight && q > 0)
                {
                    a.clear();
                    for (const InteriorDoor& d : out.doors)
                        if (d.up && (d.a == here || d.b == here))
                            a = d.a == here ? d.b : d.a;
                    if (a.empty())
                        continue;
                }
                adj[a].push_back(b);
                adj[b].push_back(a);
            }
        }
        std::set<std::string> seen;
        std::deque<std::string> q;
        for (const Room& rm : out.rooms)
            if (rm.entry)
            {
                seen.insert(rm.id);
                q.push_back(rm.id);
            }
        while (!q.empty())
        {
            const std::string c = q.front();
            q.pop_front();
            for (const std::string& n : adj[c])
                if (seen.insert(n).second)
                    q.push_back(n);
        }
        for (Room& rm : out.rooms)
            rm.reachable = seen.count(rm.id) > 0;
    }

    // Grids for picking.
    for (const PlateW& p : x.plates)
    {
        FloorGrid fg;
        fg.tier = p.tier;
        fg.floor = p.floor;
        fg.y = p.y;
        if (const std::vector<double>& l = x.level[size_t(&p - x.plates.data())]; !l.empty())
        {
            fg.level.assign(l.begin(), l.end());
            for (size_t c = 0; c < l.size(); ++c)
                if (p.room[c] == kCoreCell)
                    fg.level[c] = p.y;
                else if (p.mask[c])
                    fg.y = std::min(fg.y, l[c]);
        }
        fg.height = p.y + p.h - fg.y;
        fg.origin = x.g.o;
        fg.ax = x.g.ax;
        fg.az = x.g.az;
        fg.u0 = x.g.u0;
        fg.v0 = x.g.v0;
        fg.cell = x.g.c;
        fg.nx = x.g.nx;
        fg.nz = x.g.nz;
        fg.prefix = p.prefix;
        fg.room.resize(p.room.size());
        for (size_t c = 0; c < p.room.size(); ++c)
            fg.room[c] = p.room[c] >= 0 ? (p.mask[c] ? remap[size_t(p.room[c])] : -1) : p.room[c];
        out.grids.push_back(std::move(fg));
    }

    // Geometry ------------------------------------------------------------------------------------------------------------------
    std::map<int, size_t> partOf; // room index (in x.rooms) -> part
    std::vector<size_t> corePart;
    for (size_t k = 0; k < x.rooms.size(); ++k)
        if (remap[k] >= 0)
        {
            partOf[int(k)] = out.parts.size();
            InteriorPart part;
            part.id = out.rooms[size_t(remap[k])].id;
            part.floor = x.rooms[k].plate >= 0 ? x.plates[size_t(x.rooms[k].plate)].floor : -1;
            out.parts.push_back(std::move(part));
        }
    for (const CoreW& k : x.cores)
    {
        corePart.push_back(out.parts.size());
        out.parts.push_back({k.id, -1, {}});
    }
    // Ceilings are a part per floor, so a cutaway can lift them off the floor being worked on.
    std::vector<size_t> ceilingPart;
    for (const PlateW& p : x.plates)
    {
        ceilingPart.push_back(out.parts.size());
        out.parts.push_back({p.prefix + "/ceiling", p.floor, {}});
    }
    auto soupFor = [&](const PlateW& p, int e) -> Soup* {
        if (e == kCoreCell)
            return p.core >= 0 ? &out.parts[corePart[size_t(p.core)]].soup : nullptr;
        auto it = partOf.find(e);
        return it == partOf.end() ? nullptr : &out.parts[it->second].soup;
    };
    auto kindOfE = [&](int e) { return e >= 0 ? x.rooms[size_t(e)].kind : std::string("core"); };

    for (const PlateW& p : x.plates)
    {
        const FloorPlate& fp = r.plates[size_t(p.index)];
        const int pIndex = int(&p - x.plates.data());
        const double yc = p.y + p.h - kSlab;
        // Floors and ceilings.
        for (size_t k = 0; k < x.rooms.size(); ++k)
        {
            if (remap[k] < 0 || x.rooms[k].plate != int(&p - x.plates.data()))
                continue;
            Soup& s = out.parts[partOf[int(k)]].soup;
            const Room& rm = out.rooms[size_t(remap[k])];
            const int fm = floorMat(rm.kind);
            for (size_t qi = 0; qi < rm.rects.size(); ++qi)
            {
                const auto& q = rm.rects[qi];
                const double fy = qi < rm.rectY.size() ? rm.rectY[qi] : rm.y;
                std::vector<Vec3> f, c;
                for (const Vec2& v : q)
                {
                    f.push_back({v.x, fy, v.y});
                    c.push_back({v.x, yc, v.y});
                }
                face(s, f, kIntUp, fm);
                face(out.parts[ceilingPart[size_t(&p - x.plates.data())]].soup, c, -kIntUp, IntCeiling);
            }
            // A riser where the room's floor steps between terraces, facing the lower one.
            if (!x.level[size_t(pIndex)].empty())
                for (int c : cellsOf(p, int(k)))
                {
                    const int i = c % x.g.nx, j = c / x.g.nx;
                    for (int axis = 0; axis < 2; ++axis)
                    {
                        const int ni = i + (axis == 0 ? 1 : 0), nj = j + (axis == 1 ? 1 : 0);
                        if (ni >= x.g.nx || nj >= x.g.nz || p.room[size_t(x.g.id(ni, nj))] != int(k))
                            continue;
                        const double l0 = x.levelAt(pIndex, i, j), l1 = x.levelAt(pIndex, ni, nj);
                        if (std::abs(l0 - l1) < 0.5)
                            continue;
                        // The shared edge in grid-local space, and which way the lower side lies.
                        const double eu = x.g.u0 + (axis == 0 ? ni : i) * x.g.c, ev = x.g.v0 + (axis == 1 ? nj : j) * x.g.c;
                        const Vec2 la = Vec2(eu, ev), lb = axis == 0 ? Vec2(eu, ev + x.g.c) : Vec2(eu + x.g.c, ev);
                        const Vec3 toward = (axis == 0 ? x.g.dirU() : x.g.dirV()) * (l1 < l0 ? 1.0 : -1.0);
                        const double lo = std::min(l0, l1), hi = std::max(l0, l1);
                        quadF(s, x.g.w3(la.x, la.y, lo), x.g.w3(lb.x, lb.y, lo), x.g.w3(lb.x, lb.y, hi), x.g.w3(la.x, la.y, hi), toward, fm);
                    }
                }
        }
        // The rooms' side of the outside walls, openings cut through.
        for (size_t li = 0; li < p.loops.size(); ++li)
        {
            const Ring& loop = p.loops[li];
            const bool hole = p.loopIsHole[li] != 0;
            for (size_t e = 0; e < loop.size(); ++e)
            {
                const Vec2 a = loop[e], b = loop[(e + 1) % loop.size()];
                const double L = glm::length(b - a);
                if (L < 5.0)
                    continue;
                const Vec2 dir = (b - a) / L;
                const Vec2 out2 = outward(a, b);
                const Vec2 into = hole ? out2 : -out2; // toward the rooms
                const Vec3 P{a.x, 0.0, a.y}, U{dir.x, 0.0, dir.y}, N{into.x, 0.0, into.y};
                // Who owns the wall along the edge.
                std::vector<std::pair<double, int>> marks;
                std::vector<double> markLevel; // the floor's level there (a stepped ground floor's terrace)
                const double step = x.g.c * 0.5;
                for (double s = step * 0.5; s < L; s += step)
                {
                    const Vec2 q = a + dir * s + into * (x.g.c * 0.75);
                    int i = 0, j = 0, ent = kNone;
                    double lvl = p.y;
                    if (x.g.cellOf(x.g.local(q), i, j))
                        ent = p.room[size_t(x.g.id(i, j))], lvl = x.levelAt(pIndex, i, j);
                    if (ent == kNone)
                    {
                        const Vec2 q2 = a + dir * s + into * (x.g.c * 1.6);
                        if (x.g.cellOf(x.g.local(q2), i, j))
                            ent = p.room[size_t(x.g.id(i, j))], lvl = x.levelAt(pIndex, i, j);
                    }
                    marks.push_back({s, ent});
                    markLevel.push_back(lvl);
                }
                for (size_t m = 0; m < marks.size(); ++m)
                    if (marks[m].second == kNone)
                    {
                        // Gaps take the nearest owner either side.
                        for (size_t d = 1; d < marks.size(); ++d)
                        {
                            if (m >= d && marks[m - d].second != kNone)
                            {
                                marks[m].second = marks[m - d].second;
                                markLevel[m] = markLevel[m - d];
                                break;
                            }
                            if (m + d < marks.size() && marks[m + d].second != kNone)
                            {
                                marks[m].second = marks[m + d].second;
                                markLevel[m] = markLevel[m + d];
                                break;
                            }
                        }
                    }
                // Openings on this face.
                std::vector<Hole> facadeHoles;
                std::vector<int> holeOpening;
                for (int o : p.openings)
                {
                    const Opening& op = r.openings[size_t(o)];
                    const Vec2 on{op.normal.x, op.normal.z};
                    if (glm::dot(on, -into) < 0.9)
                        continue;
                    const Vec2 rel = Vec2(op.origin.x, op.origin.z) - a;
                    const double dist = glm::dot(rel, -into);
                    if (std::abs(dist - fp.wall) > 10.0)
                        continue;
                    const double s = glm::dot(rel, dir);
                    if (s < -op.width * 0.5 || s > L + op.width * 0.5)
                        continue;
                    facadeHoles.push_back({s - op.width * 0.5, s + op.width * 0.5, op.origin.y, std::min(op.origin.y + op.height, yc)});
                    holeOpening.push_back(o);
                }
                size_t m0 = 0;
                while (m0 < marks.size())
                {
                    size_t m1 = m0;
                    while (m1 + 1 < marks.size() && marks[m1 + 1].second == marks[m0].second && markLevel[m1 + 1] == markLevel[m0])
                        ++m1;
                    const int ent = marks[m0].second;
                    const double s0 = m0 == 0 ? 0.0 : (marks[m0 - 1].first + marks[m0].first) * 0.5;
                    const double s1 = m1 + 1 == marks.size() ? L : (marks[m1].first + marks[m1 + 1].first) * 0.5;
                    Soup* s = ent == kNone ? nullptr : soupFor(p, ent);
                    if (s)
                    {
                        const int wm = kindOfE(ent) == "bathroom" || kindOfE(ent) == "toilet" ? IntWallTile : (ent == kCoreCell ? IntShaft : IntWall);
                        const double y0 = markLevel[m0];
                        std::vector<Hole> holes = facadeHoles;
                        for (Hole& hh : holes)
                            hh.t0 = std::max(hh.t0, y0);
                        wallFace(*s, P, U, N, s0, s1, y0, yc, holes, wm);
                        skirting(*s, P, U, N, s0, s1, y0, holes);
                        // Reveals through the rest of the wall, sills under windows.
                        const double depth = std::max(0.0, fp.wall - st.reveal);
                        for (size_t h = 0; h < holes.size(); ++h)
                        {
                            const Hole& hh = holes[h];
                            const double c0 = std::max(hh.s0, s0), c1 = std::min(hh.s1, s1);
                            if (c1 - c0 < 1.0)
                                continue;
                            const Vec3 back = -N * depth;
                            auto at = [&](double ss, double tt) { return P + U * ss + kIntUp * tt; };
                            if (depth > 0.5)
                            {
                                if (hh.s0 >= s0 - 0.5)
                                    quadF(*s, at(hh.s0, hh.t0), at(hh.s0, hh.t1), at(hh.s0, hh.t1) + back, at(hh.s0, hh.t0) + back, U, wm);
                                if (hh.s1 <= s1 + 0.5)
                                    quadF(*s, at(hh.s1, hh.t0), at(hh.s1, hh.t1), at(hh.s1, hh.t1) + back, at(hh.s1, hh.t0) + back, -U, wm);
                                quadF(*s, at(c0, hh.t1), at(c1, hh.t1), at(c1, hh.t1) + back, at(c0, hh.t1) + back, -kIntUp, wm);
                                if (hh.t0 > y0 + 1.0)
                                    quadF(*s, at(c0, hh.t0), at(c1, hh.t0), at(c1, hh.t0) + back, at(c0, hh.t0) + back, kIntUp, wm);
                            }
                            if (hh.t0 > y0 + 30.0)
                            {
                                // A sill board.
                                const Vec3 o = N * 5.0;
                                quadF(*s, at(c0 - 4.0, hh.t0), at(c1 + 4.0, hh.t0), at(c1 + 4.0, hh.t0) + o, at(c0 - 4.0, hh.t0) + o, kIntUp, IntTrim);
                                quadF(*s, at(c0 - 4.0, hh.t0 - 3.0) + o, at(c1 + 4.0, hh.t0 - 3.0) + o, at(c1 + 4.0, hh.t0) + o, at(c0 - 4.0, hh.t0) + o, N, IntTrim);
                            }
                        }
                    }
                    m0 = m1 + 1;
                }
            }
        }
        // Partitions: both faces, doors cut through, jambs and heads.
        const std::vector<Run> runs = runsOf(x, p);
        for (const Run& run : runs)
        {
            const double line = run.axis == 0 ? x.g.u0 + run.line * x.g.c : x.g.v0 + run.line * x.g.c;
            const Vec2 alongL = run.axis == 0 ? Vec2(0, 1) : Vec2(1, 0);
            const Vec2 acrossL = run.axis == 0 ? Vec2(1, 0) : Vec2(0, 1);
            double e0 = runStart(x, run), e1 = runEnd(x, run);
            // Cells beyond each end, on each side.
            auto beyond = [&](bool end, bool high) { return beyondRun(x, p, run, end, high); };
            std::vector<const DoorW*> here;
            for (const DoorW& d : x.doors)
                if (d.plate == pIndex && d.run.axis == run.axis && d.run.line == run.line && d.at > e0 && d.at < e1 &&
                    ((d.a == run.a && d.b == run.b) || (d.a == run.b && d.b == run.a)))
                    here.push_back(&d);
            // Each side's face reaches down to its lowest floor along the run; any higher terrace hides the rest under it.
            double sideLevel[2] = {p.y, p.y};
            for (int k = run.k0; k < run.k1; ++k)
                for (int side = 0; side < 2; ++side)
                {
                    const int l = run.line - (side == 0 ? 1 : 0);
                    sideLevel[side] = std::min(sideLevel[side], run.axis == 0 ? x.levelAt(pIndex, l, k) : x.levelAt(pIndex, k, l));
                }
            for (int side = 0; side < 2; ++side)
            {
                const int ent = side == 0 ? run.a : run.b;
                Soup* s = soupFor(p, ent);
                if (!s)
                    continue;
                // A door's opening reaches this side's floor; the higher side's floor is its threshold.
                const double y0 = sideLevel[side];
                std::vector<Hole> holes;
                for (const DoorW* d : here)
                {
                    const auto [da, db] = doorLevels(x, *d);
                    holes.push_back({d->at - d->width * 0.5, d->at + d->width * 0.5, side == 0 ? da : db, std::max(da, db) + d->height});
                }
                const double off = (side == 0 ? -1.0 : 1.0) * kWall * 0.5;
                double s0 = e0, s1 = e1;
                for (int end = 0; end < 2; ++end)
                {
                    const int mine = beyond(end == 1, side == 1), other = beyond(end == 1, side == 0);
                    double adj;
                    if (mine == kNone || other == kNone)
                    {
                        const double atEnd = end == 1 ? e1 : e0;
                        const Vec2 from = (run.axis == 0 ? Vec2(line + off, atEnd) : Vec2(atEnd, line + off));
                        const double t = clipToWall(x, p, from, alongL, atEnd);
                        adj = (end == 1 ? t + 2.0 : t - 2.0) - atEnd;
                    }
                    else
                        adj = (mine == ent ? 1.0 : -1.0) * kWall * 0.5 * (end == 1 ? 1.0 : -1.0);
                    (end == 1 ? s1 : s0) += adj;
                }
                const Vec2 o2 = x.g.world(run.axis == 0 ? line + off : 0.0, run.axis == 0 ? 0.0 : line + off);
                const Vec3 P{o2.x, 0.0, o2.y};
                const Vec3 U = run.axis == 0 ? x.g.dirV() : x.g.dirU();
                const Vec3 N = (run.axis == 0 ? x.g.dirU() : x.g.dirV()) * (side == 0 ? -1.0 : 1.0);
                const int wm = kindOfE(ent) == "bathroom" || kindOfE(ent) == "toilet" ? IntWallTile : (ent == kCoreCell ? IntShaft : IntWall);
                wallFace(*s, P, U, N, s0, s1, y0, yc, holes, wm);
                skirting(*s, P, U, N, s0, s1, y0, holes);
            }
            // Jambs and head of each door, in the first side's part.
            if (Soup* s = soupFor(p, run.a >= 0 ? run.a : run.b))
                for (const DoorW* d : here)
                {
                    const double h0 = -kWall * 0.5, h1 = kWall * 0.5;
                    auto at = [&](double al, double ac, double yy) {
                        const Vec2 lp = run.axis == 0 ? Vec2(line + ac, al) : Vec2(al, line + ac);
                        return x.g.w3(lp.x, lp.y, yy);
                    };
                    const Vec3 along3 = run.axis == 0 ? x.g.dirV() : x.g.dirU();
                    const auto [la, lb] = doorLevels(x, *d);
                    const double hiL = std::max(la, lb);
                    const double lo = std::min(la, lb), a0 = d->at - d->width * 0.5, a1 = d->at + d->width * 0.5, top = hiL + d->height;
                    quadF(*s, at(a0, h0, lo), at(a0, h1, lo), at(a0, h1, top), at(a0, h0, top), along3, IntTrim);
                    quadF(*s, at(a1, h0, lo), at(a1, h1, lo), at(a1, h1, top), at(a1, h0, top), -along3, IntTrim);
                    quadF(*s, at(a0, h0, top), at(a1, h0, top), at(a1, h1, top), at(a0, h1, top), -kIntUp, IntTrim);
                    (void)acrossL;
                    if (hiL - lo > 0.5)
                        if (Soup* down = soupFor(p, la < lb ? run.a : run.b))
                        {
                            // Steps down into the lower room: the threshold in the wall, then treads out from its face.
                            const double sgn = la < lb ? -1.0 : 1.0, face0 = sgn * kWall * 0.5;
                            const Vec3 into = (run.axis == 0 ? x.g.dirU() : x.g.dirV()) * sgn;
                            const int n = std::max(1, int(std::ceil((hiL - lo) / kDoorRiser)));
                            const double rr = (hiL - lo) / n;
                            quadF(*down, at(a0, h0, hiL), at(a1, h0, hiL), at(a1, h1, hiL), at(a0, h1, hiL), kIntUp, IntStair);
                            quadF(*down, at(a0, face0, lo), at(a1, face0, lo), at(a1, face0, hiL), at(a0, face0, hiL), into, IntStair);
                            for (int i = 0; i + 1 < n; ++i)
                            {
                                const double t = hiL - (i + 1) * rr, d0 = face0 + sgn * i * kDoorGoing, d1 = face0 + sgn * (i + 1) * kDoorGoing;
                                quadF(*down, at(a0, d0, t), at(a1, d0, t), at(a1, d1, t), at(a0, d1, t), kIntUp, IntStair);
                                quadF(*down, at(a0, d1, lo), at(a1, d1, lo), at(a1, d1, t), at(a0, d1, t), into, IntStair);
                                quadF(*down, at(a0, d0, lo), at(a0, d1, lo), at(a0, d1, t), at(a0, d0, t), -along3, IntStair);
                                quadF(*down, at(a1, d0, lo), at(a1, d1, lo), at(a1, d1, t), at(a1, d0, t), along3, IntStair);
                            }
                        }
                }
        }
    }

    // Cores: landings, flights, the lift shaft.
    for (size_t ki = 0; ki < x.cores.size(); ++ki)
    {
        const CoreW& k = x.cores[ki];
        Soup& s = out.parts[corePart[ki]].soup;
        auto P = [&](double a, double b, double yy) {
            const Vec2 l = k.ol + k.eL * a + k.eW * b;
            return x.g.w3(l.x, l.y, yy);
        };
        const Vec3 eL3 = P(1, 0, 0) - P(0, 0, 0), eW3 = P(0, 1, 0) - P(0, 0, 0);
        std::vector<int> ps = k.plates;
        std::sort(ps.begin(), ps.end(), [&](int a, int b) { return x.plates[size_t(a)].floor < x.plates[size_t(b)].floor; });
        const PlateW& bottom = x.plates[size_t(ps.front())];
        const PlateW& top = x.plates[size_t(ps.back())];
        quadF(s, P(0, 0, bottom.y), P(k.L, 0, bottom.y), P(k.L, k.coreW, bottom.y), P(0, k.coreW, bottom.y), kIntUp, IntFloorStone);
        const double ceil = top.y + top.h - kSlab;
        quadF(s, P(0, 0, ceil), P(k.L, 0, ceil), P(k.L, k.coreW, ceil), P(0, k.coreW, ceil), -kIntUp, IntCeiling);
        const double Wf = k.Wf, W = k.W, Ld = k.Ld, Lm = k.Lm;
        const bool straight = k.kind == CoreKind::Straight;
        for (size_t q = 0; q < ps.size(); ++q)
        {
            const PlateW& pl = x.plates[size_t(ps[q])];
            if (q > 0 && Ld > 0.0)
            {
                // The floor landing.
                quadF(s, P(0, 0, pl.y), P(Ld, 0, pl.y), P(Ld, k.coreW, pl.y), P(0, k.coreW, pl.y), kIntUp, IntFloorStone);
                quadF(s, P(0, 0, pl.y - kSlab), P(Ld, 0, pl.y - kSlab), P(Ld, k.coreW, pl.y - kSlab), P(0, k.coreW, pl.y - kSlab), -kIntUp, IntCeiling);
                quadF(s, P(Ld, Wf, pl.y - kSlab), P(Ld, W - Wf, pl.y - kSlab), P(Ld, W - Wf, pl.y), P(Ld, Wf, pl.y), eL3, IntStair);
            }
            if (q + 1 >= ps.size())
                continue;
            const PlateW& up = x.plates[size_t(ps[q + 1])];
            const double H = up.y - pl.y, ym = pl.y + H * 0.5;
            const double riserMax = std::min(18.0, profile.maxStep);
            const double flightH = straight ? H : H * 0.5;
            const int n = std::max(1, int(std::ceil(flightH / riserMax)));
            const double rr = flightH / n, gg = k.run / n;
            Stair stair;
            stair.id = k.id + "/" + pl.prefix;
            stair.core = k.id;
            stair.kind = coreKindName(k.kind);
            stair.floor = pl.floor;
            stair.y0 = pl.y;
            stair.y1 = up.y;
            stair.risers = straight ? n : 2 * n;
            stair.riser = rr;
            stair.going = gg;
            stair.slope = std::atan2(rr, gg) * 180.0 / std::numbers::pi;
            stair.width = straight ? W : Wf;
            stair.headroom = std::min(H, up.h) - kSlab - rr;
            stair.straight = straight;
            out.stairs.push_back(stair);
            if (!straight)
            {
                // The mid landing.
                quadF(s, P(k.L - Lm, 0, ym), P(k.L, 0, ym), P(k.L, W, ym), P(k.L - Lm, W, ym), kIntUp, IntFloorStone);
                quadF(s, P(k.L - Lm, 0, ym - 18.0), P(k.L, 0, ym - 18.0), P(k.L, W, ym - 18.0), P(k.L - Lm, W, ym - 18.0), -kIntUp, IntCeiling);
                quadF(s, P(k.L - Lm, Wf, ym - 18.0), P(k.L - Lm, W - Wf, ym - 18.0), P(k.L - Lm, W - Wf, ym), P(k.L - Lm, Wf, ym), -eL3, IntStair);
            }
            for (int flight = 0; flight < (straight ? 1 : 2); ++flight)
            {
                const double b0 = flight == 0 ? 0.0 : W - Wf, b1 = straight ? W : flight == 0 ? Wf : W;
                const double yBase = flight == 0 ? pl.y : ym;
                const double dirA = flight == 0 ? 1.0 : -1.0;
                const double aStart = flight == 0 ? Ld : k.L - Lm;
                const Vec3 fwd = eL3 * dirA;
                for (int i = 0; i < n; ++i)
                {
                    const double a0 = aStart + dirA * i * gg, a1 = a0 + dirA * gg;
                    const double yLo = yBase + i * rr, yHi = yLo + rr;
                    quadF(s, P(a0, b0, yLo), P(a0, b1, yLo), P(a0, b1, yHi), P(a0, b0, yHi), -fwd, IntStair);
                    quadF(s, P(a0, b0, yHi), P(a1, b0, yHi), P(a1, b1, yHi), P(a0, b1, yHi), kIntUp, IntStair);
                    const double s0 = yBase + i * rr - 18.0, s1 = yBase + (i + 1) * rr - 18.0;
                    face(s, {P(a0, b0, std::max(s0, yBase - 18.0)), P(a1, b0, s1), P(a1, b0, yHi), P(a0, b0, yHi)}, -eW3, IntStair);
                    face(s, {P(a0, b1, std::max(s0, yBase - 18.0)), P(a1, b1, s1), P(a1, b1, yHi), P(a0, b1, yHi)}, eW3, IntStair);
                }
                const double aEnd = aStart + dirA * n * gg;
                face(s, {P(aStart, b0, yBase - 18.0), P(aEnd, b0, yBase + flightH - 18.0), P(aEnd, b1, yBase + flightH - 18.0), P(aStart, b1, yBase - 18.0)},
                     -kIntUp + fwd * 0.3, IntStair);
            }
        }
        if (k.lift)
        {
            // The shaft beside the stair, its doors on the landing.
            const double b0 = W, b1 = k.coreW, yLo = bottom.y, yHi = ceil;
            wallFace(s, P(Ld, b0, 0) - kIntUp * 0.0, eL3, -eW3, 0.0, k.L - Ld, yLo, yHi, {}, IntShaft);
            std::vector<Hole> holes;
            for (int pi : ps)
            {
                const PlateW& pl = x.plates[size_t(pi)];
                const double mid = (b1 - b0) * 0.5;
                holes.push_back({mid - 55.0, mid + 55.0, pl.y, pl.y + 215.0});
            }
            wallFace(s, P(Ld, b0, 0), eW3, -eL3, 0.0, b1 - b0, yLo, yHi, holes, IntShaft);
            Module m;
            m.type = "lift";
            m.kind = "lift";
            m.path = k.id + "/lift";
            m.size = {160, 230, 160};
            const Vec3 c3 = P(Ld + 10.0 + 80.0, (b0 + b1) * 0.5, bottom.y);
            m.origin = c3;
            m.z = -eL3;
            m.y = kIntUp;
            m.x = glm::cross(m.y, m.z);
            m.data = {{"floors", int(ps.size())}};
            out.slots.push_back(std::move(m));
        }
    }

    // Slots: doors, then the rooms' furnishing and lamps.
    for (const InteriorDoor& d : out.doors)
    {
        if (d.open)
            continue;
        Module m;
        m.type = d.width >= 150.0 ? "door.interior.double" : "door.interior";
        m.kind = "door";
        m.path = d.slot;
        m.origin = d.origin;
        m.z = d.normal;
        m.y = kIntUp;
        m.x = glm::cross(m.y, m.z);
        m.size = {d.width, d.height, d.thickness};
        m.data = {{"a", d.a}, {"b", d.b}};
        out.slots.push_back(std::move(m));
    }
    if (x.ed.furnish)
        for (const Room& rm : out.rooms)
        {
            std::vector<Vec3> keep;
            for (const InteriorDoor& d : out.doors)
                if (d.a == rm.id || d.b == rm.id)
                {
                    keep.push_back(d.origin);
                    // Steps down into this room run out from the door.
                    const double rise = d.origin.y - rm.y;
                    const Vec3 into = d.b == rm.id ? d.normal : -d.normal;
                    for (double along = 60.0; rise > 0.5 && along < (std::ceil(rise / kDoorRiser) - 1.0) * kDoorGoing + 60.0; along += 60.0)
                        keep.push_back(d.origin + into * along);
                }
            for (const PlateW& p : x.plates)
                for (int c : p.entryCells)
                    if (p.room[size_t(c)] >= 0 && remap[size_t(p.room[size_t(c)])] >= 0 && out.rooms[size_t(remap[size_t(p.room[size_t(c)])])].id == rm.id)
                    {
                        const Vec2 l = x.g.centreL(c % x.g.nx, c / x.g.nx);
                        keep.push_back(x.g.w3(l.x, l.y, rm.y));
                    }
            std::vector<Module> f = furnishArea(rm.kind, rm.rects, rm.y, rm.y + rm.height - kSlab, keep, rm.id, fnv(rm.id));
            const bool terraced = std::any_of(rm.rectY.begin(), rm.rectY.end(), [&](double y) { return y != rm.y; });
            for (Module& m : f)
            {
                // On a terraced floor a piece stands on the terrace under it.
                if (terraced && std::abs(m.origin.y - rm.y) < 0.5)
                    if (const std::optional<double> y = out.floorAt({m.origin.x, rm.y + 1.0, m.origin.z}))
                        m.origin.y = *y;
                out.slots.push_back(std::move(m));
            }
        }
    return outPtr;
}

int Interior::roomAt(Vec3 p) const
{
    for (const FloorGrid& g : grids)
    {
        if (p.y < g.y - 5.0 || p.y > g.y + g.height)
            continue;
        const Vec2 d = Vec2(p.x, p.z) - g.origin;
        const double u = glm::dot(d, g.ax), v = glm::dot(d, g.az);
        const int i = int(std::floor((u - g.u0) / g.cell)), j = int(std::floor((v - g.v0) / g.cell));
        if (i < 0 || j < 0 || i >= g.nx || j >= g.nz)
            continue;
        const int r = g.room[size_t(j * g.nx + i)];
        if (r != -1)
            return r;
    }
    return -1;
}

std::optional<double> Interior::floorAt(Vec3 p) const
{
    for (const FloorGrid& g : grids)
    {
        if (p.y < g.y - 5.0 || p.y > g.y + g.height)
            continue;
        const Vec2 d = Vec2(p.x, p.z) - g.origin;
        const double u = glm::dot(d, g.ax), v = glm::dot(d, g.az);
        const int i = int(std::floor((u - g.u0) / g.cell)), j = int(std::floor((v - g.v0) / g.cell));
        if (i < 0 || j < 0 || i >= g.nx || j >= g.nz || g.room[size_t(j * g.nx + i)] == -1)
            continue;
        const size_t c = size_t(j * g.nx + i);
        return g.level.empty() ? g.y : g.level[c];
    }
    return std::nullopt;
}

const Room* Interior::room(const std::string& id) const
{
    for (const Room& r : rooms)
        if (r.id == id)
            return &r;
    return nullptr;
}

json checkInterior(const Interior& in, const CharacterProfile& p)
{
    json issues = json::array();
    json counts = {{"roomUnreachable", 0}, {"stairSteep", 0}, {"stairLow", 0}, {"doorNarrow", 0}, {"doorLow", 0}, {"noEntry", 0}};
    auto add = [&](const char* kind, const std::string& what, const std::string& id) {
        issues.push_back({{"kind", kind}, {"building", in.building}, {"id", id}, {"what", what}});
        counts[kind] = counts[kind].get<int>() + 1;
    };
    if (in.entries.empty())
        add("noEntry", "no exterior door opens into a room", in.building);
    for (const Room& r : in.rooms)
        if (!r.reachable)
            add("roomUnreachable", r.id + " (" + r.kind + ") can't be reached from the entry", r.id);
    const double maxRise = p.maxStep, tanMax = std::tan(p.maxSlope * std::numbers::pi / 180.0);
    for (const Stair& s : in.stairs)
    {
        if (s.riser > maxRise + 0.01)
            add("stairSteep", s.id + ": risers of " + std::to_string(int(std::lround(s.riser))) + " cm are higher than the character's step (" +
                                  std::to_string(int(p.maxStep)) + " cm)",
                s.id);
        else if (s.riser / std::max(s.going, 1e-9) > tanMax + 1e-6)
            add("stairSteep", s.id + ": " + std::to_string(int(std::lround(s.slope))) + " degrees is steeper than the character walks (" +
                                  std::to_string(int(p.maxSlope)) + ")",
                s.id);
        if (s.headroom < p.height)
            add("stairLow", s.id + ": headroom " + std::to_string(int(s.headroom)) + " cm under the character's " + std::to_string(int(p.height)) + " cm", s.id);
        if (s.width < 2.0 * p.radius)
            add("doorNarrow", s.id + ": the flight is narrower than the character", s.id);
    }
    for (const InteriorDoor& d : in.doors)
    {
        if (d.width < 2.0 * p.radius)
            add("doorNarrow", d.id + ": " + std::to_string(int(d.width)) + " cm wide, the character needs " + std::to_string(int(2.0 * p.radius)), d.id);
        if (d.height < p.height)
            add("doorLow", d.id + ": " + std::to_string(int(d.height)) + " cm high, the character is " + std::to_string(int(p.height)), d.id);
    }
    return {{"issues", issues}, {"counts", counts}, {"ok", issues.empty()}};
}
} // namespace buildings
