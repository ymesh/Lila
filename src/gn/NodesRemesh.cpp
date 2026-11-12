#include "gn/NodesCommon.h"

#include "core/Profile.h"
#include "curve/Cdt.h"
#include "geo/Bvh.h"
#include "geo/Topology.h"
#include "geo/Triangulate.h"

#include <algorithm>
#include <array>
#include <map>
#include <numeric>
#include <unordered_map>
#include <unordered_set>

namespace lila::gn::nodes
{
namespace isoremesh
{
uint64_t edgeKey(int a, int b)
{
    if (a > b)
        std::swap(a, b);
    return (uint64_t(uint32_t(a)) << 32) | uint32_t(b);
}

enum FeatureBit : uint8_t
{
    kAngle = 1,
    kHard = 2,
    kSeam = 4,
    kBorder = 8,
};

/** A source edge on a feature line, for pulling remeshed feature points back onto it. */
struct Segment
{
    D3 a, b;
    int pa, pb;
};

struct Remesher
{
    std::vector<D3> pos;
    std::vector<std::array<int, 3>> tris; // tris[t][0] < 0 when removed
    std::unordered_map<uint64_t, uint8_t> feature;
    std::vector<uint8_t> kind;   // per point: 0 free, 1 on a feature line, 2 fixed
    std::vector<double> target;  // per point edge length wanted
    std::vector<int> hint;       // per feature point: the source segment it was last nearest to

    std::vector<Segment> segments;
    std::vector<std::vector<int>> segmentsAt; // per source point
    const geo::MeshBvh* bvh = nullptr;
    std::vector<double> sourceTarget; // per source point
    const geo::Mesh* source = nullptr;

    std::vector<std::vector<int>> vertTris;
    std::unordered_map<uint64_t, std::vector<int>> edgeTris;

    bool alive(int t) const { return tris[size_t(t)][0] >= 0; }

    void buildAdjacency()
    {
        vertTris.assign(pos.size(), {});
        edgeTris.clear();
        edgeTris.reserve(tris.size() * 2);
        for (int t = 0; t < int(tris.size()); ++t)
        {
            if (!alive(t))
                continue;
            const auto& v = tris[size_t(t)];
            for (int i = 0; i < 3; ++i)
            {
                vertTris[size_t(v[size_t(i)])].push_back(t);
                edgeTris[edgeKey(v[size_t(i)], v[size_t((i + 1) % 3)])].push_back(t);
            }
        }
    }

    double edgeTarget(int a, int b) const { return 0.5 * (target[size_t(a)] + target[size_t(b)]); }

    D3 triNormal(const std::array<int, 3>& v) const
    {
        return glm::cross(pos[size_t(v[1])] - pos[size_t(v[0])], pos[size_t(v[2])] - pos[size_t(v[0])]);
    }

    int addPoint(const D3& p, uint8_t k, double t, int h)
    {
        pos.push_back(p);
        kind.push_back(k);
        target.push_back(t);
        hint.push_back(h);
        return int(pos.size()) - 1;
    }

    bool splitPass()
    {
        buildAdjacency();
        struct Long
        {
            double len;
            int a, b;
        };
        std::vector<Long> longs;
        for (const auto& [key, ts] : edgeTris)
        {
            int a = int(key >> 32), b = int(key & 0xffffffffu);
            double len = glm::length(pos[size_t(a)] - pos[size_t(b)]);
            if (len > edgeTarget(a, b) * 4.0 / 3.0)
                longs.push_back({len, a, b});
        }
        std::sort(longs.begin(), longs.end(), [](const Long& x, const Long& y) { return x.len != y.len ? x.len > y.len : edgeKey(x.a, x.b) < edgeKey(y.a, y.b); });
        std::vector<uint8_t> locked(tris.size(), 0);
        bool any = false;
        for (const Long& l : longs)
        {
            const std::vector<int>& ts = edgeTris[edgeKey(l.a, l.b)];
            if (std::any_of(ts.begin(), ts.end(), [&](int t) { return locked[size_t(t)] != 0; }))
                continue;
            auto f = feature.find(edgeKey(l.a, l.b));
            uint8_t mask = f != feature.end() ? f->second : 0;
            int m = addPoint((pos[size_t(l.a)] + pos[size_t(l.b)]) * 0.5, mask ? 1 : 0, edgeTarget(l.a, l.b), mask ? hint[size_t(l.a)] : -1);
            if (mask)
            {
                feature.erase(edgeKey(l.a, l.b));
                feature[edgeKey(l.a, m)] = mask;
                feature[edgeKey(m, l.b)] = mask;
            }
            for (int t : std::vector<int>(ts))
            {
                std::array<int, 3> v = tris[size_t(t)];
                int i = 0;
                while (!((v[size_t(i)] == l.a && v[size_t((i + 1) % 3)] == l.b) || (v[size_t(i)] == l.b && v[size_t((i + 1) % 3)] == l.a)))
                    ++i;
                int x = v[size_t(i)], y = v[size_t((i + 1) % 3)], z = v[size_t((i + 2) % 3)];
                tris[size_t(t)] = {x, m, z};
                tris.push_back({m, y, z});
                locked[size_t(t)] = 1;
                locked.push_back(1);
            }
            any = true;
        }
        return any;
    }

    void collapsePass()
    {
        buildAdjacency();
        struct Short
        {
            double len;
            int a, b;
        };
        std::vector<Short> shorts;
        for (const auto& [key, ts] : edgeTris)
        {
            int a = int(key >> 32), b = int(key & 0xffffffffu);
            double len = glm::length(pos[size_t(a)] - pos[size_t(b)]);
            if (len < edgeTarget(a, b) * 0.8 && (ts.size() == 1 || ts.size() == 2))
                shorts.push_back({len, a, b});
        }
        std::sort(shorts.begin(), shorts.end(), [](const Short& x, const Short& y) { return x.len != y.len ? x.len < y.len : edgeKey(x.a, x.b) < edgeKey(y.a, y.b); });
        std::vector<uint8_t> locked(pos.size(), 0);
        auto ring = [&](int p) {
            std::vector<int> r;
            for (int t : vertTris[size_t(p)])
                if (alive(t))
                    for (int v : tris[size_t(t)])
                        if (v != p && std::find(r.begin(), r.end(), v) == r.end())
                            r.push_back(v);
            return r;
        };
        for (const Short& s : shorts)
        {
            int a = s.a, b = s.b;
            if (locked[size_t(a)] || locked[size_t(b)])
                continue;
            bool onFeature = feature.count(edgeKey(a, b)) > 0;
            int ka = kind[size_t(a)], kb = kind[size_t(b)];
            if (ka == 2 && kb == 2)
                continue;
            if (ka >= 1 && kb >= 1 && !onFeature)
                continue;
            // remove collapses onto keep; the point with more constraints stays.
            int keep = kb >= ka ? b : a, remove = keep == b ? a : b;
            if (kind[size_t(remove)] == 2)
                continue;
            D3 p = ka == 0 && kb == 0 ? (pos[size_t(a)] + pos[size_t(b)]) * 0.5 : pos[size_t(keep)];
            const std::vector<int>& shared = edgeTris[edgeKey(a, b)];
            std::vector<int> ra = ring(a), rb = ring(b);
            int common = 0;
            for (int v : ra)
                if (std::find(rb.begin(), rb.end(), v) != rb.end())
                    ++common;
            if (common != int(shared.size()))
                continue;
            const double high = edgeTarget(a, b) * 4.0 / 3.0;
            bool ok = true;
            for (int v : ra)
                ok &= v == b || glm::length(p - pos[size_t(v)]) <= high;
            for (int v : rb)
                ok &= v == a || glm::length(p - pos[size_t(v)]) <= high;
            if (!ok)
                continue;
            for (int end : {a, b})
                for (int t : vertTris[size_t(end)])
                {
                    if (!alive(t))
                        continue;
                    std::array<int, 3> v = tris[size_t(t)];
                    if (std::find(v.begin(), v.end(), a) != v.end() && std::find(v.begin(), v.end(), b) != v.end())
                        continue;
                    D3 before = triNormal(v);
                    for (int& x : v)
                        if (x == a || x == b)
                            x = -1;
                    std::array<D3, 3> q;
                    for (int i = 0; i < 3; ++i)
                        q[size_t(i)] = v[size_t(i)] < 0 ? p : pos[size_t(v[size_t(i)])];
                    D3 after = glm::cross(q[1] - q[0], q[2] - q[0]);
                    double la = glm::length(after), lb = glm::length(before);
                    if (la < 1e-12 * std::max(1.0, lb) || glm::dot(after, before) < 0.2 * la * lb)
                        ok = false;
                }
            if (!ok)
                continue;
            std::vector<std::pair<uint64_t, uint8_t>> moved;
            for (int v : ring(remove))
            {
                auto f = feature.find(edgeKey(remove, v));
                if (f == feature.end())
                    continue;
                if (v != keep)
                    moved.push_back({edgeKey(keep, v), f->second});
                feature.erase(f);
            }
            for (auto [k, m] : moved)
                feature[k] = m;
            for (int t : vertTris[size_t(remove)])
            {
                if (!alive(t))
                    continue;
                auto& v = tris[size_t(t)];
                if (std::find(v.begin(), v.end(), keep) != v.end())
                {
                    v = {-1, -1, -1};
                    continue;
                }
                for (int& x : v)
                    if (x == remove)
                        x = keep;
                vertTris[size_t(keep)].push_back(t);
            }
            pos[size_t(keep)] = p;
            locked[size_t(a)] = locked[size_t(b)] = 1;
            for (int v : ra)
                locked[size_t(v)] = 1;
            for (int v : rb)
                locked[size_t(v)] = 1;
        }
    }

    void flipPass()
    {
        buildAdjacency();
        std::vector<int> valence(pos.size(), 0);
        std::vector<uint8_t> border(pos.size(), 0);
        for (const auto& [key, ts] : edgeTris)
        {
            int a = int(key >> 32), b = int(key & 0xffffffffu);
            ++valence[size_t(a)];
            ++valence[size_t(b)];
            if (ts.size() != 2)
                border[size_t(a)] = border[size_t(b)] = 1;
        }
        auto ideal = [&](int p) { return border[size_t(p)] ? 4 : 6; };
        std::vector<uint8_t> locked(tris.size(), 0);
        std::unordered_set<uint64_t> made;
        std::vector<uint64_t> keys;
        keys.reserve(edgeTris.size());
        for (const auto& kv : edgeTris)
            keys.push_back(kv.first);
        std::sort(keys.begin(), keys.end());
        for (uint64_t key : keys)
        {
            const std::vector<int>& ts = edgeTris[key];
            if (ts.size() != 2 || feature.count(key) || locked[size_t(ts[0])] || locked[size_t(ts[1])])
                continue;
            int a = int(key >> 32), b = int(key & 0xffffffffu);
            auto other = [&](int t) {
                for (int v : tris[size_t(t)])
                    if (v != a && v != b)
                        return v;
                return -1;
            };
            int c = other(ts[0]), d = other(ts[1]);
            if (c < 0 || d < 0 || c == d || edgeTris.count(edgeKey(c, d)) || made.count(edgeKey(c, d)))
                continue;
            auto dev = [&](int p, int delta) {
                int x = valence[size_t(p)] + delta - ideal(p);
                return x * x;
            };
            int before = dev(a, 0) + dev(b, 0) + dev(c, 0) + dev(d, 0);
            int after = dev(a, -1) + dev(b, -1) + dev(c, 1) + dev(d, 1);
            if (after >= before)
                continue;
            // Orient so t1 runs a -> b.
            std::array<int, 3> t1 = tris[size_t(ts[0])];
            bool forward = false;
            for (int i = 0; i < 3; ++i)
                forward |= t1[size_t(i)] == a && t1[size_t((i + 1) % 3)] == b;
            int x = forward ? a : b, y = forward ? b : a;
            int up = forward ? c : d, down = forward ? d : c;
            // x -> y -> up and y -> x -> down become x -> down -> up and down -> y -> up.
            std::array<int, 3> n1 = {x, down, up}, n2 = {down, y, up};
            D3 o = triNormal(tris[size_t(ts[0])]) + triNormal(tris[size_t(ts[1])]);
            D3 m1 = triNormal(n1), m2 = triNormal(n2);
            double l1 = glm::length(m1), l2 = glm::length(m2), lo = glm::length(o);
            if (l1 < 1e-12 * lo || l2 < 1e-12 * lo || glm::dot(m1, o) <= 0.0 || glm::dot(m2, o) <= 0.0 || glm::dot(m1, m2) < 0.5 * l1 * l2)
                continue;
            int tu = forward ? ts[0] : ts[1], td = forward ? ts[1] : ts[0];
            tris[size_t(tu)] = n1;
            tris[size_t(td)] = n2;
            locked[size_t(tu)] = locked[size_t(td)] = 1;
            made.insert(edgeKey(c, d));
            --valence[size_t(a)];
            --valence[size_t(b)];
            ++valence[size_t(c)];
            ++valence[size_t(d)];
        }
    }

    /** Nearest point on the feature lines, walking from the segment it was last near. */
    D3 onFeature(int p)
    {
        int s = hint[size_t(p)];
        if (s < 0 || segments.empty())
            return pos[size_t(p)];
        const D3 q = pos[size_t(p)];
        auto closest = [&](int k, D3& at) {
            const Segment& g = segments[size_t(k)];
            D3 d = g.b - g.a;
            double t = std::clamp(glm::dot(q - g.a, d) / std::max(1e-24, glm::dot(d, d)), 0.0, 1.0);
            at = g.a + d * t;
            return glm::length(at - q);
        };
        D3 best;
        double bestD = closest(s, best);
        for (int guard = 0; guard < 64; ++guard)
        {
            int next = -1;
            for (int end : {segments[size_t(s)].pa, segments[size_t(s)].pb})
                for (int k : segmentsAt[size_t(end)])
                {
                    D3 at;
                    double d = closest(k, at);
                    if (d < bestD - 1e-12)
                    {
                        bestD = d;
                        best = at;
                        next = k;
                    }
                }
            if (next < 0)
                break;
            s = next;
        }
        hint[size_t(p)] = s;
        return best;
    }

    void relaxPass()
    {
        buildAdjacency();
        std::vector<D3> normal(pos.size(), D3(0.0));
        for (int t = 0; t < int(tris.size()); ++t)
            if (alive(t))
            {
                D3 n = triNormal(tris[size_t(t)]);
                for (int v : tris[size_t(t)])
                    normal[size_t(v)] += n;
            }
        std::vector<D3> next = pos;
        for (int p = 0; p < int(pos.size()); ++p)
        {
            if (vertTris[size_t(p)].empty() || kind[size_t(p)] == 2)
                continue;
            if (kind[size_t(p)] == 1)
            {
                std::vector<int> along;
                for (int t : vertTris[size_t(p)])
                    for (int v : tris[size_t(t)])
                        if (v != p && feature.count(edgeKey(p, v)) && std::find(along.begin(), along.end(), v) == along.end())
                            along.push_back(v);
                if (along.size() != 2)
                    continue;
                D3 dir = pos[size_t(along[1])] - pos[size_t(along[0])];
                double l = glm::length(dir);
                if (l < 1e-12)
                    continue;
                dir /= l;
                D3 mid = (pos[size_t(along[0])] + pos[size_t(along[1])]) * 0.5;
                next[size_t(p)] = pos[size_t(p)] + dir * (0.5 * glm::dot(mid - pos[size_t(p)], dir));
                continue;
            }
            D3 sum(0.0);
            double w = 0.0;
            for (int t : vertTris[size_t(p)])
            {
                const auto& v = tris[size_t(t)];
                double area = glm::length(triNormal(v));
                D3 centroid = (pos[size_t(v[0])] + pos[size_t(v[1])] + pos[size_t(v[2])]) / 3.0;
                sum += centroid * area;
                w += area;
            }
            if (w <= 0.0)
                continue;
            D3 n = normal[size_t(p)];
            double nl = glm::length(n);
            D3 move = sum / w - pos[size_t(p)];
            if (nl > 1e-12)
            {
                n /= nl;
                move -= n * glm::dot(move, n);
            }
            next[size_t(p)] = pos[size_t(p)] + move;
        }
        pos = std::move(next);
    }

    void projectPass(double adaptivity, double length)
    {
        for (int p = 0; p < int(pos.size()); ++p)
        {
            if (vertTris.size() > size_t(p) && vertTris[size_t(p)].empty())
                continue;
            if (kind[size_t(p)] == 1)
                pos[size_t(p)] = onFeature(p);
            geo::BvhClosest hit;
            if (!bvh->closestPoint(pos[size_t(p)], hit))
                continue;
            if (kind[size_t(p)] == 0)
                pos[size_t(p)] = hit.point;
            if (adaptivity > 0.0)
            {
                const Index* c = &bvh->triangles().corners[size_t(hit.triangle) * 3];
                double t = 0.0;
                for (int i = 0; i < 3; ++i)
                    t += hit.bary[i] * sourceTarget[source->cornerPoint(c[i])];
                target[size_t(p)] = t;
            }
            else
                target[size_t(p)] = length;
        }
    }
};

/** Per point: how sharply the surface turns there, radians per cm. */
std::vector<double> curvature(const geo::Mesh& m)
{
    std::vector<D3> n(m.pointCapacity(), D3(0.0));
    for (Index f = 0; f < m.faceCapacity(); ++f)
        if (m.faceValid(f))
        {
            D3 a = m.faceAreaVector(f);
            m.forEachFaceCorner(f, [&](Index k) { n[m.cornerPoint(k)] += a; });
        }
    for (D3& v : n)
        if (glm::length(v) > 1e-20)
            v = glm::normalize(v);
    std::vector<double> k(m.pointCapacity(), 0.0);
    for (Index e = 0; e < m.edgeCapacity(); ++e)
    {
        if (!m.edgeValid(e))
            continue;
        Index a = m.edgePoint(e, 0), b = m.edgePoint(e, 1);
        double len = m.edgeLength(e);
        if (len < 1e-12)
            continue;
        double turn = std::acos(std::clamp(glm::dot(n[a], n[b]), -1.0, 1.0)) / len;
        k[a] = std::max(k[a], turn);
        k[b] = std::max(k[b], turn);
    }
    return k;
}

bool uvSplit(const geo::Mesh& m, std::span<const glm::vec2> uv, Index e)
{
    Index k1 = m.edgeCorner(e), k2 = m.cornerRadial(k1);
    auto same = [](glm::vec2 a, glm::vec2 b) { return glm::length(a - b) <= 1e-6f; };
    if (m.cornerPoint(k1) == m.cornerPoint(k2))
        return !same(uv[k1], uv[k2]) || !same(uv[m.cornerNext(k1)], uv[m.cornerNext(k2)]);
    return !same(uv[k1], uv[m.cornerNext(k2)]) || !same(uv[m.cornerNext(k1)], uv[k2]);
}

struct RemeshOptions
{
    double length = 0.0;
    int iterations = 8;
    double featureAngle = 45.0;
    bool hardEdges = true;
    bool seams = true;
    double adaptivity = 0.0;
};

geo::Mesh remesh(const geo::Mesh& src, const RemeshOptions& o)
{
    LILA_ZONE();
    if (src.numFaces() == 0)
        return src;
    geo::MeshBvh bvh(src);
    Remesher r;
    r.bvh = &bvh;
    r.source = &src;
    double length = o.length;
    if (length <= 0.0)
    {
        double sum = 0.0;
        for (Index e = 0; e < src.edgeCapacity(); ++e)
            if (src.edgeValid(e))
                sum += src.edgeLength(e);
        length = src.numEdges() ? sum / src.numEdges() : 1.0;
    }
    std::vector<double> bend = curvature(src);
    r.sourceTarget.resize(src.pointCapacity());
    for (Index p = 0; p < src.pointCapacity(); ++p)
        r.sourceTarget[p] = std::max(length * 0.2, length / (1.0 + o.adaptivity * 4.0 * bend[p] * length));

    std::vector<int> id(src.pointCapacity(), -1);
    for (Index p = 0; p < src.pointCapacity(); ++p)
        if (src.pointValid(p))
            id[p] = r.addPoint(src.position(p), 0, o.adaptivity > 0.0 ? r.sourceTarget[p] : length, -1);
    geo::Triangulation tri = geo::triangulate(src);
    for (Index t = 0; t < tri.triangleCount(); ++t)
        r.tris.push_back({id[src.cornerPoint(tri.corners[t * 3])], id[src.cornerPoint(tri.corners[t * 3 + 1])], id[src.cornerPoint(tri.corners[t * 3 + 2])]});

    auto hard = src.edgeAttrs().get<bool>(geo::attr::kHard);
    auto seam = src.edgeAttrs().get<bool>(geo::attr::kSeam);
    auto uv = src.cornerAttrs().get<glm::vec2>(geo::attr::uvName(0));
    const double cosLimit = std::cos(glm::radians(o.featureAngle));
    r.segmentsAt.resize(src.pointCapacity());
    std::vector<int> featureCount(src.pointCapacity(), 0);
    std::vector<uint8_t> manifold(src.pointCapacity(), 1);
    for (Index e = 0; e < src.edgeCapacity(); ++e)
    {
        if (!src.edgeValid(e) || src.edgeCorner(e) == geo::kInvalid)
            continue;
        Index faces = src.edgeFaceCount(e);
        uint8_t mask = 0;
        if (faces == 1)
            mask |= kBorder;
        else if (faces > 2)
        {
            mask |= kAngle;
            manifold[src.edgePoint(e, 0)] = manifold[src.edgePoint(e, 1)] = 0;
        }
        else
        {
            Index c = src.edgeCorner(e);
            if (o.featureAngle >= 0.0 && glm::dot(src.faceNormal(src.cornerFace(c)), src.faceNormal(src.cornerFace(src.cornerRadial(c)))) < cosLimit)
                mask |= kAngle;
            if (o.hardEdges && !hard.empty() && hard[e])
                mask |= kHard;
            if (o.seams && ((!seam.empty() && seam[e]) || (!uv.empty() && uvSplit(src, uv, e))))
                mask |= kSeam;
        }
        if (!mask)
            continue;
        Index a = src.edgePoint(e, 0), b = src.edgePoint(e, 1);
        r.feature[edgeKey(id[a], id[b])] = mask;
        int s = int(r.segments.size());
        r.segments.push_back({src.position(a), src.position(b), int(a), int(b)});
        r.segmentsAt[a].push_back(s);
        r.segmentsAt[b].push_back(s);
        ++featureCount[a];
        ++featureCount[b];
    }
    for (Index p = 0; p < src.pointCapacity(); ++p)
    {
        if (id[p] < 0 || featureCount[p] == 0)
            continue;
        r.kind[size_t(id[p])] = featureCount[p] == 2 && manifold[p] ? 1 : 2;
        r.hint[size_t(id[p])] = r.segmentsAt[p].front();
    }

    for (int it = 0; it < o.iterations; ++it)
    {
        for (int pass = 0; pass < 6 && r.splitPass(); ++pass)
            ;
        r.collapsePass();
        r.flipPass();
        r.relaxPass();
        r.projectPass(o.adaptivity, length);
    }

    // Output, with attributes sampled from the nearest place on the source.
    geo::Mesh out;
    for (int d = 0; d < 4; ++d)
        out.attributes(geo::Domain(d)).mergeLayout(src.attributes(geo::Domain(d)));
    out.cornerAttrs().remove(geo::attr::kNormal);
    out.materialNames() = src.materialNames();
    out.colorSetInfo() = src.colorSetInfo();
    std::vector<Index> newId(r.pos.size(), geo::kInvalid);
    for (const auto& t : r.tris)
        if (t[0] >= 0)
            for (int v : t)
                if (newId[size_t(v)] == geo::kInvalid)
                    newId[size_t(v)] = out.addPoint(r.pos[size_t(v)]);
    auto sample = [&](const D3& p, std::array<Index, 3>& corners, std::array<float, 3>& w, Index& face) {
        geo::BvhClosest hit;
        if (!bvh.closestPoint(p, hit))
            return false;
        const Index* c = &bvh.triangles().corners[size_t(hit.triangle) * 3];
        corners = {c[0], c[1], c[2]};
        w = {float(hit.bary.x), float(hit.bary.y), float(hit.bary.z)};
        face = hit.face;
        return true;
    };
    std::array<Index, 3> corners;
    std::array<float, 3> w;
    Index face = geo::kInvalid;
    for (int v = 0; v < int(r.pos.size()); ++v)
    {
        if (newId[size_t(v)] == geo::kInvalid || !sample(r.pos[size_t(v)], corners, w, face))
            continue;
        std::array<Index, 3> pts = {src.cornerPoint(corners[0]), src.cornerPoint(corners[1]), src.cornerPoint(corners[2])};
        out.pointAttrs().mixFrom(src.pointAttrs(), newId[size_t(v)], pts, w);
    }
    auto outHard = out.edgeAttrs().find(geo::attr::kHard);
    auto outSeam = out.edgeAttrs().find(geo::attr::kSeam);
    for (const auto& t : r.tris)
    {
        if (t[0] < 0)
            continue;
        Index f = out.addFace({newId[size_t(t[0])], newId[size_t(t[1])], newId[size_t(t[2])]});
        if (f == geo::kInvalid)
            continue;
        D3 centroid = (r.pos[size_t(t[0])] + r.pos[size_t(t[1])] + r.pos[size_t(t[2])]) / 3.0;
        if (sample(centroid, corners, w, face))
            out.faceAttrs().copyFrom(src.faceAttrs(), f, face);
        for (int i = 0; i < 3; ++i)
        {
            Index k = out.findCorner(f, newId[size_t(t[size_t(i)])]);
            // Sampled a little inside the triangle so corners on a uv seam read their own side of it.
            D3 at = glm::mix(r.pos[size_t(t[size_t(i)])], centroid, 0.1);
            if (k != geo::kInvalid && sample(at, corners, w, face))
                out.cornerAttrs().mixFrom(src.cornerAttrs(), k, corners, w);
            auto fe = r.feature.find(edgeKey(t[size_t(i)], t[size_t((i + 1) % 3)]));
            if (fe == r.feature.end() || k == geo::kInvalid)
                continue;
            Index e = out.cornerEdge(k);
            if (outHard && (fe->second & kHard))
                outHard->values<bool>()[e] = true;
            if (outSeam && (fe->second & kSeam) && !seam.empty())
                outSeam->values<bool>()[e] = true;
        }
    }
    return out;
}

void remeshNode()
{
    registerDecl({
        .type = "IsotropicRemesh",
        .label = "Isotropic Remesh",
        .category = "Mesh",
        .doc = "Rebuilds the surface as even triangles of the target edge length, kept on the original shape. Sharp, hard, seam and border edges "
               "stay as lines in the new mesh. Adaptivity makes triangles smaller where the surface curves. Attributes and uvs are sampled from "
               "the original.",
        .sockets = {.inputs = {geoIn("mesh"), bounded(in("edgeLength", T::Float, 0.0, "Target edge length in cm; 0 keeps the mean edge length."), 0.0),
                               bounded(in("iterations", T::Int, 8), 1, 100),
                               bounded(in("featureAngle", T::Float, 45.0, "Edges sharper than this many degrees are kept; negative off."), -1.0, 180.0),
                               in("keepHardEdges", T::Bool, true), in("keepSeams", T::Bool, true, "Uv seams stay as lines, so uvs carry over cleanly."),
                               bounded(in("adaptivity", T::Float, 0.0, "0 even, 1 down to a fifth of the length where curved."), 0.0, 1.0)},
                    .outputs = {geoOut("mesh")}},
        .exec =
            [](ExecContext& c) {
                RemeshOptions o;
                o.length = std::max(0.0, c.f("edgeLength"));
                o.iterations = int(std::clamp<int64_t>(c.i("iterations"), 1, 100));
                o.featureAngle = c.f("featureAngle");
                o.hardEdges = c.b("keepHardEdges");
                o.seams = c.b("keepSeams");
                o.adaptivity = std::clamp(c.f("adaptivity"), 0.0, 1.0);
                GeometryPtr g = c.geometry("mesh");
                if (g->mesh && o.length > 0.0)
                {
                    double area = 0.0;
                    for (Index f = 0; f < g->mesh->faceCapacity(); ++f)
                        if (g->mesh->faceValid(f))
                            area += g->mesh->faceArea(f);
                    double estimate = area / (o.length * o.length * 0.433);
                    if (estimate > 2.0e7)
                        throw ExecError(c.node.name + ": an edge length of " + std::to_string(o.length) + " cm would make about " +
                                        std::to_string(int64_t(estimate)) + " triangles; raise it");
                }
                c.out("mesh", editComponents(g, {.mesh = [&](geo::Mesh& m) { m = remesh(m, o); }}));
            },
    });
}
} // namespace isoremesh

namespace tri2d
{
void triangulate2dNode()
{
    registerDecl({
        .type = "Triangulate2D",
        .label = "Triangulate 2D",
        .category = "Mesh",
        .doc = "Triangulates points, curves and mesh outlines (open borders and loose edges) in a plane, keeping each as triangle edges. Inside fills only what closed "
               "curves enclose, with curves inside others making holes; hull fills the whole outline. Points keep their height off the plane, so "
               "scattered heights make a terrain surface.",
        .props = {prop("fill", {"inside", "hull"}, "inside", "Inside falls back to the hull when there are no closed curves.")},
        .sockets = {.inputs = {geoIn(), in("normal", T::Vector, vec(0, 1, 0), "The plane's up; triangles face it.")},
                    .outputs = {geoOut("mesh"), out("lostEdges", T::Int, "Curve or edge segments that could not be kept, such as ones crossing another.")}},
        .exec =
            [](ExecContext& c) {
                LILA_ZONE();
                D3 n = c.v("normal");
                if (glm::length(n) < 1e-12)
                    throw ExecError(c.node.name + ": the plane normal is zero");
                n = glm::normalize(n);
                D3 u = std::abs(n.x) < 0.9 ? glm::normalize(glm::cross(n, D3(1, 0, 0))) : glm::normalize(glm::cross(n, D3(0, 1, 0)));
                D3 v = glm::cross(n, u);
                GeometryPtr gIn = c.geometry("geometry");
                Geometry g = gIn->instances ? realize(*gIn) : *gIn;
                std::vector<D3> pts;
                std::vector<std::pair<int, int>> segs;
                bool closed = false;
                if (g.points)
                    for (Index p = 0; p < g.points->pointCapacity(); ++p)
                        if (g.points->pointValid(p))
                            pts.push_back(g.points->position(p));
                if (g.mesh)
                {
                    const geo::Mesh& m = *g.mesh;
                    int base = int(pts.size());
                    for (Index p = 0; p < m.pointCapacity(); ++p)
                        pts.push_back(m.position(p));
                    std::vector<Index> outline;
                    for (Index e = 0; e < m.edgeCapacity(); ++e)
                        if (m.edgeValid(e) && m.edgeFaceCount(e) != 2)
                        {
                            outline.push_back(e);
                            segs.push_back({base + int(m.edgePoint(e, 0)), base + int(m.edgePoint(e, 1))});
                        }
                    for (const geo::EdgePath& path : geo::chainEdges(m, outline))
                        closed |= path.closed;
                }
                if (g.curves)
                {
                    CurveSet cs = g.curves->evaluated();
                    for (size_t k = 0; k < cs.numCurves(); ++k)
                    {
                        int base = int(pts.size()), count = int(cs.curveSize(k));
                        for (const D3& p : cs.curvePoints(k))
                            pts.push_back(p);
                        bool cyc = cs.cyclic[k] != 0 && count >= 3;
                        closed |= cyc;
                        for (int i = 0; i + 1 < count; ++i)
                            segs.push_back({base + i, base + i + 1});
                        if (cyc)
                            segs.push_back({base + count - 1, base});
                    }
                }
                std::vector<glm::dvec2> flat(pts.size());
                for (size_t i = 0; i < pts.size(); ++i)
                    flat[i] = {glm::dot(pts[i], u), glm::dot(pts[i], v)};
                bool inside = c.prop("fill") == "inside" && closed;
                curve::CdtResult r = curve::constrainedDelaunay(flat, segs, inside);
                geo::Mesh out;
                std::vector<Index> id(pts.size(), geo::kInvalid);
                for (const auto& t : r.triangles)
                {
                    Index q[3];
                    for (int i = 0; i < 3; ++i)
                    {
                        Index& x = id[size_t(t[size_t(i)])];
                        if (x == geo::kInvalid)
                            x = out.addPoint(pts[size_t(t[size_t(i)])]);
                        q[i] = x;
                    }
                    out.addFace({q[0], q[1], q[2]});
                }
                c.out("mesh", meshGeometry(std::move(out)));
                c.out("lostEdges", constantInt(r.lostSegments));
            },
    });
}
} // namespace tri2d

namespace tetra
{
using Tet = std::array<int, 4>;

double orient3(const D3& a, const D3& b, const D3& c, const D3& d) { return glm::dot(a - d, glm::cross(b - d, c - d)); }

/** Bowyer-Watson Delaunay tetrahedralization of points in the unit cube. */
class Delaunay3
{
public:
    struct Cell
    {
        Tet v;
        std::array<int, 4> n{-1, -1, -1, -1}; // n[i] lies across the face opposite v[i]
        D3 centre{0.0};
        double radius2 = 0.0;
        bool dead = false;
    };

    std::vector<D3> pts;
    std::vector<Cell> cells;
    int real = 0;

    explicit Delaunay3(std::vector<D3> points) : pts(std::move(points))
    {
        real = int(pts.size());
        pts.push_back({-100, -100, -100});
        pts.push_back({400, -100, -100});
        pts.push_back({-100, 400, -100});
        pts.push_back({-100, -100, 400});
        add({real, real + 1, real + 2, real + 3});
    }

    void insert(int p)
    {
        const D3& P = pts[size_t(p)];
        int seed = locate(P);
        if (seed < 0)
            return;
        std::vector<int> cavity = {seed};
        mIn.resize(cells.size(), 0);
        mIn[size_t(seed)] = 1;
        for (size_t k = 0; k < cavity.size(); ++k)
            for (int nb : cells[size_t(cavity[k])].n)
                if (nb >= 0 && !mIn[size_t(nb)] && contains(nb, P))
                {
                    mIn[size_t(nb)] = 1;
                    cavity.push_back(nb);
                }
        struct Face
        {
            std::array<int, 3> v;
            int outside;
        };
        std::vector<Face> faces;
        // Grow the cavity until every face on it sees the new point from inside, so it is star-shaped from it.
        for (int guard = 0; guard < 64; ++guard)
        {
            faces.clear();
            int grow = -1;
            for (int ci : cavity)
            {
                const Cell& cl = cells[size_t(ci)];
                for (int i = 0; i < 4; ++i)
                {
                    int nb = cl.n[size_t(i)];
                    if (nb >= 0 && mIn[size_t(nb)])
                        continue;
                    std::array<int, 3> f = faceOf(cl.v, i);
                    double s = orient3(pts[size_t(f[0])], pts[size_t(f[1])], pts[size_t(f[2])], P);
                    if (s <= 1e-18)
                    {
                        if (nb >= 0)
                            grow = nb;
                        continue;
                    }
                    faces.push_back({f, nb});
                }
                if (grow >= 0)
                    break;
            }
            if (grow < 0)
                break;
            mIn[size_t(grow)] = 1;
            cavity.push_back(grow);
        }
        for (int ci : cavity)
        {
            cells[size_t(ci)].dead = true;
            mIn[size_t(ci)] = 0;
        }
        std::unordered_map<uint64_t, std::pair<int, int>> open;
        for (const Face& f : faces)
        {
            int id = add({f.v[0], f.v[1], f.v[2], p});
            cells[size_t(id)].n[3] = f.outside;
            if (f.outside >= 0)
            {
                Cell& o = cells[size_t(f.outside)];
                for (int i = 0; i < 4; ++i)
                {
                    std::array<int, 3> of = faceOf(o.v, i);
                    std::sort(of.begin(), of.end());
                    std::array<int, 3> mine = f.v;
                    std::sort(mine.begin(), mine.end());
                    if (of == mine)
                        o.n[size_t(i)] = id;
                }
            }
            for (int i = 0; i < 3; ++i)
            {
                int a = cells[size_t(id)].v[size_t((i + 1) % 3)], b = cells[size_t(id)].v[size_t((i + 2) % 3)];
                uint64_t key = a < b ? (uint64_t(uint32_t(a)) << 32) | uint32_t(b) : (uint64_t(uint32_t(b)) << 32) | uint32_t(a);
                auto it = open.find(key);
                if (it == open.end())
                    open[key] = {id, i};
                else
                {
                    cells[size_t(id)].n[size_t(i)] = it->second.first;
                    cells[size_t(it->second.first)].n[size_t(it->second.second)] = id;
                    open.erase(it);
                }
            }
        }
        mLast = int(cells.size()) - 1;
    }

private:
    std::vector<uint8_t> mIn;
    int mLast = 0;

    /** The face opposite v[i], wound so the cell's remaining vertex is on its positive side. */
    std::array<int, 3> faceOf(const Tet& v, int i) const
    {
        static const int kFaces[4][3] = {{1, 2, 3}, {0, 3, 2}, {0, 1, 3}, {0, 2, 1}};
        std::array<int, 3> f = {v[size_t(kFaces[i][0])], v[size_t(kFaces[i][1])], v[size_t(kFaces[i][2])]};
        if (orient3(pts[size_t(f[0])], pts[size_t(f[1])], pts[size_t(f[2])], pts[size_t(v[size_t(i)])]) < 0.0)
            std::swap(f[1], f[2]);
        return f;
    }

    bool contains(int c, const D3& p) const
    {
        const Cell& cl = cells[size_t(c)];
        D3 d = p - cl.centre;
        return glm::dot(d, d) < cl.radius2 * (1.0 - 1e-12);
    }

    int add(Tet v)
    {
        if (orient3(pts[size_t(v[0])], pts[size_t(v[1])], pts[size_t(v[2])], pts[size_t(v[3])]) < 0.0)
            std::swap(v[0], v[1]);
        Cell c;
        c.v = v;
        const D3 &a = pts[size_t(v[0])], &b = pts[size_t(v[1])], &cc = pts[size_t(v[2])], &d = pts[size_t(v[3])];
        D3 ba = b - a, ca = cc - a, da = d - a;
        double det = 2.0 * glm::dot(ba, glm::cross(ca, da));
        if (std::abs(det) < 1e-300)
        {
            c.centre = (a + b + cc + d) * 0.25;
            c.radius2 = 1e300;
        }
        else
        {
            D3 num = glm::dot(ba, ba) * glm::cross(ca, da) + glm::dot(ca, ca) * glm::cross(da, ba) + glm::dot(da, da) * glm::cross(ba, ca);
            D3 off = num / det;
            c.centre = a + off;
            c.radius2 = glm::dot(off, off);
        }
        cells.push_back(c);
        return int(cells.size()) - 1;
    }

    int locate(const D3& p)
    {
        int t = mLast;
        if (t < 0 || size_t(t) >= cells.size() || cells[size_t(t)].dead)
            for (t = int(cells.size()) - 1; t >= 0 && cells[size_t(t)].dead; --t)
                ;
        for (size_t guard = 0; t >= 0 && guard < cells.size() + 64; ++guard)
        {
            const Cell& c = cells[size_t(t)];
            int next = -1;
            for (int i = 0; i < 4 && next < 0; ++i)
            {
                int k = (i + int(guard)) % 4;
                std::array<int, 3> f = faceOf(c.v, k);
                // The cell's own vertex is on the positive side, so a negative point lies beyond this face.
                if (orient3(pts[size_t(f[0])], pts[size_t(f[1])], pts[size_t(f[2])], p) < 0.0 && c.n[size_t(k)] >= 0)
                    next = c.n[size_t(k)];
            }
            if (next < 0)
                return contains(t, p) ? t : fallback(p);
            t = next;
        }
        return fallback(p);
    }

    int fallback(const D3& p) const
    {
        for (int k = int(cells.size()) - 1; k >= 0; --k)
            if (!cells[size_t(k)].dead && contains(k, p))
                return k;
        return -1;
    }
};

/** Inside a closed surface by ray parity, the majority of three directions. */
bool insideSurface(const geo::MeshBvh& bvh, const D3& p, double eps)
{
    static const D3 dirs[3] = {glm::normalize(D3(0.5773, 0.5774, 0.5775)), glm::normalize(D3(-0.7071, 0.1, 0.7)), glm::normalize(D3(0.13, -0.99, 0.05))};
    int votes = 0;
    for (const D3& d : dirs)
    {
        int hits = 0;
        D3 o = p;
        for (int guard = 0; guard < 4096; ++guard)
        {
            geo::BvhHit h;
            if (!bvh.raycast(o, d, h))
                break;
            ++hits;
            o = h.point + d * eps;
        }
        votes += hits % 2;
    }
    return votes >= 2;
}

uint64_t morton(const D3& p)
{
    auto spread = [](uint64_t x) {
        x &= 0x1fffff;
        x = (x | x << 32) & 0x1f00000000ffffULL;
        x = (x | x << 16) & 0x1f0000ff0000ffULL;
        x = (x | x << 8) & 0x100f00f00f00f00fULL;
        x = (x | x << 4) & 0x10c30c30c30c30c3ULL;
        x = (x | x << 2) & 0x1249249249249249ULL;
        return x;
    };
    auto q = [](double v) { return uint64_t(std::clamp(v, 0.0, 1.0) * 2097151.0); };
    return spread(q(p.x)) | spread(q(p.y)) << 1 | spread(q(p.z)) << 2;
}

struct TetResult
{
    std::vector<D3> points;
    std::vector<Tet> tets;
};

TetResult tetrahedralize(const geo::Mesh& m, double spacing, const std::string& node)
{
    LILA_ZONE();
    TetResult res;
    geo::Bounds b = m.bounds();
    if (!b.valid() || m.numFaces() == 0)
        return res;
    geo::MeshBvh bvh(m);
    const double diag = std::max(1e-9, glm::length(b.size()));
    std::map<std::array<double, 3>, int> seen;
    auto addPoint = [&](const D3& p) {
        auto [it, fresh] = seen.try_emplace({p.x, p.y, p.z}, int(res.points.size()));
        if (fresh)
            res.points.push_back(p);
    };
    for (Index p = 0; p < m.pointCapacity(); ++p)
        if (m.pointValid(p))
        {
            bool used = false;
            m.forEachPointCorner(p, [&](Index) { used = true; });
            if (used)
                addPoint(m.position(p));
        }
    if (spacing > 0.0)
    {
        glm::ivec3 n = glm::max(glm::ivec3(1), glm::ivec3(glm::floor(b.size() / spacing)));
        double count = double(n.x) * n.y * n.z;
        if (count > 2.0e6)
            throw ExecError(node + ": a spacing of " + std::to_string(spacing) + " cm puts about " + std::to_string(int64_t(count)) + " points in the bounds; raise it");
        D3 start = b.center() - D3(n - 1) * spacing * 0.5;
        for (int z = 0; z < n.z; ++z)
            for (int y = 0; y < n.y; ++y)
                for (int x = 0; x < n.x; ++x)
                {
                    D3 p = start + D3(x, y, z) * spacing;
                    geo::BvhClosest near;
                    if (bvh.closestPoint(p, near, spacing * 0.5))
                        continue;
                    if (insideSurface(bvh, p, diag * 1e-7))
                        addPoint(p);
                }
    }
    if (res.points.size() < 4)
        return res;

    // Triangulated in the unit cube, nudged by a hair so four points on one sphere don't stall it.
    D3 lo = b.min;
    double extent = std::max({b.size().x, b.size().y, b.size().z, 1e-9});
    std::vector<D3> unit(res.points.size());
    for (size_t i = 0; i < unit.size(); ++i)
    {
        uint32_t h = hash32(uint32_t(i), 0x7e7a);
        D3 jitter(hashToUnit(h) - 0.5, hashToUnit(hash32(h, 1)) - 0.5, hashToUnit(hash32(h, 2)) - 0.5);
        unit[i] = (res.points[i] - lo) / extent + jitter * 1e-9;
    }
    std::vector<int> order(unit.size());
    std::iota(order.begin(), order.end(), 0);
    std::vector<uint64_t> code(unit.size());
    for (size_t i = 0; i < unit.size(); ++i)
        code[i] = morton(unit[i]);
    std::sort(order.begin(), order.end(), [&](int a, int c) { return code[size_t(a)] != code[size_t(c)] ? code[size_t(a)] < code[size_t(c)] : a < c; });
    // A padding box well outside the points goes in first, so the hull of the real points forms away from the super tet.
    const int real = int(unit.size());
    for (int k = 0; k < 8; ++k)
        unit.push_back(D3(k & 1 ? 2.5 : -1.5, k & 2 ? 2.5 : -1.5, k & 4 ? 2.5 : -1.5));
    Delaunay3 dt(unit);
    for (int k = real; k < real + 8; ++k)
        dt.insert(k);
    for (int i : order)
        dt.insert(i);

    const double minVolume = 1e-12 * extent * extent * extent;
    for (const auto& c : dt.cells)
    {
        if (c.dead || c.v[0] >= real || c.v[1] >= real || c.v[2] >= real || c.v[3] >= real)
            continue;
        const D3 &p0 = res.points[size_t(c.v[0])], &p1 = res.points[size_t(c.v[1])], &p2 = res.points[size_t(c.v[2])], &p3 = res.points[size_t(c.v[3])];
        if (std::abs(orient3(p0, p1, p2, p3)) / 6.0 < minVolume)
            continue;
        if (!insideSurface(bvh, (p0 + p1 + p2 + p3) * 0.25, diag * 1e-7))
            continue;
        res.tets.push_back(c.v);
    }
    return res;
}

void tetNode()
{
    registerDecl({
        .type = "Tetrahedralize",
        .label = "Tetrahedralize",
        .category = "Mesh",
        .doc = "Fills a closed mesh with tetrahedra through its points, plus a grid of points inside at the spacing. Each tet comes out as its own "
               "four triangles, with the tetIndex attribute on its faces and points and tetPoint on its points (the shared vertex each came from, "
               "so tets can be welded back together). Surface selects the faces on the outside. The tets follow the surface through its points.",
        .sockets = {.inputs = {geoIn("mesh"), bounded(in("spacing", T::Float, 0.0, "Distance between points added inside, cm; 0 uses only the surface points."), 0.0),
                               bounded(in("shrink", T::Float, 0.0, "Scales each tet towards its centre, to see them apart."), 0.0, 1.0)},
                    .outputs = {geoOut("mesh"), fout("tetIndex", T::Int), fout("surface", T::Bool), out("tetCount", T::Int)}},
        .exec =
            [](ExecContext& c) {
                LILA_ZONE();
                GeometryPtr g = c.geometry("mesh");
                double shrink = std::clamp(c.f("shrink"), 0.0, 1.0);
                TetResult r;
                if (g->mesh)
                    r = tetrahedralize(*g->mesh, std::max(0.0, c.f("spacing")), c.node.name);
                std::map<std::array<int, 3>, int> faceUse;
                for (const Tet& t : r.tets)
                    for (int i = 0; i < 4; ++i)
                    {
                        std::array<int, 3> f = {t[size_t((i + 1) % 4)], t[size_t((i + 2) % 4)], t[size_t((i + 3) % 4)]};
                        std::sort(f.begin(), f.end());
                        ++faceUse[f];
                    }
                geo::Mesh out;
                out.reserve(r.tets.size() * 4, r.tets.size() * 6, r.tets.size() * 4, r.tets.size() * 12);
                std::string tetName = "tetIndex", pointName = "tetPoint", surfaceName = c.anonymous("surface");
                std::vector<int32_t> pointTet, pointSource, faceTet;
                std::vector<uint8_t> faceSurface;
                for (size_t k = 0; k < r.tets.size(); ++k)
                {
                    const Tet& t = r.tets[k];
                    D3 centre(0.0);
                    for (int v : t)
                        centre += r.points[size_t(v)] * 0.25;
                    Index ids[4];
                    for (int i = 0; i < 4; ++i)
                    {
                        ids[i] = out.addPoint(glm::mix(r.points[size_t(t[size_t(i)])], centre, shrink));
                        pointTet.push_back(int32_t(k));
                        pointSource.push_back(int32_t(t[size_t(i)]));
                    }
                    for (int i = 0; i < 4; ++i)
                    {
                        int a = (i + 1) % 4, b = (i + 2) % 4, d = (i + 3) % 4;
                        const D3 &pa = r.points[size_t(t[size_t(a)])], &pb = r.points[size_t(t[size_t(b)])], &pd = r.points[size_t(t[size_t(d)])];
                        // Wound to face away from the vertex opposite.
                        if (glm::dot(glm::cross(pb - pa, pd - pa), r.points[size_t(t[size_t(i)])] - pa) > 0.0)
                            std::swap(b, d);
                        if (out.addFace({ids[a], ids[b], ids[d]}) == geo::kInvalid)
                            continue;
                        std::array<int, 3> f = {t[size_t((i + 1) % 4)], t[size_t((i + 2) % 4)], t[size_t((i + 3) % 4)]};
                        std::sort(f.begin(), f.end());
                        faceTet.push_back(int32_t(k));
                        faceSurface.push_back(faceUse[f] == 1 ? 1 : 0);
                    }
                }
                out.pointAttrs().getOrAdd<int32_t>(tetName);
                out.pointAttrs().getOrAdd<int32_t>(pointName);
                out.faceAttrs().getOrAdd<int32_t>(tetName);
                out.faceAttrs().getOrAdd<bool>(surfaceName);
                auto pt = out.pointAttrs().get<int32_t>(tetName);
                auto ps = out.pointAttrs().get<int32_t>(pointName);
                std::copy(pointTet.begin(), pointTet.end(), pt.begin());
                std::copy(pointSource.begin(), pointSource.end(), ps.begin());
                auto ft = out.faceAttrs().get<int32_t>(tetName);
                auto fs = out.faceAttrs().get<bool>(surfaceName);
                for (size_t f = 0; f < faceTet.size() && f < ft.size(); ++f)
                {
                    ft[f] = faceTet[f];
                    fs[f] = faceSurface[f] != 0;
                }
                Geometry result = *g;
                result.mesh = std::make_shared<const geo::Mesh>(std::move(out));
                c.out("mesh", makeGeometry(std::move(result)));
                c.out("tetIndex", attributeField(tetName, T::Int));
                c.out("surface", attributeField(surfaceName, T::Bool));
                c.out("tetCount", constantInt(int64_t(r.tets.size())));
            },
    });
}
} // namespace tetra

void registerRemeshNodes()
{
    isoremesh::remeshNode();
    tri2d::triangulate2dNode();
    tetra::tetNode();
}
} // namespace lila::gn::nodes
