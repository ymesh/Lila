// Ground cover: rules from biomes, layers and masks to scattered pieces with footprints, placed deterministically.

#include "GroundCover.h"

#include <lila/Profile.h>

#include <tbb/parallel_for.h>

#include <algorithm>
#include <cmath>
#include <set>
#include <span>
#include <stdexcept>
#include <unordered_map>

using namespace lila;

namespace terrain
{
using json = nlohmann::json;

namespace
{
uint32_t coverHash(uint32_t a, uint32_t b, uint32_t c)
{
    uint32_t h = a * 0x8da6b343u ^ b * 0xd8163841u ^ c * 0xcb1ab31fu;
    h ^= h >> 16;
    h *= 0x7feb352du;
    h ^= h >> 15;
    h *= 0x846ca68bu;
    h ^= h >> 16;
    return h;
}

double coverUnit(uint32_t h) { return double(h >> 8) / double(1u << 24); }

double coverSample(const std::vector<float>& f, const Heightfield& h, double x, double z)
{
    const double fx = std::clamp((x - h.origin.x) / h.cell, 0.0, double(h.nx - 1)), fz = std::clamp((z - h.origin.y) / h.cell, 0.0, double(h.nz - 1));
    const int x0 = std::min(int(fx), h.nx - 2), z0 = std::min(int(fz), h.nz - 2);
    const double tx = fx - x0, tz = fz - z0;
    auto v = [&](int a, int b) { return double(f[h.index(a, b)]); };
    return (v(x0, z0) * (1 - tx) + v(x0 + 1, z0) * tx) * (1 - tz) + (v(x0, z0 + 1) * (1 - tx) + v(x0 + 1, z0 + 1) * tx) * tz;
}

/** A rule's inputs as grids; a missing one makes the rule fit nowhere unless inverted. */
struct CoverInputs
{
    std::vector<std::pair<const Term*, const std::vector<float>*>> terms;
    std::vector<const std::vector<float>*> weights;
    bool dead = false;

    double fit(const Heightfield& h, double x, double z) const
    {
        if (dead)
            return 0.0;
        double a = 1.0;
        for (const auto& [t, g] : terms)
            a *= t->eval(g ? coverSample(*g, h, x, z) : 0.0);
        if (!weights.empty())
        {
            double w = 0.0;
            for (const std::vector<float>* g : weights)
                w += coverSample(*g, h, x, z);
            a *= std::clamp(w, 0.0, 1.0);
        }
        return a;
    }
};

struct Candidate
{
    glm::dvec2 p;
    double fit;
    uint32_t hash;
    int32_t i = 0, j = 0;
};

int64_t cellKey(int x, int z) { return (int64_t(x) << 32) ^ int64_t(uint32_t(z)); }

/** Pieces bucketed by position, for footprint checks. */
class FootprintGrid
{
public:
    explicit FootprintGrid(double cell) : mCell(std::max(1.0, cell)) {}

    bool clear(const std::vector<CoverPiece>& pieces, glm::dvec2 p, double r, double reach) const
    {
        const int cx = int(std::floor(p.x / mCell)), cz = int(std::floor(p.y / mCell));
        const int ring = int(std::ceil((r + reach) / mCell));
        for (int dz = -ring; dz <= ring; ++dz)
            for (int dx = -ring; dx <= ring; ++dx)
            {
                auto it = mCells.find(cellKey(cx + dx, cz + dz));
                if (it == mCells.end())
                    continue;
                for (uint32_t i : it->second)
                {
                    const CoverPiece& o = pieces[i];
                    const double d = glm::length(glm::dvec2(o.position.x, o.position.z) - p);
                    if (d < r + o.radius)
                        return false;
                }
            }
        return true;
    }

    void add(glm::dvec2 p, uint32_t index) { mCells[cellKey(int(std::floor(p.x / mCell)), int(std::floor(p.y / mCell)))].push_back(index); }

private:
    double mCell;
    std::unordered_map<int64_t, std::vector<uint32_t>> mCells;
};
} // namespace

json CoverSpec::toJson() const
{
    json rulesJ = json::array();
    for (const CoverRule& r : rules)
    {
        json when = json::object();
        for (const Term& t : r.terms)
            when[t.input] = t.toJson();
        if (!r.biomes.empty())
            when["biome"] = r.biomes;
        if (!r.layers.empty())
            when["layer"] = r.layers;
        rulesJ.push_back({{"name", r.name},
                          {"items", r.items},
                          {"density", r.density},
                          {"footprint", r.footprint},
                          {"when", when},
                          {"scale", json::array({r.scaleMin, r.scaleMax})},
                          {"align", r.align},
                          {"turn", r.turn},
                          {"sink", r.sink},
                          {"solid", r.solid},
                          {"minFit", r.minFit},
                          {"faceDownhill", r.faceDownhill}});
    }
    return {{"seed", seed}, {"maxCount", maxCount}, {"densityScale", densityScale}, {"rules", rulesJ}};
}

CoverSpec CoverSpec::fromJson(const json& j)
{
    CoverSpec s;
    const json* list = &j;
    if (j.is_object())
    {
        s.seed = j.value("seed", s.seed);
        s.maxCount = std::max(0, j.value("maxCount", s.maxCount));
        s.densityScale = std::max(0.0, j.value("densityScale", s.densityScale));
        if (!j.contains("rules"))
            throw std::runtime_error("ground cover needs rules: a list of {name, items, density, when, ...}");
        list = &j["rules"];
    }
    json expanded = json::array();
    if (list->is_string())
        expanded.push_back(*list);
    else if (list->is_array())
        expanded = *list;
    else
        throw std::runtime_error("rules is a list of {name, items, density, when, ...} or preset names");
    std::set<std::string> names;
    for (json rj : expanded)
    {
        // A preset by name, or {preset: name, ...overrides}.
        if (rj.is_string() || (rj.is_object() && rj.contains("preset")))
        {
            const std::string preset = rj.is_string() ? rj.get<std::string>() : rj["preset"].get<std::string>();
            CoverRule base = coverPreset(preset);
            json pj = CoverSpec{{base}}.toJson()["rules"][0];
            if (rj.is_object())
                for (const auto& [k, v] : rj.items())
                    if (k != "preset")
                        pj[k] = v;
            rj = pj;
        }
        if (!rj.is_object())
            throw std::runtime_error("each rule is {name, items, density, when, ...}");
        CoverRule r;
        r.name = rj.value("name", std::string("cover") + std::to_string(s.rules.size() + 1));
        if (!names.insert(r.name).second)
            throw std::runtime_error("two rules are named " + r.name);
        if (rj.contains("items"))
        {
            const json& it = rj["items"];
            if (it.is_string())
                r.items.push_back(it.get<std::string>());
            else if (it.is_array())
                for (const json& i : it)
                    r.items.push_back(i.get<std::string>());
        }
        if (r.items.empty())
            throw std::runtime_error(r.name + ": give items, the objects to scatter");
        r.density = std::max(0.0, rj.value("density", r.density));
        r.footprint = std::max(0.0, rj.value("footprint", r.footprint));
        std::vector<std::vector<Term>> any;
        readWhen(rj.value("when", json()), r.terms, any, r.biomes, r.name, &r.layers);
        if (!any.empty())
            throw std::runtime_error(r.name + ": when is one {input: range} object; use another rule for an alternative");
        if (rj.contains("scale"))
        {
            const json& sc = rj["scale"];
            if (sc.is_number())
                r.scaleMin = r.scaleMax = sc.get<double>();
            else if (sc.is_array() && sc.size() == 2)
            {
                r.scaleMin = sc[0].get<double>();
                r.scaleMax = sc[1].get<double>();
            }
            else
                throw std::runtime_error(r.name + ": scale is a number or [min, max]");
        }
        r.scaleMin = std::max(1e-3, r.scaleMin);
        r.scaleMax = std::max(r.scaleMin, r.scaleMax);
        r.align = std::clamp(rj.value("align", r.align), 0.0, 1.0);
        r.turn = std::clamp(rj.value("turn", r.turn), 0.0, 180.0);
        r.sink = rj.value("sink", r.sink);
        r.solid = rj.value("solid", r.solid);
        r.minFit = std::clamp(rj.value("minFit", r.minFit), 0.0, 1.0);
        r.faceDownhill = rj.value("faceDownhill", r.faceDownhill);
        s.rules.push_back(std::move(r));
    }
    return s;
}

Heightfield heightfieldFromGridMesh(const geo::Mesh& m)
{
    LILA_ZONE();

    const size_t n = size_t(m.numPoints());
    if (n < 4)
        return {};
    const double z0 = m.position(0).z;
    const double tol = 1e-6 * std::max(1.0, glm::length(m.position(geo::Index(n - 1)) - m.position(0)));
    size_t cx = 1;
    while (cx < n && std::abs(m.position(geo::Index(cx)).z - z0) <= tol)
        ++cx;
    if (cx < 2 || n % cx != 0 || n / cx < 2)
        return {};
    const size_t cz = n / cx;
    std::vector<double> xs(cx), zs(cz);
    for (size_t i = 0; i < cx; ++i)
        xs[i] = m.position(geo::Index(i)).x;
    for (size_t j = 0; j < cz; ++j)
        zs[j] = m.position(geo::Index(j * cx)).z;
    for (size_t i = 1; i < cx; ++i)
        if (xs[i] <= xs[i - 1])
            return {};
    for (size_t j = 1; j < cz; ++j)
        if (zs[j] <= zs[j - 1])
            return {};
    // The last point must sit on the last column and row, or this isn't a grid.
    const glm::dvec3 last = m.position(geo::Index(n - 1));
    if (std::abs(last.x - xs.back()) > tol || std::abs(last.z - zs.back()) > tol)
        return {};
    const double cell = xs[1] - xs[0];
    auto even = [&](const std::vector<double>& v) {
        for (size_t i = 1; i < v.size(); ++i)
            if (std::abs((v[i] - v[i - 1]) - cell) > 1e-4 * cell)
                return false;
        return true;
    };
    std::vector<std::pair<std::string, std::span<const float>>> attrs;
    for (const std::string& name : m.pointAttrs().names())
        if (std::span<const float> v = m.pointAttrs().get<float>(name); v.size() == n)
            attrs.push_back({name, v});
    Heightfield h;
    if (even(xs) && even(zs))
    {
        h = geo::makeHeightfield(int(cx), int(cz), cell, {xs[0], zs[0]});
        for (size_t k = 0; k < n; ++k)
            h.heights[k] = float(m.position(geo::Index(k)).y);
        for (const auto& [name, v] : attrs)
            h.masks[name].assign(v.begin(), v.end());
        return h;
    }
    // Stepped: even spacing at the step, the shorter last span resampled.
    const int nx = int(std::lround((xs.back() - xs[0]) / cell)) + 1, nz = int(std::lround((zs.back() - zs[0]) / cell)) + 1;
    h = geo::makeHeightfield(std::max(2, nx), std::max(2, nz), cell, {xs[0], zs[0]});
    auto locate = [](const std::vector<double>& v, double t, size_t& i, double& f) {
        const auto it = std::upper_bound(v.begin(), v.end(), t);
        i = std::min(size_t(std::max<ptrdiff_t>(1, it - v.begin())), v.size() - 1) - 1;
        f = std::clamp((t - v[i]) / (v[i + 1] - v[i]), 0.0, 1.0);
    };
    std::vector<std::pair<std::string, std::vector<float>*>> masks;
    for (const auto& [name, v] : attrs)
        masks.push_back({name, &h.mask(name)});
    tbb::parallel_for(0, h.nz, [&](int z) {
        size_t j;
        double tz;
        locate(zs, std::min(zs.back(), zs[0] + z * cell), j, tz);
        for (int x = 0; x < h.nx; ++x)
        {
            size_t i;
            double tx;
            locate(xs, std::min(xs.back(), xs[0] + x * cell), i, tx);
            auto mix = [&](auto value) {
                const double a = value(j * cx + i), b = value(j * cx + i + 1), c = value((j + 1) * cx + i), d = value((j + 1) * cx + i + 1);
                return (a * (1 - tx) + b * tx) * (1 - tz) + (c * (1 - tx) + d * tx) * tz;
            };
            const size_t k = h.index(x, z);
            h.heights[k] = float(mix([&](size_t p) { return m.position(geo::Index(p)).y; }));
            for (size_t a = 0; a < attrs.size(); ++a)
                (*masks[a].second)[k] = float(mix([&](size_t p) { return double(attrs[a].second[p]); }));
        }
    });
    return h;
}

CoverSpec defaultCover(const Heightfield& h)
{
    auto term = [](const char* input, double lo, double hi, double fade) {
        Term t;
        t.input = input;
        t.lo = lo;
        t.hi = hi;
        t.fade = fade;
        return t;
    };
    constexpr double inf = 1e30;
    CoverSpec s;
    CoverRule rocks;
    rocks.name = "rocks";
    rocks.items = {"rock"};
    rocks.density = 0.02;
    rocks.scaleMin = 0.6;
    rocks.scaleMax = 1.6;
    rocks.align = 0.7;
    rocks.sink = 10.0;
    if (h.findMask("deposit"))
        rocks.terms = {term("deposit", 0.25, inf, 0.15), term("slope", -inf, 45.0, 5.0)};
    else
        rocks.terms = {term("slope", 25.0, 45.0, 5.0)};

    CoverRule trees;
    trees.name = "trees";
    trees.items = {"tree"};
    trees.density = 0.01;
    trees.footprint = 250.0;
    trees.scaleMin = 0.7;
    trees.scaleMax = 1.3;
    trees.terms = {term("slope", -inf, 30.0, 5.0)};
    if (h.findMask(biomeMask("forest")))
        trees.biomes = {"forest"};
    else if (h.findMask("moisture"))
        trees.terms.push_back(term("moisture", 0.45, inf, 0.1));
    else
        trees.terms.push_back(term("wet", 0.3, inf, 0.1));

    CoverRule debris;
    debris.name = "debris";
    debris.items = {"debris"};
    debris.density = 0.05;
    debris.scaleMin = 0.5;
    debris.scaleMax = 1.5;
    debris.align = 1.0;
    debris.terms = {term("nearCliff", 0.2, inf, 0.2), term("slope", -inf, 40.0, 5.0)};

    CoverRule grass;
    grass.name = "grass";
    grass.items = {"grassTuft"};
    grass.density = 0.2;
    grass.footprint = 20.0;
    grass.align = 1.0;
    grass.solid = false;
    if (h.findMask(layerMask("grass")))
        grass.layers = {"grass"};
    else
        grass.terms = {term("slope", -inf, 25.0, 5.0)};

    s.rules = {rocks, trees, debris, grass};
    return s;
}

const std::vector<std::string>& coverPresetNames()
{
    static const std::vector<std::string> n = {"scree", "ledgePlants", "hangingRoots"};
    return n;
}

CoverRule coverPreset(const std::string& name)
{
    auto term = [](const char* input, double lo, double hi, double fade) {
        Term t;
        t.input = input;
        t.lo = lo;
        t.hi = hi;
        t.fade = fade;
        return t;
    };
    constexpr double inf = 1e30;
    CoverRule r;
    r.name = name;
    if (name == "scree")
    {
        // The talus band terrainCliffMesh writes below each cliff, on slopes scree can rest on.
        r.items = {"scree"};
        r.density = 0.3;
        r.scaleMin = 0.4;
        r.scaleMax = 1.2;
        r.align = 0.8;
        r.sink = 5.0;
        r.terms = {term("cliffTalus", 0.15, inf, 0.1), term("slope", 15.0, 42.0, 5.0)};
    }
    else if (name == "ledgePlants")
    {
        r.items = {"ledgePlant"};
        r.density = 0.15;
        r.scaleMin = 0.6;
        r.scaleMax = 1.3;
        r.align = 1.0;
        r.solid = false;
        r.terms = {term("nearCliff", 0.3, inf, 0.15), term("slope", -inf, 30.0, 5.0)};
    }
    else if (name == "hangingRoots")
    {
        r.items = {"hangingRoot"};
        r.density = 0.05;
        r.scaleMin = 0.7;
        r.scaleMax = 1.4;
        r.align = 0.3;
        r.turn = 15.0;
        r.faceDownhill = true;
        r.terms = {term("cliffTop", 0.2, inf, 0.1), term("slope", 30.0, inf, 5.0)};
    }
    else
    {
        std::string all;
        for (const std::string& n : coverPresetNames())
            all += (all.empty() ? "" : ", ") + n;
        throw std::runtime_error("no ground cover preset " + name + " (" + all + ")");
    }
    return r;
}

CoverResult scatterCover(const Heightfield& h, const CoverSpec& s, const std::vector<std::vector<double>>& itemRadius)
{
    LILA_ZONE();

    CoverResult out;
    out.rules.resize(s.rules.size());
    if (h.empty() || s.rules.empty())
        return out;
    SurfaceFields fields(h);
    std::set<std::string> missing;
    std::vector<CoverInputs> inputs(s.rules.size());
    double maxRadius = 1.0;
    for (size_t r = 0; r < s.rules.size(); ++r)
    {
        const CoverRule& rule = s.rules[r];
        CoverInputs& in = inputs[r];
        for (const Term& t : rule.terms)
        {
            const std::vector<float>* g = t.blur > 0.0 ? fields.blurred(t.input, t.blur) : fields.get(t.input);
            if (!g)
            {
                missing.insert(t.input);
                in.dead = in.dead || !t.invert;
            }
            in.terms.push_back({&t, g});
        }
        auto weights = [&](const std::vector<std::string>& names, std::string (*mask)(const std::string&)) {
            for (const std::string& n : names)
                if (const std::vector<float>* g = fields.get(mask(n)))
                    in.weights.push_back(g);
                else
                    missing.insert(mask(n));
        };
        weights(rule.biomes, &biomeMask);
        weights(rule.layers, &layerMask);
        if (in.weights.empty() && (!rule.biomes.empty() || !rule.layers.empty()))
            in.dead = true;
        for (size_t i = 0; i < rule.items.size(); ++i)
        {
            const double own = r < itemRadius.size() && i < itemRadius[r].size() ? itemRadius[r][i] : 50.0;
            maxRadius = std::max(maxRadius, (rule.footprint > 0.0 ? rule.footprint : own) * rule.scaleMax);
        }
    }
    out.missing.assign(missing.begin(), missing.end());

    const glm::dvec2 lo = h.origin, e = h.extent();
    // Candidates per rule on a jittered grid, row by row; each kept by its fit, in a fixed order.
    std::vector<std::vector<Candidate>> candidates(s.rules.size());
    double expected = 0.0;
    for (size_t r = 0; r < s.rules.size(); ++r)
    {
        const CoverRule& rule = s.rules[r];
        const double density = rule.density * s.densityScale;
        if (density <= 0.0 || inputs[r].dead)
            continue;
        const double spacing = 100.0 / std::sqrt(density);
        const int64_t cx = int64_t(std::ceil(e.x / spacing)), cz = int64_t(std::ceil(e.y / spacing));
        if (cx * cz > 50'000'000)
            throw std::runtime_error(rule.name + ": " + std::to_string(cx * cz) + " candidates; lower its density");
        std::vector<std::vector<Candidate>> rows(static_cast<size_t>(cz));
        const uint32_t ruleSeed = coverHash(s.seed, uint32_t(r), 0x51ed27u);
        tbb::parallel_for(int64_t(0), cz, [&](int64_t j) {
            for (int64_t i = 0; i < cx; ++i)
            {
                const uint32_t hsh = coverHash(uint32_t(i), uint32_t(j), ruleSeed);
                const glm::dvec2 p = lo + glm::dvec2((double(i) + coverUnit(hsh)) * spacing, (double(j) + coverUnit(coverHash(hsh, 1u, 7u))) * spacing);
                if (p.x > lo.x + e.x || p.y > lo.y + e.y)
                    continue;
                const double fit = inputs[r].fit(h, p.x, p.y);
                if (fit >= rule.minFit && fit > 0.0)
                    rows[size_t(j)].push_back({p, fit, hsh, int32_t(i), int32_t(j)});
            }
        });
        for (std::vector<Candidate>& row : rows)
            for (const Candidate& c : row)
            {
                expected += c.fit;
                candidates[r].push_back(c);
            }
        out.rules[r].candidates = candidates[r].size();
    }
    if (s.maxCount > 0 && expected > double(s.maxCount))
        out.thinned = double(s.maxCount) / expected;
    else if (s.maxCount == 0)
        out.thinned = 0.0;

    FootprintGrid solid(maxRadius * 2.0);
    for (size_t r = 0; r < s.rules.size(); ++r)
    {
        const CoverRule& rule = s.rules[r];
        FootprintGrid own(maxRadius * 2.0);
        std::vector<uint32_t> mine;
        double fitSum = 0.0;
        for (const Candidate& c : candidates[r])
        {
            if (coverUnit(coverHash(c.hash, 2u, 11u)) >= c.fit * out.thinned)
                continue;
            const int item = int(coverHash(c.hash, 3u, 13u) % uint32_t(rule.items.size()));
            const double rnd = coverUnit(coverHash(c.hash, 4u, 17u));
            const double scale = rule.scaleMin + (rule.scaleMax - rule.scaleMin) * rnd;
            const double ownRadius = r < itemRadius.size() && size_t(item) < itemRadius[r].size() ? itemRadius[r][size_t(item)] : 50.0;
            const double radius = (rule.footprint > 0.0 ? rule.footprint : ownRadius) * scale;
            if (!solid.clear(out.pieces, c.p, radius, maxRadius) || !own.clear(out.pieces, c.p, radius, maxRadius))
            {
                ++out.rules[r].blocked;
                continue;
            }
            CoverPiece p;
            p.position = {c.p.x, h.heightAt(c.p.x, c.p.y), c.p.y};
            p.normal = h.normalAt(c.p.x, c.p.y);
            p.slope = std::acos(std::clamp(p.normal.y, -1.0, 1.0)) * 57.29577951308232;
            if (const double hl = std::hypot(p.normal.x, p.normal.z); hl > 1e-9)
                p.downhill = {p.normal.x / hl, 0.0, p.normal.z / hl};
            p.scale = scale;
            p.turn = (coverUnit(coverHash(c.hash, 5u, 19u)) * 2.0 - 1.0) * rule.turn / 57.29577951308232;
            // Facing downhill: the turn that brings +Z onto the downhill direction, jittered by turn.
            if (rule.faceDownhill)
                p.turn += std::atan2(p.downhill.x, p.downhill.z);
            p.radius = radius;
            p.fit = float(c.fit);
            p.random = float(coverUnit(coverHash(c.hash, 6u, 23u)));
            p.rule = int(r);
            p.item = item;
            p.cellX = c.i;
            p.cellZ = c.j;
            const uint32_t index = uint32_t(out.pieces.size());
            out.pieces.push_back(p);
            own.add(c.p, index);
            if (rule.solid)
                solid.add(c.p, index);
            fitSum += c.fit;
            ++out.rules[r].placed;
        }
        out.rules[r].meanFit = out.rules[r].placed ? fitSum / double(out.rules[r].placed) : 0.0;
    }
    return out;
}
} // namespace terrain
