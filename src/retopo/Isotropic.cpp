#include "retopo/Isotropic.h"

#include "core/Profile.h"

#include <tbb/parallel_for.h>

#include <algorithm>
#include <cmath>
#include <unordered_map>

namespace lila::retopo::detail
{
namespace
{
/** How a vertex may move: freely over the surface, along its feature line only, or not at all. */
enum class Kind : uint8_t
{
    Free,
    Line,
    Fixed,
};

struct Iso
{
    std::vector<glm::dvec3> pos;
    std::vector<std::array<Index, 3>> tri;
    std::vector<uint8_t> triDead;
    std::vector<uint8_t> vertDead;
    std::vector<double> len;
    std::vector<float> dens;
    std::vector<uint8_t> locked;
    /** Per vertex, the other ends of its feature edges. */
    std::vector<std::vector<Index>> fnb;
    std::vector<std::vector<Index>> vtris;

    Index addVertex(const glm::dvec3& p, double l, float d)
    {
        pos.push_back(p);
        len.push_back(l);
        dens.push_back(d);
        locked.push_back(0);
        vertDead.push_back(0);
        vtris.emplace_back();
        fnb.emplace_back();
        return Index(pos.size() - 1);
    }
    Index addTri(const std::array<Index, 3>& t)
    {
        tri.push_back(t);
        triDead.push_back(0);
        const Index id = Index(tri.size() - 1);
        for (Index v : t)
            vtris[v].push_back(id);
        return id;
    }
    void unlink(Index v, Index t)
    {
        auto& list = vtris[v];
        list.erase(std::remove(list.begin(), list.end(), t), list.end());
    }
    void killTri(Index t)
    {
        triDead[t] = 1;
        for (Index v : tri[t])
            unlink(v, t);
    }
    bool isFeature(Index a, Index b) const { return std::find(fnb[a].begin(), fnb[a].end(), b) != fnb[a].end(); }
    void addFeature(Index a, Index b)
    {
        if (a != b && !isFeature(a, b))
        {
            fnb[a].push_back(b);
            fnb[b].push_back(a);
        }
    }
    bool removeFeature(Index a, Index b)
    {
        auto it = std::find(fnb[a].begin(), fnb[a].end(), b);
        if (it == fnb[a].end())
            return false;
        fnb[a].erase(it);
        fnb[b].erase(std::find(fnb[b].begin(), fnb[b].end(), a));
        return true;
    }
    Kind kind(Index v) const
    {
        if (locked[v])
            return Kind::Fixed;
        if (fnb[v].empty())
            return Kind::Free;
        return fnb[v].size() == 2 ? Kind::Line : Kind::Fixed;
    }
    /** The live triangles holding edge (a, b). */
    std::vector<Index> edgeTris(Index a, Index b) const
    {
        std::vector<Index> out;
        for (Index t : vtris[a])
            if (tri[t][0] == b || tri[t][1] == b || tri[t][2] == b)
                out.push_back(t);
        return out;
    }
    bool connected(Index a, Index b) const
    {
        for (Index t : vtris[a])
            if (tri[t][0] == b || tri[t][1] == b || tri[t][2] == b)
                return true;
        return false;
    }
    void neighbours(Index v, std::vector<Index>& out) const
    {
        out.clear();
        for (Index t : vtris[v])
            for (Index w : tri[t])
                if (w != v)
                    out.push_back(w);
        std::sort(out.begin(), out.end());
        out.erase(std::unique(out.begin(), out.end()), out.end());
    }
    glm::dvec3 normal(Index t) const { return glm::cross(pos[tri[t][1]] - pos[tri[t][0]], pos[tri[t][2]] - pos[tri[t][0]]); }
    double target(Index a, Index b) const { return 0.5 * (len[a] + len[b]); }
};

/** The corner of t at v, rotated so t reads (v, next, prev). */
std::array<Index, 3> rotatedTo(const std::array<Index, 3>& t, Index v)
{
    if (t[1] == v)
        return {t[1], t[2], t[0]};
    if (t[2] == v)
        return {t[2], t[0], t[1]};
    return t;
}

void splitLong(Iso& m)
{
    for (int pass = 0; pass < 8; ++pass)
    {
        bool any = false;
        std::vector<uint8_t> touched(m.tri.size(), 0);
        const Index count = Index(m.tri.size());
        for (Index t = 0; t < count; ++t)
        {
            if (m.triDead[t] || (t < Index(touched.size()) && touched[t]))
                continue;
            for (int k = 0; k < 3; ++k)
            {
                const Index a = m.tri[t][k], b = m.tri[t][(k + 1) % 3];
                const double l = m.target(a, b);
                if (glm::distance(m.pos[a], m.pos[b]) <= 4.0 / 3.0 * l)
                    continue;
                const std::vector<Index> around = m.edgeTris(a, b);
                bool free = around.size() <= 2;
                for (Index u : around)
                    free = free && !(u < Index(touched.size()) && touched[u]);
                if (!free)
                    continue;
                const Index mid = m.addVertex(0.5 * (m.pos[a] + m.pos[b]), 0.5 * (m.len[a] + m.len[b]), 0.5f * (m.dens[a] + m.dens[b]));
                if (m.removeFeature(a, b))
                {
                    m.addFeature(a, mid);
                    m.addFeature(mid, b);
                }
                for (Index u : around)
                {
                    // u reads (p, q, r) with the split edge p-q.
                    std::array<Index, 3> f = m.tri[u];
                    int e = 0;
                    while (!((f[e] == a && f[(e + 1) % 3] == b) || (f[e] == b && f[(e + 1) % 3] == a)))
                        ++e;
                    const Index p = f[e], q = f[(e + 1) % 3], r = f[(e + 2) % 3];
                    m.killTri(u);
                    const Index n1 = m.addTri({p, mid, r}), n2 = m.addTri({mid, q, r});
                    touched.resize(m.tri.size(), 0);
                    touched[n1] = touched[n2] = 1;
                }
                any = true;
                break;
            }
        }
        if (!any)
            break;
    }
}

/** Whether moving a to `to` keeps every triangle round a (other than those with b) facing the same way and not too long. */
bool fanStaysValid(const Iso& m, Index a, Index b, const glm::dvec3& to, double maxLen)
{
    for (Index t : m.vtris[a])
    {
        const auto& f = m.tri[t];
        if (f[0] == b || f[1] == b || f[2] == b)
            continue;
        const std::array<Index, 3> r = rotatedTo(f, a);
        const glm::dvec3 before = m.normal(t);
        const glm::dvec3 after = glm::cross(m.pos[r[1]] - to, m.pos[r[2]] - to);
        const double lb = glm::length(before), la = glm::length(after);
        if (la <= 1e-12 * std::max(1.0, lb) || glm::dot(before, after) <= 0.3 * lb * la)
            return false;
        if (glm::distance(to, m.pos[r[1]]) > maxLen || glm::distance(to, m.pos[r[2]]) > maxLen)
            return false;
    }
    return true;
}

bool tryCollapse(Iso& m, Index a, Index b, std::vector<uint8_t>& touched)
{
    const Kind ka = m.kind(a), kb = m.kind(b);
    if (ka == Kind::Fixed)
        return false;
    if (ka == Kind::Line && !m.isFeature(a, b))
        return false;
    const std::vector<Index> around = m.edgeTris(a, b);
    if (around.empty() || around.size() > 2)
        return false;
    // Link condition: a and b share exactly the vertices across the edge's own triangles.
    std::vector<Index> na, nb, opp;
    m.neighbours(a, na);
    m.neighbours(b, nb);
    for (Index t : around)
        for (Index w : m.tri[t])
            if (w != a && w != b)
                opp.push_back(w);
    std::sort(opp.begin(), opp.end());
    std::vector<Index> shared;
    std::set_intersection(na.begin(), na.end(), nb.begin(), nb.end(), std::back_inserter(shared));
    if (shared != opp)
        return false;
    // A free vertex sliding into a line vertex would leave the feature where it is anyway.
    const glm::dvec3 to = (ka == Kind::Free && kb == Kind::Free) ? 0.5 * (m.pos[a] + m.pos[b]) : m.pos[b];
    const double maxLen = 4.0 / 3.0 * m.target(a, b);
    if (!fanStaysValid(m, a, b, to, maxLen))
        return false;
    if (to != m.pos[b] && !fanStaysValid(m, b, a, to, maxLen))
        return false;
    for (Index t : m.vtris[a])
        if (touched.size() > t && touched[t])
            return false;
    for (Index t : m.vtris[b])
        if (touched.size() > t && touched[t])
            return false;

    const std::vector<Index> moved = m.fnb[a];
    for (Index other : moved)
    {
        m.removeFeature(a, other);
        if (other != b)
            m.addFeature(b, other);
    }
    for (Index t : around)
        m.killTri(t);
    const std::vector<Index> fan = m.vtris[a];
    for (Index t : fan)
    {
        for (Index& w : m.tri[t])
            if (w == a)
                w = b;
        m.vtris[b].push_back(t);
    }
    m.vtris[a].clear();
    m.vertDead[a] = 1;
    m.pos[b] = to;
    m.len[b] = 0.5 * (m.len[a] + m.len[b]);
    for (Index t : m.vtris[b])
        if (t < touched.size())
            touched[t] = 1;
    return true;
}

void collapseShort(Iso& m)
{
    for (int pass = 0; pass < 4; ++pass)
    {
        bool any = false;
        std::vector<uint8_t> touched(m.tri.size(), 0);
            for (Index t = 0; t < Index(m.tri.size()); ++t)
        {
            if (m.triDead[t] || touched[t])
                continue;
            for (int k = 0; k < 3 && !m.triDead[t]; ++k)
            {
                const Index a = m.tri[t][k], b = m.tri[t][(k + 1) % 3];
                if (glm::distance(m.pos[a], m.pos[b]) >= 4.0 / 5.0 * m.target(a, b))
                    continue;
                if (tryCollapse(m, a, b, touched) || tryCollapse(m, b, a, touched))
                {
                    any = true;
                    break;
                }
            }
        }
        if (!any)
            break;
    }
}

void flipEdges(Iso& m)
{
    std::vector<int> valence(m.pos.size(), 0);
    std::vector<uint8_t> border(m.pos.size(), 0);
    std::vector<Index> nb;
    for (Index v = 0; v < Index(m.pos.size()); ++v)
    {
        if (m.vertDead[v])
            continue;
        m.neighbours(v, nb);
        valence[v] = int(nb.size());
        for (Index w : nb)
            if (m.edgeTris(v, w).size() == 1)
                border[v] = 1;
    }
    auto deviation = [&](Index v, int val) {
        const int want = border[v] ? 4 : 6;
        return (val - want) * (val - want);
    };
    for (int pass = 0; pass < 3; ++pass)
    {
        bool any = false;
        for (Index t = 0; t < Index(m.tri.size()); ++t)
        {
            if (m.triDead[t])
                continue;
            for (int k = 0; k < 3; ++k)
            {
                const Index a = m.tri[t][k], b = m.tri[t][(k + 1) % 3], c = m.tri[t][(k + 2) % 3];
                if (m.isFeature(a, b))
                    continue;
                const std::vector<Index> around = m.edgeTris(a, b);
                if (around.size() != 2)
                    continue;
                const Index u = around[0] == t ? around[1] : around[0];
                Index d = kInvalid;
                for (Index w : m.tri[u])
                    if (w != a && w != b)
                        d = w;
                if (d == kInvalid || d == c || valence[a] <= 3 || valence[b] <= 3 || m.connected(c, d))
                    continue;
                const int before = deviation(a, valence[a]) + deviation(b, valence[b]) + deviation(c, valence[c]) + deviation(d, valence[d]);
                const int after = deviation(a, valence[a] - 1) + deviation(b, valence[b] - 1) + deviation(c, valence[c] + 1) +
                                  deviation(d, valence[d] + 1);
                if (after >= before)
                    continue;
                const glm::dvec3 n0 = m.normal(t), n1 = m.normal(u);
                const glm::dvec3 avg = glm::normalize(glm::normalize(n0) + glm::normalize(n1));
                const glm::dvec3 p1 = glm::cross(m.pos[d] - m.pos[a], m.pos[c] - m.pos[a]);
                const glm::dvec3 p2 = glm::cross(m.pos[b] - m.pos[d], m.pos[c] - m.pos[d]);
                const double l1 = glm::length(p1), l2 = glm::length(p2);
                if (l1 <= 1e-12 || l2 <= 1e-12 || glm::dot(p1, avg) < 0.5 * l1 || glm::dot(p2, avg) < 0.5 * l2)
                    continue;
                m.killTri(t);
                m.killTri(u);
                m.addTri({a, d, c});
                m.addTri({d, b, c});
                --valence[a];
                --valence[b];
                ++valence[c];
                ++valence[d];
                any = true;
                break;
            }
        }
        if (!any)
            break;
    }
}

void relaxAndProject(Iso& m, const geo::MeshBvh& bvh, const FeatureLines& creases, const FeatureLines& border)
{
    const Index n = Index(m.pos.size());
    std::vector<glm::dvec3> vn(n, glm::dvec3(0.0));
    for (Index t = 0; t < Index(m.tri.size()); ++t)
        if (!m.triDead[t])
        {
            const glm::dvec3 f = m.normal(t);
            for (Index v : m.tri[t])
                vn[v] += f;
        }
    const std::vector<std::vector<Index>>& featNb = m.fnb;
    std::vector<glm::dvec3> next = m.pos;
    tbb::parallel_for(Index(0), n, [&](Index v) {
        if (m.vertDead[v] || m.vtris[v].empty())
            return;
        const Kind k = m.kind(v);
        if (k == Kind::Fixed)
            return;
        if (k == Kind::Line)
        {
            const glm::dvec3 mid = 0.5 * (m.pos[featNb[v][0]] + m.pos[featNb[v][1]]);
            const FeatureLines& lines = m.edgeTris(v, featNb[v][0]).size() == 1 ? border : creases;
            glm::dvec3 on;
            const glm::dvec3 want = m.pos[v] + 0.5 * (mid - m.pos[v]);
            next[v] = lines.closest(want, 2.0 * m.len[v], on) ? on : want;
            return;
        }
        glm::dvec3 c(0.0);
        double w = 0.0;
        for (Index t : m.vtris[v])
        {
            const double area = glm::length(m.normal(t));
            c += area * (m.pos[m.tri[t][0]] + m.pos[m.tri[t][1]] + m.pos[m.tri[t][2]]) / 3.0;
            w += area;
        }
        if (w <= 0.0)
            return;
        c /= w;
        const double nl = glm::length(vn[v]);
        glm::dvec3 d = c - m.pos[v];
        if (nl > 0.0)
            d -= vn[v] / nl * glm::dot(vn[v] / nl, d);
        geo::BvhClosest hit;
        next[v] = bvh.closestPoint(m.pos[v] + d, hit) ? hit.point : m.pos[v] + d;
    });
    // A move that turns a triangle over is undone.
    for (int pass = 0; pass < 6; ++pass)
    {
        bool undone = false;
        for (Index t = 0; t < Index(m.tri.size()); ++t)
        {
            if (m.triDead[t])
                continue;
            const auto& f = m.tri[t];
            const glm::dvec3 before = m.normal(t), after = glm::cross(next[f[1]] - next[f[0]], next[f[2]] - next[f[0]]);
            if (glm::dot(before, after) > 0.2 * glm::length(before) * glm::length(after))
                continue;
            for (Index v : f)
                if (next[v] != m.pos[v])
                {
                    next[v] = m.pos[v];
                    undone = true;
                }
        }
        if (!undone)
            break;
    }
    m.pos.swap(next);
}
} // namespace

Surface remeshIsotropic(const Surface& s, const std::vector<double>& target, const geo::MeshBvh& bvh, const FeatureLines& creases,
                        const FeatureLines& border,
                        std::vector<double>* outTarget, int iterations)
{
    LILA_ZONE();

    Iso m;
    const Index n = s.vertexCount();
    for (Index v = 0; v < n; ++v)
    {
        m.addVertex(s.pos[v], target[v], v < Index(s.density.size()) ? s.density[v] : 1.0f);
        m.locked[v] = (s.feature.size() > size_t(v) && s.feature[v] == FeatureKind::Corner) ||
                      (s.pinned.size() > size_t(v) && s.pinned[v]);
    }
    for (const auto& t : s.tris)
        m.addTri(t);
    for (size_t e = 0; e < s.edges.size(); ++e)
        if (s.edgeFeature[e] || s.edgeBoundary[e])
            m.addFeature(s.edges[e][0], s.edges[e][1]);

    for (int it = 0; it < iterations; ++it)
    {
        Budget::checkpoint();
        splitLong(m);
        collapseShort(m);
        flipEdges(m);
        relaxAndProject(m, bvh, creases, border);
    }

    Surface out;
    std::vector<Index> remap(m.pos.size(), kInvalid);
    for (Index t = 0; t < Index(m.tri.size()); ++t)
    {
        if (m.triDead[t])
            continue;
        std::array<Index, 3> f;
        for (int k = 0; k < 3; ++k)
        {
            const Index v = m.tri[t][k];
            if (remap[v] == kInvalid)
            {
                remap[v] = Index(out.pos.size());
                out.pos.push_back(m.pos[v]);
                out.density.push_back(m.dens[v]);
                if (outTarget)
                    outTarget->push_back(m.len[v]);
            }
            f[k] = remap[v];
        }
        out.tris.push_back(f);
    }
    for (Index v = 0; v < Index(m.fnb.size()); ++v)
        for (Index u : m.fnb[v])
            if (v < u && remap[v] != kInvalid && remap[u] != kInvalid)
                out.hard.insert(edgeKey(remap[v], remap[u]));
    out.pins = s.pins;
    return out;
}
} // namespace lila::retopo::detail
