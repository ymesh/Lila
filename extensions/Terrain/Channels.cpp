// Coast features: craft classes, beach, surf, shallows and reef masks, mangroves in the tidal band, braided channels
// carved to a craft's minimum width and depth, stash-spot candidates, and dams with their spillways.

#include "Coast.h"
#include "Water.h"

#include <lila/Profile.h>

#include <tbb/parallel_for.h>

#include <algorithm>
#include <cmath>
#include <format>
#include <map>
#include <numbers>

namespace terrain
{
using json = nlohmann::json;

namespace coast_detail
{
constexpr double kCoastDeg = 180.0 / std::numbers::pi;

double smooth01(double a, double b, double x)
{
    if (b <= a)
        return x < a ? 0.0 : 1.0;
    const double t = std::clamp((x - a) / (b - a), 0.0, 1.0);
    return t * t * (3.0 - 2.0 * t);
}

std::vector<glm::dvec2> resample(const std::vector<glm::dvec2>& line, double step)
{
    if (line.size() < 2 || step <= 0.0)
        return line;
    std::vector<glm::dvec2> out = {line.front()};
    double carry = 0.0;
    for (size_t k = 1; k < line.size(); ++k)
    {
        const glm::dvec2 a = line[k - 1], b = line[k];
        const double len = glm::length(b - a);
        double t = step - carry;
        while (t < len)
        {
            out.push_back(a + (b - a) * (t / len));
            t += step;
        }
        carry = len - (t - step);
    }
    if (glm::length(out.back() - line.back()) > step * 0.25)
        out.push_back(line.back());
    else
        out.back() = line.back();
    return out;
}

std::vector<glm::dvec2> chaikin(std::vector<glm::dvec2> line, int iterations)
{
    for (int it = 0; it < iterations && line.size() >= 3; ++it)
    {
        std::vector<glm::dvec2> next = {line.front()};
        for (size_t k = 0; k + 1 < line.size(); ++k)
        {
            next.push_back(line[k] * 0.75 + line[k + 1] * 0.25);
            next.push_back(line[k] * 0.25 + line[k + 1] * 0.75);
        }
        next.push_back(line.back());
        line = std::move(next);
    }
    return line;
}

double lineLength(const std::vector<glm::dvec2>& l)
{
    double s = 0.0;
    for (size_t k = 1; k < l.size(); ++k)
        s += glm::length(l[k] - l[k - 1]);
    return s;
}

double segmentDistance(glm::dvec2 p, glm::dvec2 a, glm::dvec2 b, double* along = nullptr)
{
    const glm::dvec2 ab = b - a;
    const double l2 = glm::dot(ab, ab);
    const double t = l2 > 0.0 ? std::clamp(glm::dot(p - a, ab) / l2, 0.0, 1.0) : 0.0;
    if (along)
        *along = t;
    return glm::length(p - (a + ab * t));
}

double slopeDeg(const Heightfield& h, int x, int z)
{
    const int x0 = std::max(0, x - 1), x1 = std::min(h.nx - 1, x + 1), z0 = std::max(0, z - 1), z1 = std::min(h.nz - 1, z + 1);
    const double gx = (h.at(x1, z) - h.at(x0, z)) / (std::max(1, x1 - x0) * h.cell);
    const double gz = (h.at(x, z1) - h.at(x, z0)) / (std::max(1, z1 - z0) * h.cell);
    return std::atan(std::sqrt(gx * gx + gz * gz)) * kCoastDeg;
}

double fbm(double x, double z, uint32_t seed)
{
    double v = 0.0, amp = 0.5, f = 1.0;
    for (int o = 0; o < 4; ++o)
    {
        v += amp * gradientNoise(x * f, z * f, seed + uint32_t(o) * 1013u);
        amp *= 0.5;
        f *= 2.03;
    }
    return v;
}

/** Share of set samples inside a square of radius r around each sample (summed-area table). */
std::vector<float> boxShare(const std::vector<uint8_t>& set, int nx, int nz, int r)
{
    std::vector<double> sat(size_t(nx + 1) * size_t(nz + 1), 0.0);
    for (int z = 0; z < nz; ++z)
    {
        double row = 0.0;
        for (int x = 0; x < nx; ++x)
        {
            row += set[size_t(z) * size_t(nx) + size_t(x)];
            sat[size_t(z + 1) * size_t(nx + 1) + size_t(x + 1)] = sat[size_t(z) * size_t(nx + 1) + size_t(x + 1)] + row;
        }
    }
    std::vector<float> out(size_t(nx) * size_t(nz));
    for (int z = 0; z < nz; ++z)
        for (int x = 0; x < nx; ++x)
        {
            const int x0 = std::max(0, x - r), x1 = std::min(nx, x + r + 1), z0 = std::max(0, z - r), z1 = std::min(nz, z + r + 1);
            const double s = sat[size_t(z1) * size_t(nx + 1) + size_t(x1)] - sat[size_t(z0) * size_t(nx + 1) + size_t(x1)] -
                             sat[size_t(z1) * size_t(nx + 1) + size_t(x0)] + sat[size_t(z0) * size_t(nx + 1) + size_t(x0)];
            out[size_t(z) * size_t(nx) + size_t(x)] = float(s / double((x1 - x0) * (z1 - z0)));
        }
    return out;
}

uint32_t hash32(uint32_t x)
{
    x ^= x >> 16;
    x *= 0x7feb352du;
    x ^= x >> 15;
    x *= 0x846ca68bu;
    x ^= x >> 16;
    return x;
}

double rand01(uint32_t seed, uint32_t k) { return double(hash32(seed * 0x9e3779b9u + k * 0x85ebca6bu + 0x165667b1u) & 0xffffff) / double(0xffffff); }

/** Unit direction of a polyline at point k. */
glm::dvec2 tangentAt(const std::vector<glm::dvec2>& l, size_t k)
{
    const glm::dvec2 a = l[k > 0 ? k - 1 : 0], b = l[std::min(l.size() - 1, k + 1)];
    const glm::dvec2 d = b - a;
    const double len = glm::length(d);
    return len > 1e-12 ? d / len : glm::dvec2(1.0, 0.0);
}
} // namespace coast_detail

using namespace coast_detail;

// ---------------------------------------------------------------------------------------------------- craft

const std::vector<CraftClass>& craftClasses()
{
    static const std::vector<CraftClass> c = [] {
        std::vector<CraftClass> v;
        auto boat = [&](const char* name, const char* label, double draft, double minDepth, double minWidth, double beam, double speed) {
            CraftClass k;
            k.name = name;
            k.label = label;
            k.kind = "boat";
            k.draft = draft;
            k.minDepth = minDepth;
            k.minWidth = minWidth;
            k.beam = beam;
            k.speed = speed;
            v.push_back(k);
        };
        auto air = [&](const char* name, const char* label, double approach, double climb, double clearance, double length, double halfWidth, double runway, double speed) {
            CraftClass k;
            k.name = name;
            k.label = label;
            k.kind = "air";
            k.approachAngle = approach;
            k.climbAngle = climb;
            k.clearance = clearance;
            k.corridorLength = length;
            k.corridorHalfWidth = halfWidth;
            k.runway = runway;
            k.speed = speed;
            v.push_back(k);
        };
        boat("skiff", "Skiff", 40.0, 70.0, 400.0, 200.0, 12.0);
        boat("airboat", "Airboat", 10.0, 25.0, 600.0, 250.0, 15.0);
        boat("goFast", "Go-fast", 90.0, 150.0, 800.0, 280.0, 25.0);
        boat("semiSubmersible", "Semi-submersible", 280.0, 450.0, 1200.0, 450.0, 3.5);
        boat("cutter", "Coast guard cutter", 420.0, 650.0, 3000.0, 1100.0, 13.0);
        air("light", "Light aircraft", 4.0, 5.0, 3000.0, 300000.0, 15000.0, 45000.0, 50.0);
        air("twin", "Twin turboprop", 3.0, 4.0, 6000.0, 500000.0, 25000.0, 90000.0, 75.0);
        return v;
    }();
    return c;
}

const CraftClass* craftClass(std::string_view name)
{
    for (const CraftClass& c : craftClasses())
        if (c.name == name)
            return &c;
    return nullptr;
}

json craftJson(const CraftClass& c)
{
    json j = {{"name", c.name}, {"label", c.label}, {"kind", c.kind}, {"speed", c.speed}};
    if (c.kind == "boat")
    {
        j["draft"] = c.draft;
        j["minDepth"] = c.minDepth;
        j["minWidth"] = c.minWidth;
        j["beam"] = c.beam;
    }
    else
    {
        j["approachAngle"] = c.approachAngle;
        j["climbAngle"] = c.climbAngle;
        j["clearance"] = c.clearance;
        j["corridorLength"] = c.corridorLength;
        j["corridorHalfWidth"] = c.corridorHalfWidth;
        j["runway"] = c.runway;
    }
    return j;
}

// ---------------------------------------------------------------------------------------------------- specs

namespace
{
json coastPointsJson(const std::vector<glm::dvec2>& l)
{
    json a = json::array();
    for (const glm::dvec2& p : l)
        a.push_back({p.x, p.y});
    return a;
}

std::vector<glm::dvec2> coastPointsFrom(const json& j)
{
    std::vector<glm::dvec2> out;
    if (j.is_array())
        for (const json& p : j)
            if (p.is_array() && p.size() >= 2)
                out.push_back({p[0].get<double>(), p[1].get<double>()});
    return out;
}

double coastNum(const json& j, const char* k, double def)
{
    if (!j.contains(k) || j[k].is_null())
        return def;
    return j[k].get<double>();
}
} // namespace

json ChannelSpec::toJson() const
{
    json j = {{"id", id},         {"path", coastPointsJson(path)}, {"craft", craft},       {"braids", braids},   {"braidSpread", braidSpread},
              {"width", width},   {"depth", depth},           {"margin", margin},     {"bankAngle", bankAngle}, {"reErode", reErode},
              {"meander", meander}, {"seed", seed}};
    j["waterLevel"] = std::isnan(waterLevel) ? json(nullptr) : json(waterLevel);
    return j;
}

ChannelSpec ChannelSpec::fromJson(const json& j)
{
    ChannelSpec s;
    s.id = j.value("id", std::string());
    s.path = coastPointsFrom(j.value("path", json::array()));
    s.craft = j.value("craft", s.craft);
    s.braids = std::clamp(j.value("braids", s.braids), 1, 8);
    s.braidSpread = std::max(0.0, coastNum(j, "braidSpread", s.braidSpread));
    s.width = std::max(0.0, coastNum(j, "width", s.width));
    s.depth = std::max(0.0, coastNum(j, "depth", s.depth));
    s.margin = std::clamp(coastNum(j, "margin", s.margin), 1.0, 5.0);
    s.waterLevel = coastNum(j, "waterLevel", s.waterLevel);
    s.bankAngle = std::clamp(coastNum(j, "bankAngle", s.bankAngle), 5.0, 80.0);
    s.reErode = std::clamp(j.value("reErode", s.reErode), 0, 50);
    s.meander = std::clamp(coastNum(j, "meander", s.meander), 0.0, 5.0);
    s.seed = j.value("seed", s.seed);
    return s;
}

json LaneSpec::toJson() const
{
    json j = {{"id", id}, {"kind", kind}, {"craft", craft}, {"path", coastPointsJson(path)}};
    if (at)
        j["at"] = {at->x, at->y};
    return j;
}

LaneSpec LaneSpec::fromJson(const json& j)
{
    LaneSpec s;
    s.id = j.value("id", std::string());
    s.kind = j.value("kind", s.kind);
    s.craft = j.value("craft", s.craft);
    s.path = coastPointsFrom(j.value("path", json::array()));
    if (j.contains("at") && j["at"].is_array() && j["at"].size() >= 2)
        s.at = glm::dvec2(j["at"][0].get<double>(), j["at"][1].get<double>());
    return s;
}

json AirstripSpec::toJson() const
{
    json j = {{"id", id}, {"a", {a.x, a.y}}, {"b", {b.x, b.y}}, {"aircraft", aircraft}, {"bothEnds", bothEnds}};
    j["elevation"] = std::isnan(elevation) ? json(nullptr) : json(elevation);
    return j;
}

AirstripSpec AirstripSpec::fromJson(const json& j)
{
    AirstripSpec s;
    s.id = j.value("id", std::string());
    const std::vector<glm::dvec2> ab = coastPointsFrom(json::array({j.value("a", json::array({0.0, 0.0})), j.value("b", json::array({0.0, 0.0}))}));
    if (ab.size() == 2)
    {
        s.a = ab[0];
        s.b = ab[1];
    }
    s.aircraft = j.value("aircraft", s.aircraft);
    s.elevation = coastNum(j, "elevation", s.elevation);
    s.bothEnds = j.value("bothEnds", s.bothEnds);
    return s;
}

json DamSpec::toJson() const
{
    json j = {{"id", id},     {"a", {a.x, a.y}},        {"b", {b.x, b.y}},    {"thickness", thickness}, {"faceSlope", faceSlope},
              {"spillwayWidth", spillwayWidth}, {"freeboard", freeboard}, {"spillwayAt", spillwayAt}};
    j["crest"] = std::isnan(crest) ? json(nullptr) : json(crest);
    return j;
}

DamSpec DamSpec::fromJson(const json& j)
{
    DamSpec s;
    s.id = j.value("id", std::string());
    const std::vector<glm::dvec2> ab = coastPointsFrom(json::array({j.value("a", json::array({0.0, 0.0})), j.value("b", json::array({0.0, 0.0}))}));
    if (ab.size() == 2)
    {
        s.a = ab[0];
        s.b = ab[1];
    }
    s.crest = coastNum(j, "crest", s.crest);
    s.thickness = std::max(100.0, coastNum(j, "thickness", s.thickness));
    s.faceSlope = std::clamp(coastNum(j, "faceSlope", s.faceSlope), 0.1, 6.0);
    s.spillwayWidth = std::max(0.0, coastNum(j, "spillwayWidth", s.spillwayWidth));
    s.freeboard = std::max(0.0, coastNum(j, "freeboard", s.freeboard));
    s.spillwayAt = std::clamp(coastNum(j, "spillwayAt", s.spillwayAt), 0.0, 1.0);
    return s;
}

json CoastSpec::settingsJson() const
{
    const CoastMasks& m = masks;
    const MangroveSettings& g = mangroves;
    const RouteSettings& r = routes;
    return {{"seaLevel", seaLevel},
            {"masks",
             {{"enabled", m.enabled},       {"beachSlope", m.beachSlope},       {"beachHeight", m.beachHeight},       {"beachBelow", m.beachBelow},
              {"surfDepth", m.surfDepth},   {"surfWidth", m.surfWidth},         {"shallowDepth", m.shallowDepth},     {"reefMinDepth", m.reefMinDepth},
              {"reefMaxDepth", m.reefMaxDepth}, {"reefMinDistance", m.reefMinDistance}, {"reefMaxDistance", m.reefMaxDistance},
              {"reefCover", m.reefCover},   {"reefScale", m.reefScale},         {"seed", m.seed}}},
            {"mangroves",
             {{"enabled", g.enabled},       {"tidalLow", g.tidalLow},   {"tidalHigh", g.tidalHigh},     {"maxSlope", g.maxSlope},
              {"landcover", g.landcover},   {"code", g.code},           {"fromWetland", g.fromWetland}, {"fromBand", g.fromBand},
              {"shelterRadius", g.shelterRadius}, {"shelterMax", g.shelterMax}}},
            {"stash",
             {{"enabled", stash.enabled}, {"bankHeight", stash.bankHeight}, {"bankReach", stash.bankReach}, {"canopy", stash.canopy},
              {"spacing", stash.spacing}, {"maxCount", stash.maxCount}}},
            {"routes",
             {{"coastLanes", r.coastLanes},   {"coastDepth", r.coastDepth}, {"coastCraft", r.coastCraft}, {"minLaneLength", r.minLaneLength},
              {"riverLanes", r.riverLanes},   {"channelLanes", r.channelLanes}, {"snap", r.snap}, {"linkReach", r.linkReach},
              {"spacing", r.spacing}}}};
}

void CoastSpec::readSettings(const json& j)
{
    if (!j.is_object())
        return;
    seaLevel = coastNum(j, "seaLevel", seaLevel);
    if (const json m = j.value("masks", json::object()); m.is_object())
    {
        CoastMasks& s = masks;
        s.enabled = m.value("enabled", s.enabled);
        s.beachSlope = std::clamp(coastNum(m, "beachSlope", s.beachSlope), 0.0, 90.0);
        s.beachHeight = std::max(0.0, coastNum(m, "beachHeight", s.beachHeight));
        s.beachBelow = std::max(0.0, coastNum(m, "beachBelow", s.beachBelow));
        s.surfDepth = std::max(1.0, coastNum(m, "surfDepth", s.surfDepth));
        s.surfWidth = std::max(1.0, coastNum(m, "surfWidth", s.surfWidth));
        s.shallowDepth = std::max(1.0, coastNum(m, "shallowDepth", s.shallowDepth));
        s.reefMinDepth = std::max(0.0, coastNum(m, "reefMinDepth", s.reefMinDepth));
        s.reefMaxDepth = std::max(s.reefMinDepth + 1.0, coastNum(m, "reefMaxDepth", s.reefMaxDepth));
        s.reefMinDistance = std::max(0.0, coastNum(m, "reefMinDistance", s.reefMinDistance));
        s.reefMaxDistance = std::max(s.reefMinDistance + 1.0, coastNum(m, "reefMaxDistance", s.reefMaxDistance));
        s.reefCover = std::clamp(coastNum(m, "reefCover", s.reefCover), 0.0, 1.0);
        s.reefScale = std::max(100.0, coastNum(m, "reefScale", s.reefScale));
        s.seed = m.value("seed", s.seed);
    }
    if (const json m = j.value("mangroves", json::object()); m.is_object())
    {
        MangroveSettings& s = mangroves;
        s.enabled = m.value("enabled", s.enabled);
        s.tidalLow = coastNum(m, "tidalLow", s.tidalLow);
        s.tidalHigh = std::max(s.tidalLow + 1.0, coastNum(m, "tidalHigh", s.tidalHigh));
        s.maxSlope = std::clamp(coastNum(m, "maxSlope", s.maxSlope), 0.1, 90.0);
        s.landcover = m.value("landcover", s.landcover);
        s.code = m.value("code", s.code);
        s.fromWetland = m.value("fromWetland", s.fromWetland);
        s.fromBand = m.value("fromBand", s.fromBand);
        s.shelterRadius = std::max(100.0, coastNum(m, "shelterRadius", s.shelterRadius));
        s.shelterMax = std::clamp(coastNum(m, "shelterMax", s.shelterMax), 0.0, 1.0);
    }
    if (const json m = j.value("stash", json::object()); m.is_object())
    {
        stash.enabled = m.value("enabled", stash.enabled);
        stash.bankHeight = std::max(0.0, coastNum(m, "bankHeight", stash.bankHeight));
        stash.bankReach = std::max(1.0, coastNum(m, "bankReach", stash.bankReach));
        stash.canopy = std::clamp(coastNum(m, "canopy", stash.canopy), 0.0, 1.0);
        stash.spacing = std::max(0.0, coastNum(m, "spacing", stash.spacing));
        stash.maxCount = std::clamp(m.value("maxCount", stash.maxCount), 0, 10000);
    }
    if (const json m = j.value("routes", json::object()); m.is_object())
    {
        RouteSettings& r = routes;
        r.coastLanes = m.value("coastLanes", r.coastLanes);
        r.coastDepth = std::max(1.0, coastNum(m, "coastDepth", r.coastDepth));
        r.coastCraft = m.value("coastCraft", r.coastCraft);
        r.minLaneLength = std::max(0.0, coastNum(m, "minLaneLength", r.minLaneLength));
        r.riverLanes = m.value("riverLanes", r.riverLanes);
        r.channelLanes = m.value("channelLanes", r.channelLanes);
        r.snap = std::max(0.0, coastNum(m, "snap", r.snap));
        r.linkReach = std::max(0.0, coastNum(m, "linkReach", r.linkReach));
        r.spacing = std::max(10.0, coastNum(m, "spacing", r.spacing));
    }
}

const std::vector<std::string>& coastMasks()
{
    static const std::vector<std::string> m = {"beach", "surf", "shallows", "reef", "tidal", "mangrove", "channel", "dam", "seabed"};
    return m;
}

// ---------------------------------------------------------------------------------------------------- masks

void applyCoastMasks(Heightfield& h, double seaLevel, const CoastMasks& s, CoastResult& r)
{
    LILA_ZONE();

    const size_t n = h.size();
    std::vector<uint8_t> sea = seaOf(h, seaLevel);
    if (const std::vector<float>* ch = h.findMask("channel"))
        for (size_t i = 0; i < n; ++i)
            if ((*ch)[i] > 0.5f && h.heights[i] < seaLevel)
                sea[i] = 1;
    std::vector<uint8_t> land(n);
    for (size_t i = 0; i < n; ++i)
        land[i] = !sea[i];
    const std::vector<float> toSea = distanceToSet(sea, h.nx, h.nz, h.cell);
    const std::vector<float> toLand = distanceToSet(land, h.nx, h.nz, h.cell);

    std::vector<float>& beach = h.mask("beach");
    std::vector<float>& surf = h.mask("surf");
    std::vector<float>& shallows = h.mask("shallows");
    std::vector<float>& reef = h.mask("reef");
    std::vector<uint8_t> dryBeach(n, 0);
    tbb::parallel_for(0, h.nz, [&](int z) {
        for (int x = 0; x < h.nx; ++x)
        {
            const size_t i = h.index(x, z);
            beach[i] = surf[i] = shallows[i] = reef[i] = 0.0f;
            if (sea[i])
                continue;
            const double above = h.heights[i] - seaLevel;
            if (toSea[i] > 8000.0 || above > s.beachHeight)
                continue;
            const double v = (1.0 - smooth01(s.beachHeight * 0.6, s.beachHeight, above)) * (1.0 - smooth01(s.beachSlope * 0.7, s.beachSlope, slopeDeg(h, x, z)));
            beach[i] = float(v);
            dryBeach[i] = v > 0.5;
        }
    });
    const std::vector<float> toBeach = distanceToSet(dryBeach, h.nx, h.nz, h.cell);
    const double surfReach = s.surfWidth;
    tbb::parallel_for(0, h.nz, [&](int z) {
        for (int x = 0; x < h.nx; ++x)
        {
            const size_t i = h.index(x, z);
            if (!sea[i])
                continue;
            const double depth = seaLevel - h.heights[i];
            if (depth <= s.beachBelow && toBeach[i] <= toLand[i] + 2.0 * h.cell)
                beach[i] = float(1.0 - smooth01(s.beachBelow * 0.5, s.beachBelow, depth));
            shallows[i] = float(1.0 - smooth01(s.shallowDepth * 0.6, s.shallowDepth, depth));
            if (depth <= s.surfDepth && toLand[i] <= surfReach)
            {
                const double exposed = toBeach[i] <= toLand[i] + 3.0 * h.cell ? 1.0 : 0.5;
                surf[i] = float(exposed * (1.0 - smooth01(s.surfDepth * 0.5, s.surfDepth, depth)) * (1.0 - smooth01(surfReach * 0.5, surfReach, toLand[i])));
            }
            if (depth >= s.reefMinDepth && depth <= s.reefMaxDepth && toLand[i] >= s.reefMinDistance && toLand[i] <= s.reefMaxDistance && s.reefCover > 0.0)
            {
                const glm::dvec2 p = h.samplePos(x, z);
                const double nz = fbm(p.x / s.reefScale, p.y / s.reefScale, s.seed ^ 0x5eefu);
                const double threshold = 0.35 - 0.7 * s.reefCover;
                const double band = smooth01(s.reefMinDepth, s.reefMinDepth * 1.5 + 1.0, depth) * (1.0 - smooth01(s.reefMaxDepth * 0.8, s.reefMaxDepth, depth));
                reef[i] = float(smooth01(threshold - 0.05, threshold + 0.05, nz) * band);
            }
        }
    });
    const double a = h.cell * h.cell * 1e-4;
    for (size_t i = 0; i < n; ++i)
    {
        r.beachArea += beach[i] * a;
        r.surfArea += surf[i] * a;
        r.shallowArea += shallows[i] * a;
        r.reefArea += reef[i] * a;
    }
}

// ---------------------------------------------------------------------------------------------------- mangroves

void applyMangroves(Heightfield& h, double seaLevel, const MangroveSettings& s, CoastResult& r)
{
    LILA_ZONE();

    const size_t n = h.size();
    std::vector<float>& tidal = h.mask("tidal");
    const double fade = std::max(10.0, (s.tidalHigh - s.tidalLow) * 0.15);
    for (size_t i = 0; i < n; ++i)
    {
        const double rel = h.heights[i] - seaLevel;
        tidal[i] = float(smooth01(s.tidalLow - fade, s.tidalLow, rel) * (1.0 - smooth01(s.tidalHigh, s.tidalHigh + fade, rel)));
    }
    std::vector<float>& mangrove = h.mask("mangrove");
    std::fill(mangrove.begin(), mangrove.end(), 0.0f);
    if (!s.enabled)
    {
        r.mangroveSource = "none";
        return;
    }

    const std::vector<float>* codes = s.landcover.empty() ? nullptr : h.findMask(s.landcover);
    auto codeAt = [&](size_t i) { return codes ? int(std::lround((*codes)[i] * 255.0f)) : -1; };
    size_t coded = 0, wet = 0;
    const std::vector<float>* wetMask = h.findMask("landcover_wetland");
    for (size_t i = 0; i < n; ++i)
    {
        coded += codeAt(i) == s.code;
        wet += codeAt(i) == 90 || (wetMask && (*wetMask)[i] > 0.5f);
    }
    std::vector<float> slope(n);
    tbb::parallel_for(0, h.nz, [&](int z) {
        for (int x = 0; x < h.nx; ++x)
            slope[h.index(x, z)] = float(slopeDeg(h, x, z));
    });
    auto flat = [&](size_t i) { return 1.0 - smooth01(s.maxSlope * 0.7, s.maxSlope, slope[i]); };

    if (coded > 0)
    {
        r.mangroveSource = "worldcover";
        // Land cover is the truth for where they are; the band only trims them off ground well above the tide.
        for (size_t i = 0; i < n; ++i)
            if (codeAt(i) == s.code)
            {
                const double rel = h.heights[i] - seaLevel;
                mangrove[i] = float(1.0 - smooth01(s.tidalHigh * 2.0, s.tidalHigh * 3.0 + fade, rel));
            }
        r.notes.push_back(std::format("mangroves from land cover class {} in {}", s.code, s.landcover));
    }
    else if (s.fromWetland && wet > 0)
    {
        r.mangroveSource = "wetland";
        for (size_t i = 0; i < n; ++i)
            if (codeAt(i) == 90 || (wetMask && (*wetMask)[i] > 0.5f))
                mangrove[i] = float(tidal[i] * flat(i));
        r.notes.push_back("no mangrove class in the land cover; mangroves are its wetland inside the tidal band");
    }
    else if (s.fromBand)
    {
        r.mangroveSource = "band";
        std::vector<uint8_t> water = seaOf(h, seaLevel);
        if (const std::vector<float>* ch = h.findMask("channel"))
            for (size_t i = 0; i < n; ++i)
                water[i] = water[i] || (*ch)[i] > 0.5f;
        const int rad = std::clamp(int(s.shelterRadius / h.cell), 1, 512);
        const std::vector<float> share = boxShare(water, h.nx, h.nz, rad);
        const std::vector<float> toWater = distanceToSet(water, h.nx, h.nz, h.cell);
        for (size_t i = 0; i < n; ++i)
        {
            const double sheltered = (1.0 - smooth01(s.shelterMax * 0.8, s.shelterMax, share[i])) * smooth01(0.01, 0.04, share[i]);
            const double near = 1.0 - smooth01(s.shelterRadius * 0.25, s.shelterRadius * 0.5, toWater[i]);
            mangrove[i] = float(tidal[i] * flat(i) * sheltered * near);
        }
        r.notes.push_back(codes ? "no mangrove or wetland class in the land cover; mangroves are the sheltered tidal band"
                                : "no land cover mask (" + s.landcover + "); mangroves are the sheltered tidal band");
    }
    else
    {
        r.mangroveSource = "none";
        r.warnings.push_back("mangroves are on but there is no mangrove or wetland land cover and the tidal band source is off");
    }
    const double a = h.cell * h.cell * 1e-4;
    for (size_t i = 0; i < n; ++i)
        r.mangroveArea += mangrove[i] * a;
}

// ---------------------------------------------------------------------------------------------------- channels

void measureStrand(const Heightfield& h, ChannelStrand& s, double waterLevel, double craftDepth, double searchWidth)
{
    const size_t n = s.points.size();
    s.width.assign(n, 0.0);
    s.depth.assign(n, 0.0);
    s.minWidth = n ? 1e30 : 0.0;
    s.minDepth = n ? 1e30 : 0.0;
    const double step = std::max(1.0, h.cell * 0.5);
    for (size_t k = 0; k < n; ++k)
    {
        const glm::dvec2 p = s.points[k];
        const glm::dvec2 t = tangentAt(s.points, k);
        const glm::dvec2 nrm(-t.y, t.x);
        const double depth = waterLevel - h.heightAt(p.x, p.y);
        s.depth[k] = depth;
        double width = 0.0;
        if (depth >= craftDepth)
        {
            for (int side : {-1, 1})
            {
                double d = 0.0;
                while (d < searchWidth * 0.5)
                {
                    const glm::dvec2 q = p + nrm * (double(side) * (d + step));
                    if (!h.contains(q.x, q.y) || waterLevel - h.heightAt(q.x, q.y) < craftDepth)
                        break;
                    d += step;
                }
                width += d;
            }
        }
        s.width[k] = width;
        s.minWidth = std::min(s.minWidth, width);
        s.minDepth = std::min(s.minDepth, depth);
    }
}

ChannelResult carveChannel(Heightfield& h, const ChannelSpec& s, double seaLevel, std::vector<std::string>& warnings)
{
    LILA_ZONE();

    ChannelResult r;
    r.id = s.id;
    const CraftClass* craft = craftClass(s.craft);
    if (!craft || craft->kind != "boat")
    {
        warnings.push_back("channel " + s.id + ": no boat class " + s.craft + "; sized for a skiff");
        craft = craftClass("skiff");
    }
    r.craft = craft->name;
    r.waterLevel = std::isnan(s.waterLevel) ? seaLevel : s.waterLevel;
    r.width = s.width > 0.0 ? s.width : craft->minWidth * s.margin;
    r.depth = s.depth > 0.0 ? s.depth : craft->minDepth * s.margin;
    if (s.path.size() < 2)
    {
        warnings.push_back("channel " + s.id + ": its path has fewer than two points");
        return r;
    }
    const double W = r.width, bed = r.waterLevel - r.depth;
    const double step = std::max(h.cell, W * 0.5);
    std::vector<glm::dvec2> main = resample(chaikin(s.path, 2), step);
    const double total = lineLength(main);

    // The main strand meanders a little; braids split off it and rejoin, bowing out to either side.
    auto distances = [](const std::vector<glm::dvec2>& l) {
        std::vector<double> d(l.size(), 0.0);
        for (size_t k = 1; k < l.size(); ++k)
            d[k] = d[k - 1] + glm::length(l[k] - l[k - 1]);
        return d;
    };
    const std::vector<double> along = distances(main);
    const double wave = std::max(W * 14.0, h.cell * 8.0);
    auto bend = [&](double d, double phase) {
        const double env = smooth01(0.0, wave * 0.5, d) * smooth01(0.0, wave * 0.5, total - d);
        return s.meander * W * std::sin(2.0 * std::numbers::pi * d / wave + phase) * env;
    };
    std::vector<glm::dvec2> mainBent(main.size());
    for (size_t k = 0; k < main.size(); ++k)
    {
        const glm::dvec2 t = tangentAt(main, k);
        mainBent[k] = main[k] + glm::dvec2(-t.y, t.x) * bend(along[k], rand01(s.seed, 7) * 6.283);
    }
    std::vector<std::vector<glm::dvec2>> strands = {mainBent};
    const double spread = s.braidSpread > 0.0 ? s.braidSpread : W * 3.0;
    for (int b = 1; b < s.braids; ++b)
    {
        const double f0 = 0.08 + 0.3 * rand01(s.seed, uint32_t(b) * 3), f1 = 0.62 + 0.3 * rand01(s.seed, uint32_t(b) * 3 + 1);
        const double side = (b % 2 ? 1.0 : -1.0) * (1.0 + 0.5 * ((b - 1) / 2));
        std::vector<glm::dvec2> strand;
        for (size_t k = 0; k < main.size(); ++k)
        {
            const double f = total > 0.0 ? along[k] / total : 0.0;
            if (f < f0 || f > f1)
                continue;
            const double u = (f - f0) / (f1 - f0);
            const glm::dvec2 t = tangentAt(main, k);
            const double off = side * spread * std::sin(std::numbers::pi * u) + bend(along[k], rand01(s.seed, uint32_t(b) * 3 + 2) * 6.283) * 0.5;
            strand.push_back(mainBent[k] + glm::dvec2(-t.y, t.x) * off);
        }
        if (strand.size() >= 3)
            strands.push_back(chaikin(strand, 1));
    }

    // Carve: a flat bed across the width, banks rising at bankAngle until they meet the ground.
    // Every point within half the width must sit between samples at the bed, so the flat bottom reaches past it by a cell.
    const double flat = W * 0.5 + h.cell * std::numbers::sqrt2;
    if (h.cell > W * 0.5)
        warnings.push_back(std::format("channel {}: the terrain's {:.0f} cm cells are coarse for a {:.0f} cm channel; it is cut {:.0f} cm wide", s.id, h.cell, W, flat * 2.0));
    const double tanB = std::tan(s.bankAngle / kCoastDeg);
    const double top = h.range().y;
    const double margin = flat + std::clamp((top - bed) / tanB, h.cell, W * 10.0);
    std::vector<float> target(h.size(), std::numeric_limits<float>::infinity());
    std::vector<uint8_t> core(h.size(), 0);
    for (const std::vector<glm::dvec2>& st : strands)
        for (size_t k = 0; k + 1 < st.size(); ++k)
        {
            const glm::dvec2 a = st[k], b = st[k + 1];
            const int x0 = std::max(0, int(std::floor((std::min(a.x, b.x) - margin - h.origin.x) / h.cell)));
            const int x1 = std::min(h.nx - 1, int(std::ceil((std::max(a.x, b.x) + margin - h.origin.x) / h.cell)));
            const int z0 = std::max(0, int(std::floor((std::min(a.y, b.y) - margin - h.origin.y) / h.cell)));
            const int z1 = std::min(h.nz - 1, int(std::ceil((std::max(a.y, b.y) + margin - h.origin.y) / h.cell)));
            for (int z = z0; z <= z1; ++z)
                for (int x = x0; x <= x1; ++x)
                {
                    const double d = segmentDistance(h.samplePos(x, z), a, b);
                    if (d > margin)
                        continue;
                    const size_t i = h.index(x, z);
                    const double t = d <= flat ? bed : bed + (d - flat) * tanB;
                    target[i] = std::min(target[i], float(t));
                    if (d <= flat)
                        core[i] = 1;
                }
        }
    std::vector<uint8_t> touched(h.size(), 0);
    for (size_t i = 0; i < h.size(); ++i)
        if (target[i] < h.heights[i])
        {
            r.cut += double(h.heights[i] - target[i]) * h.cell * h.cell;
            h.heights[i] = target[i];
            touched[i] = 1;
        }

    // Settle the banks: relax what was cut, and a ring round it, toward its neighbours; the bed stays put.
    for (int pass = 0; pass < s.reErode; ++pass)
    {
        std::vector<float> next = h.heights;
        tbb::parallel_for(1, h.nz - 1, [&](int z) {
            for (int x = 1; x < h.nx - 1; ++x)
            {
                const size_t i = h.index(x, z);
                if (core[i] || !std::isfinite(target[i]))
                    continue;
                double sum = 0.0;
                for (int dz = -1; dz <= 1; ++dz)
                    for (int dx = -1; dx <= 1; ++dx)
                        sum += h.heights[h.index(x + dx, z + dz)];
                const double avg = sum / 9.0;
                // Only lower: settling never fills the channel back in.
                next[i] = float(std::min(double(h.heights[i]), 0.5 * h.heights[i] + 0.5 * avg));
            }
        });
        h.heights.swap(next);
    }

    std::vector<float>& channel = h.mask("channel");
    std::vector<float>& water = h.mask("water");
    for (size_t i = 0; i < h.size(); ++i)
        if (core[i])
        {
            channel[i] = 1.0f;
            if (h.heights[i] < r.waterLevel)
                water[i] = 1.0f;
        }

    for (const std::vector<glm::dvec2>& st : strands)
    {
        ChannelStrand cs;
        cs.points = resample(st, step);
        measureStrand(h, cs, r.waterLevel, craft->minDepth, W * 4.0);
        r.strands.push_back(std::move(cs));
    }
    return r;
}

// ---------------------------------------------------------------------------------------------------- stash spots

void findStashSpots(const Heightfield& h, const StashSettings& s, CoastResult& r)
{
    LILA_ZONE();

    std::vector<StashSpot> cands;
    const std::vector<float>* sea = h.findMask("sea");
    const std::vector<float>* mangrove = h.findMask("mangrove");
    auto maskAt = [&](const std::vector<float>* m, glm::dvec2 p) {
        if (!m)
            return 0.0;
        const int x = std::clamp(int(std::lround((p.x - h.origin.x) / h.cell)), 0, h.nx - 1);
        const int z = std::clamp(int(std::lround((p.y - h.origin.y) / h.cell)), 0, h.nz - 1);
        return double((*m)[h.index(x, z)]);
    };
    for (const ChannelResult& c : r.channels)
    {
        const double half = c.width * 0.5;
        for (size_t si = 0; si < c.strands.size(); ++si)
        {
            const ChannelStrand& st = c.strands[si];
            if (st.points.size() < 2)
                continue;
            double dist = 0.0;
            for (size_t k = 0; k < st.points.size(); ++k)
            {
                if (k > 0)
                    dist += glm::length(st.points[k] - st.points[k - 1]);
                const glm::dvec2 p = st.points[k];
                const glm::dvec2 t = tangentAt(st.points, k);
                const glm::dvec2 nrm(-t.y, t.x);
                const std::string base = c.id + "_s" + std::to_string(si) + "_";
                const long metres = std::lround(dist / 100.0);
                for (int side : {-1, 1})
                {
                    const glm::dvec2 edge = p + nrm * (double(side) * half);
                    // Overhang: the bank rises steeply right beside the water.
                    double rise = 0.0;
                    for (double d = h.cell; d <= s.bankReach; d += h.cell)
                    {
                        const glm::dvec2 q = edge + nrm * (double(side) * d);
                        if (h.contains(q.x, q.y))
                            rise = std::max(rise, h.heightAt(q.x, q.y) - c.waterLevel);
                    }
                    if (rise >= s.bankHeight)
                        cands.push_back({base + "overhang_" + std::to_string(metres) + (side < 0 ? "L" : "R"), "overhang",
                                         glm::dvec3(edge.x, c.waterLevel, edge.y), c.id, std::min(1.0, rise / (s.bankHeight * 3.0)) * 0.8});
                    double canopy = 0.0;
                    for (double d = 0.0; d <= s.bankReach; d += h.cell)
                    {
                        const glm::dvec2 q = edge + nrm * (double(side) * d);
                        canopy = std::max(canopy, maskAt(mangrove, q));
                    }
                    if (mangrove && canopy >= s.canopy)
                        cands.push_back({base + "canopy_" + std::to_string(metres) + (side < 0 ? "L" : "R"), "canopy",
                                         glm::dvec3(edge.x, c.waterLevel, edge.y), c.id, 0.6 + 0.3 * canopy});
                }
            }
            // A strand's end away from open sea is a dead end; braids end on the main strand, so only the main one counts.
            if (si == 0)
                for (size_t end : {size_t(0), st.points.size() - 1})
                {
                    const glm::dvec2 p = st.points[end];
                    if (maskAt(sea, p) < 0.5)
                        cands.push_back({c.id + "_deadEnd_" + (end == 0 ? "start" : "end"), "deadEnd", glm::dvec3(p.x, c.waterLevel, p.y), c.id, 1.0});
                }
        }
    }
    std::stable_sort(cands.begin(), cands.end(), [](const StashSpot& a, const StashSpot& b) { return a.score > b.score; });
    for (const StashSpot& c : cands)
    {
        if (int(r.stash.size()) >= s.maxCount)
            break;
        const bool crowded = std::any_of(r.stash.begin(), r.stash.end(), [&](const StashSpot& o) {
            return glm::length(glm::dvec2(o.at.x - c.at.x, o.at.z - c.at.z)) < s.spacing;
        });
        if (!crowded)
            r.stash.push_back(c);
    }
}

// ---------------------------------------------------------------------------------------------------- dams

glm::dvec2 downstreamSide(const Heightfield& h, glm::dvec2 a, glm::dvec2 b, double reach)
{
    const glm::dvec2 d = b - a;
    const double len = glm::length(d);
    const glm::dvec2 nrm = len > 0.0 ? glm::dvec2(-d.y, d.x) / len : glm::dvec2(0.0, 1.0);
    double plus = 0.0, minus = 0.0;
    for (int k = 1; k <= 9; ++k)
    {
        const glm::dvec2 p = a + d * (double(k) / 10.0);
        for (int s = 1; s <= 4; ++s)
        {
            const glm::dvec2 q1 = p + nrm * (reach * s / 4.0), q2 = p - nrm * (reach * s / 4.0);
            plus += h.heightAt(q1.x, q1.y);
            minus += h.heightAt(q2.x, q2.y);
        }
    }
    return plus < minus ? nrm : -nrm;
}

std::vector<DamResult> applyDams(Heightfield& h, std::vector<DamSpec>& dams)
{
    LILA_ZONE();

    std::vector<DamResult> out;
    std::vector<float>& mask = h.mask("dam");
    for (DamSpec& s : dams)
    {
        DamResult r;
        r.id = s.id;
        const glm::dvec2 ab = s.b - s.a;
        r.length = glm::length(ab);
        if (r.length < h.cell)
        {
            out.push_back(r);
            continue;
        }
        if (std::isnan(s.crest))
            s.crest = std::min(h.heightAt(s.a.x, s.a.y), h.heightAt(s.b.x, s.b.y));
        double lowest = 1e30;
        for (int k = 0; k <= 64; ++k)
        {
            const glm::dvec2 p = s.a + ab * (k / 64.0);
            lowest = std::min(lowest, h.heightAt(p.x, p.y));
        }
        r.crest = s.crest;
        r.sill = s.sill();
        r.height = s.crest - lowest;
        r.downstream = downstreamSide(h, s.a, s.b, std::max(r.length * 0.5, 20.0 * h.cell));
        r.spillway = s.a + ab * s.spillwayAt;
        const double halfT = s.thickness * 0.5;
        const double reach = halfT + std::max(0.0, r.height) * s.faceSlope + h.cell;
        const int x0 = std::max(0, int(std::floor((std::min(s.a.x, s.b.x) - reach - h.origin.x) / h.cell)));
        const int x1 = std::min(h.nx - 1, int(std::ceil((std::max(s.a.x, s.b.x) + reach - h.origin.x) / h.cell)));
        const int z0 = std::max(0, int(std::floor((std::min(s.a.y, s.b.y) - reach - h.origin.y) / h.cell)));
        const int z1 = std::min(h.nz - 1, int(std::ceil((std::max(s.a.y, s.b.y) + reach - h.origin.y) / h.cell)));
        const glm::dvec2 dir = ab / r.length;
        for (int z = z0; z <= z1; ++z)
            for (int x = x0; x <= x1; ++x)
            {
                const glm::dvec2 p = h.samplePos(x, z);
                double t = 0.0;
                const double d = segmentDistance(p, s.a, s.b, &t);
                double top = s.crest;
                if (std::abs(glm::dot(p - r.spillway, dir)) <= s.spillwayWidth * 0.5)
                    top = r.sill;
                const double profile = d <= halfT ? top : top - (d - halfT) / s.faceSlope;
                const size_t i = h.index(x, z);
                if (profile > h.heights[i])
                {
                    r.fill += (profile - h.heights[i]) * h.cell * h.cell;
                    h.heights[i] = float(profile);
                    mask[i] = 1.0f;
                }
            }
        out.push_back(r);
    }
    return out;
}

std::vector<glm::dvec2> traceDownhill(const Heightfield& h, glm::dvec2 start, double maxLength)
{
    std::vector<glm::dvec2> out;
    if (h.empty())
        return out;
    int x = std::clamp(int(std::lround((start.x - h.origin.x) / h.cell)), 0, h.nx - 1);
    int z = std::clamp(int(std::lround((start.y - h.origin.y) / h.cell)), 0, h.nz - 1);
    const std::vector<float>* sea = h.findMask("sea");
    const std::vector<float>* water = h.findMask("water");
    std::vector<uint8_t> seen(h.size(), 0);
    double length = 0.0;
    int flatSteps = 0;
    out.push_back(h.samplePos(x, z));
    while (length < maxLength)
    {
        const size_t i = h.index(x, z);
        seen[i] = 1;
        if (x == 0 || z == 0 || x == h.nx - 1 || z == h.nz - 1)
            break;
        if (length > 5.0 * h.cell && ((sea && (*sea)[i] > 0.5f) || (water && (*water)[i] > 0.5f)))
            break;
        int bx = -1, bz = -1;
        double best = 1e30;
        for (int dz = -1; dz <= 1; ++dz)
            for (int dx = -1; dx <= 1; ++dx)
            {
                if (!dx && !dz)
                    continue;
                const size_t j = h.index(x + dx, z + dz);
                if (seen[j])
                    continue;
                const double v = (h.heights[j] - h.heights[i]) / (dx && dz ? std::numbers::sqrt2 : 1.0);
                if (v < best)
                {
                    best = v;
                    bx = x + dx;
                    bz = z + dz;
                }
            }
        if (bx < 0)
            break;
        // Out of a pit by its lowest rim, for a while; a long climb means the valley ended.
        flatSteps = best >= 0.0 ? flatSteps + 1 : 0;
        if (flatSteps > 40)
            break;
        length += glm::length(h.samplePos(bx, bz) - h.samplePos(x, z));
        x = bx;
        z = bz;
        out.push_back(h.samplePos(x, z));
    }
    return chaikin(resample(out, std::max(h.cell * 3.0, 1.0)), 2);
}

// ---------------------------------------------------------------------------------------------------- coast

CoastResult applyCoast(Heightfield& h, const CoastSpec& spec, const WaterResult* water)
{
    LILA_ZONE();

    CoastResult r;
    r.seaLevel = spec.seaLevel;
    if (h.empty())
        return r;
    h.mask("channel");
    for (const ChannelSpec& c : spec.channels)
        r.channels.push_back(carveChannel(h, c, spec.seaLevel, r.warnings));
    if (spec.masks.enabled)
        applyCoastMasks(h, spec.seaLevel, spec.masks, r);
    applyMangroves(h, spec.seaLevel, spec.mangroves, r);
    // Channels were measured right after their own carve; measure again on the final ground, which later channels may have cut.
    for (ChannelResult& c : r.channels)
    {
        const CraftClass* craft = craftClass(c.craft);
        for (ChannelStrand& s : c.strands)
            measureStrand(h, s, c.waterLevel, craft ? craft->minDepth : 0.0, c.width * 4.0);
    }
    if (spec.stash.enabled)
        findStashSpots(h, spec.stash, r);
    buildRoutes(h, spec, water, r);
    return r;
}
} // namespace terrain
