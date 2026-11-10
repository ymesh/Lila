#include "sculpt/SubdLevels.h"

#include "core/Profile.h"
#include "geo/Symmetry.h"

#include <tbb/parallel_for.h>
#include <tbb/parallel_reduce.h>

#include <algorithm>
#include <array>
#include <unordered_map>

namespace lila::sculpt
{
namespace
{
using geo::kInvalid;
using geo::PointStencil;

constexpr Index kGrain = 4096;

template <class T> std::vector<T> applyStencil(const PointStencil& s, std::span<const T> in)
{
    std::vector<T> out(s.rows());
    tbb::parallel_for(tbb::blocked_range<Index>(0, s.rows(), kGrain), [&](const tbb::blocked_range<Index>& r) {
        for (Index i = r.begin(); i < r.end(); ++i)
        {
            T sum(0);
            for (Index k = s.offsets[i]; k < s.offsets[i + 1]; ++k)
                sum += in[s.cols[k]] * T(s.weights[k]);
            out[i] = sum;
        }
    });
    return out;
}

PointStencil transpose(const PointStencil& s)
{
    PointStencil t;
    t.inputs = s.rows();
    t.offsets.assign(size_t(s.inputs) + 1, 0);
    for (Index c : s.cols)
        ++t.offsets[c + 1];
    for (size_t i = 1; i < t.offsets.size(); ++i)
        t.offsets[i] += t.offsets[i - 1];
    t.cols.resize(s.cols.size());
    t.weights.resize(s.weights.size());
    std::vector<Index> fill(t.offsets.begin(), t.offsets.end() - 1);
    for (Index r = 0; r < s.rows(); ++r)
        for (Index k = s.offsets[r]; k < s.offsets[r + 1]; ++k)
        {
            const Index at = fill[s.cols[k]]++;
            t.cols[at] = r;
            t.weights[at] = s.weights[k];
        }
    return t;
}

std::shared_ptr<const SubdLevel> levelAbove(const geo::Mesh& below, const geo::SubdivideOptions& opts, PointStencil& stencil)
{
    LILA_ZONE();

    geo::SubdivideOptions once = opts;
    once.levels = 1;
    // Levels are built one at a time, so fitting island interiors would cost a solve per level.
    once.uvs = geo::SubdivideUvs::Blend;
    auto lvl = std::make_shared<SubdLevel>();
    stencil = std::move(geo::subdivideStencils(below, once).front());
    geo::SubdivideMap map;
    lvl->mesh = geo::subdivide(below, once, &map);
    geo::clearSymmetryKeys(lvl->mesh, 0);
    lvl->parentPoint = std::move(map.points);
    lvl->down = transpose(stencil);
    const geo::Mesh& m = lvl->mesh;
    lvl->framePeer.assign(m.pointCapacity(), kInvalid);
    for (Index c = 0; c < m.cornerCapacity(); ++c)
        if (m.cornerValid(c) && lvl->framePeer[m.cornerPoint(c)] == kInvalid)
            lvl->framePeer[m.cornerPoint(c)] = m.cornerNextPoint(c);
    return lvl;
}

struct Frame
{
    glm::dvec3 t, b, n;
};

/** Each point's detail frame on level `level` at positions: the surface normal and a tangent toward its frame peer. */
std::vector<Frame> frames(const SubdLevel& lvl, std::span<const glm::dvec3> pos)
{
    LILA_ZONE();

    const geo::Mesh& m = lvl.mesh;
    std::vector<glm::dvec3> faceN(m.faceCapacity(), glm::dvec3(0.0));
    tbb::parallel_for(tbb::blocked_range<Index>(0, m.faceCapacity(), kGrain), [&](const tbb::blocked_range<Index>& r) {
        for (Index f = r.begin(); f < r.end(); ++f)
        {
            if (!m.faceValid(f))
                continue;
            glm::dvec3 n(0.0);
            const Index first = m.faceCorner(f);
            Index c = first;
            do
            {
                const glm::dvec3& a = pos[m.cornerPoint(c)];
                const glm::dvec3& b = pos[m.cornerNextPoint(c)];
                n += glm::cross(a, b);
                c = m.cornerNext(c);
            } while (c != first);
            faceN[f] = n;
        }
    });
    std::vector<Frame> out(m.pointCapacity());
    tbb::parallel_for(tbb::blocked_range<Index>(0, m.pointCapacity(), kGrain), [&](const tbb::blocked_range<Index>& r) {
        for (Index p = r.begin(); p < r.end(); ++p)
        {
            if (!m.pointValid(p))
                continue;
            glm::dvec3 n(0.0);
            m.forEachPointCorner(p, [&](Index c) { n += faceN[m.cornerFace(c)]; });
            n = glm::length(n) > 0.0 ? glm::normalize(n) : glm::dvec3(0.0, 1.0, 0.0);
            glm::dvec3 t = lvl.framePeer[p] != kInvalid ? pos[lvl.framePeer[p]] - pos[p] : glm::dvec3(0.0);
            t -= n * glm::dot(t, n);
            if (glm::length(t) <= 1e-300)
            {
                t = std::abs(n.x) < 0.9 ? glm::dvec3(1.0, 0.0, 0.0) : glm::dvec3(0.0, 1.0, 0.0);
                t -= n * glm::dot(t, n);
            }
            t = glm::normalize(t);
            out[p] = {t, glm::cross(n, t), n};
        }
    });
    return out;
}

/** min |S x - y|^2 + lambda |x - x0|^2 by Jacobi-preconditioned conjugate gradients on the normal equations. */
std::vector<glm::dvec3> leastSquaresDown(const PointStencil& S, const PointStencil& ST, std::span<const glm::dvec3> y, std::vector<glm::dvec3> x)
{
    LILA_ZONE();

    const Index n = ST.rows();
    std::vector<double> diag(n, 0.0);
    double meanDiag = 0.0;
    for (Index j = 0; j < n; ++j)
    {
        for (Index k = ST.offsets[j]; k < ST.offsets[j + 1]; ++k)
            diag[j] += ST.weights[k] * ST.weights[k];
        meanDiag += diag[j];
    }
    meanDiag = n ? meanDiag / double(n) : 1.0;
    const double lambda = 1e-6 * std::max(meanDiag, 1e-12);
    const std::vector<glm::dvec3> x0 = x;

    auto normalOp = [&](std::span<const glm::dvec3> v) {
        std::vector<glm::dvec3> sv = applyStencil<glm::dvec3>(S, v);
        std::vector<glm::dvec3> out = applyStencil<glm::dvec3>(ST, sv);
        tbb::parallel_for(Index(0), n, [&](Index j) { out[j] += lambda * v[j]; });
        return out;
    };
    auto dot3 = [&](const std::vector<glm::dvec3>& a, const std::vector<glm::dvec3>& b) {
        return tbb::parallel_reduce(
            tbb::blocked_range<Index>(0, n, kGrain), glm::dvec3(0.0),
            [&](const tbb::blocked_range<Index>& r, glm::dvec3 acc) {
                for (Index j = r.begin(); j < r.end(); ++j)
                    acc += a[j] * b[j];
                return acc;
            },
            [](glm::dvec3 a, glm::dvec3 b) { return a + b; });
    };

    std::vector<glm::dvec3> rhs = applyStencil<glm::dvec3>(ST, y);
    for (Index j = 0; j < n; ++j)
        rhs[j] += lambda * x0[j];
    std::vector<glm::dvec3> r = normalOp(x);
    for (Index j = 0; j < n; ++j)
        r[j] = rhs[j] - r[j];
    std::vector<glm::dvec3> z(n), p(n);
    for (Index j = 0; j < n; ++j)
        z[j] = r[j] / (diag[j] + lambda);
    p = z;
    glm::dvec3 rz = dot3(r, z);
    const glm::dvec3 rhsNorm = dot3(rhs, rhs);
    const double tol2 = 1e-22;
    for (int it = 0; it < 400; ++it)
    {
        const glm::dvec3 rr = dot3(r, r);
        if (rr.x <= tol2 * rhsNorm.x && rr.y <= tol2 * rhsNorm.y && rr.z <= tol2 * rhsNorm.z)
            break;
        std::vector<glm::dvec3> ap = normalOp(p);
        const glm::dvec3 pap = dot3(p, ap);
        glm::dvec3 alpha(0.0);
        for (int c = 0; c < 3; ++c)
            alpha[c] = pap[c] > 0.0 ? rz[c] / pap[c] : 0.0;
        tbb::parallel_for(Index(0), n, [&](Index j) {
            x[j] += alpha * p[j];
            r[j] -= alpha * ap[j];
            z[j] = r[j] / (diag[j] + lambda);
        });
        const glm::dvec3 rzNext = dot3(r, z);
        glm::dvec3 beta(0.0);
        for (int c = 0; c < 3; ++c)
            beta[c] = rz[c] > 0.0 ? rzNext[c] / rz[c] : 0.0;
        rz = rzNext;
        tbb::parallel_for(Index(0), n, [&](Index j) { p[j] = z[j] + beta * p[j]; });
    }
    return x;
}

template <class T> std::vector<T> layerUp(const PointStencil& s, const std::vector<T>& below, const std::vector<T>* diff, T fallback, Index belowCount)
{
    if (below.empty() && (!diff || diff->empty()))
        return {};
    std::vector<T> in = below.empty() ? std::vector<T>(belowCount, fallback) : below;
    std::vector<T> out = applyStencil<T>(s, in);
    if (diff && !diff->empty())
        for (size_t i = 0; i < out.size(); ++i)
            out[i] += (*diff)[i];
    for (T& v : out)
        v = glm::clamp(v, T(0.0f), T(1.0f));
    return out;
}

template <class T> void layerDown(const PointStencil& s, const SubdLevel& lvl, std::vector<T>& layer, std::vector<T>& diff)
{
    if (layer.empty())
        return;
    std::vector<T> coarse(lvl.parentPoint.size(), T(0.0f));
    for (size_t j = 0; j < coarse.size(); ++j)
        if (lvl.parentPoint[j] != kInvalid)
            coarse[j] = layer[lvl.parentPoint[j]];
    std::vector<T> back = applyStencil<T>(s, coarse);
    diff.resize(layer.size());
    bool any = false;
    for (size_t i = 0; i < layer.size(); ++i)
    {
        diff[i] = layer[i] - back[i];
        any = any || diff[i] != T(0.0f);
    }
    if (!any)
        diff.clear();
    layer = std::move(coarse);
}
} // namespace

std::shared_ptr<const SubdStack> buildSubdStack(const geo::Mesh& cage, const geo::SubdivideOptions& opts)
{
    LILA_ZONE();

    auto s = std::make_shared<SubdStack>();
    s->opts = opts;
    auto base = std::make_shared<SubdLevel>();
    base->mesh = cage;
    s->levels.push_back(base);
    for (int l = 0; l < opts.levels; ++l)
        s->levels.push_back(levelAbove(s->levels.back()->mesh, opts, s->stencils.emplace_back()));
    return s;
}

std::shared_ptr<const SubdStack> addSubdLevel(const SubdStack& s)
{
    auto out = std::make_shared<SubdStack>(s);
    out->levels.push_back(levelAbove(s.levels.back()->mesh, s.opts, out->stencils.emplace_back()));
    out->opts.levels = out->top();
    return out;
}

std::shared_ptr<const SubdStack> truncateSubdStack(const SubdStack& s, int top)
{
    auto out = std::make_shared<SubdStack>();
    out->opts = s.opts;
    out->opts.levels = top;
    out->levels.assign(s.levels.begin(), s.levels.begin() + top + 1);
    out->stencils.assign(s.stencils.begin(), s.stencils.begin() + top);
    return out;
}

void stepUp(const SubdStack& s, int to, const DetailPtr& detail, std::vector<glm::dvec3>& positions, LevelLayers& layers)
{
    LILA_ZONE();

    const PointStencil& S = s.stencils[size_t(to - 1)];
    const SubdLevel& lvl = *s.levels[size_t(to)];
    const Index below = Index(positions.size());
    std::vector<glm::dvec3> q = S.apply(positions);
    if (detail && !detail->offset.empty())
    {
        const std::vector<Frame> f = frames(lvl, q);
        tbb::parallel_for(Index(0), Index(q.size()), [&](Index i) {
            const glm::dvec3& o = detail->offset[i];
            q[i] += f[i].t * o.x + f[i].b * o.y + f[i].n * o.z;
        });
    }
    positions = std::move(q);
    const LevelLayers* d = detail ? &detail->layers : nullptr;
    layers.color = layerUp<glm::vec4>(S, layers.color, d ? &d->color : nullptr, glm::vec4(1.0f), below);
    layers.mask = layerUp<float>(S, layers.mask, d ? &d->mask : nullptr, 0.0f, below);
    layers.freeze = layerUp<float>(S, layers.freeze, d ? &d->freeze : nullptr, 0.0f, below);
}

DetailPtr stepDown(const SubdStack& s, int from, std::vector<glm::dvec3>& positions, LevelLayers& layers)
{
    LILA_ZONE();

    const PointStencil& S = s.stencils[size_t(from - 1)];
    const SubdLevel& lvl = *s.levels[size_t(from)];
    std::vector<glm::dvec3> start(lvl.parentPoint.size(), glm::dvec3(0.0));
    for (size_t j = 0; j < start.size(); ++j)
        if (lvl.parentPoint[j] != kInvalid)
            start[j] = positions[lvl.parentPoint[j]];
    std::vector<glm::dvec3> coarse = leastSquaresDown(S, lvl.down, positions, std::move(start));
    const std::vector<glm::dvec3> q = S.apply(coarse);
    const std::vector<Frame> f = frames(lvl, q);

    auto d = std::make_shared<LevelDetail>();
    d->offset.resize(positions.size());
    tbb::parallel_for(Index(0), Index(positions.size()), [&](Index i) {
        const glm::dvec3 v = positions[i] - q[i];
        d->offset[i] = {glm::dot(v, f[i].t), glm::dot(v, f[i].b), glm::dot(v, f[i].n)};
    });
    positions = std::move(coarse);
    layerDown<glm::vec4>(S, lvl, layers.color, d->layers.color);
    layerDown<float>(S, lvl, layers.mask, d->layers.mask);
    layerDown<float>(S, lvl, layers.freeze, d->layers.freeze);
    return d;
}
std::shared_ptr<const SubdStack> dropSubdLevelsBelow(const SubdStack& s, int bottom)
{
    auto out = std::make_shared<SubdStack>();
    out->opts = s.opts;
    out->opts.levels = s.top() - bottom;
    auto base = std::make_shared<SubdLevel>();
    base->mesh = s.levels[size_t(bottom)]->mesh;
    out->levels.push_back(base);
    out->levels.insert(out->levels.end(), s.levels.begin() + bottom + 1, s.levels.end());
    out->stencils.assign(s.stencils.begin() + bottom, s.stencils.end());
    return out;
}

std::vector<Index> coarsePoints(const SubdLevel& level)
{
    std::vector<Index> out(level.mesh.pointCapacity(), kInvalid);
    for (Index c = 0; c < Index(level.parentPoint.size()); ++c)
        if (level.parentPoint[c] != kInvalid)
            out[level.parentPoint[c]] = c;
    return out;
}

namespace
{
using PointKey = std::vector<Index>;

struct PointKeyHash
{
    size_t operator()(const PointKey& k) const
    {
        size_t h = k.size();
        for (Index i : k)
            h ^= std::hash<Index>{}(i) + 0x9e3779b97f4a7c15ull + (h << 6) + (h >> 2);
        return h;
    }
};

constexpr Index kSeveral = kInvalid - 1;

/**
 * Each point's place in one subdivision of a cage, in cage ids: a cage point is itself, an edge point the two cage points
 * of its edge, a face point the cage points of its face. Every face of a subdivision has exactly one cage point corner,
 * so the cage points of the faces around a point name it. Empty where that doesn't hold.
 */
std::vector<PointKey> subdivisionKeys(const geo::Mesh& m, std::span<const Index> coarse)
{
    std::vector<Index> faceCage(m.faceCapacity(), kInvalid);
    for (Index f = 0; f < m.faceCapacity(); ++f)
        if (m.faceValid(f))
            m.forEachFaceCorner(f, [&](Index c) {
                if (const Index k = coarse[m.cornerPoint(c)]; k != kInvalid)
                    faceCage[f] = faceCage[f] == kInvalid ? k : kSeveral;
            });
    std::vector<PointKey> keys(m.pointCapacity());
    tbb::parallel_for(Index(0), m.pointCapacity(), [&](Index p) {
        if (!m.pointValid(p))
            return;
        if (coarse[p] != kInvalid)
        {
            keys[p] = {coarse[p]};
            return;
        }
        PointKey k;
        for (Index f : m.pointFaces(p))
        {
            if (faceCage[f] == kInvalid || faceCage[f] == kSeveral)
                return;
            k.push_back(faceCage[f]);
        }
        std::ranges::sort(k);
        k.erase(std::unique(k.begin(), k.end()), k.end());
        if (k.size() >= 2)
            keys[p] = std::move(k);
    });
    return keys;
}
} // namespace

std::vector<Index> matchSubdivisions(const geo::Mesh& a, std::span<const Index> coarseA, const geo::Mesh& b, std::span<const Index> coarseB)
{
    LILA_ZONE();

    if (a.numPoints() != b.numPoints() || coarseA.size() < size_t(a.pointCapacity()) || coarseB.size() < size_t(b.pointCapacity()))
        return {};
    const std::vector<PointKey> ka = subdivisionKeys(a, coarseA);
    const std::vector<PointKey> kb = subdivisionKeys(b, coarseB);
    std::unordered_map<PointKey, Index, PointKeyHash> at;
    at.reserve(size_t(b.numPoints()));
    for (Index p = 0; p < b.pointCapacity(); ++p)
        if (b.pointValid(p) && (kb[p].empty() || !at.emplace(kb[p], p).second))
            return {};
    std::vector<Index> out(a.pointCapacity(), kInvalid);
    for (Index p = 0; p < a.pointCapacity(); ++p)
        if (a.pointValid(p))
        {
            auto it = at.find(ka[p]);
            if (it == at.end())
                return {};
            out[p] = it->second;
        }
    return out;
}

std::optional<geo::Mesh> reconstructCage(const geo::Mesh& fine, std::vector<Index>& coarseOf)
{
    LILA_ZONE();

    // Around every face of a subdivision: its cage point, an edge point, its face point, an edge point.
    enum : int8_t
    {
        kUnset = -1,
        kCage = 0,
        kEdge = 1,
        kFace = 2,
    };
    constexpr int8_t kPattern[4] = {kCage, kEdge, kFace, kEdge};

    if (fine.numFaces() == 0)
        return std::nullopt;
    for (Index f = 0; f < fine.faceCapacity(); ++f)
        if (fine.faceValid(f) && fine.faceSize(f) != 4)
            return std::nullopt;

    std::vector<int8_t> label(fine.pointCapacity(), kUnset);
    std::vector<int8_t> faceCageAt(fine.faceCapacity(), -1);
    auto corners = [&](Index f) {
        std::array<Index, 4> c;
        Index x = fine.faceCorner(f);
        for (Index& out : c)
        {
            out = x;
            x = fine.cornerNext(x);
        }
        return c;
    };
    // Where face f's cage point corner must be, from the points already labelled; -1 when nothing says.
    auto cageCornerOf = [&](Index f) {
        const std::array<Index, 4> c = corners(f);
        for (int i = 0; i < 4; ++i)
            if (label[fine.cornerPoint(c[size_t(i)])] == kCage)
                return i;
        for (int i = 0; i < 4; ++i)
            if (label[fine.cornerPoint(c[size_t(i)])] == kFace)
                return (i + 2) % 4;
        return -1;
    };

    // Labels the component of seed with its cage corner at `at`. Fails on a conflict, a face point on the border, or an
    // edge point not between exactly two cage points. Fills the faces and points it touched, for rolling back.
    auto attempt = [&](Index seed, int at, std::vector<Index>& faces, std::vector<Index>& touched, int& score) {
        faces.clear();
        touched.clear();
        score = 0;
        faceCageAt[seed] = int8_t(at);
        faces.push_back(seed);
        for (size_t q = 0; q < faces.size(); ++q)
        {
            const Index f = faces[q];
            const std::array<Index, 4> c = corners(f);
            for (int i = 0; i < 4; ++i)
            {
                const Index p = fine.cornerPoint(c[size_t(i)]);
                const int8_t want = kPattern[(i - faceCageAt[f] + 4) % 4];
                if (label[p] == kUnset)
                {
                    label[p] = want;
                    touched.push_back(p);
                }
                else if (label[p] != want)
                    return false;
            }
            for (Index corner : c)
            {
                bool ok = true;
                fine.forEachEdgeCorner(fine.cornerEdge(corner), [&](Index r) {
                    const Index g = fine.cornerFace(r);
                    if (!ok || faceCageAt[g] >= 0)
                        return;
                    const int k = cageCornerOf(g);
                    if (k < 0)
                        ok = false;
                    else
                    {
                        faceCageAt[g] = int8_t(k);
                        faces.push_back(g);
                    }
                });
                if (!ok)
                    return false;
            }
        }
        for (Index p : touched)
        {
            if (label[p] == kFace)
            {
                if (fine.isBoundaryPoint(p))
                    return false;
                score += fine.pointValence(p) == 4;
            }
            else if (label[p] == kEdge)
            {
                int cages = 0;
                fine.forEachPointEdge(p, [&](Index e) { cages += label[fine.edgeOtherPoint(e, p)] == kCage; });
                if (cages != 2)
                    return false;
            }
        }
        return true;
    };
    auto rollBack = [&](const std::vector<Index>& faces, const std::vector<Index>& touched) {
        for (Index f : faces)
            faceCageAt[f] = -1;
        for (Index p : touched)
            label[p] = kUnset;
    };

    std::vector<Index> faces, touched;
    for (Index seed = 0; seed < fine.faceCapacity(); ++seed)
    {
        if (!fine.faceValid(seed) || faceCageAt[seed] >= 0)
            continue;
        // Swapping the cage and face points also labels a quad grid consistently; prefer the cage made of quads.
        int best = -1, bestScore = -1;
        for (int at = 0; at < 4; ++at)
        {
            int score = 0;
            const bool ok = attempt(seed, at, faces, touched, score);
            rollBack(faces, touched);
            if (ok && score > bestScore)
            {
                best = at;
                bestScore = score;
            }
        }
        int score = 0;
        if (best < 0 || !attempt(seed, best, faces, touched, score))
            return std::nullopt;
    }
    for (Index p = 0; p < fine.pointCapacity(); ++p)
        if (fine.pointValid(p) && label[p] == kUnset)
            return std::nullopt;

    geo::Mesh cage = fine;
    const Index oldFaces = fine.faceCapacity();
    for (Index p = 0; p < fine.pointCapacity(); ++p)
    {
        if (!fine.pointValid(p) || label[p] != kFace)
            continue;
        Index first = kInvalid;
        fine.forEachPointCorner(p, [&](Index c) {
            if (first == kInvalid)
                first = c;
        });
        std::vector<Index> points, src;
        Index c = first;
        do
        {
            const Index v = fine.cornerNext(fine.cornerNext(c));
            points.push_back(fine.cornerPoint(v));
            src.push_back(v);
            // The next face around p across the edge coming into p, so the cage points follow the faces' winding.
            const Index r = fine.cornerRadial(fine.cornerPrev(c));
            if (r == fine.cornerPrev(c) || fine.cornerPoint(r) != p || points.size() > 64)
                return std::nullopt;
            c = r;
        } while (c != first);
        if (cage.addFace(points, fine.cornerFace(first), src) == kInvalid)
            return std::nullopt;
    }
    for (Index f = 0; f < oldFaces; ++f)
        if (cage.faceValid(f))
            cage.removeFace(f);
    cage.removeAllLoose();
    geo::MeshRemap remap;
    cage.compact(&remap);
    geo::clearSymmetryKeys(cage, 0);
    coarseOf.assign(fine.pointCapacity(), kInvalid);
    for (Index p = 0; p < fine.pointCapacity(); ++p)
        if (fine.pointValid(p) && label[p] == kCage)
            coarseOf[p] = remap.points[p];
    return cage;
}
} // namespace lila::sculpt
