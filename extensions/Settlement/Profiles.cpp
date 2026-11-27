// Settlement: the built-in styles and their JSON form.

#include "SettlementCore.h"
#include "Parcels.h"
#include "SettlementTerrain.h"
#include "Generators.h"

#include <algorithm>
#include <cctype>
#include <functional>
#include <stdexcept>

namespace settlement
{
namespace
{
struct DoubleField
{
    const char* name;
    double Profile::*member;
};
struct IntField
{
    const char* name;
    int Profile::*member;
};

constexpr DoubleField kDoubles[] = {
    {"widthArterial", &Profile::widthArterial}, {"widthCollector", &Profile::widthCollector},
    {"widthLocal", &Profile::widthLocal},       {"widthAlley", &Profile::widthAlley},
    {"widthPath", &Profile::widthPath},
    {"segment", &Profile::segment},             {"turn", &Profile::turn},
    {"minAngle", &Profile::minAngle},           {"spacing", &Profile::spacing},
    {"maxGrade", &Profile::maxGrade},           {"terraceGrade", &Profile::terraceGrade},
    {"density", &Profile::density},
    {"falloff", &Profile::falloff},             {"maxBlock", &Profile::maxBlock},
    {"gridAngle", &Profile::gridAngle},         {"gridWidth", &Profile::gridWidth},
    {"gridDepth", &Profile::gridDepth},         {"coreRadius", &Profile::coreRadius},
    {"lotWidth", &Profile::lotWidth},           {"lotDepth", &Profile::lotDepth},
    {"minLot", &Profile::minLot},               {"frontSetback", &Profile::frontSetback},
    {"sideSetback", &Profile::sideSetback},     {"buildDepth", &Profile::buildDepth},
    {"floorHeight", &Profile::floorHeight},     {"foundation", &Profile::foundation},
    {"commercial", &Profile::commercial},
    {"windAngle", &Profile::windAngle},
    {"townRadius", &Profile::townRadius},       {"townCentreX", &Profile::townCentreX},
    {"townCentreZ", &Profile::townCentreZ},
    {"targetEntropyMin", &Profile::targetEntropyMin}, {"targetEntropyMax", &Profile::targetEntropyMax},
    {"targetDeadEndMin", &Profile::targetDeadEndMin}, {"targetDeadEndMax", &Profile::targetDeadEndMax},
    {"targetFourWayMin", &Profile::targetFourWayMin}, {"targetFourWayMax", &Profile::targetFourWayMax},
    {"targetBlockMin", &Profile::targetBlockMin},     {"targetBlockMax", &Profile::targetBlockMax},
};

constexpr IntField kInts[] = {
    {"gates", &Profile::gates}, {"arterialEvery", &Profile::arterialEvery}, {"rings", &Profile::rings},
    {"spokes", &Profile::spokes}, {"minFloors", &Profile::minFloors},       {"maxFloors", &Profile::maxFloors},
    {"shopHouseFloors", &Profile::shopHouseFloors},
};

/** Hamlets and villages: a few lanes, wide plots, detached houses. */
Profile hamlet()
{
    Profile p;
    p.name = "hamlet";
    p.pattern = Pattern::Organic;
    p.widthArterial = 600.0, p.widthCollector = 450.0, p.widthLocal = 350.0, p.widthAlley = 250.0;
    p.gates = 3, p.segment = 4000.0, p.turn = 22.0, p.spacing = 4000.0;
    p.density = 0.35, p.falloff = 2.2, p.maxBlock = 40000.0, p.plaza = false;
    p.lotWidth = 2200.0, p.lotDepth = 4500.0, p.minLot = 150.0;
    p.frontSetback = 400.0, p.sideSetback = 400.0, p.buildDepth = 1200.0;
    p.minFloors = 1, p.maxFloors = 2, p.floorHeight = 300.0, p.roof = "gable", p.commercial = 0.1;
    return p;
}

/** A medieval market town: streets converging on the market, narrow deep burgage plots, row houses. */
Profile medievalTown() { return Profile{}; }

/** A street village strung along one road. */
Profile streetVillage()
{
    Profile p = hamlet();
    p.name = "streetVillage";
    p.pattern = Pattern::Linear;
    p.gates = 2, p.density = 0.25;
    p.lotWidth = 1500.0, p.lotDepth = 6000.0, p.frontSetback = 200.0, p.sideSetback = 250.0;
    return p;
}

/** A small US town: a survey grid, alleys behind, detached houses set back from the street. */
Profile usTown()
{
    Profile p;
    p.name = "usTown";
    p.pattern = Pattern::Grid;
    p.widthArterial = 2400.0, p.widthCollector = 1800.0, p.widthLocal = 1500.0, p.widthAlley = 500.0;
    p.gridWidth = 9000.0, p.gridDepth = 20000.0, p.arterialEvery = 6, p.maxBlock = 25000.0;
    p.plaza = false, p.maxGrade = 0.08;
    p.lotWidth = 1500.0, p.lotDepth = 4000.0, p.minLot = 200.0;
    p.frontSetback = 600.0, p.sideSetback = 150.0, p.buildDepth = 1600.0;
    p.minFloors = 1, p.maxFloors = 3, p.floorHeight = 300.0, p.roof = "gable", p.commercial = 0.2;
    return p;
}

/**
 * A Mexican city: a grid of perimeter blocks, one- and two-storey houses wall to wall along every street with patios
 * behind, flat roofs, narrow streets, the centre denser and taller.
 */
Profile mexicanCity()
{
    Profile p;
    p.name = "mexicanCity";
    p.pattern = Pattern::Grid;
    p.era = "modern";
    p.blockType = "perimeter";
    p.lotMethod = "skeleton";
    p.widthArterial = 2000.0, p.widthCollector = 1200.0, p.widthLocal = 900.0, p.widthAlley = 450.0, p.widthPath = 250.0;
    p.gridWidth = 8000.0, p.gridDepth = 8000.0, p.arterialEvery = 5, p.maxBlock = 20000.0;
    p.plaza = true, p.maxGrade = 0.12;
    p.lotWidth = 1500.0, p.lotDepth = 2000.0, p.minLot = 60.0;
    p.frontSetback = 0.0, p.sideSetback = 0.0, p.buildDepth = 1400.0;
    p.minFloors = 1, p.maxFloors = 3, p.floorHeight = 330.0, p.roof = "flat", p.commercial = 0.3, p.falloff = 2.5;
    p.shopHouseFloors = 4;
    return p;
}

/** Manhattan: long thin blocks, avenues every few streets, towers rising to the centre. */
Profile manhattan()
{
    Profile p;
    p.name = "manhattan";
    p.pattern = Pattern::Grid;
    p.widthArterial = 3000.0, p.widthCollector = 2400.0, p.widthLocal = 1800.0, p.widthAlley = 600.0;
    p.gridWidth = 6000.0, p.gridDepth = 25000.0, p.arterialEvery = 3, p.maxBlock = 30000.0;
    p.plaza = false, p.maxGrade = 0.08, p.falloff = 1.2;
    p.lotWidth = 800.0, p.lotDepth = 3000.0, p.minLot = 80.0;
    p.frontSetback = 0.0, p.sideSetback = 0.0, p.buildDepth = 3000.0;
    p.minFloors = 5, p.maxFloors = 60, p.floorHeight = 380.0, p.roof = "flat", p.commercial = 0.7;
    return p;
}

/** A European capital: a medieval core inside a ring boulevard where the walls were, perimeter blocks beyond. */
Profile europeanCity()
{
    Profile p;
    p.name = "europeanCity";
    p.pattern = Pattern::OrganicGrid;
    p.widthArterial = 2400.0, p.widthCollector = 1500.0, p.widthLocal = 1000.0, p.widthAlley = 400.0;
    p.gates = 6, p.coreRadius = 0.35;
    p.gridWidth = 11300.0, p.gridDepth = 11300.0, p.arterialEvery = 5, p.gridAngle = 12.0;
    p.lotWidth = 1200.0, p.lotDepth = 2500.0, p.minLot = 100.0;
    p.frontSetback = 0.0, p.sideSetback = 0.0, p.buildDepth = 1600.0;
    p.minFloors = 3, p.maxFloors = 7, p.floorHeight = 340.0, p.roof = "mansard", p.commercial = 0.45;
    return p;
}

/** A modern city: big blocks, setback towers, wide roads. */
Profile modernCity()
{
    Profile p;
    p.name = "modernCity";
    p.pattern = Pattern::Grid;
    p.widthArterial = 4000.0, p.widthCollector = 2800.0, p.widthLocal = 2000.0, p.widthAlley = 800.0;
    p.gridWidth = 15000.0, p.gridDepth = 15000.0, p.arterialEvery = 3, p.maxBlock = 40000.0;
    p.plaza = false, p.maxGrade = 0.07, p.falloff = 2.4;
    p.lotWidth = 4000.0, p.lotDepth = 5000.0, p.minLot = 400.0;
    p.frontSetback = 800.0, p.sideSetback = 800.0, p.buildDepth = 4000.0;
    p.minFloors = 4, p.maxFloors = 80, p.floorHeight = 400.0, p.roof = "flat", p.commercial = 0.6;
    return p;
}

/** A sci-fi megacity: rings and spokes around a hub, enormous blocks, towers kilometres high. */
Profile scifiMegacity()
{
    Profile p;
    p.name = "scifiMegacity";
    p.pattern = Pattern::Radial;
    p.widthArterial = 8000.0, p.widthCollector = 5000.0, p.widthLocal = 3000.0, p.widthAlley = 1200.0;
    p.rings = 6, p.spokes = 12, p.maxBlock = 120000.0, p.plaza = true;
    p.maxGrade = 0.1, p.falloff = 1.4;
    p.lotWidth = 8000.0, p.lotDepth = 10000.0, p.minLot = 1500.0;
    p.frontSetback = 1500.0, p.sideSetback = 1500.0, p.buildDepth = 9000.0;
    p.minFloors = 20, p.maxFloors = 400, p.floorHeight = 450.0, p.roof = "flat", p.commercial = 0.5;
    return p;
}

struct Builtin
{
    const char* name;
    Profile (*make)();
};

constexpr Builtin kBuiltins[] = {
    {"hamlet", hamlet},           {"streetVillage", streetVillage}, {"medievalTown", medievalTown},
    {"usTown", usTown},           {"mexicanCity", mexicanCity},     {"manhattan", manhattan},
    {"europeanCity", europeanCity}, {"modernCity", modernCity},     {"scifiMegacity", scifiMegacity},
};
} // namespace

const char* patternName(Pattern p)
{
    switch (p)
    {
    case Pattern::Organic: return "organic";
    case Pattern::Grid: return "grid";
    case Pattern::Radial: return "radial";
    case Pattern::Linear: return "linear";
    case Pattern::OrganicGrid: return "organicGrid";
    case Pattern::None: return "none";
    case Pattern::Tensor: return "tensor";
    case Pattern::Contour: return "contour";
    case Pattern::Osm: return "osm";
    case Pattern::Example: return "example";
    }
    return "organic";
}

bool parsePattern(std::string_view name, Pattern* out)
{
    for (Pattern p : {Pattern::Organic, Pattern::Grid, Pattern::Radial, Pattern::Linear, Pattern::OrganicGrid, Pattern::None,
                      Pattern::Tensor, Pattern::Contour, Pattern::Osm, Pattern::Example})
        if (name == patternName(p))
        {
            *out = p;
            return true;
        }
    return false;
}

std::vector<std::string> profileNames()
{
    std::vector<std::string> out;
    for (const Builtin& b : kBuiltins)
        out.emplace_back(b.name);
    return out;
}

bool builtinProfile(std::string_view name, Profile* out)
{
    for (const Builtin& b : kBuiltins)
        if (name == b.name)
        {
            *out = b.make();
            applyBuiltinTargets(*out);
            return true;
        }
    return false;
}

json toJson(const Profile& p)
{
    json j = {{"name", p.name}, {"pattern", patternName(p.pattern)}, {"plaza", p.plaza}, {"roof", p.roof}};
    j["era"] = eraOf(p);
    j["blockType"] = p.blockType;
    j["lotMethod"] = p.lotMethod;
    for (const DoubleField& f : kDoubles)
        j[f.name] = p.*f.member;
    for (const IntField& f : kInts)
        j[f.name] = p.*f.member;
    j["siting"] = toJson(p.siting);
    return j;
}

void applyJson(Profile& p, const json& j)
{
    if (!j.is_object())
        throw std::runtime_error("profile overrides must be a JSON object");
    for (auto& [k, v] : j.items())
    {
        bool done = false;
        for (const DoubleField& f : kDoubles)
            if (k == f.name)
            {
                if (!v.is_number())
                    throw std::runtime_error("profile field " + k + " takes a number");
                p.*f.member = v.get<double>();
                done = true;
            }
        for (const IntField& f : kInts)
            if (k == f.name)
            {
                if (!v.is_number())
                    throw std::runtime_error("profile field " + k + " takes a number");
                p.*f.member = int(v.get<double>());
                done = true;
            }
        if (done)
            continue;
        if (k == "name" && v.is_string())
            p.name = v.get<std::string>();
        else if (k == "roof" && v.is_string())
            p.roof = v.get<std::string>();
        else if (k == "plaza" && v.is_boolean())
            p.plaza = v.get<bool>();
        else if (k == "era" && v.is_string())
            p.era = v.get<std::string>();
        else if (k == "blockType" && v.is_string())
            p.blockType = v.get<std::string>();
        else if (k == "lotMethod" && v.is_string())
            p.lotMethod = v.get<std::string>();
        else if (k == "siting")
            applyJson(p.siting, v);
        else if (k == "pattern" && v.is_string())
        {
            if (!parsePattern(v.get<std::string>(), &p.pattern))
                throw std::runtime_error("unknown pattern '" + v.get<std::string>() + "' (organic, grid, radial, linear, organicGrid, none, tensor, contour, osm, example)");
        }
        else
            throw std::runtime_error("unknown or mistyped profile field '" + k + "'");
    }
    validateParcelProfile(p);
}

double streetWidth(const Profile& p, StreetClass c)
{
    switch (c)
    {
    case StreetClass::Arterial: return p.widthArterial;
    case StreetClass::Collector: return p.widthCollector;
    case StreetClass::Local: return p.widthLocal;
    case StreetClass::Alley: return p.widthAlley;
    case StreetClass::Path: return p.widthPath;
    case StreetClass::Boundary:
    case StreetClass::Water: return 0.0;
    }
    return p.widthLocal;
}

namespace
{
RoadProfile road(const char* name, int lanes, double laneWidth, double median, double sidewalk, double verge, double speed, bool oneWay = false)
{
    RoadProfile r;
    r.name = name;
    r.lanes = lanes;
    r.laneWidth = laneWidth;
    r.median = median;
    r.sidewalk = sidewalk;
    r.verge = verge;
    r.speed = speed;
    r.oneWay = oneWay;
    return r;
}

RoadProfile parked(RoadProfile r, double parking)
{
    r.parking = parking;
    return r;
}

const std::vector<RoadProfile>& roadBuiltins()
{
    static const std::vector<RoadProfile> all = {
        road("highway", 4, 365.0, 300.0, 0.0, 300.0, 100.0),
        road("avenue", 4, 325.0, 200.0, 300.0, 0.0, 50.0),
        road("street", 2, 300.0, 0.0, 200.0, 0.0, 40.0),
        parked(road("parkedStreet", 2, 300.0, 0.0, 200.0, 0.0, 40.0), 220.0),
        road("lane", 2, 250.0, 0.0, 100.0, 0.0, 30.0),
        road("alley", 1, 300.0, 0.0, 0.0, 0.0, 15.0),
        road("serviceRoad", 2, 300.0, 0.0, 0.0, 50.0, 30.0),
        road("track", 1, 300.0, 0.0, 0.0, 0.0, 20.0),
        road("footpath", 0, 0.0, 0.0, 100.0, 0.0, 5.0),
        road("stairs", 0, 0.0, 0.0, 125.0, 0.0, 3.0),
    };
    return all;
}

struct RoadField
{
    const char* name;
    double RoadProfile::*member;
};

constexpr RoadField kRoadFields[] = {{"laneWidth", &RoadProfile::laneWidth}, {"median", &RoadProfile::median},
                                     {"sidewalk", &RoadProfile::sidewalk},   {"verge", &RoadProfile::verge},
                                     {"parking", &RoadProfile::parking},     {"speed", &RoadProfile::speed}};
} // namespace

std::vector<std::string> roadProfileNames()
{
    std::vector<std::string> out;
    for (const RoadProfile& r : roadBuiltins())
        out.push_back(r.name);
    return out;
}

bool builtinRoadProfile(std::string_view name, RoadProfile* out)
{
    for (const RoadProfile& r : roadBuiltins())
        if (r.name == name)
        {
            *out = r;
            return true;
        }
    return false;
}

const char* defaultRoadProfile(StreetClass c)
{
    switch (c)
    {
    case StreetClass::Arterial: return "avenue";
    case StreetClass::Collector: return "street";
    case StreetClass::Local: return "lane";
    case StreetClass::Alley: return "alley";
    case StreetClass::Path: return "footpath";
    default: return "lane";
    }
}

json toJson(const RoadProfile& p)
{
    json j = {{"name", p.name}, {"lanes", p.lanes}, {"oneWay", p.oneWay}, {"width", p.width()}};
    for (const RoadField& f : kRoadFields)
        j[f.name] = p.*f.member;
    return j;
}

void applyJson(RoadProfile& p, const json& j)
{
    if (!j.is_object())
        throw std::runtime_error("a road profile is a JSON object");
    for (auto& [k, v] : j.items())
    {
        bool done = false;
        for (const RoadField& f : kRoadFields)
            if (k == f.name)
            {
                if (!v.is_number() || v.get<double>() < 0.0)
                    throw std::runtime_error("road profile field " + k + " takes a number, 0 or more");
                p.*f.member = v.get<double>();
                done = true;
            }
        if (done || k == "width")
            continue;
        if (k == "lanes" && v.is_number() && v.get<double>() >= 0.0)
            p.lanes = int(v.get<double>());
        else if (k == "oneWay" && v.is_boolean())
            p.oneWay = v.get<bool>();
        else if (k == "name" && v.is_string())
            p.name = v.get<std::string>();
        else
            throw std::runtime_error("unknown or mistyped road profile field '" + k + "'");
    }
}

std::vector<CrossSlot> crossSection(const RoadProfile& p)
{
    std::vector<CrossSlot> side;
    for (const auto& [symbol, size] : {std::pair{"Verge", p.verge}, std::pair{"Sidewalk", p.sidewalk}, std::pair{"Parking", p.parking}})
        if (size > 0.0)
            side.push_back({symbol, size});
    std::vector<CrossSlot> out = side;
    const int back = p.oneWay ? 0 : p.lanes / 2;
    for (int k = 0; k < back; ++k)
        out.push_back({"Lane", p.laneWidth});
    if (p.median > 0.0)
        out.push_back({"Median", p.median});
    for (int k = back; k < p.lanes; ++k)
        out.push_back({"Lane", p.laneWidth});
    out.insert(out.end(), side.rbegin(), side.rend());
    return out;
}

std::string grammarOf(std::span<const CrossSlot> slots)
{
    std::string g = "[";
    for (size_t k = 0; k < slots.size(); ++k)
        g += (k ? "," : "") + slots[k].symbol;
    return g + "]";
}

std::vector<std::string> grammarSymbols(std::string_view grammar)
{
    // A small recursive reader: sequence := item*, item := symbol | '[' sequence (',' sequence)* ']' count?
    size_t at = 0;
    std::function<std::vector<std::string>()> sequence = [&]() {
        std::vector<std::string> out;
        while (at < grammar.size())
        {
            const char c = grammar[at];
            if (c == ']')
                break;
            if (c == ',' || std::isspace(static_cast<unsigned char>(c)))
            {
                ++at;
                continue;
            }
            std::vector<std::string> item;
            if (c == '[')
            {
                ++at;
                item = sequence();
                if (at < grammar.size() && grammar[at] == ']')
                    ++at;
            }
            else
            {
                std::string symbol;
                while (at < grammar.size() && std::string_view(",[]* \t\r\n").find(grammar[at]) == std::string_view::npos)
                    symbol += grammar[at++];
                item = {symbol};
            }
            int repeat = 1;
            if (at < grammar.size() && grammar[at] == '*')
                ++at;
            else if (at < grammar.size() && std::isdigit(static_cast<unsigned char>(grammar[at])) && c == '[')
            {
                repeat = 0;
                while (at < grammar.size() && std::isdigit(static_cast<unsigned char>(grammar[at])))
                    repeat = repeat * 10 + (grammar[at++] - '0');
                repeat = std::clamp(repeat, 1, 64);
            }
            for (int r = 0; r < repeat; ++r)
                out.insert(out.end(), item.begin(), item.end());
        }
        return out;
    };
    return sequence();
}

RoadProfile profileFromSlots(std::string name, std::span<const CrossSlot> slots, std::vector<std::string>* unknown)
{
    auto has = [](const std::string& s, std::initializer_list<const char*> words) {
        std::string lower;
        for (char c : s)
            lower += char(std::tolower(static_cast<unsigned char>(c)));
        for (const char* w : words)
            if (lower.find(w) != std::string::npos)
                return true;
        return false;
    };
    RoadProfile p;
    p.name = std::move(name);
    p.lanes = 0;
    double lanes = 0.0, sidewalk = 0.0, verge = 0.0, parking = 0.0, median = 0.0;
    for (const CrossSlot& s : slots)
    {
        if (has(s.symbol, {"median", "divider", "island", "reserve"}))
            median += s.size;
        else if (has(s.symbol, {"park"}))
            parking += s.size;
        else if (has(s.symbol, {"sidewalk", "pavement", "footway", "walk", "curb", "kerb", "gutter", "ped"}))
            sidewalk += s.size;
        else if (has(s.symbol, {"lane", "drive", "carriage", "road", "bus", "tram", "bike", "cycle", "traffic"}))
        {
            lanes += s.size;
            ++p.lanes;
        }
        else
        {
            if (unknown && !has(s.symbol, {"verge", "grass", "planter", "tree", "shoulder", "lawn"}))
                unknown->push_back(s.symbol);
            verge += s.size;
        }
    }
    p.laneWidth = p.lanes > 0 ? lanes / p.lanes : 0.0;
    p.median = median;
    p.sidewalk = 0.5 * sidewalk;
    p.verge = 0.5 * verge;
    p.parking = 0.5 * parking;
    return p;
}
} // namespace settlement
