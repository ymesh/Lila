// Settlement: OpenStreetMap buildings and places in a settlement. Buildings become lots and envelopes with their own
// footprints, heights and roofs; places become businesses, uses and landmarks; blocks the data leaves sparse keep the
// lots Settlement cut around the real buildings. Every name here is fictional already (terrainImportOsm named them).

#include "Generators.h"
#include "Parcels.h"

#include <lila/Profile.h>

#include <algorithm>
#include <cmath>
#include <limits>
#include <map>
#include <unordered_map>

namespace settlement
{
namespace
{
constexpr uint64_t kOsmKey52 = (1ull << 52) - 1;
/** Blocks smaller than this (m2) inside imported farmland are town blocks, not fields. */
constexpr double kFarmBlockM2 = 10000.0;

const GenSpec* osmSpecOf(const Layout& l) { return l.gen && l.gen->spec ? l.gen->spec.get() : nullptr; }

Use osmUseOf(const std::string& program, const std::string& kind)
{
    if (kind == "religious")
        return Use::Religious;
    if (kind == "school" || kind == "hospital" || kind == "government" || kind == "police" || kind == "stadium" || kind == "airport" || kind == "landmark")
        return Use::Civic;
    if (kind == "cemetery")
        return Use::Cemetery;
    if (kind == "market")
        return Use::Market;
    if (kind == "port")
        return Use::Port;
    if (kind == "park")
        return Use::Park;
    if (program == "warehouse" || program == "factory" || program == "packingPlant" || program == "coldStorage" || program == "truckYard")
        return Use::Industrial;
    if (program == "farm" || program == "greenhouse")
        return Use::Farm;
    if (program.empty() || program == "apartments" || program == "mansion")
        return Use::Residential;
    return Use::Commercial;
}

/** The landmark an imported building or area is, by what it is: empty for an ordinary one. */
std::string osmLandmarkOf(const std::string& program, const std::string& kind)
{
    static const char* kLandmarks[] = {"cathedral", "church", "stadium", "townhall", "police", "hospital", "market", "port", "cemetery",
                                       "airport", "airstrip", "lighthouse", "museum", "theatre", "university", "ferryTerminal", "marina",
                                       "courthouse", "military", "busStation"};
    for (const char* k : kLandmarks)
        if (program == k)
            return program;
    return kind == "landmark" ? std::string("landmark") : std::string();
}

int osmWealthOf(const OsmBuildingIn& b)
{
    if (b.program == "mansion" || b.building == "villa" || b.building == "mansion")
        return 2;
    if (b.building == "hut" || b.building == "shed" || b.building == "cabin" || b.building == "static_caravan")
        return 0;
    return 1;
}

/** The era a start_date names: medieval, early, industrial or modern; empty when it names none. */
std::string osmEraFromDate(const std::string& date)
{
    int year = 0;
    for (size_t i = 0; i + 4 <= date.size() && !year; ++i)
        if (std::all_of(date.begin() + long(i), date.begin() + long(i) + 4, [](char c) { return c >= '0' && c <= '9'; }))
            year = std::stoi(date.substr(i, 4));
    if (!year)
        return {};
    return year < 1500 ? "medieval" : year < 1850 ? "early" : year < 1945 ? "industrial" : "modern";
}

/** roof:shape as a roof the envelope draws, and where it came from. */
std::string osmRoofOf(const std::string& shape, std::string* source)
{
    static const std::pair<const char*, const char*> kRoofs[] = {
        {"flat", "flat"},       {"gabled", "gable"}, {"hipped", "hip"},       {"half-hipped", "hip"}, {"side_hipped", "hip"},
        {"pyramidal", "hip"},   {"mansard", "mansard"}, {"gambrel", "mansard"}, {"dome", "dome"},       {"onion", "dome"},
        {"round", "gable"},     {"saltbox", "gable"},   {"skillion", "flat"},   {"lean_to", "flat"},     {"cone", "hip"},
    };
    for (const auto& [k, v] : kRoofs)
        if (shape == k)
        {
            *source = shape == v || (shape == "gabled" || shape == "hipped") ? "tag" : "tag (" + shape + " drawn as " + v + ")";
            return v;
        }
    *source = shape.empty() ? "profile" : "profile (roof:shape " + shape + " not drawn)";
    return {};
}

/** Footprint of an imported ring: counter-clockwise, duplicate and collinear corners dropped. */
Polygon osmFootprint(const std::vector<Vec2>& ring)
{
    std::vector<Vec2> pts = ring;
    if (pts.size() > 1 && glm::length(pts.front() - pts.back()) < 1e-6)
        pts.pop_back();
    return makePolygon(std::move(pts));
}

struct OsmBox
{
    Vec2 lo{std::numeric_limits<double>::max()}, hi{-std::numeric_limits<double>::max()};
    void add(Vec2 q) { lo = glm::min(lo, q), hi = glm::max(hi, q); }
    bool holds(Vec2 q) const { return q.x >= lo.x && q.y >= lo.y && q.x <= hi.x && q.y <= hi.y; }
    bool meets(const OsmBox& b) const { return lo.x <= b.hi.x && b.lo.x <= hi.x && lo.y <= b.hi.y && b.lo.y <= hi.y; }
};

OsmBox osmBoxOf(const Polygon& p)
{
    OsmBox b;
    for (const Vec2& q : p.pts)
        b.add(q);
    return b;
}

/** Coarse grid of boxes, for finding which of many polygons hold or meet a point or box. */
struct OsmBins
{
    double cell = 5000.0;
    std::unordered_map<uint64_t, std::vector<int>> at;
    static uint64_t key(int64_t x, int64_t y) { return uint64_t(x) * 0x9E3779B97F4A7C15ull ^ uint64_t(y); }
    void add(const OsmBox& b, int id)
    {
        for (int64_t y = int64_t(std::floor(b.lo.y / cell)); y <= int64_t(std::floor(b.hi.y / cell)); ++y)
            for (int64_t x = int64_t(std::floor(b.lo.x / cell)); x <= int64_t(std::floor(b.hi.x / cell)); ++x)
                at[key(x, y)].push_back(id);
    }
    const std::vector<int>* at_(Vec2 q) const
    {
        auto it = at.find(key(int64_t(std::floor(q.x / cell)), int64_t(std::floor(q.y / cell))));
        return it == at.end() ? nullptr : &it->second;
    }
};

double osmPointSegment(Vec2 q, Vec2 a, Vec2 b)
{
    const Vec2 d = b - a;
    const double len2 = glm::dot(d, d);
    const double t = len2 > 1e-12 ? std::clamp(glm::dot(q - a, d) / len2, 0.0, 1.0) : 0.0;
    return glm::length(q - (a + d * t));
}

int osmDistrictAt(const Layout& l, Vec2 q)
{
    for (size_t d = 0; d < l.districts.size(); ++d)
        if (contains(l.districts[d].outline.pts, q))
            return int(d);
    return -1;
}

double osmMedian(std::vector<double> v)
{
    if (v.empty())
        return std::numeric_limits<double>::quiet_NaN();
    std::sort(v.begin(), v.end());
    return v.size() % 2 ? v[v.size() / 2] : 0.5 * (v[v.size() / 2 - 1] + v[v.size() / 2]);
}

/** Floors a building's tags give (levels, else height over the storey), or -1. */
double osmTaggedFloors(const OsmBuildingIn& b, double storey)
{
    if (b.levels > 0)
        return b.levels;
    if (std::isfinite(b.height) && b.height > 0.0)
        return std::max(1.0, std::round((b.height - (std::isfinite(b.roofHeight) ? b.roofHeight : 0.0)) / storey));
    return -1.0;
}

} // namespace

/** What placeOsmBuildings found and osmEnvelope reads: the buildings by id, their parts, and the district medians. */
struct OsmIndex
{
    std::unordered_map<std::string, const OsmBuildingIn*> byId;
    std::unordered_map<std::string, std::vector<const OsmBuildingIn*>> parts; // building id -> its parts
    std::map<std::pair<int, int>, double> districtFloors;                     // (district, use) -> osmMedian floors
};

bool hasOsmBuildings(const Layout& l)
{
    const GenSpec* s = osmSpecOf(l);
    return s && (!s->osmBuildings.empty() || !s->osmPlaces.empty());
}

void placeOsmBuildings(Layout& l, const Site& site, const Ground& g, const Options& o, size_t firstBlock, std::vector<char>* fresh)
{
    LILA_ZONE();

    (void)g;
    const GenSpec* spec = osmSpecOf(l);
    if (!spec)
        return;
    GenLayout& gen = *l.gen;
    const Profile& p = o.profile;

    // Buildings: parts and buildings drawn inside a larger one become volumes of it, the rest stand on their own lots.
    struct Fp
    {
        const OsmBuildingIn* b;
        Polygon poly;
        OsmBox box;
        Vec2 c{0.0};
        double area = 0.0;
        int owner = -1; // the building it is a volume of
    };
    std::vector<Fp> fps;
    fps.reserve(spec->osmBuildings.size());
    for (const OsmBuildingIn& b : spec->osmBuildings)
    {
        Polygon poly = osmFootprint(b.outer);
        if (poly.size() < 3 || area(poly) < 100.0 * 100.0)
            continue;
        Fp f{&b, std::move(poly)};
        f.box = osmBoxOf(f.poly);
        f.c = centroid(f.poly);
        f.area = area(f.poly);
        fps.push_back(std::move(f));
    }
    OsmBins fpBins;
    for (size_t i = 0; i < fps.size(); ++i)
        if (!fps[i].b->part)
            fpBins.add(fps[i].box, int(i));
    for (size_t i = 0; i < fps.size(); ++i)
    {
        Fp& f = fps[i];
        const std::vector<int>* cand = fpBins.at_(f.c);
        if (!cand)
            continue;
        int best = -1;
        for (int j : *cand)
        {
            const Fp& h = fps[size_t(j)];
            if (size_t(j) == i || h.area <= f.area * (f.b->part ? 1.0 : 2.0) - 1e-6 || !h.box.holds(f.c) || !contains(h.poly.pts, f.c))
                continue;
            if (best < 0 || h.area < fps[size_t(best)].area)
                best = j;
        }
        f.owner = best;
    }
    gen.osmIndex = std::make_shared<OsmIndex>();
    OsmIndex& index = *gen.osmIndex;
    for (const Fp& f : fps)
    {
        index.byId[f.b->id] = f.b;
        if (f.owner >= 0)
        {
            index.parts[fps[size_t(f.owner)].b->id].push_back(f.b);
            ++gen.osmParts;
        }
    }
    // District defaults by use: the osmMedian floors of the buildings around that tag theirs.
    {
        std::map<std::pair<int, int>, std::vector<double>> samples;
        for (const Fp& f : fps)
            if (f.owner < 0)
                if (const double fl = osmTaggedFloors(*f.b, p.floorHeight); fl > 0.0)
                    samples[{osmDistrictAt(l, f.c), int(osmUseOf(f.b->program, f.b->kind))}].push_back(fl);
        for (auto& [k, v] : samples)
            if (v.size() >= 3)
                index.districtFloors[k] = osmMedian(std::move(v));
    }

    // Blocks: which buildings stand in each, and how much of it they cover.
    OsmBins blockBins;
    blockBins.cell = std::max(5000.0, 2.0 * p.lotDepth);
    std::vector<OsmBox> blockBox(l.blocks.size());
    for (size_t b = firstBlock; b < l.blocks.size(); ++b)
    {
        blockBox[b] = osmBoxOf(l.blocks[b].outline);
        blockBins.add(blockBox[b], int(b));
    }
    std::vector<std::vector<int>> inBlock(l.blocks.size());
    for (size_t i = 0; i < fps.size(); ++i)
    {
        if (fps[i].owner >= 0)
            continue;
        int block = -1;
        if (const std::vector<int>* cand = blockBins.at_(fps[i].c))
            for (int b : *cand)
                if (blockBox[size_t(b)].holds(fps[i].c) && contains(l.blocks[size_t(b)].outline.pts, fps[i].c))
                {
                    block = b;
                    break;
                }
        if (block >= 0)
            inBlock[size_t(block)].push_back(int(i));
        else if (firstBlock == 0)
            ++gen.osmOutside;
    }

    // Lots Settlement cut: gone from dense blocks, kept in sparse ones where no building stands.
    std::vector<char> drop(l.lots.size(), 0);
    std::vector<char> denseBlock(l.blocks.size(), 0);
    for (size_t b = firstBlock; b < l.blocks.size(); ++b)
    {
        if (inBlock[b].empty())
            continue;
        double covered = 0.0;
        for (int i : inBlock[b])
            covered += fps[size_t(i)].area;
        const double share = covered / std::max(l.blocks[b].area * kM2, 1.0);
        denseBlock[b] = spec->osmFill == "none" || (spec->osmFill == "sparse" && share >= spec->osmCoverage);
        ++(denseBlock[b] ? gen.osmDenseBlocks : gen.osmSparseBlocks);
    }
    for (size_t k = 0; k < l.lots.size(); ++k)
    {
        const Lot& lot = l.lots[k];
        if (size_t(lot.block) < firstBlock || !lot.osmId.empty())
            continue;
        // fill none keeps none of Settlement's own lots, in blocks the data leaves empty too.
        if (spec->osmFill == "none" || (!inBlock[size_t(lot.block)].empty() && denseBlock[size_t(lot.block)]))
        {
            drop[k] = 1;
            continue;
        }
        // Any footprint reaching into the lot takes it, the block's own and one whose centre lies in the next block.
        const OsmBox lb = osmBoxOf(lot.outline);
        for (int64_t y = int64_t(std::floor(lb.lo.y / fpBins.cell)); y <= int64_t(std::floor(lb.hi.y / fpBins.cell)) && !drop[k]; ++y)
            for (int64_t x = int64_t(std::floor(lb.lo.x / fpBins.cell)); x <= int64_t(std::floor(lb.hi.x / fpBins.cell)) && !drop[k]; ++x)
                if (auto it = fpBins.at.find(OsmBins::key(x, y)); it != fpBins.at.end())
                    for (int i : it->second)
                        if (fps[size_t(i)].box.meets(lb) && polygonsShareArea(lot.outline, fps[size_t(i)].poly, 2.0))
                        {
                            drop[k] = 1;
                            break;
                        }
    }
    {
        size_t w = 0;
        for (size_t k = 0; k < l.lots.size(); ++k)
        {
            if (drop[k])
                continue;
            if (l.lots[k].osmId.empty() && size_t(l.lots[k].block) >= firstBlock && !inBlock[size_t(l.lots[k].block)].empty())
                ++gen.osmFillLots;
            if (w != k)
            {
                l.lots[w] = std::move(l.lots[k]);
                if (fresh)
                    (*fresh)[w] = (*fresh)[k];
            }
            l.lots[w].id = int(w);
            ++w;
        }
        l.lots.resize(w);
        if (fresh)
            fresh->resize(w);
    }

    // Footprints the data draws over each other: the larger stands, the smaller is left out (counted in overlapping).
    for (size_t b = firstBlock; b < l.blocks.size(); ++b)
    {
        std::vector<int>& in = inBlock[b];
        std::stable_sort(in.begin(), in.end(), [&](int x, int y) { return fps[size_t(x)].area > fps[size_t(y)].area; });
        std::vector<int> kept;
        for (int i : in)
            if (std::ranges::none_of(kept, [&](int k) { return fps[size_t(k)].box.meets(fps[size_t(i)].box) && polygonsShareArea(fps[size_t(k)].poly, fps[size_t(i)].poly, 2.0); }))
                kept.push_back(i);
            else
                ++gen.osmOverlapping;
        std::sort(kept.begin(), kept.end());
        in = std::move(kept);
    }

    // A lot per building: its sides facing the block's street sides are its frontages.
    for (size_t b = firstBlock; b < l.blocks.size(); ++b)
        for (int i : inBlock[b])
        {
            const Fp& f = fps[size_t(i)];
            const Block& block = l.blocks[b];
            Lot lot;
            lot.id = int(l.lots.size());
            lot.block = int(b);
            lot.outline = f.poly;
            lot.outline.tag.assign(lot.outline.size(), -1);
            const double reach = std::max(800.0, 0.25 * p.lotDepth);
            for (size_t s = 0; s < lot.outline.size(); ++s)
            {
                const Vec2 a = lot.outline.pts[s], z = lot.outline.pts[(s + 1) % lot.outline.size()];
                const Vec2 m = (a + z) * 0.5;
                const double len = glm::length(z - a);
                if (len < 100.0)
                    continue;
                const Vec2 u = (z - a) / len;
                double best = reach;
                for (size_t t = 0; t < block.outline.size(); ++t)
                {
                    if (block.outline.tag[t] < 0)
                        continue;
                    const Vec2 ba = block.outline.pts[t], bz = block.outline.pts[(t + 1) % block.outline.size()];
                    const double bl = glm::length(bz - ba);
                    if (bl < 1e-6 || glm::dot(u, (bz - ba) / bl) < 0.8)
                        continue;
                    const double d = osmPointSegment(m, ba, bz);
                    if (d < best)
                    {
                        best = d;
                        lot.outline.tag[s] = block.outline.tag[t];
                    }
                }
            }
            lot.key = lila::names::textKey("osm|" + f.b->id) & kOsmKey52;
            lot.area = f.area / kM2;
            lot.random = float(unitRandom(hash64(lot.key) ^ hash64(o.seed)));
            finishOsmLot(lot, l);
            lot.use = osmUseOf(f.b->program, f.b->kind);
            lot.wealth = osmWealthOf(*f.b);
            lot.osmId = f.b->id;
            lot.name = f.b->name;
            lot.program = f.b->program;
            lot.landmark = osmLandmarkOf(f.b->program, f.b->kind);
            lot.reasons = {"OpenStreetMap building " + f.b->id};
            l.lots.push_back(std::move(lot));
            if (fresh)
                fresh->push_back(1);
            ++gen.osmLots;
        }

    // Places: points onto the building or lot they stand on, areas onto the lots inside them.
    OsmBins lotBins;
    lotBins.cell = std::max(2000.0, p.lotDepth);
    std::vector<OsmBox> lotBox(l.lots.size());
    for (size_t k = 0; k < l.lots.size(); ++k)
    {
        lotBox[k] = osmBoxOf(l.lots[k].outline);
        if (size_t(l.lots[k].block) >= firstBlock)
            lotBins.add(lotBox[k], int(k));
    }
    auto lotUnder = [&](Vec2 q, double reach) -> int {
        int best = -1;
        double bestD = reach;
        for (int dx = -1; dx <= 1; ++dx)
            for (int dy = -1; dy <= 1; ++dy)
                if (const std::vector<int>* cand = lotBins.at_(q + Vec2(dx, dy) * lotBins.cell))
                    for (int k : *cand)
                    {
                        const Lot& lot = l.lots[size_t(k)];
                        if (lot.use == Use::Plaza || lot.use == Use::Open)
                            continue;
                        const double d = contains(lot.outline.pts, q) ? -1.0 : boundaryDistance(lot.outline.pts, q);
                        // A building holding the point wins over a lot beside it.
                        const double score = d < 0.0 ? (lot.osmId.empty() ? -1.0 : -2.0) : d;
                        if (score < bestD)
                        {
                            bestD = score;
                            best = k;
                        }
                    }
        return best;
    };
    std::map<std::string, size_t> siteOf;
    for (const OsmSite& s : gen.osmSites)
        siteOf[s.id] = &s - gen.osmSites.data();
    for (const OsmPlaceIn& pl : spec->osmPlaces)
    {
        if (siteOf.count(pl.id))
            continue;
        OsmSite made{pl.id, pl.kind, pl.program, pl.name, pl.at};
        if (pl.rings.empty())
        {
            if (pl.kind == "town" || pl.kind == "district" || pl.kind == "island" || pl.kind == "mountain" || pl.kind == "bay" || pl.kind == "beach")
            {
                made.placed = pl.kind == "district" ? "district" : "site";
                gen.osmSites.push_back(std::move(made));
                continue;
            }
            const int k = lotUnder(pl.at, 2500.0);
            if (k < 0)
            {
                made.placed = "site";
                gen.osmSites.push_back(std::move(made));
                continue;
            }
            Lot& lot = l.lots[size_t(k)];
            lot.businesses.push_back({pl.name, pl.program, pl.id, pl.at});
            if (lot.program.empty() || lot.program == "apartments")
            {
                lot.program = pl.program;
                lot.name = pl.name;
            }
            if (lot.osmId.empty() && lot.use == Use::Residential)
            {
                const Use u = osmUseOf(pl.program, pl.kind);
                lot.use = u == Use::Residential ? Use::Commercial : u;
            }
            if (lot.landmark.empty())
                lot.landmark = osmLandmarkOf(pl.program, pl.kind);
            made.lots.push_back(lot.key);
            made.placed = lot.osmId.empty() ? "lot" : "building";
            if (!lot.landmark.empty())
                made.landmarkLot = lot.key;
            gen.osmSites.push_back(std::move(made));
            continue;
        }
        OsmBox area;
        for (const auto& r : pl.rings)
        {
            Polygon poly = osmFootprint(r);
            if (poly.size() >= 3)
            {
                for (const Vec2& q : poly.pts)
                    area.add(q);
                made.rings.push_back(std::move(poly));
            }
        }
        if (made.rings.empty())
            continue;
        auto inside = [&](Vec2 q) {
            int n = 0;
            for (const Polygon& r : made.rings)
                n += contains(r.pts, q) ? 1 : 0;
            return n % 2 == 1;
        };
        const Use areaUse = osmUseOf(pl.program, pl.kind);
        const std::string landmark = osmLandmarkOf(pl.program, pl.kind);
        Lot* largest = nullptr;
        std::vector<int> near;
        for (int64_t y = int64_t(std::floor(area.lo.y / lotBins.cell)); y <= int64_t(std::floor(area.hi.y / lotBins.cell)); ++y)
            for (int64_t x = int64_t(std::floor(area.lo.x / lotBins.cell)); x <= int64_t(std::floor(area.hi.x / lotBins.cell)); ++x)
                if (auto it = lotBins.at.find(OsmBins::key(x, y)); it != lotBins.at.end())
                    near.insert(near.end(), it->second.begin(), it->second.end());
        std::sort(near.begin(), near.end());
        near.erase(std::unique(near.begin(), near.end()), near.end());
        for (const int k : near)
        {
            Lot& lot = l.lots[size_t(k)];
            if (size_t(lot.block) < firstBlock || !area.meets(lotBox[size_t(k)]) || !inside(centroid(lot.outline)))
                continue;
            // Farmland the streets have cut into town blocks is the data lagging the town: those blocks are built on.
            if (areaUse == Use::Farm && lot.osmId.empty() && size_t(lot.block) < l.blocks.size() && l.blocks[size_t(lot.block)].area < kFarmBlockM2)
                continue;
            made.lots.push_back(lot.key);
            if (lot.osmId.empty())
            {
                if (areaUse != Use::Commercial && areaUse != Use::Residential)
                    lot.use = areaUse;
                if (lot.program.empty())
                    lot.program = pl.program;
            }
            else if (lot.program.empty())
                lot.program = pl.program;
            if (!largest || lot.area > largest->area)
                largest = &lot;
        }
        if (largest && !landmark.empty() && std::ranges::none_of(l.lots, [&](const Lot& x) { return x.landmark == landmark && inside(centroid(x.outline)); }))
        {
            if (largest->landmark.empty())
                largest->landmark = landmark;
            if (largest->name.empty())
                largest->name = pl.name;
            made.landmarkLot = largest->key;
        }
        made.placed = made.lots.empty() ? "site" : "area";
        gen.osmSites.push_back(std::move(made));
    }
    // A neighbourhood's fictional name goes to the district it lies in, unless the user named that district.
    for (const OsmSite& s : gen.osmSites)
        if (s.kind == "district" && !s.name.empty())
            if (const int d = osmDistrictAt(l, s.at); d >= 0)
            {
                District& dist = l.districts[size_t(d)];
                const bool given = std::ranges::any_of(site.districtNames, [&](const auto& n) { return n.first == dist.id; });
                const bool named = std::ranges::any_of(gen.osmSites, [&](const OsmSite& o2) {
                    return o2.kind == "district" && &o2 < &s && osmDistrictAt(l, o2.at) == d;
                });
                if (!given && !named)
                    dist.name = s.name;
            }
    if (gen.osmOverlapping > 0 && firstBlock == 0)
        gen.warnings.push_back("osm: " + std::to_string(gen.osmOverlapping) + " imported buildings overlap a larger one and were left out");
    if (gen.osmOutside > 0 && firstBlock == 0)
        gen.warnings.push_back("osm: " + std::to_string(gen.osmOutside) + " imported buildings stand on no block (on a street, by water or outside the outline) and were left out");
}

Envelope osmEnvelope(const Lot& lot, const Layout& l, const Ground& g, const Options& o, const Site& site)
{
    LILA_ZONE();
    Envelope env;
    if (!l.gen || !l.gen->osmIndex)
        return env;
    const OsmIndex& index = *l.gen->osmIndex;
    auto it = index.byId.find(lot.osmId);
    if (it == index.byId.end())
        return env;
    const OsmBuildingIn& b = *it->second;
    const Profile& p = o.profile;
    env.lot = lot.id;
    env.key = lot.key;
    env.footprint = lot.outline;
    for (int& t : env.footprint.tag)
        t = t >= 0 ? 0 : 2;
    env.use = lot.use;
    env.landmark = lot.landmark;
    env.wealth = lot.wealth;
    env.route = lot.route;
    env.osmId = lot.osmId;
    env.colour = b.colour;
    env.roofColour = b.roofColour;
    env.material = b.material;
    env.roofMaterial = b.roofMaterial;
    env.roofShape = b.roofShape;
    env.eraName = osmEraFromDate(b.startDate);
    if (env.eraName.empty())
        env.eraName = eraOf(p);
    for (const auto& h : b.holes)
        if (Polygon poly = osmFootprint(h); poly.size() >= 3)
            env.holes.push_back(std::move(poly));

    // Front: out of the longest street side, else toward the nearest street side of the block.
    double longest = 0.0;
    for (size_t k = 0; k < env.footprint.size(); ++k)
    {
        const Vec2 a = env.footprint.pts[k], z = env.footprint.pts[(k + 1) % env.footprint.size()];
        const double len = glm::length(z - a);
        if (env.footprint.tag[k] == 0 && len > longest)
        {
            longest = len;
            env.front = Vec2((z - a).y, -(z - a).x) / len;
        }
    }
    const Vec2 c = centroid(env.footprint);
    if (longest == 0.0 && lot.block >= 0 && size_t(lot.block) < l.blocks.size())
    {
        const Polygon& bo = l.blocks[size_t(lot.block)].outline;
        double best = std::numeric_limits<double>::max();
        for (size_t k = 0; k < bo.size(); ++k)
        {
            if (bo.tag[k] < 0)
                continue;
            const Vec2 a = bo.pts[k], z = bo.pts[(k + 1) % bo.size()];
            const double d = osmPointSegment(c, a, z);
            if (d < best && glm::length(z - a) > 1e-6)
            {
                best = d;
                env.front = Vec2((z - a).y, -(z - a).x) / glm::length(z - a);
            }
        }
    }

    // Heights: tags first, then the district's buildings of the same use, then the profile.
    const double storey = p.floorHeight * (lot.use == Use::Religious ? 1.6 : lot.use == Use::Industrial ? 1.4 : lot.use == Use::Market ? 1.3 : 1.0);
    double wall = 0.0;
    int floors = 0;
    if (std::isfinite(b.height) && b.height > 0.0)
    {
        const double roof = std::isfinite(b.roofHeight) ? b.roofHeight : b.roofLevels > 0 ? b.roofLevels * storey * 0.8 : 0.0;
        wall = std::max(100.0, b.height - roof);
        floors = b.levels > 0 ? b.levels : std::max(1, int(std::lround(wall / storey)));
        env.heightSource = "height";
    }
    else if (b.levels > 0)
    {
        floors = b.levels;
        wall = floors * storey;
        env.heightSource = "levels";
    }
    else if (auto d = index.districtFloors.find({osmDistrictAt(l, c), int(lot.use)}); d != index.districtFloors.end())
    {
        floors = std::max(1, int(std::lround(d->second)));
        wall = floors * storey;
        env.heightSource = "district";
    }
    else
    {
        const double dist = std::clamp(glm::length(c - l.center) / std::max(l.radius, 1.0), 0.0, 1.0);
        double fl = p.minFloors + (p.maxFloors - p.minFloors) * std::pow(1.0 - dist, std::max(p.falloff, 0.1));
        fl *= (0.8 + 0.4 * double(lot.random)) * std::max(o.heightScale, 0.0);
        floors = std::max(1, int(std::lround(fl)));
        switch (lot.use)
        {
        case Use::Religious: floors = std::max(floors, 2); break;
        case Use::Industrial: floors = std::clamp(floors, 1, 3); break;
        case Use::Market: floors = std::clamp(floors, 1, 2); break;
        default: break;
        }
        wall = floors * storey;
        env.heightSource = "profile";
    }
    env.floors = floors;
    env.height = wall;

    double lo = 0.0, hi = 0.0;
    envelopeGround(env.footprint, g, lo, hi);
    env.foundationSource = "ground";
    if (auto f = site.foundations.find(lot.key); f != site.foundations.end())
    {
        lo = std::min(lo, f->second.min);
        hi = std::max(hi, f->second.max);
        env.foundationSource = "terrainRegionStats";
    }
    env.ground = lo;
    env.base = lo - p.foundation;
    env.foundationDepth = hi - env.base;
    env.top = hi + wall;

    // Volumes: the building:parts inside it when it has any, else its own footprint from its min_height.
    if (auto parts = index.parts.find(b.id); parts != index.parts.end())
    {
        for (const OsmBuildingIn* part : parts->second)
        {
            Polygon fp = osmFootprint(part->outer);
            if (fp.size() < 3)
                continue;
            fp.tag.assign(fp.size(), 2);
            const double partWall = std::isfinite(part->height) && part->height > 0.0 ? part->height - (std::isfinite(part->roofHeight) ? part->roofHeight : 0.0)
                                    : part->levels > 0                                ? part->levels * storey
                                                                                      : wall;
            const double bottom = std::isfinite(part->minHeight) && part->minHeight > 0.0 ? hi + part->minHeight
                                  : part->minLevel > 0                                    ? hi + part->minLevel * storey
                                                                                          : env.base;
            const double top = std::max(bottom + 50.0, hi + partWall);
            // A part keeps its own roof (a domed tower stays domed); the height comes from its tags, else from its shape.
            std::string partSource;
            std::string partRoof = osmRoofOf(part->roofShape, &partSource);
            double partRoofHeight = 0.0;
            if (partRoof == "flat")
                partRoof.clear();
            if (!partRoof.empty())
            {
                Vec2 pc{0.0}, pu{1.0, 0.0}, pv{0.0, 1.0};
                double phu = 0.0, phv = 0.0;
                boxOf(fp, pc, pu, pv, phu, phv);
                partRoofHeight = std::isfinite(part->roofHeight) && part->roofHeight > 0.0 ? part->roofHeight
                                 : part->roofLevels > 0                                  ? part->roofLevels * storey * 0.8
                                 : partRoof == "dome"                                    ? std::min(phu, phv)
                                 : partRoof == "mansard"                                 ? 0.9 * p.floorHeight
                                                                                         : phv * std::tan(30.0 * kPi / 180.0);
            }
            env.tiers.push_back({std::move(fp), bottom, top, partRoof, partRoofHeight});
            env.top = std::max(env.top, top);
        }
        std::sort(env.tiers.begin(), env.tiers.end(), [](const Envelope::Tier& x, const Envelope::Tier& y) { return x.bottom < y.bottom; });
    }
    if (env.tiers.empty())
    {
        const double bottom = std::isfinite(b.minHeight) && b.minHeight > 0.0 ? hi + b.minHeight : b.minLevel > 0 ? hi + b.minLevel * storey : env.base;
        env.tiers.push_back({env.footprint, bottom, env.top});
    }

    // Roof: roof:shape when it is one the envelope draws, else what the profile and use give.
    env.roof = osmRoofOf(b.roofShape, &env.roofSource);
    if (env.roof.empty())
        env.roof = env.tiers.size() > 1 || lot.use == Use::Industrial || floors > 6 ? "flat"
                   : lot.use == Use::Religious                                     ? "gable"
                   : p.roof == "gable" || p.roof == "hip" || p.roof == "flat" || p.roof == "mansard" || p.roof == "dome" ? p.roof
                                                                                                                         : "gable";
    Vec2 bc{0.0}, bu{1.0, 0.0}, bv{0.0, 1.0};
    double hu = 0.0, hv = 0.0;
    boxOf(env.tiers.back().footprint, bc, bu, bv, hu, hv);
    env.ridge = bu;
    if (std::isfinite(b.roofHeight) && b.roofHeight > 0.0)
        env.roofHeight = b.roofHeight;
    else if (b.roofLevels > 0)
        env.roofHeight = b.roofLevels * storey * 0.8;
    else if (env.roof == "gable" || env.roof == "hip")
        env.roofHeight = hv * std::tan(30.0 * kPi / 180.0);
    else if (env.roof == "mansard")
        env.roofHeight = 0.9 * p.floorHeight;
    else if (env.roof == "dome")
        env.roofHeight = std::min(hu, hv);

    // Entrances: the middle of each street side, longest first; a back building's door faces its front.
    std::vector<std::pair<double, Vec2>> doors;
    for (size_t k = 0; k < env.footprint.size(); ++k)
        if (env.footprint.tag[k] == 0)
        {
            const Vec2 a = env.footprint.pts[k], z = env.footprint.pts[(k + 1) % env.footprint.size()];
            if (glm::length(z - a) >= 100.0)
                doors.push_back({glm::length(z - a), (a + z) * 0.5});
        }
    if (doors.empty())
    {
        double best = -2.0;
        for (size_t k = 0; k < env.footprint.size(); ++k)
        {
            const Vec2 a = env.footprint.pts[k], z = env.footprint.pts[(k + 1) % env.footprint.size()];
            if (glm::length(z - a) < 1e-6)
                continue;
            const Vec2 out(glm::normalize(z - a).y, -glm::normalize(z - a).x);
            if (const double f = glm::dot(out, env.front); f > best)
            {
                best = f;
                doors = {{glm::length(z - a), (a + z) * 0.5}};
            }
        }
    }
    std::stable_sort(doors.begin(), doors.end(), [](const auto& x, const auto& y) { return x.first > y.first; });
    for (const auto& [len, at] : doors)
        env.entrances.push_back(at);

    env.groundUse = lot.use == Use::Market ? Use::Commercial : lot.use;
    env.corner = lot.corner ? "wrap" : "none";
    env.partyWith.assign(env.footprint.size(), -1);
    return env;
}

void finishProgram(Envelope& env, const Lot& lot, const Layout& l)
{
    LILA_ZONE();
    env.name = lot.name;
    env.program = lot.program;
    env.businesses = lot.businesses;
    if (!env.businesses.empty() && (env.groundUse == Use::Residential || env.groundUse == Use::Civic))
        env.groundUse = env.groundUse == Use::Civic ? Use::Civic : Use::Commercial;
    const std::string& pg = env.program;
    const Vec2 c = centroid(env.footprint);
    const double dist = std::clamp(glm::length(c - l.center) / std::max(l.radius, 1.0), 0.0, 1.0);
    const bool old = env.eraName == "medieval" || env.eraName == "early";
    double water = std::numeric_limits<double>::max();
    for (const WaterLine& w : l.water)
        for (const Vec2& q : w.pts)
            water = std::min(water, glm::length(q - c));
    // Each rule names what decided it, so a wrong guess can be traced and overridden.
    auto set = [&](const char* style, std::string why) {
        env.style = style;
        env.styleFrom = std::move(why);
    };
    if (pg == "cathedral" || pg == "church" || env.use == Use::Religious)
        set("church", "religious use");
    else if (env.use == Use::Cemetery)
        set("mausoleum", "stands in a cemetery");
    else if (env.use == Use::Civic)
        set("civic", "civic use");
    else if (pg == "packingPlant" || pg == "coldStorage" || (env.use == Use::Industrial && lot.use == Use::Farm))
        set("packingPlant", "packing or cold storage");
    else if (env.use == Use::Port || (env.use == Use::Industrial && water < 15000.0))
        set("portShed", env.use == Use::Port ? "port use" : "industry within 150 m of water");
    else if (env.use == Use::Industrial)
        set("warehouse", "industrial use");
    else if (pg == "mansion" || (env.use == Use::Residential && env.wealth >= 2 && env.floors <= 3 && lot.area >= 400.0))
        set("mansion", pg == "mansion" ? "mansion or villa" : "wealthy, low and on a lot of 400 m2 or more");
    else if ((pg == "hotel" || pg == "apartments") && env.floors >= 6)
        set("resortTower", "hotel or apartments of 6 floors or more");
    else if (water < 6000.0 && env.floors <= 4 && dist < 0.8)
        set("seafront", "within 60 m of water");
    else if (old || (dist < 0.3 && env.floors <= 3))
        set("colonial", old ? "built " + env.eraName : "three floors or fewer in the old centre");
    else if (env.use == Use::Farm || (dist > 0.85 && env.floors <= 1))
        set(env.eraName == "modern" ? "ruralShack" : "adobe", env.use == Use::Farm ? "farm use" : "single storey at the edge");
    else if ((env.groundUse == Use::Commercial || env.use == Use::Commercial) && env.floors <= l.profile.shopHouseFloors)
        set("residential", "shop under a house of " + std::to_string(l.profile.shopHouseFloors) + " floors or fewer");
    else if (env.groundUse == Use::Commercial || env.use == Use::Commercial)
        set("modernCommercial", "commercial use");
    else
        set("residential", "residential");
}

json osmSitesJson(const Layout& l)
{
    json out = json::array();
    if (!l.gen)
        return out;
    for (const OsmSite& s : l.gen->osmSites)
    {
        json rings = json::array();
        for (const Polygon& r : s.rings)
        {
            json ring = json::array();
            for (const Vec2& q : r.pts)
                ring.push_back({q.x, q.y});
            rings.push_back(ring);
        }
        out.push_back({{"id", s.id},
                       {"kind", s.kind},
                       {"program", s.program},
                       {"name", s.name},
                       {"at", {s.at.x, s.at.y}},
                       {"rings", rings},
                       {"lots", s.lots},
                       {"landmarkLot", s.landmarkLot},
                       {"placed", s.placed}});
    }
    return out;
}
} // namespace settlement
