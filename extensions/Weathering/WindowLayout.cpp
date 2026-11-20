#include "WindowLayout.h"

#include <algorithm>
#include <cmath>
#include <map>

namespace weathering::window
{
using gen::hash01;
using gen::unit;

namespace
{
struct Slab
{
    glm::dvec3 axis{1, 0, 0};
    std::vector<Index> faces;
};

/** Faces of a wall grouped into slabs: about the same horizontal facing (either way), close together along it. */
std::vector<Slab> slabsOf(const Mesh& m, double maxThickness)
{
    struct Cluster
    {
        glm::dvec3 axis;
        std::vector<Index> faces;
    };
    std::vector<Cluster> clusters;
    const double same = std::cos(glm::radians(15.0));
    for (Index f = 0; f < m.faceCapacity(); ++f)
    {
        if (!m.faceValid(f) || m.faceArea(f) < 1e-4)
            continue;
        const glm::dvec3 n = m.faceNormal(f);
        if (std::abs(n.y) > 0.35)
            continue;
        glm::dvec3 h = unit(glm::dvec3(n.x, 0.0, n.z));
        if (h.x < -1e-6 || (std::abs(h.x) <= 1e-6 && h.z < 0.0))
            h = -h;
        Cluster* into = nullptr;
        for (Cluster& c : clusters)
            if (glm::dot(c.axis, h) > same)
                into = &c;
        if (!into)
        {
            clusters.push_back({h, {}});
            into = &clusters.back();
        }
        into->faces.push_back(f);
    }
    std::vector<Slab> out;
    for (Cluster& c : clusters)
    {
        std::ranges::sort(c.faces, [&](Index a, Index b) { return glm::dot(m.faceCentroid(a), c.axis) < glm::dot(m.faceCentroid(b), c.axis); });
        Slab cur{c.axis, {}};
        double last = -1e300;
        for (Index f : c.faces)
        {
            const double s = glm::dot(m.faceCentroid(f), c.axis);
            if (!cur.faces.empty() && s - last > maxThickness)
            {
                out.push_back(std::move(cur));
                cur = Slab{c.axis, {}};
            }
            cur.faces.push_back(f);
            last = s;
        }
        if (!cur.faces.empty())
            out.push_back(std::move(cur));
    }
    return out;
}

double snapTo(const std::vector<double>& sorted, double x, double within)
{
    const auto it = std::ranges::lower_bound(sorted, x);
    double best = x, bestD = within;
    for (auto j : {it, it == sorted.begin() ? it : std::prev(it)})
        if (j != sorted.end() && std::abs(*j - x) <= bestD)
        {
            bestD = std::abs(*j - x);
            best = *j;
        }
    return best;
}

void simplify(const std::vector<glm::dvec2>& in, size_t a, size_t b, double tol, std::vector<uint8_t>& keep)
{
    if (b <= a + 1)
        return;
    const glm::dvec2 d = in[b] - in[a];
    const double len = glm::length(d);
    double worst = -1.0;
    size_t at = a;
    for (size_t k = a + 1; k < b; ++k)
    {
        const glm::dvec2 p = in[k] - in[a];
        const double dist = len > 1e-12 ? std::abs(d.x * p.y - d.y * p.x) / len : glm::length(p);
        if (dist > worst)
        {
            worst = dist;
            at = k;
        }
    }
    if (worst > tol)
    {
        keep[at] = 1;
        simplify(in, a, at, tol, keep);
        simplify(in, at, b, tol, keep);
    }
}

double signedArea(const std::vector<glm::dvec2>& p)
{
    double a = 0.0;
    for (size_t k = 0; k < p.size(); ++k)
        a += p[k].x * p[(k + 1) % p.size()].y - p[(k + 1) % p.size()].x * p[k].y;
    return a * 0.5;
}

void openingsInSlab(const Mesh& m, const Slab& slab, const glm::dvec3& hostCenter, const FindOptions& o, std::vector<Opening>& out)
{
    const glm::dvec3 H = slab.axis;
    const glm::dvec3 Ua = unit(glm::cross(glm::dvec3(0, 1, 0), H), glm::dvec3(1, 0, 0));
    double uMin = 1e300, uMax = -1e300, vMin = 1e300, vMax = -1e300, sMin = 1e300, sMax = -1e300;
    std::vector<double> us, vs;
    std::vector<std::vector<glm::dvec2>> polys;
    for (Index f : slab.faces)
    {
        std::vector<glm::dvec2> poly;
        for (Index p : m.facePoints(f))
        {
            const glm::dvec3 q = m.position(p);
            const glm::dvec2 uv(glm::dot(q, Ua), q.y);
            poly.push_back(uv);
            uMin = std::min(uMin, uv.x);
            uMax = std::max(uMax, uv.x);
            vMin = std::min(vMin, uv.y);
            vMax = std::max(vMax, uv.y);
            const double s = glm::dot(q, H);
            sMin = std::min(sMin, s);
            sMax = std::max(sMax, s);
            us.push_back(uv.x);
            vs.push_back(uv.y);
        }
        polys.push_back(std::move(poly));
    }
    const double du = uMax - uMin, dv = vMax - vMin;
    if (du < o.minSize || dv < o.minSize)
        return;
    const double cell = o.cell > 0.0 ? o.cell : std::clamp(std::max(du, dv) / 300.0, 0.5, 8.0);
    // One empty cell of padding left, right and on top; none below, so doors reach the bottom row.
    const int nu = int(std::ceil(du / cell)) + 2, nv = int(std::ceil(dv / cell)) + 1;
    const double gu0 = uMin - cell, gv0 = vMin;
    std::vector<uint8_t> covered(size_t(nu) * size_t(nv), 0);
    auto idx = [&](int i, int j) { return size_t(j) * size_t(nu) + size_t(i); };
    for (const auto& poly : polys)
    {
        double a0 = 1e300, a1 = -1e300, b0 = 1e300, b1 = -1e300;
        for (const glm::dvec2& p : poly)
        {
            a0 = std::min(a0, p.x);
            a1 = std::max(a1, p.x);
            b0 = std::min(b0, p.y);
            b1 = std::max(b1, p.y);
        }
        const int i0 = std::max(0, int(std::floor((a0 - gu0) / cell))), i1 = std::min(nu - 1, int(std::ceil((a1 - gu0) / cell)));
        const int j0 = std::max(0, int(std::floor((b0 - gv0) / cell))), j1 = std::min(nv - 1, int(std::ceil((b1 - gv0) / cell)));
        for (int j = j0; j <= j1; ++j)
            for (int i = i0; i <= i1; ++i)
                if (!covered[idx(i, j)] && gen::insidePolygon(poly, {gu0 + (i + 0.5) * cell, gv0 + (j + 0.5) * cell}))
                    covered[idx(i, j)] = 1;
    }
    // Closing (grow then shrink by a cell) seals mortar joints and hairline gaps between pieces.
    auto grow = [&](const std::vector<uint8_t>& in, uint8_t value) {
        std::vector<uint8_t> o2 = in;
        for (int j = 0; j < nv; ++j)
            for (int i = 0; i < nu; ++i)
            {
                if (in[idx(i, j)] == value)
                    continue;
                for (auto [di, dj] : {std::pair{1, 0}, {-1, 0}, {0, 1}, {0, -1}})
                {
                    const int a = i + di, b = j + dj;
                    if (a >= 0 && a < nu && b >= 0 && b < nv && in[idx(a, b)] == value)
                        o2[idx(i, j)] = value;
                }
            }
        return o2;
    };
    covered = grow(grow(covered, 1), 0);
    for (int j = 0; j < nv; ++j)
    {
        covered[idx(0, j)] = 0;
        covered[idx(nu - 1, j)] = 0;
    }
    for (int i = 0; i < nu; ++i)
        covered[idx(i, nv - 1)] = 0;

    std::vector<int> comp(covered.size(), -1);
    std::vector<std::vector<std::pair<int, int>>> comps;
    for (int j = 0; j < nv; ++j)
        for (int i = 0; i < nu; ++i)
        {
            if (covered[idx(i, j)] || comp[idx(i, j)] >= 0)
                continue;
            const int c = int(comps.size());
            comps.emplace_back();
            std::vector<std::pair<int, int>> stack{{i, j}};
            comp[idx(i, j)] = c;
            while (!stack.empty())
            {
                auto [a, b] = stack.back();
                stack.pop_back();
                comps[size_t(c)].push_back({a, b});
                for (auto [di, dj] : {std::pair{1, 0}, {-1, 0}, {0, 1}, {0, -1}})
                {
                    const int x = a + di, y = b + dj;
                    if (x >= 0 && x < nu && y >= 0 && y < nv && !covered[idx(x, y)] && comp[idx(x, y)] < 0)
                    {
                        comp[idx(x, y)] = c;
                        stack.push_back({x, y});
                    }
                }
            }
        }

    std::ranges::sort(us);
    std::ranges::sort(vs);
    glm::dvec3 N = H;
    double outer = sMax;
    if (glm::dot(hostCenter, H) > (sMin + sMax) * 0.5)
    {
        N = -H;
        outer = -sMin;
    }
    if (o.flip)
    {
        N = -N;
        outer = N == H ? sMax : -sMin;
    }
    const glm::dvec3 U = glm::cross(glm::dvec3(0, 1, 0), N);
    const double uSign = glm::dot(U, Ua) > 0.0 ? 1.0 : -1.0;

    for (const auto& cells : comps)
    {
        int i0 = nu, i1 = -1, j0 = nv, j1 = -1;
        bool outside = false;
        for (auto [i, j] : cells)
        {
            i0 = std::min(i0, i);
            i1 = std::max(i1, i);
            j0 = std::min(j0, j);
            j1 = std::max(j1, j);
            outside = outside || i == 0 || i == nu - 1 || j == nv - 1;
        }
        if (outside)
            continue;
        double a0 = gu0 + i0 * cell, a1 = gu0 + (i1 + 1) * cell, b0 = gv0 + j0 * cell, b1 = gv0 + (j1 + 1) * cell;
        if (a1 - a0 < o.minSize || b1 - b0 < o.minSize)
            continue;
        const double fill = double(cells.size()) / double((i1 - i0 + 1) * (j1 - j0 + 1));
        Opening op;
        op.door = j0 == 0;
        op.rectangular = fill > 0.9;
        a0 = snapTo(us, a0, cell * 1.5);
        a1 = snapTo(us, a1, cell * 1.5);
        b0 = snapTo(vs, b0, cell * 1.5);
        b1 = snapTo(vs, b1, cell * 1.5);
        std::vector<glm::dvec2> outline;
        if (op.rectangular)
            outline = {{a0, b0}, {a1, b0}, {a1, b1}, {a0, b1}};
        else
        {
            std::map<int, std::pair<int, int>> span;
            for (auto [i, j] : cells)
            {
                auto [it, added] = span.try_emplace(j, i, i);
                it->second.first = std::min(it->second.first, i);
                it->second.second = std::max(it->second.second, i);
            }
            std::vector<glm::dvec2> left, right;
            const double in = cell * 0.5;
            for (const auto& [j, lr] : span)
            {
                const double lo = gv0 + j * cell, hi = lo + cell;
                left.push_back({gu0 + lr.first * cell + in, std::max(lo, b0)});
                left.push_back({gu0 + lr.first * cell + in, std::min(hi, b1)});
                right.push_back({gu0 + (lr.second + 1) * cell - in, std::max(lo, b0)});
                right.push_back({gu0 + (lr.second + 1) * cell - in, std::min(hi, b1)});
            }
            std::vector<glm::dvec2> loop = left;
            for (auto it = right.rbegin(); it != right.rend(); ++it)
                loop.push_back(*it);
            std::vector<uint8_t> keep(loop.size(), 0);
            keep.front() = keep.back() = 1;
            simplify(loop, 0, loop.size() - 1, cell * 1.5, keep);
            for (size_t k = 0; k < loop.size(); ++k)
                if (keep[k])
                    outline.push_back(loop[k]);
        }
        // From the slab's u to the outward frame's.
        for (glm::dvec2& p : outline)
            p.x *= uSign;
        if (signedArea(outline) < 0.0)
            std::ranges::reverse(outline);
        op.u0 = 1e300;
        op.u1 = -1e300;
        for (const glm::dvec2& p : outline)
        {
            op.u0 = std::min(op.u0, p.x);
            op.u1 = std::max(op.u1, p.x);
        }
        op.v0 = b0;
        op.v1 = b1;
        op.outline = std::move(outline);
        op.U = U;
        op.V = glm::dvec3(0, 1, 0);
        op.N = N;
        op.base = N * outer;
        op.depth = sMax - sMin;
        out.push_back(std::move(op));
    }
}
} // namespace

std::vector<Opening> findOpenings(const Mesh& wall, const glm::dvec3& hostCenter, const FindOptions& o)
{
    std::vector<Opening> out;
    for (const Slab& s : slabsOf(wall, o.maxThickness))
        openingsInSlab(wall, s, hostCenter, o, out);
    return out;
}

const char* glassName(Glass g)
{
    switch (g)
    {
    case Glass::Intact: return "intact";
    case Glass::Cracked: return "cracked";
    case Glass::Broken: return "broken";
    case Glass::Boarded: return "boarded";
    case Glass::None: return "none";
    case Glass::Mixed: return "mixed";
    }
    return "intact";
}

bool parseGlass(const std::string& s, Glass* out)
{
    for (Glass g : {Glass::Intact, Glass::Cracked, Glass::Broken, Glass::Boarded, Glass::None, Glass::Mixed})
        if (s == glassName(g))
        {
            *out = g;
            return true;
        }
    return false;
}

bool parseInterior(const std::string& s, Interior* out)
{
    const std::pair<const char*, Interior> names[] = {{"room", Interior::Room}, {"card", Interior::Card}, {"mesh", Interior::Mesh}, {"none", Interior::None}};
    for (const auto& [n, v] : names)
        if (s == n)
        {
            *out = v;
            return true;
        }
    return false;
}

Result layout(std::span<const Opening> openings, const Pieces& pieces, const Options& o)
{
    Result r;
    const double messy = 1.0 - std::clamp(o.neatness, 0.0, 1.0);
    const double damage = std::clamp(o.damage, 0.0, 1.0);
    const double fs = std::max(0.01, o.frameScale);
    auto pick = [&](Slot s, uint32_t a, uint32_t b) { return gen::pickWeighted(pieces.weights[s], int(pieces.slot[s].size()), hash01(o.seed, 900 + s, a, b)); };
    auto add = [&](Slot s, int piece, int window, const glm::dmat4& xf, uint32_t a, uint32_t b) {
        Placed p;
        p.xf = xf;
        p.slot = s;
        p.piece = piece;
        p.window = window;
        p.random = float(hash01(o.seed, 950 + s, a, b));
        r.placed.push_back(p);
    };

    for (const Opening& op0 : openings)
    {
        if (op0.door && !o.doors)
        {
            ++r.doorsSkipped;
            continue;
        }
        const int w = int(r.openings.size());
        r.openings.push_back(op0);
        const Opening& op = r.openings.back();
        const uint32_t wid = uint32_t(w);
        const double width = op.u1 - op.u0, height = op.v1 - op.v0;
        const double uc = (op.u0 + op.u1) * 0.5;

        double memberWidth = 0.0, memberDepth = 6.0;
        gen::Fit frameFit;
        const bool hasFrame = !pieces.slot[Frame].empty();
        int framePick = 0;
        if (hasFrame)
        {
            framePick = pick(Frame, wid, 0);
            frameFit = gen::fitPiece(pieces.slot[Frame][size_t(framePick)], false, true);
            memberWidth = frameFit.size.y * fs;
            memberDepth = frameFit.size.z * fs;
        }
        double wf = -(o.frameInset + memberDepth * 0.5);
        if (op.depth > 1e-3 && -wf + memberDepth * 0.5 > op.depth)
            wf = -op.depth * 0.5;

        double bottom = op.v0;
        if (o.sill && !op.door && !pieces.slot[Sill].empty())
        {
            const int sp = pick(Sill, wid, 1);
            const gen::Fit f = gen::fitPiece(pieces.slot[Sill][size_t(sp)], false, true);
            const double t = f.size.y * fs;
            const double back = wf - memberDepth * 0.5, front = std::max(o.sillProject, back + 1.0);
            const glm::dvec3 scale(width / f.size.x, fs, (front - back) / f.size.z);
            add(Sill, sp, w, gen::place(f, op.at(uc, op.v0 + t * 0.5, (front + back) * 0.5), op.U, op.V, op.N, scale), wid, 1);
            bottom = op.v0 + t;
        }

        std::vector<glm::dvec2> outline = op.outline;
        for (glm::dvec2& p : outline)
            p.y = std::max(p.y, bottom);
        if (hasFrame)
            for (size_t k = 0; k < outline.size(); ++k)
            {
                const glm::dvec2 a = outline[k], b = outline[(k + 1) % outline.size()];
                const double len = glm::length(b - a);
                if (len < 1e-3)
                    continue;
                const glm::dvec2 t = (b - a) / len;
                const glm::dvec2 inward(-t.y, t.x);
                const glm::dvec2 c = (a + b) * 0.5 + inward * memberWidth * 0.5;
                const glm::dvec3 X = op.U * t.x + op.V * t.y, Y = op.U * inward.x + op.V * inward.y;
                add(Frame, framePick, w, gen::place(frameFit, op.at(c.x, c.y, wf), X, Y, op.N, glm::dvec3(len / frameFit.size.x, fs, fs)), wid, uint32_t(10 + k));
            }

        const double iu0 = op.u0 + memberWidth, iu1 = op.u1 - memberWidth, iv0 = bottom + memberWidth, iv1 = op.v1 - memberWidth;
        const int cols = std::max(1, o.columns), rows = std::max(1, o.rows);
        if (hasFrame && iu1 > iu0 && iv1 > iv0)
        {
            const double mw = 0.6;
            for (int k = 1; k < cols; ++k)
            {
                const double u = iu0 + (iu1 - iu0) * k / cols;
                add(Frame, framePick, w, gen::place(frameFit, op.at(u, (iv0 + iv1) * 0.5, wf), op.V, -op.U, op.N, glm::dvec3((iv1 - iv0) / frameFit.size.x, fs * mw, fs * mw)),
                    wid, uint32_t(100 + k));
            }
            for (int k = 1; k < rows; ++k)
            {
                const double v = iv0 + (iv1 - iv0) * k / rows;
                add(Frame, framePick, w, gen::place(frameFit, op.at((iu0 + iu1) * 0.5, v, wf), op.U, op.V, op.N, glm::dvec3((iu1 - iu0) / frameFit.size.x, fs * mw, fs * mw)),
                    wid, uint32_t(200 + k));
            }
        }

        Glass state = o.glass;
        if (state == Glass::Mixed && hash01(o.seed, 31, wid) < damage * 0.2)
            state = Glass::Boarded;
        if (state == Glass::Boarded && !pieces.slot[Board].empty())
        {
            ++r.boarded;
            const gen::Fit probe = gen::fitPiece(pieces.slot[Board][0], false, true);
            const double bw = probe.size.y;
            const int n = std::max(1, int(std::round(height / (bw * 1.3))));
            for (int k = 0; k < n; ++k)
            {
                const uint32_t id = uint32_t(300 + k);
                if (n > 2 && hash01(o.seed, 32, wid, id) < damage * 0.25 * messy)
                    continue;
                const int bp = pick(Board, wid, id);
                const gen::Fit f = gen::fitPiece(pieces.slot[Board][size_t(bp)], false, true);
                const double len = width + 2.0 * o.boardOverhang * (1.0 + 0.3 * messy * (hash01(o.seed, 33, wid, id) - 0.5));
                const double v = op.v0 + height * (k + 0.5) / n + messy * (hash01(o.seed, 34, wid, id) - 0.5) * bw * 0.3;
                const double u = uc + messy * (hash01(o.seed, 35, wid, id) - 0.5) * o.boardOverhang;
                const double turn = messy * (hash01(o.seed, 36, wid, id) - 0.5) * 2.0 * 12.0;
                const glm::dvec3 c = op.at(u, v, f.size.z * 0.5 + 0.3);
                add(Board, bp, w, gen::rotateAbout(c, op.N, turn) * gen::place(f, c, op.U, op.V, op.N, glm::dvec3(len / f.size.x, 1.0, 1.0)), wid, id);
            }
        }
        else if (state != Glass::None && state != Glass::Boarded && iu1 > iu0 && iv1 > iv0)
        {
            for (int cx = 0; cx < cols; ++cx)
                for (int cy = 0; cy < rows; ++cy)
                {
                    const uint32_t id = uint32_t(400 + cx * 64 + cy);
                    Slot s = state == Glass::Cracked ? Cracked : (state == Glass::Broken ? Broken : Pane);
                    if (state == Glass::Mixed)
                    {
                        const double h = hash01(o.seed, 37, wid, id);
                        s = h < damage * 0.35 ? Broken : (h < damage * 0.7 ? Cracked : Pane);
                    }
                    if (pieces.slot[s].empty())
                        s = Pane;
                    if (pieces.slot[s].empty())
                        continue;
                    const double pu0 = iu0 + (iu1 - iu0) * cx / cols, pu1 = iu0 + (iu1 - iu0) * (cx + 1) / cols;
                    const double pv0 = iv0 + (iv1 - iv0) * cy / rows, pv1 = iv0 + (iv1 - iv0) * (cy + 1) / rows;
                    const int gp = pick(s, wid, id);
                    const gen::Fit f = gen::fitPiece(pieces.slot[s][size_t(gp)]);
                    add(s, gp, w, gen::place(f, op.at((pu0 + pu1) * 0.5, (pv0 + pv1) * 0.5, wf), op.U, op.V, op.N,
                                             glm::dvec3((pu1 - pu0) / f.size.x, (pv1 - pv0) / f.size.y, 1.0)),
                        wid, id);
                    ++(s == Pane ? r.panes : (s == Cracked ? r.cracked : r.broken));
                }
        }

        const double back = -std::max(op.depth, 0.0) - 0.5;
        const double floor = op.door ? op.v0 : op.v0 - 90.0;
        const double roomW = std::max(2.0 * width, 200.0);
        const double roomH = std::max(op.v1 + 40.0 - floor, 250.0);
        const double roomD = std::max(10.0, o.interiorDepth);
        if (o.interior == Interior::Room)
            add(RoomMesh, 0, w, gen::place(gen::Fit{}, op.at(uc, floor, back), op.U, op.V, op.N, glm::dvec3(roomW, roomH, roomD)), wid, 500);
        else if (o.interior == Interior::Card)
            add(CardMesh, 0, w, gen::place(gen::Fit{}, op.at(uc, (op.v0 + op.v1) * 0.5, back - 20.0), op.U, op.V, op.N, glm::dvec3(width * 1.6, height * 1.6, 1.0)),
                wid, 501);
        else if (o.interior == Interior::Mesh && !pieces.slot[InteriorPiece].empty())
        {
            const int ip = pick(InteriorPiece, wid, 502);
            const gen::Fit f = gen::fitPiece(pieces.slot[InteriorPiece][size_t(ip)]);
            const double s = std::min({roomW * 0.9 / f.size.x, roomH * 0.9 / f.size.y, roomD * 0.9 / f.size.z});
            add(InteriorPiece, ip, w, gen::place(f, op.at(uc, floor + f.size.y * s * 0.5, back - roomD * 0.5), op.U, op.V, op.N, glm::dvec3(s)), wid, 502);
        }
    }
    return r;
}

Mesh unitRoom(const std::string& material)
{
    Mesh m;
    gen::addQuad(m, {-0.5, 0, 0}, {0.5, 0, 0}, {0.5, 0, -1}, {-0.5, 0, -1});
    gen::addQuad(m, {-0.5, 1, -1}, {0.5, 1, -1}, {0.5, 1, 0}, {-0.5, 1, 0});
    gen::addQuad(m, {-0.5, 0, -1}, {0.5, 0, -1}, {0.5, 1, -1}, {-0.5, 1, -1});
    gen::addQuad(m, {-0.5, 0, 0}, {-0.5, 0, -1}, {-0.5, 1, -1}, {-0.5, 1, 0});
    gen::addQuad(m, {0.5, 0, -1}, {0.5, 0, 0}, {0.5, 1, 0}, {0.5, 1, -1});
    gen::setMaterial(m, material);
    return m;
}

Mesh unitCard(const std::string& material)
{
    Mesh m;
    gen::addQuad(m, {-0.5, -0.5, 0}, {0.5, -0.5, 0}, {0.5, 0.5, 0}, {-0.5, 0.5, 0});
    gen::setMaterial(m, material);
    return m;
}
} // namespace weathering::window
