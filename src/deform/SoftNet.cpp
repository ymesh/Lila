#include "deform/SoftNet.h"

#include "core/Profile.h"

#ifdef _MSC_VER
#pragma warning(push, 0)
#endif
#include <Eigen/Sparse>
#ifdef _MSC_VER
#pragma warning(pop)
#endif

#include <tbb/parallel_for.h>

#include <algorithm>
#include <cmath>

namespace lila::deform::softnet
{
namespace
{
using SpMat = Eigen::SparseMatrix<double>;

/** The rotation taking unit a onto unit b by the shortest arc. */
glm::dmat3 shortestArc(const glm::dvec3& a, const glm::dvec3& b)
{
    const glm::dvec3 v = glm::cross(a, b);
    const double c = glm::dot(a, b);
    if (c < -1.0 + 1e-12)
    {
        glm::dvec3 axis = glm::cross(a, std::abs(a.x) < 0.9 ? glm::dvec3(1, 0, 0) : glm::dvec3(0, 1, 0));
        axis = glm::normalize(axis);
        return glm::dmat3(2.0 * glm::outerProduct(axis, axis)) - glm::dmat3(1.0);
    }
    const glm::dmat3 k(0.0, v.z, -v.y, -v.z, 0.0, v.x, v.y, -v.x, 0.0);
    return glm::dmat3(1.0) + k + k * k * (1.0 / (1.0 + c));
}

glm::dvec3 flatten(const glm::dvec3& v, const glm::dvec3& n) { return v - n * glm::dot(v, n); }

/** cos and sin of the turn about n that best carries the rest directions (already on n's plane) onto the posed ones. */
glm::dvec2 bestTurn(const KnotRig& k, const glm::dmat3& carried, const glm::dvec3& origin, const glm::dvec3& n, std::span<const glm::dvec3> points)
{
    double c = 0.0, s = 0.0;
    for (size_t i = 0; i < k.along.size() && i < k.restDirections.size(); ++i)
    {
        const glm::dvec3 v = flatten(at(k.along[i], points) - origin, n);
        const double len = glm::length(v);
        if (len < 1e-12)
            continue;
        const glm::dvec3 r = carried * k.restDirections[i];
        c += glm::dot(r, v) / len;
        s += glm::dot(glm::cross(r, v), n) / len;
    }
    const double m = std::hypot(c, s);
    return m > 1e-12 ? glm::dvec2(c / m, s / m) : glm::dvec2(1.0, 0.0);
}
} // namespace

std::array<double, 4> bernstein(double t)
{
    t = std::clamp(t, 0.0, 1.0);
    const double u = 1.0 - t;
    return {u * u * u, 3.0 * u * u * t, 3.0 * u * t * t, t * t * t};
}

void InfluenceTable::apply(std::span<const glm::dvec3> moves, std::span<glm::dvec3> out) const
{
    LILA_ZONE();
    const size_t n = std::min(points(), out.size());
    if (moves.size() < dofs)
        return;
    tbb::parallel_for(tbb::blocked_range<size_t>(0, n, 2048), [&](const tbb::blocked_range<size_t>& r) {
        for (size_t i = r.begin(); i < r.end(); ++i)
        {
            glm::dvec3 d(0.0);
            for (uint32_t e = offsets[i]; e < offsets[i + 1]; ++e)
                d += double(entries[e].weight) * moves[entries[e].dof];
            out[i] = d;
        }
    });
}

std::vector<glm::dvec3> sampleDeltas(std::span<const Sample> samples, std::span<const Run> runs, size_t knots, std::span<const glm::dvec3> moves)
{
    std::vector<glm::dvec3> out(samples.size(), glm::dvec3(0.0));
    for (size_t s = 0; s < samples.size(); ++s)
    {
        const Sample& smp = samples[s];
        if (smp.run < 0)
        {
            if (smp.knot >= 0 && size_t(smp.knot) < moves.size())
                out[s] = moves[size_t(smp.knot)];
            continue;
        }
        const Run& r = runs[size_t(smp.run)];
        const std::array<double, 4> b = bernstein(smp.t);
        const std::array<long long, 4> dof = {r.knot[0], r.tangent[0] < 0 ? -1 : (long long)knots + r.tangent[0],
                                              r.tangent[1] < 0 ? -1 : (long long)knots + r.tangent[1], r.knot[1]};
        for (int i = 0; i < 4; ++i)
            if (dof[size_t(i)] >= 0 && size_t(dof[size_t(i)]) < moves.size())
                out[s] += b[size_t(i)] * moves[size_t(dof[size_t(i)])];
    }
    return out;
}

InfluenceTable buildInfluence(const geo::Mesh& topology, std::span<const Sample> samples, std::span<const Run> runs, size_t knots, size_t tangents,
                              double edgeLength, double reach, double stiffness, double cutoff)
{
    LILA_ZONE();
    InfluenceTable table;
    const size_t n = topology.pointCapacity();
    const size_t dofs = dofCount(knots, tangents);
    if (n == 0 || samples.empty() || dofs == 0)
        return table;

    // The graph Laplacian: it depends on the topology alone, so the table is the same whatever pose it is made in.
    std::vector<Eigen::Triplet<double>> lt;
    std::vector<double> degree(n, 0.0);
    for (geo::Index e = 0; e < topology.edgeCapacity(); ++e)
    {
        if (!topology.edgeValid(e))
            continue;
        const geo::Index a = topology.edgePoint(e, 0), b = topology.edgePoint(e, 1);
        if (a >= n || b >= n || a == b)
            continue;
        lt.emplace_back(Eigen::Index(a), Eigen::Index(b), -1.0);
        lt.emplace_back(Eigen::Index(b), Eigen::Index(a), -1.0);
        degree[a] += 1.0;
        degree[b] += 1.0;
    }
    for (size_t i = 0; i < n; ++i)
        lt.emplace_back(Eigen::Index(i), Eigen::Index(i), degree[i]);
    const Eigen::Index size = Eigen::Index(n);
    SpMat l(size, size);
    l.setFromTriplets(lt.begin(), lt.end());

    // C: samples x points (barycentric), S: samples x dofs (Bernstein weights of the run's control points).
    std::vector<Eigen::Triplet<double>> ct, st;
    for (size_t s = 0; s < samples.size(); ++s)
    {
        const Sample& smp = samples[s];
        for (int k = 0; k < 3; ++k)
            if (smp.at.points[size_t(k)] < n && smp.at.bary[k] != 0.0)
                ct.emplace_back(Eigen::Index(s), Eigen::Index(smp.at.points[size_t(k)]), smp.at.bary[k]);
        if (smp.run < 0)
        {
            if (smp.knot >= 0 && size_t(smp.knot) < knots)
                st.emplace_back(Eigen::Index(s), Eigen::Index(smp.knot), 1.0);
            continue;
        }
        const Run& r = runs[size_t(smp.run)];
        const std::array<double, 4> b = bernstein(smp.t);
        const std::array<int, 4> dof = {r.knot[0], r.tangent[0] < 0 ? -1 : int(knots) + r.tangent[0], r.tangent[1] < 0 ? -1 : int(knots) + r.tangent[1],
                                        r.knot[1]};
        for (int i = 0; i < 4; ++i)
            if (dof[size_t(i)] >= 0 && size_t(dof[size_t(i)]) < dofs && b[size_t(i)] != 0.0)
                st.emplace_back(Eigen::Index(s), Eigen::Index(dof[size_t(i)]), b[size_t(i)]);
    }
    SpMat c(Eigen::Index(samples.size()), size), sd(Eigen::Index(samples.size()), Eigen::Index(dofs));
    c.setFromTriplets(ct.begin(), ct.end());
    sd.setFromTriplets(st.begin(), st.end());

    // In edge units: the screening makes a displacement fade over about reach, and the samples pull far harder than
    // bending resists, so the surface meets them closely.
    const double h = std::max(edgeLength, 1e-9);
    const double r = std::max(reach, h) / h;
    const double weight = std::max(stiffness, 1e-6) * 100.0;
    SpMat a = SpMat(l.transpose()) * l;
    SpMat id(size, size);
    id.setIdentity();
    a += (1.0 / (r * r * r * r)) * id;
    const SpMat ctr = c.transpose();
    a += weight * SpMat(ctr * c);
    Eigen::SimplicialLDLT<SpMat> solver;
    {
        LILA_ZONE_N("softNet factor");
        solver.compute(a);
    }
    if (solver.info() != Eigen::Success)
        return table;
    const SpMat rhs = weight * SpMat(ctr * sd);

    // One back-substitution per degree of freedom; tiny far-field values are dropped.
    std::vector<std::vector<std::pair<uint32_t, float>>> columns(dofs);
    {
        LILA_ZONE_N("softNet influence");
        tbb::parallel_for(size_t(0), dofs, [&](size_t j) {
            Eigen::VectorXd b = Eigen::VectorXd(rhs.col(Eigen::Index(j)));
            if (b.isZero(0.0))
                return;
            const Eigen::VectorXd x = solver.solve(b);
            const double peak = x.cwiseAbs().maxCoeff();
            if (!(peak > 0.0))
                return;
            const double floor = peak * cutoff;
            for (Eigen::Index i = 0; i < x.size(); ++i)
                if (std::abs(x[i]) > floor)
                    columns[j].emplace_back(uint32_t(i), float(x[i]));
        });
    }
    table.dofs = dofs;
    table.offsets.assign(n + 1, 0);
    for (const auto& col : columns)
        for (const auto& [p, w] : col)
            ++table.offsets[p + 1];
    for (size_t i = 0; i < n; ++i)
        table.offsets[i + 1] += table.offsets[i];
    table.entries.resize(table.offsets[n]);
    std::vector<uint32_t> fill(table.offsets.begin(), table.offsets.end() - 1);
    for (size_t j = 0; j < dofs; ++j)
        for (const auto& [p, w] : columns[j])
            table.entries[fill[p]++] = {uint32_t(j), w};
    return table;
}

glm::dvec3 normalAt(const SurfacePoint& s, std::span<const glm::dvec3> points)
{
    const glm::dvec3 a = points[s.points[0]], b = points[s.points[1]], c = points[s.points[2]];
    const glm::dvec3 n = glm::cross(b - a, c - a);
    const double len = glm::length(n);
    return len > 1e-300 ? n / len : glm::dvec3(0, 1, 0);
}

glm::dmat3 restFrame(const KnotRig& k, std::span<const glm::dvec3> points)
{
    const glm::dvec3 z = normalAt(k.at, points);
    const glm::dvec3 o = at(k.at, points);
    glm::dvec3 x(0.0);
    for (const SurfacePoint& p : k.along)
        if (const glm::dvec3 v = flatten(at(p, points) - o, z); glm::length(v) > 1e-12)
        {
            x = glm::normalize(v);
            break;
        }
    if (glm::length(x) < 0.5)
        x = glm::normalize(flatten(std::abs(z.x) < 0.9 ? glm::dvec3(1, 0, 0) : glm::dvec3(0, 0, 1), z));
    return glm::dmat3(x, glm::cross(z, x), z);
}

std::vector<glm::dmat4> knotFrames(std::span<const KnotRig> knots, std::span<const glm::dvec3> points)
{
    LILA_ZONE();
    struct Posed
    {
        glm::dvec3 origin, normal;
        glm::dmat3 carried;
        glm::dvec2 turn{1.0, 0.0};
    };
    std::vector<Posed> posed(knots.size());
    bool anyJunction = false;
    for (size_t i = 0; i < knots.size(); ++i)
    {
        const KnotRig& k = knots[i];
        Posed& p = posed[i];
        p.origin = at(k.at, points);
        p.normal = normalAt(k.at, points);
        p.carried = shortestArc(k.rest[2], p.normal) * k.rest;
        anyJunction = anyJunction || k.degree >= 3;
        if (k.degree >= 3 || k.blend.empty())
            p.turn = bestTurn(k, p.carried, p.origin, p.normal, points);
    }
    std::vector<glm::dmat4> out(knots.size(), glm::dmat4(1.0));
    for (size_t i = 0; i < knots.size(); ++i)
    {
        const KnotRig& k = knots[i];
        glm::dvec2 turn = posed[i].turn;
        if (k.degree < 3 && anyJunction && !k.blend.empty())
        {
            glm::dvec2 sum(0.0);
            for (const auto& [j, w] : k.blend)
                if (j >= 0 && size_t(j) < posed.size())
                    sum += w * posed[size_t(j)].turn;
            const double m = glm::length(sum);
            turn = m > 1e-12 ? sum / m : glm::dvec2(1.0, 0.0);
        }
        const glm::dvec3 n = posed[i].normal;
        const glm::dvec3 x0 = posed[i].carried[0];
        const glm::dvec3 x = glm::normalize(x0 * turn.x + glm::cross(n, x0) * turn.y);
        glm::dmat4& f = out[i];
        f[0] = glm::dvec4(x, 0.0);
        f[1] = glm::dvec4(glm::cross(n, x), 0.0);
        f[2] = glm::dvec4(n, 0.0);
        f[3] = glm::dvec4(posed[i].origin, 1.0);
    }
    return out;
}
} // namespace lila::deform::softnet
