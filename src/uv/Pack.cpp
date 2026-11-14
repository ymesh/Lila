#include "uv/Pack.h"

#include "core/Profile.h"
#include "core/Progress.h"
#include "uv/Flow.h"
#include "uv/Layout.h"
#include "uv/Unwrap.h"
#include "uv/UvCommon.h"

#include <tbb/parallel_for.h>

#include <glm/gtc/constants.hpp>

#include <algorithm>
#include <atomic>
#include <bit>
#include <chrono>
#include <cmath>
#include <functional>
#include <map>
#include <numeric>
#include <optional>
#include <set>
#include <thread>

namespace lila::uv
{
namespace
{
using detail::Tri2;
using Clock = std::chrono::steady_clock;

/** Cells covered by a shape, as sorted, merged [x0, x1) spans per row. */
struct Spans
{
    int w = 0, h = 0;
    std::vector<uint32_t> rowStart; // h + 1
    std::vector<glm::ivec2> spans;
};

void mergeRows(Spans& m, std::vector<glm::ivec3>& raw)
{
    std::sort(raw.begin(), raw.end(), [](const glm::ivec3& a, const glm::ivec3& b) { return a.x < b.x || (a.x == b.x && a.y < b.y); });
    m.rowStart.assign(size_t(m.h) + 1, 0);
    m.spans.clear();
    size_t i = 0;
    for (int y = 0; y < m.h; ++y)
    {
        m.rowStart[y] = uint32_t(m.spans.size());
        while (i < raw.size() && raw[i].x == y)
        {
            glm::ivec2 sp(raw[i].y, raw[i].z);
            ++i;
            while (i < raw.size() && raw[i].x == y && raw[i].y <= sp.y)
            {
                sp.y = std::max(sp.y, raw[i].z);
                ++i;
            }
            m.spans.push_back(sp);
        }
    }
    m.rowStart[m.h] = uint32_t(m.spans.size());
}

/** x range of the triangle within the slab y0 <= y <= y1. */
bool slabRange(const Tri2& t, double y0, double y1, double& x0, double& x1)
{
    x0 = std::numeric_limits<double>::max();
    x1 = -x0;
    bool any = false;
    auto take = [&](double x) {
        x0 = std::min(x0, x);
        x1 = std::max(x1, x);
        any = true;
    };
    for (int i = 0; i < 3; ++i)
    {
        const glm::dvec2& a = t[i];
        const glm::dvec2& b = t[(i + 1) % 3];
        if (a.y >= y0 && a.y <= y1)
            take(a.x);
        for (double yy : {y0, y1})
            if ((a.y - yy) * (b.y - yy) < 0.0)
                take(a.x + (b.x - a.x) * (yy - a.y) / (b.y - a.y));
    }
    return any;
}

/**
 * Conservative raster of triangles given in cell units: every point of every triangle lies in a closed cell that is
 * set. Cells outside w x h are dropped.
 */
void rasterTris(std::span<const Tri2> tris, Spans& out)
{
    std::vector<glm::ivec3> raw;
    raw.reserve(tris.size() * 4);
    for (const Tri2& t : tris)
    {
        double ty0 = std::min({t[0].y, t[1].y, t[2].y}), ty1 = std::max({t[0].y, t[1].y, t[2].y});
        double tx0 = std::min({t[0].x, t[1].x, t[2].x}), tx1 = std::max({t[0].x, t[1].x, t[2].x});
        if (ty1 < 0.0 || ty0 > double(out.h) || tx1 < 0.0 || tx0 > double(out.w))
            continue;
        int r0 = std::clamp(int(std::floor(ty0)), 0, out.h - 1);
        int r1 = std::clamp(int(std::ceil(ty1)) - 1, r0, out.h - 1);
        for (int r = r0; r <= r1; ++r)
        {
            double x0, x1;
            if (!slabRange(t, double(r), double(r + 1), x0, x1) || x1 < 0.0 || x0 > double(out.w))
                continue;
            int a = std::clamp(int(std::floor(x0)), 0, out.w - 1);
            int b = std::clamp(int(std::ceil(x1)), a + 1, out.w);
            raw.emplace_back(r, a, b);
        }
    }
    mergeRows(out, raw);
}

/**
 * Grows the cells by every offset whose cell gap is under pd cells, so a shape's raw cells kept off the result lie at
 * least pd cells (Euclidean) from any point of this one. The result's corner sits `off` cells below and left.
 */
Spans grow(const Spans& s, double pd, glm::ivec2& off)
{
    off = glm::ivec2(0);
    if (pd <= 0.0)
        return s;
    std::vector<int> hw;
    for (int k = 0;; ++k)
    {
        double gy = std::max(0, k - 1);
        if (gy >= pd)
            break;
        hw.push_back(int(std::ceil(std::sqrt(pd * pd - gy * gy) - 1e-12)));
    }
    const int R = int(hw.size()) - 1, H = hw[0];
    off = glm::ivec2(H, R);
    Spans g;
    g.w = s.w + 2 * H;
    g.h = s.h + 2 * R;
    std::vector<glm::ivec3> raw;
    raw.reserve(s.spans.size() * (2 * R + 1));
    for (int y = 0; y < s.h; ++y)
        for (uint32_t i = s.rowStart[y]; i < s.rowStart[y + 1]; ++i)
            for (int dy = -R; dy <= R; ++dy)
            {
                int d = hw[size_t(std::abs(dy))];
                raw.emplace_back(y + dy + R, s.spans[i].x + H - d, s.spans[i].y + H + d);
            }
    mergeRows(g, raw);
    return g;
}

/** Occupancy of one packing region: a bit per cell, set when taken (islands with their padding, margin, obstacles). */
struct Bin
{
    int w = 0, h = 0, words = 0;
    std::vector<uint64_t> occ;
    /** Longest free run per row, to skip rows quickly. */
    std::vector<int> maxRun;
    /** Free runs per row as [x0, x1), left to right. */
    std::vector<std::vector<glm::ivec2>> runs;

    void init(int width, int height, int margin)
    {
        w = std::max(1, width);
        h = std::max(1, height);
        words = (w + 63) / 64;
        occ.assign(size_t(h) * words, 0);
        for (int y = 0; y < h; ++y)
        {
            // Bits past the right edge count as taken, so scans stop there.
            if (w % 64)
                occ[size_t(y) * words + words - 1] |= ~uint64_t(0) << (w % 64);
            if (y < margin || y >= h - margin)
                fill(y, 0, w);
            else if (margin > 0)
            {
                fill(y, 0, margin);
                fill(y, w - margin, w);
            }
        }
        maxRun.resize(h);
        runs.resize(h);
        for (int y = 0; y < h; ++y)
            updateRow(y);
    }

    const uint64_t* row(int y) const { return &occ[size_t(y) * words]; }

    void fill(int y, int a, int b)
    {
        a = std::max(a, 0);
        b = std::min(b, w);
        uint64_t* r = &occ[size_t(y) * words];
        while (a < b)
        {
            int wi = a >> 6, lo = a & 63, n = std::min(64 - lo, b - a);
            uint64_t bits = n == 64 ? ~uint64_t(0) : ((uint64_t(1) << n) - 1) << lo;
            r[wi] |= bits;
            a += n;
        }
    }

    /** First taken cell in [a, b) of row y, or -1. */
    int firstSet(int y, int a, int b) const
    {
        if (a >= b)
            return -1;
        const uint64_t* r = row(y);
        int wi = a >> 6;
        uint64_t word = r[wi] & (~uint64_t(0) << (a & 63));
        for (;;)
        {
            if (word)
            {
                int p = wi * 64 + std::countr_zero(word);
                return p < b ? p : -1;
            }
            if (++wi * 64 >= b || wi >= words)
                return -1;
            word = r[wi];
        }
    }

    /** First free cell at or after a in row y, or w. */
    int firstClear(int y, int a) const
    {
        if (a >= w)
            return w;
        const uint64_t* r = row(y);
        int wi = a >> 6;
        uint64_t word = ~r[wi] & (~uint64_t(0) << (a & 63));
        for (;;)
        {
            if (word)
                return std::min(w, wi * 64 + std::countr_zero(word));
            if (++wi >= words)
                return w;
            word = ~r[wi];
        }
    }

    void updateRow(int y)
    {
        std::vector<glm::ivec2>& r = runs[y];
        r.clear();
        int best = 0, x = firstClear(y, 0);
        while (x < w)
        {
            int e = firstSet(y, x, w);
            if (e < 0)
                e = w;
            r.emplace_back(x, e);
            best = std::max(best, e - x);
            x = firstClear(y, e);
        }
        maxRun[y] = best;
    }

    /** ORs shape cells in with the shape's corner at (x, y); cells outside are dropped. */
    void place(const Spans& s, int x, int y)
    {
        for (int r = 0; r < s.h; ++r)
        {
            int ty = y + r;
            if (ty < 0 || ty >= h || s.rowStart[r] == s.rowStart[r + 1])
                continue;
            for (uint32_t i = s.rowStart[r]; i < s.rowStart[r + 1]; ++i)
                fill(ty, x + s.spans[i].x, x + s.spans[i].y);
            updateRow(ty);
        }
    }

    size_t freeCells() const
    {
        size_t n = 0;
        for (uint64_t v : occ)
            n += size_t(64 - std::popcount(v));
        return n;
    }
};

/** One rotation (and mirror) of an island at one scale. */
struct Mask
{
    bool valid = false;
    Spans raw, grown;
    glm::ivec2 growOff{0};
    glm::dvec2 origin{0.0}; // cell position of the raster's corner, in transformed island space
    std::vector<int> need;  // widest span per row
    int wideRow = 0;
    glm::ivec2 wideSpan{0}; // the widest span, in wideRow
    std::vector<int> checkOrder; // rows by need, widest first
};

struct Orientation
{
    double angle = 0.0;
    bool flip = false;
};

glm::dvec2 orient(glm::dvec2 p, const Orientation& o, double c, double s)
{
    if (o.flip)
        p.x = -p.x;
    return glm::dvec2(p.x * c - p.y * s, p.x * s + p.y * c);
}

struct Island
{
    size_t chart = 0;
    std::vector<Tri2> tris;
    double area = 0.0;
    glm::dvec2 size{0.0};
    std::vector<Orientation> orients;
    std::vector<int> bins; // bins it may go to, in order of preference
};

/** Corner of the island's raster: computed exactly as makeMask does, so placed cells line up with the search. */
glm::dvec2 maskOrigin(const Island& isl, const Orientation& o, double scale)
{
    double c = std::cos(o.angle), s = std::sin(o.angle);
    glm::dvec2 lo(std::numeric_limits<double>::max());
    for (const Tri2& t : isl.tris)
        for (const glm::dvec2& p : t)
            lo = glm::min(lo, orient(p, o, c, s) * scale);
    return glm::floor(lo);
}

Mask makeMask(const Island& isl, const Orientation& o, double scale, int maxW, int maxH, double pd)
{
    Mask m;
    double c = std::cos(o.angle), s = std::sin(o.angle);
    std::vector<Tri2> q(isl.tris.size());
    glm::dvec2 lo(std::numeric_limits<double>::max()), hi(-std::numeric_limits<double>::max());
    for (size_t i = 0; i < isl.tris.size(); ++i)
        for (int k = 0; k < 3; ++k)
        {
            q[i][k] = orient(isl.tris[i][k], o, c, s) * scale;
            lo = glm::min(lo, q[i][k]);
            hi = glm::max(hi, q[i][k]);
        }
    if (q.empty())
        return m;
    m.origin = glm::floor(lo);
    m.raw.w = std::max(1, int(std::ceil(hi.x - m.origin.x)));
    m.raw.h = std::max(1, int(std::ceil(hi.y - m.origin.y)));
    if (m.raw.w > maxW || m.raw.h > maxH)
        return m;
    for (Tri2& t : q)
        for (glm::dvec2& p : t)
            p -= m.origin;
    rasterTris(q, m.raw);
    m.grown = grow(m.raw, pd, m.growOff);
    m.need.assign(m.raw.h, 0);
    int widest = -1;
    for (int r = 0; r < m.raw.h; ++r)
        for (uint32_t i = m.raw.rowStart[r]; i < m.raw.rowStart[r + 1]; ++i)
        {
            const glm::ivec2& sp = m.raw.spans[i];
            m.need[r] = std::max(m.need[r], sp.y - sp.x);
            if (sp.y - sp.x > widest)
            {
                widest = sp.y - sp.x;
                m.wideRow = r;
                m.wideSpan = sp;
            }
        }
    m.checkOrder.resize(m.raw.h);
    std::iota(m.checkOrder.begin(), m.checkOrder.end(), 0);
    std::stable_sort(m.checkOrder.begin(), m.checkOrder.end(), [&](int a, int b) { return m.need[a] > m.need[b]; });
    m.valid = true;
    return m;
}

bool sameCells(const Mask& a, const Mask& b)
{
    return a.raw.w == b.raw.w && a.raw.h == b.raw.h && a.raw.rowStart == b.raw.rowStart && a.raw.spans == b.raw.spans;
}

/** Lowest, then leftmost spot for the mask with y at most yMax. */
bool findSpot(const Bin& bin, const Mask& m, int yMax, glm::ivec2& at)
{
    const Spans& s = m.raw;
    const int xMax = bin.w - s.w;
    yMax = std::min(yMax, bin.h - s.h);
    if (xMax < 0)
        return false;
    int startRow = m.wideRow;
    for (int y = 0; y <= yMax; ++y)
    {
        if (bin.maxRun[y + m.wideRow] < m.need[m.wideRow])
            continue;
        bool rowsOk = true;
        for (size_t k = 0; k < m.checkOrder.size() && rowsOk; ++k)
            rowsOk = bin.maxRun[y + m.checkOrder[k]] >= m.need[m.checkOrder[k]];
        if (!rowsOk)
            continue;
        // Only free runs of the widest row that can hold its widest span are worth trying.
        const int wa = m.wideSpan.x, wb = m.wideSpan.y;
        for (const glm::ivec2& run : bin.runs[y + m.wideRow])
        {
            if (run.y - run.x < wb - wa)
                continue;
            int x = std::max(0, run.x - wa);
            const int xEnd = std::min(xMax, run.y - wb);
            while (x <= xEnd)
            {
                int failRow = -1, failAt = -1, failA = 0;
                for (int k = 0; k < s.h && failRow < 0; ++k)
                {
                    int r = startRow + k;
                    if (r >= s.h)
                        r -= s.h;
                    for (uint32_t i = s.rowStart[r]; i < s.rowStart[r + 1]; ++i)
                    {
                        int e = bin.firstSet(y + r, x + s.spans[i].x, x + s.spans[i].y);
                        if (e >= 0)
                        {
                            failRow = r;
                            failAt = e;
                            failA = s.spans[i].x;
                            break;
                        }
                    }
                }
                if (failRow < 0)
                {
                    at = glm::ivec2(x, y);
                    return true;
                }
                // That span cannot start anywhere up to the taken cell, so skip past the run it starts.
                startRow = failRow;
                x = std::max(x + 1, bin.firstClear(y + failRow, failAt) - failA);
            }
        }
    }
    return false;
}

struct Placed
{
    int bin = -1;
    int mask = -1;
    glm::ivec2 cell{0};
};

struct Attempt
{
    bool ok = false;
    double scale = 0.0;
    int heuristic = 0;
    std::vector<Placed> placed;
    std::vector<uint8_t> binUsed;
    double usedArea = 0.0; // cells
};

struct BinDef
{
    int tu = 0, tv = 0;
    glm::dvec2 lo{0.0}, hi{1.0};
};

/** Everything the attempts share. */
struct PackContext
{
    std::vector<Island> islands;
    std::vector<BinDef> binDefs;
    std::vector<std::vector<size_t>> orders;
};

/** The search at one raster resolution. */
struct Level
{
    int grid = 0;      // cells per tile
    double pd = 0.0;   // padding, cells
    std::vector<Bin> templates; // empty bins with margins and fixed islands
};

Attempt runAttempt(const PackContext& ctx, const Level& lv, const std::vector<std::vector<Mask>>& masks, int heuristic, double scale,
                   const std::function<bool()>& stop)
{
    LILA_ZONE();

    Attempt at;
    at.scale = scale;
    at.heuristic = heuristic;
    at.placed.resize(ctx.islands.size());
    at.binUsed.assign(ctx.binDefs.size(), 0);
    std::vector<std::optional<Bin>> bins(ctx.binDefs.size());
    for (size_t i : ctx.orders[size_t(heuristic)])
    {
        if (stop())
            return at;
        const std::vector<Mask>& ms = masks[i];
        bool placed = false;
        for (int b : ctx.islands[i].bins)
        {
            if (!bins[b])
                bins[b] = lv.templates[b];
            const Bin& bin = *bins[b];
            int best = -1, bestTop = std::numeric_limits<int>::max();
            glm::ivec2 bestAt(0);
            for (size_t o = 0; o < ms.size(); ++o)
            {
                const Mask& m = ms[o];
                if (!m.valid || m.raw.w > bin.w || m.raw.h > bin.h)
                    continue;
                glm::ivec2 p;
                int yMax = best < 0 ? bin.h : bestTop - m.raw.h;
                if (!findSpot(bin, m, yMax, p))
                    continue;
                int top = p.y + m.raw.h;
                if (top < bestTop || (top == bestTop && p.x < bestAt.x))
                {
                    best = int(o);
                    bestTop = top;
                    bestAt = p;
                }
            }
            if (best < 0)
                continue;
            const Mask& m = ms[size_t(best)];
            bins[b]->place(m.grown, bestAt.x - m.growOff.x, bestAt.y - m.growOff.y);
            at.placed[i] = {b, best, bestAt};
            at.binUsed[b] = 1;
            at.usedArea += ctx.islands[i].area * scale * scale;
            placed = true;
            break;
        }
        if (!placed)
            return at;
    }
    at.ok = true;
    return at;
}

/** Island orders the passes try: by area, longest side, box area, then jittered area orders. */
std::vector<std::vector<size_t>> islandOrders(const std::vector<Island>& islands, int effort)
{
    const int count = effort <= 0 ? 1 : effort == 1 ? 3 : effort == 2 ? 6 : 12;
    std::vector<std::vector<size_t>> out;
    for (int h = 0; h < count; ++h)
    {
        std::vector<double> key(islands.size());
        uint32_t seed = 0x9e3779b9u * uint32_t(h + 1);
        for (size_t i = 0; i < islands.size(); ++i)
        {
            const Island& is = islands[i];
            switch (h)
            {
            case 0: key[i] = is.area; break;
            case 1: key[i] = std::max(is.size.x, is.size.y); break;
            case 2: key[i] = is.size.x * is.size.y; break;
            case 3: key[i] = std::min(is.size.x, is.size.y); break;
            case 4: key[i] = is.size.x + is.size.y; break;
            default:
            {
                seed = seed * 1664525u + 1013904223u;
                key[i] = is.area * (0.7 + 0.6 * double(seed >> 8) / double(1u << 24));
                break;
            }
            }
        }
        std::vector<size_t> order(islands.size());
        std::iota(order.begin(), order.end(), size_t(0));
        std::stable_sort(order.begin(), order.end(), [&](size_t a, size_t b) { return key[a] > key[b]; });
        out.push_back(std::move(order));
    }
    return out;
}

/** Points sorted into a uniform grid, for "is there a point near p" queries. */
struct PointGrid
{
    double cell = 1.0;
    std::vector<std::pair<int64_t, glm::dvec2>> pts;

    static int64_t key(int64_t x, int64_t y) { return (x << 32) ^ (y & 0xffffffff); }

    PointGrid(std::span<const glm::dvec2> p, double c) : cell(std::max(c, 1e-12))
    {
        pts.reserve(p.size());
        for (const glm::dvec2& q : p)
            pts.emplace_back(key(int64_t(std::floor(q.x / cell)), int64_t(std::floor(q.y / cell))), q);
        std::sort(pts.begin(), pts.end(), [](const auto& a, const auto& b) { return a.first < b.first; });
    }

    bool near(const glm::dvec2& p, double tol) const
    {
        int64_t cx = int64_t(std::floor(p.x / cell)), cy = int64_t(std::floor(p.y / cell));
        for (int64_t dx = -1; dx <= 1; ++dx)
            for (int64_t dy = -1; dy <= 1; ++dy)
            {
                int64_t k = key(cx + dx, cy + dy);
                auto it = std::lower_bound(pts.begin(), pts.end(), k, [](const auto& a, int64_t v) { return a.first < v; });
                for (; it != pts.end() && it->first == k; ++it)
                    if (glm::length(it->second - p) <= tol)
                        return true;
            }
        return false;
    }
};

/** Maps a follower's uvs onto its leader's: R(angle) * mirror(p - from) + to. */
struct StackXform
{
    bool mirror = false;
    double angle = 0.0;
    glm::dvec2 from{0.0}, to{0.0};

    glm::dvec2 apply(glm::dvec2 p) const
    {
        p -= from;
        if (mirror)
            p.x = -p.x;
        double c = std::cos(angle), s = std::sin(angle);
        return glm::dvec2(p.x * c - p.y * s, p.x * s + p.y * c) + to;
    }
};

struct ShapeSig
{
    glm::dvec2 centre{0.0};
    double radius = 0.0;
    size_t anchor = 0;
    double area = 0.0;
};

ShapeSig signature(const Chart& ch)
{
    ShapeSig s;
    for (const glm::dvec2& p : ch.uv)
        s.centre += p;
    s.centre /= double(std::max<size_t>(1, ch.uv.size()));
    for (size_t i = 0; i < ch.uv.size(); ++i)
    {
        double r = glm::length(ch.uv[i] - s.centre);
        if (r > s.radius * (1.0 + 1e-12))
        {
            s.radius = r;
            s.anchor = i;
        }
    }
    s.area = std::abs(ch.areaUv());
    return s;
}

/**
 * True when b is a rigid copy (or mirror image, if allowed) of a within tol: rotated so one of b's outermost vertices
 * lands on a's outermost vertex, every vertex of b must land on a vertex of a.
 */
bool matchShape(const Chart& a, const ShapeSig& sa, const PointGrid& ga, const Chart& b, const ShapeSig& sb, double tol, bool mirrors,
                StackXform& out)
{
    if (a.uv.size() != b.uv.size() || a.tris.size() != b.tris.size())
        return false;
    if (std::abs(sa.radius - sb.radius) > tol || std::abs(sa.area - sb.area) > 0.02 * std::max(sa.area, sb.area))
        return false;
    double a3 = a.area3d(), b3 = b.area3d();
    if (std::abs(a3 - b3) > 0.02 * std::max(a3, b3))
        return false;
    glm::dvec2 da = a.uv[sa.anchor] - sa.centre;
    double angA = std::atan2(da.y, da.x);
    int tried = 0;
    for (size_t k = 0; k < b.uv.size() && tried < 32; ++k)
    {
        glm::dvec2 dk = b.uv[k] - sb.centre;
        if (std::abs(glm::length(dk) - sa.radius) > tol)
            continue;
        ++tried;
        for (int m = 0; m < (mirrors ? 2 : 1); ++m)
        {
            StackXform x;
            x.mirror = m == 1;
            x.from = sb.centre;
            x.to = sa.centre;
            glm::dvec2 q = x.mirror ? glm::dvec2(-dk.x, dk.y) : dk;
            x.angle = angA - std::atan2(q.y, q.x);
            bool ok = true;
            for (size_t v = 0; v < b.uv.size() && ok; ++v)
                ok = ga.near(x.apply(b.uv[v]), tol);
            if (ok)
            {
                out = x;
                return true;
            }
        }
    }
    return false;
}

std::vector<double> baseAngles(const PackOptions& o)
{
    std::vector<double> deg;
    if (!o.angles.empty())
        deg = o.angles;
    else
    {
        deg.push_back(0.0);
        if (o.rotateStep > 0)
            for (int a = o.rotateStep; a < 360; a += o.rotateStep)
                deg.push_back(double(a));
    }
    std::vector<double> rad;
    for (double d : deg)
        rad.push_back(glm::radians(d));
    return rad;
}

/**
 * The turn that puts an island's edge grid on the axes when it has one (hard surface quads), else its flow or its
 * smallest bounding rectangle (uprightAngle), held to the rotations the pack allows: any angle with free rotation, the nearest multiple of rotateStep
 * otherwise, and none for an exact angle list or a step of 0.
 */
void preOrientChart(Chart& ch, const PackOptions& o)
{
    if (ch.uv.size() < 2)
        return;
    double angle = uprightAngle(ch);
    if (!o.freeRotation)
    {
        if (!o.angles.empty() || o.rotateStep <= 0)
            return;
        const double step = glm::radians(double(o.rotateStep));
        angle = std::round(angle / step) * step;
    }
    if (std::abs(angle) > 1e-12)
        transformChart(ch, ch.uvCentroid(), angle);
}

/**
 * Free rotation: of every 5 degrees and the angles that put a hull edge on an axis, the few whose bounding box is
 * smallest, each with its quarter turns.
 */
void freeAngles(const Chart& ch, std::vector<double>& out)
{
    std::vector<glm::dvec2> hull = convexHull(ch.uv);
    std::vector<double> tryAngles;
    for (int d = 0; d < 90; d += 5)
        tryAngles.push_back(glm::radians(double(d)));
    for (size_t i = 0; i < hull.size(); ++i)
    {
        glm::dvec2 e = hull[(i + 1) % hull.size()] - hull[i];
        if (glm::length(e) > 0.0)
            tryAngles.push_back(std::fmod(-std::atan2(e.y, e.x) + glm::two_pi<double>(), glm::half_pi<double>()));
    }
    std::vector<std::pair<double, double>> cand; // box area, angle
    for (double a : tryAngles)
    {
        double c = std::cos(a), s = std::sin(a);
        glm::dvec2 lo(std::numeric_limits<double>::max()), hi(-lo);
        for (const glm::dvec2& p : hull)
        {
            glm::dvec2 r(p.x * c - p.y * s, p.x * s + p.y * c);
            lo = glm::min(lo, r);
            hi = glm::max(hi, r);
        }
        cand.emplace_back((hi.x - lo.x) * (hi.y - lo.y), a);
    }
    std::stable_sort(cand.begin(), cand.end(), [](const auto& x, const auto& y) { return x.first < y.first; });
    std::vector<double> picked;
    for (const auto& [area, a] : cand)
    {
        if (picked.size() >= 3)
            break;
        if (std::none_of(picked.begin(), picked.end(), [&](double b) { return std::abs(a - b) < glm::radians(2.0); }))
            picked.push_back(a);
    }
    for (double a : picked)
        for (int q = 0; q < 4; ++q)
            out.push_back(a + q * glm::half_pi<double>());
}

void dedupeAngles(std::vector<double>& a)
{
    for (double& x : a)
    {
        x = std::fmod(x, glm::two_pi<double>());
        if (x < 0.0)
            x += glm::two_pi<double>();
    }
    std::sort(a.begin(), a.end());
    std::vector<double> out;
    for (double x : a)
        if (out.empty() || x - out.back() > glm::radians(0.25))
            out.push_back(x);
    if (out.size() > 1 && glm::two_pi<double>() - out.back() + out.front() <= glm::radians(0.25))
        out.pop_back();
    a = std::move(out);
}

/** Splits the rectangle among weights, largest first, cutting across the longer side (slice and dice). */
void splitRegions(glm::dvec2 lo, glm::dvec2 hi, std::span<const std::pair<double, int>> items, std::vector<std::pair<glm::dvec2, glm::dvec2>>& out)
{
    if (items.empty())
        return;
    if (items.size() == 1)
    {
        out[size_t(items[0].second)] = {lo, hi};
        return;
    }
    double total = 0.0;
    for (const auto& it : items)
        total += it.first;
    // Split into two halves of nearly equal weight.
    size_t k = 0;
    double acc = 0.0;
    while (k + 1 < items.size() && (acc + items[k].first) <= total * 0.5)
        acc += items[k++].first;
    if (k == 0)
        acc = items[k++].first;
    double f = total > 0.0 ? acc / total : 0.5;
    glm::dvec2 ext = hi - lo;
    if (ext.x >= ext.y)
    {
        double mid = lo.x + ext.x * f;
        splitRegions(lo, {mid, hi.y}, items.subspan(0, k), out);
        splitRegions({mid, lo.y}, hi, items.subspan(k), out);
    }
    else
    {
        double mid = lo.y + ext.y * f;
        splitRegions(lo, {hi.x, mid}, items.subspan(0, k), out);
        splitRegions({lo.x, mid}, hi, items.subspan(k), out);
    }
}

void atomicMax(std::atomic<double>& a, double v)
{
    double cur = a.load();
    while (v > cur && !a.compare_exchange_weak(cur, v))
    {
    }
}

/** GroupLayouts: packs each group's charts as a Fill layout of its own; the least dense group stands for the result. */
PackResult packGroupLayouts(std::span<Chart> charts, const PackOptions& opts, std::span<const PackChartInfo> info)
{
    const Clock::time_point t0 = Clock::now();
    std::map<int, std::vector<size_t>> groups;
    for (size_t i = 0; i < charts.size(); ++i)
        groups[i < info.size() ? info[i].group : 0].push_back(i);
    PackOptions one = opts;
    one.layout = PackLayout::Fill;
    PackResult res;
    res.ok = true;
    res.stackedOn.assign(charts.size(), -1);
    res.tile.assign(charts.size(), -1);
    bool first = true;
    for (const auto& [group, members] : groups)
    {
        std::vector<Chart> sub;
        std::vector<PackChartInfo> subInfo;
        for (size_t i : members)
        {
            sub.push_back(std::move(charts[i]));
            subInfo.push_back(i < info.size() ? info[i] : PackChartInfo{});
        }
        PackResult r = packCharts(sub, one, subInfo);
        for (size_t k = 0; k < members.size(); ++k)
        {
            charts[members[k]] = std::move(sub[k]);
            res.tile[members[k]] = r.tile[k];
            if (r.stackedOn[k] >= 0)
                res.stackedOn[members[k]] = int(members[size_t(r.stackedOn[k])]);
        }
        res.cancelled |= r.cancelled;
        res.ok &= r.ok;
        res.stacked += r.stacked;
        res.attempts += r.attempts;
        res.tilesUsed = std::max(res.tilesUsed, r.tilesUsed);
        if (first || r.coverage < res.coverage)
        {
            res.coverage = r.coverage;
            res.texelsPerUnit = r.texelsPerUnit;
        }
        first = false;
        if (!r.ok)
            break;
    }
    res.seconds = std::chrono::duration<double>(Clock::now() - t0).count();
    return res;
}
} // namespace

PackResult packCharts(std::span<Chart> charts, const PackOptions& opts, std::span<const PackChartInfo> info)
{
    LILA_ZONE();

    if (opts.layout == PackLayout::GroupLayouts)
        return packGroupLayouts(charts, opts, info);
    const Clock::time_point t0 = Clock::now();
    PackResult res;
    res.stackedOn.assign(charts.size(), -1);
    res.tile.assign(charts.size(), -1);
    auto chartInfo = [&](size_t i) { return i < info.size() ? info[i] : PackChartInfo{}; };
    std::vector<size_t> moving;
    for (size_t i = 0; i < charts.size(); ++i)
        if (!chartInfo(i).fixed)
            moving.push_back(i);
    if (moving.empty())
    {
        res.ok = true;
        return res;
    }

    const int resolution = std::max(1, opts.resolution);
    // Finer search grids pack denser (islands lose less to rounding) and cost more; effort picks the default.
    const int autoGrid = opts.effort <= 0 ? 1024 : opts.effort == 1 ? 2048 : 4096;
    const int grid = std::clamp(opts.workingResolution > 0 ? opts.workingResolution : std::min(resolution, autoGrid), 16, 4096);
    const double texPerCell = double(resolution) / grid;
    const double mip = std::ldexp(1.0, std::clamp(opts.paddingMips, 0, 12));
    const double padTex = std::max(0.0, opts.padding) * mip;
    const double marginTex = (opts.margin < 0.0 ? opts.padding * 0.5 : opts.margin) * mip;
    const int tilesU = std::max(1, opts.tilesU), tilesV = std::max(1, opts.tilesV);
    glm::dvec2 regLo = glm::clamp(glm::min(opts.regionMin, opts.regionMax), 0.0, 1.0);
    glm::dvec2 regHi = glm::clamp(glm::max(opts.regionMin, opts.regionMax), 0.0, 1.0);
    if (regHi.x - regLo.x <= 1e-6 || regHi.y - regLo.y <= 1e-6)
    {
        regLo = glm::dvec2(0.0);
        regHi = glm::dvec2(1.0);
    }

    // Tile each island sits in now, before anything moves.
    std::vector<glm::ivec2> homeTile(charts.size(), glm::ivec2(0));
    if (opts.layout == PackLayout::KeepTile)
        for (size_t i : moving)
        {
            glm::dvec2 c = charts[i].uvCentroid();
            homeTile[i] = glm::max(glm::ivec2(glm::floor(c)), glm::ivec2(0));
        }

    tbb::parallel_for(size_t(0), moving.size(), [&](size_t k) {
        Chart& ch = charts[moving[k]];
        if (!opts.lockScale && opts.normalizeScale)
            matchSurfaceArea(ch);
        if (opts.preOrient)
            preOrientChart(ch, opts);
        glm::dvec2 lo = ch.uvMin();
        for (glm::dvec2& p : ch.uv)
            p -= lo;
    });

    // Groups and the bins each one may use.
    std::vector<int> groupKeys;
    for (size_t i : moving)
        groupKeys.push_back(chartInfo(i).group);
    std::sort(groupKeys.begin(), groupKeys.end());
    groupKeys.erase(std::unique(groupKeys.begin(), groupKeys.end()), groupKeys.end());
    auto groupIndex = [&](size_t i) { return int(std::lower_bound(groupKeys.begin(), groupKeys.end(), chartInfo(i).group) - groupKeys.begin()); };

    std::vector<BinDef> binDefs;
    std::vector<std::vector<int>> chartBins(charts.size());
    auto tileDef = [&](int tu, int tv) {
        BinDef d;
        d.tu = tu;
        d.tv = tv;
        d.lo = glm::dvec2(tu, tv) + regLo;
        d.hi = glm::dvec2(tu, tv) + regHi;
        return d;
    };
    switch (opts.layout)
    {
    case PackLayout::Fill:
    case PackLayout::GroupLayouts:
    {
        std::vector<int> all;
        for (int t = 0; t < tilesU * tilesV; ++t)
        {
            binDefs.push_back(tileDef(t % tilesU, t / tilesU));
            all.push_back(t);
        }
        for (size_t i : moving)
            chartBins[i] = all;
        break;
    }
    case PackLayout::GroupTiles:
        for (size_t g = 0; g < groupKeys.size(); ++g)
            binDefs.push_back(tileDef(int(g) % tilesU, int(g) / tilesU));
        for (size_t i : moving)
            chartBins[i] = {groupIndex(i)};
        break;
    case PackLayout::GroupRegions:
    {
        std::vector<std::pair<double, int>> weights(groupKeys.size());
        for (size_t g = 0; g < weights.size(); ++g)
            weights[g] = {0.0, int(g)};
        for (size_t i : moving)
            weights[size_t(groupIndex(i))].first += std::abs(charts[i].areaUv());
        std::stable_sort(weights.begin(), weights.end(), [](const auto& a, const auto& b) { return a.first > b.first; });
        std::vector<std::pair<glm::dvec2, glm::dvec2>> rects(groupKeys.size());
        splitRegions(regLo, regHi, weights, rects);
        for (auto& [lo, hi] : rects)
        {
            BinDef d;
            d.lo = lo;
            d.hi = hi;
            binDefs.push_back(d);
        }
        for (size_t i : moving)
            chartBins[i] = {groupIndex(i)};
        break;
    }
    case PackLayout::KeepTile:
    {
        std::vector<glm::ivec2> tiles;
        for (size_t i : moving)
            tiles.push_back(homeTile[i]);
        auto less = [](const glm::ivec2& a, const glm::ivec2& b) { return a.y < b.y || (a.y == b.y && a.x < b.x); };
        std::sort(tiles.begin(), tiles.end(), less);
        tiles.erase(std::unique(tiles.begin(), tiles.end()), tiles.end());
        for (const glm::ivec2& t : tiles)
            binDefs.push_back(tileDef(t.x, t.y));
        for (size_t i : moving)
            chartBins[i] = {int(std::lower_bound(tiles.begin(), tiles.end(), homeTile[i], less) - tiles.begin())};
        break;
    }
    }

    // Stacking: followers take their leader's spot.
    std::vector<int> leaderOf(charts.size(), -1);
    std::vector<StackXform> stackX(charts.size());
    if (opts.stack)
    {
        LILA_ZONE_N("stack");
        std::vector<ShapeSig> sig(charts.size());
        tbb::parallel_for(size_t(0), moving.size(), [&](size_t k) { sig[moving[k]] = signature(charts[moving[k]]); });
        // Candidates are bucketed by counts and bins, then compared within a narrow area window.
        std::map<std::tuple<size_t, size_t, std::vector<int>>, std::vector<size_t>> buckets;
        for (size_t i : moving)
            buckets[{charts[i].uv.size(), charts[i].tris.size(), chartBins[i]}].push_back(i);
        for (auto& [key, members] : buckets)
        {
            std::vector<size_t> leaders;
            std::vector<std::unique_ptr<PointGrid>> grids;
            for (size_t i : members)
            {
                double tol = std::max(1e-9, opts.stackTolerance * std::sqrt(sig[i].area));
                for (size_t l = 0; l < leaders.size(); ++l)
                {
                    size_t j = leaders[l];
                    if (!grids[l])
                        grids[l] = std::make_unique<PointGrid>(charts[j].uv, tol * 2.0);
                    StackXform x;
                    if (matchShape(charts[j], sig[j], *grids[l], charts[i], sig[i], tol, opts.stackMirrored, x))
                    {
                        leaderOf[i] = int(j);
                        stackX[i] = x;
                        break;
                    }
                }
                if (leaderOf[i] < 0)
                {
                    leaders.push_back(i);
                    grids.emplace_back();
                }
            }
        }
    }

    PackContext ctx;
    ctx.binDefs = binDefs;
    std::vector<int> islandOf(charts.size(), -1);
    std::vector<double> base = baseAngles(opts);
    for (size_t i : moving)
    {
        if (leaderOf[i] >= 0)
            continue;
        const Chart& ch = charts[i];
        Island is;
        is.chart = i;
        is.tris.resize(ch.tris.size());
        for (size_t t = 0; t < ch.tris.size(); ++t)
            is.tris[t] = {ch.uv[ch.tris[t][0]], ch.uv[ch.tris[t][1]], ch.uv[ch.tris[t][2]]};
        is.area = std::abs(ch.areaUv());
        is.size = ch.uvMax() - ch.uvMin();
        is.bins = chartBins[i];
        islandOf[i] = int(ctx.islands.size());
        ctx.islands.push_back(std::move(is));
    }
    tbb::parallel_for(size_t(0), ctx.islands.size(), [&](size_t k) {
        Island& is = ctx.islands[k];
        std::vector<double> angles = base;
        if (opts.freeRotation)
            freeAngles(charts[is.chart], angles);
        dedupeAngles(angles);
        for (int f = 0; f < (opts.flip ? 2 : 1); ++f)
            for (double a : angles)
                is.orients.push_back({a, f == 1});
    });
    for (size_t i : moving)
        if (leaderOf[i] >= 0)
            ++res.stacked;
    ctx.orders = islandOrders(ctx.islands, opts.effort);

    // A level is the search at one raster resolution: its bins hold the margins and every fixed chart, padded.
    auto makeLevel = [&](int cells) {
        Level lv;
        lv.grid = cells;
        const double tpc = double(resolution) / cells;
        lv.pd = padTex / tpc;
        const int margin = std::max(0, int(std::ceil(marginTex / tpc - 1e-9)));
        lv.templates.resize(binDefs.size());
        tbb::parallel_for(size_t(0), binDefs.size(), [&](size_t b) {
            const BinDef& d = binDefs[b];
            Bin& bin = lv.templates[b];
            bin.init(int(std::floor((d.hi.x - d.lo.x) * cells + 1e-6)), int(std::floor((d.hi.y - d.lo.y) * cells + 1e-6)), margin);
            for (size_t i = 0; i < charts.size(); ++i)
            {
                if (!chartInfo(i).fixed)
                    continue;
                const Chart& ch = charts[i];
                glm::dvec2 lo = ch.uvMin(), hi = ch.uvMax();
                if (hi.x < d.lo.x || hi.y < d.lo.y || lo.x > d.hi.x || lo.y > d.hi.y)
                    continue;
                std::vector<Tri2> tris(ch.tris.size());
                for (size_t t = 0; t < ch.tris.size(); ++t)
                    for (int k = 0; k < 3; ++k)
                        tris[t][k] = (ch.uv[ch.tris[t][k]] - d.lo) * double(cells);
                Spans s;
                s.w = bin.w;
                s.h = bin.h;
                rasterTris(tris, s);
                glm::ivec2 off;
                Spans g = grow(s, lv.pd, off);
                bin.place(g, -off.x, -off.y);
            }
        });
        return lv;
    };

    progress::Scope scope("Packing UVs", 0.0, progress::Cancellable);
    std::atomic<bool> cancelled{false};
    std::atomic<int> attempts{0};
    auto stopAll = [&] {
        if (scope.cancelled())
            cancelled = true;
        return cancelled.load(std::memory_order_relaxed);
    };

    auto masksAt = [&](const Level& lv, double scale) {
        LILA_ZONE_N("masks");
        std::vector<std::vector<Mask>> masks(ctx.islands.size());
        tbb::parallel_for(size_t(0), ctx.islands.size(), [&](size_t k) {
            const Island& is = ctx.islands[k];
            int maxW = 0, maxH = 0;
            for (int b : is.bins)
            {
                maxW = std::max(maxW, lv.templates[b].w);
                maxH = std::max(maxH, lv.templates[b].h);
            }
            masks[k].resize(is.orients.size());
            for (size_t o = 0; o < is.orients.size(); ++o)
            {
                masks[k][o] = makeMask(is, is.orients[o], scale, maxW, maxH, lv.pd);
                for (size_t p = 0; p < o && masks[k][o].valid; ++p)
                    if (masks[k][p].valid && sameCells(masks[k][o], masks[k][p]))
                        masks[k][o].valid = false;
            }
        });
        return masks;
    };

    const int H = int(ctx.orders.size());
    // Runs every pass at every scale; a scale counts as fitting when any pass fits, and the lowest such pass is kept.
    auto runScales = [&](const Level& lv, const std::vector<double>& scales, std::atomic<double>& bestOk,
                         std::vector<std::optional<Attempt>>& out) {
        out.assign(scales.size(), std::nullopt);
        tbb::parallel_for(size_t(0), scales.size(), size_t(1), [&](size_t c) {
            if (stopAll())
                return;
            std::vector<std::vector<Mask>> masks = masksAt(lv, scales[c]);
            std::atomic<int> firstOk{H};
            std::vector<Attempt> tries(static_cast<size_t>(H));
            Attempt* tryOut = tries.data();
            const double scale = scales[c];
            std::atomic<int>* first = &firstOk;
            std::atomic<double>* bestP = &bestOk;
            tbb::parallel_for(0, H, 1, [&, tryOut, scale, first, bestP](int h) {
                // Results that cannot change the outcome are dropped early: a larger scale already fits, or a lower pass did.
                auto stop = [&, scale, first, bestP, h] {
                    return stopAll() || bestP->load(std::memory_order_relaxed) > scale || first->load(std::memory_order_relaxed) < h;
                };
                tryOut[h] = runAttempt(ctx, lv, masks, h, scale, stop);
                ++attempts;
                if (tryOut[h].ok)
                {
                    int cur = first->load();
                    while (h < cur && !first->compare_exchange_weak(cur, h))
                    {
                    }
                }
            });
            for (int h = 0; h < H; ++h)
                if (tries[size_t(h)].ok)
                {
                    atomicMax(bestOk, scales[c]);
                    out[c] = std::move(tries[size_t(h)]);
                    return;
                }
        });
    };

    auto binsUsed = [](const Attempt& a) { return std::count(a.binUsed.begin(), a.binUsed.end(), uint8_t(1)); };
    const int cores = int(std::max(1u, std::thread::hardware_concurrency()));
    const int K = std::clamp(cores / std::max(1, H), 6, 16);
    auto spread = [&](double a, double b) {
        std::vector<double> v;
        for (int k = 0; k < K; ++k)
            v.push_back(a * std::pow(b / a, double(k) / (K - 1)));
        return v;
    };

    /**
     * Largest scale that fits, starting from candidates between c0 * f0 and c0 * f1: brackets down or up until some fit
     * and some fail, then narrows the gap to 0.2%.
     */
    auto search = [&](const Level& lv, double c0, double f0, double f1, int maxRounds, std::optional<Attempt>& best) {
        std::atomic<double> bestOk{0.0};
        double lo = 0.0, hi = std::numeric_limits<double>::infinity();
        std::vector<double> cand = spread(c0 * f0, c0 * f1);
        for (int round = 0; round < maxRounds && !cand.empty(); ++round)
        {
            if (stopAll())
                break;
            if (opts.timeLimit > 0.0 && best && std::chrono::duration<double>(Clock::now() - t0).count() > opts.timeLimit)
                break;
            std::vector<std::optional<Attempt>> tries;
            runScales(lv, cand, bestOk, tries);
            for (size_t c = 0; c < cand.size(); ++c)
                if (tries[c] && cand[c] > lo)
                {
                    lo = cand[c];
                    best = std::move(tries[c]);
                }
            for (size_t c = 0; c < cand.size(); ++c)
                if (!tries[c] && cand[c] > lo && !cancelled)
                    hi = std::min(hi, cand[c]);
            double tried0 = *std::min_element(cand.begin(), cand.end()), tried1 = *std::max_element(cand.begin(), cand.end());
            cand.clear();
            scope.step();
            if (lo == 0.0)
            {
                if (tried0 > 1e-9)
                    cand = spread(tried0 * 0.05, tried0 * 0.9);
            }
            else if (!std::isfinite(hi))
                cand = spread(tried1 * 1.04, tried1 * 3.0);
            else if (hi / lo - 1.0 > 0.002)
                for (int k = 1; k <= K; ++k)
                    cand.push_back(lo + (hi - lo) * k / (K + 1));
        }
        return lo;
    };

    std::optional<Attempt> best;
    Level fine = makeLevel(grid);
    int bestGrid = grid;
    double fixedScale = 0.0;
    if (opts.lockScale)
        fixedScale = double(grid);
    else if (opts.texelsPerUnit > 0.0)
        fixedScale = opts.texelsPerUnit / texPerCell;

    progress::runOnWorker([&] {
        if (fixedScale > 0.0)
        {
            scope.setTotal(1.0);
            std::vector<std::vector<Mask>> masks = masksAt(fine, fixedScale);
            std::vector<Attempt> tries(static_cast<size_t>(H));
            tbb::parallel_for(0, H, 1, [&](int h) {
                tries[size_t(h)] = runAttempt(ctx, fine, masks, h, fixedScale, stopAll);
                ++attempts;
            });
            for (Attempt& a : tries)
                if (a.ok && (!best || binsUsed(a) < binsUsed(*best)))
                    best = std::move(a);
            scope.step();
            return;
        }

        // First guess: islands as squares of their area plus padding filling three quarters of the free space.
        double totalArea = 0.0, sumSqrt = 0.0;
        for (const Island& is : ctx.islands)
        {
            totalArea += is.area;
            sumSqrt += std::sqrt(is.area);
        }
        auto firstGuess = [&](const Level& lv) {
            double usable = 0.0;
            for (const Bin& b : lv.templates)
                usable += double(b.freeCells());
            usable *= 0.75;
            double qa = std::max(totalArea, 1e-30), qb = 2.0 * lv.pd * sumSqrt, qc = double(ctx.islands.size()) * lv.pd * lv.pd - usable;
            double c0 = (-qb + std::sqrt(std::max(0.0, qb * qb - 4.0 * qa * qc))) / (2.0 * qa);
            return c0 > 0.0 ? c0 : std::sqrt(usable / qa) * 0.25;
        };

        // Large grids are searched coarse first; the fine search then only has to refine around that answer.
        const int coarseGrid = 1024;
        scope.setTotal(grid > coarseGrid ? 20.0 : 14.0);
        if (grid > coarseGrid)
        {
            Level coarse = makeLevel(coarseGrid);
            std::optional<Attempt> rough;
            double sc = search(coarse, firstGuess(coarse), 0.55, 1.3, 12, rough);
            if (sc > 0.0)
            {
                double f = double(grid) / coarseGrid;
                search(fine, sc * f, 0.99, 1.12, 8, best);
                if (!best && !cancelled)
                {
                    best = std::move(rough);
                    bestGrid = coarseGrid;
                }
            }
            if (rough && !best && !cancelled)
            {
                best = std::move(rough);
                bestGrid = coarseGrid;
            }
        }
        else
            search(fine, firstGuess(fine), 0.55, 1.3, 14, best);
        scope.set(scope.total());
    });

    res.attempts = attempts;
    res.seconds = std::chrono::duration<double>(Clock::now() - t0).count();
    if (cancelled)
    {
        res.cancelled = true;
        return res;
    }
    if (!best)
        return res;

    // Map each island into its bin: orient, scale to cells, then shift to the placed corner.
    const Attempt& at = *best;
    auto place = [&](Chart& ch, size_t isl, const StackXform* sx) {
        const Island& is = ctx.islands[isl];
        const Placed& p = at.placed[isl];
        const Orientation& o = is.orients[size_t(p.mask)];
        const BinDef& d = binDefs[size_t(p.bin)];
        const glm::dvec2 origin = maskOrigin(is, o, at.scale);
        double c = std::cos(o.angle), s = std::sin(o.angle);
        for (glm::dvec2& uv : ch.uv)
        {
            glm::dvec2 q = sx ? sx->apply(uv) : uv;
            glm::dvec2 cell = orient(q, o, c, s) * at.scale - origin + glm::dvec2(p.cell);
            uv = d.lo + cell / double(bestGrid);
        }
    };
    tbb::parallel_for(size_t(0), moving.size(), [&](size_t k) {
        size_t i = moving[k];
        size_t l = leaderOf[i] >= 0 ? size_t(leaderOf[i]) : i;
        size_t isl = size_t(islandOf[l]);
        place(charts[i], isl, leaderOf[i] >= 0 ? &stackX[i] : nullptr);
        const BinDef& d = binDefs[size_t(at.placed[isl].bin)];
        res.tile[i] = d.tu + 10 * d.tv;
        res.stackedOn[i] = leaderOf[i];
    });

    double binArea = 0.0;
    std::vector<int> tilesSeen;
    for (size_t b = 0; b < binDefs.size(); ++b)
        if (at.binUsed[b])
        {
            const BinDef& d = binDefs[b];
            binArea += (d.hi.x - d.lo.x) * (d.hi.y - d.lo.y) * double(bestGrid) * bestGrid;
            tilesSeen.push_back(binDefs[b].tu + 10 * binDefs[b].tv);
        }
    std::sort(tilesSeen.begin(), tilesSeen.end());
    res.tilesUsed = int(std::unique(tilesSeen.begin(), tilesSeen.end()) - tilesSeen.begin());
    res.ok = true;
    res.texelsPerUnit = at.scale * double(resolution) / bestGrid;
    res.coverage = binArea > 0.0 ? at.usedArea / binArea : 0.0;
    return res;
}

namespace
{
/**
 * Removes the fixed islands that share no material with a moving one: another material is another texture, so
 * its islands take no room in this layout.
 */
void dropFixedOfOtherMaterials(std::span<const PackTarget> targets, std::vector<Chart>& charts, std::vector<PackChartInfo>& info,
                               std::vector<size_t>& owner)
{
    auto materialsOf = [&](size_t i) {
        const geo::Mesh& mesh = *targets[owner[i]].mesh;
        std::span<const int32_t> ids = mesh.faceAttrs().get<int32_t>(geo::attr::kMaterial);
        const auto& names = mesh.materialNames();
        std::set<std::string> out;
        for (Index f : charts[i].faces)
        {
            const int32_t id = f < ids.size() ? ids[f] : 0;
            out.insert(id >= 0 && size_t(id) < names.size() ? names[size_t(id)] : "#" + std::to_string(id));
        }
        return out;
    };
    std::set<std::string> moving;
    for (size_t i = 0; i < charts.size(); ++i)
        if (!info[i].fixed)
            moving.merge(materialsOf(i));
    size_t kept = 0;
    for (size_t i = 0; i < charts.size(); ++i)
    {
        if (info[i].fixed)
        {
            const std::set<std::string> mine = materialsOf(i);
            if (std::none_of(mine.begin(), mine.end(), [&](const std::string& m) { return moving.count(m) != 0; }))
                continue;
        }
        if (kept != i)
        {
            charts[kept] = std::move(charts[i]);
            info[kept] = info[i];
            owner[kept] = owner[i];
        }
        ++kept;
    }
    charts.resize(kept);
    info.resize(kept);
    owner.resize(kept);
}
} // namespace

PackResult packMeshes(std::span<PackTarget> targets, std::string_view uvSet, const PackOptions& opts)
{
    LILA_ZONE();

    std::vector<Chart> charts;
    std::vector<PackChartInfo> info;
    std::vector<size_t> owner;
    std::vector<std::string> warnings;
    std::map<std::string, int> groupIds;
    auto groupId = [&](const std::string& key) { return groupIds.emplace(key, int(groupIds.size())).first->second; };
    for (size_t t = 0; t < targets.size(); ++t)
    {
        PackTarget& tg = targets[t];
        if (!tg.mesh)
            continue;
        geo::Mesh& mesh = *tg.mesh;
        ChartOptions co;
        co.source = IslandSource::Uvs;
        co.uvSet = uvSet;
        std::vector<Index> live = liveFaces(mesh, tg.faces);
        std::vector<uint8_t> inSet(mesh.faceCapacity(), 0), keep(mesh.faceCapacity(), 0);
        for (Index f : live)
            inSet[f] = 1;
        for (Index f : tg.fixedFaces)
            if (f < keep.size())
                keep[f] = 1;
        const bool wholeMesh = tg.othersFixed || !tg.fixedFaces.empty();
        std::vector<Chart> local = buildCharts(mesh, wholeMesh ? std::span<const Index>{} : std::span<const Index>(live), co);
        if (!tg.stack.empty())
            if (FinalMap map = applyDensityAreas(mesh, local, uvSet, tg.stack); !map.warning.empty())
                warnings.push_back(map.warning);
        std::span<const int32_t> material = mesh.faceAttrs().get<int32_t>(geo::attr::kMaterial);
        std::span<const int32_t> userGroup = mesh.faceAttrs().get<int32_t>(opts.groupAttr);
        for (Chart& ch : local)
        {
            PackChartInfo ci;
            bool selected = false, kept = false;
            for (Index f : ch.faces)
            {
                selected |= inSet[f] != 0;
                kept |= keep[f] != 0;
            }
            ci.fixed = (tg.othersFixed && !selected) || kept;
            if (opts.fixPinned)
                ci.fixed |= std::any_of(ch.pinned.begin(), ch.pinned.end(), [](uint8_t p) { return p != 0; });
            Index f0 = ch.faces.empty() ? 0 : ch.faces.front();
            switch (opts.groupBy)
            {
            case PackGroupBy::None: break;
            case PackGroupBy::Object: ci.group = int(t); break;
            case PackGroupBy::Material:
            {
                int id = f0 < material.size() ? material[f0] : 0;
                const auto& names = mesh.materialNames();
                ci.group = groupId(id >= 0 && size_t(id) < names.size() ? names[size_t(id)] : "#" + std::to_string(id));
                break;
            }
            case PackGroupBy::Attribute: ci.group = f0 < userGroup.size() ? userGroup[f0] : 0; break;
            }
            charts.push_back(std::move(ch));
            info.push_back(ci);
            owner.push_back(t);
        }
    }
    dropFixedOfOtherMaterials(targets, charts, info, owner);
    // Degenerate islands get a fresh unwrap so they have an area to pack; positions go to world space for density.
    tbb::parallel_for(size_t(0), charts.size(), [&](size_t i) {
        if (info[i].fixed)
            return;
        Chart& ch = charts[i];
        if (std::abs(ch.areaUv()) <= 1e-14 * std::max(1.0, ch.area3d()))
            unwrapChart(ch);
        const glm::dmat3& lin = targets[owner[i]].linear;
        if (lin != glm::dmat3(1.0))
        {
            const double before = ch.area3d();
            for (glm::dvec3& p : ch.pos)
                p = lin * p;
            if (before > 0.0)
                ch.densityArea *= ch.area3d() / before;
        }
    });
    PackResult r = packCharts(charts, opts, info);
    r.warnings = std::move(warnings);
    if (!r.ok)
        return r;
    for (size_t t = 0; t < targets.size(); ++t)
    {
        std::vector<Chart> mine;
        for (size_t i = 0; i < charts.size(); ++i)
            if (owner[i] == t && !info[i].fixed)
                mine.push_back(std::move(charts[i]));
        if (!mine.empty())
            writeCharts(*targets[t].mesh, mine, uvSet);
    }
    return r;
}

PackResult packIslands(geo::Mesh& mesh, std::span<const Index> faces, std::string_view uvSet, const PackOptions& opts)
{
    PackTarget t;
    t.mesh = &mesh;
    t.faces.assign(faces.begin(), faces.end());
    return packMeshes(std::span<PackTarget>(&t, 1), uvSet, opts);
}

std::vector<std::vector<std::vector<Index>>> stackedIslands(const geo::Mesh& mesh, std::string_view uvSet, double tolerance)
{
    LILA_ZONE();

    std::vector<std::vector<std::vector<Index>>> out;
    if (mesh.cornerAttrs().get<glm::vec2>(uvSet).empty())
        return out;
    ChartOptions co;
    co.source = IslandSource::Uvs;
    co.uvSet = uvSet;
    std::vector<Chart> charts = buildCharts(mesh, {}, co);
    std::vector<ShapeSig> sig(charts.size());
    for (size_t i = 0; i < charts.size(); ++i)
        sig[i] = signature(charts[i]);
    std::vector<size_t> order(charts.size());
    std::iota(order.begin(), order.end(), size_t(0));
    std::sort(order.begin(), order.end(), [&](size_t a, size_t b) { return sig[a].centre.x < sig[b].centre.x; });
    std::vector<int> stackOf(charts.size(), -1);
    for (size_t k = 0; k < order.size(); ++k)
    {
        size_t i = order[k];
        if (stackOf[i] >= 0)
            continue;
        double tol = std::max(1e-7, tolerance * std::sqrt(sig[i].area));
        std::unique_ptr<PointGrid> grid;
        std::vector<size_t> members{i};
        for (size_t q = k + 1; q < order.size() && sig[order[q]].centre.x - sig[i].centre.x <= tol; ++q)
        {
            size_t j = order[q];
            if (stackOf[j] >= 0 || charts[j].uv.size() != charts[i].uv.size() || glm::length(sig[j].centre - sig[i].centre) > tol)
                continue;
            if (!grid)
                grid = std::make_unique<PointGrid>(charts[i].uv, tol * 2.0);
            bool same = std::all_of(charts[j].uv.begin(), charts[j].uv.end(), [&](const glm::dvec2& p) { return grid->near(p, tol); });
            if (same)
                members.push_back(j);
        }
        if (members.size() < 2)
            continue;
        std::vector<std::vector<Index>> stack;
        for (size_t m : members)
        {
            stackOf[m] = int(out.size());
            stack.push_back(charts[m].faces);
        }
        out.push_back(std::move(stack));
    }
    return out;
}
} // namespace lila::uv
