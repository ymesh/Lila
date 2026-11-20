#include "EdgeGeometry.h"

#include <lila/Boolean.h>
#include <lila/MeshOps.h>
#include <lila/Profile.h>

#include <glm/gtc/constants.hpp>

#include <algorithm>
#include <cctype>
#include <cmath>
#include <map>
#include <numeric>
#include <unordered_map>

namespace weathering::edges
{
using namespace lila;

namespace
{
constexpr const char* kCutSlot = "__edgeChipCut";
constexpr const char* kBaseSlot = "__edgeChipBase";

double edgeHash(uint32_t seed, uint32_t a, uint32_t b = 0, uint32_t c = 0)
{
    uint32_t h = seed * 0x9E3779B9u ^ (a + 0x7F4A7C15u) * 0x85EBCA6Bu ^ (b + 0x165667B1u) * 0xC2B2AE35u ^ (c + 0x27D4EB2Fu) * 0x9E3779B1u;
    h ^= h >> 16;
    h *= 0x7FEB352Du;
    h ^= h >> 15;
    h *= 0x846CA68Bu;
    h ^= h >> 16;
    return double(h) / 4294967296.0;
}

glm::dvec3 unitOr(const glm::dvec3& v, const glm::dvec3& fallback)
{
    const double l = glm::length(v);
    return l > 1e-12 ? v / l : fallback;
}

double smooth01(double x)
{
    x = std::clamp(x, 0.0, 1.0);
    return x * x * (3.0 - 2.0 * x);
}

std::string lower(std::string s)
{
    std::ranges::transform(s, s.begin(), [](unsigned char c) { return char(std::tolower(c)); });
    return s;
}

/** The two faces of a manifold edge: a runs from point p to q in its winding, b the other way. */
bool edgeFaces(const Mesh& m, Index e, Index p, Index q, Index& a, Index& b)
{
    a = b = geo::kInvalid;
    std::vector<Index> faces;
    m.forEachEdgeCorner(e, [&](Index c) {
        faces.push_back(m.cornerFace(c));
        if (m.cornerPoint(c) == p && m.cornerNextPoint(c) == q)
            a = m.cornerFace(c);
        else if (m.cornerPoint(c) == q && m.cornerNextPoint(c) == p)
            b = m.cornerFace(c);
    });
    if (faces.size() != 2)
        return false;
    if (a == geo::kInvalid || b == geo::kInvalid || a == b)
    {
        a = faces[0];
        b = faces[1];
    }
    return true;
}

/** The chain's frame at each point (and once more at the start for a closed chain), with arc length. */
struct Station
{
    glm::dvec3 p{0.0};
    glm::dvec3 t{0.0};
    glm::dvec3 nA{0.0}, nB{0.0};
    glm::dvec3 aA{0.0}, aB{0.0};
    double s = 0.0;
};

std::vector<Station> stationsOf(const Mesh& m, const Chain& c)
{
    const size_t n = c.points.size();
    std::vector<Station> st(n);
    for (size_t i = 0; i < n; ++i)
        st[i].p = m.position(c.points[i]);
    for (size_t k = 0; k < c.edges.size(); ++k)
    {
        const size_t i = k, j = (k + 1) % n;
        const glm::dvec3 d = st[j].p - st[i].p;
        const glm::dvec3 t = unitOr(d, {1.0, 0.0, 0.0});
        Index fa, fb;
        if (!edgeFaces(m, c.edges[k], c.points[i], c.points[j], fa, fb))
            continue;
        const glm::dvec3 mid = (st[i].p + st[j].p) * 0.5;
        const glm::dvec3 nA = m.faceNormal(fa), nB = m.faceNormal(fb);
        glm::dvec3 aA = unitOr(glm::cross(nA, t), {0.0, 1.0, 0.0}), aB = unitOr(glm::cross(nB, t), {0.0, 1.0, 0.0});
        if (glm::dot(aA, m.faceCentroid(fa) - mid) < 0.0)
            aA = -aA;
        if (glm::dot(aB, m.faceCentroid(fb) - mid) < 0.0)
            aB = -aB;
        for (size_t at : {i, j})
        {
            st[at].t += t;
            st[at].nA += nA;
            st[at].nB += nB;
            st[at].aA += aA;
            st[at].aB += aB;
        }
    }
    for (Station& s : st)
    {
        s.t = unitOr(s.t, {1.0, 0.0, 0.0});
        s.nA = unitOr(s.nA, {0.0, 1.0, 0.0});
        s.nB = unitOr(s.nB, {0.0, 1.0, 0.0});
        s.aA = unitOr(s.aA, {0.0, 0.0, 1.0});
        s.aB = unitOr(s.aB, {0.0, 0.0, -1.0});
    }
    if (c.closed && n > 1)
        st.push_back(st.front());
    for (size_t i = 1; i < st.size(); ++i)
        st[i].s = st[i - 1].s + glm::length(st[i].p - st[i - 1].p);
    return st;
}

Station frameAt(const std::vector<Station>& st, double s)
{
    if (s <= st.front().s)
        return st.front();
    if (s >= st.back().s)
        return st.back();
    const auto it = std::upper_bound(st.begin(), st.end(), s, [](double v, const Station& x) { return v < x.s; });
    const Station& b = *it;
    const Station& a = *(it - 1);
    const double t = b.s > a.s ? (s - a.s) / (b.s - a.s) : 0.0;
    Station r;
    r.p = glm::mix(a.p, b.p, t);
    r.t = unitOr(glm::mix(a.t, b.t, t), a.t);
    r.nA = unitOr(glm::mix(a.nA, b.nA, t), a.nA);
    r.nB = unitOr(glm::mix(a.nB, b.nB, t), a.nB);
    r.aA = unitOr(glm::mix(a.aA, b.aA, t), a.aA);
    r.aB = unitOr(glm::mix(a.aB, b.aB, t), a.aB);
    r.s = s;
    return r;
}

/** Samples of [s0, s1]: the ends, every station between, and steps no longer than step. */
std::vector<double> samplesOf(const std::vector<Station>& st, double s0, double s1, double step)
{
    std::vector<double> out = {s0, s1};
    for (const Station& x : st)
        if (x.s > s0 + 1e-9 && x.s < s1 - 1e-9)
            out.push_back(x.s);
    const int steps = std::clamp(int(std::ceil((s1 - s0) / std::max(step, 1e-6))), 1, 64);
    for (int k = 1; k < steps; ++k)
        out.push_back(s0 + (s1 - s0) * double(k) / steps);
    std::ranges::sort(out);
    out.erase(std::unique(out.begin(), out.end(), [](double a, double b) { return std::abs(a - b) < 1e-7; }), out.end());
    return out;
}

/** Whether each piece of a run is placed, from the seed. */
std::vector<bool> placedPieces(size_t count, double coverage, uint32_t seed, uint32_t chain)
{
    std::vector<bool> out(count);
    for (size_t k = 0; k < count; ++k)
        out[k] = edgeHash(seed, chain, uint32_t(k), 1) < coverage;
    return out;
}

void neighbours(const std::vector<bool>& placed, size_t k, bool closed, bool& prev, bool& next)
{
    const size_t n = placed.size();
    prev = closed ? placed[(k + n - 1) % n] : k > 0 && placed[k - 1];
    next = closed ? placed[(k + 1) % n] : k + 1 < n && placed[k + 1];
}
} // namespace

bool skippedFace(const Mesh& m, Index f)
{
    const std::span<const int32_t> ids = m.faceAttrs().get<int32_t>(geo::attr::kMaterial);
    if (f < ids.size() && ids[f] >= 0 && size_t(ids[f]) < m.materialNames().size())
    {
        const std::string name = lower(m.materialNames()[size_t(ids[f])]);
        if (name.find("nodraw") != std::string::npos || name == "clip" || name == "skip" || name == "toolclip" || name == "toolskip" ||
            name == "tool_clip" || name == "tool_skip")
            return true;
    }
    if (const std::span<const bool> hidden = m.faceAttrs().get<bool>(kHiddenFaceAttr); f < hidden.size() && hidden[f])
        return true;
    if (const std::span<const int32_t> hidden = m.faceAttrs().get<int32_t>(kHiddenFaceAttr); f < hidden.size() && hidden[f] != 0)
        return true;
    return false;
}

std::vector<SharpEdge> sharpEdges(const Mesh& m, double angle, unsigned kinds)
{
    std::vector<SharpEdge> out;
    const std::span<const bool> contact = m.edgeAttrs().get<bool>(kContactEdgeAttr);
    const double cosLimit = std::cos(glm::radians(std::clamp(angle, 0.0, 180.0)));
    for (Index e = 0; e < m.edgeCapacity(); ++e)
    {
        if (!m.edgeValid(e) || m.edgeFaceCount(e) != 2 || (e < contact.size() && contact[e]))
            continue;
        Index f[2], k = 0;
        m.forEachEdgeCorner(e, [&](Index c) {
            if (k < 2)
                f[k++] = m.cornerFace(c);
        });
        if (k != 2 || f[0] == f[1] || skippedFace(m, f[0]) || skippedFace(m, f[1]))
            continue;
        const glm::dvec3 n0 = m.faceNormal(f[0]), n1 = m.faceNormal(f[1]);
        const double d = std::clamp(glm::dot(n0, n1), -1.0, 1.0);
        if (d >= cosLimit)
            continue;
        const glm::dvec3 mid = (m.position(m.edgePoint(e, 0)) + m.position(m.edgePoint(e, 1))) * 0.5;
        const double side = glm::dot(n0, m.faceCentroid(f[1]) - mid) + glm::dot(n1, m.faceCentroid(f[0]) - mid);
        const EdgeKind kind = side < 0.0 ? kConvex : kConcave;
        if (!(kinds & kind))
            continue;
        out.push_back({e, kind, glm::degrees(std::acos(d))});
    }
    return out;
}

std::vector<Chain> chainEdges(const Mesh& m, const std::vector<SharpEdge>& edges, double splitAngle)
{
    std::unordered_map<Index, std::vector<size_t>> at;
    for (size_t i = 0; i < edges.size(); ++i)
        for (int k = 0; k < 2; ++k)
            at[m.edgePoint(edges[i].edge, k)].push_back(i);
    std::vector<bool> used(edges.size(), false);
    const double cosSplit = std::cos(glm::radians(std::clamp(splitAngle, 0.0, 180.0)));

    // From point `from`, the unused edge of the kind that carries on most straight from dir, if it turns little enough.
    auto next = [&](Index from, const glm::dvec3& dir, EdgeKind kind) -> size_t {
        size_t best = edges.size();
        double bestCos = cosSplit - 1e-9;
        for (size_t j : at[from])
        {
            if (used[j] || edges[j].kind != kind)
                continue;
            const Index q = m.edgeOtherPoint(edges[j].edge, from);
            const double c = glm::dot(dir, unitOr(m.position(q) - m.position(from), dir));
            if (c > bestCos)
            {
                bestCos = c;
                best = j;
            }
        }
        return best;
    };

    std::vector<Chain> out;
    for (size_t i = 0; i < edges.size(); ++i)
    {
        if (used[i])
            continue;
        used[i] = true;
        Chain c;
        c.kind = edges[i].kind;
        c.points = {m.edgePoint(edges[i].edge, 0), m.edgePoint(edges[i].edge, 1)};
        c.edges = {edges[i].edge};
        // Forward from the last point.
        for (;;)
        {
            const Index tail = c.points.back();
            const glm::dvec3 dir = unitOr(m.position(tail) - m.position(c.points[c.points.size() - 2]), {1.0, 0.0, 0.0});
            const size_t j = next(tail, dir, c.kind);
            if (j == edges.size())
                break;
            used[j] = true;
            const Index q = m.edgeOtherPoint(edges[j].edge, tail);
            c.edges.push_back(edges[j].edge);
            if (q == c.points.front())
            {
                c.closed = true;
                break;
            }
            c.points.push_back(q);
        }
        // Backward from the first point.
        while (!c.closed)
        {
            const Index head = c.points.front();
            const glm::dvec3 dir = unitOr(m.position(head) - m.position(c.points[1]), {1.0, 0.0, 0.0});
            const size_t j = next(head, dir, c.kind);
            if (j == edges.size())
                break;
            used[j] = true;
            c.points.insert(c.points.begin(), m.edgeOtherPoint(edges[j].edge, head));
            c.edges.insert(c.edges.begin(), edges[j].edge);
        }
        out.push_back(std::move(c));
    }
    return out;
}

Mesh decalStrips(const Mesh& m, const std::vector<Chain>& chains, const std::vector<DecalTrim>& trims, const StripOptions& o, StripStats* stats)
{
    LILA_ZONE();
    Mesh all;
    StripStats local;
    const double width = std::max(o.width, 1e-3);
    for (size_t ci = 0; ci < chains.size(); ++ci)
    {
        const Chain& c = chains[ci];
        std::vector<const DecalTrim*> suit;
        for (const DecalTrim& t : trims)
            if (t.kinds & c.kind)
                suit.push_back(&t);
        if (suit.empty() || c.edges.empty())
            continue;
        const std::vector<Station> st = stationsOf(m, c);
        const double length = st.back().s;
        if (length <= 1e-6)
            continue;
        double segment = o.segment;
        if (segment <= 0.0)
        {
            segment = 0.0;
            for (const DecalTrim* t : suit)
                segment += std::max(t->size.x, 1e-3) * width / std::max(t->size.y, 1e-3);
            segment /= double(suit.size());
        }
        const size_t count = size_t(std::max(1.0, std::round(length / std::max(segment, 1e-3))));
        const std::vector<bool> placed = placedPieces(count, o.coverage, o.seed, uint32_t(ci));
        ++local.chains;
        for (size_t k = 0; k < count; ++k)
        {
            if (!placed[k])
                continue;
            const double s0 = length * double(k) / double(count), s1 = length * double(k + 1) / double(count);
            const DecalTrim& trim = *suit[std::min(suit.size() - 1, size_t(edgeHash(o.seed, uint32_t(ci), uint32_t(k), 2) * double(suit.size())))];
            const double widthScale = 1.0 + std::clamp(o.jitter, 0.0, 0.95) * (2.0 * edgeHash(o.seed, uint32_t(ci), uint32_t(k), 3) - 1.0);
            bool prev, next;
            neighbours(placed, k, c.closed, prev, next);
            const double fade = std::min(std::max(o.fade, 0.0), (s1 - s0) * 0.5);
            auto taper = [&](double s) {
                double t = 1.0;
                if (fade > 1e-6)
                {
                    if (!prev)
                        t *= smooth01((s - s0) / fade);
                    if (!next)
                        t *= smooth01((s1 - s) / fade);
                }
                return std::max(t, 0.02);
            };

            std::vector<glm::dvec3> pts;
            std::vector<glm::vec2> uvs;
            std::vector<float> alpha;
            const std::vector<double> samples = samplesOf(st, s0, s1, fade > 1e-6 ? std::min(fade * 0.34, (s1 - s0) / 4.0) : (s1 - s0) / 4.0);
            const double vMid = (trim.uvMin.y + trim.uvMax.y) * 0.5, uMid = (trim.uvMin.x + trim.uvMax.x) * 0.5;
            for (double s : samples)
            {
                const Station f = frameAt(st, s);
                const double tp = taper(s);
                const double half = 0.5 * width * widthScale * tp;
                const glm::dvec3 up = unitOr(f.nA + f.nB, f.nA);
                pts.push_back(f.p + f.aA * half + f.nA * o.offset);
                // The crest twice, once per side, so each side shades flat.
                pts.push_back(f.p + up * o.offset);
                pts.push_back(f.p + up * o.offset);
                pts.push_back(f.p + f.aB * half + f.nB * o.offset);
                const double along = (s - s0) / (s1 - s0);
                for (int r = 0; r < 4; ++r)
                {
                    if (!trim.alongV)
                    {
                        const double u = trim.uvMin.x + along * (trim.uvMax.x - trim.uvMin.x);
                        uvs.emplace_back(float(u), float(r == 0 ? trim.uvMin.y : r == 3 ? trim.uvMax.y : vMid));
                    }
                    else
                    {
                        const double v = trim.uvMin.y + along * (trim.uvMax.y - trim.uvMin.y);
                        uvs.emplace_back(float(r == 0 ? trim.uvMin.x : r == 3 ? trim.uvMax.x : uMid), float(v));
                    }
                    alpha.push_back(float(tp));
                }
            }
            std::vector<Index> sizes, idx;
            for (size_t i = 0; i + 1 < samples.size(); ++i)
                for (int side = 0; side < 2; ++side)
                {
                    const Index a = Index(i * 4 + side * 2), b = Index((i + 1) * 4 + side * 2), cc = Index((i + 1) * 4 + side * 2 + 1), d = Index(i * 4 + side * 2 + 1);
                    Index quad[4] = {a, b, cc, d};
                    const glm::dvec3 qn = glm::cross(pts[cc] - pts[a], pts[d] - pts[b]);
                    const Station f = frameAt(st, samples[i]);
                    if (glm::dot(qn, side == 0 ? f.nA : f.nB) < 0.0)
                        std::reverse(std::begin(quad), std::end(quad));
                    sizes.push_back(4);
                    idx.insert(idx.end(), std::begin(quad), std::end(quad));
                }
            Mesh piece = Mesh::fromPolygons(pts, sizes, idx);
            std::span<glm::vec2> uv = piece.cornerAttrs().getOrAdd<glm::vec2>(geo::attr::uvName(0));
            for (Index cr = 0; cr < piece.cornerCapacity(); ++cr)
                if (piece.cornerValid(cr))
                    uv[cr] = uvs[piece.cornerPoint(cr)];
            std::span<glm::vec4> col = piece.pointAttrs().getOrAdd<glm::vec4>(geo::attr::kColor);
            for (Index p = 0; p < piece.pointCapacity() && p < alpha.size(); ++p)
                col[p] = glm::vec4(1.0f, 1.0f, 1.0f, alpha[p]);
            if (!o.material.empty())
                scene::materials::assign(piece, o.material, {});
            all.append(piece);
            ++local.pieces;
            ++(c.kind == kConvex ? local.convex : local.concave);
        }
    }
    if (stats)
        *stats = local;
    return all;
}

Mesh sweepPieces(const Mesh& m, const std::vector<Chain>& chains, const std::vector<SweepPiece>& pieces, const SweepOptions& o, StripStats* stats)
{
    LILA_ZONE();
    struct Prepared
    {
        const Mesh* mesh;
        double weight, minX, length;
    };
    std::vector<Prepared> prep;
    double total = 0.0, meanLength = 0.0;
    for (const SweepPiece& p : pieces)
    {
        const geo::Bounds b = p.mesh.bounds();
        if (!b.valid() || b.size().x <= 1e-6 || p.weight <= 0.0)
            continue;
        prep.push_back({&p.mesh, p.weight, b.min.x, b.size().x});
        total += p.weight;
        meanLength += p.weight * b.size().x;
    }
    Mesh all;
    StripStats local;
    if (prep.empty())
    {
        if (stats)
            *stats = local;
        return all;
    }
    meanLength /= total;
    const double scale = std::max(o.scale, 1e-4);
    for (size_t ci = 0; ci < chains.size(); ++ci)
    {
        const Chain& c = chains[ci];
        if (c.edges.empty())
            continue;
        const std::vector<Station> st = stationsOf(m, c);
        const double length = st.back().s;
        if (length <= 1e-6)
            continue;
        ++local.chains;
        const size_t count = size_t(std::max(1.0, std::round(length / (meanLength * scale))));
        const std::vector<bool> placed = placedPieces(count, o.coverage, o.seed, uint32_t(ci));
        for (size_t k = 0; k < count; ++k)
        {
            if (!placed[k])
                continue;
            double u = edgeHash(o.seed, uint32_t(ci), uint32_t(k), 4) * total;
            size_t pick = prep.size() - 1;
            for (size_t i = 0; i < prep.size(); ++i)
                if ((u -= prep[i].weight) < 0.0)
                {
                    pick = i;
                    break;
                }
            const Prepared& p = prep[pick];
            const double s0 = length * double(k) / double(count), s1 = length * double(k + 1) / double(count);
            Mesh piece = *p.mesh;
            for (Index v = 0; v < piece.pointCapacity(); ++v)
            {
                if (!piece.pointValid(v))
                    continue;
                const glm::dvec3 q = piece.position(v);
                const Station f = frameAt(st, s0 + (q.x - p.minX) / p.length * (s1 - s0));
                const glm::dvec3 up = unitOr(f.nA + f.nB, f.nA);
                const glm::dvec3 side = unitOr(glm::cross(f.t, up), f.aA);
                piece.setPosition(v, f.p + up * (q.y * scale + o.offset) + side * (q.z * scale));
            }
            all.append(piece);
            ++local.pieces;
            ++(c.kind == kConvex ? local.convex : local.concave);
        }
    }
    if (stats)
        *stats = local;
    return all;
}

Mesh chipCutter(double length, double radius, double shape, uint32_t seed, uint32_t index)
{
    shape = std::clamp(shape, 0.0, 1.0);
    const int rings = std::max(1, int(std::round(glm::mix(5.0, 2.0, shape))));
    const int around = std::max(3, int(std::round(glm::mix(9.0, 4.0, shape))));
    const double noise = glm::mix(0.45, 0.08, shape);
    std::vector<glm::dvec3> pts;
    uint32_t k = 0;
    auto h = [&] { return edgeHash(seed, index, 7, k++); };
    for (int end : {-1, 1})
    {
        const double rr = radius * glm::mix(0.2, 0.6, shape) * h();
        const double a = glm::two_pi<double>() * h();
        pts.emplace_back(end * length * 0.5 * (0.85 + 0.15 * h()), rr * std::cos(a), rr * std::sin(a));
    }
    const double phase = glm::two_pi<double>() * h();
    for (int r = 0; r < rings; ++r)
    {
        const double x = -length * 0.5 + length * double(r + 1) / double(rings + 1) + (h() - 0.5) * length * 0.1;
        const double t = std::clamp(2.0 * x / length, -1.0, 1.0);
        // Realistic chips taper to points; toon chunks stay blocky to their ends.
        const double profile = glm::mix(std::sqrt(std::max(0.0, 1.0 - t * t)), 1.0 - 0.25 * std::abs(t), shape) * (shape > 0.5 ? 1.0 : 0.9 + 0.1 * h());
        for (int i = 0; i < around; ++i)
        {
            const double a = phase + glm::two_pi<double>() * (double(i) + (h() - 0.5) * 0.6) / double(around);
            const double rad = radius * profile * (1.0 - noise * h());
            pts.emplace_back(x, rad * std::cos(a), rad * std::sin(a));
        }
    }
    return geo::convexHullMesh(pts, 64);
}

ChipResult chipEdges(const Mesh& in, const glm::dmat4& toWorld, const ChipOptions& o, const std::vector<Mesh>& cutters)
{
    LILA_ZONE();
    ChipResult r;
    Mesh m = in;
    const glm::dmat3 lin(toWorld);
    const double scale = std::max(1e-9, std::cbrt(std::abs(glm::determinant(lin))));
    const double size = std::max(o.size, 1e-3) / scale, depth = std::max(o.depth, 1e-4) / scale;
    const double shape = std::clamp(o.shape, 0.0, 1.0);
    const std::vector<SharpEdge> sharp = sharpEdges(m, o.angle, kConvex);
    r.edges = sharp.size();

    // Cutters prepared once: centred, with their widest point at radius 1 from the X axis and length 1.
    std::vector<Mesh> users;
    for (const Mesh& c : cutters)
    {
        const geo::Bounds b = c.bounds();
        if (!b.valid() || b.size().x <= 1e-9)
            continue;
        Mesh u = c;
        const glm::dvec3 centre = b.center();
        double rad = 0.0;
        for (Index p = 0; p < u.pointCapacity(); ++p)
            if (u.pointValid(p))
            {
                const glm::dvec3 q = u.position(p) - centre;
                rad = std::max(rad, std::hypot(q.y, q.z));
            }
        if (rad <= 1e-9)
            continue;
        for (Index p = 0; p < u.pointCapacity(); ++p)
            if (u.pointValid(p))
            {
                const glm::dvec3 q = u.position(p) - centre;
                u.setPosition(p, {q.x / b.size().x, q.y / rad, q.z / rad});
            }
        users.push_back(std::move(u));
    }

    std::vector<Mesh> placed;
    const int subChips = 1 + int(std::round((1.0 - shape) * 2.0));
    uint32_t counter = 0;
    for (const SharpEdge& se : sharp)
    {
        const glm::dvec3 p0 = m.position(m.edgePoint(se.edge, 0)), p1 = m.position(m.edgePoint(se.edge, 1));
        const double len = glm::length(p1 - p0);
        if (len <= 1e-9)
            continue;
        const glm::dvec3 t = (p1 - p0) / len;
        Index fa, fb;
        if (!edgeFaces(m, se.edge, m.edgePoint(se.edge, 0), m.edgePoint(se.edge, 1), fa, fb))
            continue;
        const glm::dvec3 up = unitOr(m.faceNormal(fa) + m.faceNormal(fb), m.faceNormal(fa));
        const glm::dvec3 across = unitOr(glm::cross(t, up), {0.0, 0.0, 1.0});
        const int slots = std::max(1, int(std::floor(len / size)));
        for (int j = 0; j < slots; ++j)
        {
            if (edgeHash(o.seed, se.edge, uint32_t(j), 11) >= o.amount)
                continue;
            if (placed.size() >= o.maxChips)
            {
                r.warning = "Chip limit reached (" + std::to_string(o.maxChips) + "); raise size or lower amount";
                break;
            }
            const double at = (double(j) + 0.5 + (edgeHash(o.seed, se.edge, uint32_t(j), 12) - 0.5) * 0.5) / double(slots);
            const glm::dvec3 centre = p0 + t * (len * at);
            const double chipLength = size * (0.6 + 0.6 * edgeHash(o.seed, se.edge, uint32_t(j), 13));
            const double chipRadius = depth * (0.5 + 0.5 * edgeHash(o.seed, se.edge, uint32_t(j), 14));
            for (int sub = 0; sub < subChips; ++sub)
            {
                const auto hs = [&](uint32_t k) { return edgeHash(o.seed, se.edge, uint32_t(j) * 16u + uint32_t(sub), 20 + k); };
                const double subLength = sub == 0 ? chipLength : chipLength * (0.4 + 0.4 * hs(0));
                const double subRadius = sub == 0 ? chipRadius : chipRadius * (0.6 + 0.4 * hs(1));
                const double shift = sub == 0 ? 0.0 : (hs(2) - 0.5) * chipLength * 0.8;
                const double turn = glm::two_pi<double>() * hs(3);
                Mesh cut;
                if (!users.empty())
                {
                    cut = users[std::min(users.size() - 1, size_t(hs(4) * double(users.size())))];
                    for (Index p = 0; p < cut.pointCapacity(); ++p)
                        if (cut.pointValid(p))
                        {
                            const glm::dvec3 q = cut.position(p);
                            cut.setPosition(p, {q.x * subLength, q.y * subRadius, q.z * subRadius});
                        }
                }
                else
                    cut = chipCutter(subLength, subRadius, shape, o.seed, counter);
                ++counter;
                const double c = std::cos(turn), s = std::sin(turn);
                const glm::dvec3 ey = up * c - across * s, ez = up * s + across * c;
                for (Index p = 0; p < cut.pointCapacity(); ++p)
                    if (cut.pointValid(p))
                    {
                        const glm::dvec3 q = cut.position(p);
                        cut.setPosition(p, centre + t * (q.x + shift) + ey * q.y + ez * q.z);
                    }
                scene::materials::assign(cut, kCutSlot, {});
                placed.push_back(std::move(cut));
            }
        }
    }

    if (o.bevel > 0.0 && !sharp.empty())
    {
        std::vector<Index> ids;
        for (const SharpEdge& se : sharp)
            ids.push_back(se.edge);
        geo::BevelOptions bv;
        bv.offset = o.bevel / scale;
        bv.segments = 1;
        bv.smoothingAngle = 30.0;
        geo::bevelEdges(m, ids, bv);
    }
    if (placed.empty())
    {
        r.mesh = std::move(m);
        return r;
    }

    const bool hadSlots = !m.materialNames().empty() && m.faceAttrs().has(geo::attr::kMaterial);
    if (!hadSlots)
        scene::materials::assign(m, kBaseSlot, {});
    std::vector<scene::BooleanOperand> ops = {{&m, glm::dmat4(1.0)}};
    for (const Mesh& c : placed)
        ops.push_back({&c, glm::dmat4(1.0)});
    scene::BooleanOptions bo;
    bo.op = scene::BooleanOp::Difference;
    bo.cleanup = false;
    bo.splitNgons = false;
    bo.hardAngle = 30.0;
    scene::BooleanResult b = scene::meshBoolean(ops, glm::dmat4(1.0), bo);
    if (!b.ok())
    {
        r.warning = "Chips not cut: " + b.error;
        r.mesh = in;
        return r;
    }
    Mesh out = std::move(b.mesh);
    r.chips = placed.size();

    // Cut faces take the slot of the face they broke into (or the given one) and world-projected uvs.
    const geo::MeshBvh original(m);
    std::map<std::string, std::vector<uint32_t>> bySlot;
    std::vector<Index> cutFaces;
    const std::span<const int32_t> slots = std::as_const(out).faceAttrs().get<int32_t>(geo::attr::kMaterial);
    for (Index f = 0; f < out.faceCapacity(); ++f)
    {
        if (!out.faceValid(f) || f >= slots.size() || scene::materials::slotName(out, slots[f]) != kCutSlot)
            continue;
        cutFaces.push_back(f);
        std::string slot = o.material;
        if (slot.empty())
        {
            geo::BvhClosest hit;
            const std::span<const int32_t> own = m.faceAttrs().get<int32_t>(geo::attr::kMaterial);
            if (original.closestPoint(out.faceCentroid(f), hit) && hit.face < own.size())
                slot = scene::materials::slotName(m, own[hit.face]);
        }
        bySlot[slot.empty() ? std::string(kBaseSlot) : slot].push_back(f);
    }
    for (const auto& [slot, faces] : bySlot)
        scene::materials::assign(out, slot, faces);
    if (!hadSlots)
    {
        if (o.material.empty())
        {
            out.faceAttrs().remove(geo::attr::kMaterial);
            out.materialNames().clear();
        }
        else
        {
            std::vector<uint32_t> rest;
            const std::span<const int32_t> now = std::as_const(out).faceAttrs().get<int32_t>(geo::attr::kMaterial);
            for (Index f = 0; f < out.faceCapacity(); ++f)
                if (out.faceValid(f) && f < now.size() && scene::materials::slotName(out, now[f]) == kBaseSlot)
                    rest.push_back(f);
            scene::materials::assign(out, scene::materials::kDefaultSlot, rest);
        }
    }

    const glm::dmat3 normalToWorld = glm::transpose(glm::inverse(lin));
    const double uvScale = std::max(o.uvScale, 1e-6);
    std::span<glm::vec2> uv = out.cornerAttrs().getOrAdd<glm::vec2>(geo::attr::uvName(0));
    for (Index f : cutFaces)
    {
        const glm::dvec3 n = glm::abs(unitOr(normalToWorld * out.faceNormal(f), {0.0, 1.0, 0.0}));
        const int axis = n.x >= n.y && n.x >= n.z ? 0 : n.y >= n.z ? 1 : 2;
        out.forEachFaceCorner(f, [&](Index c) {
            const glm::dvec3 w = glm::dvec3(toWorld * glm::dvec4(out.position(out.cornerPoint(c)), 1.0));
            const glm::dvec2 p = axis == 0 ? glm::dvec2(w.z, w.y) : axis == 1 ? glm::dvec2(w.x, w.z) : glm::dvec2(w.x, w.y);
            uv[c] = glm::vec2(p / uvScale);
        });
    }
    r.mesh = std::move(out);
    return r;
}
} // namespace weathering::edges
