#include "uv/Hotspot.h"

#include "core/Profile.h"
#include "geo/Attribute.h"
#include "geo/Ops.h"
#include "uv/Uv.h"

#include <tbb/parallel_for.h>

#include <algorithm>
#include <cmath>
#include <map>
#include <numbers>
#include <unordered_map>

namespace lila::uv
{
namespace
{
/** Turns the chart's uvs a quarter turn k times about their centroid. */
void quarterTurns(Chart& chart, int k)
{
    k = ((k % 4) + 4) % 4;
    if (k == 0)
        return;
    const glm::dvec2 c = chart.uvCentroid();
    for (glm::dvec2& p : chart.uv)
    {
        const glm::dvec2 d = p - c;
        const glm::dvec2 r = k == 1 ? glm::dvec2(-d.y, d.x) : k == 2 ? -d : glm::dvec2(d.y, -d.x);
        p = c + r;
    }
}

void rotateUv(Chart& chart, double angle)
{
    const glm::dvec2 c = chart.uvCentroid();
    const double cs = std::cos(angle), sn = std::sin(angle);
    for (glm::dvec2& p : chart.uv)
    {
        const glm::dvec2 d = p - c;
        p = c + glm::dvec2(cs * d.x - sn * d.y, sn * d.x + cs * d.y);
    }
}

/** Area weighted direction of increasing world height in uv, over the chart's triangles. */
glm::dvec2 upGradient(const Chart& chart, const glm::dvec3& up)
{
    glm::dvec2 sum(0.0);
    for (const auto& t : chart.tris)
    {
        const glm::dvec2 e1 = chart.uv[t[1]] - chart.uv[t[0]], e2 = chart.uv[t[2]] - chart.uv[t[0]];
        const double det = e1.x * e2.y - e2.x * e1.y;
        if (std::abs(det) < 1e-20)
            continue;
        const double f0 = glm::dot(chart.pos[t[0]], up);
        const double d1 = glm::dot(chart.pos[t[1]], up) - f0, d2 = glm::dot(chart.pos[t[2]], up) - f0;
        const glm::dvec2 g((d1 * e2.y - d2 * e1.y) / det, (d2 * e1.x - d1 * e2.x) / det);
        sum += g * std::abs(det);
    }
    return sum;
}

glm::dvec3 chartNormal(const Chart& chart)
{
    glm::dvec3 n(0.0);
    for (const auto& t : chart.tris)
        n += glm::cross(chart.pos[t[1]] - chart.pos[t[0]], chart.pos[t[2]] - chart.pos[t[0]]);
    const double len = glm::length(n);
    return len > 1e-20 ? n / len : glm::dvec3(0.0);
}

/** How much of its tightest bounding rectangle an island must fill to be rectified onto it. */
constexpr double kRectifyFill = 0.9;

/** Every triangle faces within half a degree of the island's normal. */
bool isPlanar(const Chart& chart, const glm::dvec3& normal)
{
    const double minCos = std::cos(glm::radians(0.5));
    for (const auto& t : chart.tris)
    {
        const glm::dvec3 n = glm::cross(chart.pos[t[1]] - chart.pos[t[0]], chart.pos[t[2]] - chart.pos[t[0]]);
        const double len = glm::length(n);
        if (len > 1e-20 && glm::dot(n / len, normal) < minCos)
            return false;
    }
    return true;
}

/** Lays the island's points flat in its plane: uvs are their coordinates along two axes across the normal. */
void projectToPlane(Chart& chart, const glm::dvec3& normal)
{
    const glm::dvec3 helper = std::abs(normal.y) < 0.9 ? glm::dvec3(0, 1, 0) : glm::dvec3(1, 0, 0);
    const glm::dvec3 u = glm::normalize(glm::cross(helper, normal)), v = glm::cross(normal, u);
    chart.uv.resize(chart.pos.size());
    for (size_t i = 0; i < chart.pos.size(); ++i)
        chart.uv[i] = {glm::dot(chart.pos[i], u), glm::dot(chart.pos[i], v)};
}

/** A disk whose boundary sits at an even distance from its centre and fills the circle: caps, discs, round plates. */
bool looksRadial(const Chart& chart)
{
    if (chart.euler() != 1)
        return false;
    const std::vector<std::vector<Index>> loops = chart.boundaryLoops();
    if (loops.size() != 1 || loops[0].size() < 8)
        return false;
    const glm::dvec2 c = chart.uvCentroid();
    double mean = 0.0;
    for (Index v : loops[0])
        mean += glm::length(chart.uv[v] - c);
    mean /= double(loops[0].size());
    if (mean <= 0.0)
        return false;
    double var = 0.0;
    for (Index v : loops[0])
    {
        const double d = glm::length(chart.uv[v] - c) - mean;
        var += d * d;
    }
    const double cv = std::sqrt(var / double(loops[0].size())) / mean;
    const double fill = std::abs(chart.areaUv()) / (std::numbers::pi * mean * mean);
    return cv < 0.08 && fill > 0.85;
}

bool hasTag(const HotspotTrim& t, const std::string& tag) { return std::ranges::find(t.tags, tag) != t.tags.end(); }

uint64_t splitmix(uint64_t x)
{
    x += 0x9E3779B97F4A7C15ull;
    x = (x ^ (x >> 30)) * 0xBF58476D1CE4E5B9ull;
    x = (x ^ (x >> 27)) * 0x94D049BB133111EBull;
    return x ^ (x >> 31);
}
} // namespace

double hotspotRandom(uint32_t seed, uint64_t a, uint64_t b)
{
    const uint64_t h = splitmix(splitmix(splitmix(seed) ^ a) ^ (b * 0x2545F4914F6CDD1Dull));
    return double(h >> 11) * (1.0 / 9007199254740992.0);
}

IslandMeasure measureChart(Chart& chart, const HotspotOptions& opts)
{
    IslandMeasure m;
    m.area3d = chart.area3d();
    m.normal = chartNormal(chart);
    if (chart.uv.size() != chart.pos.size() || m.area3d <= 0.0)
        return m;

    // A flat island is laid flat exactly: an unwrap can fold polygons with points in a row, and rectifying would shear
    // slanted quads (a carved wall) into a grid.
    const bool planar = isPlanar(chart, m.normal);
    if (planar)
        projectToPlane(chart, m.normal);

    // Mirrored islands would put the trim on backwards.
    if (chart.areaUv() < 0.0)
        for (glm::dvec2& p : chart.uv)
            p.x = -p.x;

    const glm::dvec3 up = glm::length(opts.up) > 0.0 ? glm::normalize(opts.up) : glm::dvec3(0.0, 1.0, 0.0);
    const bool flat = std::abs(glm::dot(m.normal, up)) > std::cos(glm::radians(opts.flatTolerance));
    m.radial = looksRadial(chart);
    if (!m.radial)
    {
        std::vector<glm::dvec2> pts(chart.uv.begin(), chart.uv.end());
        glm::dvec2 box(0.0);
        const double angle = minAreaRectAngle(pts, &box);
        const bool boxy = box.x * box.y > 0.0 && std::abs(chart.areaUv()) / (box.x * box.y) > kRectifyFill;
        Chart straight = chart;
        m.rectified = planar ? StraightenResult::Failed : straightenChart(straight);
        // Pulling the outline onto a rectangle warps an arch or a stair profile; those keep their unwrapped shape.
        if (m.rectified == StraightenResult::Rectangle && !boxy)
            m.rectified = StraightenResult::Failed;
        if (m.rectified != StraightenResult::Failed)
            chart = std::move(straight);
        else if (opts.orient == HotspotOrient::WorldUp && !flat)
        {
            // A ramp's side has no box edge to follow (its tightest box lies along the slope), so up runs straight up.
            const glm::dvec2 g = upGradient(chart, up);
            if (glm::length(g) > 0.0)
                rotateUv(chart, std::numbers::pi * 0.5 - std::atan2(g.y, g.x));
        }
        else
            rotateUv(chart, -angle);
    }

    HotspotOrient orient = opts.orient;
    if (orient == HotspotOrient::WorldUp && (flat || m.radial))
        orient = HotspotOrient::LongestU;
    if (orient == HotspotOrient::WorldUp)
    {
        const glm::dvec2 g = upGradient(chart, up);
        // The quarter turn that brings the up gradient closest to +V.
        int best = 0;
        double bestDot = -1e300;
        for (int k = 0; k < 4; ++k)
        {
            const glm::dvec2 r = k == 0 ? g : k == 1 ? glm::dvec2(-g.y, g.x) : k == 2 ? -g : glm::dvec2(g.y, -g.x);
            if (r.y > bestDot)
            {
                bestDot = r.y;
                best = k;
            }
        }
        quarterTurns(chart, best);
    }
    else if (orient == HotspotOrient::LongestU)
    {
        const glm::dvec2 size = chart.uvMax() - chart.uvMin();
        if (size.y > size.x)
            quarterTurns(chart, 1);
    }

    const double areaUv = std::abs(chart.areaUv());
    if (areaUv > 0.0)
    {
        const double s = std::sqrt(m.area3d / areaUv);
        for (glm::dvec2& p : chart.uv)
            p *= s;
    }
    const glm::dvec2 size = chart.uvMax() - chart.uvMin();
    m.width = size.x;
    m.height = size.y;
    m.fill = size.x > 0.0 && size.y > 0.0 ? std::abs(chart.areaUv()) / (size.x * size.y) : 0.0;
    return m;
}

std::vector<TrimCandidate> rankTrims(const IslandMeasure& island, std::span<const HotspotTrim> trims, const HotspotOptions& opts)
{
    std::vector<TrimCandidate> out;
    if (island.width <= 0.0 || island.height <= 0.0)
        return out;

    auto usable = [&](const HotspotTrim& t) {
        if (t.reserved)
            return false;
        if (!opts.tag.empty())
            return hasTag(t, opts.tag);
        return t.kind != TrimKind::Unique && t.kind != TrimKind::Flat;
    };
    const bool anyRadial = std::ranges::any_of(trims, [&](const HotspotTrim& t) { return t.kind == TrimKind::Radial && usable(t); });
    const bool wantRadial = island.radial && anyRadial;
    const bool keepsUp = opts.orient == HotspotOrient::WorldUp;

    for (size_t i = 0; i < trims.size(); ++i)
    {
        const HotspotTrim& t = trims[i];
        if (!usable(t) || (t.kind == TrimKind::Radial) != wantRadial)
            continue;
        const double tw = std::max(t.worldSize.x, 1e-9), th = std::max(t.worldSize.y, 1e-9);
        TrimCandidate best;
        best.score = 1e300;
        for (int r = 0; r < 2; ++r)
        {
            if (r == 1 && !(opts.allowRotate && t.allowRotate))
                break;
            const double w = r ? island.height : island.width, h = r ? island.width : island.height;
            double aspect = 0.0, size = 0.0;
            switch (t.kind)
            {
            case TrimKind::StripU:
                size = std::abs(std::log2(h / th));
                // Tiling strips suit long islands; a tall one only reaches them by squashing.
                aspect = 0.5 * std::max(0.0, std::log2(h / w));
                break;
            case TrimKind::StripV:
                size = std::abs(std::log2(w / tw));
                aspect = 0.5 * std::max(0.0, std::log2(w / h));
                break;
            default:
                aspect = std::abs(std::log2((w / h) / (tw / th)));
                size = std::abs(std::log2(std::sqrt(w * h) / std::sqrt(tw * th)));
                break;
            }
            double facing = 0.0;
            if (glm::length(t.normal) > 0.0 && glm::length(island.normal) > 0.0)
                facing = 0.5 * (1.0 - glm::dot(glm::normalize(t.normal), island.normal));
            double score = opts.aspectWeight * aspect + opts.sizeWeight * size + opts.normalWeight * facing;
            // A quarter turn undoes world-up orientation, so it has to win clearly.
            if (r == 1 && keepsUp && !island.radial)
                score += 0.25;
            if (score < best.score)
                best = {int(i), score, size, r == 1};
        }
        if (best.trim >= 0)
            out.push_back(best);
    }
    std::ranges::stable_sort(out, [&](const TrimCandidate& a, const TrimCandidate& b) {
        const bool ra = a.sizeError > opts.maxSizeError, rb = b.sizeError > opts.maxSizeError;
        if (ra != rb)
            return !ra;
        return a.score < b.score;
    });
    return out;
}

void placeChart(Chart& chart, const HotspotTrim& t, bool rotate, const HotspotOptions& opts, double rand0, double rand1, double rand2)
{
    if (chart.uv.empty())
        return;
    if (rotate)
        quarterTurns(chart, 1);
    if (opts.randomFlip && t.allowRotate && rand1 < 0.5)
        quarterTurns(chart, 2);
    if (opts.randomMirror && rand2 < 0.5)
        for (glm::dvec2& p : chart.uv)
            p.x = -p.x;

    const glm::dvec2 lo = chart.uvMin(), hi = chart.uvMax();
    const glm::dvec2 size = glm::max(hi - lo, glm::dvec2(1e-12));
    const glm::dvec2 rmin = t.min + glm::dvec2(t.inset), rmax = glm::max(t.max - glm::dvec2(t.inset), rmin + glm::dvec2(1e-9));
    const glm::dvec2 room = rmax - rmin;

    TrimFit fit = t.fit;
    TrimKind kind = t.kind;
    if (kind == TrimKind::StripU || kind == TrimKind::StripV)
        fit = TrimFit::Seamless;
    else if (fit == TrimFit::Seamless)
        kind = size.x >= size.y ? TrimKind::StripU : TrimKind::StripV;

    glm::dvec2 scale(1.0), origin(rmin);
    if (fit == TrimFit::Fill)
        scale = room / size;
    else if (fit == TrimFit::Seamless && kind == TrimKind::StripU)
    {
        scale = glm::dvec2(room.y / size.y);
        // Unslid islands sit centred on the strip, so an end face shows the motif in the middle and a long face is symmetric.
        const double centred = (t.min.x + t.max.x - size.x * scale.x) * 0.5;
        origin = glm::dvec2(opts.randomOffset ? rmin.x + rand0 * (t.max.x - t.min.x) : centred, rmin.y);
    }
    else if (fit == TrimFit::Seamless && kind == TrimKind::StripV)
    {
        scale = glm::dvec2(room.x / size.x);
        const double centred = (t.min.y + t.max.y - size.y * scale.y) * 0.5;
        origin = glm::dvec2(rmin.x, opts.randomOffset ? rmin.y + rand0 * (t.max.y - t.min.y) : centred);
    }
    else
    {
        scale = glm::dvec2(std::min(room.x / size.x, room.y / size.y));
        origin = rmin + (room - size * scale) * 0.5;
    }
    for (glm::dvec2& p : chart.uv)
        p = (p - lo) * scale + origin;
}

Index markSeamsByAngle(geo::Mesh& mesh, std::span<const Index> faces, double degrees)
{
    LILA_ZONE();

    const std::vector<Index> live = liveFaces(mesh, faces);
    std::vector<uint8_t> in(mesh.faceCapacity(), 0);
    for (Index f : live)
        in[f] = 1;
    std::span<bool> seam = mesh.edgeAttrs().getOrAdd<bool>(geo::attr::kSeam);
    Index added = 0;
    for (Index f : live)
        mesh.forEachFaceCorner(f, [&](Index c) {
            const Index e = mesh.cornerEdge(c);
            if (seam[e])
                return;
            bool cut = !mesh.isManifoldEdge(e);
            if (!cut)
            {
                const Index other = mesh.oppositeFace(e, f);
                cut = other == kInvalid || !in[other] || (degrees > 0.0 && dihedralAngle(mesh, e) > degrees);
            }
            if (cut)
            {
                seam[e] = true;
                ++added;
            }
        });
    return added;
}

namespace
{
/** World height of the tallest tiling strip the matcher may pick, 0 for none. */
double tallestStrip(std::span<const HotspotTrim> trims, const HotspotOptions& opts)
{
    double h = 0.0;
    for (const HotspotTrim& t : trims)
        if (t.kind == TrimKind::StripU && !t.reserved && (opts.tag.empty() || std::ranges::find(t.tags, opts.tag) != t.tags.end()))
            h = std::max(h, t.worldSize.y);
    return h;
}

/**
 * Cuts the islands taller than any strip into courses of about the strip's height along their measured V (or U when the
 * matcher would turn them), marking the cuts as seams. Returns how many islands it cut; newFaces gets the faces it made.
 */
size_t sliceOversizedCharts(geo::Mesh& mesh, const std::vector<Chart>& charts, std::span<const HotspotTrim> trims, const HotspotOptions& opts,
                            std::vector<Index>& newFaces)
{
    const double strip = tallestStrip(trims, opts);
    if (strip <= 0.0)
        return 0;
    const double limit = strip * std::exp2(opts.maxSizeError);
    struct Cut
    {
        std::vector<Index> faces;
        glm::dvec3 origin, normal;
        std::vector<double> at;
    };
    std::vector<Cut> cuts(charts.size());
    tbb::parallel_for(size_t(0), charts.size(), size_t(1), [&](size_t i) {
        Chart ch = charts[i];
        const IslandMeasure m = measureChart(ch, opts);
        if (m.radial)
            return;
        const bool acrossU = opts.allowRotate && m.width < m.height;
        const double across = acrossU ? m.width : m.height;
        if (across <= limit)
            return;
        // The measured uvs are in cm, so the gradient of the across coordinate is a unit direction in 3D.
        glm::dvec3 grad{0.0};
        for (const auto& t : ch.tris)
        {
            const glm::dvec3 e1 = ch.pos[t[1]] - ch.pos[t[0]], e2 = ch.pos[t[2]] - ch.pos[t[0]];
            const glm::dvec3 n = glm::cross(e1, e2);
            const double nn = glm::dot(n, n);
            if (nn <= 1e-12)
                continue;
            const int k = acrossU ? 0 : 1;
            const double s1 = ch.uv[t[1]][k] - ch.uv[t[0]][k], s2 = ch.uv[t[2]][k] - ch.uv[t[0]][k];
            grad += (s1 * glm::cross(e2, n) + s2 * glm::cross(n, e1)) / nn * std::sqrt(nn);
        }
        const double g = glm::length(grad);
        if (g <= 1e-9)
            return;
        grad /= g;
        const int k = acrossU ? 0 : 1;
        double lo = ch.uv[0][k], hi = lo;
        for (const glm::dvec2& u : ch.uv)
        {
            lo = std::min(lo, u[k]);
            hi = std::max(hi, u[k]);
        }
        const int courses = std::max(2, int(std::lround(across / strip)));
        Cut& c = cuts[i];
        c.faces = ch.faces;
        c.normal = grad;
        c.origin = ch.pos[0] - grad * (ch.uv[0][k] - lo);
        for (int j = 1; j < courses; ++j)
            c.at.push_back((hi - lo) * double(j) / courses);
    });
    size_t sliced = 0;
    for (Cut& c : cuts)
    {
        if (c.at.empty())
            continue;
        ++sliced;
        std::vector<Index> faces = c.faces;
        for (double at : c.at)
        {
            const geo::PlaneCut r = geo::cutAlongPlane(mesh, c.origin + c.normal * at, c.normal, 1e-4, {}, faces);
            std::span<bool> seam = mesh.edgeAttrs().getOrAdd<bool>(geo::attr::kSeam);
            for (Index e : r.edges)
                seam[e] = true;
            faces.insert(faces.end(), r.newFaces.begin(), r.newFaces.end());
            newFaces.insert(newFaces.end(), r.newFaces.begin(), r.newFaces.end());
        }
    }
    return sliced;
}
/**
 * Each measured chart's band leader: charts sharing an edge that runs from the bottom to the top of both (walls round a
 * corner, and the bevel faces between them) are one band, led by its largest chart, so they take one trim and chain.
 */
std::vector<size_t> bandLeaders(const geo::Mesh& mesh, std::span<const Chart> charts)
{
    constexpr size_t kNone = ~size_t(0);
    constexpr double kTol = 0.05;
    std::vector<size_t> chartOf(mesh.faceCapacity(), kNone);
    std::vector<std::unordered_map<Index, glm::dvec2>> uvAt(charts.size());
    for (size_t i = 0; i < charts.size(); ++i)
    {
        for (Index f : charts[i].faces)
            chartOf[f] = i;
        for (size_t v = 0; v < charts[i].uv.size(); ++v)
            uvAt[i].emplace(charts[i].vertexPoint[v], charts[i].uv[v]);
    }
    std::vector<size_t> root(charts.size());
    for (size_t i = 0; i < root.size(); ++i)
        root[i] = i;
    auto find = [&](size_t i) {
        while (root[i] != i)
            i = root[i] = root[root[i]];
        return i;
    };
    auto spansHeight = [&](size_t i, Index p0, Index p1) {
        const auto a = uvAt[i].find(p0), b = uvAt[i].find(p1);
        if (a == uvAt[i].end() || b == uvAt[i].end() || charts[i].uv.empty())
            return false;
        const double h = charts[i].uvMax().y - charts[i].uvMin().y;
        return h > kTol && std::abs(a->second.x - b->second.x) < kTol && std::abs(a->second.y - b->second.y) > h - kTol;
    };
    for (size_t i = 0; i < charts.size(); ++i)
        for (Index f : charts[i].faces)
            mesh.forEachFaceCorner(f, [&](Index c) {
                const Index e = mesh.cornerEdge(c);
                const Index o = mesh.oppositeFace(e, f);
                if (o == kInvalid || o >= chartOf.size() || chartOf[o] == kNone || chartOf[o] <= i)
                    return;
                const size_t j = chartOf[o];
                const Index p0 = mesh.edgePoint(e, 0), p1 = mesh.edgePoint(e, 1);
                if (spansHeight(i, p0, p1) && spansHeight(j, p0, p1))
                    root[find(j)] = find(i);
            });
    std::vector<size_t> leader(charts.size(), kNone);
    for (size_t i = 0; i < charts.size(); ++i)
    {
        size_t& l = leader[find(i)];
        if (l == kNone || charts[i].area3d() > charts[l].area3d() + 1e-9)
            l = i;
    }
    std::vector<size_t> out(charts.size());
    for (size_t i = 0; i < charts.size(); ++i)
        out[i] = leader[find(i)];
    return out;
}
} // namespace

double startOnWholeSheets(Chart& chart, const HotspotTrim& t)
{
    if (chart.uv.empty())
        return 1.0;
    const double sheet = t.max.x - t.min.x;
    const double lo = chart.uvMin().x, span = chart.uvMax().x - lo;
    if (sheet <= 0.0 || span <= 0.0)
        return 1.0;
    const double k = std::max(1.0, std::round(span / sheet)) * sheet / span;
    // Too short a run would be stretched out of shape; it keeps its scale and only starts on the sheet's edge.
    const double used = std::abs(k - 1.0) <= kMaxWholeSheetStretch ? k : 1.0;
    for (glm::dvec2& p : chart.uv)
        p.x = t.min.x + (p.x - lo) * used;
    return used;
}

size_t chainStripCharts(std::span<Chart> charts, std::span<const int> trimOf, std::span<const HotspotTrim> trims,
                        bool wholeSheets, const std::function<void(Chart&, int trim)>& align)
{
    LILA_ZONE();

    constexpr double kEps = 1e-5;
    constexpr size_t kNone = ~size_t(0);
    auto onStrip = [&](size_t i) {
        const int t = trimOf[i];
        return t >= 0 && size_t(t) < trims.size() && trims[size_t(t)].kind == TrimKind::StripU && !charts[i].uv.empty();
    };
    struct End
    {
        size_t chart;
        double v;
    };
    // The mesh points on each strip chart's low-U end, with their v.
    std::unordered_map<Index, std::vector<End>> lowEnds;
    for (size_t i = 0; i < charts.size(); ++i)
        if (onStrip(i))
        {
            const Chart& ch = charts[i];
            const double lo = ch.uvMin().x;
            for (size_t v = 0; v < ch.uv.size(); ++v)
                if (ch.uv[v].x < lo + kEps)
                    lowEnds[ch.vertexPoint[v]].push_back({i, ch.uv[v].y});
        }
    // next[i] is the chart on the same trim whose low end shares at least two points, at the same v, with i's high end.
    std::vector<size_t> next(charts.size(), kNone), prev(charts.size(), kNone);
    for (size_t i = 0; i < charts.size(); ++i)
    {
        if (!onStrip(i))
            continue;
        const Chart& ch = charts[i];
        const double hi = ch.uvMax().x;
        std::map<size_t, int> shared;
        for (size_t v = 0; v < ch.uv.size(); ++v)
            if (ch.uv[v].x > hi - kEps)
                if (auto it = lowEnds.find(ch.vertexPoint[v]); it != lowEnds.end())
                    for (const End& e : it->second)
                        if (e.chart != i && trimOf[e.chart] == trimOf[i] && std::abs(e.v - ch.uv[v].y) < kEps)
                            ++shared[e.chart];
        for (const auto& [j, n] : shared)
            if (n >= 2 && prev[j] == kNone)
            {
                next[i] = j;
                prev[j] = i;
                break;
            }
    }

    auto shift = [&](size_t i, double du) {
        for (glm::dvec2& p : charts[i].uv)
            p.x += du;
    };
    auto scaleU = [&](size_t i, double k) {
        const double lo = charts[i].uvMin().x;
        for (glm::dvec2& p : charts[i].uv)
            p.x = lo + (p.x - lo) * k;
    };
    auto length = [&](size_t i) { return charts[i].uv.empty() ? 0.0 : charts[i].uvMax().x - charts[i].uvMin().x; };
    std::vector<size_t> order(charts.size());
    for (size_t i = 0; i < order.size(); ++i)
        order[i] = i;
    std::ranges::stable_sort(order, [&](size_t a, size_t b) { return length(a) > length(b); });
    std::vector<uint8_t> done(charts.size(), 0);
    size_t moved = 0;
    for (size_t root : order)
    {
        if (done[root])
            continue;
        done[root] = 1;
        double k = 1.0;
        if (wholeSheets && onStrip(root))
            k = startOnWholeSheets(charts[root], trims[size_t(trimOf[root])]);
        else if (align && trimOf[root] >= 0)
            align(charts[root], trimOf[root]);
        // Alternating steps, so a closed loop's seam lands opposite the root.
        for (size_t fwd = root, back = root; fwd != kNone || back != kNone;)
        {
            if (fwd != kNone)
            {
                const size_t j = next[fwd];
                fwd = j != kNone && !done[j] ? j : kNone;
                if (fwd != kNone)
                {
                    scaleU(j, k);
                    shift(j, charts[prev[j]].uvMax().x - charts[j].uvMin().x);
                    done[j] = 1;
                    ++moved;
                }
            }
            if (back != kNone)
            {
                const size_t j = prev[back];
                back = j != kNone && !done[j] ? j : kNone;
                if (back != kNone)
                {
                    scaleU(j, k);
                    shift(j, charts[next[j]].uvMin().x - charts[j].uvMax().x);
                    done[j] = 1;
                    ++moved;
                }
            }
        }
    }
    return moved;
}

HotspotResult hotspot(geo::Mesh& mesh, std::span<const Index> faceIds, std::span<const HotspotTrim> trims, std::string_view uvSet,
                      const HotspotOptions& opts)
{
    LILA_ZONE();

    HotspotResult res;
    const std::vector<Index> faces = liveFaces(mesh, faceIds);
    if (faces.empty() || trims.empty())
        return res;
    if (opts.splitAngle > 0.0)
        res.seamsAdded = markSeamsByAngle(mesh, faces, opts.splitAngle);

    UnwrapFacesOptions uo;
    uo.keepPlacement = false;
    uo.pack = false;
    uo.unwrap.usePins = false;
    res.seamsAdded += unwrapFaces(mesh, faces, uvSet, uo).seamsAdded;

    ChartOptions co;
    co.uvSet = uvSet;
    co.pinAttr = {};
    std::vector<Chart> charts = buildCharts(mesh, faces, co);
    if (opts.sliceOversized)
    {
        std::vector<Index> made;
        if (const size_t sliced = sliceOversizedCharts(mesh, charts, trims, opts, made))
        {
            HotspotOptions again = opts;
            again.sliceOversized = false;
            std::vector<Index> all = faces;
            all.insert(all.end(), made.begin(), made.end());
            HotspotResult r = hotspot(mesh, all, trims, uvSet, again);
            r.seamsAdded += res.seamsAdded;
            r.sliced = sliced;
            return r;
        }
    }
    res.islands.resize(charts.size());

    std::vector<std::vector<TrimCandidate>> ranked(charts.size());
    tbb::parallel_for(size_t(0), charts.size(), size_t(1), [&](size_t i) {
        HotspotIsland& out = res.islands[i];
        out.faces = charts[i].faces;
        out.measure = measureChart(charts[i], opts);
        ranked[i] = rankTrims(out.measure, trims, opts);
    });

    // The island's first face keys its randomness, so it does not depend on the other islands; a band shares its leader's.
    const std::vector<size_t> leaders = bandLeaders(mesh, charts);
    auto keyOf = [&](size_t i) { return charts[leaders[i]].faces.empty() ? uint64_t(leaders[i]) : uint64_t(charts[leaders[i]].faces.front()); };
    auto ownPick = [&](size_t i) {
        const std::vector<TrimCandidate>& r = ranked[i];
        if (opts.cycle > 0)
            return size_t(opts.cycle) % r.size();
        std::vector<size_t> pool;
        const TrimCandidate& best = r.front();
        const int group = trims[size_t(best.trim)].altGroup;
        for (size_t k = 0; k < r.size(); ++k)
        {
            const TrimCandidate& c = r[k];
            const bool tied = c.score <= best.score + opts.tieTolerance && (c.sizeError > opts.maxSizeError) == (best.sizeError > opts.maxSizeError);
            // Alt groups swap interchangeable trims: same group and the same world size.
            const HotspotTrim& a = trims[size_t(c.trim)];
            const HotspotTrim& b = trims[size_t(best.trim)];
            const bool sameGroup = group >= 0 && a.altGroup == group && glm::length(a.worldSize - b.worldSize) <= 1e-3 * glm::length(b.worldSize) &&
                                   c.rotated == best.rotated;
            if (tied || sameGroup)
                pool.push_back(k);
        }
        return pool[std::min(pool.size() - 1, size_t(hotspotRandom(opts.seed, keyOf(i), 0) * double(pool.size())))];
    };
    constexpr size_t kNoPick = ~size_t(0);
    std::vector<size_t> pick(charts.size(), kNoPick);
    for (size_t i = 0; i < charts.size(); ++i)
        if (leaders[i] == i && !ranked[i].empty())
            pick[i] = ownPick(i);
    for (size_t i = 0; i < charts.size(); ++i)
    {
        if (leaders[i] == i || ranked[i].empty())
            continue;
        const size_t l = leaders[i];
        if (pick[l] != kNoPick)
        {
            const TrimCandidate& lead = ranked[l][pick[l]];
            for (size_t k = 0; k < ranked[i].size() && pick[i] == kNoPick; ++k)
                if (ranked[i][k].trim == lead.trim && ranked[i][k].rotated == lead.rotated && (ranked[i][k].sizeError <= opts.maxSizeError || lead.sizeError > opts.maxSizeError))
                    pick[i] = k;
        }
        if (pick[i] == kNoPick)
            pick[i] = ownPick(i);
    }

    tbb::parallel_for(size_t(0), charts.size(), size_t(1), [&](size_t i) {
        if (pick[i] == kNoPick)
            return;
        HotspotIsland& out = res.islands[i];
        const TrimCandidate& c = ranked[i][pick[i]];
        const uint64_t key = keyOf(i);
        placeChart(charts[i], trims[size_t(c.trim)], c.rotated, opts, hotspotRandom(opts.seed, key, 1), hotspotRandom(opts.seed, key, 2),
                   hotspotRandom(opts.seed, key, 3));
        out.trim = c.trim;
        out.score = c.score;
        out.sizeError = c.sizeError;
        out.rotated = c.rotated;
        out.poor = c.sizeError > opts.maxSizeError;
    });

    std::vector<int> trimOf(charts.size());
    for (size_t i = 0; i < charts.size(); ++i)
        trimOf[i] = res.islands[i].trim;
    chainStripCharts(charts, trimOf, trims, !opts.randomOffset);
    writeCharts(mesh, charts, uvSet);
    for (const HotspotIsland& is : res.islands)
    {
        if (is.trim < 0)
            ++res.failed;
        else
        {
            ++res.matched;
            if (is.poor)
                ++res.poor;
        }
    }
    return res;
}
} // namespace lila::uv
