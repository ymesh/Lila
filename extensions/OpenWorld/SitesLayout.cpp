// Site kits: the kinds, their settings, the spec, the slot types they publish and the emitters every kit lays out with.

#include "SitesKit.h"

#include <algorithm>
#include <cmath>
#include <mutex>
#include <numbers>
#include <stdexcept>

namespace openworld::sites
{
namespace
{
using T = Param::Type;
constexpr double kNaN = std::numeric_limits<double>::quiet_NaN();

Param p(std::string name, T type, json def, std::string doc, double lo = kNaN, double hi = kNaN, std::vector<std::string> choices = {})
{
    return {.name = std::move(name), .type = type, .def = std::move(def), .doc = std::move(doc), .min = lo, .max = hi, .choices = std::move(choices)};
}

uint64_t mix(uint64_t a, uint64_t b) { return lila::names::hash64(a ^ (b + 0x9E3779B97F4A7C15ull + (a << 6) + (a >> 2))); }
} // namespace

const std::vector<Param>& params()
{
    static const std::vector<Param> all = {
        // Cargo port
        p("quayLength", T::Float, 60000.0, "Length of the quay and the yard behind it, cm.", 12000.0, 400000.0),
        p("yardDepth", T::Float, 32000.0, "Depth of the yard inland from the quay, cm.", 12000.0, 200000.0),
        p("berths", T::Int, 2, "Berths along the quay.", 1.0, 12.0),
        p("stsCranes", T::Int, 3, "Ship-to-shore cranes per berth.", 0.0, 8.0),
        p("blockRows", T::Int, 6, "Container rows per yard block (across the block).", 2.0, 10.0),
        p("tiers", T::Int, 4, "Highest container stack.", 1.0, 7.0),
        p("fill", T::Float, 0.75, "How full the yard is, 0 to 1: stacks are left out from the top down.", 0.0, 1.0),
        p("liveries", T::Int, 6, "Container liveries: fictional shipping lines.", 1.0, 16.0),
        p("rtgCranes", T::Int, 1, "Rubber-tyred gantry cranes per yard block.", 0.0, 3.0),
        p("reeferRacks", T::Int, 2, "Reefer racks (refrigerated container rows with power gantries).", 0.0, 8.0),
        p("warehouses", T::Int, 2, "Warehouses at the back of the yard.", 0.0, 8.0),
        p("customsGate", T::Bool, true, "Customs gate with booths and barrier lanes on the access road."),
        p("railSpur", T::Bool, true, "Rail spur into the yard."),
        p("dredgeDepth", T::Float, 1400.0, "Water depth kept along the berths, cm.", 200.0, 3000.0),
        p("ships", T::Int, 1, "Moored container ships (placeholders).", 0.0, 12.0),
        // Cruise terminal
        p("pierLength", T::Float, 32000.0, "Pier length out from the shore, cm.", 4000.0, 100000.0),
        p("pierWidth", T::Float, 3000.0, "Pier deck width, cm.", 800.0, 8000.0),
        p("shoreLength", T::Float, 16000.0, "Quay along the shore either side of the pier, cm.", 4000.0, 80000.0),
        p("terminalFloors", T::Int, 2, "Terminal building floors.", 1.0, 5.0),
        p("cruiseShips", T::Int, 1, "Ships moored along the pier (placeholders), one a side.", 0.0, 2.0),
        // Lighthouse
        p("towerHeight", T::Float, 2400.0, "Tower height to the lantern gallery, cm.", 600.0, 8000.0),
        p("keeperHouse", T::Bool, true, "Keeper's house beside the tower."),
        // Cemetery
        p("cemeteryWidth", T::Float, 16000.0, "Width along the front wall, cm.", 3000.0, 100000.0),
        p("cemeteryDepth", T::Float, 12000.0, "Depth from the front wall, cm.", 3000.0, 100000.0),
        p("mausoleums", T::Float, 0.35, "Share of plots holding a family mausoleum instead of a grave, 0 to 1.", 0.0, 1.0),
        p("plotWidth", T::Float, 450.0, "Plot width, cm (mausoleums take one to two plots).", 200.0, 1500.0),
        p("chapel", T::Bool, true, "Chapel at the end of the main path."),
        // Airstrip
        p("runwayLength", T::Float, 90000.0, "Runway length, cm.", 20000.0, 400000.0),
        p("runwayWidth", T::Float, 1800.0, "Runway width, cm.", 600.0, 6000.0),
        p("aircraft", T::String, "light", "Aircraft class the corridors are cut for: light or twin.", kNaN, kNaN, {"light", "twin"}),
        p("hangars", T::Int, 1, "Hangars on the apron.", 0.0, 6.0),
        p("fuelDrums", T::Int, 12, "Fuel drums by the hangar.", 0.0, 80.0),
        p("windsock", T::Bool, true, "Windsock beside the runway."),
        p("accessTrack", T::Bool, true, "Dirt track from the apron off the site."),
        // Farm
        p("farmWidth", T::Float, 60000.0, "Farm length along its road, cm.", 12000.0, 400000.0),
        p("farmDepth", T::Float, 40000.0, "Farm depth across the road (fields both sides), cm.", 8000.0, 200000.0),
        p("fieldWidth", T::Float, 8000.0, "Field strip width along the road, cm.", 2000.0, 40000.0),
        p("rowSpacing", T::Float, 150.0, "Crop row spacing, cm.", 50.0, 600.0),
        p("greenhouses", T::Float, 0.25, "Share of fields under greenhouses, 0 to 1.", 0.0, 1.0),
        p("shadeHouses", T::Float, 0.15, "Share of fields under shade houses, 0 to 1.", 0.0, 1.0),
        p("crops", T::String, "tomato, chile, cucumber, corn", "Crops the open fields cycle through (comma list)."),
        p("packingPlant", T::Bool, true, "Packing plant in the farmstead."),
        p("coldStorage", T::Bool, true, "Cold storage beside the packing plant."),
        p("truckYard", T::Bool, true, "Trucking yard with parked trailers."),
        p("trailers", T::Int, 12, "Trailers parked in the trucking yard.", 0.0, 80.0),
        p("canal", T::Bool, true, "Irrigation canal along the back of the fields, a sluice gate per field."),
        // Mountain village
        p("houses", T::Int, 24, "Adobe houses along the dirt streets.", 2.0, 200.0),
        p("villageRadius", T::Float, 9000.0, "How far the streets run out from the plaza, cm.", 3000.0, 40000.0),
        p("streets", T::Int, 4, "Dirt streets out from the plaza.", 1.0, 8.0),
        p("church", T::Bool, true, "Church on the plaza."),
        p("plaza", T::Bool, true, "Plaza with a well, benches and trees."),
        // Compounds
        p("compoundWidth", T::Float, 9000.0, "Walled area along the front, cm.", 3000.0, 40000.0),
        p("compoundDepth", T::Float, 8000.0, "Walled area depth, cm.", 3000.0, 40000.0),
        p("wallHeight", T::Float, 350.0, "Perimeter wall height, cm.", 100.0, 1000.0),
        p("mansionFloors", T::Int, 2, "Mansion floors.", 1.0, 4.0),
        p("pool", T::Bool, true, "Swimming pool."),
        p("helipad", T::Bool, true, "Helipad."),
        p("guardHouse", T::Bool, true, "Guard house at the gate."),
        // Private island
        p("islandRadius", T::Float, 14000.0, "Distance from the compound's centre to the shore on the dock side, cm.", 4000.0, 80000.0),
        p("raiseIsland", T::Bool, false, "Raise the ground inside the island radius above the water (an island where there was sea)."),
        p("dockLength", T::Float, 6000.0, "Dock length out from the shore, cm.", 1000.0, 30000.0),
        p("boats", T::Int, 2, "Boats moored at the dock.", 0.0, 8.0),
    };
    return all;
}

const Param* findParam(std::string_view name)
{
    for (const Param& x : params())
        if (x.name == name)
            return &x;
    return nullptr;
}

const std::vector<Kind>& kinds()
{
    static const std::vector<Kind> all = {
        {"port", "Cargo Port", "building-warehouse", "Coast",
         "A container port: quay and dredged berths, ship-to-shore cranes, a container yard of stacked blocks in fictional liveries with "
         "gantry cranes and reefer racks, warehouses, a customs gate and a rail spur.",
         {"harbour", "harbor", "container terminal", "docks", "shipping", "gta", "cityengine"},
         {"quayLength", "yardDepth", "berths", "stsCranes", "blockRows", "tiers", "fill", "liveries", "rtgCranes", "reeferRacks", "warehouses",
          "customsGate", "railSpur", "dredgeDepth", "ships"},
         "CargoPort"},
        {"cruise", "Cruise Terminal", "ship", "Coast", "A cruise pier with a terminal building, its plaza, quays either side and a moored ship placeholder.",
         {"pier", "ferry", "harbour", "malecon", "passenger terminal"},
         {"pierLength", "pierWidth", "shoreLength", "terminalFloors", "cruiseShips"},
         "CruiseTerminal"},
        {"lighthouse", "Lighthouse", "building-lighthouse", "Coast", "A lighthouse tower with its lantern, a keeper's house and a low wall.",
         {"beacon", "headland", "faro"},
         {"towerHeight", "keeperHouse"},
         "Lighthouse"},
        {"cemetery", "Cemetery", "building-monument", "Inland",
         "A walled cemetery: a grid of plots on paths, family mausoleums (domes, columns, glass fronts, air-conditioned chapels) among "
         "plain graves, a chapel and the gate.",
         {"graveyard", "panteon", "mausoleum", "narco cemetery", "tombs"},
         {"cemeteryWidth", "cemeteryDepth", "mausoleums", "plotWidth", "chapel"},
         "Cemetery"},
        {"airstrip", "Hidden Airstrip", "plane", "Inland",
         "A dirt runway graded into the terrain with approach and departure corridors, an apron with hangars, a windsock, fuel drums and an "
         "access track.",
         {"runway", "landing strip", "narco airstrip", "aerodrome", "pista"},
         {"runwayLength", "runwayWidth", "aircraft", "hangars", "fuelDrums", "windsock", "accessTrack"},
         "Airstrip"},
        {"farm", "Industrial Farm", "plant-2", "Inland",
         "Field strips along a farm road with crop rows, greenhouses and shade houses, a packing plant, cold storage, a trucking yard and an "
         "irrigation canal.",
         {"fields", "agriculture", "greenhouse", "packing plant", "ranch", "rancho", "crops"},
         {"farmWidth", "farmDepth", "fieldWidth", "rowSpacing", "greenhouses", "shadeHouses", "crops", "packingPlant", "coldStorage", "truckYard",
          "trailers", "canal"},
         "Farm"},
        {"village", "Mountain Village", "home", "Inland", "Adobe houses on terraced pads along dirt streets around a plaza with a church.",
         {"pueblo", "hamlet", "sierra", "adobe", "rural town"},
         {"houses", "villageRadius", "streets", "church", "plaza"},
         "Village"},
        {"compound", "Walled Compound", "building-castle", "Compounds",
         "A walled mansion compound: perimeter walls with cameras, the main gate and guard house, the mansion, pool, helipad, garage and "
         "palm-lined drive.",
         {"mansion", "narco mansion", "estate", "villa", "hacienda"},
         {"compoundWidth", "compoundDepth", "wallHeight", "mansionFloors", "pool", "helipad", "guardHouse"},
         "Compound"},
        {"island", "Private Island", "beach", "Compounds",
         "A private island compound: the mansion, pool and helipad inside a low wall, a dock with boats and a boathouse on the shore.",
         {"island", "hideout", "isla", "private estate"},
         {"islandRadius", "raiseIsland", "compoundWidth", "compoundDepth", "wallHeight", "mansionFloors", "pool", "helipad", "dockLength", "boats"},
         "IslandCompound"},
    };
    return all;
}

const Kind* findKind(std::string_view id)
{
    for (const Kind& k : kinds())
        if (k.id == id)
            return &k;
    return nullptr;
}

std::string kindList()
{
    std::string out;
    for (const Kind& k : kinds())
        out += (out.empty() ? "" : ", ") + k.id;
    return out;
}

const std::vector<std::string>& styles()
{
    static const std::vector<std::string> s = {"colonial", "seafront", "resortTower", "modernCommercial", "mansion", "civic",
                                               "warehouse", "packingPlant", "portShed", "adobe", "ruralShack", "church",
                                               "mausoleum", "hangar", "lighthouse", "terminal"};
    return s;
}

std::string ownerOf(const std::string& siteId) { return "site:" + siteId; }

// Spec

double Spec::f(const char* key) const { return params.at(key).get<double>(); }
int Spec::i(const char* key) const { return params.at(key).get<int>(); }
bool Spec::b(const char* key) const { return params.at(key).get<bool>(); }
std::string Spec::s(const char* key) const { return params.at(key).get<std::string>(); }

json Spec::toJson() const
{
    return {{"kind", kind},       {"id", id},         {"name", name},           {"culture", culture},
            {"seed", seed},       {"waterLevel", waterLevel}, {"at", {at.x, at.y}}, {"heading", heading},
            {"params", params}};
}

json checkParam(const Kind& k, const std::string& name, const json& value)
{
    const Param* d = findParam(name);
    if (!d)
        throw std::runtime_error("no site setting named " + name);
    if (std::ranges::find(k.params, name) == k.params.end())
    {
        std::string takers;
        for (const Kind& o : kinds())
            if (std::ranges::find(o.params, name) != o.params.end())
                takers += (takers.empty() ? "" : ", ") + o.id;
        throw std::runtime_error(name + " is not a " + k.id + " setting (it belongs to " + takers + ")");
    }
    auto range = [&](double v) {
        if ((!std::isnan(d->min) && v < d->min) || (!std::isnan(d->max) && v > d->max))
            throw std::runtime_error(name + " is " + std::to_string(v) + ", outside " + std::to_string(d->min) + " to " + std::to_string(d->max));
    };
    switch (d->type)
    {
    case T::Int:
    {
        if (!value.is_number())
            throw std::runtime_error(name + " takes a whole number");
        const double v = value.get<double>();
        range(v);
        return int(std::lround(v));
    }
    case T::Float:
    {
        if (!value.is_number())
            throw std::runtime_error(name + " takes a number");
        const double v = value.get<double>();
        range(v);
        return v;
    }
    case T::Bool:
        if (value.is_boolean())
            return value;
        if (value.is_number())
            return value.get<double>() != 0.0;
        throw std::runtime_error(name + " takes true or false");
    case T::String:
    {
        if (!value.is_string())
            throw std::runtime_error(name + " takes text");
        const std::string v = value.get<std::string>();
        if (!d->choices.empty() && std::ranges::find(d->choices, v) == d->choices.end())
        {
            std::string all;
            for (const std::string& c : d->choices)
                all += (all.empty() ? "" : ", ") + c;
            throw std::runtime_error(name + " is one of " + all + ", not " + v);
        }
        return v;
    }
    }
    return value;
}

Spec specFromJson(const json& j)
{
    if (!j.is_object())
        throw std::runtime_error("a site spec is an object");
    static const std::vector<std::string> keys = {"kind", "id", "name", "culture", "seed", "waterLevel", "at", "heading", "params"};
    for (const auto& [key, v] : j.items())
        if (std::ranges::find(keys, key) == keys.end())
            throw std::runtime_error("unknown site spec key " + key);
    Spec s;
    s.kind = j.value("kind", std::string());
    const Kind* k = findKind(s.kind);
    if (!k)
        throw std::runtime_error("kind is one of " + kindList() + (s.kind.empty() ? std::string() : ", not " + s.kind));
    s.id = j.value("id", s.kind);
    if (s.id.empty() || s.id.find_first_of("/ \t\n") != std::string::npos)
        throw std::runtime_error("a site id is a word without slashes or spaces, not '" + s.id + "'");
    s.culture = j.value("culture", std::string("mexican"));
    lila::names::requireCulture(s.culture);
    s.seed = j.value("seed", int64_t(0));
    s.waterLevel = j.value("waterLevel", 0.0);
    if (const json at = j.value("at", json::array({0.0, 0.0})); at.is_array() && at.size() >= 2 && at[0].is_number() && at[1].is_number())
        s.at = {at[0].get<double>(), at.size() == 3 ? at[2].get<double>() : at[1].get<double>()};
    else
        throw std::runtime_error("at is [x, z] in cm");
    s.heading = j.value("heading", 0.0);
    s.name = j.value("name", std::string());
    if (s.name.empty())
        s.name = siteName(s.kind, s.culture, s.id);
    const json given = j.value("params", json::object());
    if (!given.is_object())
        throw std::runtime_error("params is an object of settings");
    for (const auto& [key, v] : given.items())
        s.params[key] = checkParam(*k, key, v);
    for (const std::string& name : k->params)
        if (!s.params.contains(name))
            s.params[name] = findParam(name)->def;
    return s;
}

std::string siteName(const std::string& kind, const std::string& culture, const std::string& id, uint64_t salt)
{
    using lila::names::Kind;
    lila::names::NameBook book(culture);
    const uint64_t key = mix(lila::names::textKey(id), salt);
    if (kind == "port")
        return book.place(Kind::Port, "port", key);
    if (kind == "cruise")
        return book.place(Kind::Business, "cruiseTerminal", key);
    if (kind == "lighthouse")
        return book.place(Kind::Business, "lighthouse", key);
    if (kind == "cemetery")
        return book.place(Kind::Cemetery, "cemetery", key);
    if (kind == "airstrip")
        return book.place(Kind::Business, "airstrip", key);
    if (kind == "farm")
        return book.place(Kind::Business, "farm", key);
    if (kind == "village")
        return book.place(Kind::Town, {}, key);
    if (kind == "island")
        return book.place(Kind::Island, {}, key);
    return book.place(Kind::Business, "mansion", key);
}

// Slot types

const std::vector<TypeDef>& slotTypes()
{
    static const std::vector<TypeDef> all = {
        {"container.reefer", "Reefer container", {244, 259, 1219}, "ground", "box", {"industrial", "port", "reefer"}, "A refrigerated 40 ft container; +Z along its length."},
        {"crane", "Crane", {1000, 3000, 1000}, "ground", "box", {"industrial"}, "Any crane."},
        {"crane.sts", "Ship-to-shore crane", {2800, 7500, 9000}, "ground", "box", {"industrial", "port"},
         "A quay gantry crane on its rails; +Z the boom out over the water."},
        {"crane.rtg", "Gantry crane (RTG)", {1200, 2100, 2600}, "ground", "box", {"industrial", "port"},
         "A rubber-tyred gantry straddling a container block; +Z across the rows."},
        {"reefer", "Reefer gear", {300, 300, 300}, "ground", "box", {"industrial", "port", "reefer"}, "Refrigerated container equipment."},
        {"reefer.rack", "Reefer rack", {1300, 800, 300}, "ground", "panel", {"industrial", "port", "reefer"},
         "A power and access gantry beside a row of reefers; +X along the row."},
        {"quay", "Quay part", {500, 150, 100}, "ground", "box", {"port", "waterfront"}, "Any quay part."},
        {"quay.edge", "Quay edge", {500, 150, 100}, "ground", "box", {"port", "waterfront"}, "A piece of the quay's cope beam; +X along the edge, +Z to the water."},
        {"quay.fender", "Fender", {150, 200, 60}, "ground", "panel", {"port", "waterfront"}, "A rubber fender on the quay face; +Z to the water."},
        {"light", "Light", {100, 1000, 100}, "ground", "pole", {"lighting"}, "Any light."},
        {"light.mast", "Floodlight mast", {100, 3000, 100}, "ground", "pole", {"lighting", "industrial"}, "A yard floodlight mast."},
        {"rail", "Rail part", {1000, 30, 250}, "ground", "row", {"rail"}, "Any rail part."},
        {"rail.track", "Rail track", {1000, 30, 250}, "ground", "row", {"rail"}, "A piece of track; +X along the rails."},
        {"rail.bumper", "Buffer stop", {300, 120, 250}, "ground", "box", {"rail"}, "The buffer stop at a spur's end; +Z faces the track."},
        {"gate", "Gate", {500, 250, 30}, "ground", "panel", {"access"}, "Any gate."},
        {"gate.boom", "Barrier arm", {450, 120, 30}, "ground", "pole", {"access", "traffic"}, "A barrier arm across a vehicle lane; +X along the arm."},
        {"gate.vehicle", "Vehicle gate", {600, 220, 20}, "ground", "panel", {"access"}, "A sliding or swing vehicle gate; +X across the opening."},
        {"gate.main", "Main gate", {500, 350, 60}, "ground", "panel", {"access"}, "The main gate in a perimeter wall; +X across the opening."},
        {"fence", "Fence", {300, 250, 10}, "ground", "panel", {"boundary"}, "Any fence piece."},
        {"fence.chainlink", "Chain-link fence", {300, 250, 10}, "ground", "panel", {"boundary", "industrial"}, "A chain-link fence piece; +X along it."},
        {"fence.wood", "Wooden fence", {300, 120, 10}, "ground", "panel", {"boundary", "rural"}, "A post-and-rail fence piece; +X along it."},
        {"wall", "Wall", {300, 300, 30}, "ground", "panel", {"boundary"}, "Any wall piece."},
        {"wall.perimeter", "Perimeter wall", {300, 300, 30}, "ground", "panel", {"boundary"}, "A perimeter wall piece; +X along it, scaled to the wall height."},
        {"wall.low", "Low wall", {300, 100, 30}, "ground", "panel", {"boundary"}, "A low garden or cemetery wall piece; +X along it."},
        {"camera", "Camera", {30, 30, 40}, "free", "box", {"security"}, "Any camera."},
        {"camera.security", "Security camera", {30, 30, 40}, "free", "box", {"security"}, "A camera on a wall or post; +Z its view."},
        {"ship", "Ship", {25000, 2500, 3200}, "free", "box", {"vessel"}, "Any ship."},
        {"ship.cargo", "Container ship", {3200, 2500, 25000}, "free", "box", {"vessel", "port"}, "A moored container ship; +Z along the hull to the bow."},
        {"ship.cruise", "Cruise ship", {3600, 4500, 29000}, "free", "box", {"vessel", "cruise"}, "A moored cruise ship; +Z along the hull to the bow."},
        {"gangway", "Gangway", {300, 300, 1500}, "free", "box", {"cruise", "waterfront"}, "A boarding gangway; +Z from the pier to the ship."},
        {"dock", "Dock", {500, 100, 300}, "free", "panel", {"waterfront"}, "Any dock part."},
        {"dock.pier", "Pier deck", {500, 100, 300}, "free", "panel", {"waterfront"}, "A piece of pier deck on piles; +X along the pier, scaled to its width."},
        {"dock.floating", "Floating dock", {1000, 60, 300}, "free", "panel", {"waterfront"}, "A floating pontoon; +X along it."},
        {"boat", "Boat", {700, 150, 250}, "free", "box", {"vessel"}, "Any boat."},
        {"boat.speedboat", "Speedboat", {280, 200, 900}, "free", "box", {"vessel"}, "A moored speedboat; +Z to the bow."},
        {"boat.skiff", "Skiff", {200, 120, 700}, "free", "box", {"vessel"}, "A small open boat; +Z to the bow."},
        {"grave", "Grave", {100, 40, 220}, "ground", "box", {"cemetery"}, "Any grave."},
        {"grave.plain", "Plain grave", {100, 40, 220}, "ground", "box", {"cemetery"}, "A plain grave slab; +Z to the head."},
        {"grave.headstone", "Headstone grave", {100, 110, 220}, "ground", "box", {"cemetery"}, "A grave with a headstone; +Z to the head."},
        {"grave.cross", "Cross grave", {100, 150, 220}, "ground", "box", {"cemetery"}, "A grave with a cross; +Z to the head."},
        {"path", "Path", {300, 5, 250}, "ground", "row", {"paving"}, "Any path piece."},
        {"path.cemetery", "Cemetery path", {300, 5, 250}, "ground", "row", {"paving", "cemetery"}, "A paved cemetery path piece; +X along it, scaled to its width."},
        {"greenhouse", "Greenhouse part", {960, 450, 2500}, "ground", "box", {"farm"}, "Any greenhouse part."},
        {"greenhouse.frame", "Greenhouse span", {960, 450, 2500}, "ground", "box", {"farm"}, "One bay of a multi-span greenhouse; +Z along the span."},
        {"shadehouse", "Shade house part", {960, 400, 2500}, "ground", "box", {"farm"}, "Any shade house part."},
        {"shadehouse.frame", "Shade house bay", {960, 400, 2500}, "ground", "box", {"farm"}, "One bay of a shade house (posts and mesh); +Z along it."},
        {"irrigation", "Irrigation part", {150, 120, 100}, "ground", "box", {"farm", "water"}, "Any irrigation part."},
        {"irrigation.gate", "Sluice gate", {150, 120, 100}, "ground", "box", {"farm", "water"}, "A sluice gate where a field takes water from the canal."},
        {"irrigation.pump", "Pump house", {300, 250, 300}, "ground", "box", {"farm", "water"}, "The pump feeding the canal."},
        {"truck", "Truck part", {255, 400, 1360}, "ground", "box", {"vehicle"}, "Any truck part."},
        {"truck.trailer", "Trailer", {255, 400, 1360}, "ground", "box", {"vehicle", "farm"}, "A parked semi-trailer; +Z to its front."},
        {"truck.tractor", "Tractor unit", {255, 380, 650}, "ground", "box", {"vehicle"}, "A truck's tractor unit; +Z forward."},
        {"runway", "Runway marking", {300, 5, 50}, "ground", "panel", {"airstrip"}, "Any runway marking."},
        {"runway.threshold", "Threshold bar", {1200, 5, 300}, "ground", "panel", {"airstrip"}, "A painted or stone threshold bar across a runway end."},
        {"runway.edge", "Edge marker", {40, 40, 40}, "ground", "cone", {"airstrip"}, "A runway edge marker (painted tyre, cone or light)."},
        {"windsock", "Windsock", {100, 600, 100}, "ground", "pole", {"airstrip"}, "A windsock on its mast."},
        {"drum", "Drum", {60, 90, 60}, "ground", "cylinder", {"industrial"}, "Any drum."},
        {"drum.fuel", "Fuel drum", {60, 90, 60}, "ground", "cylinder", {"industrial", "airstrip"}, "A 200 l fuel drum."},
        {"hangar.door", "Hangar door", {1600, 550, 20}, "wall", "panel", {"airstrip", "opening"}, "A hangar's doors; +Z out of the wall."},
        {"pool", "Pool", {1000, 150, 500}, "ground", "box", {"leisure"}, "Any pool."},
        {"pool.swimming", "Swimming pool", {1000, 150, 500}, "ground", "box", {"leisure"}, "A swimming pool, its rim at the ground."},
        {"helipad", "Helipad", {2000, 10, 2000}, "ground", "cylinder", {"aviation"}, "A helipad pad and marking."},
        {"lighthouse", "Lighthouse part", {300, 400, 300}, "ground", "lamp", {"lighting", "waterfront"}, "Any lighthouse part."},
        {"lighthouse.lantern", "Lantern", {300, 400, 300}, "free", "lamp", {"lighting", "waterfront"}, "The lantern room on top of the tower."},
    };
    return all;
}

// Geometry

Ring rect(double x0, double z0, double x1, double z1)
{
    const double a = std::min(x0, x1), b = std::max(x0, x1), c = std::min(z0, z1), d = std::max(z0, z1);
    // Counter-clockwise in (X, Z), as Terrain and Settlement take rings.
    return {{a, c}, {b, c}, {b, d}, {a, d}};
}

Ring orientedRect(P2 c, double hw, double hd, double heading)
{
    const P2 ax(std::cos(heading), -std::sin(heading)), az(std::sin(heading), std::cos(heading));
    return {c - ax * hw - az * hd, c + ax * hw - az * hd, c + ax * hw + az * hd, c - ax * hw + az * hd};
}

Ring circle(P2 c, double r, int n)
{
    Ring out;
    for (int k = 0; k < n; ++k)
    {
        const double a = 2.0 * std::numbers::pi * k / n;
        out.push_back(c + P2(std::cos(a), std::sin(a)) * r);
    }
    return out;
}

double ringArea(const Ring& r)
{
    double a = 0.0;
    for (size_t i = 0; i < r.size(); ++i)
    {
        const P2 p = r[i], q = r[(i + 1) % r.size()];
        a += p.x * q.y - q.x * p.y;
    }
    return std::abs(a) * 0.5;
}

bool insideRing(const Ring& r, P2 p)
{
    bool in = false;
    for (size_t i = 0, j = r.size() - 1; i < r.size(); j = i++)
        if ((r[i].y > p.y) != (r[j].y > p.y) && p.x < (r[j].x - r[i].x) * (p.y - r[i].y) / (r[j].y - r[i].y) + r[i].x)
            in = !in;
    return in;
}

namespace
{
bool segmentsCross(P2 a, P2 b, P2 c, P2 d)
{
    auto cross = [](P2 u, P2 v) { return u.x * v.y - u.y * v.x; };
    const double d1 = cross(b - a, c - a), d2 = cross(b - a, d - a), d3 = cross(d - c, a - c), d4 = cross(d - c, b - c);
    return ((d1 > 0) != (d2 > 0)) && ((d3 > 0) != (d4 > 0)) && d1 != 0 && d2 != 0 && d3 != 0 && d4 != 0;
}
} // namespace

bool ringsOverlap(const Ring& a, const Ring& b)
{
    if (a.size() < 3 || b.size() < 3)
        return false;
    for (size_t i = 0; i < a.size(); ++i)
        for (size_t j = 0; j < b.size(); ++j)
            if (segmentsCross(a[i], a[(i + 1) % a.size()], b[j], b[(j + 1) % b.size()]))
                return true;
    return insideRing(a, centroidOf(b)) || insideRing(b, centroidOf(a));
}

P2 centroidOf(const Ring& r)
{
    P2 c(0.0);
    for (P2 p : r)
        c += p;
    return r.empty() ? c : c / double(r.size());
}

// Kit

Kit::Kit(const Spec& s, Layout& out) : spec(s), L(out), mBook(s.culture) {}

double Kit::rnd(uint64_t a, uint64_t b, uint64_t c) const
{
    const uint64_t h = mix(mix(mix(lila::names::textKey(spec.id), uint64_t(spec.seed)), a), mix(b, c));
    return double(h >> 11) * (1.0 / 9007199254740992.0);
}

int Kit::pick(int lo, int hi, uint64_t a, uint64_t b, uint64_t c) const
{
    return lo + std::min(hi - lo, int(rnd(a, b, c) * double(hi - lo + 1)));
}

namespace
{
std::mutex gAvoidLock;
std::shared_ptr<const lila::names::NameHashes> gAvoid;
uint64_t gAvoidRevision = 0;
} // namespace

void setAvoidNames(lila::names::NameHashes names)
{
    std::lock_guard l(gAvoidLock);
    if (gAvoid && gAvoid->exact == names.exact && gAvoid->contained == names.contained)
        return;
    gAvoid = std::make_shared<const lila::names::NameHashes>(std::move(names));
    ++gAvoidRevision;
}

std::shared_ptr<const lila::names::NameHashes> avoidNames()
{
    std::lock_guard l(gAvoidLock);
    return gAvoid;
}

uint64_t avoidRevision()
{
    std::lock_guard l(gAvoidLock);
    return gAvoidRevision;
}

namespace
{
/** A name from make(salt) that matches no real name imported into the scene (salted retries, as site names do). */
template <class F>
std::string clearOfImported(F&& make)
{
    const std::shared_ptr<const lila::names::NameHashes> avoid = avoidNames();
    std::string n = make(0);
    for (uint64_t salt = 1; avoid && avoid->matches(n) && salt < 32; ++salt)
        n = make(salt);
    return n;
}
} // namespace

std::string Kit::name(const std::string& role, lila::names::Kind kind, std::string_view program)
{
    const uint64_t key = lila::names::textKey(spec.id + "/" + role);
    const std::string n = clearOfImported([&](uint64_t salt) { return mBook.place(kind, program, key ^ (salt * 0x9E3779B97F4A7C15ull)); });
    L.names[role] = n;
    return n;
}

std::string Kit::street(const std::string& role, int cls)
{
    const uint64_t key = lila::names::textKey(spec.id + "/" + role);
    const std::string n = clearOfImported([&](uint64_t salt) { return mBook.street(cls, key ^ (salt * 0x9E3779B97F4A7C15ull)); });
    L.names[role] = n;
    return n;
}

SlotOut& Kit::slot(std::string path, std::string type, P2 at, double heading, double y, Level level)
{
    SlotOut s;
    s.path = std::move(path);
    s.type = std::move(type);
    s.at = at;
    s.heading = heading;
    s.y = y;
    s.level = level;
    L.slots.push_back(std::move(s));
    return L.slots.back();
}

namespace
{
/** The footprint length a segment type runs along x (or z for crop rows and containers), from the site kits' table or W0's. */
double nominalOf(const std::string& type, bool* alongZ)
{
    *alongZ = type == "crop.row";
    if (type == "crop.row")
        return 1000.0;
    for (const TypeDef& t : slotTypes())
        if (t.id == type)
            return t.footprint.x;
    return 300.0;
}
} // namespace

int Kit::along(const std::string& prefix, const std::string& type, P2 a, P2 b, double nominal, double y, Level level, double heightScale)
{
    const double len = glm::length(b - a);
    if (len < 1.0)
        return 0;
    bool alongZ = false;
    const double base = nominalOf(type, &alongZ);
    const int n = std::max(1, int(std::lround(len / std::max(1.0, nominal))));
    const P2 d = (b - a) / len;
    const double piece = len / n;
    for (int k = 0; k < n; ++k)
    {
        const P2 c = a + d * (piece * (k + 0.5));
        SlotOut& s = slot(prefix + "/" + std::to_string(k), type, c, alongZ ? facing(d) : alongDir(d), y, level);
        if (alongZ)
            s.scale = {1.0, heightScale, piece / base};
        else
            s.scale = {piece / base, heightScale, 1.0};
    }
    return n;
}

int Kit::alongRing(const std::string& prefix, const std::string& type, const Ring& pts, bool closed, double nominal,
                   const std::vector<std::pair<double, double>>& gaps, double heightScale)
{
    int count = 0;
    double walked = 0.0;
    const size_t edges = closed ? pts.size() : pts.size() - 1;
    for (size_t e = 0; e < edges; ++e)
    {
        const P2 a = pts[e], b = pts[(e + 1) % pts.size()];
        const double len = glm::length(b - a);
        // Each edge is split at the gaps it holds; the pieces between them get their own ids.
        std::vector<std::pair<double, double>> keep = {{0.0, len}};
        for (const auto& [g0, g1] : gaps)
        {
            std::vector<std::pair<double, double>> next;
            for (const auto& [k0, k1] : keep)
            {
                const double c0 = std::clamp(g0 - walked, k0, k1), c1 = std::clamp(g1 - walked, k0, k1);
                if (c1 <= c0)
                {
                    next.push_back({k0, k1});
                    continue;
                }
                if (c0 > k0)
                    next.push_back({k0, c0});
                if (k1 > c1)
                    next.push_back({c1, k1});
            }
            keep = std::move(next);
        }
        const P2 d = len > 0.0 ? (b - a) / len : P2(1.0, 0.0);
        for (size_t k = 0; k < keep.size(); ++k)
            count += along(prefix + "/e" + std::to_string(e) + (keep.size() > 1 ? "p" + std::to_string(k) : std::string()), type,
                           a + d * keep[k].first, a + d * keep[k].second, nominal, 0.0, Level::Ground, heightScale);
        walked += len;
    }
    return count;
}

EnvelopeOut& Kit::envelope(std::string path, Ring footprint, std::string style, std::string program, std::string use, int floors, double height,
                           std::string roof)
{
    EnvelopeOut e;
    e.path = std::move(path);
    e.footprint = std::move(footprint);
    e.style = std::move(style);
    e.program = std::move(program);
    e.use = std::move(use);
    e.floors = floors;
    e.height = height;
    e.roof = std::move(roof);
    if (e.roof == "gable" || e.roof == "hip" || e.roof == "mansard")
        e.roofHeight = 250.0;
    else if (e.roof == "dome")
        e.roofHeight = 300.0;
    L.envelopes.push_back(std::move(e));
    return L.envelopes.back();
}

GroundOp& Kit::ground(std::string id, std::string command, json flags)
{
    L.ground.push_back({std::move(id), std::move(command), std::move(flags)});
    return L.ground.back();
}

Part& Kit::part(std::string id, std::string kind, Ring ring, Part::Needs needs)
{
    Part pt;
    pt.id = std::move(id);
    pt.kind = std::move(kind);
    pt.ring = std::move(ring);
    pt.needs = needs;
    L.parts.push_back(std::move(pt));
    return L.parts.back();
}

P2 dirToWorld(const Spec& s, P2 d)
{
    const double h = s.heading * std::numbers::pi / 180.0, c = std::cos(h), sn = std::sin(h);
    // A turn about +Y: local +X goes to (cos, -sin), local +Z to (sin, cos).
    return {d.x * c + d.y * sn, -d.x * sn + d.y * c};
}

P2 toWorld(const Spec& s, P2 local) { return s.at + dirToWorld(s, local); }

double facing(P2 f) { return std::atan2(f.x, f.y); }
double alongDir(P2 d) { return std::atan2(-d.y, d.x); }

json pts(const Ring& r)
{
    json out = json::array();
    for (P2 q : r)
        out.push_back(json::array({q.x, q.y}));
    return out;
}

json pt(P2 q) { return json::array({q.x, q.y}); }

Layout build(const Spec& s)
{
    Layout L;
    Kit k(s, L);
    L.names["site"] = s.name;
    if (s.kind == "port")
        buildPort(k);
    else if (s.kind == "cruise")
        buildCruise(k);
    else if (s.kind == "lighthouse")
        buildLighthouse(k);
    else if (s.kind == "cemetery")
        buildCemetery(k);
    else if (s.kind == "airstrip")
        buildAirstrip(k);
    else if (s.kind == "farm")
        buildFarm(k);
    else if (s.kind == "village")
        buildVillage(k);
    else if (s.kind == "compound")
        buildCompound(k);
    else if (s.kind == "island")
        buildIsland(k);
    else
        throw std::runtime_error("kind is one of " + kindList() + ", not " + s.kind);

    // Every envelope carries its site, kind and a name where its program names one.
    json byType = json::object(), byStyle = json::object();
    for (const SlotOut& sl : L.slots)
        byType[sl.type] = byType.value(sl.type, 0) + 1;
    for (EnvelopeOut& e : L.envelopes)
    {
        byStyle[e.style] = byStyle.value(e.style, 0) + 1;
        e.extra["site"] = s.id;
        e.extra["siteKind"] = s.kind;
    }
    L.stats["slots"] = L.slots.size();
    L.stats["slotTypes"] = byType;
    L.stats["envelopes"] = L.envelopes.size();
    L.stats["styles"] = byStyle;
    L.stats["groundOps"] = L.ground.size();
    L.stats["parts"] = L.parts.size();
    L.stats["area"] = ringArea(L.boundary) * 1e-4;
    return L;
}
} // namespace openworld::sites
