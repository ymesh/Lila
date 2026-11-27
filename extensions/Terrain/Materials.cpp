// Terrain surfaces: layer weights from rules over masks and derived fields, biomes from climate, and the images engines
// and texture graphs read them from (splats, colour, masks laid over the mesh's uvs).

#include "Materials.h"

#include "Analysis.h"
#include "Shaping.h"

#include <lila/ImageIO.h>
#include <lila/Profile.h>

#include <tbb/parallel_for.h>

#include <algorithm>
#include <cmath>
#include <limits>
#include <numeric>
#include <set>
#include <stdexcept>

using namespace lila;

namespace terrain
{
using json = nlohmann::json;

namespace
{
constexpr double kMatDeg = 57.29577951308232;
constexpr double kMatInf = 1e30;

double matSmooth(double x)
{
    x = std::clamp(x, 0.0, 1.0);
    return x * x * (3.0 - 2.0 * x);
}

uint32_t matNameSeed(const std::string& s)
{
    uint32_t h = 2166136261u;
    for (unsigned char c : s)
        h = (h ^ c) * 16777619u;
    return h;
}

/** Three octaves of gradient noise, about -1 to 1. */
double matBreakupNoise(double x, double z, uint32_t seed)
{
    double sum = 0.0, amp = 1.0, norm = 0.0, f = 1.0;
    for (int o = 0; o < 3; ++o)
    {
        sum += amp * gradientNoise(x * f + 17.3 * o, z * f - 9.1 * o, seed + uint32_t(o) * 0x9e3779b9u);
        norm += amp;
        amp *= 0.5;
        f *= 2.0;
    }
    return sum / norm;
}

double matDefaultFade(const std::string& input)
{
    if (input == "slope" || input == "aspect")
        return 5.0;
    if (input == "height")
        return 500.0;
    if (input == "temperature")
        return 2.0;
    if (input == "curvature")
        return 0.05;
    return 0.08;
}

bool matValidName(const std::string& s)
{
    if (s.empty() || !std::isalpha(static_cast<unsigned char>(s[0])))
        return false;
    return std::all_of(s.begin(), s.end(), [](char c) { return std::isalnum(static_cast<unsigned char>(c)) || c == '_'; });
}

double matBound(const json& v, double fallback)
{
    return v.is_number() ? v.get<double>() : fallback;
}

Term termFromJson(const std::string& input, const json& v)
{
    Term t;
    t.input = input;
    t.fade = matDefaultFade(input);
    if (v.is_array())
    {
        if (v.size() != 2)
            throw std::runtime_error(input + ": a range is [min, max]");
        t.lo = matBound(v[0], -kMatInf);
        t.hi = matBound(v[1], kMatInf);
    }
    else if (v.is_object())
    {
        if (v.contains("range"))
        {
            const json& r = v["range"];
            if (!r.is_array() || r.size() != 2)
                throw std::runtime_error(input + ": range is [min, max]");
            t.lo = matBound(r[0], -kMatInf);
            t.hi = matBound(r[1], kMatInf);
        }
        t.lo = matBound(v.value("min", json(nullptr)), t.lo);
        t.hi = matBound(v.value("max", json(nullptr)), t.hi);
        t.lo = matBound(v.value("above", json(nullptr)), t.lo);
        t.hi = matBound(v.value("below", json(nullptr)), t.hi);
        t.fade = std::max(0.0, v.value("fade", t.fade));
        t.invert = v.value("invert", false);
        t.blur = std::max(0.0, v.value("blur", 0.0));
    }
    else if (v.is_number())
        t.lo = v.get<double>();
    else
        throw std::runtime_error(input + ": give [min, max], {range, fade, invert} or a minimum");
    if (t.lo > t.hi)
        throw std::runtime_error(input + ": min is above max");
    return t;
}

void matReadNames(const json& v, std::vector<std::string>& out, const std::string& owner, const char* what)
{
    if (v.is_string())
        out.push_back(v.get<std::string>());
    else if (v.is_array())
        for (const json& b : v)
            out.push_back(b.get<std::string>());
    else
        throw std::runtime_error(owner + ": " + what + " is a name or a list of names");
}
} // namespace

void readWhen(const json& when, std::vector<Term>& all, std::vector<std::vector<Term>>& any, std::vector<std::string>& biomes, const std::string& owner,
              std::vector<std::string>* layers)
{
    auto group = [&](const json& g, std::vector<Term>& out) {
        if (!g.is_object())
            throw std::runtime_error(owner + ": when is {input: [min, max], ...} or a list of those");
        for (const auto& [k, v] : g.items())
        {
            if (k == "biome")
                matReadNames(v, biomes, owner, "biome");
            else if (k == "layer" && layers)
                matReadNames(v, *layers, owner, "layer");
            else
                out.push_back(termFromJson(k, v));
        }
    };
    if (when.is_null())
        return;
    if (when.is_array())
        for (const json& g : when)
        {
            any.emplace_back();
            group(g, any.back());
        }
    else
        group(when, all);
}

namespace
{

json matTermsJson(const std::vector<Term>& terms)
{
    json j = json::object();
    for (const Term& t : terms)
        j[t.input] = t.toJson();
    return j;
}

/** Bilinear value of a grid at a fractional sample position. */
double matBilinear(const std::vector<float>& f, int nx, int nz, double fx, double fz)
{
    fx = std::clamp(fx, 0.0, double(nx - 1));
    fz = std::clamp(fz, 0.0, double(nz - 1));
    const int x0 = std::min(int(fx), nx - 2), z0 = std::min(int(fz), nz - 2);
    const double tx = fx - x0, tz = fz - z0;
    auto v = [&](int a, int b) { return double(f[size_t(b) * size_t(nx) + size_t(a)]); };
    return (v(x0, z0) * (1 - tx) + v(x0 + 1, z0) * tx) * (1 - tz) + (v(x0, z0 + 1) * (1 - tx) + v(x0 + 1, z0 + 1) * tx) * tz;
}

struct MatGrad
{
    double gx, gz;
};

MatGrad matGradAt(const Heightfield& h, int x, int z)
{
    const int x0 = std::max(x - 1, 0), x1 = std::min(x + 1, h.nx - 1), z0 = std::max(z - 1, 0), z1 = std::min(z + 1, h.nz - 1);
    return {(h.at(x1, z) - h.at(x0, z)) / (h.cell * std::max(1, x1 - x0)), (h.at(x, z1) - h.at(x, z0)) / (h.cell * std::max(1, z1 - z0))};
}

/** Row sums in parallel, added in row order, so the total is the same at any thread count. */
template <class F> double matGridSum(const Heightfield& h, F value)
{
    std::vector<double> rows(size_t(h.nz), 0.0);
    tbb::parallel_for(0, h.nz, [&](int z) {
        double s = 0.0;
        for (int x = 0; x < h.nx; ++x)
            s += value(h.index(x, z));
        rows[size_t(z)] = s;
    });
    return std::accumulate(rows.begin(), rows.end(), 0.0);
}

struct MatNamedMaterial
{
    const char* layer;
    const char* material;
};

const MatNamedMaterial kLayerMaterials[] = {
    {"grass", "meadowGrass"}, {"meadow", "meadowGrass"}, {"rock", "sunlitStone"},   {"cliff", "sunlitStone"}, {"stone", "sunlitStone"},
    {"dirt", "dirtPath"},     {"road", "dirtPath"},      {"path", "dirtPath"},      {"gravel", "riverRock"},  {"scree", "riverRock"},
    {"riverbed", "riverRock"}, {"moss", "meadowMoss"},   {"forest", "meadowMoss"},
};

struct MatNamedColor
{
    const char* name;
    glm::dvec3 color;
};

const MatNamedColor kLayerColors[] = {
    {"grass", {0.30, 0.42, 0.16}},  {"meadow", {0.36, 0.45, 0.18}}, {"rock", {0.42, 0.40, 0.37}},  {"cliff", {0.40, 0.38, 0.35}},
    {"stone", {0.45, 0.43, 0.40}},  {"dirt", {0.36, 0.27, 0.18}},   {"road", {0.40, 0.33, 0.24}},  {"path", {0.42, 0.34, 0.24}},
    {"gravel", {0.50, 0.47, 0.42}}, {"scree", {0.48, 0.45, 0.41}},  {"riverbed", {0.33, 0.31, 0.27}}, {"sand", {0.76, 0.68, 0.50}},
    {"snow", {0.92, 0.94, 0.97}},   {"moss", {0.25, 0.36, 0.14}},   {"forest", {0.20, 0.27, 0.12}}, {"mud", {0.27, 0.21, 0.15}},
};
} // namespace

double Term::eval(double v) const
{
    double t;
    if (v >= lo && v <= hi)
        t = 1.0;
    else if (fade <= 0.0)
        t = 0.0;
    else
        t = matSmooth(1.0 - (v < lo ? lo - v : v - hi) / fade);
    return invert ? 1.0 - t : t;
}

json Term::toJson() const
{
    json r = json::array({lo <= -kMatInf ? json(nullptr) : json(lo), hi >= kMatInf ? json(nullptr) : json(hi)});
    json j = {{"range", r}, {"fade", fade}};
    if (invert)
        j["invert"] = true;
    if (blur > 0.0)
        j["blur"] = blur;
    return j;
}

std::string layerMask(const std::string& layer) { return "layer_" + layer; }
std::string biomeMask(const std::string& biome) { return "biome_" + biome; }

const std::vector<std::string>& surfaceFieldNames()
{
    static const std::vector<std::string> n = {"height", "slope", "aspect", "curvature", "nearCliff", "temperature"};
    return n;
}

// ---------------------------------------------------------------------------------------------------- rules JSON

json MaterialRules::toJson() const
{
    json layersJ = json::array();
    for (const LayerRule& l : layers)
    {
        json j = {{"name", l.name}, {"material", l.material}, {"roughness", l.roughness}, {"opacity", l.opacity}};
        if (l.colorGiven)
            j["color"] = json::array({l.color.x, l.color.y, l.color.z});
        if (l.tileCm > 0.0)
            j["tileSize"] = l.tileCm;
        json when = matTermsJson(l.terms);
        if (!l.biomes.empty())
            when["biome"] = l.biomes;
        if (!when.empty())
            j["when"] = when;
        if (l.noise.amount > 0.0)
            j["noise"] = {{"amount", l.noise.amount}, {"scale", l.noise.scale}};
        layersJ.push_back(j);
    }
    return {{"seed", seed}, {"layers", layersJ}};
}

MaterialRules MaterialRules::fromJson(const json& j)
{
    MaterialRules r;
    const json* list = &j;
    if (j.is_object())
    {
        r.seed = j.value("seed", 0u);
        if (!j.contains("layers"))
            throw std::runtime_error("rules need layers: a list, bottom first");
        list = &j["layers"];
    }
    if (!list->is_array() || list->empty())
        throw std::runtime_error("layers is a list of {name, material, when, ...}, bottom first");
    std::set<std::string> names;
    for (const json& lj : *list)
    {
        if (!lj.is_object())
            throw std::runtime_error("each layer is {name, material, when, ...}");
        LayerRule l;
        l.name = lj.value("name", std::string());
        if (!matValidName(l.name))
            throw std::runtime_error("layer name '" + l.name + "' must start with a letter and use letters, digits and _");
        if (!names.insert(l.name).second)
            throw std::runtime_error("two layers are named " + l.name);
        l.material = lj.contains("material") ? (lj["material"].is_string() ? lj["material"].get<std::string>() : std::string()) : defaultMaterialFor(l.name);
        l.color = defaultColorFor(l.name);
        if (lj.contains("color"))
        {
            const json& c = lj["color"];
            if (!c.is_array() || c.size() != 3)
                throw std::runtime_error(l.name + ": color is [r, g, b], 0 to 1");
            l.color = {c[0].get<double>(), c[1].get<double>(), c[2].get<double>()};
            l.colorGiven = true;
        }
        l.roughness = std::clamp(lj.value("roughness", l.roughness), 0.0, 1.0);
        l.tileCm = std::max(0.0, lj.value("tileSize", 0.0));
        l.opacity = std::clamp(lj.value("opacity", 1.0), 0.0, 1.0);
        std::vector<std::vector<Term>> any;
        readWhen(lj.value("when", json()), l.terms, any, l.biomes, l.name);
        if (!any.empty())
            throw std::runtime_error(l.name + ": when is one {input: range} object; use another layer for an alternative");
        if (lj.contains("noise"))
        {
            const json& n = lj["noise"];
            if (n.is_number())
                l.noise.amount = n.get<double>();
            else if (n.is_object())
            {
                l.noise.amount = n.value("amount", 0.0);
                l.noise.scale = std::max(1.0, n.value("scale", l.noise.scale));
            }
        }
        l.noise.amount = std::clamp(l.noise.amount, 0.0, 2.0);
        r.layers.push_back(std::move(l));
    }
    return r;
}

std::string defaultMaterialFor(const std::string& layer)
{
    for (const MatNamedMaterial& m : kLayerMaterials)
        if (layer == m.layer)
            return m.material;
    return {};
}

glm::dvec3 defaultColorFor(const std::string& layer)
{
    // The table is in display (sRGB) values; layer colours are linear.
    for (const MatNamedColor& c : kLayerColors)
        if (layer == c.name)
            return glm::pow(c.color, glm::dvec3(2.2));
    // Any other layer: a muted colour of its own.
    const double hue = double(matNameSeed(layer) % 360u) / 60.0;
    const double x = 1.0 - std::abs(std::fmod(hue, 2.0) - 1.0);
    glm::dvec3 rgb = hue < 1 ? glm::dvec3(1, x, 0) : hue < 2 ? glm::dvec3(x, 1, 0) : hue < 3 ? glm::dvec3(0, 1, x) : hue < 4 ? glm::dvec3(0, x, 1) : hue < 5 ? glm::dvec3(x, 0, 1) : glm::dvec3(1, 0, x);
    return glm::pow(0.25 + rgb * 0.3, glm::dvec3(2.2));
}

std::optional<glm::dvec3> knownMaterialColor(const std::string& id)
{
    // Measured from each Meadowlight graph's baseColor; running a graph inside a command would compile it first.
    static const std::pair<const char*, glm::dvec3> known[] = {
        {"meadowGrass", {0.087, 0.246, 0.069}},
        {"dirtPath", {0.394, 0.198, 0.084}},
        {"riverRock", {0.138, 0.157, 0.169}},
        {"sunlitStone", {0.516, 0.415, 0.299}},
    };
    const std::string tail = id.substr(id.find_last_of('/') == std::string::npos ? 0 : id.find_last_of('/') + 1);
    for (const auto& [name, c] : known)
        if (tail == name)
            return c;
    return std::nullopt;
}

// ---------------------------------------------------------------------------------------------------- fields

SurfaceFields::SurfaceFields(const Heightfield& h) : mH(h) {}

const std::vector<float>* SurfaceFields::get(const std::string& name)
{
    LILA_ZONE();

    const Heightfield& h = mH;
    if (name == "height")
        return &h.heights;
    if (auto it = mDerived.find(name); it != mDerived.end())
        return &it->second;
    std::vector<float> out(h.size(), 0.0f);
    auto each = [&](auto fn) {
        tbb::parallel_for(0, h.nz, [&](int z) {
            for (int x = 0; x < h.nx; ++x)
                out[h.index(x, z)] = float(fn(x, z, h.index(x, z)));
        });
    };
    if (name == "slope")
        each([&](int x, int z, size_t) {
            const MatGrad g = matGradAt(h, x, z);
            return std::atan(std::hypot(g.gx, g.gz)) * kMatDeg;
        });
    else if (name == "aspect")
        each([&](int x, int z, size_t) {
            const MatGrad g = matGradAt(h, x, z);
            if (std::hypot(g.gx, g.gz) < 1e-9)
                return -1.0;
            const double a = std::atan2(-g.gx, g.gz) * kMatDeg;
            return a < 0.0 ? a + 360.0 : a;
        });
    else if (name == "curvature")
        each([&](int x, int z, size_t i) {
            const int x0 = std::max(x - 1, 0), x1 = std::min(x + 1, h.nx - 1), z0 = std::max(z - 1, 0), z1 = std::min(z + 1, h.nz - 1);
            return -(h.at(x0, z) + h.at(x1, z) + h.at(x, z0) + h.at(x, z1) - 4.0 * h.heights[i]) / (h.cell * h.cell) * 100.0;
        });
    else if (name == "temperature")
    {
        const std::vector<float>* m = h.findMask("temperature");
        if (!m || m->size() != h.size())
            return nullptr;
        each([&](int, int, size_t i) { return maskToTemperature((*m)[i]); });
    }
    else if (name == "nearCliff")
    {
        // Ground within a few tens of metres of a cliff (slope over 45 degrees), not the cliff itself.
        const std::vector<float>& slope = *get("slope");
        const int r = std::clamp(int(std::lround(3000.0 / h.cell)), 1, 64);
        std::vector<float> cliff(h.size()), pass(h.size());
        for (size_t i = 0; i < cliff.size(); ++i)
            cliff[i] = slope[i] > 45.0f ? 1.0f : 0.0f;
        tbb::parallel_for(0, h.nz, [&](int z) {
            for (int x = 0; x < h.nx; ++x)
            {
                float m = 0.0f;
                for (int d = -r; d <= r; ++d)
                    m = std::max(m, cliff[h.index(std::clamp(x + d, 0, h.nx - 1), z)]);
                pass[h.index(x, z)] = m;
            }
        });
        tbb::parallel_for(0, h.nz, [&](int z) {
            for (int x = 0; x < h.nx; ++x)
            {
                float m = 0.0f;
                for (int d = -r; d <= r; ++d)
                    m = std::max(m, pass[h.index(x, std::clamp(z + d, 0, h.nz - 1))]);
                out[h.index(x, z)] = m * (1.0f - cliff[h.index(x, z)]);
            }
        });
    }
    else if (const std::vector<float>* m = h.findMask(name); m && m->size() == h.size())
        return m;
    else if (std::find(Analysis::fieldNames().begin(), Analysis::fieldNames().end(), name) != Analysis::fieldNames().end())
    {
        if (!mShared)
            mShared = std::make_shared<const Heightfield>(h);
        const std::vector<float>& f = analyse(mShared)->field(name);
        if (f.size() != h.size())
            return nullptr;
        out = f;
    }
    else
        return nullptr;
    return &mDerived.emplace(name, std::move(out)).first->second;
}

const std::vector<float>* SurfaceFields::blurred(const std::string& name, double radius)
{
    const std::vector<float>* v = get(name);
    const int r = int(std::lround(radius / mH.cell));
    if (!v || r < 1)
        return v;
    const std::string key = name + "#blur" + std::to_string(r);
    if (auto it = mDerived.find(key); it != mDerived.end())
        return &it->second;
    return &mDerived.emplace(key, boxBlurred(*v, mH.nx, mH.nz, r, 3)).first->second;
}

// ---------------------------------------------------------------------------------------------------- layer weights

namespace
{
/** A rule's inputs resolved to grids; null terms read 0. */
struct ResolvedRule
{
    std::vector<std::pair<const Term*, const std::vector<float>*>> terms;
    std::vector<const std::vector<float>*> biomes;
    bool dead = false;
};

ResolvedRule resolveRule(SurfaceFields& f, const std::vector<Term>& terms, const std::vector<std::string>& biomes, std::set<std::string>& missing)
{
    ResolvedRule r;
    for (const Term& t : terms)
    {
        const std::vector<float>* g = t.blur > 0.0 ? f.blurred(t.input, t.blur) : f.get(t.input);
        if (!g)
        {
            missing.insert(t.input);
            if (!t.invert)
                r.dead = true;
        }
        r.terms.push_back({&t, g});
    }
    for (const std::string& b : biomes)
    {
        const std::vector<float>* g = f.get(biomeMask(b));
        if (!g)
            missing.insert(biomeMask(b));
        else
            r.biomes.push_back(g);
    }
    if (!biomes.empty() && r.biomes.empty())
        r.dead = true;
    return r;
}

double ruleAt(const ResolvedRule& r, size_t k)
{
    if (r.dead)
        return 0.0;
    double a = 1.0;
    for (const auto& [t, g] : r.terms)
        a *= t->eval(g ? double((*g)[k]) : 0.0);
    if (!r.biomes.empty())
    {
        double b = 0.0;
        for (const std::vector<float>* g : r.biomes)
            b += (*g)[k];
        a *= std::clamp(b, 0.0, 1.0);
    }
    return a;
}

void eraseMasksWithPrefix(Heightfield& h, const std::string& prefix)
{
    for (auto it = h.masks.begin(); it != h.masks.end();)
        it = it->first.starts_with(prefix) ? h.masks.erase(it) : std::next(it);
}

double weightSumError(const Heightfield& h, const std::vector<std::string>& masks)
{
    std::vector<const std::vector<float>*> m;
    for (const std::string& n : masks)
        m.push_back(h.findMask(n));
    std::vector<double> rows(size_t(h.nz), 0.0);
    tbb::parallel_for(0, h.nz, [&](int z) {
        double worst = 0.0;
        for (int x = 0; x < h.nx; ++x)
        {
            double s = 0.0;
            for (const auto* g : m)
                s += (*g)[h.index(x, z)];
            worst = std::max(worst, std::abs(s - 1.0));
        }
        rows[size_t(z)] = worst;
    });
    return *std::max_element(rows.begin(), rows.end());
}
} // namespace

RulesResult applyMaterialRules(Heightfield& h, const MaterialRules& r)
{
    LILA_ZONE();

    RulesResult out;
    if (h.empty() || r.layers.empty())
        return out;
    eraseMasksWithPrefix(h, "layer_");
    const size_t n = r.layers.size();
    std::set<std::string> missing;
    std::vector<ResolvedRule> rules(n);
    {
        SurfaceFields f(h);
        for (size_t i = 1; i < n; ++i)
            rules[i] = resolveRule(f, r.layers[i].terms, r.layers[i].biomes, missing);
        std::vector<std::vector<float>> w(n, std::vector<float>(h.size(), 0.0f));
        tbb::parallel_for(0, h.nz, [&](int z) {
            for (int x = 0; x < h.nx; ++x)
            {
                const size_t k = h.index(x, z);
                const glm::dvec2 p = h.samplePos(x, z);
                double remaining = 1.0;
                for (size_t i = n; i-- > 1;)
                {
                    const LayerRule& l = r.layers[i];
                    double a = l.opacity * ruleAt(rules[i], k);
                    // Noise breaks up the edges of where the rule holds; it never lays the layer where the rule is off.
                    if (l.noise.amount > 0.0 && a > 0.0)
                        a = matSmooth(a + l.noise.amount * matBreakupNoise(p.x / l.noise.scale, p.y / l.noise.scale, r.seed ^ matNameSeed(l.name)));
                    const double wi = a * remaining;
                    remaining -= wi;
                    w[i][k] = float(wi);
                }
                w[0][k] = float(std::max(0.0, remaining));
            }
        });
        for (size_t i = 0; i < n; ++i)
            h.masks[layerMask(r.layers[i].name)] = std::move(w[i]);
    }
    std::vector<std::string> names, masks;
    for (const LayerRule& l : r.layers)
    {
        names.push_back(l.name);
        masks.push_back(layerMask(l.name));
    }
    out.layers = coverageOf(h, names, masks);
    out.missing.assign(missing.begin(), missing.end());
    out.maxSumError = weightSumError(h, masks);
    return out;
}

std::vector<Coverage> coverageOf(const Heightfield& h, const std::vector<std::string>& names, const std::vector<std::string>& masks)
{
    LILA_ZONE();

    std::vector<Coverage> out;
    std::vector<const std::vector<float>*> grids;
    for (size_t i = 0; i < masks.size(); ++i)
    {
        const std::vector<float>* g = h.findMask(masks[i]);
        grids.push_back(g && g->size() == h.size() ? g : nullptr);
    }
    const double samples = double(h.size());
    const glm::dvec2 e = h.extent();
    const double areaM2 = e.x * e.y / 10000.0;
    // Dominance: which mask is largest at each sample (the first wins a tie).
    std::vector<int> top(h.size(), -1);
    tbb::parallel_for(0, h.nz, [&](int z) {
        for (int x = 0; x < h.nx; ++x)
        {
            const size_t k = h.index(x, z);
            float best = 0.0f;
            for (size_t i = 0; i < grids.size(); ++i)
                if (grids[i] && (*grids[i])[k] > best)
                {
                    best = (*grids[i])[k];
                    top[k] = int(i);
                }
        }
    });
    for (size_t i = 0; i < masks.size(); ++i)
    {
        Coverage c;
        c.name = names[i];
        c.mask = masks[i];
        if (grids[i])
        {
            c.coverage = matGridSum(h, [&](size_t k) { return double((*grids[i])[k]); }) / samples;
            c.dominant = matGridSum(h, [&](size_t k) { return top[k] == int(i) ? 1.0 : 0.0; }) / samples;
        }
        c.areaM2 = c.coverage * areaM2;
        out.push_back(c);
    }
    return out;
}

// ---------------------------------------------------------------------------------------------------- biomes

double temperatureToMask(double celsius) { return std::clamp((celsius + 30.0) / 70.0, 0.0, 1.0); }
double maskToTemperature(double m) { return m * 70.0 - 30.0; }

json BiomeSpec::toJson() const
{
    const Climate& c = climate;
    json biomesJ = json::array();
    for (const BiomeRule& b : biomes)
    {
        json j = {{"name", b.name}, {"when", matTermsJson(b.terms)}};
        if (!b.paint.empty())
            j["paint"] = b.paint;
        biomesJ.push_back(j);
    }
    json j = {{"climate",
               {{"seaLevel", c.seaLevel},
                {"temperature", c.temperature},
                {"lapseRate", c.lapseRate},
                {"aspectEffect", c.aspectEffect},
                {"sunAzimuth", c.sunAzimuth},
                {"rain", c.rain},
                {"windAzimuth", c.windAzimuth},
                {"rainShadow", c.rainShadow},
                {"shadowDistance", c.shadowDistance},
                {"orographic", c.orographic},
                {"groundWater", c.groundWater}}},
              {"biomes", biomesJ},
              {"sharpness", sharpness}};
    if (!landcover.empty())
    {
        json classes = json::object();
        for (const auto& [b, v] : landcoverClasses)
            classes[b] = v;
        j["landcover"] = {{"mask", landcover}, {"scale", landcoverScale}, {"weight", landcoverWeight}, {"classes", classes}};
    }
    return j;
}

BiomeSpec BiomeSpec::fromJson(const json& j)
{
    BiomeSpec s = defaultBiomes();
    if (!j.is_object())
        throw std::runtime_error("biomes are {climate, biomes, sharpness, landcover}");
    if (j.contains("climate"))
    {
        const json& c = j["climate"];
        if (!c.is_object())
            throw std::runtime_error("climate is an object");
        Climate& m = s.climate;
        m.seaLevel = c.value("seaLevel", m.seaLevel);
        m.temperature = c.value("temperature", m.temperature);
        m.lapseRate = c.value("lapseRate", m.lapseRate);
        m.aspectEffect = c.value("aspectEffect", m.aspectEffect);
        m.sunAzimuth = c.value("sunAzimuth", m.sunAzimuth);
        m.rain = std::clamp(c.value("rain", m.rain), 0.0, 1.0);
        m.windAzimuth = c.value("windAzimuth", m.windAzimuth);
        m.rainShadow = std::clamp(c.value("rainShadow", m.rainShadow), 0.0, 1.0);
        m.shadowDistance = std::max(0.0, c.value("shadowDistance", m.shadowDistance));
        m.orographic = std::max(0.0, c.value("orographic", m.orographic));
        m.groundWater = std::clamp(c.value("groundWater", m.groundWater), 0.0, 1.0);
    }
    s.sharpness = std::clamp(j.value("sharpness", s.sharpness), 0.1, 64.0);
    if (j.contains("biomes"))
    {
        const json& list = j["biomes"];
        if (!list.is_array() || list.empty())
            throw std::runtime_error("biomes is a list of {name, when, paint}");
        s.biomes.clear();
        std::set<std::string> names;
        for (const json& bj : list)
        {
            BiomeRule b;
            b.name = bj.value("name", std::string());
            if (!matValidName(b.name))
                throw std::runtime_error("biome name '" + b.name + "' must start with a letter and use letters, digits and _");
            if (!names.insert(b.name).second)
                throw std::runtime_error("two biomes are named " + b.name);
            std::vector<std::vector<Term>> any;
            std::vector<std::string> nested;
            readWhen(bj.value("when", json()), b.terms, any, nested, b.name);
            if (!any.empty() || !nested.empty())
                throw std::runtime_error(b.name + ": when is one {input: range} object");
            b.paint = bj.value("paint", std::string());
            s.biomes.push_back(std::move(b));
        }
    }
    if (j.contains("landcover"))
    {
        const json& l = j["landcover"];
        s.landcover = l.value("mask", std::string());
        s.landcoverScale = std::max(1.0, l.value("scale", 255.0));
        s.landcoverWeight = std::clamp(l.value("weight", 1.0), 0.0, 1.0);
        s.landcoverClasses.clear();
        if (l.contains("classes"))
            for (const auto& [b, v] : l["classes"].items())
            {
                std::vector<int> codes;
                if (v.is_number())
                    codes.push_back(v.get<int>());
                else
                    for (const json& c : v)
                        codes.push_back(c.get<int>());
                s.landcoverClasses[b] = codes;
            }
        else
            // ESA WorldCover classes onto the default biomes.
            s.landcoverClasses = {{"forest", {10, 95}}, {"scrub", {20, 60}}, {"meadow", {30, 40, 50}}, {"snow", {70}}, {"wetland", {80, 90}}, {"alpine", {100}}};
    }
    return s;
}

BiomeSpec defaultBiomes()
{
    BiomeSpec s;
    auto t = [](const char* input, double lo, double hi, double fade) {
        Term x;
        x.input = input;
        x.lo = lo;
        x.hi = hi;
        x.fade = fade;
        return x;
    };
    s.biomes = {{"snow", {t("temperature", -kMatInf, -3.0, 2.0)}, {}},
                {"alpine", {t("temperature", -3.0, 3.0, 2.0)}, {}},
                {"forest", {t("temperature", 3.0, 24.0, 3.0), t("moisture", 0.5, 1.0, 0.1)}, {}},
                {"meadow", {t("temperature", 3.0, 26.0, 3.0), t("moisture", 0.25, 0.6, 0.1)}, {}},
                {"scrub", {t("temperature", 3.0, kMatInf, 3.0), t("moisture", -kMatInf, 0.3, 0.1)}, {}},
                {"wetland", {t("moisture", 0.85, kMatInf, 0.05), t("slope", -kMatInf, 4.0, 2.0)}, {}}};
    return s;
}

RulesResult applyBiomes(Heightfield& h, const BiomeSpec& s)
{
    LILA_ZONE();

    RulesResult out;
    if (h.empty() || s.biomes.empty())
        return out;
    eraseMasksWithPrefix(h, "biome_");
    const Climate& c = s.climate;
    const glm::dvec2 range = h.range();
    {
        SurfaceFields f(h);
        const std::vector<float>& slope = *f.get("slope");
        const std::vector<float>& aspect = *f.get("aspect");
        const std::vector<float>* wet = f.get("wet");
        const double sunAz = c.sunAzimuth / kMatDeg, windAz = c.windAzimuth / kMatDeg;
        // The wind travels along dir; the shadow looks back along it.
        const glm::dvec2 dir(std::sin(windAz), -std::cos(windAz));
        const double reach = std::min(c.shadowDistance, glm::length(h.extent()));
        const int steps = c.rainShadow > 0.0 && reach > h.cell ? std::clamp(int(reach / h.cell), 1, 96) : 0;
        const double stepLen = steps ? reach / steps : 0.0;
        const double shadowScale = std::max(1000.0, 0.5 * (range.y - range.x));
        std::vector<float> temp(h.size()), moist(h.size());
        tbb::parallel_for(0, h.nz, [&](int z) {
            for (int x = 0; x < h.nx; ++x)
            {
                const size_t k = h.index(x, z);
                const double y = h.heights[k];
                double t = c.temperature - c.lapseRate * (y - c.seaLevel) / 10000.0;
                if (aspect[k] >= 0.0f)
                    t += c.aspectEffect * std::cos(aspect[k] / kMatDeg - sunAz) * std::sin(slope[k] / kMatDeg);
                temp[k] = float(temperatureToMask(t));

                double shadow = 0.0;
                for (int sIdx = 1; sIdx <= steps; ++sIdx)
                {
                    const double fx = x - dir.x * stepLen * sIdx / h.cell, fz = z - dir.y * stepLen * sIdx / h.cell;
                    if (fx < 0.0 || fz < 0.0 || fx > h.nx - 1 || fz > h.nz - 1)
                        break;
                    shadow = std::max(shadow, (matBilinear(h.heights, h.nx, h.nz, fx, fz) - y) / shadowScale);
                }
                const MatGrad g = matGradAt(h, x, z);
                const double windward = std::clamp((g.gx * dir.x + g.gz * dir.y) / 0.5, 0.0, 1.0);
                double m = c.rain * (1.0 - c.rainShadow * std::clamp(shadow, 0.0, 1.0)) * (1.0 + c.orographic * windward);
                if (wet)
                    m += c.groundWater * (*wet)[k] * (1.0 - std::clamp(m, 0.0, 1.0));
                moist[k] = float(std::clamp(m, 0.0, 1.0));
            }
        });
        h.masks["temperature"] = std::move(temp);
        h.masks["moisture"] = std::move(moist);
    }

    const size_t n = s.biomes.size();
    std::set<std::string> missing;
    SurfaceFields f(h);
    std::vector<std::vector<std::pair<const Term*, const std::vector<float>*>>> terms(n);
    std::vector<const std::vector<float>*> paints(n, nullptr);
    for (size_t b = 0; b < n; ++b)
    {
        for (const Term& t : s.biomes[b].terms)
        {
            const std::vector<float>* g = f.get(t.input);
            if (!g)
                missing.insert(t.input);
            terms[b].push_back({&t, g});
        }
        if (!s.biomes[b].paint.empty())
        {
            paints[b] = f.get(s.biomes[b].paint);
            if (!paints[b])
                missing.insert(s.biomes[b].paint);
        }
    }
    const std::vector<float>* landcover = s.landcover.empty() ? nullptr : f.get(s.landcover);
    if (!s.landcover.empty() && !landcover)
        missing.insert(s.landcover);
    std::map<int, size_t> classBiome;
    for (const auto& [name, codes] : s.landcoverClasses)
        for (size_t b = 0; b < n; ++b)
            if (s.biomes[b].name == name)
                for (int code : codes)
                    classBiome[code] = b;

    std::vector<std::vector<float>> w(n, std::vector<float>(h.size(), 0.0f));
    tbb::parallel_for(0, h.nz, [&](int z) {
        std::vector<double> logFit(n), weight(n);
        for (int x = 0; x < h.nx; ++x)
        {
            const size_t k = h.index(x, z);
            // Fit in log space: 0 inside every range, minus how many fades away outside one, so weights never all vanish.
            double best = -kMatInf;
            for (size_t b = 0; b < n; ++b)
            {
                double l = 0.0;
                for (const auto& [t, g] : terms[b])
                {
                    const double v = g ? (*g)[k] : 0.0;
                    const double d = v < t->lo ? t->lo - v : v > t->hi ? v - t->hi : 0.0;
                    const double outside = t->invert ? (d > 0.0 ? 0.0 : 1.0) : d / std::max(1e-9, t->fade);
                    l -= outside;
                }
                logFit[b] = l * s.sharpness;
                best = std::max(best, logFit[b]);
            }
            double sum = 0.0;
            for (size_t b = 0; b < n; ++b)
                sum += weight[b] = std::exp(logFit[b] - best);
            for (size_t b = 0; b < n; ++b)
                weight[b] /= sum;
            double paintSum = 0.0;
            for (size_t b = 0; b < n; ++b)
                if (paints[b])
                    paintSum += std::clamp(double((*paints[b])[k]), 0.0, 1.0);
            if (paintSum > 0.0)
            {
                const double alpha = std::min(1.0, paintSum);
                for (size_t b = 0; b < n; ++b)
                    weight[b] = (1.0 - alpha) * weight[b] + (paints[b] ? alpha * std::clamp(double((*paints[b])[k]), 0.0, 1.0) / paintSum : 0.0);
            }
            if (landcover)
                if (auto it = classBiome.find(int(std::lround((*landcover)[k] * s.landcoverScale))); it != classBiome.end())
                    for (size_t b = 0; b < n; ++b)
                        weight[b] = (1.0 - s.landcoverWeight) * weight[b] + (b == it->second ? s.landcoverWeight : 0.0);
            for (size_t b = 0; b < n; ++b)
                w[b][k] = float(weight[b]);
        }
    });
    std::vector<std::string> names, masks;
    for (size_t b = 0; b < n; ++b)
    {
        names.push_back(s.biomes[b].name);
        masks.push_back(biomeMask(s.biomes[b].name));
        h.masks[masks.back()] = std::move(w[b]);
    }
    out.layers = coverageOf(h, names, masks);
    out.missing.assign(missing.begin(), missing.end());
    out.maxSumError = weightSumError(h, masks);
    return out;
}

// ---------------------------------------------------------------------------------------------------- default layers

/** Lowest default snow above the sea when the terrain has no temperature: 3 km, so hills and tropical ranges stay clear. */
constexpr double kDefaultSnowlineCm = 300000.0;

MaterialRules defaultMaterialRules(const Heightfield& h, std::optional<double> seaLevel)
{
    auto term = [](const char* input, double lo, double hi, double fade) {
        Term t;
        t.input = input;
        t.lo = lo;
        t.hi = hi;
        t.fade = fade;
        return t;
    };
    auto layer = [](const char* name, std::vector<Term> terms, double noise = 0.0, double scale = 3000.0) {
        LayerRule l;
        l.name = name;
        l.material = defaultMaterialFor(name);
        l.color = defaultColorFor(name);
        // A library material with no measured colour draws in the layer's own.
        l.colorGiven = !l.material.empty() && !knownMaterialColor(l.material);
        l.terms = std::move(terms);
        l.noise = {noise, scale};
        return l;
    };
    MaterialRules r;
    r.layers.push_back(layer("grass", {}));
    if (h.findMask(biomeMask("forest")))
    {
        LayerRule l = layer("forest", {}, 0.2, 6000.0);
        l.biomes = {"forest"};
        r.layers.push_back(std::move(l));
    }
    else
    {
        // Wooded hillsides over grassy flats; the coast's first metres stay open.
        std::vector<Term> where = {term("slope", 10.0, 38.0, 4.0)};
        if (seaLevel)
            where.push_back(term("height", *seaLevel + 1500.0, kMatInf, 1000.0));
        r.layers.push_back(layer("forest", std::move(where), 0.35, 6000.0));
    }
    // Erosion's masks: a terrain never eroded has neither.
    if (h.findMask("wear"))
        r.layers.push_back(layer("dirt", {term("wear", 0.25, kMatInf, 0.15)}, 0.25, 4000.0));
    if (h.findMask("deposit"))
        r.layers.push_back(layer("gravel", {term("deposit", 0.3, kMatInf, 0.15)}, 0.2, 2500.0));
    if (seaLevel)
        r.layers.push_back(layer("sand", {term("height", -kMatInf, *seaLevel + 300.0, 200.0), term("slope", -kMatInf, 15.0, 5.0)}, 0.15, 3000.0));
    r.layers.push_back(layer("riverbed", {term("flow", 0.82, kMatInf, 0.04)}));
    r.layers.push_back(layer("rock", {term("slope", 38.0, kMatInf, 6.0)}, 0.2, 2500.0));
    const glm::dvec2 range = h.range();
    const double span = std::max(1.0, range.y - range.x);
    if (h.findMask("temperature"))
        r.layers.push_back(layer("snow", {term("temperature", -kMatInf, -2.0, 2.0), term("slope", -kMatInf, 50.0, 5.0)}, 0.2, 5000.0));
    else if (seaLevel)
    {
        // Measured up from the sea, not the seabed, and never below a snowline: low coasts and hills stay clear.
        const double land = std::max(0.0, range.y - *seaLevel);
        const double line = *seaLevel + std::max(kDefaultSnowlineCm, 0.82 * land);
        r.layers.push_back(layer("snow", {term("height", line, kMatInf, std::max(2000.0, 0.04 * land)), term("slope", -kMatInf, 50.0, 5.0)}, 0.2, 5000.0));
    }
    else
        r.layers.push_back(layer("snow", {term("height", range.x + 0.82 * span, kMatInf, 0.04 * span), term("slope", -kMatInf, 50.0, 5.0)}, 0.2, 5000.0));
    for (const char* road : {"road", "roads", "roadSurface", "path", "paths"})
        if (h.findMask(road))
        {
            LayerRule l = layer("road", {term(road, 0.5, kMatInf, 0.2)});
            r.layers.push_back(l);
            break;
        }
    return r;
}

// ---------------------------------------------------------------------------------------------------- images

std::vector<std::vector<uint8_t>> packSplats(const Heightfield& h, const std::vector<std::string>& masks)
{
    LILA_ZONE();

    const size_t n = masks.size(), images = (n + 3) / 4;
    std::vector<const std::vector<float>*> g;
    for (const std::string& m : masks)
    {
        const std::vector<float>* v = h.findMask(m);
        if (!v || v->size() != h.size())
            throw std::runtime_error("no mask " + m);
        g.push_back(v);
    }
    std::vector<std::vector<uint8_t>> out(images, std::vector<uint8_t>(h.size() * 4, 0));
    tbb::parallel_for(0, h.nz, [&](int z) {
        std::vector<double> v(n);
        std::vector<int> q(n);
        std::vector<size_t> order(n);
        for (int x = 0; x < h.nx; ++x)
        {
            const size_t k = h.index(x, z);
            double sum = 0.0;
            for (size_t i = 0; i < n; ++i)
                sum += v[i] = std::clamp(double((*g[i])[k]), 0.0, 1.0);
            int used = 0;
            if (sum <= 0.0)
            {
                std::fill(q.begin(), q.end(), 0);
                if (n)
                    q[0] = 255;
                used = 255;
            }
            else
                for (size_t i = 0; i < n; ++i)
                {
                    v[i] = v[i] * 255.0 / sum;
                    q[i] = int(std::floor(v[i]));
                    used += q[i];
                }
            // Largest remainder: the bytes left go to the largest fractions, lower index first on a tie.
            std::iota(order.begin(), order.end(), size_t(0));
            std::stable_sort(order.begin(), order.end(), [&](size_t a, size_t b) { return v[a] - q[a] > v[b] - q[b]; });
            for (size_t j = 0; used < 255 && j < n; ++j, ++used)
                ++q[order[j]];
            for (size_t i = 0; i < n; ++i)
                out[i / 4][k * 4 + i % 4] = uint8_t(q[i]);
        }
    });
    return out;
}

json writeSplats(const Heightfield& h, const std::vector<std::string>& masks, const std::filesystem::path& folder, const std::string& name)
{
    LILA_ZONE();

    json out = json::array();
    if (masks.empty())
        return out;
    const std::vector<std::vector<uint8_t>> splats = packSplats(h, masks);
    std::filesystem::create_directories(folder);
    for (size_t s = 0; s < splats.size(); ++s)
    {
        image::Image img{h.nx, h.nz, std::vector<float>(h.size() * 4)};
        for (size_t i = 0; i < img.rgba.size(); ++i)
            img.rgba[i] = float(splats[s][i]) / 255.0f;
        // An unused alpha channel would read as an empty weight; channels past the last mask stay 0.
        const std::filesystem::path file = folder / (name + "_splat" + std::to_string(s) + ".png");
        image::write(file, img, 8);
        json channels = json::array();
        for (size_t i = s * 4; i < std::min(masks.size(), s * 4 + 4); ++i)
            channels.push_back(masks[i]);
        out.push_back({{"file", file.generic_string()}, {"channels", channels}});
    }
    return out;
}

std::vector<glm::vec3> blendColors(const Heightfield& h, const std::vector<std::string>& masks, const std::vector<glm::dvec3>& colors)
{
    LILA_ZONE();

    std::vector<const std::vector<float>*> g;
    for (const std::string& m : masks)
        g.push_back(h.findMask(m));
    std::vector<glm::vec3> out(h.size(), glm::vec3(0.0f));
    tbb::parallel_for(0, h.nz, [&](int z) {
        for (int x = 0; x < h.nx; ++x)
        {
            const size_t k = h.index(x, z);
            glm::dvec3 c(0.0);
            for (size_t i = 0; i < g.size(); ++i)
                if (g[i] && g[i]->size() == h.size())
                    c += colors[i] * double((*g[i])[k]);
            out[k] = glm::vec3(c);
        }
    });
    return out;
}

std::vector<float> gridImage(const Heightfield& h, const std::vector<float>& values, int width, int height, ImageLayout layout)
{
    std::vector<float> out(size_t(width) * size_t(height));
    tbb::parallel_for(0, height, [&](int py) {
        const double t = height > 1 ? double(py) / double(height - 1) : 0.0;
        const double fz = (layout == ImageLayout::Uv ? 1.0 - t : t) * double(h.nz - 1);
        for (int px = 0; px < width; ++px)
        {
            const double fx = width > 1 ? double(px) / double(width - 1) * double(h.nx - 1) : 0.0;
            out[size_t(py) * size_t(width) + size_t(px)] = float(matBilinear(values, h.nx, h.nz, fx, fz));
        }
    });
    return out;
}
} // namespace terrain
