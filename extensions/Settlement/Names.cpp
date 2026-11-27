// Settlement: street and district names from a naming culture, stable by the ids of the streets they belong to.

#include "Generators.h"

#include <lila/Names.h>

#include <lila/Profile.h>

#include <algorithm>
#include <array>
#include <cctype>
#include <cmath>
#include <map>
#include <numeric>
#include <set>
#include <stdexcept>

namespace settlement
{
namespace
{
/** A name for the street keyed h of class c; attempt picks another when the first is taken. */
std::string cultureStreetName(const std::string& culture, StreetClass cls, uint64_t h, int attempt)
{
    return lila::names::streetName(culture, int(std::min(cls, StreetClass::Path)), h, attempt);
}

std::string cultureDistrictName(const std::string& culture, uint64_t h, int attempt) { return lila::names::districtName(culture, h, attempt); }

std::string ordinal(int n)
{
    const int tens = n % 100;
    const char* suffix = tens >= 11 && tens <= 13 ? "th" : n % 10 == 1 ? "st" : n % 10 == 2 ? "nd" : n % 10 == 3 ? "rd" : "th";
    return std::to_string(n) + suffix;
}

int findRootOf(std::vector<int>& parent, int x)
{
    while (parent[size_t(x)] != x)
        x = parent[size_t(x)] = parent[size_t(parent[size_t(x)])];
    return x;
}

/** Runs of street edges carried on through junctions by their straightest continuation of the same class. */
std::vector<std::vector<int>> strokes(const StreetGraph& g)
{
    std::vector<int> parent(g.edges.size());
    std::iota(parent.begin(), parent.end(), 0);
    for (size_t n = 0; n < g.nodes.size(); ++n)
    {
        std::vector<int> es;
        for (int e : g.incident(int(n)))
            if (g.isStreet(e))
                es.push_back(e);
        auto away = [&](int e) {
            const StreetEdge& x = g.edges[size_t(e)];
            const Vec2 d = g.nodes[size_t(x.a == int(n) ? x.b : x.a)].p - g.nodes[n].p;
            const double len = glm::length(d);
            return len > 1e-9 ? d / len : Vec2(1.0, 0.0);
        };
        struct Pair
        {
            double straight;
            int a, b;
        };
        std::vector<Pair> pairs;
        for (size_t i = 0; i < es.size(); ++i)
            for (size_t j = i + 1; j < es.size(); ++j)
            {
                if (g.edges[size_t(es[i])].cls != g.edges[size_t(es[j])].cls)
                    continue;
                const double straight = -glm::dot(away(es[i]), away(es[j]));
                // Through a plain node a street bends as it likes; at a junction it carries on only within 40 degrees.
                if (es.size() == 2 || straight >= 0.766)
                    pairs.push_back({straight, es[i], es[j]});
            }
        std::sort(pairs.begin(), pairs.end(), [](const Pair& x, const Pair& y) {
            return x.straight != y.straight ? x.straight > y.straight : std::pair(x.a, x.b) < std::pair(y.a, y.b);
        });
        std::set<int> used;
        for (const Pair& p : pairs)
            if (!used.count(p.a) && !used.count(p.b))
            {
                used.insert(p.a);
                used.insert(p.b);
                parent[size_t(findRootOf(parent, p.a))] = findRootOf(parent, p.b);
            }
    }
    std::map<int, std::vector<int>> byRoot;
    for (size_t e = 0; e < g.edges.size(); ++e)
        if (g.isStreet(int(e)))
            byRoot[findRootOf(parent, int(e))].push_back(int(e));
    std::vector<std::vector<int>> out;
    for (auto& [root, edges] : byRoot)
        out.push_back(std::move(edges));
    return out;
}

/** Quarters between the main roads: faces of the arterials, the outline, water and exclusions. */
void findDistricts(Layout& l)
{
    StreetGraph major = l.graph;
    for (size_t e = 0; e < major.edges.size(); ++e)
        if (major.edges[e].alive && major.edges[e].cls > StreetClass::Arterial && major.edges[e].cls < StreetClass::Boundary)
            major.removeEdge(int(e));
    auto q = [](Vec2 p) { return uint64_t(std::llround(p.x / 10.0)) * 0x100000001B3ull ^ uint64_t(std::llround(p.y / 10.0)); };
    for (const Face& f : planarFaces(major))
    {
        if (f.outline.size() < 3)
            continue;
        std::set<uint64_t> around;
        for (int e : f.outline.tag)
        {
            const StreetEdge& x = major.edges[size_t(e)];
            if (x.street != 0)
                around.insert(x.street);
            else
                around.insert(hash64(q(major.nodes[size_t(x.a)].p)) ^ hash64(q(major.nodes[size_t(x.b)].p)));
        }
        uint64_t id = 0x6A09E667F3BCC908ull;
        for (uint64_t k : around)
            id = hash64(id ^ k);
        District d;
        d.id = id & ((1ull << 48) - 1); // small enough for any JSON reader
        d.outline = f.outline;
        d.area = area(f.outline) / kM2;
        d.center = centroid(f.outline);
        l.districts.push_back(std::move(d));
    }
    std::sort(l.districts.begin(), l.districts.end(), [](const District& a, const District& b) { return a.id < b.id; });
}

/** Unit direction along the grid's own axes: u along it, v across it. */
struct GridAxes
{
    Vec2 u, v;
};

/** "North 3rd Street" or "East 2nd Avenue" for a stroke running along the grid; empty when it does not. */
std::string gridName(const Layout& l, const std::vector<int>& edges, const GridAxes& ax)
{
    const StreetGraph& g = l.graph;
    Vec2 dir(0.0), mid(0.0);
    double total = 0.0;
    for (int e : edges)
    {
        const Vec2 a = g.nodes[size_t(g.edges[size_t(e)].a)].p, b = g.nodes[size_t(g.edges[size_t(e)].b)].p;
        Vec2 d = b - a;
        const double len = glm::length(d);
        if (len < 1e-9)
            continue;
        if (glm::dot(d, dir) < 0.0)
            d = -d;
        dir += d;
        mid += (a + b) * 0.5 * len;
        total += len;
    }
    if (total <= 0.0 || glm::length(dir) < 1e-9)
        return {};
    dir /= glm::length(dir);
    mid /= total;
    const double along = std::abs(glm::dot(dir, ax.u)), across = std::abs(glm::dot(dir, ax.v));
    const Vec2 off = mid - l.center;
    if (along > 0.966) // within 15 degrees of the grid's run: a numbered street, counted across the grid
    {
        const int k = int(std::lround(glm::dot(off, ax.v) / std::max(l.profile.gridDepth, 100.0)));
        return (k == 0 ? std::string() : k > 0 ? "South " : "North ") + ordinal(std::abs(k) + 1) + " Street";
    }
    if (across > 0.966)
    {
        const int k = int(std::lround(glm::dot(off, ax.u) / std::max(l.profile.gridWidth, 100.0)));
        return (k == 0 ? std::string() : k > 0 ? "East " : "West ") + ordinal(std::abs(k) + 1) + " Avenue";
    }
    return {};
}
} // namespace

std::vector<std::string> cultureNames() { return lila::names::cultures(); }

std::string defaultCulture(std::string_view profileName)
{
    std::string n(profileName);
    std::transform(n.begin(), n.end(), n.begin(), [](unsigned char ch) { return char(std::tolower(ch)); });
    auto has = [&](const char* s) { return n.find(s) != std::string::npos; };
    if (has("scifi") || has("cyber") || has("sector") || has("arcology"))
        return "sector";
    if (has("roman") || has("castrum"))
        return "latin";
    if (has("mexic") || has("sinaloa"))
        return "mexican";
    if (has("colonial") || has("spanish") || has("medina"))
        return "spanish";
    if (n.starts_with("us") || has("manhattan") || has("midwest") || has("grid"))
        return "usGrid";
    if (has("european") || has("haussmann") || has("baroque") || has("french") || has("renaissance"))
        return "french";
    return "oldEnglish";
}

/** What an imported street run says of a street: its name, or (unnamed) its OSM id; by length along the street. */
struct ImportedLabel
{
    std::string name, osmId;
};

/**
 * Street id to the label of the imported run its edges lie on (within the graph's snap, running the same way), the one
 * covering most of its length; streets laid by Settlement itself are left out.
 */
std::map<uint64_t, ImportedLabel> importedLabels(const Layout& l)
{
    std::map<uint64_t, ImportedLabel> out;
    if (!l.gen || l.gen->importedRuns.empty())
        return out;
    const StreetGraph& g = l.graph;
    constexpr double kCell = 5000.0;
    auto cellOf = [&](Vec2 p) { return std::pair<int64_t, int64_t>{int64_t(std::floor(p.x / kCell)), int64_t(std::floor(p.y / kCell))}; };
    std::map<std::pair<int64_t, int64_t>, std::vector<std::pair<size_t, size_t>>> grid;
    const std::vector<ImportedStreet>& runs = l.gen->importedRuns;
    for (size_t r = 0; r < runs.size(); ++r)
        for (size_t i = 0; i + 1 < runs[r].pts.size(); ++i)
        {
            const Vec2 a = runs[r].pts[i], b = runs[r].pts[i + 1];
            const auto ca = cellOf(glm::min(a, b)), cb = cellOf(glm::max(a, b));
            for (int64_t x = ca.first; x <= cb.first; ++x)
                for (int64_t z = ca.second; z <= cb.second; ++z)
                    grid[{x, z}].push_back({r, i});
        }
    const double reach = std::max(200.0, g.snap() * 1.5);
    std::map<uint64_t, std::map<size_t, double>> along;
    for (size_t e = 0; e < g.edges.size(); ++e)
    {
        if (!g.isStreet(int(e)) || g.edges[e].street == 0)
            continue;
        const Vec2 a = g.nodes[size_t(g.edges[e].a)].p, b = g.nodes[size_t(g.edges[e].b)].p;
        const double len = glm::length(b - a);
        if (len < 1e-6)
            continue;
        const Vec2 m = (a + b) * 0.5, d = (b - a) / len;
        size_t best = SIZE_MAX;
        double bestD = reach;
        const auto c = cellOf(m);
        for (int64_t x = c.first - 1; x <= c.first + 1; ++x)
            for (int64_t z = c.second - 1; z <= c.second + 1; ++z)
                if (auto it = grid.find({x, z}); it != grid.end())
                    for (const auto& [r, i] : it->second)
                    {
                        const Vec2 p = runs[r].pts[i], q = runs[r].pts[i + 1];
                        const double sl = glm::length(q - p);
                        if (sl < 1e-6 || std::abs(glm::dot((q - p) / sl, d)) < 0.9)
                            continue;
                        const double t = std::clamp(glm::dot(m - p, q - p) / (sl * sl), 0.0, 1.0);
                        if (const double dist = glm::length(m - (p + (q - p) * t)); dist < bestD)
                            best = r, bestD = dist;
                    }
        if (best != SIZE_MAX)
            along[g.edges[e].street][best] += len;
    }
    for (const auto& [street, byRun] : along)
    {
        const auto top = std::max_element(byRun.begin(), byRun.end(), [](const auto& x, const auto& y) { return x.second < y.second; });
        out[street] = {runs[top->first].name, runs[top->first].osmId};
    }
    return out;
}

void nameStreets(Layout& l, const Site& site)
{
    LILA_ZONE();

    l.culture = site.culture.empty() ? defaultCulture(l.profile.name) : site.culture;
    lila::names::requireCulture(l.culture);
    const std::string& culture = l.culture;
    // Imported real names (as hashes): a generated name that matches one is passed over like a taken one.
    const lila::names::NameHashes* avoid = l.gen && l.gen->spec && !l.gen->spec->avoid.empty() ? &l.gen->spec->avoid : nullptr;
    auto real = [&](const std::string& s) { return avoid && avoid->matches(s); };
    l.streetNames.clear();
    l.districts.clear();
    findDistricts(l);

    const StreetGraph& g = l.graph;
    std::map<uint64_t, std::string> given;
    for (const StreetOverride& o : site.overrides)
        if (!o.name.empty())
            given[o.street] = o.name;
    std::map<int, std::string> pathNames;
    for (const SitePath& p : site.paths)
        if (!p.name.empty())
            pathNames[p.id] = p.name;

    struct Named
    {
        uint64_t key;
        std::vector<int> edges;
        std::string name;
        bool fixed = false; // given by the user, or a grid number: shared names are meant
    };
    // Imported streets keep the fictional names they came with (Terrain names each real street once), street by street;
    // unnamed imported ones are named from their OSM id, so they keep their names across reruns.
    const std::map<uint64_t, ImportedLabel> imported = importedLabels(l);
    std::vector<Named> runs;
    const double gridRad = glm::radians(l.profile.gridAngle);
    const GridAxes ax{{std::cos(gridRad), std::sin(gridRad)}, {-std::sin(gridRad), std::cos(gridRad)}};
    const bool grid = culture == "usGrid";
    for (std::vector<int>& edges : strokes(g))
    {
        Named n;
        n.key = ~uint64_t(0);
        uint64_t givenStreet = ~uint64_t(0);
        for (int e : edges)
        {
            const StreetEdge& x = g.edges[size_t(e)];
            n.key = std::min(n.key, hash64(x.street));
            if (auto it = given.find(x.street); it != given.end() && x.street < givenStreet)
            {
                givenStreet = x.street;
                n.name = it->second;
            }
        }
        if (n.name.empty())
            for (int e : edges)
                if (auto it = pathNames.find(g.edges[size_t(e)].path); it != pathNames.end())
                {
                    n.name = it->second;
                    break;
                }
        if (n.name.empty() && !imported.empty())
        {
            std::map<std::string, double> byName, byId;
            for (int e : edges)
                if (auto it = imported.find(g.edges[size_t(e)].street); it != imported.end())
                    (it->second.name.empty() ? byId[it->second.osmId] : byName[it->second.name]) += g.length(e);
            auto longest = [](const std::map<std::string, double>& m) {
                return std::max_element(m.begin(), m.end(), [](const auto& x, const auto& y) { return x.second < y.second; })->first;
            };
            if (!byName.empty())
                n.name = longest(byName);
            else if (!byId.empty())
                n.key = lila::names::textKey("osm|" + longest(byId));
        }
        if (n.name.empty() && grid && g.edges[size_t(edges.front())].cls <= StreetClass::Local)
            n.name = gridName(l, edges, ax);
        n.fixed = !n.name.empty();
        n.edges = std::move(edges);
        runs.push_back(std::move(n));
    }
    // Lowest key first, so a street keeps its name whatever is added elsewhere unless two pick the same.
    std::sort(runs.begin(), runs.end(), [](const Named& a, const Named& b) { return a.key < b.key; });
    std::set<std::string> taken;
    for (const Named& n : runs)
        if (n.fixed)
            taken.insert(n.name);
    for (const auto& [street, label] : imported)
        if (!label.name.empty())
            taken.insert(label.name);
    for (Named& n : runs)
    {
        if (n.fixed)
            continue;
        const StreetClass cls = g.edges[size_t(n.edges.front())].cls;
        for (int attempt = 0; attempt < 64 && n.name.empty(); ++attempt)
            if (std::string s = cultureStreetName(culture, cls, n.key, attempt); !taken.count(s) && !real(s))
                n.name = std::move(s);
        // Numbered only from a base that is itself no real name: a number doesn't make a real street's name fictional.
        for (int attempt = 0; attempt < 64 * 16 && n.name.empty(); ++attempt)
            if (const std::string base = cultureStreetName(culture, cls, n.key, attempt % 64); !real(base))
                if (std::string s = base + " " + std::to_string((n.key + uint64_t(attempt / 64)) % 1000 + 1); !taken.count(s) && !real(s))
                    n.name = std::move(s);
        if (n.name.empty())
            n.name = cultureStreetName(culture, cls, n.key, 0) + " " + std::to_string(n.key % 100000);
        taken.insert(n.name);
    }
    for (const Named& n : runs)
        for (int e : n.edges)
            l.streetNames[g.edges[size_t(e)].street] = n.name;
    // A stroke can carry on from one imported street into another: each keeps its own name.
    for (const auto& [street, label] : imported)
        if (!label.name.empty() && !given.count(street))
            l.streetNames[street] = label.name;

    std::map<uint64_t, std::string> districtGiven(site.districtNames.begin(), site.districtNames.end());
    std::set<std::string> districtTaken;
    for (const District& d : l.districts)
        if (auto it = districtGiven.find(d.id); it != districtGiven.end())
            districtTaken.insert(it->second);
    for (District& d : l.districts)
    {
        if (auto it = districtGiven.find(d.id); it != districtGiven.end())
        {
            d.name = it->second;
            continue;
        }
        if (grid && contains(d.outline.pts, l.center))
            d.name = "Downtown";
        for (int attempt = 0; attempt < 64 && d.name.empty(); ++attempt)
            if (std::string s = cultureDistrictName(culture, d.id, attempt); !districtTaken.count(s) && !real(s))
                d.name = std::move(s);
        for (int attempt = 0; attempt < 64 * 16 && d.name.empty(); ++attempt)
            if (const std::string base = cultureDistrictName(culture, d.id, attempt % 64); !real(base))
                if (std::string s = base + " " + std::to_string((d.id + uint64_t(attempt / 64)) % 100 + 1); !districtTaken.count(s) && !real(s))
                    d.name = std::move(s);
        if (d.name.empty())
            d.name = cultureDistrictName(culture, d.id, 0) + " " + std::to_string(d.id % 100000);
        districtTaken.insert(d.name);
    }
}

std::string streetName(const Layout& l, uint64_t street)
{
    auto it = l.streetNames.find(street);
    return it != l.streetNames.end() ? it->second : std::string();
}
} // namespace settlement
