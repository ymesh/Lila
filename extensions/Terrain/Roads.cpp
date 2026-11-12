// Roads on heightfields: horizontal curves, a longitudinal profile of grades joined by vertical curves, banking, cut and
// fill at talus angles, retaining walls, bridge and tunnel spans, junctions and road masks. Pure functions of a heightfield
// in its own space; the Terrain Roads node and the road commands call them.

#include "Roads.h"

#include <lila/Profile.h>

#include <tbb/parallel_for.h>

#include <algorithm>
#include <cmath>
#include <functional>
#include <limits>
#include <map>
#include <memory>
#include <mutex>
#include <span>
#include <stdexcept>

using namespace lila;

namespace terrain
{
using json = nlohmann::json;

namespace roadsolve
{
namespace
{
constexpr double kPi = 3.14159265358979323846;
constexpr double kInf = std::numeric_limits<double>::infinity();

double radians(double deg) { return deg * kPi / 180.0; }
double cross2(glm::dvec2 a, glm::dvec2 b) { return a.x * b.y - a.y * b.x; }
/** Left of travel, in the heightfield's (X, Z): positive cross is to the left. */
glm::dvec2 leftOf(glm::dvec2 d) { return {-d.y, d.x}; }

glm::dvec2 unit2(glm::dvec2 v)
{
    const double l = glm::length(v);
    return l > 1e-12 ? v / l : glm::dvec2(1.0, 0.0);
}

struct NumField
{
    const char* name;
    double RoadProfile::* member;
};

constexpr NumField kProfileFields[] = {
    {"laneWidth", &RoadProfile::laneWidth},       {"shoulder", &RoadProfile::shoulder},         {"kerbHeight", &RoadProfile::kerbHeight},
    {"kerbWidth", &RoadProfile::kerbWidth},       {"sidewalk", &RoadProfile::sidewalk},         {"verge", &RoadProfile::verge},
    {"ditchWidth", &RoadProfile::ditchWidth},     {"ditchDepth", &RoadProfile::ditchDepth},     {"guardrailFill", &RoadProfile::guardrailFill},
    {"markerSpacing", &RoadProfile::markerSpacing}, {"poleSpacing", &RoadProfile::poleSpacing}, {"kerbReturn", &RoadProfile::kerbReturn},
    {"dirtEdge", &RoadProfile::dirtEdge},
};

struct DesignField
{
    const char* name;
    double RoadDesign::* member;
};

constexpr DesignField kDesignFields[] = {
    {"designSpeed", &RoadDesign::designSpeed},   {"maxGrade", &RoadDesign::maxGrade},         {"minVerticalRadius", &RoadDesign::minVerticalRadius},
    {"maxBank", &RoadDesign::maxBank},           {"bankRunoff", &RoadDesign::bankRunoff},     {"minCurveRadius", &RoadDesign::minCurveRadius},
    {"cutAngle", &RoadDesign::cutAngle},         {"fillAngle", &RoadDesign::fillAngle},       {"wallHeight", &RoadDesign::wallHeight},
    {"bridgeHeight", &RoadDesign::bridgeHeight}, {"tunnelDepth", &RoadDesign::tunnelDepth},   {"tunnelRadius", &RoadDesign::tunnelRadius},
    {"deckDepth", &RoadDesign::deckDepth},       {"pillarSpacing", &RoadDesign::pillarSpacing}, {"fitTolerance", &RoadDesign::fitTolerance},
};

std::vector<glm::dvec2> pointsFrom(const json& j)
{
    std::vector<glm::dvec2> out;
    if (j.is_array())
        for (const json& p : j)
            out.push_back({p.at(0).get<double>(), p.at(1).get<double>()});
    return out;
}
} // namespace
} // namespace roadsolve

using namespace roadsolve;

json RoadProfile::toJson() const
{
    json j = {{"lanes", lanes}, {"surface", surface}};
    for (const NumField& f : kProfileFields)
        j[f.name] = this->*f.member;
    return j;
}

void RoadProfile::read(const json& j)
{
    if (!j.is_object())
        return;
    if (j.contains("lanes"))
        lanes = std::clamp(j["lanes"].get<int>(), 1, 16);
    if (j.contains("surface"))
        surface = j["surface"].get<std::string>();
    for (const NumField& f : kProfileFields)
        if (j.contains(f.name))
            this->*f.member = std::max(0.0, j[f.name].get<double>());
    laneWidth = std::max(1.0, laneWidth);
}

double RoadDesign::speedRadius() const
{
    const double v = std::max(1.0, designSpeed);
    const double friction = std::clamp(0.19 - v / 1600.0, 0.08, 0.2);
    return v * v / (127.0 * (maxBank * 0.01 + friction)) * 100.0;
}

json RoadDesign::toJson() const
{
    json j = json::object();
    for (const DesignField& f : kDesignFields)
        j[f.name] = this->*f.member;
    return j;
}

void RoadDesign::read(const json& j)
{
    if (!j.is_object())
        return;
    for (const DesignField& f : kDesignFields)
        if (j.contains(f.name))
            this->*f.member = std::max(0.0, j[f.name].get<double>());
    cutAngle = std::clamp(cutAngle, 5.0, 89.0);
    fillAngle = std::clamp(fillAngle, 5.0, 89.0);
    designSpeed = std::max(1.0, designSpeed);
}

const std::vector<std::string>& roadKindNames()
{
    static const std::vector<std::string> n = {"road", "path", "track", "rail", "runway"};
    return n;
}

const std::vector<std::string>& roadProfileNames()
{
    static const std::vector<std::string> n = {"rural", "highway", "urban", "lane", "path", "track", "rail", "runway"};
    return n;
}

std::string defaultRoadProfile(const std::string& kind)
{
    if (kind == "road")
        return "rural";
    return kind;
}

RoadProfile roadProfilePreset(const std::string& name)
{
    RoadProfile p;
    if (name == "rural")
    {
        p.ditchWidth = 120.0;
        p.ditchDepth = 50.0;
    }
    else if (name == "highway")
    {
        p.lanes = 4, p.laneWidth = 375.0, p.shoulder = 250.0, p.verge = 200.0, p.ditchWidth = 200.0, p.ditchDepth = 70.0;
        p.guardrailFill = 200.0, p.markerSpacing = 5000.0, p.kerbReturn = 1500.0;
    }
    else if (name == "urban")
    {
        p.lanes = 2, p.laneWidth = 325.0, p.shoulder = 0.0, p.kerbHeight = 15.0, p.kerbWidth = 15.0, p.sidewalk = 200.0, p.verge = 50.0;
        p.guardrailFill = 0.0, p.markerSpacing = 0.0, p.poleSpacing = 3000.0, p.kerbReturn = 600.0, p.dirtEdge = 50.0;
    }
    else if (name == "lane")
    {
        p.lanes = 2, p.laneWidth = 275.0, p.shoulder = 0.0, p.verge = 50.0, p.ditchWidth = 60.0, p.ditchDepth = 30.0;
        p.guardrailFill = 0.0, p.markerSpacing = 0.0, p.kerbReturn = 400.0;
    }
    else if (name == "path")
    {
        p.lanes = 1, p.laneWidth = 200.0, p.shoulder = 0.0, p.verge = 30.0, p.surface = "dirt";
        p.guardrailFill = 0.0, p.markerSpacing = 0.0, p.kerbReturn = 150.0, p.dirtEdge = 80.0;
    }
    else if (name == "track")
    {
        p.lanes = 1, p.laneWidth = 300.0, p.shoulder = 0.0, p.verge = 50.0, p.surface = "gravel";
        p.guardrailFill = 0.0, p.markerSpacing = 0.0, p.kerbReturn = 300.0, p.dirtEdge = 120.0;
    }
    else if (name == "rail")
    {
        p.lanes = 1, p.laneWidth = 300.0, p.shoulder = 50.0, p.verge = 100.0, p.ditchWidth = 100.0, p.ditchDepth = 50.0, p.surface = "ballast";
        p.guardrailFill = 0.0, p.markerSpacing = 0.0, p.poleSpacing = 5000.0, p.kerbReturn = 0.0, p.dirtEdge = 100.0;
    }
    else if (name == "runway")
    {
        p.lanes = 1, p.laneWidth = 4500.0, p.shoulder = 750.0, p.verge = 1500.0, p.surface = "concrete";
        p.guardrailFill = 0.0, p.markerSpacing = 6000.0, p.kerbReturn = 0.0, p.dirtEdge = 300.0;
    }
    else
    {
        std::string all;
        for (const std::string& n : roadProfileNames())
            all += (all.empty() ? "" : ", ") + n;
        throw std::invalid_argument("profile '" + name + "' is not one of " + all);
    }
    return p;
}

RoadDesign roadDesignPreset(const std::string& kind)
{
    RoadDesign d;
    if (kind == "path")
    {
        d.designSpeed = 10.0, d.maxGrade = 15.0, d.minVerticalRadius = 2000.0, d.maxBank = 2.0, d.cutAngle = 50.0, d.fillAngle = 35.0;
        d.wallHeight = 300.0, d.bridgeHeight = 400.0, d.tunnelDepth = 1500.0, d.deckDepth = 60.0, d.pillarSpacing = 1500.0, d.fitTolerance = 30.0;
    }
    else if (kind == "track")
    {
        d.designSpeed = 30.0, d.maxGrade = 12.0, d.minVerticalRadius = 10000.0, d.maxBank = 4.0, d.cutAngle = 45.0, d.fillAngle = 35.0;
        d.wallHeight = 400.0, d.bridgeHeight = 600.0, d.tunnelDepth = 2000.0, d.deckDepth = 100.0, d.pillarSpacing = 2000.0;
    }
    else if (kind == "rail")
    {
        d.designSpeed = 120.0, d.maxGrade = 2.5, d.minVerticalRadius = 500000.0, d.maxBank = 10.0, d.bankRunoff = 0.3;
        d.bridgeHeight = 600.0, d.tunnelDepth = 1500.0, d.deckDepth = 200.0, d.pillarSpacing = 4000.0;
    }
    else if (kind == "runway")
    {
        d.designSpeed = 250.0, d.maxGrade = 1.5, d.minVerticalRadius = 3000000.0, d.maxBank = 0.0, d.minCurveRadius = 1.0e7;
        d.bridgeHeight = 0.0, d.tunnelDepth = 0.0, d.fitTolerance = 100.0;
    }
    else if (kind != "road")
    {
        std::string all;
        for (const std::string& n : roadKindNames())
            all += (all.empty() ? "" : ", ") + n;
        throw std::invalid_argument("kind '" + kind + "' is not one of " + all);
    }
    return d;
}

json Road::toJson() const
{
    json j = {{"id", id},
              {"kind", kind},
              {"profileName", profileName},
              {"profile", profile.toJson()},
              {"design", design.toJson()},
              {"smooth", smooth},
              {"masks", masks}};
    if (ring)
    {
        j["ring"] = true;
        j["ringCrossFall"] = ringCrossFall;
    }
    if (!object.empty())
        j["object"] = object;
    else
    {
        json p = json::array();
        for (const glm::dvec2& q : path)
            p.push_back(json::array({q.x, q.y}));
        j["path"] = p;
    }
    json k = json::array();
    for (const RoadKey& key : keys)
        k.push_back(json::array({key.at.x, key.at.y, key.height}));
    j["keys"] = k;
    return j;
}

Road Road::fromJson(const json& j)
{
    Road r;
    r.id = j.value("id", std::string());
    r.kind = j.value("kind", std::string("road"));
    r.design = roadDesignPreset(r.kind);
    r.profileName = j.value("profileName", std::string());
    if (r.profileName.empty())
        r.profileName = defaultRoadProfile(r.kind);
    r.profile = roadProfilePreset(r.profileName);
    if (j.contains("profile"))
        r.profile.read(j["profile"]);
    if (j.contains("design"))
        r.design.read(j["design"]);
    r.path = pointsFrom(j.value("path", json::array()));
    r.object = j.value("object", std::string());
    r.smooth = j.value("smooth", true);
    r.ring = j.value("ring", false);
    r.ringCrossFall = j.value("ringCrossFall", 2.0);
    if (j.contains("masks"))
        r.masks = j["masks"].get<std::vector<std::string>>();
    else if (r.kind == "road" || r.kind == "runway")
        r.masks = {"paved"};
    if (j.contains("keys") && j["keys"].is_array())
        for (const json& k : j["keys"])
            r.keys.push_back({{k.at(0).get<double>(), k.at(1).get<double>()}, k.at(2).get<double>()});
    return r;
}

namespace roadsolve
{
namespace
{
struct Station
{
    glm::dvec2 p{0.0};
    glm::dvec2 t{1.0, 0.0};
    double s = 0.0;
    double kappa = 0.0;
};

std::vector<glm::dvec2> withoutRepeats(const std::vector<glm::dvec2>& in, double eps)
{
    std::vector<glm::dvec2> out;
    for (const glm::dvec2& p : in)
        if (out.empty() || glm::length(p - out.back()) > eps)
            out.push_back(p);
    return out;
}

/** Douglas-Peucker in the plane, keeping the ends. */
std::vector<glm::dvec2> simplifyPlan(const std::vector<glm::dvec2>& pts, double tol)
{
    if (pts.size() < 3)
        return pts;
    std::vector<uint8_t> keep(pts.size(), 0);
    keep.front() = keep.back() = 1;
    std::vector<std::pair<size_t, size_t>> stack{{0, pts.size() - 1}};
    while (!stack.empty())
    {
        const auto [a, b] = stack.back();
        stack.pop_back();
        const glm::dvec2 d = pts[b] - pts[a];
        const double len = glm::length(d);
        double worst = -1.0;
        size_t at = a;
        for (size_t k = a + 1; k < b; ++k)
        {
            const double e = len > 1e-12 ? std::abs(cross2(d, pts[k] - pts[a])) / len : glm::length(pts[k] - pts[a]);
            if (e > worst)
                worst = e, at = k;
        }
        if (worst > tol)
        {
            keep[at] = 1;
            stack.push_back({a, at});
            stack.push_back({at, b});
        }
    }
    std::vector<glm::dvec2> out;
    for (size_t k = 0; k < pts.size(); ++k)
        if (keep[k])
            out.push_back(pts[k]);
    return out;
}

struct Piece
{
    bool arc = false;
    glm::dvec2 a{0.0}, b{0.0};
    glm::dvec2 c{0.0};
    double r = 0.0, a0 = 0.0, sweep = 0.0, len = 0.0;
};

/** The path with each corner rounded into an arc of radius (smaller where the legs are too short), sampled every ds. */
std::vector<Station> alignPlan(std::vector<glm::dvec2> line, double radius, double ds, bool smooth, double& minRadius, std::vector<std::string>& warnings)
{
    line = withoutRepeats(line, 1e-6);
    minRadius = kInf;
    if (line.size() < 2)
        return {};
    const size_t n = line.size();
    std::vector<double> legs(n - 1), tangent(n, 0.0), turn(n, 0.0);
    for (size_t k = 0; k + 1 < n; ++k)
        legs[k] = glm::length(line[k + 1] - line[k]);
    int tight = 0;
    if (smooth && radius > 0.0)
        for (size_t i = 1; i + 1 < n; ++i)
        {
            const glm::dvec2 d0 = unit2(line[i] - line[i - 1]), d1 = unit2(line[i + 1] - line[i]);
            double delta = std::acos(std::clamp(glm::dot(d0, d1), -1.0, 1.0));
            if (delta < 1e-5)
                continue;
            delta = std::min(delta, radians(176.0));
            turn[i] = delta;
            const double before = i == 1 ? legs[0] : legs[i - 1] * 0.5;
            const double after = i + 2 == n ? legs[i] : legs[i] * 0.5;
            tangent[i] = std::min(radius * std::tan(delta * 0.5), std::min(before, after));
            const double r = tangent[i] / std::tan(delta * 0.5);
            minRadius = std::min(minRadius, r);
            if (r < radius * 0.999)
                ++tight;
        }
    if (tight > 0)
        warnings.push_back(std::to_string(tight) + " corner(s) too close together for the " + std::to_string(int(std::lround(radius))) +
                           " cm design radius: tighter curves were used (minCurveRadius)");

    std::vector<Piece> pieces;
    glm::dvec2 cur = line[0];
    for (size_t i = 1; i < n; ++i)
    {
        if (i + 1 < n && tangent[i] > 0.0)
        {
            const glm::dvec2 d0 = unit2(line[i] - line[i - 1]), d1 = unit2(line[i + 1] - line[i]);
            const glm::dvec2 t1 = line[i] - d0 * tangent[i], t2 = line[i] + d1 * tangent[i];
            if (glm::length(t1 - cur) > 1e-9)
                pieces.push_back({false, cur, t1, {}, 0.0, 0.0, 0.0, glm::length(t1 - cur)});
            const double sign = cross2(d0, d1) >= 0.0 ? 1.0 : -1.0;
            const double r = tangent[i] / std::tan(turn[i] * 0.5);
            Piece arc;
            arc.arc = true;
            arc.r = r;
            arc.c = t1 + leftOf(d0) * sign * r;
            arc.a0 = std::atan2(t1.y - arc.c.y, t1.x - arc.c.x);
            arc.sweep = sign * turn[i];
            arc.len = r * turn[i];
            arc.a = t1;
            arc.b = t2;
            pieces.push_back(arc);
            cur = t2;
        }
        else
        {
            if (glm::length(line[i] - cur) > 1e-9)
                pieces.push_back({false, cur, line[i], {}, 0.0, 0.0, 0.0, glm::length(line[i] - cur)});
            cur = line[i];
        }
    }
    double total = 0.0;
    for (const Piece& p : pieces)
        total += p.len;
    if (total < 1e-6)
        return {};
    const size_t m = std::max<size_t>(2, size_t(std::ceil(total / ds)));
    std::vector<Station> out(m + 1);
    size_t pi = 0;
    double start = 0.0;
    for (size_t k = 0; k <= m; ++k)
    {
        const double s = total * double(k) / double(m);
        while (pi + 1 < pieces.size() && s > start + pieces[pi].len)
            start += pieces[pi++].len;
        const Piece& p = pieces[pi];
        const double u = std::clamp(s - start, 0.0, p.len);
        Station& st = out[k];
        st.s = s;
        if (p.arc)
        {
            const double a = p.a0 + p.sweep * (u / p.len);
            st.p = p.c + glm::dvec2(std::cos(a), std::sin(a)) * p.r;
            const double sign = p.sweep >= 0.0 ? 1.0 : -1.0;
            st.t = glm::dvec2(-std::sin(a), std::cos(a)) * sign;
            st.kappa = sign / p.r;
        }
        else
        {
            st.t = unit2(p.b - p.a);
            st.p = p.a + st.t * u;
        }
    }
    return out;
}

/** Means of values over a window of length along s, centred on each station. */
std::vector<double> movingMean(const std::vector<double>& s, const std::vector<double>& v, double window)
{
    const size_t m = s.size();
    std::vector<double> out(m);
    size_t lo = 0, hi = 0;
    double sum = 0.0;
    for (size_t k = 0; k < m; ++k)
    {
        while (hi < m && s[hi] <= s[k] + window * 0.5)
            sum += v[hi++];
        while (s[lo] < s[k] - window * 0.5)
            sum -= v[lo++];
        out[k] = sum / double(hi - lo);
    }
    return out;
}

/** Profile points kept by Douglas-Peucker on heights, with the forced ones always kept. */
void simplifyProfile(const std::vector<double>& s, const std::vector<double>& t, size_t a, size_t b, double tol, std::vector<uint8_t>& keep)
{
    std::vector<std::pair<size_t, size_t>> stack{{a, b}};
    while (!stack.empty())
    {
        const auto [i, j] = stack.back();
        stack.pop_back();
        double worst = -1.0;
        size_t at = i;
        for (size_t k = i + 1; k < j; ++k)
        {
            const double line = t[i] + (t[j] - t[i]) * (s[k] - s[i]) / (s[j] - s[i]);
            const double e = std::abs(t[k] - line);
            if (e > worst)
                worst = e, at = k;
        }
        if (worst > tol)
        {
            keep[at] = 1;
            stack.push_back({i, at});
            stack.push_back({at, j});
        }
    }
}
} // namespace
} // namespace roadsolve

std::vector<double> solveRoadProfile(const std::vector<double>& s, const std::vector<double>& g, const std::vector<std::pair<size_t, double>>& keys,
                                     double maxGrade, double minRadius, double window, double tolerance, double* achievedRadius,
                                     std::vector<std::string>* warnings)
{
    LILA_ZONE();

    const size_t n = s.size();
    if (achievedRadius)
        *achievedRadius = kInf;
    if (n < 2)
        return g;
    auto warn = [&](const std::string& w) {
        if (warnings)
            warnings->push_back(w);
    };
    const double G = std::max(0.0, maxGrade);
    std::vector<double> t = movingMean(s, g, std::max(window, 1e-6));
    std::vector<uint8_t> fixed(n, 0);
    for (const auto& [i, height] : keys)
        if (i < n)
        {
            t[i] = height;
            fixed[i] = 1;
        }
    for (int pass = 0; pass < 6; ++pass)
    {
        for (size_t k = 1; k < n; ++k)
            if (!fixed[k])
                t[k] = std::clamp(t[k], t[k - 1] - G * (s[k] - s[k - 1]), t[k - 1] + G * (s[k] - s[k - 1]));
        for (size_t k = n - 1; k-- > 0;)
            if (!fixed[k])
                t[k] = std::clamp(t[k], t[k + 1] - G * (s[k + 1] - s[k]), t[k + 1] + G * (s[k + 1] - s[k]));
    }
    for (size_t a = 0, b = 0; b < n; ++b)
        if (fixed[b])
        {
            if (fixed[a] && a != b && std::abs(t[b] - t[a]) > G * (s[b] - s[a]) + 1e-6)
                warn("height keys at " + std::to_string(int(s[a])) + " and " + std::to_string(int(s[b])) + " cm along are steeper than maxGrade apart");
            a = b;
        }

    std::vector<uint8_t> keep(n, 0);
    keep[0] = keep[n - 1] = 1;
    for (size_t k = 0; k < n; ++k)
        keep[k] = keep[k] | fixed[k];
    for (size_t a = 0, b = 1; b < n; ++b)
        if (keep[b])
        {
            simplifyProfile(s, t, a, b, std::max(1e-3, tolerance), keep);
            a = b;
        }
    for (size_t k = 0; k < n; ++k)
        keep[k] = keep[k] | fixed[k];

    // Points of intersection of the grade lines: station, height, forced (an end or a key), key height.
    std::vector<double> S, Y, K;
    std::vector<uint8_t> F;
    for (size_t k = 0; k < n; ++k)
        if (keep[k])
        {
            S.push_back(s[k]);
            Y.push_back(t[k]);
            F.push_back(uint8_t(k == 0 || k == n - 1 || fixed[k]));
            K.push_back(fixed[k] && k != 0 && k != n - 1 ? 1.0 : 0.0);
        }
    std::vector<double> keyHeight = Y;
    const double R = std::max(0.0, minRadius);
    std::vector<double> gr, L;
    auto measure = [&] {
        const size_t m = S.size();
        gr.assign(m - 1, 0.0);
        L.assign(m, 0.0);
        for (size_t j = 0; j + 1 < m; ++j)
            gr[j] = (Y[j + 1] - Y[j]) / (S[j + 1] - S[j]);
        for (size_t j = 1; j + 1 < m; ++j)
            L[j] = R * std::abs(gr[j] - gr[j - 1]);
    };
    auto change = [&](size_t j) { return j == 0 || j + 1 >= S.size() ? 0.0 : gr[j] - gr[j - 1]; };
    for (int round = 0; round < 24; ++round)
    {
        // Curves that would overlap drop the gentler of their two bends, unless it is forced.
        for (;;)
        {
            measure();
            size_t drop = SIZE_MAX;
            for (size_t j = 0; j + 1 < S.size() && drop == SIZE_MAX; ++j)
            {
                if (L[j] * 0.5 + L[j + 1] * 0.5 <= S[j + 1] - S[j] + 1e-9)
                    continue;
                const bool a = !F[j], b = !F[j + 1];
                if (a && b)
                    drop = std::abs(change(j)) <= std::abs(change(j + 1)) ? j : j + 1;
                else if (a)
                    drop = j;
                else if (b)
                    drop = j + 1;
            }
            if (drop == SIZE_MAX)
                break;
            S.erase(S.begin() + ptrdiff_t(drop));
            Y.erase(Y.begin() + ptrdiff_t(drop));
            F.erase(F.begin() + ptrdiff_t(drop));
            K.erase(K.begin() + ptrdiff_t(drop));
            keyHeight.erase(keyHeight.begin() + ptrdiff_t(drop));
        }
        // A key inside a vertical curve: move its grade point so the curve, not the grade lines, passes through it.
        bool moved = false;
        for (size_t j = 1; j + 1 < S.size(); ++j)
        {
            if (K[j] == 0.0)
                continue;
            for (int it = 0; it < 40; ++it)
            {
                const double g0 = (Y[j] - Y[j - 1]) / (S[j] - S[j - 1]), g1 = (Y[j + 1] - Y[j]) / (S[j + 1] - S[j]);
                const double A = g1 - g0;
                // The curve's length capped by the room beside it, which keeps this iteration contracting.
                const double len = std::min(R * std::abs(A), std::min(S[j] - S[j - 1], S[j + 1] - S[j]));
                const double next = keyHeight[j] - A * len / 8.0;
                const double step = 0.5 * (next - Y[j]);
                Y[j] += step;
                moved |= std::abs(step) > 1e-6;
                if (std::abs(step) < 1e-7)
                    break;
            }
        }
        // Moved keys can steepen the grades beside them: the free grade points follow, keys and ends stay.
        for (int pass = 0; pass < 2; ++pass)
        {
            for (size_t j = 1; j < S.size(); ++j)
                if (!F[j])
                    Y[j] = std::clamp(Y[j], Y[j - 1] - G * (S[j] - S[j - 1]), Y[j - 1] + G * (S[j] - S[j - 1]));
            for (size_t j = S.size() - 1; j-- > 0;)
                if (!F[j])
                    Y[j] = std::clamp(Y[j], Y[j + 1] - G * (S[j + 1] - S[j]), Y[j + 1] + G * (S[j + 1] - S[j]));
        }
        if (!moved)
            break;
    }
    measure();
    for (double v : gr)
        if (std::abs(v) > G + 1e-9)
        {
            warn("the height keys need grades over maxGrade");
            break;
        }
    // Curves still too long for their tangents (between forced points) are shortened: a tighter radius, reported.
    for (int it = 0; it < 200; ++it)
    {
        bool any = false;
        for (size_t j = 0; j + 1 < S.size(); ++j)
        {
            const double need = L[j] * 0.5 + L[j + 1] * 0.5, room = S[j + 1] - S[j];
            if (need > room + 1e-9)
            {
                L[j] *= room / need;
                L[j + 1] *= room / need;
                any = true;
            }
        }
        if (!any)
            break;
    }
    // The curves' lengths are settled: Newton steps put each key exactly on its curve.
    auto onKeys = [&] {
        for (int it = 0; it < 30; ++it)
        {
            double worst = 0.0;
            for (size_t j = 1; j + 1 < S.size(); ++j)
            {
                if (K[j] == 0.0)
                    continue;
                const double s1 = S[j] - S[j - 1], s2 = S[j + 1] - S[j];
                const double A = (Y[j + 1] - Y[j]) / s2 - (Y[j] - Y[j - 1]) / s1;
                const double miss = Y[j] + A * L[j] / 8.0 - keyHeight[j];
                Y[j] -= miss / (1.0 - L[j] / 8.0 * (1.0 / s1 + 1.0 / s2));
                worst = std::max(worst, std::abs(miss));
            }
            for (size_t j = 0; j + 1 < S.size(); ++j)
                gr[j] = (Y[j + 1] - Y[j]) / (S[j + 1] - S[j]);
            if (worst < 1e-6)
                break;
        }
        for (size_t j = 0; j + 1 < S.size(); ++j)
            gr[j] = (Y[j + 1] - Y[j]) / (S[j + 1] - S[j]);
    };
    onKeys();
    // A key moved onto its curve can steepen the grade beside it past maxGrade: that curve is tightened (reported
    // below as a tighter radius) and the free grade points follow, until the grades hold or the keys themselves need more.
    bool steepKeys = false;
    for (int round = 0; round < 16 && G > 0.0; ++round)
    {
        bool steep = false, tightened = false;
        for (size_t j = 0; j + 1 < S.size(); ++j)
            if (std::abs(gr[j]) > G + 1e-9)
            {
                steep = true;
                for (size_t k : {j, j + 1})
                    if (K[k] != 0.0 && L[k] > 0.0)
                    {
                        L[k] = L[k] < 2.0 ? 0.0 : 0.5 * L[k];
                        tightened = true;
                    }
            }
        if (!steep)
            break;
        for (int pass = 0; pass < 2; ++pass)
        {
            for (size_t j = 1; j < S.size(); ++j)
                if (!F[j])
                    Y[j] = std::clamp(Y[j], Y[j - 1] - G * (S[j] - S[j - 1]), Y[j - 1] + G * (S[j] - S[j - 1]));
            for (size_t j = S.size() - 1; j-- > 0;)
                if (!F[j])
                    Y[j] = std::clamp(Y[j], Y[j + 1] - G * (S[j + 1] - S[j]), Y[j + 1] + G * (S[j + 1] - S[j]));
        }
        onKeys();
        if (!tightened)
        {
            steepKeys = std::ranges::any_of(gr, [&](double v) { return std::abs(v) > G + 1e-9; });
            break;
        }
    }
    if (steepKeys && warnings && std::ranges::find(*warnings, std::string("the height keys need grades over maxGrade")) == warnings->end())
        warn("the height keys need grades over maxGrade");
    double achieved = kInf;
    for (size_t j = 1; j + 1 < S.size(); ++j)
        if (std::abs(change(j)) > 1e-12)
            achieved = std::min(achieved, L[j] / std::abs(change(j)));
    if (achieved < R * 0.999)
        warn("vertical curves between height keys are tighter than minVerticalRadius (" + std::to_string(int(achieved)) + " cm)");
    if (achievedRadius)
        *achievedRadius = achieved;

    std::vector<double> y(n);
    size_t j = 0;
    const size_t m = S.size();
    auto onCurve = [&](size_t pv, double si) {
        const double x = si - (S[pv] - L[pv] * 0.5);
        const double g0 = gr[pv - 1], A = change(pv);
        return Y[pv] - g0 * L[pv] * 0.5 + g0 * x + A * x * x / (2.0 * L[pv]);
    };
    for (size_t i = 0; i < n; ++i)
    {
        const double si = s[i];
        while (j + 2 < m && si > S[j + 1])
            ++j;
        if (j > 0 && L[j] > 1e-9 && si < S[j] + L[j] * 0.5)
            y[i] = onCurve(j, si);
        else if (j + 2 < m && L[j + 1] > 1e-9 && si > S[j + 1] - L[j + 1] * 0.5)
            y[i] = onCurve(j + 1, si);
        else
            y[i] = Y[j] + gr[j] * (si - S[j]);
    }
    return y;
}

namespace roadsolve
{
namespace
{
/** Superelevation per station: into each curve as speed and radius ask, capped, ramped in at the runoff rate, keys held. */
std::vector<double> solveBank(const std::vector<Station>& st, const RoadDesign& d, double pavedHalf, const std::vector<std::pair<size_t, double>>& keys)
{
    const size_t n = st.size();
    const double emax = d.maxBank * 0.01;
    std::vector<double> target(n, 0.0);
    for (size_t i = 0; i < n; ++i)
        if (std::abs(st[i].kappa) > 1e-12)
        {
            const double rm = 1.0 / std::abs(st[i].kappa) / 100.0;
            target[i] = (st[i].kappa > 0.0 ? 1.0 : -1.0) * std::min(emax, d.designSpeed * d.designSpeed / (127.0 * rm));
        }
    const double rate = d.bankRunoff * 0.01 / std::max(pavedHalf, 50.0);
    std::vector<double> f(target), b(target), e(n);
    for (size_t i = 1; i < n; ++i)
        f[i] = std::clamp(target[i], f[i - 1] - rate * (st[i].s - st[i - 1].s), f[i - 1] + rate * (st[i].s - st[i - 1].s));
    for (size_t i = n - 1; i-- > 0;)
        b[i] = std::clamp(target[i], b[i + 1] - rate * (st[i + 1].s - st[i].s), b[i + 1] + rate * (st[i + 1].s - st[i].s));
    for (size_t i = 0; i < n; ++i)
        e[i] = 0.5 * (f[i] + b[i]);
    if (!keys.empty())
    {
        std::vector<uint8_t> fixed(n, 0);
        for (const auto& [i, v] : keys)
            if (i < n)
            {
                e[i] = v;
                fixed[i] = 1;
            }
        for (int pass = 0; pass < 4; ++pass)
        {
            for (size_t i = 1; i < n; ++i)
                if (!fixed[i])
                    e[i] = std::clamp(e[i], e[i - 1] - rate * (st[i].s - st[i - 1].s), e[i - 1] + rate * (st[i].s - st[i - 1].s));
            for (size_t i = n - 1; i-- > 0;)
                if (!fixed[i])
                    e[i] = std::clamp(e[i], e[i + 1] - rate * (st[i + 1].s - st[i].s), e[i + 1] + rate * (st[i + 1].s - st[i].s));
        }
    }
    return e;
}

/** Stations of a laid road bucketed on a grid, so points near it find their nearest segment fast. */
struct StationGrid
{
    const RoadResult* road = nullptr;
    double size = 1.0;
    std::map<std::pair<int64_t, int64_t>, std::vector<uint32_t>> cells;

    StationGrid(const RoadResult& r, double reach) : road(&r)
    {
        double step = 0.0;
        for (size_t k = 1; k < r.along.size(); ++k)
            step = std::max(step, r.along[k] - r.along[k - 1]);
        size = std::max(reach + step, 1.0);
        for (size_t k = 0; k < r.centreline.size(); ++k)
            cells[key({r.centreline[k].x, r.centreline[k].z})].push_back(uint32_t(k));
    }
    std::pair<int64_t, int64_t> key(glm::dvec2 p) const { return {int64_t(std::floor(p.x / size)), int64_t(std::floor(p.y / size))}; }

    /** Nearest point on the road within reach of p (as RoadResult::surfaceAt), or station SIZE_MAX when none is that close. */
    RoadResult::Surface near(glm::dvec2 p) const
    {
        const RoadResult& r = *road;
        RoadResult::Surface best;
        best.station = SIZE_MAX;
        double bestD = kInf;
        const auto [cx, cz] = key(p);
        for (int64_t dz = -1; dz <= 1; ++dz)
            for (int64_t dx = -1; dx <= 1; ++dx)
            {
                auto it = cells.find({cx + dx, cz + dz});
                if (it == cells.end())
                    continue;
                for (uint32_t k : it->second)
                    for (size_t seg : {size_t(k) > 0 ? size_t(k) - 1 : size_t(k), size_t(k)})
                    {
                        if (seg + 1 >= r.centreline.size())
                            continue;
                        const glm::dvec2 a(r.centreline[seg].x, r.centreline[seg].z), b(r.centreline[seg + 1].x, r.centreline[seg + 1].z);
                        const glm::dvec2 ab = b - a;
                        const double len2 = std::max(1e-12, glm::dot(ab, ab));
                        const double raw = glm::dot(p - a, ab) / len2;
                        const double t = std::clamp(raw, 0.0, 1.0);
                        const double d = glm::length(p - (a + ab * t));
                        if (d < bestD - 1e-12)
                        {
                            bestD = d;
                            const double len = std::sqrt(len2);
                            best.station = t < 0.5 ? seg : seg + 1;
                            best.lateral = cross2(ab / len, p - a);
                            best.overshoot = r.ring ? 0.0 : (seg == 0 && raw < 0.0) ? -raw * len : (seg + 2 == r.centreline.size() && raw > 1.0) ? (raw - 1.0) * len : 0.0;
                            const double y = r.centreline[seg].y + (r.centreline[seg + 1].y - r.centreline[seg].y) * t;
                            const double e = r.bank[seg] + (r.bank[seg + 1] - r.bank[seg]) * t;
                            best.height = y - e * best.lateral;
                        }
                    }
            }
        return best;
    }
};

bool pointInPolygon(glm::dvec2 p, const std::vector<glm::dvec2>& poly)
{
    bool in = false;
    for (size_t i = 0, j = poly.size() - 1; i < poly.size(); j = i++)
        if ((poly[i].y > p.y) != (poly[j].y > p.y) && p.x < (poly[j].x - poly[i].x) * (p.y - poly[i].y) / (poly[j].y - poly[i].y) + poly[i].x)
            in = !in;
    return in;
}

/** Runs of true stations no shorter than minRun, with gaps under maxGap closed first. */
void cleanRuns(std::vector<uint8_t>& flag, const std::vector<double>& s, double maxGap, double minRun)
{
    const size_t n = flag.size();
    for (size_t i = 0; i < n;)
    {
        if (flag[i])
        {
            ++i;
            continue;
        }
        size_t j = i;
        while (j < n && !flag[j])
            ++j;
        if (i > 0 && j < n && s[j] - s[i - 1] < maxGap)
            std::fill(flag.begin() + ptrdiff_t(i), flag.begin() + ptrdiff_t(j), uint8_t(1));
        i = j;
    }
    for (size_t i = 0; i < n;)
    {
        if (!flag[i])
        {
            ++i;
            continue;
        }
        size_t j = i;
        while (j < n && flag[j])
            ++j;
        if (s[j - 1] - s[i] < minRun)
            std::fill(flag.begin() + ptrdiff_t(i), flag.begin() + ptrdiff_t(j), uint8_t(0));
        i = j;
    }
}

struct Earlier
{
    const Road* road;
    const RoadResult* result;
    std::unique_ptr<StationGrid> grid;
};

/** Slope of a laid road's plane along dir at p (rise per cm), from its banked surface either side. */
double planeSlope(const StationGrid& g, glm::dvec2 p, glm::dvec2 dir)
{
    const RoadResult::Surface a = g.near(p + dir * 50.0), b = g.near(p - dir * 50.0);
    if (a.station == SIZE_MAX || b.station == SIZE_MAX)
        return 0.0;
    return (a.height - b.height) / 100.0;
}

/** Centre of a closed loop of stations (the last repeats the first). */
glm::dvec2 ringCentre(const std::vector<Station>& st)
{
    glm::dvec2 c(0.0);
    for (size_t i = 0; i + 1 < st.size(); ++i)
        c += st[i].p / double(st.size() - 1);
    return c;
}

glm::dvec2 ringCentre(const std::vector<glm::dvec3>& centreline)
{
    glm::dvec2 c(0.0);
    for (size_t i = 0; i + 1 < centreline.size(); ++i)
        c += glm::dvec2(centreline[i].x, centreline[i].z) / double(centreline.size() - 1);
    return c;
}

/** One road laid into h after the earlier ones: its result, with locked marking the formations no later road may touch. */
RoadResult layRoad(Heightfield& h, const Road& rd, std::vector<glm::dvec2> line, const std::vector<Earlier>& earlier, std::vector<uint8_t>& locked)
{
    LILA_ZONE();

    RoadResult res;
    const RoadProfile& pr = rd.profile;
    const RoadDesign& de = rd.design;
    res.formationHalf = pr.formationHalf();
    res.pavedHalf = pr.pavedHalf();
    res.kerbHalf = pr.kerbHalf();
    const double fh = res.formationHalf;
    const double ds = std::clamp(h.cell * 0.5, 50.0, 1000.0);

    line = withoutRepeats(line, 1e-6);
    if (line.size() < 2)
    {
        res.warnings.push_back("the road needs at least two distinct points");
        return res;
    }
    res.ring = rd.ring;
    // Ring: the loop closed on its first point; it neither joins nor simplifies.
    double ringTurn = 0.0;
    if (rd.ring)
    {
        if (line.size() > 1 && glm::length(line.front() - line.back()) < 1e-6)
            line.pop_back();
        if (line.size() < 3)
        {
            res.warnings.push_back("a ring needs at least three distinct points");
            return res;
        }
        for (size_t k = 0; k < line.size(); ++k)
            ringTurn += cross2(line[k], line[(k + 1) % line.size()]);
        line.push_back(line.front());
    }
    else
    {
        // Ends close to an earlier road join it at its centreline.
        for (const Earlier& e : earlier)
            for (glm::dvec2* end : {&line.front(), &line.back()})
            {
                const RoadResult::Surface q = e.grid->near(*end);
                if (q.station == SIZE_MAX || q.overshoot > 0.0 || std::abs(q.lateral) > e.result->formationHalf + 2.0 * fh || e.result->span[q.station] != 0)
                    continue;
                const glm::dvec2 t = e.result->tangents[q.station];
                *end -= leftOf(t) * q.lateral;
            }
        // A routed polyline zigzags between grid samples; its turns, not its steps, are the alignment.
        if (line.size() > 2)
            line = simplifyPlan(line, std::max(h.cell * 0.75, 1.0));
    }

    const double radius = de.minCurveRadius > 0.0 ? de.minCurveRadius : de.speedRadius();
    const std::vector<Station> st = alignPlan(line, radius, ds, rd.smooth && !rd.ring, res.minCurveRadius, res.warnings);
    const size_t n = st.size();
    if (n < 2)
    {
        res.warnings.push_back("the road is too short to lay");
        return res;
    }
    std::vector<double> s(n), g(n);
    for (size_t i = 0; i < n; ++i)
    {
        s[i] = st[i].s;
        g[i] = h.heightAt(st[i].p.x, st[i].p.y);
    }
    res.length = s.back();

    std::vector<std::pair<size_t, double>> keys;
    for (const RoadKey& k : rd.keys)
    {
        size_t best = 0;
        double bd = kInf;
        for (size_t i = 0; i < n; ++i)
            if (const double d = glm::length(st[i].p - k.at); d < bd)
                bd = d, best = i;
        keys.push_back({best, k.height});
    }
    const double G = de.maxGrade * 0.01;
    const double window = std::max(4.0 * fh, 6.0 * h.cell);
    std::vector<std::string> profileWarnings;
    std::vector<double> y = solveRoadProfile(s, g, keys, G, de.minVerticalRadius, window, de.fitTolerance, nullptr, nullptr);
    // A ring is level at the mean ground under it, or at its first height key.
    double ringHeight = 0.0;
    if (rd.ring)
    {
        for (size_t i = 0; i + 1 < n; ++i)
            ringHeight += g[i] / double(n - 1);
        if (!rd.keys.empty())
            ringHeight = rd.keys.front().height;
    }

    // Junctions: where the centreline runs over an earlier road's paved width at grade.
    res.span.assign(n, 0);
    std::vector<std::pair<size_t, double>> bankKeys;
    struct Run
    {
        const Earlier* other;
        size_t from, to;
    };
    std::vector<Run> runs;
    /** A ring this road ends on: the station at the ring's edge and the direction away from it. */
    struct RingEnd
    {
        const Earlier* ring;
        size_t edge;
        glm::dvec2 away;
        int step;
    };
    std::vector<RingEnd> ringEnds;
    for (const Earlier& e : std::span<const Earlier>(earlier.data(), rd.ring ? 0 : earlier.size()))
    {
        std::vector<uint8_t> inside(n, 0);
        for (size_t i = 0; i < n; ++i)
        {
            const RoadResult::Surface q = e.grid->near(st[i].p);
            inside[i] = q.station != SIZE_MAX && q.overshoot <= 0.0 && std::abs(q.lateral) <= e.result->kerbHalf && e.result->span[q.station] == 0;
        }
        for (size_t i = 0; i < n;)
        {
            if (!inside[i])
            {
                ++i;
                continue;
            }
            size_t j = i;
            while (j + 1 < n && inside[j + 1])
                ++j;
            const size_t mid = (i + j) / 2;
            const RoadResult::Surface q = e.grid->near(st[mid].p);
            const double fillMid = y[mid] - g[mid];
            const bool separated = std::abs(y[mid] - q.height) > std::max(500.0, de.deckDepth + 300.0) ||
                                   (de.bridgeHeight > 0.0 && fillMid > de.bridgeHeight) || (de.tunnelDepth > 0.0 && -fillMid > de.tunnelDepth);
            if (!separated)
            {
                for (size_t k : {i, j})
                {
                    keys.push_back({k, e.grid->near(st[k].p).height});
                    bankKeys.push_back({k, -planeSlope(*e.grid, st[k].p, leftOf(st[k].t))});
                }
                for (size_t k = i; k <= j; ++k)
                    res.span[k] = 3;
                runs.push_back({&e, i, j});
                if (e.road->ring && (i == 0 || j == n - 1))
                    ringEnds.push_back({&e, i == 0 ? j : i, i == 0 ? st[j].t : -st[i].t, i == 0 ? 1 : -1});
                res.junctions.push_back({e.road->id, i == 0 || j == n - 1 ? "T" : "cross", {st[mid].p.x, q.height, st[mid].p.y}});
            }
            i = j + 1;
        }
    }
    double vr = kInf;
    std::vector<double> e;
    if (rd.ring)
    {
        // Level along, falling away from the island across: the island is on the left of a counter-clockwise loop.
        y.assign(n, ringHeight);
        e.assign(n, (ringTurn > 0.0 ? -1.0 : 1.0) * rd.ringCrossFall * 0.01);
        res.minVerticalRadius = vr;
    }
    else
    {
        y = solveRoadProfile(s, g, keys, G, de.minVerticalRadius, window, de.fitTolerance, &vr, &profileWarnings);
        res.minVerticalRadius = vr;
        for (const std::string& w : profileWarnings)
            res.warnings.push_back(w);
        e = solveBank(st, de, res.pavedHalf, bankKeys);
    }

    // Spans: deep fill bridges, deep cut tunnels; short ones close up, tiny ones go.
    if (!rd.ring && (de.bridgeHeight > 0.0 || de.tunnelDepth > 0.0))
    {
        std::vector<uint8_t> bridge(n, 0), tunnel(n, 0);
        const std::vector<float>* flow = h.findMask("flow");
        const std::vector<float>* lakes = h.findMask("lakes");
        for (size_t i = 0; i < n; ++i)
        {
            if (res.span[i] == 3)
                continue;
            // Rivers and lakes are bridged however low the deck: a road never dams water.
            const bool water = (flow && flow->size() == h.size() && h.maskAt("flow", st[i].p.x, st[i].p.y) >= 0.75) ||
                               (lakes && lakes->size() == h.size() && h.maskAt("lakes", st[i].p.x, st[i].p.y) > 0.0);
            bridge[i] = de.bridgeHeight > 0.0 && (y[i] - g[i] > de.bridgeHeight || water);
            tunnel[i] = de.tunnelDepth > 0.0 && g[i] - y[i] > de.tunnelDepth;
        }
        cleanRuns(bridge, s, std::max(2000.0, 4.0 * fh), std::max(fh, 2.0 * ds));
        cleanRuns(tunnel, s, std::max(2000.0, 4.0 * fh), std::max(fh, 2.0 * ds));
        for (size_t i = 0; i < n; ++i)
            if (res.span[i] != 3)
                res.span[i] = bridge[i] ? 1 : tunnel[i] ? 2 : 0;
    }
    for (size_t i = 0; i < n;)
    {
        const uint8_t k = res.span[i];
        size_t j = i;
        while (j + 1 < n && res.span[j + 1] == k)
            ++j;
        if (k == 1 || k == 2)
            res.spans.push_back({i, j, k == 1 ? "bridge" : "tunnel"});
        i = j + 1;
    }

    // Stations: the result's arrays.
    res.centreline.resize(n);
    res.tangents.resize(n);
    res.along = s;
    res.ground = g;
    res.bank = e;
    res.grade.resize(n);
    res.curvature.resize(n);
    for (size_t i = 0; i < n; ++i)
    {
        res.centreline[i] = {st[i].p.x, y[i], st[i].p.y};
        res.tangents[i] = st[i].t;
        res.curvature[i] = st[i].kappa;
        res.grade[i] = i + 1 < n ? (y[i + 1] - y[i]) / std::max(1e-9, s[i + 1] - s[i]) : res.grade[i - 1];
        res.maxGrade = std::max(res.maxGrade, std::abs(res.grade[i]) * 100.0);
        if (res.span[i] != 3)
            res.maxBank = std::max(res.maxBank, std::abs(e[i]) * 100.0);
    }

    // Each side of each station: where the talus slope meets the ground, and whether it is a wall instead.
    const double tanCut = std::tan(radians(de.cutAngle)), tanFill = std::tan(radians(de.fillAngle));
    const double slopeCap = de.wallHeight > 0.0 ? de.wallHeight / std::min(tanCut, tanFill) + 2.0 * h.cell : 30000.0;
    std::vector<double> extent[2];
    std::vector<uint8_t> wall[2], wallCut[2];
    for (int side = 0; side < 2; ++side)
    {
        extent[side].assign(n, 0.0);
        wall[side].assign(n, 0);
        wallCut[side].assign(n, 0);
    }
    const double march = std::max(h.cell * 0.5, 10.0);
    for (size_t i = 0; i < n; ++i)
    {
        if (res.span[i] != 0)
            continue;
        for (int side = 0; side < 2; ++side)
        {
            const double sgn = side == 0 ? 1.0 : -1.0;
            const glm::dvec2 out = leftOf(st[i].t) * sgn;
            const double ye = y[i] - e[i] * sgn * fh;
            const bool cutSide = h.heightAt((st[i].p + out * (fh + march)).x, (st[i].p + out * (fh + march)).y) > ye;
            const double start = fh + (cutSide ? pr.ditchWidth : 0.0);
            double r = 0.0, height = 0.0;
            bool met = false;
            for (; r <= slopeCap; r += march)
            {
                const glm::dvec2 q = st[i].p + out * (start + r);
                const double gq = h.heightAt(q.x, q.y);
                if (gq <= ye + r * tanCut && gq >= ye - r * tanFill)
                {
                    met = true;
                    height = gq - ye;
                    break;
                }
                height = gq - ye;
            }
            extent[side][i] = (cutSide ? pr.ditchWidth : 0.0) + std::min(r, slopeCap);
            if (de.wallHeight > 0.0 && (!met || std::abs(height) > de.wallHeight))
            {
                wall[side][i] = 1;
                wallCut[side][i] = height > 0.0;
            }
            else if (!met)
                extent[side][i] = 30000.0;
        }
    }
    for (int side = 0; side < 2; ++side)
    {
        cleanRuns(wall[side], s, 1000.0, 1000.0);
        for (size_t i = 0; i < n; ++i)
            if (wall[side][i])
                extent[side][i] = 0.0;
    }

    // Each sample near the road: its nearest station (exact on the station polyline), lateral offset and overshoot past the ends.
    double maxExtent = 0.0;
    for (int side = 0; side < 2; ++side)
        for (double v : extent[side])
            maxExtent = std::max(maxExtent, v);
    const double reach = fh + std::max(pr.dirtEdge, maxExtent) + h.cell * 2.0;
    glm::dvec2 lo(kInf), hi(-kInf);
    for (const Station& p : st)
    {
        lo = glm::min(lo, p.p);
        hi = glm::max(hi, p.p);
    }
    const int x0 = std::max(0, int(std::floor((lo.x - reach - h.origin.x) / h.cell)));
    const int z0 = std::max(0, int(std::floor((lo.y - reach - h.origin.y) / h.cell)));
    const int x1 = std::min(h.nx - 1, int(std::ceil((hi.x + reach - h.origin.x) / h.cell)));
    const int z1 = std::min(h.nz - 1, int(std::ceil((hi.y + reach - h.origin.y) / h.cell)));
    if (x1 < x0 || z1 < z0)
    {
        res.warnings.push_back("the road misses the terrain");
        return res;
    }
    const int w = x1 - x0 + 1, dz = z1 - z0 + 1;
    const size_t cells = size_t(w) * size_t(dz);
    std::vector<float> dist(cells, std::numeric_limits<float>::infinity());
    std::vector<uint32_t> seg(cells, 0);
    std::vector<float> segT(cells, 0.0f);
    // Chunks of stations: a sample tests a chunk's chord first and only runs its stations when the chord could beat the best.
    const size_t stride = std::max<size_t>(1, size_t(std::max(ds, reach * 0.5) / ds));
    for (size_t a = 0; a + 1 < n; a += stride)
    {
        const size_t b = std::min(n - 1, a + stride);
        glm::dvec2 blo(kInf), bhi(-kInf);
        double bow = 0.0;
        const glm::dvec2 chord = st[b].p - st[a].p;
        const double chordLen = glm::length(chord);
        for (size_t k = a; k <= b; ++k)
        {
            blo = glm::min(blo, st[k].p);
            bhi = glm::max(bhi, st[k].p);
            bow = std::max(bow, chordLen > 1e-9 ? std::abs(cross2(chord / chordLen, st[k].p - st[a].p)) : glm::length(st[k].p - st[a].p));
        }
        double r = fh + std::max(pr.dirtEdge, 0.0) + h.cell * 2.0;
        for (size_t k = a; k <= b; ++k)
            r = std::max(r, fh + std::max(pr.dirtEdge, std::max(extent[0][k], extent[1][k])) + h.cell * 2.0);
        const int ax = std::max(x0, int(std::floor((blo.x - r - h.origin.x) / h.cell))), bx = std::min(x1, int(std::ceil((bhi.x + r - h.origin.x) / h.cell)));
        const int az = std::max(z0, int(std::floor((blo.y - r - h.origin.y) / h.cell))), bz = std::min(z1, int(std::ceil((bhi.y + r - h.origin.y) / h.cell)));
        for (int z = az; z <= bz; ++z)
            for (int x = ax; x <= bx; ++x)
            {
                const glm::dvec2 p = h.samplePos(x, z);
                const size_t c = size_t(z - z0) * size_t(w) + size_t(x - x0);
                const double ct = chordLen > 1e-9 ? std::clamp(glm::dot(p - st[a].p, chord) / (chordLen * chordLen), 0.0, 1.0) : 0.0;
                const double cd = glm::length(p - (st[a].p + chord * ct));
                if (cd - bow > r || cd - bow >= double(dist[c]))
                    continue;
                for (size_t k = a; k < b; ++k)
                {
                    const glm::dvec2 ab = st[k + 1].p - st[k].p;
                    const double len2 = std::max(1e-12, glm::dot(ab, ab));
                    const double raw = glm::dot(p - st[k].p, ab) / len2;
                    const double t = std::clamp(raw, 0.0, 1.0);
                    const double d = glm::length(p - (st[k].p + ab * t));
                    if (d < dist[c])
                    {
                        dist[c] = float(d);
                        seg[c] = uint32_t(k);
                        segT[c] = float(raw);
                    }
                }
            }
    }

    std::vector<std::vector<float>*> extra;
    for (const std::string& m : rd.masks)
        if (!m.empty())
            extra.push_back(&h.mask(m));
    std::vector<float>& surfaceMask = h.mask(kRoadSurfaceMask);
    std::vector<float>& vergeMask = h.mask(kRoadVergeMask);
    std::vector<float>& dirtMask = h.mask(kRoadDirtMask);
    std::vector<float>& wearMask = h.mask(kRoadWearMask);
    std::vector<float>& protectMask = h.mask(kErosionProtectMask);
    const std::vector<float> before(h.heights);
    std::vector<double> rowCut(size_t(dz), 0.0), rowFill(size_t(dz), 0.0), rowArea(size_t(dz), 0.0);
    std::vector<uint8_t> claim(cells, 0);
    uint32_t seed = 2166136261u;
    for (char ch : rd.id)
        seed = (seed ^ uint8_t(ch)) * 16777619u;
    tbb::parallel_for(z0, z1 + 1, [&](int z) {
        for (int x = x0; x <= x1; ++x)
        {
            const size_t c = size_t(z - z0) * size_t(w) + size_t(x - x0);
            if (!std::isfinite(dist[c]))
                continue;
            const size_t k = seg[c];
            const double raw = segT[c];
            const double t = std::clamp(raw, 0.0, 1.0);
            const glm::dvec2 p = h.samplePos(x, z);
            const glm::dvec2 ab = st[k + 1].p - st[k].p;
            const double len = std::max(1e-9, glm::length(ab));
            const double d = cross2(ab / len, p - st[k].p);
            const double over = rd.ring ? 0.0 : (k == 0 && raw < 0.0) ? -raw * len : (k + 2 == n && raw > 1.0) ? (raw - 1.0) * len : 0.0;
            const size_t near = t < 0.5 ? k : k + 1;
            const uint8_t span = res.span[near];
            const double yc = y[k] + (y[k + 1] - y[k]) * t;
            const double ec = e[k] + (e[k + 1] - e[k]) * t;
            const double ad = std::abs(d);
            const int side = d >= 0.0 ? 0 : 1;
            const double sgn = side == 0 ? 1.0 : -1.0;
            const size_t idx = h.index(x, z);
            const double old = before[idx];
            if (span == 2 || span == 3)
                continue;
            if (span == 1)
            {
                if (ad <= fh && over <= 0.0 && !locked[idx])
                {
                    const double v = std::min(old, yc - ec * d - de.deckDepth);
                    h.heights[idx] = float(v);
                    rowCut[size_t(z - z0)] += old - v;
                }
                continue;
            }
            const bool onFormation = ad <= fh && over <= 0.0;
            double target = old;
            bool write = true;
            if (onFormation)
                target = yc - ec * d;
            else
            {
                const double edgeD = std::min(ad, fh) * sgn;
                const double ye = yc - ec * edgeD;
                double r = over > 0.0 ? (ad <= fh ? over : std::hypot(ad - fh, over)) : ad - fh;
                if (over <= 0.0 && wall[side][near])
                    write = false;
                else if (over <= 0.0 && pr.ditchWidth > 0.0 && old > ye && r <= pr.ditchWidth)
                    target = std::min(old, ye - pr.ditchDepth * (1.0 - std::abs(2.0 * r / pr.ditchWidth - 1.0)));
                else
                {
                    if (over <= 0.0 && pr.ditchWidth > 0.0 && old > ye)
                        r -= pr.ditchWidth;
                    target = std::clamp(old, ye - r * tanFill, ye + r * tanCut);
                }
            }
            if (write && !locked[idx])
            {
                h.heights[idx] = float(target);
                rowCut[size_t(z - z0)] += std::max(0.0, old - target);
                rowFill[size_t(z - z0)] += std::max(0.0, target - old);
                if (target != old || onFormation)
                    protectMask[idx] = std::max(protectMask[idx], 1.0f);
            }
            // Every sample the formation's bilinear surface reads from is protected, however coarse the grid.
            if (over <= 0.0 && ad <= fh + h.cell)
                protectMask[idx] = 1.0f;
            if (onFormation)
            {
                rowArea[size_t(z - z0)] += 1.0;
                claim[c] = 1;
            }
            // Masks: paved surface and verge with a cell-wide soft edge, the worn dirt band, wheel tracks.
            const double cellSize = h.cell;
            const double effD = over > 0.0 ? std::hypot(ad, over) : ad;
            const float paved = float(std::clamp((res.pavedHalf - effD) / cellSize + 0.5, 0.0, 1.0));
            const double vergeOuter = fh + (pr.ditchWidth > 0.0 ? pr.ditchWidth : 0.0);
            const float verge = float(std::min(std::clamp((effD - res.pavedHalf) / cellSize + 0.5, 0.0, 1.0), std::clamp((vergeOuter - effD) / cellSize + 0.5, 0.0, 1.0)));
            surfaceMask[idx] = std::max(surfaceMask[idx], paved);
            for (std::vector<float>* m : extra)
                (*m)[idx] = std::max((*m)[idx], paved);
            vergeMask[idx] = std::max(vergeMask[idx], verge);
            const double noise = 0.5 + 0.5 * gradientNoise(p.x / 300.0, p.y / 300.0, seed);
            if (pr.dirtEdge > 0.0 && effD > res.pavedHalf - cellSize)
            {
                const double band = 1.0 - std::clamp((effD - fh) / pr.dirtEdge, 0.0, 1.0);
                const float dirt = float(std::clamp(band * (0.4 + 0.8 * noise), 0.0, 1.0) * (effD >= res.pavedHalf ? 1.0 : 0.5));
                dirtMask[idx] = std::max(dirtMask[idx], dirt);
            }
            double wear = 0.0;
            if (over <= 0.0)
            {
                if (pr.surface == "dirt")
                    wear = std::max(0.0, 1.0 - ad / std::max(res.pavedHalf * 0.6, 1.0));
                else
                {
                    const int lanes = std::max(1, pr.lanes);
                    for (int l = 0; l < lanes; ++l)
                    {
                        const double centre = -pr.carriageway() * 0.5 + pr.laneWidth * (double(l) + 0.5);
                        for (double off : {-85.0, 85.0})
                            wear = std::max(wear, 1.0 - std::abs(d - centre - off) / std::max(45.0, cellSize * 0.75));
                    }
                }
                wear = std::clamp(wear * (0.6 + 0.4 * noise), 0.0, 1.0);
            }
            wearMask[idx] = std::max(wearMask[idx], float(wear));
        }
    });
    for (int z = 0; z < dz; ++z)
    {
        res.cut += rowCut[size_t(z)];
        res.fill += rowFill[size_t(z)];
        res.area += rowArea[size_t(z)];
    }
    res.cut *= h.cell * h.cell;
    res.fill *= h.cell * h.cell;
    res.area *= h.cell * h.cell;
    for (int z = z0; z <= z1; ++z)
        for (int x = x0; x <= x1; ++x)
            if (claim[size_t(z - z0) * size_t(w) + size_t(x - x0)])
                locked[h.index(x, z)] = 1;

    // Walls as runs along the formation edge.
    for (int side = 0; side < 2; ++side)
    {
        const double sgn = side == 0 ? 1.0 : -1.0;
        RoadResult::Wall cur;
        auto flush = [&] {
            if (cur.points.size() >= 2)
                res.walls.push_back(cur);
            cur.points.clear();
        };
        for (size_t i = 0; i < n; ++i)
        {
            if (!wall[side][i] || res.span[i] != 0)
            {
                flush();
                continue;
            }
            const glm::dvec2 at = st[i].p + leftOf(st[i].t) * sgn * fh;
            const double ye = y[i] - e[i] * sgn * fh;
            const glm::dvec2 q = st[i].p + leftOf(st[i].t) * sgn * (fh + h.cell);
            const double gq = before[h.index(std::clamp(int(std::lround((q.x - h.origin.x) / h.cell)), 0, h.nx - 1),
                                             std::clamp(int(std::lround((q.y - h.origin.y) / h.cell)), 0, h.nz - 1))];
            cur.side = side == 0 ? 1 : -1;
            cur.cut = wallCut[side][i];
            cur.points.push_back({at, std::max(ye, gq), std::min(ye, gq)});
        }
        flush();
    }

    // Bridges, tunnels, culverts.
    for (const RoadResult::Span& sp : res.spans)
    {
        if (sp.kind == "bridge")
        {
            RoadResult::Bridge b;
            const size_t a = sp.from > 0 ? sp.from - 1 : sp.from, z = std::min(n - 1, sp.to + 1);
            for (size_t i = a; i <= z; ++i)
            {
                b.deck.push_back(res.centreline[i]);
                b.maxHeight = std::max(b.maxHeight, y[i] - g[i]);
            }
            b.length = s[z] - s[a];
            const double spacing = std::max(de.pillarSpacing, 100.0);
            const int count = int(std::floor(b.length / spacing));
            for (int c = 1; c <= count; ++c)
            {
                const double at = s[a] + b.length * double(c) / double(count + 1);
                size_t i = a;
                while (i + 1 < z && s[i + 1] < at)
                    ++i;
                const double t = std::clamp((at - s[i]) / std::max(1e-9, s[i + 1] - s[i]), 0.0, 1.0);
                const glm::dvec2 p = st[i].p + (st[i + 1].p - st[i].p) * t;
                const double top = y[i] + (y[i + 1] - y[i]) * t - de.deckDepth;
                b.pillars.push_back({{p.x, top, p.y}, h.heightAt(p.x, p.y)});
            }
            res.bridges.push_back(std::move(b));
        }
        else
        {
            RoadResult::Tunnel tn;
            tn.radius = de.tunnelRadius > 0.0 ? de.tunnelRadius : fh * 1.1;
            for (size_t i = sp.from; i <= sp.to; ++i)
            {
                tn.floor.push_back(res.centreline[i]);
                tn.axis.push_back(res.centreline[i] + glm::dvec3(0.0, tn.radius * 0.5, 0.0));
            }
            tn.length = s[sp.to] - s[sp.from];
            tn.portals[0] = {res.centreline[sp.from], st[sp.from].t};
            tn.portals[1] = {res.centreline[sp.to], -st[sp.to].t};
            res.tunnels.push_back(std::move(tn));
        }
    }
    if (const std::vector<float>* flow = h.findMask("flow"); flow && flow->size() == h.size())
        for (size_t k = 1; k < n; ++k)
        {
            if (res.span[k] != 0)
                continue;
            const double a = h.maskAt("flow", st[k - 1].p.x, st[k - 1].p.y), b = h.maskAt("flow", st[k].p.x, st[k].p.y);
            if (a < 0.55 && b >= 0.55 && b < 0.75)
                res.culverts.push_back(st[k].p);
        }

    // Props: guardrails over high fill, walls on the fill side and bridges; markers and poles at their spacing.
    const double railOffset = std::min(res.pavedHalf + 50.0, fh);
    auto edgePoint = [&](size_t i, int sgn, double off) {
        const glm::dvec2 q = st[i].p + leftOf(st[i].t) * double(sgn) * off;
        return glm::dvec3(q.x, y[i] - e[i] * double(sgn) * off, q.y);
    };
    if (pr.guardrailFill > 0.0)
        for (int sgn : {1, -1})
        {
            std::vector<uint8_t> need(n, 0);
            const int side = sgn > 0 ? 0 : 1;
            for (size_t i = 0; i < n; ++i)
            {
                if (res.span[i] == 1)
                    need[i] = 1;
                else if (res.span[i] == 0)
                {
                    const glm::dvec2 q = st[i].p + leftOf(st[i].t) * double(sgn) * (fh + h.cell);
                    const double drop = (y[i] - e[i] * double(sgn) * fh) - h.heightAt(q.x, q.y);
                    need[i] = drop > pr.guardrailFill || (wall[side][i] && !wallCut[side][i]);
                }
            }
            cleanRuns(need, s, 2000.0, 1500.0);
            RoadResult::PropRun run;
            run.kind = "guardrail";
            run.side = sgn;
            for (size_t i = 0; i <= n; ++i)
            {
                if (i < n && need[i])
                    run.points.push_back(edgePoint(i, sgn, railOffset));
                else if (!run.points.empty())
                {
                    if (run.points.size() >= 2)
                        res.guardrails.push_back(run);
                    run.points.clear();
                }
            }
        }
    auto railedAt = [&](size_t i, int sgn) {
        for (const RoadResult::PropRun& r : res.guardrails)
            if (r.side == sgn)
                for (const glm::dvec3& p : r.points)
                    if (glm::length(glm::dvec2(p.x, p.z) - (st[i].p + leftOf(st[i].t) * double(sgn) * railOffset)) < 1.0)
                        return true;
        return false;
    };
    auto place = [&](const std::string& kind, double spacing, int sgn, double off, bool skipRailed) {
        if (spacing <= 0.0)
            return;
        for (double at = spacing * 0.5; at < res.length; at += spacing)
        {
            size_t i = size_t(std::lower_bound(s.begin(), s.end(), at) - s.begin());
            i = std::min(i, n - 1);
            if (res.span[i] == 2 || res.span[i] == 3 || (skipRailed && railedAt(i, sgn)))
                continue;
            res.props.push_back({kind, sgn, edgePoint(i, sgn, off), st[i].t});
        }
    };
    const double markerOffset = rd.kind == "runway" ? res.pavedHalf + 100.0 : 0.5 * (res.pavedHalf + fh);
    place(rd.kind == "runway" ? "edgeLight" : "marker", pr.markerSpacing, 1, markerOffset, true);
    place(rd.kind == "runway" ? "edgeLight" : "marker", pr.markerSpacing, -1, markerOffset, true);
    place(rd.kind == "rail" ? "mast" : "pole", pr.poleSpacing, -1, std::max(res.pavedHalf, fh - 30.0), false);

    // Junction corners: kerb returns between this road's edges and the other road's, on the other road's plane.
    for (const Run& run : runs)
    {
        const Earlier& o = *run.other;
        const double r = pr.kerbReturn;
        if (r <= 0.0)
            continue;
        for (size_t at : {run.from, run.to})
        {
            if ((at == run.from && run.from == 0) || (at == run.to && run.to == n - 1))
                continue;
            const glm::dvec2 u = at == run.from ? -st[at].t : st[at].t;
            const RoadResult::Surface q = o.grid->near(st[at].p);
            if (q.station == SIZE_MAX)
                continue;
            const glm::dvec2 tA = o.result->tangents[q.station];
            const glm::dvec2 cA(o.result->centreline[q.station].x, o.result->centreline[q.station].z);
            const RoadResult::Surface beyond = o.grid->near(st[at].p + u * o.result->kerbHalf);
            const double sideA = beyond.station != SIZE_MAX && beyond.lateral < 0.0 ? -1.0 : 1.0;
            const glm::dvec2 eA = cA + leftOf(tA) * sideA * o.result->kerbHalf;
            for (double sideB : {1.0, -1.0})
            {
                const glm::dvec2 eB = st[at].p + leftOf(st[at].t) * sideB * res.kerbHalf;
                const double den = cross2(u, tA);
                if (std::abs(den) < 1e-6)
                    continue;
                const double along = cross2(eA - eB, tA) / den;
                const glm::dvec2 c = eB + u * along;
                const glm::dvec2 u1 = tA * (glm::dot(tA, c - st[at].p) >= 0.0 ? 1.0 : -1.0);
                const double theta = std::acos(std::clamp(glm::dot(u1, u), -1.0, 1.0));
                if (theta < radians(15.0) || theta > radians(165.0))
                    continue;
                const double tl = r / std::tan(theta * 0.5);
                const glm::dvec2 t1 = c + u1 * tl, t2 = c + u * tl;
                const glm::dvec2 centre = c + unit2(u1 + u) * (r / std::sin(theta * 0.5));
                const double a1 = std::atan2(t1.y - centre.y, t1.x - centre.x);
                double sweep = std::atan2(t2.y - centre.y, t2.x - centre.x) - a1;
                while (sweep > kPi)
                    sweep -= 2.0 * kPi;
                while (sweep < -kPi)
                    sweep += 2.0 * kPi;
                auto planeAt = [&](glm::dvec2 p) {
                    const RoadResult::Surface sq = o.grid->near(p);
                    return sq.station == SIZE_MAX ? h.heightAt(p.x, p.y) : sq.height;
                };
                RoadResult::KerbReturn kr;
                kr.other = o.road->id;
                kr.corner = {c.x, planeAt(c), c.y};
                std::vector<glm::dvec2> poly{c};
                for (int k = 0; k <= 8; ++k)
                {
                    const double a = a1 + sweep * double(k) / 8.0;
                    const glm::dvec2 p = centre + glm::dvec2(std::cos(a), std::sin(a)) * r;
                    poly.push_back(p);
                    kr.arc.push_back({p.x, planeAt(p), p.y});
                }
                res.kerbReturns.push_back(kr);
                glm::dvec2 plo(kInf), phi(-kInf);
                for (const glm::dvec2& p : poly)
                {
                    plo = glm::min(plo, p);
                    phi = glm::max(phi, p);
                }
                const int px0 = std::max(0, int(std::floor((plo.x - h.origin.x) / h.cell))), px1 = std::min(h.nx - 1, int(std::ceil((phi.x - h.origin.x) / h.cell)));
                const int pz0 = std::max(0, int(std::floor((plo.y - h.origin.y) / h.cell))), pz1 = std::min(h.nz - 1, int(std::ceil((phi.y - h.origin.y) / h.cell)));
                for (int z = pz0; z <= pz1; ++z)
                    for (int x = px0; x <= px1; ++x)
                    {
                        const glm::dvec2 p = h.samplePos(x, z);
                        if (!pointInPolygon(p, poly))
                            continue;
                        const size_t idx = h.index(x, z);
                        h.heights[idx] = float(planeAt(p));
                        surfaceMask[idx] = 1.0f;
                        protectMask[idx] = 1.0f;
                        locked[idx] = 1;
                    }
            }
        }
    }

    // Islands: a ring's raised centre, and a splitter where this road enters a ring.
    const double rise = pr.kerbHeight > 0.0 ? pr.kerbHeight : 15.0;
    std::vector<float>& islandMask = h.mask(kRoadIslandMask);
    auto raiseIsland = [&](RoadResult::Island island, const std::function<double(glm::dvec2)>& top) {
        glm::dvec2 plo(kInf), phi(-kInf);
        for (const glm::dvec2& p : island.outline)
        {
            plo = glm::min(plo, p);
            phi = glm::max(phi, p);
        }
        const int px0 = std::max(0, int(std::floor((plo.x - h.origin.x) / h.cell))), px1 = std::min(h.nx - 1, int(std::ceil((phi.x - h.origin.x) / h.cell)));
        const int pz0 = std::max(0, int(std::floor((plo.y - h.origin.y) / h.cell))), pz1 = std::min(h.nz - 1, int(std::ceil((phi.y - h.origin.y) / h.cell)));
        for (int z = pz0; z <= pz1; ++z)
            for (int x = px0; x <= px1; ++x)
            {
                const glm::dvec2 p = h.samplePos(x, z);
                if (!pointInPolygon(p, island.outline))
                    continue;
                const size_t idx = h.index(x, z);
                h.heights[idx] = float(top(p));
                islandMask[idx] = 1.0f;
                surfaceMask[idx] = 0.0f;
                protectMask[idx] = 1.0f;
                locked[idx] = 1;
            }
        res.islands.push_back(std::move(island));
    };
    if (rd.ring)
    {
        const glm::dvec2 c = ringCentre(st);
        double radiusMean = 0.0;
        for (size_t i = 0; i + 1 < n; ++i)
            radiusMean += glm::length(st[i].p - c) / double(n - 1);
        const double inner = radiusMean - res.kerbHalf;
        if (inner > h.cell)
        {
            RoadResult::Island island;
            island.kind = "central";
            // The inner kerb is the ring's high edge.
            island.height = ringHeight + std::abs(e[0]) * res.kerbHalf + rise;
            for (int k = 0; k < 64; ++k)
            {
                const double a = 2.0 * kPi * double(k) / 64.0;
                island.outline.push_back(c + glm::dvec2(std::cos(a), std::sin(a)) * inner);
            }
            const double top = island.height;
            raiseIsland(std::move(island), [top](glm::dvec2) { return top; });
        }
    }
    for (const RingEnd& re : ringEnds)
    {
        const RoadResult& ring = *re.ring->result;
        const glm::dvec2 c = ringCentre(ring.centreline);
        double radiusMean = 0.0;
        for (size_t k = 0; k + 1 < ring.centreline.size(); ++k)
            radiusMean += glm::length(glm::dvec2(ring.centreline[k].x, ring.centreline[k].z) - c) / double(ring.centreline.size() - 1);
        // The island's base sits on the ring's outer kerb, as wide as both shoulders, tapering back along this road.
        const double outer = radiusMean + ring.kerbHalf;
        const double base = std::max(100.0, std::min(2.0 * pr.shoulder, res.pavedHalf));
        const double length = std::max(1500.0, 4.0 * base);
        size_t k = re.edge;
        while ((re.step > 0 ? k + 1 < n : k > 0) && glm::length(st[k].p - c) < outer)
            k = re.step > 0 ? k + 1 : k - 1;
        const glm::dvec2 nose = st[k].p;
        const glm::dvec2 side = leftOf(re.away) * (base * 0.5);
        RoadResult::Island island;
        island.kind = "splitter";
        island.other = re.ring->road->id;
        island.outline = {nose + side, nose + re.away * length, nose - side};
        island.height = y[k] + rise;
        raiseIsland(std::move(island), [&](glm::dvec2 p) { return res.surfaceAt(p).height + rise; });
    }
    return res;
}
} // namespace
} // namespace roadsolve

RoadResult::Surface RoadResult::surfaceAt(glm::dvec2 p) const
{
    Surface best;
    double bestD = kInf;
    for (size_t k = 0; k + 1 < centreline.size(); ++k)
    {
        const glm::dvec2 a(centreline[k].x, centreline[k].z), b(centreline[k + 1].x, centreline[k + 1].z);
        const glm::dvec2 ab = b - a;
        const double len2 = std::max(1e-12, glm::dot(ab, ab));
        const double raw = glm::dot(p - a, ab) / len2;
        const double t = std::clamp(raw, 0.0, 1.0);
        const double d = glm::length(p - (a + ab * t));
        if (d < bestD - 1e-12)
        {
            bestD = d;
            const double len = std::sqrt(len2);
            best.station = t < 0.5 ? k : k + 1;
            best.lateral = cross2(ab / len, p - a);
            best.overshoot = ring ? 0.0 : (k == 0 && raw < 0.0) ? -raw * len : (k + 2 == centreline.size() && raw > 1.0) ? (raw - 1.0) * len : 0.0;
            const double y = centreline[k].y + (centreline[k + 1].y - centreline[k].y) * t;
            const double e = bank[k] + (bank[k + 1] - bank[k]) * t;
            best.height = y - e * best.lateral;
        }
    }
    return best;
}

std::map<std::string, RoadResult> applyRoads(Heightfield& h, const std::vector<RoadInput>& roads)
{
    LILA_ZONE();

    std::map<std::string, RoadResult> out;
    if (h.empty())
        return out;
    std::vector<uint8_t> locked(h.size(), 0);
    std::vector<Earlier> earlier;
    for (const RoadInput& in : roads)
    {
        RoadResult r = layRoad(h, in.road, in.line, earlier, locked);
        auto [it, added] = out.insert_or_assign(in.road.id, std::move(r));
        (void)added;
        if (!it->second.empty())
        {
            Earlier e;
            e.road = &in.road;
            e.result = &it->second;
            e.grid = std::make_unique<StationGrid>(it->second, it->second.formationHalf * 3.0 + 1000.0);
            earlier.push_back(std::move(e));
        }
    }
    return out;
}

namespace roadsolve
{
namespace
{
struct PublishedRoads
{
    std::mutex mutex;
    std::map<std::pair<uint64_t, std::string>, std::map<std::string, RoadResult>> results;
};

PublishedRoads& publishedRoads()
{
    static PublishedRoads p;
    return p;
}
} // namespace
} // namespace roadsolve

void publishRoadResults(uint64_t modifier, const std::string& node, std::map<std::string, RoadResult> results)
{
    PublishedRoads& p = publishedRoads();
    std::lock_guard lock(p.mutex);
    p.results[{modifier, node}] = std::move(results);
}

std::map<std::string, RoadResult> roadResults(uint64_t modifier, const std::string& node)
{
    PublishedRoads& p = publishedRoads();
    std::lock_guard lock(p.mutex);
    auto it = p.results.find({modifier, node});
    return it == p.results.end() ? std::map<std::string, RoadResult>{} : it->second;
}
} // namespace terrain
